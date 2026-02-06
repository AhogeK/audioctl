//
// 应用音量控制模块单元测试
// Created by AhogeK on 02/05/26.
//

#include "app_volume_control.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

// 测试初始化和清理
static int test_init_cleanup()
{
    printf("Testing app_volume_control init and cleanup...\n");

    OSStatus status = app_volume_control_init();
    if (status != noErr)
    {
        printf("FAIL: Initialization failed with status %d\n", (int)status);
        return 1;
    }

    // 重复初始化应该成功
    status = app_volume_control_init();
    if (status != noErr)
    {
        printf("FAIL: Repeated initialization failed\n");
        return 1;
    }

    app_volume_control_cleanup();

    // 重复清理不应该崩溃
    app_volume_control_cleanup();

    printf("PASS: Init and cleanup test\n");
    return 0;
}

// 测试应用注册
static int test_app_registration()
{
    printf("Testing app registration...\n");

    app_volume_control_init();

    // 注册一个应用
    OSStatus status = app_volume_register(1234, "com.test.app1", "TestApp1");
    if (status != noErr)
    {
        printf("FAIL: App registration failed\n");
        app_volume_control_cleanup();
        return 1;
    }

    // 重复注册同一个PID应该更新信息
    status = app_volume_register(1234, "com.test.app1.updated", "TestApp1Updated");
    if (status != noErr)
    {
        printf("FAIL: App re-registration failed\n");
        app_volume_control_cleanup();
        return 1;
    }

    // 注册多个应用
    for (int i = 0; i < 5; i++)
    {
        pid_t pid = 2000 + i;
        char bundleId[256];
        char name[256];
        snprintf(bundleId, sizeof(bundleId), "com.test.app%d", i);
        snprintf(name, sizeof(name), "TestApp%d", i);

        status = app_volume_register(pid, bundleId, name);
        if (status != noErr)
        {
            printf("FAIL: Failed to register app %d\n", i);
            app_volume_control_cleanup();
            return 1;
        }
    }

    app_volume_control_cleanup();
    printf("PASS: App registration test\n");
    return 0;
}

// 测试应用注销
static int test_app_unregister()
{
    printf("Testing app unregistration...\n");

    app_volume_control_init();

    // 注册并注销
    app_volume_register(1234, "com.test.app", "TestApp");

    OSStatus status = app_volume_unregister(1234);
    if (status != noErr)
    {
        printf("FAIL: App unregistration failed\n");
        app_volume_control_cleanup();
        return 1;
    }

    // 注销不存在的应用应该返回错误
    status = app_volume_unregister(9999);
    if (status == noErr)
    {
        printf("FAIL: Unregistering non-existent app should fail\n");
        app_volume_control_cleanup();
        return 1;
    }

    app_volume_control_cleanup();
    printf("PASS: App unregistration test\n");
    return 0;
}

// 测试音量设置和获取
static int test_volume_control()
{
    printf("Testing volume control...\n");

    app_volume_control_init();

    // 注册应用
    app_volume_register(1234, "com.test.app", "TestApp");

    // 测试设置音量
    OSStatus status = app_volume_set(1234, 0.5f);
    if (status != noErr)
    {
        printf("FAIL: Volume set failed\n");
        app_volume_control_cleanup();
        return 1;
    }

    // 测试获取音量
    Float32 volume;
    status = app_volume_get(1234, &volume);
    if (status != noErr)
    {
        printf("FAIL: Volume get failed\n");
        app_volume_control_cleanup();
        return 1;
    }

    // 允许小的浮点误差
    if (volume < 0.49f || volume > 0.51f)
    {
        printf("FAIL: Volume mismatch: expected ~0.5, got %f\n", volume);
        app_volume_control_cleanup();
        return 1;
    }

    // 测试边界值
    app_volume_set(1234, 0.0f);
    app_volume_get(1234, &volume);
    if (volume != 0.0f)
    {
        printf("FAIL: Volume should be 0.0, got %f\n", volume);
        app_volume_control_cleanup();
        return 1;
    }

    app_volume_set(1234, 1.0f);
    app_volume_get(1234, &volume);
    if (volume != 1.0f)
    {
        printf("FAIL: Volume should be 1.0, got %f\n", volume);
        app_volume_control_cleanup();
        return 1;
    }

    // 测试超出范围的值应该被截断
    app_volume_set(1234, 2.0f); // 应该被截断到 1.0
    app_volume_get(1234, &volume);
    if (volume != 1.0f)
    {
        printf("FAIL: Volume should be clamped to 1.0, got %f\n", volume);
        app_volume_control_cleanup();
        return 1;
    }

    app_volume_set(1234, -0.5f); // 应该被截断到 0.0
    app_volume_get(1234, &volume);
    if (volume != 0.0f)
    {
        printf("FAIL: Volume should be clamped to 0.0, got %f\n", volume);
        app_volume_control_cleanup();
        return 1;
    }

    // 测试获取不存在的应用的音量
    status = app_volume_get(9999, &volume);
    if (status == noErr)
    {
        printf("FAIL: Getting volume for non-existent app should fail\n");
        app_volume_control_cleanup();
        return 1;
    }

    app_volume_control_cleanup();
    printf("PASS: Volume control test\n");
    return 0;
}

