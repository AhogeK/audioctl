#include "app_volume_control.h"
#include <CoreServices/CoreServices.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "audio_apps.h"
#include "ipc/ipc_client.h"
#include "virtual_device_manager.h"

// 全局音量列表
static AppVolumeList g_volumeList = {0};
static bool g_initialized = false;

// IPC 客户端上下文
static IPCClientContext g_ipcClient = {0};
static bool g_ipcInitialized = false;

#pragma mark - IPC 客户端管理

static OSStatus
ensure_ipc_client_connected (void)
{
  if (!g_ipcInitialized)
    {
      if (ipc_client_init (&g_ipcClient) != 0)
	{
	  return -1;
	}
      g_ipcInitialized = true;
    }

  if (!ipc_client_is_connected (&g_ipcClient)
      && ipc_client_connect (&g_ipcClient) != 0)
    {
      // 连接失败，尝试重连
      if (ipc_client_should_reconnect (&g_ipcClient))
	{
	  ipc_client_reconnect (&g_ipcClient);
	}
      return -1;
    }

  return noErr;
}

#pragma mark - 初始化和清理

OSStatus
app_volume_control_init (void)
{
  if (g_initialized)
    {
      return noErr;
    }

  memset (&g_volumeList, 0, sizeof (g_volumeList));
  pthread_mutex_init (&g_volumeList.mutex, NULL);
  g_volumeList.count = 0;

  g_initialized = true;

  return noErr;
}

void
app_volume_control_cleanup (void)
{
  if (!g_initialized)
    {
      return;
    }

  pthread_mutex_destroy (&g_volumeList.mutex);
  memset (&g_volumeList, 0, sizeof (g_volumeList));

  g_initialized = false;
}

#pragma mark - 内部辅助函数

static AppVolumeInfo *
find_entry_unlocked (pid_t pid)
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

static AppVolumeInfo *
find_or_create_entry (pid_t pid)
{
  AppVolumeInfo *entry = find_entry_unlocked (pid);
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
  memset (entry, 0, sizeof (AppVolumeInfo));
  entry->pid = pid;
  entry->volume = 1.0f; // 默认音量100%
  entry->isMuted = false;
  entry->isActive = true;

  return entry;
}

#pragma mark - 驱动通信 (通过 IPC)

OSStatus
app_volume_sync_to_driver (void)
{
  // 现在通过 IPC 客户端自动同步，此函数保留用于兼容性
  // 实际的同步在 set/mute/register 操作中直接完成
  return noErr;
}

#pragma mark - 音量控制

OSStatus
app_volume_set (pid_t pid, Float32 volume)
{
  if (volume < 0.0f)
    volume = 0.0f;
  if (volume > 1.0f)
    volume = 1.0f;

  pthread_mutex_lock (&g_volumeList.mutex);

  AppVolumeInfo *entry = find_entry_unlocked (pid);
  if (entry == NULL)
    {
      pthread_mutex_unlock (&g_volumeList.mutex);
      return -1; // 应用未找到
    }

  entry->volume = volume;

  pthread_mutex_unlock (&g_volumeList.mutex);

  // 同步到 IPC 服务端
  if (ensure_ipc_client_connected () == noErr)
    {
      ipc_client_set_app_volume (&g_ipcClient, pid, volume);
    }

  return noErr;
}

OSStatus
app_volume_get (pid_t pid, Float32 *outVolume)
{
  if (outVolume == NULL)
    {
      return paramErr;
    }

  pthread_mutex_lock (&g_volumeList.mutex);

  const AppVolumeInfo *entry = find_entry_unlocked (pid);
  if (entry == NULL)
    {
      pthread_mutex_unlock (&g_volumeList.mutex);
      return -1; // 应用未找到
    }

  *outVolume = entry->volume;

  pthread_mutex_unlock (&g_volumeList.mutex);

  return noErr;
}

OSStatus
app_volume_set_mute (pid_t pid, bool mute)
{
  pthread_mutex_lock (&g_volumeList.mutex);

  AppVolumeInfo *entry = find_entry_unlocked (pid);
  if (entry == NULL)
    {
      pthread_mutex_unlock (&g_volumeList.mutex);
      return -1; // 应用未找到
    }

  entry->isMuted = mute;

  pthread_mutex_unlock (&g_volumeList.mutex);

  // 同步到 IPC 服务端
  if (ensure_ipc_client_connected () == noErr)
    {
      ipc_client_set_app_mute (&g_ipcClient, pid, mute);
    }

  return noErr;
}

