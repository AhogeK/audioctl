# 🤖 Agent 开发架构与执行协议 (AGENTS.md)

本文件定义了项目开发的核心工作流。它融合了任务追踪（bd）、动态日志系统（DevLog）以及**上下文即代码（Context-as-Code）** 的工程理念。

**⚠️ 核心指令：Agent 必须使用简体中文与用户交互！无论是回复、思考链 (Chain of Thought) 还是日志记录，严禁使用英文作为主语言。⚠️
**

***

## 🕒 时间与上下文真实性 (Time Integrity)

> ⚠️ **核心准则**：严禁在日志命名或内容中使用“记忆中”的年份。
>
> 在创建任何带有时间戳的文件（如 `BUGFIX_YYYY_MM_DD.md`）之前，Agent **必须**首先执行系统指令（如 `date` 或特定 Shell
> 指令）以获取当前的绝对真实时间。

***

## 🧠 认知与行为准则 (Cognitive & Behavioral Standards)

Agent 在执行任务前，必须遵循以下认知流程：

1. **环境感知 (Context Awareness)**: 在编写代码前，优先读取 `README.md` 理解架构，读取 `include/*.h` 理解现有 API 契约。
2. **防御性思维 (Defensive Mindset)**: 默认假设系统资源有限、音频设备可能随时断开。
3. **最小破坏原则 (Minimal Invasion)**: 优先复用现有函数，避免产生重复造轮子（DRY）的代码。
4. **实时性敬畏 (Real-time Respect)**: 在处理音频回调（IOProc）时，时刻警惕耗时操作。

***

## 🏗 任务追踪指令参考 (Issue Tracking)

项目使用 **bd** (beads) 系统作为唯一的任务状态真理来源。

* **`bd onboard`**: 初始化项目协作环境。
* **`bd ready`**: 检索当前可执行的待办事项。
* **`bd show <id>`**: 深度解析特定任务的需求背景。
* **`bd update <id> --status in_progress`**: 正式激活任务并锁定开发权。
* **`bd close <id>`**: 完成任务，触发后续归档流程。
* **`bd sync`**: 强制同步本地任务状态至远端。

***

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

### 3. 架构决策记录 (`devlog/decisions/`)

* **触发条件**: 涉及系统级重构、核心数据结构变更或引入新的底层依赖。
* **命名规范**: `ADR_[系统指令获取的日期]_[决策简述].md`
* **关键内容**: 背景 (Context)、选项 (Options)、决策 (Decision)、后果 (Consequences)。

***

## 📝 代码风格指南

### 语言标准

* **C 标准**: C11 (`CMAKE_C_STANDARD 11`)
* **Objective-C 标准**: C11 (`CMAKE_OBJC_STANDARD 11`)
* **ARC**: Objective-C 代码启用 `-fobjc-arc`

### 代码格式规范

#### 1. 缩进与空格

* **缩进**: 4 个空格 (不使用 Tab)
* **换行**: Unix 风格 (LF)
* **大括号**: Allman 风格，单独成行

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

#### 6. 错误处理与调试

* 使用 `OSStatus` 返回错误码 (macOS 标准)。
* 检查所有 `OSStatus` 返回值。
* 使用 `noErr` 常量判断成功。
* **CoreAudio 特有**: 遇到 `kAudioHardware...` 错误时，尝试打印 4 字符错误码 (FourCC)。

```c
OSStatus status = some_function();
if (status != noErr)
{
    // 推荐添加 4CC 转换逻辑以便于调试
    char fourcc[5] = {0};
    *(UInt32 *)fourcc = CFSwapInt32HostToBig(status);
    fprintf(stderr, "Error: %d ('%s')\n", status, fourcc);
    return status;
}
```

#### 7. 注释规范

* 使用 `//` 单行注释。
* 关键函数添加功能注释。
* 复杂逻辑添加解释注释。
* **FIXME/TODO**: 对于临时代码，必须标记 `// TODO: [说明]` 并建议生成对应的 `bd` issue。

#### 8. 内存管理

* **C 标准内存**: 使用 `malloc`/`free`，注意检查 NULL。
* **CoreFoundation 对象**: 严格遵循 "Create/Copy rule" (需要 Release) 和 "Get rule" (不需要 Release)。
    * 示例: `CFStringCreate...` 必须配合 `CFRelease`。
* **Objective-C 代码**: 使用 ARC，无需手动 retain/release。

#### 9. 实时音频线程约束 (Real-time Constraints)

> ⚠️ **在 `AudioDeviceIOProc` 回调中严禁执行以下操作**：

* **分配内存**: 禁止 `malloc`, `free`, `ObjC对象创建`。
* **文件 I/O**: 禁止 `printf`, `logging`, 文件读写。
* **锁操作**: 禁止使用互斥锁 (`pthread_mutex`)，应使用原子操作 (`OSAtomic`, `std::atomic`) 或无锁循环队列。
* **ObjC 消息发送**: 尽量避免在回调中调用 ObjC 方法，除非确定不会触发锁或内存分配。

#### 10. 布尔值

* 使用 `<stdbool.h>` 的 `bool` 类型。
* 使用 `true`/`false` 而非 `YES`/`NO` (C 代码)。

***

## 🔨 构建与测试规范 (Build & Test Protocol)

> ⚠️ **严禁手动构建**: Agent 不应手动执行 `mkdir cmake-build-debug` 或直接调用 `cmake`。

* **标准化构建**: 必须使用项目提供的脚本 `./scripts/install.sh` 进行构建和安装。
    * 构建并安装: `./scripts/install.sh install`
    * 仅清理: 在脚本中未直接提供，可删除 `cmake-build-*` 目录。
* **验证流程**:

    1. 代码变更后，运行 `./scripts/install.sh install --no-coreaudio-restart` 进行构建验证。
    2. 仅在确实需要验证运行时行为且获得用户明确允许时，才重启 CoreAudio。
  3. **自测闭环**: 在提交代码前，必须编写或运行至少一个相关的测试用例（参考 `tests/` 目录）。

***

## 🚀 任务完结工作流 (Landing the Plane)

* **遗留扫描**: 为任何未完成的子任务或待优化的代码块创建新的 `bd` Issue。
* **质量门控**: 必须运行现有的测试套件和构建，确保交付的代码是健康的。
* **日志落盘**: 根据当前任务类型，在 `/devlog` 中同步更新日志文件，文件名需严格基于系统实时时间。
* **知识沉淀**: 如果发现了新的系统特性或坑点，更新 `SKILL.md` 或项目的 `README.md`。
* **禁止强制推送**: 代码的提交推送这一步交给用户，用户需要先review你的代码才行，除非用户主动提出直接提交推送PR等这类操作的请求。
* **环境净空**: 清理 Stash 缓存，剪枝已失效的远程分支，确保工作区整洁。
* **上下文交接**: 在 Session 结束时提供简洁的下一步建议。

***

## 🚫 关键约束 (Critical Rules)

* **动态时间前置**: 任何涉及日期的输出，必须以 `date` 指令的结果为准，拒绝 hardcode。
* **Context-as-Code**: `/devlog` 文件夹不仅是日志，更是 Agent 跨 Session 维持长期记忆的核心资产。
* **专业表达**: 使用地道的简体中文进行技术描述，确保文档的工程严谨性。
* **必须构建和测试**: 每次代码变更后必须进行严格的构建与测试。
* **权限边界**: 严禁在没有用户明确指令的情况下修改 `.gitignore`、CI 配置文件或删除用户数据。
