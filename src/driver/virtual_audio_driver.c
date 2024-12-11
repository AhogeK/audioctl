//
// Created by AhogeK on 12/10/24.
//

#include "driver/virtual_audio_driver.h"
#include <stdatomic.h>
#include <pthread.h>
#include "mach/mach_time.h"

//==================================================================================================
#pragma mark -
#pragma mark NullAudio State
//==================================================================================================
// VirtualAudio 示例的目的是提供一个基础实现，展示驱动程序需要做的最小功能集。
// 该示例驱动程序具有以下特性：
// - 一个插件
// - 自定义属性，选择器为 kPlugIn_CustomPropertyID = 'PCst'
// - 一个音频盒
// - 一个设备
// - 支持 44100 和 48000 采样率
// - 通过硬编码提供 1.0 的采样率标量
// - 一个输入流
// - 支持 2 通道 32 位浮点 LPCM 采样
// - 始终输出零
// - 一个输出流
// - 支持 2 通道 32 位浮点 LPCM 采样
// - 写入的数据被忽略
// - 控制项
//   - 主输入音量
//   - 主输出音量
//   - 主输入静音
//   - 主输出静音
//   - 主输入数据源
//   - 主输出数据源
//   - 主直通数据目标
//   - 所有这些仅用于演示，实际并不操作数据

// 声明此驱动程序实现的所有对象的内部对象 ID。
// 注意：此驱动程序具有固定的对象集，永远不会增长或缩小。
enum {
    kObjectID_PlugIn = kAudioObjectPlugInObject,
    kObjectID_Box = 2,
    kObjectID_Device = 3,
    kObjectID_Stream_Input = 4,
    kObjectID_Volume_Input_Master = 5,
    kObjectID_Mute_Input_Master = 6,
    kObjectID_DataSource_Input_Master = 7,
    kObjectID_Stream_Output = 8,
    kObjectID_Volume_Output_Master = 9,
    kObjectID_Mute_Output_Master = 10,
    kObjectID_DataSource_Output_Master = 11,
    kObjectID_DataDestination_PlayThru_Master = 12
};

// 定义插件状态相关的全局变量
static pthread_mutex_t gPlugIn_StateMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32 gPlugIn_RefCount = 0;
static AudioServerPlugInHostRef gPlugIn_Host = NULL;
static const AudioObjectPropertySelector kPlugIn_CustomPropertyID = 0x50437374; // 'PCst'

// 定义盒子相关的全局变量
static CFStringRef gBox_Name = NULL;
static Boolean gBox_Acquired = true;

// 定义设备相关的全局变量
static pthread_mutex_t gDevice_IOMutex = PTHREAD_MUTEX_INITIALIZER;
static Float64 gDevice_SampleRate = 44100.0;
static UInt64 gDevice_IOIsRunning = 0;
static const UInt32 kDevice_RingBufferSize = 16384;
static Float64 gDevice_HostTicksPerFrame = 0.0;
static UInt64 gDevice_NumberTimeStamps = 0;
static Float64 gDevice_AnchorSampleTime = 0.0;
static UInt64 gDevice_AnchorHostTime = 0;

// 定义流相关的全局变量
static bool gStream_Input_IsActive = true;
static bool gStream_Output_IsActive = true;

// 定义音量相关的全局变量
static const Float32 kVolume_MinDB = -96.0f;
static const Float32 kVolume_MaxDB = 6.0f;
static Float32 gVolume_Input_Master_Value = 0.0f;
static Float32 gVolume_Output_Master_Value = 0.0f;

// 定义静音相关的全局变量
static bool gMute_Input_Master_Value = false;
static bool gMute_Output_Master_Value = false;

// 定义数据源相关的全局变量
static const UInt32 kDataSource_NumberItems = 4;
static UInt32 gDataSource_Input_Master_Value = 0;
static UInt32 gDataSource_Output_Master_Value = 0;
static UInt32 gDataDestination_PlayThru_Master_Value = 0;

#pragma mark The Interface

