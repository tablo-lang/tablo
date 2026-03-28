#include "tablo_ext.h"

#include <GLFW/glfw3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tablo_threads.h"

typedef struct GlfwWindowHandle {
    GLFWwindow* window;
    struct GlfwWindowHandle* prev_live;
    struct GlfwWindowHandle* next_live;
    const TabloExtCallback* input_callback;
    const TabloExtCallback* window_callback;
    TabloExtCallbackGate* input_callback_gate;
    uint64_t input_callback_generation;
    TabloExtCallbackGate* window_callback_gate;
    uint64_t window_callback_generation;
    int64_t callback_queue_limit;
    int64_t dropped_callback_count;
    int64_t rejected_callback_count;
} GlfwWindowHandle;

static once_flag g_glfw_once = ONCE_FLAG_INIT;
static mtx_t g_glfw_mutex;
static bool g_glfw_mutex_ready = false;
static bool g_glfw_initialized = false;
static int g_glfw_live_window_count = 0;
static GlfwWindowHandle* g_glfw_live_windows = NULL;
static GlfwWindowHandle* g_glfw_test_last_window = NULL;
static char g_glfw_last_error[512];

static void (*g_glfw_retain_callback)(const TabloExtCallback* callback) = NULL;
static void (*g_glfw_release_callback)(const TabloExtCallback* callback) = NULL;
static bool (*g_glfw_post_callback)(const TabloExtCallback* callback,
                                    const TabloExtValue* args,
                                    int arg_count,
                                    char* error_buf,
                                    size_t error_buf_size) = NULL;
static TabloExtCallbackGate* (*g_glfw_create_callback_gate)(void) = NULL;
static void (*g_glfw_retain_callback_gate)(TabloExtCallbackGate* gate) = NULL;
static void (*g_glfw_release_callback_gate)(TabloExtCallbackGate* gate) = NULL;
static uint64_t (*g_glfw_reset_callback_gate)(TabloExtCallbackGate* gate) = NULL;
static int64_t (*g_glfw_get_callback_gate_invalidated_count)(const TabloExtCallbackGate* gate) = NULL;
static int64_t (*g_glfw_reset_callback_gate_invalidated_count)(TabloExtCallbackGate* gate) = NULL;
static bool (*g_glfw_post_callback_gated)(const TabloExtCallback* callback,
                                          const TabloExtValue* args,
                                          int arg_count,
                                          const TabloExtCallbackGate* gate,
                                          uint64_t gate_generation,
                                          char* error_buf,
                                          size_t error_buf_size) = NULL;
static int (*g_glfw_get_posted_callback_pending_count)(const TabloExtCallback* callback) = NULL;
static bool (*g_glfw_is_posted_callback_queue_open)(const TabloExtCallback* callback) = NULL;

static void glfw_ext_set_init_error(char* error_buf, size_t error_buf_size, const char* message) {
    if (!error_buf || error_buf_size == 0) return;
    snprintf(error_buf, error_buf_size, "%s", message ? message : "Unknown GLFW extension error");
}

static void glfw_ext_set_runtime_error(TabloExtCallContext* ctx, const char* message) {
    if (!ctx || !ctx->api || !ctx->api->set_runtime_error) return;
    ctx->api->set_runtime_error(ctx, message ? message : "GLFW extension runtime error");
}

static void glfw_ext_runtime_error_from_last(TabloExtCallContext* ctx, const char* fallback) {
    char message[768];
    if (g_glfw_last_error[0] != '\0') {
        snprintf(message, sizeof(message), "%s", g_glfw_last_error);
    } else {
        snprintf(message, sizeof(message), "%s", fallback ? fallback : "GLFW extension runtime error");
    }
    glfw_ext_set_runtime_error(ctx, message);
}

static void glfw_ext_init_once(void) {
    if (mtx_init(&g_glfw_mutex, mtx_plain) == thrd_success) {
        g_glfw_mutex_ready = true;
    }
    g_glfw_last_error[0] = '\0';
}

static void glfw_ext_lock(void) {
    call_once(&g_glfw_once, glfw_ext_init_once);
    if (g_glfw_mutex_ready) {
        mtx_lock(&g_glfw_mutex);
    }
}

static void glfw_ext_unlock(void) {
    if (g_glfw_mutex_ready) {
        mtx_unlock(&g_glfw_mutex);
    }
}

static void glfw_ext_store_last_error_message(const char* message) {
    glfw_ext_lock();
    snprintf(g_glfw_last_error,
             sizeof(g_glfw_last_error),
             "%s",
             message && message[0] != '\0' ? message : "Unknown GLFW error");
    glfw_ext_unlock();
}

static void glfw_ext_store_last_errorf(const char* prefix, const char* detail) {
    char buffer[512];
    snprintf(buffer,
             sizeof(buffer),
             "%s%s%s",
             prefix ? prefix : "",
             prefix && prefix[0] != '\0' && detail && detail[0] != '\0' ? ": " : "",
             detail && detail[0] != '\0' ? detail : "Unknown GLFW error");
    glfw_ext_store_last_error_message(buffer);
}

static void glfw_ext_clear_last_error(void) {
    glfw_ext_lock();
    g_glfw_last_error[0] = '\0';
    glfw_ext_unlock();
}

static bool glfw_ext_has_last_error(void) {
    bool has_error = false;
    glfw_ext_lock();
    has_error = g_glfw_last_error[0] != '\0';
    glfw_ext_unlock();
    return has_error;
}

static void glfw_ext_error_callback(int code, const char* description) {
    char buffer[512];
    snprintf(buffer,
             sizeof(buffer),
             "GLFW error %d: %s",
             code,
             description && description[0] != '\0' ? description : "Unknown GLFW error");
    glfw_ext_store_last_error_message(buffer);
}

static TabloExtTypeDesc glfw_ext_make_type(TabloExtTypeTag tag, const char* handle_type_name, bool nullable) {
    TabloExtTypeDesc type;
    type.tag = tag;
    type.handle_type_name = handle_type_name;
    type.element_type = NULL;
    type.tuple_element_types = NULL;
    type.tuple_element_count = 0;
    type.callback_param_types = NULL;
    type.callback_param_count = 0;
    type.callback_result_type = NULL;
    type.nullable = nullable;
    return type;
}

static TabloExtTypeDesc glfw_ext_make_tuple_type(const TabloExtTypeDesc* element_types,
                                               int element_count,
                                               bool nullable) {
    TabloExtTypeDesc type;
    type.tag = TABLO_EXT_TYPE_TUPLE;
    type.handle_type_name = NULL;
    type.element_type = NULL;
    type.tuple_element_types = element_types;
    type.tuple_element_count = element_count;
    type.callback_param_types = NULL;
    type.callback_param_count = 0;
    type.callback_result_type = NULL;
    type.nullable = nullable;
    return type;
}

static TabloExtTypeDesc glfw_ext_make_callback_type(const TabloExtTypeDesc* result_type,
                                                  const TabloExtTypeDesc* param_types,
                                                  int param_count,
                                                  bool nullable) {
    TabloExtTypeDesc type;
    type.tag = TABLO_EXT_TYPE_CALLBACK;
    type.handle_type_name = NULL;
    type.element_type = NULL;
    type.tuple_element_types = NULL;
    type.tuple_element_count = 0;
    type.callback_param_types = param_types;
    type.callback_param_count = param_count;
    type.callback_result_type = result_type;
    type.nullable = nullable;
    return type;
}

static TabloExtFunctionDef glfw_ext_make_function_def(const char* name,
                                                    TabloExtTypeDesc result_type,
                                                    const TabloExtTypeDesc* param_types,
                                                    int param_count,
                                                    TabloExtFunctionCallback callback) {
    TabloExtFunctionDef def;
    def.name = name;
    def.result_type = result_type;
    def.param_types = param_types;
    def.param_count = param_count;
    def.callback = callback;
    return def;
}

static char* glfw_ext_dup_string_arg(TabloExtCallContext* ctx, int index) {
    const char* chars = NULL;
    int length = 0;
    char* copy = NULL;
    if (!ctx || !ctx->api || !ctx->api->get_string_arg) return NULL;
    if (!ctx->api->get_string_arg(ctx, index, &chars, &length)) return NULL;
    if (length < 0) length = (int)strlen(chars ? chars : "");
    copy = (char*)calloc((size_t)length + 1u, sizeof(char));
    if (!copy) {
        glfw_ext_set_runtime_error(ctx, "Out of memory while copying GLFW string argument");
        return NULL;
    }
    if (length > 0 && chars) {
        memcpy(copy, chars, (size_t)length);
    }
    copy[length] = '\0';
    return copy;
}

static GlfwWindowHandle* glfw_ext_window_from_arg(TabloExtCallContext* ctx, int index) {
    void* payload = NULL;
    GlfwWindowHandle* handle = NULL;
    if (!ctx || !ctx->api || !ctx->api->get_handle_arg) return NULL;
    if (!ctx->api->get_handle_arg(ctx, index, "GlfwWindow", &payload)) return NULL;
    handle = (GlfwWindowHandle*)payload;
    if (!handle || !handle->window) {
        glfw_ext_set_runtime_error(ctx, "GLFW window handle is not initialized");
        return NULL;
    }
    return handle;
}

static GlfwWindowHandle* glfw_ext_window_handle_from_arg(TabloExtCallContext* ctx, int index) {
    void* payload = NULL;
    if (!ctx || !ctx->api || !ctx->api->get_handle_arg) return NULL;
    if (!ctx->api->get_handle_arg(ctx, index, "GlfwWindow", &payload)) return NULL;
    return (GlfwWindowHandle*)payload;
}

static bool glfw_ext_ensure_callback_gate(TabloExtCallContext* ctx, TabloExtCallbackGate** gate_slot);
static uint64_t glfw_ext_invalidate_callback_gate(TabloExtCallbackGate* gate);
static void glfw_ext_release_callback_gate(TabloExtCallbackGate** gate_slot);
static int glfw_ext_force_terminate_all_windows(void);

static bool glfw_ext_handle_is_live_locked(GlfwWindowHandle* handle) {
    if (!handle) return false;
    return handle->prev_live != NULL || handle->next_live != NULL || g_glfw_live_windows == handle;
}

static void glfw_ext_link_live_window_locked(GlfwWindowHandle* handle) {
    if (!handle || glfw_ext_handle_is_live_locked(handle)) return;
    handle->prev_live = NULL;
    handle->next_live = g_glfw_live_windows;
    if (g_glfw_live_windows) {
        g_glfw_live_windows->prev_live = handle;
    }
    g_glfw_live_windows = handle;
    g_glfw_live_window_count++;
}

static void glfw_ext_unlink_live_window_locked(GlfwWindowHandle* handle) {
    if (!handle || !glfw_ext_handle_is_live_locked(handle)) return;
    if (handle->prev_live) {
        handle->prev_live->next_live = handle->next_live;
    } else {
        g_glfw_live_windows = handle->next_live;
    }
    if (handle->next_live) {
        handle->next_live->prev_live = handle->prev_live;
    }
    handle->prev_live = NULL;
    handle->next_live = NULL;
    if (g_glfw_live_window_count > 0) {
        g_glfw_live_window_count--;
    }
}

static void glfw_ext_release_window_callbacks(GlfwWindowHandle* handle) {
    if (!handle) return;
    if (handle->input_callback && g_glfw_release_callback) {
        g_glfw_release_callback(handle->input_callback);
        handle->input_callback = NULL;
    }
    if (handle->window_callback && g_glfw_release_callback) {
        g_glfw_release_callback(handle->window_callback);
        handle->window_callback = NULL;
    }
}

static void glfw_ext_clear_input_callback_handle(GlfwWindowHandle* handle) {
    if (!handle) return;
    handle->input_callback_generation = glfw_ext_invalidate_callback_gate(handle->input_callback_gate);
    if (handle->input_callback && g_glfw_release_callback) {
        g_glfw_release_callback(handle->input_callback);
    }
    handle->input_callback = NULL;
}

static void glfw_ext_clear_window_callback_handle(GlfwWindowHandle* handle) {
    if (!handle) return;
    handle->window_callback_generation = glfw_ext_invalidate_callback_gate(handle->window_callback_gate);
    if (handle->window_callback && g_glfw_release_callback) {
        g_glfw_release_callback(handle->window_callback);
    }
    handle->window_callback = NULL;
}

static bool glfw_ext_ensure_callback_gate(TabloExtCallContext* ctx, TabloExtCallbackGate** gate_slot) {
    if (!gate_slot) return false;
    if (*gate_slot) return true;
    if (!g_glfw_create_callback_gate) {
        if (ctx) {
            glfw_ext_set_runtime_error(ctx, "GLFW callback gates are not initialized");
        }
        return false;
    }
    *gate_slot = g_glfw_create_callback_gate();
    if (!*gate_slot) {
        if (ctx) {
            glfw_ext_set_runtime_error(ctx, "Out of memory while creating a GLFW callback gate");
        }
        return false;
    }
    return true;
}

static uint64_t glfw_ext_invalidate_callback_gate(TabloExtCallbackGate* gate) {
    if (!gate || !g_glfw_reset_callback_gate) return 0;
    return g_glfw_reset_callback_gate(gate);
}

static void glfw_ext_release_callback_gate(TabloExtCallbackGate** gate_slot) {
    if (!gate_slot || !*gate_slot) return;
    if (g_glfw_release_callback_gate) {
        g_glfw_release_callback_gate(*gate_slot);
    }
    *gate_slot = NULL;
}

