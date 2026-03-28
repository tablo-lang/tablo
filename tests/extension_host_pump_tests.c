#include "runtime.h"
#include "artifact.h"
#include "safe_alloc.h"
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
    snprintf(path, 512, "%s\\tablo_host_pump_%llu%s", temp_dir, nonce, ext ? ext : "");
#else
    snprintf(path, 512, "%s/tablo_host_pump_%llu%s", temp_dir, nonce, ext ? ext : "");
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

static char* locate_test_extension_library(void) {
#ifdef _WIN32
    static const char* candidates[] = {
        "..\\build-tablo\\Release\\tablo_test_extension.dll",
        "..\\build-tablo\\Debug\\tablo_test_extension.dll",
        "..\\build-tablo\\tablo_test_extension.dll",
        "..\\build\\Release\\tablo_test_extension.dll",
        "..\\build\\Debug\\tablo_test_extension.dll",
        "..\\build\\tablo_test_extension.dll",
        "..\\build-nan\\Release\\tablo_test_extension.dll",
        "..\\build-nan\\Debug\\tablo_test_extension.dll",
        "..\\build-nan\\tablo_test_extension.dll",
        "tablo_test_extension.dll",
        "..\\Release\\tablo_test_extension.dll",
        "..\\Debug\\tablo_test_extension.dll"
    };
#elif __APPLE__
    static const char* candidates[] = {
        "../build-tablo/Release/tablo_test_extension.dylib",
        "../build-tablo/Debug/tablo_test_extension.dylib",
        "../build-tablo/tablo_test_extension.dylib",
        "../build/Release/tablo_test_extension.dylib",
        "../build/Debug/tablo_test_extension.dylib",
        "../build/tablo_test_extension.dylib",
        "../build-nan/Release/tablo_test_extension.dylib",
        "../build-nan/Debug/tablo_test_extension.dylib",
        "../build-nan/tablo_test_extension.dylib",
        "tablo_test_extension.dylib",
        "../Release/tablo_test_extension.dylib",
        "../Debug/tablo_test_extension.dylib"
    };
#else
    static const char* candidates[] = {
        "../build-tablo/Release/tablo_test_extension.so",
        "../build-tablo/Debug/tablo_test_extension.so",
        "../build-tablo/tablo_test_extension.so",
        "../build/Release/tablo_test_extension.so",
        "../build/Debug/tablo_test_extension.so",
        "../build/tablo_test_extension.so",
        "../build-nan/Release/tablo_test_extension.so",
        "../build-nan/Debug/tablo_test_extension.so",
        "../build-nan/tablo_test_extension.so",
        "tablo_test_extension.so",
        "../Release/tablo_test_extension.so",
        "../Debug/tablo_test_extension.so"
    };
#endif

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (file_exists(candidates[i])) {
            return safe_strdup(candidates[i]);
        }
    }
    return NULL;
}

static int write_temp_artifact_direct(const char* source_path,
                                      RuntimeOptions options,
                                      char** out_artifact_path) {
    char* artifact_path = NULL;
    Runtime* rt = NULL;
    char error_buf[256];
    int main_index = -1;

    if (!source_path || !out_artifact_path) return 0;
    *out_artifact_path = NULL;

    artifact_path = make_temp_path_with_ext(".tbc");
    if (!artifact_path) return 0;
    remove(artifact_path);

    rt = runtime_create_with_options(source_path, options);
    if (!rt || runtime_has_error(rt) || runtime_get_load_mode(rt) != RUNTIME_LOAD_SOURCE) {
        if (rt) runtime_free(rt);
        cleanup_temp(artifact_path);
        return 0;
    }

    for (int i = 0; i < rt->function_count; i++) {
        if (rt->functions[i] == rt->main_function) {
            main_index = i;
            break;
        }
    }
    if (main_index < 0) {
        runtime_free(rt);
        cleanup_temp(artifact_path);
        return 0;
    }

    error_buf[0] = '\0';
    if (!artifact_write_file(artifact_path,
                             rt->init_function,
                             rt->functions,
                             rt->function_count,
                             main_index,
                             0,
                             NULL,
                             0,
                             rt->interface_dispatch_entries,
                             rt->interface_dispatch_count,
                             error_buf,
                             sizeof(error_buf)) ||
        !file_exists(artifact_path)) {
        if (error_buf[0] != '\0') {
            fprintf(stderr, "artifact write failed: %s\n", error_buf);
        }
        runtime_free(rt);
        cleanup_temp(artifact_path);
        return 0;
    }

    runtime_free(rt);
    *out_artifact_path = artifact_path;
    return 1;
}

static int runtime_run_function_and_take_int(Runtime* rt,
                                             const char* function_name,
                                             int* out_value) {
    Value out;

    if (!rt || !function_name || !out_value) return 0;
    if (runtime_run_function(rt, function_name) != 0) {
        fprintf(stderr, "%s: runtime_run_function failed: %s\n",
                function_name,
                runtime_get_error(rt) ? runtime_get_error(rt) : "(no runtime error)");
        return 0;
    }
    if (runtime_has_error(rt)) {
        fprintf(stderr, "%s: runtime reported error: %s\n",
                function_name,
                runtime_get_error(rt) ? runtime_get_error(rt) : "(no runtime error)");
        return 0;
    }
    if (!runtime_take_return_value(rt, &out)) {
        fprintf(stderr, "%s: return value was not available\n", function_name);
        return 0;
    }
    if (value_get_type(&out) != VAL_INT) {
        fprintf(stderr, "%s: return value had unexpected type %d\n",
                function_name,
                (int)value_get_type(&out));
        value_free(&out);
        return 0;
    }

    *out_value = (int)value_get_int(&out);
    value_free(&out);
    return 1;
}

