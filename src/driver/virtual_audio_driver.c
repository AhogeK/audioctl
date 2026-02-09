#include "driver/virtual_audio_driver.h"
#include "driver/app_volume_driver.h"
#include <pthread.h>
#include <mach/mach_time.h>

// 只定义输出流，因为这是一个纯输出设备
enum
{
    kObjectID_PlugIn = kAudioObjectPlugInObject,
    kObjectID_Device = 3,
    kObjectID_Stream_Output = 4
};

static pthread_mutex_t gPlugIn_StateMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32 gPlugIn_RefCount = 0;
static AudioServerPlugInHostRef gPlugIn_Host = NULL;
static Float64 gDevice_SampleRate = 48000.0;
static UInt64 gDevice_IOIsRunning = 0;
static const UInt32 kDevice_RingBufferSize = 16384;
static Float64 gDevice_HostTicksPerFrame = 0.0;
static UInt64 gDevice_NumberTimeStamps = 0;
static UInt64 gDevice_AnchorHostTime = 0;

#define RING_BUFFER_FRAMES 16384
static Float32 gRingBuffer[RING_BUFFER_FRAMES * 2];

// Forward Declarations
static HRESULT VirtualAudioDriver_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG VirtualAudioDriver_AddRef(void* inDriver);
static ULONG VirtualAudioDriver_Release(void* inDriver);
static OSStatus VirtualAudioDriver_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus VirtualAudioDriver_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription,
                                                const AudioServerPlugInClientInfo* inClientInfo,
                                                AudioObjectID* outDeviceObjectID);
static OSStatus VirtualAudioDriver_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus VirtualAudioDriver_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                   const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus VirtualAudioDriver_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver,
                                                      AudioObjectID inDeviceObjectID,
                                                      const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus VirtualAudioDriver_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                                    AudioObjectID inDeviceObjectID,
                                                                    UInt64 inChangeAction, void* inChangeInfo);
static OSStatus VirtualAudioDriver_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                                  AudioObjectID inDeviceObjectID, UInt64 inChangeAction,
                                                                  void* inChangeInfo);
static Boolean VirtualAudioDriver_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                              pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus VirtualAudioDriver_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                      pid_t inClientProcessID,
                                                      const AudioObjectPropertyAddress* inAddress,
                                                      Boolean* outIsSettable);
static OSStatus VirtualAudioDriver_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                       pid_t inClientProcessID,
                                                       const AudioObjectPropertyAddress* inAddress,
                                                       UInt32 inQualifierDataSize, const void* inQualifierData,
                                                       UInt32* outDataSize);
static OSStatus VirtualAudioDriver_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                   pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress,
                                                   UInt32 inQualifierDataSize, const void* inQualifierData,
                                                   UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus VirtualAudioDriver_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                   pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress,
                                                   UInt32 inQualifierDataSize, const void* inQualifierData,
                                                   UInt32 inDataSize, const void* inData);
static OSStatus VirtualAudioDriver_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                           UInt32 inClientID);
static OSStatus VirtualAudioDriver_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                          UInt32 inClientID);
static OSStatus VirtualAudioDriver_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                    UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime,
                                                    UInt64* outSeed);
static OSStatus VirtualAudioDriver_WillDoIOOperation(AudioServerPlugInDriverRef inDriver,
                                                     AudioObjectID inDeviceObjectID, UInt32 inClientID,
                                                     UInt32 inOperationID, Boolean* outWillDo,
                                                     Boolean* outWillDoInPlace);
static OSStatus VirtualAudioDriver_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                    UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                                    const AudioServerPlugInIOCycleInfo* inIOCycleInfo);
static OSStatus VirtualAudioDriver_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                 AudioObjectID inStreamObjectID, UInt32 inClientID,
                                                 UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                                 const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer,
                                                 void* ioSecondaryBuffer);
static OSStatus VirtualAudioDriver_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                  UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                                  const AudioServerPlugInIOCycleInfo* inIOCycleInfo);

