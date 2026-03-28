#ifdef _WIN32
#define TokenType Win32TokenType
#include <windows.h>
#undef TokenType
#include <sys/types.h>
#include <sys/stat.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif

#include "runtime.h"
#include "safe_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

typedef int (*GlfwTestEmitScrollLastWindowFn)(double xoffset, double yoffset);
typedef int (*GlfwTestEmitWindowResizeLastWindowFn)(int width, int height);
typedef int (*GlfwTestEmitScrollLiveWindowAtFn)(int index, double xoffset, double yoffset);
typedef int (*GlfwTestEmitWindowResizeLiveWindowAtFn)(int index, int width, int height);
typedef int (*GlfwTestClearLastWindowInputCallbackFn)(void);
typedef int (*GlfwTestClearInputCallbackLiveWindowAtFn)(int index);
typedef int (*GlfwTestGetLastWindowCanPostCallbacksFn)(void);
typedef int (*GlfwTestGetLiveWindowCanPostCallbacksAtFn)(int index);
typedef int (*GlfwTestGetLastWindowPendingCallbackCountFn)(void);
typedef int (*GlfwTestGetLiveWindowPendingCallbackCountAtFn)(int index);
typedef int64_t (*GlfwTestGetLastWindowDroppedCallbackCountFn)(void);
typedef int64_t (*GlfwTestResetLastWindowDroppedCallbackCountFn)(void);
typedef int64_t (*GlfwTestGetLiveWindowDroppedCallbackCountAtFn)(int index);
typedef int64_t (*GlfwTestResetLiveWindowDroppedCallbackCountAtFn)(int index);
typedef int64_t (*GlfwTestGetLastWindowInvalidatedCallbackCountFn)(void);
typedef int64_t (*GlfwTestResetLastWindowInvalidatedCallbackCountFn)(void);
typedef int64_t (*GlfwTestGetLiveWindowInvalidatedCallbackCountAtFn)(int index);
typedef int64_t (*GlfwTestResetLiveWindowInvalidatedCallbackCountAtFn)(int index);
typedef int64_t (*GlfwTestGetLastWindowRejectedCallbackCountFn)(void);
typedef int64_t (*GlfwTestResetLastWindowRejectedCallbackCountFn)(void);
typedef int64_t (*GlfwTestGetLiveWindowRejectedCallbackCountAtFn)(int index);
typedef int64_t (*GlfwTestResetLiveWindowRejectedCallbackCountAtFn)(int index);
typedef int (*GlfwTestIsInitializedFn)(void);
typedef int (*GlfwTestGetLiveWindowCountFn)(void);

static unsigned long long next_temp_path_nonce(void) {
    temp_path_counter++;
    return (((unsigned long long)time(NULL)) << 32) ^
           (unsigned long long)clock() ^
           temp_path_counter;
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
    snprintf(path, 512, "%s\\tablo_glfw_host_%llu%s", temp_dir, nonce, ext ? ext : "");
#else
    snprintf(path, 512, "%s/tablo_glfw_host_%llu%s", temp_dir, nonce, ext ? ext : "");
#endif
    return path;
}

