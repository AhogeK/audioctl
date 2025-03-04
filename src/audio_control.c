//
// Created by AhogeK on 11/20/24.
//

#include "audio_control.h"

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

// 获取设备支持的dB范围
static OSStatus
getDeviceVolumeDbRange(AudioDeviceID deviceId, AudioObjectPropertyScope scope, Float32 *minDb, Float32 *maxDb) {
    AudioObjectPropertyAddress propertyAddress = {
            kAudioDevicePropertyVolumeRangeDecibels,
            scope,
            kAudioObjectPropertyElementMain
    };

    AudioValueRange dbRange;
    UInt32 dataSize = sizeof(AudioValueRange);

    OSStatus status = AudioObjectGetPropertyData(deviceId, &propertyAddress,
                                                 0, NULL,
                                                 &dataSize, &dbRange);

    if (status != noErr) {
        // 回退到默认值
        *minDb = -96.0f;
        *maxDb = 0.0f;
        return status;
    }

    *minDb = dbRange.mMinimum;
    *maxDb = dbRange.mMaximum;

    return noErr;
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

    // 尝试获取 dB 音量并转换为标量值
    Float32 volumeDB;
    status = getAudioProperty(deviceId, kAudioDevicePropertyVolumeDecibels, scope,
                              kAudioObjectPropertyElementMain, &volumeDB, &dataSize);
    if (status != noErr) {
        return;
    }

    *hasVolumeControl = true;

    // 获取设备实际的dB范围
    Float32 minDb, maxDb;
    if (getDeviceVolumeDbRange(deviceId, scope, &minDb, &maxDb) == noErr) {
        // 限制在设备范围内
        if (volumeDB > maxDb) volumeDB = maxDb;
        if (volumeDB < minDb) volumeDB = minDb;

        // 根据实际范围计算百分比
        *volume = (volumeDB - minDb) / (maxDb - minDb);
    } else {
        // 没有范围信息时使用默认计算
        if (volumeDB > 0.0f) {
            *volume = 1.0f;
        } else if (volumeDB < -96.0f) {
            *volume = 0.0f;
        } else {
            *volume = (volumeDB + 96.0f) / 96.0f;
        }
    }
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

// 检查设备是否在指定范围内运行
static Boolean isDeviceRunningInScope(AudioDeviceID deviceId, AudioObjectPropertyScope scope) {
    UInt32 isRunning = 0;
    UInt32 dataSize = sizeof(UInt32);
    AudioObjectPropertyAddress propertyAddress = {
            .mSelector = kAudioDevicePropertyDeviceIsRunning,
            .mScope = scope,
            .mElement = kAudioObjectPropertyElementMain
    };

    // 首先检查属性是否存在
    if (!AudioObjectHasProperty(deviceId, &propertyAddress)) {
        return false;
    }

    OSStatus status = AudioObjectGetPropertyData(deviceId,
                                                 &propertyAddress,
                                                 0,
                                                 NULL,
                                                 &dataSize,
                                                 &isRunning);

    return (status == noErr && isRunning != 0);
}

// 检查设备是否是当前默认设备
static Boolean isDefaultDevice(AudioDeviceID deviceId, AudioObjectPropertyScope scope) {
    AudioDeviceID defaultDevice;
    UInt32 dataSize = sizeof(AudioDeviceID);
    AudioObjectPropertyAddress propertyAddress = {
            .mSelector = (scope == kAudioDevicePropertyScopeInput) ?
                         kAudioHardwarePropertyDefaultInputDevice :
                         kAudioHardwarePropertyDefaultOutputDevice,
            .mScope = kAudioObjectPropertyScopeGlobal,
            .mElement = kAudioObjectPropertyElementMain
    };

    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                                 &propertyAddress,
                                                 0,
                                                 NULL,
                                                 &dataSize,
                                                 &defaultDevice);

    return (status == noErr && defaultDevice == deviceId);
}

