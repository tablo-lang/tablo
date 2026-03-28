#ifndef VM_H
#define VM_H

#include "bytecode.h"
#include "cycle_gc.h"
#include "sfc64.h"
#include "types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <threads.h>

typedef enum {
    VAL_INT,
    VAL_BOOL,
    VAL_DOUBLE,
    VAL_BIGINT,
    VAL_STRING,
    VAL_BYTES,
    VAL_ARRAY,
    VAL_NIL,
    VAL_FUNCTION,
    VAL_NATIVE,
    VAL_RECORD,
    VAL_TUPLE,
    VAL_MAP,
    VAL_SET,
    VAL_SOCKET,
    VAL_FILE
} ValueType;

typedef struct VM VM;
typedef struct NativeExtensionRegistry NativeExtensionRegistry;
typedef struct VmPostedEventQueue VmPostedEventQueue;
typedef bool (*VmPostedEventDispatchFn)(VM* vm, void* payload);
typedef void (*VmPostedEventFreeFn)(void* payload);
typedef bool (*VmPollWaitCallback)(VM* vm, void* payload);
typedef void (*VmPollWaitFreeFn)(void* payload);

typedef struct ObjString {
     int length;
     uint32_t hash; // cached (mixed) hash; UINT32_MAX means unset
     int ref_count;
     char* chars;
} ObjString;

typedef struct ObjBytesBuffer {
    uint8_t* data;
    int capacity;
    int ref_count;
} ObjBytesBuffer;

typedef struct ObjBytes {
    ObjBytesBuffer* buffer;
    int offset;
    int length;
    int ref_count;
} ObjBytes;

typedef struct ObjBigInt {
    int ref_count;
    int sign;          // -1, 0, 1
    uint32_t* limbs;   // base 2^32, little-endian
    size_t count;      // number of limbs
} ObjBigInt;

typedef enum {
    ARRAY_KIND_BOXED = 0,
    ARRAY_KIND_INT = 1,
    ARRAY_KIND_DOUBLE = 2,
    ARRAY_KIND_BOOL = 3,
    ARRAY_KIND_BYTE = 4
} ArrayKind;

typedef struct ObjArray {
      int count;
      int capacity;
      int ref_count;
      CycleGcNode gc_node;
      ArrayKind kind;
      union {
          struct Value* elements; // ARRAY_KIND_BOXED
          int64_t* ints;          // ARRAY_KIND_INT
          double* doubles;        // ARRAY_KIND_DOUBLE
          uint8_t* bools;         // ARRAY_KIND_BOOL (0/1)
          uint8_t* bytes;         // ARRAY_KIND_BYTE (0..255)
      } data;
} ObjArray;

struct Value;

typedef enum {
    JIT_FUNC_STATE_COLD = 0,
    JIT_FUNC_STATE_QUEUED = 1,
    JIT_FUNC_STATE_COMPILING = 2,
    JIT_FUNC_STATE_COMPILED_STUB = 3,
    JIT_FUNC_STATE_COMPILED_NATIVE = 4,
    JIT_FUNC_STATE_FAILED = 5
} JitFunctionState;

typedef enum {
    JIT_REASON_NONE = 0,
    JIT_REASON_QUEUED_HOT = 1,
    JIT_REASON_NATIVE_HINT = 2,
    JIT_REASON_NATIVE_EXACT = 3,
    JIT_REASON_STUB_FALLBACK = 4,
    JIT_REASON_UNSUPPORTED_ASYNC = 5,
    JIT_REASON_UNSUPPORTED_SHAPE = 6
} JitFunctionReason;

typedef enum {
    JIT_SUMMARY_KIND_NONE = 0,
    JIT_SUMMARY_KIND_INT_LOCAL_CONST_BINARY = 1,
    JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_BINARY = 2,
    JIT_SUMMARY_KIND_INT_TWOARG_BINARY = 3,
    JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_BINARY = 4,
    JIT_SUMMARY_KIND_BOOL_TWOARG_COMPARE = 5,
    JIT_SUMMARY_KIND_BOOL_GUARDED_TWOARG_COMPARE = 6,
    JIT_SUMMARY_KIND_INT_TWOARG_SELECTOR = 7,
    JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_SELECTOR = 8,
    JIT_SUMMARY_KIND_BOOL_LOCAL_CONST_COMPARE = 9,
    JIT_SUMMARY_KIND_INT_LOCAL_CONST_SELECTOR = 10,
    JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_SELECTOR = 11
} JitSummaryKind;

typedef enum {
    JIT_PROFILE_FLAG_NONE = 0,
    JIT_PROFILE_FLAG_ASYNC = 1 << 0,
    JIT_PROFILE_FLAG_HAS_CAPTURES = 1 << 1
} JitFunctionProfileFlags;

typedef enum {
    JIT_PROFILE_SUPPORT_NONE = 0,
    JIT_PROFILE_SUPPORT_STUB = 1 << 0,
    JIT_PROFILE_SUPPORT_NATIVE_SUMMARY = 1 << 1
} JitFunctionProfileSupport;

typedef enum {
    JIT_PROFILE_NATIVE_FAMILY_NONE = 0,
    JIT_PROFILE_NATIVE_FAMILY_ARITHMETIC = 1 << 0,
    JIT_PROFILE_NATIVE_FAMILY_COMPARE = 1 << 1,
    JIT_PROFILE_NATIVE_FAMILY_SELECTOR = 1 << 2
} JitFunctionProfileNativeFamily;

typedef enum {
    JIT_SUMMARY_OP_NONE = 0,
    JIT_SUMMARY_OP_ADD = 1,
    JIT_SUMMARY_OP_SUB = 2,
    JIT_SUMMARY_OP_MUL = 3,
    JIT_SUMMARY_OP_DIV = 4,
    JIT_SUMMARY_OP_MOD = 5,
    JIT_SUMMARY_OP_BIT_AND = 6,
    JIT_SUMMARY_OP_BIT_OR = 7,
    JIT_SUMMARY_OP_BIT_XOR = 8,
    JIT_SUMMARY_OP_LT = 9,
    JIT_SUMMARY_OP_LE = 10,
    JIT_SUMMARY_OP_EQ = 11,
    JIT_SUMMARY_OP_NE = 12,
    JIT_SUMMARY_OP_GT = 13,
    JIT_SUMMARY_OP_GE = 14
} JitSummaryOp;

