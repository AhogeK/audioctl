#!/usr/bin/env bash
#
# AudioCtl Driver Install Script (Idempotent, Single CoreAudio Restart)
#
# Usage:
#   ./scripts/install.sh install [--release] [--no-coreaudio-restart]
#   ./scripts/install.sh uninstall [--no-coreaudio-restart]
#   ./scripts/install.sh status

set -euo pipefail
IFS=$'\n\t'

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_TYPE="Debug"
PARALLEL_JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

DRIVER_NAME="VirtualAudioDriver"
DRIVER_DST="/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}.driver"

RESTART_COREAUDIO="true"
ADHOC_SIGN="true"

RED='\e[0;31m'
GREEN='\e[0;32m'
YELLOW='\e[1;33m'
BLUE='\e[0;34m'
CYAN='\e[0;36m'
NC='\e[0m'

log_info() { printf "${BLUE}[信息]${NC} %s\n" "$*"; }
log_success() { printf "${GREEN}[成功]${NC} %s\n" "$*"; }
log_warn() { printf "${YELLOW}[警告]${NC} %s\n" "$*"; }
log_error() { printf "${RED}[错误]${NC} %s\n" "$*"; }
log_step() { printf "\n${CYAN}========== %s ==========${NC}\n" "$*"; }

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    log_error "缺少命令: $1"
    exit 1
  }
}

require_not_root() {
  if [[ "${EUID}" -eq 0 ]]; then
    log_error "请不要使用 sudo 运行此脚本"
    log_info "脚本会在需要时自动请求 sudo 权限"
    exit 1
  fi
}

detect_build_dir() {
  if [[ "${BUILD_TYPE}" == "Release" || "${BUILD_TYPE}" == "release" ]]; then
    BUILD_TYPE="Release"
    BUILD_DIR="${PROJECT_DIR}/cmake-build-release"
  else
    BUILD_TYPE="Debug"
    BUILD_DIR="${PROJECT_DIR}/cmake-build-debug"
  fi
}

# Calculate directory hash for idempotency check
hash_dir() {
  local dir="$1"
  if [[ ! -d "$dir" ]]; then
    echo "MISSING"
    return 0
  fi
  (cd "$dir" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256) |
    shasum -a 256 | awk '{print $1}'
}

# 获取 audioctl PID 文件路径（与代码中 constants.c 保持一致）
get_audioctl_pid_file() {
  local home="${HOME}"
  echo "${home}/Library/Application Support/audioctl/audioctl.pid"
}

# 获取 audioctl 锁文件路径
get_audioctl_lock_file() {
  local home="${HOME}"
  echo "${home}/Library/Application Support/audioctl/audioctl.lock"
}

kill_audioctl_processes() {
  log_info "清理 audioctl 相关进程..."

  local pid_file
  pid_file="$(get_audioctl_pid_file)"
  local lock_file
  lock_file="$(get_audioctl_lock_file)"

  # [修复] 杀死路由进程 (使用新的 PID 文件路径)
  if [[ -f "${pid_file}" ]]; then
    local router_pid
    router_pid="$(cat "${pid_file}" 2>/dev/null)" || true
    if [[ -n "${router_pid}" ]] && kill -0 "${router_pid}" 2>/dev/null; then
      log_info "停止路由进程 (PID: ${router_pid})..."
      kill -TERM "${router_pid}" 2>/dev/null || true
      sleep 1
      # 如果还在运行，强制杀死
      if kill -0 "${router_pid}" 2>/dev/null; then
        kill -KILL "${router_pid}" 2>/dev/null || true
      fi
    fi
    rm -f "${pid_file}"
  fi

  # [兼容] 清理旧的 PID 文件路径（如果存在）
  if [[ -f "/tmp/audioctl_router.pid" ]]; then
    local old_pid
    old_pid="$(cat /tmp/audioctl_router.pid 2>/dev/null)" || true
    if [[ -n "${old_pid}" ]] && kill -0 "${old_pid}" 2>/dev/null; then
      log_warn "发现旧路径的残留进程，正在清理..."
      kill -TERM "${old_pid}" 2>/dev/null || true
      sleep 1
      kill -KILL "${old_pid}" 2>/dev/null || true
    fi
    rm -f /tmp/audioctl_router.pid
  fi

  # [增强] 杀死任何名为 audioctl 的进程 (包括前台进程)
  # 循环检查直到进程完全消失，防止死锁
  local max_retries=10
  local retry=0

  while true; do
    local audioctl_pids
    audioctl_pids="$(pgrep -x audioctl 2>/dev/null)" || true

    if [[ -z "${audioctl_pids}" ]]; then
      break
    fi

    if ((retry == 0)); then
      log_warn "发现残留 audioctl 进程，正在清理..."
      echo "${audioctl_pids}" | xargs -I {} kill -TERM {} 2>/dev/null || true
    elif ((retry >= 3)); then
      log_warn "audioctl 进程响应缓慢，强制杀死..."
      echo "${audioctl_pids}" | xargs -I {} kill -KILL {} 2>/dev/null || true
    fi

    if ((retry >= max_retries)); then
      log_error "无法杀死 audioctl 进程，这可能会导致安装失败"
      break
    fi

    sleep 0.5
    retry=$((retry + 1))
  done

  # 清理新的锁文件
  rm -f "${lock_file}"
  # [兼容] 清理旧的锁文件
  rm -f "/tmp/audioctl_router.lock"

  # 清理共享内存（如果存在）
  local shm_id
  shm_id="$(ipcs -m 2>/dev/null | grep "0x564f4c00" | awk '{print $2}')" || true
  if [[ -n "${shm_id}" ]]; then
    log_info "清理共享内存..."
    ipcrm -m "${shm_id}" 2>/dev/null || true
  fi

  log_success "进程清理完成"
}

