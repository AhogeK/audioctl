#!/usr/bin/env bash

# AudioCtl 安装脚本
# 用法: ./install.sh [命令] [选项]

set -euo pipefail
IFS=$'\n\t'

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_TYPE="Debug"
PARALLEL_JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

DRIVER_NAME="VirtualAudioDriver"
DRIVER_PATH="/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}.driver"

CMAKE=""
NINJA=""
BUILD_DIR=""

# 运行选项
RESTART_COREAUDIO="true"
HARD_RESTART_COREAUDIO="false" # 禁用强制重启
AUTO_ROLLBACK="true"
CPU_ROLLBACK_THRESHOLD="80" # 阈值再调低，更敏感
CPU_ROLLBACK_WINDOW_SEC="2" # 反应更快
ADHOC_SIGN="true"

RED='\e[0;31m'
GREEN='\e[0;32m'
YELLOW='\e[1;33m'
BLUE='\e[0;34m'
CYAN='\e[0;36m'
NC='\e[0m'

log_info() { printf "${BLUE}[信息]${NC} %s\n" "$1"; }
log_success() { printf "${GREEN}[成功]${NC} %s\n" "$1"; }
log_warn() { printf "${YELLOW}[警告]${NC} %s\n" "$1"; }
log_error() { printf "${RED}[错误]${NC} %s\n" "$1"; }
log_step() { printf "\n${CYAN}========== %s ==========${NC}\n" "$1"; }

check_not_root() {
  if [[ "${EUID}" -eq 0 ]]; then
    log_error "不要使用 sudo 运行此脚本。"
    log_info "脚本会在需要时自动请求 sudo 权限。"
    exit 1
  fi
}

detect_tools() {
  CMAKE="$(command -v cmake || true)"
  NINJA="$(command -v ninja || true)"

  if [[ -z "${CMAKE}" ]]; then
    log_error "找不到 cmake。请安装: brew install cmake"
    exit 1
  fi
  if [[ -z "${NINJA}" ]]; then
    log_error "找不到 ninja。请安装: brew install ninja"
    exit 1
  fi

  if [[ "${BUILD_TYPE}" == "Release" || "${BUILD_TYPE}" == "release" ]]; then
    BUILD_TYPE="Release"
    BUILD_DIR="${PROJECT_DIR}/cmake-build-release"
  else
    BUILD_TYPE="Debug"
    BUILD_DIR="${PROJECT_DIR}/cmake-build-debug"
  fi

  if [[ -d "${PROJECT_DIR}/cmake-build-Debug" && "${BUILD_DIR}" != "${PROJECT_DIR}/cmake-build-Debug" ]]; then
    log_warn "删除旧的构建目录: cmake-build-Debug"
    rm -rf "${PROJECT_DIR}/cmake-build-Debug"
  fi
}

coreaudio_kickstart() {
  sudo launchctl kickstart -k system/com.apple.audio.coreaudiod >/dev/null 2>&1 || true
}

coreaudio_stop_soft() {
  sudo killall coreaudiod >/dev/null 2>&1 || true
}

coreaudio_stop_hard() {
  sudo killall -9 coreaudiod >/dev/null 2>&1 || true
}

coreaudio_wait_healthy() {
  local timeout_sec="${1:-10}"
  local start
  start="$(date +%s)"

  while true; do
    if pgrep -x coreaudiod >/dev/null 2>&1; then
      return 0
    fi
    local now
    now="$(date +%s)"
    if ((now - start >= timeout_sec)); then
      return 1
    fi
    sleep 0.2
  done
}

coreaudio_cpu_pct() {
  local pid="$1"
  local v
  v="$(ps -o %cpu= -p "${pid}" 2>/dev/null | awk '{print $1}' | head -n 1)"
  if [[ -z "${v}" ]]; then
    echo "0"
    return 0
  fi
  echo "${v}" | awk '{printf "%d\n", ($1+0.5)}'
}

