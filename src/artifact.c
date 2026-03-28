#include "artifact.h"
#include "safe_alloc.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARTIFACT_MAGIC "TBLC"
#define ARTIFACT_MAGIC_SIZE 4
#define ARTIFACT_VERSION 16u
#define ARTIFACT_MIN_SUPPORTED_VERSION 2u
#define ARTIFACT_MAX_COUNT 100000000u
#define ARTIFACT_MAX_STRING_BYTES (1u << 28)

static void set_error(char* error_buf, size_t error_buf_size, const char* fmt, ...) {
    if (!error_buf || error_buf_size == 0) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(error_buf, error_buf_size, fmt, args);
    va_end(args);
}

static const char* artifact_strerror(int errnum, char* scratch, size_t scratch_size) {
#ifdef _WIN32
    if (!scratch || scratch_size == 0) return "unknown error";
    if (strerror_s(scratch, scratch_size, errnum) != 0) {
        snprintf(scratch, scratch_size, "errno=%d", errnum);
    }
    return scratch;
#else
    (void)scratch;
    (void)scratch_size;
    return strerror(errnum);
#endif
}

static bool write_u8(FILE* f, uint8_t v) {
    return fwrite(&v, sizeof(v), 1, f) == 1;
}

static bool write_u32_le(FILE* f, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xffu);
    b[1] = (uint8_t)((v >> 8) & 0xffu);
    b[2] = (uint8_t)((v >> 16) & 0xffu);
    b[3] = (uint8_t)((v >> 24) & 0xffu);
    return fwrite(b, sizeof(b), 1, f) == 1;
}

static bool write_i32_le(FILE* f, int32_t v) {
    return write_u32_le(f, (uint32_t)v);
}

static bool write_u64_le(FILE* f, uint64_t v) {
    uint8_t b[8];
    b[0] = (uint8_t)(v & 0xffu);
    b[1] = (uint8_t)((v >> 8) & 0xffu);
    b[2] = (uint8_t)((v >> 16) & 0xffu);
    b[3] = (uint8_t)((v >> 24) & 0xffu);
    b[4] = (uint8_t)((v >> 32) & 0xffu);
    b[5] = (uint8_t)((v >> 40) & 0xffu);
    b[6] = (uint8_t)((v >> 48) & 0xffu);
    b[7] = (uint8_t)((v >> 56) & 0xffu);
    return fwrite(b, sizeof(b), 1, f) == 1;
}

static bool write_i64_le(FILE* f, int64_t v) {
    return write_u64_le(f, (uint64_t)v);
}

static bool write_double_le(FILE* f, double v) {
    union {
        uint64_t u;
        double d;
    } x;
    x.d = v;
    return write_u64_le(f, x.u);
}

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t offset;
} ArtifactReader;

static bool artifact_reader_read(ArtifactReader* reader, void* out, size_t bytes) {
    if (!reader || !out) return false;
    if (bytes > reader->size || reader->offset > reader->size - bytes) return false;
    memcpy(out, reader->data + reader->offset, bytes);
    reader->offset += bytes;
    return true;
}

static bool read_u8(ArtifactReader* reader, uint8_t* out) {
    return artifact_reader_read(reader, out, sizeof(*out));
}

static bool read_u32_le(ArtifactReader* reader, uint32_t* out) {
    if (!out) return false;
    uint8_t b[4];
    if (!artifact_reader_read(reader, b, sizeof(b))) return false;
    *out = ((uint32_t)b[0]) |
           ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
    return true;
}

static bool read_i32_le(ArtifactReader* reader, int32_t* out) {
    if (!out) return false;
    uint32_t tmp = 0;
    if (!read_u32_le(reader, &tmp)) return false;
    *out = (int32_t)tmp;
    return true;
}

static bool read_u64_le(ArtifactReader* reader, uint64_t* out) {
    if (!out) return false;
    uint8_t b[8];
    if (!artifact_reader_read(reader, b, sizeof(b))) return false;
    *out = ((uint64_t)b[0]) |
           ((uint64_t)b[1] << 8) |
           ((uint64_t)b[2] << 16) |
           ((uint64_t)b[3] << 24) |
           ((uint64_t)b[4] << 32) |
           ((uint64_t)b[5] << 40) |
           ((uint64_t)b[6] << 48) |
           ((uint64_t)b[7] << 56);
    return true;
}

static bool read_i64_le(ArtifactReader* reader, int64_t* out) {
    if (!out) return false;
    uint64_t tmp = 0;
    if (!read_u64_le(reader, &tmp)) return false;
    *out = (int64_t)tmp;
    return true;
}

static bool read_double_le(ArtifactReader* reader, double* out) {
    if (!out) return false;
    union {
        uint64_t u;
        double d;
    } x;
    if (!read_u64_le(reader, &x.u)) return false;
    *out = x.d;
    return true;
}

static bool write_string(FILE* f, const char* s) {
    if (!s) s = "";
    size_t len = strlen(s);
    if (len > UINT32_MAX) return false;
    if (!write_u32_le(f, (uint32_t)len)) return false;
    if (len == 0) return true;
    return fwrite(s, 1, len, f) == len;
}

