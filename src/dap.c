#include "dap.h"

#include "cJSON.h"
#include "cli.h"
#include "runtime.h"
#include "safe_alloc.h"
#include "typechecker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "tablo_threads.h"

#ifndef TABLO_VERSION
#define TABLO_VERSION "dev"
#endif

typedef struct {
    char* source_path;
    int line;
    char* condition;
    int hit_condition_kind;
    int hit_condition_value;
    int hit_count;
} DapLineBreakpoint;

typedef struct {
    DapLineBreakpoint* items;
    int count;
    int capacity;
} DapBreakpointList;

typedef enum {
    DAP_EXEC_START = 0,
    DAP_EXEC_CONTINUE = 1,
    DAP_EXEC_STEP_IN = 2,
    DAP_EXEC_STEP_OVER = 3,
    DAP_EXEC_STEP_OUT = 4
} DapExecAction;

typedef struct {
    bool stopped;
    const char* stop_reason;
    const char* stop_text;
    bool terminated;
    bool has_error;
    const char* error_message;
} DapExecutionResult;

typedef struct {
    struct DapSession* session;
    DapExecAction action;
} DapExecThreadArgs;

typedef struct {
    int ref_id;
    Value value;
} DapVariableRef;

typedef struct {
    DapVariableRef* items;
    int count;
    int capacity;
    int next_ref;
} DapVariableRefList;

typedef struct DapSession {
    Runtime* runtime;
    RuntimeOptions options;
    char* program_path;
    char** argv;
    int argc;
    bool launched;
    bool execution_started;
    bool execution_running;
    bool execution_stopped;
    bool terminated;
    int stop_generation;
    bool stop_on_runtime_error;
    bool stop_on_entry;
    bool worker_joinable;
    thrd_t worker_thread;
    mtx_t state_mutex;
    cnd_t state_cond;
    mtx_t io_mutex;
    DapBreakpointList breakpoints;
    DapVariableRefList variable_refs;
    char* output_buf;
    int output_len;
    int output_cap;
    int next_seq;
} DapSession;

enum {
    DAP_STOP_ID_STRIDE = 10000,
    DAP_VARIABLES_REF_ARGUMENTS_BASE = 1000000,
    DAP_VARIABLES_REF_LOCALS_BASE = 2000000,
    DAP_VARIABLES_REF_GLOBALS_BASE = 3000000,
    DAP_VARIABLES_REF_CHILDREN_BASE = 4000000
};

typedef enum {
    DAP_HIT_CONDITION_NONE = 0,
    DAP_HIT_CONDITION_EXACT = 1,
    DAP_HIT_CONDITION_AT_LEAST = 2,
    DAP_HIT_CONDITION_MODULO = 3
} DapHitConditionKind;

static int dap_eval_expression(DapSession* session,
                               int frame_id,
                               const char* expression,
                               Value* out_value,
                               const char** out_error);
static DapExecutionResult dap_resume_execution(DapSession* session, DapExecAction action);
static void dap_emit_post_execution(DapSession* session, const DapExecutionResult* result);

static void dap_breakpoint_list_init(DapBreakpointList* list) {
    if (!list) return;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void dap_breakpoint_list_free(DapBreakpointList* list) {
    if (!list) return;
    if (list->items) {
        for (int i = 0; i < list->count; i++) {
            if (list->items[i].source_path) free(list->items[i].source_path);
            if (list->items[i].condition) free(list->items[i].condition);
        }
        free(list->items);
    }
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int dap_breakpoint_list_push(DapBreakpointList* list,
                                    const char* source_path,
                                    int line,
                                    const char* condition,
                                    int hit_condition_kind,
                                    int hit_condition_value) {
    if (!list || !source_path || source_path[0] == '\0' || line <= 0) return 0;

    if (list->count >= list->capacity) {
        int new_capacity = list->capacity > 0 ? list->capacity * 2 : 8;
        DapLineBreakpoint* new_items =
            (DapLineBreakpoint*)realloc(list->items, (size_t)new_capacity * sizeof(DapLineBreakpoint));
        if (!new_items) return 0;
        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count].source_path = safe_strdup(source_path);
    if (!list->items[list->count].source_path) return 0;
    list->items[list->count].condition = NULL;
    if (condition && condition[0] != '\0') {
        list->items[list->count].condition = safe_strdup(condition);
        if (!list->items[list->count].condition) {
            free(list->items[list->count].source_path);
            list->items[list->count].source_path = NULL;
            return 0;
        }
    }
    list->items[list->count].line = line;
    list->items[list->count].hit_condition_kind = hit_condition_kind;
    list->items[list->count].hit_condition_value = hit_condition_value;
    list->items[list->count].hit_count = 0;
    list->count++;
    return 1;
}

static void dap_breakpoint_list_remove_source(DapBreakpointList* list, const char* source_path) {
    if (!list || !source_path) return;
    for (int i = 0; i < list->count;) {
        DapLineBreakpoint* bp = &list->items[i];
        if (bp->source_path && strcmp(bp->source_path, source_path) == 0) {
            free(bp->source_path);
            if (bp->condition) free(bp->condition);
            if (i + 1 < list->count) {
                memmove(&list->items[i],
                        &list->items[i + 1],
                        (size_t)(list->count - i - 1) * sizeof(DapLineBreakpoint));
            }
            list->count--;
            continue;
        }
        i++;
    }
}

static void dap_variable_ref_list_init(DapVariableRefList* list) {
    if (!list) return;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    list->next_ref = DAP_VARIABLES_REF_CHILDREN_BASE;
}

static void dap_variable_ref_list_clear(DapVariableRefList* list) {
    if (!list) return;
    if (list->items) {
        for (int i = 0; i < list->count; i++) {
            value_free(&list->items[i].value);
        }
        free(list->items);
    }
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void dap_variable_ref_list_set_stop_generation(DapVariableRefList* list, int stop_generation) {
    if (!list) return;
    list->next_ref = DAP_VARIABLES_REF_CHILDREN_BASE + (stop_generation * DAP_STOP_ID_STRIDE);
}

static DapVariableRef* dap_variable_ref_list_find(DapVariableRefList* list, int ref_id) {
    if (!list || ref_id < DAP_VARIABLES_REF_CHILDREN_BASE) return NULL;
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].ref_id == ref_id) {
            return &list->items[i];
        }
    }
    return NULL;
}

static int dap_variable_ref_list_add(DapVariableRefList* list, const Value* value) {
    DapVariableRef* item = NULL;

    if (!list || !value) return 0;
    if (list->count >= list->capacity) {
        int new_capacity = list->capacity > 0 ? list->capacity * 2 : 16;
        DapVariableRef* new_items =
            (DapVariableRef*)realloc(list->items, (size_t)new_capacity * sizeof(DapVariableRef));
        if (!new_items) return 0;
        list->items = new_items;
        list->capacity = new_capacity;
    }

    item = &list->items[list->count++];
    item->ref_id = list->next_ref++;
    item->value = *value;
    value_retain(&item->value);
    return item->ref_id;
}

static int dap_session_init(DapSession* session) {
    if (!session) return 0;
    memset(session, 0, sizeof(*session));
    dap_breakpoint_list_init(&session->breakpoints);
    dap_variable_ref_list_init(&session->variable_refs);
    dap_variable_ref_list_set_stop_generation(&session->variable_refs, 0);
    if (mtx_init(&session->state_mutex, mtx_plain) != thrd_success) {
        return 0;
    }
    if (cnd_init(&session->state_cond) != thrd_success) {
        mtx_destroy(&session->state_mutex);
        return 0;
    }
    if (mtx_init(&session->io_mutex, mtx_plain) != thrd_success) {
        cnd_destroy(&session->state_cond);
        mtx_destroy(&session->state_mutex);
        return 0;
    }
    session->next_seq = 1;
    return 1;
}

static void dap_session_join_worker(DapSession* session) {
    thrd_t worker = {0};
    int join_result = 0;
    bool should_join = false;

    if (!session) return;

    mtx_lock(&session->state_mutex);
    should_join = session->worker_joinable && !session->execution_running;
    if (should_join) {
        worker = session->worker_thread;
        session->worker_joinable = false;
    }
    mtx_unlock(&session->state_mutex);

    if (should_join) {
        thrd_join(worker, &join_result);
        (void)join_result;
    }
}

static void dap_session_wait_until_not_running(DapSession* session) {
    if (!session) return;

    mtx_lock(&session->state_mutex);
    while (session->execution_running) {
        cnd_wait(&session->state_cond, &session->state_mutex);
    }
    mtx_unlock(&session->state_mutex);
    dap_session_join_worker(session);
}

static void dap_session_snapshot(DapSession* session, bool* out_running, bool* out_stopped, bool* out_terminated) {
    if (out_running) *out_running = false;
    if (out_stopped) *out_stopped = false;
    if (out_terminated) *out_terminated = false;
    if (!session) return;

    mtx_lock(&session->state_mutex);
    if (out_running) *out_running = session->execution_running;
    if (out_stopped) *out_stopped = session->execution_stopped;
    if (out_terminated) *out_terminated = session->terminated;
    mtx_unlock(&session->state_mutex);
}

static void dap_session_free_runtime(DapSession* session) {
    if (!session || !session->runtime) return;
    vm_set_output_callback(session->runtime->vm, NULL, NULL);
    dap_variable_ref_list_clear(&session->variable_refs);
    session->stop_generation = 0;
    dap_variable_ref_list_set_stop_generation(&session->variable_refs, session->stop_generation);
    runtime_free(session->runtime);
    session->runtime = NULL;
    session->execution_started = false;
    session->execution_running = false;
    session->execution_stopped = false;
    session->stop_on_entry = false;
}

static void dap_session_free(DapSession* session) {
    if (!session) return;
    if (session->runtime && session->execution_running) {
        vm_debug_request_pause(session->runtime->vm);
    }
    dap_session_wait_until_not_running(session);
    dap_session_free_runtime(session);
    if (session->program_path) free(session->program_path);
    session->program_path = NULL;
    if (session->argv) {
        for (int i = 0; i < session->argc; i++) {
            if (session->argv[i]) free(session->argv[i]);
        }
        free(session->argv);
    }
    session->argv = NULL;
    session->argc = 0;
    dap_breakpoint_list_free(&session->breakpoints);
    dap_variable_ref_list_clear(&session->variable_refs);
    if (session->output_buf) free(session->output_buf);
    session->output_buf = NULL;
    session->output_len = 0;
    session->output_cap = 0;
    session->launched = false;
    session->terminated = false;
    session->stop_on_runtime_error = false;
    session->stop_on_entry = false;
    mtx_destroy(&session->io_mutex);
    cnd_destroy(&session->state_cond);
    mtx_destroy(&session->state_mutex);
}

static void dap_output_capture(void* user_data, const char* text, int length) {
    DapSession* session = (DapSession*)user_data;
    if (!session || !text || length <= 0) return;

    if (session->output_len + length + 1 > session->output_cap) {
        int new_cap = session->output_cap > 0 ? session->output_cap * 2 : 128;
        while (new_cap < session->output_len + length + 1) {
            new_cap *= 2;
        }
        char* new_buf = (char*)realloc(session->output_buf, (size_t)new_cap);
        if (!new_buf) return;
        session->output_buf = new_buf;
        session->output_cap = new_cap;
    }

    memcpy(session->output_buf + session->output_len, text, (size_t)length);
    session->output_len += length;
    session->output_buf[session->output_len] = '\0';
}

static const char* dap_path_basename(const char* path) {
    const char* name = path;
    if (!path) return "";
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }
    return name;
}

static int dap_ascii_starts_with_ignore_case(const char* text, const char* prefix) {
    if (!text || !prefix) return 0;
    while (*prefix) {
        if (*text == '\0') return 0;
        if (tolower((unsigned char)*text) != tolower((unsigned char)*prefix)) {
            return 0;
        }
        text++;
        prefix++;
    }
    return 1;
}

static char* dap_normalize_path_for_compare(const char* path) {
    if (!path) return NULL;
    size_t len = strlen(path);
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;

    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        char c = path[i];
        if (c == '\\' || c == '/') c = '/';
#ifdef _WIN32
        c = (char)tolower((unsigned char)c);
#endif
        out[pos++] = c;
    }
    out[pos] = '\0';

    while (pos > 1 && out[pos - 1] == '/') {
#ifdef _WIN32
        if (pos == 3 && out[1] == ':' && out[2] == '/') break;
#endif
        out[--pos] = '\0';
    }

    while (out[0] == '.' && out[1] == '/') {
        memmove(out, out + 2, strlen(out + 2) + 1);
    }

    return out;
}

static int dap_path_is_suffix_component(const char* text, const char* suffix) {
    if (!text || !suffix) return 0;
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > text_len) return 0;
    if (strcmp(text + (text_len - suffix_len), suffix) != 0) return 0;
    if (suffix_len == text_len) return 1;
    return text[text_len - suffix_len - 1] == '/';
}