// 驱动接口结构体定义
AudioServerPlugInDriverInterface gAudioServerPlugInDriverInterface = {
        // COM 接口函数 - 必需
        .QueryInterface                     = VirtualAudioDriver_QueryInterface,
        .AddRef                             = VirtualAudioDriver_AddRef,
        .Release                            = VirtualAudioDriver_Release,

        // 基本操作 - 必需
        .Initialize                         = VirtualAudioDriver_Initialize,
        .CreateDevice                       = VirtualAudioDriver_CreateDevice,
        .DestroyDevice                      = VirtualAudioDriver_DestroyDevice,
        .AddDeviceClient                    = VirtualAudioDriver_AddDeviceClient,
        .RemoveDeviceClient                 = VirtualAudioDriver_RemoveDeviceClient,
        .PerformDeviceConfigurationChange   = VirtualAudioDriver_PerformDeviceConfigurationChange,
        .AbortDeviceConfigurationChange     = VirtualAudioDriver_AbortDeviceConfigurationChange,

        // 属性操作 - 必需
        .HasProperty                        = VirtualAudioDriver_HasProperty,
        .IsPropertySettable                 = VirtualAudioDriver_IsPropertySettable,
        .GetPropertyDataSize                = VirtualAudioDriver_GetPropertyDataSize,
        .GetPropertyData                    = VirtualAudioDriver_GetPropertyData,
        .SetPropertyData                    = VirtualAudioDriver_SetPropertyData,

        // IO 操作 - 必需
        .StartIO                            = VirtualAudioDriver_StartIO,
        .StopIO                             = VirtualAudioDriver_StopIO,
        .GetZeroTimeStamp                   = VirtualAudioDriver_GetZeroTimeStamp,
        .WillDoIOOperation                  = VirtualAudioDriver_WillDoIOOperation,
        .BeginIOOperation                   = VirtualAudioDriver_BeginIOOperation,
        .DoIOOperation                      = VirtualAudioDriver_DoIOOperation,
        .EndIOOperation                     = VirtualAudioDriver_EndIOOperation
};

AudioServerPlugInDriverInterface *gAudioServerPlugInDriverInterfacePtr = &gAudioServerPlugInDriverInterface;
AudioServerPlugInDriverRef gAudioServerPlugInDriverRef = &gAudioServerPlugInDriverInterfacePtr;

#pragma mark Factory

void *VirtualAudioDriver_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) {
    // 这是 CFPlugIn 工厂函数。它的作用是为给定的类型创建实现，前提是该类型是受支持的。
    // 因为这个驱动程序很简单，并且所有初始化都是在加载 bundle 时通过静态初始化来处理的，
    // 所以只需要返回指向驱动程序接口的 AudioServerPlugInDriverRef 即可。
    //
    // 一个更复杂的驱动程序需要创建基础对象来满足用于发现实际与驱动程序通信接口的 IUnknown 方法。
    // 驱动程序的大部分初始化应该在驱动程序的 AudioServerPlugInDriverInterface 的 Initialize() 方法中处理。

#pragma unused(inAllocator)
    void *theAnswer = NULL;
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) {
        theAnswer = gAudioServerPlugInDriverRef;
    }
    return theAnswer;
}

#pragma mark Inheritence

static HRESULT VirtualAudioDriver_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface) {
    // 这个函数被 HAL 调用，以获取与插件通信的接口。
    // AudioServerPlugIns 必须支持 IUnknown 接口和 AudioServerPlugInDriverInterface。
    // 由于所有接口都必须提供 IUnknown 接口，因此无论请求的是哪个接口，我们都可以返回 gAudioServerPlugInDriverInterfacePtr 中的单个接口。

    // 局部变量声明
    HRESULT theAnswer = 0;
    CFUUIDRef theRequestedUUID = NULL;

    // 验证参数
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError;
        goto Done;
    }
    if (outInterface == NULL) {
        theAnswer = kAudioHardwareIllegalOperationError;
        goto Done;
    }

    // 从 inUUID 创建 CFUUIDRef
    theRequestedUUID = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (theRequestedUUID == NULL) {
        theAnswer = kAudioHardwareIllegalOperationError;
        goto Done;
    }

    // AudioServerPlugIns 仅支持两个接口，IUnknown (必须由所有 CFPlugIns 支持) 和 AudioServerPlugInDriverInterface (HAL 实际使用的接口)。
    if (CFEqual(theRequestedUUID, IUnknownUUID) || CFEqual(theRequestedUUID, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gPlugIn_StateMutex);
        ++gPlugIn_RefCount;
        pthread_mutex_unlock(&gPlugIn_StateMutex);
        *outInterface = gAudioServerPlugInDriverRef;
    } else {
        theAnswer = E_NOINTERFACE;
    }

    // 确保释放创建的 UUID
    CFRelease(theRequestedUUID);

    Done:
    return theAnswer;
}

