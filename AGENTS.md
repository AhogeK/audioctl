# 🤖 Audioctl Agent Protocols

## 🚨 核心禁令

- **语言**: 全程简体中文
- **Git**: 严禁 `git commit/push/pull`（除非用户明确指令）
- **安装**: **严禁** `./scripts/install.sh` 任何参数（包括 `--no-coreaudio-restart`）
- **物理操作**: 严禁切换设备/播放音频/系统设置

## 🧠 记忆协议 (Memory Bank)

**Boot时必读**: `memory-bank/progress.md` + `implementation-plan.md`
**Sleep时更新**: `progress.md` 里程碑、`decisions.md` ADR

## 🎯 职责分工

| 角色     | 可执行                                                  | 禁止                               |
|:-------|:-----------------------------------------------------|:---------------------------------|
| **AI** | `cmake --build`, `ctest`, `clang-format`, 代码审查, 日志分析 | `install.sh`, 物理测试, `git commit` |
| **人类** | `./scripts/install.sh install`, 设备切换, 播放测试, 结果反馈     | -                                |

## 🔄 测试协作流程

```
AI: 编译检查通过 → "请执行安装: ./scripts/install.sh install"
人类: [执行安装, 复制输出]
AI: "请验证: ./audioctl virtual-status, 预期: 已绑定到XX"
人类: [执行, 反馈结果]
AI: [分析 → 下一步或诊断]
```

**铁律**: AI绝不假设物理状态, 必须等待人类反馈命令输出

## 🛠️ 构建命令

```bash
# AI执行: 编译+测试+格式化
cmake --build cmake-build-debug && cd cmake-build-debug && ctest --output-on-failure
find src include tests -name "*.[chm]" | xargs clang-format -i

# 人类执行: 安装
./scripts/install.sh install  # 会自动重启coreaudiod
```

## 📋 其他

- **格式化**: LLVM风格 (`clang-format`)
- **实时约束**: `AudioDeviceIOProc`中禁`malloc`/锁/`printf`
- **Checkpoint**: 可提交时列出变更+测试点, 等人类确认
- **Beads**: `bd ready`看任务, `bd update/close/sync`管理

## 🔍 日志诊断

```bash
# coreaudiod错误
/usr/bin/log show --predicate 'process == "coreaudiod"' --last 30s 2>&1 | grep -iE "VADriver|anchor|sample" | tail -20

# 驱动日志
sudo log show --predicate 'message contains "GetZTS"' --last 30s 2>&1 | head -10
```

**关键指标**: `sample diff: 1024`=时戳跳跃, `cycle=1909`=周期数

---

> *For advanced agent configuration, capability boundaries, and tool definitions, strictly refer
to `.opencode/agents/audio_architect.md`*