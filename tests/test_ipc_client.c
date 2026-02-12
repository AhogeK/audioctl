//
// Created by AhogeK on 02/12/26.
//

#include "ipc/ipc_client.h"
#include <stdio.h>
#include <unistd.h>

static int
test_ipc_client_init (void)
{
  printf ("  Testing ipc_client_init...\n");

  IPCClientContext ctx;
  int result = ipc_client_init (&ctx);

  if (result != 0)
    {
      printf ("    ❌ FAIL: ipc_client_init returned %d\n", result);
      return 1;
    }

  if (ctx.fd != -1)
    {
      printf ("    ❌ FAIL: Initial fd should be -1\n");
      return 1;
    }

  if (ctx.connected)
    {
      printf ("    ❌ FAIL: Should not be connected initially\n");
      return 1;
    }

  if (ctx.cached_volume != 1.0f)
    {
      printf ("    ❌ FAIL: Default volume should be 1.0, got %f\n",
	      ctx.cached_volume);
      return 1;
    }

  printf ("    ✅ PASS: Client initialization correct\n");
  return 0;
}

static int
test_ipc_client_cache (void)
{
  printf ("  Testing ipc_client cache operations...\n");

  IPCClientContext ctx;
  ipc_client_init (&ctx);

  // 测试设置缓存
  ipc_client_set_cache (&ctx, 1234, 0.5f, true);

  if (ctx.cached_pid != 1234)
    {
      printf ("    ❌ FAIL: Cached PID mismatch\n");
      return 1;
    }

  if (ctx.cached_volume != 0.5f)
    {
      printf ("    ❌ FAIL: Cached volume mismatch\n");
      return 1;
    }

  if (!ctx.cached_muted)
    {
      printf ("    ❌ FAIL: Cached mute state mismatch\n");
      return 1;
    }

  if (!ctx.cache_valid)
    {
      printf ("    ❌ FAIL: Cache should be valid\n");
      return 1;
    }

  // 测试快速获取（应该返回缓存值）
  float volume;
  bool muted;
  (void) ipc_client_get_volume_fast (&ctx, 1234, &volume, &muted);

  // 未连接时应该返回 -1 但使用缓存值
  if (volume != 0.5f || muted != true)
    {
      printf ("    ❌ FAIL: Fast get should return cached values\n");
      return 1;
    }

  printf ("    ✅ PASS: Cache operations correct\n");
  return 0;
}

static int
test_ipc_client_reconnect (void)
{
  printf ("  Testing ipc_client reconnect logic...\n");

  IPCClientContext ctx;
  ipc_client_init (&ctx);

  // 初始状态应该需要重连（未连接且尝试次数为 0）
  if (!ipc_client_should_reconnect (&ctx))
    {
      printf ("    ❌ FAIL: Should need reconnect initially\n");
      return 1;
    }

  // 模拟多次重连失败
  for (int i = 0; i < 6; i++)
    {
      ctx.reconnect_attempts = i;
      bool should = ipc_client_should_reconnect (&ctx);

      if (i < 5 && !should)
	{
	  printf ("    ❌ FAIL: Should reconnect when attempts=%d\n", i);
	  return 1;
	}

      if (i >= 5 && should)
	{
	  printf ("    ❌ FAIL: Should not reconnect when attempts=%d (max "
		  "reached)\n",
		  i);
	  return 1;
	}
    }

  // 测试重置
  ctx.reconnect_attempts = 5;
  ipc_client_reset_reconnect (&ctx);

  if (ctx.reconnect_attempts != 0)
    {
      printf ("    ❌ FAIL: Reset should clear attempts\n");
      return 1;
    }

  printf ("    ✅ PASS: Reconnect logic correct\n");
  return 0;
}

// 在子进程中启动服务端进行集成测试
static int
test_ipc_client_integration (void)
{
  printf ("  Testing ipc_client integration (requires server)...\n");

  // 注意：这需要 IPC 服务正在运行
  // 如果服务未运行，这些测试会跳过

  IPCClientContext ctx;
  ipc_client_init (&ctx);

  // 尝试连接
  int result = ipc_client_connect (&ctx);

  if (result != 0)
    {
      printf ("    ℹ️  SKIP: IPC service not running (expected in unit test)\n");
      return 0; // 这不是失败，只是服务没启动
    }

  printf ("    ✅ Connected to IPC service\n");

  // 测试 Ping
  result = ipc_client_ping (&ctx);
  if (result != 0)
    {
      printf ("    ❌ FAIL: Ping failed\n");
      ipc_client_disconnect (&ctx);
      return 1;
    }
  printf ("    ✅ Ping successful\n");

  // 测试注册
  result = ipc_client_register_app (&ctx, getpid (), "TestApp", 0.8f, false);
  if (result != 0)
    {
      printf ("    ❌ FAIL: Register app failed\n");
      ipc_client_disconnect (&ctx);
      return 1;
    }
  printf ("    ✅ Register app successful\n");

  // 测试获取音量
  float volume;
  bool muted;
  result = ipc_client_get_app_volume (&ctx, getpid (), &volume, &muted);
  if (result != 0)
    {
      printf ("    ❌ FAIL: Get volume failed\n");
      ipc_client_disconnect (&ctx);
      return 1;
    }
  if (volume != 0.8f || muted != false)
    {
      printf ("    ❌ FAIL: Volume/mute mismatch (got vol=%f, muted=%d)\n",
	      volume, muted);
      ipc_client_disconnect (&ctx);
      return 1;
    }
  printf ("    ✅ Get volume correct (vol=%.1f, muted=%d)\n", volume, muted);

  // 测试设置音量
  result = ipc_client_set_app_volume (&ctx, getpid (), 0.5f);
  if (result != 0)
    {
      printf ("    ❌ FAIL: Set volume failed\n");
      ipc_client_disconnect (&ctx);
      return 1;
    }

  // 验证设置成功
  result = ipc_client_get_app_volume (&ctx, getpid (), &volume, &muted);
  if (result != 0 || volume != 0.5f)
    {
      printf ("    ❌ FAIL: Volume not updated correctly\n");
      ipc_client_disconnect (&ctx);
      return 1;
    }
  printf ("    ✅ Set volume successful\n");

  // 测试注销
  result = ipc_client_unregister_app (&ctx, getpid ());
  if (result != 0)
    {
      printf ("    ❌ FAIL: Unregister app failed\n");
      ipc_client_disconnect (&ctx);
      return 1;
    }
  printf ("    ✅ Unregister app successful\n");

  ipc_client_disconnect (&ctx);
  printf ("    ✅ Integration tests passed\n");
  return 0;
}

int
run_ipc_client_tests (void)
{
  printf ("\n----------------------------------------\n");
  printf ("IPC Client Tests\n");
  printf ("----------------------------------------\n");

  int failed = 0;
  failed += test_ipc_client_init ();
  failed += test_ipc_client_cache ();
  failed += test_ipc_client_reconnect ();
  failed += test_ipc_client_integration ();

  printf ("----------------------------------------\n");
  if (failed == 0)
    {
      printf ("IPC Client Tests: PASSED ✅\n");
    }
  else
    {
      printf ("IPC Client Tests: %d FAILED ❌\n", failed);
    }
  printf ("----------------------------------------\n");

  return failed;
}
