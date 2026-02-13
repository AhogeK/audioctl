# ⚖️ 架构决策记录 (ADR)

## 2026-02-14 - 基于 BackgroundMusic 架构完全重写虚拟驱动

* **场景上下文**: 当前虚拟驱动存在 coreaudiod CPU 100%+ 的稳定性问题，无法正常工作。
* **尝试过的废案**: 
  - 调整 ZeroTimeStampPeriod
  - 使用原子操作替代 mutex
  - 外部 Router 架构（IOProc 无法获取数据）
* **最终决定**: 
  - 参考 BackgroundMusic (GPLv2, 18.6k stars) 的架构
  - 在驱动内部直接实现 per-app 音量控制
  - 使用 CAMutex 风格的轻量锁
  - 移除外部 Router 架构
* **后续影响**: 需要完全重写 `src/driver/virtual_audio_driver.c`

---

## 2026-02-14 - 连续线性时钟替代离散阶梯时钟

* **场景上下文**: HAL 报错 `TimeStampOutOfLine`，sample diff: 1024。离散阶梯式时间戳不符合 HAL 预期。
* **尝试过的废案**:
  - 调整 ZeroTimeStampPeriod
  - 离散周期计数模型 (cycleCount * 512)
* **最终决定**:
  - 使用 mach_timebase_info 直接计算 ticksToSeconds
  - GetZeroTimeStamp 返回连续线性时间: `currentSample = deltaTicks * ticksToSeconds * sampleRate`
  - StartIO/StopIO 添加锚点日志
* **后续影响**: 需要观察是否还有 TimeStampOutOfLine 错误

---

## 2026-02-14 - GetZeroTimeStamp 修复尝试

* **场景上下文**: sample diff: 1024 错误导致没声音
* **尝试过的废案**:
  1. 简单周期计算 → 仍有 sample diff 错误
  2. 限幅追赶时钟 → 导致崩溃
  3. pthread_mutex_t 保护 → 导致崩溃
* **当前状态**: 
  - 将 ZeroTimeStampPeriod 从 512 改为 16384（大周期）
  - 待测试是否能解决问题
* **待解决**: 需要找到不崩溃的限幅方案

---

## 2026-02-14 - 严格 Scope 校验策略 (Strict Scope Handling)

* **场景上下文**: 驱动在 `audioctl` 测试正常但在系统设置中无声。
* **根本原因**: CoreAudio 在查询 `kAudioDevicePropertyStreamConfiguration` 时，会传入 `kAudioObjectPropertyScopeGlobal` (
  wildcard) 或错误的 Scope。旧逻辑直接返回 input+output 的所有流配置，导致 CoreAudio 误认为设备是 4声道 (2 in + 2 out)，而实际
  buffer 只有 2声道，从而丢弃数据。
* **最终决定**:
    - 在 `GetPropertyData` 中严格校验 `inQuery->mScope`。
    - 只有当请求 Scope 精确匹配 Input 或 Output 时，才返回对应的流配置。
    - 对于 Global Scope，不返回任何流配置。
* **结果**: 彻底解决了无声问题。

---

## 2026-02-14 - 自适应飞轮时钟 (Freewheel Clock Strategy)

* **场景上下文**: 即使使用了连续线性时钟，由于系统调度抖动，计算出的 HostTime 仍可能与 HAL 的预期存在微小偏差 (Sample
  Drift)，导致 `TimeStampOutOfLine`。
* **最终决定**:
    - 放弃每次都用 `mach_absolute_time` 计算当前采样点。
    - 采用 "Freewheel" (飞轮) 模式：仅在 `StartIO` 时记录一次基准时间 (Anchor Time)。
    - 后续所有时间戳均基于：`BaseTime + (CycleCount * BufferSize)` 推算。
    - 忽略系统的实时抖动，强行让时钟对齐到理想的 IO 周期。
* **结果**: 彻底消除了 Glitching 和 Drift，驱动运行极其平稳。

---

## [日期] - [决策简述，例如：放弃互斥锁改用无锁环形队列]

* **场景上下文**: 在 `AudioDeviceIOProc` 中遇到高频抢占导致的爆音。
* **尝试过的废案**: `pthread_mutex_t` (原因：引发系统级死锁或优先级反转)。
* **最终决定**: 采用 C11 `<stdatomic.h>` 实现 Lock-free RingBuffer。
* **后续影响**: 读写两端必须严格遵守单一生产者/单一消费者模型。
