//
// Created by AhogeK on 02/12/26.
//

#include "ipc/ipc_client.h"
#include "ipc/ipc_protocol.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

// 配置参数
#define IPC_CONNECT_TIMEOUT_SEC 2
#define IPC_SEND_TIMEOUT_SEC 5
#define IPC_RECV_TIMEOUT_SEC 5
#define IPC_RECONNECT_MAX_ATTEMPTS 5
#define IPC_RECONNECT_BASE_DELAY_MS 100
#define IPC_CACHE_TTL_MS 100 // 缓存有效期 100ms

// 获取当前时间戳（毫秒）
static uint64_t
get_timestamp_ms (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return (uint64_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 设置 socket 超时
static int
set_socket_timeout (int fd, int timeout_sec)
{
  struct timeval tv;
  tv.tv_sec = timeout_sec;
  tv.tv_usec = 0;

  if (setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv)) < 0)
    {
      return -1;
    }
  if (setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof (tv)) < 0)
    {
      return -1;
    }
  return 0;
}

// 初始化客户端
int
ipc_client_init (IPCClientContext *ctx)
{
  if (ctx == NULL)
    return -1;

  memset (ctx, 0, sizeof (IPCClientContext));
  ctx->fd = -1;
  ctx->connected = false;
  ctx->cached_pid = -1;
  ctx->cached_volume = 1.0f; // 默认音量 100%
  ctx->cached_muted = false;
  ctx->cache_valid = false;
  ctx->reconnect_attempts = 0;

  return 0;
}

// 连接到服务端
int
ipc_client_connect (IPCClientContext *ctx)
{
  if (ctx == NULL)
    return -1;

  // 如果已连接，先断开
  if (ctx->connected)
    {
      ipc_client_disconnect (ctx);
    }

  // 获取 socket 路径
  char socket_path[PATH_MAX];
  if (get_ipc_socket_path (socket_path, sizeof (socket_path)) != 0)
    {
      fprintf (stderr, "无法获取 socket 路径\n");
      return -1;
    }

  // 检查 socket 文件是否存在
  if (access (socket_path, F_OK) != 0)
    {
      fprintf (stderr, "IPC 服务未运行 (socket 不存在)\n");
      return -1;
    }

  // 创建 socket
  ctx->fd = socket (AF_UNIX, SOCK_STREAM, 0);
  if (ctx->fd < 0)
    {
      perror ("socket");
      return -1;
    }

  // 设置超时
  if (set_socket_timeout (ctx->fd, IPC_CONNECT_TIMEOUT_SEC) < 0)
    {
      perror ("setsockopt");
      close (ctx->fd);
      ctx->fd = -1;
      return -1;
    }

  // 配置地址
  struct sockaddr_un addr;
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, socket_path, sizeof (addr.sun_path) - 1);

  // 连接
  if (connect (ctx->fd, (struct sockaddr *) &addr, sizeof (addr)) < 0
      && errno != EINPROGRESS && errno != EAGAIN)
    {
      perror ("connect");
      close (ctx->fd);
      ctx->fd = -1;
      return -1;
    }

  ctx->connected = true;
  ctx->last_activity = get_timestamp_ms ();
  ctx->reconnect_attempts = 0;

  return 0;
}

// 断开连接
void
ipc_client_disconnect (IPCClientContext *ctx)
{
  if (ctx == NULL)
    return;

  if (ctx->fd >= 0)
    {
      close (ctx->fd);
      ctx->fd = -1;
    }
  ctx->connected = false;
}

// 清理资源
void
ipc_client_cleanup (IPCClientContext *ctx)
{
  if (ctx == NULL)
    return;

  ipc_client_disconnect (ctx);
  memset (ctx, 0, sizeof (IPCClientContext));
  ctx->fd = -1;
  ctx->cached_pid = -1;
  ctx->cached_volume = 1.0f;
}

// 检查连接状态
bool
ipc_client_is_connected (IPCClientContext *ctx)
{
  if (ctx == NULL)
    return false;
  return ctx->connected && ctx->fd >= 0;
}

