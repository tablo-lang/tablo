#include "vm.h"
#include "lexer.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int memory_tests_passed = 0;
static int memory_tests_failed = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            printf("  PASS: %s\n", message); \
            memory_tests_passed++; \
        } else { \
            printf("  FAIL: %s\n", message); \
            memory_tests_failed++; \
        } \
    } while(0)

void test_null_pointer_safety() {
    printf("Testing null pointer safety...\n");

    Value* null_val = NULL;
    TEST_ASSERT(value_is_nil(null_val), "value_is_nil handles NULL");
    TEST_ASSERT(!value_is_true(null_val), "value_is_true handles NULL");
    TEST_ASSERT(!value_equals(null_val, null_val), "value_equals handles NULL");

    Value val;
    value_init_nil(&val);
    TEST_ASSERT(value_is_nil(&val), "value_is_nil works with non-NULL");
}

void test_array_bounds() {
    printf("Testing array bounds checking...\n");

    VM vm;
    vm_init(&vm);

    ObjArray* arr = obj_array_create(&vm, 4);
    TEST_ASSERT(arr != NULL, "Array creation succeeds");

    Value val1, val2, val3;
    value_init_int(&val1, 1);
    value_init_int(&val2, 2);
    value_init_int(&val3, 3);

    obj_array_push(arr, val1);
    obj_array_push(arr, val2);
    obj_array_push(arr, val3);

    TEST_ASSERT(arr->count == 3, "Array count is correct");

    Value result;
    obj_array_get(arr, 0, &result);
    TEST_ASSERT(value_get_type(&result) == VAL_INT && value_get_int(&result) == 1, "Get valid index 0 works");

    obj_array_get(arr, 2, &result);
    TEST_ASSERT(value_get_type(&result) == VAL_INT && value_get_int(&result) == 3, "Get valid index 2 works");

    obj_array_get(arr, -1, &result);
    TEST_ASSERT(value_get_type(&result) == VAL_NIL, "Get index -1 returns nil");

    obj_array_get(arr, 100, &result);
    TEST_ASSERT(value_get_type(&result) == VAL_NIL, "Get out-of-bounds index returns nil");

    obj_array_free(arr);
    vm_free(&vm);
}

void test_array_pop() {
    printf("Testing array pop...\n");

    VM vm;
    vm_init(&vm);

    ObjArray* arr = obj_array_create(&vm, 4);

    Value val1, val2, val3;
    value_init_int(&val1, 1);
    value_init_int(&val2, 2);
    value_init_int(&val3, 3);

    obj_array_push(arr, val1);
    obj_array_push(arr, val2);
    obj_array_push(arr, val3);

    Value popped;
    obj_array_pop(arr, &popped);
    TEST_ASSERT(value_get_type(&popped) == VAL_INT && value_get_int(&popped) == 3, "Pop returns last element");
    TEST_ASSERT(arr->count == 2, "Array count decreases after pop");

    obj_array_pop(arr, &popped);
    obj_array_pop(arr, &popped);

    obj_array_pop(arr, &popped);
    TEST_ASSERT(value_get_type(&popped) == VAL_NIL, "Pop from empty array returns nil");

    obj_array_free(arr);
    vm_free(&vm);
}

void test_cycle_collection() {
    printf("Testing cycle collection...\n");

    VM vm;
    vm_init(&vm);

    ObjArray* arr = obj_array_create(&vm, 4);
    TEST_ASSERT(arr != NULL, "Array creation succeeds (cycle test)");

    Value self;
    value_init_array(&self, arr);

    // Move the only external reference into the array itself, creating an unreachable cycle.
    obj_array_push(arr, self);
    value_init_nil(&self);

    int reclaimed = vm_gc_collect(&vm);
    TEST_ASSERT(reclaimed >= 1, "Cycle collector reclaims unreachable cyclic array");
    TEST_ASSERT(vm_gc_tracked_count(&vm) == 0, "No tracked objects remain after collection");

    vm_free(&vm);
}

