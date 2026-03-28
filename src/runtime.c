#include "runtime.h"
#include "artifact.h"
#include "builtins.h"
#include "native_extension.h"
#include "path_utils.h"
#include "safe_alloc.h"
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifdef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

// Bump when cache key semantics must change (for example compiler/bytecode behavior updates).
static const char* RUNTIME_CACHE_ABI_TAG = "tablo-cache-abi-2026-03-20-native-extension-v1";

typedef struct {
    bool has_override;
    char* source;
    int64_t priority;
    char** phases;
    int phase_count;
} RuntimePostedEventMetaState;

typedef struct {
    RuntimePostedEventExtraField* fields;
    int field_count;
} RuntimePostedEventExtraState;

typedef struct {
    RuntimePostedEventKind kind;
    char* callback_name;
    char* text_value;
    int64_t int_value_a;
    int64_t int_value_b;
    bool bool_value;
    double double_value;
    RuntimePostedEventMetaState meta;
    RuntimePostedEventExtraState extra;
} RuntimePostedEventPayload;

typedef struct {
    RuntimePostedEventKind kind;
    char* text_value;
    int64_t int_value_a;
    int64_t int_value_b;
    bool bool_value;
    double double_value;
    RuntimePostedEventMetaState meta;
    RuntimePostedEventExtraState extra;
} RuntimePostedEventBatchItem;

typedef struct {
    char* callback_name;
    RuntimePostedEventBatchItem* items;
    int item_count;
} RuntimePostedEventBatchPayload;

static void runtime_set_error_buffer(char* error_buf, size_t error_buf_size, const char* fmt, ...) {
    va_list args;
    if (!error_buf || error_buf_size == 0) return;
    va_start(args, fmt);
    vsnprintf(error_buf, error_buf_size, fmt, args);
    va_end(args);
    error_buf[error_buf_size - 1] = '\0';
}

static char* runtime_strdup_fallible(const char* text) {
    size_t len = 0;
    char* copy = NULL;
    if (!text) return NULL;
    len = strlen(text);
    copy = (char*)malloc(len + 1u);
    if (!copy) return NULL;
    memcpy(copy, text, len + 1u);
    return copy;
}

static void runtime_posted_event_meta_state_free(RuntimePostedEventMetaState* meta) {
    if (!meta) return;
    free(meta->source);
    if (meta->phases) {
        for (int i = 0; i < meta->phase_count; i++) {
            free(meta->phases[i]);
        }
        free(meta->phases);
    }
    memset(meta, 0, sizeof(*meta));
}

static bool runtime_clone_posted_event_meta(RuntimePostedEventMetaState* dst,
                                            const RuntimePostedEventMetaOverride* src,
                                            char* error_buf,
                                            size_t error_buf_size) {
    if (!dst) return false;
    memset(dst, 0, sizeof(*dst));
    if (!src) {
        return true;
    }
    if (!src->source || src->source[0] == '\0') {
        runtime_set_error_buffer(error_buf, error_buf_size, "Event meta source is required");
        return false;
    }
    if (src->phase_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Event meta phase count is invalid");
        return false;
    }
    if (src->phase_count > 0 && !src->phases) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Event meta phases are required");
        return false;
    }

    dst->source = runtime_strdup_fallible(src->source);
    if (!dst->source) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while copying event meta source");
        runtime_posted_event_meta_state_free(dst);
        return false;
    }
    dst->priority = src->priority;
    dst->phase_count = src->phase_count;
    dst->has_override = true;

    if (src->phase_count > 0) {
        dst->phases = (char**)calloc((size_t)src->phase_count, sizeof(char*));
        if (!dst->phases) {
            runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while allocating event meta phases");
            runtime_posted_event_meta_state_free(dst);
            return false;
        }
        for (int i = 0; i < src->phase_count; i++) {
            const char* phase = src->phases[i] ? src->phases[i] : "";
            dst->phases[i] = runtime_strdup_fallible(phase);
            if (!dst->phases[i]) {
                runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while copying event meta phase");
                runtime_posted_event_meta_state_free(dst);
                return false;
            }
        }
    }

    return true;
}

static bool runtime_clone_posted_event_meta_state(RuntimePostedEventMetaState* dst,
                                                  const RuntimePostedEventMetaState* src,
                                                  char* error_buf,
                                                  size_t error_buf_size) {
    RuntimePostedEventMetaOverride override_meta;
    if (!src || !src->has_override) {
        memset(dst, 0, sizeof(*dst));
        return true;
    }
    override_meta.source = src->source;
    override_meta.priority = src->priority;
    override_meta.phases = (const char* const*)src->phases;
    override_meta.phase_count = src->phase_count;
    return runtime_clone_posted_event_meta(dst, &override_meta, error_buf, error_buf_size);
}

static bool runtime_posted_event_extra_field_name_is_reserved(RuntimePostedEventKind kind, const char* name) {
    if (!name || name[0] == '\0') return false;
    if (strcmp(name, "kind") == 0 || strcmp(name, "meta") == 0) {
        return true;
    }
    switch (kind) {
        case RUNTIME_POSTED_EVENT_INPUT:
            return strcmp(name, "device") == 0 ||
                   strcmp(name, "code") == 0 ||
                   strcmp(name, "pressed") == 0;
        case RUNTIME_POSTED_EVENT_WINDOW:
            return strcmp(name, "event") == 0 ||
                   strcmp(name, "width") == 0 ||
                   strcmp(name, "height") == 0 ||
                   strcmp(name, "focused") == 0;
        case RUNTIME_POSTED_EVENT_FRAME:
            return strcmp(name, "phase") == 0 ||
                   strcmp(name, "frame") == 0 ||
                   strcmp(name, "dt") == 0;
        default:
            return false;
    }
}

static void runtime_posted_event_extra_value_free(RuntimePostedEventExtraValue* value) {
    if (!value) return;
    if (value->kind == RUNTIME_POSTED_EVENT_FIELD_STRING) {
        free((void*)value->string_value);
    } else if ((value->kind == RUNTIME_POSTED_EVENT_FIELD_ARRAY ||
                value->kind == RUNTIME_POSTED_EVENT_FIELD_TUPLE) &&
               value->items) {
        RuntimePostedEventExtraValue* items = (RuntimePostedEventExtraValue*)value->items;
        for (int i = 0; i < value->item_count; i++) {
            runtime_posted_event_extra_value_free(&items[i]);
        }
        free(items);
    } else if (value->kind == RUNTIME_POSTED_EVENT_FIELD_MAP && value->map_entries) {
        RuntimePostedEventExtraMapEntry* entries = (RuntimePostedEventExtraMapEntry*)value->map_entries;
        for (int i = 0; i < value->map_entry_count; i++) {
            free((void*)entries[i].key);
            runtime_posted_event_extra_value_free(&entries[i].value);
        }
        free(entries);
    }
    memset(value, 0, sizeof(*value));
}

static void runtime_posted_event_extra_state_free(RuntimePostedEventExtraState* extra) {
    if (!extra) return;
    if (extra->fields) {
        for (int i = 0; i < extra->field_count; i++) {
            free((void*)extra->fields[i].name);
            runtime_posted_event_extra_value_free(&extra->fields[i].value);
        }
        free(extra->fields);
    }
    memset(extra, 0, sizeof(*extra));
}

static bool runtime_clone_posted_event_extra_value(RuntimePostedEventExtraValue* dst,
                                                   const RuntimePostedEventExtraValue* src,
                                                   bool allow_container,
                                                   bool allow_map,
                                                   char* error_buf,
                                                   size_t error_buf_size) {
    if (!dst || !src) return false;
    memset(dst, 0, sizeof(*dst));
    dst->kind = src->kind;
    switch (src->kind) {
        case RUNTIME_POSTED_EVENT_FIELD_INT:
            dst->int_value = src->int_value;
            return true;
        case RUNTIME_POSTED_EVENT_FIELD_BOOL:
            dst->bool_value = src->bool_value;
            return true;
        case RUNTIME_POSTED_EVENT_FIELD_DOUBLE:
            dst->double_value = src->double_value;
            return true;
        case RUNTIME_POSTED_EVENT_FIELD_STRING:
            dst->string_value = runtime_strdup_fallible(src->string_value ? src->string_value : "");
            if (!dst->string_value) {
                runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while copying event extra field string value");
                return false;
            }
            return true;
        case RUNTIME_POSTED_EVENT_FIELD_ARRAY:
        case RUNTIME_POSTED_EVENT_FIELD_TUPLE: {
            RuntimePostedEventExtraValue* items = NULL;
            if (!allow_container) {
                runtime_set_error_buffer(error_buf, error_buf_size, "Nested event extra array/tuple values are not supported");
                return false;
            }
            if (src->item_count < 0) {
                runtime_set_error_buffer(error_buf, error_buf_size, "Event extra array/tuple item count is invalid");
                return false;
            }
            if (src->item_count > 0 && !src->items) {
                runtime_set_error_buffer(error_buf, error_buf_size, "Event extra array/tuple items are required");
                return false;
            }
            dst->item_count = src->item_count;
            if (src->item_count == 0) {
                return true;
            }
            items = (RuntimePostedEventExtraValue*)calloc((size_t)src->item_count, sizeof(RuntimePostedEventExtraValue));
            if (!items) {
                runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while allocating event extra array/tuple items");
                return false;
            }
            dst->items = items;
            for (int i = 0; i < src->item_count; i++) {
                if (!runtime_clone_posted_event_extra_value(&items[i],
                                                            &src->items[i],
                                                            false,
                                                            false,
                                                            error_buf,
                                                            error_buf_size)) {
                    runtime_posted_event_extra_value_free(dst);
                    return false;
                }
            }
            return true;
        }
        case RUNTIME_POSTED_EVENT_FIELD_MAP: {
            RuntimePostedEventExtraMapEntry* entries = NULL;
            if (!allow_map) {
                runtime_set_error_buffer(error_buf, error_buf_size, "Nested event extra map values are not supported");
                return false;
            }
            if (src->map_entry_count < 0) {
                runtime_set_error_buffer(error_buf, error_buf_size, "Event extra map entry count is invalid");
                return false;
            }
            if (src->map_entry_count > 0 && !src->map_entries) {
                runtime_set_error_buffer(error_buf, error_buf_size, "Event extra map entries are required");
                return false;
            }
            dst->map_entry_count = src->map_entry_count;
            if (src->map_entry_count == 0) {
                return true;
            }
            entries = (RuntimePostedEventExtraMapEntry*)calloc((size_t)src->map_entry_count, sizeof(RuntimePostedEventExtraMapEntry));
            if (!entries) {
                runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while allocating event extra map entries");
                return false;
            }
            dst->map_entries = entries;
            for (int i = 0; i < src->map_entry_count; i++) {
                const char* key = src->map_entries[i].key;
                if (!key || key[0] == '\0') {
                    runtime_set_error_buffer(error_buf, error_buf_size, "Event extra map entry key is required");
                    runtime_posted_event_extra_value_free(dst);
                    return false;
                }
                for (int j = 0; j < i; j++) {
                    if (strcmp(entries[j].key, key) == 0) {
                        runtime_set_error_buffer(error_buf, error_buf_size, "Event extra map entry '%s' is duplicated", key);
                        runtime_posted_event_extra_value_free(dst);
                        return false;
                    }
                }
                entries[i].key = runtime_strdup_fallible(key);
                if (!entries[i].key) {
                    runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while copying event extra map entry key");
                    runtime_posted_event_extra_value_free(dst);
                    return false;
                }
                if (!runtime_clone_posted_event_extra_value(&entries[i].value,
                                                            &src->map_entries[i].value,
                                                            true,
                                                            false,
                                                            error_buf,
                                                            error_buf_size)) {
                    runtime_posted_event_extra_value_free(dst);
                    return false;
                }
            }
            return true;
        }
        default:
            runtime_set_error_buffer(error_buf, error_buf_size, "Event extra field has an unsupported type");
            return false;
    }
}

static bool runtime_clone_posted_event_extra_state(RuntimePostedEventExtraState* dst,
                                                   RuntimePostedEventKind kind,
                                                   const RuntimePostedEventExtraField* src_fields,
                                                   int field_count,
                                                   char* error_buf,
                                                   size_t error_buf_size) {
    if (!dst) return false;
    memset(dst, 0, sizeof(*dst));
    if (field_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Event extra field count is invalid");
        return false;
    }
    if (field_count == 0) {
        return true;
    }
    if (!src_fields) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Event extra fields are required");
        return false;
    }

    dst->fields = (RuntimePostedEventExtraField*)calloc((size_t)field_count, sizeof(RuntimePostedEventExtraField));
    if (!dst->fields) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while allocating event extra fields");
        return false;
    }
    dst->field_count = field_count;

    for (int i = 0; i < field_count; i++) {
        const char* name = src_fields[i].name;
        if (!name || name[0] == '\0') {
            runtime_set_error_buffer(error_buf, error_buf_size, "Event extra field name is required");
            runtime_posted_event_extra_state_free(dst);
            return false;
        }
        if (runtime_posted_event_extra_field_name_is_reserved(kind, name)) {
            runtime_set_error_buffer(error_buf,
                                     error_buf_size,
                                     "Event extra field '%s' conflicts with a built-in event field",
                                     name);
            runtime_posted_event_extra_state_free(dst);
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(dst->fields[j].name, name) == 0) {
                runtime_set_error_buffer(error_buf,
                                         error_buf_size,
                                         "Event extra field '%s' is duplicated",
                                         name);
                runtime_posted_event_extra_state_free(dst);
                return false;
            }
        }

        dst->fields[i].name = runtime_strdup_fallible(name);
        if (!dst->fields[i].name) {
            runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while copying event extra field name");
            runtime_posted_event_extra_state_free(dst);
            return false;
        }
        if (!runtime_clone_posted_event_extra_value(&dst->fields[i].value,
                                                    &src_fields[i].value,
                                                    true,
                                                    true,
                                                    error_buf,
                                                    error_buf_size)) {
            if (!error_buf || error_buf[0] == '\0') {
                runtime_set_error_buffer(error_buf, error_buf_size, "Event extra field '%s' has an unsupported type", name);
            }
            runtime_posted_event_extra_state_free(dst);
            return false;
        }
    }

    return true;
}

static bool runtime_clone_posted_event_extra_state_from_state(RuntimePostedEventExtraState* dst,
                                                              RuntimePostedEventKind kind,
                                                              const RuntimePostedEventExtraState* src,
                                                              char* error_buf,
                                                              size_t error_buf_size) {
    if (!src || src->field_count == 0) {
        memset(dst, 0, sizeof(*dst));
        return true;
    }
    return runtime_clone_posted_event_extra_state(dst,
                                                  kind,
                                                  src->fields,
                                                  src->field_count,
                                                  error_buf,
                                                  error_buf_size);
}

static void runtime_posted_event_payload_free(void* payload) {
    RuntimePostedEventPayload* event = (RuntimePostedEventPayload*)payload;
    if (!event) return;
    free(event->callback_name);
    free(event->text_value);
    runtime_posted_event_meta_state_free(&event->meta);
    runtime_posted_event_extra_state_free(&event->extra);
    free(event);
}

static void runtime_posted_event_batch_payload_free(void* payload) {
    RuntimePostedEventBatchPayload* batch = (RuntimePostedEventBatchPayload*)payload;
    if (!batch) return;
    free(batch->callback_name);
    if (batch->items) {
        for (int i = 0; i < batch->item_count; i++) {
            free(batch->items[i].text_value);
            runtime_posted_event_meta_state_free(&batch->items[i].meta);
            runtime_posted_event_extra_state_free(&batch->items[i].extra);
        }
        free(batch->items);
    }
    free(batch);
}

static bool runtime_build_string_array_value(VM* vm,
                                             const char* const* items,
                                             int item_count,
                                             Value* out_value) {
    ObjArray* array = NULL;
    if (!vm || !out_value || item_count < 0) return false;

    array = obj_array_create(vm, item_count > 0 ? item_count : 1);
    value_init_array(out_value, array);
    for (int i = 0; i < item_count; i++) {
        Value item_value;
        const char* item_chars = items && items[i] ? items[i] : "";
        value_init_string_n(&item_value, item_chars, (int)strlen(item_chars));
        obj_array_push(array, item_value);
    }
    return !vm->error_occurred;
}

static bool runtime_map_set_string_field(ObjMap* map, const char* key, const char* chars) {
    Value value;
    if (!map || !key) return false;
    value_init_string_n(&value, chars ? chars : "", (int)strlen(chars ? chars : ""));
    obj_map_set_cstr(map, key, value);
    value_free(&value);
    return true;
}

static bool runtime_map_set_int_field(ObjMap* map, const char* key, int64_t int_value) {
    Value value;
    if (!map || !key) return false;
    value_init_int(&value, int_value);
    obj_map_set_cstr(map, key, value);
    return true;
}

static bool runtime_map_set_bool_field(ObjMap* map, const char* key, bool bool_value) {
    Value value;
    if (!map || !key) return false;
    value_init_bool(&value, bool_value);
    obj_map_set_cstr(map, key, value);
    return true;
}

static bool runtime_map_set_double_field(ObjMap* map, const char* key, double double_value) {
    Value value;
    if (!map || !key) return false;
    value_init_double(&value, double_value);
    obj_map_set_cstr(map, key, value);
    return true;
}

static bool runtime_build_posted_event_extra_value(VM* vm,
                                                   const RuntimePostedEventExtraValue* extra_value,
                                                   Value* out_value) {
    if (!vm || !extra_value || !out_value) return false;
    value_init_nil(out_value);
    switch (extra_value->kind) {
        case RUNTIME_POSTED_EVENT_FIELD_INT:
            value_init_int(out_value, extra_value->int_value);
            return true;
        case RUNTIME_POSTED_EVENT_FIELD_BOOL:
            value_init_bool(out_value, extra_value->bool_value);
            return true;
        case RUNTIME_POSTED_EVENT_FIELD_DOUBLE:
            value_init_double(out_value, extra_value->double_value);
            return true;
        case RUNTIME_POSTED_EVENT_FIELD_STRING:
            value_init_string_n(out_value,
                                extra_value->string_value ? extra_value->string_value : "",
                                (int)strlen(extra_value->string_value ? extra_value->string_value : ""));
            return !vm->error_occurred;
        case RUNTIME_POSTED_EVENT_FIELD_ARRAY: {
            ObjArray* array = obj_array_create(vm, extra_value->item_count > 0 ? extra_value->item_count : 1);
            value_init_array(out_value, array);
            for (int i = 0; i < extra_value->item_count; i++) {
                Value item_value;
                if (!runtime_build_posted_event_extra_value(vm, &extra_value->items[i], &item_value)) {
                    value_free(out_value);
                    value_init_nil(out_value);
                    return false;
                }
                obj_array_push(array, item_value);
            }
            return !vm->error_occurred;
        }
        case RUNTIME_POSTED_EVENT_FIELD_TUPLE: {
            ObjTuple* tuple = obj_tuple_create(vm, extra_value->item_count);
            value_init_tuple(out_value, tuple);
            for (int i = 0; i < extra_value->item_count; i++) {
                Value item_value;
                if (!runtime_build_posted_event_extra_value(vm, &extra_value->items[i], &item_value)) {
                    value_free(out_value);
                    value_init_nil(out_value);
                    return false;
                }
                obj_tuple_set(tuple, i, item_value);
            }
            return !vm->error_occurred;
        }
        case RUNTIME_POSTED_EVENT_FIELD_MAP: {
            ObjMap* map = obj_map_create(vm);
            value_init_map(out_value, map);
            for (int i = 0; i < extra_value->map_entry_count; i++) {
                Value entry_value;
                if (!runtime_build_posted_event_extra_value(vm, &extra_value->map_entries[i].value, &entry_value)) {
                    value_free(out_value);
                    value_init_nil(out_value);
                    return false;
                }
                obj_map_set_cstr(map, extra_value->map_entries[i].key, entry_value);
                value_free(&entry_value);
            }
            return !vm->error_occurred;
        }
        default:
            vm_runtime_error(vm, "Unsupported host-posted event extra field type");
            return false;
    }
}

static bool runtime_map_set_extra_field(VM* vm, ObjMap* map, const RuntimePostedEventExtraField* field) {
    Value value;
    if (!vm || !map || !field || !field->name) return false;
    if (!runtime_build_posted_event_extra_value(vm, &field->value, &value)) {
        return false;
    }
    obj_map_set_cstr(map, field->name, value);
    value_free(&value);
    return !vm->error_occurred;
}

