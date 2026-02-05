#!/bin/bash
#
# AudioCtl 安装脚本
# 用法: ./install.sh [命令] [选项]
#

set -e  # 遇到错误立即退出

# 项目根目录
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# 默认配置
BUILD_TYPE="Debug"
PARALLEL_JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
DRIVER_NAME="VirtualAudioDriver"
DRIVER_PATH="/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}.driver"
BUILD_DIR=""

# 颜色定义
RED='\e[0;31m'
GREEN='\e[0;32m'
YELLOW='\e[1;33m'
BLUE='\e[0;34m'
CYAN='\e[0;36m'
BOLD='\e[1m'
NC='\e[0m'

# 工具路径
CMAKE=""
NINJA=""

# ============================================
# 工具函数
# ============================================

log_info() {
    printf "${BLUE}[INFO]${NC} %s\n" "$1"
}

log_success() {
    printf "${GREEN}[OK]${NC} %s\n" "$1"
}

log_warn() {
    printf "${YELLOW}[WARN]${NC} %s\n" "$1"
}

log_error() {
    printf "${RED}[ERROR]${NC} %s\n" "$1"
}

log_step() {
    printf "\n${CYAN}========== %s ==========${NC}\n" "$1"
}

check_root() {
    if [ "$EUID" -eq 0 ]; then
        log_error "请不要使用 sudo 运行此脚本"
        log_info "脚本会在需要时自动请求 sudo 权限"
        exit 1
    fi
}

detect_tools() {
    CMAKE=$(which cmake)
    NINJA=$(which ninja)
    
    if [ -z "$CMAKE" ]; then
        log_error "找不到 cmake，请安装: brew install cmake"
        exit 1
    fi
    
    if [ -z "$NINJA" ]; then
        log_error "找不到 ninja，请安装: brew install ninja"
        exit 1
    fi
    
    # 设置构建目录
    BUILD_DIR="${PROJECT_DIR}/cmake-build-${BUILD_TYPE}"
}

# ============================================
# 命令: install
# ============================================