static int dap_paths_match_loose(const char* a, const char* b) {
    int match = 0;
    char* aa = NULL;
    char* bb = NULL;

    if (!a || !b) return 0;
    aa = dap_normalize_path_for_compare(a);
    bb = dap_normalize_path_for_compare(b);
    if (!aa || !bb) {
        free(aa);
        free(bb);
        return 0;
    }

    if (strcmp(aa, bb) == 0) {
        match = 1;
    } else if (dap_path_is_suffix_component(aa, bb) || dap_path_is_suffix_component(bb, aa)) {
        match = 1;
    }

    free(aa);
    free(bb);
    return match;
}

static int dap_parse_positive_int_string(const char* text, int* out_value) {
    int value = 0;
    int saw_digit = 0;

    if (out_value) *out_value = 0;
    if (!text || !out_value) return 0;
    while (*text && isspace((unsigned char)*text)) text++;
    while (*text && isdigit((unsigned char)*text)) {
        saw_digit = 1;
        value = value * 10 + (*text - '0');
        text++;
    }
    while (*text && isspace((unsigned char)*text)) text++;
    if (!saw_digit || *text != '\0' || value <= 0) return 0;
    *out_value = value;
    return 1;
}

static int dap_parse_hit_condition(const char* text, int* out_kind, int* out_value) {
    int value = 0;

    if (out_kind) *out_kind = DAP_HIT_CONDITION_NONE;
    if (out_value) *out_value = 0;
    if (!text || text[0] == '\0') return 1;

    while (*text && isspace((unsigned char)*text)) text++;
    if (*text == '\0') return 1;

    if (text[0] == '>' && text[1] == '=') {
        if (!dap_parse_positive_int_string(text + 2, &value)) return 0;
        if (out_kind) *out_kind = DAP_HIT_CONDITION_AT_LEAST;
        if (out_value) *out_value = value;
        return 1;
    }
    if (text[0] == '=' && text[1] == '=') {
        if (!dap_parse_positive_int_string(text + 2, &value)) return 0;
        if (out_kind) *out_kind = DAP_HIT_CONDITION_EXACT;
        if (out_value) *out_value = value;
        return 1;
    }
    if (text[0] == '%') {
        if (!dap_parse_positive_int_string(text + 1, &value)) return 0;
        if (out_kind) *out_kind = DAP_HIT_CONDITION_MODULO;
        if (out_value) *out_value = value;
        return 1;
    }
    if (!dap_parse_positive_int_string(text, &value)) return 0;
    if (out_kind) *out_kind = DAP_HIT_CONDITION_EXACT;
    if (out_value) *out_value = value;
    return 1;
}

static DapLineBreakpoint* dap_find_breakpoint(DapSession* session, const char* source_file, int line) {
    if (!session || !source_file || line <= 0) return NULL;
    for (int i = 0; i < session->breakpoints.count; i++) {
        DapLineBreakpoint* bp = &session->breakpoints.items[i];
        if (bp->line != line) continue;
        if (!dap_paths_match_loose(source_file, bp->source_path)) continue;
        return bp;
    }
    return NULL;
}

static bool dap_value_is_truthy_for_breakpoint(const Value* value) {
    if (!value) return false;
    switch (value_get_type(value)) {
        case VAL_NIL:
            return false;
        case VAL_BOOL:
            return value_get_bool(value);
        case VAL_INT:
            return value_get_int(value) != 0;
        case VAL_DOUBLE:
            return value_get_double(value) != 0.0;
        case VAL_STRING: {
            ObjString* str = value_get_string_obj(value);
            return str && str->chars && str->length > 0;
        }
        default:
            return true;
    }
}

static bool dap_breakpoint_hit_condition_matches(DapLineBreakpoint* bp) {
    if (!bp) return false;
    switch (bp->hit_condition_kind) {
        case DAP_HIT_CONDITION_NONE:
            return true;
        case DAP_HIT_CONDITION_EXACT:
            return bp->hit_count == bp->hit_condition_value;
        case DAP_HIT_CONDITION_AT_LEAST:
            return bp->hit_count >= bp->hit_condition_value;
        case DAP_HIT_CONDITION_MODULO:
            return bp->hit_condition_value > 0 && (bp->hit_count % bp->hit_condition_value) == 0;
        default:
            return true;
    }
}

static bool dap_breakpoint_condition_matches(DapSession* session, DapLineBreakpoint* bp) {
    Value result_value;
    const char* eval_error = NULL;
    bool matches = false;

    if (!bp || !bp->condition || bp->condition[0] == '\0') return true;
    if (!dap_eval_expression(session, 1, bp->condition, &result_value, &eval_error)) {
        return false;
    }
    matches = dap_value_is_truthy_for_breakpoint(&result_value);
    value_free(&result_value);
    (void)eval_error;
    return matches;
}

static bool dap_breakpoint_should_stop(DapSession* session, DapLineBreakpoint* bp) {
    if (!bp) return true;
    bp->hit_count++;
    if (!dap_breakpoint_hit_condition_matches(bp)) return false;
    if (!dap_breakpoint_condition_matches(session, bp)) return false;
    return true;
}

static int dap_read_content_length(FILE* input, int* out_length) {
    char line[1024];
    int content_length = -1;

    if (!out_length) return 0;
    while (fgets(line, sizeof(line), input)) {
        if (strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0) {
            break;
        }
        if (dap_ascii_starts_with_ignore_case(line, "Content-Length:")) {
            const char* value = line + 15;
            while (*value == ' ' || *value == '\t') value++;
            content_length = atoi(value);
        }
    }

    if (content_length < 0) return 0;
    *out_length = content_length;
    return 1;
}

static int dap_write_message_locked(cJSON* message) {
    char* payload = NULL;
    int payload_len = 0;

    if (!message) return 0;
    payload = cJSON_PrintUnformatted(message);
    if (!payload) return 0;

    payload_len = (int)strlen(payload);
    printf("Content-Length: %d\r\n\r\n%s", payload_len, payload);
    fflush(stdout);
    free(payload);
    return 1;
}

static int dap_write_response(DapSession* session,
                              int request_seq,
                              const char* command,
                              int success,
                              cJSON* body,
                              const char* message) {
    cJSON* response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "seq", 0);
    cJSON_AddStringToObject(response, "type", "response");
    cJSON_AddNumberToObject(response, "request_seq", request_seq);
    cJSON_AddBoolToObject(response, "success", success ? 1 : 0);
    cJSON_AddStringToObject(response, "command", command ? command : "");
    if (message) {
        cJSON_AddStringToObject(response, "message", message);
    }
    if (body) {
        cJSON_AddItemToObject(response, "body", body);
    } else {
        cJSON_AddItemToObject(response, "body", cJSON_CreateObject());
    }
    mtx_lock(&session->io_mutex);
    cJSON_ReplaceItemInObject(response, "seq", cJSON_CreateNumber(session->next_seq++));
    int ok = dap_write_message_locked(response);
    mtx_unlock(&session->io_mutex);
    cJSON_Delete(response);
    return ok;
}

static int dap_write_event(DapSession* session, const char* event_name, cJSON* body) {
    cJSON* event = cJSON_CreateObject();
    cJSON_AddNumberToObject(event, "seq", 0);
    cJSON_AddStringToObject(event, "type", "event");
    cJSON_AddStringToObject(event, "event", event_name ? event_name : "");
    if (body) {
        cJSON_AddItemToObject(event, "body", body);
    } else {
        cJSON_AddItemToObject(event, "body", cJSON_CreateObject());
    }
    mtx_lock(&session->io_mutex);
    cJSON_ReplaceItemInObject(event, "seq", cJSON_CreateNumber(session->next_seq++));
    int ok = dap_write_message_locked(event);
    mtx_unlock(&session->io_mutex);
    cJSON_Delete(event);
    return ok;
}

static int dap_write_output_event(DapSession* session, const char* category, const char* output) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "category", category ? category : "console");
    cJSON_AddStringToObject(body, "output", output ? output : "");
    return dap_write_event(session, "output", body);
}

static int dap_make_frame_id(DapSession* session, int index_from_top) {
    if (!session || index_from_top < 0 || index_from_top >= DAP_STOP_ID_STRIDE - 1) return 0;
    return (session->stop_generation * DAP_STOP_ID_STRIDE) + index_from_top + 1;
}

static int dap_frame_index_from_frame_id(DapSession* session, int frame_id) {
    int generation = 0;
    int slot = 0;

    if (!session || frame_id <= 0) return -1;
    generation = frame_id / DAP_STOP_ID_STRIDE;
    slot = frame_id % DAP_STOP_ID_STRIDE;
    if (slot <= 0) return -1;
    if (generation != session->stop_generation) return -1;
    return slot - 1;
}

static int dap_variables_ref_for_arguments(DapSession* session, int frame_id) {
    if (dap_frame_index_from_frame_id(session, frame_id) < 0) return 0;
    return DAP_VARIABLES_REF_ARGUMENTS_BASE + frame_id;
}

static int dap_variables_ref_for_locals(DapSession* session, int frame_id) {
    if (dap_frame_index_from_frame_id(session, frame_id) < 0) return 0;
    return DAP_VARIABLES_REF_LOCALS_BASE + frame_id;
}

static int dap_variables_ref_for_globals(DapSession* session, int frame_id) {
    if (dap_frame_index_from_frame_id(session, frame_id) < 0) return 0;
    return DAP_VARIABLES_REF_GLOBALS_BASE + frame_id;
}

static int dap_arguments_frame_id_from_ref(DapSession* session, int variables_ref) {
    int frame_id = 0;
    if (variables_ref <= DAP_VARIABLES_REF_ARGUMENTS_BASE || variables_ref >= DAP_VARIABLES_REF_LOCALS_BASE) {
        return -1;
    }
    frame_id = variables_ref - DAP_VARIABLES_REF_ARGUMENTS_BASE;
    return dap_frame_index_from_frame_id(session, frame_id) >= 0 ? frame_id : -1;
}

static int dap_locals_frame_id_from_ref(DapSession* session, int variables_ref) {
    int frame_id = 0;
    if (variables_ref <= DAP_VARIABLES_REF_LOCALS_BASE || variables_ref >= DAP_VARIABLES_REF_GLOBALS_BASE) {
        return -1;
    }
    frame_id = variables_ref - DAP_VARIABLES_REF_LOCALS_BASE;
    return dap_frame_index_from_frame_id(session, frame_id) >= 0 ? frame_id : -1;
}

static int dap_globals_frame_id_from_ref(DapSession* session, int variables_ref) {
    int frame_id = 0;
    if (variables_ref <= DAP_VARIABLES_REF_GLOBALS_BASE || variables_ref >= DAP_VARIABLES_REF_CHILDREN_BASE) {
        return -1;
    }
    frame_id = variables_ref - DAP_VARIABLES_REF_GLOBALS_BASE;
    return dap_frame_index_from_frame_id(session, frame_id) >= 0 ? frame_id : -1;
}

static int dap_value_is_expandable(const Value* value) {
    if (!value) return 0;

    switch (value_get_type(value)) {
        case VAL_ARRAY:
        case VAL_RECORD:
        case VAL_TUPLE:
        case VAL_MAP:
        case VAL_SET:
            return 1;
        default:
            return 0;
    }
}

static int dap_value_named_child_count(const Value* value) {
    ObjRecord* record = NULL;

    if (!value || value_get_type(value) != VAL_RECORD) return 0;
    record = value_get_record_obj(value);
    if (!record) return 0;

    if (value_is_future(value)) {
        return obj_future_is_ready(record) ? 2 : 1;
    }
    return record->field_count;
}

static int dap_value_indexed_child_count(const Value* value) {
    if (!value) return 0;

    switch (value_get_type(value)) {
        case VAL_ARRAY: {
            ObjArray* array = value_get_array_obj(value);
            return array ? array->count : 0;
        }
        case VAL_TUPLE: {
            ObjTuple* tuple = value_get_tuple_obj(value);
            return tuple ? tuple->element_count : 0;
        }
        case VAL_MAP: {
            ObjMap* map = value_get_map_obj(value);
            return map ? map->count : 0;
        }
        case VAL_SET: {
            ObjSet* set = value_get_set_obj(value);
            return set ? set->count : 0;
        }
        default:
            return 0;
    }
}

static RecordDef* dap_lookup_runtime_record_def(DapSession* session, ObjRecord* record) {
    Symbol* sym = NULL;

    if (!record) return NULL;
    if (record->def) return record->def;
    if (!session || !session->runtime || !session->runtime->globals) return NULL;
    if (!record->type_name || record->type_name[0] == '\0') return NULL;

    sym = symbol_table_get(session->runtime->globals, record->type_name);
    if (!sym || !sym->type || sym->type->kind != TYPE_RECORD || !sym->type->record_def) {
        return NULL;
    }
    return sym->type->record_def;
}

