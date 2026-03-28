#include "test_runner.h"
#include "../../src/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

static int tests_passed = 0;
static int tests_failed = 0;

void output_init(CapturedOutput* output) {
    output->data = (char*)malloc(MAX_OUTPUT_SIZE);
    output->size = 0;
    output->capacity = MAX_OUTPUT_SIZE;
    output->data[0] = '\0';
}

void output_capture(CapturedOutput* output) {
    output_init(output);
    fflush(stdout);

#ifdef _WIN32
    output->old_stdout = _dup(_fileno(stdout));
    if (tmpfile_s(&output->file) != 0 || !output->file) {
        output->file = NULL;
        return;
    }
    _dup2(_fileno(output->file), _fileno(stdout));
#else
    output->old_stdout = dup(STDOUT_FILENO);
    output->file = tmpfile();
    dup2(fileno(output->file), STDOUT_FILENO);
#endif
}

void output_restore(CapturedOutput* output) {
    fflush(stdout);

#ifdef _WIN32
    _dup2(output->old_stdout, _fileno(stdout));
    _close(output->old_stdout);
#else
    dup2(output->old_stdout, STDOUT_FILENO);
    close(output->old_stdout);
#endif

    if (!output->file) {
        output->size = 0;
        output->data[0] = '\0';
        return;
    }

    rewind(output->file);

    size_t bytes_read = fread(output->data, 1, output->capacity - 1, output->file);
    output->data[bytes_read] = '\0';
    output->size = bytes_read;

    fclose(output->file);
}

void output_free(CapturedOutput* output) {
    if (output->data) {
        free(output->data);
        output->data = NULL;
    }
    output->size = 0;
    output->capacity = 0;
}

int compare_outputs(const char* actual, const char* expected) {
    if (!actual || !expected) {
        return 0;
    }

    size_t actual_len = strlen(actual);
    size_t expected_len = strlen(expected);

    if (actual_len != expected_len) {
        return 0;
    }

    return strcmp(actual, expected) == 0;
}

char* read_file_contents(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* contents = (char*)malloc(length + 1);
    if (contents) {
        size_t read = fread(contents, 1, length, file);
        contents[read] = '\0';
    }

    fclose(file);
    return contents;
}

int write_test_file(const char* filename, const char* content) {
    FILE* file = fopen(filename, "w");
    if (!file) {
        return 0;
    }

    if (content) {
        fputs(content, file);
    }

    fclose(file);
    return 1;
}

void cleanup_test_file(const char* filename) {
    remove(filename);
}

int run_test(TestConfig* config) {
    printf("\n=== %s ===\n", config->name);

    char* expected = NULL;
    if (config->expected_file) {
        expected = read_file_contents(config->expected_file);
    }

    CapturedOutput captured;
    output_capture(&captured);

    Runtime* rt = runtime_create(config->source_file);

    int exit_code = -1;
    if (!runtime_has_error(rt)) {
        exit_code = runtime_run(rt);
    }

    output_restore(&captured);

    int passed = 0;
    char* error_msg = NULL;

    if (runtime_has_error(rt)) {
        error_msg = (char*)runtime_get_error(rt);
        passed = (!config->should_pass) && (error_msg != NULL);
    } else {
        if (expected) {
            passed = compare_outputs(captured.data, expected);
        } else {
            passed = config->should_pass && (exit_code == 0);
        }
    }

    if (passed) {
        tests_passed++;
        printf("PASS\n");
    } else {
        tests_failed++;
        printf("FAIL\n");

        if (error_msg) {
            printf("  Error: %s\n", error_msg);
            if (config->should_pass && captured.data && captured.data[0] != '\0') {
                size_t out_len = strlen(captured.data);
                const char* tail = captured.data;
                const size_t tail_max = 2000;
                if (out_len > tail_max) {
                    tail = captured.data + (out_len - tail_max);
                }
                printf("  Output before error (tail):\n%s\n", tail);
            }
        } else {
            printf("  Expected output:\n%s\n", expected ? expected : "<none>");
            printf("  Actual output:\n%s\n", captured.data ? captured.data : "<none>");
        }
    }

    output_free(&captured);
    runtime_free(rt);

    if (expected) {
        free(expected);
    }

    if (config->cleanup_files && config->test_files_to_create) {
        for (int i = 0; i < config->test_files_count; i++) {
            cleanup_test_file(config->test_files_to_create[i]);
        }
    }

    return passed;
}

