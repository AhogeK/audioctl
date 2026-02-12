//
// Created by AhogeK on 02/12/26.
//

#include "ipc/ipc_server.h"
#include "ipc/ipc_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

// 客户端连接结构
typedef struct ClientConnection
{
    int fd;
    pid_t pid;
    struct ClientConnection* next;
} ClientConnection;

static ClientConnection* g_connections = NULL;
static IPCServerContext* g_server_ctx = NULL;
// 【关键修复】添加互斥锁保护连接链表
static pthread_mutex_t g_connections_mutex = PTHREAD_MUTEX_INITIALIZER;

// 信号处理
static void signal_handler(int sig)
{
    if ((sig == SIGTERM || sig == SIGINT) && g_server_ctx != NULL)
    {
        g_server_ctx->running = false;
    }
}

// 获取当前时间戳（毫秒）
static uint64_t get_timestamp_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 设置 socket 为非阻塞
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 添加客户端连接
static int add_connection(int fd, pid_t pid)
{
    ClientConnection* conn = malloc(sizeof(ClientConnection));
    if (conn == NULL)
        return -1;

    conn->fd = fd;
    conn->pid = pid;

    // 【关键修复】使用互斥锁保护链表操作
    pthread_mutex_lock(&g_connections_mutex);
    conn->next = g_connections;
    g_connections = conn;
    pthread_mutex_unlock(&g_connections_mutex);

    return 0;
}

// 移除客户端连接
static void remove_connection(int fd)
{
    // 【关键修复】使用互斥锁保护链表操作
    pthread_mutex_lock(&g_connections_mutex);
    ClientConnection** current = &g_connections;

    // 【关键修复】防止链表损坏导致的无限循环，设置最大迭代次数
    int max_iterations = 10000;
    int iterations = 0;

    while (*current != NULL && iterations < max_iterations)
    {
        iterations++;
        if ((*current)->fd == fd)
        {
            ClientConnection* to_remove = *current;
            *current = (*current)->next;
            pthread_mutex_unlock(&g_connections_mutex); // 解锁后再关闭 fd
            close(to_remove->fd);
            free(to_remove);
            return;
        }
        current = &(*current)->next;
    }
    
    pthread_mutex_unlock(&g_connections_mutex);
}