static bool runtime_map_set_extra_fields(VM* vm, ObjMap* map, const RuntimePostedEventExtraState* extra) {
    if (!vm || !map) return false;
    if (!extra || extra->field_count == 0) {
        return true;
    }
    for (int i = 0; i < extra->field_count; i++) {
        if (!runtime_map_set_extra_field(vm, map, &extra->fields[i])) {
            return false;
        }
    }
    return true;
}

static bool runtime_build_event_meta_value(VM* vm,
                                           const RuntimePostedEventMetaState* override_meta,
                                           const char* source,
                                           int64_t priority,
                                           const char* const* phases,
                                           int phase_count,
                                           Value* out_value) {
    ObjMap* meta_map = NULL;
    Value phases_value;
    Value meta_value;

    const char* effective_source = source;
    int64_t effective_priority = priority;
    const char* const* effective_phases = phases;
    int effective_phase_count = phase_count;

    if (!vm || !out_value || !source || phase_count < 0) return false;
    if (override_meta && override_meta->has_override) {
        effective_source = override_meta->source ? override_meta->source : source;
        effective_priority = override_meta->priority;
        effective_phases = (const char* const*)override_meta->phases;
        effective_phase_count = override_meta->phase_count;
    }

    value_init_nil(&phases_value);
    value_init_nil(&meta_value);
    if (!runtime_build_string_array_value(vm, effective_phases, effective_phase_count, &phases_value)) {
        return false;
    }

    meta_map = obj_map_create(vm);
    if (!runtime_map_set_string_field(meta_map, "source", effective_source) ||
        !runtime_map_set_int_field(meta_map, "priority", effective_priority)) {
        value_free(&phases_value);
        return false;
    }
    obj_map_set_cstr(meta_map, "phases", phases_value);
    value_free(&phases_value);

    value_init_map(&meta_value, meta_map);
    *out_value = meta_value;
    return !vm->error_occurred;
}

static bool runtime_build_input_event_value(VM* vm,
                                            const RuntimePostedEventPayload* event,
                                            Value* out_value) {
    static const char* phases[] = { "queue", "dispatch" };
    ObjMap* event_map = NULL;
    Value meta_value;
    Value event_value;

    if (!vm || !event || !out_value) return false;
    value_init_nil(&meta_value);
    value_init_nil(&event_value);
    if (!runtime_build_event_meta_value(vm, &event->meta, "input", 1, phases, 2, &meta_value)) {
        return false;
    }

    event_map = obj_map_create(vm);
    if (!runtime_map_set_string_field(event_map, "kind", "input") ||
        !runtime_map_set_string_field(event_map, "device", event->text_value) ||
        !runtime_map_set_int_field(event_map, "code", event->int_value_a) ||
        !runtime_map_set_bool_field(event_map, "pressed", event->bool_value)) {
        value_free(&meta_value);
        return false;
    }
    obj_map_set_cstr(event_map, "meta", meta_value);
    value_free(&meta_value);
    if (!runtime_map_set_extra_fields(vm, event_map, &event->extra)) {
        return false;
    }

    value_init_map(&event_value, event_map);
    *out_value = event_value;
    return !vm->error_occurred;
}

static bool runtime_build_window_event_value(VM* vm,
                                             const RuntimePostedEventPayload* event,
                                             Value* out_value) {
    static const char* phases[] = { "layout", "present" };
    ObjMap* event_map = NULL;
    Value meta_value;
    Value event_value;

    if (!vm || !event || !out_value) return false;
    value_init_nil(&meta_value);
    value_init_nil(&event_value);
    if (!runtime_build_event_meta_value(vm, &event->meta, "window", 4, phases, 2, &meta_value)) {
        return false;
    }

    event_map = obj_map_create(vm);
    if (!runtime_map_set_string_field(event_map, "kind", "window") ||
        !runtime_map_set_string_field(event_map, "event", event->text_value) ||
        !runtime_map_set_int_field(event_map, "width", event->int_value_a) ||
        !runtime_map_set_int_field(event_map, "height", event->int_value_b) ||
        !runtime_map_set_bool_field(event_map, "focused", event->bool_value)) {
        value_free(&meta_value);
        return false;
    }
    obj_map_set_cstr(event_map, "meta", meta_value);
    value_free(&meta_value);
    if (!runtime_map_set_extra_fields(vm, event_map, &event->extra)) {
        return false;
    }

    value_init_map(&event_value, event_map);
    *out_value = event_value;
    return !vm->error_occurred;
}

static bool runtime_build_frame_event_value(VM* vm,
                                            const RuntimePostedEventPayload* event,
                                            Value* out_value) {
    static const char* phases[] = { "update", "render" };
    ObjMap* event_map = NULL;
    Value meta_value;
    Value event_value;

    if (!vm || !event || !out_value) return false;
    value_init_nil(&meta_value);
    value_init_nil(&event_value);
    if (!runtime_build_event_meta_value(vm, &event->meta, "frame", 5, phases, 2, &meta_value)) {
        return false;
    }

    event_map = obj_map_create(vm);
    if (!runtime_map_set_string_field(event_map, "kind", "frame") ||
        !runtime_map_set_string_field(event_map, "phase", event->text_value) ||
        !runtime_map_set_int_field(event_map, "frame", event->int_value_a) ||
        !runtime_map_set_double_field(event_map, "dt", event->double_value)) {
        value_free(&meta_value);
        return false;
    }
    obj_map_set_cstr(event_map, "meta", meta_value);
    value_free(&meta_value);
    if (!runtime_map_set_extra_fields(vm, event_map, &event->extra)) {
        return false;
    }

    value_init_map(&event_value, event_map);
    *out_value = event_value;
    return !vm->error_occurred;
}

static bool runtime_build_posted_event_value(VM* vm,
                                             const RuntimePostedEventPayload* event,
                                             Value* out_value) {
    if (!vm || !event || !out_value) return false;
    switch (event->kind) {
        case RUNTIME_POSTED_EVENT_INPUT:
            return runtime_build_input_event_value(vm, event, out_value);
        case RUNTIME_POSTED_EVENT_WINDOW:
            return runtime_build_window_event_value(vm, event, out_value);
        case RUNTIME_POSTED_EVENT_FRAME:
            return runtime_build_frame_event_value(vm, event, out_value);
        default:
            vm_runtime_error(vm, "Unsupported host-posted event kind");
            return false;
    }
}

static bool runtime_dispatch_posted_event(VM* vm, void* payload) {
    RuntimePostedEventPayload* event = (RuntimePostedEventPayload*)payload;
    Value callee;
    Value arg_value;
    Value result_value;
    char error_message[256];

    if (!vm || !event || !event->callback_name) return false;

    callee = vm_get_global(vm, event->callback_name);
    if (value_get_type(&callee) != VAL_FUNCTION && value_get_type(&callee) != VAL_NATIVE) {
        snprintf(error_message,
                 sizeof(error_message),
                 "Host-posted callback '%s' is not callable",
                 event->callback_name);
        vm_runtime_error(vm, error_message);
        return false;
    }

    value_init_nil(&arg_value);
    value_init_nil(&result_value);
    if (!runtime_build_posted_event_value(vm, event, &arg_value)) {
        value_free(&arg_value);
        return false;
    }

    if (vm_call_value_sync(vm, &callee, &arg_value, 1, &result_value) != 0) {
        value_free(&arg_value);
        return false;
    }

    value_free(&arg_value);
    value_free(&result_value);
    return !vm->error_occurred;
}

static bool runtime_build_posted_event_batch_value(VM* vm,
                                                   const RuntimePostedEventBatchPayload* batch,
                                                   Value* out_value) {
    ObjArray* array = NULL;
    if (!vm || !batch || !out_value || batch->item_count < 0) return false;

    array = obj_array_create(vm, batch->item_count > 0 ? batch->item_count : 1);
    value_init_array(out_value, array);
    for (int i = 0; i < batch->item_count; i++) {
        RuntimePostedEventPayload item_event;
        Value item_value;

        memset(&item_event, 0, sizeof(item_event));
        value_init_nil(&item_value);
        item_event.kind = batch->items[i].kind;
        item_event.text_value = batch->items[i].text_value;
        item_event.int_value_a = batch->items[i].int_value_a;
        item_event.int_value_b = batch->items[i].int_value_b;
        item_event.bool_value = batch->items[i].bool_value;
        item_event.double_value = batch->items[i].double_value;
        item_event.meta = batch->items[i].meta;
        item_event.extra = batch->items[i].extra;

        if (!runtime_build_posted_event_value(vm, &item_event, &item_value)) {
            value_free(out_value);
            value_init_nil(out_value);
            return false;
        }
        obj_array_push(array, item_value);
    }
    return !vm->error_occurred;
}

static bool runtime_dispatch_posted_event_batch(VM* vm, void* payload) {
    RuntimePostedEventBatchPayload* batch = (RuntimePostedEventBatchPayload*)payload;
    Value callee;
    Value arg_value;
    Value result_value;
    char error_message[256];

    if (!vm || !batch || !batch->callback_name) return false;

    callee = vm_get_global(vm, batch->callback_name);
    if (value_get_type(&callee) != VAL_FUNCTION && value_get_type(&callee) != VAL_NATIVE) {
        snprintf(error_message,
                 sizeof(error_message),
                 "Host-posted batch callback '%s' is not callable",
                 batch->callback_name);
        vm_runtime_error(vm, error_message);
        return false;
    }

    value_init_nil(&arg_value);
    value_init_nil(&result_value);
    if (!runtime_build_posted_event_batch_value(vm, batch, &arg_value)) {
        value_free(&arg_value);
        return false;
    }

    if (vm_call_value_sync(vm, &callee, &arg_value, 1, &result_value) != 0) {
        value_free(&arg_value);
        return false;
    }

    value_free(&arg_value);
    value_free(&result_value);
    return !vm->error_occurred;
}

static bool runtime_post_event(Runtime* rt,
                               RuntimePostedEventKind kind,
                               const char* callback_name,
                               const char* text_value,
                               int64_t int_value_a,
                               int64_t int_value_b,
                               bool bool_value,
                               double double_value,
                               const RuntimePostedEventMetaOverride* meta_override,
                               const RuntimePostedEventExtraField* extra_fields,
                               int extra_field_count,
                               char* error_buf,
                               size_t error_buf_size) {
    VmPostedEventQueue* queue = NULL;
    RuntimePostedEventPayload* event = NULL;

    if (error_buf && error_buf_size > 0) {
        error_buf[0] = '\0';
    }
    if (!rt || !rt->vm) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Runtime is not initialized");
        return false;
    }
    if (!callback_name || callback_name[0] == '\0') {
        runtime_set_error_buffer(error_buf, error_buf_size, "Callback name is required");
        return false;
    }
    if (!text_value) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Event text payload is required");
        return false;
    }

    queue = vm_get_posted_event_queue(rt->vm);
    if (!queue) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Runtime posted event queue is unavailable");
        return false;
    }

    event = (RuntimePostedEventPayload*)calloc(1, sizeof(RuntimePostedEventPayload));
    if (!event) {
        vm_posted_event_queue_release(queue);
        runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while allocating host-posted event");
        return false;
    }

    event->kind = kind;
    event->callback_name = runtime_strdup_fallible(callback_name);
    event->text_value = runtime_strdup_fallible(text_value);
    event->int_value_a = int_value_a;
    event->int_value_b = int_value_b;
    event->bool_value = bool_value;
    event->double_value = double_value;
    if (!event->callback_name || !event->text_value ||
        !runtime_clone_posted_event_meta(&event->meta, meta_override, error_buf, error_buf_size) ||
        !runtime_clone_posted_event_extra_state(&event->extra,
                                                kind,
                                                extra_fields,
                                                extra_field_count,
                                                error_buf,
                                                error_buf_size)) {
        runtime_posted_event_payload_free(event);
        vm_posted_event_queue_release(queue);
        if (!error_buf || error_buf[0] == '\0') {
            runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while copying host-posted event payload");
        }
        return false;
    }

    if (!vm_posted_event_queue_enqueue(queue,
                                       runtime_dispatch_posted_event,
                                       runtime_posted_event_payload_free,
                                       event)) {
        runtime_posted_event_payload_free(event);
        vm_posted_event_queue_release(queue);
        runtime_set_error_buffer(error_buf, error_buf_size, "Runtime posted event queue is closed");
        return false;
    }

    vm_posted_event_queue_release(queue);
    return true;
}

static bool runtime_post_event_batch(Runtime* rt,
                                     const char* callback_name,
                                     const RuntimePostedEventBatchItem* items,
                                     int item_count,
                                     char* error_buf,
                                     size_t error_buf_size) {
    VmPostedEventQueue* queue = NULL;
    RuntimePostedEventBatchPayload* batch = NULL;

    if (error_buf && error_buf_size > 0) {
        error_buf[0] = '\0';
    }
    if (!rt || !rt->vm) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Runtime is not initialized");
        return false;
    }
    if (!callback_name || callback_name[0] == '\0') {
        runtime_set_error_buffer(error_buf, error_buf_size, "Callback name is required");
        return false;
    }
    if (item_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Event batch count is invalid");
        return false;
    }
    if (item_count > 0 && !items) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Event batch items are required");
        return false;
    }

    queue = vm_get_posted_event_queue(rt->vm);
    if (!queue) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Runtime posted event queue is unavailable");
        return false;
    }

    batch = (RuntimePostedEventBatchPayload*)calloc(1, sizeof(RuntimePostedEventBatchPayload));
    if (!batch) {
        vm_posted_event_queue_release(queue);
        runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while allocating host-posted event batch");
        return false;
    }

    batch->item_count = item_count;
    batch->callback_name = runtime_strdup_fallible(callback_name);
    if (!batch->callback_name) {
        runtime_posted_event_batch_payload_free(batch);
        vm_posted_event_queue_release(queue);
        runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while copying host-posted batch callback name");
        return false;
    }

    if (item_count > 0) {
        batch->items = (RuntimePostedEventBatchItem*)calloc((size_t)item_count, sizeof(RuntimePostedEventBatchItem));
        if (!batch->items) {
            runtime_posted_event_batch_payload_free(batch);
            vm_posted_event_queue_release(queue);
            runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while allocating host-posted batch items");
            return false;
        }
        for (int i = 0; i < item_count; i++) {
            batch->items[i].kind = items[i].kind;
            batch->items[i].text_value = runtime_strdup_fallible(items[i].text_value ? items[i].text_value : "");
            batch->items[i].int_value_a = items[i].int_value_a;
            batch->items[i].int_value_b = items[i].int_value_b;
            batch->items[i].bool_value = items[i].bool_value;
            batch->items[i].double_value = items[i].double_value;
            if (!batch->items[i].text_value ||
                !runtime_clone_posted_event_meta_state(&batch->items[i].meta,
                                                       &items[i].meta,
                                                       error_buf,
                                                       error_buf_size) ||
                !runtime_clone_posted_event_extra_state_from_state(&batch->items[i].extra,
                                                                   items[i].kind,
                                                                   &items[i].extra,
                                                       error_buf,
                                                       error_buf_size)) {
                runtime_posted_event_batch_payload_free(batch);
                vm_posted_event_queue_release(queue);
                if (!error_buf || error_buf[0] == '\0') {
                    runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while copying host-posted batch payload");
                }
                return false;
            }
        }
    }

    if (!vm_posted_event_queue_enqueue(queue,
                                       runtime_dispatch_posted_event_batch,
                                       runtime_posted_event_batch_payload_free,
                                       batch)) {
        runtime_posted_event_batch_payload_free(batch);
        vm_posted_event_queue_release(queue);
        runtime_set_error_buffer(error_buf, error_buf_size, "Runtime posted event queue is closed");
        return false;
    }

    vm_posted_event_queue_release(queue);
    return true;
}

static void runtime_set_fallback_error(Runtime* rt, const char* message) {
    if (!rt) return;
    const char* text = (message && message[0] != '\0') ? message : "Out of memory";
    rt->fallback_error_set = true;
    snprintf(rt->fallback_error_message, sizeof(rt->fallback_error_message), "%s", text);
}

static bool runtime_extensions_enabled(RuntimeOptions options) {
    return options.extension_paths && options.extension_path_count > 0;
}

static bool runtime_path_is_regular_file(const char* path) {
    if (!path || path[0] == '\0') return false;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return false;
    return (st.st_mode & _S_IFREG) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode);
#endif
}

static bool runtime_dir_has_module_marker(const char* dir_path) {
    if (!dir_path || dir_path[0] == '\0') return false;

    const char* err = NULL;
    char* mod_path = path_sandbox_join_alloc(dir_path, "tablo.mod", &err);
    if (mod_path) {
        bool ok = runtime_path_is_regular_file(mod_path);
        free(mod_path);
        if (ok) return true;
    }

    char* lock_path = path_sandbox_join_alloc(dir_path, "tablo.lock", &err);
    if (lock_path) {
        bool ok = runtime_path_is_regular_file(lock_path);
        free(lock_path);
        if (ok) return true;
    }

    return false;
}

static char* runtime_find_module_root_alloc(const char* entry_file_path) {
    if (!entry_file_path || entry_file_path[0] == '\0') return safe_strdup(".");

    char* cur = path_dirname_alloc(entry_file_path);
    if (!cur) return NULL;

    char* fallback = safe_strdup(cur);
    if (!fallback) {
        free(cur);
        return NULL;
    }

    while (1) {
        if (runtime_dir_has_module_marker(cur)) {
            free(fallback);
            return cur;
        }

        char* parent = path_dirname_alloc(cur);
        if (!parent) break;
        if (strcmp(parent, cur) == 0) {
            free(parent);
            break;
        }
        free(cur);
        cur = parent;
    }

    free(cur);
    return fallback;
}

static bool runtime_is_path_sep(char c) {
    return c == '/' || c == '\\';
}

static char* runtime_normalize_entry_path_alloc(const char* file_path, const char* sandbox_root) {
    if (!file_path) return safe_strdup("");
    if (!sandbox_root || sandbox_root[0] == '\0') return safe_strdup(file_path);

    if (path_is_absolute(file_path)) return safe_strdup(file_path);
#ifdef _WIN32
    if (strchr(file_path, ':') != NULL) return safe_strdup(file_path);
#endif

    if (strcmp(sandbox_root, ".") != 0) return safe_strdup(file_path);

    if (file_path[0] == '.' || runtime_is_path_sep(file_path[0])) return safe_strdup(file_path);

    size_t len = strlen(file_path);
    char* out = (char*)safe_malloc(len + 3);
    out[0] = '.';
    out[1] = '/';
    memcpy(out + 2, file_path, len + 1);
    return out;
}

static uint32_t runtime_typecheck_flags(RuntimeOptions options) {
    uint32_t flags = 0;
    if (options.typecheck.warn_unused_error) flags |= 1u;
    if (options.typecheck.strict_errors) flags |= 2u;
    return flags;
}

static void runtime_apply_capability_options(VM* vm, RuntimeOptions options) {
    if (!vm) return;
    vm_set_file_io_enabled(vm, !options.capabilities.deny_file_io);
    vm_set_network_enabled(vm, !options.capabilities.deny_network);
    vm_set_process_enabled(vm, !options.capabilities.deny_process);
    vm_set_sqlite_enabled(vm, !options.capabilities.deny_sqlite);
    vm_set_threading_enabled(vm, !options.capabilities.deny_threading);
    vm_set_max_open_files(vm, options.max_open_files);
    vm_set_max_open_sockets(vm, options.max_open_sockets);
}

static bool runtime_file_content_hash(const char* path, uint64_t* out_hash) {
    if (!path || !out_hash) return false;

    FILE* f = fopen(path, "rb");
    if (!f) return false;

    uint64_t h = 1469598103934665603ull;
    unsigned char buf[4096];
    while (1) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        for (size_t i = 0; i < n; i++) {
            h ^= (uint64_t)buf[i];
            h *= 1099511628211ull;
        }
        if (n < sizeof(buf)) {
            if (ferror(f)) {
                fclose(f);
                return false;
            }
            break;
        }
    }

    fclose(f);
    *out_hash = h;
    return true;
}

