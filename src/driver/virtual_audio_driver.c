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

// 核心状态变量
static _Atomic UInt64 gDevice_IOIsRunning = 0;
static Float64 gDevice_HostTicksPerFrame = 0.0;
static _Atomic UInt64 gDevice_NumberTimeStamps = 0;
static _Atomic UInt64 gDevice_AnchorHostTime = 0;
// [Freewheel] 维护一个全局的采样计数器，作为所有时间的基准
static _Atomic UInt64 gDevice_CurrentFrameCount = 0;

// Zero TimeStamp period for scheduling jitter tolerance
static const UInt32 kZeroTimeStampPeriod = 4096;
static atomic_uint_fast64_t gZTS_Seed = 1;

// Loopback buffer for input stream reading output data
static Float32 gLoopbackBuffer[16384];
static volatile atomic_uint gLoopbackWritePos = 0;
static volatile atomic_uint gLoopbackReadPos = 0;

// 定义 Log Subsystem
static os_log_t gLog = NULL;

// Forward Declarations

static HRESULT
VirtualAudioDriver_QueryInterface (void *inDriver, REFIID inUUID,
				   LPVOID *outInterface);
static ULONG
VirtualAudioDriver_AddRef (void *inDriver);
static ULONG
VirtualAudioDriver_Release (void *inDriver);
static OSStatus
VirtualAudioDriver_Initialize (AudioServerPlugInDriverRef inDriver,
			       AudioServerPlugInHostRef inHost);
static OSStatus
VirtualAudioDriver_CreateDevice (
  AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription,
  const AudioServerPlugInClientInfo *inClientInfo,
  AudioObjectID *outDeviceObjectID);
static OSStatus
VirtualAudioDriver_DestroyDevice (AudioServerPlugInDriverRef inDriver,
				  AudioObjectID inDeviceObjectID);
