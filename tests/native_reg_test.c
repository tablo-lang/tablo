#include "vm.h"
#include <stdio.h>

int main() {
    printf("Native function registration test...\n");

    VM vm;
    vm_init(&vm);

    Value print_func = vm_get_global(&vm, "print");
    ValueType print_type = value_get_type(&print_func);

    if (print_type == VAL_NATIVE) {
        printf("PASS: Native function registered\n");
    } else {
        printf("FAIL: Native function not registered (type=%d)\n", (int)print_type);
    }

    vm_free(&vm);

    return 0;
}