static bool runtime_file_mtime_token(const char* path, int64_t* out_mtime) {
    if (!path || !out_mtime) return false;

    int64_t mtime = 0;
    int64_t size = 0;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return false;
    mtime = (int64_t)st.st_mtime;
    size = (int64_t)st.st_size;
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    mtime = (int64_t)st.st_mtime;
    size = (int64_t)st.st_size;
#endif

    uint64_t hash = 0;
    if (!runtime_file_content_hash(path, &hash)) return false;

    // Use a cheap content signature so cache invalidation is reliable even if
    // filesystem timestamps are coarse.
    uint64_t token = hash;
    token ^= (uint64_t)mtime;
    token *= 1099511628211ull;
    token ^= (uint64_t)size;
    token *= 1099511628211ull;
    *out_mtime = (int64_t)token;
    return true;
}

static uint64_t runtime_cache_hash(const char* file_path, uint32_t typecheck_flags) {
    uint64_t h = 1469598103934665603ull;
    const unsigned char* abi = (const unsigned char*)RUNTIME_CACHE_ABI_TAG;
    while (abi && *abi) {
        h ^= (uint64_t)(*abi++);
        h *= 1099511628211ull;
    }
    const unsigned char* p = (const unsigned char*)file_path;
    while (p && *p) {
        h ^= (uint64_t)(*p++);
        h *= 1099511628211ull;
    }
    for (int i = 0; i < 4; i++) {
        h ^= (uint64_t)((typecheck_flags >> (i * 8)) & 0xffu);
        h *= 1099511628211ull;
    }
    return h;
}

static char* runtime_temp_dir_alloc(void) {
#ifdef _WIN32
    char* tmp = NULL;
    size_t n = 0;
    if (_dupenv_s(&tmp, &n, "TEMP") == 0 && tmp && tmp[0] != '\0') {
        char* out = safe_strdup(tmp);
        free(tmp);
        return out;
    }
    if (tmp) {
        free(tmp);
        tmp = NULL;
    }
    if (_dupenv_s(&tmp, &n, "TMP") == 0 && tmp && tmp[0] != '\0') {
        char* out = safe_strdup(tmp);
        free(tmp);
        return out;
    }
    if (tmp) free(tmp);
    return safe_strdup(".");
#else
    const char* tmp = getenv("TMPDIR");
    if (!tmp || tmp[0] == '\0') tmp = "/tmp";
    return safe_strdup(tmp);
#endif
}

static char* runtime_cache_path_alloc(const char* file_path, uint32_t typecheck_flags) {
    if (!file_path) return NULL;

    char* base = runtime_temp_dir_alloc();
    uint64_t hash = runtime_cache_hash(file_path, typecheck_flags);
    const char* sep = "";
    size_t base_len = strlen(base);
    if (base_len > 0) {
        char tail = base[base_len - 1];
        if (tail != '/' && tail != '\\') {
            sep = "/";
        }
    }

    char filename[64];
#ifdef _WIN32
    snprintf(filename, sizeof(filename), "tablo-%016I64x.tbcc", (unsigned __int64)hash);
#else
    snprintf(filename, sizeof(filename), "tablo-%016llx.tbcc", (unsigned long long)hash);
#endif

    size_t out_len = base_len + strlen(sep) + strlen(filename);
    char* out = (char*)safe_malloc(out_len + 1);
    memcpy(out, base, base_len);
    memcpy(out + base_len, sep, strlen(sep));
    memcpy(out + base_len + strlen(sep), filename, strlen(filename));
    out[out_len] = '\0';
    free(base);
    return out;
}

static void runtime_register_functions(Runtime* rt) {
    if (!rt || !rt->vm || !rt->functions) return;
    for (int i = 0; i < rt->function_count; i++) {
        ObjFunction* func = rt->functions[i];
        if (func && func->name) {
            Value val;
            value_init_function(&val, func);
            vm_set_global(rt->vm, func->name, val);
        }
    }
}

static void runtime_register_interface_impls(Runtime* rt) {
    if (!rt || !rt->vm || !rt->interface_dispatch_entries) return;
    for (int i = 0; i < rt->interface_dispatch_count; i++) {
        InterfaceDispatchEntry* entry = &rt->interface_dispatch_entries[i];
        if (!entry->interface_name || !entry->record_name || !entry->method_name || !entry->function_name) {
            continue;
        }
        if (!vm_register_interface_impl(rt->vm,
                                        entry->interface_name,
                                        entry->record_name,
                                        entry->method_name,
                                        entry->function_name)) {
            vm_runtime_error(rt->vm, "Failed to register interface dispatch mapping");
            return;
        }
    }
}

static bool runtime_map_local_const_summary_kind(JitSummaryOp op,
                                                 bool guarded,
                                                 JitCompiledKind* out_kind) {
    if (!out_kind) return false;
    switch (op) {
        case JIT_SUMMARY_OP_ADD:
        case JIT_SUMMARY_OP_SUB:
        case JIT_SUMMARY_OP_MUL:
        case JIT_SUMMARY_OP_DIV:
        case JIT_SUMMARY_OP_MOD:
        case JIT_SUMMARY_OP_BIT_AND:
        case JIT_SUMMARY_OP_BIT_OR:
        case JIT_SUMMARY_OP_BIT_XOR:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCAL_CONST_GENERIC
                                : JIT_COMPILED_KIND_INT_BINARY_LOCAL_CONST_GENERIC;
            return true;
        default:
            return false;
    }
}

static bool runtime_map_twoarg_int_summary_kind(JitSummaryOp op,
                                                bool guarded,
                                                JitCompiledKind* out_kind) {
    if (!out_kind) return false;
    switch (op) {
        case JIT_SUMMARY_OP_ADD:
        case JIT_SUMMARY_OP_SUB:
        case JIT_SUMMARY_OP_MUL:
        case JIT_SUMMARY_OP_BIT_AND:
        case JIT_SUMMARY_OP_BIT_OR:
        case JIT_SUMMARY_OP_BIT_XOR:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCALS_GENERIC
                                : JIT_COMPILED_KIND_INT_BINARY_LOCALS_GENERIC;
            return true;
        default:
            return false;
    }
}

static bool runtime_map_twoarg_bool_summary_kind(JitSummaryOp op,
                                                 bool guarded,
                                                 JitCompiledKind* out_kind) {
    if (!out_kind) return false;
    switch (op) {
        case JIT_SUMMARY_OP_LT:
        case JIT_SUMMARY_OP_LE:
        case JIT_SUMMARY_OP_EQ:
        case JIT_SUMMARY_OP_NE:
        case JIT_SUMMARY_OP_GT:
        case JIT_SUMMARY_OP_GE:
            *out_kind = guarded ? JIT_COMPILED_KIND_BOOL_COMPARE_GUARDED_LOCALS_GENERIC
                                : JIT_COMPILED_KIND_BOOL_COMPARE_LOCALS_GENERIC;
            return true;
        default:
            return false;
    }
}

static bool runtime_map_local_const_bool_summary_kind(JitSummaryOp op,
                                                      JitCompiledKind* out_kind) {
    if (!out_kind) return false;
    switch (op) {
        case JIT_SUMMARY_OP_LT:
        case JIT_SUMMARY_OP_LE:
        case JIT_SUMMARY_OP_EQ:
        case JIT_SUMMARY_OP_NE:
        case JIT_SUMMARY_OP_GT:
        case JIT_SUMMARY_OP_GE:
            *out_kind = JIT_COMPILED_KIND_BOOL_COMPARE_LOCAL_CONST_GENERIC;
            return true;
        default:
            return false;
    }
}

static bool runtime_map_twoarg_selector_summary_kind(JitSummaryOp op,
                                                     bool guarded,
                                                     JitCompiledKind* out_kind) {
    if (!out_kind) return false;
    switch (op) {
        case JIT_SUMMARY_OP_LT:
        case JIT_SUMMARY_OP_LE:
        case JIT_SUMMARY_OP_GT:
        case JIT_SUMMARY_OP_GE:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCALS_GENERIC
                                : JIT_COMPILED_KIND_INT_SELECTOR_LOCALS_GENERIC;
            return true;
        default:
            return false;
    }
}

static bool runtime_map_local_const_selector_summary_kind(JitSummaryOp op,
                                                          bool guarded,
                                                          bool return_local_when_true,
                                                          JitCompiledKind* out_kind) {
    if (!out_kind) return false;
    switch (op) {
        case JIT_SUMMARY_OP_LT:
        case JIT_SUMMARY_OP_LE:
        case JIT_SUMMARY_OP_GT:
        case JIT_SUMMARY_OP_GE:
            (void)return_local_when_true;
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCAL_CONST_GENERIC
                                : JIT_COMPILED_KIND_INT_SELECTOR_LOCAL_CONST_GENERIC;
            return true;
        default:
            return false;
    }
}

static bool runtime_jit_plan_from_summary(const JitFunctionSummary* summary,
                                          JitCompiledPlan* out_plan) {
    if (!summary || !out_plan || summary->kind == JIT_SUMMARY_KIND_NONE) return false;

    memset(out_plan, 0, sizeof(*out_plan));
    switch (summary->kind) {
        case JIT_SUMMARY_KIND_INT_LOCAL_CONST_BINARY:
            if (!runtime_map_local_const_summary_kind(summary->op, false, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->int_const0 = summary->int_const0;
            return true;
        case JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_BINARY:
            if (!runtime_map_local_const_summary_kind(summary->op, true, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->int_const0 = summary->int_const0;
            out_plan->int_const1 = summary->int_const1;
            return true;
        case JIT_SUMMARY_KIND_INT_TWOARG_BINARY:
            if (!runtime_map_twoarg_int_summary_kind(summary->op, false, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            return true;
        case JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_BINARY:
            if (!runtime_map_twoarg_int_summary_kind(summary->op, true, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            out_plan->int_const0 = summary->int_const0;
            return true;
        case JIT_SUMMARY_KIND_BOOL_TWOARG_COMPARE:
            if (!runtime_map_twoarg_bool_summary_kind(summary->op, false, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            return true;
        case JIT_SUMMARY_KIND_BOOL_LOCAL_CONST_COMPARE:
            if (!runtime_map_local_const_bool_summary_kind(summary->op, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->int_const0 = summary->int_const0;
            return true;
        case JIT_SUMMARY_KIND_BOOL_GUARDED_TWOARG_COMPARE:
            if (!runtime_map_twoarg_bool_summary_kind(summary->op, true, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            out_plan->int_const0 = summary->int_const0;
            out_plan->int_const1 = summary->int_const1;
            return true;
        case JIT_SUMMARY_KIND_INT_TWOARG_SELECTOR:
            if (!runtime_map_twoarg_selector_summary_kind(summary->op, false, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            return true;
        case JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_SELECTOR:
            if (!runtime_map_twoarg_selector_summary_kind(summary->op, true, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            out_plan->int_const0 = summary->int_const0;
            return true;
        case JIT_SUMMARY_KIND_INT_LOCAL_CONST_SELECTOR:
            if (!runtime_map_local_const_selector_summary_kind(summary->op,
                                                               false,
                                                               summary->slot1 != 0,
                                                               &out_plan->kind)) {
                return false;
            }
            out_plan->op = summary->op;
            out_plan->flags = summary->slot1 != 0 ? JIT_PLAN_FLAG_RETURN_LOCAL_ON_TRUE : JIT_PLAN_FLAG_NONE;
            out_plan->local_slot = summary->slot0;
            out_plan->int_const0 = summary->int_const0;
            return true;
        case JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_SELECTOR:
            if (!runtime_map_local_const_selector_summary_kind(summary->op,
                                                               true,
                                                               summary->slot1 != 0,
                                                               &out_plan->kind)) {
                return false;
            }
            out_plan->op = summary->op;
            out_plan->flags = summary->slot1 != 0 ? JIT_PLAN_FLAG_RETURN_LOCAL_ON_TRUE : JIT_PLAN_FLAG_NONE;
            out_plan->local_slot = summary->slot0;
            out_plan->int_const0 = summary->int_const0;
            out_plan->int_const1 = summary->int_const1;
            return true;
        default:
            return false;
    }
}

static bool runtime_jit_plan_uses_second_local_slot(JitCompiledKind kind) {
    switch (kind) {
        case JIT_COMPILED_KIND_INT_ADD_LOCALS:
        case JIT_COMPILED_KIND_INT_SUB_LOCALS:
        case JIT_COMPILED_KIND_INT_MUL_LOCALS:
        case JIT_COMPILED_KIND_INT_BIT_AND_LOCALS:
        case JIT_COMPILED_KIND_INT_BIT_OR_LOCALS:
        case JIT_COMPILED_KIND_INT_BIT_XOR_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_ADD_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SUB_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MUL_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_AND_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_OR_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_XOR_LOCALS:
        case JIT_COMPILED_KIND_BOOL_LT_LOCALS:
        case JIT_COMPILED_KIND_BOOL_LE_LOCALS:
        case JIT_COMPILED_KIND_BOOL_EQ_LOCALS:
        case JIT_COMPILED_KIND_BOOL_NE_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GT_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GE_LOCALS:
        case JIT_COMPILED_KIND_INT_SELECT_LT_LOCALS:
        case JIT_COMPILED_KIND_INT_SELECT_LE_LOCALS:
        case JIT_COMPILED_KIND_INT_SELECT_GT_LOCALS:
        case JIT_COMPILED_KIND_INT_SELECT_GE_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LT_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LE_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GT_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GE_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_LT_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_LE_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_EQ_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_NE_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_GT_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_GE_LOCALS:
        case JIT_COMPILED_KIND_BOOL_COMPARE_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_BOOL_COMPARE_GUARDED_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_INT_SELECTOR_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_INT_BINARY_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCALS_GENERIC:
            return true;
        default:
            return false;
    }
}

static bool runtime_validate_jit_hint_plan(const ObjFunction* func,
                                           const JitCompiledPlan* plan,
                                           const char* label,
                                           const char* subject,
                                           char* error_buf,
                                           size_t error_buf_size) {
    if (!func || !plan) return false;

    JitCompiledKind kind = plan->kind;
    if (kind == JIT_COMPILED_KIND_NONE) return true;
    if (kind < JIT_COMPILED_KIND_NONE ||
        kind > JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCALS_GENERIC) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s has unknown %s kind", label, subject);
        return false;
    }
    if (func->param_count <= 0 || plan->local_slot >= func->param_count) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s has out-of-range %s local slot", label, subject);
        return false;
    }

    if (runtime_jit_plan_uses_second_local_slot(kind)) {
        if (func->param_count <= 1 || plan->local_slot_b >= func->param_count ||
            plan->local_slot_b == plan->local_slot) {
            snprintf(error_buf,
                     error_buf_size,
                     "Invalid artifact: %s has invalid secondary %s local slot",
                     label,
                     subject);
            return false;
        }
    }

    switch (kind) {
        case JIT_COMPILED_KIND_INT_BINARY_LOCAL_CONST_GENERIC:
        case JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCAL_CONST_GENERIC:
        case JIT_COMPILED_KIND_INT_BINARY_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCALS_GENERIC:
            if (plan->op < JIT_SUMMARY_OP_ADD || plan->op > JIT_SUMMARY_OP_BIT_XOR) {
                snprintf(error_buf, error_buf_size, "Invalid artifact: %s has invalid %s op", label, subject);
                return false;
            }
            break;
        case JIT_COMPILED_KIND_BOOL_COMPARE_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_BOOL_COMPARE_GUARDED_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_BOOL_COMPARE_LOCAL_CONST_GENERIC:
        case JIT_COMPILED_KIND_INT_SELECTOR_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_INT_SELECTOR_LOCAL_CONST_GENERIC:
        case JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCAL_CONST_GENERIC:
            if (plan->op < JIT_SUMMARY_OP_LT || plan->op > JIT_SUMMARY_OP_GE) {
                snprintf(error_buf, error_buf_size, "Invalid artifact: %s has invalid %s op", label, subject);
                return false;
            }
            break;
        default:
            if (plan->op != JIT_SUMMARY_OP_NONE) {
                snprintf(error_buf, error_buf_size, "Invalid artifact: %s has unexpected %s op", label, subject);
                return false;
            }
            break;
    }

    switch (kind) {
        case JIT_COMPILED_KIND_INT_SELECTOR_LOCAL_CONST_GENERIC:
        case JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCAL_CONST_GENERIC:
            if ((plan->flags & ~JIT_PLAN_FLAG_RETURN_LOCAL_ON_TRUE) != 0) {
                snprintf(error_buf,
                         error_buf_size,
                         "Invalid artifact: %s has invalid %s selector flags",
                         label,
                         subject);
                return false;
            }
            break;
        default:
            if (plan->flags != JIT_PLAN_FLAG_NONE) {
                snprintf(error_buf, error_buf_size, "Invalid artifact: %s has unexpected %s flags", label, subject);
                return false;
            }
            break;
    }

    switch (kind) {
        case JIT_COMPILED_KIND_INT_DIV_LOCAL_CONST:
            if (plan->int_const0 == 0) {
                snprintf(error_buf, error_buf_size, "Invalid artifact: %s has zero %s divisor", label, subject);
                return false;
            }
            break;
        case JIT_COMPILED_KIND_INT_MOD_LOCAL_CONST:
            if (plan->int_const0 == 0) {
                snprintf(error_buf, error_buf_size, "Invalid artifact: %s has zero %s modulo divisor", label, subject);
                return false;
            }
            break;
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_DIV_CONST:
            if (plan->int_const1 == 0) {
                snprintf(error_buf, error_buf_size, "Invalid artifact: %s has zero guarded %s divisor", label, subject);
                return false;
            }
            break;
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MOD_CONST:
            if (plan->int_const1 == 0) {
                snprintf(error_buf,
                         error_buf_size,
                         "Invalid artifact: %s has zero guarded %s modulo divisor",
                         label,
                         subject);
                return false;
            }
            break;
        case JIT_COMPILED_KIND_INT_BINARY_LOCAL_CONST_GENERIC:
            if ((plan->op == JIT_SUMMARY_OP_DIV || plan->op == JIT_SUMMARY_OP_MOD) && plan->int_const0 == 0) {
                snprintf(error_buf, error_buf_size, "Invalid artifact: %s has zero %s divisor", label, subject);
                return false;
            }
            break;
        case JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCAL_CONST_GENERIC:
            if ((plan->op == JIT_SUMMARY_OP_DIV || plan->op == JIT_SUMMARY_OP_MOD) && plan->int_const1 == 0) {
                snprintf(error_buf, error_buf_size, "Invalid artifact: %s has zero guarded %s divisor", label, subject);
                return false;
            }
            break;
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_LT_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_LE_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_EQ_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_NE_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_GT_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_GE_LOCALS:
            if (plan->int_const1 != 0 && plan->int_const1 != 1) {
                snprintf(error_buf, error_buf_size, "Invalid artifact: %s has invalid guarded bool %s", label, subject);
                return false;
            }
            break;
        default:
            break;
    }

    return true;
}

static bool runtime_validate_jit_profile_semantics(const ObjFunction* func,
                                                   const char* label,
                                                   char* error_buf,
                                                   size_t error_buf_size) {
    if (!func) return false;
    if (func->jit_profile.param_count != func->param_count ||
        func->jit_profile.local_count != func->local_count ||
        func->jit_profile.capture_count != func->capture_count) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s JIT profile counts do not match function signature", label);
        return false;
    }

    uint8_t expected_flags = JIT_PROFILE_FLAG_NONE;
    if (func->is_async) expected_flags |= JIT_PROFILE_FLAG_ASYNC;
    if (func->capture_count != 0) expected_flags |= JIT_PROFILE_FLAG_HAS_CAPTURES;
    if (func->jit_profile.flags != expected_flags) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s JIT profile flags do not match function metadata", label);
        return false;
    }

    uint8_t expected_support = JIT_PROFILE_SUPPORT_NONE;
    if (!func->is_async) {
        expected_support |= JIT_PROFILE_SUPPORT_STUB;
        if (func->capture_count == 0 &&
            func->jit_profile.summary.kind != JIT_SUMMARY_KIND_NONE &&
            func->jit_hint_plan.kind != JIT_COMPILED_KIND_NONE) {
            expected_support |= JIT_PROFILE_SUPPORT_NATIVE_SUMMARY;
        }
    }
    if (func->jit_profile.support_mask != expected_support) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s JIT profile support mask does not match function metadata", label);
        return false;
    }

    uint8_t expected_native_family_mask = JIT_PROFILE_NATIVE_FAMILY_NONE;
    switch (func->jit_profile.summary.kind) {
        case JIT_SUMMARY_KIND_INT_LOCAL_CONST_BINARY:
        case JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_BINARY:
        case JIT_SUMMARY_KIND_INT_TWOARG_BINARY:
        case JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_BINARY:
            expected_native_family_mask = JIT_PROFILE_NATIVE_FAMILY_ARITHMETIC;
            break;
        case JIT_SUMMARY_KIND_BOOL_TWOARG_COMPARE:
        case JIT_SUMMARY_KIND_BOOL_LOCAL_CONST_COMPARE:
        case JIT_SUMMARY_KIND_BOOL_GUARDED_TWOARG_COMPARE:
            expected_native_family_mask = JIT_PROFILE_NATIVE_FAMILY_COMPARE;
            break;
        case JIT_SUMMARY_KIND_INT_TWOARG_SELECTOR:
        case JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_SELECTOR:
        case JIT_SUMMARY_KIND_INT_LOCAL_CONST_SELECTOR:
        case JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_SELECTOR:
            expected_native_family_mask = JIT_PROFILE_NATIVE_FAMILY_SELECTOR;
            break;
        case JIT_SUMMARY_KIND_NONE:
        default:
            expected_native_family_mask = JIT_PROFILE_NATIVE_FAMILY_NONE;
            break;
    }
    if (func->jit_profile.native_family_mask != expected_native_family_mask) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s JIT profile native family mask does not match function metadata", label);
        return false;
    }

    if (func->jit_profile.summary.kind == JIT_SUMMARY_KIND_NONE) return true;

    if (func->jit_profile.summary.kind < JIT_SUMMARY_KIND_NONE ||
        func->jit_profile.summary.kind > JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_SELECTOR) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s has unknown JIT summary kind", label);
        return false;
    }
    if (func->jit_profile.summary.op < JIT_SUMMARY_OP_NONE || func->jit_profile.summary.op > JIT_SUMMARY_OP_GE) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s has unknown JIT summary op", label);
        return false;
    }
    if ((func->jit_profile.summary.kind == JIT_SUMMARY_KIND_INT_LOCAL_CONST_SELECTOR ||
         func->jit_profile.summary.kind == JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_SELECTOR) &&
        func->jit_profile.summary.slot1 > 1) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s has invalid selector summary branch flag", label);
        return false;
    }

    JitCompiledPlan summary_plan;
    if (!runtime_jit_plan_from_summary(&func->jit_profile.summary, &summary_plan)) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s has unsupported JIT summary metadata", label);
        return false;
    }
    if (!runtime_validate_jit_hint_plan(func, &summary_plan, label, "JIT summary", error_buf, error_buf_size)) {
        return false;
    }

    if (func->jit_hint_plan.kind != JIT_COMPILED_KIND_NONE) {
        if (summary_plan.kind != func->jit_hint_plan.kind ||
            summary_plan.op != func->jit_hint_plan.op ||
            summary_plan.flags != func->jit_hint_plan.flags ||
            summary_plan.local_slot != func->jit_hint_plan.local_slot ||
            summary_plan.local_slot_b != func->jit_hint_plan.local_slot_b ||
            summary_plan.int_const0 != func->jit_hint_plan.int_const0 ||
            summary_plan.int_const1 != func->jit_hint_plan.int_const1) {
            snprintf(error_buf,
                     error_buf_size,
                     "Invalid artifact: %s JIT summary does not match JIT hint plan",
                     label);
            return false;
        }
    }

    return true;
}

static bool runtime_validate_jit_hint_semantics(const ObjFunction* func,
                                                const char* label,
                                                char* error_buf,
                                                size_t error_buf_size) {
    return runtime_validate_jit_hint_plan(func,
                                          &func->jit_hint_plan,
                                          label,
                                          "JIT hint",
                                          error_buf,
                                          error_buf_size);
}

static bool runtime_validate_function_semantics(VM* vm,
                                                const ObjFunction* func,
                                                const char* role,
                                                char* error_buf,
                                                size_t error_buf_size) {
    const char* label = (role && role[0] != '\0') ? role : "artifact function";
    if (!func) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s is null", label);
        return false;
    }
    if (func->param_count < 0 || func->local_count < 0) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s has negative signature counts", label);
        return false;
    }
    if (func->local_count < func->param_count) {
        snprintf(error_buf,
                 error_buf_size,
                 "Invalid artifact: %s local_count (%d) is smaller than param_count (%d)",
                 label,
                 func->local_count,
                 func->param_count);
        return false;
    }
    if (func->capture_count < 0 || func->capture_count > func->local_count) {
        snprintf(error_buf,
                 error_buf_size,
                 "Invalid artifact: %s capture_count (%d) is out of range for local_count (%d)",
                 label,
                 func->capture_count,
                 func->local_count);
        return false;
    }
    if (func->capture_count > 0 && !func->capture_local_slots) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s is missing closure capture slot metadata", label);
        return false;
    }
    for (int i = 0; i < func->capture_count; i++) {
        int slot = func->capture_local_slots[i];
        if (slot < func->param_count || slot >= func->local_count) {
            snprintf(error_buf,
                     error_buf_size,
                     "Invalid artifact: %s capture slot %d is out of bounds (params=%d locals=%d)",
                     label,
                     slot,
                     func->param_count,
                     func->local_count);
            return false;
        }
    }
    if (vm && vm->config.max_stack_size > 0 && func->local_count > vm->config.max_stack_size) {
        snprintf(error_buf,
                 error_buf_size,
                 "Invalid artifact: %s local_count (%d) exceeds VM stack limit (%d)",
                 label,
                 func->local_count,
                 vm->config.max_stack_size);
        return false;
    }
    if (func->chunk.code_count <= 0 || !func->chunk.code) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s has no bytecode", label);
        return false;
    }
    if (!func->chunk.debug_info) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s is missing debug metadata", label);
        return false;
    }
    if (func->constants.constant_count < 0) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s has negative constant count", label);
        return false;
    }
    if (func->constants.constant_count > 0 && !func->constants.constants) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: %s has missing constant pool storage", label);
        return false;
    }
    if (!runtime_validate_jit_hint_semantics(func, label, error_buf, error_buf_size)) {
        return false;
    }
    if (!runtime_validate_jit_profile_semantics(func, label, error_buf, error_buf_size)) {
        return false;
    }
    return true;
}

static bool runtime_validate_loaded_artifact_semantics(VM* vm,
                                                       const LoadedBytecodeArtifact* artifact,
                                                       char* error_buf,
                                                       size_t error_buf_size) {
    if (!artifact || !artifact->functions || artifact->function_count <= 0) {
        snprintf(error_buf, error_buf_size, "Invalid artifact payload");
        return false;
    }
    if (artifact->main_index < 0 || artifact->main_index >= artifact->function_count) {
        snprintf(error_buf, error_buf_size, "Artifact main function index is invalid");
        return false;
    }
    if (artifact->interface_dispatch_count < 0) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: interface dispatch count is negative");
        return false;
    }
    if (artifact->interface_dispatch_count > 0 && !artifact->interface_dispatch_entries) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: missing interface dispatch payload");
        return false;
    }

    for (int i = 0; i < artifact->interface_dispatch_count; i++) {
        const InterfaceDispatchEntry* entry = &artifact->interface_dispatch_entries[i];
        if (!entry->interface_name || !entry->record_name || !entry->method_name || !entry->function_name) {
            snprintf(error_buf, error_buf_size, "Invalid artifact: interface dispatch mapping[%d] is incomplete", i);
            return false;
        }
        if (entry->interface_name[0] == '\0' ||
            entry->record_name[0] == '\0' ||
            entry->method_name[0] == '\0' ||
            entry->function_name[0] == '\0') {
            snprintf(error_buf, error_buf_size, "Invalid artifact: interface dispatch mapping[%d] has empty names", i);
            return false;
        }
    }

    for (int i = 0; i < artifact->function_count; i++) {
        char role_buf[64];
        snprintf(role_buf, sizeof(role_buf), "function[%d]", i);
        if (!runtime_validate_function_semantics(vm, artifact->functions[i], role_buf, error_buf, error_buf_size)) {
            return false;
        }
    }

    ObjFunction* main_fn = artifact->functions[artifact->main_index];
    if (!main_fn) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: main function pointer is null");
        return false;
    }
    if (main_fn->param_count != 0) {
        snprintf(error_buf, error_buf_size, "Invalid artifact: main function must not take parameters");
        return false;
    }

    if (artifact->init_function) {
        if (!runtime_validate_function_semantics(vm,
                                                 artifact->init_function,
                                                 "init function",
                                                 error_buf,
                                                 error_buf_size)) {
            return false;
        }
        if (artifact->init_function->param_count != 0) {
            snprintf(error_buf, error_buf_size, "Invalid artifact: init function must not take parameters");
            return false;
        }
    }

    return true;
}

