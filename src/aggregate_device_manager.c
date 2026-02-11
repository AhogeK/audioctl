//
// Aggregate Device 管理模块实现 - 稳健 4 通道版
// Created by AhogeK on 02/05/26.
//

#include "aggregate_device_manager.h"
#include "aggregate_volume_proxy.h"
#include "virtual_device_manager.h"
#include "audio_control.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <mach/mach_time.h>

#pragma mark - 监听器

// 防止递归调用的标志
static volatile atomic_bool g_listenerReentrantGuard = false;
// 上次处理时间，用于限流
static volatile atomic_ullong g_lastListenerTime = 0;

static OSStatus device_listener_proc(AudioObjectID __unused inObjectID, UInt32 inNumberAddresses,
                                     const AudioObjectPropertyAddress inAddresses[], void* __unused inClientData)
{
    // 防止递归调用 - 如果已经在处理中，直接返回
    if (atomic_exchange(&g_listenerReentrantGuard, true))
    {
        return noErr;
    }

    // 限流：最少间隔 2 秒才处理一次
    UInt64 now = mach_absolute_time();
    UInt64 lastTime = atomic_load(&g_lastListenerTime);
    if (now - lastTime < 2000000000ULL) // 约2秒（mach_absolute_time单位）
    {
        atomic_store(&g_listenerReentrantGuard, false);
        return noErr;
    }

    bool shouldDeactivate = false;

    for (UInt32 i = 0; i < inNumberAddresses; i++)
    {
        if (inAddresses[i].mSelector != kAudioHardwarePropertyDevices &&
            inAddresses[i].mSelector != kAudioHardwarePropertyDefaultOutputDevice)
        {
            continue;
        }

        if (!aggregate_device_is_active())
        {
            continue;
        }

        // [健康检查] 检查 Aggregate Device 是否健康
        if (!aggregate_device_is_healthy())
        {
            printf("⚠️ 检测到 Aggregate Device 状态异常（物理设备可能已断开）\n");
            shouldDeactivate = true;
        }
    }

    if (shouldDeactivate)
    {
        // [安全网] 检查 HAL 是否处于可交互状态
        // 尝试读取一个简单的全局属性，如果失败，说明 HAL 正在重启或挂起，此时绝对不能调用 SetProperty
        AudioDeviceID defaultDev;
        UInt32 size = sizeof(defaultDev);
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
        };
        OSStatus probeStatus = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, &defaultDev);

        if (probeStatus != noErr)
        {
            printf("⚠️ HAL 状态异常 (%d)，跳过自动回退操作以防止死锁\n", probeStatus);
        }
        else
        {
            printf("🔄 正在执行安全回退：切换回系统默认物理设备...\n");
            // 安全回退：停用 Aggregate Device，恢复到最佳可用的物理设备
            aggregate_device_deactivate();
        }
    }

    // Update time regardless of outcome to throttle ALL checks
    atomic_store(&g_lastListenerTime, now);

    atomic_store(&g_listenerReentrantGuard, false);
    return noErr;
}

OSStatus aggregate_device_init(void)
{
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr, device_listener_proc, NULL);
    if (status != noErr) return status;

    addr.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
    return AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr, device_listener_proc, NULL);
}

void aggregate_device_cleanup(void)
{
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &addr, device_listener_proc, NULL);

    addr.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &addr, device_listener_proc, NULL);
}

#pragma mark - 内部辅助函数

static OSStatus get_all_devices(AudioDeviceID** devices, UInt32* count)
{
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize);
    if (status != noErr) return status;

    *count = dataSize / sizeof(AudioDeviceID);
    if (*count == 0)
    {
        *devices = NULL;
        return noErr;
    }

    *devices = (AudioDeviceID*)malloc(dataSize);
    if (*devices == NULL) return kAudioHardwareUnspecifiedError;

    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize, *devices);
    if (status != noErr)
    {
        free(*devices);
        *devices = NULL;
    }
    return status;
}

/**
 * 获取设备的 UID
 * uidSize 显式传递以确保 CFStringGetCString 的内存安全，防止缓冲区溢出
 */
