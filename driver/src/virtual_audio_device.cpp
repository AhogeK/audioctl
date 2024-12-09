//
// Created by AhogeK on 12/7/24.
//

#include "virtual_audio_device.h"
#include <os/log.h>
#include <CoreAudioTypes/CoreAudioTypes.h>

// Using macro here because os_log requires string literals for format strings
// and string concatenation needs to happen during preprocessing
#define LOG_PREFIX "[VirtualAudioDevice] "

bool VirtualAudioDevice::init(IOUserAudioDriver *driver, bool supports_prewarming,
                              OSString *device_uid, OSString *model_uid,
                              OSString *manufacturer_uid, uint32_t zero_timestamp_period) {
    // 用于跟踪初始化进度的标志
    bool super_init_done = false;
    bool lock_allocated = false;

    // 调用父类初始化
    if (!IOUserAudioDevice::init(driver, supports_prewarming, device_uid,
                                 model_uid, manufacturer_uid,
                                 zero_timestamp_period)) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Super class initialization failed");
        goto cleanup;
    }
    super_init_done = true;

    // 初始化互斥锁
    config_lock_ = IOLockAlloc();
    if (!config_lock_) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to allocate config lock");
        goto cleanup;
    }
    lock_allocated = true;

    // 验证驱动实例
    if (!driver) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Invalid driver instance");
        goto cleanup;
    }
    audio_driver_ = driver;

    // 初始化指针和状态
    output_stream_ = nullptr;
    is_running_ = false;

    // 初始化基本的音频格式
    if (!initialize_audio_format()) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to initialize audio format");
        goto cleanup;
    }

    // 配置设备基本属性
    if (!configure_device_properties()) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to configure device properties");
        goto cleanup;
    }

    // 记录初始化成功的日志
    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Initializing device with format: %d Hz, %d channels, %d bits",
           static_cast<int>(current_format_.mSampleRate),
           current_format_.mChannelsPerFrame,
           current_format_.mBitsPerChannel);

    return true;

    cleanup:
    // 清理资源（按照与分配相反的顺序）
    if (lock_allocated && config_lock_) {  // 合并两个 if 语句
        IOLockFree(config_lock_);
        config_lock_ = nullptr;
    }

    // 如果父类初始化成功但后续步骤失败，需要调用父类的 free
    if (super_init_done) {
        IOUserAudioDevice::free();
    }

    // 重置所有成员变量到安全状态
    audio_driver_ = nullptr;
    output_stream_ = nullptr;
    is_running_ = false;
    current_format_ = IOUserAudioStreamBasicDescription();
    backup_format_ = IOUserAudioStreamBasicDescription();

    return false;
}

bool VirtualAudioDevice::initialize_audio_format() {
    current_format_.mSampleRate = 48000.0;  // 采样率 48kHz
    current_format_.mFormatID = IOUserAudioFormatID::LinearPCM;
    current_format_.mFormatFlags = IOUserAudioFormatFlags::FormatFlagsNativeFloatPacked;
    current_format_.mBytesPerPacket = 8;    // 每数据包字节数 (2通道 * 4字节)
    current_format_.mFramesPerPacket = 1;    // 每数据包帧数
    current_format_.mBytesPerFrame = 8;      // 每帧字节数 (2通道 * 4字节)
    current_format_.mChannelsPerFrame = 2;   // 双声道
    current_format_.mBitsPerChannel = 32;    // 32位每声道

    // 验证格式是否有效
    if (!validate_audio_format(current_format_)) {
        return false;
    }

    backup_format_ = current_format_;  // 初始化备份格式
    return true;
}

// 配置设备属性
bool VirtualAudioDevice::configure_device_properties() {
    kern_return_t result;

    result = SetCanBeDefaultOutputDevice(true);
    if (result != kIOReturnSuccess) {
        return false;
    }

    result = SetCanBeDefaultSystemOutputDevice(true);
    if (result != kIOReturnSuccess) {
        return false;
    }

    return true;
}

