//
// Created by AhogeK on 12/9/24.
//

#include "driver/virtual_audio_device.h"
#include <pthread.h>

// 默认音频格式配置
static const AudioStreamBasicDescription kDefaultAudioFormat = {
        .mSampleRate = 48000.0,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 8,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = 8,
        .mChannelsPerFrame = 2,
        .mBitsPerChannel = 32
};

// 设备初始化
OSStatus virtual_device_create(VirtualAudioDevice **outDevice) {
    if (outDevice == NULL) {
        return kAudioHardwareIllegalOperationError;
    }

    // 分配设备结构体内存
    VirtualAudioDevice *device = (VirtualAudioDevice *) calloc(1, sizeof(VirtualAudioDevice));
    if (device == NULL) {
        return kAudio_MemFullError;
    }

    // 初始化基本属性
    device->deviceID = arc4random();
    device->deviceUID = CFStringCreateWithCString(NULL, "com.ahogek.virtualaudiodriver", kCFStringEncodingUTF8);
    device->deviceName = CFStringCreateWithCString(NULL, "Virtual Audio Driver", kCFStringEncodingUTF8);

    // 初始化状态
    device->state = DEVICE_STATE_STOPPED;
    atomic_store(&device->deviceIsRunning, false);
    pthread_mutex_init(&device->stateMutex, NULL);

    // 初始化音量为 100.0（最大值）
    atomic_store(&device->volumeControlValue, 100.0f);
    atomic_store(&device->muteState, false);

    // 初始化输出流
    device->outputStream.streamID = arc4random();
    device->outputStream.format = kDefaultAudioFormat;
    device->outputStream.isActive = false;
    device->outputStream.bufferFrameSize = 512; // 音频缓冲区大小设置为512帧

    // 分配音频缓冲区
    device->outputStream.bufferList = (AudioBufferList *) calloc(1,
                                                                 sizeof(AudioBufferList) + sizeof(AudioBuffer));
    if (device->outputStream.bufferList == NULL) {
        virtual_device_destroy(device);
        return kAudio_MemFullError;
    }

    device->outputStream.bufferList->mNumberBuffers = 1;
    device->outputStream.bufferList->mBuffers[0].mNumberChannels = 2;
    device->outputStream.bufferList->mBuffers[0].mDataByteSize =
            device->outputStream.bufferFrameSize * sizeof(Float32) * 2;
    device->outputStream.bufferList->mBuffers[0].mData =
            calloc(1, device->outputStream.bufferList->mBuffers[0].mDataByteSize);

    if (device->outputStream.bufferList->mBuffers[0].mData == NULL) {
        virtual_device_destroy(device);
        return kAudio_MemFullError;
    }

    *outDevice = device;
    return kAudioHardwareNoError;
}

// 设备销毁
void virtual_device_destroy(VirtualAudioDevice *device) {
    if (device != NULL) {
        // 停止设备
        if (atomic_load(&device->deviceIsRunning)) {
            virtual_device_stop(device);
        }

        // 清理音频缓冲区
        if (device->outputStream.bufferList != NULL) {
            if (device->outputStream.bufferList->mBuffers[0].mData != NULL) {
                free(device->outputStream.bufferList->mBuffers[0].mData);
            }
            free(device->outputStream.bufferList);
        }

        // 清理 CFString 对象
        if (device->deviceUID != NULL) {
            CFRelease(device->deviceUID);
        }
        if (device->deviceName != NULL) {
            CFRelease(device->deviceName);
        }

        // 清理互斥锁
        pthread_mutex_destroy(&device->stateMutex);

        // 释放设备结构题
        free(device);
    }
}

// 设备启动
OSStatus virtual_device_start(VirtualAudioDevice *device) {
    // 参数检查
    if (device == NULL) {
        return kAudioHardwareIllegalOperationError;
    }

    // 获取状态锁
    pthread_mutex_lock(&device->stateMutex);

    // 检查当前状态
    if (device->state == DEVICE_STATE_RUNNING) {
        pthread_mutex_unlock(&device->stateMutex);
        return kAudioHardwareNoError; // 设备已经在运行
    }

    // 检查设备是否处于可启动状态
    if (device->state != DEVICE_STATE_STOPPED) {
        pthread_mutex_unlock(&device->stateMutex);
        return kAudioHardwareIllegalOperationError;
    }

    // 初始化音频输出流
    if (!device->outputStream.isActive) {
        // 清空音频缓冲区
        if (device->outputStream.bufferList != NULL &&
            device->outputStream.bufferList->mBuffers[0].mData != NULL) {
            memset(device->outputStream.bufferList->mBuffers[0].mData, 0,
                   device->outputStream.bufferList->mBuffers[0].mDataByteSize);
        }

        device->outputStream.isActive = true;
    }

    // 更新设备状态
    device->state = DEVICE_STATE_RUNNING;
    atomic_store(&device->deviceIsRunning, true);

    // 释放状态锁
    pthread_mutex_unlock(&device->stateMutex);

    return kAudioHardwareNoError;
}