static bool runtime_apply_artifact(Runtime* rt, LoadedBytecodeArtifact* artifact, const char* error_context) {
    if (!rt || !rt->vm || !artifact) return false;

    char semantic_error[256];
    if (!runtime_validate_loaded_artifact_semantics(rt->vm, artifact, semantic_error, sizeof(semantic_error))) {
        if (error_context && error_context[0] != '\0') {
            char prefixed[320];
            snprintf(prefixed, sizeof(prefixed), "%s: %s", error_context, semantic_error);
            vm_runtime_error(rt->vm, prefixed);
        } else {
            vm_runtime_error(rt->vm, semantic_error);
        }
        return false;
    }

    rt->init_function = artifact->init_function;
    rt->functions = artifact->functions;
    rt->function_count = artifact->function_count;
    rt->main_function = rt->functions[artifact->main_index];
    rt->interface_dispatch_entries = artifact->interface_dispatch_entries;
    rt->interface_dispatch_count = artifact->interface_dispatch_count;
    rt->globals = NULL;
    rt->init_ran = false;

    artifact->init_function = NULL;
    artifact->functions = NULL;
    artifact->function_count = 0;
    artifact->main_index = -1;
    artifact->interface_dispatch_entries = NULL;
    artifact->interface_dispatch_count = 0;

    runtime_register_functions(rt);
    runtime_register_interface_impls(rt);
    return true;
}

static bool runtime_validate_cached_artifact(const LoadedBytecodeArtifact* artifact, uint32_t expected_flags) {
    if (!artifact) return false;
    if (artifact->typecheck_flags != expected_flags) return false;
    if (artifact->dependency_count <= 0 || !artifact->dependencies) return false;
    if (artifact->function_count <= 0 || !artifact->functions) return false;
    if (artifact->interface_dispatch_count < 0) return false;
    if (artifact->interface_dispatch_count > 0 && !artifact->interface_dispatch_entries) return false;

    // Cache artifacts must carry function source metadata so runtime stack traces
    // can include file locations for traps/panics.
    if (artifact->init_function &&
        (!artifact->init_function->source_file || artifact->init_function->source_file[0] == '\0')) {
        return false;
    }
    for (int i = 0; i < artifact->function_count; i++) {
        ObjFunction* fn = artifact->functions[i];
        if (!fn || !fn->source_file || fn->source_file[0] == '\0') {
            return false;
        }
    }

    for (int i = 0; i < artifact->dependency_count; i++) {
        const LoadedArtifactDependency* dep = &artifact->dependencies[i];
        int64_t now = 0;
        if (!dep->path || dep->path[0] == '\0') return false;
        if (!runtime_file_mtime_token(dep->path, &now)) return false;
        if (now != dep->mtime) return false;
    }
    return true;
}

static bool runtime_try_load_cache(Runtime* rt, const char* cache_path, uint32_t typecheck_flags) {
    if (!rt || !cache_path || cache_path[0] == '\0') return false;

    bool loaded = false;
    if (artifact_file_is_bytecode(cache_path)) {
        LoadedBytecodeArtifact artifact;
        char err[256];
        if (artifact_load_file(cache_path, &artifact, err, sizeof(err))) {
            if (runtime_validate_cached_artifact(&artifact, typecheck_flags)) {
                loaded = runtime_apply_artifact(rt, &artifact, "Failed to apply cached artifact");
                if (loaded) {
                    rt->load_mode = RUNTIME_LOAD_CACHE;
                } else {
                    if (rt->vm && vm_has_error(rt->vm)) {
                        vm_clear_error(rt->vm);
                    }
                    remove(cache_path);
                }
            } else {
                remove(cache_path);
            }
            artifact_loaded_free(&artifact);
        } else {
            // Corrupt or stale cache artifact; ignore and fall back to source compilation.
            remove(cache_path);
        }
    }

    return loaded;
}

static void runtime_try_write_cache(const char* file_path,
                                    uint32_t typecheck_flags,
                                    ModuleLoader* loader,
                                    ObjFunction* init_function,
                                    ObjFunction** functions,
                                    int function_count,
                                    ObjFunction* main_function,
                                    const InterfaceDispatchEntry* interface_dispatch_entries,
                                    int interface_dispatch_count) {
    if (!file_path || !loader || !functions || function_count <= 0 || !main_function) return;

    int main_index = -1;
    for (int i = 0; i < function_count; i++) {
        if (functions[i] == main_function) {
            main_index = i;
            break;
        }
    }
    if (main_index < 0) return;

    int dep_count = loader->module_count;
    if (dep_count <= 0 || !loader->modules) return;

    ArtifactDependencyInfo* deps = (ArtifactDependencyInfo*)safe_calloc((size_t)dep_count, sizeof(ArtifactDependencyInfo));
    bool deps_ok = true;
    for (int i = 0; i < dep_count; i++) {
        Module* mod = loader->modules[i];
        if (!mod || !mod->path || mod->path[0] == '\0') {
            deps_ok = false;
            break;
        }
        int64_t mtime = 0;
        if (!runtime_file_mtime_token(mod->path, &mtime)) {
            deps_ok = false;
            break;
        }
        deps[i].path = mod->path;
        deps[i].mtime = mtime;
    }

    if (deps_ok) {
        char* cache_path = runtime_cache_path_alloc(file_path, typecheck_flags);
        if (cache_path) {
            char err[256];
            (void)artifact_write_file(cache_path,
                                      init_function,
                                      functions,
                                      function_count,
                                      main_index,
                                      typecheck_flags,
                                      deps,
                                      dep_count,
                                      interface_dispatch_entries,
                                      interface_dispatch_count,
                                      err,
                                      sizeof(err));
            free(cache_path);
        }
    }

    free(deps);
}

Runtime* runtime_create(const char* file_path) {
    RuntimeOptions options = {0};
    return runtime_create_with_options(file_path, options);
}

Runtime* runtime_create_with_options(const char* file_path, RuntimeOptions options) {
    Runtime* rt = (Runtime*)calloc(1, sizeof(Runtime));
    if (!rt) {
        return NULL;
    }

    rt->load_mode = RUNTIME_LOAD_SOURCE;

    rt->vm = (VM*)calloc(1, sizeof(VM));
    if (!rt->vm) {
        runtime_set_fallback_error(rt, "Out of memory while allocating VM state");
        return rt;
    }

    SafeAllocJmpContext alloc_ctx;
    jmp_buf alloc_env;
    safe_alloc_push_jmp_context(&alloc_ctx,
                                &alloc_env,
                                rt->fallback_error_message,
                                sizeof(rt->fallback_error_message));

#define RUNTIME_CREATE_RETURN(value) \
    do { \
        safe_alloc_pop_jmp_context(&alloc_ctx); \
        return (value); \
    } while (0)

    if (setjmp(alloc_env) != 0) {
        rt->create_recovery_error = true;
        runtime_set_fallback_error(rt,
                                   rt->fallback_error_message[0]
                                       ? rt->fallback_error_message
                                       : "Out of memory while creating runtime");
        RUNTIME_CREATE_RETURN(rt);
    }

    vm_init(rt->vm);
    runtime_apply_capability_options(rt->vm, options);
    register_builtins(rt->vm);
    rt->vm_initialized = true;

    if (runtime_extensions_enabled(options)) {
        char ext_err[256];
        ext_err[0] = '\0';
        rt->extension_registry = native_extension_registry_create();
        if (!rt->extension_registry) {
            runtime_set_fallback_error(rt, "Out of memory while allocating extension registry");
            RUNTIME_CREATE_RETURN(rt);
        }
        if (!native_extension_registry_load_paths(rt->extension_registry,
                                                  options.extension_paths,
                                                  options.extension_path_count,
                                                  ext_err,
                                                  sizeof(ext_err))) {
            runtime_set_fallback_error(rt, ext_err[0] ? ext_err : "Failed to load native extensions");
            RUNTIME_CREATE_RETURN(rt);
        }
        rt->vm->extension_registry = rt->extension_registry;
        if (!native_extension_registry_register_vm_globals(rt->extension_registry,
                                                           rt->vm,
                                                           ext_err,
                                                           sizeof(ext_err))) {
            runtime_set_fallback_error(rt, ext_err[0] ? ext_err : "Failed to register native extension globals");
            RUNTIME_CREATE_RETURN(rt);
        }
        options.typecheck.extension_registry = rt->extension_registry;
    }

    char* sandbox_root = runtime_find_module_root_alloc(file_path);
    vm_set_sandbox_root(rt->vm, sandbox_root);
    free(sandbox_root);

    char* effective_file_path =
        runtime_normalize_entry_path_alloc(file_path ? file_path : "", vm_get_sandbox_root(rt->vm));
    const char* entry_path = effective_file_path ? effective_file_path : (file_path ? file_path : "");

    Value entry_file_val;
    value_init_string(&entry_file_val, entry_path);
    vm_set_global(rt->vm, "__vml_entry_file", entry_file_val);

    uint32_t typecheck_flags = runtime_typecheck_flags(options);
    rt->cache_path = runtime_cache_path_alloc(entry_path, typecheck_flags);

    if (artifact_file_is_bytecode(entry_path)) {
        LoadedBytecodeArtifact artifact;
        char err[256];
        if (!artifact_load_file(entry_path, &artifact, err, sizeof(err))) {
            vm_runtime_error(rt->vm, err[0] ? err : "Failed to load bytecode artifact");
            if (effective_file_path) free(effective_file_path);
            RUNTIME_CREATE_RETURN(rt);
        }
        if (!runtime_apply_artifact(rt, &artifact, "Failed to apply loaded artifact")) {
            artifact_loaded_free(&artifact);
            if (effective_file_path) free(effective_file_path);
            RUNTIME_CREATE_RETURN(rt);
        }
        rt->load_mode = RUNTIME_LOAD_ARTIFACT;
        artifact_loaded_free(&artifact);
        if (effective_file_path) free(effective_file_path);
        RUNTIME_CREATE_RETURN(rt);
    }

    if (!runtime_extensions_enabled(options) &&
        runtime_try_load_cache(rt, rt->cache_path, typecheck_flags)) {
        if (effective_file_path) free(effective_file_path);
        RUNTIME_CREATE_RETURN(rt);
    }

    ModuleLoader* loader = module_loader_create(vm_get_sandbox_root(rt->vm));
    LoadResult load_result = module_loader_load_main_with_options(loader, entry_path, options.typecheck);

    if (load_result.error) {
        rt->error = load_result.error;
        if (load_result.interface_dispatch_entries) {
            interface_dispatch_entries_free(load_result.interface_dispatch_entries,
                                            load_result.interface_dispatch_count);
            load_result.interface_dispatch_entries = NULL;
            load_result.interface_dispatch_count = 0;
        }
        module_loader_free(loader);
        if (effective_file_path) free(effective_file_path);
        RUNTIME_CREATE_RETURN(rt);
    }

    rt->init_function = load_result.init_function;
    rt->main_function = load_result.main_function;
    rt->globals = load_result.globals;
    rt->functions = load_result.functions;
    rt->function_count = load_result.function_count;
    rt->interface_dispatch_entries = load_result.interface_dispatch_entries;
    rt->interface_dispatch_count = load_result.interface_dispatch_count;

    runtime_register_functions(rt);
    runtime_register_interface_impls(rt);

    if (!runtime_extensions_enabled(options)) {
        runtime_try_write_cache(entry_path,
                                typecheck_flags,
                                loader,
                                rt->init_function,
                                rt->functions,
                                rt->function_count,
                                rt->main_function,
                                rt->interface_dispatch_entries,
                                rt->interface_dispatch_count);
    }

    module_loader_free(loader);

    if (effective_file_path) free(effective_file_path);
    RUNTIME_CREATE_RETURN(rt);

#undef RUNTIME_CREATE_RETURN
}