static int glfw_ext_callback_pending_count(const TabloExtCallback* callback) {
    if (!callback || !g_glfw_get_posted_callback_pending_count) {
        return 0;
    }
    return g_glfw_get_posted_callback_pending_count(callback);
}

static int glfw_ext_callback_pending_count_total(GlfwWindowHandle* handle) {
    int total = 0;
    if (!handle) return 0;
    if (handle->input_callback) {
        int pending = glfw_ext_callback_pending_count(handle->input_callback);
        if (pending > 0) total += pending;
    }
    if (handle->window_callback && handle->window_callback != handle->input_callback) {
        int pending = glfw_ext_callback_pending_count(handle->window_callback);
        if (pending > 0) total += pending;
    }
    return total;
}

static bool glfw_ext_callback_queue_is_open(const TabloExtCallback* callback) {
    if (!callback || !g_glfw_is_posted_callback_queue_open) {
        return false;
    }
    return g_glfw_is_posted_callback_queue_open(callback);
}

static int64_t glfw_ext_callback_gate_invalidated_count(const TabloExtCallbackGate* gate) {
    if (!gate || !g_glfw_get_callback_gate_invalidated_count) {
        return 0;
    }
    return g_glfw_get_callback_gate_invalidated_count(gate);
}

static int64_t glfw_ext_reset_callback_gate_invalidated_count(TabloExtCallbackGate* gate) {
    if (!gate || !g_glfw_reset_callback_gate_invalidated_count) {
        return 0;
    }
    return g_glfw_reset_callback_gate_invalidated_count(gate);
}

static int64_t glfw_ext_get_invalidated_callback_count_total(GlfwWindowHandle* handle) {
    int64_t total = 0;
    if (!handle) return 0;
    total += glfw_ext_callback_gate_invalidated_count(handle->input_callback_gate);
    total += glfw_ext_callback_gate_invalidated_count(handle->window_callback_gate);
    return total;
}

static int64_t glfw_ext_reset_invalidated_callback_count_total(GlfwWindowHandle* handle) {
    int64_t total = 0;
    if (!handle) return 0;
    total += glfw_ext_reset_callback_gate_invalidated_count(handle->input_callback_gate);
    total += glfw_ext_reset_callback_gate_invalidated_count(handle->window_callback_gate);
    return total;
}

static const TabloExtCallback* glfw_ext_active_callback(GlfwWindowHandle* handle) {
    if (!handle) return NULL;
    return handle->input_callback ? handle->input_callback : handle->window_callback;
}

static bool glfw_ext_should_drop_callback_event(GlfwWindowHandle* handle, const TabloExtCallback* callback) {
    int pending_count = 0;
    if (!handle || !callback) return false;
    if (handle->callback_queue_limit <= 0) return false;
    pending_count = glfw_ext_callback_pending_count_total(handle);
    if (pending_count < 0) {
        pending_count = 0;
    }
    if ((int64_t)pending_count >= handle->callback_queue_limit) {
        handle->dropped_callback_count++;
        return true;
    }
    return false;
}

static int glfw_ext_force_terminate_all_windows(void) {
    int destroyed_count = 0;
    bool should_terminate = false;

    while (1) {
        GlfwWindowHandle* handle = NULL;
        GLFWwindow* window = NULL;

        glfw_ext_lock();
        handle = g_glfw_live_windows;
        if (!handle) {
            should_terminate = g_glfw_initialized;
            g_glfw_initialized = false;
            glfw_ext_unlock();
            break;
        }
        window = handle->window;
        handle->window = NULL;
        glfw_ext_unlink_live_window_locked(handle);
        glfw_ext_unlock();

        if (window) {
            glfwSetWindowUserPointer(window, NULL);
            glfwDestroyWindow(window);
            destroyed_count++;
        }
        glfw_ext_clear_input_callback_handle(handle);
        glfw_ext_clear_window_callback_handle(handle);
    }

    if (should_terminate) {
        glfwTerminate();
        glfw_ext_clear_last_error();
    }
    return destroyed_count;
}

static GlfwWindowHandle* glfw_ext_test_window_at_live_index(int index) {
    GlfwWindowHandle* handle = NULL;
    int current = 0;
    if (index < 0) return NULL;
    glfw_ext_lock();
    handle = g_glfw_live_windows;
    while (handle && current < index) {
        handle = handle->next_live;
        current++;
    }
    glfw_ext_unlock();
    return current == index ? handle : NULL;
}

static void glfw_ext_destroy_window_handle(void* payload) {
    GlfwWindowHandle* handle = (GlfwWindowHandle*)payload;
    GLFWwindow* window = NULL;
    if (!handle) return;
    glfw_ext_lock();
    if (handle->window) {
        window = handle->window;
        handle->window = NULL;
        glfw_ext_unlink_live_window_locked(handle);
    }
    glfw_ext_unlock();
    if (window) {
        glfwSetWindowUserPointer(window, NULL);
        glfwDestroyWindow(window);
    }
    glfw_ext_clear_input_callback_handle(handle);
    glfw_ext_clear_window_callback_handle(handle);
    glfw_ext_release_callback_gate(&handle->input_callback_gate);
    glfw_ext_release_callback_gate(&handle->window_callback_gate);
    glfw_ext_lock();
    if (g_glfw_test_last_window == handle) {
        g_glfw_test_last_window = NULL;
    }
    glfw_ext_unlock();
    free(handle);
}

static bool glfw_ext_ensure_initialized(void) {
    bool already_initialized = false;
    glfw_ext_lock();
    already_initialized = g_glfw_initialized;
    glfw_ext_unlock();
    if (already_initialized) return true;

    glfwSetErrorCallback(glfw_ext_error_callback);
    glfw_ext_clear_last_error();
    if (glfwInit() != GLFW_TRUE) {
        if (g_glfw_last_error[0] == '\0') {
            glfw_ext_store_last_error_message("GLFW initialization failed");
        }
        return false;
    }

    glfw_ext_lock();
    g_glfw_initialized = true;
    glfw_ext_unlock();
    return true;
}
static bool glfw_ext_build_meta_value(TabloExtValue* out_meta,
                                      TabloExtMapEntry* meta_entries,
                                      TabloExtValue* phase_storage,
                                      const char* phase0,
                                      const char* phase1) {
    const char* phase_chars[2];
    int phase_lengths[2];
    TabloExtEventMetaSpec meta_spec;

    if (!out_meta || !meta_entries || !phase_storage || !phase0 || !phase1) return false;
    phase_chars[0] = phase0;
    phase_chars[1] = phase1;
    phase_lengths[0] = (int)strlen(phase0);
    phase_lengths[1] = (int)strlen(phase1);
    meta_spec.source_chars = "glfw";
    meta_spec.source_length = 4;
    meta_spec.priority = 1;
    meta_spec.phases.chars = phase_chars;
    meta_spec.phases.lengths = phase_lengths;
    meta_spec.phases.count = 2;
    return tablo_ext_build_event_meta_map(out_meta, meta_entries, 3, phase_storage, 2, &meta_spec);
}

static bool glfw_ext_post_callback_event(GlfwWindowHandle* handle,
                                         const TabloExtCallback* callback,
                                         TabloExtCallbackGate* gate,
                                         uint64_t generation,
                                         const TabloExtValue* event_value,
                                         const char* failure_prefix) {
    char error_buf[512];
    if (!handle || !callback || !event_value || !g_glfw_post_callback) return true;
    error_buf[0] = '\0';
    if (gate && g_glfw_post_callback_gated) {
        if (g_glfw_post_callback_gated(callback,
                                       event_value,
                                       1,
                                       gate,
                                       generation,
                                       error_buf,
                                       sizeof(error_buf))) {
            return true;
        }
    } else if (g_glfw_post_callback(callback, event_value, 1, error_buf, sizeof(error_buf))) {
        return true;
    }
    if (handle && callback && !glfw_ext_callback_queue_is_open(callback)) {
        handle->rejected_callback_count++;
    }
    glfw_ext_store_last_errorf(failure_prefix, error_buf);
    return false;
}

static bool glfw_ext_post_input_event(GlfwWindowHandle* handle,
                                      const char* device,
                                      int64_t code,
                                      bool pressed,
                                      const char* phase0,
                                      const char* phase1) {
    TabloExtValue phase_storage[2];
    TabloExtMapEntry meta_entries[3];
    TabloExtMapEntry event_entries[5];
    TabloExtValue meta_value;
    TabloExtValue event_value;
    TabloExtInputEventSpec event_spec;

    if (!handle || !handle->input_callback || !g_glfw_post_callback) return true;
    if (glfw_ext_should_drop_callback_event(handle, handle->input_callback)) return true;
    if (!glfw_ext_build_meta_value(&meta_value, meta_entries, phase_storage, phase0, phase1)) {
        glfw_ext_store_last_error_message("Failed to build GLFW input event metadata");
        return false;
    }

    event_spec.device_chars = device;
    event_spec.device_length = -1;
    event_spec.code = code;
    event_spec.pressed = pressed;
    event_spec.meta_value = meta_value;
    if (!tablo_ext_build_input_event_map(&event_value, event_entries, 5, &event_spec)) {
        glfw_ext_store_last_error_message("Failed to build GLFW input event");
        return false;
    }

    return glfw_ext_post_callback_event(handle,
                                        handle->input_callback,
                                        handle->input_callback_gate,
                                        handle->input_callback_generation,
                                        &event_value,
                                        "Failed to queue GLFW input callback");
}

static bool glfw_ext_post_cursor_event(GlfwWindowHandle* handle,
                                       double x,
                                       double y,
                                       const char* phase0,
                                       const char* phase1) {
    TabloExtValue phase_storage[2];
    TabloExtMapEntry meta_entries[3];
    TabloExtMapEntry event_entries[6];
    TabloExtValue meta_value;
    TabloExtValue event_value;
    TabloExtMapBuilder builder;

    if (!handle || !handle->input_callback || !g_glfw_post_callback) return true;
    if (glfw_ext_should_drop_callback_event(handle, handle->input_callback)) return true;
    if (!glfw_ext_build_meta_value(&meta_value, meta_entries, phase_storage, phase0, phase1)) {
        glfw_ext_store_last_error_message("Failed to build GLFW cursor event metadata");
        return false;
    }

    tablo_ext_map_builder_init(&builder, event_entries, 6);
    if (!tablo_ext_map_builder_add_string(&builder, "kind", 4, "input", 5) ||
        !tablo_ext_map_builder_add_string(&builder, "device", 6, "cursor", 6) ||
        !tablo_ext_map_builder_add_string(&builder, "event", 5, "move", 4) ||
        !tablo_ext_map_builder_add_double(&builder, "x", 1, x) ||
        !tablo_ext_map_builder_add_double(&builder, "y", 1, y) ||
        !tablo_ext_map_builder_add_value(&builder, "meta", 4, meta_value)) {
        glfw_ext_store_last_error_message("Failed to build GLFW cursor event");
        return false;
    }
    event_value = tablo_ext_map_builder_build_map(&builder);

    return glfw_ext_post_callback_event(handle,
                                        handle->input_callback,
                                        handle->input_callback_gate,
                                        handle->input_callback_generation,
                                        &event_value,
                                        "Failed to queue GLFW cursor callback");
}

static bool glfw_ext_post_scroll_event(GlfwWindowHandle* handle,
                                       double xoffset,
                                       double yoffset,
                                       const char* phase0,
                                       const char* phase1) {
    TabloExtValue phase_storage[2];
    TabloExtMapEntry meta_entries[3];
    TabloExtMapEntry event_entries[6];
    TabloExtValue meta_value;
    TabloExtValue event_value;
    TabloExtMapBuilder builder;

    if (!handle || !handle->input_callback || !g_glfw_post_callback) return true;
    if (glfw_ext_should_drop_callback_event(handle, handle->input_callback)) return true;
    if (!glfw_ext_build_meta_value(&meta_value, meta_entries, phase_storage, phase0, phase1)) {
        glfw_ext_store_last_error_message("Failed to build GLFW scroll event metadata");
        return false;
    }

    tablo_ext_map_builder_init(&builder, event_entries, 6);
    if (!tablo_ext_map_builder_add_string(&builder, "kind", 4, "input", 5) ||
        !tablo_ext_map_builder_add_string(&builder, "device", 6, "mouse", 5) ||
        !tablo_ext_map_builder_add_string(&builder, "event", 5, "scroll", 6) ||
        !tablo_ext_map_builder_add_double(&builder, "xoffset", 7, xoffset) ||
        !tablo_ext_map_builder_add_double(&builder, "yoffset", 7, yoffset) ||
        !tablo_ext_map_builder_add_value(&builder, "meta", 4, meta_value)) {
        glfw_ext_store_last_error_message("Failed to build GLFW scroll event");
        return false;
    }
    event_value = tablo_ext_map_builder_build_map(&builder);

    return glfw_ext_post_callback_event(handle,
                                        handle->input_callback,
                                        handle->input_callback_gate,
                                        handle->input_callback_generation,
                                        &event_value,
                                        "Failed to queue GLFW scroll callback");
}

