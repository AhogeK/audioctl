//
// Aggregate Device 音量代理模块
// 实现 Aggregate Device 音量控制，级联到物理设备
// Created by Agent on 02/11/26.
//

#ifndef AUDIOCTL_AGGREGATE_VOLUME_PROXY_H
#define AUDIOCTL_AGGREGATE_VOLUME_PROXY_H

#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>

#pragma mark - 音量代理生命周期

/**
 * 启动 Aggregate Device 音量代理
 *
 * 此函数启动一个后台线程，监听 Aggregate Device 的音量变化，
 * 并将变化同步到绑定的物理设备。
 *
 * 工作原理：
 *   1. 注册 CoreAudio 属性监听器，监听 Aggregate Device 音量变化
 *   2. 当检测到音量变化时，获取当前绑定的物理设备
 *   3. 将新音量值设置到物理设备
 *   4. 确保 Aggregate Device 的静音状态也同步
 *
 * @return OSStatus 操作状态
 */
OSStatus
aggregate_volume_proxy_start (void);

/**
 * 停止 Aggregate Device 音量代理
 *
 * 清理资源，注销监听器，停止后台线程。
 */
void
aggregate_volume_proxy_stop (void);

#pragma mark - 音量控制

/**
 * 获取 Aggregate Device 当前音量
 *
 * 实际上读取的是绑定的物理设备的音量
 *
 * @param outVolume 输出音量值（0.0 - 1.0）
 * @return OSStatus 操作状态
 */
OSStatus
aggregate_volume_get (Float32 *outVolume);

/**
 * 设置 Aggregate Device 音量
 *
 * 设置音量到 Aggregate Device，并级联到物理设备
 *
 * @param volume 音量值（0.0 - 1.0）
 * @return OSStatus 操作状态
 */
OSStatus
aggregate_volume_set (Float32 volume);

/**
 * 获取 Aggregate Device 静音状态
 *
 * @param outIsMuted 输出静音状态
 * @return OSStatus 操作状态
 */
OSStatus
aggregate_volume_get_mute (bool *outIsMuted);

/**
 * 设置 Aggregate Device 静音状态
 *
 * @param isMuted 静音状态
 * @return OSStatus 操作状态
 */
OSStatus
aggregate_volume_set_mute (bool isMuted);

#pragma mark - 状态查询

/**
 * 检查音量代理是否正在运行
 */
bool
aggregate_volume_proxy_is_running (void);

/**
 * 获取当前绑定的物理设备音量（用于调试）
 */
OSStatus
aggregate_volume_get_physical_device_volume (Float32 *outVolume);

#endif // AUDIOCTL_AGGREGATE_VOLUME_PROXY_H
