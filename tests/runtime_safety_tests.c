#include "runtime.h"
#include "artifact.h"
#include "bytecode.h"
#include "safe_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <setjmp.h>

#ifdef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

static int tests_passed = 0;
static int tests_failed = 0;
static unsigned long long temp_path_counter = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            printf("  PASS: %s\n", message); \
            tests_passed++; \
        } else { \
            printf("  FAIL: %s\n", message); \
            tests_failed++; \
        } \
    } while (0)

static unsigned long long next_temp_path_nonce(void) {
    temp_path_counter++;
    return (((unsigned long long)time(NULL)) << 32) ^
           (unsigned long long)clock() ^
           ((unsigned long long)(unsigned int)rand() << 16) ^
           temp_path_counter;
}

static char* write_temp_vml(const char* content) {
#ifdef _WIN32
    const char* temp_dir = getenv("TEMP");
    if (!temp_dir) temp_dir = ".";
#else
    const char* temp_dir = getenv("TMPDIR");
    if (!temp_dir) temp_dir = "/tmp";
#endif

    char* path = (char*)safe_malloc(512);
    unsigned long long nonce = next_temp_path_nonce();
#ifdef _WIN32
    snprintf(path, 512, "%s\\tablo_runtime_safety_%llu.tblo", temp_dir, nonce);
#else
    snprintf(path, 512, "%s/tablo_runtime_safety_%llu.tblo", temp_dir, nonce);
#endif

    FILE* f = fopen(path, "wb");
    if (!f) {
        free(path);
        return NULL;
    }

    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return path;
}

static char* make_temp_path_with_ext(const char* ext) {
#ifdef _WIN32
    const char* temp_dir = getenv("TEMP");
    if (!temp_dir) temp_dir = ".";
#else
    const char* temp_dir = getenv("TMPDIR");
    if (!temp_dir) temp_dir = "/tmp";
#endif

    char* path = (char*)safe_malloc(512);
    unsigned long long nonce = next_temp_path_nonce();
#ifdef _WIN32
    snprintf(path, 512, "%s\\tablo_runtime_safety_%llu%s", temp_dir, nonce, ext ? ext : "");
#else
    snprintf(path, 512, "%s/tablo_runtime_safety_%llu%s", temp_dir, nonce, ext ? ext : "");
#endif
    return path;
}

static int write_file_all(const char* filename, const char* content) {
    FILE* f = fopen(filename, "wb");
    if (!f) return 0;
    if (content) {
        fwrite(content, 1, strlen(content), f);
    }
    fclose(f);
    return 1;
}

static int read_file_bytes_all(const char* filename, uint8_t** out_bytes, size_t* out_size) {
    FILE* f = NULL;
    long size = 0;
    uint8_t* bytes = NULL;
    if (out_bytes) *out_bytes = NULL;
    if (out_size) *out_size = 0;
    if (!filename || !out_bytes || !out_size) return 0;

    f = fopen(filename, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    bytes = (uint8_t*)safe_malloc((size_t)size);
    if (fread(bytes, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(bytes);
        return 0;
    }
    fclose(f);
    *out_bytes = bytes;
    *out_size = (size_t)size;
    return 1;
}

static int file_exists(const char* path) {
    if (!path || path[0] == '\0') return 0;
#ifdef _WIN32
    struct _stat64 st;
    return _stat64(path, &st) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

static double now_ms(void) {
    clock_t now = clock();
    if (now == (clock_t)-1) return 0.0;
    return ((double)now * 1000.0) / (double)CLOCKS_PER_SEC;
}

static double median3(double a, double b, double c) {
    if (a > b) {
        double t = a;
        a = b;
        b = t;
    }
    if (b > c) {
        double t = b;
        b = c;
        c = t;
    }
    if (a > b) {
        double t = a;
        a = b;
        b = t;
    }
    return b;
}

static void wait_for_mtime_tick(void) {
    clock_t start = clock();
    while (start != (clock_t)-1) {
        clock_t now = clock();
        if (now == (clock_t)-1) break;
        double elapsed = (double)(now - start) / (double)CLOCKS_PER_SEC;
        if (elapsed >= 2.0) break;
    }
}

static void cleanup_temp(char* path) {
    if (!path) return;
    remove(path);
    free(path);
}

static int message_is_oom(const char* message) {
    return message && strstr(message, "Out of memory") != NULL;
}

static void test_recoverable_safe_alloc_guard(void) {
    printf("Testing recoverable safe alloc guard...\n");

    SafeAllocJmpContext alloc_ctx;
    jmp_buf alloc_env;
    char alloc_message[256] = {0};
    safe_alloc_push_jmp_context(&alloc_ctx, &alloc_env, alloc_message, sizeof(alloc_message));

    if (setjmp(alloc_env) == 0) {
        (void)safe_malloc(SIZE_MAX);
        safe_alloc_pop_jmp_context(&alloc_ctx);
        TEST_ASSERT(0, "safe_malloc(SIZE_MAX) triggers OOM jump");
        return;
    }

    safe_alloc_pop_jmp_context(&alloc_ctx);
    TEST_ASSERT(1, "safe_malloc(SIZE_MAX) triggers OOM jump");
    TEST_ASSERT(alloc_message[0] != '\0', "OOM jump captures allocation failure message");
}

static void test_zero_size_safe_alloc_requests(void) {
    printf("Testing zero-size safe alloc requests...\n");

    void* a = safe_malloc(0);
    TEST_ASSERT(1, "safe_malloc(0) does not abort or longjmp");

    void* b = safe_calloc(0, sizeof(int));
    TEST_ASSERT(1, "safe_calloc(0, sizeof(int)) does not abort or longjmp");

    void* c = safe_realloc(a, 0);
    TEST_ASSERT(c == NULL, "safe_realloc(ptr, 0) clears the allocation without aborting");

    free(b);
}

static void test_runtime_create_injected_oom_recovery(void) {
    printf("Testing runtime_create injected OOM recovery...\n");

    const char* source =
        "func helper(v: int): int {\n"
        "    return v + 1;\n"
        "}\n"
        "func main(): void {\n"
        "    var out: int = helper(41);\n"
        "    println(out);\n"
        "}\n";
    char* source_path = write_temp_vml(source);
    TEST_ASSERT(source_path != NULL, "OOM create recovery fixture written");
    if (!source_path) return;

    int saw_oom = 0;
    int succeeded = 0;
    int failed = 0;
    size_t success_fail_after = 0;

    for (size_t fail_after = 0; fail_after < 2048; fail_after++) {
        safe_alloc_set_fail_after(fail_after);
        Runtime* rt = runtime_create(source_path);
        safe_alloc_clear_fail_after();

        if (!rt) {
            failed = 1;
            break;
        }

        if (runtime_has_error(rt)) {
            const char* err = runtime_get_error(rt);
            if (!message_is_oom(err)) {
                failed = 1;
                runtime_free(rt);
                break;
            }
            saw_oom++;
            runtime_free(rt);
            continue;
        }

        succeeded = 1;
        success_fail_after = fail_after;
        runtime_free(rt);
        break;
    }

    TEST_ASSERT(!failed, "runtime_create turns injected OOM into a recoverable runtime error");
    TEST_ASSERT(saw_oom > 0, "runtime_create exercised at least one injected OOM path");
    TEST_ASSERT(succeeded, "runtime_create still succeeds once failpoint is past all create-time allocations");
    if (succeeded) {
        TEST_ASSERT(success_fail_after > 0, "runtime_create OOM sweep crossed at least one guarded allocation point");
    }

    cleanup_temp(source_path);
}

static void test_runtime_run_injected_oom_recovery(void) {
    printf("Testing runtime_run injected OOM recovery...\n");

    const char* source =
        "func main(): void {\n"
        "    var text: string = \"seed\";\n"
        "    var i: int = 0;\n"
        "    while (i < 24) {\n"
        "        text = text + \"-abcdefgh\";\n"
        "        i = i + 1;\n"
        "    }\n"
        "    println(len(text));\n"
        "}\n";
    char* source_path = write_temp_vml(source);
    TEST_ASSERT(source_path != NULL, "OOM run recovery fixture written");
    if (!source_path) return;

    int saw_oom = 0;
    int succeeded = 0;
    int failed = 0;
    size_t success_fail_after = 0;

    for (size_t fail_after = 0; fail_after < 2048; fail_after++) {
        Runtime* rt = runtime_create(source_path);
        if (!rt || runtime_has_error(rt)) {
            failed = 1;
            if (rt) runtime_free(rt);
            break;
        }

        safe_alloc_set_fail_after(fail_after);
        int rc = runtime_run(rt);
        safe_alloc_clear_fail_after();

        if (runtime_has_error(rt)) {
            const char* err = runtime_get_error(rt);
            if (!message_is_oom(err)) {
                failed = 1;
                runtime_free(rt);
                break;
            }
            saw_oom++;
            runtime_free(rt);
            continue;
        }

        if (rc != 0) {
            failed = 1;
            runtime_free(rt);
            break;
        }

        succeeded = 1;
        success_fail_after = fail_after;
        runtime_free(rt);
        break;
    }

    TEST_ASSERT(!failed, "runtime_run turns injected OOM into a recoverable runtime error");
    TEST_ASSERT(saw_oom > 0, "runtime_run exercised at least one injected OOM path");
    TEST_ASSERT(succeeded, "runtime_run still succeeds once failpoint is past all execution-time allocations");
    if (succeeded) {
        TEST_ASSERT(success_fail_after > 0, "runtime_run OOM sweep crossed at least one guarded execution allocation");
    }

    cleanup_temp(source_path);
}

static int write_u32_le_file(FILE* f, uint32_t v) {
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xffu);
    b[1] = (unsigned char)((v >> 8) & 0xffu);
    b[2] = (unsigned char)((v >> 16) & 0xffu);
    b[3] = (unsigned char)((v >> 24) & 0xffu);
    return fwrite(b, 1, sizeof(b), f) == sizeof(b);
}

static int write_i32_le_file(FILE* f, int32_t v) {
    return write_u32_le_file(f, (uint32_t)v);
}

static int write_len_string_file(FILE* f, const char* text) {
    if (!text) text = "";
    size_t len = strlen(text);
    if (len > UINT32_MAX) return 0;
    if (!write_u32_le_file(f, (uint32_t)len)) return 0;
    if (len == 0) return 1;
    return fwrite(text, 1, len, f) == len;
}

static char* write_temp_cold_start_program(int helper_count) {
    if (helper_count < 1) helper_count = 1;

    char* path = make_temp_path_with_ext(".tblo");
    if (!path) return NULL;

    FILE* f = fopen(path, "wb");
    if (!f) {
        free(path);
        return NULL;
    }

    for (int i = 0; i < helper_count; i++) {
        fprintf(f,
                "func helper%d(x: int): int {\n"
                "    return x + %d;\n"
                "}\n",
                i,
                i);
    }

    fprintf(f,
            "func main(): void {\n"
            "    var x: int = helper0(1);\n"
            "    if (x > 0) {\n"
            "        var y: int = x + 1;\n"
            "        if (y > 2) {\n"
            "        }\n"
            "    }\n"
            "}\n");

    fclose(f);
    return path;
}

static int runtime_measure_create_and_run_ms(const char* path,
                                             RuntimeLoadMode* out_mode,
                                             double* out_ms) {
    if (!path || path[0] == '\0') return 0;

    double start_ms = now_ms();
    Runtime* rt = runtime_create(path);
    if (!rt) return 0;

    RuntimeLoadMode mode = runtime_get_load_mode(rt);
    if (runtime_has_error(rt)) {
        runtime_free(rt);
        return 0;
    }

    int rc = runtime_run(rt);
    int ok = (rc == 0) && !runtime_has_error(rt);
    double end_ms = now_ms();

    if (out_mode) *out_mode = mode;
    if (out_ms) *out_ms = (end_ms >= start_ms) ? (end_ms - start_ms) : 0.0;
    runtime_free(rt);
    return ok;
}

static int write_temp_artifact_from_source(const char* source_path, char** out_artifact_path) {
    if (!source_path || !out_artifact_path) return 0;
    *out_artifact_path = NULL;

    Runtime* rt_source = runtime_create(source_path);
    if (!rt_source || runtime_has_error(rt_source)) {
        if (rt_source) runtime_free(rt_source);
        return 0;
    }

    int main_index = -1;
    for (int i = 0; i < rt_source->function_count; i++) {
        if (rt_source->functions[i] == rt_source->main_function) {
            main_index = i;
            break;
        }
    }
    if (main_index < 0) {
        runtime_free(rt_source);
        return 0;
    }

    char* artifact_path = make_temp_path_with_ext(".tbc");
    if (!artifact_path) {
        runtime_free(rt_source);
        return 0;
    }
    remove(artifact_path);

    char err[256];
    int wrote = artifact_write_file(artifact_path,
                                    rt_source->init_function,
                                    rt_source->functions,
                                    rt_source->function_count,
                                    main_index,
                                    0,
                                    NULL,
                                    0,
                                    rt_source->interface_dispatch_entries,
                                    rt_source->interface_dispatch_count,
                                    err,
                                    sizeof(err));
    runtime_free(rt_source);

    if (!wrote) {
        cleanup_temp(artifact_path);
        return 0;
    }

    *out_artifact_path = artifact_path;
    return 1;
}

static void test_smoke_ok(void) {
    printf("Testing smoke run...\n");

    const char* source =
        "func main(): void {\n"
        "    var x: int = 1;\n"
        "}\n";

    char* filename = write_temp_vml(source);
    TEST_ASSERT(filename != NULL, "Temp file created");
    if (!filename) return;

    Runtime* rt = runtime_create(filename);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt), "runtime_create has no error");

    if (!runtime_has_error(rt)) {
        int rc = runtime_run(rt);
        TEST_ASSERT(rc == 0, "runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt), "runtime_run has no error");
    }

    runtime_free(rt);
    cleanup_temp(filename);
}

static void test_division_by_zero(void) {
    printf("Testing division by zero...\n");

    const char* source =
        "func main(): void {\n"
        "    var a: int = 10;\n"
        "    var b: int = 0;\n"
        "    var c: int = a / b;\n"
        "    println(c);\n"
        "}\n";

    char* filename = write_temp_vml(source);
    TEST_ASSERT(filename != NULL, "Temp file created");
    if (!filename) return;

    Runtime* rt = runtime_create(filename);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt), "runtime_create has no error");

    if (!runtime_has_error(rt)) {
        runtime_run(rt);
        TEST_ASSERT(runtime_has_error(rt), "Division by zero produces runtime error");
    }

    runtime_free(rt);
    cleanup_temp(filename);
}

static void test_array_bounds(void) {
    printf("Testing array bounds...\n");

    const char* source =
        "func main(): void {\n"
        "    var arr: array<int> = [10, 20, 30];\n"
        "    var x: int = arr[-1];\n"
        "    println(x);\n"
        "}\n";

    char* filename = write_temp_vml(source);
    TEST_ASSERT(filename != NULL, "Temp file created");
    if (!filename) return;

    Runtime* rt = runtime_create(filename);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt), "runtime_create has no error");

    if (!runtime_has_error(rt)) {
        runtime_run(rt);
        TEST_ASSERT(runtime_has_error(rt), "Out-of-range array index produces runtime error");
    }

    runtime_free(rt);
    cleanup_temp(filename);
}

