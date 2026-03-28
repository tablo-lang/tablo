#include "vm.h"
#include <stdio.h>

int main() {
    VM vm;
    vm_init(&vm);

    Value val;
    value_init_int(&val, 5);
    vm_set_global(&vm, "x", val);

    Value result = vm_get_global(&vm, "x");

    if (value_get_type(&result) == VAL_INT && value_get_int(&result) == 5) {
        printf("SUCCESS: Global lookup works\n");
    } else {
        printf("FAILURE: Global lookup failed\n");
    }

    vm_free(&vm);
    return 0;
}
