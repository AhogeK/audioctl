//
// Created by AhogeK on 11/20/24.
//

#include "audio_control.h"
#include <CoreAudio/CoreAudio.h>

// 获取音频设备属性
static OSStatus getAudioProperty(AudioDeviceID deviceId,
                                 AudioObjectPropertySelector selector,
                                 AudioObjectPropertyScope scope,
                                 UInt32 element,
                                 void *data,
                                 UInt32 *dataSize) {
    AudioObjectPropertyAddress propertyAddress = {
            selector,
            scope,
            element
    };
    return AudioObjectGetPropertyData(deviceId, &propertyAddress, 0, NULL, dataSize, data);
}

// 检查属性是否可设置
static OSStatus isPropertySettable(AudioDeviceID deviceId,
                                   AudioObjectPropertyScope scope,
                                   UInt32 element,
                                   Boolean *settable) {
    AudioObjectPropertyAddress propertyAddress = {
            kAudioDevicePropertyVolumeScalar,  // 使用正确的属性选择器
            scope,
            element
    };
    return AudioObjectIsPropertySettable(deviceId, &propertyAddress, settable);
}

// 获取设备音量的详细实现
static void getVolumeInfo(AudioDeviceID deviceId,
                          AudioDeviceType deviceType,
                          Float32 *volume,
                          Boolean *hasVolumeControl) {
    *volume = 0.0f;
    *hasVolumeControl = false;

    AudioObjectPropertyScope scope = (deviceType == kDeviceTypeInput) ?
                                     kAudioDevicePropertyScopeInput :
                                     kAudioDevicePropertyScopeOutput;

    UInt32 dataSize = sizeof(Float32);
    Boolean isSettable = false;
    OSStatus status;

    // 尝试获取主音量
    status = isPropertySettable(deviceId, scope, kAudioObjectPropertyElementMain, &isSettable);
    if (status == noErr && isSettable &&
        getAudioProperty(deviceId, kAudioDevicePropertyVolumeScalar, scope,
                         kAudioObjectPropertyElementMain, volume, &dataSize) == noErr) {
        *hasVolumeControl = true;
        return;
    }

    // 尝试获取第一个通道的音量
    status = isPropertySettable(deviceId, scope, 1, &isSettable);
    if (status == noErr && isSettable &&
        getAudioProperty(deviceId, kAudioDevicePropertyVolumeScalar, scope,
                         1, volume, &dataSize) == noErr) {
        *hasVolumeControl = true;
        return;
    }

    // 尝试获取 dB 音量
    Float32 volumeDB;
    status = getAudioProperty(deviceId, kAudioDevicePropertyVolumeDecibels, scope,
                              kAudioObjectPropertyElementMain, &volumeDB, &dataSize);
    if (status != noErr) {
        return;
    }

    *hasVolumeControl = true;
    if (volumeDB > 0.0f) {
        *volume = 1.0f;
        return;
    }

    if (volumeDB < -96.0f) {
        *volume = 0.0f;
        return;
    }

    *volume = (volumeDB + 96.0f) / 96.0f;
}

// 获取单个范围（输入/输出）的通道数
static UInt32 getChannelCountForScope(AudioDeviceID deviceId,
                                      AudioObjectPropertyScope scope) {
    UInt32 channelCount = 0;
    AudioObjectPropertyAddress propertyAddress = {
            kAudioDevicePropertyStreamConfiguration,
            scope,
            kAudioObjectPropertyElementMain
    };

    // 获取所需的数据大小
    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(deviceId,
                                                     &propertyAddress,
                                                     0,
                                                     NULL,
                                                     &dataSize);
    if (status != noErr || dataSize == 0) {
        return 0;
    }

    // 分配缓冲区
    AudioBufferList *bufferList = malloc(dataSize);
    if (!bufferList) {
        return 0;
    }

    // 获取通道配置
    status = AudioObjectGetPropertyData(deviceId,
                                        &propertyAddress,
                                        0,
                                        NULL,
                                        &dataSize,
                                        bufferList);

    if (status == noErr) {
        // 计算总通道数
        for (UInt32 i = 0; i < bufferList->mNumberBuffers; i++) {
            channelCount += bufferList->mBuffers[i].mNumberChannels;
        }
    }

    free(bufferList);
    return channelCount;
}

// 获取设备通道数量
static void getChannelCounts(AudioDeviceID deviceId,
                             UInt32 *inputCount,
                             UInt32 *outputCount) {
    // 初始化输出参数
    *inputCount = 0;
    *outputCount = 0;

    // 获取输入通道数
    *inputCount = getChannelCountForScope(deviceId, kAudioDevicePropertyScopeInput);

    // 获取输出通道数
    *outputCount = getChannelCountForScope(deviceId, kAudioDevicePropertyScopeOutput);
}