static void test_replace_stress_no_abort(void) {
    printf("Testing replace stress (no abort/runtime corruption)...\n");

    const char* source =
        "func churn(seed: int): int {\n"
        "    var s: string = \"aaa.bbb.ccc:ddd/eee\";\n"
        "    var i: int = 0;\n"
        "    while (i < 1200) {\n"
        "        var mode: int = (i + seed) % 9;\n"
        "        if (mode == 0) {\n"
        "            s = replace(s, \".\", \"/\");\n"
        "        } else if (mode == 1) {\n"
        "            s = replace(s, \"/\", \".\");\n"
        "        } else if (mode == 2) {\n"
        "            s = replace(s, \"aa\", \"a\");\n"
        "        } else if (mode == 3) {\n"
        "            s = replace(s, \"a\", \"aa\");\n"
        "        } else if (mode == 4) {\n"
        "            s = replace(s, \"\", \"_\");\n"
        "        } else if (mode == 5) {\n"
        "            s = replace(s, \"not-found\", \"x\");\n"
        "        } else if (mode == 6) {\n"
        "            s = replace(s, \":\", \"::\");\n"
        "        } else if (mode == 7) {\n"
        "            s = replace(s, \"::\", \":\");\n"
        "        } else {\n"
        "            s = replace(s, \"bbb\", \"bb\");\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return len(s);\n"
        "}\n"
        "func main(): void {\n"
        "    var total: int = 0;\n"
        "    var j: int = 0;\n"
        "    while (j < 200) {\n"
        "        total = total + churn(j);\n"
        "        j = j + 1;\n"
        "    }\n"
        "    if (total <= 0) {\n"
        "        panic(\"replace stress produced invalid total\");\n"
        "    }\n"
        "}\n";

    char* filename = write_temp_vml(source);
    TEST_ASSERT(filename != NULL, "Temp file created");
    if (!filename) return;

    const int runs = 20;
    for (int i = 0; i < runs; i++) {
        Runtime* rt = runtime_create(filename);
        TEST_ASSERT(rt != NULL, "replace stress runtime_create returns Runtime");
        if (!rt) break;

        TEST_ASSERT(!runtime_has_error(rt), "replace stress runtime_create has no error");
        if (!runtime_has_error(rt)) {
            int rc = runtime_run(rt);
            TEST_ASSERT(rc == 0, "replace stress runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt), "replace stress runtime_run has no error");
        }

        runtime_free(rt);
    }

    cleanup_temp(filename);
}

static void test_core_builtin_churn_no_abort(void) {
    printf("Testing core builtins churn (no abort/runtime corruption)...\n");

    const char* source =
        "func churn(seed: int): int {\n"
        "    var text: string = \"alpha,beta,gamma,delta\";\n"
        "    var counts: map<string, int> = {};\n"
        "    var tags: set<string> = {\"_seed\"};\n"
        "    var i: int = 0;\n"
        "    while (i < 900) {\n"
        "        var mode: int = (i + seed) % 8;\n"
        "        if (mode == 0) {\n"
        "            text = replace(text, \",\", \"/\");\n"
        "        } else if (mode == 1) {\n"
        "            text = replace(text, \"/\", \",\");\n"
        "        } else if (mode == 2) {\n"
        "            var parts: array<string> = split(text, \",\");\n"
        "            if (len(parts) > 0) {\n"
        "                var key: string = parts[0];\n"
        "                var old: int? = mapGet(counts, key);\n"
        "                if (old == nil) {\n"
        "                    mapSetString(counts, key, 1);\n"
        "                } else {\n"
        "                    mapSetString(counts, key, (old as int) + 1);\n"
        "                }\n"
        "                setAddString(tags, key);\n"
        "            }\n"
        "        } else if (mode == 3) {\n"
        "            var enc = stringToBytes(text);\n"
        "            if (enc.1 != nil) {\n"
        "                panic(\"stringToBytes failed in churn\");\n"
        "            }\n"
        "            var hex: string = bytesToHex(enc.0).0;\n"
        "            var dec = hexToBytes(hex);\n"
        "            if (dec.1 != nil) {\n"
        "                panic(\"hexToBytes failed in churn\");\n"
        "            }\n"
        "            if (len(dec.0) == 0) {\n"
        "                panic(\"decoded bytes unexpectedly empty\");\n"
        "            }\n"
        "        } else if (mode == 4) {\n"
        "            var payload: map<string, any> = {};\n"
        "            mapSetString(payload, \"seed\", seed);\n"
        "            mapSetString(payload, \"i\", i);\n"
        "            mapSetString(payload, \"text\", text);\n"
        "            var encoded = jsonStringify(payload as any);\n"
        "            if (encoded.1 != nil) {\n"
        "                panic(\"jsonStringify failed in churn\");\n"
        "            }\n"
        "            var parsed = jsonParse(encoded.0);\n"
        "            if (parsed.1 != nil) {\n"
        "                panic(\"jsonParse failed in churn\");\n"
        "            }\n"
        "            var parsed_map: map<string, any> = parsed.0 as map<string, any>;\n"
        "            if (mapGetString(parsed_map, \"seed\") as int != seed) {\n"
        "                panic(\"parsed seed mismatch\");\n"
        "            }\n"
        "        } else if (mode == 5) {\n"
        "            var joined: string = join(split(text, \",\"), \",\");\n"
        "            if (len(joined) > 0) {\n"
        "                text = joined;\n"
        "            }\n"
        "        } else if (mode == 6) {\n"
        "            var k: string = \"k\" + str((i + seed) % 17);\n"
        "            mapSetString(counts, k, i);\n"
        "            if (mapHasString(counts, k)) {\n"
        "                setAddString(tags, k);\n"
        "            }\n"
        "        } else {\n"
        "            if (len(text) > 6) {\n"
        "                text = substring(text, 0, len(text) - 1);\n"
        "            }\n"
        "            text = text + \"x\";\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return len(text) + mapCount(counts) + setCount(tags);\n"
        "}\n"
        "func main(): void {\n"
        "    var sum: int = 0;\n"
        "    var r: int = 0;\n"
        "    while (r < 120) {\n"
        "        sum = sum + churn(r);\n"
        "        r = r + 1;\n"
        "    }\n"
        "    if (sum <= 0) {\n"
        "        panic(\"core churn produced invalid sum\");\n"
        "    }\n"
        "}\n";

    char* filename = write_temp_vml(source);
    TEST_ASSERT(filename != NULL, "Temp file created");
    if (!filename) return;

    const int runs = 10;
    for (int i = 0; i < runs; i++) {
        Runtime* rt = runtime_create(filename);
        TEST_ASSERT(rt != NULL, "core churn runtime_create returns Runtime");
        if (!rt) break;

        TEST_ASSERT(!runtime_has_error(rt), "core churn runtime_create has no error");
        if (!runtime_has_error(rt)) {
            int rc = runtime_run(rt);
            TEST_ASSERT(rc == 0, "core churn runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt), "core churn runtime_run has no error");
        }

        runtime_free(rt);
    }

    cleanup_temp(filename);
}

static void test_sort_reverse_builtin_churn_no_abort(void) {
    printf("Testing sort/reverse builtins churn (no abort/runtime corruption)...\n");

    const char* source =
        "func churn(seed: int): int {\n"
        "    var ints: array<int> = [9, 1, 7, 3, 5, 2, 8, 4, 6, 0];\n"
        "    var words: array<string> = [\"delta\", \"alpha\", \"charlie\", \"bravo\"];\n"
        "    var flags: array<bool> = [true, false, true, false, true, false];\n"
        "    var mixed: array<any> = [3 as any, \"m\" as any, 1 as any];\n"
        "    var i: int = 0;\n"
        "    while (i < 700) {\n"
        "        var mode: int = (i + seed) % 6;\n"
        "        if (mode == 0) {\n"
        "            sort(ints);\n"
        "        } else if (mode == 1) {\n"
        "            reverse(ints);\n"
        "        } else if (mode == 2) {\n"
        "            sort(words);\n"
        "        } else if (mode == 3) {\n"
        "            reverse(words);\n"
        "        } else if (mode == 4) {\n"
        "            sort(flags);\n"
        "            reverse(flags);\n"
        "        } else {\n"
        "            sort(mixed);\n"
        "            reverse(mixed);\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    if ((mixed[1] as string) != \"m\") {\n"
        "        panic(\"mixed center element changed\");\n"
        "    }\n"
        "    return ints[0] + len(words) + len(mixed);\n"
        "}\n"
        "func main(): void {\n"
        "    var total: int = 0;\n"
        "    var r: int = 0;\n"
        "    while (r < 100) {\n"
        "        total = total + churn(r);\n"
        "        r = r + 1;\n"
        "    }\n"
        "    if (total == 0) {\n"
        "        panic(\"sort/reverse churn produced invalid total\");\n"
        "    }\n"
        "}\n";

    char* filename = write_temp_vml(source);
    TEST_ASSERT(filename != NULL, "Temp file created");
    if (!filename) return;

    const int runs = 12;
    for (int i = 0; i < runs; i++) {
        Runtime* rt = runtime_create(filename);
        TEST_ASSERT(rt != NULL, "sort/reverse churn runtime_create returns Runtime");
        if (!rt) break;

        TEST_ASSERT(!runtime_has_error(rt), "sort/reverse churn runtime_create has no error");
        if (!runtime_has_error(rt)) {
            int rc = runtime_run(rt);
            TEST_ASSERT(rc == 0, "sort/reverse churn runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt), "sort/reverse churn runtime_run has no error");
        }

        runtime_free(rt);
    }

    cleanup_temp(filename);
}

static void test_execution_limit(void) {
    printf("Testing execution limit...\n");

    const char* source =
        "func main(): void {\n"
        "    while (true) { }\n"
        "}\n";

    char* filename = write_temp_vml(source);
    TEST_ASSERT(filename != NULL, "Temp file created");
    if (!filename) return;

    Runtime* rt = runtime_create(filename);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt), "runtime_create has no error");

    if (!runtime_has_error(rt)) {
        rt->vm->config.max_instructions = 10000;
        runtime_run(rt);
        TEST_ASSERT(runtime_has_error(rt), "Execution limit produces runtime error");
    }

    runtime_free(rt);
    cleanup_temp(filename);
}

static void test_stack_trace_for_runtime_trap(void) {
    printf("Testing stack trace on runtime trap...\n");

    const char* source =
        "func leaf(flag: int): int {\n"
        "    if (flag == 1) {\n"
        "        var a: int = 10;\n"
        "        var b: int = 0;\n"
        "        return a / b;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "func middle(): int {\n"
        "    return leaf(1);\n"
        "}\n"
        "func main(): void {\n"
        "    var x: int = middle();\n"
        "    println(x);\n"
        "}\n";

    char* filename = write_temp_vml(source);
    TEST_ASSERT(filename != NULL, "Temp file created");
    if (!filename) return;

    Runtime* rt = runtime_create(filename);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt), "runtime_create has no error");

    if (!runtime_has_error(rt)) {
        runtime_run(rt);
        TEST_ASSERT(runtime_has_error(rt), "Nested division by zero produces runtime error");
        if (runtime_has_error(rt)) {
            const char* err = runtime_get_error(rt);
            TEST_ASSERT(err != NULL, "Runtime error message is available");
            TEST_ASSERT(err && strstr(err, "Division by zero") != NULL, "Runtime error message includes trap cause");
            TEST_ASSERT(err && strstr(err, "Stack trace:") != NULL, "Runtime error message includes stack trace header");
            TEST_ASSERT(err && strstr(err, "at leaf") != NULL, "Stack trace includes leaf frame");
            TEST_ASSERT(err && strstr(err, "at middle") != NULL, "Stack trace includes middle frame");
            TEST_ASSERT(err && strstr(err, "at main") != NULL, "Stack trace includes main frame");
        }
    }

    runtime_free(rt);
    cleanup_temp(filename);
}

static void test_stack_trace_for_panic(void) {
    printf("Testing stack trace on panic...\n");

    const char* source =
        "func leaf(): void {\n"
        "    panic(\"boom\");\n"
        "}\n"
        "func middle(): void {\n"
        "    leaf();\n"
        "}\n"
        "func main(): void {\n"
        "    middle();\n"
        "}\n";

    char* filename = write_temp_vml(source);
    TEST_ASSERT(filename != NULL, "Temp file created");
    if (!filename) return;

    Runtime* rt = runtime_create(filename);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt), "runtime_create has no error");

    if (!runtime_has_error(rt)) {
        runtime_run(rt);
        TEST_ASSERT(runtime_has_error(rt), "Nested panic produces runtime error");
        if (runtime_has_error(rt)) {
            const char* err = runtime_get_error(rt);
            TEST_ASSERT(err != NULL, "Panic error message is available");
            TEST_ASSERT(err && strstr(err, "Panic: boom") != NULL, "Panic message includes user text");
            TEST_ASSERT(err && strstr(err, "Stack trace:") != NULL, "Panic message includes stack trace header");
            TEST_ASSERT(err && strstr(err, "at leaf") != NULL, "Panic stack trace includes leaf frame");
            TEST_ASSERT(err && strstr(err, "at middle") != NULL, "Panic stack trace includes middle frame");
            TEST_ASSERT(err && strstr(err, "at main") != NULL, "Panic stack trace includes main frame");
        }
    }

    runtime_free(rt);
    cleanup_temp(filename);
}

static void test_cache_cold_start_and_invalidation(void) {
    printf("Testing bytecode cache cold start and invalidation...\n");

    const char* source_v1 =
        "func main(): void {\n"
        "    var x: int = 1;\n"
        "}\n";
    const char* source_v2 =
        "func main(): void {\n"
        "    var x: int = 2;\n"
        "}\n";

    char* filename = write_temp_vml(source_v1);
    TEST_ASSERT(filename != NULL, "Temp source file created");
    if (!filename) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(filename);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt_probe), "Probe runtime_create has no error");
    if (runtime_has_error(rt_probe)) {
        runtime_free(rt_probe);
        cleanup_temp(filename);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for source file");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        (void)remove(cache_path_copy);
    }

    Runtime* rt_first = runtime_create(filename);
    TEST_ASSERT(rt_first != NULL, "First runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt_first), "First runtime_create has no error");
    if (!runtime_has_error(rt_first)) {
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "First load mode is source");
        int rc = runtime_run(rt_first);
        TEST_ASSERT(rc == 0, "First runtime_run succeeds");
    }
    runtime_free(rt_first);

    if (cache_path_copy) {
        TEST_ASSERT(file_exists(cache_path_copy), "Cache artifact is written after first run");
    }

    Runtime* rt_second = runtime_create(filename);
    TEST_ASSERT(rt_second != NULL, "Second runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt_second), "Second runtime_create has no error");
    if (!runtime_has_error(rt_second)) {
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Second load mode is cache hit");
        int rc = runtime_run(rt_second);
        TEST_ASSERT(rc == 0, "Second runtime_run succeeds");
    }
    runtime_free(rt_second);

    wait_for_mtime_tick();
    TEST_ASSERT(write_file_all(filename, source_v2), "Source file rewritten for cache invalidation");

    Runtime* rt_third = runtime_create(filename);
    TEST_ASSERT(rt_third != NULL, "Third runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt_third), "Third runtime_create has no error");
    if (!runtime_has_error(rt_third)) {
        TEST_ASSERT(runtime_get_load_mode(rt_third) == RUNTIME_LOAD_SOURCE, "Load mode falls back to source after mtime change");
        int rc = runtime_run(rt_third);
        TEST_ASSERT(rc == 0, "Third runtime_run succeeds");
    }
    runtime_free(rt_third);

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(filename);
}

static void test_cache_dependency_invalidation(void) {
    printf("Testing bytecode cache invalidation on dependency change...\n");

    char* main_path = make_temp_path_with_ext(".tblo");
    TEST_ASSERT(main_path != NULL, "Temp main source path created");
    if (!main_path) return;

    size_t module_path_cap = strlen(main_path) + 16;
    char* module_path = (char*)safe_malloc(module_path_cap);
    snprintf(module_path, module_path_cap, "%s_dep.tblo", main_path);

    const char* module_name = strrchr(module_path, '\\');
    if (!module_name) {
        module_name = strrchr(module_path, '/');
    }
    module_name = module_name ? (module_name + 1) : module_path;

    char main_source[2048];
    snprintf(main_source,
             sizeof(main_source),
             "import \"%s\";\n"
             "\n"
             "func main(): void {\n"
             "    if (depValue() != 2) {\n"
             "        panic(\"dep mismatch\");\n"
             "    }\n"
             "}\n",
             module_name);

    const char* dep_v1 =
        "func depValue(): int {\n"
        "    return 2;\n"
        "}\n";
    const char* dep_v2 =
        "func depValue(): int {\n"
        "    return 1;\n"
        "}\n";

    int wrote_main = write_file_all(main_path, main_source);
    TEST_ASSERT(wrote_main, "Main source written for dependency cache test");
    int wrote_dep_v1 = write_file_all(module_path, dep_v1);
    TEST_ASSERT(wrote_dep_v1, "Dependency source v1 written for cache test");
    if (!wrote_main || !wrote_dep_v1) {
        cleanup_temp(module_path);
        cleanup_temp(main_path);
        return;
    }

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(main_path);
    TEST_ASSERT(rt_probe != NULL, "Dependency cache probe runtime_create returns Runtime");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Dependency cache probe runtime_create has no error");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(module_path);
        cleanup_temp(main_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for dependency cache test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        (void)remove(cache_path_copy);
    }

    Runtime* rt_first = runtime_create(main_path);
    TEST_ASSERT(rt_first != NULL, "Dependency test first runtime_create returns Runtime");
    TEST_ASSERT(rt_first && !runtime_has_error(rt_first), "Dependency test first runtime_create has no error");
    if (rt_first && !runtime_has_error(rt_first)) {
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "Dependency test first load mode is source");
        int rc = runtime_run(rt_first);
        TEST_ASSERT(rc == 0, "Dependency test first runtime_run succeeds");
    }
    if (rt_first) runtime_free(rt_first);

    Runtime* rt_second = runtime_create(main_path);
    TEST_ASSERT(rt_second != NULL, "Dependency test second runtime_create returns Runtime");
    TEST_ASSERT(rt_second && !runtime_has_error(rt_second), "Dependency test second runtime_create has no error");
    if (rt_second && !runtime_has_error(rt_second)) {
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Dependency test second load mode is cache hit");
        int rc = runtime_run(rt_second);
        TEST_ASSERT(rc == 0, "Dependency test second runtime_run succeeds");
    }
    if (rt_second) runtime_free(rt_second);

    wait_for_mtime_tick();
    int wrote_dep_v2 = write_file_all(module_path, dep_v2);
    TEST_ASSERT(wrote_dep_v2, "Dependency source rewritten for cache invalidation");
    if (!wrote_dep_v2) {
        if (cache_path_copy) {
            remove(cache_path_copy);
            free(cache_path_copy);
        }
        cleanup_temp(module_path);
        cleanup_temp(main_path);
        return;
    }

    Runtime* rt_third = runtime_create(main_path);
    TEST_ASSERT(rt_third != NULL, "Dependency test third runtime_create returns Runtime");
    TEST_ASSERT(rt_third && !runtime_has_error(rt_third), "Dependency test third runtime_create has no create error");
    if (rt_third && !runtime_has_error(rt_third)) {
        TEST_ASSERT(runtime_get_load_mode(rt_third) == RUNTIME_LOAD_SOURCE,
                    "Dependency mtime change invalidates cache to source load");
        int rc = runtime_run(rt_third);
        TEST_ASSERT(rc != 0, "Dependency test third runtime_run fails after dep change");
        TEST_ASSERT(runtime_has_error(rt_third), "Dependency test third runtime_run exposes runtime error");
        if (runtime_has_error(rt_third)) {
            const char* err = runtime_get_error(rt_third);
            TEST_ASSERT(err && strstr(err, "dep mismatch") != NULL, "Dependency test runtime error includes mismatch panic");
        }
    }
    if (rt_third) runtime_free(rt_third);

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(module_path);
    cleanup_temp(main_path);
}