void VirtualAudioDevice::free() {
    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Beginning device cleanup");

    // 首先获取锁，确保清理过程的原子性
    if (config_lock_) {
        IOLockLock(config_lock_);
    }

    // 停止所有IO操作
    // 即使获取锁失败也要尝试停止IO，因为这是关键的清理步骤
    if (is_running_) {
        if (kern_return_t result = StopIO(IOUserAudioStartStopFlags::None); result != kIOReturnSuccess) {
            os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to stop IO during cleanup: %d", result);
        }
        is_running_ = false;
    }

    // 清理输出流
    if (output_stream_) {
        // 首先停用流
        kern_return_t result = output_stream_->SetStreamIsActive(false);
        if (result != kIOReturnSuccess) {
            os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to deactivate stream during cleanup: %d", result);
        }

        // 从设备中移除流
        result = RemoveStream(output_stream_);
        if (result != kIOReturnSuccess) {
            os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to remove stream during cleanup: %d", result);
        }

        // 安全释放流对象
        OSSafeReleaseNULL(output_stream_);
    }

    // 清理音频格式
    current_format_ = IOUserAudioStreamBasicDescription();  // 重置为默认值
    backup_format_ = IOUserAudioStreamBasicDescription();  // 重置为默认值

    // 清理驱动实例引用
    if (audio_driver_) {
        audio_driver_ = nullptr;  // 外部传入的实例，不需要释放
    }

    // 释放锁
    // 确保在最后清理锁，这样其他可能的清理操作都在锁的保护下完成
    if (config_lock_) {
        IOLockUnlock(config_lock_);
        IOLockFree(config_lock_);
        config_lock_ = nullptr;
    }

    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Device cleanup completed");

    // 调用父类的释放方法
    IOUserAudioDevice::free();
}

kern_return_t VirtualAudioDevice::StartIO(AudioDriverKit::IOUserAudioStartStopFlags flags) {
    if (!is_initialized()) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Device not properly initialized");
        return kIOReturnNotReady;
    }

    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Starting IO with flags: %u", flags);

    IOLockLock(config_lock_);

    // 首先检查当前状态
    if (is_running_) {
        os_log(OS_LOG_DEFAULT, LOG_PREFIX "IO is already running");
        IOLockUnlock(config_lock_);
        return kIOReturnSuccess;
    }

    // 调用父类的 StartIO 方法
    kern_return_t result = IOUserAudioDevice::StartIO(flags);
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to start super class: %d", result);
        IOLockUnlock(config_lock_);
        return result;
    }

    // 确保输出流存在且处于活动状态
    if (!output_stream_) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "No output stream available");
        IOLockUnlock(config_lock_);
        return kIOReturnError;
    }

    // 激活输出流
    result = output_stream_->SetStreamIsActive(true);
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to activate output stream: %d", result);
        // 回滚父类的 StartIO
        IOUserAudioDevice::StopIO(flags);
        IOLockUnlock(config_lock_);
        return result;
    }

    // 更新运行状态
    is_running_ = true;

    IOLockUnlock(config_lock_);
    os_log(OS_LOG_DEFAULT, LOG_PREFIX "IO started successfully");
    return kIOReturnSuccess;
}

kern_return_t VirtualAudioDevice::StopIO(AudioDriverKit::IOUserAudioStartStopFlags flags) {
    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Stopping IO with flags: %u", flags);

    IOLockLock(config_lock_);

    // 首先检查当前状态
    if (!is_running_) {
        os_log(OS_LOG_DEFAULT, LOG_PREFIX "IO is already stopped");
        IOLockUnlock(config_lock_);
        return kIOReturnSuccess;
    }

    // 停用输出流
    if (output_stream_) {
        kern_return_t stream_result = output_stream_->SetStreamIsActive(false);
        if (stream_result != kIOReturnSuccess) {
            os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to deactivate output stream: %d", stream_result);
            // 继续执行，尝试停止父类的 IO
        }
    }

    // 更新运行状态（在调用父类方法之前）
    is_running_ = false;

    // 调用父类的 StopIO 方法
    if (kern_return_t result = IOUserAudioDevice::StopIO(flags); result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to stop super class: %d", result);
        // 如果父类停止失败，恢复运行状态
        is_running_ = true;
        if (output_stream_) {
            output_stream_->SetStreamIsActive(true);
        }
        IOLockUnlock(config_lock_);
        return result;
    }

    IOLockUnlock(config_lock_);
    os_log(OS_LOG_DEFAULT, LOG_PREFIX "IO stopped successfully");
    return kIOReturnSuccess;
}

