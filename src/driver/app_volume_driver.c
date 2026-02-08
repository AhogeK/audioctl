//
// 驱动端应用音量控制 - 极简安全版（无共享内存）
// Created by AhogeK on 02/05/26.
//
// 注意：此为简化版本，仅保留基础框架，确保驱动稳定运行
// 高级功能（CLI 控制音量）后续通过更安全的方式实现

#include "driver/app_volume_driver.h"
#include <pthread.h>
#include <stdatomic.h>

// 简化的客户端条目
typedef struct
{
    UInt32 clientID;
    pid_t pid;
    bool active;
} SimpleClientEntry;

#define MAX_SIMPLE_CLIENTS 64

static SimpleClientEntry g_clients[MAX_SIMPLE_CLIENTS];
static atomic_int g_clientCount = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized = false;

// 默认音量表（简化：所有客户端默认音量 1.0，即无调节）
// 后续可以通过 HAL 属性接口暴露控制点
static Float32 g_defaultVolume = 1.0f;

#pragma mark - 初始化和清理

void app_volume_driver_init(void)
{
    if (g_initialized)
    {
        return;
    }

    memset(g_clients, 0, sizeof(g_clients));
    atomic_store(&g_clientCount, 0);
    g_initialized = true;
}

void app_volume_driver_cleanup(void)
{
    if (!g_initialized)
    {
        return;
    }

    pthread_mutex_lock(&g_mutex);
    memset(g_clients, 0, sizeof(g_clients));
    atomic_store(&g_clientCount, 0);
    pthread_mutex_unlock(&g_mutex);

    g_initialized = false;
}

#pragma mark - 客户端管理

OSStatus app_volume_driver_add_client(UInt32 clientID, pid_t pid, const char* bundleId, const char* name)
{
    (void)bundleId; // 暂时未使用
    (void)name; // 暂时未使用

    pthread_mutex_lock(&g_mutex);

    // 检查是否已存在
    for (int i = 0; i < MAX_SIMPLE_CLIENTS; i++)
    {
        if (g_clients[i].active && g_clients[i].clientID == clientID)
        {
            g_clients[i].pid = pid;
            pthread_mutex_unlock(&g_mutex);
            return noErr;
        }
    }

    // 查找空槽
    for (int i = 0; i < MAX_SIMPLE_CLIENTS; i++)
    {
        if (!g_clients[i].active)
        {
            g_clients[i].clientID = clientID;
            g_clients[i].pid = pid;
            g_clients[i].active = true;
            atomic_fetch_add(&g_clientCount, 1);
            pthread_mutex_unlock(&g_mutex);
            return noErr;
        }
    }

    pthread_mutex_unlock(&g_mutex);
    return kAudioHardwareBadDeviceError;  // 客户端列表已满
}

OSStatus app_volume_driver_remove_client(UInt32 clientID)
{
    pthread_mutex_lock(&g_mutex);

    for (int i = 0; i < MAX_SIMPLE_CLIENTS; i++)
    {
        if (g_clients[i].active && g_clients[i].clientID == clientID)
        {
            g_clients[i].active = false;
            g_clients[i].clientID = 0;
            g_clients[i].pid = 0;
            atomic_fetch_sub(&g_clientCount, 1);
            pthread_mutex_unlock(&g_mutex);
            return noErr;
        }
    }

    pthread_mutex_unlock(&g_mutex);
    return kAudioHardwareBadDeviceError;  // 客户端未找到
}

pid_t app_volume_driver_get_pid(UInt32 clientID)
{
    pid_t result = -1;

    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < MAX_SIMPLE_CLIENTS; i++)
    {
        if (g_clients[i].active && g_clients[i].clientID == clientID)
        {
            result = g_clients[i].pid;
            break;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    
    return result;
}

#pragma mark - 音量应用

Float32 app_volume_driver_get_volume(UInt32 clientID, bool* outIsMuted)
{
    (void)clientID; // 简化版本：不根据客户端区分音量

    if (outIsMuted != NULL)
    {
        *outIsMuted = false;
    }

    // 简化版本：返回默认音量 1.0（即不调节）
    // 后续可以通过自定义 HAL 属性来控制
    return g_defaultVolume;
}

void app_volume_driver_apply_volume(UInt32 clientID, void* buffer, UInt32 frameCount, UInt32 channels)
{
    (void)clientID;

    if (buffer == NULL || frameCount == 0)
    {
        return;
    }

    Float32 volume = g_defaultVolume;

    // 如果音量是100%，不需要处理
    if (volume >= 0.999f)
    {
        return;
    }

    // 应用音量到音频缓冲区
    Float32* samples = (Float32*)buffer;
    UInt32 totalSamples = frameCount * channels;

    for (UInt32 i = 0; i < totalSamples; i++) {
        samples[i] *= volume;
    }
}
