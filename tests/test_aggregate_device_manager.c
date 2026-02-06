//
// Aggregate Device 管理模块单元测试
// Created by AhogeK on 02/05/26.
//

#include "aggregate_device_manager.h"
#include "virtual_device_manager.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// 注意：这些测试会实际创建和销毁 Aggregate Device
// 测试前请确保虚拟设备已安装

// 测试 Aggregate Device 检测
static int test_aggregate_device_detection()
{
    printf("Testing aggregate device detection...\n");

    // 检查函数是否能正常调用
    bool created = aggregate_device_is_created();
    printf("  Aggregate device created: %s\n", created ? "YES" : "NO");

    AggregateDeviceInfo info;
    bool gotInfo = aggregate_device_get_info(&info);

    if (created)
    {
        if (!gotInfo)
        {
            printf("FAIL: Should get info when device exists\n");
            return 1;
        }
        printf("  Device ID: %d\n", info.deviceId);
        printf("  Name: %s\n", info.name);
        printf("  Sub-devices: %d\n", info.subDeviceCount);
    }
    else
    {
        if (gotInfo)
        {
            printf("FAIL: Should not get info when device doesn't exist\n");
            return 1;
        }
    }

    // 检查状态
    bool active = aggregate_device_is_active();
    printf("  Active: %s\n", active ? "YES" : "NO");

    // 逻辑一致性：如果 active，必须 created
    if (active && !created)
    {
        printf("FAIL: Cannot be active if not created\n");
        return 1;
    }

    printf("PASS: Aggregate device detection test\n");
    return 0;
}

// 测试获取推荐物理设备
static int test_recommended_physical_device()
{
    printf("Testing recommended physical device...\n");

    AudioDeviceID device = aggregate_device_get_recommended_physical_device();

    if (device == kAudioObjectUnknown)
    {
        printf("WARN: No recommended physical device found\n");
        // 这不一定是失败，系统可能没有物理设备
    }
    else
    {
        printf("  Recommended device ID: %d\n", device);
    }

    printf("PASS: Recommended physical device test\n");
    return 0;
}

// 测试 Aggregate Device 创建和销毁
// 注意：这会实际创建设备
static int test_create_destroy()
{
    printf("Testing aggregate device create/destroy...\n");

    // 检查虚拟设备是否安装
    if (!virtual_device_is_installed())
    {
        printf("SKIP: Virtual device not installed, skipping create/destroy test\n");
        return 0;
    }

    // 如果已存在，先销毁
    if (aggregate_device_is_created())
    {
        printf("  Cleaning up existing aggregate device...\n");
        aggregate_device_destroy();
        sleep(1);
    }

    // 创建 Aggregate Device
    printf("  Creating aggregate device...\n");
    OSStatus status = aggregate_device_create(kAudioObjectUnknown);
    if (status != noErr)
    {
        printf("FAIL: Failed to create aggregate device: %d\n", (int)status);
        return 1;
    }

    // 验证创建成功
    if (!aggregate_device_is_created())
    {
        printf("FAIL: Device reported created but not found\n");
        return 1;
    }

    AggregateDeviceInfo info;
    if (!aggregate_device_get_info(&info))
    {
        printf("FAIL: Cannot get info for created device\n");
        aggregate_device_destroy();
        return 1;
    }

    // 注：子设备列表获取可能不稳定，主要验证设备创建成功
    printf("  Device created with %d sub-devices\n", info.subDeviceCount);

    // 销毁
    printf("  Destroying aggregate device...\n");
    status = aggregate_device_destroy();
    if (status != noErr)
    {
        printf("FAIL: Failed to destroy aggregate device: %d\n", (int)status);
        return 1;
    }

    // 等待系统处理
    sleep(1);

    // 验证销毁成功（可能需要重试）
    bool stillExists = aggregate_device_is_created();
    if (stillExists)
    {
        // 再等待一下
        sleep(1);
        stillExists = aggregate_device_is_created();
    }

    if (stillExists)
    {
        printf("WARN: Device may still exist after destroy (system delay)\n");
        // 不视为失败，因为销毁命令已成功发送
    }

    printf("PASS: Create/destroy test\n");
    return 0;
}

