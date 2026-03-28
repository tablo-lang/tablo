#ifndef SAFE_ALLOC_H
#define SAFE_ALLOC_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>

typedef struct SafeAllocJmpContext {
    jmp_buf* env;
    char* message_buffer;
    size_t message_buffer_size;
    struct SafeAllocJmpContext* prev;
} SafeAllocJmpContext;

void safe_alloc_push_jmp_context(SafeAllocJmpContext* context,
                                 jmp_buf* env,
                                 char* message_buffer,
                                 size_t message_buffer_size);
void safe_alloc_pop_jmp_context(SafeAllocJmpContext* context);
void safe_alloc_set_fail_after(size_t successful_allocations_before_failure);
void safe_alloc_clear_fail_after(void);
bool safe_alloc_failpoint_is_active(void);

void* safe_malloc_impl(size_t size, const char* file, int line);
void* safe_calloc_impl(size_t count, size_t size, const char* file, int line);
void* safe_realloc_impl(void* ptr, size_t size, const char* file, int line);
char* safe_strdup_impl(const char* s, const char* file, int line);

#define safe_malloc(size) safe_malloc_impl(size, __FILE__, __LINE__)
#define safe_calloc(count, size) safe_calloc_impl(count, size, __FILE__, __LINE__)
#define safe_realloc(ptr, size) safe_realloc_impl(ptr, size, __FILE__, __LINE__)
#define safe_strdup(s) safe_strdup_impl(s, __FILE__, __LINE__)

static inline bool safe_mul(size_t a, size_t b, size_t* result) {
    if (a == 0 || b == 0) {
        *result = 0;
        return true;
    }
    if (a > SIZE_MAX / b) {
        return false;
    }
    *result = a * b;
    return true;
}

#endif
