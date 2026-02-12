//
// Created by AhogeK on 02/12/26.
//

#ifndef AUDIOCTL_IPC_SERVER_H
#define AUDIOCTL_IPC_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {

#endif

// ============================================================================
// 客户端状态结构
// ============================================================================

typedef struct IPCClientEntry
{
  pid_t pid;		       // 应用进程ID
  float volume;		       // 当前音量 (0.0 - 1.0)
  bool muted;		       // 静音状态
  uint64_t connected_at;       // 连接时间戳（毫秒）
  char app_name[256];	       // 应用名称
  struct IPCClientEntry *next; // 链表下一个节点
} IPCClientEntry;

// ============================================================================
// 服务端上下文
// ============================================================================

typedef struct IPCServerContext
{
  int listen_fd;	    // 监听 socket
  int epoll_fd;		    // epoll 文件描述符（macOS 使用 kqueue）
  IPCClientEntry *clients;  // 客户端链表头
  uint32_t client_count;    // 客户端数量
  bool running;		    // 运行状态
  uint32_t next_request_id; // 下一个请求ID
} IPCServerContext;

// ============================================================================
// 服务端 API
// ============================================================================

/**
 * 初始化 IPC 服务端
 *
 * @param ctx 服务端上下文指针
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_server_init (IPCServerContext *ctx);

/**
 * 运行 IPC 服务端主循环
 * 阻塞直到接收到停止信号
 *
 * @param ctx 服务端上下文指针
 */
void
ipc_server_run (IPCServerContext *ctx);

/**
 * 停止 IPC 服务端
 *
 * @param ctx 服务端上下文指针
 */
void
ipc_server_stop (IPCServerContext *ctx);

/**
 * 清理 IPC 服务端资源
 *
 * @param ctx 服务端上下文指针
 */
void
ipc_server_cleanup (IPCServerContext *ctx);

/**
 * 查找客户端
 *
 * @param ctx 服务端上下文指针
 * @param pid 目标应用PID
 * @return 找到的客户端条目，未找到返回 NULL
 */
IPCClientEntry *
ipc_server_find_client (IPCServerContext *ctx, pid_t pid);

/**
 * 注册新客户端
 *
 * @param ctx 服务端上下文指针
 * @param pid 应用进程ID
 * @param volume 初始音量
 * @param muted 初始静音状态
 * @param app_name 应用名称
 * @return 成功返回 0，失败返回 -1
 */
int
ipc_server_register_client (IPCServerContext *ctx, pid_t pid, float volume,
			    bool muted, const char *app_name);

/**
 * 注销客户端
 *
 * @param ctx 服务端上下文指针
 * @param pid 应用进程ID
 * @return 成功返回 0，未找到返回 -1
 */
int
ipc_server_unregister_client (IPCServerContext *ctx, pid_t pid);

/**
 * 设置客户端音量
 *
 * @param ctx 服务端上下文指针
 * @param pid 应用进程ID
 * @param volume 音量值 (0.0 - 1.0)
 * @return 成功返回 0，未找到返回 -1
 */
int
ipc_server_set_volume (IPCServerContext *ctx, pid_t pid, float volume);

/**
 * 获取客户端音量
 *
 * @param ctx 服务端上下文指针
 * @param pid 应用进程ID
 * @param volume 输出音量值指针
 * @param muted 输出静音状态指针
 * @return 成功返回 0，未找到返回 -1
 */
int
ipc_server_get_volume (IPCServerContext *ctx, pid_t pid, float *volume,
		       bool *muted);

/**
 * 设置客户端静音状态
 *
 * @param ctx 服务端上下文指针
 * @param pid 应用进程ID
 * @param muted 静音状态
 * @return 成功返回 0，未找到返回 -1
 */
int
ipc_server_set_mute (IPCServerContext *ctx, pid_t pid, bool muted);

/**
 * 获取所有客户端列表
 * 注意：返回的数组需要调用者使用 free() 释放
 *
 * @param ctx 服务端上下文指针
 * @param count 输出客户端数量
 * @return 客户端信息数组，失败返回 NULL
 */
IPCClientEntry *
ipc_server_list_clients (IPCServerContext *ctx, uint32_t *count);

/**
 * 获取客户端数量
 *
 * @param ctx 服务端上下文指针
 * @return 客户端数量
 */
uint32_t
ipc_server_get_client_count (IPCServerContext *ctx);

#ifdef __cplusplus
}
#endif

#endif // AUDIOCTL_IPC_SERVER_H
