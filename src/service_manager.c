//
// Created by AhogeK on 11/22/24.
// Modified on 02/11/26: 迁移 PID 文件到 ~/Library/Application Support/audioctl/
//

#include "service_manager.h"
#include "constants.h"
#include "ipc/ipc_protocol.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <pwd.h>

// 获取当前用户名
static const char *
get_current_username (void)
{
  uid_t uid = getuid ();
  struct passwd pwd;
  struct passwd *result = NULL;

  long bufsize = sysconf (_SC_GETPW_R_SIZE_MAX);
  if (bufsize == -1)
    bufsize = 16384;

  char *buffer = malloc (bufsize);
  if (buffer == NULL)
    {
      fprintf (stderr, "内存分配失败\n");
      return "unknown";
    }

  int ret = getpwuid_r (uid, &pwd, buffer, bufsize, &result);

  if (ret != 0 || result == NULL)
    {
      free (buffer);
      return "unknown";
    }

  const char *username = strdup (pwd.pw_name);
  free (buffer);
  return username;
}

void
print_version (void)
{
  printf ("%s version %s\n", SERVICE_NAME, SERVICE_VERSION);
}

// 写入日志
static void
write_log (const char *message)
{
  char log_path[PATH_MAX];
  if (get_log_file_path (log_path, sizeof (log_path)) != 0)
    return;

  FILE *log_file = fopen (log_path, "a");
  if (!log_file)
    return;

  time_t now;
  struct tm time_info;
  char date[32];

  time (&now);
  localtime_r (&now, &time_info);
  strftime (date, sizeof (date), "%Y-%m-%d %H:%M:%S", &time_info);

  fprintf (log_file, "[%s] [%s v%s] %s\n", date, SERVICE_NAME, SERVICE_VERSION,
	   message);
  fchmod (fileno (log_file), FILE_MODE);
  fclose (log_file);
}

// 读取PID文件
static pid_t
read_pid_file (void)
{
  char pid_path[PATH_MAX];
  if (get_pid_file_path (pid_path, sizeof (pid_path)) != 0)
    return -1;

  FILE *pid_file = fopen (pid_path, "r");
  if (!pid_file)
    return -1;

  char buffer[32];
  if (fgets (buffer, sizeof (buffer), pid_file) == NULL)
    {
      fclose (pid_file);
      return -1;
    }
  fclose (pid_file);

  errno = 0;
  char *endptr;
  long pid = strtol (buffer, &endptr, 10);

  if (endptr == buffer || (*endptr != '\n' && *endptr != '\0')
      || errno == ERANGE || pid < 1 || pid > INT32_MAX)
    {
      return -1;
    }

  return (pid_t) pid;
}

// 写入PID文件
static int
write_pid_file (pid_t pid)
{
  char pid_path[PATH_MAX];
  if (get_pid_file_path (pid_path, sizeof (pid_path)) != 0)
    return -1;

  FILE *pid_file = fopen (pid_path, "w");
  if (!pid_file)
    return -1;

  fprintf (pid_file, "%d\n", pid);
  fchmod (fileno (pid_file), FILE_MODE);
  fclose (pid_file);

  return 0;
}

// 检查进程是否在运行
bool
service_is_running (void)
{
  pid_t pid = read_pid_file ();
  if (pid == -1)
    return false;

  return (kill (pid, 0) == 0);
}

// 守护进程初始化
static void
init_daemon (void)
{
  pid_t pid = fork ();
  if (pid < 0)
    exit (EXIT_FAILURE);
  if (pid > 0)
    exit (EXIT_SUCCESS);

  if (setsid () < 0)
    exit (EXIT_FAILURE);

  signal (SIGHUP, SIG_IGN);

  pid = fork ();
  if (pid < 0)
    exit (EXIT_FAILURE);
  if (pid > 0)
    exit (EXIT_SUCCESS);

  if (chdir ("/") < 0)
    exit (EXIT_FAILURE);

  umask (0);

  long maxfd = sysconf (_SC_OPEN_MAX);
  if (maxfd == -1)
    maxfd = 1024;
  if (maxfd > INT32_MAX)
    maxfd = INT32_MAX;

  for (int fd = (int) maxfd; fd >= 0; fd--)
    close (fd);

  int fd = open ("/dev/null", O_RDWR);
  if (fd == -1)
    exit (EXIT_FAILURE);

  if (dup2 (fd, STDIN_FILENO) == -1)
    exit (EXIT_FAILURE);
  if (dup2 (fd, STDOUT_FILENO) == -1)
    exit (EXIT_FAILURE);
  if (dup2 (fd, STDERR_FILENO) == -1)
    exit (EXIT_FAILURE);
  if (fd > STDERR_FILENO)
    close (fd);
}

// 运行守护进程
_Noreturn void
run_daemon (void)
{
  // 【关键修复】先初始化 daemon（fork 并关闭文件描述符）
  init_daemon ();

  // 【关键修复】在 init_daemon() 之后写入 PID
  // 此时我们已经是最终的守护进程（孙子进程）
  write_pid_file (getpid ());

  write_log ("守护进程启动");

  while (1)
    {
      write_log ("服务正在运行...");
      sleep (3600);
    }
}

