#include "bytecode.h"
#include "safe_alloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void chunk_init(Chunk* chunk) {
    chunk->code = NULL;
    chunk->debug_info = NULL;
    chunk->code_count = 0;
    chunk->code_capacity = 0;
}

void chunk_free(Chunk* chunk) {
    if (chunk->code) free(chunk->code);
    if (chunk->debug_info) free(chunk->debug_info);
    chunk_init(chunk);
}

int chunk_emit(Chunk* chunk, uint8_t byte, int line) {
    chunk->code_count++;
    if (chunk->code_count > chunk->code_capacity) {
        chunk->code_capacity = chunk->code_count * 2;
        chunk->code = (uint8_t*)safe_realloc(chunk->code, chunk->code_capacity * sizeof(uint8_t));
        chunk->debug_info = (DebugInfo*)safe_realloc(chunk->debug_info, chunk->code_capacity * sizeof(DebugInfo));
    }
    
    chunk->code[chunk->code_count - 1] = byte;
    chunk_set_debug_info(chunk, chunk->code_count - 1, line);
    
    return chunk->code_count - 1;
}

void chunk_set_debug_info(Chunk* chunk, int pc, int line) {
    if (pc >= 0 && pc < chunk->code_count) {
        chunk->debug_info[pc].index = pc;
        chunk->debug_info[pc].line = line;
    }
}

uint8_t chunk_get(Chunk* chunk, int offset) {
    if (offset >= 0 && offset < chunk->code_count) {
        return chunk->code[offset];
    }
    return 0;
}

void constant_pool_init(ConstantPool* pool) {
    pool->constants = NULL;
    pool->constant_count = 0;
    pool->constant_capacity = 0;
}

void constant_pool_free(ConstantPool* pool) {
    for (int i = 0; i < pool->constant_count; i++) {
        if ((pool->constants[i].type_index == 2 || pool->constants[i].type_index == 3) &&
            pool->constants[i].as_string) {
            free(pool->constants[i].as_string);
        }
    }
    if (pool->constants) free(pool->constants);
    constant_pool_init(pool);
}

int constant_pool_add(ConstantPool* pool, Constant constant) {
    pool->constant_count++;
    if (pool->constant_count > pool->constant_capacity) {
        pool->constant_capacity = pool->constant_count * 2;
        pool->constants = (Constant*)safe_realloc(pool->constants, pool->constant_capacity * sizeof(Constant));
    }
    
    if ((constant.type_index == 2 || constant.type_index == 3) && constant.as_string) {
        constant.as_string = safe_strdup(constant.as_string);
    }
    
    pool->constants[pool->constant_count - 1] = constant;
    return pool->constant_count - 1;
}

