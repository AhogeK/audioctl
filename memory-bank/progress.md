# 项目进度 - 虚拟音频驱动

## 当前状态 (2026-02-15)

### 🟢 已完成 (Stable)
- **核心音频功能修复**:
    - [x] **解决无声问题**: 修复 mScope 校验逻辑
    - [x] **解决爆音/卡顿**: 实现 Freewheel Clock
    - [x] **格式稳定性**: 锁定 Interleaved Float32
- **Loopback Buffer 关键修复** (2026-02-15):
    - [x] 增大 buffer: 16384 → 65536 采样 (32768 帧 @ 48kHz)
    - [x] 修复读写竞争: acquire/release memory order
    - [x] StartIO 清零缓冲区防止旧数据
    - [x] 溢出保护: 写入前检查是否会追上 readPos
    - [x] 欠载优化: 部分数据读取 + 静音填充
- **基础架构**:
    - [x] 驱动安装/卸载脚本
    - [x] CLI 控制工具
    - [x] 日志系统

### 🟡 本次会话完成 (2026-02-15)

- **虚拟设备状态显示优化**:
    - [x] 修复 `virtual-status` 检测 Router 状态问题（改用进程检测）
    - [x] 改进 `internal-route` 命令：无参数时自动检测并显示后台 Router 日志
    - [x] 实现绑定信息持久化（`save_bound_physical_device` 等函数）
    - [x] 虚拟设备显示绑定状态和物理设备音量（未绑定显示"未绑定"，已绑定显示绑定设备名）
    - [x] 修复编译警告（类型转换、冗余循环等）

### 📋 下一步

1. **测试验证**：验证绑定信息功能（use-virtual 前后状态变化）
    - 参见 `memory-bank/test-protocol.md` 了解协作流程
2. **代码清理**：移除临时调试代码

### 🔴 阻塞问题

- 无

---

## 📝 历史调试记录

### 2026-02-14: 最终修复 (No Sound & Glitching)
- **问题**: 驱动无声，控制台大量 `TimeStampOutOfLine` 和 `AudioConverter` 错误。
- **根因**:
  1. **无声**: `GetPropertyData` 处理 `kAudioDevicePropertyStreamConfiguration` 时未检查 `mScope`，导致 CoreAudio 获取了错误的声道配置 (Input + Output = 4ch)，与实际 buffer (2ch) 不匹配。
  2. **爆音**: `GetZeroTimeStamp` 使用系统时间计算采样点，存在微小漂移，导致 CoreAudio 认为时钟不同步。
- **修复**:
  1. 严格校验 `mScope`，只返回请求 Scope 的流配置。
  2. 重写时钟逻辑为 "Freewheel" 模式，在 `StartIO` 时锚定时间，之后完全依赖 IO 周期累加样本数。
  3. 恢复 Interleaved 格式。

### 2026-02-14: 格式回滚与时钟量化
- **变更**: 回滚格式为 `Interleaved`，尝试量化时钟 (未完全解决问题)。

### 2026-02-14: GetZeroTimeStamp 问题
- **问题**: `sample diff: 1024` 错误导致没声音。
- **尝试**: 多种时钟计算方式均失败，最终通过 "Freewheel" 方案解决。
