//
// Created by AhogeK on 11/21/24.
//

#ifndef AUDIO_APPS_H
#define AUDIO_APPS_H

#include <CoreAudio/CoreAudio.h>

typedef struct {
    char bundleId[256];    // 应用程序包标识符
    char name[256];        // 应用程序名称
    Float32 volume;        // 音量 (0.0 - 1.0)
    pid_t pid;            // 进程ID
    AudioDeviceID deviceId; // 使用的设备ID
} AudioAppInfo;

// 获取正在使用音频的应用程序列表
OSStatus getAudioApps(AudioAppInfo** apps, UInt32* appCount);

// 释放应用程序列表内存
void freeAudioApps(AudioAppInfo* apps);

#endif //AUDIO_APPS_H
