//
// Created by AhogeK on 12/9/24.
//

#include <stdio.h>

extern int run_basic_device_tests(void);
extern int run_device_control_tests(void);
extern int run_audio_control_tests(void);
extern int run_audio_processing_tests(void);
extern int run_device_state_tests(void);

// 新增测试
extern int run_app_volume_control_tests(void);
extern int run_virtual_device_manager_tests(void);
extern int run_aggregate_device_manager_tests(void);
extern int run_ipc_protocol_tests(void);
extern int run_ipc_client_tests(void);

int main()
{
    printf("========================================\n");
    printf("    AudioCtl Test Suite\n");
    printf("========================================\n");
    int failed = 0;

    // 原有测试
    failed += run_basic_device_tests();
    failed += run_device_control_tests();
    failed += run_audio_control_tests();
    failed += run_audio_processing_tests();
    failed += run_device_state_tests();

    // 新增测试
    failed += run_app_volume_control_tests();
    failed += run_virtual_device_manager_tests();
    failed += run_aggregate_device_manager_tests();
    failed += run_ipc_protocol_tests();
    failed += run_ipc_client_tests();

    printf("\n========================================\n");
    printf("Test summary: ");
    if (failed == 0)
    {
        printf("All tests PASSED! ✅\n");
        printf("========================================\n");
        return 0;
    }
    else
    {
        printf("%d tests FAILED! ❌\n", failed);
        printf("========================================\n");
        return 1;
    }
}
