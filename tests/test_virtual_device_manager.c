//
// 虚拟设备管理模块单元测试
// Created by AhogeK on 02/05/26.
//

#include "virtual_device_manager.h"
#include <stdio.h>
#include <string.h>

// 注意：这些测试依赖实际的 CoreAudio 系统状态
// 如果虚拟设备未安装，部分测试会被跳过

// 测试虚拟设备检测
static int test_virtual_device_detection()
{
    printf("Testing virtual device detection...\n");

    // 这个测试只是检查函数是否可以正常调用
    // 不假设虚拟设备是否安装
    bool installed = virtual_device_is_installed();
    printf("  Virtual device installed: %s\n", installed ? "YES" : "NO");

    VirtualDeviceInfo info;
    bool gotInfo = virtual_device_get_info(&info);

    if (installed)
    {
        if (!gotInfo)
        {
            printf("FAIL: Should get info when device is installed\n");
            return 1;
        }
        printf("  Device ID: %d\n", info.deviceId);
        printf("  Name: %s\n", info.name);
        printf("  Active: %s\n", info.isActive ? "YES" : "NO");
    }
    else
    {
        if (gotInfo)
        {
            printf("FAIL: Should not get info when device is not installed\n");
            return 1;
        }
    }

    printf("PASS: Virtual device detection test\n");
    return 0;
}

// 测试虚拟设备状态检查
static int test_virtual_device_status()
{
    printf("Testing virtual device status checks...\n");

    // 测试函数是否能正常调用
    bool activeOutput = virtual_device_is_active_output();
    bool activeInput = virtual_device_is_active_input();
    bool active = virtual_device_is_active();

    printf("  Active as output: %s\n", activeOutput ? "YES" : "NO");
    printf("  Active as input: %s\n", activeInput ? "YES" : "NO");
    printf("  Active (any): %s\n", active ? "YES" : "NO");

    // 逻辑一致性检查
    if (active != (activeOutput || activeInput))
    {
        printf("FAIL: Active state inconsistency\n");
        return 1;
    }

    printf("PASS: Virtual device status test\n");
    return 0;
}

// 测试应用音量控制可用性检查
static int test_app_volume_availability()
{
    printf("Testing app volume availability...\n");

    bool canControl = virtual_device_can_control_app_volume();
    const char* status = virtual_device_get_app_volume_status();

    printf("  Can control app volume: %s\n", canControl ? "YES" : "NO");
    printf("  Status: %s\n", status);

    // 如果虚拟设备未安装或未激活，应该不能控制
    if (!virtual_device_is_installed())
    {
        if (canControl)
        {
            printf("FAIL: Should not be able to control when device not installed\n");
            return 1;
        }
    }

    if (!virtual_device_is_active())
    {
        if (canControl)
        {
            printf("FAIL: Should not be able to control when device not active\n");
            return 1;
        }
    }

    printf("PASS: App volume availability test\n");
    return 0;
}

// 测试虚拟设备状态打印（主要是检查不崩溃）
static int test_print_status()
{
    printf("Testing print status (visual check)...\n");

    printf("  --- virtual_device_print_status() output: ---\n");
    virtual_device_print_status();
    printf("  --- end of output ---\n");

    printf("PASS: Print status test\n");
    return 0;
}

// 测试当前输出设备信息获取
static int test_current_output_info()
{
    printf("Testing current output info...\n");

    VirtualDeviceInfo info;
    OSStatus status = virtual_device_get_current_output_info(&info);

    if (status != noErr)
    {
        printf("FAIL: Failed to get current output info\n");
        return 1;
    }

    printf("  Current output device ID: %d\n", info.deviceId);
    printf("  Name: %s\n", info.name);
    printf("  Is virtual: %s\n", info.isInstalled ? "YES" : "NO");

    printf("PASS: Current output info test\n");
    return 0;
}

int run_virtual_device_manager_tests()
{
    printf("\nRunning virtual device manager tests...\n");
    printf("(Note: Some tests depend on actual system state)\n");
    int failed = 0;

    failed += test_virtual_device_detection();
    failed += test_virtual_device_status();
    failed += test_app_volume_availability();
    failed += test_print_status();
    failed += test_current_output_info();

    return failed;
}