cmd_install() {
    local run_tests=false
    local driver_only=false
    local skip_existing=false
    local force=false
    
    # 解析选项
    while [[ $# -gt 0 ]]; do
        case $1 in
            --release)
                BUILD_TYPE="Release"
                BUILD_DIR="${PROJECT_DIR}/cmake-build-${BUILD_TYPE}"
                shift
                ;;
            -j|--jobs)
                PARALLEL_JOBS="$2"
                shift 2
                ;;
            --test)
                run_tests=true
                shift
                ;;
            --driver-only)
                driver_only=true
                shift
                ;;
            --skip-existing)
                skip_existing=true
                shift
                ;;
            --force)
                force=true
                shift
                ;;
            *)
                log_error "未知选项: $1"
                exit 1
                ;;
        esac
    done
    
    log_step "安装 AudioCtl"
    log_info "构建类型: ${BUILD_TYPE}"
    log_info "并行任务: ${PARALLEL_JOBS}"
    log_info "构建目录: ${BUILD_DIR}"
    
    # 检查是否已安装
    local is_installed=false
    if [ -d "$DRIVER_PATH" ]; then
        is_installed=true
    fi
    
    if [ "$is_installed" = true ] && [ "$skip_existing" = true ]; then
        log_info "检测到已安装，使用 --skip-existing 跳过"
        log_info "要更新到最新版本，请运行: ./install.sh reinstall"
        return 0
    fi
    
    if [ "$is_installed" = true ]; then
        if [ "$force" = true ]; then
            log_info "检测到已安装，--force 强制重装..."
        else
            log_info "检测到已安装，默认执行重装（更新到最新版本）..."
        fi
        log_info "如需跳过，请使用: ./install.sh install --skip-existing"
    fi
    
    # 卸载旧驱动
    cmd_uninstall --quiet
    
    # 创建构建目录
    mkdir -p "${BUILD_DIR}"
    
    # 配置
    log_step "配置项目"
    "${CMAKE}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DCMAKE_MAKE_PROGRAM="${NINJA}" \
        -G Ninja \
        -S "${PROJECT_DIR}" \
        -B "${BUILD_DIR}"
    log_success "配置完成"
    
    # 构建驱动
    log_step "构建虚拟音频驱动"
    "${CMAKE}" --build "${BUILD_DIR}" --target virtual_audio_driver -j "${PARALLEL_JOBS}"
    log_success "驱动构建完成"
    
    # 构建 audioctl
    if [ "$driver_only" = false ]; then
        log_step "构建 audioctl"
        "${CMAKE}" --build "${BUILD_DIR}" --target audioctl -j "${PARALLEL_JOBS}"
        log_success "audioctl 构建完成"
    fi
    
    # 安装驱动
    log_step "安装驱动"
    sudo "${CMAKE}" --install "${BUILD_DIR}"
    sudo chown -R root:wheel "$DRIVER_PATH"
    sudo chmod -R 755 "$DRIVER_PATH"
    log_success "驱动安装完成"
    
    # 重启 CoreAudio
    log_step "重启 CoreAudio"
    sudo launchctl kickstart -k system/com.apple.audio.coreaudiod || {
        log_warn "重启失败，尝试替代方法"
        sudo killall -9 coreaudiod 2>/dev/null || true
    }
    sleep 2
    log_success "CoreAudio 已重启"
    
    # 运行测试
    if [ "$run_tests" = true ]; then
        log_step "运行单元测试"
        "${CMAKE}" --build "${BUILD_DIR}" --target test_virtual_audio_device -j "${PARALLEL_JOBS}"
        "${BUILD_DIR}/tests/test_virtual_audio_device"
    fi
    
    # 验证
    log_step "验证安装"
    if [ -d "$DRIVER_PATH" ]; then
        log_success "驱动文件存在"
    else
        log_error "驱动文件不存在"
        exit 1
    fi
    
    if [ "$driver_only" = false ] && [ -f "${BUILD_DIR}/bin/audioctl" ]; then
        log_success "audioctl 可执行文件存在"
        "${BUILD_DIR}/bin/audioctl" --version 2>/dev/null || true
    fi
    
    # 打印使用指南
    printf "\n"
    log_success "安装完成!"
    printf "\n"
    printf "${CYAN}快速开始:${NC}\n"
    printf "\n"
    printf "1. 检查虚拟设备状态:\n"
    printf "   %s/bin/audioctl virtual-status\n" "${BUILD_DIR}"
    printf "\n"
    printf "2. 切换到虚拟设备（启用应用音量控制）:\n"
    printf "   %s/bin/audioctl use-virtual\n" "${BUILD_DIR}"
    printf "\n"
    printf "3. 查看应用音量列表:\n"
    printf "   %s/bin/audioctl app-volumes\n" "${BUILD_DIR}"
    printf "\n"
    printf "4. 设置应用音量（例如 Safari 50%%）:\n"
    printf "   %s/bin/audioctl app-volume Safari 50\n" "${BUILD_DIR}"
    printf "\n"
    printf "${YELLOW}提示:${NC} 添加到 PATH:\n"
    printf "   echo 'export PATH=\"%s/bin:\$PATH\"' >> ~/.zshrc\n" "${BUILD_DIR}"
}

# ============================================
# 命令: uninstall
# ============================================

cmd_uninstall() {
    local quiet=false
    
    while [[ $# -gt 0 ]]; do
        case $1 in
            --quiet)
                quiet=true
                shift
                ;;
            *)
                break
                ;;
        esac
    done
    
    [ "$quiet" = false ] && log_step "卸载 AudioCtl"
    
    # 停止 CoreAudio
    if [ "$quiet" = false ]; then
        log_info "停止 CoreAudio..."
    fi
    sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || true
    sleep 1
    
    # 删除驱动
    if [ -d "$DRIVER_PATH" ]; then
        [ "$quiet" = false ] && log_info "删除驱动..."
        sudo rm -rf "$DRIVER_PATH"
        [ "$quiet" = false ] && log_success "驱动已删除"
    else
        [ "$quiet" = false ] && log_warn "驱动未安装"
    fi
    
    [ "$quiet" = false ] && log_success "卸载完成"
}

