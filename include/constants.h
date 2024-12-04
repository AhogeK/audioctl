//
// Created by AhogeK on 11/22/24.
//

#ifndef AUDIOCTL_CONSTANTS_H
#define AUDIOCTL_CONSTANTS_H

// 服务信息
#define SERVICE_NAME "AudioCTL"

#ifndef AUDIOCTL_VERSION
#define SERVICE_VERSION "unknown"
#else
#define SERVICE_VERSION AUDIOCTL_VERSION
#endif

#ifdef DEBUG
#define SERVICE_RUN_DIR "/tmp"
#define SERVICE_LOG_DIR "/tmp"
#else
#define SERVICE_RUN_DIR "/var/run/audioctl"
#define SERVICE_LOG_DIR "/var/log/audioctl"
#endif

#define PID_FILENAME "audioctl.pid"
#define LOG_FILENAME "audioctl.log"

#define PID_FILE SERVICE_RUN_DIR "/" PID_FILENAME
#define LOG_FILE SERVICE_LOG_DIR "/" LOG_FILENAME

#define SERVICE_USER "root"
#define SERVICE_GROUP "wheel"

#define DIR_MODE (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) //755
#define FILE_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) //644

// 定义 ANSI 颜色常量
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_BOLD_GREEN "\033[1;32m"
#define ANSI_COLOR_BOLD_RED "\033[1;31m"
#define ANSI_COLOR_RESET "\x1b[0m"


#endif //AUDIOCTL_CONSTANTS_H
