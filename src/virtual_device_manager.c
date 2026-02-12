//
// 虚拟音频设备管理模块实现
// Created by AhogeK on 02/05/26.
//

#include "virtual_device_manager.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aggregate_device_manager.h"
#include "audio_control.h"
#include "ipc/ipc_protocol.h"

#pragma mark - 设备状态持久化

// 保存/恢复设备状态文件路径
static const char *kDeviceStatePath
  = "/Users/ahogek/Library/Application Support/audioctl/last_device.txt";

// 保存当前默认输出设备到文件
static OSStatus
save_current_device (AudioDeviceID deviceId)
{
  // 确保目录存在
  char dirPath[PATH_MAX];
  snprintf (dirPath, sizeof (dirPath),
	    "/Users/%s/Library/Application Support/audioctl", getlogin ());
  mkdir (dirPath, 0755);

  FILE *fp = fopen (kDeviceStatePath, "w");
  if (!fp)
    {
      fprintf (stderr, "⚠️ 无法创建设备状态文件: %s\n", kDeviceStatePath);
      return -1;
    }

  fprintf (fp, "%u", deviceId);
  fclose (fp);
  return noErr;
}

// 从文件恢复之前的设备
static AudioDeviceID
restore_previous_device (void)
{
  FILE *fp = fopen (kDeviceStatePath, "r");
  if (!fp)
    {
      return kAudioObjectUnknown;
    }

  unsigned int deviceId = kAudioObjectUnknown;
  char buf[32];
  if (fgets (buf, sizeof (buf), fp))
    {
      deviceId = (unsigned int) strtoul (buf, NULL, 10);
    }
  fclose (fp);

  return deviceId;
}

#pragma mark - 内部辅助函数

// 获取所有音频设备列表
static OSStatus
get_all_devices (AudioDeviceID **devices, UInt32 *count)
{
  AudioObjectPropertyAddress propertyAddress
    = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
       kAudioObjectPropertyElementMain};

  UInt32 dataSize = 0;
  OSStatus status
    = AudioObjectGetPropertyDataSize (kAudioObjectSystemObject,
				      &propertyAddress, 0, NULL, &dataSize);
  if (status != noErr)
    return status;

  *count = dataSize / sizeof (AudioDeviceID);
  *devices = (AudioDeviceID *) malloc (dataSize);
  if (*devices == NULL)
    return -1;

  status
    = AudioObjectGetPropertyData (kAudioObjectSystemObject, &propertyAddress, 0,
				  NULL, &dataSize, *devices);
  if (status != noErr)
    {
      free (*devices);
      *devices = NULL;
    }

  return status;
}

// 获取设备的UID
static OSStatus
get_device_uid (AudioDeviceID deviceId, char *uid, size_t uidSize)
{
  CFStringRef uidRef = NULL;
  UInt32 dataSize = sizeof (CFStringRef);
  AudioObjectPropertyAddress propertyAddress
    = {kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
       kAudioObjectPropertyElementMain};

  OSStatus status = AudioObjectGetPropertyData (deviceId, &propertyAddress, 0,
						NULL, &dataSize, &uidRef);
  if (status != noErr || uidRef == NULL)
    return status;

  CFStringGetCString (uidRef, uid, (CFIndex) uidSize, kCFStringEncodingUTF8);
  CFRelease (uidRef);

  return noErr;
}

// 获取设备的名称
static OSStatus
get_device_name (AudioDeviceID deviceId, char *name, size_t nameSize)
{
  CFStringRef nameRef = NULL;
  UInt32 dataSize = sizeof (CFStringRef);
  AudioObjectPropertyAddress propertyAddress
    = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
       kAudioObjectPropertyElementMain};

  OSStatus status = AudioObjectGetPropertyData (deviceId, &propertyAddress, 0,
						NULL, &dataSize, &nameRef);
  if (status != noErr || nameRef == NULL)
    {
      // 尝试备用属性
      propertyAddress.mSelector = kAudioDevicePropertyDeviceNameCFString;
      status = AudioObjectGetPropertyData (deviceId, &propertyAddress, 0, NULL,
					   &dataSize, &nameRef);
      if (status != noErr || nameRef == NULL)
	return status;
    }

  CFStringGetCString (nameRef, name, (CFIndex) nameSize, kCFStringEncodingUTF8);
  CFRelease (nameRef);

  return noErr;
}