// 设备停止
OSStatus virtual_device_stop(VirtualAudioDevice *device) {
    // 参数检查
    if (device == NULL) {
        return kAudioHardwareIllegalOperationError;
    }

    // 获取状态锁
    pthread_mutex_lock(&device->stateMutex);

    // 检查当前状态
    if (device->state == DEVICE_STATE_STOPPED) {
        pthread_mutex_unlock(&device->stateMutex);
        return kAudioHardwareNoError; // 设备已经停止
    }

    // 检查设备是否处于可停止状态
    if (device->state != DEVICE_STATE_RUNNING) {
        pthread_mutex_unlock(&device->stateMutex);
        return kAudioHardwareIllegalOperationError;
    }

    // 停止音频输出流
    if (device->outputStream.isActive) {
        // 停止数据流
        device->outputStream.isActive = false;

        // 清空音频缓冲区
        if (device->outputStream.bufferList != NULL &&
            device->outputStream.bufferList->mBuffers[0].mData != NULL) {
            memset(device->outputStream.bufferList->mBuffers[0].mData, 0,
                   device->outputStream.bufferList->mBuffers[0].mDataByteSize);
        }
    }

    // 更新设备状态
    device->state = DEVICE_STATE_STOPPED;
    atomic_store(&device->deviceIsRunning, false);

    // 释放状态锁
    pthread_mutex_unlock(&device->stateMutex);

    return kAudioHardwareNoError;
}

// 设置静音状态
OSStatus virtual_device_set_mute(VirtualAudioDevice *device, Boolean mute) {
    if (device == NULL) {
        return kAudioHardwareIllegalOperationError;
    }

    // 获取状态锁
    pthread_mutex_lock(&device->stateMutex);

    // 设置静音状态
    atomic_store(&device->muteState, mute);

    // 释放状态锁
    pthread_mutex_unlock(&device->stateMutex);

    return kAudioHardwareNoError;
}

// 获取静音状态
OSStatus virtual_device_get_mute(const VirtualAudioDevice *device, Boolean *outMute) {
    if (device == NULL || outMute == NULL) {
        return kAudioHardwareIllegalOperationError;
    }

    // 获取静音状态
    *outMute = atomic_load(&device->muteState);

    return kAudioHardwareNoError;
}

// 设置音量
OSStatus virtual_device_set_volume(VirtualAudioDevice *device, Float32 volume) {
    if (device == NULL) {
        return kAudioHardwareIllegalOperationError;
    }

    // 检查音量范围 (0.0-100.0)
    if (volume < 0.0f || volume > 100.0f) {
        return kAudioHardwareIllegalOperationError;
    }

    // 获取状态锁
    pthread_mutex_lock(&device->stateMutex);

    // 直接存储浮点数值
    atomic_store(&device->volumeControlValue, volume);

    // 释放状态锁
    pthread_mutex_unlock(&device->stateMutex);

    return kAudioHardwareNoError;
}

// 获取音量
OSStatus virtual_device_get_volume(const VirtualAudioDevice *device, Float32 *outVolume) {
    if (device == NULL || outVolume == NULL) {
        return kAudioHardwareIllegalOperationError;
    }

    // 直接获取浮点数值
    *outVolume = atomic_load(&device->volumeControlValue);

    return kAudioHardwareNoError;
}


void apply_volume_and_clamp(Float32 *samples, UInt32 sampleCount, Float32 volumeScale) {
    for (UInt32 i = 0; i < sampleCount; i++) {
        samples[i] *= volumeScale;

        // 防止音频信号过载
        if (samples[i] > 1.0f) {
            samples[i] = 1.0f;
        } else if (samples[i] < -1.0f) {
            samples[i] = -1.0f;
        }
    }
}

// 输出处理
OSStatus virtual_device_process_output(const VirtualAudioDevice *device,
                                       AudioBufferList *outputData,
                                       UInt32 frameCount) {
    if (device == NULL || outputData == NULL || frameCount == 0) {
        return kAudioHardwareIllegalOperationError;
    }

    if (device->state != DEVICE_STATE_RUNNING) {
        return kAudioHardwareNotRunningError;
    }

    // 获取当前音量和静音状态
    Float32 currentVolume = atomic_load(&device->volumeControlValue);
    Boolean isMuted = atomic_load(&device->muteState);

    // 音量系数 (0.0 - 1.0)
    Float32 volumeScale = isMuted ? 0.0f : (currentVolume / 100.0f);

    // 处理每个缓冲区
    for (UInt32 bufferIndex = 0; bufferIndex < outputData->mNumberBuffers; bufferIndex++) {
        AudioBuffer *buffer = &outputData->mBuffers[bufferIndex];
        Float32 *samples = (Float32 *) buffer->mData;
        UInt32 sampleCount = frameCount * buffer->mNumberChannels;

        // 如果完全静音或音量为0，直接清零
        if (isMuted || currentVolume <= 0.0f) {
            memset(samples, 0, sampleCount * sizeof(Float32));
        } else {
            apply_volume_and_clamp(samples, sampleCount, volumeScale);
        }

        // 更新已处理的字节数
        buffer->mDataByteSize = sampleCount * sizeof(Float32);
    }

    return kAudioHardwareNoError;
}

// 获取设备状态
OSStatus virtual_device_get_state(VirtualAudioDevice *device, DeviceState *outState) {
    if (device == NULL || outState == NULL) {
        return kAudioHardwareIllegalOperationError;
    }

    // 使用互斥锁保护状态访问
    pthread_mutex_lock(&device->stateMutex);
    *outState = device->state;
    pthread_mutex_unlock(&device->stateMutex);

    return kAudioHardwareNoError;
}

// 获取设备运行状态
OSStatus virtual_device_is_running(const VirtualAudioDevice *device, Boolean *outIsRunning) {
    if (device == NULL || outIsRunning == NULL) {
        return kAudioHardwareIllegalOperationError;
    }

    // 使用原子操作获取运行状态
    *outIsRunning = atomic_load(&device->deviceIsRunning);
    return kAudioHardwareNoError;
}