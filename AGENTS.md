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

## ☁️ 云端与版本控制权限（Human-only）

本项目中，涉及版本控制（Git）与云平台的操作必须严格由**人类**执行。AI **严禁**擅自执行以下命令：

- `git commit` (严禁！即使是本地提交)
- `git push`
- `git pull` / `git merge` / `git rebase`
- 创建或更新 PR（GitHub/GitLab 等）

### AI 的正确交付姿势

1. **代码清理**: 在任务完成后，AI 应主动清理调试代码（如 `printf`, `os_log` 调试信息）、临时文件与注释。
2. **状态检查**: 运行 `git status` 告知用户哪些文件已修改。
3. **提交建议**: 生成推荐的 Commit Message 供用户参考，**但绝不执行 commit 命令**。
4. **等待指令**: 除非用户明确发出指令（如“请帮我提交代码”），否则 AI 必须停在 `git add` 之前或之后，把提交权交给人类。

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

## 🎯 AI 与人类职责分工

### AI 职责（我）

1. **编译检查**: 确保代码能编译通过
2. **单测运行**: 确保单元测试通过
3. **日志分析**: 当用户测试出现问题时，负责查看日志分析原因

### 人类职责（你）

1. **实际安装**: 运行 `./scripts/install.sh install`（注意：必须重启 coreaudiod）
2. **实际测试**: 切换设备、播放音频等实际操作
3. **问题反馈**: 告诉 AI 出现了什么问题（如"没声音"）

### 关键提醒

- **安装必须重启 coreaudiod**: 不能使用 `--no-coreaudio-restart`，否则新代码不会生效
- 安装后需要手动重启 coreaudiod 或等待系统加载新驱动

---

## 🔍 调试日志查看流程

当用户测试出现问题时，AI 需要按以下步骤查看日志：

### 1. 查看 coreaudiod 中的错误

```bash
/usr/bin/log show --predicate 'process == "coreaudiod"' --last 30s 2>&1 | grep -iE "timestamp|VADriver|anchor|sample" | tail -20
```

### 2. 查看驱动调试日志（需要 sudo）

```bash
sudo log show --predicate 'message contains "GetZTS"' --last 30s 2>&1 | head -10
```

（"GetZTS" 是自定义的日志关键词，可在代码中修改）

### 3. 关键指标解读

- `sample diff: 1024` = 时间戳跳跃了 2 个周期（应该 512）
- `cycle=1909` = 当前处于第 1909 个周期
- `sample=977408` = 返回的 sampleTime（应该是 cycle * 512）
- `host=261074594005` = 返回的 HostTime（应该是周期边界对应的时间）

### 4. 分析逻辑

- 如果 `cycle` 每次调用都在增长 → 说明周期计算正确
- 如果 `sample` 不是 512 的倍数 → 需要对齐到周期边界
- 如果 `host` 和 `now` 差值很大 → outHostTime 返回的不是周期起始时间

---

> *For advanced agent configuration, capability boundaries, and tool definitions, strictly refer
to `.opencode/agents/audio_architect.md`*