AudioServerPlugInDriverInterface gAudioServerPlugInDriverInterface = {
    .QueryInterface = VirtualAudioDriver_QueryInterface, .AddRef = VirtualAudioDriver_AddRef,
    .Release = VirtualAudioDriver_Release, .Initialize = VirtualAudioDriver_Initialize,
    .CreateDevice = VirtualAudioDriver_CreateDevice, .DestroyDevice = VirtualAudioDriver_DestroyDevice,
    .AddDeviceClient = VirtualAudioDriver_AddDeviceClient, .RemoveDeviceClient = VirtualAudioDriver_RemoveDeviceClient,
    .PerformDeviceConfigurationChange = VirtualAudioDriver_PerformDeviceConfigurationChange,
    .AbortDeviceConfigurationChange = VirtualAudioDriver_AbortDeviceConfigurationChange,
    .HasProperty = VirtualAudioDriver_HasProperty, .IsPropertySettable = VirtualAudioDriver_IsPropertySettable,
    .GetPropertyDataSize = VirtualAudioDriver_GetPropertyDataSize,
    .GetPropertyData = VirtualAudioDriver_GetPropertyData, .SetPropertyData = VirtualAudioDriver_SetPropertyData,
    .StartIO = VirtualAudioDriver_StartIO, .StopIO = VirtualAudioDriver_StopIO,
    .GetZeroTimeStamp = VirtualAudioDriver_GetZeroTimeStamp, .WillDoIOOperation = VirtualAudioDriver_WillDoIOOperation,
    .BeginIOOperation = VirtualAudioDriver_BeginIOOperation, .DoIOOperation = VirtualAudioDriver_DoIOOperation,
    .EndIOOperation = VirtualAudioDriver_EndIOOperation
};

AudioServerPlugInDriverInterface* gAudioServerPlugInDriverInterfacePtr = &gAudioServerPlugInDriverInterface;
AudioServerPlugInDriverRef gAudioServerPlugInDriverRef = &gAudioServerPlugInDriverInterfacePtr;

void* AudioServerPlugIn_Initialize(CFAllocatorRef __unused inAllocator, CFUUIDRef inRequestedTypeUUID)
{
    return CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID) ? gAudioServerPlugInDriverRef : NULL;
}

static HRESULT VirtualAudioDriver_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
    if (inDriver != gAudioServerPlugInDriverRef || !outInterface) return kAudioHardwareBadObjectError;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    HRESULT res = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID))
    {
        pthread_mutex_lock(&gPlugIn_StateMutex);
        gPlugIn_RefCount++;
        pthread_mutex_unlock(&gPlugIn_StateMutex);
        *outInterface = gAudioServerPlugInDriverRef;
        res = S_OK;
    }
    CFRelease(req);
    return res;
}

static ULONG VirtualAudioDriver_AddRef(void* __unused inDriver)
{
    pthread_mutex_lock(&gPlugIn_StateMutex);
    gPlugIn_RefCount++;
    ULONG res = gPlugIn_RefCount;
    pthread_mutex_unlock(&gPlugIn_StateMutex);
    return res;
}

static ULONG VirtualAudioDriver_Release(void* __unused inDriver)
{
    pthread_mutex_lock(&gPlugIn_StateMutex);
    ULONG res = (gPlugIn_RefCount > 0) ? --gPlugIn_RefCount : 0;
    pthread_mutex_unlock(&gPlugIn_StateMutex);
    return res;
}

static OSStatus VirtualAudioDriver_Initialize(AudioServerPlugInDriverRef __unused inDriver,
                                              AudioServerPlugInHostRef inHost)
{
    gPlugIn_Host = inHost;
    struct mach_timebase_info tb;
    mach_timebase_info(&tb);
    Float64 freq = (Float64)tb.denom / (Float64)tb.numer * 1000000000.0;
    gDevice_HostTicksPerFrame = freq / gDevice_SampleRate;
    app_volume_driver_init();
    return 0;
}

