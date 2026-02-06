//
// Aggregate Device 管理模块实现 - 稳健 4 通道版
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

static OSStatus get_all_devices(AudioDeviceID** devices, UInt32* count)
{
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize);
    if (status != noErr) return status;
    *count = dataSize / sizeof(AudioDeviceID);
    *devices = (AudioDeviceID*)malloc(dataSize);
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize, *devices);
    return status;
}

static OSStatus get_device_uid(AudioDeviceID deviceId, char* uid, size_t uidSize)
{
    CFStringRef uidRef = NULL;
    UInt32 dataSize = sizeof(CFStringRef);
    AudioObjectPropertyAddress propertyAddress = {
        kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectGetPropertyData(deviceId, &propertyAddress, 0, NULL, &dataSize, &uidRef);
    if (status != noErr || uidRef == NULL) return status;
    CFStringGetCString(uidRef, uid, (CFIndex)uidSize, kCFStringEncodingUTF8);
    CFRelease(uidRef);
    return noErr;
}

static OSStatus get_device_name(AudioDeviceID deviceId, char* name, size_t nameSize)
{
    CFStringRef nameRef = NULL;
    UInt32 dataSize = sizeof(CFStringRef);
    AudioObjectPropertyAddress propertyAddress = {
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectGetPropertyData(deviceId, &propertyAddress, 0, NULL, &dataSize, &nameRef);
    if (status == noErr && nameRef)
    {
        CFStringGetCString(nameRef, name, (CFIndex)nameSize, kCFStringEncodingUTF8);
        CFRelease(nameRef);
        return noErr;
    }
    return status;
}

static bool is_virtual_device(AudioDeviceID deviceId)
{
    char uid[256] = {0};
    get_device_uid(deviceId, uid, sizeof(uid));
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
    if (get_all_devices(&devices, &count) != noErr) return kAudioObjectUnknown;
    AudioDeviceID agg = kAudioObjectUnknown;
    for (UInt32 i = 0; i < count; i++)
    {
        if (!is_aggregate_device(devices[i])) continue;
        char name[256] = {0};
        get_device_name(devices[i], name, sizeof(name));
        if (strstr(name, "audioctl Aggregate"))
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
    if (!virtual_device_get_info(&vInfo)) return -1;
    if (physicalDeviceID == kAudioObjectUnknown) physicalDeviceID = aggregate_device_get_recommended_physical_device();
    if (physicalDeviceID == kAudioObjectUnknown) return -1;

    aggregate_device_destroy();

    char uid[256];
    snprintf(uid, sizeof(uid), "%s-%d", AGGREGATE_DEVICE_UID_PREFIX, (int)getpid());
    CFStringRef uidRef = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    CFStringRef nameRef = CFStringCreateWithCString(NULL, AGGREGATE_DEVICE_NAME, kCFStringEncodingUTF8);

    char vUID[256], pUID[256];
    get_device_uid(vInfo.deviceId, vUID, sizeof(vUID));
    get_device_uid(physicalDeviceID, pUID, sizeof(pUID));
    CFStringRef vUIDRef = CFStringCreateWithCString(NULL, vUID, kCFStringEncodingUTF8);
    CFStringRef pUIDRef = CFStringCreateWithCString(NULL, pUID, kCFStringEncodingUTF8);

    CFMutableArrayRef sublist = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    int ch2 = 2, drift1 = 1, drift0 = 0;
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

    CFMutableDictionaryRef desc = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), uidRef);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), nameRef);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), sublist);
    CFDictionarySetValue(desc, CFSTR("master"), pUIDRef);

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

    if (status == noErr) printf("✅ Aggregate Device 已恢复 4 通道布局\n");
    return status;
}

OSStatus aggregate_device_destroy(void)
{
    AudioDeviceID agg = find_aggregate_device();
    if (agg != kAudioObjectUnknown) return AudioHardwareDestroyAggregateDevice(agg);
    return noErr;
}

OSStatus aggregate_device_activate(void)
{
    if (!aggregate_device_is_created()) aggregate_device_create(kAudioObjectUnknown);
    AudioDeviceID agg = find_aggregate_device();
    if (agg == kAudioObjectUnknown) return -1;

    AudioObjectPropertyAddress outAddr = {
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectSetPropertyData(kAudioObjectSystemObject, &outAddr, 0, NULL, sizeof(AudioDeviceID),
                                                 &agg);

    if (status == noErr)
    {
        char physName[256] = "未知物理设备";
        AggregateDeviceInfo info;
        if (aggregate_device_get_info(&info))
        {
            for (UInt32 i = 0; i < info.subDeviceCount; i++)
            {
                if (!is_virtual_device(info.subDevices[i]))
                {
                    get_device_name(info.subDevices[i], physName, sizeof(physName));
                    break;
                }
            }
        }
        printf("✅ Aggregate Device 已设为默认输出\n");
        printf("   音频流: 应用 → 虚拟设备(音量控制) → %s\n", physName);
    }
    return status;
}

OSStatus aggregate_device_deactivate(void)
{
    AudioDeviceID physical = aggregate_device_get_recommended_physical_device();
    AudioObjectPropertyAddress outAddr = {
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    return AudioObjectSetPropertyData(kAudioObjectSystemObject, &outAddr, 0, NULL, sizeof(AudioDeviceID), &physical);
}

bool aggregate_device_is_created(void) { return find_aggregate_device() != kAudioObjectUnknown; }

bool aggregate_device_is_active(void)
{
    AudioDeviceID agg = find_aggregate_device();
    AudioDeviceID def;
    UInt32 sz = sizeof(def);
    AudioObjectPropertyAddress outAddr = {
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &outAddr, 0, NULL, &sz, &def) != noErr) return false;
    return (agg != kAudioObjectUnknown && agg == def);
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
    AudioObjectGetPropertyDataSize(agg, &subAddr, 0, NULL, &sz);
    outInfo->subDeviceCount = sz / sizeof(AudioDeviceID);
    AudioObjectGetPropertyData(agg, &subAddr, 0, NULL, &sz, outInfo->subDevices);
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
    printf("✅ Aggregate Device 已就绪 (稳健模式)\n");
}

AudioDeviceID aggregate_device_get_recommended_physical_device(void)
{
    AudioDeviceID* devices = NULL;
    UInt32 count = 0;
    get_all_devices(&devices, &count);
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