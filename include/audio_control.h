//
// Created by AhogeK on 11/20/24.
//
#ifndef AUDIO_CONTROL_H
#define AUDIO_CONTROL_H

#include <CoreAudio/CoreAudio.h>
#include <CoreServices/CoreServices.h>
#include <AudioToolbox/AudioToolbox.h>

typedef enum {
    kDeviceTypeUnknown = 0,
    kDeviceTypeInput = 1,
    kDeviceTypeOutput = 2,
    kDeviceTypeInputOutput = 3
} AudioDeviceType;

typedef struct {
    AudioDeviceID deviceId;
    char name[256];
    Float32 volume;
    Boolean isMuted;
    UInt32 sampleRate;
    AudioDeviceType deviceType; // 替换原来的isInput
    UInt32 inputChannelCount; // 输入通道数
    UInt32 outputChannelCount; // 输出通道数
    UInt32 channelCount; // 通道数量
    UInt32 bitsPerChannel; // 每个通道的位数
    UInt32 formatFlags; // 音频格式标志
    UInt32 transportType; // 传输类型
    Boolean isRunning; // 设备是否正在运行
    Boolean hasVolumeControl; // 设备是否有音量控制
} AudioDeviceInfo;

// 核心功能函数声明
OSStatus getDeviceList(AudioDeviceInfo **devices, UInt32 *deviceCount);

OSStatus getDeviceInfo(AudioDeviceID deviceId, AudioDeviceInfo *info);

OSStatus setDeviceVolume(AudioDeviceID deviceId, Float32 volume);

OSStatus setDeviceActive(AudioDeviceID deviceId);

// 辅助函数声明
const char *getTransportTypeName(UInt32 transportType);

const char *getFormatFlagsDescription(UInt32 formatFlags);

#endif //AUDIO_CONTROL_H
