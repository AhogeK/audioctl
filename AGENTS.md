# 🤖 Audioctl Agent Protocols

> **注意**: 本项目采用 **OpenCode** 标准进行多 Agent 管理，并深度融合 **Vibe Coding** 的状态机心智模型。详细的角色定义与权限配置请参阅
`.opencode/agents/` 目录。

所有接入本项目的 AI Agent（无论是否通过特定配置加载），都**必须**严格遵守以下全局基准协议。

---

## 🚨 全局核心指令 (Global Meta-Instructions)

* **语言强制**: 无论你的默认心智设定是什么，在本项目中**必须全程使用简体中文**进行思考链推演与交互输出。
* **安全红线**: 涉及 **CoreAudio (`coreaudiod`)** 的底层重启操作极其危险，严禁在任何自动化重试的高频循环中执行。
* **代码契约**: 遇到系统级 API 的逻辑冲突时，必须以 `include/*.h` 的头文件声明为最高真理，拒绝大模型的幻觉猜测。

---

## 🧠 记忆体与状态同步 (Memory Bank Protocols)

本项目拒绝 AI 依赖易失性的对话窗口。AI 必须将 `memory-bank/` 目录作为“外部海马体”，并严格遵循以下上下文生命周期：

* **唤醒自检 (On-Boot)**: 开始任何工程任务前，**必须静默读取** `memory-bank/progress.md` 与 `implementation-plan.md`
  ，确认当前所处战区与下一步行动。
* **决策快照 (On-Decision)**: 发生重大架构选择、技术栈妥协或底层 Bug 规避时，必须追加至 `memory-bank/decisions.md`
  ，清晰记录“技术上下文与最终后果”。
* **休眠存档 (On-Sleep)**: 完成阶段性交付后，必须主动更新 `progress.md` 的里程碑打勾状态，确保多轮对话或多 Agent
  协作时不发生上下文断层。

> ⚠️ **边界守卫**: `devlog/` 目录专用于生成面向**人类开发者**阅读的 Release Note 或项目进度审查，AI **严禁**
> 将其作为自身逻辑连贯性的记忆读取源。

---

## ☁️ 云端操作权限（Human-only）

本项目中，涉及远端仓库或云平台的操作必须由**人类**执行，AI 不得代为完成或自动化触发，包括但不限于：

- `git commit` / `git push` / `git pull --rebase` / `git merge`
- 创建或更新 PR（GitHub/GitLab 等）
- 任何需要上传、发布、同步到远端的动作

AI 的职责是：

- 在每个“可提交的小步骤”完成后，生成清晰的变更说明与测试清单
- 停下并询问人类是否执行测试、是否提交、提交信息如何写
- 若需要拉取远端更新，明确提醒人类先完成同步，再继续开发

## ⛳ 小步提交确认（Checkpoint Gate）

任何达到“可以提交”的粒度后，AI 必须进入 Checkpoint：

- 列出本次变更点（面向 code review）
- 列出建议测试点与命令（面向验证）
- 等待人类确认后才继续下一步工作

详细协议参见 `.opencode/commands/checkpoint.md`。

## 📋 结构化任务管理 (Beads Integration)

除了 `memory-bank` 的上下文记忆，本项目使用 **bd (beads)** 进行工程任务的结构化跟踪：

* **任务发现**: 通过 `bd ready` 查询无阻塞的可执行任务（具备依赖树意识）。
* **多会话协作**: bd 数据库（`.beads/issues.jsonl`）存储在 Git 中，支持多 Agent 或多轮会话的状态同步。
* **优先级驱动**: 任务带有 P0/P1/P2 优先级，AI 应优先处理高优先级的 `ready` 任务。

**关键命令**：

- `bd ready` - 查看可执行任务
- `bd update <id> --status in_progress` - 认领任务
- `bd close <id>` - 完成任务
- `bd sync` - 同步至 Git（必须在 sleep 前执行）

详细协议请参阅 `.opencode/commands/bd.md`。

---

## 📂 架构战区映射 (Architecture Map)

在特定领域进行开发时，请遵循以下架构规范索引：

| 领域        | 核心驱动文件                                | 核心职责说明                           |
|:----------|:--------------------------------------|:---------------------------------|
| **持久化记忆** | `memory-bank/*.md`                    | 存放 AI 工作流的进度锚点、战术地图与架构决策。        |
| **底层架构**  | `.opencode/agents/audio_architect.md` | CoreAudio 交互、HAL 层规范与驱动开发的实时性红线。 |
| **任务分发**  | `.beads/README.md`                    | 基于 `bd` 命令行工具的工程任务领取、流转与状态更新。    |
| **构建系统**  | `CMakeLists.txt`                      | C/Objective-C 混合工程的编译指令与测试断言门控。  |

---

## 🛡️ 通用开发红线

* **格式化约束**: 提交任何代码前必须符合 LLVM 风格（强制依赖 `clang-format` 自动化对齐）。
* **实时性约束**: 在 `AudioDeviceIOProc` 回调中**绝对禁止**内存分配（`malloc`/`new`）与阻塞性锁操作（应使用无锁数据结构或
  Atomics）。
* **生态纯洁性**: 严禁引入 Web 技术栈（如 Electron、Tauri 等），坚持 macOS 极致原生性能与体验。

---

> *For advanced agent configuration, capability boundaries, and tool definitions, strictly refer
to `.opencode/agents/audio_architect.md`*