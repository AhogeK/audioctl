#include "audio_control.h"

void printUsage()
{
    printf("使用方法：\n");
    printf("  audioctl [命令] [参数]\n\n");
    printf("可用命令：\n");
    printf("  list                 - 显示所有音频设备\n");
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

    if (info->deviceType != kDeviceTypeInput)
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

        OSStatus status = getDeviceList(&devices, &deviceCount);
        if (status == noErr)
        {
            printf("发现 %d 个音频设备:\n", deviceCount);
            for (UInt32 i = 0; i < deviceCount; i++)
            {
                printDeviceInfo(&devices[i]);
            }
            free(devices);
        }
    }

    return 0;
}
