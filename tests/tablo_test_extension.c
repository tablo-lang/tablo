#include "tablo_ext.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

typedef struct {
    int64_t value;
} CounterHandle;

typedef struct {
    const TabloExtCallback* callback;
} StoredIntCallbackHandle;

typedef struct {
    thrd_t thread;
    mtx_t mutex;
    const TabloExtCallback* callback;
    int64_t value;
    int delay_ms;
    bool joinable;
    bool done;
    bool post_ok;
    char error[256];
} PostedIntCallbackWorkerHandle;

typedef struct {
    thrd_t thread;
    mtx_t mutex;
    const TabloExtCallback* callback;
    int64_t value;
    char* label;
    int delay_ms;
    bool joinable;
    bool done;
    bool post_ok;
    char error[256];
} PostedTupleCallbackWorkerHandle;

typedef struct {
    thrd_t thread;
    mtx_t mutex;
    const TabloExtCallback* callback;
    int64_t value;
    int delay_ms;
    bool joinable;
    bool done;
    bool post_ok;
    char error[256];
} PostedArrayCallbackWorkerHandle;

typedef struct {
    thrd_t thread;
    mtx_t mutex;
    const TabloExtCallback* callback;
    int64_t value;
    char* label;
    int delay_ms;
    bool joinable;
    bool done;
    bool post_ok;
    char error[256];
} PostedMapCallbackWorkerHandle;

typedef struct {
    thrd_t thread;
    mtx_t mutex;
    const TabloExtCallback* callback;
    int64_t value;
    int delay_ms;
    bool joinable;
    bool done;
    bool post_ok;
    char error[256];
} PostedEventBatchCallbackWorkerHandle;

static void (*g_ext_retain_callback)(const TabloExtCallback* callback) = NULL;
static void (*g_ext_release_callback)(const TabloExtCallback* callback) = NULL;
static bool (*g_ext_post_callback)(const TabloExtCallback* callback,
                                   const TabloExtValue* args,
                                   int arg_count,
                                   char* error_buf,
                                   size_t error_buf_size) = NULL;

static void ext_set_init_error(char* error_buf, size_t error_buf_size, const char* message) {
    if (!error_buf || error_buf_size == 0) return;
    snprintf(error_buf, error_buf_size, "%s", message ? message : "Unknown extension error");
}

static void ext_set_runtime_error(TabloExtCallContext* ctx, const char* message) {
    if (!ctx || !ctx->api || !ctx->api->set_runtime_error) return;
    ctx->api->set_runtime_error(ctx, message ? message : "Extension runtime error");
}

static TabloExtTypeDesc ext_make_type(TabloExtTypeTag tag, const char* handle_type_name, bool nullable) {
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

static TabloExtTypeDesc ext_make_array_type(const TabloExtTypeDesc* element_type, bool nullable) {
    TabloExtTypeDesc type;
    type.tag = TABLO_EXT_TYPE_ARRAY;
    type.handle_type_name = NULL;
    type.element_type = element_type;
    type.tuple_element_types = NULL;
    type.tuple_element_count = 0;
    type.callback_param_types = NULL;
    type.callback_param_count = 0;
    type.callback_result_type = NULL;
    type.nullable = nullable;
    return type;
}

static TabloExtTypeDesc ext_make_tuple_type(const TabloExtTypeDesc* element_types,
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

static TabloExtTypeDesc ext_make_map_type(bool nullable) {
    TabloExtTypeDesc type;
    type.tag = TABLO_EXT_TYPE_MAP;
    type.handle_type_name = NULL;
    type.element_type = NULL;
    type.tuple_element_types = NULL;
    type.tuple_element_count = 0;
    type.callback_param_types = NULL;
    type.callback_param_count = 0;
    type.callback_result_type = NULL;
    type.nullable = nullable;
    return type;
}

static TabloExtTypeDesc ext_make_callback_type(const TabloExtTypeDesc* result_type,
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

static TabloExtFunctionDef ext_make_function_def(const char* name,
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

static CounterHandle* ext_counter_from_arg(TabloExtCallContext* ctx, int index) {
    void* payload = NULL;
    if (!ctx || !ctx->api || !ctx->api->get_handle_arg) return NULL;
    if (!ctx->api->get_handle_arg(ctx, index, "Counter", &payload)) return NULL;
    if (!payload) {
        ext_set_runtime_error(ctx, "Counter handle payload is null");
        return NULL;
    }
    return (CounterHandle*)payload;
}

static void ext_counter_destroy(void* payload) {
    if (payload) free(payload);
}

static void ext_stored_int_callback_destroy(void* payload) {
    StoredIntCallbackHandle* handle = (StoredIntCallbackHandle*)payload;
    if (!handle) return;
    if (handle->callback && g_ext_release_callback) {
        g_ext_release_callback(handle->callback);
        handle->callback = NULL;
    }
    free(handle);
}

static void ext_sleep_ms(int delay_ms) {
    if (delay_ms <= 0) return;
    struct timespec req;
    req.tv_sec = delay_ms / 1000;
    req.tv_nsec = (long)(delay_ms % 1000) * 1000000L;
    while (thrd_sleep(&req, &req) == -1) {
    }
}

static void ext_posted_int_callback_worker_destroy(void* payload) {
    PostedIntCallbackWorkerHandle* handle = (PostedIntCallbackWorkerHandle*)payload;
    if (!handle) return;

    if (handle->joinable) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
    }
    if (handle->callback && g_ext_release_callback) {
        g_ext_release_callback(handle->callback);
        handle->callback = NULL;
    }
    mtx_destroy(&handle->mutex);
    free(handle);
}

static void ext_posted_tuple_callback_worker_destroy(void* payload) {
    PostedTupleCallbackWorkerHandle* handle = (PostedTupleCallbackWorkerHandle*)payload;
    if (!handle) return;

    if (handle->joinable) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
    }
    if (handle->callback && g_ext_release_callback) {
        g_ext_release_callback(handle->callback);
        handle->callback = NULL;
    }
    if (handle->label) {
        free(handle->label);
        handle->label = NULL;
    }
    mtx_destroy(&handle->mutex);
    free(handle);
}

static void ext_posted_array_callback_worker_destroy(void* payload) {
    PostedArrayCallbackWorkerHandle* handle = (PostedArrayCallbackWorkerHandle*)payload;
    if (!handle) return;

    if (handle->joinable) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
    }
    if (handle->callback && g_ext_release_callback) {
        g_ext_release_callback(handle->callback);
        handle->callback = NULL;
    }
    mtx_destroy(&handle->mutex);
    free(handle);
}

static void ext_posted_map_callback_worker_destroy(void* payload) {
    PostedMapCallbackWorkerHandle* handle = (PostedMapCallbackWorkerHandle*)payload;
    if (!handle) return;

    if (handle->joinable) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
    }
    if (handle->callback && g_ext_release_callback) {
        g_ext_release_callback(handle->callback);
        handle->callback = NULL;
    }
    if (handle->label) {
        free(handle->label);
        handle->label = NULL;
    }
    mtx_destroy(&handle->mutex);
    free(handle);
}

static void ext_posted_event_batch_callback_worker_destroy(void* payload) {
    PostedEventBatchCallbackWorkerHandle* handle = (PostedEventBatchCallbackWorkerHandle*)payload;
    if (!handle) return;

    if (handle->joinable) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
    }
    if (handle->callback && g_ext_release_callback) {
        g_ext_release_callback(handle->callback);
        handle->callback = NULL;
    }
    mtx_destroy(&handle->mutex);
    free(handle);
}

static void ext_fill_event_batch_payload(int64_t base,
                                         const char* source,
                                         TabloExtValue batch_items[2],
                                         TabloExtMapEntry event_entries[2][3],
                                         TabloExtMapEntry meta_entries[2][3],
                                         TabloExtValue meta_phase_items[2][2]) {
    const char* phase_names[2] = {"input", "frame"};
    int phase_lengths[2] = {5, 5};
    memset(batch_items, 0, sizeof(TabloExtValue) * 2u);
    memset(event_entries, 0, sizeof(TabloExtMapEntry) * 6u);
    memset(meta_entries, 0, sizeof(TabloExtMapEntry) * 6u);
    memset(meta_phase_items, 0, sizeof(TabloExtValue) * 4u);

    for (int i = 0; i < 2; i++) {
        TabloExtEventMetaSpec meta_spec;
        TabloExtNamedEventSpec event_spec;
        TabloExtValue meta_value;

        memset(&meta_spec, 0, sizeof(meta_spec));
        memset(&event_spec, 0, sizeof(event_spec));
        memset(&meta_value, 0, sizeof(meta_value));

        meta_spec.source_chars = source ? source : "";
        meta_spec.source_length = source ? (int)strlen(source) : 0;
        meta_spec.priority = i + 1;
        meta_spec.phases.chars = phase_names;
        meta_spec.phases.lengths = phase_lengths;
        meta_spec.phases.count = 2;

        event_spec.name_chars = i == 0 ? "down" : "up";
        event_spec.name_length = i == 0 ? 4 : 2;
        event_spec.delta = base + i;

        (void)tablo_ext_build_event_meta_map(&meta_value, meta_entries[i], 3, meta_phase_items[i], 2, &meta_spec);
        event_spec.meta_value = meta_value;
        (void)tablo_ext_build_named_event_map(&batch_items[i], event_entries[i], 3, &event_spec);
    }
}

static int ext_posted_int_callback_worker_main(void* user_data) {
    PostedIntCallbackWorkerHandle* handle = (PostedIntCallbackWorkerHandle*)user_data;
    TabloExtValue arg;
    bool post_ok = false;
    char error_buf[256];

    if (!handle) return 0;

    ext_sleep_ms(handle->delay_ms);

    memset(&arg, 0, sizeof(arg));
    memset(error_buf, 0, sizeof(error_buf));
    arg.tag = TABLO_EXT_TYPE_INT;
    arg.as.int_value = handle->value;

    if (g_ext_post_callback && handle->callback) {
        post_ok = g_ext_post_callback(handle->callback, &arg, 1, error_buf, sizeof(error_buf));
    } else {
        snprintf(error_buf, sizeof(error_buf), "%s", "Callback post hook is not initialized");
    }

    mtx_lock(&handle->mutex);
    handle->post_ok = post_ok;
    handle->done = true;
    if (!post_ok) {
        snprintf(handle->error, sizeof(handle->error), "%s", error_buf[0] ? error_buf : "Failed to post queued callback");
    } else {
        handle->error[0] = '\0';
    }
    if (handle->callback && g_ext_release_callback) {
        g_ext_release_callback(handle->callback);
        handle->callback = NULL;
    }
    mtx_unlock(&handle->mutex);
    return 0;
}

static int ext_posted_tuple_callback_worker_main(void* user_data) {
    PostedTupleCallbackWorkerHandle* handle = (PostedTupleCallbackWorkerHandle*)user_data;
    TabloExtValue tuple_items[2];
    TabloExtValue arg;
    bool post_ok = false;
    char error_buf[256];

    if (!handle) return 0;

    ext_sleep_ms(handle->delay_ms);

    memset(tuple_items, 0, sizeof(tuple_items));
    memset(&arg, 0, sizeof(arg));
    memset(error_buf, 0, sizeof(error_buf));

    tuple_items[0].tag = TABLO_EXT_TYPE_INT;
    tuple_items[0].as.int_value = handle->value;
    tuple_items[1].tag = TABLO_EXT_TYPE_STRING;
    tuple_items[1].as.string_value.chars = handle->label ? handle->label : "";
    tuple_items[1].as.string_value.length = handle->label ? (int)strlen(handle->label) : 0;

    arg.tag = TABLO_EXT_TYPE_TUPLE;
    arg.as.tuple_value.items = tuple_items;
    arg.as.tuple_value.count = 2;

    if (g_ext_post_callback && handle->callback) {
        post_ok = g_ext_post_callback(handle->callback, &arg, 1, error_buf, sizeof(error_buf));
    } else {
        snprintf(error_buf, sizeof(error_buf), "%s", "Callback post hook is not initialized");
    }

    mtx_lock(&handle->mutex);
    handle->post_ok = post_ok;
    handle->done = true;
    if (!post_ok) {
        snprintf(handle->error, sizeof(handle->error), "%s", error_buf[0] ? error_buf : "Failed to post queued tuple callback");
    } else {
        handle->error[0] = '\0';
    }
    if (handle->callback && g_ext_release_callback) {
        g_ext_release_callback(handle->callback);
        handle->callback = NULL;
    }
    mtx_unlock(&handle->mutex);
    return 0;
}

static int ext_posted_array_callback_worker_main(void* user_data) {
    PostedArrayCallbackWorkerHandle* handle = (PostedArrayCallbackWorkerHandle*)user_data;
    TabloExtValue array_items[3];
    TabloExtValue arg;
    bool post_ok = false;
    char error_buf[256];

    if (!handle) return 0;

    ext_sleep_ms(handle->delay_ms);

    memset(array_items, 0, sizeof(array_items));
    memset(&arg, 0, sizeof(arg));
    memset(error_buf, 0, sizeof(error_buf));

    array_items[0].tag = TABLO_EXT_TYPE_INT;
    array_items[0].as.int_value = handle->value;
    array_items[1].tag = TABLO_EXT_TYPE_INT;
    array_items[1].as.int_value = handle->value + 1;
    array_items[2].tag = TABLO_EXT_TYPE_INT;
    array_items[2].as.int_value = handle->value + 2;

    arg.tag = TABLO_EXT_TYPE_ARRAY;
    arg.as.array_value.items = array_items;
    arg.as.array_value.count = 3;

    if (g_ext_post_callback && handle->callback) {
        post_ok = g_ext_post_callback(handle->callback, &arg, 1, error_buf, sizeof(error_buf));
    } else {
        snprintf(error_buf, sizeof(error_buf), "%s", "Callback post hook is not initialized");
    }

    mtx_lock(&handle->mutex);
    handle->post_ok = post_ok;
    handle->done = true;
    if (!post_ok) {
        snprintf(handle->error, sizeof(handle->error), "%s", error_buf[0] ? error_buf : "Failed to post queued array callback");
    } else {
        handle->error[0] = '\0';
    }
    if (handle->callback && g_ext_release_callback) {
        g_ext_release_callback(handle->callback);
        handle->callback = NULL;
    }
    mtx_unlock(&handle->mutex);
    return 0;
}

