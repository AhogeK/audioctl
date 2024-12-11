#!/bin/bash

# 检查是否以 root 权限运行
if [ "$EUID" -ne 0 ]; then
    echo "请使用 sudo 运行此脚本"
    exit 1
fi

# 确认当前目录是项目根目录
if [ ! -f "CMakeLists.txt" ]; then
    echo "当前目录不是项目根目录，请在项目根目录下运行此脚本"
    exit 1
fi

# 默认配置
BUILD_TYPE="Debug"
PARALLEL_JOBS=10
DRIVER_NAME="VirtualAudioDriver"
DRIVER_PATH="/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}.driver"
BUILD_AUDIOCTL=false

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        -j|--jobs)
            if [[ $2 =~ ^[0-9]+$ ]]; then
                PARALLEL_JOBS="$2"
                shift
            else
                echo "无效的并行任务数: $2"
                exit 1
            fi
            shift
            ;;
        --with-audioctl)
            BUILD_AUDIOCTL=true
            shift
            ;;
        *)
            echo "未知参数: $1"
            exit 1
            ;;
    esac
done

# 项目根目录
PROJECT_DIR=$(pwd)
# 构建目录
BUILD_DIR="${PROJECT_DIR}/cmake-build-${BUILD_TYPE}"

# 使用系统中的 cmake 和 ninja
CMAKE=$(which cmake)
NINJA=$(which ninja)

function check_tools() {
    if [ -z "$CMAKE" ]; then
        echo "找不到 cmake，请确保已安装并在 PATH 中"
        exit 1
    fi

    if [ -z "$NINJA" ]; then
        echo "找不到 ninja，请确保已安装并在 PATH 中"
        exit 1
    fi
}

function uninstall_old_driver() {
    if [ -d "$DRIVER_PATH" ]; then
        echo "发现已安装的驱动，正在卸载..."
        if ! rm -rf "$DRIVER_PATH"; then
            echo "卸载旧驱动失败"
            exit 1
        fi
        echo "旧驱动已卸载"
    fi
}

function configure_and_build() {
    echo "使用构建类型: ${BUILD_TYPE}"
    echo "并行任务数: ${PARALLEL_JOBS}"
    echo "构建目录: ${BUILD_DIR}"

    if ! mkdir -p "${BUILD_DIR}"; then
        echo "创建构建目录失败"
        exit 1
    fi

    echo "清理构建目录..."
    if ! "$CMAKE" --build "${BUILD_DIR}" --target clean 2>/dev/null; then
        echo "注意：清理构建目录失败，可能是首次构建"
    fi

    echo "配置项目..."
    if ! "$CMAKE" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DCMAKE_MAKE_PROGRAM="${NINJA}" \
        -G Ninja \
        -S "${PROJECT_DIR}" \
        -B "${BUILD_DIR}"; then
        echo "CMake 配置失败"
        exit 1
    fi

    echo "构建驱动..."
    if ! "$CMAKE" --build "${BUILD_DIR}" --target virtual_audio_driver -j "${PARALLEL_JOBS}"; then
        echo "构建驱动失败"
        exit 1
    fi

    echo "安装驱动..."
    if ! "$CMAKE" --install "${BUILD_DIR}"; then
        echo "安装驱动失败"
        exit 1
    fi

    if [ "$BUILD_AUDIOCTL" = true ]; then
        echo "构建 audioctl..."
        if ! "$CMAKE" --build "${BUILD_DIR}" --target audioctl -j "${PARALLEL_JOBS}"; then
            echo "构建 audioctl 失败"
            exit 1
        fi
    fi
}

function restart_coreaudio() {
    echo "重启 CoreAudio 服务..."
    if ! launchctl kickstart -k system/com.apple.audio.coreaudiod; then
        echo "重启 CoreAudio 服务失败"
        exit 1
    fi
}

function verify_installation() {
    echo "验证驱动安装..."
    if [ ! -d "$DRIVER_PATH" ]; then
        echo "驱动安装失败：找不到驱动文件"
        exit 1
    fi

    if [ ! -r "$DRIVER_PATH" ] || [ ! -x "$DRIVER_PATH" ]; then
        echo "驱动安装失败：权限不正确"
        exit 1
    fi

    echo "驱动安装验证完成"
    echo "驱动和程序安装或更新完成"
}

check_tools
uninstall_old_driver
configure_and_build
restart_coreaudio
verify_installation