# 关键音频应用列表 - 这些应用如果正在运行，需要用户手动退出
# 原因：这些应用通常涉及未保存的工程/项目，直接重启 CoreAudio 可能导致数据丢失
declare -a CRITICAL_AUDIO_APPS=(
  "Logic Pro"
  "Logic Pro X"
  "GarageBand"
  "Pro Tools"
  "Ableton Live"
  "Cubase"
  "Studio One"
  "Reaper"
  "Reason"
  "FL Studio"
)

# 普通音频应用列表
declare -a NORMAL_AUDIO_APPS=(
  "Music"
  "Spotify"
  "Zoom"
  "FaceTime"
  "QuickTime Player"
  "VLC"
  "IINA"
  "Safari"
  "Chrome"
)

# 检查是否有关键音频应用正在运行
# 返回值：0=有关键应用运行，1=没有
# 输出：正在运行的关键应用列表
check_critical_audio_apps() {
  local apps=""

  for app in "${CRITICAL_AUDIO_APPS[@]}"; do
    if pgrep -x "${app}" >/dev/null 2>&1 || pgrep -f "${app}" >/dev/null 2>&1; then
      apps="${apps}${app}, "
    fi
  done

  # 去除末尾的逗号和空格
  apps="${apps%, }"

  if [[ -n "${apps}" ]]; then
    echo "${apps}"
    return 0
  fi

  return 1
}

# 检查是否有普通音频应用正在运行
check_normal_audio_apps() {
  local apps=""

  for app in "${NORMAL_AUDIO_APPS[@]}"; do
    if pgrep -x "${app}" >/dev/null 2>&1 || pgrep -f "${app}" >/dev/null 2>&1; then
      apps="${apps}${app}, "
    fi
  done

  # 去除末尾的逗号和空格
  apps="${apps%, }"

  if [[ -n "${apps}" ]]; then
    echo "${apps}"
    return 0
  fi

  return 1
}

# [兼容] 旧的函数名，现在检查所有音频应用
check_running_audio_apps() {
  local critical_apps=""
  local normal_apps=""
  local all_apps=""

  if check_critical_audio_apps >/dev/null 2>&1; then
    critical_apps=$(check_critical_audio_apps)
    all_apps="${critical_apps}"
  fi

  if check_normal_audio_apps >/dev/null 2>&1; then
    normal_apps=$(check_normal_audio_apps)
    if [[ -n "${all_apps}" ]]; then
      all_apps="${all_apps}, ${normal_apps}"
    else
      all_apps="${normal_apps}"
    fi
  fi

  if [[ -n "${all_apps}" ]]; then
    echo "${all_apps}"
    return 0
  fi

  return 1
}

