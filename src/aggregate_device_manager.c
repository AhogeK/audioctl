//
// Aggregate Device 管理模块实现
// Created by AhogeK on 02/05/26.
//

#include "aggregate_device_manager.h"
#include "virtual_device_manager.h"
#include "audio_control.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#pragma mark - 内部辅助函数

// 获取所有音频设备
static OSStatus get_all_devices(AudioDeviceID** devices, UInt32* count)
{
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize);
    if (status != noErr) return status;

    *count = dataSize / sizeof(AudioDeviceID);
    *devices = (AudioDeviceID*)malloc(dataSize);
    if (*devices == NULL) return -1;

    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize, *devices);
    if (status != noErr)
    {
        free(*devices);
        *devices = NULL;
    }

    return status;
}

// 获取设备的UID
static OSStatus get_device_uid(AudioDeviceID deviceId, char* uid, size_t uidSize)
{
    CFStringRef uidRef = NULL;
    UInt32 dataSize = sizeof(CFStringRef);
    AudioObjectPropertyAddress propertyAddress = {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    OSStatus status = AudioObjectGetPropertyData(deviceId, &propertyAddress, 0, NULL, &dataSize, &uidRef);
    if (status != noErr || uidRef == NULL) return status;

    CFStringGetCString(uidRef, uid, (CFIndex)uidSize, kCFStringEncodingUTF8);
    CFRelease(uidRef);

    return noErr;
}

// 获取设备的名称
static OSStatus get_device_name(AudioDeviceID deviceId, char* name, size_t nameSize)
{
    CFStringRef nameRef = NULL;
    UInt32 dataSize = sizeof(CFStringRef);
    AudioObjectPropertyAddress propertyAddress = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    OSStatus status = AudioObjectGetPropertyData(deviceId, &propertyAddress, 0, NULL, &dataSize, &nameRef);
    if (status != noErr || nameRef == NULL)
    {
        // 尝试备用属性
        propertyAddress.mSelector = kAudioDevicePropertyDeviceNameCFString;
        status = AudioObjectGetPropertyData(deviceId, &propertyAddress, 0, NULL, &dataSize, &nameRef);
        if (status != noErr || nameRef == NULL) return status;
    }

    CFStringGetCString(nameRef, name, (CFIndex)nameSize, kCFStringEncodingUTF8);
    CFRelease(nameRef);

    return noErr;
}

// 检查设备是否是虚拟设备
static bool is_virtual_device(AudioDeviceID deviceId)
{
    char uid[256] = {0};
    if (get_device_uid(deviceId, uid, sizeof(uid)) != noErr) return false;
    return strstr(uid, VIRTUAL_DEVICE_UID) != NULL || strstr(uid, "Virtual") != NULL;
}

// 检查设备是否是 Aggregate Device
static bool is_aggregate_device(AudioDeviceID deviceId)
{
    AudioObjectPropertyAddress propertyAddress = {
        kAudioObjectPropertyClass,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    AudioClassID classId = 0;
    UInt32 dataSize = sizeof(AudioClassID);
    OSStatus status = AudioObjectGetPropertyData(deviceId, &propertyAddress, 0, NULL, &dataSize, &classId);

    if (status != noErr) return false;
    return classId == kAudioAggregateDeviceClassID;
}

// 查找 Aggregate Device
static AudioDeviceID find_aggregate_device(void)
{
    AudioDeviceID* devices = NULL;
    UInt32 count = 0;

    OSStatus status = get_all_devices(&devices, &count);
    if (status != noErr || devices == NULL) return kAudioObjectUnknown;

    AudioDeviceID aggregateDevice = kAudioObjectUnknown;

    for (UInt32 i = 0; i < count; i++)
    {
        if (!is_aggregate_device(devices[i])) continue;

        char uid[256] = {0};
        status = get_device_uid(devices[i], uid, sizeof(uid));
        if (status == noErr && strstr(uid, AGGREGATE_DEVICE_UID_PREFIX) != NULL)
        {
            aggregateDevice = devices[i];
            break;
        }

        // 备用：通过名称匹配
        char name[256] = {0};
        status = get_device_name(devices[i], name, sizeof(name));
        if (status == noErr && strstr(name, "audioctl") != NULL)
        {
            aggregateDevice = devices[i];
        }
    }

    free(devices);
    return aggregateDevice;
}

// 获取默认输出设备
static AudioDeviceID get_default_output_device(void)
{
    AudioDeviceID deviceId = kAudioObjectUnknown;
    UInt32 dataSize = sizeof(AudioDeviceID);
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize, &deviceId);
    return deviceId;
}

#pragma mark - 设备检测

bool aggregate_device_is_created(void)
{
    return find_aggregate_device() != kAudioObjectUnknown;
}

bool aggregate_device_get_info(AggregateDeviceInfo* outInfo)
{
    if (outInfo == NULL) return false;

    memset(outInfo, 0, sizeof(AggregateDeviceInfo));

    AudioDeviceID aggregateDevice = find_aggregate_device();
    if (aggregateDevice == kAudioObjectUnknown)
    {
        outInfo->isCreated = false;
        return false;
    }

    outInfo->deviceId = aggregateDevice;
    outInfo->isCreated = true;
    get_device_name(aggregateDevice, outInfo->name, sizeof(outInfo->name));
    get_device_uid(aggregateDevice, outInfo->uid, sizeof(outInfo->uid));

    // 检查是否正在使用
    outInfo->isActive = aggregate_device_is_active();

    // 获取子设备列表
    AudioObjectPropertyAddress propertyAddress = {
        kAudioAggregateDevicePropertyActiveSubDeviceList,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(aggregateDevice, &propertyAddress, 0, NULL, &dataSize);
    if (status == noErr && dataSize > 0)
    {
        UInt32 deviceCount = dataSize / sizeof(AudioDeviceID);
        if (deviceCount > 8) deviceCount = 8;

        AudioDeviceID subDevices[8];
        status = AudioObjectGetPropertyData(aggregateDevice, &propertyAddress, 0, NULL, &dataSize, subDevices);
        if (status == noErr)
        {
            outInfo->subDeviceCount = deviceCount;
            memcpy(outInfo->subDevices, subDevices, deviceCount * sizeof(AudioDeviceID));
        }
    }

    return true;
}

bool aggregate_device_is_active(void)
{
    AudioDeviceID aggregateDevice = find_aggregate_device();
    if (aggregateDevice == kAudioObjectUnknown) return false;

    AudioDeviceID defaultOutput = get_default_output_device();
    return aggregateDevice == defaultOutput;
}

#pragma mark - 设备管理

OSStatus aggregate_device_create(AudioDeviceID physicalDeviceID)
{
    // 检查虚拟设备是否安装
    VirtualDeviceInfo virtualInfo;
    if (!virtual_device_get_info(&virtualInfo))
    {
        fprintf(stderr, "错误: 虚拟音频设备未安装\n");
        return -1;
    }

    // 如果未指定物理设备，自动选择
    if (physicalDeviceID == kAudioObjectUnknown)
    {
        physicalDeviceID = aggregate_device_get_recommended_physical_device();
        if (physicalDeviceID == kAudioObjectUnknown)
        {
            fprintf(stderr, "错误: 未找到可用的物理音频设备\n");
            return -1;
        }

        char name[256] = {0};
        get_device_name(physicalDeviceID, name, sizeof(name));
        printf("自动选择物理设备: %s (ID: %d)\n", name, physicalDeviceID);
    }

    // 检查是否已存在
    AudioDeviceID existing = find_aggregate_device();
    if (existing != kAudioObjectUnknown)
    {
        printf("Aggregate Device 已存在，先销毁旧的...\n");
        aggregate_device_destroy();
        sleep(1); // 等待系统处理
    }

    // 创建 Aggregate Device 配置
    // 使用 AudioHardwareCreateAggregateDevice API

    // 创建 UID
    char uid[256];
    snprintf(uid, sizeof(uid), "%s-%d", AGGREGATE_DEVICE_UID_PREFIX, (int)getpid());

    CFStringRef uidRef = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    CFStringRef nameRef = CFStringCreateWithCString(NULL, AGGREGATE_DEVICE_NAME, kCFStringEncodingUTF8);

    // 创建子设备列表
    CFMutableArrayRef subDevices = CFArrayCreateMutable(NULL, 0, NULL);

    // 添加虚拟设备（作为第一个设备，用于接收和处理音频）
    CFNumberRef virtualRef = CFNumberCreate(NULL, kCFNumberSInt32Type, &virtualInfo.deviceId);
    CFArrayAppendValue(subDevices, virtualRef);
    CFRelease(virtualRef);

    // 添加物理设备（作为输出设备）
    CFNumberRef physicalRef = CFNumberCreate(NULL, kCFNumberSInt32Type, &physicalDeviceID);
    CFArrayAppendValue(subDevices, physicalRef);
    CFRelease(physicalRef);

    // 创建设备描述字典
    CFMutableDictionaryRef description = CFDictionaryCreateMutable(NULL, 0,
                                                                   &kCFTypeDictionaryKeyCallBacks,
                                                                   &kCFTypeDictionaryValueCallBacks);

    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceUIDKey), uidRef);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceNameKey), nameRef);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceSubDeviceListKey), subDevices);

    // 设置为私有设备（不显示在系统偏好设置中）
    int isPrivate = 1;
    CFNumberRef privateRef = CFNumberCreate(NULL, kCFNumberIntType, &isPrivate);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceIsPrivateKey), privateRef);
    CFRelease(privateRef);

    // 设置时钟设备为物理设备（更好的同步）
    char physicalUID[256] = {0};
    get_device_uid(physicalDeviceID, physicalUID, sizeof(physicalUID));
    CFStringRef clockUIDRef = CFStringCreateWithCString(NULL, physicalUID, kCFStringEncodingUTF8);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceMasterSubDeviceKey), clockUIDRef);
    CFRelease(clockUIDRef);

    // 创建 Aggregate Device
    AudioDeviceID aggregateDevice = kAudioObjectUnknown;
    OSStatus status = AudioHardwareCreateAggregateDevice(description, &aggregateDevice);

    // 清理
    CFRelease(description);
    CFRelease(subDevices);
    CFRelease(uidRef);
    CFRelease(nameRef);

    if (status != noErr)
    {
        fprintf(stderr, "创建 Aggregate Device 失败: %d\n", status);
        return status;
    }

    printf("✅ Aggregate Device 创建成功 (ID: %d)\n", aggregateDevice);
    printf("   包含设备:\n");
    printf("   - 虚拟设备: %s\n", virtualInfo.name);

    char physName[256] = {0};
    get_device_name(physicalDeviceID, physName, sizeof(physName));
    printf("   - 物理设备: %s\n", physName);

    return noErr;
}

