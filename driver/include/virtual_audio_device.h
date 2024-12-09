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
    IOUserAudioStreamBasicDescription backup_format_;
    IOUserAudioStreamBasicDescription current_format_;

    // 设备状态和锁
    IOLock *config_lock_{nullptr};
    IOUserAudioDriver *audio_driver_{nullptr};
    IOUserAudioStream *output_stream_{nullptr};
    bool is_running_{false};

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
    kern_return_t handle_configuration_error(kern_return_t result, bool was_running);

    kern_return_t handle_configuration_change(uint64_t in_change_action,
                                              const OSObject *in_change_info);

    kern_return_t change_sample_rate(const OSObject *in_change_info);

    kern_return_t change_format(const OSObject *in_change_info);

    kern_return_t change_channels(const OSObject *in_change_info);

    // 流管理方法
    kern_return_t configure_new_stream(IOUserAudioStream *stream) const;

    kern_return_t restore_old_stream(IOUserAudioStream *old_stream,
                                     bool was_active,
                                     const IOUserAudioStreamBasicDescription &old_format);

    // 初始化和配置方法
    bool initialize_audio_format();

    bool configure_device_properties();

    bool is_initialized() const { return config_lock_ != nullptr && audio_driver_ != nullptr; }

    static bool validate_audio_format(const IOUserAudioStreamBasicDescription &format);
};

#endif //AUDIOCTL_VIRTUAL_AUDIO_DEVICE_H