// 测试静音控制
static int test_mute_control()
{
    printf("Testing mute control...\n");

    app_volume_control_init();

    // 注册应用
    app_volume_register(1234, "com.test.app", "TestApp");

    // 测试设置静音
    OSStatus status = app_volume_set_mute(1234, true);
    if (status != noErr)
    {
        printf("FAIL: Mute set failed\n");
        app_volume_control_cleanup();
        return 1;
    }

    // 测试获取静音状态
    bool isMuted;
    status = app_volume_get_mute(1234, &isMuted);
    if (status != noErr)
    {
        printf("FAIL: Mute get failed\n");
        app_volume_control_cleanup();
        return 1;
    }

    if (!isMuted)
    {
        printf("FAIL: Mute state should be true\n");
        app_volume_control_cleanup();
        return 1;
    }

    // 测试取消静音
    app_volume_set_mute(1234, false);
    app_volume_get_mute(1234, &isMuted);
    if (isMuted)
    {
        printf("FAIL: Mute state should be false\n");
        app_volume_control_cleanup();
        return 1;
    }

    // 测试获取不存在的应用的静音状态
    status = app_volume_get_mute(9999, &isMuted);
    if (status == noErr)
    {
        printf("FAIL: Getting mute for non-existent app should fail\n");
        app_volume_control_cleanup();
        return 1;
    }

    app_volume_control_cleanup();
    printf("PASS: Mute control test\n");
    return 0;
}

// 测试应用查找
static int test_app_find()
{
    printf("Testing app find...\n");

    app_volume_control_init();

    // 注册应用
    app_volume_register(1234, "com.test.app", "TestApp");

    // 查找存在的应用
    AppVolumeInfo* info = app_volume_find(1234);
    if (info == NULL)
    {
        printf("FAIL: Should find registered app\n");
        app_volume_control_cleanup();
        return 1;
    }

    if (info->pid != 1234)
    {
        printf("FAIL: Found app has wrong PID\n");
        app_volume_control_cleanup();
        return 1;
    }

    // 查找不存在的应用
    info = app_volume_find(9999);
    if (info != NULL)
    {
        printf("FAIL: Should not find unregistered app\n");
        app_volume_control_cleanup();
        return 1;
    }

    app_volume_control_cleanup();
    printf("PASS: App find test\n");
    return 0;
}

// 测试获取所有应用列表
static int test_get_all_apps()
{
    printf("Testing get all apps...\n");

    app_volume_control_init();

    // 初始应该为空
    AppVolumeInfo* apps = NULL;
    UInt32 count = 0;

    OSStatus status = app_volume_get_all(&apps, &count);
    if (status != noErr)
    {
        printf("FAIL: Get all apps failed\n");
        app_volume_control_cleanup();
        return 1;
    }

    app_volume_free_list(apps);

    // 注册一些应用
    for (int i = 0; i < 3; i++)
    {
        pid_t pid = 3000 + i;
        char bundleId[256];
        char name[256];
        snprintf(bundleId, sizeof(bundleId), "com.test.app%d", i);
        snprintf(name, sizeof(name), "TestApp%d", i);
        app_volume_register(pid, bundleId, name);
    }

    // 获取列表
    status = app_volume_get_all(&apps, &count);
    if (status != noErr || count != 3)
    {
        printf("FAIL: Expected 3 apps, got %d\n", count);
        app_volume_free_list(apps);
        app_volume_control_cleanup();
        return 1;
    }

    // 验证应用信息
    for (UInt32 i = 0; i < count; i++)
    {
        if (apps[i].pid < 3000 || apps[i].pid > 3002)
        {
            printf("FAIL: Unexpected PID in list\n");
            app_volume_free_list(apps);
            app_volume_control_cleanup();
            return 1;
        }
    }

    app_volume_free_list(apps);
    app_volume_control_cleanup();
    printf("PASS: Get all apps test\n");
    return 0;
}

// 测试活动应用计数
static int test_active_count()
{
    printf("Testing active count...\n");

    app_volume_control_init();

    // 初始应该为0
    UInt32 count = app_volume_get_active_count();
    if (count != 0)
    {
        printf("FAIL: Initial active count should be 0, got %d\n", count);
        app_volume_control_cleanup();
        return 1;
    }

    // 注册应用（默认是活跃的）
    app_volume_register(1234, "com.test.app1", "TestApp1");
    app_volume_register(1235, "com.test.app2", "TestApp2");

    count = app_volume_get_active_count();
    if (count != 2)
    {
        printf("FAIL: Active count should be 2, got %d\n", count);
        app_volume_control_cleanup();
        return 1;
    }

    // 设置一个为非活跃
    app_volume_set_active(1234, false);

    count = app_volume_get_active_count();
    if (count != 1)
    {
        printf("FAIL: Active count should be 1, got %d\n", count);
        app_volume_control_cleanup();
        return 1;
    }

    app_volume_control_cleanup();
    printf("PASS: Active count test\n");
    return 0;
}

int run_app_volume_control_tests()
{
    printf("\nRunning app volume control tests...\n");
    int failed = 0;

    failed += test_init_cleanup();
    failed += test_app_registration();
    failed += test_app_unregister();
    failed += test_volume_control();
    failed += test_mute_control();
    failed += test_app_find();
    failed += test_get_all_apps();
    failed += test_active_count();

    return failed;
}
