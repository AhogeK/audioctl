#include "audio_control.h"
#include "audio_apps.h"

// 定义 ANSI 颜色常量
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_RESET "\x1b[0m"

// 命令行选项和程序选项的定义
typedef struct
{
    const char shortOpt;
    const char* longOpt;
    const char* description;
    bool* flag;
} CommandOption;

typedef struct
{
    bool showOnlyActive;
    bool showOnlyInput;
    bool showOnlyOutput;
    // 后续添加更多选项
} ProgramOptions;

// 创建命令行选项和程序选项的关联
CommandOption* createCommandOptions(ProgramOptions* opts)
{
    static CommandOption options[] = {
        {'a', "active", "只列出使用中的设备", NULL},
        {'i', "input", "只列出输入设备", NULL},
        {'o', "output", "只列出输出设备", NULL},
        // 后续在这里添加更多选项
        {0, NULL, NULL, NULL} // 结束标记
    };

    options[0].flag = &opts->showOnlyActive;
    options[1].flag = &opts->showOnlyInput;
    options[2].flag = &opts->showOnlyOutput;
    // 后续在这里添加更多选项

    return options;
}

// 打印使用帮助信息
void printUsage()
{
    printf("使用方法：\n");
    printf(" audioctl [命令] [参数]\n\n");
    printf("可用命令：\n");
    printf(" list              - 显示所有音频设备\n");

    // 创建临时选项结构来获取选项描述
    ProgramOptions tempOpts = {0};
    const CommandOption* options = createCommandOptions(&tempOpts);

    printf("\n选项：\n");
    for (int i = 0; options[i].shortOpt != 0; i++)
    {
        printf(" -%c, --%-10s - %s\n",
               options[i].shortOpt,
               options[i].longOpt,
               options[i].description);
    }

    printf("\n选项可组合使用，例如：\n");
    printf(" list -ai         - 只列出使用中的输入设备\n");
    printf(" list -ao         - 只列出使用中的输出设备\n");
}

// 解析单个短选项
bool parseShortOption(const char opt, const CommandOption* options)
{
    for (int i = 0; options[i].shortOpt != 0; i++)
    {
        if (opt == options[i].shortOpt)
        {
            *options[i].flag = true;
            return true;
        }
    }
    printf("警告：未知选项: -%c\n", opt);
    return false;
}

// 解析长选项
bool parseLongOption(const char* arg, const CommandOption* options)
{
    // 跳过开头的 --
    arg += 2;

    for (int i = 0; options[i].longOpt != 0; i++)
    {
        if (strcmp(arg, options[i].longOpt) == 0)
        {
            *options[i].flag = true;
            return true;
        }
    }
    printf("警告：未知选项: --%s\n", arg);
    return false;
}

// 解析所有参数
bool parseOptions(const int argc, char* argv[], ProgramOptions* opts, const CommandOption* options)
{
    // 初始化所有选项为 false
    memset(opts, 0, sizeof(ProgramOptions));

    // 从参数2开始解析
    for (int i = 2; i < argc; i++)
    {
        if (argv[i][0] == '-')
        {
            // 检查是否只有一个横杠
            if (argv[i][1] == '\0')
            {
                printf("错误：无效的参数 '%s'\n", argv[i]);
                printf("参数格式必须是 '-x' 或 '--option'\n");
                printUsage();
                return false;
            }

            if (argv[i][1] == '-')
            {
                // 长选项
                if (argv[i][2] == '\0')
                {
                    printf("错误：无效的长参数 '%s'\n", argv[i]);
                    printf("长参数格式必须是 '--option'\n");
                    printUsage();
                    return false;
                }
                if (!parseLongOption(argv[i], options))
                {
                    return false;
                }
            }
            else
            {
                // 短选项
                const char* c = argv[i] + 1;
                while (*c != '\0')
                {
                    if (!parseShortOption(*c, options))
                    {
                        return false;
                    }
                    c++;
                }
            }
        }
        else
        {
            printf("错误：无效的参数 '%s'\n", argv[i]);
            printf("所有参数必须以 '-' 或 '--' 开头\n");
            printUsage();
            return false;
        }
    }

    // 验证选项组合的有效性
    if (opts->showOnlyInput && opts->showOnlyOutput)
    {
        printf("错误：不能同时指定输入和输出设备\n");
        printUsage();
        return false;
    }

    return true;
}

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
           info->isRunning ? ANSI_COLOR_GREEN : "",
           info->isRunning ? "使用中" : "空闲",
           info->isRunning ? ANSI_COLOR_RESET : "");
    printf("\n\n");
}

int main(const int argc, char* argv[])
{
    if (argc < 2)
    {
        printUsage();
        return 1;
    }

    if (strcmp(argv[1], "list") == 0)
    {
        ProgramOptions opts;
        const CommandOption* options = createCommandOptions(&opts);

        if (!parseOptions(argc, argv, &opts, options))
        {
            return 1;
        }

        AudioDeviceInfo* devices;
        UInt32 deviceCount;

        const OSStatus status = getDeviceList(&devices, &deviceCount);
        if (status == noErr)
        {
            // 计算符合条件的设备数量
            UInt32 matchedCount = 0;
            for (UInt32 i = 0; i < deviceCount; i++)
            {
                // 首先检查活跃状态
                if (opts.showOnlyActive && !devices[i].isRunning)
                {
                    continue;
                }

                // 然后检查设备类型
                if (opts.showOnlyInput && devices[i].deviceType != kDeviceTypeInput)
                {
                    continue;
                }
                if (opts.showOnlyOutput && devices[i].deviceType != kDeviceTypeOutput)
                {
                    continue;
                }

                matchedCount++;
            }

            // 打印设备数量信息
            printf("发现 %d 个", matchedCount);
            if (opts.showOnlyActive)
            {
                printf("使用中的");
            }
            if (opts.showOnlyInput)
            {
                printf("输入");
            }
            else if (opts.showOnlyOutput)
            {
                printf("输出");
            }
            printf("音频设备:\n");

            // 打印设备信息
            for (UInt32 i = 0; i < deviceCount; i++)
            {
                // 首先检查活跃状态
                if (opts.showOnlyActive && !devices[i].isRunning)
                {
                    continue;
                }

                // 然后检查设备类型
                if (opts.showOnlyInput && devices[i].deviceType != kDeviceTypeInput)
                {
                    continue;
                }
                if (opts.showOnlyOutput && devices[i].deviceType != kDeviceTypeOutput)
                {
                    continue;
                }

                printDeviceInfo(&devices[i]);
            }
            free(devices);
        }
        else
        {
            printf("获取设备列表失败，错误码: %d\n", (int)status);
            return 1;
        }
    }
    else if (strcmp(argv[1], "apps") == 0)
    {
        AudioAppInfo* apps;
        UInt32 appCount;

        const OSStatus status = getAudioApps(&apps, &appCount);
        if (status == noErr)
        {
            printf("发现 %d 个正在使用音频的应用程序:\n\n", appCount);

            for (UInt32 i = 0; i < appCount; i++)
            {
                printf("应用: %s (PID: %d)\n", apps[i].name, apps[i].pid);
                printf("音量: %.0f%%\n", apps[i].volume * 100);
            }

            freeAudioApps(apps);
        }
        else
        {
            printf("获取应用程序音频信息失败，错误码: %d\n", (int)status);
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
