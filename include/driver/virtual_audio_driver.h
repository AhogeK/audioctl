//
// Created by AhogeK on 12/10/24.
//

#ifndef AUDIOCTL_VIRTUAL_AUDIO_DRIVER_H
#define AUDIOCTL_VIRTUAL_AUDIO_DRIVER_H

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>

//==================================================================================================
#pragma mark -
#pragma mark Macros
//==================================================================================================
#if TARGET_RT_BIG_ENDIAN
#define FourCCToCString(the4CC) { ((char*)&the4CC)[0], ((char*)&the4CC)[1], ((char*)&the4CC)[2], ((char*)&the4CC)[3], 0 }
#else
#define FourCCToCString(the4CC) { ((char*)&the4CC)[3], ((char*)&the4CC)[2], ((char*)&the4CC)[1], ((char*)&the4CC)[0], 0 }
#endif

#if DEBUG
#define DebugMsg(inFormat, ...) printf(inFormat "\n", ## __VA_ARGS__)
#define FailIf(inCondition, inHandler, inMessage) \
    do { \
        if(inCondition) { \
            DebugMsg(inMessage); \
            goto inHandler; \
        } \
    } while(0)

#define FailWithAction(inCondition, inAction, inHandler, inMessage) \
    do { \
        if(inCondition) { \
            DebugMsg(inMessage); \
            { inAction; } \
            goto inHandler; \
        } \
    } while(0)

#else
#define DebugMsg(inFormat, ...)
#define FailIf(inCondition, inHandler, inMessage) \
    do { \
        if(inCondition) { \
            goto inHandler; \
        } \
    } while(0)

#define FailWithAction(inCondition, inAction, inHandler, inMessage) \
    do { \
        if(inCondition) { \
            { inAction; } \
            goto inHandler; \
        } \
    } while(0)
#endif

#define kPlugIn_BundleID                "com.ahogek.VirtualAudioDriver"
#define kBox_UID                        "C604CE03-358B-4903-B7B3-45FEC49B689E"
#define kDevice_UID                     "0E1D42AE-F2ED-4A48-9624-C770025E32A4"
#define kDevice_ModelUID                "56304703-6894-4B97-94A3-B7A551D35150"
#define kDataSource_ItemNamePattern     "Data Source Item %d"

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


// 属性操作辅助函数声明
static Boolean VirtualAudioDriver_HasPlugInProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                    pid_t inClientProcessID,
                                                    const AudioObjectPropertyAddress *inAddress);

static Boolean VirtualAudioDriver_HasBoxProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                 pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress);

static Boolean VirtualAudioDriver_HasDeviceProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                    pid_t inClientProcessID,
                                                    const AudioObjectPropertyAddress *inAddress);

static Boolean VirtualAudioDriver_HasStreamProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                    pid_t inClientProcessID,
                                                    const AudioObjectPropertyAddress *inAddress);

static Boolean VirtualAudioDriver_HasControlProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                     pid_t inClientProcessID,
                                                     const AudioObjectPropertyAddress *inAddress);

static OSStatus
VirtualAudioDriver_IsPlugInPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                            pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                            Boolean *outIsSettable);

static OSStatus VirtualAudioDriver_IsBoxPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                         pid_t inClientProcessID,
                                                         const AudioObjectPropertyAddress *inAddress,
                                                         Boolean *outIsSettable);

static OSStatus
VirtualAudioDriver_IsDevicePropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                            pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                            Boolean *outIsSettable);

static OSStatus
VirtualAudioDriver_IsStreamPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                            pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                            Boolean *outIsSettable);

static OSStatus
VirtualAudioDriver_IsControlPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                             pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                             Boolean *outIsSettable);

static OSStatus
VirtualAudioDriver_GetPlugInPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                             pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                             UInt32 inQualifierDataSize, const void *inQualifierData,
                                             UInt32 *outDataSize);

static OSStatus VirtualAudioDriver_GetBoxPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                          pid_t inClientProcessID,
                                                          const AudioObjectPropertyAddress *inAddress,
                                                          UInt32 inQualifierDataSize, const void *inQualifierData,
                                                          UInt32 *outDataSize);

static OSStatus
VirtualAudioDriver_GetDevicePropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                             pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                             UInt32 inQualifierDataSize, const void *inQualifierData,
                                             UInt32 *outDataSize);

static OSStatus
VirtualAudioDriver_GetStreamPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                             pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                             UInt32 inQualifierDataSize, const void *inQualifierData,
                                             UInt32 *outDataSize);

static OSStatus
VirtualAudioDriver_GetControlPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                              pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress,
                                              UInt32 inQualifierDataSize, const void *inQualifierData,
                                              UInt32 *outDataSize);

static OSStatus VirtualAudioDriver_GetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                         pid_t inClientProcessID,
                                                         const AudioObjectPropertyAddress *inAddress,
                                                         UInt32 inQualifierDataSize, const void *inQualifierData,
                                                         UInt32 inDataSize, UInt32 *outDataSize, void *outData);

static OSStatus VirtualAudioDriver_GetBoxPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                      pid_t inClientProcessID,
                                                      const AudioObjectPropertyAddress *inAddress,
                                                      UInt32 inQualifierDataSize, const void *inQualifierData,
                                                      UInt32 inDataSize, UInt32 *outDataSize, void *outData);

static OSStatus VirtualAudioDriver_GetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                         pid_t inClientProcessID,
                                                         const AudioObjectPropertyAddress *inAddress,
                                                         UInt32 inQualifierDataSize, const void *inQualifierData,
                                                         UInt32 inDataSize, UInt32 *outDataSize, void *outData);

static OSStatus VirtualAudioDriver_GetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                         pid_t inClientProcessID,
                                                         const AudioObjectPropertyAddress *inAddress,
                                                         UInt32 inQualifierDataSize, const void *inQualifierData,
                                                         UInt32 inDataSize, UInt32 *outDataSize, void *outData);

static OSStatus VirtualAudioDriver_GetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                                          pid_t inClientProcessID,
                                                          const AudioObjectPropertyAddress *inAddress,
                                                          UInt32 inQualifierDataSize, const void *inQualifierData,
                                                          UInt32 inDataSize, UInt32 *outDataSize, void *outData);

#endif //AUDIOCTL_VIRTUAL_AUDIO_DRIVER_H
