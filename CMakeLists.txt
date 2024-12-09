cmake_minimum_required(VERSION 3.20)
project(audioctl C OBJC CXX OBJCXX)

# 基本设置
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_OBJC_STANDARD 11)
set(CMAKE_OBJC_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum OS X deployment version")

# 编译选项
option(DEBUG_MODE "Enable debug mode" ON)
add_compile_options(-Wall -Wextra)

if (DEBUG_MODE)
    add_compile_definitions(DEBUG)
    add_compile_options(-g -O0)
    message(STATUS "Building in DEBUG mode")
else ()
    add_compile_definitions(-O2)
    message(STATUS "Building in RELEASE mode")
endif ()

# 查找必要的框架
find_library(CORE_AUDIO_LIBRARY CoreAudio REQUIRED)
find_library(CORE_FOUNDATION_LIBRARY CoreFoundation REQUIRED)
find_library(AUDIO_TOOLBOX_LIBRARY AudioToolbox REQUIRED)
find_library(MEDIA_TOOLBOX_LIBRARY MediaToolbox REQUIRED)
find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
find_library(APPKIT_LIBRARY AppKit REQUIRED)

# 添加项目的 include 目录
include_directories(
        "${CMAKE_SOURCE_DIR}/include"
)

# 查找源文件
file(GLOB_RECURSE SOURCE_FILES
        "${CMAKE_SOURCE_DIR}/src/*.c"
        "${CMAKE_SOURCE_DIR}/src/*.m"
)

file(GLOB_RECURSE HEADER_FILES
        "${CMAKE_SOURCE_DIR}/include/*.h"
)

# 添加主可执行文件
add_executable(audioctl ${SOURCE_FILES} ${HEADER_FILES})

# 链接主程序需要的框架
target_link_libraries(audioctl PRIVATE
        ${CORE_AUDIO_LIBRARY}
        ${CORE_FOUNDATION_LIBRARY}
        ${AUDIO_TOOLBOX_LIBRARY}
        ${MEDIA_TOOLBOX_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${APPKIT_LIBRARY}
)

# 设置主程序属性
set_target_properties(audioctl PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 生成 compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 添加驱动程序子目录
add_subdirectory(driver)