static OSStatus VirtualAudioDriver_StartIO(AudioServerPlugInDriverRef __unused inDriver,
                                           AudioObjectID __unused inDeviceObjectID, UInt32 __unused inClientID)
{
    pthread_mutex_lock(&gPlugIn_StateMutex);
    if (gDevice_IOIsRunning == 0)
    {
        gDevice_NumberTimeStamps = 0;
        gDevice_AnchorHostTime = mach_absolute_time();
        memset(gRingBuffer, 0, sizeof(gRingBuffer));
    }
    gDevice_IOIsRunning++;
    pthread_mutex_unlock(&gPlugIn_StateMutex);
    return 0;
}

static OSStatus VirtualAudioDriver_StopIO(AudioServerPlugInDriverRef __unused inDriver,
                                          AudioObjectID __unused inDeviceObjectID, UInt32 __unused inClientID)
{
    pthread_mutex_lock(&gPlugIn_StateMutex);
    if (gDevice_IOIsRunning > 0) gDevice_IOIsRunning--;
    pthread_mutex_unlock(&gPlugIn_StateMutex);
    return 0;
}

static OSStatus VirtualAudioDriver_GetZeroTimeStamp(AudioServerPlugInDriverRef __unused inDriver,
                                                    AudioObjectID __unused inDeviceObjectID, UInt32 __unused inClientID,
                                                    Float64* outSampleTime, UInt64* outHostTime,
                                                    UInt64* __unused outSeed)
{
    // 使用原子操作代替锁，避免潜在的死锁
    UInt64 now = mach_absolute_time();
    Float64 ticksPerBuf = gDevice_HostTicksPerFrame * (Float64)kDevice_RingBufferSize;

    // 安全地更新计数器，最多处理 1000 次迭代（防止无限循环）
    UInt64 localNumberTimeStamps = gDevice_NumberTimeStamps;
    int maxIterations = 1000;
    while (maxIterations-- > 0 &&
        (Float64)gDevice_AnchorHostTime + (Float64)(localNumberTimeStamps + 1) * ticksPerBuf <= (Float64)now)
    {
        localNumberTimeStamps++;
    }
    gDevice_NumberTimeStamps = localNumberTimeStamps;

    *outSampleTime = (Float64)localNumberTimeStamps * (Float64)kDevice_RingBufferSize;
    *outHostTime = gDevice_AnchorHostTime + (UInt64)((Float64)localNumberTimeStamps * ticksPerBuf);
    *outSeed = 1;
    return 0;
}

static OSStatus VirtualAudioDriver_WillDoIOOperation(AudioServerPlugInDriverRef __unused inDriver,
                                                     AudioObjectID __unused inDeviceObjectID,
                                                     UInt32 __unused inClientID, UInt32 inOperationID,
                                                     Boolean* outWillDo, Boolean* outWillDoInPlace)
{
    // 纯输出设备，只处理写入操作
    bool ok = (inOperationID == kAudioServerPlugInIOOperationWriteMix ||
        inOperationID == kAudioServerPlugInIOOperationProcessOutput ||
        inOperationID == kAudioServerPlugInIOOperationProcessMix);
    if (outWillDo) *outWillDo = ok;
    if (outWillDoInPlace) *outWillDoInPlace = true;
    return 0;
}

static OSStatus VirtualAudioDriver_DoIOOperation(AudioServerPlugInDriverRef __unused inDriver,
                                                 AudioObjectID __unused inDeviceObjectID,
                                                 AudioObjectID __unused inStreamObjectID, UInt32 inClientID,
                                                 UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                                 const AudioServerPlugInIOCycleInfo* __unused inIOCycleInfo,
                                                 void* ioMainBuffer,
                                                 void* __unused ioSecondaryBuffer)
{
    if (!ioMainBuffer || inIOBufferFrameSize == 0) return 0;

    // 只处理输出操作
    if (inOperationID == kAudioServerPlugInIOOperationWriteMix ||
        inOperationID == kAudioServerPlugInIOOperationProcessOutput ||
        inOperationID == kAudioServerPlugInIOOperationProcessMix)
    {
        Float32* buf = (Float32*)ioMainBuffer;
        UInt32 frames = (inIOBufferFrameSize > RING_BUFFER_FRAMES) ? RING_BUFFER_FRAMES : inIOBufferFrameSize;

        // 应用音量控制
        app_volume_driver_apply_volume(inClientID, buf, frames, 2);

        // 保存到环形缓冲区（用于监控/调试，实际输出由系统处理）
        static UInt32 writePos = 0;
        for (UInt32 f = 0; f < frames; f++)
        {
            UInt32 p = ((writePos + f) % RING_BUFFER_FRAMES) * 2;
            gRingBuffer[p] = buf[f * 2];
            gRingBuffer[p + 1] = buf[f * 2 + 1];
        }
        writePos = (writePos + frames) % RING_BUFFER_FRAMES;
    }

    return 0;
}