static char* dap_format_value_text(DapSession* session, const Value* value) {
    ObjRecord* record = NULL;
    RecordDef* def = NULL;
    char* buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;

    if (!session || !session->runtime || !session->runtime->vm || !value) return safe_strdup("");
    if (value_get_type(value) != VAL_RECORD || value_is_future(value)) {
        return vm_debug_format_value(session->runtime->vm, value);
    }

    record = value_get_record_obj(value);
    def = dap_lookup_runtime_record_def(session, record);
    if (!record || record->field_count <= 0) {
        return vm_debug_format_value(session->runtime->vm, value);
    }

    capacity = 64;
    buffer = (char*)safe_malloc(capacity);
    buffer[0] = '\0';
    buffer[length++] = '{';
    buffer[length] = '\0';

    for (int i = 0; i < record->field_count; i++) {
        const char* field_name = (def && i < def->field_count && def->fields[i].name) ? def->fields[i].name : NULL;
        char fallback_name[32];
        char* field_value = NULL;
        size_t needed = 0;

        if (!field_name) {
            snprintf(fallback_name, sizeof(fallback_name), "field%d", i);
            field_name = fallback_name;
        }
        field_value = vm_debug_format_value(session->runtime->vm, &record->fields[i]);
        if (!field_value) field_value = safe_strdup("");

        needed = length + (i > 0 ? 2 : 0) + (field_name ? strlen(field_name) : 0) + 2 + strlen(field_value) + 2;
        if (needed > capacity) {
            while (capacity < needed) capacity *= 2;
            buffer = (char*)safe_realloc(buffer, capacity);
        }

        if (i > 0) {
            memcpy(buffer + length, ", ", 2);
            length += 2;
        }
        if (field_name) {
            size_t field_name_len = strlen(field_name);
            memcpy(buffer + length, field_name, field_name_len);
            length += field_name_len;
            memcpy(buffer + length, ": ", 2);
            length += 2;
        }
        memcpy(buffer + length, field_value, strlen(field_value));
        length += strlen(field_value);
        buffer[length] = '\0';
        free(field_value);
    }

    if (length + 2 > capacity) {
        capacity = length + 2;
        buffer = (char*)safe_realloc(buffer, capacity);
    }
    buffer[length++] = '}';
    buffer[length] = '\0';
    return buffer;
}

static void dap_flush_captured_output(DapSession* session) {
    if (!session || !session->output_buf || session->output_len <= 0) return;
    dap_write_output_event(session, "stdout", session->output_buf);
    session->output_len = 0;
    session->output_buf[0] = '\0';
}

static void dap_apply_breakpoints(DapSession* session) {
    if (!session || !session->runtime) return;
    vm_debug_clear_line_breakpoints(session->runtime->vm);
    for (int i = 0; i < session->breakpoints.count; i++) {
        (void)vm_debug_add_line_breakpoint(session->runtime->vm,
                                           session->breakpoints.items[i].source_path,
                                           session->breakpoints.items[i].line);
    }
}

static int dap_collect_string_array(cJSON* args_item, char*** out_items, int* out_count) {
    if (!out_items || !out_count) return 0;
    *out_items = NULL;
    *out_count = 0;
    if (!args_item) return 1;
    if (!cJSON_IsArray(args_item)) return 0;

    int count = cJSON_GetArraySize(args_item);
    if (count <= 0) return 1;

    char** items = (char**)safe_calloc((size_t)count, sizeof(char*));
    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(args_item, i);
        if (!item || !cJSON_IsString(item) || !item->valuestring) {
            for (int j = 0; j < i; j++) {
                if (items[j]) free(items[j]);
            }
            free(items);
            return 0;
        }
        items[i] = safe_strdup(item->valuestring);
        if (!items[i]) {
            for (int j = 0; j <= i; j++) {
                if (items[j]) free(items[j]);
            }
            free(items);
            return 0;
        }
    }

    *out_items = items;
    *out_count = count;
    return 1;
}

static int dap_replace_launch_args(DapSession* session, cJSON* args_item) {
    char** argv = NULL;
    int argc = 0;
    if (!dap_collect_string_array(args_item, &argv, &argc)) {
        return 0;
    }

    if (session->argv) {
        for (int i = 0; i < session->argc; i++) {
            if (session->argv[i]) free(session->argv[i]);
        }
        free(session->argv);
    }
    session->argv = argv;
    session->argc = argc;
    return 1;
}

static int dap_set_source_breakpoints(DapSession* session, const char* source_path, cJSON* breakpoints_item, cJSON* lines_item) {
    if (!session || !source_path || source_path[0] == '\0') return 0;

    dap_session_wait_until_not_running(session);

    dap_breakpoint_list_remove_source(&session->breakpoints, source_path);

    if (breakpoints_item && cJSON_IsArray(breakpoints_item)) {
        int count = cJSON_GetArraySize(breakpoints_item);
        for (int i = 0; i < count; i++) {
            cJSON* bp = cJSON_GetArrayItem(breakpoints_item, i);
            cJSON* line_item = bp ? cJSON_GetObjectItemCaseSensitive(bp, "line") : NULL;
            cJSON* condition_item = bp ? cJSON_GetObjectItemCaseSensitive(bp, "condition") : NULL;
            cJSON* hit_condition_item = bp ? cJSON_GetObjectItemCaseSensitive(bp, "hitCondition") : NULL;
            const char* condition_text = NULL;
            const char* hit_condition_text = NULL;
            int hit_condition_kind = DAP_HIT_CONDITION_NONE;
            int hit_condition_value = 0;
            if (!line_item || !cJSON_IsNumber(line_item)) return 0;
            if (condition_item) {
                if (!cJSON_IsString(condition_item) || !condition_item->valuestring) return 0;
                condition_text = condition_item->valuestring;
            }
            if (hit_condition_item) {
                if (!cJSON_IsString(hit_condition_item) || !hit_condition_item->valuestring) return 0;
                hit_condition_text = hit_condition_item->valuestring;
            }
            if (!dap_parse_hit_condition(hit_condition_text, &hit_condition_kind, &hit_condition_value)) {
                return 0;
            }
            if (!dap_breakpoint_list_push(&session->breakpoints,
                                          source_path,
                                          (int)cJSON_GetNumberValue(line_item),
                                          condition_text,
                                          hit_condition_kind,
                                          hit_condition_value)) {
                return 0;
            }
        }
    } else if (lines_item && cJSON_IsArray(lines_item)) {
        int count = cJSON_GetArraySize(lines_item);
        for (int i = 0; i < count; i++) {
            cJSON* line_item = cJSON_GetArrayItem(lines_item, i);
            if (!line_item || !cJSON_IsNumber(line_item)) return 0;
            if (!dap_breakpoint_list_push(&session->breakpoints,
                                          source_path,
                                          (int)cJSON_GetNumberValue(line_item),
                                          NULL,
                                          DAP_HIT_CONDITION_NONE,
                                          0)) {
                return 0;
            }
        }
    }

    dap_apply_breakpoints(session);
    return 1;
}

static void dap_emit_terminated_event(DapSession* session) {
    if (!session) return;
    dap_write_event(session, "terminated", cJSON_CreateObject());
}

static const char* dap_error_summary(const char* error_message) {
    static char summary[256];
    size_t length = 0;

    if (!error_message || error_message[0] == '\0') return NULL;

    while (error_message[length] != '\0' &&
           error_message[length] != '\r' &&
           error_message[length] != '\n' &&
           length + 1 < sizeof(summary)) {
        summary[length] = error_message[length];
        length++;
    }
    summary[length] = '\0';
    return length > 0 ? summary : NULL;
}

static void dap_emit_stopped_event(DapSession* session, const char* reason, const char* text) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "reason", reason ? reason : "pause");
    if (text && text[0] != '\0') {
        cJSON_AddStringToObject(body, "text", text);
    }
    cJSON_AddNumberToObject(body, "threadId", 1);
    cJSON_AddBoolToObject(body, "allThreadsStopped", 1);
    dap_write_event(session, "stopped", body);
}

static void dap_session_broadcast_state(DapSession* session) {
    if (!session) return;
    mtx_lock(&session->state_mutex);
    cnd_broadcast(&session->state_cond);
    mtx_unlock(&session->state_mutex);
}

static int dap_execution_thread_main(void* user_data) {
    DapExecThreadArgs* args = (DapExecThreadArgs*)user_data;
    DapExecutionResult result = {0};
    DapSession* session = NULL;

    if (!args) return 0;
    session = args->session;
    result = dap_resume_execution(session, args->action);
    free(args);

    if (session) {
        mtx_lock(&session->state_mutex);
        session->execution_running = false;
        session->execution_stopped = result.stopped;
        if (result.stopped) {
            session->stop_generation++;
            dap_variable_ref_list_set_stop_generation(&session->variable_refs, session->stop_generation);
        }
        if (result.terminated) {
            session->terminated = true;
        }
        mtx_unlock(&session->state_mutex);

        dap_emit_post_execution(session, &result);
        dap_session_broadcast_state(session);
    }

    return 0;
}

static int dap_start_execution_async(DapSession* session, DapExecAction action, const char** out_error) {
    DapExecThreadArgs* args = NULL;
    int create_result = thrd_error;

    if (out_error) *out_error = NULL;
    if (!session || !session->runtime) {
        if (out_error) *out_error = "No active debug session";
        return 0;
    }

    dap_session_join_worker(session);

    args = (DapExecThreadArgs*)safe_malloc(sizeof(DapExecThreadArgs));
    args->session = session;
    args->action = action;

    mtx_lock(&session->state_mutex);
    if (session->execution_running) {
        mtx_unlock(&session->state_mutex);
        free(args);
        if (out_error) *out_error = "Program is already running";
        return 0;
    }
    session->execution_running = true;
    session->execution_stopped = false;
    if (action == DAP_EXEC_START) {
        session->execution_started = true;
    }
    mtx_unlock(&session->state_mutex);

    create_result = thrd_create(&session->worker_thread, dap_execution_thread_main, args);
    if (create_result != thrd_success) {
        mtx_lock(&session->state_mutex);
        session->execution_running = false;
        if (action == DAP_EXEC_START) {
            session->execution_started = false;
        }
        mtx_unlock(&session->state_mutex);
        free(args);
        if (out_error) *out_error = "Failed to start debugger execution thread";
        return 0;
    }

    mtx_lock(&session->state_mutex);
    session->worker_joinable = true;
    mtx_unlock(&session->state_mutex);
    return 1;
}

static DapExecutionResult dap_resume_execution(DapSession* session, DapExecAction action) {
    DapExecutionResult result = {0};
    int rc = 0;
    const VmDebugStopInfo* stop = NULL;
    DapExecAction next_action = action;

    if (!session || !session->runtime) {
        result.has_error = true;
        result.error_message = "No active debug session";
        return result;
    }

    session->output_len = 0;
    if (session->output_buf) {
        session->output_buf[0] = '\0';
    }
    dap_variable_ref_list_clear(&session->variable_refs);

    for (;;) {
        switch (next_action) {
            case DAP_EXEC_START:
                if (session->stop_on_entry) {
                    session->runtime->debug_stop_on_main_entry_pending = true;
                    session->stop_on_entry = false;
                }
                rc = runtime_run(session->runtime);
                session->execution_started = true;
                break;
            case DAP_EXEC_CONTINUE:
                vm_debug_prepare_continue(session->runtime->vm);
                rc = runtime_resume(session->runtime);
                break;
            case DAP_EXEC_STEP_IN:
                vm_debug_prepare_step_in(session->runtime->vm);
                rc = runtime_resume(session->runtime);
                break;
            case DAP_EXEC_STEP_OVER:
                vm_debug_prepare_step_over(session->runtime->vm);
                rc = runtime_resume(session->runtime);
                break;
            case DAP_EXEC_STEP_OUT:
                vm_debug_prepare_step_out(session->runtime->vm);
                rc = runtime_resume(session->runtime);
                break;
        }

        stop = vm_debug_get_stop_info(session->runtime->vm);
        if (rc == 1 && stop && stop->kind != VM_DEBUG_STOP_NONE) {
            if (stop->kind == VM_DEBUG_STOP_BREAKPOINT) {
                DapLineBreakpoint* bp = dap_find_breakpoint(session, stop->source_file, stop->line);
                if (bp && !dap_breakpoint_should_stop(session, bp)) {
                    next_action = DAP_EXEC_CONTINUE;
                    continue;
                }
            }

            result.stopped = true;
            if (stop->kind == VM_DEBUG_STOP_BREAKPOINT) {
                result.stop_reason = "breakpoint";
            } else if (stop->kind == VM_DEBUG_STOP_ENTRY) {
                result.stop_reason = "entry";
            } else if (stop->kind == VM_DEBUG_STOP_STEP) {
                result.stop_reason = "step";
            } else if (stop->kind == VM_DEBUG_STOP_EXCEPTION) {
                result.stop_reason = "exception";
                result.stop_text = dap_error_summary(runtime_get_error(session->runtime));
            } else {
                result.stop_reason = "pause";
            }
            return result;
        }

        if (runtime_has_error(session->runtime)) {
            result.terminated = true;
            result.has_error = true;
            result.error_message = runtime_get_error(session->runtime);
            return result;
        }

        if (rc == 0) {
            result.terminated = true;
            return result;
        }

        result.terminated = true;
        result.has_error = true;
        result.error_message = "Debug session ended unexpectedly";
        return result;
    }
}

