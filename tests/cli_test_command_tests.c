#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#endif

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                         \
    do {                                               \
        if (cond) {                                    \
            printf("  PASS: %s\n", msg);               \
            tests_passed++;                            \
        } else {                                       \
            printf("  FAIL: %s\n", msg);               \
            tests_failed++;                            \
        }                                              \
    } while (0)

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

static int decode_system_exit_code(int status) {
#ifdef _WIN32
    return status;
#else
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

static const char* find_tablo_executable(void) {
#ifdef _WIN32
    static const char* candidates[] = {
        "..\\build-tablo\\Release\\tablo.exe",
        "..\\build-tablo\\Debug\\tablo.exe",
        "..\\build-tablo\\tablo.exe",
        "..\\build\\Release\\tablo.exe",
        "..\\build\\Debug\\tablo.exe",
        "..\\build\\tablo.exe",
        "../build-tablo/Release/tablo.exe",
        "../build-tablo/Debug/tablo.exe",
        "../build-tablo/tablo.exe",
        "../build/Release/tablo.exe",
        "../build/Debug/tablo.exe",
        "../build/tablo.exe"
    };
#else
    static const char* candidates[] = {
        "../build-tablo/Release/tablo",
        "../build-tablo/tablo",
        "../build-tablo/Debug/tablo",
        "../build/Release/tablo",
        "../build/tablo",
        "../build/Debug/tablo"
    };
#endif

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (file_exists(candidates[i])) {
            return candidates[i];
        }
    }
    return NULL;
}

static const char* find_tablo_test_extension_library(void) {
#ifdef _WIN32
    static const char* candidates[] = {
        "..\\build-tablo\\Release\\tablo_test_extension.dll",
        "..\\build-tablo\\Debug\\tablo_test_extension.dll",
        "..\\build-tablo\\tablo_test_extension.dll",
        "..\\build\\Release\\tablo_test_extension.dll",
        "..\\build\\Debug\\tablo_test_extension.dll",
        "..\\build\\tablo_test_extension.dll",
        "../build-tablo/Release/tablo_test_extension.dll",
        "../build-tablo/Debug/tablo_test_extension.dll",
        "../build-tablo/tablo_test_extension.dll",
        "../build/Release/tablo_test_extension.dll",
        "../build/Debug/tablo_test_extension.dll",
        "../build/tablo_test_extension.dll"
    };
#else
    static const char* candidates[] = {
        "../build-tablo/Release/tablo_test_extension.so",
        "../build-tablo/Debug/tablo_test_extension.so",
        "../build-tablo/tablo_test_extension.so",
        "../build/Release/tablo_test_extension.so",
        "../build/Debug/tablo_test_extension.so",
        "../build/tablo_test_extension.so"
    };
#endif

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (file_exists(candidates[i])) {
            return candidates[i];
        }
    }
    return NULL;
}

static int run_cli_command(const char* exe, const char* args, int expect_success, const char* label) {
    char cmd[2048];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "\"%s\" %s > NUL 2>&1", exe, args);
#else
    snprintf(cmd, sizeof(cmd), "\"%s\" %s > /dev/null 2>&1", exe, args);
#endif

    int raw = system(cmd);
    int code = decode_system_exit_code(raw);
    int ok = expect_success ? (code == 0) : (code != 0);

    TEST_ASSERT(ok, label);
    if (!ok) {
        printf("    command: %s\n", cmd);
        printf("    exit code: %d\n", code);
    }
    return ok;
}

static int file_contains_text(const char* path, const char* needle) {
    if (!path || !needle) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return 0;
    }
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';

    int ok = strstr(buf, needle) != NULL;
    free(buf);
    return ok;
}

static int file_lacks_text(const char* path, const char* needle) {
    return !file_contains_text(path, needle);
}

static int run_raw_command_capture_contains(const char* raw_cmd,
                                            const char* out_path,
                                            const char* needle,
                                            int expect_success,
                                            const char* label) {
    if (!raw_cmd || !out_path || !needle) {
        TEST_ASSERT(0, label);
        return 0;
    }

    char cmd[4096];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "cmd /c \"%s > %s 2>&1\"", raw_cmd, out_path);
#else
    snprintf(cmd, sizeof(cmd), "%s > %s 2>&1", raw_cmd, out_path);
#endif

    int raw = system(cmd);
    int code = decode_system_exit_code(raw);
    int ok_exit = expect_success ? (code == 0) : (code != 0);
    int ok_contains = file_contains_text(out_path, needle);
    int ok = ok_exit && ok_contains;

    TEST_ASSERT(ok, label);
    if (!ok) {
        printf("    command: %s\n", cmd);
        printf("    exit code: %d\n", code);
        printf("    expected output needle: %s\n", needle);
    }

    remove(out_path);
    return ok;
}

static int run_raw_command_capture(const char* raw_cmd,
                                   const char* out_path,
                                   int expect_success,
                                   const char* label) {
    if (!raw_cmd || !out_path) {
        TEST_ASSERT(0, label);
        return 0;
    }

    char cmd[4096];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "cmd /c \"%s > %s 2>&1\"", raw_cmd, out_path);
#else
    snprintf(cmd, sizeof(cmd), "%s > %s 2>&1", raw_cmd, out_path);
#endif

    int raw = system(cmd);
    int code = decode_system_exit_code(raw);
    int ok = expect_success ? (code == 0) : (code != 0);

    TEST_ASSERT(ok, label);
    if (!ok) {
        printf("    command: %s\n", cmd);
        printf("    exit code: %d\n", code);
    }
    return ok;
}

static int write_temp_test_file(char* out_path, size_t out_size, const char* content) {
    if (!out_path || out_size == 0 || !content) return 0;

    srand((unsigned int)time(NULL));
#ifdef _WIN32
    snprintf(out_path, out_size, "tablo_tests\\tmp_cli_fail_%d_test.tblo", rand());
#else
    snprintf(out_path, out_size, "tablo_tests/tmp_cli_fail_%d_test.tblo", rand());
#endif

    FILE* f = fopen(out_path, "wb");
    if (!f) return 0;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return 1;
}

static int write_temp_timeout_test_file(char* out_path, size_t out_size) {
    if (!out_path || out_size == 0) return 0;
    srand((unsigned int)time(NULL) ^ 0x5a5a5a5aU);
#ifdef _WIN32
    snprintf(out_path, out_size, "tablo_tests\\tmp_cli_timeout_%d_test.tblo", rand());
#else
    snprintf(out_path, out_size, "tablo_tests/tmp_cli_timeout_%d_test.tblo", rand());
#endif

    const char* content =
        "func testSpin(): void {\n"
        "    while (true) {\n"
        "    }\n"
        "}\n"
        "func main(): void {\n"
        "    // legacy fallback not used in this timeout test\n"
        "}\n";

    FILE* f = fopen(out_path, "wb");
    if (!f) return 0;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return 1;
}

static int write_temp_lsp_stdio_input(char* out_path, size_t out_size) {
    static const char* init_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}";
    static const char* symbol_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/documentSymbol\","
        "\"params\":{\"textDocument\":{\"uri\":\"tablo_tests/lsp_symbols_test.tblo\"}}}";
    static const char* hover_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"tablo_tests/lsp_symbols_test.tblo\"},"
        "\"position\":{\"line\":20,\"character\":11}}}";
    static const char* definition_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/definition\","
        "\"params\":{\"textDocument\":{\"uri\":\"tablo_tests/lsp_symbols_test.tblo\"},"
        "\"position\":{\"line\":24,\"character\":16}}}";
    static const char* hover_local_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"tablo_tests/lsp_symbols_test.tblo\"},"
        "\"position\":{\"line\":38,\"character\":12}}}";
    static const char* definition_param_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"textDocument/definition\","
        "\"params\":{\"textDocument\":{\"uri\":\"tablo_tests/lsp_symbols_test.tblo\"},"
        "\"position\":{\"line\":36,\"character\":22}}}";
    static const char* definition_type_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/definition\","
        "\"params\":{\"textDocument\":{\"uri\":\"tablo_tests/lsp_symbols_test.tblo\"},"
        "\"position\":{\"line\":41,\"character\":23}}}";
    static const char* hover_generic_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"tablo_tests/lsp_symbols_test.tblo\"},"
        "\"position\":{\"line\":57,\"character\":14}}}";
    static const char* definition_generic_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"textDocument/definition\","
        "\"params\":{\"textDocument\":{\"uri\":\"tablo_tests/lsp_symbols_test.tblo\"},"
        "\"position\":{\"line\":58,\"character\":17}}}";
    static const char* shutdown_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"shutdown\",\"params\":{}}";
    static const char* exit_msg =
        "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":{}}";

    if (!out_path || out_size == 0) return 0;
    srand((unsigned int)time(NULL) ^ 0x13572468U);
#ifdef _WIN32
    snprintf(out_path, out_size, ".tmp_cli_lsp_stdio_%d.in", rand());
#else
    snprintf(out_path, out_size, ".tmp_cli_lsp_stdio_%d.in", rand());
#endif

    FILE* f = fopen(out_path, "wb");
    if (!f) return 0;
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(init_msg), init_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(symbol_msg), symbol_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(hover_msg), hover_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(definition_msg), definition_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(hover_local_msg), hover_local_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(definition_param_msg), definition_param_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(definition_type_msg), definition_type_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(hover_generic_msg), hover_generic_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(definition_generic_msg), definition_generic_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(shutdown_msg), shutdown_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(exit_msg), exit_msg);
    fclose(f);
    return 1;
}

static int write_temp_lsp_diagnostics_input(char* out_path, size_t out_size) {
    static const char* init_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}";
    static const char* did_open_msg =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///virtual/lsp_diag_test.tblo\","
        "\"languageId\":\"tablo\",\"version\":1,"
        "\"text\":\"func main(): void {\\n    var value: int = \\\"oops\\\";\\n}\\n\"}}}";
    static const char* did_close_msg =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///virtual/lsp_diag_test.tblo\"}}}";
    static const char* shutdown_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\",\"params\":{}}";
    static const char* exit_msg =
        "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":{}}";

    if (!out_path || out_size == 0) return 0;
    srand((unsigned int)time(NULL) ^ 0x24681357U);
    snprintf(out_path, out_size, ".tmp_cli_lsp_diag_%d.in", rand());

    FILE* f = fopen(out_path, "wb");
    if (!f) return 0;
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(init_msg), init_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(did_open_msg), did_open_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(did_close_msg), did_close_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(shutdown_msg), shutdown_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(exit_msg), exit_msg);
    fclose(f);
    return 1;
}

static int write_temp_lsp_unsaved_input(char* out_path, size_t out_size) {
    static const char* init_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}";
    static const char* did_open_msg =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///virtual/lsp_unsaved_hover_definition.tblo\","
        "\"languageId\":\"tablo\",\"version\":1,"
        "\"text\":\"record Point {\\n    x: int,\\n    y: int\\n}\\n\\nfunc sample(point: Point): int {\\n    var scratch: int = point.x;\\n    return scratch;\\n}\\n\"}}}";
    static const char* did_change_msg =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///virtual/lsp_unsaved_hover_definition.tblo\",\"version\":2},"
        "\"contentChanges\":[{\"text\":\"record Point {\\n    x: int,\\n    y: int\\n}\\n\\nfunc sample(point: Point): int {\\n    var fresh: int = point.x;\\n    return fresh;\\n}\\n\\nconst CHANGED: int = 1;\\n\"}]}}";
    static const char* symbol_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/documentSymbol\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///virtual/lsp_unsaved_hover_definition.tblo\"}}}";
    static const char* hover_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///virtual/lsp_unsaved_hover_definition.tblo\"},"
        "\"position\":{\"line\":7,\"character\":12}}}";
    static const char* definition_local_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/definition\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///virtual/lsp_unsaved_hover_definition.tblo\"},"
        "\"position\":{\"line\":7,\"character\":12}}}";
    static const char* definition_type_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/definition\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///virtual/lsp_unsaved_hover_definition.tblo\"},"
        "\"position\":{\"line\":5,\"character\":19}}}";
    static const char* shutdown_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"shutdown\",\"params\":{}}";
    static const char* exit_msg =
        "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":{}}";

    if (!out_path || out_size == 0) return 0;
    srand((unsigned int)time(NULL) ^ 0x31415926U);
    snprintf(out_path, out_size, ".tmp_cli_lsp_unsaved_%d.in", rand());

    FILE* f = fopen(out_path, "wb");
    if (!f) return 0;
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(init_msg), init_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(did_open_msg), did_open_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(did_change_msg), did_change_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(symbol_msg), symbol_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(hover_msg), hover_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(definition_local_msg), definition_local_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(definition_type_msg), definition_type_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(shutdown_msg), shutdown_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(exit_msg), exit_msg);
    fclose(f);
    return 1;
}

