#include "runtime.h"
#include "path_utils.h"
#include "safe_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

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
    } while (0)

static char* write_temp_vml(const char* content) {
    const char* temp_dir = getenv("TEMP");
    if (!temp_dir) temp_dir = ".";

    char* path = (char*)safe_malloc(512);
    snprintf(path, 512, "%s\\tablo_safety_test_%d.tblo", temp_dir, rand());

    FILE* f = fopen(path, "wb");
    if (!f) {
        free(path);
        return NULL;
    }

    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return path;
}

static void test_runtime_smoke(void) {
    printf("Testing runtime smoke...\n");
    const char* src =
        "func main(): void {\n"
        "    var x: int = 1;\n"
        "    println(x);\n"
        "}\n";

    char* path = write_temp_vml(src);
    TEST_ASSERT(path != NULL, "Temp TabloLang source file created");
    if (!path) return;

    Runtime* rt = runtime_create(path);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt), "runtime_create has no error");
    if (!runtime_has_error(rt)) {
        int rc = runtime_run(rt);
        TEST_ASSERT(rc == 0, "runtime_run returns success");
        TEST_ASSERT(!runtime_has_error(rt), "runtime_run completes without error");
    }

    runtime_free(rt);
    remove(path);
    free(path);
}

static void test_execution_limit(void) {
    printf("Testing execution limit...\n");
    const char* src =
        "func main(): void {\n"
        "    while (true) { }\n"
        "}\n";

    char* path = write_temp_vml(src);
    TEST_ASSERT(path != NULL, "Temp TabloLang source file created");
    if (!path) return;

    Runtime* rt = runtime_create(path);
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime");
    TEST_ASSERT(!runtime_has_error(rt), "runtime_create has no error");
    if (!runtime_has_error(rt)) {
        rt->vm->config.max_instructions = 1000;
        int rc = runtime_run(rt);
        (void)rc;
        TEST_ASSERT(runtime_has_error(rt), "Execution limit triggers runtime error");
    }

    runtime_free(rt);
    remove(path);
    free(path);
}

static void test_path_traversal_rejection(void) {
    printf("Testing path traversal rejection...\n");
    Runtime* rt = runtime_create("..\\definitely_not_allowed.tblo");
    TEST_ASSERT(rt != NULL, "runtime_create returns Runtime");
    TEST_ASSERT(runtime_has_error(rt), "Path traversal is rejected");
    runtime_free(rt);
}