static void dap_emit_post_execution(DapSession* session, const DapExecutionResult* result) {
    if (!session || !result) return;
    dap_flush_captured_output(session);
    if (result->has_error && result->error_message && result->error_message[0] != '\0') {
        dap_write_output_event(session, "stderr", result->error_message);
        dap_write_output_event(session, "stderr", "\n");
    }
    if (result->stopped) {
        dap_emit_stopped_event(session, result->stop_reason, result->stop_text);
    } else if (result->terminated) {
        dap_emit_terminated_event(session);
    }
}

static cJSON* dap_make_source_object(const char* path) {
    cJSON* source = cJSON_CreateObject();
    cJSON_AddStringToObject(source, "name", dap_path_basename(path));
    cJSON_AddStringToObject(source, "path", path ? path : "");
    return source;
}

static int dap_handle_initialize(DapSession* session, int request_seq) {
    cJSON* body = cJSON_CreateObject();
    cJSON* exception_filters = cJSON_CreateArray();
    cJSON* panic_filter = cJSON_CreateObject();

    (void)session;
    cJSON_AddBoolToObject(body, "supportsConfigurationDoneRequest", 1);
    cJSON_AddBoolToObject(body, "supportsFunctionBreakpoints", 0);
    cJSON_AddBoolToObject(body, "supportsConditionalBreakpoints", 1);
    cJSON_AddBoolToObject(body, "supportsHitConditionalBreakpoints", 1);
    cJSON_AddBoolToObject(body, "supportsTerminateRequest", 0);
    cJSON_AddBoolToObject(body, "supportsRestartRequest", 0);
    cJSON_AddBoolToObject(body, "supportsExceptionInfoRequest", 1);
    cJSON_AddBoolToObject(body, "supportsPauseRequest", 1);
    cJSON_AddBoolToObject(body, "supportsSetVariable", 1);
    cJSON_AddStringToObject(panic_filter, "filter", "panic");
    cJSON_AddStringToObject(panic_filter, "label", "Panics");
    cJSON_AddStringToObject(panic_filter, "description", "Stop when a runtime panic or error is raised");
    cJSON_AddItemToArray(exception_filters, panic_filter);
    cJSON_AddItemToObject(body, "exceptionBreakpointFilters", exception_filters);
    return dap_write_response(session, request_seq, "initialize", 1, body, NULL);
}

static int dap_handle_launch(DapSession* session, int request_seq, cJSON* arguments) {
    cJSON* program_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "program") : NULL;
    cJSON* no_debug_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "noDebug") : NULL;
    cJSON* stop_on_entry_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "stopOnEntry") : NULL;
    cJSON* args_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "args") : NULL;
    cJSON* warn_unused_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "warnUnusedError") : NULL;
    cJSON* strict_errors_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "strictErrors") : NULL;

    if (session->runtime || session->launched) {
        return dap_write_response(session, request_seq, "launch", 0, NULL, "Launch already requested");
    }
    if (!program_item || !cJSON_IsString(program_item) || !program_item->valuestring || program_item->valuestring[0] == '\0') {
        return dap_write_response(session, request_seq, "launch", 0, NULL, "Missing launch.arguments.program");
    }
    if (no_debug_item && cJSON_IsBool(no_debug_item) && cJSON_IsTrue(no_debug_item)) {
        return dap_write_response(session, request_seq, "launch", 0, NULL, "launch.noDebug=true is not supported");
    }
    if (!dap_replace_launch_args(session, args_item)) {
        return dap_write_response(session, request_seq, "launch", 0, NULL, "launch.arguments.args must be an array of strings");
    }

    session->options = (RuntimeOptions){0};
    session->options.typecheck.warn_unused_error = warn_unused_item && cJSON_IsBool(warn_unused_item) && cJSON_IsTrue(warn_unused_item);
    session->options.typecheck.strict_errors = strict_errors_item && cJSON_IsBool(strict_errors_item) && cJSON_IsTrue(strict_errors_item);
    if (session->options.typecheck.strict_errors) {
        session->options.typecheck.warn_unused_error = true;
    }

    session->program_path = safe_strdup(program_item->valuestring);
    session->runtime = runtime_create_with_options(program_item->valuestring, session->options);
    if (!session->runtime) {
        if (session->program_path) {
            free(session->program_path);
            session->program_path = NULL;
        }
        return dap_write_response(session, request_seq, "launch", 0, NULL, "Failed to create runtime");
    }
    if (runtime_has_error(session->runtime)) {
        const char* message = runtime_get_error(session->runtime);
        dap_session_free_runtime(session);
        if (session->program_path) {
            free(session->program_path);
            session->program_path = NULL;
        }
        return dap_write_response(session,
                                  request_seq,
                                  "launch",
                                  0,
                                  NULL,
                                  (message && message[0] != '\0') ? message : "Failed to create runtime");
    }

    runtime_set_argv(session->runtime, session->argc, session->argv);
    vm_set_output_callback(session->runtime->vm, dap_output_capture, session);
    vm_debug_set_break_on_runtime_error(session->runtime->vm, session->stop_on_runtime_error);
    dap_apply_breakpoints(session);
    session->launched = true;
    session->terminated = false;
    session->stop_on_entry = stop_on_entry_item && cJSON_IsBool(stop_on_entry_item) && cJSON_IsTrue(stop_on_entry_item);

    dap_write_response(session, request_seq, "launch", 1, NULL, NULL);
    dap_write_event(session, "initialized", cJSON_CreateObject());
    return 1;
}

static int dap_handle_set_breakpoints(DapSession* session, int request_seq, cJSON* arguments) {
    cJSON* source_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "source") : NULL;
    cJSON* path_item = source_item ? cJSON_GetObjectItemCaseSensitive(source_item, "path") : NULL;
    cJSON* breakpoints_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "breakpoints") : NULL;
    cJSON* lines_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "lines") : NULL;
    cJSON* body = NULL;
    cJSON* out_breakpoints = NULL;

    if (!path_item || !cJSON_IsString(path_item) || !path_item->valuestring || path_item->valuestring[0] == '\0') {
        return dap_write_response(session, request_seq, "setBreakpoints", 0, NULL, "Missing arguments.source.path");
    }
    if (!dap_set_source_breakpoints(session, path_item->valuestring, breakpoints_item, lines_item)) {
        return dap_write_response(session, request_seq, "setBreakpoints", 0, NULL, "Invalid breakpoint payload");
    }

    body = cJSON_CreateObject();
    out_breakpoints = cJSON_CreateArray();
    cJSON_AddItemToObject(body, "breakpoints", out_breakpoints);

    if (breakpoints_item && cJSON_IsArray(breakpoints_item)) {
        int count = cJSON_GetArraySize(breakpoints_item);
        for (int i = 0; i < count; i++) {
            cJSON* bp = cJSON_GetArrayItem(breakpoints_item, i);
            cJSON* line_item = bp ? cJSON_GetObjectItemCaseSensitive(bp, "line") : NULL;
            cJSON* out_bp = cJSON_CreateObject();
            cJSON_AddBoolToObject(out_bp, "verified", 1);
            cJSON_AddNumberToObject(out_bp, "line", line_item ? cJSON_GetNumberValue(line_item) : 0);
            cJSON_AddItemToArray(out_breakpoints, out_bp);
        }
    } else if (lines_item && cJSON_IsArray(lines_item)) {
        int count = cJSON_GetArraySize(lines_item);
        for (int i = 0; i < count; i++) {
            cJSON* line_item = cJSON_GetArrayItem(lines_item, i);
            cJSON* out_bp = cJSON_CreateObject();
            cJSON_AddBoolToObject(out_bp, "verified", 1);
            cJSON_AddNumberToObject(out_bp, "line", line_item ? cJSON_GetNumberValue(line_item) : 0);
            cJSON_AddItemToArray(out_breakpoints, out_bp);
        }
    }

    return dap_write_response(session, request_seq, "setBreakpoints", 1, body, NULL);
}

static int dap_handle_set_exception_breakpoints(DapSession* session, int request_seq, cJSON* arguments) {
    cJSON* filters_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "filters") : NULL;
    bool stop_on_runtime_error = false;

    if (filters_item && !cJSON_IsArray(filters_item)) {
        return dap_write_response(session,
                                  request_seq,
                                  "setExceptionBreakpoints",
                                  0,
                                  NULL,
                                  "arguments.filters must be an array of strings");
    }

    if (filters_item) {
        int count = cJSON_GetArraySize(filters_item);
        for (int i = 0; i < count; i++) {
            cJSON* filter_item = cJSON_GetArrayItem(filters_item, i);
            if (!filter_item || !cJSON_IsString(filter_item) || !filter_item->valuestring) {
                return dap_write_response(session,
                                          request_seq,
                                          "setExceptionBreakpoints",
                                          0,
                                          NULL,
                                          "arguments.filters must be an array of strings");
            }
            if (strcmp(filter_item->valuestring, "panic") == 0) {
                stop_on_runtime_error = true;
            }
        }
    }

    session->stop_on_runtime_error = stop_on_runtime_error;
    if (session->runtime && session->runtime->vm) {
        vm_debug_set_break_on_runtime_error(session->runtime->vm, session->stop_on_runtime_error);
    }
    return dap_write_response(session, request_seq, "setExceptionBreakpoints", 1, NULL, NULL);
}

static int dap_handle_configuration_done(DapSession* session, int request_seq) {
    const char* start_error = NULL;

    if (!session->runtime || !session->launched) {
        return dap_write_response(session, request_seq, "configurationDone", 0, NULL, "Launch has not completed");
    }
    if (session->execution_started) {
        return dap_write_response(session, request_seq, "configurationDone", 1, NULL, NULL);
    }

    if (!dap_start_execution_async(session, DAP_EXEC_START, &start_error)) {
        return dap_write_response(session,
                                  request_seq,
                                  "configurationDone",
                                  0,
                                  NULL,
                                  start_error ? start_error : "Failed to start program");
    }
    return dap_write_response(session, request_seq, "configurationDone", 1, NULL, NULL);
}

static int dap_handle_pause(DapSession* session, int request_seq, cJSON* arguments) {
    cJSON* thread_id_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "threadId") : NULL;
    bool running = false;
    bool stopped = false;
    bool terminated = false;

    if (!session->runtime || !session->execution_started) {
        return dap_write_response(session, request_seq, "pause", 0, NULL, "Program is not running");
    }
    if (!thread_id_item || !cJSON_IsNumber(thread_id_item) || (int)cJSON_GetNumberValue(thread_id_item) != 1) {
        return dap_write_response(session, request_seq, "pause", 0, NULL, "Invalid threadId");
    }

    dap_session_snapshot(session, &running, &stopped, &terminated);
    if (!running || stopped || terminated) {
        return dap_write_response(session, request_seq, "pause", 0, NULL, "Program is not currently running");
    }

    vm_debug_request_pause(session->runtime->vm);
    return dap_write_response(session, request_seq, "pause", 1, NULL, NULL);
}

static int dap_handle_threads(DapSession* session, int request_seq) {
    cJSON* body = cJSON_CreateObject();
    cJSON* threads = cJSON_CreateArray();
    cJSON* thread = cJSON_CreateObject();
    (void)session;

    cJSON_AddNumberToObject(thread, "id", 1);
    cJSON_AddStringToObject(thread, "name", "main");
    cJSON_AddItemToArray(threads, thread);
    cJSON_AddItemToObject(body, "threads", threads);
    return dap_write_response(session, request_seq, "threads", 1, body, NULL);
}

