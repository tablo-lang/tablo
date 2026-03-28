#include "safe_alloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>

static _Thread_local SafeAllocJmpContext* g_safe_alloc_jmp_context = NULL;
static _Thread_local bool g_safe_alloc_failpoint_enabled = false;
static _Thread_local size_t g_safe_alloc_failpoint_remaining = 0;

void safe_alloc_push_jmp_context(SafeAllocJmpContext* context,
                                 jmp_buf* env,
                                 char* message_buffer,
                                 size_t message_buffer_size) {
    if (!context || !env) return;
    context->env = env;
    context->message_buffer = message_buffer;
    context->message_buffer_size = message_buffer_size;
    context->prev = g_safe_alloc_jmp_context;
    g_safe_alloc_jmp_context = context;
    if (message_buffer && message_buffer_size > 0) {
        message_buffer[0] = '\0';
    }
}

void safe_alloc_pop_jmp_context(SafeAllocJmpContext* context) {
    if (!context) return;

    if (g_safe_alloc_jmp_context == context) {
        g_safe_alloc_jmp_context = context->prev;
    } else {
        SafeAllocJmpContext* node = g_safe_alloc_jmp_context;
        while (node && node->prev != context) {
            node = node->prev;
        }
        if (node && node->prev == context) {
            node->prev = context->prev;
        }
    }

    context->env = NULL;
    context->message_buffer = NULL;
    context->message_buffer_size = 0;
    context->prev = NULL;
}

void safe_alloc_set_fail_after(size_t successful_allocations_before_failure) {
    g_safe_alloc_failpoint_enabled = true;
    g_safe_alloc_failpoint_remaining = successful_allocations_before_failure;
}

void safe_alloc_clear_fail_after(void) {
    g_safe_alloc_failpoint_enabled = false;
    g_safe_alloc_failpoint_remaining = 0;
}

bool safe_alloc_failpoint_is_active(void) {
    return g_safe_alloc_failpoint_enabled;
}

static void safe_alloc_fail(const char* action,
                            size_t size_a,
                            size_t size_b,
                            const char* file,
                            int line) {
    char message[256];
    if (size_b > 0) {
        snprintf(message,
                 sizeof(message),
                 "Out of memory (%s failed, requested %zu * %zu bytes at %s:%d)",
                 action ? action : "allocation",
                 size_a,
                 size_b,
                 file ? file : "<unknown>",
                 line);
    } else {
        snprintf(message,
                 sizeof(message),
                 "Out of memory (%s failed, requested %zu bytes at %s:%d)",
                 action ? action : "allocation",
                 size_a,
                 file ? file : "<unknown>",
                 line);
    }

    if (g_safe_alloc_jmp_context && g_safe_alloc_jmp_context->env) {
        if (g_safe_alloc_jmp_context->message_buffer && g_safe_alloc_jmp_context->message_buffer_size > 0) {
            snprintf(g_safe_alloc_jmp_context->message_buffer,
                     g_safe_alloc_jmp_context->message_buffer_size,
                     "%s",
                     message);
        }
        longjmp(*(g_safe_alloc_jmp_context->env), 1);
    }

    fprintf(stderr, "Fatal: %s\n", message);
    abort();
}

static bool safe_alloc_should_fail(void) {
    if (!g_safe_alloc_failpoint_enabled) {
        return false;
    }
    if (g_safe_alloc_failpoint_remaining == 0) {
        return true;
    }
    g_safe_alloc_failpoint_remaining--;
    return false;
}

void* safe_malloc_impl(size_t size, const char* file, int line) {
    if (size == 0) {
        return malloc(0);
    }
    size_t requested = size;
    if (safe_alloc_should_fail()) {
        safe_alloc_fail("malloc", requested, 0, file, line);
    }
    void* ptr = malloc(requested);
    if (!ptr) {
        safe_alloc_fail("malloc", requested, 0, file, line);
    }
    return ptr;
}

void* safe_calloc_impl(size_t count, size_t size, const char* file, int line) {
    if (count == 0 || size == 0) {
        return calloc(count, size);
    }
    size_t request_count = count;
    size_t request_size = size;
    if (safe_alloc_should_fail()) {
        safe_alloc_fail("calloc", request_count, request_size, file, line);
    }
    void* ptr = calloc(request_count, request_size);
    if (!ptr) {
        safe_alloc_fail("calloc", request_count, request_size, file, line);
    }
    return ptr;
}

void* safe_realloc_impl(void* ptr, size_t size, const char* file, int line) {
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    size_t requested = size;
    if (safe_alloc_should_fail()) {
        safe_alloc_fail("realloc", requested, 0, file, line);
    }
    void* new_ptr = realloc(ptr, requested);
    if (!new_ptr) {
        safe_alloc_fail("realloc", requested, 0, file, line);
    }
    return new_ptr;
}

char* safe_strdup_impl(const char* s, const char* file, int line) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* dup = (char*)safe_malloc_impl(len, file, line);
    memcpy(dup, s, len);
    return dup;
}
