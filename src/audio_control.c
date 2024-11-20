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

    // 检查设备是输入还是输出设备
    propertyAddress.mSelector = kAudioDevicePropertyStreamConfiguration;

    // 首先检查是否有输入流
    propertyAddress.mScope = kAudioDevicePropertyScopeInput;
    UInt32 inputDataSize = 0;
    status = AudioObjectGetPropertyDataSize(
        deviceId,
        &propertyAddress,
        0,
        NULL,
        &inputDataSize);

    bool hasInput = (status == noErr && inputDataSize > 0);

    // 然后检查是否有输出流
    propertyAddress.mScope = kAudioDevicePropertyScopeOutput;
    UInt32 outputDataSize = 0;
    status = AudioObjectGetPropertyDataSize(
        deviceId,
        &propertyAddress,
        0,
        NULL,
        &outputDataSize);

    bool hasOutput = (status == noErr && outputDataSize > 0);

    // 设置设备类型
    if (hasInput && hasOutput)
    {
        info->deviceType = kDeviceTypeInputOutput;
    }
    else if (hasInput)
    {
        info->deviceType = kDeviceTypeInput;
    }
    else if (hasOutput)
    {
        info->deviceType = kDeviceTypeOutput;
    }
    else
    {
        info->deviceType = kDeviceTypeUnknown;
    }

    // 获取音量
    Float32 volume = 0.0f;
    bool volumeValid = false;

    if (hasOutput)
    {
        // 首先检查设备是否支持音量控制
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

    // 对输入设备也进行类似的检查
    if (!volumeValid && hasInput)
    {
        propertyAddress.mSelector = kAudioDevicePropertyVolumeScalar;
        propertyAddress.mScope = kAudioDevicePropertyScopeInput;
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

    // 如果上面的方法都失败了，尝试获取虚拟主音量
    if (!volumeValid)
    {
        propertyAddress.mSelector = kAudioDevicePropertyVolumeScalar;
        propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
        propertyAddress.mElement = kAudioObjectPropertyElementMain;
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

    // 如果还是获取不到，尝试获取解码器音量
    if (!volumeValid)
    {
        propertyAddress.mSelector = kAudioDevicePropertyVolumeDecibels;
        status = AudioObjectGetPropertyData(
            deviceId,
            &propertyAddress,
            0,
            NULL,
            &dataSize,
            &volume);

        if (status == noErr)
        {
            // 将分贝转换为标量值 (0.0 - 1.0)
            if (volume > 0.0f)
            {
                info->volume = 1.0f;
            }
            else if (volume < -96.0f)
            {
                info->volume = 0.0f;
            }
            else
            {
                info->volume = (volume + 96.0f) / 96.0f;
            }
            volumeValid = true;
        }
    }

    // 获取静音状态
    propertyAddress.mSelector = kAudioDevicePropertyMute;
    propertyAddress.mScope = kAudioDevicePropertyScopeOutput;
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

    // 获取通道数量（分别获取输入和输出通道）
    if (hasInput && inputDataSize > 0)
    {
        AudioBufferList* bufferList = (AudioBufferList*)malloc(inputDataSize);
        if (bufferList)
        {
            propertyAddress.mScope = kAudioDevicePropertyScopeInput;
            propertyAddress.mSelector = kAudioDevicePropertyStreamConfiguration;
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

    if (hasOutput && outputDataSize > 0)
    {
        AudioBufferList* bufferList = (AudioBufferList*)malloc(outputDataSize);
        if (bufferList)
        {
            propertyAddress.mScope = kAudioDevicePropertyScopeOutput;
            propertyAddress.mSelector = kAudioDevicePropertyStreamConfiguration;
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

    // 更新总通道数
    info->channelCount = info->inputChannelCount + info->outputChannelCount;

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
    }

    // 获取设备是否活跃/正在使用
    propertyAddress.mSelector = kAudioDevicePropertyDeviceIsAlive;
    propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
    dataSize = sizeof(UInt32);
    UInt32 isAlive;
    bool deviceIsActive = false;

    // 检查输入状态
    if (hasInput)
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

    // 检查输出状态
    if (hasOutput && !deviceIsActive)
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
        UInt32 isRunning;

        if (hasInput)
        {
            propertyAddress.mScope = kAudioDevicePropertyScopeInput;
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

        if (hasOutput && !info->isRunning)
        {
            propertyAddress.mScope = kAudioDevicePropertyScopeOutput;
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
