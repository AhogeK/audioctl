#include "driver/virtual_audio_driver.h"
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include "driver/app_volume_driver.h"

// 定义输出流和输入流（支持双工操作）
enum
{
    kObjectID_PlugIn = kAudioObjectPlugInObject,
    kObjectID_Device = 3,
    kObjectID_Stream_Output = 4,
    kObjectID_Stream_Input = 5 // 添加输入流支持，用于 IOProc 录制
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

// [修复] Loopback 缓冲区 - 用于输入操作读取输出数据
static Float32 gLoopbackBuffer[16384];
static volatile atomic_uint gLoopbackWritePos = 0;
static volatile atomic_uint gLoopbackReadPos = 0;

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
    .QueryInterface = VirtualAudioDriver_QueryInterface,
    .AddRef = VirtualAudioDriver_AddRef,
    .Release = VirtualAudioDriver_Release,
    .Initialize = VirtualAudioDriver_Initialize,
    .CreateDevice = VirtualAudioDriver_CreateDevice,
    .DestroyDevice = VirtualAudioDriver_DestroyDevice,
    .AddDeviceClient = VirtualAudioDriver_AddDeviceClient,
    .RemoveDeviceClient = VirtualAudioDriver_RemoveDeviceClient,
    .PerformDeviceConfigurationChange = VirtualAudioDriver_PerformDeviceConfigurationChange,
    .AbortDeviceConfigurationChange = VirtualAudioDriver_AbortDeviceConfigurationChange,
    .HasProperty = VirtualAudioDriver_HasProperty,
    .IsPropertySettable = VirtualAudioDriver_IsPropertySettable,
    .GetPropertyDataSize = VirtualAudioDriver_GetPropertyDataSize,
    .GetPropertyData = VirtualAudioDriver_GetPropertyData,
    .SetPropertyData = VirtualAudioDriver_SetPropertyData,
    .StartIO = VirtualAudioDriver_StartIO,
    .StopIO = VirtualAudioDriver_StopIO,
    .GetZeroTimeStamp = VirtualAudioDriver_GetZeroTimeStamp,
    .WillDoIOOperation = VirtualAudioDriver_WillDoIOOperation,
    .BeginIOOperation = VirtualAudioDriver_BeginIOOperation,
    .DoIOOperation = VirtualAudioDriver_DoIOOperation,
    .EndIOOperation = VirtualAudioDriver_EndIOOperation};

AudioServerPlugInDriverInterface* gAudioServerPlugInDriverInterfacePtr = &gAudioServerPlugInDriverInterface;
AudioServerPlugInDriverRef gAudioServerPlugInDriverRef = &gAudioServerPlugInDriverInterfacePtr;

void* AudioServerPlugIn_Initialize(CFAllocatorRef __unused inAllocator, CFUUIDRef inRequestedTypeUUID)
{
    return CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID) ? gAudioServerPlugInDriverRef : NULL;
}

static HRESULT VirtualAudioDriver_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
    if (inDriver != gAudioServerPlugInDriverRef || !outInterface)
        return kAudioHardwareBadObjectError;
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

static OSStatus VirtualAudioDriver_StartIO(AudioServerPlugInDriverRef __unused inDriver, AudioObjectID inDeviceObjectID,
                                           UInt32 __unused inClientID)
{
    // 验证设备 ID 是否正确
    if (inDeviceObjectID != kObjectID_Device)
    {
        return kAudioHardwareBadObjectError;
    }

    pthread_mutex_lock(&gPlugIn_StateMutex);
    if (gDevice_IOIsRunning == 0)
    {
        gDevice_NumberTimeStamps = 0;
        gDevice_AnchorHostTime = mach_absolute_time();
    }
    gDevice_IOIsRunning++;
    pthread_mutex_unlock(&gPlugIn_StateMutex);

    return 0;
}