typedef enum {
    JIT_COMPILED_KIND_NONE = 0,
    JIT_COMPILED_KIND_STUB = 1,
    JIT_COMPILED_KIND_INT_ADD_LOCAL_CONST = 2,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_ADD_CONST = 3,
    JIT_COMPILED_KIND_INT_MUL_LOCAL_CONST = 4,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MUL_CONST = 5,
    JIT_COMPILED_KIND_INT_SUB_LOCAL_CONST = 6,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SUB_CONST = 7,
    JIT_COMPILED_KIND_INT_DIV_LOCAL_CONST = 8,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_DIV_CONST = 9,
    JIT_COMPILED_KIND_INT_MOD_LOCAL_CONST = 10,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MOD_CONST = 11,
    JIT_COMPILED_KIND_INT_BIT_AND_LOCAL_CONST = 12,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_AND_CONST = 13,
    JIT_COMPILED_KIND_INT_BIT_OR_LOCAL_CONST = 14,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_OR_CONST = 15,
    JIT_COMPILED_KIND_INT_BIT_XOR_LOCAL_CONST = 16,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_XOR_CONST = 17,
    JIT_COMPILED_KIND_INT_ADD_LOCALS = 18,
    JIT_COMPILED_KIND_INT_SUB_LOCALS = 19,
    JIT_COMPILED_KIND_INT_MUL_LOCALS = 20,
    JIT_COMPILED_KIND_INT_BIT_AND_LOCALS = 21,
    JIT_COMPILED_KIND_INT_BIT_OR_LOCALS = 22,
    JIT_COMPILED_KIND_INT_BIT_XOR_LOCALS = 23,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_ADD_LOCALS = 24,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SUB_LOCALS = 25,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MUL_LOCALS = 26,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_AND_LOCALS = 27,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_OR_LOCALS = 28,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_XOR_LOCALS = 29,
    JIT_COMPILED_KIND_BOOL_LT_LOCALS = 30,
    JIT_COMPILED_KIND_BOOL_LE_LOCALS = 31,
    JIT_COMPILED_KIND_BOOL_EQ_LOCALS = 32,
    JIT_COMPILED_KIND_BOOL_NE_LOCALS = 33,
    JIT_COMPILED_KIND_BOOL_GT_LOCALS = 34,
    JIT_COMPILED_KIND_BOOL_GE_LOCALS = 35,
    JIT_COMPILED_KIND_INT_SELECT_LT_LOCALS = 36,
    JIT_COMPILED_KIND_INT_SELECT_LE_LOCALS = 37,
    JIT_COMPILED_KIND_INT_SELECT_GT_LOCALS = 38,
    JIT_COMPILED_KIND_INT_SELECT_GE_LOCALS = 39,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LT_LOCALS = 40,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LE_LOCALS = 41,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GT_LOCALS = 42,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GE_LOCALS = 43,
    JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_LT_LOCALS = 44,
    JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_LE_LOCALS = 45,
    JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_EQ_LOCALS = 46,
    JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_NE_LOCALS = 47,
    JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_GT_LOCALS = 48,
    JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_GE_LOCALS = 49,
    JIT_COMPILED_KIND_BOOL_LT_LOCAL_CONST = 50,
    JIT_COMPILED_KIND_BOOL_LE_LOCAL_CONST = 51,
    JIT_COMPILED_KIND_BOOL_EQ_LOCAL_CONST = 52,
    JIT_COMPILED_KIND_BOOL_NE_LOCAL_CONST = 53,
    JIT_COMPILED_KIND_BOOL_GT_LOCAL_CONST = 54,
    JIT_COMPILED_KIND_BOOL_GE_LOCAL_CONST = 55,
    JIT_COMPILED_KIND_INT_SELECT_LT_LOCAL_CONST_RET_LOCAL = 56,
    JIT_COMPILED_KIND_INT_SELECT_LT_LOCAL_CONST_RET_CONST = 57,
    JIT_COMPILED_KIND_INT_SELECT_LE_LOCAL_CONST_RET_LOCAL = 58,
    JIT_COMPILED_KIND_INT_SELECT_LE_LOCAL_CONST_RET_CONST = 59,
    JIT_COMPILED_KIND_INT_SELECT_GT_LOCAL_CONST_RET_LOCAL = 60,
    JIT_COMPILED_KIND_INT_SELECT_GT_LOCAL_CONST_RET_CONST = 61,
    JIT_COMPILED_KIND_INT_SELECT_GE_LOCAL_CONST_RET_LOCAL = 62,
    JIT_COMPILED_KIND_INT_SELECT_GE_LOCAL_CONST_RET_CONST = 63,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LT_LOCAL_CONST_RET_LOCAL = 64,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LT_LOCAL_CONST_RET_CONST = 65,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LE_LOCAL_CONST_RET_LOCAL = 66,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LE_LOCAL_CONST_RET_CONST = 67,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GT_LOCAL_CONST_RET_LOCAL = 68,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GT_LOCAL_CONST_RET_CONST = 69,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GE_LOCAL_CONST_RET_LOCAL = 70,
    JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GE_LOCAL_CONST_RET_CONST = 71,
    JIT_COMPILED_KIND_BOOL_COMPARE_LOCALS_GENERIC = 72,
    JIT_COMPILED_KIND_BOOL_COMPARE_GUARDED_LOCALS_GENERIC = 73,
    JIT_COMPILED_KIND_BOOL_COMPARE_LOCAL_CONST_GENERIC = 74,
    JIT_COMPILED_KIND_INT_SELECTOR_LOCALS_GENERIC = 75,
    JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCALS_GENERIC = 76,
    JIT_COMPILED_KIND_INT_SELECTOR_LOCAL_CONST_GENERIC = 77,
    JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCAL_CONST_GENERIC = 78,
    JIT_COMPILED_KIND_INT_BINARY_LOCAL_CONST_GENERIC = 79,
    JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCAL_CONST_GENERIC = 80,
    JIT_COMPILED_KIND_INT_BINARY_LOCALS_GENERIC = 81,
    JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCALS_GENERIC = 82
} JitCompiledKind;

typedef enum {
    JIT_PLAN_FLAG_NONE = 0,
    JIT_PLAN_FLAG_RETURN_LOCAL_ON_TRUE = 1
} JitCompiledFlags;

typedef struct {
    JitSummaryKind kind;
    JitSummaryOp op;
    uint8_t slot0;
    uint8_t slot1;
    int64_t int_const0;
    int64_t int_const1;
} JitFunctionSummary;

typedef struct {
    uint8_t flags;
    uint8_t support_mask;
    uint8_t native_family_mask;
    uint8_t reserved1;
    int32_t param_count;
    int32_t local_count;
    int32_t capture_count;
    JitFunctionSummary summary;
} JitFunctionProfile;

