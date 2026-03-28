#ifndef RUNTIME_H
#define RUNTIME_H

#include "vm.h"
#include "file_loader.h"

typedef enum {
    RUNTIME_LOAD_SOURCE = 0,
    RUNTIME_LOAD_CACHE = 1,
    RUNTIME_LOAD_ARTIFACT = 2
} RuntimeLoadMode;

typedef struct {
    VM* vm;
    bool vm_initialized;
    bool create_recovery_error;
    NativeExtensionRegistry* extension_registry;
    ObjFunction* init_function;
    ObjFunction* main_function;
    SymbolTable* globals;
    ObjFunction** functions;
    int function_count;
    InterfaceDispatchEntry* interface_dispatch_entries;
    int interface_dispatch_count;
    bool init_ran;
    bool debug_stop_on_main_entry_pending;
    RuntimeLoadMode load_mode;
    char* cache_path;
    Error* error;
    bool fallback_error_set;
    char fallback_error_message[256];
} Runtime;

Runtime* runtime_create(const char* file_path);

typedef struct {
    bool deny_file_io;
    bool deny_network;
    bool deny_process;
    bool deny_sqlite;
    bool deny_threading;
} RuntimeCapabilityOptions;

typedef struct {
    TypeCheckOptions typecheck;
    RuntimeCapabilityOptions capabilities;
    int max_open_files;
    int max_open_sockets;
    const char* const* extension_paths;
    int extension_path_count;
} RuntimeOptions;

typedef enum {
    RUNTIME_POSTED_EVENT_INPUT = 0,
    RUNTIME_POSTED_EVENT_WINDOW = 1,
    RUNTIME_POSTED_EVENT_FRAME = 2
} RuntimePostedEventKind;

typedef enum {
    RUNTIME_POSTED_EVENT_FIELD_INT = 0,
    RUNTIME_POSTED_EVENT_FIELD_BOOL = 1,
    RUNTIME_POSTED_EVENT_FIELD_DOUBLE = 2,
    RUNTIME_POSTED_EVENT_FIELD_STRING = 3,
    RUNTIME_POSTED_EVENT_FIELD_ARRAY = 4,
    RUNTIME_POSTED_EVENT_FIELD_TUPLE = 5,
    RUNTIME_POSTED_EVENT_FIELD_MAP = 6
} RuntimePostedEventFieldKind;

typedef struct RuntimePostedEventExtraValue RuntimePostedEventExtraValue;
typedef struct RuntimePostedEventExtraMapEntry RuntimePostedEventExtraMapEntry;

struct RuntimePostedEventExtraValue {
    RuntimePostedEventFieldKind kind;
    int64_t int_value;
    bool bool_value;
    double double_value;
    const char* string_value;
    const RuntimePostedEventExtraValue* items;
    int item_count;
    const RuntimePostedEventExtraMapEntry* map_entries;
    int map_entry_count;
};

struct RuntimePostedEventExtraMapEntry {
    const char* key;
    RuntimePostedEventExtraValue value;
};

typedef struct {
    const char* name;
    RuntimePostedEventExtraValue value;
} RuntimePostedEventExtraField;

typedef struct {
    const char* source;
    int64_t priority;
    const char* const* phases;
    int phase_count;
} RuntimePostedEventMetaOverride;

typedef struct {
    const char* device;
    int64_t code;
    bool pressed;
    const RuntimePostedEventMetaOverride* meta_override;
    const RuntimePostedEventExtraField* extra_fields;
    int extra_field_count;
} RuntimePostedInputEvent;

typedef struct {
    const char* event_name;
    int64_t width;
    int64_t height;
    bool focused;
    const RuntimePostedEventMetaOverride* meta_override;
    const RuntimePostedEventExtraField* extra_fields;
    int extra_field_count;
} RuntimePostedWindowEvent;

typedef struct {
    const char* phase;
    int64_t frame_number;
    double delta_seconds;
    const RuntimePostedEventMetaOverride* meta_override;
    const RuntimePostedEventExtraField* extra_fields;
    int extra_field_count;
} RuntimePostedFrameEvent;

typedef struct {
    RuntimePostedEventKind kind;
    union {
        RuntimePostedInputEvent input;
        RuntimePostedWindowEvent window;
        RuntimePostedFrameEvent frame;
    } as;
} RuntimePostedEvent;

typedef struct {
    RuntimePostedEventExtraValue* items;
    int capacity;
    int count;
} RuntimePostedEventExtraArrayBuilder;

typedef struct {
    RuntimePostedEventExtraMapEntry* entries;
    int capacity;
    int count;
} RuntimePostedEventExtraMapBuilder;

typedef struct {
    const char* label;
    bool active;
    int64_t score;
    const RuntimePostedEventExtraValue* coords_items;
    int coords_count;
    const RuntimePostedEventExtraValue* buttons_items;
    int button_count;
} RuntimePostedInputStatePayloadSpec;

typedef struct {
    int64_t score;
    const RuntimePostedEventExtraValue* combo_items;
    int combo_count;
} RuntimePostedInputComboPayloadSpec;

typedef struct {
    const char* title;
    const RuntimePostedEventExtraValue* rect_items;
    int rect_count;
} RuntimePostedWindowRectPayloadSpec;

typedef struct {
    const char* phase;
    const RuntimePostedEventExtraValue* marker_items;
    int marker_count;
} RuntimePostedFrameMarkerPayloadSpec;

typedef struct {
    const char* owner;
    int64_t frame;
} RuntimePostedEventContextSpec;

typedef enum {
    RUNTIME_POSTED_TYPED_PAYLOAD_NONE = 0,
    RUNTIME_POSTED_TYPED_PAYLOAD_INPUT_STATE = 1,
    RUNTIME_POSTED_TYPED_PAYLOAD_INPUT_COMBO = 2,
    RUNTIME_POSTED_TYPED_PAYLOAD_WINDOW_RECT = 3,
    RUNTIME_POSTED_TYPED_PAYLOAD_FRAME_MARKER = 4
} RuntimePostedTypedPayloadKind;

typedef struct {
    RuntimePostedEventKind kind;
    union {
        RuntimePostedInputEvent input;
        RuntimePostedWindowEvent window;
        RuntimePostedFrameEvent frame;
    } event;
    RuntimePostedTypedPayloadKind payload_kind;
    union {
        RuntimePostedInputStatePayloadSpec input_state;
        RuntimePostedInputComboPayloadSpec input_combo;
        RuntimePostedWindowRectPayloadSpec window_rect;
        RuntimePostedFrameMarkerPayloadSpec frame_marker;
    } payload;
    bool has_context_spec;
    RuntimePostedEventContextSpec context_spec;
} RuntimePostedTypedEvent;

typedef union {
    const RuntimePostedInputStatePayloadSpec* input_state;
    const RuntimePostedInputComboPayloadSpec* input_combo;
} RuntimePostedTypedInputBatchPayloads;

