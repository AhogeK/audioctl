cmake_minimum_required(VERSION 3.10)
project(audioctl C OBJC)

# 设置 C 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 设置 Objective-C 标准
set(CMAKE_OBJC_STANDARD 11)
set(CMAKE_OBJC_STANDARD_REQUIRED ON)

# 设置最低 macOS 版本为 10.0
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.0" CACHE STRING "Minimum OS X deployment version")

# 添加项目的 include 目录
include_directories(
        "${CMAKE_SOURCE_DIR}/include"
)

# 查找 CoreAudio 框架
find_library(CORE_AUDIO_LIBRARY CoreAudio REQUIRED)
find_library(CORE_FOUNDATION_LIBRARY CoreFoundation REQUIRED)
find_library(AUDIO_TOOLBOX_LIBRARY AudioToolbox REQUIRED)
find_library(MEDIA_TOOLBOX_LIBRARY MediaToolbox REQUIRED)
find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
find_library(APPKIT_LIBRARY AppKit REQUIRED)

# 查找所有的源文件和头文件
file(GLOB_RECURSE SOURCE_FILES
        "${CMAKE_SOURCE_DIR}/src/*.c"
)

file(GLOB_RECURSE OBJC_FILES
        "${CMAKE_SOURCE_DIR}/src/*.m"
)

file(GLOB_RECURSE HEADER_FILES
        "${CMAKE_SOURCE_DIR}/include/*.h"
)

# 添加可执行文件
add_executable(audioctl ${SOURCE_FILES} ${OBJC_FILES} ${HEADER_FILES})

# 链接库和框架
target_link_libraries(audioctl PRIVATE
        ${CORE_AUDIO_LIBRARY}
        ${CORE_FOUNDATION_LIBRARY}
        ${AUDIO_TOOLBOX_LIBRARY}
        ${MEDIA_TOOLBOX_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${APPKIT_LIBRARY}  # 添加 AppKit 框架
)

# 设置 Objective-C 的编译选项（如果需要）
set_target_properties(audioctl PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 添加版本信息
set(AUDIOCTL_VERSION_MAJOR 1)
set(AUDIOCTL_VERSION_MINOR 0)
set(AUDIOCTL_VERSION_PATCH 0)
target_compile_definitions(audioctl PRIVATE
        AUDIOCTL_VERSION="${AUDIOCTL_VERSION_MAJOR}.${AUDIOCTL_VERSION_MINOR}.${AUDIOCTL_VERSION_PATCH}"
)

# 生成 compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)