static const char* extension_host_callback_pump_source(void) {
    return
        "var hostCounter: Counter = counterNew(100);\n"
        "var hostInputHits: int = 0;\n"
        "var hostWindowHits: int = 0;\n"
        "var hostFrameHits: int = 0;\n"
        "var hostInputBatchHits: int = 0;\n"
        "var hostWindowBatchHits: int = 0;\n"
        "var hostFrameBatchHits: int = 0;\n"
        "var hostMixedBatchHits: int = 0;\n"
        "var hostCustomInputHits: int = 0;\n"
        "var hostCustomMixedBatchHits: int = 0;\n"
        "var hostCustomMixedInputHits: int = 0;\n"
        "var hostCustomMixedWindowHits: int = 0;\n"
        "var hostCustomMixedFrameHits: int = 0;\n"
        "var hostExtraInputHits: int = 0;\n"
        "var hostExtraMixedBatchHits: int = 0;\n"
        "var hostStructuredInputHits: int = 0;\n"
        "var hostStructuredMixedBatchHits: int = 0;\n"
        "var hostMappedInputHits: int = 0;\n"
        "var hostMappedMixedBatchHits: int = 0;\n"
        "var hostPresetWindowHits: int = 0;\n"
        "var hostPresetFrameHits: int = 0;\n"
        "var hostPresetInputBatchHits: int = 0;\n"
        "var hostPresetWindowBatchHits: int = 0;\n"
        "var hostPresetFrameBatchHits: int = 0;\n"
        "var hostPresetTypedMixedBatchHits: int = 0;\n"
        "var hostFrameEnvelopeBatchHits: int = 0;\n"
        "var hostFrameEnvelopeBuilderHits: int = 0;\n"
        "var hostFrameEnvelopeHeapBuilderHits: int = 0;\n"
        "var hostEventLoopSessionFrameHits: int = 0;\n"
        "var hostEventLoopSessionAltFrameHits: int = 0;\n"
        "var hostSessionTickInputHits: int = 0;\n"
        "var hostSessionSlowTickInputHits: int = 0;\n"
        "var delayedHostWorker: PostedIntCallbackWorker? = nil;\n"
        "func onHostInputEvent(event: map<string, any>): void {\n"
        "    if (extSummarizeInputEvent(event) == \"input:keyboard:32:true:input:1:2\") {\n"
        "        hostInputHits = hostInputHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostWindowEvent(event: map<string, any>): void {\n"
        "    if (extSummarizeWindowEvent(event) == \"window:resize:1280:720:true:window:4:2\") {\n"
        "        hostWindowHits = hostWindowHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostFrameEvent(event: map<string, any>): void {\n"
        "    if (extSummarizeFrameEvent(event) == \"frame:update:60:0.250:frame:5:2\") {\n"
        "        hostFrameHits = hostFrameHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostInputBatch(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 2 &&\n"
        "        extSummarizeInputEvent(events[0]) == \"input:keyboard:11:true:input:1:2\" &&\n"
        "        extSummarizeInputEvent(events[1]) == \"input:mouse:12:false:input:1:2\") {\n"
        "        hostInputBatchHits = hostInputBatchHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostWindowBatch(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 2 &&\n"
        "        extSummarizeWindowEvent(events[0]) == \"window:resize:800:600:true:window:4:2\" &&\n"
        "        extSummarizeWindowEvent(events[1]) == \"window:blur:800:600:false:window:4:2\") {\n"
        "        hostWindowBatchHits = hostWindowBatchHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostFrameBatch(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 2 &&\n"
        "        extSummarizeFrameEvent(events[0]) == \"frame:update:70:0.125:frame:5:2\" &&\n"
        "        extSummarizeFrameEvent(events[1]) == \"frame:render:71:0.250:frame:5:2\") {\n"
        "        hostFrameBatchHits = hostFrameBatchHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostMixedBatch(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 3 &&\n"
        "        extSummarizeInputEvent(events[0]) == \"input:gamepad:21:true:input:1:2\" &&\n"
        "        extSummarizeWindowEvent(events[1]) == \"window:focus:1024:768:true:window:4:2\" &&\n"
        "        extSummarizeFrameEvent(events[2]) == \"frame:present:88:0.500:frame:5:2\") {\n"
        "        hostMixedBatchHits = hostMixedBatchHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostCustomInputEvent(event: map<string, any>): void {\n"
        "    if (extSummarizeInputEvent(event) == \"input:pen:9:false:host-input:42:3\") {\n"
        "        hostCustomInputHits = hostCustomInputHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostCustomMixedBatch(events: array<map<string, any>>): void {\n"
        "    var ok_input = false;\n"
        "    var ok_window = false;\n"
        "    var ok_frame = false;\n"
        "    if (len(events) == 3) {\n"
        "        ok_input = extSummarizeInputEvent(events[0]) == \"input:touch:55:true:host-mixed-input:11:1\";\n"
        "        ok_window = extSummarizeWindowEvent(events[1]) == \"window:minimize:1440:900:false:host-mixed-window:12:3\";\n"
        "        ok_frame = extSummarizeFrameEvent(events[2]) == \"frame:physics:144:0.016:host-mixed-frame:13:2\";\n"
        "    }\n"
        "    if (ok_input) {\n"
        "        hostCustomMixedInputHits = hostCustomMixedInputHits + 1;\n"
        "    }\n"
        "    if (ok_window) {\n"
        "        hostCustomMixedWindowHits = hostCustomMixedWindowHits + 1;\n"
        "    }\n"
        "    if (ok_frame) {\n"
        "        hostCustomMixedFrameHits = hostCustomMixedFrameHits + 1;\n"
        "    }\n"
        "    if (ok_input && ok_window && ok_frame) {\n"
        "        hostCustomMixedBatchHits = hostCustomMixedBatchHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostExtraInputEvent(event: map<string, any>): void {\n"
        "    if (extSummarizeInputEvent(event) == \"input:stylus:77:true:input:1:2\" &&\n"
        "        (mapGet(event, \"playerId\") as int) == 7 &&\n"
        "        (mapGet(event, \"label\") as string) == \"aim\" &&\n"
        "        (mapGet(event, \"analog\") as double) == 0.75 &&\n"
        "        !(mapGet(event, \"consumed\") as bool)) {\n"
        "        hostExtraInputHits = hostExtraInputHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostExtraMixedBatch(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 3 &&\n"
        "        extSummarizeInputEvent(events[0]) == \"input:keyboard:91:false:input:1:2\" &&\n"
        "        extSummarizeWindowEvent(events[1]) == \"window:overlay:1600:900:true:window:4:2\" &&\n"
        "        extSummarizeFrameEvent(events[2]) == \"frame:tick:233:0.033:frame:5:2\" &&\n"
        "        (mapGet(events[0], \"slot\") as int) == 2 &&\n"
        "        (mapGet(events[1], \"scene\") as string) == \"pause-menu\" &&\n"
        "        (mapGet(events[2], \"fixed\") as bool) &&\n"
        "        (mapGet(events[2], \"alpha\") as double) == 0.5) {\n"
        "        hostExtraMixedBatchHits = hostExtraMixedBatchHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostStructuredInputEvent(event: map<string, any>): void {\n"
        "    var samples = mapGet(event, \"samples\") as array<int>;\n"
        "    var bounds = mapGet(event, \"bounds\") as (int, int);\n"
        "    if (extSummarizeInputEvent(event) == \"input:keyboard:101:true:input:1:2\" &&\n"
        "        len(samples) == 3 && samples[0] == 4 && samples[1] == 5 && samples[2] == 6 &&\n"
        "        bounds.0 == 64 && bounds.1 == 128) {\n"
        "        hostStructuredInputHits = hostStructuredInputHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostStructuredMixedBatch(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 3) {\n"
        "        var input_samples = mapGet(events[0], \"samples\") as array<int>;\n"
        "        var window_rect = mapGet(events[1], \"rect\") as (int, int);\n"
        "        var frame_markers = mapGet(events[2], \"markers\") as array<string>;\n"
        "        if (extSummarizeInputEvent(events[0]) == \"input:controller:5:true:input:1:2\" &&\n"
        "            len(input_samples) == 2 && input_samples[0] == 8 && input_samples[1] == 9 &&\n"
        "            extSummarizeWindowEvent(events[1]) == \"window:viewport:1600:900:true:window:4:2\" &&\n"
        "            window_rect.0 == 10 && window_rect.1 == 20 &&\n"
        "            extSummarizeFrameEvent(events[2]) == \"frame:step:301:0.008:frame:5:2\" &&\n"
        "            len(frame_markers) == 2 && frame_markers[0] == \"fixed\" && frame_markers[1] == \"net\") {\n"
        "            hostStructuredMixedBatchHits = hostStructuredMixedBatchHits + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func onHostMappedInputEvent(event: map<string, any>): void {\n"
        "    var payload = mapGet(event, \"payload\") as map<string, any>;\n"
        "    var context = mapGet(event, \"context\") as map<string, any>;\n"
        "    var coords = mapGet(payload, \"coords\") as (int, int);\n"
        "    var buttons = mapGet(payload, \"buttons\") as array<int>;\n"
        "    if (extSummarizeInputEvent(event) == \"input:tablet:44:true:input:1:2\" &&\n"
        "        (mapGet(payload, \"label\") as string) == \"ink\" &&\n"
        "        (mapGet(payload, \"active\") as bool) &&\n"
        "        (mapGet(payload, \"score\") as int) == 11 &&\n"
        "        (mapGet(context, \"owner\") as string) == \"host\" &&\n"
        "        (mapGet(context, \"frame\") as int) == 7 &&\n"
        "        coords.0 == 3 && coords.1 == 4 &&\n"
        "        len(buttons) == 2 && buttons[0] == 1 && buttons[1] == 2) {\n"
        "        hostMappedInputHits = hostMappedInputHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostMappedMixedBatch(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 3) {\n"
        "        var input_payload = mapGet(events[0], \"payload\") as map<string, any>;\n"
        "        var window_payload = mapGet(events[1], \"payload\") as map<string, any>;\n"
        "        var frame_payload = mapGet(events[2], \"payload\") as map<string, any>;\n"
        "        var combo = mapGet(input_payload, \"combo\") as array<string>;\n"
        "        var rect = mapGet(window_payload, \"rect\") as (int, int);\n"
        "        var markers = mapGet(frame_payload, \"markers\") as array<string>;\n"
        "        if (extSummarizeInputEvent(events[0]) == \"input:gamepad:88:false:input:1:2\" &&\n"
        "            (mapGet(input_payload, \"score\") as int) == 5 &&\n"
        "            len(combo) == 2 && combo[0] == \"a\" && combo[1] == \"b\" &&\n"
        "            extSummarizeWindowEvent(events[1]) == \"window:move:1920:1080:true:window:4:2\" &&\n"
        "            (mapGet(window_payload, \"title\") as string) == \"scene\" &&\n"
        "            rect.0 == 30 && rect.1 == 40 &&\n"
        "            extSummarizeFrameEvent(events[2]) == \"frame:simulate:410:0.017:frame:5:2\" &&\n"
        "            (mapGet(frame_payload, \"phase\") as string) == \"late\" &&\n"
        "            len(markers) == 2 && markers[0] == \"gpu\" && markers[1] == \"cpu\") {\n"
        "            hostMappedMixedBatchHits = hostMappedMixedBatchHits + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func onHostPresetWindowEvent(event: map<string, any>): void {\n"
        "    var payload = mapGet(event, \"payload\") as map<string, any>;\n"
        "    var context = mapGet(event, \"context\") as map<string, any>;\n"
        "    var rect = mapGet(payload, \"rect\") as (int, int);\n"
        "    if (extSummarizeWindowEvent(event) == \"window:resize:1280:720:false:window:4:2\" &&\n"
        "        (mapGet(payload, \"title\") as string) == \"editor\" &&\n"
        "        rect.0 == 1280 && rect.1 == 720 &&\n"
        "        (mapGet(context, \"owner\") as string) == \"ui\" &&\n"
        "        (mapGet(context, \"frame\") as int) == 99) {\n"
        "        hostPresetWindowHits = hostPresetWindowHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostPresetFrameEvent(event: map<string, any>): void {\n"
        "    var payload = mapGet(event, \"payload\") as map<string, any>;\n"
        "    var context = mapGet(event, \"context\") as map<string, any>;\n"
        "    var markers = mapGet(payload, \"markers\") as array<string>;\n"
        "    if (extSummarizeFrameEvent(event) == \"frame:render:77:0.025:frame:5:2\" &&\n"
        "        (mapGet(payload, \"phase\") as string) == \"post\" &&\n"
        "        len(markers) == 2 && markers[0] == \"gpu\" && markers[1] == \"ui\" &&\n"
        "        (mapGet(context, \"owner\") as string) == \"renderer\" &&\n"
        "        (mapGet(context, \"frame\") as int) == 77) {\n"
        "        hostPresetFrameHits = hostPresetFrameHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostPresetInputBatch(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 2) {\n"
        "        var payload0 = mapGet(events[0], \"payload\") as map<string, any>;\n"
        "        var payload1 = mapGet(events[1], \"payload\") as map<string, any>;\n"
        "        var context0 = mapGet(events[0], \"context\") as map<string, any>;\n"
        "        var context1 = mapGet(events[1], \"context\") as map<string, any>;\n"
        "        var coords0 = mapGet(payload0, \"coords\") as (int, int);\n"
        "        var coords1 = mapGet(payload1, \"coords\") as (int, int);\n"
        "        var buttons0 = mapGet(payload0, \"buttons\") as array<int>;\n"
        "        var buttons1 = mapGet(payload1, \"buttons\") as array<int>;\n"
        "        if (extSummarizeInputEvent(events[0]) == \"input:tablet:61:true:input:1:2\" &&\n"
        "            extSummarizeInputEvent(events[1]) == \"input:stylus:62:false:input:1:2\" &&\n"
        "            (mapGet(payload0, \"label\") as string) == \"drag\" &&\n"
        "            (mapGet(payload1, \"label\") as string) == \"hover\" &&\n"
        "            coords0.0 == 10 && coords0.1 == 20 && coords1.0 == 30 && coords1.1 == 40 &&\n"
        "            len(buttons0) == 1 && buttons0[0] == 1 &&\n"
        "            len(buttons1) == 2 && buttons1[0] == 2 && buttons1[1] == 3 &&\n"
        "            (mapGet(context0, \"owner\") as string) == \"ui\" && (mapGet(context0, \"frame\") as int) == 200 &&\n"
        "            (mapGet(context1, \"owner\") as string) == \"ui\" && (mapGet(context1, \"frame\") as int) == 201) {\n"
        "            hostPresetInputBatchHits = hostPresetInputBatchHits + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func onHostPresetWindowBatch(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 2) {\n"
        "        var payload0 = mapGet(events[0], \"payload\") as map<string, any>;\n"
        "        var payload1 = mapGet(events[1], \"payload\") as map<string, any>;\n"
        "        var context0 = mapGet(events[0], \"context\") as map<string, any>;\n"
        "        var context1 = mapGet(events[1], \"context\") as map<string, any>;\n"
        "        var rect0 = mapGet(payload0, \"rect\") as (int, int);\n"
        "        var rect1 = mapGet(payload1, \"rect\") as (int, int);\n"
        "        if (extSummarizeWindowEvent(events[0]) == \"window:resize:1440:900:true:window:4:2\" &&\n"
        "            extSummarizeWindowEvent(events[1]) == \"window:move:1600:900:true:window:4:2\" &&\n"
        "            (mapGet(payload0, \"title\") as string) == \"editor-left\" &&\n"
        "            (mapGet(payload1, \"title\") as string) == \"scene\" &&\n"
        "            rect0.0 == 1440 && rect0.1 == 900 && rect1.0 == 50 && rect1.1 == 60 &&\n"
        "            (mapGet(context0, \"owner\") as string) == \"window\" && (mapGet(context0, \"frame\") as int) == 300 &&\n"
        "            (mapGet(context1, \"owner\") as string) == \"window\" && (mapGet(context1, \"frame\") as int) == 301) {\n"
        "            hostPresetWindowBatchHits = hostPresetWindowBatchHits + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func onHostPresetFrameBatch(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 2) {\n"
        "        var payload0 = mapGet(events[0], \"payload\") as map<string, any>;\n"
        "        var payload1 = mapGet(events[1], \"payload\") as map<string, any>;\n"
        "        var context0 = mapGet(events[0], \"context\") as map<string, any>;\n"
        "        var context1 = mapGet(events[1], \"context\") as map<string, any>;\n"
        "        var markers0 = mapGet(payload0, \"markers\") as array<string>;\n"
        "        var markers1 = mapGet(payload1, \"markers\") as array<string>;\n"
        "        if (extSummarizeFrameEvent(events[0]) == \"frame:logic:500:0.010:frame:5:2\" &&\n"
        "            extSummarizeFrameEvent(events[1]) == \"frame:render:501:0.016:frame:5:2\" &&\n"
        "            (mapGet(payload0, \"phase\") as string) == \"pre\" &&\n"
        "            (mapGet(payload1, \"phase\") as string) == \"post\" &&\n"
        "            len(markers0) == 2 && markers0[0] == \"ai\" && markers0[1] == \"nav\" &&\n"
        "            len(markers1) == 1 && markers1[0] == \"gpu\" &&\n"
        "            (mapGet(context0, \"owner\") as string) == \"sim\" && (mapGet(context0, \"frame\") as int) == 500 &&\n"
        "            (mapGet(context1, \"owner\") as string) == \"render\" && (mapGet(context1, \"frame\") as int) == 501) {\n"
        "            hostPresetFrameBatchHits = hostPresetFrameBatchHits + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func onHostPresetTypedMixedBatch(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 3) {\n"
        "        var input_payload = mapGet(events[0], \"payload\") as map<string, any>;\n"
        "        var window_payload = mapGet(events[1], \"payload\") as map<string, any>;\n"
        "        var frame_payload = mapGet(events[2], \"payload\") as map<string, any>;\n"
        "        var input_context = mapGet(events[0], \"context\") as map<string, any>;\n"
        "        var window_context = mapGet(events[1], \"context\") as map<string, any>;\n"
        "        var frame_context = mapGet(events[2], \"context\") as map<string, any>;\n"
        "        var combo = mapGet(input_payload, \"combo\") as array<string>;\n"
        "        var rect = mapGet(window_payload, \"rect\") as (int, int);\n"
        "        var markers = mapGet(frame_payload, \"markers\") as array<string>;\n"
        "        if (extSummarizeInputEvent(events[0]) == \"input:gamepad:73:true:input:1:2\" &&\n"
        "            extSummarizeWindowEvent(events[1]) == \"window:resize:1720:960:true:window:4:2\" &&\n"
        "            extSummarizeFrameEvent(events[2]) == \"frame:render:910:0.014:frame:5:2\" &&\n"
        "            (mapGet(input_payload, \"score\") as int) == 17 &&\n"
        "            len(combo) == 2 && combo[0] == \"lb\" && combo[1] == \"rb\" &&\n"
        "            (mapGet(window_payload, \"title\") as string) == \"inspector\" &&\n"
        "            rect.0 == 1720 && rect.1 == 960 &&\n"
        "            (mapGet(frame_payload, \"phase\") as string) == \"present\" &&\n"
        "            len(markers) == 2 && markers[0] == \"gpu\" && markers[1] == \"present\" &&\n"
        "            (mapGet(input_context, \"owner\") as string) == \"pad\" && (mapGet(input_context, \"frame\") as int) == 910 &&\n"
        "            (mapGet(window_context, \"owner\") as string) == \"dock\" && (mapGet(window_context, \"frame\") as int) == 911 &&\n"
        "            (mapGet(frame_context, \"owner\") as string) == \"renderer\" && (mapGet(frame_context, \"frame\") as int) == 912) {\n"
        "            hostPresetTypedMixedBatchHits = hostPresetTypedMixedBatchHits + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func onHostFrameEnvelopeBatch(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 3) {\n"
        "        var input_payload = mapGet(events[0], \"payload\") as map<string, any>;\n"
        "        var window_payload = mapGet(events[1], \"payload\") as map<string, any>;\n"
        "        var frame_payload = mapGet(events[2], \"payload\") as map<string, any>;\n"
        "        var input_context = mapGet(events[0], \"context\") as map<string, any>;\n"
        "        var window_context = mapGet(events[1], \"context\") as map<string, any>;\n"
        "        var frame_context = mapGet(events[2], \"context\") as map<string, any>;\n"
        "        var combo = mapGet(input_payload, \"combo\") as array<string>;\n"
        "        var rect = mapGet(window_payload, \"rect\") as (int, int);\n"
        "        var markers = mapGet(frame_payload, \"markers\") as array<string>;\n"
        "        if (extSummarizeInputEvent(events[0]) == \"input:gamepad:73:true:input:1:2\" &&\n"
        "            extSummarizeWindowEvent(events[1]) == \"window:resize:1720:960:true:window:4:2\" &&\n"
        "            extSummarizeFrameEvent(events[2]) == \"frame:render:910:0.014:frame:5:2\" &&\n"
        "            (mapGet(input_payload, \"score\") as int) == 17 &&\n"
        "            len(combo) == 2 && combo[0] == \"lb\" && combo[1] == \"rb\" &&\n"
        "            (mapGet(window_payload, \"title\") as string) == \"inspector\" &&\n"
        "            rect.0 == 1720 && rect.1 == 960 &&\n"
        "            (mapGet(frame_payload, \"phase\") as string) == \"present\" &&\n"
        "            len(markers) == 2 && markers[0] == \"gpu\" && markers[1] == \"present\" &&\n"
        "            (mapGet(input_context, \"owner\") as string) == \"loop\" && (mapGet(input_context, \"frame\") as int) == 1200 &&\n"
        "            (mapGet(window_context, \"owner\") as string) == \"loop\" && (mapGet(window_context, \"frame\") as int) == 1200 &&\n"
        "            (mapGet(frame_context, \"owner\") as string) == \"loop\" && (mapGet(frame_context, \"frame\") as int) == 1200) {\n"
        "            hostFrameEnvelopeBatchHits = hostFrameEnvelopeBatchHits + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func onHostFrameEnvelopeBuilder(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 3) {\n"
        "        var input_payload = mapGet(events[0], \"payload\") as map<string, any>;\n"
        "        var frame_payload = mapGet(events[1], \"payload\") as map<string, any>;\n"
        "        var window_payload = mapGet(events[2], \"payload\") as map<string, any>;\n"
        "        var input_context = mapGet(events[0], \"context\") as map<string, any>;\n"
        "        var frame_context = mapGet(events[1], \"context\") as map<string, any>;\n"
        "        var window_context = mapGet(events[2], \"context\") as map<string, any>;\n"
        "        var coords = mapGet(input_payload, \"coords\") as (int, int);\n"
        "        var markers = mapGet(frame_payload, \"markers\") as array<string>;\n"
        "        var rect = mapGet(window_payload, \"rect\") as (int, int);\n"
        "        if (extSummarizeInputEvent(events[0]) == \"input:mouse:201:true:input:1:2\" &&\n"
        "            extSummarizeFrameEvent(events[1]) == \"frame:simulate:1201:0.008:frame:5:2\" &&\n"
        "            extSummarizeWindowEvent(events[2]) == \"window:resize:1280:720:true:window:4:2\" &&\n"
        "            (mapGet(input_payload, \"label\") as string) == \"drag\" &&\n"
        "            coords.0 == 12 && coords.1 == 34 &&\n"
        "            len(markers) == 2 && markers[0] == \"fixed\" && markers[1] == \"late\" &&\n"
        "            (mapGet(window_payload, \"title\") as string) == \"main\" &&\n"
        "            rect.0 == 1280 && rect.1 == 720 &&\n"
        "            (mapGet(input_context, \"owner\") as string) == \"builder\" && (mapGet(input_context, \"frame\") as int) == 1300 &&\n"
        "            (mapGet(frame_context, \"owner\") as string) == \"builder\" && (mapGet(frame_context, \"frame\") as int) == 1300 &&\n"
        "            (mapGet(window_context, \"owner\") as string) == \"builder\" && (mapGet(window_context, \"frame\") as int) == 1300) {\n"
        "            hostFrameEnvelopeBuilderHits = hostFrameEnvelopeBuilderHits + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func onHostFrameEnvelopeHeapBuilder(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 3) {\n"
        "        var input_payload = mapGet(events[0], \"payload\") as map<string, any>;\n"
        "        var frame_payload = mapGet(events[1], \"payload\") as map<string, any>;\n"
        "        var window_payload = mapGet(events[2], \"payload\") as map<string, any>;\n"
        "        var input_context = mapGet(events[0], \"context\") as map<string, any>;\n"
        "        var frame_context = mapGet(events[1], \"context\") as map<string, any>;\n"
        "        var window_context = mapGet(events[2], \"context\") as map<string, any>;\n"
        "        var combo = mapGet(input_payload, \"combo\") as array<string>;\n"
        "        var markers = mapGet(frame_payload, \"markers\") as array<string>;\n"
        "        var rect = mapGet(window_payload, \"rect\") as (int, int);\n"
        "        if (extSummarizeInputEvent(events[0]) == \"input:keyboard:301:false:input:1:2\" &&\n"
        "            extSummarizeFrameEvent(events[1]) == \"frame:render:1401:0.011:frame:5:2\" &&\n"
        "            extSummarizeWindowEvent(events[2]) == \"window:move:900:700:false:window:4:2\" &&\n"
        "            (mapGet(input_payload, \"score\") as int) == 44 &&\n"
        "            len(combo) == 2 && combo[0] == \"x\" && combo[1] == \"y\" &&\n"
        "            (mapGet(frame_payload, \"phase\") as string) == \"present\" &&\n"
        "            len(markers) == 2 && markers[0] == \"render\" && markers[1] == \"ui\" &&\n"
        "            (mapGet(window_payload, \"title\") as string) == \"aux\" &&\n"
        "            rect.0 == 9 && rect.1 == 7 &&\n"
        "            (mapGet(input_context, \"owner\") as string) == \"heap\" && (mapGet(input_context, \"frame\") as int) == 1400 &&\n"
        "            (mapGet(frame_context, \"owner\") as string) == \"heap\" && (mapGet(frame_context, \"frame\") as int) == 1400 &&\n"
        "            (mapGet(window_context, \"owner\") as string) == \"heap\" && (mapGet(window_context, \"frame\") as int) == 1400) {\n"
        "            hostFrameEnvelopeHeapBuilderHits = hostFrameEnvelopeHeapBuilderHits + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func onHostEventLoopSessionFrame(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 3) {\n"
        "        var input_payload = mapGet(events[0], \"payload\") as map<string, any>;\n"
        "        var frame_payload = mapGet(events[1], \"payload\") as map<string, any>;\n"
        "        var window_payload = mapGet(events[2], \"payload\") as map<string, any>;\n"
        "        var input_context = mapGet(events[0], \"context\") as map<string, any>;\n"
        "        var frame_context = mapGet(events[1], \"context\") as map<string, any>;\n"
        "        var window_context = mapGet(events[2], \"context\") as map<string, any>;\n"
        "        var coords = mapGet(input_payload, \"coords\") as (int, int);\n"
        "        var markers = mapGet(frame_payload, \"markers\") as array<string>;\n"
        "        var rect = mapGet(window_payload, \"rect\") as (int, int);\n"
        "        if (extSummarizeInputEvent(events[0]) == \"input:touch:401:true:input:1:2\" &&\n"
        "            extSummarizeFrameEvent(events[1]) == \"frame:update:1501:0.012:frame:5:2\" &&\n"
        "            extSummarizeWindowEvent(events[2]) == \"window:focus:1024:768:true:window:4:2\" &&\n"
        "            (mapGet(input_payload, \"label\") as string) == \"session\" &&\n"
        "            coords.0 == 6 && coords.1 == 8 &&\n"
        "            (mapGet(frame_payload, \"phase\") as string) == \"sim\" &&\n"
        "            len(markers) == 2 && markers[0] == \"tick\" && markers[1] == \"sim\" &&\n"
        "            (mapGet(window_payload, \"title\") as string) == \"session-window\" &&\n"
        "            rect.0 == 10 && rect.1 == 20 &&\n"
        "            (mapGet(input_context, \"owner\") as string) == \"session\" && (mapGet(input_context, \"frame\") as int) == 1500 &&\n"
        "            (mapGet(frame_context, \"owner\") as string) == \"session\" && (mapGet(frame_context, \"frame\") as int) == 1500 &&\n"
        "            (mapGet(window_context, \"owner\") as string) == \"session\" && (mapGet(window_context, \"frame\") as int) == 1500) {\n"
        "            hostEventLoopSessionFrameHits = hostEventLoopSessionFrameHits + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func onHostEventLoopSessionFrameAlt(events: array<map<string, any>>): void {\n"
        "    if (len(events) == 3) {\n"
        "        var input_payload = mapGet(events[0], \"payload\") as map<string, any>;\n"
        "        var frame_payload = mapGet(events[1], \"payload\") as map<string, any>;\n"
        "        var window_payload = mapGet(events[2], \"payload\") as map<string, any>;\n"
        "        var input_context = mapGet(events[0], \"context\") as map<string, any>;\n"
        "        var frame_context = mapGet(events[1], \"context\") as map<string, any>;\n"
        "        var window_context = mapGet(events[2], \"context\") as map<string, any>;\n"
        "        var coords = mapGet(input_payload, \"coords\") as (int, int);\n"
        "        var markers = mapGet(frame_payload, \"markers\") as array<string>;\n"
        "        var rect = mapGet(window_payload, \"rect\") as (int, int);\n"
        "        if (extSummarizeInputEvent(events[0]) == \"input:touch:401:true:input:1:2\" &&\n"
        "            extSummarizeFrameEvent(events[1]) == \"frame:update:1501:0.012:frame:5:2\" &&\n"
        "            extSummarizeWindowEvent(events[2]) == \"window:focus:1024:768:true:window:4:2\" &&\n"
        "            (mapGet(input_payload, \"label\") as string) == \"session\" &&\n"
        "            coords.0 == 6 && coords.1 == 8 &&\n"
        "            (mapGet(frame_payload, \"phase\") as string) == \"sim\" &&\n"
        "            len(markers) == 2 && markers[0] == \"tick\" && markers[1] == \"sim\" &&\n"
        "            (mapGet(window_payload, \"title\") as string) == \"session-window\" &&\n"
        "            rect.0 == 10 && rect.1 == 20 &&\n"
        "            (mapGet(input_context, \"owner\") as string) == \"session\" && (mapGet(input_context, \"frame\") as int) == 1500 &&\n"
        "            (mapGet(frame_context, \"owner\") as string) == \"session\" && (mapGet(frame_context, \"frame\") as int) == 1500 &&\n"
        "            (mapGet(window_context, \"owner\") as string) == \"session\" && (mapGet(window_context, \"frame\") as int) == 1500) {\n"
        "            hostEventLoopSessionAltFrameHits = hostEventLoopSessionAltFrameHits + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func onHostSessionTickInputEvent(event: map<string, any>): void {\n"
        "    if (extSummarizeInputEvent(event) == \"input:keyboard:777:true:input:1:2\") {\n"
        "        hostSessionTickInputHits = hostSessionTickInputHits + 1;\n"
        "    }\n"
        "}\n"
        "func onHostSessionSlowTickInputEvent(event: map<string, any>): void {\n"
        "    var i = 0;\n"
        "    var total = 0;\n"
        "    while (i < 2000000) {\n"
        "        total = total + 1;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    if (total > 0 && extSummarizeInputEvent(event) == \"input:keyboard:778:true:input:1:2\") {\n"
        "        hostSessionSlowTickInputHits = hostSessionSlowTickInputHits + 1;\n"
        "    }\n"
        "}\n"
        "func spawnAndJoinPostedWorker(): int {\n"
        "    var worker: PostedIntCallbackWorker = extStartPostedIntCallbackWorker((func(delta: int): void {\n"
        "        counterAdd(hostCounter, delta);\n"
        "    }), 23, 0);\n"
        "    if (!extJoinPostedIntCallbackWorker(worker)) {\n"
        "        panic(\"worker join failed\");\n"
        "    }\n"
        "    return counterGet(hostCounter);\n"
        "}\n"
        "func getHostCounter(): int {\n"
        "    return counterGet(hostCounter);\n"
        "}\n"
        "func getHostInputHits(): int {\n"
        "    return hostInputHits;\n"
        "}\n"
        "func getHostWindowHits(): int {\n"
        "    return hostWindowHits;\n"
        "}\n"
        "func getHostFrameHits(): int {\n"
        "    return hostFrameHits;\n"
        "}\n"
        "func getHostInputBatchHits(): int {\n"
        "    return hostInputBatchHits;\n"
        "}\n"
        "func getHostWindowBatchHits(): int {\n"
        "    return hostWindowBatchHits;\n"
        "}\n"
        "func getHostFrameBatchHits(): int {\n"
        "    return hostFrameBatchHits;\n"
        "}\n"
        "func getHostMixedBatchHits(): int {\n"
        "    return hostMixedBatchHits;\n"
        "}\n"
        "func getHostCustomInputHits(): int {\n"
        "    return hostCustomInputHits;\n"
        "}\n"
        "func getHostCustomMixedBatchHits(): int {\n"
        "    return hostCustomMixedBatchHits;\n"
        "}\n"
        "func getHostCustomMixedInputHits(): int {\n"
        "    return hostCustomMixedInputHits;\n"
        "}\n"
        "func getHostCustomMixedWindowHits(): int {\n"
        "    return hostCustomMixedWindowHits;\n"
        "}\n"
        "func getHostCustomMixedFrameHits(): int {\n"
        "    return hostCustomMixedFrameHits;\n"
        "}\n"
        "func getHostExtraInputHits(): int {\n"
        "    return hostExtraInputHits;\n"
        "}\n"
        "func getHostExtraMixedBatchHits(): int {\n"
        "    return hostExtraMixedBatchHits;\n"
        "}\n"
        "func getHostStructuredInputHits(): int {\n"
        "    return hostStructuredInputHits;\n"
        "}\n"
        "func getHostStructuredMixedBatchHits(): int {\n"
        "    return hostStructuredMixedBatchHits;\n"
        "}\n"
        "func getHostMappedInputHits(): int {\n"
        "    return hostMappedInputHits;\n"
        "}\n"
        "func getHostMappedMixedBatchHits(): int {\n"
        "    return hostMappedMixedBatchHits;\n"
        "}\n"
        "func getHostPresetWindowHits(): int {\n"
        "    return hostPresetWindowHits;\n"
        "}\n"
        "func getHostPresetFrameHits(): int {\n"
        "    return hostPresetFrameHits;\n"
        "}\n"
        "func getHostPresetInputBatchHits(): int {\n"
        "    return hostPresetInputBatchHits;\n"
        "}\n"
        "func getHostPresetWindowBatchHits(): int {\n"
        "    return hostPresetWindowBatchHits;\n"
        "}\n"
        "func getHostPresetFrameBatchHits(): int {\n"
        "    return hostPresetFrameBatchHits;\n"
        "}\n"
        "func getHostPresetTypedMixedBatchHits(): int {\n"
        "    return hostPresetTypedMixedBatchHits;\n"
        "}\n"
        "func getHostFrameEnvelopeBatchHits(): int {\n"
        "    return hostFrameEnvelopeBatchHits;\n"
        "}\n"
        "func getHostFrameEnvelopeBuilderHits(): int {\n"
        "    return hostFrameEnvelopeBuilderHits;\n"
        "}\n"
        "func getHostFrameEnvelopeHeapBuilderHits(): int {\n"
        "    return hostFrameEnvelopeHeapBuilderHits;\n"
        "}\n"
        "func getHostEventLoopSessionFrameHits(): int {\n"
        "    return hostEventLoopSessionFrameHits;\n"
        "}\n"
        "func getHostEventLoopSessionAltFrameHits(): int {\n"
        "    return hostEventLoopSessionAltFrameHits;\n"
        "}\n"
        "func getHostSessionTickInputHits(): int {\n"
        "    return hostSessionTickInputHits;\n"
        "}\n"
        "func getHostSessionSlowTickInputHits(): int {\n"
        "    return hostSessionSlowTickInputHits;\n"
        "}\n"
        "func startDelayedPostedWorker(): int {\n"
        "    delayedHostWorker = extStartPostedIntCallbackWorker((func(delta: int): void {\n"
        "        counterAdd(hostCounter, delta);\n"
        "    }), 31, 25);\n"
        "    return counterGet(hostCounter);\n"
        "}\n"
        "func joinDelayedPostedWorker(): int {\n"
        "    if (delayedHostWorker == nil) {\n"
        "        return 0;\n"
        "    }\n"
        "    var worker: PostedIntCallbackWorker = delayedHostWorker as PostedIntCallbackWorker;\n"
        "    var ok: bool = extJoinPostedIntCallbackWorker(worker);\n"
        "    delayedHostWorker = nil;\n"
        "    if (ok) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "func main(): void {\n"
        "}\n";
}

static void exercise_runtime_host_callback_pump_path(const char* path,
                                                     RuntimeOptions options,
                                                     RuntimeLoadMode expected_mode,
                                                     const char* label_prefix) {
    Runtime* rt = runtime_create_with_options(path, options);
    RuntimePostedInputEvent input_event = {
        .device = "keyboard",
        .code = 32,
        .pressed = true
    };
    RuntimePostedInputEvent session_tick_input_event = {
        .device = "keyboard",
        .code = 777,
        .pressed = true
    };
    RuntimePostedInputEvent session_slow_tick_input_event = {
        .device = "keyboard",
        .code = 778,
        .pressed = true
    };
    RuntimePostedWindowEvent window_event = {
        .event_name = "resize",
        .width = 1280,
        .height = 720,
        .focused = true
    };
    RuntimePostedFrameEvent frame_event = {
        .phase = "update",
        .frame_number = 60,
        .delta_seconds = 0.25
    };
    RuntimePostedInputEvent input_batch[2] = {
        { .device = "keyboard", .code = 11, .pressed = true },
        { .device = "mouse", .code = 12, .pressed = false }
    };
    RuntimePostedWindowEvent window_batch[2] = {
        { .event_name = "resize", .width = 800, .height = 600, .focused = true },
        { .event_name = "blur", .width = 800, .height = 600, .focused = false }
    };
    RuntimePostedFrameEvent frame_batch[2] = {
        { .phase = "update", .frame_number = 70, .delta_seconds = 0.125 },
        { .phase = "render", .frame_number = 71, .delta_seconds = 0.25 }
    };
    RuntimePostedEvent mixed_batch[3] = {0};
    const char* custom_input_phases[3] = { "capture", "route", "dispatch" };
    const RuntimePostedEventMetaOverride custom_input_meta = {
        "host-input",
        42,
        custom_input_phases,
        3
    };
    RuntimePostedInputEvent custom_input_event = {
        .device = "pen",
        .code = 9,
        .pressed = false,
        .meta_override = &custom_input_meta
    };
    const char* custom_mixed_input_phases[1] = { "sample" };
    const char* custom_mixed_window_phases[3] = { "blur", "layout", "present" };
    const char* custom_mixed_frame_phases[2] = { "simulate", "present" };
    const RuntimePostedEventMetaOverride custom_mixed_input_meta = {
        "host-mixed-input",
        11,
        custom_mixed_input_phases,
        1
    };
    const RuntimePostedEventMetaOverride custom_mixed_window_meta = {
        "host-mixed-window",
        12,
        custom_mixed_window_phases,
        3
    };
    const RuntimePostedEventMetaOverride custom_mixed_frame_meta = {
        "host-mixed-frame",
        13,
        custom_mixed_frame_phases,
        2
    };
    RuntimePostedEvent custom_mixed_batch[3] = {0};
    const RuntimePostedEventExtraField extra_input_fields[4] = {
        { .name = "playerId", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 7 } },
        { .name = "label", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "aim" } },
        { .name = "analog", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_DOUBLE, .double_value = 0.75 } },
        { .name = "consumed", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_BOOL, .bool_value = false } }
    };
    RuntimePostedInputEvent extra_input_event = {
        .device = "stylus",
        .code = 77,
        .pressed = true,
        .extra_fields = extra_input_fields,
        .extra_field_count = 4
    };
    const RuntimePostedEventExtraField invalid_input_fields[1] = {
        { .name = "kind", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "bad" } }
    };
    RuntimePostedInputEvent invalid_input_event = {
        .device = "stylus",
        .code = 78,
        .pressed = false,
        .extra_fields = invalid_input_fields,
        .extra_field_count = 1
    };
    const RuntimePostedEventExtraField extra_mixed_input_fields[1] = {
        { .name = "slot", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 2 } }
    };
    const RuntimePostedEventExtraField extra_mixed_window_fields[1] = {
        { .name = "scene", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "pause-menu" } }
    };
    const RuntimePostedEventExtraField extra_mixed_frame_fields[2] = {
        { .name = "fixed", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_BOOL, .bool_value = true } },
        { .name = "alpha", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_DOUBLE, .double_value = 0.5 } }
    };
    const RuntimePostedEventExtraValue structured_input_samples[3] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 4 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 5 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 6 }
    };
    const RuntimePostedEventExtraValue structured_input_bounds[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 64 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 128 }
    };
    const RuntimePostedEventExtraField structured_input_fields[2] = {
        { .name = "samples", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_ARRAY, .items = structured_input_samples, .item_count = 3 } },
        { .name = "bounds", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_TUPLE, .items = structured_input_bounds, .item_count = 2 } }
    };
    RuntimePostedInputEvent structured_input_event = {
        .device = "keyboard",
        .code = 101,
        .pressed = true,
        .extra_fields = structured_input_fields,
        .extra_field_count = 2
    };
    const RuntimePostedEventExtraValue structured_mixed_input_samples[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 8 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 9 }
    };
    const RuntimePostedEventExtraValue structured_mixed_window_rect[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 10 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 20 }
    };
    const RuntimePostedEventExtraValue structured_mixed_frame_markers[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "fixed" },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "net" }
    };
    const RuntimePostedEventExtraField structured_mixed_input_fields[1] = {
        { .name = "samples", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_ARRAY, .items = structured_mixed_input_samples, .item_count = 2 } }
    };
    const RuntimePostedEventExtraField structured_mixed_window_fields[1] = {
        { .name = "rect", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_TUPLE, .items = structured_mixed_window_rect, .item_count = 2 } }
    };
    const RuntimePostedEventExtraField structured_mixed_frame_fields[1] = {
        { .name = "markers", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_ARRAY, .items = structured_mixed_frame_markers, .item_count = 2 } }
    };
    RuntimePostedEvent structured_mixed_batch[3] = {0};
    RuntimePostedEventExtraValue mapped_input_coords[2];
    RuntimePostedEventExtraValue mapped_input_buttons[2];
    RuntimePostedInputStatePayloadSpec mapped_input_payload_spec = {0};
    RuntimePostedEventContextSpec mapped_input_context_spec = {0};
    RuntimePostedInputEvent mapped_input_event = {
        .device = "tablet",
        .code = 44,
        .pressed = true
    };
    const RuntimePostedEventExtraValue mapped_mixed_input_combo[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "a" },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "b" }
    };
    const RuntimePostedEventExtraValue mapped_mixed_window_rect[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 30 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 40 }
    };
    const RuntimePostedEventExtraValue mapped_mixed_frame_markers[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "gpu" },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "cpu" }
    };
    RuntimePostedEventExtraMapEntry mapped_mixed_input_payload_entries[2];
    RuntimePostedInputComboPayloadSpec mapped_mixed_input_payload_spec = {0};
    RuntimePostedEventExtraMapEntry mapped_mixed_window_payload_entries[2];
    RuntimePostedWindowRectPayloadSpec mapped_mixed_window_payload_spec = {0};
    RuntimePostedEventExtraMapEntry mapped_mixed_frame_payload_entries[2];
    RuntimePostedFrameMarkerPayloadSpec mapped_mixed_frame_payload_spec = {0};
    RuntimePostedEventExtraField mapped_mixed_input_fields[1];
    RuntimePostedEventExtraField mapped_mixed_window_fields[1];
    RuntimePostedEventExtraField mapped_mixed_frame_fields[1];
    RuntimePostedEvent mapped_mixed_batch[3] = {0};
    const RuntimePostedEventExtraValue preset_window_rect[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 1280 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 720 }
    };
    RuntimePostedWindowRectPayloadSpec preset_window_payload_spec = {0};
    RuntimePostedEventContextSpec preset_window_context_spec = {0};
    RuntimePostedWindowEvent preset_window_event = {
        .event_name = "resize",
        .width = 1280,
        .height = 720,
        .focused = false
    };
    const RuntimePostedEventExtraValue preset_frame_markers[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "gpu" },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "ui" }
    };
    RuntimePostedFrameMarkerPayloadSpec preset_frame_payload_spec = {0};
    RuntimePostedEventContextSpec preset_frame_context_spec = {0};
    RuntimePostedFrameEvent preset_frame_event = {
        .phase = "render",
        .frame_number = 77,
        .delta_seconds = 0.025
    };
    RuntimePostedInputEvent preset_input_batch_events[2] = {
        { .device = "tablet", .code = 61, .pressed = true },
        { .device = "stylus", .code = 62, .pressed = false }
    };
    const RuntimePostedEventExtraValue preset_input_batch_coords0[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 10 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 20 }
    };
    const RuntimePostedEventExtraValue preset_input_batch_coords1[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 30 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 40 }
    };
    const RuntimePostedEventExtraValue preset_input_batch_buttons0[1] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 1 }
    };
    const RuntimePostedEventExtraValue preset_input_batch_buttons1[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 2 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 3 }
    };
    RuntimePostedInputStatePayloadSpec preset_input_batch_payload_specs[2] = {0};
    RuntimePostedEventContextSpec preset_input_batch_context_specs[2] = {0};
    RuntimePostedWindowEvent preset_window_batch_events[2] = {
        { .event_name = "resize", .width = 1440, .height = 900, .focused = true },
        { .event_name = "move", .width = 1600, .height = 900, .focused = true }
    };
    const RuntimePostedEventExtraValue preset_window_batch_rect0[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 1440 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 900 }
    };
    const RuntimePostedEventExtraValue preset_window_batch_rect1[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 50 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 60 }
    };
    RuntimePostedWindowRectPayloadSpec preset_window_batch_payload_specs[2] = {0};
    RuntimePostedEventContextSpec preset_window_batch_context_specs[2] = {0};
    RuntimePostedFrameEvent preset_frame_batch_events[2] = {
        { .phase = "logic", .frame_number = 500, .delta_seconds = 0.010 },
        { .phase = "render", .frame_number = 501, .delta_seconds = 0.016 }
    };
    const RuntimePostedEventExtraValue preset_frame_batch_markers0[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "ai" },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "nav" }
    };
    const RuntimePostedEventExtraValue preset_frame_batch_markers1[1] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "gpu" }
    };
    RuntimePostedFrameMarkerPayloadSpec preset_frame_batch_payload_specs[2] = {0};
    RuntimePostedEventContextSpec preset_frame_batch_context_specs[2] = {0};
    const RuntimePostedEventExtraValue preset_typed_mixed_combo[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "lb" },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "rb" }
    };
    const RuntimePostedEventExtraValue preset_typed_mixed_rect[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 1720 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 960 }
    };
    const RuntimePostedEventExtraValue preset_typed_mixed_markers[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "gpu" },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "present" }
    };
    RuntimePostedTypedEvent preset_typed_mixed_batch[3] = {0};
    RuntimePostedTypedInputBatchSpec preset_frame_envelope_input_batch = {0};
    RuntimePostedTypedWindowBatchSpec preset_frame_envelope_window_batch = {0};
    RuntimePostedTypedFrameBatchSpec preset_frame_envelope_frame_batch = {0};
    RuntimePostedFrameEnvelopeBatchSpec preset_frame_envelope_batch = {0};
    RuntimePostedTypedEvent frame_envelope_builder_events[4] = {0};
    RuntimePostedFrameEnvelopeBuilder frame_envelope_builder = {0};
    RuntimePostedFrameEnvelopeHeapBuilder frame_envelope_heap_builder = {0};
    RuntimeHostEventLoopSession host_event_loop_session;
    RuntimeHostEventLoopSessionStepResult session_step_result = {0};
    RuntimeHostEventLoopSessionStepOptions session_step_options = {0};
    RuntimePostedTypedInputBatchSpec preset_typed_mixed_input_batch = {0};
    RuntimePostedTypedWindowBatchSpec preset_typed_mixed_window_batch = {0};
    RuntimePostedTypedFrameBatchSpec preset_typed_mixed_frame_batch = {0};
    const RuntimePostedEventExtraValue frame_envelope_builder_coords[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 12 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 34 }
    };
    const RuntimePostedEventExtraValue frame_envelope_builder_markers[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "fixed" },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "late" }
    };
    const RuntimePostedEventExtraValue frame_envelope_builder_rect[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 1280 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 720 }
    };
    const RuntimePostedEventExtraValue frame_envelope_heap_combo[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "x" },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "y" }
    };
    const RuntimePostedEventExtraValue frame_envelope_heap_markers[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "render" },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "ui" }
    };
    const RuntimePostedEventExtraValue frame_envelope_heap_rect[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 9 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 7 }
    };
    const RuntimePostedEventExtraValue event_loop_session_coords[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 6 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 8 }
    };
    const RuntimePostedEventExtraValue event_loop_session_markers[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "tick" },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_STRING, .string_value = "sim" }
    };
    const RuntimePostedEventExtraValue event_loop_session_rect[2] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 10 },
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 20 }
    };
    const RuntimePostedEventExtraValue invalid_nested_leaf[1] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 1 }
    };
    const RuntimePostedEventExtraValue invalid_nested_outer[1] = {
        { .kind = RUNTIME_POSTED_EVENT_FIELD_ARRAY, .items = invalid_nested_leaf, .item_count = 1 }
    };
    const RuntimePostedEventExtraField invalid_nested_input_fields[1] = {
        { .name = "nested", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_ARRAY, .items = invalid_nested_outer, .item_count = 1 } }
    };
    RuntimePostedInputEvent invalid_nested_input_event = {
        .device = "keyboard",
        .code = 102,
        .pressed = false,
        .extra_fields = invalid_nested_input_fields,
        .extra_field_count = 1
    };
    const RuntimePostedEventExtraMapEntry invalid_nested_map_child_entries[1] = {
        { .key = "value", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_INT, .int_value = 3 } }
    };
    const RuntimePostedEventExtraMapEntry invalid_nested_map_entries[1] = {
        { .key = "child", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_MAP, .map_entries = invalid_nested_map_child_entries, .map_entry_count = 1 } }
    };
    const RuntimePostedEventExtraField invalid_nested_map_input_fields[1] = {
        { .name = "payload", .value = { .kind = RUNTIME_POSTED_EVENT_FIELD_MAP, .map_entries = invalid_nested_map_entries, .map_entry_count = 1 } }
    };
    RuntimePostedInputEvent invalid_nested_map_input_event = {
        .device = "keyboard",
        .code = 103,
        .pressed = true,
        .extra_fields = invalid_nested_map_input_fields,
        .extra_field_count = 1
    };
    RuntimePostedEvent extra_mixed_batch[3] = {0};
    char label[256];
    char error_buf[256];
    int value = 0;
    int drained = 0;
    bool previous_auto_drain = false;