static ULONG VirtualAudioDriver_AddRef(void *inDriver) {
    // 该函数在递增后返回引用计数结果。

    // 声明局部变量
    ULONG theAnswer = 0;

    // 检查参数
    if (inDriver != gAudioServerPlugInDriverRef) {
        goto Done; // 如果驱动引用不匹配，直接结束
    }

    // 增加引用计数
    pthread_mutex_lock(&gPlugIn_StateMutex);
    if (gPlugIn_RefCount < UINT32_MAX) {
        ++gPlugIn_RefCount;
    }
    theAnswer = gPlugIn_RefCount;
    pthread_mutex_unlock(&gPlugIn_StateMutex);

    Done:
    return theAnswer;
}

static ULONG VirtualAudioDriver_Release(void *inDriver) {
    // 该函数在递减后返回引用计数结果。

    // 声明局部变量
    ULONG theAnswer = 0;

    // 检查参数
    if (inDriver != gAudioServerPlugInDriverRef) {
        goto Done; // 如果驱动引用不匹配，直接结束
    }

    // 减少引用计数
    pthread_mutex_lock(&gPlugIn_StateMutex);
    if (gPlugIn_RefCount > 0) {
        --gPlugIn_RefCount;
        // 注意即使引用计数归零，也不需要做特别处理，因为 HAL 永远不会完全释放一个打开的插件。
        // 我们仍然管理引用计数以确保 API 语义正确。
    }
    theAnswer = gPlugIn_RefCount;
    pthread_mutex_unlock(&gPlugIn_StateMutex);

    Done:
    return theAnswer;
}

#pragma mark Basic Operations

static OSStatus VirtualAudioDriver_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    // 该方法的作用是初始化驱动。一个特定的任务是存储 AudioServerPlugInHostRef 以便后续使用。
    // 注意，当此调用返回时，HAL 将扫描驱动维护的各个列表（如设备列表）以获取驱动正在发布的初始对象集。
    // 因此，无需通知 HAL 关于在执行此方法过程中创建的任何对象。

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError;
        goto Done;
    }

    // 存储 AudioServerPlugInHostRef
    gPlugIn_Host = inHost;

    // 从设置中初始化 box acquired 属性
    CFPropertyListRef theSettingsData = NULL;
    gPlugIn_Host->CopyFromStorage(gPlugIn_Host, CFSTR("box acquired"), &theSettingsData);
    if (theSettingsData != NULL) {
        if (CFGetTypeID(theSettingsData) == CFBooleanGetTypeID()) {
            gBox_Acquired = CFBooleanGetValue((CFBooleanRef) theSettingsData);
        } else if (CFGetTypeID(theSettingsData) == CFNumberGetTypeID()) {
            SInt32 theValue = 0;
            CFNumberGetValue((CFNumberRef) theSettingsData, kCFNumberSInt32Type, &theValue);
            gBox_Acquired = theValue ? 1 : 0;
        }
        CFRelease(theSettingsData);
    }

    // 从设置中初始化 box name
    gPlugIn_Host->CopyFromStorage(gPlugIn_Host, CFSTR("box name"), &theSettingsData);
    if (theSettingsData != NULL) {
        if (CFGetTypeID(theSettingsData) == CFStringGetTypeID()) {
            gBox_Name = (CFStringRef) theSettingsData;
            CFRetain(gBox_Name);
        }
        CFRelease(theSettingsData);
    }

    // 如果没有设置 box name，则直接设置一个默认值
    if (gBox_Name == NULL) {
        gBox_Name = CFSTR("Virtual Audio Box");
    }

    // 计算每帧的主机时钟周期
    struct mach_timebase_info theTimeBaseInfo;
    mach_timebase_info(&theTimeBaseInfo);
    Float64 theHostClockFrequency = (Float64) theTimeBaseInfo.denom / (Float64) theTimeBaseInfo.numer;
    theHostClockFrequency *= 1000000000.0;
    gDevice_HostTicksPerFrame = theHostClockFrequency / gDevice_SampleRate;

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription,
                                                const AudioServerPlugInClientInfo *inClientInfo,
                                                AudioObjectID *outDeviceObjectID) {
    // 此方法用于告诉实现传输管理器语义的驱动程序从一组 AudioEndpoints 创建 AudioEndpointDevice。
    // 由于此驱动程序不是传输管理器，我们只需检查参数并返回 kAudioHardwareUnsupportedOperationError。

#pragma unused(inDescription)
#pragma unused(inClientInfo)
#pragma unused(outDeviceObjectID)

    // 声明局部变量
    OSStatus theAnswer = kAudioHardwareUnsupportedOperationError;

    // 检查参数
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError;
        goto Done;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID) {
    // 此方法用于告诉实现传输管理器语义的驱动程序销毁 AudioEndpointDevice。
    // 由于此驱动程序不是传输管理器，我们只需检查参数并返回 kAudioHardwareUnsupportedOperationError。

#pragma unused(inDeviceObjectID)

    // 声明局部变量
    OSStatus theAnswer = kAudioHardwareUnsupportedOperationError;

    // 检查参数
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError;
        goto Done;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                   const AudioServerPlugInClientInfo *inClientInfo) {
    // 此方法用于通知驱动程序有新的客户端正在使用给定的设备
    // 这允许设备根据客户端的不同而采取不同的行为
    // 此驱动程序不需要跟踪使用设备的客户端，因此我们只需检查参数并成功返回

#pragma unused(inClientInfo)

    // 声明返回状态变量
    OSStatus theAnswer = 0;

    // 验证参数
    FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "VirtualAudioDriver_AddDeviceClient: 无效的驱动程序引用");
    FailIf(inDeviceObjectID != kObjectID_Device, Done, "VirtualAudioDriver_AddDeviceClient: 无效的设备对象ID");

    Done:
    return theAnswer;
}

