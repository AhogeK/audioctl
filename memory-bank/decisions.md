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

## 2026-02-15 - 噪音问题根因分析

* **场景上下文**: 用户测试发现，使用 Discord 时出现明显噪音/卡顿；不使用 Discord 时音频正常。
* **问题本质**: 驱动本身工作正常，问题出在特定应用（Discord）的音频行为与驱动的兼容性。
* **可能原因分析**:
    1. **多流/多客户端**: Discord 可能同时创建多个音频流（输入+输出或多个输出流），每个流有不同的 `clientID`
    2. **采样率/格式不匹配**: Discord 可能使用了与默认不同的采样率（如 48kHz vs 44.1kHz）或格式
    3. **调度抖动**: Discord 的音频回调时机可能与其他应用不同，导致时序问题
    4. **客户端识别问题**: 当前驱动使用单一的全局 ring buffer，当多个客户端同时写入时可能产生竞争
* **验证方法**:
    - 使用 `sudo log stream` 监控 Discord 音频创建过程
    - 检查 Discord 是否使用了不同的采样率
    - 确认是否有多个 clientID 同时活跃
* **待解决**: 需要针对多客户端场景进行优化

---

## 2026-02-15 - Router 架构重构：移除 internal-route 子进程

* **场景上下文**: 之前 Router 通过 `internal-route` 子进程启动，增加了进程管理和通信复杂度。现在改为直接调用，简化架构。
* **变更内容**:
    - 移除 `internal-route` 内部命令处理
    - 移除 `spawn_router()` 和 `kill_router()` 函数
    - 新增 `start_router_service()` 和 `stop_router_service()` 直接管理 Router
    - `use-virtual` 直接启动 Router，显示状态信息
    - `use-physical` 直接停止 Router，显示停止信息
    - 在头文件中暴露 `get_default_output_device()` 和 `get_default_input_device()`
* **用户体验提升**:
    - 启动时显示 Router 状态和性能监控信息
    - 统一的命令入口：`use-virtual` 启动，`use-physical` 停止
    - 更易理解的服务状态
* **代码简化**: 移除了子进程管理、PID 文件、锁文件等复杂逻辑

---

## 2026-02-15 - 移除废弃的 Audio Router 架构

* **场景上下文**: Audio Router 是基于早期串联架构（Virtual Device -> Ring Buffer -> Physical Device）的外部进程方案，但现在驱动已采用
  loopback 模式，Router 已无用。
* **已废弃代码**:
    - `src/audio_router.c` - 外部 Router 实现
    - `include/audio_router.h` - Router 头文件
    - `spawn_router()` / `kill_router()` / `load_target_device_uid()` 函数
    - `internal-route` 命令处理
    - `virtual_device_activate_with_router()` 函数
* **架构变更**:
    - 驱动使用内置 loopback buffer (gLoopbackBuffer)
    - WriteMix 将音频写入 loopback buffer
    - ReadInput 从 loopback buffer 读取
    - 无需外部进程转发音频
* **命令变更**:
    - `use-virtual` 不再启动 Router，只激活虚拟设备和 IPC 服务
    - `use-physical` 不再停止 Router
    - 移除 `internal-route` 内部命令
* **结果**: 代码简化，架构清晰，避免维护废弃代码

---

## 2026-02-15 - Loopback Buffer 关键修复

* **场景上下文**: 驱动使用 loopback buffer 实现音频环回，但存在严重的竞争条件和缓冲区管理问题，可能导致无声、噪音或数据损坏。
* **发现的问题**:
    1. **缓冲区过小**: 16384 采样 (8192 帧) 仅约 170ms，多客户端时容易溢出
    2. **读写竞争**: 没有正确的 memory order，CPU 可能重排序操作，导致读取半写入数据
    3. **指针未复位**: StartIO 中未清零缓冲区，重启音频可能读到旧数据
    4. **溢出处理缺失**: 写入时不检查是否会追上读指针，导致数据覆盖
    5. **欠载处理粗糙**: 数据不足时直接输出静音，没有尝试读取部分数据