static OSStatus VirtualAudioDriver_StopIO(AudioServerPlugInDriverRef __unused inDriver,
                                          AudioObjectID __unused inDeviceObjectID, UInt32 __unused inClientID)
{
    pthread_mutex_lock(&gPlugIn_StateMutex);
    if (gDevice_IOIsRunning > 0)
        gDevice_IOIsRunning--;
    pthread_mutex_unlock(&gPlugIn_StateMutex);

    return 0;
}

// [时钟策略 - Clock Strategy]
//
// 当前配置：物理设备作为 Master Clock (见 aggregate_device_manager.c 第 279 行)
// CFDictionarySetValue(desc, CFSTR("master"), pUIDRef);
//
// 这意味着：
//   - 虚拟设备作为从设备 (Slave)，跟随物理设备的时钟
//   - 物理设备提供稳定的硬件时钟基准
//   - 虚拟设备应用漂移补偿 (drift correction) 来同步
//
// 本函数生成虚拟时钟时间戳，仅用于：
//   1. 驱动自身的缓冲区管理
//   2. 当虚拟设备作为独立设备使用时的时钟源
//
// 性能优化：
//   - 使用局部变量计算，减少内存访问
//   - while 循环限制最大迭代次数（防止 CPU 峰值）
//   - 标量读取无需锁保护
static OSStatus VirtualAudioDriver_GetZeroTimeStamp(AudioServerPlugInDriverRef __unused inDriver,
                                                    AudioObjectID __unused inDeviceObjectID, UInt32 __unused inClientID,
                                                    Float64* outSampleTime, UInt64* outHostTime,
                                                    UInt64* __unused outSeed)
{
    // 获取当前时间
    UInt64 now = mach_absolute_time();

    // 计算每个缓冲区的时间戳数
    const Float64 ticksPerBuf = gDevice_HostTicksPerFrame * (Float64)kDevice_RingBufferSize;
    const UInt64 anchorHostTime = gDevice_AnchorHostTime;

    // 局部变量计算，减少内存访问
    UInt64 localNumberTimeStamps = gDevice_NumberTimeStamps;
    const Float64 currentTargetTime = (Float64)anchorHostTime + (Float64)(localNumberTimeStamps + 1) * ticksPerBuf;

    // 计算需要前进多少缓冲区
    if (currentTargetTime <= (Float64)now)
    {
        // 计算可以前进的缓冲区数量，限制最大迭代次数
        const Float64 diff = (Float64)now - (Float64)anchorHostTime;
        const UInt64 targetBuffers = (UInt64)(diff / ticksPerBuf);

        // 限制单次前进的最大缓冲区数（防止长时间阻塞）
        const UInt64 maxAdvance = 1000;
        if (targetBuffers > localNumberTimeStamps + maxAdvance)
        {
            localNumberTimeStamps = targetBuffers - maxAdvance;
        }
        else if (targetBuffers > localNumberTimeStamps)
        {
            localNumberTimeStamps = targetBuffers;
        }
    }

    // 写回全局变量
    gDevice_NumberTimeStamps = localNumberTimeStamps;

    // 计算输出时间戳
    *outSampleTime = (Float64)localNumberTimeStamps * (Float64)kDevice_RingBufferSize;
    *outHostTime = anchorHostTime + (UInt64)((Float64)localNumberTimeStamps * ticksPerBuf);
    *outSeed = 1;

    return 0;
}

static OSStatus VirtualAudioDriver_WillDoIOOperation(AudioServerPlugInDriverRef __unused inDriver,
                                                     AudioObjectID __unused inDeviceObjectID,
                                                     UInt32 __unused inClientID, UInt32 inOperationID,
                                                     Boolean* outWillDo, Boolean* outWillDoInPlace)
{
    // 支持读写操作（双工设备）
    // 输入操作：用于 IOProc 录制（loopback）
    // 输出操作：用于音量控制和音频输出
    bool ok = (inOperationID == kAudioServerPlugInIOOperationReadInput ||
               inOperationID == kAudioServerPlugInIOOperationWriteMix ||
               inOperationID == kAudioServerPlugInIOOperationProcessOutput ||
               inOperationID == kAudioServerPlugInIOOperationProcessMix);
    if (outWillDo)
        *outWillDo = ok;
    if (outWillDoInPlace)
        *outWillDoInPlace = true;
    return 0;
}

