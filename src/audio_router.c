#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

// Forward declaration
void start_router_loop(void);

static OSStatus ioProc(AudioDeviceID inDevice,
                       const AudioTimeStamp* inNow,
                       const AudioBufferList* inInputData,
                       const AudioTimeStamp* inInputTime,
                       AudioBufferList* outOutputData,
                       const AudioTimeStamp* inOutputTime,
                       void* inClientData)
{
#pragma unused(inDevice, inNow, inInputTime, inOutputTime, inClientData)

    if (inInputData->mNumberBuffers == 0 || outOutputData->mNumberBuffers < 2) return noErr;

    // 获取输入 (来自虚拟设备)
    const AudioBuffer* inputBuffer = &inInputData->mBuffers[0];
    Float32* in = (Float32*)inputBuffer->mData;
    UInt32 frames = inputBuffer->mDataByteSize / (sizeof(Float32) * 2);

    // 获取输出 (物理扬声器)
    AudioBuffer* targetBuffer = &outOutputData->mBuffers[1];
    Float32* out = (Float32*)targetBuffer->mData;

    // 使用叠加混合
    for (UInt32 i = 0; i < frames * 2; i++)
    {
        out[i] += in[i];
    }

    return noErr;
}

void start_router_loop(void)
{
    AudioDeviceID aggDevice = kAudioObjectUnknown;

    // 简单的轮询查找 Aggregate Device
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size);
    int count = size / sizeof(AudioDeviceID);
    AudioDeviceID* ids = malloc(size);

    if (ids)
    {
        // 关键修复：必须先填充 ids 数组
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, ids) == noErr)
        {
            // 尝试通过 UID 或名称查找
            for (int i = 0; i < count; i++)
            {
                char nameStr[256] = {0};
                char uidStr[256] = {0};
                UInt32 propSize;

                // 检查名称
                propSize = sizeof(CFStringRef);
                CFStringRef name = NULL;
                AudioObjectPropertyAddress nameAddr = {
                    kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
                };
                if (AudioObjectGetPropertyData(ids[i], &nameAddr, 0, NULL, &propSize, &name) == noErr && name)
                {
                    CFStringGetCString(name, nameStr, sizeof(nameStr), kCFStringEncodingUTF8);
                    CFRelease(name);
                }

                // 检查 UID
                propSize = sizeof(CFStringRef);
                CFStringRef uid = NULL;
                AudioObjectPropertyAddress uidAddr = {
                    kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
                };
                if (AudioObjectGetPropertyData(ids[i], &uidAddr, 0, NULL, &propSize, &uid) == noErr && uid)
                {
                    CFStringGetCString(uid, uidStr, sizeof(uidStr), kCFStringEncodingUTF8);
                    CFRelease(uid);
                }

                if (strstr(nameStr, "audioctl Aggregate") || strstr(uidStr, "audioctl-aggregate"))
                {
                    aggDevice = ids[i];
                    break;
                }
            }
        }
        free(ids);
    }

    if (aggDevice == kAudioObjectUnknown)
    {
        fprintf(stderr, "Router: Aggregate Device not found. Waiting...\n");
        sleep(2);
        exit(1);
    }

    printf("Router: Attached to Device ID %d\n", aggDevice);

    AudioDeviceIOProcID ioProcID;
    OSStatus status = AudioDeviceCreateIOProcID(aggDevice, ioProc, NULL, &ioProcID);
    if (status != noErr)
    {
        fprintf(stderr, "Router: CreateIOProc failed: %d\n", status);
        exit(1);
    }

    status = AudioDeviceStart(aggDevice, ioProcID);
    if (status != noErr)
    {
        fprintf(stderr, "Router: StartIO failed: %d\n", status);
        exit(1);
    }

    printf("Router: Routing Active.\n");

    // Keep alive
    while (1)
    {
        sleep(60);
    }
}