static OSStatus
VirtualAudioDriver_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                      const AudioServerPlugInClientInfo *inClientInfo) {
    // 此方法用于通知驱动程序某个客户端不再使用指定的设备。
    // 由于此驱动程序不需要跟踪客户端，因此只需检查参数并成功返回。

#pragma unused(inClientInfo)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_RemoveDeviceClient: 无效的驱动程序引用");
    FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_RemoveDeviceClient: 无效的设备对象ID");

    Done:
    return theAnswer;
}

static OSStatus
VirtualAudioDriver_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                    UInt64 inChangeAction, void *inChangeInfo) {
    // 此方法在设备可以执行通过 RequestDeviceConfigurationChange() 请求的配置更改时被调用。
    // 参数 inChangeAction 和 inChangeInfo 与传递给 RequestDeviceConfigurationChange() 的参数相同。
    //
    // HAL 保证在此方法执行期间 IO 将被停止。HAL 还会处理非控制相关属性的具体变化。
    // 这意味着只需要为 HAL 不知道的自定义属性或控制发送通知。
    //
    // 对于此驱动程序实现的设备，只有采样率更改需要通过此过程，
    // 因为它是设备中唯一可以更改的非控制状态。对于此更改，新的采样率通过 inChangeAction 参数传入。

#pragma unused(inChangeInfo)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_PerformDeviceConfigurationChange: 无效的驱动程序引用");
    FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_PerformDeviceConfigurationChange: 无效的设备对象ID");
    FailWithAction((inChangeAction != 44100) && (inChangeAction != 48000), theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_PerformDeviceConfigurationChange: 无效的采样率");

    // 锁定状态互斥锁
    pthread_mutex_lock(&gPlugIn_StateMutex);

    // 更改采样率（使用显式类型转换）
    gDevice_SampleRate = (Float64) (inChangeAction);

    // 重新计算依赖于采样率的状态
    struct mach_timebase_info theTimeBaseInfo;
    mach_timebase_info(&theTimeBaseInfo);
    Float64 theHostClockFrequency = (Float64) theTimeBaseInfo.denom / (Float64) theTimeBaseInfo.numer;
    theHostClockFrequency *= 1000000000.0;
    gDevice_HostTicksPerFrame = theHostClockFrequency / gDevice_SampleRate;

    // 解锁状态互斥锁
    pthread_mutex_unlock(&gPlugIn_StateMutex);

    Done:
    return theAnswer;
}

static OSStatus
VirtualAudioDriver_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                  UInt64 inChangeAction, void *inChangeInfo) {
    // 此方法用于通知驱动程序配置更改请求被拒绝。
    // 这为驱动程序提供了清理与请求相关状态的机会。
    // 对于此驱动程序，取消的配置更改不需要任何操作。
    // 因此，我们只需检查参数并返回。

#pragma unused(inChangeAction, inChangeInfo)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_AbortDeviceConfigurationChange: 无效的驱动程序引用");
    FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_AbortDeviceConfigurationChange: 无效的设备对象ID");

    Done:
    return theAnswer;
}

#pragma mark Property Operations

