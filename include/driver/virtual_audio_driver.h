//
// Created by AhogeK on 12/10/24.
//

#ifndef AUDIOCTL_VIRTUAL_AUDIO_DRIVER_H
#define AUDIOCTL_VIRTUAL_AUDIO_DRIVER_H

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>

#define kPlugIn_BundleID                "com.ahogek.VirtualAudioDriver"
#define kBox_UID                        "C604CE03-358B-4903-B7B3-45FEC49B689E"
#define kDevice_UID                     "0E1D42AE-F2ED-4A48-9624-C770025E32A4"
#define kDevice_ModelUID                "56304703-6894-4B97-94A3-B7A551D35150"

// 插件工厂函数声明
void* VirtualAudioDriver_Create(CFAllocatorRef inAllocator, CFUUIDRef requestedTypeUUID);

// 驱动接口引用声明
extern AudioServerPlugInDriverInterface gAudioServerPlugInDriverInterface;
extern AudioServerPlugInDriverInterface* gAudioServerPlugInDriverInterfacePtr;
extern AudioServerPlugInDriverRef gAudioServerPlugInDriverRef;

#endif //AUDIOCTL_VIRTUAL_AUDIO_DRIVER_H