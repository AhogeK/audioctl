---
description: "小步提交确认闸门：每个可提交步骤完成后，停下等待人类确认测试与提交"
---

# ⛳ Checkpoint Gate

当完成一个“可以提交的小步骤”后，AI 必须立刻停下进入 Checkpoint，并输出以下内容供人类确认。

## 变更说明（面向 Code Review）

- 说明本次改动影响的模块/文件范围
- 说明行为变化与预期收益
- 列出潜在风险点与回滚策略（如适用）

## 测试清单（面向验证）

- 必跑项（例如：格式化、单测、集成测试）
- 可选项（例如：手工验证路径、性能观测点）
- 若涉及 CoreAudio/实时回调，明确提醒避免高频重启与危险操作

人类可执行命令建议（示例，按需取用）：

    find src include tests -name "*.[chm]" | xargs clang-format -i
    ./scripts/install.sh install --no-coreaudio-restart
    cd cmake-build-debug && ctest --output-on-failure

## 人类确认点（AI 在此暂停等待）

- 是否执行上述测试？实际输出是否通过？
- 是否提交？准备拆成几次 commit？
- commit message 使用什么？
- 是否需要创建/更新 PR（由人类执行）