// 启动服务
ServiceStatus
service_start (void)
{
  if (service_is_running ())
    {
      printf ("服务已在运行中\n");
      return SERVICE_STATUS_ALREADY_RUNNING;
    }

  pid_t pid = fork ();
  if (pid < 0)
    {
      printf ("服务启动失败\n");
      return SERVICE_STATUS_ERROR;
    }

  if (pid == 0)
    {
      run_daemon ();
    }

  // 等待守护进程启动
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
  nanosleep (&ts, NULL);

  if (service_is_running ())
    {
      printf ("服务启动成功 (PID: %d)\n", read_pid_file ());
      return SERVICE_STATUS_SUCCESS;
    }

  printf ("服务启动失败\n");
  return SERVICE_STATUS_ERROR;
}

// 停止服务
ServiceStatus
service_stop (void)
{
  pid_t pid = read_pid_file ();
  if (pid <= 0)
    {
      printf ("服务未运行\n");
      return SERVICE_STATUS_NOT_RUNNING;
    }

  if (kill (pid, SIGTERM) == 0)
    {
      // 删除 PID 文件
      char pid_path[PATH_MAX];
      if (get_pid_file_path (pid_path, sizeof (pid_path)) == 0)
	{
	  unlink (pid_path);
	}
      printf ("服务停止成功\n");
      return SERVICE_STATUS_SUCCESS;
    }

  printf ("服务停止失败: %s\n", strerror (errno));
  return SERVICE_STATUS_ERROR;
}

// 重启服务
ServiceStatus
service_restart (void)
{
  printf ("正在重启 %s 服务...\n", SERVICE_NAME);

  ServiceStatus stop_status = service_stop ();
  if (stop_status != SERVICE_STATUS_SUCCESS
      && stop_status != SERVICE_STATUS_NOT_RUNNING)
    {
      return stop_status;
    }

  // 等待服务停止
  sleep (1);

  return service_start ();
}

// 检查是否有root权限
bool
check_root_privileges (void)
{
  return (getuid () == 0);
}

void
print_service_status (void)
{
  pid_t pid = read_pid_file ();
  bool is_running = service_is_running ();

  printf ("%s 服务状态：\n", SERVICE_NAME);

  if (is_running)
    {
      printf ("● %s - 版本 %s\n", SERVICE_NAME, SERVICE_VERSION);
      printf ("状态：" ANSI_COLOR_BOLD_GREEN "运行中" ANSI_COLOR_RESET
	      " (PID: %d)\n",
	      pid);

      // 使用 sysctl 获取进程启动时间
      struct kinfo_proc proc_info;
      size_t proc_info_size = sizeof (proc_info);
      int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};

      if (sysctl (mib, 4, &proc_info, &proc_info_size, NULL, 0) == 0)
	{
	  time_t start_time = proc_info.kp_proc.p_un.__p_starttime.tv_sec;
	  char time_str[100];
	  struct tm tm_info;
	  localtime_r (&start_time, &tm_info);
	  strftime (time_str, sizeof (time_str), "%Y-%m-%d %H:%M:%S", &tm_info);
	  printf ("启动时间：%s\n", time_str);
	}

      // 检查日志文件
      char log_path[PATH_MAX];
      if (get_log_file_path (log_path, sizeof (log_path)) == 0)
	{
	  struct stat log_stat;
	  if (stat (log_path, &log_stat) == 0)
	    {
	      printf ("日志文件：%s\n", log_path);
	      printf ("日志大小：%.2f KB\n", (float) log_stat.st_size / 1024);
	    }
	}

      // 显示 PID 文件路径
      char pid_path[PATH_MAX];
      if (get_pid_file_path (pid_path, sizeof (pid_path)) == 0)
	{
	  printf ("PID 文件：%s\n", pid_path);
	}
    }
  else
    {
      printf ("● %s - 版本 %s\n", SERVICE_NAME, SERVICE_VERSION);
      printf ("状态：" ANSI_COLOR_BOLD_RED "未运行" ANSI_COLOR_RESET "\n");
    }

  // 显示支持目录信息
  char support_dir[PATH_MAX];
  if (get_support_directory (support_dir, sizeof (support_dir)) == 0)
    {
      printf ("\n配置目录：%s\n", support_dir);
    }

  printf ("当前用户：%s\n", get_current_username ());

  // 检查 IPC 服务状态
  printf ("\n========== IPC 服务状态 ==========\n");

  char socket_path[PATH_MAX];
  if (get_ipc_socket_path (socket_path, sizeof (socket_path)) == 0)
    {
      struct stat sock_stat;
      if (stat (socket_path, &sock_stat) == 0 && S_ISSOCK (sock_stat.st_mode))
	{
	  printf ("● IPC 服务\n");
	  printf ("状态：" ANSI_COLOR_BOLD_GREEN "运行中" ANSI_COLOR_RESET
		  "\n");
	  printf ("Socket：%s\n", socket_path);

	  // 检查 socket 文件修改时间
	  char time_str[100];
	  struct tm tm_info;
	  localtime_r (&sock_stat.st_mtime, &tm_info);
	  strftime (time_str, sizeof (time_str), "%Y-%m-%d %H:%M:%S", &tm_info);
	  printf ("启动时间：%s\n", time_str);
	}
      else
	{
	  printf ("● IPC 服务\n");
	  printf ("状态：" ANSI_COLOR_BOLD_RED "未运行" ANSI_COLOR_RESET "\n");
	  printf ("Socket：%s (不存在)\n", socket_path);
	}
    }
  else
    {
      printf ("● IPC 服务：无法获取 Socket 路径\n");
    }
}
