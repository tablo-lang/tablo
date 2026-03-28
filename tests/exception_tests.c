#include "vm.h"
#include "bytecode.h"
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void test_exception_handler_stack(void) {
    printf("Testing exception handler stack...\n");
    
    VM vm;
    vm_init(&vm);
    
    // Push handlers
    vm_push_exception_handler(&vm, 100);
    vm_push_exception_handler(&vm, 200);
    vm_push_exception_handler(&vm, 300);
    
    if (vm.exception_handler_count == 3) {
        tests_passed++;
        printf("  PASS: Push 3 handlers\n");
    } else {
        tests_failed++;
        printf("  FAIL: Push 3 handlers (got %d)\n", vm.exception_handler_count);
    }
    
    // Pop handlers
    ExceptionHandler handler1 = vm_pop_exception_handler(&vm);
    ExceptionHandler handler2 = vm_pop_exception_handler(&vm);
    ExceptionHandler handler3 = vm_pop_exception_handler(&vm);
    
    if (handler1.handler_ip == 300 && handler2.handler_ip == 200 && handler3.handler_ip == 100) {
        tests_passed++;
        printf("  PASS: Pop handlers in LIFO order\n");
    } else {
        tests_failed++;
        printf("  FAIL: Pop handlers in LIFO order\n");
    }
    
    // Pop from empty stack
    ExceptionHandler handler4 = vm_pop_exception_handler(&vm);
    
    if (handler4.handler_ip == -1) {
        tests_passed++;
        printf("  PASS: Pop from empty stack returns -1\n");
    } else {
        tests_failed++;
        printf("  FAIL: Pop from empty stack returns -1 (got %d)\n", handler4.handler_ip);
    }
    
    vm_free(&vm);
}

static void test_exception_value(void) {
    printf("Testing exception value...\n");
    
    VM vm;
    vm_init(&vm);
    
    if (!vm_has_exception(&vm)) {
        tests_passed++;
        printf("  PASS: No exception initially\n");
    } else {
        tests_failed++;
        printf("  FAIL: No exception initially\n");
    }
    
    Value exc_val;
    value_init_int(&exc_val, 42);
    vm_throw_exception(&vm, exc_val);
    
    if (vm_has_exception(&vm)) {
        tests_passed++;
        printf("  PASS: Exception detected after throw\n");
    } else {
        tests_failed++;
        printf("  FAIL: Exception detected after throw\n");
    }
    
    if (value_get_type(&vm.exception_value) == VAL_INT &&
        value_get_int(&vm.exception_value) == 42) {
        tests_passed++;
        printf("  PASS: Exception value preserved\n");
    } else {
        tests_failed++;
        printf("  FAIL: Exception value preserved\n");
    }
    
    vm_free(&vm);
}

static void test_exception_cleanup(void) {
    printf("Testing exception cleanup...\n");
    
    VM vm;
    vm_init(&vm);
    
    Value exc_val;
    value_init_int(&exc_val, 123);
    vm_throw_exception(&vm, exc_val);
    
    if (vm_has_exception(&vm)) {
        tests_passed++;
        printf("  PASS: Exception thrown\n");
    } else {
        tests_failed++;
        printf("  FAIL: Exception thrown\n");
    }
    
    // Simulate exception being caught
    value_init_nil(&vm.exception_value);
    vm.in_exception = false;
    
    if (!vm_has_exception(&vm)) {
        tests_passed++;
        printf("  PASS: Exception cleared after catch\n");
    } else {
        tests_failed++;
        printf("  FAIL: Exception cleared after catch\n");
    }
    
    vm_free(&vm);
}

int main(void) {
    printf("Running TabloLang Exception Tests...\n\n");
    
    test_exception_handler_stack();
    printf("\n");
    
    test_exception_value();
    printf("\n");
    
    test_exception_cleanup();
    printf("\n");
    
    printf("Exception Test Results:\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("\n");
    
    if (tests_failed == 0) {
        printf("All tests passed!\n");
    }
    
    return tests_failed > 0 ? 1 : 0;
}