static bool glfw_ext_post_cursor_enter_event(GlfwWindowHandle* handle,
                                             bool entered,
                                             const char* phase0,
                                             const char* phase1) {
    TabloExtValue phase_storage[2];
    TabloExtMapEntry meta_entries[3];
    TabloExtMapEntry event_entries[6];
    TabloExtValue meta_value;
    TabloExtValue event_value;
    TabloExtMapBuilder builder;

    if (!handle || !handle->input_callback || !g_glfw_post_callback) return true;
    if (glfw_ext_should_drop_callback_event(handle, handle->input_callback)) return true;
    if (!glfw_ext_build_meta_value(&meta_value, meta_entries, phase_storage, phase0, phase1)) {
        glfw_ext_store_last_error_message("Failed to build GLFW cursor enter metadata");
        return false;
    }

    tablo_ext_map_builder_init(&builder, event_entries, 6);
    if (!tablo_ext_map_builder_add_string(&builder, "kind", 4, "input", 5) ||
        !tablo_ext_map_builder_add_string(&builder, "device", 6, "cursor", 6) ||
        !tablo_ext_map_builder_add_string(&builder, "event", 5, entered ? "enter" : "leave", -1) ||
        !tablo_ext_map_builder_add_bool(&builder, "entered", 7, entered) ||
        !tablo_ext_map_builder_add_value(&builder, "meta", 4, meta_value)) {
        glfw_ext_store_last_error_message("Failed to build GLFW cursor enter event");
        return false;
    }
    event_value = tablo_ext_map_builder_build_map(&builder);

    return glfw_ext_post_callback_event(handle,
                                        handle->input_callback,
                                        handle->input_callback_gate,
                                        handle->input_callback_generation,
                                        &event_value,
                                        "Failed to queue GLFW cursor enter callback");
}

static bool glfw_ext_utf8_from_codepoint(uint32_t codepoint, char out_text[5], int* out_length) {
    if (!out_text || !out_length) return false;
    if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
        return false;
    }

    if (codepoint <= 0x7Fu) {
        out_text[0] = (char)codepoint;
        out_text[1] = '\0';
        *out_length = 1;
        return true;
    }
    if (codepoint <= 0x7FFu) {
        out_text[0] = (char)(0xC0u | (codepoint >> 6));
        out_text[1] = (char)(0x80u | (codepoint & 0x3Fu));
        out_text[2] = '\0';
        *out_length = 2;
        return true;
    }
    if (codepoint <= 0xFFFFu) {
        out_text[0] = (char)(0xE0u | (codepoint >> 12));
        out_text[1] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out_text[2] = (char)(0x80u | (codepoint & 0x3Fu));
        out_text[3] = '\0';
        *out_length = 3;
        return true;
    }

    out_text[0] = (char)(0xF0u | (codepoint >> 18));
    out_text[1] = (char)(0x80u | ((codepoint >> 12) & 0x3Fu));
    out_text[2] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
    out_text[3] = (char)(0x80u | (codepoint & 0x3Fu));
    out_text[4] = '\0';
    *out_length = 4;
    return true;
}

static bool glfw_ext_post_char_event(GlfwWindowHandle* handle,
                                     uint32_t codepoint,
                                     const char* phase0,
                                     const char* phase1) {
    TabloExtValue phase_storage[2];
    TabloExtMapEntry meta_entries[3];
    TabloExtMapEntry event_entries[6];
    TabloExtValue meta_value;
    TabloExtValue event_value;
    TabloExtMapBuilder builder;
    char text[5];
    int text_length = 0;

    if (!handle || !handle->input_callback || !g_glfw_post_callback) return true;
    if (glfw_ext_should_drop_callback_event(handle, handle->input_callback)) return true;
    if (!glfw_ext_utf8_from_codepoint(codepoint, text, &text_length)) {
        glfw_ext_store_last_error_message("Invalid GLFW character input codepoint");
        return false;
    }
    if (!glfw_ext_build_meta_value(&meta_value, meta_entries, phase_storage, phase0, phase1)) {
        glfw_ext_store_last_error_message("Failed to build GLFW character event metadata");
        return false;
    }

    tablo_ext_map_builder_init(&builder, event_entries, 6);
    if (!tablo_ext_map_builder_add_string(&builder, "kind", 4, "input", 5) ||
        !tablo_ext_map_builder_add_string(&builder, "device", 6, "text", 4) ||
        !tablo_ext_map_builder_add_string(&builder, "event", 5, "char", 4) ||
        !tablo_ext_map_builder_add_int(&builder, "codepoint", 9, (int64_t)codepoint) ||
        !tablo_ext_map_builder_add_string(&builder, "text", 4, text, text_length) ||
        !tablo_ext_map_builder_add_value(&builder, "meta", 4, meta_value)) {
        glfw_ext_store_last_error_message("Failed to build GLFW character event");
        return false;
    }
    event_value = tablo_ext_map_builder_build_map(&builder);

    return glfw_ext_post_callback_event(handle,
                                        handle->input_callback,
                                        handle->input_callback_gate,
                                        handle->input_callback_generation,
                                        &event_value,
                                        "Failed to queue GLFW character callback");
}

static bool glfw_ext_post_window_event(GlfwWindowHandle* handle,
                                       const char* event_name,
                                       int64_t width,
                                       int64_t height,
                                       bool focused,
                                       const char* phase0,
                                       const char* phase1) {
    TabloExtValue phase_storage[2];
    TabloExtMapEntry meta_entries[3];
    TabloExtMapEntry event_entries[6];
    TabloExtValue meta_value;
    TabloExtValue event_value;
    TabloExtWindowEventSpec event_spec;

    if (!handle || !handle->window_callback || !g_glfw_post_callback) return true;
    if (glfw_ext_should_drop_callback_event(handle, handle->window_callback)) return true;
    if (!glfw_ext_build_meta_value(&meta_value, meta_entries, phase_storage, phase0, phase1)) {
        glfw_ext_store_last_error_message("Failed to build GLFW window event metadata");
        return false;
    }

    event_spec.event_chars = event_name;
    event_spec.event_length = -1;
    event_spec.width = width;
    event_spec.height = height;
    event_spec.focused = focused;
    event_spec.meta_value = meta_value;
    if (!tablo_ext_build_window_event_map(&event_value, event_entries, 6, &event_spec)) {
        glfw_ext_store_last_error_message("Failed to build GLFW window event");
        return false;
    }

    return glfw_ext_post_callback_event(handle,
                                        handle->window_callback,
                                        handle->window_callback_gate,
                                        handle->window_callback_generation,
                                        &event_value,
                                        "Failed to queue GLFW window callback");
}

static void glfw_ext_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    GlfwWindowHandle* handle = (GlfwWindowHandle*)glfwGetWindowUserPointer(window);
    (void)scancode;
    (void)mods;
    if (!handle) return;
    if (action == GLFW_PRESS || action == GLFW_RELEASE || action == GLFW_REPEAT) {
        (void)glfw_ext_post_input_event(handle,
                                        "keyboard",
                                        (int64_t)key,
                                        action != GLFW_RELEASE,
                                        "poll",
                                        "input");
    }
}

static void glfw_ext_mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    GlfwWindowHandle* handle = (GlfwWindowHandle*)glfwGetWindowUserPointer(window);
    (void)mods;
    if (!handle) return;
    if (action == GLFW_PRESS || action == GLFW_RELEASE) {
        (void)glfw_ext_post_input_event(handle,
                                        "mouse",
                                        (int64_t)button,
                                        action == GLFW_PRESS,
                                        "poll",
                                        "input");
    }
}

static void glfw_ext_cursor_pos_callback(GLFWwindow* window, double x, double y) {
    GlfwWindowHandle* handle = (GlfwWindowHandle*)glfwGetWindowUserPointer(window);
    if (!handle) return;
    (void)glfw_ext_post_cursor_event(handle, x, y, "poll", "input");
}

static void glfw_ext_scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    GlfwWindowHandle* handle = (GlfwWindowHandle*)glfwGetWindowUserPointer(window);
    if (!handle) return;
    (void)glfw_ext_post_scroll_event(handle, xoffset, yoffset, "poll", "input");
}

static void glfw_ext_cursor_enter_callback(GLFWwindow* window, int entered) {
    GlfwWindowHandle* handle = (GlfwWindowHandle*)glfwGetWindowUserPointer(window);
    if (!handle) return;
    (void)glfw_ext_post_cursor_enter_event(handle, entered == GLFW_TRUE, "poll", "input");
}

static void glfw_ext_char_callback(GLFWwindow* window, unsigned int codepoint) {
    GlfwWindowHandle* handle = (GlfwWindowHandle*)glfwGetWindowUserPointer(window);
    if (!handle) return;
    (void)glfw_ext_post_char_event(handle, (uint32_t)codepoint, "poll", "input");
}

static void glfw_ext_window_size_callback(GLFWwindow* window, int width, int height) {
    GlfwWindowHandle* handle = (GlfwWindowHandle*)glfwGetWindowUserPointer(window);
    int focused = 0;
    if (!handle) return;
    focused = glfwGetWindowAttrib(window, GLFW_FOCUSED);
    (void)glfw_ext_post_window_event(handle,
                                     "resize",
                                     (int64_t)width,
                                     (int64_t)height,
                                     focused == GLFW_TRUE,
                                     "poll",
                                     "window");
}

static void glfw_ext_window_focus_callback(GLFWwindow* window, int focused) {
    GlfwWindowHandle* handle = (GlfwWindowHandle*)glfwGetWindowUserPointer(window);
    int width = 0;
    int height = 0;
    if (!handle) return;
    glfwGetWindowSize(window, &width, &height);
    (void)glfw_ext_post_window_event(handle,
                                     focused == GLFW_TRUE ? "focus" : "blur",
                                     (int64_t)width,
                                     (int64_t)height,
                                     focused == GLFW_TRUE,
                                     "poll",
                                     "window");
}

static void glfw_ext_window_close_callback(GLFWwindow* window) {
    GlfwWindowHandle* handle = (GlfwWindowHandle*)glfwGetWindowUserPointer(window);
    int width = 0;
    int height = 0;
    int focused = 0;
    if (!handle) return;
    glfwGetWindowSize(window, &width, &height);
    focused = glfwGetWindowAttrib(window, GLFW_FOCUSED);
    (void)glfw_ext_post_window_event(handle,
                                     "close",
                                     (int64_t)width,
                                     (int64_t)height,
                                     focused == GLFW_TRUE,
                                     "poll",
                                     "window");
}

static bool glfw_ext_init_fn(TabloExtCallContext* ctx) {
    if (glfw_ext_ensure_initialized()) {
        glfw_ext_clear_last_error();
        return ctx->api->set_bool_result(ctx, true);
    }
    return ctx->api->set_bool_result(ctx, false);
}

static bool glfw_ext_terminate_fn(TabloExtCallContext* ctx) {
    if (!ctx || !ctx->api || !ctx->api->set_bool_result) return false;
    glfw_ext_lock();
    if (!g_glfw_initialized) {
        glfw_ext_unlock();
        return ctx->api->set_bool_result(ctx, true);
    }
    if (g_glfw_live_window_count > 0) {
        glfw_ext_unlock();
        glfw_ext_set_runtime_error(ctx, "glfwTerminate requires all GlfwWindow handles to be destroyed first");
        return false;
    }
    glfwTerminate();
    g_glfw_initialized = false;
    glfw_ext_unlock();
    glfw_ext_clear_last_error();
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_get_last_error_fn(TabloExtCallContext* ctx) {
    char buffer[512];
    if (!ctx || !ctx->api || !ctx->api->set_string_result) return false;
    glfw_ext_lock();
    snprintf(buffer, sizeof(buffer), "%s", g_glfw_last_error);
    glfw_ext_unlock();
    return ctx->api->set_string_result(ctx, buffer, -1);
}

static bool glfw_ext_get_version_string_fn(TabloExtCallContext* ctx) {
    const char* version = glfwGetVersionString();
    if (!ctx || !ctx->api || !ctx->api->set_string_result) return false;
    return ctx->api->set_string_result(ctx, version ? version : "", -1);
}

static bool glfw_ext_get_time_fn(TabloExtCallContext* ctx) {
    if (!ctx || !ctx->api || !ctx->api->set_double_result) return false;
    return ctx->api->set_double_result(ctx, glfwGetTime());
}

static bool glfw_ext_is_initialized_fn(TabloExtCallContext* ctx) {
    bool initialized = false;
    if (!ctx || !ctx->api || !ctx->api->set_bool_result) return false;
    glfw_ext_lock();
    initialized = g_glfw_initialized;
    glfw_ext_unlock();
    return ctx->api->set_bool_result(ctx, initialized);
}

static bool glfw_ext_get_live_window_count_fn(TabloExtCallContext* ctx) {
    int live_window_count = 0;
    if (!ctx || !ctx->api || !ctx->api->set_int_result) return false;
    glfw_ext_lock();
    live_window_count = g_glfw_live_window_count;
    glfw_ext_unlock();
    return ctx->api->set_int_result(ctx, live_window_count);
}

static bool glfw_ext_can_terminate_fn(TabloExtCallContext* ctx) {
    bool can_terminate = false;
    if (!ctx || !ctx->api || !ctx->api->set_bool_result) return false;
    glfw_ext_lock();
    can_terminate = g_glfw_live_window_count == 0;
    glfw_ext_unlock();
    return ctx->api->set_bool_result(ctx, can_terminate);
}

static bool glfw_ext_force_terminate_fn(TabloExtCallContext* ctx) {
    int destroyed_count = 0;
    if (!ctx || !ctx->api || !ctx->api->set_int_result) return false;
    destroyed_count = glfw_ext_force_terminate_all_windows();
    return ctx->api->set_int_result(ctx, destroyed_count);
}

static bool glfw_ext_create_window_common(TabloExtCallContext* ctx, bool with_context) {
    int64_t width = 0;
    int64_t height = 0;
    bool visible = true;
    char* title = NULL;
    GLFWwindow* window = NULL;
    GlfwWindowHandle* handle = NULL;

    if (!ctx || !ctx->api) return false;
    if (!ctx->api->get_int_arg(ctx, 0, &width) || !ctx->api->get_int_arg(ctx, 1, &height) ||
        !ctx->api->get_bool_arg(ctx, 3, &visible)) {
        return false;
    }
    if (width <= 0 || height <= 0 || width > INT32_MAX || height > INT32_MAX) {
        glfw_ext_set_runtime_error(ctx, "glfwCreateWindow width/height are invalid");
        return false;
    }
    title = glfw_ext_dup_string_arg(ctx, 2);
    if (!title) return false;
    if (!glfw_ext_ensure_initialized()) {
        free(title);
        glfw_ext_runtime_error_from_last(ctx, "glfwCreateWindow requires glfwInit to succeed first");
        return false;
    }

    glfw_ext_clear_last_error();
    glfwDefaultWindowHints();
    if (!with_context) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    } else {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    }
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
    window = glfwCreateWindow((int)width, (int)height, title, NULL, NULL);
    free(title);
    if (!window) {
        glfw_ext_runtime_error_from_last(ctx, "Failed to create GLFW window");
        return false;
    }

    handle = (GlfwWindowHandle*)calloc(1, sizeof(GlfwWindowHandle));
    if (!handle) {
        glfwDestroyWindow(window);
        glfw_ext_set_runtime_error(ctx, "Out of memory while creating GlfwWindow handle");
        return false;
    }

    handle->window = window;
    glfwSetWindowUserPointer(window, handle);
    glfwSetKeyCallback(window, glfw_ext_key_callback);
    glfwSetMouseButtonCallback(window, glfw_ext_mouse_button_callback);
    glfwSetCursorPosCallback(window, glfw_ext_cursor_pos_callback);
    glfwSetScrollCallback(window, glfw_ext_scroll_callback);
    glfwSetCursorEnterCallback(window, glfw_ext_cursor_enter_callback);
    glfwSetCharCallback(window, glfw_ext_char_callback);
    glfwSetWindowSizeCallback(window, glfw_ext_window_size_callback);
    glfwSetWindowFocusCallback(window, glfw_ext_window_focus_callback);
    glfwSetWindowCloseCallback(window, glfw_ext_window_close_callback);
    glfw_ext_lock();
    glfw_ext_link_live_window_locked(handle);
    g_glfw_test_last_window = handle;
    glfw_ext_unlock();
    return ctx->api->set_handle_result(ctx, "GlfwWindow", handle);
}