static OSStatus get_device_uid(AudioDeviceID deviceId, char* uid, size_t uidSize)
{
    CFStringRef uidRef = NULL;
    UInt32 dataSize = sizeof(CFStringRef);
    AudioObjectPropertyAddress propertyAddress = {
        kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectGetPropertyData(deviceId, &propertyAddress, 0, NULL, &dataSize, &uidRef);
    if (status != noErr) return status;
    if (uidRef == NULL) return kAudioHardwareUnknownPropertyError;

    CFStringGetCString(uidRef, uid, (CFIndex)uidSize, kCFStringEncodingUTF8);
    CFRelease(uidRef);
    return noErr;
}

/**
 * 获取设备的名称
 * nameSize 显式传递以确保 CFStringGetCString 的内存安全
 */
static OSStatus get_device_name(AudioDeviceID deviceId, char* name, size_t nameSize)
{
    CFStringRef nameRef = NULL;
    UInt32 dataSize = sizeof(CFStringRef);
    AudioObjectPropertyAddress propertyAddress = {
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectGetPropertyData(deviceId, &propertyAddress, 0, NULL, &dataSize, &nameRef);
    if (status != noErr) return status;
    if (nameRef == NULL) return kAudioHardwareUnknownPropertyError;

    CFStringGetCString(nameRef, name, (CFIndex)nameSize, kCFStringEncodingUTF8);
    CFRelease(nameRef);
    return noErr;
}

static bool is_virtual_device(AudioDeviceID deviceId)
{
    char uid[256] = {0};
    if (get_device_uid(deviceId, uid, sizeof(uid)) != noErr) return false;
    return strstr(uid, VIRTUAL_DEVICE_UID) != NULL || strstr(uid, "Virtual") != NULL;
}

static bool is_aggregate_device(AudioDeviceID deviceId)
{
    AudioObjectPropertyAddress propertyAddress = {
        kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    AudioClassID classId = 0;
    UInt32 dataSize = sizeof(AudioClassID);
    OSStatus status = AudioObjectGetPropertyData(deviceId, &propertyAddress, 0, NULL, &dataSize, &classId);
    return (status == noErr && classId == kAudioAggregateDeviceClassID);
}

static AudioDeviceID find_aggregate_device(void)
{
    AudioDeviceID* devices = NULL;
    UInt32 count = 0;
    if (get_all_devices(&devices, &count) != noErr || devices == NULL) return kAudioObjectUnknown;

    AudioDeviceID agg = kAudioObjectUnknown;
    for (UInt32 i = 0; i < count; i++)
    {
        if (!is_aggregate_device(devices[i])) continue;

        char name[256] = {0};
        char uid[256] = {0};

        // 优先通过 UID 匹配，回退到通过名称匹配
        if ((get_device_uid(devices[i], uid, sizeof(uid)) == noErr &&
                strstr(uid, AGGREGATE_DEVICE_UID_PREFIX)) ||
            (get_device_name(devices[i], name, sizeof(name)) == noErr &&
                (strcasecmp(name, AGGREGATE_DEVICE_NAME) == 0 ||
                    strstr(name, "audioctl Aggregate") ||
                    strstr(name, "AudioCTL Aggregate"))))
        {
            agg = devices[i];
            break;
        }
    }
    free(devices);
    return agg;
}

#pragma mark - 设备管理

OSStatus aggregate_device_create(AudioDeviceID physicalDeviceID)
{
    VirtualDeviceInfo vInfo;
    if (!virtual_device_get_info(&vInfo)) return kAudioHardwareNotRunningError;

    if (physicalDeviceID == kAudioObjectUnknown)
    {
        physicalDeviceID = aggregate_device_get_recommended_physical_device();
    }
    if (physicalDeviceID == kAudioObjectUnknown) return kAudioHardwareBadDeviceError;

    aggregate_device_destroy();

    char uid[256];
    snprintf(uid, sizeof(uid), "%s-%d", AGGREGATE_DEVICE_UID_PREFIX, (int)getpid());
    CFStringRef uidRef = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    CFStringRef nameRef = CFStringCreateWithCString(NULL, AGGREGATE_DEVICE_NAME, kCFStringEncodingUTF8);

    char vUID[256];
    char pUID[256];
    get_device_uid(vInfo.deviceId, vUID, sizeof(vUID));
    get_device_uid(physicalDeviceID, pUID, sizeof(pUID));
    CFStringRef vUIDRef = CFStringCreateWithCString(NULL, vUID, kCFStringEncodingUTF8);
    CFStringRef pUIDRef = CFStringCreateWithCString(NULL, pUID, kCFStringEncodingUTF8);

    CFMutableArrayRef sublist = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    int ch2 = 2;
    int drift1 = 1;
    int drift0 = 0;
    CFNumberRef n2 = CFNumberCreate(NULL, kCFNumberIntType, &ch2);
    CFNumberRef nOn = CFNumberCreate(NULL, kCFNumberIntType, &drift1);
    CFNumberRef nOff = CFNumberCreate(NULL, kCFNumberIntType, &drift0);

    // Sub 1: Virtual Out (Aggregate Out 1-2)
    CFMutableDictionaryRef s1 = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(s1, CFSTR("uid"), vUIDRef);
    CFDictionarySetValue(s1, CFSTR("outputs"), n2);
    CFDictionarySetValue(s1, CFSTR("drift correction"), nOn);
    CFArrayAppendValue(sublist, s1);
    CFRelease(s1);

    // Sub 2: Physical Out (Aggregate Out 3-4)
    // [时钟策略] 物理设备不启用漂移补偿，因为它作为 Master Clock
    // 虚拟设备启用漂移补偿来同步到物理设备的时钟
    CFMutableDictionaryRef s2 = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(s2, CFSTR("uid"), pUIDRef);
    CFDictionarySetValue(s2, CFSTR("outputs"), n2);
    CFDictionarySetValue(s2, CFSTR("drift correction"), nOff);
    CFArrayAppendValue(sublist, s2);
    CFRelease(s2);

    // Sub 3: Virtual In (Aggregate In 1-2)
    CFMutableDictionaryRef s3 = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(s3, CFSTR("uid"), vUIDRef);
    CFDictionarySetValue(s3, CFSTR("inputs"), n2);
    CFDictionarySetValue(s3, CFSTR("drift correction"), nOn);
    CFArrayAppendValue(sublist, s3);
    CFRelease(s3);

    // [关键配置] Master Clock 选择策略
    //
    // 设置物理设备作为 Master Clock 的原因：
    //   1. 物理设备有稳定的硬件时钟（晶振），抖动小
    //   2. 虚拟设备是软件生成的时钟，精度较低
    //   3. 如果虚拟设备作为 Master，其时钟抖动会影响整个 Aggregate 的稳定性
    //   4. 虚拟设备启用 drift correction，自动同步到物理设备的时钟
    //
    // 配置详情：
    //   - 物理设备: drift correction = OFF, 作为 Master Clock
    //   - 虚拟设备: drift correction = ON, 跟随 Master Clock
    CFMutableDictionaryRef desc = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), uidRef);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), nameRef);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), sublist);
    CFDictionarySetValue(desc, CFSTR("master"), pUIDRef); // 物理设备作为时钟主设备

    AudioDeviceID agg = kAudioObjectUnknown;
    OSStatus status = AudioHardwareCreateAggregateDevice(desc, &agg);

    CFRelease(desc);
    CFRelease(sublist);
    CFRelease(n2);
    CFRelease(nOn);
    CFRelease(nOff);
    CFRelease(uidRef);
    CFRelease(nameRef);
    CFRelease(vUIDRef);
    CFRelease(pUIDRef);

    if (status == noErr)
    {
        printf("✅ Aggregate Device 已恢复 4 通道布局\n");
    }
    return status;
}

