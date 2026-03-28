#include "vm.h"
#include "builtins.h"
#include <stdio.h>

int main() {
    printf("Simple native function test...\n");
    fflush(stdout);

    VM vm;
    vm_init(&vm);
    printf("VM initialized\n");
    fflush(stdout);

    register_builtins(&vm);
    printf("Builtins registered\n");
    fflush(stdout);

    Value print_func = vm_get_global(&vm, "print");
    ValueType print_type = value_get_type(&print_func);
    printf("Got print function, type=%d\n", (int)print_type);
    fflush(stdout);

    Value len_func = vm_get_global(&vm, "len");
    ValueType len_type = value_get_type(&len_func);
    printf("Got len function, type=%d\n", (int)len_type);
    fflush(stdout);

    ObjNative* print_native = print_type == VAL_NATIVE ? value_get_native_obj(&print_func) : NULL;
    ObjNative* len_native = len_type == VAL_NATIVE ? value_get_native_obj(&len_func) : NULL;
    printf("Print func: %p\n", (void*)print_native);
    printf("Len func: %p\n", (void*)len_native);
    fflush(stdout);

    vm_free(&vm);

    printf("VM freed\n");
    fflush(stdout);

    return 0;
}
