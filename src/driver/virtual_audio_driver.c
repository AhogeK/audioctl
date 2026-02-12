#include "driver/virtual_audio_driver.h"
#include <mach/mach_time.h>
#include <math.h>
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
// 【关键修复】使用原子变量避免实时线程中的互斥锁
static _Atomic UInt64 gDevice_IOIsRunning = 0;
static Float64 gDevice_HostTicksPerFrame = 0.0;
static _Atomic UInt64 gDevice_NumberTimeStamps = 0;
static _Atomic UInt64 gDevice_AnchorHostTime = 0;

// 重构新增：Zero TimeStamp 周期和 Seed
static const UInt32 kZeroTimeStampPeriod = 512; // 512 帧短周期
static atomic_uint_fast64_t gZTS_Seed = 1; // 原子 Seed，用于强制 Host 重收敛

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

// 【关键修复】StartIO - 使用原子操作避免实时线程中的互斥锁
// 实时音频线程中严禁使用 mutex，会导致优先级反转和音频卡顿
static OSStatus VirtualAudioDriver_StartIO(AudioServerPlugInDriverRef __unused inDriver, AudioObjectID inDeviceObjectID,
                                           UInt32 __unused inClientID)
{
    // 验证设备 ID 是否正确
    if (inDeviceObjectID != kObjectID_Device)
    {
        return -1; // 通用错误码
    }

    // 【修复】使用原子操作替代互斥锁
    // 先读取当前计数，检查是否为 0
    UInt64 prevCount = atomic_fetch_add_explicit(&gDevice_IOIsRunning, 1, memory_order_acq_rel);

    if (prevCount == 0)
    {
        // 第一个客户端启动，重置时间戳计数并设置新的 Anchor 时间
        atomic_store_explicit(&gDevice_NumberTimeStamps, 0, memory_order_release);
        atomic_store_explicit(&gDevice_AnchorHostTime, mach_absolute_time(), memory_order_release);
        // 自增 Seed 强制 Host 重新收敛时钟
        atomic_fetch_add_explicit(&gZTS_Seed, 1, memory_order_release);
    }

    return 0;
}

// 【关键修复】StopIO - 使用原子操作避免实时线程中的互斥锁
// 注意：此函数可能在非实时上下文中调用，但仍需避免锁以保证响应性
static OSStatus VirtualAudioDriver_StopIO(AudioServerPlugInDriverRef __unused inDriver,
                                          AudioObjectID __unused inDeviceObjectID, UInt32 __unused inClientID)
{
    // 【修复】使用原子操作替代互斥锁
    // fetch_sub 返回的是递减前的值
    UInt64 prevCount = atomic_fetch_sub_explicit(&gDevice_IOIsRunning, 1, memory_order_acq_rel);

    if (prevCount == 1)
    {
        // 最后一个客户端停止，重置 ring buffer
        atomic_store_explicit(&gLoopbackWritePos, 0, memory_order_relaxed);
        atomic_store_explicit(&gLoopbackReadPos, 0, memory_order_relaxed);
        // 清零缓冲区（防止下次启动读到垃圾数据）
        // 注意：memset 不是原子操作，但此时所有 IO 都已停止，是安全的
        memset(gLoopbackBuffer, 0, sizeof(gLoopbackBuffer));
        // 自增 Seed 强制 Host 重新收敛
        atomic_fetch_add_explicit(&gZTS_Seed, 1, memory_order_release);
    }

    return 0;
}

// [重构时钟策略 - Refactored Clock Strategy]
//
// 周期从 16384 帧改为 512 帧，消除"慢放感"
// 基于 mach_absolute_time() 的单调推导，不再依赖缓冲区计数
// gZTS_Seed 在 Start/Stop/Underrun/Overrun 时自增，强制 Host 重新收敛
//
// 技术规格：
//   - 周期: 512 帧 (约 10.67ms @ 48kHz)
//   - 格式: Non-Interleaved Float32, 2ch
//   - 时钟源: mach_absolute_time() 转换为音频采样时间

