//
// Created by AhogeK on 11/20/24.
//

#include "audio_control.h"

OSStatus getDeviceList(AudioDeviceInfo** devices, UInt32* deviceCount)
{
    // 获取设备数量
    AudioObjectPropertyAddress propertyAddress = {
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
    UInt32 numDevices = dataSize / sizeof(AudioDeviceID); // 改用不同的变量名
    AudioDeviceID* deviceIds = malloc(dataSize);

    status = AudioObjectGetPropertyData(
        kAudioObjectSystemObject,
        &propertyAddress,
        0,
        NULL,
        &dataSize,
        deviceIds);

    if (status != noErr)
    {
        free(deviceIds);
        return status;
    }

    // 创建设备信息数组
    *devices = malloc(numDevices * sizeof(AudioDeviceInfo));
    *deviceCount = numDevices; // 使用新的变量名

    // 获取每个设备的详细信息
    for (UInt32 i = 0; i < numDevices; i++)
    {
        getDeviceInfo(deviceIds[i], &(*devices)[i]);
    }

    free(deviceIds);
    return noErr;
}

OSStatus getDeviceInfo(AudioDeviceID deviceId, AudioDeviceInfo* info)
{
    if (info == NULL) return kAudioHardwareIllegalOperationError;

    OSStatus finalStatus = noErr;
    OSStatus status;
    UInt32 dataSize;

    // 初始化结构体，设置默认值
    memset(info, 0, sizeof(AudioDeviceInfo));
    info->deviceId = deviceId;
    info->volume = 0.0f;
    info->isMuted = false;
    info->deviceType = kDeviceTypeUnknown;
    info->inputChannelCount = 0;
    info->outputChannelCount = 0;
    info->channelCount = 0;
    info->bitsPerChannel = 0;
    info->formatFlags = 0;
    info->transportType = 0;
    info->isRunning = false;
    info->hasVolumeControl = false;
    strcpy(info->name, "Unknown Device");

    // 获取设备名称
    AudioObjectPropertyAddress propertyAddress = {
        kAudioDevicePropertyDeviceNameCFString,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    CFStringRef deviceName;
    dataSize = sizeof(deviceName);

    status = AudioObjectGetPropertyData(
        deviceId,
        &propertyAddress,
        0,
        NULL,
        &dataSize,
        &deviceName);

    if (status == noErr)
    {
        CFStringGetCString(deviceName,
                           info->name,
                           sizeof(info->name),
                           kCFStringEncodingUTF8);
        CFRelease(deviceName);
    }
    else
    {
        finalStatus = status;
    }

    // 获取通道数量（分别获取输入和输出通道）
    propertyAddress.mSelector = kAudioDevicePropertyStreamConfiguration;

    // 获取输入通道
    propertyAddress.mScope = kAudioDevicePropertyScopeInput;
    UInt32 inputDataSize = 0;
    status = AudioObjectGetPropertyDataSize(
        deviceId,
        &propertyAddress,
        0,
        NULL,
        &inputDataSize);

    if (status == noErr && inputDataSize > 0)
    {
        AudioBufferList* bufferList = (AudioBufferList*)malloc(inputDataSize);
        if (bufferList)
        {
            status = AudioObjectGetPropertyData(
                deviceId,
                &propertyAddress,
                0,
                NULL,
                &inputDataSize,
                bufferList);

            if (status == noErr)
            {
                info->inputChannelCount = 0;
                for (UInt32 i = 0; i < bufferList->mNumberBuffers; i++)
                {
                    info->inputChannelCount += bufferList->mBuffers[i].mNumberChannels;
                }
            }
            free(bufferList);
        }
    }

    // 获取输出通道
    propertyAddress.mScope = kAudioDevicePropertyScopeOutput;
    UInt32 outputDataSize = 0;
    status = AudioObjectGetPropertyDataSize(
        deviceId,
        &propertyAddress,
        0,
        NULL,
        &outputDataSize);

    if (status == noErr && outputDataSize > 0)
    {
        AudioBufferList* bufferList = (AudioBufferList*)malloc(outputDataSize);
        if (bufferList)
        {
            status = AudioObjectGetPropertyData(
                deviceId,
                &propertyAddress,
                0,
                NULL,
                &outputDataSize,
                bufferList);

            if (status == noErr)
            {
                info->outputChannelCount = 0;
                for (UInt32 i = 0; i < bufferList->mNumberBuffers; i++)
                {
                    info->outputChannelCount += bufferList->mBuffers[i].mNumberChannels;
                }
            }
            free(bufferList);
        }
    }

    // 更新总通道数和设备类型
    info->channelCount = info->inputChannelCount + info->outputChannelCount;

    // 根据通道数量设置设备类型
    if (info->inputChannelCount > 0 && info->outputChannelCount > 0)
    {
        info->deviceType = kDeviceTypeInputOutput;
    }
    else if (info->inputChannelCount > 0)
    {
        info->deviceType = kDeviceTypeInput;
    }
    else if (info->outputChannelCount > 0)
    {
        info->deviceType = kDeviceTypeOutput;
    }
    else
    {
        info->deviceType = kDeviceTypeUnknown;
    }

    // 根据设备类型获取音量或输入音量
    Float32 volume = 0.0f;
    bool volumeValid = false;

    if (info->deviceType == kDeviceTypeOutput || info->deviceType == kDeviceTypeInputOutput)
    {
        // 获取输出音量
        propertyAddress.mSelector = kAudioDevicePropertyVolumeScalar;
        propertyAddress.mScope = kAudioDevicePropertyScopeOutput;
        propertyAddress.mElement = kAudioObjectPropertyElementMain;

        Boolean isSettable = false;
        status = AudioObjectIsPropertySettable(
            deviceId,
            &propertyAddress,
            &isSettable);

        if (status == noErr)
        {
            info->hasVolumeControl = isSettable;

            if (isSettable)
            {
                dataSize = sizeof(Float32);
                status = AudioObjectGetPropertyData(
                    deviceId,
                    &propertyAddress,
                    0,
                    NULL,
                    &dataSize,
                    &volume);

                if (status == noErr)
                {
                    info->volume = volume;
                    volumeValid = true;
                }
            }
        }
    }

    if (info->deviceType == kDeviceTypeInput ||
        (info->deviceType == kDeviceTypeInputOutput && !volumeValid))
    {
        // 首先尝试获取输入音量
        propertyAddress.mSelector = kAudioDevicePropertyVolumeScalar;
        propertyAddress.mScope = kAudioDevicePropertyScopeInput;
        propertyAddress.mElement = kAudioObjectPropertyElementMain;

        Boolean isSettable = false;
        status = AudioObjectIsPropertySettable(
            deviceId,
            &propertyAddress,
            &isSettable);

        if (status == noErr && isSettable)
        {
            dataSize = sizeof(Float32);
            status = AudioObjectGetPropertyData(
                deviceId,
                &propertyAddress,
                0,
                NULL,
                &dataSize,
                &volume);

            if (status == noErr)
            {
                info->volume = volume;
                info->hasVolumeControl = true;
                volumeValid = true;
            }
        }

        // 如果上面失败了，尝试获取第一个通道的音量
        if (!volumeValid)
        {
            propertyAddress.mSelector = kAudioDevicePropertyVolumeScalar;
            propertyAddress.mElement = 1; // 第一个通道

            status = AudioObjectIsPropertySettable(
                deviceId,
                &propertyAddress,
                &isSettable);

            if (status == noErr && isSettable)
            {
                dataSize = sizeof(Float32);
                status = AudioObjectGetPropertyData(
                    deviceId,
                    &propertyAddress,
                    0,
                    NULL,
                    &dataSize,
                    &volume);

                if (status == noErr)
                {
                    info->volume = volume;
                    info->hasVolumeControl = true;
                    volumeValid = true;
                }
            }
        }

        // 如果还是失败，尝试获取音量的 dB 值
        if (!volumeValid)
        {
            propertyAddress.mSelector = kAudioDevicePropertyVolumeDecibels;
            propertyAddress.mElement = kAudioObjectPropertyElementMain;

            Float32 volumeDB;
            status = AudioObjectGetPropertyData(
                deviceId,
                &propertyAddress,
                0,
                NULL,
                &dataSize,
                &volumeDB);

            if (status == noErr)
            {
                // 将 dB 转换为 0-1 范围的标量值
                if (volumeDB > 0.0f)
                {
                    info->volume = 1.0f;
                }
                else if (volumeDB < -96.0f)
                {
                    info->volume = 0.0f;
                }
                else
                {
                    info->volume = (volumeDB + 96.0f) / 96.0f;
                }
                info->hasVolumeControl = true;
                volumeValid = true;
            }
        }
    }

    // 获取静音状态
    // 只为输出设备获取静音状态
    if (info->deviceType == kDeviceTypeOutput || info->deviceType == kDeviceTypeInputOutput)
    {
        propertyAddress.mSelector = kAudioDevicePropertyMute;
        propertyAddress.mScope = kAudioDevicePropertyScopeOutput;
        propertyAddress.mElement = kAudioObjectPropertyElementMain;
        dataSize = sizeof(UInt32);
        UInt32 muteState;

        status = AudioObjectGetPropertyData(
            deviceId,
            &propertyAddress,
            0,
            NULL,
            &dataSize,
            &muteState);

        info->isMuted = (status == noErr) ? (muteState != 0) : false;
    }
    else
    {
        info->isMuted = false; // 对于非输出设备，设置为 false
    }

    // 获取当前采样率
    propertyAddress.mSelector = kAudioDevicePropertyNominalSampleRate;
    propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
    dataSize = sizeof(Float64);
    Float64 sampleRate;

    status = AudioObjectGetPropertyData(
        deviceId,
        &propertyAddress,
        0,
        NULL,
        &dataSize,
        &sampleRate);

    info->sampleRate = (status == noErr) ? (UInt32)sampleRate : 0;

    // 获取数据格式
    propertyAddress.mSelector = kAudioDevicePropertyStreamFormat;
    propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
    dataSize = sizeof(AudioStreamBasicDescription);
    AudioStreamBasicDescription streamFormat;

    status = AudioObjectGetPropertyData(
        deviceId,
        &propertyAddress,
        0,
        NULL,
        &dataSize,
        &streamFormat);

    if (status == noErr)
    {
        info->bitsPerChannel = streamFormat.mBitsPerChannel;
        info->formatFlags = streamFormat.mFormatFlags;
    }

    // 获取设备传输类型
    propertyAddress.mSelector = kAudioDevicePropertyTransportType;
    propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
    dataSize = sizeof(UInt32);
    UInt32 transportType;

    status = AudioObjectGetPropertyData(
        deviceId,
        &propertyAddress,
        0,
        NULL,
        &dataSize,
        &transportType);

    if (status == noErr)
    {
        info->transportType = transportType;
        // 如果是 Continuity Camera，直接设置为不可调节
        if (transportType == kAudioDeviceTransportTypeContinuityCaptureWired ||
            transportType == kAudioDeviceTransportTypeContinuityCaptureWireless)
        {
            info->hasVolumeControl = false;
        }
    }

    // 获取设备是否活跃/正在使用
    propertyAddress.mSelector = kAudioDevicePropertyDeviceIsAlive;
    propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
    dataSize = sizeof(UInt32);
    UInt32 isAlive;
    bool deviceIsActive = false;

    // 根据设备类型检查活跃状态
    if (info->deviceType == kDeviceTypeInput || info->deviceType == kDeviceTypeInputOutput)
    {
        propertyAddress.mScope = kAudioDevicePropertyScopeInput;
        status = AudioObjectGetPropertyData(
            deviceId,
            &propertyAddress,
            0,
            NULL,
            &dataSize,
            &isAlive);

        if (status == noErr && isAlive)
        {
            deviceIsActive = true;
        }
    }

    if ((info->deviceType == kDeviceTypeOutput ||
        info->deviceType == kDeviceTypeInputOutput) && !deviceIsActive)
    {
        propertyAddress.mScope = kAudioDevicePropertyScopeOutput;
        status = AudioObjectGetPropertyData(
            deviceId,
            &propertyAddress,
            0,
            NULL,
            &dataSize,
            &isAlive);

        if (status == noErr && isAlive)
        {
            deviceIsActive = true;
        }
    }

    // 如果设备是活跃的，检查是否正在处理音频
    if (deviceIsActive)
    {
        propertyAddress.mSelector = kAudioDevicePropertyDeviceIsRunning;

        if (info->deviceType == kDeviceTypeInput ||
            info->deviceType == kDeviceTypeInputOutput)
        {
            propertyAddress.mScope = kAudioDevicePropertyScopeInput;
            UInt32 isRunning;
            status = AudioObjectGetPropertyData(
                deviceId,
                &propertyAddress,
                0,
                NULL,
                &dataSize,
                &isRunning);

            if (status == noErr && isRunning)
            {
                info->isRunning = true;
            }
        }

        if ((info->deviceType == kDeviceTypeOutput ||
            info->deviceType == kDeviceTypeInputOutput) && !info->isRunning)
        {
            propertyAddress.mScope = kAudioDevicePropertyScopeOutput;
            UInt32 isRunning;
            status = AudioObjectGetPropertyData(
                deviceId,
                &propertyAddress,
                0,
                NULL,
                &dataSize,
                &isRunning);

            if (status == noErr && isRunning)
            {
                info->isRunning = true;
            }
        }
    }

    // 如果设备没有活跃或运行，尝试检查是否在处理音频
    if (!info->isRunning)
    {
        propertyAddress.mSelector = kAudioDevicePropertyDeviceIsRunningSomewhere;
        propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
        UInt32 isRunning;
        status = AudioObjectGetPropertyData(
            deviceId,
            &propertyAddress,
            0,
            NULL,
            &dataSize,
            &isRunning);

        if (status == noErr && isRunning)
        {
            info->isRunning = true;
        }
    }

    return finalStatus;
}

const char* getTransportTypeName(UInt32 transportType)
{
    switch (transportType)
    {
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

const char* getFormatFlagsDescription(UInt32 formatFlags)
{
    if (formatFlags & kAudioFormatFlagIsFloat) return "Float";
    if (formatFlags & kAudioFormatFlagIsSignedInteger) return "Signed Integer";
    if (formatFlags & kAudioFormatFlagIsNonInterleaved) return "Non-interleaved";
    return "Unknown";
}