static int ext_posted_map_callback_worker_main(void* user_data) {
    PostedMapCallbackWorkerHandle* handle = (PostedMapCallbackWorkerHandle*)user_data;
    TabloExtValue coords_items[2];
    TabloExtValue tag_items[2];
    TabloExtValue meta_phase_items[2];
    TabloExtMapEntry meta_entries[3];
    TabloExtMapEntry map_entries[6];
    TabloExtValue arg;
    bool post_ok = false;
    char error_buf[256];

    if (!handle) return 0;

    ext_sleep_ms(handle->delay_ms);

    memset(coords_items, 0, sizeof(coords_items));
    memset(tag_items, 0, sizeof(tag_items));
    memset(meta_phase_items, 0, sizeof(meta_phase_items));
    memset(meta_entries, 0, sizeof(meta_entries));
    memset(map_entries, 0, sizeof(map_entries));
    memset(&arg, 0, sizeof(arg));
    memset(error_buf, 0, sizeof(error_buf));

    map_entries[0].key_chars = "delta";
    map_entries[0].key_length = 5;
    map_entries[0].value.tag = TABLO_EXT_TYPE_INT;
    map_entries[0].value.as.int_value = handle->value;

    map_entries[1].key_chars = "label";
    map_entries[1].key_length = 5;
    map_entries[1].value.tag = TABLO_EXT_TYPE_STRING;
    map_entries[1].value.as.string_value.chars = handle->label ? handle->label : "";
    map_entries[1].value.as.string_value.length = handle->label ? (int)strlen(handle->label) : 0;

    map_entries[2].key_chars = "active";
    map_entries[2].key_length = 6;
    map_entries[2].value.tag = TABLO_EXT_TYPE_BOOL;
    map_entries[2].value.as.bool_value = true;

    coords_items[0].tag = TABLO_EXT_TYPE_INT;
    coords_items[0].as.int_value = handle->value;
    coords_items[1].tag = TABLO_EXT_TYPE_INT;
    coords_items[1].as.int_value = handle->value + 1;

    tag_items[0].tag = TABLO_EXT_TYPE_STRING;
    tag_items[0].as.string_value.chars = "native";
    tag_items[0].as.string_value.length = 6;
    tag_items[1].tag = TABLO_EXT_TYPE_STRING;
    tag_items[1].as.string_value.chars = handle->label ? handle->label : "";
    tag_items[1].as.string_value.length = handle->label ? (int)strlen(handle->label) : 0;

    map_entries[3].key_chars = "coords";
    map_entries[3].key_length = 6;
    map_entries[3].value.tag = TABLO_EXT_TYPE_TUPLE;
    map_entries[3].value.as.tuple_value.items = coords_items;
    map_entries[3].value.as.tuple_value.count = 2;

    map_entries[4].key_chars = "tags";
    map_entries[4].key_length = 4;
    map_entries[4].value.tag = TABLO_EXT_TYPE_ARRAY;
    map_entries[4].value.as.array_value.items = tag_items;
    map_entries[4].value.as.array_value.count = 2;

    meta_phase_items[0].tag = TABLO_EXT_TYPE_STRING;
    meta_phase_items[0].as.string_value.chars = "thread";
    meta_phase_items[0].as.string_value.length = 6;
    meta_phase_items[1].tag = TABLO_EXT_TYPE_STRING;
    meta_phase_items[1].as.string_value.chars = "queue";
    meta_phase_items[1].as.string_value.length = 5;

    meta_entries[0].key_chars = "source";
    meta_entries[0].key_length = 6;
    meta_entries[0].value.tag = TABLO_EXT_TYPE_STRING;
    meta_entries[0].value.as.string_value.chars = "worker";
    meta_entries[0].value.as.string_value.length = 6;
    meta_entries[1].key_chars = "priority";
    meta_entries[1].key_length = 8;
    meta_entries[1].value.tag = TABLO_EXT_TYPE_INT;
    meta_entries[1].value.as.int_value = 2;
    meta_entries[2].key_chars = "phases";
    meta_entries[2].key_length = 6;
    meta_entries[2].value.tag = TABLO_EXT_TYPE_ARRAY;
    meta_entries[2].value.as.array_value.items = meta_phase_items;
    meta_entries[2].value.as.array_value.count = 2;

    map_entries[5].key_chars = "meta";
    map_entries[5].key_length = 4;
    map_entries[5].value.tag = TABLO_EXT_TYPE_MAP;
    map_entries[5].value.as.map_value.entries = meta_entries;
    map_entries[5].value.as.map_value.count = 3;

    arg.tag = TABLO_EXT_TYPE_MAP;
    arg.as.map_value.entries = map_entries;
    arg.as.map_value.count = 6;

    if (g_ext_post_callback && handle->callback) {
        post_ok = g_ext_post_callback(handle->callback, &arg, 1, error_buf, sizeof(error_buf));
    } else {
        snprintf(error_buf, sizeof(error_buf), "%s", "Callback post hook is not initialized");
    }

    mtx_lock(&handle->mutex);
    handle->post_ok = post_ok;
    handle->done = true;
    if (!post_ok) {
        snprintf(handle->error, sizeof(handle->error), "%s", error_buf[0] ? error_buf : "Failed to post queued map callback");
    } else {
        handle->error[0] = '\0';
    }
    if (handle->callback && g_ext_release_callback) {
        g_ext_release_callback(handle->callback);
        handle->callback = NULL;
    }
    mtx_unlock(&handle->mutex);
    return 0;
}

static int ext_posted_event_batch_callback_worker_main(void* user_data) {
    PostedEventBatchCallbackWorkerHandle* handle = (PostedEventBatchCallbackWorkerHandle*)user_data;
    TabloExtValue batch_items[2];
    TabloExtMapEntry event_entries[2][3];
    TabloExtMapEntry meta_entries[2][3];
    TabloExtValue meta_phase_items[2][2];
    TabloExtValue arg;
    bool post_ok = false;
    char error_buf[256];

    if (!handle) return 0;

    ext_sleep_ms(handle->delay_ms);

    memset(&arg, 0, sizeof(arg));
    memset(error_buf, 0, sizeof(error_buf));
    ext_fill_event_batch_payload(handle->value,
                                 "batch",
                                 batch_items,
                                 event_entries,
                                 meta_entries,
                                 meta_phase_items);

    arg = tablo_ext_make_array_value(batch_items, 2);

    if (g_ext_post_callback && handle->callback) {
        post_ok = g_ext_post_callback(handle->callback, &arg, 1, error_buf, sizeof(error_buf));
    } else {
        snprintf(error_buf, sizeof(error_buf), "%s", "Callback post hook is not initialized");
    }

    mtx_lock(&handle->mutex);
    handle->post_ok = post_ok;
    handle->done = true;
    if (!post_ok) {
        snprintf(handle->error,
                 sizeof(handle->error),
                 "%s",
                 error_buf[0] ? error_buf : "Failed to post queued event batch callback");
    } else {
        handle->error[0] = '\0';
    }
    if (handle->callback && g_ext_release_callback) {
        g_ext_release_callback(handle->callback);
        handle->callback = NULL;
    }
    mtx_unlock(&handle->mutex);
    return 0;
}

static bool ext_read_meta_map(TabloExtCallContext* ctx,
                              const TabloExtValue* value,
                              const char* context,
                              const char** out_source,
                              int* out_source_len,
                              int64_t* out_priority,
                              int* out_phase_count) {
    const TabloExtMapEntry* entries = NULL;
    const TabloExtMapEntry* source_entry = NULL;
    const TabloExtMapEntry* priority_entry = NULL;
    const TabloExtMapEntry* phases_entry = NULL;
    const TabloExtValue* phase_items = NULL;
    char message[160];

    if (out_source) *out_source = NULL;
    if (out_source_len) *out_source_len = 0;
    if (out_priority) *out_priority = 0;
    if (out_phase_count) *out_phase_count = 0;

    if (!value || value->tag != TABLO_EXT_TYPE_MAP || value->is_nil) {
        snprintf(message, sizeof(message), "%s expected meta:map<string, any>", context);
        ext_set_runtime_error(ctx, message);
        return false;
    }

    entries = value->as.map_value.entries;
    source_entry = tablo_ext_find_map_entry(entries, value->as.map_value.count, "source");
    priority_entry = tablo_ext_find_map_entry(entries, value->as.map_value.count, "priority");
    phases_entry = tablo_ext_find_map_entry(entries, value->as.map_value.count, "phases");
    if (!source_entry || !priority_entry || !phases_entry) {
        snprintf(message, sizeof(message), "%s expected meta.source, meta.priority, and meta.phases", context);
        ext_set_runtime_error(ctx, message);
        return false;
    }
    if (source_entry->value.tag != TABLO_EXT_TYPE_STRING || source_entry->value.is_nil) {
        snprintf(message, sizeof(message), "%s expected meta.source:string", context);
        ext_set_runtime_error(ctx, message);
        return false;
    }
    if (priority_entry->value.tag != TABLO_EXT_TYPE_INT || priority_entry->value.is_nil) {
        snprintf(message, sizeof(message), "%s expected meta.priority:int", context);
        ext_set_runtime_error(ctx, message);
        return false;
    }
    if (phases_entry->value.tag != TABLO_EXT_TYPE_ARRAY || phases_entry->value.is_nil) {
        snprintf(message, sizeof(message), "%s expected meta.phases:array<string>", context);
        ext_set_runtime_error(ctx, message);
        return false;
    }

    phase_items = phases_entry->value.as.array_value.items;
    if (phases_entry->value.as.array_value.count < 0 ||
        (phases_entry->value.as.array_value.count > 0 && !phase_items)) {
        snprintf(message, sizeof(message), "%s expected initialized meta.phases:array<string>", context);
        ext_set_runtime_error(ctx, message);
        return false;
    }
    for (int i = 0; i < phases_entry->value.as.array_value.count; i++) {
        if (phase_items[i].tag != TABLO_EXT_TYPE_STRING || phase_items[i].is_nil) {
            snprintf(message, sizeof(message), "%s expected meta.phases:array<string>", context);
            ext_set_runtime_error(ctx, message);
            return false;
        }
    }

    if (out_source) {
        *out_source = source_entry->value.as.string_value.chars ? source_entry->value.as.string_value.chars : "";
    }
    if (out_source_len) {
        *out_source_len = source_entry->value.as.string_value.length >= 0
                              ? source_entry->value.as.string_value.length
                              : (int)strlen(source_entry->value.as.string_value.chars ? source_entry->value.as.string_value.chars : "");
    }
    if (out_priority) {
        *out_priority = priority_entry->value.as.int_value;
    }
    if (out_phase_count) {
        *out_phase_count = phases_entry->value.as.array_value.count;
    }
    return true;
}

static bool ext_add(TabloExtCallContext* ctx) {
    int64_t left = 0;
    int64_t right = 0;
    if (!ctx->api->get_int_arg(ctx, 0, &left)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &right)) return false;
    return ctx->api->set_int_result(ctx, left + right);
}

static bool ext_hello(TabloExtCallContext* ctx) {
    const char* name = NULL;
    int name_len = 0;
    static const char prefix[] = "hello, ";
    if (!ctx->api->get_string_arg(ctx, 0, &name, &name_len)) return false;

    size_t total_len = (sizeof(prefix) - 1u) + (size_t)name_len;
    char* out = (char*)malloc(total_len + 1u);
    if (!out) {
        ext_set_runtime_error(ctx, "Out of memory while formatting extHello result");
        return false;
    }

    memcpy(out, prefix, sizeof(prefix) - 1u);
    if (name_len > 0) memcpy(out + (sizeof(prefix) - 1u), name, (size_t)name_len);
    out[total_len] = '\0';

    bool ok = ctx->api->set_string_result(ctx, out, (int)total_len);
    free(out);
    return ok;
}

static bool ext_starts_with(TabloExtCallContext* ctx) {
    const char* text = NULL;
    const char* prefix = NULL;
    int text_len = 0;
    int prefix_len = 0;
    if (!ctx->api->get_string_arg(ctx, 0, &text, &text_len)) return false;
    if (!ctx->api->get_string_arg(ctx, 1, &prefix, &prefix_len)) return false;

    bool matches = false;
    if (prefix_len <= text_len) {
        matches = memcmp(text, prefix, (size_t)prefix_len) == 0;
    }
    return ctx->api->set_bool_result(ctx, matches);
}

static bool ext_scale(TabloExtCallContext* ctx) {
    double value = 0.0;
    double factor = 0.0;
    if (!ctx->api->get_double_arg(ctx, 0, &value)) return false;
    if (!ctx->api->get_double_arg(ctx, 1, &factor)) return false;
    return ctx->api->set_double_result(ctx, value * factor);
}

static bool ext_echo_bytes(TabloExtCallContext* ctx) {
    const uint8_t* bytes = NULL;
    int length = 0;
    if (!ctx->api->get_bytes_arg(ctx, 0, &bytes, &length)) return false;
    return ctx->api->set_bytes_result(ctx, bytes, length);
}

static bool ext_counter_new(TabloExtCallContext* ctx) {
    int64_t start = 0;
    CounterHandle* counter = NULL;
    if (!ctx->api->get_int_arg(ctx, 0, &start)) return false;

    counter = (CounterHandle*)malloc(sizeof(CounterHandle));
    if (!counter) {
        ext_set_runtime_error(ctx, "Out of memory while allocating Counter handle");
        return false;
    }
    counter->value = start;
    return ctx->api->set_handle_result(ctx, "Counter", counter);
}

static bool ext_counter_get(TabloExtCallContext* ctx) {
    CounterHandle* counter = ext_counter_from_arg(ctx, 0);
    if (!counter) return false;
    return ctx->api->set_int_result(ctx, counter->value);
}

static bool ext_counter_add(TabloExtCallContext* ctx) {
    CounterHandle* counter = ext_counter_from_arg(ctx, 0);
    int64_t delta = 0;
    if (!counter) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &delta)) return false;
    counter->value += delta;
    return ctx->api->set_int_result(ctx, counter->value);
}

static bool ext_counter_label(TabloExtCallContext* ctx) {
    CounterHandle* counter = ext_counter_from_arg(ctx, 0);
    const char* prefix = NULL;
    int prefix_len = 0;
    char value_buf[64];
    int value_len = 0;
    char* out = NULL;
    size_t total_len = 0;

    if (!counter) return false;
    if (!ctx->api->get_string_arg(ctx, 1, &prefix, &prefix_len)) return false;

    value_len = snprintf(value_buf, sizeof(value_buf), "%lld", (long long)counter->value);
    if (value_len < 0) {
        ext_set_runtime_error(ctx, "Failed to format Counter label");
        return false;
    }

    total_len = (size_t)prefix_len + (size_t)value_len;
    out = (char*)malloc(total_len + 1u);
    if (!out) {
        ext_set_runtime_error(ctx, "Out of memory while formatting Counter label");
        return false;
    }

    if (prefix_len > 0) memcpy(out, prefix, (size_t)prefix_len);
    memcpy(out + prefix_len, value_buf, (size_t)value_len);
    out[total_len] = '\0';

    {
        bool ok = ctx->api->set_string_result(ctx, out, (int)total_len);
        free(out);
        return ok;
    }
}

static bool ext_sum_ints(TabloExtCallContext* ctx) {
    const TabloExtValue* items = NULL;
    int count = 0;
    int64_t total = 0;

    if (!ctx->api->get_array_arg(ctx, 0, &items, &count)) return false;
    for (int i = 0; i < count; i++) {
        total += items[i].as.int_value;
    }
    return ctx->api->set_int_result(ctx, total);
}

static bool ext_make_int_range(TabloExtCallContext* ctx) {
    int64_t start = 0;
    int64_t count = 0;
    TabloExtValue* items = NULL;
    bool ok = false;

    if (!ctx->api->get_int_arg(ctx, 0, &start)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &count)) return false;
    if (count < 0 || count > 1024) {
        ext_set_runtime_error(ctx, "extMakeIntRange count must be between 0 and 1024");
        return false;
    }

    if (count > 0) {
        items = (TabloExtValue*)calloc((size_t)count, sizeof(TabloExtValue));
        if (!items) {
            ext_set_runtime_error(ctx, "Out of memory while building extMakeIntRange result");
            return false;
        }
        for (int64_t i = 0; i < count; i++) {
            items[i].tag = TABLO_EXT_TYPE_INT;
            items[i].as.int_value = start + i;
        }
    }

    ok = ctx->api->set_array_result(ctx, items, (int)count);
    free(items);
    return ok;
}

static bool ext_sum_counters(TabloExtCallContext* ctx) {
    const TabloExtValue* items = NULL;
    int count = 0;
    int64_t total = 0;

    if (!ctx->api->get_array_arg(ctx, 0, &items, &count)) return false;
    for (int i = 0; i < count; i++) {
        CounterHandle* counter = (CounterHandle*)items[i].as.handle_value;
        if (!counter) {
            ext_set_runtime_error(ctx, "Counter array element payload is null");
            return false;
        }
        total += counter->value;
    }
    return ctx->api->set_int_result(ctx, total);
}

static bool ext_join_strings(TabloExtCallContext* ctx) {
    const TabloExtValue* items = NULL;
    int count = 0;
    const char* sep = NULL;
    int sep_len = 0;
    size_t total_len = 0;
    char* out = NULL;
    size_t offset = 0;
    bool ok = false;

    if (!ctx->api->get_array_arg(ctx, 0, &items, &count)) return false;
    if (!ctx->api->get_string_arg(ctx, 1, &sep, &sep_len)) return false;

    for (int i = 0; i < count; i++) {
        total_len += (size_t)items[i].as.string_value.length;
        if (i + 1 < count) {
            total_len += (size_t)sep_len;
        }
    }

    out = (char*)malloc(total_len + 1u);
    if (!out) {
        ext_set_runtime_error(ctx, "Out of memory while building extJoinStrings result");
        return false;
    }

    for (int i = 0; i < count; i++) {
        if (items[i].as.string_value.length > 0) {
            memcpy(out + offset,
                   items[i].as.string_value.chars,
                   (size_t)items[i].as.string_value.length);
            offset += (size_t)items[i].as.string_value.length;
        }
        if (i + 1 < count && sep_len > 0) {
            memcpy(out + offset, sep, (size_t)sep_len);
            offset += (size_t)sep_len;
        }
    }
    out[offset] = '\0';

    ok = ctx->api->set_string_result(ctx, out, (int)offset);
    free(out);
    return ok;
}