kern_return_t
VirtualAudioDevice::PerformDeviceConfigurationChange(uint64_t in_change_action, OSObject *in_change_info) {
    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Beginning configuration change action: %llu", in_change_action);

    IOLockLock(config_lock_);

    const bool was_running = is_running_;
    backup_format_ = current_format_;
    kern_return_t result;

    result = IOUserAudioDevice::PerformDeviceConfigurationChange(in_change_action, in_change_info);
    if (result != kIOReturnSuccess) {
        return handle_configuration_error(result, was_running);
    }

    if (was_running) {
        result = StopIO(IOUserAudioStartStopFlags::None);
        if (result != kIOReturnSuccess) {
            return handle_configuration_error(result, was_running);
        }
    }

    result = handle_configuration_change(in_change_action, in_change_info);
    if (result != kIOReturnSuccess) {
        return handle_configuration_error(result, was_running);
    }

    result = configure_streams();
    if (result != kIOReturnSuccess) {
        return handle_configuration_error(result, was_running);
    }

    if (was_running) {
        result = StartIO(IOUserAudioStartStopFlags::None);
        if (result != kIOReturnSuccess) {
            return handle_configuration_error(result, was_running);
        }
    }

    IOLockUnlock(config_lock_);
    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Configuration change completed successfully");
    return kIOReturnSuccess;
}

kern_return_t VirtualAudioDevice::handle_configuration_error(kern_return_t result, bool was_running) {
    os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Configuration change error: %d", result);
    current_format_ = backup_format_;
    if (was_running) {
        StartIO(IOUserAudioStartStopFlags::None);
    }
    IOLockUnlock(config_lock_);
    return result;
}

kern_return_t VirtualAudioDevice::handle_configuration_change(uint64_t in_change_action, const OSObject *in_change_info) {
    switch (in_change_action) {
        case kChangeSampleRate:
            return change_sample_rate(in_change_info);
        case kChangeFormat:
            return change_format(in_change_info);
        case kChangeChannels:
            return change_channels(in_change_info);
        default:
            os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Unknown configuration change action: %llu", in_change_action);
            return kIOReturnUnsupported;
    }
}

kern_return_t VirtualAudioDevice::change_sample_rate(const OSObject *in_change_info) {
    if (!is_initialized()) {
        return kIOReturnNotReady;
    }

    const auto *sampleRate = OSDynamicCast(OSNumber, in_change_info);
    if (!sampleRate) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Invalid sample rate change info");
        return kIOReturnBadArgument;
    }

    // 创建临时格式进行验证
    IOUserAudioStreamBasicDescription tempFormat = current_format_;
    tempFormat.mSampleRate = static_cast<double>(sampleRate->unsigned64BitValue());

    if (!validate_audio_format(tempFormat)) {
        return kIOReturnUnsupported;
    }

    current_format_.mSampleRate = tempFormat.mSampleRate;
    return kIOReturnSuccess;
}

kern_return_t VirtualAudioDevice::change_format(const OSObject *in_change_info) {
    const auto *format = OSDynamicCast(OSNumber, in_change_info);
    if (!format) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Invalid format change info");
        return kIOReturnBadArgument;
    }

    auto newFormat = static_cast<IOUserAudioFormatID>(format->unsigned32BitValue());
    if (newFormat != IOUserAudioFormatID::LinearPCM) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Unsupported format: %u", static_cast<uint32_t>(newFormat));
        return kIOReturnUnsupported;
    }

    current_format_.mFormatID = newFormat;
    return kIOReturnSuccess;
}