OSStatus
app_volume_get_mute (pid_t pid, bool *outMute)
{
  if (outMute == NULL)
    {
      return paramErr;
    }

  pthread_mutex_lock (&g_volumeList.mutex);

  const AppVolumeInfo *entry = find_entry_unlocked (pid);
  if (entry == NULL)
    {
      pthread_mutex_unlock (&g_volumeList.mutex);
      return -1; // 应用未找到
    }

  *outMute = entry->isMuted;

  pthread_mutex_unlock (&g_volumeList.mutex);

  return noErr;
}

#pragma mark - 应用管理

OSStatus
app_volume_register (pid_t pid, const char *bundleId, const char *name)
{
  pthread_mutex_lock (&g_volumeList.mutex);

  AppVolumeInfo *entry = find_or_create_entry (pid);
  if (entry == NULL)
    {
      pthread_mutex_unlock (&g_volumeList.mutex);
      return -1; // 无法创建条目
    }

  if (bundleId != NULL)
    {
      strncpy (entry->bundleId, bundleId, sizeof (entry->bundleId) - 1);
      entry->bundleId[sizeof (entry->bundleId) - 1] = '\0';
    }

  if (name != NULL)
    {
      strncpy (entry->name, name, sizeof (entry->name) - 1);
      entry->name[sizeof (entry->name) - 1] = '\0';
    }

  entry->isActive = true;

  pthread_mutex_unlock (&g_volumeList.mutex);

  // 同步到 IPC 服务端
  if (ensure_ipc_client_connected () == noErr)
    {
      // 使用应用名称或 bundleId 作为名称
      const char *appName;
      if (bundleId)
	appName = name ? name : bundleId;
      else
	appName = name ? name : "Unknown";
      ipc_client_register_app (&g_ipcClient, pid, appName, entry->volume,
			       entry->isMuted);
    }

  return noErr;
}

OSStatus
app_volume_unregister (pid_t pid)
{
  pthread_mutex_lock (&g_volumeList.mutex);

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
	  memset (&g_volumeList.entries[g_volumeList.count], 0,
		  sizeof (AppVolumeInfo));

	  pthread_mutex_unlock (&g_volumeList.mutex);

	  // 同步到 IPC 服务端
	  if (ensure_ipc_client_connected () == noErr)
	    {
	      ipc_client_unregister_app (&g_ipcClient, pid);
	    }

	  return noErr;
	}
    }

  pthread_mutex_unlock (&g_volumeList.mutex);
  return -1; // 应用未找到
}

OSStatus
app_volume_set_active (pid_t pid, bool active)
{
  pthread_mutex_lock (&g_volumeList.mutex);

  AppVolumeInfo *entry = find_entry_unlocked (pid);
  if (entry == NULL)
    {
      pthread_mutex_unlock (&g_volumeList.mutex);
      return -1; // 应用未找到
    }

  entry->isActive = active;

  pthread_mutex_unlock (&g_volumeList.mutex);

  // 状态变更通过 IPC 同步（如果需要）
  // 当前实现中 isActive 是本地状态，不需要同步到服务端

  return noErr;
}

#pragma mark - 查询

OSStatus
app_volume_get_all (AppVolumeInfo **outApps, UInt32 *outCount)
{
  if (outApps == NULL || outCount == NULL)
    {
      return paramErr;
    }

  pthread_mutex_lock (&g_volumeList.mutex);

  if (g_volumeList.count == 0)
    {
      *outApps = NULL;
      *outCount = 0;
      pthread_mutex_unlock (&g_volumeList.mutex);
      return noErr;
    }

  *outApps
    = (AppVolumeInfo *) malloc (g_volumeList.count * sizeof (AppVolumeInfo));
  if (*outApps == NULL)
    {
      pthread_mutex_unlock (&g_volumeList.mutex);
      return memFullErr;
    }

  memcpy (*outApps, g_volumeList.entries,
	  g_volumeList.count * sizeof (AppVolumeInfo));
  *outCount = g_volumeList.count;

  pthread_mutex_unlock (&g_volumeList.mutex);

  return noErr;
}

void
app_volume_free_list (AppVolumeInfo *apps)
{
  if (apps != NULL)
    {
      free (apps);
    }
}