static bool ext_swap_pair(TabloExtCallContext* ctx) {
    const TabloExtValue* items = NULL;
    int count = 0;
    TabloExtValue result_items[2];

    if (!ctx->api->get_tuple_arg(ctx, 0, &items, &count)) return false;
    if (count != 2) {
        ext_set_runtime_error(ctx, "extSwapPair expected a 2-element tuple");
        return false;
    }

    memset(result_items, 0, sizeof(result_items));
    result_items[0].tag = TABLO_EXT_TYPE_STRING;
    result_items[0].as.string_value = items[1].as.string_value;
    result_items[1].tag = TABLO_EXT_TYPE_INT;
    result_items[1].as.int_value = items[0].as.int_value;
    return ctx->api->set_tuple_result(ctx, result_items, 2);
}

static bool ext_counter_snapshot_tuple(TabloExtCallContext* ctx) {
    CounterHandle* counter = ext_counter_from_arg(ctx, 0);
    CounterHandle* clone = NULL;
    TabloExtValue result_items[2];

    if (!counter) return false;

    clone = (CounterHandle*)malloc(sizeof(CounterHandle));
    if (!clone) {
        ext_set_runtime_error(ctx, "Out of memory while cloning Counter handle");
        return false;
    }
    clone->value = counter->value;

    memset(result_items, 0, sizeof(result_items));
    result_items[0].tag = TABLO_EXT_TYPE_HANDLE;
    result_items[0].handle_type_name = "Counter";
    result_items[0].as.handle_value = clone;
    result_items[1].tag = TABLO_EXT_TYPE_INT;
    result_items[1].as.int_value = counter->value;

    if (!ctx->api->set_tuple_result(ctx, result_items, 2)) {
        free(clone);
        return false;
    }
    return true;
}

static bool ext_make_event_map(TabloExtCallContext* ctx) {
    CounterHandle* counter = ext_counter_from_arg(ctx, 0);
    int64_t delta = 0;
    const char* label = NULL;
    int label_len = 0;
    CounterHandle* snapshot = NULL;
    TabloExtValue coords_items[2];
    TabloExtValue tag_items[2];
    TabloExtValue meta_phase_items[2];
    TabloExtMapEntry meta_entries[3];
    TabloExtMapEntry entries[7];
    TabloExtArrayBuilder coords_builder;
    TabloExtMapBuilder event_builder;
    TabloExtEventMetaSpec meta_spec;
    TabloExtValue meta_value;
    TabloExtValue tags_value;
    const char* tag_names[2] = {"native", "event"};
    int tag_lengths[2] = {6, 5};
    const char* phase_names[2] = {"create", "dispatch"};
    int phase_lengths[2] = {6, 8};

    if (!counter) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &delta)) return false;
    if (!ctx->api->get_string_arg(ctx, 2, &label, &label_len)) return false;

    snapshot = (CounterHandle*)malloc(sizeof(CounterHandle));
    if (!snapshot) {
        ext_set_runtime_error(ctx, "Out of memory while cloning Counter handle for extMakeEventMap");
        return false;
    }
    snapshot->value = counter->value;

    memset(entries, 0, sizeof(entries));
    memset(coords_items, 0, sizeof(coords_items));
    memset(tag_items, 0, sizeof(tag_items));
    memset(meta_phase_items, 0, sizeof(meta_phase_items));
    memset(meta_entries, 0, sizeof(meta_entries));
    memset(&meta_spec, 0, sizeof(meta_spec));
    memset(&meta_value, 0, sizeof(meta_value));
    memset(&tags_value, 0, sizeof(tags_value));

    tablo_ext_array_builder_init(&coords_builder, coords_items, 2);
    tablo_ext_map_builder_init(&event_builder, entries, 7);

    meta_spec.source_chars = "native-ext";
    meta_spec.source_length = 10;
    meta_spec.priority = 3;
    meta_spec.phases.chars = phase_names;
    meta_spec.phases.lengths = phase_lengths;
    meta_spec.phases.count = 2;

    if (!tablo_ext_array_builder_add_int(&coords_builder, delta) ||
        !tablo_ext_array_builder_add_int(&coords_builder, counter->value) ||
        !tablo_ext_build_string_array(&tags_value, tag_items, 2, tag_names, tag_lengths, 2) ||
        !tablo_ext_build_event_meta_map(&meta_value, meta_entries, 3, meta_phase_items, 2, &meta_spec) ||
        !tablo_ext_map_builder_add_string(&event_builder, "label", 5, label, label_len) ||
        !tablo_ext_map_builder_add_int(&event_builder, "delta", 5, delta) ||
        !tablo_ext_map_builder_add_bool(&event_builder, "active", 6, true) ||
        !tablo_ext_map_builder_add_handle(&event_builder, "counter", 7, "Counter", snapshot) ||
        !tablo_ext_map_builder_add_value(&event_builder,
                                       "coords",
                                       6,
                                       tablo_ext_array_builder_build_tuple(&coords_builder)) ||
        !tablo_ext_map_builder_add_value(&event_builder, "tags", 4, tags_value) ||
        !tablo_ext_map_builder_add_value(&event_builder, "meta", 4, meta_value)) {
        ext_set_runtime_error(ctx, "Internal error while building extMakeEventMap payload");
        free(snapshot);
        return false;
    }

    if (!ctx->api->set_map_result(ctx, entries, event_builder.count)) {
        free(snapshot);
        return false;
    }
    return true;
}

static bool ext_summarize_event_map(TabloExtCallContext* ctx) {
    const TabloExtMapEntry* entries = NULL;
    int count = 0;
    const TabloExtMapEntry* label_entry = NULL;
    const TabloExtMapEntry* delta_entry = NULL;
    const TabloExtMapEntry* active_entry = NULL;
    const TabloExtMapEntry* counter_entry = NULL;
    const TabloExtMapEntry* coords_entry = NULL;
    const TabloExtMapEntry* tags_entry = NULL;
    const TabloExtMapEntry* meta_entry = NULL;
    CounterHandle* counter = NULL;
    const TabloExtValue* coords_items = NULL;
    const TabloExtValue* tag_items = NULL;
    int coords_count = 0;
    int tag_count = 0;
    const char* meta_source = NULL;
    int meta_source_len = 0;
    int64_t meta_priority = 0;
    int meta_phase_count = 0;
    int64_t total = 0;
    const char* label = NULL;
    int label_len = 0;
    const char* bool_text = NULL;
    char total_buf[64];
    char coord_buf[64];
    char tag_count_buf[64];
    int total_len_written = 0;
    int coord_len_written = 0;
    int tag_count_len_written = 0;
    size_t total_len = 0;
    char* out = NULL;
    bool ok = false;

    if (!ctx->api->get_map_arg(ctx, 0, &entries, &count)) return false;

    label_entry = tablo_ext_find_map_entry(entries, count, "label");
    delta_entry = tablo_ext_find_map_entry(entries, count, "delta");
    active_entry = tablo_ext_find_map_entry(entries, count, "active");
    counter_entry = tablo_ext_find_map_entry(entries, count, "counter");
    coords_entry = tablo_ext_find_map_entry(entries, count, "coords");
    tags_entry = tablo_ext_find_map_entry(entries, count, "tags");
    meta_entry = tablo_ext_find_map_entry(entries, count, "meta");
    if (!label_entry || !delta_entry || !active_entry || !counter_entry || !coords_entry || !tags_entry || !meta_entry) {
        ext_set_runtime_error(ctx, "extSummarizeEventMap expected label, delta, active, counter, coords, tags, and meta entries");
        return false;
    }
    if (label_entry->value.tag != TABLO_EXT_TYPE_STRING || label_entry->value.is_nil) {
        ext_set_runtime_error(ctx, "extSummarizeEventMap expected label:string");
        return false;
    }
    if (delta_entry->value.tag != TABLO_EXT_TYPE_INT || delta_entry->value.is_nil) {
        ext_set_runtime_error(ctx, "extSummarizeEventMap expected delta:int");
        return false;
    }
    if (active_entry->value.tag != TABLO_EXT_TYPE_BOOL || active_entry->value.is_nil) {
        ext_set_runtime_error(ctx, "extSummarizeEventMap expected active:bool");
        return false;
    }
    if (counter_entry->value.tag != TABLO_EXT_TYPE_HANDLE || counter_entry->value.is_nil ||
        !counter_entry->value.handle_type_name ||
        strcmp(counter_entry->value.handle_type_name, "Counter") != 0) {
        ext_set_runtime_error(ctx, "extSummarizeEventMap expected counter:Counter");
        return false;
    }
    counter = (CounterHandle*)counter_entry->value.as.handle_value;
    if (!counter) {
        ext_set_runtime_error(ctx, "extSummarizeEventMap received a null Counter payload");
        return false;
    }
    if (coords_entry->value.tag != TABLO_EXT_TYPE_TUPLE || coords_entry->value.is_nil) {
        ext_set_runtime_error(ctx, "extSummarizeEventMap expected coords:(int, int)");
        return false;
    }
    coords_items = coords_entry->value.as.tuple_value.items;
    coords_count = coords_entry->value.as.tuple_value.count;
    if (coords_count != 2 || !coords_items ||
        coords_items[0].tag != TABLO_EXT_TYPE_INT || coords_items[0].is_nil ||
        coords_items[1].tag != TABLO_EXT_TYPE_INT || coords_items[1].is_nil) {
        ext_set_runtime_error(ctx, "extSummarizeEventMap expected coords:(int, int)");
        return false;
    }
    if (tags_entry->value.tag != TABLO_EXT_TYPE_ARRAY || tags_entry->value.is_nil) {
        ext_set_runtime_error(ctx, "extSummarizeEventMap expected tags:array<string>");
        return false;
    }
    tag_items = tags_entry->value.as.array_value.items;
    tag_count = tags_entry->value.as.array_value.count;
    if (tag_count < 0 || (tag_count > 0 && !tag_items)) {
        ext_set_runtime_error(ctx, "extSummarizeEventMap expected initialized tags:array<string>");
        return false;
    }
    for (int i = 0; i < tag_count; i++) {
        if (tag_items[i].tag != TABLO_EXT_TYPE_STRING || tag_items[i].is_nil) {
            ext_set_runtime_error(ctx, "extSummarizeEventMap expected tags:array<string>");
            return false;
        }
    }
    if (!ext_read_meta_map(ctx,
                           &meta_entry->value,
                           "extSummarizeEventMap",
                           &meta_source,
                           &meta_source_len,
                           &meta_priority,
                           &meta_phase_count)) {
        return false;
    }

    label = label_entry->value.as.string_value.chars ? label_entry->value.as.string_value.chars : "";
    label_len = label_entry->value.as.string_value.length;
    if (label_len < 0) {
        label_len = (int)strlen(label);
    }
    total = counter->value + delta_entry->value.as.int_value;
    bool_text = active_entry->value.as.bool_value ? "true" : "false";
    total_len_written = snprintf(total_buf, sizeof(total_buf), "%lld", (long long)total);
    coord_len_written = snprintf(coord_buf, sizeof(coord_buf), "%lld", (long long)coords_items[0].as.int_value);
    tag_count_len_written = snprintf(tag_count_buf, sizeof(tag_count_buf), "%d", tag_count);
    if (total_len_written < 0 || coord_len_written < 0 || tag_count_len_written < 0) {
        ext_set_runtime_error(ctx, "Failed to format extSummarizeEventMap result");
        return false;
    }

    total_len = (size_t)label_len + 1u +
                (size_t)total_len_written + 1u +
                (size_t)coord_len_written + 1u +
                (size_t)tag_count_len_written + 1u +
                strlen(bool_text) + 1u +
                (size_t)meta_source_len + 1u +
                20u + 1u +
                20u;
    out = (char*)malloc(total_len + 1u);
    if (!out) {
        ext_set_runtime_error(ctx, "Out of memory while building extSummarizeEventMap result");
        return false;
    }
    memcpy(out, label, (size_t)label_len);
    out[label_len] = ':';
    memcpy(out + label_len + 1, total_buf, (size_t)total_len_written);
    out[label_len + 1 + total_len_written] = ':';
    memcpy(out + label_len + 1 + total_len_written + 1, coord_buf, (size_t)coord_len_written);
    out[label_len + 1 + total_len_written + 1 + coord_len_written] = ':';
    memcpy(out + label_len + 1 + total_len_written + 1 + coord_len_written + 1,
           tag_count_buf,
           (size_t)tag_count_len_written);
    out[label_len + 1 + total_len_written + 1 + coord_len_written + 1 + tag_count_len_written] = ':';
    memcpy(out + label_len + 1 + total_len_written + 1 + coord_len_written + 1 + tag_count_len_written + 1,
           bool_text,
           strlen(bool_text));
    {
        size_t offset = label_len + 1u + (size_t)total_len_written + 1u + (size_t)coord_len_written + 1u +
                        (size_t)tag_count_len_written + 1u + strlen(bool_text);
        int meta_priority_written = 0;
        int meta_phase_written = 0;
        out[offset] = ':';
        offset += 1u;
        memcpy(out + offset, meta_source, (size_t)meta_source_len);
        offset += (size_t)meta_source_len;
        out[offset] = ':';
        offset += 1u;
        meta_priority_written = snprintf(out + offset, 21u, "%lld", (long long)meta_priority);
        offset += (size_t)meta_priority_written;
        out[offset] = ':';
        offset += 1u;
        meta_phase_written = snprintf(out + offset, 21u, "%d", meta_phase_count);
        offset += (size_t)meta_phase_written;
        out[offset] = '\0';
        total_len = offset;
    }

    ok = ctx->api->set_string_result(ctx, out, (int)total_len);
    free(out);
    return ok;
}

static bool ext_make_event_batch(TabloExtCallContext* ctx) {
    int64_t base = 0;
    TabloExtValue batch_items[2];
    TabloExtMapEntry event_entries[2][3];
    TabloExtMapEntry meta_entries[2][3];
    TabloExtValue meta_phase_items[2][2];

    if (!ctx->api->get_int_arg(ctx, 0, &base)) return false;
    ext_fill_event_batch_payload(base, "batch", batch_items, event_entries, meta_entries, meta_phase_items);

    return ctx->api->set_array_result(ctx, batch_items, 2);
}

static bool ext_summarize_event_batch(TabloExtCallContext* ctx) {
    const TabloExtValue* items = NULL;
    int count = 0;
    int64_t total = 0;
    const char* first_name = NULL;
    int first_name_len = 0;
    const char* meta_source = NULL;
    int meta_source_len = 0;
    int64_t meta_priority = 0;
    int meta_phase_count = 0;
    char buffer[160];
    int written = 0;

    if (!ctx->api->get_array_arg(ctx, 0, &items, &count)) return false;
    if (count <= 0 || !items) {
        ext_set_runtime_error(ctx, "extSummarizeEventBatch expected a non-empty array<map<string, any>>");
        return false;
    }

    for (int i = 0; i < count; i++) {
        const TabloExtValue* item = &items[i];
        const TabloExtMapEntry* name_entry = NULL;
        const TabloExtMapEntry* delta_entry = NULL;
        const TabloExtMapEntry* meta_entry = NULL;
        if (item->tag != TABLO_EXT_TYPE_MAP || item->is_nil) {
            ext_set_runtime_error(ctx, "extSummarizeEventBatch expected array<map<string, any>>");
            return false;
        }
        name_entry = tablo_ext_find_map_entry(item->as.map_value.entries, item->as.map_value.count, "name");
        delta_entry = tablo_ext_find_map_entry(item->as.map_value.entries, item->as.map_value.count, "delta");
        meta_entry = tablo_ext_find_map_entry(item->as.map_value.entries, item->as.map_value.count, "meta");
        if (!name_entry || !delta_entry || !meta_entry) {
            ext_set_runtime_error(ctx, "extSummarizeEventBatch expected event name, delta, and meta fields");
            return false;
        }
        if (name_entry->value.tag != TABLO_EXT_TYPE_STRING || name_entry->value.is_nil) {
            ext_set_runtime_error(ctx, "extSummarizeEventBatch expected event name:string");
            return false;
        }
        if (delta_entry->value.tag != TABLO_EXT_TYPE_INT || delta_entry->value.is_nil) {
            ext_set_runtime_error(ctx, "extSummarizeEventBatch expected event delta:int");
            return false;
        }
        if (i == 0) {
            first_name = name_entry->value.as.string_value.chars ? name_entry->value.as.string_value.chars : "";
            first_name_len = name_entry->value.as.string_value.length >= 0
                                 ? name_entry->value.as.string_value.length
                                 : (int)strlen(first_name);
            if (!ext_read_meta_map(ctx,
                                   &meta_entry->value,
                                   "extSummarizeEventBatch",
                                   &meta_source,
                                   &meta_source_len,
                                   &meta_priority,
                                   &meta_phase_count)) {
                return false;
            }
        }
        total += delta_entry->value.as.int_value;
    }

    written = snprintf(buffer,
                       sizeof(buffer),
                       "%d:%lld:%.*s:%.*s:%d",
                       count,
                       (long long)total,
                       first_name_len,
                       first_name ? first_name : "",
                       meta_source_len,
                       meta_source ? meta_source : "",
                       meta_phase_count);
    if (written < 0 || written >= (int)sizeof(buffer)) {
        ext_set_runtime_error(ctx, "Failed to format extSummarizeEventBatch result");
        return false;
    }
    return ctx->api->set_string_result(ctx, buffer, written);
}

