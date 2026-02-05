//
// 应用音量控制模块实现
// Created by AhogeK on 02/05/26.
//

#include "app_volume_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <pthread.h>
#include <unistd.h>
#include <CoreServices/CoreServices.h>

// 共享内存键
#define APP_VOLUME_SHM_KEY 0x564F4C00  // "VOL" + 0x00

// 全局音量列表
static AppVolumeList g_volumeList = {0};
static bool g_initialized = false;

// 共享内存
static int g_shmId = -1;
static AppVolumeList* g_shmVolumeList = NULL;

#pragma mark - 初始化和清理

OSStatus app_volume_control_init(void)
{
    if (g_initialized)
    {
        return noErr;
    }

    memset(&g_volumeList, 0, sizeof(g_volumeList));
    pthread_mutex_init(&g_volumeList.mutex, NULL);
    g_volumeList.count = 0;

    g_initialized = true;

    // 初始化共享内存
    OSStatus status = app_volume_shm_init();
    if (status != noErr)
    {
        printf("警告: 无法初始化共享内存: %d\n", status);
    }

    return noErr;
}

void app_volume_control_cleanup(void)
{
    if (!g_initialized)
    {
        return;
    }

    app_volume_shm_cleanup();

    pthread_mutex_destroy(&g_volumeList.mutex);
    memset(&g_volumeList, 0, sizeof(g_volumeList));

    g_initialized = false;
}

#pragma mark - 内部辅助函数

static AppVolumeInfo* find_entry_unlocked(pid_t pid)
{
    for (UInt32 i = 0; i < g_volumeList.count; i++)
    {
        if (g_volumeList.entries[i].pid == pid)
        {
            return &g_volumeList.entries[i];
        }
    }
    return NULL;
}

static AppVolumeInfo* find_entry_by_name_unlocked(const char* name)
{
    for (UInt32 i = 0; i < g_volumeList.count; i++)
    {
        if (strcasecmp(g_volumeList.entries[i].name, name) == 0)
        {
            return &g_volumeList.entries[i];
        }
    }
    return NULL;
}

static AppVolumeInfo* find_or_create_entry(pid_t pid)
{
    AppVolumeInfo* entry = find_entry_unlocked(pid);
    if (entry != NULL)
    {
        return entry;
    }

    // 创建新条目
    if (g_volumeList.count >= MAX_APP_VOLUME_ENTRIES)
    {
        return NULL; // 列表已满
    }

    entry = &g_volumeList.entries[g_volumeList.count++];
    memset(entry, 0, sizeof(AppVolumeInfo));
    entry->pid = pid;
    entry->volume = 1.0f; // 默认音量100%
    entry->isMuted = false;
    entry->isActive = true;

    return entry;
}

#pragma mark - 音量控制

OSStatus app_volume_set(pid_t pid, Float32 volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    pthread_mutex_lock(&g_volumeList.mutex);

    AppVolumeInfo* entry = find_entry_unlocked(pid);
    if (entry == NULL)
    {
        pthread_mutex_unlock(&g_volumeList.mutex);
        return -1; // 应用未找到
    }

    entry->volume = volume;

    pthread_mutex_unlock(&g_volumeList.mutex);

    // 同步到共享内存
    app_volume_sync_to_shm();

    return noErr;
}

OSStatus app_volume_get(pid_t pid, Float32* outVolume)
{
    if (outVolume == NULL)
    {
        return paramErr;
    }

    pthread_mutex_lock(&g_volumeList.mutex);

    AppVolumeInfo* entry = find_entry_unlocked(pid);
    if (entry == NULL)
    {
        pthread_mutex_unlock(&g_volumeList.mutex);
        return -1; // 应用未找到
    }

    *outVolume = entry->volume;

    pthread_mutex_unlock(&g_volumeList.mutex);

    return noErr;
}

OSStatus app_volume_set_mute(pid_t pid, bool mute)
{
    pthread_mutex_lock(&g_volumeList.mutex);

    AppVolumeInfo* entry = find_entry_unlocked(pid);
    if (entry == NULL)
    {
        pthread_mutex_unlock(&g_volumeList.mutex);
        return -1; // 应用未找到
    }

    entry->isMuted = mute;

    pthread_mutex_unlock(&g_volumeList.mutex);

    // 同步到共享内存
    app_volume_sync_to_shm();

    return noErr;
}

OSStatus app_volume_get_mute(pid_t pid, bool* outMute)
{
    if (outMute == NULL)
    {
        return paramErr;
    }

    pthread_mutex_lock(&g_volumeList.mutex);

    AppVolumeInfo* entry = find_entry_unlocked(pid);
    if (entry == NULL)
    {
        pthread_mutex_unlock(&g_volumeList.mutex);
        return -1; // 应用未找到
    }

    *outMute = entry->isMuted;

    pthread_mutex_unlock(&g_volumeList.mutex);

    return noErr;
}

#pragma mark - 应用管理

