//
// 应用音量控制模块 - 管理和控制每个应用的音量
// Created by AhogeK on 02/05/26.
//

#ifndef AUDIOCTL_APP_VOLUME_CONTROL_H
#define AUDIOCTL_APP_VOLUME_CONTROL_H

#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>
#include <sys/types.h>

// 最大支持的应用数量
#define MAX_APP_VOLUME_ENTRIES 64

// 应用音量信息结构体
typedef struct
{
    pid_t pid; // 进程ID
    char bundleId[256]; // 应用Bundle ID
    char name[256]; // 应用名称
    Float32 volume; // 音量 (0.0 - 1.0)
    bool isMuted; // 是否静音
    bool isActive; // 是否正在使用音频
    AudioDeviceID deviceId; // 当前使用的设备
} AppVolumeInfo;

// 应用音量列表
typedef struct
{
    AppVolumeInfo entries[MAX_APP_VOLUME_ENTRIES];
    UInt32 count;
    pthread_mutex_t mutex;
} AppVolumeList;

#pragma mark - 初始化和清理

// 初始化应用音量控制系统
OSStatus app_volume_control_init(void);

// 清理应用音量控制系统
void app_volume_control_cleanup(void);

#pragma mark - 音量控制

// 设置指定应用的音量 (0.0 - 1.0)
OSStatus app_volume_set(pid_t pid, Float32 volume);

// 获取指定应用的音量
OSStatus app_volume_get(pid_t pid, Float32* outVolume);

// 设置指定应用的静音状态
OSStatus app_volume_set_mute(pid_t pid, bool mute);

// 获取指定应用的静音状态
OSStatus app_volume_get_mute(pid_t pid, bool* outMute);

#pragma mark - 应用管理

// 注册一个应用到音量控制系统
OSStatus app_volume_register(pid_t pid, const char* bundleId, const char* name);

// 从音量控制系统移除一个应用
OSStatus app_volume_unregister(pid_t pid);

// 更新应用的活动状态
OSStatus app_volume_set_active(pid_t pid, bool active);

#pragma mark - 查询

// 获取所有应用音量信息
OSStatus app_volume_get_all(AppVolumeInfo* * outApps, UInt32* outCount);

// 释放应用音量信息列表
void app_volume_free_list(AppVolumeInfo* apps);

// 根据PID查找应用音量信息
AppVolumeInfo* app_volume_find(pid_t pid);

// 获取当前正在播放音频的应用数量
UInt32 app_volume_get_active_count(void);

#pragma mark - 共享内存（用于驱动通信）

// 创建/打开共享内存区域
OSStatus app_volume_shm_init(void);

// 关闭共享内存
void app_volume_shm_cleanup(void);

// 将当前音量列表同步到共享内存
OSStatus app_volume_sync_to_shm(void);

// 从共享内存同步音量列表
OSStatus app_volume_sync_from_shm(void);

#pragma mark - 命令行接口

// 列出所有应用的音量信息
void app_volume_cli_list(void);

// 设置指定应用的音量
int app_volume_cli_set(const char* appNameOrPid, Float32 volume);

// 设置指定应用的静音状态
int app_volume_cli_mute(const char* appNameOrPid, bool mute);

#endif //AUDIOCTL_APP_VOLUME_CONTROL_H