static int dap_handle_stack_trace(DapSession* session, int request_seq, cJSON* arguments) {
    cJSON* body = cJSON_CreateObject();
    cJSON* frames = cJSON_CreateArray();
    int start_frame = 0;
    int levels = -1;
    bool running = false;
    bool stopped = false;
    bool terminated = false;

    dap_session_wait_until_not_running(session);

    if (arguments) {
        cJSON* start_item = cJSON_GetObjectItemCaseSensitive(arguments, "startFrame");
        cJSON* levels_item = cJSON_GetObjectItemCaseSensitive(arguments, "levels");
        if (start_item && cJSON_IsNumber(start_item)) start_frame = (int)cJSON_GetNumberValue(start_item);
        if (levels_item && cJSON_IsNumber(levels_item)) levels = (int)cJSON_GetNumberValue(levels_item);
    }

    if (!session || !session->runtime || !session->execution_started) {
        cJSON_Delete(body);
        return dap_write_response(session, request_seq, "stackTrace", 0, NULL, "Program is not running");
    }

    dap_session_snapshot(session, &running, &stopped, &terminated);
    if (!stopped) {
        cJSON_Delete(body);
        if (terminated) {
            return dap_write_response(session, request_seq, "stackTrace", 0, NULL, "Program has terminated");
        }
        return dap_write_response(session, request_seq, "stackTrace", 0, NULL, "No active stop to inspect");
    }

    if (session && session->runtime) {
        int frame_count = vm_debug_frame_count(session->runtime->vm);
        int end_frame = frame_count;
        if (levels >= 0 && start_frame + levels < end_frame) {
            end_frame = start_frame + levels;
        }
        for (int i = start_frame; i < end_frame; i++) {
            VmDebugFrameInfo info;
            if (!vm_debug_get_frame_info(session->runtime->vm, i, &info)) continue;
            cJSON* frame = cJSON_CreateObject();
            cJSON_AddNumberToObject(frame, "id", dap_make_frame_id(session, i));
            cJSON_AddStringToObject(frame, "name", info.function_name ? info.function_name : "<anon>");
            if (info.source_file && info.source_file[0] != '\0') {
                cJSON_AddItemToObject(frame, "source", dap_make_source_object(info.source_file));
            }
            cJSON_AddNumberToObject(frame, "line", info.line > 0 ? info.line : 1);
            cJSON_AddNumberToObject(frame, "column", 1);
            cJSON_AddItemToArray(frames, frame);
        }
        cJSON_AddNumberToObject(body, "totalFrames", frame_count);
    } else {
        cJSON_AddNumberToObject(body, "totalFrames", 0);
    }
    cJSON_AddItemToObject(body, "stackFrames", frames);
    return dap_write_response(session, request_seq, "stackTrace", 1, body, NULL);
}

static int dap_handle_exception_info(DapSession* session, int request_seq, cJSON* arguments) {
    const VmDebugStopInfo* stop = NULL;
    const char* error_message = NULL;
    const char* summary = NULL;
    cJSON* thread_id_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "threadId") : NULL;
    cJSON* body = NULL;
    cJSON* details = NULL;
    bool running = false;
    bool stopped = false;
    bool terminated = false;

    dap_session_wait_until_not_running(session);

    if (!session->runtime || !session->execution_started) {
        return dap_write_response(session, request_seq, "exceptionInfo", 0, NULL, "Program is not running");
    }
    dap_session_snapshot(session, &running, &stopped, &terminated);
    if (!stopped) {
        if (terminated) {
            return dap_write_response(session, request_seq, "exceptionInfo", 0, NULL, "Program has terminated");
        }
        return dap_write_response(session, request_seq, "exceptionInfo", 0, NULL, "No active exception stop");
    }
    if (!thread_id_item || !cJSON_IsNumber(thread_id_item) || (int)cJSON_GetNumberValue(thread_id_item) != 1) {
        return dap_write_response(session, request_seq, "exceptionInfo", 0, NULL, "Invalid threadId");
    }

    stop = vm_debug_get_stop_info(session->runtime->vm);
    if (!stop || stop->kind != VM_DEBUG_STOP_EXCEPTION) {
        return dap_write_response(session, request_seq, "exceptionInfo", 0, NULL, "No active exception stop");
    }

    error_message = runtime_get_error(session->runtime);
    summary = dap_error_summary(error_message);

    body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "exceptionId", "panic");
    cJSON_AddStringToObject(body, "breakMode", "always");
    cJSON_AddStringToObject(body, "description", summary ? summary : "Runtime panic");

    details = cJSON_CreateObject();
    cJSON_AddStringToObject(details, "message", summary ? summary : "Runtime panic");
    if (error_message && error_message[0] != '\0') {
        cJSON_AddStringToObject(details, "stackTrace", error_message);
    }
    cJSON_AddItemToObject(body, "details", details);
    return dap_write_response(session, request_seq, "exceptionInfo", 1, body, NULL);
}

static int dap_handle_scopes(DapSession* session, int request_seq, cJSON* arguments) {
    cJSON* frame_id_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "frameId") : NULL;
    cJSON* body = NULL;
    cJSON* scopes = NULL;
    cJSON* arguments_scope = NULL;
    cJSON* locals_scope = NULL;
    cJSON* globals_scope = NULL;
    int frame_id = 0;
    int index_from_top = 0;
    bool running = false;
    bool stopped = false;
    bool terminated = false;

    dap_session_wait_until_not_running(session);

    if (!session->runtime || !session->execution_started) {
        return dap_write_response(session, request_seq, "scopes", 0, NULL, "Program is not running");
    }
    dap_session_snapshot(session, &running, &stopped, &terminated);
    if (!stopped) {
        if (terminated) {
            return dap_write_response(session, request_seq, "scopes", 0, NULL, "Program has terminated");
        }
        return dap_write_response(session, request_seq, "scopes", 0, NULL, "No active stop to inspect");
    }
    if (!frame_id_item || !cJSON_IsNumber(frame_id_item)) {
        return dap_write_response(session, request_seq, "scopes", 0, NULL, "Missing arguments.frameId");
    }

    frame_id = (int)cJSON_GetNumberValue(frame_id_item);
    index_from_top = dap_frame_index_from_frame_id(session, frame_id);
    if (index_from_top < 0 || index_from_top >= vm_debug_frame_count(session->runtime->vm)) {
        return dap_write_response(session, request_seq, "scopes", 0, NULL, "Invalid frameId");
    }

    body = cJSON_CreateObject();
    scopes = cJSON_CreateArray();
    cJSON_AddItemToObject(body, "scopes", scopes);

    arguments_scope = cJSON_CreateObject();
    cJSON_AddStringToObject(arguments_scope, "name", "Arguments");
    cJSON_AddNumberToObject(arguments_scope, "variablesReference", dap_variables_ref_for_arguments(session, frame_id));
    cJSON_AddBoolToObject(arguments_scope, "expensive", 0);
    cJSON_AddStringToObject(arguments_scope, "presentationHint", "arguments");
    cJSON_AddItemToArray(scopes, arguments_scope);

    locals_scope = cJSON_CreateObject();
    cJSON_AddStringToObject(locals_scope, "name", "Locals");
    cJSON_AddNumberToObject(locals_scope, "variablesReference", dap_variables_ref_for_locals(session, frame_id));
    cJSON_AddBoolToObject(locals_scope, "expensive", 0);
    cJSON_AddStringToObject(locals_scope, "presentationHint", "locals");
    cJSON_AddItemToArray(scopes, locals_scope);

    globals_scope = cJSON_CreateObject();
    cJSON_AddStringToObject(globals_scope, "name", "Globals");
    cJSON_AddNumberToObject(globals_scope, "variablesReference", dap_variables_ref_for_globals(session, frame_id));
    cJSON_AddBoolToObject(globals_scope, "expensive", 0);
    cJSON_AddItemToArray(scopes, globals_scope);

    return dap_write_response(session, request_seq, "scopes", 1, body, NULL);
}

static void dap_add_variable_json(cJSON* variables,
                                  const char* name,
                                  const Value* value,
                                  DapSession* session) {
    cJSON* variable = NULL;
    char* value_text = NULL;
    const char* type_text = NULL;
    int child_ref = 0;
    int named_count = 0;
    int indexed_count = 0;

    if (!variables || !name || !value || !session || !session->runtime || !session->runtime->vm) return;

    value_text = dap_format_value_text(session, value);
    type_text = vm_debug_value_type_name(value);
    if (dap_value_is_expandable(value)) {
        child_ref = dap_variable_ref_list_add(&session->variable_refs, value);
        named_count = dap_value_named_child_count(value);
        indexed_count = dap_value_indexed_child_count(value);
    }

    variable = cJSON_CreateObject();
    cJSON_AddStringToObject(variable, "name", name);
    cJSON_AddStringToObject(variable, "value", value_text ? value_text : "");
    cJSON_AddStringToObject(variable, "type", type_text ? type_text : "unknown");
    cJSON_AddNumberToObject(variable, "variablesReference", child_ref);
    if (named_count > 0) {
        cJSON_AddNumberToObject(variable, "namedVariables", named_count);
    }
    if (indexed_count > 0) {
        cJSON_AddNumberToObject(variable, "indexedVariables", indexed_count);
    }
    cJSON_AddItemToArray(variables, variable);

    if (value_text) free(value_text);
}

static void dap_add_text_variable_json(cJSON* variables,
                                       const char* name,
                                       const char* value_text,
                                       const char* type_text) {
    cJSON* variable = NULL;

    if (!variables || !name) return;
    variable = cJSON_CreateObject();
    cJSON_AddStringToObject(variable, "name", name);
    cJSON_AddStringToObject(variable, "value", value_text ? value_text : "");
    cJSON_AddStringToObject(variable, "type", type_text ? type_text : "unknown");
    cJSON_AddNumberToObject(variable, "variablesReference", 0);
    cJSON_AddItemToArray(variables, variable);
}

static void dap_append_array_children(cJSON* variables, DapSession* session, ObjArray* array) {
    char name_buf[32];
    Value element;

    if (!variables || !session || !array) return;
    for (int i = 0; i < array->count; i++) {
        snprintf(name_buf, sizeof(name_buf), "[%d]", i);
        obj_array_get(array, i, &element);
        dap_add_variable_json(variables, name_buf, &element, session);
        value_free(&element);
    }
}

static void dap_append_record_children(cJSON* variables, DapSession* session, const Value* parent) {
    ObjRecord* record = NULL;
    RecordDef* def = NULL;

    if (!variables || !session || !parent) return;
    record = value_get_record_obj(parent);
    if (!record) return;
    def = dap_lookup_runtime_record_def(session, record);

    if (value_is_future(parent)) {
        Value result_value;
        const char* panic_message = NULL;
        if (obj_future_is_panicked(record)) {
            dap_add_text_variable_json(variables, "state", "panicked", "string");
            panic_message = obj_future_get_panic_message(record);
            dap_add_text_variable_json(variables, "panic", panic_message ? panic_message : "", "string");
            return;
        }
        if (obj_future_is_ready(record)) {
            dap_add_text_variable_json(variables, "state", "resolved", "string");
            if (obj_future_try_get(record, &result_value)) {
                dap_add_variable_json(variables, "value", &result_value, session);
                value_free(&result_value);
            } else {
                dap_add_text_variable_json(variables, "value", "nil", "nil");
            }
            return;
        }

        dap_add_text_variable_json(variables, "state", "pending", "string");
        return;
    }

    for (int i = 0; i < record->field_count; i++) {
        const char* field_name = NULL;
        char fallback_name[32];

        if (def && i < def->field_count && def->fields[i].name) {
            field_name = def->fields[i].name;
        } else {
            snprintf(fallback_name, sizeof(fallback_name), "field%d", i);
            field_name = fallback_name;
        }
        dap_add_variable_json(variables, field_name, &record->fields[i], session);
    }
}

static void dap_append_tuple_children(cJSON* variables, DapSession* session, ObjTuple* tuple) {
    char name_buf[32];

    if (!variables || !session || !tuple) return;
    for (int i = 0; i < tuple->element_count; i++) {
        snprintf(name_buf, sizeof(name_buf), "[%d]", i);
        dap_add_variable_json(variables, name_buf, &tuple->elements[i], session);
    }
}

static void dap_append_map_children(cJSON* variables, DapSession* session, ObjMap* map) {
    VM* vm = NULL;

    if (!variables || !session || !session->runtime || !session->runtime->vm || !map) return;
    vm = session->runtime->vm;

    for (int i = 0; i < map->capacity; i++) {
        MapSlot* slot = &map->slots[i];
        char* key_text = NULL;
        char* name_text = NULL;
        size_t name_len = 0;
        if (slot->hash < 2) continue;

        key_text = vm_debug_format_value(vm, &slot->key);
        if (!key_text) continue;
        name_len = strlen(key_text) + 3;
        name_text = (char*)safe_malloc(name_len);
        snprintf(name_text, name_len, "[%s]", key_text);
        dap_add_variable_json(variables, name_text, &slot->value, session);
        free(name_text);
        free(key_text);
    }
}

static void dap_append_set_children(cJSON* variables, DapSession* session, ObjSet* set) {
    char name_buf[32];
    int index = 0;

    if (!variables || !session || !set) return;
    for (int i = 0; i < set->capacity; i++) {
        SetSlot* slot = &set->slots[i];
        if (slot->hash < 2) continue;
        snprintf(name_buf, sizeof(name_buf), "[%d]", index++);
        dap_add_variable_json(variables, name_buf, &slot->value, session);
    }
}