OSStatus app_volume_register(pid_t pid, const char* bundleId, const char* name)
{
    pthread_mutex_lock(&g_volumeList.mutex);

    AppVolumeInfo* entry = find_or_create_entry(pid);
    if (entry == NULL)
    {
        pthread_mutex_unlock(&g_volumeList.mutex);
        return -1; // 无法创建条目
    }

    if (bundleId != NULL)
    {
        strncpy(entry->bundleId, bundleId, sizeof(entry->bundleId) - 1);
        entry->bundleId[sizeof(entry->bundleId) - 1] = '\0';
    }

    if (name != NULL)
    {
        strncpy(entry->name, name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';
    }

    entry->isActive = true;

    pthread_mutex_unlock(&g_volumeList.mutex);

    // 同步到共享内存
    app_volume_sync_to_shm();

    return noErr;
}

OSStatus app_volume_unregister(pid_t pid)
{
    pthread_mutex_lock(&g_volumeList.mutex);

    for (UInt32 i = 0; i < g_volumeList.count; i++)
    {
        if (g_volumeList.entries[i].pid == pid)
        {
            // 移动后续条目
            for (UInt32 j = i; j < g_volumeList.count - 1; j++)
            {
                g_volumeList.entries[j] = g_volumeList.entries[j + 1];
            }
            g_volumeList.count--;
            memset(&g_volumeList.entries[g_volumeList.count], 0, sizeof(AppVolumeInfo));

            pthread_mutex_unlock(&g_volumeList.mutex);

            // 同步到共享内存
            app_volume_sync_to_shm();

            return noErr;
        }
    }

    pthread_mutex_unlock(&g_volumeList.mutex);
    return -1; // 应用未找到
}

OSStatus app_volume_set_active(pid_t pid, bool active)
{
    pthread_mutex_lock(&g_volumeList.mutex);

    AppVolumeInfo* entry = find_entry_unlocked(pid);
    if (entry == NULL)
    {
        pthread_mutex_unlock(&g_volumeList.mutex);
        return -1; // 应用未找到
    }

    entry->isActive = active;

    pthread_mutex_unlock(&g_volumeList.mutex);

    // 同步到共享内存
    app_volume_sync_to_shm();

    return noErr;
}

#pragma mark - 查询

OSStatus app_volume_get_all(AppVolumeInfo** outApps, UInt32* outCount)
{
    if (outApps == NULL || outCount == NULL)
    {
        return paramErr;
    }

    pthread_mutex_lock(&g_volumeList.mutex);

    if (g_volumeList.count == 0)
    {
        *outApps = NULL;
        *outCount = 0;
        pthread_mutex_unlock(&g_volumeList.mutex);
        return noErr;
    }

    *outApps = (AppVolumeInfo*)malloc(g_volumeList.count * sizeof(AppVolumeInfo));
    if (*outApps == NULL)
    {
        pthread_mutex_unlock(&g_volumeList.mutex);
        return memFullErr;
    }

    memcpy(*outApps, g_volumeList.entries, g_volumeList.count * sizeof(AppVolumeInfo));
    *outCount = g_volumeList.count;

    pthread_mutex_unlock(&g_volumeList.mutex);

    return noErr;
}

void app_volume_free_list(AppVolumeInfo* apps)
{
    if (apps != NULL)
    {
        free(apps);
    }
}

AppVolumeInfo* app_volume_find(pid_t pid)
{
    pthread_mutex_lock(&g_volumeList.mutex);
    AppVolumeInfo* entry = find_entry_unlocked(pid);
    pthread_mutex_unlock(&g_volumeList.mutex);
    return entry;
}

UInt32 app_volume_get_active_count(void)
{
    pthread_mutex_lock(&g_volumeList.mutex);

    UInt32 count = 0;
    for (UInt32 i = 0; i < g_volumeList.count; i++)
    {
        if (g_volumeList.entries[i].isActive)
        {
            count++;
        }
    }

    pthread_mutex_unlock(&g_volumeList.mutex);
    return count;
}

#pragma mark - 共享内存（用于驱动通信）

OSStatus app_volume_shm_init(void)
{
    // 创建共享内存段
    g_shmId = shmget(APP_VOLUME_SHM_KEY, sizeof(AppVolumeList), IPC_CREAT | 0666);
    if (g_shmId < 0)
    {
        perror("shmget");
        return -1;
    }

    // 附加共享内存
    g_shmVolumeList = (AppVolumeList*)shmat(g_shmId, NULL, 0);
    if (g_shmVolumeList == (void*)-1)
    {
        perror("shmat");
        g_shmVolumeList = NULL;
        return -1;
    }

    // 初始化共享内存
    memset(g_shmVolumeList, 0, sizeof(AppVolumeList));
    pthread_mutex_init(&g_shmVolumeList->mutex, NULL);

    return noErr;
}

void app_volume_shm_cleanup(void)
{
    if (g_shmVolumeList != NULL)
    {
        shmdt(g_shmVolumeList);
        g_shmVolumeList = NULL;
    }

    if (g_shmId >= 0)
    {
        shmctl(g_shmId, IPC_RMID, NULL);
        g_shmId = -1;
    }
}

OSStatus app_volume_sync_to_shm(void)
{
    if (g_shmVolumeList == NULL)
    {
        return -1;
    }

    pthread_mutex_lock(&g_volumeList.mutex);
    pthread_mutex_lock(&g_shmVolumeList->mutex);

    memcpy(g_shmVolumeList, &g_volumeList, sizeof(AppVolumeList));

    pthread_mutex_unlock(&g_shmVolumeList->mutex);
    pthread_mutex_unlock(&g_volumeList.mutex);

    return noErr;
}

OSStatus app_volume_sync_from_shm(void)
{
    if (g_shmVolumeList == NULL)
    {
        return -1;
    }

    pthread_mutex_lock(&g_shmVolumeList->mutex);
    pthread_mutex_lock(&g_volumeList.mutex);

    memcpy(&g_volumeList, g_shmVolumeList, sizeof(AppVolumeList));

    pthread_mutex_unlock(&g_volumeList.mutex);
    pthread_mutex_unlock(&g_shmVolumeList->mutex);

    return noErr;
}

#pragma mark - 命令行接口

void app_volume_cli_list(void)
{
    AppVolumeInfo* apps = NULL;
    UInt32 count = 0;

    OSStatus status = app_volume_get_all(&apps, &count);
    if (status != noErr)
    {
        printf("获取应用音量列表失败\n");
        return;
    }

    if (count == 0)
    {
        printf("没有活动的音频应用\n");
        return;
    }

    printf("发现 %d 个音频应用:\n\n", count);
    printf("%-8s %-20s %-30s %-10s %-8s\n", "PID", "名称", "Bundle ID", "音量", "静音");
    printf("%-8s %-20s %-30s %-10s %-8s\n", "--------", "--------------------", "------------------------------",
           "----------", "--------");

    for (UInt32 i = 0; i < count; i++)
    {
        AppVolumeInfo* app = &apps[i];
        printf("%-8d %-20s %-30s %-9.0f%% %s\n",
               app->pid,
               app->name,
               app->bundleId[0] ? app->bundleId : "N/A",
               app->volume * 100.0f,
               app->isMuted ? "是" : "否");
    }

    app_volume_free_list(apps);
}

int app_volume_cli_set(const char* appNameOrPid, Float32 volume)
{
    if (volume < 0.0f || volume > 100.0f)
    {
        printf("错误: 音量必须在 0-100 之间\n");
        return 1;
    }

    // 尝试解析为PID
    char* endptr;
    pid_t pid = (pid_t)strtol(appNameOrPid, &endptr, 10);

    pthread_mutex_lock(&g_volumeList.mutex);

    AppVolumeInfo* entry = NULL;
    if (*endptr == '\0')
    {
        // 成功解析为数字PID
        entry = find_entry_unlocked(pid);
    }
    else
    {
        // 按名称查找
        entry = find_entry_by_name_unlocked(appNameOrPid);
    }

    if (entry == NULL)
    {
        pthread_mutex_unlock(&g_volumeList.mutex);
        printf("错误: 找不到应用 '%s'\n", appNameOrPid);
        return 1;
    }

    pid_t targetPid = entry->pid;
    char appName[256];
    strncpy(appName, entry->name, sizeof(appName) - 1);
    appName[sizeof(appName) - 1] = '\0';

    pthread_mutex_unlock(&g_volumeList.mutex);

    // 设置音量
    OSStatus status = app_volume_set(targetPid, volume / 100.0f);
    if (status != noErr)
    {
        printf("错误: 无法设置应用 '%s' 的音量\n", appName);
        return 1;
    }

    printf("已将应用 '%s' (PID: %d) 的音量设置为 %.0f%%\n", appName, targetPid, volume);
    return 0;
}

int app_volume_cli_mute(const char* appNameOrPid, bool mute)
{
    // 尝试解析为PID
    char* endptr;
    pid_t pid = (pid_t)strtol(appNameOrPid, &endptr, 10);

    pthread_mutex_lock(&g_volumeList.mutex);

    AppVolumeInfo* entry = NULL;
    if (*endptr == '\0')
    {
        entry = find_entry_unlocked(pid);
    }
    else
    {
        entry = find_entry_by_name_unlocked(appNameOrPid);
    }

    if (entry == NULL)
    {
        pthread_mutex_unlock(&g_volumeList.mutex);
        printf("错误: 找不到应用 '%s'\n", appNameOrPid);
        return 1;
    }

    pid_t targetPid = entry->pid;
    char appName[256];
    strncpy(appName, entry->name, sizeof(appName) - 1);
    appName[sizeof(appName) - 1] = '\0';

    pthread_mutex_unlock(&g_volumeList.mutex);

    OSStatus status = app_volume_set_mute(targetPid, mute);
    if (status != noErr)
    {
        printf("错误: 无法%s应用 '%s'\n", mute ? "静音" : "取消静音", appName);
        return 1;
    }

    printf("已%s应用 '%s' (PID: %d)\n", mute ? "静音" : "取消静音", appName, targetPid);
    return 0;
}