void runtime_free(Runtime* rt) {
    if (!rt) return;
    if (rt->extension_registry) {
        // Run shutdown hooks while the VM is still alive, but keep extension libraries loaded
        // until vm_free() finishes releasing extension-owned handles and callbacks.
        native_extension_registry_shutdown(rt->extension_registry);
    }
    if (rt->vm) {
        if (rt->vm_initialized && !rt->create_recovery_error) {
            vm_free(rt->vm);
        }
        free(rt->vm);
        rt->vm = NULL;
    }
    if (rt->extension_registry) {
        native_extension_registry_free(rt->extension_registry);
        rt->extension_registry = NULL;
    }
    if (rt->create_recovery_error) {
        if (rt->cache_path) {
            free(rt->cache_path);
            rt->cache_path = NULL;
        }
        error_free(rt->error);
        rt->error = NULL;
        free(rt);
        return;
    }
    if (rt->init_function) {
        obj_function_free(rt->init_function);
        rt->init_function = NULL;
    }
    if (rt->functions) {
        free(rt->functions);
    }
    if (rt->interface_dispatch_entries) {
        interface_dispatch_entries_free(rt->interface_dispatch_entries, rt->interface_dispatch_count);
        rt->interface_dispatch_entries = NULL;
        rt->interface_dispatch_count = 0;
    }
    if (rt->globals) {
        symbol_table_free(rt->globals);
    }
    if (rt->cache_path) {
        free(rt->cache_path);
    }
    error_free(rt->error);
    free(rt);
}

void runtime_set_argv(Runtime* rt, int argc, char** argv) {
    if (!rt || !rt->vm) return;

    ObjArray* arr = obj_array_create(rt->vm, argc > 0 ? argc : 1);
    for (int i = 0; i < argc; i++) {
        Value str_val;
        value_init_string(&str_val, argv[i]);
        obj_array_push(arr, str_val);
    }

    Value argv_val;
    value_init_array(&argv_val, arr);
    vm_set_global(rt->vm, "argv", argv_val);
}

void runtime_set_thread_channels(Runtime* rt, int inbox_channel_id, int outbox_channel_id) {
    if (!rt || !rt->vm) return;

    Value inbox_value;
    Value outbox_value;
    value_init_int(&inbox_value, inbox_channel_id);
    value_init_int(&outbox_value, outbox_channel_id);
    vm_set_global(rt->vm, "__thread_inbox", inbox_value);
    vm_set_global(rt->vm, "__thread_outbox", outbox_value);
}

int runtime_run(Runtime* rt) {
    if (!rt || !rt->main_function) {
        return -1;
    }

    SafeAllocJmpContext alloc_ctx;
    jmp_buf alloc_env;
    char alloc_message[256] = {0};
    safe_alloc_push_jmp_context(&alloc_ctx, &alloc_env, alloc_message, sizeof(alloc_message));

#define RUNTIME_RUN_RETURN(value) \
    do { \
        safe_alloc_pop_jmp_context(&alloc_ctx); \
        return (value); \
    } while (0)

    if (setjmp(alloc_env) != 0) {
        runtime_set_fallback_error(rt,
                                   alloc_message[0]
                                       ? alloc_message
                                       : "Out of memory while executing program");
        RUNTIME_RUN_RETURN(-1);
    }

    if (!rt->init_ran && rt->init_function) {
        int rc = vm_execute(rt->vm, rt->init_function);
        if (rc == 1 || runtime_has_error(rt)) {
            RUNTIME_RUN_RETURN(rc);
        }
        rt->init_ran = true;
        obj_function_free(rt->init_function);
        rt->init_function = NULL;
        if (rc != 0) {
            RUNTIME_RUN_RETURN(rc);
        }
    }

    if (rt->debug_stop_on_main_entry_pending) {
        vm_debug_request_stop_on_entry(rt->vm);
        rt->debug_stop_on_main_entry_pending = false;
    }

    int rc = vm_execute(rt->vm, rt->main_function);
    RUNTIME_RUN_RETURN(rc);

#undef RUNTIME_RUN_RETURN
}

int runtime_resume(Runtime* rt) {
    if (!rt || !rt->vm) {
        return -1;
    }

    SafeAllocJmpContext alloc_ctx;
    jmp_buf alloc_env;
    char alloc_message[256] = {0};
    safe_alloc_push_jmp_context(&alloc_ctx, &alloc_env, alloc_message, sizeof(alloc_message));

#define RUNTIME_RESUME_RETURN(value) \
    do { \
        safe_alloc_pop_jmp_context(&alloc_ctx); \
        return (value); \
    } while (0)

    if (setjmp(alloc_env) != 0) {
        runtime_set_fallback_error(rt,
                                   alloc_message[0]
                                       ? alloc_message
                                       : "Out of memory while resuming program");
        RUNTIME_RESUME_RETURN(-1);
    }

    int rc = vm_resume(rt->vm);
    if (!rt->init_ran && rt->init_function) {
        if (rc == 1 || runtime_has_error(rt)) {
            RUNTIME_RESUME_RETURN(rc);
        }
        rt->init_ran = true;
        obj_function_free(rt->init_function);
        rt->init_function = NULL;
        if (rc != 0) {
            RUNTIME_RESUME_RETURN(rc);
        }
        if (rt->debug_stop_on_main_entry_pending) {
            vm_debug_request_stop_on_entry(rt->vm);
            rt->debug_stop_on_main_entry_pending = false;
        }
        rc = vm_execute(rt->vm, rt->main_function);
    }
    RUNTIME_RESUME_RETURN(rc);

#undef RUNTIME_RESUME_RETURN
}

int runtime_run_function(Runtime* rt, const char* function_name) {
    if (!rt || !rt->vm || !function_name || function_name[0] == '\0') {
        return -1;
    }

    SafeAllocJmpContext alloc_ctx;
    jmp_buf alloc_env;
    char alloc_message[256] = {0};
    safe_alloc_push_jmp_context(&alloc_ctx, &alloc_env, alloc_message, sizeof(alloc_message));

#define RUNTIME_RUN_FN_RETURN(value) \
    do { \
        safe_alloc_pop_jmp_context(&alloc_ctx); \
        return (value); \
    } while (0)

    if (setjmp(alloc_env) != 0) {
        runtime_set_fallback_error(rt,
                                   alloc_message[0]
                                       ? alloc_message
                                       : "Out of memory while executing function");
        RUNTIME_RUN_FN_RETURN(-1);
    }

    if (!rt->init_ran && rt->init_function) {
        int rc = vm_execute(rt->vm, rt->init_function);
        rt->init_ran = true;
        obj_function_free(rt->init_function);
        rt->init_function = NULL;
        if (rc != 0 || runtime_has_error(rt)) {
            RUNTIME_RUN_FN_RETURN(rc);
        }
    }

    Value callee = vm_get_global(rt->vm, function_name);
    if (value_get_type(&callee) != VAL_FUNCTION || !value_get_function_obj(&callee)) {
        vm_runtime_error(rt->vm, "Thread entry function not found");
        RUNTIME_RUN_FN_RETURN(-1);
    }

    int rc = vm_execute(rt->vm, value_get_function_obj(&callee));
    RUNTIME_RUN_FN_RETURN(rc);

#undef RUNTIME_RUN_FN_RETURN
}

bool runtime_take_return_value(Runtime* rt, Value* out) {
    if (!rt || !rt->vm || !out) return false;
    return vm_take_return_value(rt->vm, out);
}

bool runtime_has_error(Runtime* rt) {
    if (!rt) return false;
    return rt->fallback_error_set || rt->error != NULL || (rt->vm && vm_has_error(rt->vm));
}

const char* runtime_get_error(Runtime* rt) {
    if (!rt) return NULL;
    if (rt->fallback_error_set && rt->fallback_error_message[0] != '\0') {
        return rt->fallback_error_message;
    }
    if (rt->error) {
        return rt->error->message;
    }
    if (rt->vm && vm_has_error(rt->vm)) {
        return vm_get_error(rt->vm);
    }
    return NULL;
}

RuntimeLoadMode runtime_get_load_mode(Runtime* rt) {
    if (!rt) return RUNTIME_LOAD_SOURCE;
    return rt->load_mode;
}

const char* runtime_get_cache_path(Runtime* rt) {
    if (!rt) return NULL;
    return rt->cache_path;
}

bool runtime_has_posted_callbacks(Runtime* rt) {
    if (!rt || !rt->vm) return false;
    return vm_has_posted_event_queue_work(rt->vm);
}

int runtime_posted_callback_pending_count(Runtime* rt) {
    if (!rt || !rt->vm) return 0;
    return vm_posted_event_queue_pending_count(rt->vm);
}

bool runtime_close_posted_callback_queue(Runtime* rt) {
    if (!rt || !rt->vm) return false;
    return vm_close_posted_event_queue(rt->vm);
}

bool runtime_is_posted_callback_queue_open(Runtime* rt) {
    if (!rt || !rt->vm) return false;
    return vm_is_posted_event_queue_open(rt->vm);
}

int runtime_drain_posted_callbacks(Runtime* rt, int max_callbacks) {
    if (!rt || !rt->vm) return 0;
    return vm_drain_posted_event_queue(rt->vm, max_callbacks);
}

int runtime_drain_posted_callbacks_for_ms(Runtime* rt, int max_callbacks, int64_t max_millis) {
    if (!rt || !rt->vm) return 0;
    return vm_drain_posted_event_queue_for_ms(rt->vm, max_callbacks, max_millis);
}

bool runtime_wait_for_posted_callbacks(Runtime* rt, int64_t timeout_millis) {
    if (!rt || !rt->vm) return false;
    return vm_wait_for_posted_event_queue(rt->vm, timeout_millis);
}

int runtime_wait_and_drain_posted_callbacks(Runtime* rt, int max_callbacks, int64_t timeout_millis) {
    if (!rt || !rt->vm) return 0;
    return vm_wait_and_drain_posted_event_queue(rt->vm, max_callbacks, timeout_millis);
}

bool runtime_get_posted_callback_auto_drain(Runtime* rt) {
    if (!rt || !rt->vm) return false;
    return vm_get_posted_event_auto_drain(rt->vm);
}

bool runtime_set_posted_callback_auto_drain(Runtime* rt, bool enabled) {
    if (!rt || !rt->vm) return false;
    return vm_set_posted_event_auto_drain(rt->vm, enabled);
}

static bool runtime_prepare_augmented_extra_fields(const RuntimePostedEventExtraField* base_fields,
                                                   int base_count,
                                                   const RuntimePostedEventExtraField* generated_fields,
                                                   int generated_count,
                                                   RuntimePostedEventExtraField** owned_out,
                                                   const RuntimePostedEventExtraField** merged_out,
                                                   int* merged_count_out,
                                                   char* error_buf,
                                                   size_t error_buf_size) {
    RuntimePostedEventExtraField* merged = NULL;

    if (!owned_out || !merged_out || !merged_count_out) return false;
    *owned_out = NULL;
    *merged_out = NULL;
    *merged_count_out = 0;

    if (base_count < 0 || generated_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Event extra field count is invalid");
        return false;
    }

    if (base_count == 0 && generated_count == 0) {
        return true;
    }
    if (base_count == 0) {
        *merged_out = generated_fields;
        *merged_count_out = generated_count;
        return true;
    }
    if (generated_count == 0) {
        *merged_out = base_fields;
        *merged_count_out = base_count;
        return true;
    }

    merged = (RuntimePostedEventExtraField*)calloc((size_t)(base_count + generated_count),
                                                   sizeof(RuntimePostedEventExtraField));
    if (!merged) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while preparing event extra fields");
        return false;
    }
    memcpy(merged, base_fields, (size_t)base_count * sizeof(RuntimePostedEventExtraField));
    memcpy(merged + base_count,
           generated_fields,
           (size_t)generated_count * sizeof(RuntimePostedEventExtraField));
    *owned_out = merged;
    *merged_out = merged;
    *merged_count_out = base_count + generated_count;
    return true;
}

static bool runtime_post_input_event_with_generated_fields(Runtime* rt,
                                                           const char* callback_name,
                                                           const RuntimePostedInputEvent* event,
                                                           const RuntimePostedEventExtraField* generated_fields,
                                                           int generated_count,
                                                           char* error_buf,
                                                           size_t error_buf_size) {
    RuntimePostedInputEvent merged_event;
    RuntimePostedEventExtraField* owned_fields = NULL;
    const RuntimePostedEventExtraField* merged_fields = NULL;
    int merged_count = 0;
    bool ok = false;

    if (!event) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Input event payload is required");
        return false;
    }

    if (!runtime_prepare_augmented_extra_fields(event->extra_fields,
                                                event->extra_field_count,
                                                generated_fields,
                                                generated_count,
                                                &owned_fields,
                                                &merged_fields,
                                                &merged_count,
                                                error_buf,
                                                error_buf_size)) {
        return false;
    }

    merged_event = *event;
    merged_event.extra_fields = merged_fields;
    merged_event.extra_field_count = merged_count;
    ok = runtime_post_input_event(rt, callback_name, &merged_event, error_buf, error_buf_size);
    free(owned_fields);
    return ok;
}

static bool runtime_post_window_event_with_generated_fields(Runtime* rt,
                                                            const char* callback_name,
                                                            const RuntimePostedWindowEvent* event,
                                                            const RuntimePostedEventExtraField* generated_fields,
                                                            int generated_count,
                                                            char* error_buf,
                                                            size_t error_buf_size) {
    RuntimePostedWindowEvent merged_event;
    RuntimePostedEventExtraField* owned_fields = NULL;
    const RuntimePostedEventExtraField* merged_fields = NULL;
    int merged_count = 0;
    bool ok = false;

    if (!event) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Window event payload is required");
        return false;
    }

    if (!runtime_prepare_augmented_extra_fields(event->extra_fields,
                                                event->extra_field_count,
                                                generated_fields,
                                                generated_count,
                                                &owned_fields,
                                                &merged_fields,
                                                &merged_count,
                                                error_buf,
                                                error_buf_size)) {
        return false;
    }

    merged_event = *event;
    merged_event.extra_fields = merged_fields;
    merged_event.extra_field_count = merged_count;
    ok = runtime_post_window_event(rt, callback_name, &merged_event, error_buf, error_buf_size);
    free(owned_fields);
    return ok;
}

static bool runtime_post_frame_event_with_generated_fields(Runtime* rt,
                                                           const char* callback_name,
                                                           const RuntimePostedFrameEvent* event,
                                                           const RuntimePostedEventExtraField* generated_fields,
                                                           int generated_count,
                                                           char* error_buf,
                                                           size_t error_buf_size) {
    RuntimePostedFrameEvent merged_event;
    RuntimePostedEventExtraField* owned_fields = NULL;
    const RuntimePostedEventExtraField* merged_fields = NULL;
    int merged_count = 0;
    bool ok = false;

    if (!event) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Frame event payload is required");
        return false;
    }

    if (!runtime_prepare_augmented_extra_fields(event->extra_fields,
                                                event->extra_field_count,
                                                generated_fields,
                                                generated_count,
                                                &owned_fields,
                                                &merged_fields,
                                                &merged_count,
                                                error_buf,
                                                error_buf_size)) {
        return false;
    }

    merged_event = *event;
    merged_event.extra_fields = merged_fields;
    merged_event.extra_field_count = merged_count;
    ok = runtime_post_frame_event(rt, callback_name, &merged_event, error_buf, error_buf_size);
    free(owned_fields);
    return ok;
}

static void runtime_free_owned_extra_field_arrays(RuntimePostedEventExtraField** owned_fields,
                                                  int count) {
    if (!owned_fields) return;
    for (int i = 0; i < count; i++) {
        free(owned_fields[i]);
    }
}

bool runtime_post_input_event(Runtime* rt,
                              const char* callback_name,
                              const RuntimePostedInputEvent* event,
                              char* error_buf,
                              size_t error_buf_size) {
    if (!event) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Input event payload is required");
        return false;
    }
    return runtime_post_event(rt,
                              RUNTIME_POSTED_EVENT_INPUT,
                              callback_name,
                              event->device ? event->device : "",
                              event->code,
                              0,
                              event->pressed,
                              0.0,
                              event->meta_override,
                              event->extra_fields,
                              event->extra_field_count,
                              error_buf,
                              error_buf_size);
}

bool runtime_post_input_state_event(Runtime* rt,
                                    const char* callback_name,
                                    const RuntimePostedInputEvent* event,
                                    const RuntimePostedInputStatePayloadSpec* payload_spec,
                                    const RuntimePostedEventContextSpec* context_spec,
                                    char* error_buf,
                                    size_t error_buf_size) {
    RuntimePostedEventExtraField generated_fields[2];
    RuntimePostedEventExtraMapEntry payload_entries[5];
    RuntimePostedEventExtraMapEntry context_entries[2];
    int generated_count = 0;

    if (payload_spec) {
        generated_fields[generated_count++] = runtime_posted_event_build_input_state_payload_field(payload_entries,
                                                                                                   5,
                                                                                                   payload_spec);
    }
    if (context_spec) {
        generated_fields[generated_count++] = runtime_posted_event_build_context_field(context_entries,
                                                                                       2,
                                                                                       context_spec);
    }
    return runtime_post_input_event_with_generated_fields(rt,
                                                          callback_name,
                                                          event,
                                                          generated_fields,
                                                          generated_count,
                                                          error_buf,
                                                          error_buf_size);
}

bool runtime_post_input_combo_event(Runtime* rt,
                                    const char* callback_name,
                                    const RuntimePostedInputEvent* event,
                                    const RuntimePostedInputComboPayloadSpec* payload_spec,
                                    const RuntimePostedEventContextSpec* context_spec,
                                    char* error_buf,
                                    size_t error_buf_size) {
    RuntimePostedEventExtraField generated_fields[2];
    RuntimePostedEventExtraMapEntry payload_entries[2];
    RuntimePostedEventExtraMapEntry context_entries[2];
    int generated_count = 0;

    if (payload_spec) {
        generated_fields[generated_count++] = runtime_posted_event_build_input_combo_payload_field(payload_entries,
                                                                                                   2,
                                                                                                   payload_spec);
    }
    if (context_spec) {
        generated_fields[generated_count++] = runtime_posted_event_build_context_field(context_entries,
                                                                                       2,
                                                                                       context_spec);
    }
    return runtime_post_input_event_with_generated_fields(rt,
                                                          callback_name,
                                                          event,
                                                          generated_fields,
                                                          generated_count,
                                                          error_buf,
                                                          error_buf_size);
}

bool runtime_post_window_event(Runtime* rt,
                               const char* callback_name,
                               const RuntimePostedWindowEvent* event,
                               char* error_buf,
                               size_t error_buf_size) {
    if (!event) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Window event payload is required");
        return false;
    }
    return runtime_post_event(rt,
                              RUNTIME_POSTED_EVENT_WINDOW,
                              callback_name,
                              event->event_name ? event->event_name : "",
                              event->width,
                              event->height,
                              event->focused,
                              0.0,
                              event->meta_override,
                              event->extra_fields,
                              event->extra_field_count,
                              error_buf,
                              error_buf_size);
}

bool runtime_post_window_rect_event(Runtime* rt,
                                    const char* callback_name,
                                    const RuntimePostedWindowEvent* event,
                                    const RuntimePostedWindowRectPayloadSpec* payload_spec,
                                    const RuntimePostedEventContextSpec* context_spec,
                                    char* error_buf,
                                    size_t error_buf_size) {
    RuntimePostedEventExtraField generated_fields[2];
    RuntimePostedEventExtraMapEntry payload_entries[2];
    RuntimePostedEventExtraMapEntry context_entries[2];
    int generated_count = 0;

    if (payload_spec) {
        generated_fields[generated_count++] = runtime_posted_event_build_window_rect_payload_field(payload_entries,
                                                                                                   2,
                                                                                                   payload_spec);
    }
    if (context_spec) {
        generated_fields[generated_count++] = runtime_posted_event_build_context_field(context_entries,
                                                                                       2,
                                                                                       context_spec);
    }
    return runtime_post_window_event_with_generated_fields(rt,
                                                           callback_name,
                                                           event,
                                                           generated_fields,
                                                           generated_count,
                                                           error_buf,
                                                           error_buf_size);
}

