# audioctl

> Mac 端的音频控制工具，支持应用级音量控制。

## 功能特性

- **音频设备管理**: 列出、切换、设置系统音频设备
- **音量控制**: 控制输入/输出设备的音量
- **虚拟音频驱动**: 创建虚拟音频设备用于应用级音量控制
- **应用音量控制**: 单独控制每个应用的音量，同时保持音频输出到物理设备

## 安装

### 快速安装（推荐）

```bash
# 克隆仓库
git clone https://github.com/yourusername/audioctl.git
cd audioctl

# 一键安装
./scripts/install.sh install

# 或者 Release 模式
./scripts/install.sh install --release

# 添加到 PATH
echo 'export PATH="'$(pwd)'/cmake-build-debug/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

### 卸载

```bash
# 一键卸载（自动恢复物理设备、删除驱动和聚合设备）
./scripts/install.sh uninstall
```

### 开发构建

```bash
# 仅构建不重启 CoreAudio（适合开发调试）
./scripts/install.sh install --no-coreaudio-restart
```

## 使用说明

### 快速开始

```bash
# 1. 检查虚拟设备状态
audioctl virtual-status

# 2. 检查 Aggregate Device 状态（音频路由）
audioctl agg-status

# 3. 切换到虚拟设备（自动创建 Aggregate Device 并路由音频）
# 首次运行会自动启动后台路由进程
audioctl use-virtual

# 4. 恢复物理设备（停止路由进程，恢复系统默认输出）
audioctl use-physical
```

### 基础命令

```bash
# 显示帮助
audioctl help

# 列出所有音频设备
audioctl list

# 设置当前输出设备音量 (0-100)
audioctl set -o 50

# 设置当前输入设备音量 (0-100)
audioctl set -i 75

# 切换到指定设备
audioctl set 117
```

### 虚拟设备与 Aggregate Device

**Aggregate Device** 是 macOS 的音频设备聚合技术，它将虚拟设备和物理设备组合在一起：

```
应用音频 → 虚拟设备(应用音量控制) → 物理设备(实际输出)
```

相关命令：

```bash
# 查看虚拟设备状态
audioctl virtual-status

# 查看 Aggregate Device 状态（包含绑定的物理设备）
audioctl agg-status

# 切换到虚拟设备（自动创建 Aggregate Device 并绑定物理设备）
audioctl use-virtual

# 恢复到物理设备（移除 Aggregate Device）
audioctl use-physical
```

### 应用音量控制

**前置条件**: 必须先运行 `audioctl use-virtual` 创建 Aggregate Device

```bash
# 列出所有音频应用及其音量
audioctl app-volumes

# 设置指定应用的音量 (0-100)
audioctl app-volume Safari 50    # 按应用名称
audioctl app-volume 1234 30      # 按 PID

# 静音/取消静音
audioctl app-mute Safari
audioctl app-unmute Safari
```

如果 Aggregate Device 未激活，会提示：

```
⚠️  Aggregate Device 未激活，无法使用应用音量控制
请运行: audioctl use-virtual 激活
```

## 工作原理

### 系统架构

传统音频路径：

```
应用 → 物理设备（无法单独控制应用音量）
```

Aggregate Device 音频路径：

```
应用 → 虚拟设备(应用音量控制) → 物理设备(实际输出)
         ↓
    共享内存音量表
         ↑
   audioctl app-volume
```

### 组件说明

| 组件                         | 源码文件                         | 功能                   |
|----------------------------|------------------------------|----------------------|
| **Virtual Audio Driver**   | `virtual_audio_driver.c`     | HAL 插件，接收应用音频并应用音量控制 |
| **Aggregate Device**       | `aggregate_device_manager.c` | 系统级设备聚合，路由音频到物理设备    |
| **App Volume Control**     | `app_volume_control.c`       | 管理应用音量设置，提供 CLI      |
| **Virtual Device Manager** | `virtual_device_manager.c`   | 检测虚拟设备状态、切换设备        |

### 工作流程

1. **安装驱动**: 虚拟驱动安装到 `/Library/Audio/Plug-Ins/HAL/`
2. **创建 Aggregate**: `use-virtual` 创建 Aggregate Device，包含：
    - 虚拟设备（主设备，接收所有音频）
    - 物理设备（时钟主设备，实际输出）
3. **音频流**: 系统 → 虚拟设备 → 应用音量处理 → 物理设备
4. **音量控制**: 驱动根据 PID 从共享内存读取音量并应用

### 为什么需要 Aggregate Device

如果不使用 Aggregate Device：

- 切换到虚拟设备后，音频只到虚拟设备，没有到物理设备
- **结果是：没有声音**

使用 Aggregate Device 后：

- 虚拟设备接收所有应用音频
- 音量控制后，音频自动路由到绑定的物理设备
- **结果是：有声音，且能控制每个应用的音量**

## 故障排除

### 切换后没有声音

```bash
# 检查 Aggregate Device 是否正确创建
audioctl agg-status

# 如果没有显示物理设备，重新创建
audioctl use-physical
audioctl use-virtual
```

### 虚拟设备未显示

```bash
# 重启 CoreAudio 服务
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
```

### 应用音量控制不生效

```bash
# 1. 检查 Aggregate Device 状态
audioctl agg-status

# 2. 确保已激活
audioctl use-virtual

# 3. 重启音乐应用
```

### 无法安装驱动

```bash
sudo ninja install
sudo chown -R root:wheel /Library/Audio/Plug-Ins/HAL/VirtualAudioDriver.driver
sudo chmod -R 755 /Library/Audio/Plug-Ins/HAL/VirtualAudioDriver.driver
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
```

## 技术限制

- 虚拟设备处理会引入少量延迟（约 5-10ms）
- 部分应用可能需要重启才能识别设备切换
- Aggregate Device 在重启后可能需要重新创建
- 共享内存方式仅支持单机使用

## 开发状态

- ✅ 基础设备控制
- ✅ 虚拟音频驱动
- 🚧 应用音量控制
- 🚧 Aggregate Device 音频路由
- 🚧 自动绑定物理设备
- 🚧 图形界面 (计划中)

## 许可证

MIT License