// 检查设备运行状态
static void checkDeviceRunningStatus(AudioDeviceID deviceId, AudioDeviceInfo *info) {
    // 首先检查设备是否活跃
    UInt32 isAlive = 0;
    UInt32 dataSize = sizeof(UInt32);
    AudioObjectPropertyAddress aliveAddress = {
            .mSelector = kAudioDevicePropertyDeviceIsAlive,
            .mScope = kAudioObjectPropertyScopeGlobal,
            .mElement = kAudioObjectPropertyElementMain
    };

    OSStatus status = AudioObjectGetPropertyData(deviceId,
                                                 &aliveAddress,
                                                 0,
                                                 NULL,
                                                 &dataSize,
                                                 &isAlive);

    if (status != noErr || !isAlive) {
        info->isRunning = false;
        return;
    }

    // 根据设备类型检查运行状态
    switch (info->deviceType) {
        case kDeviceTypeInput:
            info->isRunning = isDeviceRunningInScope(deviceId, kAudioDevicePropertyScopeInput) ||
                              isDefaultDevice(deviceId, kAudioDevicePropertyScopeInput);
            break;

        case kDeviceTypeOutput:
            info->isRunning = isDeviceRunningInScope(deviceId, kAudioDevicePropertyScopeOutput) ||
                              isDefaultDevice(deviceId, kAudioDevicePropertyScopeOutput);
            break;

        case kDeviceTypeInputOutput:
            info->isRunning = isDeviceRunningInScope(deviceId, kAudioDevicePropertyScopeInput) ||
                              isDeviceRunningInScope(deviceId, kAudioDevicePropertyScopeOutput) ||
                              isDefaultDevice(deviceId, kAudioDevicePropertyScopeInput) ||
                              isDefaultDevice(deviceId, kAudioDevicePropertyScopeOutput);
            break;

        default:
            info->isRunning = false;
    }

    // 添加额外的检查：如果设备正在处理音频流
    AudioObjectPropertyAddress streamAddress = {
            .mSelector = kAudioDevicePropertyStreamConfiguration,
            .mScope = (info->deviceType == kDeviceTypeInput) ?
                      kAudioDevicePropertyScopeInput :
                      kAudioDevicePropertyScopeOutput,
            .mElement = kAudioObjectPropertyElementMain
    };

    if (AudioObjectHasProperty(deviceId, &streamAddress)) {
        UInt32 processingState = 0;
        dataSize = sizeof(UInt32);
        status = AudioObjectGetPropertyData(deviceId,
                                            &streamAddress,
                                            0,
                                            NULL,
                                            &dataSize,
                                            &processingState);
        if (status == noErr && processingState != 0) {
            info->isRunning = true;
        }
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
    OSStatus status = getAudioProperty(deviceId, kAudioDevicePropertyDeviceNameCFString,
                                       kAudioObjectPropertyScopeGlobal, 0, &deviceName, &dataSize);
    if (status != noErr) {
        return status; // 返回获取设备名称时的错误状态
    }
    CFStringGetCString(deviceName, info->name, sizeof(info->name), kCFStringEncodingUTF8);
    CFRelease(deviceName);

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

    return noErr; // 确保在成功完成所有操作后返回 noErr
}

OSStatus setDeviceVolume(AudioDeviceID deviceId, Float32 volume) {
    AudioDeviceInfo deviceInfo;
    OSStatus status = getDeviceInfo(deviceId, &deviceInfo);
    if (status != noErr) {
        return status;
    }

    if (!deviceInfo.hasVolumeControl) {
        return kAudioHardwareUnsupportedOperationError;
    }

    AudioObjectPropertyScope scope = (deviceInfo.deviceType == kDeviceTypeInput) ?
                                     kAudioDevicePropertyScopeInput :
                                     kAudioDevicePropertyScopeOutput;

    // 检查并取消静音状态
    AudioObjectPropertyAddress mutePropertyAddress = {
            .mSelector = kAudioDevicePropertyMute,
            .mScope = scope,
            .mElement = kAudioObjectPropertyElementMain
    };

    Boolean isMuteSettable = false;
    status = AudioObjectIsPropertySettable(deviceId, &mutePropertyAddress, &isMuteSettable);
    if (status == noErr && isMuteSettable) {
        UInt32 muteValue = 0; // 0 表示取消静音
        status = AudioObjectSetPropertyData(
                deviceId,
                &mutePropertyAddress,
                0,
                NULL,
                sizeof(muteValue),
                &muteValue
        );
        if (status != noErr) {
            printf("警告：无法取消设备的静音状态，错误码: %d\n", status);
        }
    }

    // 尝试使用dB值设置音量 - 这是更精确的方法
    Float32 minDb, maxDb;
    if (getDeviceVolumeDbRange(deviceId, scope, &minDb, &maxDb) == noErr) {
        // 根据百分比计算对应的dB值
        Float32 dbValue = minDb + volume * (maxDb - minDb);

        AudioObjectPropertyAddress dbPropertyAddress = {
                .mSelector = kAudioDevicePropertyVolumeDecibels,
                .mScope = scope,
                .mElement = kAudioObjectPropertyElementMain
        };

        Boolean isDbSettable = false;
        status = AudioObjectIsPropertySettable(deviceId, &dbPropertyAddress, &isDbSettable);
        if (status == noErr && isDbSettable) {
            status = AudioObjectSetPropertyData(
                    deviceId,
                    &dbPropertyAddress,
                    0,
                    NULL,
                    sizeof(dbValue),
                    &dbValue
            );
            if (status == noErr) {
                return noErr;
            }
            // 如果失败，回退到标量设置
        }
    }

    // 尝试在主元素上设置 VolumeScalar
    AudioObjectPropertyAddress propertyAddress = {
            .mSelector = kAudioDevicePropertyVolumeScalar,
            .mScope = scope,
            .mElement = kAudioObjectPropertyElementMain
    };

    Boolean isSettable = false;
    status = AudioObjectIsPropertySettable(deviceId, &propertyAddress, &isSettable);
    if (status == noErr && isSettable) {
        status = AudioObjectSetPropertyData(
                deviceId,
                &propertyAddress,
                0,
                NULL,
                sizeof(volume),
                &volume
        );
        if (status == noErr) {
            return noErr;
        }
    }

    // 如果主元素设置失败，尝试设置各个通道的 VolumeScalar
    UInt32 channelCount = (deviceInfo.deviceType == kDeviceTypeInput) ?
                          deviceInfo.inputChannelCount :
                          deviceInfo.outputChannelCount;

    Boolean success = false;
    for (UInt32 channel = 1; channel <= channelCount; channel++) {
        propertyAddress.mElement = channel;
        status = AudioObjectIsPropertySettable(deviceId, &propertyAddress, &isSettable);
        if (status == noErr && isSettable) {
            status = AudioObjectSetPropertyData(
                    deviceId,
                    &propertyAddress,
                    0,
                    NULL,
                    sizeof(volume),
                    &volume
            );
            if (status == noErr) {
                success = true;
            }
        }
    }

    if (success) {
        return noErr;
    } else {
        printf("错误：无法设置设备的音量，错误码: %d\n", status);
        return status;
    }
}

OSStatus setDeviceActive(AudioDeviceID deviceId) {
    AudioDeviceInfo deviceInfo;
    OSStatus status = getDeviceInfo(deviceId, &deviceInfo);
    if (status != noErr) {
        return status;
    }

    // 根据设备类型选择正确的属性地址
    AudioObjectPropertyAddress propertyAddress = {
            kAudioHardwarePropertyDefaultOutputDevice,  // 使用系统输出设备属性
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
    };

    if (deviceInfo.deviceType == kDeviceTypeInput) {
        propertyAddress.mSelector = kAudioHardwarePropertyDefaultInputDevice;
    }

    // 设置默认设备
    status = AudioObjectSetPropertyData(kAudioObjectSystemObject,
                                        &propertyAddress,
                                        0,
                                        NULL,
                                        sizeof(AudioDeviceID),
                                        &deviceId);

    if (status != noErr) {
        printf("错误：无法设置设备为默认设备，错误码: %d\n", status);
        return status;
    }

    // 验证设置是否生效
    AudioDeviceID currentDevice;
    UInt32 propertySize = sizeof(AudioDeviceID);
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                        &propertyAddress,
                                        0,
                                        NULL,
                                        &propertySize,
                                        &currentDevice);

    if (status != noErr || currentDevice != deviceId) {
        printf("错误：设备切换未生效\n");
        return kAudioHardwareUnspecifiedError;
    }

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