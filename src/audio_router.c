//
// Audio Router - 串联架构核心实现
// Virtual Device -> Ring Buffer -> Physical Device
// Created by AhogeK on 02/12/26.
//

#include "audio_router.h"
#include <CoreAudio/CoreAudio.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "virtual_device_manager.h"

static AudioRouterContext g_router = {0};

// ====== 环形缓冲区实现 (Lock-Free) ======

static void
rb_init (RouterRingBuffer *rb, uint32_t capacity_frames, uint32_t channels)
{
  // Prevent division by zero: ensure capacity > 0
  if (capacity_frames == 0 || channels == 0)
    {
      fprintf (
	stderr,
	"[AudioRouter] Error: Invalid ring buffer params: frames=%u, ch=%u\n",
	capacity_frames, channels);
      rb->capacity = 0;
      rb->buffer = NULL;
      return;
    }

  // Prevent multiplication overflow
  uint64_t total_size = (uint64_t) capacity_frames * (uint64_t) channels;
  if (total_size > UINT32_MAX)
    {
      fprintf (stderr, "[AudioRouter] Error: Ring buffer size overflow\n");
      rb->capacity = 0;
      rb->buffer = NULL;
      return;
    }

  rb->capacity = (uint32_t) total_size;
  rb->buffer = (float *) calloc (rb->capacity, sizeof (float));
  atomic_init (&rb->write_pos, 0);
  atomic_init (&rb->read_pos, 0);
}

static void
rb_destroy (RouterRingBuffer *rb)
{
  if (rb->buffer)
    {
      free (rb->buffer);
      rb->buffer = NULL;
    }
}

// Write data (called by input callback - Producer)
static void
rb_write (RouterRingBuffer *rb, const float *data, uint32_t frame_count,
	  uint32_t channels)
{
  // Check if buffer is valid and initialized
  if (rb == NULL || rb->buffer == NULL || rb->capacity == 0 || data == NULL)
    {
      return;
    }

  uint32_t sample_count = frame_count * channels;
  uint32_t current_write
    = atomic_load_explicit (&rb->write_pos, memory_order_relaxed);
  uint32_t current_read
    = atomic_load_explicit (&rb->read_pos, memory_order_acquire);

  // Calculate available space (capacity - 1 to distinguish full from empty)
  uint32_t size = (current_write >= current_read)
		    ? (current_write - current_read)
		    : (rb->capacity - current_read + current_write);
  uint32_t free_space = rb->capacity - 1 - size;

  if (free_space < sample_count)
    {
      g_router.overrun_count++;
      // Strategy: discard new data to maintain sync
      return;
    }

  for (uint32_t i = 0; i < sample_count; i++)
    {
      rb->buffer[current_write] = data[i];
      current_write = (current_write + 1) % rb->capacity;
    }

  atomic_store_explicit (&rb->write_pos, current_write, memory_order_release);
}

// Read data (called by output callback - Consumer)
static void
rb_read (RouterRingBuffer *rb, float *data, uint32_t frame_count,
	 uint32_t channels)
{
  // Check if buffer is valid and initialized
  if (rb == NULL || rb->buffer == NULL || rb->capacity == 0 || data == NULL)
    {
      return;
    }

  uint32_t sample_count = frame_count * channels;
  uint32_t current_read
    = atomic_load_explicit (&rb->read_pos, memory_order_relaxed);
  uint32_t current_write
    = atomic_load_explicit (&rb->write_pos, memory_order_acquire);

  uint32_t available = (current_write >= current_read)
			 ? (current_write - current_read)
			 : (rb->capacity - current_read + current_write);

  if (available < sample_count)
    {
      g_router.underrun_count++;
      // Not enough data, output silence
      memset (data, 0, sample_count * sizeof (float));
      return;
    }

  for (uint32_t i = 0; i < sample_count; i++)
    {
      data[i] = rb->buffer[current_read];
      current_read = (current_read + 1) % rb->capacity;
    }

  atomic_store_explicit (&rb->read_pos, current_read, memory_order_release);
}

// ====== IO 回调函数 ======

