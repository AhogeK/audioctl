#include <errno.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#include "app_volume_control.h"
#include "audio_apps.h"
#include "audio_control.h"
#include "audio_router.h"
#include "constants.h"
#include "ipc/ipc_protocol.h"
#include "ipc/ipc_server.h"
#include "service_manager.h"
#include "virtual_device_manager.h"

// ============================================================================
// Router 后台服务管理
// ============================================================================

static void
kill_router (void)
{
  system ("pkill -f 'audioctl internal-route' >/dev/null 2>&1");
}

static pid_t
spawn_router (const char *self_path, const char *physical_uid)
{
  kill_router ();

  pid_t pid;
  posix_spawnattr_t attr;
  posix_spawnattr_init (&attr);
  posix_spawnattr_setflags (&attr, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup (&attr, 0);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init (&actions);

  int dev_null = open ("/dev/null", O_WRONLY);
  if (dev_null >= 0)
    {
      posix_spawn_file_actions_adddup2 (&actions, dev_null, STDOUT_FILENO);
      posix_spawn_file_actions_adddup2 (&actions, dev_null, STDERR_FILENO);
      posix_spawn_file_actions_addclose (&actions, dev_null);
    }

  // 传递物理设备 UID 作为参数
  char uid_arg[512];
  snprintf (uid_arg, sizeof (uid_arg), "--router-target=%s", physical_uid);
  char *argv[] = {"audioctl", "internal-route", uid_arg, NULL};

  int ret = posix_spawn (&pid, self_path, &actions, &attr, argv, NULL);

  posix_spawn_file_actions_destroy (&actions);
  posix_spawnattr_destroy (&attr);
  if (dev_null >= 0)
    close (dev_null);

  return (ret == 0) ? pid : -1;
}

// ============================================================================
// IPC 服务管理
// ============================================================================

static void
kill_ipc_service (void)
{
  // 发送 SIGTERM 给 IPC 服务进程
  system ("pkill -f 'audioctl internal-ipc-service' >/dev/null 2>&1");

  // 清理 socket 文件
  char socket_path[PATH_MAX];
  if (get_ipc_socket_path (socket_path, sizeof (socket_path)) == 0)
    {
      unlink (socket_path);
    }
}

void
spawn_ipc_service (const char *self_path)
{
  kill_ipc_service (); // 确保旧的已停止

  pid_t pid;

  // 配置 spawn 属性
  posix_spawnattr_t attr;
  posix_spawnattr_init (&attr);

  // 设置进程组，脱离控制终端
  posix_spawnattr_setflags (&attr, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup (&attr, 0);

  // 配置文件描述符：只重定向 stdout，保留 stderr 用于错误诊断
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init (&actions);

  int dev_null = open ("/dev/null", O_WRONLY);
  if (dev_null >= 0)
    {
      posix_spawn_file_actions_adddup2 (&actions, dev_null, STDOUT_FILENO);
      // stderr 不重定向，保留用于错误输出
      posix_spawn_file_actions_addclose (&actions, dev_null);
    }

  // 构建参数数组
  char *argv[] = {"audioctl", "internal-ipc-service", NULL};

  // 启动进程
  int ret = posix_spawn (&pid, self_path, &actions, &attr, argv, NULL);

  // 清理资源
  posix_spawn_file_actions_destroy (&actions);
  posix_spawnattr_destroy (&attr);
  if (dev_null >= 0)
    close (dev_null);

  if (ret == 0)
    {
      printf ("🚀 IPC 服务已启动 (PID: %d)\n", pid);
      // 给服务一点时间初始化
      struct timespec ts = {0, 100000000}; // 100ms
      nanosleep (&ts, NULL);
    }
  else
    {
      fprintf (stderr, "⚠️  无法启动 IPC 服务: %s\n", strerror (ret));
    }
}

// 命令行选项和程序选项的定义
typedef struct
{
  const char shortOpt;
  const char *longOpt;
  const char *description;
  size_t flagOffset; // 使用偏移量代替直接指针
} CommandOption;

typedef struct
{
  bool showOnlyActive;
  bool showOnlyInput;
  bool showOnlyOutput;
  bool setInputVolume;
  bool setOutputVolume;
  // 后续添加更多选项
} ProgramOptions;

// 创建命令行选项和程序选项的关联
static const CommandOption *
getCommandOptions (void)
{
  static const CommandOption options[] = {
    {'a', "active", "只列出使用中的设备",
     offsetof (ProgramOptions, showOnlyActive)},
    {'i', "input", "只列出输入设备或设置输入设备音量",
     offsetof (ProgramOptions, showOnlyInput)},
    {'o', "output", "只列出输出设备或设置输出设备音量",
     offsetof (ProgramOptions, showOnlyOutput)},
    // 后续在这里添加更多选项
    {0, NULL, NULL, 0} // 结束标记
  };
  return options;
}

// 打印使用帮助信息
void
printUsage ()
{
  printf ("使用方法：\n");
  printf (" audioctl [命令] [参数]\n\n");
  printf ("========== 基础命令 ==========\n");
  printf (" help                   - 显示帮助信息\n");
  printf (" list                   - 显示所有音频设备\n");
  printf (
    " set -i/o [音量]        - 设置当前使用中的输入或输出设备的音量 (0-100)\n");
  printf (" set [设备ID]           - 将指定ID的设备设置为使用中\n\n");

  printf ("========== 虚拟设备命令 ==========\n");
  printf (" virtual-status         - 显示虚拟设备状态\n");
  printf (" use-virtual            - 切换到虚拟音频设备，自动启动所有服务\n");
  printf (" use-physical           - 恢复到物理设备，停止所有服务\n");
  printf (" agg-status             - 显示 Aggregate Device 状态\n\n");

  printf ("========== 应用音量控制 ==========\n");
  printf (" apps                   - 显示所有音频应用\n");
  printf (" app-volumes            - 显示所有应用音量控制列表\n");
  printf (" app-volume [应用] [音量] - 设置指定应用的音量 (0-100)\n");
  printf ("                          应用可以是PID或应用名称\n");
  printf (" app-mute [应用]        - 静音指定应用\n");
  printf (" app-unmute [应用]      - 取消静音指定应用\n\n");

  printf ("========== 系统命令 ==========\n");
  printf (" --version, -v          - 显示版本信息\n");
  printf (" --service-status       - 查看所有服务状态\n\n");

  printf ("========== 使用示例 ==========\n");
  printf (" audioctl virtual-status          # 检查虚拟设备状态\n");
  printf (" audioctl use-virtual             # 切换到虚拟设备（创建Aggregate "
	  "Device）\n");
  printf (" audioctl agg-status              # 查看Aggregate Device状态\n");
  printf (" audioctl app-volumes             # 查看应用音量列表\n");
  printf (" audioctl app-volume Safari 50    # 设置Safari音量为50%%\n");
  printf (" audioctl app-mute Chrome         # 静音Chrome\n");
  printf (" audioctl use-physical            # 恢复物理设备\n\n");

  // 使用专门用于显示帮助的选项数组
  const CommandOption *options = getCommandOptions ();

  printf ("\n选项：\n");
  for (int i = 0; options[i].shortOpt != 0; i++)
    {
      printf (" -%c, --%-12s - %s\n", options[i].shortOpt, options[i].longOpt,
	      options[i].description);
    }

  printf ("\n选项可组合使用，例如：\n");
  printf (" list -ai          - 只列出使用中的输入设备\n");
  printf (" list -ao          - 只列出使用中的输出设备\n");
  printf (" set  -o 44.1      - 将当前使用中的输出设备音量设置为 44.1%%\n");
  printf (" set  -i 50        - 将当前使用中的输入设备音量设置为 50.0%%\n");
  printf (" set 117           - "
	  "将ID为117的设备设置为使用中（自动识别输入/输出设备）\n");
}

// 解析单个短选项
bool
parseShortOption (const char opt, const CommandOption *options,
		  ProgramOptions *opts)
{
  for (int i = 0; options[i].shortOpt != 0; i++)
    {
      if (opt == options[i].shortOpt)
	{
	  // 使用偏移量设置对应的标志位
	  bool *flag = (bool *) ((char *) opts + options[i].flagOffset);
	  *flag = true;
	  return true;
	}
    }
  printf ("警告：未知选项: -%c\n", opt);
  return false;
}

// 解析长选项
bool
parseLongOption (const char *arg, const CommandOption *options,
		 ProgramOptions *opts)
{
  // 跳过开头的 --
  arg += 2;

  for (int i = 0; options[i].longOpt != 0; i++)
    {
      if (strcmp (arg, options[i].longOpt) == 0)
	{
	  // 使用偏移量设置对应的标志位
	  bool *flag = (bool *) ((char *) opts + options[i].flagOffset);
	  *flag = true;
	  return true;
	}
    }
  printf ("警告：未知选项: --%s\n", arg);
  return false;
}

// 验证选项组合的有效性
static bool
validateOptionCombination (const ProgramOptions *opts)
{
  if (opts->showOnlyInput && opts->showOnlyOutput)
    {
      printf ("错误：不能同时指定输入和输出设备\n");
      printUsage ();
      return false;
    }
  return true;
}

// 处理无效参数
static bool
handleInvalidArgument (const char *arg, bool isLongOption)
{
  if (isLongOption)
    {
      printf ("错误：无效的长参数 '%s'\n", arg);
      printf ("长参数格式必须是 '--option'\n");
    }
  else
    {
      printf ("错误：无效的参数 '%s'\n", arg);
      printf ("参数格式必须是 '-x' 或 '--option'\n");
    }
  printUsage ();
  return false;
}

// 处理单个参数
static bool
handleArgument (const char *arg, const CommandOption *options,
		ProgramOptions *opts)
{
  // 检查是否是选项参数
  if (arg[0] != '-')
    {
      printf ("错误：无效的参数 '%s'\n", arg);
      printf ("所有参数必须以 '-' 或 '--' 开头\n");
      printUsage ();
      return false;
    }

  // 检查是否只有一个横杠
  if (arg[1] == '\0')
    {
      return handleInvalidArgument (arg, false);
    }

  // 处理长选项
  if (arg[1] == '-')
    {
      if (arg[2] == '\0')
	{
	  return handleInvalidArgument (arg, true);
	}
      return parseLongOption (arg, options, opts);
    }

  // 处理短选项
  const char *c = arg + 1;
  while (*c != '\0')
    {
      if (!parseShortOption (*c, options, opts))
	{
	  return false;
	}
      c++;
    }

  return true;
}

// 解析所有参数
bool
parseOptions (const int argc, char *argv[], ProgramOptions *opts)
{
  // 初始化所有选项为 false
  memset (opts, 0, sizeof (ProgramOptions));

  const CommandOption *options = getCommandOptions ();

  // 从参数2开始解析
  for (int i = 2; i < argc; i++)
    {
      if (!handleArgument (argv[i], options, opts))
	{
	  return false;
	}
    }

  // 验证选项组合的有效性
  return validateOptionCombination (opts);
}

// 辅助函数：根据 UID 获取设备名称
// 返回值：true 表示成功获取到设备名称，false 表示失败（name 中为 UID）
static bool
get_device_name_by_uid (const char *uid, char *name, const size_t name_size)
{
  AudioDeviceID device = find_device_by_uid (uid);
  if (device != kAudioObjectUnknown)
    {
      AudioDeviceInfo info;
      if (getDeviceInfo (device, &info) == noErr)
	{
	  strncpy (name, info.name, name_size - 1);
	  name[name_size - 1] = '\0';
	  return true;
	}
    }
  strncpy (name, uid, name_size - 1);
  name[name_size - 1] = '\0';
  return false;
}

static void
printDeviceTypeAndChannels (const AudioDeviceInfo *info)
{
  switch (info->deviceType)
    {
    case kDeviceTypeInput:
      printf ("输入设备 (通道数: %d)", info->inputChannelCount);
      break;
    case kDeviceTypeOutput:
      printf ("输出设备 (通道数: %d)", info->outputChannelCount);
      break;
    case kDeviceTypeInputOutput:
      printf ("输入/输出设备 (输入通道: %d, 输出通道: %d, 总通道: %d)",
	      info->inputChannelCount, info->outputChannelCount,
	      info->channelCount);
      break;
    default:
      printf ("未知类型");
      break;
    }
}

static void
printVolumeInfo (const AudioDeviceInfo *info)
{
  if (info->deviceType == kDeviceTypeInput)
    {
      printf ("\n  输入音量: ");
      if (!info->hasVolumeControl
	  || info->transportType
	       == kAudioDeviceTransportTypeContinuityCaptureWired
	  || info->transportType
	       == kAudioDeviceTransportTypeContinuityCaptureWireless)
	{
	  printf ("不可调节");
	}
      else
	{
	  printf ("%.1f%%", info->volume * 100.0);
	}
    }
  else if (info->deviceType == kDeviceTypeOutput
	   || info->deviceType == kDeviceTypeInputOutput)
    {
      printf ("\n  音量: ");

      if (!info->hasVolumeControl)
	{
	  printf ("不可调节");
	}
      else
	{
	  printf ("%.1f%%", info->volume * 100.0);
	}

      // 只为输出设备显示静音状态
      printf (", 静音: %s", info->isMuted ? "是" : "否");
    }
}

void
printDeviceInfo (const AudioDeviceInfo *info)
{
  printf ("ID: %d, 名称: %s, ", info->deviceId, info->name);

  printDeviceTypeAndChannels (info);

  printf ("\n  传输类型: %s", getTransportTypeName (info->transportType));

  // 【特殊处理】虚拟设备：显示绑定状态
  if (info->transportType == kAudioDeviceTransportTypeVirtual)
    {
      char boundUid[256] = {0};
      if (get_bound_physical_device_uid (boundUid, sizeof (boundUid)))
	{
	  // 根据 UID 查找设备并获取友好名称
	  AudioDeviceID boundDevice = find_device_by_uid (boundUid);
	  if (boundDevice != kAudioObjectUnknown)
	    {
	      AudioDeviceInfo boundInfo;
	      printf ("\n  绑定状态: 已绑定到 %s",
		      getDeviceInfo (boundDevice, &boundInfo) == noErr
			? boundInfo.name
			: boundUid);
	    }
	  else
	    {
	      printf ("\n  绑定状态: 已绑定到 %s", boundUid);
	    }
	}
      else
	{
	  printf ("\n  绑定状态: 未绑定");
	}
    }

  printVolumeInfo (info);

  printf ("\n  采样率: %d Hz", info->sampleRate);
  if (info->bitsPerChannel > 0)
    {
      printf (", 位深度: %d bits", info->bitsPerChannel);
      printf (", 格式: %s", getFormatFlagsDescription (info->formatFlags));
    }
  printf (", 状态: %s%s%s", info->isRunning ? ANSI_COLOR_GREEN : "",
	  info->isRunning ? "使用中" : "空闲",
	  info->isRunning ? ANSI_COLOR_RESET : "");
  printf ("\n\n");
}

// 处理版本命令
static bool
handleVersionCommand (const char *arg)
{
  if (strcmp (arg, "--version") == 0 || strcmp (arg, "-v") == 0)
    {
      print_version ();
      return true;
    }
  return false;
}

// 打印匹配的设备数量信息
static void
printMatchedDeviceCount (UInt32 matchedCount, const ProgramOptions *opts)
{
  printf ("发现 %d 个", matchedCount);
  if (opts->showOnlyActive)
    {
      printf ("使用中的");
    }
  if (opts->showOnlyInput)
    {
      printf ("输入");
    }
  else if (opts->showOnlyOutput)
    {
      printf ("输出");
    }
  printf ("音频设备:\n");
}

// 检查设备是否匹配过滤条件
static bool
isDeviceMatched (const AudioDeviceInfo *device, const ProgramOptions *opts)
{
  // 检查活跃状态
  if (opts->showOnlyActive && !device->isRunning)
    {
      return false;
    }

  // 检查设备类型
  if (opts->showOnlyInput && device->deviceType != kDeviceTypeInput)
    {
      return false;
    }
  if (opts->showOnlyOutput && device->deviceType != kDeviceTypeOutput
      && device->deviceType != kDeviceTypeInputOutput)
    {
      return false;
    }

  return true;
}

// 计算匹配的设备数量
static UInt32
countMatchedDevices (const AudioDeviceInfo *devices, UInt32 deviceCount,
		     const ProgramOptions *opts)
{
  UInt32 matchedCount = 0;
  for (UInt32 i = 0; i < deviceCount; i++)
    {
      if (isDeviceMatched (&devices[i], opts))
	{
	  matchedCount++;
	}
    }
  return matchedCount;
}

// 处理设备列表命令
static int
handleListCommand (int argc, char *argv[])
{
  ProgramOptions opts;
  if (!parseOptions (argc, argv, &opts))
    {
      return 1;
    }

  AudioDeviceInfo *devices;
  UInt32 deviceCount;
  const OSStatus status = getDeviceList (&devices, &deviceCount);

  if (status != noErr)
    {
      printf ("获取设备列表失败，错误码: %d\n", status); // 移除了多余的类型转换
      return 1;
    }

  // 计算并显示匹配的设备数量
  UInt32 matchedCount = countMatchedDevices (devices, deviceCount, &opts);
  printMatchedDeviceCount (matchedCount, &opts);

  // 打印设备信息
  for (UInt32 i = 0; i < deviceCount; i++)
    {
      if (isDeviceMatched (&devices[i], &opts))
	{
	  printDeviceInfo (&devices[i]);
	}
    }

  free (devices);
  return 0;
}

// 处理应用程序音频信息命令
static int
handleAppsCommand (void)
{
  AudioAppInfo *apps;
  UInt32 appCount;
  const OSStatus status = getAudioApps (&apps, &appCount);

  if (status != noErr)
    {
      printf ("获取应用程序音频信息失败，错误码: %d\n", status);
      return 1;
    }

  printf ("发现 %d 个正在使用音频的应用程序:\n\n", appCount);
  for (UInt32 i = 0; i < appCount; i++)
    {
      printf ("应用: %s (PID: %d)\n", apps[i].name, apps[i].pid);
      printf ("音量: %.0f%%\n", apps[i].volume * 100);
    }

  freeAudioApps (apps);
  return 0;
}

// 基础辅助函数实现
static void
printUsageError (void)
{
  printf ("错误：'set' 命令格式不正确\n");
  printf ("用法：audioctl set -i/o [音量值]\n");
  printf ("      audioctl set [设备ID]\n");
  printf ("示例：audioctl set -o 44.1\n");
  printf ("      audioctl set -i 50\n");
  printf ("      audioctl set 117\n");
}

static float
parseVolume (const char *volumeStr)
{
  char *endptr;
  float volume = strtof (volumeStr, &endptr);
  return (*endptr == '\0' && volume >= 0.0f && volume <= 100.0f) ? volume
								 : -1.0f;
}

static long
parseDeviceId (const char *deviceIdStr)
{
  char *endptr;
  long deviceId = strtol (deviceIdStr, &endptr, 10);
  return (*endptr == '\0' && deviceId > 0 && deviceId <= INT32_MAX) ? deviceId
								    : -1;
}

static const char *
getDeviceTypeString (AudioDeviceType deviceType)
{
  switch (deviceType)
    {
    case kDeviceTypeInput:
      return "输入";
    case kDeviceTypeOutput:
      return "输出";
    case kDeviceTypeInputOutput:
      return "输入/输出";
    default:
      return "未知类型";
    }
}

static bool
findRunningDevice (bool isInput, AudioDeviceID *deviceId, char **deviceName)
{
  AudioDeviceInfo *devices;
  UInt32 deviceCount;
  OSStatus status = getDeviceList (&devices, &deviceCount);
  if (status != noErr)
    {
      return false;
    }

  bool found = false;
  for (UInt32 i = 0; i < deviceCount; i++)
    {
      if (devices[i].isRunning
	  && ((isInput && devices[i].deviceType == kDeviceTypeInput)
	      || (!isInput && devices[i].deviceType == kDeviceTypeOutput)))
	{
	  *deviceId = devices[i].deviceId;
	  *deviceName = strdup (devices[i].name); // 复制设备名称
	  found = true;
	  break;
	}
    }

  free (devices);
  return found;
}

// 业务逻辑函数实现
static int
handleVolumeSet (int argc, char *argv[])
{
  if (argc != 4)
    {
      printf ("错误：设置音量需要一个选项和一个音量值\n");
      printf ("用法：audioctl set -i/o [音量值]\n");
      return 1;
    }

  bool isInput = strcmp (argv[2], "-i") == 0;
  if (!isInput && strcmp (argv[2], "-o") != 0)
    {
      printf ("错误：无效的选项 '%s'\n", argv[2]);
      printf ("选项必须是 '-i' (输入设备) 或 '-o' (输出设备)\n");
      return 1;
    }

  float volume = parseVolume (argv[3]);
  if (volume < 0.0f)
    {
      printf ("错误：音量值必须是 0 到 100 之间的数字\n");
      return 1;
    }

  AudioDeviceID targetDeviceId;
  char *deviceName;

  // 串联模式下，虚拟设备激活时，控制绑定的物理设备音量
  if (!isInput && virtual_device_is_active_output ())
    {
      char boundUid[256];
      if (get_bound_physical_device_uid (boundUid, sizeof (boundUid)))
	{
	  targetDeviceId = find_device_by_uid (boundUid);
	  if (targetDeviceId != kAudioObjectUnknown)
	    {
	      AudioDeviceInfo info;
	      if (getDeviceInfo (targetDeviceId, &info) == noErr)
		{
		  deviceName = strdup (info.name);
		}
	      else
		{
		  deviceName = strdup (boundUid);
		}
	    }
	  else
	    {
	      printf ("错误：无法找到绑定的物理设备\n");
	      return 1;
	    }
	}
      else
	{
	  printf ("错误：无法获取绑定设备信息，请重新运行 use-virtual\n");
	  return 1;
	}
    }
  else
    {
      if (!findRunningDevice (isInput, &targetDeviceId, &deviceName))
	{
	  printf ("错误：没有找到使用中的%s设备\n", isInput ? "输入" : "输出");
	  return 1;
	}
    }

  OSStatus status = setDeviceVolume (targetDeviceId, volume / 100.0f);
  if (status != noErr)
    {
      printf ("错误：设置%s设备 '%s' 的音量失败\n", isInput ? "输入" : "输出",
	      deviceName);
      free (deviceName);
      return 1;
    }

  printf ("已将%s设备 '%s' 的音量设置为 %.1f%%\n", isInput ? "输入" : "输出",
	  deviceName, volume);
  free (deviceName); // 直接释放，无需类型转换
  return 0;
}

static int
handleDeviceSwitch (int argc, char *argv[])
{
  if (argc != 3)
    {
      printf ("错误：设置使用中设备只需要设备ID\n");
      printf ("用法：audioctl set [设备ID]\n");
      return 1;
    }

  long deviceId = parseDeviceId (argv[2]);
  if (deviceId <= 0)
    {
      printf ("错误：无效的设备ID\n");
      return 1;
    }

  AudioDeviceInfo deviceInfo;
  OSStatus status = getDeviceInfo ((AudioDeviceID) deviceId, &deviceInfo);
  if (status != noErr)
    {
      printf ("错误：找不到ID为 %ld 的设备\n", deviceId);
      return 1;
    }

  status = setDeviceActive ((AudioDeviceID) deviceId);
  if (status != noErr)
    {
      printf ("错误：无法将设备 '%s' 设置为使用中\n", deviceInfo.name);
      return 1;
    }

  const char *deviceTypeStr = getDeviceTypeString (deviceInfo.deviceType);
  printf ("已将%s设备 '%s' (ID: %ld) 设置为使用中\n", deviceTypeStr,
	  deviceInfo.name, deviceId);
  return 0;
}

// 处理音量设置命令
static int
handleSetCommand (int argc, char *argv[])
{
  if (argc < 3)
    {
      printUsageError ();
      return 1;
    }

  return (argv[2][0] == '-') ? handleVolumeSet (argc, argv)
			     : handleDeviceSwitch (argc, argv);
}

static int
handleAppVolumeCommands (int argc, char *argv[])
{
  // 串联模式下，检查虚拟设备是否激活
  if (!virtual_device_is_active ())
    {
      printf ("⚠️  虚拟设备未激活，无法使用应用音量控制\n");
      if (strcmp (argv[1], "app-volumes") == 0)
	{
	  printf ("\n");
	  virtual_device_print_status ();
	  printf ("\n请运行: audioctl use-virtual 激活\n");
	}
      else
	{
	  printf ("请运行: audioctl use-virtual 激活\n");
	}
      return 1;
    }

  app_volume_control_init ();
  int result = 0;

  if (strcmp (argv[1], "app-volumes") == 0)
    {
      app_volume_cli_list ();
    }
  else if (strcmp (argv[1], "app-volume") == 0)
    {
      if (argc < 4)
	{
	  printf ("错误: 需要应用名称/PID和音量值\n用法: audioctl app-volume "
		  "[应用] [音量]\n");
	  result = 1;
	}
      else
	{
	  float volume = strtof (argv[3], NULL);
	  result = app_volume_cli_set (argv[2], volume);
	}
    }
  else if (strcmp (argv[1], "app-mute") == 0
	   || strcmp (argv[1], "app-unmute") == 0)
    {
      if (argc < 3)
	{
	  printf ("错误: 需要应用名称/PID\n用法: audioctl %s [应用]\n",
		  argv[1]);
	  result = 1;
	}
      else
	{
	  bool mute = (strcmp (argv[1], "app-mute") == 0);
	  result = app_volume_cli_mute (argv[2], mute);
	}
    }

  app_volume_control_cleanup ();
  return result;
}

static int
handleVirtualDeviceCommands (int __unused argc, char *argv[])
{
  if (strcmp (argv[1], "virtual-status") == 0)
    {
      virtual_device_print_status ();
      return 0;
    }

  if (strcmp (argv[1], "use-virtual") == 0)
    {
      if (!virtual_device_is_installed ())
	{
	  printf ("❌ 虚拟音频设备未安装\n\n请运行以下命令安装:\n  cd "
		  "cmake-build-debug\n  sudo ninja "
		  "install\n\n安装后重启音频服务:\n  sudo launchctl kickstart "
		  "-k system/com.apple.audio.coreaudiod\n");
	  return 1;
	}

      // 检查是否已经处于虚拟设备状态
      if (virtual_device_is_active_output ())
	{
	  printf ("ℹ️  已经绑定到虚拟音频设备\n");
	  virtual_device_print_status ();
	  return 0;
	}

      // Use serial mode: App -> Virtual Device -> Router -> Physical Speaker
      // This is required for per-app volume control

      // Important: do not query current physical device before switching!
      // Querying default device locks CoreAudio state and prevents switching
      // Instead, after activation, Router will automatically bind to current
      // default physical device

      // 【关键】先获取当前物理设备（在切换前！）
      AudioDeviceID physical_device = get_default_output_device ();
      char physical_uid[256] = {0};
      Float32 physical_volume = 1.0f; // 默认100%
      if (physical_device != kAudioObjectUnknown)
	{
	  AudioObjectPropertyAddress addr
	    = {kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
	       kAudioObjectPropertyElementMain};
	  CFStringRef uid_ref = NULL;
	  UInt32 uid_size = sizeof (CFStringRef);
	  OSStatus uid_status
	    = AudioObjectGetPropertyData (physical_device, &addr, 0, NULL,
					  &uid_size, &uid_ref);
	  if (uid_status == noErr && uid_ref != NULL)
	    {
	      CFStringGetCString (uid_ref, physical_uid, sizeof (physical_uid),
				  kCFStringEncodingUTF8);
	      CFRelease (uid_ref);

	      // 【关键】保存绑定的物理设备信息
	      save_bound_physical_device (physical_uid);
	    }

	  // 【音量同步】获取物理设备当前音量
	  AudioDeviceInfo device_info;
	  if (getDeviceInfo (physical_device, &device_info) == noErr)
	    {
	      physical_volume = device_info.volume;
	      printf ("🎚️  物理设备音量: %.0f%%\n", physical_volume * 100.0f);
	    }
	}

      // Switch to serial mode (use Virtual Device as default output)
      if (virtual_device_activate_with_router () != noErr)
	return 1;

      // 【音量同步】尝试同步音量到虚拟设备（静默处理，失败不提示）
      // Router 层已应用增益补偿，此处仅为尝试通知系统期望音量
      if (physical_volume < 1.0f)
	{
	  AudioDeviceID virtual_device = get_default_output_device ();
	  if (virtual_device != kAudioObjectUnknown)
	    {
	      (void) setDeviceVolume (virtual_device, physical_volume);
	      // 虚拟设备不支持标准音量控制是预期行为，静默处理
	    }
	}

      // 【步骤1】先启动 IPC 服务（基础设施就绪）
      char self_path[4096];
      uint32_t size = sizeof (self_path);
      if (_NSGetExecutablePath (self_path, &size) == 0)
	{
	  spawn_ipc_service (self_path);
	}

      // 【步骤2】启动 Router（带增益补偿）
      if (strlen (physical_uid) > 0)
	{
	  printf ("🔄 启动 Audio Router...\n");
	  OSStatus router_status
	    = audio_router_start_with_volume (physical_uid, physical_volume);
	  if (router_status != noErr)
	    {
	      fprintf (stderr, "❌ 启动 Router 失败: %d\n", router_status);
	    }
	  else
	    {
	      printf ("✅ Router 已启动（增益补偿: %.0f%%）\n",
		      physical_volume * 100.0f);
	      // 根据 UID 查找设备名称
	      char device_name[256];
	      (void) get_device_name_by_uid (physical_uid, device_name,
					     sizeof (device_name));
	      printf ("   目标设备: %s\n", device_name);
	      printf ("   缓冲区: %d 帧 (约 %d ms)\n",
		      ROUTER_BUFFER_FRAME_COUNT,
		      (ROUTER_BUFFER_FRAME_COUNT * 1000) / 48000);
	      printf ("   监控: 每 5 秒报告一次性能状态\n");
	    }

	  // 【步骤3】后台 Router 启动
	  pid_t router_pid = spawn_router (self_path, physical_uid);
	  if (router_pid > 0)
	    {
	      // 等待 Router 初始化
	      sleep (1);

	      // 显示启动状态
	      printf ("\n✅ Router 已启动 (PID: %d)\n", router_pid);
	      // 根据 UID 查找设备名称
	      char bg_device_name[256];
	      (void) get_device_name_by_uid (physical_uid, bg_device_name,
					     sizeof (bg_device_name));
	      printf ("   目标设备: %s\n", bg_device_name);
	      printf ("   缓冲区: %d 帧 (约 %d ms)\n",
		      ROUTER_BUFFER_FRAME_COUNT,
		      (ROUTER_BUFFER_FRAME_COUNT * 1000) / 48000);
	      printf ("   状态: 🟢 运行平稳\n");
	    }
	}
      else
	{
	  fprintf (stderr, "⚠️  无法获取物理设备，Router 未启动\n");
	}

      printf ("\n📝 提示: 使用 'audioctl virtual-status' 查看详细状态\n");
      printf ("       使用 'audioctl use-physical' 恢复物理设备\n");
      printf ("       查看实时日志: 'audioctl internal-route'\n\n");

      return 0;
    }

  if (strcmp (argv[1], "use-physical") == 0)
    {
      // 停止 Router
      printf ("⏹️  停止 Audio Router...\n");
      kill_router ();
      printf ("✅ Router 已停止\n");

      // 清除绑定信息
      clear_binding_info ();

      kill_ipc_service ();
      // 恢复到物理设备
      return virtual_device_deactivate () == noErr ? 0 : 1;
    }

  return 1;
}

static int
handleServiceCommands (const char *cmd)
{
  if (strcmp (cmd, "--service-status") == 0)
    {
      print_service_status ();
      return 0;
    }
  return 1;
}

// 主函数
int
main (const int argc, char *argv[])
{
  if (argc < 2)
    {
      printUsage ();
      return 1;
    }

  const char *cmd = argv[1];

  if (handleVersionCommand (cmd))
    return 0;
  if (strcmp (cmd, "help") == 0)
    {
      printUsage ();
      return 0;
    }

  if (strcmp (cmd, "list") == 0)
    return handleListCommand (argc, argv);
  if (strcmp (cmd, "set") == 0)
    return handleSetCommand (argc, argv);
  if (strcmp (cmd, "apps") == 0)
    return handleAppsCommand ();

  if (strncmp (cmd, "app-", 4) == 0)
    return handleAppVolumeCommands (argc, argv);

  if (strcmp (cmd, "virtual-status") == 0 || strcmp (cmd, "use-virtual") == 0
      || strcmp (cmd, "use-physical") == 0)
    {
      return handleVirtualDeviceCommands (argc, argv);
    }

  if (strcmp (cmd, "internal-route") == 0)
    {
      // 解析 --router-target 参数（仅后台启动时使用）
      char target_uid[256] = {0};
      for (int i = 2; i < argc; i++)
	{
	  if (strncmp (argv[i], "--router-target=", 16) == 0)
	    {
	      strncpy (target_uid, argv[i] + 16, sizeof (target_uid) - 1);
	      break;
	    }
	}

      // 如果指定了目标设备，说明是后台启动模式
      if (strlen (target_uid) > 0)
	{
	  OSStatus status = audio_router_start (target_uid);
	  if (status != noErr)
	    {
	      fprintf (stderr, "❌ 启动 Router 失败: %d\n", status);
	      return 1;
	    }

	  while (audio_router_is_running ())
	    {
	      sleep (1);
	    }

	  audio_router_stop ();
	  return 0;
	}

      // 没有指定目标设备，进入日志查看模式
      printf ("📊 Router 实时日志查看模式\n");
      printf ("═══════════════════════════════════════════════════\n\n");

      // 检查 Router 是否已在运行
      if (is_router_process_running ())
	{
	  printf ("✅ 检测到后台 Router 正在运行\n");
	  printf ("🔄 开始实时显示日志（按 Ctrl+C 退出）...\n\n");

	  // 执行 log stream 命令显示 Router 日志 (syslog 使用 --process)
	  execlp ("log", "log", "stream", "--process", "audioctl", NULL);
	  perror ("exec log stream failed");
	  return 1;
	}
      else
	{
	  // Router 未运行，询问是否要启动
	  printf ("⚠️  Router 未在后台运行\n\n");
	  printf ("选项:\n");
	  printf ("  1. 运行 'audioctl use-virtual' 启动后台 Router\n");
	  printf ("  2. 运行 'audioctl internal-route --foreground' "
		  "前台调试模式\n\n");
	  return 0;
	}
    }

  if (strcmp (cmd, "internal-ipc-service") == 0)
    {
      IPCServerContext ctx;

      if (ipc_server_init (&ctx) != 0)
	{
	  fprintf (stderr, "❌ 无法初始化 IPC 服务端\n");
	  return 1;
	}

      printf ("🚀 IPC 服务已启动 (PID: %d)\n", getpid ());
      ipc_server_run (&ctx);
      ipc_server_cleanup (&ctx);

      return 0;
    }

  if (strncmp (cmd, "--", 2) == 0)
    return handleServiceCommands (cmd);

  printf ("未知命令: %s\n", cmd);
  printUsage ();
  return 1;
}