static bool ext_make_input_event(TabloExtCallContext* ctx) {
    const char* device = NULL;
    int device_len = 0;
    int64_t code = 0;
    bool pressed = false;
    const char* phase_names[2] = {"queue", "dispatch"};
    int phase_lengths[2] = {5, 8};
    TabloExtValue meta_phase_items[2];
    TabloExtMapEntry meta_entries[3];
    TabloExtMapEntry entries[5];
    TabloExtEventMetaSpec meta_spec;
    TabloExtInputEventSpec event_spec;
    TabloExtValue meta_value;

    if (!ctx->api->get_string_arg(ctx, 0, &device, &device_len)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &code)) return false;
    if (!ctx->api->get_bool_arg(ctx, 2, &pressed)) return false;

    memset(meta_phase_items, 0, sizeof(meta_phase_items));
    memset(meta_entries, 0, sizeof(meta_entries));
    memset(entries, 0, sizeof(entries));
    memset(&meta_spec, 0, sizeof(meta_spec));
    memset(&event_spec, 0, sizeof(event_spec));
    memset(&meta_value, 0, sizeof(meta_value));

    meta_spec.source_chars = "input";
    meta_spec.source_length = 5;
    meta_spec.priority = 1;
    meta_spec.phases.chars = phase_names;
    meta_spec.phases.lengths = phase_lengths;
    meta_spec.phases.count = 2;

    event_spec.device_chars = device;
    event_spec.device_length = device_len;
    event_spec.code = code;
    event_spec.pressed = pressed;

    if (!tablo_ext_build_event_meta_map(&meta_value, meta_entries, 3, meta_phase_items, 2, &meta_spec)) {
        ext_set_runtime_error(ctx, "Internal error while building extMakeInputEvent meta payload");
        return false;
    }
    event_spec.meta_value = meta_value;

    if (!tablo_ext_build_input_event_map(&meta_value, entries, 5, &event_spec)) {
        ext_set_runtime_error(ctx, "Internal error while building extMakeInputEvent payload");
        return false;
    }
    return ctx->api->set_map_result(ctx, entries, 5);
}

static bool ext_summarize_input_event(TabloExtCallContext* ctx) {
    const TabloExtMapEntry* entries = NULL;
    int count = 0;
    const TabloExtMapEntry* kind_entry = NULL;
    const TabloExtMapEntry* device_entry = NULL;
    const TabloExtMapEntry* code_entry = NULL;
    const TabloExtMapEntry* pressed_entry = NULL;
    const TabloExtMapEntry* meta_entry = NULL;
    const char* meta_source = NULL;
    int meta_source_len = 0;
    int64_t meta_priority = 0;
    int meta_phase_count = 0;
    char buffer[160];
    int written = 0;

    if (!ctx->api->get_map_arg(ctx, 0, &entries, &count)) return false;
    kind_entry = tablo_ext_find_map_entry(entries, count, "kind");
    device_entry = tablo_ext_find_map_entry(entries, count, "device");
    code_entry = tablo_ext_find_map_entry(entries, count, "code");
    pressed_entry = tablo_ext_find_map_entry(entries, count, "pressed");
    meta_entry = tablo_ext_find_map_entry(entries, count, "meta");
    if (!kind_entry || !device_entry || !code_entry || !pressed_entry || !meta_entry) {
        ext_set_runtime_error(ctx, "extSummarizeInputEvent expected kind, device, code, pressed, and meta fields");
        return false;
    }
    if (kind_entry->value.tag != TABLO_EXT_TYPE_STRING || kind_entry->value.is_nil ||
        device_entry->value.tag != TABLO_EXT_TYPE_STRING || device_entry->value.is_nil ||
        code_entry->value.tag != TABLO_EXT_TYPE_INT || code_entry->value.is_nil ||
        pressed_entry->value.tag != TABLO_EXT_TYPE_BOOL || pressed_entry->value.is_nil) {
        ext_set_runtime_error(ctx, "extSummarizeInputEvent received malformed input event fields");
        return false;
    }
    if (!ext_read_meta_map(ctx,
                           &meta_entry->value,
                           "extSummarizeInputEvent",
                           &meta_source,
                           &meta_source_len,
                           &meta_priority,
                           &meta_phase_count)) {
        return false;
    }
    written = snprintf(buffer,
                       sizeof(buffer),
                       "%.*s:%.*s:%lld:%s:%.*s:%lld:%d",
                       kind_entry->value.as.string_value.length,
                       kind_entry->value.as.string_value.chars,
                       device_entry->value.as.string_value.length,
                       device_entry->value.as.string_value.chars,
                       (long long)code_entry->value.as.int_value,
                       pressed_entry->value.as.bool_value ? "true" : "false",
                       meta_source_len,
                       meta_source ? meta_source : "",
                       (long long)meta_priority,
                       meta_phase_count);
    if (written < 0 || written >= (int)sizeof(buffer)) {
        ext_set_runtime_error(ctx, "Failed to format extSummarizeInputEvent result");
        return false;
    }
    return ctx->api->set_string_result(ctx, buffer, written);
}

static bool ext_make_window_event(TabloExtCallContext* ctx) {
    const char* event_name = NULL;
    int event_name_len = 0;
    int64_t width = 0;
    int64_t height = 0;
    bool focused = false;
    const char* phase_names[2] = {"layout", "present"};
    int phase_lengths[2] = {6, 7};
    TabloExtValue meta_phase_items[2];
    TabloExtMapEntry meta_entries[3];
    TabloExtMapEntry entries[6];
    TabloExtEventMetaSpec meta_spec;
    TabloExtWindowEventSpec event_spec;
    TabloExtValue meta_value;

    if (!ctx->api->get_string_arg(ctx, 0, &event_name, &event_name_len)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &width)) return false;
    if (!ctx->api->get_int_arg(ctx, 2, &height)) return false;
    if (!ctx->api->get_bool_arg(ctx, 3, &focused)) return false;

    memset(meta_phase_items, 0, sizeof(meta_phase_items));
    memset(meta_entries, 0, sizeof(meta_entries));
    memset(entries, 0, sizeof(entries));
    memset(&meta_spec, 0, sizeof(meta_spec));
    memset(&event_spec, 0, sizeof(event_spec));
    memset(&meta_value, 0, sizeof(meta_value));

    meta_spec.source_chars = "window";
    meta_spec.source_length = 6;
    meta_spec.priority = 4;
    meta_spec.phases.chars = phase_names;
    meta_spec.phases.lengths = phase_lengths;
    meta_spec.phases.count = 2;

    event_spec.event_chars = event_name;
    event_spec.event_length = event_name_len;
    event_spec.width = width;
    event_spec.height = height;
    event_spec.focused = focused;

    if (!tablo_ext_build_event_meta_map(&meta_value, meta_entries, 3, meta_phase_items, 2, &meta_spec)) {
        ext_set_runtime_error(ctx, "Internal error while building extMakeWindowEvent meta payload");
        return false;
    }
    event_spec.meta_value = meta_value;

    if (!tablo_ext_build_window_event_map(&meta_value, entries, 6, &event_spec)) {
        ext_set_runtime_error(ctx, "Internal error while building extMakeWindowEvent payload");
        return false;
    }
    return ctx->api->set_map_result(ctx, entries, 6);
}

static bool ext_summarize_window_event(TabloExtCallContext* ctx) {
    const TabloExtMapEntry* entries = NULL;
    int count = 0;
    const TabloExtMapEntry* kind_entry = NULL;
    const TabloExtMapEntry* event_entry = NULL;
    const TabloExtMapEntry* width_entry = NULL;
    const TabloExtMapEntry* height_entry = NULL;
    const TabloExtMapEntry* focused_entry = NULL;
    const TabloExtMapEntry* meta_entry = NULL;
    const char* meta_source = NULL;
    int meta_source_len = 0;
    int64_t meta_priority = 0;
    int meta_phase_count = 0;
    char buffer[192];
    int written = 0;

    if (!ctx->api->get_map_arg(ctx, 0, &entries, &count)) return false;
    kind_entry = tablo_ext_find_map_entry(entries, count, "kind");
    event_entry = tablo_ext_find_map_entry(entries, count, "event");
    width_entry = tablo_ext_find_map_entry(entries, count, "width");
    height_entry = tablo_ext_find_map_entry(entries, count, "height");
    focused_entry = tablo_ext_find_map_entry(entries, count, "focused");
    meta_entry = tablo_ext_find_map_entry(entries, count, "meta");
    if (!kind_entry || !event_entry || !width_entry || !height_entry || !focused_entry || !meta_entry) {
        ext_set_runtime_error(ctx, "extSummarizeWindowEvent expected kind, event, width, height, focused, and meta fields");
        return false;
    }
    if (kind_entry->value.tag != TABLO_EXT_TYPE_STRING || kind_entry->value.is_nil ||
        event_entry->value.tag != TABLO_EXT_TYPE_STRING || event_entry->value.is_nil ||
        width_entry->value.tag != TABLO_EXT_TYPE_INT || width_entry->value.is_nil ||
        height_entry->value.tag != TABLO_EXT_TYPE_INT || height_entry->value.is_nil ||
        focused_entry->value.tag != TABLO_EXT_TYPE_BOOL || focused_entry->value.is_nil) {
        ext_set_runtime_error(ctx, "extSummarizeWindowEvent received malformed window event fields");
        return false;
    }
    if (!ext_read_meta_map(ctx,
                           &meta_entry->value,
                           "extSummarizeWindowEvent",
                           &meta_source,
                           &meta_source_len,
                           &meta_priority,
                           &meta_phase_count)) {
        return false;
    }
    written = snprintf(buffer,
                       sizeof(buffer),
                       "%.*s:%.*s:%lld:%lld:%s:%.*s:%lld:%d",
                       kind_entry->value.as.string_value.length,
                       kind_entry->value.as.string_value.chars,
                       event_entry->value.as.string_value.length,
                       event_entry->value.as.string_value.chars,
                       (long long)width_entry->value.as.int_value,
                       (long long)height_entry->value.as.int_value,
                       focused_entry->value.as.bool_value ? "true" : "false",
                       meta_source_len,
                       meta_source ? meta_source : "",
                       (long long)meta_priority,
                       meta_phase_count);
    if (written < 0 || written >= (int)sizeof(buffer)) {
        ext_set_runtime_error(ctx, "Failed to format extSummarizeWindowEvent result");
        return false;
    }
    return ctx->api->set_string_result(ctx, buffer, written);
}

static bool ext_make_frame_event(TabloExtCallContext* ctx) {
    const char* phase = NULL;
    int phase_len = 0;
    int64_t frame_number = 0;
    double dt = 0.0;
    const char* phase_names[2] = {"update", "render"};
    int phase_lengths[2] = {6, 6};
    TabloExtValue meta_phase_items[2];
    TabloExtMapEntry meta_entries[3];
    TabloExtMapEntry entries[5];
    TabloExtEventMetaSpec meta_spec;
    TabloExtFrameEventSpec event_spec;
    TabloExtValue meta_value;

    if (!ctx->api->get_string_arg(ctx, 0, &phase, &phase_len)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &frame_number)) return false;
    if (!ctx->api->get_double_arg(ctx, 2, &dt)) return false;

    memset(meta_phase_items, 0, sizeof(meta_phase_items));
    memset(meta_entries, 0, sizeof(meta_entries));
    memset(entries, 0, sizeof(entries));
    memset(&meta_spec, 0, sizeof(meta_spec));
    memset(&event_spec, 0, sizeof(event_spec));
    memset(&meta_value, 0, sizeof(meta_value));

    meta_spec.source_chars = "frame";
    meta_spec.source_length = 5;
    meta_spec.priority = 5;
    meta_spec.phases.chars = phase_names;
    meta_spec.phases.lengths = phase_lengths;
    meta_spec.phases.count = 2;

    event_spec.phase_chars = phase;
    event_spec.phase_length = phase_len;
    event_spec.frame_number = frame_number;
    event_spec.delta_seconds = dt;

    if (!tablo_ext_build_event_meta_map(&meta_value, meta_entries, 3, meta_phase_items, 2, &meta_spec)) {
        ext_set_runtime_error(ctx, "Internal error while building extMakeFrameEvent meta payload");
        return false;
    }
    event_spec.meta_value = meta_value;

    if (!tablo_ext_build_frame_event_map(&meta_value, entries, 5, &event_spec)) {
        ext_set_runtime_error(ctx, "Internal error while building extMakeFrameEvent payload");
        return false;
    }
    return ctx->api->set_map_result(ctx, entries, 5);
}

static bool ext_summarize_frame_event(TabloExtCallContext* ctx) {
    const TabloExtMapEntry* entries = NULL;
    int count = 0;
    const TabloExtMapEntry* kind_entry = NULL;
    const TabloExtMapEntry* phase_entry = NULL;
    const TabloExtMapEntry* frame_entry = NULL;
    const TabloExtMapEntry* dt_entry = NULL;
    const TabloExtMapEntry* meta_entry = NULL;
    const char* meta_source = NULL;
    int meta_source_len = 0;
    int64_t meta_priority = 0;
    int meta_phase_count = 0;
    char buffer[192];
    int written = 0;

    if (!ctx->api->get_map_arg(ctx, 0, &entries, &count)) return false;
    kind_entry = tablo_ext_find_map_entry(entries, count, "kind");
    phase_entry = tablo_ext_find_map_entry(entries, count, "phase");
    frame_entry = tablo_ext_find_map_entry(entries, count, "frame");
    dt_entry = tablo_ext_find_map_entry(entries, count, "dt");
    meta_entry = tablo_ext_find_map_entry(entries, count, "meta");
    if (!kind_entry || !phase_entry || !frame_entry || !dt_entry || !meta_entry) {
        ext_set_runtime_error(ctx, "extSummarizeFrameEvent expected kind, phase, frame, dt, and meta fields");
        return false;
    }
    if (kind_entry->value.tag != TABLO_EXT_TYPE_STRING || kind_entry->value.is_nil ||
        phase_entry->value.tag != TABLO_EXT_TYPE_STRING || phase_entry->value.is_nil ||
        frame_entry->value.tag != TABLO_EXT_TYPE_INT || frame_entry->value.is_nil ||
        dt_entry->value.tag != TABLO_EXT_TYPE_DOUBLE || dt_entry->value.is_nil) {
        ext_set_runtime_error(ctx, "extSummarizeFrameEvent received malformed frame event fields");
        return false;
    }
    if (!ext_read_meta_map(ctx,
                           &meta_entry->value,
                           "extSummarizeFrameEvent",
                           &meta_source,
                           &meta_source_len,
                           &meta_priority,
                           &meta_phase_count)) {
        return false;
    }
    written = snprintf(buffer,
                       sizeof(buffer),
                       "%.*s:%.*s:%lld:%.3f:%.*s:%lld:%d",
                       kind_entry->value.as.string_value.length,
                       kind_entry->value.as.string_value.chars,
                       phase_entry->value.as.string_value.length,
                       phase_entry->value.as.string_value.chars,
                       (long long)frame_entry->value.as.int_value,
                       dt_entry->value.as.double_value,
                       meta_source_len,
                       meta_source ? meta_source : "",
                       (long long)meta_priority,
                       meta_phase_count);
    if (written < 0 || written >= (int)sizeof(buffer)) {
        ext_set_runtime_error(ctx, "Failed to format extSummarizeFrameEvent result");
        return false;
    }
    return ctx->api->set_string_result(ctx, buffer, written);
}