static void test_cache_separated_by_typecheck_flags(void) {
    printf("Testing bytecode cache separation by typecheck flags...\n");

    const char* source =
        "func returnsValue(): (int, Error?) {\n"
        "    return (7, nil);\n"
        "}\n"
        "\n"
        "func main(): void {\n"
        "    returnsValue();\n"
        "}\n";

    char* source_path = write_temp_vml(source);
    TEST_ASSERT(source_path != NULL, "Temp source file created for typecheck-flag cache test");
    if (!source_path) return;

    RuntimeOptions options_default = (RuntimeOptions){0};
    RuntimeOptions options_warn = (RuntimeOptions){0};
    options_warn.typecheck.warn_unused_error = true;

    RuntimeOptions options_strict = (RuntimeOptions){0};
    options_strict.typecheck.warn_unused_error = true;
    options_strict.typecheck.strict_errors = true;

    char* cache_default = NULL;
    char* cache_warn = NULL;
    char* cache_strict = NULL;

    Runtime* probe_default = runtime_create_with_options(source_path, options_default);
    TEST_ASSERT(probe_default != NULL, "Default probe runtime_create_with_options returns Runtime");
    TEST_ASSERT(probe_default && !runtime_has_error(probe_default), "Default probe runtime_create_with_options has no error");
    if (probe_default) {
        const char* cache = runtime_get_cache_path(probe_default);
        TEST_ASSERT(cache != NULL && cache[0] != '\0', "Default probe cache path is available");
        if (cache && cache[0] != '\0') cache_default = safe_strdup(cache);
        runtime_free(probe_default);
    }

    Runtime* probe_warn = runtime_create_with_options(source_path, options_warn);
    TEST_ASSERT(probe_warn != NULL, "Warn probe runtime_create_with_options returns Runtime");
    TEST_ASSERT(probe_warn && !runtime_has_error(probe_warn), "Warn probe runtime_create_with_options has no error");
    if (probe_warn) {
        const char* cache = runtime_get_cache_path(probe_warn);
        TEST_ASSERT(cache != NULL && cache[0] != '\0', "Warn probe cache path is available");
        if (cache && cache[0] != '\0') cache_warn = safe_strdup(cache);
        runtime_free(probe_warn);
    }

    Runtime* probe_strict = runtime_create_with_options(source_path, options_strict);
    TEST_ASSERT(probe_strict != NULL, "Strict probe runtime_create_with_options returns Runtime");
    if (probe_strict) {
        const char* cache = runtime_get_cache_path(probe_strict);
        TEST_ASSERT(cache != NULL && cache[0] != '\0', "Strict probe cache path is available");
        if (cache && cache[0] != '\0') cache_strict = safe_strdup(cache);
        TEST_ASSERT(runtime_has_error(probe_strict), "Strict probe reports unused-error as type error");
        runtime_free(probe_strict);
    }

    TEST_ASSERT(cache_default != NULL, "Default cache path copied");
    TEST_ASSERT(cache_warn != NULL, "Warn cache path copied");
    TEST_ASSERT(cache_strict != NULL, "Strict cache path copied");
    if (!cache_default || !cache_warn || !cache_strict) {
        cleanup_temp(cache_default);
        cleanup_temp(cache_warn);
        cleanup_temp(cache_strict);
        cleanup_temp(source_path);
        return;
    }

    TEST_ASSERT(strcmp(cache_default, cache_warn) != 0, "Default and warn cache paths differ");
    TEST_ASSERT(strcmp(cache_default, cache_strict) != 0, "Default and strict cache paths differ");
    TEST_ASSERT(strcmp(cache_warn, cache_strict) != 0, "Warn and strict cache paths differ");

    remove(cache_default);
    remove(cache_warn);
    remove(cache_strict);

    Runtime* rt_default_first = runtime_create_with_options(source_path, options_default);
    TEST_ASSERT(rt_default_first != NULL, "Default first runtime_create_with_options returns Runtime");
    TEST_ASSERT(rt_default_first && !runtime_has_error(rt_default_first), "Default first runtime_create_with_options has no error");
    if (rt_default_first && !runtime_has_error(rt_default_first)) {
        TEST_ASSERT(runtime_get_load_mode(rt_default_first) == RUNTIME_LOAD_SOURCE, "Default first load mode is source");
        int rc = runtime_run(rt_default_first);
        TEST_ASSERT(rc == 0, "Default first runtime_run succeeds");
    }
    if (rt_default_first) runtime_free(rt_default_first);
    TEST_ASSERT(file_exists(cache_default), "Default cache artifact exists after first run");

    Runtime* rt_default_second = runtime_create_with_options(source_path, options_default);
    TEST_ASSERT(rt_default_second != NULL, "Default second runtime_create_with_options returns Runtime");
    TEST_ASSERT(rt_default_second && !runtime_has_error(rt_default_second), "Default second runtime_create_with_options has no error");
    if (rt_default_second && !runtime_has_error(rt_default_second)) {
        TEST_ASSERT(runtime_get_load_mode(rt_default_second) == RUNTIME_LOAD_CACHE, "Default second load mode is cache hit");
        int rc = runtime_run(rt_default_second);
        TEST_ASSERT(rc == 0, "Default second runtime_run succeeds");
    }
    if (rt_default_second) runtime_free(rt_default_second);

    Runtime* rt_warn_first = runtime_create_with_options(source_path, options_warn);
    TEST_ASSERT(rt_warn_first != NULL, "Warn first runtime_create_with_options returns Runtime");
    TEST_ASSERT(rt_warn_first && !runtime_has_error(rt_warn_first), "Warn first runtime_create_with_options has no error");
    if (rt_warn_first && !runtime_has_error(rt_warn_first)) {
        TEST_ASSERT(runtime_get_load_mode(rt_warn_first) == RUNTIME_LOAD_SOURCE, "Warn first load mode is source");
        int rc = runtime_run(rt_warn_first);
        TEST_ASSERT(rc == 0, "Warn first runtime_run succeeds");
    }
    if (rt_warn_first) runtime_free(rt_warn_first);
    TEST_ASSERT(file_exists(cache_warn), "Warn cache artifact exists after first run");

    Runtime* rt_warn_second = runtime_create_with_options(source_path, options_warn);
    TEST_ASSERT(rt_warn_second != NULL, "Warn second runtime_create_with_options returns Runtime");
    TEST_ASSERT(rt_warn_second && !runtime_has_error(rt_warn_second), "Warn second runtime_create_with_options has no error");
    if (rt_warn_second && !runtime_has_error(rt_warn_second)) {
        TEST_ASSERT(runtime_get_load_mode(rt_warn_second) == RUNTIME_LOAD_CACHE, "Warn second load mode is cache hit");
        int rc = runtime_run(rt_warn_second);
        TEST_ASSERT(rc == 0, "Warn second runtime_run succeeds");
    }
    if (rt_warn_second) runtime_free(rt_warn_second);

    Runtime* rt_strict = runtime_create_with_options(source_path, options_strict);
    TEST_ASSERT(rt_strict != NULL, "Strict runtime_create_with_options returns Runtime");
    if (rt_strict) {
        TEST_ASSERT(runtime_has_error(rt_strict), "Strict runtime_create_with_options rejects unused error result");
        if (runtime_has_error(rt_strict)) {
            const char* err = runtime_get_error(rt_strict);
            TEST_ASSERT(err && strstr(err, "Unused (value, Error?) result") != NULL,
                        "Strict typecheck error includes unused-result message");
        }
        runtime_free(rt_strict);
    }

    TEST_ASSERT(!file_exists(cache_strict), "Strict cache artifact is not written on typecheck error");
    TEST_ASSERT(file_exists(cache_default), "Default cache artifact remains available");
    TEST_ASSERT(file_exists(cache_warn), "Warn cache artifact remains available");

    remove(cache_default);
    remove(cache_warn);
    remove(cache_strict);
    cleanup_temp(cache_default);
    cleanup_temp(cache_warn);
    cleanup_temp(cache_strict);
    cleanup_temp(source_path);
}

static void test_artifact_load_mode(void) {
    printf("Testing artifact load mode...\n");

    const char* source =
        "func main(): void {\n"
        "    var x: int = 7;\n"
        "}\n";

    char* source_path = write_temp_vml(source);
    TEST_ASSERT(source_path != NULL, "Temp source file created for artifact test");
    if (!source_path) return;

    Runtime* rt_source = runtime_create(source_path);
    TEST_ASSERT(rt_source != NULL, "Source runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt_source), "Source runtime_create has no error");
    if (runtime_has_error(rt_source)) {
        runtime_free(rt_source);
        cleanup_temp(source_path);
        return;
    }

    int main_index = -1;
    for (int i = 0; i < rt_source->function_count; i++) {
        if (rt_source->functions[i] == rt_source->main_function) {
            main_index = i;
            break;
        }
    }
    TEST_ASSERT(main_index >= 0, "Main function index resolved for artifact write");

    char* artifact_path = make_temp_path_with_ext(".tbc");
    TEST_ASSERT(artifact_path != NULL, "Temp artifact path allocated");
    if (!artifact_path) {
        runtime_free(rt_source);
        cleanup_temp(source_path);
        return;
    }
    remove(artifact_path);

    char err[256];
    int wrote = artifact_write_file(artifact_path,
                                    rt_source->init_function,
                                    rt_source->functions,
                                    rt_source->function_count,
                                    main_index,
                                    0,
                                    NULL,
                                    0,
                                    rt_source->interface_dispatch_entries,
                                    rt_source->interface_dispatch_count,
                                    err,
                                    sizeof(err));
    TEST_ASSERT(wrote, "Artifact write succeeds");
    TEST_ASSERT(file_exists(artifact_path), "Artifact file exists");

    runtime_free(rt_source);

    Runtime* rt_artifact = runtime_create(artifact_path);
    TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt_artifact), "Artifact runtime_create has no error");
    if (!runtime_has_error(rt_artifact)) {
        TEST_ASSERT(runtime_get_load_mode(rt_artifact) == RUNTIME_LOAD_ARTIFACT, "Load mode is artifact");
        int rc = runtime_run(rt_artifact);
        TEST_ASSERT(rc == 0, "Artifact runtime_run succeeds");
    }
    runtime_free(rt_artifact);

    cleanup_temp(artifact_path);
    cleanup_temp(source_path);
}

static void test_artifact_stack_trace_source_locations(void) {
    printf("Testing artifact stack trace source locations...\n");

    const char* source =
        "func leaf(): void {\n"
        "    panic(\"artifact boom\");\n"
        "}\n"
        "func middle(): void {\n"
        "    leaf();\n"
        "}\n"
        "func main(): void {\n"
        "    middle();\n"
        "}\n";

    char* source_path = write_temp_vml(source);
    TEST_ASSERT(source_path != NULL, "Temp source file created for artifact stack trace test");
    if (!source_path) return;

    Runtime* rt_source = runtime_create(source_path);
    TEST_ASSERT(rt_source != NULL, "Source runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt_source), "Source runtime_create has no error");
    if (runtime_has_error(rt_source)) {
        runtime_free(rt_source);
        cleanup_temp(source_path);
        return;
    }

    int main_index = -1;
    for (int i = 0; i < rt_source->function_count; i++) {
        if (rt_source->functions[i] == rt_source->main_function) {
            main_index = i;
            break;
        }
    }
    TEST_ASSERT(main_index >= 0, "Main function index resolved for artifact stack trace write");

    char* artifact_path = make_temp_path_with_ext(".tbc");
    TEST_ASSERT(artifact_path != NULL, "Temp artifact path allocated for stack trace test");
    if (!artifact_path) {
        runtime_free(rt_source);
        cleanup_temp(source_path);
        return;
    }
    remove(artifact_path);

    char err_write[256];
    int wrote = artifact_write_file(artifact_path,
                                    rt_source->init_function,
                                    rt_source->functions,
                                    rt_source->function_count,
                                    main_index,
                                    0,
                                    NULL,
                                    0,
                                    rt_source->interface_dispatch_entries,
                                    rt_source->interface_dispatch_count,
                                    err_write,
                                    sizeof(err_write));
    TEST_ASSERT(wrote, "Artifact write succeeds for stack trace test");
    TEST_ASSERT(file_exists(artifact_path), "Artifact file exists for stack trace test");
    runtime_free(rt_source);

    Runtime* rt_artifact = runtime_create(artifact_path);
    TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt_artifact), "Artifact runtime_create has no error");
    if (!runtime_has_error(rt_artifact)) {
        runtime_run(rt_artifact);
        TEST_ASSERT(runtime_has_error(rt_artifact), "Artifact runtime panic produces runtime error");
        if (runtime_has_error(rt_artifact)) {
            const char* err = runtime_get_error(rt_artifact);
            char leaf_loc_prefix[1024];
            leaf_loc_prefix[0] = '\0';
            snprintf(leaf_loc_prefix, sizeof(leaf_loc_prefix), "at leaf (%s:", source_path);
            TEST_ASSERT(err != NULL, "Artifact runtime error message is available");
            TEST_ASSERT(err && strstr(err, "Panic: artifact boom") != NULL, "Artifact runtime error includes panic message");
            TEST_ASSERT(err && strstr(err, "Stack trace:") != NULL, "Artifact runtime error includes stack trace header");
            TEST_ASSERT(err && strstr(err, "at leaf") != NULL, "Artifact stack trace includes leaf frame");
            TEST_ASSERT(err && strstr(err, "at middle") != NULL, "Artifact stack trace includes middle frame");
            TEST_ASSERT(err && strstr(err, "at main") != NULL, "Artifact stack trace includes main frame");
            TEST_ASSERT(err && strstr(err, source_path) != NULL, "Artifact stack trace includes source file path");
            TEST_ASSERT(err && strstr(err, leaf_loc_prefix) != NULL, "Artifact leaf frame includes source path and line");
        }
    }
    runtime_free(rt_artifact);

    cleanup_temp(artifact_path);
    cleanup_temp(source_path);
}

static void test_interface_impl_runtime_dispatch(void) {
    printf("Testing interface runtime dispatch...\n");

    const char* source =
        "record Point { x: int, y: int };\n"
        "record Vector { vx: int, vy: int };\n"
        "interface Mover {\n"
        "    move(dx: int, dy: int): int;\n"
        "};\n"
        "func pointMove(p: Point, dx: int, dy: int): int {\n"
        "    return p.x + p.y + dx + dy;\n"
        "}\n"
        "func vectorMove(v: Vector, dx: int, dy: int): int {\n"
        "    return v.vx * 100 + v.vy * 10 + dx + dy;\n"
        "}\n"
        "impl Mover as Point {\n"
        "    move = pointMove;\n"
        "};\n"
        "impl Mover as Vector {\n"
        "    move = vectorMove;\n"
        "};\n"
        "func dispatchPoint(): int {\n"
        "    var p: Point = { x: 1, y: 2 };\n"
        "    var m: Mover = p;\n"
        "    return m.move(3, 4);\n"
        "}\n"
        "func dispatchVector(): int {\n"
        "    var v: Vector = { vx: 5, vy: 6 };\n"
        "    var m: Mover = v;\n"
        "    return m.move(3, 4);\n"
        "}\n"
        "func main(): void {\n"
        "}\n";

    char* source_path = write_temp_vml(source);
    TEST_ASSERT(source_path != NULL, "Temp source file created for interface dispatch test");
    if (!source_path) return;

    Runtime* rt = runtime_create(source_path);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime for interface dispatch test");
    TEST_ASSERT(rt && !runtime_has_error(rt), "runtime_create has no load error for interface dispatch test");
    if (rt && !runtime_has_error(rt)) {
        int rc_main = runtime_run(rt);
        TEST_ASSERT(rc_main == 0, "interface dispatch test main() executes");
        TEST_ASSERT(!runtime_has_error(rt), "interface dispatch test main() has no runtime error");

        if (rc_main == 0 && !runtime_has_error(rt)) {
            int rc_point = runtime_run_function(rt, "dispatchPoint");
            TEST_ASSERT(rc_point == 0, "dispatchPoint() executes");
            TEST_ASSERT(!runtime_has_error(rt), "dispatchPoint() has no runtime error");
            if (rc_point == 0 && !runtime_has_error(rt)) {
                Value out_point;
                int got_point = runtime_take_return_value(rt, &out_point);
                TEST_ASSERT(got_point, "dispatchPoint() return value captured");
                if (got_point) {
                    TEST_ASSERT(value_get_type(&out_point) == VAL_INT &&
                                    value_get_int(&out_point) == 10,
                                "dispatchPoint() uses Point impl mapping");
                    value_free(&out_point);
                }
            }

            int rc_vector = runtime_run_function(rt, "dispatchVector");
            TEST_ASSERT(rc_vector == 0, "dispatchVector() executes");
            TEST_ASSERT(!runtime_has_error(rt), "dispatchVector() has no runtime error");
            if (rc_vector == 0 && !runtime_has_error(rt)) {
                Value out_vector;
                int got_vector = runtime_take_return_value(rt, &out_vector);
                TEST_ASSERT(got_vector, "dispatchVector() return value captured");
                if (got_vector) {
                    TEST_ASSERT(value_get_type(&out_vector) == VAL_INT &&
                                    value_get_int(&out_vector) == 567,
                                "dispatchVector() uses Vector impl mapping");
                    value_free(&out_vector);
                }
            }
        }
    }

    if (rt) runtime_free(rt);
    cleanup_temp(source_path);
}

static int write_temp_zero_code_artifact(char** out_path) {
    if (!out_path) return 0;
    *out_path = NULL;

    char* artifact_path = make_temp_path_with_ext(".tbc");
    if (!artifact_path) return 0;

    FILE* f = fopen(artifact_path, "wb");
    if (!f) {
        free(artifact_path);
        return 0;
    }

    int ok = 1;
    ok = ok && fwrite("TBLC", 1, 4, f) == 4;
    ok = ok && write_u32_le_file(f, 7u);   // artifact version
    ok = ok && write_u32_le_file(f, 0u);   // typecheck flags
    ok = ok && write_u32_le_file(f, 0u);   // dependency_count
    ok = ok && write_u32_le_file(f, 0u);   // interface_dispatch_count
    ok = ok && write_u32_le_file(f, 1u);   // function_count
    ok = ok && write_i32_le_file(f, 0);    // main index
    ok = ok && fwrite("\0", 1, 1, f) == 1; // has init function
    ok = ok && write_len_string_file(f, "main");
    ok = ok && write_len_string_file(f, "malformed.tblo");
    ok = ok && fputc(0, f) != EOF;         // is_async
    ok = ok && write_i32_le_file(f, -1);   // defer_handler_ip
    ok = ok && write_i32_le_file(f, -1);   // defer_return_slot
    ok = ok && write_u32_le_file(f, 0u);   // param_count
    ok = ok && write_u32_le_file(f, 0u);   // local_count
    ok = ok && write_u32_le_file(f, 0u);   // capture_count
    ok = ok && write_u32_le_file(f, 0u);   // code_count
    ok = ok && write_u32_le_file(f, 0u);   // debug_count
    ok = ok && write_u32_le_file(f, 0u);   // const_count

    fclose(f);

    if (!ok) {
        remove(artifact_path);
        free(artifact_path);
        return 0;
    }

    *out_path = artifact_path;
    return 1;
}

static int write_temp_bad_signature_artifact(char** out_path) {
    if (!out_path) return 0;
    *out_path = NULL;

    char* artifact_path = make_temp_path_with_ext(".tbc");
    if (!artifact_path) return 0;

    FILE* f = fopen(artifact_path, "wb");
    if (!f) {
        free(artifact_path);
        return 0;
    }

    int ok = 1;
    unsigned char return_op = (unsigned char)OP_RET;
    ok = ok && fwrite("TBLC", 1, 4, f) == 4;
    ok = ok && write_u32_le_file(f, 7u);   // artifact version
    ok = ok && write_u32_le_file(f, 0u);   // typecheck flags
    ok = ok && write_u32_le_file(f, 0u);   // dependency_count
    ok = ok && write_u32_le_file(f, 0u);   // interface_dispatch_count
    ok = ok && write_u32_le_file(f, 1u);   // function_count
    ok = ok && write_i32_le_file(f, 0);    // main index
    ok = ok && fwrite("\0", 1, 1, f) == 1; // has init function
    ok = ok && write_len_string_file(f, "main");
    ok = ok && write_len_string_file(f, "bad_signature.tblo");
    ok = ok && fputc(0, f) != EOF;         // is_async
    ok = ok && write_i32_le_file(f, -1);   // defer_handler_ip
    ok = ok && write_i32_le_file(f, -1);   // defer_return_slot
    ok = ok && write_u32_le_file(f, 2u);   // param_count
    ok = ok && write_u32_le_file(f, 1u);   // local_count (invalid: locals < params)
    ok = ok && write_u32_le_file(f, 0u);   // capture_count
    ok = ok && write_u32_le_file(f, 1u);   // code_count
    ok = ok && fwrite(&return_op, 1, 1, f) == 1;
    ok = ok && write_u32_le_file(f, 1u);   // debug_count
    ok = ok && write_i32_le_file(f, 1);    // line number
    ok = ok && write_u32_le_file(f, 0u);   // const_count

    fclose(f);

    if (!ok) {
        remove(artifact_path);
        free(artifact_path);
        return 0;
    }

    *out_path = artifact_path;
    return 1;
}