// 【关键修复】安全的 GetZeroTimeStamp 实现，防止 CPU 卡死
// 修复内容：
// 1. 防止除零：检查 gDevice_HostTicksPerFrame 是否为 0
// 2. 防止时间回绕：检查 AnchorTime 是否已初始化
// 3. 防止 Inf/NaN：验证计算结果有效性
static OSStatus VirtualAudioDriver_GetZeroTimeStamp(AudioServerPlugInDriverRef __unused inDriver,
                                                    AudioObjectID __unused inDeviceObjectID, UInt32 __unused inClientID,
                                                    Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed)
{
    // 【修复】检查输出指针
    if (outSampleTime == NULL || outHostTime == NULL || outSeed == NULL)
    {
        return -1; // 通用错误
    }

    // 【修复】防止除零：如果 HostTicksPerFrame 未初始化，使用默认值
    // 默认值基于 48kHz 和纳秒时钟
    Float64 hostTicksPerFrame = gDevice_HostTicksPerFrame;
    if (hostTicksPerFrame <= 0.0)
    {
        // 安全默认值：假设 1 tick = 1 ns @ 48kHz
        hostTicksPerFrame = 1000000000.0 / 48000.0;
    }

    // 【修复】防止时间回绕：检查 AnchorTime 是否已初始化
    UInt64 anchorTime = gDevice_AnchorHostTime;
    if (anchorTime == 0)
    {
        // 未初始化，使用当前时间作为锚点
        anchorTime = mach_absolute_time();
        gDevice_AnchorHostTime = anchorTime;
    }

    // 获取当前 mach 时间
    UInt64 now = mach_absolute_time();

    // 【修复】防止时间回绕：如果 now < anchor，说明时钟异常
    UInt64 elapsedTicks;
    if (now >= anchorTime)
    {
        elapsedTicks = now - anchorTime;
    }
    else
    {
        // 时钟回绕或异常，重置锚点
        anchorTime = now;
        gDevice_AnchorHostTime = anchorTime;
        elapsedTicks = 0;
    }

    // 计算经过的帧数
    Float64 elapsedFrames = (Float64)elapsedTicks / hostTicksPerFrame;

    // 【修复】防止 Inf/NaN：检查结果有效性
    if (!isfinite(elapsedFrames) || elapsedFrames < 0.0)
    {
        elapsedFrames = 0.0;
    }

    // 计算周期数 (每 512 帧一个周期)
    UInt64 periodCount = (UInt64)(elapsedFrames / (Float64)kZeroTimeStampPeriod);

    // 计算采样时间：周期数 * 周期大小
    Float64 sampleTime = (Float64)periodCount * (Float64)kZeroTimeStampPeriod;

    // 【修复】防止 Inf/NaN
    if (!isfinite(sampleTime) || sampleTime < 0.0)
    {
        sampleTime = 0.0;
        periodCount = 0;
    }

    *outSampleTime = sampleTime;

    // 计算对应的主机时间
    Float64 hostTimeFloat = (Float64)anchorTime + (Float64)(periodCount * kZeroTimeStampPeriod) * hostTicksPerFrame;

    // 【修复】防止 Inf/NaN 和溢出
    if (!isfinite(hostTimeFloat) || hostTimeFloat < 0.0 || hostTimeFloat > (Float64)UINT64_MAX)
    {
        *outHostTime = anchorTime;
    }
    else
    {
        *outHostTime = (UInt64)hostTimeFloat;
    }

    // 返回原子 Seed
    *outSeed = atomic_load(&gZTS_Seed);

    return 0;
}

static OSStatus VirtualAudioDriver_WillDoIOOperation(AudioServerPlugInDriverRef __unused inDriver,
                                                     AudioObjectID __unused inDeviceObjectID,
                                                     UInt32 __unused inClientID, UInt32 inOperationID,
                                                     Boolean* outWillDo, Boolean* outWillDoInPlace)
{
    // 【修复】只启用必要的操作，避免 MixOutput 短路 WriteMix
    // ProcessOutput: per-client gain (per-app volume)
    // WriteMix: write to loopback ring (once per cycle)
    // ReadInput: read from loopback ring
    // ⚠️ 不要启用 ProcessMix/MixOutput，否则 WriteMix 不会被调用，导致无声
    bool ok = false;
    switch (inOperationID)
    {
    case kAudioServerPlugInIOOperationProcessOutput:
    case kAudioServerPlugInIOOperationWriteMix:
    case kAudioServerPlugInIOOperationReadInput:
        ok = true;
        break;
    default:
        ok = false;
        break;
    }
    if (outWillDo)
        *outWillDo = ok;
    if (outWillDoInPlace)
        *outWillDoInPlace = true;
    return 0;
}

