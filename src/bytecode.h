#ifndef BYTECODE_H
#define BYTECODE_H

#include <stdint.h>

typedef enum {
    OP_NOP,
    OP_CONST,
    OP_CONST16,                        // const index encoded as u16 (big-endian)
    OP_LOAD_LOCAL,
    OP_STORE_LOCAL,
    OP_ADD_LOCAL_CONST,
    OP_SUB_LOCAL_CONST,
    OP_ADD2_LOCAL_CONST,                  // local[a]+=c1; local[b]+=c2 (u8 a, u8 c1, u8 b, u8 c2, pad)
    OP_NEGATE_LOCAL,
    OP_LOAD_GLOBAL,
    OP_LOAD_GLOBAL16,                  // global name constant index encoded as u16 (big-endian)
    OP_STORE_GLOBAL,
    OP_STORE_GLOBAL16,                 // global name constant index encoded as u16 (big-endian)
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_NEG,
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_NOT,
    OP_AND,
    OP_OR,
    OP_BIT_NOT,
    OP_BIT_AND,
    OP_BIT_OR,
    OP_BIT_XOR,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_CALL,
    OP_CALL_GLOBAL,
    OP_CALL_GLOBAL16,                  // global name constant index encoded as u16 (big-endian), then u8 argc
    OP_DEFER,                         // store deferred call descriptor (u8 argc)
    OP_DEFER_HAS,                     // push bool (true if current frame has pending defers)
    OP_DEFER_CALL,                    // call last deferred call (leaves result on stack)
    OP_AWAIT,                         // await future on stack, yielding resolved value or suspending async task
    OP_RET,
    OP_ARRAY_NEW,
    OP_ARRAY_GET,
    OP_ARRAY_SET,
    OP_ARRAY_LEN,
    OP_ARRAY_PUSH,
    OP_ARRAY_POP,
    OP_STRING_LEN,
    OP_STRING_CONCAT,
    OP_TYPEOF,
    OP_CAST_INT,
    OP_CAST_BOOL,
    OP_CAST_DOUBLE,
    OP_CAST_STRING,
    OP_CAST_BIGINT,
    OP_PRINT,
    OP_PRINTLN,
    OP_POP,
    OP_DUP,
    OP_TRY,   // Reserved (exceptions not supported)
    OP_THROW, // Reserved (exceptions not supported)
    OP_CATCH, // Reserved (exceptions not supported)
    OP_RECORD_NEW,
    OP_RECORD_SET_FIELD,
    OP_RECORD_GET_FIELD,
    OP_TUPLE_NEW,
    OP_TUPLE_GET,
    OP_TUPLE_SET,
    OP_MAP_NEW,
    OP_MAP_SET,
    OP_MAP_GET,
    OP_MAP_HAS,
    OP_MAP_DELETE,
    OP_MAP_KEYS,
    OP_MAP_VALUES,
    OP_SET_NEW,
    OP_SET_ADD,
    OP_SET_HAS,
    OP_SET_REMOVE,
    OP_SET_TO_ARRAY,

    // Superinstructions / fast paths.
    OP_ADD_LOCAL_DIV_LOCALS,   // local[dst] += local[num] / local[den]
    OP_JUMP_IF_LOCAL_LT,       // if local[a] <  local[b] jump (i16 offset)
    OP_JUMP_IF_LOCAL_LE,       // if local[a] <= local[b] jump (i16 offset)
    OP_JUMP_IF_LOCAL_GT,       // if local[a] >  local[b] jump (i16 offset)
    OP_JUMP_IF_LOCAL_GE,       // if local[a] >= local[b] jump (i16 offset)
    OP_JUMP_IF_LOCAL_LT_CONST, // if local[a] <  const[c] jump (i16 offset)
    OP_JUMP_IF_LOCAL_LE_CONST, // if local[a] <= const[c] jump (i16 offset)
    OP_JUMP_IF_LOCAL_GT_CONST, // if local[a] >  const[c] jump (i16 offset)
    OP_JUMP_IF_LOCAL_GE_CONST, // if local[a] >= const[c] jump (i16 offset)
    OP_JUMP_IF_FALSE_POP,      // if top-of-stack is false jump; else pop it (i16 offset + padding)
    OP_JUMP_IF_STACK_LT_LOCAL, // if pop() <  local[b] jump (i16 offset)
    OP_JUMP_IF_STACK_LE_LOCAL, // if pop() <= local[b] jump (i16 offset)
    OP_JUMP_IF_STACK_GT_LOCAL, // if pop() >  local[b] jump (i16 offset)
    OP_JUMP_IF_STACK_GE_LOCAL, // if pop() >= local[b] jump (i16 offset)
    OP_JUMP_IF_STACK_LT_CONST, // if pop() <  const[c] jump (i16 offset)
    OP_JUMP_IF_STACK_LE_CONST, // if pop() <= const[c] jump (i16 offset)
    OP_JUMP_IF_STACK_GT_CONST, // if pop() >  const[c] jump (i16 offset)
    OP_JUMP_IF_STACK_GE_CONST, // if pop() >= const[c] jump (i16 offset)

    // Array superinstructions (avoid pushing array values / RC churn).
    OP_ARRAY_GET_LOCAL,        // [..., idx] -> [..., elem] (u8 array_slot)
    OP_ARRAY_GET_LOCAL_CONST,  // [...] -> [..., elem] (u8 array_slot, u8 idx)
    OP_ARRAY_GET_LOCAL_LOCAL,  // [...] -> [..., elem] (u8 array_slot, u8 idx_slot)
    OP_ARRAY_SET_LOCAL,        // [..., idx, value] -> [...] (u8 array_slot)
    OP_ARRAY_SET_LOCAL_CONST,  // [..., value] -> [...] (u8 array_slot, u8 idx)
    OP_ARRAY_SET_LOCAL_LOCAL,  // [..., value] -> [...] (u8 array_slot, u8 idx_slot)

    // High-level array ops (C loops instead of bytecode loops).
    OP_ARRAY_COPY,                        // [..., dst, src] -> [...]
    OP_ARRAY_COPY_LOCAL_LOCAL,            // (u8 dst_slot, u8 src_slot)
    OP_ARRAY_REVERSE_PREFIX,              // [..., arr, hi] -> [...]
    OP_ARRAY_REVERSE_PREFIX_LOCAL_LOCAL,  // (u8 arr_slot, u8 hi_slot)
    OP_ARRAY_ROTATE_PREFIX_LEFT,          // [..., arr, hi] -> [...]
    OP_ARRAY_ROTATE_PREFIX_LEFT_LOCAL_LOCAL, // (u8 arr_slot, u8 hi_slot)
    OP_ARRAY_ROTATE_PREFIX_RIGHT,         // [..., arr, hi] -> [...]
    OP_ARRAY_ROTATE_PREFIX_RIGHT_LOCAL_LOCAL, // (u8 arr_slot, u8 hi_slot)

    // Array+record field superinstructions (avoid materializing record values / RC churn).
    OP_ARRAY_BOUNDS_CHECK_LOCAL_CONST,    // [...] -> [...] (u8 arr_slot, u8 idx)
    OP_ARRAY_BOUNDS_CHECK_LOCAL_LOCAL,    // [...] -> [...] (u8 arr_slot, u8 idx_slot)
    OP_ARRAY_GET_FIELD_LOCAL_CONST,       // [...] -> [..., field] (u8 arr_slot, u8 idx, u8 field)
    OP_ARRAY_GET_FIELD_LOCAL_LOCAL,       // [...] -> [..., field] (u8 arr_slot, u8 idx_slot, u8 field)
    OP_ARRAY_SET_FIELD_LOCAL_CONST,       // [..., value] -> [...] (u8 arr_slot, u8 idx, u8 field)
    OP_ARRAY_SET_FIELD_LOCAL_LOCAL,       // [..., value] -> [...] (u8 arr_slot, u8 idx_slot, u8 field)
    OP_JUMP_IF_ARRAY_FALSE_LOCAL_CONST,   // if array[const_idx] is false jump (u8 arr_slot, u8 idx, i16 offset)
    OP_JUMP_IF_ARRAY_FALSE_LOCAL_LOCAL,   // if array[local_idx] is false jump (u8 arr_slot, u8 idx_slot, i16 offset)

    // Math intrinsics (avoid OP_LOAD_GLOBAL + OP_CALL overhead).
    OP_SQRT,                              // [..., x] -> [..., sqrt(x)]

    // Typed numeric ops (avoid bigint/string branches in generic ops).
    OP_ADD_INT,
    OP_SUB_INT,
    OP_MUL_INT,
    OP_DIV_INT,
    OP_MOD_INT,
    OP_NEG_INT,
    OP_BIT_AND_INT,
    OP_BIT_OR_INT,
    OP_BIT_XOR_INT,
    OP_BIT_NOT_INT,
    OP_ADD_DOUBLE,
    OP_SUB_DOUBLE,
    OP_MUL_DOUBLE,
    OP_DIV_DOUBLE,
    OP_NEG_DOUBLE,

    // Typed superinstructions (avoid LOAD_LOCAL + typed op sequences).
    OP_ADD_LOCALS_INT,                    // [... ] -> [..., a+b] (u8 a_slot, u8 b_slot)
    OP_SUB_LOCALS_INT,                    // [... ] -> [..., a-b] (u8 a_slot, u8 b_slot)
    OP_MUL_LOCALS_INT,                    // [... ] -> [..., a*b] (u8 a_slot, u8 b_slot)
    OP_DIV_LOCALS_INT,                    // [... ] -> [..., a/b] (u8 a_slot, u8 b_slot)
    OP_MOD_LOCALS_INT,                    // [... ] -> [..., a%b] (u8 a_slot, u8 b_slot)
    OP_BIT_AND_LOCALS_INT,                // [... ] -> [..., a&b] (u8 a_slot, u8 b_slot)
    OP_BIT_OR_LOCALS_INT,                 // [... ] -> [..., a|b] (u8 a_slot, u8 b_slot)
    OP_BIT_XOR_LOCALS_INT,                // [... ] -> [..., a^b] (u8 a_slot, u8 b_slot)
    OP_ADD_LOCALS_DOUBLE,                 // [... ] -> [..., a+b] (u8 a_slot, u8 b_slot)
    OP_SUB_LOCALS_DOUBLE,                 // [... ] -> [..., a-b] (u8 a_slot, u8 b_slot)
    OP_MUL_LOCALS_DOUBLE,                 // [... ] -> [..., a*b] (u8 a_slot, u8 b_slot)
    OP_DIV_LOCALS_DOUBLE,                 // [... ] -> [..., a/b] (u8 a_slot, u8 b_slot)
    OP_MUL_LOCALS_INT_TO_LOCAL,           // local[dst]=local[a]*local[b] (u8 dst, u8 a, u8 b, pad, pad, pad)
    OP_MUL_LOCALS_DOUBLE_TO_LOCAL,        // local[dst]=local[a]*local[b] (u8 dst, u8 a, u8 b, pad, pad, pad)
    OP_SQRT_LOCAL_DOUBLE,                 // [... ] -> [..., sqrt(x)] (u8 x_slot)
    OP_ARRAY_LEN_LOCAL,                   // [... ] -> [..., len(arr)] (u8 arr_slot)

    // Stack+local superinstructions (avoid LOAD_LOCAL + typed op).
    OP_ADD_STACK_LOCAL_INT,               // [..., a] -> [..., a+b] (u8 b_slot)
    OP_SUB_STACK_LOCAL_INT,               // [..., a] -> [..., a-b] (u8 b_slot)
    OP_MUL_STACK_LOCAL_INT,               // [..., a] -> [..., a*b] (u8 b_slot)
    OP_DIV_STACK_LOCAL_INT,               // [..., a] -> [..., a/b] (u8 b_slot)
    OP_MOD_STACK_LOCAL_INT,               // [..., a] -> [..., a%b] (u8 b_slot)
    OP_BIT_AND_STACK_LOCAL_INT,           // [..., a] -> [..., a&b] (u8 b_slot)
    OP_BIT_OR_STACK_LOCAL_INT,            // [..., a] -> [..., a|b] (u8 b_slot)
    OP_BIT_XOR_STACK_LOCAL_INT,           // [..., a] -> [..., a^b] (u8 b_slot)
    OP_ADD_STACK_LOCAL_DOUBLE,            // [..., a] -> [..., a+b] (u8 b_slot)
    OP_SUB_STACK_LOCAL_DOUBLE,            // [..., a] -> [..., a-b] (u8 b_slot)
    OP_MUL_STACK_LOCAL_DOUBLE,            // [..., a] -> [..., a*b] (u8 b_slot)
    OP_DIV_STACK_LOCAL_DOUBLE,            // [..., a] -> [..., a/b] (u8 b_slot)

    // Const-immediate numeric superinstructions (avoid OP_CONST dispatch).
    // These are length-preserving: 3 bytes (opcode, u8 const_idx, padding).
    OP_ADD_STACK_CONST_INT,               // [..., a] -> [..., a+c] (u8 const_idx)
    OP_SUB_STACK_CONST_INT,               // [..., a] -> [..., a-c] (u8 const_idx)
    OP_MUL_STACK_CONST_INT,               // [..., a] -> [..., a*c] (u8 const_idx)
    OP_DIV_STACK_CONST_INT,               // [..., a] -> [..., a/c] (u8 const_idx)
    OP_MOD_STACK_CONST_INT,               // [..., a] -> [..., a%c] (u8 const_idx)
    OP_BIT_AND_STACK_CONST_INT,           // [..., a] -> [..., a&c] (u8 const_idx)
    OP_BIT_OR_STACK_CONST_INT,            // [..., a] -> [..., a|c] (u8 const_idx)
    OP_BIT_XOR_STACK_CONST_INT,           // [..., a] -> [..., a^c] (u8 const_idx)
    OP_ADD_STACK_CONST_DOUBLE,            // [..., a] -> [..., a+c] (u8 const_idx)
    OP_SUB_STACK_CONST_DOUBLE,            // [..., a] -> [..., a-c] (u8 const_idx)
    OP_MUL_STACK_CONST_DOUBLE,            // [..., a] -> [..., a*c] (u8 const_idx)
    OP_DIV_STACK_CONST_DOUBLE,            // [..., a] -> [..., a/c] (u8 const_idx)

    // Local+const numeric superinstructions (avoid LOAD_LOCAL + OP_CONST dispatch).
    // These are length-preserving: 5 bytes (opcode, u8 local_slot, u8 const_idx, padding, padding).
    OP_ADD_LOCAL_CONST_INT,               // [...] -> [..., local[a]+c] (u8 a_slot, u8 const_idx)
    OP_SUB_LOCAL_CONST_INT,               // [...] -> [..., local[a]-c] (u8 a_slot, u8 const_idx)
    OP_MUL_LOCAL_CONST_INT,               // [...] -> [..., local[a]*c] (u8 a_slot, u8 const_idx)
    OP_DIV_LOCAL_CONST_INT,               // [...] -> [..., local[a]/c] (u8 a_slot, u8 const_idx)
    OP_MOD_LOCAL_CONST_INT,               // [...] -> [..., local[a]%c] (u8 a_slot, u8 const_idx)
    OP_BIT_AND_LOCAL_CONST_INT,           // [...] -> [..., local[a]&c] (u8 a_slot, u8 const_idx)
    OP_BIT_OR_LOCAL_CONST_INT,            // [...] -> [..., local[a]|c] (u8 a_slot, u8 const_idx)
    OP_BIT_XOR_LOCAL_CONST_INT,           // [...] -> [..., local[a]^c] (u8 a_slot, u8 const_idx)
    OP_ADD_LOCAL_CONST_DOUBLE,            // [...] -> [..., local[a]+c] (u8 a_slot, u8 const_idx)
    OP_SUB_LOCAL_CONST_DOUBLE,            // [...] -> [..., local[a]-c] (u8 a_slot, u8 const_idx)
    OP_MUL_LOCAL_CONST_DOUBLE,            // [...] -> [..., local[a]*c] (u8 a_slot, u8 const_idx)
    OP_DIV_LOCAL_CONST_DOUBLE,            // [...] -> [..., local[a]/c] (u8 a_slot, u8 const_idx)

    // Typed array element ops (avoid kind switching in hot loops).
    OP_ARRAY_GET_LOCAL_CONST_INT,         // [...] -> [..., elem] (u8 array_slot, u8 idx)
    OP_ARRAY_GET_LOCAL_LOCAL_INT,         // [...] -> [..., elem] (u8 array_slot, u8 idx_slot)
    OP_ARRAY_SET_LOCAL_CONST_INT,         // [..., value] -> [...] (u8 array_slot, u8 idx)
    OP_ARRAY_SET_LOCAL_LOCAL_INT,         // [..., value] -> [...] (u8 array_slot, u8 idx_slot)
    OP_ARRAY_GET_LOCAL_CONST_DOUBLE,      // [...] -> [..., elem] (u8 array_slot, u8 idx)
    OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE,      // [...] -> [..., elem] (u8 array_slot, u8 idx_slot)
    OP_ARRAY_SET_LOCAL_CONST_DOUBLE,      // [..., value] -> [...] (u8 array_slot, u8 idx)
    OP_ARRAY_SET_LOCAL_LOCAL_DOUBLE,      // [..., value] -> [...] (u8 array_slot, u8 idx_slot)

    // Typed array->local superinstructions (avoid ARRAY_GET + STORE_LOCAL).
    // These are length-preserving: 5 bytes (opcode, u8 array_slot, u8 idx/idx_slot, u8 dst_slot, padding).
    OP_ARRAY_GET_LOCAL_CONST_INT_TO_LOCAL,    // local[dst]=arr[idx] (u8 array_slot, u8 idx, u8 dst_slot)
    OP_ARRAY_GET_LOCAL_LOCAL_INT_TO_LOCAL,    // local[dst]=arr[local[idx]] (u8 array_slot, u8 idx_slot, u8 dst_slot)
    OP_ARRAY_GET_LOCAL_CONST_DOUBLE_TO_LOCAL, // local[dst]=arr[idx] (u8 array_slot, u8 idx, u8 dst_slot)
    OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE_TO_LOCAL, // local[dst]=arr[local[idx]] (u8 array_slot, u8 idx_slot, u8 dst_slot)

    // Local update ops (avoid LOAD_LOCAL + op + STORE_LOCAL in hot loops).
    OP_ADD_LOCAL_STACK_INT,               // local[slot] += pop() (u8 slot)
    OP_SUB_LOCAL_STACK_INT,               // local[slot] -= pop() (u8 slot)
    OP_ADD_LOCAL_STACK_DOUBLE,            // local[slot] += pop() (u8 slot)
    OP_SUB_LOCAL_STACK_DOUBLE,            // local[slot] -= pop() (u8 slot)

    // Fused numeric+array ops (avoid ARRAY_GET + MUL + ADD patterns in hot loops).
    OP_MADD_LOCAL_ARRAY_LOCAL_INT,        // local[acc] += pop()*arr[idx] (u8 acc_slot, u8 arr_slot, u8 idx_slot)
    OP_MADD_LOCAL_ARRAY_LOCAL_DOUBLE,     // local[acc] += pop()*arr[idx] (u8 acc_slot, u8 arr_slot, u8 idx_slot)

    // Fused spectral-norm kernel ops.
    OP_EVALA_RECIP_LOCALS_DOUBLE,         // [... ] -> [..., 1.0/denom] (u8 a_slot, u8 b_slot)
    OP_EVALA_MADD_LOCAL_ARRAY_LOCAL_DOUBLE, // local[acc] += evalA(a,b)*arr[idx] (u8 acc, u8 arr, u8 idx, u8 a, u8 b, pad)

    // Double ops that avoid OP_CONST + OP_CAST_DOUBLE + OP_DIV_DOUBLE patterns.
    OP_RECIP_INT_TO_DOUBLE,              // [..., x:int] -> [..., 1.0/(double)x]

    // Equality jump superinstructions (avoid EQ/NE + JUMP_IF_FALSE patterns).
    OP_JUMP_IF_LOCAL_EQ,                 // if local[a] == local[b] jump (i16 offset)
    OP_JUMP_IF_LOCAL_NE,                 // if local[a] != local[b] jump (i16 offset)
    OP_JUMP_IF_LOCAL_EQ_CONST,           // if local[a] == const[c] jump (i16 offset)
    OP_JUMP_IF_LOCAL_NE_CONST,           // if local[a] != const[c] jump (i16 offset)

    // Inline caches / global fast paths.
    OP_CALL_GLOBAL_SLOT,                 // call global by slot index (u8 slot, u8 argc)
    OP_LOAD_GLOBAL_SLOT,                 // load global by slot index (u8 slot)
    OP_STORE_GLOBAL_SLOT,                // store global by slot index (u8 slot)

    // Global op superinstructions (avoid stack traffic / RC churn).
    // Name-based (u8 dst_name_idx, u8 a_name_idx, u8 b_name_idx, pad, pad, pad)
    OP_ADD_GLOBAL_GLOBAL_TO_GLOBAL,
    OP_SUB_GLOBAL_GLOBAL_TO_GLOBAL,
    OP_MUL_GLOBAL_GLOBAL_TO_GLOBAL,
    OP_DIV_GLOBAL_GLOBAL_TO_GLOBAL,
    // Slot-based (u8 dst_slot, u8 a_slot, u8 b_slot, pad, pad, pad)
    OP_ADD_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT,
    OP_SUB_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT,
    OP_MUL_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT,
    OP_DIV_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT,
    OP_JUMP_IF_LOCAL_EQ_GLOBAL,          // if local[a] == global[name] jump (i16 offset)
    OP_JUMP_IF_LOCAL_NE_GLOBAL,          // if local[a] != global[name] jump (i16 offset)
    OP_JUMP_IF_LOCAL_EQ_GLOBAL_SLOT,     // if local[a] == global[slot] jump (i16 offset)
    OP_JUMP_IF_LOCAL_NE_GLOBAL_SLOT,     // if local[a] != global[slot] jump (i16 offset)

    // Closure construction.
    OP_MAKE_CLOSURE,                     // [..., template_fn, cap0, ..., capN-1] -> [..., closure_fn] (u8 capture_count)

    // Interface dynamic dispatch and runtime conformance checks.
    OP_TYPE_TEST_INTERFACE_METHOD,      // test whether top value can satisfy interface method (u16 interface_name_idx, u16 method_name_idx or 0xffff)
    OP_CALL_INTERFACE,                   // call interface method (u16 interface_name_idx, u16 method_name_idx, u8 argc)

    // Record construction with runtime type tag.
    OP_RECORD_NEW_NAMED                  // create record (u16 record_name_idx, u8 field_count)
} OpCode;

typedef struct {
    int index;
    int line;
} DebugInfo;

typedef struct {
    uint8_t* code;
    DebugInfo* debug_info;
    int code_count;
    int code_capacity;
} Chunk;

typedef struct {
    union {
        int64_t as_int;
        double as_double;
        char* as_string;
    };
    int type_index;
} Constant;

typedef struct {
    Constant* constants;
    int constant_count;
    int constant_capacity;
} ConstantPool;

void chunk_init(Chunk* chunk);
void chunk_free(Chunk* chunk);
int chunk_emit(Chunk* chunk, uint8_t byte, int line);
void chunk_set_debug_info(Chunk* chunk, int pc, int line);
uint8_t chunk_get(Chunk* chunk, int offset);

void constant_pool_init(ConstantPool* pool);
void constant_pool_free(ConstantPool* pool);
int constant_pool_add(ConstantPool* pool, Constant constant);

const char* op_code_to_string(OpCode op);

#endif
