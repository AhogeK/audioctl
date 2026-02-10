//
// Driver-side per-app volume control (minimal, no shared memory)
// Created by AhogeK on 02/05/26.
//
// This is a simplified version keeping only the basic framework for driver stability.
// Advanced features (CLI volume control) will be implemented through safer mechanisms later.

#include "driver/app_volume_driver.h"
#include <pthread.h>
#include <stdatomic.h>

// Simplified client entry structure
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

// Default volume table (simplified: all clients default to 1.0, i.e., no attenuation)
// Future: expose control through HAL property interface
static Float32 g_defaultVolume = 1.0f;

#pragma mark - Initialization and Cleanup

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

#pragma mark - Client Management

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
    return kAudioHardwareBadDeviceError; // Client list full
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
    return kAudioHardwareBadDeviceError; // Client not found
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
    (void)clientID; // Simplified: no per-client volume differentiation

    if (outIsMuted != NULL)
    {
        *outIsMuted = false;
    }

    // Simplified: return default volume 1.0 (no attenuation)
    // Future: control via custom HAL properties
    return g_defaultVolume;
}

// [实时音频路径 - 必须无锁]
// 此函数在 AudioServerPlugIn 的 IOProc 回调中被调用
// 时间约束：每 2-10ms 必须完成，严禁：
//   - 锁操作 (pthread_mutex, atomic 操作是安全的)
//   - 内存分配
//   - 系统调用
//
// 当前实现：
//   - 只读取简单的标量变量 g_defaultVolume
//   - 无需锁保护（标量读取是原子的）
//   - 时间复杂度 O(n)，n=frameCount*channels，实际处理极快
void app_volume_driver_apply_volume(UInt32 clientID, void* buffer, UInt32 frameCount, UInt32 channels)
{
    (void)clientID;

    if (buffer == NULL || frameCount == 0)
    {
        return;
    }

    // 标量读取无需锁（32-bit Float32 在 x86_64/ARM64 上自然对齐，读取是原子的）
    Float32 volume = g_defaultVolume;

    // 如果音量是100%，不需要处理（零成本路径）
    if (volume >= 0.999f)
    {
        return;
    }

    // 应用音量到音频缓冲区
    Float32* samples = (Float32*)buffer;
    UInt32 totalSamples = frameCount * channels;

    // 简单标量乘法，SIMD 友好的循环
    for (UInt32 i = 0; i < totalSamples; i++)
    {
        samples[i] *= volume;
    }
}
