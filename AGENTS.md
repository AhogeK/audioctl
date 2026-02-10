# 🤖 Agent 开发架构与执行协议 (AGENTS.md)

本文件定义了项目开发的核心工作流。它融合了任务追踪（bd）、动态日志系统（DevLog）。

**⚠️agent 必须使用简体中文跟用户交互！⚠️**

---

## 🕒 时间与上下文真实性 (Time Integrity)

> ⚠️ **核心准则**：严禁在日志命名或内容中使用“记忆中”的年份。
>
> 在创建任何带有时间戳的文件（如 `BUGFIX_YYYY_MM_DD.md`）之前，Agent **必须**首先执行系统指令（如 `date` 或特定 Shell
> 指令）以获取当前的绝对真实时间。

---

## 🏗 任务追踪指令参考 (Issue Tracking)

项目使用 **bd** (beads) 系统作为唯一的任务状态真理来源。

* **`bd onboard`**: 初始化项目协作环境。
* **`bd ready`**: 检索当前可执行的待办事项。
* **`bd show <id>`**: 深度解析特定任务的需求背景。
* **`bd update <id> --status in_progress`**: 正式激活任务并锁定开发权。
* **`bd close <id>`**: 完成任务，触发后续归档流程。
* **`bd sync`**: 强制同步本地任务状态至远端。

---

## 📂 结构化开发日志协议 (DevLog Protocol)

Agent 必须在 `/devlog` 目录下维护实时日志。**所有描述必须使用简体中文。**

### 1. 增量功能日志 (`devlog/activity/`)

* **触发条件**: 功能迭代、架构优化、依赖库变更。
* **命名规范**: `ACTIVITY_[系统指令获取的日期].md`
* **关键内容**: 变更动机、技术选型（如为何引入特定 MCP Server）、受影响的组件、待解决的边缘问题。

### 2. 故障修复日志 (`devlog/fix/`)

* **触发条件**: 逻辑漏洞修复、性能调优、RAG 幻觉修正。
* **命名规范**: `FIX_[系统指令获取的日期].md`
* **关键内容**: 场景复现路径、根因分析 (Root Cause)、补丁逻辑说明、防回归测试方案。

---

## 🔨 构建与测试指令

### 构建命令 (Build Commands)

```bash
# Debug 模式构建 (默认)
mkdir -p cmake-build-debug
cd cmake-build-debug
cmake .. -DDEBUG_MODE=ON
ninja

# Release 模式构建
mkdir -p cmake-build-release
cd cmake-build-release
cmake .. -DDEBUG_MODE=OFF
ninja

# 一键安装脚本
./scripts/install.sh install
./scripts/install.sh install --release
```

### 测试命令 (Test Commands)

```bash
# 运行所有测试
cd cmake-build-debug
ninja test

# 或使用 ctest
ctest

# 运行单个测试 (注意: 项目使用统一的测试可执行文件)
cd cmake-build-debug/tests
./test_virtual_audio_device
```

### 安装与部署

```bash
# 安装驱动到系统
sudo ninja install
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
```

---

## 📝 代码风格指南

### 语言标准

- **C 标准**: C11 (`CMAKE_C_STANDARD 11`)
- **Objective-C 标准**: C11 (`CMAKE_OBJC_STANDARD 11`)
- **ARC**: Objective-C 代码启用 `-fobjc-arc`

### 编译警告

始终启用严格警告:

```cmake
add_compile_options(-Wall -Wextra)
```

### 代码格式规范

#### 1. 缩进与空格

- **缩进**: 4 个空格 (不使用 Tab)
- **换行**: Unix 风格 (LF)
- **大括号**: Allman 风格，单独成行

```c
// 正确
if (condition)
{
    do_something();
}

// 错误
if (condition) {
    do_something();
}
```

#### 2. 命名规范