static char* write_temp_vml(const char* content) {
    char* path = make_temp_path_with_ext(".tblo");
    FILE* f = fopen(path, "wb");
    if (!f) {
        free(path);
        return NULL;
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return path;
}

static void cleanup_temp(char* path) {
    if (!path) return;
    remove(path);
    free(path);
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

static char* duplicate_path(const char* path) {
    size_t len = 0;
    char* copy = NULL;
    if (!path) return NULL;
    len = strlen(path);
    copy = (char*)safe_malloc(len + 1u);
    memcpy(copy, path, len + 1u);
    return copy;
}

static char* locate_glfw_extension_library(void) {
#ifdef _WIN32
    static const char* candidates[] = {
        "..\\build-glfw\\Release\\tablo_glfw_extension.dll",
        "..\\build-glfw\\Debug\\tablo_glfw_extension.dll",
        "..\\build-glfw\\tablo_glfw_extension.dll",
        "tablo_glfw_extension.dll",
        "..\\Release\\tablo_glfw_extension.dll",
        "..\\Debug\\tablo_glfw_extension.dll"
    };
#elif __APPLE__
    static const char* candidates[] = {
        "../build-glfw/Release/tablo_glfw_extension.dylib",
        "../build-glfw/Debug/tablo_glfw_extension.dylib",
        "../build-glfw/tablo_glfw_extension.dylib",
        "tablo_glfw_extension.dylib",
        "../Release/tablo_glfw_extension.dylib",
        "../Debug/tablo_glfw_extension.dylib"
    };
#else
    static const char* candidates[] = {
        "../build-glfw/Release/tablo_glfw_extension.so",
        "../build-glfw/Debug/tablo_glfw_extension.so",
        "../build-glfw/tablo_glfw_extension.so",
        "tablo_glfw_extension.so",
        "../Release/tablo_glfw_extension.so",
        "../Debug/tablo_glfw_extension.so"
    };
#endif

    int count = (int)(sizeof(candidates) / sizeof(candidates[0]));
    for (int i = 0; i < count; i++) {
        if (file_exists(candidates[i])) {
            return duplicate_path(candidates[i]);
        }
    }
    return NULL;
}

#ifdef _WIN32
typedef HMODULE DynamicLibraryHandle;
#else
typedef void* DynamicLibraryHandle;
#endif

static DynamicLibraryHandle load_dynamic_library(const char* path) {
#ifdef _WIN32
    return LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void unload_dynamic_library(DynamicLibraryHandle handle) {
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
}

static void* load_dynamic_symbol(DynamicLibraryHandle handle, const char* name) {
    if (!handle || !name) return NULL;
#ifdef _WIN32
    return (void*)GetProcAddress(handle, name);
#else
    return dlsym(handle, name);
#endif
}

static const char* glfw_queue_close_source(void) {
    return
        "var glfwHostQueueCloseWindow: any = nil;\n"
        "func onGlfwHostQueueCloseEvent(event: map<string, any>): void {\n"
        "}\n"
        "func main(): void {\n"
        "    if (!glfwInit()) {\n"
        "        panic(\"glfwInit failed in GLFW host queue-close test\");\n"
        "    }\n"
        "    var window: GlfwWindow = glfwCreateWindow(80, 48, \"TabloLang GLFW host queue close\", false);\n"
        "    if (!glfwSetInputCallback(window, onGlfwHostQueueCloseEvent)) {\n"
        "        panic(\"glfwSetInputCallback failed in GLFW host queue-close test\");\n"
        "    }\n"
        "    glfwHostQueueCloseWindow = window as any;\n"
        "}\n";
}

static const char* glfw_non_main_callback_source(void) {
    return
        "func onGlfwNonMainEvent(event: map<string, any>): void {\n"
        "}\n"
        "func setup(): int {\n"
        "    if (!glfwInit()) return -1;\n"
        "    var window: GlfwWindow = glfwCreateWindow(80, 48, \"TabloLang GLFW non-main callback\", false);\n"
        "    if (!glfwSetInputCallback(window, onGlfwNonMainEvent)) {\n"
        "        glfwDestroyWindow(window);\n"
        "        glfwTerminate();\n"
        "        return -2;\n"
        "    }\n"
        "    glfwDestroyWindow(window);\n"
        "    if (!glfwTerminate()) return -3;\n"
        "    return 1;\n"
        "}\n"
        "func main(): void {\n"
        "    if (setup() != 1) {\n"
        "        panic(\"non-main GLFW callback setup failed\");\n"
        "    }\n"
        "}\n";
}

static const char* glfw_live_callback_unload_source(void) {
    return
        "var glfwHostUnloadWindow: any = nil;\n"
        "func onGlfwHostUnloadInput(event: map<string, any>): void {\n"
        "}\n"
        "func onGlfwHostUnloadWindow(event: map<string, any>): void {\n"
        "}\n"
        "func main(): void {\n"
        "    if (!glfwInit()) {\n"
        "        panic(\"glfwInit failed in GLFW host unload test\");\n"
        "    }\n"
        "    extSetPostedCallbackAutoDrain(false);\n"
        "    var window: GlfwWindow = glfwCreateWindow(96, 64, \"TabloLang GLFW host unload\", false);\n"
        "    if (!glfwSetInputCallback(window, onGlfwHostUnloadInput)) {\n"
        "        panic(\"glfwSetInputCallback failed in GLFW host unload test\");\n"
        "    }\n"
        "    if (!glfwSetWindowCallback(window, onGlfwHostUnloadWindow)) {\n"
        "        panic(\"glfwSetWindowCallback failed in GLFW host unload test\");\n"
        "    }\n"
        "    if (!glfwTestEmitScroll(window, 21.0, 22.0)) {\n"
        "        panic(\"glfwTestEmitScroll failed in GLFW host unload test\");\n"
        "    }\n"
        "    glfwHostUnloadWindow = window as any;\n"
        "}\n";
}

static const char* glfw_live_window_callback_unload_source(void) {
    return
        "var glfwHostUnloadWindowOnly: any = nil;\n"
        "func onGlfwHostUnloadWindowOnly(event: map<string, any>): void {\n"
        "}\n"
        "func main(): void {\n"
        "    if (!glfwInit()) {\n"
        "        panic(\"glfwInit failed in GLFW host window unload test\");\n"
        "    }\n"
        "    extSetPostedCallbackAutoDrain(false);\n"
        "    var window: GlfwWindow = glfwCreateWindow(96, 64, \"TabloLang GLFW host window unload\", false);\n"
        "    if (!glfwSetWindowCallback(window, onGlfwHostUnloadWindowOnly)) {\n"
        "        panic(\"glfwSetWindowCallback failed in GLFW host window unload test\");\n"
        "    }\n"
        "    glfwHostUnloadWindowOnly = window as any;\n"
        "}\n";
}

static const char* glfw_mixed_callback_burst_source(void) {
    return
        "var glfwHostMixedWindow: any = nil;\n"
        "func onGlfwHostMixedInput(event: map<string, any>): void {\n"
        "}\n"
        "func onGlfwHostMixedWindow(event: map<string, any>): void {\n"
        "}\n"
        "func main(): void {\n"
        "    if (!glfwInit()) {\n"
        "        panic(\"glfwInit failed in GLFW mixed callback burst test\");\n"
        "    }\n"
        "    extSetPostedCallbackAutoDrain(false);\n"
        "    var window: GlfwWindow = glfwCreateWindow(96, 64, \"TabloLang GLFW mixed callback burst\", false);\n"
        "    if (!glfwSetInputCallback(window, onGlfwHostMixedInput)) {\n"
        "        panic(\"glfwSetInputCallback failed in GLFW mixed callback burst test\");\n"
        "    }\n"
        "    if (!glfwSetWindowCallback(window, onGlfwHostMixedWindow)) {\n"
        "        panic(\"glfwSetWindowCallback failed in GLFW mixed callback burst test\");\n"
        "    }\n"
        "    if (!glfwSetCallbackQueueLimit(window, 1)) {\n"
        "        panic(\"glfwSetCallbackQueueLimit failed in GLFW mixed callback burst test\");\n"
        "    }\n"
        "    glfwHostMixedWindow = window as any;\n"
        "}\n";
}

static const char* glfw_multi_window_isolation_source(void) {
    return
        "var glfwHostWindowA: any = nil;\n"
        "var glfwHostWindowB: any = nil;\n"
        "func onGlfwHostWindowAInput(event: map<string, any>): void {\n"
        "}\n"
        "func onGlfwHostWindowBWindow(event: map<string, any>): void {\n"
        "}\n"
        "func main(): void {\n"
        "    if (!glfwInit()) {\n"
        "        panic(\"glfwInit failed in GLFW multi-window isolation test\");\n"
        "    }\n"
        "    extSetPostedCallbackAutoDrain(false);\n"
        "    var windowA: GlfwWindow = glfwCreateWindow(80, 48, \"TabloLang GLFW window A\", false);\n"
        "    var windowB: GlfwWindow = glfwCreateWindow(80, 48, \"TabloLang GLFW window B\", false);\n"
        "    if (!glfwSetCallbackQueueLimit(windowA, 1)) {\n"
        "        panic(\"glfwSetCallbackQueueLimit windowA failed in GLFW multi-window isolation test\");\n"
        "    }\n"
        "    if (!glfwSetCallbackQueueLimit(windowB, 1)) {\n"
        "        panic(\"glfwSetCallbackQueueLimit windowB failed in GLFW multi-window isolation test\");\n"
        "    }\n"
        "    if (!glfwSetInputCallback(windowA, onGlfwHostWindowAInput)) {\n"
        "        panic(\"glfwSetInputCallback windowA failed in GLFW multi-window isolation test\");\n"
        "    }\n"
        "    if (!glfwSetWindowCallback(windowB, onGlfwHostWindowBWindow)) {\n"
        "        panic(\"glfwSetWindowCallback windowB failed in GLFW multi-window isolation test\");\n"
        "    }\n"
        "    glfwHostWindowA = windowA as any;\n"
        "    glfwHostWindowB = windowB as any;\n"
        "}\n";
}

static const char* glfw_query_helpers_source(void) {
    return
        "func main(): void {\n"
        "    if (!glfwInit()) {\n"
        "        panic(\"glfwInit failed in GLFW query helper test\");\n"
        "    }\n"
        "    var workarea = glfwGetPrimaryMonitorWorkarea();\n"
        "    if (workarea.2 <= 0 || workarea.3 <= 0) {\n"
        "        panic(\"glfwGetPrimaryMonitorWorkarea returned an invalid tuple in GLFW query helper test\");\n"
        "    }\n"
        "    var primaryScale = glfwGetPrimaryMonitorContentScale();\n"
        "    if (primaryScale.0 <= 0.0 || primaryScale.1 <= 0.0) {\n"
        "        panic(\"glfwGetPrimaryMonitorContentScale returned an invalid tuple in GLFW query helper test\");\n"
        "    }\n"
        "    var window: GlfwWindow = glfwCreateWindow(144, 96, \"TabloLang GLFW query helpers\", false);\n"
        "    if (!glfwShowWindow(window)) {\n"
        "        panic(\"glfwShowWindow failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (glfwGetWindowAttrib(window, glfwWindowAttribVisible()) == 0) {\n"
        "        panic(\"glfwGetWindowAttrib visible should report true in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwSetWindowPos(window, 96, 88)) {\n"
        "        panic(\"glfwSetWindowPos failed in GLFW query helper test\");\n"
        "    }\n"
        "    var pos = glfwGetWindowPos(window);\n"
        "    if (pos.0 < 88 || pos.0 > 116 || pos.1 < 80 || pos.1 > 108) {\n"
        "        panic(\"glfwGetWindowPos returned an unexpected tuple in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwSetCursorMode(window, glfwCursorModeHidden())) {\n"
        "        panic(\"glfwSetCursorMode hidden failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (glfwGetCursorMode(window) != glfwCursorModeHidden()) {\n"
        "        panic(\"glfwGetCursorMode should report hidden mode in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwSetCursorMode(window, glfwCursorModeNormal())) {\n"
        "        panic(\"glfwSetCursorMode normal failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (glfwGetCursorMode(window) != glfwCursorModeNormal()) {\n"
        "        panic(\"glfwGetCursorMode should report normal mode in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwFocusWindow(window)) {\n"
        "        panic(\"glfwFocusWindow failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwMaximizeWindow(window)) {\n"
        "        panic(\"glfwMaximizeWindow failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwPollEvents()) {\n"
        "        panic(\"glfwPollEvents after maximize failed in GLFW query helper test\");\n"
        "    }\n"
        "    var maximizedAttr = glfwGetWindowAttrib(window, glfwWindowAttribMaximized());\n"
        "    if (maximizedAttr != 0 && maximizedAttr != 1) {\n"
        "        panic(\"glfwGetWindowAttrib maximized should be boolean-like in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwIconifyWindow(window)) {\n"
        "        panic(\"glfwIconifyWindow failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwPollEvents()) {\n"
        "        panic(\"glfwPollEvents after iconify failed in GLFW query helper test\");\n"
        "    }\n"
        "    var iconifiedAttr = glfwGetWindowAttrib(window, glfwWindowAttribIconified());\n"
        "    if (iconifiedAttr != 0 && iconifiedAttr != 1) {\n"
        "        panic(\"glfwGetWindowAttrib iconified should be boolean-like in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwRestoreWindow(window)) {\n"
        "        panic(\"glfwRestoreWindow failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwPollEvents()) {\n"
        "        panic(\"glfwPollEvents after restore failed in GLFW query helper test\");\n"
        "    }\n"
        "    var focusedAttr = glfwGetWindowAttrib(window, glfwWindowAttribFocused());\n"
        "    if (focusedAttr != 0 && focusedAttr != 1) {\n"
        "        panic(\"glfwGetWindowAttrib focused should be boolean-like in GLFW query helper test\");\n"
        "    }\n"
        "    if (glfwGetKey(window, 65) != 0) {\n"
        "        panic(\"glfwGetKey should report GLFW_RELEASE in GLFW query helper test\");\n"
        "    }\n"
        "    if (glfwGetMouseButton(window, 0) != 0) {\n"
        "        panic(\"glfwGetMouseButton should report GLFW_RELEASE in GLFW query helper test\");\n"
        "    }\n"
        "    var fb = glfwGetFramebufferSize(window);\n"
        "    if (fb.0 <= 0 || fb.1 <= 0) {\n"
        "        panic(\"glfwGetFramebufferSize returned an invalid tuple in GLFW query helper test\");\n"
        "    }\n"
        "    var scale = glfwGetWindowContentScale(window);\n"
        "    if (scale.0 <= 0.0 || scale.1 <= 0.0) {\n"
        "        panic(\"glfwGetWindowContentScale returned an invalid tuple in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwHideWindow(window)) {\n"
        "        panic(\"glfwHideWindow failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (glfwGetWindowAttrib(window, glfwWindowAttribVisible()) != 0) {\n"
        "        panic(\"glfwGetWindowAttrib visible should report false after hide in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwDestroyWindow(window)) {\n"
        "        panic(\"glfwDestroyWindow failed in GLFW query helper test\");\n"
        "    }\n"
        "    var contextA: GlfwWindow = glfwCreateWindowWithContext(96, 64, \"TabloLang GLFW context A\", false);\n"
        "    var contextB: GlfwWindow = glfwCreateWindowWithContext(96, 64, \"TabloLang GLFW context B\", false);\n"
        "    if (glfwHasAnyCurrentContext()) {\n"
        "        panic(\"glfwHasAnyCurrentContext should start false in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwMakeContextCurrent(contextA)) {\n"
        "        panic(\"glfwMakeContextCurrent for contextA failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwHasCurrentContext(contextA)) {\n"
        "        panic(\"glfwHasCurrentContext should report true for contextA in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwHasAnyCurrentContext()) {\n"
        "        panic(\"glfwHasAnyCurrentContext should report true after make current in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwSwapInterval(0)) {\n"
        "        panic(\"glfwSwapInterval failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwSwapBuffers(contextA)) {\n"
        "        panic(\"glfwSwapBuffers for contextA failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (glfwRawMouseMotionSupported()) {\n"
        "        if (!glfwSetCursorMode(contextA, glfwCursorModeDisabled())) {\n"
        "            panic(\"glfwSetCursorMode disabled for contextA failed in GLFW query helper test\");\n"
        "        }\n"
        "        if (!glfwSetRawMouseMotion(contextA, true)) {\n"
        "            panic(\"glfwSetRawMouseMotion enable for contextA failed in GLFW query helper test\");\n"
        "        }\n"
        "        if (!glfwGetRawMouseMotion(contextA)) {\n"
        "            panic(\"glfwGetRawMouseMotion should report true for contextA in GLFW query helper test\");\n"
        "        }\n"
        "        if (!glfwSetRawMouseMotion(contextA, false)) {\n"
        "            panic(\"glfwSetRawMouseMotion disable for contextA failed in GLFW query helper test\");\n"
        "        }\n"
        "        if (glfwGetRawMouseMotion(contextA)) {\n"
        "            panic(\"glfwGetRawMouseMotion should report false for contextA after disable in GLFW query helper test\");\n"
        "        }\n"
        "        if (!glfwSetCursorMode(contextA, glfwCursorModeNormal())) {\n"
        "            panic(\"glfwSetCursorMode normal for contextA after raw mouse motion failed in GLFW query helper test\");\n"
        "        }\n"
        "    }\n"
        "    if (!glfwMakeContextCurrent(contextB)) {\n"
        "        panic(\"glfwMakeContextCurrent for contextB failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwHasCurrentContext(contextB) || glfwHasCurrentContext(contextA)) {\n"
        "        panic(\"Current context should move from contextA to contextB in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwSwapBuffers(contextB)) {\n"
        "        panic(\"glfwSwapBuffers for contextB failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwClearCurrentContext()) {\n"
        "        panic(\"glfwClearCurrentContext failed in GLFW query helper test\");\n"
        "    }\n"
        "    if (glfwHasAnyCurrentContext()) {\n"
        "        panic(\"glfwHasAnyCurrentContext should return false after clear in GLFW query helper test\");\n"
        "    }\n"
        "    if (glfwHasCurrentContext(contextA) || glfwHasCurrentContext(contextB)) {\n"
        "        panic(\"No window should report current context after clear in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwDestroyWindow(contextB) || !glfwDestroyWindow(contextA)) {\n"
        "        panic(\"glfwDestroyWindow failed for context windows in GLFW query helper test\");\n"
        "    }\n"
        "    if (!glfwTerminate()) {\n"
        "        panic(\"glfwTerminate failed in GLFW query helper test\");\n"
        "    }\n"
        "}\n";
}

static void test_glfw_extension_non_main_callback_registration(void) {
    Runtime* rt = NULL;
    RuntimeOptions options = {0};
    char* extension_path = locate_glfw_extension_library();
    char* source_path = NULL;
    const char* extension_paths[1];

    printf("Testing GLFW extension non-main callback registration...\n");

    TEST_ASSERT(extension_path != NULL, "GLFW extension library located for non-main callback test");
    if (!extension_path) {
        return;
    }

    source_path = write_temp_vml(glfw_non_main_callback_source());
    TEST_ASSERT(source_path != NULL, "Temp TabloLang source created for non-main GLFW callback test");
    if (!source_path) {
        free(extension_path);
        return;
    }

    extension_paths[0] = extension_path;
    options.extension_paths = extension_paths;
    options.extension_path_count = 1;

    rt = runtime_create_with_options(source_path, options);
    TEST_ASSERT(rt != NULL, "runtime_create_with_options returned Runtime for non-main GLFW callback test");
    TEST_ASSERT(rt && !runtime_has_error(rt), "Non-main GLFW callback runtime loaded without error");
    if (!rt || runtime_has_error(rt)) {
        if (rt) runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    TEST_ASSERT(runtime_run(rt) == 0, "Non-main GLFW callback runtime run succeeded");
    TEST_ASSERT(!runtime_has_error(rt), "Non-main GLFW callback setup finished without runtime error");

    runtime_free(rt);
    cleanup_temp(source_path);
    free(extension_path);
}

static void test_glfw_extension_unload_with_live_callbacks(void) {
    Runtime* rt = NULL;
    RuntimeOptions options = {0};
    char* extension_path = locate_glfw_extension_library();
    char* source_path = NULL;
    DynamicLibraryHandle lib = NULL;
    GlfwTestEmitScrollLastWindowFn emit_scroll = NULL;
    GlfwTestGetLastWindowCanPostCallbacksFn can_post = NULL;
    GlfwTestIsInitializedFn is_initialized = NULL;
    GlfwTestGetLiveWindowCountFn get_live_window_count = NULL;
    const char* extension_paths[1];

    printf("Testing GLFW extension unload with live callbacks...\n");

    TEST_ASSERT(extension_path != NULL, "GLFW extension library located for live-callback unload test");
    if (!extension_path) {
        return;
    }

    source_path = write_temp_vml(glfw_live_callback_unload_source());
    TEST_ASSERT(source_path != NULL, "Temp TabloLang source created for GLFW live-callback unload test");
    if (!source_path) {
        free(extension_path);
        return;
    }

    extension_paths[0] = extension_path;
    options.extension_paths = extension_paths;
    options.extension_path_count = 1;

    rt = runtime_create_with_options(source_path, options);
    TEST_ASSERT(rt != NULL, "runtime_create_with_options returned Runtime for GLFW live-callback unload test");
    TEST_ASSERT(rt && !runtime_has_error(rt), "GLFW live-callback unload runtime loaded without error");
    if (!rt || runtime_has_error(rt)) {
        if (rt) runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    lib = load_dynamic_library(extension_path);
    TEST_ASSERT(lib != NULL, "GLFW extension library loaded for live-callback unload test exports");
    if (!lib) {
        runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    emit_scroll = (GlfwTestEmitScrollLastWindowFn)load_dynamic_symbol(lib, "tablo_glfw_test_emit_scroll_last_window");
    can_post = (GlfwTestGetLastWindowCanPostCallbacksFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_last_window_can_post_callbacks");
    is_initialized = (GlfwTestIsInitializedFn)load_dynamic_symbol(lib, "tablo_glfw_test_is_initialized");
    get_live_window_count = (GlfwTestGetLiveWindowCountFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_live_window_count");

    TEST_ASSERT(emit_scroll != NULL, "Live-callback unload test emit export located");
    TEST_ASSERT(can_post != NULL, "Live-callback unload test can-post export located");
    TEST_ASSERT(is_initialized != NULL, "Live-callback unload test init-state export located");
    TEST_ASSERT(get_live_window_count != NULL, "Live-callback unload test live-window export located");
    if (!emit_scroll || !can_post || !is_initialized || !get_live_window_count) {
        unload_dynamic_library(lib);
        runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    TEST_ASSERT(runtime_run(rt) == 0, "GLFW live-callback unload runtime run succeeded");
    TEST_ASSERT(!runtime_has_error(rt), "GLFW live-callback unload setup finished without runtime error");
    TEST_ASSERT(runtime_has_posted_callbacks(rt), "GLFW live-callback unload test leaves one posted callback pending");
    TEST_ASSERT(is_initialized() == 1, "GLFW remains initialized while the live-callback unload runtime is still active");
    TEST_ASSERT(get_live_window_count() == 1, "GLFW live-callback unload test keeps one live window before runtime_free");
    TEST_ASSERT(can_post() == 1, "GLFW live-callback unload test still reports callback posting before runtime_free");

    runtime_free(rt);
    rt = NULL;

    TEST_ASSERT(is_initialized() == 0, "GLFW unload hook terminates GLFW during runtime_free");
    TEST_ASSERT(get_live_window_count() == 0, "GLFW unload hook destroys live windows during runtime_free");
    TEST_ASSERT(can_post() == 0, "GLFW unload hook clears callback posting after runtime_free");
    TEST_ASSERT(emit_scroll(31.0, 32.0) == 0, "GLFW test emit fails after runtime_free unloads the live callback target");

    unload_dynamic_library(lib);
    cleanup_temp(source_path);
    free(extension_path);
}

static void test_glfw_extension_unload_with_live_window_callbacks(void) {
    Runtime* rt = NULL;
    RuntimeOptions options = {0};
    char* extension_path = locate_glfw_extension_library();
    char* source_path = NULL;
    DynamicLibraryHandle lib = NULL;
    GlfwTestEmitWindowResizeLastWindowFn emit_window_resize = NULL;
    GlfwTestGetLastWindowCanPostCallbacksFn can_post = NULL;
    GlfwTestIsInitializedFn is_initialized = NULL;
    GlfwTestGetLiveWindowCountFn get_live_window_count = NULL;
    const char* extension_paths[1];

    printf("Testing GLFW extension unload with live window callbacks...\n");

    TEST_ASSERT(extension_path != NULL, "GLFW extension library located for live-window unload test");
    if (!extension_path) {
        return;
    }

    source_path = write_temp_vml(glfw_live_window_callback_unload_source());
    TEST_ASSERT(source_path != NULL, "Temp TabloLang source created for GLFW live-window unload test");
    if (!source_path) {
        free(extension_path);
        return;
    }

    extension_paths[0] = extension_path;
    options.extension_paths = extension_paths;
    options.extension_path_count = 1;

    rt = runtime_create_with_options(source_path, options);
    TEST_ASSERT(rt != NULL, "runtime_create_with_options returned Runtime for GLFW live-window unload test");
    TEST_ASSERT(rt && !runtime_has_error(rt), "GLFW live-window unload runtime loaded without error");
    if (!rt || runtime_has_error(rt)) {
        if (rt) runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    lib = load_dynamic_library(extension_path);
    TEST_ASSERT(lib != NULL, "GLFW extension library loaded for live-window unload test exports");
    if (!lib) {
        runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    emit_window_resize = (GlfwTestEmitWindowResizeLastWindowFn)load_dynamic_symbol(lib, "tablo_glfw_test_emit_window_resize_last_window");
    can_post = (GlfwTestGetLastWindowCanPostCallbacksFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_last_window_can_post_callbacks");
    is_initialized = (GlfwTestIsInitializedFn)load_dynamic_symbol(lib, "tablo_glfw_test_is_initialized");
    get_live_window_count = (GlfwTestGetLiveWindowCountFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_live_window_count");

    TEST_ASSERT(emit_window_resize != NULL, "Live-window unload test emit export located");
    TEST_ASSERT(can_post != NULL, "Live-window unload test can-post export located");
    TEST_ASSERT(is_initialized != NULL, "Live-window unload test init-state export located");
    TEST_ASSERT(get_live_window_count != NULL, "Live-window unload test live-window export located");
    if (!emit_window_resize || !can_post || !is_initialized || !get_live_window_count) {
        unload_dynamic_library(lib);
        runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    TEST_ASSERT(runtime_run(rt) == 0, "GLFW live-window unload runtime run succeeded");
    TEST_ASSERT(!runtime_has_error(rt), "GLFW live-window unload setup finished without runtime error");
    TEST_ASSERT(!runtime_has_posted_callbacks(rt), "GLFW live-window unload setup starts without queued callbacks");
    TEST_ASSERT(is_initialized() == 1, "GLFW remains initialized while the live-window unload runtime is still active");
    TEST_ASSERT(get_live_window_count() == 1, "GLFW live-window unload test keeps one live window before runtime_free");
    TEST_ASSERT(can_post() == 1, "GLFW live-window unload test reports window callback posting before runtime_free");

    TEST_ASSERT(emit_window_resize(123, 77) == 1, "GLFW live-window unload test queues a window resize callback");
    TEST_ASSERT(runtime_has_posted_callbacks(rt), "GLFW live-window unload test leaves one posted window callback pending");

    runtime_free(rt);
    rt = NULL;

    TEST_ASSERT(is_initialized() == 0, "GLFW unload hook terminates GLFW during runtime_free for window callbacks");
    TEST_ASSERT(get_live_window_count() == 0, "GLFW unload hook destroys live windows during runtime_free for window callbacks");
    TEST_ASSERT(can_post() == 0, "GLFW unload hook clears window callback posting after runtime_free");
    TEST_ASSERT(emit_window_resize(124, 78) == 0, "GLFW window resize emit fails after runtime_free unloads the live window callback target");

    unload_dynamic_library(lib);
    cleanup_temp(source_path);
    free(extension_path);
}

static void test_glfw_extension_mixed_callback_burst_diagnostics(void) {
    Runtime* rt = NULL;
    RuntimeOptions options = {0};
    char* extension_path = locate_glfw_extension_library();
    char* source_path = NULL;
    DynamicLibraryHandle lib = NULL;
    GlfwTestEmitScrollLastWindowFn emit_scroll = NULL;
    GlfwTestEmitWindowResizeLastWindowFn emit_window_resize = NULL;
    GlfwTestClearLastWindowInputCallbackFn clear_input = NULL;
    GlfwTestGetLastWindowCanPostCallbacksFn can_post = NULL;
    GlfwTestGetLastWindowPendingCallbackCountFn get_pending = NULL;
    GlfwTestGetLastWindowDroppedCallbackCountFn get_dropped = NULL;
    GlfwTestResetLastWindowDroppedCallbackCountFn reset_dropped = NULL;
    GlfwTestGetLastWindowInvalidatedCallbackCountFn get_invalidated = NULL;
    GlfwTestResetLastWindowInvalidatedCallbackCountFn reset_invalidated = NULL;
    GlfwTestGetLastWindowRejectedCallbackCountFn get_rejected = NULL;
    GlfwTestResetLastWindowRejectedCallbackCountFn reset_rejected = NULL;
    const char* extension_paths[1];

    printf("Testing GLFW mixed callback burst diagnostics...\n");

    TEST_ASSERT(extension_path != NULL, "GLFW extension library located for mixed callback burst test");
    if (!extension_path) {
        return;
    }

    source_path = write_temp_vml(glfw_mixed_callback_burst_source());
    TEST_ASSERT(source_path != NULL, "Temp TabloLang source created for GLFW mixed callback burst test");
    if (!source_path) {
        free(extension_path);
        return;
    }

    extension_paths[0] = extension_path;
    options.extension_paths = extension_paths;
    options.extension_path_count = 1;

    rt = runtime_create_with_options(source_path, options);
    TEST_ASSERT(rt != NULL, "runtime_create_with_options returned Runtime for GLFW mixed callback burst test");
    TEST_ASSERT(rt && !runtime_has_error(rt), "GLFW mixed callback burst runtime loaded without error");
    if (!rt || runtime_has_error(rt)) {
        if (rt) runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    lib = load_dynamic_library(extension_path);
    TEST_ASSERT(lib != NULL, "GLFW extension library loaded for mixed callback burst test exports");
    if (!lib) {
        runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    emit_scroll = (GlfwTestEmitScrollLastWindowFn)load_dynamic_symbol(lib, "tablo_glfw_test_emit_scroll_last_window");
    emit_window_resize = (GlfwTestEmitWindowResizeLastWindowFn)load_dynamic_symbol(lib, "tablo_glfw_test_emit_window_resize_last_window");
    clear_input = (GlfwTestClearLastWindowInputCallbackFn)load_dynamic_symbol(lib, "tablo_glfw_test_clear_last_window_input_callback");
    can_post = (GlfwTestGetLastWindowCanPostCallbacksFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_last_window_can_post_callbacks");
    get_pending = (GlfwTestGetLastWindowPendingCallbackCountFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_last_window_pending_callback_count");
    get_dropped = (GlfwTestGetLastWindowDroppedCallbackCountFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_last_window_dropped_callback_count");
    reset_dropped = (GlfwTestResetLastWindowDroppedCallbackCountFn)load_dynamic_symbol(lib, "tablo_glfw_test_reset_last_window_dropped_callback_count");
    get_invalidated = (GlfwTestGetLastWindowInvalidatedCallbackCountFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_last_window_invalidated_callback_count");
    reset_invalidated = (GlfwTestResetLastWindowInvalidatedCallbackCountFn)load_dynamic_symbol(lib, "tablo_glfw_test_reset_last_window_invalidated_callback_count");
    get_rejected = (GlfwTestGetLastWindowRejectedCallbackCountFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_last_window_rejected_callback_count");
    reset_rejected = (GlfwTestResetLastWindowRejectedCallbackCountFn)load_dynamic_symbol(lib, "tablo_glfw_test_reset_last_window_rejected_callback_count");

    TEST_ASSERT(emit_scroll != NULL, "Mixed callback burst scroll export located");
    TEST_ASSERT(emit_window_resize != NULL, "Mixed callback burst window export located");
    TEST_ASSERT(clear_input != NULL, "Mixed callback burst clear-input export located");
    TEST_ASSERT(can_post != NULL, "Mixed callback burst can-post export located");
    TEST_ASSERT(get_pending != NULL, "Mixed callback burst pending-count export located");
    TEST_ASSERT(get_dropped != NULL, "Mixed callback burst dropped-count export located");
    TEST_ASSERT(reset_dropped != NULL, "Mixed callback burst dropped-reset export located");
    TEST_ASSERT(get_invalidated != NULL, "Mixed callback burst invalidated-count export located");
    TEST_ASSERT(reset_invalidated != NULL, "Mixed callback burst invalidated-reset export located");
    TEST_ASSERT(get_rejected != NULL, "Mixed callback burst rejected-count export located");
    TEST_ASSERT(reset_rejected != NULL, "Mixed callback burst rejected-reset export located");
    if (!emit_scroll || !emit_window_resize || !clear_input || !can_post || !get_pending ||
        !get_dropped || !reset_dropped || !get_invalidated || !reset_invalidated ||
        !get_rejected || !reset_rejected) {
        unload_dynamic_library(lib);
        runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    TEST_ASSERT(runtime_run(rt) == 0, "GLFW mixed callback burst runtime run succeeded");
    TEST_ASSERT(!runtime_has_error(rt), "GLFW mixed callback burst setup finished without runtime error");
    TEST_ASSERT(can_post() == 1, "GLFW mixed callback burst starts postable");
    TEST_ASSERT(get_pending() == 0, "GLFW mixed callback burst pending count starts at zero");
    TEST_ASSERT(reset_dropped() == 0, "GLFW mixed callback burst dropped counter starts at zero");
    TEST_ASSERT(reset_invalidated() == 0, "GLFW mixed callback burst invalidated counter starts at zero");
    TEST_ASSERT(reset_rejected() == 0, "GLFW mixed callback burst rejected counter starts at zero");

    TEST_ASSERT(emit_scroll(41.0, 42.0) == 1, "GLFW mixed callback burst queues the first input event");
    TEST_ASSERT(get_pending() == 1, "GLFW mixed callback burst pending count includes the queued input event");
    TEST_ASSERT(emit_window_resize(333, 222) == 1, "GLFW mixed callback burst accepts a second window emit even when it will be dropped");
    TEST_ASSERT(get_pending() == 1, "GLFW mixed callback burst pending count stays capped across callback families");
    TEST_ASSERT(get_dropped() == 1, "GLFW mixed callback burst drop counter increments for the second family event");

    TEST_ASSERT(clear_input() == 1, "GLFW mixed callback burst can clear the input callback while work is pending");
    TEST_ASSERT(can_post() == 1, "GLFW mixed callback burst still reports postable through the window callback after input clear");
    TEST_ASSERT(runtime_drain_posted_callbacks(rt, 8) == 1, "GLFW mixed callback burst drains one queued callback");
    TEST_ASSERT(get_pending() == 0, "GLFW mixed callback burst pending count resets after drain");
    TEST_ASSERT(get_invalidated() == 1, "GLFW mixed callback burst invalidated counter increments for the cleared queued input event");
    TEST_ASSERT(reset_invalidated() == 1, "GLFW mixed callback burst invalidated reset returns the cleared event count");
    TEST_ASSERT(reset_dropped() == 1, "GLFW mixed callback burst dropped reset returns the dropped cross-family event count");
    TEST_ASSERT(get_rejected() == 0, "GLFW mixed callback burst rejected count stays zero before queue close");

    TEST_ASSERT(runtime_close_posted_callback_queue(rt), "GLFW mixed callback burst can close the posted callback queue");
    TEST_ASSERT(emit_window_resize(334, 223) == 0, "GLFW mixed callback burst window emit fails after queue close");
    TEST_ASSERT(can_post() == 0, "GLFW mixed callback burst reports non-postable after queue close");
    TEST_ASSERT(get_rejected() == 1, "GLFW mixed callback burst rejected counter increments after queue close");
    TEST_ASSERT(reset_rejected() == 1, "GLFW mixed callback burst rejected reset returns the queue-closed event count");

    runtime_free(rt);
    unload_dynamic_library(lib);
    cleanup_temp(source_path);
    free(extension_path);
}

static void test_glfw_extension_multi_window_queue_isolation(void) {
    Runtime* rt = NULL;
    RuntimeOptions options = {0};
    char* extension_path = locate_glfw_extension_library();
    char* source_path = NULL;
    DynamicLibraryHandle lib = NULL;
    GlfwTestEmitScrollLiveWindowAtFn emit_scroll_at = NULL;
    GlfwTestEmitWindowResizeLiveWindowAtFn emit_window_resize_at = NULL;
    GlfwTestClearInputCallbackLiveWindowAtFn clear_input_at = NULL;
    GlfwTestGetLiveWindowCanPostCallbacksAtFn can_post_at = NULL;
    GlfwTestGetLiveWindowPendingCallbackCountAtFn get_pending_at = NULL;
    GlfwTestGetLiveWindowDroppedCallbackCountAtFn get_dropped_at = NULL;
    GlfwTestResetLiveWindowDroppedCallbackCountAtFn reset_dropped_at = NULL;
    GlfwTestGetLiveWindowInvalidatedCallbackCountAtFn get_invalidated_at = NULL;
    GlfwTestResetLiveWindowInvalidatedCallbackCountAtFn reset_invalidated_at = NULL;
    GlfwTestGetLiveWindowRejectedCallbackCountAtFn get_rejected_at = NULL;
    GlfwTestResetLiveWindowRejectedCallbackCountAtFn reset_rejected_at = NULL;
    GlfwTestGetLiveWindowCountFn get_live_window_count = NULL;
    const char* extension_paths[1];

    printf("Testing GLFW multi-window queue isolation...\n");

    TEST_ASSERT(extension_path != NULL, "GLFW extension library located for multi-window isolation test");
    if (!extension_path) {
        return;
    }

    source_path = write_temp_vml(glfw_multi_window_isolation_source());
    TEST_ASSERT(source_path != NULL, "Temp TabloLang source created for GLFW multi-window isolation test");
    if (!source_path) {
        free(extension_path);
        return;
    }

    extension_paths[0] = extension_path;
    options.extension_paths = extension_paths;
    options.extension_path_count = 1;

    rt = runtime_create_with_options(source_path, options);
    TEST_ASSERT(rt != NULL, "runtime_create_with_options returned Runtime for GLFW multi-window isolation test");
    TEST_ASSERT(rt && !runtime_has_error(rt), "GLFW multi-window isolation runtime loaded without error");
    if (!rt || runtime_has_error(rt)) {
        if (rt) runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    lib = load_dynamic_library(extension_path);
    TEST_ASSERT(lib != NULL, "GLFW extension library loaded for multi-window isolation test exports");
    if (!lib) {
        runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    emit_scroll_at = (GlfwTestEmitScrollLiveWindowAtFn)load_dynamic_symbol(lib, "tablo_glfw_test_emit_scroll_live_window_at");
    emit_window_resize_at = (GlfwTestEmitWindowResizeLiveWindowAtFn)load_dynamic_symbol(lib, "tablo_glfw_test_emit_window_resize_live_window_at");
    clear_input_at = (GlfwTestClearInputCallbackLiveWindowAtFn)load_dynamic_symbol(lib, "tablo_glfw_test_clear_input_callback_live_window_at");
    can_post_at = (GlfwTestGetLiveWindowCanPostCallbacksAtFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_live_window_can_post_callbacks_at");
    get_pending_at = (GlfwTestGetLiveWindowPendingCallbackCountAtFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_live_window_pending_callback_count_at");
    get_dropped_at = (GlfwTestGetLiveWindowDroppedCallbackCountAtFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_live_window_dropped_callback_count_at");
    reset_dropped_at = (GlfwTestResetLiveWindowDroppedCallbackCountAtFn)load_dynamic_symbol(lib, "tablo_glfw_test_reset_live_window_dropped_callback_count_at");
    get_invalidated_at = (GlfwTestGetLiveWindowInvalidatedCallbackCountAtFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_live_window_invalidated_callback_count_at");
    reset_invalidated_at = (GlfwTestResetLiveWindowInvalidatedCallbackCountAtFn)load_dynamic_symbol(lib, "tablo_glfw_test_reset_live_window_invalidated_callback_count_at");
    get_rejected_at = (GlfwTestGetLiveWindowRejectedCallbackCountAtFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_live_window_rejected_callback_count_at");
    reset_rejected_at = (GlfwTestResetLiveWindowRejectedCallbackCountAtFn)load_dynamic_symbol(lib, "tablo_glfw_test_reset_live_window_rejected_callback_count_at");
    get_live_window_count = (GlfwTestGetLiveWindowCountFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_live_window_count");

    TEST_ASSERT(emit_scroll_at != NULL, "Multi-window isolation scroll export located");
    TEST_ASSERT(emit_window_resize_at != NULL, "Multi-window isolation window export located");
    TEST_ASSERT(clear_input_at != NULL, "Multi-window isolation clear-input export located");
    TEST_ASSERT(can_post_at != NULL, "Multi-window isolation can-post export located");
    TEST_ASSERT(get_pending_at != NULL, "Multi-window isolation pending export located");
    TEST_ASSERT(get_dropped_at != NULL, "Multi-window isolation dropped export located");
    TEST_ASSERT(reset_dropped_at != NULL, "Multi-window isolation dropped-reset export located");
    TEST_ASSERT(get_invalidated_at != NULL, "Multi-window isolation invalidated export located");
    TEST_ASSERT(reset_invalidated_at != NULL, "Multi-window isolation invalidated-reset export located");
    TEST_ASSERT(get_rejected_at != NULL, "Multi-window isolation rejected export located");
    TEST_ASSERT(reset_rejected_at != NULL, "Multi-window isolation rejected-reset export located");
    TEST_ASSERT(get_live_window_count != NULL, "Multi-window isolation live-window export located");
    if (!emit_scroll_at || !emit_window_resize_at || !clear_input_at || !can_post_at || !get_pending_at ||
        !get_dropped_at || !reset_dropped_at || !get_invalidated_at || !reset_invalidated_at ||
        !get_rejected_at || !reset_rejected_at || !get_live_window_count) {
        unload_dynamic_library(lib);
        runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    TEST_ASSERT(runtime_run(rt) == 0, "GLFW multi-window isolation runtime run succeeded");
    TEST_ASSERT(!runtime_has_error(rt), "GLFW multi-window isolation setup finished without runtime error");
    TEST_ASSERT(get_live_window_count() == 2, "GLFW multi-window isolation starts with two live windows");

    TEST_ASSERT(reset_dropped_at(0) == 0 && reset_dropped_at(1) == 0, "GLFW multi-window isolation dropped counters start at zero");
    TEST_ASSERT(reset_invalidated_at(0) == 0 && reset_invalidated_at(1) == 0, "GLFW multi-window isolation invalidated counters start at zero");
    TEST_ASSERT(reset_rejected_at(0) == 0 && reset_rejected_at(1) == 0, "GLFW multi-window isolation rejected counters start at zero");
    TEST_ASSERT(get_pending_at(0) == 0 && get_pending_at(1) == 0, "GLFW multi-window isolation pending counts start at zero");

    TEST_ASSERT(emit_scroll_at(1, 51.0, 52.0) == 1, "GLFW multi-window isolation queues an input event for one window");
    TEST_ASSERT(get_pending_at(1) == 1, "GLFW multi-window isolation pending count updates for the input window");
    TEST_ASSERT(get_pending_at(0) == 0, "GLFW multi-window isolation pending count stays zero for the other window");
    TEST_ASSERT(emit_window_resize_at(0, 401, 301) == 1, "GLFW multi-window isolation queues a window event for the other window");
    TEST_ASSERT(get_pending_at(0) == 1, "GLFW multi-window isolation pending count updates for the window-callback window");
    TEST_ASSERT(get_pending_at(1) == 1, "GLFW multi-window isolation preserves the input-window pending count");

    TEST_ASSERT(emit_scroll_at(1, 53.0, 54.0) == 1, "GLFW multi-window isolation accepts a second input emit even when it will be dropped");
    TEST_ASSERT(get_dropped_at(1) == 1, "GLFW multi-window isolation drops are isolated to the input window");
    TEST_ASSERT(get_dropped_at(0) == 0, "GLFW multi-window isolation does not leak drops to the window-callback window");

    TEST_ASSERT(clear_input_at(1) == 1, "GLFW multi-window isolation can clear the input callback on one window");
    TEST_ASSERT(runtime_drain_posted_callbacks(rt, 8) == 2, "GLFW multi-window isolation drains the two queued callbacks");
    TEST_ASSERT(get_invalidated_at(1) == 1, "GLFW multi-window isolation invalidation is isolated to the cleared input window");
    TEST_ASSERT(get_invalidated_at(0) == 0, "GLFW multi-window isolation leaves the window-callback window invalidation count untouched");
    TEST_ASSERT(get_pending_at(0) == 0 && get_pending_at(1) == 0, "GLFW multi-window isolation pending counts reset after drain");

    TEST_ASSERT(runtime_close_posted_callback_queue(rt), "GLFW multi-window isolation can close the posted callback queue");
    TEST_ASSERT(emit_window_resize_at(0, 402, 302) == 0, "GLFW multi-window isolation window emit fails after queue close");
    TEST_ASSERT(get_rejected_at(0) == 1, "GLFW multi-window isolation rejection is attributed to the window-callback window");
    TEST_ASSERT(get_rejected_at(1) == 0, "GLFW multi-window isolation does not leak rejection counts to the input window");

    runtime_free(rt);
    unload_dynamic_library(lib);
    cleanup_temp(source_path);
    free(extension_path);
}

static void test_glfw_extension_query_helpers(void) {
    Runtime* rt = NULL;
    RuntimeOptions options = {0};
    char* extension_path = locate_glfw_extension_library();
    char* source_path = NULL;
    const char* extension_paths[1];

    printf("Testing GLFW query helpers...\n");

    TEST_ASSERT(extension_path != NULL, "GLFW extension library located for query helper test");
    if (!extension_path) {
        return;
    }

    source_path = write_temp_vml(glfw_query_helpers_source());
    TEST_ASSERT(source_path != NULL, "Temp TabloLang source created for GLFW query helper test");
    if (!source_path) {
        free(extension_path);
        return;
    }

    extension_paths[0] = extension_path;
    options.extension_paths = extension_paths;
    options.extension_path_count = 1;

    rt = runtime_create_with_options(source_path, options);
    TEST_ASSERT(rt != NULL, "runtime_create_with_options returned Runtime for GLFW query helper test");
    TEST_ASSERT(rt && !runtime_has_error(rt), "GLFW query helper runtime loaded without error");
    if (!rt || runtime_has_error(rt)) {
        if (rt) runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    TEST_ASSERT(runtime_run(rt) == 0, "GLFW query helper runtime run succeeded");
    TEST_ASSERT(!runtime_has_error(rt), "GLFW query helper runtime finished without error");

    runtime_free(rt);
    cleanup_temp(source_path);
    free(extension_path);
}

static void test_glfw_extension_queue_close_rejections(void) {
    Runtime* rt = NULL;
    RuntimeOptions options = {0};
    char* extension_path = locate_glfw_extension_library();
    char* source_path = NULL;
    DynamicLibraryHandle lib = NULL;
    GlfwTestEmitScrollLastWindowFn emit_scroll = NULL;
    GlfwTestGetLastWindowCanPostCallbacksFn can_post = NULL;
    GlfwTestGetLastWindowRejectedCallbackCountFn get_rejected = NULL;
    GlfwTestResetLastWindowRejectedCallbackCountFn reset_rejected = NULL;
    const char* extension_paths[1];

    printf("Testing GLFW extension queue-close rejection path...\n");

    TEST_ASSERT(extension_path != NULL, "GLFW extension library located");
    if (!extension_path) {
        return;
    }

    source_path = write_temp_vml(glfw_queue_close_source());
    TEST_ASSERT(source_path != NULL, "Temp TabloLang source created for GLFW host rejection test");
    if (!source_path) {
        free(extension_path);
        return;
    }

    extension_paths[0] = extension_path;
    options.extension_paths = extension_paths;
    options.extension_path_count = 1;

    rt = runtime_create_with_options(source_path, options);
    TEST_ASSERT(rt != NULL, "runtime_create_with_options returned Runtime for GLFW host rejection test");
    TEST_ASSERT(rt && !runtime_has_error(rt), "GLFW host rejection runtime loaded without error");
    if (!rt || runtime_has_error(rt)) {
        if (rt) runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    lib = load_dynamic_library(extension_path);
    TEST_ASSERT(lib != NULL, "GLFW extension library loaded for host-side test exports");
    if (!lib) {
        runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    emit_scroll = (GlfwTestEmitScrollLastWindowFn)load_dynamic_symbol(lib, "tablo_glfw_test_emit_scroll_last_window");
    can_post = (GlfwTestGetLastWindowCanPostCallbacksFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_last_window_can_post_callbacks");
    get_rejected = (GlfwTestGetLastWindowRejectedCallbackCountFn)load_dynamic_symbol(lib, "tablo_glfw_test_get_last_window_rejected_callback_count");
    reset_rejected = (GlfwTestResetLastWindowRejectedCallbackCountFn)load_dynamic_symbol(lib, "tablo_glfw_test_reset_last_window_rejected_callback_count");

    TEST_ASSERT(emit_scroll != NULL, "Host test emit export located");
    TEST_ASSERT(can_post != NULL, "Host test can-post export located");
    TEST_ASSERT(get_rejected != NULL, "Host test rejected-count export located");
    TEST_ASSERT(reset_rejected != NULL, "Host test rejected-reset export located");
    if (!emit_scroll || !can_post || !get_rejected || !reset_rejected) {
        unload_dynamic_library(lib);
        runtime_free(rt);
        cleanup_temp(source_path);
        free(extension_path);
        return;
    }

    TEST_ASSERT(runtime_run(rt) == 0, "GLFW host rejection setup runtime run succeeded");
    TEST_ASSERT(!runtime_has_error(rt), "GLFW host rejection setup finished without runtime error");
    TEST_ASSERT(runtime_is_posted_callback_queue_open(rt), "Runtime posted callback queue starts open");
    TEST_ASSERT(can_post() == 1, "GLFW host test callback reports postable before queue close");
    TEST_ASSERT(reset_rejected() == 0, "GLFW host test rejected counter starts at zero");

    TEST_ASSERT(runtime_close_posted_callback_queue(rt), "Runtime posted callback queue closes explicitly");
    TEST_ASSERT(!runtime_is_posted_callback_queue_open(rt), "Runtime posted callback queue reports closed after explicit close");
    TEST_ASSERT(!runtime_has_posted_callbacks(rt), "Runtime has no posted callbacks after explicit close");
    TEST_ASSERT(!runtime_close_posted_callback_queue(rt), "Closing the posted callback queue twice is a no-op");

    TEST_ASSERT(emit_scroll(11.0, 12.0) == 0, "GLFW test emit fails once the posted callback queue is closed");
    TEST_ASSERT(!runtime_has_error(rt), "Queue-closed GLFW emit does not poison the runtime");
    TEST_ASSERT(can_post() == 0, "GLFW host test callback reports non-postable after queue close");
    TEST_ASSERT(get_rejected() == 1, "GLFW rejected callback counter increments on queue-closed emit");
    TEST_ASSERT(reset_rejected() == 1, "GLFW rejected callback reset returns the queued-close rejection count");
    TEST_ASSERT(get_rejected() == 0, "GLFW rejected callback counter resets to zero");

    runtime_free(rt);
    unload_dynamic_library(lib);
    cleanup_temp(source_path);
    free(extension_path);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("Running TabloLang GLFW Extension Host Tests...\n\n");

    test_glfw_extension_non_main_callback_registration();
    test_glfw_extension_unload_with_live_callbacks();
    test_glfw_extension_unload_with_live_window_callbacks();
    test_glfw_extension_mixed_callback_burst_diagnostics();
    test_glfw_extension_multi_window_queue_isolation();
    test_glfw_extension_query_helpers();
    test_glfw_extension_queue_close_rejections();

    printf("\nGLFW Extension Host Test Results:\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