// 获取基本音频属性
static void getBasicAudioProperties(AudioDeviceID deviceId, AudioDeviceInfo *info) {
    UInt32 dataSize;

    // 获取采样率
    Float64 sampleRate;
    dataSize = sizeof(Float64);
    if (getAudioProperty(deviceId, kAudioDevicePropertyNominalSampleRate,
                         kAudioObjectPropertyScopeGlobal, 0,
                         &sampleRate, &dataSize) == noErr) {
        info->sampleRate = (UInt32) sampleRate;
    }

    // 获取音频格式
    AudioStreamBasicDescription streamFormat;
    dataSize = sizeof(AudioStreamBasicDescription);
    if (getAudioProperty(deviceId, kAudioDevicePropertyStreamFormat,
                         kAudioObjectPropertyScopeGlobal, 0,
                         &streamFormat, &dataSize) == noErr) {
        info->bitsPerChannel = streamFormat.mBitsPerChannel;
        info->formatFlags = streamFormat.mFormatFlags;
    }
}

// 获取设备静音状态
static void getDeviceMuteState(AudioDeviceID deviceId, AudioDeviceInfo *info) {
    if (info->deviceType != kDeviceTypeOutput && info->deviceType != kDeviceTypeInputOutput) {
        return;
    }

    UInt32 muteState;
    UInt32 dataSize = sizeof(UInt32);
    info->isMuted = (getAudioProperty(deviceId, kAudioDevicePropertyMute,
                                      kAudioDevicePropertyScopeOutput, 0,
                                      &muteState, &dataSize) == noErr) && muteState != 0;
}

// 获取设备传输类型
static void getDeviceTransportType(AudioDeviceID deviceId, AudioDeviceInfo *info) {
    UInt32 dataSize = sizeof(UInt32);
    OSStatus status = getAudioProperty(deviceId, kAudioDevicePropertyTransportType,
                                       kAudioObjectPropertyScopeGlobal, 0,
                                       &info->transportType, &dataSize);
    if (status != noErr) {
        return;
    }

    // Continuity Camera 设备没有音量控制
    if (info->transportType == kAudioDeviceTransportTypeContinuityCaptureWired ||
        info->transportType == kAudioDeviceTransportTypeContinuityCaptureWireless) {
        info->hasVolumeControl = false;
    }
}

// 获取设备状态
static void getDeviceStatus(AudioDeviceID deviceId, AudioDeviceInfo *info) {
    getDeviceMuteState(deviceId, info);
    getDeviceTransportType(deviceId, info);
}

// 检查设备是否在某处运行
static Boolean isDeviceRunningAnywhere(AudioDeviceID deviceId) {
    UInt32 isRunning;
    UInt32 dataSize = sizeof(UInt32);

    OSStatus status = getAudioProperty(deviceId, kAudioDevicePropertyDeviceIsRunningSomewhere,
                                       kAudioObjectPropertyScopeGlobal, 0,
                                       &isRunning, &dataSize);
    return status == noErr && isRunning != 0;
}

// 检查设备是否在指定范围内运行
static Boolean isDeviceRunningInScope(AudioDeviceID deviceId, AudioDeviceType deviceType) {
    UInt32 isRunning;
    UInt32 dataSize = sizeof(UInt32);
    AudioObjectPropertyScope scope = (deviceType == kDeviceTypeInput) ?
                                     kAudioDevicePropertyScopeInput :
                                     kAudioDevicePropertyScopeOutput;

    OSStatus status = getAudioProperty(deviceId, kAudioDevicePropertyDeviceIsRunning,
                                       scope, 0, &isRunning, &dataSize);
    return status == noErr && isRunning != 0;
}

// 检查设备运行状态
static void checkDeviceRunningStatus(AudioDeviceID deviceId, AudioDeviceInfo *info) {
    UInt32 isAlive;
    UInt32 dataSize = sizeof(UInt32);

    // 检查设备是否活跃
    OSStatus status = getAudioProperty(deviceId, kAudioDevicePropertyDeviceIsAlive,
                                       kAudioObjectPropertyScopeGlobal, 0,
                                       &isAlive, &dataSize);
    if (status != noErr || !isAlive) {
        info->isRunning = false;
        return;
    }

    // 首先检查设备是否在指定范围内运行
    info->isRunning = isDeviceRunningInScope(deviceId, info->deviceType);

    // 如果没有在指定范围内运行，检查是否在其他地方运行
    if (!info->isRunning) {
        info->isRunning = isDeviceRunningAnywhere(deviceId);
    }
}

