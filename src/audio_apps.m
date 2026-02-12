//
// Created by AhogeK on 02/12/26.
//
// 【重要说明】应用检测架构限制：
// 当前使用 Aggregate Device 模式（App -> Aggregate -> Virtual + Speaker）
// 在此模式下，应用连接 Aggregate Device，Virtual Device 作为子设备无法直接获取客户端列表。
//
// 本实现采用启发式方法：
// 1. 查询所有前台应用（排除系统进程）
// 2. 显示这些应用的音量控制状态
// 3. 用户可以为任何应用设置音量，即使它当前没有播放音频
//
// 【未来改进】串联架构（App -> Virtual -> Router -> Speaker）
// 在串联架构下，驱动可以通过 AddDeviceClient 准确知道哪些应用连接了 Virtual Device
//

#include "audio_apps.h"
#include "aggregate_device_manager.h"
#include "virtual_device_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <AppKit/AppKit.h>

// 使用 AppKit 获取前台应用列表（更可靠）
static OSStatus getFrontmostApps(AudioAppInfo** apps, UInt32* appCount)
{
    @autoreleasepool
    {
        NSWorkspace* workspace = [NSWorkspace sharedWorkspace];
        NSArray* runningApps = [workspace runningApplications];

        // 计算符合条件的应用数量
        NSUInteger eligibleCount = 0;
        for (NSRunningApplication* app in runningApps)
        {
            // 只包括常规应用和辅助应用，排除后台守护进程
            if (app.activationPolicy == NSApplicationActivationPolicyProhibited)
            {
                continue;
            }

            // 获取应用名称用于过滤
            char appName[256] = {0};
            if (app.localizedName)
            {
                [app.localizedName getCString:appName
                                    maxLength:sizeof(appName) - 1
                                     encoding:NSUTF8StringEncoding];
            }

            // 排除后台进程和系统组件（通过名称启发式判断）
            if (strstr(appName, "cef_server") != NULL ||
                strstr(appName, "Google Chrome Helper") != NULL ||
                strstr(appName, "Safari Networking") != NULL ||
                strstr(appName, "Safari Graphics") != NULL ||
                strstr(appName, "Safari Web Content") != NULL ||
                strstr(appName, "com.apple.") != NULL ||
                strstr(appName, "System") != NULL)
            {
                continue;
            }

            // 必须有 Bundle ID（真正的应用才有 Bundle ID）
            if (app.bundleIdentifier == nil)
            {
                continue;
            }

            // 排除没有 UI 的进程（通过检查是否有主窗口）
            // 注意：某些应用可能在后台启动，这里我们宽松处理

            eligibleCount++;
        }

        if (eligibleCount == 0)
        {
            *apps = NULL;
            *appCount = 0;
            return noErr;
        }

        // 分配内存
        AudioAppInfo* result = calloc(eligibleCount, sizeof(AudioAppInfo));
        if (!result)
        {
            return -1;
        }

        // 填充应用信息
        UInt32 index = 0;
        for (NSRunningApplication* app in runningApps)
        {
            if (app.activationPolicy == NSApplicationActivationPolicyProhibited)
            {
                continue;
            }

            // 应用名称
            char appName[256] = {0};
            if (app.localizedName)
            {
                [app.localizedName getCString:appName
                                    maxLength:sizeof(appName) - 1
                                     encoding:NSUTF8StringEncoding];
            }
            else
            {
                strncpy(appName, "Unknown", sizeof(appName) - 1);
            }

            // 排除后台进程和系统组件
            if (strstr(appName, "cef_server") != NULL ||
                strstr(appName, "Google Chrome Helper") != NULL ||
                strstr(appName, "Safari Networking") != NULL ||
                strstr(appName, "Safari Graphics") != NULL ||
                strstr(appName, "Safari Web Content") != NULL ||
                strstr(appName, "com.apple.") != NULL ||
                strstr(appName, "System") != NULL)
            {
                continue;
            }

            // 必须有 Bundle ID
            if (app.bundleIdentifier == nil)
            {
                continue;
            }

            // 跳过系统进程和辅助进程
            if (strncmp(appName, "kernel_task", 11) == 0 ||
                strncmp(appName, "launchd", 7) == 0 ||
                strncmp(appName, "coreaudiod", 10) == 0 ||
                strncmp(appName, "logd", 4) == 0 ||
                strncmp(appName, "syslogd", 7) == 0 ||
                strncmp(appName, "mds", 3) == 0 ||
                strncmp(appName, "mdworker", 8) == 0 ||
                strncmp(appName, "WindowServer", 12) == 0 ||
                strncmp(appName, "Dock", 4) == 0 ||
                strncmp(appName, "Finder", 6) == 0 ||
                strncmp(appName, "SystemUIServer", 14) == 0 ||
                strncmp(appName, "loginwindow", 11) == 0 ||
                strncmp(appName, "talagentd", 9) == 0 ||
                strncmp(appName, "Wallpaper", 9) == 0 ||
                strncmp(appName, "CoreServicesUIAgent", 19) == 0 ||
                strncmp(appName, "WindowManager", 13) == 0 ||
                strncmp(appName, "Control Center", 14) == 0 ||
                strncmp(appName, "Accessibility", 13) == 0 ||
                strncmp(appName, "CoreLocationAgent", 17) == 0 ||
                strncmp(appName, "Notification Center", 19) == 0 ||
                strncmp(appName, "TextInputMenuAgent", 18) == 0 ||
                strncmp(appName, "TextInputSwitcher", 17) == 0 ||
                strncmp(appName, "Spotlight", 9) == 0 ||
                strncmp(appName, "SoftwareUpdateNotificationManager", 33) == 0 ||
                strncmp(appName, "Single Sign-On", 14) == 0 ||
                strncmp(appName, "Wi-Fi", 5) == 0 ||
                strncmp(appName, "BackgroundTaskManagementAgent", 29) == 0 ||
                strncmp(appName, "Universal Control", 17) == 0 ||
                strncmp(appName, "Shortcuts", 9) == 0 ||
                strncmp(appName, "Keychain Circle Notification", 28) == 0 ||
                strncmp(appName, "AquaAppearanceHelper", 20) == 0 ||
                strncmp(appName, "coreautha", 9) == 0 ||
                strncmp(appName, "LinkedNotesUIService", 20) == 0 ||
                strncmp(appName, "MobileDeviceUpdater", 19) == 0 ||
                strncmp(appName, "AirPlay Screen Mirroring", 24) == 0)
            {
                continue;
            }

            // 跳过帮助进程和辅助进程
            if (strstr(appName, " Helper") != NULL ||
                strstr(appName, "(Plugin)") != NULL ||
                strstr(appName, "(Renderer)") != NULL ||
                strstr(appName, " Networking") != NULL ||
                strstr(appName, " Graphics and Media") != NULL ||
                strstr(appName, " Web Content") != NULL ||
                strstr(appName, " Menubar") != NULL)
            {
                continue;
            }

            strncpy(result[index].name, appName, sizeof(result[index].name) - 1);
            result[index].name[sizeof(result[index].name) - 1] = '\0';

            // PID
            result[index].pid = app.processIdentifier;

            // Bundle ID
            if (app.bundleIdentifier)
            {
                [app.bundleIdentifier getCString:result[index].bundleId
                                       maxLength:sizeof(result[index].bundleId) - 1
                                        encoding:NSUTF8StringEncoding];
                result[index].bundleId[sizeof(result[index].bundleId) - 1] = '\0';
            }

            // 默认设备（Aggregate Device 或默认输出）
            AggregateDeviceInfo aggInfo;
            if (aggregate_device_get_info(&aggInfo))
            {
                result[index].deviceId = aggInfo.deviceId;
            }
            else
            {
                // 获取默认输出设备
                UInt32 dataSize = sizeof(AudioDeviceID);
                AudioObjectPropertyAddress address = {
                    kAudioHardwarePropertyDefaultOutputDevice,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &dataSize, &result[index].deviceId);
            }

            // 默认音量 100%
            result[index].volume = 1.0f;

            index++;
        }

        *apps = result;
        *appCount = (UInt32)index;

        return noErr;
    }
}

OSStatus getAudioApps(AudioAppInfo** apps, UInt32* appCount)
{
    if (!apps || !appCount)
    {
        return -1;
    }

    *apps = NULL;
    *appCount = 0;

    // 【说明】Aggregate Device 架构限制：
    // 应用连接 Aggregate Device，Virtual Device 作为子设备无法直接获取客户端列表。
    // 因此我们使用前台应用列表作为近似，允许用户为任何前台应用设置音量。
    // 当应用实际播放音频时，音量控制会生效。

    return getFrontmostApps(apps, appCount);
}

void freeAudioApps(AudioAppInfo* apps)
{
    if (apps)
    {
        free(apps);
    }
}
