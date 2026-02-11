//
// Created by AhogeK on 02/12/26.
//

#include "ipc/ipc_protocol.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

int get_ipc_socket_path(char* path, size_t path_size)
{
    char support_dir[PATH_MAX];
    if (get_support_directory(support_dir, sizeof(support_dir)) != 0)
    {
        return -1;
    }

    int written = snprintf(path, path_size, "%s/%s", support_dir, IPC_SOCKET_FILENAME);
    if (written < 0 || (size_t)written >= path_size)
    {
        fprintf(stderr, "错误: 路径缓冲区太小\n");
        return -1;
    }

    return 0;
}

void ipc_init_header(IPCMessageHeader* header, uint16_t command, uint32_t payload_len,
                     uint32_t request_id)
{
    if (!header) return;

    header->magic = IPC_MAGIC;
    header->version = IPC_PROTOCOL_VERSION;
    header->command = command;
    header->payload_len = payload_len;
    header->request_id = request_id;
}

bool ipc_validate_header(const IPCMessageHeader* header)
{
    if (!header) return false;

    // 验证魔数
    if (header->magic != IPC_MAGIC)
    {
        return false;
    }

    // 验证版本
    if (header->version != IPC_PROTOCOL_VERSION)
    {
        return false;
    }

    // 验证负载长度
    if (header->payload_len > IPC_MAX_PAYLOAD_SIZE)
    {
        return false;
    }

    // 验证指令有效性
    switch (header->command)
    {
    case kIPCCommandRegister:
    case kIPCCommandUnregister:
    case kIPCCommandGetVolume:
    case kIPCCommandSetVolume:
    case kIPCCommandGetMute:
    case kIPCCommandSetMute:
    case kIPCCommandListClients:
    case kIPCCommandPing:
    case kIPCCommandResponse:
    case kIPCCommandError:
        return true;
    default:
        return false;
    }
}

const char* ipc_status_to_string(int32_t status)
{
    switch (status)
    {
    case kIPCStatusOK:
        return "OK";
    case kIPCStatusInvalidHeader:
        return "Invalid message header";
    case kIPCStatusUnknownCommand:
        return "Unknown command";
    case kIPCStatusPayloadTooLarge:
        return "Payload too large";
    case kIPCStatusClientNotFound:
        return "Client not found";
    case kIPCStatusInvalidVolume:
        return "Invalid volume value";
    case kIPCStatusServiceUnavailable:
        return "Service unavailable";
    case kIPCStatusInternalError:
        return "Internal error";
    default:
        return "Unknown status";
    }
}