// 发送消息
int
ipc_client_send (IPCClientContext *ctx, const IPCMessageHeader *header,
		 const void *payload)
{
  if (ctx == NULL || header == NULL)
    return -1;
  if (!ipc_client_is_connected (ctx))
    return -1;

  // 发送头
  ssize_t sent = send (ctx->fd, header, sizeof (IPCMessageHeader), 0);
  if (sent != sizeof (IPCMessageHeader))
    {
      ctx->connected = false;
      return -1;
    }

  // 发送负载
  if (payload != NULL && header->payload_len > 0)
    {
      sent = send (ctx->fd, payload, header->payload_len, 0);
      if (sent != (ssize_t) header->payload_len)
	{
	  ctx->connected = false;
	  return -1;
	}
    }

  ctx->last_activity = get_timestamp_ms ();
  return 0;
}

// 接收响应
int
ipc_client_recv (IPCClientContext *ctx, IPCMessageHeader *header, void *payload,
		 size_t payload_size)
{
  if (ctx == NULL || header == NULL)
    return -1;
  if (!ipc_client_is_connected (ctx))
    return -1;

  // 接收头
  ssize_t received = recv (ctx->fd, header, sizeof (IPCMessageHeader), 0);
  if (received <= 0)
    {
      ctx->connected = false;
      return -1;
    }

  if (received != sizeof (IPCMessageHeader))
    {
      return -1;
    }

  // 验证头
  if (!ipc_validate_header (header))
    {
      return -1;
    }

  // 接收负载
  if (payload != NULL && header->payload_len > 0)
    {
      if (header->payload_len > payload_size)
	{
	  // 缓冲区太小
	  return -1;
	}

      received = recv (ctx->fd, payload, header->payload_len, 0);
      if (received != (ssize_t) header->payload_len)
	{
	  ctx->connected = false;
	  return -1;
	}
    }

  ctx->last_activity = get_timestamp_ms ();
  return 0;
}

// 同步发送请求并接收响应
int
ipc_client_send_sync (IPCClientContext *ctx,
		      const IPCMessageHeader *request_header,
		      const void *request_payload,
		      IPCMessageHeader *response_header, void *response_payload,
		      size_t response_size)
{
  if (ctx == NULL || request_header == NULL || response_header == NULL)
    return -1;

  // 发送请求
  if (ipc_client_send (ctx, request_header, request_payload) != 0)
    {
      return -1;
    }

  // 接收响应
  if (ipc_client_recv (ctx, response_header, response_payload, response_size)
      != 0)
    {
      return -1;
    }

  return 0;
}

// 快速获取音量（带缓存，非阻塞）
int
ipc_client_get_volume_fast (IPCClientContext *ctx, pid_t pid, float *volume,
			    bool *muted)
{
  if (ctx == NULL || volume == NULL || muted == NULL)
    return -1;

  // 检查缓存是否有效
  uint64_t now = get_timestamp_ms ();
  if (ctx->cache_valid && ctx->cached_pid == pid
      && (now - ctx->cache_timestamp) < IPC_CACHE_TTL_MS)
    {
      *volume = ctx->cached_volume;
      *muted = ctx->cached_muted;
      return 0;
    }

  // 缓存无效或过期，尝试从服务端获取
  if (!ipc_client_is_connected (ctx))
    {
      // 未连接，使用缓存值（即使过期）
      *volume = ctx->cached_volume;
      *muted = ctx->cached_muted;
      return -1;
    }

  // 构建请求
  IPCMessageHeader request;
  ipc_init_header (&request, kIPCCommandGetVolume, sizeof (pid_t), 1);

  // 发送请求并接收响应
  IPCMessageHeader response = {0};
  IPCVolumeResponse vol_response = {0};

  if (ipc_client_send_sync (ctx, &request, &pid, &response, &vol_response,
			    sizeof (vol_response))
      != 0)
    {
      // 请求失败，使用缓存值
      *volume = ctx->cached_volume;
      *muted = ctx->cached_muted;
      return -1;
    }

  // 检查响应状态
  if (response.command == kIPCCommandResponse
      && vol_response.status == kIPCStatusOK)
    {
      // 更新缓存
      ctx->cached_pid = pid;
      ctx->cached_volume = vol_response.volume;
      ctx->cached_muted = vol_response.muted;
      ctx->cache_timestamp = now;
      ctx->cache_valid = true;

      *volume = vol_response.volume;
      *muted = vol_response.muted;
      return 0;
    }

  // 服务端返回错误，使用缓存值
  *volume = ctx->cached_volume;
  *muted = ctx->cached_muted;
  return -1;
}