coreaudio_kickstart_once() {
  if [[ "${RESTART_COREAUDIO}" != "true" ]]; then
    log_warn "跳过 CoreAudio 重启 (--no-coreaudio-restart)"
    return 0
  fi

  # ⚠️ 强烈警告：CoreAudio 重启会中断所有音频
  echo ""
  log_warn "═══════════════════════════════════════════════════════════"
  log_warn "⚠️  警告：即将重启 CoreAudio 服务"
  log_warn "═══════════════════════════════════════════════════════════"
  log_warn ""
  log_warn "这将强制终止所有正在进行的音频会话，包括："
  log_warn "  • 正在播放的音乐或视频（Music, Spotify, VLC 等）"
  log_warn "  • 正在进行的视频通话（Zoom, FaceTime 等）"
  log_warn "  • 正在录音的音频软件（Logic Pro, GarageBand 等）"
  log_warn "  • 浏览器中的音频/视频播放"
  log_warn ""

  # [严格检查] 检测关键音频应用（必须退出）
  local critical_apps
  if critical_apps=$(check_critical_audio_apps); then
    log_error ""
    log_error "═══════════════════════════════════════════════════════════"
    log_error "🛑 检测到关键音频应用正在运行"
    log_error "═══════════════════════════════════════════════════════════"
    log_error ""
    log_error "以下应用必须退出后才能继续："
    log_error "  ${critical_apps}"
    log_error ""
    log_error "原因："
    log_error "  这些应用通常涉及未保存的工程或项目文件，"
    log_error "  直接重启 CoreAudio 可能导致数据丢失。"
    log_error ""
    log_error "请执行以下操作："
    log_error "  1. 保存所有工作并退出上述应用"
    log_error "  2. 重新运行此安装脚本"
    log_error ""
    log_error "或者使用 --no-coreaudio-restart 参数跳过重启："
    log_error "  ./scripts/install.sh install --no-coreaudio-restart"
    log_error "═══════════════════════════════════════════════════════════"
    echo ""
    return 1
  fi

  # [普通检查] 检测普通音频应用（仅警告）
  local running_apps
  if running_apps=$(check_normal_audio_apps); then
    log_warn "检测到以下音频相关应用正在运行："
    log_warn "  ${running_apps}"
    log_warn ""
    log_warn "这些应用将在重启后恢复，但当前会话会被中断！"
    log_warn ""
  fi

  log_warn "建议操作："
  log_warn "  • 先手动暂停或保存您的工作"
  log_warn "  • 或使用 --no-coreaudio-restart 参数跳过重启"
  log_warn "  • 稍后手动运行：sudo launchctl kickstart -k system/com.apple.coreaudiod"
  log_warn "═══════════════════════════════════════════════════════════"
  echo ""

  # 要求用户确认
  local confirm
  echo -n "是否确认重启 CoreAudio? [y/N] "
  read -r confirm

  if [[ ! "${confirm}" =~ ^[Yy]$ ]]; then
    log_info "已取消 CoreAudio 重启"
    log_info "注意：驱动已安装，但需要重启 CoreAudio 才能生效"
    log_info "您可以稍后手动重启：sudo launchctl kickstart -k system/com.apple.audio.coreaudiod"
    return 0
  fi

  # 在重启前先清理所有相关进程
  kill_audioctl_processes

  # 额外等待以确保端口释放
  sleep 1

  log_info "重启 CoreAudio（仅一次）..."
  sudo /bin/launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || true
}

wait_coreaudiod() {
  local timeout_sec="${1:-15}"
  local start
  start="$(date +%s)"

  while true; do
    if pgrep -x coreaudiod >/dev/null 2>&1; then
      return 0
    fi
    if (("$(date +%s)" - start >= timeout_sec)); then
      return 1
    fi
    sleep 1
  done
}

adhoc_sign_bundle() {
  local bundle_path="$1"
  if [[ "${ADHOC_SIGN}" != "true" ]]; then
    return 0
  fi
  log_info "临时签名: ${bundle_path}"
  sudo /usr/bin/codesign --force --deep --sign - "${bundle_path}"
  # [新增：验证签名完整性，避免安装损坏的 bundle 导致 coreaudiod 异常]
  sudo /usr/bin/codesign --verify --deep --strict "${bundle_path}" || {
    log_error "签名验证失败: ${bundle_path}"
    exit 1
  }
}

find_built_driver() {
  local cand1="${BUILD_DIR}/${DRIVER_NAME}.driver"
  local cand2="${BUILD_DIR}/driver/${DRIVER_NAME}.driver"
  local cand3="${BUILD_DIR}/drivers/${DRIVER_NAME}.driver"

  if [[ -d "${cand1}" ]]; then
    echo "${cand1}"
    return 0
  fi
  if [[ -d "${cand2}" ]]; then
    echo "${cand2}"
    return 0
  fi
  if [[ -d "${cand3}" ]]; then
    echo "${cand3}"
    return 0
  fi

  local found
  found="$(find "${BUILD_DIR}" -maxdepth 6 -type d -name "${DRIVER_NAME}.driver" 2>/dev/null | head -n 1 || true)"
  if [[ -z "${found}" ]]; then
    return 1
  fi
  echo "${found}"
}