static int write_temp_dap_stdio_input(char* out_path, size_t out_size) {
    enum {
        GEN2_FRAME = 20001,
        GEN3_FRAME = 30001,
        GEN3_ARGS = 1030001,
        GEN3_LOCALS = 2030001,
        GEN3_POINT_CHILD = 4030000,
        GEN4_FRAME = 40001,
        GEN4_LOCALS = 2040001,
        GEN4_NUMBERS_CHILD = 4040001
    };
    static const char* init_msg =
        "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\","
        "\"arguments\":{\"clientID\":\"tests\",\"adapterID\":\"tablo\"}}";
    static const char* launch_msg =
        "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\","
        "\"arguments\":{\"program\":\"tablo_tests/debug_dap_variables_test.tblo\"}}";
    static const char* set_breakpoints_msg =
        "{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\","
        "\"arguments\":{\"source\":{\"path\":\"tablo_tests/debug_dap_variables_test.tblo\"},"
        "\"breakpoints\":[{\"line\":11}]}}";
    static const char* config_done_msg =
        "{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\",\"arguments\":{}}";
    static const char* threads_msg =
        "{\"seq\":5,\"type\":\"request\",\"command\":\"threads\",\"arguments\":{}}";
    static const char* stack_main_msg =
        "{\"seq\":6,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}";
    static const char* step_in_msg =
        "{\"seq\":7,\"type\":\"request\",\"command\":\"stepIn\",\"arguments\":{\"threadId\":1}}";
    static const char* stack_helper_msg =
        "{\"seq\":8,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}";
    static const char* next_msg =
        "{\"seq\":9,\"type\":\"request\",\"command\":\"next\",\"arguments\":{\"threadId\":1}}";
    char scopes_point_msg[160];
    char variables_point_arguments_msg[180];
    char variables_point_locals_msg[180];
    char variables_point_children_msg[180];
    char evaluate_point_field_msg[200];
    static const char* next_after_point_msg =
        "{\"seq\":15,\"type\":\"request\",\"command\":\"next\",\"arguments\":{\"threadId\":1}}";
    char stale_scopes_after_next_msg[160];
    char stale_variables_after_next_msg[180];
    char scopes_after_next_msg[160];
    char variables_after_next_msg[180];
    char variables_numbers_children_msg[180];
    char evaluate_numbers_index_msg[200];
    char evaluate_global_msg[220];
    char evaluate_missing_msg[200];
    static const char* continue_msg =
        "{\"seq\":24,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}";
    static const char* disconnect_msg =
        "{\"seq\":25,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{}}";

    if (!out_path || out_size == 0) return 0;
#ifdef _WIN32
    snprintf(out_path, out_size, ".tmp_cli_dap_stdio.in");
#else
    snprintf(out_path, out_size, ".tmp_cli_dap_stdio.in");
#endif

    FILE* f = fopen(out_path, "wb");
    if (!f) return 0;

    snprintf(scopes_point_msg,
             sizeof(scopes_point_msg),
             "{\"seq\":10,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%d}}",
             GEN3_FRAME);
    snprintf(variables_point_arguments_msg,
             sizeof(variables_point_arguments_msg),
             "{\"seq\":11,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%d}}",
             GEN3_ARGS);
    snprintf(variables_point_locals_msg,
             sizeof(variables_point_locals_msg),
             "{\"seq\":12,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%d}}",
             GEN3_LOCALS);
    snprintf(variables_point_children_msg,
             sizeof(variables_point_children_msg),
             "{\"seq\":13,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%d}}",
             GEN3_POINT_CHILD);
    snprintf(evaluate_point_field_msg,
             sizeof(evaluate_point_field_msg),
             "{\"seq\":14,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"point.field0\",\"frameId\":%d,\"context\":\"watch\"}}",
             GEN3_FRAME);
    snprintf(stale_scopes_after_next_msg,
             sizeof(stale_scopes_after_next_msg),
             "{\"seq\":16,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%d}}",
             GEN3_FRAME);
    snprintf(stale_variables_after_next_msg,
             sizeof(stale_variables_after_next_msg),
             "{\"seq\":17,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%d}}",
             GEN3_POINT_CHILD);
    snprintf(scopes_after_next_msg,
             sizeof(scopes_after_next_msg),
             "{\"seq\":18,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%d}}",
             GEN4_FRAME);
    snprintf(variables_after_next_msg,
             sizeof(variables_after_next_msg),
             "{\"seq\":19,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%d}}",
             GEN4_LOCALS);
    snprintf(variables_numbers_children_msg,
             sizeof(variables_numbers_children_msg),
             "{\"seq\":20,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%d}}",
             GEN4_NUMBERS_CHILD);
    snprintf(evaluate_numbers_index_msg,
             sizeof(evaluate_numbers_index_msg),
             "{\"seq\":21,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"numbers[1]\",\"frameId\":%d,\"context\":\"watch\"}}",
             GEN4_FRAME);
    snprintf(evaluate_global_msg,
             sizeof(evaluate_global_msg),
             "{\"seq\":22,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"__vml_entry_file\",\"frameId\":%d,\"context\":\"watch\"}}",
             GEN4_FRAME);
    snprintf(evaluate_missing_msg,
             sizeof(evaluate_missing_msg),
             "{\"seq\":23,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"missingName\",\"frameId\":%d,\"context\":\"watch\"}}",
             GEN4_FRAME);

    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(init_msg), init_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(launch_msg), launch_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(set_breakpoints_msg), set_breakpoints_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(config_done_msg), config_done_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(threads_msg), threads_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(stack_main_msg), stack_main_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(step_in_msg), step_in_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(stack_helper_msg), stack_helper_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(next_msg), next_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(scopes_point_msg), scopes_point_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(variables_point_arguments_msg), variables_point_arguments_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(variables_point_locals_msg), variables_point_locals_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(variables_point_children_msg), variables_point_children_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(evaluate_point_field_msg), evaluate_point_field_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(next_after_point_msg), next_after_point_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(stale_scopes_after_next_msg), stale_scopes_after_next_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(stale_variables_after_next_msg), stale_variables_after_next_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(scopes_after_next_msg), scopes_after_next_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(variables_after_next_msg), variables_after_next_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(variables_numbers_children_msg), variables_numbers_children_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(evaluate_numbers_index_msg), evaluate_numbers_index_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(evaluate_global_msg), evaluate_global_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(evaluate_missing_msg), evaluate_missing_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(continue_msg), continue_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(disconnect_msg), disconnect_msg);
    fclose(f);
    return 1;
}

static int write_temp_dap_exception_input(char* out_path, size_t out_size) {
    enum {
        GEN1_FRAME = 10001,
        GEN1_LOCALS = 2010001
    };
    static const char* init_msg =
        "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\","
        "\"arguments\":{\"clientID\":\"tests\",\"adapterID\":\"tablo\"}}";
    static const char* set_exception_breakpoints_msg =
        "{\"seq\":2,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\","
        "\"arguments\":{\"filters\":[\"panic\"]}}";
    static const char* launch_msg =
        "{\"seq\":3,\"type\":\"request\",\"command\":\"launch\","
        "\"arguments\":{\"program\":\"tablo_tests/debug_dap_exception_test.tblo\"}}";
    static const char* config_done_msg =
        "{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\",\"arguments\":{}}";
    static const char* exception_info_msg =
        "{\"seq\":5,\"type\":\"request\",\"command\":\"exceptionInfo\",\"arguments\":{\"threadId\":1}}";
    static const char* stack_trace_msg =
        "{\"seq\":6,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}";
    char scopes_msg[160];
    char variables_msg[180];
    char evaluate_msg[200];
    static const char* continue_msg =
        "{\"seq\":10,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}";
    static const char* disconnect_msg =
        "{\"seq\":11,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{}}";

    if (!out_path || out_size == 0) return 0;
    snprintf(out_path, out_size, ".tmp_cli_dap_exception.in");

    FILE* f = fopen(out_path, "wb");
    if (!f) return 0;

    snprintf(scopes_msg,
             sizeof(scopes_msg),
             "{\"seq\":7,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%d}}",
             GEN1_FRAME);
    snprintf(variables_msg,
             sizeof(variables_msg),
             "{\"seq\":8,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%d}}",
             GEN1_LOCALS);
    snprintf(evaluate_msg,
             sizeof(evaluate_msg),
             "{\"seq\":9,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"current\",\"frameId\":%d,\"context\":\"watch\"}}",
             GEN1_FRAME);

    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(init_msg), init_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(set_exception_breakpoints_msg), set_exception_breakpoints_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(launch_msg), launch_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(config_done_msg), config_done_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(exception_info_msg), exception_info_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(stack_trace_msg), stack_trace_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(scopes_msg), scopes_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(variables_msg), variables_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(evaluate_msg), evaluate_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(continue_msg), continue_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(disconnect_msg), disconnect_msg);
    fclose(f);
    return 1;
}

static int write_temp_dap_stepout_input(char* out_path, size_t out_size) {
    static const char* init_msg =
        "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\","
        "\"arguments\":{\"clientID\":\"tests\",\"adapterID\":\"tablo\"}}";
    static const char* launch_msg =
        "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\","
        "\"arguments\":{\"program\":\"tablo_tests/debug_breakpoint_test.tblo\"}}";
    static const char* set_breakpoints_msg =
        "{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\","
        "\"arguments\":{\"source\":{\"path\":\"tablo_tests/debug_breakpoint_test.tblo\"},"
        "\"breakpoints\":[{\"line\":8}]}}";
    static const char* config_done_msg =
        "{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\",\"arguments\":{}}";
    static const char* step_in_msg =
        "{\"seq\":5,\"type\":\"request\",\"command\":\"stepIn\",\"arguments\":{\"threadId\":1}}";
    static const char* step_out_msg =
        "{\"seq\":6,\"type\":\"request\",\"command\":\"stepOut\",\"arguments\":{\"threadId\":1}}";
    static const char* stack_trace_msg =
        "{\"seq\":7,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}";
    static const char* disconnect_msg =
        "{\"seq\":8,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{}}";

    if (!out_path || out_size == 0) return 0;
    snprintf(out_path, out_size, ".tmp_cli_dap_stepout.in");

    FILE* f = fopen(out_path, "wb");
    if (!f) return 0;

    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(init_msg), init_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(launch_msg), launch_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(set_breakpoints_msg), set_breakpoints_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(config_done_msg), config_done_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(step_in_msg), step_in_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(step_out_msg), step_out_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(stack_trace_msg), stack_trace_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(disconnect_msg), disconnect_msg);
    fclose(f);
    return 1;
}

static int write_temp_dap_stoponentry_input(char* out_path, size_t out_size) {
    enum {
        GEN1_FRAME = 10001
    };
    static const char* init_msg =
        "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\","
        "\"arguments\":{\"clientID\":\"tests\",\"adapterID\":\"tablo\"}}";
    static const char* launch_msg =
        "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\","
        "\"arguments\":{\"program\":\"tablo_tests/debug_breakpoint_test.tblo\",\"stopOnEntry\":true}}";
    static const char* config_done_msg =
        "{\"seq\":3,\"type\":\"request\",\"command\":\"configurationDone\",\"arguments\":{}}";
    static const char* stack_trace_msg =
        "{\"seq\":4,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}";
    static const char* continue_msg =
        "{\"seq\":5,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}";
    static char post_continue_stack_trace_msg[160];
    static char post_continue_evaluate_msg[200];
    static const char* disconnect_msg =
        "{\"seq\":8,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{}}";

    if (!out_path || out_size == 0) return 0;
    snprintf(out_path, out_size, ".tmp_cli_dap_stoponentry.in");

    FILE* f = fopen(out_path, "wb");
    if (!f) return 0;

    snprintf(post_continue_stack_trace_msg,
             sizeof(post_continue_stack_trace_msg),
             "{\"seq\":6,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    snprintf(post_continue_evaluate_msg,
             sizeof(post_continue_evaluate_msg),
             "{\"seq\":7,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"seed\",\"frameId\":%d,\"context\":\"watch\"}}",
             GEN1_FRAME);

    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(init_msg), init_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(launch_msg), launch_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(config_done_msg), config_done_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(stack_trace_msg), stack_trace_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(continue_msg), continue_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(post_continue_stack_trace_msg), post_continue_stack_trace_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(post_continue_evaluate_msg), post_continue_evaluate_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(disconnect_msg), disconnect_msg);
    fclose(f);
    return 1;
}

static int write_temp_dap_pause_input(char* out_path, size_t out_size) {
    static const char* init_msg =
        "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\","
        "\"arguments\":{\"clientID\":\"tests\",\"adapterID\":\"tablo\"}}";
    static const char* launch_msg =
        "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\","
        "\"arguments\":{\"program\":\"tablo_tests/debug_dap_pause_test.tblo\"}}";
    static const char* config_done_msg =
        "{\"seq\":3,\"type\":\"request\",\"command\":\"configurationDone\",\"arguments\":{}}";
    static const char* pause_msg =
        "{\"seq\":4,\"type\":\"request\",\"command\":\"pause\",\"arguments\":{\"threadId\":1}}";
    static const char* stack_trace_msg =
        "{\"seq\":5,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}";
    static const char* continue_msg =
        "{\"seq\":6,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}";
    static const char* disconnect_msg =
        "{\"seq\":7,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{}}";

    if (!out_path || out_size == 0) return 0;
    snprintf(out_path, out_size, ".tmp_cli_dap_pause.in");

    FILE* f = fopen(out_path, "wb");
    if (!f) return 0;

    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(init_msg), init_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(launch_msg), launch_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(config_done_msg), config_done_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(pause_msg), pause_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(stack_trace_msg), stack_trace_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(continue_msg), continue_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(disconnect_msg), disconnect_msg);
    fclose(f);
    return 1;
}