bool runtime_post_frame_event(Runtime* rt,
                              const char* callback_name,
                              const RuntimePostedFrameEvent* event,
                              char* error_buf,
                              size_t error_buf_size) {
    if (!event) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Frame event payload is required");
        return false;
    }
    return runtime_post_event(rt,
                              RUNTIME_POSTED_EVENT_FRAME,
                              callback_name,
                              event->phase ? event->phase : "",
                              event->frame_number,
                              0,
                              false,
                              event->delta_seconds,
                              event->meta_override,
                              event->extra_fields,
                              event->extra_field_count,
                              error_buf,
                              error_buf_size);
}

bool runtime_post_frame_marker_event(Runtime* rt,
                                     const char* callback_name,
                                     const RuntimePostedFrameEvent* event,
                                     const RuntimePostedFrameMarkerPayloadSpec* payload_spec,
                                     const RuntimePostedEventContextSpec* context_spec,
                                     char* error_buf,
                                     size_t error_buf_size) {
    RuntimePostedEventExtraField generated_fields[2];
    RuntimePostedEventExtraMapEntry payload_entries[2];
    RuntimePostedEventExtraMapEntry context_entries[2];
    int generated_count = 0;

    if (payload_spec) {
        generated_fields[generated_count++] = runtime_posted_event_build_frame_marker_payload_field(payload_entries,
                                                                                                    2,
                                                                                                    payload_spec);
    }
    if (context_spec) {
        generated_fields[generated_count++] = runtime_posted_event_build_context_field(context_entries,
                                                                                       2,
                                                                                       context_spec);
    }
    return runtime_post_frame_event_with_generated_fields(rt,
                                                          callback_name,
                                                          event,
                                                          generated_fields,
                                                          generated_count,
                                                          error_buf,
                                                          error_buf_size);
}

bool runtime_post_input_event_batch(Runtime* rt,
                                    const char* callback_name,
                                    const RuntimePostedInputEvent* events,
                                    int event_count,
                                    char* error_buf,
                                    size_t error_buf_size) {
    RuntimePostedEventBatchItem* items = NULL;
    bool ok = false;
    int prepared_count = 0;

    if (event_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Input event batch count is invalid");
        return false;
    }
    if (event_count > 0 && !events) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Input event batch payload is required");
        return false;
    }

    if (event_count > 0) {
        items = (RuntimePostedEventBatchItem*)calloc((size_t)event_count, sizeof(RuntimePostedEventBatchItem));
        if (!items) {
            runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while preparing input event batch");
            return false;
        }
        for (int i = 0; i < event_count; i++) {
            items[i].kind = RUNTIME_POSTED_EVENT_INPUT;
            items[i].text_value = (char*)(events[i].device ? events[i].device : "");
            items[i].int_value_a = events[i].code;
            items[i].bool_value = events[i].pressed;
            if (!runtime_clone_posted_event_meta(&items[i].meta,
                                                 events[i].meta_override,
                                                 error_buf,
                                                 error_buf_size) ||
                !runtime_clone_posted_event_extra_state(&items[i].extra,
                                                        RUNTIME_POSTED_EVENT_INPUT,
                                                        events[i].extra_fields,
                                                        events[i].extra_field_count,
                                                 error_buf,
                                                 error_buf_size)) {
                prepared_count = i + 1;
                goto cleanup;
            }
            prepared_count = i + 1;
        }
    }

    ok = runtime_post_event_batch(rt,
                                  callback_name,
                                  items,
                                  event_count,
                                  error_buf,
                                  error_buf_size);
cleanup:
    if (items) {
        for (int i = 0; i < prepared_count; i++) {
            runtime_posted_event_meta_state_free(&items[i].meta);
            runtime_posted_event_extra_state_free(&items[i].extra);
        }
    }
    free(items);
    return ok;
}

bool runtime_post_input_state_event_batch(Runtime* rt,
                                          const char* callback_name,
                                          const RuntimePostedInputEvent* events,
                                          const RuntimePostedInputStatePayloadSpec* payload_specs,
                                          const RuntimePostedEventContextSpec* context_specs,
                                          int event_count,
                                          char* error_buf,
                                          size_t error_buf_size) {
    RuntimePostedInputEvent* merged_events = NULL;
    RuntimePostedEventExtraField** owned_fields = NULL;
    RuntimePostedEventExtraField* generated_fields = NULL;
    RuntimePostedEventExtraMapEntry* payload_entries = NULL;
    RuntimePostedEventExtraMapEntry* context_entries = NULL;
    bool ok = false;

    if (event_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Input event batch count is invalid");
        return false;
    }
    if (event_count > 0 && !events) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Input event batch payload is required");
        return false;
    }
    if (event_count == 0) {
        return runtime_post_input_event_batch(rt, callback_name, events, event_count, error_buf, error_buf_size);
    }

    merged_events = (RuntimePostedInputEvent*)calloc((size_t)event_count, sizeof(RuntimePostedInputEvent));
    owned_fields = (RuntimePostedEventExtraField**)calloc((size_t)event_count, sizeof(RuntimePostedEventExtraField*));
    generated_fields = (RuntimePostedEventExtraField*)calloc((size_t)event_count * 2u, sizeof(RuntimePostedEventExtraField));
    payload_entries = (RuntimePostedEventExtraMapEntry*)calloc((size_t)event_count * 5u, sizeof(RuntimePostedEventExtraMapEntry));
    context_entries = (RuntimePostedEventExtraMapEntry*)calloc((size_t)event_count * 2u, sizeof(RuntimePostedEventExtraMapEntry));
    if (!merged_events || !owned_fields || !generated_fields || !payload_entries || !context_entries) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while preparing input event batch");
        goto cleanup;
    }

    for (int i = 0; i < event_count; i++) {
        RuntimePostedEventExtraField* generated = generated_fields + (i * 2);
        const RuntimePostedEventExtraField* merged_extra = NULL;
        int generated_count = 0;
        int merged_count = 0;

        merged_events[i] = events[i];
        if (payload_specs) {
            generated[generated_count++] = runtime_posted_event_build_input_state_payload_field(payload_entries + (i * 5),
                                                                                                5,
                                                                                                &payload_specs[i]);
        }
        if (context_specs) {
            generated[generated_count++] = runtime_posted_event_build_context_field(context_entries + (i * 2),
                                                                                    2,
                                                                                    &context_specs[i]);
        }
        if (!runtime_prepare_augmented_extra_fields(events[i].extra_fields,
                                                    events[i].extra_field_count,
                                                    generated,
                                                    generated_count,
                                                    &owned_fields[i],
                                                    &merged_extra,
                                                    &merged_count,
                                                    error_buf,
                                                    error_buf_size)) {
            goto cleanup;
        }
        merged_events[i].extra_fields = merged_extra;
        merged_events[i].extra_field_count = merged_count;
    }

    ok = runtime_post_input_event_batch(rt,
                                        callback_name,
                                        merged_events,
                                        event_count,
                                        error_buf,
                                        error_buf_size);

cleanup:
    runtime_free_owned_extra_field_arrays(owned_fields, event_count);
    free(context_entries);
    free(payload_entries);
    free(generated_fields);
    free(owned_fields);
    free(merged_events);
    return ok;
}

bool runtime_post_input_combo_event_batch(Runtime* rt,
                                          const char* callback_name,
                                          const RuntimePostedInputEvent* events,
                                          const RuntimePostedInputComboPayloadSpec* payload_specs,
                                          const RuntimePostedEventContextSpec* context_specs,
                                          int event_count,
                                          char* error_buf,
                                          size_t error_buf_size) {
    RuntimePostedInputEvent* merged_events = NULL;
    RuntimePostedEventExtraField** owned_fields = NULL;
    RuntimePostedEventExtraField* generated_fields = NULL;
    RuntimePostedEventExtraMapEntry* payload_entries = NULL;
    RuntimePostedEventExtraMapEntry* context_entries = NULL;
    bool ok = false;

    if (event_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Input event batch count is invalid");
        return false;
    }
    if (event_count > 0 && !events) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Input event batch payload is required");
        return false;
    }
    if (event_count == 0) {
        return runtime_post_input_event_batch(rt, callback_name, events, event_count, error_buf, error_buf_size);
    }

    merged_events = (RuntimePostedInputEvent*)calloc((size_t)event_count, sizeof(RuntimePostedInputEvent));
    owned_fields = (RuntimePostedEventExtraField**)calloc((size_t)event_count, sizeof(RuntimePostedEventExtraField*));
    generated_fields = (RuntimePostedEventExtraField*)calloc((size_t)event_count * 2u, sizeof(RuntimePostedEventExtraField));
    payload_entries = (RuntimePostedEventExtraMapEntry*)calloc((size_t)event_count * 2u, sizeof(RuntimePostedEventExtraMapEntry));
    context_entries = (RuntimePostedEventExtraMapEntry*)calloc((size_t)event_count * 2u, sizeof(RuntimePostedEventExtraMapEntry));
    if (!merged_events || !owned_fields || !generated_fields || !payload_entries || !context_entries) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while preparing input event batch");
        goto cleanup;
    }

    for (int i = 0; i < event_count; i++) {
        RuntimePostedEventExtraField* generated = generated_fields + (i * 2);
        const RuntimePostedEventExtraField* merged_extra = NULL;
        int generated_count = 0;
        int merged_count = 0;

        merged_events[i] = events[i];
        if (payload_specs) {
            generated[generated_count++] = runtime_posted_event_build_input_combo_payload_field(payload_entries + (i * 2),
                                                                                                2,
                                                                                                &payload_specs[i]);
        }
        if (context_specs) {
            generated[generated_count++] = runtime_posted_event_build_context_field(context_entries + (i * 2),
                                                                                    2,
                                                                                    &context_specs[i]);
        }
        if (!runtime_prepare_augmented_extra_fields(events[i].extra_fields,
                                                    events[i].extra_field_count,
                                                    generated,
                                                    generated_count,
                                                    &owned_fields[i],
                                                    &merged_extra,
                                                    &merged_count,
                                                    error_buf,
                                                    error_buf_size)) {
            goto cleanup;
        }
        merged_events[i].extra_fields = merged_extra;
        merged_events[i].extra_field_count = merged_count;
    }

    ok = runtime_post_input_event_batch(rt,
                                        callback_name,
                                        merged_events,
                                        event_count,
                                        error_buf,
                                        error_buf_size);

cleanup:
    runtime_free_owned_extra_field_arrays(owned_fields, event_count);
    free(context_entries);
    free(payload_entries);
    free(generated_fields);
    free(owned_fields);
    free(merged_events);
    return ok;
}

bool runtime_post_window_event_batch(Runtime* rt,
                                     const char* callback_name,
                                     const RuntimePostedWindowEvent* events,
                                     int event_count,
                                     char* error_buf,
                                     size_t error_buf_size) {
    RuntimePostedEventBatchItem* items = NULL;
    bool ok = false;
    int prepared_count = 0;

    if (event_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Window event batch count is invalid");
        return false;
    }
    if (event_count > 0 && !events) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Window event batch payload is required");
        return false;
    }

    if (event_count > 0) {
        items = (RuntimePostedEventBatchItem*)calloc((size_t)event_count, sizeof(RuntimePostedEventBatchItem));
        if (!items) {
            runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while preparing window event batch");
            return false;
        }
        for (int i = 0; i < event_count; i++) {
            items[i].kind = RUNTIME_POSTED_EVENT_WINDOW;
            items[i].text_value = (char*)(events[i].event_name ? events[i].event_name : "");
            items[i].int_value_a = events[i].width;
            items[i].int_value_b = events[i].height;
            items[i].bool_value = events[i].focused;
            if (!runtime_clone_posted_event_meta(&items[i].meta,
                                                 events[i].meta_override,
                                                 error_buf,
                                                 error_buf_size) ||
                !runtime_clone_posted_event_extra_state(&items[i].extra,
                                                        RUNTIME_POSTED_EVENT_WINDOW,
                                                        events[i].extra_fields,
                                                        events[i].extra_field_count,
                                                 error_buf,
                                                 error_buf_size)) {
                prepared_count = i + 1;
                goto cleanup;
            }
            prepared_count = i + 1;
        }
    }

    ok = runtime_post_event_batch(rt,
                                  callback_name,
                                  items,
                                  event_count,
                                  error_buf,
                                  error_buf_size);
cleanup:
    if (items) {
        for (int i = 0; i < prepared_count; i++) {
            runtime_posted_event_meta_state_free(&items[i].meta);
            runtime_posted_event_extra_state_free(&items[i].extra);
        }
    }
    free(items);
    return ok;
}

bool runtime_post_window_rect_event_batch(Runtime* rt,
                                          const char* callback_name,
                                          const RuntimePostedWindowEvent* events,
                                          const RuntimePostedWindowRectPayloadSpec* payload_specs,
                                          const RuntimePostedEventContextSpec* context_specs,
                                          int event_count,
                                          char* error_buf,
                                          size_t error_buf_size) {
    RuntimePostedWindowEvent* merged_events = NULL;
    RuntimePostedEventExtraField** owned_fields = NULL;
    RuntimePostedEventExtraField* generated_fields = NULL;
    RuntimePostedEventExtraMapEntry* payload_entries = NULL;
    RuntimePostedEventExtraMapEntry* context_entries = NULL;
    bool ok = false;

    if (event_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Window event batch count is invalid");
        return false;
    }
    if (event_count > 0 && !events) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Window event batch payload is required");
        return false;
    }
    if (event_count == 0) {
        return runtime_post_window_event_batch(rt, callback_name, events, event_count, error_buf, error_buf_size);
    }

    merged_events = (RuntimePostedWindowEvent*)calloc((size_t)event_count, sizeof(RuntimePostedWindowEvent));
    owned_fields = (RuntimePostedEventExtraField**)calloc((size_t)event_count, sizeof(RuntimePostedEventExtraField*));
    generated_fields = (RuntimePostedEventExtraField*)calloc((size_t)event_count * 2u, sizeof(RuntimePostedEventExtraField));
    payload_entries = (RuntimePostedEventExtraMapEntry*)calloc((size_t)event_count * 2u, sizeof(RuntimePostedEventExtraMapEntry));
    context_entries = (RuntimePostedEventExtraMapEntry*)calloc((size_t)event_count * 2u, sizeof(RuntimePostedEventExtraMapEntry));
    if (!merged_events || !owned_fields || !generated_fields || !payload_entries || !context_entries) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while preparing window event batch");
        goto cleanup;
    }

    for (int i = 0; i < event_count; i++) {
        RuntimePostedEventExtraField* generated = generated_fields + (i * 2);
        const RuntimePostedEventExtraField* merged_extra = NULL;
        int generated_count = 0;
        int merged_count = 0;

        merged_events[i] = events[i];
        if (payload_specs) {
            generated[generated_count++] = runtime_posted_event_build_window_rect_payload_field(payload_entries + (i * 2),
                                                                                                2,
                                                                                                &payload_specs[i]);
        }
        if (context_specs) {
            generated[generated_count++] = runtime_posted_event_build_context_field(context_entries + (i * 2),
                                                                                    2,
                                                                                    &context_specs[i]);
        }
        if (!runtime_prepare_augmented_extra_fields(events[i].extra_fields,
                                                    events[i].extra_field_count,
                                                    generated,
                                                    generated_count,
                                                    &owned_fields[i],
                                                    &merged_extra,
                                                    &merged_count,
                                                    error_buf,
                                                    error_buf_size)) {
            goto cleanup;
        }
        merged_events[i].extra_fields = merged_extra;
        merged_events[i].extra_field_count = merged_count;
    }

    ok = runtime_post_window_event_batch(rt,
                                         callback_name,
                                         merged_events,
                                         event_count,
                                         error_buf,
                                         error_buf_size);

cleanup:
    runtime_free_owned_extra_field_arrays(owned_fields, event_count);
    free(context_entries);
    free(payload_entries);
    free(generated_fields);
    free(owned_fields);
    free(merged_events);
    return ok;
}

bool runtime_post_frame_event_batch(Runtime* rt,
                                    const char* callback_name,
                                    const RuntimePostedFrameEvent* events,
                                    int event_count,
                                    char* error_buf,
                                    size_t error_buf_size) {
    RuntimePostedEventBatchItem* items = NULL;
    bool ok = false;
    int prepared_count = 0;

    if (event_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Frame event batch count is invalid");
        return false;
    }
    if (event_count > 0 && !events) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Frame event batch payload is required");
        return false;
    }

    if (event_count > 0) {
        items = (RuntimePostedEventBatchItem*)calloc((size_t)event_count, sizeof(RuntimePostedEventBatchItem));
        if (!items) {
            runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while preparing frame event batch");
            return false;
        }
        for (int i = 0; i < event_count; i++) {
            items[i].kind = RUNTIME_POSTED_EVENT_FRAME;
            items[i].text_value = (char*)(events[i].phase ? events[i].phase : "");
            items[i].int_value_a = events[i].frame_number;
            items[i].double_value = events[i].delta_seconds;
            if (!runtime_clone_posted_event_meta(&items[i].meta,
                                                 events[i].meta_override,
                                                 error_buf,
                                                 error_buf_size) ||
                !runtime_clone_posted_event_extra_state(&items[i].extra,
                                                        RUNTIME_POSTED_EVENT_FRAME,
                                                        events[i].extra_fields,
                                                        events[i].extra_field_count,
                                                 error_buf,
                                                 error_buf_size)) {
                prepared_count = i + 1;
                goto cleanup;
            }
            prepared_count = i + 1;
        }
    }

    ok = runtime_post_event_batch(rt,
                                  callback_name,
                                  items,
                                  event_count,
                                  error_buf,
                                  error_buf_size);
cleanup:
    if (items) {
        for (int i = 0; i < prepared_count; i++) {
            runtime_posted_event_meta_state_free(&items[i].meta);
            runtime_posted_event_extra_state_free(&items[i].extra);
        }
    }
    free(items);
    return ok;
}

bool runtime_post_frame_marker_event_batch(Runtime* rt,
                                           const char* callback_name,
                                           const RuntimePostedFrameEvent* events,
                                           const RuntimePostedFrameMarkerPayloadSpec* payload_specs,
                                           const RuntimePostedEventContextSpec* context_specs,
                                           int event_count,
                                           char* error_buf,
                                           size_t error_buf_size) {
    RuntimePostedFrameEvent* merged_events = NULL;
    RuntimePostedEventExtraField** owned_fields = NULL;
    RuntimePostedEventExtraField* generated_fields = NULL;
    RuntimePostedEventExtraMapEntry* payload_entries = NULL;
    RuntimePostedEventExtraMapEntry* context_entries = NULL;
    bool ok = false;

    if (event_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Frame event batch count is invalid");
        return false;
    }
    if (event_count > 0 && !events) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Frame event batch payload is required");
        return false;
    }
    if (event_count == 0) {
        return runtime_post_frame_event_batch(rt, callback_name, events, event_count, error_buf, error_buf_size);
    }

    merged_events = (RuntimePostedFrameEvent*)calloc((size_t)event_count, sizeof(RuntimePostedFrameEvent));
    owned_fields = (RuntimePostedEventExtraField**)calloc((size_t)event_count, sizeof(RuntimePostedEventExtraField*));
    generated_fields = (RuntimePostedEventExtraField*)calloc((size_t)event_count * 2u, sizeof(RuntimePostedEventExtraField));
    payload_entries = (RuntimePostedEventExtraMapEntry*)calloc((size_t)event_count * 2u, sizeof(RuntimePostedEventExtraMapEntry));
    context_entries = (RuntimePostedEventExtraMapEntry*)calloc((size_t)event_count * 2u, sizeof(RuntimePostedEventExtraMapEntry));
    if (!merged_events || !owned_fields || !generated_fields || !payload_entries || !context_entries) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while preparing frame event batch");
        goto cleanup;
    }

    for (int i = 0; i < event_count; i++) {
        RuntimePostedEventExtraField* generated = generated_fields + (i * 2);
        const RuntimePostedEventExtraField* merged_extra = NULL;
        int generated_count = 0;
        int merged_count = 0;

        merged_events[i] = events[i];
        if (payload_specs) {
            generated[generated_count++] = runtime_posted_event_build_frame_marker_payload_field(payload_entries + (i * 2),
                                                                                                 2,
                                                                                                 &payload_specs[i]);
        }
        if (context_specs) {
            generated[generated_count++] = runtime_posted_event_build_context_field(context_entries + (i * 2),
                                                                                    2,
                                                                                    &context_specs[i]);
        }
        if (!runtime_prepare_augmented_extra_fields(events[i].extra_fields,
                                                    events[i].extra_field_count,
                                                    generated,
                                                    generated_count,
                                                    &owned_fields[i],
                                                    &merged_extra,
                                                    &merged_count,
                                                    error_buf,
                                                    error_buf_size)) {
            goto cleanup;
        }
        merged_events[i].extra_fields = merged_extra;
        merged_events[i].extra_field_count = merged_count;
    }

    ok = runtime_post_frame_event_batch(rt,
                                        callback_name,
                                        merged_events,
                                        event_count,
                                        error_buf,
                                        error_buf_size);

