//
// 虚拟音频设备管理模块
// 用于检测、控制虚拟设备状态
// Created by AhogeK on 02/05/26.
//

#ifndef AUDIOCTL_VIRTUAL_DEVICE_MANAGER_H
#define AUDIOCTL_VIRTUAL_DEVICE_MANAGER_H

#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>

// 虚拟设备信息
typedef struct
{
  AudioDeviceID deviceId;
  bool isInstalled;
  bool isActive;
  char name[256];
  char uid[256];
} VirtualDeviceInfo;

// 虚拟设备的UID（与驱动中定义的一致）
#define VIRTUAL_DEVICE_UID "0E1D42AE-F2ED-4A48-9624-C770025E32A4"
#define VIRTUAL_DEVICE_NAME "Virtual Audio Device"

#pragma mark - 辅助函数

// 获取当前默认输出设备ID
AudioDeviceID
get_default_output_device (void);

// 获取当前默认输入设备ID
AudioDeviceID
get_default_input_device (void);

#pragma mark - 设备检测

// 检测虚拟设备是否安装
// 通过查找具有特定UID的设备来判断
bool
virtual_device_is_installed (void);

// 获取虚拟设备信息
// 返回 true 表示找到了虚拟设备
bool
virtual_device_get_info (VirtualDeviceInfo *outInfo);

// 检测虚拟设备是否正在作为默认输出设备使用
bool
virtual_device_is_active_output (void);

// 检测虚拟设备是否正在作为默认输入设备使用
bool
virtual_device_is_active_input (void);

// 检测虚拟设备是否正在使用（输入或输出）
bool
virtual_device_is_active (void);

#pragma mark - 设备控制

// 将虚拟设备设为默认输出设备
// 返回 noErr 表示成功
OSStatus
virtual_device_set_as_default_output (void);

// 将虚拟设备设为默认输入设备
OSStatus
virtual_device_set_as_default_input (void);

// 切换到虚拟设备（同时设为默认输入和输出）
OSStatus
virtual_device_activate (void);

// 【新架构】激活虚拟设备并启动 Router（串联模式）
// App -> Virtual Device -> Router -> Physical Device
OSStatus
virtual_device_activate_with_router (void);

// 恢复到物理设备（查找第一个非虚拟设备设为默认）
OSStatus
virtual_device_deactivate (void);

#pragma mark - Router 进程检测

// 检测 Router 进程是否正在运行（通过查找 audioctl internal-route 进程）
bool
is_router_process_running (void);

#pragma mark - 绑定信息持久化

// 保存绑定的物理设备 UID
OSStatus
save_bound_physical_device (const char *physicalUid);

// 获取绑定的物理设备 UID
bool
get_bound_physical_device_uid (char *uid, size_t uidSize);

// 清除绑定信息
void
clear_binding_info (void);

#pragma mark - 状态报告

// 打印虚拟设备状态信息
void
virtual_device_print_status (void);

// 获取当前默认输出设备的信息
OSStatus
virtual_device_get_current_output_info (VirtualDeviceInfo *outInfo);

#pragma mark - 应用音量控制前置检查

// 检查是否可以进行应用音量控制
// 返回 true 表示虚拟设备已安装且正在使用，可以进行应用音量控制
bool
virtual_device_can_control_app_volume (void);

// 获取应用音量控制的可用性描述
// 返回一个描述字符串，说明为什么可以或不可以进行应用音量控制
const char *
virtual_device_get_app_volume_status (void);

#endif // AUDIOCTL_VIRTUAL_DEVICE_MANAGER_H
