//
// Created by AhogeK on 12/9/24.
//

#ifndef AUDIOCTL_VIRTUAL_AUDIO_DEVICE_H
#define AUDIOCTL_VIRTUAL_AUDIO_DEVICE_H

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdatomic.h>

// 设备状态枚举
typedef enum {
    DEVICE_STATE_UNINITIALIZED = 0,    // 设备未初始化
    DEVICE_STATE_STOPPED = 1,          // 设备已停止
    DEVICE_STATE_RUNNING = 2,          // 设备正在运行
    DEVICE_STATE_ERROR = 3             // 设备出错
} DeviceState;

// 输出流结构
typedef struct {
    AudioObjectID streamID;                        // 流ID
    AudioStreamBasicDescription format;            // 音频格式
    Boolean isActive;                              // 流是否激活
    AudioBufferList *bufferList;                   // 音频缓冲区
    UInt32 bufferFrameSize;                        // 缓冲区帧大小
} AudioOutputStream;

// 虚拟音频设备结构
typedef struct {
    // 基本标识
    AudioObjectID deviceID;                        // 设备ID
    CFStringRef deviceUID;                         // 设备唯一标识符
    CFStringRef deviceName;                        // 设备名称

    // 设备状态
    DeviceState state;                             // 当前状态
    atomic_bool deviceIsRunning;                   // 设备运行状态
    pthread_mutex_t stateMutex;                    // 状态互斥锁

    // 音频控制
    _Atomic Float32 volumeControlValue;            // 音量控制值（0.0-100.0）
    atomic_bool muteState;                         // 静音状态

    // 输出流
    AudioOutputStream outputStream;                // 输出流

    // 时钟信息
    UInt64 anchorHostTime;                         // 主机时间锚点
    Float64 sampleRate;                            // 当前采样率
} VirtualAudioDevice;

// 设备初始化和清理
OSStatus virtual_device_create(VirtualAudioDevice **outDevice);

void virtual_device_destroy(VirtualAudioDevice *device);


// 设备控制
OSStatus virtual_device_start(VirtualAudioDevice *device);

OSStatus virtual_device_stop(VirtualAudioDevice *device);


// 音频控制
OSStatus virtual_device_set_mute(VirtualAudioDevice *device, Boolean mute);

OSStatus virtual_device_get_mute(const VirtualAudioDevice *device, Boolean *outMute);

OSStatus virtual_device_set_volume(VirtualAudioDevice *device, Float32 volume);

OSStatus virtual_device_get_volume(const VirtualAudioDevice *device, Float32 *outVolume);


// 输出处理
OSStatus virtual_device_process_output(const VirtualAudioDevice *device,
                                       AudioBufferList *outputData,
                                       UInt32 frameCount);


// 状态查询
OSStatus virtual_device_get_state(VirtualAudioDevice *device, DeviceState *outState);

OSStatus virtual_device_is_running(const VirtualAudioDevice *device, Boolean *outIsRunning);

#endif //AUDIOCTL_VIRTUAL_AUDIO_DEVICE_H