kern_return_t VirtualAudioDevice::change_channels(const OSObject *in_change_info) {
    const auto *channels = OSDynamicCast(OSNumber, in_change_info);
    if (!channels) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Invalid channels change info");
        return kIOReturnBadArgument;
    }

    uint32_t newChannels = channels->unsigned32BitValue();
    if (newChannels < 1 || newChannels > 8) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Unsupported channel count: %u", newChannels);
        return kIOReturnUnsupported;
    }

    current_format_.mChannelsPerFrame = newChannels;
    current_format_.mBytesPerFrame = newChannels * (current_format_.mBitsPerChannel / 8);
    current_format_.mBytesPerPacket = current_format_.mBytesPerFrame * current_format_.mFramesPerPacket;
    return kIOReturnSuccess;
}

kern_return_t VirtualAudioDevice::AbortDeviceConfigurationChange(uint64_t change_action, OSObject *change_info) {
    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Aborting configuration change action: %llu", change_action);

    IOLockLock(config_lock_);

    // 备份当前状态
    const bool was_running = is_running_;
    kern_return_t result;

    // 首先调用父类的实现
    result = IOUserAudioDevice::AbortDeviceConfigurationChange(change_action, change_info);
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Super class AbortDeviceConfigurationChange failed: %d", result);
        IOLockUnlock(config_lock_);
        return result;
    }

    // 恢复到备份的格式
    current_format_ = backup_format_;

    // 根据配置变更类型处理中止操作
    switch (change_action) {
        case kChangeSampleRate:
            os_log(OS_LOG_DEFAULT, LOG_PREFIX "Aborting sample rate change");
            break;

        case kChangeFormat:
            os_log(OS_LOG_DEFAULT, LOG_PREFIX "Aborting format change");
            break;

        case kChangeChannels:
            os_log(OS_LOG_DEFAULT, LOG_PREFIX "Aborting channels change");
            break;

        default:
            os_log(OS_LOG_DEFAULT, LOG_PREFIX "Aborting unknown configuration change: %llu", change_action);
            break;
    }

    // 重新配置流以恢复原始状态
    result = configure_streams();
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX
                "Failed to reconfigure streams after aborting configuration change: %d", result);
        // 尝试恢复运行状态
        if (was_running && !is_running_) {
            kern_return_t start_result = StartIO(IOUserAudioStartStopFlags::None);
            if (start_result != kIOReturnSuccess) {
                os_log_error(OS_LOG_DEFAULT, LOG_PREFIX
                        "Failed to restore running state during error recovery: %d", start_result);
            }
        }
        IOLockUnlock(config_lock_);
        return result;
    }

    // 恢复运行状态
    if (was_running != is_running_) {
        if (was_running) {
            // 如果之前在运行但现在停止了，需要重新启动
            result = StartIO(IOUserAudioStartStopFlags::None);
            if (result != kIOReturnSuccess) {
                os_log_error(OS_LOG_DEFAULT, LOG_PREFIX
                        "Failed to restart IO after aborting configuration change: %d", result);
                IOLockUnlock(config_lock_);
                return result;
            }
            os_log(OS_LOG_DEFAULT, LOG_PREFIX "Successfully restored running state");
        } else {
            // 如果之前没有运行但现在在运行，需要停止
            result = StopIO(IOUserAudioStartStopFlags::None);
            if (result != kIOReturnSuccess) {
                os_log_error(OS_LOG_DEFAULT, LOG_PREFIX
                        "Failed to stop IO after aborting configuration change: %d", result);
                IOLockUnlock(config_lock_);
                return result;
            }
            os_log(OS_LOG_DEFAULT, LOG_PREFIX "Successfully restored stopped state");
        }
    }

    IOLockUnlock(config_lock_);
    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Configuration change abort completed successfully");
    return kIOReturnSuccess;
}

