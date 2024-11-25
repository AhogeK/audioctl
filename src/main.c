#include "audio_control.h"
#include "audio_apps.h"
#include "service_manager.h"

// 定义 ANSI 颜色常量
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_RESET "\x1b[0m"

// 命令行选项和程序选项的定义
typedef struct {
    const char shortOpt;
    const char *longOpt;
    const char *description;
    size_t flagOffset;  // 使用偏移量代替直接指针
} CommandOption;

typedef struct {
    bool showOnlyActive;
    bool showOnlyInput;
    bool showOnlyOutput;
    // 后续添加更多选项
} ProgramOptions;

// 创建命令行选项和程序选项的关联
static const CommandOption *getCommandOptions(void) {
    static const CommandOption options[] = {
            {'a', "active", "只列出使用中的设备", offsetof(ProgramOptions, showOnlyActive)},
            {'i', "input",  "只列出输入设备",     offsetof(ProgramOptions, showOnlyInput)},
            {'o', "output", "只列出输出设备",     offsetof(ProgramOptions, showOnlyOutput)},
            // 后续在这里添加更多选项
            {0, NULL, NULL, 0} // 结束标记
    };
    return options;
}

// 打印使用帮助信息
void printUsage() {
    printf("使用方法：\n");
    printf(" audioctl [命令] [参数]\n\n");
    printf("可用命令：\n");
    printf(" list - 显示所有音频设备\n");

    // 使用专门用于显示帮助的选项数组
    const CommandOption *options = getCommandOptions();

    printf("\n选项：\n");
    for (int i = 0; options[i].shortOpt != 0; i++) {
        printf(" -%c, --%-10s - %s\n",
               options[i].shortOpt,
               options[i].longOpt,
               options[i].description);
    }

    printf("\n选项可组合使用，例如：\n");
    printf(" list -ai - 只列出使用中的输入设备\n");
    printf(" list -ao - 只列出使用中的输出设备\n");
}

// 解析单个短选项
bool parseShortOption(const char opt, const CommandOption *options, ProgramOptions *opts) {
    for (int i = 0; options[i].shortOpt != 0; i++) {
        if (opt == options[i].shortOpt) {
            // 使用偏移量设置对应的标志位
            bool *flag = (bool *) ((char *) opts + options[i].flagOffset);
            *flag = true;
            return true;
        }
    }
    printf("警告：未知选项: -%c\n", opt);
    return false;
}

// 解析长选项
bool parseLongOption(const char *arg, const CommandOption *options, ProgramOptions *opts) {
    // 跳过开头的 --
    arg += 2;

    for (int i = 0; options[i].longOpt != 0; i++) {
        if (strcmp(arg, options[i].longOpt) == 0) {
            // 使用偏移量设置对应的标志位
            bool *flag = (bool *) ((char *) opts + options[i].flagOffset);
            *flag = true;
            return true;
        }
    }
    printf("警告：未知选项: --%s\n", arg);
    return false;
}

// 验证选项组合的有效性
static bool validateOptionCombination(const ProgramOptions *opts) {
    if (opts->showOnlyInput && opts->showOnlyOutput) {
        printf("错误：不能同时指定输入和输出设备\n");
        printUsage();
        return false;
    }
    return true;
}

// 处理无效参数
static bool handleInvalidArgument(const char *arg, bool isLongOption) {
    if (isLongOption) {
        printf("错误：无效的长参数 '%s'\n", arg);
        printf("长参数格式必须是 '--option'\n");
    } else {
        printf("错误：无效的参数 '%s'\n", arg);
        printf("参数格式必须是 '-x' 或 '--option'\n");
    }
    printUsage();
    return false;
}

// 处理单个参数
static bool handleArgument(const char *arg, const CommandOption *options, ProgramOptions *opts) {
    // 检查是否是选项参数
    if (arg[0] != '-') {
        printf("错误：无效的参数 '%s'\n", arg);
        printf("所有参数必须以 '-' 或 '--' 开头\n");
        printUsage();
        return false;
    }

    // 检查是否只有一个横杠
    if (arg[1] == '\0') {
        return handleInvalidArgument(arg, false);
    }

    // 处理长选项
    if (arg[1] == '-') {
        if (arg[2] == '\0') {
            return handleInvalidArgument(arg, true);
        }
        return parseLongOption(arg, options, opts);
    }

    // 处理短选项
    const char *c = arg + 1;
    while (*c != '\0') {
        if (!parseShortOption(*c, options, opts)) {
            return false;
        }
        c++;
    }

    return true;
}

// 解析所有参数
bool parseOptions(const int argc, char *argv[], ProgramOptions *opts) {
    // 初始化所有选项为 false
    memset(opts, 0, sizeof(ProgramOptions));

    const CommandOption *options = getCommandOptions();

    // 从参数2开始解析
    for (int i = 2; i < argc; i++) {
        if (!handleArgument(argv[i], options, opts)) {
            return false;
        }
    }

    // 验证选项组合的有效性
    return validateOptionCombination(opts);
}