// 输入回调：从虚拟设备读取数据 -> 存入 RingBuffer
static OSStatus
input_callback (AudioDeviceID inDevice, const AudioTimeStamp *inNow,
		const AudioBufferList *inInputData,
		const AudioTimeStamp *inInputTime,
		AudioBufferList *outOutputData,
		const AudioTimeStamp *inOutputTime, void *inClientData)
{
  (void) inDevice;
  (void) inNow;
  (void) inInputTime;
  (void) outOutputData;
  (void) inOutputTime;
  (void) inClientData;

  if (!g_router.is_running || inInputData->mNumberBuffers == 0)
    {
      return noErr;
    }

  const AudioBuffer *inputBuffer = &inInputData->mBuffers[0];
  if (inputBuffer->mDataByteSize == 0 || inputBuffer->mData == NULL)
    {
      return noErr;
    }

  const float *src = (const float *) inputBuffer->mData;
  uint32_t frames
    = inputBuffer->mDataByteSize / (sizeof (float) * g_router.channels);

  rb_write (&g_router.ring_buffer, src, frames, g_router.channels);
  g_router.frames_transferred += frames;

  return noErr;
}

// 输出回调：从 RingBuffer 取出数据 -> 写入物理设备
static OSStatus
output_callback (AudioDeviceID inDevice, const AudioTimeStamp *inNow,
		 const AudioBufferList *inInputData,
		 const AudioTimeStamp *inInputTime,
		 AudioBufferList *outOutputData,
		 const AudioTimeStamp *inOutputTime, void *inClientData)
{
  (void) inDevice;
  (void) inNow;
  (void) inInputData;
  (void) inInputTime;
  (void) inOutputTime;
  (void) inClientData;

  if (!g_router.is_running || outOutputData->mNumberBuffers == 0)
    {
      return noErr;
    }

  AudioBuffer *outputBuffer = &outOutputData->mBuffers[0];
  if (outputBuffer->mDataByteSize == 0 || outputBuffer->mData == NULL)
    {
      return noErr;
    }

  float *dst = (float *) outputBuffer->mData;
  uint32_t frames
    = outputBuffer->mDataByteSize / (sizeof (float) * g_router.channels);

  rb_read (&g_router.ring_buffer, dst, frames, g_router.channels);

  return noErr;
}

// ====== 设备查找 ======

static AudioDeviceID
find_device_by_uid (const char *uid)
{
  AudioObjectPropertyAddress addr
    = {kAudioHardwarePropertyTranslateUIDToDevice,
       kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

  CFStringRef uidRef
    = CFStringCreateWithCString (NULL, uid, kCFStringEncodingUTF8);
  AudioDeviceID deviceID = kAudioObjectUnknown;
  UInt32 size = sizeof (AudioDeviceID);

  OSStatus status = AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr,
						sizeof (CFStringRef), &uidRef,
						&size, &deviceID);
  CFRelease (uidRef);

  if (status != noErr || deviceID == kAudioObjectUnknown)
    {
      return kAudioObjectUnknown;
    }

  return deviceID;
}

static bool
get_device_sample_rate (AudioDeviceID device, uint32_t *sample_rate)
{
  AudioObjectPropertyAddress addr
    = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
       kAudioObjectPropertyElementMain};

  Float64 rate = 0;
  UInt32 size = sizeof (rate);
  OSStatus status
    = AudioObjectGetPropertyData (device, &addr, 0, NULL, &size, &rate);

  if (status == noErr)
    {
      *sample_rate = (uint32_t) rate;
      return true;
    }

  return false;
}

// ====== 公共 API ======

