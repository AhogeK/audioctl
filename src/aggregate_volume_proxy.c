//
// Aggregate Device 音量代理模块实现
// Created by Agent on 02/11/26.
//

#include "aggregate_volume_proxy.h"
#include "aggregate_device_manager.h"
#include "audio_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

// 代理状态
static bool g_proxy_running = false;
static pthread_mutex_t g_proxy_mutex = PTHREAD_MUTEX_INITIALIZER;

// 音量属性监听地址
static AudioObjectPropertyAddress g_volume_address = {
    .mSelector = kAudioDevicePropertyVolumeScalar,
    .mScope = kAudioDevicePropertyScopeOutput,
    .mElement = kAudioObjectPropertyElementMain
};

static AudioObjectPropertyAddress g_mute_address = {
    .mSelector = kAudioDevicePropertyMute,
    .mScope = kAudioDevicePropertyScopeOutput,
    .mElement = kAudioObjectPropertyElementMain
};

// 前向声明
static OSStatus aggregate_volume_set_physical_device_volume(Float32 volume);
static OSStatus aggregate_volume_set_physical_device_mute(bool isMuted);

#pragma mark - 音量代理生命周期

OSStatus aggregate_volume_proxy_start(void)
{
    // Aggregate Device 本身不支持音量控制属性 (kAudioDevicePropertyVolumeScalar)
    // 所有的音量控制都直接透传给绑定的物理设备
    // 因此不需要注册监听器或启动后台线程

    printf("✅ Aggregate Device 音量代理已就绪\n");
    printf("   现在可以在系统中调节音量，命令将直接控制绑定的物理设备\n");

    return noErr;
}

void aggregate_volume_proxy_stop(void)
{
    // 无需清理
}

#pragma mark - 音量控制

OSStatus aggregate_volume_get(Float32* outVolume)
{
    if (outVolume == NULL)
    {
        return paramErr;
    }

    // 直接读取绑定的物理设备音量
    return aggregate_volume_get_physical_device_volume(outVolume);
}

OSStatus aggregate_volume_set(Float32 volume)
{
    // 限制音量范围
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    // 直接设置绑定的物理设备音量
    // 忽略设置 Aggregate Device 音量（因为它不支持）
    return aggregate_volume_set_physical_device_volume(volume);
}

OSStatus aggregate_volume_get_mute(bool* outIsMuted)
{
    if (outIsMuted == NULL)
    {
        return paramErr;
    }

    // 直接读取绑定的物理设备静音状态
    AudioDeviceID physicalDevice = aggregate_device_get_physical_device();
    if (physicalDevice == kAudioObjectUnknown)
    {
        return kAudioHardwareBadDeviceError;
    }

    UInt32 dataSize = sizeof(UInt32);
    UInt32 isMuted = 0;
    OSStatus status = AudioObjectGetPropertyData(physicalDevice, &g_mute_address,
                                                 0, NULL, &dataSize, &isMuted);

    if (status == noErr)
    {
        *outIsMuted = (isMuted != 0);
    }

    return status;
}

OSStatus aggregate_volume_set_mute(bool isMuted)
{
    // 直接设置绑定的物理设备静音状态
    return aggregate_volume_set_physical_device_mute(isMuted);
}

#pragma mark - 物理设备音量控制

OSStatus aggregate_volume_get_physical_device_volume(Float32* outVolume)
{
    if (outVolume == NULL)
    {
        return paramErr;
    }

    // 获取当前绑定的物理设备
    AudioDeviceID physicalDevice = aggregate_device_get_physical_device();
    if (physicalDevice == kAudioObjectUnknown)
    {
        return kAudioHardwareBadDeviceError;
    }

    // 读取物理设备的音量
    UInt32 dataSize = sizeof(Float32);
    OSStatus status = AudioObjectGetPropertyData(physicalDevice, &g_volume_address,
                                                 0, NULL, &dataSize, outVolume);

    return status;
}

static OSStatus aggregate_volume_set_physical_device_volume(Float32 volume)
{
    // 获取当前绑定的物理设备
    AudioDeviceID physicalDevice = aggregate_device_get_physical_device();
    if (physicalDevice == kAudioObjectUnknown)
    {
        return kAudioHardwareBadDeviceError;
    }

    // 设置物理设备的音量
    OSStatus status = AudioObjectSetPropertyData(physicalDevice, &g_volume_address,
                                                 0, NULL, sizeof(Float32), &volume);

    return status;
}

static OSStatus aggregate_volume_set_physical_device_mute(bool isMuted)
{
    // 获取当前绑定的物理设备
    AudioDeviceID physicalDevice = aggregate_device_get_physical_device();
    if (physicalDevice == kAudioObjectUnknown)
    {
        return kAudioHardwareBadDeviceError;
    }

    UInt32 muteValue = isMuted ? 1 : 0;

    // 设置物理设备的静音状态
    OSStatus status = AudioObjectSetPropertyData(physicalDevice, &g_mute_address,
                                                 0, NULL, sizeof(UInt32), &muteValue);

    return status;
}

#pragma mark - 状态查询

bool aggregate_volume_proxy_is_running(void)
{
    pthread_mutex_lock(&g_proxy_mutex);
    bool running = g_proxy_running;
    pthread_mutex_unlock(&g_proxy_mutex);
    return running;
}

#pragma mark - 内部实现

// (已移除监听器和后台线程逻辑，因为 Aggregate Device 不支持音量属性)
