# 🤖 Agent 开发架构与执行协议 (AGENTS.md)

本文件定义了项目开发的核心工作流。它融合了任务追踪（bd）、动态日志系统（DevLog）以及**上下文即代码（Context-as-Code）** 的工程理念。

⚠️**核心指令：Agent 必须使用简体中文与用户交互！无论是回复、思考链 (Chain of Thought)、Todo 工具内容还是日志记录，
严禁使用英文作为主语言**⚠️

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

## 🏗 任务追踪与执行规范 (Task Execution)

项目使用 **bd** (beads) 系统管理任务，并要求 Agent 活用内置的 **Todo** 工具进行细粒度追踪。

1. **bd (Beads) 指令参考**:
    - **`bd onboard`**: 初始化项目协作环境。
    - **`bd ready`**: 检索当前可执行的待办事项。
    - **`bd show <id>`**: 深度解析特定任务的需求背景。
    - **`bd update <id> --status in_progress`**: 正式激活任务并锁定开发权。
    - **`bd close <id>`**: 完成任务，触发后续归档流程。
    - **`bd sync`**: 强制同步本地任务状态至远端。

2. **活用 Todo 工具**:
    - 在开始任何复杂任务前，**必须**先使用 `todowrite` 创建任务清单。
    - 任务分解应遵循“小而美”原则：每一步必须足够具体（如“修改 Makefile 添加 X 标志”而非“优化构建”）。
    - **严禁跳步**: 每完成一个子任务，立即更新状态为 `completed`。
    - 遵循“计划 -> 执行 -> 验证”闭环。

3. **按部就班的流程**:
    - **Understand**: 先读取关键文件（README, include/*.h, DevLog），确认当前状态。
    - **Plan**: 在 DevLog 中记录详细的实施计划。
    - **Execute**: 严格按照计划执行，遇到阻碍及时修正计划。
    - **Verify**: 每个阶段完成后必须进行构建测试或功能验证。

4. **阶段性停顿与提交协议 (Phase Checkpoints)**:
    - **自动保存意识**: 严禁在没有中间确认的情况下进行超大规模的代码改动。
    - **主动停顿**: 每当完成一个具有独立意义的子任务（如：成功定义了 IPC 协议、完成了一个核心函数的重构并通过编译）时，Agent
      **必须**主动停顿，展示当前变更并建议用户进行 `git commit`。
    - **防混乱回退**: 坚持“小步快跑”原则。频繁的小规模提交能确保在后续步骤出错时，系统可以安全回滚到最近的稳定节点，避免代码混乱。

5. **系统洁净度与残留管理 (System Cleanliness)**:
    - **卸载闭环**: 在重构或废弃功能时，Agent 必须主动检查并清理系统级残留，包括但不限于：
        - `~/Library/LaunchAgents/` 和 `/Library/LaunchDaemons/` 中的 `.plist` 文件。
        - `/usr/local/bin/` 或 `/usr/local/libexec/` 中的残留二进制。
        - `/tmp/` 中的 PID 文件和 Socket 文件。
    - **主动检查**: 在创建新的系统服务前，先扫描是否存在同名的旧服务并执行 `launchctl unload`。

***

## 🔌 通信架构规范 (IPC Architecture)

### 核心原则

- **规避签名限制**: 优先使用 **Unix Domain Socket** 或 **Localhost TCP** 进行跨进程通信，避免使用受 Team ID 签名严格约束的
  Apple Native XPC (NSXPCConnection)。
- **Socket 路径**: 统一存放在 `~/Library/Application Support/audioctl/` 下，确保普通用户权限即可操作。

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

以[.clang-format](.clang-format)为准

同样用户有在使用sonar来规范代码

#### 实时音频线程约束 (Real-time Constraints)

> ⚠️ **在 `AudioDeviceIOProc` 回调中严禁执行以下操作**：

* **分配内存**: 禁止 `malloc`, `free`, `ObjC对象创建`。
* **文件 I/O**: 禁止 `printf`, `logging`, 文件读写。
* **锁操作**: 禁止使用互斥锁 (`pthread_mutex`)，应使用原子操作 (`OSAtomic`, `std::atomic`) 或无锁循环队列。
* **ObjC 消息发送**: 尽量避免在回调中调用 ObjC 方法，除非确定不会触发锁或内存分配。

***

## 🚀 任务完结工作流 (Landing the Plane)

* **遗留扫描**: 为任何未完成的子任务或待优化的代码块创建新的 `bd` Issue。
* **质量门控**: 必须运行现有的测试套件和构建，确保交付的代码是健康的。
* **日志落盘**: 根据当前任务类型，在 `/devlog` 中同步更新日志文件，文件名需严格基于系统实时时间。
* **知识沉淀**: 如果发现了新的系统特性或坑点，更新 `SKILL.md` 或项目的 `README.md`。
* **禁止强制推送**: 代码的提交推送这一步交给用户，用户需要先review你的代码才行，除非用户主动提出直接提交推送PR等这类操作的请求。
* **环境净空**: 清理 Stash 缓存，剪枝已失效的远程分支，确保工作区整洁。
* **上下文交接**: 在 Session 结束时提供简洁的下一步建议。

任务提供人工MCP，如果你有询问其他更高级AI或者看/查询什么网站仓库，请停下来告知我，由我回复你结果，如果有需要我问的内容可覆写[文件](devlog/ask_expert.txt)

***

## 🚫 关键约束 (Critical Rules)

* **动态时间前置**: 任何涉及日期的输出，必须以 `date` 指令的结果为准，拒绝 hardcode。
* **Context-as-Code**: `/devlog` 文件夹不仅是日志，更是 Agent 跨 Session 维持长期记忆的核心资产。
* **专业表达**: 使用地道的简体中文进行技术描述，确保文档的工程严谨性。
* **必须构建和测试**: 每次代码变更后必须进行严格的构建与测试。
* **权限边界**: 严禁在没有用户明确指令的情况下修改 `.gitignore`、CI 配置文件或删除用户数据。

***

## ⚠️ 系统级服务操作规范 (System Service Safety)

### 📚 事故案例库 (Incident Reference)

**详情 **:
[1](devlog/incidents/INCIDENT_2026_02_11_CoreAudio_System_Crash.md)
[2](devlog/incidents/INCIDENT_2026_02_12_CoreAudio_Install_Crash.md)

**教训**:

- 40 分钟内重启 15+ 次导致系统负载飙升至 200+
- 未监控资源占用，无视系统警告信号
- 发现超时后错误地继续重启，形成恶性循环

**后续**: 用户被迫强制重启电脑，丢失未保存工作。