OSStatus
audio_router_start (const char *physical_device_uid)
{
  if (g_router.is_running)
    {
      printf ("Router 已在运行\n");
      return noErr;
    }

  printf ("🔄 启动 Audio Router...\n");
  printf ("   物理设备 UID: %s\n", physical_device_uid);

  // Get virtual device
  VirtualDeviceInfo vInfo;
  if (!virtual_device_get_info (&vInfo))
    {
      fprintf (stderr, "❌ 未找到虚拟设备\n");
      return kAudioHardwareNotRunningError;
    }
  g_router.input_device = vInfo.deviceId;

  // Get physical device
  g_router.output_device = find_device_by_uid (physical_device_uid);
  if (g_router.output_device == kAudioObjectUnknown)
    {
      fprintf (stderr, "❌ 无法找到物理设备: %s\n", physical_device_uid);
      return kAudioHardwareBadDeviceError;
    }

  // Get audio format info
  uint32_t virtual_rate = 0;
  uint32_t physical_rate = 0;
  if (!get_device_sample_rate (g_router.input_device, &virtual_rate))
    {
      fprintf (stderr, "⚠️ 无法获取虚拟设备采样率，使用默认 48000\n");
      virtual_rate = 48000;
    }
  if (!get_device_sample_rate (g_router.output_device, &physical_rate))
    {
      fprintf (stderr, "⚠️ 无法获取物理设备采样率，使用默认 48000\n");
      physical_rate = 48000;
    }

  // Check sample rate match
  if (virtual_rate != physical_rate)
    {
      fprintf (stderr, "⚠️ 采样率不匹配: 虚拟设备=%u, 物理设备=%u\n",
	       virtual_rate, physical_rate);
      fprintf (stderr, "   这可能导致音频问题\n");
    }

  g_router.sample_rate = virtual_rate;
  g_router.channels = 2;	  // Assume stereo
  g_router.bits_per_channel = 32; // Float32

  // Initialize Ring Buffer
  rb_init (&g_router.ring_buffer, ROUTER_BUFFER_FRAME_COUNT, g_router.channels);

  // Reset statistics
  g_router.frames_transferred = 0;
  g_router.underrun_count = 0;
  g_router.overrun_count = 0;

  // Create IO Proc
  OSStatus status
    = AudioDeviceCreateIOProcID (g_router.input_device, &input_callback, NULL,
				 &g_router.input_proc_id);
  if (status != noErr)
    {
      fprintf (stderr, "❌ 创建输入 IOProc 失败: %d\n", status);
      rb_destroy (&g_router.ring_buffer);
      return status;
    }

  status = AudioDeviceCreateIOProcID (g_router.output_device, &output_callback,
				      NULL, &g_router.output_proc_id);
  if (status != noErr)
    {
      fprintf (stderr, "❌ 创建输出 IOProc 失败: %d\n", status);
      AudioDeviceDestroyIOProcID (g_router.input_device,
				  g_router.input_proc_id);
      rb_destroy (&g_router.ring_buffer);
      return status;
    }

  // Start IO
  status = AudioDeviceStart (g_router.input_device, g_router.input_proc_id);
  if (status != noErr)
    {
      fprintf (stderr, "❌ 启动输入设备失败: %d\n", status);
      goto cleanup;
    }

  // 等待一点数据积累
  struct timespec accum_ts = {0, 5000000}; // 5ms
  nanosleep (&accum_ts, NULL);

  status = AudioDeviceStart (g_router.output_device, g_router.output_proc_id);
  if (status != noErr)
    {
      fprintf (stderr, "❌ 启动输出设备失败: %d\n", status);
      AudioDeviceStop (g_router.input_device, g_router.input_proc_id);
      goto cleanup;
    }

  g_router.is_running = true;
  printf ("✅ Router 已启动\n");
  printf ("   音频流: Virtual Device -> Ring Buffer -> Physical Device\n");
  printf ("   采样率: %u Hz, 通道: %u\n", g_router.sample_rate,
	  g_router.channels);

  return noErr;

cleanup:
  AudioDeviceDestroyIOProcID (g_router.input_device, g_router.input_proc_id);
  AudioDeviceDestroyIOProcID (g_router.output_device, g_router.output_proc_id);
  rb_destroy (&g_router.ring_buffer);
  return status;
}

void
audio_router_stop (void)
{
  if (!g_router.is_running)
    {
      return;
    }

  printf ("⏹️  停止 Audio Router...\n");

  g_router.is_running = false;

  // 停止 IO
  AudioDeviceStop (g_router.output_device, g_router.output_proc_id);
  AudioDeviceStop (g_router.input_device, g_router.input_proc_id);

  // 销毁 IO Proc
  AudioDeviceDestroyIOProcID (g_router.output_device, g_router.output_proc_id);
  AudioDeviceDestroyIOProcID (g_router.input_device, g_router.input_proc_id);

  // 销毁 Ring Buffer
  rb_destroy (&g_router.ring_buffer);

  printf ("✅ Router 已停止\n");
}

bool
audio_router_is_running (void)
{
  return g_router.is_running;
}

bool
audio_router_get_physical_device_uid (char *uid, size_t size)
{
  if (!g_router.is_running || g_router.output_device == kAudioObjectUnknown)
    {
      return false;
    }

  AudioObjectPropertyAddress addr
    = {kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
       kAudioObjectPropertyElementMain};

  CFStringRef uidRef = NULL;
  UInt32 dataSize = sizeof (CFStringRef);
  OSStatus status = AudioObjectGetPropertyData (g_router.output_device, &addr,
						0, NULL, &dataSize, &uidRef);

  if (status != noErr || uidRef == NULL)
    {
      return false;
    }

  CFStringGetCString (uidRef, uid, (CFIndex) size, kCFStringEncodingUTF8);
  CFRelease (uidRef);

  return true;
}

void
audio_router_get_stats (uint64_t *frames_transferred, uint32_t *underruns,
			uint32_t *overruns)
{
  if (frames_transferred)
    *frames_transferred = g_router.frames_transferred;
  if (underruns)
    *underruns = g_router.underrun_count;
  if (overruns)
    *overruns = g_router.overrun_count;
}