coreaudio_watchdog_or_rollback() {
  if [[ "${AUTO_ROLLBACK}" != "true" ]]; then
    return 0
  fi

  if ! pgrep -x coreaudiod >/dev/null 2>&1; then
    log_warn "coreaudiod 未运行，跳过 CPU 监控。"
    return 0
  fi

  local pid
  pid="$(pgrep -x coreaudiod | head -n 1)"

  log_info "CoreAudio 监控: 阈值=${CPU_ROLLBACK_THRESHOLD}% 窗口=${CPU_ROLLBACK_WINDOW_SEC}秒 pid=${pid}"
  local hit=0
  for _ in $(seq 1 "${CPU_ROLLBACK_WINDOW_SEC}"); do
    local cpu
    cpu="$(coreaudio_cpu_pct "${pid}")"
    if ((cpu >= CPU_ROLLBACK_THRESHOLD)); then
      hit=$((hit + 1))
    else
      hit=0
    fi
    sleep 1
  done

  if ((hit >= CPU_ROLLBACK_WINDOW_SEC)); then
    log_error "coreaudiod CPU 连续 ${CPU_ROLLBACK_WINDOW_SEC} 秒保持在 >= ${CPU_ROLLBACK_THRESHOLD}%。正在回滚驱动。"
    if [[ -d "${DRIVER_PATH}" ]]; then
      sudo rm -rf "${DRIVER_PATH}" || true
    fi

    log_step "重启 CoreAudio (回滚)"
    coreaudio_stop_soft
    coreaudio_kickstart
    if coreaudio_wait_healthy 10; then
      log_success "回滚完成，CoreAudio 已重启。"
    else
      log_warn "尝试回滚，但 CoreAudio 可能仍处于非健康状态。"
    fi

    exit 2
  fi
}

adhoc_sign_driver_if_needed() {
  if [[ "${ADHOC_SIGN}" != "true" ]]; then
    return 0
  fi
  if [[ ! -d "${DRIVER_PATH}" ]]; then
    return 0
  fi

  log_info "对驱动进行 Ad-hoc 签名: ${DRIVER_PATH}"
  sudo codesign --force --deep --sign - "${DRIVER_PATH}" >/dev/null 2>&1 || true
}

cmake_configure() {
  log_step "配置项目"
  mkdir -p "${BUILD_DIR}"

  "${CMAKE}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_MAKE_PROGRAM="${NINJA}" \
    -G Ninja \
    -S "${PROJECT_DIR}" \
    -B "${BUILD_DIR}"

  log_success "配置完成"
}

cmake_build_targets() {
  local driver_only="$1"

  log_step "编译驱动"
  "${CMAKE}" --build "${BUILD_DIR}" --target virtual_audio_driver -j "${PARALLEL_JOBS}"
  log_success "驱动编译完成"

  if [[ "${driver_only}" == "false" ]]; then
    log_step "编译 audioctl"
    "${CMAKE}" --build "${BUILD_DIR}" --target audioctl -j "${PARALLEL_JOBS}"
    log_success "audioctl 编译完成"
  fi
}

install_driver() {
  log_step "安装驱动"
  sudo "${CMAKE}" --install "${BUILD_DIR}"

  log_info "设置权限 root:wheel 755 并移除隔离标记..."
  sudo chown -R root:wheel "${DRIVER_PATH}"
  sudo chmod -R 755 "${DRIVER_PATH}"
  # 移除 macOS 自动添加的隔离属性
  sudo xattr -rd com.apple.quarantine "${DRIVER_PATH}" 2>/dev/null || true

  log_success "驱动已安装: ${DRIVER_PATH}"

  adhoc_sign_driver_if_needed
}

