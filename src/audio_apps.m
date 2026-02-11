#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreAudio/CoreAudio.h>
#import "audio_apps.h"
#import "app_volume_control.h"

@interface AudioAppManager : NSObject
+ (NSArray *)getRunningAudioApps;
@end

@implementation AudioAppManager

+ (NSArray *)getRunningAudioApps {
    NSWorkspace *workspace = [NSWorkspace sharedWorkspace];
    NSArray *runningApps = [workspace runningApplications];
    NSMutableArray *audioApps = [NSMutableArray array];

    // 直接列出所有常规应用，不依赖 CoreAudio 状态
    // 这样即使 HAL 挂了，我们也能预设音量
    for (NSRunningApplication *app in runningApps) {
        // 放宽条件：包括 Regular (0) 和 Accessory (1)
        // 排除 Prohibited (2) 即后台守护进程
        if (app.activationPolicy != NSApplicationActivationPolicyProhibited) {
            [audioApps addObject:app];
        }
    }
    
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

        // 初始化应用音量控制系统
        app_volume_control_init();

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

            // 注册应用到音量控制系统
            app_volume_register(info->pid, info->bundleId, info->name);
            
            // 从音量控制系统获取音量设置
            Float32 volume;
            if (app_volume_get(info->pid, &volume) == noErr) {
                info->volume = volume;
            } else {
                info->volume = 1.0f;  // 默认100%
            }
        }

        return noErr;
    }
}

void freeAudioApps(AudioAppInfo *apps) {
    if (apps != NULL) {
        free(apps);
    }
}