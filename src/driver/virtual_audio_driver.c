//
// Created by AhogeK on 12/10/24.
//

#include "driver/virtual_audio_driver.h"
#include <stdatomic.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <sys/syslog.h>

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

static OSStatus VirtualAudioDriver_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                       pid_t inClientProcessID,
                                                       const AudioObjectPropertyAddress *inAddress,
                                                       UInt32 inQualifierDataSize, const void *inQualifierData,
                                                       UInt32 *outDataSize) {
    // 此方法返回属性的数据大小（以字节为单位）

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetPropertyDataSize: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetPropertyDataSize: 地址为空");
    FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetPropertyDataSize: 没有地方存放返回值");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inObjectID) {
        case kObjectID_PlugIn:
            theAnswer = VirtualAudioDriver_GetPlugInPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress,
                                                                     inQualifierDataSize, inQualifierData, outDataSize);
            break;

        case kObjectID_Box:
            theAnswer = VirtualAudioDriver_GetBoxPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress,
                                                                  inQualifierDataSize, inQualifierData, outDataSize);
            break;

        case kObjectID_Device:
            theAnswer = VirtualAudioDriver_GetDevicePropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress,
                                                                     inQualifierDataSize, inQualifierData, outDataSize);
            break;

        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            theAnswer = VirtualAudioDriver_GetStreamPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress,
                                                                     inQualifierDataSize, inQualifierData, outDataSize);
            break;

        case kObjectID_Volume_Input_Master:
        case kObjectID_Volume_Output_Master:
        case kObjectID_Mute_Input_Master:
        case kObjectID_Mute_Output_Master:
        case kObjectID_DataSource_Input_Master:
        case kObjectID_DataSource_Output_Master:
        case kObjectID_DataDestination_PlayThru_Master:
            theAnswer = VirtualAudioDriver_GetControlPropertyDataSize(inDriver, inObjectID, inClientProcessID,
                                                                      inAddress, inQualifierDataSize, inQualifierData,
                                                                      outDataSize);
            break;

        default:
            theAnswer = kAudioHardwareBadObjectError;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                   pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                                   UInt32 inQualifierDataSize, const void *inQualifierData,
                                                   UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetPropertyData: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetPropertyData: 地址为空");
    FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetPropertyData: 没有地方存放返回值大小");
    FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetPropertyData: 没有地方存放返回值");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    //
    // 另外，由于大部分返回的数据是静态的，很少需要锁定状态互斥锁。
    switch (inObjectID) {
        case kObjectID_PlugIn:
            theAnswer = VirtualAudioDriver_GetPlugInPropertyData(inDriver,
                                                                 inObjectID,
                                                                 inClientProcessID,
                                                                 inAddress,
                                                                 inQualifierDataSize,
                                                                 inQualifierData,
                                                                 inDataSize,
                                                                 outDataSize,
                                                                 outData);
            break;

        case kObjectID_Box:
            theAnswer = VirtualAudioDriver_GetBoxPropertyData(inDriver,
                                                              inObjectID,
                                                              inClientProcessID,
                                                              inAddress,
                                                              inQualifierDataSize,
                                                              inQualifierData,
                                                              inDataSize,
                                                              outDataSize,
                                                              outData);
            break;

        case kObjectID_Device:
            theAnswer = VirtualAudioDriver_GetDevicePropertyData(inDriver,
                                                                 inObjectID,
                                                                 inClientProcessID,
                                                                 inAddress,
                                                                 inQualifierDataSize,
                                                                 inQualifierData,
                                                                 inDataSize,
                                                                 outDataSize,
                                                                 outData);
            break;

        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            theAnswer = VirtualAudioDriver_GetStreamPropertyData(inDriver,
                                                                 inObjectID,
                                                                 inClientProcessID,
                                                                 inAddress,
                                                                 inQualifierDataSize,
                                                                 inQualifierData,
                                                                 inDataSize,
                                                                 outDataSize,
                                                                 outData);
            break;

        case kObjectID_Volume_Input_Master:
        case kObjectID_Volume_Output_Master:
        case kObjectID_Mute_Input_Master:
        case kObjectID_Mute_Output_Master:
        case kObjectID_DataSource_Input_Master:
        case kObjectID_DataSource_Output_Master:
        case kObjectID_DataDestination_PlayThru_Master:
            theAnswer = VirtualAudioDriver_GetControlPropertyData(inDriver,
                                                                  inObjectID,
                                                                  inClientProcessID,
                                                                  inAddress,
                                                                  inQualifierDataSize,
                                                                  inQualifierData,
                                                                  inDataSize,
                                                                  outDataSize,
                                                                  outData);
            break;

        default:
            theAnswer = kAudioHardwareBadObjectError;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_SetPropertyData(AudioServerPlugInDriverRef inDriver,
                                                   AudioObjectID inObjectID,
                                                   pid_t inClientProcessID,
                                                   const AudioObjectPropertyAddress *inAddress,
                                                   UInt32 inQualifierDataSize,
                                                   const void *inQualifierData,
                                                   UInt32 inDataSize,
                                                   const void *inData) {
    // 声明局部变量
    OSStatus theAnswer = 0;
    UInt32 theNumberPropertiesChanged = 0;
    AudioObjectPropertyAddress theChangedAddresses[2];

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef,
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_SetPropertyData: 错误的驱动引用");
    FailWithAction(inAddress == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetPropertyData: 地址为空");

    // 注意，对于每个对象，此驱动程序实现了所有必需的属性以及一些有用但不是必需的额外属性。
    // 在 VirtualAudioDriver_GetPropertyData() 方法中有关于每个属性的更详细的注释。
    switch (inObjectID) {
        case kObjectID_PlugIn:
            theAnswer = VirtualAudioDriver_SetPlugInPropertyData(inDriver,
                                                                 inObjectID,
                                                                 inClientProcessID,
                                                                 inAddress,
                                                                 inQualifierDataSize,
                                                                 inQualifierData,
                                                                 inDataSize,
                                                                 inData,
                                                                 &theNumberPropertiesChanged,
                                                                 theChangedAddresses);
            break;

        case kObjectID_Box:
            theAnswer = VirtualAudioDriver_SetBoxPropertyData(inDriver,
                                                              inObjectID,
                                                              inClientProcessID,
                                                              inAddress,
                                                              inQualifierDataSize,
                                                              inQualifierData,
                                                              inDataSize,
                                                              inData,
                                                              &theNumberPropertiesChanged,
                                                              theChangedAddresses);
            break;

        case kObjectID_Device:
            theAnswer = VirtualAudioDriver_SetDevicePropertyData(inDriver,
                                                                 inObjectID,
                                                                 inClientProcessID,
                                                                 inAddress,
                                                                 inQualifierDataSize,
                                                                 inQualifierData,
                                                                 inDataSize,
                                                                 inData,
                                                                 &theNumberPropertiesChanged,
                                                                 theChangedAddresses);
            break;

        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            theAnswer = VirtualAudioDriver_SetStreamPropertyData(inDriver,
                                                                 inObjectID,
                                                                 inClientProcessID,
                                                                 inAddress,
                                                                 inQualifierDataSize,
                                                                 inQualifierData,
                                                                 inDataSize,
                                                                 inData,
                                                                 &theNumberPropertiesChanged,
                                                                 theChangedAddresses);
            break;

        case kObjectID_Volume_Input_Master:
        case kObjectID_Volume_Output_Master:
        case kObjectID_Mute_Input_Master:
        case kObjectID_Mute_Output_Master:
        case kObjectID_DataSource_Input_Master:
        case kObjectID_DataSource_Output_Master:
        case kObjectID_DataDestination_PlayThru_Master:
            theAnswer = VirtualAudioDriver_SetControlPropertyData(inDriver,
                                                                  inObjectID,
                                                                  inClientProcessID,
                                                                  inAddress,
                                                                  inQualifierDataSize,
                                                                  inQualifierData,
                                                                  inDataSize,
                                                                  inData,
                                                                  &theNumberPropertiesChanged,
                                                                  theChangedAddresses);
            break;

        default:
            theAnswer = kAudioHardwareBadObjectError;
            break;
    };

    // 发送任何通知
    if (theNumberPropertiesChanged > 0) {
        gPlugIn_Host->PropertiesChanged(gPlugIn_Host,
                                        inObjectID,
                                        theNumberPropertiesChanged,
                                        theChangedAddresses);
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

static OSStatus
VirtualAudioDriver_GetPlugInPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                             pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                             UInt32 inQualifierDataSize, const void *inQualifierData,
                                             UInt32 *outDataSize) {
    // 此方法返回插件对象上的属性数据大小（以字节为单位）

#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetPlugInPropertyDataSize: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetPlugInPropertyDataSize: 地址为空");
    FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetPlugInPropertyDataSize: 没有地方存放返回值");
    FailWithAction(inObjectID != kObjectID_PlugIn, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetPlugInPropertyDataSize: 不是插件对象");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetPlugInPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID);
            break;

        case kAudioObjectPropertyManufacturer:
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = gBox_Acquired ? 2 * sizeof(AudioClassID) : sizeof(AudioClassID);
            break;

        case kAudioPlugInPropertyBoxList:
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioPlugInPropertyTranslateUIDToBox:
            *outDataSize = sizeof(AudioObjectID);
            break;

        case kAudioPlugInPropertyDeviceList:
            *outDataSize = gBox_Acquired ? sizeof(AudioClassID) : 0;
            break;

        case kAudioPlugInPropertyTranslateUIDToDevice:
            *outDataSize = sizeof(AudioObjectID);
            break;

        case kAudioPlugInPropertyResourceBundle:
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyCustomPropertyInfoList:
            *outDataSize = sizeof(AudioServerPlugInCustomPropertyInfo);
            break;

        case kPlugIn_CustomPropertyID:
            FailWithAction(inQualifierDataSize != sizeof(CFStringRef),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetPlugInPropertyDataSize: 限定符大小错误");
            FailWithAction(inQualifierData == NULL,
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetPlugInPropertyDataSize: 限定符为空");
            DebugMsg("VirtualAudioDriver_GetPlugInPropertyDataSize: 传递给我们的限定符是:");
            CFShow(*((const CFPropertyListRef *) inQualifierData));
            *outDataSize = sizeof(CFPropertyListRef);
            break;

        default:
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_GetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                         pid_t inClientProcessID,
                                                         const AudioObjectPropertyAddress *inAddress,
                                                         UInt32 inQualifierDataSize, const void *inQualifierData,
                                                         UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
#pragma unused(inClientProcessID)

    // 声明局部变量
    OSStatus theAnswer = 0;
    UInt32 theNumberItemsToFetch;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetPlugInPropertyData: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetPlugInPropertyData: 地址为空");
    FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetPlugInPropertyData: 没有地方存放返回值大小");
    FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetPlugInPropertyData: 没有地方存放返回值");
    FailWithAction(inObjectID != kObjectID_PlugIn, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetPlugInPropertyData: 不是插件对象");

    // 处理不同的属性选择器
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            // 插件的基类是 kAudioObjectClassID
            FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: 空间不足以返回插件的 kAudioObjectPropertyBaseClass");
            *((AudioClassID *) outData) = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioObjectPropertyClass:
            // 常规驱动的类总是 kAudioPlugInClassID
            FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: 空间不足以返回插件的 kAudioObjectPropertyClass");
            *((AudioClassID *) outData) = kAudioPlugInClassID;
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioObjectPropertyOwner:
            // 插件没有所有者对象
            FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: 空间不足以返回插件的 kAudioObjectPropertyOwner");
            *((AudioObjectID *) outData) = kAudioObjectUnknown;
            *outDataSize = sizeof(AudioObjectID);
            break;

        case kAudioObjectPropertyManufacturer:
            // 插件制造商的可读名称
            FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: 空间不足以返回插件的 kAudioObjectPropertyManufacturer");
            *((CFStringRef *) outData) = CFSTR("Virtual Audio Driver");
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyOwnedObjects:
            // 计算请求的项目数量
            theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

            // 限制为驱动程序实现的盒子数量（只有1个）
            if (theNumberItemsToFetch > (gBox_Acquired ? 2 : 1)) {
                theNumberItemsToFetch = (gBox_Acquired ? 2 : 1);
            }

            // 将设备的对象ID写入返回值
            if (theNumberItemsToFetch > 1) {
                ((AudioObjectID *) outData)[0] = kObjectID_Box;
                ((AudioObjectID *) outData)[1] = kObjectID_Device;
            } else if (theNumberItemsToFetch > 0) {
                ((AudioObjectID *) outData)[0] = kObjectID_Box;
            }

            *outDataSize = theNumberItemsToFetch * sizeof(AudioClassID);
            break;

        case kAudioPlugInPropertyBoxList:
            // 计算请求的项目数量
            theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

            // 限制为驱动程序实现的盒子数量（只有1个）
            if (theNumberItemsToFetch > 1) {
                theNumberItemsToFetch = 1;
            }

            // 将设备的对象ID写入返回值
            if (theNumberItemsToFetch > 0) {
                ((AudioObjectID *) outData)[0] = kObjectID_Box;
            }

            *outDataSize = theNumberItemsToFetch * sizeof(AudioClassID);
            break;

        case kAudioPlugInPropertyTranslateUIDToBox:
            // 将CFString限定符转换为对应的盒子对象ID
            FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: 空间不足以返回 kAudioPlugInPropertyTranslateUIDToBox");
            FailWithAction(inQualifierDataSize != sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: kAudioPlugInPropertyTranslateUIDToBox 的限定符大小错误");
            FailWithAction(inQualifierData == NULL, theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: kAudioPlugInPropertyTranslateUIDToBox 没有限定符");

            if (CFStringCompare(*((CFStringRef *) inQualifierData), CFSTR(kBox_UID), 0) == kCFCompareEqualTo) {
                *((AudioObjectID *) outData) = kObjectID_Box;
            } else {
                *((AudioObjectID *) outData) = kAudioObjectUnknown;
            }
            *outDataSize = sizeof(AudioObjectID);
            break;

        case kAudioPlugInPropertyDeviceList:
            // 计算请求的项目数量
            theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

            // 限制为驱动程序实现的设备数量
            if (theNumberItemsToFetch > (gBox_Acquired ? 1 : 0)) {
                theNumberItemsToFetch = (gBox_Acquired ? 1 : 0);
            }

            // 将设备的对象ID写入返回值
            if (theNumberItemsToFetch > 0) {
                ((AudioObjectID *) outData)[0] = kObjectID_Device;
            }

            *outDataSize = theNumberItemsToFetch * sizeof(AudioClassID);
            break;

        case kAudioPlugInPropertyTranslateUIDToDevice:
            // 将CFString限定符转换为对应的设备对象ID
            FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: 空间不足以返回 kAudioPlugInPropertyTranslateUIDToDevice");
            FailWithAction(inQualifierDataSize != sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: kAudioPlugInPropertyTranslateUIDToDevice 的限定符大小错误");
            FailWithAction(inQualifierData == NULL, theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: kAudioPlugInPropertyTranslateUIDToDevice 没有限定符");

            if (CFStringCompare(*((CFStringRef *) inQualifierData), CFSTR(kDevice_UID), 0) == kCFCompareEqualTo) {
                *((AudioObjectID *) outData) = kObjectID_Device;
            } else {
                *((AudioObjectID *) outData) = kAudioObjectUnknown;
            }
            *outDataSize = sizeof(AudioObjectID);
            break;

        case kAudioPlugInPropertyResourceBundle:
            // 资源包是相对于插件包路径的路径
            // 要指定应使用插件包本身，我们只返回空字符串
            FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: 空间不足以返回 kAudioPlugInPropertyResourceBundle");
            *((CFStringRef *) outData) = CFSTR("");
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyCustomPropertyInfoList:
            // 返回描述自定义属性数据类型的 AudioServerPlugInCustomPropertyInfo 数组
            FailWithAction(inDataSize < sizeof(AudioServerPlugInCustomPropertyInfo),
                           theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: 空间不足以返回 kAudioObjectPropertyCustomPropertyInfoList");
            ((AudioServerPlugInCustomPropertyInfo *) outData)->mSelector = kPlugIn_CustomPropertyID;
            ((AudioServerPlugInCustomPropertyInfo *) outData)->mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFString;
            ((AudioServerPlugInCustomPropertyInfo *) outData)->mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
            *outDataSize = sizeof(AudioServerPlugInCustomPropertyInfo);
            break;

        case kPlugIn_CustomPropertyID:
            FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: 空间不足以返回 kPlugIn_CustomPropertyID");
            FailWithAction(inQualifierDataSize != sizeof(CFPropertyListRef),
                           theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: kPlugIn_CustomPropertyID 的限定符大小错误");
            FailWithAction(inQualifierData == NULL, theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetPlugInPropertyData: kPlugIn_CustomPropertyID 没有限定符");

            DebugMsg("VirtualAudioDriver_GetPlugInPropertyData: 传递给我们的限定符是:");
            const CFPropertyListRef qualifierData = *((const CFPropertyListRef *) inQualifierData);
            CFShow(qualifierData);

            *((CFStringRef *) outData) = CFSTR("Virtual Audio Driver Custom Property");
            *outDataSize = sizeof(CFStringRef);
            break;

        default:
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_SetPlugInPropertyData(AudioServerPlugInDriverRef inDriver,
                                                         AudioObjectID inObjectID,
                                                         pid_t inClientProcessID,
                                                         const AudioObjectPropertyAddress *inAddress,
                                                         UInt32 inQualifierDataSize,
                                                         const void *inQualifierData,
                                                         UInt32 inDataSize,
                                                         const void *inData,
                                                         UInt32 *outNumberPropertiesChanged,
                                                         const AudioObjectPropertyAddress *outChangedAddresses) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData, inDataSize, inData)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef,
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_SetPlugInPropertyData: 错误的驱动引用");
    FailWithAction(inAddress == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetPlugInPropertyData: 地址为空");
    FailWithAction(outNumberPropertiesChanged == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetPlugInPropertyData: 没有地方返回更改的属性数量");
    FailWithAction(outChangedAddresses == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetPlugInPropertyData: 没有地方返回更改的属性");
    FailWithAction(inObjectID != kObjectID_PlugIn,
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_SetPlugInPropertyData: 不是插件对象");

    // 初始化返回的更改属性数量
    *outNumberPropertiesChanged = 0;

    // 注意，对于每个对象，此驱动程序实现了所有必需的属性以及一些有用但不是必需的额外属性。
    // 在 VirtualAudioDriver_GetPlugInPropertyData() 方法中有关于每个属性的更详细的注释。
    switch (inAddress->mSelector) {
        case kPlugIn_CustomPropertyID:
            FailWithAction(inDataSize != sizeof(CFStringRef),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_SetPlugInPropertyData: kPlugIn_CustomPropertyID 的返回值空间不足");
            FailWithAction(inQualifierDataSize != sizeof(CFPropertyListRef),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_SetPlugInPropertyData: kPlugIn_CustomPropertyID 的限定符大小错误");
            FailWithAction(inQualifierData == NULL,
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_SetPlugInPropertyData: kPlugIn_CustomPropertyID 无限定符");
            DebugMsg("VirtualAudioDriver_SetPlugInPropertyData: 传递给我们的限定符是:");
            CFShow(*((CFPropertyListRef *) inQualifierData));
            DebugMsg("VirtualAudioDriver_SetPlugInPropertyData: 传递给我们的数据是:");
            CFShow(*((CFStringRef *) inData));
            break;

        default:
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    };

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

static OSStatus VirtualAudioDriver_GetBoxPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                          pid_t inClientProcessID,
                                                          const AudioObjectPropertyAddress *inAddress,
                                                          UInt32 inQualifierDataSize, const void *inQualifierData,
                                                          UInt32 *outDataSize) {
    // 此方法返回音频盒对象上的属性数据大小（以字节为单位）

#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetBoxPropertyDataSize: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetBoxPropertyDataSize: 地址为空");
    FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetBoxPropertyDataSize: 没有地方存放返回值");
    FailWithAction(inObjectID != kObjectID_Box, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetBoxPropertyDataSize: 不是音频盒对象");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetBoxPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID);
            break;

        case kAudioObjectPropertyName:
        case kAudioObjectPropertyModelName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertySerialNumber:
        case kAudioObjectPropertyFirmwareVersion:
        case kAudioBoxPropertyBoxUID:
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = 0;
            break;

        case kAudioObjectPropertyIdentify:
        case kAudioBoxPropertyTransportType:
        case kAudioBoxPropertyHasAudio:
        case kAudioBoxPropertyHasVideo:
        case kAudioBoxPropertyHasMIDI:
        case kAudioBoxPropertyIsProtected:
        case kAudioBoxPropertyAcquired:
        case kAudioBoxPropertyAcquisitionFailed:
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioBoxPropertyDeviceList: {
            pthread_mutex_lock(&gPlugIn_StateMutex);
            *outDataSize = gBox_Acquired ? sizeof(AudioObjectID) : 0;
            pthread_mutex_unlock(&gPlugIn_StateMutex);
        }
            break;

        default:
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_GetBoxPropertyData(AudioServerPlugInDriverRef inDriver,
                                                      AudioObjectID inObjectID,
                                                      pid_t inClientProcessID,
                                                      const AudioObjectPropertyAddress *inAddress,
                                                      UInt32 inQualifierDataSize,
                                                      const void *inQualifierData,
                                                      UInt32 inDataSize,
                                                      UInt32 *outDataSize,
                                                      void *outData) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetBoxPropertyData: 错误的驱动引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetBoxPropertyData: 地址为空");
    FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetBoxPropertyData: 没有地方存放返回值大小");
    FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetBoxPropertyData: 没有地方存放返回值");
    FailWithAction(inObjectID != kObjectID_Box, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetBoxPropertyData: 不是插件对象");

    // 注意对于每个对象，此驱动程序实现了所有必需的属性以及一些有用但不是必需的额外属性
    //
    // 另外，由于大多数将要返回的数据是静态的，很少需要锁定状态互斥锁

    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            // kAudioBoxClassID 的基类是 kAudioObjectClassID
            FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioObjectPropertyBaseClass返回值空间不足");
            *((AudioClassID *) outData) = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioObjectPropertyClass:
            // 对于常规驱动程序，类始终是 kAudioBoxClassID
            FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioObjectPropertyClass返回值空间不足");
            *((AudioClassID *) outData) = kAudioBoxClassID;
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioObjectPropertyOwner:
            // 所有者是插件对象
            FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioObjectPropertyOwner返回值空间不足");
            *((AudioObjectID *) outData) = kObjectID_PlugIn;
            *outDataSize = sizeof(AudioObjectID);
            break;

        case kAudioObjectPropertyName:
            // 这是box制造商的人类可读名称
            FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioObjectPropertyManufacturer返回值空间不足");
            pthread_mutex_lock(&gPlugIn_StateMutex);
            *((CFStringRef *) outData) = gBox_Name;
            pthread_mutex_unlock(&gPlugIn_StateMutex);
            if (*((CFStringRef *) outData) != NULL) {
                CFRetain(*((CFStringRef *) outData));
            }
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyModelName:
            // 这是box型号的人类可读名称
            FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioObjectPropertyManufacturer返回值空间不足");
            *((CFStringRef *) outData) = CFSTR("虚拟音频设备");
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyManufacturer:
            // 这是box制造商的人类可读名称
            FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioObjectPropertyManufacturer返回值空间不足");
            *((CFStringRef *) outData) = CFSTR("虚拟音频制造商");
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyOwnedObjects:
            // 这返回对象直接拥有的对象。Box不拥有任何对象
            *outDataSize = 0;
            break;

        case kAudioObjectPropertyIdentify:
            // 这用于在UI中高亮显示设备，但其值没有实际意义
            FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioObjectPropertyIdentify返回值空间不足");
            *((UInt32 *) outData) = 0;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioObjectPropertySerialNumber:
            // 这是box的人类可读序列号
            FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioObjectPropertySerialNumber返回值空间不足");
            *((CFStringRef *) outData) = CFSTR("00000001");
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyFirmwareVersion:
            // 这是box的人类可读固件版本
            FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioObjectPropertyFirmwareVersion返回值空间不足");
            *((CFStringRef *) outData) = CFSTR("1.0");
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioBoxPropertyBoxUID:
            // Box和设备一样具有UID
            FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioObjectPropertyManufacturer返回值空间不足");
            *((CFStringRef *) outData) = CFSTR(kBox_UID);
            break;

        case kAudioBoxPropertyTransportType:
            // 此值表示设备如何连接到系统。这可以是任何32位整数，
            // 但此属性的常用值在 <CoreAudio/AudioHardwareBase.h> 中定义
            FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioDevicePropertyTransportType返回值空间不足");
            *((UInt32 *) outData) = kAudioDeviceTransportTypeVirtual;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioBoxPropertyHasAudio:
            // 指示box是否具有音频功能
            FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioBoxPropertyHasAudio返回值空间不足");
            *((UInt32 *) outData) = 1;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioBoxPropertyHasVideo:
            // 指示box是否具有视频功能
            FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioBoxPropertyHasVideo返回值空间不足");
            *((UInt32 *) outData) = 0;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioBoxPropertyHasMIDI:
            // 指示box是否具有MIDI功能
            FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioBoxPropertyHasMIDI返回值空间不足");
            *((UInt32 *) outData) = 0;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioBoxPropertyIsProtected:
            // 指示box是否需要认证才能使用
            FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioBoxPropertyIsProtected返回值空间不足");
            *((UInt32 *) outData) = 0;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioBoxPropertyAcquired:
            // 当设置为非零值时，设备被本地机器获取使用
            FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioBoxPropertyAcquired返回值空间不足");
            pthread_mutex_lock(&gPlugIn_StateMutex);
            *((UInt32 *) outData) = gBox_Acquired ? 1 : 0;
            pthread_mutex_unlock(&gPlugIn_StateMutex);
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioBoxPropertyAcquisitionFailed:
            // 这用于通知尝试获取设备失败的情况
            FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                           "VirtualAudioDriver_GetBoxPropertyData: box的kAudioBoxPropertyAcquisitionFailed返回值空间不足");
            *((UInt32 *) outData) = 0;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioBoxPropertyDeviceList:
            // 这用于指示哪些设备来自这个box
            pthread_mutex_lock(&gPlugIn_StateMutex);
            if (gBox_Acquired) {
                FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done,
                               "VirtualAudioDriver_GetBoxPropertyData: box的kAudioBoxPropertyDeviceList返回值空间不足");
                *((AudioObjectID *) outData) = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
            } else {
                *outDataSize = 0;
            }
            pthread_mutex_unlock(&gPlugIn_StateMutex);
            break;

        default:
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_SetBoxPropertyData(AudioServerPlugInDriverRef inDriver,
                                                      AudioObjectID inObjectID,
                                                      pid_t inClientProcessID,
                                                      const AudioObjectPropertyAddress *inAddress,
                                                      UInt32 inQualifierDataSize,
                                                      const void *inQualifierData,
                                                      UInt32 inDataSize,
                                                      const void *inData,
                                                      UInt32 *outNumberPropertiesChanged,
                                                      AudioObjectPropertyAddress outChangedAddresses[2]) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData, inDataSize, inData)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef,
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_SetBoxPropertyData: 错误的驱动引用");
    FailWithAction(inAddress == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetBoxPropertyData: 地址为空");
    FailWithAction(outNumberPropertiesChanged == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetBoxPropertyData: 没有地方返回更改的属性数量");
    FailWithAction(outChangedAddresses == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetBoxPropertyData: 没有地方返回更改的属性");
    FailWithAction(inObjectID != kObjectID_Box,
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_SetBoxPropertyData: 不是盒子对象");

    // 初始化返回的更改属性数量
    *outNumberPropertiesChanged = 0;

    // 注意，对于每个对象，此驱动程序实现了所有必需的属性以及一些有用但不是必需的额外属性。
    // 在 VirtualAudioDriver_GetPlugInPropertyData() 方法中有关于每个属性的更详细的注释。
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyName:
            // 盒子应允许其名称可编辑
        {
            FailWithAction(inDataSize != sizeof(CFStringRef),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_SetBoxPropertyData: kAudioObjectPropertyName 的数据大小错误");
            FailWithAction(inData == NULL,
                           theAnswer = kAudioHardwareIllegalOperationError,
                           Done,
                           "VirtualAudioDriver_SetBoxPropertyData: 没有数据设置为 kAudioObjectPropertyName");

            // 使用 const 来避免丢失 const 限定符
            const CFStringRef *theNewName = (const CFStringRef *) inData;

            pthread_mutex_lock(&gPlugIn_StateMutex);
            if ((theNewName != NULL) && (*theNewName != NULL)) {
                CFRetain(*theNewName);
            }
            if (gBox_Name != NULL) {
                CFRelease(gBox_Name);
            }
            gBox_Name = *theNewName;
            pthread_mutex_unlock(&gPlugIn_StateMutex);

            *outNumberPropertiesChanged = 1;
            outChangedAddresses[0].mSelector = kAudioObjectPropertyName;
            outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
            outChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;
        }
            break;

        case kAudioObjectPropertyIdentify:
            // 由于我们没有任何实际的硬件闪烁，我们将为此属性安排一个通知以进行测试。
            // 请注意，真正的实现应该仅在硬件希望应用程序为设备闪烁其 UI 时发送通知。
        {
            syslog(LOG_NOTICE, "The identify property has been set on the Box implemented by the VirtualAudio driver.");
            FailWithAction(inDataSize != sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_SetBoxPropertyData: kAudioObjectPropertyIdentify 的数据大小错误");
            dispatch_after(dispatch_time(0, 2ULL * 1000ULL * 1000ULL * 1000ULL),
                           dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                        AudioObjectPropertyAddress theAddress = {kAudioObjectPropertyIdentify,
                                                                 kAudioObjectPropertyScopeGlobal,
                                                                 kAudioObjectPropertyElementMain};
                        gPlugIn_Host->PropertiesChanged(gPlugIn_Host, kObjectID_Box, 1, &theAddress);
                    });
        }
            break;

        case kAudioBoxPropertyAcquired:
            // 当盒子被获取时，意味着其内容（即设备）对系统可用
        {
            FailWithAction(inDataSize != sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_SetBoxPropertyData: kAudioBoxPropertyAcquired 的数据大小错误");

            pthread_mutex_lock(&gPlugIn_StateMutex);
            const UInt32 *newValue = (const UInt32 *) inData;
            if (gBox_Acquired != (*newValue != 0)) {
                // 新值不同于旧值，因此保存它
                gBox_Acquired = *newValue != 0;
                gPlugIn_Host->WriteToStorage(gPlugIn_Host, CFSTR("box acquired"),
                                             gBox_Acquired ? kCFBooleanTrue : kCFBooleanFalse);

                // 这意味着该属性和设备列表属性已更改
                *outNumberPropertiesChanged = 2;
                outChangedAddresses[0].mSelector = kAudioBoxPropertyAcquired;
                outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
                outChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;
                outChangedAddresses[1].mSelector = kAudioBoxPropertyDeviceList;
                outChangedAddresses[1].mScope = kAudioObjectPropertyScopeGlobal;
                outChangedAddresses[1].mElement = kAudioObjectPropertyElementMain;

                // 这也意味着插件的设备列表已更改
                dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                    AudioObjectPropertyAddress theAddress = {kAudioPlugInPropertyDeviceList,
                                                             kAudioObjectPropertyScopeGlobal,
                                                             kAudioObjectPropertyElementMain};
                    gPlugIn_Host->PropertiesChanged(gPlugIn_Host, kObjectID_PlugIn, 1, &theAddress);
                });
            }
            pthread_mutex_unlock(&gPlugIn_StateMutex);
        }
            break;

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

static OSStatus
VirtualAudioDriver_GetDevicePropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                             pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                             UInt32 inQualifierDataSize, const void *inQualifierData,
                                             UInt32 *outDataSize) {
    // 此方法返回设备对象上的属性数据大小（以字节为单位）

#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetDevicePropertyDataSize: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetDevicePropertyDataSize: 地址为空");
    FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetDevicePropertyDataSize: 没有地方存放返回值");
    FailWithAction(inObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetDevicePropertyDataSize: 不是设备对象");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    switch (inAddress->mSelector) {
        // 基本属性
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID);
            break;

            // 字符串属性
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyElementName:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
            *outDataSize = sizeof(CFStringRef);
            break;

            // 对象列表属性
        case kAudioObjectPropertyOwnedObjects:
            switch (inAddress->mScope) {
                case kAudioObjectPropertyScopeGlobal:
                    *outDataSize = 8 * sizeof(AudioObjectID);
                    break;
                case kAudioObjectPropertyScopeInput:
                case kAudioObjectPropertyScopeOutput:
                    *outDataSize = 4 * sizeof(AudioObjectID);
                    break;
            }
            break;

            // 整数属性
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
            *outDataSize = sizeof(UInt32);
            break;

            // 流属性
        case kAudioDevicePropertyStreams:
            switch (inAddress->mScope) {
                case kAudioObjectPropertyScopeGlobal:
                    *outDataSize = 2 * sizeof(AudioObjectID);
                    break;
                case kAudioObjectPropertyScopeInput:
                case kAudioObjectPropertyScopeOutput:
                    *outDataSize = sizeof(AudioObjectID);
                    break;
            }
            break;

            // 控制列表
        case kAudioObjectPropertyControlList:
            *outDataSize = 7 * sizeof(AudioObjectID);
            break;

            // 采样率相关
        case kAudioDevicePropertyNominalSampleRate:
            *outDataSize = sizeof(Float64);
            break;

        case kAudioDevicePropertyAvailableNominalSampleRates:
            *outDataSize = 2 * sizeof(AudioValueRange);
            break;

            // 声道相关
        case kAudioDevicePropertyPreferredChannelsForStereo:
            *outDataSize = 2 * sizeof(UInt32);
            break;

        case kAudioDevicePropertyPreferredChannelLayout:
            *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions) + (2 * sizeof(AudioChannelDescription));
            break;

            // 其他属性
        case kAudioDevicePropertyDeviceIsAlive:
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioDevicePropertyRelatedDevices:
            *outDataSize = sizeof(AudioObjectID);
            break;

        case kAudioDevicePropertyIcon:
            *outDataSize = sizeof(CFURLRef);
            break;

        default:
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_GetDevicePropertyData(AudioServerPlugInDriverRef inDriver,
                                                         AudioObjectID inObjectID,
                                                         pid_t inClientProcessID,
                                                         const AudioObjectPropertyAddress *inAddress,
                                                         UInt32 inQualifierDataSize,
                                                         const void *inQualifierData,
                                                         UInt32 inDataSize,
                                                         UInt32 *outDataSize,
                                                         void *outData) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

    // 声明局部变量
    OSStatus theAnswer = 0;
    UInt32 theNumberItemsToFetch;
    UInt32 theItemIndex;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef,
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_GetDevicePropertyData: 错误的驱动引用");
    FailWithAction(inAddress == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_GetDevicePropertyData: 地址为空");
    FailWithAction(outDataSize == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_GetDevicePropertyData: 没有地方存放返回值大小");
    FailWithAction(outData == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_GetDevicePropertyData: 没有地方存放返回值");
    FailWithAction(inObjectID != kObjectID_Device,
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_GetDevicePropertyData: 不是设备对象");

    // 注意对于每个对象，此驱动程序实现了所有必需的属性以及一些有用但不是必需的额外属性
    //
    // 另外，由于大多数将要返回的数据是静态的，很少需要锁定状态互斥锁

    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            // kAudioDeviceClassID 的基类是 kAudioObjectClassID
            FailWithAction(inDataSize < sizeof(AudioClassID),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioObjectPropertyBaseClass 返回值空间不足");
            *((AudioClassID *) outData) = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioObjectPropertyClass:
            // 设备创建者的类始终是 kAudioDeviceClassID
            FailWithAction(inDataSize < sizeof(AudioClassID),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioObjectPropertyClass 返回值空间不足");
            *((AudioClassID *) outData) = kAudioDeviceClassID;
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioObjectPropertyOwner:
            // 设备的所有者是插件对象
            FailWithAction(inDataSize < sizeof(AudioObjectID),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioObjectPropertyOwner 返回值空间不足");
            *((AudioObjectID *) outData) = kObjectID_PlugIn;
            *outDataSize = sizeof(AudioObjectID);
            break;

        case kAudioObjectPropertyName:
            // 这是设备的人类可读名称
            FailWithAction(inDataSize < sizeof(CFStringRef),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioObjectPropertyName 返回值空间不足");
            *((CFStringRef *) outData) = CFSTR("虚拟音频设备");
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyManufacturer:
            // 这是插件制造商的人类可读名称
            FailWithAction(inDataSize < sizeof(CFStringRef),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioObjectPropertyManufacturer 返回值空间不足");
            *((CFStringRef *) outData) = CFSTR("虚拟音频制造商");
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyElementName:
            // 这是元素的人类可读名称
            FailWithAction(inDataSize < sizeof(CFStringRef),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioObjectPropertyElementName 返回值空间不足");
            switch (inAddress->mElement) {
                case 0:
                    *((CFStringRef *) outData) = CFSTR("主声道");
                    break;
                case 1:
                    *((CFStringRef *) outData) = CFSTR("左声道");
                    break;
                case 2:
                    *((CFStringRef *) outData) = CFSTR("右声道");
                    break;
                default:
                    *((CFStringRef *) outData) = CFSTR("未知声道");
                    break;
            }
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyOwnedObjects:
            // 计算请求的项目数量。注意这个数量可以小于实际列表的大小
            theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

            // 设备拥有其流和控制。注意返回的内容取决于请求的范围
            switch (inAddress->mScope) {
                case kAudioObjectPropertyScopeGlobal:
                    // 全局范围意味着返回所有对象
                    if (theNumberItemsToFetch > 9) {
                        theNumberItemsToFetch = 9;
                    }

                    // 填充列表，返回所有请求的对象
                    for (theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex) {
                        ((AudioObjectID *) outData)[theItemIndex] = kObjectID_Stream_Input + theItemIndex;
                    }
                    break;

                case kAudioObjectPropertyScopeInput:
                    // 输入范围意味着只返回输入端的对象
                    if (theNumberItemsToFetch > 4) {
                        theNumberItemsToFetch = 4;
                    }

                    // 填充列表，返回正确的对象
                    for (theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex) {
                        ((AudioObjectID *) outData)[theItemIndex] = kObjectID_Stream_Input + theItemIndex;
                    }
                    break;

                case kAudioObjectPropertyScopeOutput:
                    // 输出范围意味着只返回输出端的对象
                    if (theNumberItemsToFetch > 4) {
                        theNumberItemsToFetch = 4;
                    }

                    // 填充列表，返回正确的对象
                    for (theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex) {
                        ((AudioObjectID *) outData)[theItemIndex] = kObjectID_Stream_Output + theItemIndex;
                    }
                    break;
            }

            // 报告写入了多少数据
            *outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
            break;

        case kAudioDevicePropertyDeviceUID:
            // 这是一个可以在不同启动会话中识别相同音频设备的持久令牌
            FailWithAction(inDataSize < sizeof(CFStringRef),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyDeviceUID 返回值空间不足");
            *((CFStringRef *) outData) = CFSTR(kDevice_UID);
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioDevicePropertyModelUID:
            // 这是一个可以识别相同类型设备的持久令牌
            FailWithAction(inDataSize < sizeof(CFStringRef),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyModelUID 返回值空间不足");
            *((CFStringRef *) outData) = CFSTR(kDevice_ModelUID);
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioDevicePropertyTransportType:
            // 此值表示设备如何连接到系统
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyTransportType 返回值空间不足");
            *((UInt32 *) outData) = kAudioDeviceTransportTypeVirtual;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioDevicePropertyRelatedDevices:
            // 相关设备属性标识非常密切相关的设备对象
            theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

            // 我们只有一个设备
            if (theNumberItemsToFetch > 1) {
                theNumberItemsToFetch = 1;
            }

            // 将设备的对象ID写入返回值
            if (theNumberItemsToFetch > 0) {
                ((AudioObjectID *) outData)[0] = kObjectID_Device;
            }

            // 报告写入了多少数据
            *outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
            break;

        case kAudioDevicePropertyClockDomain:
            // 此属性允许设备声明它在硬件中与哪些其他设备同步
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyClockDomain 返回值空间不足");
            *((UInt32 *) outData) = 0;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioDevicePropertyDeviceIsAlive:
            // 此属性返回设备是否处于活动状态
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyDeviceIsAlive 返回值空间不足");
            *((UInt32 *) outData) = 1;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioDevicePropertyDeviceIsRunning:
            // 此属性返回设备的IO是否正在运行
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyDeviceIsRunning 返回值空间不足");
            pthread_mutex_lock(&gPlugIn_StateMutex);
            *((UInt32 *) outData) = (gDevice_IOIsRunning > 0) ? 1 : 0;
            pthread_mutex_unlock(&gPlugIn_StateMutex);
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            // 此属性返回设备是否可以作为默认设备
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyDeviceCanBeDefaultDevice 返回值空间不足");
            *((UInt32 *) outData) = 1;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            // 此属性返回设备是否可以作为系统默认设备
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyDeviceCanBeDefaultSystemDevice 返回值空间不足");
            *((UInt32 *) outData) = 1;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioDevicePropertyLatency:
            // 此属性返回设备的呈现延迟
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyLatency 返回值空间不足");
            *((UInt32 *) outData) = 0;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioDevicePropertyStreams:
            // 计算请求的项目数量
            theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

            // 返回的内容取决于请求的范围
            switch (inAddress->mScope) {
                case kAudioObjectPropertyScopeGlobal:
                    // 全局范围意味着返回所有流
                    if (theNumberItemsToFetch > 2) {
                        theNumberItemsToFetch = 2;
                    }

                    // 填充列表，返回所有请求的流对象
                    if (theNumberItemsToFetch > 0) {
                        ((AudioObjectID *) outData)[0] = kObjectID_Stream_Input;
                    }
                    if (theNumberItemsToFetch > 1) {
                        ((AudioObjectID *) outData)[1] = kObjectID_Stream_Output;
                    }
                    break;

                case kAudioObjectPropertyScopeInput:
                    // 输入范围意味着只返回输入端的流对象
                    if (theNumberItemsToFetch > 1) {
                        theNumberItemsToFetch = 1;
                    }

                    // 填充列表，返回输入流对象
                    if (theNumberItemsToFetch > 0) {
                        ((AudioObjectID *) outData)[0] = kObjectID_Stream_Input;
                    }
                    break;

                case kAudioObjectPropertyScopeOutput:
                    // 输出范围意味着只返回输出端的流对象
                    if (theNumberItemsToFetch > 1) {
                        theNumberItemsToFetch = 1;
                    }

                    // 填充列表，返回输出流对象
                    if (theNumberItemsToFetch > 0) {
                        ((AudioObjectID *) outData)[0] = kObjectID_Stream_Output;
                    }
                    break;
            }

            // 报告写入了多少数据
            *outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
            break;

        case kAudioObjectPropertyControlList:
            // 计算请求的项目数量
            theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);
            if (theNumberItemsToFetch > 7) {
                theNumberItemsToFetch = 7;
            }

            // 填充列表，返回所有请求的控制对象
            for (theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex) {
                if (theItemIndex < 3) {
                    ((AudioObjectID *) outData)[theItemIndex] = kObjectID_Volume_Input_Master + theItemIndex;
                } else {
                    ((AudioObjectID *) outData)[theItemIndex] = kObjectID_Volume_Output_Master + (theItemIndex - 3);
                }
            }

            // 报告写入了多少数据
            *outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
            break;

        case kAudioDevicePropertySafetyOffset:
            // 此属性返回HAL可以读取和写入的最近距离
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertySafetyOffset 返回值空间不足");
            *((UInt32 *) outData) = 0;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioDevicePropertyNominalSampleRate:
            // 此属性返回设备的标称采样率
            FailWithAction(inDataSize < sizeof(Float64),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyNominalSampleRate 返回值空间不足");
            pthread_mutex_lock(&gPlugIn_StateMutex);
            *((Float64 *) outData) = gDevice_SampleRate;
            pthread_mutex_unlock(&gPlugIn_StateMutex);
            *outDataSize = sizeof(Float64);
            break;

        case kAudioDevicePropertyAvailableNominalSampleRates:
            // 此属性返回设备支持的所有标称采样率
            theNumberItemsToFetch = inDataSize / sizeof(AudioValueRange);

            // 限制返回的项目数量
            if (theNumberItemsToFetch > 2) {
                theNumberItemsToFetch = 2;
            }

            // 填充返回数组
            if (theNumberItemsToFetch > 0) {
                ((AudioValueRange *) outData)[0].mMinimum = 44100.0;
                ((AudioValueRange *) outData)[0].mMaximum = 44100.0;
            }
            if (theNumberItemsToFetch > 1) {
                ((AudioValueRange *) outData)[1].mMinimum = 48000.0;
                ((AudioValueRange *) outData)[1].mMaximum = 48000.0;
            }

            // 报告写入了多少数据
            *outDataSize = theNumberItemsToFetch * sizeof(AudioValueRange);
            break;

        case kAudioDevicePropertyIsHidden:
            // 返回设备是否对客户端可见
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyIsHidden 返回值空间不足");
            *((UInt32 *) outData) = 0;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioDevicePropertyPreferredChannelsForStereo:
            // 返回默认的立体声左右声道
            FailWithAction(inDataSize < (2 * sizeof(UInt32)),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyPreferredChannelsForStereo 返回值空间不足");
            ((UInt32 *) outData)[0] = 1;
            ((UInt32 *) outData)[1] = 2;
            *outDataSize = 2 * sizeof(UInt32);
            break;

        case kAudioDevicePropertyPreferredChannelLayout: {
            // 返回设备的默认AudioChannelLayout
            const UInt32 numChannels = 2;  // 明确声明通道数
            UInt32 theACLSize = offsetof(AudioChannelLayout, mChannelDescriptions) +
                                (numChannels * sizeof(AudioChannelDescription));

            // 检查输入缓冲区大小
            FailWithAction(inDataSize < theACLSize,
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyPreferredChannelLayout 返回值空间不足");

            AudioChannelLayout *layout = (AudioChannelLayout *) outData;
            layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            layout->mChannelBitmap = 0;
            layout->mNumberChannelDescriptions = numChannels;

            // 使用明确的边界检查
            for (theItemIndex = 0; theItemIndex < numChannels && theItemIndex < 2; ++theItemIndex) {
                layout->mChannelDescriptions[theItemIndex].mChannelLabel =
                        (theItemIndex == 0) ? kAudioChannelLabel_Left : kAudioChannelLabel_Right;
                layout->mChannelDescriptions[theItemIndex].mChannelFlags = 0;
                layout->mChannelDescriptions[theItemIndex].mCoordinates[0] = 0;
                layout->mChannelDescriptions[theItemIndex].mCoordinates[1] = 0;
                layout->mChannelDescriptions[theItemIndex].mCoordinates[2] = 0;
            }

            *outDataSize = theACLSize;
        }
            break;

        case kAudioDevicePropertyZeroTimeStampPeriod:
            // 此属性返回HAL在零时间戳之间应期待的帧数
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyZeroTimeStampPeriod 返回值空间不足");
            *((UInt32 *) outData) = kDevice_RingBufferSize;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioDevicePropertyIcon: {
            // 这是指向设备图标的 CFURL
            FailWithAction(inDataSize < sizeof(CFURLRef),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 设备的 kAudioDevicePropertyIcon 返回值空间不足");
            CFBundleRef theBundle = CFBundleGetBundleWithIdentifier(CFSTR(kPlugIn_BundleID));
            FailWithAction(theBundle == NULL,
                           theAnswer = kAudioHardwareUnspecifiedError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 无法获取插件包的 kAudioDevicePropertyIcon");
            CFURLRef theURL = CFBundleCopyResourceURL(theBundle, CFSTR("DeviceIcon.icns"), NULL, NULL);
            FailWithAction(theURL == NULL,
                           theAnswer = kAudioHardwareUnspecifiedError,
                           Done,
                           "VirtualAudioDriver_GetDevicePropertyData: 无法获取 kAudioDevicePropertyIcon 的 URL");
            *((CFURLRef *) outData) = theURL;
            *outDataSize = sizeof(CFURLRef);
        }
            break;

        default:
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_SetDevicePropertyData(AudioServerPlugInDriverRef inDriver,
                                                         AudioObjectID inObjectID,
                                                         pid_t inClientProcessID,
                                                         const AudioObjectPropertyAddress *inAddress,
                                                         UInt32 inQualifierDataSize,
                                                         const void *inQualifierData,
                                                         UInt32 inDataSize,
                                                         const void *inData,
                                                         UInt32 *outNumberPropertiesChanged,
                                                         AudioObjectPropertyAddress outChangedAddresses[2]) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

    // 声明局部变量
    OSStatus theAnswer = 0;
    Float64 theOldSampleRate;
    UInt64 theNewSampleRate;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef,
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_SetDevicePropertyData: 错误的驱动引用");
    FailWithAction(inAddress == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetDevicePropertyData: 地址为空");
    FailWithAction(outNumberPropertiesChanged == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetDevicePropertyData: 没有地方返回更改的属性数量");
    FailWithAction(outChangedAddresses == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetDevicePropertyData: 没有地方返回更改的属性");
    FailWithAction(inObjectID != kObjectID_Device,
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_SetDevicePropertyData: 不是设备对象");

    // 初始化返回的更改属性数量
    *outNumberPropertiesChanged = 0;

    // 注意，对于每个对象，此驱动程序实现了所有必需的属性以及一些有用但不是必需的额外属性。
    // 在 VirtualAudioDriver_GetDevicePropertyData() 方法中有关于每个属性的更详细的注释。
    switch (inAddress->mSelector) {
        case kAudioDevicePropertyNominalSampleRate:
            // 更改采样率需要通过 RequestConfigChange/PerformConfigChange 机制处理。

            // 检查参数
            FailWithAction(inDataSize != sizeof(Float64),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_SetDevicePropertyData: kAudioDevicePropertyNominalSampleRate 的数据大小错误");
            FailWithAction((*((const Float64 *) inData) != 44100.0) && (*((const Float64 *) inData) != 48000.0),
                           theAnswer = kAudioHardwareIllegalOperationError,
                           Done,
                           "VirtualAudioDriver_SetDevicePropertyData: 不支持的 kAudioDevicePropertyNominalSampleRate 值");

            // 确保新值与旧值不同
            pthread_mutex_lock(&gPlugIn_StateMutex);
            theOldSampleRate = gDevice_SampleRate;
            pthread_mutex_unlock(&gPlugIn_StateMutex);

            if (*((const Float64 *) inData) != theOldSampleRate) {
                *outNumberPropertiesChanged = 1;
                outChangedAddresses[0].mSelector = kAudioDevicePropertyNominalSampleRate;
                outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
                outChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;

                // 异步调度更改
                theOldSampleRate = *((const Float64 *) inData);
                theNewSampleRate = (UInt64) theOldSampleRate;
                dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                    gPlugIn_Host->RequestDeviceConfigurationChange(gPlugIn_Host, kObjectID_Device, theNewSampleRate,
                                                                   NULL);
                });
            }
            break;

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

static OSStatus
VirtualAudioDriver_GetStreamPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                             pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                             UInt32 inQualifierDataSize, const void *inQualifierData,
                                             UInt32 *outDataSize) {
    // 此方法返回流对象上的属性数据大小（以字节为单位）

#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetStreamPropertyDataSize: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetStreamPropertyDataSize: 地址为空");
    FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetStreamPropertyDataSize: 没有地方存放返回值");
    FailWithAction((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output),
                   theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetStreamPropertyDataSize: 不是流对象");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    // 在 VirtualAudioDriver_GetStreamPropertyData() 方法中有关于每个属性的更详细说明。
    switch (inAddress->mSelector) {
        // 基本属性
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID);
            break;

        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = 0 * sizeof(AudioObjectID);
            break;

            // 字符串属性
        case kAudioObjectPropertyName:
            *outDataSize = sizeof(CFStringRef);
            break;

            // 状态属性
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
            *outDataSize = sizeof(UInt32);
            break;

            // 格式属性
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outDataSize = sizeof(AudioStreamBasicDescription);
            break;

            // 可用格式属性
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = 2 * sizeof(AudioStreamRangedDescription);
            break;

        default:
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_GetStreamPropertyData(AudioServerPlugInDriverRef inDriver,
                                                         AudioObjectID inObjectID,
                                                         pid_t inClientProcessID,
                                                         const AudioObjectPropertyAddress *inAddress,
                                                         UInt32 inQualifierDataSize,
                                                         const void *inQualifierData,
                                                         UInt32 inDataSize,
                                                         UInt32 *outDataSize,
                                                         void *outData) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

    // 声明局部变量
    OSStatus theAnswer = 0;
    UInt32 theNumberItemsToFetch;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef,
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_GetStreamPropertyData: 错误的驱动引用");
    FailWithAction(inAddress == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_GetStreamPropertyData: 地址为空");
    FailWithAction(outDataSize == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_GetStreamPropertyData: 没有地方存放返回值大小");
    FailWithAction(outData == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_GetStreamPropertyData: 没有地方存放返回值");
    FailWithAction((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output),
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_GetStreamPropertyData: 不是流对象");

    // 注意，对于每个对象，此驱动程序实现了所有必需的属性以及一些有用但不是必需的额外属性。
    // 此外，由于大多数将要返回的数据是静态的，很少需要锁定状态互斥锁。

    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            // kAudioStreamClassID 的基类是 kAudioObjectClassID
            FailWithAction(inDataSize < sizeof(AudioClassID),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetStreamPropertyData: 流的 kAudioObjectPropertyBaseClass 返回值空间不足");
            *((AudioClassID *) outData) = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioObjectPropertyClass:
            // 流创建者的类始终是 kAudioStreamClassID
            FailWithAction(inDataSize < sizeof(AudioClassID),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetStreamPropertyData: 流的 kAudioObjectPropertyClass 返回值空间不足");
            *((AudioClassID *) outData) = kAudioStreamClassID;
            *outDataSize = sizeof(AudioClassID);
            break;

        case kAudioObjectPropertyOwner:
            // 流的所有者是设备对象
            FailWithAction(inDataSize < sizeof(AudioObjectID),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetStreamPropertyData: 流的 kAudioObjectPropertyOwner 返回值空间不足");
            *((AudioObjectID *) outData) = kObjectID_Device;
            *outDataSize = sizeof(AudioObjectID);
            break;

        case kAudioObjectPropertyOwnedObjects:
            // 流不拥有任何对象
            *outDataSize = 0 * sizeof(AudioObjectID);
            break;

        case kAudioObjectPropertyName:
            // 这是流的人类可读名称
            FailWithAction(inDataSize < sizeof(CFStringRef),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetStreamPropertyData: 流的 kAudioObjectPropertyName 返回值空间不足");
            *((CFStringRef *) outData) = (inObjectID == kObjectID_Stream_Input) ? CFSTR("输入流名称") : CFSTR(
                    "输出流名称");
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioStreamPropertyIsActive:
            // 此属性告诉设备给定流是否用于IO。注意需要锁定状态以检查此值。
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetStreamPropertyData: 流的 kAudioStreamPropertyIsActive 返回值空间不足");
            pthread_mutex_lock(&gPlugIn_StateMutex);
            *((UInt32 *) outData) = (inObjectID == kObjectID_Stream_Input) ? gStream_Input_IsActive
                                                                           : gStream_Output_IsActive;
            pthread_mutex_unlock(&gPlugIn_StateMutex);
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioStreamPropertyDirection:
            // 返回流是输入流还是输出流。
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetStreamPropertyData: 流的 kAudioStreamPropertyDirection 返回值空间不足");
            *((UInt32 *) outData) = (inObjectID == kObjectID_Stream_Input) ? 1 : 0;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioStreamPropertyTerminalType:
            // 返回流的另一端的类型，例如扬声器或麦克风。此属性的值在 <CoreAudio/AudioHardwareBase.h> 中定义。
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetStreamPropertyData: 流的 kAudioStreamPropertyTerminalType 返回值空间不足");
            *((UInt32 *) outData) = (inObjectID == kObjectID_Stream_Input) ? kAudioStreamTerminalTypeMicrophone
                                                                           : kAudioStreamTerminalTypeSpeaker;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioStreamPropertyStartingChannel:
            // 返回流的第一个通道的绝对通道号。
            // 例如，如果设备有两个输出流，每个流有两个通道，那么第一个流的起始通道号为1，第二个流的起始通道号为3。
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetStreamPropertyData: 流的 kAudioStreamPropertyStartingChannel 返回值空间不足");
            *((UInt32 *) outData) = 1;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioStreamPropertyLatency:
            // 返回流的额外呈现延迟。
            FailWithAction(inDataSize < sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetStreamPropertyData: 流的 kAudioStreamPropertyLatency 返回值空间不足");
            *((UInt32 *) outData) = 0;
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            // 返回流的当前格式（使用 AudioStreamBasicDescription）。
            // 注意需要持有状态锁以获取此值。
            // 对于不覆盖混合操作的设备，虚拟格式必须与物理格式相同。
            FailWithAction(inDataSize < sizeof(AudioStreamBasicDescription),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_GetStreamPropertyData: 流的 kAudioStreamPropertyVirtualFormat 返回值空间不足");
            pthread_mutex_lock(&gPlugIn_StateMutex);
            ((AudioStreamBasicDescription *) outData)->mSampleRate = gDevice_SampleRate;
            ((AudioStreamBasicDescription *) outData)->mFormatID = kAudioFormatLinearPCM;
            ((AudioStreamBasicDescription *) outData)->mFormatFlags =
                    kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
            ((AudioStreamBasicDescription *) outData)->mBytesPerPacket = 8;
            ((AudioStreamBasicDescription *) outData)->mFramesPerPacket = 1;
            ((AudioStreamBasicDescription *) outData)->mBytesPerFrame = 8;
            ((AudioStreamBasicDescription *) outData)->mChannelsPerFrame = 2;
            ((AudioStreamBasicDescription *) outData)->mBitsPerChannel = 32;
            pthread_mutex_unlock(&gPlugIn_StateMutex);
            *outDataSize = sizeof(AudioStreamBasicDescription);
            break;

        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            // 返回一个描述支持的格式的 AudioStreamRangedDescriptions 数组。
            // 计算请求的项目数量。注意，该数量可以小于实际列表的大小，在这种情况下，只返回该数量的项目。
            theNumberItemsToFetch = inDataSize / sizeof(AudioStreamRangedDescription);

            // 将其限制为我们拥有的项目数量。
            if (theNumberItemsToFetch > 2) {
                theNumberItemsToFetch = 2;
            }

            // 填充返回数组。
            if (theNumberItemsToFetch > 0) {
                ((AudioStreamRangedDescription *) outData)[0].mFormat.mSampleRate = 44100.0;
                ((AudioStreamRangedDescription *) outData)[0].mFormat.mFormatID = kAudioFormatLinearPCM;
                ((AudioStreamRangedDescription *) outData)[0].mFormat.mFormatFlags =
                        kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
                ((AudioStreamRangedDescription *) outData)[0].mFormat.mBytesPerPacket = 8;
                ((AudioStreamRangedDescription *) outData)[0].mFormat.mFramesPerPacket = 1;
                ((AudioStreamRangedDescription *) outData)[0].mFormat.mBytesPerFrame = 8;
                ((AudioStreamRangedDescription *) outData)[0].mFormat.mChannelsPerFrame = 2;
                ((AudioStreamRangedDescription *) outData)[0].mFormat.mBitsPerChannel = 32;
                ((AudioStreamRangedDescription *) outData)[0].mSampleRateRange.mMinimum = 44100.0;
                ((AudioStreamRangedDescription *) outData)[0].mSampleRateRange.mMaximum = 44100.0;
            }
            if (theNumberItemsToFetch > 1) {
                ((AudioStreamRangedDescription *) outData)[1].mFormat.mSampleRate = 48000.0;
                ((AudioStreamRangedDescription *) outData)[1].mFormat.mFormatID = kAudioFormatLinearPCM;
                ((AudioStreamRangedDescription *) outData)[1].mFormat.mFormatFlags =
                        kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
                ((AudioStreamRangedDescription *) outData)[1].mFormat.mBytesPerPacket = 8;
                ((AudioStreamRangedDescription *) outData)[1].mFormat.mFramesPerPacket = 1;
                ((AudioStreamRangedDescription *) outData)[1].mFormat.mBytesPerFrame = 8;
                ((AudioStreamRangedDescription *) outData)[1].mFormat.mChannelsPerFrame = 2;
                ((AudioStreamRangedDescription *) outData)[1].mFormat.mBitsPerChannel = 32;
                ((AudioStreamRangedDescription *) outData)[1].mSampleRateRange.mMinimum = 48000.0;
                ((AudioStreamRangedDescription *) outData)[1].mSampleRateRange.mMaximum = 48000.0;
            }

            // 报告写入了多少数据。
            *outDataSize = theNumberItemsToFetch * sizeof(AudioStreamRangedDescription);
            break;

        default:
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    };

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_SetStreamPropertyData(AudioServerPlugInDriverRef inDriver,
                                                         AudioObjectID inObjectID,
                                                         pid_t inClientProcessID,
                                                         const AudioObjectPropertyAddress *inAddress,
                                                         UInt32 inQualifierDataSize,
                                                         const void *inQualifierData,
                                                         UInt32 inDataSize,
                                                         const void *inData,
                                                         UInt32 *outNumberPropertiesChanged,
                                                         AudioObjectPropertyAddress outChangedAddresses[2]) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

    // 声明局部变量
    OSStatus theAnswer = 0;
    Float64 theOldSampleRate;
    UInt64 theNewSampleRate;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef,
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_SetStreamPropertyData: 错误的驱动引用");
    FailWithAction(inAddress == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetStreamPropertyData: 地址为空");
    FailWithAction(outNumberPropertiesChanged == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetStreamPropertyData: 没有地方返回更改的属性数量");
    FailWithAction(outChangedAddresses == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetStreamPropertyData: 没有地方返回更改的属性");
    FailWithAction((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output),
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_SetStreamPropertyData: 不是流对象");

    // 初始化返回的更改属性数量
    *outNumberPropertiesChanged = 0;

    // 注意，对于每个对象，此驱动程序实现了所有必需的属性以及一些有用但不是必需的额外属性。
    // 在 VirtualAudioDriver_GetStreamPropertyData() 方法中有关于每个属性的更详细的注释。
    switch (inAddress->mSelector) {
        case kAudioStreamPropertyIsActive:
            // 更改流的活动状态不会影响IO或更改结构，因此我们可以只保存状态并发送通知。
            FailWithAction(inDataSize != sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_SetStreamPropertyData: kAudioStreamPropertyIsActive 的数据大小错误");

            pthread_mutex_lock(&gPlugIn_StateMutex);
            if (inObjectID == kObjectID_Stream_Input) {
                if (gStream_Input_IsActive != (*((const UInt32 *) inData) != 0)) {
                    gStream_Input_IsActive = *((const UInt32 *) inData) != 0;
                    *outNumberPropertiesChanged = 1;
                    outChangedAddresses[0].mSelector = kAudioStreamPropertyIsActive;
                    outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
                    outChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;
                }
            } else {
                if (gStream_Output_IsActive != (*((const UInt32 *) inData) != 0)) {
                    gStream_Output_IsActive = *((const UInt32 *) inData) != 0;
                    *outNumberPropertiesChanged = 1;
                    outChangedAddresses[0].mSelector = kAudioStreamPropertyIsActive;
                    outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
                    outChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;
                }
            }
            pthread_mutex_unlock(&gPlugIn_StateMutex);
            break;

        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            // 更改流格式需要通过 RequestConfigChange/PerformConfigChange 机制处理。
            // 请注意，由于此设备仅支持2通道32位浮点数据，因此唯一可以更改的是采样率。
            FailWithAction(inDataSize != sizeof(AudioStreamBasicDescription),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_SetStreamPropertyData: kAudioStreamPropertyPhysicalFormat 的数据大小错误");
            FailWithAction(((const AudioStreamBasicDescription *) inData)->mFormatID != kAudioFormatLinearPCM,
                           theAnswer = kAudioDeviceUnsupportedFormatError,
                           Done,
                           "VirtualAudioDriver_SetStreamPropertyData: kAudioStreamPropertyPhysicalFormat 的格式ID不支持");
            FailWithAction(((const AudioStreamBasicDescription *) inData)->mFormatFlags !=
                           (kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked),
                           theAnswer = kAudioDeviceUnsupportedFormatError,
                           Done,
                           "VirtualAudioDriver_SetStreamPropertyData: kAudioStreamPropertyPhysicalFormat 的格式标志不支持");
            FailWithAction(((const AudioStreamBasicDescription *) inData)->mBytesPerPacket != 8,
                           theAnswer = kAudioDeviceUnsupportedFormatError,
                           Done,
                           "VirtualAudioDriver_SetStreamPropertyData: kAudioStreamPropertyPhysicalFormat 的每个包字节数不支持");
            FailWithAction(((const AudioStreamBasicDescription *) inData)->mFramesPerPacket != 1,
                           theAnswer = kAudioDeviceUnsupportedFormatError,
                           Done,
                           "VirtualAudioDriver_SetStreamPropertyData: kAudioStreamPropertyPhysicalFormat 的每个包帧数不支持");
            FailWithAction(((const AudioStreamBasicDescription *) inData)->mBytesPerFrame != 8,
                           theAnswer = kAudioDeviceUnsupportedFormatError,
                           Done,
                           "VirtualAudioDriver_SetStreamPropertyData: kAudioStreamPropertyPhysicalFormat 的每帧字节数不支持");
            FailWithAction(((const AudioStreamBasicDescription *) inData)->mChannelsPerFrame != 2,
                           theAnswer = kAudioDeviceUnsupportedFormatError,
                           Done,
                           "VirtualAudioDriver_SetStreamPropertyData: kAudioStreamPropertyPhysicalFormat 的每帧通道数不支持");
            FailWithAction(((const AudioStreamBasicDescription *) inData)->mBitsPerChannel != 32,
                           theAnswer = kAudioDeviceUnsupportedFormatError,
                           Done,
                           "VirtualAudioDriver_SetStreamPropertyData: kAudioStreamPropertyPhysicalFormat 的每通道位数不支持");
            FailWithAction((((const AudioStreamBasicDescription *) inData)->mSampleRate != 44100.0) &&
                           (((const AudioStreamBasicDescription *) inData)->mSampleRate != 48000.0),
                           theAnswer = kAudioHardwareIllegalOperationError,
                           Done,
                           "VirtualAudioDriver_SetStreamPropertyData: kAudioStreamPropertyPhysicalFormat 的采样率不支持");

            // 如果我们能走到这一步，说明请求的格式是我们支持的，所以确保采样率实际上是不同的
            pthread_mutex_lock(&gPlugIn_StateMutex);
            theOldSampleRate = gDevice_SampleRate;
            pthread_mutex_unlock(&gPlugIn_StateMutex);

            if (((const AudioStreamBasicDescription *) inData)->mSampleRate != theOldSampleRate) {
                // 异步调度更改
                theOldSampleRate = ((const AudioStreamBasicDescription *) inData)->mSampleRate;
                theNewSampleRate = (UInt64) theOldSampleRate;
                dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                    gPlugIn_Host->RequestDeviceConfigurationChange(gPlugIn_Host, kObjectID_Device, theNewSampleRate,
                                                                   NULL);
                });
            }
            break;

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

static OSStatus
VirtualAudioDriver_GetControlPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                              pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                              UInt32 inQualifierDataSize, const void *inQualifierData,
                                              UInt32 *outDataSize) {
    // 此方法返回控制对象上的属性数据大小（以字节为单位）

#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

    // 声明局部变量
    OSStatus theAnswer = 0;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done,
                   "VirtualAudioDriver_GetControlPropertyDataSize: 无效的驱动程序引用");
    FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetControlPropertyDataSize: 地址为空");
    FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done,
                   "VirtualAudioDriver_GetControlPropertyDataSize: 没有地方存放返回值");

    // 注意：对于每个对象，此驱动程序实现了所有必需的属性，
    // 以及一些有用但非必需的额外属性。
    switch (inObjectID) {
        // 音量控制
        case kObjectID_Volume_Input_Master:
        case kObjectID_Volume_Output_Master:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    break;

                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    break;

                case kAudioObjectPropertyOwnedObjects:
                    *outDataSize = 0;
                    break;

                case kAudioControlPropertyScope:
                    *outDataSize = sizeof(AudioObjectPropertyScope);
                    break;

                case kAudioControlPropertyElement:
                    *outDataSize = sizeof(AudioObjectPropertyElement);
                    break;

                case kAudioLevelControlPropertyScalarValue:
                case kAudioLevelControlPropertyDecibelValue:
                case kAudioLevelControlPropertyConvertScalarToDecibels:
                case kAudioLevelControlPropertyConvertDecibelsToScalar:
                    *outDataSize = sizeof(Float32);
                    break;

                case kAudioLevelControlPropertyDecibelRange:
                    *outDataSize = sizeof(AudioValueRange);
                    break;

                default:
                    theAnswer = kAudioHardwareUnknownPropertyError;
                    break;
            }
            break;

            // 静音控制
        case kObjectID_Mute_Input_Master:
        case kObjectID_Mute_Output_Master:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    break;

                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    break;

                case kAudioObjectPropertyOwnedObjects:
                    *outDataSize = 0;
                    break;

                case kAudioControlPropertyScope:
                    *outDataSize = sizeof(AudioObjectPropertyScope);
                    break;

                case kAudioControlPropertyElement:
                    *outDataSize = sizeof(AudioObjectPropertyElement);
                    break;

                case kAudioBooleanControlPropertyValue:
                    *outDataSize = sizeof(UInt32);
                    break;

                default:
                    theAnswer = kAudioHardwareUnknownPropertyError;
                    break;
            }
            break;

            // 数据源控制
        case kObjectID_DataSource_Input_Master:
        case kObjectID_DataSource_Output_Master:
        case kObjectID_DataDestination_PlayThru_Master:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID);
                    break;

                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID);
                    break;

                case kAudioObjectPropertyOwnedObjects:
                    *outDataSize = 0;
                    break;

                case kAudioControlPropertyScope:
                    *outDataSize = sizeof(AudioObjectPropertyScope);
                    break;

                case kAudioControlPropertyElement:
                    *outDataSize = sizeof(AudioObjectPropertyElement);
                    break;

                case kAudioSelectorControlPropertyCurrentItem:
                    *outDataSize = sizeof(UInt32);
                    break;

                case kAudioSelectorControlPropertyAvailableItems:
                    *outDataSize = kDataSource_NumberItems * sizeof(UInt32);
                    break;

                case kAudioSelectorControlPropertyItemName:
                    *outDataSize = sizeof(CFStringRef);
                    break;

                default:
                    theAnswer = kAudioHardwareUnknownPropertyError;
                    break;
            }
            break;

        default:
            theAnswer = kAudioHardwareBadObjectError;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_GetControlPropertyData(AudioServerPlugInDriverRef inDriver,
                                                          AudioObjectID inObjectID,
                                                          pid_t inClientProcessID,
                                                          const AudioObjectPropertyAddress *inAddress,
                                                          UInt32 inQualifierDataSize,
                                                          const void *inQualifierData,
                                                          UInt32 inDataSize,
                                                          UInt32 *outDataSize,
                                                          void *outData) {
#pragma unused(inClientProcessID)

    // 声明局部变量
    OSStatus theAnswer = 0;
    UInt32 theNumberItemsToFetch;
    UInt32 theItemIndex;

    // 检查参数
    FailWithAction(inDriver != gAudioServerPlugInDriverRef,
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_GetControlPropertyData: 错误的驱动引用");
    FailWithAction(inAddress == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_GetControlPropertyData: 地址为空");
    FailWithAction(outDataSize == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_GetControlPropertyData: 没有地方存放返回值大小");
    FailWithAction(outData == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_GetControlPropertyData: 没有地方存放返回值");

    // 注意，对于每个对象，此驱动程序实现了所有必需的属性以及一些有用但不是必需的额外属性。
    //
    // 此外，由于大多数将要返回的数据是静态的，很少需要锁定状态互斥锁。

    switch (inObjectID) {
        case kObjectID_Volume_Input_Master:
        case kObjectID_Volume_Output_Master:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    // kAudioVolumeControlClassID 的基类是 kAudioLevelControlClassID
                    FailWithAction(inDataSize < sizeof(AudioClassID),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 音量控制的 kAudioObjectPropertyBaseClass 返回值空间不足");
                    *((AudioClassID *) outData) = kAudioLevelControlClassID;
                    *outDataSize = sizeof(AudioClassID);
                    break;

                case kAudioObjectPropertyClass:
                    // 音量控制属于 kAudioVolumeControlClassID 类
                    FailWithAction(inDataSize < sizeof(AudioClassID),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 音量控制的 kAudioObjectPropertyClass 返回值空间不足");
                    *((AudioClassID *) outData) = kAudioVolumeControlClassID;
                    *outDataSize = sizeof(AudioClassID);
                    break;

                case kAudioObjectPropertyOwner:
                    // 控制器的所有者是设备对象
                    FailWithAction(inDataSize < sizeof(AudioObjectID),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 音量控制的 kAudioObjectPropertyOwner 返回值空间不足");
                    *((AudioObjectID *) outData) = kObjectID_Device;
                    *outDataSize = sizeof(AudioObjectID);
                    break;

                case kAudioObjectPropertyOwnedObjects:
                    // 控制器不拥有任何对象
                    *outDataSize = 0 * sizeof(AudioObjectID);
                    break;

                case kAudioControlPropertyScope:
                    // 此属性返回控制器所附加的范围
                    FailWithAction(inDataSize < sizeof(AudioObjectPropertyScope),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 音量控制的 kAudioControlPropertyScope 返回值空间不足");
                    *((AudioObjectPropertyScope *) outData) = (inObjectID == kObjectID_Volume_Input_Master) ?
                                                              kAudioObjectPropertyScopeInput :
                                                              kAudioObjectPropertyScopeOutput;
                    *outDataSize = sizeof(AudioObjectPropertyScope);
                    break;

                case kAudioControlPropertyElement:
                    // 此属性返回控制器所附加的元素
                    FailWithAction(inDataSize < sizeof(AudioObjectPropertyElement),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 音量控制的 kAudioControlPropertyElement 返回值空间不足");
                    *((AudioObjectPropertyElement *) outData) = kAudioObjectPropertyElementMain;
                    *outDataSize = sizeof(AudioObjectPropertyElement);
                    break;

                case kAudioLevelControlPropertyScalarValue:
                    // 返回控制器在 0 到 1 的标准化范围内的值
                    // 注意需要获取状态锁来检查此值
                    FailWithAction(inDataSize < sizeof(Float32),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 音量控制的 kAudioLevelControlPropertyScalarValue 返回值空间不足");
                    pthread_mutex_lock(&gPlugIn_StateMutex);
                    *((Float32 *) outData) = (inObjectID == kObjectID_Volume_Input_Master) ?
                                             gVolume_Input_Master_Value :
                                             gVolume_Output_Master_Value;
                    pthread_mutex_unlock(&gPlugIn_StateMutex);
                    *outDataSize = sizeof(Float32);
                    break;

                case kAudioLevelControlPropertyDecibelValue:
                    // 返回控制器的分贝值
                    // 注意需要获取状态锁来检查此值
                    FailWithAction(inDataSize < sizeof(Float32),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 音量控制的 kAudioLevelControlPropertyDecibelValue 返回值空间不足");
                    pthread_mutex_lock(&gPlugIn_StateMutex);
                    *((Float32 *) outData) = (inObjectID == kObjectID_Volume_Input_Master) ?
                                             gVolume_Input_Master_Value :
                                             gVolume_Output_Master_Value;
                    pthread_mutex_unlock(&gPlugIn_StateMutex);

                    // 注意在转换为分贝之前我们对标量值进行平方运算，以便为滑块提供更好的曲线
                    *((Float32 *) outData) *= *((Float32 *) outData);
                    *((Float32 *) outData) = kVolume_MinDB + (*((Float32 *) outData) * (kVolume_MaxDB - kVolume_MinDB));

                    // 报告写入了多少数据
                    *outDataSize = sizeof(Float32);
                    break;

                case kAudioLevelControlPropertyDecibelRange:
                    // 返回控制器的分贝范围
                    FailWithAction(inDataSize < sizeof(AudioValueRange),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 音量控制的 kAudioLevelControlPropertyDecibelRange 返回值空间不足");
                    ((AudioValueRange *) outData)->mMinimum = kVolume_MinDB;
                    ((AudioValueRange *) outData)->mMaximum = kVolume_MaxDB;
                    *outDataSize = sizeof(AudioValueRange);
                    break;

                case kAudioLevelControlPropertyConvertScalarToDecibels:
                    // 将 outData 中的标量值转换为分贝
                    FailWithAction(inDataSize < sizeof(Float32),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 音量控制的 kAudioLevelControlPropertyDecibelValue 返回值空间不足");

                    // 将值限制在 0 和 1 之间
                    if (*((Float32 *) outData) < 0.0) {
                        *((Float32 *) outData) = 0;
                    }
                    if (*((Float32 *) outData) > 1.0) {
                        *((Float32 *) outData) = 1.0f;
                    }

                    // 注意在转换为分贝之前我们对标量值进行平方运算，以便为滑块提供更好的曲线
                    *((Float32 *) outData) *= *((Float32 *) outData);
                    *((Float32 *) outData) = kVolume_MinDB + (*((Float32 *) outData) * (kVolume_MaxDB - kVolume_MinDB));

                    // 报告写入了多少数据
                    *outDataSize = sizeof(Float32);
                    break;

                case kAudioLevelControlPropertyConvertDecibelsToScalar:
                    // 将 outData 中的分贝值转换为标量
                    FailWithAction(inDataSize < sizeof(Float32),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 音量控制的 kAudioLevelControlPropertyDecibelValue 返回值空间不足");

                    // 将值限制在 kVolume_MinDB 和 kVolume_MaxDB 之间
                    if (*((Float32 *) outData) < kVolume_MinDB) {
                        *((Float32 *) outData) = kVolume_MinDB;
                    }
                    if (*((Float32 *) outData) > kVolume_MaxDB) {
                        *((Float32 *) outData) = kVolume_MaxDB;
                    }

                    // 注意在转换为分贝之前我们对标量值进行平方运算，以便为滑块提供更好的曲线。这里我们撤销该操作。
                    *((Float32 *) outData) = *((Float32 *) outData) - kVolume_MinDB;
                    *((Float32 *) outData) /= kVolume_MaxDB - kVolume_MinDB;
                    *((Float32 *) outData) = sqrtf(*((Float32 *) outData));

                    // 报告写入了多少数据
                    *outDataSize = sizeof(Float32);
                    break;

                default:
                    theAnswer = kAudioHardwareUnknownPropertyError;
                    break;
            }
            break;

        case kObjectID_Mute_Input_Master:
        case kObjectID_Mute_Output_Master:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    // kAudioMuteControlClassID 的基类是 kAudioBooleanControlClassID
                    FailWithAction(inDataSize < sizeof(AudioClassID),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 静音控制的 kAudioObjectPropertyBaseClass 返回值空间不足");
                    *((AudioClassID *) outData) = kAudioBooleanControlClassID;
                    *outDataSize = sizeof(AudioClassID);
                    break;

                case kAudioObjectPropertyClass:
                    // 静音控制属于 kAudioMuteControlClassID 类
                    FailWithAction(inDataSize < sizeof(AudioClassID),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 静音控制的 kAudioObjectPropertyClass 返回值空间不足");
                    *((AudioClassID *) outData) = kAudioMuteControlClassID;
                    *outDataSize = sizeof(AudioClassID);
                    break;

                case kAudioObjectPropertyOwner:
                    // 控制器的所有者是设备对象
                    FailWithAction(inDataSize < sizeof(AudioObjectID),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 静音控制的 kAudioObjectPropertyOwner 返回值空间不足");
                    *((AudioObjectID *) outData) = kObjectID_Device;
                    *outDataSize = sizeof(AudioObjectID);
                    break;

                case kAudioObjectPropertyOwnedObjects:
                    // 控制器不拥有任何对象
                    *outDataSize = 0 * sizeof(AudioObjectID);
                    break;

                case kAudioControlPropertyScope:
                    // 此属性返回控制器所附加的范围
                    FailWithAction(inDataSize < sizeof(AudioObjectPropertyScope),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 静音控制的 kAudioControlPropertyScope 返回值空间不足");
                    *((AudioObjectPropertyScope *) outData) = (inObjectID == kObjectID_Mute_Input_Master) ?
                                                              kAudioObjectPropertyScopeInput :
                                                              kAudioObjectPropertyScopeOutput;
                    *outDataSize = sizeof(AudioObjectPropertyScope);
                    break;

                case kAudioControlPropertyElement:
                    // 此属性返回控制器所附加的元素
                    FailWithAction(inDataSize < sizeof(AudioObjectPropertyElement),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 静音控制的 kAudioControlPropertyElement 返回值空间不足");
                    *((AudioObjectPropertyElement *) outData) = kAudioObjectPropertyElementMain;
                    *outDataSize = sizeof(AudioObjectPropertyElement);
                    break;

                case kAudioBooleanControlPropertyValue:
                    // 返回静音控制的值，其中 0 表示静音关闭且可以听到音频，1 表示静音打开且无法听到音频。
                    // 注意需要获取状态锁来检查此值。
                    FailWithAction(inDataSize < sizeof(UInt32),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 静音控制的 kAudioBooleanControlPropertyValue 返回值空间不足");

                    pthread_mutex_lock(&gPlugIn_StateMutex);

                    UInt32 muteValue;
                    if (inObjectID == kObjectID_Mute_Input_Master) {
                        muteValue = gMute_Input_Master_Value ? 1 : 0;
                    } else {
                        muteValue = gMute_Output_Master_Value ? 1 : 0;
                    }
                    *((UInt32 *) outData) = muteValue;

                    pthread_mutex_unlock(&gPlugIn_StateMutex);

                    *outDataSize = sizeof(UInt32);
                    break;

                default:
                    theAnswer = kAudioHardwareUnknownPropertyError;
                    break;
            }
            break;

        case kObjectID_DataSource_Input_Master:
        case kObjectID_DataSource_Output_Master:
        case kObjectID_DataDestination_PlayThru_Master:
            switch (inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    // kAudioDataSourceControlClassID 的基类是 kAudioSelectorControlClassID
                    FailWithAction(inDataSize < sizeof(AudioClassID),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 数据源控制的 kAudioObjectPropertyBaseClass 返回值空间不足");
                    *((AudioClassID *) outData) = kAudioSelectorControlClassID;
                    *outDataSize = sizeof(AudioClassID);
                    break;

                case kAudioObjectPropertyClass:
                    // 数据源控制属于 kAudioDataSourceControlClassID 类
                    FailWithAction(inDataSize < sizeof(AudioClassID),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 数据源控制的 kAudioObjectPropertyClass 返回值空间不足");
                    *((AudioClassID *) outData) = kAudioDataSourceControlClassID;
                    switch (inObjectID) {
                        case kObjectID_DataSource_Input_Master:
                        case kObjectID_DataSource_Output_Master:
                            *((AudioClassID *) outData) = kAudioDataSourceControlClassID;
                            break;
                        case kObjectID_DataDestination_PlayThru_Master:
                            *((AudioClassID *) outData) = kAudioDataDestinationControlClassID;
                            break;
                    }
                    *outDataSize = sizeof(AudioClassID);
                    break;

                case kAudioObjectPropertyOwner:
                    // 控制器的所有者是设备对象
                    FailWithAction(inDataSize < sizeof(AudioObjectID),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 数据源控制的 kAudioObjectPropertyOwner 返回值空间不足");
                    *((AudioObjectID *) outData) = kObjectID_Device;
                    *outDataSize = sizeof(AudioObjectID);
                    break;

                case kAudioObjectPropertyOwnedObjects:
                    // 控制器不拥有任何对象
                    *outDataSize = 0 * sizeof(AudioObjectID);
                    break;

                case kAudioControlPropertyScope:
                    // 此属性返回控制器所附加的范围
                    FailWithAction(inDataSize < sizeof(AudioObjectPropertyScope),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 数据源控制的 kAudioControlPropertyScope 返回值空间不足");
                    switch (inObjectID) {
                        case kObjectID_DataSource_Input_Master:
                            *((AudioObjectPropertyScope *) outData) = kAudioObjectPropertyScopeInput;
                            break;
                        case kObjectID_DataSource_Output_Master:
                            *((AudioObjectPropertyScope *) outData) = kAudioObjectPropertyScopeOutput;
                            break;
                        case kObjectID_DataDestination_PlayThru_Master:
                            *((AudioObjectPropertyScope *) outData) = kAudioObjectPropertyScopePlayThrough;
                            break;
                    }
                    *outDataSize = sizeof(AudioObjectPropertyScope);
                    break;

                case kAudioControlPropertyElement:
                    // 此属性返回控制器所附加的元素
                    FailWithAction(inDataSize < sizeof(AudioObjectPropertyElement),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 数据源控制的 kAudioControlPropertyElement 返回值空间不足");
                    *((AudioObjectPropertyElement *) outData) = kAudioObjectPropertyElementMain;
                    *outDataSize = sizeof(AudioObjectPropertyElement);
                    break;

                case kAudioSelectorControlPropertyCurrentItem:
                    // 返回数据源选择器的值
                    // 注意需要获取状态锁来检查此值
                    FailWithAction(inDataSize < sizeof(UInt32),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 数据源控制的 kAudioSelectorControlPropertyCurrentItem 返回值空间不足");
                    pthread_mutex_lock(&gPlugIn_StateMutex);
                    switch (inObjectID) {
                        case kObjectID_DataSource_Input_Master:
                            *((UInt32 *) outData) = gDataSource_Input_Master_Value;
                            break;
                        case kObjectID_DataSource_Output_Master:
                            *((UInt32 *) outData) = gDataSource_Output_Master_Value;
                            break;
                        case kObjectID_DataDestination_PlayThru_Master:
                            *((UInt32 *) outData) = gDataDestination_PlayThru_Master_Value;
                            break;
                    }
                    pthread_mutex_unlock(&gPlugIn_StateMutex);
                    *outDataSize = sizeof(UInt32);
                    break;

                case kAudioSelectorControlPropertyAvailableItems:
                    // 返回数据源控制支持的所有项目的 ID
                    // 计算请求的项目数量。注意此数量可以小于实际列表的大小，在这种情况下，只返回该数量的项目
                    theNumberItemsToFetch = inDataSize / sizeof(UInt32);

                    // 将其限制为我们拥有的项目数量
                    if (theNumberItemsToFetch > kDataSource_NumberItems) {
                        theNumberItemsToFetch = kDataSource_NumberItems;
                    }

                    // 填充返回数组
                    theItemIndex = 0; // 确保初始化
                    for (; theItemIndex < theNumberItemsToFetch; ++theItemIndex) {
                        ((UInt32 *) outData)[theItemIndex] = theItemIndex;
                    }

                    // 报告写入了多少数据
                    *outDataSize = theNumberItemsToFetch * sizeof(UInt32);
                    break;

                case kAudioSelectorControlPropertyItemName:
                    // 返回选择器项在限定符中的用户可读名称
                    FailWithAction(inDataSize < sizeof(CFStringRef),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 数据源控制的 kAudioSelectorControlPropertyItemName 返回值空间不足");
                    FailWithAction(inQualifierDataSize != sizeof(UInt32),
                                   theAnswer = kAudioHardwareBadPropertySizeError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 数据源控制的 kAudioSelectorControlPropertyItemName 限定符大小错误");
                    FailWithAction(*((const UInt32 *) inQualifierData) >= kDataSource_NumberItems,
                                   theAnswer = kAudioHardwareIllegalOperationError,
                                   Done,
                                   "VirtualAudioDriver_GetControlPropertyData: 数据源控制的 kAudioSelectorControlPropertyItemName 限定符中的项无效");
                    *((CFStringRef *) outData) = CFStringCreateWithFormat(NULL, NULL,
                                                                          CFSTR(kDataSource_ItemNamePattern),
                                                                          *((const UInt32 *) inQualifierData));
                    *outDataSize = sizeof(CFStringRef);
                    break;

                default:
                    theAnswer = kAudioHardwareUnknownPropertyError;
                    break;
            }
            break;

        default:
            theAnswer = kAudioHardwareBadObjectError;
            break;
    }

    Done:
    return theAnswer;
}

static OSStatus VirtualAudioDriver_SetControlPropertyData(AudioServerPlugInDriverRef inDriver,
                                                          AudioObjectID inObjectID,
                                                          pid_t inClientProcessID,
                                                          const AudioObjectPropertyAddress *inAddress,
                                                          UInt32 inQualifierDataSize,
                                                          const void *inQualifierData,
                                                          UInt32 inDataSize,
                                                          const void *inData,
                                                          UInt32 *outNumberPropertiesChanged,
                                                          AudioObjectPropertyAddress outChangedAddresses[2]) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

    // 声明局部变量，用于存储操作结果和临时数据
    OSStatus theAnswer = 0;
    Float32 theNewVolume;
    // 声明指针用于追踪当前要操作的控制值
    bool *currentMuteValue = NULL;          // 用于追踪静音状态
    Float32 *currentVolumeValue = NULL;        // 用于追踪音量值
    UInt32 *currentDataSourceValue = NULL;     // 用于追踪数据源选择

    // 验证输入参数的有效性
    // 检查驱动引用是否正确
    FailWithAction(inDriver != gAudioServerPlugInDriverRef,
                   theAnswer = kAudioHardwareBadObjectError,
                   Done,
                   "VirtualAudioDriver_SetControlPropertyData: 错误的驱动引用");
    // 检查属性地址是否有效
    FailWithAction(inAddress == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetControlPropertyData: 地址为空");
    // 确保有地方存储更改的属性数量
    FailWithAction(outNumberPropertiesChanged == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetControlPropertyData: 没有地方返回更改的属性数量");
    // 确保有地方存储更改的属性地址
    FailWithAction(outChangedAddresses == NULL,
                   theAnswer = kAudioHardwareIllegalOperationError,
                   Done,
                   "VirtualAudioDriver_SetControlPropertyData: 没有地方返回更改的属性");

    // 初始化返回的更改属性数量为0
    *outNumberPropertiesChanged = 0;

    // 根据对象ID确定要操作的具体控制值
    // 这个switch语句将正确的全局变量地址分配给相应的指针
    switch (inObjectID) {
        case kObjectID_Volume_Input_Master:
            currentVolumeValue = &gVolume_Input_Master_Value;
            break;
        case kObjectID_Volume_Output_Master:
            currentVolumeValue = &gVolume_Output_Master_Value;
            break;
        case kObjectID_Mute_Input_Master:
            currentMuteValue = &gMute_Input_Master_Value;
            break;
        case kObjectID_Mute_Output_Master:
            currentMuteValue = &gMute_Output_Master_Value;
            break;
        case kObjectID_DataSource_Input_Master:
            currentDataSourceValue = &gDataSource_Input_Master_Value;
            break;
        case kObjectID_DataSource_Output_Master:
            currentDataSourceValue = &gDataSource_Output_Master_Value;
            break;
        case kObjectID_DataDestination_PlayThru_Master:
            currentDataSourceValue = &gDataDestination_PlayThru_Master_Value;
            break;
        default:
            // 如果对象ID无效，返回错误
            theAnswer = kAudioHardwareBadObjectError;
            goto Done;
    }

    // 根据属性选择器处理不同类型的控制属性
    switch (inAddress->mSelector) {
        case kAudioLevelControlPropertyScalarValue:
            // 处理标量音量值（范围0.0到1.0）
            // 当此值改变时，对应的dB值也会改变
            FailWithAction(inDataSize != sizeof(Float32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_SetControlPropertyData: kAudioLevelControlPropertyScalarValue 的数据大小错误");

            // 获取并限制新的音量值在有效范围内
            theNewVolume = *((const Float32 *) inData);
            if (theNewVolume < 0.0) theNewVolume = 0.0f;
            if (theNewVolume > 1.0) theNewVolume = 1.0f;

            // 使用互斥锁保护共享资源的访问
            pthread_mutex_lock(&gPlugIn_StateMutex);
            // 仅当值真正改变时才更新
            if (currentVolumeValue && *currentVolumeValue != theNewVolume) {
                *currentVolumeValue = theNewVolume;
                // 标记两个属性发生改变：标量值和dB值
                *outNumberPropertiesChanged = 2;
                // 设置第一个改变的属性（标量值）
                outChangedAddresses[0].mSelector = kAudioLevelControlPropertyScalarValue;
                outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
                outChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;
                // 设置第二个改变的属性（dB值）
                outChangedAddresses[1].mSelector = kAudioLevelControlPropertyDecibelValue;
                outChangedAddresses[1].mScope = kAudioObjectPropertyScopeGlobal;
                outChangedAddresses[1].mElement = kAudioObjectPropertyElementMain;
            }
            pthread_mutex_unlock(&gPlugIn_StateMutex);
            break;

        case kAudioLevelControlPropertyDecibelValue:
            // 处理分贝值，需要先转换为标量值
            // 因为内部是用标量值存储的
            FailWithAction(inDataSize != sizeof(Float32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_SetControlPropertyData: kAudioLevelControlPropertyDecibelValue 的数据大小错误");

            // 获取并限制新的dB值在有效范围内
            theNewVolume = *((const Float32 *) inData);
            if (theNewVolume < kVolume_MinDB) theNewVolume = kVolume_MinDB;
            if (theNewVolume > kVolume_MaxDB) theNewVolume = kVolume_MaxDB;

            // 将dB值转换为标量值，使用平方根提供更好的音量控制曲线
            theNewVolume = (theNewVolume - kVolume_MinDB) / (kVolume_MaxDB - kVolume_MinDB);
            theNewVolume = sqrtf(theNewVolume);

            pthread_mutex_lock(&gPlugIn_StateMutex);
            if (currentVolumeValue && *currentVolumeValue != theNewVolume) {
                *currentVolumeValue = theNewVolume;
                // 同样标记两个属性的改变
                *outNumberPropertiesChanged = 2;
                outChangedAddresses[0].mSelector = kAudioLevelControlPropertyScalarValue;
                outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
                outChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;
                outChangedAddresses[1].mSelector = kAudioLevelControlPropertyDecibelValue;
                outChangedAddresses[1].mScope = kAudioObjectPropertyScopeGlobal;
                outChangedAddresses[1].mElement = kAudioObjectPropertyElementMain;
            }
            pthread_mutex_unlock(&gPlugIn_StateMutex);
            break;

        case kAudioBooleanControlPropertyValue:
            // 处理布尔控制值（如静音状态）
            FailWithAction(inDataSize != sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_SetControlPropertyData: kAudioBooleanControlPropertyValue 的数据大小错误");

            pthread_mutex_lock(&gPlugIn_StateMutex);
            // 检查并更新静音状态
            if (currentMuteValue && *currentMuteValue != (*((const UInt32 *) inData) != 0)) {
                *currentMuteValue = *((const UInt32 *) inData) != 0;
                // 只标记一个属性改变
                *outNumberPropertiesChanged = 1;
                outChangedAddresses[0].mSelector = kAudioBooleanControlPropertyValue;
                outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
                outChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;
            }
            pthread_mutex_unlock(&gPlugIn_StateMutex);
            break;

        case kAudioSelectorControlPropertyCurrentItem:
            // 处理选择器控件的当前项（如输入/输出源选择）
            FailWithAction(inDataSize != sizeof(UInt32),
                           theAnswer = kAudioHardwareBadPropertySizeError,
                           Done,
                           "VirtualAudioDriver_SetControlPropertyData: kAudioSelectorControlPropertyCurrentItem 的数据大小错误");

            // 确保选择的项在有效范围内
            FailWithAction(*((const UInt32 *) inData) >= kDataSource_NumberItems,
                           theAnswer = kAudioHardwareIllegalOperationError,
                           Done,
                           "VirtualAudioDriver_SetControlPropertyData: kAudioSelectorControlPropertyCurrentItem 请求的项目不在可用项目列表中");

            pthread_mutex_lock(&gPlugIn_StateMutex);
            // 检查并更新数据源选择
            if (currentDataSourceValue && *currentDataSourceValue != *((const UInt32 *) inData)) {
                *currentDataSourceValue = *((const UInt32 *) inData);
                *outNumberPropertiesChanged = 1;
                outChangedAddresses[0].mSelector = kAudioSelectorControlPropertyCurrentItem;
                outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
                outChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;
            }
            pthread_mutex_unlock(&gPlugIn_StateMutex);
            break;

        default:
            // 处理未知的属性选择器
            theAnswer = kAudioHardwareUnknownPropertyError;
            break;
    }

    Done:
    // 返回操作结果
    return theAnswer;
}