static void dap_append_value_children(cJSON* variables, DapSession* session, const Value* parent) {
    if (!variables || !session || !parent) return;

    switch (value_get_type(parent)) {
        case VAL_ARRAY:
            dap_append_array_children(variables, session, value_get_array_obj(parent));
            break;
        case VAL_RECORD:
            dap_append_record_children(variables, session, parent);
            break;
        case VAL_TUPLE:
            dap_append_tuple_children(variables, session, value_get_tuple_obj(parent));
            break;
        case VAL_MAP:
            dap_append_map_children(variables, session, value_get_map_obj(parent));
            break;
        case VAL_SET:
            dap_append_set_children(variables, session, value_get_set_obj(parent));
            break;
        default:
            break;
    }
}

static void dap_eval_skip_ws(const char** cursor) {
    if (!cursor || !*cursor) return;
    while (**cursor && isspace((unsigned char)**cursor)) {
        (*cursor)++;
    }
}

static int dap_eval_parse_identifier(const char** cursor, char* buffer, size_t buffer_size) {
    size_t length = 0;

    if (!cursor || !*cursor || !buffer || buffer_size == 0) return 0;
    dap_eval_skip_ws(cursor);
    if (!(isalpha((unsigned char)**cursor) || **cursor == '_')) {
        return 0;
    }

    while (**cursor && (isalnum((unsigned char)**cursor) || **cursor == '_')) {
        if (length + 1 >= buffer_size) return 0;
        buffer[length++] = **cursor;
        (*cursor)++;
    }
    buffer[length] = '\0';
    return 1;
}

static int dap_eval_parse_nonnegative_int(const char** cursor, int* out_value) {
    int value = 0;
    int saw_digit = 0;

    if (!cursor || !*cursor || !out_value) return 0;
    dap_eval_skip_ws(cursor);
    while (**cursor && isdigit((unsigned char)**cursor)) {
        saw_digit = 1;
        value = value * 10 + (**cursor - '0');
        (*cursor)++;
    }
    if (!saw_digit) return 0;
    *out_value = value;
    return 1;
}

static int dap_eval_parse_string_literal(const char** cursor, char* buffer, size_t buffer_size) {
    char quote = '\0';
    size_t length = 0;

    if (!cursor || !*cursor || !buffer || buffer_size == 0) return 0;
    dap_eval_skip_ws(cursor);
    quote = **cursor;
    if (quote != '"' && quote != '\'') {
        return 0;
    }
    (*cursor)++;

    while (**cursor && **cursor != quote) {
        if (**cursor == '\\') {
            (*cursor)++;
            if (!**cursor) return 0;
        }
        if (length + 1 >= buffer_size) return 0;
        buffer[length++] = **cursor;
        (*cursor)++;
    }
    if (**cursor != quote) return 0;
    (*cursor)++;
    buffer[length] = '\0';
    return 1;
}

static char* dap_trim_copy(const char* text) {
    const char* start = text;
    const char* end = NULL;
    size_t length = 0;
    char* trimmed = NULL;

    if (!text) return NULL;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }

    length = (size_t)(end - start);
    trimmed = (char*)safe_malloc(length + 1);
    memcpy(trimmed, start, length);
    trimmed[length] = '\0';
    return trimmed;
}

static int dap_parse_assignment_literal(const char* expression, Value* out_value, const char** out_error) {
    char* trimmed = NULL;
    const char* cursor = NULL;
    char string_buffer[1024];
    char* endptr = NULL;

    if (out_error) *out_error = NULL;
    if (out_value) value_init_nil(out_value);
    if (!expression || !out_value) {
        if (out_error) *out_error = "Missing assignment value";
        return -1;
    }

    trimmed = dap_trim_copy(expression);
    if (!trimmed || trimmed[0] == '\0') {
        if (trimmed) free(trimmed);
        if (out_error) *out_error = "Missing assignment value";
        return -1;
    }

    if (strcmp(trimmed, "nil") == 0) {
        free(trimmed);
        value_init_nil(out_value);
        return 1;
    }
    if (strcmp(trimmed, "true") == 0) {
        free(trimmed);
        value_init_bool(out_value, true);
        return 1;
    }
    if (strcmp(trimmed, "false") == 0) {
        free(trimmed);
        value_init_bool(out_value, false);
        return 1;
    }

    if (trimmed[0] == '"' || trimmed[0] == '\'') {
        cursor = trimmed;
        if (!dap_eval_parse_string_literal(&cursor, string_buffer, sizeof(string_buffer))) {
            free(trimmed);
            if (out_error) *out_error = "Invalid string literal for setVariable";
            return -1;
        }
        dap_eval_skip_ws(&cursor);
        if (*cursor != '\0') {
            free(trimmed);
            if (out_error) *out_error = "Unexpected trailing tokens after string literal";
            return -1;
        }
        free(trimmed);
        value_init_string(out_value, string_buffer);
        return 1;
    }

    endptr = NULL;
    (void)strtoll(trimmed, &endptr, 10);
    if (endptr && endptr != trimmed && *endptr == '\0' &&
        strchr(trimmed, '.') == NULL &&
        strchr(trimmed, 'e') == NULL &&
        strchr(trimmed, 'E') == NULL) {
        int64_t int_value = (int64_t)strtoll(trimmed, NULL, 10);
        free(trimmed);
        value_init_int(out_value, int_value);
        return 1;
    }

    endptr = NULL;
    (void)strtod(trimmed, &endptr);
    if (endptr && endptr != trimmed && *endptr == '\0') {
        double double_value = strtod(trimmed, NULL);
        free(trimmed);
        value_init_double(out_value, double_value);
        return 1;
    }

    free(trimmed);
    return 0;
}

static int dap_parse_assignment_value(DapSession* session,
                                      int frame_id,
                                      const char* expression,
                                      Value* out_value,
                                      const char** out_error) {
    int literal_result = 0;

    if (out_error) *out_error = NULL;
    if (out_value) value_init_nil(out_value);
    literal_result = dap_parse_assignment_literal(expression, out_value, out_error);
    if (literal_result != 0) {
        return literal_result > 0;
    }
    return dap_eval_expression(session, frame_id, expression, out_value, out_error);
}

static int dap_eval_lookup_root_value(DapSession* session, int frame_id, const char* name, Value* out_value) {
    int index_from_top = 0;
    int local_count = 0;
    int global_count = 0;

    if (out_value) value_init_nil(out_value);
    if (!session || !session->runtime || !session->runtime->vm || !name || name[0] == '\0' || !out_value) {
        return 0;
    }

    index_from_top = frame_id - 1;
    local_count = vm_debug_frame_local_count(session->runtime->vm, index_from_top);
    for (int i = 0; i < local_count; i++) {
        const char* local_name = NULL;
        const Value* local_value = NULL;
        if (!vm_debug_get_frame_local(session->runtime->vm, index_from_top, i, &local_name, &local_value)) continue;
        if (!local_name || strcmp(local_name, name) != 0) continue;
        *out_value = *local_value;
        value_retain(out_value);
        return 1;
    }

    global_count = vm_debug_global_count(session->runtime->vm);
    for (int i = 0; i < global_count; i++) {
        const char* global_name = NULL;
        const Value* global_value = NULL;
        if (!vm_debug_get_global(session->runtime->vm, i, &global_name, &global_value)) continue;
        if (!global_name || strcmp(global_name, name) != 0) continue;
        *out_value = *global_value;
        value_retain(out_value);
        return 1;
    }

    return 0;
}

static int dap_eval_apply_record_field(DapSession* session,
                                       ObjRecord* record,
                                       const char* field_name,
                                       Value* out_value) {
    RecordDef* def = NULL;

    if (out_value) value_init_nil(out_value);
    if (!session || !record || !field_name || field_name[0] == '\0' || !out_value) return 0;

    if (record->type_name && strcmp(record->type_name, VM_FUTURE_RUNTIME_TYPE_NAME) == 0) {
        if (strcmp(field_name, "state") == 0) {
            if (obj_future_is_panicked(record)) {
                value_init_string(out_value, "panicked");
            } else if (obj_future_is_ready(record)) {
                value_init_string(out_value, "resolved");
            } else {
                value_init_string(out_value, "pending");
            }
            return 1;
        }
        if (strcmp(field_name, "value") == 0) {
            if (!obj_future_is_ready(record) || obj_future_is_panicked(record)) return 0;
            return obj_future_try_get(record, out_value) ? 1 : 0;
        }
        if (strcmp(field_name, "panic") == 0) {
            const char* panic_message = NULL;
            if (!obj_future_is_panicked(record)) return 0;
            panic_message = obj_future_get_panic_message(record);
            value_init_string(out_value, panic_message ? panic_message : "");
            return 1;
        }
        return 0;
    }

    def = dap_lookup_runtime_record_def(session, record);
    if (def) {
        int field_index = record_def_get_field_index(def, field_name);
        if (field_index >= 0 && field_index < record->field_count) {
            *out_value = record->fields[field_index];
            value_retain(out_value);
            return 1;
        }
    }

    if (strncmp(field_name, "field", 5) == 0) {
        const char* suffix = field_name + 5;
        int field_index = 0;
        if (*suffix != '\0') {
            while (*suffix) {
                if (!isdigit((unsigned char)*suffix)) return 0;
                field_index = field_index * 10 + (*suffix - '0');
                suffix++;
            }
            if (field_index >= 0 && field_index < record->field_count) {
                *out_value = record->fields[field_index];
                value_retain(out_value);
                return 1;
            }
        }
    }

    return 0;
}

static int dap_eval_apply_field(DapSession* session,
                                const Value* current,
                                const char* field_name,
                                Value* out_value) {
    ObjMap* map = NULL;

    if (out_value) value_init_nil(out_value);
    if (!session || !current || !field_name || field_name[0] == '\0' || !out_value) return 0;

    if (value_get_type(current) == VAL_RECORD) {
        return dap_eval_apply_record_field(session, value_get_record_obj(current), field_name, out_value);
    }

    if (value_get_type(current) == VAL_MAP) {
        map = value_get_map_obj(current);
        return obj_map_try_get_cstr(map, field_name, out_value) ? 1 : 0;
    }

    return 0;
}

static int dap_eval_apply_int_index(const Value* current, int index, Value* out_value) {
    Value key;

    if (out_value) value_init_nil(out_value);
    if (!current || index < 0 || !out_value) return 0;

    switch (value_get_type(current)) {
        case VAL_ARRAY: {
            ObjArray* array = value_get_array_obj(current);
            if (!array || index >= array->count) return 0;
            obj_array_get(array, index, out_value);
            return 1;
        }
        case VAL_TUPLE: {
            ObjTuple* tuple = value_get_tuple_obj(current);
            if (!tuple || index >= tuple->element_count) return 0;
            *out_value = tuple->elements[index];
            value_retain(out_value);
            return 1;
        }
        case VAL_MAP: {
            ObjMap* map = value_get_map_obj(current);
            value_init_int(&key, index);
            if (!obj_map_has(map, key)) return 0;
            *out_value = obj_map_get(map, key);
            return 1;
        }
        default:
            return 0;
    }
}

static int dap_eval_apply_string_index(const Value* current, const char* key_text, Value* out_value) {
    ObjMap* map = NULL;

    if (out_value) value_init_nil(out_value);
    if (!current || !key_text || !out_value) return 0;
    if (value_get_type(current) != VAL_MAP) return 0;

    map = value_get_map_obj(current);
    return obj_map_try_get_cstr(map, key_text, out_value) ? 1 : 0;
}

