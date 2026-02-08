//
// 驱动端应用音量控制 - 在虚拟驱动中使用
// Created by AhogeK on 02/05/26.
//

#ifndef AUDIOCTL_APP_VOLUME_DRIVER_H
#define AUDIOCTL_APP_VOLUME_DRIVER_H

#include <CoreAudio/AudioServerPlugIn.h>
#include <stdbool.h>
#include <sys/types.h>

// 最大客户端数量
#define MAX_CLIENT_ENTRIES 64

// 客户端音量条目
typedef struct
{
    UInt32 clientID; // HAL客户端ID
    pid_t pid; // 进程ID
    char bundleId[256]; // Bundle ID
    char name[256]; // 应用名称
    Float32 volume; // 当前音量
    bool isMuted; // 是否静音
    bool isActive; // 是否活跃
} ClientVolumeEntry;

// 自定义属性用于应用音量控制
#define kAudioDevicePropertyAppVolume 0x61766F6C // 'avol'

// 应用音量属性数据结构
typedef struct
{
    pid_t pid;
    Float32 volume;
    bool isMuted;
} AppVolumePropertyData;

// 客户端音量管理器
typedef struct
{
    ClientVolumeEntry entries[MAX_CLIENT_ENTRIES];
    UInt32 count;
    pthread_mutex_t mutex;
} ClientVolumeManager;

#pragma mark - 初始化和清理

// 初始化客户端音量管理器
void app_volume_driver_init(void);

// 清理客户端音量管理器
void app_volume_driver_cleanup(void);

#pragma mark - 客户端管理

// 添加客户端
OSStatus app_volume_driver_add_client(UInt32 clientID, pid_t pid, const char* bundleId, const char* name);

// 移除客户端
OSStatus app_volume_driver_remove_client(UInt32 clientID);

// 更新指定 PID 的音量
OSStatus app_volume_driver_update_by_pid(pid_t pid, Float32 volume, bool isMuted);

// 根据ClientID查找PID
pid_t app_volume_driver_get_pid(UInt32 clientID);

#pragma mark - 音量应用

// 获取指定客户端的音量 (从共享内存读取)
Float32 app_volume_driver_get_volume(UInt32 clientID, bool* outIsMuted);

// 应用音量到音频缓冲区
void app_volume_driver_apply_volume(UInt32 clientID, void* buffer, UInt32 frameCount, UInt32 channels);

#endif //AUDIOCTL_APP_VOLUME_DRIVER_H