// 检查设备是否匹配虚拟设备
static bool
is_virtual_device (AudioDeviceID deviceId)
{
  char uid[256] = {0};
  char name[256] = {0};

  get_device_uid (deviceId, uid, sizeof (uid));
  get_device_name (deviceId, name, sizeof (name));

  return (strstr (uid, VIRTUAL_DEVICE_UID) != NULL
	  || strstr (name, "Virtual Audio") != NULL);
}

// 搜索所有设备以查找虚拟设备
static AudioDeviceID
search_for_virtual_device (void)
{
  AudioDeviceID *devices = NULL;
  UInt32 count = 0;
  AudioDeviceID found = kAudioObjectUnknown;

  if (get_all_devices (&devices, &count) == noErr && devices != NULL)
    {
      for (UInt32 i = 0; i < count; i++)
	{
	  if (is_virtual_device (devices[i]))
	    {
	      found = devices[i];
	      break;
	    }
	}
      free (devices);
    }
  return found;
}

// 查找虚拟设备
static AudioDeviceID
find_virtual_device (void)
{
  // 最多尝试 5 次，每次间隔 500ms
  for (int attempt = 0; attempt < 5; attempt++)
    {
      AudioDeviceID virtualDevice = search_for_virtual_device ();
      if (virtualDevice != kAudioObjectUnknown)
	{
	  return virtualDevice;
	}

      struct timespec ts = {0, 500000000}; // 500ms
      nanosleep (&ts, NULL);
    }

  return kAudioObjectUnknown;
}

