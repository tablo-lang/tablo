#include "vm.h"
#include "jit.h"
#include "native_extension.h"
#include "safe_alloc.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>

static bool value_type_is_unmanaged(ValueType type);
static void async_task_free(AsyncTask* task);
static const char* vm_record_runtime_type_name(ObjRecord* record);
static int64_t vm_time_monotonic_ms(void);
static void vm_sleep_ms(int64_t ms);

typedef struct VmPostedEventEntry {
    VmPostedEventDispatchFn dispatch_fn;
    VmPostedEventFreeFn free_fn;
    void* payload;
    struct VmPostedEventEntry* next;
} VmPostedEventEntry;

struct VmPostedEventQueue {
    mtx_t mutex;
    cnd_t cond;
    int ref_count;
    int pending_count;
    bool closed;
    VM* vm;
    VmPostedEventEntry* head;
    VmPostedEventEntry* tail;
};

static VmPostedEventQueue* vm_posted_event_queue_create(VM* vm) {
    VmPostedEventQueue* queue = (VmPostedEventQueue*)calloc(1, sizeof(VmPostedEventQueue));
    if (!queue) {
        return NULL;
    }
    if (mtx_init(&queue->mutex, mtx_plain) != thrd_success) {
        free(queue);
        return NULL;
    }
    if (cnd_init(&queue->cond) != thrd_success) {
        mtx_destroy(&queue->mutex);
        free(queue);
        return NULL;
    }
    queue->ref_count = 1;
    queue->pending_count = 0;
    queue->closed = false;
    queue->vm = vm;
    return queue;
}

void vm_posted_event_queue_retain(VmPostedEventQueue* queue) {
    if (!queue) return;
    mtx_lock(&queue->mutex);
    queue->ref_count++;
    mtx_unlock(&queue->mutex);
}

void vm_posted_event_queue_release(VmPostedEventQueue* queue) {
    bool destroy = false;
    if (!queue) return;

    mtx_lock(&queue->mutex);
    queue->ref_count--;
    destroy = queue->ref_count <= 0;
    mtx_unlock(&queue->mutex);

    if (!destroy) return;

    cnd_destroy(&queue->cond);
    mtx_destroy(&queue->mutex);
    free(queue);
}

static bool vm_posted_event_queue_close(VmPostedEventQueue* queue) {
    VmPostedEventEntry* head = NULL;
    bool closed_now = false;
    if (!queue) return false;

    mtx_lock(&queue->mutex);
    if (!queue->closed) {
        queue->closed = true;
        closed_now = true;
        queue->vm = NULL;
        head = queue->head;
        queue->head = NULL;
        queue->tail = NULL;
        queue->pending_count = 0;
        cnd_broadcast(&queue->cond);
    }
    mtx_unlock(&queue->mutex);

    while (head) {
        VmPostedEventEntry* next = head->next;
        if (head->free_fn) {
            head->free_fn(head->payload);
        }
        free(head);
        head = next;
    }
    return closed_now;
}

VmPostedEventQueue* vm_get_posted_event_queue(VM* vm) {
    if (!vm || !vm->posted_event_queue) return NULL;
    vm_posted_event_queue_retain(vm->posted_event_queue);
    return vm->posted_event_queue;
}

bool vm_posted_event_queue_is_open(VmPostedEventQueue* queue) {
    bool is_open = false;
    if (!queue) return false;
    mtx_lock(&queue->mutex);
    is_open = !queue->closed;
    mtx_unlock(&queue->mutex);
    return is_open;
}

bool vm_close_posted_event_queue(VM* vm) {
    if (!vm || !vm->posted_event_queue) return false;
    return vm_posted_event_queue_close(vm->posted_event_queue);
}

bool vm_is_posted_event_queue_open(VM* vm) {
    if (!vm || !vm->posted_event_queue) return false;
    return vm_posted_event_queue_is_open(vm->posted_event_queue);
}

bool vm_posted_event_queue_enqueue(VmPostedEventQueue* queue,
                                   VmPostedEventDispatchFn dispatch_fn,
                                   VmPostedEventFreeFn free_fn,
                                   void* payload) {
    VmPostedEventEntry* entry = NULL;
    bool ok = false;

    if (!queue || !dispatch_fn || !payload) return false;

    entry = (VmPostedEventEntry*)calloc(1, sizeof(VmPostedEventEntry));
    if (!entry) return false;

    entry->dispatch_fn = dispatch_fn;
    entry->free_fn = free_fn;
    entry->payload = payload;

    mtx_lock(&queue->mutex);
    if (!queue->closed) {
        if (queue->tail) {
            queue->tail->next = entry;
        } else {
            queue->head = entry;
        }
        queue->tail = entry;
        queue->pending_count++;
        cnd_signal(&queue->cond);
        ok = true;
    }
    mtx_unlock(&queue->mutex);

    if (!ok) {
        free(entry);
    }
    return ok;
}

static VmPostedEventEntry* vm_posted_event_queue_dequeue(VmPostedEventQueue* queue) {
    VmPostedEventEntry* entry = NULL;
    if (!queue) return NULL;

    mtx_lock(&queue->mutex);
    entry = queue->head;
    if (entry) {
        queue->head = entry->next;
        if (!queue->head) {
            queue->tail = NULL;
        }
        queue->pending_count--;
        entry->next = NULL;
    }
    mtx_unlock(&queue->mutex);
    return entry;
}

static void vm_posted_event_queue_wait(VmPostedEventQueue* queue, int64_t wait_ms) {
    if (wait_ms <= 0) return;
    if (!queue) {
        vm_sleep_ms(wait_ms);
        return;
    }

    mtx_lock(&queue->mutex);
    if (!queue->closed && !queue->head) {
        struct timespec ts;
        if (timespec_get(&ts, TIME_UTC) == TIME_UTC) {
            int64_t sec_add = wait_ms / 1000LL;
            int64_t nsec_add = (wait_ms % 1000LL) * 1000000LL;
            ts.tv_sec += (time_t)sec_add;
            ts.tv_nsec += (long)nsec_add;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += ts.tv_nsec / 1000000000L;
                ts.tv_nsec %= 1000000000L;
            }
            (void)cnd_timedwait(&queue->cond, &queue->mutex, &ts);
        }
    }
    mtx_unlock(&queue->mutex);
}

int vm_posted_event_queue_pending_count(VM* vm) {
    int pending_count = 0;
    if (!vm || !vm->posted_event_queue) return 0;

    mtx_lock(&vm->posted_event_queue->mutex);
    pending_count = vm->posted_event_queue->pending_count;
    mtx_unlock(&vm->posted_event_queue->mutex);
    return pending_count;
}

bool vm_has_posted_event_queue_work(VM* vm) {
    return vm_posted_event_queue_pending_count(vm) > 0;
}

static int vm_process_posted_events_with_budget(VM* vm, int max_events, int64_t deadline_ms) {
    int processed = 0;
    if (!vm || !vm->posted_event_queue) return 0;
    if (max_events == 0) return 0;

    while (1) {
        if (deadline_ms >= 0 && processed > 0 && vm_time_monotonic_ms() >= deadline_ms) {
            break;
        }
        VmPostedEventEntry* entry = vm_posted_event_queue_dequeue(vm->posted_event_queue);
        if (!entry) {
            break;
        }
        if (!vm->error_occurred && entry->dispatch_fn) {
            (void)entry->dispatch_fn(vm, entry->payload);
        }
        if (entry->free_fn) {
            entry->free_fn(entry->payload);
        }
        free(entry);
        processed++;
        if (max_events > 0 && processed >= max_events) {
            break;
        }
        if (vm->error_occurred) {
            break;
        }
        if (deadline_ms >= 0 && vm_time_monotonic_ms() >= deadline_ms) {
            break;
        }
    }
    return processed;
}

static int vm_process_posted_events(VM* vm, int max_events) {
    return vm_process_posted_events_with_budget(vm, max_events, -1);
}

int vm_drain_posted_event_queue(VM* vm, int max_events) {
    if (!vm || max_events == 0) return 0;
    return vm_process_posted_events(vm, max_events);
}

int vm_drain_posted_event_queue_for_ms(VM* vm, int max_events, int64_t max_millis) {
    int64_t deadline_ms = -1;
    if (!vm || max_events == 0) return 0;
    if (max_millis > 0) {
        int64_t now_ms = vm_time_monotonic_ms();
        deadline_ms = now_ms >= INT64_MAX - max_millis ? INT64_MAX : now_ms + max_millis;
    }
    return vm_process_posted_events_with_budget(vm, max_events, deadline_ms);
}

bool vm_wait_for_posted_event_queue(VM* vm, int64_t timeout_millis) {
    if (!vm || !vm->posted_event_queue) return false;
    if (vm_has_posted_event_queue_work(vm)) return true;
    if (timeout_millis > 0) {
        vm_posted_event_queue_wait(vm->posted_event_queue, timeout_millis);
    }
    return vm_has_posted_event_queue_work(vm);
}

int vm_wait_and_drain_posted_event_queue(VM* vm, int max_events, int64_t timeout_millis) {
    if (!vm || max_events == 0) return 0;
    if (!vm_has_posted_event_queue_work(vm) && timeout_millis > 0) {
        vm_posted_event_queue_wait(vm->posted_event_queue, timeout_millis);
    }
    return vm_process_posted_events(vm, max_events);
}

bool vm_get_posted_event_auto_drain(VM* vm) {
    if (!vm) return false;
    return vm->posted_event_auto_drain;
}

bool vm_set_posted_event_auto_drain(VM* vm, bool enabled) {
    bool previous = false;
    if (!vm) return false;
    previous = vm->posted_event_auto_drain;
    vm->posted_event_auto_drain = enabled;
    return previous;
}

static void vm_gc_maybe_collect(VM* vm) {
    if (!vm) return;
    if (vm->cycle_gc.threshold <= 0) return;
    int allocations_since_collect = cycle_gc_allocations_since_collect(&vm->cycle_gc);
    if (allocations_since_collect >= vm->cycle_gc.threshold) {
        vm_gc_collect(vm);
    }
}

// Socket includes
#ifdef _WIN32
    #define NOMINMAX
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
    #define CLOSESOCKET closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    #include <fcntl.h>
    #define CLOSESOCKET close
#endif

void value_init_nil(Value* val) {
    value_set_type(val, VAL_NIL);
}

void value_init_int(Value* val, int64_t value) {
    value_set_type(val, VAL_INT);
    value_set_int(val, value);
}

void value_init_bool(Value* val, bool value) {
    value_set_type(val, VAL_BOOL);
    value_set_bool(val, value);
}

void value_init_double(Value* val, double value) {
    value_set_type(val, VAL_DOUBLE);
    value_set_double(val, value);
}

void value_init_bigint(Value* val, ObjBigInt* bigint) {
    value_set_type(val, VAL_BIGINT);
    value_set_bigint_obj(val, bigint);
}

void value_init_string(Value* val, const char* str) {
    value_set_type(val, VAL_STRING);
    if (!str) {
        value_set_string_obj(val, NULL);
        return;
    }
    size_t len = strlen(str);
    if (len > (size_t)INT_MAX) len = (size_t)INT_MAX;
    value_set_string_obj(val, obj_string_create(str, (int)len));
}

void value_init_string_n(Value* val, const char* str, int length) {
    value_set_type(val, VAL_STRING);
    if (!str) {
        value_set_string_obj(val, NULL);
        return;
    }
    if (length < 0) length = 0;
    value_set_string_obj(val, obj_string_create(str, length));
}

void value_init_bytes(Value* val, ObjBytes* bytes) {
    value_set_type(val, VAL_BYTES);
    value_set_bytes_obj(val, bytes);
}

void value_init_array(Value* val, ObjArray* arr) {
    value_set_type(val, VAL_ARRAY);
    value_set_array_obj(val, arr);
}

void value_init_function(Value* val, ObjFunction* func) {
    value_set_type(val, VAL_FUNCTION);
    value_set_function_obj(val, func);
}

void value_init_native(Value* val, ObjNative* native) {
    value_set_type(val, VAL_NATIVE);
    value_set_native_obj(val, native);
}

void value_init_record(Value* val, ObjRecord* record) {
    value_set_type(val, VAL_RECORD);
    value_set_record_obj(val, record);
}

void value_init_tuple(Value* val, ObjTuple* tuple) {
    value_set_type(val, VAL_TUPLE);
    value_set_tuple_obj(val, tuple);
}

void value_init_map(Value* val, ObjMap* map) {
    value_set_type(val, VAL_MAP);
    value_set_map_obj(val, map);
}

void value_init_set(Value* val, ObjSet* set) {
    value_set_type(val, VAL_SET);
    value_set_set_obj(val, set);
}

bool value_is_nil(Value* val) {
    if (!val) return true;
    return value_get_type(val) == VAL_NIL;
}

bool value_is_true(Value* val) {
    if (!val) return false;
    if (value_get_type(val) != VAL_BOOL) return false;
    return value_get_bool(val);
}

static bool value_equals_internal(const Value* a, const Value* b, int depth) {
    if (!a || !b) return false;
    if (depth > 64) return false;

    ValueType a_type = value_get_type(a);
    ValueType b_type = value_get_type(b);
    if (a_type != b_type) {
        if ((a_type == VAL_INT && b_type == VAL_BIGINT) ||
            (a_type == VAL_BIGINT && b_type == VAL_INT)) {
            const ObjBigInt* big = (a_type == VAL_BIGINT) ? value_get_bigint_obj(a) : value_get_bigint_obj(b);
            int64_t small_val = (a_type == VAL_INT) ? value_get_int(a) : value_get_int(b);
            ObjBigInt* temp = obj_bigint_from_int64(small_val);
            int cmp = obj_bigint_compare(big, temp);
            obj_bigint_release(temp);
            return cmp == 0;
        }
        if (a_type == VAL_NIL || b_type == VAL_NIL) {
            return false;
        }
    }

    switch (a_type) {
        case VAL_NIL:
            return b_type == VAL_NIL;
        case VAL_INT:
            return b_type == VAL_INT ? value_get_int(a) == value_get_int(b) : false;
        case VAL_BOOL:
            return b_type == VAL_BOOL ? value_get_bool(a) == value_get_bool(b) : false;
        case VAL_BIGINT:
            return b_type == VAL_BIGINT ? obj_bigint_compare(value_get_bigint_obj(a), value_get_bigint_obj(b)) == 0 : false;
        case VAL_DOUBLE:
            return b_type == VAL_DOUBLE ? value_get_double(a) == value_get_double(b) : false;
        case VAL_STRING: {
            ObjString* a_string = value_get_string_obj(a);
            ObjString* b_string = value_get_string_obj(b);
            return b_type == VAL_STRING &&
                   a_string && b_string &&
                   strcmp(a_string->chars, b_string->chars) == 0;
        }
        case VAL_BYTES:
            return value_get_bytes_obj(a) == value_get_bytes_obj(b);
        case VAL_ARRAY:
            return value_get_array_obj(a) == value_get_array_obj(b);
        case VAL_RECORD:
            return value_get_record_obj(a) == value_get_record_obj(b);
        case VAL_TUPLE: {
            ObjTuple* a_tuple = value_get_tuple_obj(a);
            ObjTuple* b_tuple = value_get_tuple_obj(b);
            if (a_tuple == b_tuple) return true;
            if (!a_tuple || !b_tuple) return false;
            if (a_tuple->element_count != b_tuple->element_count) return false;
            for (int i = 0; i < a_tuple->element_count; i++) {
                if (!value_equals_internal(&a_tuple->elements[i], &b_tuple->elements[i], depth + 1)) {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

bool value_equals(Value* a, Value* b) {
    return value_equals_internal(a, b, 0);
}

static void vm_write_output_n(VM* vm, const char* text, int length) {
    if (!text || length <= 0) return;
    if (vm && vm->output_callback) {
        vm->output_callback(vm->output_callback_user_data, text, length);
        return;
    }
    (void)fwrite(text, 1, (size_t)length, stdout);
}

static void vm_write_output(VM* vm, const char* text) {
    if (!text) return;
    vm_write_output_n(vm, text, (int)strlen(text));
}

static void vm_write_output_format(VM* vm, const char* fmt, ...) {
    char stack_buf[128];
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, args);
    va_end(args);
    if (needed <= 0) {
        return;
    }
    if (needed < (int)sizeof(stack_buf)) {
        vm_write_output_n(vm, stack_buf, needed);
        return;
    }

    char* heap_buf = (char*)safe_malloc((size_t)needed + 1);
    va_start(args, fmt);
    vsnprintf(heap_buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    vm_write_output_n(vm, heap_buf, needed);
    free(heap_buf);
}

void value_print(VM* vm, Value* val) {
    switch (value_get_type(val)) {
        case VAL_NIL: vm_write_output(vm, "nil"); break;
        case VAL_INT: vm_write_output_format(vm, "%lld", (long long)value_get_int(val)); break;
        case VAL_BOOL: vm_write_output(vm, value_get_bool(val) ? "true" : "false"); break;
        case VAL_BIGINT: {
            char* str = obj_bigint_to_string(value_get_bigint_obj(val));
            vm_write_output(vm, str ? str : "0");
            if (str) free(str);
            break;
        }
        case VAL_DOUBLE: {
            double d = value_get_double(val);
            if (isnan(d)) {
                vm_write_output(vm, "nan");
            } else if (isinf(d)) {
                vm_write_output(vm, d < 0.0 ? "-inf" : "inf");
            } else if (d == (int64_t)d) {
                vm_write_output_format(vm, "%.1f", d);
            } else {
                vm_write_output_format(vm, "%g", d);
            }
            break;
        }
        case VAL_STRING: {
            ObjString* str = value_get_string_obj(val);
            vm_write_output(vm, str ? str->chars : "null");
            break;
        }
        case VAL_BYTES:
            vm_write_output_format(vm,
                                   "<bytes len=%d>",
                                   (value_get_bytes_obj(val) ? value_get_bytes_obj(val)->length : 0));
            break;
        case VAL_ARRAY: {
            vm_write_output(vm, "[");
            ObjArray* arr = value_get_array_obj(val);
            for (int i = 0; arr && i < arr->count; i++) {
                if (i > 0) vm_write_output(vm, ", ");
                if (arr->kind == ARRAY_KIND_BOXED) {
                    value_print(vm, &arr->data.elements[i]);
                } else {
                    Value tmp;
                    switch (arr->kind) {
                        case ARRAY_KIND_INT:
                            value_init_int(&tmp, arr->data.ints[i]);
                            break;
                        case ARRAY_KIND_DOUBLE:
                            value_init_double(&tmp, arr->data.doubles[i]);
                            break;
                        case ARRAY_KIND_BOOL:
                            value_init_bool(&tmp, arr->data.bools[i] != 0);
                            break;
                        case ARRAY_KIND_BYTE:
                            value_init_int(&tmp, (int64_t)arr->data.bytes[i]);
                            break;
                        default:
                            value_init_nil(&tmp);
                            break;
                    }
                    value_print(vm, &tmp);
                }
            }
            vm_write_output(vm, "]");
            break;
        }
        case VAL_FUNCTION: vm_write_output(vm, "<func>"); break;
        case VAL_NATIVE: vm_write_output(vm, "<native>"); break;
        case VAL_RECORD: {
            ObjRecord* record = value_get_record_obj(val);
            if (record && record->is_native_opaque) {
                vm_write_output_format(vm,
                                       "<%s>",
                                       (record->type_name && record->type_name[0] != '\0')
                                           ? record->type_name
                                           : "opaque");
                break;
            }
            vm_write_output(vm, "{");
            if (record && record->def) {
                for (int i = 0; i < record->field_count; i++) {
                    if (i > 0) vm_write_output(vm, ", ");
                    if (i < record->def->field_count) {
                        vm_write_output_format(vm, "%s: ", record->def->fields[i].name);
                    }
                    value_print(vm, &record->fields[i]);
                }
            }
            vm_write_output(vm, "}");
            break;
        }
        case VAL_TUPLE: {
            vm_write_output(vm, "(");
            ObjTuple* tuple = value_get_tuple_obj(val);
            if (tuple) {
                for (int i = 0; i < tuple->element_count; i++) {
                    if (i > 0) vm_write_output(vm, ", ");
                    value_print(vm, &tuple->elements[i]);
                }
            }
            vm_write_output(vm, ")");
            break;
        }
        case VAL_MAP: {
            vm_write_output(vm, "{");
            ObjMap* map = value_get_map_obj(val);
            if (map) {
                bool first = true;
                for (int i = 0; i < map->capacity; i++) {
                    MapSlot* slot = &map->slots[i];
                    if (slot->hash < 2) continue;
                    if (!first) vm_write_output(vm, ", ");
                    first = false;
                    value_print(vm, &slot->key);
                    vm_write_output(vm, ": ");
                    value_print(vm, &slot->value);
                }
            }
            vm_write_output(vm, "}");
            break;
        }
        case VAL_SET: {
            vm_write_output(vm, "{");
            ObjSet* set = value_get_set_obj(val);
            if (set) {
                bool first = true;
                for (int i = 0; i < set->capacity; i++) {
                    SetSlot* slot = &set->slots[i];
                    if (slot->hash < 2) continue;
                    if (!first) vm_write_output(vm, ", ");
                    first = false;
                    value_print(vm, &slot->value);
                }
            }
            vm_write_output(vm, "}");
            break;
        }
        case VAL_FILE:
            if (value_get_file_obj(val) && !value_get_file_obj(val)->is_closed && value_get_file_obj(val)->handle) {
                vm_write_output(vm, "<file>");
            } else {
                vm_write_output(vm, "<file:closed>");
            }
            break;
        default: vm_write_output(vm, "<unknown>"); break;
    }
}

typedef struct {
    char* text;
    int length;
    int capacity;
} VmDebugTextBuffer;

static void vm_debug_text_capture(void* user_data, const char* text, int length) {
    VmDebugTextBuffer* buffer = (VmDebugTextBuffer*)user_data;
    if (!buffer || !text || length <= 0) return;

    if (buffer->length + length + 1 > buffer->capacity) {
        int new_capacity = buffer->capacity > 0 ? buffer->capacity * 2 : 64;
        while (new_capacity < buffer->length + length + 1) {
            new_capacity *= 2;
        }
        char* new_text = (char*)realloc(buffer->text, (size_t)new_capacity);
        if (!new_text) return;
        buffer->text = new_text;
        buffer->capacity = new_capacity;
    }

    memcpy(buffer->text + buffer->length, text, (size_t)length);
    buffer->length += length;
    buffer->text[buffer->length] = '\0';
}

char* vm_debug_format_value(VM* vm, const Value* value) {
    VmDebugTextBuffer buffer = {0};
    VmOutputCallback prev_callback = NULL;
    void* prev_user_data = NULL;
    Value temp;

    if (!value) {
        return safe_strdup("<null>");
    }

    if (vm) {
        prev_callback = vm->output_callback;
        prev_user_data = vm->output_callback_user_data;
        vm_set_output_callback(vm, vm_debug_text_capture, &buffer);
    }

    temp = *value;
    value_print(vm, &temp);

    if (vm) {
        vm_set_output_callback(vm, prev_callback, prev_user_data);
    }

    if (!buffer.text) {
        return safe_strdup("");
    }
    return buffer.text;
}

const char* vm_debug_value_type_name(const Value* value) {
    if (!value) return "unknown";

    switch (value_get_type((Value*)value)) {
        case VAL_NIL: return "nil";
        case VAL_INT: return "int";
        case VAL_BOOL: return "bool";
        case VAL_DOUBLE: return "double";
        case VAL_BIGINT: return "bigint";
        case VAL_STRING: return "string";
        case VAL_BYTES: return "bytes";
        case VAL_ARRAY: return "array";
        case VAL_FUNCTION: return "function";
        case VAL_NATIVE: return "native";
        case VAL_RECORD: {
            ObjRecord* record = value_get_record_obj((Value*)value);
            const char* record_name = vm_record_runtime_type_name(record);
            return (record_name && record_name[0] != '\0') ? record_name : "record";
        }
        case VAL_TUPLE: return "tuple";
        case VAL_MAP: return "map";
        case VAL_SET: return "set";
        case VAL_SOCKET: return "socket";
        case VAL_FILE: return "file";
        default: return "unknown";
    }
}

void value_retain(Value* val) {
    if (!val) return;

    switch (value_get_type(val)) {
        case VAL_STRING:
            if (value_get_string_obj(val)) obj_string_retain(value_get_string_obj(val));
            break;
        case VAL_BYTES:
            if (value_get_bytes_obj(val)) obj_bytes_retain(value_get_bytes_obj(val));
            break;
        case VAL_BIGINT:
            if (value_get_bigint_obj(val)) obj_bigint_retain(value_get_bigint_obj(val));
            break;
        case VAL_ARRAY:
            if (value_get_array_obj(val)) obj_array_retain(value_get_array_obj(val));
            break;
        case VAL_FUNCTION:
            if (value_get_function_obj(val)) obj_function_retain(value_get_function_obj(val));
            break;
        case VAL_NATIVE:
            if (value_get_native_obj(val)) obj_native_retain(value_get_native_obj(val));
            break;
        case VAL_RECORD:
            if (value_get_record_obj(val)) obj_record_retain(value_get_record_obj(val));
            break;
        case VAL_TUPLE:
            if (value_get_tuple_obj(val)) obj_tuple_retain(value_get_tuple_obj(val));
            break;
        case VAL_MAP:
            if (value_get_map_obj(val)) obj_map_retain(value_get_map_obj(val));
            break;
        case VAL_SET:
            if (value_get_set_obj(val)) obj_set_retain(value_get_set_obj(val));
            break;
        case VAL_SOCKET:
            if (value_get_socket_obj(val)) obj_socket_retain(value_get_socket_obj(val));
            break;
        case VAL_FILE:
            if (value_get_file_obj(val)) obj_file_retain(value_get_file_obj(val));
            break;
        default:
            break;
    }
}

void value_free(Value* val) {
    switch (value_get_type(val)) {
        case VAL_STRING:
            if (value_get_string_obj(val)) obj_string_release(value_get_string_obj(val));
            break;
        case VAL_BYTES:
            if (value_get_bytes_obj(val)) obj_bytes_release(value_get_bytes_obj(val));
            break;
        case VAL_BIGINT:
            if (value_get_bigint_obj(val)) obj_bigint_release(value_get_bigint_obj(val));
            break;
        case VAL_ARRAY:
            if (value_get_array_obj(val)) obj_array_release(value_get_array_obj(val));
            break;
        case VAL_FUNCTION:
            if (value_get_function_obj(val)) obj_function_release(value_get_function_obj(val));
            break;
        case VAL_NATIVE:
            if (value_get_native_obj(val)) obj_native_release(value_get_native_obj(val));
            break;
        case VAL_TUPLE:
            if (value_get_tuple_obj(val)) obj_tuple_release(value_get_tuple_obj(val));
            break;
        case VAL_RECORD:
            if (value_get_record_obj(val)) obj_record_release(value_get_record_obj(val));
            break;
        case VAL_MAP:
            if (value_get_map_obj(val)) obj_map_release(value_get_map_obj(val));
            break;
        case VAL_SET:
            if (value_get_set_obj(val)) obj_set_release(value_get_set_obj(val));
            break;
        case VAL_SOCKET:
            if (value_get_socket_obj(val)) obj_socket_release(value_get_socket_obj(val));
            break;
        case VAL_FILE:
            if (value_get_file_obj(val)) obj_file_release(value_get_file_obj(val));
            break;
        default:
            break;
    }
    value_init_nil(val);
}

static ObjBigInt* bigint_from_value(Value* val, bool* needs_release) {
    if (needs_release) *needs_release = false;
    if (!val) return NULL;
    if (value_get_type(val) == VAL_BIGINT) {
        return value_get_bigint_obj(val);
    }
    if (value_get_type(val) == VAL_INT) {
        if (needs_release) *needs_release = true;
        return obj_bigint_from_int64(value_get_int(val));
    }
    return NULL;
}

static int vm_compute_arithmetic_op(VM* vm, OpCode op, const Value* a, const Value* b, Value* out) {
    if (!vm || !a || !b || !out) return -1;
    ValueType a_type = value_get_type(a);
    ValueType b_type = value_get_type(b);

    switch (op) {
        case OP_ADD: {
            if ((a_type == VAL_BIGINT || b_type == VAL_BIGINT) &&
                (a_type == VAL_DOUBLE || b_type == VAL_DOUBLE)) {
                vm_runtime_error(vm, "Cannot mix bigint and double");
                return -1;
            } else if (a_type == VAL_BIGINT || b_type == VAL_BIGINT) {
                bool release_a = false;
                bool release_b = false;
                Value a_copy = *a;
                Value b_copy = *b;
                ObjBigInt* big_a = bigint_from_value(&a_copy, &release_a);
                ObjBigInt* big_b = bigint_from_value(&b_copy, &release_b);
                if (!big_a || !big_b) {
                    vm_runtime_error(vm, "Operands must be numbers or strings");
                    if (release_a && big_a) obj_bigint_release(big_a);
                    if (release_b && big_b) obj_bigint_release(big_b);
                    return -1;
                }
                ObjBigInt* sum = obj_bigint_add(big_a, big_b);
                if (release_a) obj_bigint_release(big_a);
                if (release_b) obj_bigint_release(big_b);
                value_init_bigint(out, sum);
                return 0;
            } else if (a_type == VAL_INT && b_type == VAL_INT) {
                value_init_int(out, value_get_int(a) + value_get_int(b));
                return 0;
            } else if (a_type == VAL_DOUBLE && b_type == VAL_DOUBLE) {
                value_init_double(out, value_get_double(a) + value_get_double(b));
                return 0;
            } else if (a_type == VAL_STRING && b_type == VAL_STRING) {
                ObjString* a_string = value_get_string_obj(a);
                ObjString* b_string = value_get_string_obj(b);
                int len = a_string->length + b_string->length;
                if (len > vm->config.max_string_length) {
                    vm_runtime_error(vm, "String length exceeds maximum allowed");
                    return -1;
                }
                char* chars = (char*)safe_malloc(len + 1);
                memcpy(chars, a_string->chars, a_string->length);
                memcpy(chars + a_string->length, b_string->chars, b_string->length);
                chars[len] = '\0';
                value_init_string(out, chars);
                free(chars);
                return 0;
            }

            vm_runtime_error(vm, "Operands must be numbers or strings");
            return -1;
        }

        case OP_SUB: {
            if ((a_type == VAL_BIGINT || b_type == VAL_BIGINT) &&
                (a_type == VAL_DOUBLE || b_type == VAL_DOUBLE)) {
                vm_runtime_error(vm, "Cannot mix bigint and double");
                return -1;
            } else if (a_type == VAL_BIGINT || b_type == VAL_BIGINT) {
                bool release_a = false;
                bool release_b = false;
                Value a_copy = *a;
                Value b_copy = *b;
                ObjBigInt* big_a = bigint_from_value(&a_copy, &release_a);
                ObjBigInt* big_b = bigint_from_value(&b_copy, &release_b);
                if (!big_a || !big_b) {
                    vm_runtime_error(vm, "Operands must be numbers");
                    if (release_a && big_a) obj_bigint_release(big_a);
                    if (release_b && big_b) obj_bigint_release(big_b);
                    return -1;
                }
                ObjBigInt* diff = obj_bigint_sub(big_a, big_b);
                if (release_a) obj_bigint_release(big_a);
                if (release_b) obj_bigint_release(big_b);
                value_init_bigint(out, diff);
                return 0;
            } else if (a_type == VAL_INT && b_type == VAL_INT) {
                value_init_int(out, value_get_int(a) - value_get_int(b));
                return 0;
            } else if (a_type == VAL_DOUBLE && b_type == VAL_DOUBLE) {
                value_init_double(out, value_get_double(a) - value_get_double(b));
                return 0;
            }

            vm_runtime_error(vm, "Operands must be numbers");
            return -1;
        }

        case OP_MUL: {
            if ((a_type == VAL_BIGINT || b_type == VAL_BIGINT) &&
                (a_type == VAL_DOUBLE || b_type == VAL_DOUBLE)) {
                vm_runtime_error(vm, "Cannot mix bigint and double");
                return -1;
            } else if (a_type == VAL_BIGINT || b_type == VAL_BIGINT) {
                bool release_a = false;
                bool release_b = false;
                Value a_copy = *a;
                Value b_copy = *b;
                ObjBigInt* big_a = bigint_from_value(&a_copy, &release_a);
                ObjBigInt* big_b = bigint_from_value(&b_copy, &release_b);
                if (!big_a || !big_b) {
                    vm_runtime_error(vm, "Operands must be numbers");
                    if (release_a && big_a) obj_bigint_release(big_a);
                    if (release_b && big_b) obj_bigint_release(big_b);
                    return -1;
                }
                ObjBigInt* prod = obj_bigint_mul(big_a, big_b);
                if (release_a) obj_bigint_release(big_a);
                if (release_b) obj_bigint_release(big_b);
                value_init_bigint(out, prod);
                return 0;
            } else if (a_type == VAL_INT && b_type == VAL_INT) {
                value_init_int(out, value_get_int(a) * value_get_int(b));
                return 0;
            } else if (a_type == VAL_DOUBLE && b_type == VAL_DOUBLE) {
                value_init_double(out, value_get_double(a) * value_get_double(b));
                return 0;
            }

            vm_runtime_error(vm, "Operands must be numbers");
            return -1;
        }

        case OP_DIV: {
            if ((a_type == VAL_BIGINT || b_type == VAL_BIGINT) &&
                (a_type == VAL_DOUBLE || b_type == VAL_DOUBLE)) {
                vm_runtime_error(vm, "Cannot mix bigint and double");
                return -1;
            } else if (a_type == VAL_BIGINT || b_type == VAL_BIGINT) {
                bool release_a = false;
                bool release_b = false;
                Value a_copy = *a;
                Value b_copy = *b;
                ObjBigInt* big_a = bigint_from_value(&a_copy, &release_a);
                ObjBigInt* big_b = bigint_from_value(&b_copy, &release_b);
                if (!big_a || !big_b) {
                    vm_runtime_error(vm, "Operands must be numbers");
                    if (release_a && big_a) obj_bigint_release(big_a);
                    if (release_b && big_b) obj_bigint_release(big_b);
                    return -1;
                }
                bool div_by_zero = false;
                ObjBigInt* quot = obj_bigint_div(big_a, big_b, &div_by_zero);
                if (release_a) obj_bigint_release(big_a);
                if (release_b) obj_bigint_release(big_b);
                if (div_by_zero) {
                    vm_runtime_error(vm, "Division by zero");
                    if (quot) obj_bigint_release(quot);
                    return -1;
                }
                value_init_bigint(out, quot);
                return 0;
            } else if (a_type == VAL_INT && b_type == VAL_INT) {
                if (value_get_int(b) == 0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }
                value_init_int(out, value_get_int(a) / value_get_int(b));
                return 0;
            } else if (a_type == VAL_DOUBLE && b_type == VAL_DOUBLE) {
                if (value_get_double(b) == 0.0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }
                value_init_double(out, value_get_double(a) / value_get_double(b));
                return 0;
            }

            vm_runtime_error(vm, "Operands must be numbers");
            return -1;
        }

        default:
            vm_runtime_error(vm, "Unsupported arithmetic opcode");
            return -1;
    }
}

static int vm_compute_comparison_op(VM* vm, OpCode op, const Value* a, const Value* b, Value* out) {
    if (!vm || !a || !b || !out) return -1;

    ValueType a_type = value_get_type(a);
    ValueType b_type = value_get_type(b);
    if ((a_type == VAL_BIGINT || b_type == VAL_BIGINT) &&
        (a_type == VAL_DOUBLE || b_type == VAL_DOUBLE)) {
        vm_runtime_error(vm, "Cannot mix bigint and double");
        return -1;
    }

    bool result = false;
    if (a_type == VAL_BIGINT || b_type == VAL_BIGINT) {
        bool release_a = false;
        bool release_b = false;
        Value a_copy = *a;
        Value b_copy = *b;
        ObjBigInt* big_a = bigint_from_value(&a_copy, &release_a);
        ObjBigInt* big_b = bigint_from_value(&b_copy, &release_b);
        if (!big_a || !big_b) {
            vm_runtime_error(vm, "Operands must be comparable");
            if (release_a && big_a) obj_bigint_release(big_a);
            if (release_b && big_b) obj_bigint_release(big_b);
            return -1;
        }

        int cmp = obj_bigint_compare(big_a, big_b);
        if (release_a) obj_bigint_release(big_a);
        if (release_b) obj_bigint_release(big_b);

        switch (op) {
            case OP_LT: result = cmp < 0; break;
            case OP_LE: result = cmp <= 0; break;
            case OP_GT: result = cmp > 0; break;
            case OP_GE: result = cmp >= 0; break;
            default:
                vm_runtime_error(vm, "Unsupported comparison opcode");
                return -1;
        }
    } else if (a_type == VAL_INT && b_type == VAL_INT) {
        int64_t av = value_get_int(a);
        int64_t bv = value_get_int(b);
        switch (op) {
            case OP_LT: result = av < bv; break;
            case OP_LE: result = av <= bv; break;
            case OP_GT: result = av > bv; break;
            case OP_GE: result = av >= bv; break;
            default:
                vm_runtime_error(vm, "Unsupported comparison opcode");
                return -1;
        }
    } else if (a_type == VAL_DOUBLE && b_type == VAL_DOUBLE) {
        double av = value_get_double(a);
        double bv = value_get_double(b);
        switch (op) {
            case OP_LT: result = av < bv; break;
            case OP_LE: result = av <= bv; break;
            case OP_GT: result = av > bv; break;
            case OP_GE: result = av >= bv; break;
            default:
                vm_runtime_error(vm, "Unsupported comparison opcode");
                return -1;
        }
    } else {
        vm_runtime_error(vm, "Operands must be comparable");
        return -1;
    }

    value_init_bool(out, result);
    return 0;
}

static bool vm_value_equals_fast_for_branch(const Value* a, const Value* b) {
    if (!a || !b) return false;
    ValueType a_type = value_get_type(a);
    ValueType b_type = value_get_type(b);
    if (a_type == VAL_INT && b_type == VAL_INT) {
        return value_get_int(a) == value_get_int(b);
    }
    if (a_type == VAL_BOOL && b_type == VAL_BOOL) {
        return value_get_bool(a) == value_get_bool(b);
    }
    if (a_type == VAL_DOUBLE && b_type == VAL_DOUBLE) {
        return value_get_double(a) == value_get_double(b);
    }
    if (a_type == VAL_STRING && b_type == VAL_STRING) {
        ObjString* a_str = value_get_string_obj(a);
        ObjString* b_str = value_get_string_obj(b);
        return a_str && b_str && strcmp(a_str->chars, b_str->chars) == 0;
    }
    return value_equals((Value*)a, (Value*)b);
}

static bool vm_value_equals_constant_fast_for_branch(const Value* a, const Constant* c) {
    if (!a || !c) return false;
    ValueType a_type = value_get_type(a);
    if (a_type == VAL_INT && c->type_index == 0) {
        return value_get_int(a) == c->as_int;
    }
    if (a_type == VAL_BOOL && c->type_index == 4) {
        return value_get_bool(a) == (c->as_int != 0);
    }
    if (a_type == VAL_DOUBLE && c->type_index == 1) {
        return value_get_double(a) == c->as_double;
    }
    if (a_type == VAL_STRING && c->type_index == 2) {
        ObjString* a_str = value_get_string_obj(a);
        return a_str && c->as_string && strcmp(a_str->chars, c->as_string) == 0;
    }
    return false;
}

ObjString* obj_string_create(const char* chars, int length) {
    ObjString* str = (ObjString*)safe_malloc(sizeof(ObjString));
    str->length = length;
    str->hash = UINT32_MAX;
    str->chars = (char*)safe_malloc(length + 1);
    memcpy(str->chars, chars, length);
    str->chars[length] = '\0';
    str->ref_count = 1;
    return str;
}

void obj_string_retain(ObjString* str) {
    if (str) str->ref_count++;
}

void obj_string_release(ObjString* str) {
    if (!str) return;
    str->ref_count--;
    if (str->ref_count <= 0) {
        obj_string_free(str);
    }
}

void obj_string_free(ObjString* str) {
    if (!str) return;
    if (str->chars) free(str->chars);
    free(str);
}

ObjBytesBuffer* obj_bytes_buffer_create(int capacity) {
    if (capacity < 0) capacity = 0;

    ObjBytesBuffer* buf = (ObjBytesBuffer*)safe_malloc(sizeof(ObjBytesBuffer));
    buf->capacity = capacity;
    buf->ref_count = 1;
    buf->data = NULL;
    if (capacity > 0) {
        buf->data = (uint8_t*)safe_malloc((size_t)capacity);
    }
    return buf;
}

void obj_bytes_buffer_retain(ObjBytesBuffer* buf) {
    if (buf) buf->ref_count++;
}

void obj_bytes_buffer_release(ObjBytesBuffer* buf) {
    if (!buf) return;
    buf->ref_count--;
    if (buf->ref_count <= 0) {
        obj_bytes_buffer_free(buf);
    }
}

void obj_bytes_buffer_free(ObjBytesBuffer* buf) {
    if (!buf) return;
    if (buf->data) free(buf->data);
    free(buf);
}

ObjBytes* obj_bytes_create(ObjBytesBuffer* buffer, int offset, int length) {
    if (offset < 0) offset = 0;
    if (length < 0) length = 0;

    ObjBytes* bytes = (ObjBytes*)safe_malloc(sizeof(ObjBytes));
    bytes->buffer = buffer;
    bytes->offset = offset;
    bytes->length = length;
    bytes->ref_count = 1;
    if (buffer) {
        obj_bytes_buffer_retain(buffer);
        if (bytes->offset > buffer->capacity) bytes->offset = buffer->capacity;
        int max_len = buffer->capacity - bytes->offset;
        if (bytes->length > max_len) bytes->length = max_len;
    } else {
        bytes->offset = 0;
        bytes->length = 0;
    }
    return bytes;
}

ObjBytes* obj_bytes_create_copy(const uint8_t* data, int length) {
    if (length < 0) length = 0;
    ObjBytesBuffer* buf = obj_bytes_buffer_create(length);
    if (length > 0 && data) {
        memcpy(buf->data, data, (size_t)length);
    }

    ObjBytes* bytes = obj_bytes_create(buf, 0, length);
    obj_bytes_buffer_release(buf);
    return bytes;
}

ObjBytes* obj_bytes_create_with_size(int length, uint8_t fill) {
    if (length < 0) length = 0;
    ObjBytesBuffer* buf = obj_bytes_buffer_create(length);
    if (length > 0) {
        memset(buf->data, fill, (size_t)length);
    }

    ObjBytes* bytes = obj_bytes_create(buf, 0, length);
    obj_bytes_buffer_release(buf);
    return bytes;
}

ObjBytes* obj_bytes_slice(ObjBytes* bytes, int start, int end) {
    if (!bytes) {
        return obj_bytes_create_with_size(0, 0);
    }

    int len = bytes->length;
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (end < start) end = start;

    return obj_bytes_create(bytes->buffer, bytes->offset + start, end - start);
}

uint8_t* obj_bytes_data(ObjBytes* bytes) {
    if (!bytes || !bytes->buffer || !bytes->buffer->data) return NULL;
    if (bytes->offset < 0 || bytes->offset > bytes->buffer->capacity) return NULL;
    return bytes->buffer->data + bytes->offset;
}

void obj_bytes_retain(ObjBytes* bytes) {
    if (bytes) bytes->ref_count++;
}

void obj_bytes_release(ObjBytes* bytes) {
    if (!bytes) return;
    bytes->ref_count--;
    if (bytes->ref_count <= 0) {
        obj_bytes_free(bytes);
    }
}

void obj_bytes_free(ObjBytes* bytes) {
    if (!bytes) return;
    if (bytes->buffer) obj_bytes_buffer_release(bytes->buffer);
    free(bytes);
}

ObjArray* obj_array_create(VM* vm, int capacity) {
    return obj_array_create_typed(vm, capacity, ARRAY_KIND_BOXED);
}

ObjArray* obj_array_create_typed(VM* vm, int capacity, ArrayKind kind) {
    ObjArray* arr = (ObjArray*)safe_malloc(sizeof(ObjArray));
    arr->count = 0;
    arr->capacity = capacity > 0 ? capacity : 4;
    arr->ref_count = 1;
    arr->kind = kind;
    switch (kind) {
        case ARRAY_KIND_BOXED:
            arr->data.elements = (Value*)safe_malloc((size_t)arr->capacity * sizeof(Value));
            break;
        case ARRAY_KIND_INT:
            arr->data.ints = (int64_t*)safe_malloc((size_t)arr->capacity * sizeof(int64_t));
            break;
        case ARRAY_KIND_DOUBLE:
            arr->data.doubles = (double*)safe_malloc((size_t)arr->capacity * sizeof(double));
            break;
        case ARRAY_KIND_BOOL:
            arr->data.bools = (uint8_t*)safe_malloc((size_t)arr->capacity * sizeof(uint8_t));
            break;
        case ARRAY_KIND_BYTE:
            arr->data.bytes = (uint8_t*)safe_malloc((size_t)arr->capacity * sizeof(uint8_t));
            break;
        default:
            arr->kind = ARRAY_KIND_BOXED;
            arr->data.elements = (Value*)safe_malloc((size_t)arr->capacity * sizeof(Value));
            break;
    }
    cycle_gc_node_init(&arr->gc_node, CYCLE_GC_OBJ_ARRAY, arr);
    if (vm) {
        cycle_gc_track(&vm->cycle_gc, &arr->gc_node);
        vm_gc_maybe_collect(vm);
    }
    return arr;
}

void obj_array_retain(ObjArray* arr) {
    if (arr) arr->ref_count++;
}

void obj_array_release(ObjArray* arr) {
    if (!arr) return;
    arr->ref_count--;
    if (arr->ref_count <= 0) {
        obj_array_free(arr);
    }
}

void obj_array_free(ObjArray* arr) {
    if (!arr) return;
    cycle_gc_untrack(&arr->gc_node);
    switch (arr->kind) {
        case ARRAY_KIND_BOXED:
            if (arr->data.elements) {
                for (int i = 0; i < arr->count; i++) {
                    value_free(&arr->data.elements[i]);
                }
                free(arr->data.elements);
            }
            break;
        case ARRAY_KIND_INT:
            if (arr->data.ints) free(arr->data.ints);
            break;
        case ARRAY_KIND_DOUBLE:
            if (arr->data.doubles) free(arr->data.doubles);
            break;
        case ARRAY_KIND_BOOL:
            if (arr->data.bools) free(arr->data.bools);
            break;
        case ARRAY_KIND_BYTE:
            if (arr->data.bytes) free(arr->data.bytes);
            break;
        default:
            break;
    }
    free(arr);
}

void obj_array_convert_to_boxed(ObjArray* arr) {
    if (!arr) return;
    if (arr->kind == ARRAY_KIND_BOXED) return;

    Value* elements = (Value*)safe_malloc((size_t)arr->capacity * sizeof(Value));
    for (int i = 0; i < arr->count; i++) {
        switch (arr->kind) {
            case ARRAY_KIND_INT:
                value_init_int(&elements[i], arr->data.ints[i]);
                break;
            case ARRAY_KIND_DOUBLE:
                value_init_double(&elements[i], arr->data.doubles[i]);
                break;
            case ARRAY_KIND_BOOL:
                value_init_bool(&elements[i], arr->data.bools[i] != 0);
                break;
            case ARRAY_KIND_BYTE:
                value_init_int(&elements[i], (int64_t)arr->data.bytes[i]);
                break;
            default:
                value_init_nil(&elements[i]);
                break;
        }
    }

    switch (arr->kind) {
        case ARRAY_KIND_INT:
            free(arr->data.ints);
            break;
        case ARRAY_KIND_DOUBLE:
            free(arr->data.doubles);
            break;
        case ARRAY_KIND_BOOL:
            free(arr->data.bools);
            break;
        case ARRAY_KIND_BYTE:
            free(arr->data.bytes);
            break;
        default:
            break;
    }

    arr->kind = ARRAY_KIND_BOXED;
    arr->data.elements = elements;
}

static void obj_array_convert_byte_to_int(ObjArray* arr) {
    if (!arr) return;
    if (arr->kind != ARRAY_KIND_BYTE) return;

    int64_t* ints = (int64_t*)safe_malloc((size_t)arr->capacity * sizeof(int64_t));
    for (int i = 0; i < arr->count; i++) {
        ints[i] = (int64_t)arr->data.bytes[i];
    }

    free(arr->data.bytes);
    arr->kind = ARRAY_KIND_INT;
    arr->data.ints = ints;
}

void obj_array_push(ObjArray* arr, Value val) {
    if (arr->count >= arr->capacity) {
        arr->capacity *= 2;
        switch (arr->kind) {
            case ARRAY_KIND_BOXED:
                arr->data.elements = (Value*)safe_realloc(arr->data.elements, (size_t)arr->capacity * sizeof(Value));
                break;
            case ARRAY_KIND_INT:
                arr->data.ints = (int64_t*)safe_realloc(arr->data.ints, (size_t)arr->capacity * sizeof(int64_t));
                break;
            case ARRAY_KIND_DOUBLE:
                arr->data.doubles = (double*)safe_realloc(arr->data.doubles, (size_t)arr->capacity * sizeof(double));
                break;
            case ARRAY_KIND_BOOL:
                arr->data.bools = (uint8_t*)safe_realloc(arr->data.bools, (size_t)arr->capacity * sizeof(uint8_t));
                break;
            case ARRAY_KIND_BYTE:
                arr->data.bytes = (uint8_t*)safe_realloc(arr->data.bytes, (size_t)arr->capacity * sizeof(uint8_t));
                break;
            default:
                break;
        }
    }
    switch (arr->kind) {
        case ARRAY_KIND_BOXED:
            arr->data.elements[arr->count++] = val;
            break;
        case ARRAY_KIND_INT:
            if (value_get_type(&val) != VAL_INT) {
                obj_array_convert_to_boxed(arr);
                arr->data.elements[arr->count++] = val;
                break;
            }
            arr->data.ints[arr->count++] = value_get_int(&val);
            break;
        case ARRAY_KIND_DOUBLE:
            if (value_get_type(&val) != VAL_DOUBLE) {
                obj_array_convert_to_boxed(arr);
                arr->data.elements[arr->count++] = val;
                break;
            }
            arr->data.doubles[arr->count++] = value_get_double(&val);
            break;
        case ARRAY_KIND_BOOL:
            if (value_get_type(&val) != VAL_BOOL) {
                obj_array_convert_to_boxed(arr);
                arr->data.elements[arr->count++] = val;
                break;
            }
            arr->data.bools[arr->count++] = (uint8_t)(value_get_bool(&val) ? 1 : 0);
            break;
        case ARRAY_KIND_BYTE:
            if (value_get_type(&val) != VAL_INT) {
                obj_array_convert_to_boxed(arr);
                arr->data.elements[arr->count++] = val;
                break;
            }
            if (value_get_int(&val) < 0 || value_get_int(&val) > 255) {
                obj_array_convert_byte_to_int(arr);
                arr->data.ints[arr->count++] = value_get_int(&val);
                break;
            }
            arr->data.bytes[arr->count++] = (uint8_t)value_get_int(&val);
            break;
        default:
            obj_array_convert_to_boxed(arr);
            arr->data.elements[arr->count++] = val;
            break;
    }
}

void obj_array_pop(ObjArray* arr, Value* out) {
    if (!out) return;
    if (arr->count <= 0) {
        value_init_nil(out);
        return;
    }
    arr->count--;
    switch (arr->kind) {
        case ARRAY_KIND_BOXED:
            *out = arr->data.elements[arr->count];
            break;
        case ARRAY_KIND_INT:
            value_init_int(out, arr->data.ints[arr->count]);
            break;
        case ARRAY_KIND_DOUBLE:
            value_init_double(out, arr->data.doubles[arr->count]);
            break;
        case ARRAY_KIND_BOOL:
            value_init_bool(out, arr->data.bools[arr->count] != 0);
            break;
        case ARRAY_KIND_BYTE:
            value_init_int(out, (int64_t)arr->data.bytes[arr->count]);
            break;
        default:
            value_init_nil(out);
            break;
    }
}

void obj_array_set(ObjArray* arr, int index, Value val) {
    if (index < 0 || index >= arr->count) {
        return;
    }
    switch (arr->kind) {
        case ARRAY_KIND_BOXED:
            arr->data.elements[index] = val;
            break;
        case ARRAY_KIND_INT:
            if (value_get_type(&val) != VAL_INT) {
                obj_array_convert_to_boxed(arr);
                arr->data.elements[index] = val;
                break;
            }
            arr->data.ints[index] = value_get_int(&val);
            break;
        case ARRAY_KIND_DOUBLE:
            if (value_get_type(&val) != VAL_DOUBLE) {
                obj_array_convert_to_boxed(arr);
                arr->data.elements[index] = val;
                break;
            }
            arr->data.doubles[index] = value_get_double(&val);
            break;
        case ARRAY_KIND_BOOL:
            if (value_get_type(&val) != VAL_BOOL) {
                obj_array_convert_to_boxed(arr);
                arr->data.elements[index] = val;
                break;
            }
            arr->data.bools[index] = (uint8_t)(value_get_bool(&val) ? 1 : 0);
            break;
        case ARRAY_KIND_BYTE:
            if (value_get_type(&val) != VAL_INT) {
                obj_array_convert_to_boxed(arr);
                arr->data.elements[index] = val;
                break;
            }
            if (value_get_int(&val) < 0 || value_get_int(&val) > 255) {
                obj_array_convert_byte_to_int(arr);
                arr->data.ints[index] = value_get_int(&val);
                break;
            }
            arr->data.bytes[index] = (uint8_t)value_get_int(&val);
            break;
        default:
            obj_array_convert_to_boxed(arr);
            arr->data.elements[index] = val;
            break;
    }
}

void obj_array_get(ObjArray* arr, int index, Value* out) {
    if (!out) return;
    if (index < 0 || index >= arr->count) {
        value_init_nil(out);
        return;
    }
    switch (arr->kind) {
        case ARRAY_KIND_BOXED:
            *out = arr->data.elements[index];
            break;
        case ARRAY_KIND_INT:
            value_init_int(out, arr->data.ints[index]);
            break;
        case ARRAY_KIND_DOUBLE:
            value_init_double(out, arr->data.doubles[index]);
            break;
        case ARRAY_KIND_BOOL:
            value_init_bool(out, arr->data.bools[index] != 0);
            break;
        case ARRAY_KIND_BYTE:
            value_init_int(out, (int64_t)arr->data.bytes[index]);
            break;
        default:
            value_init_nil(out);
            break;
    }
}

ObjFunction* obj_function_create(void) {
    ObjFunction* func = (ObjFunction*)safe_malloc(sizeof(ObjFunction));
    chunk_init(&func->chunk);
    constant_pool_init(&func->constants);
    func->param_count = 0;
    func->param_names = NULL;
    func->local_count = 0;
    func->local_types = NULL;
    func->local_names = NULL;
    func->debug_local_names = NULL;
    func->capture_count = 0;
    func->capture_local_slots = NULL;
    func->captured_values = NULL;
    func->is_async = false;
    func->defer_handler_ip = -1;
    func->defer_return_slot = -1;
    func->name = NULL;
    func->source_file = NULL;
    func->jit_entry_count = 0;
    func->jit_hot = false;
    func->jit_state = JIT_FUNC_STATE_COLD;
    func->jit_reason = JIT_REASON_NONE;
    func->jit_compile_attempts = 0;
    func->jit_compiled_call_count = 0;
    func->jit_compiled_entry = NULL;
    memset(&func->jit_profile, 0, sizeof(func->jit_profile));
    func->jit_profile.summary.kind = JIT_SUMMARY_KIND_NONE;
    func->jit_profile.summary.op = JIT_SUMMARY_OP_NONE;
    func->jit_hint_plan.kind = JIT_COMPILED_KIND_NONE;
    func->jit_hint_plan.local_slot = 0;
    func->jit_hint_plan.local_slot_b = 0;
    func->jit_hint_plan.int_const0 = 0;
    func->jit_hint_plan.int_const1 = 0;
    func->jit_compiled_plan.kind = JIT_COMPILED_KIND_NONE;
    func->jit_compiled_plan.local_slot = 0;
    func->jit_compiled_plan.local_slot_b = 0;
    func->jit_compiled_plan.int_const0 = 0;
    func->jit_compiled_plan.int_const1 = 0;
    func->ref_count = 1;
    func->global_slot_cache = NULL;
    func->global_slot_cache_count = 0;
    return func;
}

void obj_function_retain(ObjFunction* func) {
    if (func) func->ref_count++;
}

void obj_function_release(ObjFunction* func) {
    if (!func) return;
    func->ref_count--;
    if (func->ref_count <= 0) {
        obj_function_free(func);
    }
}

void obj_function_free(ObjFunction* func) {
    if (!func) return;
    chunk_free(&func->chunk);
    constant_pool_free(&func->constants);
    if (func->param_names) {
        for (int i = 0; i < func->param_count; i++) {
            if (func->param_names[i]) free(func->param_names[i]);
        }
        free(func->param_names);
    }
    if (func->local_names) {
        for (int i = 0; i < func->local_count; i++) {
            if (func->local_names[i]) free(func->local_names[i]);
        }
        free(func->local_names);
    }
    if (func->debug_local_names) {
        for (int i = 0; i < func->local_count; i++) {
            if (func->debug_local_names[i]) free(func->debug_local_names[i]);
        }
        free(func->debug_local_names);
    }
    if (func->local_types) free(func->local_types);
    if (func->capture_local_slots) free(func->capture_local_slots);
    if (func->captured_values) {
        for (int i = 0; i < func->capture_count; i++) {
            value_free(&func->captured_values[i]);
        }
        free(func->captured_values);
    }
    if (func->global_slot_cache) free(func->global_slot_cache);
    if (func->name) free(func->name);
    if (func->source_file) free(func->source_file);
    free(func);
}

static ObjFunction* obj_function_clone_with_captures(const ObjFunction* template_func,
                                                     const Value* capture_values,
                                                     int capture_count) {
    if (!template_func) return NULL;
    if (template_func->capture_count != capture_count) return NULL;
    if (capture_count > 0 && (!template_func->capture_local_slots || !capture_values)) return NULL;

    ObjFunction* closure = obj_function_create();
    if (!closure) return NULL;

    closure->param_count = template_func->param_count;
    if (closure->param_count > 0) {
        closure->param_names = (char**)safe_malloc((size_t)closure->param_count * sizeof(char*));
        for (int i = 0; i < closure->param_count; i++) {
            closure->param_names[i] = template_func->param_names && template_func->param_names[i]
                ? safe_strdup(template_func->param_names[i])
                : NULL;
        }
    }

    closure->local_count = template_func->local_count;
    if (closure->local_count > 0) {
        closure->local_names = (char**)safe_malloc((size_t)closure->local_count * sizeof(char*));
        closure->debug_local_names = (char**)safe_malloc((size_t)closure->local_count * sizeof(char*));
        for (int i = 0; i < closure->local_count; i++) {
            closure->local_names[i] = template_func->local_names && template_func->local_names[i]
                ? safe_strdup(template_func->local_names[i])
                : NULL;
            closure->debug_local_names[i] = template_func->debug_local_names && template_func->debug_local_names[i]
                ? safe_strdup(template_func->debug_local_names[i])
                : NULL;
        }
        if (template_func->local_types) {
            closure->local_types = (int*)safe_malloc((size_t)closure->local_count * sizeof(int));
            memcpy(closure->local_types, template_func->local_types, (size_t)closure->local_count * sizeof(int));
        }
    }

    closure->capture_count = capture_count;
    if (capture_count > 0) {
        closure->capture_local_slots = (int*)safe_malloc((size_t)capture_count * sizeof(int));
        memcpy(closure->capture_local_slots,
               template_func->capture_local_slots,
               (size_t)capture_count * sizeof(int));

        closure->captured_values = (Value*)safe_malloc((size_t)capture_count * sizeof(Value));
        for (int i = 0; i < capture_count; i++) {
            closure->captured_values[i] = capture_values[i];
            if (!value_type_is_unmanaged(value_get_type(&closure->captured_values[i]))) {
                value_retain(&closure->captured_values[i]);
            }
        }
    }

    if (template_func->name) {
        closure->name = safe_strdup(template_func->name);
    }
    closure->is_async = template_func->is_async;
    closure->defer_handler_ip = template_func->defer_handler_ip;
    closure->defer_return_slot = template_func->defer_return_slot;
    if (template_func->source_file) {
        closure->source_file = safe_strdup(template_func->source_file);
    }

    int code_count = template_func->chunk.code_count;
    if (code_count > 0) {
        closure->chunk.code = (uint8_t*)safe_malloc((size_t)code_count);
        memcpy(closure->chunk.code, template_func->chunk.code, (size_t)code_count);
        closure->chunk.code_count = code_count;
        closure->chunk.code_capacity = code_count;

        closure->chunk.debug_info = (DebugInfo*)safe_malloc((size_t)code_count * sizeof(DebugInfo));
        if (template_func->chunk.debug_info) {
            memcpy(closure->chunk.debug_info,
                   template_func->chunk.debug_info,
                   (size_t)code_count * sizeof(DebugInfo));
        } else {
            for (int i = 0; i < code_count; i++) {
                closure->chunk.debug_info[i].index = i;
                closure->chunk.debug_info[i].line = 0;
            }
        }
    }

    for (int i = 0; i < template_func->constants.constant_count; i++) {
        Constant c = template_func->constants.constants[i];
        constant_pool_add(&closure->constants, c);
    }

    return closure;
}

static bool vm_bind_captures_to_frame(VM* vm, const CallFrame* frame, ObjFunction* func) {
    if (!vm || !frame || !func) return false;
    if (func->capture_count <= 0) return true;

    if (!func->capture_local_slots || !func->captured_values) {
        vm_runtime_error(vm, "Cannot call unbound closure template");
        return false;
    }

    for (int i = 0; i < func->capture_count; i++) {
        int slot = func->capture_local_slots[i];
        if (slot < func->param_count || slot >= frame->slots_count) {
            vm_runtime_error(vm, "Closure capture slot out of bounds");
            return false;
        }

        Value* dst = &vm->stack.values[frame->slots_start + slot];
        value_free(dst);
        *dst = func->captured_values[i];
        if (!value_type_is_unmanaged(value_get_type(dst))) {
            value_retain(dst);
        }
    }

    return true;
}

static int vm_invoke_builtin_native(VM* vm, ObjNative* native) {
    if (!native || !native->builtin_function) {
        vm_runtime_error(vm, "Invalid builtin native function");
        return -1;
    }
    native->builtin_function((void*)vm);
    return vm_has_error(vm) ? -1 : 0;
}

static int vm_invoke_extension_native(VM* vm, ObjNative* native) {
    if (!native) {
        vm_runtime_error(vm, "Invalid extension native function");
        return -1;
    }
    return native_extension_invoke_function(native->userdata, vm);
}

static bool vm_native_invoke_failed(VM* vm, int invoke_result) {
    if (invoke_result == 0) {
        return false;
    }
    if (!vm->error_occurred) {
        vm_runtime_error(vm, "Native function invocation failed");
    }
    return true;
}

ObjNative* obj_native_create_builtin(void (*function)(void* vm), int arity) {
    ObjNative* native = (ObjNative*)safe_malloc(sizeof(ObjNative));
    native->invoke = vm_invoke_builtin_native;
    native->builtin_function = function;
    native->userdata = NULL;
    native->arity = arity;
    native->ref_count = 1;
    return native;
}

ObjNative* obj_native_create_extension(void* userdata, int arity) {
    ObjNative* native = (ObjNative*)safe_malloc(sizeof(ObjNative));
    native->invoke = vm_invoke_extension_native;
    native->builtin_function = NULL;
    native->userdata = userdata;
    native->arity = arity;
    native->ref_count = 1;
    return native;
}

void obj_native_retain(ObjNative* native) {
    if (native) native->ref_count++;
}

void obj_native_release(ObjNative* native) {
    if (!native) return;
    native->ref_count--;
    if (native->ref_count <= 0) {
        obj_native_free(native);
    }
}

void obj_native_free(ObjNative* native) {
    if (!native) return;
    free(native);
}

ObjRecord* obj_record_create(VM* vm, RecordDef* def) {
    ObjRecord* record = (ObjRecord*)safe_malloc(sizeof(ObjRecord));
    record->def = def;
    record->type_name = (def && def->name) ? safe_strdup(def->name) : NULL;
    record->field_count = def ? def->field_count : 0;
    record->fields = record->field_count > 0
        ? (Value*)safe_malloc(record->field_count * sizeof(Value))
        : NULL;
    for (int i = 0; i < record->field_count; i++) {
        value_init_nil(&record->fields[i]);
    }
    record->is_native_opaque = false;
    record->native_opaque_payload = NULL;
    record->native_opaque_destroy = NULL;
    record->ref_count = 1;
    cycle_gc_node_init(&record->gc_node, CYCLE_GC_OBJ_RECORD, record);
    if (vm) {
        cycle_gc_track(&vm->cycle_gc, &record->gc_node);
        vm_gc_maybe_collect(vm);
    }
    return record;
}

ObjRecord* obj_record_create_with_count(VM* vm, int field_count) {
    ObjRecord* record = (ObjRecord*)safe_malloc(sizeof(ObjRecord));
    record->def = NULL;
    record->type_name = NULL;
    record->field_count = field_count < 0 ? 0 : field_count;
    record->fields = record->field_count > 0
        ? (Value*)safe_malloc(record->field_count * sizeof(Value))
        : NULL;
    for (int i = 0; i < record->field_count; i++) {
        value_init_nil(&record->fields[i]);
    }
    record->is_native_opaque = false;
    record->native_opaque_payload = NULL;
    record->native_opaque_destroy = NULL;
    record->ref_count = 1;
    cycle_gc_node_init(&record->gc_node, CYCLE_GC_OBJ_RECORD, record);
    if (vm) {
        cycle_gc_track(&vm->cycle_gc, &record->gc_node);
        vm_gc_maybe_collect(vm);
    }
    return record;
}

ObjRecord* obj_record_create_opaque(VM* vm,
                                    const char* type_name,
                                    void* payload,
                                    void (*destroy)(void* payload)) {
    ObjRecord* record = obj_record_create_with_count(vm, 0);
    if (!record) return NULL;
    if (record->type_name) {
        free(record->type_name);
        record->type_name = NULL;
    }
    record->type_name = safe_strdup(type_name ? type_name : "opaque");
    record->is_native_opaque = true;
    record->native_opaque_payload = payload;
    record->native_opaque_destroy = destroy;
    return record;
}

void obj_record_retain(ObjRecord* record) {
    if (record) record->ref_count++;
}

void obj_record_release(ObjRecord* record) {
    if (!record) return;
    record->ref_count--;
    if (record->ref_count <= 0) {
        obj_record_free(record);
    }
}

void obj_record_free(ObjRecord* record) {
    if (!record) return;
    cycle_gc_untrack(&record->gc_node);
    if (record->type_name) {
        free(record->type_name);
        record->type_name = NULL;
    }
    if (record->fields) {
        for (int i = 0; i < record->field_count; i++) {
            value_free(&record->fields[i]);
        }
        free(record->fields);
    }
    if (record->is_native_opaque && record->native_opaque_destroy && record->native_opaque_payload) {
        record->native_opaque_destroy(record->native_opaque_payload);
        record->native_opaque_payload = NULL;
    }
    free(record);
}

void obj_record_set_field(ObjRecord* record, int field_idx, Value val) {
    // Transfer ownership of `val` into the record field.
    // Callers should not value_free(&val) after this call.
    if (!record || field_idx < 0 || field_idx >= record->field_count) {
        value_free(&val);
        return;
    }
    value_free(&record->fields[field_idx]);
    record->fields[field_idx] = val;
}

void obj_record_get_field(ObjRecord* record, int field_idx, Value* out) {
    if (!out) return;
    if (!record || field_idx < 0 || field_idx >= record->field_count) {
        value_init_nil(out);
        return;
    }
    *out = record->fields[field_idx];
    value_retain(out);
}

int obj_record_get_field_index(ObjRecord* record, const char* field_name) {
    if (!record || !field_name || !record->def) return -1;
    return record_def_get_field_index(record->def, field_name);
}

enum {
    FUTURE_FIELD_STATE = 0,
    FUTURE_FIELD_VALUE = 1,
    FUTURE_FIELD_PANIC = 2,
    FUTURE_FIELD_COUNT = 3
};

enum {
    FUTURE_STATE_PENDING = 0,
    FUTURE_STATE_RESOLVED = 1,
    FUTURE_STATE_PANICKED = 2
};

static void obj_future_set_type_name(ObjRecord* future) {
    if (!future) return;
    if (future->type_name) {
        free(future->type_name);
        future->type_name = NULL;
    }
    future->type_name = safe_strdup(VM_FUTURE_RUNTIME_TYPE_NAME);
}

ObjRecord* obj_future_create_pending(VM* vm) {
    ObjRecord* future = obj_record_create_with_count(vm, FUTURE_FIELD_COUNT);
    if (!future) return NULL;
    obj_future_set_type_name(future);

    Value state;
    value_init_int(&state, FUTURE_STATE_PENDING);
    obj_record_set_field(future, FUTURE_FIELD_STATE, state);

    Value panic;
    value_init_nil(&panic);
    obj_record_set_field(future, FUTURE_FIELD_PANIC, panic);
    return future;
}

ObjRecord* obj_future_create_resolved(VM* vm, Value value) {
    ObjRecord* future = obj_future_create_pending(vm);
    if (!future) return NULL;
    if (!obj_future_resolve(future, value)) {
        obj_record_release(future);
        return NULL;
    }
    return future;
}

bool value_is_future(const Value* val) {
    if (!val || value_get_type(val) != VAL_RECORD) return false;
    ObjRecord* record = value_get_record_obj(val);
    return record &&
           record->field_count >= FUTURE_FIELD_COUNT &&
           record->type_name &&
           strcmp(record->type_name, VM_FUTURE_RUNTIME_TYPE_NAME) == 0;
}

bool obj_future_is_ready(ObjRecord* future) {
    if (!future || future->field_count < FUTURE_FIELD_COUNT) return false;
    return value_get_type(&future->fields[FUTURE_FIELD_STATE]) == VAL_INT &&
           value_get_int(&future->fields[FUTURE_FIELD_STATE]) != FUTURE_STATE_PENDING;
}

bool obj_future_is_panicked(ObjRecord* future) {
    if (!future || future->field_count < FUTURE_FIELD_COUNT) return false;
    return value_get_type(&future->fields[FUTURE_FIELD_STATE]) == VAL_INT &&
           value_get_int(&future->fields[FUTURE_FIELD_STATE]) == FUTURE_STATE_PANICKED;
}

bool obj_future_resolve(ObjRecord* future, Value value) {
    if (!future || future->field_count < FUTURE_FIELD_COUNT) return false;
    if (obj_future_is_ready(future)) return false;

    Value stored = value;
    value_retain(&stored);
    obj_record_set_field(future, FUTURE_FIELD_VALUE, stored);

    Value panic;
    value_init_nil(&panic);
    obj_record_set_field(future, FUTURE_FIELD_PANIC, panic);

    Value state;
    value_init_int(&state, FUTURE_STATE_RESOLVED);
    obj_record_set_field(future, FUTURE_FIELD_STATE, state);
    return true;
}

bool obj_future_resolve_panic(VM* vm, ObjRecord* future, const char* message) {
    if (!future || future->field_count < FUTURE_FIELD_COUNT) return false;
    if (obj_future_is_ready(future)) return false;

    Value value;
    value_init_nil(&value);
    obj_record_set_field(future, FUTURE_FIELD_VALUE, value);

    Value panic;
    value_init_string(&panic, message && message[0] != '\0' ? message : "Runtime error");
    if (vm && vm->error_occurred) {
        value_free(&panic);
        return false;
    }
    obj_record_set_field(future, FUTURE_FIELD_PANIC, panic);

    Value state;
    value_init_int(&state, FUTURE_STATE_PANICKED);
    obj_record_set_field(future, FUTURE_FIELD_STATE, state);
    return true;
}

bool obj_future_try_get(ObjRecord* future, Value* out) {
    if (!out) return false;
    if (!future || future->field_count < FUTURE_FIELD_COUNT || !obj_future_is_ready(future) || obj_future_is_panicked(future)) {
        value_init_nil(out);
        return false;
    }

    obj_record_get_field(future, FUTURE_FIELD_VALUE, out);
    return true;
}

const char* obj_future_get_panic_message(ObjRecord* future) {
    if (!future || future->field_count < FUTURE_FIELD_COUNT || !obj_future_is_panicked(future)) {
        return NULL;
    }
    if (value_get_type(&future->fields[FUTURE_FIELD_PANIC]) != VAL_STRING) {
        return "Runtime error";
    }
    ObjString* panic = value_get_string_obj(&future->fields[FUTURE_FIELD_PANIC]);
    return (panic && panic->chars && panic->chars[0] != '\0') ? panic->chars : "Runtime error";
}

struct AsyncTask {
    ObjFunction* function;
    int ip;
    Value* stack_values;
    int stack_count;
    int slots_count;
    DeferredCall* defers;
    int defer_count;
    int defer_capacity;
    ObjRecord* async_result_future;
    bool has_resume_value;
    Value resume_value;
    bool has_resume_panic;
    char* resume_panic_message;
    bool panic_unwinding;
    char* panic_message;
    bool entry_profiled;
    AsyncTask* next;
};

struct FutureWaitEntry {
    ObjRecord* future;
    AsyncTask* tasks_head;
    AsyncTask* tasks_tail;
    FutureWaitEntry* next;
};

struct TimerWaitEntry {
    ObjRecord* future;
    int64_t deadline_ms;
    TimerWaitEntry* next;
};

struct VmPollWaitEntry {
    VmPollWaitCallback callback;
    VmPollWaitFreeFn free_fn;
    void* payload;
    VmPollWaitEntry* next;
};

// Tuple operations
ObjTuple* obj_tuple_create(VM* vm, int element_count) {
    ObjTuple* tuple = (ObjTuple*)safe_malloc(sizeof(ObjTuple));
    tuple->element_count = element_count;
    tuple->elements = (Value*)safe_calloc(element_count, sizeof(Value));
    tuple->ref_count = 1;
    cycle_gc_node_init(&tuple->gc_node, CYCLE_GC_OBJ_TUPLE, tuple);
    if (vm) {
        cycle_gc_track(&vm->cycle_gc, &tuple->gc_node);
        vm_gc_maybe_collect(vm);
    }
    return tuple;
}

void obj_tuple_retain(ObjTuple* tuple) {
    if (tuple) tuple->ref_count++;
}

void obj_tuple_release(ObjTuple* tuple) {
    if (!tuple) return;
    tuple->ref_count--;
    if (tuple->ref_count <= 0) {
        obj_tuple_free(tuple);
    }
}

void obj_tuple_free(ObjTuple* tuple) {
    if (!tuple) return;
    cycle_gc_untrack(&tuple->gc_node);
    if (tuple->elements) {
        for (int i = 0; i < tuple->element_count; i++) {
            value_free(&tuple->elements[i]);
        }
        free(tuple->elements);
    }
    free(tuple);
}

void obj_tuple_set(ObjTuple* tuple, int index, Value val) {
    if (!tuple || index < 0 || index >= tuple->element_count) return;
    value_free(&tuple->elements[index]);
    tuple->elements[index] = val;
    value_retain(&val);
}

void obj_tuple_get(ObjTuple* tuple, int index, Value* out) {
    if (!out) return;
    if (!tuple || index < 0 || index >= tuple->element_count) {
        value_init_nil(out);
        return;
    }
    *out = tuple->elements[index];
    value_retain(out);
}

// Hash function for values (for maps and sets)
typedef struct HashProbeStats {
    bool initialized;
    bool collect_enabled;
    bool dump_enabled;
    bool dumped;
    uint64_t map_insert_calls;
    uint64_t map_insert_probes;
    uint32_t map_insert_max_probe;
    uint64_t map_lookup_calls;
    uint64_t map_lookup_probes;
    uint32_t map_lookup_max_probe;
    uint64_t map_delete_calls;
    uint64_t map_delete_probes;
    uint32_t map_delete_max_probe;
    uint64_t set_insert_calls;
    uint64_t set_insert_probes;
    uint32_t set_insert_max_probe;
    uint64_t set_lookup_calls;
    uint64_t set_lookup_probes;
    uint32_t set_lookup_max_probe;
    uint64_t set_remove_calls;
    uint64_t set_remove_probes;
    uint32_t set_remove_max_probe;
} HashProbeStats;

static HashProbeStats g_hash_probe_stats = {0};

static bool hash_probe_env_enabled(const char* value) {
    if (!value || value[0] == '\0') return false;
    if (strcmp(value, "1") == 0) return true;
    if (strcmp(value, "true") == 0) return true;
    if (strcmp(value, "TRUE") == 0) return true;
    if (strcmp(value, "yes") == 0) return true;
    if (strcmp(value, "on") == 0) return true;
    return false;
}

static void hash_probe_stats_init_once(void) {
    if (g_hash_probe_stats.initialized) return;
    g_hash_probe_stats.initialized = true;
    const char* env = getenv("TABLO_HASH_PROBE_STATS");
    g_hash_probe_stats.dump_enabled = hash_probe_env_enabled(env);
#ifdef NDEBUG
    g_hash_probe_stats.collect_enabled = g_hash_probe_stats.dump_enabled;
#else
    // Collect probe stats in debug builds by default.
    g_hash_probe_stats.collect_enabled = true;
#endif
}

static bool hash_probe_collect_enabled(void) {
    hash_probe_stats_init_once();
    return g_hash_probe_stats.collect_enabled;
}

static void hash_probe_record(uint64_t* calls, uint64_t* probes_total, uint32_t* max_probe, uint32_t probes) {
    if (!hash_probe_collect_enabled()) return;
    (*calls)++;
    *probes_total += probes;
    if (probes > *max_probe) *max_probe = probes;
}

static void hash_probe_record_map_insert(uint32_t probes) {
    hash_probe_record(&g_hash_probe_stats.map_insert_calls,
                      &g_hash_probe_stats.map_insert_probes,
                      &g_hash_probe_stats.map_insert_max_probe,
                      probes);
}

static void hash_probe_record_map_lookup(uint32_t probes) {
    hash_probe_record(&g_hash_probe_stats.map_lookup_calls,
                      &g_hash_probe_stats.map_lookup_probes,
                      &g_hash_probe_stats.map_lookup_max_probe,
                      probes);
}

static void hash_probe_record_map_delete(uint32_t probes) {
    hash_probe_record(&g_hash_probe_stats.map_delete_calls,
                      &g_hash_probe_stats.map_delete_probes,
                      &g_hash_probe_stats.map_delete_max_probe,
                      probes);
}

static void hash_probe_record_set_insert(uint32_t probes) {
    hash_probe_record(&g_hash_probe_stats.set_insert_calls,
                      &g_hash_probe_stats.set_insert_probes,
                      &g_hash_probe_stats.set_insert_max_probe,
                      probes);
}

static void hash_probe_record_set_lookup(uint32_t probes) {
    hash_probe_record(&g_hash_probe_stats.set_lookup_calls,
                      &g_hash_probe_stats.set_lookup_probes,
                      &g_hash_probe_stats.set_lookup_max_probe,
                      probes);
}

static void hash_probe_record_set_remove(uint32_t probes) {
    hash_probe_record(&g_hash_probe_stats.set_remove_calls,
                      &g_hash_probe_stats.set_remove_probes,
                      &g_hash_probe_stats.set_remove_max_probe,
                      probes);
}

static void hash_probe_dump_once(void) {
    hash_probe_stats_init_once();
    if (!g_hash_probe_stats.dump_enabled || g_hash_probe_stats.dumped) return;
    g_hash_probe_stats.dumped = true;
    if (!g_hash_probe_stats.collect_enabled) return;

    uint64_t total_calls = g_hash_probe_stats.map_insert_calls +
                           g_hash_probe_stats.map_lookup_calls +
                           g_hash_probe_stats.map_delete_calls +
                           g_hash_probe_stats.set_insert_calls +
                           g_hash_probe_stats.set_lookup_calls +
                           g_hash_probe_stats.set_remove_calls;
    if (total_calls == 0) return;

    fprintf(stderr, "Hash probe stats:\n");
    if (g_hash_probe_stats.map_insert_calls > 0) {
        fprintf(stderr, "  map.insert avg=%.2f max=%u calls=%llu\n",
                (double)g_hash_probe_stats.map_insert_probes / (double)g_hash_probe_stats.map_insert_calls,
                g_hash_probe_stats.map_insert_max_probe,
                (unsigned long long)g_hash_probe_stats.map_insert_calls);
    }
    if (g_hash_probe_stats.map_lookup_calls > 0) {
        fprintf(stderr, "  map.lookup avg=%.2f max=%u calls=%llu\n",
                (double)g_hash_probe_stats.map_lookup_probes / (double)g_hash_probe_stats.map_lookup_calls,
                g_hash_probe_stats.map_lookup_max_probe,
                (unsigned long long)g_hash_probe_stats.map_lookup_calls);
    }
    if (g_hash_probe_stats.map_delete_calls > 0) {
        fprintf(stderr, "  map.delete avg=%.2f max=%u calls=%llu\n",
                (double)g_hash_probe_stats.map_delete_probes / (double)g_hash_probe_stats.map_delete_calls,
                g_hash_probe_stats.map_delete_max_probe,
                (unsigned long long)g_hash_probe_stats.map_delete_calls);
    }
    if (g_hash_probe_stats.set_insert_calls > 0) {
        fprintf(stderr, "  set.insert avg=%.2f max=%u calls=%llu\n",
                (double)g_hash_probe_stats.set_insert_probes / (double)g_hash_probe_stats.set_insert_calls,
                g_hash_probe_stats.set_insert_max_probe,
                (unsigned long long)g_hash_probe_stats.set_insert_calls);
    }
    if (g_hash_probe_stats.set_lookup_calls > 0) {
        fprintf(stderr, "  set.lookup avg=%.2f max=%u calls=%llu\n",
                (double)g_hash_probe_stats.set_lookup_probes / (double)g_hash_probe_stats.set_lookup_calls,
                g_hash_probe_stats.set_lookup_max_probe,
                (unsigned long long)g_hash_probe_stats.set_lookup_calls);
    }
    if (g_hash_probe_stats.set_remove_calls > 0) {
        fprintf(stderr, "  set.remove avg=%.2f max=%u calls=%llu\n",
                (double)g_hash_probe_stats.set_remove_probes / (double)g_hash_probe_stats.set_remove_calls,
                g_hash_probe_stats.set_remove_max_probe,
                (unsigned long long)g_hash_probe_stats.set_remove_calls);
    }
}

static uint64_t hash_mix64(uint64_t h) {
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

static uint32_t hash_mix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

static uint32_t hash_fold64_to32(uint64_t h) {
    uint64_t mixed = hash_mix64(h);
    return hash_mix32((uint32_t)(mixed ^ (mixed >> 32)));
}

static uint64_t hash_string_internal64(const char* str, int length) {
    uint64_t hash = 1469598103934665603ULL;      // FNV-1a 64-bit offset basis
    const uint64_t prime = 1099511628211ULL;     // FNV-1a 64-bit prime
    for (int i = 0; i < length; i++) {
        hash ^= (uint64_t)(unsigned char)str[i];
        hash *= prime;
    }
    return hash_mix64(hash ^ (uint64_t)length);
}

static uint32_t value_hash(Value* val) {
    switch (value_get_type(val)) {
        case VAL_INT:
            return hash_fold64_to32(((uint64_t)value_get_int(val)) ^ 0x9e3779b97f4a7c15ULL);
        case VAL_STRING: {
            ObjString* str = value_get_string_obj(val);
            if (!str || !str->chars) return 0;
            if (str->hash == UINT32_MAX) {
                str->hash = hash_fold64_to32(hash_string_internal64(str->chars, str->length));
            }
            return str->hash;
        }
        case VAL_BIGINT: {
            ObjBigInt* bigint = value_get_bigint_obj(val);
            if (!bigint || bigint->count == 0) return 0;
            uint64_t hash = 1469598103934665603ULL;
            const uint64_t prime = 1099511628211ULL;
            for (size_t i = 0; i < bigint->count; i++) {
                uint32_t limb = bigint->limbs[i];
                hash ^= (uint64_t)(unsigned char)(limb & 0xFF);
                hash *= prime;
                hash ^= (uint64_t)(unsigned char)((limb >> 8) & 0xFF);
                hash *= prime;
                hash ^= (uint64_t)(unsigned char)((limb >> 16) & 0xFF);
                hash *= prime;
                hash ^= (uint64_t)(unsigned char)((limb >> 24) & 0xFF);
                hash *= prime;
            }
            hash ^= bigint->sign < 0 ? 0xa5a5a5a5a5a5a5a5ULL : 0x5a5a5a5a5a5a5a5aULL;
            return hash_fold64_to32(hash);
        }
        default:
            return 0;
    }
}

static bool value_equals_strict(Value* a, Value* b) {
    if (value_get_type(a) != value_get_type(b)) return false;
    switch (value_get_type(a)) {
        case VAL_INT:
            return value_get_int(a) == value_get_int(b);
        case VAL_BIGINT: {
            ObjBigInt* a_bigint = value_get_bigint_obj(a);
            ObjBigInt* b_bigint = value_get_bigint_obj(b);
            if (a_bigint == b_bigint) return true;
            return obj_bigint_compare(a_bigint, b_bigint) == 0;
        }
        case VAL_STRING:
            if (value_get_string_obj(a) == value_get_string_obj(b)) return true;
            if (!value_get_string_obj(a) || !value_get_string_obj(b)) {
                return value_get_string_obj(a) == value_get_string_obj(b);
            }
            if (value_get_string_obj(a)->length != value_get_string_obj(b)->length) return false;
            return strcmp(value_get_string_obj(a)->chars, value_get_string_obj(b)->chars) == 0;
        default:
            return false;
    }
}

// Fast comparator for map/set keys: only int/string are valid key types.
static bool value_equals_key_fast(const Value* a, const Value* b) {
    if (!a || !b || value_get_type(a) != value_get_type(b)) return false;
    if (value_get_type(a) == VAL_INT) {
        return value_get_int(a) == value_get_int(b);
    }
    if (value_get_type(a) != VAL_STRING) {
        return false;
    }
    ObjString* a_string = value_get_string_obj(a);
    ObjString* b_string = value_get_string_obj(b);
    if (a_string == b_string) return true;
    if (!a_string || !b_string) return false;
    if (a_string->length != b_string->length) return false;
    if (!a_string->chars || !b_string->chars) return false;
    if (a_string->length == 0) return true;
    return memcmp(a_string->chars, b_string->chars, (size_t)a_string->length) == 0;
}

static uint32_t map_string_hash_chars(const char* chars, int length) {
    if (!chars || length < 0) return 0;
    return hash_fold64_to32(hash_string_internal64(chars, length));
}

static uint32_t map_string_hash_obj(ObjString* key) {
    if (!key || !key->chars) return 0;
    if (key->hash == UINT32_MAX) {
        key->hash = map_string_hash_chars(key->chars, key->length);
    }
    return key->hash;
}

static bool map_slot_key_equals_string_chars(const MapSlot* slot, const char* key_chars, int key_length) {
    if (!slot || slot->hash < 2 || value_get_type(&slot->key) != VAL_STRING) return false;
    ObjString* slot_key = value_get_string_obj(&slot->key);
    if (!slot_key || !slot_key->chars) return false;
    if (!key_chars || key_length < 0) return false;
    if (slot_key->length != key_length) return false;
    if (slot_key->chars == key_chars) return true;
    if (key_length == 0) return true;
    return memcmp(slot_key->chars, key_chars, (size_t)key_length) == 0;
}

static bool map_slot_key_equals_string_obj(const MapSlot* slot, ObjString* key) {
    if (!slot || slot->hash < 2 || value_get_type(&slot->key) != VAL_STRING) return false;
    ObjString* slot_key = value_get_string_obj(&slot->key);
    if (!slot_key || !slot_key->chars || !key || !key->chars) return false;
    if (slot_key == key) return true;
    if (slot_key->length != key->length) return false;
    if (slot_key->chars == key->chars) return true;
    if (key->length == 0) return true;
    return memcmp(slot_key->chars, key->chars, (size_t)key->length) == 0;
}

static bool set_slot_value_equals_string_chars(const SetSlot* slot, const char* value_chars, int value_length) {
    if (!slot || slot->hash < 2 || value_get_type(&slot->value) != VAL_STRING) return false;
    ObjString* slot_value = value_get_string_obj(&slot->value);
    if (!slot_value || !slot_value->chars) return false;
    if (!value_chars || value_length < 0) return false;
    if (slot_value->length != value_length) return false;
    if (slot_value->chars == value_chars) return true;
    if (value_length == 0) return true;
    return memcmp(slot_value->chars, value_chars, (size_t)value_length) == 0;
}

// Map operations
ObjMap* obj_map_create(VM* vm) {
    ObjMap* map = (ObjMap*)safe_malloc(sizeof(ObjMap));
    map->capacity = 16;
    map->count = 0;
    map->used = 0;
    map->slots = (MapSlot*)safe_calloc(map->capacity, sizeof(MapSlot));
    map->ref_count = 1;
    cycle_gc_node_init(&map->gc_node, CYCLE_GC_OBJ_MAP, map);
    if (vm) {
        cycle_gc_track(&vm->cycle_gc, &map->gc_node);
        vm_gc_maybe_collect(vm);
    }
    return map;
}

void obj_map_retain(ObjMap* map) {
    if (map) map->ref_count++;
}

void obj_map_release(ObjMap* map) {
    if (!map) return;
    map->ref_count--;
    if (map->ref_count <= 0) {
        obj_map_free(map);
    }
}

void obj_map_free(ObjMap* map) {
    if (!map) return;
    cycle_gc_untrack(&map->gc_node);
    if (map->slots) {
        for (int i = 0; i < map->capacity; i++) {
            MapSlot* slot = &map->slots[i];
            if (slot->hash < 2) continue;
            value_free(&slot->key);
            value_free(&slot->value);
        }
        free(map->slots);
    }
    free(map);
}

static void obj_map_adjust_capacity(ObjMap* map, int new_capacity) {
    if (!map || new_capacity < 16) return;
    if ((new_capacity & (new_capacity - 1)) != 0) return; // must be power of two

    MapSlot* old_slots = map->slots;
    int old_capacity = map->capacity;

    MapSlot* new_slots = (MapSlot*)safe_calloc(new_capacity, sizeof(MapSlot));

    map->slots = new_slots;
    map->capacity = new_capacity;
    map->count = 0;
    map->used = 0;

    uint32_t mask = (uint32_t)(new_capacity - 1);
    for (int i = 0; i < old_capacity; i++) {
        MapSlot* old = &old_slots[i];
        if (old->hash < 2) continue;

        uint32_t idx = old->hash & mask;
        while (new_slots[idx].hash != 0) {
            idx = (idx + 1) & mask;
        }
        new_slots[idx] = *old;
        map->count++;
        map->used++;
    }

    if (old_slots) free(old_slots);
}

static void obj_map_ensure_capacity(ObjMap* map, int additional) {
    if (!map) return;
    if (additional <= 0) return;

    int max_load = (map->capacity * 3) / 4;
    int needed = map->count + additional;
    if (needed <= max_load && map->used <= max_load) return;

    int new_capacity = map->capacity;
    if (needed > max_load) {
        do {
            new_capacity *= 2;
            max_load = (new_capacity * 3) / 4;
        } while (needed > max_load);
    }

    obj_map_adjust_capacity(map, new_capacity);
}

static void obj_map_maybe_compact_after_delete(ObjMap* map) {
    if (!map || !map->slots || map->capacity < 16) return;

    if (map->count == 0) {
        if (map->capacity > 16) {
            obj_map_adjust_capacity(map, 16);
        } else {
            memset(map->slots, 0, (size_t)map->capacity * sizeof(MapSlot));
            map->used = 0;
        }
        return;
    }

    if (map->capacity > 16 && map->count <= (map->capacity >> 3)) {
        int new_capacity = map->capacity;
        while (new_capacity > 16 && map->count <= (new_capacity >> 3)) {
            new_capacity >>= 1;
        }
        if (new_capacity != map->capacity) {
            obj_map_adjust_capacity(map, new_capacity);
            return;
        }
    }
}

static void obj_map_backshift_delete_at(ObjMap* map, uint32_t hole_idx) {
    if (!map || !map->slots || map->capacity <= 0) return;

    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t hole = hole_idx;
    uint32_t scan = (hole + 1) & mask;

    while (map->slots[scan].hash >= 2) {
        uint32_t home = map->slots[scan].hash & mask;
        uint32_t dist_scan = (scan - home) & mask;
        uint32_t dist_hole = (hole - home) & mask;
        if (dist_hole < dist_scan) {
            map->slots[hole] = map->slots[scan];
            hole = scan;
        }
        scan = (scan + 1) & mask;
    }

    Value nil;
    value_init_nil(&nil);
    map->slots[hole].hash = 0;
    map->slots[hole].key = nil;
    map->slots[hole].value = nil;

    if (map->count > 0) map->count--;
    if (map->used > 0) map->used--;
}

static MapSlot* obj_map_find_slot(ObjMap* map, Value* key, uint32_t hash, bool* out_found) {
    if (out_found) *out_found = false;
    if (!map || !map->slots || map->capacity <= 0) return NULL;

    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;

    while (true) {
        MapSlot* slot = &map->slots[idx];
        if (slot->hash == 0) {
            if (out_found) *out_found = false;
            hash_probe_record_map_insert(probes);
            return slot;
        }
        if (slot->hash == hash && value_equals_key_fast(&slot->key, key)) {
            if (out_found) *out_found = true;
            hash_probe_record_map_insert(probes);
            return slot;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

static MapSlot* obj_map_find_slot_string(ObjMap* map, ObjString* key, uint32_t hash, bool* out_found) {
    if (out_found) *out_found = false;
    if (!map || !map->slots || map->capacity <= 0 || !key || !key->chars) return NULL;

    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;

    while (true) {
        MapSlot* slot = &map->slots[idx];
        if (slot->hash == 0) {
            if (out_found) *out_found = false;
            hash_probe_record_map_insert(probes);
            return slot;
        }
        if (slot->hash == hash && map_slot_key_equals_string_obj(slot, key)) {
            if (out_found) *out_found = true;
            hash_probe_record_map_insert(probes);
            return slot;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

static MapSlot* obj_map_find_slot_string_chars(ObjMap* map, const char* key_chars, int key_length, uint32_t hash, bool* out_found) {
    if (out_found) *out_found = false;
    if (!map || !map->slots || map->capacity <= 0 || !key_chars || key_length < 0) return NULL;

    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;

    while (true) {
        MapSlot* slot = &map->slots[idx];
        if (slot->hash == 0) {
            if (out_found) *out_found = false;
            hash_probe_record_map_insert(probes);
            return slot;
        }
        if (slot->hash == hash && map_slot_key_equals_string_chars(slot, key_chars, key_length)) {
            if (out_found) *out_found = true;
            hash_probe_record_map_insert(probes);
            return slot;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

void obj_map_set(ObjMap* map, Value key, Value value) {
    if (!map) return;

    if (value_get_type(&key) == VAL_STRING) {
        obj_map_set_string(map, value_get_string_obj(&key), value);
        return;
    }

    // Only int and string keys are supported.
    if (value_get_type(&key) != VAL_INT) return;

    obj_map_ensure_capacity(map, 1);

    uint32_t hash = value_hash(&key) | 2U;
    bool found = false;
    MapSlot* slot = obj_map_find_slot(map, &key, hash, &found);
    if (!slot) return;

    if (found) {
        value_free(&slot->value);
        slot->value = value;
        value_retain(&slot->value);
        return;
    }

    if (slot->hash == 0) {
        map->used++;
    }
    slot->hash = hash;
    slot->key = key;
    value_retain(&slot->key);
    slot->value = value;
    value_retain(&slot->value);
    map->count++;
}

void obj_map_set_cstr_n(ObjMap* map, const char* key_chars, int key_length, Value value) {
    if (!map || !key_chars || key_length < 0) return;

    obj_map_ensure_capacity(map, 1);

    uint32_t hash = map_string_hash_chars(key_chars, key_length) | 2U;
    bool found = false;
    MapSlot* slot = obj_map_find_slot_string_chars(map, key_chars, key_length, hash, &found);
    if (!slot) return;

    if (found) {
        value_free(&slot->value);
        slot->value = value;
        value_retain(&slot->value);
        return;
    }

    if (slot->hash == 0) {
        map->used++;
    }
    slot->hash = hash;
    value_set_type(&slot->key, VAL_STRING);
    value_set_string_obj(&slot->key, obj_string_create(key_chars, key_length));
    slot->value = value;
    value_retain(&slot->value);
    map->count++;
}

void obj_map_set_cstr(ObjMap* map, const char* key_chars, Value value) {
    if (!key_chars) return;
    obj_map_set_cstr_n(map, key_chars, (int)strlen(key_chars), value);
}

void obj_map_set_string(ObjMap* map, ObjString* key, Value value) {
    if (!map || !key || !key->chars) return;

    obj_map_ensure_capacity(map, 1);

    uint32_t hash = map_string_hash_obj(key) | 2U;
    bool found = false;
    MapSlot* slot = obj_map_find_slot_string(map, key, hash, &found);
    if (!slot) return;

    if (found) {
        value_free(&slot->value);
        slot->value = value;
        value_retain(&slot->value);
        return;
    }

    if (slot->hash == 0) {
        map->used++;
    }
    slot->hash = hash;
    Value key_val;
    value_set_type(&key_val, VAL_STRING);
    value_set_string_obj(&key_val, key);
    slot->key = key_val;
    value_retain(&slot->key);
    slot->value = value;
    value_retain(&slot->value);
    map->count++;
}

Value obj_map_get(ObjMap* map, Value key) {
    Value nil;
    value_init_nil(&nil);
    if (!map || !map->slots) return nil;

    if (value_get_type(&key) == VAL_STRING) {
        return obj_map_get_string(map, value_get_string_obj(&key));
    }

    if (value_get_type(&key) != VAL_INT) return nil;

    uint32_t hash = value_hash(&key) | 2U;
    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        MapSlot* slot = &map->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_map_lookup(probes);
            return nil;
        }
        if (slot->hash == hash && value_equals_key_fast(&slot->key, &key)) {
            Value result = slot->value;
            value_retain(&result);
            hash_probe_record_map_lookup(probes);
            return result;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

Value obj_map_get_cstr_n(ObjMap* map, const char* key_chars, int key_length) {
    Value nil;
    value_init_nil(&nil);
    if (!map || !map->slots || !key_chars || key_length < 0) return nil;

    uint32_t hash = map_string_hash_chars(key_chars, key_length) | 2U;
    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        MapSlot* slot = &map->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_map_lookup(probes);
            return nil;
        }
        if (slot->hash == hash && map_slot_key_equals_string_chars(slot, key_chars, key_length)) {
            Value result = slot->value;
            value_retain(&result);
            hash_probe_record_map_lookup(probes);
            return result;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

Value obj_map_get_cstr(ObjMap* map, const char* key_chars) {
    if (!key_chars) {
        Value nil;
        value_init_nil(&nil);
        return nil;
    }
    return obj_map_get_cstr_n(map, key_chars, (int)strlen(key_chars));
}

Value obj_map_get_string(ObjMap* map, ObjString* key) {
    Value nil;
    value_init_nil(&nil);
    if (!map || !map->slots || !key || !key->chars) return nil;

    uint32_t hash = map_string_hash_obj(key) | 2U;
    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        MapSlot* slot = &map->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_map_lookup(probes);
            return nil;
        }
        if (slot->hash == hash && map_slot_key_equals_string_obj(slot, key)) {
            Value result = slot->value;
            value_retain(&result);
            hash_probe_record_map_lookup(probes);
            return result;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

bool obj_map_has(ObjMap* map, Value key) {
    if (!map || !map->slots) return false;

    if (value_get_type(&key) == VAL_STRING) {
        return obj_map_has_string(map, value_get_string_obj(&key));
    }

    if (value_get_type(&key) != VAL_INT) return false;

    uint32_t hash = value_hash(&key) | 2U;
    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        MapSlot* slot = &map->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_map_lookup(probes);
            return false;
        }
        if (slot->hash == hash && value_equals_key_fast(&slot->key, &key)) {
            hash_probe_record_map_lookup(probes);
            return true;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

bool obj_map_has_cstr_n(ObjMap* map, const char* key_chars, int key_length) {
    if (!map || !map->slots || !key_chars || key_length < 0) return false;

    uint32_t hash = map_string_hash_chars(key_chars, key_length) | 2U;
    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        MapSlot* slot = &map->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_map_lookup(probes);
            return false;
        }
        if (slot->hash == hash && map_slot_key_equals_string_chars(slot, key_chars, key_length)) {
            hash_probe_record_map_lookup(probes);
            return true;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

bool obj_map_has_cstr(ObjMap* map, const char* key_chars) {
    if (!key_chars) return false;
    return obj_map_has_cstr_n(map, key_chars, (int)strlen(key_chars));
}

bool obj_map_has_string(ObjMap* map, ObjString* key) {
    if (!map || !map->slots || !key || !key->chars) return false;

    uint32_t hash = map_string_hash_obj(key) | 2U;
    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        MapSlot* slot = &map->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_map_lookup(probes);
            return false;
        }
        if (slot->hash == hash && map_slot_key_equals_string_obj(slot, key)) {
            hash_probe_record_map_lookup(probes);
            return true;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

bool obj_map_try_get_cstr(ObjMap* map, const char* key, Value* out) {
    if (out) value_init_nil(out);
    if (!map || !map->slots || !key) return false;

    int key_length = (int)strlen(key);
    uint32_t hash = map_string_hash_chars(key, key_length) | 2U;
    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        MapSlot* slot = &map->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_map_lookup(probes);
            return false;
        }
        if (slot->hash == hash && map_slot_key_equals_string_chars(slot, key, key_length)) {
            if (out) {
                Value result = slot->value;
                value_retain(&result);
                *out = result;
            }
            hash_probe_record_map_lookup(probes);
            return true;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

void obj_map_delete(ObjMap* map, Value key) {
    if (!map) return;

    if (value_get_type(&key) == VAL_STRING) {
        obj_map_delete_string(map, value_get_string_obj(&key));
        return;
    }

    if (value_get_type(&key) != VAL_INT) return;

    if (!map->slots || map->capacity <= 0) return;

    uint32_t hash = value_hash(&key) | 2U;
    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        MapSlot* slot = &map->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_map_delete(probes);
            return;
        }
        if (slot->hash == hash && value_equals_key_fast(&slot->key, &key)) {
            value_free(&slot->key);
            value_free(&slot->value);
            obj_map_backshift_delete_at(map, idx);
            hash_probe_record_map_delete(probes);
            obj_map_maybe_compact_after_delete(map);
            return;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

void obj_map_delete_cstr_n(ObjMap* map, const char* key_chars, int key_length) {
    if (!map || !key_chars || key_length < 0) return;
    if (!map->slots || map->capacity <= 0) return;

    uint32_t hash = map_string_hash_chars(key_chars, key_length) | 2U;
    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        MapSlot* slot = &map->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_map_delete(probes);
            return;
        }
        if (slot->hash == hash && map_slot_key_equals_string_chars(slot, key_chars, key_length)) {
            value_free(&slot->key);
            value_free(&slot->value);
            obj_map_backshift_delete_at(map, idx);
            hash_probe_record_map_delete(probes);
            obj_map_maybe_compact_after_delete(map);
            return;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

void obj_map_delete_cstr(ObjMap* map, const char* key_chars) {
    if (!key_chars) return;
    obj_map_delete_cstr_n(map, key_chars, (int)strlen(key_chars));
}

void obj_map_delete_string(ObjMap* map, ObjString* key) {
    if (!map || !key || !key->chars) return;
    if (!map->slots || map->capacity <= 0) return;

    uint32_t hash = map_string_hash_obj(key) | 2U;
    uint32_t mask = (uint32_t)(map->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        MapSlot* slot = &map->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_map_delete(probes);
            return;
        }
        if (slot->hash == hash && map_slot_key_equals_string_obj(slot, key)) {
            value_free(&slot->key);
            value_free(&slot->value);
            obj_map_backshift_delete_at(map, idx);
            hash_probe_record_map_delete(probes);
            obj_map_maybe_compact_after_delete(map);
            return;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

int obj_map_count(ObjMap* map) {
    if (!map) return 0;
    return map->count;
}

void obj_map_keys(ObjMap* map, ObjArray* result) {
    if (!map || !result) return;

    for (int i = 0; i < map->capacity; i++) {
        MapSlot* slot = &map->slots[i];
        if (slot->hash < 2) continue;
        Value out = slot->key;
        value_retain(&out);
        obj_array_push(result, out);
    }
}

void obj_map_values(ObjMap* map, ObjArray* result) {
    if (!map || !result) return;

    for (int i = 0; i < map->capacity; i++) {
        MapSlot* slot = &map->slots[i];
        if (slot->hash < 2) continue;
        Value out = slot->value;
        value_retain(&out);
        obj_array_push(result, out);
    }
}

// Set operations
ObjSet* obj_set_create(void) {
    ObjSet* set = (ObjSet*)safe_malloc(sizeof(ObjSet));
    set->capacity = 16;
    set->count = 0;
    set->used = 0;
    set->slots = (SetSlot*)safe_calloc(set->capacity, sizeof(SetSlot));
    set->ref_count = 1;
    return set;
}

void obj_set_retain(ObjSet* set) {
    if (set) set->ref_count++;
}

void obj_set_release(ObjSet* set) {
    if (!set) return;
    set->ref_count--;
    if (set->ref_count <= 0) {
        obj_set_free(set);
    }
}

void obj_set_free(ObjSet* set) {
    if (!set) return;
    if (set->slots) {
        for (int i = 0; i < set->capacity; i++) {
            SetSlot* slot = &set->slots[i];
            if (slot->hash < 2) continue;
            value_free(&slot->value);
        }
        free(set->slots);
    }
    free(set);
}

static void obj_set_adjust_capacity(ObjSet* set, int new_capacity) {
    if (!set || new_capacity < 16) return;
    if ((new_capacity & (new_capacity - 1)) != 0) return;

    SetSlot* old_slots = set->slots;
    int old_capacity = set->capacity;

    SetSlot* new_slots = (SetSlot*)safe_calloc(new_capacity, sizeof(SetSlot));

    set->slots = new_slots;
    set->capacity = new_capacity;
    set->count = 0;
    set->used = 0;

    uint32_t mask = (uint32_t)(new_capacity - 1);
    for (int i = 0; i < old_capacity; i++) {
        SetSlot* old = &old_slots[i];
        if (old->hash < 2) continue;

        uint32_t idx = old->hash & mask;
        while (new_slots[idx].hash != 0) {
            idx = (idx + 1) & mask;
        }
        new_slots[idx] = *old;
        set->count++;
        set->used++;
    }

    if (old_slots) free(old_slots);
}

static void obj_set_ensure_capacity(ObjSet* set, int additional) {
    if (!set) return;
    if (additional <= 0) return;

    int max_load = (set->capacity * 3) / 4;
    int needed = set->count + additional;
    if (needed <= max_load && set->used <= max_load) return;

    int new_capacity = set->capacity;
    if (needed > max_load) {
        do {
            new_capacity *= 2;
            max_load = (new_capacity * 3) / 4;
        } while (needed > max_load);
    }

    obj_set_adjust_capacity(set, new_capacity);
}

static void obj_set_maybe_compact_after_remove(ObjSet* set) {
    if (!set || !set->slots || set->capacity < 16) return;

    if (set->count == 0) {
        if (set->capacity > 16) {
            obj_set_adjust_capacity(set, 16);
        } else {
            memset(set->slots, 0, (size_t)set->capacity * sizeof(SetSlot));
            set->used = 0;
        }
        return;
    }

    if (set->capacity > 16 && set->count <= (set->capacity >> 3)) {
        int new_capacity = set->capacity;
        while (new_capacity > 16 && set->count <= (new_capacity >> 3)) {
            new_capacity >>= 1;
        }
        if (new_capacity != set->capacity) {
            obj_set_adjust_capacity(set, new_capacity);
            return;
        }
    }
}

static void obj_set_backshift_remove_at(ObjSet* set, uint32_t hole_idx) {
    if (!set || !set->slots || set->capacity <= 0) return;

    uint32_t mask = (uint32_t)(set->capacity - 1);
    uint32_t hole = hole_idx;
    uint32_t scan = (hole + 1) & mask;

    while (set->slots[scan].hash >= 2) {
        uint32_t home = set->slots[scan].hash & mask;
        uint32_t dist_scan = (scan - home) & mask;
        uint32_t dist_hole = (hole - home) & mask;
        if (dist_hole < dist_scan) {
            set->slots[hole] = set->slots[scan];
            hole = scan;
        }
        scan = (scan + 1) & mask;
    }

    Value nil;
    value_init_nil(&nil);
    set->slots[hole].hash = 0;
    set->slots[hole].value = nil;

    if (set->count > 0) set->count--;
    if (set->used > 0) set->used--;
}

static SetSlot* obj_set_find_slot(ObjSet* set, Value* value, uint32_t hash, bool* out_found) {
    if (out_found) *out_found = false;
    if (!set || !set->slots || set->capacity <= 0) return NULL;

    uint32_t mask = (uint32_t)(set->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;

    while (true) {
        SetSlot* slot = &set->slots[idx];
        if (slot->hash == 0) {
            if (out_found) *out_found = false;
            hash_probe_record_set_insert(probes);
            return slot;
        }
        if (slot->hash == hash && value_equals_key_fast(&slot->value, value)) {
            if (out_found) *out_found = true;
            hash_probe_record_set_insert(probes);
            return slot;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

void obj_set_add(ObjSet* set, Value value) {
    if (!set) return;

    if (value_get_type(&value) == VAL_STRING && value_get_string_obj(&value)) {
        obj_set_add_string(set, value_get_string_obj(&value));
        return;
    }
    if (value_get_type(&value) != VAL_INT) return;

    obj_set_ensure_capacity(set, 1);

    uint32_t hash = value_hash(&value) | 2U;
    bool found = false;
    SetSlot* slot = obj_set_find_slot(set, &value, hash, &found);
    if (!slot || found) return;

    if (slot->hash == 0) {
        set->used++;
    }
    slot->hash = hash;
    slot->value = value;
    value_retain(&slot->value);
    set->count++;
}

void obj_set_add_string(ObjSet* set, ObjString* value) {
    if (!set || !value || !value->chars) return;

    obj_set_ensure_capacity(set, 1);

    uint32_t hash = map_string_hash_obj(value) | 2U;
    uint32_t mask = (uint32_t)(set->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;

    while (true) {
        SetSlot* slot = &set->slots[idx];
        if (slot->hash == 0) {
            set->used++;
            slot->hash = hash;
            Value stored;
            value_set_type(&stored, VAL_STRING);
            value_set_string_obj(&stored, value);
            slot->value = stored;
            value_retain(&slot->value);
            set->count++;
            hash_probe_record_set_insert(probes);
            return;
        }
        if (slot->hash == hash && set_slot_value_equals_string_chars(slot, value->chars, value->length)) {
            hash_probe_record_set_insert(probes);
            return;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

void obj_set_remove(ObjSet* set, Value value) {
    if (!set) return;

    if (value_get_type(&value) == VAL_STRING && value_get_string_obj(&value)) {
        obj_set_remove_string(set, value_get_string_obj(&value));
        return;
    }
    if (value_get_type(&value) != VAL_INT) return;

    if (!set->slots || set->capacity <= 0) return;

    uint32_t hash = value_hash(&value) | 2U;
    uint32_t mask = (uint32_t)(set->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        SetSlot* slot = &set->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_set_remove(probes);
            return;
        }
        if (slot->hash == hash && value_equals_key_fast(&slot->value, &value)) {
            value_free(&slot->value);
            obj_set_backshift_remove_at(set, idx);
            hash_probe_record_set_remove(probes);
            obj_set_maybe_compact_after_remove(set);
            return;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

void obj_set_remove_string(ObjSet* set, ObjString* value) {
    if (!set || !value || !value->chars) return;
    if (!set->slots || set->capacity <= 0) return;

    uint32_t hash = map_string_hash_obj(value) | 2U;
    uint32_t mask = (uint32_t)(set->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        SetSlot* slot = &set->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_set_remove(probes);
            return;
        }
        if (slot->hash == hash && set_slot_value_equals_string_chars(slot, value->chars, value->length)) {
            value_free(&slot->value);
            obj_set_backshift_remove_at(set, idx);
            hash_probe_record_set_remove(probes);
            obj_set_maybe_compact_after_remove(set);
            return;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

bool obj_set_has(ObjSet* set, Value value) {
    if (!set || !set->slots) return false;

    if (value_get_type(&value) == VAL_STRING && value_get_string_obj(&value)) {
        return obj_set_has_string(set, value_get_string_obj(&value));
    }
    if (value_get_type(&value) != VAL_INT) return false;

    uint32_t hash = value_hash(&value) | 2U;
    uint32_t mask = (uint32_t)(set->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        SetSlot* slot = &set->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_set_lookup(probes);
            return false;
        }
        if (slot->hash == hash && value_equals_key_fast(&slot->value, &value)) {
            hash_probe_record_set_lookup(probes);
            return true;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

bool obj_set_has_string(ObjSet* set, ObjString* value) {
    if (!set || !set->slots || !value || !value->chars) return false;

    uint32_t hash = map_string_hash_obj(value) | 2U;
    uint32_t mask = (uint32_t)(set->capacity - 1);
    uint32_t idx = hash & mask;
    uint32_t probes = 1;
    while (true) {
        SetSlot* slot = &set->slots[idx];
        if (slot->hash == 0) {
            hash_probe_record_set_lookup(probes);
            return false;
        }
        if (slot->hash == hash && set_slot_value_equals_string_chars(slot, value->chars, value->length)) {
            hash_probe_record_set_lookup(probes);
            return true;
        }
        idx = (idx + 1) & mask;
        if (probes < UINT32_MAX) probes++;
    }
}

int obj_set_count(ObjSet* set) {
    if (!set) return 0;
    return set->count;
}

void obj_set_to_array(ObjSet* set, ObjArray* result) {
    if (!set || !result) return;
    for (int i = 0; i < set->capacity; i++) {
        SetSlot* slot = &set->slots[i];
        if (slot->hash < 2) continue;
        Value out = slot->value;
        value_retain(&out);
        obj_array_push(result, out);
    }
}

// Socket operations
ObjSocket* obj_socket_create(VM* vm, int socket_fd, bool limit_tracked) {
    ObjSocket* sock = (ObjSocket*)safe_malloc(sizeof(ObjSocket));
    sock->socket_fd = socket_fd;
    sock->ref_count = 1;
    sock->is_connected = (socket_fd >= 0);
    sock->owner_vm = vm;
    sock->limit_tracked = limit_tracked;
    sock->transport_ctx = NULL;
    sock->transport_send = NULL;
    sock->transport_recv = NULL;
    sock->transport_close = NULL;
    return sock;
}

void obj_socket_retain(ObjSocket* sock) {
    if (sock) sock->ref_count++;
}

void obj_socket_release(ObjSocket* sock) {
    if (!sock) return;
    sock->ref_count--;
    if (sock->ref_count <= 0) {
        obj_socket_free(sock);
    }
}

void obj_socket_free(ObjSocket* sock) {
    if (!sock) return;
    if (sock->transport_close) {
        sock->transport_close(sock);
    }
    if (sock->socket_fd >= 0) {
        CLOSESOCKET(sock->socket_fd);
        sock->socket_fd = -1;
    }
    if (sock->limit_tracked) {
        VM* owner_vm = (VM*)sock->owner_vm;
        if (owner_vm && owner_vm->current_open_sockets > 0) {
            owner_vm->current_open_sockets--;
        }
        sock->limit_tracked = false;
    }
    sock->owner_vm = NULL;
    sock->transport_ctx = NULL;
    sock->transport_send = NULL;
    sock->transport_recv = NULL;
    sock->transport_close = NULL;
    free(sock);
}

void value_init_socket(Value* val, ObjSocket* sock) {
    value_set_type(val, VAL_SOCKET);
    value_set_socket_obj(val, sock);
}

// File operations
ObjFile* obj_file_create(VM* vm, FILE* handle, bool limit_tracked) {
    ObjFile* file = (ObjFile*)safe_malloc(sizeof(ObjFile));
    file->handle = handle;
    file->ref_count = 1;
    file->is_closed = (handle == NULL);
    file->owner_vm = vm;
    file->limit_tracked = limit_tracked;
    return file;
}

void obj_file_retain(ObjFile* file) {
    if (file) file->ref_count++;
}

void obj_file_release(ObjFile* file) {
    if (!file) return;
    file->ref_count--;
    if (file->ref_count <= 0) {
        obj_file_free(file);
    }
}

bool obj_file_close(ObjFile* file) {
    if (!file) return false;
    if (file->is_closed) return true;
    if (file->handle) {
        fclose(file->handle);
        file->handle = NULL;
    }
    file->is_closed = true;
    if (file->limit_tracked) {
        VM* owner_vm = (VM*)file->owner_vm;
        if (owner_vm && owner_vm->current_open_files > 0) {
            owner_vm->current_open_files--;
        }
        file->limit_tracked = false;
    }
    return true;
}

void obj_file_free(ObjFile* file) {
    if (!file) return;
    obj_file_close(file);
    file->owner_vm = NULL;
    free(file);
}

void value_init_file(Value* val, ObjFile* file) {
    value_set_type(val, VAL_FILE);
    value_set_file_obj(val, file);
}

void vm_init(VM* vm) {
    vm->stack.values = NULL;
    vm->stack.count = 0;
    vm->stack.capacity = 0;
    constant_pool_init(&vm->stack.constants);

    vm->frames = NULL;
    vm->frame_count = 0;
    vm->frame_capacity = 0;

    vm->globals = NULL;
    vm->global_count = 0;
    vm->global_capacity = 0;
    vm->global_names = NULL;
    hash_table_init(&vm->globals_hash);
    vm->globals_hash_entries = NULL;
    vm->extension_registry = NULL;
    vm->interface_dispatch_entries = NULL;
    vm->interface_dispatch_count = 0;
    vm->interface_dispatch_capacity = 0;
    vm->interface_dispatch_slot_cache = NULL;

    hash_table_init(&vm->string_pool);

    vm->exception_handlers = NULL;
    vm->exception_handler_count = 0;
    vm->exception_handler_capacity = 0;
    value_set_type(&vm->exception_value, VAL_NIL);
    vm->in_exception = false;

    vm->return_value = NULL;
    vm->error_occurred = false;
    vm->error_message = NULL;
    vm->config = vm_default_config();
    vm->sandbox_root = NULL;
    vm->file_io_enabled = true;
    vm->network_enabled = true;
    vm->process_enabled = true;
    vm->sqlite_enabled = true;
    vm->threading_enabled = true;
    vm->instruction_count = 0;
    vm->current_call_depth = 0;
    vm->current_open_files = 0;
    vm->current_open_sockets = 0;
    vm->ready_tasks_head = NULL;
    vm->ready_tasks_tail = NULL;
    vm->future_waiters = NULL;
    vm->timer_waiters = NULL;
    vm->poll_waiters = NULL;
    vm->posted_event_queue = vm_posted_event_queue_create(vm);
    vm->posted_event_auto_drain = true;
    vm->rng_seeded = false;
    vm->rng_counter = 0;
    jit_vm_init(vm);
    vm->profile_opcodes = false;
    memset(vm->opcode_counts, 0, sizeof(vm->opcode_counts));
    vm->debug_breakpoints = NULL;
    vm->debug_breakpoint_count = 0;
    vm->debug_breakpoint_capacity = 0;
    vm->debug_exec_mode = VM_DEBUG_EXEC_NONE;
    vm->debug_step_source_file = NULL;
    vm->debug_step_line = 0;
    vm->debug_step_depth = 0;
    vm->debug_skip_source_file = NULL;
    vm->debug_skip_line = 0;
    vm->debug_skip_depth = 0;
    vm->debug_stop.kind = VM_DEBUG_STOP_NONE;
    vm->debug_stop.source_file = NULL;
    vm->debug_stop.function_name = NULL;
    vm->debug_stop.line = 0;
    vm->debug_stop.call_depth = 0;
    vm->debug_stop.ip = 0;
    vm->debug_stop_on_entry_pending = false;
    vm->debug_pause_requested = 0;
    vm->debug_break_on_runtime_error = false;
    vm->output_callback = NULL;
    vm->output_callback_user_data = NULL;

    cycle_gc_init(&vm->cycle_gc);

    // Initialize Winsock on Windows
    #ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    #endif
}

void vm_free(VM* vm) {
    // Cleanup Winsock on Windows
    #ifdef _WIN32
    WSACleanup();
    #endif

    if (vm->stack.values) {
        for (int i = 0; i < vm->stack.count; i++) {
            value_free(&vm->stack.values[i]);
        }
        free(vm->stack.values);
    }
    constant_pool_free(&vm->stack.constants);

    if (vm->frames) free(vm->frames);
    if (vm->debug_breakpoints) free(vm->debug_breakpoints);
    vm->debug_breakpoints = NULL;
    vm->debug_breakpoint_count = 0;
    vm->debug_breakpoint_capacity = 0;
    vm->debug_exec_mode = VM_DEBUG_EXEC_NONE;
    vm->debug_step_source_file = NULL;
    vm->debug_step_line = 0;
    vm->debug_step_depth = 0;
    vm->debug_skip_source_file = NULL;
    vm->debug_skip_line = 0;
    vm->debug_skip_depth = 0;
    vm->debug_stop.kind = VM_DEBUG_STOP_NONE;
    vm->debug_stop.source_file = NULL;
    vm->debug_stop.function_name = NULL;
    vm->debug_stop.line = 0;
    vm->debug_stop.call_depth = 0;
    vm->debug_stop.ip = 0;
    vm->debug_stop_on_entry_pending = false;
    vm->debug_pause_requested = 0;
    vm->debug_break_on_runtime_error = false;
    vm->output_callback = NULL;
    vm->output_callback_user_data = NULL;
    jit_vm_free(vm);

    if (vm->posted_event_queue) {
        vm_posted_event_queue_close(vm->posted_event_queue);
        vm_posted_event_queue_release(vm->posted_event_queue);
        vm->posted_event_queue = NULL;
    }
    while (vm->ready_tasks_head) {
        AsyncTask* next = vm->ready_tasks_head->next;
        async_task_free(vm->ready_tasks_head);
        vm->ready_tasks_head = next;
    }
    vm->ready_tasks_tail = NULL;

    while (vm->future_waiters) {
        FutureWaitEntry* next_entry = vm->future_waiters->next;
        AsyncTask* task = vm->future_waiters->tasks_head;
        while (task) {
            AsyncTask* next_task = task->next;
            async_task_free(task);
            task = next_task;
        }
        if (vm->future_waiters->future) {
            obj_record_release(vm->future_waiters->future);
        }
        free(vm->future_waiters);
        vm->future_waiters = next_entry;
    }

    while (vm->timer_waiters) {
        TimerWaitEntry* next_entry = vm->timer_waiters->next;
        if (vm->timer_waiters->future) {
            obj_record_release(vm->timer_waiters->future);
        }
        free(vm->timer_waiters);
        vm->timer_waiters = next_entry;
    }

    while (vm->poll_waiters) {
        VmPollWaitEntry* next_entry = vm->poll_waiters->next;
        if (vm->poll_waiters->free_fn) {
            vm->poll_waiters->free_fn(vm->poll_waiters->payload);
        }
        free(vm->poll_waiters);
        vm->poll_waiters = next_entry;
    }

    if (vm->globals) {
        for (int i = 0; i < vm->global_count; i++) {
            if (vm->global_names[i]) free(vm->global_names[i]);
        }
        free(vm->globals);
    }
    if (vm->global_names) free(vm->global_names);
    if (vm->globals_hash_entries) free(vm->globals_hash_entries);
    interface_dispatch_entries_free(vm->interface_dispatch_entries, vm->interface_dispatch_count);
    vm->interface_dispatch_entries = NULL;
    vm->interface_dispatch_count = 0;
    vm->interface_dispatch_capacity = 0;
    if (vm->interface_dispatch_slot_cache) {
        free(vm->interface_dispatch_slot_cache);
        vm->interface_dispatch_slot_cache = NULL;
    }

    hash_table_free(&vm->globals_hash);

    // Reclaim cyclic garbage that RC can't collect.
    vm_gc_collect(vm);

    for (int i = 0; i < vm->string_pool.capacity; i++) {
        HashEntry* entry = vm->string_pool.entries[i];
        if (!entry) continue;
        if (value_get_type(&entry->value) == VAL_STRING) {
            obj_string_release(value_get_string_obj(&entry->value));
        }
        if (entry->key) free(entry->key);
        free(entry);
    }
    free(vm->string_pool.entries);
    vm->string_pool.entries = NULL;
    vm->string_pool.capacity = 0;
    vm->string_pool.count = 0;

    if (vm->exception_handlers) {
        free(vm->exception_handlers);
    }

    if (vm->error_message) free(vm->error_message);
    if (vm->sandbox_root) free(vm->sandbox_root);
    if (vm->return_value) {
        value_free(vm->return_value);
        free(vm->return_value);
    }
    hash_probe_dump_once();
}

void vm_set_output_callback(VM* vm, VmOutputCallback callback, void* user_data) {
    if (!vm) return;
    vm->output_callback = callback;
    vm->output_callback_user_data = user_data;
}

static void vm_stack_reserve(VM* vm, int additional);

static void push(VM* vm, Value val) {
    if (vm->stack.count >= vm->config.max_stack_size) {
        vm_runtime_error(vm, "Stack overflow");
        return;
    }
    vm_stack_reserve(vm, 1);
    if (vm->error_occurred) return;
    vm->stack.values[vm->stack.count++] = val;
}

static void push_nil_count(VM* vm, int count) {
    if (!vm || count <= 0) return;

    int base = vm->stack.count;
    int new_count = base + count;
    vm_stack_reserve(vm, count);
    if (vm->error_occurred) return;

    Value nil;
    value_init_nil(&nil);
    for (int i = 0; i < count; i++) {
        vm->stack.values[base + i] = nil;
    }
    vm->stack.count = new_count;
}

static Value pop(VM* vm) {
    if (vm->stack.count <= 0) {
        vm_runtime_error(vm, "Stack underflow");
        Value nil;
        value_init_nil(&nil);
        return nil;
    }
    return vm->stack.values[--vm->stack.count];
}

static Value peek(VM* vm, int offset) {
    int idx = vm->stack.count - 1 - offset;
    if (idx < 0 || idx >= vm->stack.count) {
        vm_runtime_error(vm, "Peek out of bounds");
        Value nil;
        value_init_nil(&nil);
        return nil;
    }
    return vm->stack.values[idx];
}

static bool value_type_is_unmanaged(ValueType type) {
    switch (type) {
        case VAL_INT:
        case VAL_BOOL:
        case VAL_DOUBLE:
        case VAL_NIL:
            return true;
        default:
            return false;
    }
}

static void stack_pop_to(VM* vm, int target_count) {
    if (!vm) return;
    if (target_count < 0) target_count = 0;

    while (vm->stack.count > target_count) {
        Value* slot = &vm->stack.values[vm->stack.count - 1];
        if (!value_type_is_unmanaged(value_get_type(slot))) {
            value_free(slot);
        }
        vm->stack.count--;
    }
}

static void callframe_free_defers(CallFrame* frame) {
    if (!frame || !frame->defers) return;

    for (int i = 0; i < frame->defer_count; i++) {
        DeferredCall* dc = &frame->defers[i];
        value_free(&dc->callee);
        for (int j = 0; j < dc->arg_count; j++) {
            value_free(&dc->args[j]);
        }
        if (dc->args) free(dc->args);
        dc->args = NULL;
        dc->arg_count = 0;
    }

    free(frame->defers);
    frame->defers = NULL;
    frame->defer_count = 0;
    frame->defer_capacity = 0;
}

static void vm_stack_reserve(VM* vm, int additional) {
    if (!vm || additional <= 0) return;
    if (vm->stack.count + additional > vm->config.max_stack_size) {
        vm_runtime_error(vm, "Stack overflow");
        return;
    }

    int needed = vm->stack.count + additional;
    if (needed <= vm->stack.capacity) return;

    int new_capacity = vm->stack.capacity > 0 ? vm->stack.capacity : 16;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    Value* new_values = (Value*)safe_realloc(vm->stack.values, new_capacity * sizeof(Value));
    vm->stack.values = new_values;
    vm->stack.capacity = new_capacity;
}

static void vm_frame_reserve(VM* vm, int additional) {
    if (!vm || additional <= 0) return;

    int needed = vm->frame_count + additional;
    if (needed <= vm->frame_capacity) return;

    int new_capacity = vm->frame_capacity > 0 ? vm->frame_capacity : 16;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    CallFrame* new_frames = (CallFrame*)safe_realloc(vm->frames, (size_t)new_capacity * sizeof(CallFrame));
    vm->frames = new_frames;
    vm->frame_capacity = new_capacity;
}

static void call_frame_defer_reserve(CallFrame* frame, int additional) {
    if (!frame || additional <= 0) return;

    int needed = frame->defer_count + additional;
    if (needed <= frame->defer_capacity) return;

    int new_capacity = frame->defer_capacity > 0 ? frame->defer_capacity : 4;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    DeferredCall* new_defers = (DeferredCall*)safe_realloc(frame->defers, (size_t)new_capacity * sizeof(DeferredCall));
    frame->defers = new_defers;
    frame->defer_capacity = new_capacity;
}

static void vm_exception_handler_reserve(VM* vm, int additional) {
    if (!vm || additional <= 0) return;

    int needed = vm->exception_handler_count + additional;
    if (needed <= vm->exception_handler_capacity) return;

    int new_capacity = vm->exception_handler_capacity > 0 ? vm->exception_handler_capacity : 8;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    ExceptionHandler* new_handlers = (ExceptionHandler*)safe_realloc(vm->exception_handlers,
                                                                     (size_t)new_capacity * sizeof(ExceptionHandler));
    vm->exception_handlers = new_handlers;
    vm->exception_handler_capacity = new_capacity;
}

static AsyncTask* async_task_create(ObjFunction* function, int stack_count) {
    AsyncTask* task = (AsyncTask*)safe_malloc(sizeof(AsyncTask));
    task->function = function;
    if (task->function) obj_function_retain(task->function);
    task->ip = 0;
    task->stack_count = stack_count;
    task->slots_count = function ? function->local_count : 0;
    task->stack_values = NULL;
    if (stack_count > 0) {
        task->stack_values = (Value*)safe_malloc((size_t)stack_count * sizeof(Value));
        for (int i = 0; i < stack_count; i++) {
            value_init_nil(&task->stack_values[i]);
        }
    }
    task->defers = NULL;
    task->defer_count = 0;
    task->defer_capacity = 0;
    task->async_result_future = NULL;
    task->has_resume_value = false;
    value_init_nil(&task->resume_value);
    task->has_resume_panic = false;
    task->resume_panic_message = NULL;
    task->panic_unwinding = false;
    task->panic_message = NULL;
    task->entry_profiled = false;
    task->next = NULL;
    return task;
}

static void async_task_free(AsyncTask* task) {
    if (!task) return;
    if (task->stack_values) {
        for (int i = 0; i < task->stack_count; i++) {
            value_free(&task->stack_values[i]);
        }
        free(task->stack_values);
    }
    if (task->defers) {
        CallFrame frame;
        memset(&frame, 0, sizeof(frame));
        frame.defers = task->defers;
        frame.defer_count = task->defer_count;
        frame.defer_capacity = task->defer_capacity;
        callframe_free_defers(&frame);
    }
    if (task->has_resume_value) {
        value_free(&task->resume_value);
    }
    if (task->resume_panic_message) {
        free(task->resume_panic_message);
    }
    if (task->panic_message) {
        free(task->panic_message);
    }
    if (task->async_result_future) {
        obj_record_release(task->async_result_future);
    }
    if (task->function) {
        obj_function_release(task->function);
    }
    free(task);
}

static void vm_enqueue_ready_task(VM* vm, AsyncTask* task) {
    if (!vm || !task) return;
    task->next = NULL;
    if (vm->ready_tasks_tail) {
        vm->ready_tasks_tail->next = task;
    } else {
        vm->ready_tasks_head = task;
    }
    vm->ready_tasks_tail = task;
}

static AsyncTask* vm_dequeue_ready_task(VM* vm) {
    if (!vm || !vm->ready_tasks_head) return NULL;
    AsyncTask* task = vm->ready_tasks_head;
    vm->ready_tasks_head = task->next;
    if (!vm->ready_tasks_head) {
        vm->ready_tasks_tail = NULL;
    }
    task->next = NULL;
    return task;
}

typedef int (*VmJitCompiledEntryFn)(VM* vm,
                                    ObjFunction* function,
                                    int args_base,
                                    int arg_count,
                                    CallFrame* io_frame,
                                    Chunk** io_chunk,
                                    uint8_t** io_code);

static int vm_enter_sync_function_from_call(VM* vm,
                                            ObjFunction* called_func,
                                            int args_base,
                                            int arg_count,
                                            CallFrame* io_frame,
                                            Chunk** io_chunk,
                                            uint8_t** io_code) {
    if (!vm || !called_func || !io_frame || !io_chunk || !io_code) return -1;

    if (called_func->param_count != arg_count) {
        vm_runtime_error(vm, "Wrong number of arguments");
        return -1;
    }
    if (vm->frame_count + 1 > vm->config.max_call_depth) {
        vm_runtime_error(vm, "Stack overflow: maximum call depth exceeded");
        return -1;
    }
    if (called_func->local_count < arg_count) {
        vm_runtime_error(vm, "Function local slots less than arguments");
        return -1;
    }
    if (called_func->is_async) {
        vm_runtime_error(vm, "Compiled entry dispatch does not support async functions");
        return -1;
    }

    if (vm->frame_count > 0) {
        vm->frames[vm->frame_count - 1] = *io_frame;
    }
    push_nil_count(vm, called_func->local_count - arg_count);

    CallFrame new_frame;
    new_frame.function = called_func;
    new_frame.ip = 0;
    new_frame.slots_start = args_base;
    new_frame.slots_count = called_func->local_count;
    new_frame.defers = NULL;
    new_frame.defer_count = 0;
    new_frame.defer_capacity = 0;
    new_frame.is_async_root = false;
    new_frame.async_result_future = NULL;
    new_frame.panic_unwinding = false;
    new_frame.panic_message = NULL;
    if (!vm_bind_captures_to_frame(vm, &new_frame, called_func)) {
        return -1;
    }

    vm_frame_reserve(vm, 1);
    vm->frames[vm->frame_count++] = new_frame;
    vm->current_call_depth = vm->frame_count;

    *io_frame = new_frame;
    *io_chunk = &new_frame.function->chunk;
    *io_code = (*io_chunk)->code;
    return 0;
}

static int vm_try_call_compiled_entry(VM* vm,
                                      ObjFunction* called_func,
                                      int args_base,
                                      int arg_count,
                                      CallFrame* io_frame,
                                      Chunk** io_chunk,
                                      uint8_t** io_code,
                                      bool* out_handled) {
    if (out_handled) *out_handled = false;
    if (!vm || !called_func || !jit_function_has_compiled_entry(called_func)) {
        return 0;
    }

    VmJitCompiledEntryFn entry = (VmJitCompiledEntryFn)called_func->jit_compiled_entry;
    if (!entry) return 0;

    int rc = entry(vm, called_func, args_base, arg_count, io_frame, io_chunk, io_code);
    if (rc == 0 && out_handled) {
        *out_handled = true;
    }
    return rc;
}

static int vm_finish_native_compiled_call_int(VM* vm, int args_base, int64_t result) {
    Value out;
    value_init_int(&out, result);
    stack_pop_to(vm, args_base);
    push(vm, out);
    return vm->error_occurred ? -1 : 0;
}

static int vm_finish_native_compiled_call_bool(VM* vm, int args_base, bool result) {
    Value out;
    value_init_bool(&out, result);
    stack_pop_to(vm, args_base);
    push(vm, out);
    return vm->error_occurred ? -1 : 0;
}

static int vm_finish_native_int_const_op(VM* vm,
                                         int args_base,
                                         JitCompiledKind kind,
                                         int64_t input,
                                         int64_t constant_value) {
    int64_t result = 0;
    switch (kind) {
        case JIT_COMPILED_KIND_INT_ADD_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_ADD_CONST:
            result = input + constant_value;
            break;
        case JIT_COMPILED_KIND_INT_MUL_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MUL_CONST:
            result = input * constant_value;
            break;
        case JIT_COMPILED_KIND_INT_SUB_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SUB_CONST:
            result = input - constant_value;
            break;
        case JIT_COMPILED_KIND_INT_DIV_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_DIV_CONST:
            if (constant_value == 0) {
                vm_runtime_error(vm, "Division by zero");
                return -1;
            }
            result = input / constant_value;
            break;
        case JIT_COMPILED_KIND_INT_MOD_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MOD_CONST:
            if (constant_value == 0) {
                vm_runtime_error(vm, "Modulo by zero");
                return -1;
            }
            result = input % constant_value;
            break;
        case JIT_COMPILED_KIND_INT_BIT_AND_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_AND_CONST:
            result = input & constant_value;
            break;
        case JIT_COMPILED_KIND_INT_BIT_OR_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_OR_CONST:
            result = input | constant_value;
            break;
        case JIT_COMPILED_KIND_INT_BIT_XOR_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_XOR_CONST:
            result = input ^ constant_value;
            break;
        default:
            vm_runtime_error(vm, "Invalid compiled entry");
            return -1;
    }

    return vm_finish_native_compiled_call_int(vm, args_base, result);
}

static int vm_finish_native_int_binary_op(VM* vm,
                                          int args_base,
                                          JitCompiledKind kind,
                                          int64_t left,
                                          int64_t right) {
    int64_t result = 0;
    switch (kind) {
        case JIT_COMPILED_KIND_INT_ADD_LOCALS:
            result = left + right;
            break;
        case JIT_COMPILED_KIND_INT_SUB_LOCALS:
            result = left - right;
            break;
        case JIT_COMPILED_KIND_INT_MUL_LOCALS:
            result = left * right;
            break;
        case JIT_COMPILED_KIND_INT_BIT_AND_LOCALS:
            result = left & right;
            break;
        case JIT_COMPILED_KIND_INT_BIT_OR_LOCALS:
            result = left | right;
            break;
        case JIT_COMPILED_KIND_INT_BIT_XOR_LOCALS:
            result = left ^ right;
            break;
        default:
            vm_runtime_error(vm, "Invalid compiled entry");
            return -1;
    }

    return vm_finish_native_compiled_call_int(vm, args_base, result);
}

static bool vm_eval_native_int_binary_summary_op(VM* vm,
                                                 JitSummaryOp op,
                                                 int64_t left,
                                                 int64_t right,
                                                 int64_t* out_result) {
    if (!out_result) return false;
    switch (op) {
        case JIT_SUMMARY_OP_ADD:
            *out_result = left + right;
            return true;
        case JIT_SUMMARY_OP_SUB:
            *out_result = left - right;
            return true;
        case JIT_SUMMARY_OP_MUL:
            *out_result = left * right;
            return true;
        case JIT_SUMMARY_OP_DIV:
            if (right == 0) {
                vm_runtime_error(vm, "Division by zero");
                return false;
            }
            *out_result = left / right;
            return true;
        case JIT_SUMMARY_OP_MOD:
            if (right == 0) {
                vm_runtime_error(vm, "Modulo by zero");
                return false;
            }
            *out_result = left % right;
            return true;
        case JIT_SUMMARY_OP_BIT_AND:
            *out_result = left & right;
            return true;
        case JIT_SUMMARY_OP_BIT_OR:
            *out_result = left | right;
            return true;
        case JIT_SUMMARY_OP_BIT_XOR:
            *out_result = left ^ right;
            return true;
        default:
            vm_runtime_error(vm, "Invalid compiled int op");
            return false;
    }
}

static int vm_finish_native_int_binary_summary_op(VM* vm,
                                                  int args_base,
                                                  JitSummaryOp op,
                                                  int64_t left,
                                                  int64_t right) {
    int64_t result = 0;
    if (!vm_eval_native_int_binary_summary_op(vm, op, left, right, &result)) return -1;
    return vm_finish_native_compiled_call_int(vm, args_base, result);
}

static int vm_finish_native_guarded_int_binary_summary_op(VM* vm,
                                                          int args_base,
                                                          JitSummaryOp op,
                                                          int64_t guarded_value,
                                                          int64_t right,
                                                          int64_t guard_const) {
    if (guarded_value < guard_const) {
        return vm_finish_native_compiled_call_int(vm, args_base, guarded_value);
    }
    return vm_finish_native_int_binary_summary_op(vm, args_base, op, guarded_value, right);
}

static bool vm_eval_native_int_compare_summary_op(VM* vm,
                                                  JitSummaryOp op,
                                                  int64_t left,
                                                  int64_t right,
                                                  bool* out_result) {
    if (!out_result) return false;
    switch (op) {
        case JIT_SUMMARY_OP_LT:
            *out_result = left < right;
            return true;
        case JIT_SUMMARY_OP_LE:
            *out_result = left <= right;
            return true;
        case JIT_SUMMARY_OP_EQ:
            *out_result = left == right;
            return true;
        case JIT_SUMMARY_OP_NE:
            *out_result = left != right;
            return true;
        case JIT_SUMMARY_OP_GT:
            *out_result = left > right;
            return true;
        case JIT_SUMMARY_OP_GE:
            *out_result = left >= right;
            return true;
        default:
            vm_runtime_error(vm, "Invalid compiled compare op");
            return false;
    }
}

static bool vm_eval_native_int_selector_summary_op(VM* vm,
                                                   JitSummaryOp op,
                                                   int64_t left,
                                                   int64_t right,
                                                   bool* out_condition) {
    if (!out_condition) return false;
    switch (op) {
        case JIT_SUMMARY_OP_LT:
            *out_condition = left < right;
            return true;
        case JIT_SUMMARY_OP_LE:
            *out_condition = left <= right;
            return true;
        case JIT_SUMMARY_OP_GT:
            *out_condition = left > right;
            return true;
        case JIT_SUMMARY_OP_GE:
            *out_condition = left >= right;
            return true;
        default:
            vm_runtime_error(vm, "Invalid compiled selector op");
            return false;
    }
}

static int vm_finish_native_int_compare_summary_op(VM* vm,
                                                   int args_base,
                                                   JitSummaryOp op,
                                                   int64_t left,
                                                   int64_t right) {
    bool result = false;
    if (!vm_eval_native_int_compare_summary_op(vm, op, left, right, &result)) return -1;
    return vm_finish_native_compiled_call_bool(vm, args_base, result);
}

static int vm_finish_native_guarded_int_compare_summary_op(VM* vm,
                                                           int args_base,
                                                           JitSummaryOp op,
                                                           int64_t left,
                                                           int64_t right,
                                                           int64_t guard_const,
                                                           bool guard_result) {
    if (left < guard_const) {
        return vm_finish_native_compiled_call_bool(vm, args_base, guard_result);
    }
    return vm_finish_native_int_compare_summary_op(vm, args_base, op, left, right);
}

static int vm_finish_native_int_compare_op(VM* vm,
                                           int args_base,
                                           JitCompiledKind kind,
                                           int64_t left,
                                           int64_t right) {
    JitSummaryOp op = JIT_SUMMARY_OP_NONE;
    switch (kind) {
        case JIT_COMPILED_KIND_BOOL_LT_LOCALS:
            op = JIT_SUMMARY_OP_LT;
            break;
        case JIT_COMPILED_KIND_BOOL_LE_LOCALS:
            op = JIT_SUMMARY_OP_LE;
            break;
        case JIT_COMPILED_KIND_BOOL_EQ_LOCALS:
            op = JIT_SUMMARY_OP_EQ;
            break;
        case JIT_COMPILED_KIND_BOOL_NE_LOCALS:
            op = JIT_SUMMARY_OP_NE;
            break;
        case JIT_COMPILED_KIND_BOOL_GT_LOCALS:
            op = JIT_SUMMARY_OP_GT;
            break;
        case JIT_COMPILED_KIND_BOOL_GE_LOCALS:
            op = JIT_SUMMARY_OP_GE;
            break;
        default:
            vm_runtime_error(vm, "Invalid compiled entry");
            return -1;
    }

    return vm_finish_native_int_compare_summary_op(vm, args_base, op, left, right);
}

static int vm_finish_native_int_compare_const_op(VM* vm,
                                                 int args_base,
                                                 JitCompiledKind kind,
                                                 int64_t left,
                                                 int64_t right_const) {
    JitSummaryOp op = JIT_SUMMARY_OP_NONE;
    switch (kind) {
        case JIT_COMPILED_KIND_BOOL_LT_LOCAL_CONST:
            op = JIT_SUMMARY_OP_LT;
            break;
        case JIT_COMPILED_KIND_BOOL_LE_LOCAL_CONST:
            op = JIT_SUMMARY_OP_LE;
            break;
        case JIT_COMPILED_KIND_BOOL_EQ_LOCAL_CONST:
            op = JIT_SUMMARY_OP_EQ;
            break;
        case JIT_COMPILED_KIND_BOOL_NE_LOCAL_CONST:
            op = JIT_SUMMARY_OP_NE;
            break;
        case JIT_COMPILED_KIND_BOOL_GT_LOCAL_CONST:
            op = JIT_SUMMARY_OP_GT;
            break;
        case JIT_COMPILED_KIND_BOOL_GE_LOCAL_CONST:
            op = JIT_SUMMARY_OP_GE;
            break;
        default:
            vm_runtime_error(vm, "Invalid compiled entry");
            return -1;
    }

    return vm_finish_native_int_compare_summary_op(vm, args_base, op, left, right_const);
}

static int vm_finish_native_guarded_int_compare_op(VM* vm,
                                                   int args_base,
                                                   JitCompiledKind kind,
                                                   int64_t left,
                                                   int64_t right,
                                                   int64_t guard_const,
                                                   bool guard_result) {
    JitSummaryOp op = JIT_SUMMARY_OP_NONE;
    if (left < guard_const) {
        return vm_finish_native_compiled_call_bool(vm, args_base, guard_result);
    }

    switch (kind) {
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_LT_LOCALS:
            op = JIT_SUMMARY_OP_LT;
            break;
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_LE_LOCALS:
            op = JIT_SUMMARY_OP_LE;
            break;
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_EQ_LOCALS:
            op = JIT_SUMMARY_OP_EQ;
            break;
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_NE_LOCALS:
            op = JIT_SUMMARY_OP_NE;
            break;
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_GT_LOCALS:
            op = JIT_SUMMARY_OP_GT;
            break;
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_GE_LOCALS:
            op = JIT_SUMMARY_OP_GE;
            break;
        default:
            vm_runtime_error(vm, "Invalid compiled entry");
            return -1;
    }

    return vm_finish_native_int_compare_summary_op(vm, args_base, op, left, right);
}

static int vm_finish_native_int_selector_summary_op(VM* vm,
                                                    int args_base,
                                                    JitSummaryOp op,
                                                    int64_t left,
                                                    int64_t right,
                                                    uint8_t true_slot,
                                                    uint8_t false_slot) {
    bool condition = false;
    uint8_t selected_slot = false_slot;
    Value* selected = NULL;

    if (!vm_eval_native_int_selector_summary_op(vm, op, left, right, &condition)) return -1;

    selected_slot = condition ? true_slot : false_slot;
    selected = &vm->stack.values[args_base + selected_slot];
    if (value_get_type(selected) != VAL_INT) {
        vm_runtime_error(vm, "Operands must be integer");
        return -1;
    }
    return vm_finish_native_compiled_call_int(vm, args_base, value_get_int(selected));
}

static int vm_finish_native_guarded_int_selector_summary_op(VM* vm,
                                                            int args_base,
                                                            JitSummaryOp op,
                                                            int64_t left,
                                                            int64_t right,
                                                            int64_t guard_const,
                                                            uint8_t true_slot,
                                                            uint8_t false_slot) {
    if (left < guard_const) {
        return vm_finish_native_compiled_call_int(vm, args_base, left);
    }
    return vm_finish_native_int_selector_summary_op(vm,
                                                    args_base,
                                                    op,
                                                    left,
                                                    right,
                                                    true_slot,
                                                    false_slot);
}

static int vm_finish_native_int_selector_op(VM* vm,
                                            int args_base,
                                            JitCompiledKind kind,
                                            int64_t left,
                                            int64_t right,
                                            uint8_t true_slot,
                                            uint8_t false_slot) {
    JitSummaryOp op = JIT_SUMMARY_OP_NONE;

    switch (kind) {
        case JIT_COMPILED_KIND_INT_SELECT_LT_LOCALS:
            op = JIT_SUMMARY_OP_LT;
            break;
        case JIT_COMPILED_KIND_INT_SELECT_LE_LOCALS:
            op = JIT_SUMMARY_OP_LE;
            break;
        case JIT_COMPILED_KIND_INT_SELECT_GT_LOCALS:
            op = JIT_SUMMARY_OP_GT;
            break;
        case JIT_COMPILED_KIND_INT_SELECT_GE_LOCALS:
            op = JIT_SUMMARY_OP_GE;
            break;
        default:
            vm_runtime_error(vm, "Invalid compiled entry");
            return -1;
    }
    return vm_finish_native_int_selector_summary_op(vm,
                                                    args_base,
                                                    op,
                                                    left,
                                                    right,
                                                    true_slot,
                                                    false_slot);
}

static int vm_finish_native_guarded_int_selector_op(VM* vm,
                                                    int args_base,
                                                    JitCompiledKind kind,
                                                    int64_t left,
                                                    int64_t right,
                                                    int64_t guard_const,
                                                    uint8_t true_slot,
                                                    uint8_t false_slot) {
    JitSummaryOp op = JIT_SUMMARY_OP_NONE;
    if (left < guard_const) {
        return vm_finish_native_compiled_call_int(vm, args_base, left);
    }

    switch (kind) {
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LT_LOCALS:
            op = JIT_SUMMARY_OP_LT;
            break;
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LE_LOCALS:
            op = JIT_SUMMARY_OP_LE;
            break;
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GT_LOCALS:
            op = JIT_SUMMARY_OP_GT;
            break;
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GE_LOCALS:
            op = JIT_SUMMARY_OP_GE;
            break;
        default:
            vm_runtime_error(vm, "Invalid compiled entry");
            return -1;
    }

    return vm_finish_native_int_selector_summary_op(vm,
                                                    args_base,
                                                    op,
                                                    left,
                                                    right,
                                                    true_slot,
                                                    false_slot);
}

static int vm_finish_native_int_selector_const_summary_op(VM* vm,
                                                          int args_base,
                                                          JitSummaryOp op,
                                                          int64_t value,
                                                          int64_t selector_const,
                                                          bool return_local_when_true) {
    bool condition = false;
    if (!vm_eval_native_int_selector_summary_op(vm, op, value, selector_const, &condition)) return -1;
    return vm_finish_native_compiled_call_int(vm,
                                              args_base,
                                              condition == return_local_when_true ? value : selector_const);
}

static int vm_finish_native_int_selector_const_op(VM* vm,
                                                  int args_base,
                                                  JitCompiledKind kind,
                                                  int64_t value,
                                                  int64_t selector_const) {
    JitSummaryOp op = JIT_SUMMARY_OP_NONE;
    bool return_local_when_true = false;

    switch (kind) {
        case JIT_COMPILED_KIND_INT_SELECT_LT_LOCAL_CONST_RET_LOCAL:
            op = JIT_SUMMARY_OP_LT;
            return_local_when_true = true;
            break;
        case JIT_COMPILED_KIND_INT_SELECT_LT_LOCAL_CONST_RET_CONST:
            op = JIT_SUMMARY_OP_LT;
            return_local_when_true = false;
            break;
        case JIT_COMPILED_KIND_INT_SELECT_LE_LOCAL_CONST_RET_LOCAL:
            op = JIT_SUMMARY_OP_LE;
            return_local_when_true = true;
            break;
        case JIT_COMPILED_KIND_INT_SELECT_LE_LOCAL_CONST_RET_CONST:
            op = JIT_SUMMARY_OP_LE;
            return_local_when_true = false;
            break;
        case JIT_COMPILED_KIND_INT_SELECT_GT_LOCAL_CONST_RET_LOCAL:
            op = JIT_SUMMARY_OP_GT;
            return_local_when_true = true;
            break;
        case JIT_COMPILED_KIND_INT_SELECT_GT_LOCAL_CONST_RET_CONST:
            op = JIT_SUMMARY_OP_GT;
            return_local_when_true = false;
            break;
        case JIT_COMPILED_KIND_INT_SELECT_GE_LOCAL_CONST_RET_LOCAL:
            op = JIT_SUMMARY_OP_GE;
            return_local_when_true = true;
            break;
        case JIT_COMPILED_KIND_INT_SELECT_GE_LOCAL_CONST_RET_CONST:
            op = JIT_SUMMARY_OP_GE;
            return_local_when_true = false;
            break;
        default:
            vm_runtime_error(vm, "Invalid compiled entry");
            return -1;
    }
    return vm_finish_native_int_selector_const_summary_op(vm,
                                                          args_base,
                                                          op,
                                                          value,
                                                          selector_const,
                                                          return_local_when_true);
}

static int vm_finish_native_guarded_int_selector_const_summary_op(VM* vm,
                                                                  int args_base,
                                                                  JitSummaryOp op,
                                                                  int64_t value,
                                                                  int64_t guard_const,
                                                                  int64_t selector_const,
                                                                  bool return_local_when_true) {
    if (value < guard_const) {
        return vm_finish_native_compiled_call_int(vm, args_base, value);
    }
    return vm_finish_native_int_selector_const_summary_op(vm,
                                                          args_base,
                                                          op,
                                                          value,
                                                          selector_const,
                                                          return_local_when_true);
}

static int vm_finish_native_guarded_int_selector_const_op(VM* vm,
                                                          int args_base,
                                                          JitCompiledKind kind,
                                                          int64_t value,
                                                          int64_t guard_const,
                                                          int64_t selector_const) {
    JitSummaryOp op = JIT_SUMMARY_OP_NONE;
    bool return_local_when_true = false;
    if (value < guard_const) {
        return vm_finish_native_compiled_call_int(vm, args_base, value);
    }

    switch (kind) {
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LT_LOCAL_CONST_RET_LOCAL:
            op = JIT_SUMMARY_OP_LT;
            return_local_when_true = true;
            break;
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LT_LOCAL_CONST_RET_CONST:
            op = JIT_SUMMARY_OP_LT;
            return_local_when_true = false;
            break;
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LE_LOCAL_CONST_RET_LOCAL:
            op = JIT_SUMMARY_OP_LE;
            return_local_when_true = true;
            break;
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LE_LOCAL_CONST_RET_CONST:
            op = JIT_SUMMARY_OP_LE;
            return_local_when_true = false;
            break;
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GT_LOCAL_CONST_RET_LOCAL:
            op = JIT_SUMMARY_OP_GT;
            return_local_when_true = true;
            break;
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GT_LOCAL_CONST_RET_CONST:
            op = JIT_SUMMARY_OP_GT;
            return_local_when_true = false;
            break;
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GE_LOCAL_CONST_RET_LOCAL:
            op = JIT_SUMMARY_OP_GE;
            return_local_when_true = true;
            break;
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GE_LOCAL_CONST_RET_CONST:
            op = JIT_SUMMARY_OP_GE;
            return_local_when_true = false;
            break;
        default:
            vm_runtime_error(vm, "Invalid compiled entry");
            return -1;
    }

    return vm_finish_native_guarded_int_selector_const_summary_op(vm,
                                                                  args_base,
                                                                  op,
                                                                  value,
                                                                  guard_const,
                                                                  selector_const,
                                                                  return_local_when_true);
}

int vm_jit_stub_compiled_entry(VM* vm,
                               ObjFunction* function,
                               int args_base,
                               int arg_count,
                               CallFrame* io_frame,
                               Chunk** io_chunk,
                               uint8_t** io_code) {
    if (!vm || !function) return -1;
    function->jit_compiled_call_count++;
    return vm_enter_sync_function_from_call(vm, function, args_base, arg_count, io_frame, io_chunk, io_code);
}

int vm_jit_native_compiled_entry(VM* vm,
                                 ObjFunction* function,
                                 int args_base,
                                 int arg_count,
                                 CallFrame* io_frame,
                                 Chunk** io_chunk,
                                 uint8_t** io_code) {
    (void)io_frame;
    (void)io_chunk;
    (void)io_code;
    if (!vm || !function) return -1;
    if (arg_count != function->param_count) {
        vm_runtime_error(vm, "Wrong number of arguments");
        return -1;
    }
    if (args_base < 0 || args_base >= vm->stack.count) {
        vm_runtime_error(vm, "Stack underflow in call");
        return -1;
    }

    Value* arg0 = &vm->stack.values[args_base + function->jit_compiled_plan.local_slot];
    function->jit_compiled_call_count++;

    switch (function->jit_compiled_plan.kind) {
        case JIT_COMPILED_KIND_INT_ADD_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_MUL_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_SUB_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_DIV_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_MOD_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_BIT_AND_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_BIT_OR_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_BIT_XOR_LOCAL_CONST:
            if (value_get_type(arg0) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_int_const_op(vm,
                                                 args_base,
                                                 function->jit_compiled_plan.kind,
                                                 value_get_int(arg0),
                                                 function->jit_compiled_plan.int_const0);

        case JIT_COMPILED_KIND_INT_BINARY_LOCAL_CONST_GENERIC:
            if (value_get_type(arg0) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_int_binary_summary_op(vm,
                                                          args_base,
                                                          function->jit_compiled_plan.op,
                                                          value_get_int(arg0),
                                                          function->jit_compiled_plan.int_const0);

        case JIT_COMPILED_KIND_INT_ADD_LOCALS:
        case JIT_COMPILED_KIND_INT_SUB_LOCALS:
        case JIT_COMPILED_KIND_INT_MUL_LOCALS:
        case JIT_COMPILED_KIND_INT_BIT_AND_LOCALS:
        case JIT_COMPILED_KIND_INT_BIT_OR_LOCALS:
        case JIT_COMPILED_KIND_INT_BIT_XOR_LOCALS: {
            Value* arg1 = &vm->stack.values[args_base + function->jit_compiled_plan.local_slot_b];
            if (value_get_type(arg0) != VAL_INT || value_get_type(arg1) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_int_binary_op(vm,
                                                  args_base,
                                                  function->jit_compiled_plan.kind,
                                                  value_get_int(arg0),
                                                  value_get_int(arg1));
        }

        case JIT_COMPILED_KIND_INT_BINARY_LOCALS_GENERIC: {
            Value* arg1 = &vm->stack.values[args_base + function->jit_compiled_plan.local_slot_b];
            if (value_get_type(arg0) != VAL_INT || value_get_type(arg1) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_int_binary_summary_op(vm,
                                                          args_base,
                                                          function->jit_compiled_plan.op,
                                                          value_get_int(arg0),
                                                          value_get_int(arg1));
        }

        case JIT_COMPILED_KIND_BOOL_LT_LOCALS:
        case JIT_COMPILED_KIND_BOOL_LE_LOCALS:
        case JIT_COMPILED_KIND_BOOL_EQ_LOCALS:
        case JIT_COMPILED_KIND_BOOL_NE_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GT_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GE_LOCALS: {
            Value* arg1 = &vm->stack.values[args_base + function->jit_compiled_plan.local_slot_b];
            if (value_get_type(arg0) != VAL_INT || value_get_type(arg1) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_int_compare_op(vm,
                                                   args_base,
                                                   function->jit_compiled_plan.kind,
                                                   value_get_int(arg0),
                                                   value_get_int(arg1));
        }

        case JIT_COMPILED_KIND_BOOL_LT_LOCAL_CONST:
        case JIT_COMPILED_KIND_BOOL_LE_LOCAL_CONST:
        case JIT_COMPILED_KIND_BOOL_EQ_LOCAL_CONST:
        case JIT_COMPILED_KIND_BOOL_NE_LOCAL_CONST:
        case JIT_COMPILED_KIND_BOOL_GT_LOCAL_CONST:
        case JIT_COMPILED_KIND_BOOL_GE_LOCAL_CONST:
            if (value_get_type(arg0) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_int_compare_const_op(vm,
                                                         args_base,
                                                         function->jit_compiled_plan.kind,
                                                         value_get_int(arg0),
                                                         function->jit_compiled_plan.int_const0);

        case JIT_COMPILED_KIND_BOOL_COMPARE_LOCALS_GENERIC: {
            Value* arg1 = &vm->stack.values[args_base + function->jit_compiled_plan.local_slot_b];
            if (value_get_type(arg0) != VAL_INT || value_get_type(arg1) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_int_compare_summary_op(vm,
                                                           args_base,
                                                           function->jit_compiled_plan.op,
                                                           value_get_int(arg0),
                                                           value_get_int(arg1));
        }

        case JIT_COMPILED_KIND_BOOL_COMPARE_LOCAL_CONST_GENERIC:
            if (value_get_type(arg0) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_int_compare_summary_op(vm,
                                                           args_base,
                                                           function->jit_compiled_plan.op,
                                                           value_get_int(arg0),
                                                           function->jit_compiled_plan.int_const0);

        case JIT_COMPILED_KIND_INT_SELECT_LT_LOCAL_CONST_RET_LOCAL:
        case JIT_COMPILED_KIND_INT_SELECT_LT_LOCAL_CONST_RET_CONST:
        case JIT_COMPILED_KIND_INT_SELECT_LE_LOCAL_CONST_RET_LOCAL:
        case JIT_COMPILED_KIND_INT_SELECT_LE_LOCAL_CONST_RET_CONST:
        case JIT_COMPILED_KIND_INT_SELECT_GT_LOCAL_CONST_RET_LOCAL:
        case JIT_COMPILED_KIND_INT_SELECT_GT_LOCAL_CONST_RET_CONST:
        case JIT_COMPILED_KIND_INT_SELECT_GE_LOCAL_CONST_RET_LOCAL:
        case JIT_COMPILED_KIND_INT_SELECT_GE_LOCAL_CONST_RET_CONST:
            if (value_get_type(arg0) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_int_selector_const_op(vm,
                                                          args_base,
                                                          function->jit_compiled_plan.kind,
                                                          value_get_int(arg0),
                                                          function->jit_compiled_plan.int_const0);

        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LT_LOCAL_CONST_RET_LOCAL:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LT_LOCAL_CONST_RET_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LE_LOCAL_CONST_RET_LOCAL:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LE_LOCAL_CONST_RET_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GT_LOCAL_CONST_RET_LOCAL:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GT_LOCAL_CONST_RET_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GE_LOCAL_CONST_RET_LOCAL:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GE_LOCAL_CONST_RET_CONST:
            if (value_get_type(arg0) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_guarded_int_selector_const_op(vm,
                                                                  args_base,
                                                                  function->jit_compiled_plan.kind,
                                                                  value_get_int(arg0),
                                                                  function->jit_compiled_plan.int_const0,
                                                                  function->jit_compiled_plan.int_const1);

        case JIT_COMPILED_KIND_INT_SELECTOR_LOCAL_CONST_GENERIC:
            if (value_get_type(arg0) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_int_selector_const_summary_op(
                vm,
                args_base,
                function->jit_compiled_plan.op,
                value_get_int(arg0),
                function->jit_compiled_plan.int_const0,
                (function->jit_compiled_plan.flags & JIT_PLAN_FLAG_RETURN_LOCAL_ON_TRUE) != 0);

        case JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCAL_CONST_GENERIC:
            if (value_get_type(arg0) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_guarded_int_selector_const_summary_op(
                vm,
                args_base,
                function->jit_compiled_plan.op,
                value_get_int(arg0),
                function->jit_compiled_plan.int_const0,
                function->jit_compiled_plan.int_const1,
                (function->jit_compiled_plan.flags & JIT_PLAN_FLAG_RETURN_LOCAL_ON_TRUE) != 0);

        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_LT_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_LE_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_EQ_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_NE_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_GT_LOCALS:
        case JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_GE_LOCALS: {
            Value* arg1 = &vm->stack.values[args_base + function->jit_compiled_plan.local_slot_b];
            if (value_get_type(arg0) != VAL_INT || value_get_type(arg1) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_guarded_int_compare_op(vm,
                                                           args_base,
                                                           function->jit_compiled_plan.kind,
                                                           value_get_int(arg0),
                                                           value_get_int(arg1),
                                                           function->jit_compiled_plan.int_const0,
                                                           function->jit_compiled_plan.int_const1 != 0);
        }

        case JIT_COMPILED_KIND_BOOL_COMPARE_GUARDED_LOCALS_GENERIC: {
            Value* arg1 = &vm->stack.values[args_base + function->jit_compiled_plan.local_slot_b];
            if (value_get_type(arg0) != VAL_INT || value_get_type(arg1) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_guarded_int_compare_summary_op(
                vm,
                args_base,
                function->jit_compiled_plan.op,
                value_get_int(arg0),
                value_get_int(arg1),
                function->jit_compiled_plan.int_const0,
                function->jit_compiled_plan.int_const1 != 0);
        }

        case JIT_COMPILED_KIND_INT_SELECT_LT_LOCALS:
        case JIT_COMPILED_KIND_INT_SELECT_LE_LOCALS:
        case JIT_COMPILED_KIND_INT_SELECT_GT_LOCALS:
        case JIT_COMPILED_KIND_INT_SELECT_GE_LOCALS: {
            Value* left = &vm->stack.values[args_base];
            Value* right = &vm->stack.values[args_base + 1];
            if (value_get_type(left) != VAL_INT || value_get_type(right) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_int_selector_op(vm,
                                                    args_base,
                                                    function->jit_compiled_plan.kind,
                                                    value_get_int(left),
                                                    value_get_int(right),
                                                    function->jit_compiled_plan.local_slot,
                                                    function->jit_compiled_plan.local_slot_b);
        }

        case JIT_COMPILED_KIND_INT_SELECTOR_LOCALS_GENERIC: {
            Value* left = &vm->stack.values[args_base];
            Value* right = &vm->stack.values[args_base + 1];
            if (value_get_type(left) != VAL_INT || value_get_type(right) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_int_selector_summary_op(vm,
                                                            args_base,
                                                            function->jit_compiled_plan.op,
                                                            value_get_int(left),
                                                            value_get_int(right),
                                                            function->jit_compiled_plan.local_slot,
                                                            function->jit_compiled_plan.local_slot_b);
        }

        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LT_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LE_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GT_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GE_LOCALS: {
            Value* left = &vm->stack.values[args_base];
            Value* right = &vm->stack.values[args_base + 1];
            if (value_get_type(left) != VAL_INT || value_get_type(right) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_guarded_int_selector_op(vm,
                                                            args_base,
                                                            function->jit_compiled_plan.kind,
                                                            value_get_int(left),
                                                            value_get_int(right),
                                                            function->jit_compiled_plan.int_const0,
                                                            function->jit_compiled_plan.local_slot,
                                                            function->jit_compiled_plan.local_slot_b);
        }

        case JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCALS_GENERIC: {
            Value* left = &vm->stack.values[args_base];
            Value* right = &vm->stack.values[args_base + 1];
            if (value_get_type(left) != VAL_INT || value_get_type(right) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_guarded_int_selector_summary_op(
                vm,
                args_base,
                function->jit_compiled_plan.op,
                value_get_int(left),
                value_get_int(right),
                function->jit_compiled_plan.int_const0,
                function->jit_compiled_plan.local_slot,
                function->jit_compiled_plan.local_slot_b);
        }

        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_ADD_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SUB_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MUL_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_AND_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_OR_LOCALS:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_XOR_LOCALS: {
            JitCompiledKind binary_kind = function->jit_compiled_plan.kind;
            Value* guarded_local = &vm->stack.values[args_base];
            Value* arg1 = &vm->stack.values[args_base + function->jit_compiled_plan.local_slot_b];
            if (value_get_type(guarded_local) != VAL_INT || value_get_type(arg1) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            if (value_get_int(guarded_local) < function->jit_compiled_plan.int_const0) {
                return vm_finish_native_compiled_call_int(vm, args_base, value_get_int(guarded_local));
            }
            switch (binary_kind) {
                case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_ADD_LOCALS:
                    binary_kind = JIT_COMPILED_KIND_INT_ADD_LOCALS;
                    break;
                case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SUB_LOCALS:
                    binary_kind = JIT_COMPILED_KIND_INT_SUB_LOCALS;
                    break;
                case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MUL_LOCALS:
                    binary_kind = JIT_COMPILED_KIND_INT_MUL_LOCALS;
                    break;
                case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_AND_LOCALS:
                    binary_kind = JIT_COMPILED_KIND_INT_BIT_AND_LOCALS;
                    break;
                case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_OR_LOCALS:
                    binary_kind = JIT_COMPILED_KIND_INT_BIT_OR_LOCALS;
                    break;
                case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_XOR_LOCALS:
                    binary_kind = JIT_COMPILED_KIND_INT_BIT_XOR_LOCALS;
                    break;
                default:
                    vm_runtime_error(vm, "Invalid compiled entry");
                    return -1;
            }
            return vm_finish_native_int_binary_op(vm,
                                                  args_base,
                                                  binary_kind,
                                                  value_get_int(arg0),
                                                  value_get_int(arg1));
        }

        case JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCALS_GENERIC: {
            Value* arg1 = &vm->stack.values[args_base + function->jit_compiled_plan.local_slot_b];
            if (value_get_type(arg0) != VAL_INT || value_get_type(arg1) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be integer");
                return -1;
            }
            return vm_finish_native_guarded_int_binary_summary_op(vm,
                                                                  args_base,
                                                                  function->jit_compiled_plan.op,
                                                                  value_get_int(arg0),
                                                                  value_get_int(arg1),
                                                                  function->jit_compiled_plan.int_const0);
        }

        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_ADD_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MUL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SUB_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_DIV_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MOD_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_AND_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_OR_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_XOR_CONST:
            if (value_get_type(arg0) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be same numeric type");
                return -1;
            }
            if (value_get_int(arg0) < function->jit_compiled_plan.int_const0) {
                return vm_finish_native_compiled_call_int(vm, args_base, value_get_int(arg0));
            }
            return vm_finish_native_int_const_op(vm,
                                                 args_base,
                                                 function->jit_compiled_plan.kind,
                                                 value_get_int(arg0),
                                                 function->jit_compiled_plan.int_const1);

        case JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCAL_CONST_GENERIC:
            if (value_get_type(arg0) != VAL_INT) {
                vm_runtime_error(vm, "Operands must be same numeric type");
                return -1;
            }
            return vm_finish_native_guarded_int_binary_summary_op(vm,
                                                                  args_base,
                                                                  function->jit_compiled_plan.op,
                                                                  value_get_int(arg0),
                                                                  function->jit_compiled_plan.int_const1,
                                                                  function->jit_compiled_plan.int_const0);

        default:
            vm_runtime_error(vm, "Invalid compiled entry");
            return -1;
    }
}

static int64_t vm_time_monotonic_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency = {0};
    static bool initialized = false;
    if (!initialized) {
        QueryPerformanceFrequency(&frequency);
        initialized = true;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    if (frequency.QuadPart == 0) {
        return 0;
    }
    return (int64_t)((counter.QuadPart * 1000LL) / frequency.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000L);
#endif
}

static void vm_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    DWORD sleep_ms = ms > INT64_C(0xffffffff) ? (DWORD)0xffffffffu : (DWORD)ms;
    Sleep(sleep_ms);
#else
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000LL);
    req.tv_nsec = (long)((ms % 1000LL) * 1000000L);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
#endif
}

static bool vm_enqueue_timer_waiter(VM* vm, ObjRecord* future, int64_t deadline_ms) {
    if (!vm || !future) return false;

    TimerWaitEntry* entry = (TimerWaitEntry*)safe_malloc(sizeof(TimerWaitEntry));
    entry->future = future;
    entry->deadline_ms = deadline_ms;
    entry->next = NULL;
    obj_record_retain(future);

    if (!vm->timer_waiters || deadline_ms < vm->timer_waiters->deadline_ms) {
        entry->next = vm->timer_waiters;
        vm->timer_waiters = entry;
        return true;
    }

    TimerWaitEntry* current = vm->timer_waiters;
    while (current->next && current->next->deadline_ms <= deadline_ms) {
        current = current->next;
    }
    entry->next = current->next;
    current->next = entry;
    return true;
}

static void vm_process_expired_timers(VM* vm) {
    if (!vm) return;

    int64_t now_ms = vm_time_monotonic_ms();
    while (vm->timer_waiters && vm->timer_waiters->deadline_ms <= now_ms) {
        TimerWaitEntry* entry = vm->timer_waiters;
        vm->timer_waiters = entry->next;

        Value nil;
        value_init_nil(&nil);
        vm_future_complete(vm, entry->future, nil);
        obj_record_release(entry->future);
        free(entry);
    }
}

static bool vm_wait_for_next_timer(VM* vm) {
    if (!vm || !vm->timer_waiters) return false;

    int64_t now_ms = vm_time_monotonic_ms();
    int64_t wait_ms = vm->timer_waiters->deadline_ms - now_ms;
    if (wait_ms > 0) {
        vm_posted_event_queue_wait(vm->posted_event_auto_drain ? vm->posted_event_queue : NULL, wait_ms);
    }
    vm_process_expired_timers(vm);
    return true;
}

bool vm_enqueue_poll_waiter(VM* vm, VmPollWaitCallback callback, VmPollWaitFreeFn free_fn, void* payload) {
    if (!vm || !callback || !payload) return false;

    VmPollWaitEntry* entry = (VmPollWaitEntry*)safe_malloc(sizeof(VmPollWaitEntry));
    entry->callback = callback;
    entry->free_fn = free_fn;
    entry->payload = payload;
    entry->next = NULL;

    if (!vm->poll_waiters) {
        vm->poll_waiters = entry;
        return true;
    }

    VmPollWaitEntry* current = vm->poll_waiters;
    while (current->next) {
        current = current->next;
    }
    current->next = entry;
    return true;
}

int vm_debug_ready_task_count(VM* vm) {
    int count = 0;
    if (!vm) return 0;

    for (AsyncTask* task = vm->ready_tasks_head; task; task = task->next) {
        count++;
    }
    return count;
}

int vm_debug_future_waiter_entry_count(VM* vm) {
    int count = 0;
    if (!vm) return 0;

    for (FutureWaitEntry* entry = vm->future_waiters; entry; entry = entry->next) {
        count++;
    }
    return count;
}

int vm_debug_future_waiter_task_count(VM* vm) {
    int count = 0;
    if (!vm) return 0;

    for (FutureWaitEntry* entry = vm->future_waiters; entry; entry = entry->next) {
        for (AsyncTask* task = entry->tasks_head; task; task = task->next) {
            count++;
        }
    }
    return count;
}

int vm_debug_timer_waiter_count(VM* vm) {
    int count = 0;
    if (!vm) return 0;

    for (TimerWaitEntry* entry = vm->timer_waiters; entry; entry = entry->next) {
        count++;
    }
    return count;
}

int vm_debug_poll_waiter_count(VM* vm) {
    int count = 0;
    if (!vm) return 0;

    for (VmPollWaitEntry* entry = vm->poll_waiters; entry; entry = entry->next) {
        count++;
    }
    return count;
}

static int vm_breakpoint_frame_line(const CallFrame* frame);

bool vm_debug_add_line_breakpoint(VM* vm, const char* source_file, int line) {
    if (!vm || !source_file || source_file[0] == '\0' || line <= 0) return false;

    if (vm->debug_breakpoint_count >= vm->debug_breakpoint_capacity) {
        int new_capacity = vm->debug_breakpoint_capacity > 0 ? vm->debug_breakpoint_capacity * 2 : 4;
        VmLineBreakpoint* new_breakpoints =
            (VmLineBreakpoint*)realloc(vm->debug_breakpoints, (size_t)new_capacity * sizeof(VmLineBreakpoint));
        if (!new_breakpoints) {
            return false;
        }
        vm->debug_breakpoints = new_breakpoints;
        vm->debug_breakpoint_capacity = new_capacity;
    }

    vm->debug_breakpoints[vm->debug_breakpoint_count].source_file = source_file;
    vm->debug_breakpoints[vm->debug_breakpoint_count].line = line;
    vm->debug_breakpoint_count++;
    return true;
}

void vm_debug_set_line_breakpoint(VM* vm, const char* source_file, int line) {
    if (!vm) return;
    vm_debug_clear_line_breakpoints(vm);
    (void)vm_debug_add_line_breakpoint(vm, source_file, line);
}

void vm_debug_clear_line_breakpoints(VM* vm) {
    if (!vm) return;
    if (vm->debug_breakpoints) {
        free(vm->debug_breakpoints);
        vm->debug_breakpoints = NULL;
    }
    vm->debug_breakpoint_count = 0;
    vm->debug_breakpoint_capacity = 0;
}

void vm_debug_set_break_on_runtime_error(VM* vm, bool enabled) {
    if (!vm) return;
    vm->debug_break_on_runtime_error = enabled;
}

void vm_debug_request_stop_on_entry(VM* vm) {
    if (!vm) return;
    vm->debug_stop_on_entry_pending = true;
    vm_debug_clear_stop_info(vm);
}

static void vm_debug_store_pause_requested(VM* vm, bool requested) {
    if (!vm) return;
#ifdef _WIN32
    InterlockedExchange((volatile LONG*)&vm->debug_pause_requested, requested ? 1L : 0L);
#else
    __atomic_store_n(&vm->debug_pause_requested, requested ? 1L : 0L, __ATOMIC_SEQ_CST);
#endif
}

static bool vm_debug_exchange_pause_requested(VM* vm, bool requested) {
    if (!vm) return false;
#ifdef _WIN32
    return InterlockedExchange((volatile LONG*)&vm->debug_pause_requested, requested ? 1L : 0L) != 0;
#else
    return __atomic_exchange_n(&vm->debug_pause_requested, requested ? 1L : 0L, __ATOMIC_SEQ_CST) != 0;
#endif
}

void vm_debug_request_pause(VM* vm) {
    vm_debug_store_pause_requested(vm, true);
}

void vm_debug_prepare_continue(VM* vm) {
    if (!vm) return;
    vm->debug_exec_mode = VM_DEBUG_EXEC_NONE;
    vm->debug_step_source_file = NULL;
    vm->debug_step_line = 0;
    vm->debug_step_depth = 0;
    if (vm->frame_count > 0) {
        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        vm->debug_skip_source_file = frame->function ? frame->function->source_file : NULL;
        vm->debug_skip_line = vm_breakpoint_frame_line(frame);
        vm->debug_skip_depth = vm->frame_count;
    } else {
        vm->debug_skip_source_file = NULL;
        vm->debug_skip_line = 0;
        vm->debug_skip_depth = 0;
    }
    vm_debug_clear_stop_info(vm);
}

void vm_debug_prepare_step_in(VM* vm) {
    if (!vm) return;
    vm_debug_prepare_continue(vm);
    vm->debug_exec_mode = VM_DEBUG_EXEC_STEP_IN;
    if (vm->frame_count > 0) {
        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        vm->debug_step_source_file = frame->function ? frame->function->source_file : NULL;
        vm->debug_step_line = vm_breakpoint_frame_line(frame);
        vm->debug_step_depth = vm->frame_count;
    }
}

void vm_debug_prepare_step_over(VM* vm) {
    if (!vm) return;
    vm_debug_prepare_continue(vm);
    vm->debug_exec_mode = VM_DEBUG_EXEC_STEP_OVER;
    if (vm->frame_count > 0) {
        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        vm->debug_step_source_file = frame->function ? frame->function->source_file : NULL;
        vm->debug_step_line = vm_breakpoint_frame_line(frame);
        vm->debug_step_depth = vm->frame_count;
    }
}

void vm_debug_prepare_step_out(VM* vm) {
    if (!vm) return;
    vm_debug_prepare_continue(vm);
    vm->debug_exec_mode = VM_DEBUG_EXEC_STEP_OUT;
    if (vm->frame_count > 0) {
        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        vm->debug_step_source_file = frame->function ? frame->function->source_file : NULL;
        vm->debug_step_line = vm_breakpoint_frame_line(frame);
        vm->debug_step_depth = vm->frame_count;
    }
}

const VmDebugStopInfo* vm_debug_get_stop_info(VM* vm) {
    if (!vm) return NULL;
    return &vm->debug_stop;
}

void vm_debug_clear_stop_info(VM* vm) {
    if (!vm) return;
    vm->debug_stop.kind = VM_DEBUG_STOP_NONE;
    vm->debug_stop.source_file = NULL;
    vm->debug_stop.function_name = NULL;
    vm->debug_stop.line = 0;
    vm->debug_stop.call_depth = 0;
    vm->debug_stop.ip = 0;
}

int vm_debug_frame_count(VM* vm) {
    if (!vm) return 0;
    return vm->frame_count;
}

static int vm_debug_frame_visible_local_count_internal(VM* vm, int index_from_top, CallFrame** out_frame) {
    int frame_index = 0;
    CallFrame* frame = NULL;
    int visible_slots = 0;
    int available_stack = 0;

    if (out_frame) *out_frame = NULL;
    if (!vm || index_from_top < 0 || index_from_top >= vm->frame_count) {
        return 0;
    }

    frame_index = vm->frame_count - 1 - index_from_top;
    frame = &vm->frames[frame_index];
    visible_slots = frame->slots_count;
    available_stack = vm->stack.count - frame->slots_start;
    if (available_stack < visible_slots) {
        visible_slots = available_stack;
    }
    if (visible_slots < 0) {
        visible_slots = 0;
    }
    if (frame->function && frame->function->local_count < visible_slots) {
        visible_slots = frame->function->local_count;
    }
    if (out_frame) {
        *out_frame = frame;
    }
    return visible_slots;
}

static int vm_debug_frame_visible_argument_count_internal(VM* vm, int index_from_top, CallFrame** out_frame) {
    CallFrame* frame = NULL;
    int visible_locals = vm_debug_frame_visible_local_count_internal(vm, index_from_top, &frame);
    int visible_arguments = 0;

    if (out_frame) *out_frame = frame;
    if (!frame || !frame->function || frame->function->param_count <= 0) {
        return 0;
    }

    visible_arguments = frame->function->param_count;
    if (visible_arguments > visible_locals) {
        visible_arguments = visible_locals;
    }
    if (visible_arguments < 0) {
        visible_arguments = 0;
    }
    return visible_arguments;
}

bool vm_debug_get_frame_info(VM* vm, int index_from_top, VmDebugFrameInfo* out) {
    if (!vm || !out || index_from_top < 0 || index_from_top >= vm->frame_count) {
        return false;
    }

    int frame_index = vm->frame_count - 1 - index_from_top;
    CallFrame* frame = &vm->frames[frame_index];
    out->function_name = NULL;
    out->source_file = NULL;
    out->line = 0;
    if (!frame) return true;

    if (frame->function && frame->function->name && frame->function->name[0] != '\0') {
        out->function_name = frame->function->name;
    } else if (frame_index == 0) {
        out->function_name = "<main>";
    } else {
        out->function_name = "<anon>";
    }

    if (frame->function) {
        out->source_file = frame->function->source_file;
    }
    if (index_from_top == 0 && vm->debug_stop.kind == VM_DEBUG_STOP_BREAKPOINT) {
        if (frame->function && frame->function->chunk.debug_info &&
            frame->ip >= 0 && frame->ip < frame->function->chunk.code_count) {
            out->line = frame->function->chunk.debug_info[frame->ip].line;
        }
    } else if (frame->function && frame->function->chunk.debug_info) {
        int ip = frame->ip > 0 ? frame->ip - 1 : 0;
        if (ip >= 0 && ip < frame->function->chunk.code_count) {
            out->line = frame->function->chunk.debug_info[ip].line;
        }
    }
    return true;
}

int vm_debug_frame_argument_count(VM* vm, int index_from_top) {
    return vm_debug_frame_visible_argument_count_internal(vm, index_from_top, NULL);
}

static bool vm_debug_assign_value_slot(Value* slot, const Value* value) {
    Value stored;

    if (!slot || !value) return false;
    stored = *value;
    value_retain(&stored);
    value_free(slot);
    *slot = stored;
    return true;
}

bool vm_debug_get_frame_argument(VM* vm,
                                 int index_from_top,
                                 int argument_index,
                                 const char** out_name,
                                 const Value** out_value) {
    CallFrame* frame = NULL;
    int visible_arguments = 0;

    if (out_name) *out_name = NULL;
    if (out_value) *out_value = NULL;
    visible_arguments = vm_debug_frame_visible_argument_count_internal(vm, index_from_top, &frame);
    if (!frame || argument_index < 0 || argument_index >= visible_arguments) {
        return false;
    }

    if (out_name) {
        const char* name = NULL;
        if (frame->function) {
            if (frame->function->param_names) {
                name = frame->function->param_names[argument_index];
            }
            if ((!name || name[0] == '\0') && frame->function->local_names) {
                name = frame->function->local_names[argument_index];
            }
            if ((!name || name[0] == '\0') && frame->function->debug_local_names) {
                name = frame->function->debug_local_names[argument_index];
            }
        }
        *out_name = name;
    }
    if (out_value) {
        *out_value = &vm->stack.values[frame->slots_start + argument_index];
    }
    return true;
}

bool vm_debug_set_frame_argument(VM* vm,
                                 int index_from_top,
                                 int argument_index,
                                 const Value* value) {
    CallFrame* frame = NULL;
    int visible_arguments = vm_debug_frame_visible_argument_count_internal(vm, index_from_top, &frame);

    if (!frame || argument_index < 0 || argument_index >= visible_arguments || !value) {
        return false;
    }
    return vm_debug_assign_value_slot(&vm->stack.values[frame->slots_start + argument_index], value);
}

int vm_debug_frame_local_count(VM* vm, int index_from_top) {
    return vm_debug_frame_visible_local_count_internal(vm, index_from_top, NULL);
}

bool vm_debug_get_frame_local(VM* vm,
                              int index_from_top,
                              int local_index,
                              const char** out_name,
                              const Value** out_value) {
    if (out_name) *out_name = NULL;
    if (out_value) *out_value = NULL;
    if (!vm || index_from_top < 0 || index_from_top >= vm->frame_count) {
        return false;
    }

    int visible_locals = vm_debug_frame_local_count(vm, index_from_top);
    if (local_index < 0 || local_index >= visible_locals) {
        return false;
    }

    int frame_index = vm->frame_count - 1 - index_from_top;
    CallFrame* frame = &vm->frames[frame_index];
    if (out_name) {
        const char* name = NULL;
        if (frame->function) {
            if (frame->function->local_names) {
                name = frame->function->local_names[local_index];
            }
            if ((!name || name[0] == '\0') && frame->function->debug_local_names) {
                name = frame->function->debug_local_names[local_index];
            }
        }
        *out_name = name;
    }
    if (out_value) {
        *out_value = &vm->stack.values[frame->slots_start + local_index];
    }
    return true;
}

bool vm_debug_set_frame_local(VM* vm,
                              int index_from_top,
                              int local_index,
                              const Value* value) {
    int visible_locals = 0;
    int frame_index = 0;
    CallFrame* frame = NULL;

    if (!vm || !value || index_from_top < 0 || index_from_top >= vm->frame_count) {
        return false;
    }

    visible_locals = vm_debug_frame_local_count(vm, index_from_top);
    if (local_index < 0 || local_index >= visible_locals) {
        return false;
    }

    frame_index = vm->frame_count - 1 - index_from_top;
    frame = &vm->frames[frame_index];
    return vm_debug_assign_value_slot(&vm->stack.values[frame->slots_start + local_index], value);
}

int vm_debug_frame_non_argument_local_count(VM* vm, int index_from_top) {
    int visible_locals = vm_debug_frame_visible_local_count_internal(vm, index_from_top, NULL);
    int visible_arguments = vm_debug_frame_visible_argument_count_internal(vm, index_from_top, NULL);
    int local_count = visible_locals - visible_arguments;

    return local_count > 0 ? local_count : 0;
}

bool vm_debug_get_frame_non_argument_local(VM* vm,
                                           int index_from_top,
                                           int local_index,
                                           const char** out_name,
                                           const Value** out_value) {
    int visible_arguments = vm_debug_frame_visible_argument_count_internal(vm, index_from_top, NULL);
    int local_slot = visible_arguments + local_index;

    if (out_name) *out_name = NULL;
    if (out_value) *out_value = NULL;
    if (local_index < 0 || local_index >= vm_debug_frame_non_argument_local_count(vm, index_from_top)) {
        return false;
    }
    return vm_debug_get_frame_local(vm, index_from_top, local_slot, out_name, out_value);
}

bool vm_debug_set_frame_non_argument_local(VM* vm,
                                           int index_from_top,
                                           int local_index,
                                           const Value* value) {
    int visible_arguments = vm_debug_frame_visible_argument_count_internal(vm, index_from_top, NULL);
    int local_slot = visible_arguments + local_index;

    if (!value) return false;
    if (local_index < 0 || local_index >= vm_debug_frame_non_argument_local_count(vm, index_from_top)) {
        return false;
    }
    return vm_debug_set_frame_local(vm, index_from_top, local_slot, value);
}

int vm_debug_global_count(VM* vm) {
    if (!vm) return 0;
    return vm->global_count;
}

bool vm_debug_get_global(VM* vm, int global_index, const char** out_name, const Value** out_value) {
    if (out_name) *out_name = NULL;
    if (out_value) *out_value = NULL;
    if (!vm || global_index < 0 || global_index >= vm->global_count) {
        return false;
    }
    if (out_name) {
        *out_name = vm->global_names ? vm->global_names[global_index] : NULL;
    }
    if (out_value) {
        *out_value = &vm->globals[global_index];
    }
    return true;
}

bool vm_debug_set_global(VM* vm, int global_index, const Value* value) {
    if (!vm || !value || global_index < 0 || global_index >= vm->global_count) {
        return false;
    }
    return vm_debug_assign_value_slot(&vm->globals[global_index], value);
}

static void vm_process_poll_waiters(VM* vm) {
    if (!vm) return;

    VmPollWaitEntry* prev = NULL;
    VmPollWaitEntry* entry = vm->poll_waiters;
    while (entry) {
        VmPollWaitEntry* next = entry->next;
        bool complete = entry->callback ? entry->callback(vm, entry->payload) : true;
        if (complete) {
            if (prev) {
                prev->next = next;
            } else {
                vm->poll_waiters = next;
            }
            if (entry->free_fn) {
                entry->free_fn(entry->payload);
            }
            free(entry);
        } else {
            prev = entry;
        }
        entry = next;
    }
}

static bool vm_wait_for_next_idle_work(VM* vm) {
    if (!vm) return false;
    if (!vm->poll_waiters && !vm->timer_waiters) {
        return false;
    }

    if (vm->poll_waiters) {
        int64_t wait_ms = 1;
        if (vm->timer_waiters) {
            int64_t now_ms = vm_time_monotonic_ms();
            int64_t timer_wait_ms = vm->timer_waiters->deadline_ms - now_ms;
            if (timer_wait_ms <= 0) {
                vm_process_expired_timers(vm);
                vm_process_poll_waiters(vm);
                return true;
            }
            if (timer_wait_ms < wait_ms) {
                wait_ms = timer_wait_ms;
            }
        }
        if (wait_ms > 0) {
            vm_posted_event_queue_wait(vm->posted_event_auto_drain ? vm->posted_event_queue : NULL, wait_ms);
        }
        vm_process_expired_timers(vm);
        vm_process_poll_waiters(vm);
        return true;
    }

    return vm_wait_for_next_timer(vm);
}

static bool vm_bind_captures_to_stack_values(Value* stack_values, int stack_count, ObjFunction* func) {
    if (!func || func->capture_count <= 0) return true;
    if (!stack_values) return false;

    for (int i = 0; i < func->capture_count; i++) {
        int slot = func->capture_local_slots ? func->capture_local_slots[i] : -1;
        if (slot < 0 || slot >= stack_count) {
            return false;
        }

        value_free(&stack_values[slot]);
        stack_values[slot] = func->captured_values[i];
        if (!value_type_is_unmanaged(value_get_type(&stack_values[slot]))) {
            value_retain(&stack_values[slot]);
        }
    }

    return true;
}

static FutureWaitEntry* vm_find_future_wait_entry(VM* vm, ObjRecord* future, FutureWaitEntry** out_prev) {
    if (out_prev) *out_prev = NULL;
    if (!vm || !future) return NULL;

    FutureWaitEntry* prev = NULL;
    FutureWaitEntry* entry = vm->future_waiters;
    while (entry) {
        if (entry->future == future) {
            if (out_prev) *out_prev = prev;
            return entry;
        }
        prev = entry;
        entry = entry->next;
    }
    return NULL;
}

static bool vm_enqueue_future_waiter(VM* vm, ObjRecord* future, AsyncTask* task) {
    if (!vm || !future || !task) return false;

    FutureWaitEntry* entry = vm_find_future_wait_entry(vm, future, NULL);
    if (!entry) {
        entry = (FutureWaitEntry*)safe_malloc(sizeof(FutureWaitEntry));
        entry->future = future;
        obj_record_retain(future);
        entry->tasks_head = NULL;
        entry->tasks_tail = NULL;
        entry->next = vm->future_waiters;
        vm->future_waiters = entry;
    }

    task->next = NULL;
    if (entry->tasks_tail) {
        entry->tasks_tail->next = task;
    } else {
        entry->tasks_head = task;
    }
    entry->tasks_tail = task;
    return true;
}

static void vm_wake_future_waiters(VM* vm, ObjRecord* future) {
    if (!vm || !future) return;

    FutureWaitEntry* prev = NULL;
    FutureWaitEntry* entry = vm_find_future_wait_entry(vm, future, &prev);
    if (!entry) return;

    Value resolved;
    value_init_nil(&resolved);
    bool has_resolved_value = obj_future_try_get(future, &resolved);
    const char* panic_message = has_resolved_value ? NULL : obj_future_get_panic_message(future);
    if (!has_resolved_value && !panic_message) {
        return;
    }

    AsyncTask* task = entry->tasks_head;
    while (task) {
        AsyncTask* next = task->next;
        task->next = NULL;
        if (has_resolved_value) {
            task->has_resume_value = true;
            task->resume_value = resolved;
            if (!value_type_is_unmanaged(value_get_type(&task->resume_value))) {
                value_retain(&task->resume_value);
            }
        } else {
            task->has_resume_panic = true;
            task->resume_panic_message = safe_strdup(panic_message);
        }
        vm_enqueue_ready_task(vm, task);
        task = next;
    }
    if (has_resolved_value) {
        value_free(&resolved);
    }

    if (prev) {
        prev->next = entry->next;
    } else {
        vm->future_waiters = entry->next;
    }
    obj_record_release(entry->future);
    free(entry);
}

bool vm_future_complete(VM* vm, ObjRecord* future, Value value) {
    if (!obj_future_resolve(future, value)) {
        return false;
    }
    vm_wake_future_waiters(vm, future);
    return true;
}

bool vm_future_complete_panic(VM* vm, ObjRecord* future, const char* message) {
    if (!obj_future_resolve_panic(vm, future, message)) {
        return false;
    }
    vm_wake_future_waiters(vm, future);
    return true;
}

ObjRecord* vm_future_sleep(VM* vm, int64_t delay_ms) {
    Value nil;
    value_init_nil(&nil);

    if (delay_ms <= 0) {
        return obj_future_create_resolved(vm, nil);
    }

    ObjRecord* future = obj_future_create_pending(vm);
    if (!future) {
        return NULL;
    }

    int64_t deadline_ms = vm_time_monotonic_ms() + delay_ms;
    if (!vm_enqueue_timer_waiter(vm, future, deadline_ms)) {
        obj_record_release(future);
        return NULL;
    }

    return future;
}

static int vm_resume_ready_task(VM* vm, CallFrame* current_frame, CallFrame* out_frame, Chunk** out_chunk, uint8_t** out_code) {
    if (!vm || !out_frame || !out_chunk || !out_code) return -1;

    AsyncTask* task = vm_dequeue_ready_task(vm);
    if (!task) return 0;

    if (current_frame && vm->frame_count > 0) {
        vm->frames[vm->frame_count - 1] = *current_frame;
    }

    vm_stack_reserve(vm, task->stack_count + (task->has_resume_value ? 1 : 0));
    if (vm->error_occurred) {
        async_task_free(task);
        return -1;
    }

    int base = vm->stack.count;
    for (int i = 0; i < task->stack_count; i++) {
        vm->stack.values[base + i] = task->stack_values[i];
        value_init_nil(&task->stack_values[i]);
    }
    vm->stack.count += task->stack_count;

    if (task->has_resume_value) {
        vm->stack.values[vm->stack.count++] = task->resume_value;
        value_init_nil(&task->resume_value);
        task->has_resume_value = false;
    }

    CallFrame new_frame;
    new_frame.function = task->function;
    new_frame.ip = task->ip;
    new_frame.slots_start = base;
    new_frame.slots_count = task->slots_count;
    new_frame.defers = task->defers;
    new_frame.defer_count = task->defer_count;
    new_frame.defer_capacity = task->defer_capacity;
    new_frame.is_async_root = true;
    new_frame.async_result_future = task->async_result_future;
    new_frame.panic_unwinding = task->panic_unwinding;
    new_frame.panic_message = task->panic_message;

    task->defers = NULL;
    task->defer_count = 0;
    task->defer_capacity = 0;
    task->async_result_future = NULL;
    task->panic_message = NULL;
    task->function = NULL;

    if (!task->entry_profiled) {
        jit_record_function_entry(vm, new_frame.function);
        task->entry_profiled = true;
    }

    vm_frame_reserve(vm, 1);
    vm->frames[vm->frame_count++] = new_frame;
    vm->current_call_depth = vm->frame_count;

    *out_frame = new_frame;
    *out_chunk = &new_frame.function->chunk;
    *out_code = (*out_chunk)->code;

    if (task->has_resume_panic && task->resume_panic_message) {
        vm_runtime_error(vm, task->resume_panic_message);
        free(task->resume_panic_message);
        task->resume_panic_message = NULL;
        task->has_resume_panic = false;
    }

    async_task_free(task);
    return 1;
}

static int vm_spawn_async_call(VM* vm,
                               ObjFunction* called_func,
                               int result_pos,
                               int arg_count,
                               bool callee_on_stack) {
    if (!vm || !called_func) return -1;
    if (called_func->local_count < arg_count) {
        vm_runtime_error(vm, "Function local slots less than arguments");
        return -1;
    }

    AsyncTask* task = async_task_create(called_func, called_func->local_count);
    ObjRecord* result_future = obj_future_create_pending(vm);
    if (!task || !result_future) {
        if (task) async_task_free(task);
        if (result_future) obj_record_release(result_future);
        vm_runtime_error(vm, "Failed to allocate async task");
        return -1;
    }

    task->async_result_future = result_future;
    obj_record_retain(result_future);

    int args_base = callee_on_stack ? (result_pos + 1) : result_pos;
    for (int i = 0; i < arg_count; i++) {
        task->stack_values[i] = vm->stack.values[args_base + i];
        if (!value_type_is_unmanaged(value_get_type(&task->stack_values[i]))) {
            value_retain(&task->stack_values[i]);
        }
    }

    if (!vm_bind_captures_to_stack_values(task->stack_values, task->stack_count, called_func)) {
        async_task_free(task);
        obj_record_release(result_future);
        vm_runtime_error(vm, "Closure capture slot out of bounds");
        return -1;
    }

    int dropped = arg_count + (callee_on_stack ? 1 : 0);
    for (int i = 0; i < dropped; i++) {
        value_free(&vm->stack.values[result_pos + i]);
    }

    if (result_pos >= vm->stack.count) {
        vm_stack_reserve(vm, 1);
        if (vm->error_occurred) {
            async_task_free(task);
            obj_record_release(result_future);
            return -1;
        }
    }

    Value future_value;
    value_init_record(&future_value, result_future);
    vm->stack.values[result_pos] = future_value;
    vm->stack.count = result_pos + 1;

    vm_enqueue_ready_task(vm, task);
    return 0;
}

static void vm_appendf(char* out, size_t cap, size_t* off, const char* fmt, ...) {
    if (!out || cap == 0 || !off || !fmt) return;
    if (*off >= cap - 1) return;

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(out + *off, cap - *off, fmt, args);
    va_end(args);

    if (written < 0) {
        out[cap - 1] = '\0';
        *off = cap - 1;
        return;
    }

    size_t add = (size_t)written;
    if (add >= cap - *off) {
        *off = cap - 1;
        out[*off] = '\0';
        return;
    }
    *off += add;
}

static const char* vm_trace_frame_name(const CallFrame* frame, int frame_index) {
    if (frame && frame->function && frame->function->name && frame->function->name[0] != '\0') {
        return frame->function->name;
    }
    if (frame_index == 0) return "<main>";
    return "<anon>";
}

static int vm_trace_frame_line(const CallFrame* frame) {
    if (!frame || !frame->function) return 0;
    int ip = frame->ip > 0 ? frame->ip - 1 : 0;
    if (!frame->function->chunk.debug_info) return 0;
    if (ip < 0 || ip >= frame->function->chunk.code_count) return 0;
    return frame->function->chunk.debug_info[ip].line;
}

static int vm_breakpoint_frame_line(const CallFrame* frame) {
    if (!frame || !frame->function) return 0;
    if (!frame->function->chunk.debug_info) return 0;
    if (frame->ip < 0 || frame->ip >= frame->function->chunk.code_count) return 0;
    return frame->function->chunk.debug_info[frame->ip].line;
}

static bool vm_path_is_sep(char c) {
    return c == '/' || c == '\\';
}

static bool vm_path_suffix_component_match(const char* path, const char* suffix) {
    if (!path || !suffix) return false;
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    if (suffix_len == 0 || suffix_len > path_len) return false;
    size_t offset = path_len - suffix_len;
    if (offset > 0 && !vm_path_is_sep(path[offset - 1])) return false;

    for (size_t i = 0; i < suffix_len; i++) {
        char a = path[offset + i];
        char b = suffix[i];
        if (vm_path_is_sep(a) && vm_path_is_sep(b)) continue;
        if (a != b) return false;
    }
    return true;
}

static bool vm_paths_match_loose(const char* a, const char* b) {
    if (!a || !b) return false;
    if (strcmp(a, b) == 0) return true;
    return vm_path_suffix_component_match(a, b) || vm_path_suffix_component_match(b, a);
}

static bool vm_should_skip_current_debug_location(VM* vm, CallFrame* frame) {
    if (!vm || !frame) return false;
    if (!vm->debug_skip_source_file || vm->debug_skip_line <= 0) return false;

    int line = vm_breakpoint_frame_line(frame);
    const char* source_file = frame->function ? frame->function->source_file : NULL;
    if (line > 0 &&
        source_file &&
        vm_paths_match_loose(source_file, vm->debug_skip_source_file) &&
        line == vm->debug_skip_line) {
        return true;
    }

    if (vm->frame_count <= vm->debug_skip_depth) {
        vm->debug_skip_source_file = NULL;
        vm->debug_skip_line = 0;
        vm->debug_skip_depth = 0;
    }
    return false;
}

static void vm_capture_debug_stop(VM* vm, CallFrame* frame, VmDebugStopKind kind, int line) {
    if (!vm || !frame) return;
    vm->debug_stop.kind = kind;
    vm->debug_stop.source_file = frame->function ? frame->function->source_file : NULL;
    vm->debug_stop.function_name = vm_trace_frame_name(frame, vm->frame_count - 1);
    vm->debug_stop.line = line;
    vm->debug_stop.call_depth = vm->frame_count;
    vm->debug_stop.ip = frame->ip;
    if (vm->frame_count > 0) {
        vm->frames[vm->frame_count - 1] = *frame;
    }
}

static bool vm_maybe_stop_on_entry(VM* vm, CallFrame* frame) {
    int line = 0;

    if (!vm || !frame || !vm->debug_stop_on_entry_pending) return false;
    if (!frame->function || !frame->function->source_file) return false;

    vm->debug_stop_on_entry_pending = false;
    line = vm_breakpoint_frame_line(frame);
    vm_capture_debug_stop(vm, frame, VM_DEBUG_STOP_ENTRY, line);
    return true;
}

static bool vm_maybe_stop_for_pause(VM* vm, CallFrame* frame) {
    int line = 0;

    if (!vm || !frame) return false;
    if (!vm_debug_exchange_pause_requested(vm, false)) return false;
    if (!frame->function || !frame->function->source_file) return false;

    line = vm_breakpoint_frame_line(frame);
    vm->debug_exec_mode = VM_DEBUG_EXEC_NONE;
    vm->debug_step_source_file = NULL;
    vm->debug_step_line = 0;
    vm->debug_step_depth = 0;
    vm->debug_skip_source_file = NULL;
    vm->debug_skip_line = 0;
    vm->debug_skip_depth = 0;
    vm_capture_debug_stop(vm, frame, VM_DEBUG_STOP_PAUSE, line);
    return true;
}

static bool vm_maybe_stop_for_step(VM* vm, CallFrame* frame) {
    if (!vm || !frame || vm->debug_exec_mode == VM_DEBUG_EXEC_NONE) return false;
    if (!frame->function || !frame->function->source_file) return false;

    int line = vm_breakpoint_frame_line(frame);
    if (line <= 0) return false;

    if (vm->debug_exec_mode == VM_DEBUG_EXEC_STEP_OUT) {
        if (vm->frame_count >= vm->debug_step_depth) return false;
        vm->debug_exec_mode = VM_DEBUG_EXEC_NONE;
        vm_capture_debug_stop(vm, frame, VM_DEBUG_STOP_STEP, line);
        return true;
    }

    bool line_changed =
        !vm_paths_match_loose(frame->function->source_file, vm->debug_step_source_file) ||
        line != vm->debug_step_line;
    bool depth_changed = vm->frame_count != vm->debug_step_depth;

    if (vm->debug_exec_mode == VM_DEBUG_EXEC_STEP_IN) {
        if (!line_changed && !depth_changed) return false;
    } else if (vm->debug_exec_mode == VM_DEBUG_EXEC_STEP_OVER) {
        if (vm->frame_count > vm->debug_step_depth) return false;
        if (!line_changed && vm->frame_count == vm->debug_step_depth) return false;
    } else {
        return false;
    }

    vm->debug_exec_mode = VM_DEBUG_EXEC_NONE;
    vm_capture_debug_stop(vm, frame, VM_DEBUG_STOP_STEP, line);
    return true;
}

static bool vm_maybe_stop_at_breakpoint(VM* vm, CallFrame* frame) {
    if (!vm || !frame || vm->debug_breakpoint_count <= 0) return false;
    if (!frame->function || !frame->function->source_file) return false;

    int line = vm_breakpoint_frame_line(frame);
    if (line <= 0) return false;

    bool matched = false;
    for (int i = 0; i < vm->debug_breakpoint_count; i++) {
        VmLineBreakpoint* bp = &vm->debug_breakpoints[i];
        if (bp->line != line) continue;
        if (!vm_paths_match_loose(frame->function->source_file, bp->source_file)) continue;
        matched = true;
        break;
    }
    if (!matched) return false;

    vm_capture_debug_stop(vm, frame, VM_DEBUG_STOP_BREAKPOINT, line);
    return true;
}

static bool vm_maybe_stop_at_runtime_error(VM* vm, CallFrame* frame) {
    int line = 0;

    if (!vm || !frame || !vm->debug_break_on_runtime_error || !vm->error_occurred) return false;
    if (vm->debug_stop.kind != VM_DEBUG_STOP_NONE) return false;

    line = vm_trace_frame_line(frame);
    if (line <= 0) {
        line = vm_breakpoint_frame_line(frame);
    }

    vm->debug_exec_mode = VM_DEBUG_EXEC_NONE;
    vm_capture_debug_stop(vm, frame, VM_DEBUG_STOP_EXCEPTION, line);
    return true;
}

static char* vm_build_runtime_error_message(VM* vm, const char* message) {
    const char* msg = (message && message[0] != '\0') ? message : "Runtime error";
    if (!vm || !vm->frames || vm->frame_count <= 0) {
        return safe_strdup(msg);
    }

    size_t cap = strlen(msg) + 64;
    for (int i = vm->frame_count - 1; i >= 0; i--) {
        CallFrame* frame = &vm->frames[i];
        const char* name = vm_trace_frame_name(frame, i);
        const char* source_file = (frame && frame->function) ? frame->function->source_file : NULL;
        cap += strlen(name) + 48;
        if (source_file && source_file[0] != '\0') {
            cap += strlen(source_file) + 8;
        }
    }

    char* out = (char*)safe_malloc(cap);
    size_t off = 0;
    vm_appendf(out, cap, &off, "%s\nStack trace:\n", msg);
    for (int i = vm->frame_count - 1; i >= 0; i--) {
        CallFrame* frame = &vm->frames[i];
        const char* name = vm_trace_frame_name(frame, i);
        const char* source_file = (frame && frame->function) ? frame->function->source_file : NULL;
        int line = vm_trace_frame_line(frame);
        if (source_file && source_file[0] != '\0' && line > 0) {
            vm_appendf(out, cap, &off, "  at %s (%s:%d)\n", name, source_file, line);
        } else if (line > 0) {
            vm_appendf(out, cap, &off, "  at %s (line %d)\n", name, line);
        } else if (source_file && source_file[0] != '\0') {
            vm_appendf(out, cap, &off, "  at %s (%s)\n", name, source_file);
        } else {
            vm_appendf(out, cap, &off, "  at %s\n", name);
        }
    }
    out[off] = '\0';
    return out;
}

void vm_runtime_error(VM* vm, const char* message) {
    if (!vm) return;
    vm->error_occurred = true;
    if (vm->error_message) free(vm->error_message);
    vm->error_message = vm_build_runtime_error_message(vm, message);
}

const char* vm_get_error(VM* vm) {
    if (!vm) return NULL;
    return vm->error_message;
}

#define VM_HASH_TABLE_MIN_CAPACITY 16
#define VM_HASH_TABLE_MAX_LOAD_NUM 7
#define VM_HASH_TABLE_MAX_LOAD_DEN 10

static uint32_t hash_table_hash_key(const char* key) {
    if (!key) return 0;
    uint64_t hash = 1469598103934665603ULL;
    const uint64_t prime = 1099511628211ULL;
    int length = 0;
    const unsigned char* p = (const unsigned char*)key;
    while (*p) {
        hash ^= (uint64_t)(*p);
        hash *= prime;
        p++;
        length++;
    }
    hash ^= (uint64_t)length;
    return hash_fold64_to32(hash);
}

static void hash_table_adjust_capacity(HashTable* table, int new_capacity) {
    if (!table || !table->entries) return;
    if (new_capacity < VM_HASH_TABLE_MIN_CAPACITY) return;
    if ((new_capacity & (new_capacity - 1)) != 0) return; // must be power-of-two

    HashEntry** old_entries = table->entries;
    int old_capacity = table->capacity;

    HashEntry** new_entries = (HashEntry**)safe_calloc((size_t)new_capacity, sizeof(HashEntry*));
    uint32_t mask = (uint32_t)(new_capacity - 1);

    for (int i = 0; i < old_capacity; i++) {
        HashEntry* entry = old_entries[i];
        if (!entry) continue;

        uint32_t idx = entry->hash & mask;
        while (new_entries[idx] != NULL) {
            idx = (idx + 1) & mask;
        }
        new_entries[idx] = entry;
    }

    free(old_entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
}

static void hash_table_ensure_capacity(HashTable* table, int additional) {
    if (!table || !table->entries || additional <= 0) return;

    int needed = table->count + additional;
    int max_load = (table->capacity * VM_HASH_TABLE_MAX_LOAD_NUM) / VM_HASH_TABLE_MAX_LOAD_DEN;
    if (max_load < 1) max_load = 1;
    if (needed <= max_load) return;

    int new_capacity = table->capacity;
    while (needed > ((new_capacity * VM_HASH_TABLE_MAX_LOAD_NUM) / VM_HASH_TABLE_MAX_LOAD_DEN)) {
        new_capacity *= 2;
    }
    hash_table_adjust_capacity(table, new_capacity);
}

static HashEntry** hash_table_find_slot(HashTable* table, const char* key, uint32_t hash, bool* out_found) {
    if (out_found) *out_found = false;
    if (!table || !table->entries || table->capacity <= 0 || !key) return NULL;

    uint32_t mask = (uint32_t)(table->capacity - 1);
    uint32_t idx = hash & mask;
    while (true) {
        HashEntry** slot = &table->entries[idx];
        HashEntry* entry = *slot;
        if (!entry) {
            if (out_found) *out_found = false;
            return slot;
        }
        if (entry->hash == hash && strcmp(entry->key, key) == 0) {
            if (out_found) *out_found = true;
            return slot;
        }
        idx = (idx + 1) & mask;
    }
}

void hash_table_init(HashTable* table) {
    table->capacity = VM_HASH_TABLE_MIN_CAPACITY;
    table->count = 0;
    table->entries = (HashEntry**)safe_calloc((size_t)table->capacity, sizeof(HashEntry*));
}

void hash_table_free(HashTable* table) {
    if (!table || !table->entries) return;
    for (int i = 0; i < table->capacity; i++) {
        HashEntry* entry = table->entries[i];
        if (!entry) continue;
        if (entry->key) free(entry->key);
        value_free(&entry->value);
        free(entry);
    }
    free(table->entries);
    table->entries = NULL;
    table->capacity = 0;
    table->count = 0;
}

void hash_table_set(HashTable* table, const char* key, Value value) {
    if (!table || !table->entries || !key) return;

    uint32_t hash = hash_table_hash_key(key);
    bool found = false;
    HashEntry** slot = hash_table_find_slot(table, key, hash, &found);
    if (!slot) return;

    if (found) {
        HashEntry* entry = *slot;
        value_free(&entry->value);
        entry->value = value;
        return;
    }

    int max_load = (table->capacity * VM_HASH_TABLE_MAX_LOAD_NUM) / VM_HASH_TABLE_MAX_LOAD_DEN;
    if (max_load < 1) max_load = 1;
    if ((table->count + 1) > max_load) {
        hash_table_ensure_capacity(table, 1);
        slot = hash_table_find_slot(table, key, hash, &found);
        if (!slot) return;
        if (found) {
            HashEntry* existing = *slot;
            value_free(&existing->value);
            existing->value = value;
            return;
        }
    }

    HashEntry* entry = (HashEntry*)safe_malloc(sizeof(HashEntry));
    entry->hash = hash;
    entry->global_slot = -1;
    entry->key = safe_strdup(key);
    entry->value = value;
    *slot = entry;
    table->count++;
}

static HashEntry* hash_table_get_entry(HashTable* table, const char* key) {
    if (!table || !key || !table->entries || table->capacity <= 0) return NULL;

    uint32_t hash = hash_table_hash_key(key);
    bool found = false;
    HashEntry** slot = hash_table_find_slot(table, key, hash, &found);
    if (!slot || !found) return NULL;
    return *slot;
}

Value hash_table_get(HashTable* table, const char* key) {
    HashEntry* entry = hash_table_get_entry(table, key);
    if (entry) {
        return entry->value;
    }

    Value nil;
    value_init_nil(&nil);
    return nil;
}

ObjString* vm_intern_string(VM* vm, const char* chars, int length) {
    Value existing = hash_table_get(&vm->string_pool, chars);
    if (value_get_type(&existing) == VAL_STRING) {
        obj_string_retain(value_get_string_obj(&existing));
        return value_get_string_obj(&existing);
    }

    ObjString* str = obj_string_create(chars, length);
    Value val;
    value_set_type(&val, VAL_STRING);
    value_set_string_obj(&val, str);
    hash_table_set(&vm->string_pool, chars, val);
    // The pool owns 1 reference; return an additional reference to the caller.
    obj_string_retain(str);
    return str;
}

void vm_set_global(VM* vm, const char* name, Value val) {
    if (!vm || !name) return;

    HashEntry* hashed_entry = hash_table_get_entry(&vm->globals_hash, name);
    if (hashed_entry &&
        hashed_entry->global_slot >= 0 &&
        hashed_entry->global_slot < vm->global_count &&
        vm->global_names &&
        vm->global_names[hashed_entry->global_slot] &&
        strcmp(vm->global_names[hashed_entry->global_slot], name) == 0) {
        int slot = hashed_entry->global_slot;
        vm->globals[slot] = val;
        value_free(&hashed_entry->value);
        hashed_entry->value = val;
        if (vm->globals_hash_entries) {
            vm->globals_hash_entries[slot] = hashed_entry;
        }
        return;
    }

    for (int i = 0; i < vm->global_count; i++) {
        if (strcmp(vm->global_names[i], name) == 0) {
            vm->globals[i] = val;
            HashEntry* entry = vm->globals_hash_entries ? vm->globals_hash_entries[i] : NULL;
            if (!entry || !entry->key || strcmp(entry->key, name) != 0) {
                entry = hashed_entry ? hashed_entry : hash_table_get_entry(&vm->globals_hash, name);
            }
            if (entry) {
                entry->global_slot = i;
                value_free(&entry->value);
                entry->value = val;
            } else {
                hash_table_set(&vm->globals_hash, name, val);
                entry = hash_table_get_entry(&vm->globals_hash, name);
                if (entry) {
                    entry->global_slot = i;
                }
            }
            if (vm->globals_hash_entries) {
                vm->globals_hash_entries[i] = entry;
            }
            return;
        }
    }

    vm->global_count++;
    if (vm->global_count > vm->global_capacity) {
        int old_capacity = vm->global_capacity;
        vm->global_capacity = vm->global_count * 2;
        vm->globals = (Value*)safe_realloc(vm->globals, vm->global_capacity * sizeof(Value));
        vm->global_names = (char**)safe_realloc(vm->global_names, vm->global_capacity * sizeof(char*));
        vm->globals_hash_entries = (HashEntry**)safe_realloc(vm->globals_hash_entries,
                                                            vm->global_capacity * sizeof(HashEntry*));
        for (int i = old_capacity; i < vm->global_capacity; i++) {
            vm->globals_hash_entries[i] = NULL;
        }
    }

    vm->global_names[vm->global_count - 1] = safe_strdup(name);
    vm->globals[vm->global_count - 1] = val;
    hash_table_set(&vm->globals_hash, name, val);
    HashEntry* inserted = hash_table_get_entry(&vm->globals_hash, name);
    if (inserted) {
        inserted->global_slot = vm->global_count - 1;
    }
    if (vm->globals_hash_entries) {
        vm->globals_hash_entries[vm->global_count - 1] = inserted;
    }
}

Value vm_get_global(VM* vm, const char* name) {
    return hash_table_get(&vm->globals_hash, name);
}

bool vm_take_return_value(VM* vm, Value* out) {
    if (!vm || !out || !vm->return_value) return false;
    *out = *(vm->return_value);
    free(vm->return_value);
    vm->return_value = NULL;
    return true;
}

static int vm_find_global_slot(VM* vm, const char* name) {
    if (!vm || !name) return -1;
    for (int i = 0; i < vm->global_count; i++) {
        if (vm->global_names[i] && strcmp(vm->global_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static int vm_resolve_global_slot_cached(VM* vm, ObjFunction* func, int name_idx) {
    if (!vm || !func) return -1;
    int const_count = func->constants.constant_count;
    if (name_idx < 0 || name_idx >= const_count) return -1;

    if (!func->global_slot_cache || func->global_slot_cache_count != const_count) {
        if (func->global_slot_cache) free(func->global_slot_cache);
        func->global_slot_cache = (int*)safe_malloc((size_t)const_count * sizeof(int));
        func->global_slot_cache_count = const_count;
        for (int i = 0; i < const_count; i++) {
            func->global_slot_cache[i] = -1;
        }
    }

    int cached = func->global_slot_cache[name_idx];
    if (cached >= 0 && cached < vm->global_count) {
        return cached;
    }

    Constant c = func->constants.constants[name_idx];
    if (c.type_index != 2 || !c.as_string) return -1;

    int slot = -1;
    HashEntry* entry = hash_table_get_entry(&vm->globals_hash, c.as_string);
    if (entry &&
        entry->global_slot >= 0 &&
        entry->global_slot < vm->global_count &&
        vm->global_names &&
        vm->global_names[entry->global_slot] &&
        strcmp(vm->global_names[entry->global_slot], c.as_string) == 0) {
        slot = entry->global_slot;
    } else {
        slot = vm_find_global_slot(vm, c.as_string);
        if (slot >= 0 && entry) {
            entry->global_slot = slot;
        }
    }
    if (slot < 0) return -1;
    func->global_slot_cache[name_idx] = slot;
    return slot;
}

static int vm_get_global_name_constant(VM* vm, ObjFunction* func, int name_idx, const char** out_name) {
    if (!vm || !func || !out_name) return -1;
    if (name_idx < 0 || name_idx >= func->constants.constant_count) {
        vm_runtime_error(vm, "Global name constant index out of bounds");
        return -1;
    }
    Constant c = func->constants.constants[name_idx];
    if (c.type_index != 2 || !c.as_string) {
        vm_runtime_error(vm, "Global name constant must be string");
        return -1;
    }
    *out_name = c.as_string;
    return 0;
}

static int vm_push_constant_by_index(VM* vm, ObjFunction* func, int index) {
    if (!vm || !func) return -1;
    if (index < 0 || index >= func->constants.constant_count) {
        vm_runtime_error(vm, "Constant index out of bounds");
        return -1;
    }

    Constant c = func->constants.constants[index];
    Value val;
    if (c.type_index == 0) {
        value_init_int(&val, c.as_int);
    } else if (c.type_index == 4) {
        value_init_bool(&val, c.as_int != 0);
    } else if (c.type_index == 1) {
        value_init_double(&val, c.as_double);
    } else if (c.type_index == 2) {
        if (!c.as_string) {
            vm_runtime_error(vm, "String constant payload missing");
            return -1;
        }
        size_t str_len = strlen(c.as_string);
        if (str_len > (size_t)INT_MAX) str_len = (size_t)INT_MAX;
        ObjString* interned = vm_intern_string(vm, c.as_string, (int)str_len);
        value_set_type(&val, VAL_STRING);
        value_set_string_obj(&val, interned);
    } else if (c.type_index == 3) {
        if (!c.as_string) {
            vm_runtime_error(vm, "BigInt constant payload missing");
            return -1;
        }
        ObjBigInt* bigint = obj_bigint_from_string(c.as_string);
        value_init_bigint(&val, bigint);
    } else {
        value_init_nil(&val);
    }

    push(vm, val);
    return 0;
}

static int vm_set_global_slot_value(VM* vm, uint8_t slot, Value val) {
    if (!vm) return -1;
    if (slot >= vm->global_count) {
        vm_runtime_error(vm, "Global slot out of bounds");
        value_free(&val);
        return -1;
    }

    vm->globals[slot] = val;

    HashEntry* entry = vm->globals_hash_entries ? vm->globals_hash_entries[slot] : NULL;
    if (entry) {
        value_free(&entry->value);
        entry->value = val;
        return 0;
    }

    if (vm->global_names && vm->global_names[slot]) {
        const char* name = vm->global_names[slot];
        hash_table_set(&vm->globals_hash, name, val);
        HashEntry* resolved = hash_table_get_entry(&vm->globals_hash, name);
        if (resolved) {
            resolved->global_slot = (int)slot;
        }
        if (vm->globals_hash_entries) {
            vm->globals_hash_entries[slot] = resolved;
        }
        return 0;
    }

    vm_runtime_error(vm, "Global slot entry missing");
    value_free(&val);
    return -1;
}

void vm_register_native(VM* vm, const char* name, void (*function)(VM* vm), int arity) {
    Value val;
    ObjNative* native = obj_native_create_builtin((void (*)(void*))(void*)function, arity);
    value_init_native(&val, native);
    vm_set_global(vm, name, val);
}

bool vm_register_native_extension(VM* vm, const char* name, void* userdata, int arity) {
    if (!vm || !name || !userdata) return false;
    Value val;
    ObjNative* native = obj_native_create_extension(userdata, arity);
    value_init_native(&val, native);
    vm_set_global(vm, name, val);
    return true;
}

void interface_dispatch_entries_free(InterfaceDispatchEntry* entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        if (entries[i].interface_name) free(entries[i].interface_name);
        if (entries[i].record_name) free(entries[i].record_name);
        if (entries[i].method_name) free(entries[i].method_name);
        if (entries[i].function_name) free(entries[i].function_name);
        entries[i].interface_name = NULL;
        entries[i].record_name = NULL;
        entries[i].method_name = NULL;
        entries[i].function_name = NULL;
    }
    free(entries);
}

static int vm_find_interface_dispatch_index(VM* vm,
                                            const char* interface_name,
                                            const char* record_name,
                                            const char* method_name) {
    if (!vm || !interface_name || !record_name || !method_name) return -1;
    for (int i = 0; i < vm->interface_dispatch_count; i++) {
        InterfaceDispatchEntry* entry = &vm->interface_dispatch_entries[i];
        if (!entry->interface_name || !entry->record_name || !entry->method_name) continue;
        if (strcmp(entry->interface_name, interface_name) == 0 &&
            strcmp(entry->record_name, record_name) == 0 &&
            strcmp(entry->method_name, method_name) == 0) {
            return i;
        }
    }
    return -1;
}

static size_t vm_record_base_name_len(const char* name) {
    if (!name) return 0;
    const char* bracket = strchr(name, '[');
    return bracket ? (size_t)(bracket - name) : strlen(name);
}

static bool vm_record_base_name_equals(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t a_len = vm_record_base_name_len(a);
    size_t b_len = vm_record_base_name_len(b);
    if (a_len != b_len) return false;
    return strncmp(a, b, a_len) == 0;
}

static int vm_find_interface_dispatch_base_fallback_index(VM* vm,
                                                          const char* interface_name,
                                                          const char* record_name,
                                                          const char* method_name) {
    if (!vm || !interface_name || !record_name || !method_name) return -1;
    for (int i = 0; i < vm->interface_dispatch_count; i++) {
        InterfaceDispatchEntry* entry = &vm->interface_dispatch_entries[i];
        if (!entry->interface_name || !entry->record_name || !entry->method_name) continue;
        if (strcmp(entry->interface_name, interface_name) != 0) continue;
        if (strcmp(entry->method_name, method_name) != 0) continue;
        if (strchr(entry->record_name, '[') != NULL) continue;
        if (vm_record_base_name_equals(entry->record_name, record_name)) {
            return i;
        }
    }
    return -1;
}

bool vm_register_interface_impl(VM* vm,
                                const char* interface_name,
                                const char* record_name,
                                const char* method_name,
                                const char* function_name) {
    if (!vm || !interface_name || !record_name || !method_name || !function_name) return false;
    if (interface_name[0] == '\0' || record_name[0] == '\0' || method_name[0] == '\0' || function_name[0] == '\0') {
        return false;
    }

    int existing = vm_find_interface_dispatch_index(vm, interface_name, record_name, method_name);
    if (existing >= 0) {
        InterfaceDispatchEntry* entry = &vm->interface_dispatch_entries[existing];
        if (entry->function_name) free(entry->function_name);
        entry->function_name = safe_strdup(function_name);
        if (vm->interface_dispatch_slot_cache && existing < vm->interface_dispatch_count) {
            vm->interface_dispatch_slot_cache[existing] = -1;
        }
        return true;
    }

    int new_count = vm->interface_dispatch_count + 1;
    if (new_count > vm->interface_dispatch_capacity) {
        int new_capacity = vm->interface_dispatch_capacity > 0 ? vm->interface_dispatch_capacity * 2 : 8;
        if (new_capacity < new_count) new_capacity = new_count;

        vm->interface_dispatch_entries = (InterfaceDispatchEntry*)safe_realloc(
            vm->interface_dispatch_entries,
            (size_t)new_capacity * sizeof(InterfaceDispatchEntry));

        for (int i = vm->interface_dispatch_capacity; i < new_capacity; i++) {
            vm->interface_dispatch_entries[i].interface_name = NULL;
            vm->interface_dispatch_entries[i].record_name = NULL;
            vm->interface_dispatch_entries[i].method_name = NULL;
            vm->interface_dispatch_entries[i].function_name = NULL;
        }

        vm->interface_dispatch_slot_cache = (int*)safe_realloc(vm->interface_dispatch_slot_cache,
                                                                (size_t)new_capacity * sizeof(int));
        for (int i = vm->interface_dispatch_capacity; i < new_capacity; i++) {
            vm->interface_dispatch_slot_cache[i] = -1;
        }

        vm->interface_dispatch_capacity = new_capacity;
    }

    InterfaceDispatchEntry* entry = &vm->interface_dispatch_entries[vm->interface_dispatch_count];
    entry->interface_name = safe_strdup(interface_name);
    entry->record_name = safe_strdup(record_name);
    entry->method_name = safe_strdup(method_name);
    entry->function_name = safe_strdup(function_name);
    vm->interface_dispatch_slot_cache[vm->interface_dispatch_count] = -1;
    vm->interface_dispatch_count = new_count;
    return true;
}

static const char* vm_resolve_interface_dispatch_function_name(VM* vm,
                                                               const char* interface_name,
                                                               const char* record_name,
                                                               const char* method_name,
                                                               int* out_dispatch_index) {
    if (out_dispatch_index) *out_dispatch_index = -1;
    if (!interface_name || !record_name || !method_name) return NULL;
    int idx = vm_find_interface_dispatch_index(vm, interface_name, record_name, method_name);
    if (idx < 0) {
        idx = vm_find_interface_dispatch_base_fallback_index(vm,
                                                             interface_name,
                                                             record_name,
                                                             method_name);
    }
    if (idx < 0) {
        return method_name;
    }
    InterfaceDispatchEntry* entry = &vm->interface_dispatch_entries[idx];
    if (!entry->function_name || entry->function_name[0] == '\0') {
        return method_name;
    }
    if (out_dispatch_index) *out_dispatch_index = idx;
    return entry->function_name;
}

static const char* vm_record_runtime_type_name(ObjRecord* record) {
    if (!record) return NULL;
    if (record->type_name && record->type_name[0] != '\0') {
        return record->type_name;
    }
    if (record->def && record->def->name && record->def->name[0] != '\0') {
        return record->def->name;
    }
    return NULL;
}

static bool vm_has_global_callable_named(VM* vm, const char* function_name) {
    if (!vm || !function_name || function_name[0] == '\0') return false;

    HashEntry* entry = hash_table_get_entry(&vm->globals_hash, function_name);
    if (!entry || entry->global_slot < 0 || entry->global_slot >= vm->global_count) {
        return false;
    }

    Value* global = &vm->globals[entry->global_slot];
    ValueType type = value_get_type(global);
    return type == VAL_FUNCTION || type == VAL_NATIVE;
}

static bool vm_record_supports_interface_method(VM* vm,
                                                const char* interface_name,
                                                const char* record_name,
                                                const char* method_name) {
    if (!vm || !interface_name || !record_name || record_name[0] == '\0') return false;
    if (!method_name || method_name[0] == '\0') return true;

    const char* function_name = vm_resolve_interface_dispatch_function_name(vm,
                                                                            interface_name,
                                                                            record_name,
                                                                            method_name,
                                                                            NULL);
    return vm_has_global_callable_named(vm, function_name);
}

bool vm_has_error(VM* vm) {
    return vm->error_occurred;
}

void vm_clear_error(VM* vm) {
    vm->error_occurred = false;
    if (vm->error_message) {
        free(vm->error_message);
        vm->error_message = NULL;
    }
}

static int vm_finish_async_panic_frame(VM* vm, CallFrame* frame, Chunk** io_chunk, uint8_t** io_code) {
    if (!vm || !frame || !frame->is_async_root) return -1;

    const char* panic_message = (frame->panic_message && frame->panic_message[0] != '\0')
        ? frame->panic_message
        : "Runtime error";

    callframe_free_defers(frame);
    stack_pop_to(vm, frame->slots_start);

    vm->frame_count--;
    vm->current_call_depth = vm->frame_count;

    ObjRecord* result_future = frame->async_result_future;
    frame->async_result_future = NULL;

    if (!result_future || !vm_future_complete_panic(vm, result_future, panic_message)) {
        if (result_future) obj_record_release(result_future);
        vm_runtime_error(vm, "Failed to resolve async future panic");
        return -1;
    }
    obj_record_release(result_future);

    if (frame->panic_message) {
        free(frame->panic_message);
        frame->panic_message = NULL;
    }
    frame->panic_unwinding = false;
    vm_clear_error(vm);

    if (vm->frame_count == 0) {
        if (io_chunk) *io_chunk = NULL;
        if (io_code) *io_code = NULL;
        return 0;
    }

    *frame = vm->frames[vm->frame_count - 1];
    if (io_chunk) *io_chunk = &frame->function->chunk;
    if (io_code) *io_code = frame->function->chunk.code;
    return 0;
}

static int vm_handle_async_frame_error(VM* vm, CallFrame* frame, Chunk** io_chunk, uint8_t** io_code) {
    if (!vm || !frame || !frame->is_async_root) return -1;

    if (!frame->panic_unwinding) {
        const char* message = vm->error_message && vm->error_message[0] != '\0'
            ? vm->error_message
            : "Runtime error";
        frame->panic_message = safe_strdup(message);
        frame->panic_unwinding = true;
    }

    vm_clear_error(vm);

    if (frame->function &&
        frame->function->defer_handler_ip >= 0 &&
        frame->function->defer_return_slot >= 0) {
        int slot_index = frame->slots_start + frame->function->defer_return_slot;
        if (slot_index < 0 || slot_index >= vm->stack.count) {
            vm_runtime_error(vm, "Async panic defer return slot out of bounds");
            return -1;
        }

        Value nil;
        value_init_nil(&nil);
        value_free(&vm->stack.values[slot_index]);
        vm->stack.values[slot_index] = nil;

        frame->ip = frame->function->defer_handler_ip;
        vm->frames[vm->frame_count - 1] = *frame;
        if (io_chunk) *io_chunk = &frame->function->chunk;
        if (io_code) *io_code = frame->function->chunk.code;
        return 0;
    }

    return vm_finish_async_panic_frame(vm, frame, io_chunk, io_code);
}

static int vm_run_loop(VM* vm, bool stop_on_frame_target, int stop_frame_count, int stop_stack_count);

int vm_execute(VM* vm, ObjFunction* func) {
    if (!vm || !func) return -1;
    vm_debug_clear_stop_info(vm);

    if (vm->return_value) {
        value_free(vm->return_value);
        free(vm->return_value);
        vm->return_value = NULL;
    }

    CallFrame frame;
    frame.function = func;
    frame.ip = 0;
    frame.slots_start = vm->stack.count;
    frame.slots_count = func->local_count;
    frame.defers = NULL;
    frame.defer_count = 0;
    frame.defer_capacity = 0;
    frame.is_async_root = false;
    frame.async_result_future = NULL;
    frame.panic_unwinding = false;
    frame.panic_message = NULL;

    for (int i = 0; i < func->local_count; i++) {
        Value nil;
        value_init_nil(&nil);
        push(vm, nil);
    }
    if (vm->error_occurred) {
        return -1;
    }
    frame.slots_start = vm->stack.count - func->local_count;
    if (!vm_bind_captures_to_frame(vm, &frame, func)) {
        return -1;
    }

    jit_record_function_entry(vm, func);

    vm_frame_reserve(vm, 1);
    vm->frames[vm->frame_count++] = frame;
    return vm_run_loop(vm, false, 0, 0);
}

int vm_resume(VM* vm) {
    if (!vm) return -1;
    vm_debug_clear_stop_info(vm);
    return vm_run_loop(vm, false, 0, 0);
}

int vm_call_value_sync(VM* vm,
                       const Value* callee,
                       const Value* args,
                       int arg_count,
                       Value* out_result) {
    Value callee_copy;
    int start_stack_count = 0;
    int start_frame_count = 0;
    bool handled = false;
    int rc = -1;

    if (!vm || !callee || !out_result) return -1;
    if (arg_count < 0) return -1;

    start_stack_count = vm->stack.count;
    start_frame_count = vm->frame_count;

    callee_copy = *callee;
    if (!value_type_is_unmanaged(value_get_type(&callee_copy))) {
        value_retain(&callee_copy);
    }
    push(vm, callee_copy);
    if (vm->error_occurred) {
        return -1;
    }

    for (int i = 0; i < arg_count; i++) {
        Value arg_copy = args[i];
        if (!value_type_is_unmanaged(value_get_type(&arg_copy))) {
            value_retain(&arg_copy);
        }
        push(vm, arg_copy);
        if (vm->error_occurred) {
            return -1;
        }
    }

    int callee_pos = start_stack_count;
    Value stacked_callee = vm->stack.values[callee_pos];
    if (value_get_type(&stacked_callee) == VAL_NATIVE) {
        ObjNative* native = value_get_native_obj(&stacked_callee);
        if (!native) {
            vm_runtime_error(vm, "Null native callback");
            return -1;
        }
        if (native->arity != arg_count) {
            vm_runtime_error(vm, "Wrong number of arguments");
            return -1;
        }

        for (int i = 0; i < arg_count; i++) {
            vm->stack.values[callee_pos + i] = vm->stack.values[callee_pos + 1 + i];
        }
        vm->stack.count--;

        if (arg_count == 0) {
            Value nil;
            value_init_nil(&nil);
            push(vm, nil);
            if (native->invoke(vm, native) != 0) {
                value_free(&stacked_callee);
                return -1;
            }
            value_free(&stacked_callee);
        } else {
            if (native->invoke(vm, native) != 0) {
                value_free(&stacked_callee);
                return -1;
            }

            int args_base = vm->stack.count - arg_count;
            for (int i = 1; i < arg_count; i++) {
                value_free(&vm->stack.values[args_base + i]);
            }
            vm->stack.count -= (arg_count - 1);
            value_free(&stacked_callee);
        }
    } else if (value_get_type(&stacked_callee) == VAL_FUNCTION) {
        ObjFunction* called_func = value_get_function_obj(&stacked_callee);
        CallFrame frame;
        Chunk* chunk = NULL;
        uint8_t* code = NULL;

        if (!called_func) {
            vm_runtime_error(vm, "Null function");
            return -1;
        }
        if (called_func->is_async) {
            vm_runtime_error(vm, "Native extensions cannot invoke async callbacks synchronously");
            return -1;
        }

        for (int i = 0; i < arg_count; i++) {
            vm->stack.values[callee_pos + i] = vm->stack.values[callee_pos + 1 + i];
        }
        vm->stack.count--;
        value_free(&stacked_callee);

        memset(&frame, 0, sizeof(frame));
        if (vm->frame_count > 0) {
            frame = vm->frames[vm->frame_count - 1];
        }
        jit_record_function_entry(vm, called_func);
        rc = vm_try_call_compiled_entry(vm,
                                        called_func,
                                        callee_pos,
                                        arg_count,
                                        &frame,
                                        &chunk,
                                        &code,
                                        &handled);
        if (rc != 0) {
            return -1;
        }

        if (!handled) {
            if (vm_enter_sync_function_from_call(vm,
                                                 called_func,
                                                 callee_pos,
                                                 arg_count,
                                                 &frame,
                                                 &chunk,
                                                 &code) != 0) {
                return -1;
            }

            rc = vm_run_loop(vm, true, start_frame_count, start_stack_count + 1);
            if (rc != 0) {
                return rc;
            }
        }
    } else {
        vm_runtime_error(vm, "Can only call functions");
        return -1;
    }

    if (vm->frame_count == start_frame_count &&
        vm->stack.count == start_stack_count &&
        vm->return_value) {
        *out_result = *(vm->return_value);
        free(vm->return_value);
        vm->return_value = NULL;
        return 0;
    }

    if (vm->frame_count != start_frame_count || vm->stack.count != start_stack_count + 1) {
        vm_runtime_error(vm, "Native callback invocation left the VM stack in an invalid state");
        return -1;
    }

    *out_result = pop(vm);
    return 0;
}

static int vm_run_loop(VM* vm, bool stop_on_frame_target, int stop_frame_count, int stop_stack_count) {
    if (!vm) return -1;

    CallFrame frame;
    memset(&frame, 0, sizeof(frame));
    Chunk* chunk = NULL;
    uint8_t* code = NULL;
    if (vm->frame_count > 0) {
        frame = vm->frames[vm->frame_count - 1];
        chunk = &frame.function->chunk;
        code = chunk->code;
    }

#define VM_ENSURE_IP_BYTES(byte_count)                                                      \
    do {                                                                                    \
        if (frame.ip < 0 ||                                                                 \
            (size_t)frame.ip + (size_t)(byte_count) > (size_t)chunk->code_count) {        \
            vm_runtime_error(vm, "Corrupted bytecode: truncated instruction stream");      \
            return -1;                                                                      \
        }                                                                                   \
    } while (0)

    for (;;) {
        if (stop_on_frame_target &&
            vm->frame_count == stop_frame_count &&
            vm->stack.count == stop_stack_count) {
            return 0;
        }

        if (vm->posted_event_auto_drain) {
            vm_process_posted_events(vm, -1);
            if (vm->error_occurred) {
                if (vm->frame_count > 0 && frame.is_async_root) {
                    if (vm_handle_async_frame_error(vm, &frame, &chunk, &code) != 0) {
                        return -1;
                    }
                    continue;
                }
                return -1;
            }
        }

        vm_process_poll_waiters(vm);
        vm_process_expired_timers(vm);

        if (vm->frame_count == 0) {
            int scheduled = vm_resume_ready_task(vm, NULL, &frame, &chunk, &code);
            if (scheduled < 0) {
                return -1;
            }
            if (scheduled > 0) {
                continue;
            }

            if (vm_wait_for_next_idle_work(vm)) {
                continue;
            }

            vm->return_value = (Value*)safe_malloc(sizeof(Value));
            value_init_nil(vm->return_value);
            return 0;
        }

        if (vm->error_occurred) {
            if (frame.is_async_root) {
                if (vm_handle_async_frame_error(vm, &frame, &chunk, &code) != 0) {
                    return -1;
                }
                continue;
            }
            if (vm_maybe_stop_at_runtime_error(vm, &frame)) {
                return 1;
            }
            return -1;
        }

        int scheduled = vm_resume_ready_task(vm, &frame, &frame, &chunk, &code);
        if (scheduled < 0) {
            return -1;
        }
        if (scheduled > 0) {
            continue;
        }

        if (vm_maybe_stop_on_entry(vm, &frame)) {
            return 1;
        }

        if (vm_maybe_stop_for_pause(vm, &frame)) {
            return 1;
        }

        if (vm_should_skip_current_debug_location(vm, &frame)) {
            goto vm_debug_continue_execution;
        }

        if (vm_maybe_stop_for_step(vm, &frame)) {
            return 1;
        }

        if (vm_maybe_stop_at_breakpoint(vm, &frame)) {
            return 1;
        }

vm_debug_continue_execution:
        if (vm->config.max_instructions > 0) {
            vm->instruction_count++;
            if (vm->instruction_count > vm->config.max_instructions) {
                vm_runtime_error(vm, "Execution limit exceeded");
                return -1;
            }
        }

        VM_ENSURE_IP_BYTES(1);
        uint8_t instruction = code[frame.ip++];
        if (vm->profile_opcodes) {
            vm->opcode_counts[instruction]++;
        }

        switch (instruction) {
            case OP_NOP:
                break;

            case OP_POP: {
                Value val = pop(vm);
                value_free(&val);
                break;
            }

            case OP_DUP: {
                if (vm->stack.count < 1) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value val = vm->stack.values[vm->stack.count - 1];
                if (!value_type_is_unmanaged(value_get_type(&val))) {
                    value_retain(&val);
                }
                push(vm, val);
                break;
            }

            case OP_CONST: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t index = code[frame.ip++];
                // 0xff is the nil sentinel
                if (index == 0xff) {
                    Value val;
                    value_init_nil(&val);
                    push(vm, val);
                    break;
                }
                if (vm_push_constant_by_index(vm, frame.function, (int)index) != 0) {
                    return -1;
                }
                break;
            }

            case OP_CONST16: {
                VM_ENSURE_IP_BYTES(2);
                int index = ((int)code[frame.ip] << 8) | (int)code[frame.ip + 1];
                frame.ip += 2;
                if (vm_push_constant_by_index(vm, frame.function, index) != 0) {
                    return -1;
                }
                break;
            }

            case OP_LOAD_LOCAL: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t slot = code[frame.ip++];
                Value val = vm->stack.values[frame.slots_start + slot];
                if (!value_type_is_unmanaged(value_get_type(&val))) {
                    value_retain(&val);
                }
                push(vm, val);
                break;
            }

            case OP_STORE_LOCAL: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t slot = code[frame.ip++];
                Value val = pop(vm);
                Value* dest = &vm->stack.values[frame.slots_start + slot];
                if (!value_type_is_unmanaged(value_get_type(dest))) {
                    value_free(dest);
                }
                *dest = val;
                break;
            }

            case OP_ADD_LOCAL_STACK_INT:
            case OP_SUB_LOCAL_STACK_INT:
            case OP_ADD_LOCAL_STACK_DOUBLE:
            case OP_SUB_LOCAL_STACK_DOUBLE: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t slot = code[frame.ip++];

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* rhs = &vm->stack.values[vm->stack.count - 1];
                Value* dst = &vm->stack.values[frame.slots_start + slot];

                bool is_double = (instruction == OP_ADD_LOCAL_STACK_DOUBLE || instruction == OP_SUB_LOCAL_STACK_DOUBLE);
                ValueType expected = is_double ? VAL_DOUBLE : VAL_INT;
                if (value_get_type(rhs) != expected || value_get_type(dst) != expected) {
                    vm_runtime_error(vm, is_double ? "Operands must be double" : "Operands must be integer");
                    if (!value_type_is_unmanaged(value_get_type(rhs))) {
                        value_free(rhs);
                    }
                    vm->stack.count--;
                    return -1;
                }

                if (is_double) {
                    if (instruction == OP_ADD_LOCAL_STACK_DOUBLE) {
                        value_set_double(dst, value_get_double(dst) + value_get_double(rhs));
                    } else {
                        value_set_double(dst, value_get_double(dst) - value_get_double(rhs));
                    }
                } else {
                    if (instruction == OP_ADD_LOCAL_STACK_INT) {
                        value_set_int(dst, value_get_int(dst) + value_get_int(rhs));
                    } else {
                        value_set_int(dst, value_get_int(dst) - value_get_int(rhs));
                    }
                }

                vm->stack.count--;
                break;
            }

            case OP_ADD_LOCAL_CONST: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t slot = code[frame.ip++];
                uint8_t const_idx = code[frame.ip++];
                if (const_idx >= frame.function->constants.constant_count) {
                    vm_runtime_error(vm, "Constant index out of bounds");
                    return -1;
                }

                Constant c = frame.function->constants.constants[const_idx];
                Value* local = &vm->stack.values[frame.slots_start + slot];
                if (value_get_type(local) == VAL_INT && c.type_index == 0) {
                    value_set_int(local, value_get_int(local) + c.as_int);
                    break;
                }
                if (value_get_type(local) == VAL_DOUBLE && c.type_index == 1) {
                    value_set_double(local, value_get_double(local) + c.as_double);
                    break;
                }

                vm_runtime_error(vm, "Operands must be numbers");
                return -1;
            }

            case OP_SUB_LOCAL_CONST: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t slot = code[frame.ip++];
                uint8_t const_idx = code[frame.ip++];
                if (const_idx >= frame.function->constants.constant_count) {
                    vm_runtime_error(vm, "Constant index out of bounds");
                    return -1;
                }

                Constant c = frame.function->constants.constants[const_idx];
                Value* local = &vm->stack.values[frame.slots_start + slot];
                if (value_get_type(local) == VAL_INT && c.type_index == 0) {
                    value_set_int(local, value_get_int(local) - c.as_int);
                    break;
                }
                if (value_get_type(local) == VAL_DOUBLE && c.type_index == 1) {
                    value_set_double(local, value_get_double(local) - c.as_double);
                    break;
                }

                vm_runtime_error(vm, "Operands must be numbers");
                return -1;
            }

            case OP_ADD2_LOCAL_CONST: {
                VM_ENSURE_IP_BYTES(5);
                uint8_t a_slot = code[frame.ip++];
                uint8_t c1_idx = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                uint8_t c2_idx = code[frame.ip++];
                frame.ip++; // padding (length-preserving superinstruction)

                if (c1_idx >= frame.function->constants.constant_count || c2_idx >= frame.function->constants.constant_count) {
                    vm_runtime_error(vm, "Constant index out of bounds");
                    return -1;
                }

                Constant c1 = frame.function->constants.constants[c1_idx];
                Constant c2 = frame.function->constants.constants[c2_idx];

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                if (value_get_type(a) == VAL_INT && c1.type_index == 0) {
                    value_set_int(a, value_get_int(a) + c1.as_int);
                } else if (value_get_type(a) == VAL_DOUBLE && c1.type_index == 1) {
                    value_set_double(a, value_get_double(a) + c1.as_double);
                } else {
                    vm_runtime_error(vm, "Operands must be numbers");
                    return -1;
                }

                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(b) == VAL_INT && c2.type_index == 0) {
                    value_set_int(b, value_get_int(b) + c2.as_int);
                } else if (value_get_type(b) == VAL_DOUBLE && c2.type_index == 1) {
                    value_set_double(b, value_get_double(b) + c2.as_double);
                } else {
                    vm_runtime_error(vm, "Operands must be numbers");
                    return -1;
                }

                break;
            }

            case OP_NEGATE_LOCAL: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t slot = code[frame.ip++];
                Value* local = &vm->stack.values[frame.slots_start + slot];
                if (value_get_type(local) == VAL_INT) {
                    value_set_int(local, -value_get_int(local));
                    break;
                }
                if (value_get_type(local) == VAL_DOUBLE) {
                    value_set_double(local, -value_get_double(local));
                    break;
                }

                vm_runtime_error(vm, "Operand must be a number");
                return -1;
            }

            case OP_ADD_LOCAL_DIV_LOCALS: {
                VM_ENSURE_IP_BYTES(3);
                uint8_t dst_slot = code[frame.ip++];
                uint8_t num_slot = code[frame.ip++];
                uint8_t den_slot = code[frame.ip++];

                Value* dst = &vm->stack.values[frame.slots_start + dst_slot];
                Value* num = &vm->stack.values[frame.slots_start + num_slot];
                Value* den = &vm->stack.values[frame.slots_start + den_slot];

                if (value_get_type(dst) != VAL_DOUBLE || value_get_type(num) != VAL_DOUBLE || value_get_type(den) != VAL_DOUBLE) {
                    vm_runtime_error(vm, "Operands must be double");
                    return -1;
                }
                if (value_get_double(den) == 0.0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }

                value_set_double(dst, value_get_double(dst) + (value_get_double(num) / value_get_double(den)));
                break;
            }

            case OP_LOAD_GLOBAL: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t name_idx = code[frame.ip++];
                Value val;
                int slot = vm_resolve_global_slot_cached(vm, frame.function, name_idx);
                if (slot >= 0 && slot <= 0xff) {
                    // Inline cache: rewrite this load to use the resolved global slot directly.
                    // opcode at (ip-2), operand at (ip-1).
                    code[frame.ip - 2] = OP_LOAD_GLOBAL_SLOT;
                    code[frame.ip - 1] = (uint8_t)slot;
                }

                if (slot >= 0 && slot < vm->global_count) {
                    val = vm->globals[slot];
                } else {
                    const char* global_name = NULL;
                    if (vm_get_global_name_constant(vm, frame.function, name_idx, &global_name) != 0) {
                        return -1;
                    }
                    val = vm_get_global(vm, global_name);
                }
                if (!value_type_is_unmanaged(value_get_type(&val))) {
                    value_retain(&val);
                }
                push(vm, val);
                break;
            }

            case OP_LOAD_GLOBAL16: {
                VM_ENSURE_IP_BYTES(2);
                int name_idx = ((int)code[frame.ip] << 8) | (int)code[frame.ip + 1];
                frame.ip += 2;

                Value val;
                int slot = vm_resolve_global_slot_cached(vm, frame.function, name_idx);
                if (slot >= 0 && slot < vm->global_count) {
                    val = vm->globals[slot];
                } else {
                    const char* global_name = NULL;
                    if (vm_get_global_name_constant(vm, frame.function, name_idx, &global_name) != 0) {
                        return -1;
                    }
                    val = vm_get_global(vm, global_name);
                }
                if (!value_type_is_unmanaged(value_get_type(&val))) {
                    value_retain(&val);
                }
                push(vm, val);
                break;
            }

            case OP_LOAD_GLOBAL_SLOT: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t slot = code[frame.ip++];
                if (slot >= vm->global_count) {
                    vm_runtime_error(vm, "Global slot out of bounds");
                    return -1;
                }
                Value val = vm->globals[slot];
                if (!value_type_is_unmanaged(value_get_type(&val))) {
                    value_retain(&val);
                }
                push(vm, val);
                break;
            }

            case OP_STORE_GLOBAL: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t name_idx = code[frame.ip++];
                Value val = pop(vm);
                const char* global_name = NULL;
                if (vm_get_global_name_constant(vm, frame.function, name_idx, &global_name) != 0) {
                    value_free(&val);
                    return -1;
                }
                int slot = vm_resolve_global_slot_cached(vm, frame.function, name_idx);
                if (slot >= 0 && slot <= 0xff) {
                    // Inline cache: rewrite this store to use the resolved global slot directly.
                    // opcode at (ip-2), operand at (ip-1).
                    code[frame.ip - 2] = OP_STORE_GLOBAL_SLOT;
                    code[frame.ip - 1] = (uint8_t)slot;
                }
                if (slot >= 0 && slot < vm->global_count) {
                    vm->globals[slot] = val;
                    HashEntry* entry = vm->globals_hash_entries ? vm->globals_hash_entries[slot] : NULL;
                    if (!entry) {
                        entry = hash_table_get_entry(&vm->globals_hash, global_name);
                        if (vm->globals_hash_entries) {
                            vm->globals_hash_entries[slot] = entry;
                        }
                    }
                    if (entry) {
                        value_free(&entry->value);
                        entry->value = val;
                    } else {
                        hash_table_set(&vm->globals_hash, global_name, val);
                        entry = hash_table_get_entry(&vm->globals_hash, global_name);
                        if (entry) {
                            entry->global_slot = slot;
                        }
                        if (vm->globals_hash_entries) {
                            vm->globals_hash_entries[slot] = entry;
                        }
                    }
                } else {
                    vm_set_global(vm, global_name, val);
                }
                break;
            }

            case OP_STORE_GLOBAL16: {
                VM_ENSURE_IP_BYTES(2);
                int name_idx = ((int)code[frame.ip] << 8) | (int)code[frame.ip + 1];
                frame.ip += 2;

                Value val = pop(vm);
                const char* global_name = NULL;
                if (vm_get_global_name_constant(vm, frame.function, name_idx, &global_name) != 0) {
                    value_free(&val);
                    return -1;
                }

                int slot = vm_resolve_global_slot_cached(vm, frame.function, name_idx);
                if (slot >= 0 && slot < vm->global_count) {
                    vm->globals[slot] = val;
                    HashEntry* entry = vm->globals_hash_entries ? vm->globals_hash_entries[slot] : NULL;
                    if (!entry) {
                        entry = hash_table_get_entry(&vm->globals_hash, global_name);
                        if (vm->globals_hash_entries) {
                            vm->globals_hash_entries[slot] = entry;
                        }
                    }
                    if (entry) {
                        value_free(&entry->value);
                        entry->value = val;
                    } else {
                        hash_table_set(&vm->globals_hash, global_name, val);
                        entry = hash_table_get_entry(&vm->globals_hash, global_name);
                        if (entry) {
                            entry->global_slot = slot;
                        }
                        if (vm->globals_hash_entries) {
                            vm->globals_hash_entries[slot] = entry;
                        }
                    }
                } else {
                    vm_set_global(vm, global_name, val);
                }
                break;
            }

            case OP_STORE_GLOBAL_SLOT: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t slot = code[frame.ip++];
                Value val = pop(vm);
                if (slot >= vm->global_count) {
                    vm_runtime_error(vm, "Global slot out of bounds");
                    value_free(&val);
                    return -1;
                }

                vm->globals[slot] = val;

                HashEntry* entry = vm->globals_hash_entries ? vm->globals_hash_entries[slot] : NULL;
                if (entry) {
                    value_free(&entry->value);
                    entry->value = val;
                } else if (vm->global_names && vm->global_names[slot]) {
                    const char* name = vm->global_names[slot];
                    hash_table_set(&vm->globals_hash, name, val);
                    entry = hash_table_get_entry(&vm->globals_hash, name);
                    if (entry) {
                        entry->global_slot = slot;
                    }
                    if (vm->globals_hash_entries) {
                        vm->globals_hash_entries[slot] = entry;
                    }
                } else {
                    vm_runtime_error(vm, "Global slot entry missing");
                    value_free(&val);
                    return -1;
                }

                // Fast path: OP_STORE_GLOBAL_SLOT s; OP_LOAD_GLOBAL_SLOT s
                // Keep the stored value on the stack and skip the redundant load.
                if ((frame.ip + 1) < chunk->code_count &&
                    code[frame.ip] == OP_LOAD_GLOBAL_SLOT &&
                    code[frame.ip + 1] == slot) {
                    Value pushed = val;
                    if (!value_type_is_unmanaged(value_get_type(&pushed))) {
                        value_retain(&pushed);
                    }
                    push(vm, pushed);
                    frame.ip += 2;
                }

                break;
            }

            case OP_ADD_GLOBAL_GLOBAL_TO_GLOBAL:
            case OP_SUB_GLOBAL_GLOBAL_TO_GLOBAL:
            case OP_MUL_GLOBAL_GLOBAL_TO_GLOBAL:
            case OP_DIV_GLOBAL_GLOBAL_TO_GLOBAL: {
                VM_ENSURE_IP_BYTES(6);
                uint8_t dst_name_idx = code[frame.ip++];
                uint8_t a_name_idx = code[frame.ip++];
                uint8_t b_name_idx = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 3;

                OpCode base_op = OP_NOP;
                uint8_t slot_op = 0;
                switch (instruction) {
                    case OP_ADD_GLOBAL_GLOBAL_TO_GLOBAL:
                        base_op = OP_ADD;
                        slot_op = OP_ADD_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT;
                        break;
                    case OP_SUB_GLOBAL_GLOBAL_TO_GLOBAL:
                        base_op = OP_SUB;
                        slot_op = OP_SUB_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT;
                        break;
                    case OP_MUL_GLOBAL_GLOBAL_TO_GLOBAL:
                        base_op = OP_MUL;
                        slot_op = OP_MUL_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT;
                        break;
                    case OP_DIV_GLOBAL_GLOBAL_TO_GLOBAL:
                        base_op = OP_DIV;
                        slot_op = OP_DIV_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT;
                        break;
                    default:
                        vm_runtime_error(vm, "Invalid global arithmetic opcode");
                        return -1;
                }

                int dst_slot = vm_resolve_global_slot_cached(vm, frame.function, dst_name_idx);
                int a_slot = vm_resolve_global_slot_cached(vm, frame.function, a_name_idx);
                int b_slot = vm_resolve_global_slot_cached(vm, frame.function, b_name_idx);

                if (dst_slot >= 0 && dst_slot <= 0xff &&
                    a_slot >= 0 && a_slot <= 0xff &&
                    b_slot >= 0 && b_slot <= 0xff &&
                    (uint8_t)dst_slot < vm->global_count &&
                    (uint8_t)a_slot < vm->global_count &&
                    (uint8_t)b_slot < vm->global_count) {

                    // Inline cache: rewrite to slot-based superinstruction.
                    if (slot_op != 0) {
                        code[frame.ip - 7] = slot_op;
                        code[frame.ip - 6] = (uint8_t)dst_slot;
                        code[frame.ip - 5] = (uint8_t)a_slot;
                        code[frame.ip - 4] = (uint8_t)b_slot;
                        code[frame.ip - 3] = 0;
                        code[frame.ip - 2] = 0;
                        code[frame.ip - 1] = 0;
                    }

                    Value a = vm->globals[(uint8_t)a_slot];
                    Value b = vm->globals[(uint8_t)b_slot];
                    Value result;
                    if (vm_compute_arithmetic_op(vm, base_op, &a, &b, &result) != 0) {
                        return -1;
                    }

                    if (vm_set_global_slot_value(vm, (uint8_t)dst_slot, result) != 0) {
                        return -1;
                    }

                    break;
                }

                // Fallback: resolve by name (rare / global slot can't be cached).
                if (dst_name_idx >= frame.function->constants.constant_count ||
                    a_name_idx >= frame.function->constants.constant_count ||
                    b_name_idx >= frame.function->constants.constant_count) {
                    vm_runtime_error(vm, "Constant index out of bounds");
                    return -1;
                }

                Constant dst_c = frame.function->constants.constants[dst_name_idx];
                Constant a_c = frame.function->constants.constants[a_name_idx];
                Constant b_c = frame.function->constants.constants[b_name_idx];
                if (!dst_c.as_string || !a_c.as_string || !b_c.as_string) {
                    vm_runtime_error(vm, "Invalid global name constant");
                    return -1;
                }

                Value a = vm_get_global(vm, a_c.as_string);
                Value b = vm_get_global(vm, b_c.as_string);
                Value result;
                if (vm_compute_arithmetic_op(vm, base_op, &a, &b, &result) != 0) {
                    return -1;
                }

                vm_set_global(vm, dst_c.as_string, result);
                break;
            }

            case OP_ADD_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
            case OP_SUB_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
            case OP_MUL_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
            case OP_DIV_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT: {
                VM_ENSURE_IP_BYTES(6);
                uint8_t dst_slot = code[frame.ip++];
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 3;

                if (dst_slot >= vm->global_count ||
                    a_slot >= vm->global_count ||
                    b_slot >= vm->global_count) {
                    vm_runtime_error(vm, "Global slot out of bounds");
                    return -1;
                }

                OpCode base_op = OP_NOP;
                switch (instruction) {
                    case OP_ADD_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT: base_op = OP_ADD; break;
                    case OP_SUB_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT: base_op = OP_SUB; break;
                    case OP_MUL_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT: base_op = OP_MUL; break;
                    case OP_DIV_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT: base_op = OP_DIV; break;
                    default:
                        vm_runtime_error(vm, "Invalid global arithmetic opcode");
                        return -1;
                }

                // In-place bigint fast path: global[dst] = global[dst] * small_int
                // This avoids allocating a new bigint object/limb buffer on hot update patterns.
                if (instruction == OP_MUL_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT && dst_slot == a_slot) {
                    Value* dst = &vm->globals[dst_slot];
                    ObjBigInt* dst_bigint = value_get_bigint_obj(dst);
                    if (value_get_type(dst) == VAL_BIGINT && dst_bigint &&
                        dst_bigint->ref_count == 1) {

                        int mult_sign = 1;
                        uint32_t mult_u32 = 0;
                        bool can_inplace = false;

                        Value mult_val = vm->globals[b_slot];
                        if (value_get_type(&mult_val) == VAL_BIGINT && value_get_bigint_obj(&mult_val)) {
                            ObjBigInt* m = value_get_bigint_obj(&mult_val);
                            if (m->sign == 0 || m->count == 0) {
                                mult_sign = 0;
                                mult_u32 = 0;
                                can_inplace = true;
                            } else if (m->count == 1) {
                                mult_sign = m->sign;
                                mult_u32 = m->limbs[0];
                                can_inplace = true;
                            }
                        } else if (value_get_type(&mult_val) == VAL_INT) {
                            int64_t m = value_get_int(&mult_val);
                            uint64_t abs_val = (m < 0) ? (uint64_t)(-(m + 1)) + 1 : (uint64_t)m;
                            if (abs_val <= (uint64_t)UINT32_MAX) {
                                mult_u32 = (uint32_t)abs_val;
                                mult_sign = (abs_val == 0) ? 0 : (m < 0 ? -1 : 1);
                                can_inplace = true;
                            }
                        }

                        if (can_inplace) {
                            if (mult_sign == 0 || mult_u32 == 0) {
                                ObjBigInt* x = dst_bigint;
                                if (x->limbs) free(x->limbs);
                                x->limbs = NULL;
                                x->count = 0;
                                x->sign = 0;
                            } else {
                                obj_bigint_mul_small_inplace(dst_bigint, mult_u32);
                                if (mult_sign < 0 && dst_bigint->sign != 0) {
                                    dst_bigint->sign = -dst_bigint->sign;
                                }
                            }
                            break;
                        }
                    }
                }

                Value a = vm->globals[a_slot];
                Value b = vm->globals[b_slot];
                Value result;
                if (vm_compute_arithmetic_op(vm, base_op, &a, &b, &result) != 0) {
                    return -1;
                }

                if (vm_set_global_slot_value(vm, dst_slot, result) != 0) {
                    return -1;
                }

                break;
            }

            case OP_ADD_INT: {
                if (vm->stack.count < 2) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* b = &vm->stack.values[vm->stack.count - 1];
                Value* a = &vm->stack.values[vm->stack.count - 2];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    value_free(a);
                    value_free(b);
                    vm->stack.count -= 2;
                    return -1;
                }

                value_set_int(a, value_get_int(a) + value_get_int(b));
                vm->stack.count--;
                break;
            }

            case OP_SUB_INT: {
                if (vm->stack.count < 2) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* b = &vm->stack.values[vm->stack.count - 1];
                Value* a = &vm->stack.values[vm->stack.count - 2];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    value_free(a);
                    value_free(b);
                    vm->stack.count -= 2;
                    return -1;
                }

                value_set_int(a, value_get_int(a) - value_get_int(b));
                vm->stack.count--;
                break;
            }

            case OP_MUL_INT: {
                if (vm->stack.count < 2) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* b = &vm->stack.values[vm->stack.count - 1];
                Value* a = &vm->stack.values[vm->stack.count - 2];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    value_free(a);
                    value_free(b);
                    vm->stack.count -= 2;
                    return -1;
                }

                value_set_int(a, value_get_int(a) * value_get_int(b));
                vm->stack.count--;
                break;
            }

            case OP_DIV_INT: {
                if (vm->stack.count < 2) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* b = &vm->stack.values[vm->stack.count - 1];
                Value* a = &vm->stack.values[vm->stack.count - 2];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    value_free(a);
                    value_free(b);
                    vm->stack.count -= 2;
                    return -1;
                }
                if (value_get_int(b) == 0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }

                value_set_int(a, value_get_int(a) / value_get_int(b));
                vm->stack.count--;
                break;
            }

            case OP_MOD_INT: {
                if (vm->stack.count < 2) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* b = &vm->stack.values[vm->stack.count - 1];
                Value* a = &vm->stack.values[vm->stack.count - 2];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    value_free(a);
                    value_free(b);
                    vm->stack.count -= 2;
                    return -1;
                }
                if (value_get_int(b) == 0) {
                    vm_runtime_error(vm, "Modulo by zero");
                    return -1;
                }

                value_set_int(a, value_get_int(a) % value_get_int(b));
                vm->stack.count--;
                break;
            }

            case OP_NEG_INT: {
                if (vm->stack.count < 1) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                if (value_get_type(a) != VAL_INT) {
                    vm_runtime_error(vm, "Operand must be integer");
                    value_free(a);
                    vm->stack.count--;
                    return -1;
                }

                value_set_int(a, -value_get_int(a));
                break;
            }

            case OP_BIT_AND_INT: {
                if (vm->stack.count < 2) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* b = &vm->stack.values[vm->stack.count - 1];
                Value* a = &vm->stack.values[vm->stack.count - 2];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    value_free(a);
                    value_free(b);
                    vm->stack.count -= 2;
                    return -1;
                }

                value_set_int(a, value_get_int(a) & value_get_int(b));
                vm->stack.count--;
                break;
            }

            case OP_BIT_OR_INT: {
                if (vm->stack.count < 2) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* b = &vm->stack.values[vm->stack.count - 1];
                Value* a = &vm->stack.values[vm->stack.count - 2];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    value_free(a);
                    value_free(b);
                    vm->stack.count -= 2;
                    return -1;
                }

                value_set_int(a, value_get_int(a) | value_get_int(b));
                vm->stack.count--;
                break;
            }

            case OP_BIT_XOR_INT: {
                if (vm->stack.count < 2) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* b = &vm->stack.values[vm->stack.count - 1];
                Value* a = &vm->stack.values[vm->stack.count - 2];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    value_free(a);
                    value_free(b);
                    vm->stack.count -= 2;
                    return -1;
                }

                value_set_int(a, value_get_int(a) ^ value_get_int(b));
                vm->stack.count--;
                break;
            }

            case OP_BIT_NOT_INT: {
                if (vm->stack.count < 1) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                if (value_get_type(a) != VAL_INT) {
                    vm_runtime_error(vm, "Operand must be integer");
                    value_free(a);
                    vm->stack.count--;
                    return -1;
                }

                value_set_int(a, ~value_get_int(a));
                break;
            }

            case OP_ADD_DOUBLE: {
                if (vm->stack.count < 2) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* b = &vm->stack.values[vm->stack.count - 1];
                Value* a = &vm->stack.values[vm->stack.count - 2];
                if (value_get_type(a) != VAL_DOUBLE || value_get_type(b) != VAL_DOUBLE) {
                    vm_runtime_error(vm, "Operands must be double");
                    value_free(a);
                    value_free(b);
                    vm->stack.count -= 2;
                    return -1;
                }

                value_set_double(a, value_get_double(a) + value_get_double(b));
                vm->stack.count--;
                break;
            }

            case OP_SUB_DOUBLE: {
                if (vm->stack.count < 2) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* b = &vm->stack.values[vm->stack.count - 1];
                Value* a = &vm->stack.values[vm->stack.count - 2];
                if (value_get_type(a) != VAL_DOUBLE || value_get_type(b) != VAL_DOUBLE) {
                    vm_runtime_error(vm, "Operands must be double");
                    value_free(a);
                    value_free(b);
                    vm->stack.count -= 2;
                    return -1;
                }

                value_set_double(a, value_get_double(a) - value_get_double(b));
                vm->stack.count--;
                break;
            }

            case OP_MUL_DOUBLE: {
                if (vm->stack.count < 2) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* b = &vm->stack.values[vm->stack.count - 1];
                Value* a = &vm->stack.values[vm->stack.count - 2];
                if (value_get_type(a) != VAL_DOUBLE || value_get_type(b) != VAL_DOUBLE) {
                    vm_runtime_error(vm, "Operands must be double");
                    value_free(a);
                    value_free(b);
                    vm->stack.count -= 2;
                    return -1;
                }

                value_set_double(a, value_get_double(a) * value_get_double(b));
                vm->stack.count--;
                break;
            }

            case OP_DIV_DOUBLE: {
                if (vm->stack.count < 2) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* b = &vm->stack.values[vm->stack.count - 1];
                Value* a = &vm->stack.values[vm->stack.count - 2];
                if (value_get_type(a) != VAL_DOUBLE || value_get_type(b) != VAL_DOUBLE) {
                    vm_runtime_error(vm, "Operands must be double");
                    value_free(a);
                    value_free(b);
                    vm->stack.count -= 2;
                    return -1;
                }
                if (value_get_double(b) == 0.0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }

                value_set_double(a, value_get_double(a) / value_get_double(b));
                vm->stack.count--;
                break;
            }

            case OP_RECIP_INT_TO_DOUBLE: {
                if (vm->stack.count < 1) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                if (value_get_type(a) != VAL_INT) {
                    vm_runtime_error(vm, "Operand must be integer");
                    value_free(a);
                    vm->stack.count--;
                    return -1;
                }
                if (value_get_int(a) == 0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }

                double recip = 1.0 / (double)value_get_int(a);
                value_set_type(a, VAL_DOUBLE);
                value_set_double(a, recip);
                break;
            }

            case OP_EVALA_RECIP_LOCALS_DOUBLE: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                int64_t i = value_get_int(a);
                int64_t j = value_get_int(b);
                int64_t ij = i + j;
                int64_t t0 = ij;
                int64_t t1 = ij + 1;
                if ((t0 & 1) != 0) {
                    t1 /= 2;
                } else {
                    t0 /= 2;
                }
                int64_t tri = t0 * t1;
                int64_t denom = tri + i + 1;

                if (denom == 0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }

                Value result;
                value_set_type(&result, VAL_DOUBLE);
                value_set_double(&result, 1.0 / (double)denom);
                push(vm, result);
                break;
            }

            case OP_EVALA_MADD_LOCAL_ARRAY_LOCAL_DOUBLE: {
                VM_ENSURE_IP_BYTES(6);
                uint8_t acc_slot = code[frame.ip++];
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_slot = code[frame.ip++];
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                frame.ip++; // padding (length-preserving superinstruction)

                Value* acc = &vm->stack.values[frame.slots_start + acc_slot];
                if (value_get_type(acc) != VAL_DOUBLE) {
                    vm_runtime_error(vm, "Accumulator must be double");
                    return -1;
                }

                Value* idx_valp = &vm->stack.values[frame.slots_start + idx_slot];
                if (value_get_type(idx_valp) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                int64_t idx64 = value_get_int(idx_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                int idx = (int)idx64;
                double elem;
                if (arr->kind == ARRAY_KIND_DOUBLE) {
                    elem = arr->data.doubles[idx];
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    Value v = arr->data.elements[idx];
                    if (value_get_type(&v) != VAL_DOUBLE) {
                        vm_runtime_error(vm, "Array element must be double");
                        return -1;
                    }
                    elem = value_get_double(&v);
                } else {
                    vm_runtime_error(vm, "Array element must be double");
                    return -1;
                }

                int64_t i = value_get_int(a);
                int64_t j = value_get_int(b);
                int64_t ij = i + j;
                int64_t t0 = ij;
                int64_t t1 = ij + 1;
                if ((t0 & 1) != 0) {
                    t1 /= 2;
                } else {
                    t0 /= 2;
                }
                int64_t tri = t0 * t1;
                int64_t denom = tri + i + 1;

                if (denom == 0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }

                double factor = 1.0 / (double)denom;
                value_set_double(acc, value_get_double(acc) + (factor * elem));
                break;
            }

            case OP_ADD_LOCALS_DOUBLE:
            case OP_SUB_LOCALS_DOUBLE:
            case OP_MUL_LOCALS_DOUBLE:
            case OP_DIV_LOCALS_DOUBLE: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 2;

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_DOUBLE || value_get_type(b) != VAL_DOUBLE) {
                    vm_runtime_error(vm, "Operands must be double");
                    return -1;
                }

                if (instruction == OP_DIV_LOCALS_DOUBLE && value_get_double(b) == 0.0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }

                Value result;
                value_set_type(&result, VAL_DOUBLE);
                switch (instruction) {
                    case OP_ADD_LOCALS_DOUBLE:
                        value_set_double(&result, value_get_double(a) + value_get_double(b));
                        break;
                    case OP_SUB_LOCALS_DOUBLE:
                        value_set_double(&result, value_get_double(a) - value_get_double(b));
                        break;
                    case OP_MUL_LOCALS_DOUBLE:
                        value_set_double(&result, value_get_double(a) * value_get_double(b));
                        break;
                    case OP_DIV_LOCALS_DOUBLE:
                        value_set_double(&result, value_get_double(a) / value_get_double(b));
                        break;
                    default:
                        value_set_double(&result, 0.0);
                        break;
                }

                push(vm, result);
                break;
            }

            case OP_MUL_LOCALS_INT_TO_LOCAL:
            case OP_MUL_LOCALS_DOUBLE_TO_LOCAL: {
                VM_ENSURE_IP_BYTES(6);
                uint8_t dst_slot = code[frame.ip++];
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 3;

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];

                Value result;
                if (instruction == OP_MUL_LOCALS_DOUBLE_TO_LOCAL) {
                    if (value_get_type(a) != VAL_DOUBLE || value_get_type(b) != VAL_DOUBLE) {
                        vm_runtime_error(vm, "Operands must be double");
                        return -1;
                    }
                    value_set_type(&result, VAL_DOUBLE);
                    value_set_double(&result, value_get_double(a) * value_get_double(b));
                } else {
                    if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                        vm_runtime_error(vm, "Operands must be integer");
                        return -1;
                    }
                    value_set_type(&result, VAL_INT);
                    value_set_int(&result, value_get_int(a) * value_get_int(b));
                }

                Value* dst = &vm->stack.values[frame.slots_start + dst_slot];
                if (!value_type_is_unmanaged(value_get_type(dst))) {
                    value_free(dst);
                }
                *dst = result;
                break;
            }

            case OP_ADD_LOCALS_INT: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 2;

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                Value result;
                value_set_type(&result, VAL_INT);
                value_set_int(&result, value_get_int(a) + value_get_int(b));
                push(vm, result);
                break;
            }

            case OP_SUB_LOCALS_INT: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 2;

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                Value result;
                value_set_type(&result, VAL_INT);
                value_set_int(&result, value_get_int(a) - value_get_int(b));
                push(vm, result);
                break;
            }

            case OP_MUL_LOCALS_INT: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 2;

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                Value result;
                value_set_type(&result, VAL_INT);
                value_set_int(&result, value_get_int(a) * value_get_int(b));
                push(vm, result);
                break;
            }

            case OP_DIV_LOCALS_INT: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 2;

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }
                if (value_get_int(b) == 0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }

                Value result;
                value_set_type(&result, VAL_INT);
                value_set_int(&result, value_get_int(a) / value_get_int(b));
                push(vm, result);
                break;
            }

            case OP_MOD_LOCALS_INT: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 2;

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }
                if (value_get_int(b) == 0) {
                    vm_runtime_error(vm, "Modulo by zero");
                    return -1;
                }

                Value result;
                value_set_type(&result, VAL_INT);
                value_set_int(&result, value_get_int(a) % value_get_int(b));
                push(vm, result);
                break;
            }

            case OP_BIT_AND_LOCALS_INT: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 2;

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                Value result;
                value_set_type(&result, VAL_INT);
                value_set_int(&result, value_get_int(a) & value_get_int(b));
                push(vm, result);
                break;
            }

            case OP_BIT_OR_LOCALS_INT: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 2;

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                Value result;
                value_set_type(&result, VAL_INT);
                value_set_int(&result, value_get_int(a) | value_get_int(b));
                push(vm, result);
                break;
            }

            case OP_BIT_XOR_LOCALS_INT: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 2;

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                Value result;
                value_set_type(&result, VAL_INT);
                value_set_int(&result, value_get_int(a) ^ value_get_int(b));
                push(vm, result);
                break;
            }

            case OP_ADD_LOCAL_CONST_INT:
            case OP_SUB_LOCAL_CONST_INT:
            case OP_MUL_LOCAL_CONST_INT:
            case OP_DIV_LOCAL_CONST_INT:
            case OP_MOD_LOCAL_CONST_INT:
            case OP_BIT_AND_LOCAL_CONST_INT:
            case OP_BIT_OR_LOCAL_CONST_INT:
            case OP_BIT_XOR_LOCAL_CONST_INT: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t const_idx = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 2;

                if (const_idx >= frame.function->constants.constant_count) {
                    vm_runtime_error(vm, "Constant index out of bounds");
                    return -1;
                }

                Constant c = frame.function->constants.constants[const_idx];
                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                if (value_get_type(a) != VAL_INT || c.type_index != 0) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                if ((instruction == OP_DIV_LOCAL_CONST_INT || instruction == OP_MOD_LOCAL_CONST_INT) && c.as_int == 0) {
                    vm_runtime_error(vm, instruction == OP_MOD_LOCAL_CONST_INT ? "Modulo by zero" : "Division by zero");
                    return -1;
                }

                Value result;
                value_set_type(&result, VAL_INT);
                switch (instruction) {
                    case OP_ADD_LOCAL_CONST_INT:
                        value_set_int(&result, value_get_int(a) + c.as_int);
                        break;
                    case OP_SUB_LOCAL_CONST_INT:
                        value_set_int(&result, value_get_int(a) - c.as_int);
                        break;
                    case OP_MUL_LOCAL_CONST_INT:
                        value_set_int(&result, value_get_int(a) * c.as_int);
                        break;
                    case OP_DIV_LOCAL_CONST_INT:
                        if (c.as_int == 2) {
                            value_set_int(&result, value_get_int(a) / 2);
                        } else if (c.as_int == -2) {
                            value_set_int(&result, value_get_int(a) / -2);
                        } else {
                            value_set_int(&result, value_get_int(a) / c.as_int);
                        }
                        break;
                    case OP_MOD_LOCAL_CONST_INT:
                        if (c.as_int == 2) {
                            value_set_int(&result, value_get_int(a) % 2);
                        } else if (c.as_int == -2) {
                            value_set_int(&result, value_get_int(a) % -2);
                        } else {
                            value_set_int(&result, value_get_int(a) % c.as_int);
                        }
                        break;
                    case OP_BIT_AND_LOCAL_CONST_INT:
                        value_set_int(&result, value_get_int(a) & c.as_int);
                        break;
                    case OP_BIT_OR_LOCAL_CONST_INT:
                        value_set_int(&result, value_get_int(a) | c.as_int);
                        break;
                    case OP_BIT_XOR_LOCAL_CONST_INT:
                        value_set_int(&result, value_get_int(a) ^ c.as_int);
                        break;
                    default:
                        value_set_int(&result, 0);
                        break;
                }
                push(vm, result);
                break;
            }

            case OP_ADD_LOCAL_CONST_DOUBLE:
            case OP_SUB_LOCAL_CONST_DOUBLE:
            case OP_MUL_LOCAL_CONST_DOUBLE:
            case OP_DIV_LOCAL_CONST_DOUBLE: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t const_idx = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip += 2;

                if (const_idx >= frame.function->constants.constant_count) {
                    vm_runtime_error(vm, "Constant index out of bounds");
                    return -1;
                }

                Constant c = frame.function->constants.constants[const_idx];
                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                if (value_get_type(a) != VAL_DOUBLE || c.type_index != 1) {
                    vm_runtime_error(vm, "Operands must be double");
                    return -1;
                }

                if (instruction == OP_DIV_LOCAL_CONST_DOUBLE && c.as_double == 0.0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }

                Value result;
                value_set_type(&result, VAL_DOUBLE);
                switch (instruction) {
                    case OP_ADD_LOCAL_CONST_DOUBLE:
                        value_set_double(&result, value_get_double(a) + c.as_double);
                        break;
                    case OP_SUB_LOCAL_CONST_DOUBLE:
                        value_set_double(&result, value_get_double(a) - c.as_double);
                        break;
                    case OP_MUL_LOCAL_CONST_DOUBLE:
                        value_set_double(&result, value_get_double(a) * c.as_double);
                        break;
                    case OP_DIV_LOCAL_CONST_DOUBLE:
                        value_set_double(&result, value_get_double(a) / c.as_double);
                        break;
                    default:
                        value_set_double(&result, 0.0);
                        break;
                }
                push(vm, result);
                break;
            }

            case OP_ADD:
            case OP_SUB:
            case OP_MUL:
            case OP_DIV: {
                Value b = pop(vm);
                Value a = pop(vm);
                Value result;

                if (vm_compute_arithmetic_op(vm, (OpCode)instruction, &a, &b, &result) != 0) {
                    value_free(&a);
                    value_free(&b);
                    return -1;
                }

                push(vm, result);
                value_free(&a);
                value_free(&b);
                break;
            }

            case OP_MOD: {
                Value b = pop(vm);
                Value a = pop(vm);
                Value result;

                if ((value_get_type(&a) == VAL_BIGINT || value_get_type(&b) == VAL_BIGINT) &&
                    (value_get_type(&a) == VAL_DOUBLE || value_get_type(&b) == VAL_DOUBLE)) {
                    vm_runtime_error(vm, "Cannot mix bigint and double");
                    value_free(&a);
                    value_free(&b);
                    return -1;
                } else if (value_get_type(&a) == VAL_BIGINT || value_get_type(&b) == VAL_BIGINT) {
                    bool release_a = false;
                    bool release_b = false;
                    ObjBigInt* big_a = bigint_from_value(&a, &release_a);
                    ObjBigInt* big_b = bigint_from_value(&b, &release_b);
                    if (!big_a || !big_b) {
                        vm_runtime_error(vm, "Operands must be integers");
                        if (release_a && big_a) obj_bigint_release(big_a);
                        if (release_b && big_b) obj_bigint_release(big_b);
                        value_free(&a);
                        value_free(&b);
                        return -1;
                    }
                    bool div_by_zero = false;
                    ObjBigInt* rem = obj_bigint_mod(big_a, big_b, &div_by_zero);
                    if (release_a) obj_bigint_release(big_a);
                    if (release_b) obj_bigint_release(big_b);
                    if (div_by_zero) {
                        vm_runtime_error(vm, "Division by zero");
                        if (rem) obj_bigint_release(rem);
                        value_free(&a);
                        value_free(&b);
                        return -1;
                    }
                    value_init_bigint(&result, rem);
                } else if (value_get_type(&a) == VAL_INT && value_get_type(&b) == VAL_INT) {
                    if (value_get_int(&b) == 0) {
                        vm_runtime_error(vm, "Division by zero");
                        value_free(&a);
                        value_free(&b);
                        return -1;
                    }
                    value_init_int(&result, value_get_int(&a) % value_get_int(&b));
                } else {
                    vm_runtime_error(vm, "Operands must be integers");
                    value_free(&a);
                    value_free(&b);
                    return -1;
                }

                push(vm, result);
                value_free(&a);
                value_free(&b);
                break;
            }

            case OP_NEG_DOUBLE: {
                if (vm->stack.count < 1) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                if (value_get_type(a) != VAL_DOUBLE) {
                    vm_runtime_error(vm, "Operand must be double");
                    value_free(a);
                    vm->stack.count--;
                    return -1;
                }

                value_set_double(a, -value_get_double(a));
                break;
            }

            case OP_NEG: {
                Value a = pop(vm);
                Value result;

                if (value_get_type(&a) == VAL_INT) {
                    value_init_int(&result, -value_get_int(&a));
                } else if (value_get_type(&a) == VAL_BIGINT) {
                    ObjBigInt* neg = obj_bigint_negate(value_get_bigint_obj(&a));
                    value_init_bigint(&result, neg);
                } else if (value_get_type(&a) == VAL_DOUBLE) {
                    value_init_double(&result, -value_get_double(&a));
                } else {
                    vm_runtime_error(vm, "Operand must be a number");
                    value_free(&a);
                    return -1;
                }

                push(vm, result);
                value_free(&a);
                break;
            }

            case OP_SQRT_LOCAL_DOUBLE: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip++;

                Value* valp = &vm->stack.values[frame.slots_start + slot];
                double x = 0.0;
                if (value_get_type(valp) == VAL_DOUBLE) {
                    x = value_get_double(valp);
                } else if (value_get_type(valp) == VAL_INT) {
                    x = (double)value_get_int(valp);
                } else {
                    x = 0.0;
                }

                Value result;
                value_set_type(&result, VAL_DOUBLE);
                value_set_double(&result, sqrt(x));
                push(vm, result);
                break;
            }

            case OP_ADD_STACK_LOCAL_DOUBLE:
            case OP_SUB_STACK_LOCAL_DOUBLE:
            case OP_MUL_STACK_LOCAL_DOUBLE:
            case OP_DIV_STACK_LOCAL_DOUBLE: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip++;

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_DOUBLE || value_get_type(b) != VAL_DOUBLE) {
                    vm_runtime_error(vm, "Operands must be double");
                    return -1;
                }

                if (instruction == OP_DIV_STACK_LOCAL_DOUBLE && value_get_double(b) == 0.0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }

                switch (instruction) {
                    case OP_ADD_STACK_LOCAL_DOUBLE:
                        value_set_double(a, value_get_double(a) + value_get_double(b));
                        break;
                    case OP_SUB_STACK_LOCAL_DOUBLE:
                        value_set_double(a, value_get_double(a) - value_get_double(b));
                        break;
                    case OP_MUL_STACK_LOCAL_DOUBLE:
                        value_set_double(a, value_get_double(a) * value_get_double(b));
                        break;
                    case OP_DIV_STACK_LOCAL_DOUBLE:
                        value_set_double(a, value_get_double(a) / value_get_double(b));
                        break;
                    default:
                        break;
                }

                break;
            }

            case OP_ADD_STACK_LOCAL_INT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip++;

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                value_set_int(a, value_get_int(a) + value_get_int(b));
                break;
            }

            case OP_SUB_STACK_LOCAL_INT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip++;

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                value_set_int(a, value_get_int(a) - value_get_int(b));
                break;
            }

            case OP_MUL_STACK_LOCAL_INT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip++;

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                value_set_int(a, value_get_int(a) * value_get_int(b));
                break;
            }

            case OP_DIV_STACK_LOCAL_INT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip++;

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }
                if (value_get_int(b) == 0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }

                value_set_int(a, value_get_int(a) / value_get_int(b));
                break;
            }

            case OP_MOD_STACK_LOCAL_INT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip++;

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }
                if (value_get_int(b) == 0) {
                    vm_runtime_error(vm, "Modulo by zero");
                    return -1;
                }

                value_set_int(a, value_get_int(a) % value_get_int(b));
                break;
            }

            case OP_BIT_AND_STACK_LOCAL_INT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip++;

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                value_set_int(a, value_get_int(a) & value_get_int(b));
                break;
            }

            case OP_BIT_OR_STACK_LOCAL_INT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip++;

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                value_set_int(a, value_get_int(a) | value_get_int(b));
                break;
            }

            case OP_BIT_XOR_STACK_LOCAL_INT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t b_slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip++;

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];
                if (value_get_type(a) != VAL_INT || value_get_type(b) != VAL_INT) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                value_set_int(a, value_get_int(a) ^ value_get_int(b));
                break;
            }

            case OP_ADD_STACK_CONST_INT:
            case OP_SUB_STACK_CONST_INT:
            case OP_MUL_STACK_CONST_INT:
            case OP_DIV_STACK_CONST_INT:
            case OP_MOD_STACK_CONST_INT:
            case OP_BIT_AND_STACK_CONST_INT:
            case OP_BIT_OR_STACK_CONST_INT:
            case OP_BIT_XOR_STACK_CONST_INT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t const_idx = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip++;

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }
                if (const_idx >= frame.function->constants.constant_count) {
                    vm_runtime_error(vm, "Constant index out of bounds");
                    return -1;
                }

                Constant c = frame.function->constants.constants[const_idx];
                Value* a = &vm->stack.values[vm->stack.count - 1];
                if (value_get_type(a) != VAL_INT || c.type_index != 0) {
                    vm_runtime_error(vm, "Operands must be integer");
                    return -1;
                }

                if ((instruction == OP_DIV_STACK_CONST_INT || instruction == OP_MOD_STACK_CONST_INT) && c.as_int == 0) {
                    vm_runtime_error(vm, instruction == OP_MOD_STACK_CONST_INT ? "Modulo by zero" : "Division by zero");
                    return -1;
                }

                switch (instruction) {
                    case OP_ADD_STACK_CONST_INT:
                        value_set_int(a, value_get_int(a) + c.as_int);
                        break;
                    case OP_SUB_STACK_CONST_INT:
                        value_set_int(a, value_get_int(a) - c.as_int);
                        break;
                    case OP_MUL_STACK_CONST_INT:
                        value_set_int(a, value_get_int(a) * c.as_int);
                        break;
                    case OP_DIV_STACK_CONST_INT:
                        if (c.as_int == 2) {
                            value_set_int(a, value_get_int(a) / 2);
                        } else if (c.as_int == -2) {
                            value_set_int(a, value_get_int(a) / -2);
                        } else {
                            value_set_int(a, value_get_int(a) / c.as_int);
                        }
                        break;
                    case OP_MOD_STACK_CONST_INT:
                        if (c.as_int == 2) {
                            value_set_int(a, value_get_int(a) % 2);
                        } else if (c.as_int == -2) {
                            value_set_int(a, value_get_int(a) % -2);
                        } else {
                            value_set_int(a, value_get_int(a) % c.as_int);
                        }
                        break;
                    case OP_BIT_AND_STACK_CONST_INT:
                        value_set_int(a, value_get_int(a) & c.as_int);
                        break;
                    case OP_BIT_OR_STACK_CONST_INT:
                        value_set_int(a, value_get_int(a) | c.as_int);
                        break;
                    case OP_BIT_XOR_STACK_CONST_INT:
                        value_set_int(a, value_get_int(a) ^ c.as_int);
                        break;
                    default:
                        break;
                }

                break;
            }

            case OP_ADD_STACK_CONST_DOUBLE:
            case OP_SUB_STACK_CONST_DOUBLE:
            case OP_MUL_STACK_CONST_DOUBLE:
            case OP_DIV_STACK_CONST_DOUBLE: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t const_idx = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip++;

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }
                if (const_idx >= frame.function->constants.constant_count) {
                    vm_runtime_error(vm, "Constant index out of bounds");
                    return -1;
                }

                Constant c = frame.function->constants.constants[const_idx];
                Value* a = &vm->stack.values[vm->stack.count - 1];
                if (value_get_type(a) != VAL_DOUBLE || c.type_index != 1) {
                    vm_runtime_error(vm, "Operands must be double");
                    return -1;
                }

                if (instruction == OP_DIV_STACK_CONST_DOUBLE && c.as_double == 0.0) {
                    vm_runtime_error(vm, "Division by zero");
                    return -1;
                }

                switch (instruction) {
                    case OP_ADD_STACK_CONST_DOUBLE:
                        value_set_double(a, value_get_double(a) + c.as_double);
                        break;
                    case OP_SUB_STACK_CONST_DOUBLE:
                        value_set_double(a, value_get_double(a) - c.as_double);
                        break;
                    case OP_MUL_STACK_CONST_DOUBLE:
                        value_set_double(a, value_get_double(a) * c.as_double);
                        break;
                    case OP_DIV_STACK_CONST_DOUBLE:
                        value_set_double(a, value_get_double(a) / c.as_double);
                        break;
                    default:
                        break;
                }

                break;
            }

            case OP_SQRT: {
                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* slot = &vm->stack.values[vm->stack.count - 1];
                double x = 0.0;
                if (value_get_type(slot) == VAL_DOUBLE) {
                    x = value_get_double(slot);
                } else if (value_get_type(slot) == VAL_INT) {
                    x = (double)value_get_int(slot);
                } else {
                    value_free(slot);
                    value_set_type(slot, VAL_DOUBLE);
                    value_set_double(slot, 0.0);
                    break;
                }

                value_set_type(slot, VAL_DOUBLE);
                value_set_double(slot, sqrt(x));
                break;
            }

            case OP_EQ: {
                Value b = pop(vm);
                Value a = pop(vm);
                Value result;
                value_init_bool(&result, value_equals(&a, &b));
                push(vm, result);
                value_free(&a);
                value_free(&b);
                break;
            }

            case OP_NE: {
                Value b = pop(vm);
                Value a = pop(vm);
                Value result;
                value_init_bool(&result, !value_equals(&a, &b));
                push(vm, result);
                value_free(&a);
                value_free(&b);
                break;
            }

            case OP_LT: {
                Value b = pop(vm);
                Value a = pop(vm);
                Value result;
                if (vm_compute_comparison_op(vm, OP_LT, &a, &b, &result) != 0) {
                    value_free(&a);
                    value_free(&b);
                    return -1;
                }

                push(vm, result);
                value_free(&a);
                value_free(&b);
                break;
            }

            case OP_LE: {
                Value b = pop(vm);
                Value a = pop(vm);
                Value result;
                if (vm_compute_comparison_op(vm, OP_LE, &a, &b, &result) != 0) {
                    value_free(&a);
                    value_free(&b);
                    return -1;
                }

                push(vm, result);
                value_free(&a);
                value_free(&b);
                break;
            }

            case OP_GT: {
                Value b = pop(vm);
                Value a = pop(vm);
                Value result;
                if (vm_compute_comparison_op(vm, OP_GT, &a, &b, &result) != 0) {
                    value_free(&a);
                    value_free(&b);
                    return -1;
                }

                push(vm, result);
                value_free(&a);
                value_free(&b);
                break;
            }

            case OP_GE: {
                Value b = pop(vm);
                Value a = pop(vm);
                Value result;
                if (vm_compute_comparison_op(vm, OP_GE, &a, &b, &result) != 0) {
                    value_free(&a);
                    value_free(&b);
                    return -1;
                }

                push(vm, result);
                value_free(&a);
                value_free(&b);
                break;
            }

            case OP_NOT: {
                Value a = pop(vm);
                Value result;
                if (value_get_type(&a) != VAL_BOOL) {
                    vm_runtime_error(vm, "Operand must be bool for logical not");
                    value_free(&a);
                    return -1;
                }
                value_init_bool(&result, !value_get_bool(&a));
                push(vm, result);
                value_free(&a);
                break;
            }

            case OP_AND: {
                Value b = pop(vm);
                Value a = pop(vm);
                Value result;
                if (value_get_type(&a) != VAL_BOOL || value_get_type(&b) != VAL_BOOL) {
                    vm_runtime_error(vm, "Operands must be bool for logical operation");
                    value_free(&a);
                    value_free(&b);
                    return -1;
                }
                value_init_bool(&result, value_get_bool(&a) && value_get_bool(&b));
                push(vm, result);
                value_free(&a);
                value_free(&b);
                break;
            }

            case OP_OR: {
                Value b = pop(vm);
                Value a = pop(vm);
                Value result;
                if (value_get_type(&a) != VAL_BOOL || value_get_type(&b) != VAL_BOOL) {
                    vm_runtime_error(vm, "Operands must be bool for logical operation");
                    value_free(&a);
                    value_free(&b);
                    return -1;
                }
                value_init_bool(&result, value_get_bool(&a) || value_get_bool(&b));
                push(vm, result);
                value_free(&a);
                value_free(&b);
                break;
            }

            case OP_BIT_NOT: {
                Value a = pop(vm);
                Value result;

                if (value_get_type(&a) == VAL_INT) {
                    value_init_int(&result, ~value_get_int(&a));
                } else if (value_get_type(&a) == VAL_BIGINT) {
                    ObjBigInt* out = obj_bigint_bit_not(value_get_bigint_obj(&a));
                    value_init_bigint(&result, out);
                } else {
                    vm_runtime_error(vm, "Operand must be int/bigint for bitwise not");
                    value_free(&a);
                    return -1;
                }

                push(vm, result);
                value_free(&a);
                break;
            }

            case OP_BIT_AND:
            case OP_BIT_OR:
            case OP_BIT_XOR: {
                Value b = pop(vm);
                Value a = pop(vm);
                Value result;

                if (value_get_type(&a) == VAL_BIGINT || value_get_type(&b) == VAL_BIGINT) {
                    bool release_a = false;
                    bool release_b = false;
                    ObjBigInt* big_a = bigint_from_value(&a, &release_a);
                    ObjBigInt* big_b = bigint_from_value(&b, &release_b);
                    if (!big_a || !big_b) {
                        vm_runtime_error(vm, "Operands must be int/bigint for bitwise operation");
                        if (release_a && big_a) obj_bigint_release(big_a);
                        if (release_b && big_b) obj_bigint_release(big_b);
                        value_free(&a);
                        value_free(&b);
                        return -1;
                    }

                    ObjBigInt* out = NULL;
                    if (instruction == OP_BIT_AND) out = obj_bigint_bit_and(big_a, big_b);
                    else if (instruction == OP_BIT_OR) out = obj_bigint_bit_or(big_a, big_b);
                    else out = obj_bigint_bit_xor(big_a, big_b);

                    if (release_a) obj_bigint_release(big_a);
                    if (release_b) obj_bigint_release(big_b);

                    value_init_bigint(&result, out);
                } else if (value_get_type(&a) == VAL_INT && value_get_type(&b) == VAL_INT) {
                    int64_t out = 0;
                    if (instruction == OP_BIT_AND) out = value_get_int(&a) & value_get_int(&b);
                    else if (instruction == OP_BIT_OR) out = value_get_int(&a) | value_get_int(&b);
                    else out = value_get_int(&a) ^ value_get_int(&b);
                    value_init_int(&result, out);
                } else {
                    vm_runtime_error(vm, "Operands must be int/bigint for bitwise operation");
                    value_free(&a);
                    value_free(&b);
                    return -1;
                }

                push(vm, result);
                value_free(&a);
                value_free(&b);
                break;
            }

            case OP_JUMP: {
                // Read jump offset as big-endian i16. Keep evaluation ordered; multiple
                // `frame.ip++` in one expression is undefined behavior (Release-only bugs).
                VM_ENSURE_IP_BYTES(2);
                uint8_t hi = code[frame.ip++];
                uint8_t lo = code[frame.ip++];
                int16_t offset = (int16_t)((hi << 8) | lo);
                frame.ip += offset;
                break;
            }

            case OP_JUMP_IF_LOCAL_LT:
            case OP_JUMP_IF_LOCAL_LE:
            case OP_JUMP_IF_LOCAL_GT:
            case OP_JUMP_IF_LOCAL_GE: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                uint8_t hi = code[frame.ip++];
                uint8_t lo = code[frame.ip++];
                int16_t offset = (int16_t)((hi << 8) | lo);

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];

                bool should_jump = false;
                if (value_get_type(a) == VAL_INT && value_get_type(b) == VAL_INT) {
                    int64_t av = value_get_int(a);
                    int64_t bv = value_get_int(b);
                    switch (instruction) {
                        case OP_JUMP_IF_LOCAL_LT: should_jump = av < bv; break;
                        case OP_JUMP_IF_LOCAL_LE: should_jump = av <= bv; break;
                        case OP_JUMP_IF_LOCAL_GT: should_jump = av > bv; break;
                        case OP_JUMP_IF_LOCAL_GE: should_jump = av >= bv; break;
                        default: break;
                    }
                } else if (value_get_type(a) == VAL_DOUBLE && value_get_type(b) == VAL_DOUBLE) {
                    double av = value_get_double(a);
                    double bv = value_get_double(b);
                    switch (instruction) {
                        case OP_JUMP_IF_LOCAL_LT: should_jump = av < bv; break;
                        case OP_JUMP_IF_LOCAL_LE: should_jump = av <= bv; break;
                        case OP_JUMP_IF_LOCAL_GT: should_jump = av > bv; break;
                        case OP_JUMP_IF_LOCAL_GE: should_jump = av >= bv; break;
                        default: break;
                    }
                } else {
                    vm_runtime_error(vm, "Operands must be same numeric type");
                    return -1;
                }

                if (should_jump) {
                    frame.ip += offset;
                }
                break;
            }

            case OP_JUMP_IF_LOCAL_EQ:
            case OP_JUMP_IF_LOCAL_NE: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t b_slot = code[frame.ip++];
                uint8_t hi = code[frame.ip++];
                uint8_t lo = code[frame.ip++];
                int16_t offset = (int16_t)((hi << 8) | lo);

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];

                bool eq = false;
                if (value_get_type(a) == VAL_INT && value_get_type(b) == VAL_INT) {
                    eq = value_get_int(a) == value_get_int(b);
                } else if (value_get_type(a) == VAL_BOOL && value_get_type(b) == VAL_BOOL) {
                    eq = value_get_bool(a) == value_get_bool(b);
                } else if (value_get_type(a) == VAL_DOUBLE && value_get_type(b) == VAL_DOUBLE) {
                    eq = value_get_double(a) == value_get_double(b);
                } else if (value_get_type(a) == VAL_STRING && value_get_type(b) == VAL_STRING) {
                    ObjString* a_str = value_get_string_obj(a);
                    ObjString* b_str = value_get_string_obj(b);
                    eq = a_str && b_str && strcmp(a_str->chars, b_str->chars) == 0;
                } else {
                    eq = value_equals(a, b);
                }

                bool should_jump = (instruction == OP_JUMP_IF_LOCAL_EQ) ? eq : !eq;
                if (should_jump) {
                    frame.ip += offset;
                }
                break;
            }

            case OP_JUMP_IF_LOCAL_LT_CONST:
            case OP_JUMP_IF_LOCAL_LE_CONST:
            case OP_JUMP_IF_LOCAL_GT_CONST:
            case OP_JUMP_IF_LOCAL_GE_CONST: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t const_idx = code[frame.ip++];
                uint8_t hi = code[frame.ip++];
                uint8_t lo = code[frame.ip++];
                int16_t offset = (int16_t)((hi << 8) | lo);

                if (const_idx >= frame.function->constants.constant_count) {
                    vm_runtime_error(vm, "Constant index out of bounds");
                    return -1;
                }

                Constant c = frame.function->constants.constants[const_idx];
                Value* a = &vm->stack.values[frame.slots_start + a_slot];

                bool should_jump = false;
                if (value_get_type(a) == VAL_INT && c.type_index == 0) {
                    int64_t av = value_get_int(a);
                    int64_t bv = c.as_int;
                    switch (instruction) {
                        case OP_JUMP_IF_LOCAL_LT_CONST: should_jump = av < bv; break;
                        case OP_JUMP_IF_LOCAL_LE_CONST: should_jump = av <= bv; break;
                        case OP_JUMP_IF_LOCAL_GT_CONST: should_jump = av > bv; break;
                        case OP_JUMP_IF_LOCAL_GE_CONST: should_jump = av >= bv; break;
                        default: break;
                    }
                } else if (value_get_type(a) == VAL_DOUBLE && c.type_index == 1) {
                    double av = value_get_double(a);
                    double bv = c.as_double;
                    switch (instruction) {
                        case OP_JUMP_IF_LOCAL_LT_CONST: should_jump = av < bv; break;
                        case OP_JUMP_IF_LOCAL_LE_CONST: should_jump = av <= bv; break;
                        case OP_JUMP_IF_LOCAL_GT_CONST: should_jump = av > bv; break;
                        case OP_JUMP_IF_LOCAL_GE_CONST: should_jump = av >= bv; break;
                        default: break;
                    }
                } else {
                    vm_runtime_error(vm, "Operands must be same numeric type");
                    return -1;
                }

                if (should_jump) {
                    frame.ip += offset;
                }
                break;
            }

            case OP_JUMP_IF_LOCAL_EQ_CONST:
            case OP_JUMP_IF_LOCAL_NE_CONST: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t const_idx = code[frame.ip++];
                uint8_t hi = code[frame.ip++];
                uint8_t lo = code[frame.ip++];
                int16_t offset = (int16_t)((hi << 8) | lo);

                if (const_idx >= frame.function->constants.constant_count) {
                    vm_runtime_error(vm, "Constant index out of bounds");
                    return -1;
                }

                Constant c = frame.function->constants.constants[const_idx];
                Value* a = &vm->stack.values[frame.slots_start + a_slot];

                bool eq = vm_value_equals_constant_fast_for_branch(a, &c);

                bool should_jump = (instruction == OP_JUMP_IF_LOCAL_EQ_CONST) ? eq : !eq;
                if (should_jump) {
                    frame.ip += offset;
                }
                break;
            }

            case OP_JUMP_IF_LOCAL_EQ_GLOBAL:
            case OP_JUMP_IF_LOCAL_NE_GLOBAL: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t name_idx = code[frame.ip++];
                uint8_t hi = code[frame.ip++];
                uint8_t lo = code[frame.ip++];
                int16_t offset = (int16_t)((hi << 8) | lo);

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = NULL;
                Value btmp;

                int slot = vm_resolve_global_slot_cached(vm, frame.function, name_idx);
                if (slot >= 0 && slot < vm->global_count) {
                    if (slot <= 0xff) {
                        // Inline cache: rewrite this compare+jump to use the resolved global slot directly.
                        // opcode at (ip-5), operand at (ip-3).
                        code[frame.ip - 5] = (instruction == OP_JUMP_IF_LOCAL_EQ_GLOBAL) ? OP_JUMP_IF_LOCAL_EQ_GLOBAL_SLOT : OP_JUMP_IF_LOCAL_NE_GLOBAL_SLOT;
                        code[frame.ip - 3] = (uint8_t)slot;
                    }
                    b = &vm->globals[slot];
                } else {
                    Constant c = frame.function->constants.constants[name_idx];
                    btmp = vm_get_global(vm, c.as_string);
                    b = &btmp;
                }

                bool eq = vm_value_equals_fast_for_branch(a, b);

                bool should_jump = (instruction == OP_JUMP_IF_LOCAL_EQ_GLOBAL) ? eq : !eq;
                if (should_jump) {
                    frame.ip += offset;
                }
                break;
            }

            case OP_JUMP_IF_LOCAL_EQ_GLOBAL_SLOT:
            case OP_JUMP_IF_LOCAL_NE_GLOBAL_SLOT: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t a_slot = code[frame.ip++];
                uint8_t g_slot = code[frame.ip++];
                uint8_t hi = code[frame.ip++];
                uint8_t lo = code[frame.ip++];
                int16_t offset = (int16_t)((hi << 8) | lo);

                if (g_slot >= vm->global_count) {
                    vm_runtime_error(vm, "Global slot out of bounds");
                    return -1;
                }

                Value* a = &vm->stack.values[frame.slots_start + a_slot];
                Value* b = &vm->globals[g_slot];

                bool eq = vm_value_equals_fast_for_branch(a, b);

                bool should_jump = (instruction == OP_JUMP_IF_LOCAL_EQ_GLOBAL_SLOT) ? eq : !eq;
                if (should_jump) {
                    frame.ip += offset;
                }
                break;
            }

            case OP_JUMP_IF_STACK_LT_LOCAL:
            case OP_JUMP_IF_STACK_LE_LOCAL:
            case OP_JUMP_IF_STACK_GT_LOCAL:
            case OP_JUMP_IF_STACK_GE_LOCAL: {
                VM_ENSURE_IP_BYTES(3);
                uint8_t b_slot = code[frame.ip++];
                uint8_t hi = code[frame.ip++];
                uint8_t lo = code[frame.ip++];
                int16_t offset = (int16_t)((hi << 8) | lo);

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* a = &vm->stack.values[vm->stack.count - 1];
                Value* b = &vm->stack.values[frame.slots_start + b_slot];

                bool should_jump = false;
                if (value_get_type(a) == VAL_INT && value_get_type(b) == VAL_INT) {
                    int64_t av = value_get_int(a);
                    int64_t bv = value_get_int(b);
                    switch (instruction) {
                        case OP_JUMP_IF_STACK_LT_LOCAL: should_jump = av < bv; break;
                        case OP_JUMP_IF_STACK_LE_LOCAL: should_jump = av <= bv; break;
                        case OP_JUMP_IF_STACK_GT_LOCAL: should_jump = av > bv; break;
                        case OP_JUMP_IF_STACK_GE_LOCAL: should_jump = av >= bv; break;
                        default: break;
                    }
                } else if (value_get_type(a) == VAL_DOUBLE && value_get_type(b) == VAL_DOUBLE) {
                    double av = value_get_double(a);
                    double bv = value_get_double(b);
                    switch (instruction) {
                        case OP_JUMP_IF_STACK_LT_LOCAL: should_jump = av < bv; break;
                        case OP_JUMP_IF_STACK_LE_LOCAL: should_jump = av <= bv; break;
                        case OP_JUMP_IF_STACK_GT_LOCAL: should_jump = av > bv; break;
                        case OP_JUMP_IF_STACK_GE_LOCAL: should_jump = av >= bv; break;
                        default: break;
                    }
                } else {
                    vm_runtime_error(vm, "Operands must be same numeric type");
                    return -1;
                }

                // Always consume the stack operand (primitive numeric).
                vm->stack.count--;

                if (should_jump) {
                    frame.ip += offset;
                }
                break;
            }

            case OP_JUMP_IF_STACK_LT_CONST:
            case OP_JUMP_IF_STACK_LE_CONST:
            case OP_JUMP_IF_STACK_GT_CONST:
            case OP_JUMP_IF_STACK_GE_CONST: {
                VM_ENSURE_IP_BYTES(3);
                uint8_t const_idx = code[frame.ip++];
                uint8_t hi = code[frame.ip++];
                uint8_t lo = code[frame.ip++];
                int16_t offset = (int16_t)((hi << 8) | lo);

                if (const_idx >= frame.function->constants.constant_count) {
                    vm_runtime_error(vm, "Constant index out of bounds");
                    return -1;
                }

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Constant c = frame.function->constants.constants[const_idx];
                Value* a = &vm->stack.values[vm->stack.count - 1];

                bool should_jump = false;
                if (value_get_type(a) == VAL_INT && c.type_index == 0) {
                    int64_t av = value_get_int(a);
                    int64_t bv = c.as_int;
                    switch (instruction) {
                        case OP_JUMP_IF_STACK_LT_CONST: should_jump = av < bv; break;
                        case OP_JUMP_IF_STACK_LE_CONST: should_jump = av <= bv; break;
                        case OP_JUMP_IF_STACK_GT_CONST: should_jump = av > bv; break;
                        case OP_JUMP_IF_STACK_GE_CONST: should_jump = av >= bv; break;
                        default: break;
                    }
                } else if (value_get_type(a) == VAL_DOUBLE && c.type_index == 1) {
                    double av = value_get_double(a);
                    double bv = c.as_double;
                    switch (instruction) {
                        case OP_JUMP_IF_STACK_LT_CONST: should_jump = av < bv; break;
                        case OP_JUMP_IF_STACK_LE_CONST: should_jump = av <= bv; break;
                        case OP_JUMP_IF_STACK_GT_CONST: should_jump = av > bv; break;
                        case OP_JUMP_IF_STACK_GE_CONST: should_jump = av >= bv; break;
                        default: break;
                    }
                } else {
                    vm_runtime_error(vm, "Operands must be same numeric type");
                    return -1;
                }

                vm->stack.count--;

                if (should_jump) {
                    frame.ip += offset;
                }
                break;
            }

            case OP_JUMP_IF_ARRAY_FALSE_LOCAL_CONST: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx = code[frame.ip++];
                uint8_t hi = code[frame.ip++];
                uint8_t lo = code[frame.ip++];
                int16_t offset = (int16_t)((hi << 8) | lo);

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                if (idx >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                bool is_true = false;
                if (arr->kind == ARRAY_KIND_BOOL) {
                    is_true = arr->data.bools[idx] != 0;
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    Value elem = arr->data.elements[idx];
                    if (value_get_type(&elem) != VAL_BOOL) {
                        vm_runtime_error(vm, "Condition must be bool");
                        return -1;
                    }
                    is_true = value_get_bool(&elem);
                } else {
                    vm_runtime_error(vm, "Condition must be bool");
                    return -1;
                }

                if (!is_true) {
                    frame.ip += offset;
                }
                break;
            }

            case OP_JUMP_IF_ARRAY_FALSE_LOCAL_LOCAL: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_slot = code[frame.ip++];
                uint8_t hi = code[frame.ip++];
                uint8_t lo = code[frame.ip++];
                int16_t offset = (int16_t)((hi << 8) | lo);

                Value* idx_valp = &vm->stack.values[frame.slots_start + idx_slot];
                if (value_get_type(idx_valp) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                int64_t idx64 = value_get_int(idx_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                int idx = (int)idx64;
                bool is_true = false;
                if (arr->kind == ARRAY_KIND_BOOL) {
                    is_true = arr->data.bools[idx] != 0;
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    Value elem = arr->data.elements[idx];
                    if (value_get_type(&elem) != VAL_BOOL) {
                        vm_runtime_error(vm, "Condition must be bool");
                        return -1;
                    }
                    is_true = value_get_bool(&elem);
                } else {
                    vm_runtime_error(vm, "Condition must be bool");
                    return -1;
                }

                if (!is_true) {
                    frame.ip += offset;
                }
                break;
            }

            case OP_JUMP_IF_FALSE: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t hi = code[frame.ip++];
                uint8_t lo = code[frame.ip++];
                int16_t offset = (int16_t)((hi << 8) | lo);
                Value condition = peek(vm, 0);
                if (value_get_type(&condition) != VAL_BOOL) {
                    vm_runtime_error(vm, "Condition must be bool");
                    return -1;
                }
                if (!value_get_bool(&condition)) {
                    frame.ip += offset;
                }
                break;
            }

            case OP_JUMP_IF_FALSE_POP: {
                VM_ENSURE_IP_BYTES(3);
                uint8_t hi = code[frame.ip++];
                uint8_t lo = code[frame.ip++];
                int16_t offset = (int16_t)((hi << 8) | lo);

                Value condition = peek(vm, 0);
                if (value_get_type(&condition) != VAL_BOOL) {
                    vm_runtime_error(vm, "Condition must be bool");
                    return -1;
                }

                if (!value_get_bool(&condition)) {
                    // Offsets for this length-preserving superinstruction are encoded
                    // relative to the same base as OP_JUMP_IF_FALSE (after hi/lo),
                    // i.e. before the padding byte (which replaced OP_POP).
                    frame.ip += offset;
                } else {
                    frame.ip++; // consume padding
                    Value val = pop(vm);
                    value_free(&val);
                }
                break;
            }

            case OP_MAKE_CLOSURE: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t capture_count = code[frame.ip++];

                int template_pos = vm->stack.count - 1 - (int)capture_count;
                if (template_pos < 0) {
                    vm_runtime_error(vm, "Stack underflow in closure creation");
                    return -1;
                }

                Value template_val = vm->stack.values[template_pos];
                if (value_get_type(&template_val) != VAL_FUNCTION || !value_get_function_obj(&template_val)) {
                    vm_runtime_error(vm, "Closure template must be a function");
                    return -1;
                }

                ObjFunction* template_func = value_get_function_obj(&template_val);
                if (template_func->capture_count != (int)capture_count) {
                    vm_runtime_error(vm, "Closure capture count mismatch");
                    return -1;
                }

                Value* captures = NULL;
                if (capture_count > 0) {
                    captures = (Value*)safe_malloc((size_t)capture_count * sizeof(Value));
                    for (int i = 0; i < capture_count; i++) {
                        captures[i] = vm->stack.values[template_pos + 1 + i];
                    }
                }

                ObjFunction* closure = obj_function_clone_with_captures(template_func, captures, (int)capture_count);
                if (captures) free(captures);
                if (!closure) {
                    vm_runtime_error(vm, "Failed to create closure");
                    return -1;
                }

                for (int i = 0; i < (int)capture_count + 1; i++) {
                    Value dropped = pop(vm);
                    value_free(&dropped);
                }

                Value closure_val;
                value_init_function(&closure_val, closure);
                push(vm, closure_val);
                break;
            }

            case OP_CALL: {
                // Fix 12: Stack is [callee, arg0, ..., argN-1] (callee pushed first)
                VM_ENSURE_IP_BYTES(1);
                uint8_t arg_count = code[frame.ip++];
                int callee_pos = vm->stack.count - 1 - arg_count;
                if (callee_pos < 0) {
                    vm_runtime_error(vm, "Stack underflow in call");
                    return -1;
                }
                Value callee = vm->stack.values[callee_pos];

                if (value_get_type(&callee) == VAL_NATIVE) {
                    ObjNative* native = value_get_native_obj(&callee);
                    vm->frames[vm->frame_count - 1] = frame;
                    if (native->arity != arg_count) {
                        vm_runtime_error(vm, "Wrong number of arguments");
                        return -1;
                    }
                    // Remove callee from under the args
                    for (int i = 0; i < arg_count; i++) {
                        vm->stack.values[callee_pos + i] = vm->stack.values[callee_pos + 1 + i];
                    }
                    vm->stack.count--;

                    if (arg_count == 0) {
                        // Provide a slot for the builtin to write its result.
                        Value nil;
                        value_init_nil(&nil);
                        push(vm, nil);
                        if (vm_native_invoke_failed(vm, native->invoke(vm, native))) {
                            value_free(&callee);
                            break;
                        }
                        value_free(&callee);
                        break;
                    }

                    if (vm_native_invoke_failed(vm, native->invoke(vm, native))) {
                        value_free(&callee);
                        break;
                    }

                    // Clean up extra arg slots: builtin writes result at lowest arg position.
                    // Release dropped args so objects don't leak.
                    int args_base = vm->stack.count - arg_count;
                    for (int i = 1; i < arg_count; i++) {
                        value_free(&vm->stack.values[args_base + i]);
                    }
                    vm->stack.count -= (arg_count - 1);

                    // Release the removed callee reference.
                    value_free(&callee);
                    break;
                }

                if (value_get_type(&callee) != VAL_FUNCTION) {
                    vm_runtime_error(vm, "Can only call functions");
                    return -1;
                }

                ObjFunction* called_func = value_get_function_obj(&callee);
                if (!called_func) {
                    vm_runtime_error(vm, "Null function");
                    return -1;
                }

                if (called_func->param_count != arg_count) {
                    vm_runtime_error(vm, "Wrong number of arguments");
                    return -1;
                }

                if (vm->frame_count + 1 > vm->config.max_call_depth) {
                    vm_runtime_error(vm, "Stack overflow: maximum call depth exceeded");
                    return -1;
                }

                if (called_func->local_count < arg_count) {
                    vm_runtime_error(vm, "Function local slots less than arguments");
                    return -1;
                }

                if (called_func->is_async) {
                    if (vm_spawn_async_call(vm, called_func, callee_pos, arg_count, true) != 0) {
                        return -1;
                    }
                    break;
                }

                // Remove callee from under the args (callee was pushed first).
                for (int i = 0; i < arg_count; i++) {
                    vm->stack.values[callee_pos + i] = vm->stack.values[callee_pos + 1 + i];
                }
                vm->stack.count--;
                // Release the removed callee reference.
                value_free(&callee);

                jit_record_function_entry(vm, called_func);
                bool handled = false;
                int compiled_rc =
                    vm_try_call_compiled_entry(vm, called_func, callee_pos, arg_count, &frame, &chunk, &code, &handled);
                if (compiled_rc != 0) {
                    return -1;
                }
                if (handled) {
                    break;
                }

                // Allocate remaining locals; parameters already occupy slots 0..arg_count-1.
                // Save the caller frame before switching.
                vm->frames[vm->frame_count - 1] = frame;
                push_nil_count(vm, called_func->local_count - arg_count);

                CallFrame new_frame;
                new_frame.function = called_func;
                new_frame.ip = 0;
                new_frame.slots_start = callee_pos;
                new_frame.slots_count = called_func->local_count;
                new_frame.defers = NULL;
                new_frame.defer_count = 0;
                new_frame.defer_capacity = 0;
                new_frame.is_async_root = false;
                new_frame.async_result_future = NULL;
                new_frame.panic_unwinding = false;
                new_frame.panic_message = NULL;
                if (!vm_bind_captures_to_frame(vm, &new_frame, called_func)) {
                    return -1;
                }
                jit_record_function_entry(vm, called_func);

                vm_frame_reserve(vm, 1);
                vm->frames[vm->frame_count++] = new_frame;
                vm->current_call_depth = vm->frame_count;

                frame = new_frame;
                chunk = &frame.function->chunk;
                code = chunk->code;
                break;
            }

            case OP_TYPE_TEST_INTERFACE_METHOD: {
                VM_ENSURE_IP_BYTES(4);
                int interface_name_idx = ((int)code[frame.ip] << 8) | (int)code[frame.ip + 1];
                frame.ip += 2;
                int method_name_idx = ((int)code[frame.ip] << 8) | (int)code[frame.ip + 1];
                frame.ip += 2;

                Value value = pop(vm);
                bool ok = false;

                if (value_get_type(&value) == VAL_RECORD) {
                    ObjRecord* record = value_get_record_obj(&value);
                    const char* record_name = vm_record_runtime_type_name(record);
                    const char* interface_name = NULL;
                    if (record_name &&
                        vm_get_global_name_constant(vm, frame.function, interface_name_idx, &interface_name) == 0) {
                        if (method_name_idx == 0xffff) {
                            ok = true;
                        } else {
                            const char* method_name = NULL;
                            if (vm_get_global_name_constant(vm, frame.function, method_name_idx, &method_name) == 0) {
                                ok = vm_record_supports_interface_method(vm,
                                                                         interface_name,
                                                                         record_name,
                                                                         method_name);
                            }
                        }
                    }
                }

                Value result;
                value_init_bool(&result, ok);
                push(vm, result);
                value_free(&value);
                break;
            }

            case OP_CALL_INTERFACE: {
                VM_ENSURE_IP_BYTES(5);
                int interface_name_idx = ((int)code[frame.ip] << 8) | (int)code[frame.ip + 1];
                frame.ip += 2;
                int method_name_idx = ((int)code[frame.ip] << 8) | (int)code[frame.ip + 1];
                frame.ip += 2;
                uint8_t arg_count = code[frame.ip++];

                int args_base = vm->stack.count - arg_count;
                if (args_base < 0) {
                    vm_runtime_error(vm, "Stack underflow in call");
                    return -1;
                }
                if (arg_count == 0) {
                    vm_runtime_error(vm, "Interface call requires a receiver argument");
                    return -1;
                }

                Value receiver = vm->stack.values[args_base];
                if (value_get_type(&receiver) != VAL_RECORD || !value_get_record_obj(&receiver)) {
                    vm_runtime_error(vm, "Interface call receiver must be a record");
                    return -1;
                }
                ObjRecord* receiver_record = value_get_record_obj(&receiver);

                const char* receiver_record_name = vm_record_runtime_type_name(receiver_record);
                if (!receiver_record_name) {
                    vm_runtime_error(vm, "Interface call receiver is missing a runtime record type");
                    return -1;
                }

                const char* interface_name = NULL;
                if (vm_get_global_name_constant(vm, frame.function, interface_name_idx, &interface_name) != 0) {
                    return -1;
                }

                const char* method_name = NULL;
                if (vm_get_global_name_constant(vm, frame.function, method_name_idx, &method_name) != 0) {
                    return -1;
                }

                int dispatch_idx = -1;
                const char* function_name = vm_resolve_interface_dispatch_function_name(
                    vm,
                    interface_name,
                    receiver_record_name,
                    method_name,
                    &dispatch_idx);
                if (!function_name || function_name[0] == '\0') {
                    vm_runtime_error(vm, "Interface dispatch resolved an invalid function name");
                    return -1;
                }

                Value callee;
                int slot = -1;

                if (dispatch_idx >= 0 &&
                    vm->interface_dispatch_slot_cache &&
                    dispatch_idx < vm->interface_dispatch_count) {
                    int cached_slot = vm->interface_dispatch_slot_cache[dispatch_idx];
                    if (cached_slot >= 0 &&
                        cached_slot < vm->global_count &&
                        vm->global_names &&
                        vm->global_names[cached_slot] &&
                        strcmp(vm->global_names[cached_slot], function_name) == 0) {
                        slot = cached_slot;
                    }
                } else {
                    slot = vm_resolve_global_slot_cached(vm, frame.function, method_name_idx);
                }

                if (slot < 0) {
                    HashEntry* entry = hash_table_get_entry(&vm->globals_hash, function_name);
                    if (entry &&
                        entry->global_slot >= 0 &&
                        entry->global_slot < vm->global_count &&
                        vm->global_names &&
                        vm->global_names[entry->global_slot] &&
                        strcmp(vm->global_names[entry->global_slot], function_name) == 0) {
                        slot = entry->global_slot;
                    } else {
                        slot = vm_find_global_slot(vm, function_name);
                    }
                }

                if (dispatch_idx >= 0 &&
                    vm->interface_dispatch_slot_cache &&
                    dispatch_idx < vm->interface_dispatch_count) {
                    vm->interface_dispatch_slot_cache[dispatch_idx] = slot;
                }

                if (slot >= 0 && slot < vm->global_count) {
                    callee = vm->globals[slot];
                } else {
                    callee = vm_get_global(vm, function_name);
                }

                if (value_get_type(&callee) == VAL_NATIVE) {
                    ObjNative* native = value_get_native_obj(&callee);
                    vm->frames[vm->frame_count - 1] = frame;
                    if (native->arity != arg_count) {
                        vm_runtime_error(vm, "Wrong number of arguments");
                        return -1;
                    }

                    if (arg_count == 0) {
                        push_nil_count(vm, 1);
                        if (vm_native_invoke_failed(vm, native->invoke(vm, native))) {
                            break;
                        }
                        break;
                    }

                    if (vm_native_invoke_failed(vm, native->invoke(vm, native))) {
                        break;
                    }

                    int base = vm->stack.count - arg_count;
                    for (int i = 1; i < arg_count; i++) {
                        value_free(&vm->stack.values[base + i]);
                    }
                    vm->stack.count -= (arg_count - 1);
                    break;
                }

                if (value_get_type(&callee) != VAL_FUNCTION) {
                    vm_runtime_error(vm, "Can only call functions");
                    return -1;
                }

                ObjFunction* called_func = value_get_function_obj(&callee);
                if (!called_func) {
                    vm_runtime_error(vm, "Null function");
                    return -1;
                }

                if (called_func->is_async) {
                    if (vm_spawn_async_call(vm, called_func, args_base, arg_count, false) != 0) {
                        return -1;
                    }
                    break;
                }

                jit_record_function_entry(vm, called_func);
                bool handled = false;
                int compiled_rc =
                    vm_try_call_compiled_entry(vm, called_func, args_base, arg_count, &frame, &chunk, &code, &handled);
                if (compiled_rc != 0) {
                    return -1;
                }
                if (handled) {
                    break;
                }

                if (called_func->param_count != arg_count) {
                    vm_runtime_error(vm, "Wrong number of arguments");
                    return -1;
                }
                if (vm->frame_count + 1 > vm->config.max_call_depth) {
                    vm_runtime_error(vm, "Stack overflow: maximum call depth exceeded");
                    return -1;
                }
                if (called_func->local_count < arg_count) {
                    vm_runtime_error(vm, "Function local slots less than arguments");
                    return -1;
                }

                vm->frames[vm->frame_count - 1] = frame;

                push_nil_count(vm, called_func->local_count - arg_count);

                CallFrame new_frame;
                new_frame.function = called_func;
                new_frame.ip = 0;
                new_frame.slots_start = args_base;
                new_frame.slots_count = called_func->local_count;
                new_frame.defers = NULL;
                new_frame.defer_count = 0;
                new_frame.defer_capacity = 0;
                new_frame.is_async_root = false;
                new_frame.async_result_future = NULL;
                new_frame.panic_unwinding = false;
                new_frame.panic_message = NULL;
                if (!vm_bind_captures_to_frame(vm, &new_frame, called_func)) {
                    return -1;
                }

                vm_frame_reserve(vm, 1);
                vm->frames[vm->frame_count++] = new_frame;
                vm->current_call_depth = vm->frame_count;

                frame = new_frame;
                chunk = &frame.function->chunk;
                code = chunk->code;
                break;
            }

            case OP_CALL_GLOBAL: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t name_idx = code[frame.ip++];
                uint8_t arg_count = code[frame.ip++];

                int args_base = vm->stack.count - arg_count;
                if (args_base < 0) {
                    vm_runtime_error(vm, "Stack underflow in call");
                    return -1;
                }

                Value callee;
                int slot = vm_resolve_global_slot_cached(vm, frame.function, name_idx);
                if (slot >= 0 && slot <= 0xff) {
                    // Inline cache: rewrite this call to use the resolved global slot directly.
                    // opcode at (ip-3), operand at (ip-2). Arg count stays at (ip-1).
                    code[frame.ip - 3] = OP_CALL_GLOBAL_SLOT;
                    code[frame.ip - 2] = (uint8_t)slot;
                }
                if (slot >= 0 && slot < vm->global_count) {
                    callee = vm->globals[slot];
                } else {
                    const char* global_name = NULL;
                    if (vm_get_global_name_constant(vm, frame.function, name_idx, &global_name) != 0) {
                        return -1;
                    }
                    callee = vm_get_global(vm, global_name);
                }

                if (value_get_type(&callee) == VAL_NATIVE) {
                    ObjNative* native = value_get_native_obj(&callee);
                    vm->frames[vm->frame_count - 1] = frame;
                    if (native->arity != arg_count) {
                        vm_runtime_error(vm, "Wrong number of arguments");
                        return -1;
                    }

                    if (arg_count == 0) {
                        // Provide a slot for the builtin to write its result.
                        push_nil_count(vm, 1);
                        if (vm_native_invoke_failed(vm, native->invoke(vm, native))) {
                            break;
                        }
                        break;
                    }

                    if (vm_native_invoke_failed(vm, native->invoke(vm, native))) {
                        break;
                    }

                    // Clean up extra arg slots: builtin writes result at lowest arg position.
                    // Release dropped args so objects don't leak.
                    int base = vm->stack.count - arg_count;
                    for (int i = 1; i < arg_count; i++) {
                        value_free(&vm->stack.values[base + i]);
                    }
                    vm->stack.count -= (arg_count - 1);
                    break;
                }

                if (value_get_type(&callee) != VAL_FUNCTION) {
                    vm_runtime_error(vm, "Can only call functions");
                    return -1;
                }

                ObjFunction* called_func = value_get_function_obj(&callee);
                if (!called_func) {
                    vm_runtime_error(vm, "Null function");
                    return -1;
                }

                if (called_func->is_async) {
                    if (vm_spawn_async_call(vm, called_func, args_base, arg_count, false) != 0) {
                        return -1;
                    }
                    break;
                }

                jit_record_function_entry(vm, called_func);
                bool handled = false;
                int compiled_rc =
                    vm_try_call_compiled_entry(vm, called_func, args_base, arg_count, &frame, &chunk, &code, &handled);
                if (compiled_rc != 0) {
                    return -1;
                }
                if (handled) {
                    break;
                }

                if (called_func->param_count != arg_count) {
                    vm_runtime_error(vm, "Wrong number of arguments");
                    return -1;
                }
                if (vm->frame_count + 1 > vm->config.max_call_depth) {
                    vm_runtime_error(vm, "Stack overflow: maximum call depth exceeded");
                    return -1;
                }
                if (called_func->local_count < arg_count) {
                    vm_runtime_error(vm, "Function local slots less than arguments");
                    return -1;
                }

                // Save the caller frame before switching.
                vm->frames[vm->frame_count - 1] = frame;

                // Allocate remaining locals; parameters already occupy slots 0..arg_count-1.
                push_nil_count(vm, called_func->local_count - arg_count);

                CallFrame new_frame;
                new_frame.function = called_func;
                new_frame.ip = 0;
                new_frame.slots_start = args_base;
                new_frame.slots_count = called_func->local_count;
                new_frame.defers = NULL;
                new_frame.defer_count = 0;
                new_frame.defer_capacity = 0;
                new_frame.is_async_root = false;
                new_frame.async_result_future = NULL;
                new_frame.panic_unwinding = false;
                new_frame.panic_message = NULL;
                if (!vm_bind_captures_to_frame(vm, &new_frame, called_func)) {
                    return -1;
                }

                vm_frame_reserve(vm, 1);
                vm->frames[vm->frame_count++] = new_frame;
                vm->current_call_depth = vm->frame_count;

                frame = new_frame;
                chunk = &frame.function->chunk;
                code = chunk->code;
                break;
            }

            case OP_CALL_GLOBAL16: {
                VM_ENSURE_IP_BYTES(3);
                int name_idx = ((int)code[frame.ip] << 8) | (int)code[frame.ip + 1];
                frame.ip += 2;
                uint8_t arg_count = code[frame.ip++];

                int args_base = vm->stack.count - arg_count;
                if (args_base < 0) {
                    vm_runtime_error(vm, "Stack underflow in call");
                    return -1;
                }

                Value callee;
                int slot = vm_resolve_global_slot_cached(vm, frame.function, name_idx);
                if (slot >= 0 && slot < vm->global_count) {
                    callee = vm->globals[slot];
                } else {
                    const char* global_name = NULL;
                    if (vm_get_global_name_constant(vm, frame.function, name_idx, &global_name) != 0) {
                        return -1;
                    }
                    callee = vm_get_global(vm, global_name);
                }

                if (value_get_type(&callee) == VAL_NATIVE) {
                    ObjNative* native = value_get_native_obj(&callee);
                    vm->frames[vm->frame_count - 1] = frame;
                    if (native->arity != arg_count) {
                        vm_runtime_error(vm, "Wrong number of arguments");
                        return -1;
                    }

                    if (arg_count == 0) {
                        // Provide a slot for the builtin to write its result.
                        push_nil_count(vm, 1);
                        if (vm_native_invoke_failed(vm, native->invoke(vm, native))) {
                            break;
                        }
                        break;
                    }

                    if (vm_native_invoke_failed(vm, native->invoke(vm, native))) {
                        break;
                    }

                    // Clean up extra arg slots: builtin writes result at lowest arg position.
                    // Release dropped args so objects don't leak.
                    int base = vm->stack.count - arg_count;
                    for (int i = 1; i < arg_count; i++) {
                        value_free(&vm->stack.values[base + i]);
                    }
                    vm->stack.count -= (arg_count - 1);
                    break;
                }

                if (value_get_type(&callee) != VAL_FUNCTION) {
                    vm_runtime_error(vm, "Can only call functions");
                    return -1;
                }

                ObjFunction* called_func = value_get_function_obj(&callee);
                if (!called_func) {
                    vm_runtime_error(vm, "Null function");
                    return -1;
                }

                if (called_func->is_async) {
                    if (vm_spawn_async_call(vm, called_func, args_base, arg_count, false) != 0) {
                        return -1;
                    }
                    break;
                }

                jit_record_function_entry(vm, called_func);
                bool handled = false;
                int compiled_rc =
                    vm_try_call_compiled_entry(vm, called_func, args_base, arg_count, &frame, &chunk, &code, &handled);
                if (compiled_rc != 0) {
                    return -1;
                }
                if (handled) {
                    break;
                }

                if (called_func->param_count != arg_count) {
                    vm_runtime_error(vm, "Wrong number of arguments");
                    return -1;
                }
                if (vm->frame_count + 1 > vm->config.max_call_depth) {
                    vm_runtime_error(vm, "Stack overflow: maximum call depth exceeded");
                    return -1;
                }
                if (called_func->local_count < arg_count) {
                    vm_runtime_error(vm, "Function local slots less than arguments");
                    return -1;
                }

                // Save the caller frame before switching.
                vm->frames[vm->frame_count - 1] = frame;

                // Allocate remaining locals; parameters already occupy slots 0..arg_count-1.
                push_nil_count(vm, called_func->local_count - arg_count);

                CallFrame new_frame;
                new_frame.function = called_func;
                new_frame.ip = 0;
                new_frame.slots_start = args_base;
                new_frame.slots_count = called_func->local_count;
                new_frame.defers = NULL;
                new_frame.defer_count = 0;
                new_frame.defer_capacity = 0;
                new_frame.is_async_root = false;
                new_frame.async_result_future = NULL;
                new_frame.panic_unwinding = false;
                new_frame.panic_message = NULL;
                if (!vm_bind_captures_to_frame(vm, &new_frame, called_func)) {
                    return -1;
                }

                vm_frame_reserve(vm, 1);
                vm->frames[vm->frame_count++] = new_frame;
                vm->current_call_depth = vm->frame_count;

                frame = new_frame;
                chunk = &frame.function->chunk;
                code = chunk->code;
                break;
            }

            case OP_CALL_GLOBAL_SLOT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t slot = code[frame.ip++];
                uint8_t arg_count = code[frame.ip++];

                int args_base = vm->stack.count - arg_count;
                if (args_base < 0) {
                    vm_runtime_error(vm, "Stack underflow in call");
                    return -1;
                }

                if (slot >= vm->global_count) {
                    vm_runtime_error(vm, "Global slot out of bounds");
                    return -1;
                }

                Value callee = vm->globals[slot];

                if (value_get_type(&callee) == VAL_NATIVE) {
                    ObjNative* native = value_get_native_obj(&callee);
                    vm->frames[vm->frame_count - 1] = frame;
                    if (native->arity != arg_count) {
                        vm_runtime_error(vm, "Wrong number of arguments");
                        return -1;
                    }

                    if (arg_count == 0) {
                        // Provide a slot for the builtin to write its result.
                        push_nil_count(vm, 1);
                        if (vm_native_invoke_failed(vm, native->invoke(vm, native))) {
                            break;
                        }
                        break;
                    }

                    if (vm_native_invoke_failed(vm, native->invoke(vm, native))) {
                        break;
                    }

                    // Clean up extra arg slots: builtin writes result at lowest arg position.
                    // Release dropped args so objects don't leak.
                    int base = vm->stack.count - arg_count;
                    for (int i = 1; i < arg_count; i++) {
                        value_free(&vm->stack.values[base + i]);
                    }
                    vm->stack.count -= (arg_count - 1);
                    break;
                }

                if (value_get_type(&callee) != VAL_FUNCTION) {
                    vm_runtime_error(vm, "Can only call functions");
                    return -1;
                }

                ObjFunction* called_func = value_get_function_obj(&callee);
                if (!called_func) {
                    vm_runtime_error(vm, "Null function");
                    return -1;
                }

                if (called_func->is_async) {
                    if (vm_spawn_async_call(vm, called_func, args_base, arg_count, false) != 0) {
                        return -1;
                    }
                    break;
                }

                jit_record_function_entry(vm, called_func);
                bool handled = false;
                int compiled_rc =
                    vm_try_call_compiled_entry(vm, called_func, args_base, arg_count, &frame, &chunk, &code, &handled);
                if (compiled_rc != 0) {
                    return -1;
                }
                if (handled) {
                    break;
                }

                if (called_func->param_count != arg_count) {
                    vm_runtime_error(vm, "Wrong number of arguments");
                    return -1;
                }
                if (vm->frame_count + 1 > vm->config.max_call_depth) {
                    vm_runtime_error(vm, "Stack overflow: maximum call depth exceeded");
                    return -1;
                }
                if (called_func->local_count < arg_count) {
                    vm_runtime_error(vm, "Function local slots less than arguments");
                    return -1;
                }

                // Save the caller frame before switching.
                vm->frames[vm->frame_count - 1] = frame;

                // Allocate remaining locals; parameters already occupy slots 0..arg_count-1.
                push_nil_count(vm, called_func->local_count - arg_count);

                CallFrame new_frame;
                new_frame.function = called_func;
                new_frame.ip = 0;
                new_frame.slots_start = args_base;
                new_frame.slots_count = called_func->local_count;
                new_frame.defers = NULL;
                new_frame.defer_count = 0;
                new_frame.defer_capacity = 0;
                new_frame.is_async_root = false;
                new_frame.async_result_future = NULL;
                new_frame.panic_unwinding = false;
                new_frame.panic_message = NULL;
                if (!vm_bind_captures_to_frame(vm, &new_frame, called_func)) {
                    return -1;
                }

                vm_frame_reserve(vm, 1);
                vm->frames[vm->frame_count++] = new_frame;
                vm->current_call_depth = vm->frame_count;

                frame = new_frame;
                chunk = &frame.function->chunk;
                code = chunk->code;
                break;
            }

            case OP_DEFER: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t arg_count = code[frame.ip++];

                int callee_pos = vm->stack.count - 1 - arg_count;
                if (callee_pos < 0) {
                    vm_runtime_error(vm, "Stack underflow in defer");
                    return -1;
                }

                DeferredCall dc;
                dc.arg_count = (int)arg_count;
                dc.args = dc.arg_count > 0 ? (Value*)safe_malloc((size_t)dc.arg_count * sizeof(Value)) : NULL;

                for (int i = dc.arg_count - 1; i >= 0; i--) {
                    dc.args[i] = pop(vm);
                }
                dc.callee = pop(vm);

                call_frame_defer_reserve(&frame, 1);
                frame.defers[frame.defer_count++] = dc;
                break;
            }

            case OP_DEFER_HAS: {
                Value b;
                value_init_bool(&b, frame.defer_count > 0);
                push(vm, b);
                break;
            }

            case OP_DEFER_CALL: {
                if (frame.defer_count <= 0) {
                    vm_runtime_error(vm, "No deferred calls");
                    return -1;
                }

                DeferredCall dc = frame.defers[frame.defer_count - 1];
                frame.defer_count--;

                // Move callee + args back to stack and perform a normal call.
                push(vm, dc.callee);
                for (int i = 0; i < dc.arg_count; i++) {
                    push(vm, dc.args[i]);
                }
                if (dc.args) free(dc.args);

                uint8_t arg_count = (uint8_t)dc.arg_count;
                int callee_pos = vm->stack.count - 1 - arg_count;
                if (callee_pos < 0) {
                    vm_runtime_error(vm, "Stack underflow in call");
                    return -1;
                }
                Value callee = vm->stack.values[callee_pos];

                if (value_get_type(&callee) == VAL_NATIVE) {
                    ObjNative* native = value_get_native_obj(&callee);
                    vm->frames[vm->frame_count - 1] = frame;
                    if (native->arity != arg_count) {
                        vm_runtime_error(vm, "Wrong number of arguments");
                        return -1;
                    }
                    // Remove callee from under the args
                    for (int i = 0; i < arg_count; i++) {
                        vm->stack.values[callee_pos + i] = vm->stack.values[callee_pos + 1 + i];
                    }
                    vm->stack.count--;

                    if (arg_count == 0) {
                        Value nil;
                        value_init_nil(&nil);
                        push(vm, nil);
                        if (vm_native_invoke_failed(vm, native->invoke(vm, native))) {
                            value_free(&callee);
                            break;
                        }
                        value_free(&callee);
                        break;
                    }

                    if (vm_native_invoke_failed(vm, native->invoke(vm, native))) {
                        value_free(&callee);
                        break;
                    }

                    int args_base = vm->stack.count - arg_count;
                    for (int i = 1; i < arg_count; i++) {
                        value_free(&vm->stack.values[args_base + i]);
                    }
                    vm->stack.count -= (arg_count - 1);

                    value_free(&callee);
                    break;
                }

                if (value_get_type(&callee) != VAL_FUNCTION) {
                    vm_runtime_error(vm, "Can only call functions");
                    return -1;
                }

                ObjFunction* called_func = value_get_function_obj(&callee);
                if (!called_func) {
                    vm_runtime_error(vm, "Null function");
                    return -1;
                }

                if (called_func->param_count != arg_count) {
                    vm_runtime_error(vm, "Wrong number of arguments");
                    return -1;
                }

                if (vm->frame_count + 1 > vm->config.max_call_depth) {
                    vm_runtime_error(vm, "Stack overflow: maximum call depth exceeded");
                    return -1;
                }

                if (called_func->local_count < arg_count) {
                    vm_runtime_error(vm, "Function local slots less than arguments");
                    return -1;
                }

                if (called_func->is_async) {
                    if (vm_spawn_async_call(vm, called_func, callee_pos, arg_count, true) != 0) {
                        return -1;
                    }
                    break;
                }

                vm->frames[vm->frame_count - 1] = frame;

                // Remove callee from under the args (callee was pushed first).
                for (int i = 0; i < arg_count; i++) {
                    vm->stack.values[callee_pos + i] = vm->stack.values[callee_pos + 1 + i];
                }
                vm->stack.count--;
                value_free(&callee);

                push_nil_count(vm, called_func->local_count - arg_count);

                CallFrame new_frame;
                new_frame.function = called_func;
                new_frame.ip = 0;
                new_frame.slots_start = callee_pos;
                new_frame.slots_count = called_func->local_count;
                new_frame.defers = NULL;
                new_frame.defer_count = 0;
                new_frame.defer_capacity = 0;
                new_frame.is_async_root = false;
                new_frame.async_result_future = NULL;
                new_frame.panic_unwinding = false;
                new_frame.panic_message = NULL;
                if (!vm_bind_captures_to_frame(vm, &new_frame, called_func)) {
                    return -1;
                }
                jit_record_function_entry(vm, called_func);

                vm_frame_reserve(vm, 1);
                vm->frames[vm->frame_count++] = new_frame;
                vm->current_call_depth = vm->frame_count;

                frame = new_frame;
                chunk = &frame.function->chunk;
                code = chunk->code;
                break;
            }

            case OP_AWAIT: {
                if (vm->stack.count <= frame.slots_start) {
                    vm_runtime_error(vm, "Stack underflow in await");
                    return -1;
                }
                if (!frame.is_async_root || !frame.async_result_future) {
                    vm_runtime_error(vm, "await is only valid in async task frames");
                    return -1;
                }

                Value awaited_value = pop(vm);
                if (!value_is_future(&awaited_value)) {
                    value_free(&awaited_value);
                    vm_runtime_error(vm, "await expects Future<T>");
                    return -1;
                }

                ObjRecord* awaited_future = value_get_record_obj(&awaited_value);
                Value resolved;
                if (obj_future_try_get(awaited_future, &resolved)) {
                    value_free(&awaited_value);
                    push(vm, resolved);
                    break;
                }
                if (obj_future_is_panicked(awaited_future)) {
                    const char* panic_message = obj_future_get_panic_message(awaited_future);
                    value_free(&awaited_value);
                    vm_runtime_error(vm, panic_message ? panic_message : "Runtime error");
                    break;
                }

                int saved_count = vm->stack.count - frame.slots_start;
                AsyncTask* task = async_task_create(frame.function, saved_count);
                if (!task) {
                    value_free(&awaited_value);
                    vm_runtime_error(vm, "Failed to allocate async suspension");
                    return -1;
                }
                task->ip = frame.ip;
                task->slots_count = frame.slots_count;
                for (int i = 0; i < saved_count; i++) {
                    task->stack_values[i] = vm->stack.values[frame.slots_start + i];
                    if (!value_type_is_unmanaged(value_get_type(&task->stack_values[i]))) {
                        value_retain(&task->stack_values[i]);
                    }
                }
                task->defers = frame.defers;
                task->defer_count = frame.defer_count;
                task->defer_capacity = frame.defer_capacity;
                frame.defers = NULL;
                frame.defer_count = 0;
                frame.defer_capacity = 0;
                task->async_result_future = frame.async_result_future;
                frame.async_result_future = NULL;
                task->panic_unwinding = frame.panic_unwinding;
                task->panic_message = frame.panic_message;
                task->entry_profiled = true;
                frame.panic_unwinding = false;
                frame.panic_message = NULL;

                if (!vm_enqueue_future_waiter(vm, awaited_future, task)) {
                    value_free(&awaited_value);
                    async_task_free(task);
                    vm_runtime_error(vm, "Failed to register future waiter");
                    return -1;
                }

                value_free(&awaited_value);
                stack_pop_to(vm, frame.slots_start);
                vm->frame_count--;
                vm->current_call_depth = vm->frame_count;

                if (vm->frame_count == 0) {
                    chunk = NULL;
                    code = NULL;
                    continue;
                }

                frame = vm->frames[vm->frame_count - 1];
                chunk = &frame.function->chunk;
                code = chunk->code;
                break;
            }

            case OP_RET: {
                Value result = pop(vm);
                if (vm->error_occurred) {
                    value_free(&result);
                    return -1;
                }

                if (frame.panic_unwinding) {
                    value_free(&result);
                    if (vm_finish_async_panic_frame(vm, &frame, &chunk, &code) != 0) {
                        return -1;
                    }
                    continue;
                }

                callframe_free_defers(&frame);

                // Unwind stack for this frame (locals + temps).
                stack_pop_to(vm, frame.slots_start);

                vm->frame_count--;
                vm->current_call_depth = vm->frame_count;

                if (frame.is_async_root) {
                    ObjRecord* result_future = frame.async_result_future;
                    frame.async_result_future = NULL;
                    if (!result_future || !vm_future_complete(vm, result_future, result)) {
                        if (result_future) obj_record_release(result_future);
                        value_free(&result);
                        vm_runtime_error(vm, "Failed to resolve async future");
                        return -1;
                    }
                    obj_record_release(result_future);
                    value_free(&result);

                    if (vm->frame_count == 0) {
                        chunk = NULL;
                        code = NULL;
                        continue;
                    }

                    frame = vm->frames[vm->frame_count - 1];
                    chunk = &frame.function->chunk;
                    code = chunk->code;
                    break;
                }

                if (vm->frame_count == 0) {
                    vm->return_value = (Value*)safe_malloc(sizeof(Value));
                    *(vm->return_value) = result;
                    return 0;
                }

                frame = vm->frames[vm->frame_count - 1];
                chunk = &frame.function->chunk;
                code = chunk->code;

                push(vm, result);
                break;
            }

            case OP_ARRAY_NEW: {
                // Fix 13: Create empty array with capacity; elements are added via OP_ARRAY_PUSH
                VM_ENSURE_IP_BYTES(1);
                uint8_t count = code[frame.ip++];
                ObjArray* arr = obj_array_create(vm, count > 0 ? count : 4);
                // Don't set arr->count or pop - elements will be pushed individually
                Value val;
                value_init_array(&val, arr);
                push(vm, val);
                break;
            }

            case OP_ARRAY_GET_LOCAL: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t array_slot = code[frame.ip++];

                Value idx_val = pop(vm);
                if (value_get_type(&idx_val) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    value_free(&idx_val);
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY && value_get_type(array_valp) != VAL_BYTES) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                if (value_get_type(array_valp) == VAL_BYTES) {
                    ObjBytes* bytes = value_get_bytes_obj(array_valp);
                    int64_t idx64 = value_get_int(&idx_val);
                    if (!bytes || idx64 < 0 || idx64 >= bytes->length) {
                        vm_runtime_error(vm, "Array index out of bounds");
                        return -1;
                    }

                    uint8_t* data = obj_bytes_data(bytes);
                    if (!data) {
                        vm_runtime_error(vm, "Invalid bytes buffer");
                        return -1;
                    }

                    Value result;
                    value_init_int(&result, (int64_t)data[(int)idx64]);
                    push(vm, result);
                    break;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                int64_t idx64 = value_get_int(&idx_val);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                int idx = (int)idx64;
                if (arr->kind == ARRAY_KIND_BOXED) {
                    Value result = arr->data.elements[idx];
                    value_retain(&result);
                    push(vm, result);
                } else {
                    Value result;
                    switch (arr->kind) {
                        case ARRAY_KIND_INT:
                            value_init_int(&result, arr->data.ints[idx]);
                            break;
                        case ARRAY_KIND_DOUBLE:
                            value_init_double(&result, arr->data.doubles[idx]);
                            break;
                        case ARRAY_KIND_BOOL:
                            value_init_bool(&result, arr->data.bools[idx] != 0);
                            break;
                        case ARRAY_KIND_BYTE:
                            value_init_int(&result, (int64_t)arr->data.bytes[idx]);
                            break;
                        default:
                            value_init_nil(&result);
                            break;
                    }
                    push(vm, result);
                }
                break;
            }

            case OP_ARRAY_GET_LOCAL_CONST: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx = code[frame.ip++];

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY && value_get_type(array_valp) != VAL_BYTES) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                if (value_get_type(array_valp) == VAL_BYTES) {
                    ObjBytes* bytes = value_get_bytes_obj(array_valp);
                    if (!bytes || idx >= bytes->length) {
                        vm_runtime_error(vm, "Array index out of bounds");
                        return -1;
                    }

                    uint8_t* data = obj_bytes_data(bytes);
                    if (!data) {
                        vm_runtime_error(vm, "Invalid bytes buffer");
                        return -1;
                    }

                    Value result;
                    value_init_int(&result, (int64_t)data[idx]);
                    push(vm, result);
                    break;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                if (idx >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                if (arr->kind == ARRAY_KIND_BOXED) {
                    Value result = arr->data.elements[idx];
                    value_retain(&result);
                    push(vm, result);
                } else {
                    Value result;
                    switch (arr->kind) {
                        case ARRAY_KIND_INT:
                            value_init_int(&result, arr->data.ints[idx]);
                            break;
                        case ARRAY_KIND_DOUBLE:
                            value_init_double(&result, arr->data.doubles[idx]);
                            break;
                        case ARRAY_KIND_BOOL:
                            value_init_bool(&result, arr->data.bools[idx] != 0);
                            break;
                        case ARRAY_KIND_BYTE:
                            value_init_int(&result, (int64_t)arr->data.bytes[idx]);
                            break;
                        default:
                            value_init_nil(&result);
                            break;
                    }
                    push(vm, result);
                }
                break;
            }

            case OP_ARRAY_GET_LOCAL_LOCAL: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_slot = code[frame.ip++];

                Value* idx_valp = &vm->stack.values[frame.slots_start + idx_slot];
                if (value_get_type(idx_valp) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY && value_get_type(array_valp) != VAL_BYTES) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                if (value_get_type(array_valp) == VAL_BYTES) {
                    ObjBytes* bytes = value_get_bytes_obj(array_valp);
                    int64_t idx64 = value_get_int(idx_valp);
                    if (!bytes || idx64 < 0 || idx64 >= bytes->length) {
                        vm_runtime_error(vm, "Array index out of bounds");
                        return -1;
                    }

                    uint8_t* data = obj_bytes_data(bytes);
                    if (!data) {
                        vm_runtime_error(vm, "Invalid bytes buffer");
                        return -1;
                    }

                    Value result;
                    value_init_int(&result, (int64_t)data[(int)idx64]);
                    push(vm, result);
                    break;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                int64_t idx64 = value_get_int(idx_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                int idx = (int)idx64;
                if (arr->kind == ARRAY_KIND_BOXED) {
                    Value result = arr->data.elements[idx];
                    value_retain(&result);
                    push(vm, result);
                } else {
                    Value result;
                    switch (arr->kind) {
                        case ARRAY_KIND_INT:
                            value_init_int(&result, arr->data.ints[idx]);
                            break;
                        case ARRAY_KIND_DOUBLE:
                            value_init_double(&result, arr->data.doubles[idx]);
                            break;
                        case ARRAY_KIND_BOOL:
                            value_init_bool(&result, arr->data.bools[idx] != 0);
                            break;
                        case ARRAY_KIND_BYTE:
                            value_init_int(&result, (int64_t)arr->data.bytes[idx]);
                            break;
                        default:
                            value_init_nil(&result);
                            break;
                    }
                    push(vm, result);
                }
                break;
            }

            case OP_ARRAY_GET_LOCAL_CONST_INT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx = code[frame.ip++];

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                if (idx >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                Value result;
                if (arr->kind == ARRAY_KIND_INT) {
                    value_set_type(&result, VAL_INT);
                    value_set_int(&result, arr->data.ints[idx]);
                } else if (arr->kind == ARRAY_KIND_BYTE) {
                    value_set_type(&result, VAL_INT);
                    value_set_int(&result, (int64_t)arr->data.bytes[idx]);
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    result = arr->data.elements[idx];
                    if (value_get_type(&result) != VAL_INT) {
                        vm_runtime_error(vm, "Array element must be integer");
                        return -1;
                    }
                } else {
                    vm_runtime_error(vm, "Array element must be integer");
                    return -1;
                }

                push(vm, result);
                break;
            }

            case OP_ARRAY_GET_LOCAL_LOCAL_INT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_slot = code[frame.ip++];

                Value* idx_valp = &vm->stack.values[frame.slots_start + idx_slot];
                if (value_get_type(idx_valp) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                int64_t idx64 = value_get_int(idx_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                int idx = (int)idx64;
                Value result;
                if (arr->kind == ARRAY_KIND_INT) {
                    value_set_type(&result, VAL_INT);
                    value_set_int(&result, arr->data.ints[idx]);
                } else if (arr->kind == ARRAY_KIND_BYTE) {
                    value_set_type(&result, VAL_INT);
                    value_set_int(&result, (int64_t)arr->data.bytes[idx]);
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    result = arr->data.elements[idx];
                    if (value_get_type(&result) != VAL_INT) {
                        vm_runtime_error(vm, "Array element must be integer");
                        return -1;
                    }
                } else {
                    vm_runtime_error(vm, "Array element must be integer");
                    return -1;
                }

                push(vm, result);
                break;
            }

            case OP_ARRAY_GET_LOCAL_CONST_DOUBLE: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx = code[frame.ip++];

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                if (idx >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                Value result;
                if (arr->kind == ARRAY_KIND_DOUBLE) {
                    value_set_type(&result, VAL_DOUBLE);
                    value_set_double(&result, arr->data.doubles[idx]);
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    result = arr->data.elements[idx];
                    if (value_get_type(&result) != VAL_DOUBLE) {
                        vm_runtime_error(vm, "Array element must be double");
                        return -1;
                    }
                } else {
                    vm_runtime_error(vm, "Array element must be double");
                    return -1;
                }

                push(vm, result);
                break;
            }

            case OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_slot = code[frame.ip++];

                Value* idx_valp = &vm->stack.values[frame.slots_start + idx_slot];
                if (value_get_type(idx_valp) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                int64_t idx64 = value_get_int(idx_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                int idx = (int)idx64;
                Value result;
                if (arr->kind == ARRAY_KIND_DOUBLE) {
                    value_set_type(&result, VAL_DOUBLE);
                    value_set_double(&result, arr->data.doubles[idx]);
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    result = arr->data.elements[idx];
                    if (value_get_type(&result) != VAL_DOUBLE) {
                        vm_runtime_error(vm, "Array element must be double");
                        return -1;
                    }
                } else {
                    vm_runtime_error(vm, "Array element must be double");
                    return -1;
                }

                push(vm, result);
                break;
            }

            case OP_ARRAY_GET_LOCAL_CONST_INT_TO_LOCAL:
            case OP_ARRAY_GET_LOCAL_LOCAL_INT_TO_LOCAL: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_or_slot = code[frame.ip++];
                uint8_t dst_slot = code[frame.ip++];
                frame.ip++; // padding (length-preserving superinstruction)

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);

                int64_t idx64 = 0;
                if (instruction == OP_ARRAY_GET_LOCAL_CONST_INT_TO_LOCAL) {
                    idx64 = (int64_t)idx_or_slot;
                } else {
                    Value* idx_valp = &vm->stack.values[frame.slots_start + idx_or_slot];
                    if (value_get_type(idx_valp) != VAL_INT) {
                        vm_runtime_error(vm, "Array index must be integer");
                        return -1;
                    }
                    idx64 = value_get_int(idx_valp);
                }

                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                int idx = (int)idx64;
                Value result;
                if (arr->kind == ARRAY_KIND_INT) {
                    value_set_type(&result, VAL_INT);
                    value_set_int(&result, arr->data.ints[idx]);
                } else if (arr->kind == ARRAY_KIND_BYTE) {
                    value_set_type(&result, VAL_INT);
                    value_set_int(&result, (int64_t)arr->data.bytes[idx]);
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    result = arr->data.elements[idx];
                    if (value_get_type(&result) != VAL_INT) {
                        vm_runtime_error(vm, "Array element must be integer");
                        return -1;
                    }
                } else {
                    vm_runtime_error(vm, "Array element must be integer");
                    return -1;
                }

                Value* dest = &vm->stack.values[frame.slots_start + dst_slot];
                if (!value_type_is_unmanaged(value_get_type(dest))) {
                    value_free(dest);
                }
                *dest = result;
                break;
            }

            case OP_ARRAY_GET_LOCAL_CONST_DOUBLE_TO_LOCAL:
            case OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE_TO_LOCAL: {
                VM_ENSURE_IP_BYTES(4);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_or_slot = code[frame.ip++];
                uint8_t dst_slot = code[frame.ip++];
                frame.ip++; // padding (length-preserving superinstruction)

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);

                int64_t idx64 = 0;
                if (instruction == OP_ARRAY_GET_LOCAL_CONST_DOUBLE_TO_LOCAL) {
                    idx64 = (int64_t)idx_or_slot;
                } else {
                    Value* idx_valp = &vm->stack.values[frame.slots_start + idx_or_slot];
                    if (value_get_type(idx_valp) != VAL_INT) {
                        vm_runtime_error(vm, "Array index must be integer");
                        return -1;
                    }
                    idx64 = value_get_int(idx_valp);
                }

                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                int idx = (int)idx64;
                Value result;
                if (arr->kind == ARRAY_KIND_DOUBLE) {
                    value_set_type(&result, VAL_DOUBLE);
                    value_set_double(&result, arr->data.doubles[idx]);
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    result = arr->data.elements[idx];
                    if (value_get_type(&result) != VAL_DOUBLE) {
                        vm_runtime_error(vm, "Array element must be double");
                        return -1;
                    }
                } else {
                    vm_runtime_error(vm, "Array element must be double");
                    return -1;
                }

                Value* dest = &vm->stack.values[frame.slots_start + dst_slot];
                if (!value_type_is_unmanaged(value_get_type(dest))) {
                    value_free(dest);
                }
                *dest = result;
                break;
            }

            case OP_MADD_LOCAL_ARRAY_LOCAL_INT: {
                VM_ENSURE_IP_BYTES(3);
                uint8_t acc_slot = code[frame.ip++];
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_slot = code[frame.ip++];

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* factor = &vm->stack.values[vm->stack.count - 1];
                if (value_get_type(factor) != VAL_INT) {
                    vm_runtime_error(vm, "Operand must be integer");
                    return -1;
                }

                Value* acc = &vm->stack.values[frame.slots_start + acc_slot];
                if (value_get_type(acc) != VAL_INT) {
                    vm_runtime_error(vm, "Accumulator must be integer");
                    return -1;
                }

                Value* idx_valp = &vm->stack.values[frame.slots_start + idx_slot];
                if (value_get_type(idx_valp) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                int64_t idx64 = value_get_int(idx_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                int idx = (int)idx64;
                int64_t elem;
                if (arr->kind == ARRAY_KIND_INT) {
                    elem = arr->data.ints[idx];
                } else if (arr->kind == ARRAY_KIND_BYTE) {
                    elem = (int64_t)arr->data.bytes[idx];
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    Value v = arr->data.elements[idx];
                    if (value_get_type(&v) != VAL_INT) {
                        vm_runtime_error(vm, "Array element must be integer");
                        return -1;
                    }
                    elem = value_get_int(&v);
                } else {
                    vm_runtime_error(vm, "Array element must be integer");
                    return -1;
                }

                value_set_int(acc, value_get_int(acc) + (value_get_int(factor) * elem));

                vm->stack.count--;
                break;
            }

            case OP_MADD_LOCAL_ARRAY_LOCAL_DOUBLE: {
                VM_ENSURE_IP_BYTES(3);
                uint8_t acc_slot = code[frame.ip++];
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_slot = code[frame.ip++];

                if (vm->stack.count <= 0) {
                    vm_runtime_error(vm, "Stack underflow");
                    return -1;
                }

                Value* factor = &vm->stack.values[vm->stack.count - 1];
                if (value_get_type(factor) != VAL_DOUBLE) {
                    vm_runtime_error(vm, "Operand must be double");
                    return -1;
                }

                Value* acc = &vm->stack.values[frame.slots_start + acc_slot];
                if (value_get_type(acc) != VAL_DOUBLE) {
                    vm_runtime_error(vm, "Accumulator must be double");
                    return -1;
                }

                Value* idx_valp = &vm->stack.values[frame.slots_start + idx_slot];
                if (value_get_type(idx_valp) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                int64_t idx64 = value_get_int(idx_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                int idx = (int)idx64;
                double elem;
                if (arr->kind == ARRAY_KIND_DOUBLE) {
                    elem = arr->data.doubles[idx];
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    Value v = arr->data.elements[idx];
                    if (value_get_type(&v) != VAL_DOUBLE) {
                        vm_runtime_error(vm, "Array element must be double");
                        return -1;
                    }
                    elem = value_get_double(&v);
                } else {
                    vm_runtime_error(vm, "Array element must be double");
                    return -1;
                }

                value_set_double(acc, value_get_double(acc) + (value_get_double(factor) * elem));

                vm->stack.count--;
                break;
            }

            case OP_ARRAY_BOUNDS_CHECK_LOCAL_CONST: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx = code[frame.ip++];

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                if (idx >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                break;
            }

            case OP_ARRAY_BOUNDS_CHECK_LOCAL_LOCAL: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_slot = code[frame.ip++];

                Value* idx_valp = &vm->stack.values[frame.slots_start + idx_slot];
                if (value_get_type(idx_valp) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                int64_t idx64 = value_get_int(idx_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                break;
            }

            case OP_ARRAY_GET_FIELD_LOCAL_CONST: {
                VM_ENSURE_IP_BYTES(3);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx = code[frame.ip++];
                uint8_t field_idx = code[frame.ip++];

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                if (idx >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                if (arr->kind != ARRAY_KIND_BOXED) {
                    vm_runtime_error(vm, "Cannot get field from non-record");
                    return -1;
                }

                Value* elem = &arr->data.elements[idx];
                if (value_get_type(elem) != VAL_RECORD || !value_get_record_obj(elem)) {
                    vm_runtime_error(vm, "Cannot get field from non-record");
                    return -1;
                }

                ObjRecord* record = value_get_record_obj(elem);
                if (field_idx >= record->field_count) {
                    vm_runtime_error(vm, "Record field index out of bounds");
                    return -1;
                }

                Value result = record->fields[field_idx];
                value_retain(&result);
                push(vm, result);
                break;
            }

            case OP_ARRAY_GET_FIELD_LOCAL_LOCAL: {
                VM_ENSURE_IP_BYTES(3);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_slot = code[frame.ip++];
                uint8_t field_idx = code[frame.ip++];

                Value* idx_valp = &vm->stack.values[frame.slots_start + idx_slot];
                if (value_get_type(idx_valp) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                int64_t idx64 = value_get_int(idx_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                int idx = (int)idx64;
                if (arr->kind != ARRAY_KIND_BOXED) {
                    vm_runtime_error(vm, "Cannot get field from non-record");
                    return -1;
                }

                Value* elem = &arr->data.elements[idx];
                if (value_get_type(elem) != VAL_RECORD || !value_get_record_obj(elem)) {
                    vm_runtime_error(vm, "Cannot get field from non-record");
                    return -1;
                }

                ObjRecord* record = value_get_record_obj(elem);
                if (field_idx >= record->field_count) {
                    vm_runtime_error(vm, "Record field index out of bounds");
                    return -1;
                }

                Value result = record->fields[field_idx];
                value_retain(&result);
                push(vm, result);
                break;
            }

            case OP_ARRAY_SET_FIELD_LOCAL_CONST: {
                VM_ENSURE_IP_BYTES(3);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx = code[frame.ip++];
                uint8_t field_idx = code[frame.ip++];

                Value value = pop(vm);

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    value_free(&value);
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                if (idx >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    value_free(&value);
                    return -1;
                }

                if (arr->kind != ARRAY_KIND_BOXED) {
                    vm_runtime_error(vm, "Cannot set field on non-record");
                    value_free(&value);
                    return -1;
                }

                Value* elem = &arr->data.elements[idx];
                if (value_get_type(elem) != VAL_RECORD || !value_get_record_obj(elem)) {
                    vm_runtime_error(vm, "Cannot set field on non-record");
                    value_free(&value);
                    return -1;
                }

                ObjRecord* record = value_get_record_obj(elem);
                if (field_idx >= record->field_count) {
                    vm_runtime_error(vm, "Record field index out of bounds");
                    value_free(&value);
                    return -1;
                }

                value_free(&record->fields[field_idx]);
                record->fields[field_idx] = value; // transfer ownership
                break;
            }

            case OP_ARRAY_SET_FIELD_LOCAL_LOCAL: {
                VM_ENSURE_IP_BYTES(3);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_slot = code[frame.ip++];
                uint8_t field_idx = code[frame.ip++];

                Value value = pop(vm);

                Value* idx_valp = &vm->stack.values[frame.slots_start + idx_slot];
                if (value_get_type(idx_valp) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    value_free(&value);
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    value_free(&value);
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                int64_t idx64 = value_get_int(idx_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    value_free(&value);
                    return -1;
                }

                int idx = (int)idx64;
                if (arr->kind != ARRAY_KIND_BOXED) {
                    vm_runtime_error(vm, "Cannot set field on non-record");
                    value_free(&value);
                    return -1;
                }

                Value* elem = &arr->data.elements[idx];
                if (value_get_type(elem) != VAL_RECORD || !value_get_record_obj(elem)) {
                    vm_runtime_error(vm, "Cannot set field on non-record");
                    value_free(&value);
                    return -1;
                }

                ObjRecord* record = value_get_record_obj(elem);
                if (field_idx >= record->field_count) {
                    vm_runtime_error(vm, "Record field index out of bounds");
                    value_free(&value);
                    return -1;
                }

                value_free(&record->fields[field_idx]);
                record->fields[field_idx] = value; // transfer ownership
                break;
            }

            case OP_ARRAY_GET: {
                Value idx_val = pop(vm);
                Value array_val = pop(vm);

                if (value_get_type(&array_val) != VAL_ARRAY && value_get_type(&array_val) != VAL_BYTES) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    value_free(&array_val);
                    value_free(&idx_val);
                    return -1;
                }

                if (value_get_type(&idx_val) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    value_free(&array_val);
                    value_free(&idx_val);
                    return -1;
                }

                int64_t idx64 = value_get_int(&idx_val);
                if (idx64 < 0 || idx64 > INT_MAX) {
                    vm_runtime_error(vm, "Array index out of range");
                    value_free(&array_val);
                    value_free(&idx_val);
                    return -1;
                }

                int idx = (int)idx64;
                if (value_get_type(&array_val) == VAL_BYTES) {
                    ObjBytes* bytes = value_get_bytes_obj(&array_val);
                    if (!bytes || idx < 0 || idx >= bytes->length) {
                        vm_runtime_error(vm, "Array index out of bounds");
                        value_free(&array_val);
                        value_free(&idx_val);
                        return -1;
                    }

                    uint8_t* data = obj_bytes_data(bytes);
                    if (!data) {
                        vm_runtime_error(vm, "Invalid bytes buffer");
                        value_free(&array_val);
                        value_free(&idx_val);
                        return -1;
                    }

                    Value result;
                    value_init_int(&result, (int64_t)data[idx]);
                    push(vm, result);
                    value_free(&array_val);
                    value_free(&idx_val);
                    break;
                }

                if (idx < 0 || idx >= value_get_array_obj(&array_val)->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    value_free(&array_val);
                    value_free(&idx_val);
                    return -1;
                }

                Value result;
                obj_array_get(value_get_array_obj(&array_val), idx, &result);
                value_retain(&result);
                push(vm, result);
                value_free(&array_val);
                value_free(&idx_val);
                break;
            }

            case OP_ARRAY_SET_LOCAL: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t array_slot = code[frame.ip++];

                Value value = pop(vm);
                Value idx_val = pop(vm);

                if (value_get_type(&idx_val) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    value_free(&idx_val);
                    value_free(&value);
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY && value_get_type(array_valp) != VAL_BYTES) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    value_free(&value);
                    return -1;
                }

                int64_t idx64 = value_get_int(&idx_val);
                if (value_get_type(array_valp) == VAL_BYTES) {
                    ObjBytes* bytes = value_get_bytes_obj(array_valp);
                    if (!bytes || idx64 < 0 || idx64 >= bytes->length) {
                        vm_runtime_error(vm, "Array index out of bounds");
                        value_free(&value);
                        return -1;
                    }

                    if (value_get_type(&value) != VAL_INT || value_get_int(&value) < 0 || value_get_int(&value) > 255) {
                        vm_runtime_error(vm, "Bytes values must be int in range 0..255");
                        value_free(&value);
                        return -1;
                    }

                    uint8_t* data = obj_bytes_data(bytes);
                    if (!data) {
                        vm_runtime_error(vm, "Invalid bytes buffer");
                        value_free(&value);
                        return -1;
                    }

                    data[(int)idx64] = (uint8_t)value_get_int(&value);
                    break;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    value_free(&value);
                    return -1;
                }

                int idx = (int)idx64;
                if (arr->kind == ARRAY_KIND_BOXED) {
                    value_free(&arr->data.elements[idx]);
                    arr->data.elements[idx] = value; // transfer ownership
                } else if (arr->kind == ARRAY_KIND_DOUBLE && value_get_type(&value) == VAL_DOUBLE) {
                    arr->data.doubles[idx] = value_get_double(&value);
                } else if (arr->kind == ARRAY_KIND_INT && value_get_type(&value) == VAL_INT) {
                    arr->data.ints[idx] = value_get_int(&value);
                } else if (arr->kind == ARRAY_KIND_BYTE && value_get_type(&value) == VAL_INT) {
                    if (value_get_int(&value) < 0 || value_get_int(&value) > 255) {
                        obj_array_convert_byte_to_int(arr);
                        arr->data.ints[idx] = value_get_int(&value);
                    } else {
                        arr->data.bytes[idx] = (uint8_t)value_get_int(&value);
                    }
                } else if (arr->kind == ARRAY_KIND_BOOL && value_get_type(&value) == VAL_BOOL) {
                    arr->data.bools[idx] = (uint8_t)(value_get_bool(&value) ? 1 : 0);
                } else {
                    obj_array_convert_to_boxed(arr);
                    value_free(&arr->data.elements[idx]);
                    arr->data.elements[idx] = value; // transfer ownership
                }
                break;
            }

            case OP_ARRAY_SET_LOCAL_CONST: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx = code[frame.ip++];

                Value value = pop(vm);

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY && value_get_type(array_valp) != VAL_BYTES) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    value_free(&value);
                    return -1;
                }

                if (value_get_type(array_valp) == VAL_BYTES) {
                    ObjBytes* bytes = value_get_bytes_obj(array_valp);
                    if (!bytes || idx >= bytes->length) {
                        vm_runtime_error(vm, "Array index out of bounds");
                        value_free(&value);
                        return -1;
                    }

                    if (value_get_type(&value) != VAL_INT || value_get_int(&value) < 0 || value_get_int(&value) > 255) {
                        vm_runtime_error(vm, "Bytes values must be int in range 0..255");
                        value_free(&value);
                        return -1;
                    }

                    uint8_t* data = obj_bytes_data(bytes);
                    if (!data) {
                        vm_runtime_error(vm, "Invalid bytes buffer");
                        value_free(&value);
                        return -1;
                    }

                    data[idx] = (uint8_t)value_get_int(&value);
                    break;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                if (idx >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    value_free(&value);
                    return -1;
                }

                if (arr->kind == ARRAY_KIND_BOXED) {
                    value_free(&arr->data.elements[idx]);
                    arr->data.elements[idx] = value; // transfer ownership
                } else if (arr->kind == ARRAY_KIND_DOUBLE && value_get_type(&value) == VAL_DOUBLE) {
                    arr->data.doubles[idx] = value_get_double(&value);
                } else if (arr->kind == ARRAY_KIND_INT && value_get_type(&value) == VAL_INT) {
                    arr->data.ints[idx] = value_get_int(&value);
                } else if (arr->kind == ARRAY_KIND_BYTE && value_get_type(&value) == VAL_INT) {
                    if (value_get_int(&value) < 0 || value_get_int(&value) > 255) {
                        obj_array_convert_byte_to_int(arr);
                        arr->data.ints[idx] = value_get_int(&value);
                    } else {
                        arr->data.bytes[idx] = (uint8_t)value_get_int(&value);
                    }
                } else if (arr->kind == ARRAY_KIND_BOOL && value_get_type(&value) == VAL_BOOL) {
                    arr->data.bools[idx] = (uint8_t)(value_get_bool(&value) ? 1 : 0);
                } else {
                    obj_array_convert_to_boxed(arr);
                    value_free(&arr->data.elements[idx]);
                    arr->data.elements[idx] = value; // transfer ownership
                }
                break;
            }

            case OP_ARRAY_SET_LOCAL_LOCAL: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_slot = code[frame.ip++];

                Value value = pop(vm);

                Value* idx_valp = &vm->stack.values[frame.slots_start + idx_slot];
                if (value_get_type(idx_valp) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    value_free(&value);
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY && value_get_type(array_valp) != VAL_BYTES) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    value_free(&value);
                    return -1;
                }

                int64_t idx64 = value_get_int(idx_valp);
                if (value_get_type(array_valp) == VAL_BYTES) {
                    ObjBytes* bytes = value_get_bytes_obj(array_valp);
                    if (!bytes || idx64 < 0 || idx64 >= bytes->length) {
                        vm_runtime_error(vm, "Array index out of bounds");
                        value_free(&value);
                        return -1;
                    }

                    if (value_get_type(&value) != VAL_INT || value_get_int(&value) < 0 || value_get_int(&value) > 255) {
                        vm_runtime_error(vm, "Bytes values must be int in range 0..255");
                        value_free(&value);
                        return -1;
                    }

                    uint8_t* data = obj_bytes_data(bytes);
                    if (!data) {
                        vm_runtime_error(vm, "Invalid bytes buffer");
                        value_free(&value);
                        return -1;
                    }

                    data[(int)idx64] = (uint8_t)value_get_int(&value);
                    break;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    value_free(&value);
                    return -1;
                }

                int idx = (int)idx64;
                if (arr->kind == ARRAY_KIND_BOXED) {
                    value_free(&arr->data.elements[idx]);
                    arr->data.elements[idx] = value; // transfer ownership
                } else if (arr->kind == ARRAY_KIND_DOUBLE && value_get_type(&value) == VAL_DOUBLE) {
                    arr->data.doubles[idx] = value_get_double(&value);
                } else if (arr->kind == ARRAY_KIND_INT && value_get_type(&value) == VAL_INT) {
                    arr->data.ints[idx] = value_get_int(&value);
                } else if (arr->kind == ARRAY_KIND_BYTE && value_get_type(&value) == VAL_INT) {
                    if (value_get_int(&value) < 0 || value_get_int(&value) > 255) {
                        obj_array_convert_byte_to_int(arr);
                        arr->data.ints[idx] = value_get_int(&value);
                    } else {
                        arr->data.bytes[idx] = (uint8_t)value_get_int(&value);
                    }
                } else if (arr->kind == ARRAY_KIND_BOOL && value_get_type(&value) == VAL_BOOL) {
                    arr->data.bools[idx] = (uint8_t)(value_get_bool(&value) ? 1 : 0);
                } else {
                    obj_array_convert_to_boxed(arr);
                    value_free(&arr->data.elements[idx]);
                    arr->data.elements[idx] = value; // transfer ownership
                }
                break;
            }

            case OP_ARRAY_SET_LOCAL_CONST_INT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx = code[frame.ip++];

                Value value = pop(vm);
                if (value_get_type(&value) != VAL_INT) {
                    vm_runtime_error(vm, "Array element must be integer");
                    value_free(&value);
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                if (idx >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                if (arr->kind == ARRAY_KIND_INT) {
                    arr->data.ints[idx] = value_get_int(&value);
                } else if (arr->kind == ARRAY_KIND_BYTE) {
                    if (value_get_int(&value) < 0 || value_get_int(&value) > 255) {
                        obj_array_convert_byte_to_int(arr);
                        arr->data.ints[idx] = value_get_int(&value);
                    } else {
                        arr->data.bytes[idx] = (uint8_t)value_get_int(&value);
                    }
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    Value* slotp = &arr->data.elements[idx];
                    if (!value_type_is_unmanaged(value_get_type(slotp))) {
                        value_free(slotp);
                    }
                    *slotp = value; // transfer ownership (unmanaged)
                } else {
                    vm_runtime_error(vm, "Array element must be integer");
                    return -1;
                }
                break;
            }

            case OP_ARRAY_SET_LOCAL_LOCAL_INT: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_slot = code[frame.ip++];

                Value value = pop(vm);
                if (value_get_type(&value) != VAL_INT) {
                    vm_runtime_error(vm, "Array element must be integer");
                    value_free(&value);
                    return -1;
                }

                Value* idx_valp = &vm->stack.values[frame.slots_start + idx_slot];
                if (value_get_type(idx_valp) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                int64_t idx64 = value_get_int(idx_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                int idx = (int)idx64;
                if (arr->kind == ARRAY_KIND_INT) {
                    arr->data.ints[idx] = value_get_int(&value);
                } else if (arr->kind == ARRAY_KIND_BYTE) {
                    if (value_get_int(&value) < 0 || value_get_int(&value) > 255) {
                        obj_array_convert_byte_to_int(arr);
                        arr->data.ints[idx] = value_get_int(&value);
                    } else {
                        arr->data.bytes[idx] = (uint8_t)value_get_int(&value);
                    }
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    Value* slotp = &arr->data.elements[idx];
                    if (!value_type_is_unmanaged(value_get_type(slotp))) {
                        value_free(slotp);
                    }
                    *slotp = value; // transfer ownership (unmanaged)
                } else {
                    vm_runtime_error(vm, "Array element must be integer");
                    return -1;
                }
                break;
            }

            case OP_ARRAY_SET_LOCAL_CONST_DOUBLE: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx = code[frame.ip++];

                Value value = pop(vm);
                if (value_get_type(&value) != VAL_DOUBLE) {
                    vm_runtime_error(vm, "Array element must be double");
                    value_free(&value);
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                if (idx >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                if (arr->kind == ARRAY_KIND_DOUBLE) {
                    arr->data.doubles[idx] = value_get_double(&value);
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    Value* slotp = &arr->data.elements[idx];
                    if (!value_type_is_unmanaged(value_get_type(slotp))) {
                        value_free(slotp);
                    }
                    *slotp = value; // transfer ownership (unmanaged)
                } else {
                    vm_runtime_error(vm, "Array element must be double");
                    return -1;
                }
                break;
            }

            case OP_ARRAY_SET_LOCAL_LOCAL_DOUBLE: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t array_slot = code[frame.ip++];
                uint8_t idx_slot = code[frame.ip++];

                Value value = pop(vm);
                if (value_get_type(&value) != VAL_DOUBLE) {
                    vm_runtime_error(vm, "Array element must be double");
                    value_free(&value);
                    return -1;
                }

                Value* idx_valp = &vm->stack.values[frame.slots_start + idx_slot];
                if (value_get_type(idx_valp) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    return -1;
                }

                Value* array_valp = &vm->stack.values[frame.slots_start + array_slot];
                if (value_get_type(array_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(array_valp);
                int64_t idx64 = value_get_int(idx_valp);
                if (idx64 < 0 || idx64 >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    return -1;
                }

                int idx = (int)idx64;
                if (arr->kind == ARRAY_KIND_DOUBLE) {
                    arr->data.doubles[idx] = value_get_double(&value);
                } else if (arr->kind == ARRAY_KIND_BOXED) {
                    Value* slotp = &arr->data.elements[idx];
                    if (!value_type_is_unmanaged(value_get_type(slotp))) {
                        value_free(slotp);
                    }
                    *slotp = value; // transfer ownership (unmanaged)
                } else {
                    vm_runtime_error(vm, "Array element must be double");
                    return -1;
                }
                break;
            }

            case OP_ARRAY_COPY: {
                // Stack convention: [..., dst, src] (src on top). Consumes both args.
                Value src_val = pop(vm);
                Value dst_val = pop(vm);

                if (value_get_type(&dst_val) != VAL_ARRAY || value_get_type(&src_val) != VAL_ARRAY) {
                    vm_runtime_error(vm, "copyInto expects two arrays");
                    value_free(&dst_val);
                    value_free(&src_val);
                    return -1;
                }

                ObjArray* dst = value_get_array_obj(&dst_val);
                ObjArray* src = value_get_array_obj(&src_val);
                if (dst->count != src->count) {
                    vm_runtime_error(vm, "copyInto expects arrays of equal length");
                    value_free(&dst_val);
                    value_free(&src_val);
                    return -1;
                }

                int count = src->count;
                if (src->kind != dst->kind) {
                    if (src->kind != ARRAY_KIND_BOXED) obj_array_convert_to_boxed(src);
                    if (dst->kind != ARRAY_KIND_BOXED) obj_array_convert_to_boxed(dst);
                }

                if (src->kind != ARRAY_KIND_BOXED) {
                    switch (src->kind) {
                        case ARRAY_KIND_INT:
                            memcpy(dst->data.ints, src->data.ints, (size_t)count * sizeof(int64_t));
                            break;
                        case ARRAY_KIND_DOUBLE:
                            memcpy(dst->data.doubles, src->data.doubles, (size_t)count * sizeof(double));
                            break;
                        case ARRAY_KIND_BOOL:
                            memcpy(dst->data.bools, src->data.bools, (size_t)count * sizeof(uint8_t));
                            break;
                        case ARRAY_KIND_BYTE:
                            memcpy(dst->data.bytes, src->data.bytes, (size_t)count * sizeof(uint8_t));
                            break;
                        default:
                            break;
                    }
                } else if (count > 0 &&
                           value_type_is_unmanaged(value_get_type(&src->data.elements[0])) &&
                           value_type_is_unmanaged(value_get_type(&dst->data.elements[0]))) {
                    memcpy(dst->data.elements, src->data.elements, (size_t)count * sizeof(Value));
                } else {
                    for (int i = 0; i < count; i++) {
                        Value elem = src->data.elements[i];
                        value_retain(&elem);
                        value_free(&dst->data.elements[i]);
                        dst->data.elements[i] = elem;
                    }
                }

                // Release stack references.
                value_free(&dst_val);
                value_free(&src_val);
                break;
            }

            case OP_ARRAY_COPY_LOCAL_LOCAL: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t dst_slot = code[frame.ip++];
                uint8_t src_slot = code[frame.ip++];

                Value* dst_valp = &vm->stack.values[frame.slots_start + dst_slot];
                Value* src_valp = &vm->stack.values[frame.slots_start + src_slot];

                if (value_get_type(dst_valp) != VAL_ARRAY || value_get_type(src_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "copyInto expects two arrays");
                    return -1;
                }

                ObjArray* dst = value_get_array_obj(dst_valp);
                ObjArray* src = value_get_array_obj(src_valp);
                if (dst->count != src->count) {
                    vm_runtime_error(vm, "copyInto expects arrays of equal length");
                    return -1;
                }

                int count = src->count;
                if (src->kind != dst->kind) {
                    if (src->kind != ARRAY_KIND_BOXED) obj_array_convert_to_boxed(src);
                    if (dst->kind != ARRAY_KIND_BOXED) obj_array_convert_to_boxed(dst);
                }

                if (src->kind != ARRAY_KIND_BOXED) {
                    switch (src->kind) {
                        case ARRAY_KIND_INT:
                            memcpy(dst->data.ints, src->data.ints, (size_t)count * sizeof(int64_t));
                            break;
                        case ARRAY_KIND_DOUBLE:
                            memcpy(dst->data.doubles, src->data.doubles, (size_t)count * sizeof(double));
                            break;
                        case ARRAY_KIND_BOOL:
                            memcpy(dst->data.bools, src->data.bools, (size_t)count * sizeof(uint8_t));
                            break;
                        case ARRAY_KIND_BYTE:
                            memcpy(dst->data.bytes, src->data.bytes, (size_t)count * sizeof(uint8_t));
                            break;
                        default:
                            break;
                    }
                } else if (count > 0 &&
                           value_type_is_unmanaged(value_get_type(&src->data.elements[0])) &&
                           value_type_is_unmanaged(value_get_type(&dst->data.elements[0]))) {
                    memcpy(dst->data.elements, src->data.elements, (size_t)count * sizeof(Value));
                } else {
                    for (int i = 0; i < count; i++) {
                        Value elem = src->data.elements[i];
                        value_retain(&elem);
                        value_free(&dst->data.elements[i]);
                        dst->data.elements[i] = elem;
                    }
                }
                break;
            }

            case OP_ARRAY_REVERSE_PREFIX: {
                // Stack convention: [..., arr, hi] (hi on top). Consumes both args.
                Value hi_val = pop(vm);
                Value arr_val = pop(vm);

                if (value_get_type(&arr_val) != VAL_ARRAY) {
                    vm_runtime_error(vm, "reversePrefix expects an array");
                    value_free(&arr_val);
                    value_free(&hi_val);
                    return -1;
                }
                if (value_get_type(&hi_val) != VAL_INT) {
                    vm_runtime_error(vm, "reversePrefix hi must be int");
                    value_free(&arr_val);
                    value_free(&hi_val);
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(&arr_val);
                int64_t hi64 = value_get_int(&hi_val);
                if (hi64 < 0 || hi64 >= arr->count) {
                    vm_runtime_error(vm, "reversePrefix hi out of bounds");
                    value_free(&arr_val);
                    value_free(&hi_val);
                    return -1;
                }

                int lo = 0;
                int hi = (int)hi64;
                while (lo < hi) {
                    if (arr->kind == ARRAY_KIND_BOXED) {
                        Value tmp = arr->data.elements[lo];
                        arr->data.elements[lo] = arr->data.elements[hi];
                        arr->data.elements[hi] = tmp;
                    } else if (arr->kind == ARRAY_KIND_INT) {
                        int64_t tmp = arr->data.ints[lo];
                        arr->data.ints[lo] = arr->data.ints[hi];
                        arr->data.ints[hi] = tmp;
                    } else if (arr->kind == ARRAY_KIND_BYTE) {
                        uint8_t tmp = arr->data.bytes[lo];
                        arr->data.bytes[lo] = arr->data.bytes[hi];
                        arr->data.bytes[hi] = tmp;
                    } else if (arr->kind == ARRAY_KIND_DOUBLE) {
                        double tmp = arr->data.doubles[lo];
                        arr->data.doubles[lo] = arr->data.doubles[hi];
                        arr->data.doubles[hi] = tmp;
                    } else if (arr->kind == ARRAY_KIND_BOOL) {
                        uint8_t tmp = arr->data.bools[lo];
                        arr->data.bools[lo] = arr->data.bools[hi];
                        arr->data.bools[hi] = tmp;
                    }
                    lo++;
                    hi--;
                }

                value_free(&arr_val);
                value_free(&hi_val);
                break;
            }

            case OP_ARRAY_REVERSE_PREFIX_LOCAL_LOCAL: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t arr_slot = code[frame.ip++];
                uint8_t hi_slot = code[frame.ip++];

                Value* arr_valp = &vm->stack.values[frame.slots_start + arr_slot];
                Value* hi_valp = &vm->stack.values[frame.slots_start + hi_slot];

                if (value_get_type(arr_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "reversePrefix expects an array");
                    return -1;
                }
                if (value_get_type(hi_valp) != VAL_INT) {
                    vm_runtime_error(vm, "reversePrefix hi must be int");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(arr_valp);
                int64_t hi64 = value_get_int(hi_valp);
                if (hi64 < 0 || hi64 >= arr->count) {
                    vm_runtime_error(vm, "reversePrefix hi out of bounds");
                    return -1;
                }

                int lo = 0;
                int hi = (int)hi64;
                while (lo < hi) {
                    if (arr->kind == ARRAY_KIND_BOXED) {
                        Value tmp = arr->data.elements[lo];
                        arr->data.elements[lo] = arr->data.elements[hi];
                        arr->data.elements[hi] = tmp;
                    } else if (arr->kind == ARRAY_KIND_INT) {
                        int64_t tmp = arr->data.ints[lo];
                        arr->data.ints[lo] = arr->data.ints[hi];
                        arr->data.ints[hi] = tmp;
                    } else if (arr->kind == ARRAY_KIND_BYTE) {
                        uint8_t tmp = arr->data.bytes[lo];
                        arr->data.bytes[lo] = arr->data.bytes[hi];
                        arr->data.bytes[hi] = tmp;
                    } else if (arr->kind == ARRAY_KIND_DOUBLE) {
                        double tmp = arr->data.doubles[lo];
                        arr->data.doubles[lo] = arr->data.doubles[hi];
                        arr->data.doubles[hi] = tmp;
                    } else if (arr->kind == ARRAY_KIND_BOOL) {
                        uint8_t tmp = arr->data.bools[lo];
                        arr->data.bools[lo] = arr->data.bools[hi];
                        arr->data.bools[hi] = tmp;
                    }
                    lo++;
                    hi--;
                }
                break;
            }

            case OP_ARRAY_ROTATE_PREFIX_LEFT: {
                // Stack convention: [..., arr, hi] (hi on top). Consumes both args.
                Value hi_val = pop(vm);
                Value arr_val = pop(vm);

                if (value_get_type(&arr_val) != VAL_ARRAY) {
                    vm_runtime_error(vm, "rotatePrefixLeft expects an array");
                    value_free(&arr_val);
                    value_free(&hi_val);
                    return -1;
                }
                if (value_get_type(&hi_val) != VAL_INT) {
                    vm_runtime_error(vm, "rotatePrefixLeft hi must be int");
                    value_free(&arr_val);
                    value_free(&hi_val);
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(&arr_val);
                int64_t hi64 = value_get_int(&hi_val);
                if (hi64 < 0 || hi64 >= arr->count) {
                    vm_runtime_error(vm, "rotatePrefixLeft hi out of bounds");
                    value_free(&arr_val);
                    value_free(&hi_val);
                    return -1;
                }

                int hi = (int)hi64;
                if (hi > 0) {
                    if (arr->kind == ARRAY_KIND_BOXED) {
                        Value first = arr->data.elements[0];
                        for (int i = 0; i < hi; i++) {
                            arr->data.elements[i] = arr->data.elements[i + 1];
                        }
                        arr->data.elements[hi] = first;
                    } else if (arr->kind == ARRAY_KIND_INT) {
                        int64_t first = arr->data.ints[0];
                        for (int i = 0; i < hi; i++) {
                            arr->data.ints[i] = arr->data.ints[i + 1];
                        }
                        arr->data.ints[hi] = first;
                    } else if (arr->kind == ARRAY_KIND_BYTE) {
                        uint8_t first = arr->data.bytes[0];
                        for (int i = 0; i < hi; i++) {
                            arr->data.bytes[i] = arr->data.bytes[i + 1];
                        }
                        arr->data.bytes[hi] = first;
                    } else if (arr->kind == ARRAY_KIND_DOUBLE) {
                        double first = arr->data.doubles[0];
                        for (int i = 0; i < hi; i++) {
                            arr->data.doubles[i] = arr->data.doubles[i + 1];
                        }
                        arr->data.doubles[hi] = first;
                    } else if (arr->kind == ARRAY_KIND_BOOL) {
                        uint8_t first = arr->data.bools[0];
                        for (int i = 0; i < hi; i++) {
                            arr->data.bools[i] = arr->data.bools[i + 1];
                        }
                        arr->data.bools[hi] = first;
                    }
                }

                value_free(&arr_val);
                value_free(&hi_val);
                break;
            }

            case OP_ARRAY_ROTATE_PREFIX_LEFT_LOCAL_LOCAL: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t arr_slot = code[frame.ip++];
                uint8_t hi_slot = code[frame.ip++];

                Value* arr_valp = &vm->stack.values[frame.slots_start + arr_slot];
                Value* hi_valp = &vm->stack.values[frame.slots_start + hi_slot];

                if (value_get_type(arr_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "rotatePrefixLeft expects an array");
                    return -1;
                }
                if (value_get_type(hi_valp) != VAL_INT) {
                    vm_runtime_error(vm, "rotatePrefixLeft hi must be int");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(arr_valp);
                int64_t hi64 = value_get_int(hi_valp);
                if (hi64 < 0 || hi64 >= arr->count) {
                    vm_runtime_error(vm, "rotatePrefixLeft hi out of bounds");
                    return -1;
                }

                int hi = (int)hi64;
                if (hi > 0) {
                    if (arr->kind == ARRAY_KIND_BOXED) {
                        Value first = arr->data.elements[0];
                        for (int i = 0; i < hi; i++) {
                            arr->data.elements[i] = arr->data.elements[i + 1];
                        }
                        arr->data.elements[hi] = first;
                    } else if (arr->kind == ARRAY_KIND_INT) {
                        int64_t first = arr->data.ints[0];
                        for (int i = 0; i < hi; i++) {
                            arr->data.ints[i] = arr->data.ints[i + 1];
                        }
                        arr->data.ints[hi] = first;
                    } else if (arr->kind == ARRAY_KIND_BYTE) {
                        uint8_t first = arr->data.bytes[0];
                        for (int i = 0; i < hi; i++) {
                            arr->data.bytes[i] = arr->data.bytes[i + 1];
                        }
                        arr->data.bytes[hi] = first;
                    } else if (arr->kind == ARRAY_KIND_DOUBLE) {
                        double first = arr->data.doubles[0];
                        for (int i = 0; i < hi; i++) {
                            arr->data.doubles[i] = arr->data.doubles[i + 1];
                        }
                        arr->data.doubles[hi] = first;
                    } else if (arr->kind == ARRAY_KIND_BOOL) {
                        uint8_t first = arr->data.bools[0];
                        for (int i = 0; i < hi; i++) {
                            arr->data.bools[i] = arr->data.bools[i + 1];
                        }
                        arr->data.bools[hi] = first;
                    }
                }
                break;
            }

            case OP_ARRAY_ROTATE_PREFIX_RIGHT: {
                // Stack convention: [..., arr, hi] (hi on top). Consumes both args.
                Value hi_val = pop(vm);
                Value arr_val = pop(vm);

                if (value_get_type(&arr_val) != VAL_ARRAY) {
                    vm_runtime_error(vm, "rotatePrefixRight expects an array");
                    value_free(&arr_val);
                    value_free(&hi_val);
                    return -1;
                }
                if (value_get_type(&hi_val) != VAL_INT) {
                    vm_runtime_error(vm, "rotatePrefixRight hi must be int");
                    value_free(&arr_val);
                    value_free(&hi_val);
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(&arr_val);
                int64_t hi64 = value_get_int(&hi_val);
                if (hi64 < 0 || hi64 >= arr->count) {
                    vm_runtime_error(vm, "rotatePrefixRight hi out of bounds");
                    value_free(&arr_val);
                    value_free(&hi_val);
                    return -1;
                }

                int hi = (int)hi64;
                if (hi > 0) {
                    if (arr->kind == ARRAY_KIND_BOXED) {
                        Value last = arr->data.elements[hi];
                        for (int i = hi; i > 0; i--) {
                            arr->data.elements[i] = arr->data.elements[i - 1];
                        }
                        arr->data.elements[0] = last;
                    } else if (arr->kind == ARRAY_KIND_INT) {
                        int64_t last = arr->data.ints[hi];
                        for (int i = hi; i > 0; i--) {
                            arr->data.ints[i] = arr->data.ints[i - 1];
                        }
                        arr->data.ints[0] = last;
                    } else if (arr->kind == ARRAY_KIND_BYTE) {
                        uint8_t last = arr->data.bytes[hi];
                        for (int i = hi; i > 0; i--) {
                            arr->data.bytes[i] = arr->data.bytes[i - 1];
                        }
                        arr->data.bytes[0] = last;
                    } else if (arr->kind == ARRAY_KIND_DOUBLE) {
                        double last = arr->data.doubles[hi];
                        for (int i = hi; i > 0; i--) {
                            arr->data.doubles[i] = arr->data.doubles[i - 1];
                        }
                        arr->data.doubles[0] = last;
                    } else if (arr->kind == ARRAY_KIND_BOOL) {
                        uint8_t last = arr->data.bools[hi];
                        for (int i = hi; i > 0; i--) {
                            arr->data.bools[i] = arr->data.bools[i - 1];
                        }
                        arr->data.bools[0] = last;
                    }
                }

                value_free(&arr_val);
                value_free(&hi_val);
                break;
            }

            case OP_ARRAY_ROTATE_PREFIX_RIGHT_LOCAL_LOCAL: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t arr_slot = code[frame.ip++];
                uint8_t hi_slot = code[frame.ip++];

                Value* arr_valp = &vm->stack.values[frame.slots_start + arr_slot];
                Value* hi_valp = &vm->stack.values[frame.slots_start + hi_slot];

                if (value_get_type(arr_valp) != VAL_ARRAY) {
                    vm_runtime_error(vm, "rotatePrefixRight expects an array");
                    return -1;
                }
                if (value_get_type(hi_valp) != VAL_INT) {
                    vm_runtime_error(vm, "rotatePrefixRight hi must be int");
                    return -1;
                }

                ObjArray* arr = value_get_array_obj(arr_valp);
                int64_t hi64 = value_get_int(hi_valp);
                if (hi64 < 0 || hi64 >= arr->count) {
                    vm_runtime_error(vm, "rotatePrefixRight hi out of bounds");
                    return -1;
                }

                int hi = (int)hi64;
                if (hi > 0) {
                    if (arr->kind == ARRAY_KIND_BOXED) {
                        Value last = arr->data.elements[hi];
                        for (int i = hi; i > 0; i--) {
                            arr->data.elements[i] = arr->data.elements[i - 1];
                        }
                        arr->data.elements[0] = last;
                    } else if (arr->kind == ARRAY_KIND_INT) {
                        int64_t last = arr->data.ints[hi];
                        for (int i = hi; i > 0; i--) {
                            arr->data.ints[i] = arr->data.ints[i - 1];
                        }
                        arr->data.ints[0] = last;
                    } else if (arr->kind == ARRAY_KIND_BYTE) {
                        uint8_t last = arr->data.bytes[hi];
                        for (int i = hi; i > 0; i--) {
                            arr->data.bytes[i] = arr->data.bytes[i - 1];
                        }
                        arr->data.bytes[0] = last;
                    } else if (arr->kind == ARRAY_KIND_DOUBLE) {
                        double last = arr->data.doubles[hi];
                        for (int i = hi; i > 0; i--) {
                            arr->data.doubles[i] = arr->data.doubles[i - 1];
                        }
                        arr->data.doubles[0] = last;
                    } else if (arr->kind == ARRAY_KIND_BOOL) {
                        uint8_t last = arr->data.bools[hi];
                        for (int i = hi; i > 0; i--) {
                            arr->data.bools[i] = arr->data.bools[i - 1];
                        }
                        arr->data.bools[0] = last;
                    }
                }
                break;
            }

            case OP_ARRAY_SET: {
                Value value = pop(vm);
                Value idx_val = pop(vm);
                Value array_val = pop(vm);

                if (value_get_type(&array_val) != VAL_ARRAY && value_get_type(&array_val) != VAL_BYTES) {
                    vm_runtime_error(vm, "Cannot index non-array");
                    value_free(&array_val);
                    value_free(&idx_val);
                    value_free(&value);
                    return -1;
                }

                if (value_get_type(&idx_val) != VAL_INT) {
                    vm_runtime_error(vm, "Array index must be integer");
                    value_free(&array_val);
                    value_free(&idx_val);
                    value_free(&value);
                    return -1;
                }

                int64_t idx64 = value_get_int(&idx_val);
                if (idx64 < 0 || idx64 > INT_MAX) {
                    vm_runtime_error(vm, "Array index out of range");
                    value_free(&array_val);
                    value_free(&idx_val);
                    value_free(&value);
                    return -1;
                }

                int idx = (int)idx64;
                if (value_get_type(&array_val) == VAL_BYTES) {
                    ObjBytes* bytes = value_get_bytes_obj(&array_val);
                    if (!bytes || idx < 0 || idx >= bytes->length) {
                        vm_runtime_error(vm, "Array index out of bounds");
                        value_free(&array_val);
                        value_free(&idx_val);
                        value_free(&value);
                        return -1;
                    }

                    if (value_get_type(&value) != VAL_INT || value_get_int(&value) < 0 || value_get_int(&value) > 255) {
                        vm_runtime_error(vm, "Bytes values must be int in range 0..255");
                        value_free(&array_val);
                        value_free(&idx_val);
                        value_free(&value);
                        return -1;
                    }

                    uint8_t* data = obj_bytes_data(bytes);
                    if (!data) {
                        vm_runtime_error(vm, "Invalid bytes buffer");
                        value_free(&array_val);
                        value_free(&idx_val);
                        value_free(&value);
                        return -1;
                    }

                    data[idx] = (uint8_t)value_get_int(&value);
                    push(vm, value);
                    value_free(&array_val);
                    value_free(&idx_val);
                    break;
                }

                ObjArray* arr = value_get_array_obj(&array_val);
                if (idx < 0 || idx >= arr->count) {
                    vm_runtime_error(vm, "Array index out of bounds");
                    value_free(&array_val);
                    value_free(&idx_val);
                    value_free(&value);
                    return -1;
                }

                if (arr->kind == ARRAY_KIND_BOXED) {
                    value_free(&arr->data.elements[idx]);
                    Value stored = value;
                    value_retain(&stored);
                    arr->data.elements[idx] = stored;
                } else if (arr->kind == ARRAY_KIND_DOUBLE && value_get_type(&value) == VAL_DOUBLE) {
                    arr->data.doubles[idx] = value_get_double(&value);
                } else if (arr->kind == ARRAY_KIND_INT && value_get_type(&value) == VAL_INT) {
                    arr->data.ints[idx] = value_get_int(&value);
                } else if (arr->kind == ARRAY_KIND_BYTE && value_get_type(&value) == VAL_INT) {
                    if (value_get_int(&value) < 0 || value_get_int(&value) > 255) {
                        obj_array_convert_byte_to_int(arr);
                        arr->data.ints[idx] = value_get_int(&value);
                    } else {
                        arr->data.bytes[idx] = (uint8_t)value_get_int(&value);
                    }
                } else if (arr->kind == ARRAY_KIND_BOOL && value_get_type(&value) == VAL_BOOL) {
                    arr->data.bools[idx] = (uint8_t)(value_get_bool(&value) ? 1 : 0);
                } else {
                    obj_array_convert_to_boxed(arr);
                    value_free(&arr->data.elements[idx]);
                    Value stored = value;
                    value_retain(&stored);
                    arr->data.elements[idx] = stored;
                }
                push(vm, value);
                value_free(&array_val);
                value_free(&idx_val);
                break;
            }

            case OP_ARRAY_LEN_LOCAL: {
                VM_ENSURE_IP_BYTES(2);
                uint8_t slot = code[frame.ip++];
                // padding (length-preserving superinstruction)
                frame.ip++;

                Value* valp = &vm->stack.values[frame.slots_start + slot];
                Value result;
                if (value_get_type(valp) == VAL_ARRAY) {
                    value_init_int(&result, value_get_array_obj(valp)->count);
                } else if (value_get_type(valp) == VAL_BYTES) {
                    ObjBytes* bytes = value_get_bytes_obj(valp);
                    value_init_int(&result, bytes ? bytes->length : 0);
                } else if (value_get_type(valp) == VAL_STRING) {
                    value_init_int(&result, value_get_string_obj(valp)->length);
                } else {
                    value_init_int(&result, 0);
                }
                push(vm, result);
                break;
            }

            case OP_ARRAY_LEN: {
                Value array = pop(vm);

                Value result;
                if (value_get_type(&array) == VAL_ARRAY) {
                    value_init_int(&result, value_get_array_obj(&array)->count);
                } else if (value_get_type(&array) == VAL_BYTES) {
                    ObjBytes* bytes = value_get_bytes_obj(&array);
                    value_init_int(&result, bytes ? bytes->length : 0);
                } else if (value_get_type(&array) == VAL_STRING) {
                    value_init_int(&result, value_get_string_obj(&array)->length);
                } else {
                    value_init_int(&result, 0);
                }
                push(vm, result);
                value_free(&array);
                break;
            }

            case OP_ARRAY_PUSH: {
                Value val = pop(vm);
                Value array = peek(vm, 0);

                if (value_get_type(&array) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot push to non-array");
                    value_free(&val);
                    return -1;
                }

                obj_array_push(value_get_array_obj(&array), val);
                break;
            }

            case OP_ARRAY_POP: {
                Value array = peek(vm, 0);

                if (value_get_type(&array) != VAL_ARRAY) {
                    vm_runtime_error(vm, "Cannot pop from non-array");
                    return -1;
                }

                Value result;
                obj_array_pop(value_get_array_obj(&array), &result);
                push(vm, result);
                break;
            }

            case OP_STRING_LEN: {
                Value str = pop(vm);

                if (value_get_type(&str) != VAL_STRING) {
                    vm_runtime_error(vm, "Can only get length of string");
                    value_free(&str);
                    return -1;
                }

                Value result;
                value_init_int(&result, value_get_string_obj(&str)->length);
                push(vm, result);
                value_free(&str);
                break;
            }

            case OP_STRING_CONCAT: {
                Value b = pop(vm);
                Value a = pop(vm);
                Value result;

                if (value_get_type(&a) != VAL_STRING || value_get_type(&b) != VAL_STRING) {
                    vm_runtime_error(vm, "Can only concatenate strings");
                    value_free(&a);
                    value_free(&b);
                    return -1;
                }

                ObjString* a_str = value_get_string_obj(&a);
                ObjString* b_str = value_get_string_obj(&b);
                int len = a_str->length + b_str->length;
                if (len > vm->config.max_string_length) {
                    vm_runtime_error(vm, "String length exceeds maximum allowed");
                    value_free(&a);
                    value_free(&b);
                    return -1;
                }
                char* chars = (char*)safe_malloc(len + 1);
                memcpy(chars, a_str->chars, a_str->length);
                memcpy(chars + a_str->length, b_str->chars, b_str->length);
                chars[len] = '\0';
                value_init_string(&result, chars);
                free(chars);
                push(vm, result);
                value_free(&a);
                value_free(&b);
                break;
            }

            case OP_TYPEOF: {
                Value a = pop(vm);
                Value result;
                char* type_str;

                switch (value_get_type(&a)) {
                    case VAL_INT: type_str = "int"; break;
                    case VAL_BOOL: type_str = "bool"; break;
                    case VAL_DOUBLE: type_str = "double"; break;
                    case VAL_BIGINT: type_str = "bigint"; break;
                    case VAL_STRING: type_str = "string"; break;
                    case VAL_BYTES: type_str = "bytes"; break;
                    case VAL_ARRAY: type_str = "array"; break;
                    case VAL_NIL: type_str = "nil"; break;
                    case VAL_FUNCTION: type_str = "function"; break;
                    case VAL_NATIVE: type_str = "native"; break;
                    case VAL_RECORD: {
                        ObjRecord* record = value_get_record_obj(&a);
                        type_str = (record && record->type_name && record->type_name[0] != '\0')
                            ? record->type_name
                            : "record";
                        break;
                    }
                    case VAL_TUPLE: type_str = "tuple"; break;
                    case VAL_MAP: type_str = "map"; break;
                    case VAL_SET: type_str = "set"; break;
                    case VAL_SOCKET: type_str = "socket"; break;
                    case VAL_FILE: type_str = "file"; break;
                    default: type_str = "unknown"; break;
                }

                value_init_string(&result, type_str);
                push(vm, result);
                value_free(&a);
                break;
            }

            case OP_CAST_INT: {
                Value a = pop(vm);
                Value result;

                if (value_get_type(&a) == VAL_INT) {
                    value_init_int(&result, value_get_int(&a));
                } else if (value_get_type(&a) == VAL_BOOL) {
                    value_init_int(&result, value_get_bool(&a) ? 1 : 0);
                } else if (value_get_type(&a) == VAL_BIGINT) {
                    ObjBigInt* bigint = value_get_bigint_obj(&a);
                    int64_t out = 0;
                    if (obj_bigint_to_int64(bigint, &out)) {
                        value_init_int(&result, out);
                    } else if (bigint && bigint->sign < 0) {
                        value_init_int(&result, INT64_MIN);
                    } else {
                        value_init_int(&result, INT64_MAX);
                    }
                } else if (value_get_type(&a) == VAL_DOUBLE) {
                    double d = value_get_double(&a);
                    if (isnan(d) || isinf(d)) {
                        value_init_int(&result, 0);
                    } else if (d > (double)INT64_MAX) {
                        value_init_int(&result, INT64_MAX);
                    } else if (d < (double)INT64_MIN) {
                        value_init_int(&result, INT64_MIN);
                    } else {
                        value_init_int(&result, (int64_t)d);
                    }
                } else if (value_get_type(&a) == VAL_STRING) {
                    char* endptr;
                    int base = 10;
                    ObjString* str_obj = value_get_string_obj(&a);
                    const char* s = str_obj ? str_obj->chars : NULL;
                    if (s) {
                        const char* p = s;
                        if ((*p == '+' || *p == '-') && p[1] == '0' && (p[2] == 'x' || p[2] == 'X')) {
                            base = 16;
                        } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                            base = 16;
                        }
                    }
                    errno = 0;
                    int64_t value = strtoll(s ? s : "", &endptr, base);
                    if (errno == ERANGE || *endptr != '\0') {
                        value_init_int(&result, 0);
                    } else {
                        value_init_int(&result, value);
                    }
                } else {
                    value_init_int(&result, 0);
                }

                push(vm, result);
                value_free(&a);
                break;
            }

            case OP_CAST_BOOL: {
                Value a = pop(vm);
                Value result;

                if (value_get_type(&a) == VAL_BOOL) {
                    // Move the existing bool value.
                    push(vm, a);
                    break;
                } else if (value_get_type(&a) == VAL_INT) {
                    value_init_bool(&result, value_get_int(&a) != 0);
                } else if (value_get_type(&a) == VAL_BIGINT) {
                    ObjBigInt* bigint = value_get_bigint_obj(&a);
                    value_init_bool(&result, bigint && bigint->sign != 0);
                } else if (value_get_type(&a) == VAL_DOUBLE) {
                    double d = value_get_double(&a);
                    if (isnan(d)) {
                        value_init_bool(&result, false);
                    } else {
                        value_init_bool(&result, d != 0.0);
                    }
                } else if (value_get_type(&a) == VAL_STRING) {
                    ObjString* str_obj = value_get_string_obj(&a);
                    const char* s = str_obj ? str_obj->chars : "";
                    if (strcmp(s, "true") == 0 || strcmp(s, "1") == 0) {
                        value_init_bool(&result, true);
                    } else {
                        value_init_bool(&result, false);
                    }
                } else {
                    value_init_bool(&result, false);
                }

                push(vm, result);
                value_free(&a);
                break;
            }

            case OP_CAST_DOUBLE: {
                Value a = pop(vm);
                Value result;

                if (value_get_type(&a) == VAL_INT) {
                    value_init_double(&result, (double)value_get_int(&a));
                } else if (value_get_type(&a) == VAL_BOOL) {
                    value_init_double(&result, value_get_bool(&a) ? 1.0 : 0.0);
                } else if (value_get_type(&a) == VAL_BIGINT) {
                    double d = obj_bigint_to_double(value_get_bigint_obj(&a));
                    if (isnan(d)) {
                        value_init_double(&result, 0.0);
                    } else {
                        value_init_double(&result, d);
                    }
                } else if (value_get_type(&a) == VAL_DOUBLE) {
                    value_init_double(&result, value_get_double(&a));
                } else if (value_get_type(&a) == VAL_STRING) {
                    char* endptr;
                    errno = 0;
                    ObjString* str_obj = value_get_string_obj(&a);
                    const char* chars = str_obj ? str_obj->chars : "";
                    double value = strtod(chars, &endptr);
                    if (errno == ERANGE || *endptr != '\0') {
                        value_init_double(&result, 0.0);
                    } else {
                        value_init_double(&result, value);
                    }
                } else {
                    value_init_double(&result, 0.0);
                }

                push(vm, result);
                value_free(&a);
                break;
            }

            case OP_CAST_STRING: {
                Value a = pop(vm);
                char buffer[256];

                if (value_get_type(&a) == VAL_STRING) {
                    // Move the existing string value.
                    push(vm, a);
                    break;
                }

                Value result;
                if (value_get_type(&a) == VAL_INT) {
                    snprintf(buffer, sizeof(buffer), "%lld", (long long)value_get_int(&a));
                    value_init_string(&result, buffer);
                } else if (value_get_type(&a) == VAL_BOOL) {
                    value_init_string(&result, value_get_bool(&a) ? "true" : "false");
                } else if (value_get_type(&a) == VAL_BIGINT) {
                    char* str = obj_bigint_to_string(value_get_bigint_obj(&a));
                    value_init_string(&result, str ? str : "0");
                    if (str) free(str);
                } else if (value_get_type(&a) == VAL_DOUBLE) {
                    snprintf(buffer, sizeof(buffer), "%g", value_get_double(&a));
                    value_init_string(&result, buffer);
                } else if (value_get_type(&a) == VAL_NIL) {
                    value_init_string(&result, "nil");
                } else {
                    value_init_string(&result, "<unknown>");
                }

                push(vm, result);
                value_free(&a);
                break;
            }

            case OP_CAST_BIGINT: {
                Value a = pop(vm);
                Value result;

                if (value_get_type(&a) == VAL_BIGINT) {
                    push(vm, a);
                    break;
                } else if (value_get_type(&a) == VAL_BOOL) {
                    ObjBigInt* bigint = obj_bigint_from_int64(value_get_bool(&a) ? 1 : 0);
                    value_init_bigint(&result, bigint);
                } else if (value_get_type(&a) == VAL_INT) {
                    ObjBigInt* bigint = obj_bigint_from_int64(value_get_int(&a));
                    value_init_bigint(&result, bigint);
                } else if (value_get_type(&a) == VAL_DOUBLE) {
                    double d = value_get_double(&a);
                    if (isnan(d) || isinf(d)) {
                        ObjBigInt* bigint = obj_bigint_from_int64(0);
                        value_init_bigint(&result, bigint);
                    } else {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "%.0f", d);
                        ObjBigInt* bigint = obj_bigint_from_string(buf);
                        value_init_bigint(&result, bigint);
                    }
                } else if (value_get_type(&a) == VAL_STRING) {
                    ObjString* str_obj = value_get_string_obj(&a);
                    ObjBigInt* bigint = obj_bigint_from_string(str_obj ? str_obj->chars : NULL);
                    value_init_bigint(&result, bigint);
                } else {
                    ObjBigInt* bigint = obj_bigint_from_int64(0);
                    value_init_bigint(&result, bigint);
                }

                push(vm, result);
                value_free(&a);
                break;
            }

            case OP_PRINT: {
                Value a = pop(vm);
                value_print(vm, &a);
                value_free(&a);
                break;
            }

            case OP_PRINTLN: {
                Value a = pop(vm);
                value_print(vm, &a);
                vm_write_output(vm, "\n");
                value_free(&a);
                break;
            }

            case OP_TRY:
            case OP_THROW:
            case OP_CATCH:
                vm_runtime_error(vm, "Exceptions are not supported");
                return -1;

            case OP_RECORD_NEW: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t field_count = code[frame.ip++];
                ObjRecord* record = obj_record_create_with_count(vm, field_count);
                Value val;
                value_init_record(&val, record);
                push(vm, val);
                break;
            }

            case OP_RECORD_NEW_NAMED: {
                VM_ENSURE_IP_BYTES(3);
                int name_idx = ((int)code[frame.ip] << 8) | (int)code[frame.ip + 1];
                frame.ip += 2;
                uint8_t field_count = code[frame.ip++];

                const char* record_name = NULL;
                if (vm_get_global_name_constant(vm, frame.function, name_idx, &record_name) != 0) {
                    return -1;
                }

                ObjRecord* record = obj_record_create_with_count(vm, field_count);
                if (record_name && record_name[0] != '\0') {
                    record->type_name = safe_strdup(record_name);
                }

                Value val;
                value_init_record(&val, record);
                push(vm, val);
                break;
            }

            case OP_RECORD_SET_FIELD: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t field_idx = code[frame.ip++];
                Value value = pop(vm);
                Value record_val = pop(vm);
                if (value_get_type(&record_val) != VAL_RECORD) {
                    vm_runtime_error(vm, "Cannot set field on non-record");
                    value_free(&record_val);
                    value_free(&value);
                    return -1;
                }
                obj_record_set_field(value_get_record_obj(&record_val), field_idx, value);
                push(vm, record_val);
                break;
            }

            case OP_RECORD_GET_FIELD: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t field_idx = code[frame.ip++];
                Value record_val = pop(vm);
                if (value_get_type(&record_val) != VAL_RECORD) {
                    vm_runtime_error(vm, "Cannot get field from non-record");
                    value_free(&record_val);
                    return -1;
                }
                Value result;
                obj_record_get_field(value_get_record_obj(&record_val), field_idx, &result);
                push(vm, result);
                value_free(&record_val);
                break;
            }

            case OP_TUPLE_NEW: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t element_count = code[frame.ip++];
                ObjTuple* tuple = obj_tuple_create(vm, element_count);
                Value val;
                value_init_tuple(&val, tuple);
                push(vm, val);
                break;
            }

            case OP_TUPLE_GET: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t index = code[frame.ip++];
                Value tuple_val = pop(vm);
                if (value_get_type(&tuple_val) != VAL_TUPLE) {
                    vm_runtime_error(vm, "Cannot get element from non-tuple");
                    value_free(&tuple_val);
                    return -1;
                }
                Value result;
                obj_tuple_get(value_get_tuple_obj(&tuple_val), index, &result);
                push(vm, result);
                value_free(&tuple_val);
                break;
            }

            case OP_TUPLE_SET: {
                VM_ENSURE_IP_BYTES(1);
                uint8_t index = code[frame.ip++];
                Value value = pop(vm);
                Value tuple_val = pop(vm);
                if (value_get_type(&tuple_val) != VAL_TUPLE) {
                    vm_runtime_error(vm, "Cannot set element on non-tuple");
                    value_free(&tuple_val);
                    value_free(&value);
                    return -1;
                }
                obj_tuple_set(value_get_tuple_obj(&tuple_val), index, value);
                push(vm, tuple_val);
                value_free(&value);
                break;
            }

            case OP_MAP_NEW: {
                ObjMap* map = obj_map_create(vm);
                Value val;
                value_init_map(&val, map);
                push(vm, val);
                break;
            }

            case OP_MAP_SET: {
                Value value = pop(vm);
                Value key = pop(vm);
                Value map_val = pop(vm);
                if (value_get_type(&map_val) != VAL_MAP) {
                    vm_runtime_error(vm, "Cannot set key on non-map");
                    value_free(&map_val);
                    value_free(&key);
                    value_free(&value);
                    return -1;
                }
                obj_map_set(value_get_map_obj(&map_val), key, value);
                push(vm, map_val);
                value_free(&value);
                value_free(&key);
                break;
            }

            case OP_MAP_GET: {
                Value key = pop(vm);
                Value map_val = pop(vm);
                if (value_get_type(&map_val) != VAL_MAP) {
                    vm_runtime_error(vm, "Cannot get from non-map");
                    value_free(&map_val);
                    value_free(&key);
                    return -1;
                }
                Value result = obj_map_get(value_get_map_obj(&map_val), key);
                push(vm, result);
                value_free(&map_val);
                value_free(&key);
                break;
            }

            case OP_MAP_HAS: {
                Value key = pop(vm);
                Value map_val = pop(vm);
                if (value_get_type(&map_val) != VAL_MAP) {
                    vm_runtime_error(vm, "Cannot check key on non-map");
                    value_free(&map_val);
                    value_free(&key);
                    return -1;
                }
                Value result;
                value_init_int(&result, obj_map_has(value_get_map_obj(&map_val), key) ? 1 : 0);
                push(vm, result);
                value_free(&map_val);
                value_free(&key);
                break;
            }

            case OP_MAP_DELETE: {
                Value key = pop(vm);
                Value map_val = pop(vm);
                if (value_get_type(&map_val) != VAL_MAP) {
                    vm_runtime_error(vm, "Cannot delete from non-map");
                    value_free(&map_val);
                    value_free(&key);
                    return -1;
                }
                obj_map_delete(value_get_map_obj(&map_val), key);
                push(vm, map_val);
                break;
            }

            case OP_MAP_KEYS: {
                Value map_val = pop(vm);
                if (value_get_type(&map_val) != VAL_MAP) {
                    vm_runtime_error(vm, "Cannot get keys from non-map");
                    value_free(&map_val);
                    return -1;
                }
                ObjMap* map_obj = value_get_map_obj(&map_val);
                ObjArray* result_arr = obj_array_create(vm, map_obj->count);
                obj_map_keys(map_obj, result_arr);
                Value result;
                value_init_array(&result, result_arr);
                push(vm, result);
                value_free(&map_val);
                break;
            }

            case OP_MAP_VALUES: {
                Value map_val = pop(vm);
                if (value_get_type(&map_val) != VAL_MAP) {
                    vm_runtime_error(vm, "Cannot get values from non-map");
                    value_free(&map_val);
                    return -1;
                }
                ObjMap* map_obj = value_get_map_obj(&map_val);
                ObjArray* result_arr = obj_array_create(vm, map_obj->count);
                obj_map_values(map_obj, result_arr);
                Value result;
                value_init_array(&result, result_arr);
                push(vm, result);
                value_free(&map_val);
                break;
            }

            case OP_SET_NEW: {
                ObjSet* set = obj_set_create();
                Value val;
                value_init_set(&val, set);
                push(vm, val);
                break;
            }

            case OP_SET_ADD: {
                Value value = pop(vm);
                Value set_val = pop(vm);
                if (value_get_type(&set_val) != VAL_SET) {
                    vm_runtime_error(vm, "Cannot add to non-set");
                    value_free(&set_val);
                    value_free(&value);
                    return -1;
                }
                obj_set_add(value_get_set_obj(&set_val), value);
                push(vm, set_val);
                value_free(&value);
                break;
            }

            case OP_SET_HAS: {
                Value value = pop(vm);
                Value set_val = pop(vm);
                if (value_get_type(&set_val) != VAL_SET) {
                    vm_runtime_error(vm, "Cannot check non-set");
                    value_free(&set_val);
                    value_free(&value);
                    return -1;
                }
                Value result;
                value_init_int(&result, obj_set_has(value_get_set_obj(&set_val), value) ? 1 : 0);
                push(vm, result);
                value_free(&set_val);
                value_free(&value);
                break;
            }

            case OP_SET_REMOVE: {
                Value value = pop(vm);
                Value set_val = pop(vm);
                if (value_get_type(&set_val) != VAL_SET) {
                    vm_runtime_error(vm, "Cannot remove from non-set");
                    value_free(&set_val);
                    value_free(&value);
                    return -1;
                }
                obj_set_remove(value_get_set_obj(&set_val), value);
                push(vm, set_val);
                value_free(&value);
                break;
            }

            case OP_SET_TO_ARRAY: {
                Value set_val = pop(vm);
                if (value_get_type(&set_val) != VAL_SET) {
                    vm_runtime_error(vm, "Cannot convert non-set to array");
                    value_free(&set_val);
                    return -1;
                }
                ObjSet* set_obj = value_get_set_obj(&set_val);
                ObjArray* result_arr = obj_array_create(vm, set_obj->count);
                obj_set_to_array(set_obj, result_arr);
                Value result;
                value_init_array(&result, result_arr);
                push(vm, result);
                value_free(&set_val);
                break;
            }

            default:
                {
                    char buf[128];
                    int ip = frame.ip - 1;
                    int line = 0;
                    if (chunk->debug_info && ip >= 0 && ip < chunk->code_count) {
                        line = chunk->debug_info[ip].line;
                    }
                    snprintf(buf, sizeof(buf), "Unknown opcode 0x%02X at ip=%d (line %d)", instruction, ip, line);
                    vm_runtime_error(vm, buf);
                }
                return -1;
        }
        if (vm->error_occurred) {
            if (frame.is_async_root) {
                if (vm_handle_async_frame_error(vm, &frame, &chunk, &code) != 0) {
                    return -1;
                }
                continue;
            }
            if (vm_maybe_stop_at_runtime_error(vm, &frame)) {
                return 1;
            }
            return -1;
        }
    }

#undef VM_ENSURE_IP_BYTES
}

void vm_push_exception_handler(VM* vm, int handler_ip) {
    vm_exception_handler_reserve(vm, 1);
    vm->exception_handlers[vm->exception_handler_count].handler_ip = handler_ip;
    vm->exception_handlers[vm->exception_handler_count].stack_depth = vm->stack.count;
    vm->exception_handler_count++;
}

ExceptionHandler vm_pop_exception_handler(VM* vm) {
    if (vm->exception_handler_count == 0) {
        ExceptionHandler empty = {-1, -1};
        return empty;
    }
    vm->exception_handler_count--;
    return vm->exception_handlers[vm->exception_handler_count];
}

void vm_throw_exception(VM* vm, Value value) {
    vm->exception_value = value;
    vm->in_exception = true;
}

bool vm_has_exception(VM* vm) {
    return vm->in_exception;
}

VMConfig vm_default_config(void) {
    VMConfig config;
    config.max_call_depth = 1000;
    config.max_stack_size = 65536;
    config.max_array_size = 1048576;
    config.max_string_length = 16777216;
    config.max_instructions = 0;
    config.max_open_files = 0;
    config.max_open_sockets = 0;
    return config;
}

void vm_set_config(VM* vm, VMConfig config) {
    vm->config = config;
}

void vm_set_sandbox_root(VM* vm, const char* root) {
    if (!vm) return;
    if (vm->sandbox_root) {
        free(vm->sandbox_root);
        vm->sandbox_root = NULL;
    }
    if (root && root[0] != '\0') {
        vm->sandbox_root = safe_strdup(root);
    }
}

const char* vm_get_sandbox_root(VM* vm) {
    if (!vm) return NULL;
    return vm->sandbox_root;
}

void vm_set_file_io_enabled(VM* vm, bool enabled) {
    if (!vm) return;
    vm->file_io_enabled = enabled;
}

bool vm_is_file_io_enabled(VM* vm) {
    if (!vm) return false;
    return vm->file_io_enabled;
}

void vm_set_network_enabled(VM* vm, bool enabled) {
    if (!vm) return;
    vm->network_enabled = enabled;
}

bool vm_is_network_enabled(VM* vm) {
    if (!vm) return false;
    return vm->network_enabled;
}

void vm_set_process_enabled(VM* vm, bool enabled) {
    if (!vm) return;
    vm->process_enabled = enabled;
}

bool vm_is_process_enabled(VM* vm) {
    if (!vm) return false;
    return vm->process_enabled;
}

void vm_set_sqlite_enabled(VM* vm, bool enabled) {
    if (!vm) return;
    vm->sqlite_enabled = enabled;
}

bool vm_is_sqlite_enabled(VM* vm) {
    if (!vm) return false;
    return vm->sqlite_enabled;
}

void vm_set_threading_enabled(VM* vm, bool enabled) {
    if (!vm) return;
    vm->threading_enabled = enabled;
}

bool vm_is_threading_enabled(VM* vm) {
    if (!vm) return false;
    return vm->threading_enabled;
}

void vm_set_max_open_files(VM* vm, int max_open_files) {
    if (!vm) return;
    vm->config.max_open_files = max_open_files < 0 ? 0 : max_open_files;
}

void vm_set_max_open_sockets(VM* vm, int max_open_sockets) {
    if (!vm) return;
    vm->config.max_open_sockets = max_open_sockets < 0 ? 0 : max_open_sockets;
}

int vm_get_max_open_files(VM* vm) {
    if (!vm) return 0;
    return vm->config.max_open_files;
}

int vm_get_max_open_sockets(VM* vm) {
    if (!vm) return 0;
    return vm->config.max_open_sockets;
}

int vm_get_current_open_files(VM* vm) {
    if (!vm) return 0;
    return vm->current_open_files;
}

int vm_get_current_open_sockets(VM* vm) {
    if (!vm) return 0;
    return vm->current_open_sockets;
}

bool vm_try_acquire_file_handle(VM* vm) {
    if (!vm) return false;
    if (vm->config.max_open_files > 0 &&
        vm->current_open_files >= vm->config.max_open_files) {
        return false;
    }
    vm->current_open_files++;
    return true;
}

void vm_release_file_handle(VM* vm) {
    if (!vm) return;
    if (vm->current_open_files > 0) {
        vm->current_open_files--;
    }
}

bool vm_try_acquire_socket_handle(VM* vm) {
    if (!vm) return false;
    if (vm->config.max_open_sockets > 0 &&
        vm->current_open_sockets >= vm->config.max_open_sockets) {
        return false;
    }
    vm->current_open_sockets++;
    return true;
}

void vm_release_socket_handle(VM* vm) {
    if (!vm) return;
    if (vm->current_open_sockets > 0) {
        vm->current_open_sockets--;
    }
}
