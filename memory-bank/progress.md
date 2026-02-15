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

### 🔴 阻塞问题

- **噪音问题**: 与特定应用（Discord）不兼容
    - **发现**: 使用 Discord 时出现噪音，不使用时正常
    - **可能原因**:
        - Discord 使用多个音频流/多个客户端
        - 多客户端写入 ring buffer 时的竞争条件

### 📋 下一步

1. [ ] **Phase 1**: 实现客户端管理结构 (ClientInfo + 客户端表)
2. [ ] **Phase 2**: 实现 per-client ring buffer
3. [ ] **Phase 3**: Discord 实机测试

---

## 🎯 AI 与人类职责分工

### AI 职责（我）
1. 编译检查
2. 单测运行  
3. 日志分析
4. 架构维护

### 人类职责（你）
1. 实际安装：`./scripts/install.sh install`
2. 实际测试：切换设备、播放音频
3. 问题反馈

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
