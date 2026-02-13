# 🗺️ Future Roadmap & Next Steps

> **Status**: Draft
> **Last Updated**: 2026-02-14

既然基础架构（Driver + Router + IPC）已经稳定，以下是针对 `audioctl` 的后续功能增强建议。

## 🚀 P0: 核心体验增强 (High Priority)

### 1. 📱 Per-App Volume Control UI

目前我们只有 CLI (`audioctl app-volume`)，用户体验不够直观。

* **目标**: 开发一个简单的状态栏应用 (Menu Bar App) 或类似 Windows "Volume Mixer" 的界面。
* **技术栈**: SwiftUI / AppKit (调用现有的 `audioctl` CLI 或通过 IPC 直接通信)。
* **功能**:
    * 列出当前播放音频的应用。
    * 提供滑块调节每个应用的音量。
    * 提供静音按钮。

### 2. 🎛️ 智能路由策略 (Smart Routing)

目前的路由策略是将所有虚拟设备的音频转发到 *系统默认物理设备*。

* **需求**: 允许用户指定特定的物理输出设备（例如：始终转发到 USB 耳机，即使系统默认是扬声器）。
* **实现**:
    * 在 IPC 协议中增加 `set-target-device` 指令。
    * Router 支持动态切换输出设备，无需重启进程。

### 3. 💾 持久化配置 (Persistence)

* **需求**: 重启后记住应用的音量设置。
* **实现**:
    * IPC Service 需要将 `AppVolumeTable` 定期保存到磁盘 (JSON/Plist)。
    * 启动时加载配置。

## 🛠️ P1: 工程化与稳定性 (Engineering)

### 1. 🔄 自动守护进程 (LaunchAgent)

目前需要手动运行 `audioctl use-virtual` 来启动服务。

* **目标**: 提供标准的 macOS `LaunchAgent` plist 文件。
* **效果**: 用户登录后自动启动 IPC Service 和 Router（如果配置为启用）。

### 2. 🧪 自动化集成测试 (Integration Tests)

* **目标**: 编写脚本自动验证 "Play -> Record" 链路。
* **实现**: 使用 `sox` 或类似工具向虚拟设备播放正弦波，同时从物理设备（或 Loopback）录制，验证频率和幅度。

### 3. 📈 性能监控面板

* **目标**: 实时显示 Router 的 `Overruns/Underruns` 和 CPU 占用。
* **实现**: 在 `audioctl virtual-status` 中增加实时刷新模式。

## 🎨 P2: 高级功能 (Nice to Have)

### 1. 🎚️ 简单的 EQ / 效果器

既然我们已经有了 Ring Buffer 中转，可以在 `rb_read` 之后加入简单的 DSP 处理。

* **功能**: 低音增强、简单的 3 段 EQ。

### 2. 📡 网络音频流 (Network Streaming)

* **想法**: 将音频流通过 TCP/UDP 发送到另一台电脑（类似 AirPlay）。

---

## 📝 建议的立即行动 (Immediate Next Action)

**建议先从 [P0-1] Per-App Volume Control UI (SwiftUI 原型) 开始。**
这将极大地提升项目的“可演示性”和实用价值。