static void test_malformed_artifact_rejected_without_crash(void) {
    printf("Testing malformed artifact rejection (no crash)...\n");

    char* artifact_path = NULL;
    int wrote = write_temp_zero_code_artifact(&artifact_path);
    TEST_ASSERT(wrote, "Malformed artifact fixture written");
    if (!wrote || !artifact_path) return;

    Runtime* rt = runtime_create(artifact_path);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime for malformed artifact");
    if (!rt) {
        cleanup_temp(artifact_path);
        return;
    }

    TEST_ASSERT(runtime_has_error(rt), "Malformed artifact is rejected during load");

    const char* err = runtime_get_error(rt);
    TEST_ASSERT(err != NULL && err[0] != '\0', "Malformed artifact rejection reports an error");
    TEST_ASSERT(err && strstr(err, "Invalid artifact") != NULL, "Malformed artifact error includes semantic validation context");

    runtime_free(rt);
    cleanup_temp(artifact_path);
}

static void test_malformed_artifact_signature_rejected_at_load(void) {
    printf("Testing malformed artifact signature rejection at load...\n");

    char* artifact_path = NULL;
    int wrote = write_temp_bad_signature_artifact(&artifact_path);
    TEST_ASSERT(wrote, "Malformed signature artifact fixture written");
    if (!wrote || !artifact_path) return;

    Runtime* rt = runtime_create(artifact_path);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime for malformed signature artifact");
    if (!rt) {
        cleanup_temp(artifact_path);
        return;
    }

    TEST_ASSERT(runtime_has_error(rt), "Malformed signature artifact is rejected during load");
    if (runtime_has_error(rt)) {
        const char* err = runtime_get_error(rt);
        TEST_ASSERT(err != NULL && err[0] != '\0', "Malformed signature artifact reports an error");
        TEST_ASSERT(err && strstr(err, "local_count") != NULL, "Malformed signature error reports invalid local/param layout");
    }

    runtime_free(rt);
    cleanup_temp(artifact_path);
}

static void test_artifact_load_injected_oom_recovery(void) {
    printf("Testing artifact load injected OOM recovery...\n");

    const char* source =
        "func main(): void {\n"
        "    println(7);\n"
        "}\n";
    char* source_path = write_temp_vml(source);
    char* artifact_path = NULL;
    uint8_t* artifact_bytes = NULL;
    size_t artifact_size = 0;
    TEST_ASSERT(source_path != NULL, "Artifact OOM recovery source fixture written");
    if (!source_path) return;

    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact OOM recovery fixture written");
    if (!artifact_ready || !artifact_path) {
        cleanup_temp(source_path);
        return;
    }

    int bytes_ready = read_file_bytes_all(artifact_path, &artifact_bytes, &artifact_size);
    TEST_ASSERT(bytes_ready, "Artifact OOM recovery bytes fixture loaded");
    if (!bytes_ready || !artifact_bytes) {
        cleanup_temp(artifact_path);
        cleanup_temp(source_path);
        return;
    }

    int saw_file_oom = 0;
    int saw_bytes_oom = 0;
    int file_succeeded = 0;
    int bytes_succeeded = 0;
    int failed = 0;

    for (size_t fail_after = 0; fail_after < 2048; fail_after++) {
        LoadedBytecodeArtifact loaded;
        safe_alloc_set_fail_after(fail_after);
        int ok = artifact_load_file(artifact_path, &loaded, NULL, 0);
        safe_alloc_clear_fail_after();

        if (!ok) {
            char err[256];
            safe_alloc_set_fail_after(fail_after);
            ok = artifact_load_file(artifact_path, &loaded, err, sizeof(err));
            safe_alloc_clear_fail_after();
            if (ok || !message_is_oom(err)) {
                failed = 1;
                break;
            }
            saw_file_oom++;
            continue;
        }

        artifact_loaded_free(&loaded);
        file_succeeded = 1;
        break;
    }

    for (size_t fail_after = 0; !failed && fail_after < 2048; fail_after++) {
        LoadedBytecodeArtifact loaded;
        char err[256];
        safe_alloc_set_fail_after(fail_after);
        int ok = artifact_load_bytes(artifact_bytes, artifact_size, &loaded, err, sizeof(err));
        safe_alloc_clear_fail_after();

        if (!ok) {
            if (!message_is_oom(err)) {
                failed = 1;
                break;
            }
            saw_bytes_oom++;
            continue;
        }

        artifact_loaded_free(&loaded);
        bytes_succeeded = 1;
        break;
    }

    TEST_ASSERT(!failed, "artifact_load_file/load_bytes turn injected OOM into recoverable errors");
    TEST_ASSERT(saw_file_oom > 0, "artifact_load_file exercised at least one injected OOM path");
    TEST_ASSERT(file_succeeded, "artifact_load_file still succeeds once failpoint is past load-time allocations");
    TEST_ASSERT(saw_bytes_oom > 0, "artifact_load_bytes exercised at least one injected OOM path");
    TEST_ASSERT(bytes_succeeded, "artifact_load_bytes still succeeds once failpoint is past parse-time allocations");

    free(artifact_bytes);
    cleanup_temp(artifact_path);
    cleanup_temp(source_path);
}

static void test_runtime_capability_denials(void) {
    printf("Testing runtime capability denials...\n");

    const char* source =
        "func expectPerm(err: Error?, label: string): void {\n"
        "    if (err == nil) {\n"
        "        panic(label + \" expected permission error\");\n"
        "    }\n"
        "    if (err.code != ERR_PERMISSION) {\n"
        "        panic(label + \" wrong error code\");\n"
        "    }\n"
        "}\n"
        "func main(): void {\n"
        "    var net = tcpConnect(\"127.0.0.1\", 80);\n"
        "    expectPerm(net.1, \"network\");\n"
        "\n"
        "    var args: array<string> = [];\n"
        "    var proc = processSpawn(\"echo\", args, false, false);\n"
        "    expectPerm(proc.1, \"process\");\n"
        "\n"
        "    var db = sqliteOpen(\":memory:\");\n"
        "    expectPerm(db.1, \"sqlite\");\n"
        "\n"
        "    var ch = syncChannelCreate(1);\n"
        "    expectPerm(ch.1, \"threading\");\n"
        "\n"
        "    var file_r = readBytes(\"capability_probe.txt\");\n"
        "    expectPerm(file_r.1, \"file\");\n"
        "}\n";

    char* source_path = write_temp_vml(source);
    TEST_ASSERT(source_path != NULL, "Temp source file created for capability denial test");
    if (!source_path) return;

    RuntimeOptions options = {0};
    options.capabilities.deny_file_io = true;
    options.capabilities.deny_network = true;
    options.capabilities.deny_process = true;
    options.capabilities.deny_sqlite = true;
    options.capabilities.deny_threading = true;

    Runtime* rt = runtime_create_with_options(source_path, options);
    TEST_ASSERT(rt != NULL, "runtime_create_with_options returns Runtime for capability denial test");
    TEST_ASSERT(rt && !runtime_has_error(rt), "runtime_create_with_options has no load error for capability denial test");
    if (rt && !runtime_has_error(rt)) {
        int rc = runtime_run(rt);
        TEST_ASSERT(rc == 0, "Capability-denied program runs without panic");
        TEST_ASSERT(!runtime_has_error(rt), "Capability-denied program reports no runtime error");
    }

    if (rt) runtime_free(rt);
    cleanup_temp(source_path);
}

static void test_runtime_resource_limits(void) {
    printf("Testing runtime file/socket resource limits...\n");

    {
        const char* file_limit_source =
            "func expectLimit(err: Error?, label: string): void {\n"
            "    if (err == nil || err.code != ERR_LIMIT) {\n"
            "        panic(label + \" expected ERR_LIMIT\");\n"
            "    }\n"
            "}\n"
            "func expectOk(err: Error?, label: string): void {\n"
            "    if (err != nil) {\n"
            "        panic(label + \" unexpected error\");\n"
            "    }\n"
            "}\n"
            "func main(): void {\n"
            "    var a = file_open(\"limit_file_a.tmp\", \"wb\");\n"
            "    expectOk(a.1, \"open-a\");\n"
            "    var b = file_open(\"limit_file_b.tmp\", \"wb\");\n"
            "    expectLimit(b.1, \"open-b\");\n"
            "    file_close(a.0);\n"
            "    var c = file_open(\"limit_file_c.tmp\", \"wb\");\n"
            "    expectOk(c.1, \"open-c\");\n"
            "    file_close(c.0);\n"
            "    delete(\"limit_file_a.tmp\");\n"
            "    delete(\"limit_file_b.tmp\");\n"
            "    delete(\"limit_file_c.tmp\");\n"
            "}\n";

        char* file_limit_path = write_temp_vml(file_limit_source);
        TEST_ASSERT(file_limit_path != NULL, "Temp source file created for file limit test");
        if (file_limit_path) {
            RuntimeOptions options = {0};
            options.max_open_files = 1;

            Runtime* rt = runtime_create_with_options(file_limit_path, options);
            TEST_ASSERT(rt != NULL, "runtime_create_with_options returns Runtime for file limit test");
            TEST_ASSERT(rt && !runtime_has_error(rt), "file limit test runtime has no load error");
            if (rt && !runtime_has_error(rt)) {
                int rc = runtime_run(rt);
                TEST_ASSERT(rc == 0, "file limit program exits successfully");
                TEST_ASSERT(!runtime_has_error(rt), "file limit program reports no runtime error");
            }
            if (rt) runtime_free(rt);

            cleanup_temp(file_limit_path);
        }
    }

    {
        const char* file_unlimited_source =
            "func expectOk(err: Error?, label: string): void {\n"
            "    if (err != nil) {\n"
            "        panic(label + \" unexpected error\");\n"
            "    }\n"
            "}\n"
            "func main(): void {\n"
            "    var a = file_open(\"nolimit_file_a.tmp\", \"wb\");\n"
            "    expectOk(a.1, \"open-a\");\n"
            "    var b = file_open(\"nolimit_file_b.tmp\", \"wb\");\n"
            "    expectOk(b.1, \"open-b\");\n"
            "    file_close(a.0);\n"
            "    file_close(b.0);\n"
            "    delete(\"nolimit_file_a.tmp\");\n"
            "    delete(\"nolimit_file_b.tmp\");\n"
            "}\n";

        char* file_unlimited_path = write_temp_vml(file_unlimited_source);
        TEST_ASSERT(file_unlimited_path != NULL, "Temp source file created for default-unlimited file test");
        if (file_unlimited_path) {
            RuntimeOptions options = {0};
            options.max_open_files = 0;

            Runtime* rt = runtime_create_with_options(file_unlimited_path, options);
            TEST_ASSERT(rt != NULL, "runtime_create_with_options returns Runtime for default-unlimited file test");
            TEST_ASSERT(rt && !runtime_has_error(rt), "default-unlimited file test runtime has no load error");
            if (rt && !runtime_has_error(rt)) {
                int rc = runtime_run(rt);
                TEST_ASSERT(rc == 0, "default-unlimited file program exits successfully");
                TEST_ASSERT(!runtime_has_error(rt), "default-unlimited file program reports no runtime error");
            }
            if (rt) runtime_free(rt);

            cleanup_temp(file_unlimited_path);
        }
    }

    {
        const char* socket_limit_source =
            "func expectLimit(err: Error?, label: string): void {\n"
            "    if (err == nil || err.code != ERR_LIMIT) {\n"
            "        panic(label + \" expected ERR_LIMIT\");\n"
            "    }\n"
            "}\n"
            "func expectOk(err: Error?, label: string): void {\n"
            "    if (err != nil) {\n"
            "        panic(label + \" unexpected error\");\n"
            "    }\n"
            "}\n"
            "func main(): void {\n"
            "    var listener = tcpListen(\"127.0.0.1\", 0);\n"
            "    expectOk(listener.1, \"listen-1\");\n"
            "    var conn = tcpConnect(\"127.0.0.1\", 80);\n"
            "    expectLimit(conn.1, \"connect-limit\");\n"
            "    tcpClose(listener.0);\n"
            "    var listener2 = tcpListen(\"127.0.0.1\", 0);\n"
            "    expectOk(listener2.1, \"listen-2\");\n"
            "    tcpClose(listener2.0);\n"
            "}\n";

        char* socket_limit_path = write_temp_vml(socket_limit_source);
        TEST_ASSERT(socket_limit_path != NULL, "Temp source file created for socket limit test");
        if (socket_limit_path) {
            RuntimeOptions options = {0};
            options.max_open_sockets = 1;

            Runtime* rt = runtime_create_with_options(socket_limit_path, options);
            TEST_ASSERT(rt != NULL, "runtime_create_with_options returns Runtime for socket limit test");
            TEST_ASSERT(rt && !runtime_has_error(rt), "socket limit test runtime has no load error");
            if (rt && !runtime_has_error(rt)) {
                int rc = runtime_run(rt);
                TEST_ASSERT(rc == 0, "socket limit program exits successfully");
                TEST_ASSERT(!runtime_has_error(rt), "socket limit program reports no runtime error");
            }
            if (rt) runtime_free(rt);

            cleanup_temp(socket_limit_path);
        }
    }
}

static void test_async_sleep_scheduler_progress(void) {
    printf("Testing asyncSleep scheduler progress...\n");

    const char* source =
        "async func delayed(): int {\n"
        "    await asyncSleep(2);\n"
        "    return 7;\n"
        "}\n"
        "func main(): void {\n"
        "    var task = delayed();\n"
        "    while (!futureIsReady(task)) {\n"
        "    }\n"
        "    if (futureGet(task) != 7) {\n"
        "        panic(\"async sleep mismatch\");\n"
        "    }\n"
        "}\n";

    char* source_path = write_temp_vml(source);
    TEST_ASSERT(source_path != NULL, "Temp source file created for asyncSleep scheduler test");
    if (!source_path) return;

    Runtime* rt = runtime_create(source_path);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime for asyncSleep scheduler test");
    TEST_ASSERT(rt && !runtime_has_error(rt), "asyncSleep scheduler test runtime has no load error");
    if (rt && !runtime_has_error(rt)) {
        rt->vm->config.max_instructions = 50000000;
        int rc = runtime_run(rt);
        TEST_ASSERT(rc == 0, "asyncSleep scheduler test program exits successfully");
        TEST_ASSERT(!runtime_has_error(rt), "asyncSleep scheduler test reports no runtime error");
    }
    if (rt) runtime_free(rt);

    cleanup_temp(source_path);
}

static void assert_async_vm_queues_drained(Runtime* rt, const char* label) {
    char message[160];

    snprintf(message, sizeof(message), "%s leaves ready task queue empty", label);
    TEST_ASSERT(rt && rt->vm && rt->vm->ready_tasks_head == NULL && rt->vm->ready_tasks_tail == NULL, message);

    snprintf(message, sizeof(message), "%s leaves future waiter queue empty", label);
    TEST_ASSERT(rt && rt->vm && rt->vm->future_waiters == NULL, message);

    snprintf(message, sizeof(message), "%s leaves timer waiter queue empty", label);
    TEST_ASSERT(rt && rt->vm && rt->vm->timer_waiters == NULL, message);

    snprintf(message, sizeof(message), "%s leaves poll waiter queue empty", label);
    TEST_ASSERT(rt && rt->vm && rt->vm->poll_waiters == NULL, message);
}

static void assert_async_vm_queues_drained_vm(VM* vm, const char* label) {
    char message[160];

    snprintf(message, sizeof(message), "%s leaves ready task queue empty", label);
    TEST_ASSERT(vm && vm->ready_tasks_head == NULL && vm->ready_tasks_tail == NULL, message);

    snprintf(message, sizeof(message), "%s leaves future waiter queue empty", label);
    TEST_ASSERT(vm && vm->future_waiters == NULL, message);

    snprintf(message, sizeof(message), "%s leaves timer waiter queue empty", label);
    TEST_ASSERT(vm && vm->timer_waiters == NULL, message);

    snprintf(message, sizeof(message), "%s leaves poll waiter queue empty", label);
    TEST_ASSERT(vm && vm->poll_waiters == NULL, message);
}

static void assert_async_pending_queue_shape(Runtime* rt,
                                             const char* label,
                                             int expected_ready_tasks,
                                             int expected_future_entries,
                                             int expected_future_tasks,
                                             int expected_timer_waiters,
                                             int expected_poll_waiters) {
    char message[192];
    VM* vm = rt ? rt->vm : NULL;

    TEST_ASSERT(vm != NULL, label);
    if (!vm) return;

    snprintf(message, sizeof(message), "%s leaves expected ready task count before vm_free", label);
    TEST_ASSERT(vm_debug_ready_task_count(vm) == expected_ready_tasks, message);

    snprintf(message, sizeof(message), "%s leaves expected future waiter entry count before vm_free", label);
    TEST_ASSERT(vm_debug_future_waiter_entry_count(vm) == expected_future_entries, message);

    snprintf(message, sizeof(message), "%s leaves expected future waiter task count before vm_free", label);
    TEST_ASSERT(vm_debug_future_waiter_task_count(vm) == expected_future_tasks, message);

    snprintf(message, sizeof(message), "%s leaves expected timer waiter count before vm_free", label);
    TEST_ASSERT(vm_debug_timer_waiter_count(vm) == expected_timer_waiters, message);

    snprintf(message, sizeof(message), "%s leaves expected poll waiter count before vm_free", label);
    TEST_ASSERT(vm_debug_poll_waiter_count(vm) == expected_poll_waiters, message);
}

static void assert_global_future_panicked(Runtime* rt,
                                          const char* global_name,
                                          const char* label,
                                          const char* expected_message_substr) {
    char message[192];
    Value global;
    ObjRecord* future = NULL;
    const char* panic_message = NULL;

    TEST_ASSERT(rt && rt->vm && global_name, label);
    if (!rt || !rt->vm || !global_name) return;

    global = vm_get_global(rt->vm, global_name);

    snprintf(message, sizeof(message), "%s exposes global future '%s'", label, global_name);
    TEST_ASSERT(value_is_future(&global), message);
    if (!value_is_future(&global)) return;

    future = value_get_record_obj(&global);

    snprintf(message, sizeof(message), "%s global future '%s' is ready", label, global_name);
    TEST_ASSERT(obj_future_is_ready(future), message);

    snprintf(message, sizeof(message), "%s global future '%s' is panicked", label, global_name);
    TEST_ASSERT(obj_future_is_panicked(future), message);

    panic_message = obj_future_get_panic_message(future);
    snprintf(message, sizeof(message), "%s global future '%s' panic message is available", label, global_name);
    TEST_ASSERT(panic_message != NULL, message);

    if (expected_message_substr) {
        snprintf(message, sizeof(message), "%s global future '%s' panic includes expected text", label, global_name);
        TEST_ASSERT(panic_message && strstr(panic_message, expected_message_substr) != NULL, message);
    }
}

static void teardown_runtime_after_manual_vm_free(Runtime* rt, const char* label) {
    if (!rt) return;

    VM* vm = rt->vm;
    TEST_ASSERT(vm != NULL, label);
    if (vm) {
        vm_free(vm);
        assert_async_vm_queues_drained_vm(vm, label);
        free(vm);
        rt->vm = NULL;
        rt->vm_initialized = false;
    }

    runtime_free(rt);
}