static int dap_eval_expression(DapSession* session,
                               int frame_id,
                               const char* expression,
                               Value* out_value,
                               const char** out_error) {
    const char* cursor = expression;
    Value current;
    char token[256];

    if (out_error) *out_error = NULL;
    if (out_value) value_init_nil(out_value);
    if (!session || !expression || !out_value) {
        if (out_error) *out_error = "Missing expression";
        return 0;
    }

    if (!dap_eval_parse_identifier(&cursor, token, sizeof(token))) {
        if (out_error) *out_error = "Evaluate supports identifier roots with optional .field and [index] selectors";
        return 0;
    }
    if (!dap_eval_lookup_root_value(session, frame_id, token, &current)) {
        if (out_error) *out_error = "Unknown identifier in current debug scope";
        return 0;
    }

    for (;;) {
        dap_eval_skip_ws(&cursor);
        if (*cursor == '\0') {
            *out_value = current;
            return 1;
        }

        if (*cursor == '.') {
            Value next;
            cursor++;
            if (!dap_eval_parse_identifier(&cursor, token, sizeof(token))) {
                value_free(&current);
                if (out_error) *out_error = "Expected field name after '.'";
                return 0;
            }
            if (!dap_eval_apply_field(session, &current, token, &next)) {
                value_free(&current);
                if (out_error) *out_error = "Field lookup failed during evaluate";
                return 0;
            }
            value_free(&current);
            current = next;
            continue;
        }

        if (*cursor == '[') {
            Value next;
            int index_value = 0;
            char key_text[256];

            cursor++;
            dap_eval_skip_ws(&cursor);
            if (dap_eval_parse_nonnegative_int(&cursor, &index_value)) {
                dap_eval_skip_ws(&cursor);
                if (*cursor != ']') {
                    value_free(&current);
                    if (out_error) *out_error = "Expected ']' after numeric index";
                    return 0;
                }
                cursor++;
                if (!dap_eval_apply_int_index(&current, index_value, &next)) {
                    value_free(&current);
                    if (out_error) *out_error = "Index lookup failed during evaluate";
                    return 0;
                }
                value_free(&current);
                current = next;
                continue;
            }

            if (!dap_eval_parse_string_literal(&cursor, key_text, sizeof(key_text))) {
                value_free(&current);
                if (out_error) *out_error = "Expected integer or quoted string inside '[]'";
                return 0;
            }
            dap_eval_skip_ws(&cursor);
            if (*cursor != ']') {
                value_free(&current);
                if (out_error) *out_error = "Expected ']' after string index";
                return 0;
            }
            cursor++;
            if (!dap_eval_apply_string_index(&current, key_text, &next)) {
                value_free(&current);
                if (out_error) *out_error = "String-key lookup failed during evaluate";
                return 0;
            }
            value_free(&current);
            current = next;
            continue;
        }

        value_free(&current);
        if (out_error) *out_error = "Unexpected trailing tokens in evaluate expression";
        return 0;
    }
}

static int dap_handle_evaluate(DapSession* session, int request_seq, cJSON* arguments) {
    cJSON* expression_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "expression") : NULL;
    cJSON* frame_id_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "frameId") : NULL;
    const VmDebugStopInfo* stop = NULL;
    const char* eval_error = NULL;
    Value result_value;
    cJSON* body = NULL;
    char* result_text = NULL;
    const char* type_text = NULL;
    int variables_ref = 0;
    int frame_id = 0;
    bool running = false;
    bool stopped = false;
    bool terminated = false;

    dap_session_wait_until_not_running(session);

    if (!session->runtime || !session->execution_started) {
        return dap_write_response(session, request_seq, "evaluate", 0, NULL, "Program is not running");
    }
    dap_session_snapshot(session, &running, &stopped, &terminated);
    if (!stopped || terminated) {
        if (terminated) {
            return dap_write_response(session, request_seq, "evaluate", 0, NULL, "Program has terminated");
        }
        return dap_write_response(session, request_seq, "evaluate", 0, NULL, "No active stop to evaluate against");
    }
    stop = vm_debug_get_stop_info(session->runtime->vm);
    if (!stop || stop->kind == VM_DEBUG_STOP_NONE) {
        return dap_write_response(session, request_seq, "evaluate", 0, NULL, "No active stop to evaluate against");
    }
    if (!expression_item || !cJSON_IsString(expression_item) || !expression_item->valuestring) {
        return dap_write_response(session, request_seq, "evaluate", 0, NULL, "Missing arguments.expression");
    }
    if (frame_id_item && cJSON_IsNumber(frame_id_item)) {
        frame_id = (int)cJSON_GetNumberValue(frame_id_item);
    } else {
        frame_id = dap_make_frame_id(session, 0);
    }
    frame_id = dap_frame_index_from_frame_id(session, frame_id) + 1;
    if (frame_id <= 0 || frame_id - 1 >= vm_debug_frame_count(session->runtime->vm)) {
        return dap_write_response(session, request_seq, "evaluate", 0, NULL, "Invalid frameId");
    }

    if (!dap_eval_expression(session, frame_id, expression_item->valuestring, &result_value, &eval_error)) {
        return dap_write_response(session,
                                  request_seq,
                                  "evaluate",
                                  0,
                                  NULL,
                                  (eval_error && eval_error[0] != '\0') ? eval_error : "Evaluate failed");
    }

    body = cJSON_CreateObject();
    result_text = dap_format_value_text(session, &result_value);
    type_text = vm_debug_value_type_name(&result_value);
    if (dap_value_is_expandable(&result_value)) {
        variables_ref = dap_variable_ref_list_add(&session->variable_refs, &result_value);
    }

    cJSON_AddStringToObject(body, "result", result_text ? result_text : "");
    cJSON_AddStringToObject(body, "type", type_text ? type_text : "unknown");
    cJSON_AddNumberToObject(body, "variablesReference", variables_ref);
    if (variables_ref > 0) {
        int named_count = dap_value_named_child_count(&result_value);
        int indexed_count = dap_value_indexed_child_count(&result_value);
        if (named_count > 0) {
            cJSON_AddNumberToObject(body, "namedVariables", named_count);
        }
        if (indexed_count > 0) {
            cJSON_AddNumberToObject(body, "indexedVariables", indexed_count);
        }
    }

    if (result_text) free(result_text);
    value_free(&result_value);
    return dap_write_response(session, request_seq, "evaluate", 1, body, NULL);
}

static int dap_handle_variables(DapSession* session, int request_seq, cJSON* arguments) {
    cJSON* variables_ref_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "variablesReference") : NULL;
    cJSON* body = NULL;
    cJSON* variables = NULL;
    int variables_ref = 0;
    int frame_id = -1;
    DapVariableRef* child_ref = NULL;
    bool running = false;
    bool stopped = false;
    bool terminated = false;

    dap_session_wait_until_not_running(session);

    if (!session->runtime || !session->execution_started) {
        return dap_write_response(session, request_seq, "variables", 0, NULL, "Program is not running");
    }
    dap_session_snapshot(session, &running, &stopped, &terminated);
    if (!stopped || terminated) {
        if (terminated) {
            return dap_write_response(session, request_seq, "variables", 0, NULL, "Program has terminated");
        }
        return dap_write_response(session, request_seq, "variables", 0, NULL, "No active stop to inspect");
    }
    if (!variables_ref_item || !cJSON_IsNumber(variables_ref_item)) {
        return dap_write_response(session, request_seq, "variables", 0, NULL, "Missing arguments.variablesReference");
    }

    variables_ref = (int)cJSON_GetNumberValue(variables_ref_item);
    body = cJSON_CreateObject();
    variables = cJSON_CreateArray();
    cJSON_AddItemToObject(body, "variables", variables);

    frame_id = dap_arguments_frame_id_from_ref(session, variables_ref);
    if (frame_id > 0) {
        int index_from_top = dap_frame_index_from_frame_id(session, frame_id);
        int argument_count = vm_debug_frame_argument_count(session->runtime->vm, index_from_top);
        for (int i = 0; i < argument_count; i++) {
            const char* argument_name = NULL;
            const Value* argument_value = NULL;
            if (!vm_debug_get_frame_argument(session->runtime->vm,
                                             index_from_top,
                                             i,
                                             &argument_name,
                                             &argument_value)) {
                continue;
            }
            if (!argument_name || argument_name[0] == '\0') {
                continue;
            }
            dap_add_variable_json(variables, argument_name, argument_value, session);
        }
        return dap_write_response(session, request_seq, "variables", 1, body, NULL);
    }

    frame_id = dap_locals_frame_id_from_ref(session, variables_ref);
    if (frame_id > 0) {
        int index_from_top = dap_frame_index_from_frame_id(session, frame_id);
        int local_count = vm_debug_frame_non_argument_local_count(session->runtime->vm, index_from_top);
        for (int i = 0; i < local_count; i++) {
            const char* local_name = NULL;
            const Value* local_value = NULL;
            if (!vm_debug_get_frame_non_argument_local(session->runtime->vm,
                                                       index_from_top,
                                                       i,
                                                       &local_name,
                                                       &local_value)) {
                continue;
            }
            if (!local_name || local_name[0] == '\0') {
                continue;
            }
            dap_add_variable_json(variables, local_name, local_value, session);
        }
        return dap_write_response(session, request_seq, "variables", 1, body, NULL);
    }

    frame_id = dap_globals_frame_id_from_ref(session, variables_ref);
    if (frame_id > 0) {
        int global_count = vm_debug_global_count(session->runtime->vm);
        (void)frame_id;
        for (int i = 0; i < global_count; i++) {
            const char* global_name = NULL;
            const Value* global_value = NULL;
            if (!vm_debug_get_global(session->runtime->vm, i, &global_name, &global_value)) {
                continue;
            }
            if (!global_name || global_name[0] == '\0') {
                continue;
            }
            dap_add_variable_json(variables, global_name, global_value, session);
        }
        return dap_write_response(session, request_seq, "variables", 1, body, NULL);
    }

    child_ref = dap_variable_ref_list_find(&session->variable_refs, variables_ref);
    if (child_ref) {
        dap_append_value_children(variables, session, &child_ref->value);
        return dap_write_response(session, request_seq, "variables", 1, body, NULL);
    }

    cJSON_Delete(body);
    return dap_write_response(session, request_seq, "variables", 0, NULL, "Unknown variablesReference");
}

static int dap_write_set_variable_response(DapSession* session,
                                           int request_seq,
                                           const Value* value) {
    cJSON* body = NULL;
    char* value_text = NULL;
    const char* type_text = NULL;
    int variables_ref = 0;

    if (!session || !value) {
        return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Failed to update variable");
    }

    dap_variable_ref_list_clear(&session->variable_refs);

    body = cJSON_CreateObject();
    value_text = dap_format_value_text(session, value);
    type_text = vm_debug_value_type_name(value);
    if (dap_value_is_expandable(value)) {
        variables_ref = dap_variable_ref_list_add(&session->variable_refs, value);
    }

    cJSON_AddStringToObject(body, "value", value_text ? value_text : "");
    cJSON_AddStringToObject(body, "type", type_text ? type_text : "unknown");
    cJSON_AddNumberToObject(body, "variablesReference", variables_ref);
    if (variables_ref > 0) {
        int named_count = dap_value_named_child_count(value);
        int indexed_count = dap_value_indexed_child_count(value);
        if (named_count > 0) {
            cJSON_AddNumberToObject(body, "namedVariables", named_count);
        }
        if (indexed_count > 0) {
            cJSON_AddNumberToObject(body, "indexedVariables", indexed_count);
        }
    }

    if (value_text) free(value_text);
    return dap_write_response(session, request_seq, "setVariable", 1, body, NULL);
}