static Boolean
VirtualAudioDriver_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID,
                               const AudioObjectPropertyAddress *inAddress) {
    // 此方法返回给定对象是否具有指定属性

    // 声明局部变量
    Boolean theAnswer = false;

    // 检查参数
    FailIf(inDriver != gAudioServerPlugInDriverRef, Done,
           "VirtualAudioDriver_HasProperty: 无效的驱动程序引用");
    FailIf(inAddress == NULL, Done,
           "VirtualAudioDriver_HasProperty: 地址为空");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inObjectID) {
        case kObjectID_PlugIn:
            theAnswer = VirtualAudioDriver_HasPlugInProperty(inDriver, inObjectID, inClientProcessID, inAddress);
            break;

        case kObjectID_Box:
            theAnswer = VirtualAudioDriver_HasBoxProperty(inDriver, inObjectID, inClientProcessID, inAddress);
            break;

        case kObjectID_Device:
            theAnswer = VirtualAudioDriver_HasDeviceProperty(inDriver, inObjectID, inClientProcessID, inAddress);
            break;

        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            theAnswer = VirtualAudioDriver_HasStreamProperty(inDriver, inObjectID, inClientProcessID, inAddress);
            break;

        case kObjectID_Volume_Input_Master:
        case kObjectID_Volume_Output_Master:
        case kObjectID_Mute_Input_Master:
        case kObjectID_Mute_Output_Master:
        case kObjectID_DataSource_Input_Master:
        case kObjectID_DataSource_Output_Master:
        case kObjectID_DataDestination_PlayThru_Master:
            theAnswer = VirtualAudioDriver_HasControlProperty(inDriver, inObjectID, inClientProcessID, inAddress);
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                      pid_t inClientProcessID,
                                                      const AudioObjectPropertyAddress *inAddress,
                                                      Boolean *outIsSettable) {
    // 此方法返回对象上的给定属性是否可以更改其值

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_IsPropertySettable: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_IsPropertySettable: 地址为空");
    FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_IsPropertySettable: 没有地方存放返回值");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inObjectID) {
        case kObjectID_PlugIn:
            theAnswer = VirtualAudioDriver_IsPlugInPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress,
                                                                    outIsSettable);
            break;

        case kObjectID_Box:
            theAnswer = VirtualAudioDriver_IsBoxPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress,
                                                                 outIsSettable);
            break;

        case kObjectID_Device:
            theAnswer = VirtualAudioDriver_IsDevicePropertySettable(inDriver, inObjectID, inClientProcessID, inAddress,
                                                                    outIsSettable);
            break;

        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            theAnswer = VirtualAudioDriver_IsStreamPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress,
                                                                    outIsSettable);
            break;

        case kObjectID_Volume_Input_Master:
        case kObjectID_Volume_Output_Master:
        case kObjectID_Mute_Input_Master:
        case kObjectID_Mute_Output_Master:
        case kObjectID_DataSource_Input_Master:
        case kObjectID_DataSource_Output_Master:
        case kObjectID_DataDestination_PlayThru_Master:
            theAnswer = VirtualAudioDriver_IsControlPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress,
                                                                     outIsSettable);
            break;

        default:
            theAnswer = kAudioHardwareBadObjectError;
            break;
    }

    Done:
    return theAnswer;
}

#pragma mark PlugIn Property Operations

static Boolean VirtualAudioDriver_HasPlugInProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                    pid_t inClientProcessID,
                                                    const AudioObjectPropertyAddress *inAddress) {
    // 此方法返回插件对象是否具有指定属性

#pragma unused(inClientProcessID)

    // 声明局部变量
    Boolean theAnswer = false;

    // 检查参数
    FailIf(inDriver != gAudioServerPlugInDriverRef, Done,
           "VirtualAudioDriver_HasPlugInProperty: 无效的驱动程序引用");
    FailIf(inAddress == NULL, Done,
           "VirtualAudioDriver_HasPlugInProperty: 地址为空");
    FailIf(inObjectID != kObjectID_PlugIn, Done,
           "VirtualAudioDriver_HasPlugInProperty: 不是插件对象");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetPlugInPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:        // 基类
        case kAudioObjectPropertyClass:            // 类
        case kAudioObjectPropertyOwner:            // 所有者
        case kAudioObjectPropertyManufacturer:     // 制造商
        case kAudioObjectPropertyOwnedObjects:     // 拥有的对象
        case kAudioPlugInPropertyBoxList:          // 音频盒列表
        case kAudioPlugInPropertyTranslateUIDToBox:    // UID转音频盒
        case kAudioPlugInPropertyDeviceList:       // 设备列表
        case kAudioPlugInPropertyTranslateUIDToDevice: // UID转设备
        case kAudioPlugInPropertyResourceBundle:    // 资源包
        case kAudioObjectPropertyCustomPropertyInfoList:    // 自定义属性信息列表
        case kPlugIn_CustomPropertyID:             // 自定义属性ID
            theAnswer = true;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus
VirtualAudioDriver_IsPlugInPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                            pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                            Boolean *outIsSettable) {
    // 此方法返回插件对象上的给定属性是否可以更改其值

#pragma unused(inClientProcessID)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_IsPlugInPropertySettable: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_IsPlugInPropertySettable: 地址为空");
    FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_IsPlugInPropertySettable: 没有地方存放返回值");
    FailWithAction(inObjectID != kObjectID_PlugIn, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_IsPlugInPropertySettable: 不是插件对象");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetPlugInPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inAddress->mSelector) {
        // 只读属性
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyTranslateUIDToBox:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
        case kAudioObjectPropertyCustomPropertyInfoList:
            *outIsSettable = false;
            break;

            // 可写属性
        case kPlugIn_CustomPropertyID:
            *outIsSettable = true;
            break;

            // 未知属性
        default:
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    }

    Done:
    return theAnswer;
}