static bool ext_invoke_int_callback(TabloExtCallContext* ctx) {
    const TabloExtCallback* callback = NULL;
    int64_t value = 0;
    TabloExtValue arg;
    TabloExtValue result;

    if (!ctx->api->get_callback_arg(ctx, 0, &callback)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &value)) return false;
    if (!callback) {
        ext_set_runtime_error(ctx, "extInvokeIntCallback requires a non-nil callback");
        return false;
    }

    memset(&arg, 0, sizeof(arg));
    memset(&result, 0, sizeof(result));
    arg.tag = TABLO_EXT_TYPE_INT;
    arg.as.int_value = value;

    if (!ctx->api->invoke_callback(ctx, callback, &arg, 1, &result)) return false;
    return ctx->api->set_int_result(ctx, result.as.int_value);
}

static bool ext_store_int_callback(TabloExtCallContext* ctx) {
    const TabloExtCallback* callback = NULL;
    StoredIntCallbackHandle* handle = NULL;

    if (!ctx->api->get_callback_arg(ctx, 0, &callback)) return false;
    if (!callback) {
        ext_set_runtime_error(ctx, "extStoreIntCallback requires a non-nil callback");
        return false;
    }
    if (!g_ext_retain_callback || !g_ext_release_callback) {
        ext_set_runtime_error(ctx, "Callback retain/release hooks are not initialized");
        return false;
    }

    handle = (StoredIntCallbackHandle*)malloc(sizeof(StoredIntCallbackHandle));
    if (!handle) {
        ext_set_runtime_error(ctx, "Out of memory while allocating StoredIntCallback handle");
        return false;
    }

    g_ext_retain_callback(callback);
    handle->callback = callback;
    if (!ctx->api->set_handle_result(ctx, "StoredIntCallback", handle)) {
        g_ext_release_callback(callback);
        free(handle);
        return false;
    }
    return true;
}

static bool ext_invoke_stored_int_callback(TabloExtCallContext* ctx) {
    void* payload = NULL;
    StoredIntCallbackHandle* handle = NULL;
    int64_t value = 0;
    TabloExtValue arg;
    TabloExtValue result;

    if (!ctx->api->get_handle_arg(ctx, 0, "StoredIntCallback", &payload)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &value)) return false;
    handle = (StoredIntCallbackHandle*)payload;
    if (!handle || !handle->callback) {
        ext_set_runtime_error(ctx, "StoredIntCallback handle payload is invalid");
        return false;
    }

    memset(&arg, 0, sizeof(arg));
    memset(&result, 0, sizeof(result));
    arg.tag = TABLO_EXT_TYPE_INT;
    arg.as.int_value = value;

    if (!ctx->api->invoke_callback(ctx, handle->callback, &arg, 1, &result)) return false;
    return ctx->api->set_int_result(ctx, result.as.int_value);
}

static bool ext_invoke_counter_label_callback(TabloExtCallContext* ctx) {
    const TabloExtCallback* callback = NULL;
    CounterHandle* counter = NULL;
    TabloExtValue arg;
    TabloExtValue result;

    if (!ctx->api->get_callback_arg(ctx, 0, &callback)) return false;
    counter = ext_counter_from_arg(ctx, 1);
    if (!counter) return false;
    if (!callback) {
        ext_set_runtime_error(ctx, "extInvokeCounterLabelCallback requires a non-nil callback");
        return false;
    }

    memset(&arg, 0, sizeof(arg));
    memset(&result, 0, sizeof(result));
    arg.tag = TABLO_EXT_TYPE_HANDLE;
    arg.handle_type_name = "Counter";
    arg.as.handle_value = counter;

    if (!ctx->api->invoke_callback(ctx, callback, &arg, 1, &result)) return false;
    return ctx->api->set_string_result(ctx,
                                       result.as.string_value.chars,
                                       result.as.string_value.length);
}

static bool ext_invoke_array_arg_callback(TabloExtCallContext* ctx) {
    const TabloExtCallback* callback = NULL;
    int64_t start = 0;
    TabloExtValue items[3];
    TabloExtValue arg;
    TabloExtValue result;

    if (!ctx->api->get_callback_arg(ctx, 0, &callback)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &start)) return false;
    if (!callback) {
        ext_set_runtime_error(ctx, "extInvokeArrayArgCallback requires a non-nil callback");
        return false;
    }

    memset(items, 0, sizeof(items));
    memset(&arg, 0, sizeof(arg));
    memset(&result, 0, sizeof(result));
    for (int i = 0; i < 3; i++) {
        items[i].tag = TABLO_EXT_TYPE_INT;
        items[i].as.int_value = start + i;
    }
    arg.tag = TABLO_EXT_TYPE_ARRAY;
    arg.as.array_value.items = items;
    arg.as.array_value.count = 3;

    if (!ctx->api->invoke_callback(ctx, callback, &arg, 1, &result)) return false;
    if (result.tag != TABLO_EXT_TYPE_INT || result.is_nil) {
        ext_set_runtime_error(ctx, "extInvokeArrayArgCallback expected an int result");
        return false;
    }
    return ctx->api->set_int_result(ctx, result.as.int_value);
}

static bool ext_invoke_tuple_result_callback(TabloExtCallContext* ctx) {
    const TabloExtCallback* callback = NULL;
    int64_t value = 0;
    TabloExtValue arg;
    TabloExtValue result;
    const TabloExtValue* items = NULL;
    char buffer[128];
    int written = 0;

    if (!ctx->api->get_callback_arg(ctx, 0, &callback)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &value)) return false;
    if (!callback) {
        ext_set_runtime_error(ctx, "extInvokeTupleResultCallback requires a non-nil callback");
        return false;
    }

    memset(&arg, 0, sizeof(arg));
    memset(&result, 0, sizeof(result));
    arg.tag = TABLO_EXT_TYPE_INT;
    arg.as.int_value = value;

    if (!ctx->api->invoke_callback(ctx, callback, &arg, 1, &result)) return false;
    if (result.tag != TABLO_EXT_TYPE_TUPLE || result.is_nil) {
        ext_set_runtime_error(ctx, "extInvokeTupleResultCallback expected a non-nil tuple result");
        return false;
    }
    if (result.as.tuple_value.count != 2 || !result.as.tuple_value.items) {
        ext_set_runtime_error(ctx, "extInvokeTupleResultCallback expected a two-element tuple result");
        return false;
    }

    items = result.as.tuple_value.items;
    if (items[0].tag != TABLO_EXT_TYPE_INT || items[0].is_nil ||
        items[1].tag != TABLO_EXT_TYPE_STRING || items[1].is_nil) {
        ext_set_runtime_error(ctx, "extInvokeTupleResultCallback expected result shape (int, string)");
        return false;
    }

    written = snprintf(buffer,
                       sizeof(buffer),
                       "%lld:%.*s",
                       (long long)items[0].as.int_value,
                       items[1].as.string_value.length,
                       items[1].as.string_value.chars ? items[1].as.string_value.chars : "");
    if (written < 0 || written >= (int)sizeof(buffer)) {
        ext_set_runtime_error(ctx, "Failed to format extInvokeTupleResultCallback result");
        return false;
    }
    return ctx->api->set_string_result(ctx, buffer, written);
}

static bool ext_invoke_array_result_callback(TabloExtCallContext* ctx) {
    const TabloExtCallback* callback = NULL;
    int64_t value = 0;
    TabloExtValue arg;
    TabloExtValue result;
    const TabloExtValue* items = NULL;
    int64_t total = 0;

    if (!ctx->api->get_callback_arg(ctx, 0, &callback)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &value)) return false;
    if (!callback) {
        ext_set_runtime_error(ctx, "extInvokeArrayResultCallback requires a non-nil callback");
        return false;
    }

    memset(&arg, 0, sizeof(arg));
    memset(&result, 0, sizeof(result));
    arg.tag = TABLO_EXT_TYPE_INT;
    arg.as.int_value = value;

    if (!ctx->api->invoke_callback(ctx, callback, &arg, 1, &result)) return false;
    if (result.tag != TABLO_EXT_TYPE_ARRAY || result.is_nil) {
        ext_set_runtime_error(ctx, "extInvokeArrayResultCallback expected a non-nil array result");
        return false;
    }
    if (result.as.array_value.count < 0 ||
        (result.as.array_value.count > 0 && !result.as.array_value.items)) {
        ext_set_runtime_error(ctx, "extInvokeArrayResultCallback expected initialized array items");
        return false;
    }

    items = result.as.array_value.items;
    for (int i = 0; i < result.as.array_value.count; i++) {
        if (items[i].tag != TABLO_EXT_TYPE_INT || items[i].is_nil) {
            ext_set_runtime_error(ctx, "extInvokeArrayResultCallback expected result shape array<int>");
            return false;
        }
        total += items[i].as.int_value;
    }

    return ctx->api->set_int_result(ctx, total);
}

static bool ext_invoke_map_result_callback(TabloExtCallContext* ctx) {
    const TabloExtCallback* callback = NULL;
    int64_t value = 0;
    TabloExtValue arg;
    TabloExtValue result;
    const TabloExtMapEntry* entries = NULL;
    const TabloExtMapEntry* label_entry = NULL;
    const TabloExtMapEntry* total_entry = NULL;
    const TabloExtMapEntry* coords_entry = NULL;
    const TabloExtMapEntry* tags_entry = NULL;
    const TabloExtMapEntry* meta_entry = NULL;
    const TabloExtValue* coords_items = NULL;
    const char* meta_source = NULL;
    int meta_source_len = 0;
    int64_t meta_priority = 0;
    int meta_phase_count = 0;
    char buffer[160];
    int written = 0;

    if (!ctx->api->get_callback_arg(ctx, 0, &callback)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &value)) return false;
    if (!callback) {
        ext_set_runtime_error(ctx, "extInvokeMapResultCallback requires a non-nil callback");
        return false;
    }

    memset(&arg, 0, sizeof(arg));
    memset(&result, 0, sizeof(result));
    arg.tag = TABLO_EXT_TYPE_INT;
    arg.as.int_value = value;

    if (!ctx->api->invoke_callback(ctx, callback, &arg, 1, &result)) return false;
    if (result.tag != TABLO_EXT_TYPE_MAP || result.is_nil) {
        ext_set_runtime_error(ctx, "extInvokeMapResultCallback expected a non-nil map result");
        return false;
    }

    entries = result.as.map_value.entries;
    label_entry = tablo_ext_find_map_entry(entries, result.as.map_value.count, "label");
    total_entry = tablo_ext_find_map_entry(entries, result.as.map_value.count, "total");
    coords_entry = tablo_ext_find_map_entry(entries, result.as.map_value.count, "coords");
    tags_entry = tablo_ext_find_map_entry(entries, result.as.map_value.count, "tags");
    meta_entry = tablo_ext_find_map_entry(entries, result.as.map_value.count, "meta");
    if (!label_entry || !total_entry || !coords_entry || !tags_entry || !meta_entry) {
        ext_set_runtime_error(ctx, "extInvokeMapResultCallback expected label, total, coords, tags, and meta fields");
        return false;
    }
    if (label_entry->value.tag != TABLO_EXT_TYPE_STRING || label_entry->value.is_nil) {
        ext_set_runtime_error(ctx, "extInvokeMapResultCallback expected label:string");
        return false;
    }
    if (total_entry->value.tag != TABLO_EXT_TYPE_INT || total_entry->value.is_nil) {
        ext_set_runtime_error(ctx, "extInvokeMapResultCallback expected total:int");
        return false;
    }
    if (coords_entry->value.tag != TABLO_EXT_TYPE_TUPLE || coords_entry->value.is_nil ||
        coords_entry->value.as.tuple_value.count != 2 || !coords_entry->value.as.tuple_value.items) {
        ext_set_runtime_error(ctx, "extInvokeMapResultCallback expected coords:(int, int)");
        return false;
    }
    coords_items = coords_entry->value.as.tuple_value.items;
    if (coords_items[0].tag != TABLO_EXT_TYPE_INT || coords_items[0].is_nil ||
        coords_items[1].tag != TABLO_EXT_TYPE_INT || coords_items[1].is_nil) {
        ext_set_runtime_error(ctx, "extInvokeMapResultCallback expected coords:(int, int)");
        return false;
    }
    if (tags_entry->value.tag != TABLO_EXT_TYPE_ARRAY || tags_entry->value.is_nil ||
        tags_entry->value.as.array_value.count < 0 ||
        (tags_entry->value.as.array_value.count > 0 && !tags_entry->value.as.array_value.items)) {
        ext_set_runtime_error(ctx, "extInvokeMapResultCallback expected tags:array<string>");
        return false;
    }
    if (!ext_read_meta_map(ctx,
                           &meta_entry->value,
                           "extInvokeMapResultCallback",
                           &meta_source,
                           &meta_source_len,
                           &meta_priority,
                           &meta_phase_count)) {
        return false;
    }

    written = snprintf(buffer,
                       sizeof(buffer),
                       "%.*s:%lld:%lld:%d:%.*s:%lld:%d",
                       label_entry->value.as.string_value.length,
                       label_entry->value.as.string_value.chars ? label_entry->value.as.string_value.chars : "",
                       (long long)total_entry->value.as.int_value,
                       (long long)coords_items[0].as.int_value,
                       tags_entry->value.as.array_value.count,
                       meta_source_len,
                       meta_source ? meta_source : "",
                       (long long)meta_priority,
                       meta_phase_count);
    if (written < 0 || written >= (int)sizeof(buffer)) {
        ext_set_runtime_error(ctx, "Failed to format extInvokeMapResultCallback result");
        return false;
    }
    return ctx->api->set_string_result(ctx, buffer, written);
}

static bool ext_start_posted_int_callback_worker(TabloExtCallContext* ctx) {
    const TabloExtCallback* callback = NULL;
    int64_t value = 0;
    int64_t delay_ms = 0;
    PostedIntCallbackWorkerHandle* handle = NULL;

    if (!ctx->api->get_callback_arg(ctx, 0, &callback)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &value)) return false;
    if (!ctx->api->get_int_arg(ctx, 2, &delay_ms)) return false;
    if (!callback) {
        ext_set_runtime_error(ctx, "extStartPostedIntCallbackWorker requires a non-nil callback");
        return false;
    }
    if (!g_ext_retain_callback || !g_ext_release_callback || !g_ext_post_callback) {
        ext_set_runtime_error(ctx, "Queued callback hooks are not initialized");
        return false;
    }
    if (delay_ms < 0 || delay_ms > 60000) {
        ext_set_runtime_error(ctx, "extStartPostedIntCallbackWorker delayMs must be between 0 and 60000");
        return false;
    }

    handle = (PostedIntCallbackWorkerHandle*)calloc(1, sizeof(PostedIntCallbackWorkerHandle));
    if (!handle) {
        ext_set_runtime_error(ctx, "Out of memory while allocating PostedIntCallbackWorker handle");
        return false;
    }
    if (mtx_init(&handle->mutex, mtx_plain) != thrd_success) {
        free(handle);
        ext_set_runtime_error(ctx, "Failed to initialize PostedIntCallbackWorker mutex");
        return false;
    }

    handle->callback = callback;
    handle->value = value;
    handle->delay_ms = (int)delay_ms;
    handle->joinable = false;
    handle->done = false;
    handle->post_ok = false;
    handle->error[0] = '\0';

    g_ext_retain_callback(callback);
    if (thrd_create(&handle->thread, ext_posted_int_callback_worker_main, handle) != thrd_success) {
        g_ext_release_callback(callback);
        mtx_destroy(&handle->mutex);
        free(handle);
        ext_set_runtime_error(ctx, "Failed to start PostedIntCallbackWorker thread");
        return false;
    }
    handle->joinable = true;

    if (!ctx->api->set_handle_result(ctx, "PostedIntCallbackWorker", handle)) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
        ext_posted_int_callback_worker_destroy(handle);
        return false;
    }
    return true;
}