#define HOST_PUMP_ASSERT(condition, suffix) \
    do { \
        snprintf(label, sizeof(label), "%s %s", label_prefix, suffix); \
        TEST_ASSERT((condition), label); \
    } while (0)

    {
        RuntimePostedEventExtraArrayBuilder coords_builder;
        RuntimePostedEventExtraArrayBuilder buttons_builder;
        runtime_posted_event_extra_array_builder_init(&coords_builder, mapped_input_coords, 2);
        runtime_posted_event_extra_array_builder_init(&buttons_builder, mapped_input_buttons, 2);

        HOST_PUMP_ASSERT(runtime_posted_event_extra_array_builder_add_int(&coords_builder, 3),
                         "mapped input preset coords builder accepts first element");
        HOST_PUMP_ASSERT(runtime_posted_event_extra_array_builder_add_int(&coords_builder, 4),
                         "mapped input preset coords builder accepts second element");
        HOST_PUMP_ASSERT(runtime_posted_event_extra_array_builder_add_int(&buttons_builder, 1),
                         "mapped input preset buttons builder accepts first element");
        HOST_PUMP_ASSERT(runtime_posted_event_extra_array_builder_add_int(&buttons_builder, 2),
                         "mapped input preset buttons builder accepts second element");
        mapped_input_payload_spec.label = "ink";
        mapped_input_payload_spec.active = true;
        mapped_input_payload_spec.score = 11;
        mapped_input_payload_spec.coords_items = mapped_input_coords;
        mapped_input_payload_spec.coords_count = 2;
        mapped_input_payload_spec.buttons_items = mapped_input_buttons;
        mapped_input_payload_spec.button_count = 2;
        mapped_input_context_spec.owner = "host";
        mapped_input_context_spec.frame = 7;
    }

    mapped_mixed_input_payload_spec.score = 5;
    mapped_mixed_input_payload_spec.combo_items = mapped_mixed_input_combo;
    mapped_mixed_input_payload_spec.combo_count = 2;
    mapped_mixed_input_fields[0] = runtime_posted_event_build_input_combo_payload_field(mapped_mixed_input_payload_entries,
                                                                                         2,
                                                                                         &mapped_mixed_input_payload_spec);
    mapped_mixed_window_payload_spec.title = "scene";
    mapped_mixed_window_payload_spec.rect_items = mapped_mixed_window_rect;
    mapped_mixed_window_payload_spec.rect_count = 2;
    mapped_mixed_window_fields[0] = runtime_posted_event_build_window_rect_payload_field(mapped_mixed_window_payload_entries,
                                                                                          2,
                                                                                          &mapped_mixed_window_payload_spec);
    mapped_mixed_frame_payload_spec.phase = "late";
    mapped_mixed_frame_payload_spec.marker_items = mapped_mixed_frame_markers;
    mapped_mixed_frame_payload_spec.marker_count = 2;
    mapped_mixed_frame_fields[0] = runtime_posted_event_build_frame_marker_payload_field(mapped_mixed_frame_payload_entries,
                                                                                          2,
                                                                                          &mapped_mixed_frame_payload_spec);
    preset_window_payload_spec.title = "editor";
    preset_window_payload_spec.rect_items = preset_window_rect;
    preset_window_payload_spec.rect_count = 2;
    preset_window_context_spec.owner = "ui";
    preset_window_context_spec.frame = 99;
    preset_frame_payload_spec.phase = "post";
    preset_frame_payload_spec.marker_items = preset_frame_markers;
    preset_frame_payload_spec.marker_count = 2;
    preset_frame_context_spec.owner = "renderer";
    preset_frame_context_spec.frame = 77;
    preset_input_batch_payload_specs[0].label = "drag";
    preset_input_batch_payload_specs[0].active = true;
    preset_input_batch_payload_specs[0].score = 21;
    preset_input_batch_payload_specs[0].coords_items = preset_input_batch_coords0;
    preset_input_batch_payload_specs[0].coords_count = 2;
    preset_input_batch_payload_specs[0].buttons_items = preset_input_batch_buttons0;
    preset_input_batch_payload_specs[0].button_count = 1;
    preset_input_batch_payload_specs[1].label = "hover";
    preset_input_batch_payload_specs[1].active = false;
    preset_input_batch_payload_specs[1].score = 22;
    preset_input_batch_payload_specs[1].coords_items = preset_input_batch_coords1;
    preset_input_batch_payload_specs[1].coords_count = 2;
    preset_input_batch_payload_specs[1].buttons_items = preset_input_batch_buttons1;
    preset_input_batch_payload_specs[1].button_count = 2;
    preset_input_batch_context_specs[0].owner = "ui";
    preset_input_batch_context_specs[0].frame = 200;
    preset_input_batch_context_specs[1].owner = "ui";
    preset_input_batch_context_specs[1].frame = 201;
    preset_window_batch_payload_specs[0].title = "editor-left";
    preset_window_batch_payload_specs[0].rect_items = preset_window_batch_rect0;
    preset_window_batch_payload_specs[0].rect_count = 2;
    preset_window_batch_payload_specs[1].title = "scene";
    preset_window_batch_payload_specs[1].rect_items = preset_window_batch_rect1;
    preset_window_batch_payload_specs[1].rect_count = 2;
    preset_window_batch_context_specs[0].owner = "window";
    preset_window_batch_context_specs[0].frame = 300;
    preset_window_batch_context_specs[1].owner = "window";
    preset_window_batch_context_specs[1].frame = 301;
    preset_frame_batch_payload_specs[0].phase = "pre";
    preset_frame_batch_payload_specs[0].marker_items = preset_frame_batch_markers0;
    preset_frame_batch_payload_specs[0].marker_count = 2;
    preset_frame_batch_payload_specs[1].phase = "post";
    preset_frame_batch_payload_specs[1].marker_items = preset_frame_batch_markers1;
    preset_frame_batch_payload_specs[1].marker_count = 1;
    preset_frame_batch_context_specs[0].owner = "sim";
    preset_frame_batch_context_specs[0].frame = 500;
    preset_frame_batch_context_specs[1].owner = "render";
    preset_frame_batch_context_specs[1].frame = 501;
    preset_typed_mixed_batch[0] = runtime_posted_typed_input_combo_event(
        &(RuntimePostedInputEvent){
            .device = "gamepad",
            .code = 73,
            .pressed = true
        },
        &(RuntimePostedInputComboPayloadSpec){
            .score = 17,
            .combo_items = preset_typed_mixed_combo,
            .combo_count = 2
        },
        &(RuntimePostedEventContextSpec){
            .owner = "pad",
            .frame = 910
        });
    preset_typed_mixed_batch[1] = runtime_posted_typed_window_rect_event(
        &(RuntimePostedWindowEvent){
            .event_name = "resize",
            .width = 1720,
            .height = 960,
            .focused = true
        },
        &(RuntimePostedWindowRectPayloadSpec){
            .title = "inspector",
            .rect_items = preset_typed_mixed_rect,
            .rect_count = 2
        },
        &(RuntimePostedEventContextSpec){
            .owner = "dock",
            .frame = 911
        });
    preset_typed_mixed_batch[2] = runtime_posted_typed_frame_marker_event(
        &(RuntimePostedFrameEvent){
            .phase = "render",
            .frame_number = 910,
            .delta_seconds = 0.014
        },
        &(RuntimePostedFrameMarkerPayloadSpec){
            .phase = "present",
            .marker_items = preset_typed_mixed_markers,
            .marker_count = 2
        },
        &(RuntimePostedEventContextSpec){
            .owner = "renderer",
            .frame = 912
        });
    preset_typed_mixed_input_batch = runtime_posted_typed_input_combo_batch(&preset_typed_mixed_batch[0].event.input,
                                                                            &preset_typed_mixed_batch[0].payload.input_combo,
                                                                            &preset_typed_mixed_batch[0].context_spec,
                                                                            1);
    preset_typed_mixed_window_batch = runtime_posted_typed_window_rect_batch(&preset_typed_mixed_batch[1].event.window,
                                                                             &preset_typed_mixed_batch[1].payload.window_rect,
                                                                             &preset_typed_mixed_batch[1].context_spec,
                                                                             1);
    preset_typed_mixed_frame_batch = runtime_posted_typed_frame_marker_batch(&preset_typed_mixed_batch[2].event.frame,
                                                                             &preset_typed_mixed_batch[2].payload.frame_marker,
                                                                             &preset_typed_mixed_batch[2].context_spec,
                                                                             1);
    preset_frame_envelope_input_batch = runtime_posted_typed_input_combo_batch(&preset_typed_mixed_batch[0].event.input,
                                                                               &preset_typed_mixed_batch[0].payload.input_combo,
                                                                               NULL,
                                                                               1);
    preset_frame_envelope_window_batch = runtime_posted_typed_window_rect_batch(&preset_typed_mixed_batch[1].event.window,
                                                                                &preset_typed_mixed_batch[1].payload.window_rect,
                                                                                NULL,
                                                                                1);
    preset_frame_envelope_frame_batch = runtime_posted_typed_frame_marker_batch(&preset_typed_mixed_batch[2].event.frame,
                                                                                &preset_typed_mixed_batch[2].payload.frame_marker,
                                                                                NULL,
                                                                                1);
    preset_frame_envelope_batch = runtime_posted_frame_envelope_batch(&preset_frame_envelope_input_batch,
                                                                      &preset_frame_envelope_window_batch,
                                                                      &preset_frame_envelope_frame_batch,
                                                                      &(RuntimePostedEventContextSpec){
                                                                          .owner = "loop",
                                                                          .frame = 1200
                                                                      });
    runtime_posted_frame_envelope_builder_init(&frame_envelope_builder,
                                               frame_envelope_builder_events,
                                               4);
    runtime_posted_frame_envelope_builder_set_shared_context(&frame_envelope_builder,
                                                             &(RuntimePostedEventContextSpec){
                                                                 .owner = "builder",
                                                                 .frame = 1300
                                                             });
    HOST_PUMP_ASSERT(runtime_posted_frame_envelope_builder_add_input_state_event(
                         &frame_envelope_builder,
                         &(RuntimePostedInputEvent){
                             .device = "mouse",
                             .code = 201,
                             .pressed = true
                         },
                         &(RuntimePostedInputStatePayloadSpec){
                             .label = "drag",
                             .active = true,
                             .score = 0,
                             .coords_items = frame_envelope_builder_coords,
                             .coords_count = 2,
                             .buttons_items = NULL,
                             .button_count = 0
                         },
                         NULL),
                     "frame envelope builder accepts an input event");
    HOST_PUMP_ASSERT(runtime_posted_frame_envelope_builder_add_frame_marker_event(
                         &frame_envelope_builder,
                         &(RuntimePostedFrameEvent){
                             .phase = "simulate",
                             .frame_number = 1201,
                             .delta_seconds = 0.008
                         },
                         &(RuntimePostedFrameMarkerPayloadSpec){
                             .phase = "fixed",
                             .marker_items = frame_envelope_builder_markers,
                             .marker_count = 2
                         },
                         NULL),
                     "frame envelope builder accepts a frame event");
    HOST_PUMP_ASSERT(runtime_posted_frame_envelope_builder_add_window_rect_event(
                         &frame_envelope_builder,
                         &(RuntimePostedWindowEvent){
                             .event_name = "resize",
                             .width = 1280,
                             .height = 720,
                             .focused = true
                         },
                         &(RuntimePostedWindowRectPayloadSpec){
                             .title = "main",
                             .rect_items = frame_envelope_builder_rect,
                             .rect_count = 2
                         },
                         NULL),
                     "frame envelope builder accepts a window event");
    runtime_posted_frame_envelope_heap_builder_init(&frame_envelope_heap_builder);
    runtime_posted_frame_envelope_heap_builder_set_shared_context(&frame_envelope_heap_builder,
                                                                  &(RuntimePostedEventContextSpec){
                                                                      .owner = "heap",
                                                                      .frame = 1400
                                                                  });
    HOST_PUMP_ASSERT(runtime_posted_frame_envelope_heap_builder_add_input_combo_event(
                         &frame_envelope_heap_builder,
                         &(RuntimePostedInputEvent){
                             .device = "keyboard",
                             .code = 301,
                             .pressed = false
                         },
                         &(RuntimePostedInputComboPayloadSpec){
                             .score = 44,
                             .combo_items = frame_envelope_heap_combo,
                             .combo_count = 2
                         },
                         NULL),
                     "frame envelope heap builder accepts an input event");
    HOST_PUMP_ASSERT(runtime_posted_frame_envelope_heap_builder_add_frame_marker_event(
                         &frame_envelope_heap_builder,
                         &(RuntimePostedFrameEvent){
                             .phase = "render",
                             .frame_number = 1401,
                             .delta_seconds = 0.011
                         },
                         &(RuntimePostedFrameMarkerPayloadSpec){
                             .phase = "present",
                             .marker_items = frame_envelope_heap_markers,
                             .marker_count = 2
                         },
                         NULL),
                     "frame envelope heap builder accepts a frame event");
    HOST_PUMP_ASSERT(runtime_posted_frame_envelope_heap_builder_add_window_rect_event(
                         &frame_envelope_heap_builder,
                         &(RuntimePostedWindowEvent){
                             .event_name = "move",
                             .width = 900,
                             .height = 700,
                             .focused = false
                         },
                         &(RuntimePostedWindowRectPayloadSpec){
                             .title = "aux",
                             .rect_items = frame_envelope_heap_rect,
                             .rect_count = 2
                         },
                         NULL),
                     "frame envelope heap builder accepts a window event");
    HOST_PUMP_ASSERT(frame_envelope_heap_builder.count == 3,
                     "frame envelope heap builder tracks appended event count");
    HOST_PUMP_ASSERT(frame_envelope_heap_builder.capacity >= 3,
                     "frame envelope heap builder grows storage as events are appended");

    mixed_batch[0].kind = RUNTIME_POSTED_EVENT_INPUT;
    mixed_batch[0].as.input.device = "gamepad";
    mixed_batch[0].as.input.code = 21;
    mixed_batch[0].as.input.pressed = true;
    mixed_batch[0].as.input.meta_override = NULL;
    mixed_batch[1].kind = RUNTIME_POSTED_EVENT_WINDOW;
    mixed_batch[1].as.window.event_name = "focus";
    mixed_batch[1].as.window.width = 1024;
    mixed_batch[1].as.window.height = 768;
    mixed_batch[1].as.window.focused = true;
    mixed_batch[1].as.window.meta_override = NULL;
    mixed_batch[2].kind = RUNTIME_POSTED_EVENT_FRAME;
    mixed_batch[2].as.frame.phase = "present";
    mixed_batch[2].as.frame.frame_number = 88;
    mixed_batch[2].as.frame.delta_seconds = 0.5;
    mixed_batch[2].as.frame.meta_override = NULL;

    custom_mixed_batch[0].kind = RUNTIME_POSTED_EVENT_INPUT;
    custom_mixed_batch[0].as.input.device = "touch";
    custom_mixed_batch[0].as.input.code = 55;
    custom_mixed_batch[0].as.input.pressed = true;
    custom_mixed_batch[0].as.input.meta_override = &custom_mixed_input_meta;
    custom_mixed_batch[1].kind = RUNTIME_POSTED_EVENT_WINDOW;
    custom_mixed_batch[1].as.window.event_name = "minimize";
    custom_mixed_batch[1].as.window.width = 1440;
    custom_mixed_batch[1].as.window.height = 900;
    custom_mixed_batch[1].as.window.focused = false;
    custom_mixed_batch[1].as.window.meta_override = &custom_mixed_window_meta;
    custom_mixed_batch[2].kind = RUNTIME_POSTED_EVENT_FRAME;
    custom_mixed_batch[2].as.frame.phase = "physics";
    custom_mixed_batch[2].as.frame.frame_number = 144;
    custom_mixed_batch[2].as.frame.delta_seconds = 0.016;
    custom_mixed_batch[2].as.frame.meta_override = &custom_mixed_frame_meta;

    extra_mixed_batch[0].kind = RUNTIME_POSTED_EVENT_INPUT;
    extra_mixed_batch[0].as.input.device = "keyboard";
    extra_mixed_batch[0].as.input.code = 91;
    extra_mixed_batch[0].as.input.pressed = false;
    extra_mixed_batch[0].as.input.meta_override = NULL;
    extra_mixed_batch[0].as.input.extra_fields = extra_mixed_input_fields;
    extra_mixed_batch[0].as.input.extra_field_count = 1;
    extra_mixed_batch[1].kind = RUNTIME_POSTED_EVENT_WINDOW;
    extra_mixed_batch[1].as.window.event_name = "overlay";
    extra_mixed_batch[1].as.window.width = 1600;
    extra_mixed_batch[1].as.window.height = 900;
    extra_mixed_batch[1].as.window.focused = true;
    extra_mixed_batch[1].as.window.meta_override = NULL;
    extra_mixed_batch[1].as.window.extra_fields = extra_mixed_window_fields;
    extra_mixed_batch[1].as.window.extra_field_count = 1;
    extra_mixed_batch[2].kind = RUNTIME_POSTED_EVENT_FRAME;
    extra_mixed_batch[2].as.frame.phase = "tick";
    extra_mixed_batch[2].as.frame.frame_number = 233;
    extra_mixed_batch[2].as.frame.delta_seconds = 0.033;
    extra_mixed_batch[2].as.frame.meta_override = NULL;
    extra_mixed_batch[2].as.frame.extra_fields = extra_mixed_frame_fields;
    extra_mixed_batch[2].as.frame.extra_field_count = 2;

    structured_mixed_batch[0].kind = RUNTIME_POSTED_EVENT_INPUT;
    structured_mixed_batch[0].as.input.device = "controller";
    structured_mixed_batch[0].as.input.code = 5;
    structured_mixed_batch[0].as.input.pressed = true;
    structured_mixed_batch[0].as.input.meta_override = NULL;
    structured_mixed_batch[0].as.input.extra_fields = structured_mixed_input_fields;
    structured_mixed_batch[0].as.input.extra_field_count = 1;
    structured_mixed_batch[1].kind = RUNTIME_POSTED_EVENT_WINDOW;
    structured_mixed_batch[1].as.window.event_name = "viewport";
    structured_mixed_batch[1].as.window.width = 1600;
    structured_mixed_batch[1].as.window.height = 900;
    structured_mixed_batch[1].as.window.focused = true;
    structured_mixed_batch[1].as.window.meta_override = NULL;
    structured_mixed_batch[1].as.window.extra_fields = structured_mixed_window_fields;
    structured_mixed_batch[1].as.window.extra_field_count = 1;
    structured_mixed_batch[2].kind = RUNTIME_POSTED_EVENT_FRAME;
    structured_mixed_batch[2].as.frame.phase = "step";
    structured_mixed_batch[2].as.frame.frame_number = 301;
    structured_mixed_batch[2].as.frame.delta_seconds = 0.008;
    structured_mixed_batch[2].as.frame.meta_override = NULL;
    structured_mixed_batch[2].as.frame.extra_fields = structured_mixed_frame_fields;
    structured_mixed_batch[2].as.frame.extra_field_count = 1;

    mapped_mixed_batch[0].kind = RUNTIME_POSTED_EVENT_INPUT;
    mapped_mixed_batch[0].as.input.device = "gamepad";
    mapped_mixed_batch[0].as.input.code = 88;
    mapped_mixed_batch[0].as.input.pressed = false;
    mapped_mixed_batch[0].as.input.meta_override = NULL;
    mapped_mixed_batch[0].as.input.extra_fields = mapped_mixed_input_fields;
    mapped_mixed_batch[0].as.input.extra_field_count = 1;
    mapped_mixed_batch[1].kind = RUNTIME_POSTED_EVENT_WINDOW;
    mapped_mixed_batch[1].as.window.event_name = "move";
    mapped_mixed_batch[1].as.window.width = 1920;
    mapped_mixed_batch[1].as.window.height = 1080;
    mapped_mixed_batch[1].as.window.focused = true;
    mapped_mixed_batch[1].as.window.meta_override = NULL;
    mapped_mixed_batch[1].as.window.extra_fields = mapped_mixed_window_fields;
    mapped_mixed_batch[1].as.window.extra_field_count = 1;
    mapped_mixed_batch[2].kind = RUNTIME_POSTED_EVENT_FRAME;
    mapped_mixed_batch[2].as.frame.phase = "simulate";
    mapped_mixed_batch[2].as.frame.frame_number = 410;
    mapped_mixed_batch[2].as.frame.delta_seconds = 0.017;
    mapped_mixed_batch[2].as.frame.meta_override = NULL;
    mapped_mixed_batch[2].as.frame.extra_fields = mapped_mixed_frame_fields;
    mapped_mixed_batch[2].as.frame.extra_field_count = 1;

    HOST_PUMP_ASSERT(rt != NULL, "runtime_create_with_options returns Runtime");
    HOST_PUMP_ASSERT(rt && !runtime_has_error(rt), "runtime_create_with_options has no load error");
    if (!rt || runtime_has_error(rt)) {
        if (rt) runtime_free(rt);
        return;
    }
    runtime_host_event_loop_session_init(&host_event_loop_session, rt);

    HOST_PUMP_ASSERT(runtime_get_load_mode(rt) == expected_mode, "load mode matches expectation");
    HOST_PUMP_ASSERT(runtime_get_posted_callback_auto_drain(rt), "auto-drain defaults to enabled");

    previous_auto_drain = runtime_set_posted_callback_auto_drain(rt, false);
    HOST_PUMP_ASSERT(previous_auto_drain, "disabling auto-drain reports previous enabled state");
    HOST_PUMP_ASSERT(!runtime_get_posted_callback_auto_drain(rt), "auto-drain can be disabled by host");
    HOST_PUMP_ASSERT(!runtime_has_posted_callbacks(rt), "host availability probe starts empty");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 0, "pending count starts empty");
    HOST_PUMP_ASSERT(!runtime_wait_for_posted_callbacks(rt, 10), "non-draining host wait times out cleanly with no work");

    HOST_PUMP_ASSERT(runtime_run(rt) == 0, "main executes successfully");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "main reports no runtime error");
    HOST_PUMP_ASSERT(!runtime_has_posted_callbacks(rt), "main leaves no queued callback work");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 0, "main leaves no pending posted callbacks");

    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "spawnAndJoinPostedWorker", &value),
                     "spawnAndJoinPostedWorker returns an int");
    HOST_PUMP_ASSERT(value == 100, "worker join returns pre-drain counter value");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "host availability probe sees queued callback work");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 1, "host sees one queued posted callback");

    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostCounter", &value),
                     "getHostCounter returns an int before manual drain");
    HOST_PUMP_ASSERT(value == 100, "counter remains unchanged before host drain");

    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "host drain processes exactly one posted callback");
    if (runtime_has_error(rt)) {
        fprintf(stderr, "%s: runtime error after host drain: %s\n",
                label_prefix,
                runtime_get_error(rt) ? runtime_get_error(rt) : "(no runtime error)");
    }
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "host drain leaves no runtime error");
    HOST_PUMP_ASSERT(!runtime_has_posted_callbacks(rt), "host availability probe clears after manual drain");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 0, "host drain clears the posted callback queue");

    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostCounter", &value),
                     "getHostCounter returns an int after manual drain");
    HOST_PUMP_ASSERT(value == 123, "counter reflects the manually drained callback");

    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 0, "host drain reports no extra posted callbacks");

    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "spawnAndJoinPostedWorker", &value),
                     "spawnAndJoinPostedWorker returns an int before budget drain");
    HOST_PUMP_ASSERT(value == 123, "worker join returns current counter value before budget drain");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 1, "host sees one queued posted callback before budget drain");

    drained = runtime_drain_posted_callbacks_for_ms(rt, 1, 50);
    HOST_PUMP_ASSERT(drained == 1, "budget drain processes exactly one posted callback");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "budget drain leaves no runtime error");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 0, "budget drain clears the posted callback queue");

    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostCounter", &value),
                     "getHostCounter returns an int after budget drain");
    HOST_PUMP_ASSERT(value == 146, "counter reflects the budget-drained callback");

    drained = runtime_drain_posted_callbacks_for_ms(rt, 0, 50);
    HOST_PUMP_ASSERT(drained == 0, "budget drain with zero max callbacks is a no-op");

    drained = runtime_wait_and_drain_posted_callbacks(rt, 1, 10);
    HOST_PUMP_ASSERT(drained == 0, "blocking drain with no queued callback times out cleanly");

    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "startDelayedPostedWorker", &value),
                     "startDelayedPostedWorker returns an int");
    HOST_PUMP_ASSERT(value == 146, "starting delayed worker leaves counter unchanged before blocking drain");
    HOST_PUMP_ASSERT(!runtime_has_posted_callbacks(rt), "delayed worker does not report work before posting");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 0, "delayed worker does not post immediately");
    HOST_PUMP_ASSERT(runtime_wait_for_posted_callbacks(rt, 200), "non-draining host wait observes queued callback work");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "host availability probe stays true until drain");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 1, "non-draining host wait does not consume queued callbacks");

    drained = runtime_wait_and_drain_posted_callbacks(rt, 1, 200);
    HOST_PUMP_ASSERT(drained == 1, "blocking drain waits for and processes one posted callback");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "blocking drain leaves no runtime error");
    HOST_PUMP_ASSERT(!runtime_has_posted_callbacks(rt), "blocking drain clears host availability probe");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 0, "blocking drain leaves no queued callbacks");

    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostCounter", &value),
                     "getHostCounter returns an int after blocking drain");
    HOST_PUMP_ASSERT(value == 177, "counter reflects the blocking-drained callback");

    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "joinDelayedPostedWorker", &value),
                     "joinDelayedPostedWorker returns an int");
    HOST_PUMP_ASSERT(value == 1, "joinDelayedPostedWorker reports success");

    HOST_PUMP_ASSERT(!runtime_host_event_loop_session_has_posted_callbacks(&host_event_loop_session),
                     "event loop session starts with no queued callback work");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_posted_callback_pending_count(&host_event_loop_session) == 0,
                     "event loop session pending count starts empty");

    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "startDelayedPostedWorker", &value),
                     "startDelayedPostedWorker returns an int before session wait");
    HOST_PUMP_ASSERT(value == 177, "starting delayed worker leaves counter unchanged before session drain");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_wait_for_posted_callbacks(&host_event_loop_session, 200),
                     "event loop session wait observes queued callback work");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_has_posted_callbacks(&host_event_loop_session),
                     "event loop session availability stays true until drain");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_posted_callback_pending_count(&host_event_loop_session) == 1,
                     "event loop session sees one pending callback");
    drained = runtime_host_event_loop_session_drain_posted_callbacks_for_ms(&host_event_loop_session, 1, 50);
    HOST_PUMP_ASSERT(drained == 1, "event loop session timed drain processes exactly one callback");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "event loop session timed drain leaves no runtime error");
    HOST_PUMP_ASSERT(!runtime_host_event_loop_session_has_posted_callbacks(&host_event_loop_session),
                     "event loop session timed drain clears queued callback work");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostCounter", &value),
                     "getHostCounter returns an int after session timed drain");
    HOST_PUMP_ASSERT(value == 208, "counter reflects the session-drained callback");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "joinDelayedPostedWorker", &value),
                     "joinDelayedPostedWorker returns an int after session drain");
    HOST_PUMP_ASSERT(value == 1, "joinDelayedPostedWorker reports success after session drain");

    runtime_host_event_loop_session_begin_frame(&host_event_loop_session,
                                                &(RuntimePostedEventContextSpec){
                                                    .owner = "session",
                                                    .frame = 1500
                                                });
    HOST_PUMP_ASSERT(host_event_loop_session.frame_builder.count == 0,
                     "event loop session begin_frame clears accumulated events");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_input_state_event(
                         &host_event_loop_session,
                         &(RuntimePostedInputEvent){
                             .device = "touch",
                             .code = 401,
                             .pressed = true
                         },
                         &(RuntimePostedInputStatePayloadSpec){
                             .label = "session",
                             .active = true,
                             .score = 0,
                             .coords_items = event_loop_session_coords,
                             .coords_count = 2,
                             .buttons_items = NULL,
                             .button_count = 0
                         },
                         NULL),
                     "event loop session accepts an input event");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_frame_marker_event(
                         &host_event_loop_session,
                         &(RuntimePostedFrameEvent){
                             .phase = "update",
                             .frame_number = 1501,
                             .delta_seconds = 0.012
                         },
                         &(RuntimePostedFrameMarkerPayloadSpec){
                             .phase = "sim",
                             .marker_items = event_loop_session_markers,
                             .marker_count = 2
                         },
                         NULL),
                     "event loop session accepts a frame event");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_window_rect_event(
                         &host_event_loop_session,
                         &(RuntimePostedWindowEvent){
                             .event_name = "focus",
                             .width = 1024,
                             .height = 768,
                             .focused = true
                         },
                         &(RuntimePostedWindowRectPayloadSpec){
                             .title = "session-window",
                             .rect_items = event_loop_session_rect,
                             .rect_count = 2
                         },
                         NULL),
                     "event loop session accepts a window event");
    HOST_PUMP_ASSERT(host_event_loop_session.frame_builder.count == 3,
                     "event loop session tracks the current frame event count");
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_flush_frame(&host_event_loop_session,
                                                                 "onHostEventLoopSessionFrame",
                                                                 error_buf,
                                                                 sizeof(error_buf)),
                     "event loop session flush queues one frame callback");
    HOST_PUMP_ASSERT(host_event_loop_session.frame_builder.count == 0,
                     "event loop session flush clears accumulated frame events");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_has_posted_callbacks(&host_event_loop_session),
                     "event loop session flush reports queued callback work");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_posted_callback_pending_count(&host_event_loop_session) == 1,
                     "event loop session flush increments pending callback count");
    drained = runtime_host_event_loop_session_drain_posted_callbacks(&host_event_loop_session, 1);
    HOST_PUMP_ASSERT(drained == 1, "event loop session drain processes the queued frame callback");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "event loop session frame drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostEventLoopSessionFrameHits", &value),
                     "getHostEventLoopSessionFrameHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "event loop session frame callback increments the expected hit counter");

    runtime_host_event_loop_session_begin_frame(&host_event_loop_session,
                                                &(RuntimePostedEventContextSpec){
                                                    .owner = "session",
                                                    .frame = 1500
                                                });
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_input_state_event(
                         &host_event_loop_session,
                         &(RuntimePostedInputEvent){
                             .device = "touch",
                             .code = 401,
                             .pressed = true
                         },
                         &(RuntimePostedInputStatePayloadSpec){
                             .label = "session",
                             .active = true,
                             .score = 0,
                             .coords_items = event_loop_session_coords,
                             .coords_count = 2,
                             .buttons_items = NULL,
                             .button_count = 0
                         },
                         NULL),
                     "event loop session flush-and-drain helper accepts an input event");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_frame_marker_event(
                         &host_event_loop_session,
                         &(RuntimePostedFrameEvent){
                             .phase = "update",
                             .frame_number = 1501,
                             .delta_seconds = 0.012
                         },
                         &(RuntimePostedFrameMarkerPayloadSpec){
                             .phase = "sim",
                             .marker_items = event_loop_session_markers,
                             .marker_count = 2
                         },
                         NULL),
                     "event loop session flush-and-drain helper accepts a frame event");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_window_rect_event(
                         &host_event_loop_session,
                         &(RuntimePostedWindowEvent){
                             .event_name = "focus",
                             .width = 1024,
                             .height = 768,
                             .focused = true
                         },
                         &(RuntimePostedWindowRectPayloadSpec){
                             .title = "session-window",
                             .rect_items = event_loop_session_rect,
                             .rect_count = 2
                         },
                         NULL),
                     "event loop session flush-and-drain helper accepts a window event");
    error_buf[0] = '\0';
    drained = 0;
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_flush_frame_and_drain_posted_callbacks_for_ms(
                         &host_event_loop_session,
                         "onHostEventLoopSessionFrame",
                         1,
                         50,
                         &drained,
                         error_buf,
                         sizeof(error_buf)),
                     "event loop session flush-and-drain helper queues and processes one frame callback");
    HOST_PUMP_ASSERT(drained == 1, "event loop session flush-and-drain helper drains exactly one queued callback");
    HOST_PUMP_ASSERT(!runtime_host_event_loop_session_has_posted_callbacks(&host_event_loop_session),
                     "event loop session flush-and-drain helper leaves no queued callback work");
    HOST_PUMP_ASSERT(host_event_loop_session.frame_builder.count == 0,
                     "event loop session flush-and-drain helper clears accumulated frame events");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostEventLoopSessionFrameHits", &value),
                     "getHostEventLoopSessionFrameHits returns an int after flush-and-drain");
    HOST_PUMP_ASSERT(value == 2, "event loop session flush-and-drain helper increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_configure_end_frame(&host_event_loop_session,
                                                                         "onHostEventLoopSessionFrame",
                                                                         1,
                                                                         50,
                                                                         error_buf,
                                                                         sizeof(error_buf)),
                     "event loop session end-frame configuration accepts callback defaults");
    runtime_host_event_loop_session_begin_frame(&host_event_loop_session,
                                                &(RuntimePostedEventContextSpec){
                                                    .owner = "session",
                                                    .frame = 1500
                                                });
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_input_state_event(
                         &host_event_loop_session,
                         &(RuntimePostedInputEvent){
                             .device = "touch",
                             .code = 401,
                             .pressed = true
                         },
                         &(RuntimePostedInputStatePayloadSpec){
                             .label = "session",
                             .active = true,
                             .score = 0,
                             .coords_items = event_loop_session_coords,
                             .coords_count = 2,
                             .buttons_items = NULL,
                             .button_count = 0
                         },
                         NULL),
                     "event loop session end-frame configuration accepts an input event");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_frame_marker_event(
                         &host_event_loop_session,
                         &(RuntimePostedFrameEvent){
                             .phase = "update",
                             .frame_number = 1501,
                             .delta_seconds = 0.012
                         },
                         &(RuntimePostedFrameMarkerPayloadSpec){
                             .phase = "sim",
                             .marker_items = event_loop_session_markers,
                             .marker_count = 2
                         },
                         NULL),
                     "event loop session end-frame configuration accepts a frame event");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_window_rect_event(
                         &host_event_loop_session,
                         &(RuntimePostedWindowEvent){
                             .event_name = "focus",
                             .width = 1024,
                             .height = 768,
                             .focused = true
                         },
                         &(RuntimePostedWindowRectPayloadSpec){
                             .title = "session-window",
                             .rect_items = event_loop_session_rect,
                             .rect_count = 2
                         },
                         NULL),
                     "event loop session end-frame configuration accepts a window event");
    error_buf[0] = '\0';
    drained = 0;
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_end_frame(&host_event_loop_session,
                                                               &drained,
                                                               error_buf,
                                                               sizeof(error_buf)),
                     "event loop session end-frame helper queues and processes one frame callback");
    HOST_PUMP_ASSERT(drained == 1, "event loop session end-frame helper drains exactly one queued callback");
    HOST_PUMP_ASSERT(!runtime_host_event_loop_session_has_posted_callbacks(&host_event_loop_session),
                     "event loop session end-frame helper leaves no queued callback work");
    HOST_PUMP_ASSERT(host_event_loop_session.frame_builder.count == 0,
                     "event loop session end-frame helper clears accumulated frame events");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostEventLoopSessionFrameHits", &value),
                     "getHostEventLoopSessionFrameHits returns an int after end-frame helper");
    HOST_PUMP_ASSERT(value == 3, "event loop session end-frame helper increments the expected hit counter");

    error_buf[0] = '\0';
    drained = -1;
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_tick(&host_event_loop_session,
                                                          false,
                                                          0,
                                                          &drained,
                                                          error_buf,
                                                          sizeof(error_buf)),
                     "event loop session tick helper succeeds with no queued callback work");
    HOST_PUMP_ASSERT(drained == 0, "event loop session tick helper drains nothing when the queue is empty");

    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "startDelayedPostedWorker", &value),
                     "startDelayedPostedWorker returns an int before session tick");
    HOST_PUMP_ASSERT(value == 208, "starting delayed worker leaves counter unchanged before session tick");
    error_buf[0] = '\0';
    drained = 0;
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_tick(&host_event_loop_session,
                                                          true,
                                                          200,
                                                          &drained,
                                                          error_buf,
                                                          sizeof(error_buf)),
                     "event loop session tick helper waits for and processes queued callback work");
    HOST_PUMP_ASSERT(drained == 1, "event loop session tick helper drains exactly one queued callback");
    HOST_PUMP_ASSERT(!runtime_host_event_loop_session_has_posted_callbacks(&host_event_loop_session),
                     "event loop session tick helper leaves no queued callback work");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostCounter", &value),
                     "getHostCounter returns an int after session tick");
    HOST_PUMP_ASSERT(value == 239, "counter reflects the session tick-drained callback");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "joinDelayedPostedWorker", &value),
                     "joinDelayedPostedWorker returns an int after session tick");
    HOST_PUMP_ASSERT(value == 1, "joinDelayedPostedWorker reports success after session tick");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_configure_end_frame(&host_event_loop_session,
                                                                         "onHostEventLoopSessionFrame",
                                                                         2,
                                                                         50,
                                                                         error_buf,
                                                                         sizeof(error_buf)),
                     "event loop session tick presets accept updated drain defaults");
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_configure_tick(&host_event_loop_session,
                                                                    RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_NO_WAIT,
                                                                    0,
                                                                    error_buf,
                                                                    sizeof(error_buf)),
                     "event loop session no-wait tick preset configures successfully");
    drained = -1;
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_tick_default(&host_event_loop_session,
                                                                  &drained,
                                                                  error_buf,
                                                                  sizeof(error_buf)),
                     "event loop session no-wait tick preset succeeds with no queued callback work");
    HOST_PUMP_ASSERT(drained == 0, "event loop session no-wait tick preset drains nothing when the queue is empty");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event(rt,
                                              "onHostSessionTickInputEvent",
                                              &session_tick_input_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "event loop session tick preset queues the first dedicated input callback");
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event(rt,
                                              "onHostSessionTickInputEvent",
                                              &session_tick_input_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "event loop session tick preset queues the second dedicated input callback");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_posted_callback_pending_count(&host_event_loop_session) == 2,
                     "event loop session tick preset sees two queued callbacks before wait mode");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_configure_tick(&host_event_loop_session,
                                                                    RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_WAIT,
                                                                    200,
                                                                    error_buf,
                                                                    sizeof(error_buf)),
                     "event loop session wait tick preset configures successfully");
    drained = 0;
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_tick_default(&host_event_loop_session,
                                                                  &drained,
                                                                  error_buf,
                                                                  sizeof(error_buf)),
                     "event loop session wait tick preset processes queued callback work");
    HOST_PUMP_ASSERT(drained == 1, "event loop session wait tick preset drains exactly one queued callback");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_posted_callback_pending_count(&host_event_loop_session) == 1,
                     "event loop session wait tick preset leaves one queued callback for later pumping");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostSessionTickInputHits", &value),
                     "getHostSessionTickInputHits returns an int after wait tick preset");
    HOST_PUMP_ASSERT(value == 1, "event loop session wait tick preset increments the dedicated hit counter once");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event(rt,
                                              "onHostSessionTickInputEvent",
                                              &session_tick_input_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "event loop session wait-and-pump preset queues an additional dedicated input callback");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_posted_callback_pending_count(&host_event_loop_session) == 2,
                     "event loop session wait-and-pump preset sees two queued callbacks before pumping");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_configure_tick(
                         &host_event_loop_session,
                         RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_WAIT_AND_PUMP_UNTIL_BUDGET,
                         200,
                         error_buf,
                         sizeof(error_buf)),
                     "event loop session wait-and-pump tick preset configures successfully");
    drained = 0;
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_tick_default(&host_event_loop_session,
                                                                  &drained,
                                                                  error_buf,
                                                                  sizeof(error_buf)),
                     "event loop session wait-and-pump tick preset processes queued callback work");
    HOST_PUMP_ASSERT(drained == 2, "event loop session wait-and-pump tick preset drains up to the stored budget");
    HOST_PUMP_ASSERT(!runtime_host_event_loop_session_has_posted_callbacks(&host_event_loop_session),
                     "event loop session wait-and-pump tick preset leaves no queued callback work");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostSessionTickInputHits", &value),
                     "getHostSessionTickInputHits returns an int after wait-and-pump tick preset");
    HOST_PUMP_ASSERT(value == 3, "event loop session wait-and-pump tick preset drains the remaining queued callbacks");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_configure_tick(&host_event_loop_session,
                                                                    RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_NO_WAIT,
                                                                    0,
                                                                    error_buf,
                                                                    sizeof(error_buf)),
                     "event loop session step helper accepts a no-wait tick preset");
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event(rt,
                                              "onHostSessionTickInputEvent",
                                              &session_tick_input_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "event loop session step helper queues a dedicated input callback");
    memset(&session_step_result, 0, sizeof(session_step_result));
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_step(&host_event_loop_session,
                                                          false,
                                                          &session_step_result,
                                                          error_buf,
                                                          sizeof(error_buf)),
                     "event loop session step helper processes a tick-only iteration");
    HOST_PUMP_ASSERT(!session_step_result.frame_posted,
                     "event loop session step helper reports that no frame was posted during a tick-only iteration");
    HOST_PUMP_ASSERT(session_step_result.callbacks_drained == 1,
                     "event loop session step helper reports one drained callback during a tick-only iteration");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostSessionTickInputHits", &value),
                     "getHostSessionTickInputHits returns an int after the session step tick path");
    HOST_PUMP_ASSERT(value == 4, "event loop session step helper increments the dedicated tick hit counter");

    runtime_host_event_loop_session_begin_frame(&host_event_loop_session,
                                                &(RuntimePostedEventContextSpec){
                                                    .owner = "session",
                                                    .frame = 1500
                                                });
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_input_state_event(
                         &host_event_loop_session,
                         &(RuntimePostedInputEvent){
                             .device = "touch",
                             .code = 401,
                             .pressed = true
                         },
                         &(RuntimePostedInputStatePayloadSpec){
                             .label = "session",
                             .active = true,
                             .score = 0,
                             .coords_items = event_loop_session_coords,
                             .coords_count = 2,
                             .buttons_items = NULL,
                             .button_count = 0
                         },
                         NULL),
                     "event loop session step helper accepts an input event");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_frame_marker_event(
                         &host_event_loop_session,
                         &(RuntimePostedFrameEvent){
                             .phase = "update",
                             .frame_number = 1501,
                             .delta_seconds = 0.012
                         },
                         &(RuntimePostedFrameMarkerPayloadSpec){
                             .phase = "sim",
                             .marker_items = event_loop_session_markers,
                             .marker_count = 2
                         },
                         NULL),
                     "event loop session step helper accepts a frame event");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_window_rect_event(
                         &host_event_loop_session,
                         &(RuntimePostedWindowEvent){
                             .event_name = "focus",
                             .width = 1024,
                             .height = 768,
                             .focused = true
                         },
                         &(RuntimePostedWindowRectPayloadSpec){
                             .title = "session-window",
                             .rect_items = event_loop_session_rect,
                             .rect_count = 2
                         },
                         NULL),
                     "event loop session step helper accepts a window event");
    memset(&session_step_result, 0, sizeof(session_step_result));
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_step(&host_event_loop_session,
                                                          true,
                                                          &session_step_result,
                                                          error_buf,
                                                          sizeof(error_buf)),
                     "event loop session step helper processes an end-frame iteration");
    HOST_PUMP_ASSERT(session_step_result.frame_posted,
                     "event loop session step helper reports that a frame was posted during an end-frame iteration");
    HOST_PUMP_ASSERT(session_step_result.callbacks_drained == 1,
                     "event loop session step helper reports one drained callback during an end-frame iteration");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostEventLoopSessionFrameHits", &value),
                     "getHostEventLoopSessionFrameHits returns an int after the session step frame path");
    HOST_PUMP_ASSERT(value == 4, "event loop session step helper increments the session frame hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_configure_tick(&host_event_loop_session,
                                                                    RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_NO_WAIT,
                                                                    0,
                                                                    error_buf,
                                                                    sizeof(error_buf)),
                     "event loop session step options accept a no-wait tick preset");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "startDelayedPostedWorker", &value),
                     "startDelayedPostedWorker returns an int before the step-options wait override");
    HOST_PUMP_ASSERT(value == 239, "starting delayed worker leaves counter unchanged before the step-options wait override");
    memset(&session_step_options, 0, sizeof(session_step_options));
    session_step_options.has_wait_timeout_millis = true;
    session_step_options.wait_timeout_millis = 200;
    memset(&session_step_result, 0, sizeof(session_step_result));
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_step_with_options(&host_event_loop_session,
                                                                       false,
                                                                       &session_step_options,
                                                                       &session_step_result,
                                                                       error_buf,
                                                                       sizeof(error_buf)),
                     "event loop session step options can force a wait timeout for one iteration");
    HOST_PUMP_ASSERT(!session_step_result.frame_posted,
                     "event loop session step options wait override does not post a frame");
    HOST_PUMP_ASSERT(session_step_result.callbacks_drained == 1,
                     "event loop session step options wait override drains exactly one delayed callback");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostCounter", &value),
                     "getHostCounter returns an int after the step-options wait override");
    HOST_PUMP_ASSERT(value == 270, "counter reflects the step-options wait override callback");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "joinDelayedPostedWorker", &value),
                     "joinDelayedPostedWorker returns an int after the step-options wait override");
    HOST_PUMP_ASSERT(value == 1, "joinDelayedPostedWorker reports success after the step-options wait override");

    runtime_host_event_loop_session_begin_frame(&host_event_loop_session,
                                                &(RuntimePostedEventContextSpec){
                                                    .owner = "session",
                                                    .frame = 1500
                                                });
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_input_state_event(
                         &host_event_loop_session,
                         &(RuntimePostedInputEvent){
                             .device = "touch",
                             .code = 401,
                             .pressed = true
                         },
                         &(RuntimePostedInputStatePayloadSpec){
                             .label = "session",
                             .active = true,
                             .score = 0,
                             .coords_items = event_loop_session_coords,
                             .coords_count = 2,
                             .buttons_items = NULL,
                             .button_count = 0
                         },
                         NULL),
                     "event loop session step options accept an input event for the frame-skip path");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_frame_marker_event(
                         &host_event_loop_session,
                         &(RuntimePostedFrameEvent){
                             .phase = "update",
                             .frame_number = 1501,
                             .delta_seconds = 0.012
                         },
                         &(RuntimePostedFrameMarkerPayloadSpec){
                             .phase = "sim",
                             .marker_items = event_loop_session_markers,
                             .marker_count = 2
                         },
                         NULL),
                     "event loop session step options accept a frame event for the frame-skip path");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_add_window_rect_event(
                         &host_event_loop_session,
                         &(RuntimePostedWindowEvent){
                             .event_name = "focus",
                             .width = 1024,
                             .height = 768,
                             .focused = true
                         },
                         &(RuntimePostedWindowRectPayloadSpec){
                             .title = "session-window",
                             .rect_items = event_loop_session_rect,
                             .rect_count = 2
                         },
                         NULL),
                     "event loop session step options accept a window event for the frame-skip path");
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event(rt,
                                              "onHostSessionTickInputEvent",
                                              &session_tick_input_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "event loop session step options queue one dedicated tick callback before skipping frame post");
    memset(&session_step_options, 0, sizeof(session_step_options));
    session_step_options.has_post_frame = true;
    session_step_options.post_frame = false;
    memset(&session_step_result, 0, sizeof(session_step_result));
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_step_with_options(&host_event_loop_session,
                                                                       true,
                                                                       &session_step_options,
                                                                       &session_step_result,
                                                                       error_buf,
                                                                       sizeof(error_buf)),
                     "event loop session step options can skip frame posting for one iteration");
    HOST_PUMP_ASSERT(!session_step_result.frame_posted,
                     "event loop session step options skip override reports that no frame was posted");
    HOST_PUMP_ASSERT(session_step_result.callbacks_drained == 1,
                     "event loop session step options skip override drains the queued tick callback");
    HOST_PUMP_ASSERT(host_event_loop_session.frame_builder.count == 3,
                     "event loop session step options skip override leaves the accumulated frame intact");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostSessionTickInputHits", &value),
                     "getHostSessionTickInputHits returns an int after the frame-skip override");
    HOST_PUMP_ASSERT(value == 5, "event loop session step options skip override drains exactly one dedicated tick callback");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostEventLoopSessionFrameHits", &value),
                     "getHostEventLoopSessionFrameHits returns an int after the frame-skip override");
    HOST_PUMP_ASSERT(value == 4, "event loop session step options skip override does not post the accumulated frame");
    memset(&session_step_options, 0, sizeof(session_step_options));
    session_step_options.has_frame_callback_name = true;
    session_step_options.frame_callback_name = "onHostEventLoopSessionFrameAlt";
    memset(&session_step_result, 0, sizeof(session_step_result));
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_step_with_options(&host_event_loop_session,
                                                                       true,
                                                                       &session_step_options,
                                                                       &session_step_result,
                                                                       error_buf,
                                                                       sizeof(error_buf)),
                     "event loop session step options can redirect the frame callback for one iteration");
    HOST_PUMP_ASSERT(session_step_result.frame_posted,
                     "event loop session step options frame callback override reports that a frame was posted");
    HOST_PUMP_ASSERT(session_step_result.callbacks_drained == 1,
                     "event loop session step options frame callback override drains the redirected frame callback");
    HOST_PUMP_ASSERT(host_event_loop_session.frame_builder.count == 0,
                     "event loop session step options frame callback override clears the accumulated frame");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostEventLoopSessionFrameHits", &value),
                     "getHostEventLoopSessionFrameHits returns an int after the frame callback override");
    HOST_PUMP_ASSERT(value == 4, "event loop session step options frame callback override leaves the default frame callback untouched");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostEventLoopSessionAltFrameHits", &value),
                     "getHostEventLoopSessionAltFrameHits returns an int after the frame callback override");
    HOST_PUMP_ASSERT(value == 1, "event loop session step options frame callback override routes one frame to the alternate callback");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_configure_tick(
                         &host_event_loop_session,
                         RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_WAIT_AND_PUMP_UNTIL_BUDGET,
                         200,
                         error_buf,
                         sizeof(error_buf)),
                     "event loop session step options accept a wait-and-pump tick preset");
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event(rt,
                                              "onHostSessionTickInputEvent",
                                              &session_tick_input_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "event loop session step options queue the first capped-drain callback");
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event(rt,
                                              "onHostSessionTickInputEvent",
                                              &session_tick_input_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "event loop session step options queue the second capped-drain callback");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_posted_callback_pending_count(&host_event_loop_session) == 2,
                     "event loop session step options see two queued callbacks before the capped drain");
    memset(&session_step_options, 0, sizeof(session_step_options));
    session_step_options.has_max_callbacks = true;
    session_step_options.max_callbacks = 1;
    memset(&session_step_result, 0, sizeof(session_step_result));
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_step_with_options(&host_event_loop_session,
                                                                       false,
                                                                       &session_step_options,
                                                                       &session_step_result,
                                                                       error_buf,
                                                                       sizeof(error_buf)),
                     "event loop session step options can override the drain cap for one iteration");
    HOST_PUMP_ASSERT(!session_step_result.frame_posted,
                     "event loop session step options drain-cap override does not post a frame");
    HOST_PUMP_ASSERT(session_step_result.callbacks_drained == 1,
                     "event loop session step options drain-cap override drains only one queued callback");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_posted_callback_pending_count(&host_event_loop_session) == 1,
                     "event loop session step options drain-cap override leaves one queued callback pending");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostSessionTickInputHits", &value),
                     "getHostSessionTickInputHits returns an int after the drain-cap override");
    HOST_PUMP_ASSERT(value == 6, "event loop session step options drain-cap override increments the dedicated tick hit counter once");
    drained = 0;
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_tick_default(&host_event_loop_session,
                                                                  &drained,
                                                                  error_buf,
                                                                  sizeof(error_buf)),
                     "event loop session tick default drains the remaining capped callback");
    HOST_PUMP_ASSERT(drained == 1, "event loop session tick default drains the remaining queued callback after the capped iteration");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostSessionTickInputHits", &value),
                     "getHostSessionTickInputHits returns an int after draining the capped remainder");
    HOST_PUMP_ASSERT(value == 7, "event loop session tick default drains the remaining capped callback");
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event(rt,
                                              "onHostSessionSlowTickInputEvent",
                                              &session_slow_tick_input_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "event loop session step options queue the first budget-limited callback");
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event(rt,
                                              "onHostSessionSlowTickInputEvent",
                                              &session_slow_tick_input_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "event loop session step options queue the second budget-limited callback");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_posted_callback_pending_count(&host_event_loop_session) == 2,
                     "event loop session step options see two queued callbacks before the budget-limited drain");
    memset(&session_step_options, 0, sizeof(session_step_options));
    session_step_options.has_max_callbacks = true;
    session_step_options.max_callbacks = 2;
    session_step_options.has_max_millis = true;
    session_step_options.max_millis = 1;
    memset(&session_step_result, 0, sizeof(session_step_result));
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_step_with_options(&host_event_loop_session,
                                                                       false,
                                                                       &session_step_options,
                                                                       &session_step_result,
                                                                       error_buf,
                                                                       sizeof(error_buf)),
                     "event loop session step options can override the drain time budget for one iteration");
    HOST_PUMP_ASSERT(!session_step_result.frame_posted,
                     "event loop session step options drain-time override does not post a frame");
    HOST_PUMP_ASSERT(session_step_result.callbacks_drained == 1,
                     "event loop session step options drain-time override stops after one slow callback");
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_posted_callback_pending_count(&host_event_loop_session) == 1,
                     "event loop session step options drain-time override leaves one slow callback pending");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostSessionSlowTickInputHits", &value),
                     "getHostSessionSlowTickInputHits returns an int after the drain-time override");
    HOST_PUMP_ASSERT(value == 1, "event loop session step options drain-time override processes one slow callback");
    drained = 0;
    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_host_event_loop_session_tick_default(&host_event_loop_session,
                                                                  &drained,
                                                                  error_buf,
                                                                  sizeof(error_buf)),
                     "event loop session tick default drains the remaining slow callback after the budget-limited iteration");
    HOST_PUMP_ASSERT(drained == 1, "event loop session tick default drains the remaining slow callback");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostSessionSlowTickInputHits", &value),
                     "getHostSessionSlowTickInputHits returns an int after draining the budget-limited remainder");
    HOST_PUMP_ASSERT(value == 2, "event loop session tick default drains the remaining slow callback");

    runtime_host_event_loop_session_clear_tick(&host_event_loop_session);
    runtime_host_event_loop_session_clear_end_frame(&host_event_loop_session);

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event(rt,
                                              "onHostInputEvent",
                                              &input_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "runtime_post_input_event queues a host input callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "host input callback reports queued work");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 1, "host input callback increments the pending count");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "host input callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "host input callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostInputHits", &value),
                     "getHostInputHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "host input callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_window_event(rt,
                                               "onHostWindowEvent",
                                               &window_event,
                                               error_buf,
                                               sizeof(error_buf)),
                     "runtime_post_window_event queues a host window callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "host window callback reports queued work");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 1, "host window callback increments the pending count");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "host window callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "host window callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostWindowHits", &value),
                     "getHostWindowHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "host window callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_frame_event(rt,
                                              "onHostFrameEvent",
                                              &frame_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "runtime_post_frame_event queues a host frame callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "host frame callback reports queued work");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 1, "host frame callback increments the pending count");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "host frame callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "host frame callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostFrameHits", &value),
                     "getHostFrameHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "host frame callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event_batch(rt,
                                                    "onHostInputBatch",
                                                    input_batch,
                                                    2,
                                                    error_buf,
                                                    sizeof(error_buf)),
                     "runtime_post_input_event_batch queues a host input batch callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "host input batch callback reports queued work");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 1, "host input batch callback increments the pending count");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "host input batch callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "host input batch callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostInputBatchHits", &value),
                     "getHostInputBatchHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "host input batch callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_window_event_batch(rt,
                                                     "onHostWindowBatch",
                                                     window_batch,
                                                     2,
                                                     error_buf,
                                                     sizeof(error_buf)),
                     "runtime_post_window_event_batch queues a host window batch callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "host window batch callback reports queued work");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 1, "host window batch callback increments the pending count");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "host window batch callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "host window batch callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostWindowBatchHits", &value),
                     "getHostWindowBatchHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "host window batch callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_frame_event_batch(rt,
                                                    "onHostFrameBatch",
                                                    frame_batch,
                                                    2,
                                                    error_buf,
                                                    sizeof(error_buf)),
                     "runtime_post_frame_event_batch queues a host frame batch callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "host frame batch callback reports queued work");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 1, "host frame batch callback increments the pending count");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "host frame batch callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "host frame batch callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostFrameBatchHits", &value),
                     "getHostFrameBatchHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "host frame batch callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_mixed_event_batch(rt,
                                                    "onHostMixedBatch",
                                                    mixed_batch,
                                                    3,
                                                    error_buf,
                                                    sizeof(error_buf)),
                     "runtime_post_mixed_event_batch queues a mixed host batch callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "mixed host batch callback reports queued work");
    HOST_PUMP_ASSERT(runtime_posted_callback_pending_count(rt) == 1, "mixed host batch callback increments the pending count");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "mixed host batch callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "mixed host batch callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostMixedBatchHits", &value),
                     "getHostMixedBatchHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "mixed host batch callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event(rt,
                                              "onHostCustomInputEvent",
                                              &custom_input_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "runtime_post_input_event queues a custom-meta host input callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "custom-meta host input callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "custom-meta host input callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "custom-meta host input callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostCustomInputHits", &value),
                     "getHostCustomInputHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "custom-meta host input callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_mixed_event_batch(rt,
                                                    "onHostCustomMixedBatch",
                                                    custom_mixed_batch,
                                                    3,
                                                    error_buf,
                                                    sizeof(error_buf)),
                     "runtime_post_mixed_event_batch queues a custom-meta mixed host batch callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "custom-meta mixed host batch callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "custom-meta mixed host batch callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "custom-meta mixed host batch callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostCustomMixedBatchHits", &value),
                     "getHostCustomMixedBatchHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "custom-meta mixed host batch callback increments the expected hit counter");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostCustomMixedInputHits", &value),
                     "getHostCustomMixedInputHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "custom-meta mixed host input summary matches");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostCustomMixedWindowHits", &value),
                     "getHostCustomMixedWindowHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "custom-meta mixed host window summary matches");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostCustomMixedFrameHits", &value),
                     "getHostCustomMixedFrameHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "custom-meta mixed host frame summary matches");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event(rt,
                                              "onHostExtraInputEvent",
                                              &extra_input_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "runtime_post_input_event queues a custom-field host input callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "custom-field host input callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "custom-field host input callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "custom-field host input callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostExtraInputHits", &value),
                     "getHostExtraInputHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "custom-field host input callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_mixed_event_batch(rt,
                                                    "onHostExtraMixedBatch",
                                                    extra_mixed_batch,
                                                    3,
                                                    error_buf,
                                                    sizeof(error_buf)),
                     "runtime_post_mixed_event_batch queues a custom-field mixed host batch callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "custom-field mixed host batch callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "custom-field mixed host batch callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "custom-field mixed host batch callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostExtraMixedBatchHits", &value),
                     "getHostExtraMixedBatchHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "custom-field mixed host batch callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_event(rt,
                                              "onHostStructuredInputEvent",
                                              &structured_input_event,
                                              error_buf,
                                              sizeof(error_buf)),
                     "runtime_post_input_event queues a structured host input callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "structured host input callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "structured host input callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "structured host input callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostStructuredInputHits", &value),
                     "getHostStructuredInputHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "structured host input callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_mixed_event_batch(rt,
                                                    "onHostStructuredMixedBatch",
                                                    structured_mixed_batch,
                                                    3,
                                                    error_buf,
                                                    sizeof(error_buf)),
                     "runtime_post_mixed_event_batch queues a structured mixed host batch callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "structured mixed host batch callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "structured mixed host batch callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "structured mixed host batch callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostStructuredMixedBatchHits", &value),
                     "getHostStructuredMixedBatchHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "structured mixed host batch callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_state_event(rt,
                                                    "onHostMappedInputEvent",
                                                    &mapped_input_event,
                                                    &mapped_input_payload_spec,
                                                    &mapped_input_context_spec,
                                                    error_buf,
                                                    sizeof(error_buf)),
                     "runtime_post_input_state_event queues a mapped host input callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "mapped host input callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "mapped host input callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "mapped host input callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostMappedInputHits", &value),
                     "getHostMappedInputHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "mapped host input callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_mixed_event_batch(rt,
                                                    "onHostMappedMixedBatch",
                                                    mapped_mixed_batch,
                                                    3,
                                                    error_buf,
                                                    sizeof(error_buf)),
                     "runtime_post_mixed_event_batch queues a mapped mixed host batch callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "mapped mixed host batch callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "mapped mixed host batch callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "mapped mixed host batch callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostMappedMixedBatchHits", &value),
                     "getHostMappedMixedBatchHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "mapped mixed host batch callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_window_rect_event(rt,
                                                    "onHostPresetWindowEvent",
                                                    &preset_window_event,
                                                    &preset_window_payload_spec,
                                                    &preset_window_context_spec,
                                                    error_buf,
                                                    sizeof(error_buf)),
                     "runtime_post_window_rect_event queues a preset host window callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "preset host window callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "preset host window callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "preset host window callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostPresetWindowHits", &value),
                     "getHostPresetWindowHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "preset host window callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_frame_marker_event(rt,
                                                     "onHostPresetFrameEvent",
                                                     &preset_frame_event,
                                                     &preset_frame_payload_spec,
                                                     &preset_frame_context_spec,
                                                     error_buf,
                                                     sizeof(error_buf)),
                     "runtime_post_frame_marker_event queues a preset host frame callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "preset host frame callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "preset host frame callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "preset host frame callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostPresetFrameHits", &value),
                     "getHostPresetFrameHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "preset host frame callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_input_state_event_batch(rt,
                                                          "onHostPresetInputBatch",
                                                          preset_input_batch_events,
                                                          preset_input_batch_payload_specs,
                                                          preset_input_batch_context_specs,
                                                          2,
                                                          error_buf,
                                                          sizeof(error_buf)),
                     "runtime_post_input_state_event_batch queues a preset host input batch callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "preset host input batch callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "preset host input batch callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "preset host input batch callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostPresetInputBatchHits", &value),
                     "getHostPresetInputBatchHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "preset host input batch callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_window_rect_event_batch(rt,
                                                          "onHostPresetWindowBatch",
                                                          preset_window_batch_events,
                                                          preset_window_batch_payload_specs,
                                                          preset_window_batch_context_specs,
                                                          2,
                                                          error_buf,
                                                          sizeof(error_buf)),
                     "runtime_post_window_rect_event_batch queues a preset host window batch callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "preset host window batch callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "preset host window batch callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "preset host window batch callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostPresetWindowBatchHits", &value),
                     "getHostPresetWindowBatchHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "preset host window batch callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_frame_marker_event_batch(rt,
                                                           "onHostPresetFrameBatch",
                                                           preset_frame_batch_events,
                                                           preset_frame_batch_payload_specs,
                                                           preset_frame_batch_context_specs,
                                                           2,
                                                           error_buf,
                                                           sizeof(error_buf)),
                     "runtime_post_frame_marker_event_batch queues a preset host frame batch callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "preset host frame batch callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "preset host frame batch callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "preset host frame batch callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostPresetFrameBatchHits", &value),
                     "getHostPresetFrameBatchHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "preset host frame batch callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_typed_mixed_event_batch(rt,
                                                          "onHostPresetTypedMixedBatch",
                                                          preset_typed_mixed_batch,
                                                          3,
                                                          error_buf,
                                                          sizeof(error_buf)),
                     "runtime_post_typed_mixed_event_batch queues a preset mixed host batch callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "preset mixed host batch callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "preset mixed host batch callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "preset mixed host batch callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostPresetTypedMixedBatchHits", &value),
                     "getHostPresetTypedMixedBatchHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "preset mixed host batch callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_typed_family_mixed_event_batch(rt,
                                                                 "onHostPresetTypedMixedBatch",
                                                                 &preset_typed_mixed_input_batch,
                                                                 &preset_typed_mixed_window_batch,
                                                                 &preset_typed_mixed_frame_batch,
                                                                 error_buf,
                                                                 sizeof(error_buf)),
                     "runtime_post_typed_family_mixed_event_batch queues a preset family mixed host batch callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "preset family mixed host batch callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "preset family mixed host batch callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "preset family mixed host batch callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostPresetTypedMixedBatchHits", &value),
                     "getHostPresetTypedMixedBatchHits returns an int after the family mixed batch helper");
    HOST_PUMP_ASSERT(value == 2, "preset family mixed host batch helper reuses the same callback contract");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_frame_envelope_batch(rt,
                                                       "onHostFrameEnvelopeBatch",
                                                       &preset_frame_envelope_batch,
                                                       error_buf,
                                                       sizeof(error_buf)),
                     "runtime_post_frame_envelope_batch queues a shared-context frame envelope callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "frame envelope callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "frame envelope callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "frame envelope callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostFrameEnvelopeBatchHits", &value),
                     "getHostFrameEnvelopeBatchHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "frame envelope callback increments the expected hit counter");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_frame_envelope_builder(rt,
                                                         "onHostFrameEnvelopeBuilder",
                                                         &frame_envelope_builder,
                                                         error_buf,
                                                         sizeof(error_buf)),
                     "runtime_post_frame_envelope_builder queues an appended frame envelope callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "frame envelope builder callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "frame envelope builder callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "frame envelope builder callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostFrameEnvelopeBuilderHits", &value),
                     "getHostFrameEnvelopeBuilderHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "frame envelope builder callback preserves append order and shared context");
    runtime_posted_frame_envelope_builder_clear(&frame_envelope_builder);
    HOST_PUMP_ASSERT(frame_envelope_builder.count == 0, "frame envelope builder can be cleared after flush");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(runtime_post_frame_envelope_heap_builder(rt,
                                                              "onHostFrameEnvelopeHeapBuilder",
                                                              &frame_envelope_heap_builder,
                                                              error_buf,
                                                              sizeof(error_buf)),
                     "runtime_post_frame_envelope_heap_builder queues a growable frame envelope callback");
    HOST_PUMP_ASSERT(runtime_has_posted_callbacks(rt), "frame envelope heap builder callback reports queued work");
    drained = runtime_drain_posted_callbacks(rt, 1);
    HOST_PUMP_ASSERT(drained == 1, "frame envelope heap builder callback drains exactly one queued event");
    HOST_PUMP_ASSERT(!runtime_has_error(rt), "frame envelope heap builder callback drain leaves no runtime error");
    value = 0;
    HOST_PUMP_ASSERT(runtime_run_function_and_take_int(rt, "getHostFrameEnvelopeHeapBuilderHits", &value),
                     "getHostFrameEnvelopeHeapBuilderHits returns an int");
    HOST_PUMP_ASSERT(value == 1, "frame envelope heap builder callback preserves shared context");
    runtime_posted_frame_envelope_heap_builder_clear(&frame_envelope_heap_builder);
    HOST_PUMP_ASSERT(frame_envelope_heap_builder.count == 0, "frame envelope heap builder can be cleared after flush");
    HOST_PUMP_ASSERT(frame_envelope_heap_builder.capacity >= 3, "frame envelope heap builder retains capacity after clear");
    runtime_posted_frame_envelope_heap_builder_free(&frame_envelope_heap_builder);
    HOST_PUMP_ASSERT(frame_envelope_heap_builder.events == NULL && frame_envelope_heap_builder.count == 0 && frame_envelope_heap_builder.capacity == 0,
                     "frame envelope heap builder can release owned storage");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(!runtime_post_input_event(rt,
                                               "onHostExtraInputEvent",
                                               &invalid_input_event,
                                               error_buf,
                                               sizeof(error_buf)),
                     "reserved host input extra field is rejected");
    HOST_PUMP_ASSERT(strstr(error_buf, "conflicts with a built-in event field") != NULL,
                     "reserved host input extra field reports a conflict");
    HOST_PUMP_ASSERT(!runtime_has_posted_callbacks(rt), "reserved host input extra field does not enqueue callback work");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(!runtime_post_input_event(rt,
                                               "onHostStructuredInputEvent",
                                               &invalid_nested_input_event,
                                               error_buf,
                                               sizeof(error_buf)),
                     "nested host input extra array/tuple value is rejected");
    HOST_PUMP_ASSERT(strstr(error_buf, "Nested event extra array/tuple values are not supported") != NULL,
                     "nested host input extra value reports the bounded-container error");
    HOST_PUMP_ASSERT(!runtime_has_posted_callbacks(rt), "nested host input extra value does not enqueue callback work");

    error_buf[0] = '\0';
    HOST_PUMP_ASSERT(!runtime_post_input_event(rt,
                                               "onHostMappedInputEvent",
                                               &invalid_nested_map_input_event,
                                               error_buf,
                                               sizeof(error_buf)),
                     "nested host input extra map value is rejected");
    HOST_PUMP_ASSERT(strstr(error_buf, "Nested event extra map values are not supported") != NULL,
                     "nested host input extra map reports the bounded-map error");
    HOST_PUMP_ASSERT(!runtime_has_posted_callbacks(rt), "nested host input extra map does not enqueue callback work");

    previous_auto_drain = runtime_set_posted_callback_auto_drain(rt, true);
    HOST_PUMP_ASSERT(!previous_auto_drain, "restoring auto-drain reports previous disabled state");
    HOST_PUMP_ASSERT(runtime_get_posted_callback_auto_drain(rt), "auto-drain can be restored by host");

    runtime_host_event_loop_session_free(&host_event_loop_session);
    runtime_free(rt);

