# 添加测试可执行文件
add_executable(test_virtual_audio_device
        test_main.c
        test_virtual_audio_device.c
        test_virtual_device_control.c
        test_audio_control.c
        test_audio_processing.c
        test_device_state.c
        # 新增测试文件
        test_app_volume_control.c
        test_virtual_device_manager.c
        test_aggregate_device_manager.c
        test_ipc_protocol.c
)

# 链接需要测试的源文件
target_sources(test_virtual_audio_device PRIVATE
        ${CMAKE_SOURCE_DIR}/src/driver/virtual_audio_device.c
        ${CMAKE_SOURCE_DIR}/src/app_volume_control.c
        ${CMAKE_SOURCE_DIR}/src/virtual_device_manager.c
        ${CMAKE_SOURCE_DIR}/src/aggregate_device_manager.c
        ${CMAKE_SOURCE_DIR}/src/aggregate_volume_proxy.c
        ${CMAKE_SOURCE_DIR}/src/ipc/ipc_protocol.c
        ${CMAKE_SOURCE_DIR}/src/ipc/ipc_server.c
        ${CMAKE_SOURCE_DIR}/src/constants.c
)

# 添加包含目录
target_include_directories(test_virtual_audio_device PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

# 链接需要的框架
target_link_libraries(test_virtual_audio_device PRIVATE
        ${CORE_AUDIO_LIBRARY}
        ${CORE_FOUNDATION_LIBRARY}
        ${AUDIO_TOOLBOX_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${APPKIT_LIBRARY}
        pthread
)

# 添加测试
add_test(
        NAME test_virtual_audio_device
        COMMAND test_virtual_audio_device
)