void printDeviceInfo(const AudioDeviceInfo *info) {
    printf("ID: %d, 名称: %s, ", info->deviceId, info->name);

    switch (info->deviceType) {
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

    if (info->deviceType == kDeviceTypeInput) {
        printf("\n  输入音量: ");
        if (!info->hasVolumeControl || info->transportType == kAudioDeviceTransportTypeContinuityCaptureWired ||
            info->transportType == kAudioDeviceTransportTypeContinuityCaptureWireless) {
            printf("不可调节");
        } else {
            printf("%.0f%%", info->volume * 100);
        }
    } else if (info->deviceType == kDeviceTypeOutput || info->deviceType == kDeviceTypeInputOutput) {
        printf("\n  音量: ");
        if (!info->hasVolumeControl) {
            printf("不可调节");
        } else {
            printf("%.0f%%", info->volume * 100);
        }
        // 只为输出设备显示静音状态
        printf(", 静音: %s", info->isMuted ? "是" : "否");
    }

    printf("\n  采样率: %d Hz", info->sampleRate);
    if (info->bitsPerChannel > 0) {
        printf(", 位深度: %d bits", info->bitsPerChannel);
        printf(", 格式: %s", getFormatFlagsDescription(info->formatFlags));
    }
    printf(", 状态: %s%s%s",
           info->isRunning ? ANSI_COLOR_GREEN : "",
           info->isRunning ? "使用中" : "空闲",
           info->isRunning ? ANSI_COLOR_RESET : "");
    printf("\n\n");
}

// 处理版本命令
static bool handleVersionCommand(const char *arg) {
    if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
        print_version();
        return true;
    }
    return false;
}

// 打印匹配的设备数量信息
static void printMatchedDeviceCount(UInt32 matchedCount, const ProgramOptions *opts) {
    printf("发现 %d 个", matchedCount);
    if (opts->showOnlyActive) {
        printf("使用中的");
    }
    if (opts->showOnlyInput) {
        printf("输入");
    } else if (opts->showOnlyOutput) {
        printf("输出");
    }
    printf("音频设备:\n");
}

// 检查设备是否匹配过滤条件
static bool isDeviceMatched(const AudioDeviceInfo *device, const ProgramOptions *opts) {
    // 检查活跃状态
    if (opts->showOnlyActive && !device->isRunning) {
        return false;
    }

    // 检查设备类型
    if (opts->showOnlyInput && device->deviceType != kDeviceTypeInput) {
        return false;
    }
    if (opts->showOnlyOutput && device->deviceType != kDeviceTypeOutput) {
        return false;
    }

    return true;
}

// 计算匹配的设备数量
static UInt32 countMatchedDevices(const AudioDeviceInfo *devices, UInt32 deviceCount,
                                  const ProgramOptions *opts) {
    UInt32 matchedCount = 0;
    for (UInt32 i = 0; i < deviceCount; i++) {
        if (isDeviceMatched(&devices[i], opts)) {
            matchedCount++;
        }
    }
    return matchedCount;
}

// 处理设备列表命令
static int handleListCommand(int argc, char *argv[]) {
    ProgramOptions opts;
    if (!parseOptions(argc, argv, &opts)) {
        return 1;
    }

    AudioDeviceInfo *devices;
    UInt32 deviceCount;
    const OSStatus status = getDeviceList(&devices, &deviceCount);

    if (status != noErr) {
        printf("获取设备列表失败，错误码: %d\n", status);  // 移除了多余的类型转换
        return 1;
    }

    // 计算并显示匹配的设备数量
    UInt32 matchedCount = countMatchedDevices(devices, deviceCount, &opts);
    printMatchedDeviceCount(matchedCount, &opts);

    // 打印设备信息
    for (UInt32 i = 0; i < deviceCount; i++) {
        if (isDeviceMatched(&devices[i], &opts)) {
            printDeviceInfo(&devices[i]);
        }
    }

    free(devices);
    return 0;
}


// 处理应用程序音频信息命令
static int handleAppsCommand(void) {
    AudioAppInfo *apps;
    UInt32 appCount;
    const OSStatus status = getAudioApps(&apps, &appCount);

    if (status != noErr) {
        printf("获取应用程序音频信息失败，错误码: %d\n", status);
        return 1;
    }

    printf("发现 %d 个正在使用音频的应用程序:\n\n", appCount);
    for (UInt32 i = 0; i < appCount; i++) {
        printf("应用: %s (PID: %d)\n", apps[i].name, apps[i].pid);
        printf("音量: %.0f%%\n", apps[i].volume * 100);
    }

    freeAudioApps(apps);
    return 0;
}

// 主函数
int main(const int argc, char *argv[]) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    // 处理版本命令
    if (handleVersionCommand(argv[1])) {
        return 0;
    }

    // 处理主要命令
    if (strcmp(argv[1], "list") == 0) {
        return handleListCommand(argc, argv);
    }

    if (strcmp(argv[1], "apps") == 0) {
        return handleAppsCommand();
    }

    if (strcmp(argv[1], "--start-service") == 0) {
        ServiceStatus status = service_start();
        return (status == SERVICE_STATUS_SUCCESS || status == SERVICE_STATUS_ALREADY_RUNNING) ? 0 : 1;
    }

    if (strcmp(argv[1], "--stop-service") == 0) {
        ServiceStatus status = service_stop();
        return (status == SERVICE_STATUS_SUCCESS || status == SERVICE_STATUS_NOT_RUNNING) ? 0 : 1;
    }

    if (strcmp(argv[1], "--restart-service") == 0) {
        return service_restart() == SERVICE_STATUS_SUCCESS ? 0 : 1;
    }

    // 处理未知命令
    printf("未知命令: %s\n", argv[1]);
    printUsage();
    return 1;
}
