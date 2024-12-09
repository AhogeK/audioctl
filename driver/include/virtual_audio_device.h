//
// Created by AhogeK on 12/7/24.
//

#ifndef AUDIOCTL_VIRTUAL_AUDIO_DEVICE_H
#define AUDIOCTL_VIRTUAL_AUDIO_DEVICE_H

#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/DriverKit.h>
#include <AudioDriverKit/IOUserAudioDevice.h>

class VirtualAudioDevice final : public IOUserAudioDevice {
public:
    // 引入基类的 init 方法
    using IOUserAudioDevice::init;
    using IOUserAudioObjectInterface::init;
    using OSObject::init;

    // 添加虚拟析构函数
    ~VirtualAudioDevice() = default;

    // 初始化和释放
    bool init(IOUserAudioDriver *driver,
              bool supports_prewarming,
              OSString *device_uid,
              OSString *model_uid,
              OSString *manufacturer_uid,
              uint32_t zero_timestamp_period) override;

    void free() override;

    // 设备生命周期管理
    kern_return_t StartIO(IOUserAudioStartStopFlags flags) override;

    kern_return_t StopIO(IOUserAudioStartStopFlags flags) override;

    // 设备配置管理
    kern_return_t PerformDeviceConfigurationChange(uint64_t change_action,
                                                   OSObject *change_info) override;

    kern_return_t AbortDeviceConfigurationChange(uint64_t change_action,
                                                 OSObject *change_info) override;

    // 采样率管理
    kern_return_t HandleChangeSampleRate(double sample_rate) override;

    // 禁止拷贝和赋值
    VirtualAudioDevice(const VirtualAudioDevice &) = delete;

    VirtualAudioDevice &operator=(const VirtualAudioDevice &) = delete;

private:
    // 音频格式相关
    IOUserAudioStreamBasicDescription backup_format;
    IOUserAudioStreamBasicDescription current_format;

    // 设备状态和锁
    IOLock *config_lock{nullptr};
    IOUserAudioDriver *audioDriver{nullptr};
    IOUserAudioStream *output_stream{nullptr};
    bool is_running{false};

    // 配置变更类型
    enum ConfigurationChangeType {
        kChangeSampleRate = 1,
        kChangeFormat = 2,
        kChangeChannels = 3
    };

    // 内部辅助方法
    kern_return_t create_streams();

    kern_return_t configure_streams();

    // 配置管理方法
    kern_return_t handleConfigurationError(kern_return_t result, bool was_running);

    kern_return_t handleConfigurationChange(uint64_t in_change_action,
                                            const OSObject *in_change_info);

    kern_return_t changeSampleRate(const OSObject *in_change_info);

    kern_return_t changeFormat(const OSObject *in_change_info);

    kern_return_t changeChannels(const OSObject *in_change_info);

    // 流管理方法
    kern_return_t configureNewStream(IOUserAudioStream *stream) const;

    kern_return_t restoreOldStream(IOUserAudioStream *old_stream,
                                   bool was_active,
                                   const IOUserAudioStreamBasicDescription &old_format);

    // 初始化和配置方法
    bool initializeAudioFormat();

    bool configureDeviceProperties();

    bool isInitialized() const { return config_lock != nullptr && audioDriver != nullptr; }

    static bool validateAudioFormat(const IOUserAudioStreamBasicDescription &format);
};

#endif //AUDIOCTL_VIRTUAL_AUDIO_DEVICE_H