restart_coreaudio_safe() {
  # [修改：新增 skip_stop 参数控制]
  local skip_stop="${1:-false}"

  if [[ "${RESTART_COREAUDIO}" != "true" ]]; then
    log_warn "跳过重启 CoreAudio (--no-coreaudio-restart)。"
    return 0
  fi

  log_step "重启 CoreAudio"

  if [[ "${HARD_RESTART_COREAUDIO}" == "true" ]]; then
    log_warn "已启用强制重启。这可能会中断系统音频。"
    coreaudio_stop_hard
  # [修改：仅当未在清理阶段停止过服务时，才执行软停止]
  # Avoid killing the daemon twice in a short period to prevent launchd trashing
  elif [[ "${skip_stop}" != "true" ]]; then
    coreaudio_stop_soft
  else
    log_info "CoreAudio 已在清理阶段停止，直接执行 Kickstart..."
  fi

  coreaudio_kickstart

  log_info "等待 CoreAudio 恢复..."
  if coreaudio_wait_healthy 12; then
    log_success "CoreAudio 正在运行。"
  else
    log_warn "CoreAudio 未能及时恢复，正在重试 kickstart..."
    coreaudio_kickstart
    if coreaudio_wait_healthy 8; then
      log_success "CoreAudio 正在运行。"
    else
      log_warn "CoreAudio 可能处于非健康状态。"
    fi
  fi

  coreaudio_watchdog_or_rollback
}

cmd_install() {
  local run_tests="false"
  local driver_only="false"
  local skip_existing="false"
  local force="false"

  # [修改：新增状态标记变量]
  # Track if we forced a stop during the cleanup phase
  local coreaudio_stopped_during_cleanup="false"

  while [[ $# -gt 0 ]]; do
    case "$1" in
    --release)
      BUILD_TYPE="Release"
      shift
      ;;
    -j | --jobs)
      PARALLEL_JOBS="$2"
      shift 2
      ;;
    --test)
      run_tests="true"
      shift
      ;;
    --driver-only)
      driver_only="true"
      shift
      ;;
    --skip-existing)
      skip_existing="true"
      shift
      ;;
    --force)
      force="true"
      shift
      ;;
    --no-coreaudio-restart)
      RESTART_COREAUDIO="false"
      shift
      ;;
    --hard-coreaudio-restart)
      HARD_RESTART_COREAUDIO="true"
      shift
      ;;
    --no-auto-rollback)
      AUTO_ROLLBACK="false"
      shift
      ;;
    --adhoc-sign)
      ADHOC_SIGN="true"
      shift
      ;;
    *)
      log_error "未知选项: $1"
      exit 1
      ;;
    esac
  done

  log_step "安装 AudioCtl"
  log_info "编译类型: ${BUILD_TYPE}"
  log_info "并行任务: ${PARALLEL_JOBS}"
  log_info "重启 CoreAudio: ${RESTART_COREAUDIO} (强制=${HARD_RESTART_COREAUDIO})"
  log_info "自动回滚: ${AUTO_ROLLBACK}"
  log_info "Ad-hoc 签名: ${ADHOC_SIGN}"

  if [[ -d "${DRIVER_PATH}" ]]; then
    if [[ "${skip_existing}" == "true" ]]; then
      log_info "驱动已安装，跳过 (--skip-existing)。"
      return 0
    fi
    if [[ "${force}" == "true" ]]; then
      log_info "驱动已安装，强制重新安装 (--force)。"
    else
      log_info "驱动已安装，正在重新安装。"
    fi
  fi

  detect_tools
  log_info "构建目录: ${BUILD_DIR}"

  log_step "清理残留音频配置"
  sudo rm -rf /var/db/CoreAudio/device-settings.plist 2>/dev/null || true
  sudo rm -rf /var/db/CoreAudio/com.apple.audio.DeviceSettings.plist 2>/dev/null || true

  cmake_configure
  cmake_build_targets "${driver_only}"

  if [[ -d "${DRIVER_PATH}" ]]; then
    log_step "移除旧驱动"
    # 尝试直接移除，如果失败（通常因为文件被占用）则停止服务后重试
    if ! sudo rm -rf "${DRIVER_PATH}" 2>/dev/null; then
      log_warn "驱动移除失败 (正在使用)。正在停止 CoreAudio 并重试..."

      # [修改：此处停止后标记状态，避免后续重复停止]
      coreaudio_stop_soft
      coreaudio_stopped_during_cleanup="true"

      sleep 1
      if ! sudo rm -rf "${DRIVER_PATH}"; then
        log_error "无法移除旧驱动，请检查权限或手动重启。"
        exit 1
      fi
    fi
  fi

  install_driver

  # [修改：根据之前的状态决定是否跳过停止动作]
  # If we stopped it during cleanup, pass "true" to skip the redundant stop
  restart_coreaudio_safe "${coreaudio_stopped_during_cleanup}"

  if [[ "${run_tests}" == "true" ]]; then
    log_step "运行测试"
    "${CMAKE}" --build "${BUILD_DIR}" --target test_virtual_audio_device -j "${PARALLEL_JOBS}"
    "${BUILD_DIR}/tests/test_virtual_audio_device"
    log_success "测试完成"
  fi

  log_step "验证安装"
  if [[ -d "${DRIVER_PATH}" ]]; then
    log_success "驱动程序包存在"
  else
    log_error "安装后驱动程序包丢失"
    exit 1
  fi

  if [[ "${driver_only}" == "false" && -f "${BUILD_DIR}/bin/audioctl" ]]; then
    log_success "audioctl 可执行文件存在"
    "${BUILD_DIR}/bin/audioctl" --version >/dev/null 2>&1 || true
  fi

  printf "\n"
  log_success "完成。"
  printf "\n"
  # shellcheck disable=SC2059
  printf "${CYAN}快速开始:${NC}\n"
  printf "  %s/bin/audioctl virtual-status\n" "${BUILD_DIR}"
  printf "  %s/bin/audioctl use-virtual\n" "${BUILD_DIR}"
  printf "  %s/bin/audioctl app-volumes\n" "${BUILD_DIR}"
  printf "  %s/bin/audioctl app-volume Safari 50\n" "${BUILD_DIR}"
  printf "\n"
}