static int write_temp_dap_setvariable_input(char* out_path, size_t out_size) {
    enum {
        GEN2_FRAME = 20001,
        GEN2_ARGS = 1020001,
        GEN3_FRAME = 30001,
        GEN4_FRAME = 40001,
        GEN4_LOCALS = 2040001,
        GEN4_POINT_CHILD = 4040000,
        GEN6_FRAME = 60001,
        GEN6_LOCALS = 2060001,
        GEN6_GLOBALS = 3060001
    };
    static const char* init_msg =
        "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\","
        "\"arguments\":{\"clientID\":\"tests\",\"adapterID\":\"tablo\"}}";
    static const char* launch_msg =
        "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\","
        "\"arguments\":{\"program\":\"tablo_tests/debug_dap_variables_test.tblo\"}}";
    static const char* set_breakpoints_msg =
        "{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\","
        "\"arguments\":{\"source\":{\"path\":\"tablo_tests/debug_dap_variables_test.tblo\"},"
        "\"breakpoints\":[{\"line\":11}]}}";
    static const char* config_done_msg =
        "{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\",\"arguments\":{}}";
    static const char* step_in_msg =
        "{\"seq\":5,\"type\":\"request\",\"command\":\"stepIn\",\"arguments\":{\"threadId\":1}}";
    char scopes_helper_msg[160];
    char set_argument_msg[200];
    char eval_argument_msg[200];
    static const char* next_helper_point_msg =
        "{\"seq\":9,\"type\":\"request\",\"command\":\"next\",\"arguments\":{\"threadId\":1}}";
    static const char* next_helper_numbers_msg =
        "{\"seq\":10,\"type\":\"request\",\"command\":\"next\",\"arguments\":{\"threadId\":1}}";
    char helper_locals_msg[180];
    char helper_point_children_msg[180];
    char eval_point_msg[200];
    static const char* next_to_main_callsite_msg =
        "{\"seq\":14,\"type\":\"request\",\"command\":\"next\",\"arguments\":{\"threadId\":1}}";
    static const char* next_to_main_total_msg =
        "{\"seq\":15,\"type\":\"request\",\"command\":\"next\",\"arguments\":{\"threadId\":1}}";
    char scopes_main_msg[160];
    char set_local_msg[200];
    char eval_local_msg[200];
    char set_global_msg[200];
    char eval_global_msg[200];
    static const char* continue_msg =
        "{\"seq\":21,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}";
    static const char* disconnect_msg =
        "{\"seq\":22,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{}}";

    if (!out_path || out_size == 0) return 0;
    snprintf(out_path, out_size, ".tmp_cli_dap_setvariable.in");

    FILE* f = fopen(out_path, "wb");
    if (!f) return 0;

    snprintf(scopes_helper_msg,
             sizeof(scopes_helper_msg),
             "{\"seq\":6,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%d}}",
             GEN2_FRAME);
    snprintf(set_argument_msg,
             sizeof(set_argument_msg),
             "{\"seq\":7,\"type\":\"request\",\"command\":\"setVariable\",\"arguments\":{\"variablesReference\":%d,\"name\":\"value\",\"value\":\"9\"}}",
             GEN2_ARGS);
    snprintf(eval_argument_msg,
             sizeof(eval_argument_msg),
             "{\"seq\":8,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"value\",\"frameId\":%d,\"context\":\"watch\"}}",
             GEN2_FRAME);
    snprintf(helper_locals_msg,
             sizeof(helper_locals_msg),
             "{\"seq\":11,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%d}}",
             GEN4_LOCALS);
    snprintf(helper_point_children_msg,
             sizeof(helper_point_children_msg),
             "{\"seq\":12,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%d}}",
             GEN4_POINT_CHILD);
    snprintf(eval_point_msg,
             sizeof(eval_point_msg),
             "{\"seq\":13,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"point.field0\",\"frameId\":%d,\"context\":\"watch\"}}",
             GEN4_FRAME);
    snprintf(scopes_main_msg,
             sizeof(scopes_main_msg),
             "{\"seq\":16,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%d}}",
             GEN6_FRAME);
    snprintf(set_local_msg,
             sizeof(set_local_msg),
             "{\"seq\":17,\"type\":\"request\",\"command\":\"setVariable\",\"arguments\":{\"variablesReference\":%d,\"name\":\"total\",\"value\":\"99\"}}",
             GEN6_LOCALS);
    snprintf(eval_local_msg,
             sizeof(eval_local_msg),
             "{\"seq\":18,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"total\",\"frameId\":%d,\"context\":\"watch\"}}",
             GEN6_FRAME);
    snprintf(set_global_msg,
             sizeof(set_global_msg),
             "{\"seq\":19,\"type\":\"request\",\"command\":\"setVariable\",\"arguments\":{\"variablesReference\":%d,\"name\":\"seed\",\"value\":\"33\"}}",
             GEN6_GLOBALS);
    snprintf(eval_global_msg,
             sizeof(eval_global_msg),
             "{\"seq\":20,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"seed\",\"frameId\":%d,\"context\":\"watch\"}}",
             GEN6_FRAME);

    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(init_msg), init_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(launch_msg), launch_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(set_breakpoints_msg), set_breakpoints_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(config_done_msg), config_done_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(step_in_msg), step_in_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(scopes_helper_msg), scopes_helper_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(set_argument_msg), set_argument_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(eval_argument_msg), eval_argument_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(next_helper_point_msg), next_helper_point_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(next_helper_numbers_msg), next_helper_numbers_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(helper_locals_msg), helper_locals_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(helper_point_children_msg), helper_point_children_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(eval_point_msg), eval_point_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(next_to_main_callsite_msg), next_to_main_callsite_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(next_to_main_total_msg), next_to_main_total_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(scopes_main_msg), scopes_main_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(set_local_msg), set_local_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(eval_local_msg), eval_local_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(set_global_msg), set_global_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(eval_global_msg), eval_global_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(continue_msg), continue_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(disconnect_msg), disconnect_msg);
    fclose(f);
    return 1;
}

static int write_temp_dap_condition_input(char* out_path, size_t out_size) {
    enum {
        GEN1_FRAME = 10001,
        GEN1_LOCALS = 2010001
    };
    static const char* init_msg =
        "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\","
        "\"arguments\":{\"clientID\":\"tests\",\"adapterID\":\"tablo\"}}";
    static const char* launch_msg =
        "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\","
        "\"arguments\":{\"program\":\"tablo_tests/debug_dap_breakpoint_conditions_test.tblo\"}}";
    static const char* set_breakpoints_msg =
        "{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\","
        "\"arguments\":{\"source\":{\"path\":\"tablo_tests/debug_dap_breakpoint_conditions_test.tblo\"},"
        "\"breakpoints\":[{\"line\":6,\"condition\":\"allow\"}]}}";
    static const char* config_done_msg =
        "{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\",\"arguments\":{}}";
    static const char* stack_trace_msg =
        "{\"seq\":5,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}";
    char scopes_msg[160];
    char variables_msg[180];
    char evaluate_msg[200];
    static const char* continue_msg =
        "{\"seq\":9,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}";
    static const char* disconnect_msg =
        "{\"seq\":10,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{}}";

    if (!out_path || out_size == 0) return 0;
    snprintf(out_path, out_size, ".tmp_cli_dap_condition.in");

    FILE* f = fopen(out_path, "wb");
    if (!f) return 0;

    snprintf(scopes_msg,
             sizeof(scopes_msg),
             "{\"seq\":6,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%d}}",
             GEN1_FRAME);
    snprintf(variables_msg,
             sizeof(variables_msg),
             "{\"seq\":7,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%d}}",
             GEN1_LOCALS);
    snprintf(evaluate_msg,
             sizeof(evaluate_msg),
             "{\"seq\":8,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"i\",\"frameId\":%d,\"context\":\"watch\"}}",
             GEN1_FRAME);

    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(init_msg), init_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(launch_msg), launch_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(set_breakpoints_msg), set_breakpoints_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(config_done_msg), config_done_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(stack_trace_msg), stack_trace_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(scopes_msg), scopes_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(variables_msg), variables_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(evaluate_msg), evaluate_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(continue_msg), continue_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(disconnect_msg), disconnect_msg);
    fclose(f);
    return 1;
}

static int write_temp_dap_hitcondition_input(char* out_path, size_t out_size) {
    enum {
        GEN1_FRAME = 10001,
        GEN1_LOCALS = 2010001
    };
    static const char* init_msg =
        "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\","
        "\"arguments\":{\"clientID\":\"tests\",\"adapterID\":\"tablo\"}}";
    static const char* launch_msg =
        "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\","
        "\"arguments\":{\"program\":\"tablo_tests/debug_dap_breakpoint_conditions_test.tblo\"}}";
    static const char* set_breakpoints_msg =
        "{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\","
        "\"arguments\":{\"source\":{\"path\":\"tablo_tests/debug_dap_breakpoint_conditions_test.tblo\"},"
        "\"breakpoints\":[{\"line\":6,\"hitCondition\":\"2\"}]}}";
    static const char* config_done_msg =
        "{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\",\"arguments\":{}}";
    static const char* stack_trace_msg =
        "{\"seq\":5,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}";
    char variables_msg[180];
    char evaluate_msg[200];
    static const char* continue_msg =
        "{\"seq\":8,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}";
    static const char* disconnect_msg =
        "{\"seq\":9,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{}}";

    if (!out_path || out_size == 0) return 0;
    snprintf(out_path, out_size, ".tmp_cli_dap_hitcondition.in");

    FILE* f = fopen(out_path, "wb");
    if (!f) return 0;

    snprintf(variables_msg,
             sizeof(variables_msg),
             "{\"seq\":6,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%d}}",
             GEN1_LOCALS);
    snprintf(evaluate_msg,
             sizeof(evaluate_msg),
             "{\"seq\":7,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"i\",\"frameId\":%d,\"context\":\"watch\"}}",
             GEN1_FRAME);

    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(init_msg), init_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(launch_msg), launch_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(set_breakpoints_msg), set_breakpoints_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(config_done_msg), config_done_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(stack_trace_msg), stack_trace_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(variables_msg), variables_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(evaluate_msg), evaluate_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(continue_msg), continue_msg);
    fprintf(f, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(disconnect_msg), disconnect_msg);
    fclose(f);
    return 1;
}

static void test_cli_test_command(void) {
    printf("Testing CLI 'test' command...\n");

    const char* exe = find_tablo_executable();
    char raw_cmd[4096];
    TEST_ASSERT(exe != NULL, "Locate tablo executable");
    if (!exe) return;

    run_cli_command(exe,
                    "test --fail-fast tablo_tests/testing_assertions_test.tblo",
                    1,
                    "tablo test succeeds for explicit passing file");

    run_cli_command(exe,
                    "test --fail-fast",
                    1,
                    "tablo test default discovery succeeds");

    run_cli_command(exe,
                    "test --list tablo_tests/testing_assertions_test.tblo",
                    1,
                    "tablo test --list succeeds for function-level tests");

    {
        char raw_cmd[2048];
        snprintf(raw_cmd,
                 sizeof(raw_cmd),
                 "\"%s\" test --list tablo_tests/debug_breakpoint_test.tblo",
                 exe);
        run_raw_command_capture_contains(raw_cmd,
                                         ".tmp_cli_test_legacy_main.out",
                                         "(legacy main fallback)",
                                         1,
                                         "tablo test keeps legacy main fallback for explicit file targets");
    }

    run_cli_command(exe,
                    "test --match testAssertEqRecord tablo_tests/testing_assertions_test.tblo",
                    1,
                    "tablo test --match runs selected function-level tests");

    run_cli_command(exe,
                    "test --match definitelyNoSuchTest tablo_tests/testing_assertions_test.tblo",
                    0,
                    "tablo test --match returns non-zero when nothing matches");

    run_cli_command(exe,
                    "test --json --match testAssertTrue tablo_tests/testing_assertions_test.tblo",
                    1,
                    "tablo test --json succeeds for filtered function-level run");

    run_cli_command(exe,
                    "test --list --json --match testAssertNil tablo_tests/testing_errorcode_test.tblo",
                    1,
                    "tablo test --list --json succeeds");

    run_cli_command(exe,
                    "test --jobs 2 --fail-fast",
                    1,
                    "tablo test --jobs runs tests in parallel");

    run_cli_command(exe,
                    "test --shard 1/1 --jobs 2 --rerun-failed 1 --fail-fast",
                    1,
                    "tablo test supports shard/jobs/rerun options together");

    run_cli_command(exe,
                    "test --shard 2/1",
                    0,
                    "tablo test rejects invalid shard index");

    run_cli_command(exe,
                    "test --fail-fast --max-open-files 2 tablo_tests/testing_assertions_test.tblo",
                    1,
                    "tablo test accepts --max-open-files");

    run_cli_command(exe,
                    "test --fail-fast --max-open-sockets 2 tablo_tests/testing_assertions_test.tblo",
                    1,
                    "tablo test accepts --max-open-sockets");

    run_cli_command(exe,
                    "test --max-open-files nope",
                    0,
                    "tablo test rejects invalid --max-open-files value");

    run_cli_command(exe,
                    "test --max-open-sockets -1",
                    0,
                    "tablo test rejects invalid --max-open-sockets value");

    run_cli_command(exe,
                    "test --ctest --max-open-files 1",
                    0,
                    "tablo test --ctest rejects runtime file-limit options");

    run_cli_command(exe,
                    "test --junit .tmp_cli_junit.xml tablo_tests/testing_assertions_test.tblo",
                    1,
                    "tablo test writes JUnit report");
    TEST_ASSERT(file_exists(".tmp_cli_junit.xml"), "JUnit report file exists");
    TEST_ASSERT(file_contains_text(".tmp_cli_junit.xml", "<testsuite"), "JUnit report contains testsuite");
    remove(".tmp_cli_junit.xml");

    {
        const char* ext_lib = find_tablo_test_extension_library();
        TEST_ASSERT(ext_lib != NULL, "Locate native extension test library for test command");
        if (ext_lib) {
            snprintf(raw_cmd,
                     sizeof(raw_cmd),
                     "\"%s\" test --ext \"%s\" --list tablo_tests/native_extension_suite.tblo",
                     exe,
                     ext_lib);
            if (run_raw_command_capture(raw_cmd,
                                        ".tmp_cli_test_ext_list.out",
                                        1,
                                        "tablo test --ext lists extension-backed tests")) {
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_list.out", "testExtensionArithmetic"),
                            "tablo test --ext discovers extension arithmetic tests");
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_list.out", "testExtensionCounter"),
                            "tablo test --ext discovers extension handle tests");
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_list.out", "testExtensionArrays"),
                            "tablo test --ext discovers extension array tests");
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_list.out", "testExtensionTuples"),
                            "tablo test --ext discovers extension tuple tests");
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_list.out", "testExtensionMaps"),
                            "tablo test --ext discovers extension map tests");
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_list.out", "testExtensionEventBatches"),
                            "tablo test --ext discovers extension event batch tests");
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_list.out", "testExtensionCallbacks"),
                            "tablo test --ext discovers extension callback tests");
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_list.out", "testExtensionCallbackResults"),
                            "tablo test --ext discovers structured callback result tests");
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_list.out", "testExtensionManualPostedCallbackPump"),
                            "tablo test --ext discovers extension manual callback pump tests");
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_list.out", "testExtensionQueuedTupleCallbacks"),
                            "tablo test --ext discovers extension queued tuple callback tests");
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_list.out", "testExtensionQueuedArrayCallbacks"),
                            "tablo test --ext discovers extension queued array callback tests");
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_list.out", "testExtensionQueuedMapCallbacks"),
                            "tablo test --ext discovers extension queued map callback tests");
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_list.out", "testExtensionQueuedEventBatchCallbacks"),
                            "tablo test --ext discovers extension queued event batch callback tests");
            }
            remove(".tmp_cli_test_ext_list.out");

            snprintf(raw_cmd,
                     sizeof(raw_cmd),
                     "\"%s\" test --ext \"%s\" --json --jobs 2 --fail-fast tablo_tests/native_extension_suite.tblo",
                     exe,
                     ext_lib);
            if (run_raw_command_capture(raw_cmd,
                                        ".tmp_cli_test_ext_run.json",
                                        1,
                                        "tablo test --ext runs extension-backed tests in child workers")) {
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_run.json", "\"testsExecuted\":13"),
                            "tablo test --ext executes all extension-backed tests");
                TEST_ASSERT(file_contains_text(".tmp_cli_test_ext_run.json", "\"passed\":13"),
                            "tablo test --ext passes extension-backed tests");
            }
            remove(".tmp_cli_test_ext_run.json");

            run_cli_command(exe,
                            "test --fail-fast tablo_tests/native_extension_suite.tblo",
                            0,
                            "tablo test without --ext rejects extension-backed tests");

            snprintf(raw_cmd,
                     sizeof(raw_cmd),
                     "\"%s\" test --ctest --ext \"%s\"",
                     exe,
                     ext_lib);
            run_raw_command_capture_contains(raw_cmd,
                                             ".tmp_cli_test_ext_ctest.out",
                                             "cannot be combined with test-runner specific options",
                                             0,
                                             "tablo test --ctest rejects --ext");
        }
    }