AppVolumeInfo *
app_volume_find (pid_t pid)
{
  pthread_mutex_lock (&g_volumeList.mutex);
  AppVolumeInfo *entry = find_entry_unlocked (pid);
  pthread_mutex_unlock (&g_volumeList.mutex);
  return entry;
}

UInt32
app_volume_get_active_count (void)
{
  pthread_mutex_lock (&g_volumeList.mutex);

  UInt32 count = 0;
  for (UInt32 i = 0; i < g_volumeList.count; i++)
    {
      if (g_volumeList.entries[i].isActive)
	{
	  count++;
	}
    }

  pthread_mutex_unlock (&g_volumeList.mutex);
  return count;
}

#pragma mark - 命令行接口

void
app_volume_cli_list (void)
{
  // 1. 获取虚拟设备 ID
  VirtualDeviceInfo vInfo;
  if (!virtual_device_get_info (&vInfo))
    {
      printf ("⚠️  虚拟音频设备未找到\n");
      printf ("请运行: audioctl use-virtual 激活虚拟设备\n");
      return;
    }

  if (!vInfo.isActive)
    {
      printf ("⚠️  虚拟音频设备未激活（不是当前默认输出设备）\n");
      printf ("请运行: audioctl use-virtual 切换到虚拟设备\n");
      return;
    }

  // 2. 初始化 IPC 客户端
  IPCClientContext ctx;
  if (ipc_client_init (&ctx) != 0)
    {
      printf ("❌ 初始化 IPC 客户端失败\n");
      return;
    }

  if (ipc_client_connect (&ctx) != 0)
    {
      printf ("⚠️  IPC 服务未运行，请使用: audioctl --start-service 启动服务\n");
      ipc_client_cleanup (&ctx);
      return;
    }

  // 3. 获取应用列表
  IPCAppInfo *apps = NULL;
  uint32_t count = 0;

  // 首先尝试从 IPC 服务获取
  if (ipc_client_list_apps (&ctx, &apps, &count) != 0 || count == 0)
    {
      AudioAppInfo *fApps = NULL;
      UInt32 fCount = 0;
      OSStatus err = getAudioApps (&fApps, &fCount);

      if (err == noErr && fCount > 0)
	{
	  apps = malloc (sizeof (IPCAppInfo) * fCount);
	}

      if (apps != NULL)
	{
	  for (UInt32 i = 0; i < fCount; i++)
	    {
	      apps[i].pid = fApps[i].pid;
	      apps[i].volume = fApps[i].volume;
	      apps[i].muted = false;
	      apps[i].connected_at = 0;
	      strncpy (apps[i].app_name, fApps[i].name,
		       sizeof (apps[i].app_name) - 1);
	      apps[i].app_name[sizeof (apps[i].app_name) - 1] = '\0';
	    }
	  count = fCount;
	}

      if (fApps != NULL)
	{
	  freeAudioApps (fApps);
	}
    }

  // 4. 显示应用列表
  printf ("\n🎵 正在使用虚拟设备的应用 (%d 个):\n", count);
  printf ("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

  if (apps != NULL && count > 0)
    {
      for (uint32_t i = 0; i < count; i++)
	{
	  const char *mute_status = apps[i].muted ? "🔇 静音" : "";
	  printf ("%-25s PID: %-6d  音量: %3.0f%% %s\n", apps[i].app_name,
		  apps[i].pid, apps[i].volume * 100.0f, mute_status);
	}

      printf ("\n💡 使用以下命令控制音量:\n");
      printf ("   audioctl app-volume <应用名/PID> <0-100>\n");
      printf ("   audioctl app-mute <应用名/PID>\n");
      printf ("   audioctl app-unmute <应用名/PID>\n");
    }
  else
    {
      printf ("暂无应用通过虚拟设备播放音频\n");
      printf ("\n提示: 启动音乐或视频应用，音频将自动路由到虚拟设备\n");
    }

  printf ("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

  // 清理资源
  if (apps)
    free (apps);
  ipc_client_disconnect (&ctx);
  ipc_client_cleanup (&ctx);
}

int
app_volume_cli_set (const char *appNameOrPid, Float32 volume)
{
  (void) appNameOrPid;
  (void) volume;
  printf ("错误: 音量控制功能正在维护中 (重构 IPC 架构)\n");
  return 1;
}

int
app_volume_cli_mute (const char *appNameOrPid, bool mute)
{
  (void) appNameOrPid;
  (void) mute;
  printf ("错误: 静音控制功能正在维护中 (重构 IPC 架构)\n");
  return 1;
}
