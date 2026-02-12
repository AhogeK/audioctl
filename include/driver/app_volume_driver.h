//
// 驱动端应用音量控制 - 极简安全版（无共享内存）
// Created by AhogeK on 02/05/26.
//

#ifndef AUDIOCTL_APP_VOLUME_DRIVER_H
#define AUDIOCTL_APP_VOLUME_DRIVER_H

#include <stdbool.h>
#include "audio_common_types.h"

#pragma mark - 初始化和清理

// 初始化客户端音量管理器
void
app_volume_driver_init (void);

// 清理客户端音量管理器
void
app_volume_driver_cleanup (void);

#pragma mark - 客户端管理

// 添加客户端（bundleId 和 name 暂时保留参数但内部未使用）
OSStatus
app_volume_driver_add_client (UInt32 clientID, pid_t pid, const char *bundleId,
			      const char *name);

// 移除客户端
OSStatus
app_volume_driver_remove_client (UInt32 clientID);

// 根据ClientID查找PID
pid_t
app_volume_driver_get_pid (UInt32 clientID);

#pragma mark - 属性访问

// 设置整个音量表 (来自 SetPropertyData)
OSStatus
app_volume_driver_set_table (const AppVolumeTable *table);

// 获取整个音量表 (来自 GetPropertyData)
OSStatus
app_volume_driver_get_table (AppVolumeTable *table);

// 获取当前连接的客户端PID列表
// outPids: 调用者提供的缓冲区
// maxCount: 缓冲区最大容量
// outActualCount: 实际返回的PID数量
OSStatus
app_volume_driver_get_client_pids (pid_t *outPids, UInt32 maxCount,
				   UInt32 *outActualCount);

#pragma mark - 音量应用

// 获取指定客户端的音量（简化版本返回默认值）
Float32
app_volume_driver_get_volume (UInt32 clientID, bool *outIsMuted);

// 应用音量到音频缓冲区
void
app_volume_driver_apply_volume (UInt32 clientID, void *buffer,
				UInt32 frameCount, UInt32 channels);

// 应用音量到 Non-Interleaved 音频缓冲区（左右声道分离）
void
app_volume_driver_apply_volume_ni (UInt32 clientID, void *leftBuffer,
				   void *rightBuffer, UInt32 frameCount);

#endif // AUDIOCTL_APP_VOLUME_DRIVER_H
