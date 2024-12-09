//
// Created by AhogeK on 12/9/24.
//

#include <stdio.h>

// 声明测试函数
extern int run_basic_device_tests(void);

extern int run_device_control_tests(void);

int main() {
    printf("Starting virtual audio device tests...\n");
    int failed = 0;

    // 运行基本设备测试
    failed += run_basic_device_tests();

    // 运行设备控制测试
    failed += run_device_control_tests();

    printf("\nTest summary: ");
    if (failed == 0) {
        printf("All tests PASSED!\n");
        return 0;
    } else {
        printf("%d tests FAILED!\n", failed);
        return 1;
    }
}