cmd_uninstall() {
  local quiet="false"
  while [[ $# -gt 0 ]]; do
    case "$1" in
    --quiet)
      quiet="true"
      shift
      ;;
    --hard-coreaudio-restart)
      HARD_RESTART_COREAUDIO="true"
      shift
      ;;
    --no-coreaudio-restart)
      RESTART_COREAUDIO="false"
      shift
      ;;
    *) break ;;
    esac
  done

  [[ "${quiet}" == "false" ]] && log_step "卸载 AudioCtl"

  if [[ -d "${DRIVER_PATH}" ]]; then
    [[ "${quiet}" == "false" ]] && log_info "正在移除驱动: ${DRIVER_PATH}"
    sudo rm -rf "${DRIVER_PATH}" || true
    [[ "${quiet}" == "false" ]] && log_success "驱动已移除"
  else
    [[ "${quiet}" == "false" ]] && log_warn "驱动未安装"
  fi

  restart_coreaudio_safe
  [[ "${quiet}" == "false" ]] && log_success "卸载完成"
}

cmd_reinstall() {
  cmd_uninstall --quiet
  cmd_install "$@"
}

cmd_status() {
  log_step "查看状态"

  # shellcheck disable=SC2059
  printf "${CYAN}驱动状态:${NC}\n"
  if [[ -d "${DRIVER_PATH}" ]]; then
    log_success "已安装: ${DRIVER_PATH}"
    # shellcheck disable=SC2012
    ls -la "${DRIVER_PATH}" | head -5 || true
  else
    log_warn "未安装"
  fi

  # shellcheck disable=SC2059
  printf "\n${CYAN}二进制文件:${NC}\n"
  if [[ -f "${PROJECT_DIR}/cmake-build-debug/bin/audioctl" ]]; then
    log_success "已编译 Debug 版本 audioctl"
    "${PROJECT_DIR}/cmake-build-debug/bin/audioctl" --version >/dev/null 2>&1 || true
  elif [[ -f "${PROJECT_DIR}/cmake-build-release/bin/audioctl" ]]; then
    log_success "已编译 Release 版本 audioctl"
    "${PROJECT_DIR}/cmake-build-release/bin/audioctl" --version >/dev/null 2>&1 || true
  else
    log_warn "audioctl 未编译"
  fi

  # shellcheck disable=SC2059
  printf "\n${CYAN}CoreAudio 状态:${NC}\n"
  if pgrep -x coreaudiod >/dev/null 2>&1; then
    local pid
    pid="$(pgrep -x coreaudiod | head -n 1)"
    local cpu
    cpu="$(coreaudio_cpu_pct "${pid}")"
    log_success "coreaudiod 正在运行: pid=${pid} cpu=${cpu}%"
  else
    log_warn "coreaudiod 未运行"
  fi
}

