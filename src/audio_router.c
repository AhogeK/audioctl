//
// Audio Router - 串联架构核心实现
// Virtual Device -> Ring Buffer -> Physical Device
// Created by AhogeK on 02/12/26.
// Optimized for low latency (42ms) with Watermark monitoring
//

#include "audio_router.h"
#include <CoreAudio/CoreAudio.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/syslog.h>
#include <time.h>
#include <unistd.h>
#include "virtual_device_manager.h"

// 日志对象
static os_log_t g_router_log = NULL;

// 控制台日志模式标志（当用户在终端直接运行 internal-route 时使用）
static bool g_console_log_mode = false;

// 统一日志宏：控制台模式用 printf，后台模式用 os_log
#define ROUTER_LOG_INFO(format, ...)                                           \
  do                                                                           \
    {                                                                          \
      if (g_console_log_mode)                                                  \
	{                                                                      \
	  printf (format "\n", ##__VA_ARGS__);                                 \
	}                                                                      \
      else if (g_router_log != NULL)                                           \
	{                                                                      \
	  syslog (LOG_NOTICE, "[Router] " format, ##__VA_ARGS__);              \
	}                                                                      \
  } while (0)

static AudioRouterContext g_router = {0};
static pthread_t g_monitor_thread = 0;
static volatile int g_monitor_running = 0;

// 设置控制台日志模式
void
audio_router_set_console_log_mode (bool enable)
{
  g_console_log_mode = enable;
}

// Watermark 监控间隔 (秒)
#define MONITOR_INTERVAL_SEC 5
// 总采样数 (帧数 * 通道数)
#define TOTAL_SAMPLES (ROUTER_BUFFER_FRAME_COUNT * ROUTER_MAX_CHANNELS)

// ====== 性能监控辅助函数 ======

// 获取当前时间戳 (微秒)
static uint64_t
get_time_us (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return (uint64_t) tv.tv_sec * 1000000ULL + (uint64_t) tv.tv_usec;
}

// 计算延迟 (毫秒)
static uint32_t
calculate_latency_ms (uint32_t buffered_frames, uint32_t sample_rate)
{
  if (sample_rate == 0)
    return 0;
  return (buffered_frames * 1000) / sample_rate;
}

// ====== 环形缓冲区实现 (Lock-Free + Bitmask优化) ======

static void
rb_init (RouterRingBuffer *rb)
{
  // 使用固定大小，必须是2的幂次方以便位掩码
  rb->capacity = TOTAL_SAMPLES;
  rb->buffer = (float *) aligned_alloc (64, rb->capacity * sizeof (float));
  if (!rb->buffer)
    {
      fprintf (stderr, "[AudioRouter] Error: Failed to allocate ring buffer\n");
      rb->capacity = 0;
      return;
    }
  // 清零缓冲区
  memset (rb->buffer, 0, rb->capacity * sizeof (float));
  atomic_init (&rb->write_pos, 0);
  atomic_init (&rb->read_pos, 0);
  atomic_init (&rb->peak_usage, 0);
  atomic_init (&rb->current_usage, 0);
  atomic_init (&rb->samples_buffered, 0);
}

static void
rb_destroy (RouterRingBuffer *rb)
{
  if (rb->buffer)
    {
      free (rb->buffer);
      rb->buffer = NULL;
      rb->capacity = 0;
    }
}

// 更新性能统计
static inline void
rb_update_stats (RouterRingBuffer *rb, uint32_t buffered_samples)
{
  // 计算当前使用率 (0-100%)
  uint32_t usage_percent = (buffered_samples * 100) / rb->capacity;
  atomic_store_explicit (&rb->current_usage, usage_percent,
			 memory_order_relaxed);
  atomic_store_explicit (&rb->samples_buffered, buffered_samples,
			 memory_order_relaxed);
  // 更新峰值
  uint32_t peak = atomic_load_explicit (&rb->peak_usage, memory_order_relaxed);
  if (usage_percent > peak)
    {
      atomic_store_explicit (&rb->peak_usage, usage_percent,
			     memory_order_relaxed);
    }
}

// Write data (called by input callback - Producer)
// 使用位掩码替代取模运算，速度提升10-20倍
static void
rb_write (RouterRingBuffer *rb, const float *data, uint32_t frame_count,
	  uint32_t channels)
{
  // Check if buffer is valid and initialized
  if (rb == NULL || rb->buffer == NULL || data == NULL)
    {
      return;
    }

  uint32_t sample_count = frame_count * channels;
  uint32_t current_write
    = atomic_load_explicit (&rb->write_pos, memory_order_relaxed);
  uint32_t current_read
    = atomic_load_explicit (&rb->read_pos, memory_order_acquire);

  // 计算已用空间和空闲空间
  uint32_t size;
  if (current_write >= current_read)
    size = current_write - current_read;
  else
    size = rb->capacity - current_read + current_write;

  // capacity - 1 是为了区分满和空
  uint32_t free_space = rb->capacity - 1 - size;

  if (free_space < sample_count)
    {
      atomic_fetch_add_explicit (&g_router.overrun_count, 1,
				 memory_order_relaxed);
      // 策略：丢弃新数据以保持同步
      return;
    }

  // 使用位掩码替代取模运算 - 关键优化！
  for (uint32_t i = 0; i < sample_count; i++)
    {
      rb->buffer[current_write & ROUTER_BUFFER_MASK] = data[i];
      current_write++;
    }

  atomic_store_explicit (&rb->write_pos, current_write, memory_order_release);

  // 更新性能统计
  rb_update_stats (rb, size + sample_count);
}

// Read data (called by output callback - Consumer)
// 使用位掩码替代取模运算
static void
rb_read (RouterRingBuffer *rb, float *data, uint32_t frame_count,
	 uint32_t channels)
{
  // Check if buffer is valid and initialized
  if (rb == NULL || rb->buffer == NULL || data == NULL)
    {
      return;
    }

  uint32_t sample_count = frame_count * channels;
  uint32_t current_read
    = atomic_load_explicit (&rb->read_pos, memory_order_relaxed);
  uint32_t current_write
    = atomic_load_explicit (&rb->write_pos, memory_order_acquire);

  // 计算可用数据量
  uint32_t available;
  if (current_write >= current_read)
    available = current_write - current_read;
  else
    available = rb->capacity - current_read + current_write;

  if (available < sample_count)
    {
      atomic_fetch_add_explicit (&g_router.underrun_count, 1,
				 memory_order_relaxed);
      // 数据不足，输出静音
      memset (data, 0, sample_count * sizeof (float));

      // 更新统计
      rb_update_stats (rb, available);
      return;
    }

  // 使用位掩码替代取模运算 - 关键优化！
  for (uint32_t i = 0; i < sample_count; i++)
    {
      data[i] = rb->buffer[current_read & ROUTER_BUFFER_MASK];
      current_read++;
    }

  atomic_store_explicit (&rb->read_pos, current_read, memory_order_release);

  // 更新性能统计
  rb_update_stats (rb, available - sample_count);
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

// 前向声明
static void
start_monitor_thread (void);
static void
stop_monitor_thread (void);

// ====== 公共 API ======

OSStatus
audio_router_start (const char *physical_device_uid)
{
  // 初始化日志系统（如果不存在）
  if (g_router_log == NULL)
    {
      g_router_log = os_log_create ("com.ahogek.audioctl", "Router");
    }

  if (g_router.is_running)
    {
      ROUTER_LOG_INFO ("Router 已在运行");
      return noErr;
    }

  ROUTER_LOG_INFO ("🔄 启动 Audio Router...");
  ROUTER_LOG_INFO ("物理设备 UID: %s", physical_device_uid);

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
  rb_init (&g_router.ring_buffer);

  // Reset statistics (使用原子操作)
  atomic_store_explicit (&g_router.frames_transferred, 0, memory_order_relaxed);
  atomic_store_explicit (&g_router.underrun_count, 0, memory_order_relaxed);
  atomic_store_explicit (&g_router.overrun_count, 0, memory_order_relaxed);

  // 记录启动时间
  g_router.start_time = get_time_us ();

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

  // 启动监控线程
  start_monitor_thread ();

  ROUTER_LOG_INFO ("✅ Router 已启动");
  ROUTER_LOG_INFO ("音频流: Virtual Device -> Ring Buffer -> Physical Device");
  ROUTER_LOG_INFO ("采样率: %u Hz, 通道: %u", g_router.sample_rate,
		   g_router.channels);
  ROUTER_LOG_INFO ("缓冲区: %u 帧 (约 %u ms)", ROUTER_BUFFER_FRAME_COUNT,
		   (ROUTER_BUFFER_FRAME_COUNT * 1000) / g_router.sample_rate);
  ROUTER_LOG_INFO ("监控: 每 %d 秒报告一次性能状态", MONITOR_INTERVAL_SEC);

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

  ROUTER_LOG_INFO ("⏹️  停止 Audio Router...");

  g_router.is_running = false;

  // 停止监控线程
  stop_monitor_thread ();

  // 停止 IO
  AudioDeviceStop (g_router.output_device, g_router.output_proc_id);
  AudioDeviceStop (g_router.input_device, g_router.input_proc_id);

  // 销毁 IO Proc
  AudioDeviceDestroyIOProcID (g_router.output_device, g_router.output_proc_id);
  AudioDeviceDestroyIOProcID (g_router.input_device, g_router.input_proc_id);

  // 销毁 Ring Buffer
  rb_destroy (&g_router.ring_buffer);

  ROUTER_LOG_INFO ("✅ Router 已停止");
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
    *frames_transferred = atomic_load_explicit (&g_router.frames_transferred,
						memory_order_relaxed);
  if (underruns)
    *underruns
      = atomic_load_explicit (&g_router.underrun_count, memory_order_relaxed);
  if (overruns)
    *overruns
      = atomic_load_explicit (&g_router.overrun_count, memory_order_relaxed);
}

// ====== 性能监控线程 ======

static void *
monitor_thread_func (void *arg)
{
  (void) arg;

  ROUTER_LOG_INFO ("[Router Monitor] 监控线程启动");

  uint32_t last_underruns = 0;
  uint32_t last_overruns = 0;
  uint64_t last_frames = 0;

  while (g_monitor_running && g_router.is_running)
    {
      sleep (MONITOR_INTERVAL_SEC);

      // 注: 循环条件已检查 g_router.is_running，这里不需要额外检查

      // 获取当前统计
      uint32_t current_underruns
	= atomic_load_explicit (&g_router.underrun_count, memory_order_relaxed);
      uint32_t current_overruns
	= atomic_load_explicit (&g_router.overrun_count, memory_order_relaxed);
      uint64_t current_frames
	= atomic_load_explicit (&g_router.frames_transferred,
				memory_order_relaxed);

      // 计算增量
      uint32_t underrun_delta = current_underruns - last_underruns;
      uint32_t overrun_delta = current_overruns - last_overruns;
      uint64_t frames_delta = current_frames - last_frames;

      // 获取 Watermark
      uint32_t current_usage
	= atomic_load_explicit (&g_router.ring_buffer.current_usage,
				memory_order_relaxed);
      uint32_t peak_usage
	= atomic_load_explicit (&g_router.ring_buffer.peak_usage,
				memory_order_relaxed);
      uint32_t samples_buffered
	= atomic_load_explicit (&g_router.ring_buffer.samples_buffered,
				memory_order_relaxed);

      // 计算延迟 (毫秒)
      uint32_t buffered_frames
	= samples_buffered / g_router.channels; // 转换为帧数
      uint32_t latency_ms
	= calculate_latency_ms (buffered_frames, g_router.sample_rate);

      // 计算运行时间
      uint64_t elapsed_us = get_time_us () - g_router.start_time;
      uint32_t elapsed_sec = (uint32_t) (elapsed_us / 1000000);

      // 输出到系统日志
      if (underrun_delta > 0 || overrun_delta > 0)
	{
	  syslog (LOG_ERR,
		  "[Router Monitor] %02u:%02u | 延迟:%ums | "
		  "缓冲:%u%% | 峰值:%u%% | 传输:%llu | "
		  "Underrun:%u | Overrun:%u",
		  elapsed_sec / 60, elapsed_sec % 60, latency_ms, current_usage,
		  peak_usage, frames_delta, underrun_delta, overrun_delta);
	}
      else
	{
	  ROUTER_LOG_INFO ("[Router Monitor] %02u:%02u | 延迟:%ums | "
			   "缓冲:%u%% | 峰值:%u%% | 传输:%llu | 状态:健康",
			   elapsed_sec / 60, elapsed_sec % 60, latency_ms,
			   current_usage, peak_usage,
			   (unsigned long long) frames_delta);
	}

      // 更新上次记录
      last_underruns = current_underruns;
      last_overruns = current_overruns;
      last_frames = current_frames;
    }

  ROUTER_LOG_INFO ("[Router Monitor] 监控线程停止");
  return NULL;
}

// 启动监控线程
static void
start_monitor_thread (void)
{
  g_monitor_running = 1;
  if (pthread_create (&g_monitor_thread, NULL, monitor_thread_func, NULL) != 0)
    {
      fprintf (stderr, "[AudioRouter] Warning: 无法创建监控线程\n");
      g_monitor_running = 0;
    }
}

// 停止监控线程
static void
stop_monitor_thread (void)
{
  g_monitor_running = 0;
  if (g_monitor_thread != 0)
    {
      pthread_join (g_monitor_thread, NULL);
      g_monitor_thread = 0;
    }
}

// ====== 公共 API 实现 ======

bool
audio_router_get_performance_info (uint32_t *latency_ms, float *watermark_peak,
				   uint32_t *buffered_frames)
{
  if (!g_router.is_running)
    return false;

  uint32_t samples
    = atomic_load_explicit (&g_router.ring_buffer.samples_buffered,
			    memory_order_relaxed);
  uint32_t peak = atomic_load_explicit (&g_router.ring_buffer.peak_usage,
					memory_order_relaxed);

  if (buffered_frames)
    *buffered_frames = samples / g_router.channels;
  if (latency_ms)
    *latency_ms = calculate_latency_ms (samples / g_router.channels,
					g_router.sample_rate);
  if (watermark_peak)
    *watermark_peak = (float) peak / 100.0f;

  return true;
}