// 刷新缓存
int
ipc_client_refresh_cache (IPCClientContext *ctx, pid_t pid)
{
  if (ctx == NULL)
    return -1;

  if (!ipc_client_is_connected (ctx))
    {
      return -1;
    }

  IPCMessageHeader request;
  ipc_init_header (&request, kIPCCommandGetVolume, sizeof (pid_t), 1);

  IPCMessageHeader response = {0};
  IPCVolumeResponse vol_response = {0};

  if (ipc_client_send_sync (ctx, &request, &pid, &response, &vol_response,
			    sizeof (vol_response))
      != 0)
    {
      return -1;
    }

  if (response.command == kIPCCommandResponse
      && vol_response.status == kIPCStatusOK)
    {
      ctx->cached_pid = pid;
      ctx->cached_volume = vol_response.volume;
      ctx->cached_muted = vol_response.muted;
      ctx->cache_timestamp = get_timestamp_ms ();
      ctx->cache_valid = true;
      return 0;
    }

  return -1;
}

// 设置缓存值
void
ipc_client_set_cache (IPCClientContext *ctx, pid_t pid, float volume,
		      bool muted)
{
  if (ctx == NULL)
    return;

  ctx->cached_pid = pid;
  ctx->cached_volume = volume;
  ctx->cached_muted = muted;
  ctx->cache_timestamp = get_timestamp_ms ();
  ctx->cache_valid = true;
}

// 注册应用
int
ipc_client_register_app (IPCClientContext *ctx, pid_t pid, const char *app_name,
			 float initial_volume, bool muted)
{
  if (ctx == NULL || app_name == NULL)
    return -1;
  if (!ipc_client_is_connected (ctx))
    return -1;

  size_t name_len = strlen (app_name) + 1; // 包含 null 终止符
  size_t payload_len = sizeof (IPCRegisterRequest) + name_len;

  uint8_t *payload = malloc (payload_len);
  if (payload == NULL)
    return -1;

  IPCRegisterRequest *req = (IPCRegisterRequest *) payload;
  req->pid = pid;
  req->initial_volume = initial_volume;
  req->muted = muted;
  memcpy (payload + sizeof (IPCRegisterRequest), app_name, name_len);

  IPCMessageHeader request;
  ipc_init_header (&request, kIPCCommandRegister, (uint32_t) payload_len, 1);

  IPCMessageHeader response = {0};
  IPCResponse resp = {0};

  int result = ipc_client_send_sync (ctx, &request, payload, &response, &resp,
				     sizeof (resp));
  free (payload);

  if (result != 0)
    return -1;

  return (response.command == kIPCCommandResponse
	  && resp.status == kIPCStatusOK)
	   ? 0
	   : -1;
}

// 注销应用
int
ipc_client_unregister_app (IPCClientContext *ctx, pid_t pid)
{
  if (ctx == NULL)
    return -1;
  if (!ipc_client_is_connected (ctx))
    return -1;

  IPCMessageHeader request;
  ipc_init_header (&request, kIPCCommandUnregister, sizeof (pid_t), 1);

  IPCMessageHeader response = {0};
  IPCResponse resp = {0};

  if (ipc_client_send_sync (ctx, &request, &pid, &response, &resp,
			    sizeof (resp))
      != 0)
    {
      return -1;
    }

  return (response.command == kIPCCommandResponse
	  && resp.status == kIPCStatusOK)
	   ? 0
	   : -1;
}

