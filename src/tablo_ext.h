#ifndef TABLO_EXT_H
#define TABLO_EXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TABLO_EXT_ABI_VERSION 13u
#define TABLO_EXT_INIT_SYMBOL "tablo_extension_init"
#define TABLO_EXT_SHUTDOWN_SYMBOL "tablo_extension_shutdown"

#ifdef _WIN32
#define TABLO_EXT_EXPORT __declspec(dllexport)
#else
#define TABLO_EXT_EXPORT __attribute__((visibility("default")))
#endif

typedef enum {
    TABLO_EXT_TYPE_VOID = 0,
    TABLO_EXT_TYPE_INT = 1,
    TABLO_EXT_TYPE_BOOL = 2,
    TABLO_EXT_TYPE_DOUBLE = 3,
    TABLO_EXT_TYPE_STRING = 4,
    TABLO_EXT_TYPE_BYTES = 5,
    TABLO_EXT_TYPE_HANDLE = 6,
    TABLO_EXT_TYPE_ARRAY = 7,
    TABLO_EXT_TYPE_TUPLE = 8,
    TABLO_EXT_TYPE_MAP = 9,
    TABLO_EXT_TYPE_CALLBACK = 10
} TabloExtTypeTag;

typedef struct TabloExtTypeDesc TabloExtTypeDesc;
typedef struct TabloExtCallback TabloExtCallback;
typedef struct TabloExtCallbackGate TabloExtCallbackGate;
typedef struct TabloExtValue TabloExtValue;
typedef struct TabloExtMapEntry TabloExtMapEntry;

struct TabloExtTypeDesc {
    TabloExtTypeTag tag;
    const char* handle_type_name;
    const TabloExtTypeDesc* element_type;
    const TabloExtTypeDesc* tuple_element_types;
    int tuple_element_count;
    const TabloExtTypeDesc* callback_param_types;
    int callback_param_count;
    const TabloExtTypeDesc* callback_result_type;
    bool nullable;
};

typedef struct {
    const char* chars;
    int length;
} TabloExtStringValue;

typedef struct {
    const uint8_t* bytes;
    int length;
} TabloExtBytesValue;

typedef struct {
    const TabloExtValue* items;
    int count;
} TabloExtArrayValue;

typedef struct {
    const TabloExtValue* items;
    int count;
} TabloExtTupleValue;

typedef struct {
    const TabloExtMapEntry* entries;
    int count;
} TabloExtMapValue;

struct TabloExtValue {
    TabloExtTypeTag tag;
    bool is_nil;
    const char* handle_type_name;
    union {
        int64_t int_value;
        bool bool_value;
        double double_value;
        TabloExtStringValue string_value;
        TabloExtBytesValue bytes_value;
        TabloExtArrayValue array_value;
        TabloExtTupleValue tuple_value;
        TabloExtMapValue map_value;
        void* handle_value;
    } as;
};

struct TabloExtMapEntry {
    const char* key_chars;
    int key_length;
    TabloExtValue value;
};

typedef struct {
    TabloExtValue* items;
    int count;
    int capacity;
} TabloExtArrayBuilder;

typedef struct {
    TabloExtMapEntry* entries;
    int count;
    int capacity;
} TabloExtMapBuilder;

typedef struct {
    const char* const* chars;
    const int* lengths;
    int count;
} TabloExtStringListSpec;

typedef struct {
    const char* source_chars;
    int source_length;
    int64_t priority;
    TabloExtStringListSpec phases;
} TabloExtEventMetaSpec;

typedef struct {
    const char* name_chars;
    int name_length;
    int64_t delta;
    TabloExtValue meta_value;
} TabloExtNamedEventSpec;

typedef struct {
    const char* device_chars;
    int device_length;
    int64_t code;
    bool pressed;
    TabloExtValue meta_value;
} TabloExtInputEventSpec;

typedef struct {
    const char* event_chars;
    int event_length;
    int64_t width;
    int64_t height;
    bool focused;
    TabloExtValue meta_value;
} TabloExtWindowEventSpec;

typedef struct {
    const char* phase_chars;
    int phase_length;
    int64_t frame_number;
    double delta_seconds;
    TabloExtValue meta_value;
} TabloExtFrameEventSpec;

