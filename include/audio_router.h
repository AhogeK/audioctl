//
// Audio Router - 串联架构核心组件
// 实现双端音频泵：Virtual Device -> Ring Buffer -> Physical Device
// Created by AhogeK on 02/12/26.
//

#ifndef AUDIOCTL_AUDIO_ROUTER_H
#define AUDIOCTL_AUDIO_ROUTER_H

#include <CoreAudio/CoreAudio.h>
#include <stdatomic.h>
#include <stdbool.h>

// 环形缓冲区大小（约 170ms @ 48kHz，双声道）
#define ROUTER_BUFFER_FRAME_COUNT 8192
#define ROUTER_MAX_CHANNELS 2

// 环形缓冲区结构
typedef struct
{
  float *buffer;
  uint32_t capacity;
  atomic_uint write_pos;
  atomic_uint read_pos;
} RouterRingBuffer;

// Router 上下文
typedef struct
{
  AudioDeviceID input_device;  // 虚拟设备 (Source)
  AudioDeviceID output_device; // 物理设备 (Sink)

  AudioDeviceIOProcID input_proc_id;
  AudioDeviceIOProcID output_proc_id;

  RouterRingBuffer ring_buffer;
  bool is_running;

  // 音频格式信息
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t bits_per_channel;

  // 统计信息
  uint64_t frames_transferred;
  uint32_t underrun_count;
  uint32_t overrun_count;
} AudioRouterContext;

/**
 * 初始化并启动路由（绑定到指定的物理设备UID）
 *
 * @param physical_device_uid 目标物理设备的 UID
 * @return OSStatus 操作状态
 */
OSStatus
audio_router_start (const char *physical_device_uid);

/**
 * 停止路由
 */
void
audio_router_stop (void);

/**
 * 检查 Router 是否正在运行
 */
bool
audio_router_is_running (void);

/**
 * 获取当前绑定的物理设备 UID
 * 用于显示状态
 *
 * @param uid 输出缓冲区
 * @param size 缓冲区大小
 * @return 成功返回 true
 */
bool
audio_router_get_physical_device_uid (char *uid, size_t size);

/**
 * 获取 Router 统计信息
 *
 * @param frames_transferred 传输的帧数
 * @param underruns 欠载次数
 * @param overruns 过载次数
 */
void
audio_router_get_stats (uint64_t *frames_transferred, uint32_t *underruns,
			uint32_t *overruns);

#endif // AUDIOCTL_AUDIO_ROUTER_H