kern_return_t VirtualAudioDevice::HandleChangeSampleRate(double sample_rate) {
    IOLockLock(config_lock_);

    // 首先调用父类的实现
    kern_return_t result = IOUserAudioDevice::HandleChangeSampleRate(sample_rate);
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Super class HandleChangeSampleRate failed: %d", result);
        IOLockUnlock(config_lock_);
        return result;
    }

    // 验证采样率
    if (sample_rate != 44100.0 && sample_rate != 48000.0 &&
        sample_rate != 88200.0 && sample_rate != 96000.0) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Unsupported sample rate: %f", sample_rate);
        IOLockUnlock(config_lock_);
        return kIOReturnUnsupported;
    }

    // 创建采样率变更请求
    uint64_t sampleRateInt;
    memcpy(&sampleRateInt, &sample_rate, sizeof(double));

    OSNumber *sampleRateNumber = OSNumber::withNumber(sampleRateInt, 64);
    if (!sampleRateNumber) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to create sample rate number object");
        IOLockUnlock(config_lock_);
        return kIOReturnNoMemory;
    }

    // 请求配置变更
    result = RequestDeviceConfigurationChange(kChangeSampleRate, sampleRateNumber);

    // 释放创建的对象
    sampleRateNumber->release();

    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to request sample rate change: %d", result);
        IOLockUnlock(config_lock_);
        return result;
    }

    IOLockUnlock(config_lock_);
    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Successfully requested sample rate change to %f Hz", sample_rate);
    return kIOReturnSuccess;
}

kern_return_t VirtualAudioDevice::create_streams() {
    IOLockLock(config_lock_);

    // 保存当前状态，用于错误恢复
    IOUserAudioStream *old_stream = output_stream_;
    IOUserAudioStreamBasicDescription old_format = current_format_;
    bool was_active = false;

    if (old_stream) {
        // 保存旧流的激活状态
        was_active = old_stream->GetStreamIsActive();
        // 暂时停用旧流
        old_stream->SetStreamIsActive(false);
    }

    kern_return_t result;
    IOBufferMemoryDescriptor *rawMemory = nullptr;
    OSSharedPtr<IOBufferMemoryDescriptor> outputMemory;
    OSSharedPtr<IOUserAudioStream> stream;

    // 计算音频缓冲区大小
    const uint32_t kBufferDurationMs = 10; // 10ms 缓冲区

    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Creating output stream with buffer duration: %u ms", kBufferDurationMs);

    auto samplesPerBuffer = static_cast<uint32_t>(
            (current_format_.mSampleRate * kBufferDurationMs) / 1000
    );

    const size_t bufferSize = samplesPerBuffer *
                              current_format_.mChannelsPerFrame *
                              (current_format_.mBitsPerChannel / 8);

    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Calculated buffer size: %zu bytes", bufferSize);

    // 创建输出流内存描述符
    result = IOBufferMemoryDescriptor::Create(
            kIOMemoryDirectionInOut,
            bufferSize,
            0,
            &rawMemory
    );

    if (result != kIOReturnSuccess || !rawMemory) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to create output memory descriptor: %d", result);
        goto cleanup;
    }

    outputMemory.reset(rawMemory, OSNoRetain);
    rawMemory = nullptr;

    stream = IOUserAudioStream::Create(
            audio_driver_,
            IOUserAudioStreamDirection::Output,
            outputMemory.get()
    );

    if (!stream) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to create output stream");
        result = kIOReturnError;
        goto cleanup;
    }

    // 配置新流
    result = configure_new_stream(stream.get());
    if (result != kIOReturnSuccess) {
        goto cleanup;
    }

    // 如果有旧流，先移除它
    if (old_stream) {
        RemoveStream(old_stream);
        OSSafeReleaseNULL(old_stream);
    }

    // 添加新流到设备
    result = AddStream(stream.get());
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to add stream to device");
        goto cleanup;
    }

    // 设置流为激活状态（如果之前的流是激活的）
    if (was_active) {
        result = stream->SetStreamIsActive(true);
        if (result != kIOReturnSuccess) {
            os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to activate stream");
            RemoveStream(stream.get());
            goto cleanup;
        }
    }

    // 所有操作成功，存储流引用
    output_stream_ = stream.get();
    stream.detach(); // 转移所有权给 output_stream_

    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Stream created and configured successfully");
    IOLockUnlock(config_lock_);
    return kIOReturnSuccess;

    cleanup:
    // 错误恢复
    if (rawMemory) {
        rawMemory->release();
    }

    // 如果创建新流失败，恢复旧流
    if (old_stream) {
        // 尝试恢复旧流
        kern_return_t restore_result = restore_old_stream(old_stream, was_active, old_format);
        if (restore_result != kIOReturnSuccess) {
            os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to restore old stream: %d", restore_result);
            // 即使恢复失败也继续，因为我们需要返回原始错误
        }
    }

    IOLockUnlock(config_lock_);
    return result != kIOReturnSuccess ? result : kIOReturnError;
}