// 获取应用音量
int
ipc_client_get_app_volume (IPCClientContext *ctx, pid_t pid, float *volume,
			   bool *muted)
{
  if (ctx == NULL || volume == NULL || muted == NULL)
    return -1;
  if (!ipc_client_is_connected (ctx))
    return -1;

  IPCMessageHeader request;
  ipc_init_header (&request, kIPCCommandGetVolume, sizeof (pid_t), 1);

  IPCMessageHeader response = {0};
  IPCVolumeResponse vol_response = {0};

  if (ipc_client_send_sync (ctx, &request, &pid, &response, &vol_response,
			    sizeof (vol_response))
      != 0)
    {
      return -1;
    }

  if (response.command == kIPCCommandResponse
      && vol_response.status == kIPCStatusOK)
    {
      *volume = vol_response.volume;
      *muted = vol_response.muted;
      return 0;
    }

  return -1;
}

// 设置应用音量
int
ipc_client_set_app_volume (IPCClientContext *ctx, pid_t pid, float volume)
{
  if (ctx == NULL)
    return -1;
  if (!ipc_client_is_connected (ctx))
    return -1;

  IPCSetVolumeRequest req;
  req.pid = pid;
  req.volume = volume;

  IPCMessageHeader request;
  ipc_init_header (&request, kIPCCommandSetVolume, sizeof (req), 1);

  IPCMessageHeader response = {0};
  IPCResponse resp = {0};

  if (ipc_client_send_sync (ctx, &request, &req, &response, &resp,
			    sizeof (resp))
      != 0)
    {
      return -1;
    }

  return (response.command == kIPCCommandResponse
	  && resp.status == kIPCStatusOK)
	   ? 0
	   : -1;
}

// 设置应用静音
int
ipc_client_set_app_mute (IPCClientContext *ctx, pid_t pid, bool muted)
{
  if (ctx == NULL)
    return -1;
  if (!ipc_client_is_connected (ctx))
    return -1;

  IPCSetMuteRequest req;
  req.pid = pid;
  req.muted = muted;

  IPCMessageHeader request;
  ipc_init_header (&request, kIPCCommandSetMute, sizeof (req), 1);

  IPCMessageHeader response = {0};
  IPCResponse resp = {0};

  if (ipc_client_send_sync (ctx, &request, &req, &response, &resp,
			    sizeof (resp))
      != 0)
    {
      return -1;
    }

  return (response.command == kIPCCommandResponse
	  && resp.status == kIPCStatusOK)
	   ? 0
	   : -1;
}

// Ping 服务端
int
ipc_client_ping (IPCClientContext *ctx)
{
  if (ctx == NULL)
    return -1;
  if (!ipc_client_is_connected (ctx))
    return -1;

  IPCMessageHeader request;
  ipc_init_header (&request, kIPCCommandPing, 0, 1);

  IPCMessageHeader response = {0};
  IPCResponse resp = {0};

  if (ipc_client_send_sync (ctx, &request, NULL, &response, &resp,
			    sizeof (resp))
      != 0)
    {
      return -1;
    }

  return (response.command == kIPCCommandResponse
	  && resp.status == kIPCStatusOK)
	   ? 0
	   : -1;
}

// 检查是否需要重连
bool
ipc_client_should_reconnect (IPCClientContext *ctx)
{
  if (ctx == NULL)
    return false;

  // 如果未连接且未达到最大重试次数
  if (!ipc_client_is_connected (ctx)
      && ctx->reconnect_attempts < IPC_RECONNECT_MAX_ATTEMPTS)
    {
      return true;
    }

  // 如果连接但长时间无活动（可选：心跳检测）
  uint64_t now = get_timestamp_ms ();
  if (ipc_client_is_connected (ctx) && (now - ctx->last_activity) > 30000
      && ipc_client_ping (ctx) != 0) // 30秒
    {
      ctx->connected = false;
      return ctx->reconnect_attempts < IPC_RECONNECT_MAX_ATTEMPTS;
    }

  return false;
}

