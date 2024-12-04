//
// Created by AhogeK on 11/22/24.
//

#include "service_manager.h"
#include "constants.h"

static int ensure_single_directory(int parent_fd, const char *name) {
    // 尝试在父目录下创建目录
    if (mkdirat(parent_fd, name, DIR_MODE) == -1 && errno != EEXIST) {
        fprintf(stderr, "无法创建目录 %s: %s\n", name, strerror(errno));
        return -1;
    }

    // 打开新创建的目录
    int fd = openat(parent_fd, name, O_DIRECTORY | O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "无法打开目录 %s: %s\n", name, strerror(errno));
        return -1;
    }

    // 获取并检查文件状态
    struct stat st;
    if (fstat(fd, &st) == -1 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s 不是一个目录\n", name);
        close(fd);
        return -1;
    }

    // 使用 getpwnam_r 获取用户信息
    struct passwd pw;
    struct passwd *pw_result = NULL;
    char pw_buffer[1024];
    if (getpwnam_r(SERVICE_USER, &pw, pw_buffer, sizeof(pw_buffer), &pw_result) != 0 || pw_result == NULL) {
        fprintf(stderr, "无法获取用户信息\n");
        close(fd);
        return -1;
    }

    // 使用 getgrnam_r 获取组信息
    struct group gr;
    struct group *gr_result;
    char gr_buffer[1024];
    if (getgrnam_r(SERVICE_GROUP, &gr, gr_buffer, sizeof(gr_buffer), &gr_result) != 0 || gr_result == NULL) {
        fprintf(stderr, "无法获取组信息\n");
        close(fd);
        return -1;
    }

    // 设置目录所有权
    if (fchown(fd, pw.pw_uid, gr.gr_gid) == -1) {
        fprintf(stderr, "无法设置目录所有权: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int ensure_directory(const char *path) {
    char path_copy[PATH_MAX];
    char const *p;
    int parent_fd = AT_FDCWD;  // 从当前工作目录开始
    int current_fd;

    // 复制路径以进行修改
    if (strlen(path) >= sizeof(path_copy)) {
        fprintf(stderr, "路径太长\n");
        return -1;
    }
    strcpy(path_copy, path);

    // 逐级创建目录
    p = path_copy;
    while (*p == '/') p++;  // 跳过开头的斜杠

    while (1) {
        char *slash = strchr(p, '/');
        if (slash) {
            *slash = '\0';  // 临时截断字符串
        }

        current_fd = ensure_single_directory(parent_fd, p);
        if (current_fd == -1) {
            if (parent_fd != AT_FDCWD) {
                close(parent_fd);
            }
            return -1;
        }

        if (parent_fd != AT_FDCWD) {
            close(parent_fd);
        }
        parent_fd = current_fd;

        if (!slash) {
            break;  // 到达路径末尾
        }

        *slash = '/';  // 恢复斜杠
        p = slash + 1;
        while (*p == '/') p++;  // 跳过连续的斜杠
    }

    // 关闭最后打开的目录
    if (parent_fd != AT_FDCWD) {
        close(parent_fd);
    }

    return 0;
}

// 获取当前用户名
static const char *get_current_username(void) {
    uid_t uid = getuid();
    struct passwd pwd;
    struct passwd *result = NULL;  // 初始化为 NULL

    // 获取缓冲区大小
    long bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufsize == -1) {
        bufsize = 16384;  // 16KB
    }

    // 分配缓冲区
    char *buffer = malloc(bufsize);
    if (buffer == NULL) {
        fprintf(stderr, "内存分配失败\n");
        return "unknown";
    }

    // 获取用户信息
    int ret = getpwuid_r(uid, &pwd, buffer, bufsize, &result);

    if (ret != 0) {
        fprintf(stderr, "获取用户信息失败: %s\n", strerror(ret));
        free(buffer);
        return "unknown";
    }

    if (result == NULL) {
        fprintf(stderr, "未找到 UID 为 %d 的用户\n", uid);
        free(buffer);
        return "unknown";
    }

    // 复制用户名
    const char *username = strdup(pwd.pw_name);
    free(buffer);

    if (username == NULL) {
        fprintf(stderr, "复制用户名时内存分配失败\n");
        return "unknown";
    }

    return username;
}

// 打印权限错误信息
static void print_permission_error(void) {
    const char *username = get_current_username();
    printf("\033[1;31m错误：需要管理员权限\033[0m\n");
    printf("当前用户: %s\n", username);
    printf("请使用 sudo 运行此命令：\n");
    printf("  sudo audioctl --start-service\n");
    printf("  sudo audioctl --stop-service\n");
    printf("  sudo audioctl --restart-service\n");
}