#ifdef _WIN32
    {
        char raw_cmd[4096];
        snprintf(raw_cmd,
                 sizeof(raw_cmd),
                 "set TABLO_TEST_FORCE_CHILD_CRASH=1&& %s test --json --fail-fast tablo_tests/testing_assertions_test.tblo",
                 exe);
        run_raw_command_capture_contains(raw_cmd,
                                         ".tmp_cli_crash_output.json",
                                         "\"status\":\"crash\"",
                                         0,
                                         "tablo test reports crash status when child aborts");
    }
#else
    {
        char raw_cmd[4096];
        snprintf(raw_cmd,
                 sizeof(raw_cmd),
                 "TABLO_TEST_FORCE_CHILD_CRASH=1 \"%s\" test --json --fail-fast tablo_tests/testing_assertions_test.tblo",
                 exe);
        run_raw_command_capture_contains(raw_cmd,
                                         ".tmp_cli_crash_output.json",
                                         "\"status\":\"crash\"",
                                         0,
                                         "tablo test reports crash status when child aborts");
    }
#endif

    char temp_fail_path[512];
    temp_fail_path[0] = '\0';
    int wrote_fail = write_temp_test_file(temp_fail_path,
                                          sizeof(temp_fail_path),
                                          "func testBoom(): void { panic(\"boom\"); }\n");
    TEST_ASSERT(wrote_fail, "Create temporary failing *_test.tblo");

    if (wrote_fail) {
        char args[1024];
        snprintf(args, sizeof(args), "test --fail-fast \"%s\"", temp_fail_path);
        run_cli_command(exe, args, 0, "tablo test returns non-zero for failing file");

#ifdef _WIN32
        {
            char raw_cmd[4096];
            snprintf(raw_cmd,
                     sizeof(raw_cmd),
                     "\"%s\" test --json --rerun-failed 1 \"%s\"",
                     exe,
                     temp_fail_path);
            run_raw_command_capture_contains(raw_cmd,
                                             ".tmp_cli_rerun_output.json",
                                             "\"attempts\":2",
                                             0,
                                             "tablo test reruns failing tests and reports attempts");
        }
#else
        {
            char raw_cmd[4096];
            snprintf(raw_cmd,
                     sizeof(raw_cmd),
                     "\"%s\" test --json --rerun-failed 1 \"%s\"",
                     exe,
                     temp_fail_path);
            run_raw_command_capture_contains(raw_cmd,
                                             ".tmp_cli_rerun_output.json",
                                             "\"attempts\":2",
                                             0,
                                             "tablo test reruns failing tests and reports attempts");
        }
#endif

        remove(temp_fail_path);
    }

    char temp_timeout_path[512];
    temp_timeout_path[0] = '\0';
    int wrote_timeout = write_temp_timeout_test_file(temp_timeout_path, sizeof(temp_timeout_path));
    TEST_ASSERT(wrote_timeout, "Create temporary timeout *_test.tblo");
    if (wrote_timeout) {
        char args[1024];
        snprintf(args, sizeof(args), "test --fail-fast --timeout-ms 50 \"%s\"", temp_timeout_path);
        run_cli_command(exe, args, 0, "tablo test returns non-zero for timed out test");
        remove(temp_timeout_path);
    }

}