install_driver_bundle() {
  log_step "安装 HAL 驱动"
  local built_driver
  built_driver="$(find_built_driver)" || {
    log_error "未找到构建好的驱动: ${BUILD_DIR}"
    log_info "请检查 CMake 输出路径中是否存在 ${DRIVER_NAME}.driver"
    exit 1
  }

  # Stage to temp, sign there, then compare hashes to avoid unnecessary restarts
  local stage_root
  stage_root="$(mktemp -d)"
  # [注意：不使用 trap，改为在函数结束前手动清理，避免 set -u 下的变量作用域问题]

  local stage_driver="${stage_root}/${DRIVER_NAME}.driver"
  /usr/bin/ditto "${built_driver}" "${stage_driver}"

  # Remove extended attributes to reduce false diffs
  /usr/bin/xattr -cr "${stage_driver}" 2>/dev/null || true

  # Sign stage first
  adhoc_sign_bundle "${stage_driver}"

  local new_hash old_hash
  new_hash="$(hash_dir "${stage_driver}")"
  old_hash="$(hash_dir "${DRIVER_DST}")"

  if [[ "${new_hash}" == "${old_hash}" ]]; then
    # [重要：清理临时目录后再返回]
    rm -rf "${stage_root}"
    log_success "驱动未变化，跳过安装和重启"
    return 0
  fi

  log_info "驱动有变化或新安装"
  log_info "复制到: ${DRIVER_DST}"

  sudo /bin/rm -rf "${DRIVER_DST}"
  sudo /usr/bin/ditto "${stage_driver}" "${DRIVER_DST}"
  sudo /usr/sbin/chown -R root:wheel "${DRIVER_DST}"
  sudo /bin/chmod -R 755 "${DRIVER_DST}"
  sudo /usr/bin/xattr -rd com.apple.quarantine "${DRIVER_DST}" 2>/dev/null || true

  # [重要：安装完成后清理临时目录]
  rm -rf "${stage_root}"

  # Single restart after copy
  coreaudio_kickstart_once
  if wait_coreaudiod 20; then
    log_success "CoreAudio 运行正常"
  else
    log_warn "CoreAudio 未在预期时间内启动"
  fi
}

cmd_install() {
  log_step "构建"
  need_cmd cmake
  need_cmd ninja
  detect_build_dir

  log_info "构建类型=${BUILD_TYPE}"
  log_info "构建目录=${BUILD_DIR}"
  log_info "并行任务=${PARALLEL_JOBS}"
  log_info "重启 CoreAudio=${RESTART_COREAUDIO}"
  log_info "临时签名=${ADHOC_SIGN}"

  /bin/mkdir -p "${BUILD_DIR}"
  cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

  log_info "构建音频驱动..."
  cmake --build "${BUILD_DIR}" --target virtual_audio_driver -j "${PARALLEL_JOBS}"

  log_info "构建 audioctl 工具..."
  cmake --build "${BUILD_DIR}" --target audioctl -j "${PARALLEL_JOBS}"

  install_driver_bundle

  log_success "安装完成"
  log_info "驱动已安装到: ${DRIVER_DST}"

  # 显示 audioctl 使用提示
  if [[ -f "${BUILD_DIR}/bin/audioctl" ]]; then
    log_info "audioctl 工具: ${BUILD_DIR}/bin/audioctl"
    echo ""
    echo "========== 使用提示 =========="
    echo "  ${BUILD_DIR}/bin/audioctl help              # 查看所有命令"
    echo "  ${BUILD_DIR}/bin/audioctl virtual-status    # 检查虚拟设备状态"
    echo "  ${BUILD_DIR}/bin/audioctl use-virtual       # 切换到虚拟设备"
    echo "  ${BUILD_DIR}/bin/audioctl use-physical      # 恢复物理设备"
    echo "  ${BUILD_DIR}/bin/audioctl app-volumes       # 查看应用音量列表"
    echo "=============================="
  fi
}