| 类型        | 命名风格              | 示例                          |
|-----------|-------------------|-----------------------------|
| 函数 (公开)   | snake_case        | `aggregate_device_create()` |
| 函数 (私有静态) | snake_case        | `get_device_uid()`          |
| 结构体       | PascalCase        | `AudioDeviceInfo`           |
| 枚举        | PascalCase + k 前缀 | `kDeviceTypeInput`          |
| 宏/常量      | UPPER_SNAKE_CASE  | `ROUTER_PID_FILE`           |
| 全局变量      | g_ 前缀 (避免使用)      | -                           |
| 局部变量      | snake_case        | `device_count`              |

#### 3. 头文件组织

```c
// 1. Created by 注释
// Created by AhogeK on 11/20/24.

// 2. Include Guard
#ifndef AUDIO_CONTROL_H
#define AUDIO_CONTROL_H

// 3. 系统头文件
#include <CoreAudio/CoreAudio.h>
#include <CoreServices/CoreServices.h>

// 4. 项目头文件 (使用引号)
#include "constants.h"

// 5. 代码内容

#endif // AUDIO_CONTROL_H
```

#### 4. 源文件组织

```c
// 1. Created by 注释
// Created by AhogeK on 11/20/24.

// 2. 项目头文件优先 (引号)
#include "audio_control.h"
#include "audio_apps.h"

// 3. 系统头文件 (尖括号)
#include <signal.h>
#include <mach-o/dyld.h>
```

#### 5. 函数定义

```c
// 函数声明 (头文件)
OSStatus setDeviceVolume(AudioDeviceID deviceId, Float32 volume);

// 函数定义 (源文件)
OSStatus setDeviceVolume(AudioDeviceID deviceId, Float32 volume)
{
    // 实现代码
}
```

#### 6. 错误处理

- 使用 `OSStatus` 返回错误码 (macOS 标准)
- 检查所有 `OSStatus` 返回值
- 使用 `noErr` 常量判断成功

```c
OSStatus status = some_function();
if (status != noErr)
{
    fprintf(stderr, "Error: %d\n", status);
    return status;
}
```

#### 7. 注释规范

- 使用 `//` 单行注释
- 关键函数添加功能注释
- 复杂逻辑添加解释注释

```c
// 获取音频设备属性
static OSStatus getAudioProperty(AudioDeviceID deviceId, ...)
{
    // 实现细节说明
}
```

#### 8. 内存管理

- C 代码: 使用 `malloc`/`free`，注意检查 NULL
- Objective-C 代码: 使用 ARC，无需手动 retain/release

#### 9. 布尔值

- 使用 `<stdbool.h>` 的 `bool` 类型
- 使用 `true`/`false` 而非 `YES`/`NO` (C 代码)

---

## 🚀 任务完结工作流 (Landing the Plane)

* **遗留扫描**: 为任何未完成的子任务或待优化的代码块创建新的 `bd` Issue。
* **质量门控**: 必须运行现有的测试套件和构建，确保交付的代码是健康的。
    ```bash
    cd cmake-build-debug
    ninja
    ninja test
    ```
* **日志落盘**: 根据当前任务类型，在 `/devlog` 中同步更新日志文件，文件名需严格基于系统实时时间。
* **禁止强制推送**: 代码的提交推送这一步交给用户，用户需要先review你的代码才行，除非用户主动提出直接提交推送PR等这类操作的请求。
* **环境净空**: 清理 Stash 缓存，剪枝已失效的远程分支，确保工作区整洁。
* **上下文交接**: 在 Session 结束时提供简洁的下一步建议。

---

## 🚫 关键约束 (Critical Rules)

* **动态时间前置**: 任何涉及日期的输出，必须以 `date` 指令的结果为准，拒绝 hardcode。
* **拒绝“推迟推送”**: 禁止向用户发送“我已准备好推送”等确认请求，Agent 应具备自主解决简单冲突并完成推送的能力。
* **Context-as-Code**: `/devlog` 文件夹不仅是日志，更是 Agent 跨 Session 维持长期记忆的核心资产。
* **专业表达**: 使用地道的简体中文进行技术描述，确保文档的工程严谨性。
* **必须构建和测试**: 每次代码变更后必须执行 `ninja` 和 `ninja test`。