OSStatus aggregate_device_destroy(void)
{
    AudioDeviceID aggregateDevice = find_aggregate_device();
    if (aggregateDevice == kAudioObjectUnknown)
    {
        return noErr; // 不存在，视为成功
    }

    // 如果当前正在使用，先停用
    if (aggregate_device_is_active())
    {
        aggregate_device_deactivate();
        sleep(1);
    }

    OSStatus status = AudioHardwareDestroyAggregateDevice(aggregateDevice);
    if (status != noErr)
    {
        fprintf(stderr, "销毁 Aggregate Device 失败: %d\n", status);
        return status;
    }

    printf("✅ Aggregate Device 已销毁\n");
    return noErr;
}

OSStatus aggregate_device_update_physical_device(AudioDeviceID newPhysicalDeviceID)
{
    // 简化方案：销毁旧的，创建新的
    OSStatus status = aggregate_device_destroy();
    if (status != noErr) return status;

    sleep(1);

    status = aggregate_device_create(newPhysicalDeviceID);
    if (status != noErr) return status;

    // 重新激活
    return aggregate_device_activate();
}

#pragma mark - 激活/停用

OSStatus aggregate_device_activate(void)
{
    // 确保 Aggregate Device 存在
    if (!aggregate_device_is_created())
    {
        OSStatus status = aggregate_device_create(kAudioObjectUnknown);
        if (status != noErr) return status;
        sleep(1);
    }

    AudioDeviceID aggregateDevice = find_aggregate_device();
    if (aggregateDevice == kAudioObjectUnknown)
    {
        return -1;
    }

    // 设为默认输出
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    OSStatus status = AudioObjectSetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL,
                                                 sizeof(AudioDeviceID), &aggregateDevice);

    if (status == noErr)
    {
        printf("✅ Aggregate Device 已设为默认输出\n");
        printf("   音频流: 应用 → 虚拟设备(音量控制) → 物理设备(输出)\n");
    }
    else
    {
        fprintf(stderr, "设置默认输出失败: %d\n", status);
    }

    return status;
}