// 【修复】ABL 稳健校验：返回实际可安全读写的帧数，0 表示布局错误
static inline UInt32 abl_ni_clamp_frames(const AudioBufferList* abl, UInt32 ch, UInt32 frames)
{
    if (abl->mNumberBuffers != ch)
        return 0;

    UInt32 minFrames = frames;
    for (UInt32 b = 0; b < ch; b++)
    {
        const AudioBuffer* buffer = (const AudioBuffer*)((const uint8_t*)&abl->mBuffers[0] + (b * sizeof(AudioBuffer)));
        if (buffer->mNumberChannels != 1 || !buffer->mData)
            return 0;

        UInt32 maxFrames = (UInt32)(buffer->mDataByteSize / sizeof(Float32));
        if (maxFrames < minFrames)
            minFrames = maxFrames;
    }
    return minFrames;
}

// 【修复】ABL 清零：将缓冲区置零（用于布局错误时输出静音）
static inline void abl_ni_zero(AudioBufferList* abl, UInt32 ch, UInt32 frames)
{
    UInt32 useCh = (abl->mNumberBuffers < ch) ? abl->mNumberBuffers : ch;
    for (UInt32 b = 0; b < useCh; b++)
    {
        AudioBuffer* buffer = (AudioBuffer*)((uint8_t*)&abl->mBuffers[0] + (b * sizeof(AudioBuffer)));
        if (buffer->mData && buffer->mDataByteSize >= frames * sizeof(Float32))
        {
            memset(buffer->mData, 0, frames * sizeof(Float32));
        }
    }
}

// 错误布局计数器（原子，用于调试）
static atomic_uint_fast64_t gABLBadLayoutCount = 0;

static inline void note_bad_abl(void) { atomic_fetch_add(&gABLBadLayoutCount, 1); }

