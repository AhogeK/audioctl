//
// Created by AhogeK on 12/10/24.
//

#ifndef AUDIOCTL_VIRTUAL_AUDIO_DRIVER_H
#define AUDIOCTL_VIRTUAL_AUDIO_DRIVER_H

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>

// 插件基础标识
#define kPlugIn_BundleID "com.ahogek.VirtualAudioDriver"
// 必须与 Info.plist 中的 CFPlugInFactories 键值完全一致！
#define kVirtualAudioDriverFactoryUUID "115FECAA-C664-4AC1-B322-C9DAF75FB39E"

#define kDevice_UID "0E1D42AE-F2ED-4A48-9624-C770025E32A4"
#define kDevice_ModelUID "56304703-6894-4B97-94A3-B7A551D35150"

// 入口函数声明
void *
AudioServerPlugIn_Initialize (CFAllocatorRef inAllocator,
			      CFUUIDRef inRequestedTypeUUID)
  __attribute__ ((visibility ("default")));

// 驱动接口引用
extern AudioServerPlugInDriverInterface gAudioServerPlugInDriverInterface;
extern AudioServerPlugInDriverInterface *gAudioServerPlugInDriverInterfacePtr;
extern AudioServerPlugInDriverRef gAudioServerPlugInDriverRef;

#endif // AUDIOCTL_VIRTUAL_AUDIO_DRIVER_H
