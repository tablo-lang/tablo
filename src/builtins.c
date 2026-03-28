#include "builtins.h"
#include "crypto_aes.h"
#include "crypto_hash.h"
#include "gzip_codec.h"
#include "http_fuzz.h"
#include "path_utils.h"
#include "vm.h"
#include "safe_alloc.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>
#include <stdint.h>
#include <stddef.h>

#ifdef TABLO_SQLITE_STATIC_LINK
#include <sqlite3.h>
#endif

typedef struct Runtime Runtime;
Runtime* runtime_create(const char* file_path);
void runtime_free(Runtime* rt);
void runtime_set_thread_channels(Runtime* rt, int inbox_channel_id, int outbox_channel_id);
int runtime_run_function(Runtime* rt, const char* function_name);
bool runtime_take_return_value(Runtime* rt, Value* out);
bool runtime_has_error(Runtime* rt);
const char* runtime_get_error(Runtime* rt);

static bool json_from_cjson(VM* vm, const cJSON* item, Value* out, int depth);
static cJSON* json_to_cjson(VM* vm, const Value* val, int depth, char* error_buf, size_t error_buf_size);
static bool json_parse_fast_scalar(VM* vm, const char* json, Value* out);
static bool json_parse_fast_common(VM* vm, const char* json, Value* out);
static bool json_stringify_fast_scalar(VM* vm, const Value* input, Value* out);
static bool json_stringify_fast_common(VM* vm, const Value* input, Value* out);
static bool json_decode_value_with_schema(VM* vm,
                                          const Value* value,
                                          const Value* schema,
                                          const char* path,
                                          const char* schema_path,
                                          int depth,
                                          Value* out,
                                          int64_t* err_code,
                                          char* err_msg,
                                          size_t err_msg_size,
                                          Value* err_data);
static int socket_send_dispatch(ObjSocket* sock, const char* data, int len);
static int socket_recv_dispatch(ObjSocket* sock, char* out, int max_len);
static void socket_close_dispatch(ObjSocket* sock);
static bool socket_recv_error_is_connection_close(void);
static bool tcpx_set_socket_timeouts(int sock_fd, int timeout_ms, const char** out_err);
static bool tcpx_connect_host_port(VM* vm,
                                   const char* host,
                                   int port,
                                   int timeout_ms,
                                   int* out_sock,
                                   int64_t* out_err_code,
                                   const char** out_err_msg);
static bool httpx_connect_socket(VM* vm,
                                 const char* host,
                                 int port,
                                 int timeout_ms,
                                 bool is_https,
                                 bool tls_insecure_skip_verify,
                                 ObjSocket** out_socket,
                                 int64_t* err_code,
                                 const char** err_msg);
#ifdef _WIN32
typedef struct TlsxTransportCtx TlsxTransportCtx;
static void tlsx_ctx_reset(TlsxTransportCtx* ctx);
static bool tlsx_handshake_client(ObjSocket* sock,
                                  const char* host,
                                  TlsxTransportCtx* ctx,
                                  bool insecure_skip_verify,
                                  const char** out_err);
static int tlsx_transport_send(ObjSocket* sock, const char* data, int len);
static int tlsx_transport_receive(ObjSocket* sock, char* out, int max_len);
static void tlsx_transport_close(ObjSocket* sock);
#endif

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#if !defined(TABLO_SQLITE_STATIC_LINK)
#include <dlfcn.h>
#endif
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

// Socket includes
#ifdef _WIN32
    #define NOMINMAX
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #ifndef SECURITY_WIN32
    #define SECURITY_WIN32
    #endif
    #include <security.h>
    #include <schannel.h>
    #include <bcrypt.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "secur32.lib")
    #pragma comment(lib, "crypt32.lib")
    #pragma comment(lib, "bcrypt.lib")
    #define CLOSESOCKET closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
#include <fcntl.h>
    #define CLOSESOCKET close
#endif

#ifdef _WIN32
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
#endif

static ObjBigInt* random_bigint_range_sfc(VM* vm, ObjBigInt* min, ObjBigInt* max);
static ObjBigInt* random_bigint_range_secure(ObjBigInt* min, ObjBigInt* max, bool* ok);

typedef enum {
    ERR_INVALID_ARGUMENT = 1,
    ERR_PARSE = 2,
    ERR_PERMISSION = 3,
    ERR_IO = 4,
    ERR_LIMIT = 5,
    ERR_UNSUPPORTED = 6,
    ERR_NETWORK = 7,
    ERR_HTTP = 8,
    ERR_CRYPTO = 9,
    ERR_INTERNAL = 10
} ErrorCode;

static void register_error_code_constants(VM* vm) {
    Value v;

    value_init_int(&v, ERR_INVALID_ARGUMENT);
    vm_set_global(vm, "ERR_INVALID_ARGUMENT", v);

    value_init_int(&v, ERR_PARSE);
    vm_set_global(vm, "ERR_PARSE", v);

    value_init_int(&v, ERR_PERMISSION);
    vm_set_global(vm, "ERR_PERMISSION", v);

    value_init_int(&v, ERR_IO);
    vm_set_global(vm, "ERR_IO", v);

    value_init_int(&v, ERR_LIMIT);
    vm_set_global(vm, "ERR_LIMIT", v);

    value_init_int(&v, ERR_UNSUPPORTED);
    vm_set_global(vm, "ERR_UNSUPPORTED", v);

    value_init_int(&v, ERR_NETWORK);
    vm_set_global(vm, "ERR_NETWORK", v);

    value_init_int(&v, ERR_HTTP);
    vm_set_global(vm, "ERR_HTTP", v);

    value_init_int(&v, ERR_CRYPTO);
    vm_set_global(vm, "ERR_CRYPTO", v);

    value_init_int(&v, ERR_INTERNAL);
    vm_set_global(vm, "ERR_INTERNAL", v);
}

#ifdef _WIN32
typedef CRITICAL_SECTION syncx_mutex_t;
typedef CONDITION_VARIABLE syncx_cond_t;
#else
typedef pthread_mutex_t syncx_mutex_t;
typedef pthread_cond_t syncx_cond_t;
#endif

static void syncx_mutex_init(syncx_mutex_t* m) {
#ifdef _WIN32
    InitializeCriticalSection(m);
#else
    pthread_mutex_init(m, NULL);
#endif
}

static void syncx_mutex_destroy(syncx_mutex_t* m) {
#ifdef _WIN32
    DeleteCriticalSection(m);
#else
    pthread_mutex_destroy(m);
#endif
}

static void syncx_mutex_lock(syncx_mutex_t* m) {
#ifdef _WIN32
    EnterCriticalSection(m);
#else
    pthread_mutex_lock(m);
#endif
}

static void syncx_mutex_unlock(syncx_mutex_t* m) {
#ifdef _WIN32
    LeaveCriticalSection(m);
#else
    pthread_mutex_unlock(m);
#endif
}

static void syncx_cond_init(syncx_cond_t* c) {
#ifdef _WIN32
    InitializeConditionVariable(c);
#else
    pthread_cond_init(c, NULL);
#endif
}

static void syncx_cond_destroy(syncx_cond_t* c) {
#ifdef _WIN32
    (void)c;
#else
    pthread_cond_destroy(c);
#endif
}

static void syncx_cond_signal(syncx_cond_t* c) {
#ifdef _WIN32
    WakeConditionVariable(c);
#else
    pthread_cond_signal(c);
#endif
}

static void syncx_cond_broadcast(syncx_cond_t* c) {
#ifdef _WIN32
    WakeAllConditionVariable(c);
#else
    pthread_cond_broadcast(c);
#endif
}

static bool syncx_cond_wait_ms(syncx_cond_t* c, syncx_mutex_t* m, int timeout_ms) {
#ifdef _WIN32
    DWORD wait_ms = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
    return SleepConditionVariableCS(c, m, wait_ms) != 0;
#else
    if (timeout_ms < 0) {
        int rc = 0;
        do {
            rc = pthread_cond_wait(c, m);
        } while (rc == EINTR);
        return rc == 0;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return false;
    }

    ts.tv_sec += timeout_ms / 1000;
    long add_ns = (long)(timeout_ms % 1000) * 1000000L;
    ts.tv_nsec += add_ns;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    int rc = 0;
    do {
        rc = pthread_cond_timedwait(c, m, &ts);
    } while (rc == EINTR);
    return rc == 0;
#endif
}

static uint64_t syncx_now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
#endif
}

#ifdef _WIN32
static DWORD syncx_current_thread_id(void) {
    return GetCurrentThreadId();
}

static bool syncx_thread_id_equals(DWORD a, DWORD b) {
    return a == b;
}
#else
static pthread_t syncx_current_thread_id(void) {
    return pthread_self();
}

static bool syncx_thread_id_equals(pthread_t a, pthread_t b) {
    return pthread_equal(a, b) != 0;
}
#endif

typedef enum {
    SYNCX_VALUE_NIL = 0,
    SYNCX_VALUE_INT = 1,
    SYNCX_VALUE_BOOL = 2,
    SYNCX_VALUE_DOUBLE = 3,
    SYNCX_VALUE_STRING = 4,
    SYNCX_VALUE_BYTES = 5,
    SYNCX_VALUE_JSON = 6
} SyncxValueKind;

typedef struct {
    SyncxValueKind kind;
    int64_t as_int;
    bool as_bool;
    double as_double;
    char* as_string;
    uint8_t* as_bytes;
    int as_bytes_len;
} SyncxValue;

typedef struct SyncxMessageNode {
    SyncxValue value;
    struct SyncxMessageNode* next;
} SyncxMessageNode;

typedef struct {
    int id;
    int capacity;
    int count;
    bool closed;
    SyncxMessageNode* head;
    SyncxMessageNode* tail;
    syncx_mutex_t mutex;
    syncx_cond_t not_empty;
    syncx_cond_t not_full;
} SyncxChannel;

typedef struct {
    int id;
    SyncxValue value;
    syncx_mutex_t mutex;
} SyncxSharedCell;

typedef struct SyncxThreadHandle {
    int id;
#ifdef _WIN32
    HANDLE os_thread;
#else
    pthread_t os_thread;
#endif
    bool started;
    bool joined;
    bool finished;
    bool has_result_payload;
    SyncxValue result_payload;
    int exit_code;
    int64_t error_code;
    char* error_message;
    syncx_mutex_t mutex;
    syncx_cond_t finished_cond;
} SyncxThreadHandle;

typedef struct {
    SyncxThreadHandle* handle;
    char* file_path;
    char* function_name;
    int inbox_channel_id;
    int outbox_channel_id;
    bool has_arg_payload;
    SyncxValue arg_payload;
} SyncxThreadTask;

typedef enum {
    ASYNC_CHANNEL_WAIT_SEND = 0,
    ASYNC_CHANNEL_WAIT_RECV = 1,
    ASYNC_CHANNEL_WAIT_RECV_TYPED = 2
} AsyncChannelWaitKind;

typedef struct {
    AsyncChannelWaitKind kind;
    ObjRecord* future;
    int channel_id;
    SyncxValue payload;
    Value schema;
} AsyncChannelWait;

typedef struct {
    int id;
    SyncxSharedCell* cell;
    bool active;
#ifdef _WIN32
    DWORD owner_thread_id;
#else
    pthread_t owner_thread_id;
#endif
} SyncxArcGuard;

#ifdef _WIN32
__declspec(thread) static int g_syncx_tls_inbox = -1;
__declspec(thread) static int g_syncx_tls_outbox = -1;
__declspec(thread) static bool g_syncx_tls_has_arg_payload = false;
__declspec(thread) static SyncxValue g_syncx_tls_arg_payload;
#else
static _Thread_local int g_syncx_tls_inbox = -1;
static _Thread_local int g_syncx_tls_outbox = -1;
static _Thread_local bool g_syncx_tls_has_arg_payload = false;
static _Thread_local SyncxValue g_syncx_tls_arg_payload;
#endif

static SyncxChannel** g_syncx_channels = NULL;
static int g_syncx_channel_count = 0;
static int g_syncx_channel_capacity = 0;
static int g_syncx_next_channel_id = 1;

static SyncxSharedCell** g_syncx_shared_cells = NULL;
static int g_syncx_shared_cell_count = 0;
static int g_syncx_shared_cell_capacity = 0;
static int g_syncx_next_shared_cell_id = 1;

static SyncxThreadHandle** g_syncx_threads = NULL;
static int g_syncx_thread_count = 0;
static int g_syncx_thread_capacity = 0;
static int g_syncx_next_thread_id = 1;

static SyncxArcGuard** g_syncx_guards = NULL;
static int g_syncx_guard_count = 0;
static int g_syncx_guard_capacity = 0;
static int g_syncx_next_guard_id = 1;

static syncx_mutex_t g_syncx_registry_mutex;

typedef struct {
    int id;
    bool capture_stdout;
    bool capture_stderr;
    bool finished;
    bool killed;
    bool stdout_truncated;
    bool stderr_truncated;
    int exit_code;
    char* stdout_buf;
    size_t stdout_len;
    size_t stdout_cap;
    char* stderr_buf;
    size_t stderr_len;
    size_t stderr_cap;
    syncx_mutex_t mutex;
#ifdef _WIN32
    HANDLE process_handle;
    HANDLE thread_handle;
    HANDLE stdin_write;
    HANDLE stdout_read;
    HANDLE stderr_read;
#else
    pid_t pid;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
#endif
} ProcxHandle;

static ProcxHandle** g_procx_handles = NULL;
static int g_procx_handle_count = 0;
static int g_procx_handle_capacity = 0;
static int g_procx_next_handle_id = 1;

#define PROCX_MAX_CAPTURE_BYTES (8u * 1024u * 1024u)
#define PROCX_WAIT_POLL_MS 10

typedef struct sqlite3 SqlxNativeDb;
typedef struct sqlite3_stmt SqlxNativeStmt;
typedef int64_t SqlxInt64;
typedef void (*sqlx_destructor_fn)(void*);

typedef int (*sqlx_open_v2_fn)(const char* filename, SqlxNativeDb** out_db, int flags, const char* vfs);
typedef int (*sqlx_close_v2_fn)(SqlxNativeDb* db);
typedef int (*sqlx_exec_fn)(SqlxNativeDb* db,
                            const char* sql,
                            int (*callback)(void*, int, char**, char**),
                            void* callback_arg,
                            char** out_errmsg);
typedef const char* (*sqlx_errmsg_fn)(SqlxNativeDb* db);
typedef void (*sqlx_free_fn)(void* ptr);
typedef int (*sqlx_prepare_v2_fn)(SqlxNativeDb* db,
                                  const char* sql,
                                  int nbyte,
                                  SqlxNativeStmt** out_stmt,
                                  const char** out_tail);
typedef int (*sqlx_bind_int64_fn)(SqlxNativeStmt* stmt, int index, SqlxInt64 value);
typedef int (*sqlx_bind_double_fn)(SqlxNativeStmt* stmt, int index, double value);
typedef int (*sqlx_bind_text_fn)(SqlxNativeStmt* stmt, int index, const char* value, int nbyte, sqlx_destructor_fn dtor);
typedef int (*sqlx_bind_blob_fn)(SqlxNativeStmt* stmt, int index, const void* value, int nbyte, sqlx_destructor_fn dtor);
typedef int (*sqlx_bind_null_fn)(SqlxNativeStmt* stmt, int index);
typedef int (*sqlx_reset_fn)(SqlxNativeStmt* stmt);
typedef int (*sqlx_clear_bindings_fn)(SqlxNativeStmt* stmt);
typedef int (*sqlx_changes_fn)(SqlxNativeDb* db);
typedef SqlxInt64 (*sqlx_last_insert_rowid_fn)(SqlxNativeDb* db);
typedef int (*sqlx_step_fn)(SqlxNativeStmt* stmt);
typedef int (*sqlx_finalize_fn)(SqlxNativeStmt* stmt);
typedef int (*sqlx_column_count_fn)(SqlxNativeStmt* stmt);
typedef const char* (*sqlx_column_name_fn)(SqlxNativeStmt* stmt, int col);
typedef int (*sqlx_column_type_fn)(SqlxNativeStmt* stmt, int col);
typedef SqlxInt64 (*sqlx_column_int64_fn)(SqlxNativeStmt* stmt, int col);
typedef double (*sqlx_column_double_fn)(SqlxNativeStmt* stmt, int col);
typedef const unsigned char* (*sqlx_column_text_fn)(SqlxNativeStmt* stmt, int col);
typedef const void* (*sqlx_column_blob_fn)(SqlxNativeStmt* stmt, int col);
typedef int (*sqlx_column_bytes_fn)(SqlxNativeStmt* stmt, int col);

typedef struct {
    bool load_attempted;
    bool available;
#ifdef _WIN32
    HMODULE lib_handle;
#elif !defined(TABLO_SQLITE_STATIC_LINK)
    void* lib_handle;
#endif
    sqlx_open_v2_fn open_v2;
    sqlx_close_v2_fn close_v2;
    sqlx_exec_fn exec;
    sqlx_errmsg_fn errmsg;
    sqlx_free_fn free_mem;
    sqlx_prepare_v2_fn prepare_v2;
    sqlx_bind_int64_fn bind_int64;
    sqlx_bind_double_fn bind_double;
    sqlx_bind_text_fn bind_text;
    sqlx_bind_blob_fn bind_blob;
    sqlx_bind_null_fn bind_null;
    sqlx_reset_fn reset;
    sqlx_clear_bindings_fn clear_bindings;
    sqlx_changes_fn changes;
    sqlx_last_insert_rowid_fn last_insert_rowid;
    sqlx_step_fn step;
    sqlx_finalize_fn finalize;
    sqlx_column_count_fn column_count;
    sqlx_column_name_fn column_name;
    sqlx_column_type_fn column_type;
    sqlx_column_int64_fn column_int64;
    sqlx_column_double_fn column_double;
    sqlx_column_text_fn column_text;
    sqlx_column_blob_fn column_blob;
    sqlx_column_bytes_fn column_bytes;
} SqlxApi;

typedef struct {
    int id;
    bool closed;
    SqlxNativeDb* db;
    syncx_mutex_t mutex;
} SqlxDbHandle;

typedef struct {
    int id;
    int db_id;
    bool finalized;
    bool done;
    SqlxNativeStmt* stmt;
} SqlxStmtHandle;

static SqlxApi g_sqlx_api = {0};
static SqlxDbHandle** g_sqlx_db_handles = NULL;
static int g_sqlx_db_count = 0;
static int g_sqlx_db_capacity = 0;
static int g_sqlx_next_db_id = 1;

static SqlxStmtHandle** g_sqlx_stmt_handles = NULL;
static int g_sqlx_stmt_count = 0;
static int g_sqlx_stmt_capacity = 0;
static int g_sqlx_next_stmt_id = 1;

#define SQLX_OPEN_READWRITE 0x00000002
#define SQLX_OPEN_CREATE 0x00000004
#define SQLX_OPEN_URI 0x00000040

#define SQLX_OK 0
#define SQLX_ROW 100
#define SQLX_DONE 101
#define SQLX_RANGE 25
#define SQLX_INTEGER 1
#define SQLX_FLOAT 2
#define SQLX_TEXT 3
#define SQLX_BLOB 4
#define SQLX_NULL 5
#define SQLX_TRANSIENT ((sqlx_destructor_fn)(intptr_t)-1)

static ProcxHandle* procx_lookup_unlocked(int id) {
    for (int i = 0; i < g_procx_handle_count; i++) {
        ProcxHandle* handle = g_procx_handles[i];
        if (handle && handle->id == id) return handle;
    }
    return NULL;
}

static void procx_append_unlocked(ProcxHandle* handle) {
    if (g_procx_handle_count >= g_procx_handle_capacity) {
        int next_capacity = g_procx_handle_capacity <= 0 ? 8 : g_procx_handle_capacity * 2;
        g_procx_handles = (ProcxHandle**)safe_realloc(g_procx_handles, (size_t)next_capacity * sizeof(ProcxHandle*));
        g_procx_handle_capacity = next_capacity;
    }
    g_procx_handles[g_procx_handle_count++] = handle;
}

static void procx_close_stdin_locked(ProcxHandle* handle);
static void procx_pump_outputs_locked(ProcxHandle* handle);
static bool procx_poll_exit_locked(ProcxHandle* handle);

static ProcxHandle* procx_handle_create(int id) {
    ProcxHandle* handle = (ProcxHandle*)safe_malloc(sizeof(ProcxHandle));
    memset(handle, 0, sizeof(*handle));
    handle->id = id;
    handle->exit_code = -1;
    syncx_mutex_init(&handle->mutex);
#ifdef _WIN32
    handle->process_handle = NULL;
    handle->thread_handle = NULL;
    handle->stdin_write = NULL;
    handle->stdout_read = NULL;
    handle->stderr_read = NULL;
#else
    handle->pid = -1;
    handle->stdin_fd = -1;
    handle->stdout_fd = -1;
    handle->stderr_fd = -1;
#endif
    return handle;
}

static void procx_sleep_ms(int ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)((unsigned int)ms * 1000u));
#endif
}

static void procx_buffer_append(char** buffer,
                                size_t* length,
                                size_t* capacity,
                                bool* truncated,
                                const char* data,
                                size_t data_len) {
    if (!buffer || !length || !capacity || !data || data_len == 0) return;

    if (*length >= PROCX_MAX_CAPTURE_BYTES) {
        if (truncated) *truncated = true;
        return;
    }

    size_t remaining = PROCX_MAX_CAPTURE_BYTES - *length;
    size_t append_len = data_len;
    if (append_len > remaining) {
        append_len = remaining;
        if (truncated) *truncated = true;
    }
    if (append_len == 0) return;

    size_t needed = *length + append_len + 1;
    if (*capacity < needed) {
        size_t next_capacity = (*capacity == 0) ? 1024 : *capacity;
        while (next_capacity < needed) {
            next_capacity *= 2;
        }
        *buffer = (char*)safe_realloc(*buffer, next_capacity);
        *capacity = next_capacity;
    }

    memcpy(*buffer + *length, data, append_len);
    *length += append_len;
    (*buffer)[*length] = '\0';
}

static Value procx_buffer_take_chunk(char** buffer,
                                     size_t* length,
                                     size_t* capacity,
                                     size_t max_bytes) {
    Value out;
    value_init_nil(&out);
    if (!buffer || !length || !capacity) return out;
    if (!*buffer || *length == 0 || max_bytes == 0) return out;

    size_t take_len = *length;
    if (take_len > max_bytes) take_len = max_bytes;

    char* chunk = (char*)safe_malloc(take_len + 1);
    memcpy(chunk, *buffer, take_len);
    chunk[take_len] = '\0';
    value_init_string(&out, chunk);
    free(chunk);

    size_t remaining = *length - take_len;
    if (remaining > 0) {
        memmove(*buffer, *buffer + take_len, remaining);
    }
    *length = remaining;
    (*buffer)[remaining] = '\0';

    if (remaining == 0) {
        free(*buffer);
        *buffer = NULL;
        *capacity = 0;
    }

    return out;
}

static Value procx_build_result_map(VM* vm, const ProcxHandle* handle, bool timed_out) {
    ObjMap* map = obj_map_create(vm);
    Value val;

    value_init_bool(&val, handle ? handle->finished : false);
    obj_map_set_cstr(map, "finished", val);
    value_free(&val);

    value_init_bool(&val, timed_out);
    obj_map_set_cstr(map, "timedOut", val);
    value_free(&val);

    value_init_int(&val, handle ? handle->exit_code : -1);
    obj_map_set_cstr(map, "exitCode", val);
    value_free(&val);

    value_init_string(&val, (handle && handle->stdout_buf) ? handle->stdout_buf : "");
    obj_map_set_cstr(map, "stdout", val);
    value_free(&val);

    value_init_string(&val, (handle && handle->stderr_buf) ? handle->stderr_buf : "");
    obj_map_set_cstr(map, "stderr", val);
    value_free(&val);

    value_init_bool(&val, handle ? handle->killed : false);
    obj_map_set_cstr(map, "killed", val);
    value_free(&val);

    value_init_bool(&val, handle ? handle->stdout_truncated : false);
    obj_map_set_cstr(map, "stdoutTruncated", val);
    value_free(&val);

    value_init_bool(&val, handle ? handle->stderr_truncated : false);
    obj_map_set_cstr(map, "stderrTruncated", val);
    value_free(&val);

    Value out;
    value_init_map(&out, map);
    return out;
}

static void procx_free_argv(char** argv, int argc) {
    if (!argv) return;
    for (int i = 0; i < argc; i++) {
        if (argv[i]) free(argv[i]);
    }
    free(argv);
}

static bool procx_build_argv(const Value* command_val,
                             const Value* args_val,
                             char*** out_argv,
                             int* out_argc,
                             const char** out_err) {
    if (out_err) *out_err = NULL;
    if (out_argv) *out_argv = NULL;
    if (out_argc) *out_argc = 0;
    if (!command_val || !args_val || !out_argv || !out_argc) return false;

    ObjString* command_str = value_get_string_obj(command_val);
    if (value_get_type(command_val) != VAL_STRING || !command_str || !command_str->chars) {
        if (out_err) *out_err = "processSpawn command must be a non-empty string";
        return false;
    }
    if (command_str->chars[0] == '\0') {
        if (out_err) *out_err = "processSpawn command cannot be empty";
        return false;
    }
    ObjArray* args_arr = value_get_array_obj(args_val);
    if (value_get_type(args_val) != VAL_ARRAY || !args_arr) {
        if (out_err) *out_err = "processSpawn args must be array<string>";
        return false;
    }

    int arg_count = args_arr->count;
    if (arg_count < 0) arg_count = 0;
    char** argv = (char**)safe_malloc((size_t)(arg_count + 2) * sizeof(char*));
    for (int i = 0; i < arg_count + 2; i++) argv[i] = NULL;

    int argc = 0;
    argv[argc++] = safe_strdup(command_str->chars);

    for (int i = 0; i < arg_count; i++) {
        Value item;
        value_init_nil(&item);
        obj_array_get(args_arr, i, &item);
        ObjString* item_str = value_get_string_obj(&item);
        if (value_get_type(&item) != VAL_STRING || !item_str || !item_str->chars) {
            procx_free_argv(argv, argc);
            if (out_err) *out_err = "processSpawn args must contain only strings";
            return false;
        }
        argv[argc++] = safe_strdup(item_str->chars);
    }
    argv[argc] = NULL;

    *out_argv = argv;
    *out_argc = argc;
    return true;
}

#ifdef _WIN32
static void procx_close_handle(HANDLE* handle) {
    if (!handle || !*handle) return;
    CloseHandle(*handle);
    *handle = NULL;
}

static void procx_pump_pipe_locked(HANDLE* read_handle,
                                   char** buffer,
                                   size_t* length,
                                   size_t* capacity,
                                   bool* truncated) {
    if (!read_handle || !*read_handle) return;

    char temp[4096];
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(*read_handle, NULL, 0, NULL, &available, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_INVALID_HANDLE) {
                // Writer side is closed; try draining any bytes still buffered in the pipe
                // before closing the read handle.
                for (;;) {
                    DWORD read_bytes = 0;
                    if (!ReadFile(*read_handle, temp, (DWORD)sizeof(temp), &read_bytes, NULL)) {
                        break;
                    }
                    if (read_bytes == 0) {
                        break;
                    }
                    procx_buffer_append(buffer, length, capacity, truncated, temp, (size_t)read_bytes);
                }
                procx_close_handle(read_handle);
            }
            break;
        }
        if (available == 0) break;

        DWORD to_read = available;
        if (to_read > (DWORD)sizeof(temp)) to_read = (DWORD)sizeof(temp);
        DWORD read_bytes = 0;
        if (!ReadFile(*read_handle, temp, to_read, &read_bytes, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF || err == ERROR_PIPE_NOT_CONNECTED) {
                procx_close_handle(read_handle);
            }
            break;
        }
        if (read_bytes == 0) {
            procx_close_handle(read_handle);
            break;
        }

        procx_buffer_append(buffer, length, capacity, truncated, temp, (size_t)read_bytes);
    }
}

static bool procx_poll_exit_locked(ProcxHandle* handle) {
    if (!handle) return false;
    if (handle->finished) return true;
    if (!handle->process_handle) {
        handle->finished = true;
        if (handle->exit_code < 0) handle->exit_code = 1;
        return true;
    }

    DWORD wait_rc = WaitForSingleObject(handle->process_handle, 0);
    if (wait_rc == WAIT_TIMEOUT) {
        return false;
    }

    DWORD raw_code = 1;
    if (!GetExitCodeProcess(handle->process_handle, &raw_code)) {
        raw_code = 1;
    }
    handle->exit_code = raw_code > (DWORD)INT_MAX ? INT_MAX : (int)raw_code;
    handle->finished = true;
    procx_close_handle(&handle->process_handle);
    procx_close_handle(&handle->thread_handle);
    return true;
}

static bool procx_windows_should_quote(const char* arg) {
    if (!arg || arg[0] == '\0') return true;
    for (const char* p = arg; *p; p++) {
        if (*p == '"' || *p == '\t' || *p == ' ') return true;
    }
    return false;
}

static char* procx_windows_quote_arg(const char* arg) {
    if (!arg) return safe_strdup("\"\"");
    if (!procx_windows_should_quote(arg)) {
        return safe_strdup(arg);
    }

    size_t len = strlen(arg);
    size_t cap = (len * 2) + 3;
    char* out = (char*)safe_malloc(cap);
    size_t pos = 0;
    out[pos++] = '"';

    size_t backslashes = 0;
    for (size_t i = 0; i < len; i++) {
        char ch = arg[i];
        if (ch == '\\') {
            backslashes++;
            continue;
        }

        if (ch == '"') {
            while (backslashes > 0) {
                out[pos++] = '\\';
                out[pos++] = '\\';
                backslashes--;
            }
            out[pos++] = '\\';
            out[pos++] = '"';
            continue;
        }

        while (backslashes > 0) {
            out[pos++] = '\\';
            backslashes--;
        }
        out[pos++] = ch;
    }

    while (backslashes > 0) {
        out[pos++] = '\\';
        out[pos++] = '\\';
        backslashes--;
    }
    out[pos++] = '"';
    out[pos] = '\0';
    return out;
}

static char* procx_windows_build_command_line(char** argv, int argc) {
    if (!argv || argc <= 0) return NULL;

    char** quoted = (char**)safe_malloc((size_t)argc * sizeof(char*));
    size_t total = 0;
    for (int i = 0; i < argc; i++) {
        quoted[i] = procx_windows_quote_arg(argv[i] ? argv[i] : "");
        total += strlen(quoted[i]);
        if (i + 1 < argc) total += 1;
    }

    char* out = (char*)safe_malloc(total + 1);
    size_t pos = 0;
    for (int i = 0; i < argc; i++) {
        size_t q_len = strlen(quoted[i]);
        memcpy(out + pos, quoted[i], q_len);
        pos += q_len;
        if (i + 1 < argc) out[pos++] = ' ';
        free(quoted[i]);
    }
    free(quoted);
    out[pos] = '\0';
    return out;
}

static bool procx_spawn_process_locked(ProcxHandle* handle,
                                       char** argv,
                                       int argc,
                                       bool capture_stdout,
                                       bool capture_stderr,
                                       const char** out_err) {
    if (out_err) *out_err = NULL;
    if (!handle || !argv || argc <= 0) {
        if (out_err) *out_err = "Invalid process spawn arguments";
        return false;
    }

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdin_read = NULL;
    HANDLE stdin_write = NULL;
    HANDLE stdout_read = NULL;
    HANDLE stdout_write = NULL;
    HANDLE stderr_read = NULL;
    HANDLE stderr_write = NULL;

    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        if (out_err) *out_err = "Failed to create stdin pipe";
        return false;
    }
    if (!SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0)) {
        procx_close_handle(&stdin_read);
        procx_close_handle(&stdin_write);
        if (out_err) *out_err = "Failed to configure stdin pipe";
        return false;
    }

    if (capture_stdout) {
        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
            !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
            procx_close_handle(&stdin_read);
            procx_close_handle(&stdin_write);
            procx_close_handle(&stdout_read);
            procx_close_handle(&stdout_write);
            if (out_err) *out_err = "Failed to create stdout pipe";
            return false;
        }
    }

    if (capture_stderr) {
        if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0) ||
            !SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
            procx_close_handle(&stdin_read);
            procx_close_handle(&stdin_write);
            procx_close_handle(&stdout_read);
            procx_close_handle(&stdout_write);
            procx_close_handle(&stderr_read);
            procx_close_handle(&stderr_write);
            if (out_err) *out_err = "Failed to create stderr pipe";
            return false;
        }
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = capture_stdout ? stdout_write : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = capture_stderr ? stderr_write : GetStdHandle(STD_ERROR_HANDLE);

    char* command_line = procx_windows_build_command_line(argv, argc);
    BOOL created = CreateProcessA(NULL,
                                  command_line,
                                  NULL,
                                  NULL,
                                  TRUE,
                                  CREATE_NO_WINDOW,
                                  NULL,
                                  NULL,
                                  &si,
                                  &pi);
    free(command_line);

    procx_close_handle(&stdin_read);
    procx_close_handle(&stdout_write);
    procx_close_handle(&stderr_write);

    if (!created) {
        procx_close_handle(&stdin_write);
        procx_close_handle(&stdout_read);
        procx_close_handle(&stderr_read);
        if (out_err) *out_err = "Failed to spawn process";
        return false;
    }

    handle->capture_stdout = capture_stdout;
    handle->capture_stderr = capture_stderr;
    handle->process_handle = pi.hProcess;
    handle->thread_handle = pi.hThread;
    handle->stdin_write = stdin_write;
    handle->stdout_read = stdout_read;
    handle->stderr_read = stderr_read;
    return true;
}

static void procx_close_stdin_locked(ProcxHandle* handle) {
    if (!handle) return;
    procx_close_handle(&handle->stdin_write);
}

static void procx_pump_outputs_locked(ProcxHandle* handle) {
    if (!handle) return;
    if (handle->capture_stdout) {
        procx_pump_pipe_locked(&handle->stdout_read,
                               &handle->stdout_buf,
                               &handle->stdout_len,
                               &handle->stdout_cap,
                               &handle->stdout_truncated);
    }
    if (handle->capture_stderr) {
        procx_pump_pipe_locked(&handle->stderr_read,
                               &handle->stderr_buf,
                               &handle->stderr_len,
                               &handle->stderr_cap,
                               &handle->stderr_truncated);
    }
}

static bool procx_kill_locked(ProcxHandle* handle) {
    if (!handle) return false;
    if (handle->finished) return true;
    if (!handle->process_handle) return false;
    if (!TerminateProcess(handle->process_handle, 137u)) {
        return false;
    }
    handle->killed = true;
    return true;
}
#else
static void procx_close_fd(int* fd) {
    if (!fd || *fd < 0) return;
    close(*fd);
    *fd = -1;
}

static bool procx_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return false;
    return true;
}

static void procx_pump_fd_locked(int* fd,
                                 char** buffer,
                                 size_t* length,
                                 size_t* capacity,
                                 bool* truncated) {
    if (!fd || *fd < 0) return;

    char temp[4096];
    for (;;) {
        ssize_t read_bytes = read(*fd, temp, sizeof(temp));
        if (read_bytes > 0) {
            procx_buffer_append(buffer, length, capacity, truncated, temp, (size_t)read_bytes);
            continue;
        }
        if (read_bytes == 0) {
            procx_close_fd(fd);
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        procx_close_fd(fd);
        break;
    }
}

static bool procx_poll_exit_locked(ProcxHandle* handle) {
    if (!handle) return false;
    if (handle->finished) return true;
    if (handle->pid <= 0) {
        handle->finished = true;
        if (handle->exit_code < 0) handle->exit_code = 1;
        return true;
    }

    int status = 0;
    pid_t result = waitpid(handle->pid, &status, WNOHANG);
    if (result == 0) return false;
    if (result < 0) {
        if (errno == ECHILD) {
            handle->finished = true;
            if (handle->exit_code < 0) handle->exit_code = 0;
            return true;
        }
        return false;
    }

    if (WIFEXITED(status)) {
        handle->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        handle->exit_code = 128 + WTERMSIG(status);
    } else {
        handle->exit_code = 1;
    }
    handle->finished = true;
    handle->pid = -1;
    return true;
}

static bool procx_spawn_process_locked(ProcxHandle* handle,
                                       char** argv,
                                       int argc,
                                       bool capture_stdout,
                                       bool capture_stderr,
                                       const char** out_err) {
    (void)argc;
    if (out_err) *out_err = NULL;
    if (!handle || !argv || !argv[0]) {
        if (out_err) *out_err = "Invalid process spawn arguments";
        return false;
    }

    int stdin_pipe[2] = { -1, -1 };
    int stdout_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };

    if (pipe(stdin_pipe) != 0) {
        if (out_err) *out_err = "Failed to create stdin pipe";
        return false;
    }
    if (capture_stdout && pipe(stdout_pipe) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        if (out_err) *out_err = "Failed to create stdout pipe";
        return false;
    }
    if (capture_stderr && pipe(stderr_pipe) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        if (capture_stdout) {
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);
        }
        if (out_err) *out_err = "Failed to create stderr pipe";
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        if (capture_stdout) {
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);
        }
        if (capture_stderr) {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }
        if (out_err) *out_err = "Failed to fork process";
        return false;
    }

    if (pid == 0) {
        (void)dup2(stdin_pipe[0], STDIN_FILENO);
        if (capture_stdout) (void)dup2(stdout_pipe[1], STDOUT_FILENO);
        if (capture_stderr) (void)dup2(stderr_pipe[1], STDERR_FILENO);

        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        if (capture_stdout) {
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);
        }
        if (capture_stderr) {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }

        execvp(argv[0], argv);
        _exit(127);
    }

    close(stdin_pipe[0]);
    if (capture_stdout) close(stdout_pipe[1]);
    if (capture_stderr) close(stderr_pipe[1]);

    handle->capture_stdout = capture_stdout;
    handle->capture_stderr = capture_stderr;
    handle->pid = pid;
    handle->stdin_fd = stdin_pipe[1];
    if (capture_stdout) {
        handle->stdout_fd = stdout_pipe[0];
        (void)procx_set_nonblocking(handle->stdout_fd);
    }
    if (capture_stderr) {
        handle->stderr_fd = stderr_pipe[0];
        (void)procx_set_nonblocking(handle->stderr_fd);
    }
    return true;
}

static void procx_close_stdin_locked(ProcxHandle* handle) {
    if (!handle) return;
    procx_close_fd(&handle->stdin_fd);
}

static void procx_pump_outputs_locked(ProcxHandle* handle) {
    if (!handle) return;
    if (handle->capture_stdout) {
        procx_pump_fd_locked(&handle->stdout_fd,
                             &handle->stdout_buf,
                             &handle->stdout_len,
                             &handle->stdout_cap,
                             &handle->stdout_truncated);
    }
    if (handle->capture_stderr) {
        procx_pump_fd_locked(&handle->stderr_fd,
                             &handle->stderr_buf,
                             &handle->stderr_len,
                             &handle->stderr_cap,
                             &handle->stderr_truncated);
    }
}

static bool procx_kill_locked(ProcxHandle* handle) {
    if (!handle) return false;
    if (handle->finished) return true;
    if (handle->pid <= 0) return false;
    if (kill(handle->pid, SIGKILL) != 0 && errno != ESRCH) {
        return false;
    }
    handle->killed = true;
    return true;
}
#endif

static bool procx_wait_locked(ProcxHandle* handle, int timeout_ms, bool* out_timed_out) {
    if (out_timed_out) *out_timed_out = false;
    if (!handle) return false;

    procx_pump_outputs_locked(handle);
    if (procx_poll_exit_locked(handle)) {
        procx_pump_outputs_locked(handle);
        return true;
    }

    if (timeout_ms <= 0) {
        if (out_timed_out) *out_timed_out = true;
        return true;
    }

    uint64_t deadline = syncx_now_ms() + (uint64_t)timeout_ms;
    while (!handle->finished) {
        uint64_t now = syncx_now_ms();
        if (now >= deadline) {
            if (out_timed_out) *out_timed_out = true;
            break;
        }

        int remaining = (int)(deadline - now);
        int sleep_ms = remaining < PROCX_WAIT_POLL_MS ? remaining : PROCX_WAIT_POLL_MS;
        if (sleep_ms < 1) sleep_ms = 1;
        procx_sleep_ms(sleep_ms);

        procx_pump_outputs_locked(handle);
        (void)procx_poll_exit_locked(handle);
    }

    procx_pump_outputs_locked(handle);
    return true;
}

static void procx_handle_dispose(ProcxHandle* handle) {
    if (!handle) return;

    syncx_mutex_lock(&handle->mutex);
    procx_close_stdin_locked(handle);
#ifdef _WIN32
    procx_close_handle(&handle->stdout_read);
    procx_close_handle(&handle->stderr_read);
    procx_close_handle(&handle->process_handle);
    procx_close_handle(&handle->thread_handle);
#else
    procx_close_fd(&handle->stdout_fd);
    procx_close_fd(&handle->stderr_fd);
    if (handle->pid > 0 && !handle->finished) {
        (void)kill(handle->pid, SIGKILL);
    }
#endif
    syncx_mutex_unlock(&handle->mutex);
    syncx_mutex_destroy(&handle->mutex);

    if (handle->stdout_buf) free(handle->stdout_buf);
    if (handle->stderr_buf) free(handle->stderr_buf);
    free(handle);
}

static SqlxDbHandle* sqlx_lookup_db_unlocked(int id) {
    for (int i = 0; i < g_sqlx_db_count; i++) {
        SqlxDbHandle* handle = g_sqlx_db_handles[i];
        if (handle && handle->id == id) return handle;
    }
    return NULL;
}

static SqlxStmtHandle* sqlx_lookup_stmt_unlocked(int id) {
    for (int i = 0; i < g_sqlx_stmt_count; i++) {
        SqlxStmtHandle* handle = g_sqlx_stmt_handles[i];
        if (handle && handle->id == id) return handle;
    }
    return NULL;
}

static bool sqlx_lookup_stmt_with_db(int stmt_id, SqlxStmtHandle** out_stmt, SqlxDbHandle** out_db) {
    if (out_stmt) *out_stmt = NULL;
    if (out_db) *out_db = NULL;

    syncx_mutex_lock(&g_syncx_registry_mutex);
    SqlxStmtHandle* stmt_handle = sqlx_lookup_stmt_unlocked(stmt_id);
    SqlxDbHandle* db_handle = stmt_handle ? sqlx_lookup_db_unlocked(stmt_handle->db_id) : NULL;
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!stmt_handle || !db_handle) {
        return false;
    }

    if (out_stmt) *out_stmt = stmt_handle;
    if (out_db) *out_db = db_handle;
    return true;
}

static void sqlx_append_db_unlocked(SqlxDbHandle* handle) {
    if (g_sqlx_db_count >= g_sqlx_db_capacity) {
        int next_capacity = g_sqlx_db_capacity <= 0 ? 8 : g_sqlx_db_capacity * 2;
        g_sqlx_db_handles = (SqlxDbHandle**)safe_realloc(g_sqlx_db_handles, (size_t)next_capacity * sizeof(SqlxDbHandle*));
        g_sqlx_db_capacity = next_capacity;
    }
    g_sqlx_db_handles[g_sqlx_db_count++] = handle;
}

static void sqlx_append_stmt_unlocked(SqlxStmtHandle* handle) {
    if (g_sqlx_stmt_count >= g_sqlx_stmt_capacity) {
        int next_capacity = g_sqlx_stmt_capacity <= 0 ? 8 : g_sqlx_stmt_capacity * 2;
        g_sqlx_stmt_handles = (SqlxStmtHandle**)safe_realloc(g_sqlx_stmt_handles, (size_t)next_capacity * sizeof(SqlxStmtHandle*));
        g_sqlx_stmt_capacity = next_capacity;
    }
    g_sqlx_stmt_handles[g_sqlx_stmt_count++] = handle;
}

static SqlxDbHandle* sqlx_db_handle_create(int id, SqlxNativeDb* db) {
    SqlxDbHandle* handle = (SqlxDbHandle*)safe_malloc(sizeof(SqlxDbHandle));
    memset(handle, 0, sizeof(*handle));
    handle->id = id;
    handle->db = db;
    handle->closed = false;
    syncx_mutex_init(&handle->mutex);
    return handle;
}

static SqlxStmtHandle* sqlx_stmt_handle_create(int id, int db_id, SqlxNativeStmt* stmt) {
    SqlxStmtHandle* handle = (SqlxStmtHandle*)safe_malloc(sizeof(SqlxStmtHandle));
    memset(handle, 0, sizeof(*handle));
    handle->id = id;
    handle->db_id = db_id;
    handle->stmt = stmt;
    handle->finalized = false;
    handle->done = false;
    return handle;
}

static void sqlx_stmt_finalize_unlocked(SqlxStmtHandle* stmt_handle) {
    if (!stmt_handle || stmt_handle->finalized) return;
    if (stmt_handle->stmt && g_sqlx_api.finalize) {
        (void)g_sqlx_api.finalize(stmt_handle->stmt);
    }
    stmt_handle->stmt = NULL;
    stmt_handle->finalized = true;
    stmt_handle->done = true;
}

static const char* sqlx_default_error_message(const char* fallback) {
    return (fallback && fallback[0] != '\0') ? fallback : "SQLite operation failed";
}

static const char* sqlx_db_error_message(SqlxNativeDb* db, const char* fallback) {
    if (g_sqlx_api.errmsg && db) {
        const char* msg = g_sqlx_api.errmsg(db);
        if (msg && msg[0] != '\0') return msg;
    }
    return sqlx_default_error_message(fallback);
}

#ifdef _WIN32
static HMODULE sqlx_load_library_system32(const char* dll_name) {
    if (!dll_name || dll_name[0] == '\0') return NULL;
    return LoadLibraryExA(dll_name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

static HMODULE sqlx_load_sqlite_library_windows(void) {
    HMODULE lib = sqlx_load_library_system32("winsqlite3.dll");
    if (!lib) {
        lib = sqlx_load_library_system32("sqlite3.dll");
    }
    if (lib) return lib;

    char exe_path[MAX_PATH];
    DWORD got = GetModuleFileNameA(NULL, exe_path, (DWORD)sizeof(exe_path));
    if (got == 0 || got >= (DWORD)sizeof(exe_path)) return NULL;

    char* exe_dir = path_dirname_alloc(exe_path);
    if (!exe_dir) return NULL;

    size_t dir_len = strlen(exe_dir);
    bool has_sep = dir_len > 0 && (exe_dir[dir_len - 1] == '\\' || exe_dir[dir_len - 1] == '/');
    const char* dll_file = "sqlite3.dll";
    size_t dll_len = strlen(dll_file);
    size_t full_len = dir_len + (has_sep ? 0 : 1) + dll_len;
    char* full_path = (char*)safe_malloc(full_len + 1);
    size_t pos = 0;
    memcpy(full_path + pos, exe_dir, dir_len);
    pos += dir_len;
    if (!has_sep) {
        full_path[pos++] = '\\';
    }
    memcpy(full_path + pos, dll_file, dll_len);
    pos += dll_len;
    full_path[pos] = '\0';

    lib = LoadLibraryExA(full_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    free(full_path);
    free(exe_dir);
    return lib;
}
#endif

static void sqlx_api_ensure_loaded(void) {
    if (g_sqlx_api.load_attempted) return;
    g_sqlx_api.load_attempted = true;
    g_sqlx_api.available = false;

#if defined(TABLO_SQLITE_STATIC_LINK)
#define SQLX_ASSIGN(name) g_sqlx_api.name = (sqlx_##name##_fn)sqlite3_##name
    SQLX_ASSIGN(open_v2);
    SQLX_ASSIGN(close_v2);
    SQLX_ASSIGN(exec);
    SQLX_ASSIGN(errmsg);
    g_sqlx_api.free_mem = (sqlx_free_fn)sqlite3_free;
    SQLX_ASSIGN(prepare_v2);
    SQLX_ASSIGN(bind_int64);
    SQLX_ASSIGN(bind_double);
    SQLX_ASSIGN(bind_text);
    SQLX_ASSIGN(bind_blob);
    SQLX_ASSIGN(bind_null);
    SQLX_ASSIGN(reset);
    SQLX_ASSIGN(clear_bindings);
    g_sqlx_api.changes = (sqlx_changes_fn)sqlite3_changes;
    SQLX_ASSIGN(last_insert_rowid);
    SQLX_ASSIGN(step);
    SQLX_ASSIGN(finalize);
    SQLX_ASSIGN(column_count);
    SQLX_ASSIGN(column_name);
    SQLX_ASSIGN(column_type);
    SQLX_ASSIGN(column_int64);
    SQLX_ASSIGN(column_double);
    SQLX_ASSIGN(column_text);
    SQLX_ASSIGN(column_blob);
    SQLX_ASSIGN(column_bytes);
#undef SQLX_ASSIGN

    if (!g_sqlx_api.open_v2 || !g_sqlx_api.close_v2 || !g_sqlx_api.exec ||
        !g_sqlx_api.errmsg || !g_sqlx_api.free_mem || !g_sqlx_api.prepare_v2 ||
        !g_sqlx_api.bind_int64 || !g_sqlx_api.bind_double || !g_sqlx_api.bind_text ||
        !g_sqlx_api.bind_blob || !g_sqlx_api.bind_null || !g_sqlx_api.reset ||
        !g_sqlx_api.clear_bindings || !g_sqlx_api.changes || !g_sqlx_api.last_insert_rowid ||
        !g_sqlx_api.step || !g_sqlx_api.finalize || !g_sqlx_api.column_count ||
        !g_sqlx_api.column_name || !g_sqlx_api.column_type || !g_sqlx_api.column_int64 ||
        !g_sqlx_api.column_double || !g_sqlx_api.column_text ||
        !g_sqlx_api.column_blob || !g_sqlx_api.column_bytes) {
        return;
    }

    g_sqlx_api.available = true;
#elif defined(_WIN32)
    HMODULE lib = sqlx_load_sqlite_library_windows();
    if (!lib) return;

    g_sqlx_api.lib_handle = lib;
#define SQLX_LOAD(name) g_sqlx_api.name = (sqlx_##name##_fn)GetProcAddress(lib, "sqlite3_" #name)
    SQLX_LOAD(open_v2);
    SQLX_LOAD(close_v2);
    SQLX_LOAD(exec);
    SQLX_LOAD(errmsg);
    g_sqlx_api.free_mem = (sqlx_free_fn)GetProcAddress(lib, "sqlite3_free");
    SQLX_LOAD(prepare_v2);
    SQLX_LOAD(bind_int64);
    SQLX_LOAD(bind_double);
    SQLX_LOAD(bind_text);
    SQLX_LOAD(bind_blob);
    SQLX_LOAD(bind_null);
    SQLX_LOAD(reset);
    SQLX_LOAD(clear_bindings);
    g_sqlx_api.changes = (sqlx_changes_fn)GetProcAddress(lib, "sqlite3_changes");
    SQLX_LOAD(last_insert_rowid);
    SQLX_LOAD(step);
    SQLX_LOAD(finalize);
    SQLX_LOAD(column_count);
    SQLX_LOAD(column_name);
    SQLX_LOAD(column_type);
    SQLX_LOAD(column_int64);
    SQLX_LOAD(column_double);
    SQLX_LOAD(column_text);
    SQLX_LOAD(column_blob);
    SQLX_LOAD(column_bytes);
#undef SQLX_LOAD

    if (!g_sqlx_api.open_v2 || !g_sqlx_api.close_v2 || !g_sqlx_api.exec ||
        !g_sqlx_api.errmsg || !g_sqlx_api.free_mem || !g_sqlx_api.prepare_v2 ||
        !g_sqlx_api.bind_int64 || !g_sqlx_api.bind_double || !g_sqlx_api.bind_text ||
        !g_sqlx_api.bind_blob || !g_sqlx_api.bind_null || !g_sqlx_api.reset ||
        !g_sqlx_api.clear_bindings || !g_sqlx_api.changes || !g_sqlx_api.last_insert_rowid ||
        !g_sqlx_api.step || !g_sqlx_api.finalize || !g_sqlx_api.column_count ||
        !g_sqlx_api.column_name || !g_sqlx_api.column_type || !g_sqlx_api.column_int64 ||
        !g_sqlx_api.column_double || !g_sqlx_api.column_text ||
        !g_sqlx_api.column_blob || !g_sqlx_api.column_bytes) {
        FreeLibrary(lib);
        g_sqlx_api.lib_handle = NULL;
        return;
    }

    g_sqlx_api.available = true;
#else
    void* lib = NULL;
    const char* candidates[] = {
        "libsqlite3.so.0",
        "libsqlite3.so",
        "libsqlite3.dylib",
        "sqlite3.dylib",
        "sqlite3.so",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        lib = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (lib) break;
    }
    if (!lib) return;

    g_sqlx_api.lib_handle = lib;
#define SQLX_LOAD(name) g_sqlx_api.name = (sqlx_##name##_fn)dlsym(lib, "sqlite3_" #name)
    SQLX_LOAD(open_v2);
    SQLX_LOAD(close_v2);
    SQLX_LOAD(exec);
    SQLX_LOAD(errmsg);
    g_sqlx_api.free_mem = (sqlx_free_fn)dlsym(lib, "sqlite3_free");
    SQLX_LOAD(prepare_v2);
    SQLX_LOAD(bind_int64);
    SQLX_LOAD(bind_double);
    SQLX_LOAD(bind_text);
    SQLX_LOAD(bind_blob);
    SQLX_LOAD(bind_null);
    SQLX_LOAD(reset);
    SQLX_LOAD(clear_bindings);
    g_sqlx_api.changes = (sqlx_changes_fn)dlsym(lib, "sqlite3_changes");
    SQLX_LOAD(last_insert_rowid);
    SQLX_LOAD(step);
    SQLX_LOAD(finalize);
    SQLX_LOAD(column_count);
    SQLX_LOAD(column_name);
    SQLX_LOAD(column_type);
    SQLX_LOAD(column_int64);
    SQLX_LOAD(column_double);
    SQLX_LOAD(column_text);
    SQLX_LOAD(column_blob);
    SQLX_LOAD(column_bytes);
#undef SQLX_LOAD

    if (!g_sqlx_api.open_v2 || !g_sqlx_api.close_v2 || !g_sqlx_api.exec ||
        !g_sqlx_api.errmsg || !g_sqlx_api.free_mem || !g_sqlx_api.prepare_v2 ||
        !g_sqlx_api.bind_int64 || !g_sqlx_api.bind_double || !g_sqlx_api.bind_text ||
        !g_sqlx_api.bind_blob || !g_sqlx_api.bind_null || !g_sqlx_api.reset ||
        !g_sqlx_api.clear_bindings || !g_sqlx_api.changes || !g_sqlx_api.last_insert_rowid ||
        !g_sqlx_api.step || !g_sqlx_api.finalize || !g_sqlx_api.column_count ||
        !g_sqlx_api.column_name || !g_sqlx_api.column_type || !g_sqlx_api.column_int64 ||
        !g_sqlx_api.column_double || !g_sqlx_api.column_text ||
        !g_sqlx_api.column_blob || !g_sqlx_api.column_bytes) {
        dlclose(lib);
        g_sqlx_api.lib_handle = NULL;
        return;
    }

    g_sqlx_api.available = true;
#endif
}

static bool sqlx_column_to_value(VM* vm,
                                 SqlxNativeStmt* stmt,
                                 int col,
                                 Value* out,
                                 const char** out_err) {
    if (out_err) *out_err = NULL;
    if (!vm || !stmt || !out) {
        if (out_err) *out_err = "Invalid sqlite row value";
        return false;
    }

    value_init_nil(out);
    int col_type = g_sqlx_api.column_type(stmt, col);
    if (col_type == SQLX_NULL) {
        value_init_nil(out);
        return true;
    }

    if (col_type == SQLX_INTEGER) {
        value_init_int(out, (int64_t)g_sqlx_api.column_int64(stmt, col));
        return true;
    }

    if (col_type == SQLX_FLOAT) {
        value_init_double(out, g_sqlx_api.column_double(stmt, col));
        return true;
    }

    if (col_type == SQLX_TEXT) {
        const unsigned char* text_ptr = g_sqlx_api.column_text(stmt, col);
        int text_len = g_sqlx_api.column_bytes(stmt, col);
        if (!text_ptr || text_len <= 0) {
            value_init_string(out, "");
            return true;
        }
        char* text = (char*)safe_malloc((size_t)text_len + 1);
        memcpy(text, text_ptr, (size_t)text_len);
        text[text_len] = '\0';
        value_init_string(out, text);
        free(text);
        return true;
    }

    if (col_type == SQLX_BLOB) {
        const void* blob_ptr = g_sqlx_api.column_blob(stmt, col);
        int blob_len = g_sqlx_api.column_bytes(stmt, col);
        if (!blob_ptr || blob_len <= 0) {
            ObjBytes* empty = obj_bytes_create_with_size(0, 0);
            if (!empty) {
                if (out_err) *out_err = "Failed to allocate bytes value";
                return false;
            }
            value_init_bytes(out, empty);
            return true;
        }

        int max_array_size = (vm->config.max_array_size > 0) ? vm->config.max_array_size : INT_MAX;
        if (blob_len > max_array_size) {
            if (out_err) *out_err = "SQLite blob exceeds max array size";
            return false;
        }

        ObjBytes* bytes = obj_bytes_create_copy((const uint8_t*)blob_ptr, blob_len);
        if (!bytes) {
            if (out_err) *out_err = "Failed to allocate bytes value";
            return false;
        }
        value_init_bytes(out, bytes);
        return true;
    }

    value_init_nil(out);
    return true;
}

static bool sqlx_stmt_row_to_map(VM* vm,
                                 SqlxNativeStmt* stmt,
                                 Value* out,
                                 const char** out_err) {
    if (out_err) *out_err = NULL;
    if (!vm || !stmt || !out) {
        if (out_err) *out_err = "Invalid sqlite row";
        return false;
    }

    int col_count = g_sqlx_api.column_count(stmt);
    if (col_count < 0) col_count = 0;
    ObjMap* row = obj_map_create(vm);
    if (!row) {
        if (out_err) *out_err = "Failed to allocate sqlite row map";
        return false;
    }

    for (int col = 0; col < col_count; col++) {
        const char* col_name = g_sqlx_api.column_name(stmt, col);
        char fallback_name[32];
        if (!col_name || col_name[0] == '\0') {
            snprintf(fallback_name, sizeof(fallback_name), "col%d", col);
            col_name = fallback_name;
        }

        Value val;
        if (!sqlx_column_to_value(vm, stmt, col, &val, out_err)) {
            return false;
        }
        obj_map_set_cstr(row, col_name, val);
        value_free(&val);
    }

    value_init_map(out, row);
    return true;
}

static void syncx_runtime_init_impl(void) {
    syncx_mutex_init(&g_syncx_registry_mutex);
}

#ifdef _WIN32
static INIT_ONCE g_syncx_init_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK syncx_runtime_init_once_win(PINIT_ONCE init_once, PVOID parameter, PVOID* context) {
    (void)init_once;
    (void)parameter;
    (void)context;
    syncx_runtime_init_impl();
    return TRUE;
}
#else
static pthread_once_t g_syncx_init_once = PTHREAD_ONCE_INIT;
#endif

static void syncx_runtime_ensure_init(void) {
#ifdef _WIN32
    InitOnceExecuteOnce(&g_syncx_init_once, syncx_runtime_init_once_win, NULL, NULL);
#else
    pthread_once(&g_syncx_init_once, syncx_runtime_init_impl);
#endif
}

static void syncx_value_init_nil(SyncxValue* v) {
    if (!v) return;
    v->kind = SYNCX_VALUE_NIL;
    v->as_int = 0;
    v->as_bool = false;
    v->as_double = 0.0;
    v->as_string = NULL;
    v->as_bytes = NULL;
    v->as_bytes_len = 0;
}

static void syncx_value_free(SyncxValue* v) {
    if (!v) return;
    if (v->as_string) {
        free(v->as_string);
        v->as_string = NULL;
    }
    if (v->as_bytes) {
        free(v->as_bytes);
        v->as_bytes = NULL;
    }
    syncx_value_init_nil(v);
}

static bool syncx_value_clone(SyncxValue* dst, const SyncxValue* src) {
    if (!dst || !src) return false;
    syncx_value_init_nil(dst);
    dst->kind = src->kind;
    dst->as_int = src->as_int;
    dst->as_bool = src->as_bool;
    dst->as_double = src->as_double;
    dst->as_bytes_len = src->as_bytes_len;
    if (src->kind == SYNCX_VALUE_STRING || src->kind == SYNCX_VALUE_JSON) {
        dst->as_string = safe_strdup(src->as_string ? src->as_string : "");
    } else if (src->kind == SYNCX_VALUE_BYTES) {
        if (src->as_bytes_len > 0) {
            dst->as_bytes = (uint8_t*)safe_malloc((size_t)src->as_bytes_len);
            memcpy(dst->as_bytes, src->as_bytes, (size_t)src->as_bytes_len);
        }
    }
    return true;
}

static bool syncx_value_from_vm_value(const Value* input, SyncxValue* out, const char** err_msg) {
    if (err_msg) *err_msg = NULL;
    if (!input || !out) return false;

    syncx_value_init_nil(out);

    switch (value_get_type(input)) {
        case VAL_NIL:
            out->kind = SYNCX_VALUE_NIL;
            return true;
        case VAL_INT:
            out->kind = SYNCX_VALUE_INT;
            out->as_int = value_get_int(input);
            return true;
        case VAL_BOOL:
            out->kind = SYNCX_VALUE_BOOL;
            out->as_bool = value_get_bool(input);
            return true;
        case VAL_DOUBLE:
            out->kind = SYNCX_VALUE_DOUBLE;
            out->as_double = value_get_double(input);
            return true;
        case VAL_STRING: {
            out->kind = SYNCX_VALUE_STRING;
            ObjString* str = value_get_string_obj(input);
            out->as_string = safe_strdup((str && str->chars) ? str->chars : "");
            return true;
        }
        case VAL_BYTES: {
            out->kind = SYNCX_VALUE_BYTES;
            ObjBytes* bytes = value_get_bytes_obj(input);
            int len = (bytes ? bytes->length : 0);
            if (len < 0) len = 0;
            out->as_bytes_len = len;
            if (len > 0) {
                uint8_t* src = obj_bytes_data(bytes);
                if (!src) {
                    if (err_msg) *err_msg = "Invalid bytes payload";
                    return false;
                }
                out->as_bytes = (uint8_t*)safe_malloc((size_t)len);
                memcpy(out->as_bytes, src, (size_t)len);
            }
            return true;
        }
        default:
            if (err_msg) *err_msg = "Only nil/int/bool/double/string/bytes are shareable across threads";
            return false;
    }
}

static bool syncx_value_to_vm_value(VM* vm, const SyncxValue* value, Value* out) {
    if (!vm || !value || !out) return false;

    switch (value->kind) {
        case SYNCX_VALUE_NIL:
            value_init_nil(out);
            return true;
        case SYNCX_VALUE_INT:
            value_init_int(out, value->as_int);
            return true;
        case SYNCX_VALUE_BOOL:
            value_init_bool(out, value->as_bool);
            return true;
        case SYNCX_VALUE_DOUBLE:
            value_init_double(out, value->as_double);
            return true;
        case SYNCX_VALUE_STRING:
            value_init_string(out, value->as_string ? value->as_string : "");
            return true;
        case SYNCX_VALUE_BYTES: {
            ObjBytes* bytes = obj_bytes_create_copy(value->as_bytes, value->as_bytes_len);
            value_init_bytes(out, bytes);
            return true;
        }
        case SYNCX_VALUE_JSON:
            return false;
        default:
            return false;
    }
}

static bool syncx_value_encode_lossless(VM* vm,
                                        const Value* value,
                                        SyncxValue* out,
                                        int64_t* err_code,
                                        char* err_msg,
                                        size_t err_msg_size) {
    if (err_code) *err_code = 0;
    if (err_msg && err_msg_size > 0) err_msg[0] = '\0';
    if (!value || !out) return false;

    const char* primitive_err = NULL;
    if (syncx_value_from_vm_value(value, out, &primitive_err)) {
        return true;
    }

    char json_err[256] = {0};
    cJSON* root = json_to_cjson(vm, value, 0, json_err, sizeof(json_err));
    if (!root) {
        if (err_code) *err_code = ERR_INVALID_ARGUMENT;
        if (err_msg && err_msg_size > 0) {
            snprintf(err_msg,
                     err_msg_size,
                     "%s",
                     json_err[0] ? json_err : (primitive_err ? primitive_err : "Value is not thread-shareable"));
        }
        return false;
    }

    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        if (err_code) *err_code = ERR_INTERNAL;
        if (err_msg && err_msg_size > 0) {
            snprintf(err_msg, err_msg_size, "Failed to encode thread payload");
        }
        return false;
    }

    syncx_value_init_nil(out);
    out->kind = SYNCX_VALUE_JSON;
    out->as_string = safe_strdup(rendered);
    cJSON_free(rendered);
    return true;
}

static bool syncx_schema_is_any(const Value* schema) {
    ObjString* schema_str = schema ? value_get_string_obj(schema) : NULL;
    if (!schema || value_get_type(schema) != VAL_STRING || !schema_str || !schema_str->chars) {
        return false;
    }
    return strcmp(schema_str->chars, "any") == 0;
}

static bool syncx_value_decode_lossless(VM* vm,
                                        const SyncxValue* in,
                                        Value* out,
                                        int64_t* err_code,
                                        char* err_msg,
                                        size_t err_msg_size);

static bool syncx_value_encode_typed(VM* vm,
                                     const Value* value,
                                     const Value* schema,
                                     SyncxValue* out,
                                     int64_t* err_code,
                                     char* err_msg,
                                     size_t err_msg_size) {
    if (err_code) *err_code = 0;
    if (err_msg && err_msg_size > 0) err_msg[0] = '\0';
    if (!vm || !value || !schema || !out) return false;

    if (syncx_schema_is_any(schema)) {
        return syncx_value_encode_lossless(vm, value, out, err_code, err_msg, err_msg_size);
    }

    Value validated;
    value_init_nil(&validated);
    Value err_data;
    value_init_nil(&err_data);
    int64_t decode_err_code = 0;
    char decode_err_msg[512] = {0};
    bool ok = json_decode_value_with_schema(vm,
                                            value,
                                            schema,
                                            "$",
                                            "$",
                                            0,
                                            &validated,
                                            &decode_err_code,
                                            decode_err_msg,
                                            sizeof(decode_err_msg),
                                            &err_data);
    value_free(&err_data);
    if (!ok) {
        if (err_code) *err_code = decode_err_code ? decode_err_code : ERR_INVALID_ARGUMENT;
        if (err_msg && err_msg_size > 0) {
            snprintf(err_msg,
                     err_msg_size,
                     "%s",
                     decode_err_msg[0] ? decode_err_msg : "Value does not match schema");
        }
        return false;
    }

    char json_err[256] = {0};
    cJSON* root = json_to_cjson(vm, &validated, 0, json_err, sizeof(json_err));
    value_free(&validated);
    if (!root) {
        if (err_code) *err_code = ERR_INVALID_ARGUMENT;
        if (err_msg && err_msg_size > 0) {
            snprintf(err_msg, err_msg_size, "%s", json_err[0] ? json_err : "Failed to encode typed payload");
        }
        return false;
    }

    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        if (err_code) *err_code = ERR_INTERNAL;
        if (err_msg && err_msg_size > 0) {
            snprintf(err_msg, err_msg_size, "Failed to encode typed payload");
        }
        return false;
    }

    syncx_value_init_nil(out);
    out->kind = SYNCX_VALUE_JSON;
    out->as_string = safe_strdup(rendered);
    cJSON_free(rendered);
    return true;
}

static bool syncx_value_decode_typed(VM* vm,
                                     const SyncxValue* in,
                                     const Value* schema,
                                     Value* out,
                                     int64_t* err_code,
                                     char* err_msg,
                                     size_t err_msg_size) {
    if (err_code) *err_code = 0;
    if (err_msg && err_msg_size > 0) err_msg[0] = '\0';
    if (!vm || !in || !schema || !out) return false;

    if (syncx_schema_is_any(schema)) {
        return syncx_value_decode_lossless(vm, in, out, err_code, err_msg, err_msg_size);
    }

    Value raw;
    value_init_nil(&raw);

    if (in->kind == SYNCX_VALUE_JSON) {
        cJSON* root = cJSON_Parse(in->as_string ? in->as_string : "");
        if (!root) {
            if (err_code) *err_code = ERR_PARSE;
            if (err_msg && err_msg_size > 0) {
                snprintf(err_msg, err_msg_size, "Failed to parse typed payload JSON");
            }
            return false;
        }
        if (!json_from_cjson(vm, root, &raw, 0)) {
            cJSON_Delete(root);
            if (err_code) *err_code = ERR_PARSE;
            if (err_msg && err_msg_size > 0) {
                snprintf(err_msg, err_msg_size, "Failed to decode typed payload JSON");
            }
            return false;
        }
        cJSON_Delete(root);
    } else {
        if (!syncx_value_to_vm_value(vm, in, &raw)) {
            if (err_code) *err_code = ERR_INVALID_ARGUMENT;
            if (err_msg && err_msg_size > 0) {
                snprintf(err_msg, err_msg_size, "Payload is not compatible with typed decode");
            }
            return false;
        }
    }

    Value err_data;
    value_init_nil(&err_data);
    bool ok = json_decode_value_with_schema(vm,
                                            &raw,
                                            schema,
                                            "$",
                                            "$",
                                            0,
                                            out,
                                            err_code,
                                            err_msg,
                                            err_msg_size,
                                            &err_data);
    value_free(&raw);
    value_free(&err_data);
    return ok;
}

static bool syncx_value_decode_lossless(VM* vm,
                                        const SyncxValue* in,
                                        Value* out,
                                        int64_t* err_code,
                                        char* err_msg,
                                        size_t err_msg_size) {
    if (err_code) *err_code = 0;
    if (err_msg && err_msg_size > 0) err_msg[0] = '\0';
    if (!vm || !in || !out) return false;

    if (in->kind == SYNCX_VALUE_JSON) {
        cJSON* root = cJSON_Parse(in->as_string ? in->as_string : "");
        if (!root) {
            if (err_code) *err_code = ERR_PARSE;
            if (err_msg && err_msg_size > 0) {
                snprintf(err_msg, err_msg_size, "Failed to parse JSON payload");
            }
            return false;
        }

        bool ok = json_from_cjson(vm, root, out, 0);
        cJSON_Delete(root);
        if (!ok) {
            if (err_code) *err_code = ERR_PARSE;
            if (err_msg && err_msg_size > 0) {
                snprintf(err_msg, err_msg_size, "Failed to decode JSON payload");
            }
            return false;
        }
        return true;
    }

    if (!syncx_value_to_vm_value(vm, in, out)) {
        if (err_code) *err_code = ERR_INVALID_ARGUMENT;
        if (err_msg && err_msg_size > 0) {
            snprintf(err_msg, err_msg_size, "Payload is not thread-shareable");
        }
        return false;
    }
    return true;
}

static SyncxChannel* syncx_channel_lookup_unlocked(int id) {
    for (int i = 0; i < g_syncx_channel_count; i++) {
        SyncxChannel* ch = g_syncx_channels[i];
        if (ch && ch->id == id) return ch;
    }
    return NULL;
}

static SyncxSharedCell* syncx_shared_cell_lookup_unlocked(int id) {
    for (int i = 0; i < g_syncx_shared_cell_count; i++) {
        SyncxSharedCell* cell = g_syncx_shared_cells[i];
        if (cell && cell->id == id) return cell;
    }
    return NULL;
}

static SyncxThreadHandle* syncx_thread_lookup_unlocked(int id) {
    for (int i = 0; i < g_syncx_thread_count; i++) {
        SyncxThreadHandle* handle = g_syncx_threads[i];
        if (handle && handle->id == id) return handle;
    }
    return NULL;
}

static SyncxArcGuard* syncx_guard_lookup_unlocked(int id) {
    for (int i = 0; i < g_syncx_guard_count; i++) {
        SyncxArcGuard* guard = g_syncx_guards[i];
        if (guard && guard->id == id) return guard;
    }
    return NULL;
}

static int syncx_guard_find_index_unlocked(int id) {
    for (int i = 0; i < g_syncx_guard_count; i++) {
        SyncxArcGuard* guard = g_syncx_guards[i];
        if (guard && guard->id == id) return i;
    }
    return -1;
}

static void syncx_append_channel_unlocked(SyncxChannel* ch) {
    if (g_syncx_channel_count >= g_syncx_channel_capacity) {
        int next_capacity = g_syncx_channel_capacity <= 0 ? 8 : g_syncx_channel_capacity * 2;
        g_syncx_channels = (SyncxChannel**)safe_realloc(g_syncx_channels, (size_t)next_capacity * sizeof(SyncxChannel*));
        g_syncx_channel_capacity = next_capacity;
    }
    g_syncx_channels[g_syncx_channel_count++] = ch;
}

static void syncx_append_shared_cell_unlocked(SyncxSharedCell* cell) {
    if (g_syncx_shared_cell_count >= g_syncx_shared_cell_capacity) {
        int next_capacity = g_syncx_shared_cell_capacity <= 0 ? 8 : g_syncx_shared_cell_capacity * 2;
        g_syncx_shared_cells = (SyncxSharedCell**)safe_realloc(g_syncx_shared_cells, (size_t)next_capacity * sizeof(SyncxSharedCell*));
        g_syncx_shared_cell_capacity = next_capacity;
    }
    g_syncx_shared_cells[g_syncx_shared_cell_count++] = cell;
}

static void syncx_append_thread_unlocked(SyncxThreadHandle* handle) {
    if (g_syncx_thread_count >= g_syncx_thread_capacity) {
        int next_capacity = g_syncx_thread_capacity <= 0 ? 8 : g_syncx_thread_capacity * 2;
        g_syncx_threads = (SyncxThreadHandle**)safe_realloc(g_syncx_threads, (size_t)next_capacity * sizeof(SyncxThreadHandle*));
        g_syncx_thread_capacity = next_capacity;
    }
    g_syncx_threads[g_syncx_thread_count++] = handle;
}

static void syncx_append_guard_unlocked(SyncxArcGuard* guard) {
    if (g_syncx_guard_count >= g_syncx_guard_capacity) {
        int next_capacity = g_syncx_guard_capacity <= 0 ? 8 : g_syncx_guard_capacity * 2;
        g_syncx_guards = (SyncxArcGuard**)safe_realloc(g_syncx_guards, (size_t)next_capacity * sizeof(SyncxArcGuard*));
        g_syncx_guard_capacity = next_capacity;
    }
    g_syncx_guards[g_syncx_guard_count++] = guard;
}

static SyncxChannel* syncx_channel_create(int id, int capacity) {
    SyncxChannel* ch = (SyncxChannel*)safe_malloc(sizeof(SyncxChannel));
    ch->id = id;
    ch->capacity = capacity;
    ch->count = 0;
    ch->closed = false;
    ch->head = NULL;
    ch->tail = NULL;
    syncx_mutex_init(&ch->mutex);
    syncx_cond_init(&ch->not_empty);
    syncx_cond_init(&ch->not_full);
    return ch;
}

static SyncxSharedCell* syncx_shared_cell_create(int id, const SyncxValue* initial) {
    SyncxSharedCell* cell = (SyncxSharedCell*)safe_malloc(sizeof(SyncxSharedCell));
    cell->id = id;
    syncx_value_init_nil(&cell->value);
    syncx_value_clone(&cell->value, initial);
    syncx_mutex_init(&cell->mutex);
    return cell;
}

static SyncxThreadHandle* syncx_thread_handle_create(int id) {
    SyncxThreadHandle* handle = (SyncxThreadHandle*)safe_malloc(sizeof(SyncxThreadHandle));
    handle->id = id;
    handle->started = false;
    handle->joined = false;
    handle->finished = false;
    handle->has_result_payload = false;
    syncx_value_init_nil(&handle->result_payload);
    handle->exit_code = 0;
    handle->error_code = 0;
    handle->error_message = NULL;
#ifdef _WIN32
    handle->os_thread = NULL;
#endif
    syncx_mutex_init(&handle->mutex);
    syncx_cond_init(&handle->finished_cond);
    return handle;
}

static SyncxArcGuard* syncx_arc_guard_create(int id, SyncxSharedCell* cell) {
    SyncxArcGuard* guard = (SyncxArcGuard*)safe_malloc(sizeof(SyncxArcGuard));
    guard->id = id;
    guard->cell = cell;
    guard->active = true;
    guard->owner_thread_id = syncx_current_thread_id();
    return guard;
}

static void syncx_thread_task_free(SyncxThreadTask* task) {
    if (!task) return;
    if (task->file_path) free(task->file_path);
    if (task->function_name) free(task->function_name);
    if (task->has_arg_payload) {
        syncx_value_free(&task->arg_payload);
    }
    free(task);
}

static bool syncx_channel_send_internal(SyncxChannel* ch,
                                        const SyncxValue* value,
                                        int timeout_ms,
                                        int64_t* err_code,
                                        const char** err_msg) {
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!ch || !value) return false;

    uint64_t deadline = timeout_ms > 0 ? (syncx_now_ms() + (uint64_t)timeout_ms) : 0;
    syncx_mutex_lock(&ch->mutex);
    while (ch->count >= ch->capacity && !ch->closed) {
        if (timeout_ms <= 0) {
            syncx_mutex_unlock(&ch->mutex);
            if (err_code) *err_code = ERR_LIMIT;
            if (err_msg) *err_msg = "Channel send timed out";
            return false;
        }

        uint64_t now = syncx_now_ms();
        if (now >= deadline) {
            syncx_mutex_unlock(&ch->mutex);
            if (err_code) *err_code = ERR_LIMIT;
            if (err_msg) *err_msg = "Channel send timed out";
            return false;
        }

        int remaining = (int)(deadline - now);
        if (remaining < 1) remaining = 1;
        (void)syncx_cond_wait_ms(&ch->not_full, &ch->mutex, remaining);
    }

    if (ch->closed) {
        syncx_mutex_unlock(&ch->mutex);
        if (err_code) *err_code = ERR_IO;
        if (err_msg) *err_msg = "Channel is closed";
        return false;
    }

    SyncxMessageNode* node = (SyncxMessageNode*)safe_malloc(sizeof(SyncxMessageNode));
    syncx_value_init_nil(&node->value);
    syncx_value_clone(&node->value, value);
    node->next = NULL;

    if (ch->tail) {
        ch->tail->next = node;
    } else {
        ch->head = node;
    }
    ch->tail = node;
    ch->count++;
    syncx_cond_signal(&ch->not_empty);
    syncx_mutex_unlock(&ch->mutex);
    return true;
}

static bool syncx_channel_recv_internal(SyncxChannel* ch,
                                        int timeout_ms,
                                        SyncxValue* out,
                                        int64_t* err_code,
                                        const char** err_msg) {
    if (out) syncx_value_init_nil(out);
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!ch || !out) return false;

    uint64_t deadline = timeout_ms > 0 ? (syncx_now_ms() + (uint64_t)timeout_ms) : 0;
    syncx_mutex_lock(&ch->mutex);
    while (ch->count == 0 && !ch->closed) {
        if (timeout_ms <= 0) {
            syncx_mutex_unlock(&ch->mutex);
            if (err_code) *err_code = ERR_LIMIT;
            if (err_msg) *err_msg = "Channel receive timed out";
            return false;
        }

        uint64_t now = syncx_now_ms();
        if (now >= deadline) {
            syncx_mutex_unlock(&ch->mutex);
            if (err_code) *err_code = ERR_LIMIT;
            if (err_msg) *err_msg = "Channel receive timed out";
            return false;
        }

        int remaining = (int)(deadline - now);
        if (remaining < 1) remaining = 1;
        (void)syncx_cond_wait_ms(&ch->not_empty, &ch->mutex, remaining);
    }

    if (ch->count == 0 && ch->closed) {
        syncx_mutex_unlock(&ch->mutex);
        if (err_code) *err_code = ERR_IO;
        if (err_msg) *err_msg = "Channel is closed";
        return false;
    }

    SyncxMessageNode* node = ch->head;
    ch->head = node->next;
    if (!ch->head) ch->tail = NULL;
    ch->count--;
    syncx_cond_signal(&ch->not_full);
    syncx_mutex_unlock(&ch->mutex);

    syncx_value_clone(out, &node->value);
    syncx_value_free(&node->value);
    free(node);
    return true;
}

static bool syncx_thread_wait_finished(SyncxThreadHandle* handle, int timeout_ms) {
    if (!handle) return false;
    uint64_t deadline = timeout_ms > 0 ? (syncx_now_ms() + (uint64_t)timeout_ms) : 0;

    while (!handle->finished) {
        if (timeout_ms <= 0) {
            return false;
        }

        uint64_t now = syncx_now_ms();
        if (now >= deadline) {
            return false;
        }

        int remaining = (int)(deadline - now);
        if (remaining < 1) remaining = 1;
        (void)syncx_cond_wait_ms(&handle->finished_cond, &handle->mutex, remaining);
    }
    return true;
}

#ifdef _WIN32
static DWORD WINAPI syncx_thread_entry(void* param)
#else
static void* syncx_thread_entry(void* param)
#endif
{
    SyncxThreadTask* task = (SyncxThreadTask*)param;
    SyncxThreadHandle* handle = task ? task->handle : NULL;

    int exit_code = 0;
    int64_t err_code = 0;
    char* err_message = NULL;
    SyncxValue return_payload;
    syncx_value_init_nil(&return_payload);
    bool has_return_payload = false;

    g_syncx_tls_inbox = task ? task->inbox_channel_id : -1;
    g_syncx_tls_outbox = task ? task->outbox_channel_id : -1;
    g_syncx_tls_has_arg_payload = false;
    syncx_value_free(&g_syncx_tls_arg_payload);
    if (task && task->has_arg_payload) {
        syncx_value_clone(&g_syncx_tls_arg_payload, &task->arg_payload);
        g_syncx_tls_has_arg_payload = true;
    }

    if (!task || !handle || !task->file_path || !task->function_name) {
        exit_code = -1;
        err_code = ERR_INTERNAL;
        err_message = safe_strdup("Invalid thread task");
    } else {
        Runtime* rt = runtime_create(task->file_path);
        if (!rt) {
            exit_code = -1;
            err_code = ERR_INTERNAL;
            err_message = safe_strdup("Failed to create thread runtime");
        } else {
            runtime_set_thread_channels(rt, task->inbox_channel_id, task->outbox_channel_id);

            int rc = runtime_run_function(rt, task->function_name);
            if (rc != 0 || runtime_has_error(rt)) {
                exit_code = rc != 0 ? rc : -1;
                err_code = ERR_INTERNAL;
                const char* runtime_err = runtime_get_error(rt);
                err_message = safe_strdup(runtime_err ? runtime_err : "Thread function failed");
            } else {
                Value ret_val;
                value_init_nil(&ret_val);
                if (runtime_take_return_value(rt, &ret_val)) {
                    char encode_err_msg[512];
                    int64_t encode_err_code = 0;
                    if (!syncx_value_encode_lossless(NULL,
                                                     &ret_val,
                                                     &return_payload,
                                                     &encode_err_code,
                                                     encode_err_msg,
                                                     sizeof(encode_err_msg))) {
                        exit_code = -1;
                        err_code = encode_err_code ? encode_err_code : ERR_INVALID_ARGUMENT;
                        err_message = safe_strdup(encode_err_msg[0] ? encode_err_msg : "Thread return value is not shareable");
                    } else {
                        has_return_payload = true;
                    }
                    value_free(&ret_val);
                } else {
                    syncx_value_init_nil(&return_payload);
                    has_return_payload = true;
                }
            }
            runtime_free(rt);
        }
    }

    if (handle) {
        syncx_mutex_lock(&handle->mutex);
        if (handle->has_result_payload) {
            syncx_value_free(&handle->result_payload);
            handle->has_result_payload = false;
        }
        if (has_return_payload) {
            syncx_value_clone(&handle->result_payload, &return_payload);
            handle->has_result_payload = true;
        }
        handle->finished = true;
        handle->exit_code = exit_code;
        handle->error_code = err_code;
        if (handle->error_message) {
            free(handle->error_message);
            handle->error_message = NULL;
        }
        handle->error_message = err_message;
        syncx_cond_broadcast(&handle->finished_cond);
        syncx_mutex_unlock(&handle->mutex);
    } else if (err_message) {
        free(err_message);
    }

    syncx_thread_task_free(task);
    g_syncx_tls_inbox = -1;
    g_syncx_tls_outbox = -1;
    if (g_syncx_tls_has_arg_payload) {
        syncx_value_free(&g_syncx_tls_arg_payload);
        g_syncx_tls_has_arg_payload = false;
    }
    syncx_value_free(&return_payload);

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void error_value_init(VM* vm, Value* out, int64_t code, const char* message, Value data) {
    ObjRecord* rec = obj_record_create_with_count(vm, 3);

    Value code_val;
    value_init_int(&code_val, code);
    obj_record_set_field(rec, 0, code_val);

    Value message_val;
    value_init_string(&message_val, message ? message : "");
    obj_record_set_field(rec, 1, message_val);

    obj_record_set_field(rec, 2, data);

    value_init_record(out, rec);
}

static void result_tuple_init_data(VM* vm, Value* out, Value ok_value, int64_t err_code, const char* err_msg, Value err_data) {
    ObjTuple* tuple = obj_tuple_create(vm, 2);

    obj_tuple_set(tuple, 0, ok_value);
    value_free(&ok_value);

    Value err_val;
    if (err_code != 0) {
        error_value_init(vm, &err_val, err_code, err_msg, err_data);
    } else {
        value_free(&err_data);
        value_init_nil(&err_val);
    }
    obj_tuple_set(tuple, 1, err_val);
    value_free(&err_val);

    value_init_tuple(out, tuple);
}

static void result_tuple_set_data(VM* vm, Value* slot, Value ok_value, int64_t err_code, const char* err_msg, Value err_data) {
    Value out;
    result_tuple_init_data(vm, &out, ok_value, err_code, err_msg, err_data);
    value_free(slot);
    *slot = out;
}

static void result_tuple_set(VM* vm, Value* slot, Value ok_value, int64_t err_code, const char* err_msg) {
    Value data;
    value_init_nil(&data);
    result_tuple_set_data(vm, slot, ok_value, err_code, err_msg, data);
}

static void result_permission_denied(VM* vm, Value* slot, Value ok_value, const char* message) {
    result_tuple_set(vm,
                     slot,
                     ok_value,
                     ERR_PERMISSION,
                     message ? message : "Operation is disabled by runtime capability policy");
}

static char* resolve_file_path(VM* vm, const char* user_path, const char** out_err) {
    if (out_err) *out_err = NULL;
    if (!vm_is_file_io_enabled(vm)) {
        if (out_err) *out_err = "File I/O is disabled";
        return NULL;
    }

    const char* root = vm_get_sandbox_root(vm);
    if (!root || root[0] == '\0') {
        return safe_strdup(user_path);
    }

    const char* err = NULL;
    char* resolved = path_sandbox_resolve_alloc(root, user_path, true, &err);
    if (!resolved) {
        if (out_err) *out_err = err ? err : "Path not allowed";
        return NULL;
    }

    return resolved;
}

static bool open_file_with_limit(VM* vm,
                                 const char* path,
                                 const char* mode,
                                 FILE** out_file,
                                 int64_t* out_err_code,
                                 const char** out_err_msg) {
    if (out_file) *out_file = NULL;
    if (out_err_code) *out_err_code = 0;
    if (out_err_msg) *out_err_msg = NULL;
    if (!vm || !path || !mode || !out_file) return false;

    if (!vm_try_acquire_file_handle(vm)) {
        if (out_err_code) *out_err_code = ERR_LIMIT;
        if (out_err_msg) *out_err_msg = "Open file limit exceeded";
        return false;
    }

    FILE* file = NULL;
    const char* sandbox_root = vm_get_sandbox_root(vm);
    if (sandbox_root && sandbox_root[0] != '\0') {
        const char* sandbox_open_err = NULL;
        file = path_sandbox_fopen_resolved(sandbox_root, path, mode, &sandbox_open_err);
        if (!file && sandbox_open_err && out_err_msg) {
            *out_err_msg = sandbox_open_err;
        }
    } else {
        file = fopen(path, mode);
    }

    if (!file) {
        vm_release_file_handle(vm);
        if (out_err_code) *out_err_code = ERR_IO;
        if (out_err_msg && !*out_err_msg) *out_err_msg = "Failed to open file";
        return false;
    }

    *out_file = file;
    return true;
}

static void close_file_with_limit(VM* vm, FILE** file_ptr) {
    if (!vm || !file_ptr || !*file_ptr) return;
    fclose(*file_ptr);
    *file_ptr = NULL;
    vm_release_file_handle(vm);
}

static const char* json_value_type_name(ValueType type) {
    switch (type) {
        case VAL_INT: return "int";
        case VAL_BOOL: return "bool";
        case VAL_DOUBLE: return "double";
        case VAL_BIGINT: return "bigint";
        case VAL_STRING: return "string";
        case VAL_BYTES: return "bytes";
        case VAL_ARRAY: return "array";
        case VAL_NIL: return "nil";
        case VAL_FUNCTION: return "function";
        case VAL_NATIVE: return "native";
        case VAL_RECORD: return "record";
        case VAL_TUPLE: return "tuple";
        case VAL_MAP: return "map";
        case VAL_SET: return "set";
        case VAL_SOCKET: return "socket";
        case VAL_FILE: return "file";
        default: return "unknown";
    }
}

static ArrayKind json_detect_dense_primitive_array_kind(const cJSON* array_item) {
    if (!array_item || !cJSON_IsArray(array_item)) {
        return ARRAY_KIND_BOXED;
    }

    const cJSON* child = array_item->child;
    if (!child) {
        return ARRAY_KIND_BOXED;
    }

    ArrayKind detected = ARRAY_KIND_BOXED;
    while (child) {
        if (cJSON_IsNumber(child)) {
            if (cJSON_IsInt64(child)) {
                if (detected == ARRAY_KIND_BOXED) {
                    detected = ARRAY_KIND_INT;
                } else if (detected == ARRAY_KIND_BOOL) {
                    return ARRAY_KIND_BOXED;
                }
            } else {
                if (detected == ARRAY_KIND_BOXED || detected == ARRAY_KIND_INT) {
                    detected = ARRAY_KIND_DOUBLE;
                } else if (detected == ARRAY_KIND_BOOL) {
                    return ARRAY_KIND_BOXED;
                }
            }
        } else if (cJSON_IsTrue(child) || cJSON_IsFalse(child)) {
            if (detected == ARRAY_KIND_BOXED) {
                detected = ARRAY_KIND_BOOL;
            } else if (detected != ARRAY_KIND_BOOL) {
                return ARRAY_KIND_BOXED;
            }
        } else {
            return ARRAY_KIND_BOXED;
        }

        child = child->next;
    }

    return detected;
}

static bool json_from_cjson(VM* vm, const cJSON* item, Value* out, int depth) {
    (void)vm;
    if (depth > CJSON_NESTING_LIMIT) {
        return false;
    }

    if (!item) {
        value_init_nil(out);
        return true;
    }

    if (cJSON_IsNull(item)) {
        value_init_nil(out);
        return true;
    }

    if (cJSON_IsFalse(item)) {
        value_init_bool(out, false);
        return true;
    }

    if (cJSON_IsTrue(item)) {
        value_init_bool(out, true);
        return true;
    }

    if (cJSON_IsNumber(item)) {
        if (cJSON_IsInt64(item)) {
            value_init_int(out, cJSON_GetInt64Value(item));
        } else {
            value_init_double(out, cJSON_GetNumberValue(item));
        }
        return true;
    }

    if (cJSON_IsString(item)) {
        const char* s = cJSON_GetStringValue(item);
        value_init_string(out, s ? s : "");
        return true;
    }

    if (cJSON_IsArray(item)) {
        int count = cJSON_GetArraySize(item);
        ArrayKind fast_kind = json_detect_dense_primitive_array_kind(item);
        if (count > 0 && fast_kind != ARRAY_KIND_BOXED) {
            ObjArray* arr = obj_array_create_typed(vm, count, fast_kind);
            const cJSON* child = item->child;
            int idx = 0;

            if (fast_kind == ARRAY_KIND_INT) {
                while (child) {
                    arr->data.ints[idx++] = cJSON_GetInt64Value(child);
                    child = child->next;
                }
            } else if (fast_kind == ARRAY_KIND_DOUBLE) {
                while (child) {
                    arr->data.doubles[idx++] = cJSON_GetNumberValue(child);
                    child = child->next;
                }
            } else if (fast_kind == ARRAY_KIND_BOOL) {
                while (child) {
                    arr->data.bools[idx++] = (uint8_t)(cJSON_IsTrue(child) ? 1 : 0);
                    child = child->next;
                }
            }

            arr->count = idx;
            value_init_array(out, arr);
            return true;
        }

        ObjArray* arr = obj_array_create(vm, count);
        const cJSON* child = item->child;
        while (child) {
            Value elem;
            if (!json_from_cjson(vm, child, &elem, depth + 1)) {
                obj_array_free(arr);
                value_init_nil(out);
                return false;
            }
            obj_array_push(arr, elem);
            child = child->next;
        }
        value_init_array(out, arr);
        return true;
    }

    if (cJSON_IsObject(item)) {
        ObjMap* map = obj_map_create(vm);
        const cJSON* child = item->child;
        while (child) {
            if (!child->string) {
                obj_map_free(map);
                value_init_nil(out);
                return false;
            }
            Value val;
            if (!json_from_cjson(vm, child, &val, depth + 1)) {
                obj_map_free(map);
                value_init_nil(out);
                return false;
            }
            obj_map_set_cstr(map, child->string, val);
            value_free(&val);
            child = child->next;
        }
        value_init_map(out, map);
        return true;
    }

    return false;
}

static cJSON* json_to_cjson(VM* vm, const Value* val, int depth, char* error_buf, size_t error_buf_size) {
    (void)vm;
    if (depth > CJSON_NESTING_LIMIT) {
        snprintf(error_buf, error_buf_size, "jsonStringify nesting limit exceeded");
        return NULL;
    }

    switch (value_get_type(val)) {
        case VAL_NIL:
            return cJSON_CreateNull();
        case VAL_BOOL:
            return cJSON_CreateBool(value_get_bool(val));
        case VAL_INT:
            return cJSON_CreateInt64(value_get_int(val));
        case VAL_DOUBLE:
            if (isnan(value_get_double(val)) || isinf(value_get_double(val))) {
                snprintf(error_buf, error_buf_size, "jsonStringify cannot encode NaN or Infinity");
                return NULL;
            }
            return cJSON_CreateNumber(value_get_double(val));
        case VAL_BIGINT: {
            int64_t out = 0;
            if (!obj_bigint_to_int64(value_get_bigint_obj(val), &out)) {
                snprintf(error_buf, error_buf_size, "jsonStringify BigInt out of int64 range");
                return NULL;
            }
            return cJSON_CreateInt64(out);
        }
        case VAL_STRING: {
            ObjString* str = value_get_string_obj(val);
            return cJSON_CreateString(str ? str->chars : "");
        }
        case VAL_ARRAY: {
            ObjArray* vml_arr = value_get_array_obj(val);
            if (!vml_arr) {
                snprintf(error_buf, error_buf_size, "jsonStringify array is null");
                return NULL;
            }
            cJSON* arr = cJSON_CreateArray();
            if (!arr) {
                snprintf(error_buf, error_buf_size, "jsonStringify failed to create array");
                return NULL;
            }
            if (vml_arr->kind == ARRAY_KIND_INT ||
                vml_arr->kind == ARRAY_KIND_DOUBLE ||
                vml_arr->kind == ARRAY_KIND_BOOL ||
                vml_arr->kind == ARRAY_KIND_BYTE) {
                for (int i = 0; i < vml_arr->count; i++) {
                    cJSON* child = NULL;
                    if (vml_arr->kind == ARRAY_KIND_INT) {
                        child = cJSON_CreateInt64(vml_arr->data.ints[i]);
                    } else if (vml_arr->kind == ARRAY_KIND_DOUBLE) {
                        double d = vml_arr->data.doubles[i];
                        if (isnan(d) || isinf(d)) {
                            cJSON_Delete(arr);
                            snprintf(error_buf, error_buf_size, "jsonStringify cannot encode NaN or Infinity");
                            return NULL;
                        }
                        child = cJSON_CreateNumber(d);
                    } else if (vml_arr->kind == ARRAY_KIND_BOOL) {
                        child = cJSON_CreateBool(vml_arr->data.bools[i] != 0);
                    } else if (vml_arr->kind == ARRAY_KIND_BYTE) {
                        child = cJSON_CreateInt64((int64_t)vml_arr->data.bytes[i]);
                    }

                    if (!child) {
                        cJSON_Delete(arr);
                        snprintf(error_buf, error_buf_size, "jsonStringify failed to encode array item");
                        return NULL;
                    }
                    if (!cJSON_AddItemToArray(arr, child)) {
                        cJSON_Delete(child);
                        cJSON_Delete(arr);
                        snprintf(error_buf, error_buf_size, "jsonStringify failed to add array item");
                        return NULL;
                    }
                }
                return arr;
            }

            for (int i = 0; i < vml_arr->count; i++) {
                cJSON* child = json_to_cjson(vm, &vml_arr->data.elements[i], depth + 1, error_buf, error_buf_size);
                if (!child) {
                    cJSON_Delete(arr);
                    return NULL;
                }
                if (!cJSON_AddItemToArray(arr, child)) {
                    cJSON_Delete(child);
                    cJSON_Delete(arr);
                    snprintf(error_buf, error_buf_size, "jsonStringify failed to add array item");
                    return NULL;
                }
            }
            return arr;
        }
        case VAL_MAP: {
            ObjMap* map = value_get_map_obj(val);
            if (!map) {
                snprintf(error_buf, error_buf_size, "jsonStringify map is null");
                return NULL;
            }
            cJSON* obj = cJSON_CreateObject();
            if (!obj) {
                snprintf(error_buf, error_buf_size, "jsonStringify failed to create object");
                return NULL;
            }
            if (!map->slots) {
                cJSON_Delete(obj);
                snprintf(error_buf, error_buf_size, "jsonStringify map storage is null");
                return NULL;
            }
            for (int i = 0; i < map->capacity; i++) {
                MapSlot* slot = &map->slots[i];
                if (slot->hash < 2) continue;

                const Value* key = &slot->key;
                const Value* value = &slot->value;

                const char* key_str = NULL;
                char key_buf[32];
                if (value_get_type(key) == VAL_STRING) {
                    ObjString* key_obj = value_get_string_obj(key);
                    key_str = key_obj ? key_obj->chars : "";
                } else if (value_get_type(key) == VAL_INT) {
                    snprintf(key_buf, sizeof(key_buf), "%lld", (long long)value_get_int(key));
                    key_str = key_buf;
                } else {
                    cJSON_Delete(obj);
                    snprintf(error_buf, error_buf_size, "jsonStringify map keys must be string or int");
                    return NULL;
                }

                cJSON* child = json_to_cjson(vm, value, depth + 1, error_buf, error_buf_size);
                if (!child) {
                    cJSON_Delete(obj);
                    return NULL;
                }

                if (!cJSON_AddItemToObject(obj, key_str, child)) {
                    cJSON_Delete(child);
                    cJSON_Delete(obj);
                    snprintf(error_buf, error_buf_size, "jsonStringify failed to add object item");
                    return NULL;
                }
            }
            return obj;
        }
        default:
            snprintf(error_buf, error_buf_size, "jsonStringify does not support type: %s", json_value_type_name(value_get_type(val)));
            return NULL;
    }
}

static bool map_lookup_string_key(ObjMap* map, const char* key, Value* out, bool* exists) {
    if (exists) *exists = false;
    if (out) value_init_nil(out);
    if (!map || !key) return false;
    bool has = obj_map_try_get_cstr(map, key, out);
    if (exists) *exists = has;
    return true;
}

static bool json_decode_identifier_like(const char* s) {
    if (!s || s[0] == '\0') return false;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (int i = 1; s[i] != '\0'; i++) {
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return false;
    }
    return true;
}

static void json_decode_path_field(const char* base, const char* field, char* out, size_t out_size) {
    const char* root = (base && base[0] != '\0') ? base : "$";
    const char* name = field ? field : "";
    if (!out || out_size == 0) return;

    if (name[0] == '\0') {
        snprintf(out, out_size, "%s", root);
        return;
    }

    if (json_decode_identifier_like(name)) {
        snprintf(out, out_size, "%s.%s", root, name);
        return;
    }

    snprintf(out, out_size, "%s[\"%s\"]", root, name);
}

static void json_decode_path_index(const char* base, int index, char* out, size_t out_size) {
    const char* root = (base && base[0] != '\0') ? base : "$";
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "%s[%d]", root, index);
}

static void json_decode_path_for_key(const char* base, const Value* key, char* out, size_t out_size) {
    const char* root = (base && base[0] != '\0') ? base : "$";
    if (!out || out_size == 0) return;
    if (!key) {
        snprintf(out, out_size, "%s[?]", root);
        return;
    }

    ObjString* key_str = value_get_string_obj(key);
    if (value_get_type(key) == VAL_STRING && key_str && key_str->chars) {
        json_decode_path_field(root, key_str->chars, out, out_size);
        return;
    }
    if (value_get_type(key) == VAL_INT) {
        snprintf(out, out_size, "%s[%lld]", root, (long long)value_get_int(key));
        return;
    }

    snprintf(out, out_size, "%s[?]", root);
}

static bool json_parse_int64_strict(const char* text, int64_t* out_value) {
    if (!text || text[0] == '\0') return false;

    errno = 0;
    char* end = NULL;
    long long parsed = strtoll(text, &end, 10);
    if (errno == ERANGE || end == text || (end && *end != '\0')) {
        return false;
    }

    if (out_value) {
        *out_value = (int64_t)parsed;
    }
    return true;
}

static void json_error_data_set_string(ObjMap* map, const char* key, const char* value) {
    if (!map || !key) return;
    Value v;
    value_init_string(&v, value ? value : "");
    obj_map_set_cstr(map, key, v);
    value_free(&v);
}

static void json_error_data_set_int(ObjMap* map, const char* key, int64_t value) {
    if (!map || !key) return;
    Value v;
    value_init_int(&v, value);
    obj_map_set_cstr(map, key, v);
    value_free(&v);
}

static Value json_decode_make_error_data_ext(VM* vm,
                                             const char* path,
                                             const char* schema_path,
                                             const char* expected,
                                             const char* actual,
                                             const char* kind,
                                             const char* detail) {
    ObjMap* data_map = obj_map_create(vm);

    json_error_data_set_string(data_map, "path", path ? path : "$");
    if (schema_path && schema_path[0] != '\0') {
        json_error_data_set_string(data_map, "schemaPath", schema_path);
    }
    json_error_data_set_string(data_map, "expected", expected ? expected : "");
    json_error_data_set_string(data_map, "actual", actual ? actual : "");
    // Canonical aliases for typed decoders; keep expected/actual for compatibility.
    json_error_data_set_string(data_map, "expectedType", expected ? expected : "");
    json_error_data_set_string(data_map, "actualType", actual ? actual : "");
    if (kind && kind[0] != '\0') {
        json_error_data_set_string(data_map, "kind", kind);
    }
    if (detail && detail[0] != '\0') {
        json_error_data_set_string(data_map, "detail", detail);
    }

    Value out;
    value_init_map(&out, data_map);
    return out;
}

static void json_decode_describe_value(const Value* value, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!value) {
        snprintf(out, out_size, "null");
        return;
    }

    switch (value_get_type(value)) {
        case VAL_NIL:
            snprintf(out, out_size, "nil");
            return;
        case VAL_BOOL:
            snprintf(out, out_size, "%s", value_get_bool(value) ? "true" : "false");
            return;
        case VAL_INT:
            snprintf(out, out_size, "%lld", (long long)value_get_int(value));
            return;
        case VAL_DOUBLE: {
            double d = value_get_double(value);
            if (isnan(d)) {
                snprintf(out, out_size, "nan");
            } else if (isinf(d)) {
                snprintf(out, out_size, "%s", d < 0 ? "-inf" : "inf");
            } else {
                snprintf(out, out_size, "%g", d);
            }
            return;
        }
        case VAL_BIGINT: {
            char* text = obj_bigint_to_string(value_get_bigint_obj(value));
            snprintf(out, out_size, "%s", text ? text : "0");
            if (text) free(text);
            return;
        }
        case VAL_STRING: {
            ObjString* str = value_get_string_obj(value);
            const char* s = (str && str->chars) ? str->chars : "";
            snprintf(out, out_size, "\"%s\"", s);
            return;
        }
        case VAL_BYTES:
            snprintf(out, out_size, "<bytes len=%d>", value_get_bytes_obj(value) ? value_get_bytes_obj(value)->length : 0);
            return;
        case VAL_ARRAY:
            snprintf(out, out_size, "<array len=%d>", value_get_array_obj(value) ? value_get_array_obj(value)->count : 0);
            return;
        case VAL_MAP:
            snprintf(out, out_size, "<map count=%d>", value_get_map_obj(value) ? value_get_map_obj(value)->count : 0);
            return;
        default:
            snprintf(out, out_size, "<%s>", json_value_type_name(value_get_type(value)));
            return;
    }
}

static bool json_decode_deep_equal(const Value* a, const Value* b, int depth) {
    if (!a || !b) return false;
    if (depth > CJSON_NESTING_LIMIT) return false;

    ValueType a_type = value_get_type(a);
    ValueType b_type = value_get_type(b);
    if (a_type != b_type) {
        if (a_type == VAL_INT && b_type == VAL_DOUBLE) {
            return (double)value_get_int(a) == value_get_double(b);
        }
        if (a_type == VAL_DOUBLE && b_type == VAL_INT) {
            return value_get_double(a) == (double)value_get_int(b);
        }
        if (a_type == VAL_INT && b_type == VAL_BIGINT) {
            ObjBigInt* temp = obj_bigint_from_int64(value_get_int(a));
            int cmp = obj_bigint_compare(temp, value_get_bigint_obj(b));
            obj_bigint_release(temp);
            return cmp == 0;
        }
        if (a_type == VAL_BIGINT && b_type == VAL_INT) {
            ObjBigInt* temp = obj_bigint_from_int64(value_get_int(b));
            int cmp = obj_bigint_compare(value_get_bigint_obj(a), temp);
            obj_bigint_release(temp);
            return cmp == 0;
        }
        return false;
    }

    switch (a_type) {
        case VAL_NIL:
            return true;
        case VAL_BOOL:
            return value_get_bool(a) == value_get_bool(b);
        case VAL_INT:
            return value_get_int(a) == value_get_int(b);
        case VAL_DOUBLE:
            return value_get_double(a) == value_get_double(b);
        case VAL_BIGINT:
            return obj_bigint_compare(value_get_bigint_obj(a), value_get_bigint_obj(b)) == 0;
        case VAL_STRING: {
            ObjString* a_str = value_get_string_obj(a);
            ObjString* b_str = value_get_string_obj(b);
            const char* as = (a_str && a_str->chars) ? a_str->chars : "";
            const char* bs = (b_str && b_str->chars) ? b_str->chars : "";
            return strcmp(as, bs) == 0;
        }
        case VAL_BYTES: {
            ObjBytes* a_bytes = value_get_bytes_obj(a);
            ObjBytes* b_bytes = value_get_bytes_obj(b);
            int alen = a_bytes ? a_bytes->length : 0;
            int blen = b_bytes ? b_bytes->length : 0;
            if (alen != blen) return false;
            if (alen == 0) return true;
            uint8_t* ad = obj_bytes_data(a_bytes);
            uint8_t* bd = obj_bytes_data(b_bytes);
            if (!ad || !bd) return false;
            return memcmp(ad, bd, (size_t)alen) == 0;
        }
        case VAL_ARRAY: {
            ObjArray* a_arr = value_get_array_obj(a);
            ObjArray* b_arr = value_get_array_obj(b);
            if (!a_arr || !b_arr) return a_arr == b_arr;
            if (a_arr->count != b_arr->count) return false;
            for (int i = 0; i < a_arr->count; i++) {
                Value av;
                Value bv;
                obj_array_get(a_arr, i, &av);
                obj_array_get(b_arr, i, &bv);
                bool eq = json_decode_deep_equal(&av, &bv, depth + 1);
                if (!eq) return false;
            }
            return true;
        }
        case VAL_MAP: {
            ObjMap* a_map = value_get_map_obj(a);
            ObjMap* b_map = value_get_map_obj(b);
            if (!a_map || !b_map) return a_map == b_map;
            if (a_map->count != b_map->count) return false;
            for (int i = 0; i < a_map->capacity; i++) {
                MapSlot* slot = &a_map->slots[i];
                if (slot->hash < 2) continue;
                if (!obj_map_has(b_map, slot->key)) return false;
                Value other = obj_map_get(b_map, slot->key);
                bool eq = json_decode_deep_equal(&slot->value, &other, depth + 1);
                value_free(&other);
                if (!eq) return false;
            }
            return true;
        }
        default: {
            Value va = *a;
            Value vb = *b;
            return value_equals(&va, &vb);
        }
    }
}

static void json_trim_ascii_whitespace(const char** start, const char** end) {
    if (!start || !end || !*start || !*end) return;
    while (*start < *end && isspace((unsigned char)**start)) (*start)++;
    while (*end > *start && isspace((unsigned char)*((*end) - 1))) (*end)--;
}

static bool json_parse_fast_plain_string(const char* start, const char* end, Value* out) {
    if (!start || !end || !out) return false;
    if ((end - start) < 2) return false;
    if (start[0] != '"' || end[-1] != '"') return false;

    const char* inner_start = start + 1;
    const char* inner_end = end - 1;
    for (const char* p = inner_start; p < inner_end; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\\' || ch < 0x20) {
            // Escaped/control strings fall back to the full parser.
            return false;
        }
    }

    ptrdiff_t len_diff = inner_end - inner_start;
    if (len_diff < 0 || len_diff > INT_MAX) return false;
    size_t len = (size_t)len_diff;
    char* text = (char*)safe_malloc(len + 1);
    if (len > 0) {
        memcpy(text, inner_start, len);
    }
    text[len] = '\0';
    value_init_string(out, text);
    free(text);
    return true;
}

static bool json_parse_fast_number_token(const char* start, const char* end, Value* out) {
    if (!start || !end || !out || start >= end) return false;

    const char* p = start;
    if (*p == '-') {
        p++;
        if (p >= end) return false;
    }

    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        p++;
        while (p < end && *p >= '0' && *p <= '9') p++;
    } else {
        return false;
    }

    bool is_float = false;
    if (p < end && *p == '.') {
        is_float = true;
        p++;
        if (p >= end || *p < '0' || *p > '9') return false;
        while (p < end && *p >= '0' && *p <= '9') p++;
    }

    if (p < end && (*p == 'e' || *p == 'E')) {
        is_float = true;
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        if (p >= end || *p < '0' || *p > '9') return false;
        while (p < end && *p >= '0' && *p <= '9') p++;
    }

    if (p != end) return false;

    ptrdiff_t len_diff = end - start;
    if (len_diff <= 0 || len_diff > INT_MAX) return false;
    size_t len = (size_t)len_diff;

    char stack_buf[64];
    char* number_text = stack_buf;
    bool used_heap = false;
    if (len >= sizeof(stack_buf)) {
        number_text = (char*)safe_malloc(len + 1);
        used_heap = true;
    }
    memcpy(number_text, start, len);
    number_text[len] = '\0';

    bool ok = false;
    if (!is_float) {
        errno = 0;
        char* endptr = NULL;
        long long parsed = strtoll(number_text, &endptr, 10);
        if (errno == 0 && endptr == number_text + len) {
            value_init_int(out, (int64_t)parsed);
            ok = true;
        }
    } else {
        errno = 0;
        char* endptr = NULL;
        double parsed = strtod(number_text, &endptr);
        if (errno == 0 && endptr == number_text + len && !isnan(parsed) && !isinf(parsed)) {
            value_init_double(out, parsed);
            ok = true;
        }
    }

    if (used_heap) {
        free(number_text);
    }
    return ok;
}

static bool json_parse_fast_scalar(VM* vm, const char* json, Value* out) {
    (void)vm;
    if (!json || !out) return false;

    const char* start = json;
    const char* end = json + strlen(json);
    json_trim_ascii_whitespace(&start, &end);
    if (start >= end) return false;

    ptrdiff_t len_diff = end - start;
    if (len_diff <= 0) return false;
    size_t len = (size_t)len_diff;

    if (len == 4 && memcmp(start, "null", 4) == 0) {
        value_init_nil(out);
        return true;
    }
    if (len == 4 && memcmp(start, "true", 4) == 0) {
        value_init_bool(out, true);
        return true;
    }
    if (len == 5 && memcmp(start, "false", 5) == 0) {
        value_init_bool(out, false);
        return true;
    }
    if (start[0] == '"' && end[-1] == '"') {
        return json_parse_fast_plain_string(start, end, out);
    }
    if (start[0] == '-' || (start[0] >= '0' && start[0] <= '9')) {
        return json_parse_fast_number_token(start, end, out);
    }
    return false;
}

static void json_parse_fast_skip_ws(const char** cur, const char* end) {
    if (!cur || !*cur) return;
    while (*cur < end && isspace((unsigned char)**cur)) {
        (*cur)++;
    }
}

static bool json_parse_fast_plain_string_ptr(const char** cur, const char* end, Value* out) {
    if (!cur || !*cur || !out) return false;
    const char* p = *cur;
    if (p >= end || *p != '"') return false;
    p++;

    const char* start = p;
    while (p < end) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"') break;
        if (ch == '\\' || ch < 0x20) return false;
        p++;
    }
    if (p >= end || *p != '"') return false;

    ptrdiff_t len_diff = p - start;
    if (len_diff < 0 || len_diff > INT_MAX) return false;
    size_t len = (size_t)len_diff;

    char* text = (char*)safe_malloc(len + 1);
    if (len > 0) {
        memcpy(text, start, len);
    }
    text[len] = '\0';
    value_init_string(out, text);
    free(text);

    *cur = p + 1;
    return true;
}

static bool json_parse_fast_number_ptr(const char** cur, const char* end, Value* out) {
    if (!cur || !*cur || !out) return false;
    const char* p = *cur;
    if (p >= end) return false;

    if (*p == '-') {
        p++;
        if (p >= end) return false;
    }

    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        p++;
        while (p < end && *p >= '0' && *p <= '9') p++;
    } else {
        return false;
    }

    if (p < end && *p == '.') {
        p++;
        if (p >= end || *p < '0' || *p > '9') return false;
        while (p < end && *p >= '0' && *p <= '9') p++;
    }

    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        if (p >= end || *p < '0' || *p > '9') return false;
        while (p < end && *p >= '0' && *p <= '9') p++;
    }

    if (!json_parse_fast_number_token(*cur, p, out)) return false;
    *cur = p;
    return true;
}

static bool json_parse_fast_literal_ptr(const char** cur, const char* end, Value* out) {
    if (!cur || !*cur || !out) return false;
    const char* p = *cur;
    size_t rem = (size_t)(end - p);

    if (rem >= 4 && memcmp(p, "true", 4) == 0) {
        value_init_bool(out, true);
        *cur = p + 4;
        return true;
    }
    if (rem >= 5 && memcmp(p, "false", 5) == 0) {
        value_init_bool(out, false);
        *cur = p + 5;
        return true;
    }
    if (rem >= 4 && memcmp(p, "null", 4) == 0) {
        value_init_nil(out);
        *cur = p + 4;
        return true;
    }
    return false;
}

static bool json_parse_fast_primitive_ptr(const char** cur, const char* end, Value* out) {
    if (!cur || !*cur || !out) return false;
    if (*cur >= end) return false;

    unsigned char ch = (unsigned char)**cur;
    if (ch == '"') return json_parse_fast_plain_string_ptr(cur, end, out);
    if (ch == '-' || (ch >= '0' && ch <= '9')) return json_parse_fast_number_ptr(cur, end, out);
    if (ch == 't' || ch == 'f' || ch == 'n') return json_parse_fast_literal_ptr(cur, end, out);
    return false;
}

static bool json_parse_fast_primitive_array_ptr(VM* vm, const char** cur, const char* end, Value* out) {
    if (!vm || !cur || !*cur || !out) return false;
    const char* p = *cur;
    if (p >= end || *p != '[') return false;
    p++;
    json_parse_fast_skip_ws(&p, end);

    ObjArray* arr = obj_array_create(vm, 4);
    if (p < end && *p == ']') {
        p++;
        value_init_array(out, arr);
        *cur = p;
        return true;
    }

    while (p < end) {
        Value elem;
        if (!json_parse_fast_primitive_ptr(&p, end, &elem)) {
            obj_array_free(arr);
            return false;
        }
        obj_array_push(arr, elem);

        json_parse_fast_skip_ws(&p, end);
        if (p < end && *p == ',') {
            p++;
            json_parse_fast_skip_ws(&p, end);
            continue;
        }
        if (p < end && *p == ']') {
            p++;
            value_init_array(out, arr);
            *cur = p;
            return true;
        }
        break;
    }

    obj_array_free(arr);
    return false;
}

static bool json_parse_fast_flat_object_ptr(VM* vm, const char** cur, const char* end, Value* out) {
    if (!vm || !cur || !*cur || !out) return false;
    const char* p = *cur;
    if (p >= end || *p != '{') return false;
    p++;
    json_parse_fast_skip_ws(&p, end);

    ObjMap* map = obj_map_create(vm);
    if (p < end && *p == '}') {
        p++;
        value_init_map(out, map);
        *cur = p;
        return true;
    }

    while (p < end) {
        Value key;
        if (!json_parse_fast_plain_string_ptr(&p, end, &key)) {
            obj_map_free(map);
            return false;
        }

        json_parse_fast_skip_ws(&p, end);
        if (p >= end || *p != ':') {
            value_free(&key);
            obj_map_free(map);
            return false;
        }
        p++;
        json_parse_fast_skip_ws(&p, end);

        Value val;
        bool ok = false;
        if (p < end && *p == '[') {
            ok = json_parse_fast_primitive_array_ptr(vm, &p, end, &val);
        } else {
            ok = json_parse_fast_primitive_ptr(&p, end, &val);
        }
        if (!ok) {
            value_free(&key);
            obj_map_free(map);
            return false;
        }

        obj_map_set(map, key, val);
        value_free(&key);
        value_free(&val);

        json_parse_fast_skip_ws(&p, end);
        if (p < end && *p == ',') {
            p++;
            json_parse_fast_skip_ws(&p, end);
            continue;
        }
        if (p < end && *p == '}') {
            p++;
            value_init_map(out, map);
            *cur = p;
            return true;
        }
        break;
    }

    obj_map_free(map);
    return false;
}

static bool json_parse_fast_common(VM* vm, const char* json, Value* out) {
    if (!vm || !json || !out) return false;

    const char* start = json;
    const char* end = json + strlen(json);
    json_trim_ascii_whitespace(&start, &end);
    if (start >= end) return false;

    const char* p = start;
    Value parsed;
    bool ok = false;
    if (*p == '{') {
        ok = json_parse_fast_flat_object_ptr(vm, &p, end, &parsed);
    } else if (*p == '[') {
        ok = json_parse_fast_primitive_array_ptr(vm, &p, end, &parsed);
    } else {
        ok = json_parse_fast_scalar(vm, start, &parsed);
        p = end;
    }
    if (!ok) return false;

    json_parse_fast_skip_ws(&p, end);
    if (p != end) {
        value_free(&parsed);
        return false;
    }

    *out = parsed;
    return true;
}

static size_t json_string_escaped_length(const char* text, bool* ok) {
    if (ok) *ok = false;
    if (!text) {
        if (ok) *ok = true;
        return 0;
    }

    size_t total = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p != '\0'; p++) {
        size_t add = 1;
        switch (*p) {
            case '"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                add = 2;
                break;
            default:
                if (*p < 0x20) add = 6;
                break;
        }
        if (total > SIZE_MAX - add) {
            return 0;
        }
        total += add;
    }

    if (ok) *ok = true;
    return total;
}

static char json_hex_digit(unsigned int value) {
    return (char)(value < 10 ? ('0' + value) : ('a' + (value - 10)));
}

static char* json_string_write_escaped(char* dst, const char* text) {
    const unsigned char* p = (const unsigned char*)(text ? text : "");
    while (*p != '\0') {
        unsigned char ch = *p++;
        switch (ch) {
            case '"':
                *dst++ = '\\';
                *dst++ = '"';
                break;
            case '\\':
                *dst++ = '\\';
                *dst++ = '\\';
                break;
            case '\b':
                *dst++ = '\\';
                *dst++ = 'b';
                break;
            case '\f':
                *dst++ = '\\';
                *dst++ = 'f';
                break;
            case '\n':
                *dst++ = '\\';
                *dst++ = 'n';
                break;
            case '\r':
                *dst++ = '\\';
                *dst++ = 'r';
                break;
            case '\t':
                *dst++ = '\\';
                *dst++ = 't';
                break;
            default:
                if (ch < 0x20) {
                    *dst++ = '\\';
                    *dst++ = 'u';
                    *dst++ = '0';
                    *dst++ = '0';
                    *dst++ = json_hex_digit((unsigned int)(ch >> 4));
                    *dst++ = json_hex_digit((unsigned int)(ch & 0x0F));
                } else {
                    *dst++ = (char)ch;
                }
                break;
        }
    }
    return dst;
}

static bool json_stringify_fast_scalar(VM* vm, const Value* input, Value* out) {
    if (!vm || !input || !out) return false;
    int max_len = vm->config.max_string_length;

    switch (value_get_type(input)) {
        case VAL_NIL: {
            if (max_len > 0 && max_len < 4) return false;
            value_init_string(out, "null");
            return true;
        }
        case VAL_BOOL: {
            bool b = value_get_bool(input);
            const char* text = b ? "true" : "false";
            int needed = b ? 4 : 5;
            if (max_len > 0 && max_len < needed) return false;
            value_init_string(out, text);
            return true;
        }
        case VAL_INT: {
            char buf[64];
            int written = snprintf(buf, sizeof(buf), "%lld", (long long)value_get_int(input));
            if (written <= 0) return false;
            if (max_len > 0 && written > max_len) return false;
            value_init_string(out, buf);
            return true;
        }
        case VAL_STRING: {
            ObjString* input_str = value_get_string_obj(input);
            const char* text = (input_str && input_str->chars) ? input_str->chars : "";
            bool escaped_ok = false;
            size_t escaped_len = json_string_escaped_length(text, &escaped_ok);
            if (!escaped_ok) return false;
            if (escaped_len > SIZE_MAX - 3) return false;
            size_t total_len = escaped_len + 2; // quotes
            if (max_len > 0 && (int64_t)total_len > (int64_t)max_len) return false;

            char* rendered = (char*)safe_malloc(total_len + 1);
            char* p = rendered;
            *p++ = '"';
            p = json_string_write_escaped(p, text);
            *p++ = '"';
            *p = '\0';

            value_init_string(out, rendered);
            free(rendered);
            return true;
        }
        default:
            return false;
    }
}

typedef struct JsonFastBuffer {
    char* data;
    size_t len;
    size_t cap;
    int max_len;
} JsonFastBuffer;

static bool json_fast_buffer_reserve(JsonFastBuffer* buf, size_t add_len) {
    if (!buf) return false;
    if (add_len > SIZE_MAX - buf->len) return false;
    size_t needed = buf->len + add_len;
    if (buf->max_len > 0 && needed > (size_t)buf->max_len) return false;
    if (needed + 1 <= buf->cap) return true;

    size_t new_cap = buf->cap > 0 ? buf->cap : 128;
    while (new_cap < needed + 1) {
        if (new_cap > (SIZE_MAX / 2)) return false;
        new_cap *= 2;
    }
    buf->data = (char*)safe_realloc(buf->data, new_cap);
    buf->cap = new_cap;
    return true;
}

static bool json_fast_buffer_append_char(JsonFastBuffer* buf, char ch) {
    if (!json_fast_buffer_reserve(buf, 1)) return false;
    buf->data[buf->len++] = ch;
    buf->data[buf->len] = '\0';
    return true;
}

static bool json_fast_buffer_append_bytes(JsonFastBuffer* buf, const char* text, size_t len) {
    if (!buf || (!text && len > 0)) return false;
    if (len == 0) return true;
    if (!json_fast_buffer_reserve(buf, len)) return false;
    memcpy(buf->data + buf->len, text, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return true;
}

static bool json_fast_buffer_append_cstr(JsonFastBuffer* buf, const char* text) {
    if (!text) text = "";
    return json_fast_buffer_append_bytes(buf, text, strlen(text));
}

static bool json_fast_buffer_append_int(JsonFastBuffer* buf, int64_t value) {
    char num[64];
    int written = snprintf(num, sizeof(num), "%lld", (long long)value);
    if (written <= 0) return false;
    return json_fast_buffer_append_bytes(buf, num, (size_t)written);
}

static bool json_fast_buffer_append_quoted_string(JsonFastBuffer* buf, const char* text) {
    bool escaped_ok = false;
    size_t escaped_len = json_string_escaped_length(text ? text : "", &escaped_ok);
    if (!escaped_ok) return false;
    if (!json_fast_buffer_reserve(buf, escaped_len + 2)) return false;

    buf->data[buf->len++] = '"';
    char* dst = buf->data + buf->len;
    json_string_write_escaped(dst, text ? text : "");
    buf->len += escaped_len;
    buf->data[buf->len++] = '"';
    buf->data[buf->len] = '\0';
    return true;
}

static bool json_stringify_fast_write_value(JsonFastBuffer* buf, const Value* value, int depth);

static bool json_stringify_fast_write_array(JsonFastBuffer* buf, ObjArray* arr, int depth) {
    if (!buf || !arr) return false;
    if (depth > CJSON_NESTING_LIMIT) return false;
    if (!json_fast_buffer_append_char(buf, '[')) return false;

    for (int i = 0; i < arr->count; i++) {
        if (i > 0 && !json_fast_buffer_append_char(buf, ',')) return false;
        switch (arr->kind) {
            case ARRAY_KIND_INT:
                if (!json_fast_buffer_append_int(buf, arr->data.ints[i])) return false;
                break;
            case ARRAY_KIND_BOOL:
                if (!json_fast_buffer_append_cstr(buf, arr->data.bools[i] ? "true" : "false")) return false;
                break;
            case ARRAY_KIND_BYTE:
                if (!json_fast_buffer_append_int(buf, (int64_t)arr->data.bytes[i])) return false;
                break;
            case ARRAY_KIND_BOXED: {
                Value elem;
                obj_array_get(arr, i, &elem);
                if (!json_stringify_fast_write_value(buf, &elem, depth + 1)) return false;
                break;
            }
            default:
                // Keep number formatting and edge cases delegated to cJSON.
                return false;
        }
    }

    return json_fast_buffer_append_char(buf, ']');
}

static bool json_stringify_fast_write_map(JsonFastBuffer* buf, ObjMap* map, int depth) {
    if (!buf || !map || !map->slots) return false;
    if (depth > CJSON_NESTING_LIMIT) return false;
    if (!json_fast_buffer_append_char(buf, '{')) return false;

    bool first = true;
    for (int i = 0; i < map->capacity; i++) {
        MapSlot* slot = &map->slots[i];
        if (slot->hash < 2) continue;

        const char* key_chars = NULL;
        char key_buf[64];
        if (value_get_type(&slot->key) == VAL_STRING) {
            ObjString* key_str = value_get_string_obj(&slot->key);
            key_chars = (key_str && key_str->chars) ? key_str->chars : "";
        } else if (value_get_type(&slot->key) == VAL_INT) {
            int written = snprintf(key_buf, sizeof(key_buf), "%lld", (long long)value_get_int(&slot->key));
            if (written <= 0 || written >= (int)sizeof(key_buf)) return false;
            key_chars = key_buf;
        } else {
            return false;
        }

        if (!first && !json_fast_buffer_append_char(buf, ',')) return false;
        if (!json_fast_buffer_append_quoted_string(buf, key_chars)) return false;
        if (!json_fast_buffer_append_char(buf, ':')) return false;
        if (!json_stringify_fast_write_value(buf, &slot->value, depth + 1)) return false;
        first = false;
    }

    return json_fast_buffer_append_char(buf, '}');
}

static bool json_stringify_fast_write_value(JsonFastBuffer* buf, const Value* value, int depth) {
    if (!buf || !value) return false;
    switch (value_get_type(value)) {
        case VAL_NIL:
            return json_fast_buffer_append_cstr(buf, "null");
        case VAL_BOOL:
            return json_fast_buffer_append_cstr(buf, value_get_bool(value) ? "true" : "false");
        case VAL_INT:
            return json_fast_buffer_append_int(buf, value_get_int(value));
        case VAL_STRING: {
            ObjString* str = value_get_string_obj(value);
            return json_fast_buffer_append_quoted_string(buf, str ? str->chars : "");
        }
        case VAL_ARRAY:
            return json_stringify_fast_write_array(buf, value_get_array_obj(value), depth);
        case VAL_MAP:
            return json_stringify_fast_write_map(buf, value_get_map_obj(value), depth);
        default:
            return false;
    }
}

static bool json_stringify_fast_common(VM* vm, const Value* input, Value* out) {
    if (!vm || !input || !out) return false;

    JsonFastBuffer buf;
    buf.len = 0;
    buf.cap = 128;
    buf.max_len = vm->config.max_string_length;
    buf.data = (char*)safe_malloc(buf.cap);
    buf.data[0] = '\0';

    bool ok = json_stringify_fast_write_value(&buf, input, 0);
    if (!ok) {
        free(buf.data);
        return false;
    }

    Value rendered;
    value_init_string(&rendered, buf.data);
    free(buf.data);
    *out = rendered;
    return true;
}

static void json_parse_compute_line_column(const char* json, int64_t offset, int64_t* out_line, int64_t* out_column) {
    int64_t line = 1;
    int64_t column = 1;
    if (!json || offset <= 0) {
        if (out_line) *out_line = line;
        if (out_column) *out_column = column;
        return;
    }

    for (int64_t i = 0; i < offset && json[i] != '\0'; i++) {
        if (json[i] == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
    }

    if (out_line) *out_line = line;
    if (out_column) *out_column = column;
}

static void json_parse_make_near_snippet(const char* err_ptr, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!err_ptr || err_ptr[0] == '\0') {
        snprintf(out, out_size, "<eof>");
        return;
    }

    int written = 0;
    for (int i = 0; err_ptr[i] != '\0' && i < 32 && written < (int)out_size - 1; i++) {
        unsigned char ch = (unsigned char)err_ptr[i];
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            out[written++] = ' ';
            continue;
        }
        if (ch < 32) {
            out[written++] = '?';
            continue;
        }
        out[written++] = (char)ch;
    }
    out[written] = '\0';
}

static Value json_parse_make_error_data(VM* vm,
                                        int64_t offset,
                                        int64_t line,
                                        int64_t column,
                                        const char* near_snippet,
                                        bool at_eof) {
    ObjMap* data_map = obj_map_create(vm);
    json_error_data_set_string(data_map, "path", "$");
    json_error_data_set_int(data_map, "offset", offset);
    json_error_data_set_int(data_map, "line", line);
    json_error_data_set_int(data_map, "column", column);
    json_error_data_set_string(data_map, "near", near_snippet && near_snippet[0] != '\0' ? near_snippet : "<eof>");

    ObjMap* span_map = obj_map_create(vm);
    json_error_data_set_int(span_map, "start", offset);
    json_error_data_set_int(span_map, "end", at_eof ? offset : offset + 1);

    Value span_val;
    value_init_map(&span_val, span_map);
    obj_map_set_cstr(data_map, "span", span_val);
    value_free(&span_val);

    Value out;
    value_init_map(&out, data_map);
    return out;
}

static bool json_decode_fail_schema(VM* vm,
                                    int64_t* err_code,
                                    char* err_msg,
                                    size_t err_msg_size,
                                    const char* path,
                                    const char* schema_path,
                                    const char* detail,
                                    Value* err_data) {
    if (err_code) *err_code = ERR_INVALID_ARGUMENT;
    if (err_msg && err_msg_size > 0) {
        snprintf(err_msg,
                 err_msg_size,
                 "jsonDecode schema error at %s: %s",
                 (path && path[0] != '\0') ? path : "$",
                 detail ? detail : "invalid schema");
    }
    if (err_data) {
        value_free(err_data);
        *err_data = json_decode_make_error_data_ext(vm,
                                                    (path && path[0] != '\0') ? path : "$",
                                                    (schema_path && schema_path[0] != '\0') ? schema_path : "$",
                                                    "valid schema",
                                                    "invalid schema",
                                                    "schema_error",
                                                    detail ? detail : "invalid schema");
    }
    return false;
}

static bool json_decode_fail_type(VM* vm,
                                  int64_t* err_code,
                                  char* err_msg,
                                  size_t err_msg_size,
                                  const char* path,
                                  const char* schema_path,
                                  const char* expected,
                                  const Value* actual,
                                  Value* err_data) {
    const char* actual_type = actual ? json_value_type_name(value_get_type(actual)) : "unknown";
    if (err_code) *err_code = ERR_PARSE;
    if (err_msg && err_msg_size > 0) {
        snprintf(err_msg,
                 err_msg_size,
                 "jsonDecode type mismatch at %s: expected %s, got %s",
                 (path && path[0] != '\0') ? path : "$",
                 expected ? expected : "unknown",
                 actual_type);
    }
    if (err_data) {
        value_free(err_data);
        *err_data = json_decode_make_error_data_ext(vm,
                                                    (path && path[0] != '\0') ? path : "$",
                                                    (schema_path && schema_path[0] != '\0') ? schema_path : "$",
                                                    expected ? expected : "unknown",
                                                    actual_type,
                                                    "type_mismatch",
                                                    NULL);
    }
    return false;
}

static bool json_decode_value_with_schema(VM* vm,
                                          const Value* value,
                                          const Value* schema,
                                          const char* path,
                                          const char* schema_path,
                                          int depth,
                                          Value* out,
                                          int64_t* err_code,
                                          char* err_msg,
                                          size_t err_msg_size,
                                          Value* err_data);

static bool json_decode_copy_value(const Value* value, Value* out) {
    if (!value || !out) return false;
    *out = *value;
    value_retain(out);
    return true;
}

static bool json_decode_by_string_schema(VM* vm,
                                         const Value* value,
                                         const char* schema_name,
                                         const char* path,
                                         const char* schema_path,
                                         Value* out,
                                         int64_t* err_code,
                                         char* err_msg,
                                         size_t err_msg_size,
                                         Value* err_data) {
    if (!schema_name || schema_name[0] == '\0') {
        return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, schema_path, "schema string is empty", err_data);
    }

    size_t schema_len = strlen(schema_name);
    bool nullable = false;
    if (schema_len > 1 && schema_name[schema_len - 1] == '?') {
        nullable = true;
        schema_len--;
    }

    char base[64];
    if (schema_len >= sizeof(base)) {
        return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, schema_path, "schema string is too long", err_data);
    }
    memcpy(base, schema_name, schema_len);
    base[schema_len] = '\0';

    if (nullable && value_get_type(value) == VAL_NIL) {
        value_init_nil(out);
        return true;
    }

    if (strcmp(base, "any") == 0) {
        return json_decode_copy_value(value, out);
    }

    if (strcmp(base, "int") == 0) {
        if (value_get_type(value) != VAL_INT) {
            return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, path, schema_path, "int", value, err_data);
        }
        return json_decode_copy_value(value, out);
    }

    if (strcmp(base, "double") == 0) {
        if (value_get_type(value) == VAL_DOUBLE) {
            return json_decode_copy_value(value, out);
        }
        if (value_get_type(value) == VAL_INT) {
            value_init_double(out, (double)value_get_int(value));
            return true;
        }
        return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, path, schema_path, "double", value, err_data);
    }

    if (strcmp(base, "number") == 0) {
        if (value_get_type(value) != VAL_INT && value_get_type(value) != VAL_DOUBLE) {
            return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, path, schema_path, "number", value, err_data);
        }
        return json_decode_copy_value(value, out);
    }

    if (strcmp(base, "bool") == 0) {
        if (value_get_type(value) != VAL_BOOL) {
            return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, path, schema_path, "bool", value, err_data);
        }
        return json_decode_copy_value(value, out);
    }

    if (strcmp(base, "string") == 0) {
        if (value_get_type(value) != VAL_STRING) {
            return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, path, schema_path, "string", value, err_data);
        }
        return json_decode_copy_value(value, out);
    }

    if (strcmp(base, "bytes") == 0) {
        if (value_get_type(value) == VAL_BYTES) {
            return json_decode_copy_value(value, out);
        }
        ObjArray* value_arr = value_get_array_obj(value);
        if (value_get_type(value) == VAL_ARRAY && value_arr) {
            ObjArray* in_arr = value_arr;
            ObjBytes* out_bytes = obj_bytes_create_with_size(in_arr->count, 0);
            uint8_t* out_data = obj_bytes_data(out_bytes);
            if (!out_data && in_arr->count > 0) {
                obj_bytes_release(out_bytes);
                return json_decode_fail_schema(vm,
                                               err_code,
                                               err_msg,
                                               err_msg_size,
                                               path,
                                               schema_path,
                                               "failed to allocate bytes buffer",
                                               err_data);
            }
            for (int i = 0; i < in_arr->count; i++) {
                Value elem;
                obj_array_get(in_arr, i, &elem);

                char child_path[512];
                json_decode_path_index(path, i, child_path, sizeof(child_path));

                if (value_get_type(&elem) != VAL_INT) {
                    obj_bytes_release(out_bytes);
                    bool fail = json_decode_fail_type(vm,
                                                      err_code,
                                                      err_msg,
                                                      err_msg_size,
                                                      child_path,
                                                      schema_path,
                                                      "byte (int 0..255)",
                                                      &elem,
                                                      err_data);
                    return fail;
                }

                int64_t elem_int = value_get_int(&elem);
                if (elem_int < 0 || elem_int > 255) {
                    if (err_code) *err_code = ERR_PARSE;
                    if (err_msg && err_msg_size > 0) {
                        snprintf(err_msg,
                                 err_msg_size,
                                 "jsonDecode range mismatch at %s: expected byte 0..255, got %lld",
                                 child_path,
                                 (long long)elem_int);
                    }
                    if (err_data) {
                        char actual_buf[64];
                        snprintf(actual_buf, sizeof(actual_buf), "%lld", (long long)elem_int);
                        value_free(err_data);
                        *err_data = json_decode_make_error_data_ext(vm,
                                                                    child_path,
                                                                    schema_path,
                                                                    "byte (0..255)",
                                                                    actual_buf,
                                                                    "range_error",
                                                                    "byte value must be between 0 and 255");
                    }
                    obj_bytes_release(out_bytes);
                    return false;
                }

                out_data[i] = (uint8_t)elem_int;
            }

            value_init_bytes(out, out_bytes);
            return true;
        }
        return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, path, schema_path, "bytes or array<int>", value, err_data);
    }

    if (strcmp(base, "nil") == 0) {
        if (value_get_type(value) != VAL_NIL) {
            return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, path, schema_path, "nil", value, err_data);
        }
        value_init_nil(out);
        return true;
    }

    if (strcmp(base, "array") == 0) {
        if (value_get_type(value) != VAL_ARRAY) {
            return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, path, schema_path, "array", value, err_data);
        }
        return json_decode_copy_value(value, out);
    }

    if (strcmp(base, "map") == 0 || strcmp(base, "record") == 0 || strcmp(base, "object") == 0) {
        if (value_get_type(value) != VAL_MAP) {
            return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, path, schema_path, "map", value, err_data);
        }
        return json_decode_copy_value(value, out);
    }

    return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, schema_path, "unknown schema type", err_data);
}

static bool json_decode_record_object(VM* vm,
                                      const Value* value,
                                      ObjMap* fields_schema,
                                      bool allow_extra,
                                      const char* path,
                                      const char* schema_fields_path,
                                      int depth,
                                      Value* out,
                                      int64_t* err_code,
                                      char* err_msg,
                                      size_t err_msg_size,
    Value* err_data) {
    ObjMap* in_map = value_get_map_obj(value);
    if (value_get_type(value) != VAL_MAP || !in_map) {
        return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, path, schema_fields_path, "map", value, err_data);
    }
    ObjMap* out_map = obj_map_create(vm);

    if (!fields_schema || !fields_schema->slots) {
        obj_map_free(out_map);
        return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, schema_fields_path, "record schema fields must be map", err_data);
    }

    for (int i = 0; i < fields_schema->capacity; i++) {
        MapSlot* schema_slot = &fields_schema->slots[i];
        if (schema_slot->hash < 2) continue;

        ObjString* schema_key_str = value_get_string_obj(&schema_slot->key);
        if (value_get_type(&schema_slot->key) != VAL_STRING || !schema_key_str || !schema_key_str->chars) {
            obj_map_free(out_map);
            return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, schema_fields_path, "record schema field names must be string", err_data);
        }

        char field_path[512];
        json_decode_path_field(path, schema_key_str->chars, field_path, sizeof(field_path));
        char field_schema_path[512];
        json_decode_path_field(schema_fields_path, schema_key_str->chars, field_schema_path, sizeof(field_schema_path));

        if (!obj_map_has(in_map, schema_slot->key)) {
            if (err_code) *err_code = ERR_PARSE;
            if (err_msg && err_msg_size > 0) {
                snprintf(err_msg, err_msg_size, "jsonDecode missing required field at %s", field_path);
            }
            if (err_data) {
                value_free(err_data);
                *err_data = json_decode_make_error_data_ext(vm,
                                                            field_path,
                                                            field_schema_path,
                                                            "present",
                                                            "missing",
                                                            "missing_field",
                                                            "required field is missing");
            }
            obj_map_free(out_map);
            return false;
        }

        Value raw_field = obj_map_get(in_map, schema_slot->key);
        Value decoded_field;
        value_init_nil(&decoded_field);
        bool ok = json_decode_value_with_schema(vm,
                                                &raw_field,
                                                &schema_slot->value,
                                                field_path,
                                                field_schema_path,
                                                depth + 1,
                                                &decoded_field,
                                                err_code,
                                                err_msg,
                                                err_msg_size,
                                                err_data);
        value_free(&raw_field);
        if (!ok) {
            value_free(&decoded_field);
            obj_map_free(out_map);
            return false;
        }

        obj_map_set(out_map, schema_slot->key, decoded_field);
        value_free(&decoded_field);
    }

    if (!allow_extra) {
        for (int i = 0; i < in_map->capacity; i++) {
            MapSlot* in_slot = &in_map->slots[i];
            if (in_slot->hash < 2) continue;

            ObjString* in_key_str = value_get_string_obj(&in_slot->key);
            if (value_get_type(&in_slot->key) != VAL_STRING || !in_key_str || !in_key_str->chars) {
                obj_map_free(out_map);
                return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, path, schema_fields_path, "string key", &in_slot->key, err_data);
            }

            if (!obj_map_has(fields_schema, in_slot->key)) {
                char field_path[512];
                json_decode_path_field(path, in_key_str->chars, field_path, sizeof(field_path));
                if (err_code) *err_code = ERR_PARSE;
                if (err_msg && err_msg_size > 0) {
                    snprintf(err_msg, err_msg_size, "jsonDecode unexpected field at %s", field_path);
                }
                if (err_data) {
                    value_free(err_data);
                    *err_data = json_decode_make_error_data_ext(vm,
                                                                field_path,
                                                                schema_fields_path,
                                                                "<no extra fields>",
                                                                json_value_type_name(value_get_type(&in_slot->value)),
                                                                "unexpected_field",
                                                                "field is not declared in schema");
                }
                obj_map_free(out_map);
                return false;
            }
        }
    }

    value_init_map(out, out_map);
    return true;
}

static bool json_decode_value_with_schema(VM* vm,
                                          const Value* value,
                                          const Value* schema,
                                          const char* path,
                                          const char* schema_path,
                                          int depth,
                                          Value* out,
                                          int64_t* err_code,
                                          char* err_msg,
                                          size_t err_msg_size,
                                          Value* err_data) {
    if (!vm || !value || !schema || !out) {
        return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, schema_path, "internal null argument", err_data);
    }

    if (depth > CJSON_NESTING_LIMIT) {
        if (err_code) *err_code = ERR_LIMIT;
        if (err_msg && err_msg_size > 0) {
            snprintf(err_msg, err_msg_size, "jsonDecode nesting limit exceeded at %s", (path && path[0] != '\0') ? path : "$");
        }
        if (err_data) {
            value_free(err_data);
            *err_data = json_decode_make_error_data_ext(vm,
                                                        (path && path[0] != '\0') ? path : "$",
                                                        (schema_path && schema_path[0] != '\0') ? schema_path : "$",
                                                        "nesting depth <= limit",
                                                        "limit exceeded",
                                                        "limit_exceeded",
                                                        "jsonDecode nesting limit exceeded");
        }
        return false;
    }

    if (value_get_type(schema) == VAL_STRING) {
        ObjString* schema_name_obj = value_get_string_obj(schema);
        const char* schema_name = (schema_name_obj && schema_name_obj->chars) ? schema_name_obj->chars : "";
        return json_decode_by_string_schema(vm,
                                            value,
                                            schema_name,
                                            path,
                                            schema_path,
                                            out,
                                            err_code,
                                            err_msg,
                                            err_msg_size,
                                            err_data);
    }

    ObjMap* schema_map = value_get_map_obj(schema);
    if (value_get_type(schema) != VAL_MAP || !schema_map) {
        return json_decode_fail_schema(vm,
                                       err_code,
                                       err_msg,
                                       err_msg_size,
                                       path,
                                       schema_path,
                                       "schema must be string or map descriptor",
                                       err_data);
    }

    Value type_val;
    bool has_type = false;
    map_lookup_string_key(schema_map, "type", &type_val, &has_type);

    if (!has_type) {
        return json_decode_record_object(vm,
                                         value,
                                         schema_map,
                                         false,
                                         path,
                                         schema_path,
                                         depth,
                                         out,
                                         err_code,
                                         err_msg,
                                         err_msg_size,
                                         err_data);
    }

    ObjString* type_str = value_get_string_obj(&type_val);
    if (value_get_type(&type_val) != VAL_STRING || !type_str || !type_str->chars) {
        value_free(&type_val);
        return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, schema_path, "schema.type must be string", err_data);
    }

    const char* kind = type_str->chars;

    if (strcmp(kind, "nullable") == 0) {
        Value inner;
        bool has_inner = false;
        map_lookup_string_key(schema_map, "inner", &inner, &has_inner);
        value_free(&type_val);
        char inner_schema_path[512];
        json_decode_path_field(schema_path, "inner", inner_schema_path, sizeof(inner_schema_path));
        if (!has_inner) {
            return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, inner_schema_path, "nullable schema requires inner", err_data);
        }
        if (value_get_type(value) == VAL_NIL) {
            value_free(&inner);
            value_init_nil(out);
            return true;
        }
        bool ok = json_decode_value_with_schema(vm,
                                                value,
                                                &inner,
                                                path,
                                                inner_schema_path,
                                                depth + 1,
                                                out,
                                                err_code,
                                                err_msg,
                                                err_msg_size,
                                                err_data);
        value_free(&inner);
        return ok;
    }

    if (strcmp(kind, "literal") == 0) {
        Value literal_value;
        bool has_literal = false;
        map_lookup_string_key(schema_map, "value", &literal_value, &has_literal);
        value_free(&type_val);

        char literal_schema_path[512];
        json_decode_path_field(schema_path, "value", literal_schema_path, sizeof(literal_schema_path));
        if (!has_literal) {
            return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, literal_schema_path, "literal schema requires value", err_data);
        }

        bool eq = json_decode_deep_equal(value, &literal_value, 0);
        if (eq) {
            value_free(&literal_value);
            return json_decode_copy_value(value, out);
        }

        if (err_code) *err_code = ERR_PARSE;
        if (err_msg && err_msg_size > 0) {
            snprintf(err_msg, err_msg_size, "jsonDecode literal mismatch at %s", (path && path[0] != '\0') ? path : "$");
        }
        if (err_data) {
            char expected_buf[128];
            char actual_buf[128];
            json_decode_describe_value(&literal_value, expected_buf, sizeof(expected_buf));
            json_decode_describe_value(value, actual_buf, sizeof(actual_buf));
            value_free(err_data);
            *err_data = json_decode_make_error_data_ext(vm,
                                                        (path && path[0] != '\0') ? path : "$",
                                                        literal_schema_path,
                                                        expected_buf,
                                                        actual_buf,
                                                        "literal_mismatch",
                                                        "value does not match required literal");
        }
        value_free(&literal_value);
        return false;
    }

    if (strcmp(kind, "enum") == 0) {
        Value enum_values;
        bool has_values = false;
        map_lookup_string_key(schema_map, "values", &enum_values, &has_values);
        value_free(&type_val);

        char enum_values_schema_path[512];
        json_decode_path_field(schema_path, "values", enum_values_schema_path, sizeof(enum_values_schema_path));
        if (!has_values) {
            return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, enum_values_schema_path, "enum schema requires values", err_data);
        }
        ObjArray* opts = value_get_array_obj(&enum_values);
        if (value_get_type(&enum_values) != VAL_ARRAY || !opts || opts->count <= 0) {
            value_free(&enum_values);
            return json_decode_fail_schema(vm,
                                           err_code,
                                           err_msg,
                                           err_msg_size,
                                           path,
                                           enum_values_schema_path,
                                           "enum values must be a non-empty array",
                                           err_data);
        }

        bool matched = false;
        for (int i = 0; i < opts->count; i++) {
            Value opt;
            obj_array_get(opts, i, &opt);
            bool eq = json_decode_deep_equal(value, &opt, 0);
            if (eq) {
                matched = true;
                break;
            }
        }
        value_free(&enum_values);

        if (matched) {
            return json_decode_copy_value(value, out);
        }

        if (err_code) *err_code = ERR_PARSE;
        if (err_msg && err_msg_size > 0) {
            snprintf(err_msg,
                     err_msg_size,
                     "jsonDecode enum mismatch at %s: value did not match any enum option",
                     (path && path[0] != '\0') ? path : "$");
        }
        if (err_data) {
            char actual_buf[128];
            json_decode_describe_value(value, actual_buf, sizeof(actual_buf));
            value_free(err_data);
            *err_data = json_decode_make_error_data_ext(vm,
                                                        (path && path[0] != '\0') ? path : "$",
                                                        enum_values_schema_path,
                                                        "one of enum values",
                                                        actual_buf,
                                                        "enum_mismatch",
                                                        "value did not match any enum literal");
        }
        return false;
    }

    if (strcmp(kind, "oneOf") == 0) {
        Value variants;
        bool has_variants = false;
        map_lookup_string_key(schema_map, "variants", &variants, &has_variants);
        value_free(&type_val);

        char variants_schema_path[512];
        json_decode_path_field(schema_path, "variants", variants_schema_path, sizeof(variants_schema_path));
        if (!has_variants) {
            return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, variants_schema_path, "oneOf schema requires variants", err_data);
        }
        ObjArray* variant_arr = value_get_array_obj(&variants);
        if (value_get_type(&variants) != VAL_ARRAY || !variant_arr || variant_arr->count <= 0) {
            value_free(&variants);
            return json_decode_fail_schema(vm,
                                           err_code,
                                           err_msg,
                                           err_msg_size,
                                           path,
                                           variants_schema_path,
                                           "oneOf variants must be a non-empty array",
                                           err_data);
        }

        bool matched = false;
        int64_t first_code = ERR_PARSE;
        char first_msg[256];
        first_msg[0] = '\0';
        bool have_first = false;
        Value first_err_data;
        value_init_nil(&first_err_data);

        for (int i = 0; i < variant_arr->count; i++) {
            Value variant_schema;
            obj_array_get(variant_arr, i, &variant_schema);

            char variant_schema_path[512];
            json_decode_path_index(variants_schema_path, i, variant_schema_path, sizeof(variant_schema_path));

            Value candidate;
            value_init_nil(&candidate);
            Value variant_err_data;
            value_init_nil(&variant_err_data);
            int64_t variant_code = 0;
            char variant_msg[256];
            variant_msg[0] = '\0';

            bool ok = json_decode_value_with_schema(vm,
                                                    value,
                                                    &variant_schema,
                                                    path,
                                                    variant_schema_path,
                                                    depth + 1,
                                                    &candidate,
                                                    &variant_code,
                                                    variant_msg,
                                                    sizeof(variant_msg),
                                                    &variant_err_data);

            if (ok) {
                value_free(&variant_err_data);
                value_free(&first_err_data);
                value_free(&variants);
                *out = candidate;
                matched = true;
                break;
            }

            if (!have_first) {
                have_first = true;
                first_code = variant_code ? variant_code : ERR_PARSE;
                snprintf(first_msg, sizeof(first_msg), "%s", variant_msg[0] ? variant_msg : "variant mismatch");
                first_err_data = variant_err_data;
                value_init_nil(&variant_err_data);
            }

            value_free(&candidate);
            value_free(&variant_err_data);
        }

        if (matched) {
            return true;
        }

        if (err_code) *err_code = first_code ? first_code : ERR_PARSE;
        if (err_msg && err_msg_size > 0) {
            snprintf(err_msg,
                     err_msg_size,
                     "jsonDecode oneOf mismatch at %s: none of %d variants matched",
                     (path && path[0] != '\0') ? path : "$",
                     variant_arr->count);
        }
        if (err_data) {
            char actual_buf[128];
            char detail_buf[320];
            json_decode_describe_value(value, actual_buf, sizeof(actual_buf));
            snprintf(detail_buf,
                     sizeof(detail_buf),
                     "none of %d variants matched; first error: %s",
                     variant_arr->count,
                     first_msg[0] ? first_msg : "variant mismatch");
            value_free(err_data);
            *err_data = json_decode_make_error_data_ext(vm,
                                                        (path && path[0] != '\0') ? path : "$",
                                                        variants_schema_path,
                                                        "one matching variant",
                                                        actual_buf,
                                                        "no_variant_match",
                                                        detail_buf);
        }
        value_free(&first_err_data);
        value_free(&variants);
        return false;
    }

    if (strcmp(kind, "array") == 0) {
        Value items_schema;
        bool has_items = false;
        map_lookup_string_key(schema_map, "items", &items_schema, &has_items);
        value_free(&type_val);
        char items_schema_path[512];
        json_decode_path_field(schema_path, "items", items_schema_path, sizeof(items_schema_path));
        if (!has_items) {
            return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, items_schema_path, "array schema requires items", err_data);
        }
        ObjArray* in_arr = value_get_array_obj(value);
        if (value_get_type(value) != VAL_ARRAY || !in_arr) {
            value_free(&items_schema);
            return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, path, schema_path, "array", value, err_data);
        }

        ObjArray* out_arr = obj_array_create(vm, in_arr->count > 0 ? in_arr->count : 4);

        for (int i = 0; i < in_arr->count; i++) {
            Value elem;
            obj_array_get(in_arr, i, &elem);

            char child_path[512];
            json_decode_path_index(path, i, child_path, sizeof(child_path));

            Value decoded_elem;
            value_init_nil(&decoded_elem);
            if (!json_decode_value_with_schema(vm,
                                               &elem,
                                               &items_schema,
                                               child_path,
                                               items_schema_path,
                                               depth + 1,
                                               &decoded_elem,
                                               err_code,
                                               err_msg,
                                               err_msg_size,
                                               err_data)) {
                value_free(&decoded_elem);
                obj_array_free(out_arr);
                value_free(&items_schema);
                return false;
            }

            obj_array_push(out_arr, decoded_elem);
        }

        value_free(&items_schema);
        value_init_array(out, out_arr);
        return true;
    }

    if (strcmp(kind, "map") == 0) {
        Value values_schema;
        bool has_values = false;
        map_lookup_string_key(schema_map, "values", &values_schema, &has_values);
        char values_schema_path[512];
        json_decode_path_field(schema_path, "values", values_schema_path, sizeof(values_schema_path));
        if (!has_values) {
            value_free(&type_val);
            return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, values_schema_path, "map schema requires values", err_data);
        }

        bool keys_as_int = false;
        Value keys_val;
        bool has_keys = false;
        map_lookup_string_key(schema_map, "keys", &keys_val, &has_keys);
        char keys_schema_path[512];
        json_decode_path_field(schema_path, "keys", keys_schema_path, sizeof(keys_schema_path));
        if (has_keys) {
            ObjString* keys_type_str = value_get_string_obj(&keys_val);
            if (value_get_type(&keys_val) != VAL_STRING || !keys_type_str || !keys_type_str->chars) {
                value_free(&keys_val);
                value_free(&values_schema);
                value_free(&type_val);
                return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, keys_schema_path, "map schema keys must be \"string\" or \"int\"", err_data);
            }
            if (strcmp(keys_type_str->chars, "string") == 0) {
                keys_as_int = false;
            } else if (strcmp(keys_type_str->chars, "int") == 0) {
                keys_as_int = true;
            } else {
                value_free(&keys_val);
                value_free(&values_schema);
                value_free(&type_val);
                return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, keys_schema_path, "map schema keys must be \"string\" or \"int\"", err_data);
            }
            value_free(&keys_val);
        }
        value_free(&type_val);

        ObjMap* in_map = value_get_map_obj(value);
        if (value_get_type(value) != VAL_MAP || !in_map) {
            value_free(&values_schema);
            return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, path, schema_path, "map", value, err_data);
        }

        ObjMap* out_map = obj_map_create(vm);

        for (int i = 0; i < in_map->capacity; i++) {
            MapSlot* in_slot = &in_map->slots[i];
            if (in_slot->hash < 2) continue;

            char child_path[512];
            json_decode_path_for_key(path, &in_slot->key, child_path, sizeof(child_path));

            Value out_key;
            value_init_nil(&out_key);

            if (keys_as_int) {
                ObjString* in_slot_key_str = value_get_string_obj(&in_slot->key);
                if (value_get_type(&in_slot->key) == VAL_INT) {
                    value_init_int(&out_key, value_get_int(&in_slot->key));
                } else if (value_get_type(&in_slot->key) == VAL_STRING && in_slot_key_str && in_slot_key_str->chars) {
                    int64_t parsed_key = 0;
                    const char* key_text = in_slot_key_str->chars;
                    if (!json_parse_int64_strict(key_text, &parsed_key)) {
                        if (err_code) *err_code = ERR_PARSE;
                        if (err_msg && err_msg_size > 0) {
                            snprintf(err_msg,
                                     err_msg_size,
                                     "jsonDecode invalid int key at %s: \"%s\"",
                                     child_path,
                                     key_text);
                        }
                        if (err_data) {
                            value_free(err_data);
                            *err_data = json_decode_make_error_data_ext(vm,
                                                                        child_path,
                                                                        keys_schema_path,
                                                                        "int key",
                                                                        key_text,
                                                                        "invalid_key",
                                                                        "map key is not a valid decimal int literal");
                        }
                        obj_map_free(out_map);
                        value_free(&values_schema);
                        return false;
                    }
                    value_init_int(&out_key, parsed_key);
                } else {
                    obj_map_free(out_map);
                    value_free(&values_schema);
                    return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, child_path, keys_schema_path, "int key", &in_slot->key, err_data);
                }
            } else {
                ObjString* in_slot_key_str = value_get_string_obj(&in_slot->key);
                if (value_get_type(&in_slot->key) != VAL_STRING || !in_slot_key_str || !in_slot_key_str->chars) {
                    obj_map_free(out_map);
                    value_free(&values_schema);
                    return json_decode_fail_type(vm, err_code, err_msg, err_msg_size, child_path, keys_schema_path, "string key", &in_slot->key, err_data);
                }
                out_key = in_slot->key;
                value_retain(&out_key);
            }

            if (obj_map_has(out_map, out_key)) {
                if (err_code) *err_code = ERR_PARSE;
                if (err_msg && err_msg_size > 0) {
                    snprintf(err_msg,
                             err_msg_size,
                             "jsonDecode duplicate key after coercion at %s",
                             child_path);
                }
                if (err_data) {
                    char actual_buf[64];
                    if (value_get_type(&out_key) == VAL_INT) {
                        snprintf(actual_buf, sizeof(actual_buf), "%lld", (long long)value_get_int(&out_key));
                    } else if (value_get_type(&out_key) == VAL_STRING &&
                               value_get_string_obj(&out_key) &&
                               value_get_string_obj(&out_key)->chars) {
                        snprintf(actual_buf, sizeof(actual_buf), "%s", value_get_string_obj(&out_key)->chars);
                    } else {
                        snprintf(actual_buf, sizeof(actual_buf), "unknown");
                    }
                    value_free(err_data);
                    *err_data = json_decode_make_error_data_ext(vm,
                                                                child_path,
                                                                keys_schema_path,
                                                                "unique map keys",
                                                                actual_buf,
                                                                "duplicate_key",
                                                                "multiple input keys normalized to the same key");
                }
                value_free(&out_key);
                obj_map_free(out_map);
                value_free(&values_schema);
                return false;
            }

            Value decoded_val;
            value_init_nil(&decoded_val);
            if (!json_decode_value_with_schema(vm,
                                               &in_slot->value,
                                               &values_schema,
                                               child_path,
                                               values_schema_path,
                                               depth + 1,
                                               &decoded_val,
                                               err_code,
                                               err_msg,
                                               err_msg_size,
                                               err_data)) {
                value_free(&decoded_val);
                value_free(&out_key);
                obj_map_free(out_map);
                value_free(&values_schema);
                return false;
            }

            obj_map_set(out_map, out_key, decoded_val);
            value_free(&decoded_val);
            value_free(&out_key);
        }

        value_free(&values_schema);
        value_init_map(out, out_map);
        return true;
    }

    if (strcmp(kind, "record") == 0 || strcmp(kind, "object") == 0) {
        Value fields_val;
        bool has_fields = false;
        map_lookup_string_key(schema_map, "fields", &fields_val, &has_fields);
        char fields_schema_path[512];
        json_decode_path_field(schema_path, "fields", fields_schema_path, sizeof(fields_schema_path));

        bool allow_extra = false;
        Value allow_extra_val;
        bool has_allow_extra = false;
        map_lookup_string_key(schema_map, "allowExtra", &allow_extra_val, &has_allow_extra);
        char allow_extra_schema_path[512];
        json_decode_path_field(schema_path, "allowExtra", allow_extra_schema_path, sizeof(allow_extra_schema_path));

        value_free(&type_val);

        if (has_allow_extra) {
            if (value_get_type(&allow_extra_val) != VAL_BOOL) {
                value_free(&allow_extra_val);
                if (has_fields) value_free(&fields_val);
                return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, allow_extra_schema_path, "record schema allowExtra must be bool", err_data);
            }
            allow_extra = value_get_bool(&allow_extra_val);
            value_free(&allow_extra_val);
        }

        if (!has_fields) {
            return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, fields_schema_path, "record schema requires fields", err_data);
        }
        ObjMap* fields_schema_map = value_get_map_obj(&fields_val);
        if (value_get_type(&fields_val) != VAL_MAP || !fields_schema_map) {
            value_free(&fields_val);
            return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, fields_schema_path, "record schema fields must be map", err_data);
        }

        bool ok = json_decode_record_object(vm,
                                            value,
                                            fields_schema_map,
                                            allow_extra,
                                            path,
                                            fields_schema_path,
                                            depth + 1,
                                            out,
                                            err_code,
                                            err_msg,
                                            err_msg_size,
                                            err_data);
        value_free(&fields_val);
        return ok;
    }

    value_free(&type_val);
    return json_decode_fail_schema(vm, err_code, err_msg, err_msg_size, path, schema_path, "unknown descriptor type", err_data);
}

void builtin_print(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    value_print(vm, &val);

    Value nil;
    value_init_nil(&nil);
    value_free(slot);
    *slot = nil;
}

void builtin_println(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    value_print(vm, &val);
    if (vm->output_callback) {
        vm->output_callback(vm->output_callback_user_data, "\n", 1);
    } else {
        printf("\n");
    }

    Value nil;
    value_init_nil(&nil);
    value_free(slot);
    *slot = nil;
}

void builtin_panic(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value msg_val = *slot;

    ObjString* msg_str = value_get_string_obj(&msg_val);
    if (value_get_type(&msg_val) == VAL_STRING && msg_str && msg_str->chars) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "Panic: %s", msg_str->chars);
        vm_runtime_error(vm, buf);
    } else {
        vm_runtime_error(vm, "Panic");
    }

    Value nil;
    value_init_nil(&nil);
    value_free(slot);
    *slot = nil;
}

void builtin_must(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;

    ObjTuple* tuple = value_get_tuple_obj(&val);
    if (value_get_type(&val) != VAL_TUPLE || !tuple || tuple->element_count != 2) {
        vm_runtime_error(vm, "must expects (value, Error?) tuple");
        Value nil;
        value_init_nil(&nil);
        value_free(slot);
        *slot = nil;
        return;
    }

    Value ok;
    obj_tuple_get(tuple, 0, &ok);
    Value err;
    obj_tuple_get(tuple, 1, &err);

    if (value_get_type(&err) == VAL_NIL) {
        value_free(slot);
        *slot = ok;
        value_free(&err);
        return;
    }

    char buf[1024];
    buf[0] = '\0';

    ObjRecord* err_record = value_get_record_obj(&err);
    if (value_get_type(&err) == VAL_RECORD && err_record) {
        Value msg_val;
        obj_record_get_field(err_record, 1, &msg_val);
        ObjString* msg_str = value_get_string_obj(&msg_val);
        if (value_get_type(&msg_val) == VAL_STRING && msg_str && msg_str->chars && msg_str->chars[0] != '\0') {
            snprintf(buf, sizeof(buf), "must failed: %s", msg_str->chars);
        } else {
            snprintf(buf, sizeof(buf), "must failed");
        }
        value_free(&msg_val);
    } else {
        ObjString* err_str = value_get_string_obj(&err);
        if (value_get_type(&err) == VAL_STRING && err_str && err_str->chars && err_str->chars[0] != '\0') {
            snprintf(buf, sizeof(buf), "must failed: %s", err_str->chars);
        } else {
            snprintf(buf, sizeof(buf), "must failed");
        }
    }

    vm_runtime_error(vm, buf);

    Value nil;
    value_init_nil(&nil);
    value_free(slot);
    *slot = nil;

    value_free(&ok);
    value_free(&err);
}

void builtin_wrap_error(VM* vm) {
    Value* err_slot = &vm->stack.values[vm->stack.count - 2];
    Value context_val = vm->stack.values[vm->stack.count - 1];
    Value err_val = *err_slot;

    if (value_get_type(&err_val) == VAL_NIL) {
        return;
    }

    ObjString* context_str = value_get_string_obj(&context_val);
    if (value_get_type(&context_val) != VAL_STRING) {
        vm_runtime_error(vm, "wrapError expects context:string");
        Value nil;
        value_init_nil(&nil);
        value_free(err_slot);
        *err_slot = nil;
        return;
    }

    ObjRecord* err_record = value_get_record_obj(&err_val);
    if (value_get_type(&err_val) != VAL_RECORD || !err_record || err_record->field_count < 3) {
        vm_runtime_error(vm, "wrapError expects Error?");
        Value nil;
        value_init_nil(&nil);
        value_free(err_slot);
        *err_slot = nil;
        return;
    }

    int64_t code = 0;
    Value code_val = err_record->fields[0];
    if (value_get_type(&code_val) == VAL_INT) {
        code = value_get_int(&code_val);
    }

    const char* msg = "";
    Value msg_val = err_record->fields[1];
    ObjString* msg_str = value_get_string_obj(&msg_val);
    if (value_get_type(&msg_val) == VAL_STRING && msg_str && msg_str->chars) {
        msg = msg_str->chars;
    }

    const char* ctx = (context_str && context_str->chars) ? context_str->chars : "";

    bool has_ctx = ctx && ctx[0] != '\0';
    bool has_msg = msg && msg[0] != '\0';

    char* combined = NULL;
    if (has_ctx && has_msg) {
        size_t ctx_len = strlen(ctx);
        size_t msg_len = strlen(msg);
        size_t total = ctx_len + 2 + msg_len;
        combined = (char*)safe_malloc(total + 1);
        snprintf(combined, total + 1, "%s: %s", ctx, msg);
    } else if (has_ctx) {
        combined = safe_strdup(ctx);
    } else {
        combined = safe_strdup(msg);
    }

    // Move the original error into Error.data as the cause.
    Value out;
    Value data = err_val;
    error_value_init(vm, &out, code, combined, data);
    if (combined) free(combined);

    *err_slot = out;
}

void builtin_len(VM* vm) {
    Value val = vm->stack.values[vm->stack.count - 1];
    Value result;
    
    if (value_get_type(&val) == VAL_ARRAY) {
        ObjArray* arr = value_get_array_obj(&val);
        value_init_int(&result, arr ? arr->count : 0);
    } else if (value_get_type(&val) == VAL_BYTES) {
        ObjBytes* bytes = value_get_bytes_obj(&val);
        value_init_int(&result, bytes ? bytes->length : 0);
    } else if (value_get_type(&val) == VAL_STRING) {
        ObjString* str = value_get_string_obj(&val);
        value_init_int(&result, str ? str->length : 0);
    } else {
        value_init_int(&result, 0);
    }
    
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    value_free(slot);
    *slot = result;
}

void builtin_typeOf(VM* vm) {
    Value val = vm->stack.values[vm->stack.count - 1];
    Value result;
    char* type_str;
    
    switch (value_get_type(&val)) {
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
            ObjRecord* record = value_get_record_obj(&val);
            if (value_is_future(&val)) {
                type_str = VM_FUTURE_RUNTIME_TYPE_NAME;
            } else {
                type_str = (record && record->type_name && record->type_name[0] != '\0')
                    ? record->type_name
                    : "record";
            }
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
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    value_free(slot);
    *slot = result;
}

static ObjRecord* builtin_expect_future(VM* vm, const Value* value, const char* builtin_name) {
    if (!value_is_future(value)) {
        char message[128];
        snprintf(message, sizeof(message), "%s expects Future<T>", builtin_name);
        vm_runtime_error(vm, message);
        return NULL;
    }

    return value_get_record_obj(value);
}

static void async_channel_wait_free(void* payload) {
    AsyncChannelWait* wait = (AsyncChannelWait*)payload;
    if (!wait) return;
    if (wait->future) {
        obj_record_release(wait->future);
    }
    syncx_value_free(&wait->payload);
    value_free(&wait->schema);
    free(wait);
}

static void async_channel_result_init(VM* vm, Value* out, Value ok_value, int64_t err_code, const char* err_msg) {
    Value err_data;
    value_init_nil(&err_data);
    result_tuple_init_data(vm, out, ok_value, err_code, err_msg, err_data);
}

static ObjRecord* async_channel_build_resolved_future(VM* vm, Value ok_value, int64_t err_code, const char* err_msg) {
    Value result;
    async_channel_result_init(vm, &result, ok_value, err_code, err_msg);
    ObjRecord* future = obj_future_create_resolved(vm, result);
    value_free(&result);
    return future;
}

static bool async_channel_complete_future(VM* vm, ObjRecord* future, Value ok_value, int64_t err_code, const char* err_msg) {
    Value result;
    async_channel_result_init(vm, &result, ok_value, err_code, err_msg);
    bool completed = vm_future_complete(vm, future, result);
    value_free(&result);
    return completed;
}

static ObjRecord* async_channel_error_future(VM* vm, bool ok_is_bool, bool ok_bool, int64_t err_code, const char* err_msg) {
    Value ok;
    if (ok_is_bool) {
        value_init_bool(&ok, ok_bool);
    } else {
        value_init_nil(&ok);
    }
    return async_channel_build_resolved_future(vm, ok, err_code, err_msg);
}

static bool async_channel_poll_waiter(VM* vm, void* payload) {
    AsyncChannelWait* wait = (AsyncChannelWait*)payload;
    if (!vm || !wait || !wait->future) return true;

    syncx_runtime_ensure_init();

    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxChannel* ch = syncx_channel_lookup_unlocked(wait->channel_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!ch) {
        if (wait->kind == ASYNC_CHANNEL_WAIT_SEND) {
            Value ok;
            value_init_bool(&ok, false);
            (void)async_channel_complete_future(vm, wait->future, ok, ERR_INVALID_ARGUMENT, "Unknown channel id");
        } else {
            Value ok;
            value_init_nil(&ok);
            (void)async_channel_complete_future(vm, wait->future, ok, ERR_INVALID_ARGUMENT, "Unknown channel id");
        }
        return true;
    }

    if (wait->kind == ASYNC_CHANNEL_WAIT_SEND) {
        int64_t err_code = 0;
        const char* err_msg = NULL;
        bool sent = syncx_channel_send_internal(ch, &wait->payload, 0, &err_code, &err_msg);
        if (!sent && err_code == ERR_LIMIT) {
            return false;
        }

        Value ok;
        value_init_bool(&ok, sent);
        if (sent) {
            (void)async_channel_complete_future(vm, wait->future, ok, 0, NULL);
        } else {
            (void)async_channel_complete_future(vm,
                                                wait->future,
                                                ok,
                                                err_code ? err_code : ERR_IO,
                                                err_msg ? err_msg : "Failed to send channel message");
        }
        return true;
    }

    SyncxValue message;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    bool got = syncx_channel_recv_internal(ch, 0, &message, &err_code, &err_msg);
    if (!got && err_code == ERR_LIMIT) {
        return false;
    }

    if (!got) {
        Value ok;
        value_init_nil(&ok);
        (void)async_channel_complete_future(vm,
                                            wait->future,
                                            ok,
                                            err_code ? err_code : ERR_IO,
                                            err_msg ? err_msg : "Failed to receive channel message");
        return true;
    }

    Value ok;
    bool decoded = false;
    if (wait->kind == ASYNC_CHANNEL_WAIT_RECV_TYPED) {
        char decode_err_msg[512];
        int64_t decode_err_code = 0;
        decoded = syncx_value_decode_typed(vm,
                                           &message,
                                           &wait->schema,
                                           &ok,
                                           &decode_err_code,
                                           decode_err_msg,
                                           sizeof(decode_err_msg));
        syncx_value_free(&message);
        if (!decoded) {
            Value nil_ok;
            value_init_nil(&nil_ok);
            (void)async_channel_complete_future(vm,
                                                wait->future,
                                                nil_ok,
                                                decode_err_code ? decode_err_code : ERR_INVALID_ARGUMENT,
                                                decode_err_msg[0] ? decode_err_msg : "Typed channel payload does not match schema");
            return true;
        }
    } else {
        decoded = syncx_value_to_vm_value(vm, &message, &ok);
        syncx_value_free(&message);
        if (!decoded) {
            Value nil_ok;
            value_init_nil(&nil_ok);
            (void)async_channel_complete_future(vm, wait->future, nil_ok, ERR_INTERNAL, "Failed to decode channel payload");
            return true;
        }
    }

    (void)async_channel_complete_future(vm, wait->future, ok, 0, NULL);
    return true;
}

static void async_channel_return_future(VM* vm, Value* slot, ObjRecord* future) {
    value_free(slot);
    value_init_record(slot, future);
}

void builtin_future_pending(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    ObjRecord* future = obj_future_create_pending(vm);
    if (!future) {
        vm_runtime_error(vm, "futurePending failed to allocate future");
        return;
    }

    value_free(slot);
    value_init_record(slot, future);
}

void builtin_future_resolved(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value input = *slot;
    ObjRecord* future = obj_future_create_resolved(vm, input);
    if (!future) {
        vm_runtime_error(vm, "futureResolved failed to allocate future");
        return;
    }

    value_free(slot);
    value_init_record(slot, future);
}

void builtin_future_is_ready(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value future_val = *slot;
    ObjRecord* future = builtin_expect_future(vm, &future_val, "futureIsReady");
    if (!future) return;

    Value result;
    value_init_bool(&result, obj_future_is_ready(future));
    value_free(slot);
    *slot = result;
}

void builtin_future_complete(VM* vm) {
    Value* future_slot = &vm->stack.values[vm->stack.count - 2];
    Value future_val = *future_slot;
    Value value_val = vm->stack.values[vm->stack.count - 1];
    ObjRecord* future = builtin_expect_future(vm, &future_val, "futureComplete");
    if (!future) return;

    Value result;
    value_init_bool(&result, vm_future_complete(vm, future, value_val));
    value_free(future_slot);
    *future_slot = result;
}

void builtin_future_get(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value future_val = *slot;
    ObjRecord* future = builtin_expect_future(vm, &future_val, "futureGet");
    if (!future) return;

    Value result;
    if (obj_future_try_get(future, &result)) {
        value_free(slot);
        *slot = result;
        return;
    }

    const char* panic_message = obj_future_get_panic_message(future);
    if (panic_message) {
        vm_runtime_error(vm, panic_message);
        return;
    }

    vm_runtime_error(vm, "futureGet called on a pending future");
}

void builtin_ext_posted_callback_pending_count(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value result;
    value_init_int(&result, (int64_t)vm_posted_event_queue_pending_count(vm));
    value_free(slot);
    *slot = result;
}

void builtin_ext_drain_posted_callbacks(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value max_count_val = *slot;
    int64_t max_count = 0;
    Value result;

    if (value_get_type(&max_count_val) != VAL_INT) {
        vm_runtime_error(vm, "extDrainPostedCallbacks expects int maxCount");
        return;
    }

    max_count = value_get_int(&max_count_val);
    if (max_count < 0 || max_count > INT_MAX) {
        vm_runtime_error(vm, "extDrainPostedCallbacks maxCount must be between 0 and INT_MAX");
        return;
    }

    value_init_int(&result, (int64_t)vm_drain_posted_event_queue(vm, (int)max_count));
    if (vm->error_occurred) {
        value_free(&result);
        return;
    }

    value_free(slot);
    *slot = result;
}

void builtin_ext_set_posted_callback_auto_drain(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value enabled_val = *slot;
    Value result;

    if (value_get_type(&enabled_val) != VAL_BOOL) {
        vm_runtime_error(vm, "extSetPostedCallbackAutoDrain expects bool enabled");
        return;
    }

    value_init_bool(&result, vm_set_posted_event_auto_drain(vm, value_get_bool(&enabled_val)));
    value_free(slot);
    *slot = result;
}

void builtin_async_sleep(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value duration_val = *slot;
    if (value_get_type(&duration_val) != VAL_INT) {
        vm_runtime_error(vm, "asyncSleep expects int milliseconds");
        return;
    }

    ObjRecord* future = vm_future_sleep(vm, value_get_int(&duration_val));
    if (!future) {
        vm_runtime_error(vm, "asyncSleep failed to allocate future");
        return;
    }

    value_free(slot);
    value_init_record(slot, future);
}

void builtin_async_channel_send(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value channel_id_val = vm->stack.values[vm->stack.count - 2];
    Value payload_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        ObjRecord* future = async_channel_error_future(vm, true, false, ERR_PERMISSION, "Threading runtime is disabled");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelSend failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    if (value_get_type(&channel_id_val) != VAL_INT) {
        ObjRecord* future = async_channel_error_future(vm, true, false, ERR_INVALID_ARGUMENT, "asyncChannelSend expects (int, any)");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelSend failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    SyncxValue payload;
    const char* conv_err = NULL;
    if (!syncx_value_from_vm_value(&payload_val, &payload, &conv_err)) {
        ObjRecord* future = async_channel_error_future(vm,
                                                       true,
                                                       false,
                                                       ERR_INVALID_ARGUMENT,
                                                       conv_err ? conv_err : "Unsupported payload type");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelSend failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    syncx_runtime_ensure_init();
    int channel_id = (int)value_get_int(&channel_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxChannel* ch = syncx_channel_lookup_unlocked(channel_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!ch) {
        syncx_value_free(&payload);
        ObjRecord* future = async_channel_error_future(vm, true, false, ERR_INVALID_ARGUMENT, "Unknown channel id");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelSend failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    int64_t err_code = 0;
    const char* err_msg = NULL;
    bool sent = syncx_channel_send_internal(ch, &payload, 0, &err_code, &err_msg);
    if (sent || err_code != ERR_LIMIT) {
        ObjRecord* future = async_channel_error_future(vm,
                                                       true,
                                                       sent,
                                                       sent ? 0 : (err_code ? err_code : ERR_IO),
                                                       sent ? NULL : (err_msg ? err_msg : "Failed to send channel message"));
        syncx_value_free(&payload);
        if (!future) {
            vm_runtime_error(vm, "asyncChannelSend failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    ObjRecord* future = obj_future_create_pending(vm);
    if (!future) {
        syncx_value_free(&payload);
        vm_runtime_error(vm, "asyncChannelSend failed to allocate future");
        return;
    }

    AsyncChannelWait* wait = (AsyncChannelWait*)safe_malloc(sizeof(AsyncChannelWait));
    wait->kind = ASYNC_CHANNEL_WAIT_SEND;
    wait->future = future;
    obj_record_retain(future);
    wait->channel_id = channel_id;
    wait->payload = payload;
    value_init_nil(&wait->schema);

    if (!vm_enqueue_poll_waiter(vm, async_channel_poll_waiter, async_channel_wait_free, wait)) {
        async_channel_wait_free(wait);
        vm_runtime_error(vm, "asyncChannelSend failed to register channel waiter");
        return;
    }

    async_channel_return_future(vm, slot, future);
}

void builtin_async_channel_send_typed(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value channel_id_val = vm->stack.values[vm->stack.count - 3];
    Value payload_val = vm->stack.values[vm->stack.count - 2];
    Value schema_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        ObjRecord* future = async_channel_error_future(vm, true, false, ERR_PERMISSION, "Threading runtime is disabled");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelSendTyped failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    if (value_get_type(&channel_id_val) != VAL_INT) {
        ObjRecord* future = async_channel_error_future(vm, true, false, ERR_INVALID_ARGUMENT, "asyncChannelSendTyped expects (int, any, any)");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelSendTyped failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    SyncxValue payload;
    syncx_value_init_nil(&payload);
    int64_t encode_err_code = 0;
    char encode_err_msg[512];
    if (!syncx_value_encode_typed(vm,
                                  &payload_val,
                                  &schema_val,
                                  &payload,
                                  &encode_err_code,
                                  encode_err_msg,
                                  sizeof(encode_err_msg))) {
        ObjRecord* future = async_channel_error_future(vm,
                                                       true,
                                                       false,
                                                       encode_err_code ? encode_err_code : ERR_INVALID_ARGUMENT,
                                                       encode_err_msg[0] ? encode_err_msg : "Payload does not match schema");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelSendTyped failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    syncx_runtime_ensure_init();
    int channel_id = (int)value_get_int(&channel_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxChannel* ch = syncx_channel_lookup_unlocked(channel_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!ch) {
        syncx_value_free(&payload);
        ObjRecord* future = async_channel_error_future(vm, true, false, ERR_INVALID_ARGUMENT, "Unknown channel id");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelSendTyped failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    int64_t err_code = 0;
    const char* err_msg = NULL;
    bool sent = syncx_channel_send_internal(ch, &payload, 0, &err_code, &err_msg);
    if (sent || err_code != ERR_LIMIT) {
        ObjRecord* future = async_channel_error_future(vm,
                                                       true,
                                                       sent,
                                                       sent ? 0 : (err_code ? err_code : ERR_IO),
                                                       sent ? NULL : (err_msg ? err_msg : "Failed to send typed channel message"));
        syncx_value_free(&payload);
        if (!future) {
            vm_runtime_error(vm, "asyncChannelSendTyped failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    ObjRecord* future = obj_future_create_pending(vm);
    if (!future) {
        syncx_value_free(&payload);
        vm_runtime_error(vm, "asyncChannelSendTyped failed to allocate future");
        return;
    }

    AsyncChannelWait* wait = (AsyncChannelWait*)safe_malloc(sizeof(AsyncChannelWait));
    wait->kind = ASYNC_CHANNEL_WAIT_SEND;
    wait->future = future;
    obj_record_retain(future);
    wait->channel_id = channel_id;
    wait->payload = payload;
    value_init_nil(&wait->schema);

    if (!vm_enqueue_poll_waiter(vm, async_channel_poll_waiter, async_channel_wait_free, wait)) {
        async_channel_wait_free(wait);
        vm_runtime_error(vm, "asyncChannelSendTyped failed to register channel waiter");
        return;
    }

    async_channel_return_future(vm, slot, future);
}

void builtin_async_channel_recv(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value channel_id_val = *slot;

    if (!vm_is_threading_enabled(vm)) {
        ObjRecord* future = async_channel_error_future(vm, false, false, ERR_PERMISSION, "Threading runtime is disabled");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelRecv failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    if (value_get_type(&channel_id_val) != VAL_INT) {
        ObjRecord* future = async_channel_error_future(vm, false, false, ERR_INVALID_ARGUMENT, "asyncChannelRecv expects (int)");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelRecv failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    syncx_runtime_ensure_init();
    int channel_id = (int)value_get_int(&channel_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxChannel* ch = syncx_channel_lookup_unlocked(channel_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!ch) {
        ObjRecord* future = async_channel_error_future(vm, false, false, ERR_INVALID_ARGUMENT, "Unknown channel id");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelRecv failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    SyncxValue message;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    bool got = syncx_channel_recv_internal(ch, 0, &message, &err_code, &err_msg);
    if (got) {
        Value ok;
        if (!syncx_value_to_vm_value(vm, &message, &ok)) {
            syncx_value_free(&message);
            ObjRecord* future = async_channel_error_future(vm, false, false, ERR_INTERNAL, "Failed to decode channel payload");
            if (!future) {
                vm_runtime_error(vm, "asyncChannelRecv failed to allocate future");
                return;
            }
            async_channel_return_future(vm, slot, future);
            return;
        }
        syncx_value_free(&message);
        ObjRecord* future = async_channel_build_resolved_future(vm, ok, 0, NULL);
        if (!future) {
            vm_runtime_error(vm, "asyncChannelRecv failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }
    if (err_code != ERR_LIMIT) {
        ObjRecord* future = async_channel_error_future(vm,
                                                       false,
                                                       false,
                                                       err_code ? err_code : ERR_IO,
                                                       err_msg ? err_msg : "Failed to receive channel message");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelRecv failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    ObjRecord* future = obj_future_create_pending(vm);
    if (!future) {
        vm_runtime_error(vm, "asyncChannelRecv failed to allocate future");
        return;
    }

    AsyncChannelWait* wait = (AsyncChannelWait*)safe_malloc(sizeof(AsyncChannelWait));
    wait->kind = ASYNC_CHANNEL_WAIT_RECV;
    wait->future = future;
    obj_record_retain(future);
    wait->channel_id = channel_id;
    syncx_value_init_nil(&wait->payload);
    value_init_nil(&wait->schema);

    if (!vm_enqueue_poll_waiter(vm, async_channel_poll_waiter, async_channel_wait_free, wait)) {
        async_channel_wait_free(wait);
        vm_runtime_error(vm, "asyncChannelRecv failed to register channel waiter");
        return;
    }

    async_channel_return_future(vm, slot, future);
}

void builtin_async_channel_recv_typed(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value channel_id_val = vm->stack.values[vm->stack.count - 2];
    Value schema_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        ObjRecord* future = async_channel_error_future(vm, false, false, ERR_PERMISSION, "Threading runtime is disabled");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelRecvTyped failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    if (value_get_type(&channel_id_val) != VAL_INT) {
        ObjRecord* future = async_channel_error_future(vm, false, false, ERR_INVALID_ARGUMENT, "asyncChannelRecvTyped expects (int, any)");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelRecvTyped failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    syncx_runtime_ensure_init();
    int channel_id = (int)value_get_int(&channel_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxChannel* ch = syncx_channel_lookup_unlocked(channel_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!ch) {
        ObjRecord* future = async_channel_error_future(vm, false, false, ERR_INVALID_ARGUMENT, "Unknown channel id");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelRecvTyped failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    SyncxValue message;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    bool got = syncx_channel_recv_internal(ch, 0, &message, &err_code, &err_msg);
    if (got) {
        Value ok;
        char decode_err_msg[512];
        int64_t decode_err_code = 0;
        if (!syncx_value_decode_typed(vm,
                                      &message,
                                      &schema_val,
                                      &ok,
                                      &decode_err_code,
                                      decode_err_msg,
                                      sizeof(decode_err_msg))) {
            syncx_value_free(&message);
            ObjRecord* future = async_channel_error_future(vm,
                                                           false,
                                                           false,
                                                           decode_err_code ? decode_err_code : ERR_INVALID_ARGUMENT,
                                                           decode_err_msg[0] ? decode_err_msg : "Typed channel payload does not match schema");
            if (!future) {
                vm_runtime_error(vm, "asyncChannelRecvTyped failed to allocate future");
                return;
            }
            async_channel_return_future(vm, slot, future);
            return;
        }
        syncx_value_free(&message);
        ObjRecord* future = async_channel_build_resolved_future(vm, ok, 0, NULL);
        if (!future) {
            vm_runtime_error(vm, "asyncChannelRecvTyped failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }
    if (err_code != ERR_LIMIT) {
        ObjRecord* future = async_channel_error_future(vm,
                                                       false,
                                                       false,
                                                       err_code ? err_code : ERR_IO,
                                                       err_msg ? err_msg : "Failed to receive typed channel message");
        if (!future) {
            vm_runtime_error(vm, "asyncChannelRecvTyped failed to allocate future");
            return;
        }
        async_channel_return_future(vm, slot, future);
        return;
    }

    ObjRecord* future = obj_future_create_pending(vm);
    if (!future) {
        vm_runtime_error(vm, "asyncChannelRecvTyped failed to allocate future");
        return;
    }

    AsyncChannelWait* wait = (AsyncChannelWait*)safe_malloc(sizeof(AsyncChannelWait));
    wait->kind = ASYNC_CHANNEL_WAIT_RECV_TYPED;
    wait->future = future;
    obj_record_retain(future);
    wait->channel_id = channel_id;
    syncx_value_init_nil(&wait->payload);
    wait->schema = schema_val;
    value_retain(&wait->schema);

    if (!vm_enqueue_poll_waiter(vm, async_channel_poll_waiter, async_channel_wait_free, wait)) {
        async_channel_wait_free(wait);
        vm_runtime_error(vm, "asyncChannelRecvTyped failed to register channel waiter");
        return;
    }

    async_channel_return_future(vm, slot, future);
}

void builtin_toInt(VM* vm) {
    Value val = vm->stack.values[vm->stack.count - 1];
    Value result;
    
    if (value_get_type(&val) == VAL_INT) {
        value_init_int(&result, value_get_int(&val));
    } else if (value_get_type(&val) == VAL_BOOL) {
        value_init_int(&result, value_get_bool(&val) ? 1 : 0);
    } else if (value_get_type(&val) == VAL_BIGINT) {
        ObjBigInt* bi = value_get_bigint_obj(&val);
        int64_t out = 0;
        if (obj_bigint_to_int64(bi, &out)) {
            value_init_int(&result, out);
        } else if (bi && bi->sign < 0) {
            value_init_int(&result, INT64_MIN);
        } else {
            value_init_int(&result, INT64_MAX);
        }
    } else if (value_get_type(&val) == VAL_DOUBLE) {
        double d = value_get_double(&val);
        if (isnan(d) || isinf(d)) {
            value_init_int(&result, 0);
        } else if (d > (double)INT64_MAX) {
            value_init_int(&result, INT64_MAX);
        } else if (d < (double)INT64_MIN) {
            value_init_int(&result, INT64_MIN);
        } else {
            value_init_int(&result, (int64_t)d);
        }
    } else if (value_get_type(&val) == VAL_STRING) {
        char* endptr;
        int base = 10;
        ObjString* str = value_get_string_obj(&val);
        const char* s = str ? str->chars : NULL;
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
    
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    value_free(slot);
    *slot = result;
}

void builtin_toDouble(VM* vm) {
    Value val = vm->stack.values[vm->stack.count - 1];
    Value result;
    
    if (value_get_type(&val) == VAL_INT) {
        value_init_double(&result, (double)value_get_int(&val));
    } else if (value_get_type(&val) == VAL_BOOL) {
        value_init_double(&result, value_get_bool(&val) ? 1.0 : 0.0);
    } else if (value_get_type(&val) == VAL_BIGINT) {
        double d = obj_bigint_to_double(value_get_bigint_obj(&val));
        if (isnan(d)) {
            value_init_double(&result, 0.0);
        } else {
            value_init_double(&result, d);
        }
    } else if (value_get_type(&val) == VAL_DOUBLE) {
        value_init_double(&result, value_get_double(&val));
    } else if (value_get_type(&val) == VAL_STRING) {
        char* endptr;
        errno = 0;
        ObjString* str = value_get_string_obj(&val);
        const char* text = (str && str->chars) ? str->chars : "";
        double value = strtod(text, &endptr);
        if (errno == ERANGE || *endptr != '\0') {
            value_init_double(&result, 0.0);
        } else {
            value_init_double(&result, value);
        }
    } else {
        value_init_double(&result, 0.0);
    }
    
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    value_free(slot);
    *slot = result;
}

void builtin_toBigInt(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;

    Value ok;
    const char* err_msg = NULL;

    if (value_get_type(&val) == VAL_BIGINT) {
        ok = val;
        value_retain(&ok);
        result_tuple_set(vm, slot, ok, 0, NULL);
        return;
    }

    ObjBigInt* bigint = NULL;
    if (value_get_type(&val) == VAL_INT) {
        bigint = obj_bigint_from_int64(value_get_int(&val));
    } else if (value_get_type(&val) == VAL_BOOL) {
        bigint = obj_bigint_from_int64(value_get_bool(&val) ? 1 : 0);
    } else if (value_get_type(&val) == VAL_DOUBLE) {
        double d = value_get_double(&val);
        if (isnan(d) || isinf(d)) {
            bigint = obj_bigint_from_int64(0);
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "%.0f", d);
            bigint = obj_bigint_from_string(buf);
        }
    } else if (value_get_type(&val) == VAL_STRING) {
        ObjString* str = value_get_string_obj(&val);
        if (!obj_bigint_try_from_string(str ? str->chars : NULL, &bigint)) {
            err_msg = "Invalid bigint literal";
            bigint = obj_bigint_from_int64(0);
        }
    } else {
        bigint = obj_bigint_from_int64(0);
    }

    value_init_bigint(&ok, bigint);
    if (err_msg) {
        Value data = val;
        value_retain(&data);
        result_tuple_set_data(vm, slot, ok, ERR_PARSE, err_msg, data);
        return;
    }

    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_toHexBigInt(VM* vm) {
    Value val = vm->stack.values[vm->stack.count - 1];
    Value result;

    ObjBigInt* val_bigint = value_get_bigint_obj(&val);
    if (value_get_type(&val) == VAL_BIGINT && val_bigint) {
        char* hex = obj_bigint_to_hex_string(val_bigint);
        value_init_string(&result, hex ? hex : "0x0");
        if (hex) free(hex);
    } else if (value_get_type(&val) == VAL_INT) {
        ObjBigInt* tmp = obj_bigint_from_int64(value_get_int(&val));
        char* hex = obj_bigint_to_hex_string(tmp);
        obj_bigint_release(tmp);
        value_init_string(&result, hex ? hex : "0x0");
        if (hex) free(hex);
    } else {
        value_init_string(&result, "0x0");
    }

    Value* slot = &vm->stack.values[vm->stack.count - 1];
    value_free(slot);
    *slot = result;
}

static int hex_nibble_value(char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

static bool url_is_unreserved_char(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= '0' && c <= '9') return true;
    return c == '-' || c == '.' || c == '_' || c == '~';
}

static char url_hex_upper(int value) {
    if (value >= 0 && value <= 9) return (char)('0' + value);
    return (char)('A' + (value - 10));
}

void builtin_bytesToHex(VM* vm) {
    static const char hex_digits[] = "0123456789abcdef";
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;

    ObjBytes* val_bytes = value_get_bytes_obj(&val);
    if (value_get_type(&val) != VAL_BYTES || !val_bytes) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "bytesToHex expects bytes");
        return;
    }

    ObjBytes* bytes = val_bytes;
    int count = bytes->length;
    if (count <= 0) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, 0, NULL);
        return;
    }

    size_t out_len = (size_t)count * 2;
    if ((int64_t)out_len > (int64_t)vm->config.max_string_length) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "bytesToHex result exceeds max string length");
        return;
    }

    uint8_t* data = obj_bytes_data(bytes);
    if (!data) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_IO, "Invalid bytes buffer");
        return;
    }

    char* out = (char*)safe_malloc(out_len + 1);
    for (int i = 0; i < count; i++) {
        uint8_t b = data[i];
        out[(size_t)i * 2] = hex_digits[(b >> 4) & 0x0F];
        out[(size_t)i * 2 + 1] = hex_digits[b & 0x0F];
    }
    out[out_len] = '\0';

    Value ok;
    value_init_string(&ok, out);
    free(out);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_hexToBytes(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;

    ObjString* val_str = value_get_string_obj(&val);
    const char* s = (value_get_type(&val) == VAL_STRING && val_str) ? val_str->chars : NULL;
    int len = (value_get_type(&val) == VAL_STRING && val_str) ? val_str->length : 0;
    if (!s || len <= 0) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, 0, NULL);
        return;
    }

    int start = 0;
    if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        start = 2;
    }

    int cap = (len - start) / 2 + 1;
    if (cap < 4) cap = 4;
    if (vm->config.max_array_size > 0 && cap > vm->config.max_array_size) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "hexToBytes result exceeds max array size");
        return;
    }

    ObjBytesBuffer* buf = obj_bytes_buffer_create(cap);
    int count = 0;

    int high_nibble = -1;
    for (int i = start; i < len; i++) {
        char c = s[i];
        if (isspace((unsigned char)c) || c == '_' || c == ':' || c == '-') {
            continue;
        }

        int v = hex_nibble_value(c);
        if (v < 0) {
            Value ok;
            value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
            obj_bytes_buffer_release(buf);
            result_tuple_set(vm, slot, ok, ERR_PARSE, "Invalid hex string");
            return;
        }

        if (high_nibble < 0) {
            high_nibble = v;
        } else {
            uint8_t byte_val = (uint8_t)((high_nibble << 4) | v);
            if (count < cap) {
                buf->data[count++] = byte_val;
            }
            high_nibble = -1;
        }
    }

    if (high_nibble >= 0) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        obj_bytes_buffer_release(buf);
        result_tuple_set(vm, slot, ok, ERR_PARSE, "Invalid hex string");
        return;
    }

    Value ok;
    value_init_bytes(&ok, obj_bytes_create(buf, 0, count));
    obj_bytes_buffer_release(buf);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_url_encode(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;

    Value ok;
    value_init_string(&ok, "");

    ObjString* val_str = value_get_string_obj(&val);
    if (value_get_type(&val) != VAL_STRING || !val_str || !val_str->chars) {
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "urlEncode expects string argument");
        return;
    }

    const unsigned char* input = (const unsigned char*)val_str->chars;
    int input_len = val_str->length;
    if (input_len < 0) input_len = 0;

    int max_string_length = vm->config.max_string_length;
    size_t out_len = 0;
    for (int i = 0; i < input_len; i++) {
        size_t add = url_is_unreserved_char(input[i]) ? 1U : 3U;
        if (out_len > SIZE_MAX - add) {
            result_tuple_set(vm, slot, ok, ERR_LIMIT, "urlEncode result too large");
            return;
        }
        out_len += add;
        if (max_string_length > 0 && out_len > (size_t)max_string_length) {
            result_tuple_set(vm, slot, ok, ERR_LIMIT, "urlEncode result exceeds max string length");
            return;
        }
    }

    char* out = (char*)safe_malloc(out_len + 1);
    size_t pos = 0;
    for (int i = 0; i < input_len; i++) {
        unsigned char c = input[i];
        if (url_is_unreserved_char(c)) {
            out[pos++] = (char)c;
        } else {
            out[pos++] = '%';
            out[pos++] = url_hex_upper((int)((c >> 4) & 0x0F));
            out[pos++] = url_hex_upper((int)(c & 0x0F));
        }
    }
    out[pos] = '\0';

    value_free(&ok);
    value_init_string(&ok, out);
    free(out);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_url_decode(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;

    Value ok;
    value_init_string(&ok, "");

    ObjString* val_str = value_get_string_obj(&val);
    if (value_get_type(&val) != VAL_STRING || !val_str || !val_str->chars) {
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "urlDecode expects string argument");
        return;
    }

    const char* input = val_str->chars;
    int input_len = val_str->length;
    if (input_len < 0) input_len = 0;
    int max_string_length = vm->config.max_string_length;

    size_t out_len = 0;
    for (int i = 0; i < input_len;) {
        if (input[i] == '%') {
            if (i + 2 >= input_len) {
                char err[128];
                snprintf(err, sizeof(err), "Invalid percent-encoding at index %d", i);
                result_tuple_set(vm, slot, ok, ERR_PARSE, err);
                return;
            }

            int hi = hex_nibble_value(input[i + 1]);
            int lo = hex_nibble_value(input[i + 2]);
            if (hi < 0 || lo < 0) {
                char err[128];
                snprintf(err, sizeof(err), "Invalid percent-encoding at index %d", i);
                result_tuple_set(vm, slot, ok, ERR_PARSE, err);
                return;
            }

            unsigned char decoded = (unsigned char)((hi << 4) | lo);
            if (decoded == '\0') {
                result_tuple_set(vm, slot, ok, ERR_PARSE, "urlDecode does not support decoded NUL byte");
                return;
            }

            out_len++;
            i += 3;
        } else {
            out_len++;
            i++;
        }

        if (max_string_length > 0 && out_len > (size_t)max_string_length) {
            result_tuple_set(vm, slot, ok, ERR_LIMIT, "urlDecode result exceeds max string length");
            return;
        }
    }

    char* out = (char*)safe_malloc(out_len + 1);
    size_t pos = 0;
    for (int i = 0; i < input_len;) {
        if (input[i] == '%') {
            int hi = hex_nibble_value(input[i + 1]);
            int lo = hex_nibble_value(input[i + 2]);
            out[pos++] = (char)((hi << 4) | lo);
            i += 3;
        } else {
            out[pos++] = input[i++];
        }
    }
    out[pos] = '\0';

    value_free(&ok);
    value_init_string(&ok, out);
    free(out);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_string_to_bytes(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;

    ObjString* val_str = value_get_string_obj(&val);
    if (value_get_type(&val) != VAL_STRING || !val_str || !val_str->chars) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "stringToBytes expects string argument");
        return;
    }

    int len = val_str->length;
    if (len < 0) len = 0;
    if (vm->config.max_array_size > 0 && len > vm->config.max_array_size) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "stringToBytes result exceeds max array size");
        return;
    }

    const unsigned char* s = (const unsigned char*)val_str->chars;
    ObjBytes* bytes = obj_bytes_create_copy(s, len);

    Value ok;
    value_init_bytes(&ok, bytes);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_bytes_to_string(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;

    ObjBytes* bytes = value_get_bytes_obj(&val);
    if (value_get_type(&val) != VAL_BYTES || !bytes) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "bytesToString expects bytes");
        return;
    }

    int len = bytes->length;
    if (len < 0) len = 0;
    if (vm->config.max_string_length > 0 && len > vm->config.max_string_length) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "bytesToString result exceeds max string length");
        return;
    }

    uint8_t* data = len > 0 ? obj_bytes_data(bytes) : NULL;
    if (len > 0 && !data) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_IO, "Invalid bytes buffer");
        return;
    }

    char* out = (char*)safe_malloc((size_t)len + 1);
    if (len > 0) {
        memcpy(out, data, (size_t)len);
    }
    out[len] = '\0';

    Value ok;
    value_init_string_n(&ok, out, len);
    free(out);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sha256_bytes(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;

    ObjBytes* bytes = value_get_bytes_obj(&val);
    if (value_get_type(&val) != VAL_BYTES || !bytes) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sha256Bytes expects bytes");
        return;
    }

    int count = bytes->length;
    if (count < 0) count = 0;
    uint8_t* data = count > 0 ? obj_bytes_data(bytes) : NULL;
    if (count > 0 && !data) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_IO, "Invalid bytes buffer");
        return;
    }
    if (vm->config.max_array_size > 0 && 32 > vm->config.max_array_size) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "sha256Bytes result exceeds max array size");
        return;
    }

    uint8_t digest[32];
    crypto_sha256(data, (size_t)count, digest);

    Value ok;
    value_init_bytes(&ok, obj_bytes_create_copy(digest, (int)sizeof(digest)));
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_hmac_sha256_bytes(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value key_val = *slot;
    Value data_val = vm->stack.values[vm->stack.count - 1];

    ObjBytes* key = value_get_bytes_obj(&key_val);
    ObjBytes* data_bytes = value_get_bytes_obj(&data_val);
    if (value_get_type(&key_val) != VAL_BYTES || !key ||
        value_get_type(&data_val) != VAL_BYTES || !data_bytes) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "hmacSha256Bytes expects (bytes, bytes)");
        return;
    }

    int key_count = key->length;
    int data_count = data_bytes->length;
    if (key_count < 0) key_count = 0;
    if (data_count < 0) data_count = 0;

    uint8_t* key_data = key_count > 0 ? obj_bytes_data(key) : NULL;
    uint8_t* payload = data_count > 0 ? obj_bytes_data(data_bytes) : NULL;
    if ((key_count > 0 && !key_data) || (data_count > 0 && !payload)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_IO, "Invalid bytes buffer");
        return;
    }
    if (vm->config.max_array_size > 0 && 32 > vm->config.max_array_size) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "hmacSha256Bytes result exceeds max array size");
        return;
    }

    uint8_t digest[32];
    crypto_hmac_sha256(key_data, (size_t)key_count, payload, (size_t)data_count, digest);

    Value ok;
    value_init_bytes(&ok, obj_bytes_create_copy(digest, (int)sizeof(digest)));
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_pbkdf2_hmac_sha256_bytes(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 4];
    Value password_val = *slot;
    Value salt_val = vm->stack.values[vm->stack.count - 3];
    Value iterations_val = vm->stack.values[vm->stack.count - 2];
    Value out_len_val = vm->stack.values[vm->stack.count - 1];

    ObjBytes* password = value_get_bytes_obj(&password_val);
    ObjBytes* salt = value_get_bytes_obj(&salt_val);
    if (value_get_type(&password_val) != VAL_BYTES || !password ||
        value_get_type(&salt_val) != VAL_BYTES || !salt ||
        value_get_type(&iterations_val) != VAL_INT ||
        value_get_type(&out_len_val) != VAL_INT) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm,
                         slot,
                         ok,
                         ERR_INVALID_ARGUMENT,
                         "pbkdf2HmacSha256Bytes expects (bytes, bytes, int, int)");
        return;
    }

    int64_t iterations_raw = value_get_int(&iterations_val);
    int64_t out_len_raw = value_get_int(&out_len_val);
    if (iterations_raw <= 0 || iterations_raw > (int64_t)UINT32_MAX) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm,
                         slot,
                         ok,
                         ERR_INVALID_ARGUMENT,
                         "pbkdf2HmacSha256Bytes iterations must be between 1 and 4294967295");
        return;
    }
    if (out_len_raw < 0) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm,
                         slot,
                         ok,
                         ERR_INVALID_ARGUMENT,
                         "pbkdf2HmacSha256Bytes output length must be non-negative");
        return;
    }
    if (vm->config.max_array_size > 0 && out_len_raw > vm->config.max_array_size) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm,
                         slot,
                         ok,
                         ERR_LIMIT,
                         "pbkdf2HmacSha256Bytes result exceeds max array size");
        return;
    }
    if (out_len_raw > INT_MAX) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm,
                         slot,
                         ok,
                         ERR_LIMIT,
                         "pbkdf2HmacSha256Bytes output length too large");
        return;
    }

    int password_count = password->length;
    int salt_count = salt->length;
    if (password_count < 0) password_count = 0;
    if (salt_count < 0) salt_count = 0;

    uint8_t* password_data = password_count > 0 ? obj_bytes_data(password) : NULL;
    uint8_t* salt_data = salt_count > 0 ? obj_bytes_data(salt) : NULL;
    if ((password_count > 0 && !password_data) || (salt_count > 0 && !salt_data)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_IO, "Invalid bytes buffer");
        return;
    }

    int out_len = (int)out_len_raw;
    ObjBytes* out_bytes = obj_bytes_create_with_size(out_len, 0);
    uint8_t* out_data = out_len > 0 ? obj_bytes_data(out_bytes) : NULL;
    if (out_len > 0 && !out_data) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        obj_bytes_release(out_bytes);
        result_tuple_set(vm, slot, ok, ERR_IO, "Failed to allocate derived key buffer");
        return;
    }

    if (!crypto_pbkdf2_hmac_sha256(password_data,
                                   (size_t)password_count,
                                   salt_data,
                                   (size_t)salt_count,
                                   (uint32_t)iterations_raw,
                                   out_data,
                                   (size_t)out_len)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        obj_bytes_release(out_bytes);
        result_tuple_set(vm, slot, ok, ERR_CRYPTO, "pbkdf2HmacSha256Bytes failed");
        return;
    }

    Value ok;
    value_init_bytes(&ok, out_bytes);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_hkdf_hmac_sha256_bytes(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 4];
    Value ikm_val = *slot;
    Value salt_val = vm->stack.values[vm->stack.count - 3];
    Value info_val = vm->stack.values[vm->stack.count - 2];
    Value out_len_val = vm->stack.values[vm->stack.count - 1];

    ObjBytes* ikm = value_get_bytes_obj(&ikm_val);
    ObjBytes* salt = value_get_bytes_obj(&salt_val);
    ObjBytes* info = value_get_bytes_obj(&info_val);
    if (value_get_type(&ikm_val) != VAL_BYTES || !ikm ||
        value_get_type(&salt_val) != VAL_BYTES || !salt ||
        value_get_type(&info_val) != VAL_BYTES || !info ||
        value_get_type(&out_len_val) != VAL_INT) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm,
                         slot,
                         ok,
                         ERR_INVALID_ARGUMENT,
                         "hkdfHmacSha256Bytes expects (bytes, bytes, bytes, int)");
        return;
    }

    int64_t out_len_raw = value_get_int(&out_len_val);
    if (out_len_raw < 0) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "hkdfHmacSha256Bytes output length must be non-negative");
        return;
    }
    if (out_len_raw > 255 * 32) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "hkdfHmacSha256Bytes output length must be <= 8160");
        return;
    }
    if (vm->config.max_array_size > 0 && out_len_raw > vm->config.max_array_size) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "hkdfHmacSha256Bytes result exceeds max array size");
        return;
    }

    int ikm_count = ikm->length;
    int salt_count = salt->length;
    int info_count = info->length;
    if (ikm_count < 0) ikm_count = 0;
    if (salt_count < 0) salt_count = 0;
    if (info_count < 0) info_count = 0;

    uint8_t* ikm_data = ikm_count > 0 ? obj_bytes_data(ikm) : NULL;
    uint8_t* salt_data = salt_count > 0 ? obj_bytes_data(salt) : NULL;
    uint8_t* info_data = info_count > 0 ? obj_bytes_data(info) : NULL;
    if ((ikm_count > 0 && !ikm_data) || (salt_count > 0 && !salt_data) || (info_count > 0 && !info_data)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_IO, "Invalid bytes buffer");
        return;
    }

    int out_len = (int)out_len_raw;
    ObjBytes* out_bytes = obj_bytes_create_with_size(out_len, 0);
    uint8_t* out_data = out_len > 0 ? obj_bytes_data(out_bytes) : NULL;
    if (out_len > 0 && !out_data) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        obj_bytes_release(out_bytes);
        result_tuple_set(vm, slot, ok, ERR_IO, "Failed to allocate HKDF output buffer");
        return;
    }

    if (!crypto_hkdf_hmac_sha256(ikm_data,
                                 (size_t)ikm_count,
                                 salt_data,
                                 (size_t)salt_count,
                                 info_data,
                                 (size_t)info_count,
                                 out_data,
                                 (size_t)out_len)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        obj_bytes_release(out_bytes);
        result_tuple_set(vm, slot, ok, ERR_CRYPTO, "hkdfHmacSha256Bytes failed");
        return;
    }

    Value ok;
    value_init_bytes(&ok, out_bytes);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_constant_time_bytes_equal(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value left_val = *slot;
    Value right_val = vm->stack.values[vm->stack.count - 1];

    ObjBytes* left = value_get_bytes_obj(&left_val);
    ObjBytes* right = value_get_bytes_obj(&right_val);
    if (value_get_type(&left_val) != VAL_BYTES || !left ||
        value_get_type(&right_val) != VAL_BYTES || !right) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         slot,
                         ok,
                         ERR_INVALID_ARGUMENT,
                         "constantTimeBytesEqual expects (bytes, bytes)");
        return;
    }

    int left_len = left->length;
    int right_len = right->length;
    if (left_len < 0) left_len = 0;
    if (right_len < 0) right_len = 0;
    if (left_len != right_len) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, 0, NULL);
        return;
    }

    uint8_t* left_data = left_len > 0 ? obj_bytes_data(left) : NULL;
    uint8_t* right_data = right_len > 0 ? obj_bytes_data(right) : NULL;
    if ((left_len > 0 && !left_data) || (right_len > 0 && !right_data)) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_IO, "Invalid bytes buffer");
        return;
    }

    Value ok;
    value_init_bool(&ok, crypto_constant_time_equal(left_data, right_data, (size_t)left_len));
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_aes_ctr_bytes(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value key_val = *slot;
    Value counter_val = vm->stack.values[vm->stack.count - 2];
    Value input_val = vm->stack.values[vm->stack.count - 1];

    ObjBytes* key = value_get_bytes_obj(&key_val);
    ObjBytes* counter = value_get_bytes_obj(&counter_val);
    ObjBytes* input = value_get_bytes_obj(&input_val);
    if (value_get_type(&key_val) != VAL_BYTES || !key ||
        value_get_type(&counter_val) != VAL_BYTES || !counter ||
        value_get_type(&input_val) != VAL_BYTES || !input) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "aesCtrBytes expects (bytes, bytes, bytes)");
        return;
    }

    int key_len = key->length;
    int counter_len = counter->length;
    int input_len = input->length;
    if (key_len < 0) key_len = 0;
    if (counter_len < 0) counter_len = 0;
    if (input_len < 0) input_len = 0;

    if (!(key_len == 16 || key_len == 24 || key_len == 32)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "aesCtrBytes key must be 16, 24, or 32 bytes");
        return;
    }
    if (counter_len != 16) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "aesCtrBytes counter must be 16 bytes");
        return;
    }
    if (vm->config.max_array_size > 0 && input_len > vm->config.max_array_size) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "aesCtrBytes result exceeds max array size");
        return;
    }

    uint8_t* key_data = obj_bytes_data(key);
    uint8_t* counter_data = obj_bytes_data(counter);
    uint8_t* input_data = input_len > 0 ? obj_bytes_data(input) : NULL;
    if (!key_data || !counter_data || (input_len > 0 && !input_data)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_IO, "Invalid bytes buffer");
        return;
    }

    ObjBytes* out_bytes = obj_bytes_create_with_size(input_len, 0);
    uint8_t* out_data = input_len > 0 ? obj_bytes_data(out_bytes) : NULL;
    if (input_len > 0 && !out_data) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        obj_bytes_release(out_bytes);
        result_tuple_set(vm, slot, ok, ERR_IO, "Failed to allocate AES-CTR output buffer");
        return;
    }

    if (!crypto_aes_ctr(key_data,
                        (size_t)key_len,
                        counter_data,
                        input_data,
                        (size_t)input_len,
                        out_data)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        obj_bytes_release(out_bytes);
        result_tuple_set(vm, slot, ok, ERR_CRYPTO, "aesCtrBytes failed");
        return;
    }

    Value ok;
    value_init_bytes(&ok, out_bytes);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_aes_gcm_seal_bytes(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 4];
    Value key_val = *slot;
    Value nonce_val = vm->stack.values[vm->stack.count - 3];
    Value plaintext_val = vm->stack.values[vm->stack.count - 2];
    Value aad_val = vm->stack.values[vm->stack.count - 1];

    ObjBytes* key = value_get_bytes_obj(&key_val);
    ObjBytes* nonce = value_get_bytes_obj(&nonce_val);
    ObjBytes* plaintext = value_get_bytes_obj(&plaintext_val);
    ObjBytes* aad = value_get_bytes_obj(&aad_val);
    if (value_get_type(&key_val) != VAL_BYTES || !key ||
        value_get_type(&nonce_val) != VAL_BYTES || !nonce ||
        value_get_type(&plaintext_val) != VAL_BYTES || !plaintext ||
        value_get_type(&aad_val) != VAL_BYTES || !aad) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "aesGcmSealBytes expects (bytes, bytes, bytes, bytes)");
        return;
    }

    int key_len = key->length;
    int nonce_len = nonce->length;
    int plaintext_len = plaintext->length;
    int aad_len = aad->length;
    if (key_len < 0) key_len = 0;
    if (nonce_len < 0) nonce_len = 0;
    if (plaintext_len < 0) plaintext_len = 0;
    if (aad_len < 0) aad_len = 0;

    if (!(key_len == 16 || key_len == 24 || key_len == 32)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "aesGcmSealBytes key must be 16, 24, or 32 bytes");
        return;
    }
    if (nonce_len != 12) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "aesGcmSealBytes nonce must be 12 bytes");
        return;
    }
    if (vm->config.max_array_size > 0 &&
        plaintext_len > vm->config.max_array_size - 16) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "aesGcmSealBytes result exceeds max array size");
        return;
    }

    uint8_t* key_data = obj_bytes_data(key);
    uint8_t* nonce_data = obj_bytes_data(nonce);
    uint8_t* plaintext_data = plaintext_len > 0 ? obj_bytes_data(plaintext) : NULL;
    uint8_t* aad_data = aad_len > 0 ? obj_bytes_data(aad) : NULL;
    if (!key_data || !nonce_data || (plaintext_len > 0 && !plaintext_data) || (aad_len > 0 && !aad_data)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_IO, "Invalid bytes buffer");
        return;
    }

    ObjBytes* out_bytes = obj_bytes_create_with_size(plaintext_len + 16, 0);
    uint8_t* out_data = obj_bytes_data(out_bytes);
    if (!out_data && plaintext_len + 16 > 0) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        obj_bytes_release(out_bytes);
        result_tuple_set(vm, slot, ok, ERR_IO, "Failed to allocate AES-GCM output buffer");
        return;
    }

    if (!crypto_aes_gcm_seal(key_data,
                             (size_t)key_len,
                             nonce_data,
                             aad_data,
                             (size_t)aad_len,
                             plaintext_data,
                             (size_t)plaintext_len,
                             out_data,
                             out_data + plaintext_len)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        obj_bytes_release(out_bytes);
        result_tuple_set(vm, slot, ok, ERR_CRYPTO, "aesGcmSealBytes failed");
        return;
    }

    Value ok;
    value_init_bytes(&ok, out_bytes);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_aes_gcm_open_bytes(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 4];
    Value key_val = *slot;
    Value nonce_val = vm->stack.values[vm->stack.count - 3];
    Value sealed_val = vm->stack.values[vm->stack.count - 2];
    Value aad_val = vm->stack.values[vm->stack.count - 1];

    ObjBytes* key = value_get_bytes_obj(&key_val);
    ObjBytes* nonce = value_get_bytes_obj(&nonce_val);
    ObjBytes* sealed = value_get_bytes_obj(&sealed_val);
    ObjBytes* aad = value_get_bytes_obj(&aad_val);
    if (value_get_type(&key_val) != VAL_BYTES || !key ||
        value_get_type(&nonce_val) != VAL_BYTES || !nonce ||
        value_get_type(&sealed_val) != VAL_BYTES || !sealed ||
        value_get_type(&aad_val) != VAL_BYTES || !aad) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "aesGcmOpenBytes expects (bytes, bytes, bytes, bytes)");
        return;
    }

    int key_len = key->length;
    int nonce_len = nonce->length;
    int sealed_len = sealed->length;
    int aad_len = aad->length;
    if (key_len < 0) key_len = 0;
    if (nonce_len < 0) nonce_len = 0;
    if (sealed_len < 0) sealed_len = 0;
    if (aad_len < 0) aad_len = 0;

    if (!(key_len == 16 || key_len == 24 || key_len == 32)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "aesGcmOpenBytes key must be 16, 24, or 32 bytes");
        return;
    }
    if (nonce_len != 12) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "aesGcmOpenBytes nonce must be 12 bytes");
        return;
    }
    if (sealed_len < 16) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "aesGcmOpenBytes input must include a 16-byte tag");
        return;
    }

    int plaintext_len = sealed_len - 16;
    uint8_t* key_data = obj_bytes_data(key);
    uint8_t* nonce_data = obj_bytes_data(nonce);
    uint8_t* sealed_data = sealed_len > 0 ? obj_bytes_data(sealed) : NULL;
    uint8_t* aad_data = aad_len > 0 ? obj_bytes_data(aad) : NULL;
    if (!key_data || !nonce_data || !sealed_data || (aad_len > 0 && !aad_data)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_IO, "Invalid bytes buffer");
        return;
    }

    ObjBytes* out_bytes = obj_bytes_create_with_size(plaintext_len, 0);
    uint8_t* out_data = plaintext_len > 0 ? obj_bytes_data(out_bytes) : NULL;
    if (plaintext_len > 0 && !out_data) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        obj_bytes_release(out_bytes);
        result_tuple_set(vm, slot, ok, ERR_IO, "Failed to allocate AES-GCM plaintext buffer");
        return;
    }

    if (!crypto_aes_gcm_open(key_data,
                             (size_t)key_len,
                             nonce_data,
                             aad_data,
                             (size_t)aad_len,
                             sealed_data,
                             (size_t)plaintext_len,
                             sealed_data + plaintext_len,
                             out_data)) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        obj_bytes_release(out_bytes);
        result_tuple_set(vm, slot, ok, ERR_CRYPTO, "aesGcmOpenBytes failed");
        return;
    }

    Value ok;
    value_init_bytes(&ok, out_bytes);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_bytes_with_size(VM* vm) {
    // arg0 (size) at count-2, arg1 (fill) at count-1
    Value* size_slot = &vm->stack.values[vm->stack.count - 2];
    Value size_val = *size_slot;
    Value fill_val = vm->stack.values[vm->stack.count - 1];

    if (value_get_type(&size_val) != VAL_INT || value_get_type(&fill_val) != VAL_INT) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, size_slot, ok, ERR_INVALID_ARGUMENT, "bytesWithSize expects (int, int)");
        return;
    }

    int64_t size64 = value_get_int(&size_val);
    if (size64 < 0) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, size_slot, ok, ERR_INVALID_ARGUMENT, "bytesWithSize size must be non-negative");
        return;
    }

    if (vm->config.max_array_size > 0 && size64 > (int64_t)vm->config.max_array_size) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, size_slot, ok, ERR_LIMIT, "bytesWithSize size exceeds max array size");
        return;
    }

    if (size64 > INT_MAX) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, size_slot, ok, ERR_LIMIT, "bytesWithSize size too large");
        return;
    }

    int64_t fill64 = value_get_int(&fill_val);
    if (fill64 < 0 || fill64 > 255) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, size_slot, ok, ERR_INVALID_ARGUMENT, "bytesWithSize fill must be in range 0..255");
        return;
    }

    Value ok;
    value_init_bytes(&ok, obj_bytes_create_with_size((int)size64, (uint8_t)fill64));
    result_tuple_set(vm, size_slot, ok, 0, NULL);
}

void builtin_bytes_join(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value parts_val = *slot;

    Value empty_ok;
    value_init_bytes(&empty_ok, obj_bytes_create_with_size(0, 0));

    ObjArray* parts = value_get_array_obj(&parts_val);
    if (value_get_type(&parts_val) != VAL_ARRAY || !parts) {
        result_tuple_set(vm, slot, empty_ok, ERR_INVALID_ARGUMENT, "bytesJoin expects array<bytes>");
        return;
    }

    if (parts->count == 0) {
        result_tuple_set(vm, slot, empty_ok, 0, NULL);
        return;
    }

    if (parts->kind != ARRAY_KIND_BOXED) {
        result_tuple_set(vm, slot, empty_ok, ERR_INVALID_ARGUMENT, "bytesJoin expects array<bytes>");
        return;
    }

    int max_array_size = (vm->config.max_array_size > 0) ? vm->config.max_array_size : INT_MAX;
    int64_t total_len = 0;
    for (int i = 0; i < parts->count; i++) {
        Value part_val = parts->data.elements[i];
        ObjBytes* part_bytes = value_get_bytes_obj(&part_val);
        if (value_get_type(&part_val) != VAL_BYTES || !part_bytes) {
            result_tuple_set(vm, slot, empty_ok, ERR_INVALID_ARGUMENT, "bytesJoin expects array<bytes>");
            return;
        }

        int part_len = part_bytes->length;
        if (part_len < 0) part_len = 0;
        if (total_len > INT64_MAX - (int64_t)part_len) {
            result_tuple_set(vm, slot, empty_ok, ERR_LIMIT, "bytesJoin size overflow");
            return;
        }

        total_len += (int64_t)part_len;
        if (total_len > (int64_t)max_array_size) {
            result_tuple_set(vm, slot, empty_ok, ERR_LIMIT, "bytesJoin result exceeds max array size");
            return;
        }
    }

    if (parts->count == 1) {
        ObjBytes* single = value_get_bytes_obj(&parts->data.elements[0]);
        if (!single) {
            result_tuple_set(vm, slot, empty_ok, ERR_INVALID_ARGUMENT, "bytesJoin expects array<bytes>");
            return;
        }
        obj_bytes_retain(single);
        Value ok;
        value_init_bytes(&ok, single);
        result_tuple_set(vm, slot, ok, 0, NULL);
        return;
    }

    ObjBytesBuffer* buffer = obj_bytes_buffer_create((int)total_len);
    ObjBytes* joined = obj_bytes_create(buffer, 0, (int)total_len);
    obj_bytes_buffer_release(buffer);

    if (!joined) {
        result_tuple_set(vm, slot, empty_ok, ERR_IO, "Failed to allocate bytes result");
        return;
    }

    uint8_t* joined_data = obj_bytes_data(joined);
    if (total_len > 0 && !joined_data) {
        obj_bytes_release(joined);
        result_tuple_set(vm, slot, empty_ok, ERR_IO, "Invalid bytes buffer");
        return;
    }

    int offset = 0;
    for (int i = 0; i < parts->count; i++) {
        ObjBytes* part = value_get_bytes_obj(&parts->data.elements[i]);
        int part_len = part ? part->length : 0;
        if (part_len <= 0) continue;

        uint8_t* part_data = obj_bytes_data(part);
        if (!part_data) {
            obj_bytes_release(joined);
            result_tuple_set(vm, slot, empty_ok, ERR_IO, "Invalid bytes buffer");
            return;
        }

        memcpy(joined_data + offset, part_data, (size_t)part_len);
        offset += part_len;
    }

    value_free(&empty_ok);
    Value ok;
    value_init_bytes(&ok, joined);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_str(VM* vm) {
    Value val = vm->stack.values[vm->stack.count - 1];
    Value result;
    char buffer[256];
    
    if (value_get_type(&val) == VAL_STRING) {
        // Already a string; return as-is.
        return;
    }

    if (value_get_type(&val) == VAL_INT) {
        snprintf(buffer, sizeof(buffer), "%lld", (long long)value_get_int(&val));
        value_init_string(&result, buffer);
    } else if (value_get_type(&val) == VAL_BOOL) {
        value_init_string(&result, value_get_bool(&val) ? "true" : "false");
    } else if (value_get_type(&val) == VAL_BIGINT) {
        char* str = obj_bigint_to_string(value_get_bigint_obj(&val));
        value_init_string(&result, str ? str : "0");
        if (str) free(str);
    } else if (value_get_type(&val) == VAL_DOUBLE) {
        snprintf(buffer, sizeof(buffer), "%g", value_get_double(&val));
        value_init_string(&result, buffer);
    } else if (value_get_type(&val) == VAL_NIL) {
        value_init_string(&result, "nil");
    } else {
        value_init_string(&result, "<unknown>");
    }

    Value* slot = &vm->stack.values[vm->stack.count - 1];
    value_free(slot);
    *slot = result;
}

void builtin_format_double(VM* vm) {
    Value* value_slot = &vm->stack.values[vm->stack.count - 2];
    Value decimals_val = vm->stack.values[vm->stack.count - 1];
    Value value = *value_slot;

    if (value_get_type(&decimals_val) != VAL_INT ||
        (value_get_type(&value) != VAL_DOUBLE && value_get_type(&value) != VAL_INT)) {
        Value nil;
        value_init_nil(&nil);
        value_free(value_slot);
        *value_slot = nil;
        return;
    }

    double d = value_get_type(&value) == VAL_INT ? (double)value_get_int(&value) : value_get_double(&value);
    int decimals = (int)value_get_int(&decimals_val);
    if (decimals < 0) decimals = 0;
    if (decimals > 308) decimals = 308;

    Value result;
    if (isnan(d)) {
        value_init_string(&result, "nan");
    } else if (isinf(d)) {
        value_init_string(&result, d < 0.0 ? "-inf" : "inf");
    } else {
        char fmt[16];
        snprintf(fmt, sizeof(fmt), "%%.%df", decimals);

        char stack_buf[128];
        int written = snprintf(stack_buf, sizeof(stack_buf), fmt, d);
        if (written < 0) {
            value_init_string(&result, "");
        } else if (written < (int)sizeof(stack_buf)) {
            value_init_string(&result, stack_buf);
        } else {
            char* buffer = (char*)safe_malloc((size_t)written + 1);
            snprintf(buffer, (size_t)written + 1, fmt, d);
            value_init_string(&result, buffer);
            free(buffer);
        }
    }

    value_free(value_slot);
    *value_slot = result;
}

void builtin_json_parse(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value input = *slot;

    if (value_get_type(&input) != VAL_STRING) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "jsonParse requires string argument");
        return;
    }

    ObjString* input_str = value_get_string_obj(&input);
    const char* json = input_str ? input_str->chars : "";
    const char* quick = json;
    while (*quick != '\0' && isspace((unsigned char)*quick)) quick++;
    if (*quick == '"' ||
        *quick == '-' ||
        (*quick >= '0' && *quick <= '9') ||
        *quick == 't' ||
        *quick == 'f' ||
        *quick == 'n' ||
        *quick == '{' ||
        *quick == '[') {
        Value fast_result;
        if (json_parse_fast_common(vm, json, &fast_result)) {
            result_tuple_set(vm, slot, fast_result, 0, NULL);
            return;
        }
    }

    cJSON* root = cJSON_Parse(json);
    if (!root) {
        char msg[192];
        msg[0] = '\0';
        const char* err_msg = "Invalid JSON";
        const char* err = cJSON_GetErrorPtr();
        Value data;
        int64_t offset = 0;
        int64_t line = 1;
        int64_t column = 1;
        bool at_eof = true;
        char near_snippet[64];
        near_snippet[0] = '\0';

        if (err) {
            offset = (int64_t)(err - json);
            int64_t json_len = (int64_t)strlen(json);
            if (offset < 0) offset = 0;
            if (offset > json_len) offset = json_len;
            at_eof = (err[0] == '\0');

            json_parse_compute_line_column(json, offset, &line, &column);
            json_parse_make_near_snippet(err, near_snippet, sizeof(near_snippet));
            snprintf(msg,
                     sizeof(msg),
                     "Invalid JSON at line %lld, column %lld near: %s",
                     (long long)line,
                     (long long)column,
                     near_snippet[0] != '\0' ? near_snippet : "<eof>");
            err_msg = msg;
        }
        data = json_parse_make_error_data(vm, offset, line, column, near_snippet, at_eof);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set_data(vm, slot, ok, ERR_PARSE, err_msg, data);
        return;
    }

    Value result;
    if (!json_from_cjson(vm, root, &result, 0)) {
        cJSON_Delete(root);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_PARSE, "jsonParse failed");
        return;
    }

    cJSON_Delete(root);
    result_tuple_set(vm, slot, result, 0, NULL);
}

void builtin_json_stringify(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value input = *slot;
    Value fast_result;
    if (json_stringify_fast_scalar(vm, &input, &fast_result)) {
        result_tuple_set(vm, slot, fast_result, 0, NULL);
        return;
    }
    if (json_stringify_fast_common(vm, &input, &fast_result)) {
        result_tuple_set(vm, slot, fast_result, 0, NULL);
        return;
    }

    char error_buf[128];
    error_buf[0] = '\0';

    cJSON* root = json_to_cjson(vm, &input, 0, error_buf, sizeof(error_buf));
    if (!root) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, strstr(error_buf, "nesting limit") ? ERR_LIMIT : ERR_INVALID_ARGUMENT, error_buf[0] ? error_buf : "jsonStringify failed");
        return;
    }

    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_INTERNAL, "jsonStringify failed");
        return;
    }

    size_t len = strlen(rendered);
    if (vm->config.max_string_length > 0 && (int64_t)len > (int64_t)vm->config.max_string_length) {
        cJSON_free(rendered);
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "jsonStringify result exceeds max string length");
        return;
    }

    Value result;
    value_init_string(&result, rendered);
    cJSON_free(rendered);
    result_tuple_set(vm, slot, result, 0, NULL);
}

void builtin_json_stringify_pretty(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value input = *slot;
    Value fast_result;
    if (json_stringify_fast_scalar(vm, &input, &fast_result)) {
        result_tuple_set(vm, slot, fast_result, 0, NULL);
        return;
    }

    char error_buf[128];
    error_buf[0] = '\0';

    cJSON* root = json_to_cjson(vm, &input, 0, error_buf, sizeof(error_buf));
    if (!root) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, strstr(error_buf, "nesting limit") ? ERR_LIMIT : ERR_INVALID_ARGUMENT, error_buf[0] ? error_buf : "jsonStringifyPretty failed");
        return;
    }

    char* rendered = cJSON_Print(root);
    cJSON_Delete(root);
    if (!rendered) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_INTERNAL, "jsonStringifyPretty failed");
        return;
    }

    size_t len = strlen(rendered);
    if (vm->config.max_string_length > 0 && (int64_t)len > (int64_t)vm->config.max_string_length) {
        cJSON_free(rendered);
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "jsonStringifyPretty result exceeds max string length");
        return;
    }

    Value result;
    value_init_string(&result, rendered);
    cJSON_free(rendered);
    result_tuple_set(vm, slot, result, 0, NULL);
}

void builtin_json_decode(VM* vm) {
    // arg0 (value) at count-2, arg1 (schema) at count-1
    Value* value_slot = &vm->stack.values[vm->stack.count - 2];
    Value input = *value_slot;
    Value schema = vm->stack.values[vm->stack.count - 1];

    Value decoded;
    value_init_nil(&decoded);

    int64_t err_code = 0;
    char err_msg[256];
    err_msg[0] = '\0';
    Value err_data;
    value_init_nil(&err_data);

    bool ok = json_decode_value_with_schema(vm,
                                            &input,
                                            &schema,
                                            "$",
                                            "$",
                                            0,
                                            &decoded,
                                            &err_code,
                                            err_msg,
                                            sizeof(err_msg),
                                            &err_data);
    if (!ok) {
        Value nil_value;
        value_init_nil(&nil_value);
        result_tuple_set_data(vm,
                              value_slot,
                              nil_value,
                              err_code ? err_code : ERR_PARSE,
                              err_msg[0] ? err_msg : "jsonDecode failed",
                              err_data);
        value_free(&decoded);
        return;
    }

    result_tuple_set(vm, value_slot, decoded, 0, NULL);
}

void builtin_array_with_size(VM* vm) {
    // arg0 (size) at count-2, arg1 (default) at count-1
    Value* size_slot = &vm->stack.values[vm->stack.count - 2];
    Value size_val = *size_slot;
    Value default_val = vm->stack.values[vm->stack.count - 1];

    if (value_get_type(&size_val) != VAL_INT) {
        ObjArray* empty = obj_array_create(vm, 0);
        Value ok;
        value_init_array(&ok, empty);
        result_tuple_set(vm, size_slot, ok, ERR_INVALID_ARGUMENT, "arrayWithSize expects size:int");
        return;
    }

    int64_t size64 = value_get_int(&size_val);
    if (size64 < 0) {
        ObjArray* empty = obj_array_create(vm, 0);
        Value ok;
        value_init_array(&ok, empty);
        result_tuple_set(vm, size_slot, ok, ERR_INVALID_ARGUMENT, "arrayWithSize size must be non-negative");
        return;
    }

    if (vm->config.max_array_size > 0 && size64 > (int64_t)vm->config.max_array_size) {
        ObjArray* empty = obj_array_create(vm, 0);
        Value ok;
        value_init_array(&ok, empty);
        result_tuple_set(vm, size_slot, ok, ERR_LIMIT, "arrayWithSize size exceeds max array size");
        return;
    }

    if (size64 > INT_MAX) {
        ObjArray* empty = obj_array_create(vm, 0);
        Value ok;
        value_init_array(&ok, empty);
        result_tuple_set(vm, size_slot, ok, ERR_LIMIT, "arrayWithSize size too large");
        return;
    }

    int size = (int)size64;
    ArrayKind kind = ARRAY_KIND_BOXED;
    if (value_get_type(&default_val) == VAL_INT) {
        int64_t default_int = value_get_int(&default_val);
        if (default_int >= 0 && default_int <= 255) {
            kind = ARRAY_KIND_BYTE;
        } else {
            kind = ARRAY_KIND_INT;
        }
    } else if (value_get_type(&default_val) == VAL_DOUBLE) {
        kind = ARRAY_KIND_DOUBLE;
    } else if (value_get_type(&default_val) == VAL_BOOL) {
        kind = ARRAY_KIND_BOOL;
    }

    ObjArray* arr = obj_array_create_typed(vm, size, kind);
    arr->count = size;
    for (int i = 0; i < size; i++) {
        if (arr->kind == ARRAY_KIND_BOXED) {
            Value elem = default_val;
            value_retain(&elem);
            arr->data.elements[i] = elem;
        } else if (arr->kind == ARRAY_KIND_INT) {
            arr->data.ints[i] = value_get_int(&default_val);
        } else if (arr->kind == ARRAY_KIND_BYTE) {
            arr->data.bytes[i] = (uint8_t)value_get_int(&default_val);
        } else if (arr->kind == ARRAY_KIND_DOUBLE) {
            arr->data.doubles[i] = value_get_double(&default_val);
        } else if (arr->kind == ARRAY_KIND_BOOL) {
            arr->data.bools[i] = (uint8_t)(value_get_bool(&default_val) ? 1 : 0);
        }
    }

    Value ok;
    value_init_array(&ok, arr);
    result_tuple_set(vm, size_slot, ok, 0, NULL);
}

void builtin_push(VM* vm) {
    // arg0 (array) at count-2, arg1 (element) at count-1
    Value* arr_slot = &vm->stack.values[vm->stack.count - 2];
    Value* elem_slot = &vm->stack.values[vm->stack.count - 1];
    Value arr = *arr_slot;

    ObjArray* arr_obj = value_get_array_obj(&arr);
    if (value_get_type(&arr) != VAL_ARRAY || !arr_obj) {
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }

    // Move elem into the array so call cleanup can safely drop arg1.
    Value elem = *elem_slot;
    Value nil;
    value_init_nil(&nil);
    *elem_slot = nil;

    obj_array_push(arr_obj, elem);

    value_free(arr_slot);
    *arr_slot = nil;
}

void builtin_pop(VM* vm) {
    Value* arr_slot = &vm->stack.values[vm->stack.count - 1];
    Value arr = *arr_slot;
    
    ObjArray* arr_obj = value_get_array_obj(&arr);
    if (value_get_type(&arr) != VAL_ARRAY || !arr_obj) {
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }
    
    Value result;
    obj_array_pop(arr_obj, &result);
    value_free(arr_slot);
    *arr_slot = result;
}

void builtin_copy_into(VM* vm) {
    // arg0 (dst) at count-2, arg1 (src) at count-1
    Value* dst_slot = &vm->stack.values[vm->stack.count - 2];
    Value src_val = vm->stack.values[vm->stack.count - 1];
    Value dst_val = *dst_slot;

    if (value_get_type(&dst_val) != VAL_ARRAY || value_get_type(&src_val) != VAL_ARRAY) {
        vm_runtime_error(vm, "copyInto expects two arrays");
        Value nil;
        value_init_nil(&nil);
        value_free(dst_slot);
        *dst_slot = nil;
        return;
    }

    ObjArray* dst = value_get_array_obj(&dst_val);
    ObjArray* src = value_get_array_obj(&src_val);
    if (!dst || !src) {
        vm_runtime_error(vm, "copyInto expects two arrays");
        Value nil;
        value_init_nil(&nil);
        value_free(dst_slot);
        *dst_slot = nil;
        return;
    }
    if (dst->count != src->count) {
        vm_runtime_error(vm, "copyInto expects arrays of equal length");
        Value nil;
        value_init_nil(&nil);
        value_free(dst_slot);
        *dst_slot = nil;
        return;
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
    } else {
        for (int i = 0; i < count; i++) {
            Value elem = src->data.elements[i];
            value_retain(&elem);
            value_free(&dst->data.elements[i]);
            dst->data.elements[i] = elem;
        }
    }

    Value nil;
    value_init_nil(&nil);
    value_free(dst_slot);
    *dst_slot = nil;
}

void builtin_reverse_prefix(VM* vm) {
    // arg0 (array) at count-2, arg1 (hi) at count-1
    Value* arr_slot = &vm->stack.values[vm->stack.count - 2];
    Value hi_val = vm->stack.values[vm->stack.count - 1];
    Value arr_val = *arr_slot;

    if (value_get_type(&arr_val) != VAL_ARRAY) {
        vm_runtime_error(vm, "reversePrefix expects an array");
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }

    if (value_get_type(&hi_val) != VAL_INT) {
        vm_runtime_error(vm, "reversePrefix hi must be int");
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }

    ObjArray* arr = value_get_array_obj(&arr_val);
    int64_t hi64 = value_get_int(&hi_val);
    if (!arr) {
        vm_runtime_error(vm, "reversePrefix expects an array");
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }
    if (hi64 < 0 || hi64 >= arr->count) {
        vm_runtime_error(vm, "reversePrefix hi out of bounds");
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
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

    Value nil;
    value_init_nil(&nil);
    value_free(arr_slot);
    *arr_slot = nil;
}

void builtin_rotate_prefix_left(VM* vm) {
    // arg0 (array) at count-2, arg1 (hi) at count-1
    Value* arr_slot = &vm->stack.values[vm->stack.count - 2];
    Value hi_val = vm->stack.values[vm->stack.count - 1];
    Value arr_val = *arr_slot;

    if (value_get_type(&arr_val) != VAL_ARRAY) {
        vm_runtime_error(vm, "rotatePrefixLeft expects an array");
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }

    if (value_get_type(&hi_val) != VAL_INT) {
        vm_runtime_error(vm, "rotatePrefixLeft hi must be int");
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }

    ObjArray* arr = value_get_array_obj(&arr_val);
    int64_t hi64 = value_get_int(&hi_val);
    if (!arr) {
        vm_runtime_error(vm, "rotatePrefixLeft expects an array");
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }
    if (hi64 < 0 || hi64 >= arr->count) {
        vm_runtime_error(vm, "rotatePrefixLeft hi out of bounds");
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
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

    Value nil;
    value_init_nil(&nil);
    value_free(arr_slot);
    *arr_slot = nil;
}

void builtin_rotate_prefix_right(VM* vm) {
    // arg0 (array) at count-2, arg1 (hi) at count-1
    Value* arr_slot = &vm->stack.values[vm->stack.count - 2];
    Value hi_val = vm->stack.values[vm->stack.count - 1];
    Value arr_val = *arr_slot;

    if (value_get_type(&arr_val) != VAL_ARRAY) {
        vm_runtime_error(vm, "rotatePrefixRight expects an array");
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }

    if (value_get_type(&hi_val) != VAL_INT) {
        vm_runtime_error(vm, "rotatePrefixRight hi must be int");
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }

    ObjArray* arr = value_get_array_obj(&arr_val);
    int64_t hi64 = value_get_int(&hi_val);
    if (!arr) {
        vm_runtime_error(vm, "rotatePrefixRight expects an array");
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }
    if (hi64 < 0 || hi64 >= arr->count) {
        vm_runtime_error(vm, "rotatePrefixRight hi out of bounds");
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
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

    Value nil;
    value_init_nil(&nil);
    value_free(arr_slot);
    *arr_slot = nil;
}

void builtin_keys(VM* vm) {
    Value* value_slot = &vm->stack.values[vm->stack.count - 1];
    Value value = *value_slot;

    ObjArray* value_arr = value_get_array_obj(&value);
    if (value_get_type(&value) == VAL_ARRAY && value_arr) {
        ObjArray* result_arr = obj_array_create(vm, value_arr->count);
        for (int i = 0; i < value_arr->count; i++) {
            Value key;
            value_init_int(&key, i);
            obj_array_push(result_arr, key);
        }

        Value result;
        value_init_array(&result, result_arr);
        value_free(value_slot);
        *value_slot = result;
        return;
    }

    ObjMap* value_map = value_get_map_obj(&value);
    if (value_get_type(&value) == VAL_MAP) {
        int expected = value_map ? value_map->count : 0;
        ObjArray* result_arr = obj_array_create(vm, expected);
        if (value_map) obj_map_keys(value_map, result_arr);

        Value result;
        value_init_array(&result, result_arr);
        value_free(value_slot);
        *value_slot = result;
        return;
    }

    {
        Value nil;
        value_init_nil(&nil);
        value_free(value_slot);
        *value_slot = nil;
        return;
    }
}

void builtin_values(VM* vm) {
    Value* value_slot = &vm->stack.values[vm->stack.count - 1];
    Value value = *value_slot;

    ObjMap* value_map = value_get_map_obj(&value);
    if (value_get_type(&value) == VAL_MAP) {
        int expected = value_map ? value_map->count : 0;
        ObjArray* result_arr = obj_array_create(vm, expected);
        if (value_map) obj_map_values(value_map, result_arr);

        Value result;
        value_init_array(&result, result_arr);
        value_free(value_slot);
        *value_slot = result;
        return;
    }

    ObjArray* value_arr = value_get_array_obj(&value);
    if (value_get_type(&value) != VAL_ARRAY || !value_arr) {
        Value nil;
        value_init_nil(&nil);
        value_free(value_slot);
        *value_slot = nil;
        return;
    }
    
    ObjArray* result_arr = obj_array_create(vm, value_arr->count);
    for (int i =0; i < value_arr->count; i++) {
        ObjArray* src = value_arr;
        if (src->kind == ARRAY_KIND_BOXED) {
            Value val = src->data.elements[i];
            value_retain(&val);
            obj_array_push(result_arr, val);
        } else {
            Value val;
            switch (src->kind) {
                case ARRAY_KIND_INT:
                    value_init_int(&val, src->data.ints[i]);
                    break;
                case ARRAY_KIND_BYTE:
                    value_init_int(&val, (int64_t)src->data.bytes[i]);
                    break;
                case ARRAY_KIND_DOUBLE:
                    value_init_double(&val, src->data.doubles[i]);
                    break;
                case ARRAY_KIND_BOOL:
                    value_init_bool(&val, src->data.bools[i] != 0);
                    break;
                default:
                    value_init_nil(&val);
                    break;
            }
            obj_array_push(result_arr, val);
        }
    }

    Value result;
    value_init_array(&result, result_arr);
    value_free(value_slot);
    *value_slot = result;
}

void builtin_read_line(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value file_val = *slot;

    ObjString* file_str = value_get_string_obj(&file_val);
    if (value_get_type(&file_val) != VAL_STRING || !file_str || !file_str->chars) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "read_line requires string argument");
        return;
    }

    const char* resolve_err = NULL;
    char* path = resolve_file_path(vm, file_str->chars, &resolve_err);
    if (!path) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_PERMISSION, resolve_err ? resolve_err : "Path not allowed");
        return;
    }

    int64_t open_err_code = 0;
    const char* open_err_msg = NULL;
    FILE* file = NULL;
    if (!open_file_with_limit(vm, path, "r", &file, &open_err_code, &open_err_msg)) {
        free(path);
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm,
                         slot,
                         ok,
                         open_err_code ? open_err_code : ERR_IO,
                         open_err_msg ? open_err_msg : "Failed to open file");
        return;
    }

    char buffer[1024];
    if (fgets(buffer, sizeof(buffer), file)) {
        buffer[strcspn(buffer, "\n")] = '\0';
        Value ok;
        value_init_string(&ok, buffer);
        close_file_with_limit(vm, &file);
        free(path);
        result_tuple_set(vm, slot, ok, 0, NULL);
        return;
    } else {
        Value ok;
        value_init_string(&ok, "");
        close_file_with_limit(vm, &file);
        free(path);
        result_tuple_set(vm, slot, ok, 0, NULL);
        return;
    }
}

void builtin_read_all(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value file_val = *slot;

    ObjString* file_str = value_get_string_obj(&file_val);
    if (value_get_type(&file_val) != VAL_STRING || !file_str || !file_str->chars) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "read_all requires string argument");
        return;
    }

    const char* resolve_err = NULL;
    char* path = resolve_file_path(vm, file_str->chars, &resolve_err);
    if (!path) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_PERMISSION, resolve_err ? resolve_err : "Path not allowed");
        return;
    }

    int64_t open_err_code = 0;
    const char* open_err_msg = NULL;
    FILE* file = NULL;
    if (!open_file_with_limit(vm, path, "rb", &file, &open_err_code, &open_err_msg)) {
        free(path);
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm,
                         slot,
                         ok,
                         open_err_code ? open_err_code : ERR_IO,
                         open_err_msg ? open_err_msg : "Failed to open file");
        return;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        close_file_with_limit(vm, &file);
        free(path);
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_IO, "Failed to read file");
        return;
    }
    long length = ftell(file);
    if (length < 0) {
        close_file_with_limit(vm, &file);
        free(path);
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_IO, "Failed to read file");
        return;
    }
    if (vm->config.max_string_length > 0 && length > vm->config.max_string_length) {
        close_file_with_limit(vm, &file);
        free(path);
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "read_all result exceeds max string length");
        return;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        close_file_with_limit(vm, &file);
        free(path);
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_IO, "Failed to read file");
        return;
    }

    char* content = (char*)safe_malloc((size_t)length + 1);
    size_t read = fread(content, 1, (size_t)length, file);
    int had_error = ferror(file);
    content[read] = '\0';
    close_file_with_limit(vm, &file);
    free(path);

    if (had_error) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_IO, "Failed to read file");
        free(content);
        return;
    }

    Value ok;
    value_init_string(&ok, content);
    free(content);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_write_line(VM* vm) {
    // arg0 (file) at count-2, arg1 (data) at count-1
    Value* file_slot = &vm->stack.values[vm->stack.count - 2];
    Value file_val = *file_slot;
    Value data_val = vm->stack.values[vm->stack.count - 1];

    ObjString* file_str = value_get_string_obj(&file_val);
    ObjString* data_str = value_get_string_obj(&data_val);
    if (value_get_type(&file_val) != VAL_STRING || !file_str || !file_str->chars ||
        value_get_type(&data_val) != VAL_STRING || !data_str || !data_str->chars) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, file_slot, ok, ERR_INVALID_ARGUMENT, "write_line requires string arguments");
        return;
    }

    const char* resolve_err = NULL;
    char* path = resolve_file_path(vm, file_str->chars, &resolve_err);
    if (!path) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, file_slot, ok, ERR_PERMISSION, resolve_err ? resolve_err : "Path not allowed");
        return;
    }

    int64_t open_err_code = 0;
    const char* open_err_msg = NULL;
    FILE* file = NULL;
    if (!open_file_with_limit(vm, path, "ab", &file, &open_err_code, &open_err_msg)) {
        free(path);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         file_slot,
                         ok,
                         open_err_code ? open_err_code : ERR_IO,
                         open_err_msg ? open_err_msg : "Failed to open file");
        return;
    }

    bool write_ok = true;
    if (fputs(data_str->chars, file) < 0) write_ok = false;
    if (fputs("\n", file) < 0) write_ok = false;

    close_file_with_limit(vm, &file);
    free(path);

    Value ok;
    value_init_bool(&ok, write_ok);
    result_tuple_set(vm, file_slot, ok, write_ok ? 0 : ERR_IO, write_ok ? NULL : "Failed to write file");
}

void builtin_write_all(VM* vm) {
    // arg0 (file) at count-2, arg1 (data) at count-1
    Value* file_slot = &vm->stack.values[vm->stack.count - 2];
    Value file_val = *file_slot;
    Value data_val = vm->stack.values[vm->stack.count - 1];

    ObjString* file_str = value_get_string_obj(&file_val);
    ObjString* data_str = value_get_string_obj(&data_val);
    if (value_get_type(&file_val) != VAL_STRING || !file_str || !file_str->chars ||
        value_get_type(&data_val) != VAL_STRING || !data_str || !data_str->chars) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, file_slot, ok, ERR_INVALID_ARGUMENT, "write_all requires string arguments");
        return;
    }

    const char* resolve_err = NULL;
    char* path = resolve_file_path(vm, file_str->chars, &resolve_err);
    if (!path) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, file_slot, ok, ERR_PERMISSION, resolve_err ? resolve_err : "Path not allowed");
        return;
    }

    int64_t open_err_code = 0;
    const char* open_err_msg = NULL;
    FILE* file = NULL;
    if (!open_file_with_limit(vm, path, "wb", &file, &open_err_code, &open_err_msg)) {
        free(path);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         file_slot,
                         ok,
                         open_err_code ? open_err_code : ERR_IO,
                         open_err_msg ? open_err_msg : "Failed to open file");
        return;
    }

    bool write_ok = true;
    if (fputs(data_str->chars, file) < 0) write_ok = false;
    close_file_with_limit(vm, &file);
    free(path);

    Value ok;
    value_init_bool(&ok, write_ok);
    result_tuple_set(vm, file_slot, ok, write_ok ? 0 : ERR_IO, write_ok ? NULL : "Failed to write file");
}

static bool file_open_mode_allowed(const char* mode) {
    if (!mode || mode[0] == '\0') return false;
    static const char* allowed[] = {
        "r", "rb", "w", "wb", "a", "ab",
        "r+", "rb+", "w+", "wb+", "a+", "ab+",
        NULL
    };

    for (int i = 0; allowed[i]; i++) {
        if (strcmp(mode, allowed[i]) == 0) return true;
    }
    return false;
}

void builtin_file_open(VM* vm) {
    // arg0 (path) at count-2, arg1 (mode) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value path_val = *slot;
    Value mode_val = vm->stack.values[vm->stack.count - 1];

    ObjString* path_str = value_get_string_obj(&path_val);
    ObjString* mode_str = value_get_string_obj(&mode_val);
    if (value_get_type(&path_val) != VAL_STRING || !path_str || !path_str->chars ||
        value_get_type(&mode_val) != VAL_STRING || !mode_str || !mode_str->chars) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "file_open expects (string, string)");
        return;
    }

    const char* mode = mode_str->chars;
    if (!file_open_mode_allowed(mode)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "file_open mode must be one of: r, rb, w, wb, a, ab, r+, rb+, w+, wb+, a+, ab+");
        return;
    }

    const char* resolve_err = NULL;
    char* path = resolve_file_path(vm, path_str->chars, &resolve_err);
    if (!path) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_PERMISSION, resolve_err ? resolve_err : "Path not allowed");
        return;
    }

    int64_t open_err_code = 0;
    const char* open_err_msg = NULL;
    FILE* file = NULL;
    if (!open_file_with_limit(vm, path, mode, &file, &open_err_code, &open_err_msg)) {
        free(path);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         slot,
                         ok,
                         open_err_code ? open_err_code : ERR_IO,
                         open_err_msg ? open_err_msg : "Failed to open file");
        return;
    }
    free(path);

    // Favor larger buffering for streaming workloads.
    setvbuf(file, NULL, _IOFBF, 64 * 1024);

    ObjFile* file_obj = obj_file_create(vm, file, true);
    if (!file_obj) {
        close_file_with_limit(vm, &file);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INTERNAL, "Failed to allocate file handle");
        return;
    }
    Value ok;
    value_init_file(&ok, file_obj);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

static bool file_read_line_alloc(VM* vm, FILE* file, char** out_line, size_t* out_len, int64_t* out_err_code, const char** out_err_msg) {
    if (out_line) *out_line = NULL;
    if (out_len) *out_len = 0;
    if (out_err_code) *out_err_code = 0;
    if (out_err_msg) *out_err_msg = NULL;

    if (!vm || !file || !out_line) {
        if (out_err_code) *out_err_code = ERR_INVALID_ARGUMENT;
        if (out_err_msg) *out_err_msg = "Invalid file handle";
        return false;
    }

    const int max_len = vm->config.max_string_length > 0 ? vm->config.max_string_length : INT_MAX;

    size_t cap = 0;
    size_t len = 0;
    char* buf = NULL;

    char chunk[4096];
    while (true) {
        if (!fgets(chunk, (int)sizeof(chunk), file)) {
            if (ferror(file)) {
                if (buf) free(buf);
                if (out_err_code) *out_err_code = ERR_IO;
                if (out_err_msg) *out_err_msg = "Failed to read file";
                return false;
            }

            // EOF with no bytes read => signal EOF via NULL line and nil error.
            if (len == 0) {
                if (buf) free(buf);
                *out_line = NULL;
                if (out_len) *out_len = 0;
                return true;
            }
            break;
        }

        size_t chunk_len = strlen(chunk);
        bool ends_with_newline = (chunk_len > 0 && chunk[chunk_len - 1] == '\n');
        if (ends_with_newline) {
            chunk_len--;
            if (chunk_len > 0 && chunk[chunk_len - 1] == '\r') {
                chunk_len--;
            }
        }

        if (chunk_len > 0) {
            if ((int64_t)(len + chunk_len) > (int64_t)max_len) {
                if (buf) free(buf);
                if (out_err_code) *out_err_code = ERR_LIMIT;
                if (out_err_msg) *out_err_msg = "file_read_line result exceeds max string length";
                return false;
            }

            size_t needed = len + chunk_len + 1;
            if (needed > cap) {
                size_t new_cap = cap > 0 ? cap : 256;
                while (new_cap < needed) {
                    new_cap *= 2;
                }
                buf = (char*)safe_realloc(buf, new_cap);
                cap = new_cap;
            }
            memcpy(buf + len, chunk, chunk_len);
            len += chunk_len;
            buf[len] = '\0';
        } else if (!buf) {
            // Empty line (just newline). Allocate empty buffer so caller can distinguish from EOF.
            buf = (char*)safe_malloc(1);
            cap = 1;
            buf[0] = '\0';
        }

        if (ends_with_newline) {
            break;
        }
    }

    if (!buf) {
        buf = (char*)safe_malloc(1);
        cap = 1;
        buf[0] = '\0';
    }

    // Trim trailing CR if present (e.g., binary mode on Windows, or last line ends with \r).
    while (len > 0 && buf[len - 1] == '\r') {
        len--;
        buf[len] = '\0';
    }

    *out_line = buf;
    if (out_len) *out_len = len;
    return true;
}

void builtin_file_read_line(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value file_val = *slot;

    ObjFile* file_obj = value_get_file_obj(&file_val);
    if (value_get_type(&file_val) != VAL_FILE || !file_obj) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "file_read_line expects file handle");
        return;
    }
    if (file_obj->is_closed || !file_obj->handle) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_IO, "File is closed");
        return;
    }

    char* line = NULL;
    size_t line_len = 0;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    if (!file_read_line_alloc(vm, file_obj->handle, &line, &line_len, &err_code, &err_msg)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, err_code ? err_code : ERR_IO, err_msg ? err_msg : "Failed to read file");
        return;
    }

    if (!line) {
        // EOF
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, 0, NULL);
        return;
    }

    (void)line_len;
    Value ok;
    value_init_string(&ok, line);
    free(line);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_file_close(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value file_val = *slot;

    ObjFile* file_obj = value_get_file_obj(&file_val);
    if (value_get_type(&file_val) == VAL_FILE && file_obj) {
        obj_file_close(file_obj);
    }

    Value nil;
    value_init_nil(&nil);
    value_free(slot);
    *slot = nil;
}

static int io_stream_read_limit(VM* vm) {
    if (!vm || vm->config.max_string_length <= 0) {
        return 8 * 1024 * 1024;
    }
    return vm->config.max_string_length;
}

static bool io_line_push_char(char** buf,
                              size_t* len,
                              size_t* cap,
                              char c,
                              int limit,
                              int64_t* err_code,
                              const char** err_msg) {
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!buf || !len || !cap) return false;

    if (*len + 1 > (size_t)limit) {
        if (err_code) *err_code = ERR_LIMIT;
        if (err_msg) *err_msg = "line exceeds max string length";
        return false;
    }

    if (*len + 2 > *cap) {
        size_t new_cap = (*cap > 0) ? *cap : 128;
        while (*len + 2 > new_cap) new_cap *= 2;
        *buf = (char*)safe_realloc(*buf, new_cap);
        *cap = new_cap;
    }

    (*buf)[*len] = c;
    (*len)++;
    (*buf)[*len] = '\0';
    return true;
}

static bool io_read_chunk_alloc(Value handle_val,
                                int chunk_size,
                                char** out_chunk,
                                size_t* out_len,
                                bool* out_eof,
                                int64_t* out_err_code,
                                const char** out_err_msg) {
    if (out_chunk) *out_chunk = NULL;
    if (out_len) *out_len = 0;
    if (out_eof) *out_eof = false;
    if (out_err_code) *out_err_code = 0;
    if (out_err_msg) *out_err_msg = NULL;
    if (!out_chunk || !out_len || !out_eof) return false;

    if (chunk_size <= 0) {
        if (out_err_code) *out_err_code = ERR_INVALID_ARGUMENT;
        if (out_err_msg) *out_err_msg = "chunk size must be positive";
        return false;
    }

    if (value_get_type(&handle_val) == VAL_FILE) {
        ObjFile* file_obj = value_get_file_obj(&handle_val);
        if (!file_obj || file_obj->is_closed || !file_obj->handle) {
            if (out_err_code) *out_err_code = ERR_IO;
            if (out_err_msg) *out_err_msg = "File is closed";
            return false;
        }

        char* chunk = (char*)safe_malloc((size_t)chunk_size + 1);
        size_t n = fread(chunk, 1, (size_t)chunk_size, file_obj->handle);
        if (n == 0) {
            free(chunk);
            if (ferror(file_obj->handle)) {
                if (out_err_code) *out_err_code = ERR_IO;
                if (out_err_msg) *out_err_msg = "Failed to read file";
                return false;
            }
            *out_eof = true;
            return true;
        }

        chunk[n] = '\0';
        *out_chunk = chunk;
        *out_len = n;
        return true;
    }

    if (value_get_type(&handle_val) == VAL_SOCKET) {
        ObjSocket* socket_obj = value_get_socket_obj(&handle_val);
        if (!socket_obj || socket_obj->socket_fd < 0) {
            if (out_err_code) *out_err_code = ERR_NETWORK;
            if (out_err_msg) *out_err_msg = "Socket is closed";
            return false;
        }

        char* chunk = (char*)safe_malloc((size_t)chunk_size + 1);
        int n = socket_recv_dispatch(socket_obj, chunk, chunk_size);
        if (n == 0) {
            free(chunk);
            *out_eof = true;
            return true;
        }
        if (n < 0) {
            free(chunk);
            if (!socket_obj->transport_recv && socket_recv_error_is_connection_close()) {
                *out_eof = true;
                return true;
            }
            if (out_err_code) *out_err_code = ERR_NETWORK;
            if (out_err_msg) *out_err_msg = "Failed to receive";
            return false;
        }

        chunk[n] = '\0';
        *out_chunk = chunk;
        *out_len = (size_t)n;
        return true;
    }

    if (out_err_code) *out_err_code = ERR_INVALID_ARGUMENT;
    if (out_err_msg) *out_err_msg = "expected file or socket handle";
    return false;
}

static bool io_write_all_handle(Value handle_val,
                                const char* data,
                                size_t len,
                                size_t* out_written,
                                int64_t* out_err_code,
                                const char** out_err_msg) {
    if (out_written) *out_written = 0;
    if (out_err_code) *out_err_code = 0;
    if (out_err_msg) *out_err_msg = NULL;
    if (!out_written) return false;

    if (len > 0 && !data) {
        if (out_err_code) *out_err_code = ERR_INVALID_ARGUMENT;
        if (out_err_msg) *out_err_msg = "Invalid data";
        return false;
    }

    if (value_get_type(&handle_val) == VAL_FILE) {
        ObjFile* file_obj = value_get_file_obj(&handle_val);
        if (!file_obj || file_obj->is_closed || !file_obj->handle) {
            if (out_err_code) *out_err_code = ERR_IO;
            if (out_err_msg) *out_err_msg = "File is closed";
            return false;
        }

        size_t written = 0;
        while (written < len) {
            size_t n = fwrite(data + written, 1, len - written, file_obj->handle);
            if (n == 0) {
                if (ferror(file_obj->handle)) {
                    if (out_err_code) *out_err_code = ERR_IO;
                    if (out_err_msg) *out_err_msg = "Failed to write file";
                    return false;
                }
                break;
            }
            written += n;
        }

        *out_written = written;
        return written == len;
    }

    if (value_get_type(&handle_val) == VAL_SOCKET) {
        ObjSocket* socket_obj = value_get_socket_obj(&handle_val);
        if (!socket_obj || socket_obj->socket_fd < 0) {
            if (out_err_code) *out_err_code = ERR_NETWORK;
            if (out_err_msg) *out_err_msg = "Socket is closed";
            return false;
        }

        size_t sent = 0;
        while (sent < len) {
            size_t remain = len - sent;
            int chunk = remain > (size_t)INT_MAX ? INT_MAX : (int)remain;
            int n = socket_send_dispatch(socket_obj, data + sent, chunk);
            if (n <= 0) {
                if (out_err_code) *out_err_code = ERR_NETWORK;
                if (out_err_msg) *out_err_msg = "Failed to send";
                return false;
            }
            sent += (size_t)n;
        }

        *out_written = sent;
        return true;
    }

    if (out_err_code) *out_err_code = ERR_INVALID_ARGUMENT;
    if (out_err_msg) *out_err_msg = "expected file or socket handle";
    return false;
}

void builtin_io_read_line(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value handle_val = *slot;
    int limit = io_stream_read_limit(vm);

    if (value_get_type(&handle_val) == VAL_FILE) {
        ObjFile* file_obj = value_get_file_obj(&handle_val);
        if (!file_obj || file_obj->is_closed || !file_obj->handle) {
            Value ok;
            value_init_nil(&ok);
            result_tuple_set(vm, slot, ok, ERR_IO, "File is closed");
            return;
        }

        char* line = NULL;
        size_t line_len = 0;
        int64_t err_code = 0;
        const char* err_msg = NULL;
        if (!file_read_line_alloc(vm, file_obj->handle, &line, &line_len, &err_code, &err_msg)) {
            Value ok;
            value_init_nil(&ok);
            result_tuple_set(vm, slot, ok, err_code ? err_code : ERR_IO, err_msg ? err_msg : "Failed to read file");
            return;
        }

        if (!line) {
            Value ok;
            value_init_nil(&ok);
            result_tuple_set(vm, slot, ok, 0, NULL);
            return;
        }

        (void)line_len;
        Value ok;
        value_init_string(&ok, line);
        free(line);
        result_tuple_set(vm, slot, ok, 0, NULL);
        return;
    }

    if (value_get_type(&handle_val) == VAL_SOCKET) {
        ObjSocket* socket_obj = value_get_socket_obj(&handle_val);
        if (!socket_obj || socket_obj->socket_fd < 0) {
            Value ok;
            value_init_nil(&ok);
            result_tuple_set(vm, slot, ok, ERR_NETWORK, "Socket is closed");
            return;
        }

        char* line = NULL;
        size_t len = 0;
        size_t cap = 0;
        while (true) {
            char c = '\0';
            int n = socket_recv_dispatch(socket_obj, &c, 1);
            if (n == 0) {
                if (len == 0) {
                    if (line) free(line);
                    Value ok;
                    value_init_nil(&ok);
                    result_tuple_set(vm, slot, ok, 0, NULL);
                    return;
                }
                break;
            }
            if (n < 0) {
                if (!socket_obj->transport_recv && socket_recv_error_is_connection_close()) {
                    if (len == 0) {
                        if (line) free(line);
                        Value ok;
                        value_init_nil(&ok);
                        result_tuple_set(vm, slot, ok, 0, NULL);
                        return;
                    }
                    break;
                }
                if (line) free(line);
                Value ok;
                value_init_nil(&ok);
                result_tuple_set(vm, slot, ok, ERR_NETWORK, "Failed to receive");
                return;
            }

            if (c == '\n') break;

            int64_t err_code = 0;
            const char* err_msg = NULL;
            if (!io_line_push_char(&line, &len, &cap, c, limit, &err_code, &err_msg)) {
                if (line) free(line);
                Value ok;
                value_init_nil(&ok);
                result_tuple_set(vm, slot, ok, err_code ? err_code : ERR_LIMIT, err_msg ? err_msg : "line exceeds max string length");
                return;
            }
        }

        while (len > 0 && line[len - 1] == '\r') {
            len--;
            line[len] = '\0';
        }

        if (!line) {
            line = (char*)safe_malloc(1);
            line[0] = '\0';
        }

        Value ok;
        value_init_string(&ok, line);
        free(line);
        result_tuple_set(vm, slot, ok, 0, NULL);
        return;
    }

    Value ok;
    value_init_nil(&ok);
    result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "ioReadLine expects (file|socket)");
}

void builtin_io_read_all(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value handle_val = *slot;
    int limit = io_stream_read_limit(vm);

    if (value_get_type(&handle_val) != VAL_FILE && value_get_type(&handle_val) != VAL_SOCKET) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "ioReadAll expects (file|socket)");
        return;
    }

    char* buffer = NULL;
    size_t len = 0;
    size_t cap = 0;

    while (true) {
        char* chunk = NULL;
        size_t chunk_len = 0;
        bool eof = false;
        int64_t err_code = 0;
        const char* err_msg = NULL;
        if (!io_read_chunk_alloc(handle_val, 4096, &chunk, &chunk_len, &eof, &err_code, &err_msg)) {
            if (buffer) free(buffer);
            Value ok;
            value_init_string(&ok, "");
            result_tuple_set(vm, slot, ok, err_code ? err_code : ERR_IO, err_msg ? err_msg : "Failed to read");
            return;
        }
        if (eof) break;

        if (len + chunk_len > (size_t)limit) {
            free(chunk);
            if (buffer) free(buffer);
            Value ok;
            value_init_string(&ok, "");
            result_tuple_set(vm, slot, ok, ERR_LIMIT, "ioReadAll result exceeds max string length");
            return;
        }

        size_t needed = len + chunk_len + 1;
        if (needed > cap) {
            size_t new_cap = cap > 0 ? cap : 4096;
            while (new_cap < needed) new_cap *= 2;
            buffer = (char*)safe_realloc(buffer, new_cap);
            cap = new_cap;
        }

        memcpy(buffer + len, chunk, chunk_len);
        len += chunk_len;
        buffer[len] = '\0';
        free(chunk);
    }

    if (!buffer) {
        buffer = (char*)safe_malloc(1);
        buffer[0] = '\0';
    }

    Value ok;
    value_init_string(&ok, buffer);
    free(buffer);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_io_read_chunk(VM* vm) {
    Value* handle_slot = &vm->stack.values[vm->stack.count - 2];
    Value handle_val = *handle_slot;
    Value max_bytes_val = vm->stack.values[vm->stack.count - 1];

    if ((value_get_type(&handle_val) != VAL_FILE && value_get_type(&handle_val) != VAL_SOCKET) ||
        value_get_type(&max_bytes_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, handle_slot, ok, ERR_INVALID_ARGUMENT, "ioReadChunk expects (file|socket, int)");
        return;
    }

    int64_t chunk_size64 = value_get_int(&max_bytes_val);
    if (chunk_size64 < 1 || chunk_size64 > 1024 * 1024) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, handle_slot, ok, ERR_INVALID_ARGUMENT, "maxBytes must be between 1 and 1048576");
        return;
    }

    char* chunk = NULL;
    size_t chunk_len = 0;
    bool eof = false;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    if (!io_read_chunk_alloc(handle_val,
                             (int)chunk_size64,
                             &chunk,
                             &chunk_len,
                             &eof,
                             &err_code,
                             &err_msg)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, handle_slot, ok, err_code ? err_code : ERR_IO, err_msg ? err_msg : "Failed to read");
        return;
    }

    if (eof || !chunk) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, handle_slot, ok, 0, NULL);
        return;
    }

    (void)chunk_len;
    Value ok;
    value_init_string(&ok, chunk);
    free(chunk);
    result_tuple_set(vm, handle_slot, ok, 0, NULL);
}

void builtin_io_read_chunk_bytes(VM* vm) {
    Value* handle_slot = &vm->stack.values[vm->stack.count - 2];
    Value handle_val = *handle_slot;
    Value max_bytes_val = vm->stack.values[vm->stack.count - 1];

    if ((value_get_type(&handle_val) != VAL_FILE && value_get_type(&handle_val) != VAL_SOCKET) ||
        value_get_type(&max_bytes_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, handle_slot, ok, ERR_INVALID_ARGUMENT, "ioReadChunkBytes expects (file|socket, int)");
        return;
    }

    int64_t chunk_size64 = value_get_int(&max_bytes_val);
    if (chunk_size64 < 1 || chunk_size64 > 1024 * 1024) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, handle_slot, ok, ERR_INVALID_ARGUMENT, "maxBytes must be between 1 and 1048576");
        return;
    }

    char* chunk = NULL;
    size_t chunk_len = 0;
    bool eof = false;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    if (!io_read_chunk_alloc(handle_val,
                             (int)chunk_size64,
                             &chunk,
                             &chunk_len,
                             &eof,
                             &err_code,
                             &err_msg)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, handle_slot, ok, err_code ? err_code : ERR_IO, err_msg ? err_msg : "Failed to read");
        return;
    }

    if (eof || !chunk) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, handle_slot, ok, 0, NULL);
        return;
    }

    int max_array_size = (vm->config.max_array_size > 0) ? vm->config.max_array_size : INT_MAX;
    if (chunk_len > (size_t)max_array_size) {
        free(chunk);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, handle_slot, ok, ERR_LIMIT, "ioReadChunkBytes result exceeds max array size");
        return;
    }

    ObjBytes* bytes = obj_bytes_create_copy((const uint8_t*)chunk, (int)chunk_len);
    free(chunk);
    if (!bytes) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, handle_slot, ok, ERR_IO, "Failed to allocate bytes result");
        return;
    }

    Value ok;
    value_init_bytes(&ok, bytes);
    result_tuple_set(vm, handle_slot, ok, 0, NULL);
}

void builtin_io_read_exactly_bytes(VM* vm) {
    Value* handle_slot = &vm->stack.values[vm->stack.count - 2];
    Value handle_val = *handle_slot;
    Value count_val = vm->stack.values[vm->stack.count - 1];

    Value empty_ok;
    value_init_bytes(&empty_ok, obj_bytes_create_with_size(0, 0));

    if ((value_get_type(&handle_val) != VAL_FILE && value_get_type(&handle_val) != VAL_SOCKET) ||
        value_get_type(&count_val) != VAL_INT) {
        result_tuple_set(vm, handle_slot, empty_ok, ERR_INVALID_ARGUMENT, "ioReadExactlyBytes expects (file|socket, int)");
        return;
    }

    int64_t count64 = value_get_int(&count_val);
    if (count64 < 0) {
        result_tuple_set(vm, handle_slot, empty_ok, ERR_INVALID_ARGUMENT, "byteCount must be >= 0");
        return;
    }

    int max_array_size = (vm->config.max_array_size > 0) ? vm->config.max_array_size : INT_MAX;
    if (count64 > (int64_t)max_array_size) {
        result_tuple_set(vm, handle_slot, empty_ok, ERR_LIMIT, "ioReadExactlyBytes result exceeds max array size");
        return;
    }
    if (count64 > INT_MAX) {
        result_tuple_set(vm, handle_slot, empty_ok, ERR_LIMIT, "ioReadExactlyBytes result too large");
        return;
    }

    int target = (int)count64;
    if (target == 0) {
        result_tuple_set(vm, handle_slot, empty_ok, 0, NULL);
        return;
    }

    ObjBytes* out_bytes = obj_bytes_create_with_size(target, 0);
    if (!out_bytes) {
        result_tuple_set(vm, handle_slot, empty_ok, ERR_IO, "Failed to allocate bytes result");
        return;
    }

    uint8_t* out_data = obj_bytes_data(out_bytes);
    if (!out_data) {
        obj_bytes_release(out_bytes);
        result_tuple_set(vm, handle_slot, empty_ok, ERR_IO, "Invalid bytes buffer");
        return;
    }

    int offset = 0;
    while (offset < target) {
        int to_read = target - offset;
        if (to_read > 65536) to_read = 65536;

        char* chunk = NULL;
        size_t chunk_len = 0;
        bool eof = false;
        int64_t err_code = 0;
        const char* err_msg = NULL;
        if (!io_read_chunk_alloc(handle_val,
                                 to_read,
                                 &chunk,
                                 &chunk_len,
                                 &eof,
                                 &err_code,
                                 &err_msg)) {
            obj_bytes_release(out_bytes);
            result_tuple_set(vm,
                             handle_slot,
                             empty_ok,
                             err_code ? err_code : ERR_IO,
                             err_msg ? err_msg : "Failed to read");
            return;
        }

        if (eof || !chunk || chunk_len == 0) {
            if (chunk) free(chunk);
            obj_bytes_release(out_bytes);
            int64_t short_code = value_get_type(&handle_val) == VAL_SOCKET ? ERR_NETWORK : ERR_IO;
            const char* short_msg = value_get_type(&handle_val) == VAL_SOCKET
                ? "connection closed before requested bytes were read"
                : "stream ended before requested bytes were read";
            result_tuple_set(vm, handle_slot, empty_ok, short_code, short_msg);
            return;
        }

        if (chunk_len > (size_t)(target - offset)) {
            free(chunk);
            obj_bytes_release(out_bytes);
            result_tuple_set(vm, handle_slot, empty_ok, ERR_IO, "read exceeded requested byte count");
            return;
        }

        memcpy(out_data + offset, chunk, chunk_len);
        offset += (int)chunk_len;
        free(chunk);
    }

    value_free(&empty_ok);
    Value ok;
    value_init_bytes(&ok, out_bytes);
    result_tuple_set(vm, handle_slot, ok, 0, NULL);
}

void builtin_io_write_all(VM* vm) {
    Value* handle_slot = &vm->stack.values[vm->stack.count - 2];
    Value handle_val = *handle_slot;
    Value data_val = vm->stack.values[vm->stack.count - 1];

    ObjString* data_str = value_get_string_obj(&data_val);
    if ((value_get_type(&handle_val) != VAL_FILE && value_get_type(&handle_val) != VAL_SOCKET) ||
        value_get_type(&data_val) != VAL_STRING || !data_str || !data_str->chars) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, handle_slot, ok, ERR_INVALID_ARGUMENT, "ioWriteAll expects (file|socket, string)");
        return;
    }

    size_t written = 0;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    if (!io_write_all_handle(handle_val,
                             data_str->chars,
                             (size_t)data_str->length,
                             &written,
                             &err_code,
                             &err_msg)) {
        Value ok;
        value_init_int(&ok, (int64_t)written);
        result_tuple_set(vm, handle_slot, ok, err_code ? err_code : ERR_IO, err_msg ? err_msg : "Failed to write");
        return;
    }

    Value ok;
    value_init_int(&ok, (int64_t)written);
    result_tuple_set(vm, handle_slot, ok, 0, NULL);
}

void builtin_io_write_bytes_all(VM* vm) {
    Value* handle_slot = &vm->stack.values[vm->stack.count - 2];
    Value handle_val = *handle_slot;
    Value data_val = vm->stack.values[vm->stack.count - 1];

    ObjBytes* bytes = value_get_bytes_obj(&data_val);
    if ((value_get_type(&handle_val) != VAL_FILE && value_get_type(&handle_val) != VAL_SOCKET) ||
        value_get_type(&data_val) != VAL_BYTES || !bytes) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, handle_slot, ok, ERR_INVALID_ARGUMENT, "ioWriteBytesAll expects (file|socket, bytes)");
        return;
    }

    int byte_count = bytes->length;
    if (byte_count < 0) byte_count = 0;

    uint8_t* data = obj_bytes_data(bytes);
    if (byte_count > 0 && !data) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, handle_slot, ok, ERR_IO, "Invalid bytes buffer");
        return;
    }

    size_t written = 0;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    if (!io_write_all_handle(handle_val,
                             (const char*)data,
                             (size_t)byte_count,
                             &written,
                             &err_code,
                             &err_msg)) {
        Value ok;
        value_init_int(&ok, (int64_t)written);
        result_tuple_set(vm, handle_slot, ok, err_code ? err_code : ERR_IO, err_msg ? err_msg : "Failed to write");
        return;
    }

    Value ok;
    value_init_int(&ok, (int64_t)written);
    result_tuple_set(vm, handle_slot, ok, 0, NULL);
}

void builtin_io_copy(VM* vm) {
    Value* reader_slot = &vm->stack.values[vm->stack.count - 3];
    Value reader_val = *reader_slot;
    Value writer_val = vm->stack.values[vm->stack.count - 2];
    Value chunk_val = vm->stack.values[vm->stack.count - 1];

    if ((value_get_type(&reader_val) != VAL_FILE && value_get_type(&reader_val) != VAL_SOCKET) ||
        (value_get_type(&writer_val) != VAL_FILE && value_get_type(&writer_val) != VAL_SOCKET) ||
        value_get_type(&chunk_val) != VAL_INT) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, reader_slot, ok, ERR_INVALID_ARGUMENT, "ioCopy expects (file|socket, file|socket, int)");
        return;
    }

    int64_t chunk_size64 = value_get_int(&chunk_val);
    if (chunk_size64 < 1 || chunk_size64 > 1024 * 1024) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, reader_slot, ok, ERR_INVALID_ARGUMENT, "chunkBytes must be between 1 and 1048576");
        return;
    }

    int64_t total = 0;
    while (true) {
        char* chunk = NULL;
        size_t chunk_len = 0;
        bool eof = false;
        int64_t read_err_code = 0;
        const char* read_err_msg = NULL;
        if (!io_read_chunk_alloc(reader_val,
                                 (int)chunk_size64,
                                 &chunk,
                                 &chunk_len,
                                 &eof,
                                 &read_err_code,
                                 &read_err_msg)) {
            Value ok;
            value_init_int(&ok, total);
            result_tuple_set(vm, reader_slot, ok, read_err_code ? read_err_code : ERR_IO, read_err_msg ? read_err_msg : "Failed to read");
            return;
        }

        if (eof) break;

        size_t wrote = 0;
        int64_t write_err_code = 0;
        const char* write_err_msg = NULL;
        if (!io_write_all_handle(writer_val, chunk, chunk_len, &wrote, &write_err_code, &write_err_msg)) {
            free(chunk);
            Value ok;
            value_init_int(&ok, total);
            result_tuple_set(vm, reader_slot, ok, write_err_code ? write_err_code : ERR_IO, write_err_msg ? write_err_msg : "Failed to write");
            return;
        }
        free(chunk);

        if ((int64_t)wrote < 0 || total > INT64_MAX - (int64_t)wrote) {
            Value ok;
            value_init_int(&ok, total);
            result_tuple_set(vm, reader_slot, ok, ERR_LIMIT, "ioCopy byte count overflow");
            return;
        }
        total += (int64_t)wrote;
    }

    Value ok;
    value_init_int(&ok, total);
    result_tuple_set(vm, reader_slot, ok, 0, NULL);
}

void builtin_stdout_write_bytes(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value bytes_val = *slot;

    ObjBytes* bytes = value_get_bytes_obj(&bytes_val);
    if (value_get_type(&bytes_val) != VAL_BYTES || !bytes) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "stdoutWriteBytes expects bytes");
        return;
    }

    int count = bytes->length;
    if (count < 0) count = 0;

    uint8_t* data = obj_bytes_data(bytes);

#ifdef _WIN32
    int stdout_fd = _fileno(stdout);
    int old_mode = -1;
    if (stdout_fd >= 0) {
        old_mode = _setmode(stdout_fd, _O_BINARY);
    }
#endif

    const char* write_err = NULL;
    if (count > 0) {
        if (!data) {
            write_err = "Invalid bytes buffer";
        } else {
            size_t written = fwrite(data, 1, (size_t)count, stdout);
            if (written != (size_t)count || ferror(stdout)) {
                write_err = "Failed to write to stdout";
            }
        }
    }

#ifdef _WIN32
    if (stdout_fd >= 0 && old_mode != -1) {
        _setmode(stdout_fd, old_mode);
    }
#endif

    Value ok;
    value_init_bool(&ok, write_err == NULL);
    result_tuple_set(vm, slot, ok, write_err ? ERR_IO : 0, write_err);
}

void builtin_write_bytes(VM* vm) {
    // arg0 (file) at count-2, arg1 (bytes) at count-1
    Value* file_slot = &vm->stack.values[vm->stack.count - 2];
    Value file_val = *file_slot;
    Value bytes_val = vm->stack.values[vm->stack.count - 1];

    ObjString* file_str = value_get_string_obj(&file_val);
    ObjBytes* bytes = value_get_bytes_obj(&bytes_val);
    if (value_get_type(&file_val) != VAL_STRING || !file_str || !file_str->chars ||
        value_get_type(&bytes_val) != VAL_BYTES || !bytes) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, file_slot, ok, ERR_INVALID_ARGUMENT, "writeBytes expects (string, bytes)");
        return;
    }

    const char* resolve_err = NULL;
    char* path = resolve_file_path(vm, file_str->chars, &resolve_err);
    if (!path) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, file_slot, ok, ERR_PERMISSION, resolve_err ? resolve_err : "Path not allowed");
        return;
    }

    int64_t open_err_code = 0;
    const char* open_err_msg = NULL;
    FILE* file = NULL;
    if (!open_file_with_limit(vm, path, "wb", &file, &open_err_code, &open_err_msg)) {
        free(path);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         file_slot,
                         ok,
                         open_err_code ? open_err_code : ERR_IO,
                         open_err_msg ? open_err_msg : "Failed to open file");
        return;
    }

    int count = bytes->length;
    if (count < 0) count = 0;

    if (count > 0) {
        uint8_t* data = obj_bytes_data(bytes);
        if (!data) {
            close_file_with_limit(vm, &file);
            free(path);
            Value ok;
            value_init_bool(&ok, false);
            result_tuple_set(vm, file_slot, ok, ERR_IO, "Invalid bytes buffer");
            return;
        }

        size_t written = fwrite(data, 1, (size_t)count, file);
        int had_error = ferror(file);

        if (written != (size_t)count || had_error) {
            close_file_with_limit(vm, &file);
            free(path);
            Value ok;
            value_init_bool(&ok, false);
            result_tuple_set(vm, file_slot, ok, ERR_IO, "Failed to write file");
            return;
        }
    }

    close_file_with_limit(vm, &file);
    free(path);

    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, file_slot, ok, 0, NULL);
}

void builtin_append_bytes(VM* vm) {
    // arg0 (file) at count-2, arg1 (bytes) at count-1
    Value* file_slot = &vm->stack.values[vm->stack.count - 2];
    Value file_val = *file_slot;
    Value bytes_val = vm->stack.values[vm->stack.count - 1];

    ObjString* file_str = value_get_string_obj(&file_val);
    ObjBytes* bytes = value_get_bytes_obj(&bytes_val);
    if (value_get_type(&file_val) != VAL_STRING || !file_str || !file_str->chars ||
        value_get_type(&bytes_val) != VAL_BYTES || !bytes) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, file_slot, ok, ERR_INVALID_ARGUMENT, "appendBytes expects (string, bytes)");
        return;
    }

    const char* resolve_err = NULL;
    char* path = resolve_file_path(vm, file_str->chars, &resolve_err);
    if (!path) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, file_slot, ok, ERR_PERMISSION, resolve_err ? resolve_err : "Path not allowed");
        return;
    }

    int64_t open_err_code = 0;
    const char* open_err_msg = NULL;
    FILE* file = NULL;
    if (!open_file_with_limit(vm, path, "ab", &file, &open_err_code, &open_err_msg)) {
        free(path);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         file_slot,
                         ok,
                         open_err_code ? open_err_code : ERR_IO,
                         open_err_msg ? open_err_msg : "Failed to open file");
        return;
    }

    int count = bytes->length;
    if (count < 0) count = 0;

    if (count > 0) {
        uint8_t* data = obj_bytes_data(bytes);
        if (!data) {
            close_file_with_limit(vm, &file);
            free(path);
            Value ok;
            value_init_bool(&ok, false);
            result_tuple_set(vm, file_slot, ok, ERR_IO, "Invalid bytes buffer");
            return;
        }

        size_t written = fwrite(data, 1, (size_t)count, file);
        int had_error = ferror(file);

        if (written != (size_t)count || had_error) {
            close_file_with_limit(vm, &file);
            free(path);
            Value ok;
            value_init_bool(&ok, false);
            result_tuple_set(vm, file_slot, ok, ERR_IO, "Failed to write file");
            return;
        }
    }

    close_file_with_limit(vm, &file);
    free(path);

    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, file_slot, ok, 0, NULL);
}

void builtin_read_bytes(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value file_val = *slot;

    ObjString* file_str = value_get_string_obj(&file_val);
    if (value_get_type(&file_val) != VAL_STRING || !file_str || !file_str->chars) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "readBytes expects string argument");
        return;
    }

    const char* resolve_err = NULL;
    char* path = resolve_file_path(vm, file_str->chars, &resolve_err);
    if (!path) {
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_PERMISSION, resolve_err ? resolve_err : "Path not allowed");
        return;
    }

    int64_t open_err_code = 0;
    const char* open_err_msg = NULL;
    FILE* file = NULL;
    if (!open_file_with_limit(vm, path, "rb", &file, &open_err_code, &open_err_msg)) {
        free(path);
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm,
                         slot,
                         ok,
                         open_err_code ? open_err_code : ERR_IO,
                         open_err_msg ? open_err_msg : "Failed to open file");
        return;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        close_file_with_limit(vm, &file);
        free(path);
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_IO, "Failed to read file");
        return;
    }

    long length = ftell(file);
    if (length < 0) {
        close_file_with_limit(vm, &file);
        free(path);
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_IO, "Failed to read file");
        return;
    }

    if (vm->config.max_array_size > 0 && length > (long)vm->config.max_array_size) {
        close_file_with_limit(vm, &file);
        free(path);
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "readBytes result exceeds max array size");
        return;
    }

    if (length > INT_MAX) {
        close_file_with_limit(vm, &file);
        free(path);
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "readBytes result too large");
        return;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        close_file_with_limit(vm, &file);
        free(path);
        Value ok;
        value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
        result_tuple_set(vm, slot, ok, ERR_IO, "Failed to read file");
        return;
    }

    int count = (int)length;
    ObjBytesBuffer* buf = obj_bytes_buffer_create(count);
    ObjBytes* bytes = obj_bytes_create(buf, 0, count);
    obj_bytes_buffer_release(buf);
    if (count > 0) {
        uint8_t* data = obj_bytes_data(bytes);
        if (!data) {
            close_file_with_limit(vm, &file);
            free(path);
            obj_bytes_release(bytes);
            Value ok;
            value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
            result_tuple_set(vm, slot, ok, ERR_IO, "Invalid bytes buffer");
            return;
        }

        size_t read = fread(data, 1, (size_t)count, file);
        int had_error = ferror(file);
        if (had_error || read != (size_t)count) {
            close_file_with_limit(vm, &file);
            free(path);
            obj_bytes_release(bytes);
            Value ok;
            value_init_bytes(&ok, obj_bytes_create_with_size(0, 0));
            result_tuple_set(vm, slot, ok, ERR_IO, "Failed to read file");
            return;
        }
    }

    close_file_with_limit(vm, &file);
    free(path);

    Value result;
    value_init_bytes(&result, bytes);
    result_tuple_set(vm, slot, result, 0, NULL);
}

void builtin_env_get(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value name_val = *slot;

    Value ok;
    value_init_nil(&ok);

    ObjString* name_str = value_get_string_obj(&name_val);
    if (value_get_type(&name_val) != VAL_STRING || !name_str || !name_str->chars) {
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "envGet requires string argument");
        return;
    }

    const char* name = name_str->chars;
    if (!name || name[0] == '\0') {
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "envGet requires non-empty variable name");
        return;
    }
    if (strchr(name, '=') != NULL) {
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "envGet variable name cannot contain '='");
        return;
    }

#ifdef _WIN32
    char* env_value = NULL;
    size_t env_len = 0;
    errno_t rc = _dupenv_s(&env_value, &env_len, name);
    if (rc != 0) {
        result_tuple_set(vm, slot, ok, ERR_INTERNAL, "envGet failed to read environment variable");
        return;
    }
    if (!env_value) {
        result_tuple_set(vm, slot, ok, 0, NULL);
        return;
    }
    size_t value_len = strlen(env_value);
#else
    const char* env_value = getenv(name);
    if (!env_value) {
        result_tuple_set(vm, slot, ok, 0, NULL);
        return;
    }
    size_t value_len = strlen(env_value);
#endif

    int max_len = (vm && vm->config.max_string_length > 0) ? vm->config.max_string_length : INT_MAX;
    if ((int64_t)value_len > (int64_t)max_len) {
#ifdef _WIN32
        free(env_value);
#endif
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "envGet value exceeds max string length");
        return;
    }

    value_init_string(&ok, env_value);
#ifdef _WIN32
    free(env_value);
#endif
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_exists(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value file_val = *slot;

    ObjString* file_str = value_get_string_obj(&file_val);
    if (value_get_type(&file_val) != VAL_STRING || !file_str || !file_str->chars) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "exists requires string argument");
        return;
    }

    const char* resolve_err = NULL;
    char* path = resolve_file_path(vm, file_str->chars, &resolve_err);
    if (!path) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_PERMISSION, resolve_err ? resolve_err : "Path not allowed");
        return;
    }

    bool exists = (access(path, F_OK) == 0);
    const char* err = NULL;
    if (!exists && errno != ENOENT) {
        err = "Failed to check file existence";
    }
    free(path);
    Value ok;
    value_init_bool(&ok, exists);
    result_tuple_set(vm, slot, ok, err ? ERR_IO : 0, err);
}

void builtin_delete(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value file_val = *slot;

    ObjString* file_str = value_get_string_obj(&file_val);
    if (value_get_type(&file_val) != VAL_STRING || !file_str || !file_str->chars) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "delete requires string argument");
        return;
    }

    const char* resolve_err = NULL;
    char* path = resolve_file_path(vm, file_str->chars, &resolve_err);
    if (!path) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_PERMISSION, resolve_err ? resolve_err : "Path not allowed");
        return;
    }

    int result = remove(path);
    const char* err = NULL;
    if (result != 0) {
        err = "Failed to delete file";
    }
    free(path);
    Value ok;
    value_init_bool(&ok, result == 0);
    result_tuple_set(vm, slot, ok, err ? ERR_IO : 0, err);
}

void builtin_map_get(VM* vm) {
    // arg0 (map) at count-2, arg1 (key) at count-1
    Value* map_slot = &vm->stack.values[vm->stack.count - 2];
    Value key_val = vm->stack.values[vm->stack.count - 1];
    Value map_val = *map_slot;
    
    ObjMap* map_obj = value_get_map_obj(&map_val);
    if (value_get_type(&map_val) != VAL_MAP || !map_obj) {
        Value nil;
        value_init_nil(&nil);
        value_free(map_slot);
        *map_slot = nil;
        return;
    }

    Value result = (value_get_type(&key_val) == VAL_STRING)
        ? obj_map_get_string(map_obj, value_get_string_obj(&key_val))
        : obj_map_get(map_obj, key_val);
    value_free(map_slot);
    *map_slot = result;
}

void builtin_map_set(VM* vm) {
    // arg0 (map) at count-3, arg1 (key) at count-2, arg2 (value) at count-1
    Value* map_slot = &vm->stack.values[vm->stack.count - 3];
    Value key_val = vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];
    Value map_val = *map_slot;
    
    ObjMap* map_obj = value_get_map_obj(&map_val);
    if (value_get_type(&map_val) != VAL_MAP || !map_obj) {
        Value nil;
        value_init_nil(&nil);
        value_free(map_slot);
        *map_slot = nil;
        return;
    }

    if (value_get_type(&key_val) == VAL_STRING) {
        obj_map_set_string(map_obj, value_get_string_obj(&key_val), value_val);
    } else {
        obj_map_set(map_obj, key_val, value_val);
    }
    // mapSet returns void: drop the map argument so call cleanup can discard remaining args.
    Value nil;
    value_init_nil(&nil);
    value_free(map_slot);
    *map_slot = nil;
}

void builtin_map_has(VM* vm) {
    // arg0 (map) at count-2, arg1 (key) at count-1
    Value* map_slot = &vm->stack.values[vm->stack.count - 2];
    Value key_val = vm->stack.values[vm->stack.count - 1];
    Value map_val = *map_slot;
    
    ObjMap* map_obj = value_get_map_obj(&map_val);
    if (value_get_type(&map_val) != VAL_MAP || !map_obj) {
        Value result;
        value_init_bool(&result, false);
        value_free(map_slot);
        *map_slot = result;
        return;
    }

    Value result;
    value_init_bool(&result, value_get_type(&key_val) == VAL_STRING
        ? obj_map_has_string(map_obj, value_get_string_obj(&key_val))
        : obj_map_has(map_obj, key_val));
    value_free(map_slot);
    *map_slot = result;
}

void builtin_map_get_string(VM* vm) {
    // arg0 (map) at count-2, arg1 (key:string) at count-1
    Value* map_slot = &vm->stack.values[vm->stack.count - 2];
    Value key_val = vm->stack.values[vm->stack.count - 1];
    Value map_val = *map_slot;

    ObjMap* map_obj = value_get_map_obj(&map_val);
    ObjString* key_str = value_get_string_obj(&key_val);
    if (value_get_type(&map_val) != VAL_MAP || !map_obj ||
        value_get_type(&key_val) != VAL_STRING || !key_str) {
        Value nil;
        value_init_nil(&nil);
        value_free(map_slot);
        *map_slot = nil;
        return;
    }

    Value result = obj_map_get_string(map_obj, key_str);
    value_free(map_slot);
    *map_slot = result;
}

void builtin_map_set_string(VM* vm) {
    // arg0 (map) at count-3, arg1 (key:string) at count-2, arg2 (value) at count-1
    Value* map_slot = &vm->stack.values[vm->stack.count - 3];
    Value key_val = vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];
    Value map_val = *map_slot;

    ObjMap* map_obj = value_get_map_obj(&map_val);
    ObjString* key_str = value_get_string_obj(&key_val);
    if (value_get_type(&map_val) == VAL_MAP && map_obj &&
        value_get_type(&key_val) == VAL_STRING && key_str) {
        obj_map_set_string(map_obj, key_str, value_val);
    }

    // mapSetString returns void.
    Value nil;
    value_init_nil(&nil);
    value_free(map_slot);
    *map_slot = nil;
}

void builtin_map_has_string(VM* vm) {
    // arg0 (map) at count-2, arg1 (key:string) at count-1
    Value* map_slot = &vm->stack.values[vm->stack.count - 2];
    Value key_val = vm->stack.values[vm->stack.count - 1];
    Value map_val = *map_slot;

    Value result;
    value_init_bool(&result, false);
    ObjMap* map_obj = value_get_map_obj(&map_val);
    ObjString* key_str = value_get_string_obj(&key_val);
    if (value_get_type(&map_val) == VAL_MAP && map_obj &&
        value_get_type(&key_val) == VAL_STRING && key_str) {
        value_init_bool(&result, obj_map_has_string(map_obj, key_str));
    }
    value_free(map_slot);
    *map_slot = result;
}

void builtin_map_delete(VM* vm) {
    // arg0 (map) at count-2, arg1 (key) at count-1
    Value* map_slot = &vm->stack.values[vm->stack.count - 2];
    Value key_val = vm->stack.values[vm->stack.count - 1];
    Value map_val = *map_slot;
    
    ObjMap* map_obj = value_get_map_obj(&map_val);
    if (value_get_type(&map_val) != VAL_MAP || !map_obj) {
        Value nil;
        value_init_nil(&nil);
        value_free(map_slot);
        *map_slot = nil;
        return;
    }

    if (value_get_type(&key_val) == VAL_STRING) {
        obj_map_delete_string(map_obj, value_get_string_obj(&key_val));
    } else {
        obj_map_delete(map_obj, key_val);
    }
    // mapDelete returns void.
    Value nil;
    value_init_nil(&nil);
    value_free(map_slot);
    *map_slot = nil;
}

void builtin_map_delete_string(VM* vm) {
    // arg0 (map) at count-2, arg1 (key:string) at count-1
    Value* map_slot = &vm->stack.values[vm->stack.count - 2];
    Value key_val = vm->stack.values[vm->stack.count - 1];
    Value map_val = *map_slot;

    ObjMap* map_obj = value_get_map_obj(&map_val);
    ObjString* key_str = value_get_string_obj(&key_val);
    if (value_get_type(&map_val) == VAL_MAP && map_obj &&
        value_get_type(&key_val) == VAL_STRING && key_str) {
        obj_map_delete_string(map_obj, key_str);
    }

    // mapDeleteString returns void.
    Value nil;
    value_init_nil(&nil);
    value_free(map_slot);
    *map_slot = nil;
}

void builtin_map_count(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value map_val = *slot;
    
    ObjMap* map_obj = value_get_map_obj(&map_val);
    if (value_get_type(&map_val) != VAL_MAP || !map_obj) {
        value_init_int(slot, 0);
        return;
    }
    
    value_init_int(slot, map_obj->count);
}

void builtin_set_add(VM* vm) {
    // arg0 (set) at count-2, arg1 (value) at count-1
    Value* set_slot = &vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];
    Value set_val = *set_slot;
    
    ObjSet* set_obj = value_get_set_obj(&set_val);
    if (value_get_type(&set_val) != VAL_SET || !set_obj) {
        Value nil;
        value_init_nil(&nil);
        value_free(set_slot);
        *set_slot = nil;
        return;
    }
    
    if (value_get_type(&value_val) == VAL_STRING && value_get_string_obj(&value_val)) {
        obj_set_add_string(set_obj, value_get_string_obj(&value_val));
    } else {
        obj_set_add(set_obj, value_val);
    }
    // setAdd returns void.
    Value nil;
    value_init_nil(&nil);
    value_free(set_slot);
    *set_slot = nil;
}

void builtin_set_add_string(VM* vm) {
    // arg0 (set) at count-2, arg1 (value:string) at count-1
    Value* set_slot = &vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];
    Value set_val = *set_slot;

    ObjSet* set_obj = value_get_set_obj(&set_val);
    ObjString* value_str = value_get_string_obj(&value_val);
    if (value_get_type(&set_val) == VAL_SET && set_obj &&
        value_get_type(&value_val) == VAL_STRING && value_str) {
        obj_set_add_string(set_obj, value_str);
    }

    // setAddString returns void.
    Value nil;
    value_init_nil(&nil);
    value_free(set_slot);
    *set_slot = nil;
}

void builtin_set_has(VM* vm) {
    // arg0 (set) at count-2, arg1 (value) at count-1
    Value* set_slot = &vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];
    Value set_val = *set_slot;
    
    ObjSet* set_obj = value_get_set_obj(&set_val);
    if (value_get_type(&set_val) != VAL_SET || !set_obj) {
        Value result;
        value_init_bool(&result, false);
        value_free(set_slot);
        *set_slot = result;
        return;
    }
    
    Value result;
    if (value_get_type(&value_val) == VAL_STRING && value_get_string_obj(&value_val)) {
        value_init_bool(&result, obj_set_has_string(set_obj, value_get_string_obj(&value_val)));
    } else {
        value_init_bool(&result, obj_set_has(set_obj, value_val));
    }
    value_free(set_slot);
    *set_slot = result;
}

void builtin_set_has_string(VM* vm) {
    // arg0 (set) at count-2, arg1 (value:string) at count-1
    Value* set_slot = &vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];
    Value set_val = *set_slot;

    Value result;
    ObjSet* set_obj = value_get_set_obj(&set_val);
    ObjString* value_str = value_get_string_obj(&value_val);
    if (value_get_type(&set_val) == VAL_SET && set_obj &&
        value_get_type(&value_val) == VAL_STRING && value_str) {
        value_init_bool(&result, obj_set_has_string(set_obj, value_str));
    } else {
        value_init_bool(&result, false);
    }
    value_free(set_slot);
    *set_slot = result;
}

void builtin_set_remove(VM* vm) {
    // arg0 (set) at count-2, arg1 (value) at count-1
    Value* set_slot = &vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];
    Value set_val = *set_slot;
    
    ObjSet* set_obj = value_get_set_obj(&set_val);
    if (value_get_type(&set_val) != VAL_SET || !set_obj) {
        Value nil;
        value_init_nil(&nil);
        value_free(set_slot);
        *set_slot = nil;
        return;
    }
    
    if (value_get_type(&value_val) == VAL_STRING && value_get_string_obj(&value_val)) {
        obj_set_remove_string(set_obj, value_get_string_obj(&value_val));
    } else {
        obj_set_remove(set_obj, value_val);
    }
    // setRemove returns void.
    Value nil;
    value_init_nil(&nil);
    value_free(set_slot);
    *set_slot = nil;
}

void builtin_set_remove_string(VM* vm) {
    // arg0 (set) at count-2, arg1 (value:string) at count-1
    Value* set_slot = &vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];
    Value set_val = *set_slot;

    ObjSet* set_obj = value_get_set_obj(&set_val);
    ObjString* value_str = value_get_string_obj(&value_val);
    if (value_get_type(&set_val) == VAL_SET && set_obj &&
        value_get_type(&value_val) == VAL_STRING && value_str) {
        obj_set_remove_string(set_obj, value_str);
    }

    // setRemoveString returns void.
    Value nil;
    value_init_nil(&nil);
    value_free(set_slot);
    *set_slot = nil;
}

void builtin_set_count(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value set_val = *slot;
    
    ObjSet* set_obj = value_get_set_obj(&set_val);
    if (value_get_type(&set_val) != VAL_SET || !set_obj) {
        value_init_int(slot, 0);
        return;
    }
    
    value_init_int(slot, set_obj->count);
}

void builtin_set_to_array(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value set_val = *slot;
    
    ObjSet* set_obj = value_get_set_obj(&set_val);
    if (value_get_type(&set_val) != VAL_SET || !set_obj) {
        Value nil;
        value_init_nil(&nil);
        value_free(slot);
        *slot = nil;
        return;
    }
    
    ObjArray* result_arr = obj_array_create(vm, set_obj->count);
    obj_set_to_array(set_obj, result_arr);
    Value result;
    value_init_array(&result, result_arr);
    value_free(slot);
    *slot = result;
}

// ============ String Functions ============

void builtin_substring(VM* vm) {
    // arg0 (string) at count-3, arg1 (start) at count-2, arg2 (len) at count-1
    Value* str_slot = &vm->stack.values[vm->stack.count - 3];
    Value start_val = vm->stack.values[vm->stack.count - 2];
    Value len_val = vm->stack.values[vm->stack.count - 1];
    Value str_val = *str_slot;
    
    ObjString* str_obj = value_get_string_obj(&str_val);
    if (value_get_type(&str_val) != VAL_STRING || !str_obj ||
        value_get_type(&start_val) != VAL_INT || value_get_type(&len_val) != VAL_INT) {
        Value nil;
        value_init_nil(&nil);
        value_free(str_slot);
        *str_slot = nil;
        return;
    }
    
    int start = (int)value_get_int(&start_val);
    int len = (int)value_get_int(&len_val);
    int str_len = str_obj->length;
    
    // Handle negative start index
    if (start < 0) start = 0;
    if (start > str_len) start = str_len;
    
    // Handle negative or too large length
    if (len < 0) len = 0;
    if (start + len > str_len) len = str_len - start;
    
    // Create substring
    char* result = (char*)safe_malloc(len + 1);
    memcpy(result, str_obj->chars + start, len);
    result[len] = '\0';
    
    Value result_val;
    value_init_string(&result_val, result);
    free(result);
    
    value_free(str_slot);
    *str_slot = result_val;
}

void builtin_find(VM* vm) {
    // arg0 (string) at count-2, arg1 (pattern) at count-1
    Value* str_slot = &vm->stack.values[vm->stack.count - 2];
    Value pattern_val = vm->stack.values[vm->stack.count - 1];
    Value str_val = *str_slot;
    
    ObjString* str_obj = value_get_string_obj(&str_val);
    ObjString* pattern_obj = value_get_string_obj(&pattern_val);
    if (value_get_type(&str_val) != VAL_STRING || !str_obj ||
        value_get_type(&pattern_val) != VAL_STRING || !pattern_obj) {
        Value result;
        value_init_int(&result, -1);
        value_free(str_slot);
        *str_slot = result;
        return;
    }
    
    const char* str = str_obj->chars;
    const char* pattern = pattern_obj->chars;
    
    const char* found = strstr(str, pattern);
    Value result;
    if (found) {
        value_init_int(&result, (int)(found - str));
    } else {
        value_init_int(&result, -1);
    }
    
    value_free(str_slot);
    *str_slot = result;
}

void builtin_split(VM* vm) {
    // arg0 (string) at count-2, arg1 (delimiter) at count-1
    Value* str_slot = &vm->stack.values[vm->stack.count - 2];
    Value delim_val = vm->stack.values[vm->stack.count - 1];
    Value str_val = *str_slot;
    
    ObjString* str_obj = value_get_string_obj(&str_val);
    ObjString* delim_obj = value_get_string_obj(&delim_val);
    if (value_get_type(&str_val) != VAL_STRING || !str_obj ||
        value_get_type(&delim_val) != VAL_STRING || !delim_obj) {
        Value nil;
        value_init_nil(&nil);
        value_free(str_slot);
        *str_slot = nil;
        return;
    }
    
    const char* str = str_obj->chars;
    const char* delim = delim_obj->chars;
    int delim_len = delim_obj->length;
    
    ObjArray* result_arr = obj_array_create(vm, 4);
    
    if (delim_len == 0) {
        // Split into individual characters
        for (const char* p = str; *p; p++) {
            char ch[2] = {*p, '\0'};
            Value ch_val;
            value_init_string(&ch_val, ch);
            obj_array_push(result_arr, ch_val);
        }
    } else {
        const char* start = str;
        const char* p = str;
        
        while ((p = strstr(p, delim)) != NULL) {
            int len = (int)(p - start);
            char* token = (char*)safe_malloc(len + 1);
            memcpy(token, start, len);
            token[len] = '\0';
            
            Value token_val;
            value_init_string(&token_val, token);
            free(token);
            obj_array_push(result_arr, token_val);
            
            p += delim_len;
            start = p;
        }
        
        // Add remaining part
        Value last_val;
        value_init_string(&last_val, start);
        obj_array_push(result_arr, last_val);
    }
    
    Value result;
    value_init_array(&result, result_arr);
    value_free(str_slot);
    *str_slot = result;
}

void builtin_trim(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value str_val = *slot;
    
    ObjString* str_obj = value_get_string_obj(&str_val);
    if (value_get_type(&str_val) != VAL_STRING || !str_obj) {
        Value nil;
        value_init_nil(&nil);
        value_free(slot);
        *slot = nil;
        return;
    }
    
    const char* str = str_obj->chars;
    int len = str_obj->length;
    
    // Trim leading whitespace
    const char* start = str;
    while (*start && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')) {
        start++;
    }
    
    // Trim trailing whitespace
    const char* end = str + len - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        end--;
    }
    
    int new_len = (int)(end - start + 1);
    char* result = (char*)safe_malloc(new_len + 1);
    memcpy(result, start, new_len);
    result[new_len] = '\0';
    
    Value result_val;
    value_init_string(&result_val, result);
    free(result);
    
    value_free(slot);
    *slot = result_val;
}

void builtin_starts_with(VM* vm) {
    // arg0 (string) at count-2, arg1 (prefix) at count-1
    Value* str_slot = &vm->stack.values[vm->stack.count - 2];
    Value prefix_val = vm->stack.values[vm->stack.count - 1];
    Value str_val = *str_slot;
    
    ObjString* str_obj = value_get_string_obj(&str_val);
    ObjString* prefix_obj = value_get_string_obj(&prefix_val);
    if (value_get_type(&str_val) != VAL_STRING || !str_obj ||
        value_get_type(&prefix_val) != VAL_STRING || !prefix_obj) {
        Value result;
        value_init_bool(&result, false);
        value_free(str_slot);
        *str_slot = result;
        return;
    }
    
    const char* str = str_obj->chars;
    const char* prefix = prefix_obj->chars;
    int prefix_len = prefix_obj->length;
    int str_len = str_obj->length;
    
    Value result;
    if (prefix_len > str_len) {
        value_init_bool(&result, false);
    } else {
        value_init_bool(&result, strncmp(str, prefix, prefix_len) == 0);
    }
    
    value_free(str_slot);
    *str_slot = result;
}

void builtin_ends_with(VM* vm) {
    // arg0 (string) at count-2, arg1 (suffix) at count-1
    Value* str_slot = &vm->stack.values[vm->stack.count - 2];
    Value suffix_val = vm->stack.values[vm->stack.count - 1];
    Value str_val = *str_slot;
    
    ObjString* str_obj = value_get_string_obj(&str_val);
    ObjString* suffix_obj = value_get_string_obj(&suffix_val);
    if (value_get_type(&str_val) != VAL_STRING || !str_obj ||
        value_get_type(&suffix_val) != VAL_STRING || !suffix_obj) {
        Value result;
        value_init_bool(&result, false);
        value_free(str_slot);
        *str_slot = result;
        return;
    }
    
    const char* str = str_obj->chars;
    const char* suffix = suffix_obj->chars;
    int suffix_len = suffix_obj->length;
    int str_len = str_obj->length;
    
    Value result;
    if (suffix_len > str_len) {
        value_init_bool(&result, false);
    } else {
        value_init_bool(&result, strcmp(str + str_len - suffix_len, suffix) == 0);
    }
    
    value_free(str_slot);
    *str_slot = result;
}

void builtin_replace(VM* vm) {
    // arg0 (string) at count-3, arg1 (old) at count-2, arg2 (new) at count-1
    Value* str_slot = &vm->stack.values[vm->stack.count - 3];
    Value old_val = vm->stack.values[vm->stack.count - 2];
    Value new_val = vm->stack.values[vm->stack.count - 1];
    Value str_val = *str_slot;
    
    ObjString* str_obj = value_get_string_obj(&str_val);
    ObjString* old_obj = value_get_string_obj(&old_val);
    ObjString* new_obj = value_get_string_obj(&new_val);
    if (value_get_type(&str_val) != VAL_STRING || !str_obj ||
        value_get_type(&old_val) != VAL_STRING || !old_obj ||
        value_get_type(&new_val) != VAL_STRING || !new_obj) {
        Value nil;
        value_init_nil(&nil);
        value_free(str_slot);
        *str_slot = nil;
        return;
    }
    
    const char* str = str_obj->chars;
    const char* old_str = old_obj->chars;
    const char* new_str = new_obj->chars;
    int old_len = old_obj->length;
    int new_len = new_obj->length;
    
    if (old_len == 0) {
        // Nothing to replace, return original
        value_retain(&str_val);
        value_free(str_slot);
        *str_slot = str_val;
        return;
    }
    
    // Count non-overlapping occurrences.
    int64_t count = 0;
    const char* p = str;
    while ((p = strstr(p, old_str)) != NULL) {
        count++;
        p += old_len;
    }
    
    if (count == 0) {
        // No occurrences, return original
        value_retain(&str_val);
        value_free(str_slot);
        *str_slot = str_val;
        return;
    }
    
    // Calculate the resulting length using wide arithmetic to avoid overflow.
    int64_t str_len64 = (int64_t)str_obj->length;
    int64_t delta64 = (int64_t)new_len - (int64_t)old_len;
    int64_t result_len64 = str_len64 + (count * delta64);
    if (result_len64 < 0 || result_len64 > (int64_t)INT_MAX) {
        vm_runtime_error(vm, "replace result exceeds max string length");
        return;
    }
    int result_len = (int)result_len64;
    char* result = (char*)safe_malloc((size_t)result_len + 1u);
    
    // Build result
    char* dst = result;
    const char* src = str;
    p = str;
    while ((p = strstr(p, old_str)) != NULL) {
        size_t segment_len = (size_t)(p - src);
        memcpy(dst, src, segment_len);
        dst += segment_len;
        memcpy(dst, new_str, (size_t)new_len);
        dst += (size_t)new_len;
        p += old_len;
        src = p;
    }
    // Copy remaining
    size_t consumed = (size_t)(src - str);
    size_t tail_len = (size_t)str_obj->length - consumed;
    memcpy(dst, src, tail_len);
    dst += tail_len;
    *dst = '\0';
    
    Value result_val;
    value_init_string(&result_val, result);
    free(result);
    
    value_free(str_slot);
    *str_slot = result_val;
}

// ============ BigInt Functions ============

static ObjBigInt* bigint_clone_value(const ObjBigInt* src) {
    if (!src) {
        return obj_bigint_from_int64(0);
    }
    obj_bigint_retain((ObjBigInt*)src);
    return (ObjBigInt*)src;
}

static ObjBigInt* bigint_abs_clone_value(const ObjBigInt* src) {
    if (!src) {
        return obj_bigint_from_int64(0);
    }
    if (src->sign < 0) {
        return obj_bigint_negate(src);
    }
    obj_bigint_retain((ObjBigInt*)src);
    return (ObjBigInt*)src;
}

static int bigint_compare_abs_value(const ObjBigInt* a, const ObjBigInt* b) {
    if (!a || !b) return 0;
    return obj_bigint_compare_abs(a, b);
}

static ObjBigInt* bigint_mod_positive(const ObjBigInt* a, const ObjBigInt* mod) {
    bool div_by_zero = false;
    ObjBigInt* rem = obj_bigint_mod(a, mod, &div_by_zero);
    if (!rem) return obj_bigint_from_int64(0);
    if (rem->sign < 0) {
        ObjBigInt* tmp = obj_bigint_add(rem, mod);
        obj_bigint_release(rem);
        rem = tmp;
    }
    return rem;
}

static ObjBigInt* bigint_gcd_value(const ObjBigInt* a, const ObjBigInt* b) {
    ObjBigInt* x = bigint_abs_clone_value(a);
    ObjBigInt* y = bigint_abs_clone_value(b);

    while (y->sign != 0) {
        ObjBigInt* r = obj_bigint_mod(x, y, NULL);
        obj_bigint_release(x);
        x = y;
        y = r;
    }

    obj_bigint_release(y);
    return x;
}

typedef struct {
    const uint32_t* mod;
    size_t n;
    uint32_t n0inv;
    uint32_t* r2;
} MontgomeryCtx;

static size_t bigint_trim_count_u32(const uint32_t* limbs, size_t count) {
    while (count > 0 && limbs[count - 1] == 0) {
        count--;
    }
    return count;
}

static int limbs_cmp_fixed(const uint32_t* a, const uint32_t* b, size_t n) {
    for (size_t i = n; i > 0; i--) {
        uint32_t av = a[i - 1];
        uint32_t bv = b[i - 1];
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

static void limbs_sub_inplace(uint32_t* a, const uint32_t* b, size_t n) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t av = a[i];
        uint64_t bv = b[i];
        uint64_t sub = bv + borrow;
        borrow = av < sub ? 1 : 0;
        a[i] = (uint32_t)(av - sub);
    }
}

static void bigint_copy_to_fixed_limbs(const ObjBigInt* src, uint32_t* out, size_t n) {
    memset(out, 0, n * sizeof(uint32_t));
    if (!src || src->count == 0) return;
    size_t copy = src->count < n ? src->count : n;
    memcpy(out, src->limbs, copy * sizeof(uint32_t));
}

static uint32_t montgomery_n0inv(uint32_t n0) {
    // Computes -n0^{-1} mod 2^32 (requires n0 odd).
    uint32_t inv = 1;
    for (int i = 0; i < 5; i++) {
        inv *= 2u - n0 * inv;
    }
    return (uint32_t)(0u - inv);
}

static bool bigint_can_use_montgomery(const ObjBigInt* mod) {
    return mod && mod->sign > 0 && mod->count > 0 && ((mod->limbs[0] & 1u) == 1u);
}

static bool mont_ctx_init(MontgomeryCtx* ctx, const ObjBigInt* mod) {
    if (!ctx || !bigint_can_use_montgomery(mod)) return false;

    ctx->mod = mod->limbs;
    ctx->n = mod->count;
    ctx->n0inv = montgomery_n0inv(mod->limbs[0]);
    ctx->r2 = (uint32_t*)safe_calloc(ctx->n, sizeof(uint32_t));

    size_t r2_count = ctx->n * 2 + 1;
    ObjBigInt* r2_big = (ObjBigInt*)safe_malloc(sizeof(ObjBigInt));
    r2_big->ref_count = 1;
    r2_big->sign = 1;
    r2_big->count = r2_count;
    r2_big->limbs = (uint32_t*)safe_calloc(r2_count, sizeof(uint32_t));
    r2_big->limbs[ctx->n * 2] = 1;

    ObjBigInt* r2_mod = obj_bigint_mod(r2_big, mod, NULL);
    obj_bigint_release(r2_big);

    if (r2_mod && r2_mod->count > 0) {
        size_t copy = r2_mod->count < ctx->n ? r2_mod->count : ctx->n;
        memcpy(ctx->r2, r2_mod->limbs, copy * sizeof(uint32_t));
    }
    if (r2_mod) obj_bigint_release(r2_mod);

    return true;
}

static void mont_ctx_free(MontgomeryCtx* ctx) {
    if (!ctx) return;
    if (ctx->r2) free(ctx->r2);
    ctx->r2 = NULL;
    ctx->mod = NULL;
    ctx->n = 0;
    ctx->n0inv = 0;
}

static void mont_mul(uint32_t* out, const uint32_t* a, const uint32_t* b, const MontgomeryCtx* ctx, uint32_t* scratch) {
    size_t n = ctx->n;
    memset(scratch, 0, (n * 2 + 1) * sizeof(uint32_t));

    for (size_t i = 0; i < n; i++) {
        uint64_t carry = 0;
        for (size_t j = 0; j < n; j++) {
            uint64_t cur = (uint64_t)a[i] * (uint64_t)b[j] + (uint64_t)scratch[i + j] + carry;
            scratch[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        size_t k = i + n;
        while (carry != 0 && k < n * 2 + 1) {
            uint64_t cur = (uint64_t)scratch[k] + carry;
            scratch[k] = (uint32_t)cur;
            carry = cur >> 32;
            k++;
        }
    }

    for (size_t i = 0; i < n; i++) {
        uint32_t m = (uint32_t)((uint64_t)scratch[i] * (uint64_t)ctx->n0inv);
        uint64_t carry = 0;
        for (size_t j = 0; j < n; j++) {
            uint64_t cur = (uint64_t)m * (uint64_t)ctx->mod[j] + (uint64_t)scratch[i + j] + carry;
            scratch[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }

        size_t k = i + n;
        while (carry != 0 && k < n * 2 + 1) {
            uint64_t cur = (uint64_t)scratch[k] + carry;
            scratch[k] = (uint32_t)cur;
            carry = cur >> 32;
            k++;
        }
    }

    memcpy(out, scratch + n, n * sizeof(uint32_t));
    if (scratch[n * 2] != 0 || limbs_cmp_fixed(out, ctx->mod, n) >= 0) {
        limbs_sub_inplace(out, ctx->mod, n);
    }
}

static int u32_bit_length(uint32_t x) {
    int bits = 0;
    while (x) {
        bits++;
        x >>= 1;
    }
    return bits;
}

static size_t bigint_ctz_bits_positive(const ObjBigInt* x) {
    if (!x || x->count == 0 || x->sign == 0) return 0;

    size_t bits = 0;
    size_t i = 0;
    while (i < x->count && x->limbs[i] == 0) {
        bits += 32;
        i++;
    }
    if (i == x->count) return bits;

    uint32_t w = x->limbs[i];
    int tz = 0;
    while ((w & 1u) == 0u) {
        tz++;
        w >>= 1;
    }
    return bits + (size_t)tz;
}

static ObjBigInt* bigint_shift_right_bits_positive(const ObjBigInt* x, size_t shift_bits) {
    if (!x || x->count == 0 || x->sign == 0) return obj_bigint_from_int64(0);

    size_t word_shift = shift_bits / 32;
    uint32_t bit_shift = (uint32_t)(shift_bits % 32);
    if (word_shift >= x->count) return obj_bigint_from_int64(0);

    size_t out_count = x->count - word_shift;
    uint32_t* out_limbs = (uint32_t*)safe_malloc(out_count * sizeof(uint32_t));

    for (size_t i = 0; i < out_count; i++) {
        uint32_t low = x->limbs[i + word_shift];
        if (bit_shift == 0) {
            out_limbs[i] = low;
        } else {
            uint32_t high = (i + word_shift + 1 < x->count) ? x->limbs[i + word_shift + 1] : 0;
            out_limbs[i] = (low >> bit_shift) | (high << (32u - bit_shift));
        }
    }

    out_count = bigint_trim_count_u32(out_limbs, out_count);
    if (out_count == 0) {
        free(out_limbs);
        return obj_bigint_from_int64(0);
    }

    out_limbs = (uint32_t*)safe_realloc(out_limbs, out_count * sizeof(uint32_t));
    ObjBigInt* out = (ObjBigInt*)safe_malloc(sizeof(ObjBigInt));
    out->ref_count = 1;
    out->sign = 1;
    out->limbs = out_limbs;
    out->count = out_count;
    return out;
}

static ObjBigInt* bigint_mod_pow_montgomery_common(const ObjBigInt* base, const ObjBigInt* mod,
                                                   const ObjBigInt* exp_bigint, uint64_t exp_u64,
                                                   bool exp_is_bigint) {
    MontgomeryCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (!mont_ctx_init(&ctx, mod)) {
        return NULL;
    }

    size_t n = ctx.n;
    uint32_t* scratch = (uint32_t*)safe_calloc(n * 2 + 1, sizeof(uint32_t));
    uint32_t* one = (uint32_t*)safe_calloc(n, sizeof(uint32_t));
    uint32_t* base_n = (uint32_t*)safe_calloc(n, sizeof(uint32_t));
    uint32_t* base_m = (uint32_t*)safe_calloc(n, sizeof(uint32_t));
    uint32_t* acc = (uint32_t*)safe_calloc(n, sizeof(uint32_t));
    uint32_t* tmp = (uint32_t*)safe_calloc(n, sizeof(uint32_t));

    one[0] = 1;

    ObjBigInt* base_mod = bigint_mod_positive(base, mod);
    bigint_copy_to_fixed_limbs(base_mod, base_n, n);
    obj_bigint_release(base_mod);

    mont_mul(base_m, base_n, ctx.r2, &ctx, scratch);
    mont_mul(acc, one, ctx.r2, &ctx, scratch);

    if (exp_is_bigint) {
        size_t words = exp_bigint ? exp_bigint->count : 0;
        for (size_t wi = 0; wi < words; wi++) {
            uint32_t w = exp_bigint->limbs[wi];
            int bit_limit = 32;
            if (wi == words - 1) {
                bit_limit = u32_bit_length(w);
            }
            for (int bi = 0; bi < bit_limit; bi++) {
                if (w & 1u) {
                    mont_mul(tmp, acc, base_m, &ctx, scratch);
                    memcpy(acc, tmp, n * sizeof(uint32_t));
                }
                w >>= 1u;
                if (wi != words - 1 || bi != bit_limit - 1) {
                    mont_mul(tmp, base_m, base_m, &ctx, scratch);
                    memcpy(base_m, tmp, n * sizeof(uint32_t));
                }
            }
        }
    } else {
        uint64_t e = exp_u64;
        while (e > 0) {
            if (e & 1u) {
                mont_mul(tmp, acc, base_m, &ctx, scratch);
                memcpy(acc, tmp, n * sizeof(uint32_t));
            }
            e >>= 1u;
            if (e > 0) {
                mont_mul(tmp, base_m, base_m, &ctx, scratch);
                memcpy(base_m, tmp, n * sizeof(uint32_t));
            }
        }
    }

    uint32_t* result_limbs = (uint32_t*)safe_malloc(n * sizeof(uint32_t));
    mont_mul(result_limbs, acc, one, &ctx, scratch);

    size_t trimmed = bigint_trim_count_u32(result_limbs, n);
    ObjBigInt* result = NULL;
    if (trimmed == 0) {
        free(result_limbs);
        result = obj_bigint_from_int64(0);
    } else {
        result_limbs = (uint32_t*)safe_realloc(result_limbs, trimmed * sizeof(uint32_t));
        result = (ObjBigInt*)safe_malloc(sizeof(ObjBigInt));
        result->ref_count = 1;
        result->sign = 1;
        result->limbs = result_limbs;
        result->count = trimmed;
    }

    free(scratch);
    free(one);
    free(base_n);
    free(base_m);
    free(acc);
    free(tmp);
    mont_ctx_free(&ctx);
    return result;
}

static ObjBigInt* bigint_mod_pow_int_value(const ObjBigInt* base, uint64_t exp, const ObjBigInt* mod) {
    if (bigint_can_use_montgomery(mod)) {
        ObjBigInt* out = bigint_mod_pow_montgomery_common(base, mod, NULL, exp, false);
        if (out) return out;
    }

    ObjBigInt* acc = obj_bigint_from_int64(1);
    ObjBigInt* base_mod = bigint_mod_positive(base, mod);
    uint64_t e = exp;

    while (e > 0) {
        if (e & 1u) {
            ObjBigInt* tmp = obj_bigint_mul(acc, base_mod);
            obj_bigint_release(acc);
            acc = bigint_mod_positive(tmp, mod);
            obj_bigint_release(tmp);
        }
        e >>= 1u;
        if (e > 0) {
            ObjBigInt* tmp = obj_bigint_mul(base_mod, base_mod);
            obj_bigint_release(base_mod);
            base_mod = bigint_mod_positive(tmp, mod);
            obj_bigint_release(tmp);
        }
    }

    ObjBigInt* reduced = bigint_mod_positive(acc, mod);
    obj_bigint_release(acc);
    obj_bigint_release(base_mod);
    return reduced;
}

static ObjBigInt* bigint_mod_pow_bigint_value(const ObjBigInt* base, const ObjBigInt* exp, const ObjBigInt* mod) {
    if (bigint_can_use_montgomery(mod)) {
        ObjBigInt* out = bigint_mod_pow_montgomery_common(base, mod, exp, 0, true);
        if (out) return out;
    }

    ObjBigInt* acc = obj_bigint_from_int64(1);
    ObjBigInt* base_mod = bigint_mod_positive(base, mod);

    if (!exp || exp->sign == 0) {
        ObjBigInt* reduced = bigint_mod_positive(acc, mod);
        obj_bigint_release(acc);
        obj_bigint_release(base_mod);
        return reduced;
    }

    size_t words = exp->count;
    for (size_t wi = 0; wi < words; wi++) {
        uint32_t w = exp->limbs[wi];
        int bit_limit = 32;
        if (wi == words - 1) {
            bit_limit = u32_bit_length(w);
        }
        for (int bi = 0; bi < bit_limit; bi++) {
            if (w & 1u) {
                ObjBigInt* tmp = obj_bigint_mul(acc, base_mod);
                obj_bigint_release(acc);
                acc = bigint_mod_positive(tmp, mod);
                obj_bigint_release(tmp);
            }
            w >>= 1u;
            if (wi != words - 1 || bi != bit_limit - 1) {
                ObjBigInt* tmp = obj_bigint_mul(base_mod, base_mod);
                obj_bigint_release(base_mod);
                base_mod = bigint_mod_positive(tmp, mod);
                obj_bigint_release(tmp);
            }
        }
    }

    ObjBigInt* reduced = bigint_mod_positive(acc, mod);
    obj_bigint_release(acc);
    obj_bigint_release(base_mod);
    return reduced;
}

static ObjBigInt* bigint_mod_inverse_value(const ObjBigInt* a, const ObjBigInt* mod) {
    ObjBigInt* mod_abs = bigint_abs_clone_value(mod);
    if (mod_abs->sign == 0) {
        obj_bigint_release(mod_abs);
        return obj_bigint_from_int64(0);
    }

    ObjBigInt* a_mod = bigint_mod_positive(a, mod_abs);
    ObjBigInt* t = obj_bigint_from_int64(0);
    ObjBigInt* newt = obj_bigint_from_int64(1);
    ObjBigInt* r = bigint_clone_value(mod_abs);
    ObjBigInt* newr = bigint_clone_value(a_mod);
    ObjBigInt* one = obj_bigint_from_int64(1);

    while (newr->sign != 0) {
        ObjBigInt* q = obj_bigint_div(r, newr, NULL);
        ObjBigInt* q_newt = obj_bigint_mul(q, newt);
        ObjBigInt* t_minus = obj_bigint_sub(t, q_newt);
        obj_bigint_release(q_newt);
        ObjBigInt* q_newr = obj_bigint_mul(q, newr);
        ObjBigInt* r_minus = obj_bigint_sub(r, q_newr);
        obj_bigint_release(q_newr);

        obj_bigint_release(t);
        t = newt;
        newt = t_minus;

        obj_bigint_release(r);
        r = newr;
        newr = r_minus;

        obj_bigint_release(q);
    }

    ObjBigInt* result = NULL;
    if (obj_bigint_compare(r, one) != 0) {
        result = obj_bigint_from_int64(0);
    } else {
        result = bigint_mod_positive(t, mod_abs);
    }

    obj_bigint_release(mod_abs);
    obj_bigint_release(a_mod);
    obj_bigint_release(t);
    obj_bigint_release(newt);
    obj_bigint_release(r);
    obj_bigint_release(newr);
    obj_bigint_release(one);

    return result;
}

static int64_t bigint_is_probable_prime_value(VM* vm, const ObjBigInt* n, int64_t rounds) {
    if (!n || n->sign <= 0) return 0;

    int64_t small_val = 0;
    if (obj_bigint_to_int64(n, &small_val)) {
        if (small_val < 2) return 0;
        if (small_val % 2 == 0) return small_val == 2;
        for (int64_t p = 3; p * p <= small_val; p += 2) {
            if (small_val % p == 0) return 0;
        }
        return 1;
    }

    if (rounds <= 0) return 0;
    if (obj_bigint_is_even(n)) return 0;

    static const int small_primes[] = {3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    for (size_t i = 0; i < sizeof(small_primes) / sizeof(small_primes[0]); i++) {
        ObjBigInt* p = obj_bigint_from_int64(small_primes[i]);
        int cmp = obj_bigint_compare(n, p);
        if (cmp == 0) {
            obj_bigint_release(p);
            return 1;
        }
        ObjBigInt* rem = obj_bigint_mod(n, p, NULL);
        obj_bigint_release(p);
        if (rem && rem->sign == 0) {
            obj_bigint_release(rem);
            return 0;
        }
        if (rem) obj_bigint_release(rem);
    }

    ObjBigInt* one = obj_bigint_from_int64(1);
    ObjBigInt* two = obj_bigint_from_int64(2);
    ObjBigInt* n_minus_one = obj_bigint_sub(n, one);
    ObjBigInt* n_minus_two = obj_bigint_sub(n, two);

    size_t s_bits = bigint_ctz_bits_positive(n_minus_one);
    if (s_bits == 0) {
        obj_bigint_release(one);
        obj_bigint_release(two);
        obj_bigint_release(n_minus_one);
        obj_bigint_release(n_minus_two);
        return 0;
    }
    ObjBigInt* d = bigint_shift_right_bits_positive(n_minus_one, s_bits);
    int64_t s = (int64_t)s_bits;

    MontgomeryCtx mctx;
    memset(&mctx, 0, sizeof(mctx));
    if (!mont_ctx_init(&mctx, n)) {
        obj_bigint_release(one);
        obj_bigint_release(two);
        obj_bigint_release(n_minus_one);
        obj_bigint_release(n_minus_two);
        obj_bigint_release(d);
        return 0;
    }

    size_t n_limbs = mctx.n;
    uint32_t* scratch = (uint32_t*)safe_calloc(n_limbs * 2 + 1, sizeof(uint32_t));
    uint32_t* one_fixed = (uint32_t*)safe_calloc(n_limbs, sizeof(uint32_t));
    uint32_t* one_mont = (uint32_t*)safe_calloc(n_limbs, sizeof(uint32_t));
    uint32_t* n_minus_one_fixed = (uint32_t*)safe_calloc(n_limbs, sizeof(uint32_t));
    uint32_t* n_minus_one_mont = (uint32_t*)safe_calloc(n_limbs, sizeof(uint32_t));
    uint32_t* a_fixed = (uint32_t*)safe_calloc(n_limbs, sizeof(uint32_t));
    uint32_t* base_mont = (uint32_t*)safe_calloc(n_limbs, sizeof(uint32_t));
    uint32_t* x_mont = (uint32_t*)safe_calloc(n_limbs, sizeof(uint32_t));
    uint32_t* tmp = (uint32_t*)safe_calloc(n_limbs, sizeof(uint32_t));

    one_fixed[0] = 1;
    mont_mul(one_mont, one_fixed, mctx.r2, &mctx, scratch);

    bigint_copy_to_fixed_limbs(n_minus_one, n_minus_one_fixed, n_limbs);
    mont_mul(n_minus_one_mont, n_minus_one_fixed, mctx.r2, &mctx, scratch);

    static const int bases[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    int64_t total_rounds = rounds;
    for (int64_t i = 0; i < total_rounds; i++) {
        ObjBigInt* a = NULL;
        if (i < (int64_t)(sizeof(bases) / sizeof(bases[0]))) {
            a = obj_bigint_from_int64(bases[i]);
            if (obj_bigint_compare(a, n_minus_two) > 0) {
                obj_bigint_release(a);
                a = obj_bigint_from_int64(2);
            }
        } else {
            bool ok = true;
            a = random_bigint_range_secure(two, n_minus_two, &ok);
            if (!ok || !a) {
                if (a) obj_bigint_release(a);
                a = random_bigint_range_sfc(vm, two, n_minus_two);
            }
        }

        if (!a) {
            free(scratch);
            free(one_fixed);
            free(one_mont);
            free(n_minus_one_fixed);
            free(n_minus_one_mont);
            free(a_fixed);
            free(base_mont);
            free(x_mont);
            free(tmp);
            mont_ctx_free(&mctx);
            obj_bigint_release(one);
            obj_bigint_release(two);
            obj_bigint_release(n_minus_one);
            obj_bigint_release(n_minus_two);
            obj_bigint_release(d);
            return 0;
        }

        bigint_copy_to_fixed_limbs(a, a_fixed, n_limbs);
        mont_mul(base_mont, a_fixed, mctx.r2, &mctx, scratch);

        memcpy(x_mont, one_mont, n_limbs * sizeof(uint32_t));

        size_t words = d->count;
        for (size_t wi = 0; wi < words; wi++) {
            uint32_t w = d->limbs[wi];
            int bit_limit = 32;
            if (wi == words - 1) {
                bit_limit = u32_bit_length(w);
            }
            for (int bi = 0; bi < bit_limit; bi++) {
                if (w & 1u) {
                    mont_mul(tmp, x_mont, base_mont, &mctx, scratch);
                    memcpy(x_mont, tmp, n_limbs * sizeof(uint32_t));
                }
                w >>= 1u;
                if (wi != words - 1 || bi != bit_limit - 1) {
                    mont_mul(tmp, base_mont, base_mont, &mctx, scratch);
                    memcpy(base_mont, tmp, n_limbs * sizeof(uint32_t));
                }
            }
        }

        bool passed = false;
        if (memcmp(x_mont, one_mont, n_limbs * sizeof(uint32_t)) == 0 ||
            memcmp(x_mont, n_minus_one_mont, n_limbs * sizeof(uint32_t)) == 0) {
            passed = true;
        } else {
            for (int64_t r = 1; r < s; r++) {
                mont_mul(tmp, x_mont, x_mont, &mctx, scratch);
                memcpy(x_mont, tmp, n_limbs * sizeof(uint32_t));
                if (memcmp(x_mont, n_minus_one_mont, n_limbs * sizeof(uint32_t)) == 0) {
                    passed = true;
                    break;
                }
            }
        }

        obj_bigint_release(a);

        if (!passed) {
            free(scratch);
            free(one_fixed);
            free(one_mont);
            free(n_minus_one_fixed);
            free(n_minus_one_mont);
            free(a_fixed);
            free(base_mont);
            free(x_mont);
            free(tmp);
            mont_ctx_free(&mctx);
            obj_bigint_release(one);
            obj_bigint_release(two);
            obj_bigint_release(n_minus_one);
            obj_bigint_release(n_minus_two);
            obj_bigint_release(d);
            return 0;
        }
    }

    free(scratch);
    free(one_fixed);
    free(one_mont);
    free(n_minus_one_fixed);
    free(n_minus_one_mont);
    free(a_fixed);
    free(base_mont);
    free(x_mont);
    free(tmp);
    mont_ctx_free(&mctx);
    obj_bigint_release(one);
    obj_bigint_release(two);
    obj_bigint_release(n_minus_one);
    obj_bigint_release(n_minus_two);
    obj_bigint_release(d);
    return 1;
}

void builtin_abs_bigint(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;

    ObjBigInt* val_bigint = value_get_bigint_obj(&val);
    if (value_get_type(&val) == VAL_BIGINT && val_bigint) {
        if (val_bigint->sign >= 0) {
            return;
        }
        ObjBigInt* abs_val = obj_bigint_negate(val_bigint);
        Value result;
        value_init_bigint(&result, abs_val);
        value_free(slot);
        *slot = result;
        return;
    }

    ObjBigInt* zero = obj_bigint_from_int64(0);
    Value result;
    value_init_bigint(&result, zero);
    value_free(slot);
    *slot = result;
}

void builtin_sign_bigint(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    int64_t sign_val = 0;

    ObjBigInt* val_bigint = value_get_bigint_obj(&val);
    if (value_get_type(&val) == VAL_BIGINT && val_bigint) {
        if (val_bigint->sign > 0) {
            sign_val = 1;
        } else if (val_bigint->sign < 0) {
            sign_val = -1;
        }
    }

    Value result;
    value_init_int(&result, sign_val);
    value_free(slot);
    *slot = result;
}

void builtin_digits_bigint(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    int64_t digits = 0;

    ObjBigInt* val_bigint = value_get_bigint_obj(&val);
    if (value_get_type(&val) == VAL_BIGINT && val_bigint) {
        size_t len = obj_bigint_decimal_digits(val_bigint);
        digits = (len > (size_t)INT64_MAX) ? INT64_MAX : (int64_t)len;
    }

    Value result;
    value_init_int(&result, digits);
    value_free(slot);
    *slot = result;
}

void builtin_is_even_bigint(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    bool is_even = false;

    ObjBigInt* val_bigint = value_get_bigint_obj(&val);
    if (value_get_type(&val) == VAL_BIGINT && val_bigint) {
        is_even = obj_bigint_is_even(val_bigint);
    }

    Value result;
    value_init_bool(&result, is_even);
    value_free(slot);
    *slot = result;
}

void builtin_is_odd_bigint(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    bool is_odd = false;

    ObjBigInt* val_bigint = value_get_bigint_obj(&val);
    if (value_get_type(&val) == VAL_BIGINT && val_bigint) {
        is_odd = !obj_bigint_is_even(val_bigint);
    }

    Value result;
    value_init_bool(&result, is_odd);
    value_free(slot);
    *slot = result;
}

void builtin_pow_bigint(VM* vm) {
    // arg0 (base) at count-2, arg1 (exp) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value base_val = *slot;
    Value exp_val = vm->stack.values[vm->stack.count - 1];
    Value result;

    ObjBigInt* base_bigint = value_get_bigint_obj(&base_val);
    if (value_get_type(&base_val) != VAL_BIGINT || !base_bigint || value_get_type(&exp_val) != VAL_INT) {
        ObjBigInt* zero = obj_bigint_from_int64(0);
        value_init_bigint(&result, zero);
        value_free(slot);
        *slot = result;
        return;
    }

    int64_t exp = value_get_int(&exp_val);
    if (exp < 0) {
        ObjBigInt* zero = obj_bigint_from_int64(0);
        value_init_bigint(&result, zero);
        value_free(slot);
        *slot = result;
        return;
    }

    ObjBigInt* base = bigint_clone_value(base_bigint);
    ObjBigInt* acc = obj_bigint_from_int64(1);
    uint64_t e = (uint64_t)exp;

    while (e > 0) {
        if (e & 1u) {
            ObjBigInt* tmp = obj_bigint_mul(acc, base);
            obj_bigint_release(acc);
            acc = tmp;
        }
        e >>= 1u;
        if (e > 0) {
            ObjBigInt* tmp = obj_bigint_mul(base, base);
            obj_bigint_release(base);
            base = tmp;
        }
    }

    obj_bigint_release(base);
    value_init_bigint(&result, acc);
    value_free(slot);
    *slot = result;
}

void builtin_gcd_bigint(VM* vm) {
    // arg0 (a) at count-2, arg1 (b) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value a_val = *slot;
    Value b_val = vm->stack.values[vm->stack.count - 1];
    Value result;

    ObjBigInt* a_bigint = value_get_bigint_obj(&a_val);
    ObjBigInt* b_bigint = value_get_bigint_obj(&b_val);
    if (value_get_type(&a_val) != VAL_BIGINT || value_get_type(&b_val) != VAL_BIGINT ||
        !a_bigint || !b_bigint) {
        ObjBigInt* zero = obj_bigint_from_int64(0);
        value_init_bigint(&result, zero);
        value_free(slot);
        *slot = result;
        return;
    }

    ObjBigInt* gcd = bigint_gcd_value(a_bigint, b_bigint);
    value_init_bigint(&result, gcd);
    value_free(slot);
    *slot = result;
}

void builtin_lcm_bigint(VM* vm) {
    // arg0 (a) at count-2, arg1 (b) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value a_val = *slot;
    Value b_val = vm->stack.values[vm->stack.count - 1];
    Value result;

    ObjBigInt* a_bigint = value_get_bigint_obj(&a_val);
    ObjBigInt* b_bigint = value_get_bigint_obj(&b_val);
    if (value_get_type(&a_val) != VAL_BIGINT || value_get_type(&b_val) != VAL_BIGINT ||
        !a_bigint || !b_bigint) {
        ObjBigInt* zero = obj_bigint_from_int64(0);
        value_init_bigint(&result, zero);
        value_free(slot);
        *slot = result;
        return;
    }

    if (a_bigint->sign == 0 || b_bigint->sign == 0) {
        ObjBigInt* zero = obj_bigint_from_int64(0);
        value_init_bigint(&result, zero);
        value_free(slot);
        *slot = result;
        return;
    }

    ObjBigInt* gcd = bigint_gcd_value(a_bigint, b_bigint);
    ObjBigInt* a_abs = bigint_abs_clone_value(a_bigint);
    ObjBigInt* b_abs = bigint_abs_clone_value(b_bigint);
    ObjBigInt* div = obj_bigint_div(a_abs, gcd, NULL);
    ObjBigInt* lcm = obj_bigint_mul(div, b_abs);

    obj_bigint_release(gcd);
    obj_bigint_release(a_abs);
    obj_bigint_release(b_abs);
    obj_bigint_release(div);

    value_init_bigint(&result, lcm);
    value_free(slot);
    *slot = result;
}

void builtin_mod_pow_bigint(VM* vm) {
    // arg0 (base) at count-3, arg1 (exp) at count-2, arg2 (mod) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value base_val = *slot;
    Value exp_val = vm->stack.values[vm->stack.count - 2];
    Value mod_val = vm->stack.values[vm->stack.count - 1];
    Value result;

    ObjBigInt* base_bigint = value_get_bigint_obj(&base_val);
    ObjBigInt* mod_bigint = value_get_bigint_obj(&mod_val);
    if (value_get_type(&base_val) != VAL_BIGINT || !base_bigint ||
        value_get_type(&mod_val) != VAL_BIGINT || !mod_bigint) {
        ObjBigInt* zero = obj_bigint_from_int64(0);
        value_init_bigint(&result, zero);
        value_free(slot);
        *slot = result;
        return;
    }

    ObjBigInt* mod_abs = bigint_abs_clone_value(mod_bigint);
    if (mod_abs->sign == 0) {
        obj_bigint_release(mod_abs);
        ObjBigInt* zero = obj_bigint_from_int64(0);
        value_init_bigint(&result, zero);
        value_free(slot);
        *slot = result;
        return;
    }

    ObjBigInt* acc = NULL;
    if (value_get_type(&exp_val) == VAL_INT) {
        int64_t exp = value_get_int(&exp_val);
        if (exp < 0) {
            acc = obj_bigint_from_int64(0);
        } else {
            acc = bigint_mod_pow_int_value(base_bigint, (uint64_t)exp, mod_abs);
        }
    } else if (value_get_type(&exp_val) == VAL_BIGINT && value_get_bigint_obj(&exp_val)) {
        ObjBigInt* exp_bigint = value_get_bigint_obj(&exp_val);
        if (exp_bigint->sign < 0) {
            acc = obj_bigint_from_int64(0);
        } else {
            acc = bigint_mod_pow_bigint_value(base_bigint, exp_bigint, mod_abs);
        }
    } else {
        acc = obj_bigint_from_int64(0);
    }

    obj_bigint_release(mod_abs);

    value_init_bigint(&result, acc);
    value_free(slot);
    *slot = result;
}

void builtin_mod_inverse_bigint(VM* vm) {
    // arg0 (a) at count-2, arg1 (mod) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value a_val = *slot;
    Value mod_val = vm->stack.values[vm->stack.count - 1];
    Value result;

    ObjBigInt* a_bigint = value_get_bigint_obj(&a_val);
    ObjBigInt* mod_bigint = value_get_bigint_obj(&mod_val);
    if (value_get_type(&a_val) != VAL_BIGINT || value_get_type(&mod_val) != VAL_BIGINT ||
        !a_bigint || !mod_bigint) {
        ObjBigInt* zero = obj_bigint_from_int64(0);
        value_init_bigint(&result, zero);
        value_free(slot);
        *slot = result;
        return;
    }

    ObjBigInt* inv = bigint_mod_inverse_value(a_bigint, mod_bigint);
    value_init_bigint(&result, inv);
    value_free(slot);
    *slot = result;
}

void builtin_is_probable_prime_bigint(VM* vm) {
    // arg0 (n) at count-2, arg1 (rounds) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value n_val = *slot;
    Value rounds_val = vm->stack.values[vm->stack.count - 1];
    bool is_prime = false;

    ObjBigInt* n_bigint = value_get_bigint_obj(&n_val);
    if (value_get_type(&n_val) == VAL_BIGINT &&
        n_bigint &&
        value_get_type(&rounds_val) == VAL_INT) {
        is_prime = bigint_is_probable_prime_value(vm, n_bigint, value_get_int(&rounds_val)) != 0;
    }

    Value result;
    value_init_bool(&result, is_prime);
    value_free(slot);
    *slot = result;
}

void builtin_compare_bigint(VM* vm) {
    // arg0 (a) at count-2, arg1 (b) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value a_val = *slot;
    Value b_val = vm->stack.values[vm->stack.count - 1];
    int64_t cmp_val = 0;

    ObjBigInt* a_bigint = value_get_bigint_obj(&a_val);
    ObjBigInt* b_bigint = value_get_bigint_obj(&b_val);
    if (value_get_type(&a_val) == VAL_BIGINT &&
        value_get_type(&b_val) == VAL_BIGINT &&
        a_bigint &&
        b_bigint) {
        int cmp = obj_bigint_compare(a_bigint, b_bigint);
        cmp_val = (cmp < 0) ? -1 : (cmp > 0 ? 1 : 0);
    }

    Value result;
    value_init_int(&result, cmp_val);
    value_free(slot);
    *slot = result;
}

void builtin_abs_cmp_bigint(VM* vm) {
    // arg0 (a) at count-2, arg1 (b) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value a_val = *slot;
    Value b_val = vm->stack.values[vm->stack.count - 1];
    int64_t cmp_val = 0;

    ObjBigInt* a_bigint = value_get_bigint_obj(&a_val);
    ObjBigInt* b_bigint = value_get_bigint_obj(&b_val);
    if (value_get_type(&a_val) == VAL_BIGINT &&
        value_get_type(&b_val) == VAL_BIGINT &&
        a_bigint &&
        b_bigint) {
        int cmp = bigint_compare_abs_value(a_bigint, b_bigint);
        cmp_val = (cmp < 0) ? -1 : (cmp > 0 ? 1 : 0);
    }

    Value result;
    value_init_int(&result, cmp_val);
    value_free(slot);
    *slot = result;
}

void builtin_clamp_bigint(VM* vm) {
    // arg0 (value) at count-3, arg1 (min) at count-2, arg2 (max) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value value_val = *slot;
    Value min_val = vm->stack.values[vm->stack.count - 2];
    Value max_val = vm->stack.values[vm->stack.count - 1];
    Value result;

    ObjBigInt* value_bigint = value_get_bigint_obj(&value_val);
    ObjBigInt* min_bigint = value_get_bigint_obj(&min_val);
    ObjBigInt* max_bigint = value_get_bigint_obj(&max_val);
    if (value_get_type(&value_val) != VAL_BIGINT ||
        value_get_type(&min_val) != VAL_BIGINT ||
        value_get_type(&max_val) != VAL_BIGINT ||
        !value_bigint ||
        !min_bigint ||
        !max_bigint) {
        ObjBigInt* zero = obj_bigint_from_int64(0);
        value_init_bigint(&result, zero);
        value_free(slot);
        *slot = result;
        return;
    }

    const ObjBigInt* lo = min_bigint;
    const ObjBigInt* hi = max_bigint;
    if (obj_bigint_compare(lo, hi) > 0) {
        const ObjBigInt* tmp = lo;
        lo = hi;
        hi = tmp;
    }

    if (obj_bigint_compare(value_bigint, lo) < 0) {
        ObjBigInt* copy = bigint_clone_value(lo);
        value_init_bigint(&result, copy);
    } else if (obj_bigint_compare(value_bigint, hi) > 0) {
        ObjBigInt* copy = bigint_clone_value(hi);
        value_init_bigint(&result, copy);
    } else {
        ObjBigInt* copy = bigint_clone_value(value_bigint);
        value_init_bigint(&result, copy);
    }

    value_free(slot);
    *slot = result;
}

void builtin_is_zero_bigint(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    bool is_zero = false;

    ObjBigInt* val_bigint = value_get_bigint_obj(&val);
    if (value_get_type(&val) == VAL_BIGINT && val_bigint) {
        is_zero = (val_bigint->sign == 0);
    }

    Value result;
    value_init_bool(&result, is_zero);
    value_free(slot);
    *slot = result;
}

void builtin_is_negative_bigint(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    bool is_negative = false;

    ObjBigInt* val_bigint = value_get_bigint_obj(&val);
    if (value_get_type(&val) == VAL_BIGINT && val_bigint) {
        is_negative = (val_bigint->sign < 0);
    }

    Value result;
    value_init_bool(&result, is_negative);
    value_free(slot);
    *slot = result;
}

// ============ Math Functions ============

void builtin_abs_int(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    
    Value result;
    if (value_get_type(&val) == VAL_INT) {
        int64_t int_val = value_get_int(&val);
        value_init_int(&result, int_val < 0 ? -int_val : int_val);
    } else {
        value_init_int(&result, 0);
    }
    
    value_free(slot);
    *slot = result;
}

void builtin_abs_double(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    
    Value result;
    if (value_get_type(&val) == VAL_DOUBLE) {
        value_init_double(&result, fabs(value_get_double(&val)));
    } else if (value_get_type(&val) == VAL_INT) {
        value_init_double(&result, fabs((double)value_get_int(&val)));
    } else {
        value_init_double(&result, 0.0);
    }
    
    value_free(slot);
    *slot = result;
}

void builtin_min(VM* vm) {
    // arg0 (a) at count-2, arg1 (b) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value a = *slot;
    Value b = vm->stack.values[vm->stack.count - 1];
    
    Value result;
    ValueType a_type = value_get_type(&a);
    ValueType b_type = value_get_type(&b);
    if (a_type == VAL_INT && b_type == VAL_INT) {
        int64_t ai = value_get_int(&a);
        int64_t bi = value_get_int(&b);
        value_init_int(&result, ai < bi ? ai : bi);
    } else if ((a_type == VAL_INT || a_type == VAL_DOUBLE) &&
               (b_type == VAL_INT || b_type == VAL_DOUBLE)) {
        double da = (a_type == VAL_INT) ? (double)value_get_int(&a) : value_get_double(&a);
        double db = (b_type == VAL_INT) ? (double)value_get_int(&b) : value_get_double(&b);
        value_init_double(&result, da < db ? da : db);
    } else {
        value_init_nil(&result);
    }
    
    value_free(slot);
    *slot = result;
}

void builtin_max(VM* vm) {
    // arg0 (a) at count-2, arg1 (b) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value a = *slot;
    Value b = vm->stack.values[vm->stack.count - 1];
    
    Value result;
    ValueType a_type = value_get_type(&a);
    ValueType b_type = value_get_type(&b);
    if (a_type == VAL_INT && b_type == VAL_INT) {
        int64_t ai = value_get_int(&a);
        int64_t bi = value_get_int(&b);
        value_init_int(&result, ai > bi ? ai : bi);
    } else if ((a_type == VAL_INT || a_type == VAL_DOUBLE) &&
               (b_type == VAL_INT || b_type == VAL_DOUBLE)) {
        double da = (a_type == VAL_INT) ? (double)value_get_int(&a) : value_get_double(&a);
        double db = (b_type == VAL_INT) ? (double)value_get_int(&b) : value_get_double(&b);
        value_init_double(&result, da > db ? da : db);
    } else {
        value_init_nil(&result);
    }
    
    value_free(slot);
    *slot = result;
}

void builtin_floor(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    
    Value result;
    if (value_get_type(&val) == VAL_DOUBLE) {
        value_init_double(&result, floor(value_get_double(&val)));
    } else if (value_get_type(&val) == VAL_INT) {
        value_init_double(&result, (double)value_get_int(&val));
    } else {
        value_init_double(&result, 0.0);
    }
    
    value_free(slot);
    *slot = result;
}

void builtin_ceil(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    
    Value result;
    if (value_get_type(&val) == VAL_DOUBLE) {
        value_init_double(&result, ceil(value_get_double(&val)));
    } else if (value_get_type(&val) == VAL_INT) {
        value_init_double(&result, (double)value_get_int(&val));
    } else {
        value_init_double(&result, 0.0);
    }
    
    value_free(slot);
    *slot = result;
}

void builtin_round(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    
    Value result;
    if (value_get_type(&val) == VAL_DOUBLE) {
        value_init_double(&result, round(value_get_double(&val)));
    } else if (value_get_type(&val) == VAL_INT) {
        value_init_double(&result, (double)value_get_int(&val));
    } else {
        value_init_double(&result, 0.0);
    }
    
    value_free(slot);
    *slot = result;
}

void builtin_sqrt(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value val = *slot;
    
    Value result;
    if (value_get_type(&val) == VAL_DOUBLE) {
        value_init_double(&result, sqrt(value_get_double(&val)));
    } else if (value_get_type(&val) == VAL_INT) {
        value_init_double(&result, sqrt((double)value_get_int(&val)));
    } else {
        value_init_double(&result, 0.0);
    }
    
    value_free(slot);
    *slot = result;
}

void builtin_pow(VM* vm) {
    // arg0 (base) at count-2, arg1 (exp) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value base = *slot;
    Value exp = vm->stack.values[vm->stack.count - 1];
    
    Value result;
    ValueType base_type = value_get_type(&base);
    ValueType exp_type = value_get_type(&exp);
    double b = (base_type == VAL_INT) ? (double)value_get_int(&base) :
               (base_type == VAL_DOUBLE) ? value_get_double(&base) : 0.0;
    double e = (exp_type == VAL_INT) ? (double)value_get_int(&exp) :
               (exp_type == VAL_DOUBLE) ? value_get_double(&exp) : 0.0;
    
    value_init_double(&result, pow(b, e));
    
    value_free(slot);
    *slot = result;
}

// ============ Random Functions ============

static bool os_random_bytes(uint8_t* out, size_t len) {
#ifdef _WIN32
    if (!out || len == 0) return true;
    return BCryptGenRandom(NULL, out, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    if (!out || len == 0) return true;
    size_t offset = 0;
#if defined(__linux__)
    while (offset < len) {
        ssize_t got = getrandom(out + offset, len - offset, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        offset += (size_t)got;
    }
    return true;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    while (offset < len) {
        ssize_t got = read(fd, out + offset, len - offset);
        if (got < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (got == 0) {
            close(fd);
            return false;
        }
        offset += (size_t)got;
    }
    close(fd);
    return true;
#endif
#endif
}

static uint64_t rng_fallback_seed(VM* vm) {
    uint64_t seed = (uint64_t)time(NULL);
    seed ^= (uint64_t)clock() << 32;
    seed ^= (uint64_t)(uintptr_t)vm;
    seed ^= (uint64_t)(++vm->rng_counter * 0x9E3779B97F4A7C15ULL);
    return seed;
}

static void rng_seed(VM* vm, uint64_t seed) {
    sfc64_init(&vm->rng, seed);
    vm->rng_seeded = true;
}

static void rng_seed_if_needed(VM* vm) {
    if (vm->rng_seeded) return;
    uint64_t seed = 0;
    if (!os_random_bytes((uint8_t*)&seed, sizeof(seed))) {
        seed = rng_fallback_seed(vm);
    }
    rng_seed(vm, seed);
}

static uint64_t rng_next_u64(VM* vm) {
    rng_seed_if_needed(vm);
    return sfc64_next(&vm->rng);
}

static double rng_next_double(VM* vm) {
    rng_seed_if_needed(vm);
    return sfc64_double(&vm->rng);
}

static void rng_fill_bytes(VM* vm, uint8_t* out, size_t len) {
    rng_seed_if_needed(vm);
    size_t offset = 0;
    while (offset < len) {
        uint64_t r = sfc64_next(&vm->rng);
        size_t chunk = len - offset;
        if (chunk > sizeof(uint64_t)) chunk = sizeof(uint64_t);
        for (size_t i = 0; i < chunk; i++) {
            out[offset + i] = (uint8_t)(r >> (8u * (7u - (uint32_t)i)));
        }
        offset += chunk;
    }
}

static bool secure_next_u64(uint64_t* out) {
    return os_random_bytes((uint8_t*)out, sizeof(uint64_t));
}

static bool secure_fill_bytes(uint8_t* out, size_t len) {
    return os_random_bytes(out, len);
}

static double double_from_u64(uint64_t value) {
    return (value >> 11) * (1.0 / 9007199254740992.0);
}

static uint64_t bigint_seed_from_value(Value seed_val, bool* ok) {
    *ok = true;
    if (value_get_type(&seed_val) == VAL_INT) {
        return (uint64_t)value_get_int(&seed_val);
    }
    ObjBigInt* seed_bigint = value_get_bigint_obj(&seed_val);
    if (value_get_type(&seed_val) == VAL_BIGINT && seed_bigint) {
        ObjBigInt* bi = seed_bigint;
        uint64_t acc = 0;
        for (size_t i = bi->count; i > 0; i--) {
            acc = (acc << 32u) | (uint64_t)bi->limbs[i - 1];
        }
        if (bi->sign < 0) {
            acc = (~acc) + 1;
        }
        return acc;
    }
    *ok = false;
    return 0;
}

static size_t bigint_bit_length_upper(const ObjBigInt* bigint) {
    if (!bigint || bigint->count == 0) return 0;
    uint32_t top = bigint->limbs[bigint->count - 1];
    size_t bits = 0;
    while (top) {
        bits++;
        top >>= 1;
    }
    return (bigint->count - 1) * 32 + bits;
}

static ObjBigInt* bigint_from_bytes_be(const uint8_t* bytes, size_t len) {
    if (!bytes || len == 0) {
        return obj_bigint_from_int64(0);
    }

    size_t limb_count = (len + 3) / 4;
    uint32_t* limbs = (uint32_t*)safe_malloc(limb_count * sizeof(uint32_t));

    size_t byte_index = len;
    for (size_t i = 0; i < limb_count; i++) {
        uint32_t limb = 0;
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            if (byte_index == 0) break;
            byte_index--;
            limb |= (uint32_t)bytes[byte_index] << shift;
        }
        limbs[i] = limb;
    }

    size_t trimmed = bigint_trim_count_u32(limbs, limb_count);
    if (trimmed == 0) {
        free(limbs);
        return obj_bigint_from_int64(0);
    }

    limbs = (uint32_t*)safe_realloc(limbs, trimmed * sizeof(uint32_t));
    ObjBigInt* result = (ObjBigInt*)safe_malloc(sizeof(ObjBigInt));
    result->ref_count = 1;
    result->sign = 1;
    result->limbs = limbs;
    result->count = trimmed;
    return result;
}

static ObjBigInt* random_bigint_bits_sfc(VM* vm, int64_t bits) {
    if (bits <= 0) {
        return obj_bigint_from_int64(0);
    }
    size_t byte_len = (size_t)((bits + 7) / 8);
    uint8_t* bytes = (uint8_t*)safe_malloc(byte_len);
    rng_fill_bytes(vm, bytes, byte_len);
    int extra = (int)(byte_len * 8 - (size_t)bits);
    if (extra > 0) {
        bytes[0] &= (uint8_t)(0xFFu >> extra);
    }
    ObjBigInt* result = bigint_from_bytes_be(bytes, byte_len);
    free(bytes);
    return result;
}

static ObjBigInt* random_bigint_bits_secure(int64_t bits, bool* ok) {
    *ok = true;
    if (bits <= 0) {
        return obj_bigint_from_int64(0);
    }
    size_t byte_len = (size_t)((bits + 7) / 8);
    uint8_t* bytes = (uint8_t*)safe_malloc(byte_len);
    if (!secure_fill_bytes(bytes, byte_len)) {
        free(bytes);
        *ok = false;
        return obj_bigint_from_int64(0);
    }
    int extra = (int)(byte_len * 8 - (size_t)bits);
    if (extra > 0) {
        bytes[0] &= (uint8_t)(0xFFu >> extra);
    }
    ObjBigInt* result = bigint_from_bytes_be(bytes, byte_len);
    free(bytes);
    return result;
}

static ObjBigInt* random_bigint_range_sfc(VM* vm, ObjBigInt* min, ObjBigInt* max) {
    if (!min || !max) {
        return obj_bigint_from_int64(0);
    }
    if (obj_bigint_compare(min, max) > 0) {
        ObjBigInt* tmp = min;
        min = max;
        max = tmp;
    }
    if (obj_bigint_compare(min, max) == 0) {
        return bigint_clone_value(min);
    }
    ObjBigInt* range = obj_bigint_sub(max, min);
    ObjBigInt* one = obj_bigint_from_int64(1);
    ObjBigInt* span = obj_bigint_add(range, one);
    obj_bigint_release(range);
    obj_bigint_release(one);

    size_t bits = bigint_bit_length_upper(span);
    ObjBigInt* candidate = NULL;
    do {
        if (candidate) obj_bigint_release(candidate);
        candidate = random_bigint_bits_sfc(vm, (int64_t)bits);
    } while (obj_bigint_compare(candidate, span) >= 0);

    ObjBigInt* result = obj_bigint_add(min, candidate);
    obj_bigint_release(candidate);
    obj_bigint_release(span);
    return result;
}

static ObjBigInt* random_bigint_range_secure(ObjBigInt* min, ObjBigInt* max, bool* ok) {
    *ok = true;
    if (!min || !max) {
        return obj_bigint_from_int64(0);
    }
    if (obj_bigint_compare(min, max) > 0) {
        ObjBigInt* tmp = min;
        min = max;
        max = tmp;
    }
    if (obj_bigint_compare(min, max) == 0) {
        return bigint_clone_value(min);
    }
    ObjBigInt* range = obj_bigint_sub(max, min);
    ObjBigInt* one = obj_bigint_from_int64(1);
    ObjBigInt* span = obj_bigint_add(range, one);
    obj_bigint_release(range);
    obj_bigint_release(one);

    size_t bits = bigint_bit_length_upper(span);
    ObjBigInt* candidate = NULL;
    do {
        if (candidate) obj_bigint_release(candidate);
        candidate = random_bigint_bits_secure((int64_t)bits, ok);
        if (!*ok) {
            obj_bigint_release(candidate);
            obj_bigint_release(span);
            return obj_bigint_from_int64(0);
        }
    } while (obj_bigint_compare(candidate, span) >= 0);

    ObjBigInt* result = obj_bigint_add(min, candidate);
    obj_bigint_release(candidate);
    obj_bigint_release(span);
    return result;
}

static uint64_t random_u64_range_sfc(VM* vm, uint64_t span) {
    if (span == 0) {
        return rng_next_u64(vm);
    }
    uint64_t limit = UINT64_MAX - (UINT64_MAX % span);
    uint64_t r = rng_next_u64(vm);
    while (r >= limit) {
        r = rng_next_u64(vm);
    }
    return r % span;
}

static bool random_u64_range_secure(uint64_t span, uint64_t* out) {
    if (span == 0) {
        return secure_next_u64(out);
    }
    uint64_t limit = UINT64_MAX - (UINT64_MAX % span);
    uint64_t r = 0;
    do {
        if (!secure_next_u64(&r)) return false;
    } while (r >= limit);
    *out = r % span;
    return true;
}

static int64_t random_int_range_sfc(VM* vm, int64_t min, int64_t max) {
    if (max < min) {
        int64_t temp = min;
        min = max;
        max = temp;
    }
    uint64_t span = (uint64_t)max - (uint64_t)min + 1u;
    if (span == 0) {
        return (int64_t)rng_next_u64(vm);
    }
    return (int64_t)random_u64_range_sfc(vm, span) + min;
}

static bool random_int_range_secure(int64_t min, int64_t max, int64_t* out) {
    if (max < min) {
        int64_t temp = min;
        min = max;
        max = temp;
    }
    uint64_t span = (uint64_t)max - (uint64_t)min + 1u;
    if (span == 0) {
        uint64_t r = 0;
        if (!secure_next_u64(&r)) return false;
        *out = (int64_t)r;
        return true;
    }
    uint64_t r = 0;
    if (!random_u64_range_secure(span, &r)) return false;
    *out = (int64_t)r + min;
    return true;
}

void builtin_random(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    
    Value result;
    value_init_double(&result, rng_next_double(vm));
    
    value_free(slot);
    *slot = result;
}

void builtin_random_seed(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value seed_val = *slot;

    bool ok = false;
    uint64_t seed = bigint_seed_from_value(seed_val, &ok);
    if (ok) {
        rng_seed(vm, seed);
    }

    Value result;
    value_init_nil(&result);
    value_free(slot);
    *slot = result;
}

void builtin_random_int(VM* vm) {
    // arg0 (min) at count-2, arg1 (max) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value min_val = *slot;
    Value max_val = vm->stack.values[vm->stack.count - 1];
    
    Value result;
    if (value_get_type(&min_val) != VAL_INT || value_get_type(&max_val) != VAL_INT) {
        value_init_int(&result, 0);
    } else {
        value_init_int(&result, random_int_range_sfc(vm, value_get_int(&min_val), value_get_int(&max_val)));
    }
    
    value_free(slot);
    *slot = result;
}

void builtin_random_double(VM* vm) {
    // arg0 (min) at count-2, arg1 (max) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value min_val = *slot;
    Value max_val = vm->stack.values[vm->stack.count - 1];

    Value result;
    ValueType min_type = value_get_type(&min_val);
    ValueType max_type = value_get_type(&max_val);
    if ((min_type != VAL_INT && min_type != VAL_DOUBLE) ||
        (max_type != VAL_INT && max_type != VAL_DOUBLE)) {
        value_init_double(&result, 0.0);
    } else {
        double min = (min_type == VAL_INT) ? (double)value_get_int(&min_val) : value_get_double(&min_val);
        double max = (max_type == VAL_INT) ? (double)value_get_int(&max_val) : value_get_double(&max_val);
        if (max < min) {
            double temp = min;
            min = max;
            max = temp;
        }
        double r = rng_next_double(vm);
        value_init_double(&result, min + (max - min) * r);
    }

    value_free(slot);
    *slot = result;
}

void builtin_random_bigint_bits(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value bits_val = *slot;

    Value result;
    if (value_get_type(&bits_val) != VAL_INT) {
        value_init_bigint(&result, obj_bigint_from_int64(0));
    } else {
        ObjBigInt* bi = random_bigint_bits_sfc(vm, value_get_int(&bits_val));
        value_init_bigint(&result, bi);
    }

    value_free(slot);
    *slot = result;
}

void builtin_random_bigint_range(VM* vm) {
    // arg0 (min) at count-2, arg1 (max) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value min_val = *slot;
    Value max_val = vm->stack.values[vm->stack.count - 1];

    Value result;
    ObjBigInt* min_bigint = value_get_bigint_obj(&min_val);
    ObjBigInt* max_bigint = value_get_bigint_obj(&max_val);
    if (value_get_type(&min_val) != VAL_BIGINT ||
        value_get_type(&max_val) != VAL_BIGINT ||
        !min_bigint ||
        !max_bigint) {
        value_init_bigint(&result, obj_bigint_from_int64(0));
    } else {
        ObjBigInt* bi = random_bigint_range_sfc(vm, min_bigint, max_bigint);
        value_init_bigint(&result, bi);
    }

    value_free(slot);
    *slot = result;
}

void builtin_random_fill_int(VM* vm) {
    // arg0 (array) at count-3, arg1 (min) at count-2, arg2 (max) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value arr_val = *slot;
    Value min_val = vm->stack.values[vm->stack.count - 2];
    Value max_val = vm->stack.values[vm->stack.count - 1];

    if (value_get_type(&arr_val) != VAL_ARRAY ||
        value_get_type(&min_val) != VAL_INT ||
        value_get_type(&max_val) != VAL_INT) {
        value_free(slot);
        *slot = arr_val;
        return;
    }

    ObjArray* arr = value_get_array_obj(&arr_val);
    if (!arr) {
        value_free(slot);
        *slot = arr_val;
        return;
    }
    int64_t min = value_get_int(&min_val);
    int64_t max = value_get_int(&max_val);
    if (arr->kind == ARRAY_KIND_INT) {
        for (int i = 0; i < arr->count; i++) {
            arr->data.ints[i] = random_int_range_sfc(vm, min, max);
        }
    } else {
        if (arr->kind != ARRAY_KIND_BOXED) obj_array_convert_to_boxed(arr);
        for (int i = 0; i < arr->count; i++) {
            value_free(&arr->data.elements[i]);
            Value v;
            value_init_int(&v, random_int_range_sfc(vm, min, max));
            arr->data.elements[i] = v;
        }
    }

    value_retain(&arr_val);
    value_free(slot);
    *slot = arr_val;
}

void builtin_random_fill_double(VM* vm) {
    // arg0 (array) at count-3, arg1 (min) at count-2, arg2 (max) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value arr_val = *slot;
    Value min_val = vm->stack.values[vm->stack.count - 2];
    Value max_val = vm->stack.values[vm->stack.count - 1];

    ValueType min_type = value_get_type(&min_val);
    ValueType max_type = value_get_type(&max_val);
    if (value_get_type(&arr_val) != VAL_ARRAY ||
        (min_type != VAL_INT && min_type != VAL_DOUBLE) ||
        (max_type != VAL_INT && max_type != VAL_DOUBLE)) {
        value_free(slot);
        *slot = arr_val;
        return;
    }

    double min = (min_type == VAL_INT) ? (double)value_get_int(&min_val) : value_get_double(&min_val);
    double max = (max_type == VAL_INT) ? (double)value_get_int(&max_val) : value_get_double(&max_val);
    if (max < min) {
        double temp = min;
        min = max;
        max = temp;
    }
    ObjArray* arr = value_get_array_obj(&arr_val);
    if (!arr) {
        value_free(slot);
        *slot = arr_val;
        return;
    }
    if (arr->kind == ARRAY_KIND_DOUBLE) {
        for (int i = 0; i < arr->count; i++) {
            arr->data.doubles[i] = min + (max - min) * rng_next_double(vm);
        }
    } else {
        if (arr->kind != ARRAY_KIND_BOXED) obj_array_convert_to_boxed(arr);
        for (int i = 0; i < arr->count; i++) {
            value_free(&arr->data.elements[i]);
            Value v;
            value_init_double(&v, min + (max - min) * rng_next_double(vm));
            arr->data.elements[i] = v;
        }
    }

    value_retain(&arr_val);
    value_free(slot);
    *slot = arr_val;
}

void builtin_random_fill_bigint_bits(VM* vm) {
    // arg0 (array) at count-2, arg1 (bits) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value arr_val = *slot;
    Value bits_val = vm->stack.values[vm->stack.count - 1];

    if (value_get_type(&arr_val) != VAL_ARRAY || value_get_type(&bits_val) != VAL_INT) {
        value_free(slot);
        *slot = arr_val;
        return;
    }

    ObjArray* arr = value_get_array_obj(&arr_val);
    if (!arr) {
        value_free(slot);
        *slot = arr_val;
        return;
    }
    int64_t bits = value_get_int(&bits_val);
    if (arr->kind != ARRAY_KIND_BOXED) obj_array_convert_to_boxed(arr);
    for (int i = 0; i < arr->count; i++) {
        value_free(&arr->data.elements[i]);
        Value v;
        ObjBigInt* bi = random_bigint_bits_sfc(vm, bits);
        value_init_bigint(&v, bi);
        arr->data.elements[i] = v;
    }

    value_retain(&arr_val);
    value_free(slot);
    *slot = arr_val;
}

void builtin_random_fill_bigint_range(VM* vm) {
    // arg0 (array) at count-3, arg1 (min) at count-2, arg2 (max) at count-1
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value arr_val = *slot;
    Value min_val = vm->stack.values[vm->stack.count - 2];
    Value max_val = vm->stack.values[vm->stack.count - 1];

    ObjBigInt* min_bigint = value_get_bigint_obj(&min_val);
    ObjBigInt* max_bigint = value_get_bigint_obj(&max_val);
    if (value_get_type(&arr_val) != VAL_ARRAY ||
        value_get_type(&min_val) != VAL_BIGINT ||
        value_get_type(&max_val) != VAL_BIGINT ||
        !min_bigint ||
        !max_bigint) {
        value_free(slot);
        *slot = arr_val;
        return;
    }

    ObjArray* arr = value_get_array_obj(&arr_val);
    if (!arr) {
        value_free(slot);
        *slot = arr_val;
        return;
    }
    if (arr->kind != ARRAY_KIND_BOXED) obj_array_convert_to_boxed(arr);
    for (int i = 0; i < arr->count; i++) {
        value_free(&arr->data.elements[i]);
        Value v;
        ObjBigInt* bi = random_bigint_range_sfc(vm, min_bigint, max_bigint);
        value_init_bigint(&v, bi);
        arr->data.elements[i] = v;
    }

    value_retain(&arr_val);
    value_free(slot);
    *slot = arr_val;
}

void builtin_secure_random(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    uint64_t r = 0;
    if (!secure_next_u64(&r)) {
        Value ok;
        value_init_double(&ok, 0.0);
        result_tuple_set(vm, slot, ok, ERR_CRYPTO, "Secure random unavailable");
        return;
    }

    Value ok;
    value_init_double(&ok, double_from_u64(r));
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_secure_random_int(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value min_val = *slot;
    Value max_val = vm->stack.values[vm->stack.count - 1];

    if (value_get_type(&min_val) != VAL_INT || value_get_type(&max_val) != VAL_INT) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "secureRandomInt expects (int, int)");
        return;
    }

    int64_t r = 0;
    if (!random_int_range_secure(value_get_int(&min_val), value_get_int(&max_val), &r)) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_CRYPTO, "Secure random unavailable");
        return;
    }

    Value ok;
    value_init_int(&ok, r);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_secure_random_double(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value min_val = *slot;
    Value max_val = vm->stack.values[vm->stack.count - 1];

    ValueType min_type = value_get_type(&min_val);
    ValueType max_type = value_get_type(&max_val);
    if ((min_type != VAL_INT && min_type != VAL_DOUBLE) ||
        (max_type != VAL_INT && max_type != VAL_DOUBLE)) {
        Value ok;
        value_init_double(&ok, 0.0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "secureRandomDouble expects (double|int, double|int)");
        return;
    }

    double min = (min_type == VAL_INT) ? (double)value_get_int(&min_val) : value_get_double(&min_val);
    double max = (max_type == VAL_INT) ? (double)value_get_int(&max_val) : value_get_double(&max_val);
    if (max < min) {
        double temp = min;
        min = max;
        max = temp;
    }
    uint64_t r = 0;
    if (!secure_next_u64(&r)) {
        Value ok;
        value_init_double(&ok, 0.0);
        result_tuple_set(vm, slot, ok, ERR_CRYPTO, "Secure random unavailable");
        return;
    }

    Value ok;
    value_init_double(&ok, min + (max - min) * double_from_u64(r));
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_secure_random_bigint_bits(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value bits_val = *slot;

    if (value_get_type(&bits_val) != VAL_INT) {
        Value ok;
        value_init_bigint(&ok, obj_bigint_from_int64(0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "secureRandomBigIntBits expects int");
        return;
    }

    bool ok = false;
    ObjBigInt* bi = random_bigint_bits_secure(value_get_int(&bits_val), &ok);
    if (!ok) {
        if (bi) obj_bigint_release(bi);
        Value out;
        value_init_bigint(&out, obj_bigint_from_int64(0));
        result_tuple_set(vm, slot, out, ERR_CRYPTO, "Secure random unavailable");
        return;
    }

    Value out;
    value_init_bigint(&out, bi);
    result_tuple_set(vm, slot, out, 0, NULL);
}

void builtin_secure_random_bigint_range(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value min_val = *slot;
    Value max_val = vm->stack.values[vm->stack.count - 1];

    ObjBigInt* min_bigint = value_get_bigint_obj(&min_val);
    ObjBigInt* max_bigint = value_get_bigint_obj(&max_val);
    if (value_get_type(&min_val) != VAL_BIGINT ||
        value_get_type(&max_val) != VAL_BIGINT ||
        !min_bigint ||
        !max_bigint) {
        Value ok;
        value_init_bigint(&ok, obj_bigint_from_int64(0));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "secureRandomBigIntRange expects (bigint, bigint)");
        return;
    }

    bool ok = false;
    ObjBigInt* bi = random_bigint_range_secure(min_bigint, max_bigint, &ok);
    if (!ok) {
        if (bi) obj_bigint_release(bi);
        Value out;
        value_init_bigint(&out, obj_bigint_from_int64(0));
        result_tuple_set(vm, slot, out, ERR_CRYPTO, "Secure random unavailable");
        return;
    }

    Value out;
    value_init_bigint(&out, bi);
    result_tuple_set(vm, slot, out, 0, NULL);
}

void builtin_secure_random_fill_int(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value arr_val = *slot;
    Value min_val = vm->stack.values[vm->stack.count - 2];
    Value max_val = vm->stack.values[vm->stack.count - 1];

    if (value_get_type(&arr_val) != VAL_ARRAY ||
        value_get_type(&min_val) != VAL_INT ||
        value_get_type(&max_val) != VAL_INT) {
        ObjArray* empty = obj_array_create_typed(vm, 0, ARRAY_KIND_INT);
        Value ok;
        value_init_array(&ok, empty);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "secureRandomFillInt expects (array<int>, int, int)");
        return;
    }

    const char* err = NULL;
    ObjArray* arr = value_get_array_obj(&arr_val);
    if (!arr) {
        ObjArray* empty = obj_array_create_typed(vm, 0, ARRAY_KIND_INT);
        Value ok;
        value_init_array(&ok, empty);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "secureRandomFillInt expects (array<int>, int, int)");
        return;
    }
    int64_t min = value_get_int(&min_val);
    int64_t max = value_get_int(&max_val);
    if (arr->kind == ARRAY_KIND_INT) {
        for (int i = 0; i < arr->count; i++) {
            int64_t r = 0;
            if (!random_int_range_secure(min, max, &r)) {
                err = "Secure random unavailable";
                break;
            }
            arr->data.ints[i] = r;
        }
    } else {
        if (arr->kind != ARRAY_KIND_BOXED) obj_array_convert_to_boxed(arr);
        for (int i = 0; i < arr->count; i++) {
            int64_t r = 0;
            if (!random_int_range_secure(min, max, &r)) {
                err = "Secure random unavailable";
                break;
            }
            value_free(&arr->data.elements[i]);
            Value v;
            value_init_int(&v, r);
            arr->data.elements[i] = v;
        }
    }

    Value ok = arr_val;
    value_retain(&ok);
    result_tuple_set(vm, slot, ok, err ? ERR_CRYPTO : 0, err);
}

void builtin_secure_random_fill_double(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value arr_val = *slot;
    Value min_val = vm->stack.values[vm->stack.count - 2];
    Value max_val = vm->stack.values[vm->stack.count - 1];

    ValueType min_type = value_get_type(&min_val);
    ValueType max_type = value_get_type(&max_val);
    if (value_get_type(&arr_val) != VAL_ARRAY ||
        (min_type != VAL_INT && min_type != VAL_DOUBLE) ||
        (max_type != VAL_INT && max_type != VAL_DOUBLE)) {
        ObjArray* empty = obj_array_create_typed(vm, 0, ARRAY_KIND_DOUBLE);
        Value ok;
        value_init_array(&ok, empty);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "secureRandomFillDouble expects (array<double>, double|int, double|int)");
        return;
    }

    double min = (min_type == VAL_INT) ? (double)value_get_int(&min_val) : value_get_double(&min_val);
    double max = (max_type == VAL_INT) ? (double)value_get_int(&max_val) : value_get_double(&max_val);
    if (max < min) {
        double temp = min;
        min = max;
        max = temp;
    }
    const char* err = NULL;
    ObjArray* arr = value_get_array_obj(&arr_val);
    if (!arr) {
        ObjArray* empty = obj_array_create_typed(vm, 0, ARRAY_KIND_DOUBLE);
        Value ok;
        value_init_array(&ok, empty);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "secureRandomFillDouble expects (array<double>, double|int, double|int)");
        return;
    }
    if (arr->kind == ARRAY_KIND_DOUBLE) {
        for (int i = 0; i < arr->count; i++) {
            uint64_t r = 0;
            if (!secure_next_u64(&r)) {
                err = "Secure random unavailable";
                break;
            }
            arr->data.doubles[i] = min + (max - min) * double_from_u64(r);
        }
    } else {
        if (arr->kind != ARRAY_KIND_BOXED) obj_array_convert_to_boxed(arr);
        for (int i = 0; i < arr->count; i++) {
            uint64_t r = 0;
            if (!secure_next_u64(&r)) {
                err = "Secure random unavailable";
                break;
            }
            value_free(&arr->data.elements[i]);
            Value v;
            value_init_double(&v, min + (max - min) * double_from_u64(r));
            arr->data.elements[i] = v;
        }
    }

    Value ok = arr_val;
    value_retain(&ok);
    result_tuple_set(vm, slot, ok, err ? ERR_CRYPTO : 0, err);
}

void builtin_secure_random_fill_bigint_bits(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value arr_val = *slot;
    Value bits_val = vm->stack.values[vm->stack.count - 1];

    if (value_get_type(&arr_val) != VAL_ARRAY || value_get_type(&bits_val) != VAL_INT) {
        ObjArray* empty = obj_array_create_typed(vm, 0, ARRAY_KIND_BOXED);
        Value ok;
        value_init_array(&ok, empty);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "secureRandomFillBigIntBits expects (array<bigint>, int)");
        return;
    }

    const char* err = NULL;
    ObjArray* arr = value_get_array_obj(&arr_val);
    if (!arr) {
        ObjArray* empty = obj_array_create_typed(vm, 0, ARRAY_KIND_BOXED);
        Value ok;
        value_init_array(&ok, empty);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "secureRandomFillBigIntBits expects (array<bigint>, int)");
        return;
    }
    int64_t bits = value_get_int(&bits_val);
    if (arr->kind != ARRAY_KIND_BOXED) obj_array_convert_to_boxed(arr);
    for (int i = 0; i < arr->count; i++) {
        bool ok = false;
        ObjBigInt* bi = random_bigint_bits_secure(bits, &ok);
        if (!ok) {
            obj_bigint_release(bi);
            err = "Secure random unavailable";
            break;
        }
        value_free(&arr->data.elements[i]);
        Value v;
        value_init_bigint(&v, bi);
        arr->data.elements[i] = v;
    }

    Value out = arr_val;
    value_retain(&out);
    result_tuple_set(vm, slot, out, err ? ERR_CRYPTO : 0, err);
}

void builtin_secure_random_fill_bigint_range(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value arr_val = *slot;
    Value min_val = vm->stack.values[vm->stack.count - 2];
    Value max_val = vm->stack.values[vm->stack.count - 1];

    ObjBigInt* min_bigint = value_get_bigint_obj(&min_val);
    ObjBigInt* max_bigint = value_get_bigint_obj(&max_val);
    if (value_get_type(&arr_val) != VAL_ARRAY ||
        value_get_type(&min_val) != VAL_BIGINT ||
        value_get_type(&max_val) != VAL_BIGINT ||
        !min_bigint ||
        !max_bigint) {
        ObjArray* empty = obj_array_create_typed(vm, 0, ARRAY_KIND_BOXED);
        Value ok;
        value_init_array(&ok, empty);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "secureRandomFillBigIntRange expects (array<bigint>, bigint, bigint)");
        return;
    }

    const char* err = NULL;
    ObjArray* arr = value_get_array_obj(&arr_val);
    if (!arr) {
        ObjArray* empty = obj_array_create_typed(vm, 0, ARRAY_KIND_BOXED);
        Value ok;
        value_init_array(&ok, empty);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "secureRandomFillBigIntRange expects (array<bigint>, bigint, bigint)");
        return;
    }
    if (arr->kind != ARRAY_KIND_BOXED) obj_array_convert_to_boxed(arr);
    for (int i = 0; i < arr->count; i++) {
        bool ok = false;
        ObjBigInt* bi = random_bigint_range_secure(min_bigint, max_bigint, &ok);
        if (!ok) {
            obj_bigint_release(bi);
            err = "Secure random unavailable";
            break;
        }
        value_free(&arr->data.elements[i]);
        Value v;
        value_init_bigint(&v, bi);
        arr->data.elements[i] = v;
    }

    Value out = arr_val;
    value_retain(&out);
    result_tuple_set(vm, slot, out, err ? ERR_CRYPTO : 0, err);
}
// ============ Time/Date Functions ============

static int64_t time_now_unix_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    const uint64_t epoch = 116444736000000000ULL;
    if (t < epoch) return 0;
    uint64_t unix100ns = t - epoch;
    return (int64_t)(unix100ns / 10000ULL);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000L);
#endif
}

static int64_t time_now_unix_ns(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    const uint64_t epoch = 116444736000000000ULL;
    if (t < epoch) return 0;
    uint64_t unix100ns = t - epoch;
    return (int64_t)(unix100ns * 100ULL);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

static int64_t time_monotonic_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq = {0};
    static int init = 0;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = 1;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (int64_t)((counter.QuadPart * 1000LL) / freq.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000L);
#endif
}

static void time_now_datetime(int local, int* year, int* month, int* day,
                              int* hour, int* minute, int* second, int* millis) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    const uint64_t epoch = 116444736000000000ULL;
    if (t < epoch) {
        *year = *month = *day = *hour = *minute = *second = *millis = 0;
        return;
    }
    uint64_t unix100ns = t - epoch;
    time_t secs = (time_t)(unix100ns / 10000000ULL);
    int ms = (int)((unix100ns / 10000ULL) % 1000ULL);
    struct tm tm;
    if (local) {
        localtime_s(&tm, &secs);
    } else {
        gmtime_s(&tm, &secs);
    }
    *year = tm.tm_year + 1900;
    *month = tm.tm_mon + 1;
    *day = tm.tm_mday;
    *hour = tm.tm_hour;
    *minute = tm.tm_min;
    *second = tm.tm_sec;
    *millis = ms;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        *year = *month = *day = *hour = *minute = *second = *millis = 0;
        return;
    }
    time_t secs = ts.tv_sec;
    struct tm tm;
    if (local) {
        localtime_r(&secs, &tm);
    } else {
        gmtime_r(&secs, &tm);
    }
    *year = tm.tm_year + 1900;
    *month = tm.tm_mon + 1;
    *day = tm.tm_mday;
    *hour = tm.tm_hour;
    *minute = tm.tm_min;
    *second = tm.tm_sec;
    *millis = (int)(ts.tv_nsec / 1000000L);
#endif
}

static void builtin_datetime_result(VM* vm, int local) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value result;

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millis = 0;
    time_now_datetime(local, &year, &month, &day, &hour, &minute, &second, &millis);

    ObjArray* result_arr = obj_array_create(vm, 7);
    Value v;
    value_init_int(&v, year);
    obj_array_push(result_arr, v);
    value_init_int(&v, month);
    obj_array_push(result_arr, v);
    value_init_int(&v, day);
    obj_array_push(result_arr, v);
    value_init_int(&v, hour);
    obj_array_push(result_arr, v);
    value_init_int(&v, minute);
    obj_array_push(result_arr, v);
    value_init_int(&v, second);
    obj_array_push(result_arr, v);
    value_init_int(&v, millis);
    obj_array_push(result_arr, v);

    value_init_array(&result, result_arr);
    value_free(slot);
    *slot = result;
}

void builtin_time_now_millis(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value result;
    value_init_int(&result, time_now_unix_ms());
    value_free(slot);
    *slot = result;
}

void builtin_time_now_nanos(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value result;
    value_init_int(&result, time_now_unix_ns());
    value_free(slot);
    *slot = result;
}

void builtin_time_monotonic_millis(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value result;
    value_init_int(&result, time_monotonic_ms());
    value_free(slot);
    *slot = result;
}

void builtin_time_since_millis(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value start = *slot;
    Value result;

    if (value_get_type(&start) != VAL_INT) {
        value_init_int(&result, 0);
    } else {
        int64_t now = time_monotonic_ms();
        int64_t elapsed = now - value_get_int(&start);
        if (elapsed < 0) elapsed = 0;
        value_init_int(&result, elapsed);
    }

    value_free(slot);
    *slot = result;
}

void builtin_utc_datetime(VM* vm) {
    builtin_datetime_result(vm, 0);
}

void builtin_local_datetime(VM* vm) {
    builtin_datetime_result(vm, 1);
}

void builtin_log_json(VM* vm) {
    // arg0 (level) at count-3, arg1 (message) at count-2, arg2 (fields) at count-1
    Value* level_slot = &vm->stack.values[vm->stack.count - 3];
    Value level_val = *level_slot;
    Value message_val = vm->stack.values[vm->stack.count - 2];
    Value fields_val = vm->stack.values[vm->stack.count - 1];

    ObjString* level_str = value_get_string_obj(&level_val);
    ObjString* message_str = value_get_string_obj(&message_val);
    ObjMap* fields_map = value_get_map_obj(&fields_val);
    if (value_get_type(&level_val) != VAL_STRING ||
        !level_str ||
        value_get_type(&message_val) != VAL_STRING ||
        !message_str ||
        value_get_type(&fields_val) != VAL_MAP ||
        !fields_map) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, level_slot, ok, ERR_INVALID_ARGUMENT, "logJson expects (string, string, map<string, any>)");
        return;
    }

    ObjMap* entry = obj_map_create(vm);
    Value v;

    value_init_int(&v, time_now_unix_ms());
    obj_map_set_cstr(entry, "tsMillis", v);
    value_free(&v);

    value_init_string(&v, level_str->chars ? level_str->chars : "");
    obj_map_set_cstr(entry, "level", v);
    value_free(&v);

    value_init_string(&v, message_str->chars ? message_str->chars : "");
    obj_map_set_cstr(entry, "message", v);
    value_free(&v);

    obj_map_set_cstr(entry, "fields", fields_val);

    Value entry_val;
    value_init_map(&entry_val, entry);

    char error_buf[128];
    error_buf[0] = '\0';
    cJSON* root = json_to_cjson(vm, &entry_val, 0, error_buf, sizeof(error_buf));
    value_free(&entry_val);

    if (!root) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         level_slot,
                         ok,
                         strstr(error_buf, "nesting limit") ? ERR_LIMIT : ERR_INVALID_ARGUMENT,
                         error_buf[0] ? error_buf : "logJson failed");
        return;
    }

    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, level_slot, ok, ERR_INTERNAL, "logJson failed");
        return;
    }

    int wrote = fprintf(stderr, "%s\n", rendered);
    free(rendered);
    if (wrote < 0 || fflush(stderr) != 0) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, level_slot, ok, ERR_IO, "Failed to write log");
        return;
    }

    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, level_slot, ok, 0, NULL);
}

// ============ Array Functions ============

static int compare_ints(const void* a, const void* b) {
    Value* va = (Value*)a;
    Value* vb = (Value*)b;
    int64_t ai = value_get_int(va);
    int64_t bi = value_get_int(vb);
    if (ai < bi) return -1;
    if (ai > bi) return 1;
    return 0;
}

static int compare_doubles(const void* a, const void* b) {
    Value* va = (Value*)a;
    Value* vb = (Value*)b;
    double ad = value_get_double(va);
    double bd = value_get_double(vb);
    if (ad < bd) return -1;
    if (ad > bd) return 1;
    return 0;
}

static int compare_strings(const void* a, const void* b) {
    const Value* va = (const Value*)a;
    const Value* vb = (const Value*)b;
    const ObjString* sa = (va && value_get_type(va) == VAL_STRING) ? value_get_string_obj(va) : NULL;
    const ObjString* sb = (vb && value_get_type(vb) == VAL_STRING) ? value_get_string_obj(vb) : NULL;

    if (!sa && !sb) return 0;
    if (!sa) return -1;
    if (!sb) return 1;

    return strcmp(sa->chars ? sa->chars : "", sb->chars ? sb->chars : "");
}

static int compare_bools_boxed(const void* a, const void* b) {
    const Value* va = (const Value*)a;
    const Value* vb = (const Value*)b;
    const int ai = (va && value_get_type(va) == VAL_BOOL && value_get_bool(va)) ? 1 : 0;
    const int bi = (vb && value_get_type(vb) == VAL_BOOL && value_get_bool(vb)) ? 1 : 0;
    if (ai < bi) return -1;
    if (ai > bi) return 1;
    return 0;
}

static int compare_int64s(const void* a, const void* b) {
    int64_t va = *(const int64_t*)a;
    int64_t vb = *(const int64_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static int compare_raw_doubles(const void* a, const void* b) {
    double va = *(const double*)a;
    double vb = *(const double*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static int compare_u8s(const void* a, const void* b) {
    uint8_t va = *(const uint8_t*)a;
    uint8_t vb = *(const uint8_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static bool sort_boxed_array_type_is_homogeneous(const ObjArray* arr, ValueType* out_type) {
    if (!arr || arr->kind != ARRAY_KIND_BOXED || arr->count <= 0 || !arr->data.elements) {
        return false;
    }

    ValueType elem_type = value_get_type(&arr->data.elements[0]);
    if (elem_type != VAL_INT &&
        elem_type != VAL_DOUBLE &&
        elem_type != VAL_STRING &&
        elem_type != VAL_BOOL) {
        return false;
    }

    for (int i = 0; i < arr->count; i++) {
        const Value* elem = &arr->data.elements[i];
        if (value_get_type(elem) != elem_type) return false;
        if (elem_type == VAL_STRING && value_get_string_obj(elem) == NULL) return false;
    }

    if (out_type) *out_type = elem_type;
    return true;
}

void builtin_sort(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];

    if (!slot || value_get_type(slot) != VAL_ARRAY || !value_get_array_obj(slot)) return;

    ObjArray* arr = value_get_array_obj(slot);
    const int count = arr->count;
    if (count <= 1) return;

    if (arr->kind == ARRAY_KIND_INT && arr->data.ints) {
        qsort(arr->data.ints, (size_t)count, sizeof(int64_t), compare_int64s);
        return;
    }
    if (arr->kind == ARRAY_KIND_BYTE && arr->data.bytes) {
        qsort(arr->data.bytes, (size_t)count, sizeof(uint8_t), compare_u8s);
        return;
    }
    if (arr->kind == ARRAY_KIND_DOUBLE && arr->data.doubles) {
        qsort(arr->data.doubles, (size_t)count, sizeof(double), compare_raw_doubles);
        return;
    }
    if (arr->kind == ARRAY_KIND_BOOL && arr->data.bools) {
        qsort(arr->data.bools, (size_t)count, sizeof(uint8_t), compare_u8s);
        return;
    }
    if (arr->kind != ARRAY_KIND_BOXED) return;

    ValueType elem_type = VAL_NIL;
    if (!sort_boxed_array_type_is_homogeneous(arr, &elem_type)) {
        // Keep current order when boxed values are mixed/unsupported.
        return;
    }

    if (elem_type == VAL_INT) {
        qsort(arr->data.elements, (size_t)count, sizeof(Value), compare_ints);
    } else if (elem_type == VAL_DOUBLE) {
        qsort(arr->data.elements, (size_t)count, sizeof(Value), compare_doubles);
    } else if (elem_type == VAL_STRING) {
        qsort(arr->data.elements, (size_t)count, sizeof(Value), compare_strings);
    } else if (elem_type == VAL_BOOL) {
        qsort(arr->data.elements, (size_t)count, sizeof(Value), compare_bools_boxed);
    }
}

void builtin_reverse(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];

    if (!slot || value_get_type(slot) != VAL_ARRAY || !value_get_array_obj(slot)) return;

    ObjArray* arr = value_get_array_obj(slot);
    int count = arr->count;
    if (count <= 1) return;
    
    for (int i = 0; i < count / 2; i++) {
        int j = count - 1 - i;
        if (arr->kind == ARRAY_KIND_BOXED) {
            Value temp = arr->data.elements[i];
            arr->data.elements[i] = arr->data.elements[j];
            arr->data.elements[j] = temp;
        } else if (arr->kind == ARRAY_KIND_INT) {
            int64_t temp = arr->data.ints[i];
            arr->data.ints[i] = arr->data.ints[j];
            arr->data.ints[j] = temp;
        } else if (arr->kind == ARRAY_KIND_BYTE) {
            uint8_t temp = arr->data.bytes[i];
            arr->data.bytes[i] = arr->data.bytes[j];
            arr->data.bytes[j] = temp;
        } else if (arr->kind == ARRAY_KIND_DOUBLE) {
            double temp = arr->data.doubles[i];
            arr->data.doubles[i] = arr->data.doubles[j];
            arr->data.doubles[j] = temp;
        } else if (arr->kind == ARRAY_KIND_BOOL) {
            uint8_t temp = arr->data.bools[i];
            arr->data.bools[i] = arr->data.bools[j];
            arr->data.bools[j] = temp;
        }
    }
}

void builtin_find_array(VM* vm) {
    // arg0 (array) at count-2, arg1 (value) at count-1
    Value* arr_slot = &vm->stack.values[vm->stack.count - 2];
    Value search_val = vm->stack.values[vm->stack.count - 1];
    Value arr_val = *arr_slot;
    
    if (value_get_type(&arr_val) != VAL_ARRAY) {
        Value result;
        value_init_int(&result, -1);
        value_free(arr_slot);
        *arr_slot = result;
        return;
    }
    
    ObjArray* arr = value_get_array_obj(&arr_val);
    if (!arr) {
        Value result;
        value_init_int(&result, -1);
        value_free(arr_slot);
        *arr_slot = result;
        return;
    }
    int index = -1;
    
    for (int i = 0; i < arr->count; i++) {
        if (arr->kind == ARRAY_KIND_BOXED) {
            if (value_equals(&arr->data.elements[i], &search_val)) {
                index = i;
                break;
            }
        } else if (arr->kind == ARRAY_KIND_INT) {
            if (value_get_type(&search_val) == VAL_INT && arr->data.ints[i] == value_get_int(&search_val)) {
                index = i;
                break;
            }
        } else if (arr->kind == ARRAY_KIND_BYTE) {
            if (value_get_type(&search_val) == VAL_INT && (int64_t)arr->data.bytes[i] == value_get_int(&search_val)) {
                index = i;
                break;
            }
        } else if (arr->kind == ARRAY_KIND_DOUBLE) {
            if (value_get_type(&search_val) == VAL_DOUBLE && arr->data.doubles[i] == value_get_double(&search_val)) {
                index = i;
                break;
            }
        } else if (arr->kind == ARRAY_KIND_BOOL) {
            if (value_get_type(&search_val) == VAL_BOOL &&
                (arr->data.bools[i] != 0) == value_get_bool(&search_val)) {
                index = i;
                break;
            }
        }
    }
    
    Value result;
    value_init_int(&result, index);
    value_free(arr_slot);
    *arr_slot = result;
}

void builtin_contains(VM* vm) {
    // arg0 (array) at count-2, arg1 (value) at count-1
    Value* arr_slot = &vm->stack.values[vm->stack.count - 2];
    Value search_val = vm->stack.values[vm->stack.count - 1];
    Value arr_val = *arr_slot;
    
    if (value_get_type(&arr_val) != VAL_ARRAY) {
        Value result;
        value_init_bool(&result, false);
        value_free(arr_slot);
        *arr_slot = result;
        return;
    }
    
    ObjArray* arr = value_get_array_obj(&arr_val);
    if (!arr) {
        Value result;
        value_init_bool(&result, false);
        value_free(arr_slot);
        *arr_slot = result;
        return;
    }
    bool found = false;
    
    for (int i = 0; i < arr->count; i++) {
        if (arr->kind == ARRAY_KIND_BOXED) {
            if (value_equals(&arr->data.elements[i], &search_val)) {
                found = true;
                break;
            }
        } else if (arr->kind == ARRAY_KIND_INT) {
            if (value_get_type(&search_val) == VAL_INT && arr->data.ints[i] == value_get_int(&search_val)) {
                found = true;
                break;
            }
        } else if (arr->kind == ARRAY_KIND_BYTE) {
            if (value_get_type(&search_val) == VAL_INT && (int64_t)arr->data.bytes[i] == value_get_int(&search_val)) {
                found = true;
                break;
            }
        } else if (arr->kind == ARRAY_KIND_DOUBLE) {
            if (value_get_type(&search_val) == VAL_DOUBLE && arr->data.doubles[i] == value_get_double(&search_val)) {
                found = true;
                break;
            }
        } else if (arr->kind == ARRAY_KIND_BOOL) {
            if (value_get_type(&search_val) == VAL_BOOL &&
                (arr->data.bools[i] != 0) == value_get_bool(&search_val)) {
                found = true;
                break;
            }
        }
    }
    
    Value result;
    value_init_bool(&result, found);
    value_free(arr_slot);
    *arr_slot = result;
}

void builtin_slice(VM* vm) {
    // arg0 (array) at count-3, arg1 (start) at count-2, arg2 (end) at count-1
    Value* arr_slot = &vm->stack.values[vm->stack.count - 3];
    Value start_val = vm->stack.values[vm->stack.count - 2];
    Value end_val = vm->stack.values[vm->stack.count - 1];
    Value arr_val = *arr_slot;
    
    ValueType arr_type = value_get_type(&arr_val);
    if ((arr_type != VAL_ARRAY && arr_type != VAL_BYTES) ||
        value_get_type(&start_val) != VAL_INT ||
        value_get_type(&end_val) != VAL_INT) {
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }

    if (arr_type == VAL_BYTES) {
        ObjBytes* bytes = value_get_bytes_obj(&arr_val);
        int start = (int)value_get_int(&start_val);
        int end = (int)value_get_int(&end_val);
        int count = bytes ? bytes->length : 0;

        if (start < 0) start = 0;
        if (end > count) end = count;
        if (end < start) end = start;

        Value result;
        value_init_bytes(&result, obj_bytes_slice(bytes, start, end));
        value_free(arr_slot);
        *arr_slot = result;
        return;
    }

    ObjArray* arr = value_get_array_obj(&arr_val);
    if (!arr) {
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }
    int start = (int)value_get_int(&start_val);
    int end = (int)value_get_int(&end_val);
    int count = arr->count;
    
    // Handle negative indices
    if (start < 0) start = 0;
    if (end > count) end = count;
    if (end < start) end = start;
    
    int slice_count = end - start;
    ObjArray* result_arr = obj_array_create_typed(vm, slice_count, arr->kind);
    result_arr->count = slice_count;
    for (int i = 0; i < slice_count; i++) {
        int src_idx = start + i;
        if (arr->kind == ARRAY_KIND_BOXED) {
            Value elem = arr->data.elements[src_idx];
            value_retain(&elem);
            result_arr->data.elements[i] = elem;
        } else if (arr->kind == ARRAY_KIND_INT) {
            result_arr->data.ints[i] = arr->data.ints[src_idx];
        } else if (arr->kind == ARRAY_KIND_BYTE) {
            result_arr->data.bytes[i] = arr->data.bytes[src_idx];
        } else if (arr->kind == ARRAY_KIND_DOUBLE) {
            result_arr->data.doubles[i] = arr->data.doubles[src_idx];
        } else if (arr->kind == ARRAY_KIND_BOOL) {
            result_arr->data.bools[i] = arr->data.bools[src_idx];
        } else {
            Value nil;
            value_init_nil(&nil);
            obj_array_convert_to_boxed(result_arr);
            result_arr->data.elements[i] = nil;
        }
    }
    
    Value result;
    value_init_array(&result, result_arr);
    value_free(arr_slot);
    *arr_slot = result;
}

void builtin_join(VM* vm) {
    // arg0 (array) at count-2, arg1 (delimiter) at count-1
    Value* arr_slot = &vm->stack.values[vm->stack.count - 2];
    Value delim_val = vm->stack.values[vm->stack.count - 1];
    Value arr_val = *arr_slot;
    
    ObjString* delim_str = value_get_string_obj(&delim_val);
    if (value_get_type(&arr_val) != VAL_ARRAY ||
        value_get_type(&delim_val) != VAL_STRING ||
        !delim_str ||
        !delim_str->chars) {
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }
    
    ObjArray* arr = value_get_array_obj(&arr_val);
    if (!arr) {
        Value nil;
        value_init_nil(&nil);
        value_free(arr_slot);
        *arr_slot = nil;
        return;
    }
    const char* delim = delim_str->chars;
    int delim_len = delim_str->length;
    
    // Calculate total length
    int total_len = 0;
    for (int i = 0; i < arr->count; i++) {
        Value elem;
        if (arr->kind == ARRAY_KIND_BOXED) {
            elem = arr->data.elements[i];
        } else if (arr->kind == ARRAY_KIND_INT) {
            value_init_int(&elem, arr->data.ints[i]);
        } else if (arr->kind == ARRAY_KIND_BYTE) {
            value_init_int(&elem, (int64_t)arr->data.bytes[i]);
        } else if (arr->kind == ARRAY_KIND_DOUBLE) {
            value_init_double(&elem, arr->data.doubles[i]);
        } else if (arr->kind == ARRAY_KIND_BOOL) {
            value_init_bool(&elem, arr->data.bools[i] != 0);
        } else {
            value_init_nil(&elem);
        }

        if (value_get_type(&elem) == VAL_STRING && value_get_string_obj(&elem)) {
            total_len += value_get_string_obj(&elem)->length;
        } else {
            // Convert to string representation
            char buf[256];
            if (value_get_type(&elem) == VAL_INT) {
                snprintf(buf, sizeof(buf), "%lld", (long long)value_get_int(&elem));
            } else if (value_get_type(&elem) == VAL_BOOL) {
                snprintf(buf, sizeof(buf), "%s", value_get_bool(&elem) ? "true" : "false");
            } else if (value_get_type(&elem) == VAL_DOUBLE) {
                snprintf(buf, sizeof(buf), "%g", value_get_double(&elem));
            } else if (value_get_type(&elem) == VAL_BIGINT) {
                char* str = obj_bigint_to_string(value_get_bigint_obj(&elem));
                if (str) {
                    total_len += (int)strlen(str);
                    free(str);
                }
                buf[0] = '\0';
            } else {
                buf[0] = '\0';
            }
            total_len += (int)strlen(buf);
        }
        if (i < arr->count - 1) {
            total_len += delim_len;
        }
    }
    
    // Build result string
    char* result = (char*)safe_malloc(total_len + 1);
    char* p = result;
    
    for (int i = 0; i < arr->count; i++) {
        const char* elem_str;
        char buf[256];

        Value elem;
        if (arr->kind == ARRAY_KIND_BOXED) {
            elem = arr->data.elements[i];
        } else if (arr->kind == ARRAY_KIND_INT) {
            value_init_int(&elem, arr->data.ints[i]);
        } else if (arr->kind == ARRAY_KIND_BYTE) {
            value_init_int(&elem, (int64_t)arr->data.bytes[i]);
        } else if (arr->kind == ARRAY_KIND_DOUBLE) {
            value_init_double(&elem, arr->data.doubles[i]);
        } else if (arr->kind == ARRAY_KIND_BOOL) {
            value_init_bool(&elem, arr->data.bools[i] != 0);
        } else {
            value_init_nil(&elem);
        }

        if (value_get_type(&elem) == VAL_STRING && value_get_string_obj(&elem)) {
            elem_str = value_get_string_obj(&elem)->chars;
        } else if (value_get_type(&elem) == VAL_INT) {
            snprintf(buf, sizeof(buf), "%lld", (long long)value_get_int(&elem));
            elem_str = buf;
        } else if (value_get_type(&elem) == VAL_BOOL) {
            snprintf(buf, sizeof(buf), "%s", value_get_bool(&elem) ? "true" : "false");
            elem_str = buf;
        } else if (value_get_type(&elem) == VAL_DOUBLE) {
            snprintf(buf, sizeof(buf), "%g", value_get_double(&elem));
            elem_str = buf;
        } else if (value_get_type(&elem) == VAL_BIGINT) {
            char* str = obj_bigint_to_string(value_get_bigint_obj(&elem));
            elem_str = str ? str : "";
            int elem_len_big = (int)strlen(elem_str);
            memcpy(p, elem_str, elem_len_big);
            p += elem_len_big;
            if (str) free(str);
            if (i < arr->count - 1) {
                memcpy(p, delim, delim_len);
                p += delim_len;
            }
            continue;
        } else {
            elem_str = "";
        }
        
        size_t elem_len = strlen(elem_str);
        memcpy(p, elem_str, elem_len);
        p += elem_len;
        
        if (i < arr->count - 1) {
            memcpy(p, delim, delim_len);
            p += delim_len;
        }
    }
    *p = '\0';
    
    Value result_val;
    value_init_string(&result_val, result);
    free(result);
    
    value_free(arr_slot);
    *arr_slot = result_val;
}

// ============ Networking Functions ============

static bool httpx_char_is_ctl(unsigned char ch) {
    return ch < 0x20 || ch == 0x7f;
}

static bool httpx_host_is_safe(const char* host, size_t host_len) {
    if (!host || host_len == 0) return false;
    for (size_t i = 0; i < host_len; i++) {
        unsigned char ch = (unsigned char)host[i];
        if (httpx_char_is_ctl(ch) || ch == ' ') return false;
        if (ch == '/' || ch == '\\') return false;
    }
    return true;
}

static bool httpx_path_is_safe(const char* path) {
    if (!path || path[0] == '\0') return false;
    for (const unsigned char* p = (const unsigned char*)path; *p; p++) {
        if (httpx_char_is_ctl(*p) || *p == ' ') return false;
    }
    return true;
}

static bool httpx_header_name_is_safe(const char* key) {
    if (!key || key[0] == '\0') return false;
    for (const unsigned char* p = (const unsigned char*)key; *p; p++) {
        unsigned char ch = *p;
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9')) {
            continue;
        }
        switch (ch) {
            case '!':
            case '#':
            case '$':
            case '%':
            case '&':
            case '\'':
            case '*':
            case '+':
            case '-':
            case '.':
            case '^':
            case '_':
            case '`':
            case '|':
            case '~':
                continue;
            default:
                return false;
        }
    }
    return true;
}

static bool httpx_header_value_is_safe(const char* value) {
    if (!value) return false;
    for (const unsigned char* p = (const unsigned char*)value; *p; p++) {
        if (httpx_char_is_ctl(*p)) return false;
    }
    return true;
}

// Helper function to parse URL and extract host, port, and path
static int parse_url(const char* url, char* host, int* port, char* path, int* is_https) {
    if (!url || !host || !port || !path || !is_https) return -1;
    *is_https = 0;
    *port = 80;
    
    // Check for protocol
    const char* p = url;
    if (strncmp(p, "https://", 8) == 0) {
        *is_https = 1;
        *port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }
    
    // Extract host and port
    const char* host_start = p;
    const char* port_sep = strchr(p, ':');
    const char* path_sep = strchr(p, '/');
    
    if (path_sep == NULL) {
        path_sep = p + strlen(p);
    }
    
    if (port_sep != NULL && port_sep < path_sep) {
        // Port specified
        int host_len = (int)(port_sep - host_start);
        if (host_len <= 0 || host_len >= 256) return -1;
        if (!httpx_host_is_safe(host_start, (size_t)host_len)) return -1;
        memcpy(host, host_start, host_len);
        host[host_len] = '\0';

        const char* port_start = port_sep + 1;
        if (port_start >= path_sep) return -1;
        char* end_ptr = NULL;
        long parsed = strtol(port_start, &end_ptr, 10);
        if (end_ptr != path_sep || parsed < 1 || parsed > 65535) return -1;
        *port = (int)parsed;
    } else {
        // No port specified
        int host_len = (int)(path_sep - host_start);
        if (host_len <= 0 || host_len >= 256) return -1;
        if (!httpx_host_is_safe(host_start, (size_t)host_len)) return -1;
        memcpy(host, host_start, host_len);
        host[host_len] = '\0';
    }
    
    // Extract path
    if (*path_sep == '/') {
        size_t path_len = strlen(path_sep);
        if (path_len > 1023) path_len = 1023;
        memcpy(path, path_sep, path_len);
        path[path_len] = '\0';
        if (!httpx_path_is_safe(path)) return -1;
    } else {
        path[0] = '/';
        path[1] = '\0';
    }
    
    return 0;
}

static void http_legacy_request(VM* vm,
                                Value* slot,
                                const char* method,
                                const char* url,
                                const char* body,
                                ObjMap* headers);

static int parse_http_status_code(const char* response) {
    if (!response) return -1;
    const char* p = strchr(response, ' ');
    if (!p) return -1;
    while (*p == ' ') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    int code = 0;
    for (int i = 0; i < 3 && isdigit((unsigned char)p[i]); i++) {
        code = code * 10 + (p[i] - '0');
    }
    if (code < 100 || code > 999) return -1;
    return code;
}

// HTTP GET implementation
void builtin_http_get(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value url_val = *slot;
    ObjString* url_str = value_get_string_obj(&url_val);
    
    if (value_get_type(&url_val) != VAL_STRING || !url_str || !url_str->chars) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "httpGet expects string argument");
        return;
    }
    http_legacy_request(vm,
                        slot,
                        "GET",
                        url_str->chars,
                        NULL,
                        NULL);
}

// HTTP GET with headers
void builtin_http_get_with_headers(VM* vm) {
    // arg0 (url) at count-2, arg1 (headers map) at count-1
    Value* url_slot = &vm->stack.values[vm->stack.count - 2];
    Value headers_val = vm->stack.values[vm->stack.count - 1];
    Value url_val = *url_slot;
    ObjString* url_str = value_get_string_obj(&url_val);
    ObjMap* headers_map = value_get_map_obj(&headers_val);
    
    if (value_get_type(&url_val) != VAL_STRING || !url_str || !url_str->chars ||
        value_get_type(&headers_val) != VAL_MAP || !headers_map) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, url_slot, ok, ERR_INVALID_ARGUMENT, "httpGetWithHeaders expects (string, map<string, string>)");
        return;
    }

    http_legacy_request(vm,
                        url_slot,
                        "GET",
                        url_str->chars,
                        NULL,
                        headers_map);
}

// HTTP POST
void builtin_http_post(VM* vm) {
    // arg0 (url) at count-2, arg1 (body) at count-1
    Value* url_slot = &vm->stack.values[vm->stack.count - 2];
    Value body_val = vm->stack.values[vm->stack.count - 1];
    Value url_val = *url_slot;
    ObjString* url_str = value_get_string_obj(&url_val);
    ObjString* body_str = value_get_string_obj(&body_val);
    
    if (value_get_type(&url_val) != VAL_STRING || !url_str || !url_str->chars ||
        value_get_type(&body_val) != VAL_STRING || !body_str || !body_str->chars) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, url_slot, ok, ERR_INVALID_ARGUMENT, "httpPost expects (string, string)");
        return;
    }

    http_legacy_request(vm,
                        url_slot,
                        "POST",
                        url_str->chars,
                        body_str->chars,
                        NULL);
}

// HTTP POST with headers
void builtin_http_post_with_headers(VM* vm) {
    // arg0 (url) at count-3, arg1 (body) at count-2, arg2 (headers map) at count-1
    Value* url_slot = &vm->stack.values[vm->stack.count - 3];
    Value body_val = vm->stack.values[vm->stack.count - 2];
    Value headers_val = vm->stack.values[vm->stack.count - 1];
    Value url_val = *url_slot;
    ObjString* url_str = value_get_string_obj(&url_val);
    ObjString* body_str = value_get_string_obj(&body_val);
    ObjMap* headers_map = value_get_map_obj(&headers_val);
    
    if (value_get_type(&url_val) != VAL_STRING || !url_str || !url_str->chars ||
        value_get_type(&body_val) != VAL_STRING || !body_str || !body_str->chars ||
        value_get_type(&headers_val) != VAL_MAP || !headers_map) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, url_slot, ok, ERR_INVALID_ARGUMENT, "httpPostWithHeaders expects (string, string, map<string, string>)");
        return;
    }

    http_legacy_request(vm,
                        url_slot,
                        "POST",
                        url_str->chars,
                        body_str->chars,
                        headers_map);
}

static bool text_equals_ignore_case(const char* a, const char* b);
static bool parse_size_t_decimal(const char* start, const char* end, size_t* out);

enum {
    HTTPX_MIN_TIMEOUT_MS = 1,
    HTTPX_MAX_TIMEOUT_MS = 120000,
    HTTPX_RESPONSE_HEADER_SLACK = 64 * 1024,
    HTTPX_DEFAULT_MAX_BODY_BYTES = 8 * 1024 * 1024,
    HTTPX_MAX_CHUNK_HEADER_BYTES = 4096,
    HTTPX_POOL_MAX_IDLE_SOCKETS = 16
};

typedef struct {
    int status_code;
    char* body;
    size_t body_length;
    ObjMap* headers;
} HttpxResponse;

typedef struct {
    VM* owner_vm;
    char host[256];
    int port;
    bool is_https;
    bool tls_insecure_skip_verify;
    ObjSocket* socket_obj;
    uint64_t last_used_ms;
} HttpxPoolEntry;

static syncx_mutex_t g_httpx_pool_mutex;
static bool g_httpx_pool_initialized = false;
static HttpxPoolEntry g_httpx_pool_entries[HTTPX_POOL_MAX_IDLE_SOCKETS];

static bool httpx_parse_response(VM* vm,
                                 const char* response,
                                 size_t response_len,
                                 HttpxResponse* out,
                                 int64_t* err_code,
                                 const char** err_msg);

static void httpx_response_init(HttpxResponse* out) {
    if (!out) return;
    out->status_code = 0;
    out->body = NULL;
    out->body_length = 0;
    out->headers = NULL;
}

static void httpx_response_free(HttpxResponse* out) {
    if (!out) return;
    if (out->body) free(out->body);
    if (out->headers) obj_map_free(out->headers);
    out->body = NULL;
    out->body_length = 0;
    out->headers = NULL;
    out->status_code = 0;
}

static bool httpx_method_valid(const char* method) {
    if (!method || method[0] == '\0') return false;
    for (int i = 0; method[i] != '\0'; i++) {
        if (!isalpha((unsigned char)method[i])) return false;
    }
    return true;
}

static bool httpx_span_equals_ignore_case(const char* start, const char* end, const char* token) {
    if (!start || !end || !token || start > end) return false;
    size_t span_len = (size_t)(end - start);
    size_t token_len = strlen(token);
    if (span_len != token_len) return false;
    for (size_t i = 0; i < span_len; i++) {
        if (tolower((unsigned char)start[i]) != tolower((unsigned char)token[i])) {
            return false;
        }
    }
    return true;
}

static const char* httpx_map_get_header_value_ci(ObjMap* headers, const char* key) {
    if (!headers || !headers->slots || !key || key[0] == '\0') return NULL;
    for (int i = 0; i < headers->capacity; i++) {
        MapSlot* slot = &headers->slots[i];
        if (slot->hash < 2) continue;
        ObjString* key_str = value_get_string_obj(&slot->key);
        ObjString* value_str = value_get_string_obj(&slot->value);
        if (value_get_type(&slot->key) != VAL_STRING ||
            value_get_type(&slot->value) != VAL_STRING ||
            !key_str ||
            !value_str) {
            continue;
        }
        if (text_equals_ignore_case(key_str->chars, key)) {
            return value_str->chars;
        }
    }
    return NULL;
}

static bool httpx_header_value_has_token(const char* value, const char* token) {
    if (!value || !token || token[0] == '\0') return false;
    const char* cursor = value;
    while (*cursor != '\0') {
        while (*cursor != '\0' && (*cursor == ',' || isspace((unsigned char)*cursor))) cursor++;
        const char* start = cursor;
        while (*cursor != '\0' && *cursor != ',') cursor++;
        const char* end = cursor;
        while (start < end && isspace((unsigned char)*start)) start++;
        while (end > start && isspace((unsigned char)*(end - 1))) end--;
        if (httpx_span_equals_ignore_case(start, end, token)) return true;
        if (*cursor == ',') cursor++;
    }
    return false;
}

static bool httpx_headers_have_name_ci(ObjMap* headers, const char* key) {
    return httpx_map_get_header_value_ci(headers, key) != NULL;
}

static bool httpx_header_value_single_token_ci(const char* value, const char* token) {
    if (!value || !token || token[0] == '\0') return false;

    const char* cursor = value;
    const char* matched_start = NULL;
    const char* matched_end = NULL;
    int token_count = 0;
    while (*cursor != '\0') {
        while (*cursor != '\0' && (*cursor == ',' || isspace((unsigned char)*cursor))) cursor++;
        const char* start = cursor;
        while (*cursor != '\0' && *cursor != ',') cursor++;
        const char* end = cursor;
        while (start < end && isspace((unsigned char)*start)) start++;
        while (end > start && isspace((unsigned char)*(end - 1))) end--;
        if (end > start) {
            token_count++;
            matched_start = start;
            matched_end = end;
        }
        if (*cursor == ',') cursor++;
    }

    if (token_count != 1) return false;
    return httpx_span_equals_ignore_case(matched_start, matched_end, token);
}

static bool httpx_headers_connection_close(ObjMap* headers) {
    const char* value = httpx_map_get_header_value_ci(headers, "Connection");
    return httpx_header_value_has_token(value, "close");
}

static bool httpx_headers_transfer_chunked(ObjMap* headers) {
    const char* value = httpx_map_get_header_value_ci(headers, "Transfer-Encoding");
    return httpx_header_value_has_token(value, "chunked");
}

static bool httpx_headers_content_encoding_gzip_only(ObjMap* headers) {
    const char* value = httpx_map_get_header_value_ci(headers, "Content-Encoding");
    return httpx_header_value_single_token_ci(value, "gzip");
}

static bool httpx_headers_content_encoding_mentions_gzip(ObjMap* headers) {
    const char* value = httpx_map_get_header_value_ci(headers, "Content-Encoding");
    return httpx_header_value_has_token(value, "gzip");
}

static void httpx_headers_delete_name_ci(ObjMap* headers, const char* key) {
    if (!headers || !headers->slots || !key || key[0] == '\0') return;
    ObjString** matches = (ObjString**)safe_malloc((size_t)headers->capacity * sizeof(ObjString*));
    int match_count = 0;
    for (int i = 0; i < headers->capacity; i++) {
        MapSlot* slot = &headers->slots[i];
        if (slot->hash < 2) continue;
        ObjString* key_str = value_get_string_obj(&slot->key);
        if (value_get_type(&slot->key) != VAL_STRING || !key_str) continue;
        if (text_equals_ignore_case(key_str->chars, key)) {
            matches[match_count++] = key_str;
        }
    }
    for (int i = 0; i < match_count; i++) {
        obj_map_delete_string(headers, matches[i]);
    }
    free(matches);
}

static void httpx_headers_remove_content_encoding_metadata(ObjMap* headers) {
    if (!headers) return;
    httpx_headers_delete_name_ci(headers, "Content-Encoding");
    httpx_headers_delete_name_ci(headers, "Content-Length");
}

static bool httpx_response_status_has_no_body(const char* method, int status_code) {
    if (method && text_equals_ignore_case(method, "HEAD")) return true;
    if (status_code >= 100 && status_code < 200) return true;
    if (status_code == 204 || status_code == 304) return true;
    return false;
}

static size_t httpx_response_body_limit(VM* vm) {
    size_t max_body = HTTPX_DEFAULT_MAX_BODY_BYTES;
    if (vm && vm->config.max_string_length > 0) {
        int64_t limit = (int64_t)vm->config.max_string_length;
        if (limit < 0) limit = 0;
        max_body = (size_t)limit;
    }
    return max_body;
}

static void httpx_pool_ensure_initialized(void) {
    if (g_httpx_pool_initialized) return;
    syncx_runtime_ensure_init();
    syncx_mutex_lock(&g_syncx_registry_mutex);
    if (!g_httpx_pool_initialized) {
        syncx_mutex_init(&g_httpx_pool_mutex);
        memset(g_httpx_pool_entries, 0, sizeof(g_httpx_pool_entries));
        g_httpx_pool_initialized = true;
    }
    syncx_mutex_unlock(&g_syncx_registry_mutex);
}

static bool httpx_pool_entry_matches(const HttpxPoolEntry* entry,
                                     VM* vm,
                                     const char* host,
                                     int port,
                                     bool is_https,
                                     bool tls_insecure_skip_verify) {
    if (!entry || !entry->socket_obj || !host) return false;
    if (entry->owner_vm != vm) return false;
    if (entry->port != port) return false;
    if (entry->is_https != is_https) return false;
    if (entry->tls_insecure_skip_verify != tls_insecure_skip_verify) return false;
    return text_equals_ignore_case(entry->host, host);
}

static ObjSocket* httpx_pool_take_socket(VM* vm,
                                         const char* host,
                                         int port,
                                         bool is_https,
                                         bool tls_insecure_skip_verify,
                                         int timeout_ms) {
    if (!vm || !host || host[0] == '\0') return NULL;
    httpx_pool_ensure_initialized();

    ObjSocket* socket_obj = NULL;
    syncx_mutex_lock(&g_httpx_pool_mutex);
    for (int i = 0; i < HTTPX_POOL_MAX_IDLE_SOCKETS; i++) {
        HttpxPoolEntry* entry = &g_httpx_pool_entries[i];
        if (!httpx_pool_entry_matches(entry,
                                      vm,
                                      host,
                                      port,
                                      is_https,
                                      tls_insecure_skip_verify)) {
            continue;
        }
        socket_obj = entry->socket_obj;
        entry->socket_obj = NULL;
        entry->last_used_ms = syncx_now_ms();
        break;
    }
    syncx_mutex_unlock(&g_httpx_pool_mutex);

    if (!socket_obj) return NULL;
    if (socket_obj->socket_fd < 0 || !socket_obj->is_connected) {
        obj_socket_free(socket_obj);
        return NULL;
    }
    if (!vm_try_acquire_socket_handle(vm)) {
        obj_socket_free(socket_obj);
        return NULL;
    }
    socket_obj->owner_vm = vm;
    socket_obj->limit_tracked = true;

    const char* timeout_err = NULL;
    if (!tcpx_set_socket_timeouts(socket_obj->socket_fd, timeout_ms, &timeout_err)) {
        obj_socket_free(socket_obj);
        return NULL;
    }

    return socket_obj;
}

static void httpx_pool_store_socket(VM* vm,
                                    const char* host,
                                    int port,
                                    bool is_https,
                                    bool tls_insecure_skip_verify,
                                    ObjSocket* socket_obj) {
    if (!socket_obj) return;
    if (!vm || !host || host[0] == '\0' || socket_obj->socket_fd < 0 || !socket_obj->is_connected) {
        obj_socket_free(socket_obj);
        return;
    }

    if (socket_obj->limit_tracked) {
        vm_release_socket_handle((VM*)socket_obj->owner_vm);
        socket_obj->limit_tracked = false;
    }
    socket_obj->owner_vm = NULL;

    httpx_pool_ensure_initialized();

    int target_index = -1;
    int empty_index = -1;
    int oldest_index = -1;
    uint64_t oldest_used = 0;

    ObjSocket* evicted = NULL;
    syncx_mutex_lock(&g_httpx_pool_mutex);
    for (int i = 0; i < HTTPX_POOL_MAX_IDLE_SOCKETS; i++) {
        HttpxPoolEntry* entry = &g_httpx_pool_entries[i];
        if (httpx_pool_entry_matches(entry,
                                     vm,
                                     host,
                                     port,
                                     is_https,
                                     tls_insecure_skip_verify)) {
            target_index = i;
            break;
        }
        if (!entry->socket_obj) {
            if (empty_index < 0) empty_index = i;
            continue;
        }
        if (oldest_index < 0 || entry->last_used_ms < oldest_used) {
            oldest_index = i;
            oldest_used = entry->last_used_ms;
        }
    }

    if (target_index < 0) {
        if (empty_index >= 0) {
            target_index = empty_index;
        } else {
            target_index = oldest_index;
        }
    }

    if (target_index >= 0) {
        HttpxPoolEntry* target = &g_httpx_pool_entries[target_index];
        evicted = target->socket_obj;
        target->owner_vm = vm;
        target->port = port;
        target->is_https = is_https;
        target->tls_insecure_skip_verify = tls_insecure_skip_verify;
        size_t host_len = strlen(host);
        if (host_len >= sizeof(target->host)) host_len = sizeof(target->host) - 1;
        memcpy(target->host, host, host_len);
        target->host[host_len] = '\0';
        target->socket_obj = socket_obj;
        target->last_used_ms = syncx_now_ms();
    }
    syncx_mutex_unlock(&g_httpx_pool_mutex);

    if (evicted && evicted != socket_obj) {
        obj_socket_free(evicted);
    }
    if (target_index < 0) {
        obj_socket_free(socket_obj);
    }
}

static bool httpx_appendf(char* dst, size_t cap, size_t* pos, const char* fmt, ...) {
    if (!dst || !pos || !fmt) return false;
    if (*pos > cap) return false;

    va_list args;
    va_start(args, fmt);
    int wrote = vsnprintf(dst + *pos, cap - *pos + 1, fmt, args);
    va_end(args);
    if (wrote < 0) return false;
    if ((size_t)wrote > cap - *pos) return false;

    *pos += (size_t)wrote;
    return true;
}

static size_t httpx_headers_wire_len(ObjMap* headers) {
    if (!headers || !headers->slots) return 0;
    size_t total = 0;
    for (int i = 0; i < headers->capacity; i++) {
        MapSlot* slot = &headers->slots[i];
        if (slot->hash < 2) continue;
        ObjString* key_str = value_get_string_obj(&slot->key);
        ObjString* value_str = value_get_string_obj(&slot->value);
        if (value_get_type(&slot->key) != VAL_STRING || value_get_type(&slot->value) != VAL_STRING) continue;
        if (!key_str || !value_str) continue;
        total += (size_t)key_str->length + 2 + (size_t)value_str->length + 2;
    }
    return total;
}

static char* httpx_build_request(const char* method,
                                 const char* path,
                                 const char* host,
                                 const char* body,
                                 ObjMap* headers,
                                 bool keep_alive,
                                 bool accept_gzip,
                                 size_t* out_len) {
    if (out_len) *out_len = 0;
    if (!method || !path || !host) return NULL;
    if (!httpx_path_is_safe(path)) return NULL;
    if (!httpx_host_is_safe(host, strlen(host))) return NULL;

    size_t method_len = strlen(method);
    size_t path_len = strlen(path);
    size_t host_len = strlen(host);
    size_t body_len = body ? strlen(body) : 0;
    size_t headers_len = httpx_headers_wire_len(headers);
    bool has_connection_header = httpx_headers_have_name_ci(headers, "Connection");
    bool has_accept_encoding_header = httpx_headers_have_name_ci(headers, "Accept-Encoding");

    size_t total = 0;
    total += method_len + 1 + path_len + sizeof(" HTTP/1.1\r\n") - 1;
    total += sizeof("Host: ") - 1 + host_len + 2;
    total += sizeof("User-Agent: TabloLang/1.0\r\n") - 1;
    if (!has_connection_header) {
        if (keep_alive) {
            total += sizeof("Connection: keep-alive\r\n") - 1;
        } else {
            total += sizeof("Connection: close\r\n") - 1;
        }
    }
    if (accept_gzip && !has_accept_encoding_header) {
        total += sizeof("Accept-Encoding: gzip\r\n") - 1;
    }
    if (body_len > 0) {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%zu", body_len);
        if (n <= 0) return NULL;
        total += sizeof("Content-Length: ") - 1 + (size_t)n + 2;
    }
    total += headers_len;
    total += 2;
    total += body_len;

    char* req = (char*)safe_malloc(total + 1);
    size_t pos = 0;

    if (!httpx_appendf(req, total, &pos, "%s %s HTTP/1.1\r\n", method, path) ||
        !httpx_appendf(req, total, &pos, "Host: %s\r\n", host) ||
        !httpx_appendf(req, total, &pos, "User-Agent: TabloLang/1.0\r\n")) {
        free(req);
        return NULL;
    }

    if (!has_connection_header) {
        const char* connection_value = keep_alive ? "keep-alive" : "close";
        if (!httpx_appendf(req, total, &pos, "Connection: %s\r\n", connection_value)) {
            free(req);
            return NULL;
        }
    }

    if (accept_gzip && !has_accept_encoding_header) {
        if (!httpx_appendf(req, total, &pos, "Accept-Encoding: gzip\r\n")) {
            free(req);
            return NULL;
        }
    }

    if (body_len > 0) {
        if (!httpx_appendf(req, total, &pos, "Content-Length: %zu\r\n", body_len)) {
            free(req);
            return NULL;
        }
    }

    if (headers && headers->slots) {
        for (int i = 0; i < headers->capacity; i++) {
            MapSlot* slot = &headers->slots[i];
            if (slot->hash < 2) continue;
            ObjString* key_str = value_get_string_obj(&slot->key);
            ObjString* value_str = value_get_string_obj(&slot->value);
            if (value_get_type(&slot->key) != VAL_STRING || value_get_type(&slot->value) != VAL_STRING) continue;
            if (!key_str || !value_str) continue;
            if (!httpx_header_name_is_safe(key_str->chars)) {
                free(req);
                return NULL;
            }
            if (!httpx_header_value_is_safe(value_str->chars)) {
                free(req);
                return NULL;
            }
            if (!httpx_appendf(req,
                               total,
                               &pos,
                               "%s: %s\r\n",
                               key_str->chars,
                               value_str->chars)) {
                free(req);
                return NULL;
            }
        }
    }

    if (!httpx_appendf(req, total, &pos, "\r\n")) {
        free(req);
        return NULL;
    }

    if (body_len > 0) {
        if (pos + body_len > total) {
            free(req);
            return NULL;
        }
        memcpy(req + pos, body, body_len);
        pos += body_len;
    }

    req[pos] = '\0';
    if (out_len) *out_len = pos;
    return req;
}

static bool httpx_send_all(int sock, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t remain = len - sent;
        int chunk = remain > (size_t)INT_MAX ? INT_MAX : (int)remain;
        int n = send(sock, data + sent, chunk, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool httpx_send_all_socket(ObjSocket* socket_obj, const char* data, size_t len) {
    if (!socket_obj || socket_obj->socket_fd < 0) return false;
    size_t sent = 0;
    while (sent < len) {
        size_t remain = len - sent;
        int chunk = remain > (size_t)INT_MAX ? INT_MAX : (int)remain;
        int n = socket_send_dispatch(socket_obj, data + sent, chunk);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool httpx_parse_content_length_header(ObjMap* headers,
                                              int64_t* out_content_length,
                                              int64_t* err_code,
                                              const char** err_msg) {
    if (out_content_length) *out_content_length = -1;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!headers || !headers->slots) return true;

    bool seen = false;
    size_t content_length = 0;
    for (int i = 0; i < headers->capacity; i++) {
        MapSlot* slot = &headers->slots[i];
        if (slot->hash < 2) continue;
        ObjString* key_str = value_get_string_obj(&slot->key);
        ObjString* value_str = value_get_string_obj(&slot->value);
        if (value_get_type(&slot->key) != VAL_STRING ||
            value_get_type(&slot->value) != VAL_STRING ||
            !key_str ||
            !value_str) {
            continue;
        }

        const char* key = key_str->chars;
        if (!text_equals_ignore_case(key, "Content-Length")) {
            continue;
        }

        const char* value = value_str->chars;
        const char* start = value;
        const char* end = value + strlen(value);
        while (start < end && isspace((unsigned char)*start)) start++;
        while (end > start && isspace((unsigned char)*(end - 1))) end--;

        size_t parsed = 0;
        if (!parse_size_t_decimal(start, end, &parsed)) {
            if (err_code) *err_code = ERR_PARSE;
            if (err_msg) *err_msg = "Invalid Content-Length header";
            return false;
        }

        if (seen && parsed != content_length) {
            if (err_code) *err_code = ERR_PARSE;
            if (err_msg) *err_msg = "Conflicting Content-Length headers";
            return false;
        }

        seen = true;
        content_length = parsed;
    }

    if (seen) {
        if (content_length > (size_t)INT64_MAX) {
            if (err_code) *err_code = ERR_LIMIT;
            if (err_msg) *err_msg = "Content-Length exceeds supported size";
            return false;
        }
        if (out_content_length) *out_content_length = (int64_t)content_length;
    }

    return true;
}

static bool httpx_parse_size_t_hex(const char* start, const char* end, size_t* out) {
    if (out) *out = 0;
    if (!start || !end || !out || start >= end) return false;

    size_t value = 0;
    for (const char* p = start; p < end; p++) {
        unsigned char ch = (unsigned char)*p;
        size_t digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = (size_t)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = 10u + (size_t)(ch - 'a');
        } else if (ch >= 'A' && ch <= 'F') {
            digit = 10u + (size_t)(ch - 'A');
        } else {
            return false;
        }
        if (value > (SIZE_MAX - digit) / 16u) return false;
        value = value * 16u + digit;
    }

    *out = value;
    return true;
}

static bool httpx_recv_exact_socket(VM* vm,
                                    ObjSocket* socket_obj,
                                    size_t expected_len,
                                    char** out_body,
                                    size_t* out_len,
                                    int64_t* err_code,
                                    const char** err_msg) {
    if (out_body) *out_body = NULL;
    if (out_len) *out_len = 0;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!out_body || !out_len) return false;
    if (!socket_obj || socket_obj->socket_fd < 0) {
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Socket is closed";
        return false;
    }

    size_t max_body = httpx_response_body_limit(vm);
    if (expected_len > max_body) {
        if (err_code) *err_code = ERR_LIMIT;
        if (err_msg) *err_msg = "HTTP body exceeds max string length";
        return false;
    }

    char* body = (char*)safe_malloc(expected_len + 1);
    size_t received = 0;
    while (received < expected_len) {
        size_t remaining = expected_len - received;
        int want = remaining > 4096 ? 4096 : (int)remaining;
        int n = socket_recv_dispatch(socket_obj, body + received, want);
        if (n == 0) {
            free(body);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Connection closed before full response body";
            return false;
        }
        if (n < 0) {
            free(body);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Failed to receive response body";
            return false;
        }
        received += (size_t)n;
    }

    body[received] = '\0';
    *out_body = body;
    *out_len = received;
    return true;
}

static bool httpx_recv_body_until_close_socket(VM* vm,
                                               ObjSocket* socket_obj,
                                               char** out_body,
                                               size_t* out_len,
                                               int64_t* err_code,
                                               const char** err_msg) {
    if (out_body) *out_body = NULL;
    if (out_len) *out_len = 0;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!out_body || !out_len) return false;
    if (!socket_obj || socket_obj->socket_fd < 0) {
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Socket is closed";
        return false;
    }

    size_t max_body = httpx_response_body_limit(vm);
    size_t cap = max_body > 0 ? (max_body < 4096 ? max_body : 4096) : 0;
    char* body = (char*)safe_malloc(cap + 1);
    size_t len = 0;

    char chunk[4096];
    while (true) {
        int n = socket_recv_dispatch(socket_obj, chunk, (int)sizeof(chunk));
        if (n > 0) {
            if ((size_t)n > max_body - len) {
                free(body);
                if (err_code) *err_code = ERR_LIMIT;
                if (err_msg) *err_msg = "HTTP body exceeds max string length";
                return false;
            }

            if (len + (size_t)n + 1 > cap) {
                size_t next_cap = cap > 0 ? cap : 1;
                while (len + (size_t)n + 1 > next_cap) {
                    size_t grown = next_cap * 2;
                    if (grown <= next_cap || grown > max_body) {
                        grown = max_body;
                    }
                    next_cap = grown;
                    if (len + (size_t)n + 1 <= next_cap) break;
                    if (next_cap >= max_body) break;
                }
                if (len + (size_t)n + 1 > next_cap) {
                    free(body);
                    if (err_code) *err_code = ERR_LIMIT;
                    if (err_msg) *err_msg = "HTTP body exceeds max string length";
                    return false;
                }
                cap = next_cap;
                body = (char*)safe_realloc(body, cap + 1);
            }

            memcpy(body + len, chunk, (size_t)n);
            len += (size_t)n;
        } else if (n == 0) {
            break;
        } else {
            free(body);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Failed to receive response body";
            return false;
        }
    }

    body[len] = '\0';
    *out_body = body;
    *out_len = len;
    return true;
}

typedef int (*HttpxRecvFn)(void* ctx, char* out, int max_len);

static int httpx_socket_recv_adapter(void* ctx, char* out, int max_len) {
    return socket_recv_dispatch((ObjSocket*)ctx, out, max_len);
}

typedef struct HttpxFuzzBufferReader {
    const char* data;
    size_t len;
    size_t pos;
} HttpxFuzzBufferReader;

static int httpx_fuzz_buffer_recv_adapter(void* ctx, char* out, int max_len) {
    HttpxFuzzBufferReader* reader = (HttpxFuzzBufferReader*)ctx;
    size_t remaining = 0;
    size_t to_copy = 0;

    if (!reader || !out || max_len <= 0) {
        return -1;
    }
    if (reader->pos >= reader->len) {
        return 0;
    }

    remaining = reader->len - reader->pos;
    to_copy = remaining < (size_t)max_len ? remaining : (size_t)max_len;
    memcpy(out, reader->data + reader->pos, to_copy);
    reader->pos += to_copy;
    return (int)to_copy;
}

static bool httpx_read_line_reader(HttpxRecvFn recv_fn,
                                   void* recv_ctx,
                                   size_t max_line_bytes,
                                   char** out_line,
                                   size_t* out_line_len,
                                   int64_t* err_code,
                                   const char** err_msg) {
    if (out_line) *out_line = NULL;
    if (out_line_len) *out_line_len = 0;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!out_line || !recv_fn) return false;
    if (!recv_ctx) {
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "HTTP reader is closed";
        return false;
    }

    if (max_line_bytes == 0) max_line_bytes = HTTPX_MAX_CHUNK_HEADER_BYTES;
    size_t cap = max_line_bytes < 128 ? max_line_bytes : 128;
    if (cap == 0) cap = 1;
    char* line = (char*)safe_malloc(cap + 1);
    size_t len = 0;

    while (true) {
        char c = '\0';
        int n = recv_fn(recv_ctx, &c, 1);
        if (n == 0) {
            free(line);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Connection closed while reading chunk header";
            return false;
        }
        if (n < 0) {
            free(line);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Failed to receive chunk header";
            return false;
        }

        if (len + 2 > cap) {
            size_t next_cap = cap;
            while (len + 2 > next_cap && next_cap < max_line_bytes) {
                size_t grown = next_cap * 2;
                if (grown <= next_cap || grown > max_line_bytes) {
                    grown = max_line_bytes;
                }
                next_cap = grown;
                if (next_cap >= max_line_bytes) break;
            }
            if (len + 2 > next_cap) {
                free(line);
                if (err_code) *err_code = ERR_LIMIT;
                if (err_msg) *err_msg = "HTTP chunk header exceeds max size";
                return false;
            }
            cap = next_cap;
            line = (char*)safe_realloc(line, cap + 1);
        }

        line[len++] = c;
        if (c == '\n') break;
    }

    size_t logical_len = len;
    if (logical_len > 0 && line[logical_len - 1] == '\n') logical_len--;
    if (logical_len > 0 && line[logical_len - 1] == '\r') logical_len--;
    line[logical_len] = '\0';

    *out_line = line;
    if (out_line_len) *out_line_len = logical_len;
    return true;
}

static bool httpx_read_line_socket(ObjSocket* socket_obj,
                                   size_t max_line_bytes,
                                   char** out_line,
                                   size_t* out_line_len,
                                   int64_t* err_code,
                                   const char** err_msg) {
    if (!socket_obj || socket_obj->socket_fd < 0) {
        if (out_line) *out_line = NULL;
        if (out_line_len) *out_line_len = 0;
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Socket is closed";
        return false;
    }

    return httpx_read_line_reader(httpx_socket_recv_adapter,
                                  socket_obj,
                                  max_line_bytes,
                                  out_line,
                                  out_line_len,
                                  err_code,
                                  err_msg);
}

static bool httpx_consume_chunk_terminator_reader(HttpxRecvFn recv_fn,
                                                  void* recv_ctx,
                                                  int64_t* err_code,
                                                  const char** err_msg) {
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!recv_fn || !recv_ctx) {
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "HTTP reader is closed";
        return false;
    }

    char c = '\0';
    int n = recv_fn(recv_ctx, &c, 1);
    if (n == 0) {
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Connection closed while reading chunk terminator";
        return false;
    }
    if (n < 0) {
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Failed to receive chunk terminator";
        return false;
    }

    if (c == '\n') return true;
    if (c != '\r') {
        if (err_code) *err_code = ERR_PARSE;
        if (err_msg) *err_msg = "Malformed chunked response";
        return false;
    }

    n = recv_fn(recv_ctx, &c, 1);
    if (n == 0) {
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Connection closed while reading chunk terminator";
        return false;
    }
    if (n < 0) {
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Failed to receive chunk terminator";
        return false;
    }
    if (c != '\n') {
        if (err_code) *err_code = ERR_PARSE;
        if (err_msg) *err_msg = "Malformed chunked response";
        return false;
    }
    return true;
}

static bool httpx_consume_chunk_terminator(ObjSocket* socket_obj,
                                           int64_t* err_code,
                                           const char** err_msg) {
    if (!socket_obj || socket_obj->socket_fd < 0) {
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Socket is closed";
        return false;
    }

    return httpx_consume_chunk_terminator_reader(httpx_socket_recv_adapter,
                                                 socket_obj,
                                                 err_code,
                                                 err_msg);
}

static bool httpx_recv_chunked_body_reader(VM* vm,
                                           HttpxRecvFn recv_fn,
                                           void* recv_ctx,
                                           char** out_body,
                                           size_t* out_len,
                                           int64_t* err_code,
                                           const char** err_msg) {
    if (out_body) *out_body = NULL;
    if (out_len) *out_len = 0;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!out_body || !out_len || !recv_fn) return false;
    if (!recv_ctx) {
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "HTTP reader is closed";
        return false;
    }

    size_t max_body = httpx_response_body_limit(vm);
    size_t cap = max_body > 0 ? (max_body < 4096 ? max_body : 4096) : 0;
    char* body = (char*)safe_malloc(cap + 1);
    size_t len = 0;

    while (true) {
        char* chunk_header = NULL;
        size_t chunk_header_len = 0;
        if (!httpx_read_line_reader(recv_fn,
                                    recv_ctx,
                                    HTTPX_MAX_CHUNK_HEADER_BYTES,
                                    &chunk_header,
                                    &chunk_header_len,
                                    err_code,
                                    err_msg)) {
            free(body);
            return false;
        }

        const char* size_start = chunk_header;
        const char* size_end = chunk_header + chunk_header_len;
        while (size_start < size_end && isspace((unsigned char)*size_start)) size_start++;
        const char* ext = memchr(size_start, ';', (size_t)(size_end - size_start));
        if (ext) size_end = ext;
        while (size_end > size_start && isspace((unsigned char)*(size_end - 1))) size_end--;

        size_t chunk_size = 0;
        if (!httpx_parse_size_t_hex(size_start, size_end, &chunk_size)) {
            free(chunk_header);
            free(body);
            if (err_code) *err_code = ERR_PARSE;
            if (err_msg) *err_msg = "Malformed chunked response";
            return false;
        }
        free(chunk_header);

        if (chunk_size == 0) {
            while (true) {
                char* trailer_line = NULL;
                size_t trailer_len = 0;
                if (!httpx_read_line_reader(recv_fn,
                                            recv_ctx,
                                            HTTPX_MAX_CHUNK_HEADER_BYTES,
                                            &trailer_line,
                                            &trailer_len,
                                            err_code,
                                            err_msg)) {
                    free(body);
                    return false;
                }
                bool done = trailer_len == 0;
                free(trailer_line);
                if (done) break;
            }
            break;
        }

        if (chunk_size > max_body - len) {
            free(body);
            if (err_code) *err_code = ERR_LIMIT;
            if (err_msg) *err_msg = "HTTP body exceeds max string length";
            return false;
        }

        if (len + chunk_size + 1 > cap) {
            size_t next_cap = cap > 0 ? cap : 1;
            while (len + chunk_size + 1 > next_cap) {
                size_t grown = next_cap * 2;
                if (grown <= next_cap || grown > max_body) {
                    grown = max_body;
                }
                next_cap = grown;
                if (len + chunk_size + 1 <= next_cap) break;
                if (next_cap >= max_body) break;
            }
            if (len + chunk_size + 1 > next_cap) {
                free(body);
                if (err_code) *err_code = ERR_LIMIT;
                if (err_msg) *err_msg = "HTTP body exceeds max string length";
                return false;
            }
            cap = next_cap;
            body = (char*)safe_realloc(body, cap + 1);
        }

        size_t remaining = chunk_size;
        while (remaining > 0) {
            int want = remaining > 4096 ? 4096 : (int)remaining;
            int n = recv_fn(recv_ctx, body + len, want);
            if (n == 0) {
                free(body);
                if (err_code) *err_code = ERR_NETWORK;
                if (err_msg) *err_msg = "Connection closed before full response body";
                return false;
            }
            if (n < 0) {
                free(body);
                if (err_code) *err_code = ERR_NETWORK;
                if (err_msg) *err_msg = "Failed to receive response body";
                return false;
            }
            len += (size_t)n;
            remaining -= (size_t)n;
        }

        if (!httpx_consume_chunk_terminator_reader(recv_fn, recv_ctx, err_code, err_msg)) {
            free(body);
            return false;
        }
    }

    body[len] = '\0';
    *out_body = body;
    *out_len = len;
    return true;
}

static bool httpx_recv_chunked_body_socket(VM* vm,
                                           ObjSocket* socket_obj,
                                           char** out_body,
                                           size_t* out_len,
                                           int64_t* err_code,
                                           const char** err_msg) {
    if (!socket_obj || socket_obj->socket_fd < 0) {
        if (out_body) *out_body = NULL;
        if (out_len) *out_len = 0;
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Socket is closed";
        return false;
    }

    return httpx_recv_chunked_body_reader(vm,
                                          httpx_socket_recv_adapter,
                                          socket_obj,
                                          out_body,
                                          out_len,
                                          err_code,
                                          err_msg);
}

static bool httpx_maybe_decode_gzip_body(VM* vm,
                                         HttpxResponse* response,
                                         int64_t* err_code,
                                         const char** err_msg) {
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!response || !response->headers || !response->body) return true;

    if (!httpx_headers_content_encoding_mentions_gzip(response->headers)) {
        return true;
    }
    if (!httpx_headers_content_encoding_gzip_only(response->headers)) {
        if (err_code) *err_code = ERR_UNSUPPORTED;
        if (err_msg) *err_msg = "Unsupported Content-Encoding";
        return false;
    }

    size_t max_body = httpx_response_body_limit(vm);
    uint8_t* decoded = NULL;
    size_t decoded_len = 0;
    const char* gzip_err = NULL;
    if (!gzip_codec_decompress((const uint8_t*)response->body,
                               response->body_length,
                               max_body,
                               &decoded,
                               &decoded_len,
                               &gzip_err)) {
        if (decoded) free(decoded);
        if (err_code) *err_code = ERR_PARSE;
        if (err_msg) *err_msg = gzip_err ? gzip_err : "Failed to decode gzip response";
        return false;
    }
    decoded = (uint8_t*)safe_realloc(decoded, decoded_len + 1);
    decoded[decoded_len] = '\0';

    free(response->body);
    response->body = (char*)decoded;
    response->body_length = decoded_len;
    httpx_headers_remove_content_encoding_metadata(response->headers);
    return true;
}

static size_t httpx_find_header_total_len_parts(const char* prefix,
                                                size_t prefix_len,
                                                const char* chunk,
                                                size_t chunk_len) {
    size_t total = prefix_len + chunk_len;
    if (total < 2) return SIZE_MAX;

    size_t start = 0;
    if (prefix_len > 3) {
        start = prefix_len - 3;
    }

    for (size_t i = start; i + 3 < total; i++) {
        char c0 = (i < prefix_len) ? prefix[i] : chunk[i - prefix_len];
        char c1 = ((i + 1) < prefix_len) ? prefix[i + 1] : chunk[i + 1 - prefix_len];
        char c2 = ((i + 2) < prefix_len) ? prefix[i + 2] : chunk[i + 2 - prefix_len];
        char c3 = ((i + 3) < prefix_len) ? prefix[i + 3] : chunk[i + 3 - prefix_len];
        if (c0 == '\r' && c1 == '\n' && c2 == '\r' && c3 == '\n') {
            return i + 4;
        }
    }

    for (size_t i = start; i + 1 < total; i++) {
        char c0 = (i < prefix_len) ? prefix[i] : chunk[i - prefix_len];
        char c1 = ((i + 1) < prefix_len) ? prefix[i + 1] : chunk[i + 1 - prefix_len];
        if (c0 == '\n' && c1 == '\n') {
            return i + 2;
        }
    }

    return SIZE_MAX;
}

static bool httpx_consume_into_buffer(int sock,
                                      size_t consume_len,
                                      char** io_buf,
                                      size_t* io_len,
                                      size_t* io_cap,
                                      size_t max_header_bytes,
                                      int64_t* err_code,
                                      const char** err_msg) {
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!io_buf || !*io_buf || !io_len || !io_cap) return false;
    if (consume_len == 0) return true;

    size_t remaining = consume_len;
    while (remaining > 0) {
        char chunk[4096];
        int want = remaining > sizeof(chunk) ? (int)sizeof(chunk) : (int)remaining;
        int n = recv(sock, chunk, want, 0);
        if (n == 0) {
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Connection closed before response headers";
            return false;
        }
        if (n < 0) {
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Failed to receive response headers";
            return false;
        }

        if (*io_len + (size_t)n > max_header_bytes) {
            if (err_code) *err_code = ERR_LIMIT;
            if (err_msg) *err_msg = "HTTP response headers exceed max size";
            return false;
        }

        if (*io_len + (size_t)n + 1 > *io_cap) {
            size_t next_cap = *io_cap;
            if (next_cap < 256) next_cap = 256;
            while (*io_len + (size_t)n + 1 > next_cap) {
                next_cap *= 2;
                if (next_cap > max_header_bytes) {
                    next_cap = max_header_bytes;
                    break;
                }
            }
            if (*io_len + (size_t)n + 1 > next_cap) {
                if (err_code) *err_code = ERR_LIMIT;
                if (err_msg) *err_msg = "HTTP response headers exceed max size";
                return false;
            }
            *io_cap = next_cap;
            *io_buf = (char*)safe_realloc(*io_buf, *io_cap + 1);
        }

        memcpy(*io_buf + *io_len, chunk, (size_t)n);
        *io_len += (size_t)n;
        remaining -= (size_t)n;
    }

    (*io_buf)[*io_len] = '\0';
    return true;
}

static bool httpx_recv_head(VM* vm,
                            int sock,
                            HttpxResponse* out,
                            int64_t* out_content_length,
                            int64_t* err_code,
                            const char** err_msg) {
    if (out_content_length) *out_content_length = -1;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!out) return false;
    httpx_response_init(out);

    size_t max_header_bytes = 8 * 1024 * 1024;
    if (vm && vm->config.max_string_length > 0) {
        int64_t limit = (int64_t)vm->config.max_string_length + (int64_t)HTTPX_RESPONSE_HEADER_SLACK;
        if (limit < 1024) limit = 1024;
        max_header_bytes = (size_t)limit;
    }

    size_t cap = 2048;
    if (cap > max_header_bytes) cap = max_header_bytes;
    if (cap == 0) cap = 256;
    char* buf = (char*)safe_malloc(cap + 1);
    size_t len = 0;
    buf[0] = '\0';

    while (true) {
        if (len >= max_header_bytes) {
            free(buf);
            if (err_code) *err_code = ERR_LIMIT;
            if (err_msg) *err_msg = "HTTP response headers exceed max size";
            return false;
        }

        char peek_chunk[4096];
        size_t remain_limit = max_header_bytes - len;
        int peek_cap = remain_limit > sizeof(peek_chunk) ? (int)sizeof(peek_chunk) : (int)remain_limit;
        if (peek_cap < 1) peek_cap = 1;

        int n = recv(sock, peek_chunk, peek_cap, MSG_PEEK);
        if (n == 0) {
            free(buf);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Connection closed before response headers";
            return false;
        }
        if (n < 0) {
            free(buf);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Failed to receive response headers";
            return false;
        }

        size_t peek_len = (size_t)n;
        size_t total_header_len = httpx_find_header_total_len_parts(buf, len, peek_chunk, peek_len);
        size_t consume_len = peek_len;
        if (total_header_len != SIZE_MAX) {
            if (total_header_len < len) {
                free(buf);
                if (err_code) *err_code = ERR_INTERNAL;
                if (err_msg) *err_msg = "Invalid HTTP response header parsing state";
                return false;
            }
            consume_len = total_header_len - len;
        }

        int64_t consume_err_code = 0;
        const char* consume_err_msg = NULL;
        if (!httpx_consume_into_buffer(sock,
                                       consume_len,
                                       &buf,
                                       &len,
                                       &cap,
                                       max_header_bytes,
                                       &consume_err_code,
                                       &consume_err_msg)) {
            free(buf);
            if (err_code) *err_code = consume_err_code ? consume_err_code : ERR_NETWORK;
            if (err_msg) *err_msg = consume_err_msg ? consume_err_msg : "Failed to receive response headers";
            return false;
        }

        if (total_header_len != SIZE_MAX) break;
    }

    int64_t parse_err_code = 0;
    const char* parse_err_msg = NULL;
    if (!httpx_parse_response(vm, buf, len, out, &parse_err_code, &parse_err_msg)) {
        free(buf);
        if (err_code) *err_code = parse_err_code ? parse_err_code : ERR_PARSE;
        if (err_msg) *err_msg = parse_err_msg ? parse_err_msg : "Malformed HTTP response";
        return false;
    }
    free(buf);

    if (out->body) {
        free(out->body);
        out->body = NULL;
        out->body_length = 0;
    }

    int64_t content_length = -1;
    int64_t content_length_err_code = 0;
    const char* content_length_err_msg = NULL;
    if (!httpx_parse_content_length_header(out->headers,
                                           &content_length,
                                           &content_length_err_code,
                                           &content_length_err_msg)) {
        httpx_response_free(out);
        if (err_code) *err_code = content_length_err_code ? content_length_err_code : ERR_PARSE;
        if (err_msg) *err_msg = content_length_err_msg ? content_length_err_msg : "Invalid response headers";
        return false;
    }

    if (out_content_length) *out_content_length = content_length;
    return true;
}

static bool httpx_recv_head_socket(VM* vm,
                                   ObjSocket* socket_obj,
                                   HttpxResponse* out,
                                   int64_t* out_content_length,
                                   int64_t* err_code,
                                   const char** err_msg) {
    if (out_content_length) *out_content_length = -1;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!out) return false;
    httpx_response_init(out);

    if (!socket_obj || socket_obj->socket_fd < 0) {
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Socket is closed";
        return false;
    }

    if (!socket_obj->transport_recv) {
        return httpx_recv_head(vm,
                               socket_obj->socket_fd,
                               out,
                               out_content_length,
                               err_code,
                               err_msg);
    }

    size_t max_header_bytes = 8 * 1024 * 1024;
    if (vm && vm->config.max_string_length > 0) {
        int64_t limit = (int64_t)vm->config.max_string_length + (int64_t)HTTPX_RESPONSE_HEADER_SLACK;
        if (limit < 1024) limit = 1024;
        max_header_bytes = (size_t)limit;
    }

    size_t cap = 2048;
    if (cap > max_header_bytes) cap = max_header_bytes;
    if (cap == 0) cap = 256;
    char* buf = (char*)safe_malloc(cap + 1);
    size_t len = 0;
    buf[0] = '\0';

    bool found = false;
    while (!found) {
        if (len >= max_header_bytes) {
            free(buf);
            if (err_code) *err_code = ERR_LIMIT;
            if (err_msg) *err_msg = "HTTP response headers exceed max size";
            return false;
        }

        char c = '\0';
        int n = socket_recv_dispatch(socket_obj, &c, 1);
        if (n == 0) {
            free(buf);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Connection closed before response headers";
            return false;
        }
        if (n < 0) {
            free(buf);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Failed to receive response headers";
            return false;
        }

        if (len + 2 > cap) {
            size_t next_cap = cap > 0 ? cap : 256;
            while (len + 2 > next_cap) {
                next_cap *= 2;
                if (next_cap > max_header_bytes) {
                    next_cap = max_header_bytes;
                    break;
                }
            }
            if (len + 2 > next_cap) {
                free(buf);
                if (err_code) *err_code = ERR_LIMIT;
                if (err_msg) *err_msg = "HTTP response headers exceed max size";
                return false;
            }
            cap = next_cap;
            buf = (char*)safe_realloc(buf, cap + 1);
        }

        buf[len++] = c;
        buf[len] = '\0';

        if (len >= 4 &&
            buf[len - 4] == '\r' &&
            buf[len - 3] == '\n' &&
            buf[len - 2] == '\r' &&
            buf[len - 1] == '\n') {
            found = true;
            break;
        }
        if (len >= 2 &&
            buf[len - 2] == '\n' &&
            buf[len - 1] == '\n') {
            found = true;
            break;
        }
    }

    int64_t parse_err_code = 0;
    const char* parse_err_msg = NULL;
    if (!httpx_parse_response(vm, buf, len, out, &parse_err_code, &parse_err_msg)) {
        free(buf);
        if (err_code) *err_code = parse_err_code ? parse_err_code : ERR_PARSE;
        if (err_msg) *err_msg = parse_err_msg ? parse_err_msg : "Malformed HTTP response";
        return false;
    }
    free(buf);

    if (out->body) {
        free(out->body);
        out->body = NULL;
        out->body_length = 0;
    }

    int64_t content_length = -1;
    int64_t content_length_err_code = 0;
    const char* content_length_err_msg = NULL;
    if (!httpx_parse_content_length_header(out->headers,
                                           &content_length,
                                           &content_length_err_code,
                                           &content_length_err_msg)) {
        httpx_response_free(out);
        if (err_code) *err_code = content_length_err_code ? content_length_err_code : ERR_PARSE;
        if (err_msg) *err_msg = content_length_err_msg ? content_length_err_msg : "Invalid response headers";
        return false;
    }

    if (out_content_length) *out_content_length = content_length;
    return true;
}

static char* httpx_trim_copy(const char* start, const char* end) {
    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    size_t len = (size_t)(end - start);
    char* out = (char*)safe_malloc(len + 1);
    if (len > 0) memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static bool httpx_parse_response(VM* vm,
                                 const char* response,
                                 size_t response_len,
                                 HttpxResponse* out,
                                 int64_t* err_code,
                                 const char** err_msg) {
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!response || !out) return false;

    const char* header_end = strstr(response, "\r\n\r\n");
    size_t delim_len = 0;
    if (header_end) {
        delim_len = 4;
    } else {
        header_end = strstr(response, "\n\n");
        if (header_end) delim_len = 2;
    }

    if (!header_end) {
        if (err_code) *err_code = ERR_PARSE;
        if (err_msg) *err_msg = "Malformed HTTP response";
        return false;
    }

    int status = parse_http_status_code(response);
    if (status < 0) {
        if (err_code) *err_code = ERR_PARSE;
        if (err_msg) *err_msg = "Malformed HTTP status line";
        return false;
    }

    ObjMap* headers = obj_map_create(vm);
    const char* first_nl = strchr(response, '\n');
    const char* cursor = first_nl ? (first_nl + 1) : header_end;

    while (cursor < header_end) {
        const char* next_nl = memchr(cursor, '\n', (size_t)(header_end - cursor));
        const char* raw_end = next_nl ? next_nl : header_end;
        const char* line_start = cursor;
        const char* line_stop = raw_end;
        if (line_stop > line_start && line_stop[-1] == '\r') line_stop--;

        if (line_stop > line_start) {
            const char* colon = memchr(line_start, ':', (size_t)(line_stop - line_start));
            if (colon) {
                const char* key_start = line_start;
                const char* key_end = colon;
                while (key_start < key_end && isspace((unsigned char)*key_start)) key_start++;
                while (key_end > key_start && isspace((unsigned char)key_end[-1])) key_end--;
                char* val = httpx_trim_copy(colon + 1, line_stop);
                if (key_end > key_start) {
                    Value v;
                    value_init_string(&v, val);
                    obj_map_set_cstr_n(headers, key_start, (int)(key_end - key_start), v);
                    value_free(&v);
                }
                free(val);
            }
        }

        cursor = next_nl ? (next_nl + 1) : header_end;
    }

    const char* body_start = header_end + delim_len;
    const char* end = response + response_len;
    if (body_start > end) body_start = end;
    size_t body_len = (size_t)(end - body_start);

    if (vm && vm->config.max_string_length > 0 && (int64_t)body_len > (int64_t)vm->config.max_string_length) {
        obj_map_free(headers);
        if (err_code) *err_code = ERR_LIMIT;
        if (err_msg) *err_msg = "HTTP body exceeds max string length";
        return false;
    }

    char* body = (char*)safe_malloc(body_len + 1);
    if (body_len > 0) memcpy(body, body_start, body_len);
    body[body_len] = '\0';

    out->status_code = status;
    out->headers = headers;
    out->body = body;
    out->body_length = body_len;
    return true;
}

static bool httpx_execute(VM* vm,
                          const char* method,
                          const char* url,
                          const char* body,
                          ObjMap* headers,
                          int timeout_ms,
                          bool tls_insecure_skip_verify,
                          bool keep_alive,
                          bool accept_gzip,
                          HttpxResponse* out,
                          int64_t* err_code,
                          const char** err_msg) {
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!out) return false;
    httpx_response_init(out);

    if (!vm_is_network_enabled(vm)) {
        if (err_code) *err_code = ERR_PERMISSION;
        if (err_msg) *err_msg = "Network access is disabled";
        return false;
    }

    if (!method || !url) {
        if (err_code) *err_code = ERR_INVALID_ARGUMENT;
        if (err_msg) *err_msg = "Invalid HTTP request arguments";
        return false;
    }
    if (!httpx_method_valid(method)) {
        if (err_code) *err_code = ERR_INVALID_ARGUMENT;
        if (err_msg) *err_msg = "Invalid HTTP method";
        return false;
    }
    if (timeout_ms < HTTPX_MIN_TIMEOUT_MS || timeout_ms > HTTPX_MAX_TIMEOUT_MS) {
        if (err_code) *err_code = ERR_INVALID_ARGUMENT;
        if (err_msg) *err_msg = "Timeout must be between 1 and 120000 milliseconds";
        return false;
    }

    char host[256];
    char path[2048];
    int port = 0;
    int is_https = 0;
    if (parse_url(url, host, &port, path, &is_https) != 0) {
        if (err_code) *err_code = ERR_INVALID_ARGUMENT;
        if (err_msg) *err_msg = "Invalid URL";
        return false;
    }

    bool request_keep_alive = keep_alive;
    if (request_keep_alive && httpx_headers_connection_close(headers)) {
        request_keep_alive = false;
    }

    size_t req_len = 0;
    char* req = httpx_build_request(method,
                                    path,
                                    host,
                                    body,
                                    headers,
                                    request_keep_alive,
                                    accept_gzip,
                                    &req_len);
    if (!req) {
        if (err_code) *err_code = ERR_LIMIT;
        if (err_msg) *err_msg = "Failed to build HTTP request";
        return false;
    }

    ObjSocket* sock_obj = NULL;
    bool reused_socket = false;

    if (request_keep_alive) {
        sock_obj = httpx_pool_take_socket(vm,
                                          host,
                                          port,
                                          is_https != 0,
                                          tls_insecure_skip_verify,
                                          timeout_ms);
        reused_socket = sock_obj != NULL;
    }

    if (!sock_obj) {
        int64_t connect_err_code = 0;
        const char* connect_err_msg = NULL;
        if (!httpx_connect_socket(vm,
                                  host,
                                  port,
                                  timeout_ms,
                                  is_https != 0,
                                  tls_insecure_skip_verify,
                                  &sock_obj,
                                  &connect_err_code,
                                  &connect_err_msg)) {
            free(req);
            if (err_code) *err_code = connect_err_code ? connect_err_code : ERR_NETWORK;
            if (err_msg) *err_msg = connect_err_msg ? connect_err_msg : "Connection failed";
            return false;
        }
    }

    bool sent_ok = httpx_send_all_socket(sock_obj, req, req_len);
    if (!sent_ok && reused_socket) {
        obj_socket_free(sock_obj);
        sock_obj = NULL;
        reused_socket = false;

        int64_t connect_err_code = 0;
        const char* connect_err_msg = NULL;
        if (!httpx_connect_socket(vm,
                                  host,
                                  port,
                                  timeout_ms,
                                  is_https != 0,
                                  tls_insecure_skip_verify,
                                  &sock_obj,
                                  &connect_err_code,
                                  &connect_err_msg)) {
            free(req);
            if (err_code) *err_code = connect_err_code ? connect_err_code : ERR_NETWORK;
            if (err_msg) *err_msg = connect_err_msg ? connect_err_msg : "Connection failed";
            return false;
        }
        sent_ok = httpx_send_all_socket(sock_obj, req, req_len);
    }
    free(req);

    if (!sent_ok) {
        obj_socket_free(sock_obj);
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Failed to send request";
        return false;
    }

    HttpxResponse head_response;
    int64_t content_length = -1;
    int64_t head_err_code = 0;
    const char* head_err_msg = NULL;
    if (!httpx_recv_head_socket(vm,
                                sock_obj,
                                &head_response,
                                &content_length,
                                &head_err_code,
                                &head_err_msg)) {
        obj_socket_free(sock_obj);
        if (err_code) *err_code = head_err_code ? head_err_code : ERR_NETWORK;
        if (err_msg) *err_msg = head_err_msg ? head_err_msg : "Failed to receive response headers";
        return false;
    }

    bool no_body = httpx_response_status_has_no_body(method, head_response.status_code);
    bool chunked_body = !no_body && httpx_headers_transfer_chunked(head_response.headers);
    bool framed_body = no_body || chunked_body || content_length >= 0;
    bool response_connection_close = httpx_headers_connection_close(head_response.headers);

    char* response_body = NULL;
    size_t response_body_len = 0;
    int64_t body_err_code = 0;
    const char* body_err_msg = NULL;

    if (no_body) {
        response_body = (char*)safe_malloc(1);
        response_body[0] = '\0';
    } else if (chunked_body) {
        if (!httpx_recv_chunked_body_socket(vm,
                                            sock_obj,
                                            &response_body,
                                            &response_body_len,
                                            &body_err_code,
                                            &body_err_msg)) {
            httpx_response_free(&head_response);
            obj_socket_free(sock_obj);
            if (err_code) *err_code = body_err_code ? body_err_code : ERR_NETWORK;
            if (err_msg) *err_msg = body_err_msg ? body_err_msg : "Failed to receive response body";
            return false;
        }
    } else if (content_length >= 0) {
        if ((uint64_t)content_length > (uint64_t)SIZE_MAX) {
            httpx_response_free(&head_response);
            obj_socket_free(sock_obj);
            if (err_code) *err_code = ERR_LIMIT;
            if (err_msg) *err_msg = "Content-Length exceeds supported size";
            return false;
        }
        if (!httpx_recv_exact_socket(vm,
                                     sock_obj,
                                     (size_t)content_length,
                                     &response_body,
                                     &response_body_len,
                                     &body_err_code,
                                     &body_err_msg)) {
            httpx_response_free(&head_response);
            obj_socket_free(sock_obj);
            if (err_code) *err_code = body_err_code ? body_err_code : ERR_NETWORK;
            if (err_msg) *err_msg = body_err_msg ? body_err_msg : "Failed to receive response body";
            return false;
        }
    } else {
        if (!httpx_recv_body_until_close_socket(vm,
                                                sock_obj,
                                                &response_body,
                                                &response_body_len,
                                                &body_err_code,
                                                &body_err_msg)) {
            httpx_response_free(&head_response);
            obj_socket_free(sock_obj);
            if (err_code) *err_code = body_err_code ? body_err_code : ERR_NETWORK;
            if (err_msg) *err_msg = body_err_msg ? body_err_msg : "Failed to receive response body";
            return false;
        }
    }

    (void)response_body_len;
    out->status_code = head_response.status_code;
    out->headers = head_response.headers;
    out->body = response_body;
    out->body_length = response_body_len;
    head_response.headers = NULL;
    head_response.body = NULL;
    head_response.body_length = 0;

    if (accept_gzip &&
        !httpx_maybe_decode_gzip_body(vm, out, err_code, err_msg)) {
        httpx_response_free(out);
        obj_socket_free(sock_obj);
        return false;
    }

    bool can_pool = request_keep_alive &&
                    framed_body &&
                    !response_connection_close &&
                    sock_obj &&
                    sock_obj->socket_fd >= 0 &&
                    sock_obj->is_connected;
    if (can_pool) {
        httpx_pool_store_socket(vm,
                                host,
                                port,
                                is_https != 0,
                                tls_insecure_skip_verify,
                                sock_obj);
    } else {
        obj_socket_free(sock_obj);
    }

    return true;
}

static bool httpx_execute_head(VM* vm,
                               const char* method,
                               const char* url,
                               const char* body,
                               ObjMap* headers,
                               int timeout_ms,
                               bool tls_insecure_skip_verify,
                               bool keep_alive,
                               bool accept_gzip,
                               HttpxResponse* out_head,
                               ObjSocket** out_socket_obj,
                               int64_t* out_content_length,
                               int64_t* err_code,
                               const char** err_msg) {
    if (out_socket_obj) *out_socket_obj = NULL;
    if (out_content_length) *out_content_length = -1;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!out_head || !out_socket_obj || !out_content_length) return false;
    httpx_response_init(out_head);

    if (!vm_is_network_enabled(vm)) {
        if (err_code) *err_code = ERR_PERMISSION;
        if (err_msg) *err_msg = "Network access is disabled";
        return false;
    }

    if (!method || !url) {
        if (err_code) *err_code = ERR_INVALID_ARGUMENT;
        if (err_msg) *err_msg = "Invalid HTTP request arguments";
        return false;
    }
    if (!httpx_method_valid(method)) {
        if (err_code) *err_code = ERR_INVALID_ARGUMENT;
        if (err_msg) *err_msg = "Invalid HTTP method";
        return false;
    }
    if (timeout_ms < HTTPX_MIN_TIMEOUT_MS || timeout_ms > HTTPX_MAX_TIMEOUT_MS) {
        if (err_code) *err_code = ERR_INVALID_ARGUMENT;
        if (err_msg) *err_msg = "Timeout must be between 1 and 120000 milliseconds";
        return false;
    }

    char host[256];
    char path[2048];
    int port = 0;
    int is_https = 0;
    if (parse_url(url, host, &port, path, &is_https) != 0) {
        if (err_code) *err_code = ERR_INVALID_ARGUMENT;
        if (err_msg) *err_msg = "Invalid URL";
        return false;
    }

    bool request_keep_alive = keep_alive;
    if (request_keep_alive && httpx_headers_connection_close(headers)) {
        request_keep_alive = false;
    }

    size_t req_len = 0;
    char* req = httpx_build_request(method,
                                    path,
                                    host,
                                    body,
                                    headers,
                                    request_keep_alive,
                                    accept_gzip,
                                    &req_len);
    if (!req) {
        if (err_code) *err_code = ERR_LIMIT;
        if (err_msg) *err_msg = "Failed to build HTTP request";
        return false;
    }

    ObjSocket* sock_obj = NULL;
    int64_t connect_err_code = 0;
    const char* connect_err_msg = NULL;
    if (!httpx_connect_socket(vm,
                              host,
                              port,
                              timeout_ms,
                              is_https != 0,
                              tls_insecure_skip_verify,
                              &sock_obj,
                              &connect_err_code,
                              &connect_err_msg)) {
        free(req);
        if (err_code) *err_code = connect_err_code ? connect_err_code : ERR_NETWORK;
        if (err_msg) *err_msg = connect_err_msg ? connect_err_msg : "Connection failed";
        return false;
    }

    bool sent_ok = httpx_send_all_socket(sock_obj, req, req_len);
    free(req);
    if (!sent_ok) {
        obj_socket_free(sock_obj);
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Failed to send request";
        return false;
    }

    int64_t head_err_code = 0;
    const char* head_err_msg = NULL;
    if (!httpx_recv_head_socket(vm,
                                sock_obj,
                                out_head,
                                out_content_length,
                                &head_err_code,
                                &head_err_msg)) {
        obj_socket_free(sock_obj);
        if (err_code) *err_code = head_err_code ? head_err_code : ERR_NETWORK;
        if (err_msg) *err_msg = head_err_msg ? head_err_msg : "Failed to receive response headers";
        return false;
    }

    *out_socket_obj = sock_obj;
    return true;
}

static Value httpx_make_head_response_value(VM* vm,
                                            HttpxResponse* response,
                                            ObjSocket* socket_obj,
                                            int64_t content_length) {
    ObjMap* resp = obj_map_create(vm);
    Value val;

    value_init_int(&val, response ? response->status_code : 0);
    obj_map_set_cstr(resp, "status", val);
    value_free(&val);

    if (response && response->headers) {
        value_init_map(&val, response->headers);
    } else {
        ObjMap* empty = obj_map_create(vm);
        value_init_map(&val, empty);
    }
    obj_map_set_cstr(resp, "headers", val);
    value_free(&val);
    if (response) {
        response->headers = NULL;
    }

    value_init_int(&val, content_length);
    obj_map_set_cstr(resp, "contentLength", val);
    value_free(&val);

    if (socket_obj) {
        value_init_socket(&val, socket_obj);
    } else {
        value_init_nil(&val);
    }
    obj_map_set_cstr(resp, "socket", val);
    value_free(&val);

    Value out;
    value_init_map(&out, resp);
    return out;
}

static Value httpx_make_response_value(VM* vm, HttpxResponse* response) {
    ObjMap* resp = obj_map_create(vm);
    Value val;

    value_init_int(&val, response ? response->status_code : 0);
    obj_map_set_cstr(resp, "status", val);
    value_free(&val);

    if (response && response->body) {
        value_init_string_n(&val, response->body, (int)response->body_length);
    } else {
        value_init_string(&val, "");
    }
    obj_map_set_cstr(resp, "body", val);
    value_free(&val);

    if (response && response->headers) {
        value_init_map(&val, response->headers);
    } else {
        ObjMap* empty = obj_map_create(vm);
        value_init_map(&val, empty);
    }
    obj_map_set_cstr(resp, "headers", val);
    value_free(&val);
    if (response) {
        response->headers = NULL;
    }

    Value out;
    value_init_map(&out, resp);
    return out;
}

static void http_legacy_request(VM* vm,
                                Value* slot,
                                const char* method,
                                const char* url,
                                const char* body,
                                ObjMap* headers) {
    HttpxResponse response;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    if (!httpx_execute(vm,
                       method,
                       url,
                       body,
                       headers,
                       10000,
                       false,
                       false,
                       true,
                       &response,
                       &err_code,
                       &err_msg)) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm,
                         slot,
                         ok,
                         err_code ? err_code : ERR_NETWORK,
                         err_msg ? err_msg : "Request failed");
        return;
    }

    Value ok;
    value_init_string_n(&ok, response.body ? response.body : "", (int)response.body_length);
    if (response.status_code >= 400) {
        char status_msg[64];
        snprintf(status_msg, sizeof(status_msg), "HTTP error: %d", response.status_code);
        Value data;
        value_init_int(&data, response.status_code);
        result_tuple_set_data(vm, slot, ok, ERR_HTTP, status_msg, data);
        httpx_response_free(&response);
        return;
    }

    result_tuple_set(vm, slot, ok, 0, NULL);
    httpx_response_free(&response);
}

static bool httpx_decode_request_options(const Value* options_val,
                                         bool default_gzip,
                                         bool* out_tls_insecure_skip_verify,
                                         bool* out_keep_alive,
                                         bool* out_gzip,
                                         int64_t* err_code,
                                         const char** err_msg) {
    if (out_tls_insecure_skip_verify) *out_tls_insecure_skip_verify = false;
    if (out_keep_alive) *out_keep_alive = false;
    if (out_gzip) *out_gzip = default_gzip;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;

    if (!options_val || value_get_type(options_val) == VAL_NIL) {
        return true;
    }

    ObjMap* options_map = value_get_map_obj(options_val);
    if (value_get_type(options_val) != VAL_MAP || !options_map) {
        if (err_code) *err_code = ERR_INVALID_ARGUMENT;
        if (err_msg) *err_msg = "requestOptions must be map<string, any>?";
        return false;
    }

    Value tls_insecure = obj_map_get_cstr(options_map, "tlsInsecureSkipVerify");
    if (value_is_nil(&tls_insecure)) {
        tls_insecure = obj_map_get_cstr(options_map, "insecureSkipVerify");
    }

    if (!value_is_nil(&tls_insecure)) {
        if (value_get_type(&tls_insecure) != VAL_BOOL) {
            if (err_code) *err_code = ERR_INVALID_ARGUMENT;
            if (err_msg) *err_msg = "requestOptions.tlsInsecureSkipVerify must be bool";
            return false;
        }
        if (out_tls_insecure_skip_verify) {
            *out_tls_insecure_skip_verify = value_get_bool(&tls_insecure);
        }
    }

    Value keep_alive = obj_map_get_cstr(options_map, "keepAlive");
    Value connection_pool = obj_map_get_cstr(options_map, "connectionPool");

    bool has_keep_alive = !value_is_nil(&keep_alive);
    bool has_connection_pool = !value_is_nil(&connection_pool);
    bool keep_alive_value = false;
    bool connection_pool_value = false;

    if (has_keep_alive) {
        if (value_get_type(&keep_alive) != VAL_BOOL) {
            if (err_code) *err_code = ERR_INVALID_ARGUMENT;
            if (err_msg) *err_msg = "requestOptions.keepAlive must be bool";
            return false;
        }
        keep_alive_value = value_get_bool(&keep_alive);
    }

    if (has_connection_pool) {
        if (value_get_type(&connection_pool) != VAL_BOOL) {
            if (err_code) *err_code = ERR_INVALID_ARGUMENT;
            if (err_msg) *err_msg = "requestOptions.connectionPool must be bool";
            return false;
        }
        connection_pool_value = value_get_bool(&connection_pool);
    }

    if (has_keep_alive && has_connection_pool && keep_alive_value != connection_pool_value) {
        if (err_code) *err_code = ERR_INVALID_ARGUMENT;
        if (err_msg) *err_msg = "requestOptions.keepAlive and requestOptions.connectionPool must match when both are set";
        return false;
    }

    if (out_keep_alive) {
        if (has_keep_alive) {
            *out_keep_alive = keep_alive_value;
        } else if (has_connection_pool) {
            *out_keep_alive = connection_pool_value;
        }
    }

    Value gzip_value = obj_map_get_cstr(options_map, "gzip");
    if (!value_is_nil(&gzip_value)) {
        if (value_get_type(&gzip_value) != VAL_BOOL) {
            if (err_code) *err_code = ERR_INVALID_ARGUMENT;
            if (err_msg) *err_msg = "requestOptions.gzip must be bool";
            return false;
        }
        if (out_gzip) {
            *out_gzip = value_get_bool(&gzip_value);
        }
    }

    return true;
}

void builtin_http_request(VM* vm) {
    // arg0 method, arg1 url, arg2 body?, arg3 headers?, arg4 timeoutMs
    Value* method_slot = &vm->stack.values[vm->stack.count - 5];
    Value method_val = *method_slot;
    Value url_val = vm->stack.values[vm->stack.count - 4];
    Value body_val = vm->stack.values[vm->stack.count - 3];
    Value headers_val = vm->stack.values[vm->stack.count - 2];
    Value timeout_val = vm->stack.values[vm->stack.count - 1];

    ObjString* method_str = value_get_string_obj(&method_val);
    ObjString* url_str = value_get_string_obj(&url_val);
    ObjString* body_str = value_get_string_obj(&body_val);
    ObjMap* headers_map = value_get_map_obj(&headers_val);
    ValueType body_type = value_get_type(&body_val);
    ValueType headers_type = value_get_type(&headers_val);
    if (value_get_type(&method_val) != VAL_STRING || !method_str || !method_str->chars ||
        value_get_type(&url_val) != VAL_STRING || !url_str || !url_str->chars ||
        value_get_type(&timeout_val) != VAL_INT ||
        (body_type != VAL_NIL && body_type != VAL_STRING) ||
        (headers_type != VAL_NIL && headers_type != VAL_MAP) ||
        (body_type == VAL_STRING && !body_str) ||
        (headers_type == VAL_MAP && !headers_map)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         method_slot,
                         ok,
                         ERR_INVALID_ARGUMENT,
                         "httpRequest expects (method:string, url:string, body:string?, headers:map<string, string>?, timeoutMs:int)");
        return;
    }

    const char* body = NULL;
    if (body_type == VAL_STRING && body_str) {
        body = body_str->chars;
    }
    ObjMap* headers = (headers_type == VAL_MAP) ? headers_map : NULL;

    int64_t timeout64 = value_get_int(&timeout_val);
    if (timeout64 < INT_MIN || timeout64 > INT_MAX) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, method_slot, ok, ERR_INVALID_ARGUMENT, "timeoutMs is out of range");
        return;
    }

    HttpxResponse response;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    if (!httpx_execute(vm,
                       method_str->chars,
                       url_str->chars,
                       body,
                       headers,
                       (int)timeout64,
                       false,
                       false,
                       true,
                       &response,
                       &err_code,
                       &err_msg)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         method_slot,
                         ok,
                         err_code ? err_code : ERR_NETWORK,
                         err_msg ? err_msg : "Request failed");
        return;
    }

    Value ok = httpx_make_response_value(vm, &response);
    result_tuple_set(vm, method_slot, ok, 0, NULL);
    httpx_response_free(&response);
}

void builtin_http_request_head(VM* vm) {
    // arg0 method, arg1 url, arg2 body?, arg3 headers?, arg4 timeoutMs
    Value* method_slot = &vm->stack.values[vm->stack.count - 5];
    Value method_val = *method_slot;
    Value url_val = vm->stack.values[vm->stack.count - 4];
    Value body_val = vm->stack.values[vm->stack.count - 3];
    Value headers_val = vm->stack.values[vm->stack.count - 2];
    Value timeout_val = vm->stack.values[vm->stack.count - 1];

    ObjString* method_str = value_get_string_obj(&method_val);
    ObjString* url_str = value_get_string_obj(&url_val);
    ObjString* body_str = value_get_string_obj(&body_val);
    ObjMap* headers_map = value_get_map_obj(&headers_val);
    ValueType body_type = value_get_type(&body_val);
    ValueType headers_type = value_get_type(&headers_val);
    if (value_get_type(&method_val) != VAL_STRING || !method_str || !method_str->chars ||
        value_get_type(&url_val) != VAL_STRING || !url_str || !url_str->chars ||
        value_get_type(&timeout_val) != VAL_INT ||
        (body_type != VAL_NIL && body_type != VAL_STRING) ||
        (headers_type != VAL_NIL && headers_type != VAL_MAP) ||
        (body_type == VAL_STRING && !body_str) ||
        (headers_type == VAL_MAP && !headers_map)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         method_slot,
                         ok,
                         ERR_INVALID_ARGUMENT,
                         "httpRequestHead expects (method:string, url:string, body:string?, headers:map<string, string>?, timeoutMs:int)");
        return;
    }

    const char* body = NULL;
    if (body_type == VAL_STRING && body_str) {
        body = body_str->chars;
    }
    ObjMap* headers = (headers_type == VAL_MAP) ? headers_map : NULL;

    int64_t timeout64 = value_get_int(&timeout_val);
    if (timeout64 < INT_MIN || timeout64 > INT_MAX) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, method_slot, ok, ERR_INVALID_ARGUMENT, "timeoutMs is out of range");
        return;
    }

    HttpxResponse head_response;
    ObjSocket* sock_obj = NULL;
    int64_t content_length = -1;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    if (!httpx_execute_head(vm,
                            method_str->chars,
                            url_str->chars,
                            body,
                            headers,
                            (int)timeout64,
                            false,
                            false,
                            false,
                            &head_response,
                            &sock_obj,
                            &content_length,
                            &err_code,
                            &err_msg)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         method_slot,
                         ok,
                         err_code ? err_code : ERR_NETWORK,
                         err_msg ? err_msg : "Request failed");
        return;
    }

    Value ok = httpx_make_head_response_value(vm, &head_response, sock_obj, content_length);
    result_tuple_set(vm, method_slot, ok, 0, NULL);
    httpx_response_free(&head_response);
}

void builtin_http_request_with_options(VM* vm) {
    // arg0 method, arg1 url, arg2 body?, arg3 headers?, arg4 timeoutMs, arg5 requestOptions?
    Value* method_slot = &vm->stack.values[vm->stack.count - 6];
    Value method_val = *method_slot;
    Value url_val = vm->stack.values[vm->stack.count - 5];
    Value body_val = vm->stack.values[vm->stack.count - 4];
    Value headers_val = vm->stack.values[vm->stack.count - 3];
    Value timeout_val = vm->stack.values[vm->stack.count - 2];
    Value request_options_val = vm->stack.values[vm->stack.count - 1];

    ObjString* method_str = value_get_string_obj(&method_val);
    ObjString* url_str = value_get_string_obj(&url_val);
    ObjString* body_str = value_get_string_obj(&body_val);
    ObjMap* headers_map = value_get_map_obj(&headers_val);
    ObjMap* request_options_map = value_get_map_obj(&request_options_val);
    ValueType body_type = value_get_type(&body_val);
    ValueType headers_type = value_get_type(&headers_val);
    ValueType request_options_type = value_get_type(&request_options_val);
    if (value_get_type(&method_val) != VAL_STRING || !method_str || !method_str->chars ||
        value_get_type(&url_val) != VAL_STRING || !url_str || !url_str->chars ||
        value_get_type(&timeout_val) != VAL_INT ||
        (body_type != VAL_NIL && body_type != VAL_STRING) ||
        (headers_type != VAL_NIL && headers_type != VAL_MAP) ||
        (request_options_type != VAL_NIL && request_options_type != VAL_MAP) ||
        (body_type == VAL_STRING && !body_str) ||
        (headers_type == VAL_MAP && !headers_map) ||
        (request_options_type == VAL_MAP && !request_options_map)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         method_slot,
                         ok,
                         ERR_INVALID_ARGUMENT,
                         "httpRequestWithOptions expects (method:string, url:string, body:string?, headers:map<string, string>?, timeoutMs:int, requestOptions:map<string, any>?)");
        return;
    }

    const char* body = NULL;
    if (body_type == VAL_STRING && body_str) {
        body = body_str->chars;
    }
    ObjMap* headers = (headers_type == VAL_MAP) ? headers_map : NULL;

    int64_t timeout64 = value_get_int(&timeout_val);
    if (timeout64 < INT_MIN || timeout64 > INT_MAX) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, method_slot, ok, ERR_INVALID_ARGUMENT, "timeoutMs is out of range");
        return;
    }

    bool tls_insecure_skip_verify = false;
    bool keep_alive = false;
    bool accept_gzip = true;
    int64_t options_err_code = 0;
    const char* options_err_msg = NULL;
    if (!httpx_decode_request_options(&request_options_val,
                                      true,
                                      &tls_insecure_skip_verify,
                                      &keep_alive,
                                      &accept_gzip,
                                      &options_err_code,
                                      &options_err_msg)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         method_slot,
                         ok,
                         options_err_code ? options_err_code : ERR_INVALID_ARGUMENT,
                         options_err_msg ? options_err_msg : "Invalid request options");
        return;
    }

    HttpxResponse response;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    if (!httpx_execute(vm,
                       method_str->chars,
                       url_str->chars,
                       body,
                       headers,
                       (int)timeout64,
                       tls_insecure_skip_verify,
                       keep_alive,
                       accept_gzip,
                       &response,
                       &err_code,
                       &err_msg)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         method_slot,
                         ok,
                         err_code ? err_code : ERR_NETWORK,
                         err_msg ? err_msg : "Request failed");
        return;
    }

    Value ok = httpx_make_response_value(vm, &response);
    result_tuple_set(vm, method_slot, ok, 0, NULL);
    httpx_response_free(&response);
}

void builtin_http_request_head_with_options(VM* vm) {
    // arg0 method, arg1 url, arg2 body?, arg3 headers?, arg4 timeoutMs, arg5 requestOptions?
    Value* method_slot = &vm->stack.values[vm->stack.count - 6];
    Value method_val = *method_slot;
    Value url_val = vm->stack.values[vm->stack.count - 5];
    Value body_val = vm->stack.values[vm->stack.count - 4];
    Value headers_val = vm->stack.values[vm->stack.count - 3];
    Value timeout_val = vm->stack.values[vm->stack.count - 2];
    Value request_options_val = vm->stack.values[vm->stack.count - 1];

    ObjString* method_str = value_get_string_obj(&method_val);
    ObjString* url_str = value_get_string_obj(&url_val);
    ObjString* body_str = value_get_string_obj(&body_val);
    ObjMap* headers_map = value_get_map_obj(&headers_val);
    ObjMap* request_options_map = value_get_map_obj(&request_options_val);
    ValueType body_type = value_get_type(&body_val);
    ValueType headers_type = value_get_type(&headers_val);
    ValueType request_options_type = value_get_type(&request_options_val);
    if (value_get_type(&method_val) != VAL_STRING || !method_str || !method_str->chars ||
        value_get_type(&url_val) != VAL_STRING || !url_str || !url_str->chars ||
        value_get_type(&timeout_val) != VAL_INT ||
        (body_type != VAL_NIL && body_type != VAL_STRING) ||
        (headers_type != VAL_NIL && headers_type != VAL_MAP) ||
        (request_options_type != VAL_NIL && request_options_type != VAL_MAP) ||
        (body_type == VAL_STRING && !body_str) ||
        (headers_type == VAL_MAP && !headers_map) ||
        (request_options_type == VAL_MAP && !request_options_map)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         method_slot,
                         ok,
                         ERR_INVALID_ARGUMENT,
                         "httpRequestHeadWithOptions expects (method:string, url:string, body:string?, headers:map<string, string>?, timeoutMs:int, requestOptions:map<string, any>?)");
        return;
    }

    const char* body = NULL;
    if (body_type == VAL_STRING && body_str) {
        body = body_str->chars;
    }
    ObjMap* headers = (headers_type == VAL_MAP) ? headers_map : NULL;

    int64_t timeout64 = value_get_int(&timeout_val);
    if (timeout64 < INT_MIN || timeout64 > INT_MAX) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, method_slot, ok, ERR_INVALID_ARGUMENT, "timeoutMs is out of range");
        return;
    }

    bool tls_insecure_skip_verify = false;
    bool keep_alive = false;
    bool accept_gzip = false;
    int64_t options_err_code = 0;
    const char* options_err_msg = NULL;
    if (!httpx_decode_request_options(&request_options_val,
                                      false,
                                      &tls_insecure_skip_verify,
                                      &keep_alive,
                                      &accept_gzip,
                                      &options_err_code,
                                      &options_err_msg)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         method_slot,
                         ok,
                         options_err_code ? options_err_code : ERR_INVALID_ARGUMENT,
                         options_err_msg ? options_err_msg : "Invalid request options");
        return;
    }

    HttpxResponse head_response;
    ObjSocket* sock_obj = NULL;
    int64_t content_length = -1;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    if (!httpx_execute_head(vm,
                            method_str->chars,
                            url_str->chars,
                            body,
                            headers,
                            (int)timeout64,
                            tls_insecure_skip_verify,
                            keep_alive,
                            accept_gzip,
                            &head_response,
                            &sock_obj,
                            &content_length,
                            &err_code,
                            &err_msg)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         method_slot,
                         ok,
                         err_code ? err_code : ERR_NETWORK,
                         err_msg ? err_msg : "Request failed");
        return;
    }

    Value ok = httpx_make_head_response_value(vm, &head_response, sock_obj, content_length);
    result_tuple_set(vm, method_slot, ok, 0, NULL);
    httpx_response_free(&head_response);
}

enum {
    TCPX_MIN_TIMEOUT_MS = 0,
    TCPX_MAX_TIMEOUT_MS = 120000,
    HTTP_SERVER_MAX_REQUEST_BYTES = 8 * 1024 * 1024
};

static int socket_send_dispatch(ObjSocket* sock, const char* data, int len) {
    if (!sock || sock->socket_fd < 0 || !data || len < 0) return -1;
    if (len == 0) return 0;
    if (sock->transport_send) {
        return sock->transport_send(sock, data, len);
    }
    return send(sock->socket_fd, data, len, 0);
}

static int socket_recv_dispatch(ObjSocket* sock, char* out, int max_len) {
    if (!sock || sock->socket_fd < 0 || !out || max_len <= 0) return -1;
    if (sock->transport_recv) {
        return sock->transport_recv(sock, out, max_len);
    }
    return recv(sock->socket_fd, out, max_len, 0);
}

static void socket_close_dispatch(ObjSocket* sock) {
    if (!sock) return;
    if (sock->transport_close) {
        sock->transport_close(sock);
    } else if (sock->socket_fd >= 0) {
        CLOSESOCKET(sock->socket_fd);
        sock->socket_fd = -1;
    }
    if (sock->limit_tracked) {
        VM* owner_vm = (VM*)sock->owner_vm;
        vm_release_socket_handle(owner_vm);
        sock->limit_tracked = false;
    }
    sock->owner_vm = NULL;
    sock->socket_fd = -1;
    sock->is_connected = false;
}

static bool socket_recv_error_is_connection_close(void) {
#ifdef _WIN32
    int sock_err = WSAGetLastError();
    return sock_err == WSAECONNRESET || sock_err == WSAECONNABORTED ||
           sock_err == WSAENOTCONN || sock_err == WSAESHUTDOWN;
#else
    return errno == ECONNRESET || errno == ENOTCONN || errno == EPIPE;
#endif
}

static bool tcpx_set_socket_timeouts(int sock_fd, int timeout_ms, const char** out_err) {
    if (out_err) *out_err = NULL;
    if (sock_fd < 0) {
        if (out_err) *out_err = "Invalid socket";
        return false;
    }
    if (timeout_ms < 0 || timeout_ms > TCPX_MAX_TIMEOUT_MS) {
        if (out_err) *out_err = "Invalid timeout";
        return false;
    }

#ifdef _WIN32
    DWORD timeout = (DWORD)timeout_ms;
    int r1 = setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    int r2 = setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    if (r1 != 0 || r2 != 0) {
        if (out_err) *out_err = "Failed to configure socket timeouts";
        return false;
    }
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r1 = setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    int r2 = setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
    if (r1 != 0 || r2 != 0) {
        if (out_err) *out_err = "Failed to configure socket timeouts";
        return false;
    }
#endif

    return true;
}

static bool tcpx_connect_host_port(VM* vm,
                                   const char* host,
                                   int port,
                                   int timeout_ms,
                                   int* out_sock,
                                   int64_t* out_err_code,
                                   const char** out_err_msg) {
    if (out_sock) *out_sock = -1;
    if (out_err_code) *out_err_code = 0;
    if (out_err_msg) *out_err_msg = NULL;
    if (!out_sock) return false;

    if (!host || host[0] == '\0') {
        if (out_err_code) *out_err_code = ERR_INVALID_ARGUMENT;
        if (out_err_msg) *out_err_msg = "Host must be non-empty";
        return false;
    }
    if (port < 1 || port > 65535) {
        if (out_err_code) *out_err_code = ERR_INVALID_ARGUMENT;
        if (out_err_msg) *out_err_msg = "port must be in range 1..65535";
        return false;
    }
    if (timeout_ms < 1 || timeout_ms > TCPX_MAX_TIMEOUT_MS) {
        if (out_err_code) *out_err_code = ERR_INVALID_ARGUMENT;
        if (out_err_msg) *out_err_msg = "timeoutMs must be in range 1..120000";
        return false;
    }

    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = NULL;
    int gai = getaddrinfo(host, port_buf, &hints, &result);
    if (gai != 0 || !result) {
        if (out_err_code) *out_err_code = ERR_NETWORK;
        if (out_err_msg) *out_err_msg = "Host resolution failed";
        return false;
    }

    int sock_fd = -1;
    bool connected = false;
    for (struct addrinfo* ai = result; ai; ai = ai->ai_next) {
        if (!vm_try_acquire_socket_handle(vm)) {
            if (out_err_code) *out_err_code = ERR_LIMIT;
            if (out_err_msg) *out_err_msg = "Open socket limit exceeded";
            break;
        }

        sock_fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock_fd < 0) {
            vm_release_socket_handle(vm);
            continue;
        }

        const char* timeout_err = NULL;
        if (!tcpx_set_socket_timeouts(sock_fd, timeout_ms, &timeout_err)) {
            CLOSESOCKET(sock_fd);
            vm_release_socket_handle(vm);
            sock_fd = -1;
            continue;
        }

#ifdef _WIN32
        int addr_len = (int)ai->ai_addrlen;
#else
        socklen_t addr_len = (socklen_t)ai->ai_addrlen;
#endif

        if (connect(sock_fd, ai->ai_addr, addr_len) == 0) {
            connected = true;
            break;
        }

        CLOSESOCKET(sock_fd);
        vm_release_socket_handle(vm);
        sock_fd = -1;
    }

    freeaddrinfo(result);

    if (!connected || sock_fd < 0) {
        if (out_err_code && *out_err_code == 0) *out_err_code = ERR_NETWORK;
        if (out_err_msg && *out_err_msg == NULL) *out_err_msg = "Connection failed";
        return false;
    }

    *out_sock = sock_fd;
    return true;
}

#ifdef _WIN32
typedef struct TlsxTransportCtx {
    CredHandle cred;
    CtxtHandle ctx;
    bool cred_ready;
    bool ctx_ready;
    SecPkgContext_StreamSizes sizes;
    bool sizes_ready;
    uint8_t* enc_buf;
    int enc_len;
    int enc_cap;
    uint8_t* plain_pending;
    int plain_pending_len;
    int plain_pending_off;
} TlsxTransportCtx;

static void tlsx_ctx_clear_plain_pending(TlsxTransportCtx* ctx) {
    if (!ctx) return;
    if (ctx->plain_pending) {
        free(ctx->plain_pending);
        ctx->plain_pending = NULL;
    }
    ctx->plain_pending_len = 0;
    ctx->plain_pending_off = 0;
}

static bool tlsx_ctx_set_plain_pending(TlsxTransportCtx* ctx, const uint8_t* data, int len) {
    if (!ctx) return false;
    tlsx_ctx_clear_plain_pending(ctx);
    if (!data || len <= 0) return true;
    uint8_t* pending = (uint8_t*)malloc((size_t)len);
    if (!pending) return false;
    memcpy(pending, data, (size_t)len);
    ctx->plain_pending = pending;
    ctx->plain_pending_len = len;
    ctx->plain_pending_off = 0;
    return true;
}

static int tlsx_ctx_take_plain_pending(TlsxTransportCtx* ctx, char* out, int max_len) {
    if (!ctx || !out || max_len <= 0) return 0;
    int available = ctx->plain_pending_len - ctx->plain_pending_off;
    if (available <= 0) {
        tlsx_ctx_clear_plain_pending(ctx);
        return 0;
    }
    int to_copy = available < max_len ? available : max_len;
    memcpy(out, ctx->plain_pending + ctx->plain_pending_off, (size_t)to_copy);
    ctx->plain_pending_off += to_copy;
    if (ctx->plain_pending_off >= ctx->plain_pending_len) {
        tlsx_ctx_clear_plain_pending(ctx);
    }
    return to_copy;
}

static bool tlsx_ctx_ensure_enc_capacity(TlsxTransportCtx* ctx, int required) {
    if (!ctx) return false;
    if (required <= ctx->enc_cap) return true;
    int new_cap = ctx->enc_cap > 0 ? ctx->enc_cap : 16384;
    while (new_cap < required) {
        if (new_cap > INT_MAX / 2) return false;
        new_cap *= 2;
    }
    uint8_t* next = (uint8_t*)realloc(ctx->enc_buf, (size_t)new_cap);
    if (!next) return false;
    ctx->enc_buf = next;
    ctx->enc_cap = new_cap;
    return true;
}

static bool tlsx_ctx_append_encrypted(TlsxTransportCtx* ctx, const uint8_t* data, int len) {
    if (!ctx) return false;
    if (!data || len <= 0) return true;
    if (len > INT_MAX - ctx->enc_len) return false;
    int needed = ctx->enc_len + len;
    if (!tlsx_ctx_ensure_enc_capacity(ctx, needed)) return false;
    memcpy(ctx->enc_buf + ctx->enc_len, data, (size_t)len);
    ctx->enc_len += len;
    return true;
}

static void tlsx_ctx_reset(TlsxTransportCtx* ctx) {
    if (!ctx) return;
    tlsx_ctx_clear_plain_pending(ctx);
    if (ctx->enc_buf) {
        free(ctx->enc_buf);
        ctx->enc_buf = NULL;
    }
    ctx->enc_len = 0;
    ctx->enc_cap = 0;
    if (ctx->ctx_ready) {
        DeleteSecurityContext(&ctx->ctx);
        ctx->ctx_ready = false;
    }
    if (ctx->cred_ready) {
        FreeCredentialsHandle(&ctx->cred);
        ctx->cred_ready = false;
    }
    ctx->sizes_ready = false;
}

static bool tlsx_send_all_raw(int sock_fd, const uint8_t* data, int len) {
    if (sock_fd < 0 || (!data && len > 0) || len < 0) return false;
    int sent = 0;
    while (sent < len) {
        int chunk = len - sent;
        int n = send(sock_fd, (const char*)data + sent, chunk, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool tlsx_handshake_client(ObjSocket* sock,
                                  const char* host,
                                  TlsxTransportCtx* ctx,
                                  bool insecure_skip_verify,
                                  const char** out_err) {
    if (out_err) *out_err = NULL;
    if (!sock || sock->socket_fd < 0 || !host || host[0] == '\0' || !ctx) {
        if (out_err) *out_err = "Invalid TLS handshake arguments";
        return false;
    }

    SCHANNEL_CRED cred_data;
    memset(&cred_data, 0, sizeof(cred_data));
    cred_data.dwVersion = SCHANNEL_CRED_VERSION;
    cred_data.dwFlags = SCH_CRED_NO_DEFAULT_CREDS;
    if (insecure_skip_verify) {
        cred_data.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION;
#ifdef SCH_CRED_NO_SERVERNAME_CHECK
        cred_data.dwFlags |= SCH_CRED_NO_SERVERNAME_CHECK;
#endif
    } else {
        cred_data.dwFlags |= SCH_CRED_AUTO_CRED_VALIDATION;
    }
#ifdef SCH_USE_STRONG_CRYPTO
    cred_data.dwFlags |= SCH_USE_STRONG_CRYPTO;
#endif
#ifdef SP_PROT_TLS1_3_CLIENT
    cred_data.grbitEnabledProtocols = SP_PROT_TLS1_3_CLIENT | SP_PROT_TLS1_2_CLIENT;
#else
    cred_data.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
#endif

    TimeStamp expiry;
    SECURITY_STATUS ss = AcquireCredentialsHandleA(NULL,
                                                   (SEC_CHAR*)UNISP_NAME_A,
                                                   SECPKG_CRED_OUTBOUND,
                                                   NULL,
                                                   &cred_data,
                                                   NULL,
                                                   NULL,
                                                   &ctx->cred,
                                                   &expiry);
    if (ss != SEC_E_OK) {
        if (out_err) *out_err = "Failed to acquire TLS credentials";
        return false;
    }
    ctx->cred_ready = true;

    DWORD req_flags = ISC_REQ_SEQUENCE_DETECT |
                      ISC_REQ_REPLAY_DETECT |
                      ISC_REQ_CONFIDENTIALITY |
                      ISC_REQ_EXTENDED_ERROR |
                      ISC_REQ_ALLOCATE_MEMORY |
                      ISC_REQ_STREAM;
#ifdef ISC_REQ_MANUAL_CRED_VALIDATION
    if (insecure_skip_verify) {
        req_flags |= ISC_REQ_MANUAL_CRED_VALIDATION;
    }
#endif
    DWORD out_flags = 0;

    SecBuffer out_buf;
    memset(&out_buf, 0, sizeof(out_buf));
    out_buf.BufferType = SECBUFFER_TOKEN;
    SecBufferDesc out_desc;
    memset(&out_desc, 0, sizeof(out_desc));
    out_desc.ulVersion = SECBUFFER_VERSION;
    out_desc.cBuffers = 1;
    out_desc.pBuffers = &out_buf;

    ss = InitializeSecurityContextA(&ctx->cred,
                                    NULL,
                                    (SEC_CHAR*)host,
                                    req_flags,
                                    0,
                                    SECURITY_NATIVE_DREP,
                                    NULL,
                                    0,
                                    &ctx->ctx,
                                    &out_desc,
                                    &out_flags,
                                    &expiry);
    if (ss != SEC_I_CONTINUE_NEEDED && ss != SEC_E_OK) {
        if (out_err) *out_err = "TLS handshake init failed";
        return false;
    }
    ctx->ctx_ready = true;

    if (out_buf.cbBuffer > 0 && out_buf.pvBuffer) {
        bool sent = tlsx_send_all_raw(sock->socket_fd, (const uint8_t*)out_buf.pvBuffer, (int)out_buf.cbBuffer);
        FreeContextBuffer(out_buf.pvBuffer);
        out_buf.pvBuffer = NULL;
        out_buf.cbBuffer = 0;
        if (!sent) {
            if (out_err) *out_err = "Failed to send TLS handshake data";
            return false;
        }
    }

    uint8_t* in_buf = NULL;
    int in_len = 0;
    int in_cap = 0;

    while (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_INCOMPLETE_MESSAGE) {
        if (ss == SEC_E_INCOMPLETE_MESSAGE || in_len == 0) {
            uint8_t chunk[8192];
            int n = recv(sock->socket_fd, (char*)chunk, (int)sizeof(chunk), 0);
            if (n <= 0) {
                if (in_buf) free(in_buf);
                if (out_err) *out_err = "TLS handshake receive failed";
                return false;
            }
            int needed = in_len + n;
            if (needed > in_cap) {
                int new_cap = in_cap > 0 ? in_cap : 16384;
                while (new_cap < needed) {
                    if (new_cap > INT_MAX / 2) {
                        if (in_buf) free(in_buf);
                        if (out_err) *out_err = "TLS handshake buffer too large";
                        return false;
                    }
                    new_cap *= 2;
                }
                uint8_t* next = (uint8_t*)realloc(in_buf, (size_t)new_cap);
                if (!next) {
                    if (in_buf) free(in_buf);
                    if (out_err) *out_err = "TLS handshake allocation failed";
                    return false;
                }
                in_buf = next;
                in_cap = new_cap;
            }
            memcpy(in_buf + in_len, chunk, (size_t)n);
            in_len += n;
        }

        SecBuffer in_buffers[2];
        memset(in_buffers, 0, sizeof(in_buffers));
        in_buffers[0].BufferType = SECBUFFER_TOKEN;
        in_buffers[0].pvBuffer = in_buf;
        in_buffers[0].cbBuffer = (unsigned long)in_len;
        in_buffers[1].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc in_desc;
        memset(&in_desc, 0, sizeof(in_desc));
        in_desc.ulVersion = SECBUFFER_VERSION;
        in_desc.cBuffers = 2;
        in_desc.pBuffers = in_buffers;

        memset(&out_buf, 0, sizeof(out_buf));
        out_buf.BufferType = SECBUFFER_TOKEN;
        memset(&out_desc, 0, sizeof(out_desc));
        out_desc.ulVersion = SECBUFFER_VERSION;
        out_desc.cBuffers = 1;
        out_desc.pBuffers = &out_buf;

        ss = InitializeSecurityContextA(&ctx->cred,
                                        &ctx->ctx,
                                        (SEC_CHAR*)host,
                                        req_flags,
                                        0,
                                        SECURITY_NATIVE_DREP,
                                        &in_desc,
                                        0,
                                        &ctx->ctx,
                                        &out_desc,
                                        &out_flags,
                                        &expiry);

        if (out_buf.cbBuffer > 0 && out_buf.pvBuffer) {
            bool sent = tlsx_send_all_raw(sock->socket_fd, (const uint8_t*)out_buf.pvBuffer, (int)out_buf.cbBuffer);
            FreeContextBuffer(out_buf.pvBuffer);
            out_buf.pvBuffer = NULL;
            out_buf.cbBuffer = 0;
            if (!sent) {
                if (in_buf) free(in_buf);
                if (out_err) *out_err = "Failed to send TLS handshake token";
                return false;
            }
        }

        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            continue;
        }

        if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) {
            if (in_buffers[1].BufferType == SECBUFFER_EXTRA && in_buffers[1].cbBuffer > 0) {
                int extra = (int)in_buffers[1].cbBuffer;
                if (extra > in_len) extra = in_len;
                memmove(in_buf, in_buf + (in_len - extra), (size_t)extra);
                in_len = extra;
            } else {
                in_len = 0;
            }
            continue;
        }

        if (in_buf) free(in_buf);
        if (out_err) *out_err = "TLS handshake failed";
        return false;
    }

    if (ss != SEC_E_OK) {
        if (in_buf) free(in_buf);
        if (out_err) *out_err = "TLS handshake did not complete";
        return false;
    }

    if (in_len > 0) {
        if (!tlsx_ctx_append_encrypted(ctx, in_buf, in_len)) {
            if (in_buf) free(in_buf);
            if (out_err) *out_err = "TLS buffer allocation failed";
            return false;
        }
    }
    if (in_buf) free(in_buf);

    ss = QueryContextAttributesA(&ctx->ctx, SECPKG_ATTR_STREAM_SIZES, &ctx->sizes);
    if (ss != SEC_E_OK) {
        if (out_err) *out_err = "Failed to query TLS stream sizes";
        return false;
    }
    ctx->sizes_ready = true;
    return true;
}

static int tlsx_transport_send(ObjSocket* sock, const char* data, int len) {
    if (!sock || sock->socket_fd < 0 || !data || len < 0) return -1;
    if (len == 0) return 0;
    TlsxTransportCtx* ctx = (TlsxTransportCtx*)sock->transport_ctx;
    if (!ctx || !ctx->sizes_ready) return -1;

    int max_msg = (int)ctx->sizes.cbMaximumMessage;
    if (max_msg <= 0) max_msg = 16384;

    int sent_plain = 0;
    while (sent_plain < len) {
        int chunk = len - sent_plain;
        if (chunk > max_msg) chunk = max_msg;

        int packet_cap = (int)ctx->sizes.cbHeader + chunk + (int)ctx->sizes.cbTrailer;
        uint8_t* packet = (uint8_t*)malloc((size_t)packet_cap);
        if (!packet) {
            return sent_plain > 0 ? sent_plain : -1;
        }
        memset(packet, 0, (size_t)packet_cap);

        memcpy(packet + ctx->sizes.cbHeader, data + sent_plain, (size_t)chunk);

        SecBuffer buffers[4];
        memset(buffers, 0, sizeof(buffers));
        buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
        buffers[0].pvBuffer = packet;
        buffers[0].cbBuffer = ctx->sizes.cbHeader;
        buffers[1].BufferType = SECBUFFER_DATA;
        buffers[1].pvBuffer = packet + ctx->sizes.cbHeader;
        buffers[1].cbBuffer = (unsigned long)chunk;
        buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
        buffers[2].pvBuffer = packet + ctx->sizes.cbHeader + chunk;
        buffers[2].cbBuffer = ctx->sizes.cbTrailer;
        buffers[3].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc desc;
        memset(&desc, 0, sizeof(desc));
        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers = 4;
        desc.pBuffers = buffers;

        SECURITY_STATUS ss = EncryptMessage(&ctx->ctx, 0, &desc, 0);
        if (ss != SEC_E_OK) {
            free(packet);
            return sent_plain > 0 ? sent_plain : -1;
        }

        int packet_len = (int)(buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer);
        bool ok = tlsx_send_all_raw(sock->socket_fd, packet, packet_len);
        free(packet);
        if (!ok) {
            return sent_plain > 0 ? sent_plain : -1;
        }

        sent_plain += chunk;
    }

    return sent_plain;
}

static int tlsx_transport_receive(ObjSocket* sock, char* out, int max_len) {
    if (!sock || sock->socket_fd < 0 || !out || max_len <= 0) return -1;
    TlsxTransportCtx* ctx = (TlsxTransportCtx*)sock->transport_ctx;
    if (!ctx || !ctx->sizes_ready) return -1;

    int pending = tlsx_ctx_take_plain_pending(ctx, out, max_len);
    if (pending > 0) return pending;

    while (true) {
        if (ctx->enc_len <= 0) {
            uint8_t chunk[8192];
            int n = recv(sock->socket_fd, (char*)chunk, (int)sizeof(chunk), 0);
            if (n == 0) return 0;
            if (n < 0) return -1;
            if (!tlsx_ctx_append_encrypted(ctx, chunk, n)) return -1;
        }

        SecBuffer buffers[4];
        memset(buffers, 0, sizeof(buffers));
        buffers[0].BufferType = SECBUFFER_DATA;
        buffers[0].pvBuffer = ctx->enc_buf;
        buffers[0].cbBuffer = (unsigned long)ctx->enc_len;
        buffers[1].BufferType = SECBUFFER_EMPTY;
        buffers[2].BufferType = SECBUFFER_EMPTY;
        buffers[3].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc desc;
        memset(&desc, 0, sizeof(desc));
        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers = 4;
        desc.pBuffers = buffers;

        SECURITY_STATUS ss = DecryptMessage(&ctx->ctx, &desc, 0, NULL);
        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            uint8_t chunk[8192];
            int n = recv(sock->socket_fd, (char*)chunk, (int)sizeof(chunk), 0);
            if (n == 0) return 0;
            if (n < 0) return -1;
            if (!tlsx_ctx_append_encrypted(ctx, chunk, n)) return -1;
            continue;
        }
        if (ss == SEC_I_CONTEXT_EXPIRED) {
            return 0;
        }
        if (ss == SEC_I_RENEGOTIATE) {
            return -1;
        }
        if (ss != SEC_E_OK) {
            return -1;
        }

        SecBuffer* data_buf = NULL;
        SecBuffer* extra_buf = NULL;
        for (int i = 0; i < 4; i++) {
            if (buffers[i].BufferType == SECBUFFER_DATA) data_buf = &buffers[i];
            if (buffers[i].BufferType == SECBUFFER_EXTRA) extra_buf = &buffers[i];
        }

        if (extra_buf && extra_buf->cbBuffer > 0) {
            int extra = (int)extra_buf->cbBuffer;
            if (extra > ctx->enc_len) extra = ctx->enc_len;
            memmove(ctx->enc_buf, ctx->enc_buf + (ctx->enc_len - extra), (size_t)extra);
            ctx->enc_len = extra;
        } else {
            ctx->enc_len = 0;
        }

        if (data_buf && data_buf->pvBuffer && data_buf->cbBuffer > 0) {
            int produced = (int)data_buf->cbBuffer;
            int to_copy = produced < max_len ? produced : max_len;
            memcpy(out, data_buf->pvBuffer, (size_t)to_copy);
            int remain = produced - to_copy;
            if (remain > 0) {
                const uint8_t* remain_ptr = (const uint8_t*)data_buf->pvBuffer + to_copy;
                if (!tlsx_ctx_set_plain_pending(ctx, remain_ptr, remain)) {
                    return to_copy > 0 ? to_copy : -1;
                }
            }
            return to_copy;
        }
    }
}

static void tlsx_transport_close(ObjSocket* sock) {
    if (!sock) return;
    TlsxTransportCtx* ctx = (TlsxTransportCtx*)sock->transport_ctx;
    if (ctx) {
        tlsx_ctx_reset(ctx);
        free(ctx);
    }
    sock->transport_ctx = NULL;
    sock->transport_send = NULL;
    sock->transport_recv = NULL;
    sock->transport_close = NULL;
    if (sock->socket_fd >= 0) {
        CLOSESOCKET(sock->socket_fd);
        sock->socket_fd = -1;
    }
    sock->is_connected = false;
}
#endif

static bool httpx_connect_socket(VM* vm,
                                 const char* host,
                                 int port,
                                 int timeout_ms,
                                 bool is_https,
                                 bool tls_insecure_skip_verify,
                                 ObjSocket** out_socket,
                                 int64_t* err_code,
                                 const char** err_msg) {
    if (out_socket) *out_socket = NULL;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!out_socket) return false;

#ifndef _WIN32
    if (is_https) {
        if (err_code) *err_code = ERR_UNSUPPORTED;
        if (err_msg) *err_msg = "HTTPS is not supported on this build";
        return false;
    }
#endif

    int sock_fd = -1;
    int64_t connect_err_code = 0;
    const char* connect_err_msg = NULL;
    if (!tcpx_connect_host_port(vm,
                                host,
                                port,
                                timeout_ms,
                                &sock_fd,
                                &connect_err_code,
                                &connect_err_msg)) {
        if (err_code) *err_code = connect_err_code ? connect_err_code : ERR_NETWORK;
        if (err_msg) *err_msg = connect_err_msg ? connect_err_msg : "Connection failed";
        return false;
    }

    ObjSocket* sock_obj = obj_socket_create(vm, sock_fd, true);
    if (!sock_obj) {
        CLOSESOCKET(sock_fd);
        vm_release_socket_handle(vm);
        if (err_code) *err_code = ERR_INTERNAL;
        if (err_msg) *err_msg = "Failed to allocate socket";
        return false;
    }

    if (is_https) {
#ifdef _WIN32
        TlsxTransportCtx* tls_ctx = (TlsxTransportCtx*)calloc(1, sizeof(TlsxTransportCtx));
        if (!tls_ctx) {
            obj_socket_free(sock_obj);
            if (err_code) *err_code = ERR_INTERNAL;
            if (err_msg) *err_msg = "Failed to allocate TLS context";
            return false;
        }

        const char* tls_err = NULL;
        if (!tlsx_handshake_client(sock_obj, host, tls_ctx, tls_insecure_skip_verify, &tls_err)) {
            tlsx_ctx_reset(tls_ctx);
            free(tls_ctx);
            obj_socket_free(sock_obj);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = tls_err ? tls_err : "TLS handshake failed";
            return false;
        }

        sock_obj->transport_ctx = tls_ctx;
        sock_obj->transport_send = tlsx_transport_send;
        sock_obj->transport_recv = tlsx_transport_receive;
        sock_obj->transport_close = tlsx_transport_close;
        sock_obj->is_connected = true;
#else
        obj_socket_free(sock_obj);
        if (err_code) *err_code = ERR_UNSUPPORTED;
        if (err_msg) *err_msg = "HTTPS is not supported on this build";
        return false;
#endif
    }

    *out_socket = sock_obj;
    return true;
}

static bool text_equals_ignore_case(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void trim_bounds(const char** start, const char** end) {
    if (!start || !end || !*start || !*end) return;
    while (*start < *end && isspace((unsigned char)**start)) (*start)++;
    while (*end > *start && isspace((unsigned char)*((*end) - 1))) (*end)--;
}

static bool parse_size_t_decimal(const char* start, const char* end, size_t* out) {
    if (out) *out = 0;
    if (!start || !end || !out || start >= end) return false;

    size_t value = 0;
    for (const char* p = start; p < end; p++) {
        if (*p < '0' || *p > '9') return false;
        size_t digit = (size_t)(*p - '0');
        if (value > (SIZE_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

static size_t http_find_header_end(const char* data, size_t len, size_t* out_delim_len) {
    if (out_delim_len) *out_delim_len = 0;
    if (!data || len == 0) return SIZE_MAX;

    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            if (out_delim_len) *out_delim_len = 4;
            return i;
        }
    }

    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] == '\n' && data[i + 1] == '\n') {
            if (out_delim_len) *out_delim_len = 2;
            return i;
        }
    }

    return SIZE_MAX;
}

static bool http_request_content_length(const char* raw,
                                        size_t header_len,
                                        size_t* out_content_length,
                                        int64_t* err_code,
                                        const char** err_msg) {
    if (out_content_length) *out_content_length = 0;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!raw || !out_content_length) return false;

    bool seen_content_length = false;
    size_t content_length = 0;

    const char* section_end = raw + header_len;
    const char* line_start = raw;
    const char* first_nl = memchr(line_start, '\n', header_len);
    if (first_nl) {
        line_start = first_nl + 1;
    } else {
        line_start = section_end;
    }

    while (line_start < section_end) {
        const char* nl = memchr(line_start, '\n', (size_t)(section_end - line_start));
        const char* line_end = nl ? nl : section_end;
        const char* logical_end = line_end;
        if (logical_end > line_start && logical_end[-1] == '\r') logical_end--;

        if (logical_end > line_start) {
            const char* colon = memchr(line_start, ':', (size_t)(logical_end - line_start));
            if (colon) {
                const char* key_start = line_start;
                const char* key_end = colon;
                const char* value_start = colon + 1;
                const char* value_end = logical_end;
                trim_bounds(&key_start, &key_end);
                trim_bounds(&value_start, &value_end);

                char* key = httpx_trim_copy(key_start, key_end);
                if (text_equals_ignore_case(key, "Content-Length")) {
                    size_t parsed = 0;
                    bool ok = parse_size_t_decimal(value_start, value_end, &parsed);
                    if (!ok) {
                        free(key);
                        if (err_code) *err_code = ERR_PARSE;
                        if (err_msg) *err_msg = "Invalid Content-Length header";
                        return false;
                    }
                    if (seen_content_length && parsed != content_length) {
                        free(key);
                        if (err_code) *err_code = ERR_PARSE;
                        if (err_msg) *err_msg = "Conflicting Content-Length headers";
                        return false;
                    }
                    seen_content_length = true;
                    content_length = parsed;
                }
                free(key);
            }
        }

        line_start = nl ? (nl + 1) : section_end;
    }

    *out_content_length = content_length;
    return true;
}

static bool tcp_bind_and_listen(VM* vm,
                                const char* host,
                                int port,
                                int* out_sock,
                                const char** out_err) {
    if (out_sock) *out_sock = -1;
    if (out_err) *out_err = NULL;
    if (!out_sock) return false;

    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const char* bind_host = host;
    if (!bind_host || bind_host[0] == '\0' || strcmp(bind_host, "*") == 0) {
        hints.ai_flags = AI_PASSIVE;
        bind_host = NULL;
    }

    struct addrinfo* result = NULL;
    int gai = getaddrinfo(bind_host, port_buf, &hints, &result);
    if (gai != 0 || !result) {
        if (out_err) *out_err = "Host resolution failed";
        return false;
    }

    int listen_sock = -1;
    bool ok = false;
    for (struct addrinfo* ai = result; ai; ai = ai->ai_next) {
        if (!vm_try_acquire_socket_handle(vm)) {
            if (out_err) *out_err = "Open socket limit exceeded";
            break;
        }

        listen_sock = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (listen_sock < 0) {
            vm_release_socket_handle(vm);
            continue;
        }

        int reuse = 1;
#ifdef _WIN32
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#else
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

#ifdef _WIN32
        int addr_len = (int)ai->ai_addrlen;
#else
        socklen_t addr_len = (socklen_t)ai->ai_addrlen;
#endif

        if (bind(listen_sock, ai->ai_addr, addr_len) == 0 && listen(listen_sock, 16) == 0) {
            ok = true;
            break;
        }

        CLOSESOCKET(listen_sock);
        vm_release_socket_handle(vm);
        listen_sock = -1;
    }

    freeaddrinfo(result);

    if (!ok) {
        if (out_err) *out_err = "Failed to listen on socket";
        return false;
    }

    *out_sock = listen_sock;
    return true;
}

static uint64_t tcp_now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
#endif
}

static bool tcp_set_nonblocking(int sock, bool enabled, int* out_posix_flags) {
#ifdef _WIN32
    (void)out_posix_flags;
    u_long mode = enabled ? 1UL : 0UL;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return false;
    if (out_posix_flags) *out_posix_flags = flags;
    int next_flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(sock, F_SETFL, next_flags) == 0;
#endif
}

static bool tcp_err_is_would_block(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static bool tcp_accept_with_timeout(int listen_sock,
                                    int timeout_ms,
                                    int* out_client_sock,
                                    int64_t* err_code,
                                    const char** err_msg) {
    if (out_client_sock) *out_client_sock = -1;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!out_client_sock) return false;

    int old_flags = 0;
    if (!tcp_set_nonblocking(listen_sock, true, &old_flags)) {
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Failed to configure listener socket";
        return false;
    }

    uint64_t deadline = tcp_now_ms() + (uint64_t)timeout_ms;
    int accepted = -1;
    while (true) {
        accepted = (int)accept(listen_sock, NULL, NULL);
        if (accepted >= 0) break;

#ifdef _WIN32
        if (!tcp_err_is_would_block()) {
            tcp_set_nonblocking(listen_sock, false, NULL);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Accept failed";
            return false;
        }
#else
        if (errno == EINTR) {
            continue;
        }
        if (!tcp_err_is_would_block()) {
            fcntl(listen_sock, F_SETFL, old_flags);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Accept failed";
            return false;
        }
#endif

        if (tcp_now_ms() >= deadline) {
#ifdef _WIN32
            tcp_set_nonblocking(listen_sock, false, NULL);
#else
            fcntl(listen_sock, F_SETFL, old_flags);
#endif
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Accept timed out";
            return false;
        }

        uint64_t now_ms = tcp_now_ms();
        int remaining_ms = (int)(deadline - now_ms);
        if (remaining_ms <= 0) {
#ifdef _WIN32
            tcp_set_nonblocking(listen_sock, false, NULL);
#else
            fcntl(listen_sock, F_SETFL, old_flags);
#endif
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Accept timed out";
            return false;
        }

        fd_set read_set;
        FD_ZERO(&read_set);
#ifdef _WIN32
        FD_SET((SOCKET)listen_sock, &read_set);
#else
        FD_SET(listen_sock, &read_set);
#endif

        struct timeval wait_tv;
        wait_tv.tv_sec = remaining_ms / 1000;
        wait_tv.tv_usec = (remaining_ms % 1000) * 1000;

#ifdef _WIN32
        int wait_rc = select(0, &read_set, NULL, NULL, &wait_tv);
        if (wait_rc == SOCKET_ERROR) {
            tcp_set_nonblocking(listen_sock, false, NULL);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Accept wait failed";
            return false;
        }
#else
        int wait_rc = select(listen_sock + 1, &read_set, NULL, NULL, &wait_tv);
        if (wait_rc < 0) {
            if (errno == EINTR) continue;
            fcntl(listen_sock, F_SETFL, old_flags);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Accept wait failed";
            return false;
        }
#endif
        if (wait_rc == 0) {
#ifdef _WIN32
            tcp_set_nonblocking(listen_sock, false, NULL);
#else
            fcntl(listen_sock, F_SETFL, old_flags);
#endif
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Accept timed out";
            return false;
        }
    }

#ifdef _WIN32
    tcp_set_nonblocking(listen_sock, false, NULL);
#else
    fcntl(listen_sock, F_SETFL, old_flags);
#endif

    // Accept may inherit nonblocking mode from listener on some platforms.
    // Force blocking semantics so tcpReceive/httpReadRequest behave consistently.
    if (!tcp_set_nonblocking(accepted, false, NULL)) {
        CLOSESOCKET(accepted);
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Failed to configure accepted socket";
        return false;
    }

    // Bound accepted socket I/O to avoid hanging forever on partial/stalled clients.
#ifdef _WIN32
    DWORD accepted_timeout = 10000;
    int r1 = setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO, (const char*)&accepted_timeout, sizeof(accepted_timeout));
    int r2 = setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO, (const char*)&accepted_timeout, sizeof(accepted_timeout));
    if (r1 != 0 || r2 != 0) {
        CLOSESOCKET(accepted);
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Failed to configure accepted socket timeouts";
        return false;
    }
#else
    struct timeval accepted_tv;
    accepted_tv.tv_sec = 10;
    accepted_tv.tv_usec = 0;
    int r1 = setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO, (const char*)&accepted_tv, sizeof(accepted_tv));
    int r2 = setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO, (const char*)&accepted_tv, sizeof(accepted_tv));
    if (r1 != 0 || r2 != 0) {
        CLOSESOCKET(accepted);
        if (err_code) *err_code = ERR_NETWORK;
        if (err_msg) *err_msg = "Failed to configure accepted socket timeouts";
        return false;
    }
#endif

    *out_client_sock = accepted;
    return true;
}

static bool http_read_raw_request(int sock,
                                  int max_bytes,
                                  char** out_raw,
                                  size_t* out_len,
                                  int64_t* err_code,
                                  const char** err_msg) {
    if (out_raw) *out_raw = NULL;
    if (out_len) *out_len = 0;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!out_raw || !out_len) return false;

    size_t limit = (size_t)max_bytes;
    size_t cap = limit < 4096 ? limit : 4096;
    if (cap < 256) cap = limit;
    if (cap == 0) cap = 256;

    char* buf = (char*)safe_malloc(cap + 1);
    size_t len = 0;
    size_t required_total = SIZE_MAX;

    while (true) {
        if (required_total != SIZE_MAX && len >= required_total) break;
        if (len >= limit) {
            free(buf);
            if (err_code) *err_code = ERR_LIMIT;
            if (err_msg) *err_msg = "HTTP request exceeds maxBytes";
            return false;
        }

        char chunk[4096];
        size_t remaining = limit - len;
        int recv_cap = remaining > sizeof(chunk) ? (int)sizeof(chunk) : (int)remaining;
        int n = recv(sock, chunk, recv_cap, 0);
        if (n > 0) {
            if (len + (size_t)n + 1 > cap) {
                while (len + (size_t)n + 1 > cap) cap *= 2;
                if (cap > limit) cap = limit;
                if (len + (size_t)n + 1 > cap) {
                    free(buf);
                    if (err_code) *err_code = ERR_LIMIT;
                    if (err_msg) *err_msg = "HTTP request exceeds maxBytes";
                    return false;
                }
                buf = (char*)safe_realloc(buf, cap + 1);
            }
            memcpy(buf + len, chunk, (size_t)n);
            len += (size_t)n;
        } else if (n == 0) {
            if (required_total != SIZE_MAX && len >= required_total) break;
            free(buf);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Connection closed before full request";
            return false;
        } else {
            free(buf);
            if (err_code) *err_code = ERR_NETWORK;
            if (err_msg) *err_msg = "Failed to receive request";
            return false;
        }

        if (required_total == SIZE_MAX) {
            size_t delim_len = 0;
            size_t header_end = http_find_header_end(buf, len, &delim_len);
            if (header_end != SIZE_MAX) {
                size_t content_length = 0;
                int64_t parse_err_code = 0;
                const char* parse_err_msg = NULL;
                bool ok = http_request_content_length(buf,
                                                      header_end,
                                                      &content_length,
                                                      &parse_err_code,
                                                      &parse_err_msg);
                if (!ok) {
                    free(buf);
                    if (err_code) *err_code = parse_err_code ? parse_err_code : ERR_PARSE;
                    if (err_msg) *err_msg = parse_err_msg ? parse_err_msg : "Invalid HTTP headers";
                    return false;
                }

                if (header_end > SIZE_MAX - delim_len || header_end + delim_len > SIZE_MAX - content_length) {
                    free(buf);
                    if (err_code) *err_code = ERR_LIMIT;
                    if (err_msg) *err_msg = "HTTP request exceeds maxBytes";
                    return false;
                }

                required_total = header_end + delim_len + content_length;
                if (required_total > limit) {
                    free(buf);
                    if (err_code) *err_code = ERR_LIMIT;
                    if (err_msg) *err_msg = "HTTP request exceeds maxBytes";
                    return false;
                }
            }
        }
    }

    buf[len] = '\0';
    *out_raw = buf;
    *out_len = len;
    return true;
}

static bool http_parse_request_value(VM* vm,
                                     const char* raw,
                                     size_t raw_len,
                                     Value* out,
                                     int64_t* err_code,
                                     const char** err_msg) {
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!raw || !out) return false;

    size_t delim_len = 0;
    size_t header_end = http_find_header_end(raw, raw_len, &delim_len);
    if (header_end == SIZE_MAX) {
        if (err_code) *err_code = ERR_PARSE;
        if (err_msg) *err_msg = "Malformed HTTP request";
        return false;
    }

    const char* header_limit = raw + header_end;
    const char* first_nl = memchr(raw, '\n', header_end);
    if (!first_nl) {
        if (err_code) *err_code = ERR_PARSE;
        if (err_msg) *err_msg = "Malformed HTTP request line";
        return false;
    }
    const char* request_line_end = first_nl;
    if (request_line_end > raw && request_line_end[-1] == '\r') request_line_end--;

    const char* sp1 = memchr(raw, ' ', (size_t)(request_line_end - raw));
    if (!sp1) {
        if (err_code) *err_code = ERR_PARSE;
        if (err_msg) *err_msg = "Malformed HTTP request line";
        return false;
    }
    const char* sp2 = memchr(sp1 + 1, ' ', (size_t)(request_line_end - (sp1 + 1)));
    if (!sp2) {
        if (err_code) *err_code = ERR_PARSE;
        if (err_msg) *err_msg = "Malformed HTTP request line";
        return false;
    }

    char* method = httpx_trim_copy(raw, sp1);
    char* path = httpx_trim_copy(sp1 + 1, sp2);
    char* version = httpx_trim_copy(sp2 + 1, request_line_end);
    if (method[0] == '\0' || path[0] == '\0' || version[0] == '\0') {
        free(method);
        free(path);
        free(version);
        if (err_code) *err_code = ERR_PARSE;
        if (err_msg) *err_msg = "Malformed HTTP request line";
        return false;
    }

    ObjMap* headers = obj_map_create(vm);
    const char* line_start = first_nl + 1;
    while (line_start < header_limit) {
        const char* nl = memchr(line_start, '\n', (size_t)(header_limit - line_start));
        const char* line_end = nl ? nl : header_limit;
        const char* logical_end = line_end;
        if (logical_end > line_start && logical_end[-1] == '\r') logical_end--;

        if (logical_end > line_start) {
            const char* colon = memchr(line_start, ':', (size_t)(logical_end - line_start));
            if (colon) {
                const char* key_start = line_start;
                const char* key_end = colon;
                const char* value_start = colon + 1;
                const char* value_end = logical_end;
                trim_bounds(&key_start, &key_end);
                trim_bounds(&value_start, &value_end);

                char* value = httpx_trim_copy(value_start, value_end);
                if (key_end > key_start) {
                    Value v;
                    value_init_string(&v, value);
                    obj_map_set_cstr_n(headers, key_start, (int)(key_end - key_start), v);
                    value_free(&v);
                }
                free(value);
            }
        }

        line_start = nl ? (nl + 1) : header_limit;
    }

    const char* body_start = raw + header_end + delim_len;
    const char* request_end = raw + raw_len;
    if (body_start > request_end) body_start = request_end;
    size_t body_len = (size_t)(request_end - body_start);
    if (vm && vm->config.max_string_length > 0 && (int64_t)body_len > (int64_t)vm->config.max_string_length) {
        obj_map_free(headers);
        free(method);
        free(path);
        free(version);
        if (err_code) *err_code = ERR_LIMIT;
        if (err_msg) *err_msg = "HTTP body exceeds max string length";
        return false;
    }

    char* body = (char*)safe_malloc(body_len + 1);
    if (body_len > 0) memcpy(body, body_start, body_len);
    body[body_len] = '\0';

    ObjMap* req = obj_map_create(vm);
    Value val;

    value_init_string(&val, method);
    obj_map_set_cstr(req, "method", val);
    value_free(&val);

    value_init_string(&val, path);
    obj_map_set_cstr(req, "path", val);
    value_free(&val);

    value_init_string(&val, version);
    obj_map_set_cstr(req, "version", val);
    value_free(&val);

    value_init_map(&val, headers);
    obj_map_set_cstr(req, "headers", val);
    value_free(&val);

    value_init_string(&val, body);
    obj_map_set_cstr(req, "body", val);
    value_free(&val);

    Value out_value;
    value_init_map(&out_value, req);
    *out = out_value;

    free(method);
    free(path);
    free(version);
    free(body);
    return true;
}

static const char* http_reason_phrase(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "OK";
    }
}

static bool http_build_response_wire(int status_code,
                                     const char* body,
                                     size_t body_len,
                                     ObjMap* headers,
                                     char** out_wire,
                                     size_t* out_wire_len,
                                     int64_t* err_code,
                                     const char** err_msg) {
    if (out_wire) *out_wire = NULL;
    if (out_wire_len) *out_wire_len = 0;
    if (err_code) *err_code = 0;
    if (err_msg) *err_msg = NULL;
    if (!out_wire || !out_wire_len) return false;

    const char* reason = http_reason_phrase(status_code);
    const uint8_t* body_bytes = (const uint8_t*)(body ? body : "");
    size_t wire_body_len = body_len;
    uint8_t* encoded_body = NULL;

    bool has_content_type = false;
    bool gzip_encode_body = false;
    size_t custom_headers_len = 0;
    if (headers && headers->slots) {
        for (int i = 0; i < headers->capacity; i++) {
            MapSlot* slot = &headers->slots[i];
            if (slot->hash < 2) continue;
            ObjString* key_str = value_get_string_obj(&slot->key);
            ObjString* value_str = value_get_string_obj(&slot->value);
            if (value_get_type(&slot->key) != VAL_STRING ||
                value_get_type(&slot->value) != VAL_STRING ||
                !key_str ||
                !value_str) {
                if (err_code) *err_code = ERR_INVALID_ARGUMENT;
                if (err_msg) *err_msg = "headers must be map<string, string>";
                return false;
            }

            const char* key = key_str->chars;
            const char* value = value_str->chars;
            if (!httpx_header_name_is_safe(key) || !httpx_header_value_is_safe(value)) {
                if (err_code) *err_code = ERR_INVALID_ARGUMENT;
                if (err_msg) *err_msg = "headers contain invalid characters";
                return false;
            }
            if (text_equals_ignore_case(key, "Content-Length") || text_equals_ignore_case(key, "Connection")) {
                continue;
            }
            if (text_equals_ignore_case(key, "Content-Type")) {
                has_content_type = true;
            }
            if (text_equals_ignore_case(key, "Content-Encoding") &&
                httpx_header_value_single_token_ci(value, "gzip")) {
                gzip_encode_body = true;
            }
            custom_headers_len += strlen(key) + 2 + strlen(value) + 2;
        }
    }

    if (gzip_encode_body) {
        const char* gzip_err = NULL;
        if (!gzip_codec_compress(body_bytes, body_len, &encoded_body, &wire_body_len, &gzip_err)) {
            if (err_code) *err_code = ERR_INTERNAL;
            if (err_msg) *err_msg = gzip_err ? gzip_err : "Failed to gzip HTTP response";
            return false;
        }
        body_bytes = encoded_body;
    }

    size_t total = 0;
    total += strlen("HTTP/1.1 ") + 3 + 1 + strlen(reason) + 2;
    total += strlen("Content-Length: ") + 20 + 2;
    total += strlen("Connection: close\r\n");
    if (!has_content_type) {
        total += strlen("Content-Type: text/plain; charset=utf-8\r\n");
    }
    total += custom_headers_len;
    total += 2;
    total += wire_body_len;

    char* wire = (char*)safe_malloc(total + 1);
    size_t pos = 0;
    if (!httpx_appendf(wire, total, &pos, "HTTP/1.1 %d %s\r\n", status_code, reason) ||
        !httpx_appendf(wire, total, &pos, "Content-Length: %zu\r\n", wire_body_len) ||
        !httpx_appendf(wire, total, &pos, "Connection: close\r\n")) {
        free(encoded_body);
        free(wire);
        if (err_code) *err_code = ERR_INTERNAL;
        if (err_msg) *err_msg = "Failed to build HTTP response";
        return false;
    }

    if (!has_content_type) {
        if (!httpx_appendf(wire, total, &pos, "Content-Type: text/plain; charset=utf-8\r\n")) {
            free(encoded_body);
            free(wire);
            if (err_code) *err_code = ERR_INTERNAL;
            if (err_msg) *err_msg = "Failed to build HTTP response";
            return false;
        }
    }

    if (headers && headers->slots) {
        for (int i = 0; i < headers->capacity; i++) {
            MapSlot* slot = &headers->slots[i];
            if (slot->hash < 2) continue;
            ObjString* key_str = value_get_string_obj(&slot->key);
            ObjString* value_str = value_get_string_obj(&slot->value);
            if (value_get_type(&slot->key) != VAL_STRING ||
                value_get_type(&slot->value) != VAL_STRING ||
                !key_str ||
                !value_str) {
                continue;
            }
            const char* key = key_str->chars;
            const char* value = value_str->chars;
            if (!httpx_header_name_is_safe(key) || !httpx_header_value_is_safe(value)) {
                free(encoded_body);
                free(wire);
                if (err_code) *err_code = ERR_INVALID_ARGUMENT;
                if (err_msg) *err_msg = "headers contain invalid characters";
                return false;
            }
            if (text_equals_ignore_case(key, "Content-Length") || text_equals_ignore_case(key, "Connection")) {
                continue;
            }
            if (!httpx_appendf(wire, total, &pos, "%s: %s\r\n", key, value)) {
                free(encoded_body);
                free(wire);
                if (err_code) *err_code = ERR_INTERNAL;
                if (err_msg) *err_msg = "Failed to build HTTP response";
                return false;
            }
        }
    }

    if (!httpx_appendf(wire, total, &pos, "\r\n")) {
        free(encoded_body);
        free(wire);
        if (err_code) *err_code = ERR_INTERNAL;
        if (err_msg) *err_msg = "Failed to build HTTP response";
        return false;
    }
    if (wire_body_len > 0) {
        if (pos + wire_body_len > total) {
            free(encoded_body);
            free(wire);
            if (err_code) *err_code = ERR_INTERNAL;
            if (err_msg) *err_msg = "Failed to build HTTP response";
            return false;
        }
        memcpy(wire + pos, body_bytes, wire_body_len);
        pos += wire_body_len;
    }

    wire[pos] = '\0';
    free(encoded_body);
    *out_wire = wire;
    *out_wire_len = pos;
    return true;
}

bool tablo_http_fuzz_parse_request(VM* vm, const char* raw, size_t raw_len) {
    if (!vm || !raw) return false;

    Value out;
    value_init_nil(&out);
    int64_t err_code = 0;
    const char* err_msg = NULL;
    bool ok = http_parse_request_value(vm, raw, raw_len, &out, &err_code, &err_msg);
    (void)err_code;
    (void)err_msg;
    if (ok) {
        value_free(&out);
    }
    return ok;
}

bool tablo_http_fuzz_parse_response(VM* vm, const char* raw, size_t raw_len) {
    if (!vm || !raw) return false;

    HttpxResponse out;
    httpx_response_init(&out);
    int64_t err_code = 0;
    const char* err_msg = NULL;
    bool ok = httpx_parse_response(vm, raw, raw_len, &out, &err_code, &err_msg);
    if (ok) {
        int64_t content_length = -1;
        (void)httpx_parse_content_length_header(out.headers, &content_length, &err_code, &err_msg);
    }
    (void)err_code;
    (void)err_msg;
    httpx_response_free(&out);
    return ok;
}

bool tablo_http_fuzz_parse_chunked_body(VM* vm, const char* raw, size_t raw_len) {
    if (!vm || !raw) return false;

    HttpxFuzzBufferReader reader;
    char* body = NULL;
    size_t body_len = 0;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    bool ok;

    reader.data = raw;
    reader.len = raw_len;
    reader.pos = 0;

    ok = httpx_recv_chunked_body_reader(vm,
                                        httpx_fuzz_buffer_recv_adapter,
                                        &reader,
                                        &body,
                                        &body_len,
                                        &err_code,
                                        &err_msg);
    (void)body_len;
    (void)err_code;
    (void)err_msg;
    free(body);
    return ok;
}

void builtin_tcp_listen(VM* vm) {
    Value* host_slot = &vm->stack.values[vm->stack.count - 2];
    Value port_val = vm->stack.values[vm->stack.count - 1];
    Value host_val = *host_slot;

    if (!vm_is_network_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, host_slot, ok, "Network access is disabled");
        return;
    }

    ObjString* host_str = value_get_string_obj(&host_val);
    if (value_get_type(&host_val) != VAL_STRING ||
        !host_str ||
        !host_str->chars ||
        value_get_type(&port_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, host_slot, ok, ERR_INVALID_ARGUMENT, "tcpListen expects (string, int)");
        return;
    }

    int64_t port64 = value_get_int(&port_val);
    if (port64 < 0 || port64 > 65535) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, host_slot, ok, ERR_INVALID_ARGUMENT, "port must be in range 0..65535");
        return;
    }

    int listen_sock = -1;
    const char* listen_err = NULL;
    if (!tcp_bind_and_listen(vm, host_str->chars, (int)port64, &listen_sock, &listen_err)) {
        Value ok;
        value_init_nil(&ok);
        int64_t err_code = (listen_err && strcmp(listen_err, "Open socket limit exceeded") == 0)
            ? ERR_LIMIT
            : ERR_NETWORK;
        result_tuple_set(vm, host_slot, ok, err_code, listen_err ? listen_err : "Failed to listen on socket");
        return;
    }

    ObjSocket* listener = obj_socket_create(vm, listen_sock, true);
    if (!listener) {
        CLOSESOCKET(listen_sock);
        vm_release_socket_handle(vm);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, host_slot, ok, ERR_INTERNAL, "Failed to allocate socket");
        return;
    }

    Value ok;
    value_init_socket(&ok, listener);
    result_tuple_set(vm, host_slot, ok, 0, NULL);
}

void builtin_tcp_accept(VM* vm) {
    Value* listener_slot = &vm->stack.values[vm->stack.count - 2];
    Value timeout_val = vm->stack.values[vm->stack.count - 1];
    Value listener_val = *listener_slot;

    if (!vm_is_network_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, listener_slot, ok, "Network access is disabled");
        return;
    }

    ObjSocket* listener = value_get_socket_obj(&listener_val);
    if (value_get_type(&listener_val) != VAL_SOCKET ||
        !listener ||
        value_get_type(&timeout_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, listener_slot, ok, ERR_INVALID_ARGUMENT, "tcpAccept expects (socket, int)");
        return;
    }

    if (listener->socket_fd < 0) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, listener_slot, ok, ERR_NETWORK, "Socket is closed");
        return;
    }

    int64_t timeout64 = value_get_int(&timeout_val);
    if (timeout64 < TCPX_MIN_TIMEOUT_MS || timeout64 > TCPX_MAX_TIMEOUT_MS) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, listener_slot, ok, ERR_INVALID_ARGUMENT, "timeoutMs must be between 0 and 120000");
        return;
    }

    int accepted = -1;
    int64_t accept_err_code = 0;
    const char* accept_err_msg = NULL;
    if (!vm_try_acquire_socket_handle(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, listener_slot, ok, ERR_LIMIT, "Open socket limit exceeded");
        return;
    }

    if (!tcp_accept_with_timeout(listener->socket_fd,
                                 (int)timeout64,
                                 &accepted,
                                 &accept_err_code,
                                 &accept_err_msg)) {
        vm_release_socket_handle(vm);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         listener_slot,
                         ok,
                         accept_err_code ? accept_err_code : ERR_NETWORK,
                         accept_err_msg ? accept_err_msg : "Accept failed");
        return;
    }

    ObjSocket* client = obj_socket_create(vm, accepted, true);
    if (!client) {
        CLOSESOCKET(accepted);
        vm_release_socket_handle(vm);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, listener_slot, ok, ERR_INTERNAL, "Failed to allocate socket");
        return;
    }

    Value ok;
    value_init_socket(&ok, client);
    result_tuple_set(vm, listener_slot, ok, 0, NULL);
}

void builtin_http_read_request(VM* vm) {
    Value* socket_slot = &vm->stack.values[vm->stack.count - 2];
    Value max_bytes_val = vm->stack.values[vm->stack.count - 1];
    Value socket_val = *socket_slot;

    if (!vm_is_network_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, socket_slot, ok, "Network access is disabled");
        return;
    }

    ObjSocket* sock = value_get_socket_obj(&socket_val);
    if (value_get_type(&socket_val) != VAL_SOCKET ||
        !sock ||
        value_get_type(&max_bytes_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, socket_slot, ok, ERR_INVALID_ARGUMENT, "httpReadRequest expects (socket, int)");
        return;
    }

    if (sock->socket_fd < 0) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, socket_slot, ok, ERR_NETWORK, "Socket is closed");
        return;
    }

    int64_t max_bytes64 = value_get_int(&max_bytes_val);
    if (max_bytes64 < 1 || max_bytes64 > HTTP_SERVER_MAX_REQUEST_BYTES) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         socket_slot,
                         ok,
                         ERR_INVALID_ARGUMENT,
                         "maxBytes must be between 1 and 8388608");
        return;
    }

    char* raw = NULL;
    size_t raw_len = 0;
    int64_t read_err_code = 0;
    const char* read_err_msg = NULL;
    if (!http_read_raw_request(sock->socket_fd,
                               (int)max_bytes64,
                               &raw,
                               &raw_len,
                               &read_err_code,
                               &read_err_msg)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         socket_slot,
                         ok,
                         read_err_code ? read_err_code : ERR_NETWORK,
                         read_err_msg ? read_err_msg : "Failed to read HTTP request");
        return;
    }

    Value req_value;
    int64_t parse_err_code = 0;
    const char* parse_err_msg = NULL;
    bool parsed = http_parse_request_value(vm, raw, raw_len, &req_value, &parse_err_code, &parse_err_msg);
    free(raw);
    if (!parsed) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         socket_slot,
                         ok,
                         parse_err_code ? parse_err_code : ERR_PARSE,
                         parse_err_msg ? parse_err_msg : "Malformed HTTP request");
        return;
    }

    result_tuple_set(vm, socket_slot, req_value, 0, NULL);
}

void builtin_http_write_response(VM* vm) {
    Value* socket_slot = &vm->stack.values[vm->stack.count - 4];
    Value status_val = vm->stack.values[vm->stack.count - 3];
    Value body_val = vm->stack.values[vm->stack.count - 2];
    Value headers_val = vm->stack.values[vm->stack.count - 1];
    Value socket_val = *socket_slot;

    if (!vm_is_network_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, socket_slot, ok, "Network access is disabled");
        return;
    }

    ObjSocket* sock = value_get_socket_obj(&socket_val);
    ObjString* body_str = value_get_string_obj(&body_val);
    ObjMap* headers_map = value_get_map_obj(&headers_val);
    ValueType headers_type = value_get_type(&headers_val);
    if (value_get_type(&socket_val) != VAL_SOCKET || !sock ||
        value_get_type(&status_val) != VAL_INT ||
        value_get_type(&body_val) != VAL_STRING || !body_str ||
        (headers_type != VAL_NIL && headers_type != VAL_MAP) ||
        (headers_type == VAL_MAP && !headers_map)) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         socket_slot,
                         ok,
                         ERR_INVALID_ARGUMENT,
                         "httpWriteResponse expects (socket, int, string, map<string, string>?)");
        return;
    }

    if (sock->socket_fd < 0) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, socket_slot, ok, ERR_NETWORK, "Socket is closed");
        return;
    }

    int64_t status64 = value_get_int(&status_val);
    if (status64 < 100 || status64 > 999) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, socket_slot, ok, ERR_INVALID_ARGUMENT, "statusCode must be in range 100..999");
        return;
    }

    ObjMap* headers = (headers_type == VAL_MAP) ? headers_map : NULL;
    char* wire = NULL;
    size_t wire_len = 0;
    int64_t build_err_code = 0;
    const char* build_err_msg = NULL;
    if (!http_build_response_wire((int)status64,
                                  body_str->chars ? body_str->chars : "",
                                  (size_t)body_str->length,
                                  headers,
                                  &wire,
                                  &wire_len,
                                  &build_err_code,
                                  &build_err_msg)) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         socket_slot,
                         ok,
                         build_err_code ? build_err_code : ERR_INVALID_ARGUMENT,
                         build_err_msg ? build_err_msg : "Failed to build HTTP response");
        return;
    }

    bool sent = httpx_send_all(sock->socket_fd, wire, wire_len);
    free(wire);
    if (!sent) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, socket_slot, ok, ERR_NETWORK, "Failed to send response");
        return;
    }

    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, socket_slot, ok, 0, NULL);
}

// TCP Connect
void builtin_tcp_connect(VM* vm) {
    // arg0 (host) at count-2, arg1 (port) at count-1
    Value* host_slot = &vm->stack.values[vm->stack.count - 2];
    Value port_val = vm->stack.values[vm->stack.count - 1];
    Value host_val = *host_slot;

    if (!vm_is_network_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, host_slot, ok, "Network access is disabled");
        return;
    }

    ObjString* host_str = value_get_string_obj(&host_val);
    if (value_get_type(&host_val) != VAL_STRING ||
        !host_str ||
        !host_str->chars ||
        value_get_type(&port_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, host_slot, ok, ERR_INVALID_ARGUMENT, "tcpConnect expects (string, int)");
        return;
    }

    const char* host = host_str->chars;
    int port = (int)value_get_int(&port_val);

    int sock_fd = -1;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    if (!tcpx_connect_host_port(vm, host, port, 10000, &sock_fd, &err_code, &err_msg)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         host_slot,
                         ok,
                         err_code ? err_code : ERR_NETWORK,
                         err_msg ? err_msg : "Connection failed");
        return;
    }

    // Create socket object
    ObjSocket* sock_obj = obj_socket_create(vm, sock_fd, true);
    if (!sock_obj) {
        CLOSESOCKET(sock_fd);
        vm_release_socket_handle(vm);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, host_slot, ok, ERR_INTERNAL, "Failed to allocate socket");
        return;
    }
    
    Value ok;
    value_init_socket(&ok, sock_obj);
    result_tuple_set(vm, host_slot, ok, 0, NULL);
}

// TCP Send
void builtin_tcp_send(VM* vm) {
    // arg0 (socket) at count-2, arg1 (data) at count-1
    Value* sock_slot = &vm->stack.values[vm->stack.count - 2];
    Value data_val = vm->stack.values[vm->stack.count - 1];
    Value sock_val = *sock_slot;

    if (!vm_is_network_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, sock_slot, ok, "Network access is disabled");
        return;
    }
    
    ObjSocket* sock_obj = value_get_socket_obj(&sock_val);
    ObjString* data_str = value_get_string_obj(&data_val);
    if (value_get_type(&sock_val) != VAL_SOCKET ||
        value_get_type(&data_val) != VAL_STRING ||
        !sock_obj ||
        !data_str ||
        !data_str->chars) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, sock_slot, ok, ERR_INVALID_ARGUMENT, "tcpSend expects (socket, string)");
        return;
    }
    
    if (sock_obj->socket_fd < 0) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, sock_slot, ok, ERR_NETWORK, "Socket is closed");
        return;
    }
    
    const char* data = data_str->chars;
    size_t len = data_str->length;

    if (len > (size_t)INT_MAX) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, sock_slot, ok, ERR_LIMIT, "tcpSend payload exceeds max size");
        return;
    }

    int sent = socket_send_dispatch(sock_obj, data, (int)len);
    if (sent < 0) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, sock_slot, ok, ERR_NETWORK, "Failed to send");
        return;
    }

    Value ok;
    value_init_int(&ok, sent);
    result_tuple_set(vm, sock_slot, ok, 0, NULL);
}

// TCP Receive
void builtin_tcp_receive(VM* vm) {
    // arg0 (socket) at count-2, arg1 (maxBytes) at count-1
    Value* sock_slot = &vm->stack.values[vm->stack.count - 2];
    Value max_bytes_val = vm->stack.values[vm->stack.count - 1];
    Value sock_val = *sock_slot;

    if (!vm_is_network_enabled(vm)) {
        Value ok;
        value_init_string(&ok, "");
        result_permission_denied(vm, sock_slot, ok, "Network access is disabled");
        return;
    }
    
    ObjSocket* sock_obj = value_get_socket_obj(&sock_val);
    if (value_get_type(&sock_val) != VAL_SOCKET || value_get_type(&max_bytes_val) != VAL_INT || !sock_obj) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, sock_slot, ok, ERR_INVALID_ARGUMENT, "tcpReceive expects (socket, int)");
        return;
    }
    
    if (sock_obj->socket_fd < 0) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, sock_slot, ok, ERR_NETWORK, "Socket is closed");
        return;
    }
    
    int max_bytes = (int)value_get_int(&max_bytes_val);
    if (max_bytes <= 0 || max_bytes > 65536) {
        max_bytes = 65536;
    }
    
    char* buffer = (char*)safe_malloc(max_bytes + 1);
    
    int received = socket_recv_dispatch(sock_obj, buffer, max_bytes);
    
    if (received < 0) {
        free(buffer);
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, sock_slot, ok, ERR_NETWORK, "Failed to receive");
        return;
    }
    
    if (received == 0) {
        // Connection closed
        free(buffer);
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, sock_slot, ok, ERR_NETWORK, "Connection closed");
        return;
    }
    
    buffer[received] = '\0';
    
    Value ok;
    value_init_string(&ok, buffer);
    free(buffer);
    
    result_tuple_set(vm, sock_slot, ok, 0, NULL);
}

// TCP Close
void builtin_tcp_close(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];

    Value input = *slot;
    ObjSocket* input_socket = value_get_socket_obj(&input);
    if (value_get_type(&input) == VAL_SOCKET && input_socket) {
        socket_close_dispatch(input_socket);
    }

    Value nil;
    value_init_nil(&nil);
    value_free(slot);
    *slot = nil;
}

void builtin_tls_is_available(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value out;
    if (!vm_is_network_enabled(vm)) {
        value_init_bool(&out, false);
        value_free(slot);
        *slot = out;
        return;
    }
#ifdef _WIN32
    value_init_bool(&out, true);
#else
    value_init_bool(&out, false);
#endif
    value_free(slot);
    *slot = out;
}

void builtin_tls_connect(VM* vm) {
    Value* host_slot = &vm->stack.values[vm->stack.count - 3];
    Value host_val = *host_slot;
    Value port_val = vm->stack.values[vm->stack.count - 2];
    Value timeout_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_network_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, host_slot, ok, "Network access is disabled");
        return;
    }

    ObjString* host_str = value_get_string_obj(&host_val);
    if (value_get_type(&host_val) != VAL_STRING || !host_str || !host_str->chars ||
        value_get_type(&port_val) != VAL_INT ||
        value_get_type(&timeout_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, host_slot, ok, ERR_INVALID_ARGUMENT, "tlsConnect expects (string, int, int)");
        return;
    }

    const char* host = host_str->chars;
    int64_t port64 = value_get_int(&port_val);
    int64_t timeout64 = value_get_int(&timeout_val);
    if (!host || host[0] == '\0') {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, host_slot, ok, ERR_INVALID_ARGUMENT, "host must be non-empty");
        return;
    }
    if (port64 < 1 || port64 > 65535) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, host_slot, ok, ERR_INVALID_ARGUMENT, "port must be in range 1..65535");
        return;
    }
    if (timeout64 < 1 || timeout64 > TCPX_MAX_TIMEOUT_MS) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, host_slot, ok, ERR_INVALID_ARGUMENT, "timeoutMs must be in range 1..120000");
        return;
    }

#ifdef _WIN32
    int sock_fd = -1;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    if (!tcpx_connect_host_port(vm, host, (int)port64, (int)timeout64, &sock_fd, &err_code, &err_msg)) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         host_slot,
                         ok,
                         err_code ? err_code : ERR_NETWORK,
                         err_msg ? err_msg : "Connection failed");
        return;
    }

    ObjSocket* sock_obj = obj_socket_create(vm, sock_fd, true);
    if (!sock_obj) {
        CLOSESOCKET(sock_fd);
        vm_release_socket_handle(vm);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, host_slot, ok, ERR_INTERNAL, "Failed to allocate socket");
        return;
    }

    TlsxTransportCtx* tls_ctx = (TlsxTransportCtx*)calloc(1, sizeof(TlsxTransportCtx));
    if (!tls_ctx) {
        obj_socket_free(sock_obj);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, host_slot, ok, ERR_INTERNAL, "Failed to allocate TLS context");
        return;
    }

    const char* tls_err = NULL;
    if (!tlsx_handshake_client(sock_obj, host, tls_ctx, false, &tls_err)) {
        tlsx_ctx_reset(tls_ctx);
        free(tls_ctx);
        obj_socket_free(sock_obj);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, host_slot, ok, ERR_NETWORK, tls_err ? tls_err : "TLS handshake failed");
        return;
    }

    sock_obj->transport_ctx = tls_ctx;
    sock_obj->transport_send = tlsx_transport_send;
    sock_obj->transport_recv = tlsx_transport_receive;
    sock_obj->transport_close = tlsx_transport_close;
    sock_obj->is_connected = true;

    Value ok;
    value_init_socket(&ok, sock_obj);
    result_tuple_set(vm, host_slot, ok, 0, NULL);
#else
    Value ok;
    value_init_nil(&ok);
    result_tuple_set(vm, host_slot, ok, ERR_UNSUPPORTED, "TLS sockets are only available on Windows builds");
#endif
}

void builtin_tls_send(VM* vm) {
    Value* sock_slot = &vm->stack.values[vm->stack.count - 2];
    Value sock_val = *sock_slot;
    Value data_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_network_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, sock_slot, ok, "Network access is disabled");
        return;
    }

    ObjSocket* sock_obj = value_get_socket_obj(&sock_val);
    ObjString* data_str = value_get_string_obj(&data_val);
    if (value_get_type(&sock_val) != VAL_SOCKET || !sock_obj ||
        value_get_type(&data_val) != VAL_STRING || !data_str || !data_str->chars) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, sock_slot, ok, ERR_INVALID_ARGUMENT, "tlsSend expects (socket, string)");
        return;
    }

    if (sock_obj->socket_fd < 0) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, sock_slot, ok, ERR_NETWORK, "Socket is closed");
        return;
    }
    if (!sock_obj->transport_ctx || !sock_obj->transport_send || !sock_obj->transport_recv) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, sock_slot, ok, ERR_INVALID_ARGUMENT, "tlsSend expects TLS socket from tlsConnect");
        return;
    }

    size_t len = data_str->length;
    if (len > (size_t)INT_MAX) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, sock_slot, ok, ERR_LIMIT, "tlsSend payload exceeds max size");
        return;
    }

    int sent = socket_send_dispatch(sock_obj, data_str->chars, (int)len);
    if (sent < 0) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, sock_slot, ok, ERR_NETWORK, "Failed to send TLS data");
        return;
    }

    Value ok;
    value_init_int(&ok, sent);
    result_tuple_set(vm, sock_slot, ok, 0, NULL);
}

void builtin_tls_receive(VM* vm) {
    Value* sock_slot = &vm->stack.values[vm->stack.count - 2];
    Value sock_val = *sock_slot;
    Value max_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_network_enabled(vm)) {
        Value ok;
        value_init_string(&ok, "");
        result_permission_denied(vm, sock_slot, ok, "Network access is disabled");
        return;
    }

    ObjSocket* sock_obj = value_get_socket_obj(&sock_val);
    if (value_get_type(&sock_val) != VAL_SOCKET || !sock_obj || value_get_type(&max_val) != VAL_INT) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, sock_slot, ok, ERR_INVALID_ARGUMENT, "tlsReceive expects (socket, int)");
        return;
    }

    if (sock_obj->socket_fd < 0) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, sock_slot, ok, ERR_NETWORK, "Socket is closed");
        return;
    }
    if (!sock_obj->transport_ctx || !sock_obj->transport_send || !sock_obj->transport_recv) {
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, sock_slot, ok, ERR_INVALID_ARGUMENT, "tlsReceive expects TLS socket from tlsConnect");
        return;
    }

    int max_bytes = (int)value_get_int(&max_val);
    if (max_bytes <= 0 || max_bytes > 65536) {
        max_bytes = 65536;
    }

    char* buffer = (char*)safe_malloc((size_t)max_bytes + 1);
    int received = socket_recv_dispatch(sock_obj, buffer, max_bytes);
    if (received < 0) {
        free(buffer);
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, sock_slot, ok, ERR_NETWORK, "Failed to receive TLS data");
        return;
    }
    if (received == 0) {
        free(buffer);
        Value ok;
        value_init_string(&ok, "");
        result_tuple_set(vm, sock_slot, ok, ERR_NETWORK, "Connection closed");
        return;
    }

    buffer[received] = '\0';
    Value ok;
    value_init_string(&ok, buffer);
    free(buffer);
    result_tuple_set(vm, sock_slot, ok, 0, NULL);
}

void builtin_tls_close(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value socket_val = *slot;

    ObjSocket* sock_obj = value_get_socket_obj(&socket_val);
    if (value_get_type(&socket_val) != VAL_SOCKET || !sock_obj) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "tlsClose expects (socket)");
        return;
    }

    if (!sock_obj->transport_ctx || !sock_obj->transport_send || !sock_obj->transport_recv) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "tlsClose expects TLS socket from tlsConnect");
        return;
    }

    socket_close_dispatch(sock_obj);
    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_channel_create(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value capacity_val = *slot;

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&capacity_val) != VAL_INT) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncChannelCreate expects (int)");
        return;
    }

    int64_t capacity64 = value_get_int(&capacity_val);
    if (capacity64 < 1 || capacity64 > 65536) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "channel capacity must be between 1 and 65536");
        return;
    }

    syncx_runtime_ensure_init();

    syncx_mutex_lock(&g_syncx_registry_mutex);
    int id = g_syncx_next_channel_id++;
    SyncxChannel* ch = syncx_channel_create(id, (int)capacity64);
    syncx_append_channel_unlocked(ch);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    Value ok;
    value_init_int(&ok, id);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_channel_send(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value channel_id_val = vm->stack.values[vm->stack.count - 3];
    Value payload_val = vm->stack.values[vm->stack.count - 2];
    Value timeout_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&channel_id_val) != VAL_INT || value_get_type(&timeout_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncChannelSend expects (int, any, int)");
        return;
    }

    int64_t timeout64 = value_get_int(&timeout_val);
    if (timeout64 < 1 || timeout64 > 120000) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "timeoutMs must be between 1 and 120000");
        return;
    }

    SyncxValue payload;
    const char* conv_err = NULL;
    if (!syncx_value_from_vm_value(&payload_val, &payload, &conv_err)) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, conv_err ? conv_err : "Unsupported payload type");
        return;
    }

    syncx_runtime_ensure_init();

    int channel_id = (int)value_get_int(&channel_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxChannel* ch = syncx_channel_lookup_unlocked(channel_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!ch) {
        syncx_value_free(&payload);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown channel id");
        return;
    }

    int64_t err_code = 0;
    const char* err_msg = NULL;
    bool sent = syncx_channel_send_internal(ch, &payload, (int)timeout64, &err_code, &err_msg);
    syncx_value_free(&payload);
    if (!sent) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         slot,
                         ok,
                         err_code ? err_code : ERR_IO,
                         err_msg ? err_msg : "Failed to send channel message");
        return;
    }

    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_channel_send_typed(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 4];
    Value channel_id_val = vm->stack.values[vm->stack.count - 4];
    Value payload_val = vm->stack.values[vm->stack.count - 3];
    Value schema_val = vm->stack.values[vm->stack.count - 2];
    Value timeout_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&channel_id_val) != VAL_INT || value_get_type(&timeout_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncChannelSendTyped expects (int, any, any, int)");
        return;
    }

    int64_t timeout64 = value_get_int(&timeout_val);
    if (timeout64 < 1 || timeout64 > 120000) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "timeoutMs must be between 1 and 120000");
        return;
    }

    SyncxValue payload;
    syncx_value_init_nil(&payload);
    int64_t encode_err_code = 0;
    char encode_err_msg[512];
    if (!syncx_value_encode_typed(vm,
                                  &payload_val,
                                  &schema_val,
                                  &payload,
                                  &encode_err_code,
                                  encode_err_msg,
                                  sizeof(encode_err_msg))) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         slot,
                         ok,
                         encode_err_code ? encode_err_code : ERR_INVALID_ARGUMENT,
                         encode_err_msg[0] ? encode_err_msg : "Payload does not match schema");
        return;
    }

    syncx_runtime_ensure_init();
    int channel_id = (int)value_get_int(&channel_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxChannel* ch = syncx_channel_lookup_unlocked(channel_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!ch) {
        syncx_value_free(&payload);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown channel id");
        return;
    }

    int64_t err_code = 0;
    const char* err_msg = NULL;
    bool sent = syncx_channel_send_internal(ch, &payload, (int)timeout64, &err_code, &err_msg);
    syncx_value_free(&payload);
    if (!sent) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         slot,
                         ok,
                         err_code ? err_code : ERR_IO,
                         err_msg ? err_msg : "Failed to send typed channel message");
        return;
    }

    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_channel_recv(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value channel_id_val = vm->stack.values[vm->stack.count - 2];
    Value timeout_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&channel_id_val) != VAL_INT || value_get_type(&timeout_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncChannelRecv expects (int, int)");
        return;
    }

    int64_t timeout64 = value_get_int(&timeout_val);
    if (timeout64 < 1 || timeout64 > 120000) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "timeoutMs must be between 1 and 120000");
        return;
    }

    syncx_runtime_ensure_init();

    int channel_id = (int)value_get_int(&channel_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxChannel* ch = syncx_channel_lookup_unlocked(channel_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!ch) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown channel id");
        return;
    }

    SyncxValue message;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    bool got = syncx_channel_recv_internal(ch, (int)timeout64, &message, &err_code, &err_msg);
    if (!got) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         slot,
                         ok,
                         err_code ? err_code : ERR_IO,
                         err_msg ? err_msg : "Failed to receive channel message");
        return;
    }

    Value ok;
    if (!syncx_value_to_vm_value(vm, &message, &ok)) {
        syncx_value_free(&message);
        Value nil_ok;
        value_init_nil(&nil_ok);
        result_tuple_set(vm, slot, nil_ok, ERR_INTERNAL, "Failed to decode channel payload");
        return;
    }
    syncx_value_free(&message);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_channel_recv_typed(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value channel_id_val = vm->stack.values[vm->stack.count - 3];
    Value schema_val = vm->stack.values[vm->stack.count - 2];
    Value timeout_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&channel_id_val) != VAL_INT || value_get_type(&timeout_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncChannelRecvTyped expects (int, any, int)");
        return;
    }

    int64_t timeout64 = value_get_int(&timeout_val);
    if (timeout64 < 1 || timeout64 > 120000) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "timeoutMs must be between 1 and 120000");
        return;
    }

    syncx_runtime_ensure_init();

    int channel_id = (int)value_get_int(&channel_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxChannel* ch = syncx_channel_lookup_unlocked(channel_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!ch) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown channel id");
        return;
    }

    SyncxValue message;
    int64_t err_code = 0;
    const char* err_msg = NULL;
    bool got = syncx_channel_recv_internal(ch, (int)timeout64, &message, &err_code, &err_msg);
    if (!got) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         slot,
                         ok,
                         err_code ? err_code : ERR_IO,
                         err_msg ? err_msg : "Failed to receive typed channel message");
        return;
    }

    Value ok;
    char decode_err_msg[512];
    int64_t decode_err_code = 0;
    if (!syncx_value_decode_typed(vm,
                                  &message,
                                  &schema_val,
                                  &ok,
                                  &decode_err_code,
                                  decode_err_msg,
                                  sizeof(decode_err_msg))) {
        syncx_value_free(&message);
        Value nil_ok;
        value_init_nil(&nil_ok);
        result_tuple_set(vm,
                         slot,
                         nil_ok,
                         decode_err_code ? decode_err_code : ERR_INVALID_ARGUMENT,
                         decode_err_msg[0] ? decode_err_msg : "Typed channel payload does not match schema");
        return;
    }

    syncx_value_free(&message);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_channel_close(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value channel_id_val = *slot;

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&channel_id_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncChannelClose expects (int)");
        return;
    }

    syncx_runtime_ensure_init();

    int channel_id = (int)value_get_int(&channel_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxChannel* ch = syncx_channel_lookup_unlocked(channel_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!ch) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown channel id");
        return;
    }

    syncx_mutex_lock(&ch->mutex);
    ch->closed = true;
    syncx_cond_broadcast(&ch->not_empty);
    syncx_cond_broadcast(&ch->not_full);
    syncx_mutex_unlock(&ch->mutex);

    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_shared_create(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value initial = *slot;

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    SyncxValue init_value;
    const char* conv_err = NULL;
    if (!syncx_value_from_vm_value(&initial, &init_value, &conv_err)) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, conv_err ? conv_err : "Unsupported shared value");
        return;
    }

    syncx_runtime_ensure_init();

    syncx_mutex_lock(&g_syncx_registry_mutex);
    int id = g_syncx_next_shared_cell_id++;
    SyncxSharedCell* cell = syncx_shared_cell_create(id, &init_value);
    syncx_append_shared_cell_unlocked(cell);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    syncx_value_free(&init_value);

    Value ok;
    value_init_int(&ok, id);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_shared_create_typed(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value initial_val = vm->stack.values[vm->stack.count - 2];
    Value schema_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    SyncxValue init_value;
    syncx_value_init_nil(&init_value);
    int64_t encode_err_code = 0;
    char encode_err_msg[512];
    if (!syncx_value_encode_typed(vm,
                                  &initial_val,
                                  &schema_val,
                                  &init_value,
                                  &encode_err_code,
                                  encode_err_msg,
                                  sizeof(encode_err_msg))) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm,
                         slot,
                         ok,
                         encode_err_code ? encode_err_code : ERR_INVALID_ARGUMENT,
                         encode_err_msg[0] ? encode_err_msg : "Initial value does not match schema");
        return;
    }

    syncx_runtime_ensure_init();

    syncx_mutex_lock(&g_syncx_registry_mutex);
    int id = g_syncx_next_shared_cell_id++;
    SyncxSharedCell* cell = syncx_shared_cell_create(id, &init_value);
    syncx_append_shared_cell_unlocked(cell);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    syncx_value_free(&init_value);

    Value ok;
    value_init_int(&ok, id);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_shared_get(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value cell_id_val = *slot;

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&cell_id_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncSharedGet expects (int)");
        return;
    }

    syncx_runtime_ensure_init();

    int cell_id = (int)value_get_int(&cell_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxSharedCell* cell = syncx_shared_cell_lookup_unlocked(cell_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!cell) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown shared cell id");
        return;
    }

    SyncxValue snapshot;
    syncx_value_init_nil(&snapshot);
    syncx_mutex_lock(&cell->mutex);
    syncx_value_clone(&snapshot, &cell->value);
    syncx_mutex_unlock(&cell->mutex);

    Value ok;
    if (!syncx_value_to_vm_value(vm, &snapshot, &ok)) {
        syncx_value_free(&snapshot);
        Value nil_ok;
        value_init_nil(&nil_ok);
        result_tuple_set(vm, slot, nil_ok, ERR_INTERNAL, "Failed to decode shared value");
        return;
    }
    syncx_value_free(&snapshot);

    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_shared_get_typed(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value cell_id_val = vm->stack.values[vm->stack.count - 2];
    Value schema_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&cell_id_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncSharedGetTyped expects (int, any)");
        return;
    }

    syncx_runtime_ensure_init();

    int cell_id = (int)value_get_int(&cell_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxSharedCell* cell = syncx_shared_cell_lookup_unlocked(cell_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!cell) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown shared cell id");
        return;
    }

    SyncxValue snapshot;
    syncx_value_init_nil(&snapshot);
    syncx_mutex_lock(&cell->mutex);
    syncx_value_clone(&snapshot, &cell->value);
    syncx_mutex_unlock(&cell->mutex);

    Value ok;
    char decode_err_msg[512];
    int64_t decode_err_code = 0;
    if (!syncx_value_decode_typed(vm,
                                  &snapshot,
                                  &schema_val,
                                  &ok,
                                  &decode_err_code,
                                  decode_err_msg,
                                  sizeof(decode_err_msg))) {
        syncx_value_free(&snapshot);
        Value nil_ok;
        value_init_nil(&nil_ok);
        result_tuple_set(vm,
                         slot,
                         nil_ok,
                         decode_err_code ? decode_err_code : ERR_INVALID_ARGUMENT,
                         decode_err_msg[0] ? decode_err_msg : "Shared value does not match schema");
        return;
    }
    syncx_value_free(&snapshot);

    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_shared_set(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value cell_id_val = vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&cell_id_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncSharedSet expects (int, any)");
        return;
    }

    SyncxValue next_value;
    const char* conv_err = NULL;
    if (!syncx_value_from_vm_value(&value_val, &next_value, &conv_err)) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, conv_err ? conv_err : "Unsupported shared value");
        return;
    }

    syncx_runtime_ensure_init();

    int cell_id = (int)value_get_int(&cell_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxSharedCell* cell = syncx_shared_cell_lookup_unlocked(cell_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!cell) {
        syncx_value_free(&next_value);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown shared cell id");
        return;
    }

    syncx_mutex_lock(&cell->mutex);
    syncx_value_free(&cell->value);
    syncx_value_clone(&cell->value, &next_value);
    syncx_mutex_unlock(&cell->mutex);
    syncx_value_free(&next_value);

    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_shared_set_typed(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value cell_id_val = vm->stack.values[vm->stack.count - 3];
    Value value_val = vm->stack.values[vm->stack.count - 2];
    Value schema_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&cell_id_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncSharedSetTyped expects (int, any, any)");
        return;
    }

    SyncxValue next_value;
    syncx_value_init_nil(&next_value);
    int64_t encode_err_code = 0;
    char encode_err_msg[512];
    if (!syncx_value_encode_typed(vm,
                                  &value_val,
                                  &schema_val,
                                  &next_value,
                                  &encode_err_code,
                                  encode_err_msg,
                                  sizeof(encode_err_msg))) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         slot,
                         ok,
                         encode_err_code ? encode_err_code : ERR_INVALID_ARGUMENT,
                         encode_err_msg[0] ? encode_err_msg : "Shared value does not match schema");
        return;
    }

    syncx_runtime_ensure_init();

    int cell_id = (int)value_get_int(&cell_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxSharedCell* cell = syncx_shared_cell_lookup_unlocked(cell_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!cell) {
        syncx_value_free(&next_value);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown shared cell id");
        return;
    }

    syncx_mutex_lock(&cell->mutex);
    syncx_value_free(&cell->value);
    syncx_value_clone(&cell->value, &next_value);
    syncx_mutex_unlock(&cell->mutex);
    syncx_value_free(&next_value);

    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_thread_spawn(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value function_name_val = vm->stack.values[vm->stack.count - 3];
    Value inbox_val = vm->stack.values[vm->stack.count - 2];
    Value outbox_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    ObjString* function_name_str = value_get_string_obj(&function_name_val);
    if (value_get_type(&function_name_val) != VAL_STRING || !function_name_str || !function_name_str->chars ||
        value_get_type(&inbox_val) != VAL_INT || value_get_type(&outbox_val) != VAL_INT) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncThreadSpawn expects (string, int, int)");
        return;
    }

    const char* function_name = function_name_str->chars;
    if (!function_name || function_name[0] == '\0') {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Thread function name cannot be empty");
        return;
    }

    Value file_path_val = vm_get_global(vm, "__vml_entry_file");
    ObjString* file_path_str = value_get_string_obj(&file_path_val);
    if (value_get_type(&file_path_val) != VAL_STRING || !file_path_str || file_path_str->chars[0] == '\0') {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INTERNAL, "Runtime entry file is unavailable");
        return;
    }

    Value fn_check = vm_get_global(vm, function_name);
    if (value_get_type(&fn_check) != VAL_FUNCTION || !value_get_function_obj(&fn_check)) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Thread entry function not found");
        return;
    }

    syncx_runtime_ensure_init();

    int inbox_id = (int)value_get_int(&inbox_val);
    int outbox_id = (int)value_get_int(&outbox_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    if (inbox_id >= 0 && !syncx_channel_lookup_unlocked(inbox_id)) {
        syncx_mutex_unlock(&g_syncx_registry_mutex);
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown inbox channel id");
        return;
    }
    if (outbox_id >= 0 && !syncx_channel_lookup_unlocked(outbox_id)) {
        syncx_mutex_unlock(&g_syncx_registry_mutex);
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown outbox channel id");
        return;
    }
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    syncx_mutex_lock(&g_syncx_registry_mutex);
    int thread_id = g_syncx_next_thread_id++;
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    SyncxThreadHandle* handle = syncx_thread_handle_create(thread_id);
    SyncxThreadTask* task = (SyncxThreadTask*)safe_malloc(sizeof(SyncxThreadTask));
    task->handle = handle;
    task->file_path = safe_strdup(file_path_str->chars);
    task->function_name = safe_strdup(function_name);
    task->inbox_channel_id = inbox_id;
    task->outbox_channel_id = outbox_id;
    task->has_arg_payload = false;
    syncx_value_init_nil(&task->arg_payload);

#ifdef _WIN32
    HANDLE os_thread = CreateThread(NULL, 0, syncx_thread_entry, task, 0, NULL);
    if (!os_thread) {
        syncx_thread_task_free(task);
        syncx_cond_destroy(&handle->finished_cond);
        syncx_mutex_destroy(&handle->mutex);
        free(handle);
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INTERNAL, "Failed to spawn OS thread");
        return;
    }
    handle->os_thread = os_thread;
#else
    if (pthread_create(&handle->os_thread, NULL, syncx_thread_entry, task) != 0) {
        syncx_thread_task_free(task);
        syncx_cond_destroy(&handle->finished_cond);
        syncx_mutex_destroy(&handle->mutex);
        free(handle);
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INTERNAL, "Failed to spawn OS thread");
        return;
    }
#endif
    handle->started = true;

    syncx_mutex_lock(&g_syncx_registry_mutex);
    syncx_append_thread_unlocked(handle);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    Value ok;
    value_init_int(&ok, thread_id);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_thread_spawn_typed(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 5];
    Value function_name_val = vm->stack.values[vm->stack.count - 5];
    Value arg_val = vm->stack.values[vm->stack.count - 4];
    Value arg_schema_val = vm->stack.values[vm->stack.count - 3];
    Value inbox_val = vm->stack.values[vm->stack.count - 2];
    Value outbox_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    ObjString* function_name_str = value_get_string_obj(&function_name_val);
    if (value_get_type(&function_name_val) != VAL_STRING || !function_name_str || !function_name_str->chars ||
        value_get_type(&inbox_val) != VAL_INT || value_get_type(&outbox_val) != VAL_INT) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncThreadSpawnTyped expects (string, any, any, int, int)");
        return;
    }

    const char* function_name = function_name_str->chars;
    if (!function_name || function_name[0] == '\0') {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Thread function name cannot be empty");
        return;
    }

    Value file_path_val = vm_get_global(vm, "__vml_entry_file");
    ObjString* file_path_str = value_get_string_obj(&file_path_val);
    if (value_get_type(&file_path_val) != VAL_STRING || !file_path_str || file_path_str->chars[0] == '\0') {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INTERNAL, "Runtime entry file is unavailable");
        return;
    }

    Value fn_check = vm_get_global(vm, function_name);
    if (value_get_type(&fn_check) != VAL_FUNCTION || !value_get_function_obj(&fn_check)) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Thread entry function not found");
        return;
    }

    SyncxValue arg_payload;
    syncx_value_init_nil(&arg_payload);
    int64_t encode_err_code = 0;
    char encode_err_msg[512];
    if (!syncx_value_encode_typed(vm,
                                  &arg_val,
                                  &arg_schema_val,
                                  &arg_payload,
                                  &encode_err_code,
                                  encode_err_msg,
                                  sizeof(encode_err_msg))) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm,
                         slot,
                         ok,
                         encode_err_code ? encode_err_code : ERR_INVALID_ARGUMENT,
                         encode_err_msg[0] ? encode_err_msg : "Thread argument does not match schema");
        return;
    }

    syncx_runtime_ensure_init();

    int inbox_id = (int)value_get_int(&inbox_val);
    int outbox_id = (int)value_get_int(&outbox_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    if (inbox_id >= 0 && !syncx_channel_lookup_unlocked(inbox_id)) {
        syncx_mutex_unlock(&g_syncx_registry_mutex);
        syncx_value_free(&arg_payload);
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown inbox channel id");
        return;
    }
    if (outbox_id >= 0 && !syncx_channel_lookup_unlocked(outbox_id)) {
        syncx_mutex_unlock(&g_syncx_registry_mutex);
        syncx_value_free(&arg_payload);
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown outbox channel id");
        return;
    }
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    syncx_mutex_lock(&g_syncx_registry_mutex);
    int thread_id = g_syncx_next_thread_id++;
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    SyncxThreadHandle* handle = syncx_thread_handle_create(thread_id);
    SyncxThreadTask* task = (SyncxThreadTask*)safe_malloc(sizeof(SyncxThreadTask));
    task->handle = handle;
    task->file_path = safe_strdup(file_path_str->chars);
    task->function_name = safe_strdup(function_name);
    task->inbox_channel_id = inbox_id;
    task->outbox_channel_id = outbox_id;
    task->has_arg_payload = true;
    task->arg_payload = arg_payload;

#ifdef _WIN32
    HANDLE os_thread = CreateThread(NULL, 0, syncx_thread_entry, task, 0, NULL);
    if (!os_thread) {
        syncx_thread_task_free(task);
        syncx_cond_destroy(&handle->finished_cond);
        syncx_mutex_destroy(&handle->mutex);
        free(handle);
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INTERNAL, "Failed to spawn OS thread");
        return;
    }
    handle->os_thread = os_thread;
#else
    if (pthread_create(&handle->os_thread, NULL, syncx_thread_entry, task) != 0) {
        syncx_thread_task_free(task);
        syncx_cond_destroy(&handle->finished_cond);
        syncx_mutex_destroy(&handle->mutex);
        free(handle);
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INTERNAL, "Failed to spawn OS thread");
        return;
    }
#endif
    handle->started = true;

    syncx_mutex_lock(&g_syncx_registry_mutex);
    syncx_append_thread_unlocked(handle);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    Value ok;
    value_init_int(&ok, thread_id);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_thread_join(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value thread_id_val = vm->stack.values[vm->stack.count - 2];
    Value timeout_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&thread_id_val) != VAL_INT || value_get_type(&timeout_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncThreadJoin expects (int, int)");
        return;
    }

    int64_t timeout64 = value_get_int(&timeout_val);
    if (timeout64 < 1 || timeout64 > 120000) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "timeoutMs must be between 1 and 120000");
        return;
    }

    syncx_runtime_ensure_init();

    int thread_id = (int)value_get_int(&thread_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxThreadHandle* handle = syncx_thread_lookup_unlocked(thread_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!handle) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown thread id");
        return;
    }

    syncx_mutex_lock(&handle->mutex);
    if (handle->joined) {
        syncx_mutex_unlock(&handle->mutex);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Thread is already joined");
        return;
    }

    if (!syncx_thread_wait_finished(handle, (int)timeout64)) {
        syncx_mutex_unlock(&handle->mutex);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "Thread join timed out");
        return;
    }

    handle->joined = true;
    int exit_code = handle->exit_code;
    int64_t error_code = handle->error_code;
    char* error_message_copy = handle->error_message ? safe_strdup(handle->error_message) : NULL;
    syncx_mutex_unlock(&handle->mutex);

#ifdef _WIN32
    if (handle->started && handle->os_thread) {
        WaitForSingleObject(handle->os_thread, INFINITE);
        CloseHandle(handle->os_thread);
        handle->os_thread = NULL;
    }
#else
    if (handle->started) {
        (void)pthread_join(handle->os_thread, NULL);
    }
#endif

    if (error_code != 0 || exit_code != 0) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         slot,
                         ok,
                         error_code ? error_code : ERR_INTERNAL,
                         error_message_copy ? error_message_copy : "Thread execution failed");
        if (error_message_copy) free(error_message_copy);
        return;
    }

    if (error_message_copy) free(error_message_copy);
    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_thread_join_typed(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value thread_id_val = vm->stack.values[vm->stack.count - 3];
    Value schema_val = vm->stack.values[vm->stack.count - 2];
    Value timeout_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&thread_id_val) != VAL_INT || value_get_type(&timeout_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncThreadJoinTyped expects (int, any, int)");
        return;
    }

    int64_t timeout64 = value_get_int(&timeout_val);
    if (timeout64 < 1 || timeout64 > 120000) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "timeoutMs must be between 1 and 120000");
        return;
    }

    syncx_runtime_ensure_init();

    int thread_id = (int)value_get_int(&thread_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxThreadHandle* handle = syncx_thread_lookup_unlocked(thread_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!handle) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown thread id");
        return;
    }

    SyncxValue result_payload;
    syncx_value_init_nil(&result_payload);
    bool has_result_payload = false;
    syncx_mutex_lock(&handle->mutex);
    if (handle->joined) {
        syncx_mutex_unlock(&handle->mutex);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Thread is already joined");
        return;
    }

    if (!syncx_thread_wait_finished(handle, (int)timeout64)) {
        syncx_mutex_unlock(&handle->mutex);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_LIMIT, "Thread join timed out");
        return;
    }

    handle->joined = true;
    int exit_code = handle->exit_code;
    int64_t error_code = handle->error_code;
    char* error_message_copy = handle->error_message ? safe_strdup(handle->error_message) : NULL;
    if (handle->has_result_payload) {
        syncx_value_clone(&result_payload, &handle->result_payload);
        has_result_payload = true;
    }
    syncx_mutex_unlock(&handle->mutex);

#ifdef _WIN32
    if (handle->started && handle->os_thread) {
        WaitForSingleObject(handle->os_thread, INFINITE);
        CloseHandle(handle->os_thread);
        handle->os_thread = NULL;
    }
#else
    if (handle->started) {
        (void)pthread_join(handle->os_thread, NULL);
    }
#endif

    if (error_code != 0 || exit_code != 0) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm,
                         slot,
                         ok,
                         error_code ? error_code : ERR_INTERNAL,
                         error_message_copy ? error_message_copy : "Thread execution failed");
        if (error_message_copy) free(error_message_copy);
        syncx_value_free(&result_payload);
        return;
    }

    if (error_message_copy) free(error_message_copy);

    Value ok;
    if (has_result_payload) {
        char decode_err_msg[512];
        int64_t decode_err_code = 0;
        if (!syncx_value_decode_typed(vm,
                                      &result_payload,
                                      &schema_val,
                                      &ok,
                                      &decode_err_code,
                                      decode_err_msg,
                                      sizeof(decode_err_msg))) {
            syncx_value_free(&result_payload);
            Value nil_ok;
            value_init_nil(&nil_ok);
            result_tuple_set(vm,
                             slot,
                             nil_ok,
                             decode_err_code ? decode_err_code : ERR_INVALID_ARGUMENT,
                             decode_err_msg[0] ? decode_err_msg : "Thread return value does not match schema");
            return;
        }
    } else {
        value_init_nil(&ok);
    }
    syncx_value_free(&result_payload);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_thread_inbox(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value out;
    value_init_int(&out, g_syncx_tls_inbox);
    value_free(slot);
    *slot = out;
}

void builtin_sync_thread_outbox(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value out;
    value_init_int(&out, g_syncx_tls_outbox);
    value_free(slot);
    *slot = out;
}

void builtin_sync_thread_arg_typed(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value schema_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    SyncxValue payload;
    syncx_value_init_nil(&payload);
    if (g_syncx_tls_has_arg_payload) {
        syncx_value_clone(&payload, &g_syncx_tls_arg_payload);
    }

    Value ok;
    int64_t decode_err_code = 0;
    char decode_err_msg[512];
    if (!syncx_value_decode_typed(vm,
                                  &payload,
                                  &schema_val,
                                  &ok,
                                  &decode_err_code,
                                  decode_err_msg,
                                  sizeof(decode_err_msg))) {
        syncx_value_free(&payload);
        Value nil_ok;
        value_init_nil(&nil_ok);
        result_tuple_set(vm,
                         slot,
                         nil_ok,
                         decode_err_code ? decode_err_code : ERR_INVALID_ARGUMENT,
                         decode_err_msg[0] ? decode_err_msg : "Thread argument does not match schema");
        return;
    }

    syncx_value_free(&payload);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_arc_create(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value initial_val = *slot;

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    SyncxValue init_value;
    syncx_value_init_nil(&init_value);
    int64_t encode_err_code = 0;
    char encode_err_msg[512];
    if (!syncx_value_encode_lossless(vm,
                                     &initial_val,
                                     &init_value,
                                     &encode_err_code,
                                     encode_err_msg,
                                     sizeof(encode_err_msg))) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm,
                         slot,
                         ok,
                         encode_err_code ? encode_err_code : ERR_INVALID_ARGUMENT,
                         encode_err_msg[0] ? encode_err_msg : "Unsupported Arc value");
        return;
    }

    syncx_runtime_ensure_init();

    syncx_mutex_lock(&g_syncx_registry_mutex);
    int id = g_syncx_next_shared_cell_id++;
    SyncxSharedCell* cell = syncx_shared_cell_create(id, &init_value);
    syncx_append_shared_cell_unlocked(cell);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    syncx_value_free(&init_value);

    Value ok;
    value_init_int(&ok, id);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_arc_clone(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value arc_id_val = *slot;

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&arc_id_val) != VAL_INT) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncArcClone expects (int)");
        return;
    }

    syncx_runtime_ensure_init();

    int arc_id = (int)value_get_int(&arc_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxSharedCell* cell = syncx_shared_cell_lookup_unlocked(arc_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!cell) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown arc id");
        return;
    }

    Value ok;
    value_init_int(&ok, arc_id);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_arc_guard_acquire(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value arc_id_val = *slot;

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&arc_id_val) != VAL_INT) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncArcGuardAcquire expects (int)");
        return;
    }

    syncx_runtime_ensure_init();

    int arc_id = (int)value_get_int(&arc_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxSharedCell* cell = syncx_shared_cell_lookup_unlocked(arc_id);
    if (!cell) {
        syncx_mutex_unlock(&g_syncx_registry_mutex);
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown arc id");
        return;
    }

    int guard_id = g_syncx_next_guard_id++;
    SyncxArcGuard* guard = syncx_arc_guard_create(guard_id, cell);
    syncx_append_guard_unlocked(guard);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    syncx_mutex_lock(&cell->mutex);

    Value ok;
    value_init_int(&ok, guard_id);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_arc_guard_read(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value guard_id_val = *slot;

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&guard_id_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncArcGuardRead expects (int)");
        return;
    }

    syncx_runtime_ensure_init();

    int guard_id = (int)value_get_int(&guard_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxArcGuard* guard = syncx_guard_lookup_unlocked(guard_id);
    if (!guard || !guard->active) {
        syncx_mutex_unlock(&g_syncx_registry_mutex);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown or inactive guard id");
        return;
    }
    if (!syncx_thread_id_equals(guard->owner_thread_id, syncx_current_thread_id())) {
        syncx_mutex_unlock(&g_syncx_registry_mutex);
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_PERMISSION, "Guard can only be used by the acquiring thread");
        return;
    }
    SyncxSharedCell* cell = guard->cell;
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    SyncxValue snapshot;
    syncx_value_init_nil(&snapshot);
    syncx_value_clone(&snapshot, &cell->value);

    Value ok;
    int64_t decode_err_code = 0;
    char decode_err_msg[512];
    if (!syncx_value_decode_lossless(vm,
                                     &snapshot,
                                     &ok,
                                     &decode_err_code,
                                     decode_err_msg,
                                     sizeof(decode_err_msg))) {
        syncx_value_free(&snapshot);
        Value nil_ok;
        value_init_nil(&nil_ok);
        result_tuple_set(vm,
                         slot,
                         nil_ok,
                         decode_err_code ? decode_err_code : ERR_INTERNAL,
                         decode_err_msg[0] ? decode_err_msg : "Failed to decode Arc value");
        return;
    }

    syncx_value_free(&snapshot);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_arc_guard_write(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value guard_id_val = vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&guard_id_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncArcGuardWrite expects (int, any)");
        return;
    }

    SyncxValue next_value;
    syncx_value_init_nil(&next_value);
    int64_t encode_err_code = 0;
    char encode_err_msg[512];
    if (!syncx_value_encode_lossless(vm,
                                     &value_val,
                                     &next_value,
                                     &encode_err_code,
                                     encode_err_msg,
                                     sizeof(encode_err_msg))) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm,
                         slot,
                         ok,
                         encode_err_code ? encode_err_code : ERR_INVALID_ARGUMENT,
                         encode_err_msg[0] ? encode_err_msg : "Unsupported Arc value");
        return;
    }

    syncx_runtime_ensure_init();

    int guard_id = (int)value_get_int(&guard_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SyncxArcGuard* guard = syncx_guard_lookup_unlocked(guard_id);
    if (!guard || !guard->active) {
        syncx_mutex_unlock(&g_syncx_registry_mutex);
        syncx_value_free(&next_value);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown or inactive guard id");
        return;
    }
    if (!syncx_thread_id_equals(guard->owner_thread_id, syncx_current_thread_id())) {
        syncx_mutex_unlock(&g_syncx_registry_mutex);
        syncx_value_free(&next_value);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_PERMISSION, "Guard can only be used by the acquiring thread");
        return;
    }
    SyncxSharedCell* cell = guard->cell;
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    syncx_value_free(&cell->value);
    syncx_value_clone(&cell->value, &next_value);
    syncx_value_free(&next_value);

    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sync_arc_guard_release(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value guard_id_val = *slot;

    if (!vm_is_threading_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "Threading runtime is disabled");
        return;
    }

    if (value_get_type(&guard_id_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "syncArcGuardRelease expects (int)");
        return;
    }

    syncx_runtime_ensure_init();

    int guard_id = (int)value_get_int(&guard_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    int guard_index = syncx_guard_find_index_unlocked(guard_id);
    SyncxArcGuard* guard = (guard_index >= 0) ? g_syncx_guards[guard_index] : NULL;
    if (!guard || !guard->active) {
        syncx_mutex_unlock(&g_syncx_registry_mutex);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown or inactive guard id");
        return;
    }
    if (!syncx_thread_id_equals(guard->owner_thread_id, syncx_current_thread_id())) {
        syncx_mutex_unlock(&g_syncx_registry_mutex);
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_PERMISSION, "Guard can only be released by the acquiring thread");
        return;
    }

    guard->active = false;
    SyncxSharedCell* cell = guard->cell;
    g_syncx_guards[guard_index] = NULL;
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    syncx_mutex_unlock(&cell->mutex);
    free(guard);

    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_is_available(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    if (!vm_is_sqlite_enabled(vm)) {
        Value out;
        value_init_bool(&out, false);
        value_free(slot);
        *slot = out;
        return;
    }
    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    Value out;
    value_init_bool(&out, g_sqlx_api.available);
    value_free(slot);
    *slot = out;
}

void builtin_sqlite_open(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value path_val = *slot;

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    ObjString* path_str = value_get_string_obj(&path_val);
    if (value_get_type(&path_val) != VAL_STRING || !path_str || !path_str->chars) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteOpen expects (string)");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    const char* db_path = path_str->chars;
    SqlxNativeDb* db = NULL;
    int rc = g_sqlx_api.open_v2(db_path,
                                &db,
                                SQLX_OPEN_READWRITE | SQLX_OPEN_CREATE | SQLX_OPEN_URI,
                                NULL);
    if (rc != SQLX_OK || !db) {
        const char* err_msg = sqlx_db_error_message(db, "Failed to open sqlite database");
        if (db) {
            (void)g_sqlx_api.close_v2(db);
        }
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_IO, err_msg);
        return;
    }

    syncx_mutex_lock(&g_syncx_registry_mutex);
    int db_id = g_sqlx_next_db_id++;
    SqlxDbHandle* db_handle = sqlx_db_handle_create(db_id, db);
    sqlx_append_db_unlocked(db_handle);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    Value ok;
    value_init_int(&ok, db_id);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_close(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value db_id_val = *slot;

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    if (value_get_type(&db_id_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteClose expects (int)");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    int db_id = (int)value_get_int(&db_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SqlxDbHandle* db_handle = sqlx_lookup_db_unlocked(db_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!db_handle) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite db id");
        return;
    }

    int rc = SQLX_OK;
    const char* err_msg = NULL;
    syncx_mutex_lock(&db_handle->mutex);
    if (!db_handle->closed && db_handle->db) {
        syncx_mutex_lock(&g_syncx_registry_mutex);
        for (int i = 0; i < g_sqlx_stmt_count; i++) {
            SqlxStmtHandle* stmt_handle = g_sqlx_stmt_handles[i];
            if (!stmt_handle) continue;
            if (stmt_handle->db_id == db_handle->id) {
                sqlx_stmt_finalize_unlocked(stmt_handle);
            }
        }
        syncx_mutex_unlock(&g_syncx_registry_mutex);

        rc = g_sqlx_api.close_v2(db_handle->db);
        if (rc == SQLX_OK) {
            db_handle->closed = true;
            db_handle->db = NULL;
        } else {
            err_msg = sqlx_db_error_message(db_handle->db, "Failed to close sqlite database");
        }
    }
    syncx_mutex_unlock(&db_handle->mutex);

    Value ok;
    value_init_bool(&ok, rc == SQLX_OK);
    if (rc != SQLX_OK) {
        result_tuple_set(vm, slot, ok, ERR_IO, err_msg ? err_msg : "Failed to close sqlite database");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_exec(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value db_id_val = vm->stack.values[vm->stack.count - 2];
    Value sql_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    ObjString* sql_str = value_get_string_obj(&sql_val);
    if (value_get_type(&db_id_val) != VAL_INT ||
        value_get_type(&sql_val) != VAL_STRING ||
        !sql_str ||
        !sql_str->chars) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteExec expects (int, string)");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    int db_id = (int)value_get_int(&db_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SqlxDbHandle* db_handle = sqlx_lookup_db_unlocked(db_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!db_handle) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite db id");
        return;
    }

    int rc = SQLX_OK;
    const char* err_msg = NULL;
    char* sqlite_err = NULL;
    syncx_mutex_lock(&db_handle->mutex);
    if (db_handle->closed || !db_handle->db) {
        err_msg = "SQLite database is closed";
        rc = -1;
    } else {
        rc = g_sqlx_api.exec(db_handle->db, sql_str->chars, NULL, NULL, &sqlite_err);
        if (rc != SQLX_OK) {
            err_msg = sqlite_err && sqlite_err[0] ? sqlite_err : sqlx_db_error_message(db_handle->db, "SQLite exec failed");
        }
    }
    syncx_mutex_unlock(&db_handle->mutex);

    Value ok;
    value_init_bool(&ok, rc == SQLX_OK);
    if (rc != SQLX_OK) {
        result_tuple_set(vm, slot, ok, rc == -1 ? ERR_INVALID_ARGUMENT : ERR_IO, err_msg ? err_msg : "SQLite exec failed");
        if (sqlite_err) {
            g_sqlx_api.free_mem(sqlite_err);
        }
        return;
    }
    if (sqlite_err) {
        g_sqlx_api.free_mem(sqlite_err);
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_prepare(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value db_id_val = vm->stack.values[vm->stack.count - 2];
    Value sql_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    ObjString* sql_str = value_get_string_obj(&sql_val);
    if (value_get_type(&db_id_val) != VAL_INT ||
        value_get_type(&sql_val) != VAL_STRING ||
        !sql_str ||
        !sql_str->chars) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqlitePrepare expects (int, string)");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    int db_id = (int)value_get_int(&db_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SqlxDbHandle* db_handle = sqlx_lookup_db_unlocked(db_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!db_handle) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite db id");
        return;
    }

    SqlxNativeStmt* native_stmt = NULL;
    int rc = SQLX_OK;
    const char* err_msg = NULL;
    syncx_mutex_lock(&db_handle->mutex);
    if (db_handle->closed || !db_handle->db) {
        rc = -1;
        err_msg = "SQLite database is closed";
    } else {
        rc = g_sqlx_api.prepare_v2(db_handle->db, sql_str->chars, -1, &native_stmt, NULL);
        if (rc != SQLX_OK || !native_stmt) {
            err_msg = sqlx_db_error_message(db_handle->db, "Failed to prepare sqlite statement");
        }
    }
    syncx_mutex_unlock(&db_handle->mutex);

    if (rc != SQLX_OK || !native_stmt) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, rc == -1 ? ERR_INVALID_ARGUMENT : ERR_IO, err_msg ? err_msg : "Failed to prepare sqlite statement");
        return;
    }

    syncx_mutex_lock(&g_syncx_registry_mutex);
    int stmt_id = g_sqlx_next_stmt_id++;
    SqlxStmtHandle* stmt_handle = sqlx_stmt_handle_create(stmt_id, db_id, native_stmt);
    sqlx_append_stmt_unlocked(stmt_handle);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    Value ok;
    value_init_int(&ok, stmt_id);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_bind_int(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value stmt_id_val = vm->stack.values[vm->stack.count - 3];
    Value index_val = vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    if (value_get_type(&stmt_id_val) != VAL_INT ||
        value_get_type(&index_val) != VAL_INT ||
        value_get_type(&value_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteBindInt expects (int, int, int)");
        return;
    }

    int param_index = (int)value_get_int(&index_val);
    if (param_index <= 0) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteBindInt parameter index must be >= 1");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    SqlxStmtHandle* stmt_handle = NULL;
    SqlxDbHandle* db_handle = NULL;
    if (!sqlx_lookup_stmt_with_db((int)value_get_int(&stmt_id_val), &stmt_handle, &db_handle)) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite statement id");
        return;
    }

    if (stmt_handle->finalized || !stmt_handle->stmt) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "SQLite statement is finalized");
        return;
    }

    int rc = SQLX_OK;
    const char* err_msg = NULL;
    syncx_mutex_lock(&db_handle->mutex);
    if (db_handle->closed || !db_handle->db) {
        rc = -1;
        err_msg = "SQLite database is closed";
    } else {
        rc = g_sqlx_api.bind_int64(stmt_handle->stmt, param_index, (SqlxInt64)value_get_int(&value_val));
        if (rc == SQLX_OK) {
            stmt_handle->done = false;
        } else {
            err_msg = sqlx_db_error_message(db_handle->db, "sqliteBindInt failed");
        }
    }
    syncx_mutex_unlock(&db_handle->mutex);

    Value ok;
    value_init_bool(&ok, rc == SQLX_OK);
    if (rc != SQLX_OK) {
        int64_t err_code = (rc == -1 || rc == SQLX_RANGE) ? ERR_INVALID_ARGUMENT : ERR_IO;
        result_tuple_set(vm, slot, ok, err_code, err_msg ? err_msg : "sqliteBindInt failed");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_bind_double(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value stmt_id_val = vm->stack.values[vm->stack.count - 3];
    Value index_val = vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    ValueType value_type = value_get_type(&value_val);
    if (value_get_type(&stmt_id_val) != VAL_INT ||
        value_get_type(&index_val) != VAL_INT ||
        (value_type != VAL_DOUBLE && value_type != VAL_INT)) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteBindDouble expects (int, int, double|int)");
        return;
    }

    int param_index = (int)value_get_int(&index_val);
    if (param_index <= 0) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteBindDouble parameter index must be >= 1");
        return;
    }

    double number = (value_type == VAL_DOUBLE) ? value_get_double(&value_val) : (double)value_get_int(&value_val);

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    SqlxStmtHandle* stmt_handle = NULL;
    SqlxDbHandle* db_handle = NULL;
    if (!sqlx_lookup_stmt_with_db((int)value_get_int(&stmt_id_val), &stmt_handle, &db_handle)) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite statement id");
        return;
    }

    if (stmt_handle->finalized || !stmt_handle->stmt) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "SQLite statement is finalized");
        return;
    }

    int rc = SQLX_OK;
    const char* err_msg = NULL;
    syncx_mutex_lock(&db_handle->mutex);
    if (db_handle->closed || !db_handle->db) {
        rc = -1;
        err_msg = "SQLite database is closed";
    } else {
        rc = g_sqlx_api.bind_double(stmt_handle->stmt, param_index, number);
        if (rc == SQLX_OK) {
            stmt_handle->done = false;
        } else {
            err_msg = sqlx_db_error_message(db_handle->db, "sqliteBindDouble failed");
        }
    }
    syncx_mutex_unlock(&db_handle->mutex);

    Value ok;
    value_init_bool(&ok, rc == SQLX_OK);
    if (rc != SQLX_OK) {
        int64_t err_code = (rc == -1 || rc == SQLX_RANGE) ? ERR_INVALID_ARGUMENT : ERR_IO;
        result_tuple_set(vm, slot, ok, err_code, err_msg ? err_msg : "sqliteBindDouble failed");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_bind_string(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value stmt_id_val = vm->stack.values[vm->stack.count - 3];
    Value index_val = vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    ObjString* value_str = value_get_string_obj(&value_val);
    if (value_get_type(&stmt_id_val) != VAL_INT ||
        value_get_type(&index_val) != VAL_INT ||
        value_get_type(&value_val) != VAL_STRING ||
        !value_str ||
        !value_str->chars) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteBindString expects (int, int, string)");
        return;
    }

    int param_index = (int)value_get_int(&index_val);
    if (param_index <= 0) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteBindString parameter index must be >= 1");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    SqlxStmtHandle* stmt_handle = NULL;
    SqlxDbHandle* db_handle = NULL;
    if (!sqlx_lookup_stmt_with_db((int)value_get_int(&stmt_id_val), &stmt_handle, &db_handle)) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite statement id");
        return;
    }

    if (stmt_handle->finalized || !stmt_handle->stmt) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "SQLite statement is finalized");
        return;
    }

    int rc = SQLX_OK;
    const char* err_msg = NULL;
    syncx_mutex_lock(&db_handle->mutex);
    if (db_handle->closed || !db_handle->db) {
        rc = -1;
        err_msg = "SQLite database is closed";
    } else {
        int text_len = value_str->length;
        rc = g_sqlx_api.bind_text(stmt_handle->stmt,
                                  param_index,
                                  value_str->chars,
                                  text_len,
                                  SQLX_TRANSIENT);
        if (rc == SQLX_OK) {
            stmt_handle->done = false;
        } else {
            err_msg = sqlx_db_error_message(db_handle->db, "sqliteBindString failed");
        }
    }
    syncx_mutex_unlock(&db_handle->mutex);

    Value ok;
    value_init_bool(&ok, rc == SQLX_OK);
    if (rc != SQLX_OK) {
        int64_t err_code = (rc == -1 || rc == SQLX_RANGE) ? ERR_INVALID_ARGUMENT : ERR_IO;
        result_tuple_set(vm, slot, ok, err_code, err_msg ? err_msg : "sqliteBindString failed");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_bind_bytes(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 3];
    Value stmt_id_val = vm->stack.values[vm->stack.count - 3];
    Value index_val = vm->stack.values[vm->stack.count - 2];
    Value value_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    ObjBytes* value_bytes = value_get_bytes_obj(&value_val);
    if (value_get_type(&stmt_id_val) != VAL_INT ||
        value_get_type(&index_val) != VAL_INT ||
        value_get_type(&value_val) != VAL_BYTES ||
        !value_bytes) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteBindBytes expects (int, int, bytes)");
        return;
    }

    int param_index = (int)value_get_int(&index_val);
    if (param_index <= 0) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteBindBytes parameter index must be >= 1");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    SqlxStmtHandle* stmt_handle = NULL;
    SqlxDbHandle* db_handle = NULL;
    if (!sqlx_lookup_stmt_with_db((int)value_get_int(&stmt_id_val), &stmt_handle, &db_handle)) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite statement id");
        return;
    }

    if (stmt_handle->finalized || !stmt_handle->stmt) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "SQLite statement is finalized");
        return;
    }

    int blob_len = value_bytes->length;
    uint8_t* blob_data = obj_bytes_data(value_bytes);
    if (blob_len > 0 && !blob_data) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Invalid bytes buffer");
        return;
    }

    int rc = SQLX_OK;
    const char* err_msg = NULL;
    syncx_mutex_lock(&db_handle->mutex);
    if (db_handle->closed || !db_handle->db) {
        rc = -1;
        err_msg = "SQLite database is closed";
    } else {
        rc = g_sqlx_api.bind_blob(stmt_handle->stmt,
                                  param_index,
                                  blob_len > 0 ? (const void*)blob_data : NULL,
                                  blob_len,
                                  SQLX_TRANSIENT);
        if (rc == SQLX_OK) {
            stmt_handle->done = false;
        } else {
            err_msg = sqlx_db_error_message(db_handle->db, "sqliteBindBytes failed");
        }
    }
    syncx_mutex_unlock(&db_handle->mutex);

    Value ok;
    value_init_bool(&ok, rc == SQLX_OK);
    if (rc != SQLX_OK) {
        int64_t err_code = (rc == -1 || rc == SQLX_RANGE) ? ERR_INVALID_ARGUMENT : ERR_IO;
        result_tuple_set(vm, slot, ok, err_code, err_msg ? err_msg : "sqliteBindBytes failed");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_bind_null(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value stmt_id_val = vm->stack.values[vm->stack.count - 2];
    Value index_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    if (value_get_type(&stmt_id_val) != VAL_INT || value_get_type(&index_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteBindNull expects (int, int)");
        return;
    }

    int param_index = (int)value_get_int(&index_val);
    if (param_index <= 0) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteBindNull parameter index must be >= 1");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    SqlxStmtHandle* stmt_handle = NULL;
    SqlxDbHandle* db_handle = NULL;
    if (!sqlx_lookup_stmt_with_db((int)value_get_int(&stmt_id_val), &stmt_handle, &db_handle)) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite statement id");
        return;
    }

    if (stmt_handle->finalized || !stmt_handle->stmt) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "SQLite statement is finalized");
        return;
    }

    int rc = SQLX_OK;
    const char* err_msg = NULL;
    syncx_mutex_lock(&db_handle->mutex);
    if (db_handle->closed || !db_handle->db) {
        rc = -1;
        err_msg = "SQLite database is closed";
    } else {
        rc = g_sqlx_api.bind_null(stmt_handle->stmt, param_index);
        if (rc == SQLX_OK) {
            stmt_handle->done = false;
        } else {
            err_msg = sqlx_db_error_message(db_handle->db, "sqliteBindNull failed");
        }
    }
    syncx_mutex_unlock(&db_handle->mutex);

    Value ok;
    value_init_bool(&ok, rc == SQLX_OK);
    if (rc != SQLX_OK) {
        int64_t err_code = (rc == -1 || rc == SQLX_RANGE) ? ERR_INVALID_ARGUMENT : ERR_IO;
        result_tuple_set(vm, slot, ok, err_code, err_msg ? err_msg : "sqliteBindNull failed");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_reset(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value stmt_id_val = *slot;

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    if (value_get_type(&stmt_id_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteReset expects (int)");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    SqlxStmtHandle* stmt_handle = NULL;
    SqlxDbHandle* db_handle = NULL;
    if (!sqlx_lookup_stmt_with_db((int)value_get_int(&stmt_id_val), &stmt_handle, &db_handle)) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite statement id");
        return;
    }

    if (stmt_handle->finalized || !stmt_handle->stmt) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "SQLite statement is finalized");
        return;
    }

    int rc = SQLX_OK;
    const char* err_msg = NULL;
    syncx_mutex_lock(&db_handle->mutex);
    if (db_handle->closed || !db_handle->db) {
        rc = -1;
        err_msg = "SQLite database is closed";
    } else {
        rc = g_sqlx_api.reset(stmt_handle->stmt);
        if (rc == SQLX_OK) {
            stmt_handle->done = false;
        } else {
            err_msg = sqlx_db_error_message(db_handle->db, "sqliteReset failed");
        }
    }
    syncx_mutex_unlock(&db_handle->mutex);

    Value ok;
    value_init_bool(&ok, rc == SQLX_OK);
    if (rc != SQLX_OK) {
        int64_t err_code = (rc == -1 || rc == SQLX_RANGE) ? ERR_INVALID_ARGUMENT : ERR_IO;
        result_tuple_set(vm, slot, ok, err_code, err_msg ? err_msg : "sqliteReset failed");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_clear_bindings(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value stmt_id_val = *slot;

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    if (value_get_type(&stmt_id_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteClearBindings expects (int)");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    SqlxStmtHandle* stmt_handle = NULL;
    SqlxDbHandle* db_handle = NULL;
    if (!sqlx_lookup_stmt_with_db((int)value_get_int(&stmt_id_val), &stmt_handle, &db_handle)) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite statement id");
        return;
    }

    if (stmt_handle->finalized || !stmt_handle->stmt) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "SQLite statement is finalized");
        return;
    }

    int rc = SQLX_OK;
    const char* err_msg = NULL;
    syncx_mutex_lock(&db_handle->mutex);
    if (db_handle->closed || !db_handle->db) {
        rc = -1;
        err_msg = "SQLite database is closed";
    } else {
        rc = g_sqlx_api.clear_bindings(stmt_handle->stmt);
        if (rc == SQLX_OK) {
            stmt_handle->done = false;
        } else {
            err_msg = sqlx_db_error_message(db_handle->db, "sqliteClearBindings failed");
        }
    }
    syncx_mutex_unlock(&db_handle->mutex);

    Value ok;
    value_init_bool(&ok, rc == SQLX_OK);
    if (rc != SQLX_OK) {
        int64_t err_code = (rc == -1 || rc == SQLX_RANGE) ? ERR_INVALID_ARGUMENT : ERR_IO;
        result_tuple_set(vm, slot, ok, err_code, err_msg ? err_msg : "sqliteClearBindings failed");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_changes(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value db_id_val = *slot;

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    if (value_get_type(&db_id_val) != VAL_INT) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteChanges expects (int)");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    int db_id = (int)value_get_int(&db_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SqlxDbHandle* db_handle = sqlx_lookup_db_unlocked(db_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!db_handle) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite db id");
        return;
    }

    int64_t changes = 0;
    const char* err_msg = NULL;
    int rc = SQLX_OK;
    syncx_mutex_lock(&db_handle->mutex);
    if (db_handle->closed || !db_handle->db) {
        rc = -1;
        err_msg = "SQLite database is closed";
    } else {
        changes = (int64_t)g_sqlx_api.changes(db_handle->db);
    }
    syncx_mutex_unlock(&db_handle->mutex);

    Value ok;
    value_init_int(&ok, changes);
    if (rc != SQLX_OK) {
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, err_msg ? err_msg : "SQLite database is closed");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_last_insert_rowid(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value db_id_val = *slot;

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    if (value_get_type(&db_id_val) != VAL_INT) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteLastInsertRowId expects (int)");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    int db_id = (int)value_get_int(&db_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SqlxDbHandle* db_handle = sqlx_lookup_db_unlocked(db_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!db_handle) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite db id");
        return;
    }

    int64_t row_id = 0;
    const char* err_msg = NULL;
    int rc = SQLX_OK;
    syncx_mutex_lock(&db_handle->mutex);
    if (db_handle->closed || !db_handle->db) {
        rc = -1;
        err_msg = "SQLite database is closed";
    } else {
        row_id = (int64_t)g_sqlx_api.last_insert_rowid(db_handle->db);
    }
    syncx_mutex_unlock(&db_handle->mutex);

    Value ok;
    value_init_int(&ok, row_id);
    if (rc != SQLX_OK) {
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, err_msg ? err_msg : "SQLite database is closed");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_finalize(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value stmt_id_val = *slot;

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    if (value_get_type(&stmt_id_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteFinalize expects (int)");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    int stmt_id = (int)value_get_int(&stmt_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SqlxStmtHandle* stmt_handle = sqlx_lookup_stmt_unlocked(stmt_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!stmt_handle) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite statement id");
        return;
    }

    syncx_mutex_lock(&g_syncx_registry_mutex);
    sqlx_stmt_finalize_unlocked(stmt_handle);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_sqlite_step(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value stmt_id_val = *slot;

    if (!vm_is_sqlite_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, slot, ok, "SQLite access is disabled");
        return;
    }

    if (value_get_type(&stmt_id_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "sqliteStep expects (int)");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    int stmt_id = (int)value_get_int(&stmt_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SqlxStmtHandle* stmt_handle = sqlx_lookup_stmt_unlocked(stmt_id);
    SqlxDbHandle* db_handle = stmt_handle ? sqlx_lookup_db_unlocked(stmt_handle->db_id) : NULL;
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!stmt_handle || !db_handle) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown sqlite statement id");
        return;
    }

    if (stmt_handle->finalized || !stmt_handle->stmt) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "SQLite statement is finalized");
        return;
    }

    Value ok;
    int rc = SQLX_OK;
    const char* err_msg = NULL;
    bool has_row = false;
    syncx_mutex_lock(&db_handle->mutex);
    if (db_handle->closed || !db_handle->db) {
        rc = -1;
        err_msg = "SQLite database is closed";
        value_init_nil(&ok);
    } else if (stmt_handle->done) {
        rc = SQLX_DONE;
        value_init_nil(&ok);
    } else {
        rc = g_sqlx_api.step(stmt_handle->stmt);
        if (rc == SQLX_ROW) {
            has_row = sqlx_stmt_row_to_map(vm, stmt_handle->stmt, &ok, &err_msg);
            if (!has_row) {
                rc = -2;
            }
        } else if (rc == SQLX_DONE) {
            stmt_handle->done = true;
            value_init_nil(&ok);
        } else {
            err_msg = sqlx_db_error_message(db_handle->db, "sqliteStep failed");
            value_init_nil(&ok);
        }
    }
    syncx_mutex_unlock(&db_handle->mutex);

    if (rc == SQLX_ROW || rc == SQLX_DONE) {
        result_tuple_set(vm, slot, ok, 0, NULL);
        return;
    }
    if (rc == -1) {
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, err_msg ? err_msg : "SQLite database is closed");
        return;
    }
    if (rc == -2) {
        result_tuple_set(vm, slot, ok, ERR_IO, err_msg ? err_msg : "Failed to decode sqlite row");
        return;
    }
    result_tuple_set(vm, slot, ok, ERR_IO, err_msg ? err_msg : "sqliteStep failed");
}

void builtin_sqlite_query(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value db_id_val = vm->stack.values[vm->stack.count - 2];
    Value sql_val = vm->stack.values[vm->stack.count - 1];

    ObjArray* rows = obj_array_create(vm, 8);
    Value empty_ok;
    value_init_array(&empty_ok, rows);

    if (!vm_is_sqlite_enabled(vm)) {
        result_permission_denied(vm, slot, empty_ok, "SQLite access is disabled");
        return;
    }

    ObjString* sql_str = value_get_string_obj(&sql_val);
    if (value_get_type(&db_id_val) != VAL_INT ||
        value_get_type(&sql_val) != VAL_STRING ||
        !sql_str ||
        !sql_str->chars) {
        result_tuple_set(vm, slot, empty_ok, ERR_INVALID_ARGUMENT, "sqliteQuery expects (int, string)");
        return;
    }

    syncx_runtime_ensure_init();
    sqlx_api_ensure_loaded();
    if (!g_sqlx_api.available) {
        result_tuple_set(vm, slot, empty_ok, ERR_UNSUPPORTED, "SQLite runtime is unavailable on this build");
        return;
    }

    int db_id = (int)value_get_int(&db_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    SqlxDbHandle* db_handle = sqlx_lookup_db_unlocked(db_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);
    if (!db_handle) {
        result_tuple_set(vm, slot, empty_ok, ERR_INVALID_ARGUMENT, "Unknown sqlite db id");
        return;
    }

    SqlxNativeStmt* stmt = NULL;
    int rc = SQLX_OK;
    const char* err_msg = NULL;
    int64_t err_code = 0;

    syncx_mutex_lock(&db_handle->mutex);
    if (db_handle->closed || !db_handle->db) {
        err_code = ERR_INVALID_ARGUMENT;
        err_msg = "SQLite database is closed";
    } else {
        rc = g_sqlx_api.prepare_v2(db_handle->db, sql_str->chars, -1, &stmt, NULL);
        if (rc != SQLX_OK || !stmt) {
            err_code = ERR_IO;
            err_msg = sqlx_db_error_message(db_handle->db, "Failed to prepare sqlite query");
        }
    }

    while (err_code == 0) {
        rc = g_sqlx_api.step(stmt);
        if (rc == SQLX_DONE) {
            break;
        }
        if (rc != SQLX_ROW) {
            err_code = ERR_IO;
            err_msg = sqlx_db_error_message(db_handle->db, "SQLite query failed");
            break;
        }

        int max_array_size = (vm->config.max_array_size > 0) ? vm->config.max_array_size : INT_MAX;
        if (rows->count >= max_array_size) {
            err_code = ERR_LIMIT;
            err_msg = "sqliteQuery result exceeds max array size";
            break;
        }

        Value row_val;
        if (!sqlx_stmt_row_to_map(vm, stmt, &row_val, &err_msg)) {
            err_code = ERR_IO;
            if (!err_msg) err_msg = "Failed to decode sqlite row";
            break;
        }
        obj_array_push(rows, row_val);
    }

    if (stmt) {
        (void)g_sqlx_api.finalize(stmt);
    }
    syncx_mutex_unlock(&db_handle->mutex);

    if (err_code != 0) {
        result_tuple_set(vm, slot, empty_ok, err_code, err_msg ? err_msg : "SQLite query failed");
        return;
    }
    result_tuple_set(vm, slot, empty_ok, 0, NULL);
}

void builtin_process_spawn(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 4];
    Value command_val = vm->stack.values[vm->stack.count - 4];
    Value args_val = vm->stack.values[vm->stack.count - 3];
    Value capture_stdout_val = vm->stack.values[vm->stack.count - 2];
    Value capture_stderr_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_process_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "Process execution is disabled");
        return;
    }

    if (value_get_type(&capture_stdout_val) != VAL_BOOL || value_get_type(&capture_stderr_val) != VAL_BOOL) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "processSpawn expects (string, array<string>, bool, bool)");
        return;
    }

    char** argv = NULL;
    int argc = 0;
    const char* argv_err = NULL;
    if (!procx_build_argv(&command_val, &args_val, &argv, &argc, &argv_err)) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, argv_err ? argv_err : "Invalid processSpawn arguments");
        return;
    }

    bool capture_stdout = value_get_bool(&capture_stdout_val);
    bool capture_stderr = value_get_bool(&capture_stderr_val);

    syncx_runtime_ensure_init();

    syncx_mutex_lock(&g_syncx_registry_mutex);
    int process_id = g_procx_next_handle_id++;
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    ProcxHandle* handle = procx_handle_create(process_id);
    const char* spawn_err = NULL;

    syncx_mutex_lock(&handle->mutex);
    bool spawned = procx_spawn_process_locked(handle, argv, argc, capture_stdout, capture_stderr, &spawn_err);
    syncx_mutex_unlock(&handle->mutex);

    procx_free_argv(argv, argc);

    if (!spawned) {
        procx_handle_dispose(handle);
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INTERNAL, spawn_err ? spawn_err : "Failed to spawn process");
        return;
    }

    syncx_mutex_lock(&g_syncx_registry_mutex);
    procx_append_unlocked(handle);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    Value ok;
    value_init_int(&ok, process_id);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_process_write_stdin(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value process_id_val = vm->stack.values[vm->stack.count - 2];
    Value data_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_process_enabled(vm)) {
        Value ok;
        value_init_int(&ok, 0);
        result_permission_denied(vm, slot, ok, "Process execution is disabled");
        return;
    }

    ObjString* data_str = value_get_string_obj(&data_val);
    if (value_get_type(&process_id_val) != VAL_INT ||
        value_get_type(&data_val) != VAL_STRING ||
        !data_str ||
        !data_str->chars) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "processWriteStdin expects (int, string)");
        return;
    }

    syncx_runtime_ensure_init();

    int process_id = (int)value_get_int(&process_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    ProcxHandle* handle = procx_lookup_unlocked(process_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!handle) {
        Value ok;
        value_init_int(&ok, 0);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown process id");
        return;
    }

    const char* data = data_str->chars;
    size_t data_len = (size_t)(data_str->length < 0 ? 0 : data_str->length);
    size_t total_written = 0;
    int64_t err_code = 0;
    const char* err_msg = NULL;

    syncx_mutex_lock(&handle->mutex);

    if (handle->finished) {
        err_code = ERR_IO;
        err_msg = "Process already finished";
    } else {
#ifdef _WIN32
        if (!handle->stdin_write) {
            err_code = ERR_IO;
            err_msg = "Process stdin is closed";
        } else {
            while (total_written < data_len) {
                DWORD remaining = (DWORD)(data_len - total_written);
                if (remaining > 65536u) remaining = 65536u;
                DWORD wrote = 0;
                if (!WriteFile(handle->stdin_write, data + total_written, remaining, &wrote, NULL)) {
                    DWORD last_error = GetLastError();
                    if (last_error == ERROR_BROKEN_PIPE || last_error == ERROR_NO_DATA || last_error == ERROR_PIPE_NOT_CONNECTED) {
                        procx_close_stdin_locked(handle);
                        err_msg = "Process stdin is closed";
                    } else {
                        err_msg = "Failed writing process stdin";
                    }
                    err_code = ERR_IO;
                    break;
                }
                if (wrote == 0) {
                    err_code = ERR_IO;
                    err_msg = "Failed writing process stdin";
                    break;
                }
                total_written += (size_t)wrote;
            }
        }
#else
        if (handle->stdin_fd < 0) {
            err_code = ERR_IO;
            err_msg = "Process stdin is closed";
        } else {
            while (total_written < data_len) {
                ssize_t wrote = write(handle->stdin_fd, data + total_written, data_len - total_written);
                if (wrote > 0) {
                    total_written += (size_t)wrote;
                    continue;
                }
                if (wrote == 0) {
                    err_code = ERR_IO;
                    err_msg = "Failed writing process stdin";
                    break;
                }
                if (errno == EINTR) continue;
                if (errno == EPIPE || errno == ECONNRESET) {
                    procx_close_stdin_locked(handle);
                    err_code = ERR_IO;
                    err_msg = "Process stdin is closed";
                    break;
                }
                err_code = ERR_IO;
                err_msg = "Failed writing process stdin";
                break;
            }
        }
#endif
    }

    syncx_mutex_unlock(&handle->mutex);

    Value ok;
    int64_t wrote_out = total_written > (size_t)INT64_MAX ? INT64_MAX : (int64_t)total_written;
    value_init_int(&ok, wrote_out);
    if (err_code != 0) {
        result_tuple_set(vm, slot, ok, err_code, err_msg ? err_msg : "Failed writing process stdin");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_process_close_stdin(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value process_id_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_process_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "Process execution is disabled");
        return;
    }

    if (value_get_type(&process_id_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "processCloseStdin expects (int)");
        return;
    }

    syncx_runtime_ensure_init();

    int process_id = (int)value_get_int(&process_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    ProcxHandle* handle = procx_lookup_unlocked(process_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!handle) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown process id");
        return;
    }

    syncx_mutex_lock(&handle->mutex);
    procx_close_stdin_locked(handle);
    syncx_mutex_unlock(&handle->mutex);

    Value ok;
    value_init_bool(&ok, true);
    result_tuple_set(vm, slot, ok, 0, NULL);
}

static Value procx_read_output_chunk_locked(ProcxHandle* handle,
                                            bool want_stdout,
                                            size_t max_bytes,
                                            int64_t* out_err_code,
                                            const char** out_err_msg) {
    Value out;
    value_init_nil(&out);
    if (out_err_code) *out_err_code = 0;
    if (out_err_msg) *out_err_msg = NULL;
    if (!handle) {
        if (out_err_code) *out_err_code = ERR_INVALID_ARGUMENT;
        if (out_err_msg) *out_err_msg = "Unknown process id";
        return out;
    }

    if (want_stdout) {
        if (!handle->capture_stdout) {
            if (out_err_code) *out_err_code = ERR_INVALID_ARGUMENT;
            if (out_err_msg) *out_err_msg = "Process stdout was not captured";
            return out;
        }
    } else {
        if (!handle->capture_stderr) {
            if (out_err_code) *out_err_code = ERR_INVALID_ARGUMENT;
            if (out_err_msg) *out_err_msg = "Process stderr was not captured";
            return out;
        }
    }

    procx_pump_outputs_locked(handle);

    if (want_stdout) {
        return procx_buffer_take_chunk(&handle->stdout_buf,
                                       &handle->stdout_len,
                                       &handle->stdout_cap,
                                       max_bytes);
    }
    return procx_buffer_take_chunk(&handle->stderr_buf,
                                   &handle->stderr_len,
                                   &handle->stderr_cap,
                                   max_bytes);
}

void builtin_process_read_stdout(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value process_id_val = vm->stack.values[vm->stack.count - 2];
    Value max_bytes_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_process_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, slot, ok, "Process execution is disabled");
        return;
    }

    if (value_get_type(&process_id_val) != VAL_INT || value_get_type(&max_bytes_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "processReadStdout expects (int, int)");
        return;
    }

    int64_t max_bytes64 = value_get_int(&max_bytes_val);
    if (max_bytes64 < 1 || max_bytes64 > 1024 * 1024) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "maxBytes must be between 1 and 1048576");
        return;
    }

    syncx_runtime_ensure_init();

    int process_id = (int)value_get_int(&process_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    ProcxHandle* handle = procx_lookup_unlocked(process_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!handle) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown process id");
        return;
    }

    int64_t err_code = 0;
    const char* err_msg = NULL;
    Value ok;
    syncx_mutex_lock(&handle->mutex);
    ok = procx_read_output_chunk_locked(handle, true, (size_t)max_bytes64, &err_code, &err_msg);
    syncx_mutex_unlock(&handle->mutex);

    if (err_code != 0) {
        result_tuple_set(vm, slot, ok, err_code, err_msg ? err_msg : "Failed reading process stdout");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_process_read_stderr(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value process_id_val = vm->stack.values[vm->stack.count - 2];
    Value max_bytes_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_process_enabled(vm)) {
        Value ok;
        value_init_nil(&ok);
        result_permission_denied(vm, slot, ok, "Process execution is disabled");
        return;
    }

    if (value_get_type(&process_id_val) != VAL_INT || value_get_type(&max_bytes_val) != VAL_INT) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "processReadStderr expects (int, int)");
        return;
    }

    int64_t max_bytes64 = value_get_int(&max_bytes_val);
    if (max_bytes64 < 1 || max_bytes64 > 1024 * 1024) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "maxBytes must be between 1 and 1048576");
        return;
    }

    syncx_runtime_ensure_init();

    int process_id = (int)value_get_int(&process_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    ProcxHandle* handle = procx_lookup_unlocked(process_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!handle) {
        Value ok;
        value_init_nil(&ok);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown process id");
        return;
    }

    int64_t err_code = 0;
    const char* err_msg = NULL;
    Value ok;
    syncx_mutex_lock(&handle->mutex);
    ok = procx_read_output_chunk_locked(handle, false, (size_t)max_bytes64, &err_code, &err_msg);
    syncx_mutex_unlock(&handle->mutex);

    if (err_code != 0) {
        result_tuple_set(vm, slot, ok, err_code, err_msg ? err_msg : "Failed reading process stderr");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_process_wait(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 2];
    Value process_id_val = vm->stack.values[vm->stack.count - 2];
    Value timeout_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_process_enabled(vm)) {
        Value ok;
        value_init_map(&ok, obj_map_create(vm));
        result_permission_denied(vm, slot, ok, "Process execution is disabled");
        return;
    }

    if (value_get_type(&process_id_val) != VAL_INT || value_get_type(&timeout_val) != VAL_INT) {
        Value ok;
        value_init_map(&ok, obj_map_create(vm));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "processWait expects (int, int)");
        return;
    }

    int64_t timeout64 = value_get_int(&timeout_val);
    if (timeout64 < 0 || timeout64 > 120000) {
        Value ok;
        value_init_map(&ok, obj_map_create(vm));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "timeoutMs must be between 0 and 120000");
        return;
    }

    syncx_runtime_ensure_init();

    int process_id = (int)value_get_int(&process_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    ProcxHandle* handle = procx_lookup_unlocked(process_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!handle) {
        Value ok;
        value_init_map(&ok, obj_map_create(vm));
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown process id");
        return;
    }

    bool timed_out = false;
    syncx_mutex_lock(&handle->mutex);
    (void)procx_wait_locked(handle, (int)timeout64, &timed_out);
    Value ok = procx_build_result_map(vm, handle, timed_out);
    syncx_mutex_unlock(&handle->mutex);

    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_process_kill(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];
    Value process_id_val = vm->stack.values[vm->stack.count - 1];

    if (!vm_is_process_enabled(vm)) {
        Value ok;
        value_init_bool(&ok, false);
        result_permission_denied(vm, slot, ok, "Process execution is disabled");
        return;
    }

    if (value_get_type(&process_id_val) != VAL_INT) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "processKill expects (int)");
        return;
    }

    syncx_runtime_ensure_init();

    int process_id = (int)value_get_int(&process_id_val);
    syncx_mutex_lock(&g_syncx_registry_mutex);
    ProcxHandle* handle = procx_lookup_unlocked(process_id);
    syncx_mutex_unlock(&g_syncx_registry_mutex);

    if (!handle) {
        Value ok;
        value_init_bool(&ok, false);
        result_tuple_set(vm, slot, ok, ERR_INVALID_ARGUMENT, "Unknown process id");
        return;
    }

    syncx_mutex_lock(&handle->mutex);
    bool killed = procx_kill_locked(handle);
    if (killed) {
        (void)procx_poll_exit_locked(handle);
        procx_pump_outputs_locked(handle);
    }
    syncx_mutex_unlock(&handle->mutex);

    Value ok;
    value_init_bool(&ok, killed);
    if (!killed) {
        result_tuple_set(vm, slot, ok, ERR_IO, "Failed to terminate process");
        return;
    }
    result_tuple_set(vm, slot, ok, 0, NULL);
}

void builtin_gc_collect(VM* vm) {
    Value* slot = &vm->stack.values[vm->stack.count - 1];

    Value result;
    value_init_int(&result, (int64_t)vm_gc_collect(vm));

    value_free(slot);
    *slot = result;
}

void register_builtins(VM* vm) {
    vm_register_native(vm, "print", builtin_print, 1);
    vm_register_native(vm, "println", builtin_println, 1);
    vm_register_native(vm, "panic", builtin_panic, 1);
    vm_register_native(vm, "must", builtin_must, 1);
    vm_register_native(vm, "wrapError", builtin_wrap_error, 2);
    register_error_code_constants(vm);
    vm_register_native(vm, "len", builtin_len, 1);
    vm_register_native(vm, "arrayWithSize", builtin_array_with_size, 2);
    vm_register_native(vm, "bytesWithSize", builtin_bytes_with_size, 2);
    vm_register_native(vm, "typeOf", builtin_typeOf, 1);
    vm_register_native(vm, "futurePending", builtin_future_pending, 0);
    vm_register_native(vm, "futureResolved", builtin_future_resolved, 1);
    vm_register_native(vm, "futureIsReady", builtin_future_is_ready, 1);
    vm_register_native(vm, "futureComplete", builtin_future_complete, 2);
    vm_register_native(vm, "futureGet", builtin_future_get, 1);
    vm_register_native(vm, "extPostedCallbackPendingCount", builtin_ext_posted_callback_pending_count, 0);
    vm_register_native(vm, "extDrainPostedCallbacks", builtin_ext_drain_posted_callbacks, 1);
    vm_register_native(vm, "extSetPostedCallbackAutoDrain", builtin_ext_set_posted_callback_auto_drain, 1);
    vm_register_native(vm, "asyncSleep", builtin_async_sleep, 1);
    vm_register_native(vm, "asyncChannelSend", builtin_async_channel_send, 2);
    vm_register_native(vm, "asyncChannelSendTyped", builtin_async_channel_send_typed, 3);
    vm_register_native(vm, "asyncChannelRecv", builtin_async_channel_recv, 1);
    vm_register_native(vm, "asyncChannelRecvTyped", builtin_async_channel_recv_typed, 2);
    vm_register_native(vm, "toInt", builtin_toInt, 1);
    vm_register_native(vm, "toDouble", builtin_toDouble, 1);
    vm_register_native(vm, "toBigInt", builtin_toBigInt, 1);
    vm_register_native(vm, "toHexBigInt", builtin_toHexBigInt, 1);
    vm_register_native(vm, "bytesToHex", builtin_bytesToHex, 1);
    vm_register_native(vm, "hexToBytes", builtin_hexToBytes, 1);
    vm_register_native(vm, "stringToBytes", builtin_string_to_bytes, 1);
    vm_register_native(vm, "bytesToString", builtin_bytes_to_string, 1);
    vm_register_native(vm, "sha256Bytes", builtin_sha256_bytes, 1);
    vm_register_native(vm, "hmacSha256Bytes", builtin_hmac_sha256_bytes, 2);
    vm_register_native(vm, "pbkdf2HmacSha256Bytes", builtin_pbkdf2_hmac_sha256_bytes, 4);
    vm_register_native(vm, "hkdfHmacSha256Bytes", builtin_hkdf_hmac_sha256_bytes, 4);
    vm_register_native(vm, "constantTimeBytesEqual", builtin_constant_time_bytes_equal, 2);
    vm_register_native(vm, "aesCtrBytes", builtin_aes_ctr_bytes, 3);
    vm_register_native(vm, "aesGcmSealBytes", builtin_aes_gcm_seal_bytes, 4);
    vm_register_native(vm, "aesGcmOpenBytes", builtin_aes_gcm_open_bytes, 4);
    vm_register_native(vm, "bytesJoin", builtin_bytes_join, 1);
    vm_register_native(vm, "urlEncode", builtin_url_encode, 1);
    vm_register_native(vm, "urlDecode", builtin_url_decode, 1);
    vm_register_native(vm, "str", builtin_str, 1);
    vm_register_native(vm, "formatDouble", builtin_format_double, 2);
    vm_register_native(vm, "jsonParse", builtin_json_parse, 1);
    vm_register_native(vm, "jsonStringify", builtin_json_stringify, 1);
    vm_register_native(vm, "jsonStringifyPretty", builtin_json_stringify_pretty, 1);
    vm_register_native(vm, "jsonDecode", builtin_json_decode, 2);
    vm_register_native(vm, "push", builtin_push, 2);
    vm_register_native(vm, "pop", builtin_pop, 1);
    vm_register_native(vm, "copyInto", builtin_copy_into, 2);
    vm_register_native(vm, "reversePrefix", builtin_reverse_prefix, 2);
    vm_register_native(vm, "rotatePrefixLeft", builtin_rotate_prefix_left, 2);
    vm_register_native(vm, "rotatePrefixRight", builtin_rotate_prefix_right, 2);
    vm_register_native(vm, "keys", builtin_keys, 1);
    vm_register_native(vm, "values", builtin_values, 1);
    vm_register_native(vm, "read_line", builtin_read_line, 1);
    vm_register_native(vm, "read_all", builtin_read_all, 1);
    vm_register_native(vm, "write_line", builtin_write_line, 2);
    vm_register_native(vm, "write_all", builtin_write_all, 2);
    vm_register_native(vm, "file_open", builtin_file_open, 2);
    vm_register_native(vm, "file_read_line", builtin_file_read_line, 1);
    vm_register_native(vm, "file_close", builtin_file_close, 1);
    vm_register_native(vm, "ioReadLine", builtin_io_read_line, 1);
    vm_register_native(vm, "ioReadAll", builtin_io_read_all, 1);
    vm_register_native(vm, "ioReadChunk", builtin_io_read_chunk, 2);
    vm_register_native(vm, "ioReadChunkBytes", builtin_io_read_chunk_bytes, 2);
    vm_register_native(vm, "ioReadExactlyBytes", builtin_io_read_exactly_bytes, 2);
    vm_register_native(vm, "ioWriteAll", builtin_io_write_all, 2);
    vm_register_native(vm, "ioWriteBytesAll", builtin_io_write_bytes_all, 2);
    vm_register_native(vm, "ioCopy", builtin_io_copy, 3);
    vm_register_native(vm, "readBytes", builtin_read_bytes, 1);
    vm_register_native(vm, "writeBytes", builtin_write_bytes, 2);
    vm_register_native(vm, "appendBytes", builtin_append_bytes, 2);
    vm_register_native(vm, "stdoutWriteBytes", builtin_stdout_write_bytes, 1);
    vm_register_native(vm, "envGet", builtin_env_get, 1);
    vm_register_native(vm, "exists", builtin_exists, 1);
    vm_register_native(vm, "delete", builtin_delete, 1);
    vm_register_native(vm, "gcCollect", builtin_gc_collect, 0);
    // Map builtins
    vm_register_native(vm, "mapGet", builtin_map_get, 2);
    vm_register_native(vm, "mapSet", builtin_map_set, 3);
    vm_register_native(vm, "mapHas", builtin_map_has, 2);
    vm_register_native(vm, "mapGetString", builtin_map_get_string, 2);
    vm_register_native(vm, "mapSetString", builtin_map_set_string, 3);
    vm_register_native(vm, "mapHasString", builtin_map_has_string, 2);
    vm_register_native(vm, "mapDeleteString", builtin_map_delete_string, 2);
    vm_register_native(vm, "mapDelete", builtin_map_delete, 2);
    vm_register_native(vm, "mapCount", builtin_map_count, 1);
    // Set builtins
    vm_register_native(vm, "setAdd", builtin_set_add, 2);
    vm_register_native(vm, "setAddString", builtin_set_add_string, 2);
    vm_register_native(vm, "setHas", builtin_set_has, 2);
    vm_register_native(vm, "setHasString", builtin_set_has_string, 2);
    vm_register_native(vm, "setRemove", builtin_set_remove, 2);
    vm_register_native(vm, "setRemoveString", builtin_set_remove_string, 2);
    vm_register_native(vm, "setCount", builtin_set_count, 1);
    vm_register_native(vm, "setToArray", builtin_set_to_array, 1);
    // String functions
    vm_register_native(vm, "substring", builtin_substring, 3);
    vm_register_native(vm, "find", builtin_find, 2);
    vm_register_native(vm, "split", builtin_split, 2);
    vm_register_native(vm, "trim", builtin_trim, 1);
    vm_register_native(vm, "startsWith", builtin_starts_with, 2);
    vm_register_native(vm, "endsWith", builtin_ends_with, 2);
    vm_register_native(vm, "replace", builtin_replace, 3);
    // BigInt functions
    vm_register_native(vm, "absBigInt", builtin_abs_bigint, 1);
    vm_register_native(vm, "signBigInt", builtin_sign_bigint, 1);
    vm_register_native(vm, "digitsBigInt", builtin_digits_bigint, 1);
    vm_register_native(vm, "isEvenBigInt", builtin_is_even_bigint, 1);
    vm_register_native(vm, "isOddBigInt", builtin_is_odd_bigint, 1);
    vm_register_native(vm, "powBigInt", builtin_pow_bigint, 2);
    vm_register_native(vm, "gcdBigInt", builtin_gcd_bigint, 2);
    vm_register_native(vm, "lcmBigInt", builtin_lcm_bigint, 2);
    vm_register_native(vm, "modPowBigInt", builtin_mod_pow_bigint, 3);
    vm_register_native(vm, "modInverseBigInt", builtin_mod_inverse_bigint, 2);
    vm_register_native(vm, "isProbablePrimeBigInt", builtin_is_probable_prime_bigint, 2);
    vm_register_native(vm, "compareBigInt", builtin_compare_bigint, 2);
    vm_register_native(vm, "absCmpBigInt", builtin_abs_cmp_bigint, 2);
    vm_register_native(vm, "clampBigInt", builtin_clamp_bigint, 3);
    vm_register_native(vm, "isZeroBigInt", builtin_is_zero_bigint, 1);
    vm_register_native(vm, "isNegativeBigInt", builtin_is_negative_bigint, 1);
    // Math functions
    vm_register_native(vm, "absInt", builtin_abs_int, 1);
    vm_register_native(vm, "absDouble", builtin_abs_double, 1);
    vm_register_native(vm, "min", builtin_min, 2);
    vm_register_native(vm, "max", builtin_max, 2);
    vm_register_native(vm, "floor", builtin_floor, 1);
    vm_register_native(vm, "ceil", builtin_ceil, 1);
    vm_register_native(vm, "round", builtin_round, 1);
    vm_register_native(vm, "sqrt", builtin_sqrt, 1);
    vm_register_native(vm, "pow", builtin_pow, 2);
    vm_register_native(vm, "random", builtin_random, 0);
    vm_register_native(vm, "randomSeed", builtin_random_seed, 1);
    vm_register_native(vm, "randomInt", builtin_random_int, 2);
    vm_register_native(vm, "randomDouble", builtin_random_double, 2);
    vm_register_native(vm, "randomBigIntBits", builtin_random_bigint_bits, 1);
    vm_register_native(vm, "randomBigIntRange", builtin_random_bigint_range, 2);
    vm_register_native(vm, "randomFillInt", builtin_random_fill_int, 3);
    vm_register_native(vm, "randomFillDouble", builtin_random_fill_double, 3);
    vm_register_native(vm, "randomFillBigIntBits", builtin_random_fill_bigint_bits, 2);
    vm_register_native(vm, "randomFillBigIntRange", builtin_random_fill_bigint_range, 3);
    vm_register_native(vm, "secureRandom", builtin_secure_random, 0);
    vm_register_native(vm, "secureRandomInt", builtin_secure_random_int, 2);
    vm_register_native(vm, "secureRandomDouble", builtin_secure_random_double, 2);
    vm_register_native(vm, "secureRandomBigIntBits", builtin_secure_random_bigint_bits, 1);
    vm_register_native(vm, "secureRandomBigIntRange", builtin_secure_random_bigint_range, 2);
    vm_register_native(vm, "secureRandomFillInt", builtin_secure_random_fill_int, 3);
    vm_register_native(vm, "secureRandomFillDouble", builtin_secure_random_fill_double, 3);
    vm_register_native(vm, "secureRandomFillBigIntBits", builtin_secure_random_fill_bigint_bits, 2);
    vm_register_native(vm, "secureRandomFillBigIntRange", builtin_secure_random_fill_bigint_range, 3);
    // Time/Date functions
    vm_register_native(vm, "timeNowMillis", builtin_time_now_millis, 0);
    vm_register_native(vm, "timeNowNanos", builtin_time_now_nanos, 0);
    vm_register_native(vm, "timeMonotonicMillis", builtin_time_monotonic_millis, 0);
    vm_register_native(vm, "timeSinceMillis", builtin_time_since_millis, 1);
    vm_register_native(vm, "utcDateTime", builtin_utc_datetime, 0);
    vm_register_native(vm, "localDateTime", builtin_local_datetime, 0);
    vm_register_native(vm, "logJson", builtin_log_json, 3);
    // Array functions
    vm_register_native(vm, "sort", builtin_sort, 1);
    vm_register_native(vm, "reverse", builtin_reverse, 1);
    vm_register_native(vm, "findArray", builtin_find_array, 2);
    vm_register_native(vm, "contains", builtin_contains, 2);
    vm_register_native(vm, "slice", builtin_slice, 3);
    vm_register_native(vm, "join", builtin_join, 2);
    // HTTP client functions
    vm_register_native(vm, "httpGet", builtin_http_get, 1);
    vm_register_native(vm, "httpGetWithHeaders", builtin_http_get_with_headers, 2);
    vm_register_native(vm, "httpPost", builtin_http_post, 2);
    vm_register_native(vm, "httpPostWithHeaders", builtin_http_post_with_headers, 3);
    vm_register_native(vm, "httpRequest", builtin_http_request, 5);
    vm_register_native(vm, "httpRequestHead", builtin_http_request_head, 5);
    vm_register_native(vm, "httpRequestWithOptions", builtin_http_request_with_options, 6);
    vm_register_native(vm, "httpRequestHeadWithOptions", builtin_http_request_head_with_options, 6);
    vm_register_native(vm, "httpReadRequest", builtin_http_read_request, 2);
    vm_register_native(vm, "httpWriteResponse", builtin_http_write_response, 4);
    // TCP socket functions
    vm_register_native(vm, "tcpListen", builtin_tcp_listen, 2);
    vm_register_native(vm, "tcpAccept", builtin_tcp_accept, 2);
    vm_register_native(vm, "tcpConnect", builtin_tcp_connect, 2);
    vm_register_native(vm, "tcpSend", builtin_tcp_send, 2);
    vm_register_native(vm, "tcpReceive", builtin_tcp_receive, 2);
    vm_register_native(vm, "tcpClose", builtin_tcp_close, 1);
    vm_register_native(vm, "tlsIsAvailable", builtin_tls_is_available, 0);
    vm_register_native(vm, "tlsConnect", builtin_tls_connect, 3);
    vm_register_native(vm, "tlsSend", builtin_tls_send, 2);
    vm_register_native(vm, "tlsReceive", builtin_tls_receive, 2);
    vm_register_native(vm, "tlsClose", builtin_tls_close, 1);
    // Concurrency and threading functions
    vm_register_native(vm, "syncChannelCreate", builtin_sync_channel_create, 1);
    vm_register_native(vm, "syncChannelSend", builtin_sync_channel_send, 3);
    vm_register_native(vm, "syncChannelSendTyped", builtin_sync_channel_send_typed, 4);
    vm_register_native(vm, "syncChannelRecv", builtin_sync_channel_recv, 2);
    vm_register_native(vm, "syncChannelRecvTyped", builtin_sync_channel_recv_typed, 3);
    vm_register_native(vm, "syncChannelClose", builtin_sync_channel_close, 1);
    vm_register_native(vm, "syncSharedCreate", builtin_sync_shared_create, 1);
    vm_register_native(vm, "syncSharedCreateTyped", builtin_sync_shared_create_typed, 2);
    vm_register_native(vm, "syncSharedGet", builtin_sync_shared_get, 1);
    vm_register_native(vm, "syncSharedGetTyped", builtin_sync_shared_get_typed, 2);
    vm_register_native(vm, "syncSharedSet", builtin_sync_shared_set, 2);
    vm_register_native(vm, "syncSharedSetTyped", builtin_sync_shared_set_typed, 3);
    vm_register_native(vm, "syncThreadSpawn", builtin_sync_thread_spawn, 3);
    vm_register_native(vm, "syncThreadSpawnTyped", builtin_sync_thread_spawn_typed, 5);
    vm_register_native(vm, "syncThreadJoin", builtin_sync_thread_join, 2);
    vm_register_native(vm, "syncThreadJoinTyped", builtin_sync_thread_join_typed, 3);
    vm_register_native(vm, "syncThreadInbox", builtin_sync_thread_inbox, 0);
    vm_register_native(vm, "syncThreadOutbox", builtin_sync_thread_outbox, 0);
    vm_register_native(vm, "syncThreadArgTyped", builtin_sync_thread_arg_typed, 1);
    vm_register_native(vm, "syncArcCreate", builtin_sync_arc_create, 1);
    vm_register_native(vm, "syncArcClone", builtin_sync_arc_clone, 1);
    vm_register_native(vm, "syncArcGuardAcquire", builtin_sync_arc_guard_acquire, 1);
    vm_register_native(vm, "syncArcGuardRead", builtin_sync_arc_guard_read, 1);
    vm_register_native(vm, "syncArcGuardWrite", builtin_sync_arc_guard_write, 2);
    vm_register_native(vm, "syncArcGuardRelease", builtin_sync_arc_guard_release, 1);
    // SQLite functions
    vm_register_native(vm, "sqliteIsAvailable", builtin_sqlite_is_available, 0);
    vm_register_native(vm, "sqliteOpen", builtin_sqlite_open, 1);
    vm_register_native(vm, "sqliteClose", builtin_sqlite_close, 1);
    vm_register_native(vm, "sqliteExec", builtin_sqlite_exec, 2);
    vm_register_native(vm, "sqliteQuery", builtin_sqlite_query, 2);
    vm_register_native(vm, "sqlitePrepare", builtin_sqlite_prepare, 2);
    vm_register_native(vm, "sqliteBindInt", builtin_sqlite_bind_int, 3);
    vm_register_native(vm, "sqliteBindDouble", builtin_sqlite_bind_double, 3);
    vm_register_native(vm, "sqliteBindString", builtin_sqlite_bind_string, 3);
    vm_register_native(vm, "sqliteBindBytes", builtin_sqlite_bind_bytes, 3);
    vm_register_native(vm, "sqliteBindNull", builtin_sqlite_bind_null, 2);
    vm_register_native(vm, "sqliteReset", builtin_sqlite_reset, 1);
    vm_register_native(vm, "sqliteClearBindings", builtin_sqlite_clear_bindings, 1);
    vm_register_native(vm, "sqliteChanges", builtin_sqlite_changes, 1);
    vm_register_native(vm, "sqliteLastInsertRowId", builtin_sqlite_last_insert_rowid, 1);
    vm_register_native(vm, "sqliteStep", builtin_sqlite_step, 1);
    vm_register_native(vm, "sqliteFinalize", builtin_sqlite_finalize, 1);
    // Process functions
    vm_register_native(vm, "processSpawn", builtin_process_spawn, 4);
    vm_register_native(vm, "processWriteStdin", builtin_process_write_stdin, 2);
    vm_register_native(vm, "processCloseStdin", builtin_process_close_stdin, 1);
    vm_register_native(vm, "processReadStdout", builtin_process_read_stdout, 2);
    vm_register_native(vm, "processReadStderr", builtin_process_read_stderr, 2);
    vm_register_native(vm, "processWait", builtin_process_wait, 2);
    vm_register_native(vm, "processKill", builtin_process_kill, 1);
}

