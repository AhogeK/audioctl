//
// Created by AhogeK on 12/9/24.
//

#include "virtual_audio_device.h"
#include <stdio.h>
#include <string.h>

// 测试空指针参数
static int test_null_device() {
    printf("Testing null device...\n");

    OSStatus status = virtual_device_start(NULL);
    if (status != kAudioHardwareIllegalOperationError) {
        printf("FAIL: Expected illegal operation error for null device start\n");
        return 1;
    }

    status = virtual_device_stop(NULL);
    if (status != kAudioHardwareIllegalOperationError) {
        printf("FAIL: Expected illegal operation error for null device stop\n");
        return 1;
    }

    printf("PASS: Null device test\n");
    return 0;
}

// 测试正常启动和停止流程
static int test_start_stop_cycle() {
    printf("Testing start-stop cycle...\n");
    VirtualAudioDevice *device = NULL;
    OSStatus status = virtual_device_create(&device);

    if (status != kAudioHardwareNoError || device == NULL) {
        printf("FAIL: Device creation failed\n");
        return 1;
    }

    // 测试启动
    status = virtual_device_start(device);
    if (status != kAudioHardwareNoError) {
        printf("FAIL: Device start failed with status %d\n", (int) status);
        virtual_device_destroy(device);
        return 1;
    }

    if (device->state != DEVICE_STATE_RUNNING) {
        printf("FAIL: Device state not set to running\n");
        virtual_device_destroy(device);
        return 1;
    }

    if (!atomic_load(&device->deviceIsRunning)) {
        printf("FAIL: Device running flag not set\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 测试停止
    status = virtual_device_stop(device);
    if (status != kAudioHardwareNoError) {
        printf("FAIL: Device stop failed with status %d\n", (int) status);
        virtual_device_destroy(device);
        return 1;
    }

    if (device->state != DEVICE_STATE_STOPPED) {
        printf("FAIL: Device state not set to stopped\n");
        virtual_device_destroy(device);
        return 1;
    }

    if (atomic_load(&device->deviceIsRunning)) {
        printf("FAIL: Device running flag not cleared\n");
        virtual_device_destroy(device);
        return 1;
    }

    virtual_device_destroy(device);
    printf("PASS: Start-stop cycle test\n");
    return 0;
}

// 测试重复启动和停止
static int test_multiple_start_stop() {
    printf("Testing multiple start-stop...\n");
    VirtualAudioDevice *device = NULL;
    OSStatus status = virtual_device_create(&device);

    if (status != kAudioHardwareNoError || device == NULL) {
        printf("FAIL: Device creation failed\n");
        return 1;
    }

    // 多次启动停止循环
    for (int i = 0; i < 3; i++) {
        status = virtual_device_start(device);
        if (status != kAudioHardwareNoError) {
            printf("FAIL: Start cycle %d failed\n", i);
            virtual_device_destroy(device);
            return 1;
        }

        status = virtual_device_stop(device);
        if (status != kAudioHardwareNoError) {
            printf("FAIL: Stop cycle %d failed\n", i);
            virtual_device_destroy(device);
            return 1;
        }
    }

    virtual_device_destroy(device);
    printf("PASS: Multiple start-stop test\n");
    return 0;
}

// 测试错误状态下的启动停止
static int test_error_state() {
    printf("Testing error state handling...\n");
    VirtualAudioDevice *device = NULL;
    OSStatus status = virtual_device_create(&device);

    if (status != kAudioHardwareNoError || device == NULL) {
        printf("FAIL: Device creation failed\n");
        return 1;
    }

    // 设置错误状态
    device->state = DEVICE_STATE_ERROR;

    // 尝试启动
    status = virtual_device_start(device);
    if (status != kAudioHardwareIllegalOperationError) {
        printf("FAIL: Start should fail in error state\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 尝试停止
    status = virtual_device_stop(device);
    if (status != kAudioHardwareIllegalOperationError) {
        printf("FAIL: Stop should fail in error state\n");
        virtual_device_destroy(device);
        return 1;
    }

    virtual_device_destroy(device);
    printf("PASS: Error state test\n");
    return 0;
}

int run_device_control_tests() {
    printf("\nRunning device control tests...\n");
    int failed = 0;

    failed += test_null_device();
    failed += test_start_stop_cycle();
    failed += test_multiple_start_stop();
    failed += test_error_state();

    return failed;
}