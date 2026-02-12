# 🤖 Agent 开发架构与执行协议 (AGENTS.md)

本文件定义了项目开发的核心工作流。它融合了任务追踪（bd）、动态日志系统（DevLog）以及**上下文即代码（Context-as-Code）** 的工程理念。

⚠️**核心指令：Agent 必须使用简体中文与用户交互！无论是回复、思考链 (Chain of Thought)、Todo 工具内容还是日志记录，严禁使用英文作为主语言
**⚠️

---

## 🔧 构建与测试命令

### 基本构建

```bash
# 使用 install.sh 脚本（推荐）
./scripts/install.sh install              # Debug 模式
./scripts/install.sh install --release    # Release 模式
./scripts/install.sh install --no-coreaudio-restart  # 不重启 CoreAudio

# 手动 CMake 构建
cmake -B cmake-build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C cmake-build-debug
```

### 运行测试

```bash
# 运行所有测试
cd cmake-build-debug && ctest --output-on-failure

# 运行单个测试可执行文件
./cmake-build-debug/tests/test_virtual_audio_device

# 运行特定测试类别（通过 test_main.c 中的函数）
# 注：当前测试框架不支持单独运行单个测试函数，需修改 test_main.c
```

### 代码格式化

```bash
# 使用 clang-format 格式化所有源文件
find src include tests -name "*.c" -o -name "*.h" -o -name "*.m" | xargs clang-format -i

# 检查格式（不修改文件）
find src include tests -name "*.c" -o -name "*.h" -o -name "*.m" | xargs clang-format --dry-run --Werror
```

---

## 📝 代码风格指南

### 语言标准

- **C 标准**: C11 (`CMAKE_C_STANDARD 11`)
- **Objective-C 标准**: C11 (`CMAKE_OBJC_STANDARD 11`)
- **ARC**: Objective-C 代码启用 `-fobjc-arc`

### 格式化规范

- **基于**: LLVM style（详见 `.clang-format`）
- **缩进**: 4 空格，不使用 Tab
- **行宽**: 120 字符
- **大括号**: K&R 风格（函数大括号换行，控制语句大括号同行）
- **指针**: 左对齐 (`Type* ptr` 而非 `Type *ptr`)

### 命名约定

- **文件**: 蛇形命名 (`virtual_device_manager.c`, `audio_control.h`)
- **函数**: 模块前缀 + 蛇形命名 (`virtual_device_get_info`, `aggregate_device_create`)
- **类型**: 蛇形命名 + `_t` 后缀（如需要）(`AppVolumeEntry`, `VirtualDeviceInfo`)
- **宏/常量**: 全大写 + 下划线 (`MAX_APP_ENTRIES`, `VIRTUAL_DEVICE_UID`)
- **结构体成员**: 蛇形命名，清晰描述用途
- **私有函数**: `static` 修饰，以模块名开头

### 头文件规范

```c
#ifndef AUDIOCTL_FILENAME_H
#define AUDIOCTL_FILENAME_H

#include <系统头文件>      // Priority 1: 系统头文件
#include "项目头文件"      // Priority 2: 项目内部头文件

// 代码内容

#endif // AUDIOCTL_FILENAME_H
```

### 错误处理模式

- **OSStatus**: 使用 `noErr` 或 `kAudioHardwareNoError` 检查成功
- **布尔函数**: 返回 `true` 表示成功，`false` 表示失败
- **错误传播**: 立即返回错误状态，不隐藏失败

```c
OSStatus status = SomeAudioFunction();
if (status != noErr) return status;  // 立即传播错误
```

### 实时音频线程约束 (Real-time Constraints)

⚠️ **在 `AudioDeviceIOProc` 回调中严禁执行以下操作**:

- **分配内存**: 禁止 `malloc`, `free`, `ObjC 对象创建`
- **文件 I/O**: 禁止 `printf`, `logging`, 文件读写
- **锁操作**: 禁止使用互斥锁 (`pthread_mutex`)，使用原子操作或无锁队列
- **ObjC 消息发送**: 尽量避免在回调中调用 ObjC 方法

---

## 🧠 认知与行为准则

Agent 在执行任务前，必须遵循以下认知流程：

1. **环境感知**: 优先读取 `README.md` 理解架构，读取 `include/*.h` 理解 API 契约
2. **防御性思维**: 默认假设系统资源有限、音频设备可能随时断开
3. **最小破坏原则**: 优先复用现有函数，避免重复造轮子（DRY）
4. **实时性敬畏**: 在处理音频回调（IOProc）时，时刻警惕耗时操作

---

## 🏗 任务追踪与执行规范

项目使用 **bd** (beads) 系统管理任务：

- `bd onboard`: 初始化项目协作环境
- `bd ready`: 检索当前可执行的待办事项
- `bd show <id>`: 深度解析特定任务的需求背景
- `bd update <id> --status in_progress`: 正式激活任务
- `bd close <id>`: 完成任务

### 活用 Todo 工具

- 开始复杂任务前，**必须**使用 `todowrite` 创建任务清单
- 任务分解遵循"小而美"原则
- **严禁跳步**: 每完成子任务立即更新状态为 `completed`
- 遵循"计划 -> 执行 -> 验证"闭环

### 阶段性停顿与提交协议

- **自动保存意识**: 严禁在没有中间确认的情况下进行超大规模代码改动
- **主动停顿**: 每当完成独立子任务时，**必须**主动停顿，展示变更并建议 `git commit`
- **防混乱回退**: 坚持"小步快跑"原则，频繁小规模提交

---

## 📂 结构化开发日志协议 (DevLog)

Agent 必须在 `/devlog` 目录下维护实时日志。**所有描述必须使用简体中文。**

### 日志类型与命名

- **增量功能**: `devlog/activity/ACTIVITY_[日期].md`
- **故障修复**: `devlog/fix/FIX_[日期].md`
- **架构决策**: `devlog/decisions/ADR_[日期]_[简述].md`

---

## 🔌 通信架构规范

- **规避签名限制**: 优先使用 **Unix Domain Socket** 或 **Localhost TCP**
- **Socket 路径**: 统一存放在 `~/Library/Application Support/audioctl/` 下

---

## ⚠️ CoreAudio 重启安全规范

**严禁频繁重启 CoreAudio**，必须遵守：

- **重启间隔**: 每次重启后至少等待 10-15 秒，观察 CPU 负载
- **监控指标**: 使用 `top` 或 `htop` 查看整体系统负载
- **禁止重启条件**: coreaudiod CPU > 50% 或系统整体卡顿时，**禁止再次重启**
- **重启前检查**: 确认无音频应用运行（Safari, FaceTime 等），提醒用户保存工作
- **重启后验证**: 检查虚拟设备是否正常加载，确认无崩溃报告

**事故教训**: 40 分钟内重启 15+ 次导致系统负载飙升至 200+，用户被迫强制重启丢失工作。

---

## 🚀 任务完结工作流

- **遗留扫描**: 为未完成的子任务创建新的 `bd` Issue
- **质量门控**: 必须运行构建和测试，确保代码健康
- **日志落盘**: 根据任务类型在 `/devlog` 中同步更新日志
- **禁止强制推送**: 代码提交推送交给用户 review
- **上下文交接**: Session 结束时提供简洁的下一步建议

---

## 🚫 关键约束

- **动态时间前置**: 任何涉及日期的输出，必须以 `date` 指令结果为准
- **必须构建和测试**: 每次代码变更后必须进行严格的构建与测试
- **权限边界**: 严禁在没有用户明确指令的情况下修改 `.gitignore`、CI 配置文件或删除用户数据