static bool glfw_ext_create_window_fn(TabloExtCallContext* ctx) {
    return glfw_ext_create_window_common(ctx, false);
}

static bool glfw_ext_create_window_with_context_fn(TabloExtCallContext* ctx) {
    return glfw_ext_create_window_common(ctx, true);
}

static bool glfw_ext_destroy_window_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = NULL;
    GLFWwindow* window = NULL;
    if (!ctx || !ctx->api || !ctx->api->get_handle_arg) return false;
    if (!ctx->api->get_handle_arg(ctx, 0, "GlfwWindow", (void**)&handle)) return false;
    if (!handle) return false;
    if (!handle->window) {
        return ctx->api->set_bool_result(ctx, true);
    }

    glfw_ext_lock();
    window = handle->window;
    handle->window = NULL;
    glfw_ext_unlink_live_window_locked(handle);
    glfw_ext_unlock();
    glfwSetWindowUserPointer(window, NULL);
    glfwDestroyWindow(window);
    glfw_ext_clear_input_callback_handle(handle);
    glfw_ext_clear_window_callback_handle(handle);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_window_should_close_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    return ctx->api->set_bool_result(ctx, glfwWindowShouldClose(handle->window) == GLFW_TRUE);
}

static bool glfw_ext_set_window_should_close_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    bool should_close = false;
    if (!handle) return false;
    if (!ctx->api->get_bool_arg(ctx, 1, &should_close)) return false;
    glfwSetWindowShouldClose(handle->window, should_close ? GLFW_TRUE : GLFW_FALSE);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_get_window_size_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    TabloExtValue values[2];
    int width = 0;
    int height = 0;
    if (!handle) return false;
    glfwGetWindowSize(handle->window, &width, &height);
    values[0] = tablo_ext_make_int_value(width);
    values[1] = tablo_ext_make_int_value(height);
    return ctx->api->set_tuple_result(ctx, values, 2);
}

static bool glfw_ext_get_primary_monitor_workarea_fn(TabloExtCallContext* ctx) {
    TabloExtValue values[4];
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    int xpos = 0;
    int ypos = 0;
    int width = 0;
    int height = 0;
    if (!monitor) {
        glfw_ext_set_runtime_error(ctx, "glfwGetPrimaryMonitorWorkarea requires a primary monitor");
        return false;
    }
    glfwGetMonitorWorkarea(monitor, &xpos, &ypos, &width, &height);
    values[0] = tablo_ext_make_int_value(xpos);
    values[1] = tablo_ext_make_int_value(ypos);
    values[2] = tablo_ext_make_int_value(width);
    values[3] = tablo_ext_make_int_value(height);
    return ctx->api->set_tuple_result(ctx, values, 4);
}

static bool glfw_ext_get_primary_monitor_content_scale_fn(TabloExtCallContext* ctx) {
    TabloExtValue values[2];
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    float xscale = 0.0f;
    float yscale = 0.0f;
    if (!monitor) {
        glfw_ext_set_runtime_error(ctx, "glfwGetPrimaryMonitorContentScale requires a primary monitor");
        return false;
    }
    glfwGetMonitorContentScale(monitor, &xscale, &yscale);
    values[0] = tablo_ext_make_double_value((double)xscale);
    values[1] = tablo_ext_make_double_value((double)yscale);
    return ctx->api->set_tuple_result(ctx, values, 2);
}

static bool glfw_ext_get_window_attrib_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    int64_t attrib = 0;
    if (!handle) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &attrib)) return false;
    if (attrib < INT32_MIN || attrib > INT32_MAX) {
        glfw_ext_set_runtime_error(ctx, "glfwGetWindowAttrib attrib is out of range");
        return false;
    }
    return ctx->api->set_int_result(ctx, (int64_t)glfwGetWindowAttrib(handle->window, (int)attrib));
}

static bool glfw_ext_get_framebuffer_size_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    TabloExtValue values[2];
    int width = 0;
    int height = 0;
    if (!handle) return false;
    glfwGetFramebufferSize(handle->window, &width, &height);
    values[0] = tablo_ext_make_int_value(width);
    values[1] = tablo_ext_make_int_value(height);
    return ctx->api->set_tuple_result(ctx, values, 2);
}

static bool glfw_ext_get_window_pos_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    TabloExtValue values[2];
    int xpos = 0;
    int ypos = 0;
    if (!handle) return false;
    glfwGetWindowPos(handle->window, &xpos, &ypos);
    values[0] = tablo_ext_make_int_value(xpos);
    values[1] = tablo_ext_make_int_value(ypos);
    return ctx->api->set_tuple_result(ctx, values, 2);
}

static bool glfw_ext_set_window_size_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    int64_t width = 0;
    int64_t height = 0;
    if (!handle) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &width) || !ctx->api->get_int_arg(ctx, 2, &height)) return false;
    if (width <= 0 || height <= 0 || width > INT32_MAX || height > INT32_MAX) {
        glfw_ext_set_runtime_error(ctx, "glfwSetWindowSize width/height are invalid");
        return false;
    }
    glfwSetWindowSize(handle->window, (int)width, (int)height);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_set_window_pos_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    int64_t xpos = 0;
    int64_t ypos = 0;
    if (!handle) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &xpos) || !ctx->api->get_int_arg(ctx, 2, &ypos)) return false;
    if (xpos < INT32_MIN || xpos > INT32_MAX || ypos < INT32_MIN || ypos > INT32_MAX) {
        glfw_ext_set_runtime_error(ctx, "glfwSetWindowPos x/y are out of range");
        return false;
    }
    glfwSetWindowPos(handle->window, (int)xpos, (int)ypos);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_show_window_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    glfwShowWindow(handle->window);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_iconify_window_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    glfwIconifyWindow(handle->window);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_hide_window_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    glfwHideWindow(handle->window);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_restore_window_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    glfwRestoreWindow(handle->window);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_maximize_window_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    glfwMaximizeWindow(handle->window);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_focus_window_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    glfwFocusWindow(handle->window);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_make_context_current_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    glfw_ext_clear_last_error();
    glfwMakeContextCurrent(handle->window);
    if (glfwGetCurrentContext() != handle->window) {
        if (glfw_ext_has_last_error()) {
            glfw_ext_runtime_error_from_last(ctx, "glfwMakeContextCurrent failed");
        } else {
            glfw_ext_set_runtime_error(ctx, "glfwMakeContextCurrent did not make the requested window current");
        }
        return false;
    }
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_clear_current_context_fn(TabloExtCallContext* ctx) {
    if (!ctx || !ctx->api) return false;
    glfw_ext_clear_last_error();
    glfwMakeContextCurrent(NULL);
    if (glfwGetCurrentContext() != NULL) {
        if (glfw_ext_has_last_error()) {
            glfw_ext_runtime_error_from_last(ctx, "glfwClearCurrentContext failed");
        } else {
            glfw_ext_set_runtime_error(ctx, "glfwClearCurrentContext did not clear the current context");
        }
        return false;
    }
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_has_any_current_context_fn(TabloExtCallContext* ctx) {
    if (!ctx || !ctx->api) return false;
    return ctx->api->set_bool_result(ctx, glfwGetCurrentContext() != NULL);
}

static bool glfw_ext_has_current_context_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    return ctx->api->set_bool_result(ctx, glfwGetCurrentContext() == handle->window);
}

static bool glfw_ext_swap_buffers_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    glfw_ext_clear_last_error();
    glfwSwapBuffers(handle->window);
    if (glfw_ext_has_last_error()) {
        glfw_ext_runtime_error_from_last(ctx, "glfwSwapBuffers failed");
        return false;
    }
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_swap_interval_fn(TabloExtCallContext* ctx) {
    int64_t interval = 0;
    if (!ctx || !ctx->api) return false;
    if (!ctx->api->get_int_arg(ctx, 0, &interval)) return false;
    if (interval < INT32_MIN || interval > INT32_MAX) {
        glfw_ext_set_runtime_error(ctx, "glfwSwapInterval interval is out of range");
        return false;
    }
    glfw_ext_clear_last_error();
    glfwSwapInterval((int)interval);
    if (glfw_ext_has_last_error()) {
        glfw_ext_runtime_error_from_last(ctx, "glfwSwapInterval failed");
        return false;
    }
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_set_window_title_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    char* title = NULL;
    if (!handle) return false;
    title = glfw_ext_dup_string_arg(ctx, 1);
    if (!title) return false;
    glfwSetWindowTitle(handle->window, title);
    free(title);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_get_cursor_pos_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    TabloExtValue values[2];
    double x = 0.0;
    double y = 0.0;
    if (!handle) return false;
    glfwGetCursorPos(handle->window, &x, &y);
    values[0] = tablo_ext_make_double_value(x);
    values[1] = tablo_ext_make_double_value(y);
    return ctx->api->set_tuple_result(ctx, values, 2);
}

static bool glfw_ext_get_window_content_scale_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    TabloExtValue values[2];
    float xscale = 0.0f;
    float yscale = 0.0f;
    if (!handle) return false;
    glfwGetWindowContentScale(handle->window, &xscale, &yscale);
    values[0] = tablo_ext_make_double_value((double)xscale);
    values[1] = tablo_ext_make_double_value((double)yscale);
    return ctx->api->set_tuple_result(ctx, values, 2);
}

static bool glfw_ext_set_cursor_pos_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    double x = 0.0;
    double y = 0.0;
    if (!handle) return false;
    if (!ctx->api->get_double_arg(ctx, 1, &x) || !ctx->api->get_double_arg(ctx, 2, &y)) {
        return false;
    }
    glfwSetCursorPos(handle->window, x, y);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_get_cursor_mode_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    return ctx->api->set_int_result(ctx, (int64_t)glfwGetInputMode(handle->window, GLFW_CURSOR));
}

static bool glfw_ext_raw_mouse_motion_supported_fn(TabloExtCallContext* ctx) {
    return ctx->api->set_bool_result(ctx, glfwRawMouseMotionSupported() == GLFW_TRUE);
}

static bool glfw_ext_get_raw_mouse_motion_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    return ctx->api->set_bool_result(ctx, glfwGetInputMode(handle->window, GLFW_RAW_MOUSE_MOTION) == GLFW_TRUE);
}

static bool glfw_ext_set_raw_mouse_motion_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    bool enabled = false;
    if (!handle) return false;
    if (!ctx->api->get_bool_arg(ctx, 1, &enabled)) return false;
    if (enabled && glfwRawMouseMotionSupported() != GLFW_TRUE) {
        glfw_ext_set_runtime_error(ctx, "glfwSetRawMouseMotion requires platform raw mouse motion support");
        return false;
    }
    glfw_ext_clear_last_error();
    glfwSetInputMode(handle->window, GLFW_RAW_MOUSE_MOTION, enabled ? GLFW_TRUE : GLFW_FALSE);
    if (glfw_ext_has_last_error()) {
        glfw_ext_runtime_error_from_last(ctx, "glfwSetRawMouseMotion failed");
        return false;
    }
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_set_cursor_mode_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    int64_t mode = 0;
    if (!handle) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &mode)) return false;
    if (mode != GLFW_CURSOR_NORMAL &&
        mode != GLFW_CURSOR_HIDDEN &&
        mode != GLFW_CURSOR_DISABLED) {
        glfw_ext_set_runtime_error(ctx, "glfwSetCursorMode mode is invalid");
        return false;
    }
    glfwSetInputMode(handle->window, GLFW_CURSOR, (int)mode);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_cursor_mode_normal_fn(TabloExtCallContext* ctx) {
    return ctx->api->set_int_result(ctx, (int64_t)GLFW_CURSOR_NORMAL);
}