typedef struct {
    RuntimePostedTypedPayloadKind payload_kind;
    const RuntimePostedInputEvent* events;
    RuntimePostedTypedInputBatchPayloads payload_specs;
    const RuntimePostedEventContextSpec* context_specs;
    int event_count;
} RuntimePostedTypedInputBatchSpec;

typedef union {
    const RuntimePostedWindowRectPayloadSpec* window_rect;
} RuntimePostedTypedWindowBatchPayloads;

typedef struct {
    RuntimePostedTypedPayloadKind payload_kind;
    const RuntimePostedWindowEvent* events;
    RuntimePostedTypedWindowBatchPayloads payload_specs;
    const RuntimePostedEventContextSpec* context_specs;
    int event_count;
} RuntimePostedTypedWindowBatchSpec;

typedef union {
    const RuntimePostedFrameMarkerPayloadSpec* frame_marker;
} RuntimePostedTypedFrameBatchPayloads;

typedef struct {
    RuntimePostedTypedPayloadKind payload_kind;
    const RuntimePostedFrameEvent* events;
    RuntimePostedTypedFrameBatchPayloads payload_specs;
    const RuntimePostedEventContextSpec* context_specs;
    int event_count;
} RuntimePostedTypedFrameBatchSpec;

typedef struct {
    const RuntimePostedTypedInputBatchSpec* input_batch;
    const RuntimePostedTypedWindowBatchSpec* window_batch;
    const RuntimePostedTypedFrameBatchSpec* frame_batch;
    bool has_shared_context;
    RuntimePostedEventContextSpec shared_context;
} RuntimePostedFrameEnvelopeBatchSpec;

typedef struct {
    RuntimePostedTypedEvent* events;
    int capacity;
    int count;
    bool has_shared_context;
    RuntimePostedEventContextSpec shared_context;
} RuntimePostedFrameEnvelopeBuilder;

typedef struct {
    RuntimePostedTypedEvent* events;
    int capacity;
    int count;
    bool has_shared_context;
    RuntimePostedEventContextSpec shared_context;
} RuntimePostedFrameEnvelopeHeapBuilder;

typedef enum {
    RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_NO_WAIT = 0,
    RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_WAIT = 1,
    RUNTIME_HOST_EVENT_LOOP_SESSION_TICK_WAIT_AND_PUMP_UNTIL_BUDGET = 2
} RuntimeHostEventLoopSessionTickMode;

typedef struct {
    Runtime* runtime;
    RuntimePostedFrameEnvelopeHeapBuilder frame_builder;
    char* default_frame_callback_name;
    int default_max_callbacks;
    int64_t default_max_millis;
    RuntimeHostEventLoopSessionTickMode default_tick_mode;
    int64_t default_tick_wait_timeout_millis;
} RuntimeHostEventLoopSession;

typedef struct {
    bool frame_posted;
    int callbacks_drained;
} RuntimeHostEventLoopSessionStepResult;

typedef struct {
    bool has_post_frame;
    bool post_frame;
    bool has_frame_callback_name;
    const char* frame_callback_name;
    bool has_wait_timeout_millis;
    int64_t wait_timeout_millis;
    bool has_max_callbacks;
    int max_callbacks;
    bool has_max_millis;
    int64_t max_millis;
} RuntimeHostEventLoopSessionStepOptions;

static inline RuntimePostedTypedEvent runtime_posted_typed_input_event(const RuntimePostedInputEvent* event) {
    RuntimePostedTypedEvent typed_event;
    memset(&typed_event, 0, sizeof(typed_event));
    typed_event.kind = RUNTIME_POSTED_EVENT_INPUT;
    if (event) {
        typed_event.event.input = *event;
    }
    return typed_event;
}

static inline RuntimePostedTypedEvent runtime_posted_typed_input_state_event(const RuntimePostedInputEvent* event,
                                                                             const RuntimePostedInputStatePayloadSpec* payload_spec,
                                                                             const RuntimePostedEventContextSpec* context_spec) {
    RuntimePostedTypedEvent typed_event = runtime_posted_typed_input_event(event);
    typed_event.payload_kind = RUNTIME_POSTED_TYPED_PAYLOAD_INPUT_STATE;
    if (payload_spec) {
        typed_event.payload.input_state = *payload_spec;
    }
    if (context_spec) {
        typed_event.has_context_spec = true;
        typed_event.context_spec = *context_spec;
    }
    return typed_event;
}

static inline RuntimePostedTypedEvent runtime_posted_typed_input_combo_event(const RuntimePostedInputEvent* event,
                                                                             const RuntimePostedInputComboPayloadSpec* payload_spec,
                                                                             const RuntimePostedEventContextSpec* context_spec) {
    RuntimePostedTypedEvent typed_event = runtime_posted_typed_input_event(event);
    typed_event.payload_kind = RUNTIME_POSTED_TYPED_PAYLOAD_INPUT_COMBO;
    if (payload_spec) {
        typed_event.payload.input_combo = *payload_spec;
    }
    if (context_spec) {
        typed_event.has_context_spec = true;
        typed_event.context_spec = *context_spec;
    }
    return typed_event;
}

static inline RuntimePostedTypedEvent runtime_posted_typed_window_event(const RuntimePostedWindowEvent* event) {
    RuntimePostedTypedEvent typed_event;
    memset(&typed_event, 0, sizeof(typed_event));
    typed_event.kind = RUNTIME_POSTED_EVENT_WINDOW;
    if (event) {
        typed_event.event.window = *event;
    }
    return typed_event;
}

static inline RuntimePostedTypedEvent runtime_posted_typed_window_rect_event(const RuntimePostedWindowEvent* event,
                                                                             const RuntimePostedWindowRectPayloadSpec* payload_spec,
                                                                             const RuntimePostedEventContextSpec* context_spec) {
    RuntimePostedTypedEvent typed_event = runtime_posted_typed_window_event(event);
    typed_event.payload_kind = RUNTIME_POSTED_TYPED_PAYLOAD_WINDOW_RECT;
    if (payload_spec) {
        typed_event.payload.window_rect = *payload_spec;
    }
    if (context_spec) {
        typed_event.has_context_spec = true;
        typed_event.context_spec = *context_spec;
    }
    return typed_event;
}

static inline RuntimePostedTypedEvent runtime_posted_typed_frame_event(const RuntimePostedFrameEvent* event) {
    RuntimePostedTypedEvent typed_event;
    memset(&typed_event, 0, sizeof(typed_event));
    typed_event.kind = RUNTIME_POSTED_EVENT_FRAME;
    if (event) {
        typed_event.event.frame = *event;
    }
    return typed_event;
}

static inline RuntimePostedTypedEvent runtime_posted_typed_frame_marker_event(const RuntimePostedFrameEvent* event,
                                                                              const RuntimePostedFrameMarkerPayloadSpec* payload_spec,
                                                                              const RuntimePostedEventContextSpec* context_spec) {
    RuntimePostedTypedEvent typed_event = runtime_posted_typed_frame_event(event);
    typed_event.payload_kind = RUNTIME_POSTED_TYPED_PAYLOAD_FRAME_MARKER;
    if (payload_spec) {
        typed_event.payload.frame_marker = *payload_spec;
    }
    if (context_spec) {
        typed_event.has_context_spec = true;
        typed_event.context_spec = *context_spec;
    }
    return typed_event;
}