typedef struct {
    JitCompiledKind kind;
    JitSummaryOp op;
    uint8_t flags;
    uint8_t local_slot;
    uint8_t local_slot_b;
    int64_t int_const0;
    int64_t int_const1;
} JitCompiledPlan;

typedef struct ObjFunction {
    Chunk chunk;
    ConstantPool constants;
    int param_count;
    char** param_names;
    int local_count;
    char** local_names;
    char** debug_local_names;
    int* local_types;
    int capture_count;
    int* capture_local_slots;
    struct Value* captured_values;
    bool is_async;
    int defer_handler_ip;
    int defer_return_slot;
    char* name;
    char* source_file;
    uint64_t jit_entry_count;
    bool jit_hot;
    JitFunctionState jit_state;
    JitFunctionReason jit_reason;
    uint32_t jit_compile_attempts;
    uint64_t jit_compiled_call_count;
    void* jit_compiled_entry;
    JitFunctionProfile jit_profile;
    JitCompiledPlan jit_hint_plan;
    JitCompiledPlan jit_compiled_plan;
    int ref_count;
    int* global_slot_cache; // maps constant index -> vm global slot index
    int global_slot_cache_count;
} ObjFunction;

typedef struct ObjNative {
    int (*invoke)(VM* vm, struct ObjNative* native);
    void (*builtin_function)(void* vm);
    void* userdata;
    int arity;
    int ref_count;
} ObjNative;

typedef struct ObjRecord {
    RecordDef* def;       // Pointer to type definition (contains field info)
    char* type_name;      // Runtime record type tag (used for interface dispatch)
    struct Value* fields; // Array of field values
    int field_count;
    bool is_native_opaque;
    void* native_opaque_payload;
    void (*native_opaque_destroy)(void* payload);
    int ref_count;
    CycleGcNode gc_node;
} ObjRecord;

typedef struct ObjTuple {
      struct Value* elements; // Array of element values
      int element_count;
      int ref_count;
      CycleGcNode gc_node;
} ObjTuple;

// Forward declarations
typedef struct ObjMap ObjMap;
typedef struct ObjSet ObjSet;
struct Value;

typedef struct ObjSocket {
    int socket_fd;        // Native socket descriptor
    int ref_count;
    bool is_connected;    // Connection state
    void* owner_vm;
    bool limit_tracked;
    void* transport_ctx;
    int (*transport_send)(struct ObjSocket* sock, const char* data, int len);
    int (*transport_recv)(struct ObjSocket* sock, char* out, int max_len);
    void (*transport_close)(struct ObjSocket* sock);
} ObjSocket;

typedef struct ObjFile {
    FILE* handle;
    int ref_count;
    bool is_closed;
    void* owner_vm;
    bool limit_tracked;
} ObjFile;

// Phase 2 of Value-representation abstraction:
// - default: legacy tagged struct representation
// - optional: experimental NaN-boxed representation via TABLO_NAN_BOXING
// New code should prefer these helpers over direct field access.
#ifdef TABLO_NAN_BOXING

typedef struct Value {
    uint64_t bits;
} Value;

_Static_assert(VAL_FILE <= 15, "NaN-boxed Value currently supports up to 16 ValueType tags");

#define VALUE_NANBOX_TAGGED_MASK UINT64_C(0xFFF8000000000000)
#define VALUE_NANBOX_CANONICAL_NAN UINT64_C(0x7FF8000000000000)
#define VALUE_NANBOX_EXP_MASK UINT64_C(0x7FF0000000000000)
#define VALUE_NANBOX_MANTISSA_MASK UINT64_C(0x000FFFFFFFFFFFFF)
#define VALUE_NANBOX_TAG_SHIFT 47
#define VALUE_NANBOX_TAG_MASK UINT64_C(0x0007800000000000)
#define VALUE_NANBOX_PAYLOAD_MASK UINT64_C(0x00007FFFFFFFFFFF)

