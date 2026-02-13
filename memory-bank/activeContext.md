# 🧠 活跃上下文 (Active Context)

## 当前会话概览 (2026-02-14)

### 🎯 核心目标
解决虚拟音频驱动的 "无声 (No Sound)" 和 "爆音 (Glitching)" 两个致命问题，使驱动达到可用状态。

### 🔍 关键发现与修复

#### 1. 无声问题 (No Sound) - 🔴 Critical
- **现象**: 驱动加载成功，但播放音频无声，控制台报 `AudioConverter` 错误。
- **根因**: **Scope 混淆**。CoreAudio 在查询 `kAudioDevicePropertyStreamConfiguration` 时，会分别查询 Input 和 Output Scope。我们的代码未检查 `mScope`，导致对 Input Scope 的查询也返回了 Output 的流配置。CoreAudio 叠加后认为设备是 4 声道，实际缓冲区只有 2 声道，导致格式不匹配，转换器失败。
- **修复**: 在 `GetPropertyData` 中增加严格的 `mScope == kAudioObjectPropertyScopeOutput` 检查。

#### 2. 爆音与时钟漂移 (Glitching) - 🔴 Critical
- **现象**: 声音断续，控制台大量 `TimeStampOutOfLine`，提示 Sample Time 偏差。
- **根因**: **时钟抖动**。原实现依赖 `mach_absolute_time()` 实时计算采样时间，受系统调度影响，计算出的 Sample Time 与 Host Time 之间存在微小非线性漂移。
- **修复**: 实现 **"Freewheel Clock" (飞轮时钟)**。
  - `StartIO` 时锚定基准时间。
  - 之后完全根据 IO 回调次数累加 Sample Count (每次 +ZeroTimeStampPeriod)。
  - Host Time 反推：`AnchorHostTime + (SampleCount / Rate)`。
  - 彻底消除了抖动。

#### 3. 格式问题
- **决策**: 放弃 Non-Interleaved 尝试，回归最通用的 **Interleaved Float32**，确保兼容性。

### ✅ 当前状态
- **Status**: Stable / Ready
- **验证**:
  - `./scripts/install.sh install` 成功。
  - Safari/Music 播放正常，音质清晰。
  - `coreaudiod` 日志无报错。

### 🔜 下一步
- 清理代码中的调试日志。
- 实现音量调节功能 (目前音量调节无效)。