static bool glfw_ext_cursor_mode_hidden_fn(TabloExtCallContext* ctx) {
    return ctx->api->set_int_result(ctx, (int64_t)GLFW_CURSOR_HIDDEN);
}

static bool glfw_ext_cursor_mode_disabled_fn(TabloExtCallContext* ctx) {
    return ctx->api->set_int_result(ctx, (int64_t)GLFW_CURSOR_DISABLED);
}

static bool glfw_ext_window_attrib_visible_fn(TabloExtCallContext* ctx) {
    return ctx->api->set_int_result(ctx, (int64_t)GLFW_VISIBLE);
}

static bool glfw_ext_window_attrib_focused_fn(TabloExtCallContext* ctx) {
    return ctx->api->set_int_result(ctx, (int64_t)GLFW_FOCUSED);
}

static bool glfw_ext_window_attrib_iconified_fn(TabloExtCallContext* ctx) {
    return ctx->api->set_int_result(ctx, (int64_t)GLFW_ICONIFIED);
}

static bool glfw_ext_window_attrib_maximized_fn(TabloExtCallContext* ctx) {
    return ctx->api->set_int_result(ctx, (int64_t)GLFW_MAXIMIZED);
}

static bool glfw_ext_get_key_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    int64_t key = 0;
    if (!handle) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &key)) return false;
    if (key < INT32_MIN || key > INT32_MAX) {
        glfw_ext_set_runtime_error(ctx, "glfwGetKey key is out of range");
        return false;
    }
    return ctx->api->set_int_result(ctx, (int64_t)glfwGetKey(handle->window, (int)key));
}

static bool glfw_ext_get_mouse_button_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    int64_t button = 0;
    if (!handle) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &button)) return false;
    if (button < INT32_MIN || button > INT32_MAX) {
        glfw_ext_set_runtime_error(ctx, "glfwGetMouseButton button is out of range");
        return false;
    }
    return ctx->api->set_int_result(ctx, (int64_t)glfwGetMouseButton(handle->window, (int)button));
}

static bool glfw_ext_poll_events_fn(TabloExtCallContext* ctx) {
    if (!glfw_ext_ensure_initialized()) {
        glfw_ext_runtime_error_from_last(ctx, "glfwPollEvents requires glfwInit to succeed first");
        return false;
    }
    glfwPollEvents();
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_wait_events_timeout_fn(TabloExtCallContext* ctx) {
    double timeout_seconds = 0.0;
    if (!ctx || !ctx->api) return false;
    if (!ctx->api->get_double_arg(ctx, 0, &timeout_seconds)) return false;
    if (timeout_seconds < 0.0) {
        glfw_ext_set_runtime_error(ctx, "glfwWaitEventsTimeout timeout must be non-negative");
        return false;
    }
    if (!glfw_ext_ensure_initialized()) {
        glfw_ext_runtime_error_from_last(ctx, "glfwWaitEventsTimeout requires glfwInit to succeed first");
        return false;
    }
    glfwWaitEventsTimeout(timeout_seconds);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_post_empty_event_fn(TabloExtCallContext* ctx) {
    if (!ctx || !ctx->api) return false;
    if (!glfw_ext_ensure_initialized()) {
        glfw_ext_runtime_error_from_last(ctx, "glfwPostEmptyEvent requires glfwInit to succeed first");
        return false;
    }
    glfwPostEmptyEvent();
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_set_input_callback_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    const TabloExtCallback* callback = NULL;
    if (!handle) return false;
    if (!ctx->api->get_callback_arg(ctx, 1, &callback)) return false;
    if (!callback) {
        glfw_ext_set_runtime_error(ctx, "glfwSetInputCallback requires a non-nil callback");
        return false;
    }
    if (!g_glfw_retain_callback || !g_glfw_release_callback) {
        glfw_ext_set_runtime_error(ctx, "GLFW callback hooks are not initialized");
        return false;
    }
    if (!glfw_ext_ensure_callback_gate(ctx, &handle->input_callback_gate)) {
        return false;
    }
    g_glfw_retain_callback(callback);
    glfw_ext_clear_input_callback_handle(handle);
    handle->input_callback = callback;
    if (handle->input_callback_generation == 0) {
        handle->input_callback_generation = glfw_ext_invalidate_callback_gate(handle->input_callback_gate);
    }
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_set_window_callback_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    const TabloExtCallback* callback = NULL;
    if (!handle) return false;
    if (!ctx->api->get_callback_arg(ctx, 1, &callback)) return false;
    if (!callback) {
        glfw_ext_set_runtime_error(ctx, "glfwSetWindowCallback requires a non-nil callback");
        return false;
    }
    if (!g_glfw_retain_callback || !g_glfw_release_callback) {
        glfw_ext_set_runtime_error(ctx, "GLFW callback hooks are not initialized");
        return false;
    }
    if (!glfw_ext_ensure_callback_gate(ctx, &handle->window_callback_gate)) {
        return false;
    }
    g_glfw_retain_callback(callback);
    glfw_ext_clear_window_callback_handle(handle);
    handle->window_callback = callback;
    if (handle->window_callback_generation == 0) {
        handle->window_callback_generation = glfw_ext_invalidate_callback_gate(handle->window_callback_gate);
    }
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_clear_input_callback_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    glfw_ext_clear_input_callback_handle(handle);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_clear_window_callback_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    glfw_ext_clear_window_callback_handle(handle);
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_has_input_callback_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    return ctx->api->set_bool_result(ctx, handle->input_callback != NULL);
}

static bool glfw_ext_has_window_callback_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    return ctx->api->set_bool_result(ctx, handle->window_callback != NULL);
}

static bool glfw_ext_set_callback_queue_limit_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    int64_t limit = 0;
    if (!handle) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &limit)) return false;
    if (limit < 0) {
        glfw_ext_set_runtime_error(ctx, "glfwSetCallbackQueueLimit limit must be non-negative");
        return false;
    }
    handle->callback_queue_limit = limit;
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_get_callback_queue_limit_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    if (!handle) return false;
    return ctx->api->set_int_result(ctx, handle->callback_queue_limit);
}

static bool glfw_ext_get_dropped_callback_count_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_handle_from_arg(ctx, 0);
    if (!handle) return false;
    return ctx->api->set_int_result(ctx, handle->dropped_callback_count);
}

static bool glfw_ext_reset_dropped_callback_count_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_handle_from_arg(ctx, 0);
    int64_t previous = 0;
    if (!handle) return false;
    previous = handle->dropped_callback_count;
    handle->dropped_callback_count = 0;
    return ctx->api->set_int_result(ctx, previous);
}

static bool glfw_ext_can_post_callbacks_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_handle_from_arg(ctx, 0);
    const TabloExtCallback* callback = NULL;
    if (!handle) return false;
    callback = glfw_ext_active_callback(handle);
    return ctx->api->set_bool_result(ctx, callback != NULL && glfw_ext_callback_queue_is_open(callback));
}

static bool glfw_ext_get_invalidated_callback_count_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_handle_from_arg(ctx, 0);
    if (!handle) return false;
    return ctx->api->set_int_result(ctx, glfw_ext_get_invalidated_callback_count_total(handle));
}

static bool glfw_ext_reset_invalidated_callback_count_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_handle_from_arg(ctx, 0);
    if (!handle) return false;
    return ctx->api->set_int_result(ctx, glfw_ext_reset_invalidated_callback_count_total(handle));
}

static bool glfw_ext_get_rejected_callback_count_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_handle_from_arg(ctx, 0);
    if (!handle) return false;
    return ctx->api->set_int_result(ctx, handle->rejected_callback_count);
}

static bool glfw_ext_reset_rejected_callback_count_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_handle_from_arg(ctx, 0);
    int64_t previous = 0;
    if (!handle) return false;
    previous = handle->rejected_callback_count;
    handle->rejected_callback_count = 0;
    return ctx->api->set_int_result(ctx, previous);
}

static bool glfw_ext_get_callback_queue_pending_count_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_handle_from_arg(ctx, 0);
    if (!handle) return false;
    return ctx->api->set_int_result(ctx, glfw_ext_callback_pending_count_total(handle));
}

static bool glfw_ext_test_emit_scroll_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    double xoffset = 0.0;
    double yoffset = 0.0;
    if (!handle) return false;
    if (!ctx->api->get_double_arg(ctx, 1, &xoffset) || !ctx->api->get_double_arg(ctx, 2, &yoffset)) {
        return false;
    }
    if (!handle->input_callback) {
        glfw_ext_set_runtime_error(ctx, "glfwTestEmitScroll requires an input callback to be registered");
        return false;
    }
    if (!glfw_ext_post_scroll_event(handle, xoffset, yoffset, "test", "input")) {
        glfw_ext_runtime_error_from_last(ctx, "Failed to queue test GLFW scroll event");
        return false;
    }
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_test_emit_cursor_enter_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    bool entered = false;
    if (!handle) return false;
    if (!ctx->api->get_bool_arg(ctx, 1, &entered)) return false;
    if (!handle->input_callback) {
        glfw_ext_set_runtime_error(ctx, "glfwTestEmitCursorEnter requires an input callback to be registered");
        return false;
    }
    if (!glfw_ext_post_cursor_enter_event(handle, entered, "test", "input")) {
        glfw_ext_runtime_error_from_last(ctx, "Failed to queue test GLFW cursor enter event");
        return false;
    }
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_test_emit_char_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    int64_t codepoint = 0;
    if (!handle) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &codepoint)) return false;
    if (codepoint < 0 || codepoint > 0x10FFFF) {
        glfw_ext_set_runtime_error(ctx, "glfwTestEmitChar codepoint is out of range");
        return false;
    }
    if (!handle->input_callback) {
        glfw_ext_set_runtime_error(ctx, "glfwTestEmitChar requires an input callback to be registered");
        return false;
    }
    if (!glfw_ext_post_char_event(handle, (uint32_t)codepoint, "test", "input")) {
        glfw_ext_runtime_error_from_last(ctx, "Failed to queue test GLFW character event");
        return false;
    }
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_test_emit_key_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    int64_t key = 0;
    bool pressed = false;
    if (!handle) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &key) || !ctx->api->get_bool_arg(ctx, 2, &pressed)) {
        return false;
    }
    if (!handle->input_callback) {
        glfw_ext_set_runtime_error(ctx, "glfwTestEmitKey requires an input callback to be registered");
        return false;
    }
    if (!glfw_ext_post_input_event(handle, "keyboard", key, pressed, "test", "input")) {
        glfw_ext_runtime_error_from_last(ctx, "Failed to queue test GLFW key event");
        return false;
    }
    return ctx->api->set_bool_result(ctx, true);
}

static bool glfw_ext_test_emit_mouse_button_fn(TabloExtCallContext* ctx) {
    GlfwWindowHandle* handle = glfw_ext_window_from_arg(ctx, 0);
    int64_t button = 0;
    bool pressed = false;
    if (!handle) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &button) || !ctx->api->get_bool_arg(ctx, 2, &pressed)) {
        return false;
    }
    if (!handle->input_callback) {
        glfw_ext_set_runtime_error(ctx, "glfwTestEmitMouseButton requires an input callback to be registered");
        return false;
    }
    if (!glfw_ext_post_input_event(handle, "mouse", button, pressed, "test", "input")) {
        glfw_ext_runtime_error_from_last(ctx, "Failed to queue test GLFW mouse button event");
        return false;
    }
    return ctx->api->set_bool_result(ctx, true);
}