static OSStatus VirtualAudioDriver_DoIOOperation(AudioServerPlugInDriverRef __unused inDriver,
                                                 AudioObjectID __unused inDeviceObjectID,
                                                 AudioObjectID __unused inStreamObjectID, UInt32 inClientID,
                                                 UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                                 const AudioServerPlugInIOCycleInfo* __unused inIOCycleInfo,
                                                 void* ioMainBuffer, void* __unused ioSecondaryBuffer)
{
    if (!ioMainBuffer || inIOBufferFrameSize == 0)
        return 0;

    // 处理输出操作：应用音量控制并存储到 loopback 缓冲区
    if (inOperationID == kAudioServerPlugInIOOperationWriteMix ||
        inOperationID == kAudioServerPlugInIOOperationProcessOutput ||
        inOperationID == kAudioServerPlugInIOOperationProcessMix)
    {
        Float32* buf = (Float32*)ioMainBuffer;
        UInt32 frames = inIOBufferFrameSize;
        UInt32 sampleCount = frames * 2; // 2 channels

        // [实时音频路径 - 热路径 Hot Path]
        app_volume_driver_apply_volume(inClientID, buf, frames, 2);

        // [关键] 将处理后的音频数据复制到 loopback 缓冲区
        UInt32 writePos = atomic_load(&gLoopbackWritePos);
        for (UInt32 i = 0; i < sampleCount; i++)
        {
            gLoopbackBuffer[writePos] = buf[i];
            writePos = (writePos + 1) % (sizeof(gLoopbackBuffer) / sizeof(gLoopbackBuffer[0]));
        }
        atomic_store(&gLoopbackWritePos, writePos);
    }
    // 处理输入操作：从 loopback 缓冲区读取数据
    else if (inOperationID == kAudioServerPlugInIOOperationReadInput)
    {
        Float32* buf = (Float32*)ioMainBuffer;
        UInt32 frames = inIOBufferFrameSize;
        UInt32 sampleCount = frames * 2; // 2 channels

        UInt32 readPos = atomic_load(&gLoopbackReadPos);
        UInt32 writePos = atomic_load(&gLoopbackWritePos);

        // 计算可用数据量
        UInt32 available;
        if (writePos >= readPos)
            available = writePos - readPos;
        else
            available = (sizeof(gLoopbackBuffer) / sizeof(gLoopbackBuffer[0])) - readPos + writePos;

        if (available >= sampleCount)
        {
            for (UInt32 i = 0; i < sampleCount; i++)
            {
                buf[i] = gLoopbackBuffer[readPos];
                readPos = (readPos + 1) % (sizeof(gLoopbackBuffer) / sizeof(gLoopbackBuffer[0]));
            }
            atomic_store(&gLoopbackReadPos, readPos);
        }
        else
        {
            // 数据不足，输出静音
            memset(buf, 0, sampleCount * sizeof(Float32));
        }
    }

    return 0;
}

