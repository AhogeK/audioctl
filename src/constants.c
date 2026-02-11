//
// Created by AhogeK on 02/11/26.
//

#include "constants.h"
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>

/**
 * 获取实际用户主目录（处理 sudo/root 情况）
 * 优先使用 getlogin() 获取登录用户，这适用于 sudo 和 su 场景
 */
static const char* get_home_directory(void)
{
    // 方法 1: 尝试使用 getlogin() 获取登录用户名
    // 这在 sudo/su 到 root 时仍然返回原用户名
    char login_name[256];
    if (getlogin_r(login_name, sizeof(login_name)) == 0 && strlen(login_name) > 0 && strcmp(login_name, "root") != 0)
    {
        struct passwd pwd;
        struct passwd* result = NULL;
        long bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
        if (bufsize == -1) bufsize = 16384;
        char* buffer = malloc(bufsize);
        if (buffer)
        {
            if (getpwnam_r(login_name, &pwd, buffer, bufsize, &result) == 0 && result)
            {
                static char home_path[PATH_MAX];
                strncpy(home_path, pwd.pw_dir, sizeof(home_path) - 1);
                home_path[sizeof(home_path) - 1] = '\0';
                free(buffer);
                return home_path;
            }
            free(buffer);
        }
    }

    // 方法 2: 检查 SUDO_USER 环境变量
    const char* sudo_user = getenv("SUDO_USER");
    if (sudo_user && strlen(sudo_user) > 0 && strcmp(sudo_user, "root") != 0)
    {
        struct passwd pwd;
        struct passwd* result = NULL;
        long bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
        if (bufsize == -1) bufsize = 16384;
        char* buffer = malloc(bufsize);
        if (buffer)
        {
            if (getpwnam_r(sudo_user, &pwd, buffer, bufsize, &result) == 0 && result)
            {
                static char home_path[PATH_MAX];
                strncpy(home_path, pwd.pw_dir, sizeof(home_path) - 1);
                home_path[sizeof(home_path) - 1] = '\0';
                free(buffer);
                return home_path;
            }
            free(buffer);
        }
    }

    // 方法 3: 尝试获取 HOME 环境变量
    const char* home = getenv("HOME");
    if (home && strlen(home) > 0)
    {
        return home;
    }

    // 方法 4: 通过 getpwuid 获取当前用户（可能是 root）
    uid_t uid = getuid();
    struct passwd pwd;
    struct passwd* result = NULL;
    long bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufsize == -1) bufsize = 16384;
    char* buffer = malloc(bufsize);
    if (buffer)
    {
        if (getpwuid_r(uid, &pwd, buffer, bufsize, &result) == 0 && result)
        {
            static char home_path[PATH_MAX];
            strncpy(home_path, pwd.pw_dir, sizeof(home_path) - 1);
            home_path[sizeof(home_path) - 1] = '\0';
            free(buffer);
            return home_path;
        }
        free(buffer);
    }

    return NULL;
}

/**
 * 确保目录存在，如果不存在则创建
 */
static int ensure_directory_exists(const char* path)
{
    if (mkdir(path, DIR_MODE) == 0)
    {
        return 0;
    }

    if (errno == EEXIST)
    {
        struct stat st;
        if (stat(path, &st) == 0)
        {
            if (S_ISDIR(st.st_mode))
            {
                return 0; // 目录已存在
            }
            fprintf(stderr, "错误: %s 存在但不是目录\n", path);
            return -1;
        }
    }

    fprintf(stderr, "错误: 无法创建目录 %s: %s\n", path, strerror(errno));
    return -1;
}

int get_support_directory(char* path, size_t path_size)
{
    const char* home = get_home_directory();
    if (!home)
    {
        fprintf(stderr, "错误: 无法获取用户主目录\n");
        return -1;
    }

    // 构建路径: ~/Library/Application Support/audioctl/
    int written = snprintf(path, path_size, "%s/Library/Application Support/audioctl", home);
    if (written < 0 || (size_t)written >= path_size)
    {
        fprintf(stderr, "错误: 路径缓冲区太小\n");
        return -1;
    }

    // 确保目录存在
    if (ensure_directory_exists(path) != 0)
    {
        return -1;
    }

    return 0;
}

int get_pid_file_path(char* path, size_t path_size)
{
    char support_dir[PATH_MAX];
    if (get_support_directory(support_dir, sizeof(support_dir)) != 0)
    {
        return -1;
    }

    int written = snprintf(path, path_size, "%s/%s", support_dir, PID_FILENAME);
    if (written < 0 || (size_t)written >= path_size)
    {
        fprintf(stderr, "错误: 路径缓冲区太小\n");
        return -1;
    }

    return 0;
}

int get_log_file_path(char* path, size_t path_size)
{
    char support_dir[PATH_MAX];
    if (get_support_directory(support_dir, sizeof(support_dir)) != 0)
    {
        return -1;
    }

    int written = snprintf(path, path_size, "%s/%s", support_dir, LOG_FILENAME);
    if (written < 0 || (size_t)written >= path_size)
    {
        fprintf(stderr, "错误: 路径缓冲区太小\n");
        return -1;
    }

    return 0;
}

int get_lock_file_path(char* path, size_t path_size)
{
    char support_dir[PATH_MAX];
    if (get_support_directory(support_dir, sizeof(support_dir)) != 0)
    {
        return -1;
    }

    int written = snprintf(path, path_size, "%s/%s", support_dir, LOCK_FILENAME);
    if (written < 0 || (size_t)written >= path_size)
    {
        fprintf(stderr, "错误: 路径缓冲区太小\n");
        return -1;
    }

    return 0;
}
