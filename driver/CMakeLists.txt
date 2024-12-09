project(virtual_audio_driver)

# 设置 DriverKit SDK 路径
set(DRIVER_KIT_SDK_PATH1 "/System/Volumes/Data/Applications/Xcode.app/Contents/Developer/Platforms/DriverKit.platform/Developer/SDKs/DriverKit.sdk")
set(DRIVER_KIT_SDK_PATH2 "/Applications/Xcode.app/Contents/Developer/Platforms/DriverKit.platform/Developer/SDKs/DriverKit.sdk")

if (EXISTS "${DRIVER_KIT_SDK_PATH1}")
    set(DRIVER_KIT_SDK_PATH "${DRIVER_KIT_SDK_PATH1}")
    message(STATUS "Using DriverKit SDK Path: ${DRIVER_KIT_SDK_PATH1}")
elseif (EXISTS "${DRIVER_KIT_SDK_PATH2}")
    set(DRIVER_KIT_SDK_PATH "${DRIVER_KIT_SDK_PATH2}")
    message(STATUS "Using DriverKit SDK Path: ${DRIVER_KIT_SDK_PATH2}")
else ()
    message(FATAL_ERROR "DriverKit.sdk not found")
endif ()

set(DRIVER_KIT_FRAMEWORK_PATH "${DRIVER_KIT_SDK_PATH}/System/DriverKit/System/Library/Frameworks")

# 查找 DriverKit 框架
find_library(AUDIO_DRIVER_KIT_FRAMEWORK AudioDriverKit
        PATHS "${DRIVER_KIT_FRAMEWORK_PATH}"
        REQUIRED
        FRAMEWORK
)

find_library(DRIVER_KIT_FRAMEWORK DriverKit
        PATHS "${DRIVER_KIT_FRAMEWORK_PATH}"
        REQUIRED
        FRAMEWORK
)

# 查找驱动程序源文件
file(GLOB_RECURSE DRIVER_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.mm"
)

file(GLOB_RECURSE DRIVER_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
)

# 添加驱动程序目标
add_library(virtual_audio_driver MODULE
        ${DRIVER_SOURCES}
        ${DRIVER_HEADERS}
)

# 设置驱动程序的包含目录
target_include_directories(virtual_audio_driver PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        "${DRIVER_KIT_FRAMEWORK_PATH}"
)

# 设置驱动程序特定的编译选项
target_compile_options(virtual_audio_driver PRIVATE
        -fapple-kext
        -fno-rtti
        -fno-exceptions
)

# 设置驱动程序的链接选项
target_link_libraries(virtual_audio_driver PRIVATE
        "${AUDIO_DRIVER_KIT_FRAMEWORK}"
        "${DRIVER_KIT_FRAMEWORK}"
)

# 设置驱动程序属性
set_target_properties(virtual_audio_driver PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "driver"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist
)

# 设置驱动程序的安装路径（如果需要）
install(TARGETS virtual_audio_driver
        LIBRARY DESTINATION /Library/Drivers
        BUNDLE DESTINATION /Library/Drivers
)