OSStatus aggregate_device_destroy(void)
{
    AudioDeviceID agg = find_aggregate_device();
    if (agg != kAudioObjectUnknown) return AudioHardwareDestroyAggregateDevice(agg);
    return noErr;
}

OSStatus aggregate_device_update_physical_device(AudioDeviceID newPhysicalDeviceID)
{
    return aggregate_device_create(newPhysicalDeviceID);
}

/**
 * 从 Aggregate Device 中提取物理设备的名称
 * nameSize 用于确保写入名称时的内存安全
 */
static void get_physical_device_name_from_aggregate(char* outName, size_t nameSize)
{
    AggregateDeviceInfo info;
    if (!aggregate_device_get_info(&info))
    {
        return;
    }

    for (UInt32 i = 0; i < info.subDeviceCount; i++)
    {
        if (is_virtual_device(info.subDevices[i]))
        {
            continue;
        }

        get_device_name(info.subDevices[i], outName, nameSize);
        break;
    }
}

OSStatus aggregate_device_activate(void)
{
    // 获取当前默认输出设备，用于显示信息
    AudioDeviceID originalDefault = aggregate_device_get_current_default_output();
    char originalName[256] = {0};
    if (originalDefault != kAudioObjectUnknown)
    {
        get_device_name(originalDefault, originalName, sizeof(originalName));
    }

    // 如果不存在，先创建
    if (!aggregate_device_is_created())
    {
        // 传入 kAudioObjectUnknown 让 aggregate_device_create 自动选择合适的物理设备
        OSStatus createStatus = aggregate_device_create(kAudioObjectUnknown);
        if (createStatus != noErr) return createStatus;

        // 【修复】创建后等待系统刷新设备列表
        // 系统需要时间让新创建的 Aggregate Device 生效
        struct timespec ts500 = {0, 500000000};
        nanosleep(&ts500, NULL);
    }

    // 【修复】查找 Aggregate Device，如果找不到则重试几次
    AudioDeviceID agg = kAudioObjectUnknown;
    int retryCount = 0;
    const int maxRetries = 5;

    while (agg == kAudioObjectUnknown && retryCount < maxRetries)
    {
        agg = find_aggregate_device();
        if (agg == kAudioObjectUnknown)
        {
            retryCount++;
            struct timespec ts200 = {0, 200000000};
            nanosleep(&ts200, NULL);
        }
    }

    if (agg == kAudioObjectUnknown)
    {
        fprintf(stderr, "❌ 无法找到 Aggregate Device\n");
        return kAudioHardwareBadDeviceError;
    }

    AudioObjectPropertyAddress outAddr = {
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectSetPropertyData(kAudioObjectSystemObject, &outAddr, 0, NULL, sizeof(AudioDeviceID),
                                                 &agg);

    if (status != noErr) return status;

    char physName[256] = "未知物理设备";
    get_physical_device_name_from_aggregate(physName, sizeof(physName));

    printf("✅ Aggregate Device 已设为默认输出\n");
    if (strlen(originalName) > 0)
    {
        printf("   原输出设备: %s\n", originalName);
    }
    printf("   音频流: 应用 → 虚拟设备(音量控制) → %s\n", physName);

    // 启动音量代理，使 Aggregate Device 可以控制音量
    OSStatus proxyStatus = aggregate_volume_proxy_start();
    if (proxyStatus != noErr)
    {
        printf("⚠️  警告: 无法启动音量代理，Aggregate Device 音量控制可能不可用\n");
    }

    return status;
}

OSStatus aggregate_device_deactivate(void)
{
    // 优先使用 Aggregate Device 中绑定的物理设备
    AudioDeviceID physical = aggregate_device_get_physical_device();

    // 如果 Aggregate Device 不存在或没有绑定物理设备，使用当前默认设备
    if (physical == kAudioObjectUnknown)
    {
        physical = aggregate_device_get_current_default_output();
    }

    // 如果当前默认是聚合设备，需要找另一个物理设备
    if (physical == kAudioObjectUnknown || is_aggregate_device(physical))
    {
        physical = aggregate_device_get_recommended_physical_device();
    }

    if (physical == kAudioObjectUnknown || is_virtual_device(physical))
    {
        return kAudioHardwareBadDeviceError;
    }

    AudioObjectPropertyAddress outAddr = {
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectSetPropertyData(kAudioObjectSystemObject, &outAddr, 0, NULL, sizeof(AudioDeviceID),
                                                 &physical);

    if (status == noErr)
    {
        char name[256] = {0};
        get_device_name(physical, name, sizeof(name));
        printf("✅ 已恢复到物理设备: %s\n", name);
    }

    // 停止音量代理
    aggregate_volume_proxy_stop();

    return status;
}

bool aggregate_device_is_created(void)
{
    return find_aggregate_device() != kAudioObjectUnknown;
}

bool aggregate_device_is_active(void)
{
    AudioDeviceID agg = find_aggregate_device();
    if (agg == kAudioObjectUnknown) return false;

    AudioDeviceID def = kAudioObjectUnknown;
    UInt32 sz = sizeof(def);
    AudioObjectPropertyAddress outAddr = {
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };

    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &outAddr, 0, NULL, &sz, &def) != noErr)
    {
        return false;
    }
    return agg == def;
}