static bool ext_join_posted_int_callback_worker(TabloExtCallContext* ctx) {
    void* payload = NULL;
    PostedIntCallbackWorkerHandle* handle = NULL;
    bool post_ok = false;
    char error_buf[256];

    if (!ctx->api->get_handle_arg(ctx, 0, "PostedIntCallbackWorker", &payload)) return false;
    handle = (PostedIntCallbackWorkerHandle*)payload;
    if (!handle) {
        ext_set_runtime_error(ctx, "PostedIntCallbackWorker handle payload is invalid");
        return false;
    }

    if (handle->joinable) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
    }

    mtx_lock(&handle->mutex);
    post_ok = handle->done && handle->post_ok;
    snprintf(error_buf, sizeof(error_buf), "%s", handle->error);
    mtx_unlock(&handle->mutex);

    if (!post_ok) {
        ext_set_runtime_error(ctx, error_buf[0] ? error_buf : "Queued callback worker did not complete successfully");
        return false;
    }

    return ctx->api->set_bool_result(ctx, true);
}

static bool ext_start_posted_tuple_callback_worker(TabloExtCallContext* ctx) {
    const TabloExtCallback* callback = NULL;
    const char* label = NULL;
    int label_len = 0;
    int64_t value = 0;
    int64_t delay_ms = 0;
    PostedTupleCallbackWorkerHandle* handle = NULL;

    if (!ctx->api->get_callback_arg(ctx, 0, &callback)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &value)) return false;
    if (!ctx->api->get_string_arg(ctx, 2, &label, &label_len)) return false;
    if (!ctx->api->get_int_arg(ctx, 3, &delay_ms)) return false;
    if (!callback) {
        ext_set_runtime_error(ctx, "extStartPostedTupleCallbackWorker requires a non-nil callback");
        return false;
    }
    if (!g_ext_retain_callback || !g_ext_release_callback || !g_ext_post_callback) {
        ext_set_runtime_error(ctx, "Queued callback hooks are not initialized");
        return false;
    }
    if (delay_ms < 0 || delay_ms > 60000) {
        ext_set_runtime_error(ctx, "extStartPostedTupleCallbackWorker delayMs must be between 0 and 60000");
        return false;
    }

    handle = (PostedTupleCallbackWorkerHandle*)calloc(1, sizeof(PostedTupleCallbackWorkerHandle));
    if (!handle) {
        ext_set_runtime_error(ctx, "Out of memory while allocating PostedTupleCallbackWorker handle");
        return false;
    }
    if (mtx_init(&handle->mutex, mtx_plain) != thrd_success) {
        free(handle);
        ext_set_runtime_error(ctx, "Failed to initialize PostedTupleCallbackWorker mutex");
        return false;
    }

    handle->label = (char*)malloc((size_t)label_len + 1u);
    if (!handle->label) {
        mtx_destroy(&handle->mutex);
        free(handle);
        ext_set_runtime_error(ctx, "Out of memory while copying PostedTupleCallbackWorker label");
        return false;
    }
    if (label_len > 0 && label) {
        memcpy(handle->label, label, (size_t)label_len);
    }
    handle->label[label_len] = '\0';

    handle->callback = callback;
    handle->value = value;
    handle->delay_ms = (int)delay_ms;
    handle->joinable = false;
    handle->done = false;
    handle->post_ok = false;
    handle->error[0] = '\0';

    g_ext_retain_callback(callback);
    if (thrd_create(&handle->thread, ext_posted_tuple_callback_worker_main, handle) != thrd_success) {
        g_ext_release_callback(callback);
        ext_posted_tuple_callback_worker_destroy(handle);
        ext_set_runtime_error(ctx, "Failed to start PostedTupleCallbackWorker thread");
        return false;
    }
    handle->joinable = true;

    if (!ctx->api->set_handle_result(ctx, "PostedTupleCallbackWorker", handle)) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
        ext_posted_tuple_callback_worker_destroy(handle);
        return false;
    }
    return true;
}

static bool ext_join_posted_tuple_callback_worker(TabloExtCallContext* ctx) {
    void* payload = NULL;
    PostedTupleCallbackWorkerHandle* handle = NULL;
    bool post_ok = false;
    char error_buf[256];

    if (!ctx->api->get_handle_arg(ctx, 0, "PostedTupleCallbackWorker", &payload)) return false;
    handle = (PostedTupleCallbackWorkerHandle*)payload;
    if (!handle) {
        ext_set_runtime_error(ctx, "PostedTupleCallbackWorker handle payload is invalid");
        return false;
    }

    if (handle->joinable) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
    }

    mtx_lock(&handle->mutex);
    post_ok = handle->done && handle->post_ok;
    snprintf(error_buf, sizeof(error_buf), "%s", handle->error);
    mtx_unlock(&handle->mutex);

    if (!post_ok) {
        ext_set_runtime_error(ctx, error_buf[0] ? error_buf : "Queued tuple callback worker did not complete successfully");
        return false;
    }

    return ctx->api->set_bool_result(ctx, true);
}

static bool ext_start_posted_array_callback_worker(TabloExtCallContext* ctx) {
    const TabloExtCallback* callback = NULL;
    int64_t value = 0;
    int64_t delay_ms = 0;
    PostedArrayCallbackWorkerHandle* handle = NULL;

    if (!ctx->api->get_callback_arg(ctx, 0, &callback)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &value)) return false;
    if (!ctx->api->get_int_arg(ctx, 2, &delay_ms)) return false;
    if (!callback) {
        ext_set_runtime_error(ctx, "extStartPostedArrayCallbackWorker requires a non-nil callback");
        return false;
    }
    if (!g_ext_retain_callback || !g_ext_release_callback || !g_ext_post_callback) {
        ext_set_runtime_error(ctx, "Queued callback hooks are not initialized");
        return false;
    }
    if (delay_ms < 0 || delay_ms > 60000) {
        ext_set_runtime_error(ctx, "extStartPostedArrayCallbackWorker delayMs must be between 0 and 60000");
        return false;
    }

    handle = (PostedArrayCallbackWorkerHandle*)calloc(1, sizeof(PostedArrayCallbackWorkerHandle));
    if (!handle) {
        ext_set_runtime_error(ctx, "Out of memory while allocating PostedArrayCallbackWorker handle");
        return false;
    }
    if (mtx_init(&handle->mutex, mtx_plain) != thrd_success) {
        free(handle);
        ext_set_runtime_error(ctx, "Failed to initialize PostedArrayCallbackWorker mutex");
        return false;
    }

    handle->callback = callback;
    handle->value = value;
    handle->delay_ms = (int)delay_ms;
    handle->joinable = false;
    handle->done = false;
    handle->post_ok = false;
    handle->error[0] = '\0';

    g_ext_retain_callback(callback);
    if (thrd_create(&handle->thread, ext_posted_array_callback_worker_main, handle) != thrd_success) {
        g_ext_release_callback(callback);
        mtx_destroy(&handle->mutex);
        free(handle);
        ext_set_runtime_error(ctx, "Failed to start PostedArrayCallbackWorker thread");
        return false;
    }
    handle->joinable = true;

    if (!ctx->api->set_handle_result(ctx, "PostedArrayCallbackWorker", handle)) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
        ext_posted_array_callback_worker_destroy(handle);
        return false;
    }
    return true;
}

static bool ext_join_posted_array_callback_worker(TabloExtCallContext* ctx) {
    void* payload = NULL;
    PostedArrayCallbackWorkerHandle* handle = NULL;
    bool post_ok = false;
    char error_buf[256];

    if (!ctx->api->get_handle_arg(ctx, 0, "PostedArrayCallbackWorker", &payload)) return false;
    handle = (PostedArrayCallbackWorkerHandle*)payload;
    if (!handle) {
        ext_set_runtime_error(ctx, "PostedArrayCallbackWorker handle payload is invalid");
        return false;
    }

    if (handle->joinable) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
    }

    mtx_lock(&handle->mutex);
    post_ok = handle->done && handle->post_ok;
    snprintf(error_buf, sizeof(error_buf), "%s", handle->error);
    mtx_unlock(&handle->mutex);

    if (!post_ok) {
        ext_set_runtime_error(ctx, error_buf[0] ? error_buf : "Queued array callback worker did not complete successfully");
        return false;
    }

    return ctx->api->set_bool_result(ctx, true);
}

static bool ext_start_posted_event_batch_callback_worker(TabloExtCallContext* ctx) {
    const TabloExtCallback* callback = NULL;
    int64_t value = 0;
    int64_t delay_ms = 0;
    PostedEventBatchCallbackWorkerHandle* handle = NULL;

    if (!ctx->api->get_callback_arg(ctx, 0, &callback)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &value)) return false;
    if (!ctx->api->get_int_arg(ctx, 2, &delay_ms)) return false;
    if (!callback) {
        ext_set_runtime_error(ctx, "extStartPostedEventBatchCallbackWorker requires a non-nil callback");
        return false;
    }
    if (!g_ext_retain_callback || !g_ext_release_callback || !g_ext_post_callback) {
        ext_set_runtime_error(ctx, "Queued callback hooks are not initialized");
        return false;
    }
    if (delay_ms < 0 || delay_ms > 60000) {
        ext_set_runtime_error(ctx, "extStartPostedEventBatchCallbackWorker delayMs must be between 0 and 60000");
        return false;
    }

    handle = (PostedEventBatchCallbackWorkerHandle*)calloc(1, sizeof(PostedEventBatchCallbackWorkerHandle));
    if (!handle) {
        ext_set_runtime_error(ctx, "Out of memory while allocating PostedEventBatchCallbackWorker handle");
        return false;
    }
    if (mtx_init(&handle->mutex, mtx_plain) != thrd_success) {
        free(handle);
        ext_set_runtime_error(ctx, "Failed to initialize PostedEventBatchCallbackWorker mutex");
        return false;
    }

    handle->callback = callback;
    handle->value = value;
    handle->delay_ms = (int)delay_ms;
    handle->joinable = false;
    handle->done = false;
    handle->post_ok = false;
    handle->error[0] = '\0';

    g_ext_retain_callback(callback);
    if (thrd_create(&handle->thread, ext_posted_event_batch_callback_worker_main, handle) != thrd_success) {
        g_ext_release_callback(callback);
        mtx_destroy(&handle->mutex);
        free(handle);
        ext_set_runtime_error(ctx, "Failed to start PostedEventBatchCallbackWorker thread");
        return false;
    }
    handle->joinable = true;

    if (!ctx->api->set_handle_result(ctx, "PostedEventBatchCallbackWorker", handle)) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
        ext_posted_event_batch_callback_worker_destroy(handle);
        return false;
    }
    return true;
}

static bool ext_join_posted_event_batch_callback_worker(TabloExtCallContext* ctx) {
    void* payload = NULL;
    PostedEventBatchCallbackWorkerHandle* handle = NULL;
    bool post_ok = false;
    char error_buf[256];

    if (!ctx->api->get_handle_arg(ctx, 0, "PostedEventBatchCallbackWorker", &payload)) return false;
    handle = (PostedEventBatchCallbackWorkerHandle*)payload;
    if (!handle) {
        ext_set_runtime_error(ctx, "PostedEventBatchCallbackWorker handle payload is invalid");
        return false;
    }

    if (handle->joinable) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
    }

    mtx_lock(&handle->mutex);
    post_ok = handle->done && handle->post_ok;
    snprintf(error_buf, sizeof(error_buf), "%s", handle->error);
    mtx_unlock(&handle->mutex);

    if (!post_ok) {
        ext_set_runtime_error(ctx, error_buf[0] ? error_buf : "Queued event batch callback worker did not complete successfully");
        return false;
    }

    return ctx->api->set_bool_result(ctx, true);
}

static bool ext_start_posted_map_callback_worker(TabloExtCallContext* ctx) {
    const TabloExtCallback* callback = NULL;
    int64_t value = 0;
    const char* label = NULL;
    int label_len = 0;
    int64_t delay_ms = 0;
    PostedMapCallbackWorkerHandle* handle = NULL;
    char* label_copy = NULL;

    if (!ctx->api->get_callback_arg(ctx, 0, &callback)) return false;
    if (!ctx->api->get_int_arg(ctx, 1, &value)) return false;
    if (!ctx->api->get_string_arg(ctx, 2, &label, &label_len)) return false;
    if (!ctx->api->get_int_arg(ctx, 3, &delay_ms)) return false;
    if (!callback) {
        ext_set_runtime_error(ctx, "extStartPostedMapCallbackWorker requires a non-nil callback");
        return false;
    }
    if (delay_ms < 0 || delay_ms > 60000) {
        ext_set_runtime_error(ctx, "extStartPostedMapCallbackWorker delayMs must be between 0 and 60000");
        return false;
    }

    label_copy = (char*)malloc((size_t)label_len + 1u);
    if (!label_copy) {
        ext_set_runtime_error(ctx, "Out of memory while copying extStartPostedMapCallbackWorker label");
        return false;
    }
    if (label_len > 0) {
        memcpy(label_copy, label, (size_t)label_len);
    }
    label_copy[label_len] = '\0';

    handle = (PostedMapCallbackWorkerHandle*)calloc(1, sizeof(PostedMapCallbackWorkerHandle));
    if (!handle) {
        free(label_copy);
        ext_set_runtime_error(ctx, "Out of memory while allocating PostedMapCallbackWorker handle");
        return false;
    }
    if (mtx_init(&handle->mutex, mtx_plain) != thrd_success) {
        free(label_copy);
        free(handle);
        ext_set_runtime_error(ctx, "Failed to initialize PostedMapCallbackWorker mutex");
        return false;
    }

    g_ext_retain_callback(callback);
    handle->callback = callback;
    handle->value = value;
    handle->label = label_copy;
    handle->delay_ms = (int)delay_ms;
    handle->joinable = true;
    handle->done = false;
    handle->post_ok = false;
    handle->error[0] = '\0';

    if (thrd_create(&handle->thread, ext_posted_map_callback_worker_main, handle) != thrd_success) {
        mtx_destroy(&handle->mutex);
        g_ext_release_callback(callback);
        free(label_copy);
        free(handle);
        ext_set_runtime_error(ctx, "Failed to start queued map callback worker thread");
        return false;
    }

    if (!ctx->api->set_handle_result(ctx, "PostedMapCallbackWorker", handle)) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
        g_ext_release_callback(callback);
        mtx_destroy(&handle->mutex);
        free(label_copy);
        free(handle);
        return false;
    }

    return true;
}

static bool ext_join_posted_map_callback_worker(TabloExtCallContext* ctx) {
    void* payload = NULL;
    PostedMapCallbackWorkerHandle* handle = NULL;
    bool post_ok = false;
    char error_buf[256];

    if (!ctx->api->get_handle_arg(ctx, 0, "PostedMapCallbackWorker", &payload)) return false;
    handle = (PostedMapCallbackWorkerHandle*)payload;
    if (!handle) {
        ext_set_runtime_error(ctx, "PostedMapCallbackWorker handle payload is invalid");
        return false;
    }

    if (handle->joinable) {
        thrd_join(handle->thread, NULL);
        handle->joinable = false;
    }

    mtx_lock(&handle->mutex);
    post_ok = handle->done && handle->post_ok;
    snprintf(error_buf, sizeof(error_buf), "%s", handle->error);
    mtx_unlock(&handle->mutex);

    if (!post_ok) {
        ext_set_runtime_error(ctx, error_buf[0] ? error_buf : "Queued map callback worker did not complete successfully");
        return false;
    }

    return ctx->api->set_bool_result(ctx, true);
}