OSStatus aggregate_device_deactivate(void)
{
    // 恢复到推荐的物理设备
    AudioDeviceID physicalDevice = aggregate_device_get_recommended_physical_device();
    if (physicalDevice == kAudioObjectUnknown)
    {
        fprintf(stderr, "错误: 未找到可用的物理音频设备\n");
        return -1;
    }

    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    OSStatus status = AudioObjectSetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL,
                                                 sizeof(AudioDeviceID), &physicalDevice);

    if (status == noErr)
    {
        char name[256] = {0};
        get_device_name(physicalDevice, name, sizeof(name));
        printf("✅ 已恢复到物理设备: %s\n", name);
    }

    return status;
}

#pragma mark - 状态报告

void aggregate_device_print_status(void)
{
    printf("\n========== Aggregate Device 状态 ==========\n\n");

    // 检查虚拟设备
    VirtualDeviceInfo virtualInfo;
    bool hasVirtual = virtual_device_get_info(&virtualInfo);

    if (!hasVirtual)
    {
        printf("❌ 虚拟音频设备未安装\n");
        printf("   请先安装虚拟驱动: sudo ninja install\n");
        return;
    }

    printf("✅ 虚拟音频设备已安装\n");
    printf("   名称: %s\n\n", virtualInfo.name);

    // 检查 Aggregate Device
    AggregateDeviceInfo aggInfo;
    if (!aggregate_device_get_info(&aggInfo))
    {
        printf("⚠️  Aggregate Device 未创建\n");
        printf("   运行 'audioctl use-virtual' 创建并激活\n\n");

        // 显示推荐的物理设备
        AudioDeviceID recommended = aggregate_device_get_recommended_physical_device();
        if (recommended != kAudioObjectUnknown)
        {
            char name[256] = {0};
            get_device_name(recommended, name, sizeof(name));
            printf("   将自动绑定物理设备: %s\n", name);
        }
        return;
    }

    printf("✅ Aggregate Device 已创建\n");
    printf("   名称: %s\n", aggInfo.name);
    printf("   设备ID: %d\n", aggInfo.deviceId);
    printf("   子设备数: %d\n", aggInfo.subDeviceCount);

    for (UInt32 i = 0; i < aggInfo.subDeviceCount; i++)
    {
        char name[256] = {0};
        get_device_name(aggInfo.subDevices[i], name, sizeof(name));

        if (is_virtual_device(aggInfo.subDevices[i]))
        {
            printf("   - %s (虚拟设备 - 音量控制)\n", name);
        }
        else
        {
            printf("   - %s (物理设备 - 音频输出)\n", name);
        }
    }

    printf("\n");

    if (aggInfo.isActive)
    {
        printf("✅ Aggregate Device 是当前默认输出设备\n");
        printf("   应用音量控制功能已启用\n");
    }
    else
    {
        printf("⚠️  Aggregate Device 不是当前默认输出设备\n");
        printf("   运行 'audioctl use-virtual' 激活\n");
    }

    printf("\n==========================================\n");
}

