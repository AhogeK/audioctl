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
 * 获取用户主目录
 */
static const char* get_home_directory(void)
{
    const char* home = getenv("HOME");
    if (!home)
    {
        // 尝试通过 getpwuid 获取
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
    }
    return home;
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