static Boolean VirtualAudioDriver_HasProperty(AudioServerPlugInDriverRef __unused inDriver, AudioObjectID inObjectID,
                                              pid_t __unused inClientProcessID,
                                              const AudioObjectPropertyAddress* inAddress)
{
    switch (inObjectID)
    {
    case kObjectID_PlugIn: return (inAddress->mSelector == kAudioObjectPropertyBaseClass || inAddress->mSelector ==
            kAudioObjectPropertyClass || inAddress->mSelector == kAudioPlugInPropertyDeviceList);
    case kObjectID_Device:
        // 纯输出设备支持的属性
        return (inAddress->mSelector == kAudioObjectPropertyBaseClass ||
            inAddress->mSelector == kAudioObjectPropertyClass ||
            inAddress->mSelector == kAudioDevicePropertyDeviceUID ||
            inAddress->mSelector == kAudioObjectPropertyName ||
            inAddress->mSelector == kAudioObjectPropertyManufacturer ||
            inAddress->mSelector == kAudioDevicePropertyStreams ||
            inAddress->mSelector == kAudioDevicePropertyNominalSampleRate ||
            inAddress->mSelector == kAudioDevicePropertyIcon ||
            inAddress->mSelector == kAudioDevicePropertyTransportType ||
            inAddress->mSelector == kAudioDevicePropertyDeviceCanBeDefaultDevice ||
            inAddress->mSelector == kAudioDevicePropertyDeviceCanBeDefaultSystemDevice ||
            inAddress->mSelector == kAudioDevicePropertyDeviceIsAlive ||
            inAddress->mSelector == kAudioDevicePropertyDeviceIsRunning);
    case kObjectID_Stream_Output: return true;
    default: break;
    }
    return false;
}

static OSStatus VirtualAudioDriver_IsPropertySettable(AudioServerPlugInDriverRef __unused inDriver,
                                                      AudioObjectID __unused inObjectID,
                                                      pid_t __unused inClientProcessID,
                                                      const AudioObjectPropertyAddress* __unused inAddress,
                                                      Boolean* outIsSettable)
{
    *outIsSettable = false;
    return 0;
}

static OSStatus VirtualAudioDriver_GetPropertyDataSize(AudioServerPlugInDriverRef __unused inDriver,
                                                       AudioObjectID inObjectID, pid_t __unused inClientProcessID,
                                                       const AudioObjectPropertyAddress* inAddress,
                                                       UInt32 __unused inQualifierDataSize,
                                                       const void* __unused inQualifierData, UInt32* outDataSize)
{
    if ((inObjectID == kObjectID_Device && inAddress->mSelector == kAudioDevicePropertyStreams) ||
        (inObjectID == kObjectID_PlugIn && inAddress->mSelector == kAudioPlugInPropertyDeviceList))
    {
        *outDataSize = sizeof(AudioObjectID); // 只有一个输出流或设备列表
    }
    else if (inAddress->mSelector == kAudioDevicePropertyDeviceUID ||
        inAddress->mSelector == kAudioObjectPropertyName ||
        inAddress->mSelector == kAudioObjectPropertyManufacturer)
    {
        *outDataSize = sizeof(CFStringRef);
    }
    else if (inObjectID == kObjectID_Device && inAddress->mSelector == kAudioDevicePropertyIcon)
    {
        *outDataSize = sizeof(CFURLRef);
    }
    else
    {
        *outDataSize = sizeof(UInt32);
    }
    return 0;
}

