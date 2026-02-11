//
// AudioCTL 通用类型定义
// 用于 CLI 和 Driver 之间的通信
// Created by Agent on 02/11/26.
//

#ifndef AUDIO_COMMON_TYPES_H
#define AUDIO_COMMON_TYPES_H

#include <CoreAudio/CoreAudio.h>

// 自定义属性 Selector: 'apvl' (App Volumes)
#define kAudioDevicePropertyAppVolumes 0x6170766c // 'apvl'

// 自定义属性 Selector: 'apcl' (App Client List) - 用于获取连接的客户端PID列表
#define kAudioDevicePropertyAppClientList 0x6170636c // 'apcl'

// 最大支持的应用数量
#define MAX_APP_ENTRIES 64

// 单个应用音量条目
// 保持 4 字节对齐
typedef struct
{
    pid_t pid; // 进程 ID
    Float32 volume; // 音量 (0.0 - 1.0)
    UInt32 isMuted; // 是否静音 (1=true, 0=false)
    UInt32 isActive; // 是否活跃 (预留)
    char bundleId[128]; // Bundle ID (例如 "com.google.Chrome")
} AppVolumeEntry;

// 音量表（通过 CoreAudio 属性传输的完整数据块）
typedef struct
{
    UInt32 count;
    UInt32 reserved; // 填充对齐
    AppVolumeEntry entries[MAX_APP_ENTRIES];
} AppVolumeTable;

#endif // AUDIO_COMMON_TYPES_H