static bool read_string(ArtifactReader* reader, char** out) {
    if (!out) return false;
    *out = NULL;
    uint32_t len = 0;
    if (!read_u32_le(reader, &len)) return false;
    if (len > ARTIFACT_MAX_STRING_BYTES) return false;

    char* s = (char*)safe_malloc((size_t)len + 1);
    if (len > 0) {
        if (!artifact_reader_read(reader, s, len)) {
            free(s);
            return false;
        }
    }
    s[len] = '\0';
    *out = s;
    return true;
}

static bool artifact_write_function(FILE* f, ObjFunction* func) {
    if (!func) return false;

    if (!write_string(f, func->name ? func->name : "")) return false;
    if (!write_string(f, func->source_file ? func->source_file : "")) return false;
    if (!write_u8(f, func->is_async ? 1u : 0u)) return false;
    if (!write_i32_le(f, func->defer_handler_ip)) return false;
    if (!write_i32_le(f, func->defer_return_slot)) return false;

    if (func->param_count < 0 || func->local_count < 0) return false;
    if (!write_u32_le(f, (uint32_t)func->param_count)) return false;
    if (!write_u32_le(f, (uint32_t)func->local_count)) return false;
    if (func->capture_count < 0 || func->capture_count > func->local_count) return false;
    if (!write_u32_le(f, (uint32_t)func->capture_count)) return false;
    for (int i = 0; i < func->capture_count; i++) {
        if (!func->capture_local_slots) return false;
        if (func->capture_local_slots[i] < func->param_count || func->capture_local_slots[i] >= func->local_count) return false;
        if (!write_u32_le(f, (uint32_t)func->capture_local_slots[i])) return false;
    }
    for (int i = 0; i < func->local_count; i++) {
        const char* debug_name = (func->debug_local_names && func->debug_local_names[i])
            ? func->debug_local_names[i]
            : "";
        if (!write_string(f, debug_name)) return false;
    }

    if (func->chunk.code_count < 0) return false;
    if (!write_u32_le(f, (uint32_t)func->chunk.code_count)) return false;
    if (func->chunk.code_count > 0) {
        if (!func->chunk.code) return false;
        if (fwrite(func->chunk.code, 1, (size_t)func->chunk.code_count, f) != (size_t)func->chunk.code_count) {
            return false;
        }
    }

    if (!write_u32_le(f, (uint32_t)func->chunk.code_count)) return false;
    for (int i = 0; i < func->chunk.code_count; i++) {
        int32_t line = 0;
        if (func->chunk.debug_info) {
            line = (int32_t)func->chunk.debug_info[i].line;
        }
        if (!write_i32_le(f, line)) return false;
    }

    if (func->constants.constant_count < 0) return false;
    if (!write_u32_le(f, (uint32_t)func->constants.constant_count)) return false;
    for (int i = 0; i < func->constants.constant_count; i++) {
        Constant c = func->constants.constants[i];
        if (!write_i32_le(f, c.type_index)) return false;
        switch (c.type_index) {
            case 0:
            case 4:
                if (!write_i64_le(f, c.as_int)) return false;
                break;
            case 1:
                if (!write_double_le(f, c.as_double)) return false;
                break;
            case 2:
            case 3:
                if (!write_string(f, c.as_string ? c.as_string : "")) return false;
                break;
            default:
                return false;
        }
    }

    if (!write_i32_le(f, (int32_t)func->jit_hint_plan.kind)) return false;
    if (!write_i32_le(f, (int32_t)func->jit_hint_plan.op)) return false;
    if (!write_u8(f, func->jit_hint_plan.flags)) return false;
    if (!write_u8(f, func->jit_hint_plan.local_slot)) return false;
    if (!write_u8(f, func->jit_hint_plan.local_slot_b)) return false;
    if (!write_i64_le(f, func->jit_hint_plan.int_const0)) return false;
    if (!write_i64_le(f, func->jit_hint_plan.int_const1)) return false;
    if (!write_u8(f, func->jit_profile.flags)) return false;
    if (!write_u8(f, func->jit_profile.support_mask)) return false;
    if (!write_u8(f, func->jit_profile.native_family_mask)) return false;
    if (!write_i32_le(f, func->jit_profile.param_count)) return false;
    if (!write_i32_le(f, func->jit_profile.local_count)) return false;
    if (!write_i32_le(f, func->jit_profile.capture_count)) return false;
    if (!write_i32_le(f, (int32_t)func->jit_profile.summary.kind)) return false;
    if (!write_i32_le(f, (int32_t)func->jit_profile.summary.op)) return false;
    if (!write_u8(f, func->jit_profile.summary.slot0)) return false;
    if (!write_u8(f, func->jit_profile.summary.slot1)) return false;
    if (!write_i64_le(f, func->jit_profile.summary.int_const0)) return false;
    if (!write_i64_le(f, func->jit_profile.summary.int_const1)) return false;

    return true;
}

