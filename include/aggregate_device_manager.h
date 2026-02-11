//
// Aggregate Device 管理模块
// 用于创建/管理虚拟设备与物理设备的聚合
// Created by AhogeK on 02/05/26.
//

#ifndef AUDIOCTL_AGGREGATE_DEVICE_MANAGER_H
#define AUDIOCTL_AGGREGATE_DEVICE_MANAGER_H

#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>

// Aggregate Device 信息
typedef struct
{
    AudioDeviceID deviceId;
    bool isCreated;
    bool isActive;
    char name[256];
    char uid[256];
    AudioDeviceID subDevices[8]; // 包含的子设备
    UInt32 subDeviceCount;
} AggregateDeviceInfo;

// Aggregate Device 的 UID 前缀
#define AGGREGATE_DEVICE_UID_PREFIX "audioctl-aggregate"
#define AGGREGATE_DEVICE_NAME "AudioCTL Aggregate"

#pragma mark - 生命周期管理

// 初始化管理模块（注册监听器）
OSStatus aggregate_device_init(void);

// 清理管理模块（移除监听器）
void aggregate_device_cleanup(void);

#pragma mark - 设备检测

// 检测 Aggregate Device 是否已创建
bool aggregate_device_is_created(void);

// 获取 Aggregate Device 信息
bool aggregate_device_get_info(AggregateDeviceInfo* outInfo);

// 检测 Aggregate Device 是否正在使用
bool aggregate_device_is_active(void);

#pragma mark - 设备管理

// 创建 Aggregate Device
// 将虚拟设备和指定的物理输出设备组合
// 如果 physicalDeviceID 为 kAudioObjectUnknown，则自动选择第一个可用物理设备
OSStatus aggregate_device_create(AudioDeviceID physicalDeviceID);

// 销毁 Aggregate Device
OSStatus aggregate_device_destroy(void);

// 更新 Aggregate Device 的物理设备
// 当用户想切换输出到的物理设备时使用
OSStatus aggregate_device_update_physical_device(AudioDeviceID newPhysicalDeviceID);

#pragma mark - 激活/停用

// 激活 Aggregate Device（设为默认输出）
OSStatus aggregate_device_activate(void);

// 停用 Aggregate Device（恢复物理设备）
OSStatus aggregate_device_deactivate(void);

#pragma mark - 状态报告

// 打印 Aggregate Device 状态
void aggregate_device_print_status(void);

// 获取当前系统默认输出设备
AudioDeviceID aggregate_device_get_current_default_output(void);

// 获取推荐的物理设备（优先当前默认输出设备，如果是虚拟/聚合设备则选其他可用设备）
AudioDeviceID aggregate_device_get_recommended_physical_device(void);

#pragma mark - 辅助功能

// 检查 Aggregate Device 是否包含虚拟设备
bool aggregate_device_contains_virtual(const AggregateDeviceInfo* info);

// 检查 Aggregate Device 是否包含指定的物理设备
bool aggregate_device_contains_physical(const AggregateDeviceInfo* info, AudioDeviceID physicalDevice);

// 获取 Aggregate Device 中使用的物理设备
AudioDeviceID aggregate_device_get_physical_device(void);

#endif //AUDIOCTL_AGGREGATE_DEVICE_MANAGER_H