// 获取默认输出设备ID
static AudioDeviceID
get_default_output_device (void)
{
  AudioDeviceID deviceId = kAudioObjectUnknown;
  UInt32 dataSize = sizeof (AudioDeviceID);
  AudioObjectPropertyAddress propertyAddress
    = {kAudioHardwarePropertyDefaultOutputDevice,
       kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

  AudioObjectGetPropertyData (kAudioObjectSystemObject, &propertyAddress, 0,
			      NULL, &dataSize, &deviceId);
  return deviceId;
}

// 获取默认输入设备ID
static AudioDeviceID
get_default_input_device (void)
{
  AudioDeviceID deviceId = kAudioObjectUnknown;
  UInt32 dataSize = sizeof (AudioDeviceID);
  AudioObjectPropertyAddress propertyAddress
    = {kAudioHardwarePropertyDefaultInputDevice,
       kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

  AudioObjectGetPropertyData (kAudioObjectSystemObject, &propertyAddress, 0,
			      NULL, &dataSize, &deviceId);
  return deviceId;
}

#pragma mark - 设备检测

bool
virtual_device_is_installed (void)
{
  AudioDeviceID virtualDevice = find_virtual_device ();
  return virtualDevice != kAudioObjectUnknown;
}

bool
virtual_device_get_info (VirtualDeviceInfo *outInfo)
{
  if (outInfo == NULL)
    return false;

  memset (outInfo, 0, sizeof (VirtualDeviceInfo));

  AudioDeviceID virtualDevice = find_virtual_device ();
  if (virtualDevice == kAudioObjectUnknown)
    {
      outInfo->isInstalled = false;
      return false;
    }

  outInfo->deviceId = virtualDevice;
  outInfo->isInstalled = true;

  get_device_name (virtualDevice, outInfo->name, sizeof (outInfo->name));
  get_device_uid (virtualDevice, outInfo->uid, sizeof (outInfo->uid));

  // 检查是否正在使用
  outInfo->isActive = virtual_device_is_active ();

  return true;
}

bool
virtual_device_is_active_output (void)
{
  AudioDeviceID virtualDevice = find_virtual_device ();
  if (virtualDevice == kAudioObjectUnknown)
    return false;

  AudioDeviceID defaultOutput = get_default_output_device ();

  // 如果默认输出是虚拟设备，或者默认输出是聚合设备（且该聚合设备包含虚拟设备）
  if (defaultOutput == virtualDevice)
    return true;

  if (aggregate_device_is_active ())
    {
      return true;
    }

  return false;
}

bool
virtual_device_is_active_input (void)
{
  AudioDeviceID virtualDevice = find_virtual_device ();
  if (virtualDevice == kAudioObjectUnknown)
    return false;

  AudioDeviceID defaultInput = get_default_input_device ();
  return virtualDevice == defaultInput;
}

bool
virtual_device_is_active (void)
{
  return virtual_device_is_active_output ()
	 || virtual_device_is_active_input ();
}

#pragma mark - 设备控制

OSStatus
virtual_device_set_as_default_output (void)
{
  AudioDeviceID virtualDevice = find_virtual_device ();
  if (virtualDevice == kAudioObjectUnknown)
    {
      fprintf (stderr, "错误: 虚拟音频设备未安装\n");
      return -1;
    }

  AudioObjectPropertyAddress propertyAddress
    = {kAudioHardwarePropertyDefaultOutputDevice,
       kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

  OSStatus status
    = AudioObjectSetPropertyData (kAudioObjectSystemObject, &propertyAddress, 0,
				  NULL, sizeof (AudioDeviceID), &virtualDevice);

  if (status == noErr)
    {
      printf ("已将虚拟音频设备设为默认输出\n");
    }
  else
    {
      fprintf (stderr, "设置默认输出设备失败: %d\n", status);
    }

  return status;
}

OSStatus
virtual_device_set_as_default_input (void)
{
  AudioDeviceID virtualDevice = find_virtual_device ();
  if (virtualDevice == kAudioObjectUnknown)
    {
      fprintf (stderr, "错误: 虚拟音频设备未安装\n");
      return -1;
    }

  AudioObjectPropertyAddress propertyAddress
    = {kAudioHardwarePropertyDefaultInputDevice,
       kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

  OSStatus status
    = AudioObjectSetPropertyData (kAudioObjectSystemObject, &propertyAddress, 0,
				  NULL, sizeof (AudioDeviceID), &virtualDevice);

  if (status == noErr)
    {
      printf ("已将虚拟音频设备设为默认输入\n");
    }
  else
    {
      fprintf (stderr, "设置默认输入设备失败: %d\n", status);
    }

  return status;
}

OSStatus
virtual_device_activate (void)
{
  OSStatus status1 = virtual_device_set_as_default_output ();
  OSStatus status2 = virtual_device_set_as_default_input ();

  if (status1 == noErr && status2 == noErr)
    {
      printf ("虚拟音频设备已激活\n");
      printf ("提示: 现在可以使用 'audioctl app-volume' 命令控制应用音量\n");
      return noErr;
    }

  return (status1 != noErr) ? status1 : status2;
}

// 【新架构】激活虚拟设备并启动 Router（串联模式）
// App -> Virtual Device -> Router -> Physical Device
OSStatus
virtual_device_activate_with_router (void)
{
  // 0. 【修复】保存当前默认设备，以便后续恢复
  AudioDeviceID previousDevice = get_default_output_device ();
  if (previousDevice != kAudioObjectUnknown)
    {
      save_current_device (previousDevice);
      printf ("💾 已保存当前设备 ID=%d，供后续恢复\n", previousDevice);
    }

  // 1. 【修复】使用 UID 查找虚拟设备，而不是硬编码 ID
  // CoreAudio 重启后设备 ID 会重新分配
  AudioDeviceID virtualDevice = kAudioObjectUnknown;
  {
    AudioObjectPropertyAddress addr
      = {kAudioHardwarePropertyTranslateUIDToDevice,
	 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef uidRef = CFStringCreateWithCString (NULL, VIRTUAL_DEVICE_UID,
						    kCFStringEncodingUTF8);
    UInt32 size = sizeof (virtualDevice);
    OSStatus findStatus
      = AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr,
				    sizeof (CFStringRef), &uidRef, &size,
				    &virtualDevice);
    CFRelease (uidRef);

    if (findStatus != noErr || virtualDevice == kAudioObjectUnknown)
      {
	fprintf (stderr, "❌ 虚拟音频设备未找到 (UID: %s)\n",
		 VIRTUAL_DEVICE_UID);
	return kAudioHardwareBadDeviceError;
      }
    printf ("🔍 找到虚拟设备: ID=%d, UID=%s\n", virtualDevice,
	    VIRTUAL_DEVICE_UID);
  }

  // 2. 【关键修复】直接设置虚拟设备为默认，不查询其他设备
  // 查询其他设备会导致 CoreAudio 状态改变，使设置失败

  AudioObjectPropertyAddress propertyAddress
    = {kAudioHardwarePropertyDefaultOutputDevice,
       kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

  // 重试最多 3 次
  int retry = 0;
  const int maxRetries = 3;
  while (retry < maxRetries)
    {
      OSStatus status
	= AudioObjectSetPropertyData (kAudioObjectSystemObject,
				      &propertyAddress, 0, NULL,
				      sizeof (AudioDeviceID), &virtualDevice);

      if (status == noErr)
	{
	  printf ("   AudioObjectSetPropertyData 成功\n");
	}
      else
	{
	  printf ("   AudioObjectSetPropertyData 失败: %d\n", status);
	}

      if (status == noErr)
	{
	  // 验证设置是否生效
	  struct timespec verifyTs = {0, 200000000}; // 200ms
	  nanosleep (&verifyTs, NULL);

	  AudioDeviceID currentDefault = get_default_output_device ();
	  printf ("   尝试 %d: 当前默认设备 ID=%d, 目标=%d\n", retry + 1,
		  currentDefault, virtualDevice);
	  if (currentDefault == virtualDevice)
	    {
	      printf ("✅ 设置成功！\n");
	      break; // 设置成功且已生效
	    }
	  printf ("⚠️  默认设备未立即生效，等待重试...\n");
	}
      else
	{
	  fprintf (stderr, "⚠️  设置默认设备失败 (尝试 %d/%d): %d\n", retry + 1,
		   maxRetries, status);
	}

      retry++;
      if (retry < maxRetries)
	{
	  struct timespec retryTs = {0, 300000000}; // 300ms
	  nanosleep (&retryTs, NULL);
	}
    }

  if (retry >= maxRetries)
    {
      fprintf (stderr, "❌ 无法将虚拟设备设为默认输出\n");
      return kAudioHardwareUnspecifiedError;
    }

  // 4. 【修复】不切换默认输入设备
  // 根据用户需求，只切换 output，保持 input 不变
  // propertyAddress.mSelector = kAudioHardwarePropertyDefaultInputDevice;
  // AudioObjectSetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0,
  // NULL, sizeof(AudioDeviceID),
  //                            &virtualDevice);

  // 5. 【修复】再次验证
  AudioDeviceID verifyOutput = get_default_output_device ();
  if (verifyOutput != virtualDevice)
    {
      fprintf (stderr, "❌ 验证失败：默认设备未切换到虚拟设备\n");
      return kAudioHardwareUnspecifiedError;
    }

  printf ("✅ 虚拟音频设备已设为默认输出\n");
  printf ("   音频流: 应用 → 虚拟设备(音量控制) → 物理扬声器\n");
  printf ("   提示: 启动 Router 后将自动转发音频\n");

  return noErr;
}

OSStatus
virtual_device_deactivate (void)
{
  // 【修复】首先尝试恢复之前保存的设备
  AudioDeviceID previousDevice = restore_previous_device ();
  if (previousDevice != kAudioObjectUnknown)
    {
      // 验证设备是否仍然有效
      char uid[256] = {0};
      OSStatus verifyStatus
	= get_device_uid (previousDevice, uid, sizeof (uid));
      if (verifyStatus == noErr && strstr (uid, VIRTUAL_DEVICE_UID) == NULL
	  && strstr (uid, "Virtual") == NULL)
	{
	  // 设备有效且不是虚拟设备，恢复到该设备
	  AudioObjectPropertyAddress propertyAddress
	    = {kAudioHardwarePropertyDefaultOutputDevice,
	       kAudioObjectPropertyScopeGlobal,
	       kAudioObjectPropertyElementMain};

	  OSStatus status
	    = AudioObjectSetPropertyData (kAudioObjectSystemObject,
					  &propertyAddress, 0, NULL,
					  sizeof (AudioDeviceID),
					  &previousDevice);

	  if (status == noErr)
	    {
	      printf ("✅ 已恢复到之前的设备 (ID=%d, UID=%s)\n", previousDevice,
		      uid);
	      return noErr;
	    }
	  else
	    {
	      fprintf (stderr,
		       "⚠️  恢复之前的设备失败: %d，将尝试查找其他物理设备\n",
		       status);
	    }
	}
      else
	{
	  printf ("⚠️  之前保存的设备已失效或不可用，将尝试查找其他物理设备\n");
	}
    }

  // 回退方案：查找第一个非虚拟设备并设为默认
  AudioDeviceID *devices = NULL;
  UInt32 count = 0;

  OSStatus status = get_all_devices (&devices, &count);
  if (status != noErr || devices == NULL)
    return status;

  AudioDeviceID firstPhysicalDevice = kAudioObjectUnknown;

  for (UInt32 i = 0; i < count; i++)
    {
      char uid[256] = {0};
      get_device_uid (devices[i], uid, sizeof (uid));

      // 跳过虚拟设备
      if (strstr (uid, VIRTUAL_DEVICE_UID) != NULL
	  || strstr (uid, "Virtual") != NULL)
	{
	  continue;
	}

      // 检查是否为输出设备
      AudioObjectPropertyAddress propertyAddress
	= {kAudioDevicePropertyStreamConfiguration,
	   kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain};

      UInt32 dataSize = 0;
      if (AudioObjectGetPropertyDataSize (devices[i], &propertyAddress, 0, NULL,
					  &dataSize)
	    == noErr
	  && dataSize > 0)
	{
	  firstPhysicalDevice = devices[i];
	  break;
	}
    }

  free (devices);

  if (firstPhysicalDevice == kAudioObjectUnknown)
    {
      fprintf (stderr, "错误: 未找到物理音频设备\n");
      return -1;
    }

  // 设为默认输出
  AudioObjectPropertyAddress propertyAddress
    = {kAudioHardwarePropertyDefaultOutputDevice,
       kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

  status
    = AudioObjectSetPropertyData (kAudioObjectSystemObject, &propertyAddress, 0,
				  NULL, sizeof (AudioDeviceID),
				  &firstPhysicalDevice);

  if (status == noErr)
    {
      printf ("已恢复到物理音频设备 (回退方案)\n");
    }

  return status;
}

#pragma mark - 状态报告

void
virtual_device_print_status (void)
{
  VirtualDeviceInfo info;

  printf ("\n========== 虚拟音频设备状态 ==========\n\n");

  if (!virtual_device_get_info (&info))
    {
      printf ("❌ 虚拟音频设备未安装\n\n");
      printf ("请运行以下命令安装:\n");
      printf ("  sudo ninja install\n");
      printf ("\n安装后可能需要重启音频服务:\n");
      printf (
	"  sudo launchctl kickstart -k system/com.apple.audio.coreaudiod\n");
      return;
    }

  printf ("✅ 虚拟音频设备已安装\n");
  printf ("   设备ID: %d\n", info.deviceId);
  printf ("   名称: %s\n", info.name);
  printf ("   UID: %s\n", info.uid);
  printf ("\n");

  // 只检查输出状态
  if (virtual_device_is_active_output ())
    {
      printf ("✅ 虚拟设备是当前默认输出设备\n");
    }
  else
    {
      printf ("⚠️  虚拟设备不是当前默认输出设备\n");
      printf ("   使用 'audioctl use-virtual' 切换到虚拟设备\n");
    }

  printf ("\n");

  // 应用音量控制状态
  if (virtual_device_can_control_app_volume ())
    {
      printf ("✅ 应用音量控制功能可用\n");
      printf ("   可以使用 'audioctl app-volume' 命令控制单个应用音量\n");
    }
  else
    {
      printf ("❌ 应用音量控制功能不可用\n");
      printf ("   原因: %s\n", virtual_device_get_app_volume_status ());
    }

  printf ("\n========== IPC 服务状态 ==========\n");

  // 检查 IPC 服务状态
  char socket_path[PATH_MAX];
  if (get_ipc_socket_path (socket_path, sizeof (socket_path)) == 0)
    {
      struct stat sock_stat;
      if (stat (socket_path, &sock_stat) == 0 && S_ISSOCK (sock_stat.st_mode))
	{
	  printf ("✅ IPC 服务运行中\n");
	  printf ("   Socket: %s\n", socket_path);

	  // 显示 socket 文件修改时间
	  char time_str[100];
	  struct tm tm_info;
	  localtime_r (&sock_stat.st_mtime, &tm_info);
	  strftime (time_str, sizeof (time_str), "%Y-%m-%d %H:%M:%S", &tm_info);
	  printf ("   启动时间: %s\n", time_str);
	}
      else
	{
	  printf ("❌ IPC 服务未运行\n");
	  printf ("   使用 'audioctl use-virtual' 启动服务\n");
	}
    }
  else
    {
      printf ("⚠️  无法获取 IPC Socket 路径\n");
    }

  printf ("\n====================================\n");
}

OSStatus
virtual_device_get_current_output_info (VirtualDeviceInfo *outInfo)
{
  if (outInfo == NULL)
    return paramErr;

  memset (outInfo, 0, sizeof (VirtualDeviceInfo));

  AudioDeviceID currentDevice = get_default_output_device ();
  if (currentDevice == kAudioObjectUnknown)
    return -1;

  outInfo->deviceId = currentDevice;
  get_device_name (currentDevice, outInfo->name, sizeof (outInfo->name));
  get_device_uid (currentDevice, outInfo->uid, sizeof (outInfo->uid));

  // 检查是否是虚拟设备
  outInfo->isInstalled = (strstr (outInfo->uid, VIRTUAL_DEVICE_UID) != NULL
			  || strstr (outInfo->name, "Virtual") != NULL);
  outInfo->isActive = true; // 既然是默认设备，就是active的

  return noErr;
}

#pragma mark - 应用音量控制前置检查

bool
virtual_device_can_control_app_volume (void)
{
  return virtual_device_is_installed () && virtual_device_is_active_output ();
}

const char *
virtual_device_get_app_volume_status (void)
{
  if (!virtual_device_is_installed ())
    {
      return "虚拟音频设备未安装";
    }

  if (!virtual_device_is_active_output ())
    {
      return "虚拟音频设备不是当前默认输出设备，请运行 'audioctl use-virtual'";
    }

  return "虚拟设备已就绪，应用音量控制可用";
}