TABLO_EXT_EXPORT bool tablo_extension_init(const TabloExtRegistrar* registrar,
                                          char* error_buf,
                                          size_t error_buf_size) {
    TabloExtHandleTypeDef window_handle_def;
    TabloExtTypeDesc void_type;
    TabloExtTypeDesc bool_type;
    TabloExtTypeDesc int_type;
    TabloExtTypeDesc double_type;
    TabloExtTypeDesc string_type;
    TabloExtTypeDesc window_type;
    TabloExtTypeDesc event_map_type;
    TabloExtTypeDesc monitor_workarea_tuple_elements[4];
    TabloExtTypeDesc monitor_workarea_tuple_type;
    TabloExtTypeDesc window_size_tuple_elements[2];
    TabloExtTypeDesc window_size_tuple_type;
    TabloExtTypeDesc cursor_pos_tuple_elements[2];
    TabloExtTypeDesc cursor_pos_tuple_type;
    TabloExtTypeDesc window_callback_params[1];
    TabloExtTypeDesc input_callback_params[1];
    TabloExtTypeDesc window_event_callback_type;
    TabloExtTypeDesc input_event_callback_type;
    TabloExtTypeDesc create_window_params[4];
    TabloExtTypeDesc create_window_with_context_params[4];
    TabloExtTypeDesc set_window_should_close_params[2];
    TabloExtTypeDesc set_window_size_params[3];
    TabloExtTypeDesc get_window_attrib_params[2];
    TabloExtTypeDesc set_window_pos_params[3];
    TabloExtTypeDesc set_window_title_params[2];
    TabloExtTypeDesc set_cursor_pos_params[3];
    TabloExtTypeDesc set_cursor_mode_params[2];
    TabloExtTypeDesc set_raw_mouse_motion_params[2];
    TabloExtTypeDesc swap_interval_params[1];
    TabloExtTypeDesc get_key_params[2];
    TabloExtTypeDesc get_mouse_button_params[2];
    TabloExtTypeDesc test_emit_scroll_params[3];
    TabloExtTypeDesc test_emit_cursor_enter_params[2];
    TabloExtTypeDesc test_emit_char_params[2];
    TabloExtTypeDesc test_emit_key_params[3];
    TabloExtTypeDesc test_emit_mouse_button_params[3];
    TabloExtTypeDesc wait_events_timeout_params[1];
    TabloExtTypeDesc clear_input_callback_params[1];
    TabloExtTypeDesc clear_window_callback_params[1];
    TabloExtTypeDesc has_input_callback_params[1];
    TabloExtTypeDesc has_window_callback_params[1];
    TabloExtTypeDesc set_callback_queue_limit_params[2];
    TabloExtTypeDesc get_callback_queue_limit_params[1];
    TabloExtTypeDesc get_dropped_callback_count_params[1];
    TabloExtTypeDesc reset_dropped_callback_count_params[1];
    TabloExtTypeDesc can_post_callbacks_params[1];
    TabloExtTypeDesc get_invalidated_callback_count_params[1];
    TabloExtTypeDesc reset_invalidated_callback_count_params[1];
    TabloExtTypeDesc get_rejected_callback_count_params[1];
    TabloExtTypeDesc reset_rejected_callback_count_params[1];
    TabloExtTypeDesc get_callback_queue_pending_count_params[1];
    TabloExtTypeDesc set_input_callback_params[2];
    TabloExtTypeDesc set_window_callback_params[2];
    TabloExtFunctionDef init_def;
    TabloExtFunctionDef terminate_def;
    TabloExtFunctionDef get_last_error_def;
    TabloExtFunctionDef get_version_string_def;
    TabloExtFunctionDef get_time_def;
    TabloExtFunctionDef is_initialized_def;
    TabloExtFunctionDef get_live_window_count_def;
    TabloExtFunctionDef can_terminate_def;
    TabloExtFunctionDef force_terminate_def;
    TabloExtFunctionDef create_window_def;
    TabloExtFunctionDef create_window_with_context_def;
    TabloExtFunctionDef destroy_window_def;
    TabloExtFunctionDef window_should_close_def;
    TabloExtFunctionDef set_window_should_close_def;
    TabloExtFunctionDef get_window_size_def;
    TabloExtFunctionDef get_primary_monitor_workarea_def;
    TabloExtFunctionDef get_primary_monitor_content_scale_def;
    TabloExtFunctionDef get_window_attrib_def;
    TabloExtFunctionDef get_framebuffer_size_def;
    TabloExtFunctionDef get_window_pos_def;
    TabloExtFunctionDef set_window_size_def;
    TabloExtFunctionDef set_window_pos_def;
    TabloExtFunctionDef show_window_def;
    TabloExtFunctionDef iconify_window_def;
    TabloExtFunctionDef hide_window_def;
    TabloExtFunctionDef restore_window_def;
    TabloExtFunctionDef maximize_window_def;
    TabloExtFunctionDef focus_window_def;
    TabloExtFunctionDef make_context_current_def;
    TabloExtFunctionDef clear_current_context_def;
    TabloExtFunctionDef has_any_current_context_def;
    TabloExtFunctionDef has_current_context_def;
    TabloExtFunctionDef swap_buffers_def;
    TabloExtFunctionDef swap_interval_def;
    TabloExtFunctionDef set_window_title_def;
    TabloExtFunctionDef get_cursor_pos_def;
    TabloExtFunctionDef get_window_content_scale_def;
    TabloExtFunctionDef set_cursor_pos_def;
    TabloExtFunctionDef raw_mouse_motion_supported_def;
    TabloExtFunctionDef get_raw_mouse_motion_def;
    TabloExtFunctionDef set_raw_mouse_motion_def;
    TabloExtFunctionDef get_cursor_mode_def;
    TabloExtFunctionDef set_cursor_mode_def;
    TabloExtFunctionDef cursor_mode_normal_def;
    TabloExtFunctionDef cursor_mode_hidden_def;
    TabloExtFunctionDef cursor_mode_disabled_def;
    TabloExtFunctionDef window_attrib_visible_def;
    TabloExtFunctionDef window_attrib_focused_def;
    TabloExtFunctionDef window_attrib_iconified_def;
    TabloExtFunctionDef window_attrib_maximized_def;
    TabloExtFunctionDef get_key_def;
    TabloExtFunctionDef get_mouse_button_def;
    TabloExtFunctionDef poll_events_def;
    TabloExtFunctionDef wait_events_timeout_def;
    TabloExtFunctionDef post_empty_event_def;
    TabloExtFunctionDef set_input_callback_def;
    TabloExtFunctionDef set_window_callback_def;
    TabloExtFunctionDef clear_input_callback_def;
    TabloExtFunctionDef clear_window_callback_def;
    TabloExtFunctionDef has_input_callback_def;
    TabloExtFunctionDef has_window_callback_def;
    TabloExtFunctionDef set_callback_queue_limit_def;
    TabloExtFunctionDef get_callback_queue_limit_def;
    TabloExtFunctionDef get_dropped_callback_count_def;
    TabloExtFunctionDef reset_dropped_callback_count_def;
    TabloExtFunctionDef can_post_callbacks_def;
    TabloExtFunctionDef get_invalidated_callback_count_def;
    TabloExtFunctionDef reset_invalidated_callback_count_def;
    TabloExtFunctionDef get_rejected_callback_count_def;
    TabloExtFunctionDef reset_rejected_callback_count_def;
    TabloExtFunctionDef get_callback_queue_pending_count_def;
    TabloExtFunctionDef test_emit_scroll_def;
    TabloExtFunctionDef test_emit_cursor_enter_def;
    TabloExtFunctionDef test_emit_char_def;
    TabloExtFunctionDef test_emit_key_def;
    TabloExtFunctionDef test_emit_mouse_button_def;
    const TabloExtFunctionDef* function_defs[76];

    if (!registrar) {
        glfw_ext_set_init_error(error_buf, error_buf_size, "Missing GLFW extension registrar");
        return false;
    }
    if (registrar->abi_version != TABLO_EXT_ABI_VERSION) {
        glfw_ext_set_init_error(error_buf, error_buf_size, "Unsupported TabloLang extension ABI version");
        return false;
    }
    if (!registrar->register_handle_type || !registrar->register_function ||
        !registrar->retain_callback || !registrar->release_callback || !registrar->post_callback ||
        !registrar->create_callback_gate || !registrar->retain_callback_gate ||
        !registrar->release_callback_gate || !registrar->reset_callback_gate ||
        !registrar->get_callback_gate_invalidated_count ||
        !registrar->reset_callback_gate_invalidated_count ||
        !registrar->post_callback_gated ||
        !registrar->get_posted_callback_pending_count ||
        !registrar->is_posted_callback_queue_open) {
        glfw_ext_set_init_error(error_buf, error_buf_size, "GLFW registrar callbacks are not initialized");
        return false;
    }

    g_glfw_retain_callback = registrar->retain_callback;
    g_glfw_release_callback = registrar->release_callback;
    g_glfw_post_callback = registrar->post_callback;
    g_glfw_create_callback_gate = registrar->create_callback_gate;
    g_glfw_retain_callback_gate = registrar->retain_callback_gate;
    g_glfw_release_callback_gate = registrar->release_callback_gate;
    g_glfw_reset_callback_gate = registrar->reset_callback_gate;
    g_glfw_get_callback_gate_invalidated_count = registrar->get_callback_gate_invalidated_count;
    g_glfw_reset_callback_gate_invalidated_count = registrar->reset_callback_gate_invalidated_count;
    g_glfw_post_callback_gated = registrar->post_callback_gated;
    g_glfw_get_posted_callback_pending_count = registrar->get_posted_callback_pending_count;
    g_glfw_is_posted_callback_queue_open = registrar->is_posted_callback_queue_open;

    window_handle_def.name = "GlfwWindow";
    window_handle_def.destroy = glfw_ext_destroy_window_handle;
    if (!registrar->register_handle_type(registrar->host_context, &window_handle_def, error_buf, error_buf_size)) {
        return false;
    }

    void_type = glfw_ext_make_type(TABLO_EXT_TYPE_VOID, NULL, false);
    bool_type = glfw_ext_make_type(TABLO_EXT_TYPE_BOOL, NULL, false);
    int_type = glfw_ext_make_type(TABLO_EXT_TYPE_INT, NULL, false);
    double_type = glfw_ext_make_type(TABLO_EXT_TYPE_DOUBLE, NULL, false);
    string_type = glfw_ext_make_type(TABLO_EXT_TYPE_STRING, NULL, false);
    window_type = glfw_ext_make_type(TABLO_EXT_TYPE_HANDLE, "GlfwWindow", false);
    event_map_type = glfw_ext_make_type(TABLO_EXT_TYPE_MAP, NULL, false);
    monitor_workarea_tuple_elements[0] = int_type;
    monitor_workarea_tuple_elements[1] = int_type;
    monitor_workarea_tuple_elements[2] = int_type;
    monitor_workarea_tuple_elements[3] = int_type;
    monitor_workarea_tuple_type = glfw_ext_make_tuple_type(monitor_workarea_tuple_elements, 4, false);
    window_size_tuple_elements[0] = int_type;
    window_size_tuple_elements[1] = int_type;
    window_size_tuple_type = glfw_ext_make_tuple_type(window_size_tuple_elements, 2, false);
    cursor_pos_tuple_elements[0] = double_type;
    cursor_pos_tuple_elements[1] = double_type;
    cursor_pos_tuple_type = glfw_ext_make_tuple_type(cursor_pos_tuple_elements, 2, false);
    window_callback_params[0] = event_map_type;
    input_callback_params[0] = event_map_type;
    window_event_callback_type = glfw_ext_make_callback_type(&void_type, window_callback_params, 1, false);
    input_event_callback_type = glfw_ext_make_callback_type(&void_type, input_callback_params, 1, false);

    init_def = glfw_ext_make_function_def("glfwInit", bool_type, NULL, 0, glfw_ext_init_fn);
    terminate_def = glfw_ext_make_function_def("glfwTerminate", bool_type, NULL, 0, glfw_ext_terminate_fn);
    get_last_error_def = glfw_ext_make_function_def("glfwGetLastError", string_type, NULL, 0, glfw_ext_get_last_error_fn);
    get_version_string_def = glfw_ext_make_function_def("glfwGetVersionString", string_type, NULL, 0, glfw_ext_get_version_string_fn);
    get_time_def = glfw_ext_make_function_def("glfwGetTime", double_type, NULL, 0, glfw_ext_get_time_fn);
    is_initialized_def = glfw_ext_make_function_def("glfwIsInitialized", bool_type, NULL, 0, glfw_ext_is_initialized_fn);
    get_live_window_count_def = glfw_ext_make_function_def("glfwGetLiveWindowCount", int_type, NULL, 0, glfw_ext_get_live_window_count_fn);
    can_terminate_def = glfw_ext_make_function_def("glfwCanTerminate", bool_type, NULL, 0, glfw_ext_can_terminate_fn);
    force_terminate_def = glfw_ext_make_function_def("glfwForceTerminate", int_type, NULL, 0, glfw_ext_force_terminate_fn);
    create_window_params[0] = int_type;
    create_window_params[1] = int_type;
    create_window_params[2] = string_type;
    create_window_params[3] = bool_type;
    create_window_def = glfw_ext_make_function_def("glfwCreateWindow", window_type, create_window_params, 4, glfw_ext_create_window_fn);
    create_window_with_context_params[0] = int_type;
    create_window_with_context_params[1] = int_type;
    create_window_with_context_params[2] = string_type;
    create_window_with_context_params[3] = bool_type;
    create_window_with_context_def = glfw_ext_make_function_def("glfwCreateWindowWithContext", window_type, create_window_with_context_params, 4, glfw_ext_create_window_with_context_fn);
    destroy_window_def = glfw_ext_make_function_def("glfwDestroyWindow", bool_type, &window_type, 1, glfw_ext_destroy_window_fn);
    window_should_close_def = glfw_ext_make_function_def("glfwWindowShouldClose", bool_type, &window_type, 1, glfw_ext_window_should_close_fn);
    set_window_should_close_params[0] = window_type;
    set_window_should_close_params[1] = bool_type;
    set_window_should_close_def = glfw_ext_make_function_def("glfwSetWindowShouldClose", bool_type, set_window_should_close_params, 2, glfw_ext_set_window_should_close_fn);
    get_window_size_def = glfw_ext_make_function_def("glfwGetWindowSize", window_size_tuple_type, &window_type, 1, glfw_ext_get_window_size_fn);
    get_primary_monitor_workarea_def = glfw_ext_make_function_def("glfwGetPrimaryMonitorWorkarea", monitor_workarea_tuple_type, NULL, 0, glfw_ext_get_primary_monitor_workarea_fn);
    get_primary_monitor_content_scale_def = glfw_ext_make_function_def("glfwGetPrimaryMonitorContentScale", cursor_pos_tuple_type, NULL, 0, glfw_ext_get_primary_monitor_content_scale_fn);
    get_window_attrib_params[0] = window_type;
    get_window_attrib_params[1] = int_type;
    get_window_attrib_def = glfw_ext_make_function_def("glfwGetWindowAttrib", int_type, get_window_attrib_params, 2, glfw_ext_get_window_attrib_fn);
    get_framebuffer_size_def = glfw_ext_make_function_def("glfwGetFramebufferSize", window_size_tuple_type, &window_type, 1, glfw_ext_get_framebuffer_size_fn);
    get_window_pos_def = glfw_ext_make_function_def("glfwGetWindowPos", window_size_tuple_type, &window_type, 1, glfw_ext_get_window_pos_fn);
    set_window_size_params[0] = window_type;
    set_window_size_params[1] = int_type;
    set_window_size_params[2] = int_type;
    set_window_size_def = glfw_ext_make_function_def("glfwSetWindowSize", bool_type, set_window_size_params, 3, glfw_ext_set_window_size_fn);
    set_window_pos_params[0] = window_type;
    set_window_pos_params[1] = int_type;
    set_window_pos_params[2] = int_type;
    set_window_pos_def = glfw_ext_make_function_def("glfwSetWindowPos", bool_type, set_window_pos_params, 3, glfw_ext_set_window_pos_fn);
    show_window_def = glfw_ext_make_function_def("glfwShowWindow", bool_type, &window_type, 1, glfw_ext_show_window_fn);
    iconify_window_def = glfw_ext_make_function_def("glfwIconifyWindow", bool_type, &window_type, 1, glfw_ext_iconify_window_fn);
    hide_window_def = glfw_ext_make_function_def("glfwHideWindow", bool_type, &window_type, 1, glfw_ext_hide_window_fn);
    restore_window_def = glfw_ext_make_function_def("glfwRestoreWindow", bool_type, &window_type, 1, glfw_ext_restore_window_fn);
    maximize_window_def = glfw_ext_make_function_def("glfwMaximizeWindow", bool_type, &window_type, 1, glfw_ext_maximize_window_fn);
    focus_window_def = glfw_ext_make_function_def("glfwFocusWindow", bool_type, &window_type, 1, glfw_ext_focus_window_fn);
    make_context_current_def = glfw_ext_make_function_def("glfwMakeContextCurrent", bool_type, &window_type, 1, glfw_ext_make_context_current_fn);
    clear_current_context_def = glfw_ext_make_function_def("glfwClearCurrentContext", bool_type, NULL, 0, glfw_ext_clear_current_context_fn);
    has_any_current_context_def = glfw_ext_make_function_def("glfwHasAnyCurrentContext", bool_type, NULL, 0, glfw_ext_has_any_current_context_fn);
    has_current_context_def = glfw_ext_make_function_def("glfwHasCurrentContext", bool_type, &window_type, 1, glfw_ext_has_current_context_fn);
    swap_buffers_def = glfw_ext_make_function_def("glfwSwapBuffers", bool_type, &window_type, 1, glfw_ext_swap_buffers_fn);
    swap_interval_params[0] = int_type;
    swap_interval_def = glfw_ext_make_function_def("glfwSwapInterval", bool_type, swap_interval_params, 1, glfw_ext_swap_interval_fn);
    set_window_title_params[0] = window_type;
    set_window_title_params[1] = string_type;
    set_window_title_def = glfw_ext_make_function_def("glfwSetWindowTitle", bool_type, set_window_title_params, 2, glfw_ext_set_window_title_fn);
    get_cursor_pos_def = glfw_ext_make_function_def("glfwGetCursorPos", cursor_pos_tuple_type, &window_type, 1, glfw_ext_get_cursor_pos_fn);
    get_window_content_scale_def = glfw_ext_make_function_def("glfwGetWindowContentScale", cursor_pos_tuple_type, &window_type, 1, glfw_ext_get_window_content_scale_fn);
    set_cursor_pos_params[0] = window_type;
    set_cursor_pos_params[1] = double_type;
    set_cursor_pos_params[2] = double_type;
    set_cursor_pos_def = glfw_ext_make_function_def("glfwSetCursorPos", bool_type, set_cursor_pos_params, 3, glfw_ext_set_cursor_pos_fn);
    raw_mouse_motion_supported_def = glfw_ext_make_function_def("glfwRawMouseMotionSupported", bool_type, NULL, 0, glfw_ext_raw_mouse_motion_supported_fn);
    get_raw_mouse_motion_def = glfw_ext_make_function_def("glfwGetRawMouseMotion", bool_type, &window_type, 1, glfw_ext_get_raw_mouse_motion_fn);
    set_raw_mouse_motion_params[0] = window_type;
    set_raw_mouse_motion_params[1] = bool_type;
    set_raw_mouse_motion_def = glfw_ext_make_function_def("glfwSetRawMouseMotion", bool_type, set_raw_mouse_motion_params, 2, glfw_ext_set_raw_mouse_motion_fn);
    get_cursor_mode_def = glfw_ext_make_function_def("glfwGetCursorMode", int_type, &window_type, 1, glfw_ext_get_cursor_mode_fn);
    set_cursor_mode_params[0] = window_type;
    set_cursor_mode_params[1] = int_type;
    set_cursor_mode_def = glfw_ext_make_function_def("glfwSetCursorMode", bool_type, set_cursor_mode_params, 2, glfw_ext_set_cursor_mode_fn);
    cursor_mode_normal_def = glfw_ext_make_function_def("glfwCursorModeNormal", int_type, NULL, 0, glfw_ext_cursor_mode_normal_fn);
    cursor_mode_hidden_def = glfw_ext_make_function_def("glfwCursorModeHidden", int_type, NULL, 0, glfw_ext_cursor_mode_hidden_fn);
    cursor_mode_disabled_def = glfw_ext_make_function_def("glfwCursorModeDisabled", int_type, NULL, 0, glfw_ext_cursor_mode_disabled_fn);
    window_attrib_visible_def = glfw_ext_make_function_def("glfwWindowAttribVisible", int_type, NULL, 0, glfw_ext_window_attrib_visible_fn);
    window_attrib_focused_def = glfw_ext_make_function_def("glfwWindowAttribFocused", int_type, NULL, 0, glfw_ext_window_attrib_focused_fn);
    window_attrib_iconified_def = glfw_ext_make_function_def("glfwWindowAttribIconified", int_type, NULL, 0, glfw_ext_window_attrib_iconified_fn);
    window_attrib_maximized_def = glfw_ext_make_function_def("glfwWindowAttribMaximized", int_type, NULL, 0, glfw_ext_window_attrib_maximized_fn);
    get_key_params[0] = window_type;
    get_key_params[1] = int_type;
    get_key_def = glfw_ext_make_function_def("glfwGetKey", int_type, get_key_params, 2, glfw_ext_get_key_fn);
    get_mouse_button_params[0] = window_type;
    get_mouse_button_params[1] = int_type;
    get_mouse_button_def = glfw_ext_make_function_def("glfwGetMouseButton", int_type, get_mouse_button_params, 2, glfw_ext_get_mouse_button_fn);
    poll_events_def = glfw_ext_make_function_def("glfwPollEvents", bool_type, NULL, 0, glfw_ext_poll_events_fn);
    wait_events_timeout_params[0] = double_type;
    wait_events_timeout_def = glfw_ext_make_function_def("glfwWaitEventsTimeout", bool_type, wait_events_timeout_params, 1, glfw_ext_wait_events_timeout_fn);
    post_empty_event_def = glfw_ext_make_function_def("glfwPostEmptyEvent", bool_type, NULL, 0, glfw_ext_post_empty_event_fn);
    clear_input_callback_params[0] = window_type;
    clear_input_callback_def = glfw_ext_make_function_def("glfwClearInputCallback", bool_type, clear_input_callback_params, 1, glfw_ext_clear_input_callback_fn);
    clear_window_callback_params[0] = window_type;
    clear_window_callback_def = glfw_ext_make_function_def("glfwClearWindowCallback", bool_type, clear_window_callback_params, 1, glfw_ext_clear_window_callback_fn);
    has_input_callback_params[0] = window_type;
    has_input_callback_def = glfw_ext_make_function_def("glfwHasInputCallback", bool_type, has_input_callback_params, 1, glfw_ext_has_input_callback_fn);
    has_window_callback_params[0] = window_type;
    has_window_callback_def = glfw_ext_make_function_def("glfwHasWindowCallback", bool_type, has_window_callback_params, 1, glfw_ext_has_window_callback_fn);
    set_callback_queue_limit_params[0] = window_type;
    set_callback_queue_limit_params[1] = int_type;
    set_callback_queue_limit_def = glfw_ext_make_function_def("glfwSetCallbackQueueLimit", bool_type, set_callback_queue_limit_params, 2, glfw_ext_set_callback_queue_limit_fn);
    get_callback_queue_limit_params[0] = window_type;
    get_callback_queue_limit_def = glfw_ext_make_function_def("glfwGetCallbackQueueLimit", int_type, get_callback_queue_limit_params, 1, glfw_ext_get_callback_queue_limit_fn);
    get_dropped_callback_count_params[0] = window_type;
    get_dropped_callback_count_def = glfw_ext_make_function_def("glfwGetDroppedCallbackCount", int_type, get_dropped_callback_count_params, 1, glfw_ext_get_dropped_callback_count_fn);
    reset_dropped_callback_count_params[0] = window_type;
    reset_dropped_callback_count_def = glfw_ext_make_function_def("glfwResetDroppedCallbackCount", int_type, reset_dropped_callback_count_params, 1, glfw_ext_reset_dropped_callback_count_fn);
    can_post_callbacks_params[0] = window_type;
    can_post_callbacks_def = glfw_ext_make_function_def("glfwCanPostCallbacks", bool_type, can_post_callbacks_params, 1, glfw_ext_can_post_callbacks_fn);
    get_invalidated_callback_count_params[0] = window_type;
    get_invalidated_callback_count_def = glfw_ext_make_function_def("glfwGetInvalidatedCallbackCount", int_type, get_invalidated_callback_count_params, 1, glfw_ext_get_invalidated_callback_count_fn);
    reset_invalidated_callback_count_params[0] = window_type;
    reset_invalidated_callback_count_def = glfw_ext_make_function_def("glfwResetInvalidatedCallbackCount", int_type, reset_invalidated_callback_count_params, 1, glfw_ext_reset_invalidated_callback_count_fn);
    get_rejected_callback_count_params[0] = window_type;
    get_rejected_callback_count_def = glfw_ext_make_function_def("glfwGetRejectedCallbackCount", int_type, get_rejected_callback_count_params, 1, glfw_ext_get_rejected_callback_count_fn);
    reset_rejected_callback_count_params[0] = window_type;
    reset_rejected_callback_count_def = glfw_ext_make_function_def("glfwResetRejectedCallbackCount", int_type, reset_rejected_callback_count_params, 1, glfw_ext_reset_rejected_callback_count_fn);
    get_callback_queue_pending_count_params[0] = window_type;
    get_callback_queue_pending_count_def = glfw_ext_make_function_def("glfwGetCallbackQueuePendingCount", int_type, get_callback_queue_pending_count_params, 1, glfw_ext_get_callback_queue_pending_count_fn);
    set_input_callback_params[0] = window_type;
    set_input_callback_params[1] = input_event_callback_type;
    set_input_callback_def = glfw_ext_make_function_def("glfwSetInputCallback", bool_type, set_input_callback_params, 2, glfw_ext_set_input_callback_fn);
    set_window_callback_params[0] = window_type;
    set_window_callback_params[1] = window_event_callback_type;
    set_window_callback_def = glfw_ext_make_function_def("glfwSetWindowCallback", bool_type, set_window_callback_params, 2, glfw_ext_set_window_callback_fn);
    test_emit_scroll_params[0] = window_type;
    test_emit_scroll_params[1] = double_type;
    test_emit_scroll_params[2] = double_type;
    test_emit_scroll_def = glfw_ext_make_function_def("glfwTestEmitScroll", bool_type, test_emit_scroll_params, 3, glfw_ext_test_emit_scroll_fn);
    test_emit_cursor_enter_params[0] = window_type;
    test_emit_cursor_enter_params[1] = bool_type;
    test_emit_cursor_enter_def = glfw_ext_make_function_def("glfwTestEmitCursorEnter", bool_type, test_emit_cursor_enter_params, 2, glfw_ext_test_emit_cursor_enter_fn);
    test_emit_char_params[0] = window_type;
    test_emit_char_params[1] = int_type;
    test_emit_char_def = glfw_ext_make_function_def("glfwTestEmitChar", bool_type, test_emit_char_params, 2, glfw_ext_test_emit_char_fn);
    test_emit_key_params[0] = window_type;
    test_emit_key_params[1] = int_type;
    test_emit_key_params[2] = bool_type;
    test_emit_key_def = glfw_ext_make_function_def("glfwTestEmitKey", bool_type, test_emit_key_params, 3, glfw_ext_test_emit_key_fn);
    test_emit_mouse_button_params[0] = window_type;
    test_emit_mouse_button_params[1] = int_type;
    test_emit_mouse_button_params[2] = bool_type;
    test_emit_mouse_button_def = glfw_ext_make_function_def("glfwTestEmitMouseButton", bool_type, test_emit_mouse_button_params, 3, glfw_ext_test_emit_mouse_button_fn);

    function_defs[0] = &init_def;
    function_defs[1] = &terminate_def;
    function_defs[2] = &get_last_error_def;
    function_defs[3] = &get_version_string_def;
    function_defs[4] = &get_time_def;
    function_defs[5] = &is_initialized_def;
    function_defs[6] = &get_live_window_count_def;
    function_defs[7] = &can_terminate_def;
    function_defs[8] = &force_terminate_def;
    function_defs[9] = &create_window_def;
    function_defs[10] = &create_window_with_context_def;
    function_defs[11] = &destroy_window_def;
    function_defs[12] = &window_should_close_def;
    function_defs[13] = &set_window_should_close_def;
    function_defs[14] = &get_window_size_def;
    function_defs[15] = &get_primary_monitor_workarea_def;
    function_defs[16] = &get_primary_monitor_content_scale_def;
    function_defs[17] = &get_window_attrib_def;
    function_defs[18] = &get_framebuffer_size_def;
    function_defs[19] = &get_window_pos_def;
    function_defs[20] = &set_window_size_def;
    function_defs[21] = &set_window_pos_def;
    function_defs[22] = &show_window_def;
    function_defs[23] = &iconify_window_def;
    function_defs[24] = &hide_window_def;
    function_defs[25] = &restore_window_def;
    function_defs[26] = &maximize_window_def;
    function_defs[27] = &focus_window_def;
    function_defs[28] = &make_context_current_def;
    function_defs[29] = &clear_current_context_def;
    function_defs[30] = &has_any_current_context_def;
    function_defs[31] = &has_current_context_def;
    function_defs[32] = &swap_buffers_def;
    function_defs[33] = &swap_interval_def;
    function_defs[34] = &set_window_title_def;
    function_defs[35] = &get_cursor_pos_def;
    function_defs[36] = &get_window_content_scale_def;
    function_defs[37] = &set_cursor_pos_def;
    function_defs[38] = &raw_mouse_motion_supported_def;
    function_defs[39] = &get_raw_mouse_motion_def;
    function_defs[40] = &set_raw_mouse_motion_def;
    function_defs[41] = &get_cursor_mode_def;
    function_defs[42] = &set_cursor_mode_def;
    function_defs[43] = &cursor_mode_normal_def;
    function_defs[44] = &cursor_mode_hidden_def;
    function_defs[45] = &cursor_mode_disabled_def;
    function_defs[46] = &window_attrib_visible_def;
    function_defs[47] = &window_attrib_focused_def;
    function_defs[48] = &window_attrib_iconified_def;
    function_defs[49] = &window_attrib_maximized_def;
    function_defs[50] = &get_key_def;
    function_defs[51] = &get_mouse_button_def;
    function_defs[52] = &poll_events_def;
    function_defs[53] = &wait_events_timeout_def;
    function_defs[54] = &post_empty_event_def;
    function_defs[55] = &clear_input_callback_def;
    function_defs[56] = &clear_window_callback_def;
    function_defs[57] = &has_input_callback_def;
    function_defs[58] = &has_window_callback_def;
    function_defs[59] = &set_callback_queue_limit_def;
    function_defs[60] = &get_callback_queue_limit_def;
    function_defs[61] = &get_dropped_callback_count_def;
    function_defs[62] = &reset_dropped_callback_count_def;
    function_defs[63] = &can_post_callbacks_def;
    function_defs[64] = &get_invalidated_callback_count_def;
    function_defs[65] = &reset_invalidated_callback_count_def;
    function_defs[66] = &get_rejected_callback_count_def;
    function_defs[67] = &reset_rejected_callback_count_def;
    function_defs[68] = &get_callback_queue_pending_count_def;
    function_defs[69] = &set_input_callback_def;
    function_defs[70] = &set_window_callback_def;
    function_defs[71] = &test_emit_scroll_def;
    function_defs[72] = &test_emit_cursor_enter_def;
    function_defs[73] = &test_emit_char_def;
    function_defs[74] = &test_emit_key_def;
    function_defs[75] = &test_emit_mouse_button_def;

    for (int i = 0; i < 76; i++) {
        if (!registrar->register_function(registrar->host_context, function_defs[i], error_buf, error_buf_size)) {
            return false;
        }
    }

    glfw_ext_clear_last_error();
    return true;
}