const char* op_code_to_string(OpCode op) {
    switch (op) {
        case OP_NOP: return "OP_NOP";
        case OP_CONST: return "OP_CONST";
        case OP_CONST16: return "OP_CONST16";
        case OP_LOAD_LOCAL: return "OP_LOAD_LOCAL";
        case OP_STORE_LOCAL: return "OP_STORE_LOCAL";
        case OP_ADD_LOCAL_CONST: return "OP_ADD_LOCAL_CONST";
        case OP_SUB_LOCAL_CONST: return "OP_SUB_LOCAL_CONST";
        case OP_ADD2_LOCAL_CONST: return "OP_ADD2_LOCAL_CONST";
        case OP_NEGATE_LOCAL: return "OP_NEGATE_LOCAL";
        case OP_LOAD_GLOBAL: return "OP_LOAD_GLOBAL";
        case OP_LOAD_GLOBAL16: return "OP_LOAD_GLOBAL16";
        case OP_STORE_GLOBAL: return "OP_STORE_GLOBAL";
        case OP_STORE_GLOBAL16: return "OP_STORE_GLOBAL16";
        case OP_ADD: return "OP_ADD";
        case OP_SUB: return "OP_SUB";
        case OP_MUL: return "OP_MUL";
        case OP_DIV: return "OP_DIV";
        case OP_MOD: return "OP_MOD";
        case OP_NEG: return "OP_NEG";
        case OP_EQ: return "OP_EQ";
        case OP_NE: return "OP_NE";
        case OP_LT: return "OP_LT";
        case OP_LE: return "OP_LE";
        case OP_GT: return "OP_GT";
        case OP_GE: return "OP_GE";
        case OP_NOT: return "OP_NOT";
        case OP_AND: return "OP_AND";
        case OP_OR: return "OP_OR";
        case OP_BIT_NOT: return "OP_BIT_NOT";
        case OP_BIT_AND: return "OP_BIT_AND";
        case OP_BIT_OR: return "OP_BIT_OR";
        case OP_BIT_XOR: return "OP_BIT_XOR";
        case OP_JUMP: return "OP_JUMP";
        case OP_JUMP_IF_FALSE: return "OP_JUMP_IF_FALSE";
        case OP_CALL: return "OP_CALL";
        case OP_CALL_GLOBAL: return "OP_CALL_GLOBAL";
        case OP_CALL_GLOBAL16: return "OP_CALL_GLOBAL16";
        case OP_DEFER: return "OP_DEFER";
        case OP_DEFER_HAS: return "OP_DEFER_HAS";
        case OP_DEFER_CALL: return "OP_DEFER_CALL";
        case OP_AWAIT: return "OP_AWAIT";
        case OP_RET: return "OP_RET";
        case OP_ARRAY_NEW: return "OP_ARRAY_NEW";
        case OP_ARRAY_GET: return "OP_ARRAY_GET";
        case OP_ARRAY_SET: return "OP_ARRAY_SET";
        case OP_ARRAY_LEN: return "OP_ARRAY_LEN";
        case OP_ARRAY_PUSH: return "OP_ARRAY_PUSH";
        case OP_ARRAY_POP: return "OP_ARRAY_POP";
        case OP_STRING_LEN: return "OP_STRING_LEN";
        case OP_STRING_CONCAT: return "OP_STRING_CONCAT";
        case OP_TYPEOF: return "OP_TYPEOF";
        case OP_CAST_INT: return "OP_CAST_INT";
        case OP_CAST_BOOL: return "OP_CAST_BOOL";
        case OP_CAST_DOUBLE: return "OP_CAST_DOUBLE";
        case OP_CAST_STRING: return "OP_CAST_STRING";
        case OP_CAST_BIGINT: return "OP_CAST_BIGINT";
        case OP_PRINT: return "OP_PRINT";
        case OP_PRINTLN: return "OP_PRINTLN";
        case OP_POP: return "OP_POP";
        case OP_DUP: return "OP_DUP";
        case OP_TRY: return "OP_TRY";
        case OP_THROW: return "OP_THROW";
        case OP_CATCH: return "OP_CATCH";
        case OP_RECORD_NEW: return "OP_RECORD_NEW";
        case OP_RECORD_SET_FIELD: return "OP_RECORD_SET_FIELD";
        case OP_RECORD_GET_FIELD: return "OP_RECORD_GET_FIELD";
        case OP_TUPLE_NEW: return "OP_TUPLE_NEW";
        case OP_TUPLE_GET: return "OP_TUPLE_GET";
        case OP_TUPLE_SET: return "OP_TUPLE_SET";
        case OP_MAP_NEW: return "OP_MAP_NEW";
        case OP_MAP_SET: return "OP_MAP_SET";
        case OP_MAP_GET: return "OP_MAP_GET";
        case OP_MAP_HAS: return "OP_MAP_HAS";
        case OP_MAP_DELETE: return "OP_MAP_DELETE";
        case OP_MAP_KEYS: return "OP_MAP_KEYS";
        case OP_MAP_VALUES: return "OP_MAP_VALUES";
        case OP_SET_NEW: return "OP_SET_NEW";
        case OP_SET_ADD: return "OP_SET_ADD";
        case OP_SET_HAS: return "OP_SET_HAS";
        case OP_SET_REMOVE: return "OP_SET_REMOVE";
        case OP_SET_TO_ARRAY: return "OP_SET_TO_ARRAY";
        case OP_ADD_LOCAL_DIV_LOCALS: return "OP_ADD_LOCAL_DIV_LOCALS";
        case OP_JUMP_IF_LOCAL_LT: return "OP_JUMP_IF_LOCAL_LT";
        case OP_JUMP_IF_LOCAL_LE: return "OP_JUMP_IF_LOCAL_LE";
        case OP_JUMP_IF_LOCAL_GT: return "OP_JUMP_IF_LOCAL_GT";
        case OP_JUMP_IF_LOCAL_GE: return "OP_JUMP_IF_LOCAL_GE";
        case OP_JUMP_IF_LOCAL_LT_CONST: return "OP_JUMP_IF_LOCAL_LT_CONST";
        case OP_JUMP_IF_LOCAL_LE_CONST: return "OP_JUMP_IF_LOCAL_LE_CONST";
        case OP_JUMP_IF_LOCAL_GT_CONST: return "OP_JUMP_IF_LOCAL_GT_CONST";
        case OP_JUMP_IF_LOCAL_GE_CONST: return "OP_JUMP_IF_LOCAL_GE_CONST";
        case OP_JUMP_IF_FALSE_POP: return "OP_JUMP_IF_FALSE_POP";
        case OP_JUMP_IF_STACK_LT_LOCAL: return "OP_JUMP_IF_STACK_LT_LOCAL";
        case OP_JUMP_IF_STACK_LE_LOCAL: return "OP_JUMP_IF_STACK_LE_LOCAL";
        case OP_JUMP_IF_STACK_GT_LOCAL: return "OP_JUMP_IF_STACK_GT_LOCAL";
        case OP_JUMP_IF_STACK_GE_LOCAL: return "OP_JUMP_IF_STACK_GE_LOCAL";
        case OP_JUMP_IF_STACK_LT_CONST: return "OP_JUMP_IF_STACK_LT_CONST";
        case OP_JUMP_IF_STACK_LE_CONST: return "OP_JUMP_IF_STACK_LE_CONST";
        case OP_JUMP_IF_STACK_GT_CONST: return "OP_JUMP_IF_STACK_GT_CONST";
        case OP_JUMP_IF_STACK_GE_CONST: return "OP_JUMP_IF_STACK_GE_CONST";
        case OP_ARRAY_GET_LOCAL: return "OP_ARRAY_GET_LOCAL";
        case OP_ARRAY_GET_LOCAL_CONST: return "OP_ARRAY_GET_LOCAL_CONST";
        case OP_ARRAY_GET_LOCAL_LOCAL: return "OP_ARRAY_GET_LOCAL_LOCAL";
        case OP_ARRAY_SET_LOCAL: return "OP_ARRAY_SET_LOCAL";
        case OP_ARRAY_SET_LOCAL_CONST: return "OP_ARRAY_SET_LOCAL_CONST";
        case OP_ARRAY_SET_LOCAL_LOCAL: return "OP_ARRAY_SET_LOCAL_LOCAL";
        case OP_ARRAY_COPY: return "OP_ARRAY_COPY";
        case OP_ARRAY_COPY_LOCAL_LOCAL: return "OP_ARRAY_COPY_LOCAL_LOCAL";
        case OP_ARRAY_REVERSE_PREFIX: return "OP_ARRAY_REVERSE_PREFIX";
        case OP_ARRAY_REVERSE_PREFIX_LOCAL_LOCAL: return "OP_ARRAY_REVERSE_PREFIX_LOCAL_LOCAL";
        case OP_ARRAY_ROTATE_PREFIX_LEFT: return "OP_ARRAY_ROTATE_PREFIX_LEFT";
        case OP_ARRAY_ROTATE_PREFIX_LEFT_LOCAL_LOCAL: return "OP_ARRAY_ROTATE_PREFIX_LEFT_LOCAL_LOCAL";
        case OP_ARRAY_ROTATE_PREFIX_RIGHT: return "OP_ARRAY_ROTATE_PREFIX_RIGHT";
        case OP_ARRAY_ROTATE_PREFIX_RIGHT_LOCAL_LOCAL: return "OP_ARRAY_ROTATE_PREFIX_RIGHT_LOCAL_LOCAL";
        case OP_ARRAY_BOUNDS_CHECK_LOCAL_CONST: return "OP_ARRAY_BOUNDS_CHECK_LOCAL_CONST";
        case OP_ARRAY_BOUNDS_CHECK_LOCAL_LOCAL: return "OP_ARRAY_BOUNDS_CHECK_LOCAL_LOCAL";
        case OP_ARRAY_GET_FIELD_LOCAL_CONST: return "OP_ARRAY_GET_FIELD_LOCAL_CONST";
        case OP_ARRAY_GET_FIELD_LOCAL_LOCAL: return "OP_ARRAY_GET_FIELD_LOCAL_LOCAL";
        case OP_ARRAY_SET_FIELD_LOCAL_CONST: return "OP_ARRAY_SET_FIELD_LOCAL_CONST";
        case OP_ARRAY_SET_FIELD_LOCAL_LOCAL: return "OP_ARRAY_SET_FIELD_LOCAL_LOCAL";
        case OP_JUMP_IF_ARRAY_FALSE_LOCAL_CONST: return "OP_JUMP_IF_ARRAY_FALSE_LOCAL_CONST";
        case OP_JUMP_IF_ARRAY_FALSE_LOCAL_LOCAL: return "OP_JUMP_IF_ARRAY_FALSE_LOCAL_LOCAL";
        case OP_SQRT: return "OP_SQRT";
        case OP_ADD_INT: return "OP_ADD_INT";
        case OP_SUB_INT: return "OP_SUB_INT";
        case OP_MUL_INT: return "OP_MUL_INT";
        case OP_DIV_INT: return "OP_DIV_INT";
        case OP_MOD_INT: return "OP_MOD_INT";
        case OP_NEG_INT: return "OP_NEG_INT";
        case OP_BIT_AND_INT: return "OP_BIT_AND_INT";
        case OP_BIT_OR_INT: return "OP_BIT_OR_INT";
        case OP_BIT_XOR_INT: return "OP_BIT_XOR_INT";
        case OP_BIT_NOT_INT: return "OP_BIT_NOT_INT";
        case OP_ADD_DOUBLE: return "OP_ADD_DOUBLE";
        case OP_SUB_DOUBLE: return "OP_SUB_DOUBLE";
        case OP_MUL_DOUBLE: return "OP_MUL_DOUBLE";
        case OP_DIV_DOUBLE: return "OP_DIV_DOUBLE";
        case OP_NEG_DOUBLE: return "OP_NEG_DOUBLE";
        case OP_ADD_LOCALS_INT: return "OP_ADD_LOCALS_INT";
        case OP_SUB_LOCALS_INT: return "OP_SUB_LOCALS_INT";
        case OP_MUL_LOCALS_INT: return "OP_MUL_LOCALS_INT";
        case OP_DIV_LOCALS_INT: return "OP_DIV_LOCALS_INT";
        case OP_MOD_LOCALS_INT: return "OP_MOD_LOCALS_INT";
        case OP_BIT_AND_LOCALS_INT: return "OP_BIT_AND_LOCALS_INT";
        case OP_BIT_OR_LOCALS_INT: return "OP_BIT_OR_LOCALS_INT";
        case OP_BIT_XOR_LOCALS_INT: return "OP_BIT_XOR_LOCALS_INT";
        case OP_ADD_LOCALS_DOUBLE: return "OP_ADD_LOCALS_DOUBLE";
        case OP_SUB_LOCALS_DOUBLE: return "OP_SUB_LOCALS_DOUBLE";
        case OP_MUL_LOCALS_DOUBLE: return "OP_MUL_LOCALS_DOUBLE";
        case OP_DIV_LOCALS_DOUBLE: return "OP_DIV_LOCALS_DOUBLE";
        case OP_MUL_LOCALS_INT_TO_LOCAL: return "OP_MUL_LOCALS_INT_TO_LOCAL";
        case OP_MUL_LOCALS_DOUBLE_TO_LOCAL: return "OP_MUL_LOCALS_DOUBLE_TO_LOCAL";
        case OP_SQRT_LOCAL_DOUBLE: return "OP_SQRT_LOCAL_DOUBLE";
        case OP_ARRAY_LEN_LOCAL: return "OP_ARRAY_LEN_LOCAL";
        case OP_ADD_STACK_LOCAL_INT: return "OP_ADD_STACK_LOCAL_INT";
        case OP_SUB_STACK_LOCAL_INT: return "OP_SUB_STACK_LOCAL_INT";
        case OP_MUL_STACK_LOCAL_INT: return "OP_MUL_STACK_LOCAL_INT";
        case OP_DIV_STACK_LOCAL_INT: return "OP_DIV_STACK_LOCAL_INT";
        case OP_MOD_STACK_LOCAL_INT: return "OP_MOD_STACK_LOCAL_INT";
        case OP_BIT_AND_STACK_LOCAL_INT: return "OP_BIT_AND_STACK_LOCAL_INT";
        case OP_BIT_OR_STACK_LOCAL_INT: return "OP_BIT_OR_STACK_LOCAL_INT";
        case OP_BIT_XOR_STACK_LOCAL_INT: return "OP_BIT_XOR_STACK_LOCAL_INT";
        case OP_ADD_STACK_LOCAL_DOUBLE: return "OP_ADD_STACK_LOCAL_DOUBLE";
        case OP_SUB_STACK_LOCAL_DOUBLE: return "OP_SUB_STACK_LOCAL_DOUBLE";
        case OP_MUL_STACK_LOCAL_DOUBLE: return "OP_MUL_STACK_LOCAL_DOUBLE";
        case OP_DIV_STACK_LOCAL_DOUBLE: return "OP_DIV_STACK_LOCAL_DOUBLE";
        case OP_ADD_STACK_CONST_INT: return "OP_ADD_STACK_CONST_INT";
        case OP_SUB_STACK_CONST_INT: return "OP_SUB_STACK_CONST_INT";
        case OP_MUL_STACK_CONST_INT: return "OP_MUL_STACK_CONST_INT";
        case OP_DIV_STACK_CONST_INT: return "OP_DIV_STACK_CONST_INT";
        case OP_MOD_STACK_CONST_INT: return "OP_MOD_STACK_CONST_INT";
        case OP_BIT_AND_STACK_CONST_INT: return "OP_BIT_AND_STACK_CONST_INT";
        case OP_BIT_OR_STACK_CONST_INT: return "OP_BIT_OR_STACK_CONST_INT";
        case OP_BIT_XOR_STACK_CONST_INT: return "OP_BIT_XOR_STACK_CONST_INT";
        case OP_ADD_STACK_CONST_DOUBLE: return "OP_ADD_STACK_CONST_DOUBLE";
        case OP_SUB_STACK_CONST_DOUBLE: return "OP_SUB_STACK_CONST_DOUBLE";
        case OP_MUL_STACK_CONST_DOUBLE: return "OP_MUL_STACK_CONST_DOUBLE";
        case OP_DIV_STACK_CONST_DOUBLE: return "OP_DIV_STACK_CONST_DOUBLE";
        case OP_ADD_LOCAL_CONST_INT: return "OP_ADD_LOCAL_CONST_INT";
        case OP_SUB_LOCAL_CONST_INT: return "OP_SUB_LOCAL_CONST_INT";
        case OP_MUL_LOCAL_CONST_INT: return "OP_MUL_LOCAL_CONST_INT";
        case OP_DIV_LOCAL_CONST_INT: return "OP_DIV_LOCAL_CONST_INT";
        case OP_MOD_LOCAL_CONST_INT: return "OP_MOD_LOCAL_CONST_INT";
        case OP_BIT_AND_LOCAL_CONST_INT: return "OP_BIT_AND_LOCAL_CONST_INT";
        case OP_BIT_OR_LOCAL_CONST_INT: return "OP_BIT_OR_LOCAL_CONST_INT";
        case OP_BIT_XOR_LOCAL_CONST_INT: return "OP_BIT_XOR_LOCAL_CONST_INT";
        case OP_ADD_LOCAL_CONST_DOUBLE: return "OP_ADD_LOCAL_CONST_DOUBLE";
        case OP_SUB_LOCAL_CONST_DOUBLE: return "OP_SUB_LOCAL_CONST_DOUBLE";
        case OP_MUL_LOCAL_CONST_DOUBLE: return "OP_MUL_LOCAL_CONST_DOUBLE";
        case OP_DIV_LOCAL_CONST_DOUBLE: return "OP_DIV_LOCAL_CONST_DOUBLE";
        case OP_ARRAY_GET_LOCAL_CONST_INT: return "OP_ARRAY_GET_LOCAL_CONST_INT";
        case OP_ARRAY_GET_LOCAL_LOCAL_INT: return "OP_ARRAY_GET_LOCAL_LOCAL_INT";
        case OP_ARRAY_SET_LOCAL_CONST_INT: return "OP_ARRAY_SET_LOCAL_CONST_INT";
        case OP_ARRAY_SET_LOCAL_LOCAL_INT: return "OP_ARRAY_SET_LOCAL_LOCAL_INT";
        case OP_ARRAY_GET_LOCAL_CONST_DOUBLE: return "OP_ARRAY_GET_LOCAL_CONST_DOUBLE";
        case OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE: return "OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE";
        case OP_ARRAY_SET_LOCAL_CONST_DOUBLE: return "OP_ARRAY_SET_LOCAL_CONST_DOUBLE";
        case OP_ARRAY_SET_LOCAL_LOCAL_DOUBLE: return "OP_ARRAY_SET_LOCAL_LOCAL_DOUBLE";
        case OP_ARRAY_GET_LOCAL_CONST_INT_TO_LOCAL: return "OP_ARRAY_GET_LOCAL_CONST_INT_TO_LOCAL";
        case OP_ARRAY_GET_LOCAL_LOCAL_INT_TO_LOCAL: return "OP_ARRAY_GET_LOCAL_LOCAL_INT_TO_LOCAL";
        case OP_ARRAY_GET_LOCAL_CONST_DOUBLE_TO_LOCAL: return "OP_ARRAY_GET_LOCAL_CONST_DOUBLE_TO_LOCAL";
        case OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE_TO_LOCAL: return "OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE_TO_LOCAL";
        case OP_ADD_LOCAL_STACK_INT: return "OP_ADD_LOCAL_STACK_INT";
        case OP_SUB_LOCAL_STACK_INT: return "OP_SUB_LOCAL_STACK_INT";
        case OP_ADD_LOCAL_STACK_DOUBLE: return "OP_ADD_LOCAL_STACK_DOUBLE";
        case OP_SUB_LOCAL_STACK_DOUBLE: return "OP_SUB_LOCAL_STACK_DOUBLE";
        case OP_MADD_LOCAL_ARRAY_LOCAL_INT: return "OP_MADD_LOCAL_ARRAY_LOCAL_INT";
        case OP_MADD_LOCAL_ARRAY_LOCAL_DOUBLE: return "OP_MADD_LOCAL_ARRAY_LOCAL_DOUBLE";
        case OP_EVALA_RECIP_LOCALS_DOUBLE: return "OP_EVALA_RECIP_LOCALS_DOUBLE";
        case OP_EVALA_MADD_LOCAL_ARRAY_LOCAL_DOUBLE: return "OP_EVALA_MADD_LOCAL_ARRAY_LOCAL_DOUBLE";
        case OP_RECIP_INT_TO_DOUBLE: return "OP_RECIP_INT_TO_DOUBLE";
        case OP_JUMP_IF_LOCAL_EQ: return "OP_JUMP_IF_LOCAL_EQ";
        case OP_JUMP_IF_LOCAL_NE: return "OP_JUMP_IF_LOCAL_NE";
        case OP_JUMP_IF_LOCAL_EQ_CONST: return "OP_JUMP_IF_LOCAL_EQ_CONST";
        case OP_JUMP_IF_LOCAL_NE_CONST: return "OP_JUMP_IF_LOCAL_NE_CONST";
        case OP_CALL_GLOBAL_SLOT: return "OP_CALL_GLOBAL_SLOT";
        case OP_LOAD_GLOBAL_SLOT: return "OP_LOAD_GLOBAL_SLOT";
        case OP_STORE_GLOBAL_SLOT: return "OP_STORE_GLOBAL_SLOT";
        case OP_ADD_GLOBAL_GLOBAL_TO_GLOBAL: return "OP_ADD_GLOBAL_GLOBAL_TO_GLOBAL";
        case OP_SUB_GLOBAL_GLOBAL_TO_GLOBAL: return "OP_SUB_GLOBAL_GLOBAL_TO_GLOBAL";
        case OP_MUL_GLOBAL_GLOBAL_TO_GLOBAL: return "OP_MUL_GLOBAL_GLOBAL_TO_GLOBAL";
        case OP_DIV_GLOBAL_GLOBAL_TO_GLOBAL: return "OP_DIV_GLOBAL_GLOBAL_TO_GLOBAL";
        case OP_ADD_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT: return "OP_ADD_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT";
        case OP_SUB_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT: return "OP_SUB_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT";
        case OP_MUL_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT: return "OP_MUL_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT";
        case OP_DIV_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT: return "OP_DIV_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT";
        case OP_JUMP_IF_LOCAL_EQ_GLOBAL: return "OP_JUMP_IF_LOCAL_EQ_GLOBAL";
        case OP_JUMP_IF_LOCAL_NE_GLOBAL: return "OP_JUMP_IF_LOCAL_NE_GLOBAL";
        case OP_JUMP_IF_LOCAL_EQ_GLOBAL_SLOT: return "OP_JUMP_IF_LOCAL_EQ_GLOBAL_SLOT";
        case OP_JUMP_IF_LOCAL_NE_GLOBAL_SLOT: return "OP_JUMP_IF_LOCAL_NE_GLOBAL_SLOT";
        case OP_MAKE_CLOSURE: return "OP_MAKE_CLOSURE";
        case OP_TYPE_TEST_INTERFACE_METHOD: return "OP_TYPE_TEST_INTERFACE_METHOD";
        case OP_CALL_INTERFACE: return "OP_CALL_INTERFACE";
        case OP_RECORD_NEW_NAMED: return "OP_RECORD_NEW_NAMED";
        default: return "UNKNOWN";
    }
}