static const char* async_timeout_scheduler_source(void) {
    return
        "record AsyncAwaitWithTimeoutResult {\n"
        "    ready: bool,\n"
        "    timedOut: bool,\n"
        "    value: any,\n"
        "    error: Error?\n"
        "};\n"
        "func asyncAwaitWithTimeoutTimedOut(): AsyncAwaitWithTimeoutResult {\n"
        "    return { ready: false, timedOut: true, value: nil, error: nil };\n"
        "}\n"
        "async func asyncAwaitWithTimeout[T](task: Future[T], timeoutMs: int, pollIntervalMs: int): AsyncAwaitWithTimeoutResult {\n"
        "    var remaining: int = timeoutMs;\n"
        "    while (true) {\n"
        "        if (futureIsReady(task)) {\n"
        "            return { ready: true, timedOut: false, value: (await task) as any, error: nil };\n"
        "        }\n"
        "        if (remaining == 0) {\n"
        "            return asyncAwaitWithTimeoutTimedOut();\n"
        "        }\n"
        "        var step: int = pollIntervalMs;\n"
        "        if (step > remaining) {\n"
        "            step = remaining;\n"
        "        }\n"
        "        await asyncSleep(step);\n"
        "        remaining = remaining - step;\n"
        "    }\n"
        "}\n"
        "async func completeAfter(task: Future[int], value: int, delayMs: int): bool {\n"
        "    await asyncSleep(delayMs);\n"
        "    return futureComplete(task, value);\n"
        "}\n"
        "async func run(): int {\n"
        "    var slow = futurePending<int>();\n"
        "    var resolver = completeAfter(slow, 19, 4);\n"
        "    var timed = await asyncAwaitWithTimeout(slow, 1, 1);\n"
        "    if (timed.ready || !timed.timedOut || timed.error != nil || timed.value != nil) {\n"
        "        return -1;\n"
        "    }\n"
        "    while (!futureIsReady(resolver)) {\n"
        "    }\n"
        "    if (!futureGet(resolver)) {\n"
        "        return -2;\n"
        "    }\n"
        "    while (!futureIsReady(slow)) {\n"
        "    }\n"
        "    return futureGet(slow);\n"
        "}\n"
        "func main(): void {\n"
        "    var task = run();\n"
        "    while (!futureIsReady(task)) {\n"
        "    }\n"
        "    if (futureGet(task) != 19) {\n"
        "        panic(\"async timeout scheduler mismatch\");\n"
        "    }\n"
        "}\n";
}