# ============================================
# 命令: reinstall
# ============================================

cmd_reinstall() {
    cmd_uninstall --quiet
    cmd_install "$@"
}

# ============================================
# 命令: status
# ============================================

cmd_status() {
    log_step "AudioCtl 状态"
    
    # 检查驱动
    printf "${CYAN}驱动状态:${NC}\n"
    if [ -d "$DRIVER_PATH" ]; then
        log_success "驱动已安装: ${DRIVER_PATH}"
        ls -la "$DRIVER_PATH" | head -5
    else
        log_warn "驱动未安装"
    fi
    
    # 检查可执行文件
    printf "\n"
    printf "${CYAN}可执行文件:${NC}\n"
    if [ -f "${PROJECT_DIR}/cmake-build-Debug/bin/audioctl" ]; then
        log_success "Debug 版本已构建"
        "${PROJECT_DIR}/cmake-build-Debug/bin/audioctl" --version 2>/dev/null || true
    elif [ -f "${PROJECT_DIR}/cmake-build-Release/bin/audioctl" ]; then
        log_success "Release 版本已构建"
        "${PROJECT_DIR}/cmake-build-Release/bin/audioctl" --version 2>/dev/null || true
    else
        log_warn "audioctl 未构建"
    fi
    
    # 检查虚拟设备
    printf "\n"
    printf "${CYAN}虚拟设备:${NC}\n"
    if [ -f "${PROJECT_DIR}/cmake-build-Debug/bin/audioctl" ]; then
        "${PROJECT_DIR}/cmake-build-Debug/bin/audioctl" virtual-status 2>/dev/null || {
            system_profiler SPAudioDataType 2>/dev/null | grep -A2 "Virtual Audio" || log_warn "无法检测设备状态"
        }
    else
        log_warn "请先构建 audioctl"
    fi
}

# ============================================
# 命令: test
# ============================================

cmd_test() {
    local build_type="${BUILD_TYPE}"
    
    while [[ $# -gt 0 ]]; do
        case $1 in
            --release)
                build_type="Release"
                shift
                ;;
            *)
                break
                ;;
        esac
    done
    
    local build_dir="${PROJECT_DIR}/cmake-build-${build_type}"
    
    log_step "运行单元测试"
    
    if [ ! -d "$build_dir" ]; then
        log_error "构建目录不存在: ${build_dir}"
        log_info "请先运行: ./install.sh install"
        exit 1
    fi
    
    # 构建测试
    log_info "构建测试..."
    "${CMAKE}" --build "$build_dir" --target test_virtual_audio_device
    
    # 运行测试
    log_info "运行测试..."
    "$build_dir/tests/test_virtual_audio_device"
}

# ============================================
# 命令: clean
# ============================================

cmd_clean() {
    log_step "清理构建文件"
    
    # 卸载驱动
    cmd_uninstall --quiet
    
    # 删除构建目录
    for dir in "${PROJECT_DIR}"/cmake-build-*; do
        if [ -d "$dir" ]; then
            log_info "删除: ${dir}"
            rm -rf "$dir"
        fi
    done
    
    log_success "清理完成"
}

# ============================================
# 命令: help
# ============================================

