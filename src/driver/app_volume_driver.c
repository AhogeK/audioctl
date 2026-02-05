//
// 驱动端应用音量控制实现
// Created by AhogeK on 02/05/26.
//

#include "driver/app_volume_driver.h"
#include "app_volume_control.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/shm.h>

// 共享内存键（与app_volume_control.c中保持一致）
#define APP_VOLUME_SHM_KEY 0x564F4C00  // "VOL" + 0x00

// 全局管理器
static ClientVolumeManager g_clientManager = {0};
static bool g_initialized = false;

// 共享内存
static int g_shmId = -1;
static AppVolumeList* g_shmVolumeList = NULL;

#pragma mark - 初始化和清理

OSStatus app_volume_driver_init(void)
{
    if (g_initialized)
    {
        return noErr;
    }

    memset(&g_clientManager, 0, sizeof(g_clientManager));
    pthread_mutex_init(&g_clientManager.mutex, NULL);
    g_clientManager.count = 0;

    // 连接共享内存
    g_shmId = shmget(APP_VOLUME_SHM_KEY, sizeof(AppVolumeList), 0666);
    if (g_shmId >= 0)
    {
        g_shmVolumeList = (AppVolumeList*)shmat(g_shmId, NULL, 0);
        if (g_shmVolumeList == (void*)-1)
        {
            g_shmVolumeList = NULL;
        }
    }

    g_initialized = true;
    return noErr;
}

void app_volume_driver_cleanup(void)
{
    if (!g_initialized)
    {
        return;
    }

    if (g_shmVolumeList != NULL)
    {
        shmdt(g_shmVolumeList);
        g_shmVolumeList = NULL;
    }

    pthread_mutex_destroy(&g_clientManager.mutex);
    memset(&g_clientManager, 0, sizeof(g_clientManager));

    g_initialized = false;
}

#pragma mark - 内部辅助函数

static ClientVolumeEntry* find_client_by_id(UInt32 clientID)
{
    for (UInt32 i = 0; i < g_clientManager.count; i++)
    {
        if (g_clientManager.entries[i].clientID == clientID)
        {
            return &g_clientManager.entries[i];
        }
    }
    return NULL;
}

static ClientVolumeEntry* find_client_by_pid(pid_t pid)
{
    for (UInt32 i = 0; i < g_clientManager.count; i++)
    {
        if (g_clientManager.entries[i].pid == pid)
        {
            return &g_clientManager.entries[i];
        }
    }
    return NULL;
}

#pragma mark - 客户端管理

OSStatus app_volume_driver_add_client(UInt32 clientID, pid_t pid, const char* bundleId, const char* name)
{
    pthread_mutex_lock(&g_clientManager.mutex);

    // 检查是否已存在
    ClientVolumeEntry* entry = find_client_by_id(clientID);
    if (entry != NULL)
    {
        // 更新现有条目
        entry->pid = pid;
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
        pthread_mutex_unlock(&g_clientManager.mutex);
        return noErr;
    }

    // 检查PID是否已存在（可能ClientID变了）
    entry = find_client_by_pid(pid);
    if (entry != NULL)
    {
        entry->clientID = clientID;
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
        pthread_mutex_unlock(&g_clientManager.mutex);
        return noErr;
    }

    // 创建新条目
    if (g_clientManager.count >= MAX_CLIENT_ENTRIES)
    {
        pthread_mutex_unlock(&g_clientManager.mutex);
        return -1; // 列表已满
    }

    entry = &g_clientManager.entries[g_clientManager.count++];
    memset(entry, 0, sizeof(ClientVolumeEntry));
    entry->clientID = clientID;
    entry->pid = pid;
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

    pthread_mutex_unlock(&g_clientManager.mutex);

    return noErr;
}

OSStatus app_volume_driver_remove_client(UInt32 clientID)
{
    pthread_mutex_lock(&g_clientManager.mutex);

    for (UInt32 i = 0; i < g_clientManager.count; i++)
    {
        if (g_clientManager.entries[i].clientID == clientID)
        {
            // 移动后续条目
            for (UInt32 j = i; j < g_clientManager.count - 1; j++)
            {
                g_clientManager.entries[j] = g_clientManager.entries[j + 1];
            }
            g_clientManager.count--;
            memset(&g_clientManager.entries[g_clientManager.count], 0, sizeof(ClientVolumeEntry));

            pthread_mutex_unlock(&g_clientManager.mutex);
            return noErr;
        }
    }

    pthread_mutex_unlock(&g_clientManager.mutex);
    return -1; // 客户端未找到
}

pid_t app_volume_driver_get_pid(UInt32 clientID)
{
    pthread_mutex_lock(&g_clientManager.mutex);

    ClientVolumeEntry* entry = find_client_by_id(clientID);
    pid_t pid = entry != NULL ? entry->pid : -1;

    pthread_mutex_unlock(&g_clientManager.mutex);

    return pid;
}

#pragma mark - 音量应用

Float32 app_volume_driver_get_volume(UInt32 clientID, bool* outIsMuted)
{
    Float32 volume = 1.0f;
    bool isMuted = false;

    // 获取PID
    pid_t pid = app_volume_driver_get_pid(clientID);
    if (pid < 0)
    {
        // 未找到客户端，使用默认音量
        if (outIsMuted != NULL)
        {
            *outIsMuted = false;
        }
        return 1.0f;
    }

    // 从共享内存读取音量
    if (g_shmVolumeList != NULL)
    {
        pthread_mutex_lock(&g_shmVolumeList->mutex);

        for (UInt32 i = 0; i < g_shmVolumeList->count; i++)
        {
            if (g_shmVolumeList->entries[i].pid == pid)
            {
                volume = g_shmVolumeList->entries[i].volume;
                isMuted = g_shmVolumeList->entries[i].isMuted;
                break;
            }
        }

        pthread_mutex_unlock(&g_shmVolumeList->mutex);
    }

    if (outIsMuted != NULL)
    {
        *outIsMuted = isMuted;
    }

    return isMuted ? 0.0f : volume;
}

void app_volume_driver_apply_volume(UInt32 clientID, void* buffer, UInt32 frameCount, UInt32 channels)
{
    if (buffer == NULL || frameCount == 0)
    {
        return;
    }

    // 获取该客户端的音量
    Float32 volume = app_volume_driver_get_volume(clientID, NULL);

    // 如果音量是100%，不需要处理
    if (volume >= 1.0f)
    {
        return;
    }

    // 应用音量到音频缓冲区 (32位浮点格式)
    Float32* samples = (Float32*)buffer;
    UInt32 totalSamples = frameCount * channels;

    for (UInt32 i = 0; i < totalSamples; i++)
    {
        samples[i] *= volume;
    }
}