TABLO_EXT_EXPORT int tablo_glfw_test_emit_scroll_last_window(double xoffset, double yoffset) {
    GlfwWindowHandle* handle = NULL;
    glfw_ext_lock();
    handle = g_glfw_test_last_window;
    glfw_ext_unlock();
    if (!handle || !handle->window) {
        return 0;
    }
    return glfw_ext_post_scroll_event(handle, xoffset, yoffset, "test", "input") ? 1 : 0;
}

TABLO_EXT_EXPORT int tablo_glfw_test_emit_window_resize_last_window(int width, int height) {
    GlfwWindowHandle* handle = NULL;
    bool focused = false;
    glfw_ext_lock();
    handle = g_glfw_test_last_window;
    glfw_ext_unlock();
    if (!handle || !handle->window) {
        return 0;
    }
    focused = glfwGetWindowAttrib(handle->window, GLFW_FOCUSED) == GLFW_TRUE;
    return glfw_ext_post_window_event(handle, "resize", (int64_t)width, (int64_t)height, focused, "test", "window") ? 1 : 0;
}

TABLO_EXT_EXPORT int tablo_glfw_test_emit_scroll_live_window_at(int index, double xoffset, double yoffset) {
    GlfwWindowHandle* handle = glfw_ext_test_window_at_live_index(index);
    if (!handle || !handle->window) {
        return 0;
    }
    return glfw_ext_post_scroll_event(handle, xoffset, yoffset, "test", "input") ? 1 : 0;
}

