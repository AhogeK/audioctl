#include "audio_control.h"

void printUsage()
{
    printf("使用方法：\n");
    printf("  audioctl [命令] [参数]\n\n");
    printf("可用命令：\n");
    printf("  list                 - 显示所有音频设备\n");
    printf("  list --active|-a     - 只列出使用中的音频设备\n");
}

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_RESET   "\x1b[0m"

void printDeviceInfo(const AudioDeviceInfo* info)
{
    printf("ID: %d, 名称: %s, ", info->deviceId, info->name);

    switch (info->deviceType)
    {
    case kDeviceTypeInput:
        printf("输入设备 (通道数: %d)", info->inputChannelCount);
        break;
    case kDeviceTypeOutput:
        printf("输出设备 (通道数: %d)", info->outputChannelCount);
        break;
    case kDeviceTypeInputOutput:
        printf("输入/输出设备 (输入通道: %d, 输出通道: %d, 总通道: %d)",
               info->inputChannelCount, info->outputChannelCount, info->channelCount);
        break;
    default:
        printf("未知类型");
        break;
    }

    printf("\n  传输类型: %s", getTransportTypeName(info->transportType));

    if (info->deviceType == kDeviceTypeInput)
    {
        printf("\n  输入音量: ");
        if (!info->hasVolumeControl || info->transportType == kAudioDeviceTransportTypeContinuityCaptureWired ||
            info->transportType == kAudioDeviceTransportTypeContinuityCaptureWireless)
        {
            printf("不可调节");
        }
        else
        {
            printf("%.0f%%", info->volume * 100);
        }
    }
    else if (info->deviceType == kDeviceTypeOutput || info->deviceType == kDeviceTypeInputOutput)
    {
        printf("\n  音量: ");
        if (!info->hasVolumeControl)
        {
            printf("不可调节");
        }
        else
        {
            printf("%.0f%%", info->volume * 100);
        }
        // 只为输出设备显示静音状态
        printf(", 静音: %s", info->isMuted ? "是" : "否");
    }

    printf("\n  采样率: %d Hz", info->sampleRate);
    if (info->bitsPerChannel > 0)
    {
        printf(", 位深度: %d bits", info->bitsPerChannel);
        printf(", 格式: %s", getFormatFlagsDescription(info->formatFlags));
    }
    printf(", 状态: %s%s%s",
           info->isRunning ? ANSI_COLOR_RED : "",
           info->isRunning ? "使用中" : "空闲",
           info->isRunning ? ANSI_COLOR_RESET : "");
    printf("\n\n");
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        printUsage();
        return 1;
    }

    if (strcmp(argv[1], "list") == 0)
    {
        AudioDeviceInfo* devices;
        UInt32 deviceCount;
        bool showOnlyActive = false;

        // 检查是否有 --active 或 -a 参数
        if (argc > 2 && (strcmp(argv[2], "--active") == 0 || strcmp(argv[2], "-a") == 0))
        {
            showOnlyActive = true;
        }

        OSStatus status = getDeviceList(&devices, &deviceCount);
        if (status == noErr)
        {
            // 计算活跃设备数量
            UInt32 activeCount = 0;
            if (showOnlyActive)
            {
                for (UInt32 i = 0; i < deviceCount; i++)
                {
                    if (devices[i].isRunning)
                    {
                        activeCount++;
                    }
                }
            }

            // 打印设备数量信息
            if (showOnlyActive)
            {
                printf("发现 %d 个使用中的音频设备:\n", activeCount);
            }
            else
            {
                printf("发现 %d 个音频设备:\n", deviceCount);
            }

            // 打印设备信息
            for (UInt32 i = 0; i < deviceCount; i++)
            {
                if (!showOnlyActive || devices[i].isRunning)
                {
                    printDeviceInfo(&devices[i]);
                }
            }
            free(devices);
        }
        else
        {
            printf("获取设备列表失败，错误码: %d\n", (int)status);
            return 1;
        }
    }
    else
    {
        printf("未知命令: %s\n", argv[1]);
        printUsage();
        return 1;
    }

    return 0;
}