#pragma mark Box Property Operations

static Boolean VirtualAudioDriver_HasBoxProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                 pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress) {
    // 此方法返回音频盒对象是否具有指定属性

#pragma unused(inClientProcessID)

    // 声明局部变量
    Boolean theAnswer = false;

    // 检查参数
    FailIf(inDriver != gAudioServerPlugInDriverRef, Done,
           "VirtualAudioDriver_HasBoxProperty: 无效的驱动程序引用");
    FailIf(inAddress == NULL, Done,
           "VirtualAudioDriver_HasBoxProperty: 地址为空");
    FailIf(inObjectID != kObjectID_Box, Done,
           "VirtualAudioDriver_HasBoxProperty: 不是音频盒对象");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetBoxPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inAddress->mSelector) {
        // 基本对象属性
        case kAudioObjectPropertyBaseClass:        // 基类
        case kAudioObjectPropertyClass:            // 类
        case kAudioObjectPropertyOwner:            // 所有者
        case kAudioObjectPropertyName:             // 名称
        case kAudioObjectPropertyModelName:        // 型号名称
        case kAudioObjectPropertyManufacturer:     // 制造商
        case kAudioObjectPropertyOwnedObjects:     // 拥有的对象
        case kAudioObjectPropertyIdentify:         // 标识
        case kAudioObjectPropertySerialNumber:     // 序列号
        case kAudioObjectPropertyFirmwareVersion:  // 固件版本

            // 音频盒特定属性
        case kAudioBoxPropertyBoxUID:              // 音频盒UID
        case kAudioBoxPropertyTransportType:       // 传输类型
        case kAudioBoxPropertyHasAudio:            // 是否有音频
        case kAudioBoxPropertyHasVideo:            // 是否有视频
        case kAudioBoxPropertyHasMIDI:             // 是否有MIDI
        case kAudioBoxPropertyIsProtected:         // 是否受保护
        case kAudioBoxPropertyAcquired:            // 是否已获取
        case kAudioBoxPropertyAcquisitionFailed:   // 获取是否失败
        case kAudioBoxPropertyDeviceList:          // 设备列表
            theAnswer = true;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_IsBoxPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                         pid_t inClientProcessID,
                                                         const AudioObjectPropertyAddress *inAddress,
                                                         Boolean *outIsSettable) {
    // 此方法返回音频盒对象上的给定属性是否可以更改其值

#pragma unused(inClientProcessID)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_IsBoxPropertySettable: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_IsBoxPropertySettable: 地址为空");
    FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_IsBoxPropertySettable: 没有地方存放返回值");
    FailWithAction(inObjectID != kObjectID_Box, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_IsBoxPropertySettable: 不是音频盒对象");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetBoxPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inAddress->mSelector) {
        // 只读属性
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyModelName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertySerialNumber:
        case kAudioObjectPropertyFirmwareVersion:
        case kAudioBoxPropertyBoxUID:
        case kAudioBoxPropertyTransportType:
        case kAudioBoxPropertyHasAudio:
        case kAudioBoxPropertyHasVideo:
        case kAudioBoxPropertyHasMIDI:
        case kAudioBoxPropertyIsProtected:
        case kAudioBoxPropertyAcquisitionFailed:
        case kAudioBoxPropertyDeviceList:
            *outIsSettable = false;
            break;

            // 可写属性
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyIdentify:
        case kAudioBoxPropertyAcquired:
            *outIsSettable = true;
            break;

            // 未知属性
        default:
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    }

    Done:
    return theAnswer;
}