* **修复方案**:
    1. **增大 buffer**: 65536 采样 (32768 帧) = ~682ms @ 48kHz stereo
    2. **添加 memory barrier**:
        - WriteMix: `memory_order_acquire` 读取, `memory_order_release` 写入
        - ReadInput: `memory_order_acquire` 读取两个指针, `memory_order_release` 更新 readPos
    3. **StartIO 清零**: 第一个客户端启动时 memset 缓冲区为 0
    4. **溢出保护**: 写入前检查是否会追上 readPos（留出 512 采样安全边际），会则丢弃新数据
    5. **部分数据读取**: 数据不足时读取可用部分，剩余填充静音，减少卡顿感
* **代码变更**: `src/driver/virtual_audio_driver.c`
    - 宏定义: `LOOPBACK_BUFFER_SAMPLES 65536`
    - 原子变量类型: `volatile atomic_uint` → `_Atomic UInt32`
    - 缓冲区大小: `16384` → `65536`
* **验证**: 编译通过，单元测试通过

---

## 2026-02-15 - Per-Client Ring Buffer 实施计划

### 目标

解决多客户端（如 Discord）同时使用时的噪音问题，实现类似 BackgroundMusic 的架构。

### 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                    VirtualAudioDriver                       │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐        │
│  │ Client A    │  │ Client B    │  │ Client C    │  ...   │
│  │ (Safari)    │  │ (Discord)   │  │ (Spotify)   │        │
│  │ clientID=1  │  │ clientID=2  │  │ clientID=3  │        │
│  │ ring buffer │  │ ring buffer │  │ ring buffer │        │
│  │ volume=1.0   │  │ volume=0.5  │  │ volume=0.8   │        │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘        │
│         │                 │                 │                 │
│         └────────────────┼─────────────────┘                 │
│                          ▼                                   │
│              ┌─────────────────────┐                           │
│              │  Mix & Output      │ ← Input Stream           │
│              │  (ReadInput)       │                           │
│              └─────────────────────┘                           │
└─────────────────────────────────────────────────────────────┘
```

### 实施步骤

#### Phase 1: 客户端管理（基础）

- [ ] 1.1 定义客户端结构体 `ClientInfo`
    - `clientID`: UInt32
    - `pid`: pid_t
    - `volume`: Float32
    - `ringBuffer`: Float32[]
    - `writePos`: atomic_uint
    - `readPos`: atomic_uint
    - `active`: bool

- [ ] 1.2 实现客户端表管理
    - `kMaxClients = 32`
    - `find_client(clientID)`: 查找客户端
    - `create_client(clientID, pid)`: 创建客户端
    - `destroy_client(clientID)`: 销毁客户端

- [ ] 1.3 集成到 AddDeviceClient/RemoveDeviceClient
    - 新客户端连接时创建 buffer
    - 客户端断开时销毁 buffer

#### Phase 2: Per-Client Ring Buffer

- [ ] 2.1 修改 WriteMix
    - 根据 `inClientID` 查找对应客户端 buffer
    - 写入该客户端的 ring buffer

- [ ] 2.2 修改 ReadInput
    - 遍历所有活跃客户端
    - 混合所有客户端的音频数据（按音量）
    - 输出混合后的数据

- [ ] 2.3 音量控制集成
    - 读取 AppVolumeTable
    - 应用客户端音量到混合

#### Phase 3: 测试与调优

- [ ] 3.1 编译验证
- [ ] 3.2 单元测试
- [ ] 3.3 Discord 实机测试
- [ ] 3.4 性能调优（如需要）

### 技术要点

1. **Lock-free**: 使用 C11 atomics，避免 mutex
2. **内存预分配**: 客户端 buffer 在驱动加载时分配，避免实时分配
3. **混合策略**: 简单的加法混合，需考虑溢出保护
4. **时钟统一**: 保持 Freewheel 时钟，Input 和 Output 共享

### 预计改动文件

- `src/driver/virtual_audio_driver.c` - 核心逻辑
- `src/driver/virtual_audio_driver.h` - 如需要

### 风险与缓解

- **风险**: 改动较大，可能引入新问题
- **缓解**: 逐阶段实现，每阶段验证

---

## 2026-02-15 - 虚拟设备绑定信息持久化机制

* **场景上下文**: 用户反馈虚拟设备在 `use-virtual` 前后状态显示不清晰，需要知道当前绑定到哪个物理设备，以及音量应该反映物理设备状态。
* **尝试过的废案**:
    - 直接查询 CoreAudio 属性（无法直接获取绑定关系）
    - 在内存中保存绑定状态（进程重启后丢失）
* **最终决定**:
    - 在 `~/Library/Application Support/audioctl/binding_info.txt` 持久化绑定的物理设备 UID
    - `use-virtual` 时保存，`use-physical` 时清除
    - 虚拟设备音量通过查找绑定的物理设备并获取其实际音量
    - 显示格式："绑定状态: 已绑定到 [设备名]" 或 "绑定状态: 未绑定"
* **后续影响**: 绑定信息跨进程持久化，重启 audioctl 后仍能保持状态一致。

---

## 2026-02-15 - `internal-route` 命令行为重构

* **场景上下文**: 原 `internal-route` 设计有缺陷，用户无法方便地查看已运行的后台 Router 日志。
* **尝试过的废案**:
    - 直接运行 `internal-route` 启动前台 Router（与后台 Router 冲突）
    - 提示用户使用 `log stream` 命令（不够友好）
* **最终决定**:
    - 无参数运行 `internal-route`：自动检测后台 Router，若存在则执行 `log stream --process audioctl` 显示实时日志
    - 带 `--router-target=` 参数：后台启动模式（由 `use-virtual` 调用）
    - 检测到虚拟设备为默认设备时，提示正确的日志查看方式
* **后续影响**: 用户可以在 `use-virtual` 后直接运行 `audioctl internal-route` 查看实时日志，体验更流畅。

---

## 2026-02-16 - use-virtual 音量突增问题（未解决）

* **场景上下文**: 切换到虚拟设备时，声音会突然变大然后恢复。
* **尝试过的废案**:
    1. 驱动默认音量 100→0 + setDeviceVolume 同步（setDeviceVolume 对虚拟设备不生效，返回 kAudioHardwareUnsupportedOperationError）
    2. Router output_callback 添加静音处理（无效，音量突增发生在 Router 启动瞬间）
    3. 优化启动顺序：IPC → 虚拟设备(静音) → Router → 同步音量（导致完全无声）
    4. 驱动默认音量 100→50（未验证）
* **当前状态**: 问题未解决，需要进一步分析
* **根因分析**:
    - 虚拟设备的音量属性设置 (`SetPropertyData` for `kAudioDevicePropertyVolumeScalar`) 与驱动内部 `volumeControlValue` 是两套机制
    - CoreAudio 可能在驱动属性设置完成前就已经开始播放音频
    - 需要从驱动层 `volumeControlValue` 同步时机入手
* **建议方向**: 
    - 添加淡入效果 (fade-in) 掩盖突增
    - 或在驱动 StartIO 前确保音量已正确设置

* **场景上下文**: 在 `AudioDeviceIOProc` 中遇到高频抢占导致的爆音。
* **尝试过的废案**: `pthread_mutex_t` (原因：引发系统级死锁或优先级反转)。
* **最终决定**: 采用 C11 `<stdatomic.h>` 实现 Lock-free RingBuffer。
* **后续影响**: 读写两端必须严格遵守单一生产者/单一消费者模型。

---

## 2026-02-15 - 明确 AI 安装权限边界

* **场景上下文**: AI 在自动化流程中可能误执行安装脚本，导致系统驱动被意外替换或配置变更。
* **问题风险**:
    - `./scripts/install.sh install` 会将驱动文件复制到系统目录 `/Library/Audio/Plug-Ins/HAL/`
    - 即使是 `--no-coreaudio-restart` 也会修改系统文件
    - CoreAudio 驱动安装属于高风险操作，必须由人类确认后执行
* **最终决定**:
    - **AI 严禁执行**: 任何形式的 `install.sh` 脚本调用
    - **AI 仅可执行**: `cmake --build` 或 `make` 进行纯编译检查
    - **AI 职责**: 提供安装命令供开发者复制执行，提供调试指导而非亲自调试
* **后续影响**:
    - 更新 `AGENTS.md` 和 `.opencode/agents/audio_architect.md` 明确权限边界
    - AI 专注于代码层面工作：编译、单测、代码审查、日志分析指导