static OSStatus
VirtualAudioDriver_AddDeviceClient (
  AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
  const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus
VirtualAudioDriver_RemoveDeviceClient (
  AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
  const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus
VirtualAudioDriver_PerformDeviceConfigurationChange (
  AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
  UInt64 inChangeAction, void *inChangeInfo);
static OSStatus
VirtualAudioDriver_AbortDeviceConfigurationChange (
  AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
  UInt64 inChangeAction, void *inChangeInfo);
static Boolean
VirtualAudioDriver_HasProperty (AudioServerPlugInDriverRef inDriver,
				AudioObjectID inObjectID,
				pid_t inClientProcessID,
				const AudioObjectPropertyAddress *inAddress);
static OSStatus
VirtualAudioDriver_IsPropertySettable (
  AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
  pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
  Boolean *outIsSettable);
static OSStatus
VirtualAudioDriver_GetPropertyDataSize (
  AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
  pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
  UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize);
static OSStatus
VirtualAudioDriver_GetPropertyData (
  AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
  pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
  UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize,
  UInt32 *outDataSize, void *outData);
static OSStatus
VirtualAudioDriver_SetPropertyData (AudioServerPlugInDriverRef inDriver,
				    AudioObjectID inObjectID,
				    pid_t inClientProcessID,
				    const AudioObjectPropertyAddress *inAddress,
				    UInt32 inQualifierDataSize,
				    const void *inQualifierData,
				    UInt32 inDataSize, const void *inData);
static OSStatus
VirtualAudioDriver_StartIO (AudioServerPlugInDriverRef inDriver,
			    AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus
VirtualAudioDriver_StopIO (AudioServerPlugInDriverRef inDriver,
			   AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus
VirtualAudioDriver_GetZeroTimeStamp (AudioServerPlugInDriverRef inDriver,
				     AudioObjectID inDeviceObjectID,
				     UInt32 inClientID, Float64 *outSampleTime,
				     UInt64 *outHostTime, UInt64 *outSeed);
static OSStatus
VirtualAudioDriver_WillDoIOOperation (AudioServerPlugInDriverRef inDriver,
				      AudioObjectID inDeviceObjectID,
				      UInt32 inClientID, UInt32 inOperationID,
				      Boolean *outWillDo,
				      Boolean *outWillDoInPlace);
static OSStatus
VirtualAudioDriver_BeginIOOperation (
  AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
  UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
  const AudioServerPlugInIOCycleInfo *inIOCycleInfo);
static OSStatus
VirtualAudioDriver_DoIOOperation (
  AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
  AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID,
  UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
  void *ioMainBuffer, void *ioSecondaryBuffer);
static OSStatus
VirtualAudioDriver_EndIOOperation (
  AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
  UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
  const AudioServerPlugInIOCycleInfo *inIOCycleInfo);

AudioServerPlugInDriverInterface gAudioServerPlugInDriverInterface
  = {.QueryInterface = VirtualAudioDriver_QueryInterface,
     .AddRef = VirtualAudioDriver_AddRef,
     .Release = VirtualAudioDriver_Release,
     .Initialize = VirtualAudioDriver_Initialize,
     .CreateDevice = VirtualAudioDriver_CreateDevice,
     .DestroyDevice = VirtualAudioDriver_DestroyDevice,
     .AddDeviceClient = VirtualAudioDriver_AddDeviceClient,
     .RemoveDeviceClient = VirtualAudioDriver_RemoveDeviceClient,
     .PerformDeviceConfigurationChange
     = VirtualAudioDriver_PerformDeviceConfigurationChange,
     .AbortDeviceConfigurationChange
     = VirtualAudioDriver_AbortDeviceConfigurationChange,
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

AudioServerPlugInDriverInterface *gAudioServerPlugInDriverInterfacePtr
  = &gAudioServerPlugInDriverInterface;
AudioServerPlugInDriverRef gAudioServerPlugInDriverRef
  = &gAudioServerPlugInDriverInterfacePtr;

void *
AudioServerPlugIn_Initialize (CFAllocatorRef __unused inAllocator,
			      CFUUIDRef inRequestedTypeUUID)
{
  return CFEqual (inRequestedTypeUUID, kAudioServerPlugInTypeUUID)
	   ? gAudioServerPlugInDriverRef
	   : NULL;
}

static HRESULT
VirtualAudioDriver_QueryInterface (void *inDriver, REFIID inUUID,
				   LPVOID *outInterface)
{
  if (inDriver != gAudioServerPlugInDriverRef || !outInterface)
    return kAudioHardwareBadObjectError;
  CFUUIDRef req = CFUUIDCreateFromUUIDBytes (NULL, inUUID);
  HRESULT res = E_NOINTERFACE;
  if (CFEqual (req, IUnknownUUID)
      || CFEqual (req, kAudioServerPlugInDriverInterfaceUUID))
    {
      pthread_mutex_lock (&gPlugIn_StateMutex);
      gPlugIn_RefCount++;
      pthread_mutex_unlock (&gPlugIn_StateMutex);
      *outInterface = gAudioServerPlugInDriverRef;
      res = S_OK;
    }
  CFRelease (req);
  return res;
}

static ULONG
VirtualAudioDriver_AddRef (void *__unused inDriver)
{
  pthread_mutex_lock (&gPlugIn_StateMutex);
  gPlugIn_RefCount++;
  ULONG res = gPlugIn_RefCount;
  pthread_mutex_unlock (&gPlugIn_StateMutex);
  return res;
}

static ULONG
VirtualAudioDriver_Release (void *__unused inDriver)
{
  pthread_mutex_lock (&gPlugIn_StateMutex);
  ULONG res = (gPlugIn_RefCount > 0) ? --gPlugIn_RefCount : 0;
  pthread_mutex_unlock (&gPlugIn_StateMutex);
  return res;
}

static OSStatus
VirtualAudioDriver_Initialize (AudioServerPlugInDriverRef __unused inDriver,
			       AudioServerPlugInHostRef inHost)
{
  gPlugIn_Host = inHost;
  struct mach_timebase_info tb;
  mach_timebase_info (&tb);
  Float64 freq = (Float64) tb.denom / (Float64) tb.numer * 1000000000.0;
  gDevice_HostTicksPerFrame = freq / gDevice_SampleRate;

  // 初始化日志系统
  if (gLog == NULL)
    {
      gLog = os_log_create ("com.ahogek.audioctl", "Driver");
    }
  os_log_info (gLog,
	       "VirtualAudioDriver init: Rate=%.1f, Numer=%u, Denom=%u, "
	       "TicksPerFrame=%.4f",
	       gDevice_SampleRate, tb.numer, tb.denom,
	       gDevice_HostTicksPerFrame);

  app_volume_driver_init ();

  return 0;
}

// StartIO - uses atomic operations to avoid mutex in real-time thread
// Real-time audio threads must not use mutex (causes priority inversion)
static OSStatus
VirtualAudioDriver_StartIO (AudioServerPlugInDriverRef __unused inDriver,
			    AudioObjectID inDeviceObjectID,
			    UInt32 __unused inClientID)
{
  // Verify device ID
  if (inDeviceObjectID != kObjectID_Device)
    {
      return kAudioHardwareBadObjectError;
    }

  // Use atomic operations instead of mutex
  // First read current count and check if it's 0
  UInt64 prevCount
    = atomic_fetch_add_explicit (&gDevice_IOIsRunning, 1, memory_order_acq_rel);

  if (gLog)
    os_log_info (gLog, "StartIO: ClientID=%u, PrevCount=%llu", inClientID,
		 prevCount);

  if (prevCount == 0)
    {
      // 第一个客户端启动
      atomic_store_explicit (&gDevice_NumberTimeStamps, 0,
			     memory_order_release);

      // [Freewheel] 重置采样计数器
      atomic_store_explicit (&gDevice_CurrentFrameCount, 0,
			     memory_order_release);

      // 设定 Anchor 为当前时间
      UInt64 now = mach_absolute_time ();
      atomic_store_explicit (&gDevice_AnchorHostTime, now,
			     memory_order_release);

      // 自增 Seed 强制 Host 重新收敛时钟
      atomic_fetch_add_explicit (&gZTS_Seed, 1, memory_order_release);

      if (gLog)
	os_log_info (gLog, "StartIO: Freewheel Clock Started, Anchor=%llu",
		     now);
    }

  return 0;
}

// StopIO - uses atomic operations for responsiveness
static OSStatus
VirtualAudioDriver_StopIO (AudioServerPlugInDriverRef __unused inDriver,
			   AudioObjectID __unused inDeviceObjectID,
			   UInt32 __unused inClientID)
{
  // Use atomic operations instead of mutex
  // fetch_sub returns the value before decrement
  UInt64 prevCount
    = atomic_fetch_sub_explicit (&gDevice_IOIsRunning, 1, memory_order_acq_rel);

  if (gLog)
    os_log_info (gLog, "StopIO: ClientID=%u, PrevCount=%llu", inClientID,
		 prevCount);

  if (prevCount == 1)
    {
      // 最后一个客户端停止，重置 ring buffer
      atomic_store_explicit (&gLoopbackWritePos, 0, memory_order_relaxed);
      atomic_store_explicit (&gLoopbackReadPos, 0, memory_order_relaxed);
      // 清零缓冲区（防止下次启动读到垃圾数据）
      // 注意：memset 不是原子操作，但此时所有 IO 都已停止，是安全的
      memset (gLoopbackBuffer, 0, sizeof (gLoopbackBuffer));
      // 自增 Seed 强制 Host 重新收敛
      atomic_fetch_add_explicit (&gZTS_Seed, 1, memory_order_release);

      if (gLog)
	os_log_info (gLog, "StopIO: Last client, reset buffers");
    }

  return 0;
}

// GetZeroTimeStamp - Freewheel mode
// Calculates HostTime purely from internal sample counter, ignoring system
// clock jitter
static OSStatus
VirtualAudioDriver_GetZeroTimeStamp (
  AudioServerPlugInDriverRef __unused inDriver,
  AudioObjectID __unused inDeviceObjectID, UInt32 __unused inClientID,
  Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed)
{
  if (outSampleTime == NULL || outHostTime == NULL || outSeed == NULL)
    {
      return kAudioHardwareIllegalOperationError;
    }

  Float64 hostTicksPerFrame = gDevice_HostTicksPerFrame;
  if (hostTicksPerFrame <= 0.0)
    {
      hostTicksPerFrame = 1000000000.0 / 48000.0;
    }

  // Get base Anchor
  UInt64 anchorTime
    = atomic_load_explicit (&gDevice_AnchorHostTime, memory_order_acquire);

  // Get current frame count that driver has advanced to
  UInt64 currentFrames
    = atomic_load_explicit (&gDevice_CurrentFrameCount, memory_order_acquire);

  // Calculate corresponding logical HostTime
  // HostTime = Anchor + Frames * TicksPerFrame
  UInt64 offsetTicks = (UInt64) ((Float64) currentFrames * hostTicksPerFrame);
  UInt64 logicHostTime = anchorTime + offsetTicks;

  // Return results
  // This makes HAL see SampleTime and HostTime always perfectly matched to
  // 48kHz definition
  *outSampleTime = (Float64) currentFrames;
  *outHostTime = logicHostTime;
  *outSeed = atomic_load_explicit (&gZTS_Seed, memory_order_acquire);

  return 0;
}

static OSStatus
VirtualAudioDriver_WillDoIOOperation (
  AudioServerPlugInDriverRef __unused inDriver,
  AudioObjectID __unused inDeviceObjectID, UInt32 __unused inClientID,
  UInt32 inOperationID, Boolean *outWillDo, Boolean *outWillDoInPlace)
{
  // Fine-grained control for IO operations:
  // ProcessOutput: volume control, must be supported, InPlace=true
  // WriteMix: write to ring buffer, InPlace=true
  // ReadInput: read from ring buffer, InPlace=true

  bool willDo = false;
  bool willDoInPlace = true;

  switch (inOperationID)
    {
    case kAudioServerPlugInIOOperationProcessOutput:
    case kAudioServerPlugInIOOperationWriteMix:
    case kAudioServerPlugInIOOperationReadInput:
      willDo = true;
      break;
    default:
      willDo = false;
      break;
    }

  if (outWillDo)
    *outWillDo = willDo;
  if (outWillDoInPlace)
    *outWillDoInPlace = willDoInPlace;

  return 0;
}

// 错误布局计数器（原子，用于调试）
static atomic_uint_fast64_t gABLBadLayoutCount = 0;

static inline void
note_bad_abl (void)
{
  atomic_fetch_add (&gABLBadLayoutCount, 1);
}

static atomic_uint_fast64_t gIOCycleCount = 0;

static OSStatus
VirtualAudioDriver_DoIOOperation (
  AudioServerPlugInDriverRef __unused inDriver,
  AudioObjectID __unused inDeviceObjectID,
  AudioObjectID __unused inStreamObjectID, UInt32 inClientID,
  UInt32 inOperationID, UInt32 inIOBufferFrameSize,
  const AudioServerPlugInIOCycleInfo *__unused inIOCycleInfo,
  void *ioMainBuffer, void *__unused ioSecondaryBuffer)
{
  if (!ioMainBuffer || inIOBufferFrameSize == 0)
    return 0;

  // 调试日志：每 100 个周期打印一次 WriteMix，确认有数据
  // 注意：不要在实时线程频繁打印
  if (inOperationID == kAudioServerPlugInIOOperationWriteMix)
    {
      atomic_fetch_add_explicit (&gIOCycleCount, 1, memory_order_relaxed);
    }

  // Verify ABL layout and get safe frame count
  AudioBufferList *abl = (AudioBufferList *) ioMainBuffer;
  // For Interleaved format, expect mNumberBuffers=1, mNumberChannels=2
  if (abl->mNumberBuffers != 1)
    {
      note_bad_abl ();
      return 0;
    }

  AudioBuffer *buffer = &abl->mBuffers[0];
  if (!buffer->mData || buffer->mNumberChannels != 2)
    {
      note_bad_abl ();
      return 0;
    }

  // 帧数 = 字节数 / 每帧字节数 (8)
  UInt32 frames = buffer->mDataByteSize / 8;
  // 限制为请求的帧数
  if (frames > inIOBufferFrameSize)
    {
      frames = inIOBufferFrameSize;
    }

  Float32 *samples = (Float32 *) buffer->mData;

  // 处理输出操作：应用音量控制并存储到 loopback 缓冲区
  if (inOperationID == kAudioServerPlugInIOOperationProcessOutput)
    {
      // [实时音频路径 - 热路径 Hot Path]
      // 恢复音量控制
      app_volume_driver_apply_volume (inClientID, samples, frames, 2);
    }
  else if (inOperationID == kAudioServerPlugInIOOperationWriteMix)
    {
      // 将处理后的音频数据写入 loopback 缓冲区
      // 已经是 Interleaved 格式 (LRLRLR...)，直接拷贝即可
      UInt32 writePos = atomic_load (&gLoopbackWritePos);
      UInt32 totalSamples = frames * 2;

      for (UInt32 i = 0; i < totalSamples; i++)
	{
	  gLoopbackBuffer[writePos] = samples[i];
	  writePos = (writePos + 1)
		     % (sizeof (gLoopbackBuffer) / sizeof (gLoopbackBuffer[0]));
	}
      atomic_store (&gLoopbackWritePos, writePos);

      // [Freewheel] 推进时间轴
      // 这是最关键的一步：只有在这里，我们才认为时间真正前进了
      atomic_fetch_add_explicit (&gDevice_CurrentFrameCount, frames,
				 memory_order_release);
    }
  // 处理输入操作：从 loopback 缓冲区读取数据
  else if (inOperationID == kAudioServerPlugInIOOperationReadInput)
    {
      UInt32 readPos = atomic_load (&gLoopbackReadPos);
      UInt32 writePos = atomic_load (&gLoopbackWritePos);
      UInt32 bufferSize
	= sizeof (gLoopbackBuffer) / sizeof (gLoopbackBuffer[0]);

      // 计算可用数据量（采样点数）
      UInt32 available;
      if (writePos >= readPos)
	available = writePos - readPos;
      else
	available = bufferSize - readPos + writePos;

      UInt32 sampleCount = frames * 2;

      if (available >= sampleCount)
	{
	  for (UInt32 i = 0; i < sampleCount; i++)
	    {
	      samples[i] = gLoopbackBuffer[readPos];
	      readPos = (readPos + 1) % bufferSize;
	    }
	  atomic_store (&gLoopbackReadPos, readPos);
	}
      else
	{
	  // 数据不足，输出静音
	  memset (samples, 0, sampleCount * sizeof (Float32));
	}
    }

  return 0;
}

static Boolean
VirtualAudioDriver_HasProperty (AudioServerPlugInDriverRef __unused inDriver,
				AudioObjectID inObjectID,
				pid_t __unused inClientProcessID,
				const AudioObjectPropertyAddress *inAddress)
{
  switch (inObjectID)
    {
    case kObjectID_PlugIn:
      return (inAddress->mSelector == kAudioObjectPropertyBaseClass
	      || inAddress->mSelector == kAudioObjectPropertyClass
	      || inAddress->mSelector == kAudioPlugInPropertyDeviceList);
    case kObjectID_Device:
      // 设备支持的属性
      return (
	inAddress->mSelector == kAudioObjectPropertyBaseClass
	|| inAddress->mSelector == kAudioObjectPropertyClass
	|| inAddress->mSelector == kAudioDevicePropertyDeviceUID
	|| inAddress->mSelector == kAudioObjectPropertyName
	|| inAddress->mSelector == kAudioObjectPropertyManufacturer
	|| inAddress->mSelector == kAudioDevicePropertyStreams
	|| inAddress->mSelector == kAudioDevicePropertyStreamConfiguration
	|| inAddress->mSelector == kAudioDevicePropertyNominalSampleRate
	|| inAddress->mSelector == kAudioDevicePropertyIcon
	|| inAddress->mSelector == kAudioDevicePropertyTransportType
	|| inAddress->mSelector == kAudioDevicePropertyDeviceCanBeDefaultDevice
	|| inAddress->mSelector
	     == kAudioDevicePropertyDeviceCanBeDefaultSystemDevice
	|| inAddress->mSelector == kAudioDevicePropertyDeviceIsAlive
	|| inAddress->mSelector == kAudioDevicePropertyDeviceIsRunning
	|| inAddress->mSelector == kAudioDevicePropertyAppVolumes
	|| inAddress->mSelector == kAudioDevicePropertyAppClientList ||
	// 关键属性：设备类型识别、延迟、零时间戳周期
	inAddress->mSelector == kAudioDevicePropertyLatency
	|| inAddress->mSelector == kAudioDevicePropertySafetyOffset
	|| inAddress->mSelector == kAudioDevicePropertyZeroTimeStampPeriod);
    case kObjectID_Stream_Output:
    case kObjectID_Stream_Input:
      // 输入流和输出流都支持标准流属性
      return (
	inAddress->mSelector == kAudioObjectPropertyBaseClass
	|| inAddress->mSelector == kAudioObjectPropertyClass
	|| inAddress->mSelector == kAudioStreamPropertyDirection
	|| inAddress->mSelector == kAudioStreamPropertyIsActive
	|| inAddress->mSelector == kAudioStreamPropertyVirtualFormat
	|| inAddress->mSelector == kAudioStreamPropertyPhysicalFormat
	|| inAddress->mSelector == kAudioStreamPropertyAvailableVirtualFormats
	|| inAddress->mSelector == kAudioStreamPropertyAvailablePhysicalFormats
	|| inAddress->mSelector == kAudioStreamPropertyTerminalType
	|| inAddress->mSelector == kAudioStreamPropertyStartingChannel);
    default:
      break;
    }
  return false;
}

static OSStatus
VirtualAudioDriver_IsPropertySettable (
  AudioServerPlugInDriverRef __unused inDriver,
  AudioObjectID __unused inObjectID, pid_t __unused inClientProcessID,
  const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable)
{
  if (inAddress->mSelector == kAudioDevicePropertyAppVolumes)
    {
      *outIsSettable = true;
      return 0;
    }
  *outIsSettable = false;
  return 0;
}

static OSStatus
VirtualAudioDriver_GetPropertyDataSize (
  AudioServerPlugInDriverRef __unused inDriver, AudioObjectID inObjectID,
  pid_t __unused inClientProcessID, const AudioObjectPropertyAddress *inAddress,
  UInt32 __unused inQualifierDataSize, const void *__unused inQualifierData,
  UInt32 *outDataSize)
{
  // 检查自定义属性（仅对 Device 对象支持）
  if (inObjectID == kObjectID_Device)
    {
      if (inAddress->mSelector == kAudioDevicePropertyAppVolumes)
	{
	  *outDataSize = sizeof (AppVolumeTable);
	  return 0;
	}
      else if (inAddress->mSelector == kAudioDevicePropertyAppClientList)
	{
	  // 最大客户端数 * PID大小 + count字段
	  *outDataSize = sizeof (UInt32) + MAX_APP_ENTRIES * sizeof (pid_t);
	  return 0;
	}
    }

  if (inObjectID == kObjectID_Device
      && inAddress->mSelector == kAudioDevicePropertyStreams)
    {
      if (inAddress->mScope == kAudioObjectPropertyScopeGlobal)
	*outDataSize = sizeof (AudioObjectID) * 2;
      else
	*outDataSize = sizeof (AudioObjectID);
    }
  else if (inObjectID == kObjectID_Device
	   && inAddress->mSelector == kAudioDevicePropertyStreamConfiguration)
    {
      *outDataSize = sizeof (AudioBufferList)
		     + sizeof (AudioBuffer)
			 * 2; // Non-Interleaved: 2个缓冲区（每个声道一个）
    }
  else if (inObjectID == kObjectID_PlugIn
	   && inAddress->mSelector == kAudioPlugInPropertyDeviceList)
    {
      *outDataSize = sizeof (AudioObjectID); // 只有一个设备
    }
  else if (inAddress->mSelector == kAudioDevicePropertyDeviceUID
	   || inAddress->mSelector == kAudioObjectPropertyName
	   || inAddress->mSelector == kAudioObjectPropertyManufacturer)
    {
      *outDataSize = sizeof (CFStringRef);
    }
  else if (inObjectID == kObjectID_Device
	   && inAddress->mSelector == kAudioDevicePropertyIcon)
    {
      *outDataSize = sizeof (CFURLRef);
    }
  // 添加流对象属性大小（支持输入流和输出流）
  else if ((inObjectID == kObjectID_Stream_Output
	    || inObjectID == kObjectID_Stream_Input)
	   && (inAddress->mSelector
		 == kAudioStreamPropertyAvailableVirtualFormats
	       || inAddress->mSelector
		    == kAudioStreamPropertyAvailablePhysicalFormats))
    {
      *outDataSize = sizeof (AudioStreamRangedDescription);
    }
  else if ((inObjectID == kObjectID_Stream_Output
	    || inObjectID == kObjectID_Stream_Input)
	   && (inAddress->mSelector == kAudioStreamPropertyVirtualFormat
	       || inAddress->mSelector == kAudioStreamPropertyPhysicalFormat))
    {
      *outDataSize = sizeof (AudioStreamBasicDescription);
    }
  else if ((inObjectID == kObjectID_Stream_Output
	    || inObjectID == kObjectID_Stream_Input)
	   && (inAddress->mSelector == kAudioObjectPropertyBaseClass
	       || inAddress->mSelector == kAudioObjectPropertyClass))
    {
      *outDataSize = sizeof (AudioClassID);
    }
  else
    {
      *outDataSize = sizeof (UInt32);
    }
  return 0;
}

static OSStatus
VirtualAudioDriver_GetPropertyData (
  AudioServerPlugInDriverRef __unused inDriver, AudioObjectID inObjectID,
  pid_t __unused inClientProcessID, const AudioObjectPropertyAddress *inAddress,
  UInt32 __unused inQualifierDataSize, const void *__unused inQualifierData,
  UInt32 __unused inDataSize, UInt32 *outDataSize, void *outData)
{
  if (inObjectID == kObjectID_PlugIn)
    {
      if (inAddress->mSelector == kAudioObjectPropertyBaseClass)
	{
	  *((AudioClassID *) outData) = kAudioObjectClassID;
	  *outDataSize = sizeof (AudioClassID);
	}
      else if (inAddress->mSelector == kAudioObjectPropertyClass)
	{
	  *((AudioClassID *) outData) = kAudioPlugInClassID;
	  *outDataSize = sizeof (AudioClassID);
	}
      else if (inAddress->mSelector == kAudioPlugInPropertyDeviceList)
	{
	  *((AudioObjectID *) outData) = kObjectID_Device;
	  *outDataSize = sizeof (AudioObjectID);
	}
    }
  else if (inObjectID == kObjectID_Device)
    {
      switch (inAddress->mSelector)
	{
	case kAudioObjectPropertyBaseClass:
	  *((AudioClassID *) outData) = kAudioObjectClassID;
	  *outDataSize = sizeof (AudioClassID);
	  break;
	case kAudioObjectPropertyClass:
	  *((AudioClassID *) outData) = kAudioDeviceClassID;
	  *outDataSize = sizeof (AudioClassID);
	  break;
	case kAudioDevicePropertyDeviceUID:
	  *((CFStringRef *) outData) = CFSTR (kDevice_UID);
	  *outDataSize = sizeof (CFStringRef);
	  break;
	case kAudioObjectPropertyName:
	  *((CFStringRef *) outData) = CFSTR ("Virtual Audio Device");
	  *outDataSize = sizeof (CFStringRef);
	  break;
	case kAudioObjectPropertyManufacturer:
	  *((CFStringRef *) outData) = CFSTR ("Virtual Audio Driver");
	  *outDataSize = sizeof (CFStringRef);
	  break;
	case kAudioDevicePropertyStreams:
	  // 根据 Scope 返回正确的流
	  if (inAddress->mScope == kAudioObjectPropertyScopeOutput)
	    {
	      ((AudioObjectID *) outData)[0] = kObjectID_Stream_Output;
	      *outDataSize = sizeof (AudioObjectID);
	    }
	  else if (inAddress->mScope == kAudioObjectPropertyScopeInput)
	    {
	      ((AudioObjectID *) outData)[0] = kObjectID_Stream_Input;
	      *outDataSize = sizeof (AudioObjectID);
	    }
	  else
	    {
	      // Global scope: return both? CoreAudio usually asks for specific
	      // scope. If asked globally, return both.
	      ((AudioObjectID *) outData)[0] = kObjectID_Stream_Output;
	      ((AudioObjectID *) outData)[1] = kObjectID_Stream_Input;
	      *outDataSize = sizeof (AudioObjectID) * 2;
	    }
	  break;
	  case kAudioDevicePropertyStreamConfiguration: {
	    // 根据 Scope 返回正确的缓冲区配置
	    // Output Scope -> Output Buffer (2ch)
	    // Input Scope -> Input Buffer (2ch)

	    AudioBufferList *list = (AudioBufferList *) outData;
	    list->mNumberBuffers = 1;
	    AudioBuffer *buffer = &list->mBuffers[0];
	    buffer->mNumberChannels = 2; // 立体声
	    buffer->mDataByteSize = 1024 * 8;
	    buffer->mData = NULL;

	    // 注意：如果我们在这里不区分 Scope，Input 和 Output 都会得到一个
	    // Buffer 这对于 Device 来说通常是可以的，因为 CoreAudio 会根据
	    // Scope 来拿 但是如果 CoreAudio 用 Global Scope 问“总共有多少
	    // Buffer”，那我们应该返回 2 个？ 对于
	    // kAudioDevicePropertyStreamConfiguration，通常是 per-scope 的。

	    // 如果是 Input 或 Output Scope，我们只返回 1 个 buffer。
	    // 如果是 Global Scope，这可能意味着总配置？
	    // HAL 通常只在特定 Scope 下调用此属性。

	    *outDataSize = sizeof (AudioBufferList) + sizeof (AudioBuffer);
	  }
	  break;
	case kAudioDevicePropertyNominalSampleRate:
	  *((Float64 *) outData) = gDevice_SampleRate;
	  *outDataSize = sizeof (Float64);
	  break;
	case kAudioDevicePropertyDeviceIsAlive:
	  *((UInt32 *) outData) = 1;
	  *outDataSize = sizeof (UInt32);
	  break;
	case kAudioDevicePropertyDeviceIsRunning:
	  *((UInt32 *) outData) = (gDevice_IOIsRunning > 0) ? 1 : 0;
	  *outDataSize = sizeof (UInt32);
	  break;
	// 关键属性：设备延迟和同步
	case kAudioDevicePropertyLatency:
	  // 虚拟设备通常为 0 延迟
	  *((UInt32 *) outData) = 0;
	  *outDataSize = sizeof (UInt32);
	  break;
	case kAudioDevicePropertySafetyOffset:
	  // 增加安全偏移，容忍调度抖动
	  *((UInt32 *) outData) = 4096;
	  *outDataSize = sizeof (UInt32);
	  break;
	case kAudioDevicePropertyZeroTimeStampPeriod:
	  // 零时间戳周期：改为 512 帧短周期 (约 10.67ms @ 48kHz)
	  // 消除音频"慢放感"，提高实时响应性
	  *((UInt32 *) outData) = kZeroTimeStampPeriod;
	  *outDataSize = sizeof (UInt32);
	  break;
	  case kAudioDevicePropertyIcon: {
	    // 获取插件 bundle 的路径
	    CFStringRef iconPath
	      = CFSTR ("/Library/Audio/Plug-Ins/HAL/VirtualAudioDriver.driver/"
		       "Contents/Resources/DeviceIcon.icns");
	    CFURLRef iconURL
	      = CFURLCreateWithFileSystemPath (NULL, iconPath,
					       kCFURLPOSIXPathStyle, false);
	    *((CFURLRef *) outData) = iconURL;
	    *outDataSize = sizeof (CFURLRef);
	    break;
	  }
	case kAudioDevicePropertyTransportType:
	  // 设置设备类型为 Virtual，这样 Sound 设置中会显示 "Virtual" 类型
	  *((UInt32 *) outData) = kAudioDeviceTransportTypeVirtual;
	  *outDataSize = sizeof (UInt32);
	  break;
	case kAudioDevicePropertyDeviceCanBeDefaultDevice:
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
	  // 可以作为默认/系统默认设备
	  *((UInt32 *) outData) = 1;
	  *outDataSize = sizeof (UInt32);
	  break;
	case kAudioDevicePropertyAppVolumes:
	  if (*outDataSize >= sizeof (AppVolumeTable))
	    {
	      app_volume_driver_get_table ((AppVolumeTable *) outData);
	      *outDataSize = sizeof (AppVolumeTable);
	    }
	  break;
	  case kAudioDevicePropertyAppClientList: {
	    // 返回当前连接的客户端PID列表
	    // 数据格式: UInt32 count + pid_t pids[]
	    // 即使客户端列表为空，也返回 count=0 和空列表
	    UInt32 minSize = sizeof (UInt32);

	    if (*outDataSize < minSize)
	      {
		// 缓冲区太小
		*outDataSize = minSize;
		break;
	      }

	    // 计算最多能返回多少 PID
	    UInt32 availableSpace = *outDataSize - sizeof (UInt32);
	    UInt32 maxPids = availableSpace / sizeof (pid_t);

	    pid_t *pids = (pid_t *) ((UInt8 *) outData + sizeof (UInt32));
	    UInt32 actualCount = 0;

	    app_volume_driver_get_client_pids (pids, maxPids, &actualCount);

	    *(UInt32 *) outData = actualCount;
	    *outDataSize = sizeof (UInt32) + actualCount * sizeof (pid_t);
	  }
	  break;
	default:
	  break;
	}
    }
  else if (inObjectID == kObjectID_Stream_Output
	   || inObjectID == kObjectID_Stream_Input)
    {
      // 判断是输入流还是输出流
      bool isInput = (inObjectID == kObjectID_Stream_Input);

      switch (inAddress->mSelector)
	{
	case kAudioObjectPropertyBaseClass:
	  *((AudioClassID *) outData) = kAudioObjectClassID;
	  *outDataSize = sizeof (AudioClassID);
	  break;
	case kAudioObjectPropertyClass:
	  *((AudioClassID *) outData) = kAudioStreamClassID;
	  *outDataSize = sizeof (AudioClassID);
	  break;
	case kAudioStreamPropertyDirection:
	  // 0 = 输出 (output), 1 = 输入 (input)
	  *((UInt32 *) outData) = isInput ? 1 : 0;
	  *outDataSize = sizeof (UInt32);
	  break;
	case kAudioStreamPropertyIsActive:
	  *((UInt32 *) outData) = 1;
	  *outDataSize = sizeof (UInt32);
	  break;
	// 强制使用 Interleaved 格式，消除 AudioConverter 错误
	case kAudioStreamPropertyVirtualFormat:
	  case kAudioStreamPropertyPhysicalFormat: {
	    AudioStreamBasicDescription *format
	      = (AudioStreamBasicDescription *) outData;
	    format->mSampleRate = 48000.0;
	    format->mFormatID = kAudioFormatLinearPCM;
	    // 移除 NonInterleaved 标志，改为标准的交错浮点
	    format->mFormatFlags
	      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	    format->mBytesPerPacket = 8; // 2ch * 4bytes
	    format->mFramesPerPacket = 1;
	    format->mBytesPerFrame = 8; // 2ch * 4bytes
	    format->mChannelsPerFrame = 2;
	    format->mBitsPerChannel = 32;
	    format->mReserved = 0;
	    *outDataSize = sizeof (AudioStreamBasicDescription);
	  }
	  break;
	case kAudioStreamPropertyAvailableVirtualFormats:
	  case kAudioStreamPropertyAvailablePhysicalFormats: {
	    AudioStreamRangedDescription *format
	      = (AudioStreamRangedDescription *) outData;
	    format->mFormat.mSampleRate = 48000.0;
	    format->mFormat.mFormatID = kAudioFormatLinearPCM;
	    // 移除 NonInterleaved 标志
	    format->mFormat.mFormatFlags
	      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	    format->mFormat.mBytesPerPacket = 8;
	    format->mFormat.mFramesPerPacket = 1;
	    format->mFormat.mBytesPerFrame = 8;
	    format->mFormat.mChannelsPerFrame = 2;
	    format->mFormat.mBitsPerChannel = 32;
	    format->mFormat.mReserved = 0;
	    format->mSampleRateRange.mMinimum = 48000.0;
	    format->mSampleRateRange.mMaximum = 48000.0;
	    *outDataSize = sizeof (AudioStreamRangedDescription);
	  }
	  break;
	case kAudioStreamPropertyTerminalType:
	  *((UInt32 *) outData) = isInput ? kAudioStreamTerminalTypeMicrophone
					  : kAudioStreamTerminalTypeSpeaker;
	  *outDataSize = sizeof (UInt32);
	  break;
	case kAudioStreamPropertyStartingChannel:
	  *((UInt32 *) outData) = 1;
	  *outDataSize = sizeof (UInt32);
	  break;
	default:
	  break;
	}
    }
  return 0;
}

static OSStatus
VirtualAudioDriver_SetPropertyData (
  AudioServerPlugInDriverRef __unused inDriver,
  AudioObjectID __unused inObjectID, pid_t __unused inClientProcessID,
  const AudioObjectPropertyAddress *__unused inAddress,
  UInt32 __unused inQualifierDataSize, const void *__unused inQualifierData,
  UInt32 __unused inDataSize, const void *__unused inData)
{
  return 0;
}

static OSStatus
VirtualAudioDriver_CreateDevice (AudioServerPlugInDriverRef __unused inDriver,
				 CFDictionaryRef __unused inDescription,
				 const AudioServerPlugInClientInfo *__unused
				   inClientInfo,
				 AudioObjectID *__unused outDeviceObjectID)
{
  return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
VirtualAudioDriver_DestroyDevice (AudioServerPlugInDriverRef __unused inDriver,
				  AudioObjectID __unused inDeviceObjectID)
{
  return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
VirtualAudioDriver_AddDeviceClient (
  AudioServerPlugInDriverRef __unused inDriver,
  AudioObjectID __unused inDeviceObjectID,
  const AudioServerPlugInClientInfo *inClientInfo)
{
  if (inClientInfo)
    {
      // Register with local driver
      app_volume_driver_add_client (inClientInfo->mClientID,
				    inClientInfo->mProcessID, NULL, NULL);

      // This will be implemented in the new IPC architecture
    }
  return 0;
}

static OSStatus
VirtualAudioDriver_RemoveDeviceClient (
  AudioServerPlugInDriverRef __unused inDriver,
  AudioObjectID __unused inDeviceObjectID,
  const AudioServerPlugInClientInfo *inClientInfo)
{
  if (inClientInfo)
    {
      // Unregister from local driver
      app_volume_driver_remove_client (inClientInfo->mClientID);

      // This will be implemented in the new IPC architecture
    }
  return 0;
}

static OSStatus
VirtualAudioDriver_PerformDeviceConfigurationChange (
  AudioServerPlugInDriverRef __unused inDriver,
  AudioObjectID __unused inDeviceObjectID, UInt64 __unused inChangeAction,
  void *__unused inChangeInfo)
{
  return 0;
}

static OSStatus
VirtualAudioDriver_AbortDeviceConfigurationChange (
  AudioServerPlugInDriverRef __unused inDriver,
  AudioObjectID __unused inDeviceObjectID, UInt64 __unused inChangeAction,
  void *__unused inChangeInfo)
{
  return 0;
}

static OSStatus
VirtualAudioDriver_BeginIOOperation (
  AudioServerPlugInDriverRef __unused inDriver,
  AudioObjectID __unused inDeviceObjectID, UInt32 __unused inClientID,
  UInt32 __unused inOperationID, UInt32 __unused inIOBufferFrameSize,
  const AudioServerPlugInIOCycleInfo *__unused inIOCycleInfo)
{
  return 0;
}

static OSStatus
VirtualAudioDriver_EndIOOperation (
  AudioServerPlugInDriverRef __unused inDriver,
  AudioObjectID __unused inDeviceObjectID, UInt32 __unused inClientID,
  UInt32 __unused inOperationID, UInt32 __unused inIOBufferFrameSize,
  const AudioServerPlugInIOCycleInfo *__unused inIOCycleInfo)
{
  return 0;
}

#pragma mark - Helper Functions

// Helper functions can be added here