static inline TabloExtValue tablo_ext_make_nil_value(TabloExtTypeTag tag) {
    TabloExtValue value = {0};
    value.tag = tag;
    value.is_nil = true;
    return value;
}

static inline TabloExtValue tablo_ext_make_int_value(int64_t int_value) {
    TabloExtValue value = {0};
    value.tag = TABLO_EXT_TYPE_INT;
    value.as.int_value = int_value;
    return value;
}

static inline TabloExtValue tablo_ext_make_bool_value(bool bool_value) {
    TabloExtValue value = {0};
    value.tag = TABLO_EXT_TYPE_BOOL;
    value.as.bool_value = bool_value;
    return value;
}

static inline TabloExtValue tablo_ext_make_double_value(double double_value) {
    TabloExtValue value = {0};
    value.tag = TABLO_EXT_TYPE_DOUBLE;
    value.as.double_value = double_value;
    return value;
}

static inline TabloExtValue tablo_ext_make_string_value(const char* chars, int length) {
    TabloExtValue value = {0};
    value.tag = TABLO_EXT_TYPE_STRING;
    value.as.string_value.chars = chars ? chars : "";
    value.as.string_value.length = length >= 0 ? length : (int)strlen(chars ? chars : "");
    return value;
}

static inline TabloExtValue tablo_ext_make_bytes_value(const uint8_t* bytes, int length) {
    TabloExtValue value = {0};
    value.tag = TABLO_EXT_TYPE_BYTES;
    value.as.bytes_value.bytes = bytes;
    value.as.bytes_value.length = length;
    return value;
}

static inline TabloExtValue tablo_ext_make_handle_value(const char* handle_type_name, void* handle_value) {
    TabloExtValue value = {0};
    value.tag = TABLO_EXT_TYPE_HANDLE;
    value.handle_type_name = handle_type_name;
    value.as.handle_value = handle_value;
    return value;
}

static inline TabloExtValue tablo_ext_make_array_value(const TabloExtValue* items, int count) {
    TabloExtValue value = {0};
    value.tag = TABLO_EXT_TYPE_ARRAY;
    value.as.array_value.items = items;
    value.as.array_value.count = count;
    return value;
}

static inline TabloExtValue tablo_ext_make_tuple_value(const TabloExtValue* items, int count) {
    TabloExtValue value = {0};
    value.tag = TABLO_EXT_TYPE_TUPLE;
    value.as.tuple_value.items = items;
    value.as.tuple_value.count = count;
    return value;
}

static inline TabloExtValue tablo_ext_make_map_value(const TabloExtMapEntry* entries, int count) {
    TabloExtValue value = {0};
    value.tag = TABLO_EXT_TYPE_MAP;
    value.as.map_value.entries = entries;
    value.as.map_value.count = count;
    return value;
}

static inline TabloExtMapEntry tablo_ext_make_map_entry(const char* key_chars, int key_length, TabloExtValue value) {
    TabloExtMapEntry entry;
    entry.key_chars = key_chars;
    entry.key_length = key_length >= 0 ? key_length : (int)strlen(key_chars ? key_chars : "");
    entry.value = value;
    return entry;
}