// 自动重连
int
ipc_client_reconnect (IPCClientContext *ctx)
{
  if (ctx == NULL)
    return -1;

  if (ctx->reconnect_attempts >= IPC_RECONNECT_MAX_ATTEMPTS)
    {
      fprintf (stderr, "达到最大重连次数 (%d)\n", IPC_RECONNECT_MAX_ATTEMPTS);
      return -1;
    }

  // 指数退避
  int delay_ms
    = IPC_RECONNECT_BASE_DELAY_MS * (int) pow (2, ctx->reconnect_attempts);
  if (delay_ms > 5000)
    delay_ms = 5000; // 最大 5 秒

  struct timespec ts = {delay_ms / 1000, (delay_ms % 1000) * 1000000};
  nanosleep (&ts, NULL);

  ctx->reconnect_attempts++;

  // 尝试连接
  if (ipc_client_connect (ctx) == 0)
    {
      ctx->reconnect_attempts = 0;
      return 0;
    }

  return -1;
}

// 重置重连计数器
void
ipc_client_reset_reconnect (IPCClientContext *ctx)
{
  if (ctx == NULL)
    return;
  ctx->reconnect_attempts = 0;
}

// 获取应用列表
int
ipc_client_list_apps (IPCClientContext *ctx, IPCAppInfo **apps, uint32_t *count)
{
  if (ctx == NULL || apps == NULL || count == NULL)
    return -1;
  if (!ipc_client_is_connected (ctx))
    return -1;

  IPCMessageHeader request;
  ipc_init_header (&request, kIPCCommandListClients, 0, 1);

  IPCMessageHeader response = {0};
  // 分配缓冲区接收列表数据
  uint8_t *buffer = malloc (IPC_MAX_PAYLOAD_SIZE);
  if (buffer == NULL)
    return -1;

  if (ipc_client_send_sync (ctx, &request, NULL, &response, buffer,
			    IPC_MAX_PAYLOAD_SIZE)
      != 0)
    {
      free (buffer);
      return -1;
    }

  if (response.command != kIPCCommandResponse || response.payload_len == 0)
    {
      free (buffer);
      *apps = NULL;
      *count = 0;
      return (response.command == kIPCCommandResponse) ? 0 : -1;
    }

  // 计算客户端数量
  size_t entry_size
    = sizeof (pid_t) + sizeof (float) + sizeof (bool) + sizeof (uint64_t) + 256;
  uint32_t app_count = response.payload_len / (uint32_t) entry_size;

  if (app_count == 0)
    {
      free (buffer);
      *apps = NULL;
      *count = 0;
      return 0;
    }

  // 分配应用信息数组
  IPCAppInfo *app_list = malloc (sizeof (IPCAppInfo) * app_count);
  if (app_list == NULL)
    {
      free (buffer);
      return -1;
    }

  // 解析响应数据
  uint8_t const *ptr = buffer;
  for (uint32_t i = 0; i < app_count; i++)
    {
      // 读取 PID
      memcpy (&app_list[i].pid, ptr, sizeof (pid_t));
      ptr += sizeof (pid_t);

      // 读取音量
      memcpy (&app_list[i].volume, ptr, sizeof (float));
      ptr += sizeof (float);

      // 读取静音状态
      memcpy (&app_list[i].muted, ptr, sizeof (bool));
      ptr += sizeof (bool);

      // 读取连接时间
      memcpy (&app_list[i].connected_at, ptr, sizeof (uint64_t));
      ptr += sizeof (uint64_t);

      // 读取应用名称
      memcpy (app_list[i].app_name, ptr, 256);
      ptr += 256;
    }

  free (buffer);

  *apps = app_list;
  *count = app_count;

  return 0;
}