static void test_async_timeout_scheduler_progress(void) {
    printf("Testing async timeout scheduler progress...\n");

    char* source_path = write_temp_vml(async_timeout_scheduler_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for async timeout scheduler test");
    if (!source_path) return;

    Runtime* rt = runtime_create(source_path);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime for async timeout scheduler test");
    TEST_ASSERT(rt && !runtime_has_error(rt), "async timeout scheduler test runtime has no load error");
    if (rt && !runtime_has_error(rt)) {
        rt->vm->config.max_instructions = 50000000;
        int rc = runtime_run(rt);
        TEST_ASSERT(rc == 0, "async timeout scheduler test program exits successfully");
        TEST_ASSERT(!runtime_has_error(rt), "async timeout scheduler test reports no runtime error");
        assert_async_vm_queues_drained(rt, "Async timeout scheduler test");
    }
    if (rt) runtime_free(rt);

    cleanup_temp(source_path);
}

static void test_async_channel_scheduler_progress(void) {
    printf("Testing async channel scheduler progress...\n");

    const char* source =
        "async func recvLater(channelId: int): (any, Error?) {\n"
        "    return await asyncChannelRecv(channelId);\n"
        "}\n"
        "func main(): void {\n"
        "    var created = syncChannelCreate(1);\n"
        "    if (created.1 != nil) {\n"
        "        panic(\"channel create failed\");\n"
        "    }\n"
        "    var channelId = created.0 as int;\n"
        "    var task = recvLater(channelId);\n"
        "    var spins = 0;\n"
        "    while (!futureIsReady(task)) {\n"
        "        spins = spins + 1;\n"
        "        if (spins == 100) {\n"
        "            var sent = syncChannelSend(channelId, 7 as any, 5000);\n"
        "            if (!(sent.0 as bool)) {\n"
        "                panic(\"sync send failed\");\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    var recv = futureGet(task);\n"
        "    if (recv.1 != nil || recv.0 as int != 7) {\n"
        "        panic(\"async channel mismatch\");\n"
        "    }\n"
        "}\n";

    char* source_path = NULL;
#ifdef _WIN32
    {
        const char* temp_dir = getenv("TEMP");
        if (!temp_dir) temp_dir = ".";
        source_path = (char*)safe_malloc(640);
        snprintf(source_path,
                 640,
                 "%s\\tablo_runtime_safety_async_channel_%d_%lld.tblo",
                 temp_dir,
                 rand(),
                 (long long)clock());
    }
#else
    {
        const char* temp_dir = getenv("TMPDIR");
        if (!temp_dir) temp_dir = "/tmp";
        source_path = (char*)safe_malloc(640);
        snprintf(source_path,
                 640,
                 "%s/tablo_runtime_safety_async_channel_%d_%lld.tblo",
                 temp_dir,
                 rand(),
                 (long long)clock());
    }
#endif
    TEST_ASSERT(source_path != NULL, "Temp source file created for async channel scheduler test");
    if (!source_path) return;

    int wrote = write_file_all(source_path, source);
    TEST_ASSERT(wrote, "Async channel scheduler test source written");
    if (!wrote) {
        cleanup_temp(source_path);
        return;
    }

    Runtime* rt = runtime_create(source_path);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime for async channel scheduler test");
    TEST_ASSERT(rt && !runtime_has_error(rt), "async channel scheduler test runtime has no load error");
    if (rt && !runtime_has_error(rt)) {
        rt->vm->config.max_instructions = 50000000;
        int rc = runtime_run(rt);
        TEST_ASSERT(rc == 0, "async channel scheduler test program exits successfully");
        TEST_ASSERT(!runtime_has_error(rt), "async channel scheduler test reports no runtime error");
    }
    if (rt) runtime_free(rt);

    cleanup_temp(source_path);
}

static const char* async_panic_cleanup_roundtrip_source(void) {
    return
        "func completeMarker(marker: Future[int]): void {\n"
        "    if (!futureComplete(marker, 1)) {\n"
        "        panic(\"marker complete failed\");\n"
        "    }\n"
        "}\n"
        "async func panicAfterAwait(task: Future[int], marker: Future[int]): int {\n"
        "    defer completeMarker(marker);\n"
        "    var value: int = await task;\n"
        "    if (value != 9) {\n"
        "        panic(\"awaited wrong value\");\n"
        "    }\n"
        "    panic(\"async panic cleanup\");\n"
        "    return value;\n"
        "}\n"
        "func main(): void {\n"
        "    var input = futurePending<int>();\n"
        "    var marker = futurePending<int>();\n"
        "    var task = panicAfterAwait(input, marker);\n"
        "    if (futureIsReady(task) || futureIsReady(marker)) {\n"
        "        panic(\"async panic cleanup future started ready\");\n"
        "    }\n"
        "    if (!futureComplete(input, 9)) {\n"
        "        panic(\"input complete failed\");\n"
        "    }\n"
        "    while (!futureIsReady(task) || !futureIsReady(marker)) {\n"
        "    }\n"
        "    if (futureGet(marker) != 1) {\n"
        "        panic(\"cleanup marker mismatch\");\n"
        "    }\n"
        "    if (!futureIsReady(task)) {\n"
        "        panic(\"panicked future not ready\");\n"
        "    }\n"
        "}\n";
}

static void test_async_panic_cleanup_cache_and_artifact_roundtrip(void) {
    printf("Testing async panic cleanup cache and artifact roundtrip...\n");

    char* source_path = write_temp_vml(async_panic_cleanup_roundtrip_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for async panic cleanup roundtrip test");
    if (!source_path) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(source_path);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime for async panic cleanup roundtrip test");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Probe runtime_create has no error for async panic cleanup roundtrip test");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(source_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for async panic cleanup roundtrip test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        remove(cache_path_copy);
        TEST_ASSERT(!file_exists(cache_path_copy), "Async panic cleanup roundtrip probe cache artifact removed before first run");
    }

    Runtime* rt_first = runtime_create(source_path);
    TEST_ASSERT(rt_first != NULL, "First runtime_create returns Runtime for async panic cleanup roundtrip test");
    TEST_ASSERT(rt_first && !runtime_has_error(rt_first), "First runtime_create has no error for async panic cleanup roundtrip test");
    if (rt_first && !runtime_has_error(rt_first)) {
        rt_first->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "Async panic cleanup roundtrip first load mode is source");
        int rc = runtime_run(rt_first);
        TEST_ASSERT(rc == 0, "Async panic cleanup roundtrip first runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_first), "Async panic cleanup roundtrip first runtime_run has no runtime error");
        assert_async_vm_queues_drained(rt_first, "Async panic cleanup roundtrip source run");
    }
    if (rt_first) runtime_free(rt_first);

    if (cache_path_copy) {
        TEST_ASSERT(file_exists(cache_path_copy), "Async panic cleanup roundtrip cache artifact is written after first run");
    }

    Runtime* rt_second = runtime_create(source_path);
    TEST_ASSERT(rt_second != NULL, "Second runtime_create returns Runtime for async panic cleanup roundtrip test");
    TEST_ASSERT(rt_second && !runtime_has_error(rt_second), "Second runtime_create has no error for async panic cleanup roundtrip test");
    if (rt_second && !runtime_has_error(rt_second)) {
        rt_second->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Async panic cleanup roundtrip second load mode is cache");
        int rc = runtime_run(rt_second);
        TEST_ASSERT(rc == 0, "Async panic cleanup roundtrip second runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_second), "Async panic cleanup roundtrip second runtime_run has no runtime error");
        assert_async_vm_queues_drained(rt_second, "Async panic cleanup roundtrip cache run");
    }
    if (rt_second) runtime_free(rt_second);

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for async panic cleanup roundtrip test");
    if (artifact_ready && artifact_path) {
        Runtime* rt_artifact = runtime_create(artifact_path);
        TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime for async panic cleanup roundtrip test");
        TEST_ASSERT(rt_artifact && !runtime_has_error(rt_artifact), "Artifact runtime_create has no error for async panic cleanup roundtrip test");
        if (rt_artifact && !runtime_has_error(rt_artifact)) {
            rt_artifact->vm->config.max_instructions = 50000000;
            TEST_ASSERT(runtime_get_load_mode(rt_artifact) == RUNTIME_LOAD_ARTIFACT, "Async panic cleanup roundtrip artifact load mode is artifact");
            int rc = runtime_run(rt_artifact);
            TEST_ASSERT(rc == 0, "Async panic cleanup roundtrip artifact runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt_artifact), "Async panic cleanup roundtrip artifact runtime_run has no runtime error");
            assert_async_vm_queues_drained(rt_artifact, "Async panic cleanup roundtrip artifact run");
        }
        if (rt_artifact) runtime_free(rt_artifact);
        cleanup_temp(artifact_path);
    }

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(source_path);
}

static const char* async_panic_observation_roundtrip_source(void) {
    return
        "async func panicAfterAwait(task: Future[int]): int {\n"
        "    var value: int = await task;\n"
        "    if (value != 9) {\n"
        "        panic(\"awaited wrong value\");\n"
        "    }\n"
        "    panic(\"async panic observe\");\n"
        "    return value;\n"
        "}\n"
        "func main(): void {\n"
        "    var input = futurePending<int>();\n"
        "    var task = panicAfterAwait(input);\n"
        "    if (!futureComplete(input, 9)) {\n"
        "        panic(\"input complete failed\");\n"
        "    }\n"
        "    while (!futureIsReady(task)) {\n"
        "    }\n"
        "    println(futureGet(task));\n"
        "}\n";
}

static void test_async_panic_observation_cache_and_artifact_roundtrip(void) {
    printf("Testing async panic observation cache and artifact roundtrip...\n");

    char* source_path = write_temp_vml(async_panic_observation_roundtrip_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for async panic observation roundtrip test");
    if (!source_path) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(source_path);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime for async panic observation roundtrip test");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Probe runtime_create has no error for async panic observation roundtrip test");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(source_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for async panic observation roundtrip test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        remove(cache_path_copy);
        TEST_ASSERT(!file_exists(cache_path_copy), "Async panic observation roundtrip probe cache artifact removed before first run");
    }

    Runtime* rt_first = runtime_create(source_path);
    TEST_ASSERT(rt_first != NULL, "First runtime_create returns Runtime for async panic observation roundtrip test");
    TEST_ASSERT(rt_first && !runtime_has_error(rt_first), "First runtime_create has no error for async panic observation roundtrip test");
    if (rt_first && !runtime_has_error(rt_first)) {
        rt_first->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "Async panic observation roundtrip first load mode is source");
        runtime_run(rt_first);
        TEST_ASSERT(runtime_has_error(rt_first), "Async panic observation roundtrip first runtime_run reports runtime error");
        if (runtime_has_error(rt_first)) {
            const char* err = runtime_get_error(rt_first);
            TEST_ASSERT(err && strstr(err, "Panic: async panic observe") != NULL, "Async panic observation source runtime error includes panic message");
            TEST_ASSERT(err && strstr(err, "Stack trace:") != NULL, "Async panic observation source runtime error includes stack trace header");
            TEST_ASSERT(err && strstr(err, "at panicAfterAwait") != NULL, "Async panic observation source runtime error includes async frame");
        }
        assert_async_vm_queues_drained(rt_first, "Async panic observation roundtrip source run");
    }
    if (rt_first) runtime_free(rt_first);

    if (cache_path_copy) {
        TEST_ASSERT(file_exists(cache_path_copy), "Async panic observation roundtrip cache artifact is written after first run");
    }

    Runtime* rt_second = runtime_create(source_path);
    TEST_ASSERT(rt_second != NULL, "Second runtime_create returns Runtime for async panic observation roundtrip test");
    TEST_ASSERT(rt_second && !runtime_has_error(rt_second), "Second runtime_create has no error for async panic observation roundtrip test");
    if (rt_second && !runtime_has_error(rt_second)) {
        rt_second->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Async panic observation roundtrip second load mode is cache");
        runtime_run(rt_second);
        TEST_ASSERT(runtime_has_error(rt_second), "Async panic observation roundtrip second runtime_run reports runtime error");
        if (runtime_has_error(rt_second)) {
            const char* err = runtime_get_error(rt_second);
            TEST_ASSERT(err && strstr(err, "Panic: async panic observe") != NULL, "Async panic observation cache runtime error includes panic message");
            TEST_ASSERT(err && strstr(err, "Stack trace:") != NULL, "Async panic observation cache runtime error includes stack trace header");
            TEST_ASSERT(err && strstr(err, "at panicAfterAwait") != NULL, "Async panic observation cache runtime error includes async frame");
        }
        assert_async_vm_queues_drained(rt_second, "Async panic observation roundtrip cache run");
    }
    if (rt_second) runtime_free(rt_second);

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for async panic observation roundtrip test");
    if (artifact_ready && artifact_path) {
        Runtime* rt_artifact = runtime_create(artifact_path);
        TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime for async panic observation roundtrip test");
        TEST_ASSERT(rt_artifact && !runtime_has_error(rt_artifact), "Artifact runtime_create has no error for async panic observation roundtrip test");
        if (rt_artifact && !runtime_has_error(rt_artifact)) {
            rt_artifact->vm->config.max_instructions = 50000000;
            TEST_ASSERT(runtime_get_load_mode(rt_artifact) == RUNTIME_LOAD_ARTIFACT, "Async panic observation roundtrip artifact load mode is artifact");
            runtime_run(rt_artifact);
            TEST_ASSERT(runtime_has_error(rt_artifact), "Async panic observation roundtrip artifact runtime_run reports runtime error");
            if (runtime_has_error(rt_artifact)) {
                const char* err = runtime_get_error(rt_artifact);
                TEST_ASSERT(err && strstr(err, "Panic: async panic observe") != NULL, "Async panic observation artifact runtime error includes panic message");
                TEST_ASSERT(err && strstr(err, "Stack trace:") != NULL, "Async panic observation artifact runtime error includes stack trace header");
                TEST_ASSERT(err && strstr(err, "at panicAfterAwait") != NULL, "Async panic observation artifact runtime error includes async frame");
            }
            assert_async_vm_queues_drained(rt_artifact, "Async panic observation roundtrip artifact run");
        }
        if (rt_artifact) runtime_free(rt_artifact);
        cleanup_temp(artifact_path);
    }

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(source_path);
}

static const char* async_channel_panic_cleanup_roundtrip_source(void) {
    return
        "async func panicAfterRecv(channelId: int): int {\n"
        "    var recv: (any, Error?) = await asyncChannelRecv(channelId);\n"
        "    if (recv.1 != nil) {\n"
        "        panic(\"recv error\");\n"
        "    }\n"
        "    if ((recv.0 as int) != 7) {\n"
        "        panic(\"recv value mismatch\");\n"
        "    }\n"
        "    panic(\"async channel panic\");\n"
        "    return 0;\n"
        "}\n"
        "func main(): void {\n"
        "    var created = syncChannelCreate(1);\n"
        "    if (created.1 != nil) {\n"
        "        panic(\"channel create failed\");\n"
        "    }\n"
        "    var channelId = created.0 as int;\n"
        "    var task = panicAfterRecv(channelId);\n"
        "    if (futureIsReady(task)) {\n"
        "        panic(\"channel panic task started ready\");\n"
        "    }\n"
        "    var sent = syncChannelSend(channelId, 7 as any, 5000);\n"
        "    if (!(sent.0 as bool)) {\n"
        "        panic(\"channel panic send failed\");\n"
        "    }\n"
        "    while (!futureIsReady(task)) {\n"
        "    }\n"
        "}\n";
}

static void test_async_channel_panic_cleanup_cache_and_artifact_roundtrip(void) {
    printf("Testing async channel panic cleanup cache and artifact roundtrip...\n");

    char* source_path = write_temp_vml(async_channel_panic_cleanup_roundtrip_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for async channel panic cleanup roundtrip test");
    if (!source_path) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(source_path);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime for async channel panic cleanup roundtrip test");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Probe runtime_create has no error for async channel panic cleanup roundtrip test");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(source_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for async channel panic cleanup roundtrip test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        remove(cache_path_copy);
        TEST_ASSERT(!file_exists(cache_path_copy), "Async channel panic cleanup roundtrip probe cache artifact removed before first run");
    }

    Runtime* rt_first = runtime_create(source_path);
    TEST_ASSERT(rt_first != NULL, "First runtime_create returns Runtime for async channel panic cleanup roundtrip test");
    TEST_ASSERT(rt_first && !runtime_has_error(rt_first), "First runtime_create has no error for async channel panic cleanup roundtrip test");
    if (rt_first && !runtime_has_error(rt_first)) {
        rt_first->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "Async channel panic cleanup roundtrip first load mode is source");
        int rc = runtime_run(rt_first);
        TEST_ASSERT(rc == 0, "Async channel panic cleanup roundtrip first runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_first), "Async channel panic cleanup roundtrip first runtime_run has no runtime error");
        assert_async_vm_queues_drained(rt_first, "Async channel panic cleanup roundtrip source run");
    }
    if (rt_first) runtime_free(rt_first);

    if (cache_path_copy) {
        TEST_ASSERT(file_exists(cache_path_copy), "Async channel panic cleanup roundtrip cache artifact is written after first run");
    }

    Runtime* rt_second = runtime_create(source_path);
    TEST_ASSERT(rt_second != NULL, "Second runtime_create returns Runtime for async channel panic cleanup roundtrip test");
    TEST_ASSERT(rt_second && !runtime_has_error(rt_second), "Second runtime_create has no error for async channel panic cleanup roundtrip test");
    if (rt_second && !runtime_has_error(rt_second)) {
        rt_second->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Async channel panic cleanup roundtrip second load mode is cache");
        int rc = runtime_run(rt_second);
        TEST_ASSERT(rc == 0, "Async channel panic cleanup roundtrip second runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_second), "Async channel panic cleanup roundtrip second runtime_run has no runtime error");
        assert_async_vm_queues_drained(rt_second, "Async channel panic cleanup roundtrip cache run");
    }
    if (rt_second) runtime_free(rt_second);

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for async channel panic cleanup roundtrip test");
    if (artifact_ready && artifact_path) {
        Runtime* rt_artifact = runtime_create(artifact_path);
        TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime for async channel panic cleanup roundtrip test");
        TEST_ASSERT(rt_artifact && !runtime_has_error(rt_artifact), "Artifact runtime_create has no error for async channel panic cleanup roundtrip test");
        if (rt_artifact && !runtime_has_error(rt_artifact)) {
            rt_artifact->vm->config.max_instructions = 50000000;
            TEST_ASSERT(runtime_get_load_mode(rt_artifact) == RUNTIME_LOAD_ARTIFACT, "Async channel panic cleanup roundtrip artifact load mode is artifact");
            int rc = runtime_run(rt_artifact);
            TEST_ASSERT(rc == 0, "Async channel panic cleanup roundtrip artifact runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt_artifact), "Async channel panic cleanup roundtrip artifact runtime_run has no runtime error");
            assert_async_vm_queues_drained(rt_artifact, "Async channel panic cleanup roundtrip artifact run");
        }
        if (rt_artifact) runtime_free(rt_artifact);
        cleanup_temp(artifact_path);
    }

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(source_path);
}

static const char* async_timeout_roundtrip_source(void) {
    return
        "record AsyncAwaitAnyResult {\n"
        "    index: int,\n"
        "    ready: bool,\n"
        "    timedOut: bool,\n"
        "    value: any,\n"
        "    error: Error?\n"
        "};\n"
        "func asyncAwaitAnyTimedOut(): AsyncAwaitAnyResult {\n"
        "    return { index: -1, ready: false, timedOut: true, value: nil, error: nil };\n"
        "}\n"
        "async func asyncAwaitAnyWithTimeout[T](tasks: array<Future[T]>, timeoutMs: int, pollIntervalMs: int): AsyncAwaitAnyResult {\n"
        "    var remaining: int = timeoutMs;\n"
        "    while (true) {\n"
        "        var i: int = 0;\n"
        "        while (i < len(tasks)) {\n"
        "            if (futureIsReady(tasks[i])) {\n"
        "                return { index: i, ready: true, timedOut: false, value: (await tasks[i]) as any, error: nil };\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (remaining == 0) {\n"
        "            return asyncAwaitAnyTimedOut();\n"
        "        }\n"
        "        var step: int = pollIntervalMs;\n"
        "        if (step > remaining) {\n"
        "            step = remaining;\n"
        "        }\n"
        "        await asyncSleep(step);\n"
        "        remaining = remaining - step;\n"
        "    }\n"
        "}\n"
        "async func completeAfter(task: Future[int], value: int, delayMs: int): bool {\n"
        "    await asyncSleep(delayMs);\n"
        "    return futureComplete(task, value);\n"
        "}\n"
        "func main(): void {\n"
        "    var left = futurePending<int>();\n"
        "    var right = futurePending<int>();\n"
        "    var timed = asyncAwaitAnyWithTimeout([left, right], 1, 1);\n"
        "    while (!futureIsReady(timed)) {\n"
        "    }\n"
        "    var timedResult = futureGet(timed);\n"
        "    if (timedResult.ready || !timedResult.timedOut || timedResult.index != -1 || timedResult.value != nil || timedResult.error != nil) {\n"
        "        panic(\"async any timeout mismatch\");\n"
        "    }\n"
        "    if (futureIsReady(right)) {\n"
        "        panic(\"async any timeout consumed future\");\n"
        "    }\n"
        "    var resolver = completeAfter(right, 23, 4);\n"
        "    while (!futureIsReady(resolver)) {\n"
        "    }\n"
        "    if (!futureGet(resolver)) {\n"
        "        panic(\"async any resolver failed\");\n"
        "    }\n"
        "    var ready = asyncAwaitAnyWithTimeout([left, right], 5, 1);\n"
        "    while (!futureIsReady(ready)) {\n"
        "    }\n"
        "    var readyResult = futureGet(ready);\n"
        "    if (!readyResult.ready || readyResult.timedOut || readyResult.error != nil || readyResult.index != 1) {\n"
        "        panic(\"async any ready flags mismatch\");\n"
        "    }\n"
        "    if ((readyResult.value as int) != 23) {\n"
        "        panic(\"async any ready value mismatch\");\n"
        "    }\n"
        "}\n";
}

static void test_async_timeout_cache_and_artifact_roundtrip(void) {
    printf("Testing async timeout cache and artifact roundtrip...\n");

    char* source_path = write_temp_vml(async_timeout_roundtrip_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for async timeout roundtrip test");
    if (!source_path) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(source_path);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime for async timeout roundtrip test");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Probe runtime_create has no error for async timeout roundtrip test");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(source_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for async timeout roundtrip test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        remove(cache_path_copy);
        TEST_ASSERT(!file_exists(cache_path_copy), "Async timeout roundtrip probe cache artifact removed before first run");
    }

    Runtime* rt_first = runtime_create(source_path);
    TEST_ASSERT(rt_first != NULL, "First runtime_create returns Runtime for async timeout roundtrip test");
    TEST_ASSERT(rt_first && !runtime_has_error(rt_first), "First runtime_create has no error for async timeout roundtrip test");
    if (rt_first && !runtime_has_error(rt_first)) {
        rt_first->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "Async timeout roundtrip first load mode is source");
        int rc = runtime_run(rt_first);
        TEST_ASSERT(rc == 0, "Async timeout roundtrip first runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_first), "Async timeout roundtrip first runtime_run has no runtime error");
        assert_async_vm_queues_drained(rt_first, "Async timeout roundtrip source run");
    }
    if (rt_first) runtime_free(rt_first);

    if (cache_path_copy) {
        TEST_ASSERT(file_exists(cache_path_copy), "Async timeout roundtrip cache artifact is written after first run");
    }

    Runtime* rt_second = runtime_create(source_path);
    TEST_ASSERT(rt_second != NULL, "Second runtime_create returns Runtime for async timeout roundtrip test");
    TEST_ASSERT(rt_second && !runtime_has_error(rt_second), "Second runtime_create has no error for async timeout roundtrip test");
    if (rt_second && !runtime_has_error(rt_second)) {
        rt_second->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Async timeout roundtrip second load mode is cache");
        int rc = runtime_run(rt_second);
        TEST_ASSERT(rc == 0, "Async timeout roundtrip second runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_second), "Async timeout roundtrip second runtime_run has no runtime error");
        assert_async_vm_queues_drained(rt_second, "Async timeout roundtrip cache run");
    }
    if (rt_second) runtime_free(rt_second);

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for async timeout roundtrip test");
    if (artifact_ready && artifact_path) {
        Runtime* rt_artifact = runtime_create(artifact_path);
        TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime for async timeout roundtrip test");
        TEST_ASSERT(rt_artifact && !runtime_has_error(rt_artifact), "Artifact runtime_create has no error for async timeout roundtrip test");
        if (rt_artifact && !runtime_has_error(rt_artifact)) {
            rt_artifact->vm->config.max_instructions = 50000000;
            TEST_ASSERT(runtime_get_load_mode(rt_artifact) == RUNTIME_LOAD_ARTIFACT, "Async timeout roundtrip artifact load mode is artifact");
            int rc = runtime_run(rt_artifact);
            TEST_ASSERT(rc == 0, "Async timeout roundtrip artifact runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt_artifact), "Async timeout roundtrip artifact runtime_run has no runtime error");
            assert_async_vm_queues_drained(rt_artifact, "Async timeout roundtrip artifact run");
        }
        if (rt_artifact) runtime_free(rt_artifact);
        cleanup_temp(artifact_path);
    }

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(source_path);
}

static const char* async_shutdown_sleep_source(void) {
    return
        "async func sleepy(): int {\n"
        "    await asyncSleep(60000);\n"
        "    return 1;\n"
        "}\n"
        "func main(): void {\n"
        "    sleepy();\n"
        "}\n";
}

static void test_runtime_free_cleans_pending_async_sleep_roundtrip(void) {
    printf("Testing runtime_free cleanup for pending async sleep roundtrip...\n");

    char* source_path = write_temp_vml(async_shutdown_sleep_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for pending async sleep shutdown test");
    if (!source_path) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(source_path);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime for pending async sleep shutdown test");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Probe runtime_create has no error for pending async sleep shutdown test");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(source_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for pending async sleep shutdown test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        remove(cache_path_copy);
        TEST_ASSERT(!file_exists(cache_path_copy), "Pending async sleep shutdown probe cache artifact removed before first run");
    }

    Runtime* rt_first = runtime_create(source_path);
    TEST_ASSERT(rt_first != NULL, "First runtime_create returns Runtime for pending async sleep shutdown test");
    TEST_ASSERT(rt_first && !runtime_has_error(rt_first), "First runtime_create has no error for pending async sleep shutdown test");
    if (rt_first && !runtime_has_error(rt_first)) {
        rt_first->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "Pending async sleep shutdown first load mode is source");
        int rc = runtime_run(rt_first);
        TEST_ASSERT(rc == 0, "Pending async sleep shutdown first runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_first), "Pending async sleep shutdown first runtime_run has no runtime error");
        assert_async_pending_queue_shape(rt_first, "Pending async sleep shutdown source run", 0, 1, 1, 1, 0);
    }
    teardown_runtime_after_manual_vm_free(rt_first, "Pending async sleep shutdown source teardown");

    Runtime* rt_second = runtime_create(source_path);
    TEST_ASSERT(rt_second != NULL, "Second runtime_create returns Runtime for pending async sleep shutdown test");
    TEST_ASSERT(rt_second && !runtime_has_error(rt_second), "Second runtime_create has no error for pending async sleep shutdown test");
    if (rt_second && !runtime_has_error(rt_second)) {
        rt_second->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Pending async sleep shutdown second load mode is cache");
        int rc = runtime_run(rt_second);
        TEST_ASSERT(rc == 0, "Pending async sleep shutdown second runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_second), "Pending async sleep shutdown second runtime_run has no runtime error");
        assert_async_pending_queue_shape(rt_second, "Pending async sleep shutdown cache run", 0, 1, 1, 1, 0);
    }
    teardown_runtime_after_manual_vm_free(rt_second, "Pending async sleep shutdown cache teardown");

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for pending async sleep shutdown test");
    if (artifact_ready && artifact_path) {
        Runtime* rt_artifact = runtime_create(artifact_path);
        TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime for pending async sleep shutdown test");
        TEST_ASSERT(rt_artifact && !runtime_has_error(rt_artifact), "Artifact runtime_create has no error for pending async sleep shutdown test");
        if (rt_artifact && !runtime_has_error(rt_artifact)) {
            rt_artifact->vm->config.max_instructions = 50000000;
            TEST_ASSERT(runtime_get_load_mode(rt_artifact) == RUNTIME_LOAD_ARTIFACT, "Pending async sleep shutdown artifact load mode is artifact");
            int rc = runtime_run(rt_artifact);
            TEST_ASSERT(rc == 0, "Pending async sleep shutdown artifact runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt_artifact), "Pending async sleep shutdown artifact runtime_run has no runtime error");
            assert_async_pending_queue_shape(rt_artifact, "Pending async sleep shutdown artifact run", 0, 1, 1, 1, 0);
        }
        teardown_runtime_after_manual_vm_free(rt_artifact, "Pending async sleep shutdown artifact teardown");
        cleanup_temp(artifact_path);
    }

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(source_path);
}

static const char* async_shutdown_channel_source(void) {
    return
        "async func recvLater(channelId: int): int {\n"
        "    var recv: (any, Error?) = await asyncChannelRecv(channelId);\n"
        "    if (recv.1 != nil) {\n"
        "        panic(\"recv error\");\n"
        "    }\n"
        "    return recv.0 as int;\n"
        "}\n"
        "func main(): void {\n"
        "    var created = syncChannelCreate(1);\n"
        "    if (created.1 != nil) {\n"
        "        panic(\"channel create failed\");\n"
        "    }\n"
        "    recvLater(created.0 as int);\n"
        "}\n";
}

static void test_runtime_free_cleans_pending_async_channel_roundtrip(void) {
    printf("Testing runtime_free cleanup for pending async channel waiters roundtrip...\n");

    char* source_path = write_temp_vml(async_shutdown_channel_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for pending async channel shutdown test");
    if (!source_path) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(source_path);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime for pending async channel shutdown test");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Probe runtime_create has no error for pending async channel shutdown test");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(source_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for pending async channel shutdown test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        remove(cache_path_copy);
        TEST_ASSERT(!file_exists(cache_path_copy), "Pending async channel shutdown probe cache artifact removed before first run");
    }

    Runtime* rt_first = runtime_create(source_path);
    TEST_ASSERT(rt_first != NULL, "First runtime_create returns Runtime for pending async channel shutdown test");
    TEST_ASSERT(rt_first && !runtime_has_error(rt_first), "First runtime_create has no error for pending async channel shutdown test");
    if (rt_first && !runtime_has_error(rt_first)) {
        rt_first->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "Pending async channel shutdown first load mode is source");
        int rc = runtime_run(rt_first);
        TEST_ASSERT(rc == 0, "Pending async channel shutdown first runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_first), "Pending async channel shutdown first runtime_run has no runtime error");
        assert_async_pending_queue_shape(rt_first, "Pending async channel shutdown source run", 0, 1, 1, 0, 1);
    }
    teardown_runtime_after_manual_vm_free(rt_first, "Pending async channel shutdown source teardown");

    Runtime* rt_second = runtime_create(source_path);
    TEST_ASSERT(rt_second != NULL, "Second runtime_create returns Runtime for pending async channel shutdown test");
    TEST_ASSERT(rt_second && !runtime_has_error(rt_second), "Second runtime_create has no error for pending async channel shutdown test");
    if (rt_second && !runtime_has_error(rt_second)) {
        rt_second->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Pending async channel shutdown second load mode is cache");
        int rc = runtime_run(rt_second);
        TEST_ASSERT(rc == 0, "Pending async channel shutdown second runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_second), "Pending async channel shutdown second runtime_run has no runtime error");
        assert_async_pending_queue_shape(rt_second, "Pending async channel shutdown cache run", 0, 1, 1, 0, 1);
    }
    teardown_runtime_after_manual_vm_free(rt_second, "Pending async channel shutdown cache teardown");

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for pending async channel shutdown test");
    if (artifact_ready && artifact_path) {
        Runtime* rt_artifact = runtime_create(artifact_path);
        TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime for pending async channel shutdown test");
        TEST_ASSERT(rt_artifact && !runtime_has_error(rt_artifact), "Artifact runtime_create has no error for pending async channel shutdown test");
        if (rt_artifact && !runtime_has_error(rt_artifact)) {
            rt_artifact->vm->config.max_instructions = 50000000;
            TEST_ASSERT(runtime_get_load_mode(rt_artifact) == RUNTIME_LOAD_ARTIFACT, "Pending async channel shutdown artifact load mode is artifact");
            int rc = runtime_run(rt_artifact);
            TEST_ASSERT(rc == 0, "Pending async channel shutdown artifact runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt_artifact), "Pending async channel shutdown artifact runtime_run has no runtime error");
            assert_async_pending_queue_shape(rt_artifact, "Pending async channel shutdown artifact run", 0, 1, 1, 0, 1);
        }
        teardown_runtime_after_manual_vm_free(rt_artifact, "Pending async channel shutdown artifact teardown");
        cleanup_temp(artifact_path);
    }

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(source_path);
}

static const char* async_shutdown_nested_sleep_source(void) {
    return
        "async func leaf(): int {\n"
        "    await asyncSleep(60000);\n"
        "    return 1;\n"
        "}\n"
        "async func mid(): int {\n"
        "    return await leaf();\n"
        "}\n"
        "async func top(): int {\n"
        "    return await mid();\n"
        "}\n"
        "func main(): void {\n"
        "    top();\n"
        "}\n";
}

static void test_runtime_free_cleans_nested_async_sleep_roundtrip(void) {
    printf("Testing runtime_free cleanup for nested async sleep shutdown roundtrip...\n");

    char* source_path = write_temp_vml(async_shutdown_nested_sleep_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for nested async sleep shutdown test");
    if (!source_path) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(source_path);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime for nested async sleep shutdown test");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Probe runtime_create has no error for nested async sleep shutdown test");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(source_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for nested async sleep shutdown test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        remove(cache_path_copy);
        TEST_ASSERT(!file_exists(cache_path_copy), "Nested async sleep shutdown probe cache artifact removed before first run");
    }

    Runtime* rt_first = runtime_create(source_path);
    TEST_ASSERT(rt_first != NULL, "First runtime_create returns Runtime for nested async sleep shutdown test");
    TEST_ASSERT(rt_first && !runtime_has_error(rt_first), "First runtime_create has no error for nested async sleep shutdown test");
    if (rt_first && !runtime_has_error(rt_first)) {
        rt_first->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "Nested async sleep shutdown first load mode is source");
        int rc = runtime_run(rt_first);
        TEST_ASSERT(rc == 0, "Nested async sleep shutdown first runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_first), "Nested async sleep shutdown first runtime_run has no runtime error");
        assert_async_pending_queue_shape(rt_first, "Nested async sleep shutdown source run", 0, 3, 3, 1, 0);
    }
    teardown_runtime_after_manual_vm_free(rt_first, "Nested async sleep shutdown source teardown");

    Runtime* rt_second = runtime_create(source_path);
    TEST_ASSERT(rt_second != NULL, "Second runtime_create returns Runtime for nested async sleep shutdown test");
    TEST_ASSERT(rt_second && !runtime_has_error(rt_second), "Second runtime_create has no error for nested async sleep shutdown test");
    if (rt_second && !runtime_has_error(rt_second)) {
        rt_second->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Nested async sleep shutdown second load mode is cache");
        int rc = runtime_run(rt_second);
        TEST_ASSERT(rc == 0, "Nested async sleep shutdown second runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_second), "Nested async sleep shutdown second runtime_run has no runtime error");
        assert_async_pending_queue_shape(rt_second, "Nested async sleep shutdown cache run", 0, 3, 3, 1, 0);
    }
    teardown_runtime_after_manual_vm_free(rt_second, "Nested async sleep shutdown cache teardown");

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for nested async sleep shutdown test");
    if (artifact_ready && artifact_path) {
        Runtime* rt_artifact = runtime_create(artifact_path);
        TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime for nested async sleep shutdown test");
        TEST_ASSERT(rt_artifact && !runtime_has_error(rt_artifact), "Artifact runtime_create has no error for nested async sleep shutdown test");
        if (rt_artifact && !runtime_has_error(rt_artifact)) {
            rt_artifact->vm->config.max_instructions = 50000000;
            TEST_ASSERT(runtime_get_load_mode(rt_artifact) == RUNTIME_LOAD_ARTIFACT, "Nested async sleep shutdown artifact load mode is artifact");
            int rc = runtime_run(rt_artifact);
            TEST_ASSERT(rc == 0, "Nested async sleep shutdown artifact runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt_artifact), "Nested async sleep shutdown artifact runtime_run has no runtime error");
            assert_async_pending_queue_shape(rt_artifact, "Nested async sleep shutdown artifact run", 0, 3, 3, 1, 0);
        }
        teardown_runtime_after_manual_vm_free(rt_artifact, "Nested async sleep shutdown artifact teardown");
        cleanup_temp(artifact_path);
    }

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(source_path);
}

static const char* async_shutdown_nested_channel_source(void) {
    return
        "async func leaf(channelId: int): int {\n"
        "    var recv: (any, Error?) = await asyncChannelRecv(channelId);\n"
        "    if (recv.1 != nil) {\n"
        "        panic(\"recv error\");\n"
        "    }\n"
        "    return recv.0 as int;\n"
        "}\n"
        "async func mid(channelId: int): int {\n"
        "    return await leaf(channelId);\n"
        "}\n"
        "async func top(channelId: int): int {\n"
        "    return await mid(channelId);\n"
        "}\n"
        "func main(): void {\n"
        "    var created = syncChannelCreate(1);\n"
        "    if (created.1 != nil) {\n"
        "        panic(\"channel create failed\");\n"
        "    }\n"
        "    top(created.0 as int);\n"
        "}\n";
}

static void test_runtime_free_cleans_nested_async_channel_roundtrip(void) {
    printf("Testing runtime_free cleanup for nested async channel waiters roundtrip...\n");

    char* source_path = write_temp_vml(async_shutdown_nested_channel_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for nested async channel shutdown test");
    if (!source_path) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(source_path);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime for nested async channel shutdown test");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Probe runtime_create has no error for nested async channel shutdown test");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(source_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for nested async channel shutdown test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        remove(cache_path_copy);
        TEST_ASSERT(!file_exists(cache_path_copy), "Nested async channel shutdown probe cache artifact removed before first run");
    }

    Runtime* rt_first = runtime_create(source_path);
    TEST_ASSERT(rt_first != NULL, "First runtime_create returns Runtime for nested async channel shutdown test");
    TEST_ASSERT(rt_first && !runtime_has_error(rt_first), "First runtime_create has no error for nested async channel shutdown test");
    if (rt_first && !runtime_has_error(rt_first)) {
        rt_first->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "Nested async channel shutdown first load mode is source");
        int rc = runtime_run(rt_first);
        TEST_ASSERT(rc == 0, "Nested async channel shutdown first runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_first), "Nested async channel shutdown first runtime_run has no runtime error");
        assert_async_pending_queue_shape(rt_first, "Nested async channel shutdown source run", 0, 3, 3, 0, 1);
    }
    teardown_runtime_after_manual_vm_free(rt_first, "Nested async channel shutdown source teardown");

    Runtime* rt_second = runtime_create(source_path);
    TEST_ASSERT(rt_second != NULL, "Second runtime_create returns Runtime for nested async channel shutdown test");
    TEST_ASSERT(rt_second && !runtime_has_error(rt_second), "Second runtime_create has no error for nested async channel shutdown test");
    if (rt_second && !runtime_has_error(rt_second)) {
        rt_second->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Nested async channel shutdown second load mode is cache");
        int rc = runtime_run(rt_second);
        TEST_ASSERT(rc == 0, "Nested async channel shutdown second runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_second), "Nested async channel shutdown second runtime_run has no runtime error");
        assert_async_pending_queue_shape(rt_second, "Nested async channel shutdown cache run", 0, 3, 3, 0, 1);
    }
    teardown_runtime_after_manual_vm_free(rt_second, "Nested async channel shutdown cache teardown");

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for nested async channel shutdown test");
    if (artifact_ready && artifact_path) {
        Runtime* rt_artifact = runtime_create(artifact_path);
        TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime for nested async channel shutdown test");
        TEST_ASSERT(rt_artifact && !runtime_has_error(rt_artifact), "Artifact runtime_create has no error for nested async channel shutdown test");
        if (rt_artifact && !runtime_has_error(rt_artifact)) {
            rt_artifact->vm->config.max_instructions = 50000000;
            TEST_ASSERT(runtime_get_load_mode(rt_artifact) == RUNTIME_LOAD_ARTIFACT, "Nested async channel shutdown artifact load mode is artifact");
            int rc = runtime_run(rt_artifact);
            TEST_ASSERT(rc == 0, "Nested async channel shutdown artifact runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt_artifact), "Nested async channel shutdown artifact runtime_run has no runtime error");
            assert_async_pending_queue_shape(rt_artifact, "Nested async channel shutdown artifact run", 0, 3, 3, 0, 1);
        }
        teardown_runtime_after_manual_vm_free(rt_artifact, "Nested async channel shutdown artifact teardown");
        cleanup_temp(artifact_path);
    }

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(source_path);
}

static const char* async_shutdown_shared_future_source(void) {
    return
        "async func waitPlus(shared: Future[int], delta: int): int {\n"
        "    return (await shared) + delta;\n"
        "}\n"
        "func main(): void {\n"
        "    var shared = futurePending<int>();\n"
        "    var first = waitPlus(shared, 1);\n"
        "    var second = waitPlus(shared, 2);\n"
        "    var third = waitPlus(shared, 3);\n"
        "    if (typeOf(first) != \"Future\" || typeOf(second) != \"Future\" || typeOf(third) != \"Future\") {\n"
        "        panic(\"shared future shutdown tasks have wrong type\");\n"
        "    }\n"
        "}\n";
}

static void test_runtime_free_cleans_shared_future_waiters_roundtrip(void) {
    printf("Testing runtime_free cleanup for shared future waiter shutdown roundtrip...\n");

    char* source_path = write_temp_vml(async_shutdown_shared_future_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for shared future shutdown test");
    if (!source_path) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(source_path);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime for shared future shutdown test");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Probe runtime_create has no error for shared future shutdown test");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(source_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for shared future shutdown test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        remove(cache_path_copy);
        TEST_ASSERT(!file_exists(cache_path_copy), "Shared future shutdown probe cache artifact removed before first run");
    }

    Runtime* rt_first = runtime_create(source_path);
    TEST_ASSERT(rt_first != NULL, "First runtime_create returns Runtime for shared future shutdown test");
    TEST_ASSERT(rt_first && !runtime_has_error(rt_first), "First runtime_create has no error for shared future shutdown test");
    if (rt_first && !runtime_has_error(rt_first)) {
        rt_first->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "Shared future shutdown first load mode is source");
        int rc = runtime_run(rt_first);
        TEST_ASSERT(rc == 0, "Shared future shutdown first runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_first), "Shared future shutdown first runtime_run has no runtime error");
        assert_async_pending_queue_shape(rt_first, "Shared future shutdown source run", 0, 1, 3, 0, 0);
    }
    teardown_runtime_after_manual_vm_free(rt_first, "Shared future shutdown source teardown");

    Runtime* rt_second = runtime_create(source_path);
    TEST_ASSERT(rt_second != NULL, "Second runtime_create returns Runtime for shared future shutdown test");
    TEST_ASSERT(rt_second && !runtime_has_error(rt_second), "Second runtime_create has no error for shared future shutdown test");
    if (rt_second && !runtime_has_error(rt_second)) {
        rt_second->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Shared future shutdown second load mode is cache");
        int rc = runtime_run(rt_second);
        TEST_ASSERT(rc == 0, "Shared future shutdown second runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_second), "Shared future shutdown second runtime_run has no runtime error");
        assert_async_pending_queue_shape(rt_second, "Shared future shutdown cache run", 0, 1, 3, 0, 0);
    }
    teardown_runtime_after_manual_vm_free(rt_second, "Shared future shutdown cache teardown");

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for shared future shutdown test");
    if (artifact_ready && artifact_path) {
        Runtime* rt_artifact = runtime_create(artifact_path);
        TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime for shared future shutdown test");
        TEST_ASSERT(rt_artifact && !runtime_has_error(rt_artifact), "Artifact runtime_create has no error for shared future shutdown test");
        if (rt_artifact && !runtime_has_error(rt_artifact)) {
            rt_artifact->vm->config.max_instructions = 50000000;
            TEST_ASSERT(runtime_get_load_mode(rt_artifact) == RUNTIME_LOAD_ARTIFACT, "Shared future shutdown artifact load mode is artifact");
            int rc = runtime_run(rt_artifact);
            TEST_ASSERT(rc == 0, "Shared future shutdown artifact runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt_artifact), "Shared future shutdown artifact runtime_run has no runtime error");
            assert_async_pending_queue_shape(rt_artifact, "Shared future shutdown artifact run", 0, 1, 3, 0, 0);
        }
        teardown_runtime_after_manual_vm_free(rt_artifact, "Shared future shutdown artifact teardown");
        cleanup_temp(artifact_path);
    }

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(source_path);
}

static const char* async_shared_future_wakeup_roundtrip_source(void) {
    return
        "async func waitPlus(shared: Future[int], delta: int): int {\n"
        "    return (await shared) + delta;\n"
        "}\n"
        "func main(): void {\n"
        "    var shared = futurePending<int>();\n"
        "    var first = waitPlus(shared, 1);\n"
        "    var second = waitPlus(shared, 2);\n"
        "    var third = waitPlus(shared, 3);\n"
        "    if (futureIsReady(first) || futureIsReady(second) || futureIsReady(third)) {\n"
        "        panic(\"shared future waiters started ready\");\n"
        "    }\n"
        "    if (!futureComplete(shared, 10)) {\n"
        "        panic(\"shared future complete failed\");\n"
        "    }\n"
        "    while (!futureIsReady(first) || !futureIsReady(second) || !futureIsReady(third)) {\n"
        "    }\n"
        "    if (futureGet(first) != 11) {\n"
        "        panic(\"first waiter mismatch\");\n"
        "    }\n"
        "    if (futureGet(second) != 12) {\n"
        "        panic(\"second waiter mismatch\");\n"
        "    }\n"
        "    if (futureGet(third) != 13) {\n"
        "        panic(\"third waiter mismatch\");\n"
        "    }\n"
        "}\n";
}

static void test_async_shared_future_wakeup_cache_and_artifact_roundtrip(void) {
    printf("Testing async shared future wakeup cache and artifact roundtrip...\n");

    char* source_path = write_temp_vml(async_shared_future_wakeup_roundtrip_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for async shared future wakeup roundtrip test");
    if (!source_path) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(source_path);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime for async shared future wakeup roundtrip test");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Probe runtime_create has no error for async shared future wakeup roundtrip test");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(source_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for async shared future wakeup roundtrip test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        remove(cache_path_copy);
        TEST_ASSERT(!file_exists(cache_path_copy), "Async shared future wakeup roundtrip probe cache artifact removed before first run");
    }

    Runtime* rt_first = runtime_create(source_path);
    TEST_ASSERT(rt_first != NULL, "First runtime_create returns Runtime for async shared future wakeup roundtrip test");
    TEST_ASSERT(rt_first && !runtime_has_error(rt_first), "First runtime_create has no error for async shared future wakeup roundtrip test");
    if (rt_first && !runtime_has_error(rt_first)) {
        rt_first->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "Async shared future wakeup roundtrip first load mode is source");
        int rc = runtime_run(rt_first);
        TEST_ASSERT(rc == 0, "Async shared future wakeup roundtrip first runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_first), "Async shared future wakeup roundtrip first runtime_run has no runtime error");
        assert_async_vm_queues_drained(rt_first, "Async shared future wakeup roundtrip source run");
    }
    if (rt_first) runtime_free(rt_first);

    if (cache_path_copy) {
        TEST_ASSERT(file_exists(cache_path_copy), "Async shared future wakeup roundtrip cache artifact is written after first run");
    }

    Runtime* rt_second = runtime_create(source_path);
    TEST_ASSERT(rt_second != NULL, "Second runtime_create returns Runtime for async shared future wakeup roundtrip test");
    TEST_ASSERT(rt_second && !runtime_has_error(rt_second), "Second runtime_create has no error for async shared future wakeup roundtrip test");
    if (rt_second && !runtime_has_error(rt_second)) {
        rt_second->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Async shared future wakeup roundtrip second load mode is cache");
        int rc = runtime_run(rt_second);
        TEST_ASSERT(rc == 0, "Async shared future wakeup roundtrip second runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_second), "Async shared future wakeup roundtrip second runtime_run has no runtime error");
        assert_async_vm_queues_drained(rt_second, "Async shared future wakeup roundtrip cache run");
    }
    if (rt_second) runtime_free(rt_second);

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for async shared future wakeup roundtrip test");
    if (artifact_ready && artifact_path) {
        Runtime* rt_artifact = runtime_create(artifact_path);
        TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime for async shared future wakeup roundtrip test");
        TEST_ASSERT(rt_artifact && !runtime_has_error(rt_artifact), "Artifact runtime_create has no error for async shared future wakeup roundtrip test");
        if (rt_artifact && !runtime_has_error(rt_artifact)) {
            rt_artifact->vm->config.max_instructions = 50000000;
            TEST_ASSERT(runtime_get_load_mode(rt_artifact) == RUNTIME_LOAD_ARTIFACT, "Async shared future wakeup roundtrip artifact load mode is artifact");
            int rc = runtime_run(rt_artifact);
            TEST_ASSERT(rc == 0, "Async shared future wakeup roundtrip artifact runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt_artifact), "Async shared future wakeup roundtrip artifact runtime_run has no runtime error");
            assert_async_vm_queues_drained(rt_artifact, "Async shared future wakeup roundtrip artifact run");
        }
        if (rt_artifact) runtime_free(rt_artifact);
        cleanup_temp(artifact_path);
    }

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(source_path);
}

static const char* async_shared_panicked_future_roundtrip_source(void) {
    return
        "var sharedOut: Future[int] = futureResolved(0);\n"
        "var out1: Future[int] = futureResolved(0);\n"
        "var out2: Future[int] = futureResolved(0);\n"
        "var out3: Future[int] = futureResolved(0);\n"
        "async func sharedRoot(input: Future[int]): int {\n"
        "    var value: int = await input;\n"
        "    if (value != 9) {\n"
        "        panic(\"shared root wrong value\");\n"
        "    }\n"
        "    panic(\"shared root panic\");\n"
        "    return value;\n"
        "}\n"
        "async func observe(task: Future[int]): int {\n"
        "    return await task;\n"
        "}\n"
        "func rethrowFirst(): void {\n"
        "    println(futureGet(out1));\n"
        "}\n"
        "func main(): void {\n"
        "    var input = futurePending<int>();\n"
        "    sharedOut = sharedRoot(input);\n"
        "    out1 = observe(sharedOut);\n"
        "    out2 = observe(sharedOut);\n"
        "    out3 = observe(sharedOut);\n"
        "    if (!futureComplete(input, 9)) {\n"
        "        panic(\"shared input complete failed\");\n"
        "    }\n"
        "    while (!futureIsReady(sharedOut) || !futureIsReady(out1) || !futureIsReady(out2) || !futureIsReady(out3)) {\n"
        "    }\n"
        "}\n";
}

static void test_async_shared_panicked_future_cache_and_artifact_roundtrip(void) {
    printf("Testing async shared panicked future fan-out cache and artifact roundtrip...\n");

    char* source_path = write_temp_vml(async_shared_panicked_future_roundtrip_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for async shared panicked future roundtrip test");
    if (!source_path) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(source_path);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime for async shared panicked future roundtrip test");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Probe runtime_create has no error for async shared panicked future roundtrip test");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(source_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for async shared panicked future roundtrip test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        remove(cache_path_copy);
        TEST_ASSERT(!file_exists(cache_path_copy), "Async shared panicked future roundtrip probe cache artifact removed before first run");
    }

    Runtime* rt_first = runtime_create(source_path);
    TEST_ASSERT(rt_first != NULL, "First runtime_create returns Runtime for async shared panicked future roundtrip test");
    TEST_ASSERT(rt_first && !runtime_has_error(rt_first), "First runtime_create has no error for async shared panicked future roundtrip test");
    if (rt_first && !runtime_has_error(rt_first)) {
        rt_first->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "Async shared panicked future roundtrip first load mode is source");
        int rc = runtime_run(rt_first);
        TEST_ASSERT(rc == 0, "Async shared panicked future roundtrip first runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_first), "Async shared panicked future roundtrip first runtime_run has no runtime error");
        assert_global_future_panicked(rt_first, "sharedOut", "Async shared panicked future source run", "shared root panic");
        assert_global_future_panicked(rt_first, "out1", "Async shared panicked future source run", "shared root panic");
        assert_global_future_panicked(rt_first, "out2", "Async shared panicked future source run", "shared root panic");
        assert_global_future_panicked(rt_first, "out3", "Async shared panicked future source run", "shared root panic");
        assert_async_vm_queues_drained(rt_first, "Async shared panicked future source run");

        rc = runtime_run_function(rt_first, "rethrowFirst");
        TEST_ASSERT(rc != 0, "Async shared panicked future source rethrow reports failure");
        TEST_ASSERT(runtime_has_error(rt_first), "Async shared panicked future source rethrow reports runtime error");
        if (runtime_has_error(rt_first)) {
            const char* err = runtime_get_error(rt_first);
            TEST_ASSERT(err && strstr(err, "shared root panic") != NULL, "Async shared panicked future source rethrow includes shared panic message");
            TEST_ASSERT(err && strstr(err, "Stack trace:") != NULL, "Async shared panicked future source rethrow includes stack trace header");
        }
        assert_async_vm_queues_drained(rt_first, "Async shared panicked future source rethrow");
    }
    if (rt_first) runtime_free(rt_first);

    if (cache_path_copy) {
        TEST_ASSERT(file_exists(cache_path_copy), "Async shared panicked future roundtrip cache artifact is written after first run");
    }

    Runtime* rt_second = runtime_create(source_path);
    TEST_ASSERT(rt_second != NULL, "Second runtime_create returns Runtime for async shared panicked future roundtrip test");
    TEST_ASSERT(rt_second && !runtime_has_error(rt_second), "Second runtime_create has no error for async shared panicked future roundtrip test");
    if (rt_second && !runtime_has_error(rt_second)) {
        rt_second->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Async shared panicked future roundtrip second load mode is cache");
        int rc = runtime_run(rt_second);
        TEST_ASSERT(rc == 0, "Async shared panicked future roundtrip second runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_second), "Async shared panicked future roundtrip second runtime_run has no runtime error");
        assert_global_future_panicked(rt_second, "sharedOut", "Async shared panicked future cache run", "shared root panic");
        assert_global_future_panicked(rt_second, "out1", "Async shared panicked future cache run", "shared root panic");
        assert_global_future_panicked(rt_second, "out2", "Async shared panicked future cache run", "shared root panic");
        assert_global_future_panicked(rt_second, "out3", "Async shared panicked future cache run", "shared root panic");
        assert_async_vm_queues_drained(rt_second, "Async shared panicked future cache run");

        rc = runtime_run_function(rt_second, "rethrowFirst");
        TEST_ASSERT(rc != 0, "Async shared panicked future cache rethrow reports failure");
        TEST_ASSERT(runtime_has_error(rt_second), "Async shared panicked future cache rethrow reports runtime error");
        if (runtime_has_error(rt_second)) {
            const char* err = runtime_get_error(rt_second);
            TEST_ASSERT(err && strstr(err, "shared root panic") != NULL, "Async shared panicked future cache rethrow includes shared panic message");
            TEST_ASSERT(err && strstr(err, "Stack trace:") != NULL, "Async shared panicked future cache rethrow includes stack trace header");
        }
        assert_async_vm_queues_drained(rt_second, "Async shared panicked future cache rethrow");
    }
    if (rt_second) runtime_free(rt_second);

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for async shared panicked future roundtrip test");
    if (artifact_ready && artifact_path) {
        Runtime* rt_artifact = runtime_create(artifact_path);
        TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime for async shared panicked future roundtrip test");
        TEST_ASSERT(rt_artifact && !runtime_has_error(rt_artifact), "Artifact runtime_create has no error for async shared panicked future roundtrip test");
        if (rt_artifact && !runtime_has_error(rt_artifact)) {
            rt_artifact->vm->config.max_instructions = 50000000;
            TEST_ASSERT(runtime_get_load_mode(rt_artifact) == RUNTIME_LOAD_ARTIFACT, "Async shared panicked future roundtrip artifact load mode is artifact");
            int rc = runtime_run(rt_artifact);
            TEST_ASSERT(rc == 0, "Async shared panicked future roundtrip artifact runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt_artifact), "Async shared panicked future roundtrip artifact runtime_run has no runtime error");
            assert_global_future_panicked(rt_artifact, "sharedOut", "Async shared panicked future artifact run", "shared root panic");
            assert_global_future_panicked(rt_artifact, "out1", "Async shared panicked future artifact run", "shared root panic");
            assert_global_future_panicked(rt_artifact, "out2", "Async shared panicked future artifact run", "shared root panic");
            assert_global_future_panicked(rt_artifact, "out3", "Async shared panicked future artifact run", "shared root panic");
            assert_async_vm_queues_drained(rt_artifact, "Async shared panicked future artifact run");

            rc = runtime_run_function(rt_artifact, "rethrowFirst");
            TEST_ASSERT(rc != 0, "Async shared panicked future artifact rethrow reports failure");
            TEST_ASSERT(runtime_has_error(rt_artifact), "Async shared panicked future artifact rethrow reports runtime error");
            if (runtime_has_error(rt_artifact)) {
                const char* err = runtime_get_error(rt_artifact);
                TEST_ASSERT(err && strstr(err, "shared root panic") != NULL, "Async shared panicked future artifact rethrow includes shared panic message");
                TEST_ASSERT(err && strstr(err, "Stack trace:") != NULL, "Async shared panicked future artifact rethrow includes stack trace header");
            }
            assert_async_vm_queues_drained(rt_artifact, "Async shared panicked future artifact rethrow");
        }
        if (rt_artifact) runtime_free(rt_artifact);
        cleanup_temp(artifact_path);
    }

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(source_path);
}

static const char* async_shutdown_shared_panicked_future_source(void) {
    return
        "async func sharedRoot(input: Future[int]): int {\n"
        "    var value: int = await input;\n"
        "    if (value != 9) {\n"
        "        panic(\"shared root wrong value\");\n"
        "    }\n"
        "    panic(\"shared root panic\");\n"
        "    return value;\n"
        "}\n"
        "async func observe(task: Future[int]): int {\n"
        "    return await task;\n"
        "}\n"
        "func main(): void {\n"
        "    var input = futurePending<int>();\n"
        "    var shared = sharedRoot(input);\n"
        "    var first = observe(shared);\n"
        "    var second = observe(shared);\n"
        "    var third = observe(shared);\n"
        "    if (typeOf(shared) != \"Future\" || typeOf(first) != \"Future\" || typeOf(second) != \"Future\" || typeOf(third) != \"Future\") {\n"
        "        panic(\"shared panicked shutdown tasks have wrong type\");\n"
        "    }\n"
        "}\n";
}

static void test_runtime_free_cleans_shared_panicked_future_graph_roundtrip(void) {
    printf("Testing runtime_free cleanup for shared panicked-future graph shutdown roundtrip...\n");

    char* source_path = write_temp_vml(async_shutdown_shared_panicked_future_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for shared panicked-future shutdown test");
    if (!source_path) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(source_path);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime for shared panicked-future shutdown test");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Probe runtime_create has no error for shared panicked-future shutdown test");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(source_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for shared panicked-future shutdown test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        remove(cache_path_copy);
        TEST_ASSERT(!file_exists(cache_path_copy), "Shared panicked-future shutdown probe cache artifact removed before first run");
    }

    Runtime* rt_first = runtime_create(source_path);
    TEST_ASSERT(rt_first != NULL, "First runtime_create returns Runtime for shared panicked-future shutdown test");
    TEST_ASSERT(rt_first && !runtime_has_error(rt_first), "First runtime_create has no error for shared panicked-future shutdown test");
    if (rt_first && !runtime_has_error(rt_first)) {
        rt_first->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_first) == RUNTIME_LOAD_SOURCE, "Shared panicked-future shutdown first load mode is source");
        int rc = runtime_run(rt_first);
        TEST_ASSERT(rc == 0, "Shared panicked-future shutdown first runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_first), "Shared panicked-future shutdown first runtime_run has no runtime error");
        assert_async_pending_queue_shape(rt_first, "Shared panicked-future shutdown source run", 0, 2, 4, 0, 0);
    }
    teardown_runtime_after_manual_vm_free(rt_first, "Shared panicked-future shutdown source teardown");

    Runtime* rt_second = runtime_create(source_path);
    TEST_ASSERT(rt_second != NULL, "Second runtime_create returns Runtime for shared panicked-future shutdown test");
    TEST_ASSERT(rt_second && !runtime_has_error(rt_second), "Second runtime_create has no error for shared panicked-future shutdown test");
    if (rt_second && !runtime_has_error(rt_second)) {
        rt_second->vm->config.max_instructions = 50000000;
        TEST_ASSERT(runtime_get_load_mode(rt_second) == RUNTIME_LOAD_CACHE, "Shared panicked-future shutdown second load mode is cache");
        int rc = runtime_run(rt_second);
        TEST_ASSERT(rc == 0, "Shared panicked-future shutdown second runtime_run succeeds");
        TEST_ASSERT(!runtime_has_error(rt_second), "Shared panicked-future shutdown second runtime_run has no runtime error");
        assert_async_pending_queue_shape(rt_second, "Shared panicked-future shutdown cache run", 0, 2, 4, 0, 0);
    }
    teardown_runtime_after_manual_vm_free(rt_second, "Shared panicked-future shutdown cache teardown");

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for shared panicked-future shutdown test");
    if (artifact_ready && artifact_path) {
        Runtime* rt_artifact = runtime_create(artifact_path);
        TEST_ASSERT(rt_artifact != NULL, "Artifact runtime_create returns Runtime for shared panicked-future shutdown test");
        TEST_ASSERT(rt_artifact && !runtime_has_error(rt_artifact), "Artifact runtime_create has no error for shared panicked-future shutdown test");
        if (rt_artifact && !runtime_has_error(rt_artifact)) {
            rt_artifact->vm->config.max_instructions = 50000000;
            TEST_ASSERT(runtime_get_load_mode(rt_artifact) == RUNTIME_LOAD_ARTIFACT, "Shared panicked-future shutdown artifact load mode is artifact");
            int rc = runtime_run(rt_artifact);
            TEST_ASSERT(rc == 0, "Shared panicked-future shutdown artifact runtime_run succeeds");
            TEST_ASSERT(!runtime_has_error(rt_artifact), "Shared panicked-future shutdown artifact runtime_run has no runtime error");
            assert_async_pending_queue_shape(rt_artifact, "Shared panicked-future shutdown artifact run", 0, 2, 4, 0, 0);
        }
        teardown_runtime_after_manual_vm_free(rt_artifact, "Shared panicked-future shutdown artifact teardown");
        cleanup_temp(artifact_path);
    }

    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(source_path);
}

static void test_cold_start_perf_regression_hints(void) {
    printf("Testing cold-start perf regression hints...\n");

    const int samples = 3;
    const int helper_count = 900;

    char* source_path = write_temp_cold_start_program(helper_count);
    TEST_ASSERT(source_path != NULL, "Temp source file created for cold-start perf test");
    if (!source_path) return;

    char* cache_path_copy = NULL;
    Runtime* rt_probe = runtime_create(source_path);
    TEST_ASSERT(rt_probe != NULL, "Probe runtime_create returns Runtime");
    TEST_ASSERT(rt_probe && !runtime_has_error(rt_probe), "Probe runtime_create has no error");
    if (!rt_probe || runtime_has_error(rt_probe)) {
        if (rt_probe) runtime_free(rt_probe);
        cleanup_temp(source_path);
        return;
    }

    const char* probe_cache_path = runtime_get_cache_path(rt_probe);
    TEST_ASSERT(probe_cache_path != NULL && probe_cache_path[0] != '\0', "Cache path is available for cold-start perf test");
    if (probe_cache_path && probe_cache_path[0] != '\0') {
        cache_path_copy = safe_strdup(probe_cache_path);
    }
    runtime_free(rt_probe);
    if (cache_path_copy) {
        remove(cache_path_copy);
    }

    double source_ms[3] = {0.0, 0.0, 0.0};
    double cache_ms[3] = {0.0, 0.0, 0.0};
    double artifact_ms[3] = {0.0, 0.0, 0.0};

    for (int i = 0; i < samples; i++) {
        if (cache_path_copy) remove(cache_path_copy);
        RuntimeLoadMode mode = RUNTIME_LOAD_SOURCE;
        int ok = runtime_measure_create_and_run_ms(source_path, &mode, &source_ms[i]);
        TEST_ASSERT(ok, "Source cold-start sample create+run succeeds");
        TEST_ASSERT(mode == RUNTIME_LOAD_SOURCE, "Source cold-start sample load mode is source");
    }

    for (int i = 0; i < samples; i++) {
        RuntimeLoadMode mode = RUNTIME_LOAD_SOURCE;
        int ok = runtime_measure_create_and_run_ms(source_path, &mode, &cache_ms[i]);
        TEST_ASSERT(ok, "Cache hit sample create+run succeeds");
        TEST_ASSERT(mode == RUNTIME_LOAD_CACHE, "Cache hit sample load mode is cache");
    }

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_from_source(source_path, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for cold-start perf test");
    if (!artifact_ready || !artifact_path) {
        if (cache_path_copy) {
            remove(cache_path_copy);
            free(cache_path_copy);
        }
        cleanup_temp(source_path);
        return;
    }

    for (int i = 0; i < samples; i++) {
        RuntimeLoadMode mode = RUNTIME_LOAD_SOURCE;
        int ok = runtime_measure_create_and_run_ms(artifact_path, &mode, &artifact_ms[i]);
        TEST_ASSERT(ok, "Artifact sample create+run succeeds");
        TEST_ASSERT(mode == RUNTIME_LOAD_ARTIFACT, "Artifact sample load mode is artifact");
    }

    double source_median_ms = median3(source_ms[0], source_ms[1], source_ms[2]);
    double cache_median_ms = median3(cache_ms[0], cache_ms[1], cache_ms[2]);
    double artifact_median_ms = median3(artifact_ms[0], artifact_ms[1], artifact_ms[2]);

    printf("  Info: cold-start medians (ms) source=%.2f cache=%.2f artifact=%.2f\n",
           source_median_ms,
           cache_median_ms,
           artifact_median_ms);

    // Keep a generous slack to avoid machine-noise flakes while still catching
    // obvious regressions where cached/artifact startup is no better than source compilation.
    double cache_limit_ms = (source_median_ms * 0.95) + 5.0;
    double artifact_limit_ms = (source_median_ms * 0.95) + 5.0;
    TEST_ASSERT(cache_median_ms <= cache_limit_ms, "Cache hit median startup is faster than source cold-start (with slack)");
    TEST_ASSERT(artifact_median_ms <= artifact_limit_ms, "Artifact median startup is faster than source cold-start (with slack)");

    cleanup_temp(artifact_path);
    if (cache_path_copy) {
        remove(cache_path_copy);
        free(cache_path_copy);
    }
    cleanup_temp(source_path);
}

typedef void (*RuntimeSafetyTestFn)(void);

typedef struct {
    const char* name;
    RuntimeSafetyTestFn fn;
} RuntimeSafetyTestCase;

static const RuntimeSafetyTestCase g_runtime_safety_tests[] = {
    {"test_smoke_ok", test_smoke_ok},
    {"test_division_by_zero", test_division_by_zero},
    {"test_array_bounds", test_array_bounds},
    {"test_replace_stress_no_abort", test_replace_stress_no_abort},
    {"test_core_builtin_churn_no_abort", test_core_builtin_churn_no_abort},
    {"test_sort_reverse_builtin_churn_no_abort", test_sort_reverse_builtin_churn_no_abort},
    {"test_recoverable_safe_alloc_guard", test_recoverable_safe_alloc_guard},
    {"test_zero_size_safe_alloc_requests", test_zero_size_safe_alloc_requests},
    {"test_runtime_create_injected_oom_recovery", test_runtime_create_injected_oom_recovery},
    {"test_runtime_run_injected_oom_recovery", test_runtime_run_injected_oom_recovery},
    {"test_execution_limit", test_execution_limit},
    {"test_stack_trace_for_runtime_trap", test_stack_trace_for_runtime_trap},
    {"test_stack_trace_for_panic", test_stack_trace_for_panic},
    {"test_cache_cold_start_and_invalidation", test_cache_cold_start_and_invalidation},
    {"test_cache_dependency_invalidation", test_cache_dependency_invalidation},
    {"test_cache_separated_by_typecheck_flags", test_cache_separated_by_typecheck_flags},
    {"test_artifact_load_mode", test_artifact_load_mode},
    {"test_artifact_stack_trace_source_locations", test_artifact_stack_trace_source_locations},
    {"test_interface_impl_runtime_dispatch", test_interface_impl_runtime_dispatch},
    {"test_malformed_artifact_rejected_without_crash", test_malformed_artifact_rejected_without_crash},
    {"test_malformed_artifact_signature_rejected_at_load", test_malformed_artifact_signature_rejected_at_load},
    {"test_artifact_load_injected_oom_recovery", test_artifact_load_injected_oom_recovery},
    {"test_runtime_capability_denials", test_runtime_capability_denials},
    {"test_runtime_resource_limits", test_runtime_resource_limits},
    {"test_async_sleep_scheduler_progress", test_async_sleep_scheduler_progress},
    {"test_async_timeout_scheduler_progress", test_async_timeout_scheduler_progress},
    {"test_async_channel_scheduler_progress", test_async_channel_scheduler_progress},
    {"test_async_panic_cleanup_cache_and_artifact_roundtrip", test_async_panic_cleanup_cache_and_artifact_roundtrip},
    {"test_async_panic_observation_cache_and_artifact_roundtrip", test_async_panic_observation_cache_and_artifact_roundtrip},
    {"test_async_channel_panic_cleanup_cache_and_artifact_roundtrip", test_async_channel_panic_cleanup_cache_and_artifact_roundtrip},
    {"test_async_timeout_cache_and_artifact_roundtrip", test_async_timeout_cache_and_artifact_roundtrip},
    {"test_runtime_free_cleans_pending_async_sleep_roundtrip", test_runtime_free_cleans_pending_async_sleep_roundtrip},
    {"test_runtime_free_cleans_pending_async_channel_roundtrip", test_runtime_free_cleans_pending_async_channel_roundtrip},
    {"test_runtime_free_cleans_nested_async_sleep_roundtrip", test_runtime_free_cleans_nested_async_sleep_roundtrip},
    {"test_runtime_free_cleans_nested_async_channel_roundtrip", test_runtime_free_cleans_nested_async_channel_roundtrip},
    {"test_runtime_free_cleans_shared_future_waiters_roundtrip", test_runtime_free_cleans_shared_future_waiters_roundtrip},
    {"test_async_shared_future_wakeup_cache_and_artifact_roundtrip", test_async_shared_future_wakeup_cache_and_artifact_roundtrip},
    {"test_async_shared_panicked_future_cache_and_artifact_roundtrip", test_async_shared_panicked_future_cache_and_artifact_roundtrip},
    {"test_runtime_free_cleans_shared_panicked_future_graph_roundtrip", test_runtime_free_cleans_shared_panicked_future_graph_roundtrip},
    {"test_cold_start_perf_regression_hints", test_cold_start_perf_regression_hints},
};

int main(int argc, char** argv) {
    printf("Running TabloLang Runtime Safety Tests...\n\n");

    srand(1);

    if (argc > 1) {
        int matched = 0;
        for (int argi = 1; argi < argc; argi++) {
            const char* requested = argv[argi];
            int found = 0;
            for (size_t i = 0; i < sizeof(g_runtime_safety_tests) / sizeof(g_runtime_safety_tests[0]); i++) {
                if (strcmp(g_runtime_safety_tests[i].name, requested) == 0) {
                    g_runtime_safety_tests[i].fn();
                    found = 1;
                    matched++;
                    break;
                }
            }
            if (!found) {
                printf("  FAIL: Unknown runtime safety test '%s'\n", requested);
                tests_failed++;
            }
        }
        if (matched == 0 && tests_failed == 0) {
            printf("  FAIL: No runtime safety tests were selected\n");
            tests_failed++;
        }
    } else {
        for (size_t i = 0; i < sizeof(g_runtime_safety_tests) / sizeof(g_runtime_safety_tests[0]); i++) {
            g_runtime_safety_tests[i].fn();
        }
    }

    printf("\nRuntime Safety Test Results:\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