cleanup:
    runtime_free_owned_extra_field_arrays(owned_fields, event_count);
    free(context_entries);
    free(payload_entries);
    free(generated_fields);
    free(owned_fields);
    free(merged_events);
    return ok;
}

bool runtime_post_typed_mixed_event_batch(Runtime* rt,
                                          const char* callback_name,
                                          const RuntimePostedTypedEvent* events,
                                          int event_count,
                                          char* error_buf,
                                          size_t error_buf_size) {
    RuntimePostedEvent* merged_events = NULL;
    RuntimePostedEventExtraField** owned_fields = NULL;
    RuntimePostedEventExtraField* generated_fields = NULL;
    RuntimePostedEventExtraMapEntry* payload_entries = NULL;
    RuntimePostedEventExtraMapEntry* context_entries = NULL;
    bool ok = false;

    if (event_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Typed mixed event batch count is invalid");
        return false;
    }
    if (event_count > 0 && !events) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Typed mixed event batch payload is required");
        return false;
    }
    if (event_count == 0) {
        return runtime_post_mixed_event_batch(rt, callback_name, NULL, 0, error_buf, error_buf_size);
    }

    merged_events = (RuntimePostedEvent*)calloc((size_t)event_count, sizeof(RuntimePostedEvent));
    owned_fields = (RuntimePostedEventExtraField**)calloc((size_t)event_count, sizeof(RuntimePostedEventExtraField*));
    generated_fields = (RuntimePostedEventExtraField*)calloc((size_t)event_count * 2u, sizeof(RuntimePostedEventExtraField));
    payload_entries = (RuntimePostedEventExtraMapEntry*)calloc((size_t)event_count * 5u, sizeof(RuntimePostedEventExtraMapEntry));
    context_entries = (RuntimePostedEventExtraMapEntry*)calloc((size_t)event_count * 2u, sizeof(RuntimePostedEventExtraMapEntry));
    if (!merged_events || !owned_fields || !generated_fields || !payload_entries || !context_entries) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while preparing typed mixed event batch");
        goto cleanup;
    }

    for (int i = 0; i < event_count; i++) {
        RuntimePostedEventExtraField* generated = generated_fields + (i * 2);
        const RuntimePostedEventExtraField* merged_extra = NULL;
        int generated_count = 0;
        int merged_count = 0;

        merged_events[i].kind = events[i].kind;
        switch (events[i].kind) {
            case RUNTIME_POSTED_EVENT_INPUT:
                merged_events[i].as.input = events[i].event.input;
                switch (events[i].payload_kind) {
                    case RUNTIME_POSTED_TYPED_PAYLOAD_NONE:
                        break;
                    case RUNTIME_POSTED_TYPED_PAYLOAD_INPUT_STATE:
                        generated[generated_count++] = runtime_posted_event_build_input_state_payload_field(payload_entries + (i * 5),
                                                                                                            5,
                                                                                                            &events[i].payload.input_state);
                        break;
                    case RUNTIME_POSTED_TYPED_PAYLOAD_INPUT_COMBO:
                        generated[generated_count++] = runtime_posted_event_build_input_combo_payload_field(payload_entries + (i * 5),
                                                                                                            2,
                                                                                                            &events[i].payload.input_combo);
                        break;
                    default:
                        runtime_set_error_buffer(error_buf,
                                                 error_buf_size,
                                                 "Typed mixed event batch contains a non-input payload preset for an input event");
                        goto cleanup;
                }
                if (events[i].has_context_spec) {
                    generated[generated_count++] = runtime_posted_event_build_context_field(context_entries + (i * 2),
                                                                                            2,
                                                                                            &events[i].context_spec);
                }
                if (!runtime_prepare_augmented_extra_fields(events[i].event.input.extra_fields,
                                                            events[i].event.input.extra_field_count,
                                                            generated,
                                                            generated_count,
                                                            &owned_fields[i],
                                                            &merged_extra,
                                                            &merged_count,
                                                            error_buf,
                                                            error_buf_size)) {
                    goto cleanup;
                }
                merged_events[i].as.input.extra_fields = merged_extra;
                merged_events[i].as.input.extra_field_count = merged_count;
                break;
            case RUNTIME_POSTED_EVENT_WINDOW:
                merged_events[i].as.window = events[i].event.window;
                switch (events[i].payload_kind) {
                    case RUNTIME_POSTED_TYPED_PAYLOAD_NONE:
                        break;
                    case RUNTIME_POSTED_TYPED_PAYLOAD_WINDOW_RECT:
                        generated[generated_count++] = runtime_posted_event_build_window_rect_payload_field(payload_entries + (i * 5),
                                                                                                            2,
                                                                                                            &events[i].payload.window_rect);
                        break;
                    default:
                        runtime_set_error_buffer(error_buf,
                                                 error_buf_size,
                                                 "Typed mixed event batch contains a non-window payload preset for a window event");
                        goto cleanup;
                }
                if (events[i].has_context_spec) {
                    generated[generated_count++] = runtime_posted_event_build_context_field(context_entries + (i * 2),
                                                                                            2,
                                                                                            &events[i].context_spec);
                }
                if (!runtime_prepare_augmented_extra_fields(events[i].event.window.extra_fields,
                                                            events[i].event.window.extra_field_count,
                                                            generated,
                                                            generated_count,
                                                            &owned_fields[i],
                                                            &merged_extra,
                                                            &merged_count,
                                                            error_buf,
                                                            error_buf_size)) {
                    goto cleanup;
                }
                merged_events[i].as.window.extra_fields = merged_extra;
                merged_events[i].as.window.extra_field_count = merged_count;
                break;
            case RUNTIME_POSTED_EVENT_FRAME:
                merged_events[i].as.frame = events[i].event.frame;
                switch (events[i].payload_kind) {
                    case RUNTIME_POSTED_TYPED_PAYLOAD_NONE:
                        break;
                    case RUNTIME_POSTED_TYPED_PAYLOAD_FRAME_MARKER:
                        generated[generated_count++] = runtime_posted_event_build_frame_marker_payload_field(payload_entries + (i * 5),
                                                                                                             2,
                                                                                                             &events[i].payload.frame_marker);
                        break;
                    default:
                        runtime_set_error_buffer(error_buf,
                                                 error_buf_size,
                                                 "Typed mixed event batch contains a non-frame payload preset for a frame event");
                        goto cleanup;
                }
                if (events[i].has_context_spec) {
                    generated[generated_count++] = runtime_posted_event_build_context_field(context_entries + (i * 2),
                                                                                            2,
                                                                                            &events[i].context_spec);
                }
                if (!runtime_prepare_augmented_extra_fields(events[i].event.frame.extra_fields,
                                                            events[i].event.frame.extra_field_count,
                                                            generated,
                                                            generated_count,
                                                            &owned_fields[i],
                                                            &merged_extra,
                                                            &merged_count,
                                                            error_buf,
                                                            error_buf_size)) {
                    goto cleanup;
                }
                merged_events[i].as.frame.extra_fields = merged_extra;
                merged_events[i].as.frame.extra_field_count = merged_count;
                break;
            default:
                runtime_set_error_buffer(error_buf, error_buf_size, "Typed mixed event batch contains an unsupported event kind");
                goto cleanup;
        }
    }

    ok = runtime_post_mixed_event_batch(rt,
                                        callback_name,
                                        merged_events,
                                        event_count,
                                        error_buf,
                                        error_buf_size);

cleanup:
    runtime_free_owned_extra_field_arrays(owned_fields, event_count);
    free(context_entries);
    free(payload_entries);
    free(generated_fields);
    free(owned_fields);
    free(merged_events);
    return ok;
}

static bool runtime_post_typed_family_mixed_event_batch_internal(Runtime* rt,
                                                                 const char* callback_name,
                                                                 const RuntimePostedTypedInputBatchSpec* input_batch,
                                                                 const RuntimePostedTypedWindowBatchSpec* window_batch,
                                                                 const RuntimePostedTypedFrameBatchSpec* frame_batch,
                                                                 const RuntimePostedEventContextSpec* shared_context,
                                                                 char* error_buf,
                                                                 size_t error_buf_size) {
    int input_count = input_batch ? input_batch->event_count : 0;
    int window_count = window_batch ? window_batch->event_count : 0;
    int frame_count = frame_batch ? frame_batch->event_count : 0;
    int total_count = 0;
    RuntimePostedTypedEvent* events = NULL;
    bool ok = false;
    int out_index = 0;

    if (input_count < 0 || window_count < 0 || frame_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Typed family mixed event batch count is invalid");
        return false;
    }
    if (input_count > 0 && (!input_batch || !input_batch->events)) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Typed family mixed input batch payload is required");
        return false;
    }
    if (window_count > 0 && (!window_batch || !window_batch->events)) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Typed family mixed window batch payload is required");
        return false;
    }
    if (frame_count > 0 && (!frame_batch || !frame_batch->events)) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Typed family mixed frame batch payload is required");
        return false;
    }

    total_count = input_count + window_count + frame_count;
    if (total_count == 0) {
        return runtime_post_typed_mixed_event_batch(rt, callback_name, NULL, 0, error_buf, error_buf_size);
    }

    events = (RuntimePostedTypedEvent*)calloc((size_t)total_count, sizeof(RuntimePostedTypedEvent));
    if (!events) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while preparing typed family mixed event batch");
        return false;
    }

    if (input_batch) {
        for (int i = 0; i < input_count; i++) {
            const RuntimePostedEventContextSpec* context_spec = input_batch->context_specs ? &input_batch->context_specs[i] : shared_context;
            switch (input_batch->payload_kind) {
                case RUNTIME_POSTED_TYPED_PAYLOAD_NONE:
                    events[out_index] = runtime_posted_typed_input_event(&input_batch->events[i]);
                    if (context_spec) {
                        events[out_index].has_context_spec = true;
                        events[out_index].context_spec = *context_spec;
                    }
                    break;
                case RUNTIME_POSTED_TYPED_PAYLOAD_INPUT_STATE:
                    events[out_index] = runtime_posted_typed_input_state_event(&input_batch->events[i],
                                                                              input_batch->payload_specs.input_state ? &input_batch->payload_specs.input_state[i] : NULL,
                                                                              context_spec);
                    break;
                case RUNTIME_POSTED_TYPED_PAYLOAD_INPUT_COMBO:
                    events[out_index] = runtime_posted_typed_input_combo_event(&input_batch->events[i],
                                                                              input_batch->payload_specs.input_combo ? &input_batch->payload_specs.input_combo[i] : NULL,
                                                                              context_spec);
                    break;
                default:
                    runtime_set_error_buffer(error_buf,
                                             error_buf_size,
                                             "Typed family mixed input batch contains an unsupported payload preset");
                    goto cleanup;
            }
            out_index++;
        }
    }

    if (window_batch) {
        for (int i = 0; i < window_count; i++) {
            const RuntimePostedEventContextSpec* context_spec = window_batch->context_specs ? &window_batch->context_specs[i] : shared_context;
            switch (window_batch->payload_kind) {
                case RUNTIME_POSTED_TYPED_PAYLOAD_NONE:
                    events[out_index] = runtime_posted_typed_window_event(&window_batch->events[i]);
                    if (context_spec) {
                        events[out_index].has_context_spec = true;
                        events[out_index].context_spec = *context_spec;
                    }
                    break;
                case RUNTIME_POSTED_TYPED_PAYLOAD_WINDOW_RECT:
                    events[out_index] = runtime_posted_typed_window_rect_event(&window_batch->events[i],
                                                                               window_batch->payload_specs.window_rect ? &window_batch->payload_specs.window_rect[i] : NULL,
                                                                               context_spec);
                    break;
                default:
                    runtime_set_error_buffer(error_buf,
                                             error_buf_size,
                                             "Typed family mixed window batch contains an unsupported payload preset");
                    goto cleanup;
            }
            out_index++;
        }
    }

    if (frame_batch) {
        for (int i = 0; i < frame_count; i++) {
            const RuntimePostedEventContextSpec* context_spec = frame_batch->context_specs ? &frame_batch->context_specs[i] : shared_context;
            switch (frame_batch->payload_kind) {
                case RUNTIME_POSTED_TYPED_PAYLOAD_NONE:
                    events[out_index] = runtime_posted_typed_frame_event(&frame_batch->events[i]);
                    if (context_spec) {
                        events[out_index].has_context_spec = true;
                        events[out_index].context_spec = *context_spec;
                    }
                    break;
                case RUNTIME_POSTED_TYPED_PAYLOAD_FRAME_MARKER:
                    events[out_index] = runtime_posted_typed_frame_marker_event(&frame_batch->events[i],
                                                                                frame_batch->payload_specs.frame_marker ? &frame_batch->payload_specs.frame_marker[i] : NULL,
                                                                                context_spec);
                    break;
                default:
                    runtime_set_error_buffer(error_buf,
                                             error_buf_size,
                                             "Typed family mixed frame batch contains an unsupported payload preset");
                    goto cleanup;
            }
            out_index++;
        }
    }

    ok = runtime_post_typed_mixed_event_batch(rt,
                                              callback_name,
                                              events,
                                              total_count,
                                              error_buf,
                                              error_buf_size);

cleanup:
    free(events);
    return ok;
}

bool runtime_post_typed_family_mixed_event_batch(Runtime* rt,
                                                 const char* callback_name,
                                                 const RuntimePostedTypedInputBatchSpec* input_batch,
                                                 const RuntimePostedTypedWindowBatchSpec* window_batch,
                                                 const RuntimePostedTypedFrameBatchSpec* frame_batch,
                                                 char* error_buf,
                                                 size_t error_buf_size) {
    return runtime_post_typed_family_mixed_event_batch_internal(rt,
                                                                callback_name,
                                                                input_batch,
                                                                window_batch,
                                                                frame_batch,
                                                                NULL,
                                                                error_buf,
                                                                error_buf_size);
}

bool runtime_post_frame_envelope_batch(Runtime* rt,
                                       const char* callback_name,
                                       const RuntimePostedFrameEnvelopeBatchSpec* envelope,
                                       char* error_buf,
                                       size_t error_buf_size) {
    if (!envelope) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Frame envelope payload is required");
        return false;
    }
    return runtime_post_typed_family_mixed_event_batch_internal(rt,
                                                                callback_name,
                                                                envelope->input_batch,
                                                                envelope->window_batch,
                                                                envelope->frame_batch,
                                                                envelope->has_shared_context ? &envelope->shared_context : NULL,
                                                                error_buf,
                                                                error_buf_size);
}

void runtime_posted_frame_envelope_heap_builder_init(RuntimePostedFrameEnvelopeHeapBuilder* builder) {
    if (!builder) return;
    memset(builder, 0, sizeof(*builder));
}

void runtime_posted_frame_envelope_heap_builder_free(RuntimePostedFrameEnvelopeHeapBuilder* builder) {
    if (!builder) return;
    free(builder->events);
    memset(builder, 0, sizeof(*builder));
}

void runtime_posted_frame_envelope_heap_builder_clear(RuntimePostedFrameEnvelopeHeapBuilder* builder) {
    if (!builder) return;
    builder->count = 0;
    builder->has_shared_context = false;
    memset(&builder->shared_context, 0, sizeof(builder->shared_context));
}

void runtime_posted_frame_envelope_heap_builder_set_shared_context(RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                                                   const RuntimePostedEventContextSpec* shared_context) {
    if (!builder) return;
    if (shared_context) {
        builder->has_shared_context = true;
        builder->shared_context = *shared_context;
    } else {
        builder->has_shared_context = false;
        memset(&builder->shared_context, 0, sizeof(builder->shared_context));
    }
}

bool runtime_posted_frame_envelope_heap_builder_reserve(RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                                        int min_capacity) {
    RuntimePostedTypedEvent* new_events = NULL;
    int new_capacity = 0;

    if (!builder || min_capacity < 0) {
        return false;
    }
    if (min_capacity <= builder->capacity) {
        return true;
    }

    new_capacity = builder->capacity > 0 ? builder->capacity : 2;
    while (new_capacity < min_capacity) {
        if (new_capacity > INT_MAX / 2) {
            new_capacity = min_capacity;
            break;
        }
        new_capacity *= 2;
    }

    new_events = (RuntimePostedTypedEvent*)realloc(builder->events, (size_t)new_capacity * sizeof(RuntimePostedTypedEvent));
    if (!new_events) {
        return false;
    }

    builder->events = new_events;
    builder->capacity = new_capacity;
    return true;
}

bool runtime_posted_frame_envelope_heap_builder_add_typed_event(RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                                                RuntimePostedTypedEvent event) {
    if (!builder || builder->count < 0) {
        return false;
    }
    if (builder->count >= builder->capacity &&
        !runtime_posted_frame_envelope_heap_builder_reserve(builder, builder->count + 1)) {
        return false;
    }
    builder->events[builder->count++] = event;
    return true;
}

void runtime_host_event_loop_session_init(RuntimeHostEventLoopSession* session, Runtime* rt) {
    if (!session) return;
    memset(session, 0, sizeof(*session));
    session->runtime = rt;
    session->default_max_callbacks = 1;
    session->default_tick_mode = RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_NO_WAIT;
    runtime_posted_frame_envelope_heap_builder_init(&session->frame_builder);
}

void runtime_host_event_loop_session_free(RuntimeHostEventLoopSession* session) {
    if (!session) return;
    free(session->default_frame_callback_name);
    session->default_frame_callback_name = NULL;
    runtime_posted_frame_envelope_heap_builder_free(&session->frame_builder);
    session->runtime = NULL;
}

void runtime_host_event_loop_session_begin_frame(RuntimeHostEventLoopSession* session,
                                                 const RuntimePostedEventContextSpec* shared_context) {
    if (!session) return;
    runtime_posted_frame_envelope_heap_builder_clear(&session->frame_builder);
    runtime_posted_frame_envelope_heap_builder_set_shared_context(&session->frame_builder, shared_context);
}

void runtime_host_event_loop_session_clear_frame(RuntimeHostEventLoopSession* session) {
    if (!session) return;
    runtime_posted_frame_envelope_heap_builder_clear(&session->frame_builder);
}

bool runtime_host_event_loop_session_configure_end_frame(RuntimeHostEventLoopSession* session,
                                                         const char* callback_name,
                                                         int max_callbacks,
                                                         int64_t max_millis,
                                                         char* error_buf,
                                                         size_t error_buf_size) {
    char* callback_copy = NULL;

    if (!session || !session->runtime) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session is not initialized");
        return false;
    }
    if (!callback_name || callback_name[0] == '\0') {
        runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session frame callback name is required");
        return false;
    }
    if (max_callbacks < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session max callback count is invalid");
        return false;
    }

    callback_copy = runtime_strdup_fallible(callback_name);
    if (!callback_copy) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while copying host event loop session callback name");
        return false;
    }

    free(session->default_frame_callback_name);
    session->default_frame_callback_name = callback_copy;
    session->default_max_callbacks = max_callbacks;
    session->default_max_millis = max_millis;
    return true;
}

void runtime_host_event_loop_session_clear_end_frame(RuntimeHostEventLoopSession* session) {
    if (!session) return;
    free(session->default_frame_callback_name);
    session->default_frame_callback_name = NULL;
    session->default_max_callbacks = 0;
    session->default_max_millis = 0;
}