static void test_cli_run_command(void) {
    char raw_cmd[4096];

    printf("Testing CLI 'run' command...\n");

    const char* exe = find_tablo_executable();
    TEST_ASSERT(exe != NULL, "Locate tablo executable for run");
    if (!exe) return;

    {
        const char* ext_lib = find_tablo_test_extension_library();
        TEST_ASSERT(ext_lib != NULL, "Locate native extension test library");
        if (ext_lib) {
            snprintf(raw_cmd,
                     sizeof(raw_cmd),
                     "\"%s\" run --ext \"%s\" tablo_tests/native_extension_smoke.tblo",
                     exe,
                     ext_lib);
            if (run_raw_command_capture(raw_cmd,
                                        ".tmp_cli_native_extension.out",
                                        1,
                                        "tablo run --ext executes native extension programs")) {
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "hello, tablo"),
                            "tablo run --ext returns extension string results");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "3.75"),
                            "tablo run --ext returns extension double results");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "bytes=abc"),
                            "tablo run --ext returns extension bytes results");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "count=50"),
                            "tablo run --ext mutates opaque handles");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "<Counter>"),
                            "tablo run --ext prints opaque handles by type name");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "units"),
                            "tablo run --ext accepts tuple arguments and returns tuple strings");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "9"),
                            "tablo run --ext returns tuple ints");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "snapshot=50"),
                            "tablo run --ext returns tuple handle snapshots");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "7"),
                            "tablo run --ext returns tuple values nested inside map<string, any> payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "native"),
                            "tablo run --ext returns array values nested inside map<string, any> payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "native-ext"),
                            "tablo run --ext returns nested map string values inside map<string, any> payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "dispatch"),
                            "tablo run --ext returns nested map array values inside map<string, any> payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "spawn:57:7:2:true:native-ext:3:2"),
                            "tablo run --ext returns and consumes nested map<string, any> payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "input:keyboard:32:true:input:1:2"),
                            "tablo run --ext exposes input event preset helpers");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "window:resize:1280:720:true:window:4:2"),
                            "tablo run --ext exposes window event preset helpers");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "frame:update:60:0.250:frame:5:2"),
                            "tablo run --ext exposes frame event preset helpers");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "2:17:down:batch:2"),
                            "tablo run --ext returns array<map<string, any>> event batch payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "event=50"),
                            "tablo run --ext returns map handle fields as fresh opaque handles");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "15"),
                            "tablo run --ext invokes direct native callbacks");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "18"),
                            "tablo run --ext stores callbacks in opaque handles and invokes them later");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "cb=50"),
                            "tablo run --ext passes opaque handles through callback invocations");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "array_arg=21"),
                            "tablo run --ext accepts top-level array callback parameters");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "array_result=21"),
                            "tablo run --ext accepts top-level array callback results");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "12:units"),
                            "tablo run --ext accepts flat tuple callback results");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "tick:12:7:2:callback:9:2"),
                            "tablo run --ext accepts nested map callback results");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "10"),
                            "tablo run --ext returns extension int array summaries");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "24"),
                            "tablo run --ext observes opaque handle arrays");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "vm-lang-ext"),
                            "tablo run --ext accepts string arrays");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "123"),
                            "tablo run --ext drains queued foreign-thread callbacks on the VM thread");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "manual_before=200"),
                            "tablo run --ext can defer queued callbacks until explicit pumping");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "manual_pending=1"),
                            "tablo run --ext reports pending queued callbacks in manual pump mode");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "manual_drained=1"),
                            "tablo run --ext drains queued callbacks explicitly");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "manual_after=217"),
                            "tablo run --ext delivers queued callbacks after manual pumping");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "manual_pending=0"),
                            "tablo run --ext clears queued callbacks after manual pumping");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "tuple_pending=1"),
                            "tablo run --ext can queue tuple callback payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "tuple_drained=1"),
                            "tablo run --ext can drain queued tuple callback payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "tuple_after=319"),
                            "tablo run --ext preserves tuple integer payloads through queued callbacks");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "tuple_hits=1"),
                            "tablo run --ext preserves tuple string payloads through queued callbacks");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "array_post_pending=1"),
                            "tablo run --ext can queue top-level array callback payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "array_post_drained=1"),
                            "tablo run --ext can drain queued top-level array callback payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "array_post_after=596"),
                            "tablo run --ext preserves array integer payloads through queued callbacks");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "array_post_hits=1"),
                            "tablo run --ext preserves array arity and executes queued array callbacks once");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "map_pending=1"),
                            "tablo run --ext can queue map callback payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "map_drained=1"),
                            "tablo run --ext can drain queued map callback payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "map_after=429"),
                            "tablo run --ext preserves map integer payloads through queued callbacks");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "map_hits=1"),
                            "tablo run --ext preserves nested map payloads through queued callbacks");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "event_batch_pending=1"),
                            "tablo run --ext can queue batched event callback payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "event_batch_drained=1"),
                            "tablo run --ext can drain batched event callback payloads");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "event_batch_after=683"),
                            "tablo run --ext preserves queued array<map<string, any>> batch totals");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension.out", "event_batch_hits=1"),
                            "tablo run --ext executes queued array<map<string, any>> callbacks once");
            }
            remove(".tmp_cli_native_extension.out");

            run_cli_command(exe,
                            "run tablo_tests/native_extension_smoke.tblo",
                            0,
                            "tablo run without --ext rejects extension-dependent source");

            snprintf(raw_cmd,
                     sizeof(raw_cmd),
                     "\"%s\" compile --ext \"%s\" tablo_tests/native_extension_smoke.tblo -o .tmp_cli_native_extension.tbcc",
                     exe,
                     ext_lib);
            run_raw_command_capture(raw_cmd,
                                    ".tmp_cli_native_extension_compile.out",
                                    1,
                                    "tablo compile --ext writes extension-dependent artifacts");
            TEST_ASSERT(file_exists(".tmp_cli_native_extension.tbcc"),
                        "tablo compile --ext creates the requested artifact");
            remove(".tmp_cli_native_extension_compile.out");

            snprintf(raw_cmd,
                     sizeof(raw_cmd),
                     "\"%s\" run --ext \"%s\" .tmp_cli_native_extension.tbcc",
                     exe,
                     ext_lib);
            if (run_raw_command_capture(raw_cmd,
                                        ".tmp_cli_native_extension_artifact.out",
                                        1,
                                        "tablo run --ext executes extension-dependent artifacts")) {
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "count=50"),
                            "tablo run --ext preserves opaque handle behavior from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "<Counter>"),
                            "tablo run --ext preserves opaque handle printing from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "units"),
                            "tablo run --ext preserves tuple-backed extension behavior from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "snapshot=50"),
                            "tablo run --ext preserves tuple-backed handle results from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "7"),
                            "tablo run --ext preserves tuple values nested inside map<string, any> payloads from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "native"),
                            "tablo run --ext preserves array values nested inside map<string, any> payloads from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "native-ext"),
                            "tablo run --ext preserves nested map string values from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "spawn:57:7:2:true:native-ext:3:2"),
                            "tablo run --ext preserves nested map-backed extension behavior from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "input:keyboard:32:true:input:1:2"),
                            "tablo run --ext preserves input event preset behavior from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "window:resize:1280:720:true:window:4:2"),
                            "tablo run --ext preserves window event preset behavior from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "frame:update:60:0.250:frame:5:2"),
                            "tablo run --ext preserves frame event preset behavior from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "2:17:down:batch:2"),
                            "tablo run --ext preserves array<map<string, any>> event batch behavior from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "cb=50"),
                            "tablo run --ext preserves callback-backed extension behavior from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "array_arg=21"),
                            "tablo run --ext preserves array callback parameters from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "array_result=21"),
                            "tablo run --ext preserves array callback results from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "12:units"),
                            "tablo run --ext preserves tuple callback results from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "tick:12:7:2:callback:9:2"),
                            "tablo run --ext preserves nested map callback results from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "vm-lang-ext"),
                            "tablo run --ext preserves array-backed extension behavior from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "123"),
                            "tablo run --ext preserves queued callback delivery from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "manual_after=217"),
                            "tablo run --ext preserves manual queued callback pumping from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "tuple_after=319"),
                            "tablo run --ext preserves queued tuple callback pumping from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "array_post_after=596"),
                            "tablo run --ext preserves queued top-level array callback pumping from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "map_after=429"),
                            "tablo run --ext preserves queued map callback pumping from artifacts");
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_artifact.out", "event_batch_after=683"),
                            "tablo run --ext preserves queued event batch pumping from artifacts");
            }
            remove(".tmp_cli_native_extension_artifact.out");

            run_cli_command(exe,
                            "run .tmp_cli_native_extension.tbcc",
                            0,
                            "tablo run without --ext rejects extension-dependent artifacts");
            remove(".tmp_cli_native_extension.tbcc");

            snprintf(raw_cmd,
                     sizeof(raw_cmd),
                     "\"%s\" compile --ext \"%s\" tablo_tests/native_extension_bad_construct.tblo",
                     exe,
                     ext_lib);
            if (run_raw_command_capture(raw_cmd,
                                        ".tmp_cli_native_extension_bad.out",
                                        0,
                                        "tablo compile --ext rejects direct opaque handle pattern matching")) {
                TEST_ASSERT(file_contains_text(".tmp_cli_native_extension_bad.out",
                                               "Opaque native handle types cannot be pattern-matched directly"),
                            "tablo compile --ext reports opaque handle pattern-match errors");
            }
            remove(".tmp_cli_native_extension_bad.out");
        }
    }

    snprintf(raw_cmd,
             sizeof(raw_cmd),
             "\"%s\" run --profile-jit --jit-hot-threshold 2 tablo_tests/jit_profile_test.tblo",
             exe);
    if (run_raw_command_capture(raw_cmd,
                                ".tmp_cli_jit_profile.out",
                                1,
                                "tablo run --profile-jit dumps function hotness")) {
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "JIT hotness profile (threshold=2):"),
                    "tablo run --profile-jit prints the profile header");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotLeaf"),
                    "tablo run --profile-jit reports hotLeaf");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "entries=3"),
                    "tablo run --profile-jit reports hotLeaf entry count");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotFallback"),
                    "tablo run --profile-jit reports hotFallback");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotStub"),
                    "tablo run --profile-jit reports hotStub");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotSub"),
                    "tablo run --profile-jit reports hotSub");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotDiv"),
                    "tablo run --profile-jit reports hotDiv");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotMod"),
                    "tablo run --profile-jit reports hotMod");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotAnd"),
                    "tablo run --profile-jit reports hotAnd");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotOr"),
                    "tablo run --profile-jit reports hotOr");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotXor"),
                    "tablo run --profile-jit reports hotXor");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotDynamic"),
                    "tablo run --profile-jit reports dynamically-invoked hot functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "invokeDynamic"),
                    "tablo run --profile-jit reports higher-order call wrappers");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotAdd2"),
                    "tablo run --profile-jit reports two-arg native add functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotSub2"),
                    "tablo run --profile-jit reports two-arg native subtract functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotMul2"),
                    "tablo run --profile-jit reports two-arg native multiply functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotAnd2"),
                    "tablo run --profile-jit reports two-arg native bitand functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotOr2"),
                    "tablo run --profile-jit reports two-arg native bitor functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotXor2"),
                    "tablo run --profile-jit reports two-arg native bitxor functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotDynamic2"),
                    "tablo run --profile-jit reports dynamically-invoked two-arg native functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "invokeDynamic2"),
                    "tablo run --profile-jit reports two-arg higher-order wrappers");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotLt2"),
                    "tablo run --profile-jit reports two-arg native less-than functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotLe2"),
                    "tablo run --profile-jit reports two-arg native less-equal functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotEq2"),
                    "tablo run --profile-jit reports two-arg native equality functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotNe2"),
                    "tablo run --profile-jit reports two-arg native inequality functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotGt2"),
                    "tablo run --profile-jit reports two-arg native greater-than functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotGe2"),
                    "tablo run --profile-jit reports two-arg native greater-equal functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotLtConst"),
                    "tablo run --profile-jit reports local-const less-than bool functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotEqConst"),
                    "tablo run --profile-jit reports local-const equality bool functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotGeConst"),
                    "tablo run --profile-jit reports local-const greater-equal bool functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotMinConst"),
                    "tablo run --profile-jit reports local-const min selector functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotMaxConst"),
                    "tablo run --profile-jit reports local-const max selector functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedMinConst"),
                    "tablo run --profile-jit reports guarded local-const min selector functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedMaxConst"),
                    "tablo run --profile-jit reports guarded local-const max selector functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "invokeDynamicBool1"),
                    "tablo run --profile-jit reports one-arg boolean higher-order wrappers");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedLtBool2"),
                    "tablo run --profile-jit reports guarded two-arg less-than bool functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedLeBool2"),
                    "tablo run --profile-jit reports guarded two-arg less-equal bool functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedEqBool2"),
                    "tablo run --profile-jit reports guarded two-arg equality bool functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedNeBool2"),
                    "tablo run --profile-jit reports guarded two-arg inequality bool functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedGtBool2"),
                    "tablo run --profile-jit reports guarded two-arg greater-than bool functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedGeBool2"),
                    "tablo run --profile-jit reports guarded two-arg greater-equal bool functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "invokeDynamicBool2"),
                    "tablo run --profile-jit reports boolean higher-order wrappers");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotMinLt2"),
                    "tablo run --profile-jit reports two-arg native min selector functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotMaxLt2"),
                    "tablo run --profile-jit reports two-arg native max selector functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotMinLe2"),
                    "tablo run --profile-jit reports two-arg native less-equal selectors");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotMaxGt2"),
                    "tablo run --profile-jit reports two-arg native greater-than selectors");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hotMaxGe2"),
                    "tablo run --profile-jit reports two-arg native greater-equal selectors");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedMinLt2"),
                    "tablo run --profile-jit reports guarded min selector functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedMaxLt2"),
                    "tablo run --profile-jit reports guarded max selector functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedMinLe2"),
                    "tablo run --profile-jit reports guarded less-equal selectors");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedMaxGt2"),
                    "tablo run --profile-jit reports guarded greater-than selectors");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedMaxGe2"),
                    "tablo run --profile-jit reports guarded greater-equal selectors");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedAdd2"),
                    "tablo run --profile-jit reports guarded two-arg native add functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedSub2"),
                    "tablo run --profile-jit reports guarded two-arg native subtract functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedMul2"),
                    "tablo run --profile-jit reports guarded two-arg native multiply functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedAnd2"),
                    "tablo run --profile-jit reports guarded two-arg native bitand functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedOr2"),
                    "tablo run --profile-jit reports guarded two-arg native bitor functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "guardedXor2"),
                    "tablo run --profile-jit reports guarded two-arg native bitxor functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "hot=yes"),
                    "tablo run --profile-jit marks hot functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "asyncLeaf"),
                    "tablo run --profile-jit reports async functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "async=yes"),
                    "tablo run --profile-jit marks async functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "state=queued"),
                    "tablo run --profile-jit reports queued hot functions");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "reason=queued-hot"),
                    "tablo run --profile-jit reports queued hot-function reason");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "support=stub+native-summary"),
                    "tablo run --profile-jit reports native-summary backend support");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "support=stub"),
                    "tablo run --profile-jit reports stub-only backend support");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "support=none"),
                    "tablo run --profile-jit reports unsupported backend support");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "family=arithmetic"),
                    "tablo run --profile-jit reports arithmetic native families");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "family=compare"),
                    "tablo run --profile-jit reports compare native families");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "family=selector"),
                    "tablo run --profile-jit reports selector native families");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "family=none"),
                    "tablo run --profile-jit reports missing native families");
        TEST_ASSERT(file_contains_text(".tmp_cli_jit_profile.out", "149\r\n4\r\n7\r\n7\r\n") ||
                        file_contains_text(".tmp_cli_jit_profile.out", "149\n4\n7\n7\n"),
                    "tablo run --profile-jit preserves program stdout");
      }
      remove(".tmp_cli_jit_profile.out");

      snprintf(raw_cmd,
               sizeof(raw_cmd),
               "\"%s\" run --dump-jit-queue --jit-hot-threshold 2 tablo_tests/jit_profile_test.tblo",
               exe);
      if (run_raw_command_capture(raw_cmd,
                                  ".tmp_cli_jit_queue.out",
                                  1,
                                  "tablo run --dump-jit-queue dumps queued hot functions")) {
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "JIT work queue (threshold=2):"),
                      "tablo run --dump-jit-queue prints the queue header");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotLeaf"),
                      "tablo run --dump-jit-queue includes queued sync functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotFallback"),
                      "tablo run --dump-jit-queue includes queued sync native-mul functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotStub"),
                      "tablo run --dump-jit-queue includes queued sync stub-fallback functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotSub"),
                      "tablo run --dump-jit-queue includes queued sync native-sub functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotDiv"),
                      "tablo run --dump-jit-queue includes queued sync native-div functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotMod"),
                      "tablo run --dump-jit-queue includes queued sync native-mod functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotAnd"),
                      "tablo run --dump-jit-queue includes queued sync native-bitand functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotOr"),
                      "tablo run --dump-jit-queue includes queued sync native-bitor functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotXor"),
                      "tablo run --dump-jit-queue includes queued sync native-bitxor functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotDynamic"),
                      "tablo run --dump-jit-queue includes dynamically-invoked native candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "invokeDynamic"),
                      "tablo run --dump-jit-queue includes higher-order wrapper candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotAdd2"),
                      "tablo run --dump-jit-queue includes two-arg native add candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotSub2"),
                      "tablo run --dump-jit-queue includes two-arg native subtract candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotMul2"),
                      "tablo run --dump-jit-queue includes two-arg native multiply candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotAnd2"),
                      "tablo run --dump-jit-queue includes two-arg native bitand candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotOr2"),
                      "tablo run --dump-jit-queue includes two-arg native bitor candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotXor2"),
                      "tablo run --dump-jit-queue includes two-arg native bitxor candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotDynamic2"),
                      "tablo run --dump-jit-queue includes dynamically-invoked two-arg native candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "invokeDynamic2"),
                      "tablo run --dump-jit-queue includes two-arg higher-order wrapper candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotLt2"),
                      "tablo run --dump-jit-queue includes two-arg native less-than candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotLe2"),
                      "tablo run --dump-jit-queue includes two-arg native less-equal candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotEq2"),
                      "tablo run --dump-jit-queue includes two-arg native equality candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotNe2"),
                      "tablo run --dump-jit-queue includes two-arg native inequality candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotGt2"),
                      "tablo run --dump-jit-queue includes two-arg native greater-than candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotGe2"),
                      "tablo run --dump-jit-queue includes two-arg native greater-equal candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotLtConst"),
                      "tablo run --dump-jit-queue includes local-const less-than bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotEqConst"),
                      "tablo run --dump-jit-queue includes local-const equality bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotGeConst"),
                      "tablo run --dump-jit-queue includes local-const greater-equal bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotMinConst"),
                      "tablo run --dump-jit-queue includes local-const min selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotMaxConst"),
                      "tablo run --dump-jit-queue includes local-const max selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedMinConst"),
                      "tablo run --dump-jit-queue includes guarded local-const min selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedMaxConst"),
                      "tablo run --dump-jit-queue includes guarded local-const max selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "invokeDynamicBool1"),
                      "tablo run --dump-jit-queue includes one-arg boolean higher-order wrapper candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedLtBool2"),
                      "tablo run --dump-jit-queue includes guarded two-arg less-than bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedLeBool2"),
                      "tablo run --dump-jit-queue includes guarded two-arg less-equal bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedEqBool2"),
                      "tablo run --dump-jit-queue includes guarded two-arg equality bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedNeBool2"),
                      "tablo run --dump-jit-queue includes guarded two-arg inequality bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedGtBool2"),
                      "tablo run --dump-jit-queue includes guarded two-arg greater-than bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedGeBool2"),
                      "tablo run --dump-jit-queue includes guarded two-arg greater-equal bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "invokeDynamicBool2"),
                      "tablo run --dump-jit-queue includes boolean higher-order wrapper candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotMinLt2"),
                      "tablo run --dump-jit-queue includes two-arg native min selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotMaxLt2"),
                      "tablo run --dump-jit-queue includes two-arg native max selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotMinLe2"),
                      "tablo run --dump-jit-queue includes two-arg native less-equal selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotMaxGt2"),
                      "tablo run --dump-jit-queue includes two-arg native greater-than selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "hotMaxGe2"),
                      "tablo run --dump-jit-queue includes two-arg native greater-equal selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedMinLt2"),
                      "tablo run --dump-jit-queue includes guarded min selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedMaxLt2"),
                      "tablo run --dump-jit-queue includes guarded max selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedMinLe2"),
                      "tablo run --dump-jit-queue includes guarded less-equal selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedMaxGt2"),
                      "tablo run --dump-jit-queue includes guarded greater-than selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedMaxGe2"),
                      "tablo run --dump-jit-queue includes guarded greater-equal selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedAdd2"),
                      "tablo run --dump-jit-queue includes guarded two-arg native add candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedSub2"),
                      "tablo run --dump-jit-queue includes guarded two-arg native subtract candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedMul2"),
                      "tablo run --dump-jit-queue includes guarded two-arg native multiply candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedAnd2"),
                      "tablo run --dump-jit-queue includes guarded two-arg native bitand candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedOr2"),
                      "tablo run --dump-jit-queue includes guarded two-arg native bitor candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "guardedXor2"),
                      "tablo run --dump-jit-queue includes guarded two-arg native bitxor candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "asyncLeaf"),
                      "tablo run --dump-jit-queue includes queued async functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "entries=3"),
                      "tablo run --dump-jit-queue reports sync hot entry counts");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "state=queued"),
                      "tablo run --dump-jit-queue reports queued state");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "reason=queued-hot"),
                      "tablo run --dump-jit-queue reports queued hot-function reason");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "support=stub+native-summary"),
                      "tablo run --dump-jit-queue reports native-summary backend support");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "support=stub"),
                      "tablo run --dump-jit-queue reports stub-only backend support");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "support=none"),
                      "tablo run --dump-jit-queue reports unsupported backend support");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "family=arithmetic"),
                      "tablo run --dump-jit-queue reports arithmetic native families");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "family=compare"),
                      "tablo run --dump-jit-queue reports compare native families");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "family=selector"),
                      "tablo run --dump-jit-queue reports selector native families");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "family=none"),
                      "tablo run --dump-jit-queue reports missing native families");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_queue.out", "entries=2"),
                      "tablo run --dump-jit-queue reports async hot entry counts");
          TEST_ASSERT(file_lacks_text(".tmp_cli_jit_queue.out", "warmCaller"),
                      "tablo run --dump-jit-queue excludes cold functions");
      }
      remove(".tmp_cli_jit_queue.out");

      snprintf(raw_cmd,
               sizeof(raw_cmd),
               "\"%s\" run --profile-jit --dump-jit-queue --drain-jit-queue --jit-hot-threshold 2 tablo_tests/jit_profile_test.tblo",
               exe);
      if (run_raw_command_capture(raw_cmd,
                                  ".tmp_cli_jit_drain.out",
                                  1,
                                  "tablo run --drain-jit-queue drains queued hot functions")) {
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "JIT drain report: processed=60"),
                      "tablo run --drain-jit-queue reports the processed count");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "state=compiled-native"),
                      "tablo run --drain-jit-queue promotes supported native shapes to compiled-native");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "reason=native-hint"),
                      "tablo run --drain-jit-queue reports compiler-hinted native backend decisions");
          TEST_ASSERT(file_lacks_text(".tmp_cli_jit_drain.out", "reason=native-exact"),
                      "tablo run --drain-jit-queue no longer needs exact bytecode rematching for the covered leaf families");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotFallback"),
                      "tablo run --drain-jit-queue reports the native multiply fallback candidate");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotStub"),
                      "tablo run --drain-jit-queue reports the stub fallback candidate");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotSub"),
                      "tablo run --drain-jit-queue reports the native subtract candidate");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotDiv"),
                      "tablo run --drain-jit-queue reports the native divide candidate");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotMod"),
                      "tablo run --drain-jit-queue reports the native modulo candidate");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotAnd"),
                      "tablo run --drain-jit-queue reports the native bitand candidate");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotOr"),
                      "tablo run --drain-jit-queue reports the native bitor candidate");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotXor"),
                      "tablo run --drain-jit-queue reports the native bitxor candidate");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotDynamic"),
                      "tablo run --drain-jit-queue reports dynamically-invoked native candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "invokeDynamic"),
                      "tablo run --drain-jit-queue reports higher-order wrapper candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotAdd2"),
                      "tablo run --drain-jit-queue reports two-arg native add candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotSub2"),
                      "tablo run --drain-jit-queue reports two-arg native subtract candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotMul2"),
                      "tablo run --drain-jit-queue reports two-arg native multiply candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotAnd2"),
                      "tablo run --drain-jit-queue reports two-arg native bitand candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotOr2"),
                      "tablo run --drain-jit-queue reports two-arg native bitor candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotXor2"),
                      "tablo run --drain-jit-queue reports two-arg native bitxor candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotDynamic2"),
                      "tablo run --drain-jit-queue reports dynamically-invoked two-arg native candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "invokeDynamic2"),
                      "tablo run --drain-jit-queue reports two-arg higher-order wrapper candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotLt2"),
                      "tablo run --drain-jit-queue reports two-arg native less-than candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotLe2"),
                      "tablo run --drain-jit-queue reports two-arg native less-equal candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotEq2"),
                      "tablo run --drain-jit-queue reports two-arg native equality candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotNe2"),
                      "tablo run --drain-jit-queue reports two-arg native inequality candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotGt2"),
                      "tablo run --drain-jit-queue reports two-arg native greater-than candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotGe2"),
                      "tablo run --drain-jit-queue reports two-arg native greater-equal candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotLtConst"),
                      "tablo run --drain-jit-queue reports local-const less-than bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotEqConst"),
                      "tablo run --drain-jit-queue reports local-const equality bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotGeConst"),
                      "tablo run --drain-jit-queue reports local-const greater-equal bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotMinConst"),
                      "tablo run --drain-jit-queue reports local-const min selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotMaxConst"),
                      "tablo run --drain-jit-queue reports local-const max selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedMinConst"),
                      "tablo run --drain-jit-queue reports guarded local-const min selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedMaxConst"),
                      "tablo run --drain-jit-queue reports guarded local-const max selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "invokeDynamicBool1"),
                      "tablo run --drain-jit-queue reports one-arg boolean higher-order wrapper candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedLtBool2"),
                      "tablo run --drain-jit-queue reports guarded two-arg less-than bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedLeBool2"),
                      "tablo run --drain-jit-queue reports guarded two-arg less-equal bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedEqBool2"),
                      "tablo run --drain-jit-queue reports guarded two-arg equality bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedNeBool2"),
                      "tablo run --drain-jit-queue reports guarded two-arg inequality bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedGtBool2"),
                      "tablo run --drain-jit-queue reports guarded two-arg greater-than bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedGeBool2"),
                      "tablo run --drain-jit-queue reports guarded two-arg greater-equal bool candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "invokeDynamicBool2"),
                      "tablo run --drain-jit-queue reports boolean higher-order wrapper candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotMinLt2"),
                      "tablo run --drain-jit-queue reports two-arg native min selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotMaxLt2"),
                      "tablo run --drain-jit-queue reports two-arg native max selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotMinLe2"),
                      "tablo run --drain-jit-queue reports two-arg native less-equal selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotMaxGt2"),
                      "tablo run --drain-jit-queue reports two-arg native greater-than selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "hotMaxGe2"),
                      "tablo run --drain-jit-queue reports two-arg native greater-equal selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedMinLt2"),
                      "tablo run --drain-jit-queue reports guarded min selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedMaxLt2"),
                      "tablo run --drain-jit-queue reports guarded max selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedMinLe2"),
                      "tablo run --drain-jit-queue reports guarded less-equal selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedMaxGt2"),
                      "tablo run --drain-jit-queue reports guarded greater-than selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedMaxGe2"),
                      "tablo run --drain-jit-queue reports guarded greater-equal selector candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedAdd2"),
                      "tablo run --drain-jit-queue reports guarded two-arg native add candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedSub2"),
                      "tablo run --drain-jit-queue reports guarded two-arg native subtract candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedMul2"),
                      "tablo run --drain-jit-queue reports guarded two-arg native multiply candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedAnd2"),
                      "tablo run --drain-jit-queue reports guarded two-arg native bitand candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedOr2"),
                      "tablo run --drain-jit-queue reports guarded two-arg native bitor candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "guardedXor2"),
                      "tablo run --drain-jit-queue reports guarded two-arg native bitxor candidates");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "state=compiled-stub"),
                      "tablo run --drain-jit-queue keeps stub fallback for unsupported sync functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "reason=stub-fallback"),
                      "tablo run --drain-jit-queue reports the stub fallback reason");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "state=failed"),
                      "tablo run --drain-jit-queue reports unsupported queued functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "reason=unsupported-async"),
                      "tablo run --drain-jit-queue reports the async unsupported reason");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "support=stub+native-summary"),
                      "tablo run --drain-jit-queue preserves native-summary backend support");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "support=stub"),
                      "tablo run --drain-jit-queue preserves stub-only backend support");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "support=none"),
                      "tablo run --drain-jit-queue preserves unsupported backend support");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "attempts=1"),
                      "tablo run --drain-jit-queue records one compile attempt");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "JIT work queue (threshold=2):"),
                      "tablo run --drain-jit-queue still prints the queue dump when requested");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_drain.out", "<no queued functions>"),
                      "tablo run --drain-jit-queue empties the work queue");
      }
      remove(".tmp_cli_jit_drain.out");

      snprintf(raw_cmd,
               sizeof(raw_cmd),
               "\"%s\" run --profile-jit --jit-auto-compile --jit-hot-threshold 2 tablo_tests/jit_profile_test.tblo",
               exe);
      if (run_raw_command_capture(raw_cmd,
                                  ".tmp_cli_jit_auto.out",
                                  1,
                                  "tablo run --jit-auto-compile dispatches through compiled entries")) {
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotLeaf"),
                      "tablo run --jit-auto-compile reports the compiled sync function");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "state=compiled-native"),
                      "tablo run --jit-auto-compile promotes native shapes to compiled-native");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "reason=native-hint"),
                      "tablo run --jit-auto-compile reports compiler-hinted native backend decisions");
          TEST_ASSERT(file_lacks_text(".tmp_cli_jit_auto.out", "reason=native-exact"),
                      "tablo run --jit-auto-compile no longer needs exact bytecode rematching for the covered leaf families");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "compiledCalls=2"),
                      "tablo run --jit-auto-compile routes later native calls through the compiled entry");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotFallback"),
                      "tablo run --jit-auto-compile reports native multiply sync functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotStub"),
                      "tablo run --jit-auto-compile still reports stub-fallback sync functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotSub"),
                      "tablo run --jit-auto-compile reports native subtract sync functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotDiv"),
                      "tablo run --jit-auto-compile reports native divide sync functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotMod"),
                      "tablo run --jit-auto-compile reports native modulo sync functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotAnd"),
                      "tablo run --jit-auto-compile reports native bitand sync functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotOr"),
                      "tablo run --jit-auto-compile reports native bitor sync functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotXor"),
                      "tablo run --jit-auto-compile reports native bitxor sync functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotDynamic"),
                      "tablo run --jit-auto-compile reports dynamically-invoked native sync functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "invokeDynamic"),
                      "tablo run --jit-auto-compile reports higher-order wrapper functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotAdd2"),
                      "tablo run --jit-auto-compile reports two-arg native add functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotSub2"),
                      "tablo run --jit-auto-compile reports two-arg native subtract functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotMul2"),
                      "tablo run --jit-auto-compile reports two-arg native multiply functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotAnd2"),
                      "tablo run --jit-auto-compile reports two-arg native bitand functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotOr2"),
                      "tablo run --jit-auto-compile reports two-arg native bitor functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotXor2"),
                      "tablo run --jit-auto-compile reports two-arg native bitxor functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotDynamic2"),
                      "tablo run --jit-auto-compile reports dynamically-invoked two-arg native functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "invokeDynamic2"),
                      "tablo run --jit-auto-compile reports two-arg higher-order wrappers");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotLt2"),
                      "tablo run --jit-auto-compile reports two-arg native less-than functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotLe2"),
                      "tablo run --jit-auto-compile reports two-arg native less-equal functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotEq2"),
                      "tablo run --jit-auto-compile reports two-arg native equality functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotNe2"),
                      "tablo run --jit-auto-compile reports two-arg native inequality functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotGt2"),
                      "tablo run --jit-auto-compile reports two-arg native greater-than functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotGe2"),
                      "tablo run --jit-auto-compile reports two-arg native greater-equal functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotLtConst"),
                      "tablo run --jit-auto-compile reports local-const less-than bool functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotEqConst"),
                      "tablo run --jit-auto-compile reports local-const equality bool functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotGeConst"),
                      "tablo run --jit-auto-compile reports local-const greater-equal bool functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotMinConst"),
                      "tablo run --jit-auto-compile reports local-const min selector functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotMaxConst"),
                      "tablo run --jit-auto-compile reports local-const max selector functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedMinConst"),
                      "tablo run --jit-auto-compile reports guarded local-const min selector functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedMaxConst"),
                      "tablo run --jit-auto-compile reports guarded local-const max selector functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "invokeDynamicBool1"),
                      "tablo run --jit-auto-compile reports one-arg boolean higher-order wrappers");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedLtBool2"),
                      "tablo run --jit-auto-compile reports guarded two-arg less-than bool functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedLeBool2"),
                      "tablo run --jit-auto-compile reports guarded two-arg less-equal bool functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedEqBool2"),
                      "tablo run --jit-auto-compile reports guarded two-arg equality bool functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedNeBool2"),
                      "tablo run --jit-auto-compile reports guarded two-arg inequality bool functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedGtBool2"),
                      "tablo run --jit-auto-compile reports guarded two-arg greater-than bool functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedGeBool2"),
                      "tablo run --jit-auto-compile reports guarded two-arg greater-equal bool functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "invokeDynamicBool2"),
                      "tablo run --jit-auto-compile reports boolean higher-order wrappers");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotMinLt2"),
                      "tablo run --jit-auto-compile reports two-arg native min selector functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotMaxLt2"),
                      "tablo run --jit-auto-compile reports two-arg native max selector functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotMinLe2"),
                      "tablo run --jit-auto-compile reports two-arg native less-equal selectors");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotMaxGt2"),
                      "tablo run --jit-auto-compile reports two-arg native greater-than selectors");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "hotMaxGe2"),
                      "tablo run --jit-auto-compile reports two-arg native greater-equal selectors");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedMinLt2"),
                      "tablo run --jit-auto-compile reports guarded min selector functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedMaxLt2"),
                      "tablo run --jit-auto-compile reports guarded max selector functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedMinLe2"),
                      "tablo run --jit-auto-compile reports guarded less-equal selectors");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedMaxGt2"),
                      "tablo run --jit-auto-compile reports guarded greater-than selectors");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedMaxGe2"),
                      "tablo run --jit-auto-compile reports guarded greater-equal selectors");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedAdd2"),
                      "tablo run --jit-auto-compile reports guarded two-arg native add functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedSub2"),
                      "tablo run --jit-auto-compile reports guarded two-arg native subtract functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedMul2"),
                      "tablo run --jit-auto-compile reports guarded two-arg native multiply functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedAnd2"),
                      "tablo run --jit-auto-compile reports guarded two-arg native bitand functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedOr2"),
                      "tablo run --jit-auto-compile reports guarded two-arg native bitor functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "guardedXor2"),
                      "tablo run --jit-auto-compile reports guarded two-arg native bitxor functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "state=compiled-stub"),
                      "tablo run --jit-auto-compile keeps unsupported sync shapes on the stub backend");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "reason=stub-fallback"),
                      "tablo run --jit-auto-compile reports the stub fallback reason");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "asyncLeaf"),
                      "tablo run --jit-auto-compile still reports async hot functions");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "state=failed"),
                      "tablo run --jit-auto-compile marks unsupported async functions as failed");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "reason=unsupported-async"),
                      "tablo run --jit-auto-compile reports the async unsupported reason");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "support=stub+native-summary"),
                      "tablo run --jit-auto-compile preserves native-summary backend support");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "support=stub"),
                      "tablo run --jit-auto-compile preserves stub-only backend support");
          TEST_ASSERT(file_contains_text(".tmp_cli_jit_auto.out", "support=none"),
                      "tablo run --jit-auto-compile preserves unsupported backend support");
      }
      remove(".tmp_cli_jit_auto.out");

      run_cli_command(exe,
                      "run --profile-jit --jit-hot-threshold 0 tablo_tests/jit_profile_test.tblo",
                      0,
                    "tablo run rejects zero --jit-hot-threshold");
}