#pragma mark Device Property Operations

static Boolean VirtualAudioDriver_HasDeviceProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                    pid_t inClientProcessID,
                                                    const AudioObjectPropertyAddress *inAddress) {
    // 此方法返回设备对象是否具有指定属性

#pragma unused(inClientProcessID)

    // 声明局部变量
    Boolean theAnswer = false;

    // 检查参数
    FailIf(inDriver != gAudioServerPlugInDriverRef, Done,
           "VirtualAudioDriver_HasDeviceProperty: 无效的驱动程序引用");
    FailIf(inAddress == NULL, Done,
           "VirtualAudioDriver_HasDeviceProperty: 地址为空");
    FailIf(inObjectID != kObjectID_Device, Done,
           "VirtualAudioDriver_HasDeviceProperty: 不是设备对象");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetDevicePropertyData() 方法中有关于每个属性的更详细说明。
    switch (inAddress->mSelector) {
        // 基本属性（不需要特殊条件的属性）
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyIcon:
        case kAudioDevicePropertyStreams:
            theAnswer = true;
            break;

            // 需要检查作用域的属性（输入或输出）
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
            theAnswer = (inAddress->mScope == kAudioObjectPropertyScopeInput) ||
                        (inAddress->mScope == kAudioObjectPropertyScopeOutput);
            break;

            // 需要检查元素编号的属性
        case kAudioObjectPropertyElementName:
            theAnswer = inAddress->mElement <= 2;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus
VirtualAudioDriver_IsDevicePropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                            pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                            Boolean *outIsSettable) {
    // 此方法返回设备对象上的给定属性是否可以更改其值

#pragma unused(inClientProcessID)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_IsDevicePropertySettable: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_IsDevicePropertySettable: 地址为空");
    FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_IsDevicePropertySettable: 没有地方存放返回值");
    FailWithAction(inObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_IsDevicePropertySettable: 不是设备对象");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetDevicePropertyData() 方法中有关于每个属性的更详细说明。
    switch (inAddress->mSelector) {
        // 只读属性
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyElementName:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyIcon:
            *outIsSettable = false;
            break;

            // 可写属性
        case kAudioDevicePropertyNominalSampleRate:
            *outIsSettable = true;
            break;

            // 未知属性
        default:
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    }

    Done:
    return theAnswer;
}

#pragma mark Stream Property Operations

static Boolean VirtualAudioDriver_HasStreamProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                    pid_t inClientProcessID,
                                                    const AudioObjectPropertyAddress *inAddress) {
    // 此方法返回流对象是否具有指定属性

#pragma unused(inClientProcessID)

    // 声明局部变量
    Boolean theAnswer = false;

    // 检查参数
    FailIf(inDriver != gAudioServerPlugInDriverRef, Done,
           "VirtualAudioDriver_HasStreamProperty: 无效的驱动程序引用");
    FailIf(inAddress == NULL, Done,
           "VirtualAudioDriver_HasStreamProperty: 地址为空");
    FailIf((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output), Done,
           "VirtualAudioDriver_HasStreamProperty: 不是流对象");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetStreamPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inAddress->mSelector) {
        // 基本对象属性
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyName:

            // 流特定属性
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            theAnswer = true;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus
VirtualAudioDriver_IsStreamPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                            pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                            Boolean *outIsSettable) {
    // 此方法返回流对象上的给定属性是否可以更改其值

#pragma unused(inClientProcessID)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_IsStreamPropertySettable: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_IsStreamPropertySettable: 地址为空");
    FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_IsStreamPropertySettable: 没有地方存放返回值");
    FailWithAction((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output),
                   theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_IsStreamPropertySettable: 不是流对象");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetStreamPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inAddress->mSelector) {
        // 只读属性
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyName:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outIsSettable = false;
            break;

            // 可写属性
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outIsSettable = true;
            break;

            // 未知属性
        default:
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    }

    Done:
    return theAnswer;
}

#pragma mark Control Property Operations

static Boolean VirtualAudioDriver_HasControlProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                     pid_t inClientProcessID,
                                                     const AudioObjectPropertyAddress *inAddress) {
    // 此方法返回控制对象是否具有指定属性

#pragma unused(inClientProcessID)

    // 声明局部变量
    Boolean theAnswer = false;

    // 检查参数
    FailIf(inDriver != gAudioServerPlugInDriverRef, Done,
           "VirtualAudioDriver_HasControlProperty: 无效的驱动程序引用");
    FailIf(inAddress == NULL, Done,
           "VirtualAudioDriver_HasControlProperty: 地址为空");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetControlPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inObjectID) {
        // 音量控制
        case kObjectID_Volume_Input_Master:
        case kObjectID_Volume_Output_Master:
            switch (inAddress->mSelector) {
                // 基本对象属性
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioControlPropertyScope:
                case kAudioControlPropertyElement:
                    // 音量控制特定属性
                case kAudioLevelControlPropertyScalarValue:
                case kAudioLevelControlPropertyDecibelValue:
                case kAudioLevelControlPropertyDecibelRange:
                case kAudioLevelControlPropertyConvertScalarToDecibels:
                case kAudioLevelControlPropertyConvertDecibelsToScalar:
                    theAnswer = true;
                    break;
            }
            break;

            // 静音控制
        case kObjectID_Mute_Input_Master:
        case kObjectID_Mute_Output_Master:
            switch (inAddress->mSelector) {
                // 基本对象属性
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioControlPropertyScope:
                case kAudioControlPropertyElement:
                    // 静音控制特定属性
                case kAudioBooleanControlPropertyValue:
                    theAnswer = true;
                    break;
            }
            break;

            // 数据源/目标控制
        case kObjectID_DataSource_Input_Master:
        case kObjectID_DataSource_Output_Master:
        case kObjectID_DataDestination_PlayThru_Master:
            switch (inAddress->mSelector) {
                // 基本对象属性
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioControlPropertyScope:
                case kAudioControlPropertyElement:
                    // 选择器控制特定属性
                case kAudioSelectorControlPropertyCurrentItem:
                case kAudioSelectorControlPropertyAvailableItems:
                case kAudioSelectorControlPropertyItemName:
                    theAnswer = true;
                    break;
            }
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus
VirtualAudioDriver_IsControlPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                             pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                             Boolean *outIsSettable) {
    // 此方法返回控制对象上的给定属性是否可以更改其值

#pragma unused(inClientProcessID)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_IsControlPropertySettable: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_IsControlPropertySettable: 地址为空");
    FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_IsControlPropertySettable: 没有地方存放返回值");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetControlPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inObjectID) {
        // 音量控制
        case kObjectID_Volume_Input_Master:
        case kObjectID_Volume_Output_Master:
            switch (inAddress->mSelector) {
                // 只读属性
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioControlPropertyScope:
                case kAudioControlPropertyElement:
                case kAudioLevelControlPropertyDecibelRange:
                case kAudioLevelControlPropertyConvertScalarToDecibels:
                case kAudioLevelControlPropertyConvertDecibelsToScalar:
                    *outIsSettable = false;
                    break;

                    // 可写属性
                case kAudioLevelControlPropertyScalarValue:
                case kAudioLevelControlPropertyDecibelValue:
                    *outIsSettable = true;
                    break;

                    // 未知属性
                default:
                    theAnswer = kAudioHardwareUnknownPropertyError;
                    break;
            }
            break;

            // 静音控制
        case kObjectID_Mute_Input_Master:
        case kObjectID_Mute_Output_Master:
            switch (inAddress->mSelector) {
                // 只读属性
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioControlPropertyScope:
                case kAudioControlPropertyElement:
                    *outIsSettable = false;
                    break;

                    // 可写属性
                case kAudioBooleanControlPropertyValue:
                    *outIsSettable = true;
                    break;

                    // 未知属性
                default:
                    theAnswer = kAudioHardwareUnknownPropertyError;
                    break;
            }
            break;

            // 数据源/目标控制
        case kObjectID_DataSource_Input_Master:
        case kObjectID_DataSource_Output_Master:
        case kObjectID_DataDestination_PlayThru_Master:
            switch (inAddress->mSelector) {
                // 只读属性
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioControlPropertyScope:
                case kAudioControlPropertyElement:
                case kAudioSelectorControlPropertyAvailableItems:
                case kAudioSelectorControlPropertyItemName:
                    *outIsSettable = false;
                    break;

                    // 可写属性
                case kAudioSelectorControlPropertyCurrentItem:
                    *outIsSettable = true;
                    break;

                    // 未知属性
                default:
                    theAnswer = kAudioHardwareUnknownPropertyError;
                    break;
            }
            break;

            // 未知对象
        default:
            theAnswer = kAudioHardwareBadObjectError;
            break;
    }

    Done:
    return theAnswer;
}