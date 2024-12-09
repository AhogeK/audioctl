//
// Created by AhogeK on 12/9/24.
//

#include "virtual_audio_device.h"
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
    device->deviceUID = CFStringCreateWithCString(NULL, "com.ahogek.virtual.device", kCFStringEncodingUTF8);
    device->deviceName = CFStringCreateWithCString(NULL, "Virtual Audio Device", kCFStringEncodingUTF8);

    // 初始化状态
    device->state = DEVICE_STATE_STOPPED;
    atomic_store(&device->deviceIsRunning, false);
    pthread_mutex_init(&device->stateMutex, NULL);

    // 初始化音频控制
    atomic_store(&device->volumeControlValue, 100);
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
//            virtual_device_stop(device);
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