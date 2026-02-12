//
// Created by AhogeK on 02/12/26.
//

#ifndef AUDIOCTL_IPC_CLIENT_H
#define AUDIOCTL_IPC_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "ipc/ipc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 客户端上下文
// ============================================================================

typedef struct IPCClientContext
{
  int fd;		    // Socket 文件描述符
  bool connected;	    // 连接状态
  uint64_t last_activity;   // 最后活动时间戳
  int reconnect_attempts;   // 重连尝试次数
  pid_t cached_pid;	    // 缓存的 PID（用于驱动快速查询）
  float cached_volume;	    // 缓存的音量值
  bool cached_muted;	    // 缓存的静音状态
  uint64_t cache_timestamp; // 缓存时间戳
  bool cache_valid;	    // 缓存是否有效
} IPCClientContext;

// ============================================================================
// 客户端 API
// ============================================================================

/**
 * 初始化 IPC 客户端
 *
 * @param ctx 客户端上下文指针
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_init (IPCClientContext *ctx);

/**
 * 连接到 IPC 服务端
 *
 * @param ctx 客户端上下文指针
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_connect (IPCClientContext *ctx);

/**
 * 断开与服务端的连接
 *
 * @param ctx 客户端上下文指针
 */
void
ipc_client_disconnect (IPCClientContext *ctx);

/**
 * 清理客户端资源
 *
 * @param ctx 客户端上下文指针
 */
void
ipc_client_cleanup (IPCClientContext *ctx);

/**
 * 检查是否已连接
 *
 * @param ctx 客户端上下文指针
 * @return 已连接返回 true，否则返回 false
 */
bool
ipc_client_is_connected (IPCClientContext *ctx);

/**
 * 发送消息到服务端
 *
 * @param ctx 客户端上下文指针
 * @param header 消息头
 * @param payload 消息负载（可为 NULL）
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_send (IPCClientContext *ctx, const IPCMessageHeader *header,
		 const void *payload);

/**
 * 接收服务端响应
 *
 * @param ctx 客户端上下文指针
 * @param header 输出消息头
 * @param payload 输出负载缓冲区
 * @param payload_size 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_recv (IPCClientContext *ctx, IPCMessageHeader *header, void *payload,
		 size_t payload_size);

/**
 * 发送请求并等待响应（同步调用）
 *
 * @param ctx 客户端上下文指针
 * @param request_header 请求头
 * @param request_payload 请求负载
 * @param response_header 输出响应头
 * @param response_payload 输出响应负载
 * @param response_size 响应缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_send_sync (IPCClientContext *ctx,
		      const IPCMessageHeader *request_header,
		      const void *request_payload,
		      IPCMessageHeader *response_header, void *response_payload,
		      size_t response_size);

// ============================================================================
// 驱动专用快速查询 API（带原子缓存）
// ============================================================================

/**
 * 快速获取音量（带缓存）
 * 用于驱动 IOProc 回调，确保非阻塞
 *
 * @param ctx 客户端上下文指针
 * @param pid 目标应用 PID
 * @param volume 输出音量值
 * @param muted 输出静音状态
 * @return 成功返回 0，失败返回 -1（使用缓存值）
 */
int
ipc_client_get_volume_fast (IPCClientContext *ctx, pid_t pid, float *volume,
			    bool *muted);

/**
 * 更新本地缓存
 * 后台线程定期调用，保持缓存新鲜
 *
 * @param ctx 客户端上下文指针
 * @param pid 目标应用 PID
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_refresh_cache (IPCClientContext *ctx, pid_t pid);

/**
 * 设置本地缓存值（用于驱动初始化）
 *
 * @param ctx 客户端上下文指针
 * @param pid 应用 PID
 * @param volume 音量值
 * @param muted 静音状态
 */
void
ipc_client_set_cache (IPCClientContext *ctx, pid_t pid, float volume,
		      bool muted);

// ============================================================================
// 高级 API - 应用管理
// ============================================================================

/**
 * 注册应用（驱动调用）
 *
 * @param ctx 客户端上下文指针
 * @param pid 应用进程ID
 * @param app_name 应用名称
 * @param initial_volume 初始音量
 * @param muted 初始静音状态
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_register_app (IPCClientContext *ctx, pid_t pid, const char *app_name,
			 float initial_volume, bool muted);

/**
 * 注销应用
 *
 * @param ctx 客户端上下文指针
 * @param pid 应用进程ID
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_unregister_app (IPCClientContext *ctx, pid_t pid);

/**
 * 获取应用音量
 *
 * @param ctx 客户端上下文指针
 * @param pid 应用进程ID
 * @param volume 输出音量值
 * @param muted 输出静音状态
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_get_app_volume (IPCClientContext *ctx, pid_t pid, float *volume,
			   bool *muted);

/**
 * 设置应用音量
 *
 * @param ctx 客户端上下文指针
 * @param pid 应用进程ID
 * @param volume 音量值
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_set_app_volume (IPCClientContext *ctx, pid_t pid, float volume);

/**
 * 设置应用静音状态
 *
 * @param ctx 客户端上下文指针
 * @param pid 应用进程ID
 * @param muted 静音状态
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_set_app_mute (IPCClientContext *ctx, pid_t pid, bool muted);

/**
 * Ping 服务端（保活检测）
 *
 * @param ctx 客户端上下文指针
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_ping (IPCClientContext *ctx);

// ============================================================================
// 应用列表查询
// ============================================================================

/**
 * 应用信息条目（用于列表查询）
 */
typedef struct IPCAppInfo
{
  pid_t pid;		 // 进程ID
  float volume;		 // 当前音量 (0.0 - 1.0)
  bool muted;		 // 静音状态
  uint64_t connected_at; // 连接时间戳
  char app_name[256];	 // 应用名称
} IPCAppInfo;

/**
 * 获取所有已注册的应用列表
 *
 * @param ctx 客户端上下文指针
 * @param apps 输出应用列表数组（需要调用者使用 free() 释放）
 * @param count 输出应用数量
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_list_apps (IPCClientContext *ctx, IPCAppInfo **apps,
		      uint32_t *count);

// ============================================================================
// 自动重连机制
// ============================================================================

/**
 * 尝试自动重连
 * 内置指数退避策略
 *
 * @param ctx 客户端上下文指针
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_client_reconnect (IPCClientContext *ctx);

/**
 * 检查是否需要重连
 *
 * @param ctx 客户端上下文指针
 * @return 需要重连返回 true，否则返回 false
 */
bool
ipc_client_should_reconnect (IPCClientContext *ctx);

/**
 * 重置重连计数器
 *
 * @param ctx 客户端上下文指针
 */
void
ipc_client_reset_reconnect (IPCClientContext *ctx);

#ifdef __cplusplus
}
#endif

#endif // AUDIOCTL_IPC_CLIENT_H
