//
// Created by AhogeK on 11/22/24.
//

#ifndef AUDIOCTL_CONSTANTS_H
#define AUDIOCTL_CONSTANTS_H

#include <stddef.h>
#include <sys/stat.h>

// 服务信息
#define SERVICE_NAME "AudioCTL"

#ifndef AUDIOCTL_VERSION
#define SERVICE_VERSION "unknown"
#else
#define SERVICE_VERSION AUDIOCTL_VERSION
#endif

// PID 文件和日志文件目录
// 使用 ~/Library/Application Support/audioctl/ 替代 /tmp 和 /var/run
// 该目录在 macOS 上更稳定，不会被定期清理，也不需要 root 权限
#define PID_FILENAME "audioctl.pid"
#define LOG_FILENAME "audioctl.log"
#define LOCK_FILENAME "audioctl.lock"

// 目录权限: 755 (用户完全控制，组和其他人可读可执行)
#define DIR_MODE (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)
// 文件权限: 644 (用户读写，组和其他人只读)
#define FILE_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

// 定义 ANSI 颜色常量
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_BOLD_GREEN "\033[1;32m"
#define ANSI_COLOR_BOLD_RED "\033[1;31m"
#define ANSI_COLOR_RESET "\x1b[0m"

/**
 * 获取 audioctl 支持目录路径
 * 返回 ~/Library/Application Support/audioctl/
 * 如果目录不存在会自动创建
 *
 * @param path 输出缓冲区
 * @param path_size 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int get_support_directory(char* path, size_t path_size);

/**
 * 获取 PID 文件完整路径
 *
 * @param path 输出缓冲区
 * @param path_size 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int get_pid_file_path(char* path, size_t path_size);

/**
 * 获取日志文件完整路径
 *
 * @param path 输出缓冲区
 * @param path_size 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int get_log_file_path(char* path, size_t path_size);

/**
 * 获取锁文件完整路径
 *
 * @param path 输出缓冲区
 * @param path_size 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
int get_lock_file_path(char* path, size_t path_size);

#endif //AUDIOCTL_CONSTANTS_H
