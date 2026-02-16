# 项目进度 - 虚拟音频驱动

## 当前状态 (2026-02-16)

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
- **CLI 优化** (2026-02-16):
    - [x] 修复 `-ao` 选项支持输入/输出设备
    - [x] `use-virtual` 显示绑定设备名称而非 UID
    - [x] `use-physical` 显示恢复设备名称
    - [x] `list` 命令虚拟设备显示友好绑定状态
    - [x] 优化启动顺序：IPC 服务先于 Router 启动
    - [x] 简化增益补偿方案（移除渐进式增益）

### 📋 阻塞问题 (待解决)

- **use-virtual 音量突增**: 切换到虚拟设备时声音突然变大然后恢复
    - 尝试方案1: 驱动默认音量 100→0（无效，setDeviceVolume 对虚拟设备不生效）
    - 尝试方案2: Router output_callback 静音处理（无效）
    - 尝试方案3: 优化启动顺序 + setDeviceVolume（导致完全无声）
  - 尝试方案4: 驱动默认音量 100→50（无效且不符合直觉）
      - **根因分析**: CoreAudio 虚拟设备属性设置与实际 IO 路径存在时序问题
      - **建议**: 下次会话从驱动层 `volumeControlValue` 同步时机入手，或添加淡入效果

### 📋 下一步

1. **调试音量突增**: 定位 use-virtual 瞬间音量突增的根本原因
2. **Phase 2**: Per-Client Ring Buffer（解决 Discord 多客户端噪音）

---

## 📝 历史调试记录

### 2026-02-14: 最终修复 (No Sound & Glitching)

- **问题**: 驱动无声，控制台大量 `TimeStampOutOfLine` 和 `AudioConverter` 错误。
- **根因**:
    1. **无声**: `GetPropertyData` 处理 `kAudioDevicePropertyStreamConfiguration` 时未检查 `mScope`，导致 CoreAudio
       获取了错误的声道配置 (Input + Output = 4ch)，与实际 buffer (2ch) 不匹配。
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