OSStatus getDeviceList(AudioDeviceInfo **devices, UInt32 *deviceCount) {
    // 获取设备数量
    const AudioObjectPropertyAddress propertyAddress = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(
            kAudioObjectSystemObject,
            &propertyAddress,
            0,
            NULL,
            &dataSize);

    if (status != noErr) return status;

    // 分配内存并获取设备列表
    const UInt32 numDevices = dataSize / sizeof(AudioDeviceID); // 改用不同的变量名
    AudioDeviceID *deviceIds = malloc(dataSize);

    status = AudioObjectGetPropertyData(
            kAudioObjectSystemObject,
            &propertyAddress,
            0,
            NULL,
            &dataSize,
            deviceIds);

    if (status != noErr) {
        free(deviceIds);
        return status;
    }

    // 创建设备信息数组
    *devices = malloc(numDevices * sizeof(AudioDeviceInfo));
    *deviceCount = numDevices; // 使用新的变量名

    // 获取每个设备的详细信息
    for (UInt32 i = 0; i < numDevices; i++) {
        getDeviceInfo(deviceIds[i], &(*devices)[i]);
    }

    free(deviceIds);
    return noErr;
}

OSStatus getDeviceInfo(AudioDeviceID deviceId, AudioDeviceInfo *info) {
    if (!info) return kAudioHardwareIllegalOperationError;

    // 初始化结构体
    memset(info, 0, sizeof(AudioDeviceInfo));
    info->deviceId = deviceId;

    // 获取设备名称
    CFStringRef deviceName;
    UInt32 dataSize = sizeof(CFStringRef);
    if (getAudioProperty(deviceId, kAudioDevicePropertyDeviceNameCFString,
                         kAudioObjectPropertyScopeGlobal, 0, &deviceName, &dataSize) == noErr) {
        CFStringGetCString(deviceName, info->name, sizeof(info->name), kCFStringEncodingUTF8);
        CFRelease(deviceName);
    } else {
        strncpy(info->name, "Unknown Device", sizeof(info->name) - 1);
    }

    // 获取通道信息
    getChannelCounts(deviceId, &info->inputChannelCount, &info->outputChannelCount);
    info->channelCount = info->inputChannelCount + info->outputChannelCount;

    // 确定设备类型
    if (info->inputChannelCount > 0 && info->outputChannelCount > 0) {
        info->deviceType = kDeviceTypeInputOutput;
    } else if (info->inputChannelCount > 0) {
        info->deviceType = kDeviceTypeInput;
    } else if (info->outputChannelCount > 0) {
        info->deviceType = kDeviceTypeOutput;
    } else {
        info->deviceType = kDeviceTypeUnknown;
    }

    // 获取音量信息
    getVolumeInfo(deviceId, info->deviceType, &info->volume, &info->hasVolumeControl);

    // 获取基本音频属性
    getBasicAudioProperties(deviceId, info);

    // 获取设备状态信息
    getDeviceStatus(deviceId, info);

    // 检查设备运行状态
    checkDeviceRunningStatus(deviceId, info);

    return noErr;
}

const char *getTransportTypeName(const UInt32 transportType) {
    switch (transportType) {
        case kAudioDeviceTransportTypeBuiltIn:
            return "Built-in";
        case kAudioDeviceTransportTypeAggregate:
            return "Aggregate";
        case kAudioDeviceTransportTypeVirtual:
            return "Virtual";
        case kAudioDeviceTransportTypeUSB:
            return "USB";
        case kAudioDeviceTransportTypeFireWire:
            return "FireWire";
        case kAudioDeviceTransportTypeBluetooth:
            return "Bluetooth";
        case kAudioDeviceTransportTypeHDMI:
            return "HDMI";
        case kAudioDeviceTransportTypeDisplayPort:
            return "DisplayPort";
        case kAudioDeviceTransportTypeAirPlay:
            return "AirPlay";
        case kAudioDeviceTransportTypeContinuityCaptureWired:
        case kAudioDeviceTransportTypeContinuityCaptureWireless:
            return "Continuity Camera";
        default:
            return "Unknown";
    }
}

const char *getFormatFlagsDescription(const UInt32 formatFlags) {
    if (formatFlags & kAudioFormatFlagIsFloat) return "Float";
    if (formatFlags & kAudioFormatFlagIsSignedInteger) return "Signed Integer";
    if (formatFlags & kAudioFormatFlagIsNonInterleaved) return "Non-interleaved";
    return "Unknown";
}
