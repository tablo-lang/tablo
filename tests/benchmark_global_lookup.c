#include "vm.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t max_i64(int64_t a, int64_t b) {
    return a > b ? a : b;
}

static void report_ops(const char* label, int64_t iterations, clock_t start, clock_t end) {
    clock_t elapsed = end - start;
    int64_t elapsed_ms = (int64_t)((elapsed * 1000) / CLOCKS_PER_SEC);
    elapsed_ms = max_i64(elapsed_ms, 1);
    int64_t rate = (iterations * 1000) / elapsed_ms;
    printf("%s: %" PRId64 " ops in %" PRId64 " ms (%" PRId64 " ops/s)\n",
           label, iterations, elapsed_ms, rate);
}

static char** make_key_list(const char* prefix, int count) {
    char** keys = (char**)calloc((size_t)count, sizeof(char*));
    if (!keys) return NULL;
    for (int i = 0; i < count; i++) {
        char scratch[64];
        snprintf(scratch, sizeof(scratch), "%s%d", prefix, i);
        size_t len = strlen(scratch);
        keys[i] = (char*)malloc(len + 1);
        if (!keys[i]) {
            for (int j = 0; j < i; j++) free(keys[j]);
            free(keys);
            return NULL;
        }
        memcpy(keys[i], scratch, len + 1);
    }
    return keys;
}

static void free_key_list(char** keys, int count) {
    if (!keys) return;
    for (int i = 0; i < count; i++) {
        free(keys[i]);
    }
    free(keys);
}

int main(int argc, char** argv) {
    VM vm;
    vm_init(&vm);

    int scale = 1;
    if (argc > 1) {
        scale = atoi(argv[1]);
        if (scale < 1) scale = 1;
    }

    const int64_t num_iterations = (int64_t)1000000 * (int64_t)scale;
    const int num_globals = 1024;
    const int intern_key_count = 256;

    char** global_names = make_key_list("global_", num_globals);
    char** intern_keys = make_key_list("intern_", intern_key_count);
    if (!global_names || !intern_keys) {
        free_key_list(global_names, num_globals);
        free_key_list(intern_keys, intern_key_count);
        vm_free(&vm);
        fprintf(stderr, "failed to allocate benchmark key set\n");
        return 1;
    }

    for (int i = 0; i < num_globals; i++) {
        Value val;
        value_init_int(&val, i);
        vm_set_global(&vm, global_names[i], val);
    }

    clock_t start = clock();
    for (int64_t i = 0; i < num_iterations; i++) {
        int global_idx = (int)(i % num_globals);
        Value val = vm_get_global(&vm, global_names[global_idx]);
        (void)val;
    }
    clock_t end = clock();
    report_ops("perf vm globals get", num_iterations, start, end);

    start = clock();
    for (int64_t i = 0; i < num_iterations; i++) {
        int global_idx = (int)(i % num_globals);
        Value val;
        value_init_int(&val, i);
        vm_set_global(&vm, global_names[global_idx], val);
    }
    end = clock();
    report_ops("perf vm globals set", num_iterations, start, end);

    start = clock();
    for (int64_t i = 0; i < num_iterations; i++) {
        int key_idx = (int)(i % intern_key_count);
        const char* key = intern_keys[key_idx];
        ObjString* s = vm_intern_string(&vm, key, (int)strlen(key));
        obj_string_release(s);
    }
    end = clock();
    report_ops("perf vm stringPool intern", num_iterations, start, end);

    free_key_list(global_names, num_globals);
    free_key_list(intern_keys, intern_key_count);
    vm_free(&vm);
    return 0;
}
