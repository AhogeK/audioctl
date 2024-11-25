#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreAudio/CoreAudio.h>
#import "audio_apps.h"

@interface AudioAppManager : NSObject
+ (NSArray *)getRunningAudioApps;
@end

@implementation AudioAppManager

+ (NSArray *)getRunningAudioApps {
    NSWorkspace *workspace = [NSWorkspace sharedWorkspace];
    NSArray *runningApps = [workspace runningApplications];
    NSMutableArray *audioApps = [NSMutableArray array];

    // 获取系统默认输出设备
    AudioDeviceID outputDevice = 0;
    UInt32 propertySize = sizeof(AudioDeviceID);
    AudioObjectPropertyAddress propertyAddress = {
            .mSelector = kAudioHardwarePropertyDefaultOutputDevice,
            .mScope = kAudioObjectPropertyScopeGlobal,
            .mElement = kAudioObjectPropertyElementMain
    };

    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                                 &propertyAddress,
                                                 0,
                                                 NULL,
                                                 &propertySize,
                                                 &outputDevice);

    if (status != noErr) {
        NSLog(@"Error getting default output device: %d", (int) status);
        return audioApps;
    }

    // 获取输出设备的流属性
    AudioObjectPropertyAddress streamAddress = {
            .mSelector = kAudioDevicePropertyStreamConfiguration,
            .mScope = kAudioDevicePropertyScopeOutput,
            .mElement = kAudioObjectPropertyElementMain
    };

    status = AudioObjectGetPropertyDataSize(outputDevice,
                                            &streamAddress,
                                            0,
                                            NULL,
                                            &propertySize);

    if (status != noErr) {
        NSLog(@"Error getting stream configuration size: %d", (int) status);
        return audioApps;
    }

    // 修改：使用 NSMutableData 来管理内存，避免手动管理
    NSMutableData *bufferData = [NSMutableData dataWithLength:propertySize];
    AudioBufferList *bufferList = (AudioBufferList *) bufferData.mutableBytes;

    if (bufferList == NULL) {
        return audioApps;
    }

    status = AudioObjectGetPropertyData(outputDevice,
                                        &streamAddress,
                                        0,
                                        NULL,
                                        &propertySize,
                                        bufferList);

    if (status == noErr) {
        // 检查每个运行的应用是否在使用音频
        for (NSRunningApplication *app in runningApps) {
            if (app.activationPolicy == NSApplicationActivationPolicyRegular) {
                // 检查进程是否在使用音频设备
                AudioObjectPropertyAddress processAddress = {
                        .mSelector = kAudioDevicePropertyDeviceIsAlive,
                        .mScope = kAudioDevicePropertyScopeOutput,
                        .mElement = kAudioObjectPropertyElementMain
                };

                UInt32 isAlive = 0;
                propertySize = sizeof(UInt32);
                status = AudioObjectGetPropertyData(outputDevice,
                                                    &processAddress,
                                                    0,
                                                    NULL,
                                                    &propertySize,
                                                    &isAlive);

                if (status == noErr && isAlive) {
                    // 检查进程是否有音频输出
                    AudioObjectPropertyAddress outputAddress = {
                            .mSelector = kAudioDevicePropertyStreamFormat,
                            .mScope = kAudioDevicePropertyScopeOutput,
                            .mElement = kAudioObjectPropertyElementMain
                    };

                    AudioStreamBasicDescription streamFormat;
                    propertySize = sizeof(AudioStreamBasicDescription);
                    status = AudioObjectGetPropertyData(outputDevice,
                                                        &outputAddress,
                                                        0,
                                                        NULL,
                                                        &propertySize,
                                                        &streamFormat);

                    if (status == noErr && streamFormat.mChannelsPerFrame > 0) {
                        [audioApps addObject:app];
                    }
                }
            }
        }
    }

    // 不需要手动 free，NSMutableData 会自动管理内存
    return audioApps;
}

@end

OSStatus getAudioApps(AudioAppInfo **apps, UInt32 *appCount) {
    @autoreleasepool {
        // 添加参数验证
        if (apps == NULL || appCount == NULL) {
            return paramErr;
        }

        NSArray *audioApps = [AudioAppManager getRunningAudioApps];
        *appCount = (UInt32) [audioApps count];

        if (*appCount == 0) {
            *apps = NULL;
            return noErr;
        }

        // 使用 calloc 确保内存初始化为 0
        *apps = (AudioAppInfo *) calloc(*appCount, sizeof(AudioAppInfo));
        if (*apps == NULL) {
            return memFullErr;
        }

        for (NSUInteger i = 0; i < *appCount; i++) {
            NSRunningApplication *app = audioApps[i];
            AudioAppInfo *info = &(*apps)[i];

            // 添加安全检查
            if (app.bundleIdentifier == nil || app.localizedName == nil) {
                continue;
            }

            // 使用安全的字符串复制
            [app.bundleIdentifier getCString:info->bundleId
                                   maxLength:sizeof(info->bundleId) - 1  // 保留结尾的 null 字符空间
                                    encoding:NSUTF8StringEncoding];
            info->bundleId[sizeof(info->bundleId) - 1] = '\0';  // 确保字符串结束

            [app.localizedName getCString:info->name
                                maxLength:sizeof(info->name) - 1
                                 encoding:NSUTF8StringEncoding];
            info->name[sizeof(info->name) - 1] = '\0';

            // 设置进程 ID
            info->pid = app.processIdentifier;

            // 获取默认输出设备
            AudioDeviceID outputDevice;
            UInt32 propertySize = sizeof(AudioDeviceID);
            AudioObjectPropertyAddress propertyAddress = {
                    .mSelector = kAudioHardwarePropertyDefaultOutputDevice,
                    .mScope = kAudioObjectPropertyScopeGlobal,
                    .mElement = kAudioObjectPropertyElementMain
            };

            AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                       &propertyAddress,
                                       0,
                                       NULL,
                                       &propertySize,
                                       &outputDevice);

            info->deviceId = outputDevice;

            // 设置默认音量为1.0（100%）
            // 注意：这里我们设置默认值为100%，因为实际的每个应用音量需要更复杂的实现
            info->volume = 1.0f;
        }

        return noErr;
    }
}

void freeAudioApps(AudioAppInfo *apps) {
    if (apps != NULL) {
        free(apps);
    }
}