static ObjFunction* artifact_read_function(ArtifactReader* reader,
                                           uint32_t version,
                                           char* error_buf,
                                           size_t error_buf_size) {
    ObjFunction* func = obj_function_create();
    char* name = NULL;
    if (!read_string(reader, &name)) {
        set_error(error_buf, error_buf_size, "Failed to read function name");
        obj_function_free(func);
        return NULL;
    }
    if (name[0] != '\0') {
        func->name = name;
    } else {
        free(name);
        func->name = NULL;
    }

    if (version >= 3u) {
        char* source_file = NULL;
        if (!read_string(reader, &source_file)) {
            set_error(error_buf, error_buf_size, "Failed to read function source file");
            obj_function_free(func);
            return NULL;
        }
        if (source_file[0] != '\0') {
            func->source_file = source_file;
        } else {
            free(source_file);
            func->source_file = NULL;
        }
    }

    if (version >= 6u) {
        uint8_t is_async = 0;
        if (!read_u8(reader, &is_async)) {
            set_error(error_buf, error_buf_size, "Failed to read async function flag");
            obj_function_free(func);
            return NULL;
        }
        func->is_async = is_async != 0;
    }
    if (version >= 7u) {
        int32_t defer_handler_ip = -1;
        int32_t defer_return_slot = -1;
        if (!read_i32_le(reader, &defer_handler_ip) || !read_i32_le(reader, &defer_return_slot)) {
            set_error(error_buf, error_buf_size, "Failed to read defer metadata");
            obj_function_free(func);
            return NULL;
        }
        func->defer_handler_ip = defer_handler_ip;
        func->defer_return_slot = defer_return_slot;
    }

    uint32_t param_count = 0;
    uint32_t local_count = 0;
    if (!read_u32_le(reader, &param_count) || !read_u32_le(reader, &local_count)) {
        set_error(error_buf, error_buf_size, "Failed to read function signature counts");
        obj_function_free(func);
        return NULL;
    }
    if (param_count > INT_MAX || local_count > INT_MAX) {
        set_error(error_buf, error_buf_size, "Function parameter/local count out of range");
        obj_function_free(func);
        return NULL;
    }
    func->param_count = (int)param_count;
    func->local_count = (int)local_count;
    if (func->local_count > 0) {
        func->debug_local_names = (char**)safe_calloc((size_t)func->local_count, sizeof(char*));
    }
    if (version >= 4u) {
        uint32_t capture_count_u32 = 0;
        if (!read_u32_le(reader, &capture_count_u32)) {
            set_error(error_buf, error_buf_size, "Failed to read closure capture count");
            obj_function_free(func);
            return NULL;
        }
        if (capture_count_u32 > INT_MAX || capture_count_u32 > local_count) {
            set_error(error_buf, error_buf_size, "Closure capture count out of range");
            obj_function_free(func);
            return NULL;
        }

        func->capture_count = (int)capture_count_u32;
        if (func->capture_count > 0) {
            func->capture_local_slots = (int*)safe_malloc((size_t)func->capture_count * sizeof(int));
            for (int i = 0; i < func->capture_count; i++) {
                uint32_t slot_u32 = 0;
                if (!read_u32_le(reader, &slot_u32)) {
                    set_error(error_buf, error_buf_size, "Failed to read closure capture slot");
                    obj_function_free(func);
                    return NULL;
                }
                if (slot_u32 > INT_MAX || slot_u32 < param_count || slot_u32 >= local_count) {
                    set_error(error_buf, error_buf_size, "Closure capture slot out of range");
                    obj_function_free(func);
                    return NULL;
                }
                func->capture_local_slots[i] = (int)slot_u32;
            }
        }
    }
    if (version >= 8u) {
        for (int i = 0; i < func->local_count; i++) {
            char* debug_name = NULL;
            if (!read_string(reader, &debug_name)) {
                set_error(error_buf, error_buf_size, "Failed to read debug local name");
                obj_function_free(func);
                return NULL;
            }
            if (debug_name[0] != '\0') {
                func->debug_local_names[i] = debug_name;
            } else {
                free(debug_name);
            }
        }
    }

    uint32_t code_count_u32 = 0;
    if (!read_u32_le(reader, &code_count_u32)) {
        set_error(error_buf, error_buf_size, "Failed to read function bytecode length");
        obj_function_free(func);
        return NULL;
    }
    if (code_count_u32 > ARTIFACT_MAX_COUNT || code_count_u32 > INT_MAX) {
        set_error(error_buf, error_buf_size, "Function bytecode length is too large");
        obj_function_free(func);
        return NULL;
    }
    int code_count = (int)code_count_u32;
    if (code_count > 0) {
        func->chunk.code = (uint8_t*)safe_malloc((size_t)code_count);
        func->chunk.debug_info = (DebugInfo*)safe_malloc((size_t)code_count * sizeof(DebugInfo));
        if (!artifact_reader_read(reader, func->chunk.code, (size_t)code_count)) {
            set_error(error_buf, error_buf_size, "Failed to read function bytecode");
            obj_function_free(func);
            return NULL;
        }
        func->chunk.code_count = code_count;
        func->chunk.code_capacity = code_count;
    }

    uint32_t debug_count_u32 = 0;
    if (!read_u32_le(reader, &debug_count_u32)) {
        set_error(error_buf, error_buf_size, "Failed to read function debug length");
        obj_function_free(func);
        return NULL;
    }
    if (debug_count_u32 != code_count_u32) {
        set_error(error_buf, error_buf_size, "Function debug metadata does not match bytecode length");
        obj_function_free(func);
        return NULL;
    }
    for (int i = 0; i < code_count; i++) {
        int32_t line = 0;
        if (!read_i32_le(reader, &line)) {
            set_error(error_buf, error_buf_size, "Failed to read debug line table");
            obj_function_free(func);
            return NULL;
        }
        if (func->chunk.debug_info) {
            func->chunk.debug_info[i].index = i;
            func->chunk.debug_info[i].line = line;
        }
    }

    uint32_t const_count_u32 = 0;
    if (!read_u32_le(reader, &const_count_u32)) {
        set_error(error_buf, error_buf_size, "Failed to read constant pool length");
        obj_function_free(func);
        return NULL;
    }
    if (const_count_u32 > ARTIFACT_MAX_COUNT || const_count_u32 > INT_MAX) {
        set_error(error_buf, error_buf_size, "Constant pool length is too large");
        obj_function_free(func);
        return NULL;
    }
    int const_count = (int)const_count_u32;
    if (const_count > 0) {
        func->constants.constants = (Constant*)safe_calloc((size_t)const_count, sizeof(Constant));
        func->constants.constant_count = const_count;
        func->constants.constant_capacity = const_count;

        for (int i = 0; i < const_count; i++) {
            int32_t type_index = -1;
            if (!read_i32_le(reader, &type_index)) {
                set_error(error_buf, error_buf_size, "Failed to read constant type");
                obj_function_free(func);
                return NULL;
            }

            Constant c;
            memset(&c, 0, sizeof(c));
            c.type_index = type_index;
            switch (type_index) {
                case 0:
                case 4: {
                    int64_t v = 0;
                    if (!read_i64_le(reader, &v)) {
                        set_error(error_buf, error_buf_size, "Failed to read integer constant");
                        obj_function_free(func);
                        return NULL;
                    }
                    c.as_int = v;
                    break;
                }
                case 1: {
                    double v = 0.0;
                    if (!read_double_le(reader, &v)) {
                        set_error(error_buf, error_buf_size, "Failed to read double constant");
                        obj_function_free(func);
                        return NULL;
                    }
                    c.as_double = v;
                    break;
                }
                case 2:
                case 3: {
                    char* s = NULL;
                    if (!read_string(reader, &s)) {
                        set_error(error_buf, error_buf_size, "Failed to read string constant");
                        obj_function_free(func);
                        return NULL;
                    }
                    c.as_string = s;
                    break;
                }
                default:
                    set_error(error_buf, error_buf_size, "Unsupported constant type %d in artifact", type_index);
                    obj_function_free(func);
                    return NULL;
            }
            func->constants.constants[i] = c;
        }
    }

    if (version >= 9u) {
        int32_t hint_kind = 0;
        int32_t hint_op = 0;
        uint8_t hint_flags = 0;
        uint8_t hint_local_slot = 0;
        uint8_t hint_local_slot_b = 0;
        int64_t hint_const0 = 0;
        int64_t hint_const1 = 0;
        if (!read_i32_le(reader, &hint_kind) ||
            (version >= 13u && !read_i32_le(reader, &hint_op)) ||
            (version >= 13u && !read_u8(reader, &hint_flags)) ||
            !read_u8(reader, &hint_local_slot) ||
            !read_u8(reader, &hint_local_slot_b) ||
            !read_i64_le(reader, &hint_const0) ||
            !read_i64_le(reader, &hint_const1)) {
            set_error(error_buf, error_buf_size, "Failed to read function JIT hint metadata");
            obj_function_free(func);
            return NULL;
        }
        func->jit_hint_plan.kind = (JitCompiledKind)hint_kind;
        func->jit_hint_plan.op = (JitSummaryOp)hint_op;
        func->jit_hint_plan.flags = hint_flags;
        func->jit_hint_plan.local_slot = hint_local_slot;
        func->jit_hint_plan.local_slot_b = hint_local_slot_b;
        func->jit_hint_plan.int_const0 = hint_const0;
        func->jit_hint_plan.int_const1 = hint_const1;
    }
    if (version >= 10u) {
        uint8_t profile_flags = 0;
        uint8_t profile_support_mask = JIT_PROFILE_SUPPORT_NONE;
        uint8_t profile_native_family_mask = JIT_PROFILE_NATIVE_FAMILY_NONE;
        int32_t profile_param_count = func->param_count;
        int32_t profile_local_count = func->local_count;
        int32_t profile_capture_count = func->capture_count;
        int32_t summary_kind = 0;
        int32_t summary_op = 0;
        uint8_t summary_slot0 = 0;
        uint8_t summary_slot1 = 0;
        int64_t summary_const0 = 0;
        int64_t summary_const1 = 0;
        if (version >= 16u) {
            if (!read_u8(reader, &profile_flags) ||
                !read_u8(reader, &profile_support_mask) ||
                !read_u8(reader, &profile_native_family_mask) ||
                !read_i32_le(reader, &profile_param_count) ||
                !read_i32_le(reader, &profile_local_count) ||
                !read_i32_le(reader, &profile_capture_count)) {
                set_error(error_buf, error_buf_size, "Failed to read function JIT profile metadata");
                obj_function_free(func);
                return NULL;
            }
        } else if (version >= 15u) {
            if (!read_u8(reader, &profile_flags) ||
                !read_u8(reader, &profile_support_mask) ||
                !read_i32_le(reader, &profile_param_count) ||
                !read_i32_le(reader, &profile_local_count) ||
                !read_i32_le(reader, &profile_capture_count)) {
                set_error(error_buf, error_buf_size, "Failed to read function JIT profile metadata");
                obj_function_free(func);
                return NULL;
            }
        } else if (version >= 14u) {
            if (!read_u8(reader, &profile_flags) ||
                !read_i32_le(reader, &profile_param_count) ||
                !read_i32_le(reader, &profile_local_count) ||
                !read_i32_le(reader, &profile_capture_count)) {
                set_error(error_buf, error_buf_size, "Failed to read function JIT profile metadata");
                obj_function_free(func);
                return NULL;
            }
        }
        if (!read_i32_le(reader, &summary_kind) ||
            !read_i32_le(reader, &summary_op) ||
            !read_u8(reader, &summary_slot0) ||
            !read_u8(reader, &summary_slot1) ||
            !read_i64_le(reader, &summary_const0) ||
            !read_i64_le(reader, &summary_const1)) {
            set_error(error_buf, error_buf_size, "Failed to read function JIT summary metadata");
            obj_function_free(func);
            return NULL;
        }
        func->jit_profile.flags = version >= 14u
                                      ? profile_flags
                                      : (uint8_t)((func->is_async ? JIT_PROFILE_FLAG_ASYNC : 0) |
                                                  (func->capture_count != 0 ? JIT_PROFILE_FLAG_HAS_CAPTURES : 0));
        func->jit_profile.support_mask = version >= 15u
                                             ? profile_support_mask
                                             : (uint8_t)((!func->is_async ? JIT_PROFILE_SUPPORT_STUB : 0) |
                                                         ((version >= 10u &&
                                                           summary_kind != (int32_t)JIT_SUMMARY_KIND_NONE &&
                                                           func->jit_hint_plan.kind != JIT_COMPILED_KIND_NONE &&
                                                           func->capture_count == 0 &&
                                                           !func->is_async)
                                                              ? JIT_PROFILE_SUPPORT_NATIVE_SUMMARY
                                                              : 0));
        if (version >= 16u) {
            func->jit_profile.native_family_mask = profile_native_family_mask;
        } else {
            switch ((JitSummaryKind)summary_kind) {
                case JIT_SUMMARY_KIND_INT_LOCAL_CONST_BINARY:
                case JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_BINARY:
                case JIT_SUMMARY_KIND_INT_TWOARG_BINARY:
                case JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_BINARY:
                    func->jit_profile.native_family_mask = JIT_PROFILE_NATIVE_FAMILY_ARITHMETIC;
                    break;
                case JIT_SUMMARY_KIND_BOOL_TWOARG_COMPARE:
                case JIT_SUMMARY_KIND_BOOL_LOCAL_CONST_COMPARE:
                case JIT_SUMMARY_KIND_BOOL_GUARDED_TWOARG_COMPARE:
                    func->jit_profile.native_family_mask = JIT_PROFILE_NATIVE_FAMILY_COMPARE;
                    break;
                case JIT_SUMMARY_KIND_INT_TWOARG_SELECTOR:
                case JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_SELECTOR:
                case JIT_SUMMARY_KIND_INT_LOCAL_CONST_SELECTOR:
                case JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_SELECTOR:
                    func->jit_profile.native_family_mask = JIT_PROFILE_NATIVE_FAMILY_SELECTOR;
                    break;
                case JIT_SUMMARY_KIND_NONE:
                default:
                    func->jit_profile.native_family_mask = JIT_PROFILE_NATIVE_FAMILY_NONE;
                    break;
            }
        }
        func->jit_profile.param_count = profile_param_count;
        func->jit_profile.local_count = profile_local_count;
        func->jit_profile.capture_count = profile_capture_count;
        func->jit_profile.summary.kind = (JitSummaryKind)summary_kind;
        func->jit_profile.summary.op = (JitSummaryOp)summary_op;
        func->jit_profile.summary.slot0 = summary_slot0;
        func->jit_profile.summary.slot1 = summary_slot1;
        func->jit_profile.summary.int_const0 = summary_const0;
        func->jit_profile.summary.int_const1 = summary_const1;
    }

    return func;
}

