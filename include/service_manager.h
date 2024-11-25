//
// Created by AhogeK on 11/22/24.
//

#ifndef AUDIOCTL_SERVICE_MANAGER_H
#define AUDIOCTL_SERVICE_MANAGER_H

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <stdbool.h>
#include <sys/resource.h>
#include <pwd.h>
#include <grp.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

// 服务状态
typedef enum {
    SERVICE_STATUS_SUCCESS,
    SERVICE_STATUS_ERROR,
    SERVICE_STATUS_ALREADY_RUNNING,
    SERVICE_STATUS_NOT_RUNNING,
    SERVICE_STATUS_PERMISSION_DENIED
} ServiceStatus;

// 服务管理函数
ServiceStatus service_start(void);

ServiceStatus service_stop(void);

ServiceStatus service_restart(void);

bool service_is_running(void);

bool check_root_privileges(void);

void print_version(void);

#endif //AUDIOCTL_SERVICE_MANAGER_H