static void test_cli_lsp_command(void) {
    char raw_cmd[2048];
    char input_path[256];
    char diag_input_path[256];
    char unsaved_input_path[256];

    printf("Testing CLI 'lsp' command...\n");

    const char* exe = find_tablo_executable();
    TEST_ASSERT(exe != NULL, "Locate tablo executable for lsp");
    if (!exe) return;

    snprintf(raw_cmd, sizeof(raw_cmd), "\"%s\" lsp symbols tablo_tests/lsp_symbols_test.tblo", exe);
    run_raw_command_capture_contains(raw_cmd,
                                     ".tmp_cli_lsp_symbols.json",
                                     "\"name\":\"Point\"",
                                     1,
                                     "tablo lsp symbols prints document symbols JSON");

    snprintf(raw_cmd, sizeof(raw_cmd), "\"%s\" lsp symbols tablo_tests/does_not_exist.tblo", exe);
    run_raw_command_capture_contains(raw_cmd,
                                     ".tmp_cli_lsp_missing.out",
                                     "Failed to read",
                                     0,
                                     "tablo lsp symbols reports missing-file errors");

    input_path[0] = '\0';
    TEST_ASSERT(write_temp_lsp_stdio_input(input_path, sizeof(input_path)),
                "Create temporary LSP stdio input");
    if (input_path[0] != '\0') {
        snprintf(raw_cmd, sizeof(raw_cmd), "\"%s\" lsp --stdio < \"%s\"", exe, input_path);
        if (run_raw_command_capture(raw_cmd,
                                    ".tmp_cli_lsp_stdio.out",
                                    1,
                                    "tablo lsp --stdio handles initialize/symbol/hover/definition/shutdown")) {
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_stdio.out", "\"documentSymbolProvider\":true"),
                        "tablo lsp --stdio advertises document symbols");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_stdio.out", "\"hoverProvider\":true"),
                        "tablo lsp --stdio advertises hover");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_stdio.out", "\"definitionProvider\":true"),
                        "tablo lsp --stdio advertises definition");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_stdio.out", "\"name\":\"Point\""),
                        "tablo lsp --stdio returns document symbols");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_stdio.out", "\"name\":\"load\""),
                        "tablo lsp --stdio returns async function symbols");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_stdio.out", "const LIMIT: int"),
                        "tablo lsp --stdio returns hover details");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_stdio.out", "var total: int"),
                        "tablo lsp --stdio returns local hover details");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_stdio.out", "\"start\":{\"line\":1,\"character\":4}"),
                        "tablo lsp --stdio returns definition locations");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_stdio.out", "\"start\":{\"line\":35,\"character\":14}"),
                        "tablo lsp --stdio resolves parameter definitions");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_stdio.out", "\"start\":{\"line\":9,\"character\":5}"),
                        "tablo lsp --stdio resolves named type definitions");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_stdio.out", "type T: Formatter"),
                        "tablo lsp --stdio returns generic type-parameter hover details");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_stdio.out", "\"start\":{\"line\":57,\"character\":14}"),
                        "tablo lsp --stdio resolves generic type-parameter definitions");
        }
        remove(input_path);
        remove(".tmp_cli_lsp_stdio.out");
    }

    diag_input_path[0] = '\0';
    TEST_ASSERT(write_temp_lsp_diagnostics_input(diag_input_path, sizeof(diag_input_path)),
                "Create temporary LSP diagnostics stdio input");
    if (diag_input_path[0] != '\0') {
        snprintf(raw_cmd, sizeof(raw_cmd), "\"%s\" lsp --stdio < \"%s\"", exe, diag_input_path);
        if (run_raw_command_capture(raw_cmd,
                                    ".tmp_cli_lsp_diag.out",
                                    1,
                                    "tablo lsp --stdio publishes diagnostics for document sync")) {
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_diag.out", "\"method\":\"textDocument/publishDiagnostics\""),
                        "tablo lsp --stdio emits publishDiagnostics notifications");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_diag.out", "\"severity\":1"),
                        "tablo lsp --stdio emits error-severity diagnostics");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_diag.out", "expected int, got string"),
                        "tablo lsp --stdio includes diagnostic messages");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_diag.out", "\"uri\":\"file:///virtual/lsp_diag_test.tblo\",\"diagnostics\":[]"),
                        "tablo lsp --stdio clears diagnostics on didClose");
        }
        remove(diag_input_path);
        remove(".tmp_cli_lsp_diag.out");
    }

    unsaved_input_path[0] = '\0';
    TEST_ASSERT(write_temp_lsp_unsaved_input(unsaved_input_path, sizeof(unsaved_input_path)),
                "Create temporary LSP unsaved-buffer stdio input");
    if (unsaved_input_path[0] != '\0') {
        snprintf(raw_cmd, sizeof(raw_cmd), "\"%s\" lsp --stdio < \"%s\"", exe, unsaved_input_path);
        if (run_raw_command_capture(raw_cmd,
                                    ".tmp_cli_lsp_unsaved.out",
                                    1,
                                    "tablo lsp --stdio resolves documentSymbol/hover/definition from unsaved documents")) {
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_unsaved.out", "\"name\":\"CHANGED\""),
                        "tablo lsp --stdio uses changed in-memory text for document symbols");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_unsaved.out", "var fresh: int"),
                        "tablo lsp --stdio uses changed in-memory text for hover");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_unsaved.out",
                                           "\"start\":{\"line\":6,\"character\":8}"),
                        "tablo lsp --stdio resolves local definitions from unsaved documents");
            TEST_ASSERT(file_contains_text(".tmp_cli_lsp_unsaved.out",
                                           "\"start\":{\"line\":0,\"character\":7}"),
                        "tablo lsp --stdio resolves type definitions from unsaved documents");
        }
        remove(unsaved_input_path);
        remove(".tmp_cli_lsp_unsaved.out");
    }
}

