#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include "aggregate_device_manager.h"

static volatile bool g_running = true;

static void signal_handler(int __unused sig)
{
    g_running = false;
}

static void handle_interleaved_output(const Float32* in, Float32* out, UInt32 frames)
{
    for (UInt32 f = 0; f < frames; f++)
    {
        // 虚拟输入 1-2 (inInputData 通道 1-2) -> 物理输出 3-4 (outOutputData 通道 3-4)
        out[f * 4 + 2] += in[f * 2];
        out[f * 4 + 3] += in[f * 2 + 1];
    }
}

static void handle_separated_output(const Float32* in, Float32* out, UInt32 frames)
{
    UInt32 totalSamples = frames * 2;
    for (UInt32 i = 0; i < totalSamples; i++)
    {
        out[i] += in[i];
    }
}

/* 
 * CoreAudio IO 回调函数。
 * 注意：outOutputData 的签名由系统定义，必须为非 const 才能通过函数指针校验。
 */
static OSStatus ioProc(AudioDeviceID __unused inDevice,
                       const AudioTimeStamp* __unused inNow,
                       const AudioBufferList* inInputData,
                       const AudioTimeStamp* __unused inInputTime,
                       AudioBufferList* outOutputData,
                       const AudioTimeStamp* __unused inOutputTime,
                       void* __unused inClientData)
{
    OSStatus status = noErr;

    if (inInputData->mNumberBuffers == 0)
    {
        return noErr;
    }

    if (outOutputData->mNumberBuffers < 1)
    {
        return kAudioHardwareUnsupportedOperationError;
    }

    // 获取输入 (来自虚拟设备)
    const AudioBuffer* inputBuffer = &inInputData->mBuffers[0];
    if (inputBuffer->mDataByteSize == 0)
    {
        return noErr;
    }

    const Float32* in = (const Float32*)inputBuffer->mData;

    if (outOutputData->mNumberBuffers == 1)
    {
        // 4 通道在一个 buffer 里
        const AudioBuffer* outBuffer = &outOutputData->mBuffers[0];
        if (outBuffer->mNumberChannels < 4)
        {
            return kAudioHardwareUnsupportedOperationError;
        }

        Float32* out = (Float32*)outBuffer->mData;
        UInt32 frames = outBuffer->mDataByteSize / (sizeof(Float32) * 4);
        handle_interleaved_output(in, out, frames);
    }
    else if (outOutputData->mNumberBuffers >= 2)
    {
        // 分开的 buffers
        const AudioBuffer* outBuffer = &outOutputData->mBuffers[1];
        Float32* out = (Float32*)outBuffer->mData;
        UInt32 frames = outBuffer->mDataByteSize / (sizeof(Float32) * 2);
        handle_separated_output(in, out, frames);
    }

    return status;
}

static void safe_sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

static bool monitor_router_device(AudioDeviceID aggDevice)
{
    int consecutiveErrors = 0;

    while (g_running)
    {
        // 检查 Aggregate Device 是否仍然有效且是活动的
        if (!aggregate_device_is_active())
        {
            printf("Router: Aggregate Device became inactive.\n");
            return false;
        }

        // 尝试获取设备属性来检测 coreaudiod 是否还活着
        AudioObjectPropertyAddress addr = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        CFStringRef nameRef = NULL;
        UInt32 dataSize = sizeof(CFStringRef);
        OSStatus checkStatus = AudioObjectGetPropertyData(aggDevice, &addr, 0, NULL, &dataSize, &nameRef);

        if (checkStatus != noErr)
        {
            const int MAX_CONSECUTIVE_ERRORS = 5;
            consecutiveErrors++;
            fprintf(stderr, "Router: Warning - cannot communicate with coreaudiod (error %d), attempt %d/%d\n",
                    checkStatus, consecutiveErrors, MAX_CONSECUTIVE_ERRORS);

            if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS)
            {
                fprintf(stderr, "Router: coreaudiod appears to be restarting, disconnecting...\n");
                return false;
            }
        }
        else
        {
            if (nameRef) CFRelease(nameRef);
            consecutiveErrors = 0;
        }

        sleep(1);
    }
    return true;
}

static void attach_and_run_router(AudioDeviceID aggDevice)
{
    AudioDeviceIOProcID ioProcID = NULL;
    OSStatus status = AudioDeviceCreateIOProcID(aggDevice, &ioProc, NULL, &ioProcID);
    if (status != noErr)
    {
        fprintf(stderr, "Router: CreateIOProc failed: %d. Retrying...\n", status);
        sleep(2);
        return;
    }

    status = AudioDeviceStart(aggDevice, ioProcID);
    if (status != noErr)
    {
        fprintf(stderr, "Router: StartIO failed: %d\n", status);
        AudioDeviceDestroyIOProcID(aggDevice, ioProcID);
        sleep(2);
        return;
    }

    printf("Router: Routing Active.\n");

    monitor_router_device(aggDevice);

    // 安全地停止和销毁 IOProc
    printf("Router: Stopping IOProc...\n");
    OSStatus stopStatus = AudioDeviceStop(aggDevice, ioProcID);
    if (stopStatus != noErr)
    {
        fprintf(stderr, "Router: Warning - AudioDeviceStop returned %d\n", stopStatus);
    }

    // 添加短暂延迟，确保 coreaudiod 完成清理
    safe_sleep_ms(100); // 100ms

    OSStatus destroyStatus = AudioDeviceDestroyIOProcID(aggDevice, ioProcID);
    if (destroyStatus != noErr)
    {
        fprintf(stderr, "Router: Warning - AudioDeviceDestroyIOProcID returned %d\n", destroyStatus);
    }
}

void start_router_loop(void)
{
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    // 忽略 SIGPIPE，防止 coreaudiod 断开时进程崩溃
    signal(SIGPIPE, SIG_IGN);

    printf("Router: Starting router loop...\n");

    while (g_running)
    {
        AggregateDeviceInfo aggInfo;
        if (!aggregate_device_get_info(&aggInfo) || !aggInfo.isActive)
        {
            fprintf(stderr, "Router: Aggregate Device not found or inactive. Retrying in 2s...\n");
            sleep(2);
            continue;
        }

        printf("Router: Attached to Device ID %d\n", aggInfo.deviceId);
        attach_and_run_router(aggInfo.deviceId);

        if (g_running)
        {
            printf("Router: Attempting to restart routing...\n");
            // 增加等待时间，让 coreaudiod 完全恢复
            sleep(3);
        }
    }

    printf("Router: Stopped.\n");
}
