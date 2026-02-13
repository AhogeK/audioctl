---
description: "智能唤醒工作流：环境嗅探、记忆挂载与按需时效性同步"
---

# 🚀 系统唤醒与状态同步指令

请严格按照以下顺序执行初始化操作，确保你对本项目活在“当下”且具备完整的上下文：

1. **环境嗅探**：
    - 全局快速扫描当前代码库结构（重点关注 `src/` 和 `include/` 的变动）。
    - 识别项目使用的核心第三方库，并初步评估它们的“技术半衰期”。

2. **记忆挂载**：
    - 静默读取 `memory-bank/progress.md` 与 `memory-bank/implementation-plan.md`。
    - 同步当前的开发战区、未完成的任务节点以及过往的关键决策。

3. **按需时效校准**：
    - **自主判断**：基于步骤 1 识别出的技术栈，判断是否涉及近期有重大更迭的领域。
    - **主动对齐**：若有必要，主动获取当前时间或通过 `web_search` 确认相关技术的最新 SOTA（业界领先）标准。

4. **远端同步前置检查**：
    - AI 执行以下命令确保本地代码库是最新状态：

          git fetch
          git status

    - 若发现远端有更新（`git status` 显示 "behind"），执行拉取并处理可能的冲突：

          git pull --rebase

    - 若拉取过程产生冲突或代码变更，必须先重新运行质量门控后再继续开发：

          find src include tests -name "*.[chm]" | xargs clang-format -i
          ./scripts/install.sh install --no-coreaudio-restart
          cd cmake-build-debug && ctest --output-on-failure

    - 确认 `git status` 显示 "working tree clean" 且无冲突后，进入下一步。

5. **任务状态同步**：
    - 执行 `bd ready` 检查无阻塞的可执行任务。
    - 执行 `bd list --status in_progress` 确认当前进行中的工作。
    - 若存在 `in_progress` 任务，优先执行 `bd show <id>` 读取上下文。

6. **状态汇报**：
    - 结合扫描到的代码现状、`memory-bank/progress.md` 与 `bd ready` 的输出，使用 `- [ ]` 任务列表汇报当前所处的开发阶段。
    - **列出建议立即执行的前 3 个动作**（优先来自 `bd ready` 的高优先级任务）。

> **注意**：如果发现 `memory-bank` 目录或相关文件不存在，请根据当前扫描到的项目状态，主动帮我创建并初始化它们。