//
// Created by AhogeK on 12/10/24.
//

#include "driver/virtual_audio_device.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// 用于浮点数比较的误差范围
#define EPSILON 1e-6f

// 辅助函数：检查浮点数是否相等
static bool float_equals(Float32 a, Float32 b)
{
    return fabsf(a - b) < EPSILON;
}

// 测试输出处理的基本功能
static int test_basic_output_processing()
{
    printf("Testing basic output processing...\n");

    VirtualAudioDevice* device = NULL;
    OSStatus status = virtual_device_create(&device);

    if (status != kAudioHardwareNoError || device == NULL)
    {
        printf("FAIL: Device creation failed\n");
        return 1;
    }

    // 启动设备
    status = virtual_device_start(device);
    if (status != kAudioHardwareNoError)
    {
        printf("FAIL: Device start failed\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 创建测试音频缓冲区
    const UInt32 frameCount = 64;
    const UInt32 channelCount = 2;
    Float32 testData[frameCount * channelCount];

    // 填充测试数据
    for (UInt32 i = 0; i < frameCount * channelCount; i++)
    {
        testData[i] = 0.5f; // 测试用的固定振幅
    }

    AudioBuffer buffer = {
        .mNumberChannels = channelCount,
        .mDataByteSize = frameCount * channelCount * sizeof(Float32),
        .mData = testData
    };

    AudioBufferList bufferList = {
        .mNumberBuffers = 1,
        .mBuffers = {buffer}
    };

    // 测试空指针参数
    status = virtual_device_process_output(NULL, &bufferList, frameCount);
    if (status != kAudioHardwareIllegalOperationError)
    {
        printf("FAIL: Should fail with NULL device\n");
        virtual_device_destroy(device);
        return 1;
    }

    status = virtual_device_process_output(device, NULL, frameCount);
    if (status != kAudioHardwareIllegalOperationError)
    {
        printf("FAIL: Should fail with NULL buffer list\n");
        virtual_device_destroy(device);
        return 1;
    }

    status = virtual_device_process_output(device, &bufferList, 0);
    if (status != kAudioHardwareIllegalOperationError)
    {
        printf("FAIL: Should fail with zero frame count\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 测试正常音量处理
    status = virtual_device_set_volume(device, 50.0f); // 设置50%音量
    if (status != kAudioHardwareNoError)
    {
        printf("FAIL: Set volume failed\n");
        virtual_device_destroy(device);
        return 1;
    }

    status = virtual_device_process_output(device, &bufferList, frameCount);
    if (status != kAudioHardwareNoError)
    {
        printf("FAIL: Process output failed\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 验证音量缩放是否正确
    Float32* processedData = (Float32*)bufferList.mBuffers[0].mData;
    for (UInt32 i = 0; i < frameCount * channelCount; i++)
    {
        if (!float_equals(processedData[i], 0.25f))
        {
            // 0.5 * 0.5 = 0.25
            printf("FAIL: Volume scaling incorrect at sample %d\n", i);
            virtual_device_destroy(device);
            return 1;
        }
    }

    // 测试静音功能
    status = virtual_device_set_mute(device, true);
    if (status != kAudioHardwareNoError)
    {
        printf("FAIL: Set mute failed\n");
        virtual_device_destroy(device);
        return 1;
    }

    status = virtual_device_process_output(device, &bufferList, frameCount);
    if (status != kAudioHardwareNoError)
    {
        printf("FAIL: Process output failed during mute test\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 验证静音是否正确
    for (UInt32 i = 0; i < frameCount * channelCount; i++)
    {
        if (!float_equals(processedData[i], 0.0f))
        {
            printf("FAIL: Mute not working correctly at sample %d\n", i);
            virtual_device_destroy(device);
            return 1;
        }
    }

    virtual_device_destroy(device);
    printf("PASS: Basic output processing test\n");
    return 0;
}

// 测试信号限幅
static int test_output_clipping()
{
    printf("Testing output clipping...\n");

    VirtualAudioDevice* device = NULL;
    OSStatus status = virtual_device_create(&device);

    if (status != kAudioHardwareNoError || device == NULL)
    {
        printf("FAIL: Device creation failed\n");
        return 1;
    }

    // 启动设备
    status = virtual_device_start(device);
    if (status != kAudioHardwareNoError)
    {
        printf("FAIL: Device start failed\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 创建测试音频缓冲区
    const UInt32 frameCount = 64;
    const UInt32 channelCount = 2;
    Float32 testData[frameCount * channelCount];

    // 填充超出范围的测试数据
    for (UInt32 i = 0; i < frameCount * channelCount; i++)
    {
        testData[i] = 2.0f; // 超出正常范围的值
    }

    AudioBuffer buffer = {
        .mNumberChannels = channelCount,
        .mDataByteSize = frameCount * channelCount * sizeof(Float32),
        .mData = testData
    };

    AudioBufferList bufferList = {
        .mNumberBuffers = 1,
        .mBuffers = {buffer}
    };

    // 设置最大音量
    status = virtual_device_set_volume(device, 100.0f);
    if (status != kAudioHardwareNoError)
    {
        printf("FAIL: Set volume failed\n");
        virtual_device_destroy(device);
        return 1;
    }

    status = virtual_device_process_output(device, &bufferList, frameCount);
    if (status != kAudioHardwareNoError)
    {
        printf("FAIL: Process output failed\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 验证限幅是否正确
    Float32* processedData = (Float32*)bufferList.mBuffers[0].mData;
    for (UInt32 i = 0; i < frameCount * channelCount; i++)
    {
        if (!float_equals(processedData[i], 1.0f))
        {
            printf("FAIL: Clipping not working correctly at sample %d\n", i);
            virtual_device_destroy(device);
            return 1;
        }
    }

    virtual_device_destroy(device);
    printf("PASS: Output clipping test\n");
    return 0;
}

int run_audio_processing_tests()
{
    printf("\nRunning audio processing tests...\n");
    int failed = 0;

    failed += test_basic_output_processing();
    failed += test_output_clipping();

    return failed;
}