static inline RuntimePostedTypedInputBatchSpec runtime_posted_typed_input_batch(const RuntimePostedInputEvent* events,
                                                                                const RuntimePostedEventContextSpec* context_specs,
                                                                                int event_count) {
    RuntimePostedTypedInputBatchSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.payload_kind = RUNTIME_POSTED_TYPED_PAYLOAD_NONE;
    spec.events = events;
    spec.context_specs = context_specs;
    spec.event_count = event_count;
    return spec;
}

static inline RuntimePostedTypedInputBatchSpec runtime_posted_typed_input_state_batch(const RuntimePostedInputEvent* events,
                                                                                      const RuntimePostedInputStatePayloadSpec* payload_specs,
                                                                                      const RuntimePostedEventContextSpec* context_specs,
                                                                                      int event_count) {
    RuntimePostedTypedInputBatchSpec spec = runtime_posted_typed_input_batch(events, context_specs, event_count);
    spec.payload_kind = RUNTIME_POSTED_TYPED_PAYLOAD_INPUT_STATE;
    spec.payload_specs.input_state = payload_specs;
    return spec;
}

static inline RuntimePostedTypedInputBatchSpec runtime_posted_typed_input_combo_batch(const RuntimePostedInputEvent* events,
                                                                                      const RuntimePostedInputComboPayloadSpec* payload_specs,
                                                                                      const RuntimePostedEventContextSpec* context_specs,
                                                                                      int event_count) {
    RuntimePostedTypedInputBatchSpec spec = runtime_posted_typed_input_batch(events, context_specs, event_count);
    spec.payload_kind = RUNTIME_POSTED_TYPED_PAYLOAD_INPUT_COMBO;
    spec.payload_specs.input_combo = payload_specs;
    return spec;
}

static inline RuntimePostedTypedWindowBatchSpec runtime_posted_typed_window_batch(const RuntimePostedWindowEvent* events,
                                                                                  const RuntimePostedEventContextSpec* context_specs,
                                                                                  int event_count) {
    RuntimePostedTypedWindowBatchSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.payload_kind = RUNTIME_POSTED_TYPED_PAYLOAD_NONE;
    spec.events = events;
    spec.context_specs = context_specs;
    spec.event_count = event_count;
    return spec;
}

static inline RuntimePostedTypedWindowBatchSpec runtime_posted_typed_window_rect_batch(const RuntimePostedWindowEvent* events,
                                                                                       const RuntimePostedWindowRectPayloadSpec* payload_specs,
                                                                                       const RuntimePostedEventContextSpec* context_specs,
                                                                                       int event_count) {
    RuntimePostedTypedWindowBatchSpec spec = runtime_posted_typed_window_batch(events, context_specs, event_count);
    spec.payload_kind = RUNTIME_POSTED_TYPED_PAYLOAD_WINDOW_RECT;
    spec.payload_specs.window_rect = payload_specs;
    return spec;
}

static inline RuntimePostedTypedFrameBatchSpec runtime_posted_typed_frame_batch(const RuntimePostedFrameEvent* events,
                                                                                const RuntimePostedEventContextSpec* context_specs,
                                                                                int event_count) {
    RuntimePostedTypedFrameBatchSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.payload_kind = RUNTIME_POSTED_TYPED_PAYLOAD_NONE;
    spec.events = events;
    spec.context_specs = context_specs;
    spec.event_count = event_count;
    return spec;
}

static inline RuntimePostedTypedFrameBatchSpec runtime_posted_typed_frame_marker_batch(const RuntimePostedFrameEvent* events,
                                                                                       const RuntimePostedFrameMarkerPayloadSpec* payload_specs,
                                                                                       const RuntimePostedEventContextSpec* context_specs,
                                                                                       int event_count) {
    RuntimePostedTypedFrameBatchSpec spec = runtime_posted_typed_frame_batch(events, context_specs, event_count);
    spec.payload_kind = RUNTIME_POSTED_TYPED_PAYLOAD_FRAME_MARKER;
    spec.payload_specs.frame_marker = payload_specs;
    return spec;
}

static inline RuntimePostedFrameEnvelopeBatchSpec runtime_posted_frame_envelope_batch(const RuntimePostedTypedInputBatchSpec* input_batch,
                                                                                      const RuntimePostedTypedWindowBatchSpec* window_batch,
                                                                                      const RuntimePostedTypedFrameBatchSpec* frame_batch,
                                                                                      const RuntimePostedEventContextSpec* shared_context) {
    RuntimePostedFrameEnvelopeBatchSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.input_batch = input_batch;
    spec.window_batch = window_batch;
    spec.frame_batch = frame_batch;
    if (shared_context) {
        spec.has_shared_context = true;
        spec.shared_context = *shared_context;
    }
    return spec;
}

static inline void runtime_posted_frame_envelope_builder_init(RuntimePostedFrameEnvelopeBuilder* builder,
                                                              RuntimePostedTypedEvent* events,
                                                              int capacity) {
    if (!builder) return;
    builder->events = events;
    builder->capacity = capacity;
    builder->count = 0;
    builder->has_shared_context = false;
    memset(&builder->shared_context, 0, sizeof(builder->shared_context));
}

static inline void runtime_posted_frame_envelope_builder_clear(RuntimePostedFrameEnvelopeBuilder* builder) {
    if (!builder) return;
    builder->count = 0;
    builder->has_shared_context = false;
    memset(&builder->shared_context, 0, sizeof(builder->shared_context));
}