#undef HOST_PUMP_ASSERT
}

static void test_runtime_host_callback_pump_api_roundtrip(void) {
    printf("Testing runtime host callback pump API...\n");

    char* extension_path = locate_test_extension_library();
    TEST_ASSERT(extension_path != NULL, "Test extension library located for host callback pump API test");
    if (!extension_path) {
        if (extension_path) free(extension_path);
        return;
    }

    char* source_path = write_temp_vml(extension_host_callback_pump_source());
    TEST_ASSERT(source_path != NULL, "Temp source file created for host callback pump API test");
    if (!source_path) {
        free(extension_path);
        return;
    }

    const char* extension_paths[] = { extension_path };
    RuntimeOptions options = {0};
    options.extension_paths = extension_paths;
    options.extension_path_count = 1;

    exercise_runtime_host_callback_pump_path(source_path,
                                             options,
                                             RUNTIME_LOAD_SOURCE,
                                             "Host callback pump source");

    char* artifact_path = NULL;
    int artifact_ready = write_temp_artifact_direct(source_path, options, &artifact_path);
    TEST_ASSERT(artifact_ready, "Artifact is written for host callback pump API test");
    if (artifact_ready && artifact_path) {
        exercise_runtime_host_callback_pump_path(artifact_path,
                                                 options,
                                                 RUNTIME_LOAD_ARTIFACT,
                                                 "Host callback pump artifact");
        cleanup_temp(artifact_path);
    }

    cleanup_temp(source_path);
    free(extension_path);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("Running TabloLang Extension Host Pump Tests...\n\n");

    test_runtime_host_callback_pump_api_roundtrip();

    printf("\nExtension Host Pump Test Results:\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
