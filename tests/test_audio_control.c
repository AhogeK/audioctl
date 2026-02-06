//
// Created by AhogeK on 12/9/24.
//

#include "driver/virtual_audio_device.h"
#include <stdio.h>

// 测试静音控制
static int test_mute_control()
{
    printf("Testing mute control...\n");

    VirtualAudioDevice* device = NULL;
    OSStatus status = virtual_device_create(&device);

    if (status != kAudioHardwareNoError || device == NULL)
    {
        printf("FAIL: Device creation failed\n");
        return 1;
    }

    // 测试空指针
    Boolean outMute;
    status = virtual_device_get_mute(NULL, &outMute);
    if (status != kAudioHardwareIllegalOperationError)
    {
        printf("FAIL: Get mute should fail with NULL device\n");
        virtual_device_destroy(device);
        return 1;
    }

    status = virtual_device_get_mute(device, NULL);
    if (status != kAudioHardwareIllegalOperationError)
    {
        printf("FAIL: Get mute should fail with NULL outMute\n");
        virtual_device_destroy(device);
        return 1;
    }

    status = virtual_device_set_mute(NULL, true);
    if (status != kAudioHardwareIllegalOperationError)
    {
        printf("FAIL: Set mute should fail with NULL device\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 测试初始状态
    status = virtual_device_get_mute(device, &outMute);
    if (status != kAudioHardwareNoError || outMute != false)
    {
        printf("FAIL: Initial mute state incorrect\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 测试设置静音
    status = virtual_device_set_mute(device, true);
    if (status != kAudioHardwareNoError)
    {
        printf("FAIL: Set mute to true failed\n");
        virtual_device_destroy(device);
        return 1;
    }

    status = virtual_device_get_mute(device, &outMute);
    if (status != kAudioHardwareNoError || outMute != true)
    {
        printf("FAIL: Mute state not set to true\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 测试取消静音
    status = virtual_device_set_mute(device, false);
    if (status != kAudioHardwareNoError)
    {
        printf("FAIL: Set mute to false failed\n");
        virtual_device_destroy(device);
        return 1;
    }

    status = virtual_device_get_mute(device, &outMute);
    if (status != kAudioHardwareNoError || outMute != false)
    {
        printf("FAIL: Mute state not set to false\n");
        virtual_device_destroy(device);
        return 1;
    }

    virtual_device_destroy(device);
    printf("PASS: Mute control test\n");
    return 0;
}

// 测试音量控制
static int test_volume_control()
{
    printf("Testing volume control...\n");

    VirtualAudioDevice* device = NULL;
    OSStatus status = virtual_device_create(&device);

    if (status != kAudioHardwareNoError || device == NULL)
    {
        printf("FAIL: Device creation failed\n");
        return 1;
    }

    // 测试空指针
    Float32 outVolume;
    status = virtual_device_get_volume(NULL, &outVolume);
    if (status != kAudioHardwareIllegalOperationError)
    {
        printf("FAIL: Get volume should fail with NULL device\n");
        virtual_device_destroy(device);
        return 1;
    }

    status = virtual_device_get_volume(device, NULL);
    if (status != kAudioHardwareIllegalOperationError)
    {
        printf("FAIL: Get volume should fail with NULL outVolume\n");
        virtual_device_destroy(device);
        return 1;
    }

    status = virtual_device_set_volume(NULL, 50.0f);
    if (status != kAudioHardwareIllegalOperationError)
    {
        printf("FAIL: Set volume should fail with NULL device\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 测试初始音量
    status = virtual_device_get_volume(device, &outVolume);
    if (status != kAudioHardwareNoError || outVolume != 100.0f)
    {
        printf("FAIL: Initial volume incorrect\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 测试设置无效音量
    status = virtual_device_set_volume(device, -1.0f);
    if (status != kAudioHardwareIllegalOperationError)
    {
        printf("FAIL: Set volume should fail with negative value\n");
        virtual_device_destroy(device);
        return 1;
    }

    status = virtual_device_set_volume(device, 101.0f);
    if (status != kAudioHardwareIllegalOperationError)
    {
        printf("FAIL: Set volume should fail with value > 100\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 测试设置有效音量
    const Float32 testVolumes[] = {0.0f, 50.0f, 100.0f};
    for (size_t i = 0; i < sizeof(testVolumes) / sizeof(testVolumes[0]); i++)
    {
        status = virtual_device_set_volume(device, testVolumes[i]);
        if (status != kAudioHardwareNoError)
        {
            printf("FAIL: Set volume failed for %.1f\n", testVolumes[i]);
            virtual_device_destroy(device);
            return 1;
        }

        status = virtual_device_get_volume(device, &outVolume);
        if (status != kAudioHardwareNoError || outVolume != testVolumes[i])
        {
            printf("FAIL: Volume not set correctly to %.1f\n", testVolumes[i]);
            virtual_device_destroy(device);
            return 1;
        }
    }

    virtual_device_destroy(device);
    printf("PASS: Volume control test\n");
    return 0;
}

int run_audio_control_tests()
{
    printf("\nRunning audio control tests...\n");
    int failed = 0;

    failed += test_mute_control();
    failed += test_volume_control();

    return failed;
}