bool aggregate_device_get_info(AggregateDeviceInfo* outInfo)
{
    AudioDeviceID agg = find_aggregate_device();
    if (agg == kAudioObjectUnknown) return false;

    outInfo->deviceId = agg;
    outInfo->isCreated = true;
    outInfo->isActive = aggregate_device_is_active();

    AudioObjectPropertyAddress subAddr = {
        kAudioAggregateDevicePropertyActiveSubDeviceList, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 sz = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(agg, &subAddr, 0, NULL, &sz);
    if (status != noErr)
    {
        outInfo->subDeviceCount = 0;
        return true;
    }

    outInfo->subDeviceCount = sz / sizeof(AudioDeviceID);
    if (outInfo->subDeviceCount > 0)
    {
        AudioObjectGetPropertyData(agg, &subAddr, 0, NULL, &sz, outInfo->subDevices);
    }
    return true;
}

void aggregate_device_print_status(void)
{
    AggregateDeviceInfo info;
    if (!aggregate_device_get_info(&info))
    {
        printf("⚠️ Aggregate Device 未创建\n");
        return;
    }
    printf("✅ Aggregate Device 已就绪 (稳健模式)\n\n");
}

AudioDeviceID aggregate_device_get_physical_device(void)
{
    AggregateDeviceInfo info;
    if (!aggregate_device_get_info(&info)) return kAudioObjectUnknown;

    for (UInt32 i = 0; i < info.subDeviceCount; i++)
    {
        if (!is_virtual_device(info.subDevices[i]))
        {
            return info.subDevices[i];
        }
    }
    return kAudioObjectUnknown;
}

static bool check_device_alive(AudioDeviceID deviceId)
{
    if (deviceId == kAudioObjectUnknown) return false;

    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };

    UInt32 isAlive = 0;
    UInt32 size = sizeof(isAlive);
    OSStatus status = AudioObjectGetPropertyData(deviceId, &addr, 0, NULL, &size, &isAlive);

    return (status == noErr && isAlive);
}

