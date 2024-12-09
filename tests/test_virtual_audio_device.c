//
// Created by AhogeK on 12/9/24.
//

#include "virtual_audio_device.h"
#include <stdio.h>

int main() {
    VirtualAudioDevice *device = NULL;
    OSStatus status = virtual_device_create(&device);

    if (status != kAudioHardwareNoError) {
        printf("Failed to create virtual audio device. Error: %d\n", status);
        return 1;  // 返回非零表示测试失败
    }

    // 检查设备状态
    if (device->state != DEVICE_STATE_STOPPED) {
        printf("Initial device state is incorrect\n");
        return 1;
    }

    if (atomic_load(&device->deviceIsRunning) != false) {
        printf("Device should not be running initially\n");
        return 1;
    }

    if (atomic_load(&device->volumeControlValue) != 100) {
        printf("Initial volume is incorrect\n");
        return 1;
    }

    if (atomic_load(&device->muteState) != false) {
        printf("Initial mute state is incorrect\n");
        return 1;
    }

    // 检查音频格式
    if (device->outputStream.format.mSampleRate != 48000.0) {
        printf("Incorrect sample rate\n");
        return 1;
    }

    // 检查缓冲区配置
    if (device->outputStream.bufferFrameSize != 512) {
        printf("Incorrect buffer frame size\n");
        return 1;
    }

    // 清理资源
    if (device != NULL) {
        virtual_device_destroy(device);
        printf("Device destroyed successfully!\n");
    }

    return 0;  // 返回0表示测试通过
}