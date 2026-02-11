//
// Created by AhogeK on 02/12/26.
//

#ifndef AUDIOCTL_IPC_PROTOCOL_H
#define AUDIOCTL_IPC_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {

#endif

// ============================================================================
// Socket 配置
// ============================================================================

#define IPC_SOCKET_FILENAME "daemon.sock"
#define IPC_SOCKET_BACKLOG  16
#define IPC_MAX_PAYLOAD_SIZE 4096
#define IPC_PROTOCOL_VERSION 1

// ============================================================================
// 消息头 (16 bytes，紧凑对齐)
// ============================================================================

typedef struct __attribute__((packed))
{
    uint32_t magic; // 'AIPC' (0x41495043)
    uint16_t version; // 协议版本
    uint16_t command; // 指令类型
    uint32_t payload_len; // 负载长度
    uint32_t request_id; // 请求ID（用于异步匹配）
} IPCMessageHeader;

// 魔数常量
#define IPC_MAGIC 0x41495043  // 'AIPC'

// ============================================================================
// 指令集 (Command Types)
// ============================================================================

typedef enum : uint16_t
{
    // 客户端注册/注销
    kIPCCommandRegister = 0x0001, // 应用注册（驱动调用）
    kIPCCommandUnregister = 0x0002, // 应用注销

    // 音量控制
    kIPCCommandGetVolume = 0x0100, // 获取应用音量
    kIPCCommandSetVolume = 0x0101, // 设置应用音量
    kIPCCommandGetMute = 0x0102, // 获取静音状态
    kIPCCommandSetMute = 0x0103, // 设置静音状态

    // 状态查询
    kIPCCommandListClients = 0x0200, // 列出所有连接的客户端
    kIPCCommandPing = 0x0201, // 心跳检测

    // 响应
    kIPCCommandResponse = 0x8000, // 通用响应
    kIPCCommandError = 0x8001, // 错误响应
} IPCCommand;

// ============================================================================
// 状态码
// ============================================================================

typedef enum : int32_t
{
    kIPCStatusOK = 0, // 成功
    kIPCStatusInvalidHeader = -1, // 无效的消息头
    kIPCStatusUnknownCommand = -2, // 未知指令
    kIPCStatusPayloadTooLarge = -3, // 负载过大
    kIPCStatusClientNotFound = -4, // 客户端未找到
    kIPCStatusInvalidVolume = -5, // 无效音量值
    kIPCStatusServiceUnavailable = -6, // 服务不可用
    kIPCStatusInternalError = -7, // 内部错误
} IPCStatus;

// ============================================================================
// 数据结构
// ============================================================================

// 应用注册请求（驱动 -> 服务）
typedef struct __attribute__((packed))
{
    pid_t pid; // 应用进程ID
    float initial_volume; // 初始音量 (0.0 - 1.0)
    bool muted; // 初始静音状态
    // 变长字段：应用名称字符串（以null结尾）
} IPCRegisterRequest;

// 音量设置请求
typedef struct __attribute__((packed))
{
    pid_t pid; // 目标应用PID
    float volume; // 音量值 (0.0 - 1.0)
} IPCSetVolumeRequest;

// 静音设置请求
typedef struct __attribute__((packed))
{
    pid_t pid; // 目标应用PID
    bool muted; // 静音状态
} IPCSetMuteRequest;

// 客户端信息（用于 ListClients）
typedef struct __attribute__((packed))
{
    pid_t pid; // 进程ID
    float volume; // 当前音量
    bool muted; // 静音状态
    uint64_t connected_at; // 连接时间戳（毫秒）
    // 变长字段：应用名称字符串
} IPCClientInfo;

// 通用响应
typedef struct __attribute__((packed))
{
    int32_t status; // IPCStatus
    uint32_t data_len; // 附加数据长度
    // 变长字段：附加数据
} IPCResponse;

// 音量查询响应
typedef struct __attribute__((packed))
{
    int32_t status; // IPCStatus
    float volume; // 音量值
    bool muted; // 静音状态
} IPCVolumeResponse;

// ============================================================================
// 工具函数
// ============================================================================

/**
 * 获取 IPC Socket 文件完整路径
 * 路径: ~/Library/Application Support/audioctl/daemon.sock
 *
 * @param path 输出缓冲区
 * @param path_size 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int get_ipc_socket_path(char* path, size_t path_size);

/**
 * 初始化消息头
 *
 * @param header 消息头指针
 * @param command 指令类型
 * @param payload_len 负载长度
 * @param request_id 请求ID（0表示自动分配）
 */
void ipc_init_header(IPCMessageHeader* header, uint16_t command, uint32_t payload_len,
                     uint32_t request_id);

/**
 * 验证消息头有效性
 *
 * @param header 消息头指针
 * @return 有效返回 true，无效返回 false
 */
bool ipc_validate_header(const IPCMessageHeader* header);

/**
 * 将状态码转换为可读字符串
 *
 * @param status 状态码
 * @return 状态描述字符串
 */
const char* ipc_status_to_string(int32_t status);

#ifdef __cplusplus
}
#endif

#endif //AUDIOCTL_IPC_PROTOCOL_H