cmd_test() {
  local build_type="${BUILD_TYPE}"
  while [[ $# -gt 0 ]]; do
    case "$1" in
    --release)
      build_type="Release"
      shift
      ;;
    *) break ;;
    esac
  done

  detect_tools
  # shellcheck disable=SC2155
  local build_dir="${PROJECT_DIR}/cmake-build-$(echo "${build_type}" | tr '[:upper:]' '[:lower:]')"
  if [[ "${build_type}" == "Debug" ]]; then
    build_dir="${PROJECT_DIR}/cmake-build-debug"
  else
    build_dir="${PROJECT_DIR}/cmake-build-release"
  fi

  log_step "运行测试"
  if [[ ! -d "${build_dir}" ]]; then
    log_error "找不到构建目录: ${build_dir}"
    log_info "运行: ./install.sh install"
    exit 1
  fi

  "${CMAKE}" --build "${build_dir}" --target test_virtual_audio_device
  "${build_dir}/tests/test_virtual_audio_device"
  log_success "测试完成"
}

cmd_clean() {
  log_step "清理项目"
  cmd_uninstall --quiet --no-auto-rollback --no-coreaudio-restart || true

  for dir in "${PROJECT_DIR}"/cmake-build-*; do
    if [[ -d "${dir}" ]]; then
      log_info "正在删除: ${dir}"
      rm -rf "${dir}"
    fi
  done
  log_success "清理完成"
}

cmd_help() {
  # shellcheck disable=SC2059
  printf "${CYAN}AudioCtl 安装脚本${NC}\n\n"
  # shellcheck disable=SC2059
  printf "${GREEN}用法:${NC}\n"
  printf "  ./install.sh [命令] [选项]\n\n"
  # shellcheck disable=SC2059
  printf "${GREEN}命令:${NC}\n"
  printf "  install      编译并安装驱动 (+ audioctl)\n"
  printf "  uninstall    卸载驱动\n"
  printf "  reinstall    卸载并重新安装\n"
  printf "  status       查看安装状态\n"
  printf "  test         运行测试\n"
  printf "  clean        删除构建文件并卸载\n"
  printf "  help         显示此帮助信息\n\n"
  # shellcheck disable=SC2059
  printf "${GREEN}安装选项:${NC}\n"
  printf "  --release                使用 Release 模式编译 (默认为 Debug)\n"
  printf "  -j, --jobs N             并行任务数\n"
  printf "  --test                   安装后运行测试\n"
  printf "  --driver-only            仅编译/安装驱动\n"
  printf "  --skip-existing          如果驱动已存在则跳过\n"
  printf "  --force                  强制重新安装\n"
  printf "  --no-coreaudio-restart   不重启 CoreAudio\n"
  printf "  --hard-coreaudio-restart 使用 kill -9 重启 CoreAudio (存在风险)\n"
  printf "  --no-auto-rollback       禁用 CPU 自动回滚监控\n"
  printf "  --adhoc-sign             对驱动程序包进行 Ad-hoc 签名\n\n"
}

main() {
  if [[ ! -f "${PROJECT_DIR}/CMakeLists.txt" ]]; then
    log_error "找不到项目根目录。"
    exit 1
  fi
  cd "${PROJECT_DIR}"

  local command="${1:-help}"
  shift || true

  case "${command}" in
  install)
    check_not_root
    detect_tools
    cmd_install "$@"
    ;;
  uninstall)
    check_not_root
    detect_tools || true
    cmd_uninstall "$@"
    ;;
  reinstall)
    check_not_root
    detect_tools
    cmd_reinstall "$@"
    ;;
  status)
    check_not_root
    cmd_status
    ;;
  test)
    check_not_root
    cmd_test "$@"
    ;;
  clean)
    check_not_root
    cmd_clean
    ;;
  help | --help | -h)
    cmd_help
    ;;
  *)
    log_error "未知命令: ${command}"
    printf "运行 './install.sh help' 查看用法。\n"
    exit 1
    ;;
  esac
}

main "$@"