static Boolean VirtualAudioDriver_HasProperty(AudioServerPlugInDriverRef __unused inDriver, AudioObjectID inObjectID,
                                              pid_t __unused inClientProcessID,
                                              const AudioObjectPropertyAddress* inAddress)
{
    switch (inObjectID)
    {
    case kObjectID_PlugIn:
        return (inAddress->mSelector == kAudioObjectPropertyBaseClass ||
                inAddress->mSelector == kAudioObjectPropertyClass ||
                inAddress->mSelector == kAudioPlugInPropertyDeviceList);
    case kObjectID_Device:
        // 设备支持的属性
        return (inAddress->mSelector == kAudioObjectPropertyBaseClass ||
                inAddress->mSelector == kAudioObjectPropertyClass ||
                inAddress->mSelector == kAudioDevicePropertyDeviceUID ||
                inAddress->mSelector == kAudioObjectPropertyName ||
                inAddress->mSelector == kAudioObjectPropertyManufacturer ||
                inAddress->mSelector == kAudioDevicePropertyStreams ||
                inAddress->mSelector == kAudioDevicePropertyStreamConfiguration ||
                inAddress->mSelector == kAudioDevicePropertyNominalSampleRate ||
                inAddress->mSelector == kAudioDevicePropertyIcon ||
                inAddress->mSelector == kAudioDevicePropertyTransportType ||
                inAddress->mSelector == kAudioDevicePropertyDeviceCanBeDefaultDevice ||
                inAddress->mSelector == kAudioDevicePropertyDeviceCanBeDefaultSystemDevice ||
                inAddress->mSelector == kAudioDevicePropertyDeviceIsAlive ||
                inAddress->mSelector == kAudioDevicePropertyDeviceIsRunning ||
                inAddress->mSelector == kAudioDevicePropertyAppVolumes ||
                inAddress->mSelector == kAudioDevicePropertyAppClientList ||
                // [修复] 关键属性：设备类型识别、延迟、零时间戳周期
                inAddress->mSelector == kAudioDevicePropertyLatency ||
                inAddress->mSelector == kAudioDevicePropertySafetyOffset ||
                inAddress->mSelector == kAudioDevicePropertyZeroTimeStampPeriod);
    case kObjectID_Stream_Output:
    case kObjectID_Stream_Input:
        // 输入流和输出流都支持标准流属性
        return (inAddress->mSelector == kAudioObjectPropertyBaseClass ||
                inAddress->mSelector == kAudioObjectPropertyClass ||
                inAddress->mSelector == kAudioStreamPropertyDirection ||
                inAddress->mSelector == kAudioStreamPropertyIsActive ||
                inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
                inAddress->mSelector == kAudioStreamPropertyPhysicalFormat ||
                inAddress->mSelector == kAudioStreamPropertyAvailableVirtualFormats ||
                inAddress->mSelector == kAudioStreamPropertyAvailablePhysicalFormats ||
                inAddress->mSelector == kAudioStreamPropertyTerminalType ||
                inAddress->mSelector == kAudioStreamPropertyStartingChannel);
    default:
        break;
    }
    return false;
}

static OSStatus VirtualAudioDriver_IsPropertySettable(AudioServerPlugInDriverRef __unused inDriver,
                                                      AudioObjectID __unused inObjectID,
                                                      pid_t __unused inClientProcessID,
                                                      const AudioObjectPropertyAddress* inAddress,
                                                      Boolean* outIsSettable)
{
    if (inAddress->mSelector == kAudioDevicePropertyAppVolumes)
    {
        *outIsSettable = true;
        return 0;
    }
    *outIsSettable = false;
    return 0;
}

