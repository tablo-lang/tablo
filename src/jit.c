#include "jit.h"
#include "safe_alloc.h"
#include <stdlib.h>
#include <string.h>

static int jit_profile_entry_cmp_desc(const void* a, const void* b) {
    const JitProfileEntry* aa = (const JitProfileEntry*)a;
    const JitProfileEntry* bb = (const JitProfileEntry*)b;
    if (aa->entry_count < bb->entry_count) return 1;
    if (aa->entry_count > bb->entry_count) return -1;

    const char* aa_name = (aa->function && aa->function->name) ? aa->function->name : "";
    const char* bb_name = (bb->function && bb->function->name) ? bb->function->name : "";
    return strcmp(aa_name, bb_name);
}

const char* jit_function_state_name(JitFunctionState state) {
    switch (state) {
        case JIT_FUNC_STATE_COLD:
            return "cold";
        case JIT_FUNC_STATE_QUEUED:
            return "queued";
        case JIT_FUNC_STATE_COMPILING:
            return "compiling";
        case JIT_FUNC_STATE_COMPILED_STUB:
            return "compiled-stub";
        case JIT_FUNC_STATE_COMPILED_NATIVE:
            return "compiled-native";
        case JIT_FUNC_STATE_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

const char* jit_function_reason_name(JitFunctionReason reason) {
    switch (reason) {
        case JIT_REASON_NONE:
            return "none";
        case JIT_REASON_QUEUED_HOT:
            return "queued-hot";
        case JIT_REASON_NATIVE_HINT:
            return "native-hint";
        case JIT_REASON_NATIVE_EXACT:
            return "native-exact";
        case JIT_REASON_STUB_FALLBACK:
            return "stub-fallback";
        case JIT_REASON_UNSUPPORTED_ASYNC:
            return "unsupported-async";
        case JIT_REASON_UNSUPPORTED_SHAPE:
            return "unsupported-shape";
        default:
            return "unknown";
    }
}

static const char* jit_profile_support_name(uint8_t support_mask) {
    switch (support_mask) {
        case JIT_PROFILE_SUPPORT_NONE:
            return "none";
        case JIT_PROFILE_SUPPORT_STUB:
            return "stub";
        case JIT_PROFILE_SUPPORT_STUB | JIT_PROFILE_SUPPORT_NATIVE_SUMMARY:
            return "stub+native-summary";
        case JIT_PROFILE_SUPPORT_NATIVE_SUMMARY:
            return "native-summary";
        default:
            return "custom";
    }
}

static const char* jit_profile_native_family_name(uint8_t native_family_mask) {
    switch (native_family_mask) {
        case JIT_PROFILE_NATIVE_FAMILY_NONE:
            return "none";
        case JIT_PROFILE_NATIVE_FAMILY_ARITHMETIC:
            return "arithmetic";
        case JIT_PROFILE_NATIVE_FAMILY_COMPARE:
            return "compare";
        case JIT_PROFILE_NATIVE_FAMILY_SELECTOR:
            return "selector";
        default:
            return "mixed";
    }
}

static bool jit_compiler_hint_kind_supported(JitCompiledKind kind) {
    switch (kind) {
        case JIT_COMPILED_KIND_INT_ADD_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_ADD_CONST:
        case JIT_COMPILED_KIND_INT_MUL_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MUL_CONST:
        case JIT_COMPILED_KIND_INT_SUB_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SUB_CONST:
        case JIT_COMPILED_KIND_INT_DIV_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_DIV_CONST:
        case JIT_COMPILED_KIND_INT_MOD_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MOD_CONST:
        case JIT_COMPILED_KIND_INT_BIT_AND_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_AND_CONST:
        case JIT_COMPILED_KIND_INT_BIT_OR_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_OR_CONST:
        case JIT_COMPILED_KIND_INT_BIT_XOR_LOCAL_CONST:
        case JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_XOR_CONST:
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
        case JIT_COMPILED_KIND_BOOL_LT_LOCAL_CONST:
        case JIT_COMPILED_KIND_BOOL_LE_LOCAL_CONST:
        case JIT_COMPILED_KIND_BOOL_EQ_LOCAL_CONST:
        case JIT_COMPILED_KIND_BOOL_NE_LOCAL_CONST:
        case JIT_COMPILED_KIND_BOOL_GT_LOCAL_CONST:
        case JIT_COMPILED_KIND_BOOL_GE_LOCAL_CONST:
        case JIT_COMPILED_KIND_BOOL_COMPARE_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_BOOL_COMPARE_GUARDED_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_BOOL_COMPARE_LOCAL_CONST_GENERIC:
        case JIT_COMPILED_KIND_INT_SELECTOR_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_INT_SELECTOR_LOCAL_CONST_GENERIC:
        case JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCAL_CONST_GENERIC:
        case JIT_COMPILED_KIND_INT_BINARY_LOCAL_CONST_GENERIC:
        case JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCAL_CONST_GENERIC:
        case JIT_COMPILED_KIND_INT_BINARY_LOCALS_GENERIC:
        case JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCALS_GENERIC:
            return true;
        default:
            return false;
    }
}

static bool jit_compiled_plan_equals(const JitCompiledPlan* a, const JitCompiledPlan* b) {
    if (!a || !b) return false;
    return a->kind == b->kind &&
           a->op == b->op &&
           a->flags == b->flags &&
           a->local_slot == b->local_slot &&
           a->local_slot_b == b->local_slot_b &&
           a->int_const0 == b->int_const0 &&
           a->int_const1 == b->int_const1;
}

static bool jit_map_local_const_summary_kind(JitSummaryOp op,
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

static bool jit_map_twoarg_int_summary_kind(JitSummaryOp op,
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

static bool jit_map_twoarg_bool_summary_kind(JitSummaryOp op,
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

static bool jit_map_local_const_bool_summary_kind(JitSummaryOp op,
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

static bool jit_map_twoarg_selector_summary_kind(JitSummaryOp op,
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

static bool jit_map_local_const_selector_summary_kind(JitSummaryOp op,
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

static bool jit_plan_from_summary(const JitFunctionSummary* summary,
                                  JitCompiledPlan* out_plan) {
    if (!summary || !out_plan || summary->kind == JIT_SUMMARY_KIND_NONE) return false;

    memset(out_plan, 0, sizeof(*out_plan));
    switch (summary->kind) {
        case JIT_SUMMARY_KIND_INT_LOCAL_CONST_BINARY:
            if (!jit_map_local_const_summary_kind(summary->op, false, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->int_const0 = summary->int_const0;
            return true;
        case JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_BINARY:
            if (!jit_map_local_const_summary_kind(summary->op, true, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->int_const0 = summary->int_const0;
            out_plan->int_const1 = summary->int_const1;
            return true;
        case JIT_SUMMARY_KIND_INT_TWOARG_BINARY:
            if (!jit_map_twoarg_int_summary_kind(summary->op, false, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            return true;
        case JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_BINARY:
            if (!jit_map_twoarg_int_summary_kind(summary->op, true, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            out_plan->int_const0 = summary->int_const0;
            return true;
        case JIT_SUMMARY_KIND_BOOL_TWOARG_COMPARE:
            if (!jit_map_twoarg_bool_summary_kind(summary->op, false, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            return true;
        case JIT_SUMMARY_KIND_BOOL_LOCAL_CONST_COMPARE:
            if (!jit_map_local_const_bool_summary_kind(summary->op, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->int_const0 = summary->int_const0;
            return true;
        case JIT_SUMMARY_KIND_BOOL_GUARDED_TWOARG_COMPARE:
            if (!jit_map_twoarg_bool_summary_kind(summary->op, true, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            out_plan->int_const0 = summary->int_const0;
            out_plan->int_const1 = summary->int_const1;
            return true;
        case JIT_SUMMARY_KIND_INT_TWOARG_SELECTOR:
            if (!jit_map_twoarg_selector_summary_kind(summary->op, false, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            return true;
        case JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_SELECTOR:
            if (!jit_map_twoarg_selector_summary_kind(summary->op, true, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            out_plan->int_const0 = summary->int_const0;
            return true;
        case JIT_SUMMARY_KIND_INT_LOCAL_CONST_SELECTOR:
            if (!jit_map_local_const_selector_summary_kind(summary->op,
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
            if (!jit_map_local_const_selector_summary_kind(summary->op,
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

static bool jit_plan_matches_compiler_metadata(const ObjFunction* function,
                                               const JitCompiledPlan* plan) {
    JitCompiledPlan summary_plan;
    if (function && jit_plan_from_summary(&function->jit_profile.summary, &summary_plan) &&
        jit_compiled_plan_equals(plan, &summary_plan)) {
        return true;
    }
    return function && jit_compiled_plan_equals(plan, &function->jit_hint_plan);
}

static bool jit_try_apply_compiler_summary(ObjFunction* function) {
    JitCompiledPlan plan;
    if (!function || function->jit_profile.summary.kind == JIT_SUMMARY_KIND_NONE) return false;
    if ((function->jit_profile.support_mask & JIT_PROFILE_SUPPORT_NATIVE_SUMMARY) == 0) return false;
    if (!jit_plan_from_summary(&function->jit_profile.summary, &plan)) return false;
    if (!jit_compiler_hint_kind_supported(plan.kind)) return false;

    function->jit_compiled_plan = plan;
    function->jit_compiled_entry = (void*)&vm_jit_native_compiled_entry;
    return true;
}

static bool jit_try_apply_compiler_hint(ObjFunction* function) {
    if (!function || function->jit_hint_plan.kind == JIT_COMPILED_KIND_NONE) return false;
    if ((function->jit_profile.support_mask & JIT_PROFILE_SUPPORT_NATIVE_SUMMARY) == 0) return false;
    if (!jit_compiler_hint_kind_supported(function->jit_hint_plan.kind)) return false;

    function->jit_compiled_plan = function->jit_hint_plan;
    function->jit_compiled_entry = (void*)&vm_jit_native_compiled_entry;
    return true;
}

static bool jit_try_apply_recovered_summary(ObjFunction* function,
                                            const JitFunctionSummary* summary) {
    JitCompiledPlan plan;
    if (!function || !summary) return false;
    if (!jit_plan_from_summary(summary, &plan)) return false;
    if (!jit_compiler_hint_kind_supported(plan.kind)) return false;
    function->jit_compiled_plan = plan;
    function->jit_compiled_entry = (void*)&vm_jit_native_compiled_entry;
    return true;
}

static bool jit_has_trailing_nil_return(const ObjFunction* function, int body_end) {
    if (!function || body_end < 0) return false;
    if (body_end + 3 != function->chunk.code_count) return false;
    const uint8_t* code = function->chunk.code;
    return code[body_end] == OP_CONST && code[body_end + 1] == 255 && code[body_end + 2] == OP_RET;
}

static bool jit_get_int_constant(const ObjFunction* function, uint8_t const_idx, int64_t* out_value) {
    if (!function || !out_value) return false;
    if (const_idx >= function->constants.constant_count) return false;
    Constant c = function->constants.constants[const_idx];
    if (c.type_index != 0) return false;
    *out_value = c.as_int;
    return true;
}

static bool jit_get_bool_constant(const ObjFunction* function, uint8_t const_idx, bool* out_value) {
    if (!function || !out_value) return false;
    if (const_idx >= function->constants.constant_count) return false;
    Constant c = function->constants.constants[const_idx];
    if (c.type_index == 4) {
        *out_value = c.as_int != 0;
        return true;
    }
    if (c.type_index == 0 && (c.as_int == 0 || c.as_int == 1)) {
        *out_value = c.as_int != 0;
        return true;
    }
    return false;
}

static bool jit_try_match_native_local_const_int_binary(ObjFunction* function,
                                                        uint8_t opcode,
                                                        JitSummaryOp op,
                                                        JitFunctionSummary* out_summary) {
    if (!function || !function->chunk.code) return false;
    if (function->is_async || function->capture_count != 0 || function->param_count != 1 ||
        function->local_count != 1) {
        return false;
    }

    const uint8_t* code = function->chunk.code;
    int body_end = function->chunk.code_count;
    if (jit_has_trailing_nil_return(function, body_end - 3)) {
        body_end -= 3;
    }
    if (body_end != 6) return false;
    if (code[0] != opcode || code[5] != OP_RET) return false;
    if (code[1] != 0) return false;

    int64_t rhs_const = 0;
    if (!jit_get_int_constant(function, code[2], &rhs_const)) return false;
    if ((op == JIT_SUMMARY_OP_DIV || op == JIT_SUMMARY_OP_MOD) && rhs_const == 0) return false;

    JitFunctionSummary summary = {0};
    summary.kind = JIT_SUMMARY_KIND_INT_LOCAL_CONST_BINARY;
    summary.op = op;
    summary.slot0 = code[1];
    summary.int_const0 = rhs_const;
    if (out_summary) *out_summary = summary;
    return true;
}

static bool jit_try_match_native_add_local_const(ObjFunction* function,
                                                 JitFunctionSummary* out_summary) {
    return jit_try_match_native_local_const_int_binary(function,
                                                       OP_ADD_LOCAL_CONST_INT,
                                                       JIT_SUMMARY_OP_ADD,
                                                       out_summary);
}

static bool jit_try_match_native_mul_local_const(ObjFunction* function,
                                                 JitFunctionSummary* out_summary) {
    return jit_try_match_native_local_const_int_binary(function,
                                                       OP_MUL_LOCAL_CONST_INT,
                                                       JIT_SUMMARY_OP_MUL,
                                                       out_summary);
}

static bool jit_try_match_native_sub_local_const(ObjFunction* function,
                                                 JitFunctionSummary* out_summary) {
    return jit_try_match_native_local_const_int_binary(function,
                                                       OP_SUB_LOCAL_CONST_INT,
                                                       JIT_SUMMARY_OP_SUB,
                                                       out_summary);
}

static bool jit_try_match_native_div_local_const(ObjFunction* function,
                                                 JitFunctionSummary* out_summary) {
    return jit_try_match_native_local_const_int_binary(function,
                                                       OP_DIV_LOCAL_CONST_INT,
                                                       JIT_SUMMARY_OP_DIV,
                                                       out_summary);
}

static bool jit_try_match_native_mod_local_const(ObjFunction* function,
                                                 JitFunctionSummary* out_summary) {
    return jit_try_match_native_local_const_int_binary(function,
                                                       OP_MOD_LOCAL_CONST_INT,
                                                       JIT_SUMMARY_OP_MOD,
                                                       out_summary);
}

static bool jit_try_match_native_bit_and_local_const(ObjFunction* function,
                                                     JitFunctionSummary* out_summary) {
    return jit_try_match_native_local_const_int_binary(function,
                                                       OP_BIT_AND_LOCAL_CONST_INT,
                                                       JIT_SUMMARY_OP_BIT_AND,
                                                       out_summary);
}

static bool jit_try_match_native_bit_xor_local_const(ObjFunction* function,
                                                     JitFunctionSummary* out_summary) {
    return jit_try_match_native_local_const_int_binary(function,
                                                       OP_BIT_XOR_LOCAL_CONST_INT,
                                                       JIT_SUMMARY_OP_BIT_XOR,
                                                       out_summary);
}

static bool jit_try_match_native_bit_or_local_const(ObjFunction* function,
                                                    JitFunctionSummary* out_summary) {
    return jit_try_match_native_local_const_int_binary(function,
                                                       OP_BIT_OR_LOCAL_CONST_INT,
                                                       JIT_SUMMARY_OP_BIT_OR,
                                                       out_summary);
}

static bool jit_try_match_native_twoarg_locals_int(ObjFunction* function,
                                                   uint8_t opcode,
                                                   JitSummaryOp op,
                                                   JitFunctionSummary* out_summary) {
    if (!function || !function->chunk.code) return false;
    if (function->is_async || function->capture_count != 0 || function->param_count != 2 ||
        function->local_count != 2) {
        return false;
    }

    const uint8_t* code = function->chunk.code;
    int body_end = function->chunk.code_count;
    if (jit_has_trailing_nil_return(function, body_end - 3)) {
        body_end -= 3;
    }
    if (body_end != 6) return false;
    if (code[0] != opcode || code[5] != OP_RET) return false;
    if (code[1] > 1 || code[2] > 1 || code[1] == code[2]) return false;

    JitFunctionSummary summary = {0};
    summary.kind = JIT_SUMMARY_KIND_INT_TWOARG_BINARY;
    summary.op = op;
    summary.slot0 = code[1];
    summary.slot1 = code[2];
    if (out_summary) *out_summary = summary;
    return true;
}

static bool jit_try_match_native_twoarg_locals_bool_cmp(ObjFunction* function,
                                                        uint8_t opcode,
                                                        JitSummaryOp op,
                                                        JitFunctionSummary* out_summary) {
    if (!function || !function->chunk.code) return false;
    if (function->is_async || function->capture_count != 0 || function->param_count != 2 ||
        function->local_count != 2) {
        return false;
    }

    const uint8_t* code = function->chunk.code;
    int body_end = function->chunk.code_count;
    if (jit_has_trailing_nil_return(function, body_end - 3)) {
        body_end -= 3;
    }
    if (body_end != 6) return false;
    if (code[0] != OP_LOAD_LOCAL || code[2] != OP_LOAD_LOCAL || code[4] != opcode ||
        code[5] != OP_RET) {
        return false;
    }
    if (code[1] > 1 || code[3] > 1 || code[1] == code[3]) return false;

    JitFunctionSummary summary = {0};
    summary.kind = JIT_SUMMARY_KIND_BOOL_TWOARG_COMPARE;
    summary.op = op;
    summary.slot0 = code[1];
    summary.slot1 = code[3];
    if (out_summary) *out_summary = summary;
    return true;
}

static bool jit_try_match_native_guard_twoarg_locals_bool_cmp(ObjFunction* function,
                                                              uint8_t opcode,
                                                              JitSummaryOp op,
                                                              JitFunctionSummary* out_summary) {
    if (!function || !function->chunk.code) return false;
    if (function->is_async || function->capture_count != 0 || function->param_count != 2 ||
        function->local_count != 2) {
        return false;
    }

    const uint8_t* code = function->chunk.code;
    int body_end = function->chunk.code_count;
    if (jit_has_trailing_nil_return(function, body_end - 3)) {
        body_end -= 3;
    }
    if (body_end != 14) return false;
    if (code[0] != OP_JUMP_IF_LOCAL_GE_CONST || code[5] != OP_CONST || code[7] != OP_RET ||
        code[8] != OP_LOAD_LOCAL || code[10] != OP_LOAD_LOCAL || code[12] != opcode ||
        code[13] != OP_RET) {
        return false;
    }
    if (code[9] > 1 || code[11] > 1 || code[9] == code[11]) return false;
    if (code[1] != code[9]) return false;

    int16_t offset = (int16_t)(((uint16_t)code[3] << 8) | (uint16_t)code[4]);
    if (offset != 3) return false;

    int64_t guard_const = 0;
    bool guard_result = false;
    if (!jit_get_int_constant(function, code[2], &guard_const)) return false;
    if (!jit_get_bool_constant(function, code[6], &guard_result)) return false;

    JitFunctionSummary summary = {0};
    summary.kind = JIT_SUMMARY_KIND_BOOL_GUARDED_TWOARG_COMPARE;
    summary.op = op;
    summary.slot0 = code[9];
    summary.slot1 = code[11];
    summary.int_const0 = guard_const;
    summary.int_const1 = guard_result ? 1 : 0;
    if (out_summary) *out_summary = summary;
    return true;
}

static bool jit_try_match_native_twoarg_selector_int(ObjFunction* function,
                                                     uint8_t jump_opcode,
                                                     JitSummaryOp op,
                                                     JitFunctionSummary* out_summary) {
    if (!function || !function->chunk.code) return false;
    if (function->is_async || function->capture_count != 0 || function->param_count != 2 ||
        function->local_count != 2) {
        return false;
    }

    const uint8_t* code = function->chunk.code;
    int body_end = function->chunk.code_count;
    if (jit_has_trailing_nil_return(function, body_end - 3)) {
        body_end -= 3;
    }
    if (body_end != 11) return false;
    if (code[0] != jump_opcode || code[5] != OP_LOAD_LOCAL || code[7] != OP_RET ||
        code[8] != OP_LOAD_LOCAL || code[10] != OP_RET) {
        return false;
    }
    if (code[1] != 0 || code[2] != 1) return false;
    if (code[6] > 1 || code[9] > 1 || code[6] == code[9]) return false;

    int16_t offset = (int16_t)(((uint16_t)code[3] << 8) | (uint16_t)code[4]);
    if (offset != 3) return false;

    JitFunctionSummary summary = {0};
    summary.kind = JIT_SUMMARY_KIND_INT_TWOARG_SELECTOR;
    summary.op = op;
    summary.slot0 = code[6];
    summary.slot1 = code[9];
    if (out_summary) *out_summary = summary;
    return true;
}

static bool jit_try_match_native_guard_twoarg_selector_int(ObjFunction* function,
                                                           uint8_t jump_opcode,
                                                           JitSummaryOp op,
                                                           JitFunctionSummary* out_summary) {
    if (!function || !function->chunk.code) return false;
    if (function->is_async || function->capture_count != 0 || function->param_count != 2 ||
        function->local_count != 2) {
        return false;
    }

    const uint8_t* code = function->chunk.code;
    int body_end = function->chunk.code_count;
    if (jit_has_trailing_nil_return(function, body_end - 3)) {
        body_end -= 3;
    }
    if (body_end != 19) return false;
    if (code[0] != OP_JUMP_IF_LOCAL_GE_CONST || code[5] != OP_LOAD_LOCAL || code[7] != OP_RET ||
        code[8] != jump_opcode || code[13] != OP_LOAD_LOCAL || code[15] != OP_RET ||
        code[16] != OP_LOAD_LOCAL || code[18] != OP_RET) {
        return false;
    }
    if (code[1] != 0 || code[6] != 0) return false;
    if (code[9] != 0 || code[10] != 1) return false;
    if (code[14] > 1 || code[17] > 1 || code[14] == code[17]) return false;

    int16_t guard_offset = (int16_t)(((uint16_t)code[3] << 8) | (uint16_t)code[4]);
    int16_t selector_offset = (int16_t)(((uint16_t)code[11] << 8) | (uint16_t)code[12]);
    int64_t guard_const = 0;
    if (guard_offset != 3 || selector_offset != 3) return false;
    if (!jit_get_int_constant(function, code[2], &guard_const)) return false;

    JitFunctionSummary summary = {0};
    summary.kind = JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_SELECTOR;
    summary.op = op;
    summary.slot0 = code[14];
    summary.slot1 = code[17];
    summary.int_const0 = guard_const;
    if (out_summary) *out_summary = summary;
    return true;
}

static bool jit_try_match_native_guard_local_const_int_binary(ObjFunction* function,
                                                              uint8_t opcode,
                                                              JitSummaryOp op,
                                                              JitFunctionSummary* out_summary) {
    if (!function || !function->chunk.code) return false;
    if (function->is_async || function->capture_count != 0 || function->param_count != 1 ||
        function->local_count != 1) {
        return false;
    }

    const uint8_t* code = function->chunk.code;
    int body_end = function->chunk.code_count;
    if (jit_has_trailing_nil_return(function, body_end - 3)) {
        body_end -= 3;
    }
    if (body_end != 14) return false;
    if (code[0] != OP_JUMP_IF_LOCAL_GE_CONST || code[5] != OP_LOAD_LOCAL || code[7] != OP_RET ||
        code[8] != opcode || code[13] != OP_RET) {
        return false;
    }
    if (code[1] != 0 || code[6] != 0 || code[9] != 0) return false;

    int16_t offset = (int16_t)(((uint16_t)code[3] << 8) | (uint16_t)code[4]);
    if (offset != 3) return false;

    int64_t guard_const = 0;
    int64_t rhs_const = 0;
    if (!jit_get_int_constant(function, code[2], &guard_const)) return false;
    if (!jit_get_int_constant(function, code[10], &rhs_const)) return false;
    if ((op == JIT_SUMMARY_OP_DIV || op == JIT_SUMMARY_OP_MOD) && rhs_const == 0) return false;

    JitFunctionSummary summary = {0};
    summary.kind = JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_BINARY;
    summary.op = op;
    summary.slot0 = code[1];
    summary.int_const0 = guard_const;
    summary.int_const1 = rhs_const;
    if (out_summary) *out_summary = summary;
    return true;
}

static bool jit_try_match_native_guard_twoarg_locals_int(ObjFunction* function,
                                                         uint8_t opcode,
                                                         JitSummaryOp op,
                                                         JitFunctionSummary* out_summary) {
    if (!function || !function->chunk.code) return false;
    if (function->is_async || function->capture_count != 0 || function->param_count != 2 ||
        function->local_count != 2) {
        return false;
    }

    const uint8_t* code = function->chunk.code;
    int body_end = function->chunk.code_count;
    if (jit_has_trailing_nil_return(function, body_end - 3)) {
        body_end -= 3;
    }
    if (body_end != 14) return false;
    if (code[0] != OP_JUMP_IF_LOCAL_GE_CONST || code[5] != OP_LOAD_LOCAL || code[7] != OP_RET ||
        code[8] != opcode || code[13] != OP_RET) {
        return false;
    }
    if (code[1] != 0 || code[6] != 0) return false;
    if (code[9] > 1 || code[10] > 1 || code[9] == code[10]) return false;

    int16_t offset = (int16_t)(((uint16_t)code[3] << 8) | (uint16_t)code[4]);
    if (offset != 3) return false;

    int64_t guard_const = 0;
    if (!jit_get_int_constant(function, code[2], &guard_const)) return false;

    JitFunctionSummary summary = {0};
    summary.kind = JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_BINARY;
    summary.op = op;
    summary.slot0 = code[9];
    summary.slot1 = code[10];
    summary.int_const0 = guard_const;
    if (out_summary) *out_summary = summary;
    return true;
}

static bool jit_try_match_native_guard_add(ObjFunction* function,
                                           JitFunctionSummary* out_summary) {
    return jit_try_match_native_guard_local_const_int_binary(function,
                                                             OP_ADD_LOCAL_CONST_INT,
                                                             JIT_SUMMARY_OP_ADD,
                                                             out_summary);
}

static bool jit_try_match_native_guard_mul(ObjFunction* function,
                                           JitFunctionSummary* out_summary) {
    return jit_try_match_native_guard_local_const_int_binary(function,
                                                             OP_MUL_LOCAL_CONST_INT,
                                                             JIT_SUMMARY_OP_MUL,
                                                             out_summary);
}

static bool jit_try_match_native_guard_sub(ObjFunction* function,
                                           JitFunctionSummary* out_summary) {
    return jit_try_match_native_guard_local_const_int_binary(function,
                                                             OP_SUB_LOCAL_CONST_INT,
                                                             JIT_SUMMARY_OP_SUB,
                                                             out_summary);
}

static bool jit_try_match_native_guard_div(ObjFunction* function,
                                           JitFunctionSummary* out_summary) {
    return jit_try_match_native_guard_local_const_int_binary(function,
                                                             OP_DIV_LOCAL_CONST_INT,
                                                             JIT_SUMMARY_OP_DIV,
                                                             out_summary);
}

static bool jit_try_match_native_guard_mod(ObjFunction* function,
                                           JitFunctionSummary* out_summary) {
    return jit_try_match_native_guard_local_const_int_binary(function,
                                                             OP_MOD_LOCAL_CONST_INT,
                                                             JIT_SUMMARY_OP_MOD,
                                                             out_summary);
}

static bool jit_try_match_native_guard_bit_and(ObjFunction* function,
                                               JitFunctionSummary* out_summary) {
    return jit_try_match_native_guard_local_const_int_binary(function,
                                                             OP_BIT_AND_LOCAL_CONST_INT,
                                                             JIT_SUMMARY_OP_BIT_AND,
                                                             out_summary);
}

static bool jit_try_match_native_guard_bit_or(ObjFunction* function,
                                              JitFunctionSummary* out_summary) {
    return jit_try_match_native_guard_local_const_int_binary(function,
                                                             OP_BIT_OR_LOCAL_CONST_INT,
                                                             JIT_SUMMARY_OP_BIT_OR,
                                                             out_summary);
}

static bool jit_try_match_native_guard_bit_xor(ObjFunction* function,
                                               JitFunctionSummary* out_summary) {
    return jit_try_match_native_guard_local_const_int_binary(function,
                                                             OP_BIT_XOR_LOCAL_CONST_INT,
                                                             JIT_SUMMARY_OP_BIT_XOR,
                                                             out_summary);
}

static bool jit_try_recover_arithmetic_summary(ObjFunction* function,
                                               JitFunctionSummary* out_summary) {
    if (jit_try_match_native_add_local_const(function, out_summary)) return true;
    if (jit_try_match_native_mul_local_const(function, out_summary)) return true;
    if (jit_try_match_native_sub_local_const(function, out_summary)) return true;
    if (jit_try_match_native_div_local_const(function, out_summary)) return true;
    if (jit_try_match_native_mod_local_const(function, out_summary)) return true;
    if (jit_try_match_native_bit_and_local_const(function, out_summary)) return true;
    if (jit_try_match_native_bit_or_local_const(function, out_summary)) return true;
    if (jit_try_match_native_bit_xor_local_const(function, out_summary)) return true;
    if (jit_try_match_native_guard_add(function, out_summary)) return true;
    if (jit_try_match_native_guard_mul(function, out_summary)) return true;
    if (jit_try_match_native_guard_sub(function, out_summary)) return true;
    if (jit_try_match_native_guard_div(function, out_summary)) return true;
    if (jit_try_match_native_guard_mod(function, out_summary)) return true;
    if (jit_try_match_native_guard_bit_and(function, out_summary)) return true;
    if (jit_try_match_native_guard_bit_or(function, out_summary)) return true;
    if (jit_try_match_native_guard_bit_xor(function, out_summary)) return true;
    if (jit_try_match_native_twoarg_locals_int(function, OP_ADD_LOCALS_INT, JIT_SUMMARY_OP_ADD, out_summary)) return true;
    if (jit_try_match_native_twoarg_locals_int(function, OP_SUB_LOCALS_INT, JIT_SUMMARY_OP_SUB, out_summary)) return true;
    if (jit_try_match_native_twoarg_locals_int(function, OP_MUL_LOCALS_INT, JIT_SUMMARY_OP_MUL, out_summary)) return true;
    if (jit_try_match_native_twoarg_locals_int(function, OP_BIT_AND_LOCALS_INT, JIT_SUMMARY_OP_BIT_AND, out_summary)) return true;
    if (jit_try_match_native_twoarg_locals_int(function, OP_BIT_OR_LOCALS_INT, JIT_SUMMARY_OP_BIT_OR, out_summary)) return true;
    if (jit_try_match_native_twoarg_locals_int(function, OP_BIT_XOR_LOCALS_INT, JIT_SUMMARY_OP_BIT_XOR, out_summary)) return true;
    if (jit_try_match_native_guard_twoarg_locals_int(function, OP_ADD_LOCALS_INT, JIT_SUMMARY_OP_ADD, out_summary)) return true;
    if (jit_try_match_native_guard_twoarg_locals_int(function, OP_SUB_LOCALS_INT, JIT_SUMMARY_OP_SUB, out_summary)) return true;
    if (jit_try_match_native_guard_twoarg_locals_int(function, OP_MUL_LOCALS_INT, JIT_SUMMARY_OP_MUL, out_summary)) return true;
    if (jit_try_match_native_guard_twoarg_locals_int(function, OP_BIT_AND_LOCALS_INT, JIT_SUMMARY_OP_BIT_AND, out_summary)) return true;
    if (jit_try_match_native_guard_twoarg_locals_int(function, OP_BIT_OR_LOCALS_INT, JIT_SUMMARY_OP_BIT_OR, out_summary)) return true;
    if (jit_try_match_native_guard_twoarg_locals_int(function, OP_BIT_XOR_LOCALS_INT, JIT_SUMMARY_OP_BIT_XOR, out_summary)) return true;
    return false;
}

static bool jit_try_recover_exact_summary(ObjFunction* function,
                                          JitFunctionSummary* out_summary) {
    uint8_t native_family_mask = function ? function->jit_profile.native_family_mask
                                          : JIT_PROFILE_NATIVE_FAMILY_NONE;

    if ((native_family_mask & JIT_PROFILE_NATIVE_FAMILY_ARITHMETIC) != 0 &&
        jit_try_recover_arithmetic_summary(function, out_summary)) {
        return true;
    }
    if ((native_family_mask & JIT_PROFILE_NATIVE_FAMILY_COMPARE) != 0) {
        if (jit_try_match_native_twoarg_locals_bool_cmp(function, OP_LT, JIT_SUMMARY_OP_LT, out_summary)) return true;
        if (jit_try_match_native_twoarg_locals_bool_cmp(function, OP_LE, JIT_SUMMARY_OP_LE, out_summary)) return true;
        if (jit_try_match_native_twoarg_locals_bool_cmp(function, OP_EQ, JIT_SUMMARY_OP_EQ, out_summary)) return true;
        if (jit_try_match_native_twoarg_locals_bool_cmp(function, OP_NE, JIT_SUMMARY_OP_NE, out_summary)) return true;
        if (jit_try_match_native_twoarg_locals_bool_cmp(function, OP_GT, JIT_SUMMARY_OP_GT, out_summary)) return true;
        if (jit_try_match_native_twoarg_locals_bool_cmp(function, OP_GE, JIT_SUMMARY_OP_GE, out_summary)) return true;
        if (jit_try_match_native_guard_twoarg_locals_bool_cmp(function, OP_LT, JIT_SUMMARY_OP_LT, out_summary)) return true;
        if (jit_try_match_native_guard_twoarg_locals_bool_cmp(function, OP_LE, JIT_SUMMARY_OP_LE, out_summary)) return true;
        if (jit_try_match_native_guard_twoarg_locals_bool_cmp(function, OP_EQ, JIT_SUMMARY_OP_EQ, out_summary)) return true;
        if (jit_try_match_native_guard_twoarg_locals_bool_cmp(function, OP_NE, JIT_SUMMARY_OP_NE, out_summary)) return true;
        if (jit_try_match_native_guard_twoarg_locals_bool_cmp(function, OP_GT, JIT_SUMMARY_OP_GT, out_summary)) return true;
        if (jit_try_match_native_guard_twoarg_locals_bool_cmp(function, OP_GE, JIT_SUMMARY_OP_GE, out_summary)) return true;
    }
    if ((native_family_mask & JIT_PROFILE_NATIVE_FAMILY_SELECTOR) != 0) {
        if (jit_try_match_native_twoarg_selector_int(function, OP_JUMP_IF_LOCAL_GE, JIT_SUMMARY_OP_LT, out_summary)) return true;
        if (jit_try_match_native_twoarg_selector_int(function, OP_JUMP_IF_LOCAL_GT, JIT_SUMMARY_OP_LE, out_summary)) return true;
        if (jit_try_match_native_twoarg_selector_int(function, OP_JUMP_IF_LOCAL_LE, JIT_SUMMARY_OP_GT, out_summary)) return true;
        if (jit_try_match_native_twoarg_selector_int(function, OP_JUMP_IF_LOCAL_LT, JIT_SUMMARY_OP_GE, out_summary)) return true;
        if (jit_try_match_native_guard_twoarg_selector_int(function, OP_JUMP_IF_LOCAL_GE, JIT_SUMMARY_OP_LT, out_summary)) return true;
        if (jit_try_match_native_guard_twoarg_selector_int(function, OP_JUMP_IF_LOCAL_GT, JIT_SUMMARY_OP_LE, out_summary)) return true;
        if (jit_try_match_native_guard_twoarg_selector_int(function, OP_JUMP_IF_LOCAL_LE, JIT_SUMMARY_OP_GT, out_summary)) return true;
        if (jit_try_match_native_guard_twoarg_selector_int(function, OP_JUMP_IF_LOCAL_LT, JIT_SUMMARY_OP_GE, out_summary)) return true;
    }
    return false;
}

static bool jit_try_compile_native_function(ObjFunction* function) {
    if (!function) return false;
    function->jit_compiled_plan.kind = JIT_COMPILED_KIND_NONE;
    function->jit_compiled_plan.op = JIT_SUMMARY_OP_NONE;
    function->jit_compiled_plan.flags = JIT_PLAN_FLAG_NONE;
    function->jit_compiled_entry = NULL;
    if ((function->jit_profile.support_mask & JIT_PROFILE_SUPPORT_NATIVE_SUMMARY) == 0) {
        return false;
    }
    bool has_compiler_summary = function->jit_profile.summary.kind != JIT_SUMMARY_KIND_NONE;
    bool has_compiler_hint = function->jit_hint_plan.kind != JIT_COMPILED_KIND_NONE;
    if (jit_try_apply_compiler_summary(function)) return true;
    if (jit_try_apply_compiler_hint(function)) return true;
    if (has_compiler_summary || has_compiler_hint) {
        return false;
    }
    JitFunctionSummary recovered_summary = {0};
    if (!jit_try_recover_exact_summary(function, &recovered_summary)) {
        return false;
    }
    return jit_try_apply_recovered_summary(function, &recovered_summary);
}

static void jit_profile_entries_append_unique(JitProfileEntry** io_entries,
                                              int* io_count,
                                              int* io_capacity,
                                              ObjFunction* function) {
    if (!io_entries || !io_count || !io_capacity || !function) return;
    if (function->jit_entry_count == 0) return;

    for (int i = 0; i < *io_count; i++) {
        if ((*io_entries)[i].function == function) {
            return;
        }
    }

    if (*io_count >= *io_capacity) {
        int next_capacity = *io_capacity > 0 ? *io_capacity * 2 : 8;
        *io_entries = (JitProfileEntry*)safe_realloc(*io_entries,
                                                     (size_t)next_capacity * sizeof(JitProfileEntry));
        *io_capacity = next_capacity;
    }

    JitProfileEntry* added = &((*io_entries)[(*io_count)++]);
    added->function = function;
    added->entry_count = function->jit_entry_count;
    added->hot = function->jit_hot;
    added->state = function->jit_state;
    added->reason = function->jit_reason;
    added->compile_attempts = function->jit_compile_attempts;
    added->compiled_calls = function->jit_compiled_call_count;
}

static void jit_enqueue_hot_function(VM* vm, ObjFunction* function) {
    if (!vm || !function || function->jit_state != JIT_FUNC_STATE_COLD) return;

    if (vm->jit_work_queue_count >= vm->jit_work_queue_capacity) {
        int next_capacity = vm->jit_work_queue_capacity > 0 ? vm->jit_work_queue_capacity * 2 : 8;
        vm->jit_work_queue = (ObjFunction**)safe_realloc(vm->jit_work_queue,
                                                         (size_t)next_capacity * sizeof(ObjFunction*));
        vm->jit_work_queue_capacity = next_capacity;
    }

    vm->jit_work_queue[vm->jit_work_queue_count++] = function;
    function->jit_state = JIT_FUNC_STATE_QUEUED;
    function->jit_reason = JIT_REASON_QUEUED_HOT;
}

static ObjFunction* jit_dequeue_hot_function(VM* vm) {
    if (!vm || vm->jit_work_queue_count <= 0 || !vm->jit_work_queue) return NULL;

    ObjFunction* function = vm->jit_work_queue[0];
    if (vm->jit_work_queue_count > 1) {
        memmove(&vm->jit_work_queue[0],
                &vm->jit_work_queue[1],
                (size_t)(vm->jit_work_queue_count - 1) * sizeof(vm->jit_work_queue[0]));
    }
    vm->jit_work_queue_count--;
    return function;
}

static bool jit_stub_compile_function(VM* vm, ObjFunction* function) {
    (void)vm;
    if (!function) return false;
    if ((function->jit_profile.support_mask & JIT_PROFILE_SUPPORT_STUB) == 0) return false;
    function->jit_compiled_plan.kind = JIT_COMPILED_KIND_STUB;
    function->jit_compiled_entry = (void*)&vm_jit_stub_compiled_entry;
    return true;
}

void jit_vm_init(VM* vm) {
    if (!vm) return;
    vm->jit_profile_enabled = false;
    vm->jit_auto_compile_enabled = false;
    vm->jit_hot_threshold = JIT_DEFAULT_HOT_THRESHOLD;
    vm->jit_work_queue = NULL;
    vm->jit_work_queue_count = 0;
    vm->jit_work_queue_capacity = 0;
}

void jit_vm_free(VM* vm) {
    if (!vm) return;
    if (vm->jit_work_queue) {
        free(vm->jit_work_queue);
        vm->jit_work_queue = NULL;
    }
    vm->jit_work_queue_count = 0;
    vm->jit_work_queue_capacity = 0;
}

void jit_set_profile_enabled(VM* vm, bool enabled) {
    if (!vm) return;
    vm->jit_profile_enabled = enabled;
}

bool jit_is_profile_enabled(const VM* vm) {
    return vm && vm->jit_profile_enabled;
}

void jit_set_auto_compile_enabled(VM* vm, bool enabled) {
    if (!vm) return;
    vm->jit_auto_compile_enabled = enabled;
}

bool jit_is_auto_compile_enabled(const VM* vm) {
    return vm && vm->jit_auto_compile_enabled;
}

void jit_set_hot_threshold(VM* vm, uint64_t threshold) {
    if (!vm) return;
    if (threshold == 0) threshold = 1;
    vm->jit_hot_threshold = threshold;
}

uint64_t jit_get_hot_threshold(const VM* vm) {
    if (!vm) return JIT_DEFAULT_HOT_THRESHOLD;
    return vm->jit_hot_threshold > 0 ? vm->jit_hot_threshold : JIT_DEFAULT_HOT_THRESHOLD;
}

bool jit_function_has_compiled_entry(const ObjFunction* function) {
    return function && function->jit_compiled_entry != NULL;
}

void jit_record_function_entry(VM* vm, ObjFunction* function) {
    if (!vm || !function) return;

    function->jit_entry_count++;
    if (function->jit_entry_count >= jit_get_hot_threshold(vm)) {
        function->jit_hot = true;
        bool was_cold = function->jit_state == JIT_FUNC_STATE_COLD;
        jit_enqueue_hot_function(vm, function);
        if (was_cold && jit_is_auto_compile_enabled(vm)) {
            jit_drain_work_queue(vm, 1);
        }
    }
}

int jit_drain_work_queue(VM* vm, int max_items) {
    if (!vm) return 0;

    int processed = 0;
    while (vm->jit_work_queue_count > 0 && (max_items <= 0 || processed < max_items)) {
        ObjFunction* function = jit_dequeue_hot_function(vm);
        if (!function) continue;
        if (function->jit_state != JIT_FUNC_STATE_QUEUED) continue;

        function->jit_state = JIT_FUNC_STATE_COMPILING;
        function->jit_reason = JIT_REASON_NONE;
        function->jit_compile_attempts++;
        if (jit_try_compile_native_function(function)) {
            function->jit_state = JIT_FUNC_STATE_COMPILED_NATIVE;
            function->jit_reason = jit_plan_matches_compiler_metadata(function, &function->jit_compiled_plan)
                                       ? JIT_REASON_NATIVE_HINT
                                       : JIT_REASON_NATIVE_EXACT;
        } else if (jit_stub_compile_function(vm, function)) {
            function->jit_state = JIT_FUNC_STATE_COMPILED_STUB;
            function->jit_reason = JIT_REASON_STUB_FALLBACK;
        } else {
            function->jit_state = JIT_FUNC_STATE_FAILED;
            function->jit_reason = (function->jit_profile.flags & JIT_PROFILE_FLAG_ASYNC) != 0
                                       ? JIT_REASON_UNSUPPORTED_ASYNC
                                       : JIT_REASON_UNSUPPORTED_SHAPE;
        }
        processed++;
    }

    return processed;
}

int jit_collect_profile_entries(ObjFunction* init_function,
                                ObjFunction* main_function,
                                ObjFunction** functions,
                                int function_count,
                                JitProfileEntry** out_entries) {
    if (out_entries) *out_entries = NULL;
    if (!out_entries) return 0;

    JitProfileEntry* entries = NULL;
    int count = 0;
    int capacity = 0;

    jit_profile_entries_append_unique(&entries, &count, &capacity, init_function);
    jit_profile_entries_append_unique(&entries, &count, &capacity, main_function);
    if (functions) {
        for (int i = 0; i < function_count; i++) {
            jit_profile_entries_append_unique(&entries, &count, &capacity, functions[i]);
        }
    }

    if (count > 1) {
        qsort(entries, (size_t)count, sizeof(entries[0]), jit_profile_entry_cmp_desc);
    }

    *out_entries = entries;
    return count;
}

void jit_free_profile_entries(JitProfileEntry* entries) {
    if (entries) free(entries);
}

void jit_dump_profile(FILE* out,
                      const VM* vm,
                      ObjFunction* init_function,
                      ObjFunction* main_function,
                      ObjFunction** functions,
                      int function_count) {
    if (!out || !vm) return;

    JitProfileEntry* entries = NULL;
    int count = jit_collect_profile_entries(init_function,
                                            main_function,
                                            functions,
                                            function_count,
                                            &entries);
    fprintf(out, "\nJIT hotness profile (threshold=%" PRIu64 "):\n", jit_get_hot_threshold(vm));
    if (count <= 0) {
        fprintf(out, "  <no entered functions>\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        ObjFunction* function = entries[i].function;
        const char* name =
            (function && function->name && function->name[0] != '\0') ? function->name : "<anon>";
        const char* source =
            (function && function->source_file && function->source_file[0] != '\0')
                ? function->source_file
                : "<unknown>";
        fprintf(out,
                "  %-24s entries=%" PRIu64 " hot=%s state=%s reason=%s support=%s family=%s attempts=%u compiledCalls=%" PRIu64 " async=%s source=%s\n",
                name,
                entries[i].entry_count,
                entries[i].hot ? "yes" : "no",
                jit_function_state_name(entries[i].state),
                jit_function_reason_name(entries[i].reason),
                function ? jit_profile_support_name(function->jit_profile.support_mask) : "none",
                function ? jit_profile_native_family_name(function->jit_profile.native_family_mask) : "none",
                entries[i].compile_attempts,
                entries[i].compiled_calls,
                (function && function->is_async) ? "yes" : "no",
                source);
    }

    jit_free_profile_entries(entries);
}

int jit_get_work_queue_count(const VM* vm) {
    return vm ? vm->jit_work_queue_count : 0;
}

ObjFunction* jit_get_work_queue_function(const VM* vm, int index) {
    if (!vm || index < 0 || index >= vm->jit_work_queue_count || !vm->jit_work_queue) return NULL;
    return vm->jit_work_queue[index];
}

void jit_dump_work_queue(FILE* out, const VM* vm) {
    if (!out || !vm) return;

    fprintf(out, "\nJIT work queue (threshold=%" PRIu64 "):\n", jit_get_hot_threshold(vm));
    if (vm->jit_work_queue_count <= 0 || !vm->jit_work_queue) {
        fprintf(out, "  <no queued functions>\n");
        return;
    }

    for (int i = 0; i < vm->jit_work_queue_count; i++) {
        ObjFunction* function = vm->jit_work_queue[i];
        const char* name =
            (function && function->name && function->name[0] != '\0') ? function->name : "<anon>";
        const char* source =
            (function && function->source_file && function->source_file[0] != '\0')
                ? function->source_file
                : "<unknown>";
        fprintf(out,
                "  %-24s entries=%" PRIu64 " hot=%s state=%s reason=%s support=%s family=%s attempts=%u compiledCalls=%" PRIu64 " async=%s source=%s\n",
                name,
                function ? function->jit_entry_count : 0,
                (function && function->jit_hot) ? "yes" : "no",
                function ? jit_function_state_name(function->jit_state) : "unknown",
                function ? jit_function_reason_name(function->jit_reason) : "unknown",
                function ? jit_profile_support_name(function->jit_profile.support_mask) : "none",
                function ? jit_profile_native_family_name(function->jit_profile.native_family_mask) : "none",
                function ? function->jit_compile_attempts : 0,
                function ? function->jit_compiled_call_count : 0,
                (function && function->is_async) ? "yes" : "no",
                source);
    }
}