// 用于配置新流
kern_return_t VirtualAudioDevice::configure_new_stream(IOUserAudioStream *stream) const {
    kern_return_t result;

    // 设置流属性
    result = stream->SetStartingChannel(1);
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to set starting channel");
        return result;
    }

    // 设置可用格式
    result = stream->SetAvailableStreamFormats(&current_format_, 1);
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to set available formats");
        return result;
    }

    // 设置当前格式
    result = stream->SetCurrentStreamFormat(&current_format_);
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to set current format");
        return result;
    }

    // 设置终端类型
    result = stream->SetTerminalType(IOUserAudioStreamTerminalType::Line);
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to set terminal type");
        return result;
    }

    return kIOReturnSuccess;
}

// 用于恢复旧流
kern_return_t VirtualAudioDevice::restore_old_stream(IOUserAudioStream *old_stream,
                                                     bool was_active,
                                                     const IOUserAudioStreamBasicDescription &old_format) {
    kern_return_t result;

    // 恢复格式
    current_format_ = old_format;

    // 重新添加旧流
    result = AddStream(old_stream);
    if (result != kIOReturnSuccess) {
        return result;
    }

    // 如果之前是激活状态，恢复激活状态
    if (was_active) {
        result = old_stream->SetStreamIsActive(true);
        if (result != kIOReturnSuccess) {
            RemoveStream(old_stream);
            return result;
        }
    }

    // 恢复流引用
    output_stream_ = old_stream;

    return kIOReturnSuccess;
}