bool runtime_host_event_loop_session_configure_tick(RuntimeHostEventLoopSession* session,
                                                    RuntimeHostEventLoopSessionTickMode mode,
                                                    int64_t wait_timeout_millis,
                                                    char* error_buf,
                                                    size_t error_buf_size) {
    if (!session || !session->runtime) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session is not initialized");
        return false;
    }
    if (mode < RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_NO_WAIT ||
        mode > RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_WAIT_AND_PUMP_UNTIL_BUDGET) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session tick mode is invalid");
        return false;
    }
    if (wait_timeout_millis < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session tick wait timeout is invalid");
        return false;
    }

    session->default_tick_mode = mode;
    session->default_tick_wait_timeout_millis = wait_timeout_millis;
    return true;
}

void runtime_host_event_loop_session_clear_tick(RuntimeHostEventLoopSession* session) {
    if (!session) return;
    session->default_tick_mode = RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_NO_WAIT;
    session->default_tick_wait_timeout_millis = 0;
}

bool runtime_host_event_loop_session_has_posted_callbacks(const RuntimeHostEventLoopSession* session) {
    return session && session->runtime && runtime_has_posted_callbacks(session->runtime);
}

int runtime_host_event_loop_session_posted_callback_pending_count(const RuntimeHostEventLoopSession* session) {
    return (session && session->runtime) ? runtime_posted_callback_pending_count(session->runtime) : 0;
}

int runtime_host_event_loop_session_drain_posted_callbacks(RuntimeHostEventLoopSession* session,
                                                           int max_callbacks) {
    return (session && session->runtime) ? runtime_drain_posted_callbacks(session->runtime, max_callbacks) : 0;
}

int runtime_host_event_loop_session_drain_posted_callbacks_for_ms(RuntimeHostEventLoopSession* session,
                                                                  int max_callbacks,
                                                                  int64_t max_millis) {
    return (session && session->runtime) ? runtime_drain_posted_callbacks_for_ms(session->runtime, max_callbacks, max_millis) : 0;
}

bool runtime_host_event_loop_session_wait_for_posted_callbacks(RuntimeHostEventLoopSession* session,
                                                               int64_t timeout_millis) {
    return session && session->runtime && runtime_wait_for_posted_callbacks(session->runtime, timeout_millis);
}

int runtime_host_event_loop_session_wait_and_drain_posted_callbacks(RuntimeHostEventLoopSession* session,
                                                                    int max_callbacks,
                                                                    int64_t timeout_millis) {
    return (session && session->runtime) ? runtime_wait_and_drain_posted_callbacks(session->runtime, max_callbacks, timeout_millis) : 0;
}

bool runtime_host_event_loop_session_add_typed_event(RuntimeHostEventLoopSession* session,
                                                     RuntimePostedTypedEvent event) {
    return session && runtime_posted_frame_envelope_heap_builder_add_typed_event(&session->frame_builder, event);
}

bool runtime_post_frame_envelope_builder(Runtime* rt,
                                         const char* callback_name,
                                         const RuntimePostedFrameEnvelopeBuilder* builder,
                                         char* error_buf,
                                         size_t error_buf_size) {
    RuntimePostedTypedEvent* events = NULL;
    bool ok = false;

    if (!builder) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Frame envelope builder is required");
        return false;
    }
    if (builder->count < 0 || builder->count > builder->capacity) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Frame envelope builder count is invalid");
        return false;
    }
    if (builder->count > 0 && !builder->events) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Frame envelope builder payload is required");
        return false;
    }
    if (builder->count == 0) {
        return runtime_post_typed_mixed_event_batch(rt, callback_name, NULL, 0, error_buf, error_buf_size);
    }

    if (builder->has_shared_context) {
        events = (RuntimePostedTypedEvent*)calloc((size_t)builder->count, sizeof(RuntimePostedTypedEvent));
        if (!events) {
            runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while preparing frame envelope builder");
            return false;
        }
        memcpy(events, builder->events, (size_t)builder->count * sizeof(RuntimePostedTypedEvent));
        for (int i = 0; i < builder->count; i++) {
            if (!events[i].has_context_spec) {
                events[i].has_context_spec = true;
                events[i].context_spec = builder->shared_context;
            }
        }
    }

    ok = runtime_post_typed_mixed_event_batch(rt,
                                              callback_name,
                                              events ? events : builder->events,
                                              builder->count,
                                              error_buf,
                                              error_buf_size);
    free(events);
    return ok;
}

bool runtime_post_frame_envelope_heap_builder(Runtime* rt,
                                              const char* callback_name,
                                              const RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                              char* error_buf,
                                              size_t error_buf_size) {
    RuntimePostedFrameEnvelopeBuilder builder_view;

    if (!builder) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Frame envelope heap builder is required");
        return false;
    }

    builder_view.events = builder->events;
    builder_view.capacity = builder->capacity;
    builder_view.count = builder->count;
    builder_view.has_shared_context = builder->has_shared_context;
    builder_view.shared_context = builder->shared_context;
    return runtime_post_frame_envelope_builder(rt,
                                               callback_name,
                                               &builder_view,
                                               error_buf,
                                               error_buf_size);
}

bool runtime_host_event_loop_session_flush_frame(RuntimeHostEventLoopSession* session,
                                                 const char* callback_name,
                                                 char* error_buf,
                                                 size_t error_buf_size) {
    bool ok = false;

    if (!session || !session->runtime) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session is not initialized");
        return false;
    }

    ok = runtime_post_frame_envelope_heap_builder(session->runtime,
                                                  callback_name,
                                                  &session->frame_builder,
                                                  error_buf,
                                                  error_buf_size);
    if (ok) {
        runtime_posted_frame_envelope_heap_builder_clear(&session->frame_builder);
    }
    return ok;
}

bool runtime_host_event_loop_session_flush_frame_and_drain_posted_callbacks_for_ms(
    RuntimeHostEventLoopSession* session,
    const char* callback_name,
    int max_callbacks,
    int64_t max_millis,
    int* drained_callbacks,
    char* error_buf,
    size_t error_buf_size) {
    const char* runtime_error = NULL;

    if (drained_callbacks) {
        *drained_callbacks = 0;
    }
    if (!runtime_host_event_loop_session_flush_frame(session,
                                                     callback_name,
                                                     error_buf,
                                                     error_buf_size)) {
        return false;
    }

    if (drained_callbacks) {
        *drained_callbacks = runtime_host_event_loop_session_drain_posted_callbacks_for_ms(session,
                                                                                            max_callbacks,
                                                                                            max_millis);
    } else {
        (void)runtime_host_event_loop_session_drain_posted_callbacks_for_ms(session,
                                                                            max_callbacks,
                                                                            max_millis);
    }

    if (session && session->runtime && runtime_has_error(session->runtime)) {
        runtime_error = runtime_get_error(session->runtime);
        runtime_set_error_buffer(error_buf,
                                 error_buf_size,
                                 "%s",
                                 runtime_error ? runtime_error : "Runtime callback drain failed");
        return false;
    }

    return true;
}

bool runtime_host_event_loop_session_end_frame(RuntimeHostEventLoopSession* session,
                                               int* drained_callbacks,
                                               char* error_buf,
                                               size_t error_buf_size) {
    if (!session || !session->runtime) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session is not initialized");
        return false;
    }
    if (!session->default_frame_callback_name || session->default_frame_callback_name[0] == '\0') {
        runtime_set_error_buffer(error_buf,
                                 error_buf_size,
                                 "Host event loop session end-frame callback is not configured");
        return false;
    }

    return runtime_host_event_loop_session_flush_frame_and_drain_posted_callbacks_for_ms(
        session,
        session->default_frame_callback_name,
        session->default_max_callbacks,
        session->default_max_millis,
        drained_callbacks,
        error_buf,
        error_buf_size);
}

bool runtime_host_event_loop_session_tick(RuntimeHostEventLoopSession* session,
                                          bool wait_for_callbacks,
                                          int64_t wait_timeout_millis,
                                          int* drained_callbacks,
                                          char* error_buf,
                                          size_t error_buf_size) {
    const char* runtime_error = NULL;

    if (drained_callbacks) {
        *drained_callbacks = 0;
    }
    if (!session || !session->runtime) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session is not initialized");
        return false;
    }
    if (!session->default_frame_callback_name || session->default_frame_callback_name[0] == '\0') {
        runtime_set_error_buffer(error_buf,
                                 error_buf_size,
                                 "Host event loop session end-frame defaults are not configured");
        return false;
    }

    if (wait_for_callbacks && !runtime_host_event_loop_session_has_posted_callbacks(session)) {
        (void)runtime_host_event_loop_session_wait_for_posted_callbacks(session, wait_timeout_millis);
    }

    if (drained_callbacks) {
        *drained_callbacks = runtime_host_event_loop_session_drain_posted_callbacks_for_ms(session,
                                                                                            session->default_max_callbacks,
                                                                                            session->default_max_millis);
    } else {
        (void)runtime_host_event_loop_session_drain_posted_callbacks_for_ms(session,
                                                                            session->default_max_callbacks,
                                                                            session->default_max_millis);
    }

    if (runtime_has_error(session->runtime)) {
        runtime_error = runtime_get_error(session->runtime);
        runtime_set_error_buffer(error_buf,
                                 error_buf_size,
                                 "%s",
                                 runtime_error ? runtime_error : "Runtime callback drain failed");
        return false;
    }

    return true;
}

bool runtime_host_event_loop_session_tick_default(RuntimeHostEventLoopSession* session,
                                                  int* drained_callbacks,
                                                  char* error_buf,
                                                  size_t error_buf_size) {
    const char* runtime_error = NULL;

    if (drained_callbacks) {
        *drained_callbacks = 0;
    }
    if (!session || !session->runtime) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session is not initialized");
        return false;
    }

    switch (session->default_tick_mode) {
        case RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_NO_WAIT:
            return runtime_host_event_loop_session_tick(session,
                                                        false,
                                                        0,
                                                        drained_callbacks,
                                                        error_buf,
                                                        error_buf_size);

        case RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_WAIT:
            if (drained_callbacks) {
                *drained_callbacks = runtime_host_event_loop_session_wait_and_drain_posted_callbacks(
                    session,
                    1,
                    session->default_tick_wait_timeout_millis);
            } else {
                (void)runtime_host_event_loop_session_wait_and_drain_posted_callbacks(
                    session,
                    1,
                    session->default_tick_wait_timeout_millis);
            }
            if (runtime_has_error(session->runtime)) {
                runtime_error = runtime_get_error(session->runtime);
                runtime_set_error_buffer(error_buf,
                                         error_buf_size,
                                         "%s",
                                         runtime_error ? runtime_error : "Runtime callback drain failed");
                return false;
            }
            return true;

        case RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_WAIT_AND_PUMP_UNTIL_BUDGET:
            return runtime_host_event_loop_session_tick(session,
                                                        true,
                                                        session->default_tick_wait_timeout_millis,
                                                        drained_callbacks,
                                                        error_buf,
                                                        error_buf_size);
    }

    runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session tick mode is invalid");
    return false;
}

bool runtime_host_event_loop_session_step_with_options(
    RuntimeHostEventLoopSession* session,
    bool post_frame,
    const RuntimeHostEventLoopSessionStepOptions* options,
    RuntimeHostEventLoopSessionStepResult* result,
    char* error_buf,
    size_t error_buf_size) {
    RuntimeHostEventLoopSessionStepResult step_result;
    const char* runtime_error = NULL;
    bool effective_post_frame = post_frame;
    const char* effective_frame_callback_name = NULL;
    int effective_max_callbacks = 0;
    int64_t effective_max_millis = 0;
    int64_t effective_wait_timeout_millis = 0;
    int drained = 0;
    bool force_wait = false;

    memset(&step_result, 0, sizeof(step_result));
    if (!session || !session->runtime) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session is not initialized");
        return false;
    }

    effective_max_callbacks = session->default_max_callbacks;
    effective_frame_callback_name = session->default_frame_callback_name;
    effective_max_millis = session->default_max_millis;
    effective_wait_timeout_millis = session->default_tick_wait_timeout_millis;
    if (options) {
        if (options->has_post_frame) {
            effective_post_frame = options->post_frame;
        }
        if (options->has_frame_callback_name) {
            if (!options->frame_callback_name || options->frame_callback_name[0] == '\0') {
                runtime_set_error_buffer(error_buf,
                                         error_buf_size,
                                         "Host event loop session step frame callback name is invalid");
                return false;
            }
            effective_frame_callback_name = options->frame_callback_name;
        }
        if (options->has_max_callbacks) {
            if (options->max_callbacks < 0) {
                runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session step max callback count is invalid");
                return false;
            }
            effective_max_callbacks = options->max_callbacks;
        }
        if (options->has_max_millis) {
            if (options->max_millis < 0) {
                runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session step max drain time is invalid");
                return false;
            }
            effective_max_millis = options->max_millis;
        }
        if (options->has_wait_timeout_millis) {
            if (options->wait_timeout_millis < 0) {
                runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session step wait timeout is invalid");
                return false;
            }
            effective_wait_timeout_millis = options->wait_timeout_millis;
            force_wait = true;
        }
    }

    if (!effective_post_frame && options && options->has_frame_callback_name) {
        runtime_set_error_buffer(error_buf,
                                 error_buf_size,
                                 "Host event loop session step frame callback override requires frame posting");
        return false;
    }

    if (effective_post_frame) {
        if (!effective_frame_callback_name || effective_frame_callback_name[0] == '\0') {
            runtime_set_error_buffer(error_buf,
                                     error_buf_size,
                                     "Host event loop session end-frame callback is not configured");
            return false;
        }
        if (!runtime_host_event_loop_session_flush_frame_and_drain_posted_callbacks_for_ms(
                session,
                effective_frame_callback_name,
                effective_max_callbacks,
                effective_max_millis,
                &drained,
                error_buf,
                error_buf_size)) {
            return false;
        }
        step_result.frame_posted = true;
        step_result.callbacks_drained = drained;
        if (result) {
            *result = step_result;
        }
        return true;
    }

    switch (session->default_tick_mode) {
        case RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_NO_WAIT:
            if (force_wait && !runtime_host_event_loop_session_has_posted_callbacks(session)) {
                (void)runtime_host_event_loop_session_wait_for_posted_callbacks(session,
                                                                                effective_wait_timeout_millis);
            }
            drained = runtime_host_event_loop_session_drain_posted_callbacks_for_ms(session,
                                                                                     effective_max_callbacks,
                                                                                     effective_max_millis);
            break;

        case RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_WAIT:
            if (!runtime_host_event_loop_session_has_posted_callbacks(session)) {
                (void)runtime_host_event_loop_session_wait_for_posted_callbacks(session,
                                                                                effective_wait_timeout_millis);
            }
            drained = runtime_host_event_loop_session_drain_posted_callbacks_for_ms(session,
                                                                                     effective_max_callbacks,
                                                                                     effective_max_millis);
            break;

        case RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_WAIT_AND_PUMP_UNTIL_BUDGET:
            if ((force_wait || effective_wait_timeout_millis > 0) &&
                !runtime_host_event_loop_session_has_posted_callbacks(session)) {
                (void)runtime_host_event_loop_session_wait_for_posted_callbacks(session,
                                                                                effective_wait_timeout_millis);
            }
            drained = runtime_host_event_loop_session_drain_posted_callbacks_for_ms(session,
                                                                                     effective_max_callbacks,
                                                                                     effective_max_millis);
            break;

        default:
            runtime_set_error_buffer(error_buf, error_buf_size, "Host event loop session tick mode is invalid");
            return false;
    }

    if (runtime_has_error(session->runtime)) {
        runtime_error = runtime_get_error(session->runtime);
        runtime_set_error_buffer(error_buf,
                                 error_buf_size,
                                 "%s",
                                 runtime_error ? runtime_error : "Runtime callback drain failed");
        return false;
    }

    step_result.callbacks_drained = drained;
    if (result) {
        *result = step_result;
    }
    return true;
}

bool runtime_host_event_loop_session_step(RuntimeHostEventLoopSession* session,
                                          bool post_frame,
                                          RuntimeHostEventLoopSessionStepResult* result,
                                          char* error_buf,
                                          size_t error_buf_size) {
    return runtime_host_event_loop_session_step_with_options(session,
                                                             post_frame,
                                                             NULL,
                                                             result,
                                                             error_buf,
                                                             error_buf_size);
}

bool runtime_post_mixed_event_batch(Runtime* rt,
                                    const char* callback_name,
                                    const RuntimePostedEvent* events,
                                    int event_count,
                                    char* error_buf,
                                    size_t error_buf_size) {
    RuntimePostedEventBatchItem* items = NULL;
    bool ok = false;
    int prepared_count = 0;

    if (event_count < 0) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Mixed event batch count is invalid");
        return false;
    }
    if (event_count > 0 && !events) {
        runtime_set_error_buffer(error_buf, error_buf_size, "Mixed event batch payload is required");
        return false;
    }

    if (event_count > 0) {
        items = (RuntimePostedEventBatchItem*)calloc((size_t)event_count, sizeof(RuntimePostedEventBatchItem));
        if (!items) {
            runtime_set_error_buffer(error_buf, error_buf_size, "Out of memory while preparing mixed event batch");
            return false;
        }
        for (int i = 0; i < event_count; i++) {
            items[i].kind = events[i].kind;
            switch (events[i].kind) {
                case RUNTIME_POSTED_EVENT_INPUT:
                    items[i].text_value = (char*)(events[i].as.input.device ? events[i].as.input.device : "");
                    items[i].int_value_a = events[i].as.input.code;
                    items[i].bool_value = events[i].as.input.pressed;
                    if (!runtime_clone_posted_event_meta(&items[i].meta,
                                                         events[i].as.input.meta_override,
                                                         error_buf,
                                                         error_buf_size) ||
                        !runtime_clone_posted_event_extra_state(&items[i].extra,
                                                                RUNTIME_POSTED_EVENT_INPUT,
                                                                events[i].as.input.extra_fields,
                                                                events[i].as.input.extra_field_count,
                                                         error_buf,
                                                         error_buf_size)) {
                        prepared_count = i + 1;
                        goto cleanup;
                    }
                    break;
                case RUNTIME_POSTED_EVENT_WINDOW:
                    items[i].text_value = (char*)(events[i].as.window.event_name ? events[i].as.window.event_name : "");
                    items[i].int_value_a = events[i].as.window.width;
                    items[i].int_value_b = events[i].as.window.height;
                    items[i].bool_value = events[i].as.window.focused;
                    if (!runtime_clone_posted_event_meta(&items[i].meta,
                                                         events[i].as.window.meta_override,
                                                         error_buf,
                                                         error_buf_size) ||
                        !runtime_clone_posted_event_extra_state(&items[i].extra,
                                                                RUNTIME_POSTED_EVENT_WINDOW,
                                                                events[i].as.window.extra_fields,
                                                                events[i].as.window.extra_field_count,
                                                         error_buf,
                                                         error_buf_size)) {
                        prepared_count = i + 1;
                        goto cleanup;
                    }
                    break;
                case RUNTIME_POSTED_EVENT_FRAME:
                    items[i].text_value = (char*)(events[i].as.frame.phase ? events[i].as.frame.phase : "");
                    items[i].int_value_a = events[i].as.frame.frame_number;
                    items[i].double_value = events[i].as.frame.delta_seconds;
                    if (!runtime_clone_posted_event_meta(&items[i].meta,
                                                         events[i].as.frame.meta_override,
                                                         error_buf,
                                                         error_buf_size) ||
                        !runtime_clone_posted_event_extra_state(&items[i].extra,
                                                                RUNTIME_POSTED_EVENT_FRAME,
                                                                events[i].as.frame.extra_fields,
                                                                events[i].as.frame.extra_field_count,
                                                         error_buf,
                                                         error_buf_size)) {
                        prepared_count = i + 1;
                        goto cleanup;
                    }
                    break;
                default:
                    runtime_set_error_buffer(error_buf, error_buf_size, "Mixed event batch contains an unsupported event kind");
                    goto cleanup;
            }
            prepared_count = i + 1;
        }
    }

    ok = runtime_post_event_batch(rt,
                                  callback_name,
                                  items,
                                  event_count,
                                  error_buf,
                                  error_buf_size);
cleanup:
    if (items) {
        for (int i = 0; i < prepared_count; i++) {
            runtime_posted_event_meta_state_free(&items[i].meta);
            runtime_posted_event_extra_state_free(&items[i].extra);
        }
    }
    free(items);
    return ok;
}
