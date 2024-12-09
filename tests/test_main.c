//
// Created by AhogeK on 12/9/24.
//

#include <stdio.h>

extern int run_basic_device_tests(void);

extern int run_device_control_tests(void);

extern int run_audio_control_tests(void);

int main() {
    printf("Starting virtual audio device tests...\n");
    int failed = 0;

    failed += run_basic_device_tests();
    failed += run_device_control_tests();
    failed += run_audio_control_tests();

    printf("\nTest summary: ");
    if (failed == 0) {
        printf("All tests PASSED!\n");
        return 0;
    } else {
        printf("%d tests FAILED!\n", failed);
        return 1;
    }
}