static OSStatus VirtualAudioDriver_DoIOOperation(AudioServerPlugInDriverRef __unused inDriver,
                                                 AudioObjectID __unused inDeviceObjectID,
                                                 AudioObjectID __unused inStreamObjectID, UInt32 inClientID,
                                                 UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                                 const AudioServerPlugInIOCycleInfo* __unused inIOCycleInfo,
                                                 void* ioMainBuffer, void* __unused ioSecondaryBuffer)
{
    if (!ioMainBuffer || inIOBufferFrameSize == 0)
        return 0;

    // 【修复】验证 ABL 布局并获取安全帧数
    AudioBufferList* abl = (AudioBufferList*)ioMainBuffer;
    UInt32 frames = abl_ni_clamp_frames(abl, 2, inIOBufferFrameSize);
    if (frames == 0)
    {
        // 布局错误或缓冲区太小，输出静音
        note_bad_abl();
        abl_ni_zero(abl, 2, inIOBufferFrameSize);
        return 0;
    }

    // 【关键修复】使用更安全的数组索引访问，避免指针算术
    // 验证 mNumberBuffers 至少有2个缓冲区（由 abl_ni_clamp_frames 检查）
    AudioBuffer* leftBuffer = &abl->mBuffers[0];
    AudioBuffer* rightBuffer = &abl->mBuffers[1];

    // 【关键修复】验证缓冲区数据指针非空
    if (leftBuffer->mData == NULL || rightBuffer->mData == NULL)
    {
        // 数据指针为空，输出静音
        note_bad_abl();
        abl_ni_zero(abl, 2, inIOBufferFrameSize);
        return 0;
    }

    Float32* leftChannel = (Float32*)leftBuffer->mData;
    Float32* rightChannel = (Float32*)rightBuffer->mData;

    // 处理输出操作：应用音量控制并存储到 loopback 缓冲区
    if (inOperationID == kAudioServerPlugInIOOperationProcessOutput)
    {
        // [实时音频路径 - 热路径 Hot Path]
        // 对每个声道分别应用音量（in-place 处理）
        app_volume_driver_apply_volume_ni(inClientID, leftChannel, rightChannel, frames);
    }
    else if (inOperationID == kAudioServerPlugInIOOperationWriteMix)
    {
        // 将处理后的音频数据写入 loopback 缓冲区（平面存储）
        // 交错格式写入：LRLRLR...
        UInt32 writePos = atomic_load(&gLoopbackWritePos);
        for (UInt32 i = 0; i < frames; i++)
        {
            // 左声道
            gLoopbackBuffer[writePos] = leftChannel[i];
            writePos = (writePos + 1) % (sizeof(gLoopbackBuffer) / sizeof(gLoopbackBuffer[0]));
            // 右声道
            gLoopbackBuffer[writePos] = rightChannel[i];
            writePos = (writePos + 1) % (sizeof(gLoopbackBuffer) / sizeof(gLoopbackBuffer[0]));
        }
        atomic_store(&gLoopbackWritePos, writePos);
    }
    // 处理输入操作：从 loopback 缓冲区读取数据
    else if (inOperationID == kAudioServerPlugInIOOperationReadInput)
    {
        UInt32 readPos = atomic_load(&gLoopbackReadPos);
        UInt32 writePos = atomic_load(&gLoopbackWritePos);
        UInt32 bufferSize = sizeof(gLoopbackBuffer) / sizeof(gLoopbackBuffer[0]);

        // 计算可用数据量（以采样点计，每个采样点是一个声道）
        UInt32 available;
        if (writePos >= readPos)
            available = writePos - readPos;
        else
            available = bufferSize - readPos + writePos;

        UInt32 sampleCount = frames * 2; // 总采样点数（左右声道）

        if (available >= sampleCount)
        {
            for (UInt32 i = 0; i < frames; i++)
            {
                // 左声道
                leftChannel[i] = gLoopbackBuffer[readPos];
                readPos = (readPos + 1) % bufferSize;
                // 右声道
                rightChannel[i] = gLoopbackBuffer[readPos];
                readPos = (readPos + 1) % bufferSize;
            }
            atomic_store(&gLoopbackReadPos, readPos);
        }
        else
        {
            // 数据不足，输出静音
            memset(leftChannel, 0, frames * sizeof(Float32));
            memset(rightChannel, 0, frames * sizeof(Float32));
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
        *outDataSize = sizeof(AudioBufferList) + sizeof(AudioBuffer) * 2; // Non-Interleaved: 2个缓冲区（每个声道一个）
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
                // 修改：Non-Interleaved 格式下，每个声道一个独立的缓冲区
                AudioBufferList* list = (AudioBufferList*)outData;
                list->mNumberBuffers = 2; // 2个声道，每个声道一个缓冲区
                for (UInt32 i = 0; i < 2; i++)
                {
                    // 使用指针访问以避免固定大小数组越界警告
                    AudioBuffer* buffer = (AudioBuffer*)((uint8_t*)&list->mBuffers[0] + (i * sizeof(AudioBuffer)));
                    buffer->mNumberChannels = 1; // 每个缓冲区只有一个声道
                    buffer->mDataByteSize = 512 * sizeof(Float32); // 每帧4字节
                    buffer->mData = NULL;
                }
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
            // 零时间戳周期：改为 512 帧短周期 (约 10.67ms @ 48kHz)
            // 消除音频"慢放感"，提高实时响应性
            *((UInt32*)outData) = kZeroTimeStampPeriod;
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
            // 设置音频格式：48kHz, 32-bit Float, 2 channels, Non-Interleaved
            // 修改：从 Interleaved 改为 Non-Interleaved (Planar)，消除"慢放感"
            {
                AudioStreamBasicDescription* format = (AudioStreamBasicDescription*)outData;
                format->mSampleRate = 48000.0;
                format->mFormatID = kAudioFormatLinearPCM;
                format->mFormatFlags =
                    kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
                format->mBytesPerPacket = 4;
                format->mFramesPerPacket = 1;
                format->mBytesPerFrame = 4;
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
                format->mFormat.mFormatFlags =
                    kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
                format->mFormat.mBytesPerPacket = 4;
                format->mFormat.mFramesPerPacket = 1;
                format->mFormat.mBytesPerFrame = 4;
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
