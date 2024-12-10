//
// Created by AhogeK on 12/10/24.
//

#include "device/virtual_audio_driver.h"
#include <stdatomic.h>
#include <pthread.h>

// 全局变量定义
static pthread_mutex_t gPlugIn_StateMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32 gPlugIn_RefCount = 1;

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