static inline const TabloExtMapEntry* tablo_ext_find_map_entry(const TabloExtMapEntry* entries, int count, const char* key) {
    int key_length = 0;
    if (!entries || count <= 0 || !key) return NULL;
    key_length = (int)strlen(key);
    for (int i = 0; i < count; i++) {
        int entry_length = entries[i].key_length >= 0
                               ? entries[i].key_length
                               : (int)strlen(entries[i].key_chars ? entries[i].key_chars : "");
        if (!entries[i].key_chars) continue;
        if (entry_length != key_length) continue;
        if (memcmp(entries[i].key_chars, key, (size_t)key_length) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

static inline void tablo_ext_array_builder_init(TabloExtArrayBuilder* builder, TabloExtValue* items, int capacity) {
    if (!builder) return;
    builder->items = items;
    builder->count = 0;
    builder->capacity = capacity;
}

static inline bool tablo_ext_array_builder_add_value(TabloExtArrayBuilder* builder, TabloExtValue value) {
    if (!builder || !builder->items || builder->count < 0 || builder->count >= builder->capacity) return false;
    builder->items[builder->count++] = value;
    return true;
}

static inline bool tablo_ext_array_builder_add_nil(TabloExtArrayBuilder* builder, TabloExtTypeTag tag) {
    return tablo_ext_array_builder_add_value(builder, tablo_ext_make_nil_value(tag));
}

static inline bool tablo_ext_array_builder_add_int(TabloExtArrayBuilder* builder, int64_t int_value) {
    return tablo_ext_array_builder_add_value(builder, tablo_ext_make_int_value(int_value));
}

static inline bool tablo_ext_array_builder_add_bool(TabloExtArrayBuilder* builder, bool bool_value) {
    return tablo_ext_array_builder_add_value(builder, tablo_ext_make_bool_value(bool_value));
}

static inline bool tablo_ext_array_builder_add_double(TabloExtArrayBuilder* builder, double double_value) {
    return tablo_ext_array_builder_add_value(builder, tablo_ext_make_double_value(double_value));
}

static inline bool tablo_ext_array_builder_add_string(TabloExtArrayBuilder* builder, const char* chars, int length) {
    return tablo_ext_array_builder_add_value(builder, tablo_ext_make_string_value(chars, length));
}

static inline bool tablo_ext_array_builder_add_bytes(TabloExtArrayBuilder* builder, const uint8_t* bytes, int length) {
    return tablo_ext_array_builder_add_value(builder, tablo_ext_make_bytes_value(bytes, length));
}

static inline bool tablo_ext_array_builder_add_handle(TabloExtArrayBuilder* builder,
                                                    const char* handle_type_name,
                                                    void* handle_value) {
    return tablo_ext_array_builder_add_value(builder, tablo_ext_make_handle_value(handle_type_name, handle_value));
}

static inline TabloExtValue tablo_ext_array_builder_build_array(const TabloExtArrayBuilder* builder) {
    if (!builder || !builder->items || builder->count < 0) {
        return tablo_ext_make_array_value(NULL, 0);
    }
    return tablo_ext_make_array_value(builder->items, builder->count);
}

static inline TabloExtValue tablo_ext_array_builder_build_tuple(const TabloExtArrayBuilder* builder) {
    if (!builder || !builder->items || builder->count < 0) {
        return tablo_ext_make_tuple_value(NULL, 0);
    }
    return tablo_ext_make_tuple_value(builder->items, builder->count);
}

static inline void tablo_ext_map_builder_init(TabloExtMapBuilder* builder, TabloExtMapEntry* entries, int capacity) {
    if (!builder) return;
    builder->entries = entries;
    builder->count = 0;
    builder->capacity = capacity;
}

static inline bool tablo_ext_map_builder_add_value(TabloExtMapBuilder* builder,
                                                 const char* key_chars,
                                                 int key_length,
                                                 TabloExtValue value) {
    if (!builder || !builder->entries || builder->count < 0 || builder->count >= builder->capacity) return false;
    builder->entries[builder->count++] = tablo_ext_make_map_entry(key_chars, key_length, value);
    return true;
}

static inline bool tablo_ext_map_builder_add_nil(TabloExtMapBuilder* builder,
                                               const char* key_chars,
                                               int key_length,
                                               TabloExtTypeTag tag) {
    return tablo_ext_map_builder_add_value(builder, key_chars, key_length, tablo_ext_make_nil_value(tag));
}

static inline bool tablo_ext_map_builder_add_int(TabloExtMapBuilder* builder,
                                               const char* key_chars,
                                               int key_length,
                                               int64_t int_value) {
    return tablo_ext_map_builder_add_value(builder, key_chars, key_length, tablo_ext_make_int_value(int_value));
}

static inline bool tablo_ext_map_builder_add_bool(TabloExtMapBuilder* builder,
                                                const char* key_chars,
                                                int key_length,
                                                bool bool_value) {
    return tablo_ext_map_builder_add_value(builder, key_chars, key_length, tablo_ext_make_bool_value(bool_value));
}

static inline bool tablo_ext_map_builder_add_double(TabloExtMapBuilder* builder,
                                                  const char* key_chars,
                                                  int key_length,
                                                  double double_value) {
    return tablo_ext_map_builder_add_value(builder, key_chars, key_length, tablo_ext_make_double_value(double_value));
}

static inline bool tablo_ext_map_builder_add_string(TabloExtMapBuilder* builder,
                                                  const char* key_chars,
                                                  int key_length,
                                                  const char* chars,
                                                  int length) {
    return tablo_ext_map_builder_add_value(builder, key_chars, key_length, tablo_ext_make_string_value(chars, length));
}

static inline bool tablo_ext_map_builder_add_bytes(TabloExtMapBuilder* builder,
                                                 const char* key_chars,
                                                 int key_length,
                                                 const uint8_t* bytes,
                                                 int length) {
    return tablo_ext_map_builder_add_value(builder, key_chars, key_length, tablo_ext_make_bytes_value(bytes, length));
}

static inline bool tablo_ext_map_builder_add_handle(TabloExtMapBuilder* builder,
                                                  const char* key_chars,
                                                  int key_length,
                                                  const char* handle_type_name,
                                                  void* handle_value) {
    return tablo_ext_map_builder_add_value(builder,
                                         key_chars,
                                         key_length,
                                         tablo_ext_make_handle_value(handle_type_name, handle_value));
}

static inline TabloExtValue tablo_ext_map_builder_build_map(const TabloExtMapBuilder* builder) {
    if (!builder || !builder->entries || builder->count < 0) {
        return tablo_ext_make_map_value(NULL, 0);
    }
    return tablo_ext_make_map_value(builder->entries, builder->count);
}

static inline bool tablo_ext_build_string_array(TabloExtValue* out_value,
                                              TabloExtValue* item_storage,
                                              int item_capacity,
                                              const char* const* chars,
                                              const int* lengths,
                                              int count) {
    TabloExtArrayBuilder builder;
    if (!out_value || !item_storage || item_capacity < 0 || count < 0) return false;
    tablo_ext_array_builder_init(&builder, item_storage, item_capacity);
    for (int i = 0; i < count; i++) {
        const char* chars_i = chars && chars[i] ? chars[i] : "";
        int length_i = lengths ? lengths[i] : -1;
        if (!tablo_ext_array_builder_add_string(&builder, chars_i, length_i)) {
            return false;
        }
    }
    *out_value = tablo_ext_array_builder_build_array(&builder);
    return true;
}

static inline bool tablo_ext_build_event_meta_map(TabloExtValue* out_value,
                                                TabloExtMapEntry* map_entries,
                                                int map_capacity,
                                                TabloExtValue* phase_storage,
                                                int phase_capacity,
                                                const TabloExtEventMetaSpec* spec) {
    TabloExtMapBuilder builder;
    TabloExtValue phases_value;
    if (!out_value || !map_entries || !phase_storage || !spec) return false;
    if (!tablo_ext_build_string_array(&phases_value,
                                    phase_storage,
                                    phase_capacity,
                                    spec->phases.chars,
                                    spec->phases.lengths,
                                    spec->phases.count)) {
        return false;
    }
    tablo_ext_map_builder_init(&builder, map_entries, map_capacity);
    if (!tablo_ext_map_builder_add_string(&builder,
                                        "source",
                                        6,
                                        spec->source_chars ? spec->source_chars : "",
                                        spec->source_length) ||
        !tablo_ext_map_builder_add_int(&builder, "priority", 8, spec->priority) ||
        !tablo_ext_map_builder_add_value(&builder, "phases", 6, phases_value)) {
        return false;
    }
    *out_value = tablo_ext_map_builder_build_map(&builder);
    return true;
}

static inline bool tablo_ext_build_named_event_map(TabloExtValue* out_value,
                                                 TabloExtMapEntry* map_entries,
                                                 int map_capacity,
                                                 const TabloExtNamedEventSpec* spec) {
    TabloExtMapBuilder builder;
    if (!out_value || !map_entries || !spec) return false;
    tablo_ext_map_builder_init(&builder, map_entries, map_capacity);
    if (!tablo_ext_map_builder_add_string(&builder,
                                        "name",
                                        4,
                                        spec->name_chars ? spec->name_chars : "",
                                        spec->name_length) ||
        !tablo_ext_map_builder_add_int(&builder, "delta", 5, spec->delta) ||
        !tablo_ext_map_builder_add_value(&builder, "meta", 4, spec->meta_value)) {
        return false;
    }
    *out_value = tablo_ext_map_builder_build_map(&builder);
    return true;
}

static inline bool tablo_ext_build_input_event_map(TabloExtValue* out_value,
                                                 TabloExtMapEntry* map_entries,
                                                 int map_capacity,
                                                 const TabloExtInputEventSpec* spec) {
    TabloExtMapBuilder builder;
    if (!out_value || !map_entries || !spec) return false;
    tablo_ext_map_builder_init(&builder, map_entries, map_capacity);
    if (!tablo_ext_map_builder_add_string(&builder, "kind", 4, "input", 5) ||
        !tablo_ext_map_builder_add_string(&builder,
                                        "device",
                                        6,
                                        spec->device_chars ? spec->device_chars : "",
                                        spec->device_length) ||
        !tablo_ext_map_builder_add_int(&builder, "code", 4, spec->code) ||
        !tablo_ext_map_builder_add_bool(&builder, "pressed", 7, spec->pressed) ||
        !tablo_ext_map_builder_add_value(&builder, "meta", 4, spec->meta_value)) {
        return false;
    }
    *out_value = tablo_ext_map_builder_build_map(&builder);
    return true;
}

static inline bool tablo_ext_build_window_event_map(TabloExtValue* out_value,
                                                  TabloExtMapEntry* map_entries,
                                                  int map_capacity,
                                                  const TabloExtWindowEventSpec* spec) {
    TabloExtMapBuilder builder;
    if (!out_value || !map_entries || !spec) return false;
    tablo_ext_map_builder_init(&builder, map_entries, map_capacity);
    if (!tablo_ext_map_builder_add_string(&builder, "kind", 4, "window", 6) ||
        !tablo_ext_map_builder_add_string(&builder,
                                        "event",
                                        5,
                                        spec->event_chars ? spec->event_chars : "",
                                        spec->event_length) ||
        !tablo_ext_map_builder_add_int(&builder, "width", 5, spec->width) ||
        !tablo_ext_map_builder_add_int(&builder, "height", 6, spec->height) ||
        !tablo_ext_map_builder_add_bool(&builder, "focused", 7, spec->focused) ||
        !tablo_ext_map_builder_add_value(&builder, "meta", 4, spec->meta_value)) {
        return false;
    }
    *out_value = tablo_ext_map_builder_build_map(&builder);
    return true;
}

static inline bool tablo_ext_build_frame_event_map(TabloExtValue* out_value,
                                                 TabloExtMapEntry* map_entries,
                                                 int map_capacity,
                                                 const TabloExtFrameEventSpec* spec) {
    TabloExtMapBuilder builder;
    if (!out_value || !map_entries || !spec) return false;
    tablo_ext_map_builder_init(&builder, map_entries, map_capacity);
    if (!tablo_ext_map_builder_add_string(&builder, "kind", 4, "frame", 5) ||
        !tablo_ext_map_builder_add_string(&builder,
                                        "phase",
                                        5,
                                        spec->phase_chars ? spec->phase_chars : "",
                                        spec->phase_length) ||
        !tablo_ext_map_builder_add_int(&builder, "frame", 5, spec->frame_number) ||
        !tablo_ext_map_builder_add_double(&builder, "dt", 2, spec->delta_seconds) ||
        !tablo_ext_map_builder_add_value(&builder, "meta", 4, spec->meta_value)) {
        return false;
    }
    *out_value = tablo_ext_map_builder_build_map(&builder);
    return true;
}

struct TabloExtApi;

typedef struct {
    const struct TabloExtApi* api;
    void* host_context;
} TabloExtCallContext;

typedef bool (*TabloExtFunctionCallback)(TabloExtCallContext* ctx);
typedef void (*TabloExtHandleDestroyFn)(void* payload);

typedef struct {
    const char* name;
    TabloExtHandleDestroyFn destroy;
} TabloExtHandleTypeDef;

typedef struct {
    const char* name;
    TabloExtTypeDesc result_type;
    const TabloExtTypeDesc* param_types;
    int param_count;
    TabloExtFunctionCallback callback;
} TabloExtFunctionDef;

typedef struct {
    uint32_t abi_version;
    void* host_context;
    bool (*register_handle_type)(void* host_context,
                                 const TabloExtHandleTypeDef* def,
                                 char* error_buf,
                                 size_t error_buf_size);
    bool (*register_function)(void* host_context,
                              const TabloExtFunctionDef* def,
                              char* error_buf,
                              size_t error_buf_size);
    void (*retain_callback)(const TabloExtCallback* callback);
    void (*release_callback)(const TabloExtCallback* callback);
    bool (*post_callback)(const TabloExtCallback* callback,
                          const TabloExtValue* args,
                          int arg_count,
                          char* error_buf,
                          size_t error_buf_size);
    TabloExtCallbackGate* (*create_callback_gate)(void);
    void (*retain_callback_gate)(TabloExtCallbackGate* gate);
    void (*release_callback_gate)(TabloExtCallbackGate* gate);
    uint64_t (*reset_callback_gate)(TabloExtCallbackGate* gate);
    int64_t (*get_callback_gate_invalidated_count)(const TabloExtCallbackGate* gate);
    int64_t (*reset_callback_gate_invalidated_count)(TabloExtCallbackGate* gate);
    bool (*post_callback_gated)(const TabloExtCallback* callback,
                                const TabloExtValue* args,
                                int arg_count,
                                const TabloExtCallbackGate* gate,
                                uint64_t gate_generation,
                                char* error_buf,
                                size_t error_buf_size);
    int (*get_posted_callback_pending_count)(const TabloExtCallback* callback);
    bool (*is_posted_callback_queue_open)(const TabloExtCallback* callback);
} TabloExtRegistrar;

typedef struct TabloExtApi {
    uint32_t abi_version;
    int (*arg_count)(TabloExtCallContext* ctx);
    bool (*arg_is_nil)(TabloExtCallContext* ctx, int index);
    bool (*get_int_arg)(TabloExtCallContext* ctx, int index, int64_t* out_value);
    bool (*get_bool_arg)(TabloExtCallContext* ctx, int index, bool* out_value);
    bool (*get_double_arg)(TabloExtCallContext* ctx, int index, double* out_value);
    bool (*get_string_arg)(TabloExtCallContext* ctx, int index, const char** out_chars, int* out_length);
    bool (*get_bytes_arg)(TabloExtCallContext* ctx, int index, const uint8_t** out_bytes, int* out_length);
    bool (*get_handle_arg)(TabloExtCallContext* ctx, int index, const char* expected_type_name, void** out_payload);
    bool (*get_array_arg)(TabloExtCallContext* ctx, int index, const TabloExtValue** out_items, int* out_count);
    bool (*get_tuple_arg)(TabloExtCallContext* ctx, int index, const TabloExtValue** out_items, int* out_count);
    bool (*get_map_arg)(TabloExtCallContext* ctx, int index, const TabloExtMapEntry** out_entries, int* out_count);
    bool (*get_callback_arg)(TabloExtCallContext* ctx, int index, const TabloExtCallback** out_callback);
    bool (*set_nil_result)(TabloExtCallContext* ctx);
    bool (*set_int_result)(TabloExtCallContext* ctx, int64_t value);
    bool (*set_bool_result)(TabloExtCallContext* ctx, bool value);
    bool (*set_double_result)(TabloExtCallContext* ctx, double value);
    bool (*set_string_result)(TabloExtCallContext* ctx, const char* chars, int length);
    bool (*set_bytes_result)(TabloExtCallContext* ctx, const uint8_t* bytes, int length);
    bool (*set_handle_result)(TabloExtCallContext* ctx, const char* type_name, void* payload);
    bool (*set_array_result)(TabloExtCallContext* ctx, const TabloExtValue* items, int count);
    bool (*set_tuple_result)(TabloExtCallContext* ctx, const TabloExtValue* items, int count);
    bool (*set_map_result)(TabloExtCallContext* ctx, const TabloExtMapEntry* entries, int count);
    bool (*invoke_callback)(TabloExtCallContext* ctx,
                            const TabloExtCallback* callback,
                            const TabloExtValue* args,
                            int arg_count,
                            TabloExtValue* out_result);
    void (*set_runtime_error)(TabloExtCallContext* ctx, const char* message);
} TabloExtApi;

typedef bool (*TabloExtInitFn)(const TabloExtRegistrar* registrar, char* error_buf, size_t error_buf_size);
typedef void (*TabloExtShutdownFn)(void);

#ifdef __cplusplus
}
#endif

#endif