bool aggregate_device_is_healthy(void)
{
    // 1. 获取 Aggregate Device 信息
    AggregateDeviceInfo info;
    if (!aggregate_device_get_info(&info)) return false;

    // 2. 检查是否有物理设备
    AudioDeviceID physicalDevice = aggregate_device_get_physical_device();
    if (physicalDevice == kAudioObjectUnknown)
    {
        // 只有虚拟设备，没有物理设备 -> 不健康
        return false;
    }

    // 3. 检查物理设备是否存活
    if (!check_device_alive(physicalDevice))
    {
        return false;
    }

    // 4. 检查是否有虚拟设备
    if (!aggregate_device_contains_virtual(&info))
    {
        return false;
    }

    return true;
}

bool aggregate_device_contains_virtual(const AggregateDeviceInfo* info)
{
    for (UInt32 i = 0; i < info->subDeviceCount; i++)
    {
        if (is_virtual_device(info->subDevices[i])) return true;
    }
    return false;
}

bool aggregate_device_contains_physical(const AggregateDeviceInfo* info, AudioDeviceID physicalDevice)
{
    for (UInt32 i = 0; i < info->subDeviceCount; i++)
    {
        if (info->subDevices[i] == physicalDevice) return true;
    }
    return false;
}

AudioDeviceID aggregate_device_get_current_default_output(void)
{
    AudioDeviceID defaultDevice = kAudioObjectUnknown;
    UInt32 dataSize = sizeof(AudioDeviceID);
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize,
                                                 &defaultDevice);
    if (status != noErr) return kAudioObjectUnknown;
    return defaultDevice;
}

AudioDeviceID aggregate_device_get_recommended_physical_device(void)
{
    AudioDeviceID* devices = NULL;
    UInt32 count = 0;
    if (get_all_devices(&devices, &count) != noErr || devices == NULL) return kAudioObjectUnknown;

    // 首先尝试获取当前默认输出设备
    AudioDeviceID currentDefault = aggregate_device_get_current_default_output();

    // 如果当前默认设备是有效的物理设备（不是虚拟设备或聚合设备），优先使用它
    if (currentDefault != kAudioObjectUnknown &&
        !is_virtual_device(currentDefault) &&
        !is_aggregate_device(currentDefault))
    {
        // 验证它有输出流
        AudioObjectPropertyAddress streamAddr = {
            kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain
        };
        UInt32 sz = 0;
        if (AudioObjectGetPropertyDataSize(currentDefault, &streamAddr, 0, NULL, &sz) == noErr && sz > 0)
        {
            free(devices);
            return currentDefault;
        }
    }

    // 如果默认设备不可用，按顺序选择第一个可用物理设备
    AudioDeviceID rec = kAudioObjectUnknown;
    for (UInt32 i = 0; i < count; i++)
    {
        if (is_virtual_device(devices[i]) || is_aggregate_device(devices[i])) continue;

        AudioObjectPropertyAddress streamAddr = {
            kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain
        };
        UInt32 sz = 0;
        if (AudioObjectGetPropertyDataSize(devices[i], &streamAddr, 0, NULL, &sz) == noErr && sz > 0)
        {
            rec = devices[i];
            break;
        }
    }

    free(devices);
    return rec;
}