cmd_help() {
    printf "${CYAN}AudioCtl 安装脚本${NC}\n"
    printf "\n"
    printf "${GREEN}用法:${NC}\n"
    printf "    ./install.sh [命令] [选项]\n"
    printf "\n"
    printf "${GREEN}命令:${NC}\n"
    printf "    install         安装 AudioCtl（构建 + 安装驱动 + 安装工具）\n"
    printf "    uninstall       卸载驱动\n"
    printf "    reinstall       重新安装\n"
    printf "    status          查看安装状态\n"
    printf "    test            运行单元测试\n"
    printf "    clean           清理所有构建文件\n"
    printf "    help            显示此帮助信息\n"
    printf "\n"
    printf "${GREEN}install 选项:${NC}\n"
    printf "    --release          使用 Release 模式构建（默认 Debug）\n"
    printf "    -j, --jobs N       使用 N 个并行任务（默认 ${PARALLEL_JOBS}）\n"
    printf "    --test             安装后运行单元测试\n"
    printf "    --driver-only      只构建和安装驱动\n"
    printf "    --skip-existing    如果已安装则跳过（默认会重装）\n"
    printf "    --force            强制重装（与默认行为相同，显式声明）\n"
    printf "\n"
    printf "${GREEN}test 选项:${NC}\n"
    printf "    --release       测试 Release 版本（默认 Debug）\n"
    printf "\n"
    printf "${GREEN}示例:${NC}\n"
    printf "    # 安装（默认重装模式，适合更新到最新版本）\n"
    printf "    ./install.sh install\n"
    printf "\n"
    printf "    # 安装（Release 模式）\n"
    printf "    ./install.sh install --release\n"
    printf "\n"
    printf "    # 如果已安装则跳过（适合脚本中自动执行）\n"
    printf "    ./install.sh install --skip-existing\n"
    printf "\n"
    printf "    # 强制重装\n"
    printf "    ./install.sh install --force\n"
    printf "\n"
    printf "    # 安装并运行测试\n"
    printf "    ./install.sh install --test\n"
    printf "\n"
    printf "    # 只构建驱动\n"
    printf "    ./install.sh install --driver-only\n"
    printf "\n"
    printf "    # 卸载\n"
    printf "    ./install.sh uninstall\n"
    printf "\n"
    printf "    # 重新安装（等价于 install）\n"
    printf "    ./install.sh reinstall --release\n"
    printf "\n"
    printf "    # 查看状态\n"
    printf "    ./install.sh status\n"
    printf "\n"
    printf "    # 运行测试\n"
    printf "    ./install.sh test\n"
    printf "\n"
    printf "    # 清理所有\n"
    printf "    ./install.sh clean\n"
    printf "\n"
}

# ============================================
# 主入口
# ============================================

main() {
    # 显示 Banner
    printf "${CYAN}"
    printf "    _             _     _ _   _\n"
    printf "   / \\   ___  ___| |__ (_) |_| |_ ___\n"
    printf "  / _ \\ / _ \\/ __| '_ \\| | __| __/ _ \\\n"
    printf " / ___ \\  __/ (__| | | | | |_| ||  __/\n"
    printf "/_/   \\_\\___|\\___|_| |_|_|\\__|\\__\\___|\n"
    printf "${NC}\n"
    
    # 检查是否在项目根目录
    if [ ! -f "${PROJECT_DIR}/CMakeLists.txt" ]; then
        log_error "无法找到项目根目录"
        exit 1
    fi
    
    cd "$PROJECT_DIR"
    
    # 解析命令
    local command="${1:-help}"
    shift || true
    
    case "$command" in
        install)
            check_root
            detect_tools
            cmd_install "$@"
            ;;
        uninstall)
            cmd_uninstall "$@"
            ;;
        reinstall)
            check_root
            detect_tools
            cmd_reinstall "$@"
            ;;
        status)
            cmd_status
            ;;
        test)
            detect_tools
            cmd_test "$@"
            ;;
        clean)
            cmd_clean
            ;;
        help|--help|-h)
            cmd_help
            ;;
        *)
            log_error "未知命令: $command"
            echo "使用 './install.sh help' 查看帮助"
            exit 1
            ;;
    esac
}

main "$@"
