//
// Driver-side per-app volume control
// Created by AhogeK on 02/05/26.
//
// Uses IPC client with local cache for real-time audio processing

#include "driver/app_volume_driver.h"
#include <os/lock.h>
#include <stdatomic.h>
#include <sys/time.h>
#include "ipc/ipc_client.h"

// 【修复】定义缓存有效期（ms），与 ipc_client.c 保持一致
#define IPC_CACHE_TTL_MS 100

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

// IPC 客户端上下文（用于从服务端获取音量）
static IPCClientContext g_ipcClient = {0};
static bool g_ipcInitialized = false;

// 本地音量表（作为 IPC 的缓存）
static AppVolumeTable g_volumeTable = {0};
static os_unfair_lock g_tableLock = OS_UNFAIR_LOCK_INIT;

// 【关键修复】实时音频路径使用的时间戳函数
static uint64_t
get_timestamp_ms (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return (uint64_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

#pragma mark - Initialization and Cleanup

void
app_volume_driver_init (void)
{
  if (g_initialized)
    {
      return;
    }

  os_unfair_lock_lock (&g_clientLock);
  memset (g_clients, 0, sizeof (g_clients));
  atomic_store (&g_clientCount, 0);
  os_unfair_lock_unlock (&g_clientLock);

  os_unfair_lock_lock (&g_tableLock);
  memset (&g_volumeTable, 0, sizeof (g_volumeTable));
  os_unfair_lock_unlock (&g_tableLock);

  // 初始化 IPC 客户端
  if (!g_ipcInitialized)
    {
      ipc_client_init (&g_ipcClient);
      // 尝试连接 IPC 服务
      ipc_client_connect (&g_ipcClient);
      g_ipcInitialized = true;
    }

  g_initialized = true;
}

void
app_volume_driver_cleanup (void)
{
  if (!g_initialized)
    {
      return;
    }

  os_unfair_lock_lock (&g_clientLock);
  memset (g_clients, 0, sizeof (g_clients));
  atomic_store (&g_clientCount, 0);
  os_unfair_lock_unlock (&g_clientLock);

  // 清理 IPC 客户端
  if (g_ipcInitialized)
    {
      ipc_client_disconnect (&g_ipcClient);
      ipc_client_cleanup (&g_ipcClient);
      g_ipcInitialized = false;
    }

  g_initialized = false;
}

#pragma mark - Client Management

OSStatus
app_volume_driver_add_client (UInt32 clientID, pid_t pid, const char *bundleId,
			      const char *name)
{
  os_unfair_lock_lock (&g_clientLock);

  // 检查是否已存在
  for (UInt32 i = 0; i < MAX_CLIENTS; i++)
    {
      if (g_clients[i].active && g_clients[i].clientID == clientID)
	{
	  g_clients[i].pid = pid;
	  os_unfair_lock_unlock (&g_clientLock);
	  return noErr;
	}
    }

  // 查找空槽
  for (UInt32 i = 0; i < MAX_CLIENTS; i++)
    {
      if (g_clients[i].active)
	{
	  continue;
	}

      g_clients[i].clientID = clientID;
      g_clients[i].pid = pid;
      g_clients[i].active = true;
      atomic_fetch_add (&g_clientCount, 1);
      os_unfair_lock_unlock (&g_clientLock);

      // 通过 IPC 注册到服务端
      if (g_ipcInitialized && ipc_client_is_connected (&g_ipcClient))
	{
	  const char *appName = name;
	  if (appName == NULL)
	    {
	      appName = bundleId ? bundleId : "Unknown";
	    }
	  ipc_client_register_app (&g_ipcClient, pid, appName, 1.0f, false);
	}

      return noErr;
    }

  os_unfair_lock_unlock (&g_clientLock);
  return kAudioHardwareBadDeviceError; // Client list full
}

OSStatus
app_volume_driver_remove_client (UInt32 clientID)
{
  pid_t removedPid = 0;

  os_unfair_lock_lock (&g_clientLock);

  for (UInt32 i = 0; i < MAX_CLIENTS; i++)
    {
      if (g_clients[i].active && g_clients[i].clientID == clientID)
	{
	  removedPid = g_clients[i].pid;
	  g_clients[i].active = false;
	  g_clients[i].clientID = 0;
	  g_clients[i].pid = 0;
	  atomic_fetch_sub (&g_clientCount, 1);
	  os_unfair_lock_unlock (&g_clientLock);

	  // 通过 IPC 从服务端注销
	  if (g_ipcInitialized && removedPid > 0
	      && ipc_client_is_connected (&g_ipcClient))
	    {
	      ipc_client_unregister_app (&g_ipcClient, removedPid);
	    }

	  return noErr;
	}
    }

  os_unfair_lock_unlock (&g_clientLock);
  return kAudioHardwareBadDeviceError; // Client not found
}

pid_t
app_volume_driver_get_pid (UInt32 clientID)
{
  pid_t result = -1;

  // 尝试获取锁，如果拿不到就算了（避免实时线程阻塞）
  if (os_unfair_lock_trylock (&g_clientLock))
    {
      for (UInt32 i = 0; i < MAX_CLIENTS; i++)
	{
	  if (g_clients[i].active && g_clients[i].clientID == clientID)
	    {
	      result = g_clients[i].pid;
	      break;
	    }
	}
      os_unfair_lock_unlock (&g_clientLock);
    }

  return result;
}

#pragma mark - 属性访问

OSStatus
app_volume_driver_set_table (const AppVolumeTable *table)
{
  if (table == NULL)
    return kAudioHardwareIllegalOperationError;

  os_unfair_lock_lock (&g_tableLock);
  memcpy (&g_volumeTable, table, sizeof (AppVolumeTable));
  os_unfair_lock_unlock (&g_tableLock);

  return noErr;
}

OSStatus
app_volume_driver_get_table (AppVolumeTable *table)
{
  if (table == NULL)
    return kAudioHardwareIllegalOperationError;

  os_unfair_lock_lock (&g_tableLock);
  memcpy (table, &g_volumeTable, sizeof (AppVolumeTable));
  os_unfair_lock_unlock (&g_tableLock);

  return noErr;
}

OSStatus
app_volume_driver_get_client_pids (pid_t *outPids, UInt32 maxCount,
				   UInt32 *outActualCount)
{
  if (outPids == NULL || outActualCount == NULL)
    return kAudioHardwareIllegalOperationError;

  os_unfair_lock_lock (&g_clientLock);

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

  os_unfair_lock_unlock (&g_clientLock);
  return noErr;
}

#pragma mark - 音量应用

// 【关键修复】实时音频路径专用：永不阻塞，只使用缓存
// 注意：此函数在实时音频线程（IOProc）中被调用，严禁任何阻塞操作
Float32
app_volume_driver_get_volume (UInt32 clientID, bool *outIsMuted)
{
  Float32 volume = 1.0f; // 默认音量
  bool isMuted = false;

  // 1. 获取 PID (TryLock)
  pid_t pid = app_volume_driver_get_pid (clientID);

  // 2. 【关键修复】实时路径只读取缓存，永不触发同步IPC
  // 缓存过期时直接使用默认值，避免阻塞音频线程
  if (pid > 0 && g_ipcInitialized)
    {
      // 只检查缓存，不触发更新
      uint64_t now = get_timestamp_ms ();
      if (g_ipcClient.cache_valid && g_ipcClient.cached_pid == pid
	  && (now - g_ipcClient.cache_timestamp) < IPC_CACHE_TTL_MS)
	{
	  volume = g_ipcClient.cached_volume;
	  isMuted = g_ipcClient.cached_muted;
	}
      // 缓存过期时直接使用默认值（音量=1.0），不触发IPC
      // 缓存更新由后台线程完成
    }

  if (outIsMuted)
    *outIsMuted = isMuted;
  return volume;
}

void
app_volume_driver_apply_volume (UInt32 clientID, void *buffer,
				UInt32 frameCount, UInt32 channels)
{
  if (buffer == NULL || frameCount == 0)
    return;

  bool isMuted = false;
  Float32 volume = app_volume_driver_get_volume (clientID, &isMuted);

  // 静音处理
  if (isMuted)
    {
      memset (buffer, 0, frameCount * channels * sizeof (Float32));
      return;
    }

  // 音量为 1.0 时直接返回（零拷贝）
  if (volume >= 0.999f)
    return;

  // 应用音量 (Interleaved: L R L R ...)
  Float32 *samples = (Float32 *) buffer;
  UInt32 totalSamples = frameCount * channels;

  // 简单标量乘法
  for (UInt32 i = 0; i < totalSamples; i++)
    {
      samples[i] *= volume;
    }
}

void
app_volume_driver_apply_volume_ni (UInt32 clientID, void *leftBuffer,
				   void *rightBuffer, UInt32 frameCount)
{
  if ((leftBuffer == NULL && rightBuffer == NULL) || frameCount == 0)
    return;

  bool isMuted = false;
  Float32 volume = app_volume_driver_get_volume (clientID, &isMuted);

  // 静音处理
  if (isMuted)
    {
      if (leftBuffer)
	memset (leftBuffer, 0, frameCount * sizeof (Float32));
      if (rightBuffer)
	memset (rightBuffer, 0, frameCount * sizeof (Float32));
      return;
    }

  // 音量为 1.0 时直接返回（零拷贝）
  if (volume >= 0.999f)
    return;

  // 应用音量到左右声道
  Float32 *leftSamples = (Float32 *) leftBuffer;
  Float32 *rightSamples = (Float32 *) rightBuffer;

  // 简单标量乘法（分别处理左右声道）
  if (leftSamples)
    {
      for (UInt32 i = 0; i < frameCount; i++)
	{
	  leftSamples[i] *= volume;
	}
    }
  if (rightSamples)
    {
      for (UInt32 i = 0; i < frameCount; i++)
	{
	  rightSamples[i] *= volume;
	}
    }
}