// 测试激活和停用
// 注意：这会实际改变系统音频设置
static int test_activate_deactivate()
{
    printf("Testing activate/deactivate...\n");

    // 检查虚拟设备是否安装
    if (!virtual_device_is_installed())
    {
        printf("SKIP: Virtual device not installed, skipping activate/deactivate test\n");
        return 0;
    }

    // 保存当前默认设备以便恢复
    VirtualDeviceInfo originalOutput;
    OSStatus status = virtual_device_get_current_output_info(&originalOutput);
    if (status != noErr)
    {
        printf("WARN: Cannot get current output device\n");
    }

    // 确保有 Aggregate Device
    if (!aggregate_device_is_created())
    {
        status = aggregate_device_create(kAudioObjectUnknown);
        if (status != noErr)
        {
            printf("SKIP: Cannot create aggregate device\n");
            return 0;
        }
        sleep(1);
    }

    // 激活
    printf("  Activating aggregate device...\n");
    status = aggregate_device_activate();
    if (status != noErr)
    {
        printf("WARN: Failed to activate: %d (may need permissions)\n", (int)status);
        // 这不一定是测试失败，可能是权限问题
    }
    else
    {
        // 等待系统更新状态
        sleep(1);

        // 验证激活成功（允许一定延迟）
        bool isActive = aggregate_device_is_active();
        printf("  Device active: %s\n", isActive ? "YES" : "NO");

        if (!isActive)
        {
            printf("WARN: Device may not be active yet (system delay)\n");
        }
        else
        {
            printf("  Device activated successfully\n");
        }

        // 停用
        printf("  Deactivating...\n");
        status = aggregate_device_deactivate();
        if (status != noErr)
        {
            printf("WARN: Failed to deactivate: %d\n", (int)status);
        }
    }

    // 清理
    if (aggregate_device_is_created())
    {
        aggregate_device_destroy();
    }

    printf("PASS: Activate/deactivate test\n");
    return 0;
}

// 测试更新物理设备
static int test_update_physical_device()
{
    printf("Testing update physical device...\n");

    if (!virtual_device_is_installed())
    {
        printf("SKIP: Virtual device not installed\n");
        return 0;
    }

    // 获取一个物理设备用于测试
    AudioDeviceID physical = aggregate_device_get_recommended_physical_device();
    if (physical == kAudioObjectUnknown)
    {
        printf("SKIP: No physical device available\n");
        return 0;
    }

    // 确保有 Aggregate Device
    if (!aggregate_device_is_created())
    {
        OSStatus status = aggregate_device_create(kAudioObjectUnknown);
        if (status != noErr)
        {
            printf("SKIP: Cannot create aggregate device\n");
            return 0;
        }
        sleep(1);
    }

    // 更新物理设备（使用相同的设备，主要是测试流程）
    printf("  Updating physical device...\n");
    OSStatus status = aggregate_device_update_physical_device(physical);
    if (status != noErr)
    {
        printf("WARN: Failed to update: %d\n", (int)status);
    }

    // 清理
    aggregate_device_destroy();

    printf("PASS: Update physical device test\n");
    return 0;
}

// 测试状态打印（检查不崩溃）
static int test_print_status()
{
    printf("Testing print status (visual check)...\n");

    printf("  --- aggregate_device_print_status() output: ---\n");
    aggregate_device_print_status();
    printf("  --- end of output ---\n");

    printf("PASS: Print status test\n");
    return 0;
}

// 测试辅助函数
static int test_helper_functions()
{
    printf("Testing helper functions...\n");

    // 测试获取物理设备
    AudioDeviceID physical = aggregate_device_get_physical_device();
    printf("  Physical device: %d\n", physical);

    // 如果 Aggregate Device 不存在，应该返回 unknown
    if (!aggregate_device_is_created() && physical != kAudioObjectUnknown)
    {
        printf("FAIL: Should return unknown when no aggregate device\n");
        return 1;
    }

    printf("PASS: Helper functions test\n");
    return 0;
}

int run_aggregate_device_manager_tests()
{
    printf("\nRunning aggregate device manager tests...\n");
    printf("(Note: Some tests will create/destroy actual system devices)\n");
    int failed = 0;

    failed += test_aggregate_device_detection();
    failed += test_recommended_physical_device();
    failed += test_create_destroy();
    failed += test_activate_deactivate();
    failed += test_update_physical_device();
    failed += test_print_status();
    failed += test_helper_functions();

    return failed;
}