bool artifact_file_is_bytecode(const char* path) {
    if (!path || path[0] == '\0') return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    char magic[ARTIFACT_MAGIC_SIZE] = {0};
    bool ok = fread(magic, 1, ARTIFACT_MAGIC_SIZE, f) == ARTIFACT_MAGIC_SIZE &&
              memcmp(magic, ARTIFACT_MAGIC, ARTIFACT_MAGIC_SIZE) == 0;
    fclose(f);
    return ok;
}

bool artifact_write_file(const char* path,
                         ObjFunction* init_function,
                         ObjFunction** functions,
                         int function_count,
                         int main_index,
                         uint32_t typecheck_flags,
                         const ArtifactDependencyInfo* dependencies,
                         int dependency_count,
                         const InterfaceDispatchEntry* interface_dispatch_entries,
                         int interface_dispatch_count,
                         char* error_buf,
                         size_t error_buf_size) {
    if (error_buf && error_buf_size > 0) error_buf[0] = '\0';

    if (!path || path[0] == '\0') {
        set_error(error_buf, error_buf_size, "Output artifact path is empty");
        return false;
    }
    if (!functions || function_count <= 0) {
        set_error(error_buf, error_buf_size, "Artifact requires at least one function");
        return false;
    }
    if (main_index < 0 || main_index >= function_count) {
        set_error(error_buf, error_buf_size, "Main function index is out of range");
        return false;
    }
    if (dependency_count < 0) {
        set_error(error_buf, error_buf_size, "Dependency count is invalid");
        return false;
    }
    if (interface_dispatch_count < 0) {
        set_error(error_buf, error_buf_size, "Interface dispatch mapping count is invalid");
        return false;
    }
    if (interface_dispatch_count > 0 && !interface_dispatch_entries) {
        set_error(error_buf, error_buf_size, "Interface dispatch mapping payload is missing");
        return false;
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        char err_tmp[128];
        set_error(error_buf, error_buf_size, "Failed to open output artifact '%s': %s", path, artifact_strerror(errno, err_tmp, sizeof(err_tmp)));
        return false;
    }

    bool ok = true;
    ok = ok && fwrite(ARTIFACT_MAGIC, 1, ARTIFACT_MAGIC_SIZE, f) == ARTIFACT_MAGIC_SIZE;
    ok = ok && write_u32_le(f, ARTIFACT_VERSION);
    ok = ok && write_u32_le(f, typecheck_flags);
    ok = ok && write_u32_le(f, (uint32_t)dependency_count);

    for (int i = 0; ok && i < dependency_count; i++) {
        const char* dep_path = dependencies[i].path ? dependencies[i].path : "";
        ok = ok && write_string(f, dep_path);
        ok = ok && write_i64_le(f, dependencies[i].mtime);
    }

    ok = ok && write_u32_le(f, (uint32_t)interface_dispatch_count);
    for (int i = 0; ok && i < interface_dispatch_count; i++) {
        const InterfaceDispatchEntry* entry = &interface_dispatch_entries[i];
        ok = ok && write_string(f, entry->interface_name ? entry->interface_name : "");
        ok = ok && write_string(f, entry->record_name ? entry->record_name : "");
        ok = ok && write_string(f, entry->method_name ? entry->method_name : "");
        ok = ok && write_string(f, entry->function_name ? entry->function_name : "");
    }

    ok = ok && write_u32_le(f, (uint32_t)function_count);
    ok = ok && write_i32_le(f, (int32_t)main_index);
    ok = ok && write_u8(f, init_function ? 1u : 0u);

    for (int i = 0; ok && i < function_count; i++) {
        ok = ok && artifact_write_function(f, functions[i]);
    }

    if (ok && init_function) {
        ok = artifact_write_function(f, init_function);
    }

    if (!ok) {
        set_error(error_buf, error_buf_size, "Failed while writing artifact '%s'", path);
    }

    if (fclose(f) != 0) {
        if (ok) {
            set_error(error_buf, error_buf_size, "Failed to finalize artifact '%s'", path);
        }
        ok = false;
    }

    if (!ok) {
        remove(path);
    }

    return ok;
}