static inline uint64_t value_nanbox_double_to_bits(double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static inline double value_nanbox_bits_to_double(uint64_t bits) {
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline bool value_nanbox_is_tagged_bits(uint64_t bits) {
    return (bits & VALUE_NANBOX_TAGGED_MASK) == VALUE_NANBOX_TAGGED_MASK;
}

static inline uint64_t value_nanbox_pack_tagged(ValueType type, uint64_t payload) {
    return VALUE_NANBOX_TAGGED_MASK |
           ((((uint64_t)type) & UINT64_C(0xF)) << VALUE_NANBOX_TAG_SHIFT) |
           (payload & VALUE_NANBOX_PAYLOAD_MASK);
}

static inline bool value_nanbox_int_fits(int64_t value) {
    const int64_t max_inline = (((int64_t)1) << 46) - 1;
    const int64_t min_inline = -(((int64_t)1) << 46);
    return value >= min_inline && value <= max_inline;
}

static inline bool value_nanbox_pointer_fits(const void* ptr) {
    uintptr_t raw = (uintptr_t)ptr;
    return (raw & (uintptr_t)(~VALUE_NANBOX_PAYLOAD_MASK)) == 0;
}

static inline uintptr_t value_nanbox_pointer_payload(const Value* value) {
    return (uintptr_t)(value->bits & VALUE_NANBOX_PAYLOAD_MASK);
}

static inline ValueType value_get_type(const Value* value) {
    if (!value_nanbox_is_tagged_bits(value->bits)) {
        return VAL_DOUBLE;
    }
    return (ValueType)((value->bits & VALUE_NANBOX_TAG_MASK) >> VALUE_NANBOX_TAG_SHIFT);
}

static inline void value_set_type(Value* value, ValueType type) {
    if (type == VAL_DOUBLE) {
        value->bits = value_nanbox_double_to_bits(0.0);
        return;
    }
    value->bits = value_nanbox_pack_tagged(type, 0);
}

static inline int64_t value_get_int(const Value* value) {
    uint64_t payload = value->bits & VALUE_NANBOX_PAYLOAD_MASK;
    return ((int64_t)(payload << 17)) >> 17;
}

static inline void value_set_int(Value* value, int64_t v) {
    assert(value_nanbox_int_fits(v) && "NaN boxing currently stores inline ints in 47-bit signed range");
    value->bits = value_nanbox_pack_tagged(VAL_INT, (uint64_t)v);
}

static inline bool value_get_bool(const Value* value) {
    return (value->bits & UINT64_C(1)) != 0;
}

static inline void value_set_bool(Value* value, bool v) {
    value->bits = value_nanbox_pack_tagged(VAL_BOOL, v ? UINT64_C(1) : UINT64_C(0));
}

static inline double value_get_double(const Value* value) {
    if (value_nanbox_is_tagged_bits(value->bits)) {
        return value_nanbox_bits_to_double(VALUE_NANBOX_CANONICAL_NAN);
    }
    return value_nanbox_bits_to_double(value->bits);
}

static inline void value_set_double(Value* value, double v) {
    uint64_t bits = value_nanbox_double_to_bits(v);
    bool is_nan = ((bits & VALUE_NANBOX_EXP_MASK) == VALUE_NANBOX_EXP_MASK) &&
                  ((bits & VALUE_NANBOX_MANTISSA_MASK) != 0);
    if (is_nan || value_nanbox_is_tagged_bits(bits)) {
        value->bits = VALUE_NANBOX_CANONICAL_NAN;
        return;
    }
    value->bits = bits;
}

static inline ObjString* value_get_string_obj(const Value* value) {
    return (ObjString*)(uintptr_t)value_nanbox_pointer_payload(value);
}
static inline void value_set_string_obj(Value* value, ObjString* v) {
    assert(value_nanbox_pointer_fits(v) && "Pointer exceeds NaN-box payload range");
    value->bits = value_nanbox_pack_tagged(VAL_STRING, (uint64_t)(uintptr_t)v);
}

static inline ObjBytes* value_get_bytes_obj(const Value* value) {
    return (ObjBytes*)(uintptr_t)value_nanbox_pointer_payload(value);
}
static inline void value_set_bytes_obj(Value* value, ObjBytes* v) {
    assert(value_nanbox_pointer_fits(v) && "Pointer exceeds NaN-box payload range");
    value->bits = value_nanbox_pack_tagged(VAL_BYTES, (uint64_t)(uintptr_t)v);
}

static inline ObjBigInt* value_get_bigint_obj(const Value* value) {
    return (ObjBigInt*)(uintptr_t)value_nanbox_pointer_payload(value);
}
static inline void value_set_bigint_obj(Value* value, ObjBigInt* v) {
    assert(value_nanbox_pointer_fits(v) && "Pointer exceeds NaN-box payload range");
    value->bits = value_nanbox_pack_tagged(VAL_BIGINT, (uint64_t)(uintptr_t)v);
}

static inline ObjArray* value_get_array_obj(const Value* value) {
    return (ObjArray*)(uintptr_t)value_nanbox_pointer_payload(value);
}
static inline void value_set_array_obj(Value* value, ObjArray* v) {
    assert(value_nanbox_pointer_fits(v) && "Pointer exceeds NaN-box payload range");
    value->bits = value_nanbox_pack_tagged(VAL_ARRAY, (uint64_t)(uintptr_t)v);
}

static inline ObjFunction* value_get_function_obj(const Value* value) {
    return (ObjFunction*)(uintptr_t)value_nanbox_pointer_payload(value);
}
static inline void value_set_function_obj(Value* value, ObjFunction* v) {
    assert(value_nanbox_pointer_fits(v) && "Pointer exceeds NaN-box payload range");
    value->bits = value_nanbox_pack_tagged(VAL_FUNCTION, (uint64_t)(uintptr_t)v);
}

static inline ObjNative* value_get_native_obj(const Value* value) {
    return (ObjNative*)(uintptr_t)value_nanbox_pointer_payload(value);
}
static inline void value_set_native_obj(Value* value, ObjNative* v) {
    assert(value_nanbox_pointer_fits(v) && "Pointer exceeds NaN-box payload range");
    value->bits = value_nanbox_pack_tagged(VAL_NATIVE, (uint64_t)(uintptr_t)v);
}

static inline ObjRecord* value_get_record_obj(const Value* value) {
    return (ObjRecord*)(uintptr_t)value_nanbox_pointer_payload(value);
}
static inline void value_set_record_obj(Value* value, ObjRecord* v) {
    assert(value_nanbox_pointer_fits(v) && "Pointer exceeds NaN-box payload range");
    value->bits = value_nanbox_pack_tagged(VAL_RECORD, (uint64_t)(uintptr_t)v);
}

static inline ObjTuple* value_get_tuple_obj(const Value* value) {
    return (ObjTuple*)(uintptr_t)value_nanbox_pointer_payload(value);
}
static inline void value_set_tuple_obj(Value* value, ObjTuple* v) {
    assert(value_nanbox_pointer_fits(v) && "Pointer exceeds NaN-box payload range");
    value->bits = value_nanbox_pack_tagged(VAL_TUPLE, (uint64_t)(uintptr_t)v);
}

static inline ObjMap* value_get_map_obj(const Value* value) {
    return (ObjMap*)(uintptr_t)value_nanbox_pointer_payload(value);
}
static inline void value_set_map_obj(Value* value, ObjMap* v) {
    assert(value_nanbox_pointer_fits(v) && "Pointer exceeds NaN-box payload range");
    value->bits = value_nanbox_pack_tagged(VAL_MAP, (uint64_t)(uintptr_t)v);
}

static inline ObjSet* value_get_set_obj(const Value* value) {
    return (ObjSet*)(uintptr_t)value_nanbox_pointer_payload(value);
}
static inline void value_set_set_obj(Value* value, ObjSet* v) {
    assert(value_nanbox_pointer_fits(v) && "Pointer exceeds NaN-box payload range");
    value->bits = value_nanbox_pack_tagged(VAL_SET, (uint64_t)(uintptr_t)v);
}

static inline ObjSocket* value_get_socket_obj(const Value* value) {
    return (ObjSocket*)(uintptr_t)value_nanbox_pointer_payload(value);
}
static inline void value_set_socket_obj(Value* value, ObjSocket* v) {
    assert(value_nanbox_pointer_fits(v) && "Pointer exceeds NaN-box payload range");
    value->bits = value_nanbox_pack_tagged(VAL_SOCKET, (uint64_t)(uintptr_t)v);
}

static inline ObjFile* value_get_file_obj(const Value* value) {
    return (ObjFile*)(uintptr_t)value_nanbox_pointer_payload(value);
}
static inline void value_set_file_obj(Value* value, ObjFile* v) {
    assert(value_nanbox_pointer_fits(v) && "Pointer exceeds NaN-box payload range");
    value->bits = value_nanbox_pack_tagged(VAL_FILE, (uint64_t)(uintptr_t)v);
}

#else

typedef struct Value {
    ValueType type;
    union {
        int64_t as_int;
        double as_double;
        ObjString* as_string;
        ObjBytes* as_bytes;
        ObjBigInt* as_bigint;
        ObjArray* as_array;
        ObjFunction* as_function;
        ObjNative* as_native;
        ObjRecord* as_record;
        ObjTuple* as_tuple;
        ObjMap* as_map;
        ObjSet* as_set;
        ObjSocket* as_socket;
        ObjFile* as_file;
    };
} Value;

static inline ValueType value_get_type(const Value* value) { return value->type; }
static inline void value_set_type(Value* value, ValueType type) { value->type = type; }

static inline int64_t value_get_int(const Value* value) { return value->as_int; }
static inline void value_set_int(Value* value, int64_t v) { value->as_int = v; }

static inline bool value_get_bool(const Value* value) { return value->as_int != 0; }
static inline void value_set_bool(Value* value, bool v) { value->as_int = v ? 1 : 0; }

static inline double value_get_double(const Value* value) { return value->as_double; }
static inline void value_set_double(Value* value, double v) { value->as_double = v; }

static inline ObjString* value_get_string_obj(const Value* value) { return value->as_string; }
static inline void value_set_string_obj(Value* value, ObjString* v) { value->as_string = v; }

static inline ObjBytes* value_get_bytes_obj(const Value* value) { return value->as_bytes; }
static inline void value_set_bytes_obj(Value* value, ObjBytes* v) { value->as_bytes = v; }

static inline ObjBigInt* value_get_bigint_obj(const Value* value) { return value->as_bigint; }
static inline void value_set_bigint_obj(Value* value, ObjBigInt* v) { value->as_bigint = v; }

static inline ObjArray* value_get_array_obj(const Value* value) { return value->as_array; }
static inline void value_set_array_obj(Value* value, ObjArray* v) { value->as_array = v; }

static inline ObjFunction* value_get_function_obj(const Value* value) { return value->as_function; }
static inline void value_set_function_obj(Value* value, ObjFunction* v) { value->as_function = v; }

static inline ObjNative* value_get_native_obj(const Value* value) { return value->as_native; }
static inline void value_set_native_obj(Value* value, ObjNative* v) { value->as_native = v; }

static inline ObjRecord* value_get_record_obj(const Value* value) { return value->as_record; }
static inline void value_set_record_obj(Value* value, ObjRecord* v) { value->as_record = v; }

static inline ObjTuple* value_get_tuple_obj(const Value* value) { return value->as_tuple; }
static inline void value_set_tuple_obj(Value* value, ObjTuple* v) { value->as_tuple = v; }

static inline ObjMap* value_get_map_obj(const Value* value) { return value->as_map; }
static inline void value_set_map_obj(Value* value, ObjMap* v) { value->as_map = v; }

static inline ObjSet* value_get_set_obj(const Value* value) { return value->as_set; }
static inline void value_set_set_obj(Value* value, ObjSet* v) { value->as_set = v; }

static inline ObjSocket* value_get_socket_obj(const Value* value) { return value->as_socket; }
static inline void value_set_socket_obj(Value* value, ObjSocket* v) { value->as_socket = v; }

static inline ObjFile* value_get_file_obj(const Value* value) { return value->as_file; }
static inline void value_set_file_obj(Value* value, ObjFile* v) { value->as_file = v; }

#endif

typedef struct MapSlot {
    // 0 = empty, >=2 = occupied (hash|2).
    uint32_t hash;
    Value key;
    Value value;
} MapSlot;

typedef struct ObjMap {
    MapSlot* slots;
    int capacity;
    int count;
    int used;
    int ref_count;
    CycleGcNode gc_node;
} ObjMap;

typedef struct SetSlot {
    // 0 = empty, >=2 = occupied (hash|2).
    uint32_t hash;
    Value value;
} SetSlot;

typedef struct ObjSet {
    SetSlot* slots;
    int capacity;
    int count;
    int used;
    int ref_count;
} ObjSet;

typedef struct DeferredCall {
    Value callee;
    Value* args;
    int arg_count;
} DeferredCall;

typedef struct AsyncTask AsyncTask;
typedef struct FutureWaitEntry FutureWaitEntry;
typedef struct TimerWaitEntry TimerWaitEntry;
typedef struct VmPollWaitEntry VmPollWaitEntry;

typedef struct CallFrame {
    ObjFunction* function;
    int ip;
    int slots_start;
    int slots_count;
    DeferredCall* defers;
    int defer_count;
    int defer_capacity;
    bool is_async_root;
    ObjRecord* async_result_future;
    bool panic_unwinding;
    char* panic_message;
} CallFrame;

typedef enum {
    VM_DEBUG_STOP_NONE = 0,
    VM_DEBUG_STOP_BREAKPOINT = 1,
    VM_DEBUG_STOP_STEP = 2,
    VM_DEBUG_STOP_EXCEPTION = 3,
    VM_DEBUG_STOP_ENTRY = 4,
    VM_DEBUG_STOP_PAUSE = 5
} VmDebugStopKind;

typedef enum {
    VM_DEBUG_EXEC_NONE = 0,
    VM_DEBUG_EXEC_STEP_IN = 1,
    VM_DEBUG_EXEC_STEP_OVER = 2,
    VM_DEBUG_EXEC_STEP_OUT = 3
} VmDebugExecutionMode;

typedef struct {
    VmDebugStopKind kind;
    const char* source_file;
    const char* function_name;
    int line;
    int call_depth;
    int ip;
} VmDebugStopInfo;

typedef struct {
    const char* source_file;
    int line;
} VmLineBreakpoint;

typedef struct {
    const char* function_name;
    const char* source_file;
    int line;
} VmDebugFrameInfo;

typedef void (*VmOutputCallback)(void* user_data, const char* text, int length);

typedef struct {
    int handler_ip;
    int stack_depth;
} ExceptionHandler;

typedef struct HashEntry {
    uint32_t hash;
    int global_slot; // Used by vm->globals_hash; -1 when not mapped to a VM global slot.
    char* key;
    Value value;
} HashEntry;

typedef struct {
    Value* values;
    int count;
    int capacity;
    ConstantPool constants;
} Stack;

typedef struct {
    HashEntry** entries;
    int capacity;
    int count;
} HashTable;

typedef struct {
    char* interface_name;
    char* record_name;
    char* method_name;
    char* function_name;
} InterfaceDispatchEntry;

typedef struct {
    int max_call_depth;
    int max_stack_size;
    int max_array_size;
    int max_string_length;
    int64_t max_instructions;
    int max_open_files;
    int max_open_sockets;
} VMConfig;

struct VM {
    CallFrame* frames;
    int frame_count;
    int frame_capacity;
    Stack stack;
    Value* globals;
    int global_count;
    int global_capacity;
    char** global_names;
    HashTable globals_hash;
    HashEntry** globals_hash_entries; // slot -> globals_hash entry (for fast updates)
    NativeExtensionRegistry* extension_registry;
    InterfaceDispatchEntry* interface_dispatch_entries;
    int interface_dispatch_count;
    int interface_dispatch_capacity;
    int* interface_dispatch_slot_cache;
    HashTable string_pool;
    Value* return_value;
    bool error_occurred;
    char* error_message;
    VMConfig config;
    char* sandbox_root;
    bool file_io_enabled;
    bool network_enabled;
    bool process_enabled;
    bool sqlite_enabled;
    bool threading_enabled;
    int64_t instruction_count;
    int current_call_depth;
    int current_open_files;
    int current_open_sockets;
    AsyncTask* ready_tasks_head;
    AsyncTask* ready_tasks_tail;
    FutureWaitEntry* future_waiters;
    TimerWaitEntry* timer_waiters;
    VmPollWaitEntry* poll_waiters;
    VmPostedEventQueue* posted_event_queue;
    bool posted_event_auto_drain;

    // Exception handling
    ExceptionHandler* exception_handlers;
    int exception_handler_count;
    int exception_handler_capacity;
    Value exception_value;
    bool in_exception;

    SFC64Context rng;
    bool rng_seeded;
    uint64_t rng_counter;

    CycleGc cycle_gc;

    bool jit_profile_enabled;
    bool jit_auto_compile_enabled;
    uint64_t jit_hot_threshold;
    ObjFunction** jit_work_queue;
    int jit_work_queue_count;
    int jit_work_queue_capacity;
    bool profile_opcodes;
    uint64_t opcode_counts[256];

    VmLineBreakpoint* debug_breakpoints;
    int debug_breakpoint_count;
    int debug_breakpoint_capacity;
    VmDebugExecutionMode debug_exec_mode;
    const char* debug_step_source_file;
    int debug_step_line;
    int debug_step_depth;
    const char* debug_skip_source_file;
    int debug_skip_line;
    int debug_skip_depth;
    VmDebugStopInfo debug_stop;
    bool debug_stop_on_entry_pending;
    volatile long debug_pause_requested;
    bool debug_break_on_runtime_error;
    VmOutputCallback output_callback;
    void* output_callback_user_data;
};

void value_init_nil(Value* val);
void value_init_int(Value* val, int64_t value);
void value_init_bool(Value* val, bool value);
void value_init_double(Value* val, double value);
void value_init_bigint(Value* val, ObjBigInt* bigint);
void value_init_string(Value* val, const char* str);
void value_init_string_n(Value* val, const char* str, int length);
void value_init_bytes(Value* val, ObjBytes* bytes);
void value_init_array(Value* val, ObjArray* arr);
void value_init_function(Value* val, ObjFunction* func);
void value_init_native(Value* val, ObjNative* native);
void value_init_record(Value* val, ObjRecord* record);
bool value_is_nil(Value* val);
bool value_is_true(Value* val);
bool value_equals(Value* a, Value* b);
void value_print(VM* vm, Value* val);
void value_retain(Value* val);
void value_free(Value* val);
ExceptionHandler vm_pop_exception_handler(VM* vm);

ObjString* obj_string_create(const char* chars, int length);
void obj_string_retain(ObjString* str);
void obj_string_release(ObjString* str);
void obj_string_free(ObjString* str);

ObjBytesBuffer* obj_bytes_buffer_create(int capacity);
void obj_bytes_buffer_retain(ObjBytesBuffer* buf);
void obj_bytes_buffer_release(ObjBytesBuffer* buf);
void obj_bytes_buffer_free(ObjBytesBuffer* buf);

ObjBytes* obj_bytes_create(ObjBytesBuffer* buffer, int offset, int length);
ObjBytes* obj_bytes_create_copy(const uint8_t* data, int length);
ObjBytes* obj_bytes_create_with_size(int length, uint8_t fill);
ObjBytes* obj_bytes_slice(ObjBytes* bytes, int start, int end);
uint8_t* obj_bytes_data(ObjBytes* bytes);
void obj_bytes_retain(ObjBytes* bytes);
void obj_bytes_release(ObjBytes* bytes);
void obj_bytes_free(ObjBytes* bytes);

ObjBigInt* obj_bigint_from_string(const char* str);
bool obj_bigint_try_from_string(const char* str, ObjBigInt** out);
ObjBigInt* obj_bigint_from_int64(int64_t value);
void obj_bigint_retain(ObjBigInt* bigint);
void obj_bigint_release(ObjBigInt* bigint);
void obj_bigint_free(ObjBigInt* bigint);
int obj_bigint_compare(const ObjBigInt* a, const ObjBigInt* b);
int obj_bigint_compare_abs(const ObjBigInt* a, const ObjBigInt* b);
size_t obj_bigint_decimal_digits(const ObjBigInt* bigint);
bool obj_bigint_is_even(const ObjBigInt* bigint);
ObjBigInt* obj_bigint_add(const ObjBigInt* a, const ObjBigInt* b);
ObjBigInt* obj_bigint_sub(const ObjBigInt* a, const ObjBigInt* b);
ObjBigInt* obj_bigint_mul(const ObjBigInt* a, const ObjBigInt* b);
bool obj_bigint_mul_small_inplace(ObjBigInt* a, uint32_t m);
ObjBigInt* obj_bigint_div(const ObjBigInt* a, const ObjBigInt* b, bool* div_by_zero);
ObjBigInt* obj_bigint_mod(const ObjBigInt* a, const ObjBigInt* b, bool* div_by_zero);
ObjBigInt* obj_bigint_negate(const ObjBigInt* a);
ObjBigInt* obj_bigint_bit_and(const ObjBigInt* a, const ObjBigInt* b);
ObjBigInt* obj_bigint_bit_or(const ObjBigInt* a, const ObjBigInt* b);
ObjBigInt* obj_bigint_bit_xor(const ObjBigInt* a, const ObjBigInt* b);
ObjBigInt* obj_bigint_bit_not(const ObjBigInt* a);
char* obj_bigint_to_string(const ObjBigInt* bigint);
char* obj_bigint_to_hex_string(const ObjBigInt* bigint);
bool obj_bigint_to_int64(const ObjBigInt* bigint, int64_t* out);
double obj_bigint_to_double(const ObjBigInt* bigint);

ObjArray* obj_array_create(VM* vm, int capacity);
ObjArray* obj_array_create_typed(VM* vm, int capacity, ArrayKind kind);
void obj_array_retain(ObjArray* arr);
void obj_array_release(ObjArray* arr);
void obj_array_free(ObjArray* arr);
void obj_array_convert_to_boxed(ObjArray* arr);
void obj_array_push(ObjArray* arr, Value val);
void obj_array_pop(ObjArray* arr, Value* out);
void obj_array_set(ObjArray* arr, int index, Value val);
void obj_array_get(ObjArray* arr, int index, Value* out);

ObjFunction* obj_function_create(void);
void obj_function_retain(ObjFunction* func);
void obj_function_release(ObjFunction* func);
void obj_function_free(ObjFunction* func);

ObjNative* obj_native_create_builtin(void (*function)(void* vm), int arity);
ObjNative* obj_native_create_extension(void* userdata, int arity);
void obj_native_retain(ObjNative* native);
void obj_native_release(ObjNative* native);
void obj_native_free(ObjNative* native);

// Record operations
ObjRecord* obj_record_create(VM* vm, RecordDef* def);
ObjRecord* obj_record_create_with_count(VM* vm, int field_count);
ObjRecord* obj_record_create_opaque(VM* vm,
                                    const char* type_name,
                                    void* payload,
                                    void (*destroy)(void* payload));
void obj_record_retain(ObjRecord* record);
void obj_record_release(ObjRecord* record);
void obj_record_free(ObjRecord* record);
void obj_record_set_field(ObjRecord* record, int field_idx, Value val);
void obj_record_get_field(ObjRecord* record, int field_idx, Value* out);
int obj_record_get_field_index(ObjRecord* record, const char* field_name);

#define VM_FUTURE_RUNTIME_TYPE_NAME "Future"

ObjRecord* obj_future_create_pending(VM* vm);
ObjRecord* obj_future_create_resolved(VM* vm, Value value);
bool value_is_future(const Value* val);
bool obj_future_is_ready(ObjRecord* future);
bool obj_future_is_panicked(ObjRecord* future);
bool obj_future_resolve(ObjRecord* future, Value value);
bool obj_future_resolve_panic(VM* vm, ObjRecord* future, const char* message);
bool obj_future_try_get(ObjRecord* future, Value* out);
const char* obj_future_get_panic_message(ObjRecord* future);
bool vm_future_complete(VM* vm, ObjRecord* future, Value value);
bool vm_future_complete_panic(VM* vm, ObjRecord* future, const char* message);
ObjRecord* vm_future_sleep(VM* vm, int64_t delay_ms);
bool vm_enqueue_poll_waiter(VM* vm, VmPollWaitCallback callback, VmPollWaitFreeFn free_fn, void* payload);
int vm_debug_ready_task_count(VM* vm);
int vm_debug_future_waiter_entry_count(VM* vm);
int vm_debug_future_waiter_task_count(VM* vm);
int vm_debug_timer_waiter_count(VM* vm);
int vm_debug_poll_waiter_count(VM* vm);
bool vm_debug_add_line_breakpoint(VM* vm, const char* source_file, int line);
void vm_debug_set_line_breakpoint(VM* vm, const char* source_file, int line);
void vm_debug_clear_line_breakpoints(VM* vm);
void vm_debug_set_break_on_runtime_error(VM* vm, bool enabled);
void vm_debug_request_stop_on_entry(VM* vm);
void vm_debug_request_pause(VM* vm);
void vm_debug_prepare_continue(VM* vm);
void vm_debug_prepare_step_in(VM* vm);
void vm_debug_prepare_step_over(VM* vm);
void vm_debug_prepare_step_out(VM* vm);
const VmDebugStopInfo* vm_debug_get_stop_info(VM* vm);
void vm_debug_clear_stop_info(VM* vm);
int vm_debug_frame_count(VM* vm);
bool vm_debug_get_frame_info(VM* vm, int index_from_top, VmDebugFrameInfo* out);
int vm_debug_frame_argument_count(VM* vm, int index_from_top);
bool vm_debug_get_frame_argument(VM* vm,
                                 int index_from_top,
                                 int argument_index,
                                 const char** out_name,
                                 const Value** out_value);
bool vm_debug_set_frame_argument(VM* vm,
                                 int index_from_top,
                                 int argument_index,
                                 const Value* value);
int vm_debug_frame_local_count(VM* vm, int index_from_top);
bool vm_debug_get_frame_local(VM* vm,
                              int index_from_top,
                              int local_index,
                              const char** out_name,
                              const Value** out_value);
bool vm_debug_set_frame_local(VM* vm,
                              int index_from_top,
                              int local_index,
                              const Value* value);
int vm_debug_frame_non_argument_local_count(VM* vm, int index_from_top);
bool vm_debug_get_frame_non_argument_local(VM* vm,
                                           int index_from_top,
                                           int local_index,
                                           const char** out_name,
                                           const Value** out_value);
bool vm_debug_set_frame_non_argument_local(VM* vm,
                                           int index_from_top,
                                           int local_index,
                                           const Value* value);
int vm_debug_global_count(VM* vm);
bool vm_debug_get_global(VM* vm, int global_index, const char** out_name, const Value** out_value);
bool vm_debug_set_global(VM* vm, int global_index, const Value* value);
char* vm_debug_format_value(VM* vm, const Value* value);
const char* vm_debug_value_type_name(const Value* value);
void vm_set_output_callback(VM* vm, VmOutputCallback callback, void* user_data);
VmPostedEventQueue* vm_get_posted_event_queue(VM* vm);
void vm_posted_event_queue_retain(VmPostedEventQueue* queue);
void vm_posted_event_queue_release(VmPostedEventQueue* queue);
bool vm_posted_event_queue_is_open(VmPostedEventQueue* queue);
bool vm_close_posted_event_queue(VM* vm);
bool vm_is_posted_event_queue_open(VM* vm);
bool vm_posted_event_queue_enqueue(VmPostedEventQueue* queue,
                                   VmPostedEventDispatchFn dispatch_fn,
                                   VmPostedEventFreeFn free_fn,
                                   void* payload);
bool vm_has_posted_event_queue_work(VM* vm);
int vm_posted_event_queue_pending_count(VM* vm);
int vm_drain_posted_event_queue(VM* vm, int max_events);
int vm_drain_posted_event_queue_for_ms(VM* vm, int max_events, int64_t max_millis);
bool vm_wait_for_posted_event_queue(VM* vm, int64_t timeout_millis);
int vm_wait_and_drain_posted_event_queue(VM* vm, int max_events, int64_t timeout_millis);
bool vm_get_posted_event_auto_drain(VM* vm);
bool vm_set_posted_event_auto_drain(VM* vm, bool enabled);

// Tuple operations
ObjTuple* obj_tuple_create(VM* vm, int element_count);
void obj_tuple_retain(ObjTuple* tuple);
void obj_tuple_release(ObjTuple* tuple);
void obj_tuple_free(ObjTuple* tuple);
void obj_tuple_set(ObjTuple* tuple, int index, Value val);
void obj_tuple_get(ObjTuple* tuple, int index, Value* out);

// Value operations
void value_init_tuple(Value* val, ObjTuple* tuple);
void value_init_map(Value* val, ObjMap* map);
void value_init_set(Value* val, ObjSet* set);

// Map operations
ObjMap* obj_map_create(VM* vm);
void obj_map_retain(ObjMap* map);
void obj_map_release(ObjMap* map);
void obj_map_free(ObjMap* map);
void obj_map_set(ObjMap* map, Value key, Value value);
Value obj_map_get(ObjMap* map, Value key);
bool obj_map_has(ObjMap* map, Value key);
void obj_map_set_cstr_n(ObjMap* map, const char* key_chars, int key_length, Value value);
Value obj_map_get_cstr_n(ObjMap* map, const char* key_chars, int key_length);
bool obj_map_has_cstr_n(ObjMap* map, const char* key_chars, int key_length);
void obj_map_delete_cstr_n(ObjMap* map, const char* key_chars, int key_length);
void obj_map_set_cstr(ObjMap* map, const char* key_chars, Value value);
Value obj_map_get_cstr(ObjMap* map, const char* key_chars);
bool obj_map_has_cstr(ObjMap* map, const char* key_chars);
void obj_map_delete_cstr(ObjMap* map, const char* key_chars);
void obj_map_set_string(ObjMap* map, ObjString* key, Value value);
Value obj_map_get_string(ObjMap* map, ObjString* key);
bool obj_map_has_string(ObjMap* map, ObjString* key);
bool obj_map_try_get_cstr(ObjMap* map, const char* key, Value* out);
void obj_map_delete_string(ObjMap* map, ObjString* key);
void obj_map_delete(ObjMap* map, Value key);
int obj_map_count(ObjMap* map);
void obj_map_keys(ObjMap* map, ObjArray* result);
void obj_map_values(ObjMap* map, ObjArray* result);

// Set operations
ObjSet* obj_set_create(void);
void obj_set_retain(ObjSet* set);
void obj_set_release(ObjSet* set);
void obj_set_free(ObjSet* set);
void obj_set_add(ObjSet* set, Value value);
void obj_set_add_string(ObjSet* set, ObjString* value);
void obj_set_remove(ObjSet* set, Value value);
void obj_set_remove_string(ObjSet* set, ObjString* value);
bool obj_set_has(ObjSet* set, Value value);
bool obj_set_has_string(ObjSet* set, ObjString* value);
int obj_set_count(ObjSet* set);
void obj_set_to_array(ObjSet* set, ObjArray* result);

// Socket operations
ObjSocket* obj_socket_create(VM* vm, int socket_fd, bool limit_tracked);
void obj_socket_retain(ObjSocket* sock);
void obj_socket_release(ObjSocket* sock);
void obj_socket_free(ObjSocket* sock);
void value_init_socket(Value* val, ObjSocket* sock);

// File operations
ObjFile* obj_file_create(VM* vm, FILE* handle, bool limit_tracked);
void obj_file_retain(ObjFile* file);
void obj_file_release(ObjFile* file);
void obj_file_free(ObjFile* file);
bool obj_file_close(ObjFile* file);
void value_init_file(Value* val, ObjFile* file);

void vm_init(VM* vm);
void vm_free(VM* vm);
int vm_execute(VM* vm, ObjFunction* func);
int vm_call_value_sync(VM* vm,
                       const Value* callee,
                       const Value* args,
                       int arg_count,
                       Value* out_result);
int vm_jit_stub_compiled_entry(VM* vm,
                               ObjFunction* function,
                               int args_base,
                               int arg_count,
                               CallFrame* io_frame,
                               Chunk** io_chunk,
                               uint8_t** io_code);
int vm_jit_native_compiled_entry(VM* vm,
                                 ObjFunction* function,
                                 int args_base,
                                 int arg_count,
                                 CallFrame* io_frame,
                                 Chunk** io_chunk,
                                 uint8_t** io_code);
int vm_resume(VM* vm);
bool vm_take_return_value(VM* vm, Value* out);
void vm_set_global(VM* vm, const char* name, Value val);
Value vm_get_global(VM* vm, const char* name);
void vm_register_native(VM* vm, const char* name, void (*function)(VM* vm), int arity);
bool vm_register_native_extension(VM* vm, const char* name, void* userdata, int arity);
bool vm_register_interface_impl(VM* vm,
                                const char* interface_name,
                                const char* record_name,
                                const char* method_name,
                                const char* function_name);
void interface_dispatch_entries_free(InterfaceDispatchEntry* entries, int count);
bool vm_has_error(VM* vm);
void vm_clear_error(VM* vm);
void vm_runtime_error(VM* vm, const char* message);
const char* vm_get_error(VM* vm);

void hash_table_init(HashTable* table);
void hash_table_free(HashTable* table);
void hash_table_set(HashTable* table, const char* key, Value value);
Value hash_table_get(HashTable* table, const char* key);

ObjString* vm_intern_string(VM* vm, const char* chars, int length);

int vm_gc_collect(VM* vm);
int vm_gc_tracked_count(VM* vm);

VMConfig vm_default_config(void);
void vm_set_config(VM* vm, VMConfig config);

void vm_set_sandbox_root(VM* vm, const char* root);
const char* vm_get_sandbox_root(VM* vm);
void vm_set_file_io_enabled(VM* vm, bool enabled);
bool vm_is_file_io_enabled(VM* vm);
void vm_set_network_enabled(VM* vm, bool enabled);
bool vm_is_network_enabled(VM* vm);
void vm_set_process_enabled(VM* vm, bool enabled);
bool vm_is_process_enabled(VM* vm);
void vm_set_sqlite_enabled(VM* vm, bool enabled);
bool vm_is_sqlite_enabled(VM* vm);
void vm_set_threading_enabled(VM* vm, bool enabled);
bool vm_is_threading_enabled(VM* vm);
void vm_set_max_open_files(VM* vm, int max_open_files);
void vm_set_max_open_sockets(VM* vm, int max_open_sockets);
int vm_get_max_open_files(VM* vm);
int vm_get_max_open_sockets(VM* vm);
int vm_get_current_open_files(VM* vm);
int vm_get_current_open_sockets(VM* vm);
bool vm_try_acquire_file_handle(VM* vm);
void vm_release_file_handle(VM* vm);
bool vm_try_acquire_socket_handle(VM* vm);
void vm_release_socket_handle(VM* vm);

// Exception handling
void vm_push_exception_handler(VM* vm, int handler_ip);
ExceptionHandler vm_pop_exception_handler(VM* vm);
void vm_throw_exception(VM* vm, Value value);
bool vm_has_exception(VM* vm);

#endif