TABLO_EXT_EXPORT int tablo_glfw_test_emit_window_resize_live_window_at(int index, int width, int height) {
    GlfwWindowHandle* handle = glfw_ext_test_window_at_live_index(index);
    bool focused = false;
    if (!handle || !handle->window) {
        return 0;
    }
    focused = glfwGetWindowAttrib(handle->window, GLFW_FOCUSED) == GLFW_TRUE;
    return glfw_ext_post_window_event(handle, "resize", (int64_t)width, (int64_t)height, focused, "test", "window") ? 1 : 0;
}

TABLO_EXT_EXPORT int tablo_glfw_test_clear_last_window_input_callback(void) {
    GlfwWindowHandle* handle = NULL;
    glfw_ext_lock();
    handle = g_glfw_test_last_window;
    glfw_ext_unlock();
    if (!handle) {
        return 0;
    }
    glfw_ext_clear_input_callback_handle(handle);
    return 1;
}

TABLO_EXT_EXPORT int tablo_glfw_test_clear_input_callback_live_window_at(int index) {
    GlfwWindowHandle* handle = glfw_ext_test_window_at_live_index(index);
    if (!handle) {
        return 0;
    }
    glfw_ext_clear_input_callback_handle(handle);
    return 1;
}

TABLO_EXT_EXPORT int tablo_glfw_test_get_last_window_can_post_callbacks(void) {
    GlfwWindowHandle* handle = NULL;
    const TabloExtCallback* callback = NULL;
    glfw_ext_lock();
    handle = g_glfw_test_last_window;
    glfw_ext_unlock();
    if (!handle) {
        return 0;
    }
    callback = glfw_ext_active_callback(handle);
    return callback != NULL && glfw_ext_callback_queue_is_open(callback) ? 1 : 0;
}

TABLO_EXT_EXPORT int tablo_glfw_test_get_live_window_can_post_callbacks_at(int index) {
    GlfwWindowHandle* handle = glfw_ext_test_window_at_live_index(index);
    const TabloExtCallback* callback = NULL;
    if (!handle) {
        return 0;
    }
    callback = glfw_ext_active_callback(handle);
    return callback != NULL && glfw_ext_callback_queue_is_open(callback) ? 1 : 0;
}

TABLO_EXT_EXPORT int tablo_glfw_test_get_last_window_pending_callback_count(void) {
    GlfwWindowHandle* handle = NULL;
    glfw_ext_lock();
    handle = g_glfw_test_last_window;
    glfw_ext_unlock();
    return handle ? glfw_ext_callback_pending_count_total(handle) : -1;
}

TABLO_EXT_EXPORT int tablo_glfw_test_get_live_window_pending_callback_count_at(int index) {
    GlfwWindowHandle* handle = glfw_ext_test_window_at_live_index(index);
    return handle ? glfw_ext_callback_pending_count_total(handle) : -1;
}

TABLO_EXT_EXPORT int64_t tablo_glfw_test_get_last_window_dropped_callback_count(void) {
    GlfwWindowHandle* handle = NULL;
    glfw_ext_lock();
    handle = g_glfw_test_last_window;
    glfw_ext_unlock();
    return handle ? handle->dropped_callback_count : -1;
}

TABLO_EXT_EXPORT int64_t tablo_glfw_test_get_live_window_dropped_callback_count_at(int index) {
    GlfwWindowHandle* handle = glfw_ext_test_window_at_live_index(index);
    return handle ? handle->dropped_callback_count : -1;
}

TABLO_EXT_EXPORT int64_t tablo_glfw_test_reset_last_window_dropped_callback_count(void) {
    GlfwWindowHandle* handle = NULL;
    int64_t previous = -1;
    glfw_ext_lock();
    handle = g_glfw_test_last_window;
    if (handle) {
        previous = handle->dropped_callback_count;
        handle->dropped_callback_count = 0;
    }
    glfw_ext_unlock();
    return previous;
}

TABLO_EXT_EXPORT int64_t tablo_glfw_test_reset_live_window_dropped_callback_count_at(int index) {
    GlfwWindowHandle* handle = glfw_ext_test_window_at_live_index(index);
    int64_t previous = -1;
    if (!handle) return -1;
    glfw_ext_lock();
    previous = handle->dropped_callback_count;
    handle->dropped_callback_count = 0;
    glfw_ext_unlock();
    return previous;
}

TABLO_EXT_EXPORT int64_t tablo_glfw_test_get_last_window_invalidated_callback_count(void) {
    GlfwWindowHandle* handle = NULL;
    glfw_ext_lock();
    handle = g_glfw_test_last_window;
    glfw_ext_unlock();
    return handle ? glfw_ext_get_invalidated_callback_count_total(handle) : -1;
}

TABLO_EXT_EXPORT int64_t tablo_glfw_test_get_live_window_invalidated_callback_count_at(int index) {
    GlfwWindowHandle* handle = glfw_ext_test_window_at_live_index(index);
    return handle ? glfw_ext_get_invalidated_callback_count_total(handle) : -1;
}

TABLO_EXT_EXPORT int64_t tablo_glfw_test_reset_last_window_invalidated_callback_count(void) {
    GlfwWindowHandle* handle = NULL;
    int64_t previous = -1;
    glfw_ext_lock();
    handle = g_glfw_test_last_window;
    if (handle) {
        previous = glfw_ext_reset_invalidated_callback_count_total(handle);
    }
    glfw_ext_unlock();
    return previous;
}

TABLO_EXT_EXPORT int64_t tablo_glfw_test_reset_live_window_invalidated_callback_count_at(int index) {
    GlfwWindowHandle* handle = glfw_ext_test_window_at_live_index(index);
    int64_t previous = -1;
    if (!handle) return -1;
    glfw_ext_lock();
    previous = glfw_ext_reset_invalidated_callback_count_total(handle);
    glfw_ext_unlock();
    return previous;
}

TABLO_EXT_EXPORT int64_t tablo_glfw_test_get_last_window_rejected_callback_count(void) {
    GlfwWindowHandle* handle = NULL;
    glfw_ext_lock();
    handle = g_glfw_test_last_window;
    glfw_ext_unlock();
    return handle ? handle->rejected_callback_count : -1;
}

TABLO_EXT_EXPORT int64_t tablo_glfw_test_get_live_window_rejected_callback_count_at(int index) {
    GlfwWindowHandle* handle = glfw_ext_test_window_at_live_index(index);
    return handle ? handle->rejected_callback_count : -1;
}

TABLO_EXT_EXPORT int64_t tablo_glfw_test_reset_last_window_rejected_callback_count(void) {
    GlfwWindowHandle* handle = NULL;
    int64_t previous = -1;
    glfw_ext_lock();
    handle = g_glfw_test_last_window;
    if (handle) {
        previous = handle->rejected_callback_count;
        handle->rejected_callback_count = 0;
    }
    glfw_ext_unlock();
    return previous;
}

TABLO_EXT_EXPORT int64_t tablo_glfw_test_reset_live_window_rejected_callback_count_at(int index) {
    GlfwWindowHandle* handle = glfw_ext_test_window_at_live_index(index);
    int64_t previous = -1;
    if (!handle) return -1;
    glfw_ext_lock();
    previous = handle->rejected_callback_count;
    handle->rejected_callback_count = 0;
    glfw_ext_unlock();
    return previous;
}

TABLO_EXT_EXPORT int tablo_glfw_test_is_initialized(void) {
    int initialized = 0;
    glfw_ext_lock();
    initialized = g_glfw_initialized ? 1 : 0;
    glfw_ext_unlock();
    return initialized;
}

TABLO_EXT_EXPORT int tablo_glfw_test_get_live_window_count(void) {
    int live_window_count = 0;
    glfw_ext_lock();
    live_window_count = g_glfw_live_window_count;
    glfw_ext_unlock();
    return live_window_count;
}

TABLO_EXT_EXPORT void tablo_extension_shutdown(void) {
    (void)glfw_ext_force_terminate_all_windows();
}