void print_summary(void) {
    printf("\n=== Test Summary ===\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("  Total:  %d\n", tests_passed + tests_failed);

    if (tests_failed > 0) {
        printf("\nFAILED: Some tests did not pass\n");
    } else {
        printf("\nSUCCESS: All tests passed\n");
    }
}

void run_io_tests(void) {
    printf("\n--- I/O Function Tests ---\n");

    TestConfig config = {
        .name = "I/O Functions (print/println)",
        .source_file = "native_integration_tests/io_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/io_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_conversion_tests(void) {
    printf("\n--- Type Conversion Tests ---\n");

    TestConfig config = {
        .name = "Type Conversion Functions",
        .source_file = "native_integration_tests/conversion_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/conversion_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);

    TestConfig bigint_strict = {
        .name = "BigInt (toBigInt parsing errors)",
        .source_file = "native_integration_tests/bigint_parse_denied.tblo",
        .expected_file = "native_integration_tests/expected_output/bigint_parse_denied.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&bigint_strict);

    TestConfig bytes = {
        .name = "Bytes (indexing + slicing)",
        .source_file = "native_integration_tests/bytes_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/bytes_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&bytes);
}

void run_json_tests(void) {
    printf("\n--- JSON Function Tests ---\n");

    TestConfig config = {
        .name = "JSON Functions",
        .source_file = "native_integration_tests/json_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/json_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_json_stdlib_tests(void) {
    printf("\n--- JSON Stdlib Wrapper Tests ---\n");

    TestConfig config = {
        .name = "JSON stdlib wrappers (jsonDecodeAsX)",
        .source_file = "native_integration_tests/json_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/json_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_url_stdlib_tests(void) {
    printf("\n--- URL Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "URL stdlib helpers (urlEncode/urlDecode + queryParse/queryStringify)",
        .source_file = "native_integration_tests/url_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/url_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_uuid_stdlib_tests(void) {
    printf("\n--- UUID Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "UUID stdlib helpers (parse/normalize/bytes/version/variant/v4)",
        .source_file = "native_integration_tests/uuid_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/uuid_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_netip_stdlib_tests(void) {
    printf("\n--- NetIP Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "NetIP stdlib helpers (ipv4/cidr parse+format+contains)",
        .source_file = "native_integration_tests/netip_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/netip_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_semver_stdlib_tests(void) {
    printf("\n--- SemVer Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "SemVer stdlib helpers (parse/compare/satisfies/bump)",
        .source_file = "native_integration_tests/semver_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/semver_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_regexp_stdlib_tests(void) {
    printf("\n--- Regexp Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Regexp stdlib helpers (compile/match/find/replace/split)",
        .source_file = "native_integration_tests/regexp_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/regexp_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_toml_stdlib_tests(void) {
    printf("\n--- TOML Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "TOML stdlib helpers (parse/stringify/getters)",
        .source_file = "native_integration_tests/toml_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/toml_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_yaml_stdlib_tests(void) {
    printf("\n--- YAML Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "YAML stdlib helpers (parse/stringify/getters)",
        .source_file = "native_integration_tests/yaml_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/yaml_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_ini_stdlib_tests(void) {
    printf("\n--- INI Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "INI stdlib helpers (parse/stringify/getters)",
        .source_file = "native_integration_tests/ini_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/ini_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_msgpack_stdlib_tests(void) {
    printf("\n--- MessagePack Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "MessagePack stdlib helpers (encode/decode/map/array wrappers)",
        .source_file = "native_integration_tests/msgpack_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/msgpack_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_path_stdlib_tests(void) {
    printf("\n--- Path Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Path stdlib helpers (clean/join/base/dir/ext)",
        .source_file = "native_integration_tests/path_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/path_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_glob_stdlib_tests(void) {
    printf("\n--- Glob Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Glob stdlib helpers (match/**/filter wrappers)",
        .source_file = "native_integration_tests/glob_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/glob_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_fs_stdlib_tests(void) {
    printf("\n--- FS Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "FS stdlib helpers (text/bytes/lines/json/copy/move)",
        .source_file = "native_integration_tests/fs_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/fs_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_csv_stdlib_tests(void) {
    printf("\n--- CSV Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "CSV stdlib helpers (parse/stringify/header/file)",
        .source_file = "native_integration_tests/csv_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/csv_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_config_stdlib_tests(void) {
    printf("\n--- Config Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Config stdlib helpers (dotenv/env merge + typed getters)",
        .source_file = "native_integration_tests/config_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/config_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_cli_stdlib_tests(void) {
    printf("\n--- CLI Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "CLI stdlib helpers (flags/positionals + typed getters)",
        .source_file = "native_integration_tests/cli_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/cli_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_collections_stdlib_tests(void) {
    printf("\n--- Collections Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Collections stdlib helpers (dedup/count/chunk/set ops)",
        .source_file = "native_integration_tests/collections_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/collections_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_time_stdlib_tests(void) {
    printf("\n--- Time Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Time stdlib helpers (durations/deadlines/date formatting)",
        .source_file = "native_integration_tests/time_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/time_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_encoding_stdlib_tests(void) {
    printf("\n--- Encoding Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Encoding stdlib helpers (hex/base64/base64url)",
        .source_file = "native_integration_tests/encoding_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/encoding_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_text_stdlib_tests(void) {
    printf("\n--- Text Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Text stdlib helpers (split/slug/case/truncate)",
        .source_file = "native_integration_tests/text_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/text_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_log_stdlib_tests(void) {
    printf("\n--- Log Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Log stdlib helpers (leveled logging + contextual fields)",
        .source_file = "native_integration_tests/log_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/log_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_template_stdlib_tests(void) {
    printf("\n--- Template Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Template stdlib helpers ({{key}} parsing + strict/default rendering)",
        .source_file = "native_integration_tests/template_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/template_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_testing_stdlib_tests(void) {
    printf("\n--- Testing Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Testing stdlib helpers (assertions + error-code checks)",
        .source_file = "native_integration_tests/testing_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/testing_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_validate_stdlib_tests(void) {
    printf("\n--- Validate Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Validate stdlib helpers (email/url/uuid/required fields)",
        .source_file = "native_integration_tests/validate_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/validate_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_datetime_stdlib_tests(void) {
    printf("\n--- Datetime Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Datetime stdlib helpers (RFC3339 parse/format + offsets)",
        .source_file = "native_integration_tests/datetime_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/datetime_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_retry_stdlib_tests(void) {
    printf("\n--- Retry Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Retry stdlib module (backoff + circuit-breaker helpers)",
        .source_file = "native_integration_tests/retry_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/retry_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_rate_limit_stdlib_tests(void) {
    printf("\n--- Rate-limit Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Rate-limit stdlib module (token-bucket + fixed-window)",
        .source_file = "native_integration_tests/rate_limit_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/rate_limit_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_hash_stdlib_tests(void) {
    printf("\n--- Hash Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Hash stdlib module (stable hashes + adler32 + fingerprints)",
        .source_file = "native_integration_tests/hash_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/hash_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_crypto_stdlib_tests(void) {
    printf("\n--- Crypto Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Crypto stdlib module (sha256 + hmac + pbkdf2 helpers)",
        .source_file = "native_integration_tests/crypto_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/crypto_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_future_core_tests(void) {
    printf("\n--- Future Runtime Core Tests ---\n");

    TestConfig config = {
        .name = "Future runtime core (pending/resolved state and typed payloads)",
        .source_file = "native_integration_tests/future_core_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/future_core_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);

    TestConfig pending_get = {
        .name = "Future runtime core (reject get on pending future)",
        .source_file = "native_integration_tests/future_core_pending_get_error.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };

    run_test(&pending_get);
}

void run_async_basic_tests(void) {
    printf("\n--- Async Basic Tests ---\n");

    TestConfig config = {
        .name = "Async runtime core (async func without await returns ready Future)",
        .source_file = "native_integration_tests/async_basic_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/async_basic_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_async_await_tests(void) {
    printf("\n--- Async Await Tests ---\n");

    TestConfig config = {
        .name = "Async runtime core (await suspend/resume over futures)",
        .source_file = "native_integration_tests/async_await_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/async_await_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_async_control_flow_tests(void) {
    printf("\n--- Async Control-Flow Tests ---\n");

    TestConfig config = {
        .name = "Async runtime semantics (branches, nested blocks, loops, continue across await)",
        .source_file = "native_integration_tests/async_control_flow_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/async_control_flow_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_async_panic_tests(void) {
    printf("\n--- Async Panic/Error Tests ---\n");

    TestConfig config = {
        .name = "Async runtime semantics (defer, ?, and panic propagation)",
        .source_file = "native_integration_tests/async_panic_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/async_panic_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);

    TestConfig panic_get = {
        .name = "Async runtime semantics (futureGet rethrows panicked future)",
        .source_file = "native_integration_tests/async_panic_get_error.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };

    run_test(&panic_get);
}

void run_async_timer_tests(void) {
    printf("\n--- Async Timer Tests ---\n");

    TestConfig config = {
        .name = "Async runtime semantics (timer-backed asyncSleep)",
        .source_file = "native_integration_tests/async_timer_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/async_timer_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_async_channel_tests(void) {
    printf("\n--- Async Channel Tests ---\n");

    TestConfig config = {
        .name = "Async runtime semantics (channel send/recv awaitables)",
        .source_file = "native_integration_tests/async_channel_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/async_channel_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_async_stdlib_tests(void) {
    printf("\n--- Async Stdlib Tests ---\n");

    TestConfig config = {
        .name = "Async stdlib module (awaitAll and awaitAny helpers)",
        .source_file = "native_integration_tests/async_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/async_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_random_stdlib_tests(void) {
    printf("\n--- Random Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Random stdlib module (tokens + bytes + shuffle/sample helpers)",
        .source_file = "native_integration_tests/random_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/random_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_sort_stdlib_tests(void) {
    printf("\n--- Sort Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Sort stdlib module (int/double sorting + binary search + top-k + unique)",
        .source_file = "native_integration_tests/sort_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/sort_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_sort_reverse_builtin_tests(void) {
    printf("\n--- Sort/Reverse Builtin Tests ---\n");

    TestConfig config = {
        .name = "Array sort/reverse builtins (in-place mutation + mixed array safety)",
        .source_file = "native_integration_tests/sort_reverse_builtin_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/sort_reverse_builtin_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_cache_stdlib_tests(void) {
    printf("\n--- Cache Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Cache stdlib module (ttl + eviction + typed access)",
        .source_file = "native_integration_tests/cache_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/cache_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_lru_stdlib_tests(void) {
    printf("\n--- LRU Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "LRU stdlib module (bounded recency cache + eviction metadata)",
        .source_file = "native_integration_tests/lru_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/lru_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_pool_stdlib_tests(void) {
    printf("\n--- Pool Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Pool stdlib module (lease/release + typed wrappers)",
        .source_file = "native_integration_tests/pool_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/pool_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_queue_stdlib_tests(void) {
    printf("\n--- Queue Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Queue stdlib module (queue/deque + bounded + typed wrappers)",
        .source_file = "native_integration_tests/queue_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/queue_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_heap_stdlib_tests(void) {
    printf("\n--- Heap Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Heap stdlib module (priority queue + bounded + typed wrappers)",
        .source_file = "native_integration_tests/heap_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/heap_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_graph_stdlib_tests(void) {
    printf("\n--- Graph Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Graph stdlib module (BFS + shortest-path + topological sort)",
        .source_file = "native_integration_tests/graph_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/graph_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_mathx_stdlib_tests(void) {
    printf("\n--- Mathx Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Mathx stdlib module (summary/percentile/histogram/rolling window)",
        .source_file = "native_integration_tests/mathx_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/mathx_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_bitset_stdlib_tests(void) {
    printf("\n--- Bitset Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Bitset stdlib module (dense set/rank/select + set algebra)",
        .source_file = "native_integration_tests/bitset_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/bitset_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_bloom_stdlib_tests(void) {
    printf("\n--- Bloom Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Bloom stdlib module (approx membership + union + stats)",
        .source_file = "native_integration_tests/bloom_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/bloom_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_concurrency_stdlib_tests(void) {
    printf("\n--- Concurrency Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Concurrency stdlib module (channels/shared/thread/arc wrappers)",
        .source_file = "native_integration_tests/concurrency_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/concurrency_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_context_stdlib_tests(void) {
    printf("\n--- Context Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Context stdlib module (cancel/deadline propagation)",
        .source_file = "native_integration_tests/context_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/context_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_task_stdlib_tests(void) {
    printf("\n--- Task Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Task stdlib module (structured groups + cancel-on-error)",
        .source_file = "native_integration_tests/task_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/task_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_process_stdlib_tests(void) {
    printf("\n--- Process Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "Process stdlib module (spawn/wait/kill/stdin/context)",
        .source_file = "native_integration_tests/process_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/process_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_sqlite_stdlib_tests(void) {
    printf("\n--- SQLite Stdlib Helper Tests ---\n");

    TestConfig config = {
        .name = "SQLite stdlib module (open/exec/query/prepare/step/finalize)",
        .source_file = "native_integration_tests/sqlite_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/sqlite_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_sqlite_runtime_availability_tests(void) {
    printf("\n--- SQLite Runtime Availability Tests ---\n");

    TestConfig config = {
        .name = "SQLite runtime availability builtin (sqliteIsAvailable)",
        .source_file = "native_integration_tests/sqlite_runtime_availability_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/sqlite_runtime_availability_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_error_handling_tests(void) {
    printf("\n--- Error Handling Tests ---\n");

    TestConfig config = {
        .name = "Error handling (? + must + Error.data)",
        .source_file = "native_integration_tests/error_handling_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/error_handling_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);

    TestConfig must_panic = {
        .name = "Error handling (must panics on err)",
        .source_file = "native_integration_tests/must_panic_denied.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&must_panic);
}

void run_gc_tests(void) {
    printf("\n--- GC / Cycle Collector Tests ---\n");

    TestConfig config = {
        .name = "Cycle collector (gcCollect)",
        .source_file = "native_integration_tests/gc_cycle_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/gc_cycle_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);

    TestConfig map_cycles = {
        .name = "Cycle collector (gcCollect) - map self-cycle",
        .source_file = "native_integration_tests/gc_cycle_map_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/gc_cycle_map_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&map_cycles);
}

void run_map_tests(void) {
    printf("\n--- Map Builtin Tests ---\n");

    TestConfig config = {
        .name = "Map builtins (mapSet/mapGet/mapHas/mapDelete/mapCount)",
        .source_file = "native_integration_tests/map_builtin_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/map_builtin_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);

    TestConfig highbit = {
        .name = "Map builtins (high-bit int keys + churn)",
        .source_file = "native_integration_tests/map_highbit_int_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/map_highbit_int_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&highbit);
}

void run_array_tests(void) {
    printf("\n--- Array Operation Tests ---\n");

    TestConfig config = {
        .name = "Array Operations (push/pop/keys/values)",
        .source_file = "native_integration_tests/array_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/array_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_function_call_tests(void) {
    printf("\n--- User Function Call Tests ---\n");

    TestConfig config = {
        .name = "User Function Calls (args/return/nesting)",
        .source_file = "native_integration_tests/function_call_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/function_call_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);

    TestConfig forward = {
        .name = "User Function Calls (forward declaration)",
        .source_file = "native_integration_tests/forward_decl_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/forward_decl_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&forward);
}

void run_assignment_tests(void) {
    printf("\n--- Assignment Tests ---\n");

    TestConfig config = {
        .name = "Assignments (variables and array indices)",
        .source_file = "native_integration_tests/assignment_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/assignment_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_comparison_tests(void) {
    printf("\n--- Comparison Tests ---\n");

    TestConfig config = {
        .name = "Comparisons (<, <=, >, >=)",
        .source_file = "native_integration_tests/comparison_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/comparison_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_short_circuit_tests(void) {
    printf("\n--- Short-Circuit Semantics Tests ---\n");

    TestConfig config = {
        .name = "Logical short-circuit (&& / || runtime semantics)",
        .source_file = "native_integration_tests/short_circuit_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/short_circuit_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_scope_tests(void) {
    printf("\n--- Scope Tests ---\n");

    TestConfig config = {
        .name = "Block scoping (shadowing)",
        .source_file = "native_integration_tests/scope_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/scope_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_global_init_tests(void) {
    printf("\n--- Global Initialization Tests ---\n");

    TestConfig config = {
        .name = "Globals (top-level init runs before main)",
        .source_file = "native_integration_tests/global_init_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/global_init_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_loop_tests(void) {
    printf("\n--- Loop Tests ---\n");

    TestConfig config = {
        .name = "Loops (while/foreach with break/continue)",
        .source_file = "native_integration_tests/loop_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/loop_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);

    TestConfig invalid_break = {
        .name = "Loops (reject break outside loop)",
        .source_file = "native_integration_tests/break_outside_loop.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };

    run_test(&invalid_break);
}

void run_switch_tests(void) {
    printf("\n--- Switch Tests ---\n");

    TestConfig config = {
        .name = "Switch (value dispatch without fallthrough)",
        .source_file = "native_integration_tests/switch_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/switch_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_pattern_let_tests(void) {
    printf("\n--- Pattern Let Tests ---\n");

    TestConfig config = {
        .name = "Pattern let (if let / while let enum destructuring)",
        .source_file = "native_integration_tests/pattern_let_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/pattern_let_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_match_guard_tests(void) {
    printf("\n--- Match Guard Tests ---\n");

    TestConfig config = {
        .name = "Match guards (ordered guard checks and payload-bound guards)",
        .source_file = "native_integration_tests/match_guard_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/match_guard_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_match_expression_tests(void) {
    printf("\n--- Match Expression Tests ---\n");

    TestConfig config = {
        .name = "Match expressions (value-producing exhaustive matches)",
        .source_file = "native_integration_tests/match_expression_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/match_expression_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_match_block_expression_tests(void) {
    printf("\n--- Match Block Expression Tests ---\n");

    TestConfig config = {
        .name = "Match expressions with block-valued arms",
        .source_file = "native_integration_tests/match_block_expression_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/match_block_expression_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_if_expression_tests(void) {
    printf("\n--- If Expression Tests ---\n");

    TestConfig config = {
        .name = "If expressions (value-producing branches and else-if chains)",
        .source_file = "native_integration_tests/if_expression_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/if_expression_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_pattern_alternation_tests(void) {
    printf("\n--- Pattern Alternation Tests ---\n");

    TestConfig config = {
        .name = "Pattern alternation in match/if let/while let",
        .source_file = "native_integration_tests/pattern_alternation_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/pattern_alternation_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_structural_pattern_tests(void) {
    printf("\n--- Structural Pattern Tests ---\n");

    TestConfig config = {
        .name = "Tuple and record destructuring patterns",
        .source_file = "native_integration_tests/structural_pattern_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/structural_pattern_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_import_tests(void) {
    printf("\n--- Import Tests ---\n");

    TestConfig ok = {
        .name = "Imports (load dependency before main)",
        .source_file = "native_integration_tests/import_tests_main.tblo",
        .expected_file = "native_integration_tests/expected_output/import_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&ok);

    TestConfig lock_import = {
        .name = "Imports (lock-aware vendor from module root)",
        .source_file = "native_integration_tests/fixtures/module_root_lock_import/src/main.tblo",
        .expected_file = "native_integration_tests/expected_output/module_root_lock_import.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&lock_import);

    TestConfig init = {
        .name = "Imports (execute imported global initializers)",
        .source_file = "native_integration_tests/import_global_init_main.tblo",
        .expected_file = "native_integration_tests/expected_output/import_global_init_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&init);

    TestConfig traversal = {
        .name = "Imports (deny traversal)",
        .source_file = "native_integration_tests/import_escape_denied.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&traversal);

    TestConfig circular = {
        .name = "Imports (detect circular imports)",
        .source_file = "native_integration_tests/fixtures/circular_a.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&circular);

    TestConfig imported_main = {
        .name = "Imports (reject imported main())",
        .source_file = "native_integration_tests/import_main_denied.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&imported_main);
}

void run_semantic_error_tests(void) {
    printf("\n--- Language Semantics Error Tests ---\n");

    TestConfig invalid_return = {
        .name = "Semantics (reject return outside function)",
        .source_file = "native_integration_tests/return_outside_function.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&invalid_return);

    TestConfig nested_func = {
        .name = "Semantics (reject nested function declaration)",
        .source_file = "native_integration_tests/nested_function_decl_denied.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&nested_func);

    TestConfig block_import = {
        .name = "Semantics (reject import inside block)",
        .source_file = "native_integration_tests/import_inside_block_denied.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&block_import);

    TestConfig invalid_main = {
        .name = "Semantics (reject main with parameters)",
        .source_file = "native_integration_tests/main_params_denied.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&invalid_main);

    TestConfig builtin_decl = {
        .name = "Semantics (reject builtin name declarations)",
        .source_file = "native_integration_tests/builtin_name_decl_denied.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&builtin_decl);

    TestConfig truthy_string = {
        .name = "Semantics (reject string condition)",
        .source_file = "native_integration_tests/truthiness_string_denied.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&truthy_string);

    TestConfig truthy_array = {
        .name = "Semantics (reject array condition)",
        .source_file = "native_integration_tests/truthiness_array_denied.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&truthy_array);

    TestConfig truthy_record = {
        .name = "Semantics (reject record condition)",
        .source_file = "native_integration_tests/truthiness_record_denied.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&truthy_record);

    TestConfig const_pool_wide = {
        .name = "Semantics (wide constant/global operands)",
        .source_file = "native_integration_tests/constant_pool_wide_indices_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/constant_pool_wide_indices_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&const_pool_wide);

    TestConfig https_reject = {
        .name = "Networking (HTTPS URL behavior)",
        .source_file = "native_integration_tests/https_rejection_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/https_rejection_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&https_reject);
}

void run_fileio_tests(void) {
    printf("\n--- File I/O Tests ---\n");

    TestConfig allowed = {
        .name = "File I/O sandbox (allowed paths)",
        .source_file = "native_integration_tests/fileio_sandbox_allowed.tblo",
        .expected_file = "native_integration_tests/expected_output/fileio_sandbox_allowed.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&allowed);

    TestConfig denied = {
        .name = "File I/O sandbox (deny traversal)",
        .source_file = "native_integration_tests/fileio_sandbox_escape_denied.tblo",
        .expected_file = "native_integration_tests/expected_output/fileio_sandbox_escape_denied.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&denied);
}

void run_http_request_tests(void) {
    printf("\n--- HTTP Request API Tests ---\n");

    TestConfig config = {
        .name = "HTTP request API (validation + timeout + https rejection)",
        .source_file = "native_integration_tests/http_request_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/http_request_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_tls_builtin_tests(void) {
    printf("\n--- TLS Builtin Tests ---\n");

    TestConfig config = {
        .name = "TLS builtins (availability + argument validation)",
        .source_file = "native_integration_tests/tls_builtin_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/tls_builtin_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_https_localhost_insecure_tests(void) {
    printf("\n--- HTTPS Localhost (Insecure Verify Opt-in) Tests ---\n");

    TestConfig config = {
        .name = "HTTPS localhost via insecure verify opt-in (deterministic fixture)",
        .source_file = "native_integration_tests/https_localhost_insecure_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/https_localhost_insecure_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_http_server_primitives_tests(void) {
    printf("\n--- HTTP Server Primitive Tests ---\n");

    TestConfig config = {
        .name = "HTTP server primitives (listen/accept/read/write)",
        .source_file = "native_integration_tests/http_server_primitives_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/http_server_primitives_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_http_stdlib_server_tests(void) {
    printf("\n--- HTTP Stdlib Server Tests ---\n");

    TestConfig config = {
        .name = "HTTP stdlib server (routes + middleware + timeout)",
        .source_file = "native_integration_tests/http_stdlib_server_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/http_stdlib_server_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_http_query_helpers_tests(void) {
    printf("\n--- HTTP Query Helper Tests ---\n");

    TestConfig config = {
        .name = "HTTP query helpers (string/int/bool + defaults + parse errors)",
        .source_file = "native_integration_tests/http_query_helpers_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/http_query_helpers_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_http_thread_pool_tests(void) {
    printf("\n--- HTTP Thread Pool Tests ---\n");

    TestConfig config = {
        .name = "HTTP stdlib thread pool (worker runtimes + channel dispatch)",
        .source_file = "native_integration_tests/http_thread_pool_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/http_thread_pool_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_http_serve_loop_tests(void) {
    printf("\n--- HTTP Serve Loop Tests ---\n");

    TestConfig config = {
        .name = "HTTP serve loop (graceful shutdown + backpressure + timeout + request-limit behavior)",
        .source_file = "native_integration_tests/http_serve_loop_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/http_serve_loop_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_http_lifecycle_stress_tests(void) {
    printf("\n--- HTTP Lifecycle Stress Tests ---\n");

    TestConfig config = {
        .name = "HTTP lifecycle stress (repeated start/serve/stop + join, many/threadpool)",
        .source_file = "native_integration_tests/http_lifecycle_stress_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/http_lifecycle_stress_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_http_client_wrappers_tests(void) {
    printf("\n--- HTTP Client Wrapper Tests ---\n");

    TestConfig config = {
        .name = "HTTP client wrappers (typed response + 2xx/json helpers)",
        .source_file = "native_integration_tests/http_client_wrappers_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/http_client_wrappers_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_http_client_connection_pool_tests(void) {
    printf("\n--- HTTP Client Connection Pool Tests ---\n");

    TestConfig config = {
        .name = "HTTP client keep-alive pool (single-socket reuse across sequential requests)",
        .source_file = "native_integration_tests/http_client_connection_pool_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/http_client_connection_pool_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_http_gzip_tests(void) {
    printf("\n--- HTTP Gzip Tests ---\n");

    TestConfig config = {
        .name = "HTTP gzip (client decode + stdlib server negotiation + validation)",
        .source_file = "native_integration_tests/http_gzip_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/http_gzip_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_http_client_streaming_tests(void) {
    printf("\n--- HTTP Client Streaming Tests ---\n");

    TestConfig config = {
        .name = "HTTP client streaming (status/headers head + chunked body read)",
        .source_file = "native_integration_tests/http_client_streaming_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/http_client_streaming_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_io_stream_helpers_tests(void) {
    printf("\n--- Streaming I/O Helper Tests ---\n");

    TestConfig config = {
        .name = "Streaming I/O helpers (ioReadLine/ioReadAll/ioWriteAll/ioCopy)",
        .source_file = "native_integration_tests/io_stream_helpers_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/io_stream_helpers_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_io_stdlib_tests(void) {
    printf("\n--- I/O Stdlib Module Tests ---\n");

    TestConfig config = {
        .name = "I/O stdlib module (Reader/Writer + lines/copy helpers)",
        .source_file = "native_integration_tests/io_stdlib_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/io_stdlib_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_threading_runtime_tests(void) {
    printf("\n--- Threading Runtime Tests ---\n");

    TestConfig config = {
        .name = "Threading runtime (spawn/join + channels + shared state)",
        .source_file = "native_integration_tests/threading_runtime_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/threading_runtime_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_complex_tests(void) {
    printf("\n--- Complex Scenario Tests ---\n");

    TestConfig config = {
        .name = "Complex Scenarios (chained/nested calls)",
        .source_file = "native_integration_tests/complex_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/complex_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };

    run_test(&config);
}

void run_real_world_tests(void) {
    printf("\n--- Real World Program Tests ---\n");

    TestConfig bools = {
        .name = "Boolean logic (!, &&, ||, ==, !=, precedence)",
        .source_file = "native_integration_tests/boolean_logic_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/boolean_logic_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&bools);

    TestConfig bitwise = {
        .name = "Bitwise ops (&, |, ^, ~) on int/bigint",
        .source_file = "native_integration_tests/bitwise_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/bitwise_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&bitwise);

    TestConfig strings = {
        .name = "Strings (concatenation + length)",
        .source_file = "native_integration_tests/string_concat_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/string_concat_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&strings);

    TestConfig recursion = {
        .name = "Recursion (factorial/fibonacci)",
        .source_file = "native_integration_tests/recursion_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/recursion_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&recursion);

    TestConfig algorithms = {
        .name = "Arrays (reverse/filter)",
        .source_file = "native_integration_tests/array_algorithms_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/array_algorithms_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&algorithms);

    TestConfig matrix = {
        .name = "Nested arrays (matrix row sums + transpose)",
        .source_file = "native_integration_tests/matrix_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/matrix_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&matrix);

    TestConfig pipeline = {
        .name = "Data pipeline (parse strings + aggregate)",
        .source_file = "native_integration_tests/data_pipeline_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/data_pipeline_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&pipeline);

    TestConfig fileio_lines = {
        .name = "File I/O (write_line/read_line/read_all)",
        .source_file = "native_integration_tests/fileio_line_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/fileio_line_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&fileio_lines);

    TestConfig fileio_streaming = {
        .name = "File I/O (file_open/file_read_line/file_close)",
        .source_file = "native_integration_tests/fileio_streaming_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/fileio_streaming_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&fileio_streaming);

    TestConfig time_builtins = {
        .name = "Time/Date builtins",
        .source_file = "native_integration_tests/time_builtin_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/time_builtin_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&time_builtins);

    TestConfig random_builtins = {
        .name = "Random builtins",
        .source_file = "native_integration_tests/random_builtin_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/random_builtin_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&random_builtins);

    TestConfig bigint = {
        .name = "BigInt (literals + arithmetic + factorial)",
        .source_file = "native_integration_tests/bigint_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/bigint_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&bigint);

    TestConfig bigint_large = {
        .name = "BigInt (large values)",
        .source_file = "native_integration_tests/bigint_large_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/bigint_large_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&bigint_large);

    TestConfig rsa = {
        .name = "RSA library (keygen/encrypt/sign)",
        .source_file = "native_integration_tests/rsa_library_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/rsa_library_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&rsa);
}

void run_observability_tests(void) {
    printf("\n--- Observability Tests ---\n");

    TestConfig config = {
        .name = "Observability (logJson + metrics + HTTP instrumentation)",
        .source_file = "native_integration_tests/observability_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/observability_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&config);
}

void run_record_tests(void) {
    printf("\n--- Record Tests ---\n");

    TestConfig basic = {
        .name = "Records (basic declaration and field access)",
        .source_file = "native_integration_tests/record_basic_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/record_basic_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&basic);

    TestConfig nested = {
        .name = "Records (nested records)",
        .source_file = "native_integration_tests/record_nested_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/record_nested_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&nested);

    TestConfig arrays = {
        .name = "Records (arrays of records)",
        .source_file = "native_integration_tests/record_array_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/record_array_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&arrays);

    TestConfig field_access = {
        .name = "Records (field access operations)",
        .source_file = "native_integration_tests/record_field_access_tests.tblo",
        .expected_file = "native_integration_tests/expected_output/record_field_access_tests.expected",
        .should_pass = 1,
        .cleanup_files = 0
    };
    run_test(&field_access);

    TestConfig type_checking = {
        .name = "Records (type checking errors)",
        .source_file = "native_integration_tests/record_type_checking_tests.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&type_checking);

    TestConfig missing_field = {
        .name = "Records (reject missing fields in literal)",
        .source_file = "native_integration_tests/record_missing_field_denied.tblo",
        .expected_file = NULL,
        .should_pass = 0,
        .cleanup_files = 0
    };
    run_test(&missing_field);
}

int main(void) {
    printf("=== Native Function Integration Tests ===\n");

    run_io_tests();
    run_conversion_tests();
    run_json_tests();
    run_json_stdlib_tests();
    run_path_stdlib_tests();
    run_glob_stdlib_tests();
    run_fs_stdlib_tests();
    run_csv_stdlib_tests();
    run_config_stdlib_tests();
    run_cli_stdlib_tests();
    run_collections_stdlib_tests();
    run_time_stdlib_tests();
    run_encoding_stdlib_tests();
    run_text_stdlib_tests();
    run_log_stdlib_tests();
    run_template_stdlib_tests();
    run_testing_stdlib_tests();
    run_validate_stdlib_tests();
    run_datetime_stdlib_tests();
    run_retry_stdlib_tests();
    run_rate_limit_stdlib_tests();
    run_hash_stdlib_tests();
    run_crypto_stdlib_tests();
    run_future_core_tests();
    run_async_basic_tests();
    run_async_await_tests();
    run_async_control_flow_tests();
    run_async_panic_tests();
    run_async_timer_tests();
    run_async_channel_tests();
    run_async_stdlib_tests();
    run_random_stdlib_tests();
    run_sort_stdlib_tests();
    run_sort_reverse_builtin_tests();
    run_cache_stdlib_tests();
    run_lru_stdlib_tests();
    run_pool_stdlib_tests();
    run_queue_stdlib_tests();
    run_heap_stdlib_tests();
    run_graph_stdlib_tests();
    run_mathx_stdlib_tests();
    run_bitset_stdlib_tests();
    run_bloom_stdlib_tests();
    run_concurrency_stdlib_tests();
    run_context_stdlib_tests();
    run_task_stdlib_tests();
    run_process_stdlib_tests();
    run_sqlite_stdlib_tests();
    run_sqlite_runtime_availability_tests();
    run_url_stdlib_tests();
    run_uuid_stdlib_tests();
    run_netip_stdlib_tests();
    run_semver_stdlib_tests();
    run_regexp_stdlib_tests();
    run_toml_stdlib_tests();
    run_yaml_stdlib_tests();
    run_ini_stdlib_tests();
    run_msgpack_stdlib_tests();
    run_error_handling_tests();
    run_gc_tests();
    run_map_tests();
    run_array_tests();
    run_function_call_tests();
    run_assignment_tests();
    run_comparison_tests();
    run_short_circuit_tests();
    run_scope_tests();
    run_global_init_tests();
    run_loop_tests();
    run_switch_tests();
    run_pattern_let_tests();
    run_match_guard_tests();
    run_match_expression_tests();
    run_match_block_expression_tests();
    run_if_expression_tests();
    run_pattern_alternation_tests();
    run_structural_pattern_tests();
    run_import_tests();
    run_semantic_error_tests();
    run_tls_builtin_tests();
    run_https_localhost_insecure_tests();
    run_http_request_tests();
    run_http_server_primitives_tests();
    run_http_stdlib_server_tests();
    run_http_query_helpers_tests();
    run_http_thread_pool_tests();
    run_http_serve_loop_tests();
    run_http_lifecycle_stress_tests();
    run_http_client_wrappers_tests();
    run_http_client_connection_pool_tests();
    run_http_gzip_tests();
    run_http_client_streaming_tests();
    run_io_stream_helpers_tests();
    run_io_stdlib_tests();
    run_threading_runtime_tests();
    run_fileio_tests();
    run_complex_tests();
    run_real_world_tests();
    run_observability_tests();
    run_record_tests();

    print_summary();

    return tests_failed > 0 ? 1 : 0;
}
