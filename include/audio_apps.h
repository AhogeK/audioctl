//
// Created by AhogeK on 11/21/24.
//

#ifndef AUDIO_APPS_H
#define AUDIO_APPS_H

#include <CoreAudio/CoreAudio.h>

typedef struct
{
    char bundleId[256]; // 应用程序包标识符
    char name[256]; // 应用程序名称
    Float32 volume; // 音量 (0.0 - 1.0)
    pid_t pid; // 进程ID
    AudioDeviceID deviceId; // 使用的设备ID
} AudioAppInfo;

/**
 * 获取正在使用音频的应用程序列表
 *
 * ⚠️ 性能警告：此函数会遍历所有运行中的应用并进行 HAL 查询
 *   - 时间复杂度：O(n)，n 为运行中的应用数量
 *   - 每次调用涉及多次 AudioObjectGetPropertyData 系统调用
 *   - 典型耗时：10-100ms（取决于应用数量）
 *
 * 🚫 禁止在以下场景使用：
 *   - 音频回调线程（IOProc）- 会导致音频卡顿/中断
 *   - 高频轮询（如每秒多次）- 会导致 CPU 高负载
 *   - 实时性要求高的场景
 *
 * ✅ 适用场景：
 *   - 用户主动触发的 CLI 命令（如 audioctl apps）
 *   - 单次查询，不频繁调用
 *   - 后台非实时任务
 *
 * @param apps 输出参数，返回应用信息数组（需用 freeAudioApps 释放）
 * @param appCount 输出参数，返回应用数量
 * @return OSStatus 操作状态
 */
OSStatus getAudioApps(AudioAppInfo * *apps, UInt32 * appCount);

// 释放应用程序列表内存
void freeAudioApps(AudioAppInfo* apps);

#endif //AUDIO_APPS_H