static void test_sandbox_symlink_escape_rejection(void) {
    printf("Testing sandbox symlink/junction escape rejection...\n");

    const char* temp_dir = getenv("TEMP");
    if (!temp_dir || temp_dir[0] == '\0') temp_dir = ".";

    char base_dir[512];
    char sandbox_dir[512];
    char outside_dir[512];
    char secret_path[512];
    char link_path[512];
#ifdef _WIN32
    snprintf(base_dir, sizeof(base_dir), "%s\\tablo_symlink_safety_%d", temp_dir, rand());
    snprintf(sandbox_dir, sizeof(sandbox_dir), "%s\\sandbox", base_dir);
    snprintf(outside_dir, sizeof(outside_dir), "%s\\outside", base_dir);
    snprintf(secret_path, sizeof(secret_path), "%s\\secret.txt", outside_dir);
    snprintf(link_path, sizeof(link_path), "%s\\link", sandbox_dir);
#else
    snprintf(base_dir, sizeof(base_dir), "%s/tablo_symlink_safety_%d", temp_dir, rand());
    snprintf(sandbox_dir, sizeof(sandbox_dir), "%s/sandbox", base_dir);
    snprintf(outside_dir, sizeof(outside_dir), "%s/outside", base_dir);
    snprintf(secret_path, sizeof(secret_path), "%s/secret.txt", outside_dir);
    snprintf(link_path, sizeof(link_path), "%s/link", sandbox_dir);
#endif

#ifdef _WIN32
    int ok_base = (_mkdir(base_dir) == 0);
    int ok_sandbox = (_mkdir(sandbox_dir) == 0);
    int ok_outside = (_mkdir(outside_dir) == 0);
#else
    int ok_base = (mkdir(base_dir, 0777) == 0);
    int ok_sandbox = (mkdir(sandbox_dir, 0777) == 0);
    int ok_outside = (mkdir(outside_dir, 0777) == 0);
#endif
    TEST_ASSERT(ok_base && ok_sandbox && ok_outside, "Sandbox/link fixture directories created");
    if (!(ok_base && ok_sandbox && ok_outside)) {
        return;
    }

    FILE* secret = fopen(secret_path, "wb");
    TEST_ASSERT(secret != NULL, "Outside secret fixture file created");
    if (!secret) {
#ifdef _WIN32
        _rmdir(outside_dir);
        _rmdir(sandbox_dir);
        _rmdir(base_dir);
#else
        rmdir(outside_dir);
        rmdir(sandbox_dir);
        rmdir(base_dir);
#endif
        return;
    }
    fwrite("secret", 1, 6, secret);
    fclose(secret);

    int link_created = 0;
#ifdef _WIN32
    char mklink_cmd[1600];
    snprintf(mklink_cmd,
             sizeof(mklink_cmd),
             "cmd /c mklink /J \"%s\" \"%s\" >NUL 2>&1",
             link_path,
             outside_dir);
    link_created = (system(mklink_cmd) == 0);
#else
    link_created = (symlink(outside_dir, link_path) == 0);
#endif

    if (!link_created) {
        TEST_ASSERT(1, "Link creation unavailable; skipped symlink escape regression");
#ifdef _WIN32
        remove(secret_path);
        _rmdir(outside_dir);
        _rmdir(sandbox_dir);
        _rmdir(base_dir);
#else
        remove(secret_path);
        rmdir(outside_dir);
        rmdir(sandbox_dir);
        rmdir(base_dir);
#endif
        return;
    }

    const char* err = NULL;
    char* resolved = path_sandbox_resolve_alloc(sandbox_dir, "link/secret.txt", false, &err);
    TEST_ASSERT(resolved == NULL, "Symlink/junction escape path is rejected");
    if (resolved) free(resolved);
    TEST_ASSERT(err != NULL, "Symlink/junction rejection returns an error");

#ifdef _WIN32
    _rmdir(link_path);
    remove(secret_path);
    _rmdir(outside_dir);
    _rmdir(sandbox_dir);
    _rmdir(base_dir);
#else
    remove(secret_path);
    unlink(link_path);
    rmdir(outside_dir);
    rmdir(sandbox_dir);
    rmdir(base_dir);
#endif
}

static void test_sandbox_secure_open_rejects_symlink_swap(void) {
    printf("Testing sandbox secure open against symlink swap...\n");

#ifdef _WIN32
    TEST_ASSERT(1, "Secure symlink-swap open test skipped on Windows");
    return;
#else
    const char* temp_dir = getenv("TMPDIR");
    if (!temp_dir || temp_dir[0] == '\0') temp_dir = ".";

    char base_dir[512];
    char sandbox_dir[512];
    char outside_dir[512];
    char outside_file[512];
    char target_path[512];
    snprintf(base_dir, sizeof(base_dir), "%s/tablo_secure_open_%d", temp_dir, rand());
    snprintf(sandbox_dir, sizeof(sandbox_dir), "%s/sandbox", base_dir);
    snprintf(outside_dir, sizeof(outside_dir), "%s/outside", base_dir);
    snprintf(outside_file, sizeof(outside_file), "%s/secret.txt", outside_dir);
    snprintf(target_path, sizeof(target_path), "%s/target.txt", sandbox_dir);

    int ok_base = (mkdir(base_dir, 0777) == 0);
    int ok_sandbox = (mkdir(sandbox_dir, 0777) == 0);
    int ok_outside = (mkdir(outside_dir, 0777) == 0);
    TEST_ASSERT(ok_base && ok_sandbox && ok_outside, "Secure-open fixture directories created");
    if (!(ok_base && ok_sandbox && ok_outside)) {
        return;
    }

    FILE* secret = fopen(outside_file, "wb");
    TEST_ASSERT(secret != NULL, "Secure-open outside file created");
    if (!secret) {
        rmdir(outside_dir);
        rmdir(sandbox_dir);
        rmdir(base_dir);
        return;
    }
    fwrite("secret", 1, 6, secret);
    fclose(secret);

    const char* resolve_err = NULL;
    char* resolved = path_sandbox_resolve_alloc(sandbox_dir, "target.txt", true, &resolve_err);
    TEST_ASSERT(resolved != NULL, "Resolve missing target before symlink swap");
    if (!resolved) {
        remove(outside_file);
        rmdir(outside_dir);
        rmdir(sandbox_dir);
        rmdir(base_dir);
        return;
    }

    int link_created = (symlink(outside_file, target_path) == 0);
    TEST_ASSERT(link_created, "Symlink swap fixture created");
    if (link_created) {
        const char* open_err = NULL;
        FILE* f = path_sandbox_fopen_resolved(sandbox_dir, resolved, "rb", &open_err);
        TEST_ASSERT(f == NULL, "Secure open rejects symlink swapped after resolve");
        TEST_ASSERT(open_err != NULL, "Secure open reports symlink-swap error");
        if (f) fclose(f);
    }

    free(resolved);
    if (link_created) unlink(target_path);
    remove(outside_file);
    rmdir(outside_dir);
    rmdir(sandbox_dir);
    rmdir(base_dir);
#endif
}