void test_value_equality() {
    printf("Testing value equality...\n");

    Value a, b;
    value_init_int(&a, 42);
    value_init_int(&b, 42);

    TEST_ASSERT(value_equals(&a, &b), "Equal integers compare equal");

    value_init_int(&a, 42);
    value_init_int(&b, 43);
    TEST_ASSERT(!value_equals(&a, &b), "Different integers compare not equal");

    value_init_int(&a, 42);
    value_init_nil(&b);
    TEST_ASSERT(!value_equals(&a, &b), "Integer and nil compare not equal");

    value_init_nil(&a);
    value_init_nil(&b);
    TEST_ASSERT(value_equals(&a, &b), "Two nil values compare equal");
}

void test_value_accessors() {
    printf("Testing value accessor helpers...\n");

    Value v;

#ifdef TABLO_NAN_BOXING
    TEST_ASSERT(sizeof(Value) == sizeof(uint64_t), "NaN-boxed Value uses 8-byte storage");
#else
    TEST_ASSERT(sizeof(Value) >= 16, "Legacy Value uses tagged struct storage");
#endif

    value_init_int(&v, 42);
    TEST_ASSERT(value_get_type(&v) == VAL_INT, "value_get_type returns int");
    TEST_ASSERT(value_get_int(&v) == 42, "value_get_int returns int payload");

    value_init_bool(&v, true);
    TEST_ASSERT(value_get_type(&v) == VAL_BOOL, "value_get_type returns bool");
    TEST_ASSERT(value_get_bool(&v), "value_get_bool returns true");

    value_init_double(&v, 3.5);
    TEST_ASSERT(value_get_type(&v) == VAL_DOUBLE, "value_get_type returns double");
    TEST_ASSERT(value_get_double(&v) == 3.5, "value_get_double returns double payload");

    value_init_string(&v, "hello");
    TEST_ASSERT(value_get_type(&v) == VAL_STRING, "value_get_type returns string");
    TEST_ASSERT(value_get_string_obj(&v) != NULL, "value_get_string_obj returns pointer");
    TEST_ASSERT(strcmp(value_get_string_obj(&v)->chars, "hello") == 0, "value_get_string_obj content matches");
    value_free(&v);
}

void test_string_operations() {
    printf("Testing string operations...\n");

    Value str1, str2;
    value_init_string(&str1, "hello");
    value_init_string(&str2, "world");

    TEST_ASSERT(value_get_string_obj(&str1) != NULL, "String creation succeeds");
    TEST_ASSERT(value_get_string_obj(&str1)->length == 5, "String length is correct");

    TEST_ASSERT(strcmp(value_get_string_obj(&str1)->chars, "hello") == 0, "String content is correct");

    Value str3;
    value_init_string(&str3, "hello");
    TEST_ASSERT(value_equals(&str1, &str3), "Equal strings compare equal");
    TEST_ASSERT(!value_equals(&str1, &str2), "Different strings compare not equal");

    value_free(&str1);
    value_free(&str2);
    value_free(&str3);
}

void test_memory_leaks() {
    printf("Testing for memory leaks...\n");

    VM vm;
    vm_init(&vm);

    for (int i = 0; i < 1000; i++) {
        Value val;
        value_init_int(&val, i);

        ObjArray* arr = obj_array_create(&vm, 10);
        for (int j = 0; j < 10; j++) {
            Value elem;
            value_init_int(&elem, j);
            obj_array_push(arr, elem);
        }

        ObjString* str = obj_string_create("test", 4);

        obj_string_release(str);
        obj_array_free(arr);
    }

    vm_free(&vm);
    TEST_ASSERT(true, "Large number of allocations/frees completes without crash");
}

int main(void) {
    printf("Running TabloLang Memory Safety Tests...\n\n");

    test_null_pointer_safety();
    test_array_bounds();
    test_array_pop();
    test_cycle_collection();
    test_value_equality();
    test_value_accessors();
    test_string_operations();
    test_memory_leaks();

    printf("\nMemory Safety Test Results:\n");
    printf("  Passed: %d\n", memory_tests_passed);
    printf("  Failed: %d\n", memory_tests_failed);

    return memory_tests_failed > 0 ?1 : 0;
}