static void test_cli_debug_command(void) {
    char raw_cmd[2048];

    printf("Testing CLI 'debug' command...\n");

    const char* exe = find_tablo_executable();
    TEST_ASSERT(exe != NULL, "Locate tablo executable for debug");
    if (!exe) return;

    snprintf(raw_cmd,
             sizeof(raw_cmd),
             "\"%s\" debug --break 2 tablo_tests/debug_breakpoint_test.tblo",
             exe);
    if (run_raw_command_capture(raw_cmd,
                                ".tmp_cli_debug_break.out",
                                1,
                                "tablo debug stops at a source breakpoint")) {
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_break.out",
                                       "Breakpoint hit at"),
                    "tablo debug reports breakpoint hits");
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_break.out",
                                       "debug_breakpoint_test.tblo:2 in helper"),
                    "tablo debug reports stop file, line, and function");
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_break.out",
                                       "Source:     var current: int = value + 1;"),
                    "tablo debug prints the stopped source line");
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_break.out",
                                       "#0 helper"),
                    "tablo debug prints the top stack frame");
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_break.out",
                                       "#1 main"),
                    "tablo debug prints caller stack frames");
    }
    remove(".tmp_cli_debug_break.out");

    snprintf(raw_cmd,
             sizeof(raw_cmd),
             "\"%s\" debug --break 99 tablo_tests/debug_breakpoint_test.tblo",
             exe);
    run_raw_command_capture_contains(raw_cmd,
                                     ".tmp_cli_debug_miss.out",
                                     "Breakpoint not hit: tablo_tests/debug_breakpoint_test.tblo:99",
                                     0,
                                     "tablo debug returns non-zero when a breakpoint is not hit");

    snprintf(raw_cmd,
             sizeof(raw_cmd),
             "\"%s\" debug --break 99 --break 2 tablo_tests/debug_breakpoint_test.tblo",
             exe);
    if (run_raw_command_capture(raw_cmd,
                                ".tmp_cli_debug_multi_break.out",
                                1,
                                "tablo debug accepts multiple breakpoints")) {
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_multi_break.out",
                                       "debug_breakpoint_test.tblo:2 in helper"),
                    "tablo debug stops on the first matching breakpoint");
    }
    remove(".tmp_cli_debug_multi_break.out");

    snprintf(raw_cmd,
             sizeof(raw_cmd),
             "\"%s\" debug --break tablo_tests/fixtures/debug_breakpoint_helper.tblo:2 "
             "tablo_tests/debug_breakpoint_multifile_test.tblo",
             exe);
    if (run_raw_command_capture(raw_cmd,
                                ".tmp_cli_debug_multifile.out",
                                1,
                                "tablo debug supports breakpoints in imported source files")) {
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_multifile.out",
                                       "fixtures"),
                    "tablo debug reports imported-file breakpoint locations");
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_multifile.out",
                                       "debug_breakpoint_helper.tblo:2 in compute"),
                    "tablo debug stops in the imported helper function");
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_multifile.out",
                                       "#1 main"),
                    "tablo debug preserves the caller stack for imported-file breakpoints");
    }
    remove(".tmp_cli_debug_multifile.out");

    run_cli_command(exe,
                    "debug --break nope tablo_tests/debug_breakpoint_test.tblo",
                    0,
                    "tablo debug rejects invalid --break values");

    run_cli_command(exe,
                    "debug --break tablo_tests/fixtures/debug_breakpoint_helper.tblo:nope "
                    "tablo_tests/debug_breakpoint_multifile_test.tblo",
                    0,
                    "tablo debug rejects invalid file:line breakpoint values");

    snprintf(raw_cmd,
             sizeof(raw_cmd),
             "\"%s\" debug --break 8 --step-in tablo_tests/debug_breakpoint_test.tblo",
             exe);
    if (run_raw_command_capture(raw_cmd,
                                ".tmp_cli_debug_step_in.out",
                                1,
                                "tablo debug supports step-in after a breakpoint")) {
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_step_in.out",
                                       "Breakpoint hit at tablo_tests/debug_breakpoint_test.tblo:8 in main"),
                    "tablo debug step-in reports the initial breakpoint");
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_step_in.out",
                                       "Step hit at tablo_tests/debug_breakpoint_test.tblo:2 in helper"),
                    "tablo debug step-in stops inside the callee");
    }
    remove(".tmp_cli_debug_step_in.out");

    snprintf(raw_cmd,
             sizeof(raw_cmd),
             "\"%s\" debug --break 8 --step-over tablo_tests/debug_breakpoint_test.tblo",
             exe);
    if (run_raw_command_capture(raw_cmd,
                                ".tmp_cli_debug_step_over.out",
                                1,
                                "tablo debug supports step-over after a breakpoint")) {
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_step_over.out",
                                       "Step hit at tablo_tests/debug_breakpoint_test.tblo:9 in main"),
                    "tablo debug step-over stops at the next line in the caller");
    }
    remove(".tmp_cli_debug_step_over.out");

    snprintf(raw_cmd,
             sizeof(raw_cmd),
             "\"%s\" debug --break 2 --step-out tablo_tests/debug_breakpoint_test.tblo",
             exe);
    if (run_raw_command_capture(raw_cmd,
                                ".tmp_cli_debug_step_out.out",
                                1,
                                "tablo debug supports step-out after a breakpoint")) {
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_step_out.out",
                                       "Breakpoint hit at tablo_tests/debug_breakpoint_test.tblo:2 in helper"),
                    "tablo debug step-out reports the initial helper breakpoint");
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_step_out.out",
                                       "Step hit at tablo_tests/debug_breakpoint_test.tblo:8 in main"),
                    "tablo debug step-out returns to the caller frame");
    }
    remove(".tmp_cli_debug_step_out.out");

    snprintf(raw_cmd,
             sizeof(raw_cmd),
             "\"%s\" debug --break 8 --break 2 --continue tablo_tests/debug_breakpoint_test.tblo",
             exe);
    if (run_raw_command_capture(raw_cmd,
                                ".tmp_cli_debug_continue.out",
                                1,
                                "tablo debug supports continue after a breakpoint")) {
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_continue.out",
                                       "Step hit") == 0,
                    "tablo debug continue does not mislabel the next stop as a step");
        TEST_ASSERT(file_contains_text(".tmp_cli_debug_continue.out",
                                       "Breakpoint hit at tablo_tests/debug_breakpoint_test.tblo:2 in helper"),
                    "tablo debug continue resumes to the next breakpoint");
    }
    remove(".tmp_cli_debug_continue.out");
}