static bool artifact_load_bytes_internal(const uint8_t* data,
                                         size_t size,
                                         LoadedBytecodeArtifact* out,
                                         char* error_buf,
                                         size_t error_buf_size) {
    if (error_buf && error_buf_size > 0) error_buf[0] = '\0';
    if (!out) {
        set_error(error_buf, error_buf_size, "Artifact output pointer is null");
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->main_index = -1;

    if (!data || size == 0) {
        set_error(error_buf, error_buf_size, "Artifact data is empty");
        return false;
    }

    ArtifactReader reader;
    reader.data = data;
    reader.size = size;
    reader.offset = 0;

    LoadedBytecodeArtifact* tmp = out;
    memset(tmp, 0, sizeof(*tmp));
    tmp->main_index = -1;
    bool ok = true;

    char magic[ARTIFACT_MAGIC_SIZE] = {0};
    ok = ok && artifact_reader_read(&reader, magic, ARTIFACT_MAGIC_SIZE);
    if (ok && memcmp(magic, ARTIFACT_MAGIC, ARTIFACT_MAGIC_SIZE) != 0) {
        set_error(error_buf, error_buf_size, "File is not a TabloLang bytecode artifact");
        ok = false;
    }

    uint32_t version = 0;
    if (ok && !read_u32_le(&reader, &version)) ok = false;
    if (ok && (version < ARTIFACT_MIN_SUPPORTED_VERSION || version > ARTIFACT_VERSION)) {
        set_error(error_buf, error_buf_size, "Unsupported artifact version %u", version);
        ok = false;
    }

    if (ok && !read_u32_le(&reader, &tmp->typecheck_flags)) ok = false;

    uint32_t dep_count_u32 = 0;
    if (ok && !read_u32_le(&reader, &dep_count_u32)) ok = false;
    if (ok && (dep_count_u32 > ARTIFACT_MAX_COUNT || dep_count_u32 > INT_MAX)) ok = false;
    if (ok) tmp->dependency_count = (int)dep_count_u32;

    if (ok && tmp->dependency_count > 0) {
        tmp->dependencies = (LoadedArtifactDependency*)safe_calloc((size_t)tmp->dependency_count,
                                                                  sizeof(LoadedArtifactDependency));
        for (int i = 0; ok && i < tmp->dependency_count; i++) {
            if (!read_string(&reader, &tmp->dependencies[i].path)) {
                ok = false;
                break;
            }
            if (!read_i64_le(&reader, &tmp->dependencies[i].mtime)) {
                ok = false;
                break;
            }
        }
    }

    if (ok && version >= 5u) {
        uint32_t interface_dispatch_count_u32 = 0;
        if (!read_u32_le(&reader, &interface_dispatch_count_u32)) ok = false;
        if (ok &&
            (interface_dispatch_count_u32 > ARTIFACT_MAX_COUNT ||
             interface_dispatch_count_u32 > INT_MAX)) {
            ok = false;
        }
        if (ok) {
            tmp->interface_dispatch_count = (int)interface_dispatch_count_u32;
        }

        if (ok && tmp->interface_dispatch_count > 0) {
            tmp->interface_dispatch_entries = (InterfaceDispatchEntry*)safe_calloc(
                (size_t)tmp->interface_dispatch_count,
                sizeof(InterfaceDispatchEntry));
            for (int i = 0; ok && i < tmp->interface_dispatch_count; i++) {
                InterfaceDispatchEntry* entry = &tmp->interface_dispatch_entries[i];
                if (!read_string(&reader, &entry->interface_name) ||
                    !read_string(&reader, &entry->record_name) ||
                    !read_string(&reader, &entry->method_name) ||
                    !read_string(&reader, &entry->function_name)) {
                    ok = false;
                    break;
                }
            }
        }
    }

    uint32_t function_count_u32 = 0;
    int32_t main_index_i32 = -1;
    uint8_t has_init = 0;
    if (ok && !read_u32_le(&reader, &function_count_u32)) ok = false;
    if (ok && (function_count_u32 == 0 || function_count_u32 > ARTIFACT_MAX_COUNT || function_count_u32 > INT_MAX)) {
        ok = false;
    }
    if (ok && !read_i32_le(&reader, &main_index_i32)) ok = false;
    if (ok && !read_u8(&reader, &has_init)) ok = false;

    if (ok) {
        tmp->function_count = (int)function_count_u32;
        tmp->main_index = main_index_i32;
        if (tmp->main_index < 0 || tmp->main_index >= tmp->function_count) {
            set_error(error_buf, error_buf_size, "Artifact main function index is invalid");
            ok = false;
        }
    }

    if (ok) {
        tmp->functions = (ObjFunction**)safe_calloc((size_t)tmp->function_count, sizeof(ObjFunction*));
        for (int i = 0; ok && i < tmp->function_count; i++) {
            tmp->functions[i] = artifact_read_function(&reader, version, error_buf, error_buf_size);
            if (!tmp->functions[i]) ok = false;
        }
    }

    if (ok && has_init) {
        tmp->init_function = artifact_read_function(&reader, version, error_buf, error_buf_size);
        if (!tmp->init_function) ok = false;
    }

    if (ok && reader.offset != reader.size) {
        set_error(error_buf, error_buf_size, "Artifact contains trailing data");
        ok = false;
    }

    if (!ok && (error_buf && error_buf[0] == '\0')) {
        set_error(error_buf, error_buf_size, "Failed to read artifact bytes (possibly truncated or corrupted)");
    }

    if (!ok) {
        artifact_loaded_free(tmp);
        return false;
    }
    return true;
}

bool artifact_load_file(const char* path,
                        LoadedBytecodeArtifact* out,
                        char* error_buf,
                        size_t error_buf_size) {
    FILE* f = NULL;
    uint8_t* data = NULL;
    bool ok = false;
    long file_size = 0;
    LoadedBytecodeArtifact tmp;
    SafeAllocJmpContext alloc_ctx;
    jmp_buf alloc_env;
    char alloc_message[256] = {0};

    if (error_buf && error_buf_size > 0) error_buf[0] = '\0';
    if (!out) {
        set_error(error_buf, error_buf_size, "Artifact output pointer is null");
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->main_index = -1;

    if (!path || path[0] == '\0') {
        set_error(error_buf, error_buf_size, "Artifact path is empty");
        return false;
    }

    f = fopen(path, "rb");
    if (!f) {
        char err_tmp[128];
        set_error(error_buf, error_buf_size, "Failed to open artifact '%s': %s", path, artifact_strerror(errno, err_tmp, sizeof(err_tmp)));
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        set_error(error_buf, error_buf_size, "Failed to size artifact '%s'", path);
        fclose(f);
        return false;
    }
    file_size = ftell(f);
    if (file_size <= 0) {
        set_error(error_buf, error_buf_size, "Artifact '%s' is empty or unreadable", path);
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        set_error(error_buf, error_buf_size, "Failed to rewind artifact '%s'", path);
        fclose(f);
        return false;
    }

    memset(&tmp, 0, sizeof(tmp));
    tmp.main_index = -1;
    safe_alloc_push_jmp_context(&alloc_ctx, &alloc_env, alloc_message, sizeof(alloc_message));
    if (setjmp(alloc_env) != 0) {
        safe_alloc_pop_jmp_context(&alloc_ctx);
        set_error(error_buf,
                  error_buf_size,
                  "%s",
                  alloc_message[0] ? alloc_message : "Out of memory while loading artifact");
        if (f) fclose(f);
        artifact_loaded_free(&tmp);
        free(data);
        return false;
    }

    data = (uint8_t*)safe_malloc((size_t)file_size);
    if (fread(data, 1, (size_t)file_size, f) != (size_t)file_size) {
        set_error(error_buf, error_buf_size, "Failed to read artifact '%s'", path);
        safe_alloc_pop_jmp_context(&alloc_ctx);
        fclose(f);
        free(data);
        return false;
    }
    ok = artifact_load_bytes_internal(data, (size_t)file_size, &tmp, error_buf, error_buf_size);
    safe_alloc_pop_jmp_context(&alloc_ctx);

    if (fclose(f) != 0 && ok) {
        set_error(error_buf, error_buf_size, "Failed to close artifact '%s' after reading", path);
        ok = false;
        artifact_loaded_free(&tmp);
    }

    free(data);
    if (ok) {
        *out = tmp;
    } else {
        artifact_loaded_free(&tmp);
    }
    return ok;
}

bool artifact_load_bytes(const uint8_t* data,
                         size_t size,
                         LoadedBytecodeArtifact* out,
                         char* error_buf,
                         size_t error_buf_size) {
    LoadedBytecodeArtifact tmp;
    SafeAllocJmpContext alloc_ctx;
    jmp_buf alloc_env;
    char alloc_message[256] = {0};

    if (error_buf && error_buf_size > 0) error_buf[0] = '\0';
    if (!out) {
        set_error(error_buf, error_buf_size, "Artifact output pointer is null");
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->main_index = -1;

    memset(&tmp, 0, sizeof(tmp));
    tmp.main_index = -1;

    safe_alloc_push_jmp_context(&alloc_ctx, &alloc_env, alloc_message, sizeof(alloc_message));
    if (setjmp(alloc_env) != 0) {
        safe_alloc_pop_jmp_context(&alloc_ctx);
        set_error(error_buf,
                  error_buf_size,
                  "%s",
                  alloc_message[0] ? alloc_message : "Out of memory while loading artifact bytes");
        artifact_loaded_free(&tmp);
        return false;
    }

    bool ok = artifact_load_bytes_internal(data, size, &tmp, error_buf, error_buf_size);
    safe_alloc_pop_jmp_context(&alloc_ctx);
    if (ok) {
        *out = tmp;
    } else {
        artifact_loaded_free(&tmp);
    }
    return ok;
}

void artifact_loaded_free(LoadedBytecodeArtifact* artifact) {
    if (!artifact) return;

    if (artifact->init_function) {
        obj_function_free(artifact->init_function);
        artifact->init_function = NULL;
    }

    if (artifact->functions) {
        for (int i = 0; i < artifact->function_count; i++) {
            if (artifact->functions[i]) {
                obj_function_free(artifact->functions[i]);
            }
        }
        free(artifact->functions);
        artifact->functions = NULL;
    }
    artifact->function_count = 0;
    artifact->main_index = -1;

    if (artifact->dependencies) {
        for (int i = 0; i < artifact->dependency_count; i++) {
            if (artifact->dependencies[i].path) {
                free(artifact->dependencies[i].path);
            }
        }
        free(artifact->dependencies);
        artifact->dependencies = NULL;
    }
    artifact->dependency_count = 0;

    if (artifact->interface_dispatch_entries) {
        interface_dispatch_entries_free(artifact->interface_dispatch_entries,
                                        artifact->interface_dispatch_count);
        artifact->interface_dispatch_entries = NULL;
    }
    artifact->interface_dispatch_count = 0;

    artifact->typecheck_flags = 0;
}