TABLO_EXT_EXPORT bool tablo_extension_init(const TabloExtRegistrar* registrar,
                                          char* error_buf,
                                          size_t error_buf_size) {
    TabloExtHandleTypeDef counter_handle_def;
    TabloExtHandleTypeDef stored_callback_handle_def;
    TabloExtHandleTypeDef posted_callback_worker_handle_def;
    TabloExtHandleTypeDef posted_tuple_callback_worker_handle_def;
    TabloExtHandleTypeDef posted_array_callback_worker_handle_def;
    TabloExtHandleTypeDef posted_map_callback_worker_handle_def;
    TabloExtHandleTypeDef posted_event_batch_callback_worker_handle_def;
    TabloExtTypeDesc add_params[2];
    TabloExtTypeDesc hello_params[1];
    TabloExtTypeDesc starts_with_params[2];
    TabloExtTypeDesc scale_params[2];
    TabloExtTypeDesc echo_bytes_params[1];
    TabloExtTypeDesc counter_new_params[1];
    TabloExtTypeDesc counter_get_params[1];
    TabloExtTypeDesc counter_add_params[2];
    TabloExtTypeDesc counter_label_params[2];
    TabloExtTypeDesc sum_ints_params[1];
    TabloExtTypeDesc make_range_params[2];
    TabloExtTypeDesc sum_counters_params[1];
    TabloExtTypeDesc join_strings_params[2];
    TabloExtTypeDesc swap_pair_params[1];
    TabloExtTypeDesc snapshot_tuple_params[1];
    TabloExtTypeDesc make_event_batch_params[1];
    TabloExtTypeDesc summarize_event_batch_params[1];
    TabloExtTypeDesc invoke_int_callback_params[2];
    TabloExtTypeDesc store_int_callback_params[1];
    TabloExtTypeDesc invoke_stored_int_callback_params[2];
    TabloExtTypeDesc invoke_counter_label_callback_params[2];
    TabloExtTypeDesc invoke_array_arg_callback_params[2];
    TabloExtTypeDesc invoke_array_result_callback_params[2];
    TabloExtTypeDesc invoke_tuple_result_callback_params[2];
    TabloExtTypeDesc invoke_map_result_callback_params[2];
    TabloExtTypeDesc start_posted_int_callback_worker_params[3];
    TabloExtTypeDesc join_posted_int_callback_worker_params[1];
    TabloExtTypeDesc start_posted_tuple_callback_worker_params[4];
    TabloExtTypeDesc join_posted_tuple_callback_worker_params[1];
    TabloExtTypeDesc start_posted_array_callback_worker_params[3];
    TabloExtTypeDesc join_posted_array_callback_worker_params[1];
    TabloExtTypeDesc start_posted_event_batch_callback_worker_params[3];
    TabloExtTypeDesc join_posted_event_batch_callback_worker_params[1];
    TabloExtTypeDesc make_event_map_params[3];
    TabloExtTypeDesc summarize_event_map_params[1];
    TabloExtTypeDesc make_input_event_params[3];
    TabloExtTypeDesc summarize_input_event_params[1];
    TabloExtTypeDesc make_window_event_params[4];
    TabloExtTypeDesc summarize_window_event_params[1];
    TabloExtTypeDesc make_frame_event_params[3];
    TabloExtTypeDesc summarize_frame_event_params[1];
    TabloExtTypeDesc start_posted_map_callback_worker_params[4];
    TabloExtTypeDesc join_posted_map_callback_worker_params[1];
    TabloExtTypeDesc void_type;
    TabloExtTypeDesc int_type;
    TabloExtTypeDesc bool_type;
    TabloExtTypeDesc double_type;
    TabloExtTypeDesc string_type;
    TabloExtTypeDesc counter_type;
    TabloExtTypeDesc stored_callback_type;
    TabloExtTypeDesc posted_callback_worker_type;
    TabloExtTypeDesc posted_tuple_callback_worker_type;
    TabloExtTypeDesc posted_array_callback_worker_type;
    TabloExtTypeDesc posted_map_callback_worker_type;
    TabloExtTypeDesc posted_event_batch_callback_worker_type;
    TabloExtTypeDesc int_array_type;
    TabloExtTypeDesc string_array_type;
    TabloExtTypeDesc counter_array_type;
    TabloExtTypeDesc event_map_type;
    TabloExtTypeDesc event_map_array_type;
    TabloExtTypeDesc swap_pair_param_elements[2];
    TabloExtTypeDesc swap_pair_result_elements[2];
    TabloExtTypeDesc snapshot_tuple_result_elements[2];
    TabloExtTypeDesc posted_tuple_event_elements[2];
    TabloExtTypeDesc callback_tuple_result_elements[2];
    TabloExtTypeDesc pair_input_type;
    TabloExtTypeDesc pair_result_type;
    TabloExtTypeDesc snapshot_tuple_result_type;
    TabloExtTypeDesc posted_tuple_event_type;
    TabloExtTypeDesc callback_tuple_result_type;
    TabloExtTypeDesc int_callback_params[1];
    TabloExtTypeDesc counter_callback_params[1];
    TabloExtTypeDesc int_array_callback_params[1];
    TabloExtTypeDesc posted_tuple_callback_params[1];
    TabloExtTypeDesc posted_map_callback_params[1];
    TabloExtTypeDesc posted_event_batch_callback_params[1];
    TabloExtTypeDesc array_result_callback_params[1];
    TabloExtTypeDesc tuple_result_callback_params[1];
    TabloExtTypeDesc map_result_callback_params[1];
    TabloExtTypeDesc int_callback_type;
    TabloExtTypeDesc int_void_callback_type;
    TabloExtTypeDesc int_array_void_callback_type;
    TabloExtTypeDesc tuple_void_callback_type;
    TabloExtTypeDesc map_void_callback_type;
    TabloExtTypeDesc event_map_array_void_callback_type;
    TabloExtTypeDesc counter_label_callback_type;
    TabloExtTypeDesc int_array_callback_type;
    TabloExtTypeDesc array_result_callback_type;
    TabloExtTypeDesc tuple_result_callback_type;
    TabloExtTypeDesc map_result_callback_type;
    TabloExtFunctionDef add_def;
    TabloExtFunctionDef hello_def;
    TabloExtFunctionDef starts_with_def;
    TabloExtFunctionDef scale_def;
    TabloExtFunctionDef echo_bytes_def;
    TabloExtFunctionDef counter_new_def;
    TabloExtFunctionDef counter_get_def;
    TabloExtFunctionDef counter_add_def;
    TabloExtFunctionDef counter_label_def;
    TabloExtFunctionDef sum_ints_def;
    TabloExtFunctionDef make_range_def;
    TabloExtFunctionDef sum_counters_def;
    TabloExtFunctionDef join_strings_def;
    TabloExtFunctionDef swap_pair_def;
    TabloExtFunctionDef snapshot_tuple_def;
    TabloExtFunctionDef make_event_batch_def;
    TabloExtFunctionDef summarize_event_batch_def;
    TabloExtFunctionDef invoke_int_callback_def;
    TabloExtFunctionDef store_int_callback_def;
    TabloExtFunctionDef invoke_stored_int_callback_def;
    TabloExtFunctionDef invoke_counter_label_callback_def;
    TabloExtFunctionDef invoke_array_arg_callback_def;
    TabloExtFunctionDef invoke_array_result_callback_def;
    TabloExtFunctionDef invoke_tuple_result_callback_def;
    TabloExtFunctionDef invoke_map_result_callback_def;
    TabloExtFunctionDef start_posted_int_callback_worker_def;
    TabloExtFunctionDef join_posted_int_callback_worker_def;
    TabloExtFunctionDef start_posted_tuple_callback_worker_def;
    TabloExtFunctionDef join_posted_tuple_callback_worker_def;
    TabloExtFunctionDef start_posted_array_callback_worker_def;
    TabloExtFunctionDef join_posted_array_callback_worker_def;
    TabloExtFunctionDef start_posted_event_batch_callback_worker_def;
    TabloExtFunctionDef join_posted_event_batch_callback_worker_def;
    TabloExtFunctionDef make_event_map_def;
    TabloExtFunctionDef summarize_event_map_def;
    TabloExtFunctionDef make_input_event_def;
    TabloExtFunctionDef summarize_input_event_def;
    TabloExtFunctionDef make_window_event_def;
    TabloExtFunctionDef summarize_window_event_def;
    TabloExtFunctionDef make_frame_event_def;
    TabloExtFunctionDef summarize_frame_event_def;
    TabloExtFunctionDef start_posted_map_callback_worker_def;
    TabloExtFunctionDef join_posted_map_callback_worker_def;
    const TabloExtFunctionDef* function_defs[43];

    if (!registrar) {
        ext_set_init_error(error_buf, error_buf_size, "Missing extension registrar");
        return false;
    }
    if (registrar->abi_version != TABLO_EXT_ABI_VERSION) {
        ext_set_init_error(error_buf, error_buf_size, "Unsupported TabloLang extension ABI version");
        return false;
    }
    if (!registrar->register_handle_type || !registrar->register_function ||
        !registrar->retain_callback || !registrar->release_callback || !registrar->post_callback) {
        ext_set_init_error(error_buf, error_buf_size, "Registrar callbacks are not initialized");
        return false;
    }

    g_ext_retain_callback = registrar->retain_callback;
    g_ext_release_callback = registrar->release_callback;
    g_ext_post_callback = registrar->post_callback;

    counter_handle_def.name = "Counter";
    counter_handle_def.destroy = ext_counter_destroy;
    stored_callback_handle_def.name = "StoredIntCallback";
    stored_callback_handle_def.destroy = ext_stored_int_callback_destroy;
    posted_callback_worker_handle_def.name = "PostedIntCallbackWorker";
    posted_callback_worker_handle_def.destroy = ext_posted_int_callback_worker_destroy;
    posted_tuple_callback_worker_handle_def.name = "PostedTupleCallbackWorker";
    posted_tuple_callback_worker_handle_def.destroy = ext_posted_tuple_callback_worker_destroy;
    posted_array_callback_worker_handle_def.name = "PostedArrayCallbackWorker";
    posted_array_callback_worker_handle_def.destroy = ext_posted_array_callback_worker_destroy;
    posted_map_callback_worker_handle_def.name = "PostedMapCallbackWorker";
    posted_map_callback_worker_handle_def.destroy = ext_posted_map_callback_worker_destroy;
    posted_event_batch_callback_worker_handle_def.name = "PostedEventBatchCallbackWorker";
    posted_event_batch_callback_worker_handle_def.destroy = ext_posted_event_batch_callback_worker_destroy;

    void_type = ext_make_type(TABLO_EXT_TYPE_VOID, NULL, false);
    int_type = ext_make_type(TABLO_EXT_TYPE_INT, NULL, false);
    bool_type = ext_make_type(TABLO_EXT_TYPE_BOOL, NULL, false);
    double_type = ext_make_type(TABLO_EXT_TYPE_DOUBLE, NULL, false);
    string_type = ext_make_type(TABLO_EXT_TYPE_STRING, NULL, false);
    counter_type = ext_make_type(TABLO_EXT_TYPE_HANDLE, "Counter", false);
    stored_callback_type = ext_make_type(TABLO_EXT_TYPE_HANDLE, "StoredIntCallback", false);
    posted_callback_worker_type = ext_make_type(TABLO_EXT_TYPE_HANDLE, "PostedIntCallbackWorker", false);
    posted_tuple_callback_worker_type = ext_make_type(TABLO_EXT_TYPE_HANDLE, "PostedTupleCallbackWorker", false);
    posted_array_callback_worker_type = ext_make_type(TABLO_EXT_TYPE_HANDLE, "PostedArrayCallbackWorker", false);
    posted_map_callback_worker_type = ext_make_type(TABLO_EXT_TYPE_HANDLE, "PostedMapCallbackWorker", false);
    posted_event_batch_callback_worker_type = ext_make_type(TABLO_EXT_TYPE_HANDLE, "PostedEventBatchCallbackWorker", false);
    int_array_type = ext_make_array_type(&int_type, false);
    string_array_type = ext_make_array_type(&string_type, false);
    counter_array_type = ext_make_array_type(&counter_type, false);
    event_map_type = ext_make_map_type(false);
    event_map_array_type = ext_make_array_type(&event_map_type, false);
    swap_pair_param_elements[0] = int_type;
    swap_pair_param_elements[1] = string_type;
    swap_pair_result_elements[0] = string_type;
    swap_pair_result_elements[1] = int_type;
    snapshot_tuple_result_elements[0] = counter_type;
    snapshot_tuple_result_elements[1] = int_type;
    posted_tuple_event_elements[0] = int_type;
    posted_tuple_event_elements[1] = string_type;
    callback_tuple_result_elements[0] = int_type;
    callback_tuple_result_elements[1] = string_type;
    pair_input_type = ext_make_tuple_type(swap_pair_param_elements, 2, false);
    pair_result_type = ext_make_tuple_type(swap_pair_result_elements, 2, false);
    snapshot_tuple_result_type = ext_make_tuple_type(snapshot_tuple_result_elements, 2, false);
    posted_tuple_event_type = ext_make_tuple_type(posted_tuple_event_elements, 2, false);
    callback_tuple_result_type = ext_make_tuple_type(callback_tuple_result_elements, 2, false);
    int_callback_params[0] = int_type;
    counter_callback_params[0] = counter_type;
    int_array_callback_params[0] = int_array_type;
    posted_tuple_callback_params[0] = posted_tuple_event_type;
    posted_map_callback_params[0] = event_map_type;
    posted_event_batch_callback_params[0] = event_map_array_type;
    array_result_callback_params[0] = int_type;
    tuple_result_callback_params[0] = int_type;
    map_result_callback_params[0] = int_type;
    int_callback_type = ext_make_callback_type(&int_type, int_callback_params, 1, false);
    int_void_callback_type = ext_make_callback_type(&void_type, int_callback_params, 1, false);
    int_array_void_callback_type = ext_make_callback_type(&void_type, int_array_callback_params, 1, false);
    tuple_void_callback_type = ext_make_callback_type(&void_type, posted_tuple_callback_params, 1, false);
    map_void_callback_type = ext_make_callback_type(&void_type, posted_map_callback_params, 1, false);
    event_map_array_void_callback_type = ext_make_callback_type(&void_type, posted_event_batch_callback_params, 1, false);
    counter_label_callback_type = ext_make_callback_type(&string_type, counter_callback_params, 1, false);
    int_array_callback_type = ext_make_callback_type(&int_type, int_array_callback_params, 1, false);
    array_result_callback_type = ext_make_callback_type(&int_array_type, array_result_callback_params, 1, false);
    tuple_result_callback_type = ext_make_callback_type(&callback_tuple_result_type, tuple_result_callback_params, 1, false);
    map_result_callback_type = ext_make_callback_type(&event_map_type, map_result_callback_params, 1, false);

    add_params[0] = ext_make_type(TABLO_EXT_TYPE_INT, NULL, false);
    add_params[1] = ext_make_type(TABLO_EXT_TYPE_INT, NULL, false);
    hello_params[0] = ext_make_type(TABLO_EXT_TYPE_STRING, NULL, false);
    starts_with_params[0] = ext_make_type(TABLO_EXT_TYPE_STRING, NULL, false);
    starts_with_params[1] = ext_make_type(TABLO_EXT_TYPE_STRING, NULL, false);
    scale_params[0] = ext_make_type(TABLO_EXT_TYPE_DOUBLE, NULL, false);
    scale_params[1] = ext_make_type(TABLO_EXT_TYPE_DOUBLE, NULL, false);
    echo_bytes_params[0] = ext_make_type(TABLO_EXT_TYPE_BYTES, NULL, false);
    counter_new_params[0] = ext_make_type(TABLO_EXT_TYPE_INT, NULL, false);
    counter_get_params[0] = ext_make_type(TABLO_EXT_TYPE_HANDLE, "Counter", false);
    counter_add_params[0] = ext_make_type(TABLO_EXT_TYPE_HANDLE, "Counter", false);
    counter_add_params[1] = ext_make_type(TABLO_EXT_TYPE_INT, NULL, false);
    counter_label_params[0] = ext_make_type(TABLO_EXT_TYPE_HANDLE, "Counter", false);
    counter_label_params[1] = ext_make_type(TABLO_EXT_TYPE_STRING, NULL, false);
    sum_ints_params[0] = int_array_type;
    make_range_params[0] = int_type;
    make_range_params[1] = int_type;
    sum_counters_params[0] = counter_array_type;
    join_strings_params[0] = string_array_type;
    join_strings_params[1] = string_type;
    swap_pair_params[0] = pair_input_type;
    snapshot_tuple_params[0] = counter_type;
    make_event_batch_params[0] = int_type;
    summarize_event_batch_params[0] = event_map_array_type;
    invoke_int_callback_params[0] = int_callback_type;
    invoke_int_callback_params[1] = int_type;
    store_int_callback_params[0] = int_callback_type;
    invoke_stored_int_callback_params[0] = stored_callback_type;
    invoke_stored_int_callback_params[1] = int_type;
    invoke_counter_label_callback_params[0] = counter_label_callback_type;
    invoke_counter_label_callback_params[1] = counter_type;
    invoke_array_arg_callback_params[0] = int_array_callback_type;
    invoke_array_arg_callback_params[1] = int_type;
    invoke_array_result_callback_params[0] = array_result_callback_type;
    invoke_array_result_callback_params[1] = int_type;
    invoke_tuple_result_callback_params[0] = tuple_result_callback_type;
    invoke_tuple_result_callback_params[1] = int_type;
    invoke_map_result_callback_params[0] = map_result_callback_type;
    invoke_map_result_callback_params[1] = int_type;
    start_posted_int_callback_worker_params[0] = int_void_callback_type;
    start_posted_int_callback_worker_params[1] = int_type;
    start_posted_int_callback_worker_params[2] = int_type;
    join_posted_int_callback_worker_params[0] = posted_callback_worker_type;
    start_posted_tuple_callback_worker_params[0] = tuple_void_callback_type;
    start_posted_tuple_callback_worker_params[1] = int_type;
    start_posted_tuple_callback_worker_params[2] = string_type;
    start_posted_tuple_callback_worker_params[3] = int_type;
    join_posted_tuple_callback_worker_params[0] = posted_tuple_callback_worker_type;
    start_posted_array_callback_worker_params[0] = int_array_void_callback_type;
    start_posted_array_callback_worker_params[1] = int_type;
    start_posted_array_callback_worker_params[2] = int_type;
    join_posted_array_callback_worker_params[0] = posted_array_callback_worker_type;
    start_posted_event_batch_callback_worker_params[0] = event_map_array_void_callback_type;
    start_posted_event_batch_callback_worker_params[1] = int_type;
    start_posted_event_batch_callback_worker_params[2] = int_type;
    join_posted_event_batch_callback_worker_params[0] = posted_event_batch_callback_worker_type;
    make_event_map_params[0] = counter_type;
    make_event_map_params[1] = int_type;
    make_event_map_params[2] = string_type;
    summarize_event_map_params[0] = event_map_type;
    make_input_event_params[0] = string_type;
    make_input_event_params[1] = int_type;
    make_input_event_params[2] = bool_type;
    summarize_input_event_params[0] = event_map_type;
    make_window_event_params[0] = string_type;
    make_window_event_params[1] = int_type;
    make_window_event_params[2] = int_type;
    make_window_event_params[3] = bool_type;
    summarize_window_event_params[0] = event_map_type;
    make_frame_event_params[0] = string_type;
    make_frame_event_params[1] = int_type;
    make_frame_event_params[2] = double_type;
    summarize_frame_event_params[0] = event_map_type;
    start_posted_map_callback_worker_params[0] = map_void_callback_type;
    start_posted_map_callback_worker_params[1] = int_type;
    start_posted_map_callback_worker_params[2] = string_type;
    start_posted_map_callback_worker_params[3] = int_type;
    join_posted_map_callback_worker_params[0] = posted_map_callback_worker_type;

    add_def = ext_make_function_def("extAdd", ext_make_type(TABLO_EXT_TYPE_INT, NULL, false), add_params, 2, ext_add);
    hello_def = ext_make_function_def("extHello", ext_make_type(TABLO_EXT_TYPE_STRING, NULL, false), hello_params, 1, ext_hello);
    starts_with_def = ext_make_function_def("extStartsWith",
                                            ext_make_type(TABLO_EXT_TYPE_BOOL, NULL, false),
                                            starts_with_params,
                                            2,
                                            ext_starts_with);
    scale_def = ext_make_function_def("extScale", ext_make_type(TABLO_EXT_TYPE_DOUBLE, NULL, false), scale_params, 2, ext_scale);
    echo_bytes_def = ext_make_function_def("extEchoBytes",
                                           ext_make_type(TABLO_EXT_TYPE_BYTES, NULL, false),
                                           echo_bytes_params,
                                           1,
                                           ext_echo_bytes);
    counter_new_def = ext_make_function_def("counterNew",
                                            ext_make_type(TABLO_EXT_TYPE_HANDLE, "Counter", false),
                                            counter_new_params,
                                            1,
                                            ext_counter_new);
    counter_get_def = ext_make_function_def("counterGet",
                                            ext_make_type(TABLO_EXT_TYPE_INT, NULL, false),
                                            counter_get_params,
                                            1,
                                            ext_counter_get);
    counter_add_def = ext_make_function_def("counterAdd",
                                            ext_make_type(TABLO_EXT_TYPE_INT, NULL, false),
                                            counter_add_params,
                                            2,
                                            ext_counter_add);
    counter_label_def = ext_make_function_def("counterLabel",
                                              ext_make_type(TABLO_EXT_TYPE_STRING, NULL, false),
                                              counter_label_params,
                                              2,
                                              ext_counter_label);
    sum_ints_def = ext_make_function_def("extSumInts",
                                         int_type,
                                         sum_ints_params,
                                         1,
                                         ext_sum_ints);
    make_range_def = ext_make_function_def("extMakeIntRange",
                                           int_array_type,
                                           make_range_params,
                                           2,
                                           ext_make_int_range);
    sum_counters_def = ext_make_function_def("extSumCounters",
                                             int_type,
                                             sum_counters_params,
                                             1,
                                             ext_sum_counters);
    join_strings_def = ext_make_function_def("extJoinStrings",
                                             string_type,
                                             join_strings_params,
                                             2,
                                             ext_join_strings);
    swap_pair_def = ext_make_function_def("extSwapPair",
                                          pair_result_type,
                                          swap_pair_params,
                                          1,
                                          ext_swap_pair);
    snapshot_tuple_def = ext_make_function_def("extCounterSnapshotTuple",
                                               snapshot_tuple_result_type,
                                               snapshot_tuple_params,
                                               1,
                                               ext_counter_snapshot_tuple);
    make_event_batch_def = ext_make_function_def("extMakeEventBatch",
                                                 event_map_array_type,
                                                 make_event_batch_params,
                                                 1,
                                                 ext_make_event_batch);
    summarize_event_batch_def = ext_make_function_def("extSummarizeEventBatch",
                                                      string_type,
                                                      summarize_event_batch_params,
                                                      1,
                                                      ext_summarize_event_batch);
    invoke_int_callback_def = ext_make_function_def("extInvokeIntCallback",
                                                    int_type,
                                                    invoke_int_callback_params,
                                                    2,
                                                    ext_invoke_int_callback);
    store_int_callback_def = ext_make_function_def("extStoreIntCallback",
                                                   stored_callback_type,
                                                   store_int_callback_params,
                                                   1,
                                                   ext_store_int_callback);
    invoke_stored_int_callback_def = ext_make_function_def("extInvokeStoredIntCallback",
                                                           int_type,
                                                           invoke_stored_int_callback_params,
                                                           2,
                                                           ext_invoke_stored_int_callback);
    invoke_counter_label_callback_def = ext_make_function_def("extInvokeCounterLabelCallback",
                                                              string_type,
                                                              invoke_counter_label_callback_params,
                                                              2,
                                                              ext_invoke_counter_label_callback);
    invoke_array_arg_callback_def = ext_make_function_def("extInvokeArrayArgCallback",
                                                          int_type,
                                                          invoke_array_arg_callback_params,
                                                          2,
                                                          ext_invoke_array_arg_callback);
    invoke_array_result_callback_def = ext_make_function_def("extInvokeArrayResultCallback",
                                                             int_type,
                                                             invoke_array_result_callback_params,
                                                             2,
                                                             ext_invoke_array_result_callback);
    invoke_tuple_result_callback_def = ext_make_function_def("extInvokeTupleResultCallback",
                                                             string_type,
                                                             invoke_tuple_result_callback_params,
                                                             2,
                                                             ext_invoke_tuple_result_callback);
    invoke_map_result_callback_def = ext_make_function_def("extInvokeMapResultCallback",
                                                           string_type,
                                                           invoke_map_result_callback_params,
                                                           2,
                                                           ext_invoke_map_result_callback);
    start_posted_int_callback_worker_def = ext_make_function_def("extStartPostedIntCallbackWorker",
                                                                 posted_callback_worker_type,
                                                                 start_posted_int_callback_worker_params,
                                                                 3,
                                                                 ext_start_posted_int_callback_worker);
    join_posted_int_callback_worker_def = ext_make_function_def("extJoinPostedIntCallbackWorker",
                                                                ext_make_type(TABLO_EXT_TYPE_BOOL, NULL, false),
                                                                join_posted_int_callback_worker_params,
                                                                1,
                                                                ext_join_posted_int_callback_worker);
    start_posted_tuple_callback_worker_def = ext_make_function_def("extStartPostedTupleCallbackWorker",
                                                                   posted_tuple_callback_worker_type,
                                                                   start_posted_tuple_callback_worker_params,
                                                                   4,
                                                                   ext_start_posted_tuple_callback_worker);
    join_posted_tuple_callback_worker_def = ext_make_function_def("extJoinPostedTupleCallbackWorker",
                                                                  ext_make_type(TABLO_EXT_TYPE_BOOL, NULL, false),
                                                                  join_posted_tuple_callback_worker_params,
                                                                  1,
                                                                  ext_join_posted_tuple_callback_worker);
    start_posted_array_callback_worker_def = ext_make_function_def("extStartPostedArrayCallbackWorker",
                                                                   posted_array_callback_worker_type,
                                                                   start_posted_array_callback_worker_params,
                                                                   3,
                                                                   ext_start_posted_array_callback_worker);
    join_posted_array_callback_worker_def = ext_make_function_def("extJoinPostedArrayCallbackWorker",
                                                                  ext_make_type(TABLO_EXT_TYPE_BOOL, NULL, false),
                                                                  join_posted_array_callback_worker_params,
                                                                  1,
                                                                  ext_join_posted_array_callback_worker);
    start_posted_event_batch_callback_worker_def = ext_make_function_def("extStartPostedEventBatchCallbackWorker",
                                                                         posted_event_batch_callback_worker_type,
                                                                         start_posted_event_batch_callback_worker_params,
                                                                         3,
                                                                         ext_start_posted_event_batch_callback_worker);
    join_posted_event_batch_callback_worker_def = ext_make_function_def("extJoinPostedEventBatchCallbackWorker",
                                                                        ext_make_type(TABLO_EXT_TYPE_BOOL, NULL, false),
                                                                        join_posted_event_batch_callback_worker_params,
                                                                        1,
                                                                        ext_join_posted_event_batch_callback_worker);
    make_event_map_def = ext_make_function_def("extMakeEventMap",
                                               event_map_type,
                                               make_event_map_params,
                                               3,
                                               ext_make_event_map);
    summarize_event_map_def = ext_make_function_def("extSummarizeEventMap",
                                                    string_type,
                                                    summarize_event_map_params,
                                                    1,
                                                    ext_summarize_event_map);
    make_input_event_def = ext_make_function_def("extMakeInputEvent",
                                                 event_map_type,
                                                 make_input_event_params,
                                                 3,
                                                 ext_make_input_event);
    summarize_input_event_def = ext_make_function_def("extSummarizeInputEvent",
                                                      string_type,
                                                      summarize_input_event_params,
                                                      1,
                                                      ext_summarize_input_event);
    make_window_event_def = ext_make_function_def("extMakeWindowEvent",
                                                  event_map_type,
                                                  make_window_event_params,
                                                  4,
                                                  ext_make_window_event);
    summarize_window_event_def = ext_make_function_def("extSummarizeWindowEvent",
                                                       string_type,
                                                       summarize_window_event_params,
                                                       1,
                                                       ext_summarize_window_event);
    make_frame_event_def = ext_make_function_def("extMakeFrameEvent",
                                                 event_map_type,
                                                 make_frame_event_params,
                                                 3,
                                                 ext_make_frame_event);
    summarize_frame_event_def = ext_make_function_def("extSummarizeFrameEvent",
                                                      string_type,
                                                      summarize_frame_event_params,
                                                      1,
                                                      ext_summarize_frame_event);
    start_posted_map_callback_worker_def = ext_make_function_def("extStartPostedMapCallbackWorker",
                                                                 posted_map_callback_worker_type,
                                                                 start_posted_map_callback_worker_params,
                                                                 4,
                                                                 ext_start_posted_map_callback_worker);
    join_posted_map_callback_worker_def = ext_make_function_def("extJoinPostedMapCallbackWorker",
                                                                ext_make_type(TABLO_EXT_TYPE_BOOL, NULL, false),
                                                                join_posted_map_callback_worker_params,
                                                                1,
                                                                ext_join_posted_map_callback_worker);

    function_defs[0] = &add_def;
    function_defs[1] = &hello_def;
    function_defs[2] = &starts_with_def;
    function_defs[3] = &scale_def;
    function_defs[4] = &echo_bytes_def;
    function_defs[5] = &counter_new_def;
    function_defs[6] = &counter_get_def;
    function_defs[7] = &counter_add_def;
    function_defs[8] = &counter_label_def;
    function_defs[9] = &sum_ints_def;
    function_defs[10] = &make_range_def;
    function_defs[11] = &sum_counters_def;
    function_defs[12] = &join_strings_def;
    function_defs[13] = &swap_pair_def;
    function_defs[14] = &snapshot_tuple_def;
    function_defs[15] = &make_event_batch_def;
    function_defs[16] = &summarize_event_batch_def;
    function_defs[17] = &invoke_int_callback_def;
    function_defs[18] = &store_int_callback_def;
    function_defs[19] = &invoke_stored_int_callback_def;
    function_defs[20] = &invoke_counter_label_callback_def;
    function_defs[21] = &invoke_array_arg_callback_def;
    function_defs[22] = &invoke_array_result_callback_def;
    function_defs[23] = &invoke_tuple_result_callback_def;
    function_defs[24] = &invoke_map_result_callback_def;
    function_defs[25] = &start_posted_int_callback_worker_def;
    function_defs[26] = &join_posted_int_callback_worker_def;
    function_defs[27] = &start_posted_tuple_callback_worker_def;
    function_defs[28] = &join_posted_tuple_callback_worker_def;
    function_defs[29] = &start_posted_array_callback_worker_def;
    function_defs[30] = &join_posted_array_callback_worker_def;
    function_defs[31] = &start_posted_event_batch_callback_worker_def;
    function_defs[32] = &join_posted_event_batch_callback_worker_def;
    function_defs[33] = &make_event_map_def;
    function_defs[34] = &summarize_event_map_def;
    function_defs[35] = &make_input_event_def;
    function_defs[36] = &summarize_input_event_def;
    function_defs[37] = &make_window_event_def;
    function_defs[38] = &summarize_window_event_def;
    function_defs[39] = &make_frame_event_def;
    function_defs[40] = &summarize_frame_event_def;
    function_defs[41] = &start_posted_map_callback_worker_def;
    function_defs[42] = &join_posted_map_callback_worker_def;

    if (!registrar->register_handle_type(registrar->host_context,
                                         &counter_handle_def,
                                         error_buf,
                                         error_buf_size)) {
        return false;
    }
    if (!registrar->register_handle_type(registrar->host_context,
                                         &stored_callback_handle_def,
                                         error_buf,
                                         error_buf_size)) {
        return false;
    }
    if (!registrar->register_handle_type(registrar->host_context,
                                         &posted_callback_worker_handle_def,
                                         error_buf,
                                         error_buf_size)) {
        return false;
    }
    if (!registrar->register_handle_type(registrar->host_context,
                                         &posted_tuple_callback_worker_handle_def,
                                         error_buf,
                                         error_buf_size)) {
        return false;
    }
    if (!registrar->register_handle_type(registrar->host_context,
                                         &posted_array_callback_worker_handle_def,
                                         error_buf,
                                         error_buf_size)) {
        return false;
    }
    if (!registrar->register_handle_type(registrar->host_context,
                                         &posted_map_callback_worker_handle_def,
                                         error_buf,
                                         error_buf_size)) {
        return false;
    }
    if (!registrar->register_handle_type(registrar->host_context,
                                         &posted_event_batch_callback_worker_handle_def,
                                         error_buf,
                                         error_buf_size)) {
        return false;
    }

    for (size_t i = 0; i < sizeof(function_defs) / sizeof(function_defs[0]); i++) {
        if (!registrar->register_function(registrar->host_context,
                                          function_defs[i],
                                          error_buf,
                                          error_buf_size)) {
            return false;
        }
    }

    return true;
}