static void test_cli_dap_command(void) {
    char raw_cmd[2048];
    char input_path[256];
    char exception_input_path[256];
    char stepout_input_path[256];
    char stoponentry_input_path[256];
    char pause_input_path[256];
    char setvariable_input_path[256];
    char condition_input_path[256];
    char hitcondition_input_path[256];

    printf("Testing CLI 'dap' command...\n");

    const char* exe = find_tablo_executable();
    TEST_ASSERT(exe != NULL, "Locate tablo executable for dap");
    if (!exe) return;

    input_path[0] = '\0';
    TEST_ASSERT(write_temp_dap_stdio_input(input_path, sizeof(input_path)),
                "Create temporary DAP stdio input");
    if (input_path[0] != '\0') {
        snprintf(raw_cmd, sizeof(raw_cmd), "\"%s\" dap --stdio < \"%s\"", exe, input_path);
        if (run_raw_command_capture(raw_cmd,
                                    ".tmp_cli_dap_stdio.out",
                                    1,
                                    "tablo dap --stdio handles launch, breakpoints, stepping, and disconnect")) {
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"supportsPauseRequest\":true"),
                        "tablo dap --stdio advertises pause support");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"supportsSetVariable\":true"),
                        "tablo dap --stdio advertises setVariable support");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"supportsConfigurationDoneRequest\":true"),
                        "tablo dap --stdio advertises configurationDone support");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"event\":\"initialized\""),
                        "tablo dap --stdio emits initialized");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"command\":\"setBreakpoints\""),
                        "tablo dap --stdio responds to setBreakpoints");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"verified\":true"),
                        "tablo dap --stdio verifies source breakpoints");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"event\":\"stopped\""),
                        "tablo dap --stdio emits stopped events");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"reason\":\"breakpoint\""),
                        "tablo dap --stdio reports breakpoint stops");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"name\":\"main\""),
                        "tablo dap --stdio returns main stack frames");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"reason\":\"step\""),
                        "tablo dap --stdio reports step stops");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"name\":\"helper\""),
                        "tablo dap --stdio returns helper stack frames after step-in");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"command\":\"scopes\""),
                        "tablo dap --stdio responds to scopes");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"name\":\"Arguments\""),
                        "tablo dap --stdio returns an arguments scope");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"name\":\"Locals\""),
                        "tablo dap --stdio returns a locals scope");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"name\":\"Globals\""),
                        "tablo dap --stdio returns a globals scope");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"command\":\"variables\""),
                        "tablo dap --stdio responds to variables");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"request_seq\":11,\"success\":true,\"command\":\"variables\",\"body\":{\"variables\":[{\"name\":\"value\",\"value\":\"4\""),
                        "tablo dap --stdio returns parameter variables through the arguments scope");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"name\":\"point\",\"value\":\"{field0: 4, field1: 5}\",\"type\":\"Point\",\"variablesReference\":4030000"),
                        "tablo dap --stdio returns expandable record locals");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"name\":\"field0\",\"value\":\"4\""),
                        "tablo dap --stdio expands record fields");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"name\":\"field1\",\"value\":\"5\""),
                        "tablo dap --stdio expands all record fields");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"request_seq\":16,\"success\":false,\"command\":\"scopes\",\"message\":\"Invalid frameId\""),
                        "tablo dap --stdio rejects stale frame ids after a later stop");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"request_seq\":17,\"success\":false,\"command\":\"variables\",\"message\":\"Unknown variablesReference\""),
                        "tablo dap --stdio rejects stale child references after resume");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"name\":\"numbers\",\"value\":\"[4, 6]\",\"type\":\"array\",\"variablesReference\":4040001"),
                        "tablo dap --stdio returns expandable array locals");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"name\":\"[0]\",\"value\":\"4\""),
                        "tablo dap --stdio expands indexed array elements");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"name\":\"[1]\",\"value\":\"6\""),
                        "tablo dap --stdio expands all indexed array elements");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"command\":\"evaluate\""),
                        "tablo dap --stdio responds to evaluate");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"request_seq\":14,\"success\":true,\"command\":\"evaluate\",\"body\":{\"result\":\"4\",\"type\":\"int\",\"variablesReference\":0}"),
                        "tablo dap --stdio evaluates record field expressions in stopped frames");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"request_seq\":21,\"success\":true,\"command\":\"evaluate\",\"body\":{\"result\":\"6\",\"type\":\"int\",\"variablesReference\":0}"),
                        "tablo dap --stdio evaluates indexed array expressions in stopped frames");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"request_seq\":22,\"success\":true,\"command\":\"evaluate\",\"body\":{\"result\":\"tablo_tests/debug_dap_variables_test.tblo\",\"type\":\"string\",\"variablesReference\":0}"),
                        "tablo dap --stdio evaluates global identifiers in stopped frames");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"request_seq\":23,\"success\":false,\"command\":\"evaluate\",\"message\":\"Unknown identifier in current debug scope\""),
                        "tablo dap --stdio reports invalid evaluate identifiers cleanly");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"command\":\"continue\""),
                        "tablo dap --stdio responds to continue");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"allThreadsContinued\":true"),
                        "tablo dap --stdio marks continue as all-threads");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"event\":\"output\""),
                        "tablo dap --stdio emits captured program output");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"category\":\"stdout\""),
                        "tablo dap --stdio routes program stdout through output events");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stdio.out", "\"event\":\"terminated\""),
                        "tablo dap --stdio emits terminated");
        }
        remove(input_path);
        remove(".tmp_cli_dap_stdio.out");
    }

    exception_input_path[0] = '\0';
    TEST_ASSERT(write_temp_dap_exception_input(exception_input_path, sizeof(exception_input_path)),
                "Create temporary DAP exception stdio input");
    if (exception_input_path[0] != '\0') {
        snprintf(raw_cmd, sizeof(raw_cmd), "\"%s\" dap --stdio < \"%s\"", exe, exception_input_path);
        if (run_raw_command_capture(raw_cmd,
                                    ".tmp_cli_dap_exception.out",
                                    1,
                                    "tablo dap --stdio supports exception breakpoints")) {
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"exceptionBreakpointFilters\""),
                        "tablo dap --stdio advertises exception breakpoint filters");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"command\":\"setExceptionBreakpoints\""),
                        "tablo dap --stdio responds to setExceptionBreakpoints");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"reason\":\"exception\""),
                        "tablo dap --stdio reports exception stops");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"text\":\"Panic: kaboom"),
                        "tablo dap --stdio includes panic text in exception stops");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"supportsExceptionInfoRequest\":true"),
                        "tablo dap --stdio advertises exceptionInfo support");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"request_seq\":5,\"success\":true,\"command\":\"exceptionInfo\""),
                        "tablo dap --stdio responds to exceptionInfo");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"exceptionId\":\"panic\""),
                        "tablo dap --stdio reports panic exception ids");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"breakMode\":\"always\""),
                        "tablo dap --stdio reports exception break mode");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"description\":\"Panic: kaboom\""),
                        "tablo dap --stdio reports exception descriptions");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"name\":\"explode\""),
                        "tablo dap --stdio returns stack frames for exception stops");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"name\":\"current\",\"value\":\"5\""),
                        "tablo dap --stdio exposes locals at exception stops");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"request_seq\":9,\"success\":true,\"command\":\"evaluate\",\"body\":{\"result\":\"5\",\"type\":\"int\",\"variablesReference\":0}"),
                        "tablo dap --stdio evaluates locals at exception stops");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"category\":\"stderr\""),
                        "tablo dap --stdio routes exception text through stderr output on termination");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "kaboom"),
                        "tablo dap --stdio preserves the panic text through termination");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_exception.out", "\"event\":\"terminated\""),
                        "tablo dap --stdio terminates cleanly after continuing from an exception stop");
        }
        remove(exception_input_path);
        remove(".tmp_cli_dap_exception.out");
    }

    stepout_input_path[0] = '\0';
    TEST_ASSERT(write_temp_dap_stepout_input(stepout_input_path, sizeof(stepout_input_path)),
                "Create temporary DAP stepOut stdio input");
    if (stepout_input_path[0] != '\0') {
        snprintf(raw_cmd, sizeof(raw_cmd), "\"%s\" dap --stdio < \"%s\"", exe, stepout_input_path);
        if (run_raw_command_capture(raw_cmd,
                                    ".tmp_cli_dap_stepout.out",
                                    1,
                                    "tablo dap --stdio supports stepOut")) {
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stepout.out", "\"command\":\"stepOut\""),
                        "tablo dap --stdio responds to stepOut");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stepout.out", "\"reason\":\"step\""),
                        "tablo dap --stdio reports step stops for stepOut");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stepout.out", "\"request_seq\":7,\"success\":true,\"command\":\"stackTrace\""),
                        "tablo dap --stdio returns stackTrace after stepOut");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stepout.out", "\"stackFrames\":[{\"id\":30001,\"name\":\"main\""),
                        "tablo dap --stdio stepOut returns to the caller frame");
        }
        remove(stepout_input_path);
        remove(".tmp_cli_dap_stepout.out");
    }

    stoponentry_input_path[0] = '\0';
    TEST_ASSERT(write_temp_dap_stoponentry_input(stoponentry_input_path, sizeof(stoponentry_input_path)),
                "Create temporary DAP stopOnEntry stdio input");
    if (stoponentry_input_path[0] != '\0') {
        snprintf(raw_cmd, sizeof(raw_cmd), "\"%s\" dap --stdio < \"%s\"", exe, stoponentry_input_path);
        if (run_raw_command_capture(raw_cmd,
                                    ".tmp_cli_dap_stoponentry.out",
                                    1,
                                    "tablo dap --stdio supports launch.stopOnEntry")) {
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stoponentry.out",
                                           "\"request_seq\":2,\"success\":true,\"command\":\"launch\""),
                        "tablo dap --stdio accepts launch.stopOnEntry");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stoponentry.out", "\"reason\":\"entry\""),
                        "tablo dap --stdio reports entry stops");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stoponentry.out",
                                           "\"request_seq\":4,\"success\":true,\"command\":\"stackTrace\""),
                        "tablo dap --stdio returns stackTrace after an entry stop");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stoponentry.out",
                                           "\"stackFrames\":[{\"id\":10001,\"name\":\"main\""),
                        "tablo dap --stdio entry stops in main");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stoponentry.out", "\"command\":\"continue\""),
                        "tablo dap --stdio continues cleanly after an entry stop");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stoponentry.out",
                                           "\"request_seq\":6,\"success\":false,\"command\":\"stackTrace\",\"message\":\"Program has terminated\""),
                        "tablo dap --stdio rejects stackTrace after termination");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stoponentry.out",
                                           "\"request_seq\":7,\"success\":false,\"command\":\"evaluate\",\"message\":\"Program has terminated\""),
                        "tablo dap --stdio rejects evaluate after termination");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_stoponentry.out", "\"event\":\"terminated\""),
                        "tablo dap --stdio terminates cleanly after continuing from entry");
        }
        remove(stoponentry_input_path);
        remove(".tmp_cli_dap_stoponentry.out");
    }

    pause_input_path[0] = '\0';
    TEST_ASSERT(write_temp_dap_pause_input(pause_input_path, sizeof(pause_input_path)),
                "Create temporary DAP pause stdio input");
    if (pause_input_path[0] != '\0') {
        snprintf(raw_cmd, sizeof(raw_cmd), "\"%s\" dap --stdio < \"%s\"", exe, pause_input_path);
        if (run_raw_command_capture(raw_cmd,
                                    ".tmp_cli_dap_pause.out",
                                    1,
                                    "tablo dap --stdio supports pause")) {
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_pause.out",
                                           "\"request_seq\":4,\"success\":true,\"command\":\"pause\""),
                        "tablo dap --stdio responds to pause");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_pause.out", "\"reason\":\"pause\""),
                        "tablo dap --stdio reports pause stops");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_pause.out",
                                           "\"request_seq\":5,\"success\":true,\"command\":\"stackTrace\""),
                        "tablo dap --stdio returns stackTrace after pause");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_pause.out", "\"command\":\"continue\""),
                        "tablo dap --stdio continues after pause");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_pause.out", "\"event\":\"terminated\""),
                        "tablo dap --stdio terminates cleanly after continuing from pause");
        }
        remove(pause_input_path);
        remove(".tmp_cli_dap_pause.out");
    }

    setvariable_input_path[0] = '\0';
    TEST_ASSERT(write_temp_dap_setvariable_input(setvariable_input_path, sizeof(setvariable_input_path)),
                "Create temporary DAP setVariable stdio input");
    if (setvariable_input_path[0] != '\0') {
        snprintf(raw_cmd, sizeof(raw_cmd), "\"%s\" dap --stdio < \"%s\"", exe, setvariable_input_path);
        if (run_raw_command_capture(raw_cmd,
                                    ".tmp_cli_dap_setvariable.out",
                                    1,
                                    "tablo dap --stdio supports setVariable")) {
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_setvariable.out",
                                           "\"request_seq\":7,\"success\":true,\"command\":\"setVariable\",\"body\":{\"value\":\"9\",\"type\":\"int\",\"variablesReference\":0}"),
                        "tablo dap --stdio updates argument variables");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_setvariable.out",
                                           "\"request_seq\":8,\"success\":true,\"command\":\"evaluate\",\"body\":{\"result\":\"9\",\"type\":\"int\",\"variablesReference\":0}"),
                        "tablo dap --stdio evaluates updated arguments");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_setvariable.out",
                                           "\"name\":\"point\",\"value\":\"{field0: 9, field1: 10}\",\"type\":\"Point\",\"variablesReference\":4040000"),
                        "tablo dap --stdio shows locals derived from updated arguments");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_setvariable.out",
                                           "\"name\":\"numbers\",\"value\":\"[9, 11]\",\"type\":\"array\""),
                        "tablo dap --stdio shows array locals derived from updated arguments");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_setvariable.out", "\"name\":\"field0\",\"value\":\"9\""),
                        "tablo dap --stdio expands updated record children");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_setvariable.out",
                                           "\"request_seq\":13,\"success\":true,\"command\":\"evaluate\",\"body\":{\"result\":\"9\",\"type\":\"int\",\"variablesReference\":0}"),
                        "tablo dap --stdio evaluates locals after argument mutation");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_setvariable.out",
                                           "\"request_seq\":17,\"success\":true,\"command\":\"setVariable\",\"body\":{\"value\":\"99\",\"type\":\"int\",\"variablesReference\":0}"),
                        "tablo dap --stdio updates local variables");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_setvariable.out",
                                           "\"request_seq\":18,\"success\":true,\"command\":\"evaluate\",\"body\":{\"result\":\"99\",\"type\":\"int\",\"variablesReference\":0}"),
                        "tablo dap --stdio evaluates updated locals");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_setvariable.out",
                                           "\"request_seq\":19,\"success\":true,\"command\":\"setVariable\",\"body\":{\"value\":\"33\",\"type\":\"int\",\"variablesReference\":0}"),
                        "tablo dap --stdio updates global variables");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_setvariable.out",
                                           "\"request_seq\":20,\"success\":true,\"command\":\"evaluate\",\"body\":{\"result\":\"33\",\"type\":\"int\",\"variablesReference\":0}"),
                        "tablo dap --stdio evaluates updated globals");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_setvariable.out", "\"output\":\"9\\n\""),
                        "tablo dap --stdio emits output from mutated helper state");
        }
        remove(setvariable_input_path);
        remove(".tmp_cli_dap_setvariable.out");
    }

    condition_input_path[0] = '\0';
    TEST_ASSERT(write_temp_dap_condition_input(condition_input_path, sizeof(condition_input_path)),
                "Create temporary DAP conditional-breakpoint stdio input");
    if (condition_input_path[0] != '\0') {
        snprintf(raw_cmd, sizeof(raw_cmd), "\"%s\" dap --stdio < \"%s\"", exe, condition_input_path);
        if (run_raw_command_capture(raw_cmd,
                                    ".tmp_cli_dap_condition.out",
                                    1,
                                    "tablo dap --stdio supports conditional breakpoints")) {
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_condition.out", "\"supportsConditionalBreakpoints\":true"),
                        "tablo dap --stdio advertises conditional breakpoint support");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_condition.out", "\"request_seq\":3,\"success\":true,\"command\":\"setBreakpoints\""),
                        "tablo dap --stdio accepts conditional breakpoints");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_condition.out", "\"reason\":\"breakpoint\""),
                        "tablo dap --stdio stops on conditional breakpoints");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_condition.out", "debug_dap_breakpoint_conditions_test.tblo"),
                        "tablo dap --stdio reports the conditional-breakpoint source file");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_condition.out", "\"line\":6"),
                        "tablo dap --stdio stops on the expected conditional-breakpoint line");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_condition.out", "\"category\":\"stdout\",\"output\":\"0\\n\""),
                        "tablo dap --stdio runs past the first iteration before the conditional breakpoint stops");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_condition.out", "\"name\":\"i\",\"value\":\"1\""),
                        "tablo dap --stdio stops on the expected conditional-breakpoint iteration");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_condition.out",
                                           "\"request_seq\":8,\"success\":true,\"command\":\"evaluate\",\"body\":{\"result\":\"1\",\"type\":\"int\",\"variablesReference\":0}"),
                        "tablo dap --stdio evaluates locals at conditional breakpoints");
        }
        remove(condition_input_path);
        remove(".tmp_cli_dap_condition.out");
    }

    hitcondition_input_path[0] = '\0';
    TEST_ASSERT(write_temp_dap_hitcondition_input(hitcondition_input_path, sizeof(hitcondition_input_path)),
                "Create temporary DAP hit-condition stdio input");
    if (hitcondition_input_path[0] != '\0') {
        snprintf(raw_cmd, sizeof(raw_cmd), "\"%s\" dap --stdio < \"%s\"", exe, hitcondition_input_path);
        if (run_raw_command_capture(raw_cmd,
                                    ".tmp_cli_dap_hitcondition.out",
                                    1,
                                    "tablo dap --stdio supports hit conditions")) {
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_hitcondition.out", "\"supportsHitConditionalBreakpoints\":true"),
                        "tablo dap --stdio advertises hit-condition breakpoint support");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_hitcondition.out", "\"request_seq\":3,\"success\":true,\"command\":\"setBreakpoints\""),
                        "tablo dap --stdio accepts hit-condition breakpoints");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_hitcondition.out", "\"reason\":\"breakpoint\""),
                        "tablo dap --stdio stops on hit conditions");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_hitcondition.out", "debug_dap_breakpoint_conditions_test.tblo"),
                        "tablo dap --stdio reports the hit-condition source file");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_hitcondition.out", "\"line\":6"),
                        "tablo dap --stdio stops on the expected hit-condition line");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_hitcondition.out", "\"category\":\"stdout\",\"output\":\"0\\n\""),
                        "tablo dap --stdio runs past the first hit before the hit-condition breakpoint stops");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_hitcondition.out", "\"name\":\"i\",\"value\":\"1\""),
                        "tablo dap --stdio stops on the expected hit-condition iteration");
            TEST_ASSERT(file_contains_text(".tmp_cli_dap_hitcondition.out",
                                           "\"request_seq\":7,\"success\":true,\"command\":\"evaluate\",\"body\":{\"result\":\"1\",\"type\":\"int\",\"variablesReference\":0}"),
                        "tablo dap --stdio evaluates locals at hit-condition stops");
        }
        remove(hitcondition_input_path);
        remove(".tmp_cli_dap_hitcondition.out");
    }
}

int main(void) {
    printf("Running CLI Test Command Tests...\n\n");
    test_cli_run_command();
    test_cli_test_command();
    test_cli_lsp_command();
    test_cli_debug_command();
    test_cli_dap_command();

    printf("\nCLI Test Command Results:\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