// 根据 PID 查找客户端条目
IPCClientEntry* ipc_server_find_client(IPCServerContext* ctx, pid_t pid)
{
    IPCClientEntry* current = ctx->clients;
    while (current != NULL)
    {
        if (current->pid == pid)
        {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// 注册新客户端
int ipc_server_register_client(IPCServerContext* ctx, pid_t pid, float volume, bool muted, const char* app_name)
{
    // 检查是否已存在
    if (ipc_server_find_client(ctx, pid) != NULL)
    {
        return -1; // 已存在
    }

    IPCClientEntry* entry = malloc(sizeof(IPCClientEntry));
    if (entry == NULL)
        return -1;

    entry->pid = pid;
    entry->volume = volume;
    entry->muted = muted;
    entry->connected_at = get_timestamp_ms();
    strncpy(entry->app_name, app_name, sizeof(entry->app_name) - 1);
    entry->app_name[sizeof(entry->app_name) - 1] = '\0';
    entry->next = ctx->clients;

    ctx->clients = entry;
    ctx->client_count++;

    return 0;
}

// 注销客户端
int ipc_server_unregister_client(IPCServerContext* ctx, pid_t pid)
{
    IPCClientEntry** current = &ctx->clients;
    while (*current != NULL)
    {
        if ((*current)->pid == pid)
        {
            IPCClientEntry* to_remove = *current;
            *current = (*current)->next;
            free(to_remove);
            ctx->client_count--;
            return 0;
        }
        current = &(*current)->next;
    }
    return -1; // 未找到
}

// 设置客户端音量
int ipc_server_set_volume(IPCServerContext* ctx, pid_t pid, float volume)
{
    if (volume < 0.0f)
        volume = 0.0f;
    if (volume > 1.0f)
        volume = 1.0f;

    IPCClientEntry* client = ipc_server_find_client(ctx, pid);
    if (client == NULL)
        return -1;

    client->volume = volume;
    return 0;
}

// 获取客户端音量
int ipc_server_get_volume(IPCServerContext* ctx, pid_t pid, float* volume, bool* muted)
{
    const IPCClientEntry* client = ipc_server_find_client(ctx, pid);
    if (client == NULL)
        return -1;

    if (volume != NULL)
        *volume = client->volume;
    if (muted != NULL)
        *muted = client->muted;
    return 0;
}

// 设置客户端静音状态
int ipc_server_set_mute(IPCServerContext* ctx, pid_t pid, bool muted)
{
    IPCClientEntry* client = ipc_server_find_client(ctx, pid);
    if (client == NULL)
        return -1;

    client->muted = muted;
    return 0;
}

// 获取客户端数量
uint32_t ipc_server_get_client_count(IPCServerContext* ctx) { return ctx->client_count; }

// 获取所有客户端列表
IPCClientEntry* ipc_server_list_clients(IPCServerContext* ctx, uint32_t* count)
{
    if (ctx == NULL || count == NULL)
    {
        return NULL;
    }

    *count = ctx->client_count;
    if (*count == 0)
    {
        return NULL;
    }

    // 分配数组内存
    IPCClientEntry* list = malloc(sizeof(IPCClientEntry) * (*count));
    if (list == NULL)
    {
        *count = 0;
        return NULL;
    }

    // 复制客户端信息到数组
    uint32_t i = 0;
    IPCClientEntry* current = ctx->clients;
    while (current != NULL && i < *count)
    {
        memcpy(&list[i], current, sizeof(IPCClientEntry));
        list[i].next = NULL; // 数组中不需要链表指针
        i++;
        current = current->next;
    }

    return list;
}

// 初始化服务端
int ipc_server_init(IPCServerContext* ctx)
{
    if (ctx == NULL)
        return -1;

    memset(ctx, 0, sizeof(IPCServerContext));

    // 设置信号处理
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    // 获取 socket 路径
    char socket_path[PATH_MAX];
    if (get_ipc_socket_path(socket_path, sizeof(socket_path)) != 0)
    {
        fprintf(stderr, "无法获取 socket 路径\n");
        return -1;
    }

    // 清理已存在的 socket 文件
    unlink(socket_path);

    // 创建 socket
    ctx->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0)
    {
        perror("socket");
        return -1;
    }

    // 设置非阻塞
    if (set_nonblocking(ctx->listen_fd) < 0)
    {
        perror("fcntl");
        close(ctx->listen_fd);
        return -1;
    }

    // 绑定
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(ctx->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(ctx->listen_fd);
        return -1;
    }

    // 监听
    if (listen(ctx->listen_fd, IPC_SOCKET_BACKLOG) < 0)
    {
        perror("listen");
        close(ctx->listen_fd);
        unlink(socket_path);
        return -1;
    }

    // 创建 kqueue
    ctx->epoll_fd = kqueue();
    if (ctx->epoll_fd < 0)
    {
        perror("kqueue");
        close(ctx->listen_fd);
        unlink(socket_path);
        return -1;
    }

    // 注册监听 socket 到 kqueue
    struct kevent ev;
    EV_SET(&ev, ctx->listen_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(ctx->epoll_fd, &ev, 1, NULL, 0, NULL) < 0)
    {
        perror("kevent");
        close(ctx->epoll_fd);
        close(ctx->listen_fd);
        unlink(socket_path);
        return -1;
    }

    ctx->running = true;
    g_server_ctx = ctx;

    printf("IPC 服务端已启动，监听: %s\n", socket_path);
    return 0;
}

// 处理新连接
static void handle_new_connection(IPCServerContext* ctx)
{
    struct sockaddr_un addr;
    socklen_t addr_len = sizeof(addr);

    int client_fd = accept(ctx->listen_fd, (struct sockaddr*)&addr, &addr_len);
    if (client_fd < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            perror("accept");
        }
        return;
    }

    // 设置非阻塞
    if (set_nonblocking(client_fd) < 0)
    {
        close(client_fd);
        return;
    }

    // 注册到 kqueue
    struct kevent ev;
    EV_SET(&ev, client_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(ctx->epoll_fd, &ev, 1, NULL, 0, NULL) < 0)
    {
        perror("kevent");
        close(client_fd);
        return;
    }

    add_connection(client_fd, 0); // PID 会在注册时更新
    printf("新客户端连接: fd=%d\n", client_fd);
}

// 发送响应
static int send_response(int fd, uint32_t request_id, int32_t status, const void* data, uint32_t data_len)
{
    uint32_t total_len = sizeof(IPCMessageHeader) + sizeof(IPCResponse) + data_len;
    uint8_t* buffer = malloc(total_len);
    if (buffer == NULL)
        return -1;

    IPCMessageHeader* header = (IPCMessageHeader*)buffer;
    ipc_init_header(header, kIPCCommandResponse, sizeof(IPCResponse) + data_len, request_id);

    IPCResponse* resp = (IPCResponse*)(buffer + sizeof(IPCMessageHeader));
    resp->status = status;
    resp->data_len = data_len;

    if (data_len > 0 && data != NULL)
    {
        memcpy(buffer + sizeof(IPCMessageHeader) + sizeof(IPCResponse), data, data_len);
    }

    ssize_t sent = send(fd, buffer, total_len, 0);
    free(buffer);

    return (sent == (ssize_t)total_len) ? 0 : -1;
}

// 处理客户端消息
static void handle_client_message(IPCServerContext* ctx, int client_fd)
{
    IPCMessageHeader header;
    ssize_t received = recv(client_fd, &header, sizeof(header), 0);

    if (received <= 0)
    {
        // 连接关闭或错误
        remove_connection(client_fd);
        struct kevent ev;
        EV_SET(&ev, client_fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(ctx->epoll_fd, &ev, 1, NULL, 0, NULL);
        return;
    }

    if (received != sizeof(header) || !ipc_validate_header(&header))
    {
        // 无效消息头
        send_response(client_fd, 0, kIPCStatusInvalidHeader, NULL, 0);
        return;
    }

    // 读取负载
    uint8_t* payload = NULL;
    if (header.payload_len > 0)
    {
        payload = malloc(header.payload_len);
        if (payload == NULL)
        {
            send_response(client_fd, header.request_id, kIPCStatusInternalError, NULL, 0);
            return;
        }

        received = recv(client_fd, payload, header.payload_len, 0);
        if (received != (ssize_t)header.payload_len)
        {
            free(payload);
            send_response(client_fd, header.request_id, kIPCStatusPayloadTooLarge, NULL, 0);
            return;
        }
    }

    // 处理指令
    int32_t status = kIPCStatusOK;
    void* response_data = NULL;
    bool response_needs_free = false;
    uint32_t response_len = 0;
    IPCVolumeResponse vol_resp = {0}; // 提升作用域以修复 line 503

    switch (header.command)
    {
    case kIPCCommandRegister:
        {
            if (header.payload_len >= sizeof(IPCRegisterRequest) && payload != NULL)
            {
                const IPCRegisterRequest* req = (const IPCRegisterRequest*)payload;
                const char* app_name = (const char*)(payload + sizeof(IPCRegisterRequest));
                status = ipc_server_register_client(ctx, req->pid, req->initial_volume, req->muted, app_name);
            }
            else
            {
                status = kIPCStatusInvalidHeader;
            }
            break;
        }

    case kIPCCommandUnregister:
        {
            if (header.payload_len >= sizeof(pid_t) && payload != NULL)
            {
                const pid_t* pid = (const pid_t*)payload;
                status = ipc_server_unregister_client(ctx, *pid);
                if (status != 0)
                    status = kIPCStatusClientNotFound;
            }
            else
            {
                status = kIPCStatusInvalidHeader;
            }
            break;
        }

    case kIPCCommandGetVolume:
        {
            if (header.payload_len >= sizeof(pid_t) && payload != NULL)
            {
                const pid_t* pid = (const pid_t*)payload;
                float volume = 0.0f;
                bool muted = false;
                int32_t vol_status = ipc_server_get_volume(ctx, *pid, &volume, &muted);
                if (vol_status == 0)
                {
                    vol_resp.status = kIPCStatusOK;
                    vol_resp.volume = volume;
                    vol_resp.muted = muted;
                    response_data = &vol_resp;
                    response_len = sizeof(vol_resp);
                }
                else
                {
                    status = kIPCStatusClientNotFound;
                }
            }
            else
            {
                status = kIPCStatusInvalidHeader;
            }
            break;
        }

    case kIPCCommandSetVolume:
        {
            if (header.payload_len >= sizeof(IPCSetVolumeRequest) && payload != NULL)
            {
                const IPCSetVolumeRequest* req = (const IPCSetVolumeRequest*)payload;
                status = ipc_server_set_volume(ctx, req->pid, req->volume);
                if (status != 0)
                    status = kIPCStatusClientNotFound;
            }
            else
            {
                status = kIPCStatusInvalidHeader;
            }
            break;
        }

    case kIPCCommandSetMute:
        {
            if (header.payload_len >= sizeof(IPCSetMuteRequest) && payload != NULL)
            {
                const IPCSetMuteRequest* req = (const IPCSetMuteRequest*)payload;
                status = ipc_server_set_mute(ctx, req->pid, req->muted);
                if (status != 0)
                    status = kIPCStatusClientNotFound;
            }
            else
            {
                status = kIPCStatusInvalidHeader;
            }
            break;
        }

    case kIPCCommandPing:
        {
            status = kIPCStatusOK;
            break;
        }

    case kIPCCommandListClients:
        {
            uint32_t client_count = 0;
            IPCClientEntry* clients = ipc_server_list_clients(ctx, &client_count);

            if (clients == NULL || client_count == 0)
            {
                // 没有客户端，返回空列表
                status = kIPCStatusOK;
                response_len = 0;
                response_data = NULL;
                break;
            }

            // 计算需要的缓冲区大小：每个客户端的信息（不包含next指针）
            size_t entry_size = sizeof(pid_t) + sizeof(float) + sizeof(bool) + sizeof(uint64_t) + 256;
            response_len = (uint32_t)(entry_size * client_count);
            response_data = malloc(response_len);
            response_needs_free = (response_data != NULL);

            if (response_data == NULL)
            {
                status = kIPCStatusInternalError;
                free(clients);
                break;
            }

            uint8_t* ptr = (uint8_t*)response_data;
            for (uint32_t i = 0; i < client_count; i++)
            {
                // 写入 PID
                memcpy(ptr, &clients[i].pid, sizeof(pid_t));
                ptr += sizeof(pid_t);

                // 写入音量
                memcpy(ptr, &clients[i].volume, sizeof(float));
                ptr += sizeof(float);

                // 写入静音状态
                memcpy(ptr, &clients[i].muted, sizeof(bool));
                ptr += sizeof(bool);

                // 写入连接时间
                memcpy(ptr, &clients[i].connected_at, sizeof(uint64_t));
                ptr += sizeof(uint64_t);

                // 写入应用名称（固定256字节）
                memcpy(ptr, clients[i].app_name, 256);
                ptr += 256;
            }
            status = kIPCStatusOK;
            free(clients);
            break;
        }

    default:
        status = kIPCStatusUnknownCommand;
        break;
    }

    send_response(client_fd, header.request_id, status, response_data, response_len);
    if (response_needs_free)
        free(response_data);
    free(payload);
}

// 运行服务端主循环
void ipc_server_run(IPCServerContext* ctx)
{
    if (ctx == NULL || ctx->epoll_fd < 0)
        return;

    struct kevent events[32];
    struct timespec timeout;

    while (ctx->running)
    {
        timeout.tv_sec = 1;
        timeout.tv_nsec = 0;

        int nfds = kevent(ctx->epoll_fd, NULL, 0, events, 32, &timeout);

        if (nfds < 0)
        {
            if (errno == EINTR)
                continue;
            perror("kevent");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            int fd = (int)events[i].ident;

            if (fd == ctx->listen_fd)
            {
                handle_new_connection(ctx);
            }
            else if (events[i].filter == EVFILT_READ)
            {
                handle_client_message(ctx, fd);
            }
        }
    }
}

// 停止服务端
void ipc_server_stop(IPCServerContext* ctx)
{
    if (ctx != NULL)
    {
        ctx->running = false;
    }
}

// 清理服务端资源
void ipc_server_cleanup(IPCServerContext* ctx)
{
    if (ctx == NULL)
        return;

    // 关闭所有连接
    // 【关键修复】使用互斥锁保护清理操作
    pthread_mutex_lock(&g_connections_mutex);
    while (g_connections != NULL)
    {
        ClientConnection* to_remove = g_connections;
        g_connections = g_connections->next;
        close(to_remove->fd);
        free(to_remove);
    }
    pthread_mutex_unlock(&g_connections_mutex);

    // 清理所有客户端条目
    while (ctx->clients != NULL)
    {
        IPCClientEntry* next = ctx->clients->next;
        free(ctx->clients);
        ctx->clients = next;
    }
    ctx->client_count = 0;

    // 关闭 kqueue
    if (ctx->epoll_fd >= 0)
    {
        close(ctx->epoll_fd);
        ctx->epoll_fd = -1;
    }

    // 关闭监听 socket
    if (ctx->listen_fd >= 0)
    {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;

        // 删除 socket 文件
        char socket_path[PATH_MAX];
        if (get_ipc_socket_path(socket_path, sizeof(socket_path)) == 0)
        {
            unlink(socket_path);
        }
    }

    g_server_ctx = NULL;
    printf("IPC 服务端已关闭\n");
}
