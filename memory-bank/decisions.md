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

## [日期] - [决策简述，例如：放弃互斥锁改用无锁环形队列]

* **场景上下文**: 在 `AudioDeviceIOProc` 中遇到高频抢占导致的爆音。
* **尝试过的废案**: `pthread_mutex_t` (原因：引发系统级死锁或优先级反转)。
* **最终决定**: 采用 C11 `<stdatomic.h>` 实现 Lock-free RingBuffer。
* **后续影响**: 读写两端必须严格遵守单一生产者/单一消费者模型。
