//
// Created by AhogeK on 12/10/24.
//

#include "driver/virtual_audio_device.h"
#include <stdio.h>

// 测试状态查询
static int
test_state_query ()
{
  printf ("Testing device state query...\n");

  VirtualAudioDevice *device = NULL;
  OSStatus status = virtual_device_create (&device);

  if (status != kAudioHardwareNoError || device == NULL)
    {
      printf ("FAIL: Device creation failed\n");
      return 1;
    }

  // 测试空指针参数
  DeviceState outState;
  status = virtual_device_get_state (NULL, &outState);
  if (status != kAudioHardwareIllegalOperationError)
    {
      printf ("FAIL: Get state should fail with NULL device\n");
      virtual_device_destroy (device);
      return 1;
    }

  status = virtual_device_get_state (device, NULL);
  if (status != kAudioHardwareIllegalOperationError)
    {
      printf ("FAIL: Get state should fail with NULL outState\n");
      virtual_device_destroy (device);
      return 1;
    }

  // 测试初始状态
  status = virtual_device_get_state (device, &outState);
  if (status != kAudioHardwareNoError)
    {
      printf ("FAIL: Get state failed\n");
      virtual_device_destroy (device);
      return 1;
    }

  if (outState != DEVICE_STATE_STOPPED)
    {
      printf ("FAIL: Initial state should be STOPPED\n");
      virtual_device_destroy (device);
      return 1;
    }

  // 启动设备并测试状态变化
  status = virtual_device_start (device);
  if (status != kAudioHardwareNoError)
    {
      printf ("FAIL: Device start failed\n");
      virtual_device_destroy (device);
      return 1;
    }

  status = virtual_device_get_state (device, &outState);
  if (status != kAudioHardwareNoError || outState != DEVICE_STATE_RUNNING)
    {
      printf ("FAIL: State should be RUNNING after start\n");
      virtual_device_destroy (device);
      return 1;
    }

  // 停止设备并测试状态变化
  status = virtual_device_stop (device);
  if (status != kAudioHardwareNoError)
    {
      printf ("FAIL: Device stop failed\n");
      virtual_device_destroy (device);
      return 1;
    }

  status = virtual_device_get_state (device, &outState);
  if (status != kAudioHardwareNoError || outState != DEVICE_STATE_STOPPED)
    {
      printf ("FAIL: State should be STOPPED after stop\n");
      virtual_device_destroy (device);
      return 1;
    }

  virtual_device_destroy (device);
  printf ("PASS: Device state query test\n");
  return 0;
}

// 测试运行状态查询
static int
test_running_state_query ()
{
  printf ("Testing device running state query...\n");

  VirtualAudioDevice *device = NULL;
  OSStatus status = virtual_device_create (&device);

  if (status != kAudioHardwareNoError || device == NULL)
    {
      printf ("FAIL: Device creation failed\n");
      return 1;
    }

  // 测试空指针参数
  Boolean outIsRunning;
  status = virtual_device_is_running (NULL, &outIsRunning);
  if (status != kAudioHardwareIllegalOperationError)
    {
      printf ("FAIL: Is running should fail with NULL device\n");
      virtual_device_destroy (device);
      return 1;
    }

  status = virtual_device_is_running (device, NULL);
  if (status != kAudioHardwareIllegalOperationError)
    {
      printf ("FAIL: Is running should fail with NULL outIsRunning\n");
      virtual_device_destroy (device);
      return 1;
    }

  // 测试初始运行状态
  status = virtual_device_is_running (device, &outIsRunning);
  if (status != kAudioHardwareNoError)
    {
      printf ("FAIL: Is running query failed\n");
      virtual_device_destroy (device);
      return 1;
    }

  if (outIsRunning != false)
    {
      printf ("FAIL: Device should not be running initially\n");
      virtual_device_destroy (device);
      return 1;
    }

  // 启动设备并测试运行状态
  status = virtual_device_start (device);
  if (status != kAudioHardwareNoError)
    {
      printf ("FAIL: Device start failed\n");
      virtual_device_destroy (device);
      return 1;
    }

  status = virtual_device_is_running (device, &outIsRunning);
  if (status != kAudioHardwareNoError || outIsRunning != true)
    {
      printf ("FAIL: Device should be running after start\n");
      virtual_device_destroy (device);
      return 1;
    }

  // 停止设备并测试运行状态
  status = virtual_device_stop (device);
  if (status != kAudioHardwareNoError)
    {
      printf ("FAIL: Device stop failed\n");
      virtual_device_destroy (device);
      return 1;
    }

  status = virtual_device_is_running (device, &outIsRunning);
  if (status != kAudioHardwareNoError || outIsRunning != false)
    {
      printf ("FAIL: Device should not be running after stop\n");
      virtual_device_destroy (device);
      return 1;
    }

  virtual_device_destroy (device);
  printf ("PASS: Device running state query test\n");
  return 0;
}

int
run_device_state_tests ()
{
  printf ("\nRunning device state tests...\n");
  int failed = 0;

  failed += test_state_query ();
  failed += test_running_state_query ();

  return failed;
}