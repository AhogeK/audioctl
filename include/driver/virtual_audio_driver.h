//
// Created by AhogeK on 12/10/24.
//

#ifndef AUDIOCTL_VIRTUAL_AUDIO_DRIVER_H
#define AUDIOCTL_VIRTUAL_AUDIO_DRIVER_H

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>

// 基本的驱动接口结构体声明
extern AudioServerPlugInDriverInterface gAudioServerPlugInDriverInterface;
extern AudioServerPlugInDriverInterface *gAudioServerPlugInDriverInterfacePtr;
extern AudioServerPlugInDriverRef gAudioServerPlugInDriverRef;

#pragma mark Prototypes

// 插件入口点
void *VirtualAudioDriver_Create(CFAllocatorRef inAllocator, CFUUIDRef requestedTypeUUID);


// COM 接口函数声明
static HRESULT VirtualAudioDriver_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);

static ULONG VirtualAudioDriver_AddRef(void *inDriver);

static ULONG VirtualAudioDriver_Release(void *inDriver);


// 基本操作函数声明
static OSStatus VirtualAudioDriver_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);

static OSStatus VirtualAudioDriver_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription,
                                                const AudioServerPlugInClientInfo *inClientInfo,
                                                AudioObjectID *outDeviceObjectID);

static OSStatus VirtualAudioDriver_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);

static OSStatus VirtualAudioDriver_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                   const AudioServerPlugInClientInfo *inClientInfo);

static OSStatus
VirtualAudioDriver_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                      const AudioServerPlugInClientInfo *inClientInfo);

static OSStatus
VirtualAudioDriver_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                    UInt64 inChangeAction, void *inChangeInfo);

static OSStatus
VirtualAudioDriver_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                  UInt64 inChangeAction, void *inChangeInfo);


// 属性操作函数声明
static Boolean
VirtualAudioDriver_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID,
                               const AudioObjectPropertyAddress *inAddress);

static OSStatus VirtualAudioDriver_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                      pid_t inClientProcessID,
                                                      const AudioObjectPropertyAddress *inAddress,
                                                      Boolean *outIsSettable);

static OSStatus VirtualAudioDriver_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                       pid_t inClientProcessID,
                                                       const AudioObjectPropertyAddress *inAddress,
                                                       UInt32 inQualifierDataSize, const void *inQualifierData,
                                                       UInt32 *outDataSize);

static OSStatus VirtualAudioDriver_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                   pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                                   UInt32 inQualifierDataSize, const void *inQualifierData,
                                                   UInt32 inDataSize, UInt32 *outDataSize, void *outData);

static OSStatus VirtualAudioDriver_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                   pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                                   UInt32 inQualifierDataSize, const void *inQualifierData,
                                                   UInt32 inDataSize, const void *inData);


// IO 操作函数声明
static OSStatus
VirtualAudioDriver_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);

static OSStatus
VirtualAudioDriver_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);

static OSStatus VirtualAudioDriver_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                    UInt32 inClientID, Float64 *outSampleTime, UInt64 *outHostTime,
                                                    UInt64 *outSeed);

static OSStatus
VirtualAudioDriver_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                     UInt32 inClientID, UInt32 inOperationID, Boolean *outWillDo,
                                     Boolean *outWillDoInPlace);

static OSStatus VirtualAudioDriver_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                    UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                                    const AudioServerPlugInIOCycleInfo *inIOCycleInfo);

static OSStatus VirtualAudioDriver_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                 AudioObjectID inStreamObjectID, UInt32 inClientID,
                                                 UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                                 const AudioServerPlugInIOCycleInfo *inIOCycleInfo, void *ioMainBuffer,
                                                 void *ioSecondaryBuffer);

static OSStatus VirtualAudioDriver_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                  UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                                  const AudioServerPlugInIOCycleInfo *inIOCycleInfo);


#endif //AUDIOCTL_VIRTUAL_AUDIO_DRIVER_H