//
// Created by AhogeK on 02/12/26.
//

#include "ipc/ipc_protocol.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int
test_ipc_header_init (void)
{
  printf ("  Testing ipc_init_header...\n");

  IPCMessageHeader header;
  ipc_init_header (&header, kIPCCommandRegister, 256, 42);

  // 验证魔数
  if (header.magic != IPC_MAGIC)
    {
      printf (
	"    ❌ FAIL: Magic number mismatch (expected 0x%08X, got 0x%08X)\n",
	IPC_MAGIC, header.magic);
      return 1;
    }

  // 验证版本
  if (header.version != IPC_PROTOCOL_VERSION)
    {
      printf ("    ❌ FAIL: Version mismatch (expected %d, got %d)\n",
	      IPC_PROTOCOL_VERSION, header.version);
      return 1;
    }

  // 验证指令
  if (header.command != kIPCCommandRegister)
    {
      printf ("    ❌ FAIL: Command mismatch (expected %d, got %d)\n",
	      kIPCCommandRegister, header.command);
      return 1;
    }

  // 验证负载长度
  if (header.payload_len != 256)
    {
      printf ("    ❌ FAIL: Payload length mismatch (expected 256, got %d)\n",
	      header.payload_len);
      return 1;
    }

  // 验证请求ID
  if (header.request_id != 42)
    {
      printf ("    ❌ FAIL: Request ID mismatch (expected 42, got %d)\n",
	      header.request_id);
      return 1;
    }

  printf ("    ✅ PASS: Header initialization correct\n");
  return 0;
}

static int
test_ipc_header_validation (void)
{
  printf ("  Testing ipc_validate_header...\n");

  int failed = 0;

  // 测试有效消息头
  IPCMessageHeader valid_header;
  ipc_init_header (&valid_header, kIPCCommandPing, 0, 1);
  if (!ipc_validate_header (&valid_header))
    {
      printf ("    ❌ FAIL: Valid header rejected\n");
      failed++;
    }
  else
    {
      printf ("    ✅ PASS: Valid header accepted\n");
    }

  // 测试无效魔数
  IPCMessageHeader bad_magic = valid_header;
  bad_magic.magic = 0xDEADBEEF;
  if (ipc_validate_header (&bad_magic))
    {
      printf ("    ❌ FAIL: Invalid magic accepted\n");
      failed++;
    }
  else
    {
      printf ("    ✅ PASS: Invalid magic rejected\n");
    }

  // 测试无效版本
  IPCMessageHeader bad_version = valid_header;
  bad_version.version = 999;
  if (ipc_validate_header (&bad_version))
    {
      printf ("    ❌ FAIL: Invalid version accepted\n");
      failed++;
    }
  else
    {
      printf ("    ✅ PASS: Invalid version rejected\n");
    }

  // 测试超大负载
  IPCMessageHeader big_payload = valid_header;
  big_payload.payload_len = IPC_MAX_PAYLOAD_SIZE + 1;
  if (ipc_validate_header (&big_payload))
    {
      printf ("    ❌ FAIL: Oversized payload accepted\n");
      failed++;
    }
  else
    {
      printf ("    ✅ PASS: Oversized payload rejected\n");
    }

  // 测试无效指令
  IPCMessageHeader bad_cmd = valid_header;
  bad_cmd.command = 0x9999;
  if (ipc_validate_header (&bad_cmd))
    {
      printf ("    ❌ FAIL: Invalid command accepted\n");
      failed++;
    }
  else
    {
      printf ("    ✅ PASS: Invalid command rejected\n");
    }

  // 测试 NULL 指针
  if (ipc_validate_header (NULL))
    {
      printf ("    ❌ FAIL: NULL header accepted\n");
      failed++;
    }
  else
    {
      printf ("    ✅ PASS: NULL header rejected\n");
    }

  return failed;
}

static int
test_ipc_socket_path (void)
{
  printf ("  Testing get_ipc_socket_path...\n");

  char path[1024];
  int result = get_ipc_socket_path (path, sizeof (path));

  if (result != 0)
    {
      printf ("    ❌ FAIL: Failed to get socket path\n");
      return 1;
    }

  // 验证路径包含预期的组件
  if (strstr (path, "Application Support/audioctl/daemon.sock") == NULL)
    {
      printf ("    ❌ FAIL: Path doesn't contain expected components: %s\n",
	      path);
      return 1;
    }

  printf ("    ✅ PASS: Socket path correct: %s\n", path);
  return 0;
}

static int
test_ipc_status_strings (void)
{
  printf ("  Testing ipc_status_to_string...\n");

  int failed = 0;

  // 测试已知状态码
  const char *ok_str = ipc_status_to_string (kIPCStatusOK);
  if (strcmp (ok_str, "OK") != 0)
    {
      printf ("    ❌ FAIL: kIPCStatusOK string mismatch\n");
      failed++;
    }
  else
    {
      printf ("    ✅ PASS: kIPCStatusOK -> 'OK'\n");
    }

  const char *not_found_str = ipc_status_to_string (kIPCStatusClientNotFound);
  if (strcmp (not_found_str, "Client not found") != 0)
    {
      printf ("    ❌ FAIL: kIPCStatusClientNotFound string mismatch\n");
      failed++;
    }
  else
    {
      printf ("    ✅ PASS: kIPCStatusClientNotFound -> 'Client not found'\n");
    }

  // 测试未知状态码
  const char *unknown_str = ipc_status_to_string (12345);
  if (strcmp (unknown_str, "Unknown status") != 0)
    {
      printf ("    ❌ FAIL: Unknown status string mismatch\n");
      failed++;
    }
  else
    {
      printf ("    ✅ PASS: Unknown status -> 'Unknown status'\n");
    }

  return failed;
}

static int
test_ipc_struct_sizes (void)
{
  printf ("  Testing IPC struct sizes...\n");

  int failed = 0;

  // 验证消息头大小（必须是 16 字节）
  if (sizeof (IPCMessageHeader) != 16)
    {
      printf ("    ❌ FAIL: IPCMessageHeader size is %zu, expected 16\n",
	      sizeof (IPCMessageHeader));
      failed++;
    }
  else
    {
      printf ("    ✅ PASS: IPCMessageHeader size = 16 bytes\n");
    }

  // 验证其他关键结构体大小
  printf ("    ℹ️  IPCRegisterRequest size = %zu bytes\n",
	  sizeof (IPCRegisterRequest));
  printf ("    ℹ️  IPCSetVolumeRequest size = %zu bytes\n",
	  sizeof (IPCSetVolumeRequest));
  printf ("    ℹ️  IPCVolumeResponse size = %zu bytes\n",
	  sizeof (IPCVolumeResponse));

  return failed;
}

int
run_ipc_protocol_tests (void)
{
  printf ("\n----------------------------------------\n");
  printf ("IPC Protocol Tests\n");
  printf ("----------------------------------------\n");

  int failed = 0;
  failed += test_ipc_header_init ();
  failed += test_ipc_header_validation ();
  failed += test_ipc_socket_path ();
  failed += test_ipc_status_strings ();
  failed += test_ipc_struct_sizes ();

  printf ("----------------------------------------\n");
  if (failed == 0)
    {
      printf ("IPC Protocol Tests: PASSED ✅\n");
    }
  else
    {
      printf ("IPC Protocol Tests: %d FAILED ❌\n", failed);
    }
  printf ("----------------------------------------\n");

  return failed;
}