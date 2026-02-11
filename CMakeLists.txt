cmake_minimum_required(VERSION 3.20)
project(audioctl VERSION 1.0.0 LANGUAGES C OBJC)
enable_testing()

# 基本设置
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_OBJC_STANDARD 11)
set(CMAKE_OBJC_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum OS X deployment version")

# 编译选项
option(DEBUG_MODE "Enable debug mode" ON)
add_compile_options(-Wall -Wextra)

# 定义版本号
add_compile_definitions(AUDIOCTL_VERSION="${PROJECT_VERSION}")

if (DEBUG_MODE)
    add_compile_definitions(DEBUG)
    add_compile_options(-g -O0)
    message(STATUS "Building in DEBUG mode")
else ()
    add_compile_options(-O2)
    message(STATUS "Building in RELEASE mode")
endif ()

# 查找必要的框架
find_library(CORE_AUDIO_LIBRARY CoreAudio REQUIRED)
find_library(CORE_FOUNDATION_LIBRARY CoreFoundation REQUIRED)
find_library(AUDIO_TOOLBOX_LIBRARY AudioToolbox REQUIRED)
find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
find_library(APPKIT_LIBRARY AppKit REQUIRED)

# 添加项目的 include 目录
include_directories(
        "${CMAKE_SOURCE_DIR}/include"
)

# 设置驱动插件输出目录和名称
set(PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/plugins")
set(DRIVER_OUTPUT_NAME "VirtualAudioDriver")

# ============================================================================
# IPC 静态库 (audioctl_ipc)
# ============================================================================
# 用于驱动插件和主程序之间的通信
set(IPC_SOURCES
        "${CMAKE_SOURCE_DIR}/src/ipc/ipc_protocol.c"
)

set(IPC_HEADERS
        "${CMAKE_SOURCE_DIR}/include/ipc/ipc_protocol.h"
)

add_library(audioctl_ipc STATIC ${IPC_SOURCES} ${IPC_HEADERS})

# 设置 IPC 库属性
set_target_properties(audioctl_ipc PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON
        OSX_ARCHITECTURES "arm64;arm64e"
)

target_include_directories(audioctl_ipc PUBLIC
        "${CMAKE_SOURCE_DIR}/include"
)

# 定义资源文件
set(LOCALIZED_RESOURCES
        "${CMAKE_SOURCE_DIR}/en.lproj/Localizable.strings"
)

# 检查图标文件
set(ICON_FILE "${CMAKE_SOURCE_DIR}/resource/DeviceIcon.icns")
if (EXISTS ${ICON_FILE})
    set(RESOURCE_FILES ${ICON_FILE})
endif ()

# 添加虚拟音频驱动插件
add_library(virtual_audio_driver MODULE
        "${CMAKE_SOURCE_DIR}/src/driver/virtual_audio_driver.c"
        "${CMAKE_SOURCE_DIR}/src/driver/app_volume_driver.c"
)

# 添加资源文件到目标
target_sources(virtual_audio_driver PRIVATE
        ${LOCALIZED_RESOURCES}
        ${RESOURCE_FILES}
)

# 设置资源文件的安装位置
set_source_files_properties(
        ${LOCALIZED_RESOURCES}
        PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources/en.lproj"
)

if (EXISTS ${ICON_FILE})
    set_source_files_properties(
            ${ICON_FILE}
            PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources"
    )
endif ()

# 设置驱动插件属性
set_target_properties(virtual_audio_driver PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "driver"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/resource/Info.plist"
        # 强制指定关键元数据，防止 CMake 自动生成默认值覆盖 Info.plist
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.ahogek.VirtualAudioDriver"
        MACOSX_BUNDLE_BUNDLE_NAME "VirtualAudioDriver"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        OUTPUT_NAME "${DRIVER_OUTPUT_NAME}"
        # [修改：仅保留符号导出，移除 sectcreate 避免双写元数据]
        LINK_FLAGS "-Wl,-exported_symbol,_AudioServerPlugIn_Initialize"
        # [修改：构建 arm64e 架构以支持指针认证，符合现代 macOS HAL 插件要求]
        OSX_ARCHITECTURES "arm64e"
)

# 链接驱动插件需要的框架和库
target_link_libraries(virtual_audio_driver PRIVATE
        ${CORE_AUDIO_LIBRARY}
        ${CORE_FOUNDATION_LIBRARY}
        audioctl_ipc
)

# 查找源文件（排除驱动相关文件）
file(GLOB_RECURSE SOURCE_FILES
        "${CMAKE_SOURCE_DIR}/src/*.c"
        "${CMAKE_SOURCE_DIR}/src/*.m"
)

file(GLOB_RECURSE HEADER_FILES
        "${CMAKE_SOURCE_DIR}/include/*.h"
)

# 从主程序源文件中排除驱动相关源文件（驱动是单独编译的）
# 同时排除 IPC 源文件（已在 audioctl_ipc 静态库中）
list(REMOVE_ITEM SOURCE_FILES
        "${CMAKE_SOURCE_DIR}/src/driver/virtual_audio_driver.c"
        "${CMAKE_SOURCE_DIR}/src/driver/app_volume_driver.c"
        "${CMAKE_SOURCE_DIR}/src/ipc/ipc_protocol.c"
)

# 添加主可执行文件
add_executable(audioctl ${SOURCE_FILES} ${HEADER_FILES})

# 链接主程序需要的框架和库
target_link_libraries(audioctl PRIVATE
        ${CORE_AUDIO_LIBRARY}
        ${CORE_FOUNDATION_LIBRARY}
        ${AUDIO_TOOLBOX_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${APPKIT_LIBRARY}
        pthread
        audioctl_ipc
)

# 链接 Aggregate Device 需要的 AudioUnit 框架
find_library(AUDIO_UNIT_LIBRARY AudioUnit)
if (AUDIO_UNIT_LIBRARY)
    target_link_libraries(audioctl PRIVATE ${AUDIO_UNIT_LIBRARY})
endif ()

# 设置主程序属性
set_target_properties(audioctl PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 安装虚拟驱动
install(TARGETS virtual_audio_driver
        LIBRARY DESTINATION "/Library/Audio/Plug-Ins/HAL"
        BUNDLE DESTINATION "/Library/Audio/Plug-Ins/HAL"
)

# ⚠️ 注意：不在这里自动重启 CoreAudio，因为这会中断用户的音频会话
# 请使用 install.sh 脚本进行安装，它会提供详细的警告和确认提示
# 或手动重启：sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
# install(CODE "execute_process(COMMAND sudo launchctl kickstart -k system/com.apple.audio.coreaudiod)")

# 生成 compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(tests)