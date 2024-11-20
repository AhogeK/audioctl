cmake_minimum_required(VERSION 3.29)
project(audioctl C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.0" CACHE STRING "Minimum OS X deployment version")

# 添加项目的include目录
include_directories(
        ${CMAKE_SOURCE_DIR}/include
        /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/CoreAudio.framework/Headers
        /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/AudioToolbox.framework/Headers
)

# 查找CoreAudio框架
find_library(CORE_AUDIO_LIBRARY CoreAudio)
find_library(CORE_FOUNDATION_LIBRARY CoreFoundation)

# 查找所有的源文件和头文件
file(GLOB_RECURSE SOURCE_FILES
        "${CMAKE_SOURCE_DIR}/src/*.c"
)

file(GLOB_RECURSE HEADER_FILES
        "${CMAKE_SOURCE_DIR}/include/*.h"
)

# 添加可执行文件
add_executable(audioctl ${SOURCE_FILES} ${HEADER_FILES})

# 使用新式的target_include_directories（推荐的方式）
target_include_directories(audioctl
        PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

# 链接框架
target_link_libraries(audioctl PRIVATE
        ${CORE_AUDIO_LIBRARY}
        ${CORE_FOUNDATION_LIBRARY}
)