void print_version(void) {
    printf("%s version %s \n", SERVICE_NAME, SERVICE_VERSION);
}

// 写入日志
static void write_log(const char *message) {
    if (ensure_directory(SERVICE_LOG_DIR) == -1) return;

    FILE *log_file = fopen(LOG_FILE, "a");
    if (!log_file && errno == ENOENT && ensure_directory(SERVICE_LOG_DIR)) {
        log_file = fopen(LOG_FILE, "a");
    }
    if (log_file) {
        time_t now;
        struct tm time_info;
        char date[32];

        time(&now);

        localtime_r(&now, &time_info);

        strftime(date, sizeof(date), "%a %b %d %H:%M:%S %Y", &time_info);

        fprintf(log_file, "[%s] [%s v%s] %s\n", date, SERVICE_NAME, SERVICE_VERSION, message);
        fchmod(fileno(log_file), FILE_MODE);
        fclose(log_file);
    } else {
        fprintf(stderr, "警告：无法写入日志文件 %s: %s\n", LOG_FILE, strerror(errno));
    }
}

// 读取PID文件
static pid_t read_pid_file(void) {
    FILE *pid_file = fopen(PID_FILE, "r");
    if (!pid_file) return -1;

    char buffer[32];
    if (fgets(buffer, sizeof(buffer), pid_file) == NULL) {
        fclose(pid_file);
        return -1;
    }
    fclose(pid_file);

    errno = 0;
    char *endptr;

    long pid = strtol(buffer, &endptr, 10);

    if (endptr == buffer) {  // 没有进行有效转换
        return -1;
    }
    if (*endptr != '\n' && *endptr != '\0') {  // 存在非数字字符
        return -1;
    }
    if (errno == ERANGE) {  // 数值超出范围
        return -1;
    }
    if (pid < 1) {  // PID必须是正数
        return -1;
    }
    if (pid > INT32_MAX) {  // 确保不超过pid_t的范围
        return -1;
    }

    return (pid_t) pid;
}

// 写入PID文件
static int write_pid_file(pid_t pid) {
    if (ensure_directory(SERVICE_RUN_DIR) == -1) return -1;

    FILE *pid_file = fopen(PID_FILE, "w");
    if (!pid_file) return -1;

    fprintf(pid_file, "%d\n", pid);
    fchmod(fileno(pid_file), FILE_MODE);
    fclose(pid_file);

    return 0;
}

// 检查进程是否在运行
bool service_is_running(void) {
    pid_t pid = read_pid_file();
    if (pid == -1) return false;

    return (kill(pid, 0) == 0);
}

// 守护进程初始化
static void init_daemon(void) {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    if (setsid() < 0) exit(EXIT_FAILURE);

    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    if (chdir("/") < 0) {
        exit(EXIT_FAILURE);
    }

    umask(0);

    // 获取系统最大文件描述符
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd == -1) maxfd = 1024;  // 使用默认值

    // 确保不超过 INT_MAX
    if (maxfd > INT32_MAX) maxfd = INT32_MAX;

    // 关闭所有文件描述符
    for (int fd = (int) maxfd; fd >= 0; fd--) close(fd);

    // 重定向标准输入、输出、错误到/dev/null
    int fd = open("/dev/null", O_RDWR);
    if (fd == -1) exit(EXIT_FAILURE);

    if (dup2(fd, STDIN_FILENO) == -1) exit(EXIT_FAILURE);
    if (dup2(fd, STDOUT_FILENO) == -1) exit(EXIT_FAILURE);
    if (dup2(fd, STDERR_FILENO) == -1) exit(EXIT_FAILURE);
    if (fd > STDERR_FILENO) close(fd);
}

// 运行守护进程
_Noreturn void run_daemon(void) {
    // 确保以root权限运行
    if (!check_root_privileges()) {
        fprintf(stderr, "错误：守护进程需要root权限\n");
        exit(EXIT_FAILURE);
    }

    init_daemon();

    // 记录启动日志
    write_log("守护进程启动");

    // 写入PID文件
    write_pid_file(getpid());

    // 进入主循环
    while (1) {
        write_log("服务正在运行...");
        sleep(3600); // 休眠1小时
    }
}