static inline void runtime_posted_frame_envelope_builder_set_shared_context(RuntimePostedFrameEnvelopeBuilder* builder,
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

static inline bool runtime_posted_frame_envelope_builder_add_typed_event(RuntimePostedFrameEnvelopeBuilder* builder,
                                                                         RuntimePostedTypedEvent event) {
    if (!builder || !builder->events || builder->count < 0 || builder->count >= builder->capacity) {
        return false;
    }
    builder->events[builder->count++] = event;
    return true;
}

static inline bool runtime_posted_frame_envelope_builder_add_input_event(RuntimePostedFrameEnvelopeBuilder* builder,
                                                                         const RuntimePostedInputEvent* event) {
    return runtime_posted_frame_envelope_builder_add_typed_event(builder,
                                                                 runtime_posted_typed_input_event(event));
}

static inline bool runtime_posted_frame_envelope_builder_add_input_state_event(RuntimePostedFrameEnvelopeBuilder* builder,
                                                                               const RuntimePostedInputEvent* event,
                                                                               const RuntimePostedInputStatePayloadSpec* payload_spec,
                                                                               const RuntimePostedEventContextSpec* context_spec) {
    return runtime_posted_frame_envelope_builder_add_typed_event(builder,
                                                                 runtime_posted_typed_input_state_event(event,
                                                                                                        payload_spec,
                                                                                                        context_spec));
}

static inline bool runtime_posted_frame_envelope_builder_add_input_combo_event(RuntimePostedFrameEnvelopeBuilder* builder,
                                                                               const RuntimePostedInputEvent* event,
                                                                               const RuntimePostedInputComboPayloadSpec* payload_spec,
                                                                               const RuntimePostedEventContextSpec* context_spec) {
    return runtime_posted_frame_envelope_builder_add_typed_event(builder,
                                                                 runtime_posted_typed_input_combo_event(event,
                                                                                                        payload_spec,
                                                                                                        context_spec));
}

static inline bool runtime_posted_frame_envelope_builder_add_window_event(RuntimePostedFrameEnvelopeBuilder* builder,
                                                                          const RuntimePostedWindowEvent* event) {
    return runtime_posted_frame_envelope_builder_add_typed_event(builder,
                                                                 runtime_posted_typed_window_event(event));
}

static inline bool runtime_posted_frame_envelope_builder_add_window_rect_event(RuntimePostedFrameEnvelopeBuilder* builder,
                                                                               const RuntimePostedWindowEvent* event,
                                                                               const RuntimePostedWindowRectPayloadSpec* payload_spec,
                                                                               const RuntimePostedEventContextSpec* context_spec) {
    return runtime_posted_frame_envelope_builder_add_typed_event(builder,
                                                                 runtime_posted_typed_window_rect_event(event,
                                                                                                         payload_spec,
                                                                                                         context_spec));
}

static inline bool runtime_posted_frame_envelope_builder_add_frame_event(RuntimePostedFrameEnvelopeBuilder* builder,
                                                                         const RuntimePostedFrameEvent* event) {
    return runtime_posted_frame_envelope_builder_add_typed_event(builder,
                                                                 runtime_posted_typed_frame_event(event));
}

static inline bool runtime_posted_frame_envelope_builder_add_frame_marker_event(RuntimePostedFrameEnvelopeBuilder* builder,
                                                                                const RuntimePostedFrameEvent* event,
                                                                                const RuntimePostedFrameMarkerPayloadSpec* payload_spec,
                                                                                const RuntimePostedEventContextSpec* context_spec) {
    return runtime_posted_frame_envelope_builder_add_typed_event(builder,
                                                                 runtime_posted_typed_frame_marker_event(event,
                                                                                                         payload_spec,
                                                                                                         context_spec));
}

void runtime_posted_frame_envelope_heap_builder_init(RuntimePostedFrameEnvelopeHeapBuilder* builder);
void runtime_posted_frame_envelope_heap_builder_free(RuntimePostedFrameEnvelopeHeapBuilder* builder);
void runtime_posted_frame_envelope_heap_builder_clear(RuntimePostedFrameEnvelopeHeapBuilder* builder);
void runtime_posted_frame_envelope_heap_builder_set_shared_context(RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                                                   const RuntimePostedEventContextSpec* shared_context);
bool runtime_posted_frame_envelope_heap_builder_reserve(RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                                        int min_capacity);
bool runtime_posted_frame_envelope_heap_builder_add_typed_event(RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                                                RuntimePostedTypedEvent event);

static inline bool runtime_posted_frame_envelope_heap_builder_add_input_event(RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                                                              const RuntimePostedInputEvent* event) {
    return runtime_posted_frame_envelope_heap_builder_add_typed_event(builder,
                                                                      runtime_posted_typed_input_event(event));
}

static inline bool runtime_posted_frame_envelope_heap_builder_add_input_state_event(RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                                                                    const RuntimePostedInputEvent* event,
                                                                                    const RuntimePostedInputStatePayloadSpec* payload_spec,
                                                                                    const RuntimePostedEventContextSpec* context_spec) {
    return runtime_posted_frame_envelope_heap_builder_add_typed_event(builder,
                                                                      runtime_posted_typed_input_state_event(event,
                                                                                                             payload_spec,
                                                                                                             context_spec));
}

static inline bool runtime_posted_frame_envelope_heap_builder_add_input_combo_event(RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                                                                    const RuntimePostedInputEvent* event,
                                                                                    const RuntimePostedInputComboPayloadSpec* payload_spec,
                                                                                    const RuntimePostedEventContextSpec* context_spec) {
    return runtime_posted_frame_envelope_heap_builder_add_typed_event(builder,
                                                                      runtime_posted_typed_input_combo_event(event,
                                                                                                             payload_spec,
                                                                                                             context_spec));
}

static inline bool runtime_posted_frame_envelope_heap_builder_add_window_event(RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                                                               const RuntimePostedWindowEvent* event) {
    return runtime_posted_frame_envelope_heap_builder_add_typed_event(builder,
                                                                      runtime_posted_typed_window_event(event));
}

static inline bool runtime_posted_frame_envelope_heap_builder_add_window_rect_event(RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                                                                    const RuntimePostedWindowEvent* event,
                                                                                    const RuntimePostedWindowRectPayloadSpec* payload_spec,
                                                                                    const RuntimePostedEventContextSpec* context_spec) {
    return runtime_posted_frame_envelope_heap_builder_add_typed_event(builder,
                                                                      runtime_posted_typed_window_rect_event(event,
                                                                                                              payload_spec,
                                                                                                              context_spec));
}

static inline bool runtime_posted_frame_envelope_heap_builder_add_frame_event(RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                                                              const RuntimePostedFrameEvent* event) {
    return runtime_posted_frame_envelope_heap_builder_add_typed_event(builder,
                                                                      runtime_posted_typed_frame_event(event));
}

static inline bool runtime_posted_frame_envelope_heap_builder_add_frame_marker_event(RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                                                                     const RuntimePostedFrameEvent* event,
                                                                                     const RuntimePostedFrameMarkerPayloadSpec* payload_spec,
                                                                                     const RuntimePostedEventContextSpec* context_spec) {
    return runtime_posted_frame_envelope_heap_builder_add_typed_event(builder,
                                                                      runtime_posted_typed_frame_marker_event(event,
                                                                                                              payload_spec,
                                                                                                              context_spec));
}

void runtime_host_event_loop_session_init(RuntimeHostEventLoopSession* session, Runtime* rt);
void runtime_host_event_loop_session_free(RuntimeHostEventLoopSession* session);
void runtime_host_event_loop_session_begin_frame(RuntimeHostEventLoopSession* session,
                                                 const RuntimePostedEventContextSpec* shared_context);
void runtime_host_event_loop_session_clear_frame(RuntimeHostEventLoopSession* session);
bool runtime_host_event_loop_session_configure_end_frame(RuntimeHostEventLoopSession* session,
                                                         const char* callback_name,
                                                         int max_callbacks,
                                                         int64_t max_millis,
                                                         char* error_buf,
                                                         size_t error_buf_size);
void runtime_host_event_loop_session_clear_end_frame(RuntimeHostEventLoopSession* session);
bool runtime_host_event_loop_session_configure_tick(RuntimeHostEventLoopSession* session,
                                                    RuntimeHostEventLoopSessionTickMode mode,
                                                    int64_t wait_timeout_millis,
                                                    char* error_buf,
                                                    size_t error_buf_size);
void runtime_host_event_loop_session_clear_tick(RuntimeHostEventLoopSession* session);
bool runtime_host_event_loop_session_has_posted_callbacks(const RuntimeHostEventLoopSession* session);
int runtime_host_event_loop_session_posted_callback_pending_count(const RuntimeHostEventLoopSession* session);
int runtime_host_event_loop_session_drain_posted_callbacks(RuntimeHostEventLoopSession* session,
                                                           int max_callbacks);
int runtime_host_event_loop_session_drain_posted_callbacks_for_ms(RuntimeHostEventLoopSession* session,
                                                                  int max_callbacks,
                                                                  int64_t max_millis);
bool runtime_host_event_loop_session_wait_for_posted_callbacks(RuntimeHostEventLoopSession* session,
                                                               int64_t timeout_millis);
int runtime_host_event_loop_session_wait_and_drain_posted_callbacks(RuntimeHostEventLoopSession* session,
                                                                    int max_callbacks,
                                                                    int64_t timeout_millis);
bool runtime_host_event_loop_session_add_typed_event(RuntimeHostEventLoopSession* session,
                                                     RuntimePostedTypedEvent event);

static inline bool runtime_host_event_loop_session_add_input_event(RuntimeHostEventLoopSession* session,
                                                                   const RuntimePostedInputEvent* event) {
    return runtime_host_event_loop_session_add_typed_event(session,
                                                           runtime_posted_typed_input_event(event));
}

static inline bool runtime_host_event_loop_session_add_input_state_event(RuntimeHostEventLoopSession* session,
                                                                         const RuntimePostedInputEvent* event,
                                                                         const RuntimePostedInputStatePayloadSpec* payload_spec,
                                                                         const RuntimePostedEventContextSpec* context_spec) {
    return runtime_host_event_loop_session_add_typed_event(session,
                                                           runtime_posted_typed_input_state_event(event,
                                                                                                  payload_spec,
                                                                                                  context_spec));
}

static inline bool runtime_host_event_loop_session_add_input_combo_event(RuntimeHostEventLoopSession* session,
                                                                         const RuntimePostedInputEvent* event,
                                                                         const RuntimePostedInputComboPayloadSpec* payload_spec,
                                                                         const RuntimePostedEventContextSpec* context_spec) {
    return runtime_host_event_loop_session_add_typed_event(session,
                                                           runtime_posted_typed_input_combo_event(event,
                                                                                                  payload_spec,
                                                                                                  context_spec));
}

static inline bool runtime_host_event_loop_session_add_window_event(RuntimeHostEventLoopSession* session,
                                                                    const RuntimePostedWindowEvent* event) {
    return runtime_host_event_loop_session_add_typed_event(session,
                                                           runtime_posted_typed_window_event(event));
}

static inline bool runtime_host_event_loop_session_add_window_rect_event(RuntimeHostEventLoopSession* session,
                                                                         const RuntimePostedWindowEvent* event,
                                                                         const RuntimePostedWindowRectPayloadSpec* payload_spec,
                                                                         const RuntimePostedEventContextSpec* context_spec) {
    return runtime_host_event_loop_session_add_typed_event(session,
                                                           runtime_posted_typed_window_rect_event(event,
                                                                                                  payload_spec,
                                                                                                  context_spec));
}

static inline bool runtime_host_event_loop_session_add_frame_event(RuntimeHostEventLoopSession* session,
                                                                   const RuntimePostedFrameEvent* event) {
    return runtime_host_event_loop_session_add_typed_event(session,
                                                           runtime_posted_typed_frame_event(event));
}

static inline bool runtime_host_event_loop_session_add_frame_marker_event(RuntimeHostEventLoopSession* session,
                                                                          const RuntimePostedFrameEvent* event,
                                                                          const RuntimePostedFrameMarkerPayloadSpec* payload_spec,
                                                                          const RuntimePostedEventContextSpec* context_spec) {
    return runtime_host_event_loop_session_add_typed_event(session,
                                                           runtime_posted_typed_frame_marker_event(event,
                                                                                                  payload_spec,
                                                                                                  context_spec));
}

static inline RuntimePostedEventExtraValue runtime_posted_event_extra_int(int64_t value) {
    return (RuntimePostedEventExtraValue){
        .kind = RUNTIME_POSTED_EVENT_FIELD_INT,
        .int_value = value
    };
}

static inline RuntimePostedEventExtraValue runtime_posted_event_extra_bool(bool value) {
    return (RuntimePostedEventExtraValue){
        .kind = RUNTIME_POSTED_EVENT_FIELD_BOOL,
        .bool_value = value
    };
}

static inline RuntimePostedEventExtraValue runtime_posted_event_extra_double(double value) {
    return (RuntimePostedEventExtraValue){
        .kind = RUNTIME_POSTED_EVENT_FIELD_DOUBLE,
        .double_value = value
    };
}

static inline RuntimePostedEventExtraValue runtime_posted_event_extra_string(const char* value) {
    return (RuntimePostedEventExtraValue){
        .kind = RUNTIME_POSTED_EVENT_FIELD_STRING,
        .string_value = value
    };
}

static inline RuntimePostedEventExtraValue runtime_posted_event_extra_array(const RuntimePostedEventExtraValue* items,
                                                                            int item_count) {
    return (RuntimePostedEventExtraValue){
        .kind = RUNTIME_POSTED_EVENT_FIELD_ARRAY,
        .items = items,
        .item_count = item_count
    };
}

static inline RuntimePostedEventExtraValue runtime_posted_event_extra_tuple(const RuntimePostedEventExtraValue* items,
                                                                            int item_count) {
    return (RuntimePostedEventExtraValue){
        .kind = RUNTIME_POSTED_EVENT_FIELD_TUPLE,
        .items = items,
        .item_count = item_count
    };
}

static inline RuntimePostedEventExtraValue runtime_posted_event_extra_map(const RuntimePostedEventExtraMapEntry* entries,
                                                                          int entry_count) {
    return (RuntimePostedEventExtraValue){
        .kind = RUNTIME_POSTED_EVENT_FIELD_MAP,
        .map_entries = entries,
        .map_entry_count = entry_count
    };
}

static inline RuntimePostedEventExtraMapEntry runtime_posted_event_extra_map_entry(const char* key,
                                                                                   RuntimePostedEventExtraValue value) {
    return (RuntimePostedEventExtraMapEntry){
        .key = key,
        .value = value
    };
}

static inline RuntimePostedEventExtraField runtime_posted_event_extra_field(const char* name,
                                                                            RuntimePostedEventExtraValue value) {
    return (RuntimePostedEventExtraField){
        .name = name,
        .value = value
    };
}

static inline RuntimePostedEventExtraField runtime_posted_event_payload_field(RuntimePostedEventExtraValue value) {
    return runtime_posted_event_extra_field("payload", value);
}

static inline RuntimePostedEventExtraField runtime_posted_event_context_field(RuntimePostedEventExtraValue value) {
    return runtime_posted_event_extra_field("context", value);
}

static inline RuntimePostedEventExtraField runtime_posted_event_payload_map_field(const RuntimePostedEventExtraMapEntry* entries,
                                                                                  int entry_count) {
    return runtime_posted_event_payload_field(runtime_posted_event_extra_map(entries, entry_count));
}

static inline RuntimePostedEventExtraField runtime_posted_event_context_map_field(const RuntimePostedEventExtraMapEntry* entries,
                                                                                  int entry_count) {
    return runtime_posted_event_context_field(runtime_posted_event_extra_map(entries, entry_count));
}

static inline void runtime_posted_event_extra_array_builder_init(RuntimePostedEventExtraArrayBuilder* builder,
                                                                 RuntimePostedEventExtraValue* items,
                                                                 int capacity) {
    if (!builder) return;
    builder->items = items;
    builder->capacity = capacity;
    builder->count = 0;
}

static inline bool runtime_posted_event_extra_array_builder_add_value(RuntimePostedEventExtraArrayBuilder* builder,
                                                                      RuntimePostedEventExtraValue value) {
    if (!builder || !builder->items || builder->count < 0 || builder->count >= builder->capacity) {
        return false;
    }
    builder->items[builder->count++] = value;
    return true;
}

static inline bool runtime_posted_event_extra_array_builder_add_int(RuntimePostedEventExtraArrayBuilder* builder,
                                                                    int64_t value) {
    return runtime_posted_event_extra_array_builder_add_value(builder, runtime_posted_event_extra_int(value));
}

static inline bool runtime_posted_event_extra_array_builder_add_bool(RuntimePostedEventExtraArrayBuilder* builder,
                                                                     bool value) {
    return runtime_posted_event_extra_array_builder_add_value(builder, runtime_posted_event_extra_bool(value));
}

static inline bool runtime_posted_event_extra_array_builder_add_double(RuntimePostedEventExtraArrayBuilder* builder,
                                                                       double value) {
    return runtime_posted_event_extra_array_builder_add_value(builder, runtime_posted_event_extra_double(value));
}

static inline bool runtime_posted_event_extra_array_builder_add_string(RuntimePostedEventExtraArrayBuilder* builder,
                                                                       const char* value) {
    return runtime_posted_event_extra_array_builder_add_value(builder, runtime_posted_event_extra_string(value));
}

static inline RuntimePostedEventExtraValue runtime_posted_event_extra_array_builder_build_array(const RuntimePostedEventExtraArrayBuilder* builder) {
    return runtime_posted_event_extra_array(builder ? builder->items : NULL, builder ? builder->count : 0);
}

static inline RuntimePostedEventExtraValue runtime_posted_event_extra_array_builder_build_tuple(const RuntimePostedEventExtraArrayBuilder* builder) {
    return runtime_posted_event_extra_tuple(builder ? builder->items : NULL, builder ? builder->count : 0);
}

static inline void runtime_posted_event_extra_map_builder_init(RuntimePostedEventExtraMapBuilder* builder,
                                                               RuntimePostedEventExtraMapEntry* entries,
                                                               int capacity) {
    if (!builder) return;
    builder->entries = entries;
    builder->capacity = capacity;
    builder->count = 0;
}

static inline bool runtime_posted_event_extra_map_builder_add_value(RuntimePostedEventExtraMapBuilder* builder,
                                                                    const char* key,
                                                                    RuntimePostedEventExtraValue value) {
    if (!builder || !builder->entries || builder->count < 0 || builder->count >= builder->capacity || !key || key[0] == '\0') {
        return false;
    }
    builder->entries[builder->count++] = runtime_posted_event_extra_map_entry(key, value);
    return true;
}

static inline bool runtime_posted_event_extra_map_builder_add_int(RuntimePostedEventExtraMapBuilder* builder,
                                                                  const char* key,
                                                                  int64_t value) {
    return runtime_posted_event_extra_map_builder_add_value(builder, key, runtime_posted_event_extra_int(value));
}

static inline bool runtime_posted_event_extra_map_builder_add_bool(RuntimePostedEventExtraMapBuilder* builder,
                                                                   const char* key,
                                                                   bool value) {
    return runtime_posted_event_extra_map_builder_add_value(builder, key, runtime_posted_event_extra_bool(value));
}

static inline bool runtime_posted_event_extra_map_builder_add_double(RuntimePostedEventExtraMapBuilder* builder,
                                                                     const char* key,
                                                                     double value) {
    return runtime_posted_event_extra_map_builder_add_value(builder, key, runtime_posted_event_extra_double(value));
}

static inline bool runtime_posted_event_extra_map_builder_add_string(RuntimePostedEventExtraMapBuilder* builder,
                                                                     const char* key,
                                                                     const char* value) {
    return runtime_posted_event_extra_map_builder_add_value(builder, key, runtime_posted_event_extra_string(value));
}

static inline RuntimePostedEventExtraValue runtime_posted_event_extra_map_builder_build(const RuntimePostedEventExtraMapBuilder* builder) {
    return runtime_posted_event_extra_map(builder ? builder->entries : NULL, builder ? builder->count : 0);
}

static inline RuntimePostedEventExtraValue runtime_posted_event_build_input_state_payload_map(RuntimePostedEventExtraMapEntry* entries,
                                                                                              int capacity,
                                                                                              const RuntimePostedInputStatePayloadSpec* spec) {
    RuntimePostedEventExtraMapBuilder builder;
    runtime_posted_event_extra_map_builder_init(&builder, entries, capacity);
    if (spec) {
        (void)runtime_posted_event_extra_map_builder_add_string(&builder, "label", spec->label ? spec->label : "");
        (void)runtime_posted_event_extra_map_builder_add_bool(&builder, "active", spec->active);
        (void)runtime_posted_event_extra_map_builder_add_int(&builder, "score", spec->score);
        (void)runtime_posted_event_extra_map_builder_add_value(&builder,
                                                               "coords",
                                                               runtime_posted_event_extra_tuple(spec->coords_items, spec->coords_count));
        (void)runtime_posted_event_extra_map_builder_add_value(&builder,
                                                               "buttons",
                                                               runtime_posted_event_extra_array(spec->buttons_items, spec->button_count));
    }
    return runtime_posted_event_extra_map_builder_build(&builder);
}

static inline RuntimePostedEventExtraValue runtime_posted_event_build_input_combo_payload_map(RuntimePostedEventExtraMapEntry* entries,
                                                                                              int capacity,
                                                                                              const RuntimePostedInputComboPayloadSpec* spec) {
    RuntimePostedEventExtraMapBuilder builder;
    runtime_posted_event_extra_map_builder_init(&builder, entries, capacity);
    if (spec) {
        (void)runtime_posted_event_extra_map_builder_add_int(&builder, "score", spec->score);
        (void)runtime_posted_event_extra_map_builder_add_value(&builder,
                                                               "combo",
                                                               runtime_posted_event_extra_array(spec->combo_items, spec->combo_count));
    }
    return runtime_posted_event_extra_map_builder_build(&builder);
}

static inline RuntimePostedEventExtraValue runtime_posted_event_build_window_rect_payload_map(RuntimePostedEventExtraMapEntry* entries,
                                                                                              int capacity,
                                                                                              const RuntimePostedWindowRectPayloadSpec* spec) {
    RuntimePostedEventExtraMapBuilder builder;
    runtime_posted_event_extra_map_builder_init(&builder, entries, capacity);
    if (spec) {
        (void)runtime_posted_event_extra_map_builder_add_string(&builder, "title", spec->title ? spec->title : "");
        (void)runtime_posted_event_extra_map_builder_add_value(&builder,
                                                               "rect",
                                                               runtime_posted_event_extra_tuple(spec->rect_items, spec->rect_count));
    }
    return runtime_posted_event_extra_map_builder_build(&builder);
}

static inline RuntimePostedEventExtraValue runtime_posted_event_build_frame_marker_payload_map(RuntimePostedEventExtraMapEntry* entries,
                                                                                               int capacity,
                                                                                               const RuntimePostedFrameMarkerPayloadSpec* spec) {
    RuntimePostedEventExtraMapBuilder builder;
    runtime_posted_event_extra_map_builder_init(&builder, entries, capacity);
    if (spec) {
        (void)runtime_posted_event_extra_map_builder_add_string(&builder, "phase", spec->phase ? spec->phase : "");
        (void)runtime_posted_event_extra_map_builder_add_value(&builder,
                                                               "markers",
                                                               runtime_posted_event_extra_array(spec->marker_items, spec->marker_count));
    }
    return runtime_posted_event_extra_map_builder_build(&builder);
}

static inline RuntimePostedEventExtraValue runtime_posted_event_build_context_map(RuntimePostedEventExtraMapEntry* entries,
                                                                                  int capacity,
                                                                                  const RuntimePostedEventContextSpec* spec) {
    RuntimePostedEventExtraMapBuilder builder;
    runtime_posted_event_extra_map_builder_init(&builder, entries, capacity);
    if (spec) {
        (void)runtime_posted_event_extra_map_builder_add_string(&builder, "owner", spec->owner ? spec->owner : "");
        (void)runtime_posted_event_extra_map_builder_add_int(&builder, "frame", spec->frame);
    }
    return runtime_posted_event_extra_map_builder_build(&builder);
}

static inline RuntimePostedEventExtraField runtime_posted_event_build_input_state_payload_field(RuntimePostedEventExtraMapEntry* entries,
                                                                                                int capacity,
                                                                                                const RuntimePostedInputStatePayloadSpec* spec) {
    return runtime_posted_event_payload_field(runtime_posted_event_build_input_state_payload_map(entries, capacity, spec));
}

static inline RuntimePostedEventExtraField runtime_posted_event_build_input_combo_payload_field(RuntimePostedEventExtraMapEntry* entries,
                                                                                                int capacity,
                                                                                                const RuntimePostedInputComboPayloadSpec* spec) {
    return runtime_posted_event_payload_field(runtime_posted_event_build_input_combo_payload_map(entries, capacity, spec));
}

static inline RuntimePostedEventExtraField runtime_posted_event_build_window_rect_payload_field(RuntimePostedEventExtraMapEntry* entries,
                                                                                                int capacity,
                                                                                                const RuntimePostedWindowRectPayloadSpec* spec) {
    return runtime_posted_event_payload_field(runtime_posted_event_build_window_rect_payload_map(entries, capacity, spec));
}

static inline RuntimePostedEventExtraField runtime_posted_event_build_frame_marker_payload_field(RuntimePostedEventExtraMapEntry* entries,
                                                                                                 int capacity,
                                                                                                 const RuntimePostedFrameMarkerPayloadSpec* spec) {
    return runtime_posted_event_payload_field(runtime_posted_event_build_frame_marker_payload_map(entries, capacity, spec));
}

static inline RuntimePostedEventExtraField runtime_posted_event_build_context_field(RuntimePostedEventExtraMapEntry* entries,
                                                                                    int capacity,
                                                                                    const RuntimePostedEventContextSpec* spec) {
    return runtime_posted_event_context_field(runtime_posted_event_build_context_map(entries, capacity, spec));
}

Runtime* runtime_create_with_options(const char* file_path, RuntimeOptions options);
void runtime_free(Runtime* rt);
void runtime_set_argv(Runtime* rt, int argc, char** argv);
void runtime_set_thread_channels(Runtime* rt, int inbox_channel_id, int outbox_channel_id);
int runtime_run(Runtime* rt);
int runtime_resume(Runtime* rt);
int runtime_run_function(Runtime* rt, const char* function_name);
bool runtime_take_return_value(Runtime* rt, Value* out);
bool runtime_has_error(Runtime* rt);
const char* runtime_get_error(Runtime* rt);
RuntimeLoadMode runtime_get_load_mode(Runtime* rt);
const char* runtime_get_cache_path(Runtime* rt);
bool runtime_has_posted_callbacks(Runtime* rt);
int runtime_posted_callback_pending_count(Runtime* rt);
bool runtime_close_posted_callback_queue(Runtime* rt);
bool runtime_is_posted_callback_queue_open(Runtime* rt);
int runtime_drain_posted_callbacks(Runtime* rt, int max_callbacks);
int runtime_drain_posted_callbacks_for_ms(Runtime* rt, int max_callbacks, int64_t max_millis);
bool runtime_wait_for_posted_callbacks(Runtime* rt, int64_t timeout_millis);
int runtime_wait_and_drain_posted_callbacks(Runtime* rt, int max_callbacks, int64_t timeout_millis);
bool runtime_get_posted_callback_auto_drain(Runtime* rt);
bool runtime_set_posted_callback_auto_drain(Runtime* rt, bool enabled);
bool runtime_post_input_event(Runtime* rt,
                              const char* callback_name,
                              const RuntimePostedInputEvent* event,
                              char* error_buf,
                              size_t error_buf_size);
bool runtime_post_input_state_event(Runtime* rt,
                                    const char* callback_name,
                                    const RuntimePostedInputEvent* event,
                                    const RuntimePostedInputStatePayloadSpec* payload_spec,
                                    const RuntimePostedEventContextSpec* context_spec,
                                    char* error_buf,
                                    size_t error_buf_size);
bool runtime_post_input_combo_event(Runtime* rt,
                                    const char* callback_name,
                                    const RuntimePostedInputEvent* event,
                                    const RuntimePostedInputComboPayloadSpec* payload_spec,
                                    const RuntimePostedEventContextSpec* context_spec,
                                    char* error_buf,
                                    size_t error_buf_size);
bool runtime_post_window_event(Runtime* rt,
                               const char* callback_name,
                               const RuntimePostedWindowEvent* event,
                               char* error_buf,
                               size_t error_buf_size);
bool runtime_post_window_rect_event(Runtime* rt,
                                    const char* callback_name,
                                    const RuntimePostedWindowEvent* event,
                                    const RuntimePostedWindowRectPayloadSpec* payload_spec,
                                    const RuntimePostedEventContextSpec* context_spec,
                                    char* error_buf,
                                    size_t error_buf_size);
bool runtime_post_frame_event(Runtime* rt,
                              const char* callback_name,
                              const RuntimePostedFrameEvent* event,
                              char* error_buf,
                              size_t error_buf_size);
bool runtime_post_frame_marker_event(Runtime* rt,
                                     const char* callback_name,
                                     const RuntimePostedFrameEvent* event,
                                     const RuntimePostedFrameMarkerPayloadSpec* payload_spec,
                                     const RuntimePostedEventContextSpec* context_spec,
                                     char* error_buf,
                                     size_t error_buf_size);
bool runtime_post_input_event_batch(Runtime* rt,
                                    const char* callback_name,
                                    const RuntimePostedInputEvent* events,
                                    int event_count,
                                    char* error_buf,
                                    size_t error_buf_size);
bool runtime_post_input_state_event_batch(Runtime* rt,
                                          const char* callback_name,
                                          const RuntimePostedInputEvent* events,
                                          const RuntimePostedInputStatePayloadSpec* payload_specs,
                                          const RuntimePostedEventContextSpec* context_specs,
                                          int event_count,
                                          char* error_buf,
                                          size_t error_buf_size);
bool runtime_post_input_combo_event_batch(Runtime* rt,
                                          const char* callback_name,
                                          const RuntimePostedInputEvent* events,
                                          const RuntimePostedInputComboPayloadSpec* payload_specs,
                                          const RuntimePostedEventContextSpec* context_specs,
                                          int event_count,
                                          char* error_buf,
                                          size_t error_buf_size);
bool runtime_post_window_event_batch(Runtime* rt,
                                     const char* callback_name,
                                     const RuntimePostedWindowEvent* events,
                                     int event_count,
                                     char* error_buf,
                                     size_t error_buf_size);
bool runtime_post_window_rect_event_batch(Runtime* rt,
                                          const char* callback_name,
                                          const RuntimePostedWindowEvent* events,
                                          const RuntimePostedWindowRectPayloadSpec* payload_specs,
                                          const RuntimePostedEventContextSpec* context_specs,
                                          int event_count,
                                          char* error_buf,
                                          size_t error_buf_size);
bool runtime_post_frame_event_batch(Runtime* rt,
                                    const char* callback_name,
                                    const RuntimePostedFrameEvent* events,
                                    int event_count,
                                    char* error_buf,
                                    size_t error_buf_size);
bool runtime_post_frame_marker_event_batch(Runtime* rt,
                                           const char* callback_name,
                                           const RuntimePostedFrameEvent* events,
                                           const RuntimePostedFrameMarkerPayloadSpec* payload_specs,
                                           const RuntimePostedEventContextSpec* context_specs,
                                           int event_count,
                                           char* error_buf,
                                           size_t error_buf_size);
bool runtime_post_mixed_event_batch(Runtime* rt,
                                    const char* callback_name,
                                    const RuntimePostedEvent* events,
                                    int event_count,
                                    char* error_buf,
                                    size_t error_buf_size);
bool runtime_post_typed_mixed_event_batch(Runtime* rt,
                                          const char* callback_name,
                                          const RuntimePostedTypedEvent* events,
                                          int event_count,
                                          char* error_buf,
                                          size_t error_buf_size);
bool runtime_post_typed_family_mixed_event_batch(Runtime* rt,
                                                 const char* callback_name,
                                                 const RuntimePostedTypedInputBatchSpec* input_batch,
                                                 const RuntimePostedTypedWindowBatchSpec* window_batch,
                                                 const RuntimePostedTypedFrameBatchSpec* frame_batch,
                                                 char* error_buf,
                                                 size_t error_buf_size);
bool runtime_post_frame_envelope_batch(Runtime* rt,
                                       const char* callback_name,
                                       const RuntimePostedFrameEnvelopeBatchSpec* envelope,
                                       char* error_buf,
                                       size_t error_buf_size);
bool runtime_post_frame_envelope_builder(Runtime* rt,
                                         const char* callback_name,
                                         const RuntimePostedFrameEnvelopeBuilder* builder,
                                         char* error_buf,
                                         size_t error_buf_size);
bool runtime_post_frame_envelope_heap_builder(Runtime* rt,
                                              const char* callback_name,
                                              const RuntimePostedFrameEnvelopeHeapBuilder* builder,
                                              char* error_buf,
                                              size_t error_buf_size);
bool runtime_host_event_loop_session_flush_frame(RuntimeHostEventLoopSession* session,
                                                 const char* callback_name,
                                                 char* error_buf,
                                                 size_t error_buf_size);
bool runtime_host_event_loop_session_flush_frame_and_drain_posted_callbacks_for_ms(
    RuntimeHostEventLoopSession* session,
    const char* callback_name,
    int max_callbacks,
    int64_t max_millis,
    int* drained_callbacks,
    char* error_buf,
    size_t error_buf_size);

static inline bool runtime_host_event_loop_session_flush_frame_and_drain_posted_callbacks(
    RuntimeHostEventLoopSession* session,
    const char* callback_name,
    int max_callbacks,
    int* drained_callbacks,
    char* error_buf,
    size_t error_buf_size) {
    return runtime_host_event_loop_session_flush_frame_and_drain_posted_callbacks_for_ms(session,
                                                                                         callback_name,
                                                                                         max_callbacks,
                                                                                         0,
                                                                                         drained_callbacks,
                                                                                         error_buf,
                                                                                         error_buf_size);
}
bool runtime_host_event_loop_session_end_frame(RuntimeHostEventLoopSession* session,
                                               int* drained_callbacks,
                                               char* error_buf,
                                               size_t error_buf_size);
bool runtime_host_event_loop_session_tick(RuntimeHostEventLoopSession* session,
                                          bool wait_for_callbacks,
                                          int64_t wait_timeout_millis,
                                          int* drained_callbacks,
                                          char* error_buf,
                                          size_t error_buf_size);
bool runtime_host_event_loop_session_tick_default(RuntimeHostEventLoopSession* session,
                                                  int* drained_callbacks,
                                                  char* error_buf,
                                                  size_t error_buf_size);
bool runtime_host_event_loop_session_step_with_options(
    RuntimeHostEventLoopSession* session,
    bool post_frame,
    const RuntimeHostEventLoopSessionStepOptions* options,
    RuntimeHostEventLoopSessionStepResult* result,
    char* error_buf,
    size_t error_buf_size);
bool runtime_host_event_loop_session_step(RuntimeHostEventLoopSession* session,
                                          bool post_frame,
                                          RuntimeHostEventLoopSessionStepResult* result,
                                          char* error_buf,
                                          size_t error_buf_size);

#endif