AudioDeviceID aggregate_device_get_recommended_physical_device(void)
{
    AudioDeviceID* devices = NULL;
    UInt32 count = 0;

    OSStatus status = get_all_devices(&devices, &count);
    if (status != noErr || devices == NULL) return kAudioObjectUnknown;

    AudioDeviceID currentDefault = get_default_output_device();
    AudioDeviceID recommended = kAudioObjectUnknown;

    // 优先选择当前默认设备（如果不是虚拟或aggregate）
    if (currentDefault != kAudioObjectUnknown)
    {
        if (!is_virtual_device(currentDefault) && !is_aggregate_device(currentDefault))
        {
            recommended = currentDefault;
        }
    }

    // 如果没找到，选择第一个可用的物理输出设备
    if (recommended == kAudioObjectUnknown)
    {
        for (UInt32 i = 0; i < count; i++)
        {
            if (is_virtual_device(devices[i])) continue;
            if (is_aggregate_device(devices[i])) continue;

            // 检查是否有输出通道
            AudioObjectPropertyAddress propertyAddress = {
                kAudioDevicePropertyStreamConfiguration,
                kAudioDevicePropertyScopeOutput,
                kAudioObjectPropertyElementMain
            };

            UInt32 dataSize = 0;
            if (AudioObjectGetPropertyDataSize(devices[i], &propertyAddress, 0, NULL, &dataSize) == noErr)
            {
                if (dataSize > 0)
                {
                    recommended = devices[i];
                    break;
                }
            }
        }
    }

    free(devices);
    return recommended;
}

#pragma mark - 辅助功能

bool aggregate_device_contains_virtual(const AggregateDeviceInfo* info)
{
    if (info == NULL || !info->isCreated) return false;

    for (UInt32 i = 0; i < info->subDeviceCount; i++)
    {
        if (is_virtual_device(info->subDevices[i]))
        {
            return true;
        }
    }
    return false;
}

bool aggregate_device_contains_physical(const AggregateDeviceInfo* info, AudioDeviceID physicalDevice)
{
    if (info == NULL || !info->isCreated) return false;

    for (UInt32 i = 0; i < info->subDeviceCount; i++)
    {
        if (info->subDevices[i] == physicalDevice)
        {
            return true;
        }
    }
    return false;
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