static void test_sandbox_secure_open_rejects_hardlink_escape(void) {
    printf("Testing sandbox secure open against hardlink escape...\n");

#ifdef _WIN32
    TEST_ASSERT(1, "Secure hardlink-escape test skipped on Windows");
    return;
#else
    const char* temp_dir = getenv("TMPDIR");
    if (!temp_dir || temp_dir[0] == '\0') temp_dir = ".";

    char base_dir[512];
    char sandbox_dir[512];
    char outside_dir[512];
    char outside_file[512];
    char hardlink_path[512];
    snprintf(base_dir, sizeof(base_dir), "%s/tablo_hardlink_open_%d", temp_dir, rand());
    snprintf(sandbox_dir, sizeof(sandbox_dir), "%s/sandbox", base_dir);
    snprintf(outside_dir, sizeof(outside_dir), "%s/outside", base_dir);
    snprintf(outside_file, sizeof(outside_file), "%s/secret.txt", outside_dir);
    snprintf(hardlink_path, sizeof(hardlink_path), "%s/alias.txt", sandbox_dir);

    int ok_base = (mkdir(base_dir, 0777) == 0);
    int ok_sandbox = (mkdir(sandbox_dir, 0777) == 0);
    int ok_outside = (mkdir(outside_dir, 0777) == 0);
    TEST_ASSERT(ok_base && ok_sandbox && ok_outside, "Hardlink fixture directories created");
    if (!(ok_base && ok_sandbox && ok_outside)) {
        return;
    }

    FILE* secret = fopen(outside_file, "wb");
    TEST_ASSERT(secret != NULL, "Hardlink outside file created");
    if (!secret) {
        rmdir(outside_dir);
        rmdir(sandbox_dir);
        rmdir(base_dir);
        return;
    }
    fwrite("secret", 1, 6, secret);
    fclose(secret);

    int hardlink_created = (link(outside_file, hardlink_path) == 0);
    if (!hardlink_created) {
        TEST_ASSERT(1, "Hardlink creation unavailable; skipped hardlink escape regression");
        remove(outside_file);
        rmdir(outside_dir);
        rmdir(sandbox_dir);
        rmdir(base_dir);
        return;
    }

    const char* resolve_err = NULL;
    char* resolved = path_sandbox_resolve_alloc(sandbox_dir, "alias.txt", false, &resolve_err);
    TEST_ASSERT(resolved != NULL, "Resolve hardlink path inside sandbox");
    if (resolved) {
        const char* open_err = NULL;
        FILE* f = path_sandbox_fopen_resolved(sandbox_dir, resolved, "rb", &open_err);
        TEST_ASSERT(f == NULL, "Secure open rejects hardlinked regular file alias");
        TEST_ASSERT(open_err != NULL, "Secure open reports hardlink alias error");
        if (f) fclose(f);
        free(resolved);
    }

    remove(hardlink_path);
    remove(outside_file);
    rmdir(outside_dir);
    rmdir(sandbox_dir);
    rmdir(base_dir);
#endif
}

int main(void) {
    printf("Running TabloLang Safety Integration Tests...\n\n");

    srand(1);
    test_runtime_smoke();
    test_execution_limit();
    test_path_traversal_rejection();
    test_sandbox_symlink_escape_rejection();
    test_sandbox_secure_open_rejects_symlink_swap();
    test_sandbox_secure_open_rejects_hardlink_escape();

    printf("\nSafety Integration Test Results:\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
