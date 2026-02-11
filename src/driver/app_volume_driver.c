//
// Driver-side per-app volume control
// Created by AhogeK on 02/05/26.
//
// Uses local volume table (IPC will be implemented via Unix Domain Socket)

#include "driver/app_volume_driver.h"
#include <os/lock.h>
#include <stdatomic.h>

// 客户端条目结构
typedef struct
{
    UInt32 clientID;
    pid_t pid;
    bool active;
} ClientEntry;

#define MAX_CLIENTS 64U

static ClientEntry g_clients[MAX_CLIENTS];
static os_unfair_lock g_clientLock = OS_UNFAIR_LOCK_INIT;
static atomic_int g_clientCount = 0;

static bool g_initialized = false;

// 全局音量表
static AppVolumeTable g_volumeTable = {0};
static os_unfair_lock g_tableLock = OS_UNFAIR_LOCK_INIT;

#pragma mark - Initialization and Cleanup

void app_volume_driver_init(void)
{
    if (g_initialized)
    {
        return;
    }

    os_unfair_lock_lock(&g_clientLock);
    memset(g_clients, 0, sizeof(g_clients));
    atomic_store(&g_clientCount, 0);
    os_unfair_lock_unlock(&g_clientLock);

    os_unfair_lock_lock(&g_tableLock);
    memset(&g_volumeTable, 0, sizeof(g_volumeTable));
    os_unfair_lock_unlock(&g_tableLock);

    g_initialized = true;
}

void app_volume_driver_cleanup(void)
{
    if (!g_initialized)
    {
        return;
    }

    os_unfair_lock_lock(&g_clientLock);
    memset(g_clients, 0, sizeof(g_clients));
    atomic_store(&g_clientCount, 0);
    os_unfair_lock_unlock(&g_clientLock);

    g_initialized = false;
}

#pragma mark - Client Management

OSStatus app_volume_driver_add_client(UInt32 clientID, pid_t pid, const char* bundleId, const char* name)
{
    (void)bundleId;
    (void)name;

    os_unfair_lock_lock(&g_clientLock);

    // 检查是否已存在
    for (UInt32 i = 0; i < MAX_CLIENTS; i++)
    {
        if (g_clients[i].active && g_clients[i].clientID == clientID)
        {
            g_clients[i].pid = pid;
            os_unfair_lock_unlock(&g_clientLock);
            return noErr;
        }
    }

    // 查找空槽
    for (UInt32 i = 0; i < MAX_CLIENTS; i++)
    {
        if (!g_clients[i].active)
        {
            g_clients[i].clientID = clientID;
            g_clients[i].pid = pid;
            g_clients[i].active = true;
            atomic_fetch_add(&g_clientCount, 1);
            os_unfair_lock_unlock(&g_clientLock);
            return noErr;
        }
    }

    os_unfair_lock_unlock(&g_clientLock);
    return kAudioHardwareBadDeviceError; // Client list full
}

OSStatus app_volume_driver_remove_client(UInt32 clientID)
{
    os_unfair_lock_lock(&g_clientLock);

    for (UInt32 i = 0; i < MAX_CLIENTS; i++)
    {
        if (g_clients[i].active && g_clients[i].clientID == clientID)
        {
            g_clients[i].active = false;
            g_clients[i].clientID = 0;
            g_clients[i].pid = 0;
            atomic_fetch_sub(&g_clientCount, 1);
            os_unfair_lock_unlock(&g_clientLock);
            return noErr;
        }
    }

    os_unfair_lock_unlock(&g_clientLock);
    return kAudioHardwareBadDeviceError; // Client not found
}

pid_t app_volume_driver_get_pid(UInt32 clientID)
{
    pid_t result = -1;

    // 尝试获取锁，如果拿不到就算了（避免实时线程阻塞）
    if (os_unfair_lock_trylock(&g_clientLock))
    {
        for (UInt32 i = 0; i < MAX_CLIENTS; i++)
        {
            if (g_clients[i].active && g_clients[i].clientID == clientID)
            {
                result = g_clients[i].pid;
                break;
            }
        }
        os_unfair_lock_unlock(&g_clientLock);
    }

    return result;
}

#pragma mark - 属性访问

OSStatus app_volume_driver_set_table(const AppVolumeTable* table)
{
    if (table == NULL) return kAudioHardwareIllegalOperationError;

    os_unfair_lock_lock(&g_tableLock);
    memcpy(&g_volumeTable, table, sizeof(AppVolumeTable));
    os_unfair_lock_unlock(&g_tableLock);

    return noErr;
}

OSStatus app_volume_driver_get_table(AppVolumeTable* table)
{
    if (table == NULL) return kAudioHardwareIllegalOperationError;

    os_unfair_lock_lock(&g_tableLock);
    memcpy(table, &g_volumeTable, sizeof(AppVolumeTable));
    os_unfair_lock_unlock(&g_tableLock);

    return noErr;
}

OSStatus app_volume_driver_get_client_pids(pid_t* outPids, UInt32 maxCount, UInt32* outActualCount)
{
    if (outPids == NULL || outActualCount == NULL) return kAudioHardwareIllegalOperationError;

    os_unfair_lock_lock(&g_clientLock);

    UInt32 count = 0;
    for (UInt32 i = 0; i < MAX_CLIENTS; i++)
    {
        if (count >= maxCount)
        {
            break;
        }

        if (g_clients[i].active)
        {
            outPids[count++] = g_clients[i].pid;
        }
    }

    *outActualCount = count;

    os_unfair_lock_unlock(&g_clientLock);
    return noErr;
}

#pragma mark - 音量应用

Float32 app_volume_driver_get_volume(UInt32 clientID, bool* outIsMuted)
{
    Float32 volume = 1.0f;
    bool isMuted = false;

    // 1. 获取 PID (TryLock)
    pid_t pid = app_volume_driver_get_pid(clientID);

    // 2. 如果找到了 PID，查表 (TryLock)
    if (pid > 0 && os_unfair_lock_trylock(&g_tableLock))
    {
        for (UInt32 i = 0; i < g_volumeTable.count; i++)
        {
            if (g_volumeTable.entries[i].pid == pid)
            {
                volume = g_volumeTable.entries[i].volume;
                isMuted = (g_volumeTable.entries[i].isMuted != 0);
                break;
            }
        }
        os_unfair_lock_unlock(&g_tableLock);
    }

    if (outIsMuted) *outIsMuted = isMuted;
    return volume;
}

void app_volume_driver_apply_volume(UInt32 clientID, void* buffer, UInt32 frameCount, UInt32 channels)
{
    if (buffer == NULL || frameCount == 0) return;

    bool isMuted = false;
    Float32 volume = app_volume_driver_get_volume(clientID, &isMuted);

    // 静音处理
    if (isMuted)
    {
        memset(buffer, 0, frameCount * channels * sizeof(Float32));
        return;
    }

    // 音量为 1.0 时直接返回（零拷贝）
    if (volume >= 0.999f) return;

    // 应用音量
    Float32* samples = (Float32*)buffer;
    UInt32 totalSamples = frameCount * channels;

    // 简单标量乘法
    for (UInt32 i = 0; i < totalSamples; i++)
    {
        samples[i] *= volume;
    }
}