static int dap_handle_set_variable(DapSession* session, int request_seq, cJSON* arguments) {
    cJSON* variables_ref_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "variablesReference") : NULL;
    cJSON* name_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "name") : NULL;
    cJSON* value_item = arguments ? cJSON_GetObjectItemCaseSensitive(arguments, "value") : NULL;
    const VmDebugStopInfo* stop = NULL;
    const char* parse_error = NULL;
    Value assigned_value;
    int variables_ref = 0;
    int frame_id = -1;
    bool running = false;
    bool stopped = false;
    bool terminated = false;

    dap_session_wait_until_not_running(session);

    if (!session->runtime || !session->execution_started) {
        return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Program is not running");
    }
    dap_session_snapshot(session, &running, &stopped, &terminated);
    if (!stopped || terminated) {
        if (terminated) {
            return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Program has terminated");
        }
        return dap_write_response(session, request_seq, "setVariable", 0, NULL, "No active stop to edit");
    }
    stop = vm_debug_get_stop_info(session->runtime->vm);
    if (!stop || stop->kind == VM_DEBUG_STOP_NONE) {
        return dap_write_response(session, request_seq, "setVariable", 0, NULL, "No active stop to edit");
    }
    if (!variables_ref_item || !cJSON_IsNumber(variables_ref_item)) {
        return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Missing arguments.variablesReference");
    }
    if (!name_item || !cJSON_IsString(name_item) || !name_item->valuestring || name_item->valuestring[0] == '\0') {
        return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Missing arguments.name");
    }
    if (!value_item || !cJSON_IsString(value_item) || !value_item->valuestring) {
        return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Missing arguments.value");
    }

    variables_ref = (int)cJSON_GetNumberValue(variables_ref_item);
    frame_id = dap_arguments_frame_id_from_ref(session, variables_ref);
    if (frame_id <= 0) {
        frame_id = dap_locals_frame_id_from_ref(session, variables_ref);
    }
    if (frame_id <= 0) {
        frame_id = dap_globals_frame_id_from_ref(session, variables_ref);
    }
    if (frame_id <= 0) {
        return dap_write_response(session,
                                  request_seq,
                                  "setVariable",
                                  0,
                                  NULL,
                                  "Nested variable mutation is not supported");
    }
    if (dap_frame_index_from_frame_id(session, frame_id) < 0 ||
        dap_frame_index_from_frame_id(session, frame_id) >= vm_debug_frame_count(session->runtime->vm)) {
        return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Invalid variablesReference");
    }

    if (!dap_parse_assignment_value(session,
                                    frame_id,
                                    value_item->valuestring,
                                    &assigned_value,
                                    &parse_error)) {
        return dap_write_response(session,
                                  request_seq,
                                  "setVariable",
                                  0,
                                  NULL,
                                  (parse_error && parse_error[0] != '\0') ? parse_error : "Invalid setVariable value");
    }

    frame_id = dap_arguments_frame_id_from_ref(session, variables_ref);
    if (frame_id > 0) {
        int index_from_top = dap_frame_index_from_frame_id(session, frame_id);
        int argument_count = vm_debug_frame_argument_count(session->runtime->vm, index_from_top);
        for (int i = 0; i < argument_count; i++) {
            const char* argument_name = NULL;
            const Value* argument_value = NULL;
            if (!vm_debug_get_frame_argument(session->runtime->vm,
                                             index_from_top,
                                             i,
                                             &argument_name,
                                             &argument_value)) {
                continue;
            }
            if (!argument_name || strcmp(argument_name, name_item->valuestring) != 0) continue;
            if (!vm_debug_set_frame_argument(session->runtime->vm, index_from_top, i, &assigned_value)) {
                value_free(&assigned_value);
                return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Failed to update argument");
            }
            if (!vm_debug_get_frame_argument(session->runtime->vm,
                                             index_from_top,
                                             i,
                                             &argument_name,
                                             &argument_value)) {
                value_free(&assigned_value);
                return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Failed to read updated argument");
            }
            value_free(&assigned_value);
            return dap_write_set_variable_response(session, request_seq, argument_value);
        }
        value_free(&assigned_value);
        return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Unknown argument in selected frame");
    }

    frame_id = dap_locals_frame_id_from_ref(session, variables_ref);
    if (frame_id > 0) {
        int index_from_top = dap_frame_index_from_frame_id(session, frame_id);
        int local_count = vm_debug_frame_non_argument_local_count(session->runtime->vm, index_from_top);
        for (int i = 0; i < local_count; i++) {
            const char* local_name = NULL;
            const Value* local_value = NULL;
            if (!vm_debug_get_frame_non_argument_local(session->runtime->vm,
                                                       index_from_top,
                                                       i,
                                                       &local_name,
                                                       &local_value)) {
                continue;
            }
            if (!local_name || strcmp(local_name, name_item->valuestring) != 0) continue;
            if (!vm_debug_set_frame_non_argument_local(session->runtime->vm, index_from_top, i, &assigned_value)) {
                value_free(&assigned_value);
                return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Failed to update local");
            }
            if (!vm_debug_get_frame_non_argument_local(session->runtime->vm,
                                                       index_from_top,
                                                       i,
                                                       &local_name,
                                                       &local_value)) {
                value_free(&assigned_value);
                return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Failed to read updated local");
            }
            value_free(&assigned_value);
            return dap_write_set_variable_response(session, request_seq, local_value);
        }
        value_free(&assigned_value);
        return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Unknown local in selected frame");
    }

    frame_id = dap_globals_frame_id_from_ref(session, variables_ref);
    if (frame_id > 0) {
        int global_count = vm_debug_global_count(session->runtime->vm);
        for (int i = 0; i < global_count; i++) {
            const char* global_name = NULL;
            const Value* global_value = NULL;
            if (!vm_debug_get_global(session->runtime->vm, i, &global_name, &global_value)) {
                continue;
            }
            if (!global_name || strcmp(global_name, name_item->valuestring) != 0) continue;
            if (!vm_debug_set_global(session->runtime->vm, i, &assigned_value)) {
                value_free(&assigned_value);
                return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Failed to update global");
            }
            if (!vm_debug_get_global(session->runtime->vm, i, &global_name, &global_value)) {
                value_free(&assigned_value);
                return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Failed to read updated global");
            }
            value_free(&assigned_value);
            return dap_write_set_variable_response(session, request_seq, global_value);
        }
        value_free(&assigned_value);
        return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Unknown global in selected scope");
    }

    value_free(&assigned_value);
    return dap_write_response(session, request_seq, "setVariable", 0, NULL, "Unknown variablesReference");
}

static int dap_handle_resume_request(DapSession* session,
                                     int request_seq,
                                     const char* command,
                                     DapExecAction action,
                                     int include_all_threads_body) {
    const VmDebugStopInfo* stop = NULL;
    const char* start_error = NULL;
    cJSON* body = NULL;
    bool running = false;
    bool stopped = false;
    bool terminated = false;

    if (!session->runtime || !session->execution_started) {
        return dap_write_response(session, request_seq, command, 0, NULL, "Program is not running");
    }
    dap_session_snapshot(session, &running, &stopped, &terminated);
    if (running) {
        dap_session_wait_until_not_running(session);
        dap_session_snapshot(session, &running, &stopped, &terminated);
    }
    if (!stopped || terminated) {
        return dap_write_response(session, request_seq, command, 0, NULL, "No active stop to resume from");
    }
    stop = vm_debug_get_stop_info(session->runtime->vm);
    if (!stop || stop->kind == VM_DEBUG_STOP_NONE) {
        return dap_write_response(session, request_seq, command, 0, NULL, "No active stop to resume from");
    }

    if (include_all_threads_body) {
        body = cJSON_CreateObject();
        cJSON_AddBoolToObject(body, "allThreadsContinued", 1);
    }

    if (stop->kind == VM_DEBUG_STOP_EXCEPTION && runtime_has_error(session->runtime)) {
        mtx_lock(&session->state_mutex);
        session->execution_stopped = false;
        session->terminated = true;
        mtx_unlock(&session->state_mutex);
        dap_write_response(session, request_seq, command, 1, body, NULL);
        dap_emit_post_execution(session,
                                &(DapExecutionResult){
                                    .terminated = true,
                                    .has_error = true,
                                    .error_message = runtime_get_error(session->runtime),
                                });
        dap_session_broadcast_state(session);
        return 1;
    }

    if (!dap_start_execution_async(session, action, &start_error)) {
        return dap_write_response(session,
                                  request_seq,
                                  command,
                                  0,
                                  body,
                                  start_error ? start_error : "Failed to resume program");
    }

    return dap_write_response(session, request_seq, command, 1, body, NULL);
}

static int dap_handle_disconnect(DapSession* session, int request_seq) {
    bool running = false;
    bool stopped = false;
    bool terminated = false;

    dap_session_snapshot(session, &running, &stopped, &terminated);
    if (running) {
        dap_session_wait_until_not_running(session);
        dap_session_snapshot(session, &running, &stopped, &terminated);
    }

    dap_write_response(session, request_seq, "disconnect", 1, NULL, NULL);
    if (!terminated) {
        mtx_lock(&session->state_mutex);
        session->execution_stopped = false;
        session->terminated = true;
        mtx_unlock(&session->state_mutex);
        dap_emit_terminated_event(session);
        dap_session_broadcast_state(session);
    }
    return 2;
}

static int dap_run_stdio_server(void) {
    DapSession session;
    int exit_code = 0;

    if (!dap_session_init(&session)) {
        return 1;
    }

    for (;;) {
        int content_length = 0;
        char* payload = NULL;
        cJSON* message = NULL;
        cJSON* type_item = NULL;
        cJSON* command_item = NULL;
        cJSON* arguments = NULL;
        cJSON* seq_item = NULL;
        int request_seq = 0;
        int handler_result = 1;

        if (!dap_read_content_length(stdin, &content_length)) {
            break;
        }
        if (content_length <= 0) {
            exit_code = 1;
            break;
        }

        payload = (char*)safe_malloc((size_t)content_length + 1);
        if ((int)fread(payload, 1, (size_t)content_length, stdin) != content_length) {
            free(payload);
            exit_code = 1;
            break;
        }
        payload[content_length] = '\0';

        message = cJSON_Parse(payload);
        free(payload);
        if (!message) {
            continue;
        }

        type_item = cJSON_GetObjectItemCaseSensitive(message, "type");
        command_item = cJSON_GetObjectItemCaseSensitive(message, "command");
        arguments = cJSON_GetObjectItemCaseSensitive(message, "arguments");
        seq_item = cJSON_GetObjectItemCaseSensitive(message, "seq");
        request_seq = (seq_item && cJSON_IsNumber(seq_item)) ? (int)cJSON_GetNumberValue(seq_item) : 0;

        if (!type_item || !cJSON_IsString(type_item) || strcmp(type_item->valuestring, "request") != 0 ||
            !command_item || !cJSON_IsString(command_item) || !command_item->valuestring) {
            cJSON_Delete(message);
            continue;
        }

        if (strcmp(command_item->valuestring, "initialize") == 0) {
            handler_result = dap_handle_initialize(&session, request_seq);
        } else if (strcmp(command_item->valuestring, "launch") == 0) {
            handler_result = dap_handle_launch(&session, request_seq, arguments);
        } else if (strcmp(command_item->valuestring, "setBreakpoints") == 0) {
            handler_result = dap_handle_set_breakpoints(&session, request_seq, arguments);
        } else if (strcmp(command_item->valuestring, "setExceptionBreakpoints") == 0) {
            handler_result = dap_handle_set_exception_breakpoints(&session, request_seq, arguments);
        } else if (strcmp(command_item->valuestring, "configurationDone") == 0) {
            handler_result = dap_handle_configuration_done(&session, request_seq);
        } else if (strcmp(command_item->valuestring, "threads") == 0) {
            handler_result = dap_handle_threads(&session, request_seq);
        } else if (strcmp(command_item->valuestring, "stackTrace") == 0) {
            handler_result = dap_handle_stack_trace(&session, request_seq, arguments);
        } else if (strcmp(command_item->valuestring, "exceptionInfo") == 0) {
            handler_result = dap_handle_exception_info(&session, request_seq, arguments);
        } else if (strcmp(command_item->valuestring, "scopes") == 0) {
            handler_result = dap_handle_scopes(&session, request_seq, arguments);
        } else if (strcmp(command_item->valuestring, "variables") == 0) {
            handler_result = dap_handle_variables(&session, request_seq, arguments);
        } else if (strcmp(command_item->valuestring, "setVariable") == 0) {
            handler_result = dap_handle_set_variable(&session, request_seq, arguments);
        } else if (strcmp(command_item->valuestring, "evaluate") == 0) {
            handler_result = dap_handle_evaluate(&session, request_seq, arguments);
        } else if (strcmp(command_item->valuestring, "pause") == 0) {
            handler_result = dap_handle_pause(&session, request_seq, arguments);
        } else if (strcmp(command_item->valuestring, "continue") == 0) {
            handler_result = dap_handle_resume_request(&session,
                                                       request_seq,
                                                       "continue",
                                                       DAP_EXEC_CONTINUE,
                                                       1);
        } else if (strcmp(command_item->valuestring, "stepIn") == 0) {
            handler_result = dap_handle_resume_request(&session,
                                                       request_seq,
                                                       "stepIn",
                                                       DAP_EXEC_STEP_IN,
                                                       0);
        } else if (strcmp(command_item->valuestring, "next") == 0) {
            handler_result = dap_handle_resume_request(&session,
                                                       request_seq,
                                                       "next",
                                                       DAP_EXEC_STEP_OVER,
                                                       0);
        } else if (strcmp(command_item->valuestring, "stepOut") == 0) {
            handler_result = dap_handle_resume_request(&session,
                                                       request_seq,
                                                       "stepOut",
                                                       DAP_EXEC_STEP_OUT,
                                                       0);
        } else if (strcmp(command_item->valuestring, "disconnect") == 0) {
            handler_result = dap_handle_disconnect(&session, request_seq);
        } else {
            handler_result = dap_write_response(&session,
                                                request_seq,
                                                command_item->valuestring,
                                                0,
                                                NULL,
                                                "Command not supported");
        }

        cJSON_Delete(message);
        if (handler_result == 2) {
            break;
        }
    }

    dap_session_free(&session);
    return exit_code;
}

static void print_dap_usage(const char* program_name) {
    printf("Usage: %s dap <subcommand>\n", program_name);
    printf("Subcommands:\n");
    printf("  --stdio           Run a stdio DAP server (initialize/launch/setBreakpoints/setExceptionBreakpoints/configurationDone/threads/stackTrace/exceptionInfo/scopes/variables/setVariable/evaluate/pause/continue/next/stepIn/stepOut/disconnect)\n");
}

int cli_dap(int argc, char** argv) {
    if (argc < 3 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0) {
        print_dap_usage(argv[0]);
        return argc < 3 ? 1 : 0;
    }

    if (strcmp(argv[2], "--stdio") == 0) {
        return dap_run_stdio_server();
    }

    fprintf(stderr, "Error: unknown dap subcommand '%s'\n", argv[2]);
    print_dap_usage(argv[0]);
    return 1;
}