kern_return_t VirtualAudioDevice::configure_streams() {
    if (!is_initialized()) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Device not properly initialized");
        return kIOReturnNotReady;
    }

    IOLockLock(config_lock_);

    // 添加格式验证
    if (!validate_audio_format(current_format_)) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Invalid audio format configuration");
        IOLockUnlock(config_lock_);
        return kIOReturnBadArgument;
    }

    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Configuring stream with format: %d Hz, %d channels, %d bits, flags: 0x%x",
           static_cast<int>(current_format_.mSampleRate),
           current_format_.mChannelsPerFrame,
           current_format_.mBitsPerChannel,
           current_format_.mFormatFlags);

    kern_return_t result = kIOReturnSuccess;

    // 检查是否存在输出流
    if (!output_stream_) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "No output stream available to configure");
        IOLockUnlock(config_lock_);
        return kIOReturnError;
    }

    // 验证音频格式
    if (current_format_.mBitsPerChannel != 32) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Unsupported bits per channel: %u",
                     current_format_.mBitsPerChannel);
        IOLockUnlock(config_lock_);
        return kIOReturnUnsupported;
    }

    if (current_format_.mFramesPerPacket != 1) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Unsupported frames per packet: %u",
                     current_format_.mFramesPerPacket);
        IOLockUnlock(config_lock_);
        return kIOReturnUnsupported;
    }

    // 验证通道数
    if (current_format_.mChannelsPerFrame != 2) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Unsupported channels per frame: %u",
                     current_format_.mChannelsPerFrame);
        IOLockUnlock(config_lock_);
        return kIOReturnUnsupported;
    }

    // 验证采样率
    if (current_format_.mSampleRate != 44100.0 &&
        current_format_.mSampleRate != 48000.0 &&
        current_format_.mSampleRate != 88200.0 &&
        current_format_.mSampleRate != 96000.0) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Unsupported sample rate: %f",
                     current_format_.mSampleRate);
        IOLockUnlock(config_lock_);
        return kIOReturnUnsupported;
    }

    // 验证格式标志
    if (!(current_format_.mFormatFlags & IOUserAudioFormatFlags::FormatFlagsNativeFloatPacked)) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Unsupported format flags: %u",
                     current_format_.mFormatFlags);
        IOLockUnlock(config_lock_);
        return kIOReturnUnsupported;
    }

    // 设置输出安全偏移
    result = SetOutputSafetyOffset(512);
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to set output safety offset: %d", result);
        IOLockUnlock(config_lock_);
        return result;
    }

    // 设置首选立体声通道
    result = SetPreferredChannelsForStereo(0, 1);
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to set preferred stereo channels: %d", result);
        IOLockUnlock(config_lock_);
        return result;
    }

    // 配置输出流
    result = output_stream_->SetAvailableStreamFormats(&current_format_, 1);
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to set available formats for output stream: %d", result);
        IOLockUnlock(config_lock_);
        return result;
    }

    result = output_stream_->SetCurrentStreamFormat(&current_format_);
    if (result != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Failed to set current format for output stream: %d", result);
        IOLockUnlock(config_lock_);
        return result;
    }

    IOLockUnlock(config_lock_);
    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Stream configuration completed successfully");
    return result;
}

bool VirtualAudioDevice::validate_audio_format(const IOUserAudioStreamBasicDescription &format) {
    // 验证采样率
    if (format.mSampleRate != 44100.0 &&
        format.mSampleRate != 48000.0 &&
        format.mSampleRate != 88200.0 &&
        format.mSampleRate != 96000.0) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Invalid sample rate: %f", format.mSampleRate);
        return false;
    }

    // 验证基本参数
    if (format.mBitsPerChannel != 32) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Invalid bits per channel: %u",
                     format.mBitsPerChannel);
        return false;
    }

    if (format.mFramesPerPacket != 1) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Invalid frames per packet: %u",
                     format.mFramesPerPacket);
        return false;
    }

    if (format.mChannelsPerFrame != 2) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Invalid channels per frame: %u",
                     format.mChannelsPerFrame);
        return false;
    }

    // 验证格式标志
    if (!(format.mFormatFlags & IOUserAudioFormatFlags::FormatFlagsNativeFloatPacked)) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Invalid format flags: %u",
                     format.mFormatFlags);
        return false;
    }

    // 验证字节计算的一致性
    if (const uint32_t expectedBytesPerFrame = format.mChannelsPerFrame * (format.mBitsPerChannel / 8);
            format.mBytesPerFrame != expectedBytesPerFrame) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Invalid bytes per frame: %u, expected: %u",
                     format.mBytesPerFrame, expectedBytesPerFrame);
        return false;
    }

    if (const uint32_t expectedBytesPerPacket = format.mBytesPerFrame * format.mFramesPerPacket;
            format.mBytesPerPacket != expectedBytesPerPacket) {
        os_log_error(OS_LOG_DEFAULT, LOG_PREFIX "Invalid bytes per packet: %u, expected: %u",
                     format.mBytesPerPacket, expectedBytesPerPacket);
        return false;
    }

    return true;
}