static OSStatus VirtualAudioDriver_GetPropertyData(AudioServerPlugInDriverRef __unused inDriver,
                                                   AudioObjectID inObjectID, pid_t __unused inClientProcessID,
                                                   const AudioObjectPropertyAddress* inAddress,
                                                   UInt32 __unused inQualifierDataSize,
                                                   const void* __unused inQualifierData, UInt32 __unused inDataSize,
                                                   UInt32* outDataSize, void* outData)
{
    if (inObjectID == kObjectID_PlugIn)
    {
        if (inAddress->mSelector == kAudioObjectPropertyBaseClass)
        {
            *((AudioClassID*)outData) = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
        }
        else if (inAddress->mSelector == kAudioObjectPropertyClass)
        {
            *((AudioClassID*)outData) = kAudioPlugInClassID;
            *outDataSize = sizeof(AudioClassID);
        }
        else if (inAddress->mSelector == kAudioPlugInPropertyDeviceList)
        {
            *((AudioObjectID*)outData) = kObjectID_Device;
            *outDataSize = sizeof(AudioObjectID);
        }
    }
    else if (inObjectID == kObjectID_Device)
    {
        switch (inAddress->mSelector)
        {
        case kAudioObjectPropertyBaseClass:
            *((AudioClassID*)outData) = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            break;
        case kAudioObjectPropertyClass:
            *((AudioClassID*)outData) = kAudioDeviceClassID;
            *outDataSize = sizeof(AudioClassID);
            break;
        case kAudioDevicePropertyDeviceUID:
            *((CFStringRef*)outData) = CFSTR(kDevice_UID);
            *outDataSize = sizeof(CFStringRef);
            break;
        case kAudioObjectPropertyName:
            *((CFStringRef*)outData) = CFSTR("Virtual Audio Device");
            *outDataSize = sizeof(CFStringRef);
            break;
        case kAudioObjectPropertyManufacturer:
            *((CFStringRef*)outData) = CFSTR("Virtual Audio Driver");
            *outDataSize = sizeof(CFStringRef);
            break;
        case kAudioDevicePropertyStreams:
            // 只返回输出流（纯输出设备）
            ((AudioObjectID*)outData)[0] = kObjectID_Stream_Output;
            *outDataSize = sizeof(AudioObjectID);
            break;
        case kAudioDevicePropertyNominalSampleRate:
            *((Float64*)outData) = gDevice_SampleRate;
            *outDataSize = sizeof(Float64);
            break;
        case kAudioDevicePropertyDeviceIsAlive:
            *((UInt32*)outData) = 1;
            *outDataSize = sizeof(UInt32);
            break;
        case kAudioDevicePropertyDeviceIsRunning:
            *((UInt32*)outData) = (gDevice_IOIsRunning > 0) ? 1 : 0;
            *outDataSize = sizeof(UInt32);
            break;
        case kAudioDevicePropertyIcon:
            {
                // 获取插件 bundle 的路径
                CFStringRef iconPath = CFSTR(
                    "/Library/Audio/Plug-Ins/HAL/VirtualAudioDriver.driver/Contents/Resources/DeviceIcon.icns");
                CFURLRef iconURL = CFURLCreateWithFileSystemPath(NULL, iconPath, kCFURLPOSIXPathStyle, false);
                *((CFURLRef*)outData) = iconURL;
                *outDataSize = sizeof(CFURLRef);
                break;
            }
        case kAudioDevicePropertyTransportType:
            // 设置设备类型为 Virtual，这样 Sound 设置中会显示 "Virtual" 类型
            *((UInt32*)outData) = kAudioDeviceTransportTypeVirtual;
            *outDataSize = sizeof(UInt32);
            break;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            // 可以作为默认/系统默认设备
            *((UInt32*)outData) = 1;
            *outDataSize = sizeof(UInt32);
            break;
        default:
            break;
        }
    }
    else if (inObjectID == kObjectID_Stream_Output)
    {
        switch (inAddress->mSelector)
        {
        case kAudioObjectPropertyBaseClass:
            *((AudioClassID*)outData) = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            break;
        case kAudioObjectPropertyClass:
            *((AudioClassID*)outData) = kAudioStreamClassID;
            *outDataSize = sizeof(AudioClassID);
            break;
        case kAudioStreamPropertyDirection:
            // 0 = 输出 (output)
            *((UInt32*)outData) = 0;
            *outDataSize = sizeof(UInt32);
            break;
        case kAudioStreamPropertyIsActive:
            *((UInt32*)outData) = 1;
            *outDataSize = sizeof(UInt32);
            break;
        default:
            break;
        }
    }
    return 0;
}

