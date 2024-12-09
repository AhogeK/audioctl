//
// Created by AhogeK on 12/9/24.
//

#include "virtual_audio_device.h"
#include <stdio.h>

static int test_device_creation() {
    printf("Testing device creation and basic properties...\n");

    VirtualAudioDevice *device = NULL;
    OSStatus status = virtual_device_create(&device);

    if (status != kAudioHardwareNoError) {
        printf("FAIL: Failed to create virtual audio device. Error: %d\n", status);
        return 1;
    }

    // 检查设备状态
    if (device->state != DEVICE_STATE_STOPPED) {
        printf("FAIL: Initial device state is incorrect\n");
        virtual_device_destroy(device);
        return 1;
    }

    if (atomic_load(&device->deviceIsRunning) != false) {
        printf("FAIL: Device should not be running initially\n");
        virtual_device_destroy(device);
        return 1;
    }

    if (atomic_load(&device->volumeControlValue) != 100) {
        printf("FAIL: Initial volume is incorrect\n");
        virtual_device_destroy(device);
        return 1;
    }

    if (atomic_load(&device->muteState) != false) {
        printf("FAIL: Initial mute state is incorrect\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 检查音频格式
    if (device->outputStream.format.mSampleRate != 48000.0) {
        printf("FAIL: Incorrect sample rate\n");
        virtual_device_destroy(device);
        return 1;
    }

    // 检查缓冲区配置
    if (device->outputStream.bufferFrameSize != 512) {
        printf("FAIL: Incorrect buffer frame size\n");
        virtual_device_destroy(device);
        return 1;
    }

    virtual_device_destroy(device);
    printf("PASS: Device creation test\n");
    return 0;
}

int run_basic_device_tests() {
    printf("\nRunning basic device tests...\n");
    int failed = 0;

    failed += test_device_creation();

    return failed;
}