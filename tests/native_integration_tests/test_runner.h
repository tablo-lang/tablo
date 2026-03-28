#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OUTPUT_SIZE 65536

typedef struct {
    char* data;
    size_t size;
    size_t capacity;
    FILE* file;
    int old_stdout;
} CapturedOutput;

typedef struct {
    char* name;
    char* source_file;
    char* expected_file;
    int should_pass;
    int cleanup_files;
    char** test_files_to_create;
    int test_files_count;
} TestConfig;

void output_init(CapturedOutput* output);
void output_capture(CapturedOutput* output);
void output_restore(CapturedOutput* output);
void output_free(CapturedOutput* output);

int compare_outputs(const char* actual, const char* expected);
char* read_file_contents(const char* filename);
int write_test_file(const char* filename, const char* content);
void cleanup_test_file(const char* filename);

int run_test(TestConfig* config);
void print_test_result(const char* test_name, int passed, const char* actual_output, const char* expected_output);

void run_io_tests(void);
void run_conversion_tests(void);
void run_json_tests(void);
void run_json_stdlib_tests(void);
void run_path_stdlib_tests(void);
void run_glob_stdlib_tests(void);
void run_fs_stdlib_tests(void);
void run_csv_stdlib_tests(void);
void run_config_stdlib_tests(void);
void run_cli_stdlib_tests(void);
void run_collections_stdlib_tests(void);
void run_time_stdlib_tests(void);
void run_encoding_stdlib_tests(void);
void run_text_stdlib_tests(void);
void run_log_stdlib_tests(void);
void run_template_stdlib_tests(void);
void run_testing_stdlib_tests(void);
void run_validate_stdlib_tests(void);
void run_datetime_stdlib_tests(void);
void run_retry_stdlib_tests(void);
void run_rate_limit_stdlib_tests(void);
void run_hash_stdlib_tests(void);
void run_crypto_stdlib_tests(void);
void run_future_core_tests(void);
void run_async_basic_tests(void);
void run_async_await_tests(void);
void run_async_control_flow_tests(void);
void run_async_panic_tests(void);
void run_async_timer_tests(void);
void run_async_channel_tests(void);
void run_async_stdlib_tests(void);
void run_random_stdlib_tests(void);
void run_sort_stdlib_tests(void);
void run_sort_reverse_builtin_tests(void);
void run_cache_stdlib_tests(void);
void run_lru_stdlib_tests(void);
void run_pool_stdlib_tests(void);
void run_queue_stdlib_tests(void);
void run_heap_stdlib_tests(void);
void run_graph_stdlib_tests(void);
void run_mathx_stdlib_tests(void);
void run_bitset_stdlib_tests(void);
void run_bloom_stdlib_tests(void);
void run_concurrency_stdlib_tests(void);
void run_url_stdlib_tests(void);
void run_uuid_stdlib_tests(void);
void run_netip_stdlib_tests(void);
void run_semver_stdlib_tests(void);
void run_regexp_stdlib_tests(void);
void run_toml_stdlib_tests(void);
void run_yaml_stdlib_tests(void);
void run_ini_stdlib_tests(void);
void run_msgpack_stdlib_tests(void);
void run_http_query_helpers_tests(void);
void run_https_localhost_insecure_tests(void);
void run_http_serve_loop_tests(void);
void run_http_lifecycle_stress_tests(void);
void run_http_client_wrappers_tests(void);
void run_http_client_connection_pool_tests(void);
void run_short_circuit_tests(void);
void run_array_tests(void);
void run_fileio_tests(void);
void run_complex_tests(void);
void print_summary(void);

#endif
