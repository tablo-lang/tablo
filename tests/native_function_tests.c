#include "vm.h"
#include <stdio.h>
#include <string.h>
#include "builtins.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            printf("  PASS: %s\n", message); \
            tests_passed++; \
        } else { \
            printf("  FAIL: %s\n", message); \
            tests_failed++; \
        } \
    } while(0)

void test_builtin_registration() {
    printf("Testing builtin function registration...\n");
    
    VM vm;
    vm_init(&vm);
    
    register_builtins(&vm);
    
    Value print_val = vm_get_global(&vm, "print");
    TEST_ASSERT(value_get_type(&print_val) == VAL_NATIVE, "print is registered as native function");
    
    Value len_val = vm_get_global(&vm, "len");
    TEST_ASSERT(value_get_type(&len_val) == VAL_NATIVE, "len is registered as native function");
    
    Value typeOf_val = vm_get_global(&vm, "typeOf");
    TEST_ASSERT(value_get_type(&typeOf_val) == VAL_NATIVE, "typeOf is registered as native function");
    
    Value toInt_val = vm_get_global(&vm, "toInt");
    TEST_ASSERT(value_get_type(&toInt_val) == VAL_NATIVE, "toInt is registered as native function");
    
    Value toDouble_val = vm_get_global(&vm, "toDouble");
    TEST_ASSERT(value_get_type(&toDouble_val) == VAL_NATIVE, "toDouble is registered as native function");
    
    Value str_val = vm_get_global(&vm, "str");
    TEST_ASSERT(value_get_type(&str_val) == VAL_NATIVE, "str is registered as native function");
    
    Value push_val = vm_get_global(&vm, "push");
    TEST_ASSERT(value_get_type(&push_val) == VAL_NATIVE, "push is registered as native function");
    
    Value pop_val = vm_get_global(&vm, "pop");
    TEST_ASSERT(value_get_type(&pop_val) == VAL_NATIVE, "pop is registered as native function");
    
    Value keys_val = vm_get_global(&vm, "keys");
    TEST_ASSERT(value_get_type(&keys_val) == VAL_NATIVE, "keys is registered as native function");
    
    Value values_val = vm_get_global(&vm, "values");
    TEST_ASSERT(value_get_type(&values_val) == VAL_NATIVE, "values is registered as native function");
    
    Value read_line_val = vm_get_global(&vm, "read_line");
    TEST_ASSERT(value_get_type(&read_line_val) == VAL_NATIVE, "read_line is registered as native function");
    
    Value read_all_val = vm_get_global(&vm, "read_all");
    TEST_ASSERT(value_get_type(&read_all_val) == VAL_NATIVE, "read_all is registered as native function");
    
    Value write_line_val = vm_get_global(&vm, "write_line");
    TEST_ASSERT(value_get_type(&write_line_val) == VAL_NATIVE, "write_line is registered as native function");
    
    Value write_all_val = vm_get_global(&vm, "write_all");
    TEST_ASSERT(value_get_type(&write_all_val) == VAL_NATIVE, "write_all is registered as native function");
    
    Value exists_val = vm_get_global(&vm, "exists");
    TEST_ASSERT(value_get_type(&exists_val) == VAL_NATIVE, "exists is registered as native function");
    
    Value delete_val = vm_get_global(&vm, "delete");
    TEST_ASSERT(value_get_type(&delete_val) == VAL_NATIVE, "delete is registered as native function");
    
    vm_free(&vm);
}

void test_native_arity() {
    printf("\nTesting native function arity...\n");
    
    VM vm;
    vm_init(&vm);
    
    register_builtins(&vm);
    
    Value print_val = vm_get_global(&vm, "print");
    ObjNative* print_native = value_get_type(&print_val) == VAL_NATIVE
        ? value_get_native_obj(&print_val)
        : NULL;
    TEST_ASSERT(print_native && print_native->arity == 1, "print has arity 1");
    
    Value len_val = vm_get_global(&vm, "len");
    ObjNative* len_native = value_get_type(&len_val) == VAL_NATIVE
        ? value_get_native_obj(&len_val)
        : NULL;
    TEST_ASSERT(len_native && len_native->arity == 1, "len has arity 1");
    
    Value push_val = vm_get_global(&vm, "push");
    ObjNative* push_native = value_get_type(&push_val) == VAL_NATIVE
        ? value_get_native_obj(&push_val)
        : NULL;
    TEST_ASSERT(push_native && push_native->arity == 2, "push has arity 2");
    
    Value write_line_val = vm_get_global(&vm, "write_line");
    ObjNative* write_line_native = value_get_type(&write_line_val) == VAL_NATIVE
        ? value_get_native_obj(&write_line_val)
        : NULL;
    TEST_ASSERT(write_line_native && write_line_native->arity == 2, "write_line has arity 2");
    
    vm_free(&vm);
}

void test_native_value_initialization() {
    printf("\nTesting native value initialization...\n");
    
    VM vm;
    vm_init(&vm);
    
    register_builtins(&vm);
    
    Value len_val = vm_get_global(&vm, "len");
    ObjNative* len_native = value_get_type(&len_val) == VAL_NATIVE
        ? value_get_native_obj(&len_val)
        : NULL;
    TEST_ASSERT(len_native != NULL, "native object is not null");
    TEST_ASSERT(len_native && len_native->invoke != NULL, "native invoke callback is not null");
    TEST_ASSERT(len_native && len_native->builtin_function != NULL, "native builtin callback is not null");
    TEST_ASSERT(len_native && len_native->arity >= 0, "native arity is valid");
    TEST_ASSERT(len_native && len_native->ref_count > 0, "native has positive ref count");
    
    vm_free(&vm);
}

int main() {
    printf("=== Native Function Tests ===\n\n");
    
    test_builtin_registration();
    test_native_arity();
    test_native_value_initialization();
    
    printf("\nTest Results:\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}