static OSStatus VirtualAudioDriver_GetPropertyDataSize(AudioServerPlugInDriverRef __unused inDriver,
                                                       AudioObjectID inObjectID, pid_t __unused inClientProcessID,
                                                       const AudioObjectPropertyAddress* inAddress,
                                                       UInt32 __unused inQualifierDataSize,
                                                       const void* __unused inQualifierData, UInt32* outDataSize)
{
    // 检查自定义属性（仅对 Device 对象支持）
    if (inObjectID == kObjectID_Device)
    {
        if (inAddress->mSelector == kAudioDevicePropertyAppVolumes)
        {
            *outDataSize = sizeof(AppVolumeTable);
            return 0;
        }
        else if (inAddress->mSelector == kAudioDevicePropertyAppClientList)
        {
            // 最大客户端数 * PID大小 + count字段
            *outDataSize = sizeof(UInt32) + MAX_APP_ENTRIES * sizeof(pid_t);
            return 0;
        }
    }

    if (inObjectID == kObjectID_Device && inAddress->mSelector == kAudioDevicePropertyStreams)
    {
        *outDataSize = sizeof(AudioObjectID) * 2; // 输出流和输入流
    }
    else if (inObjectID == kObjectID_Device && inAddress->mSelector == kAudioDevicePropertyStreamConfiguration)
    {
        *outDataSize = sizeof(AudioBufferList) + sizeof(AudioBuffer); // 一个缓冲区
    }
    else if (inObjectID == kObjectID_PlugIn && inAddress->mSelector == kAudioPlugInPropertyDeviceList)
    {
        *outDataSize = sizeof(AudioObjectID); // 只有一个设备
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
    // [修复] 添加流对象属性大小（支持输入流和输出流）
    else if ((inObjectID == kObjectID_Stream_Output || inObjectID == kObjectID_Stream_Input) &&
             (inAddress->mSelector == kAudioStreamPropertyAvailableVirtualFormats ||
              inAddress->mSelector == kAudioStreamPropertyAvailablePhysicalFormats))
    {
        *outDataSize = sizeof(AudioStreamRangedDescription);
    }
    else if ((inObjectID == kObjectID_Stream_Output || inObjectID == kObjectID_Stream_Input) &&
             (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
              inAddress->mSelector == kAudioStreamPropertyPhysicalFormat))
    {
        *outDataSize = sizeof(AudioStreamBasicDescription);
    }
    else if ((inObjectID == kObjectID_Stream_Output || inObjectID == kObjectID_Stream_Input) &&
             (inAddress->mSelector == kAudioObjectPropertyBaseClass ||
              inAddress->mSelector == kAudioObjectPropertyClass))
    {
        *outDataSize = sizeof(AudioClassID);
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
            // 返回输出流和输入流（支持双工操作）
            ((AudioObjectID*)outData)[0] = kObjectID_Stream_Output;
            ((AudioObjectID*)outData)[1] = kObjectID_Stream_Input;
            *outDataSize = sizeof(AudioObjectID) * 2;
            break;
        case kAudioDevicePropertyStreamConfiguration:
            {
                AudioBufferList* list = (AudioBufferList*)outData;
                list->mNumberBuffers = 1;
                list->mBuffers[0].mNumberChannels = 2;
                list->mBuffers[0].mDataByteSize = 512 * 2 * sizeof(Float32);
                list->mBuffers[0].mData = NULL;
                *outDataSize = sizeof(AudioBufferList) + sizeof(AudioBuffer);
            }
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
        // [修复] 关键属性：设备延迟和同步
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
            // 虚拟设备通常为 0 延迟
            *((UInt32*)outData) = 0;
            *outDataSize = sizeof(UInt32);
            break;
        case kAudioDevicePropertyZeroTimeStampPeriod:
            // 零时间戳周期：决定系统回调频率的稳定性
            // 使用 RingBuffer 大小 16384 / 48000Hz ≈ 341ms
            // 使用 1024 帧作为一个周期
            *((UInt32*)outData) = kDevice_RingBufferSize;
            *outDataSize = sizeof(UInt32);
            break;
        case kAudioDevicePropertyIcon:
            {
                // 获取插件 bundle 的路径
                CFStringRef iconPath =
                    CFSTR("/Library/Audio/Plug-Ins/HAL/VirtualAudioDriver.driver/Contents/Resources/DeviceIcon.icns");
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
        case kAudioDevicePropertyAppVolumes:
            if (*outDataSize >= sizeof(AppVolumeTable))
            {
                app_volume_driver_get_table((AppVolumeTable*)outData);
                *outDataSize = sizeof(AppVolumeTable);
            }
            break;
        case kAudioDevicePropertyAppClientList:
            {
                // 返回当前连接的客户端PID列表
                // 数据格式: UInt32 count + pid_t pids[]
                // 即使客户端列表为空，也返回 count=0 和空列表
                UInt32 minSize = sizeof(UInt32);

                if (*outDataSize < minSize)
                {
                    // 缓冲区太小
                    *outDataSize = minSize;
                    break;
                }

                // 计算最多能返回多少 PID
                UInt32 availableSpace = *outDataSize - sizeof(UInt32);
                UInt32 maxPids = availableSpace / sizeof(pid_t);

                pid_t* pids = (pid_t*)((UInt8*)outData + sizeof(UInt32));
                UInt32 actualCount = 0;

                app_volume_driver_get_client_pids(pids, maxPids, &actualCount);

                *(UInt32*)outData = actualCount;
                *outDataSize = sizeof(UInt32) + actualCount * sizeof(pid_t);
            }
            break;
        default:
            break;
        }
    }
    else if (inObjectID == kObjectID_Stream_Output || inObjectID == kObjectID_Stream_Input)
    {
        // 判断是输入流还是输出流
        bool isInput = (inObjectID == kObjectID_Stream_Input);

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
            // 0 = 输出 (output), 1 = 输入 (input)
            *((UInt32*)outData) = isInput ? 1 : 0;
            *outDataSize = sizeof(UInt32);
            break;
        case kAudioStreamPropertyIsActive:
            *((UInt32*)outData) = 1;
            *outDataSize = sizeof(UInt32);
            break;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            // 设置音频格式：48kHz, 32-bit Float, 2 channels
            {
                AudioStreamBasicDescription* format = (AudioStreamBasicDescription*)outData;
                format->mSampleRate = 48000.0;
                format->mFormatID = kAudioFormatLinearPCM;
                format->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
                format->mBytesPerPacket = 8;
                format->mFramesPerPacket = 1;
                format->mBytesPerFrame = 8;
                format->mChannelsPerFrame = 2;
                format->mBitsPerChannel = 32;
                format->mReserved = 0;
                *outDataSize = sizeof(AudioStreamBasicDescription);
            }
            break;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            {
                AudioStreamRangedDescription* format = (AudioStreamRangedDescription*)outData;
                format->mFormat.mSampleRate = 48000.0;
                format->mFormat.mFormatID = kAudioFormatLinearPCM;
                format->mFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
                format->mFormat.mBytesPerPacket = 8;
                format->mFormat.mFramesPerPacket = 1;
                format->mFormat.mBytesPerFrame = 8;
                format->mFormat.mChannelsPerFrame = 2;
                format->mFormat.mBitsPerChannel = 32;
                format->mFormat.mReserved = 0;
                format->mSampleRateRange.mMinimum = 48000.0;
                format->mSampleRateRange.mMaximum = 48000.0;
                *outDataSize = sizeof(AudioStreamRangedDescription);
            }
            break;
        case kAudioStreamPropertyTerminalType:
            *((UInt32*)outData) = isInput ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker;
            *outDataSize = sizeof(UInt32);
            break;
        case kAudioStreamPropertyStartingChannel:
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
    if (inClientInfo)
    {
        // Register with local driver
        app_volume_driver_add_client(inClientInfo->mClientID, inClientInfo->mProcessID, NULL, NULL);

        // This will be implemented in the new IPC architecture
    }
    return 0;
}

static OSStatus VirtualAudioDriver_RemoveDeviceClient(AudioServerPlugInDriverRef __unused inDriver,
                                                      AudioObjectID __unused inDeviceObjectID,
                                                      const AudioServerPlugInClientInfo* inClientInfo)
{
    if (inClientInfo)
    {
        // Unregister from local driver
        app_volume_driver_remove_client(inClientInfo->mClientID);

        // This will be implemented in the new IPC architecture
    }
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

#pragma mark - Helper Functions

// Helper functions can be added here