// 启动服务
ServiceStatus service_start(void) {
    if (!check_root_privileges()) {
        print_permission_error();
        return SERVICE_STATUS_PERMISSION_DENIED;
    }

    if (service_is_running()) {
        return SERVICE_STATUS_ALREADY_RUNNING;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("服务启动失败\n");
        return SERVICE_STATUS_ERROR;
    }

    if (pid == 0) {
        run_daemon();
        // run_daemon是_Noreturn函数，不会返回, 所以这里不需要处理
    }

    // 等待守护进程启动
    struct timespec ts = {
            .tv_sec = 0,
            .tv_nsec = 100000000  // 100ms
    };

    nanosleep(&ts, NULL);

    if (service_is_running()) {
        printf("服务启动成功\n");
        return SERVICE_STATUS_SUCCESS;
    }

    printf("服务启动失败\n");
    return SERVICE_STATUS_ERROR;
}

// 停止服务
ServiceStatus service_stop(void) {
    if (!check_root_privileges()) {
        print_permission_error();
        return SERVICE_STATUS_PERMISSION_DENIED;
    }

    pid_t pid = read_pid_file();
    if (pid <= 0) {
        printf("服务未运行\n");
        return SERVICE_STATUS_NOT_RUNNING;
    }

    if (kill(pid, SIGTERM) == 0) {
        unlink(PID_FILE);
        printf("服务停止成功\n");
        return SERVICE_STATUS_SUCCESS;
    }

    printf("服务停止失败\n");
    return SERVICE_STATUS_ERROR;
}

// 重启服务
ServiceStatus service_restart(void) {
    if (!check_root_privileges()) {
        print_permission_error();
        return SERVICE_STATUS_PERMISSION_DENIED;
    }

    printf("正在重启 %s 服务...\n", SERVICE_NAME);

    if (service_is_running()) {
        ServiceStatus stop_status = service_stop();
        if (stop_status != SERVICE_STATUS_SUCCESS) {
            return stop_status;
        }
        // 等待服务停止
        sleep(1);
    }

    return service_start();
}

// 检查是否有root权限
bool check_root_privileges(void) {
    return (getuid() == 0);
}

void print_service_status(void) {
    pid_t pid = read_pid_file();
    bool is_running = service_is_running();

    printf("%s 服务状态：\n", SERVICE_NAME);

    if (is_running) {
        printf("● %s - 版本 %s\n", SERVICE_NAME, SERVICE_VERSION);
        printf("状态：" ANSI_COLOR_BOLD_GREEN "运行中" ANSI_COLOR_RESET "(PID: %d)\n", pid);

        // 使用 sysctl 获取进程启动时间
        struct kinfo_proc proc_info;
        size_t proc_info_size = sizeof(proc_info);
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};

        if (sysctl(mib, 4, &proc_info, &proc_info_size, NULL, 0) == 0) {
            time_t start_time = proc_info.kp_proc.p_un.__p_starttime.tv_sec;
            char time_str[100];
            struct tm tm_info;
            localtime_r(&start_time, &tm_info);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_info);
            printf("启动时间：%s\n", time_str);
        }

        // 检查日志文件
        struct stat log_stat;
        if (stat(LOG_FILE, &log_stat) == 0) {
            printf("日志文件：%s\n", LOG_FILE);
            printf("日志大小：%.2f KB\n", (float) log_stat.st_size / 1024);
        }

        // 显示运行目录信息
        printf("PID 文件：%s\n", PID_FILE);
        printf("运行目录：%s\n", SERVICE_RUN_DIR);
        printf("日志目录：%s\n", SERVICE_LOG_DIR);
    } else {
        printf("● %s - 版本 %s\n", SERVICE_NAME, SERVICE_VERSION);
        printf("状态：" ANSI_COLOR_BOLD_RED "未运行" ANSI_COLOR_RESET "\n");

        // 检查目录是否存在
        struct stat dir_stat;
        if (stat(SERVICE_RUN_DIR, &dir_stat) == 0) {
            printf("运行目录：%s（已存在）\n", SERVICE_RUN_DIR);
        } else {
            printf("运行目录：%s（未创建）\n", SERVICE_RUN_DIR);
        }

        if (stat(SERVICE_LOG_DIR, &dir_stat) == 0) {
            printf("日志目录：%s（已存在）\n", SERVICE_LOG_DIR);
        } else {
            printf("日志目录：%s（未创建）\n", SERVICE_LOG_DIR);
        }
    }

    // 显示权限信息
    printf("\n权限信息：\n");
    printf("运行用户：%s\n", SERVICE_USER);
    printf("运行用户组：%s\n", SERVICE_GROUP);
    printf("目录权限：%o\n", DIR_MODE);
    printf("文件权限：%o\n", FILE_MODE);
}