cmd_uninstall() {
  log_step "卸载"

  # 先清理进程
  kill_audioctl_processes

  # 【新增】删除 Aggregate Device
  local audioctl_bin="${BUILD_DIR:-${PROJECT_DIR}/cmake-build-debug}/bin/audioctl"
  if [[ -f "${audioctl_bin}" ]]; then
    log_info "检查并删除 Aggregate Device..."
    # 先检查是否存在 Aggregate Device
    local agg_status
    agg_status="$(${audioctl_bin} agg-status 2>&1)" || true
    if [[ "${agg_status}" == *"已就绪"* ]] || [[ "${agg_status}" == *"已创建"* ]]; then
      # 先恢复到物理设备（如果 Aggregate 是默认）
      local current_default
      current_default="$(system_profiler SPAudioDataType 2>/dev/null | grep -A 5 "Default Output" | grep "Device:" | head -1)" || true
      if [[ "${current_default}" == *"AudioCTL Aggregate"* ]]; then
        log_warn "Aggregate Device 是当前默认设备，先恢复物理设备..."
        "${audioctl_bin}" use-physical 2>/dev/null || true
        sleep 1
      fi

      # 删除 Aggregate Device
      log_info "删除 Aggregate Device..."
      "${audioctl_bin}" internal-delete-aggregate 2>/dev/null || {
        log_warn "使用内部命令删除失败，尝试备用方法..."
        # 备用方法：直接使用 CoreAudio API 删除
        # 查找所有 Aggregate Device 并删除
        local agg_devices
        agg_devices="$(system_profiler SPAudioDataType 2>/dev/null | grep -B 5 "AudioCTL Aggregate" | grep "Device ID:" | awk '{print $3}')" || true
        if [[ -n "${agg_devices}" ]]; then
          echo "${agg_devices}" | while read -r device_id; do
            if [[ -n "${device_id}" ]]; then
              log_info "删除 Aggregate Device ID: ${device_id}"
              # 使用 Python 调用 AudioHardwareDestroyAggregateDevice
              python3 <<EOF 2>/dev/null || true
import ctypes
import sys
AudioHardwareDestroyAggregateDevice = ctypes.CDLL(None).AudioHardwareDestroyAggregateDevice
AudioHardwareDestroyAggregateDevice.argtypes = [ctypes.c_uint32]
AudioHardwareDestroyAggregateDevice.restype = ctypes.c_int32
device_id = int("${device_id}")
result = AudioHardwareDestroyAggregateDevice(device_id)
print(f"删除结果: {result}")
EOF
            fi
          done
        fi
      }
      sleep 1
      log_success "Aggregate Device 已删除"
    else
      log_info "没有找到 AudioCTL Aggregate Device"
    fi
  else
    log_warn "未找到 audioctl 工具，跳过 Aggregate Device 删除"
  fi

  # 尝试恢复到物理设备（如果虚拟设备当前是默认设备）
  local current_default
  current_default="$(system_profiler SPAudioDataType 2>/dev/null | grep -A 5 "Default Output" | grep "Device:" | head -1)" || true
  if [[ "${current_default}" == *"Virtual"* ]] || [[ "${current_default}" == *"audioctl"* ]]; then
    log_warn "虚拟设备当前是默认设备，尝试恢复物理设备..."
    # 尝试使用 audioctl 恢复
    if [[ -f "${audioctl_bin}" ]]; then
      "${audioctl_bin}" use-physical 2>/dev/null || true
    fi
    sleep 1
  fi

  if [[ -d "${DRIVER_DST}" ]]; then
    log_info "删除: ${DRIVER_DST}"
    sudo /bin/rm -rf "${DRIVER_DST}"
    log_success "驱动已删除"
    coreaudio_kickstart_once
    wait_coreaudiod 20 || true
  else
    log_warn "驱动未安装"
  fi
}

cmd_status() {
  log_step "状态"
  if [[ -d "${DRIVER_DST}" ]]; then
    log_success "驱动: 已安装 (${DRIVER_DST})"
  else
    log_warn "驱动: 未安装 (${DRIVER_DST})"
  fi

  if pgrep -x coreaudiod >/dev/null 2>&1; then
    log_success "coreaudiod: 运行中"
  else
    log_warn "coreaudiod: 未运行"
  fi
}

main() {
  require_not_root
  cd "${PROJECT_DIR}"

  local cmd="${1:-help}"
  shift || true

  while [[ $# -gt 0 ]]; do
    case "$1" in
    --release)
      BUILD_TYPE="Release"
      shift
      ;;
    --no-coreaudio-restart)
      RESTART_COREAUDIO="false"
      shift
      ;;
    --no-adhoc-sign)
      ADHOC_SIGN="false"
      shift
      ;;
    *) break ;;
    esac
  done

  case "${cmd}" in
  install) cmd_install "$@" ;;
  uninstall) cmd_uninstall "$@" ;;
  status) cmd_status "$@" ;;
  help | --help | -h | "")
    cat <<EOF
用法:
  ./scripts/install.sh install [--release] [--no-coreaudio-restart] [--no-adhoc-sign]
  ./scripts/install.sh uninstall [--no-coreaudio-restart]
  ./scripts/install.sh status
EOF
    ;;
  *)
    log_error "未知命令: ${cmd}"
    exit 2
    ;;
  esac
}

main "$@"