static OSStatus VirtualAudioDriver_SetPropertyData(AudioServerPlugInDriverRef __unused inDriver,
                                                   AudioObjectID __unused inObjectID, pid_t __unused inClientProcessID,
                                                   const AudioObjectPropertyAddress* __unused inAddress,
                                                   UInt32 __unused inQualifierDataSize,
                                                   const void* __unused inQualifierData, UInt32 __unused inDataSize,
                                                   const void* __unused inData)
{
    return 0;
}

static OSStatus VirtualAudioDriver_CreateDevice(AudioServerPlugInDriverRef __unused inDriver,
                                                CFDictionaryRef __unused inDescription,
                                                const AudioServerPlugInClientInfo* __unused inClientInfo,
                                                AudioObjectID* __unused outDeviceObjectID)
{
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus VirtualAudioDriver_DestroyDevice(AudioServerPlugInDriverRef __unused inDriver,
                                                 AudioObjectID __unused inDeviceObjectID)
{
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus VirtualAudioDriver_AddDeviceClient(AudioServerPlugInDriverRef __unused inDriver,
                                                   AudioObjectID __unused inDeviceObjectID,
                                                   const AudioServerPlugInClientInfo* inClientInfo)
{
    if (inClientInfo) app_volume_driver_add_client(inClientInfo->mClientID, inClientInfo->mProcessID, NULL, NULL);
    return 0;
}

static OSStatus VirtualAudioDriver_RemoveDeviceClient(AudioServerPlugInDriverRef __unused inDriver,
                                                      AudioObjectID __unused inDeviceObjectID,
                                                      const AudioServerPlugInClientInfo* inClientInfo)
{
    if (inClientInfo) app_volume_driver_remove_client(inClientInfo->mClientID);
    return 0;
}

static OSStatus VirtualAudioDriver_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef __unused inDriver,
                                                                    AudioObjectID __unused inDeviceObjectID,
                                                                    UInt64 __unused inChangeAction,
                                                                    void* __unused inChangeInfo)
{
    return 0;
}

static OSStatus VirtualAudioDriver_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef __unused inDriver,
                                                                  AudioObjectID __unused inDeviceObjectID,
                                                                  UInt64 __unused inChangeAction,
                                                                  void* __unused inChangeInfo)
{
    return 0;
}

static OSStatus VirtualAudioDriver_BeginIOOperation(AudioServerPlugInDriverRef __unused inDriver,
                                                    AudioObjectID __unused inDeviceObjectID, UInt32 __unused inClientID,
                                                    UInt32 __unused inOperationID, UInt32 __unused inIOBufferFrameSize,
                                                    const AudioServerPlugInIOCycleInfo* __unused inIOCycleInfo)
{
    return 0;
}

static OSStatus VirtualAudioDriver_EndIOOperation(AudioServerPlugInDriverRef __unused inDriver,
                                                  AudioObjectID __unused inDeviceObjectID, UInt32 __unused inClientID,
                                                  UInt32 __unused inOperationID, UInt32 __unused inIOBufferFrameSize,
                                                  const AudioServerPlugInIOCycleInfo* __unused inIOCycleInfo)
{
    return 0;
}
