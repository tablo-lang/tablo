#include "compiler.h"
#include "safe_alloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

typedef struct MatchArmJumpList MatchArmJumpList;
typedef struct IRCseCandidate IRCseCandidate;

// Forward declarations
static void compile_expression(Compiler* comp, Expr* expr);
static void compile_map_literal(Compiler* comp, Expr* expr);
static void compile_set_literal(Compiler* comp, Expr* expr);
static void compile_record_literal(Compiler* comp, Expr* expr);
static void compile_field_access(Compiler* comp, Expr* expr);
static void compile_tuple_literal(Compiler* comp, Expr* expr);
static void compile_tuple_access(Compiler* comp, Expr* expr);
static void compile_func_literal(Compiler* comp, Expr* expr);
static void compile_type_test(Compiler* comp, Expr* expr);
static void compile_if_expression(Compiler* comp, Expr* expr);
static void compile_match_expression(Compiler* comp, Expr* expr);
static void compile_block_expression(Compiler* comp, Expr* expr);
static void compile_enum_decl(Compiler* comp, Stmt* stmt);
static void compile_match(Compiler* comp, Stmt* stmt);
static bool compile_structural_match_pattern(Compiler* comp,
                                             Expr* pattern_expr,
                                             int subject_slot,
                                             bool allow_binding,
                                             MatchArmJumpList* fail_jump_list,
                                             int line);
static void compile_statement(Compiler* comp, Stmt* stmt);
static void compile_block(Compiler* comp, Stmt* stmt);
static void compile_var_decl(Compiler* comp, Stmt* stmt);
static void compile_var_tuple_decl(Compiler* comp, Stmt* stmt);
static void compile_for_range(Compiler* comp, Stmt* stmt);
static void superinstruction_optimize(Chunk* chunk);
static bool stmt_contains_defer(Stmt* stmt);
static void compiler_try_assign_jit_hint(ObjFunction* func,
                                         Type* return_type,
                                         char** params,
                                         Type** param_types,
                                         int param_count,
                                         Stmt* body);
static int add_local(Compiler* comp, const char* name, int line);
static int add_local_anon(Compiler* comp, int line);
static int emit_byte(Compiler* comp, uint8_t byte, int line);
static int emit_byte2(Compiler* comp, uint8_t byte1, uint8_t byte2, int line);
static void emit_constant(Compiler* comp, Constant constant, int line);
static int emit_jump(Compiler* comp, uint8_t instruction, int line);
static void patch_jump(Compiler* comp, int offset);
static void compiler_set_error(Compiler* comp, const char* message, const char* file, int line, int column);
static char* compiler_enum_member_symbol_name(const char* enum_name, const char* member_name);
static int make_constant_string(Compiler* comp, const char* value, int line);

typedef struct {
    Stmt** statements;
    int stmt_count;
    int stmt_capacity;
} IRStatementList;

typedef enum {
    IR_BLOCK_TERM_END = 0,
    IR_BLOCK_TERM_BARRIER,
    IR_BLOCK_TERM_BRANCH,
    IR_BLOCK_TERM_MATCH,
    IR_BLOCK_TERM_LOOP,
    IR_BLOCK_TERM_RETURN,
    IR_BLOCK_TERM_BREAK,
    IR_BLOCK_TERM_CONTINUE
} IRBlockTerminatorKind;

typedef struct {
    Stmt** statements;
    int statement_count;
    int start_index;
    int end_index;
    IRBlockTerminatorKind terminator_kind;
    int* successors;
    int successor_count;
    int successor_capacity;
    int* predecessors;
    int predecessor_count;
    int predecessor_capacity;
} IRBasicBlock;

typedef struct {
    IRBasicBlock* blocks;
    int count;
    int capacity;
} IRBasicBlockList;

typedef struct {
    int* block_indices;
    int count;
    int capacity;
} IRBlockExitList;

typedef struct {
    int entry_block;
    IRBlockExitList fallthrough_exits;
    IRBlockExitList break_exits;
    IRBlockExitList continue_exits;
} IRStructuredBlockResult;

typedef struct {
    char* target_name;
    char* source_name;
} IRCopyAlias;

typedef struct {
    IRCseCandidate* candidates;
    int candidate_count;
    int candidate_capacity;
    IRCopyAlias* aliases;
    int alias_count;
    int alias_capacity;
} IRCseFlowState;

typedef struct {
    IRCopyAlias* aliases;
    int alias_count;
    int alias_capacity;
} IRCopyFlowState;

typedef struct {
    Stmt* enum_decl;
    int member_index;
    Expr** payload_values;
    int payload_count;
} IRConstEnumValue;

struct MatchArmJumpList {
    int* jumps;
    int count;
    int capacity;
};

static void ir_statement_list_init(IRStatementList* ir);
static void ir_statement_list_free(IRStatementList* ir);
static void ir_statement_list_append(IRStatementList* ir, Stmt* stmt);
static void ir_basic_block_list_init(IRBasicBlockList* blocks);
static void ir_basic_block_list_free(IRBasicBlockList* blocks);
static void ir_block_exit_list_init(IRBlockExitList* exits);
static void ir_block_exit_list_free(IRBlockExitList* exits);
static void ir_block_exit_list_append(IRBlockExitList* exits, int block_index);
static void ir_block_exit_list_append_all(IRBlockExitList* dst, const IRBlockExitList* src);
static void ir_structured_block_result_init(IRStructuredBlockResult* result);
static void ir_structured_block_result_free(IRStructuredBlockResult* result);
static void ir_basic_block_edge_list_append(int** io_edges,
                                            int* io_count,
                                            int* io_capacity,
                                            int edge_index);
static void ir_basic_block_list_append(IRBasicBlockList* blocks,
                                       Stmt** statements,
                                       int statement_count,
                                       int start_index,
                                       int end_index,
                                       IRBlockTerminatorKind terminator_kind);
static void ir_basic_block_list_add_edge(IRBasicBlockList* blocks, int from_index, int to_index);
static void ir_worklist_enqueue(int** io_worklist,
                                int* io_tail,
                                int* io_capacity,
                                bool* queued,
                                int block_index);
static bool ir_block_terminator_may_fallthrough(IRBlockTerminatorKind terminator_kind);
static IRBlockTerminatorKind ir_stmt_basic_block_terminator_kind(Stmt* stmt);
static void ir_stmt_as_statement_view(Stmt* stmt,
                                      IRStatementList* view,
                                      Stmt** single_stmt_storage);
static IRStructuredBlockResult ir_build_basic_blocks_for_range(IRBasicBlockList* blocks,
                                                               Stmt** statements,
                                                               int statement_count,
                                                               int start_index,
                                                               int end_index);
static IRBasicBlockList ir_build_basic_blocks(IRStatementList* ir);
static bool ir_stmt_guarantees_termination(Stmt* stmt);
static bool ir_stmt_guarantees_function_return(Stmt* stmt);
static bool ir_stmt_contains_loop_control(Stmt* stmt);
static void ir_apply_local_cse(IRStatementList* ir);
static void ir_apply_local_copy_propagation(IRStatementList* ir);
static void ir_apply_local_dead_store_elimination(Compiler* comp, IRStatementList* ir, bool allow_dead_store_elimination);
static void ir_cse_flow_state_init(IRCseFlowState* state);
static void ir_cse_flow_state_free(IRCseFlowState* state);
static void ir_cse_flow_state_replace_with_clone(IRCseFlowState* dst,
                                                 const IRCseFlowState* src);
static bool ir_cse_flow_state_equals(const IRCseFlowState* a, const IRCseFlowState* b);
static void ir_copy_flow_state_init(IRCopyFlowState* state);
static void ir_copy_flow_state_free(IRCopyFlowState* state);
static void ir_copy_flow_state_replace_with_clone(IRCopyFlowState* dst,
                                                  const IRCopyFlowState* src);
static bool ir_copy_flow_state_equals(const IRCopyFlowState* a, const IRCopyFlowState* b);
static bool ir_cse_merge_block_predecessors_seeded(const IRBasicBlockList* blocks,
                                                   const IRCseFlowState* out_states,
                                                   const bool* out_reachable,
                                                   int block_index,
                                                   int entry_block_index,
                                                   const IRCseFlowState* entry_state,
                                                   IRCseFlowState* merged_in);
static bool ir_copy_merge_block_predecessors_seeded(const IRBasicBlockList* blocks,
                                                    const IRCopyFlowState* out_states,
                                                    const bool* out_reachable,
                                                    int block_index,
                                                    int entry_block_index,
                                                    const IRCopyFlowState* entry_state,
                                                    IRCopyFlowState* merged_in);
static void ir_clone_statement_range(IRStatementList* dst,
                                     Stmt** statements,
                                     int statement_count,
                                     int start_index,
                                     int end_index);
static void ir_free_cloned_statement_range(IRStatementList* ir);
static void ir_copy_aliases_invalidate_for_stmt_fallthrough(Stmt* stmt,
                                                            IRCopyAlias* aliases,
                                                            int* alias_count);
static void ir_apply_local_cse_range(IRStatementList* ir,
                                     int start_index,
                                     int end_index,
                                     IRCseCandidate** io_candidates,
                                     int* io_candidate_count,
                                     int* io_candidate_capacity,
                                     IRCopyAlias** io_aliases,
                                     int* io_alias_count,
                                     int* io_alias_capacity,
                                     bool preserve_outgoing_state,
                                     bool structured_cfg_exit);
static void ir_simulate_local_cse_block(const IRBasicBlock* block,
                                        const IRCseFlowState* in_state,
                                        IRCseFlowState* out_state);
static void ir_apply_local_copy_propagation_range(IRStatementList* ir,
                                                  int start_index,
                                                  int end_index,
                                                  IRCopyAlias** io_aliases,
                                                  int* io_alias_count,
                                                  int* io_alias_capacity,
                                                  bool preserve_outgoing_aliases,
                                                  bool structured_cfg_exit);
static void ir_simulate_local_copy_block(const IRBasicBlock* block,
                                         const IRCopyFlowState* in_state,
                                         IRCopyFlowState* out_state);
static void ir_apply_local_cse_stmt(Stmt* stmt,
                                    IRCseCandidate** io_candidates,
                                    int* io_candidate_count,
                                    int* io_candidate_capacity,
                                    IRCopyAlias** io_aliases,
                                    int* io_alias_count,
                                    int* io_alias_capacity,
                                    bool preserve_outgoing_state,
                                    bool structured_cfg_exit);
static void ir_apply_local_copy_propagation_stmt(Stmt* stmt,
                                                 IRCopyAlias** io_aliases,
                                                 int* io_alias_count,
                                                 int* io_alias_capacity,
                                                 bool preserve_outgoing_aliases,
                                                 bool structured_cfg_exit);
static Expr* ir_try_fold_constant_expr(Compiler* comp, Expr* expr);
static bool ir_try_eval_expr_int(Compiler* comp, Expr* expr, int64_t* out_value);
static bool ir_try_eval_iterable_empty(Expr* expr, bool* out_is_empty);
static void ir_const_enum_value_init(IRConstEnumValue* value);
static void ir_const_enum_value_free(IRConstEnumValue* value);
static bool ir_try_eval_enum_member_tag(Compiler* comp, Expr* expr, Stmt** out_enum_decl, int* out_member_index);
static bool ir_try_eval_const_enum_value(Compiler* comp, Expr* expr, IRConstEnumValue* out_value);
static bool ir_enum_decls_equal(Stmt* a, Stmt* b);
static bool ir_const_enum_values_equal(const IRConstEnumValue* a, const IRConstEnumValue* b);
static bool ir_const_enum_value_as_scalar_int(const IRConstEnumValue* value, int64_t* out_value);
static bool ir_try_eval_condition_bool(Compiler* comp, Expr* condition, bool* out_value);
static void ir_lower_statement_dce(Compiler* comp, Stmt* stmt, IRStatementList* out);
static IRStatementList ir_lower_statements_dce(Compiler* comp,
                                               Stmt** statements,
                                               int stmt_count,
                                               bool allow_dead_store_elimination);
static void compile_statement_list_with_ir(Compiler* comp,
                                           Stmt** statements,
                                           int stmt_count,
                                           bool allow_dead_store_elimination);
static void ir_copy_aliases_remove_at(IRCopyAlias* aliases, int* count, int index);
static void ir_copy_aliases_clear(IRCopyAlias* aliases, int* count);
static int ir_copy_aliases_find_target(const IRCopyAlias* aliases, int count, const char* target_name);
static const char* ir_copy_aliases_resolve(const IRCopyAlias* aliases, int count, const char* name);
static bool ir_copy_aliases_would_cycle(const IRCopyAlias* aliases,
                                        int count,
                                        const char* target_name,
                                        const char* source_name);
static void ir_copy_aliases_invalidate_by_name(IRCopyAlias* aliases, int* count, const char* name);
static void ir_copy_aliases_set(IRCopyAlias** io_aliases,
                                int* io_count,
                                int* io_capacity,
                                const char* target_name,
                                const char* source_name);
static void ir_copy_aliases_replace_with_clone(IRCopyAlias** io_aliases,
                                               int* count,
                                               int* capacity,
                                               const IRCopyAlias* source,
                                               int source_count);
static bool ir_copy_aliases_sets_equal(const IRCopyAlias* a,
                                       int a_count,
                                       const IRCopyAlias* b,
                                       int b_count);
static void ir_copy_aliases_intersect_in_place(IRCopyAlias* aliases,
                                               int* count,
                                               const IRCopyAlias* other,
                                               int other_count);
static IRCopyAlias* ir_copy_aliases_clone_array(const IRCopyAlias* aliases, int count);
static void ir_rewrite_expr_with_aliases(Expr* expr, const IRCopyAlias* aliases, int alias_count);
static void ir_copy_aliases_invalidate_for_stmt_fallthrough(Stmt* stmt,
                                                            IRCopyAlias* aliases,
                                                            int* alias_count);

static Stmt* find_record_decl(Compiler* comp, const char* name) {
    if (!comp || !name) return NULL;
    for (int i = 0; i < comp->record_decl_count; i++) {
        Stmt* stmt = comp->record_decls[i];
        if (stmt && stmt->kind == STMT_RECORD_DECL && stmt->record_decl.name &&
            strcmp(stmt->record_decl.name, name) == 0) {
            return stmt;
        }
    }
    return NULL;
}

static Stmt* find_function_decl(Compiler* comp, const char* name) {
    if (!comp || !name) return NULL;
    for (int i = 0; i < comp->function_decl_count; i++) {
        Stmt* stmt = comp->function_decls[i];
        if (stmt && stmt->kind == STMT_FUNC_DECL && stmt->func_decl.name &&
            strcmp(stmt->func_decl.name, name) == 0) {
            return stmt;
        }
    }
    return NULL;
}

static bool find_enum_member_decl(Compiler* comp,
                                  const char* symbol_name,
                                  Stmt** out_enum_decl,
                                  int* out_member_index) {
    if (out_enum_decl) *out_enum_decl = NULL;
    if (out_member_index) *out_member_index = -1;
    if (!comp || !symbol_name || !comp->enum_decls) return false;

    for (int i = 0; i < comp->enum_decl_count; i++) {
        Stmt* enum_stmt = comp->enum_decls[i];
        if (!enum_stmt || enum_stmt->kind != STMT_ENUM_DECL || !enum_stmt->enum_decl.name) {
            continue;
        }

        size_t enum_len = strlen(enum_stmt->enum_decl.name);
        if (strncmp(symbol_name, enum_stmt->enum_decl.name, enum_len) != 0) continue;
        if (symbol_name[enum_len] != '_') continue;

        const char* member_name = symbol_name + enum_len + 1;
        if (!member_name || member_name[0] == '\0') continue;

        for (int j = 0; j < enum_stmt->enum_decl.member_count; j++) {
            const char* declared_member = enum_stmt->enum_decl.member_names
                ? enum_stmt->enum_decl.member_names[j]
                : NULL;
            if (!declared_member) continue;
            if (strcmp(member_name, declared_member) == 0) {
                if (out_enum_decl) *out_enum_decl = enum_stmt;
                if (out_member_index) *out_member_index = j;
                return true;
            }
        }
    }

    return false;
}

static char* match_pattern_call_symbol_name(const Expr* pattern_expr) {
    if (!pattern_expr || pattern_expr->kind != EXPR_CALL || !pattern_expr->call.callee) {
        return NULL;
    }

    Expr* callee = pattern_expr->call.callee;
    if (callee->kind == EXPR_IDENTIFIER && callee->identifier) {
        return safe_strdup(callee->identifier);
    }

    if (callee->kind == EXPR_FIELD_ACCESS &&
        callee->field_access.object &&
        callee->field_access.object->kind == EXPR_IDENTIFIER &&
        callee->field_access.object->identifier &&
        callee->field_access.field_name) {
        return compiler_enum_member_symbol_name(callee->field_access.object->identifier,
                                                callee->field_access.field_name);
    }

    return NULL;
}

static bool emit_enum_payload_value(Compiler* comp,
                                    Stmt* enum_stmt,
                                    int member_index,
                                    Expr** payload_args,
                                    int payload_arg_count,
                                    int line,
                                    const char* file,
                                    int column) {
    if (!comp || !enum_stmt || enum_stmt->kind != STMT_ENUM_DECL ||
        !enum_stmt->enum_decl.has_payload_members) {
        return false;
    }
    if (member_index < 0 || member_index >= enum_stmt->enum_decl.member_count) {
        return false;
    }

    int expected_payload_count = (enum_stmt->enum_decl.member_payload_counts &&
                                  member_index < enum_stmt->enum_decl.member_count)
                                     ? enum_stmt->enum_decl.member_payload_counts[member_index]
                                     : 0;
    if (payload_arg_count != expected_payload_count) {
        char message[320];
        snprintf(message,
                 sizeof(message),
                 "Wrong number of arguments for enum constructor '%s.%s': expected %d, got %d",
                 enum_stmt->enum_decl.name ? enum_stmt->enum_decl.name : "<enum>",
                 (enum_stmt->enum_decl.member_names && enum_stmt->enum_decl.member_names[member_index])
                     ? enum_stmt->enum_decl.member_names[member_index]
                     : "<member>",
                 expected_payload_count,
                 payload_arg_count);
        compiler_set_error(comp, message, file ? file : comp->file, line, column);
        return true;
    }

    int tuple_arity = expected_payload_count + 1; // tag + payload elements
    if (tuple_arity > 255) {
        compiler_set_error(comp,
                           "Enum payload constructor exceeds tuple arity limit (max 254 payload elements)",
                           file ? file : comp->file,
                           line,
                           column);
        return true;
    }

    emit_byte(comp, OP_TUPLE_NEW, line);
    emit_byte(comp, (uint8_t)tuple_arity, line);

    int64_t tag_value = (enum_stmt->enum_decl.member_values &&
                         member_index < enum_stmt->enum_decl.member_count)
                            ? enum_stmt->enum_decl.member_values[member_index]
                            : 0;
    emit_constant(comp, (Constant){ .as_int = tag_value, .type_index = 0 }, line);
    emit_byte(comp, OP_TUPLE_SET, line);
    emit_byte(comp, 0, line);

    for (int i = 0; i < expected_payload_count; i++) {
        compile_expression(comp, payload_args[i]);
        emit_byte(comp, OP_TUPLE_SET, line);
        emit_byte(comp, (uint8_t)(i + 1), line);
    }

    return true;
}

static int resolve_record_field_index(Compiler* comp, Expr* object, const char* field_name) {
    if (!object || !field_name || !object->type) return -1;
    if (object->type->kind != TYPE_RECORD || !object->type->record_def) return -1;
    int idx = record_def_get_field_index(object->type->record_def, field_name);
    if (idx >= 0) return idx;

    if (object->type->record_def->name && comp) {
        Stmt* decl = find_record_decl(comp, object->type->record_def->name);
        if (decl) {
            for (int i = 0; i < decl->record_decl.field_count; i++) {
                if (strcmp(decl->record_decl.field_names[i], field_name) == 0) {
                    return i;
                }
            }
        }
    }
    return -1;
}

static bool compiler_match_binding_name_looks_destructure(const char* name) {
    if (!name || name[0] == '\0') return false;
    if (strcmp(name, "_") == 0) return true;

    char first = name[0];
    return first >= 'a' && first <= 'z';
}

static void match_arm_jump_list_add(MatchArmJumpList* list, int jump) {
    if (!list || jump < 0) return;

    list->count++;
    if (list->count > list->capacity) {
        list->capacity = list->count * 2;
        list->jumps = (int*)safe_realloc(list->jumps, (size_t)list->capacity * sizeof(int));
    }
    list->jumps[list->count - 1] = jump;
}

static void match_arm_jump_list_patch_and_pop(Compiler* comp, MatchArmJumpList* list, int line) {
    if (!comp || !list || list->count <= 0) return;

    for (int i = 0; i < list->count; i++) {
        patch_jump(comp, list->jumps[i]);
    }
    emit_byte(comp, OP_POP, line);
}

static void match_arm_jump_list_free(MatchArmJumpList* list) {
    if (!list) return;
    if (list->jumps) free(list->jumps);
    list->jumps = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int resolve_record_field_index_from_type(Compiler* comp, Type* record_type, const char* field_name) {
    if (!record_type || !field_name) return -1;
    if (record_type->kind != TYPE_RECORD || !record_type->record_def) return -1;

    int idx = record_def_get_field_index(record_type->record_def, field_name);
    if (idx >= 0) return idx;

    if (record_type->record_def->name && comp) {
        Stmt* decl = find_record_decl(comp, record_type->record_def->name);
        if (decl) {
            for (int i = 0; i < decl->record_decl.field_count; i++) {
                if (strcmp(decl->record_decl.field_names[i], field_name) == 0) {
                    return i;
                }
            }
        }
    }

    return -1;
}

static void compile_match_value_compare(Compiler* comp,
                                        int subject_slot,
                                        Expr* pattern_expr,
                                        MatchArmJumpList* fail_jump_list,
                                        int line) {
    if (!comp || !pattern_expr) return;

    int pattern_line = pattern_expr->line > 0 ? pattern_expr->line : line;
    emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)subject_slot, pattern_line);
    compile_expression(comp, pattern_expr);
    emit_byte(comp, OP_EQ, pattern_line);
    int fail_jump = emit_jump(comp, OP_JUMP_IF_FALSE, pattern_line);
    match_arm_jump_list_add(fail_jump_list, fail_jump);
    emit_byte(comp, OP_POP, pattern_line);
}

static void compile_match_pattern_dispatch(Compiler* comp,
                                           Expr* pattern_expr,
                                           int subject_slot,
                                           bool allow_binding,
                                           MatchArmJumpList* fail_jump_list,
                                           int line) {
    if (!pattern_expr) return;

    if (!compile_structural_match_pattern(comp,
                                          pattern_expr,
                                          subject_slot,
                                          allow_binding,
                                          fail_jump_list,
                                          line)) {
        compile_match_value_compare(comp, subject_slot, pattern_expr, fail_jump_list, line);
    }
}

static bool compile_structural_match_pattern(Compiler* comp,
                                             Expr* pattern_expr,
                                             int subject_slot,
                                             bool allow_binding,
                                             MatchArmJumpList* fail_jump_list,
                                             int line) {
    if (!comp || !pattern_expr) return false;

    int pattern_line = pattern_expr->line > 0 ? pattern_expr->line : line;

    if (allow_binding &&
        pattern_expr->kind == EXPR_IDENTIFIER &&
        pattern_expr->identifier &&
        compiler_match_binding_name_looks_destructure(pattern_expr->identifier)) {
        if (strcmp(pattern_expr->identifier, "_") != 0) {
            int local = add_local(comp, pattern_expr->identifier, pattern_line);
            if (local >= 0) {
                emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)subject_slot, pattern_line);
                emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)local, pattern_line);
            }
        }
        return true;
    }

    if (pattern_expr->kind == EXPR_TUPLE_LITERAL) {
        if (!pattern_expr->type || pattern_expr->type->kind != TYPE_TUPLE) {
            compiler_set_error(comp,
                               "Tuple match pattern requires a tuple-typed subject",
                               pattern_expr->file ? pattern_expr->file : comp->file,
                               pattern_expr->line,
                               pattern_expr->column);
            return true;
        }

        int element_count = pattern_expr->tuple_literal.element_count;
        for (int i = 0; i < element_count; i++) {
            if (i > 255) {
                compiler_set_error(comp,
                                   "Tuple match pattern exceeds tuple index operand range (max 255 elements)",
                                   pattern_expr->file ? pattern_expr->file : comp->file,
                                   pattern_expr->line,
                                   pattern_expr->column);
                return true;
            }

            int element_slot = add_local_anon(comp, pattern_line);
            if (element_slot < 0) {
                return true;
            }

            emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)subject_slot, pattern_line);
            emit_byte(comp, OP_TUPLE_GET, pattern_line);
            emit_byte(comp, (uint8_t)i, pattern_line);
            emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)element_slot, pattern_line);

            compile_match_pattern_dispatch(comp,
                                           pattern_expr->tuple_literal.elements[i],
                                           element_slot,
                                           true,
                                           fail_jump_list,
                                           pattern_line);
        }
        return true;
    }

    if (pattern_expr->kind == EXPR_RECORD_LITERAL) {
        Type* record_type = pattern_expr->record_literal.record_type
            ? pattern_expr->record_literal.record_type
            : pattern_expr->type;
        if (!record_type || record_type->kind != TYPE_RECORD || !record_type->record_def) {
            compiler_set_error(comp,
                               "Record match pattern requires a concrete record-typed subject",
                               pattern_expr->file ? pattern_expr->file : comp->file,
                               pattern_expr->line,
                               pattern_expr->column);
            return true;
        }

        for (int i = 0; i < pattern_expr->record_literal.field_count; i++) {
            const char* field_name = pattern_expr->record_literal.field_names
                ? pattern_expr->record_literal.field_names[i]
                : NULL;
            int field_index = resolve_record_field_index_from_type(comp, record_type, field_name);
            if (field_index < 0) {
                compiler_set_error(comp,
                                   "Unknown record field in match pattern",
                                   pattern_expr->file ? pattern_expr->file : comp->file,
                                   pattern_expr->line,
                                   pattern_expr->column);
                return true;
            }
            if (field_index > 255) {
                compiler_set_error(comp,
                                   "Record match pattern field index exceeds operand range (max 255 fields)",
                                   pattern_expr->file ? pattern_expr->file : comp->file,
                                   pattern_expr->line,
                                   pattern_expr->column);
                return true;
            }

            int field_slot = add_local_anon(comp, pattern_line);
            if (field_slot < 0) {
                return true;
            }

            emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)subject_slot, pattern_line);
            emit_byte(comp, OP_RECORD_GET_FIELD, pattern_line);
            emit_byte(comp, (uint8_t)field_index, pattern_line);
            emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)field_slot, pattern_line);

            compile_match_pattern_dispatch(comp,
                                           pattern_expr->record_literal.field_values[i],
                                           field_slot,
                                           true,
                                           fail_jump_list,
                                           pattern_line);
        }
        return true;
    }

    if (pattern_expr->kind == EXPR_CALL && pattern_expr->call.callee) {
        char* pattern_symbol = match_pattern_call_symbol_name(pattern_expr);
        if (!pattern_symbol) {
            return false;
        }

        Stmt* enum_stmt = NULL;
        int member_index = -1;
        bool handled = false;
        if (find_enum_member_decl(comp, pattern_symbol, &enum_stmt, &member_index) &&
            enum_stmt && enum_stmt->enum_decl.has_payload_members) {
            int payload_count = (enum_stmt->enum_decl.member_payload_counts &&
                                 member_index < enum_stmt->enum_decl.member_count)
                                    ? enum_stmt->enum_decl.member_payload_counts[member_index]
                                    : 0;
            if (payload_count != pattern_expr->call.arg_count) {
                char message[320];
                snprintf(message,
                         sizeof(message),
                         "Enum payload match pattern arity mismatch for '%s.%s': expected %d, got %d",
                         enum_stmt->enum_decl.name ? enum_stmt->enum_decl.name : "<enum>",
                         (enum_stmt->enum_decl.member_names &&
                          member_index < enum_stmt->enum_decl.member_count &&
                          enum_stmt->enum_decl.member_names[member_index])
                             ? enum_stmt->enum_decl.member_names[member_index]
                             : "<member>",
                         payload_count,
                         pattern_expr->call.arg_count);
                compiler_set_error(comp,
                                   message,
                                   pattern_expr->file ? pattern_expr->file : comp->file,
                                   pattern_expr->line,
                                   pattern_expr->column);
                handled = true;
            } else if (payload_count > 254) {
                compiler_set_error(comp,
                                   "Enum payload match pattern exceeds tuple index operand range (max 254 payload elements)",
                                   pattern_expr->file ? pattern_expr->file : comp->file,
                                   pattern_expr->line,
                                   pattern_expr->column);
                handled = true;
            } else {
                emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)subject_slot, pattern_line);
                emit_byte(comp, OP_TYPEOF, pattern_line);
                emit_constant(comp, (Constant){ .as_string = "tuple", .type_index = 2 }, pattern_line);
                emit_byte(comp, OP_EQ, pattern_line);
                int type_fail_jump = emit_jump(comp, OP_JUMP_IF_FALSE, pattern_line);
                match_arm_jump_list_add(fail_jump_list, type_fail_jump);
                emit_byte(comp, OP_POP, pattern_line);

                emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)subject_slot, pattern_line);
                emit_byte(comp, OP_TUPLE_GET, pattern_line);
                emit_byte(comp, 0, pattern_line);
                int64_t tag_value = (enum_stmt->enum_decl.member_values &&
                                     member_index < enum_stmt->enum_decl.member_count)
                                        ? enum_stmt->enum_decl.member_values[member_index]
                                        : 0;
                emit_constant(comp, (Constant){ .as_int = tag_value, .type_index = 0 }, pattern_line);
                emit_byte(comp, OP_EQ, pattern_line);
                int tag_fail_jump = emit_jump(comp, OP_JUMP_IF_FALSE, pattern_line);
                match_arm_jump_list_add(fail_jump_list, tag_fail_jump);
                emit_byte(comp, OP_POP, pattern_line);

                for (int i = 0; i < payload_count; i++) {
                    int payload_slot = add_local_anon(comp, pattern_line);
                    if (payload_slot < 0) {
                        handled = true;
                        break;
                    }

                    emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)subject_slot, pattern_line);
                    emit_byte(comp, OP_TUPLE_GET, pattern_line);
                    emit_byte(comp, (uint8_t)(i + 1), pattern_line);
                    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)payload_slot, pattern_line);

                    compile_match_pattern_dispatch(comp,
                                                   pattern_expr->call.args[i],
                                                   payload_slot,
                                                   true,
                                                   fail_jump_list,
                                                   pattern_line);
                }
                handled = true;
            }
        }

        free(pattern_symbol);
        return handled;
    }

    return false;
}

static TokenType compound_assign_to_binary_op(TokenType op) {
    switch (op) {
        case TOKEN_PLUS_EQ: return TOKEN_PLUS;
        case TOKEN_MINUS_EQ: return TOKEN_MINUS;
        case TOKEN_STAR_EQ: return TOKEN_STAR;
        case TOKEN_SLASH_EQ: return TOKEN_SLASH;
        case TOKEN_PERCENT_EQ: return TOKEN_PERCENT;
        default: return TOKEN_ERROR;
    }
}

static int emit_byte(Compiler* comp, uint8_t byte, int line) {
    return chunk_emit(comp->chunk, byte, line);
}

static int emit_byte2(Compiler* comp, uint8_t byte1, uint8_t byte2, int line) {
    emit_byte(comp, byte1, line);
    return emit_byte(comp, byte2, line);
}

static int emit_u16_operand(Compiler* comp, uint16_t value, int line) {
    emit_byte(comp, (uint8_t)((value >> 8) & 0xff), line);
    return emit_byte(comp, (uint8_t)(value & 0xff), line);
}

static void compiler_set_error(Compiler* comp, const char* message, const char* file, int line, int column) {
    if (!comp || !message) return;
    comp->had_error = true;
    if (!comp->error) {
        const char* err_file = file ? file : comp->file;
        comp->error = error_create(ERROR_COMPILE, message, err_file, line, column);
    }
}

static void emit_arithmetic_op(Compiler* comp, TokenType op, Type* left_type, Type* right_type, int line) {
    bool is_double_bin =
        left_type && right_type &&
        left_type->kind == TYPE_DOUBLE && right_type->kind == TYPE_DOUBLE;

    if (is_double_bin) {
        switch (op) {
            case TOKEN_PLUS: emit_byte(comp, OP_ADD_DOUBLE, line); return;
            case TOKEN_MINUS: emit_byte(comp, OP_SUB_DOUBLE, line); return;
            case TOKEN_STAR: emit_byte(comp, OP_MUL_DOUBLE, line); return;
            case TOKEN_SLASH: emit_byte(comp, OP_DIV_DOUBLE, line); return;
            default: break;
        }
    }

    bool is_int_bin =
        left_type && right_type &&
        left_type->kind == TYPE_INT && right_type->kind == TYPE_INT;

    if (is_int_bin) {
        switch (op) {
            case TOKEN_PLUS: emit_byte(comp, OP_ADD_INT, line); return;
            case TOKEN_MINUS: emit_byte(comp, OP_SUB_INT, line); return;
            case TOKEN_STAR: emit_byte(comp, OP_MUL_INT, line); return;
            case TOKEN_SLASH: emit_byte(comp, OP_DIV_INT, line); return;
            case TOKEN_PERCENT: emit_byte(comp, OP_MOD_INT, line); return;
            default: break;
        }
    }

    switch (op) {
        case TOKEN_PLUS: emit_byte(comp, OP_ADD, line); break;
        case TOKEN_MINUS: emit_byte(comp, OP_SUB, line); break;
        case TOKEN_STAR: emit_byte(comp, OP_MUL, line); break;
        case TOKEN_SLASH: emit_byte(comp, OP_DIV, line); break;
        case TOKEN_PERCENT: emit_byte(comp, OP_MOD, line); break;
        default: break;
    }
}

static int add_constant_checked(Compiler* comp, Constant constant, int line) {
    int index = constant_pool_add(&comp->function->constants, constant);
    if (index < 0 || index > 0xffff) {
        compiler_set_error(comp, "Too many constants in function (max 65536)", comp ? comp->file : NULL, line, 0);
        return -1;
    }
    return index;
}

static void emit_constant_index(Compiler* comp, int index, int line) {
    if (index < 0) return;
    if (index <= 0xfe) {
        emit_byte2(comp, OP_CONST, (uint8_t)index, line);
        return;
    }
    emit_byte(comp, OP_CONST16, line);
    emit_u16_operand(comp, (uint16_t)index, line);
}

static void emit_load_global_name(Compiler* comp, int name_idx, int line) {
    if (name_idx < 0) return;
    if (name_idx <= 0xff) {
        emit_byte2(comp, OP_LOAD_GLOBAL, (uint8_t)name_idx, line);
        return;
    }
    emit_byte(comp, OP_LOAD_GLOBAL16, line);
    emit_u16_operand(comp, (uint16_t)name_idx, line);
}

static void emit_store_global_name(Compiler* comp, int name_idx, int line) {
    if (name_idx < 0) return;
    if (name_idx <= 0xff) {
        emit_byte2(comp, OP_STORE_GLOBAL, (uint8_t)name_idx, line);
        return;
    }
    emit_byte(comp, OP_STORE_GLOBAL16, line);
    emit_u16_operand(comp, (uint16_t)name_idx, line);
}

static void emit_call_global_name(Compiler* comp, int name_idx, uint8_t arg_count, int line) {
    if (name_idx < 0) return;
    if (name_idx <= 0xff) {
        emit_byte(comp, OP_CALL_GLOBAL, line);
        emit_byte(comp, (uint8_t)name_idx, line);
        emit_byte(comp, arg_count, line);
        return;
    }
    emit_byte(comp, OP_CALL_GLOBAL16, line);
    emit_u16_operand(comp, (uint16_t)name_idx, line);
    emit_byte(comp, arg_count, line);
}

static void emit_call_interface(Compiler* comp, int interface_name_idx, int method_name_idx, uint8_t arg_count, int line) {
    if (interface_name_idx < 0 || method_name_idx < 0) return;
    if (interface_name_idx > 0xffff || method_name_idx > 0xffff) {
        compiler_set_error(comp, "Too many constants in function (max 65536)", comp ? comp->file : NULL, line, 0);
        return;
    }

    emit_byte(comp, OP_CALL_INTERFACE, line);
    emit_u16_operand(comp, (uint16_t)interface_name_idx, line);
    emit_u16_operand(comp, (uint16_t)method_name_idx, line);
    emit_byte(comp, arg_count, line);
}

static void emit_type_test_interface_method(Compiler* comp, int interface_name_idx, int method_name_idx, int line) {
    if (interface_name_idx < 0 || method_name_idx < 0) return;
    if (interface_name_idx > 0xffff || method_name_idx > 0xffff) {
        compiler_set_error(comp, "Too many constants in function (max 65536)", comp ? comp->file : NULL, line, 0);
        return;
    }

    emit_byte(comp, OP_TYPE_TEST_INTERFACE_METHOD, line);
    emit_u16_operand(comp, (uint16_t)interface_name_idx, line);
    emit_u16_operand(comp, (uint16_t)method_name_idx, line);
}

static void emit_constant(Compiler* comp, Constant constant, int line) {
    int index = add_constant_checked(comp, constant, line);
    emit_constant_index(comp, index, line);
}

static int emit_jump(Compiler* comp, uint8_t instruction, int line) {
    emit_byte(comp, instruction, line);
    emit_byte(comp, 0xff, line);
    emit_byte(comp, 0xff, line);
    return comp->chunk->code_count - 2;
}

static int emit_jump_local_compare(Compiler* comp, uint8_t instruction, uint8_t a_slot, uint8_t b_slot, int line) {
    emit_byte(comp, instruction, line);
    emit_byte(comp, a_slot, line);
    emit_byte(comp, b_slot, line);
    emit_byte(comp, 0xff, line);
    emit_byte(comp, 0xff, line);
    return comp->chunk->code_count - 2;
}

static int emit_jump_local_compare_const(Compiler* comp, uint8_t instruction, uint8_t a_slot, uint8_t const_idx, int line) {
    emit_byte(comp, instruction, line);
    emit_byte(comp, a_slot, line);
    emit_byte(comp, const_idx, line);
    emit_byte(comp, 0xff, line);
    emit_byte(comp, 0xff, line);
    return comp->chunk->code_count - 2;
}

static int emit_jump_stack_compare_local(Compiler* comp, uint8_t instruction, uint8_t b_slot, int line) {
    emit_byte(comp, instruction, line);
    emit_byte(comp, b_slot, line);
    emit_byte(comp, 0xff, line);
    emit_byte(comp, 0xff, line);
    return comp->chunk->code_count - 2;
}

static int emit_jump_stack_compare_const(Compiler* comp, uint8_t instruction, uint8_t const_idx, int line) {
    emit_byte(comp, instruction, line);
    emit_byte(comp, const_idx, line);
    emit_byte(comp, 0xff, line);
    emit_byte(comp, 0xff, line);
    return comp->chunk->code_count - 2;
}

static int emit_jump_array_false_local_const(Compiler* comp, uint8_t array_slot, uint8_t idx, int line) {
    emit_byte(comp, OP_JUMP_IF_ARRAY_FALSE_LOCAL_CONST, line);
    emit_byte(comp, array_slot, line);
    emit_byte(comp, idx, line);
    emit_byte(comp, 0xff, line);
    emit_byte(comp, 0xff, line);
    return comp->chunk->code_count - 2;
}

static int emit_jump_array_false_local_local(Compiler* comp, uint8_t array_slot, uint8_t idx_slot, int line) {
    emit_byte(comp, OP_JUMP_IF_ARRAY_FALSE_LOCAL_LOCAL, line);
    emit_byte(comp, array_slot, line);
    emit_byte(comp, idx_slot, line);
    emit_byte(comp, 0xff, line);
    emit_byte(comp, 0xff, line);
    return comp->chunk->code_count - 2;
}

static void emit_loop_local_compare(Compiler* comp, uint8_t instruction, uint8_t a_slot, uint8_t b_slot, int target, int line) {
    emit_byte(comp, instruction, line);
    emit_byte(comp, a_slot, line);
    emit_byte(comp, b_slot, line);

    int after_operands = comp->chunk->code_count + 2;
    int offset = target - after_operands;
    if (offset < INT16_MIN || offset > INT16_MAX) {
        printf("Loop jump too large\n");
        emit_byte(comp, 0, line);
        emit_byte(comp, 0, line);
        return;
    }

    uint16_t encoded = (uint16_t)(int16_t)offset;
    emit_byte(comp, (uint8_t)((encoded >> 8) & 0xff), line);
    emit_byte(comp, (uint8_t)(encoded & 0xff), line);
}

static void emit_loop_local_compare_const(Compiler* comp, uint8_t instruction, uint8_t a_slot, uint8_t const_idx, int target, int line) {
    emit_byte(comp, instruction, line);
    emit_byte(comp, a_slot, line);
    emit_byte(comp, const_idx, line);

    int after_operands = comp->chunk->code_count + 2;
    int offset = target - after_operands;
    if (offset < INT16_MIN || offset > INT16_MAX) {
        printf("Loop jump too large\n");
        emit_byte(comp, 0, line);
        emit_byte(comp, 0, line);
        return;
    }

    uint16_t encoded = (uint16_t)(int16_t)offset;
    emit_byte(comp, (uint8_t)((encoded >> 8) & 0xff), line);
    emit_byte(comp, (uint8_t)(encoded & 0xff), line);
}

static void patch_jump(Compiler* comp, int offset) {
    int jump = comp->chunk->code_count - offset - 2;
    if (jump > INT16_MAX) {
        printf("Jump too large\n");
        return;
    }
    comp->chunk->code[offset] = (jump >> 8) & 0xff;
    comp->chunk->code[offset + 1] = jump & 0xff;
}

static void emit_loop(Compiler* comp, int loop_start, int line) {
    emit_byte(comp, OP_JUMP, line);

    int after_operands = comp->chunk->code_count + 2;
    int offset = loop_start - after_operands;
    if (offset < INT16_MIN || offset > INT16_MAX) {
        printf("Loop jump too large\n");
        emit_byte(comp, 0, line);
        emit_byte(comp, 0, line);
        return;
    }

    uint16_t encoded = (uint16_t)(int16_t)offset;
    emit_byte(comp, (uint8_t)((encoded >> 8) & 0xff), line);
    emit_byte(comp, (uint8_t)(encoded & 0xff), line);
}

static int make_constant_int(Compiler* comp, int64_t value, int line) {
    Constant c;
    c.as_int = value;
    c.type_index = 0;
    return add_constant_checked(comp, c, line);
}

static int make_constant_double(Compiler* comp, double value, int line) {
    Constant c;
    c.as_double = value;
    c.type_index = 1;
    return add_constant_checked(comp, c, line);
}

static int make_constant_string(Compiler* comp, const char* value, int line) {
    Constant c;
    c.as_string = (char*)value;
    c.type_index = 2;
    return add_constant_checked(comp, c, line);
}

static int resolve_local(Compiler* comp, const char* name) {
    for (int i = comp->function->local_count - 1; i >= 0; i--) {
        if (comp->function->local_names && comp->function->local_names[i] &&
            strcmp(comp->function->local_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static int resolve_global(Compiler* comp, const char* name) {
    for (int i = 0; i < comp->globals->symbol_count; i++) {
        if (strcmp(comp->globals->symbols[i]->name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int add_local(Compiler* comp, const char* name, int line) {
    if (comp->function->local_count >= 256) {
        compiler_set_error(comp, "Too many local variables in function (max 256)", comp ? comp->file : NULL, line, 0);
        return -1;
    }
    comp->function->local_count++;
    comp->function->local_names = (char**)safe_realloc(comp->function->local_names, comp->function->local_count * sizeof(char*));
    comp->function->debug_local_names =
        (char**)safe_realloc(comp->function->debug_local_names, comp->function->local_count * sizeof(char*));
    comp->function->local_names[comp->function->local_count - 1] = safe_strdup(name);
    comp->function->debug_local_names[comp->function->local_count - 1] = safe_strdup(name);
    return comp->function->local_count - 1;
}

static int add_local_anon(Compiler* comp, int line) {
    if (comp->function->local_count >= 256) {
        compiler_set_error(comp, "Too many local variables in function (max 256)", comp ? comp->file : NULL, line, 0);
        return -1;
    }
    comp->function->local_count++;
    comp->function->local_names = (char**)safe_realloc(comp->function->local_names, comp->function->local_count * sizeof(char*));
    comp->function->debug_local_names =
        (char**)safe_realloc(comp->function->debug_local_names, comp->function->local_count * sizeof(char*));
    comp->function->local_names[comp->function->local_count - 1] = NULL;
    comp->function->debug_local_names[comp->function->local_count - 1] = NULL;
    return comp->function->local_count - 1;
}

static int next_anonymous_function_id(Compiler* comp) {
    if (!comp || !comp->shared_anon_func_counter) return 0;
    int id = *comp->shared_anon_func_counter;
    (*comp->shared_anon_func_counter)++;
    return id;
}

static char* compiler_enum_member_symbol_name(const char* enum_name, const char* member_name) {
    if (!enum_name || !member_name) return NULL;
    size_t nlen = strlen(enum_name);
    size_t mlen = strlen(member_name);
    char* out = (char*)safe_malloc(nlen + 1 + mlen + 1);
    memcpy(out, enum_name, nlen);
    out[nlen] = '_';
    memcpy(out + nlen + 1, member_name, mlen);
    out[nlen + 1 + mlen] = '\0';
    return out;
}

static void end_scope(Compiler* comp, int start_slot) {
    if (!comp || !comp->function) return;
    if (start_slot < 0) start_slot = 0;

    for (int i = comp->function->local_count - 1; i >= start_slot; i--) {
        if (comp->function->local_names && comp->function->local_names[i]) {
            free(comp->function->local_names[i]);
            comp->function->local_names[i] = NULL;
        }
    }
}

static LoopContext* current_loop(Compiler* comp) {
    if (!comp || comp->loop_stack_count <= 0) return NULL;
    return &comp->loop_stack[comp->loop_stack_count - 1];
}

static void loop_add_jump(int** jumps, int* count, int* capacity, int offset) {
    (*count)++;
    if (*count > *capacity) {
        *capacity = (*count) * 2;
        *jumps = (int*)safe_realloc(*jumps, (size_t)(*capacity) * sizeof(int));
    }
    (*jumps)[*count - 1] = offset;
}

static void compiler_push_loop(Compiler* comp, int loop_start, bool continue_target_known, int continue_target) {
    comp->loop_stack_count++;
    if (comp->loop_stack_count > comp->loop_stack_capacity) {
        comp->loop_stack_capacity = comp->loop_stack_count * 2;
        comp->loop_stack = (LoopContext*)safe_realloc(comp->loop_stack, (size_t)comp->loop_stack_capacity * sizeof(LoopContext));
    }

    LoopContext* ctx = &comp->loop_stack[comp->loop_stack_count - 1];
    ctx->loop_start = loop_start;
    ctx->continue_target = continue_target;
    ctx->continue_target_known = continue_target_known;
    ctx->break_jumps = NULL;
    ctx->break_count = 0;
    ctx->break_capacity = 0;
    ctx->continue_jumps = NULL;
    ctx->continue_count = 0;
    ctx->continue_capacity = 0;
}

static void compiler_pop_loop(Compiler* comp) {
    if (!comp || comp->loop_stack_count <= 0) return;

    LoopContext* ctx = &comp->loop_stack[comp->loop_stack_count - 1];
    if (ctx->break_jumps) free(ctx->break_jumps);
    if (ctx->continue_jumps) free(ctx->continue_jumps);

    comp->loop_stack_count--;
}

static bool is_literal(Expr* expr) {
    return expr->kind == EXPR_LITERAL || expr->kind == EXPR_NIL;
}

static bool is_double_literal(Expr* expr) {
    return expr && expr->kind == EXPR_LITERAL && expr->type && expr->type->kind == TYPE_DOUBLE;
}

static bool is_bigint_literal(Expr* expr) {
    return expr && expr->kind == EXPR_LITERAL && expr->type && expr->type->kind == TYPE_BIGINT;
}

static bool is_string_literal(Expr* expr) {
    return expr && expr->kind == EXPR_LITERAL && expr->type && expr->type->kind == TYPE_STRING;
}

static bool is_bool_literal(Expr* expr) {
    return expr && expr->kind == EXPR_LITERAL && expr->type && expr->type->kind == TYPE_BOOL;
}

static bool is_noop_primitive_cast(Type* from_type, Type* to_type) {
    if (!from_type || !to_type) return false;
    if (from_type->kind != to_type->kind) return false;

    switch (to_type->kind) {
        case TYPE_INT:
        case TYPE_BOOL:
        case TYPE_DOUBLE:
        case TYPE_STRING:
        case TYPE_BIGINT:
            return true;
        default:
            return false;
    }
}

static bool expr_inline_eligible(Expr* expr) {
    if (!expr) return false;

    switch (expr->kind) {
        case EXPR_LITERAL:
            return expr->type && (expr->type->kind == TYPE_INT || expr->type->kind == TYPE_DOUBLE || expr->type->kind == TYPE_BOOL);
        case EXPR_IDENTIFIER:
            return true;
        case EXPR_BINARY:
            if (!expr->binary.left || !expr->binary.right) return false;
            return expr_inline_eligible(expr->binary.left) && expr_inline_eligible(expr->binary.right);
        case EXPR_UNARY:
            if (!expr->unary.operand) return false;
            return expr_inline_eligible(expr->unary.operand);
        case EXPR_CAST:
            if (!expr->cast.value || !expr->cast.target_type) return false;
            if (expr->cast.target_type->kind != TYPE_INT &&
                expr->cast.target_type->kind != TYPE_DOUBLE &&
                expr->cast.target_type->kind != TYPE_BOOL) {
                return false;
            }
            return expr_inline_eligible(expr->cast.value);
        default:
            return false;
    }
}

static int expr_node_count(Expr* expr) {
    if (!expr) return 0;

    switch (expr->kind) {
        case EXPR_LITERAL:
        case EXPR_IDENTIFIER:
        case EXPR_NIL:
            return 1;
        case EXPR_BINARY:
            return 1 + expr_node_count(expr->binary.left) + expr_node_count(expr->binary.right);
        case EXPR_UNARY:
            return 1 + expr_node_count(expr->unary.operand);
        case EXPR_AWAIT:
            return 1 + expr_node_count(expr->await_expr.expr);
        case EXPR_CAST:
            return 1 + expr_node_count(expr->cast.value);
        default:
            return 1000;
    }
}

static bool expr_equals(Expr* a, Expr* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        case EXPR_LITERAL:
            if (!a->type || !b->type) return false;
            if (a->type->kind != b->type->kind) return false;
            switch (a->type->kind) {
                case TYPE_INT:
                case TYPE_BOOL:
                    return a->literal.as_int == b->literal.as_int;
                case TYPE_DOUBLE:
                    return a->literal.as_double == b->literal.as_double;
                case TYPE_STRING:
                    return strcmp(a->literal.as_string ? a->literal.as_string : "",
                                  b->literal.as_string ? b->literal.as_string : "") == 0;
                default:
                    return false;
            }
        case EXPR_IDENTIFIER:
            if (!a->identifier || !b->identifier) return false;
            return strcmp(a->identifier, b->identifier) == 0;
        case EXPR_BINARY:
            if (a->binary.op != b->binary.op) return false;
            return expr_equals(a->binary.left, b->binary.left) &&
                   expr_equals(a->binary.right, b->binary.right);
        case EXPR_UNARY:
            if (a->unary.op != b->unary.op) return false;
            return expr_equals(a->unary.operand, b->unary.operand);
        case EXPR_AWAIT:
            return expr_equals(a->await_expr.expr, b->await_expr.expr);
        case EXPR_CAST:
            if (!a->cast.target_type || !b->cast.target_type) return false;
            if (a->cast.target_type->kind != b->cast.target_type->kind) return false;
            return expr_equals(a->cast.value, b->cast.value);
        case EXPR_NIL:
            return true;
        default:
            return false;
    }
}

static bool expr_inline_substitution_safe(Expr* expr) {
    if (!expr) return false;
    return expr->kind == EXPR_IDENTIFIER || expr->kind == EXPR_LITERAL;
}

static int find_param_index(Stmt* func_decl, const char* name) {
    if (!func_decl || func_decl->kind != STMT_FUNC_DECL || !name) return -1;
    for (int i = 0; i < func_decl->func_decl.param_count; i++) {
        if (func_decl->func_decl.params[i] && strcmp(func_decl->func_decl.params[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static void expr_substitute_params_in_place(Expr** exprp, Stmt* func_decl, Expr** args, int arg_count) {
    if (!exprp || !*exprp || !func_decl || func_decl->kind != STMT_FUNC_DECL) return;

    Expr* expr = *exprp;
    if (expr->kind == EXPR_IDENTIFIER && expr->identifier) {
        int idx = find_param_index(func_decl, expr->identifier);
        if (idx >= 0 && idx < arg_count && args && args[idx]) {
            Expr* replacement = expr_clone(args[idx]);
            expr_free(expr);
            *exprp = replacement;
        }
        return;
    }

    switch (expr->kind) {
        case EXPR_BINARY:
            if (expr->binary.left) expr_substitute_params_in_place(&expr->binary.left, func_decl, args, arg_count);
            if (expr->binary.right) expr_substitute_params_in_place(&expr->binary.right, func_decl, args, arg_count);
            break;
        case EXPR_UNARY:
            if (expr->unary.operand) expr_substitute_params_in_place(&expr->unary.operand, func_decl, args, arg_count);
            break;
        case EXPR_AWAIT:
            if (expr->await_expr.expr) expr_substitute_params_in_place(&expr->await_expr.expr, func_decl, args, arg_count);
            break;
        case EXPR_CAST:
            if (expr->cast.value) expr_substitute_params_in_place(&expr->cast.value, func_decl, args, arg_count);
            break;
        case EXPR_IF:
            if (expr->if_expr.condition) expr_substitute_params_in_place(&expr->if_expr.condition, func_decl, args, arg_count);
            if (expr->if_expr.then_expr) expr_substitute_params_in_place(&expr->if_expr.then_expr, func_decl, args, arg_count);
            if (expr->if_expr.else_expr) expr_substitute_params_in_place(&expr->if_expr.else_expr, func_decl, args, arg_count);
            break;
        case EXPR_MATCH:
            if (expr->match_expr.subject) {
                expr_substitute_params_in_place(&expr->match_expr.subject, func_decl, args, arg_count);
            }
            for (int i = 0; i < expr->match_expr.arm_count; i++) {
                if (expr->match_expr.patterns) {
                    expr_substitute_params_in_place(&expr->match_expr.patterns[i], func_decl, args, arg_count);
                }
                if (expr->match_expr.guards) {
                    expr_substitute_params_in_place(&expr->match_expr.guards[i], func_decl, args, arg_count);
                }
                if (expr->match_expr.values) {
                    expr_substitute_params_in_place(&expr->match_expr.values[i], func_decl, args, arg_count);
                }
            }
            if (expr->match_expr.else_expr) {
                expr_substitute_params_in_place(&expr->match_expr.else_expr, func_decl, args, arg_count);
            }
            break;
        default:
            break;
    }
}

static int expr_count_identifier(Expr* expr, const char* name) {
    if (!expr || !name) return 0;

    int count = 0;
    switch (expr->kind) {
        case EXPR_IDENTIFIER:
            return (expr->identifier && strcmp(expr->identifier, name) == 0) ? 1 : 0;
        case EXPR_BINARY:
            count += expr_count_identifier(expr->binary.left, name);
            count += expr_count_identifier(expr->binary.right, name);
            return count;
        case EXPR_UNARY:
            return expr_count_identifier(expr->unary.operand, name);
        case EXPR_AWAIT:
            return expr_count_identifier(expr->await_expr.expr, name);
        case EXPR_CAST:
            return expr_count_identifier(expr->cast.value, name);
        case EXPR_IF:
            count += expr_count_identifier(expr->if_expr.condition, name);
            count += expr_count_identifier(expr->if_expr.then_expr, name);
            count += expr_count_identifier(expr->if_expr.else_expr, name);
            return count;
        case EXPR_MATCH:
            count += expr_count_identifier(expr->match_expr.subject, name);
            for (int i = 0; i < expr->match_expr.arm_count; i++) {
                if (expr->match_expr.patterns) {
                    count += expr_count_identifier(expr->match_expr.patterns[i], name);
                }
                if (expr->match_expr.guards) {
                    count += expr_count_identifier(expr->match_expr.guards[i], name);
                }
                if (expr->match_expr.values) {
                    count += expr_count_identifier(expr->match_expr.values[i], name);
                }
            }
            count += expr_count_identifier(expr->match_expr.else_expr, name);
            return count;
        default:
            return 0;
    }
}

static void expr_substitute_identifier_in_place(Expr** exprp, const char* name, Expr* replacement) {
    if (!exprp || !*exprp || !name || !replacement) return;

    Expr* expr = *exprp;
    if (expr->kind == EXPR_IDENTIFIER && expr->identifier && strcmp(expr->identifier, name) == 0) {
        Expr* replacement_clone = expr_clone(replacement);
        expr_free(expr);
        *exprp = replacement_clone;
        return;
    }

    switch (expr->kind) {
        case EXPR_BINARY:
            if (expr->binary.left) expr_substitute_identifier_in_place(&expr->binary.left, name, replacement);
            if (expr->binary.right) expr_substitute_identifier_in_place(&expr->binary.right, name, replacement);
            break;
        case EXPR_UNARY:
            if (expr->unary.operand) expr_substitute_identifier_in_place(&expr->unary.operand, name, replacement);
            break;
        case EXPR_AWAIT:
            if (expr->await_expr.expr) expr_substitute_identifier_in_place(&expr->await_expr.expr, name, replacement);
            break;
        case EXPR_CAST:
            if (expr->cast.value) expr_substitute_identifier_in_place(&expr->cast.value, name, replacement);
            break;
        case EXPR_IF:
            if (expr->if_expr.condition) expr_substitute_identifier_in_place(&expr->if_expr.condition, name, replacement);
            if (expr->if_expr.then_expr) expr_substitute_identifier_in_place(&expr->if_expr.then_expr, name, replacement);
            if (expr->if_expr.else_expr) expr_substitute_identifier_in_place(&expr->if_expr.else_expr, name, replacement);
            break;
        case EXPR_MATCH:
            if (expr->match_expr.subject) {
                expr_substitute_identifier_in_place(&expr->match_expr.subject, name, replacement);
            }
            for (int i = 0; i < expr->match_expr.arm_count; i++) {
                if (expr->match_expr.patterns) {
                    expr_substitute_identifier_in_place(&expr->match_expr.patterns[i], name, replacement);
                }
                if (expr->match_expr.guards) {
                    expr_substitute_identifier_in_place(&expr->match_expr.guards[i], name, replacement);
                }
                if (expr->match_expr.values) {
                    expr_substitute_identifier_in_place(&expr->match_expr.values[i], name, replacement);
                }
            }
            if (expr->match_expr.else_expr) {
                expr_substitute_identifier_in_place(&expr->match_expr.else_expr, name, replacement);
            }
            break;
        default:
            break;
    }
}

static int compiler_jit_find_param_slot(const char* name, char** params, int param_count) {
    if (!name || !params || param_count <= 0) return -1;
    for (int i = 0; i < param_count; i++) {
        if (params[i] && strcmp(params[i], name) == 0) return i;
    }
    return -1;
}

static Stmt* compiler_jit_unwrap_single_stmt_block(Stmt* stmt) {
    while (stmt && stmt->kind == STMT_BLOCK && stmt->block.stmt_count == 1) {
        stmt = stmt->block.statements[0];
    }
    return stmt;
}

static bool compiler_jit_expr_is_param_identifier(Expr* expr,
                                                  char** params,
                                                  int param_count,
                                                  uint8_t* out_slot) {
    if (!expr || expr->kind != EXPR_IDENTIFIER) return false;
    int slot = compiler_jit_find_param_slot(expr->identifier, params, param_count);
    if (slot < 0 || slot > 255) return false;
    if (out_slot) *out_slot = (uint8_t)slot;
    return true;
}

static bool compiler_jit_expr_is_int_literal(Expr* expr, int64_t* out_value) {
    if (!expr || expr->kind != EXPR_LITERAL || !expr->type || expr->type->kind != TYPE_INT) return false;
    if (out_value) *out_value = expr->literal.as_int;
    return true;
}

static bool compiler_jit_expr_is_bool_literal(Expr* expr, bool* out_value) {
    if (!expr || expr->kind != EXPR_LITERAL || !expr->type || expr->type->kind != TYPE_BOOL) return false;
    if (out_value) *out_value = expr->literal.as_int != 0;
    return true;
}

static bool compiler_jit_stmt_is_return_param(Stmt* stmt,
                                              char** params,
                                              int param_count,
                                              uint8_t* out_slot) {
    stmt = compiler_jit_unwrap_single_stmt_block(stmt);
    if (!stmt || stmt->kind != STMT_RETURN || !stmt->return_value) return false;
    return compiler_jit_expr_is_param_identifier(stmt->return_value, params, param_count, out_slot);
}

static bool compiler_jit_stmt_is_return_bool_literal(Stmt* stmt, bool* out_value) {
    stmt = compiler_jit_unwrap_single_stmt_block(stmt);
    if (!stmt || stmt->kind != STMT_RETURN || !stmt->return_value) return false;
    return compiler_jit_expr_is_bool_literal(stmt->return_value, out_value);
}

static bool compiler_jit_stmt_is_return_int_literal(Stmt* stmt, int64_t* out_value) {
    stmt = compiler_jit_unwrap_single_stmt_block(stmt);
    if (!stmt || stmt->kind != STMT_RETURN || !stmt->return_value) return false;
    return compiler_jit_expr_is_int_literal(stmt->return_value, out_value);
}

static bool compiler_jit_summary_op_from_token(TokenType op, JitSummaryOp* out_op) {
    if (!out_op) return false;
    switch (op) {
        case TOKEN_PLUS:
            *out_op = JIT_SUMMARY_OP_ADD;
            return true;
        case TOKEN_MINUS:
            *out_op = JIT_SUMMARY_OP_SUB;
            return true;
        case TOKEN_STAR:
            *out_op = JIT_SUMMARY_OP_MUL;
            return true;
        case TOKEN_SLASH:
            *out_op = JIT_SUMMARY_OP_DIV;
            return true;
        case TOKEN_PERCENT:
            *out_op = JIT_SUMMARY_OP_MOD;
            return true;
        case TOKEN_BIT_AND:
            *out_op = JIT_SUMMARY_OP_BIT_AND;
            return true;
        case TOKEN_BIT_OR:
            *out_op = JIT_SUMMARY_OP_BIT_OR;
            return true;
        case TOKEN_BIT_XOR:
            *out_op = JIT_SUMMARY_OP_BIT_XOR;
            return true;
        case TOKEN_LT:
            *out_op = JIT_SUMMARY_OP_LT;
            return true;
        case TOKEN_LT_EQ:
            *out_op = JIT_SUMMARY_OP_LE;
            return true;
        case TOKEN_EQ_EQ:
            *out_op = JIT_SUMMARY_OP_EQ;
            return true;
        case TOKEN_BANG_EQ:
            *out_op = JIT_SUMMARY_OP_NE;
            return true;
        case TOKEN_GT:
            *out_op = JIT_SUMMARY_OP_GT;
            return true;
        case TOKEN_GT_EQ:
            *out_op = JIT_SUMMARY_OP_GE;
            return true;
        default:
            return false;
    }
}

static bool compiler_jit_map_local_const_summary_kind(JitSummaryOp op,
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

static bool compiler_jit_map_twoarg_int_summary_kind(JitSummaryOp op,
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

static bool compiler_jit_map_twoarg_bool_summary_kind(JitSummaryOp op,
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

static bool compiler_jit_map_local_const_bool_summary_kind(JitSummaryOp op,
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

static bool compiler_jit_map_twoarg_selector_summary_kind(JitSummaryOp op,
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

static bool compiler_jit_map_local_const_selector_summary_kind(JitSummaryOp op,
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

static bool compiler_jit_map_local_const_binary_kind(TokenType op,
                                                     bool guarded,
                                                     JitCompiledKind* out_kind) {
    if (!out_kind) return false;
    switch (op) {
        case TOKEN_PLUS:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_ADD_CONST
                                : JIT_COMPILED_KIND_INT_ADD_LOCAL_CONST;
            return true;
        case TOKEN_MINUS:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SUB_CONST
                                : JIT_COMPILED_KIND_INT_SUB_LOCAL_CONST;
            return true;
        case TOKEN_STAR:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MUL_CONST
                                : JIT_COMPILED_KIND_INT_MUL_LOCAL_CONST;
            return true;
        case TOKEN_SLASH:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_DIV_CONST
                                : JIT_COMPILED_KIND_INT_DIV_LOCAL_CONST;
            return true;
        case TOKEN_PERCENT:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MOD_CONST
                                : JIT_COMPILED_KIND_INT_MOD_LOCAL_CONST;
            return true;
        case TOKEN_BIT_AND:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_AND_CONST
                                : JIT_COMPILED_KIND_INT_BIT_AND_LOCAL_CONST;
            return true;
        case TOKEN_BIT_OR:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_OR_CONST
                                : JIT_COMPILED_KIND_INT_BIT_OR_LOCAL_CONST;
            return true;
        case TOKEN_BIT_XOR:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_XOR_CONST
                                : JIT_COMPILED_KIND_INT_BIT_XOR_LOCAL_CONST;
            return true;
        default:
            return false;
    }
}

static bool compiler_jit_map_twoarg_int_kind(TokenType op,
                                             bool guarded,
                                             JitCompiledKind* out_kind) {
    if (!out_kind) return false;
    switch (op) {
        case TOKEN_PLUS:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_ADD_LOCALS
                                : JIT_COMPILED_KIND_INT_ADD_LOCALS;
            return true;
        case TOKEN_MINUS:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SUB_LOCALS
                                : JIT_COMPILED_KIND_INT_SUB_LOCALS;
            return true;
        case TOKEN_STAR:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_MUL_LOCALS
                                : JIT_COMPILED_KIND_INT_MUL_LOCALS;
            return true;
        case TOKEN_BIT_AND:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_AND_LOCALS
                                : JIT_COMPILED_KIND_INT_BIT_AND_LOCALS;
            return true;
        case TOKEN_BIT_OR:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_OR_LOCALS
                                : JIT_COMPILED_KIND_INT_BIT_OR_LOCALS;
            return true;
        case TOKEN_BIT_XOR:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_BIT_XOR_LOCALS
                                : JIT_COMPILED_KIND_INT_BIT_XOR_LOCALS;
            return true;
        default:
            return false;
    }
}

static bool compiler_jit_map_twoarg_bool_kind(TokenType op,
                                              bool guarded,
                                              JitCompiledKind* out_kind) {
    if (!out_kind) return false;
    switch (op) {
        case TOKEN_LT:
            *out_kind = guarded ? JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_LT_LOCALS
                                : JIT_COMPILED_KIND_BOOL_LT_LOCALS;
            return true;
        case TOKEN_LT_EQ:
            *out_kind = guarded ? JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_LE_LOCALS
                                : JIT_COMPILED_KIND_BOOL_LE_LOCALS;
            return true;
        case TOKEN_EQ_EQ:
            *out_kind = guarded ? JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_EQ_LOCALS
                                : JIT_COMPILED_KIND_BOOL_EQ_LOCALS;
            return true;
        case TOKEN_BANG_EQ:
            *out_kind = guarded ? JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_NE_LOCALS
                                : JIT_COMPILED_KIND_BOOL_NE_LOCALS;
            return true;
        case TOKEN_GT:
            *out_kind = guarded ? JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_GT_LOCALS
                                : JIT_COMPILED_KIND_BOOL_GT_LOCALS;
            return true;
        case TOKEN_GT_EQ:
            *out_kind = guarded ? JIT_COMPILED_KIND_BOOL_GUARD_GE_CONST_RET_BOOL_ELSE_GE_LOCALS
                                : JIT_COMPILED_KIND_BOOL_GE_LOCALS;
            return true;
        default:
            return false;
    }
}

static bool compiler_jit_map_twoarg_selector_kind(TokenType op,
                                                  bool guarded,
                                                  JitCompiledKind* out_kind) {
    if (!out_kind) return false;
    switch (op) {
        case TOKEN_LT:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LT_LOCALS
                                : JIT_COMPILED_KIND_INT_SELECT_LT_LOCALS;
            return true;
        case TOKEN_LT_EQ:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_LE_LOCALS
                                : JIT_COMPILED_KIND_INT_SELECT_LE_LOCALS;
            return true;
        case TOKEN_GT:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GT_LOCALS
                                : JIT_COMPILED_KIND_INT_SELECT_GT_LOCALS;
            return true;
        case TOKEN_GT_EQ:
            *out_kind = guarded ? JIT_COMPILED_KIND_INT_GUARD_GE_CONST_RET_LOCAL_ELSE_SELECT_GE_LOCALS
                                : JIT_COMPILED_KIND_INT_SELECT_GE_LOCALS;
            return true;
        default:
            return false;
    }
}

static bool compiler_jit_normalize_selector_compare_op(TokenType op,
                                                       uint8_t left_slot,
                                                       uint8_t right_slot,
                                                       TokenType* out_op) {
    if (!out_op) return false;
    if (left_slot == 0 && right_slot == 1) {
        *out_op = op;
        return true;
    }
    if (left_slot != 1 || right_slot != 0) return false;

    switch (op) {
        case TOKEN_LT:
            *out_op = TOKEN_GT;
            return true;
        case TOKEN_LT_EQ:
            *out_op = TOKEN_GT_EQ;
            return true;
        case TOKEN_GT:
            *out_op = TOKEN_LT;
            return true;
        case TOKEN_GT_EQ:
            *out_op = TOKEN_LT_EQ;
            return true;
        default:
            return false;
    }
}

static bool compiler_jit_summarize_local_const_binary_expr(Expr* expr,
                                                           char** params,
                                                           int param_count,
                                                           bool guarded,
                                                           JitFunctionSummary* out_summary) {
    uint8_t slot = 0;
    int64_t rhs_const = 0;
    JitSummaryOp summary_op = JIT_SUMMARY_OP_NONE;
    JitCompiledKind compiled_kind = JIT_COMPILED_KIND_NONE;

    if (!expr || expr->kind != EXPR_BINARY || !out_summary) return false;
    if (!compiler_jit_expr_is_param_identifier(expr->binary.left, params, param_count, &slot)) return false;
    if (!compiler_jit_expr_is_int_literal(expr->binary.right, &rhs_const)) return false;
    if ((expr->binary.op == TOKEN_SLASH || expr->binary.op == TOKEN_PERCENT) && rhs_const == 0) return false;
    if (!compiler_jit_summary_op_from_token(expr->binary.op, &summary_op)) return false;
    if (!compiler_jit_map_local_const_summary_kind(summary_op, guarded, &compiled_kind)) return false;

    memset(out_summary, 0, sizeof(*out_summary));
    out_summary->kind = guarded ? JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_BINARY
                                : JIT_SUMMARY_KIND_INT_LOCAL_CONST_BINARY;
    out_summary->op = summary_op;
    out_summary->slot0 = slot;
    if (guarded) {
        out_summary->int_const1 = rhs_const;
    } else {
        out_summary->int_const0 = rhs_const;
    }
    return true;
}

static bool compiler_jit_summarize_twoarg_int_binary_expr(Expr* expr,
                                                          char** params,
                                                          int param_count,
                                                          bool guarded,
                                                          JitFunctionSummary* out_summary) {
    uint8_t left_slot = 0;
    uint8_t right_slot = 0;
    JitSummaryOp summary_op = JIT_SUMMARY_OP_NONE;
    JitCompiledKind compiled_kind = JIT_COMPILED_KIND_NONE;

    if (!expr || expr->kind != EXPR_BINARY || !out_summary) return false;
    if (!compiler_jit_expr_is_param_identifier(expr->binary.left, params, param_count, &left_slot)) return false;
    if (!compiler_jit_expr_is_param_identifier(expr->binary.right, params, param_count, &right_slot)) return false;
    if (left_slot == right_slot) return false;
    if (!compiler_jit_summary_op_from_token(expr->binary.op, &summary_op)) return false;
    if (!compiler_jit_map_twoarg_int_summary_kind(summary_op, guarded, &compiled_kind)) return false;

    memset(out_summary, 0, sizeof(*out_summary));
    out_summary->kind = guarded ? JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_BINARY
                                : JIT_SUMMARY_KIND_INT_TWOARG_BINARY;
    out_summary->op = summary_op;
    out_summary->slot0 = left_slot;
    out_summary->slot1 = right_slot;
    return true;
}

static bool compiler_jit_summarize_twoarg_selector_stmt(Stmt* stmt,
                                                        Stmt* fallback_stmt,
                                                        char** params,
                                                        int param_count,
                                                        bool guarded,
                                                        JitFunctionSummary* out_summary) {
    uint8_t left_slot = 0;
    uint8_t right_slot = 0;
    uint8_t true_slot = 0;
    uint8_t false_slot = 0;
    TokenType normalized_op = TOKEN_ERROR;
    JitSummaryOp summary_op = JIT_SUMMARY_OP_NONE;
    JitCompiledKind compiled_kind = JIT_COMPILED_KIND_NONE;
    Stmt* then_stmt = NULL;

    if (!stmt || stmt->kind != STMT_IF || !out_summary) return false;
    if (stmt->if_stmt.else_branch) return false;
    if (!stmt->if_stmt.condition || stmt->if_stmt.condition->kind != EXPR_BINARY) return false;
    if (!compiler_jit_expr_is_param_identifier(stmt->if_stmt.condition->binary.left,
                                               params,
                                               param_count,
                                               &left_slot)) {
        return false;
    }
    if (!compiler_jit_expr_is_param_identifier(stmt->if_stmt.condition->binary.right,
                                               params,
                                               param_count,
                                               &right_slot)) {
        return false;
    }
    if (left_slot == right_slot) return false;
    if (!compiler_jit_normalize_selector_compare_op(stmt->if_stmt.condition->binary.op,
                                                    left_slot,
                                                    right_slot,
                                                    &normalized_op)) {
        return false;
    }
    if (!compiler_jit_summary_op_from_token(normalized_op, &summary_op)) return false;
    if (!compiler_jit_map_twoarg_selector_summary_kind(summary_op, guarded, &compiled_kind)) return false;

    then_stmt = compiler_jit_unwrap_single_stmt_block(stmt->if_stmt.then_branch);
    fallback_stmt = compiler_jit_unwrap_single_stmt_block(fallback_stmt);
    if (!compiler_jit_stmt_is_return_param(then_stmt, params, param_count, &true_slot)) return false;
    if (!compiler_jit_stmt_is_return_param(fallback_stmt, params, param_count, &false_slot)) return false;
    if (true_slot > 1 || false_slot > 1 || true_slot == false_slot) return false;

    memset(out_summary, 0, sizeof(*out_summary));
    out_summary->kind = guarded ? JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_SELECTOR
                                : JIT_SUMMARY_KIND_INT_TWOARG_SELECTOR;
    out_summary->op = summary_op;
    out_summary->slot0 = true_slot;
    out_summary->slot1 = false_slot;
    return true;
}

static bool compiler_jit_summarize_local_const_selector_stmt(Stmt* stmt,
                                                             Stmt* fallback_stmt,
                                                             char** params,
                                                             int param_count,
                                                             bool guarded,
                                                             JitFunctionSummary* out_summary) {
    uint8_t slot = 0;
    int64_t compare_const = 0;
    int64_t then_const = 0;
    int64_t fallback_const = 0;
    TokenType normalized_op = TOKEN_ERROR;
    JitSummaryOp summary_op = JIT_SUMMARY_OP_NONE;
    JitCompiledKind compiled_kind = JIT_COMPILED_KIND_NONE;
    bool then_returns_local = false;
    bool fallback_returns_local = false;
    Stmt* then_stmt = NULL;

    if (!stmt || stmt->kind != STMT_IF || !out_summary) return false;
    if (stmt->if_stmt.else_branch) return false;
    if (!stmt->if_stmt.condition || stmt->if_stmt.condition->kind != EXPR_BINARY) return false;
    if (!compiler_jit_expr_is_param_identifier(stmt->if_stmt.condition->binary.left,
                                               params,
                                               param_count,
                                               &slot)) {
        return false;
    }
    if (!compiler_jit_expr_is_int_literal(stmt->if_stmt.condition->binary.right, &compare_const)) return false;
    if (!compiler_jit_normalize_selector_compare_op(stmt->if_stmt.condition->binary.op, 0, 1, &normalized_op)) {
        return false;
    }
    if (!compiler_jit_summary_op_from_token(normalized_op, &summary_op)) return false;

    then_stmt = compiler_jit_unwrap_single_stmt_block(stmt->if_stmt.then_branch);
    fallback_stmt = compiler_jit_unwrap_single_stmt_block(fallback_stmt);
    then_returns_local = compiler_jit_stmt_is_return_param(then_stmt, params, param_count, NULL);
    fallback_returns_local = compiler_jit_stmt_is_return_param(fallback_stmt, params, param_count, NULL);
    if (then_returns_local) {
        uint8_t then_slot = 0;
        if (!compiler_jit_stmt_is_return_param(then_stmt, params, param_count, &then_slot) || then_slot != slot) {
            return false;
        }
    } else if (!compiler_jit_stmt_is_return_int_literal(then_stmt, &then_const) || then_const != compare_const) {
        return false;
    }

    if (fallback_returns_local) {
        uint8_t fallback_slot = 0;
        if (!compiler_jit_stmt_is_return_param(fallback_stmt, params, param_count, &fallback_slot) ||
            fallback_slot != slot) {
            return false;
        }
    } else if (!compiler_jit_stmt_is_return_int_literal(fallback_stmt, &fallback_const) ||
               fallback_const != compare_const) {
        return false;
    }

    if (then_returns_local == fallback_returns_local) return false;
    if (!compiler_jit_map_local_const_selector_summary_kind(summary_op,
                                                            guarded,
                                                            then_returns_local,
                                                            &compiled_kind)) {
        return false;
    }

    memset(out_summary, 0, sizeof(*out_summary));
    out_summary->kind = guarded ? JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_SELECTOR
                                : JIT_SUMMARY_KIND_INT_LOCAL_CONST_SELECTOR;
    out_summary->op = summary_op;
    out_summary->slot0 = slot;
    out_summary->slot1 = then_returns_local ? 1u : 0u;
    if (guarded) {
        out_summary->int_const1 = compare_const;
    } else {
        out_summary->int_const0 = compare_const;
    }
    return true;
}

static bool compiler_jit_summarize_twoarg_bool_compare_expr(Expr* expr,
                                                            char** params,
                                                            int param_count,
                                                            bool guarded,
                                                            JitFunctionSummary* out_summary) {
    uint8_t left_slot = 0;
    uint8_t right_slot = 0;
    JitSummaryOp summary_op = JIT_SUMMARY_OP_NONE;
    JitCompiledKind compiled_kind = JIT_COMPILED_KIND_NONE;

    if (!expr || expr->kind != EXPR_BINARY || !out_summary) return false;
    if (!compiler_jit_expr_is_param_identifier(expr->binary.left, params, param_count, &left_slot)) return false;
    if (!compiler_jit_expr_is_param_identifier(expr->binary.right, params, param_count, &right_slot)) return false;
    if (left_slot == right_slot) return false;
    if (!compiler_jit_summary_op_from_token(expr->binary.op, &summary_op)) return false;
    if (!compiler_jit_map_twoarg_bool_summary_kind(summary_op, guarded, &compiled_kind)) return false;

    memset(out_summary, 0, sizeof(*out_summary));
    out_summary->kind = guarded ? JIT_SUMMARY_KIND_BOOL_GUARDED_TWOARG_COMPARE
                                : JIT_SUMMARY_KIND_BOOL_TWOARG_COMPARE;
    out_summary->op = summary_op;
    out_summary->slot0 = left_slot;
    out_summary->slot1 = right_slot;
    return true;
}

static bool compiler_jit_summarize_local_const_bool_compare_expr(Expr* expr,
                                                                 char** params,
                                                                 int param_count,
                                                                 JitFunctionSummary* out_summary) {
    uint8_t slot = 0;
    int64_t rhs_const = 0;
    JitSummaryOp summary_op = JIT_SUMMARY_OP_NONE;
    JitCompiledKind compiled_kind = JIT_COMPILED_KIND_NONE;

    if (!expr || expr->kind != EXPR_BINARY || !out_summary) return false;
    if (!compiler_jit_expr_is_param_identifier(expr->binary.left, params, param_count, &slot)) return false;
    if (!compiler_jit_expr_is_int_literal(expr->binary.right, &rhs_const)) return false;
    if (!compiler_jit_summary_op_from_token(expr->binary.op, &summary_op)) return false;
    if (!compiler_jit_map_local_const_bool_summary_kind(summary_op, &compiled_kind)) return false;

    memset(out_summary, 0, sizeof(*out_summary));
    out_summary->kind = JIT_SUMMARY_KIND_BOOL_LOCAL_CONST_COMPARE;
    out_summary->op = summary_op;
    out_summary->slot0 = slot;
    out_summary->int_const0 = rhs_const;
    return true;
}

static bool compiler_jit_match_guard_return_local(Stmt* stmt,
                                                  char** params,
                                                  int param_count,
                                                  uint8_t* out_slot,
                                                  int64_t* out_guard_const) {
    uint8_t condition_slot = 0;
    uint8_t return_slot = 0;
    int64_t guard_const = 0;
    Stmt* then_stmt = NULL;

    if (!stmt || stmt->kind != STMT_IF || stmt->if_stmt.else_branch) return false;
    if (!stmt->if_stmt.condition || stmt->if_stmt.condition->kind != EXPR_BINARY) return false;
    if (stmt->if_stmt.condition->binary.op != TOKEN_LT) return false;
    if (!compiler_jit_expr_is_param_identifier(stmt->if_stmt.condition->binary.left,
                                               params,
                                               param_count,
                                               &condition_slot)) {
        return false;
    }
    if (!compiler_jit_expr_is_int_literal(stmt->if_stmt.condition->binary.right, &guard_const)) return false;
    then_stmt = compiler_jit_unwrap_single_stmt_block(stmt->if_stmt.then_branch);
    if (!compiler_jit_stmt_is_return_param(then_stmt, params, param_count, &return_slot)) return false;
    if (return_slot != condition_slot) return false;

    if (out_slot) *out_slot = condition_slot;
    if (out_guard_const) *out_guard_const = guard_const;
    return true;
}

static bool compiler_jit_match_guard_return_bool(Stmt* stmt,
                                                 char** params,
                                                 int param_count,
                                                 uint8_t* out_slot,
                                                 int64_t* out_guard_const,
                                                 bool* out_guard_result) {
    uint8_t condition_slot = 0;
    int64_t guard_const = 0;
    bool guard_result = false;
    Stmt* then_stmt = NULL;

    if (!stmt || stmt->kind != STMT_IF || stmt->if_stmt.else_branch) return false;
    if (!stmt->if_stmt.condition || stmt->if_stmt.condition->kind != EXPR_BINARY) return false;
    if (stmt->if_stmt.condition->binary.op != TOKEN_LT) return false;
    if (!compiler_jit_expr_is_param_identifier(stmt->if_stmt.condition->binary.left,
                                               params,
                                               param_count,
                                               &condition_slot)) {
        return false;
    }
    if (!compiler_jit_expr_is_int_literal(stmt->if_stmt.condition->binary.right, &guard_const)) return false;
    then_stmt = compiler_jit_unwrap_single_stmt_block(stmt->if_stmt.then_branch);
    if (!compiler_jit_stmt_is_return_bool_literal(then_stmt, &guard_result)) return false;

    if (out_slot) *out_slot = condition_slot;
    if (out_guard_const) *out_guard_const = guard_const;
    if (out_guard_result) *out_guard_result = guard_result;
    return true;
}

static bool compiler_jit_plan_from_summary(const JitFunctionSummary* summary,
                                           JitCompiledPlan* out_plan) {
    if (!summary || !out_plan || summary->kind == JIT_SUMMARY_KIND_NONE) return false;

    memset(out_plan, 0, sizeof(*out_plan));
    switch (summary->kind) {
        case JIT_SUMMARY_KIND_INT_LOCAL_CONST_BINARY:
            if (!compiler_jit_map_local_const_summary_kind(summary->op, false, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->int_const0 = summary->int_const0;
            return true;
        case JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_BINARY:
            if (!compiler_jit_map_local_const_summary_kind(summary->op, true, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->int_const0 = summary->int_const0;
            out_plan->int_const1 = summary->int_const1;
            return true;
        case JIT_SUMMARY_KIND_INT_TWOARG_BINARY:
            if (!compiler_jit_map_twoarg_int_summary_kind(summary->op, false, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            return true;
        case JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_BINARY:
            if (!compiler_jit_map_twoarg_int_summary_kind(summary->op, true, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            out_plan->int_const0 = summary->int_const0;
            return true;
        case JIT_SUMMARY_KIND_BOOL_TWOARG_COMPARE:
            if (!compiler_jit_map_twoarg_bool_summary_kind(summary->op, false, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            return true;
        case JIT_SUMMARY_KIND_BOOL_LOCAL_CONST_COMPARE:
            if (!compiler_jit_map_local_const_bool_summary_kind(summary->op, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->int_const0 = summary->int_const0;
            return true;
        case JIT_SUMMARY_KIND_BOOL_GUARDED_TWOARG_COMPARE:
            if (!compiler_jit_map_twoarg_bool_summary_kind(summary->op, true, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            out_plan->int_const0 = summary->int_const0;
            out_plan->int_const1 = summary->int_const1;
            return true;
        case JIT_SUMMARY_KIND_INT_TWOARG_SELECTOR:
            if (!compiler_jit_map_twoarg_selector_summary_kind(summary->op, false, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            return true;
        case JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_SELECTOR:
            if (!compiler_jit_map_twoarg_selector_summary_kind(summary->op, true, &out_plan->kind)) return false;
            out_plan->op = summary->op;
            out_plan->local_slot = summary->slot0;
            out_plan->local_slot_b = summary->slot1;
            out_plan->int_const0 = summary->int_const0;
            return true;
        case JIT_SUMMARY_KIND_INT_LOCAL_CONST_SELECTOR:
            if (!compiler_jit_map_local_const_selector_summary_kind(summary->op,
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
            if (!compiler_jit_map_local_const_selector_summary_kind(summary->op,
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

static bool compiler_jit_try_summarize_leaf_body(Type* return_type,
                                                 char** params,
                                                 int param_count,
                                                 Stmt* body,
                                                 JitFunctionSummary* out_summary) {
    uint8_t guard_slot = 0;
    int64_t guard_const = 0;
    bool guard_result = false;
    Stmt* stmt0 = NULL;
    Stmt* stmt1 = NULL;
    Stmt* stmt2 = NULL;

    if (!return_type || !params || param_count <= 0 || !body || !out_summary) return false;
    memset(out_summary, 0, sizeof(*out_summary));
    out_summary->kind = JIT_SUMMARY_KIND_NONE;
    out_summary->op = JIT_SUMMARY_OP_NONE;

    stmt0 = compiler_jit_unwrap_single_stmt_block(body->block.statements[0]);
    if (stmt0 && stmt0->kind == STMT_RETURN) {
        if (param_count == 1 && return_type->kind == TYPE_INT &&
            compiler_jit_summarize_local_const_binary_expr(stmt0->return_value,
                                                           params,
                                                           param_count,
                                                           false,
                                                           out_summary)) {
            return true;
        }
        if (param_count == 2 && return_type->kind == TYPE_INT &&
            compiler_jit_summarize_twoarg_int_binary_expr(stmt0->return_value,
                                                          params,
                                                          param_count,
                                                          false,
                                                          out_summary)) {
            return true;
        }
        if (param_count == 2 && return_type->kind == TYPE_BOOL &&
            compiler_jit_summarize_twoarg_bool_compare_expr(stmt0->return_value,
                                                            params,
                                                            param_count,
                                                            false,
                                                            out_summary)) {
            return true;
        }
        if (param_count == 1 && return_type->kind == TYPE_BOOL &&
            compiler_jit_summarize_local_const_bool_compare_expr(stmt0->return_value,
                                                                 params,
                                                                 param_count,
                                                                 out_summary)) {
            return true;
        }
    }

    if (body->block.stmt_count == 2) {
        stmt1 = compiler_jit_unwrap_single_stmt_block(body->block.statements[1]);
        if (!stmt1 || stmt1->kind != STMT_RETURN || !stmt1->return_value) return false;

        if (param_count == 2 && return_type->kind == TYPE_INT &&
            compiler_jit_summarize_twoarg_selector_stmt(body->block.statements[0],
                                                        body->block.statements[1],
                                                        params,
                                                        param_count,
                                                        false,
                                                        out_summary)) {
            return true;
        }

        if (param_count == 1 && return_type->kind == TYPE_INT &&
            compiler_jit_summarize_local_const_selector_stmt(body->block.statements[0],
                                                             body->block.statements[1],
                                                             params,
                                                             param_count,
                                                             false,
                                                             out_summary)) {
            return true;
        }

        if (param_count == 1 && return_type->kind == TYPE_INT &&
            compiler_jit_match_guard_return_local(body->block.statements[0],
                                                  params,
                                                  param_count,
                                                  &guard_slot,
                                                  &guard_const) &&
            compiler_jit_summarize_local_const_binary_expr(stmt1->return_value,
                                                           params,
                                                           param_count,
                                                           true,
                                                           out_summary) &&
            out_summary->slot0 == guard_slot) {
            out_summary->int_const0 = guard_const;
            return true;
        }

        if (param_count == 2 && return_type->kind == TYPE_INT &&
            compiler_jit_match_guard_return_local(body->block.statements[0],
                                                  params,
                                                  param_count,
                                                  &guard_slot,
                                                  &guard_const) &&
            compiler_jit_summarize_twoarg_int_binary_expr(stmt1->return_value,
                                                          params,
                                                          param_count,
                                                          true,
                                                          out_summary) &&
            out_summary->slot0 == guard_slot) {
            out_summary->int_const0 = guard_const;
            return true;
        }

        if (param_count == 2 && return_type->kind == TYPE_BOOL &&
            compiler_jit_match_guard_return_bool(body->block.statements[0],
                                                 params,
                                                 param_count,
                                                 &guard_slot,
                                                 &guard_const,
                                                 &guard_result) &&
            compiler_jit_summarize_twoarg_bool_compare_expr(stmt1->return_value,
                                                            params,
                                                            param_count,
                                                            true,
                                                            out_summary) &&
            out_summary->slot0 == guard_slot) {
            out_summary->int_const0 = guard_const;
            out_summary->int_const1 = guard_result ? 1 : 0;
            return true;
        }
        return false;
    }

    if (body->block.stmt_count != 3) return false;
    stmt2 = compiler_jit_unwrap_single_stmt_block(body->block.statements[2]);
    if (!stmt2 || stmt2->kind != STMT_RETURN || !stmt2->return_value) return false;

    if (param_count == 2 && return_type->kind == TYPE_INT &&
        compiler_jit_match_guard_return_local(body->block.statements[0],
                                              params,
                                              param_count,
                                              &guard_slot,
                                              &guard_const) &&
        guard_slot == 0 &&
        compiler_jit_summarize_twoarg_selector_stmt(body->block.statements[1],
                                                    body->block.statements[2],
                                                    params,
                                                    param_count,
                                                    true,
                                                    out_summary)) {
        out_summary->int_const0 = guard_const;
        return true;
    }

    if (param_count == 1 && return_type->kind == TYPE_INT &&
        compiler_jit_match_guard_return_local(body->block.statements[0],
                                              params,
                                              param_count,
                                              &guard_slot,
                                              &guard_const) &&
        compiler_jit_summarize_local_const_selector_stmt(body->block.statements[1],
                                                         body->block.statements[2],
                                                         params,
                                                         param_count,
                                                         true,
                                                         out_summary) &&
        out_summary->slot0 == guard_slot) {
        out_summary->int_const0 = guard_const;
        return true;
    }

    return false;
}

static void compiler_refresh_jit_profile_metadata(ObjFunction* func) {
    JitFunctionSummary summary;
    uint8_t native_family_mask = JIT_PROFILE_NATIVE_FAMILY_NONE;

    if (!func) return;
    summary = func->jit_profile.summary;
    switch (summary.kind) {
        case JIT_SUMMARY_KIND_INT_LOCAL_CONST_BINARY:
        case JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_BINARY:
        case JIT_SUMMARY_KIND_INT_TWOARG_BINARY:
        case JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_BINARY:
            native_family_mask = JIT_PROFILE_NATIVE_FAMILY_ARITHMETIC;
            break;
        case JIT_SUMMARY_KIND_BOOL_TWOARG_COMPARE:
        case JIT_SUMMARY_KIND_BOOL_LOCAL_CONST_COMPARE:
        case JIT_SUMMARY_KIND_BOOL_GUARDED_TWOARG_COMPARE:
            native_family_mask = JIT_PROFILE_NATIVE_FAMILY_COMPARE;
            break;
        case JIT_SUMMARY_KIND_INT_TWOARG_SELECTOR:
        case JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_SELECTOR:
        case JIT_SUMMARY_KIND_INT_LOCAL_CONST_SELECTOR:
        case JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_SELECTOR:
            native_family_mask = JIT_PROFILE_NATIVE_FAMILY_SELECTOR;
            break;
        case JIT_SUMMARY_KIND_NONE:
        default:
            native_family_mask = JIT_PROFILE_NATIVE_FAMILY_NONE;
            break;
    }
    memset(&func->jit_profile, 0, sizeof(func->jit_profile));
    func->jit_profile.flags = JIT_PROFILE_FLAG_NONE;
    if (func->is_async) func->jit_profile.flags |= JIT_PROFILE_FLAG_ASYNC;
    if (func->capture_count != 0) func->jit_profile.flags |= JIT_PROFILE_FLAG_HAS_CAPTURES;
    func->jit_profile.support_mask = JIT_PROFILE_SUPPORT_NONE;
    if (!func->is_async) {
        func->jit_profile.support_mask |= JIT_PROFILE_SUPPORT_STUB;
        if (func->capture_count == 0 &&
            summary.kind != JIT_SUMMARY_KIND_NONE &&
            func->jit_hint_plan.kind != JIT_COMPILED_KIND_NONE) {
            func->jit_profile.support_mask |= JIT_PROFILE_SUPPORT_NATIVE_SUMMARY;
        }
    }
    func->jit_profile.native_family_mask = native_family_mask;
    func->jit_profile.param_count = func->param_count;
    func->jit_profile.local_count = func->local_count;
    func->jit_profile.capture_count = func->capture_count;
    func->jit_profile.summary = summary;
}

static void compiler_reset_jit_profile(ObjFunction* func) {
    if (!func) return;
    memset(&func->jit_profile.summary, 0, sizeof(func->jit_profile.summary));
    func->jit_profile.summary.kind = JIT_SUMMARY_KIND_NONE;
    func->jit_profile.summary.op = JIT_SUMMARY_OP_NONE;
    compiler_refresh_jit_profile_metadata(func);
}

static void compiler_try_assign_jit_hint(ObjFunction* func,
                                         Type* return_type,
                                         char** params,
                                         Type** param_types,
                                         int param_count,
                                         Stmt* body) {
    JitFunctionSummary summary;

    if (!func) return;
    compiler_reset_jit_profile(func);
    memset(&func->jit_hint_plan, 0, sizeof(func->jit_hint_plan));
    func->jit_hint_plan.kind = JIT_COMPILED_KIND_NONE;

    if (func->is_async || func->capture_count != 0 || !return_type || !body || body->kind != STMT_BLOCK) return;
    if (!params || !param_types || param_count <= 0 || body->block.stmt_count <= 0) return;
    for (int i = 0; i < param_count; i++) {
        if (!param_types[i] || param_types[i]->kind != TYPE_INT) return;
    }

    if (compiler_jit_try_summarize_leaf_body(return_type, params, param_count, body, &summary) &&
        compiler_jit_plan_from_summary(&summary, &func->jit_hint_plan)) {
        func->jit_profile.summary = summary;
        compiler_refresh_jit_profile_metadata(func);
        return;
    }
}

static bool func_inline_eligible(Stmt* func_decl) {
    if (!func_decl || func_decl->kind != STMT_FUNC_DECL) return false;
    if (func_decl->func_decl.is_async) return false;
    if (!func_decl->func_decl.return_type) return false;
    TypeKind ret = func_decl->func_decl.return_type->kind;
    if (ret != TYPE_INT && ret != TYPE_DOUBLE && ret != TYPE_BOOL) return false;

    Stmt* body = func_decl->func_decl.body;
    if (!body || body->kind != STMT_BLOCK) return false;
    if (body->block.stmt_count <= 0) return false;

    // Require a single return as the final statement, and only var decls before it.
    Stmt* last = body->block.statements[body->block.stmt_count - 1];
    if (!last || last->kind != STMT_RETURN || !last->return_value) return false;
    if (!expr_inline_eligible(last->return_value)) return false;

    for (int i = 0; i < body->block.stmt_count - 1; i++) {
        Stmt* stmt = body->block.statements[i];
        if (!stmt || stmt->kind != STMT_VAR_DECL) return false;
        // Avoid parameter shadowing: substitution-based inlining relies on params
        // always referring to the original call arguments.
        if (find_param_index(func_decl, stmt->var_decl.name) >= 0) return false;
        if (!stmt->var_decl.initializer) return false;
        if (!expr_inline_eligible(stmt->var_decl.initializer)) return false;
    }

    // Keep this conservative to avoid code-size explosions.
    if (body->block.stmt_count > 16) return false;
    if (func_decl->func_decl.param_count > 8) return false;

    return true;
}

static bool compile_inline_call(Compiler* comp, Stmt* func_decl, Expr* call_expr) {
    if (!comp || !func_decl || !call_expr || call_expr->kind != EXPR_CALL) return false;
    if (comp->is_top_level) return false;
    if (!func_inline_eligible(func_decl)) return false;

    int arg_count = call_expr->call.arg_count;
    if (arg_count != func_decl->func_decl.param_count) return false;

    int inline_scope_start = comp->function->local_count;

    // For hot loops, avoid evaluating args into locals. Only inline when args
    // are simple (identifier/literal), and substitute parameter identifiers
    // with the corresponding arg expression.
    for (int i = 0; i < arg_count; i++) {
        if (!expr_inline_substitution_safe(call_expr->call.args[i])) {
            return false;
        }
    }

    Stmt* body = func_decl->func_decl.body;
    Stmt* ret = body->block.statements[body->block.stmt_count - 1];

    int var_count = body->block.stmt_count - 1;
    bool* inline_var = NULL;

    Expr* ret_expr = expr_clone(ret->return_value);
    expr_substitute_params_in_place(&ret_expr, func_decl, call_expr->call.args, arg_count);

    // Inline vars used in the (evolving) return expression. Process in reverse so
    // inlining later vars can expose new opportunities for earlier vars (e.g. `ij`).
    if (var_count > 0) {
        inline_var = (bool*)safe_calloc((size_t)var_count, sizeof(bool));
        for (int i = var_count - 1; i >= 0; i--) {
            Stmt* var_stmt = body->block.statements[i];
            const char* name = var_stmt->var_decl.name;

            int uses_in_ret = expr_count_identifier(ret_expr, name);
            if (uses_in_ret <= 0) continue;

            // If a later var initializer that we will keep as a local depends on this var,
            // don't inline it away (would change semantics unless we also substitute there).
            int uses_in_noninlined_inits = 0;
            for (int j = i + 1; j < var_count; j++) {
                if (inline_var[j]) continue;
                uses_in_noninlined_inits += expr_count_identifier(body->block.statements[j]->var_decl.initializer, name);
            }
            if (uses_in_noninlined_inits != 0) continue;

            bool do_inline = false;
            if (uses_in_ret == 1) {
                do_inline = true;
            } else if (uses_in_ret <= 2) {
                int nodes = expr_node_count(var_stmt->var_decl.initializer);
                do_inline = (nodes <= 4);
            }

            if (!do_inline) continue;

            Expr* init_expr = expr_clone(var_stmt->var_decl.initializer);
            expr_substitute_params_in_place(&init_expr, func_decl, call_expr->call.args, arg_count);
            expr_substitute_identifier_in_place(&ret_expr, name, init_expr);
            expr_free(init_expr);
            inline_var[i] = true;
        }
    }

    // Emit remaining var decls as locals, preserving order.
    for (int i = 0; i < var_count; i++) {
        if (inline_var && inline_var[i]) continue;
        Stmt* var_stmt = body->block.statements[i];
        Expr* init = expr_clone(var_stmt->var_decl.initializer);
        expr_substitute_params_in_place(&init, func_decl, call_expr->call.args, arg_count);
        compile_expression(comp, init);
        expr_free(init);

        int local = add_local(comp, var_stmt->var_decl.name, var_stmt->line);
        if (local < 0) {
            if (inline_var) free(inline_var);
            expr_free(ret_expr);
            end_scope(comp, inline_scope_start);
            return false;
        }
        emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)local, var_stmt->line);
    }

    if (inline_var) free(inline_var);

    compile_expression(comp, ret_expr);
    expr_free(ret_expr);

    end_scope(comp, inline_scope_start);
    return true;
}

static Expr* fold_binary(Compiler* comp, Expr* expr) {
    char* file = comp && comp->file ? comp->file : expr->file;

    TokenType op = expr->binary.op;
    Expr* left_expr = expr->binary.left;
    Expr* right_expr = expr->binary.right;

    bool left_int_zero = left_expr &&
                         left_expr->kind == EXPR_LITERAL &&
                         left_expr->type &&
                         left_expr->type->kind == TYPE_INT &&
                         left_expr->literal.as_int == 0;
    bool right_int_zero = right_expr &&
                          right_expr->kind == EXPR_LITERAL &&
                          right_expr->type &&
                          right_expr->type->kind == TYPE_INT &&
                          right_expr->literal.as_int == 0;
    bool left_int_one = left_expr &&
                        left_expr->kind == EXPR_LITERAL &&
                        left_expr->type &&
                        left_expr->type->kind == TYPE_INT &&
                        left_expr->literal.as_int == 1;
    bool right_int_one = right_expr &&
                         right_expr->kind == EXPR_LITERAL &&
                         right_expr->type &&
                         right_expr->type->kind == TYPE_INT &&
                         right_expr->literal.as_int == 1;
    bool left_double_zero = left_expr &&
                            left_expr->kind == EXPR_LITERAL &&
                            left_expr->type &&
                            left_expr->type->kind == TYPE_DOUBLE &&
                            left_expr->literal.as_double == 0.0;
    bool right_double_zero = right_expr &&
                             right_expr->kind == EXPR_LITERAL &&
                             right_expr->type &&
                             right_expr->type->kind == TYPE_DOUBLE &&
                             right_expr->literal.as_double == 0.0;
    bool left_double_one = left_expr &&
                           left_expr->kind == EXPR_LITERAL &&
                           left_expr->type &&
                           left_expr->type->kind == TYPE_DOUBLE &&
                           left_expr->literal.as_double == 1.0;
    bool right_double_one = right_expr &&
                            right_expr->kind == EXPR_LITERAL &&
                            right_expr->type &&
                            right_expr->type->kind == TYPE_DOUBLE &&
                            right_expr->literal.as_double == 1.0;
    bool left_true = is_bool_literal(left_expr) && left_expr->literal.as_int != 0;
    bool left_false = is_bool_literal(left_expr) && left_expr->literal.as_int == 0;
    bool right_true = is_bool_literal(right_expr) && right_expr->literal.as_int != 0;
    bool right_false = is_bool_literal(right_expr) && right_expr->literal.as_int == 0;

    bool int_bin = left_expr && right_expr &&
                   left_expr->type && right_expr->type &&
                   left_expr->type->kind == TYPE_INT &&
                   right_expr->type->kind == TYPE_INT;
    bool double_bin = left_expr && right_expr &&
                      left_expr->type && right_expr->type &&
                      left_expr->type->kind == TYPE_DOUBLE &&
                      right_expr->type->kind == TYPE_DOUBLE;
    bool bool_bin = left_expr && right_expr &&
                    left_expr->type && right_expr->type &&
                    left_expr->type->kind == TYPE_BOOL &&
                    right_expr->type->kind == TYPE_BOOL;

    // Algebraic/boolean identities that keep operand evaluation semantics.
    if (op == TOKEN_PLUS) {
        bool left_empty_string = is_string_literal(left_expr) &&
                                 (!left_expr->literal.as_string || left_expr->literal.as_string[0] == '\0');
        bool right_empty_string = is_string_literal(right_expr) &&
                                  (!right_expr->literal.as_string || right_expr->literal.as_string[0] == '\0');
        if (int_bin) {
            if (right_int_zero) return expr_clone(left_expr);
            if (left_int_zero) return expr_clone(right_expr);
        } else if (double_bin) {
            if (right_double_zero) return expr_clone(left_expr);
            if (left_double_zero) return expr_clone(right_expr);
        } else if (left_empty_string && right_expr->type && right_expr->type->kind == TYPE_STRING) {
            return expr_clone(right_expr);
        } else if (right_empty_string && left_expr->type && left_expr->type->kind == TYPE_STRING) {
            return expr_clone(left_expr);
        }
    } else if (op == TOKEN_MINUS) {
        if (int_bin && right_int_zero) return expr_clone(left_expr);
        if (double_bin && right_double_zero) return expr_clone(left_expr);
    } else if (op == TOKEN_STAR) {
        if (int_bin) {
            if (right_int_one) return expr_clone(left_expr);
            if (left_int_one) return expr_clone(right_expr);
        } else if (double_bin) {
            if (right_double_one) return expr_clone(left_expr);
            if (left_double_one) return expr_clone(right_expr);
        }
    } else if (op == TOKEN_SLASH) {
        if (int_bin && right_int_one) return expr_clone(left_expr);
        if (double_bin && right_double_one) return expr_clone(left_expr);
    } else if (op == TOKEN_AND && bool_bin) {
        if (left_false) return expr_create_literal_bool(false, file, expr->line, expr->column);
        if (left_true) return expr_clone(right_expr);
        if (right_true) return expr_clone(left_expr);
    } else if (op == TOKEN_OR && bool_bin) {
        if (left_true) return expr_create_literal_bool(true, file, expr->line, expr->column);
        if (left_false) return expr_clone(right_expr);
        if (right_false) return expr_clone(left_expr);
    }

    if (!is_literal(left_expr) || !is_literal(right_expr)) {
        return NULL;
    }
    if (is_bigint_literal(left_expr) || is_bigint_literal(right_expr)) {
        return NULL;
    }

    // Handle nil-only folds for equality/inequality.
    if (left_expr->kind == EXPR_NIL || right_expr->kind == EXPR_NIL) {
        if (op == TOKEN_EQ_EQ || op == TOKEN_BANG_EQ) {
            bool eq = (left_expr->kind == EXPR_NIL) && (right_expr->kind == EXPR_NIL);
            bool res = (op == TOKEN_EQ_EQ) ? eq : !eq;
            return expr_create_literal_bool(res, file, expr->line, expr->column);
        }
        return NULL;
    }

    // Equality/inequality folds (int/bool/double/string)
    if (op == TOKEN_EQ_EQ || op == TOKEN_BANG_EQ) {
        bool eq = false;
        if (is_string_literal(left_expr) && is_string_literal(right_expr)) {
            const char* l = left_expr->literal.as_string ? left_expr->literal.as_string : "";
            const char* r = right_expr->literal.as_string ? right_expr->literal.as_string : "";
            eq = strcmp(l, r) == 0;
        } else if (is_double_literal(left_expr) && is_double_literal(right_expr)) {
            eq = left_expr->literal.as_double == right_expr->literal.as_double;
        } else if (is_bool_literal(left_expr) && is_bool_literal(right_expr)) {
            eq = left_expr->literal.as_int == right_expr->literal.as_int;
        } else if (left_expr->type && right_expr->type &&
                   left_expr->type->kind == TYPE_INT && right_expr->type->kind == TYPE_INT) {
            eq = left_expr->literal.as_int == right_expr->literal.as_int;
        } else {
            // Different literal kinds (e.g., int vs double) are not equal at runtime.
            eq = false;
        }

        bool res = (op == TOKEN_EQ_EQ) ? eq : !eq;
        return expr_create_literal_bool(res, file, expr->line, expr->column);
    }

    // Boolean ops (bool-only)
    if (op == TOKEN_AND || op == TOKEN_OR) {
        if (!is_bool_literal(left_expr) || !is_bool_literal(right_expr)) {
            return NULL;
        }
        bool left_true = left_expr->literal.as_int != 0;
        bool right_true = right_expr->literal.as_int != 0;
        bool res = (op == TOKEN_AND) ? (left_true && right_true) : (left_true || right_true);
        return expr_create_literal_bool(res, file, expr->line, expr->column);
    }

    // Bitwise ops (int-only)
    if (op == TOKEN_BIT_AND || op == TOKEN_BIT_OR || op == TOKEN_BIT_XOR) {
        if (!left_expr->type || !right_expr->type ||
            left_expr->type->kind != TYPE_INT || right_expr->type->kind != TYPE_INT) {
            return NULL;
        }
        int64_t left = left_expr->literal.as_int;
        int64_t right = right_expr->literal.as_int;
        int64_t int_result = 0;
        switch (op) {
            case TOKEN_BIT_AND: int_result = left & right; break;
            case TOKEN_BIT_OR: int_result = left | right; break;
            case TOKEN_BIT_XOR: int_result = left ^ right; break;
            default: return NULL;
        }
        return expr_create_literal_int(int_result, file, expr->line, expr->column);
    }

    // Comparisons (require both operands to be the same numeric kind)
    if (op == TOKEN_LT || op == TOKEN_LT_EQ || op == TOKEN_GT || op == TOKEN_GT_EQ) {
        if (is_double_literal(left_expr) != is_double_literal(right_expr)) {
            return NULL;
        }

        bool res = false;
        if (is_double_literal(left_expr)) {
            double l = left_expr->literal.as_double;
            double r = right_expr->literal.as_double;
            switch (op) {
                case TOKEN_LT: res = l < r; break;
                case TOKEN_LT_EQ: res = l <= r; break;
                case TOKEN_GT: res = l > r; break;
                case TOKEN_GT_EQ: res = l >= r; break;
                default: return NULL;
            }
        } else {
            if (!left_expr->type || !right_expr->type ||
                left_expr->type->kind != TYPE_INT || right_expr->type->kind != TYPE_INT) {
                return NULL;
            }
            int64_t l = left_expr->literal.as_int;
            int64_t r = right_expr->literal.as_int;
            switch (op) {
                case TOKEN_LT: res = l < r; break;
                case TOKEN_LT_EQ: res = l <= r; break;
                case TOKEN_GT: res = l > r; break;
                case TOKEN_GT_EQ: res = l >= r; break;
                default: return NULL;
            }
        }

        return expr_create_literal_bool(res, file, expr->line, expr->column);
    }

    // Arithmetic folds
    if (op == TOKEN_PLUS || op == TOKEN_MINUS || op == TOKEN_STAR || op == TOKEN_SLASH || op == TOKEN_PERCENT) {
        // Fold string literal concatenation at compile-time.
        if (op == TOKEN_PLUS && is_string_literal(left_expr) && is_string_literal(right_expr)) {
            const char* left = left_expr->literal.as_string ? left_expr->literal.as_string : "";
            const char* right = right_expr->literal.as_string ? right_expr->literal.as_string : "";
            size_t left_len = strlen(left);
            size_t right_len = strlen(right);
            char* combined = (char*)safe_malloc(left_len + right_len + 1);
            memcpy(combined, left, left_len);
            memcpy(combined + left_len, right, right_len);
            combined[left_len + right_len] = '\0';

            Expr* folded = expr_create_literal_string(combined, file, expr->line, expr->column);
            free(combined);
            return folded;
        }

        // Never treat string literals as numeric literals for folding.
        if (is_string_literal(left_expr) || is_string_literal(right_expr)) {
            return NULL;
        }

        bool use_double = is_double_literal(left_expr) || is_double_literal(right_expr);

        if (op == TOKEN_PERCENT && use_double) {
            return NULL;
        }

        if (!use_double) {
            int64_t left = left_expr->literal.as_int;
            int64_t right = right_expr->literal.as_int;
            int64_t int_result = 0;

            switch (op) {
                case TOKEN_PLUS: int_result = left + right; break;
                case TOKEN_MINUS: int_result = left - right; break;
                case TOKEN_STAR: int_result = left * right; break;
                case TOKEN_SLASH:
                    if (right == 0) return NULL;
                    int_result = left / right;
                    break;
                case TOKEN_PERCENT:
                    if (right == 0) return NULL;
                    int_result = left % right;
                    break;
                default:
                    return NULL;
            }

            return expr_create_literal_int(int_result, file, expr->line, expr->column);
        }

        // Mixed numeric types aren't supported at runtime; only fold double-double.
        if (!is_double_literal(left_expr) || !is_double_literal(right_expr)) {
            return NULL;
        }

        double left = left_expr->literal.as_double;
        double right = right_expr->literal.as_double;
        double double_result = 0.0;

        switch (op) {
            case TOKEN_PLUS: double_result = left + right; break;
            case TOKEN_MINUS: double_result = left - right; break;
            case TOKEN_STAR: double_result = left * right; break;
            case TOKEN_SLASH:
                if (right == 0.0) return NULL;
                double_result = left / right;
                break;
            default:
                return NULL;
        }

        return expr_create_literal_double(double_result, file, expr->line, expr->column);
    }

    return NULL;
}

static Expr* fold_unary(Compiler* comp, Expr* expr) {
    if (!is_literal(expr->unary.operand)) {
        return NULL;
    }
    if (is_bigint_literal(expr->unary.operand)) {
        return NULL;
    }

    if (expr->unary.operand->kind == EXPR_NIL) {
        return NULL;
    }

    TokenType op = expr->unary.op;

    if (op == TOKEN_NOT) {
        if (!is_bool_literal(expr->unary.operand)) {
            return NULL;
        }
        bool v = expr->unary.operand->literal.as_int != 0;
        return expr_create_literal_bool(!v, comp && comp->file ? comp->file : expr->file, expr->line, expr->column);
    }

    if (op == TOKEN_BIT_NOT) {
        if (!expr->unary.operand->type || expr->unary.operand->type->kind != TYPE_INT) {
            return NULL;
        }
        int64_t v = expr->unary.operand->literal.as_int;
        return expr_create_literal_int(~v, comp && comp->file ? comp->file : expr->file, expr->line, expr->column);
    }

    if (op != TOKEN_MINUS) {
        return NULL;
    }

    if (expr->unary.operand->type && expr->unary.operand->type->kind == TYPE_DOUBLE) {
        double value = expr->unary.operand->literal.as_double;
        return expr_create_literal_double(-value, comp && comp->file ? comp->file : expr->file, expr->line, expr->column);
    }

    if (expr->unary.operand->type && expr->unary.operand->type->kind == TYPE_INT) {
        int64_t value = expr->unary.operand->literal.as_int;
        return expr_create_literal_int(-value, comp && comp->file ? comp->file : expr->file, expr->line, expr->column);
    }

    return NULL;
}

static Expr* fold_expression_recursive(Compiler* comp, Expr* expr) {
    if (!expr) return NULL;

    if (expr->kind == EXPR_BINARY) {
        Expr* folded_left = fold_expression_recursive(comp, expr->binary.left);
        Expr* folded_right = fold_expression_recursive(comp, expr->binary.right);

        Expr temp = *expr;
        if (folded_left) temp.binary.left = folded_left;
        if (folded_right) temp.binary.right = folded_right;

        Expr* folded = fold_binary(comp, &temp);

        if (folded_left) expr_free(folded_left);
        if (folded_right) expr_free(folded_right);

        return folded;
    }

    if (expr->kind == EXPR_UNARY) {
        Expr* folded_operand = fold_expression_recursive(comp, expr->unary.operand);
        Expr temp = *expr;
        if (folded_operand) temp.unary.operand = folded_operand;

        Expr* folded = fold_unary(comp, &temp);
        if (folded_operand) expr_free(folded_operand);
        return folded;
    }

    if (expr->kind == EXPR_CAST) {
        if (!expr->cast.value || !expr->cast.target_type) return NULL;

        Expr* folded_value = fold_expression_recursive(comp, expr->cast.value);
        Expr* value_expr = folded_value ? folded_value : expr->cast.value;
        if (value_expr &&
            value_expr->type &&
            is_noop_primitive_cast(value_expr->type, expr->cast.target_type)) {
            if (folded_value) return folded_value;
            return expr_clone(value_expr);
        }

        if (!folded_value) return NULL;

        Type* target_clone = type_clone(expr->cast.target_type);
        Expr* folded_cast = expr_create_cast(folded_value,
                                             target_clone,
                                             comp && comp->file ? comp->file : expr->file,
                                             expr->line,
                                             expr->column);
        return folded_cast;
    }

    return NULL;
}

static void ir_statement_list_init(IRStatementList* ir) {
    if (!ir) return;
    ir->statements = NULL;
    ir->stmt_count = 0;
    ir->stmt_capacity = 0;
}

static void ir_statement_list_free(IRStatementList* ir) {
    if (!ir) return;
    if (ir->statements) free(ir->statements);
    ir->statements = NULL;
    ir->stmt_count = 0;
    ir->stmt_capacity = 0;
}

static void ir_statement_list_append(IRStatementList* ir, Stmt* stmt) {
    if (!ir || !stmt) return;
    ir->stmt_count++;
    if (ir->stmt_count > ir->stmt_capacity) {
        int next_capacity = ir->stmt_capacity > 0 ? ir->stmt_capacity * 2 : 8;
        if (next_capacity < ir->stmt_count) next_capacity = ir->stmt_count;
        ir->stmt_capacity = next_capacity;
        ir->statements = (Stmt**)safe_realloc(ir->statements, (size_t)ir->stmt_capacity * sizeof(Stmt*));
    }
    ir->statements[ir->stmt_count - 1] = stmt;
}

static void ir_basic_block_list_init(IRBasicBlockList* blocks) {
    if (!blocks) return;
    blocks->blocks = NULL;
    blocks->count = 0;
    blocks->capacity = 0;
}

static void ir_basic_block_list_free(IRBasicBlockList* blocks) {
    if (!blocks) return;
    if (blocks->blocks) {
        for (int i = 0; i < blocks->count; i++) {
            if (blocks->blocks[i].statements) free(blocks->blocks[i].statements);
            if (blocks->blocks[i].successors) free(blocks->blocks[i].successors);
            if (blocks->blocks[i].predecessors) free(blocks->blocks[i].predecessors);
        }
        free(blocks->blocks);
    }
    blocks->blocks = NULL;
    blocks->count = 0;
    blocks->capacity = 0;
}

static void ir_block_exit_list_init(IRBlockExitList* exits) {
    if (!exits) return;
    exits->block_indices = NULL;
    exits->count = 0;
    exits->capacity = 0;
}

static void ir_block_exit_list_free(IRBlockExitList* exits) {
    if (!exits) return;
    if (exits->block_indices) free(exits->block_indices);
    exits->block_indices = NULL;
    exits->count = 0;
    exits->capacity = 0;
}

static void ir_block_exit_list_append(IRBlockExitList* exits, int block_index) {
    if (!exits || block_index < 0) return;
    ir_basic_block_edge_list_append(&exits->block_indices,
                                    &exits->count,
                                    &exits->capacity,
                                    block_index);
}

static void ir_block_exit_list_append_all(IRBlockExitList* dst, const IRBlockExitList* src) {
    if (!dst || !src) return;
    for (int i = 0; i < src->count; i++) {
        ir_block_exit_list_append(dst, src->block_indices[i]);
    }
}

static void ir_structured_block_result_init(IRStructuredBlockResult* result) {
    if (!result) return;
    result->entry_block = -1;
    ir_block_exit_list_init(&result->fallthrough_exits);
    ir_block_exit_list_init(&result->break_exits);
    ir_block_exit_list_init(&result->continue_exits);
}

static void ir_structured_block_result_free(IRStructuredBlockResult* result) {
    if (!result) return;
    ir_block_exit_list_free(&result->fallthrough_exits);
    ir_block_exit_list_free(&result->break_exits);
    ir_block_exit_list_free(&result->continue_exits);
    result->entry_block = -1;
}

static void ir_basic_block_list_append(IRBasicBlockList* blocks,
                                       Stmt** statements,
                                       int statement_count,
                                       int start_index,
                                       int end_index,
                                       IRBlockTerminatorKind terminator_kind) {
    int slice_count = 0;
    if (!blocks || !statements || statement_count <= 0 || start_index < 0 ||
        end_index < start_index || end_index >= statement_count) {
        return;
    }
    slice_count = end_index - start_index + 1;
    if (slice_count <= 0) return;
    if (blocks->count >= blocks->capacity) {
        int next_capacity = blocks->capacity > 0 ? blocks->capacity * 2 : 8;
        blocks->blocks = (IRBasicBlock*)safe_realloc(blocks->blocks,
                                                     (size_t)next_capacity * sizeof(IRBasicBlock));
        blocks->capacity = next_capacity;
    }
    blocks->blocks[blocks->count].statements =
        (Stmt**)safe_calloc((size_t)slice_count, sizeof(Stmt*));
    memcpy(blocks->blocks[blocks->count].statements,
           statements + start_index,
           (size_t)slice_count * sizeof(Stmt*));
    blocks->blocks[blocks->count].statement_count = slice_count;
    blocks->blocks[blocks->count].start_index = 0;
    blocks->blocks[blocks->count].end_index = slice_count - 1;
    blocks->blocks[blocks->count].terminator_kind = terminator_kind;
    blocks->blocks[blocks->count].successors = NULL;
    blocks->blocks[blocks->count].successor_count = 0;
    blocks->blocks[blocks->count].successor_capacity = 0;
    blocks->blocks[blocks->count].predecessors = NULL;
    blocks->blocks[blocks->count].predecessor_count = 0;
    blocks->blocks[blocks->count].predecessor_capacity = 0;
    blocks->count++;
}

static void ir_basic_block_edge_list_append(int** io_edges,
                                            int* io_count,
                                            int* io_capacity,
                                            int edge_index) {
    if (!io_edges || !io_count || !io_capacity || edge_index < 0) return;
    for (int i = 0; i < *io_count; i++) {
        if ((*io_edges)[i] == edge_index) return;
    }
    if (*io_count >= *io_capacity) {
        int next_capacity = *io_capacity > 0 ? *io_capacity * 2 : 4;
        *io_edges = (int*)safe_realloc(*io_edges, (size_t)next_capacity * sizeof(int));
        *io_capacity = next_capacity;
    }
    (*io_edges)[(*io_count)++] = edge_index;
}

static void ir_basic_block_list_add_edge(IRBasicBlockList* blocks, int from_index, int to_index) {
    if (!blocks || !blocks->blocks) return;
    if (from_index < 0 || from_index >= blocks->count) return;
    if (to_index < 0 || to_index >= blocks->count) return;
    ir_basic_block_edge_list_append(&blocks->blocks[from_index].successors,
                                    &blocks->blocks[from_index].successor_count,
                                    &blocks->blocks[from_index].successor_capacity,
                                    to_index);
    ir_basic_block_edge_list_append(&blocks->blocks[to_index].predecessors,
                                    &blocks->blocks[to_index].predecessor_count,
                                    &blocks->blocks[to_index].predecessor_capacity,
                                    from_index);
}

static void ir_worklist_enqueue(int** io_worklist,
                                int* io_tail,
                                int* io_capacity,
                                bool* queued,
                                int block_index) {
    if (!io_worklist || !io_tail || !io_capacity || !queued || block_index < 0) return;
    if (queued[block_index]) return;
    if (*io_tail >= *io_capacity) {
        int next_capacity = *io_capacity > 0 ? *io_capacity * 2 : 8;
        *io_worklist =
            (int*)safe_realloc(*io_worklist, (size_t)next_capacity * sizeof(int));
        *io_capacity = next_capacity;
    }
    (*io_worklist)[(*io_tail)++] = block_index;
    queued[block_index] = true;
}

static bool ir_block_terminator_may_fallthrough(IRBlockTerminatorKind terminator_kind) {
    switch (terminator_kind) {
        case IR_BLOCK_TERM_RETURN:
        case IR_BLOCK_TERM_BREAK:
        case IR_BLOCK_TERM_CONTINUE:
            return false;
        default:
            return true;
    }
}

static IRBlockTerminatorKind ir_stmt_basic_block_terminator_kind(Stmt* stmt) {
    if (!stmt) return IR_BLOCK_TERM_END;

    switch (stmt->kind) {
        case STMT_RETURN:
            return IR_BLOCK_TERM_RETURN;
        case STMT_BREAK:
            return IR_BLOCK_TERM_BREAK;
        case STMT_CONTINUE:
            return IR_BLOCK_TERM_CONTINUE;
        case STMT_IF:
            return IR_BLOCK_TERM_BRANCH;
        case STMT_MATCH:
            return IR_BLOCK_TERM_MATCH;
        case STMT_WHILE:
        case STMT_FOREACH:
        case STMT_FOR_RANGE:
            return IR_BLOCK_TERM_LOOP;
        case STMT_BLOCK:
        case STMT_EXPR:
        case STMT_VAR_TUPLE_DECL:
        case STMT_ASSIGN_INDEX:
        case STMT_ASSIGN_FIELD:
        case STMT_DEFER:
            return IR_BLOCK_TERM_BARRIER;
        default:
            return IR_BLOCK_TERM_END;
    }
}

static void ir_stmt_as_statement_view(Stmt* stmt,
                                      IRStatementList* view,
                                      Stmt** single_stmt_storage) {
    if (!view) return;
    view->statements = NULL;
    view->stmt_count = 0;
    view->stmt_capacity = 0;
    if (!stmt) return;

    if (stmt->kind == STMT_BLOCK) {
        view->statements = stmt->block.statements;
        view->stmt_count = stmt->block.stmt_count;
        view->stmt_capacity = stmt->block.stmt_count;
        return;
    }

    if (!single_stmt_storage) return;
    single_stmt_storage[0] = stmt;
    view->statements = single_stmt_storage;
    view->stmt_count = 1;
    view->stmt_capacity = 1;
}

static IRStructuredBlockResult ir_build_basic_blocks_for_range(IRBasicBlockList* blocks,
                                                               Stmt** statements,
                                                               int statement_count,
                                                               int start_index,
                                                               int end_index) {
    IRStructuredBlockResult result;
    ir_structured_block_result_init(&result);
    if (!blocks || !statements || statement_count <= 0 || start_index < 0 ||
        end_index < start_index || end_index >= statement_count) {
        return result;
    }

    IRBlockExitList pending_fallthrough;
    ir_block_exit_list_init(&pending_fallthrough);

    int current_start = start_index;
    while (current_start <= end_index) {
        int current_end = current_start;
        IRBlockTerminatorKind terminator_kind = IR_BLOCK_TERM_END;
        for (; current_end <= end_index; current_end++) {
            terminator_kind = ir_stmt_basic_block_terminator_kind(statements[current_end]);
            if (terminator_kind != IR_BLOCK_TERM_END || current_end == end_index) {
                break;
            }
        }

        if (terminator_kind == IR_BLOCK_TERM_LOOP && current_end > current_start) {
            ir_basic_block_list_append(blocks,
                                       statements,
                                       statement_count,
                                       current_start,
                                       current_end - 1,
                                       IR_BLOCK_TERM_END);
            int preheader_block_index = blocks->count - 1;
            if (result.entry_block < 0) result.entry_block = preheader_block_index;

            for (int i = 0; i < pending_fallthrough.count; i++) {
                ir_basic_block_list_add_edge(blocks,
                                             pending_fallthrough.block_indices[i],
                                             preheader_block_index);
            }
            ir_block_exit_list_free(&pending_fallthrough);
            ir_block_exit_list_init(&pending_fallthrough);
            ir_block_exit_list_append(&pending_fallthrough, preheader_block_index);

            current_start = current_end;
            continue;
        }

        ir_basic_block_list_append(blocks,
                                   statements,
                                   statement_count,
                                   current_start,
                                   current_end,
                                   terminator_kind);
        int block_index = blocks->count - 1;
        if (result.entry_block < 0) result.entry_block = block_index;

        for (int i = 0; i < pending_fallthrough.count; i++) {
            ir_basic_block_list_add_edge(blocks, pending_fallthrough.block_indices[i], block_index);
        }
        ir_block_exit_list_free(&pending_fallthrough);
        ir_block_exit_list_init(&pending_fallthrough);

        Stmt* terminator_stmt = statements[current_end];
        switch (terminator_kind) {
            case IR_BLOCK_TERM_END:
            case IR_BLOCK_TERM_BARRIER:
                ir_block_exit_list_append(&pending_fallthrough, block_index);
                break;
            case IR_BLOCK_TERM_RETURN:
                break;
            case IR_BLOCK_TERM_BREAK:
                ir_block_exit_list_append(&result.break_exits, block_index);
                break;
            case IR_BLOCK_TERM_CONTINUE:
                ir_block_exit_list_append(&result.continue_exits, block_index);
                break;
            case IR_BLOCK_TERM_BRANCH: {
                IRStatementList then_view;
                IRStatementList else_view;
                Stmt* then_single_stmt[1];
                Stmt* else_single_stmt[1];
                ir_stmt_as_statement_view(terminator_stmt->if_stmt.then_branch,
                                          &then_view,
                                          then_single_stmt);
                ir_stmt_as_statement_view(terminator_stmt->if_stmt.else_branch,
                                          &else_view,
                                          else_single_stmt);

                if (then_view.stmt_count > 0) {
                    IRStructuredBlockResult then_result =
                        ir_build_basic_blocks_for_range(blocks,
                                                        then_view.statements,
                                                        then_view.stmt_count,
                                                        0,
                                                        then_view.stmt_count - 1);
                    if (then_result.entry_block >= 0) {
                        ir_basic_block_list_add_edge(blocks, block_index, then_result.entry_block);
                    } else {
                        ir_block_exit_list_append(&pending_fallthrough, block_index);
                    }
                    ir_block_exit_list_append_all(&pending_fallthrough, &then_result.fallthrough_exits);
                    ir_block_exit_list_append_all(&result.break_exits, &then_result.break_exits);
                    ir_block_exit_list_append_all(&result.continue_exits, &then_result.continue_exits);
                    ir_structured_block_result_free(&then_result);
                } else {
                    ir_block_exit_list_append(&pending_fallthrough, block_index);
                }

                if (else_view.stmt_count > 0) {
                    IRStructuredBlockResult else_result =
                        ir_build_basic_blocks_for_range(blocks,
                                                        else_view.statements,
                                                        else_view.stmt_count,
                                                        0,
                                                        else_view.stmt_count - 1);
                    if (else_result.entry_block >= 0) {
                        ir_basic_block_list_add_edge(blocks, block_index, else_result.entry_block);
                    } else {
                        ir_block_exit_list_append(&pending_fallthrough, block_index);
                    }
                    ir_block_exit_list_append_all(&pending_fallthrough, &else_result.fallthrough_exits);
                    ir_block_exit_list_append_all(&result.break_exits, &else_result.break_exits);
                    ir_block_exit_list_append_all(&result.continue_exits, &else_result.continue_exits);
                    ir_structured_block_result_free(&else_result);
                } else {
                    ir_block_exit_list_append(&pending_fallthrough, block_index);
                }
                break;
            }
            case IR_BLOCK_TERM_MATCH: {
                bool added_else_path = false;
                for (int arm = 0; arm < terminator_stmt->match_stmt.arm_count; arm++) {
                    IRStatementList arm_view;
                    Stmt* arm_single_stmt[1];
                    Stmt* arm_body = terminator_stmt->match_stmt.bodies
                                         ? terminator_stmt->match_stmt.bodies[arm]
                                         : NULL;
                    ir_stmt_as_statement_view(arm_body, &arm_view, arm_single_stmt);
                    if (arm_view.stmt_count > 0) {
                        IRStructuredBlockResult arm_result =
                            ir_build_basic_blocks_for_range(blocks,
                                                            arm_view.statements,
                                                            arm_view.stmt_count,
                                                            0,
                                                            arm_view.stmt_count - 1);
                        if (arm_result.entry_block >= 0) {
                            ir_basic_block_list_add_edge(blocks, block_index, arm_result.entry_block);
                        } else {
                            ir_block_exit_list_append(&pending_fallthrough, block_index);
                        }
                        ir_block_exit_list_append_all(&pending_fallthrough, &arm_result.fallthrough_exits);
                        ir_block_exit_list_append_all(&result.break_exits, &arm_result.break_exits);
                        ir_block_exit_list_append_all(&result.continue_exits, &arm_result.continue_exits);
                        ir_structured_block_result_free(&arm_result);
                    } else {
                        ir_block_exit_list_append(&pending_fallthrough, block_index);
                    }
                }

                IRStatementList else_view;
                Stmt* else_single_stmt[1];
                ir_stmt_as_statement_view(terminator_stmt->match_stmt.else_branch,
                                          &else_view,
                                          else_single_stmt);
                if (else_view.stmt_count > 0) {
                    IRStructuredBlockResult else_result =
                        ir_build_basic_blocks_for_range(blocks,
                                                        else_view.statements,
                                                        else_view.stmt_count,
                                                        0,
                                                        else_view.stmt_count - 1);
                    if (else_result.entry_block >= 0) {
                        ir_basic_block_list_add_edge(blocks, block_index, else_result.entry_block);
                        added_else_path = true;
                    } else {
                        ir_block_exit_list_append(&pending_fallthrough, block_index);
                        added_else_path = true;
                    }
                    ir_block_exit_list_append_all(&pending_fallthrough, &else_result.fallthrough_exits);
                    ir_block_exit_list_append_all(&result.break_exits, &else_result.break_exits);
                    ir_block_exit_list_append_all(&result.continue_exits, &else_result.continue_exits);
                    ir_structured_block_result_free(&else_result);
                }
                if (!added_else_path) {
                    ir_block_exit_list_append(&pending_fallthrough, block_index);
                }
                break;
            }
            case IR_BLOCK_TERM_LOOP: {
                ir_block_exit_list_append(&pending_fallthrough, block_index);

                IRStatementList body_view;
                Stmt* body_single_stmt[1];
                if (terminator_stmt->kind == STMT_WHILE) {
                    ir_stmt_as_statement_view(terminator_stmt->while_stmt.body,
                                              &body_view,
                                              body_single_stmt);
                } else if (terminator_stmt->kind == STMT_FOREACH) {
                    ir_stmt_as_statement_view(terminator_stmt->foreach.body,
                                              &body_view,
                                              body_single_stmt);
                } else {
                    ir_stmt_as_statement_view(terminator_stmt->for_range.body,
                                              &body_view,
                                              body_single_stmt);
                }

                if (body_view.stmt_count > 0) {
                    IRStructuredBlockResult body_result =
                        ir_build_basic_blocks_for_range(blocks,
                                                        body_view.statements,
                                                        body_view.stmt_count,
                                                        0,
                                                        body_view.stmt_count - 1);
                    if (body_result.entry_block >= 0) {
                        ir_basic_block_list_add_edge(blocks, block_index, body_result.entry_block);
                    }
                    for (int i = 0; i < body_result.fallthrough_exits.count; i++) {
                        ir_basic_block_list_add_edge(blocks,
                                                     body_result.fallthrough_exits.block_indices[i],
                                                     block_index);
                    }
                    for (int i = 0; i < body_result.continue_exits.count; i++) {
                        ir_basic_block_list_add_edge(blocks,
                                                     body_result.continue_exits.block_indices[i],
                                                     block_index);
                    }
                    ir_block_exit_list_append_all(&pending_fallthrough, &body_result.break_exits);
                    ir_structured_block_result_free(&body_result);
                }
                break;
            }
        }

        current_start = current_end + 1;
    }

    ir_block_exit_list_append_all(&result.fallthrough_exits, &pending_fallthrough);
    ir_block_exit_list_free(&pending_fallthrough);
    return result;
}

static IRBasicBlockList ir_build_basic_blocks(IRStatementList* ir) {
    IRBasicBlockList blocks;
    ir_basic_block_list_init(&blocks);

    if (!ir || !ir->statements || ir->stmt_count <= 0) {
        return blocks;
    }

    IRStructuredBlockResult result =
        ir_build_basic_blocks_for_range(&blocks, ir->statements, ir->stmt_count, 0, ir->stmt_count - 1);
    ir_structured_block_result_free(&result);

    return blocks;
}

static void ir_clone_statement_range(IRStatementList* dst,
                                     Stmt** statements,
                                     int statement_count,
                                     int start_index,
                                     int end_index) {
    if (!dst) return;

    dst->statements = NULL;
    dst->stmt_count = 0;
    dst->stmt_capacity = 0;
    if (!statements || statement_count <= 0 || start_index < 0 || end_index < start_index ||
        end_index >= statement_count) {
        return;
    }

    int stmt_count = end_index - start_index + 1;
    dst->statements = (Stmt**)safe_calloc((size_t)stmt_count, sizeof(Stmt*));
    dst->stmt_count = stmt_count;
    dst->stmt_capacity = stmt_count;

    for (int i = 0; i < stmt_count; i++) {
        dst->statements[i] = stmt_clone(statements[start_index + i]);
    }
}

static void ir_free_cloned_statement_range(IRStatementList* ir) {
    if (!ir) return;
    if (ir->statements) {
        for (int i = 0; i < ir->stmt_count; i++) {
            stmt_free(ir->statements[i]);
        }
        free(ir->statements);
    }
    ir->statements = NULL;
    ir->stmt_count = 0;
    ir->stmt_capacity = 0;
}

struct IRCseCandidate {
    char* var_name;
    Expr* value_expr;
    char** dependencies;
    int dependency_count;
};

static void ir_cse_invalidate_for_stmt_fallthrough(Stmt* stmt,
                                                   IRCseCandidate* candidates,
                                                   int* candidate_count,
                                                   IRCopyAlias* aliases,
                                                   int* alias_count);

static bool ir_cse_expr_eligible(Expr* expr) {
    return expr_inline_eligible(expr);
}

static bool ir_cse_expr_binary_is_commutative(Expr* expr) {
    if (!expr || expr->kind != EXPR_BINARY || !expr->type) return false;

    switch (expr->binary.op) {
        case TOKEN_PLUS:
        case TOKEN_STAR:
            return expr->type->kind == TYPE_INT || expr->type->kind == TYPE_DOUBLE;
        case TOKEN_BIT_AND:
        case TOKEN_BIT_OR:
        case TOKEN_BIT_XOR:
            return expr->type->kind == TYPE_INT;
        default:
            return false;
    }
}

static bool ir_exprs_value_equivalent(Expr* a, Expr* b) {
    if (expr_equals(a, b)) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        case EXPR_BINARY:
            if (a->binary.op != b->binary.op) return false;
            if (ir_cse_expr_binary_is_commutative(a) && ir_cse_expr_binary_is_commutative(b)) {
                return (ir_exprs_value_equivalent(a->binary.left, b->binary.left) &&
                        ir_exprs_value_equivalent(a->binary.right, b->binary.right)) ||
                       (ir_exprs_value_equivalent(a->binary.left, b->binary.right) &&
                        ir_exprs_value_equivalent(a->binary.right, b->binary.left));
            }
            return ir_exprs_value_equivalent(a->binary.left, b->binary.left) &&
                   ir_exprs_value_equivalent(a->binary.right, b->binary.right);
        case EXPR_UNARY:
            if (a->unary.op != b->unary.op) return false;
            return ir_exprs_value_equivalent(a->unary.operand, b->unary.operand);
        case EXPR_AWAIT:
            return ir_exprs_value_equivalent(a->await_expr.expr, b->await_expr.expr);
        case EXPR_CAST:
            if (!a->cast.target_type || !b->cast.target_type) return false;
            if (a->cast.target_type->kind != b->cast.target_type->kind) return false;
            return ir_exprs_value_equivalent(a->cast.value, b->cast.value);
        default:
            return false;
    }
}

static Expr* ir_clone_normalized_value_expr(Expr* expr,
                                            const IRCopyAlias* aliases,
                                            int alias_count) {
    if (!expr) return NULL;
    Expr* normalized = expr_clone(expr);
    if (!normalized) return NULL;
    ir_rewrite_expr_with_aliases(normalized, aliases, alias_count);
    return normalized;
}

static bool ir_string_list_contains(char** names, int count, const char* value) {
    if (!names || count <= 0 || !value) return false;
    for (int i = 0; i < count; i++) {
        if (names[i] && strcmp(names[i], value) == 0) return true;
    }
    return false;
}

static void ir_string_list_add_unique_owned(char*** io_names,
                                            int* io_count,
                                            int* io_capacity,
                                            const char* value) {
    if (!io_names || !io_count || !io_capacity || !value) return;
    if (ir_string_list_contains(*io_names, *io_count, value)) return;

    (*io_count)++;
    if (*io_count > *io_capacity) {
        int next_capacity = *io_capacity > 0 ? *io_capacity * 2 : 8;
        if (next_capacity < *io_count) next_capacity = *io_count;
        *io_capacity = next_capacity;
        *io_names = (char**)safe_realloc(*io_names, (size_t)(*io_capacity) * sizeof(char*));
    }
    (*io_names)[*io_count - 1] = safe_strdup(value);
}

static void ir_string_list_remove_owned(char** names, int* count, const char* value) {
    if (!names || !count || *count <= 0 || !value) return;
    for (int i = 0; i < *count; i++) {
        if (!names[i] || strcmp(names[i], value) != 0) continue;
        free(names[i]);
        for (int j = i + 1; j < *count; j++) {
            names[j - 1] = names[j];
        }
        (*count)--;
        return;
    }
}

static void ir_string_list_free_owned(char** names, int count) {
    if (!names || count <= 0) return;
    for (int i = 0; i < count; i++) {
        if (names[i]) free(names[i]);
    }
}

static void ir_collect_expr_identifiers(Expr* expr, char*** io_names, int* io_count, int* io_capacity) {
    if (!expr || !io_names || !io_count || !io_capacity) return;

    if (expr->kind == EXPR_IDENTIFIER && expr->identifier) {
        ir_string_list_add_unique_owned(io_names, io_count, io_capacity, expr->identifier);
        return;
    }

    switch (expr->kind) {
        case EXPR_BINARY:
            ir_collect_expr_identifiers(expr->binary.left, io_names, io_count, io_capacity);
            ir_collect_expr_identifiers(expr->binary.right, io_names, io_count, io_capacity);
            break;
        case EXPR_UNARY:
            ir_collect_expr_identifiers(expr->unary.operand, io_names, io_count, io_capacity);
            break;
        case EXPR_AWAIT:
            ir_collect_expr_identifiers(expr->await_expr.expr, io_names, io_count, io_capacity);
            break;
        case EXPR_CALL:
            ir_collect_expr_identifiers(expr->call.callee, io_names, io_count, io_capacity);
            for (int i = 0; i < expr->call.arg_count; i++) {
                ir_collect_expr_identifiers(expr->call.args[i], io_names, io_count, io_capacity);
            }
            break;
        case EXPR_FUNC_LITERAL:
            for (int i = 0; i < expr->func_literal.capture_count; i++) {
                const char* capture_name = expr->func_literal.capture_names
                    ? expr->func_literal.capture_names[i]
                    : NULL;
                if (capture_name) {
                    ir_string_list_add_unique_owned(io_names, io_count, io_capacity, capture_name);
                }
            }
            break;
        case EXPR_ARRAY:
        case EXPR_INDEX:
            ir_collect_expr_identifiers(expr->index.array, io_names, io_count, io_capacity);
            ir_collect_expr_identifiers(expr->index.index, io_names, io_count, io_capacity);
            break;
        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->array_literal.element_count; i++) {
                ir_collect_expr_identifiers(expr->array_literal.elements[i], io_names, io_count, io_capacity);
            }
            break;
        case EXPR_CAST:
            ir_collect_expr_identifiers(expr->cast.value, io_names, io_count, io_capacity);
            break;
        case EXPR_TRY:
            ir_collect_expr_identifiers(expr->try_expr.expr, io_names, io_count, io_capacity);
            break;
        case EXPR_TYPE_TEST:
            ir_collect_expr_identifiers(expr->type_test.value, io_names, io_count, io_capacity);
            break;
        case EXPR_IF:
            ir_collect_expr_identifiers(expr->if_expr.condition, io_names, io_count, io_capacity);
            ir_collect_expr_identifiers(expr->if_expr.then_expr, io_names, io_count, io_capacity);
            ir_collect_expr_identifiers(expr->if_expr.else_expr, io_names, io_count, io_capacity);
            break;
        case EXPR_MATCH:
            ir_collect_expr_identifiers(expr->match_expr.subject, io_names, io_count, io_capacity);
            for (int i = 0; i < expr->match_expr.arm_count; i++) {
                if (expr->match_expr.patterns) {
                    ir_collect_expr_identifiers(expr->match_expr.patterns[i], io_names, io_count, io_capacity);
                }
                if (expr->match_expr.guards) {
                    ir_collect_expr_identifiers(expr->match_expr.guards[i], io_names, io_count, io_capacity);
                }
                if (expr->match_expr.values) {
                    ir_collect_expr_identifiers(expr->match_expr.values[i], io_names, io_count, io_capacity);
                }
            }
            ir_collect_expr_identifiers(expr->match_expr.else_expr, io_names, io_count, io_capacity);
            break;
        case EXPR_RECORD_LITERAL:
            for (int i = 0; i < expr->record_literal.field_count; i++) {
                ir_collect_expr_identifiers(expr->record_literal.field_values[i], io_names, io_count, io_capacity);
            }
            break;
        case EXPR_FIELD_ACCESS:
            ir_collect_expr_identifiers(expr->field_access.object, io_names, io_count, io_capacity);
            break;
        case EXPR_TUPLE_LITERAL:
            for (int i = 0; i < expr->tuple_literal.element_count; i++) {
                ir_collect_expr_identifiers(expr->tuple_literal.elements[i], io_names, io_count, io_capacity);
            }
            break;
        case EXPR_TUPLE_ACCESS:
            ir_collect_expr_identifiers(expr->tuple_access.tuple, io_names, io_count, io_capacity);
            break;
        case EXPR_MAP_LITERAL:
            for (int i = 0; i < expr->map_literal.entry_count; i++) {
                ir_collect_expr_identifiers(expr->map_literal.keys[i], io_names, io_count, io_capacity);
                ir_collect_expr_identifiers(expr->map_literal.values[i], io_names, io_count, io_capacity);
            }
            break;
        case EXPR_SET_LITERAL:
            for (int i = 0; i < expr->set_literal.element_count; i++) {
                ir_collect_expr_identifiers(expr->set_literal.elements[i], io_names, io_count, io_capacity);
            }
            break;
        default:
            break;
    }
}

static bool ir_cse_candidate_depends_on(const IRCseCandidate* candidate, const char* name) {
    if (!candidate || !name) return false;
    if (candidate->var_name && strcmp(candidate->var_name, name) == 0) return true;
    return ir_string_list_contains(candidate->dependencies, candidate->dependency_count, name);
}

static void ir_cse_candidate_free(IRCseCandidate* candidate) {
    if (!candidate) return;
    if (candidate->var_name) free(candidate->var_name);
    if (candidate->value_expr) expr_free(candidate->value_expr);
    for (int i = 0; i < candidate->dependency_count; i++) {
        if (candidate->dependencies && candidate->dependencies[i]) {
            free(candidate->dependencies[i]);
        }
    }
    if (candidate->dependencies) free(candidate->dependencies);
    candidate->var_name = NULL;
    candidate->value_expr = NULL;
    candidate->dependencies = NULL;
    candidate->dependency_count = 0;
}

static void ir_cse_candidates_remove_at(IRCseCandidate* candidates, int* count, int index) {
    if (!candidates || !count || index < 0 || index >= *count) return;
    ir_cse_candidate_free(&candidates[index]);
    for (int i = index + 1; i < *count; i++) {
        candidates[i - 1] = candidates[i];
    }
    (*count)--;
}

static void ir_cse_candidates_clear(IRCseCandidate* candidates, int* count) {
    if (!candidates || !count) return;
    for (int i = 0; i < *count; i++) {
        ir_cse_candidate_free(&candidates[i]);
    }
    *count = 0;
}

static void ir_cse_invalidate_by_name(IRCseCandidate* candidates, int* count, const char* name) {
    if (!candidates || !count || !name) return;
    for (int i = *count - 1; i >= 0; i--) {
        if (ir_cse_candidate_depends_on(&candidates[i], name)) {
            ir_cse_candidates_remove_at(candidates, count, i);
        }
    }
}

static void ir_cse_candidate_copy_into(IRCseCandidate* dst, const IRCseCandidate* src) {
    if (!dst) return;
    dst->var_name = NULL;
    dst->value_expr = NULL;
    dst->dependencies = NULL;
    dst->dependency_count = 0;
    if (!src) return;

    dst->var_name = src->var_name ? safe_strdup(src->var_name) : NULL;
    dst->value_expr = src->value_expr ? expr_clone(src->value_expr) : NULL;
    if (src->dependency_count > 0 && src->dependencies) {
        dst->dependencies =
            (char**)safe_calloc((size_t)src->dependency_count, sizeof(char*));
        dst->dependency_count = src->dependency_count;
        for (int i = 0; i < src->dependency_count; i++) {
            dst->dependencies[i] =
                src->dependencies[i] ? safe_strdup(src->dependencies[i]) : NULL;
        }
    }
}

static IRCseCandidate* ir_cse_candidates_clone_array(const IRCseCandidate* candidates,
                                                     int count) {
    if (!candidates || count <= 0) return NULL;

    IRCseCandidate* cloned =
        (IRCseCandidate*)safe_calloc((size_t)count, sizeof(IRCseCandidate));
    for (int i = 0; i < count; i++) {
        ir_cse_candidate_copy_into(&cloned[i], &candidates[i]);
    }
    return cloned;
}

static void ir_cse_candidates_replace_with_clone(IRCseCandidate** io_candidates,
                                                 int* count,
                                                 int* capacity,
                                                 const IRCseCandidate* source,
                                                 int source_count) {
    if (!io_candidates || !count || !capacity) return;
    if (*io_candidates) {
        ir_cse_candidates_clear(*io_candidates, count);
    } else {
        *count = 0;
    }
    if (source_count > *capacity) {
        *io_candidates = (IRCseCandidate*)safe_realloc(*io_candidates,
                                                       (size_t)source_count * sizeof(IRCseCandidate));
        *capacity = source_count;
    }
    if (!*io_candidates || !source || source_count <= 0) return;
    for (int i = 0; i < source_count; i++) {
        ir_cse_candidate_copy_into(&((*io_candidates)[i]), &source[i]);
    }
    *count = source_count;
}

static int ir_cse_candidates_find_equivalent(const IRCseCandidate* candidates,
                                             int count,
                                             const IRCseCandidate* needle) {
    if (!candidates || count <= 0 || !needle || !needle->var_name || !needle->value_expr) return -1;
    for (int i = 0; i < count; i++) {
        if (!candidates[i].var_name || !candidates[i].value_expr) continue;
        if (strcmp(candidates[i].var_name, needle->var_name) != 0) continue;
        if (ir_exprs_value_equivalent(candidates[i].value_expr, needle->value_expr)) {
            return i;
        }
    }
    return -1;
}

static void ir_cse_candidates_intersect_in_place(IRCseCandidate* candidates,
                                                 int* count,
                                                 const IRCseCandidate* other,
                                                 int other_count) {
    if (!candidates || !count) return;
    for (int i = *count - 1; i >= 0; i--) {
        if (ir_cse_candidates_find_equivalent(other, other_count, &candidates[i]) < 0) {
            ir_cse_candidates_remove_at(candidates, count, i);
        }
    }
}

static bool ir_cse_candidates_sets_equal(const IRCseCandidate* a,
                                         int a_count,
                                         const IRCseCandidate* b,
                                         int b_count) {
    if (a_count != b_count) return false;
    for (int i = 0; i < a_count; i++) {
        if (ir_cse_candidates_find_equivalent(b, b_count, &a[i]) < 0) {
            return false;
        }
    }
    return true;
}

static void ir_cse_flow_state_init(IRCseFlowState* state) {
    if (!state) return;
    state->candidates = NULL;
    state->candidate_count = 0;
    state->candidate_capacity = 0;
    state->aliases = NULL;
    state->alias_count = 0;
    state->alias_capacity = 0;
}

static void ir_cse_flow_state_free(IRCseFlowState* state) {
    if (!state) return;
    ir_cse_candidates_clear(state->candidates, &state->candidate_count);
    ir_copy_aliases_clear(state->aliases, &state->alias_count);
    if (state->candidates) free(state->candidates);
    if (state->aliases) free(state->aliases);
    ir_cse_flow_state_init(state);
}

static void ir_cse_flow_state_replace_with_clone(IRCseFlowState* dst,
                                                 const IRCseFlowState* src) {
    if (!dst) return;
    ir_cse_candidates_replace_with_clone(&dst->candidates,
                                         &dst->candidate_count,
                                         &dst->candidate_capacity,
                                         src ? src->candidates : NULL,
                                         src ? src->candidate_count : 0);
    ir_copy_aliases_replace_with_clone(&dst->aliases,
                                       &dst->alias_count,
                                       &dst->alias_capacity,
                                       src ? src->aliases : NULL,
                                       src ? src->alias_count : 0);
}

static bool ir_cse_flow_state_equals(const IRCseFlowState* a, const IRCseFlowState* b) {
    int a_candidate_count = a ? a->candidate_count : 0;
    int b_candidate_count = b ? b->candidate_count : 0;
    int a_alias_count = a ? a->alias_count : 0;
    int b_alias_count = b ? b->alias_count : 0;

    return ir_cse_candidates_sets_equal(a ? a->candidates : NULL,
                                        a_candidate_count,
                                        b ? b->candidates : NULL,
                                        b_candidate_count) &&
           ir_copy_aliases_sets_equal(a ? a->aliases : NULL,
                                      a_alias_count,
                                      b ? b->aliases : NULL,
                                      b_alias_count);
}

static Stmt* ir_loop_body_stmt(Stmt* stmt) {
    if (!stmt) return NULL;
    switch (stmt->kind) {
        case STMT_WHILE:
            return stmt->while_stmt.body;
        case STMT_FOREACH:
            return stmt->foreach.body;
        case STMT_FOR_RANGE:
            return stmt->for_range.body;
        default:
            return NULL;
    }
}

static void ir_cse_merge_if_fallthrough(Stmt* then_branch,
                                        Stmt* else_branch,
                                        IRCseCandidate** io_candidates,
                                        int* candidate_count,
                                        int* candidate_capacity,
                                        IRCopyAlias** io_aliases,
                                        int* alias_count,
                                        int* alias_capacity) {
    IRCseCandidate* candidates = io_candidates ? *io_candidates : NULL;
    IRCopyAlias* aliases = io_aliases ? *io_aliases : NULL;
    int incoming_candidate_count = candidate_count ? *candidate_count : 0;
    int incoming_alias_count = alias_count ? *alias_count : 0;

    IRCseCandidate* then_candidates =
        ir_cse_candidates_clone_array(candidates, incoming_candidate_count);
    IRCseCandidate* else_candidates =
        ir_cse_candidates_clone_array(candidates, incoming_candidate_count);
    IRCopyAlias* then_aliases = NULL;
    IRCopyAlias* else_aliases = NULL;
    int then_candidate_count = incoming_candidate_count;
    int else_candidate_count = incoming_candidate_count;
    int then_alias_count = incoming_alias_count;
    int else_alias_count = incoming_alias_count;

    if (aliases && incoming_alias_count > 0) {
        then_aliases =
            (IRCopyAlias*)safe_calloc((size_t)incoming_alias_count, sizeof(IRCopyAlias));
        else_aliases =
            (IRCopyAlias*)safe_calloc((size_t)incoming_alias_count, sizeof(IRCopyAlias));
        for (int i = 0; i < incoming_alias_count; i++) {
            then_aliases[i].target_name =
                aliases[i].target_name ? safe_strdup(aliases[i].target_name) : NULL;
            then_aliases[i].source_name =
                aliases[i].source_name ? safe_strdup(aliases[i].source_name) : NULL;
            else_aliases[i].target_name =
                aliases[i].target_name ? safe_strdup(aliases[i].target_name) : NULL;
            else_aliases[i].source_name =
                aliases[i].source_name ? safe_strdup(aliases[i].source_name) : NULL;
        }
    }

    int then_candidate_capacity = incoming_candidate_count;
    int else_candidate_capacity = incoming_candidate_count;
    int then_alias_capacity = incoming_alias_count;
    int else_alias_capacity = incoming_alias_count;

    ir_apply_local_cse_stmt(then_branch,
                            &then_candidates,
                            &then_candidate_count,
                            &then_candidate_capacity,
                            &then_aliases,
                            &then_alias_count,
                            &then_alias_capacity,
                            true,
                            false);
    if (else_branch) {
        ir_apply_local_cse_stmt(else_branch,
                                &else_candidates,
                                &else_candidate_count,
                                &else_candidate_capacity,
                                &else_aliases,
                                &else_alias_count,
                                &else_alias_capacity,
                                true,
                                false);
    }

    ir_cse_candidates_replace_with_clone(io_candidates,
                                         candidate_count,
                                         candidate_capacity,
                                         then_candidates,
                                         then_candidate_count);
    candidates = io_candidates ? *io_candidates : candidates;
    ir_cse_candidates_intersect_in_place(candidates,
                                         candidate_count,
                                         else_candidates,
                                         else_candidate_count);
    if (io_aliases && alias_count && alias_capacity) {
        ir_copy_aliases_replace_with_clone(io_aliases,
                                           alias_count,
                                           alias_capacity,
                                           then_aliases,
                                           then_alias_count);
        aliases = *io_aliases;
        ir_copy_aliases_intersect_in_place(aliases,
                                           alias_count,
                                           else_aliases,
                                           else_alias_count);
    }

    ir_cse_candidates_clear(then_candidates, &then_candidate_count);
    ir_cse_candidates_clear(else_candidates, &else_candidate_count);
    if (then_candidates) free(then_candidates);
    if (else_candidates) free(else_candidates);
    ir_copy_aliases_clear(then_aliases, &then_alias_count);
    ir_copy_aliases_clear(else_aliases, &else_alias_count);
    if (then_aliases) free(then_aliases);
    if (else_aliases) free(else_aliases);
}

static void ir_cse_merge_match_fallthrough(Stmt* stmt,
                                           IRCseCandidate** io_candidates,
                                           int* candidate_count,
                                           int* candidate_capacity,
                                           IRCopyAlias** io_aliases,
                                           int* alias_count,
                                           int* alias_capacity) {
    if (!stmt) return;

    IRCseCandidate* incoming_candidates = io_candidates ? *io_candidates : NULL;
    IRCopyAlias* incoming_aliases = io_aliases ? *io_aliases : NULL;
    int incoming_candidate_count = candidate_count ? *candidate_count : 0;
    int incoming_alias_count = alias_count ? *alias_count : 0;
    bool merged_any = false;

    for (int arm = 0; arm < stmt->match_stmt.arm_count; arm++) {
        Stmt* body = (stmt->match_stmt.bodies && arm < stmt->match_stmt.arm_count)
                         ? stmt->match_stmt.bodies[arm]
                         : NULL;
        IRCseCandidate* branch_candidates =
            ir_cse_candidates_clone_array(incoming_candidates, incoming_candidate_count);
        IRCopyAlias* branch_aliases =
            ir_copy_aliases_clone_array(incoming_aliases, incoming_alias_count);
        int branch_candidate_count = incoming_candidate_count;
        int branch_candidate_capacity = incoming_candidate_count;
        int branch_alias_count = incoming_alias_count;
        int branch_alias_capacity = incoming_alias_count;

        if (body) {
            ir_apply_local_cse_stmt(body,
                                    &branch_candidates,
                                    &branch_candidate_count,
                                    &branch_candidate_capacity,
                                    &branch_aliases,
                                    &branch_alias_count,
                                    &branch_alias_capacity,
                                    true,
                                    false);
        }

        if (!merged_any) {
            ir_cse_candidates_replace_with_clone(io_candidates,
                                                 candidate_count,
                                                 candidate_capacity,
                                                 branch_candidates,
                                                 branch_candidate_count);
            ir_copy_aliases_replace_with_clone(io_aliases,
                                               alias_count,
                                               alias_capacity,
                                               branch_aliases,
                                               branch_alias_count);
            merged_any = true;
        } else {
            ir_cse_candidates_intersect_in_place(*io_candidates,
                                                 candidate_count,
                                                 branch_candidates,
                                                 branch_candidate_count);
            ir_copy_aliases_intersect_in_place(*io_aliases,
                                               alias_count,
                                               branch_aliases,
                                               branch_alias_count);
        }

        ir_cse_candidates_clear(branch_candidates, &branch_candidate_count);
        ir_copy_aliases_clear(branch_aliases, &branch_alias_count);
        if (branch_candidates) free(branch_candidates);
        if (branch_aliases) free(branch_aliases);
    }

    if (stmt->match_stmt.else_branch || !merged_any) {
        IRCseCandidate* else_candidates =
            ir_cse_candidates_clone_array(incoming_candidates, incoming_candidate_count);
        IRCopyAlias* else_aliases =
            ir_copy_aliases_clone_array(incoming_aliases, incoming_alias_count);
        int else_candidate_count = incoming_candidate_count;
        int else_candidate_capacity = incoming_candidate_count;
        int else_alias_count = incoming_alias_count;
        int else_alias_capacity = incoming_alias_count;

        if (stmt->match_stmt.else_branch) {
            ir_apply_local_cse_stmt(stmt->match_stmt.else_branch,
                                    &else_candidates,
                                    &else_candidate_count,
                                    &else_candidate_capacity,
                                    &else_aliases,
                                    &else_alias_count,
                                    &else_alias_capacity,
                                    true,
                                    false);
        }

        if (!merged_any) {
            ir_cse_candidates_replace_with_clone(io_candidates,
                                                 candidate_count,
                                                 candidate_capacity,
                                                 else_candidates,
                                                 else_candidate_count);
            ir_copy_aliases_replace_with_clone(io_aliases,
                                               alias_count,
                                               alias_capacity,
                                               else_aliases,
                                               else_alias_count);
        } else {
            ir_cse_candidates_intersect_in_place(*io_candidates,
                                                 candidate_count,
                                                 else_candidates,
                                                 else_candidate_count);
            ir_copy_aliases_intersect_in_place(*io_aliases,
                                               alias_count,
                                               else_aliases,
                                               else_alias_count);
        }

        ir_cse_candidates_clear(else_candidates, &else_candidate_count);
        ir_copy_aliases_clear(else_aliases, &else_alias_count);
        if (else_candidates) free(else_candidates);
        if (else_aliases) free(else_aliases);
    }
}

static void ir_cse_merge_loop_fallthrough(Stmt* stmt,
                                          IRCseCandidate** io_candidates,
                                          int* candidate_count,
                                          int* candidate_capacity,
                                          IRCopyAlias** io_aliases,
                                          int* alias_count,
                                          int* alias_capacity) {
    if (!stmt) return;

    Stmt* body_stmt = ir_loop_body_stmt(stmt);
    if (!body_stmt) return;

    IRStatementList body_view;
    Stmt* body_single_stmt[1];
    ir_stmt_as_statement_view(body_stmt, &body_view, body_single_stmt);
    if (body_view.stmt_count <= 0) return;

    IRCseCandidate* incoming_candidates = io_candidates ? *io_candidates : NULL;
    IRCopyAlias* incoming_aliases = io_aliases ? *io_aliases : NULL;
    int incoming_candidate_count = candidate_count ? *candidate_count : 0;
    int incoming_alias_count = alias_count ? *alias_count : 0;
    IRBasicBlockList blocks;
    ir_basic_block_list_init(&blocks);
    IRStructuredBlockResult body_result =
        ir_build_basic_blocks_for_range(&blocks,
                                        body_view.statements,
                                        body_view.stmt_count,
                                        0,
                                        body_view.stmt_count - 1);
    if (blocks.count <= 0 || body_result.entry_block < 0) {
        ir_structured_block_result_free(&body_result);
        ir_basic_block_list_free(&blocks);
        return;
    }

    IRCseFlowState seed_state;
    ir_cse_flow_state_init(&seed_state);
    seed_state.candidates = ir_cse_candidates_clone_array(incoming_candidates, incoming_candidate_count);
    seed_state.candidate_count = incoming_candidate_count;
    seed_state.candidate_capacity = incoming_candidate_count;
    seed_state.aliases = ir_copy_aliases_clone_array(incoming_aliases, incoming_alias_count);
    seed_state.alias_count = incoming_alias_count;
    seed_state.alias_capacity = incoming_alias_count;

    IRCseFlowState* out_states =
        (IRCseFlowState*)safe_calloc((size_t)blocks.count, sizeof(IRCseFlowState));
    bool* out_reachable = (bool*)safe_calloc((size_t)blocks.count, sizeof(bool));
    int worklist_capacity = blocks.count > 0 ? blocks.count : 1;
    int* worklist = (int*)safe_calloc((size_t)worklist_capacity, sizeof(int));
    bool* queued = (bool*)safe_calloc((size_t)blocks.count, sizeof(bool));
    int worklist_head = 0;
    int worklist_tail = 0;

    for (int i = 0; i < blocks.count; i++) {
        ir_cse_flow_state_init(&out_states[i]);
    }

    ir_worklist_enqueue(&worklist,
                        &worklist_tail,
                        &worklist_capacity,
                        queued,
                        body_result.entry_block);

    while (worklist_head < worklist_tail) {
        int block_index = worklist[worklist_head++];
        queued[block_index] = false;

        IRCseFlowState merged_in;
        IRCseFlowState new_out;
        ir_cse_flow_state_init(&merged_in);
        ir_cse_flow_state_init(&new_out);

        bool block_reachable =
            ir_cse_merge_block_predecessors_seeded(&blocks,
                                                   out_states,
                                                   out_reachable,
                                                   block_index,
                                                   body_result.entry_block,
                                                   &seed_state,
                                                   &merged_in);
        if (block_reachable) {
            ir_simulate_local_cse_block(&blocks.blocks[block_index], &merged_in, &new_out);
            if (!out_reachable[block_index] ||
                !ir_cse_flow_state_equals(&out_states[block_index], &new_out)) {
                ir_cse_flow_state_replace_with_clone(&out_states[block_index], &new_out);
                out_reachable[block_index] = true;
                for (int i = 0; i < blocks.blocks[block_index].successor_count; i++) {
                    int succ_index = blocks.blocks[block_index].successors[i];
                    if (succ_index < 0 || succ_index >= blocks.count) continue;
                    ir_worklist_enqueue(&worklist,
                                        &worklist_tail,
                                        &worklist_capacity,
                                        queued,
                                        succ_index);
                }
            }
        }

        ir_cse_flow_state_free(&merged_in);
        ir_cse_flow_state_free(&new_out);
    }

    IRCseFlowState merged_exits;
    ir_cse_flow_state_init(&merged_exits);
    bool merged_any = false;
    IRBlockExitList* exit_lists[3] = {
        &body_result.fallthrough_exits,
        &body_result.continue_exits,
        &body_result.break_exits
    };

    for (int list_index = 0; list_index < 3; list_index++) {
        IRBlockExitList* exits = exit_lists[list_index];
        if (!exits) continue;
        for (int i = 0; i < exits->count; i++) {
            int exit_block_index = exits->block_indices[i];
            if (exit_block_index < 0 || exit_block_index >= blocks.count) continue;
            if (!out_reachable[exit_block_index]) continue;

            if (!merged_any) {
                ir_cse_flow_state_replace_with_clone(&merged_exits, &out_states[exit_block_index]);
                merged_any = true;
            } else {
                ir_cse_candidates_intersect_in_place(merged_exits.candidates,
                                                     &merged_exits.candidate_count,
                                                     out_states[exit_block_index].candidates,
                                                     out_states[exit_block_index].candidate_count);
                ir_copy_aliases_intersect_in_place(merged_exits.aliases,
                                                   &merged_exits.alias_count,
                                                   out_states[exit_block_index].aliases,
                                                   out_states[exit_block_index].alias_count);
            }
        }
    }

    if (merged_any) {
        ir_cse_candidates_intersect_in_place(merged_exits.candidates,
                                             &merged_exits.candidate_count,
                                             incoming_candidates,
                                             incoming_candidate_count);
        ir_copy_aliases_intersect_in_place(merged_exits.aliases,
                                           &merged_exits.alias_count,
                                           incoming_aliases,
                                           incoming_alias_count);

        ir_cse_candidates_replace_with_clone(io_candidates,
                                             candidate_count,
                                             candidate_capacity,
                                             merged_exits.candidates,
                                             merged_exits.candidate_count);
        ir_copy_aliases_replace_with_clone(io_aliases,
                                           alias_count,
                                           alias_capacity,
                                           merged_exits.aliases,
                                           merged_exits.alias_count);
    }

    ir_cse_flow_state_free(&merged_exits);
    for (int i = 0; i < blocks.count; i++) {
        ir_cse_flow_state_free(&out_states[i]);
    }
    ir_cse_flow_state_free(&seed_state);
    ir_structured_block_result_free(&body_result);
    ir_basic_block_list_free(&blocks);
    free(out_states);
    free(out_reachable);
    free(worklist);
    free(queued);
}

static void ir_cse_invalidate_for_stmt_fallthrough(Stmt* stmt,
                                                   IRCseCandidate* candidates,
                                                   int* candidate_count,
                                                   IRCopyAlias* aliases,
                                                   int* alias_count) {
    if (!stmt) return;

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            if (stmt->var_decl.name) {
                ir_cse_invalidate_by_name(candidates, candidate_count, stmt->var_decl.name);
                ir_copy_aliases_invalidate_by_name(aliases, alias_count, stmt->var_decl.name);
            }
            break;
        case STMT_VAR_TUPLE_DECL:
            if (stmt->var_tuple_decl.names) {
                for (int i = 0; i < stmt->var_tuple_decl.name_count; i++) {
                    const char* name = stmt->var_tuple_decl.names[i];
                    if (name) {
                        ir_cse_invalidate_by_name(candidates, candidate_count, name);
                        ir_copy_aliases_invalidate_by_name(aliases, alias_count, name);
                    }
                }
            }
            ir_cse_candidates_clear(candidates, candidate_count);
            ir_copy_aliases_clear(aliases, alias_count);
            break;
        case STMT_ASSIGN:
            if (stmt->assign.name) {
                ir_cse_invalidate_by_name(candidates, candidate_count, stmt->assign.name);
                ir_copy_aliases_invalidate_by_name(aliases, alias_count, stmt->assign.name);
            }
            break;
        case STMT_BLOCK:
            for (int i = 0; i < stmt->block.stmt_count; i++) {
                ir_cse_invalidate_for_stmt_fallthrough(stmt->block.statements[i],
                                                       candidates,
                                                       candidate_count,
                                                       aliases,
                                                       alias_count);
            }
            break;
        case STMT_IF:
            ir_cse_invalidate_for_stmt_fallthrough(stmt->if_stmt.then_branch,
                                                   candidates,
                                                   candidate_count,
                                                   aliases,
                                                   alias_count);
            ir_cse_invalidate_for_stmt_fallthrough(stmt->if_stmt.else_branch,
                                                   candidates,
                                                   candidate_count,
                                                   aliases,
                                                   alias_count);
            break;
        case STMT_MATCH:
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                if (stmt->match_stmt.bodies) {
                    ir_cse_invalidate_for_stmt_fallthrough(stmt->match_stmt.bodies[i],
                                                           candidates,
                                                           candidate_count,
                                                           aliases,
                                                           alias_count);
                }
            }
            ir_cse_invalidate_for_stmt_fallthrough(stmt->match_stmt.else_branch,
                                                   candidates,
                                                   candidate_count,
                                                   aliases,
                                                   alias_count);
            break;
        case STMT_WHILE:
            ir_cse_invalidate_for_stmt_fallthrough(stmt->while_stmt.body,
                                                   candidates,
                                                   candidate_count,
                                                   aliases,
                                                   alias_count);
            break;
        case STMT_FOREACH:
            if (stmt->foreach.var_name) {
                ir_cse_invalidate_by_name(candidates, candidate_count, stmt->foreach.var_name);
                ir_copy_aliases_invalidate_by_name(aliases, alias_count, stmt->foreach.var_name);
            }
            ir_cse_invalidate_for_stmt_fallthrough(stmt->foreach.body,
                                                   candidates,
                                                   candidate_count,
                                                   aliases,
                                                   alias_count);
            break;
        case STMT_FOR_RANGE:
            if (stmt->for_range.var_name) {
                ir_cse_invalidate_by_name(candidates, candidate_count, stmt->for_range.var_name);
                ir_copy_aliases_invalidate_by_name(aliases, alias_count, stmt->for_range.var_name);
            }
            ir_cse_invalidate_for_stmt_fallthrough(stmt->for_range.body,
                                                   candidates,
                                                   candidate_count,
                                                   aliases,
                                                   alias_count);
            break;
        case STMT_EXPR:
        case STMT_ASSIGN_INDEX:
        case STMT_ASSIGN_FIELD:
        case STMT_DEFER:
        case STMT_RETURN:
        case STMT_BREAK:
        case STMT_CONTINUE:
        default:
            ir_cse_candidates_clear(candidates, candidate_count);
            ir_copy_aliases_clear(aliases, alias_count);
            break;
    }
}

static void ir_apply_local_cse_stmt(Stmt* stmt,
                                    IRCseCandidate** io_candidates,
                                    int* io_candidate_count,
                                    int* io_candidate_capacity,
                                    IRCopyAlias** io_aliases,
                                    int* io_alias_count,
                                    int* io_alias_capacity,
                                    bool preserve_outgoing_state,
                                    bool structured_cfg_exit) {
    IRCseCandidate* candidates = io_candidates ? *io_candidates : NULL;
    int candidate_count = io_candidate_count ? *io_candidate_count : 0;
    int candidate_capacity = io_candidate_capacity ? *io_candidate_capacity : 0;
    IRCopyAlias* aliases = io_aliases ? *io_aliases : NULL;
    int alias_count = io_alias_count ? *io_alias_count : 0;
    int alias_capacity = io_alias_capacity ? *io_alias_capacity : 0;

    if (!stmt) return;

    if (stmt->kind == STMT_VAR_DECL) {
        if (stmt->var_decl.name) {
            ir_cse_invalidate_by_name(candidates, &candidate_count, stmt->var_decl.name);
            ir_copy_aliases_invalidate_by_name(aliases, &alias_count, stmt->var_decl.name);
        }

        if (stmt->var_decl.initializer &&
            ir_cse_expr_eligible(stmt->var_decl.initializer)) {
            Expr* normalized_init =
                ir_clone_normalized_value_expr(stmt->var_decl.initializer, aliases, alias_count);
            bool reused_existing_value = false;

            for (int j = 0; j < candidate_count; j++) {
                if (!candidates[j].value_expr || !candidates[j].var_name) continue;
                if (normalized_init &&
                    ir_exprs_value_equivalent(normalized_init, candidates[j].value_expr)) {
                    Expr* old_init = stmt->var_decl.initializer;
                    Expr* replacement = expr_create_identifier(candidates[j].var_name,
                                                               stmt->file ? stmt->file : old_init->file,
                                                               old_init->line,
                                                               old_init->column);
                    expr_free(old_init);
                    stmt->var_decl.initializer = replacement;
                    if (stmt->var_decl.name) {
                        ir_copy_aliases_set(&aliases,
                                            &alias_count,
                                            &alias_capacity,
                                            stmt->var_decl.name,
                                            candidates[j].var_name);
                    }
                    reused_existing_value = true;
                    break;
                }
            }

            if (!reused_existing_value &&
                stmt->var_decl.name &&
                normalized_init &&
                normalized_init->kind == EXPR_IDENTIFIER &&
                normalized_init->identifier) {
                ir_copy_aliases_set(&aliases,
                                    &alias_count,
                                    &alias_capacity,
                                    stmt->var_decl.name,
                                    normalized_init->identifier);
            }

            if (!reused_existing_value &&
                stmt->var_decl.name &&
                normalized_init &&
                normalized_init->kind != EXPR_IDENTIFIER) {
                if (candidate_count >= candidate_capacity) {
                    int next_capacity = candidate_capacity > 0 ? candidate_capacity * 2 : 8;
                    candidates = (IRCseCandidate*)safe_realloc(candidates,
                                                               (size_t)next_capacity * sizeof(IRCseCandidate));
                    candidate_capacity = next_capacity;
                }

                IRCseCandidate* added = &candidates[candidate_count++];
                added->var_name = safe_strdup(stmt->var_decl.name);
                added->value_expr = normalized_init;
                added->dependencies = NULL;
                added->dependency_count = 0;

                int dep_capacity = 0;
                ir_collect_expr_identifiers(added->value_expr,
                                            &added->dependencies,
                                            &added->dependency_count,
                                            &dep_capacity);
                normalized_init = NULL;
            }

            if (normalized_init) expr_free(normalized_init);
        }

        if (io_candidates) *io_candidates = candidates;
        if (io_candidate_count) *io_candidate_count = candidate_count;
        if (io_candidate_capacity) *io_candidate_capacity = candidate_capacity;
        if (io_aliases) *io_aliases = aliases;
        if (io_alias_count) *io_alias_count = alias_count;
        if (io_alias_capacity) *io_alias_capacity = alias_capacity;
        return;
    }

    if (stmt->kind == STMT_ASSIGN) {
        if (stmt->assign.name) {
            ir_cse_invalidate_by_name(candidates, &candidate_count, stmt->assign.name);
            ir_copy_aliases_invalidate_by_name(aliases, &alias_count, stmt->assign.name);
        }
        if (stmt->assign.name && stmt->assign.value) {
            Expr* normalized_value =
                ir_clone_normalized_value_expr(stmt->assign.value, aliases, alias_count);
            bool reused_existing_value = false;

            for (int j = 0; j < candidate_count; j++) {
                if (!candidates[j].value_expr || !candidates[j].var_name) continue;
                if (normalized_value &&
                    ir_exprs_value_equivalent(normalized_value, candidates[j].value_expr)) {
                    Expr* old_value = stmt->assign.value;
                    Expr* replacement = expr_create_identifier(candidates[j].var_name,
                                                               stmt->file ? stmt->file : old_value->file,
                                                               old_value->line,
                                                               old_value->column);
                    expr_free(old_value);
                    stmt->assign.value = replacement;
                    ir_copy_aliases_set(&aliases,
                                        &alias_count,
                                        &alias_capacity,
                                        stmt->assign.name,
                                        candidates[j].var_name);
                    reused_existing_value = true;
                    break;
                }
            }

            if (!reused_existing_value &&
                normalized_value &&
                normalized_value->kind == EXPR_IDENTIFIER &&
                normalized_value->identifier) {
                ir_copy_aliases_set(&aliases,
                                    &alias_count,
                                    &alias_capacity,
                                    stmt->assign.name,
                                    normalized_value->identifier);
            }

            if (!reused_existing_value &&
                normalized_value &&
                normalized_value->kind != EXPR_IDENTIFIER) {
                if (candidate_count >= candidate_capacity) {
                    int next_capacity = candidate_capacity > 0 ? candidate_capacity * 2 : 8;
                    candidates = (IRCseCandidate*)safe_realloc(candidates,
                                                               (size_t)next_capacity * sizeof(IRCseCandidate));
                    candidate_capacity = next_capacity;
                }

                IRCseCandidate* added = &candidates[candidate_count++];
                added->var_name = safe_strdup(stmt->assign.name);
                added->value_expr = normalized_value;
                added->dependencies = NULL;
                added->dependency_count = 0;

                int dep_capacity = 0;
                ir_collect_expr_identifiers(added->value_expr,
                                            &added->dependencies,
                                            &added->dependency_count,
                                            &dep_capacity);
                normalized_value = NULL;
            }

            if (normalized_value) expr_free(normalized_value);
        }

        if (io_candidates) *io_candidates = candidates;
        if (io_candidate_count) *io_candidate_count = candidate_count;
        if (io_candidate_capacity) *io_candidate_capacity = candidate_capacity;
        if (io_aliases) *io_aliases = aliases;
        if (io_alias_count) *io_alias_count = alias_count;
        if (io_alias_capacity) *io_alias_capacity = alias_capacity;
        return;
    }

    if (stmt->kind == STMT_BLOCK && preserve_outgoing_state && stmt->block.stmt_count > 0) {
        IRStatementList nested_ir;
        nested_ir.statements = stmt->block.statements;
        nested_ir.stmt_count = stmt->block.stmt_count;
        nested_ir.stmt_capacity = stmt->block.stmt_count;
        ir_apply_local_cse_range(&nested_ir,
                                 0,
                                 nested_ir.stmt_count - 1,
                                 &candidates,
                                 &candidate_count,
                                 &candidate_capacity,
                                 &aliases,
                                 &alias_count,
                                 &alias_capacity,
                                 true,
                                 false);
        if (io_candidates) *io_candidates = candidates;
        if (io_candidate_count) *io_candidate_count = candidate_count;
        if (io_candidate_capacity) *io_candidate_capacity = candidate_capacity;
        if (io_aliases) *io_aliases = aliases;
        if (io_alias_count) *io_alias_count = alias_count;
        if (io_alias_capacity) *io_alias_capacity = alias_capacity;
        return;
    }

    if (stmt->kind == STMT_IF && preserve_outgoing_state) {
        if (structured_cfg_exit) {
            if (io_candidates) *io_candidates = candidates;
            if (io_candidate_count) *io_candidate_count = candidate_count;
            if (io_candidate_capacity) *io_candidate_capacity = candidate_capacity;
            if (io_aliases) *io_aliases = aliases;
            if (io_alias_count) *io_alias_count = alias_count;
            if (io_alias_capacity) *io_alias_capacity = alias_capacity;
            return;
        }
        ir_cse_merge_if_fallthrough(stmt->if_stmt.then_branch,
                                    stmt->if_stmt.else_branch,
                                    &candidates,
                                    &candidate_count,
                                    &candidate_capacity,
                                    &aliases,
                                    &alias_count,
                                    &alias_capacity);
        if (io_candidates) *io_candidates = candidates;
        if (io_candidate_count) *io_candidate_count = candidate_count;
        if (io_candidate_capacity) *io_candidate_capacity = candidate_capacity;
        if (io_aliases) *io_aliases = aliases;
        if (io_alias_count) *io_alias_count = alias_count;
        if (io_alias_capacity) *io_alias_capacity = alias_capacity;
        return;
    }

    if (stmt->kind == STMT_MATCH && preserve_outgoing_state) {
        if (structured_cfg_exit) {
            if (io_candidates) *io_candidates = candidates;
            if (io_candidate_count) *io_candidate_count = candidate_count;
            if (io_candidate_capacity) *io_candidate_capacity = candidate_capacity;
            if (io_aliases) *io_aliases = aliases;
            if (io_alias_count) *io_alias_count = alias_count;
            if (io_alias_capacity) *io_alias_capacity = alias_capacity;
            return;
        }
        ir_cse_merge_match_fallthrough(stmt,
                                       &candidates,
                                       &candidate_count,
                                       &candidate_capacity,
                                       &aliases,
                                       &alias_count,
                                       &alias_capacity);
        if (io_candidates) *io_candidates = candidates;
        if (io_candidate_count) *io_candidate_count = candidate_count;
        if (io_candidate_capacity) *io_candidate_capacity = candidate_capacity;
        if (io_aliases) *io_aliases = aliases;
        if (io_alias_count) *io_alias_count = alias_count;
        if (io_alias_capacity) *io_alias_capacity = alias_capacity;
        return;
    }

    if (preserve_outgoing_state &&
        (stmt->kind == STMT_BREAK || stmt->kind == STMT_CONTINUE)) {
        if (io_candidates) *io_candidates = candidates;
        if (io_candidate_count) *io_candidate_count = candidate_count;
        if (io_candidate_capacity) *io_candidate_capacity = candidate_capacity;
        if (io_aliases) *io_aliases = aliases;
        if (io_alias_count) *io_alias_count = alias_count;
        if (io_alias_capacity) *io_alias_capacity = alias_capacity;
        return;
    }

    if (preserve_outgoing_state &&
        structured_cfg_exit &&
        (stmt->kind == STMT_WHILE ||
         stmt->kind == STMT_FOREACH ||
         stmt->kind == STMT_FOR_RANGE)) {
        ir_cse_merge_loop_fallthrough(stmt,
                                      &candidates,
                                      &candidate_count,
                                      &candidate_capacity,
                                      &aliases,
                                      &alias_count,
                                      &alias_capacity);
        if (io_candidates) *io_candidates = candidates;
        if (io_candidate_count) *io_candidate_count = candidate_count;
        if (io_candidate_capacity) *io_candidate_capacity = candidate_capacity;
        if (io_aliases) *io_aliases = aliases;
        if (io_alias_count) *io_alias_count = alias_count;
        if (io_alias_capacity) *io_alias_capacity = alias_capacity;
        return;
    }

    if (stmt->kind == STMT_VAR_TUPLE_DECL && stmt->var_tuple_decl.names) {
        if (preserve_outgoing_state) {
            ir_cse_invalidate_for_stmt_fallthrough(stmt,
                                                   candidates,
                                                   &candidate_count,
                                                   aliases,
                                                   &alias_count);
        } else {
            for (int n = 0; n < stmt->var_tuple_decl.name_count; n++) {
                const char* name = stmt->var_tuple_decl.names[n];
                if (name) {
                    ir_cse_invalidate_by_name(candidates, &candidate_count, name);
                    ir_copy_aliases_invalidate_by_name(aliases, &alias_count, name);
                }
            }
            ir_cse_candidates_clear(candidates, &candidate_count);
            ir_copy_aliases_clear(aliases, &alias_count);
        }
    } else if (preserve_outgoing_state) {
        ir_cse_invalidate_for_stmt_fallthrough(stmt,
                                               candidates,
                                               &candidate_count,
                                               aliases,
                                               &alias_count);
    } else {
        ir_cse_candidates_clear(candidates, &candidate_count);
        ir_copy_aliases_clear(aliases, &alias_count);
    }

    if (io_candidates) *io_candidates = candidates;
    if (io_candidate_count) *io_candidate_count = candidate_count;
    if (io_candidate_capacity) *io_candidate_capacity = candidate_capacity;
    if (io_aliases) *io_aliases = aliases;
    if (io_alias_count) *io_alias_count = alias_count;
    if (io_alias_capacity) *io_alias_capacity = alias_capacity;
}

static void ir_apply_local_cse_range(IRStatementList* ir,
                                     int start_index,
                                     int end_index,
                                     IRCseCandidate** io_candidates,
                                     int* io_candidate_count,
                                     int* io_candidate_capacity,
                                     IRCopyAlias** io_aliases,
                                     int* io_alias_count,
                                     int* io_alias_capacity,
                                     bool preserve_outgoing_state,
                                     bool structured_cfg_exit) {
    IRCseCandidate* candidates = io_candidates ? *io_candidates : NULL;
    int candidate_count = io_candidate_count ? *io_candidate_count : 0;
    int candidate_capacity = io_candidate_capacity ? *io_candidate_capacity : 0;
    IRCopyAlias* aliases = io_aliases ? *io_aliases : NULL;
    int alias_count = io_alias_count ? *io_alias_count : 0;
    int alias_capacity = io_alias_capacity ? *io_alias_capacity : 0;

    for (int i = start_index; i <= end_index; i++) {
        Stmt* stmt = ir->statements[i];
        bool is_final_stmt = (i == end_index);
        if (!stmt) continue;
        ir_apply_local_cse_stmt(stmt,
                                &candidates,
                                &candidate_count,
                                &candidate_capacity,
                                &aliases,
                                &alias_count,
                                &alias_capacity,
                                is_final_stmt && preserve_outgoing_state,
                                is_final_stmt && structured_cfg_exit);
    }

    if (io_candidates) *io_candidates = candidates;
    if (io_candidate_count) *io_candidate_count = candidate_count;
    if (io_candidate_capacity) *io_candidate_capacity = candidate_capacity;
    if (io_aliases) *io_aliases = aliases;
    if (io_alias_count) *io_alias_count = alias_count;
    if (io_alias_capacity) *io_alias_capacity = alias_capacity;
}

static void ir_simulate_local_cse_block(const IRBasicBlock* block,
                                        const IRCseFlowState* in_state,
                                        IRCseFlowState* out_state) {
    if (!block || !out_state) return;

    bool preserve_outgoing_state =
        block->successor_count > 0 ||
        block->terminator_kind == IR_BLOCK_TERM_BREAK ||
        block->terminator_kind == IR_BLOCK_TERM_CONTINUE ||
        block->terminator_kind == IR_BLOCK_TERM_RETURN;

    IRStatementList cloned_ir;
    ir_clone_statement_range(&cloned_ir,
                             block->statements,
                             block->statement_count,
                             block->start_index,
                             block->end_index);
    ir_cse_flow_state_replace_with_clone(out_state, in_state);
    if (cloned_ir.stmt_count > 0) {
        ir_apply_local_cse_range(&cloned_ir,
                                 0,
                                 cloned_ir.stmt_count - 1,
                                 &out_state->candidates,
                                 &out_state->candidate_count,
                                 &out_state->candidate_capacity,
                                 &out_state->aliases,
                                 &out_state->alias_count,
                                 &out_state->alias_capacity,
                                 preserve_outgoing_state,
                                 true);
    }
    ir_free_cloned_statement_range(&cloned_ir);
}

static bool ir_cse_merge_block_predecessors_seeded(const IRBasicBlockList* blocks,
                                                   const IRCseFlowState* out_states,
                                                   const bool* out_reachable,
                                                   int block_index,
                                                   int entry_block_index,
                                                   const IRCseFlowState* entry_state,
                                                   IRCseFlowState* merged_in) {
    if (!blocks || !merged_in || block_index < 0 || block_index >= blocks->count) return false;

    ir_cse_flow_state_free(merged_in);
    if (!entry_state && block_index == 0) {
        return true;
    }

    bool merged_any = false;
    if (entry_state && block_index == entry_block_index) {
        ir_cse_flow_state_replace_with_clone(merged_in, entry_state);
        merged_any = true;
    }

    const IRBasicBlock* block = &blocks->blocks[block_index];
    for (int i = 0; i < block->predecessor_count; i++) {
        int pred_index = block->predecessors[i];
        if (pred_index < 0 || pred_index >= blocks->count) continue;
        if (!out_reachable || !out_reachable[pred_index]) continue;

        if (!merged_any) {
            ir_cse_flow_state_replace_with_clone(merged_in, &out_states[pred_index]);
            merged_any = true;
        } else {
            ir_cse_candidates_intersect_in_place(merged_in->candidates,
                                                 &merged_in->candidate_count,
                                                 out_states[pred_index].candidates,
                                                 out_states[pred_index].candidate_count);
            ir_copy_aliases_intersect_in_place(merged_in->aliases,
                                               &merged_in->alias_count,
                                               out_states[pred_index].aliases,
                                               out_states[pred_index].alias_count);
        }
    }

    return merged_any;
}

static bool ir_cse_merge_block_predecessors(const IRBasicBlockList* blocks,
                                            const IRCseFlowState* out_states,
                                            const bool* out_reachable,
                                            int block_index,
                                            IRCseFlowState* merged_in) {
    return ir_cse_merge_block_predecessors_seeded(blocks,
                                                  out_states,
                                                  out_reachable,
                                                  block_index,
                                                  -1,
                                                  NULL,
                                                  merged_in);
}

static void ir_apply_local_cse(IRStatementList* ir) {
    if (!ir || !ir->statements || ir->stmt_count <= 1) return;

    IRBasicBlockList blocks = ir_build_basic_blocks(ir);
    if (blocks.count <= 0) {
        ir_basic_block_list_free(&blocks);
        return;
    }

    IRCseFlowState* in_states =
        (IRCseFlowState*)safe_calloc((size_t)blocks.count, sizeof(IRCseFlowState));
    IRCseFlowState* out_states =
        (IRCseFlowState*)safe_calloc((size_t)blocks.count, sizeof(IRCseFlowState));
    bool* in_reachable = (bool*)safe_calloc((size_t)blocks.count, sizeof(bool));
    bool* out_reachable = (bool*)safe_calloc((size_t)blocks.count, sizeof(bool));
    int worklist_capacity = blocks.count > 0 ? blocks.count : 1;
    int* worklist = (int*)safe_calloc((size_t)worklist_capacity, sizeof(int));
    bool* queued = (bool*)safe_calloc((size_t)blocks.count, sizeof(bool));
    int worklist_head = 0;
    int worklist_tail = 0;

    for (int i = 0; i < blocks.count; i++) {
        ir_cse_flow_state_init(&in_states[i]);
        ir_cse_flow_state_init(&out_states[i]);
    }

    ir_worklist_enqueue(&worklist, &worklist_tail, &worklist_capacity, queued, 0);

    while (worklist_head < worklist_tail) {
        int block_index = worklist[worklist_head++];
        queued[block_index] = false;

        IRCseFlowState merged_in;
        IRCseFlowState new_out;
        ir_cse_flow_state_init(&merged_in);
        ir_cse_flow_state_init(&new_out);

        bool block_reachable =
            ir_cse_merge_block_predecessors(&blocks,
                                            out_states,
                                            out_reachable,
                                            block_index,
                                            &merged_in);
        if (!block_reachable) {
            ir_cse_flow_state_free(&in_states[block_index]);
            ir_cse_flow_state_free(&out_states[block_index]);
            in_reachable[block_index] = false;
            out_reachable[block_index] = false;
            ir_cse_flow_state_free(&merged_in);
            ir_cse_flow_state_free(&new_out);
            continue;
        }

        bool in_changed =
            !in_reachable[block_index] ||
            !ir_cse_flow_state_equals(&in_states[block_index], &merged_in);
        if (in_changed) {
            ir_cse_flow_state_replace_with_clone(&in_states[block_index], &merged_in);
            in_reachable[block_index] = true;
        }

        ir_simulate_local_cse_block(&blocks.blocks[block_index], &merged_in, &new_out);
        bool out_changed =
            !out_reachable[block_index] ||
            !ir_cse_flow_state_equals(&out_states[block_index], &new_out);
        if (out_changed) {
            ir_cse_flow_state_replace_with_clone(&out_states[block_index], &new_out);
            out_reachable[block_index] = true;

            for (int i = 0; i < blocks.blocks[block_index].successor_count; i++) {
                int succ_index = blocks.blocks[block_index].successors[i];
                if (succ_index < 0 || succ_index >= blocks.count) continue;
                ir_worklist_enqueue(&worklist,
                                    &worklist_tail,
                                    &worklist_capacity,
                                    queued,
                                    succ_index);
            }
        }

        ir_cse_flow_state_free(&merged_in);
        ir_cse_flow_state_free(&new_out);
    }

    for (int i = 0; i < blocks.count; i++) {
        if (!in_reachable[i]) continue;
        IRCseFlowState block_state;
        ir_cse_flow_state_init(&block_state);
        ir_cse_flow_state_replace_with_clone(&block_state, &in_states[i]);
        IRStatementList block_ir = {
            blocks.blocks[i].statements,
            blocks.blocks[i].statement_count,
            blocks.blocks[i].statement_count
        };
        ir_apply_local_cse_range(&block_ir,
                                 blocks.blocks[i].start_index,
                                 blocks.blocks[i].end_index,
                                 &block_state.candidates,
                                 &block_state.candidate_count,
                                 &block_state.candidate_capacity,
                                 &block_state.aliases,
                                 &block_state.alias_count,
                                 &block_state.alias_capacity,
                                 blocks.blocks[i].successor_count > 0,
                                 true);
        ir_cse_flow_state_free(&block_state);
    }

    for (int i = 0; i < blocks.count; i++) {
        ir_cse_flow_state_free(&in_states[i]);
        ir_cse_flow_state_free(&out_states[i]);
    }
    free(in_states);
    free(out_states);
    free(in_reachable);
    free(out_reachable);
    free(worklist);
    free(queued);
    ir_basic_block_list_free(&blocks);
}

static void ir_copy_aliases_remove_at(IRCopyAlias* aliases, int* count, int index) {
    if (!aliases || !count || index < 0 || index >= *count) return;
    if (aliases[index].target_name) free(aliases[index].target_name);
    if (aliases[index].source_name) free(aliases[index].source_name);
    for (int i = index + 1; i < *count; i++) {
        aliases[i - 1] = aliases[i];
    }
    (*count)--;
}

static void ir_copy_aliases_clear(IRCopyAlias* aliases, int* count) {
    if (!aliases || !count) return;
    for (int i = 0; i < *count; i++) {
        if (aliases[i].target_name) free(aliases[i].target_name);
        if (aliases[i].source_name) free(aliases[i].source_name);
    }
    *count = 0;
}

static int ir_copy_aliases_find_target(const IRCopyAlias* aliases, int count, const char* target_name) {
    if (!aliases || count <= 0 || !target_name) return -1;
    for (int i = 0; i < count; i++) {
        if (aliases[i].target_name && strcmp(aliases[i].target_name, target_name) == 0) {
            return i;
        }
    }
    return -1;
}

static const char* ir_copy_aliases_resolve(const IRCopyAlias* aliases, int count, const char* name) {
    if (!aliases || count <= 0 || !name) return name;

    const char* resolved = name;
    for (int step = 0; step < count; step++) {
        int idx = ir_copy_aliases_find_target(aliases, count, resolved);
        if (idx < 0 || !aliases[idx].source_name) break;
        if (strcmp(aliases[idx].source_name, resolved) == 0) break;
        resolved = aliases[idx].source_name;
    }
    return resolved;
}

static bool ir_copy_aliases_would_cycle(const IRCopyAlias* aliases,
                                        int count,
                                        const char* target_name,
                                        const char* source_name) {
    if (!aliases || count <= 0 || !target_name || !source_name) return false;
    const char* current = source_name;
    for (int step = 0; step < count; step++) {
        int idx = ir_copy_aliases_find_target(aliases, count, current);
        if (idx < 0 || !aliases[idx].source_name) return false;
        if (strcmp(aliases[idx].source_name, target_name) == 0) return true;
        if (strcmp(aliases[idx].source_name, current) == 0) return false;
        current = aliases[idx].source_name;
    }
    return false;
}

static void ir_copy_aliases_invalidate_by_name(IRCopyAlias* aliases, int* count, const char* name) {
    if (!aliases || !count || !name) return;
    for (int i = *count - 1; i >= 0; i--) {
        if ((aliases[i].target_name && strcmp(aliases[i].target_name, name) == 0) ||
            (aliases[i].source_name && strcmp(aliases[i].source_name, name) == 0)) {
            ir_copy_aliases_remove_at(aliases, count, i);
        }
    }
}

static void ir_copy_aliases_set(IRCopyAlias** io_aliases,
                                int* io_count,
                                int* io_capacity,
                                const char* target_name,
                                const char* source_name) {
    if (!io_aliases || !io_count || !io_capacity || !target_name || !source_name) return;

    ir_copy_aliases_invalidate_by_name(*io_aliases, io_count, target_name);

    if (strcmp(target_name, source_name) == 0) return;
    if (ir_copy_aliases_would_cycle(*io_aliases, *io_count, target_name, source_name)) return;

    if (*io_count >= *io_capacity) {
        int next_capacity = *io_capacity > 0 ? *io_capacity * 2 : 8;
        *io_aliases = (IRCopyAlias*)safe_realloc(*io_aliases, (size_t)next_capacity * sizeof(IRCopyAlias));
        *io_capacity = next_capacity;
    }

    IRCopyAlias* added = &((*io_aliases)[(*io_count)++]);
    added->target_name = safe_strdup(target_name);
    added->source_name = safe_strdup(source_name);
}

static IRCopyAlias* ir_copy_aliases_clone_array(const IRCopyAlias* aliases, int count) {
    if (!aliases || count <= 0) return NULL;

    IRCopyAlias* cloned =
        (IRCopyAlias*)safe_calloc((size_t)count, sizeof(IRCopyAlias));
    for (int i = 0; i < count; i++) {
        cloned[i].target_name = aliases[i].target_name ? safe_strdup(aliases[i].target_name) : NULL;
        cloned[i].source_name = aliases[i].source_name ? safe_strdup(aliases[i].source_name) : NULL;
    }
    return cloned;
}

static void ir_copy_aliases_replace_with_clone(IRCopyAlias** io_aliases,
                                               int* count,
                                               int* capacity,
                                               const IRCopyAlias* source,
                                               int source_count) {
    if (!io_aliases || !count || !capacity) return;
    if (*io_aliases) {
        ir_copy_aliases_clear(*io_aliases, count);
    } else {
        *count = 0;
    }
    if (source_count > *capacity) {
        *io_aliases = (IRCopyAlias*)safe_realloc(*io_aliases,
                                                 (size_t)source_count * sizeof(IRCopyAlias));
        *capacity = source_count;
    }
    if (!*io_aliases || !source || source_count <= 0) return;
    for (int i = 0; i < source_count; i++) {
        (*io_aliases)[i].target_name =
            source[i].target_name ? safe_strdup(source[i].target_name) : NULL;
        (*io_aliases)[i].source_name =
            source[i].source_name ? safe_strdup(source[i].source_name) : NULL;
    }
    *count = source_count;
}

static void ir_copy_aliases_intersect_in_place(IRCopyAlias* aliases,
                                               int* count,
                                               const IRCopyAlias* other,
                                               int other_count) {
    if (!aliases || !count) return;
    for (int i = *count - 1; i >= 0; i--) {
        int match_index =
            ir_copy_aliases_find_target(other, other_count, aliases[i].target_name);
        if (match_index < 0 ||
            !aliases[i].source_name ||
            !other[match_index].source_name ||
            strcmp(aliases[i].source_name, other[match_index].source_name) != 0) {
            ir_copy_aliases_remove_at(aliases, count, i);
        }
    }
}

static bool ir_copy_aliases_sets_equal(const IRCopyAlias* a,
                                       int a_count,
                                       const IRCopyAlias* b,
                                       int b_count) {
    if (a_count != b_count) return false;
    for (int i = 0; i < a_count; i++) {
        int idx = ir_copy_aliases_find_target(b, b_count, a[i].target_name);
        if (idx < 0) return false;
        if ((!a[i].source_name || !b[idx].source_name) &&
            a[i].source_name != b[idx].source_name) {
            return false;
        }
        if (a[i].source_name && b[idx].source_name &&
            strcmp(a[i].source_name, b[idx].source_name) != 0) {
            return false;
        }
    }
    return true;
}

static void ir_copy_flow_state_init(IRCopyFlowState* state) {
    if (!state) return;
    state->aliases = NULL;
    state->alias_count = 0;
    state->alias_capacity = 0;
}

static void ir_copy_flow_state_free(IRCopyFlowState* state) {
    if (!state) return;
    ir_copy_aliases_clear(state->aliases, &state->alias_count);
    if (state->aliases) free(state->aliases);
    ir_copy_flow_state_init(state);
}

static void ir_copy_flow_state_replace_with_clone(IRCopyFlowState* dst,
                                                  const IRCopyFlowState* src) {
    if (!dst) return;
    ir_copy_aliases_replace_with_clone(&dst->aliases,
                                       &dst->alias_count,
                                       &dst->alias_capacity,
                                       src ? src->aliases : NULL,
                                       src ? src->alias_count : 0);
}

static bool ir_copy_flow_state_equals(const IRCopyFlowState* a, const IRCopyFlowState* b) {
    return ir_copy_aliases_sets_equal(a ? a->aliases : NULL,
                                      a ? a->alias_count : 0,
                                      b ? b->aliases : NULL,
                                      b ? b->alias_count : 0);
}

static void ir_copy_aliases_merge_if_fallthrough(Stmt* then_branch,
                                                 Stmt* else_branch,
                                                 IRCopyAlias** io_aliases,
                                                 int* alias_count,
                                                 int* alias_capacity) {
    IRCopyAlias* aliases = io_aliases ? *io_aliases : NULL;
    int incoming_alias_count = alias_count ? *alias_count : 0;
    IRCopyAlias* then_aliases = ir_copy_aliases_clone_array(aliases, incoming_alias_count);
    IRCopyAlias* else_aliases = ir_copy_aliases_clone_array(aliases, incoming_alias_count);
    int then_alias_count = incoming_alias_count;
    int else_alias_count = incoming_alias_count;
    int then_alias_capacity = incoming_alias_count;
    int else_alias_capacity = incoming_alias_count;

    ir_apply_local_copy_propagation_stmt(then_branch,
                                         &then_aliases,
                                         &then_alias_count,
                                         &then_alias_capacity,
                                         true,
                                         false);
    if (else_branch) {
        ir_apply_local_copy_propagation_stmt(else_branch,
                                             &else_aliases,
                                             &else_alias_count,
                                             &else_alias_capacity,
                                             true,
                                             false);
    }

    ir_copy_aliases_replace_with_clone(io_aliases,
                                       alias_count,
                                       alias_capacity,
                                       then_aliases,
                                       then_alias_count);
    aliases = io_aliases ? *io_aliases : aliases;
    ir_copy_aliases_intersect_in_place(aliases,
                                       alias_count,
                                       else_aliases,
                                       else_alias_count);

    ir_copy_aliases_clear(then_aliases, &then_alias_count);
    ir_copy_aliases_clear(else_aliases, &else_alias_count);
    if (then_aliases) free(then_aliases);
    if (else_aliases) free(else_aliases);
}

static void ir_copy_aliases_merge_match_fallthrough(Stmt* stmt,
                                                    IRCopyAlias** io_aliases,
                                                    int* alias_count,
                                                    int* alias_capacity) {
    if (!stmt) return;

    IRCopyAlias* incoming_aliases = io_aliases ? *io_aliases : NULL;
    int incoming_alias_count = alias_count ? *alias_count : 0;
    bool merged_any = false;

    for (int arm = 0; arm < stmt->match_stmt.arm_count; arm++) {
        Stmt* body = (stmt->match_stmt.bodies && arm < stmt->match_stmt.arm_count)
                         ? stmt->match_stmt.bodies[arm]
                         : NULL;
        IRCopyAlias* branch_aliases =
            ir_copy_aliases_clone_array(incoming_aliases, incoming_alias_count);
        int branch_alias_count = incoming_alias_count;
        int branch_alias_capacity = incoming_alias_count;

        if (body) {
            ir_apply_local_copy_propagation_stmt(body,
                                                 &branch_aliases,
                                                 &branch_alias_count,
                                                 &branch_alias_capacity,
                                                 true,
                                                 false);
        }

        if (!merged_any) {
            ir_copy_aliases_replace_with_clone(io_aliases,
                                               alias_count,
                                               alias_capacity,
                                               branch_aliases,
                                               branch_alias_count);
            merged_any = true;
        } else {
            ir_copy_aliases_intersect_in_place(*io_aliases,
                                               alias_count,
                                               branch_aliases,
                                               branch_alias_count);
        }

        ir_copy_aliases_clear(branch_aliases, &branch_alias_count);
        if (branch_aliases) free(branch_aliases);
    }

    if (stmt->match_stmt.else_branch || !merged_any) {
        IRCopyAlias* else_aliases =
            ir_copy_aliases_clone_array(incoming_aliases, incoming_alias_count);
        int else_alias_count = incoming_alias_count;
        int else_alias_capacity = incoming_alias_count;

        if (stmt->match_stmt.else_branch) {
            ir_apply_local_copy_propagation_stmt(stmt->match_stmt.else_branch,
                                                 &else_aliases,
                                                 &else_alias_count,
                                                 &else_alias_capacity,
                                                 true,
                                                 false);
        }

        if (!merged_any) {
            ir_copy_aliases_replace_with_clone(io_aliases,
                                               alias_count,
                                               alias_capacity,
                                               else_aliases,
                                               else_alias_count);
        } else {
            ir_copy_aliases_intersect_in_place(*io_aliases,
                                               alias_count,
                                               else_aliases,
                                               else_alias_count);
        }

        ir_copy_aliases_clear(else_aliases, &else_alias_count);
        if (else_aliases) free(else_aliases);
    }
}

static void ir_copy_aliases_merge_loop_fallthrough(Stmt* stmt,
                                                   IRCopyAlias** io_aliases,
                                                   int* alias_count,
                                                   int* alias_capacity) {
    if (!stmt) return;

    Stmt* body_stmt = ir_loop_body_stmt(stmt);
    if (!body_stmt) return;

    IRStatementList body_view;
    Stmt* body_single_stmt[1];
    ir_stmt_as_statement_view(body_stmt, &body_view, body_single_stmt);
    if (body_view.stmt_count <= 0) return;

    IRCopyAlias* incoming_aliases = io_aliases ? *io_aliases : NULL;
    int incoming_alias_count = alias_count ? *alias_count : 0;
    IRCopyAlias* body_aliases =
        ir_copy_aliases_clone_array(incoming_aliases, incoming_alias_count);
    int body_alias_count = incoming_alias_count;
    int body_alias_capacity = incoming_alias_count;

    ir_apply_local_copy_propagation_range(&body_view,
                                          0,
                                          body_view.stmt_count - 1,
                                          &body_aliases,
                                          &body_alias_count,
                                          &body_alias_capacity,
                                          true,
                                          true);

    ir_copy_aliases_intersect_in_place(body_aliases,
                                       &body_alias_count,
                                       incoming_aliases,
                                       incoming_alias_count);
    ir_copy_aliases_replace_with_clone(io_aliases,
                                       alias_count,
                                       alias_capacity,
                                       body_aliases,
                                       body_alias_count);

    ir_copy_aliases_clear(body_aliases, &body_alias_count);
    if (body_aliases) free(body_aliases);
}

static void ir_copy_aliases_invalidate_for_stmt_fallthrough(Stmt* stmt,
                                                            IRCopyAlias* aliases,
                                                            int* alias_count) {
    if (!stmt || !aliases || !alias_count) return;

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            if (stmt->var_decl.name) {
                ir_copy_aliases_invalidate_by_name(aliases, alias_count, stmt->var_decl.name);
            }
            break;
        case STMT_VAR_TUPLE_DECL:
            if (stmt->var_tuple_decl.names) {
                for (int i = 0; i < stmt->var_tuple_decl.name_count; i++) {
                    const char* name = stmt->var_tuple_decl.names[i];
                    if (name) {
                        ir_copy_aliases_invalidate_by_name(aliases, alias_count, name);
                    }
                }
            }
            break;
        case STMT_ASSIGN:
            if (stmt->assign.name) {
                ir_copy_aliases_invalidate_by_name(aliases, alias_count, stmt->assign.name);
            }
            break;
        case STMT_BLOCK:
            for (int i = 0; i < stmt->block.stmt_count; i++) {
                ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt->block.statements[i],
                                                                aliases,
                                                                alias_count);
            }
            break;
        case STMT_IF:
            ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt->if_stmt.then_branch,
                                                            aliases,
                                                            alias_count);
            ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt->if_stmt.else_branch,
                                                            aliases,
                                                            alias_count);
            break;
        case STMT_MATCH:
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                if (stmt->match_stmt.bodies) {
                    ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt->match_stmt.bodies[i],
                                                                    aliases,
                                                                    alias_count);
                }
            }
            ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt->match_stmt.else_branch,
                                                            aliases,
                                                            alias_count);
            break;
        case STMT_WHILE:
            ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt->while_stmt.body,
                                                            aliases,
                                                            alias_count);
            break;
        case STMT_FOREACH:
            if (stmt->foreach.var_name) {
                ir_copy_aliases_invalidate_by_name(aliases, alias_count, stmt->foreach.var_name);
            }
            ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt->foreach.body,
                                                            aliases,
                                                            alias_count);
            break;
        case STMT_FOR_RANGE:
            if (stmt->for_range.var_name) {
                ir_copy_aliases_invalidate_by_name(aliases, alias_count, stmt->for_range.var_name);
            }
            ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt->for_range.body,
                                                            aliases,
                                                            alias_count);
            break;
        case STMT_EXPR:
        case STMT_ASSIGN_INDEX:
        case STMT_ASSIGN_FIELD:
        case STMT_DEFER:
        case STMT_RETURN:
        case STMT_BREAK:
        case STMT_CONTINUE:
        default:
            ir_copy_aliases_clear(aliases, alias_count);
            break;
    }
}

static void ir_rewrite_expr_with_aliases(Expr* expr, const IRCopyAlias* aliases, int alias_count) {
    if (!expr || !aliases || alias_count <= 0) return;

    if (expr->kind == EXPR_IDENTIFIER && expr->identifier) {
        const char* resolved = ir_copy_aliases_resolve(aliases, alias_count, expr->identifier);
        if (resolved && strcmp(resolved, expr->identifier) != 0) {
            char* replacement = safe_strdup(resolved);
            free(expr->identifier);
            expr->identifier = replacement;
        }
        return;
    }

    switch (expr->kind) {
        case EXPR_BINARY:
            ir_rewrite_expr_with_aliases(expr->binary.left, aliases, alias_count);
            ir_rewrite_expr_with_aliases(expr->binary.right, aliases, alias_count);
            break;
        case EXPR_UNARY:
            ir_rewrite_expr_with_aliases(expr->unary.operand, aliases, alias_count);
            break;
        case EXPR_AWAIT:
            ir_rewrite_expr_with_aliases(expr->await_expr.expr, aliases, alias_count);
            break;
        case EXPR_CALL:
            ir_rewrite_expr_with_aliases(expr->call.callee, aliases, alias_count);
            for (int i = 0; i < expr->call.arg_count; i++) {
                ir_rewrite_expr_with_aliases(expr->call.args[i], aliases, alias_count);
            }
            break;
        case EXPR_ARRAY:
        case EXPR_INDEX:
            ir_rewrite_expr_with_aliases(expr->index.array, aliases, alias_count);
            ir_rewrite_expr_with_aliases(expr->index.index, aliases, alias_count);
            break;
        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->array_literal.element_count; i++) {
                ir_rewrite_expr_with_aliases(expr->array_literal.elements[i], aliases, alias_count);
            }
            break;
        case EXPR_CAST:
            ir_rewrite_expr_with_aliases(expr->cast.value, aliases, alias_count);
            break;
        case EXPR_TRY:
            ir_rewrite_expr_with_aliases(expr->try_expr.expr, aliases, alias_count);
            break;
        case EXPR_TYPE_TEST:
            ir_rewrite_expr_with_aliases(expr->type_test.value, aliases, alias_count);
            break;
        case EXPR_IF:
            ir_rewrite_expr_with_aliases(expr->if_expr.condition, aliases, alias_count);
            ir_rewrite_expr_with_aliases(expr->if_expr.then_expr, aliases, alias_count);
            ir_rewrite_expr_with_aliases(expr->if_expr.else_expr, aliases, alias_count);
            break;
        case EXPR_MATCH:
            ir_rewrite_expr_with_aliases(expr->match_expr.subject, aliases, alias_count);
            break;
        case EXPR_RECORD_LITERAL:
            for (int i = 0; i < expr->record_literal.field_count; i++) {
                ir_rewrite_expr_with_aliases(expr->record_literal.field_values[i], aliases, alias_count);
            }
            break;
        case EXPR_FIELD_ACCESS:
            ir_rewrite_expr_with_aliases(expr->field_access.object, aliases, alias_count);
            break;
        case EXPR_TUPLE_LITERAL:
            for (int i = 0; i < expr->tuple_literal.element_count; i++) {
                ir_rewrite_expr_with_aliases(expr->tuple_literal.elements[i], aliases, alias_count);
            }
            break;
        case EXPR_TUPLE_ACCESS:
            ir_rewrite_expr_with_aliases(expr->tuple_access.tuple, aliases, alias_count);
            break;
        case EXPR_MAP_LITERAL:
            for (int i = 0; i < expr->map_literal.entry_count; i++) {
                ir_rewrite_expr_with_aliases(expr->map_literal.keys[i], aliases, alias_count);
                ir_rewrite_expr_with_aliases(expr->map_literal.values[i], aliases, alias_count);
            }
            break;
        case EXPR_SET_LITERAL:
            for (int i = 0; i < expr->set_literal.element_count; i++) {
                ir_rewrite_expr_with_aliases(expr->set_literal.elements[i], aliases, alias_count);
            }
            break;
        default:
            break;
    }
}

static void ir_collect_stmt_identifiers(Stmt* stmt, char*** io_names, int* io_count, int* io_capacity) {
    if (!stmt || !io_names || !io_count || !io_capacity) return;

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            ir_collect_expr_identifiers(stmt->var_decl.initializer, io_names, io_count, io_capacity);
            break;
        case STMT_VAR_TUPLE_DECL:
            ir_collect_expr_identifiers(stmt->var_tuple_decl.initializer, io_names, io_count, io_capacity);
            break;
        case STMT_EXPR:
            ir_collect_expr_identifiers(stmt->expr_stmt, io_names, io_count, io_capacity);
            break;
        case STMT_ASSIGN:
            ir_collect_expr_identifiers(stmt->assign.value, io_names, io_count, io_capacity);
            break;
        case STMT_ASSIGN_INDEX:
            ir_collect_expr_identifiers(stmt->assign_index.target, io_names, io_count, io_capacity);
            ir_collect_expr_identifiers(stmt->assign_index.index, io_names, io_count, io_capacity);
            ir_collect_expr_identifiers(stmt->assign_index.value, io_names, io_count, io_capacity);
            break;
        case STMT_ASSIGN_FIELD:
            ir_collect_expr_identifiers(stmt->assign_field.object, io_names, io_count, io_capacity);
            ir_collect_expr_identifiers(stmt->assign_field.value, io_names, io_count, io_capacity);
            break;
        case STMT_IF:
            ir_collect_expr_identifiers(stmt->if_stmt.condition, io_names, io_count, io_capacity);
            ir_collect_stmt_identifiers(stmt->if_stmt.then_branch, io_names, io_count, io_capacity);
            ir_collect_stmt_identifiers(stmt->if_stmt.else_branch, io_names, io_count, io_capacity);
            break;
        case STMT_MATCH:
            ir_collect_expr_identifiers(stmt->match_stmt.subject, io_names, io_count, io_capacity);
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                if (stmt->match_stmt.patterns) {
                    ir_collect_expr_identifiers(stmt->match_stmt.patterns[i], io_names, io_count, io_capacity);
                }
                if (stmt->match_stmt.guards) {
                    ir_collect_expr_identifiers(stmt->match_stmt.guards[i], io_names, io_count, io_capacity);
                }
                if (stmt->match_stmt.bodies) {
                    ir_collect_stmt_identifiers(stmt->match_stmt.bodies[i], io_names, io_count, io_capacity);
                }
            }
            ir_collect_stmt_identifiers(stmt->match_stmt.else_branch, io_names, io_count, io_capacity);
            break;
        case STMT_WHILE:
            ir_collect_expr_identifiers(stmt->while_stmt.condition, io_names, io_count, io_capacity);
            ir_collect_stmt_identifiers(stmt->while_stmt.body, io_names, io_count, io_capacity);
            break;
        case STMT_FOREACH:
            ir_collect_expr_identifiers(stmt->foreach.iterable, io_names, io_count, io_capacity);
            ir_collect_stmt_identifiers(stmt->foreach.body, io_names, io_count, io_capacity);
            break;
        case STMT_FOR_RANGE:
            ir_collect_expr_identifiers(stmt->for_range.start, io_names, io_count, io_capacity);
            ir_collect_expr_identifiers(stmt->for_range.end, io_names, io_count, io_capacity);
            ir_collect_stmt_identifiers(stmt->for_range.body, io_names, io_count, io_capacity);
            break;
        case STMT_RETURN:
            ir_collect_expr_identifiers(stmt->return_value, io_names, io_count, io_capacity);
            break;
        case STMT_DEFER:
            ir_collect_expr_identifiers(stmt->defer_expr, io_names, io_count, io_capacity);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < stmt->block.stmt_count; i++) {
                ir_collect_stmt_identifiers(stmt->block.statements[i], io_names, io_count, io_capacity);
            }
            break;
        default:
            break;
    }
}

static bool ir_expr_dead_store_safe(Expr* expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case EXPR_IDENTIFIER:
        case EXPR_LITERAL:
        case EXPR_NIL:
            return true;
        default:
            return false;
    }
}

static bool* ir_mark_local_assign_targets(IRStatementList* ir) {
    if (!ir || ir->stmt_count <= 0) return NULL;

    bool* assign_is_local = (bool*)safe_calloc((size_t)ir->stmt_count, sizeof(bool));
    char** known_locals = NULL;
    int known_count = 0;
    int known_capacity = 0;

    for (int i = 0; i < ir->stmt_count; i++) {
        Stmt* stmt = ir->statements[i];
        if (!stmt) continue;

        if (stmt->kind == STMT_ASSIGN && stmt->assign.name) {
            // Dead-store elimination is only sound for assignments targeting
            // locals declared within this lowered statement list. Outer-scope
            // locals may be read after the nested block/loop finishes.
            assign_is_local[i] = ir_string_list_contains(known_locals, known_count, stmt->assign.name);
        }

        if (stmt->kind == STMT_VAR_DECL && stmt->var_decl.name) {
            ir_string_list_add_unique_owned(&known_locals,
                                            &known_count,
                                            &known_capacity,
                                            stmt->var_decl.name);
            continue;
        }

        if (stmt->kind == STMT_VAR_TUPLE_DECL && stmt->var_tuple_decl.names) {
            for (int n = 0; n < stmt->var_tuple_decl.name_count; n++) {
                const char* name = stmt->var_tuple_decl.names[n];
                if (name) {
                    ir_string_list_add_unique_owned(&known_locals,
                                                    &known_count,
                                                    &known_capacity,
                                                    name);
                }
            }
        }
    }

    ir_string_list_free_owned(known_locals, known_count);
    if (known_locals) free(known_locals);
    return assign_is_local;
}

static void ir_apply_local_copy_propagation_stmt(Stmt* stmt,
                                                 IRCopyAlias** io_aliases,
                                                 int* io_alias_count,
                                                 int* io_alias_capacity,
                                                 bool preserve_outgoing_aliases,
                                                 bool structured_cfg_exit) {
    IRCopyAlias* aliases = io_aliases ? *io_aliases : NULL;
    int alias_count = io_alias_count ? *io_alias_count : 0;
    int alias_capacity = io_alias_capacity ? *io_alias_capacity : 0;

    if (!stmt) return;

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            ir_rewrite_expr_with_aliases(stmt->var_decl.initializer, aliases, alias_count);
            if (stmt->var_decl.name) {
                ir_copy_aliases_invalidate_by_name(aliases, &alias_count, stmt->var_decl.name);
            }
            if (stmt->var_decl.name &&
                stmt->var_decl.initializer &&
                stmt->var_decl.initializer->kind == EXPR_IDENTIFIER &&
                stmt->var_decl.initializer->identifier) {
                ir_copy_aliases_set(&aliases,
                                    &alias_count,
                                    &alias_capacity,
                                    stmt->var_decl.name,
                                    stmt->var_decl.initializer->identifier);
            }
            break;
        case STMT_ASSIGN:
            ir_rewrite_expr_with_aliases(stmt->assign.value, aliases, alias_count);
            if (stmt->assign.name) {
                ir_copy_aliases_invalidate_by_name(aliases, &alias_count, stmt->assign.name);
            }
            if (stmt->assign.name &&
                stmt->assign.op == TOKEN_ASSIGN &&
                stmt->assign.value &&
                stmt->assign.value->kind == EXPR_IDENTIFIER &&
                stmt->assign.value->identifier) {
                ir_copy_aliases_set(&aliases,
                                    &alias_count,
                                    &alias_capacity,
                                    stmt->assign.name,
                                    stmt->assign.value->identifier);
            }
            break;
        case STMT_BLOCK:
            if (preserve_outgoing_aliases && stmt->block.stmt_count > 0) {
                IRStatementList nested_ir;
                nested_ir.statements = stmt->block.statements;
                nested_ir.stmt_count = stmt->block.stmt_count;
                nested_ir.stmt_capacity = stmt->block.stmt_count;
                ir_apply_local_copy_propagation_range(&nested_ir,
                                                      0,
                                                      nested_ir.stmt_count - 1,
                                                      &aliases,
                                                      &alias_count,
                                                      &alias_capacity,
                                                      true,
                                                      false);
            } else if (preserve_outgoing_aliases) {
                ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt, aliases, &alias_count);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
        case STMT_IF:
            ir_rewrite_expr_with_aliases(stmt->if_stmt.condition, aliases, alias_count);
            if (preserve_outgoing_aliases) {
                if (structured_cfg_exit) {
                    break;
                }
                ir_copy_aliases_merge_if_fallthrough(stmt->if_stmt.then_branch,
                                                     stmt->if_stmt.else_branch,
                                                     &aliases,
                                                     &alias_count,
                                                     &alias_capacity);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
        case STMT_VAR_TUPLE_DECL:
            ir_rewrite_expr_with_aliases(stmt->var_tuple_decl.initializer, aliases, alias_count);
            if (stmt->var_tuple_decl.names) {
                for (int n = 0; n < stmt->var_tuple_decl.name_count; n++) {
                    const char* name = stmt->var_tuple_decl.names[n];
                    if (name) {
                        ir_copy_aliases_invalidate_by_name(aliases, &alias_count, name);
                    }
                }
            }
            if (preserve_outgoing_aliases) {
                ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt, aliases, &alias_count);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
        case STMT_EXPR:
            ir_rewrite_expr_with_aliases(stmt->expr_stmt, aliases, alias_count);
            if (preserve_outgoing_aliases) {
                ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt, aliases, &alias_count);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
        case STMT_ASSIGN_INDEX:
            ir_rewrite_expr_with_aliases(stmt->assign_index.target, aliases, alias_count);
            ir_rewrite_expr_with_aliases(stmt->assign_index.index, aliases, alias_count);
            ir_rewrite_expr_with_aliases(stmt->assign_index.value, aliases, alias_count);
            if (preserve_outgoing_aliases) {
                ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt, aliases, &alias_count);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
        case STMT_ASSIGN_FIELD:
            ir_rewrite_expr_with_aliases(stmt->assign_field.object, aliases, alias_count);
            ir_rewrite_expr_with_aliases(stmt->assign_field.value, aliases, alias_count);
            if (preserve_outgoing_aliases) {
                ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt, aliases, &alias_count);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
        case STMT_MATCH:
            ir_rewrite_expr_with_aliases(stmt->match_stmt.subject, aliases, alias_count);
            if (preserve_outgoing_aliases) {
                if (structured_cfg_exit) {
                    break;
                }
                ir_copy_aliases_merge_match_fallthrough(stmt,
                                                        &aliases,
                                                        &alias_count,
                                                        &alias_capacity);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
        case STMT_WHILE:
            ir_rewrite_expr_with_aliases(stmt->while_stmt.condition, aliases, alias_count);
            if (preserve_outgoing_aliases) {
                if (structured_cfg_exit) {
                    break;
                }
                ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt, aliases, &alias_count);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
        case STMT_FOREACH:
            ir_rewrite_expr_with_aliases(stmt->foreach.iterable, aliases, alias_count);
            if (preserve_outgoing_aliases) {
                if (structured_cfg_exit) {
                    break;
                }
                ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt, aliases, &alias_count);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
        case STMT_FOR_RANGE:
            ir_rewrite_expr_with_aliases(stmt->for_range.start, aliases, alias_count);
            ir_rewrite_expr_with_aliases(stmt->for_range.end, aliases, alias_count);
            if (preserve_outgoing_aliases) {
                if (structured_cfg_exit) {
                    break;
                }
                ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt, aliases, &alias_count);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
        case STMT_RETURN:
            ir_rewrite_expr_with_aliases(stmt->return_value, aliases, alias_count);
            if (preserve_outgoing_aliases) {
                ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt, aliases, &alias_count);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            if (preserve_outgoing_aliases && structured_cfg_exit) {
                break;
            }
            if (preserve_outgoing_aliases) {
                ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt, aliases, &alias_count);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
        case STMT_DEFER:
            ir_rewrite_expr_with_aliases(stmt->defer_expr, aliases, alias_count);
            if (preserve_outgoing_aliases) {
                ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt, aliases, &alias_count);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
        default:
            if (preserve_outgoing_aliases) {
                ir_copy_aliases_invalidate_for_stmt_fallthrough(stmt, aliases, &alias_count);
            } else {
                ir_copy_aliases_clear(aliases, &alias_count);
            }
            break;
    }

    if (io_aliases) *io_aliases = aliases;
    if (io_alias_count) *io_alias_count = alias_count;
    if (io_alias_capacity) *io_alias_capacity = alias_capacity;
}

static void ir_apply_local_copy_propagation_range(IRStatementList* ir,
                                                  int start_index,
                                                  int end_index,
                                                  IRCopyAlias** io_aliases,
                                                  int* io_alias_count,
                                                  int* io_alias_capacity,
                                                  bool preserve_outgoing_aliases,
                                                  bool structured_cfg_exit) {
    IRCopyAlias* aliases = io_aliases ? *io_aliases : NULL;
    int alias_count = io_alias_count ? *io_alias_count : 0;
    int alias_capacity = io_alias_capacity ? *io_alias_capacity : 0;

    for (int i = start_index; i <= end_index; i++) {
        Stmt* stmt = ir->statements[i];
        if (!stmt) continue;
        bool is_final_stmt = (i == end_index);
        ir_apply_local_copy_propagation_stmt(stmt,
                                             &aliases,
                                             &alias_count,
                                             &alias_capacity,
                                             is_final_stmt && preserve_outgoing_aliases,
                                             is_final_stmt && structured_cfg_exit);
    }

    if (io_aliases) *io_aliases = aliases;
    if (io_alias_count) *io_alias_count = alias_count;
    if (io_alias_capacity) *io_alias_capacity = alias_capacity;
}

static void ir_simulate_local_copy_block(const IRBasicBlock* block,
                                         const IRCopyFlowState* in_state,
                                         IRCopyFlowState* out_state) {
    if (!block || !out_state) return;

    bool preserve_outgoing_aliases =
        block->successor_count > 0 ||
        block->terminator_kind == IR_BLOCK_TERM_BREAK ||
        block->terminator_kind == IR_BLOCK_TERM_CONTINUE ||
        block->terminator_kind == IR_BLOCK_TERM_RETURN;

    IRStatementList cloned_ir;
    ir_clone_statement_range(&cloned_ir,
                             block->statements,
                             block->statement_count,
                             block->start_index,
                             block->end_index);
    ir_copy_flow_state_replace_with_clone(out_state, in_state);
    if (cloned_ir.stmt_count > 0) {
        ir_apply_local_copy_propagation_range(&cloned_ir,
                                              0,
                                              cloned_ir.stmt_count - 1,
                                              &out_state->aliases,
                                              &out_state->alias_count,
                                              &out_state->alias_capacity,
                                              preserve_outgoing_aliases,
                                              true);
    }
    ir_free_cloned_statement_range(&cloned_ir);
}

static bool ir_copy_merge_block_predecessors_seeded(const IRBasicBlockList* blocks,
                                                    const IRCopyFlowState* out_states,
                                                    const bool* out_reachable,
                                                    int block_index,
                                                    int entry_block_index,
                                                    const IRCopyFlowState* entry_state,
                                                    IRCopyFlowState* merged_in) {
    if (!blocks || !merged_in || block_index < 0 || block_index >= blocks->count) return false;

    ir_copy_flow_state_free(merged_in);
    if (!entry_state && block_index == 0) {
        return true;
    }

    bool merged_any = false;
    if (entry_state && block_index == entry_block_index) {
        ir_copy_flow_state_replace_with_clone(merged_in, entry_state);
        merged_any = true;
    }

    const IRBasicBlock* block = &blocks->blocks[block_index];
    for (int i = 0; i < block->predecessor_count; i++) {
        int pred_index = block->predecessors[i];
        if (pred_index < 0 || pred_index >= blocks->count) continue;
        if (!out_reachable || !out_reachable[pred_index]) continue;

        if (!merged_any) {
            ir_copy_flow_state_replace_with_clone(merged_in, &out_states[pred_index]);
            merged_any = true;
        } else {
            ir_copy_aliases_intersect_in_place(merged_in->aliases,
                                               &merged_in->alias_count,
                                               out_states[pred_index].aliases,
                                               out_states[pred_index].alias_count);
        }
    }

    return merged_any;
}

static bool ir_copy_merge_block_predecessors(const IRBasicBlockList* blocks,
                                             const IRCopyFlowState* out_states,
                                             const bool* out_reachable,
                                             int block_index,
                                             IRCopyFlowState* merged_in) {
    return ir_copy_merge_block_predecessors_seeded(blocks,
                                                   out_states,
                                                   out_reachable,
                                                   block_index,
                                                   -1,
                                                   NULL,
                                                   merged_in);
}

static void ir_apply_local_copy_propagation(IRStatementList* ir) {
    if (!ir || !ir->statements || ir->stmt_count <= 1) return;

    IRBasicBlockList blocks = ir_build_basic_blocks(ir);
    if (blocks.count <= 0) {
        ir_basic_block_list_free(&blocks);
        return;
    }

    IRCopyFlowState* in_states =
        (IRCopyFlowState*)safe_calloc((size_t)blocks.count, sizeof(IRCopyFlowState));
    IRCopyFlowState* out_states =
        (IRCopyFlowState*)safe_calloc((size_t)blocks.count, sizeof(IRCopyFlowState));
    bool* in_reachable = (bool*)safe_calloc((size_t)blocks.count, sizeof(bool));
    bool* out_reachable = (bool*)safe_calloc((size_t)blocks.count, sizeof(bool));
    int worklist_capacity = blocks.count > 0 ? blocks.count : 1;
    int* worklist = (int*)safe_calloc((size_t)worklist_capacity, sizeof(int));
    bool* queued = (bool*)safe_calloc((size_t)blocks.count, sizeof(bool));
    int worklist_head = 0;
    int worklist_tail = 0;

    for (int i = 0; i < blocks.count; i++) {
        ir_copy_flow_state_init(&in_states[i]);
        ir_copy_flow_state_init(&out_states[i]);
    }

    ir_worklist_enqueue(&worklist, &worklist_tail, &worklist_capacity, queued, 0);

    while (worklist_head < worklist_tail) {
        int block_index = worklist[worklist_head++];
        queued[block_index] = false;

        IRCopyFlowState merged_in;
        IRCopyFlowState new_out;
        ir_copy_flow_state_init(&merged_in);
        ir_copy_flow_state_init(&new_out);

        bool block_reachable =
            ir_copy_merge_block_predecessors(&blocks,
                                             out_states,
                                             out_reachable,
                                             block_index,
                                             &merged_in);
        if (!block_reachable) {
            ir_copy_flow_state_free(&in_states[block_index]);
            ir_copy_flow_state_free(&out_states[block_index]);
            in_reachable[block_index] = false;
            out_reachable[block_index] = false;
            ir_copy_flow_state_free(&merged_in);
            ir_copy_flow_state_free(&new_out);
            continue;
        }

        bool in_changed =
            !in_reachable[block_index] ||
            !ir_copy_flow_state_equals(&in_states[block_index], &merged_in);
        if (in_changed) {
            ir_copy_flow_state_replace_with_clone(&in_states[block_index], &merged_in);
            in_reachable[block_index] = true;
        }

        ir_simulate_local_copy_block(&blocks.blocks[block_index], &merged_in, &new_out);
        bool out_changed =
            !out_reachable[block_index] ||
            !ir_copy_flow_state_equals(&out_states[block_index], &new_out);
        if (out_changed) {
            ir_copy_flow_state_replace_with_clone(&out_states[block_index], &new_out);
            out_reachable[block_index] = true;

            for (int i = 0; i < blocks.blocks[block_index].successor_count; i++) {
                int succ_index = blocks.blocks[block_index].successors[i];
                if (succ_index < 0 || succ_index >= blocks.count) continue;
                ir_worklist_enqueue(&worklist,
                                    &worklist_tail,
                                    &worklist_capacity,
                                    queued,
                                    succ_index);
            }
        }

        ir_copy_flow_state_free(&merged_in);
        ir_copy_flow_state_free(&new_out);
    }

    for (int i = 0; i < blocks.count; i++) {
        if (!in_reachable[i]) continue;
        IRCopyFlowState block_state;
        ir_copy_flow_state_init(&block_state);
        ir_copy_flow_state_replace_with_clone(&block_state, &in_states[i]);
        IRStatementList block_ir = {
            blocks.blocks[i].statements,
            blocks.blocks[i].statement_count,
            blocks.blocks[i].statement_count
        };
        ir_apply_local_copy_propagation_range(&block_ir,
                                              blocks.blocks[i].start_index,
                                              blocks.blocks[i].end_index,
                                              &block_state.aliases,
                                              &block_state.alias_count,
                                              &block_state.alias_capacity,
                                              blocks.blocks[i].successor_count > 0,
                                              true);
        ir_copy_flow_state_free(&block_state);
    }

    for (int i = 0; i < blocks.count; i++) {
        ir_copy_flow_state_free(&in_states[i]);
        ir_copy_flow_state_free(&out_states[i]);
    }
    free(in_states);
    free(out_states);
    free(in_reachable);
    free(out_reachable);
    free(worklist);
    free(queued);
    ir_basic_block_list_free(&blocks);
}

static void ir_apply_local_dead_store_elimination(Compiler* comp,
                                                  IRStatementList* ir,
                                                  bool allow_dead_store_elimination) {
    if (!ir || !ir->statements || ir->stmt_count <= 0) return;
    if (!allow_dead_store_elimination) return;

    bool top_level_globals = comp && comp->is_top_level && comp->depth == 0;
    bool* keep_stmt = (bool*)safe_calloc((size_t)ir->stmt_count, sizeof(bool));
    bool* assign_is_local = ir_mark_local_assign_targets(ir);
    for (int i = 0; i < ir->stmt_count; i++) {
        keep_stmt[i] = true;
    }

    char** live_names = NULL;
    int live_count = 0;
    int live_capacity = 0;

    for (int i = ir->stmt_count - 1; i >= 0; i--) {
        Stmt* stmt = ir->statements[i];
        if (!stmt) {
            keep_stmt[i] = false;
            continue;
        }

        char** used_names = NULL;
        int used_count = 0;
        int used_capacity = 0;
        bool removed = false;
        bool is_local_def = false;

        if (stmt->kind == STMT_VAR_DECL) {
            ir_collect_expr_identifiers(stmt->var_decl.initializer, &used_names, &used_count, &used_capacity);
            is_local_def = !top_level_globals && stmt->var_decl.name != NULL;

            bool removable_candidate =
                is_local_def &&
                stmt->var_decl.initializer &&
                ir_expr_dead_store_safe(stmt->var_decl.initializer) &&
                !ir_string_list_contains(live_names, live_count, stmt->var_decl.name);
            if (removable_candidate) {
                keep_stmt[i] = false;
                removed = true;
            } else if (is_local_def) {
                ir_string_list_remove_owned(live_names, &live_count, stmt->var_decl.name);
            }
        } else if (stmt->kind == STMT_ASSIGN) {
            ir_collect_expr_identifiers(stmt->assign.value, &used_names, &used_count, &used_capacity);
            is_local_def =
                !top_level_globals &&
                stmt->assign.name != NULL &&
                assign_is_local &&
                assign_is_local[i];

            bool removable_candidate =
                is_local_def &&
                stmt->assign.op == TOKEN_ASSIGN &&
                stmt->assign.value &&
                ir_expr_dead_store_safe(stmt->assign.value) &&
                !ir_string_list_contains(live_names, live_count, stmt->assign.name);
            if (removable_candidate) {
                keep_stmt[i] = false;
                removed = true;
            } else if (is_local_def) {
                ir_string_list_remove_owned(live_names, &live_count, stmt->assign.name);
            }
        } else if (stmt->kind == STMT_VAR_TUPLE_DECL) {
            ir_collect_expr_identifiers(stmt->var_tuple_decl.initializer, &used_names, &used_count, &used_capacity);
            if (!top_level_globals && stmt->var_tuple_decl.names) {
                for (int n = 0; n < stmt->var_tuple_decl.name_count; n++) {
                    const char* name = stmt->var_tuple_decl.names[n];
                    if (name) {
                        ir_string_list_remove_owned(live_names, &live_count, name);
                    }
                }
            }
        } else {
            ir_collect_stmt_identifiers(stmt, &used_names, &used_count, &used_capacity);
        }

        if (!removed) {
            for (int u = 0; u < used_count; u++) {
                if (used_names[u]) {
                    ir_string_list_add_unique_owned(&live_names,
                                                    &live_count,
                                                    &live_capacity,
                                                    used_names[u]);
                }
            }
        }

        ir_string_list_free_owned(used_names, used_count);
        if (used_names) free(used_names);
    }

    int write = 0;
    for (int i = 0; i < ir->stmt_count; i++) {
        if (!keep_stmt[i]) continue;
        ir->statements[write++] = ir->statements[i];
    }
    ir->stmt_count = write;

    ir_string_list_free_owned(live_names, live_count);
    if (live_names) free(live_names);
    if (assign_is_local) free(assign_is_local);
    if (keep_stmt) free(keep_stmt);
}

static bool ir_stmt_guarantees_termination(Stmt* stmt) {
    if (!stmt) return false;

    switch (stmt->kind) {
        case STMT_RETURN:
        case STMT_BREAK:
        case STMT_CONTINUE:
            return true;
        case STMT_IF:
            if (!stmt->if_stmt.else_branch) return false;
            return ir_stmt_guarantees_termination(stmt->if_stmt.then_branch) &&
                   ir_stmt_guarantees_termination(stmt->if_stmt.else_branch);
        case STMT_BLOCK: {
            if (!stmt->block.statements || stmt->block.stmt_count <= 0) return false;
            for (int i = 0; i < stmt->block.stmt_count; i++) {
                if (ir_stmt_guarantees_termination(stmt->block.statements[i])) {
                    return true;
                }
            }
            return false;
        }
        case STMT_MATCH: {
            if (!stmt->match_stmt.else_branch) return false;
            if (!ir_stmt_guarantees_termination(stmt->match_stmt.else_branch)) return false;
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                if (!ir_stmt_guarantees_termination(stmt->match_stmt.bodies[i])) {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

static bool ir_stmt_guarantees_function_return(Stmt* stmt) {
    if (!stmt) return false;

    switch (stmt->kind) {
        case STMT_RETURN:
            return true;
        case STMT_IF:
            if (!stmt->if_stmt.else_branch) return false;
            return ir_stmt_guarantees_function_return(stmt->if_stmt.then_branch) &&
                   ir_stmt_guarantees_function_return(stmt->if_stmt.else_branch);
        case STMT_BLOCK: {
            if (!stmt->block.statements || stmt->block.stmt_count <= 0) return false;
            for (int i = 0; i < stmt->block.stmt_count; i++) {
                if (ir_stmt_guarantees_function_return(stmt->block.statements[i])) {
                    return true;
                }
            }
            return false;
        }
        case STMT_MATCH: {
            if (!stmt->match_stmt.else_branch) return false;
            if (!ir_stmt_guarantees_function_return(stmt->match_stmt.else_branch)) return false;
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                if (!ir_stmt_guarantees_function_return(stmt->match_stmt.bodies[i])) {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

static bool ir_stmt_contains_loop_control(Stmt* stmt) {
    if (!stmt) return false;

    switch (stmt->kind) {
        case STMT_BREAK:
        case STMT_CONTINUE:
            return true;
        case STMT_BLOCK:
            for (int i = 0; i < stmt->block.stmt_count; i++) {
                if (ir_stmt_contains_loop_control(stmt->block.statements[i])) {
                    return true;
                }
            }
            return false;
        case STMT_IF:
            return ir_stmt_contains_loop_control(stmt->if_stmt.then_branch) ||
                   ir_stmt_contains_loop_control(stmt->if_stmt.else_branch);
        case STMT_MATCH:
            if (ir_stmt_contains_loop_control(stmt->match_stmt.else_branch)) return true;
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                if (ir_stmt_contains_loop_control(stmt->match_stmt.bodies[i])) {
                    return true;
                }
            }
            return false;
        case STMT_WHILE:
            return ir_stmt_contains_loop_control(stmt->while_stmt.body);
        case STMT_FOREACH:
            return ir_stmt_contains_loop_control(stmt->foreach.body);
        case STMT_FOR_RANGE:
            return ir_stmt_contains_loop_control(stmt->for_range.body);
        default:
            return false;
    }
}

static Expr* ir_try_fold_constant_expr(Compiler* comp, Expr* expr) {
    if (!expr) return NULL;
    if (expr->kind == EXPR_LITERAL || expr->kind == EXPR_NIL) {
        return expr_clone(expr);
    }
    return fold_expression_recursive(comp, expr);
}

static bool ir_try_eval_expr_int(Compiler* comp, Expr* expr, int64_t* out_value) {
    if (out_value) *out_value = 0;
    Expr* folded = ir_try_fold_constant_expr(comp, expr);
    if (!folded) return false;

    bool ok = false;
    int64_t value = 0;
    if (folded->kind == EXPR_LITERAL &&
        folded->type &&
        folded->type->kind == TYPE_INT) {
        ok = true;
        value = folded->literal.as_int;
    }
    expr_free(folded);

    if (ok && out_value) *out_value = value;
    return ok;
}

static bool ir_try_eval_iterable_empty(Expr* expr, bool* out_is_empty) {
    if (out_is_empty) *out_is_empty = false;
    if (!expr) return false;

    if (expr->kind == EXPR_CAST && expr->cast.value) {
        return ir_try_eval_iterable_empty(expr->cast.value, out_is_empty);
    }

    if (expr->kind == EXPR_ARRAY_LITERAL) {
        if (out_is_empty) *out_is_empty = (expr->array_literal.element_count == 0);
        return true;
    }

    return false;
}

static void ir_const_enum_value_init(IRConstEnumValue* value) {
    if (!value) return;
    value->enum_decl = NULL;
    value->member_index = -1;
    value->payload_values = NULL;
    value->payload_count = 0;
}

static void ir_const_enum_value_free(IRConstEnumValue* value) {
    if (!value) return;
    if (value->payload_values) {
        for (int i = 0; i < value->payload_count; i++) {
            if (value->payload_values[i]) expr_free(value->payload_values[i]);
        }
        free(value->payload_values);
    }
    ir_const_enum_value_init(value);
}

static bool ir_try_eval_enum_member_tag(Compiler* comp,
                                        Expr* expr,
                                        Stmt** out_enum_decl,
                                        int* out_member_index) {
    if (out_enum_decl) *out_enum_decl = NULL;
    if (out_member_index) *out_member_index = -1;
    if (!comp || !expr) return false;

    char* symbol_name = NULL;
    if (expr->kind == EXPR_IDENTIFIER && expr->identifier) {
        symbol_name = safe_strdup(expr->identifier);
    } else if (expr->kind == EXPR_CALL) {
        symbol_name = match_pattern_call_symbol_name(expr);
    } else {
        return false;
    }

    if (!symbol_name) return false;
    bool found = find_enum_member_decl(comp, symbol_name, out_enum_decl, out_member_index);
    free(symbol_name);
    return found;
}

static bool ir_try_eval_const_enum_value(Compiler* comp, Expr* expr, IRConstEnumValue* out_value) {
    if (!out_value) return false;
    ir_const_enum_value_free(out_value);
    if (!comp || !expr) return false;

    Stmt* enum_stmt = NULL;
    int member_index = -1;
    if (!ir_try_eval_enum_member_tag(comp, expr, &enum_stmt, &member_index) ||
        !enum_stmt || enum_stmt->kind != STMT_ENUM_DECL ||
        member_index < 0 || member_index >= enum_stmt->enum_decl.member_count) {
        return false;
    }

    int expected_payload_count = (enum_stmt->enum_decl.member_payload_counts &&
                                  member_index < enum_stmt->enum_decl.member_count)
                                     ? enum_stmt->enum_decl.member_payload_counts[member_index]
                                     : 0;
    Expr** payload_args = NULL;
    int payload_arg_count = 0;

    if (expr->kind == EXPR_IDENTIFIER) {
        if (expected_payload_count != 0) {
            // Payload constructors are values only when invoked as calls.
            return false;
        }
    } else if (expr->kind == EXPR_CALL) {
        payload_args = expr->call.args;
        payload_arg_count = expr->call.arg_count;
        if (payload_arg_count != expected_payload_count) return false;
    } else {
        return false;
    }

    if (expected_payload_count > 0) {
        out_value->payload_values = (Expr**)safe_calloc((size_t)expected_payload_count, sizeof(Expr*));
        out_value->payload_count = expected_payload_count;

        for (int i = 0; i < expected_payload_count; i++) {
            Expr* folded_payload = ir_try_fold_constant_expr(comp, payload_args[i]);
            if (!folded_payload ||
                (folded_payload->kind != EXPR_LITERAL && folded_payload->kind != EXPR_NIL)) {
                if (folded_payload) expr_free(folded_payload);
                ir_const_enum_value_free(out_value);
                return false;
            }
            out_value->payload_values[i] = folded_payload;
        }
    }

    out_value->enum_decl = enum_stmt;
    out_value->member_index = member_index;
    return true;
}

static bool ir_enum_decls_equal(Stmt* a, Stmt* b) {
    if (a == b) return true;
    if (!a || !b || a->kind != STMT_ENUM_DECL || b->kind != STMT_ENUM_DECL) return false;

    const char* a_name = a->enum_decl.name;
    const char* b_name = b->enum_decl.name;
    if (!a_name || !b_name) return false;
    return strcmp(a_name, b_name) == 0;
}

static bool ir_const_enum_values_equal(const IRConstEnumValue* a, const IRConstEnumValue* b) {
    if (!a || !b || !a->enum_decl || !b->enum_decl) return false;
    if (!ir_enum_decls_equal(a->enum_decl, b->enum_decl)) return false;

    if (a->member_index != b->member_index) return false;
    if (a->payload_count != b->payload_count) return false;

    for (int i = 0; i < a->payload_count; i++) {
        if (!expr_equals(a->payload_values[i], b->payload_values[i])) {
            return false;
        }
    }
    return true;
}

static bool ir_const_enum_value_as_scalar_int(const IRConstEnumValue* value, int64_t* out_value) {
    if (out_value) *out_value = 0;
    if (!value || !value->enum_decl || value->enum_decl->kind != STMT_ENUM_DECL) return false;
    if (value->enum_decl->enum_decl.has_payload_members) return false;
    if (!value->enum_decl->enum_decl.member_values) return false;
    if (value->member_index < 0 || value->member_index >= value->enum_decl->enum_decl.member_count) return false;

    if (out_value) {
        *out_value = value->enum_decl->enum_decl.member_values[value->member_index];
    }
    return true;
}

static bool ir_try_eval_condition_bool(Compiler* comp, Expr* condition, bool* out_value) {
    if (out_value) *out_value = false;
    if (!condition) return false;

    if (condition->kind == EXPR_LITERAL &&
        condition->type &&
        condition->type->kind == TYPE_BOOL) {
        if (out_value) *out_value = condition->literal.as_int != 0;
        return true;
    }

    if (condition->kind == EXPR_BINARY &&
        condition->binary.left &&
        condition->binary.right &&
        (condition->binary.op == TOKEN_AND || condition->binary.op == TOKEN_OR)) {
        bool left_value = false;
        if (ir_try_eval_condition_bool(comp, condition->binary.left, &left_value)) {
            if (condition->binary.op == TOKEN_AND && !left_value) {
                if (out_value) *out_value = false;
                return true;
            }
            if (condition->binary.op == TOKEN_OR && left_value) {
                if (out_value) *out_value = true;
                return true;
            }

            bool right_value = false;
            if (ir_try_eval_condition_bool(comp, condition->binary.right, &right_value)) {
                if (out_value) *out_value = right_value;
                return true;
            }
        }
    }

    Expr* folded = fold_expression_recursive(comp, condition);
    if (!folded) return false;

    bool has_value = false;
    bool value = false;
    if (folded->kind == EXPR_LITERAL &&
        folded->type &&
        folded->type->kind == TYPE_BOOL) {
        value = folded->literal.as_int != 0;
        has_value = true;
    }
    expr_free(folded);

    if (has_value && out_value) *out_value = value;
    return has_value;
}

static void ir_lower_statement_dce(Compiler* comp, Stmt* stmt, IRStatementList* out) {
    if (!stmt || !out) return;

    if (stmt->kind == STMT_IF) {
        bool condition_value = false;
        if (ir_try_eval_condition_bool(comp, stmt->if_stmt.condition, &condition_value)) {
            if (condition_value) {
                if (stmt->if_stmt.then_branch) {
                    ir_lower_statement_dce(comp, stmt->if_stmt.then_branch, out);
                }
            } else if (stmt->if_stmt.else_branch) {
                ir_lower_statement_dce(comp, stmt->if_stmt.else_branch, out);
            }
            return;
        }
    } else if (stmt->kind == STMT_WHILE) {
        bool condition_value = false;
        if (ir_try_eval_condition_bool(comp, stmt->while_stmt.condition, &condition_value)) {
            if (!condition_value) {
                // Dead loop body: while(false) / while(<constant-false-expression>)
                return;
            }
            if (!ir_stmt_contains_loop_control(stmt->while_stmt.body) &&
                ir_stmt_guarantees_function_return(stmt->while_stmt.body)) {
                // while(true) { <always-returns> } executes the body once and returns.
                ir_lower_statement_dce(comp, stmt->while_stmt.body, out);
                return;
            }
        }
    } else if (stmt->kind == STMT_FOR_RANGE) {
        int64_t start_value = 0;
        int64_t end_value = 0;
        if (ir_try_eval_expr_int(comp, stmt->for_range.start, &start_value) &&
            ir_try_eval_expr_int(comp, stmt->for_range.end, &end_value) &&
            start_value >= end_value) {
            // Dead range loop body: foreach (i in start..end) where start >= end.
            return;
        }
    } else if (stmt->kind == STMT_FOREACH) {
        bool iterable_is_empty = false;
        if (ir_try_eval_iterable_empty(stmt->foreach.iterable, &iterable_is_empty) &&
            iterable_is_empty) {
            // Dead foreach body: iterable is compile-time known empty.
            return;
        }
    } else if (stmt->kind == STMT_MATCH) {
        Expr* folded_subject = ir_try_fold_constant_expr(comp, stmt->match_stmt.subject);
        IRConstEnumValue const_enum_subject;
        ir_const_enum_value_init(&const_enum_subject);
        bool has_const_enum_subject = ir_try_eval_const_enum_value(comp,
                                                                   stmt->match_stmt.subject,
                                                                   &const_enum_subject);

        bool has_guards = false;
        if (stmt->match_stmt.guards) {
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                if (stmt->match_stmt.guards[i]) {
                    has_guards = true;
                    break;
                }
            }
        }

        if (!has_guards && (folded_subject || has_const_enum_subject)) {
            bool saw_unknown_pattern = false;
            bool indeterminate_match = false;
            int matched_arm = -1;

            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                Expr* pattern_expr = stmt->match_stmt.patterns ? stmt->match_stmt.patterns[i] : NULL;
                int payload_bind_count = (stmt->match_stmt.payload_binding_counts &&
                                          i < stmt->match_stmt.arm_count)
                                             ? stmt->match_stmt.payload_binding_counts[i]
                                             : 0;

                bool pattern_known = false;
                bool equals_subject = false;

                if (has_const_enum_subject) {
                    Stmt* pattern_enum_decl = NULL;
                    int pattern_member_index = -1;
                    if (ir_try_eval_enum_member_tag(comp,
                                                    pattern_expr,
                                                    &pattern_enum_decl,
                                                    &pattern_member_index)) {
                        if (!ir_enum_decls_equal(pattern_enum_decl, const_enum_subject.enum_decl) ||
                            pattern_member_index != const_enum_subject.member_index) {
                            pattern_known = true;
                            equals_subject = false;
                        } else if (payload_bind_count <= 0) {
                            IRConstEnumValue const_enum_pattern;
                            ir_const_enum_value_init(&const_enum_pattern);
                            if (ir_try_eval_const_enum_value(comp, pattern_expr, &const_enum_pattern)) {
                                pattern_known = true;
                                equals_subject = ir_const_enum_values_equal(&const_enum_subject,
                                                                             &const_enum_pattern);
                            }
                            ir_const_enum_value_free(&const_enum_pattern);
                        }
                    }
                }

                if (!pattern_known && folded_subject) {
                    Expr* folded_pattern = ir_try_fold_constant_expr(comp, pattern_expr);
                    if (folded_pattern) {
                        pattern_known = true;
                        equals_subject = expr_equals(folded_subject, folded_pattern);
                        expr_free(folded_pattern);
                    }
                }

                // Non-payload enums are runtime ints; allow comparing enum subjects against int patterns.
                if (!pattern_known && has_const_enum_subject && payload_bind_count <= 0) {
                    int64_t subject_scalar = 0;
                    Expr* folded_pattern = ir_try_fold_constant_expr(comp, pattern_expr);
                    if (ir_const_enum_value_as_scalar_int(&const_enum_subject, &subject_scalar) &&
                        folded_pattern &&
                        folded_pattern->kind == EXPR_LITERAL &&
                        folded_pattern->type &&
                        folded_pattern->type->kind == TYPE_INT) {
                        pattern_known = true;
                        equals_subject = (subject_scalar == folded_pattern->literal.as_int);
                    }
                    if (folded_pattern) expr_free(folded_pattern);
                }

                if (!pattern_known) {
                    saw_unknown_pattern = true;
                    continue;
                }

                if (equals_subject) {
                    if (saw_unknown_pattern) {
                        indeterminate_match = true;
                    } else {
                        matched_arm = i;
                    }
                    break;
                }
            }

            if (matched_arm >= 0) {
                Stmt* matched_body = stmt->match_stmt.bodies ? stmt->match_stmt.bodies[matched_arm] : NULL;
                if (matched_body) {
                    ir_lower_statement_dce(comp, matched_body, out);
                }
                if (folded_subject) expr_free(folded_subject);
                ir_const_enum_value_free(&const_enum_subject);
                return;
            }

            if (!indeterminate_match && !saw_unknown_pattern) {
                if (stmt->match_stmt.else_branch) {
                    ir_lower_statement_dce(comp, stmt->match_stmt.else_branch, out);
                }
                if (folded_subject) expr_free(folded_subject);
                ir_const_enum_value_free(&const_enum_subject);
                return;
            }

            if (folded_subject) expr_free(folded_subject);
        }
        if (has_guards && folded_subject) {
            expr_free(folded_subject);
        }
        ir_const_enum_value_free(&const_enum_subject);
    }

    ir_statement_list_append(out, stmt);
}

static IRStatementList ir_lower_statements_dce(Compiler* comp,
                                               Stmt** statements,
                                               int stmt_count,
                                               bool allow_dead_store_elimination) {
    IRStatementList ir;
    ir_statement_list_init(&ir);

    if (!statements || stmt_count <= 0) return ir;

    for (int i = 0; i < stmt_count; i++) {
        int before_count = ir.stmt_count;
        ir_lower_statement_dce(comp, statements[i], &ir);
        if (ir.stmt_count > before_count &&
            ir_stmt_guarantees_termination(ir.statements[ir.stmt_count - 1])) {
            break;
        }
    }

    if (!comp || allow_dead_store_elimination) {
        ir_apply_local_cse(&ir);
        ir_apply_local_copy_propagation(&ir);
        ir_apply_local_dead_store_elimination(comp, &ir, allow_dead_store_elimination);
    } else {
        ir_apply_local_cse(&ir);
        ir_apply_local_copy_propagation(&ir);
    }
    return ir;
}

static void compile_statement_list_with_ir(Compiler* comp,
                                           Stmt** statements,
                                           int stmt_count,
                                           bool allow_dead_store_elimination) {
    IRStatementList ir =
        ir_lower_statements_dce(comp, statements, stmt_count, allow_dead_store_elimination);
    for (int i = 0; i < ir.stmt_count; i++) {
        compile_statement(comp, ir.statements[i]);
    }
    ir_statement_list_free(&ir);
}

static void compile_literal(Compiler* comp, Expr* expr) {
    int line = expr->line;

    if (expr->kind == EXPR_LITERAL) {
        if (expr->type && expr->type->kind == TYPE_DOUBLE) {
            emit_constant(comp, (Constant){.as_double = expr->literal.as_double, .type_index = 1}, line);
        } else if (expr->type && expr->type->kind == TYPE_BOOL) {
            emit_constant(comp, (Constant){.as_int = expr->literal.as_int, .type_index = 4}, line);
        } else if (expr->type && expr->type->kind == TYPE_BIGINT) {
            emit_constant(comp, (Constant){.as_string = expr->literal.as_string, .type_index = 3}, line);
        } else if (expr->type && expr->type->kind == TYPE_STRING) {
            emit_constant(comp, (Constant){.as_string = expr->literal.as_string, .type_index = 2}, line);
        } else {
            emit_constant(comp, (Constant){.as_int = expr->literal.as_int, .type_index = 0}, line);
        }
    } else if (expr->kind == EXPR_NIL) {
        emit_byte(comp, OP_CONST, line);
        emit_byte(comp, 0xff, line);
    } else if (expr->kind == EXPR_ARRAY_LITERAL) {
        if (expr->array_literal.element_count > 255) {
            compiler_set_error(comp, "Array literal has too many elements for one instruction (max 255)", expr->file, expr->line, expr->column);
            return;
        }
        emit_byte(comp, OP_ARRAY_NEW, line);
        emit_byte(comp, (uint8_t)expr->array_literal.element_count, line);

        for (int i = 0; i < expr->array_literal.element_count; i++) {
            compile_expression(comp, expr->array_literal.elements[i]);
            emit_byte(comp, OP_ARRAY_PUSH, line);
        }
    }
}

static void compile_identifier(Compiler* comp, Expr* expr) {
    int line = expr->line;
    int local = resolve_local(comp, expr->identifier);

    if (local >= 0) {
        emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)local, line);
    } else {
        Stmt* enum_stmt = NULL;
        int member_index = -1;
        if (find_enum_member_decl(comp, expr->identifier, &enum_stmt, &member_index) &&
            enum_stmt && enum_stmt->enum_decl.has_payload_members &&
            (!expr->type || expr->type->kind != TYPE_FUNCTION)) {
            int payload_count = (enum_stmt->enum_decl.member_payload_counts &&
                                 member_index < enum_stmt->enum_decl.member_count)
                                    ? enum_stmt->enum_decl.member_payload_counts[member_index]
                                    : 0;
            if (payload_count > 0) {
                char message[320];
                snprintf(message,
                         sizeof(message),
                         "Enum payload constructor '%s.%s' must be called with %d argument(s)",
                         enum_stmt->enum_decl.name ? enum_stmt->enum_decl.name : "<enum>",
                         (enum_stmt->enum_decl.member_names && enum_stmt->enum_decl.member_names[member_index])
                             ? enum_stmt->enum_decl.member_names[member_index]
                             : "<member>",
                         payload_count);
                compiler_set_error(comp, message, expr->file ? expr->file : comp->file, expr->line, expr->column);
                return;
            }

            (void)emit_enum_payload_value(comp,
                                          enum_stmt,
                                          member_index,
                                          NULL,
                                          0,
                                          expr->line,
                                          expr->file,
                                          expr->column);
            return;
        }

        // Use constant pool for name-based global lookup at runtime
        int name_idx = make_constant_string(comp, expr->identifier, line);
        if (name_idx < 0) return;
        emit_load_global_name(comp, name_idx, line);
    }
}

static bool match_evalA_denom_locals(Compiler* comp, Expr* denom_expr, uint8_t* out_a_slot, uint8_t* out_b_slot) {
    if (!comp || !denom_expr || !out_a_slot || !out_b_slot) return false;

    // Match:
    //   (( (t * (t + 1)) / 2) + a + 1)
    // where:
    //   t = a + b
    //
    // This is the hot spectral-norm kernel evalA denominator, with `a` being
    // the left operand of `t` (the term added back after the division).

    Expr* add1 = denom_expr;
    if (add1->kind != EXPR_BINARY || add1->binary.op != TOKEN_PLUS) return false;

    Expr* add1_left = add1->binary.left;
    Expr* add1_right = add1->binary.right;
    if (!add1_left || !add1_right) return false;
    if (add1_right->kind != EXPR_LITERAL || !add1_right->type || add1_right->type->kind != TYPE_INT) return false;
    if (add1_right->literal.as_int != 1) return false;

    Expr* add2 = add1_left;
    if (add2->kind != EXPR_BINARY || add2->binary.op != TOKEN_PLUS) return false;
    if (!add2->binary.left || !add2->binary.right) return false;

    Expr* a_expr = NULL;
    Expr* div_expr = NULL;
    if (add2->binary.right->kind == EXPR_IDENTIFIER) {
        div_expr = add2->binary.left;
        a_expr = add2->binary.right;
    } else if (add2->binary.left->kind == EXPR_IDENTIFIER) {
        div_expr = add2->binary.right;
        a_expr = add2->binary.left;
    } else {
        return false;
    }

    if (!a_expr || !a_expr->type || a_expr->type->kind != TYPE_INT) return false;
    if (!div_expr || div_expr->kind != EXPR_BINARY || div_expr->binary.op != TOKEN_SLASH) return false;
    if (!div_expr->binary.left || !div_expr->binary.right) return false;

    Expr* div_rhs = div_expr->binary.right;
    if (div_rhs->kind != EXPR_LITERAL || !div_rhs->type || div_rhs->type->kind != TYPE_INT) return false;
    if (div_rhs->literal.as_int != 2) return false;

    Expr* mul_expr = div_expr->binary.left;
    if (!mul_expr || mul_expr->kind != EXPR_BINARY || mul_expr->binary.op != TOKEN_STAR) return false;
    if (!mul_expr->binary.left || !mul_expr->binary.right) return false;

    Expr* t_expr = NULL;
    Expr* t_plus_one = NULL;

    // Identify t and (t + 1) in either multiplication order.
    Expr* left = mul_expr->binary.left;
    Expr* right = mul_expr->binary.right;

    if (right->kind == EXPR_BINARY && right->binary.op == TOKEN_PLUS &&
        ((right->binary.left && right->binary.left->kind == EXPR_LITERAL && right->binary.left->type && right->binary.left->type->kind == TYPE_INT && right->binary.left->literal.as_int == 1) ||
         (right->binary.right && right->binary.right->kind == EXPR_LITERAL && right->binary.right->type && right->binary.right->type->kind == TYPE_INT && right->binary.right->literal.as_int == 1))) {
        t_expr = left;
        t_plus_one = right;
    } else if (left->kind == EXPR_BINARY && left->binary.op == TOKEN_PLUS &&
               ((left->binary.left && left->binary.left->kind == EXPR_LITERAL && left->binary.left->type && left->binary.left->type->kind == TYPE_INT && left->binary.left->literal.as_int == 1) ||
                (left->binary.right && left->binary.right->kind == EXPR_LITERAL && left->binary.right->type && left->binary.right->type->kind == TYPE_INT && left->binary.right->literal.as_int == 1))) {
        t_expr = right;
        t_plus_one = left;
    } else {
        return false;
    }

    if (!t_expr || !t_plus_one) return false;
    if (t_expr->kind != EXPR_BINARY || t_expr->binary.op != TOKEN_PLUS) return false;
    if (!t_expr->binary.left || !t_expr->binary.right) return false;
    if (t_expr->binary.left->kind != EXPR_IDENTIFIER || t_expr->binary.right->kind != EXPR_IDENTIFIER) return false;

    Expr* a_in_t = t_expr->binary.left;
    Expr* b_in_t = t_expr->binary.right;
    if (!a_in_t || !b_in_t) return false;
    if (!a_in_t->type || a_in_t->type->kind != TYPE_INT) return false;
    if (!b_in_t->type || b_in_t->type->kind != TYPE_INT) return false;

    if (!expr_equals(a_expr, a_in_t)) return false;

    // Verify (t + 1) uses the same t expression.
    if (t_plus_one->kind != EXPR_BINARY || t_plus_one->binary.op != TOKEN_PLUS) return false;
    if (!t_plus_one->binary.left || !t_plus_one->binary.right) return false;

    Expr* one_side = NULL;
    Expr* other_side = NULL;
    if (t_plus_one->binary.left->kind == EXPR_LITERAL &&
        t_plus_one->binary.left->type && t_plus_one->binary.left->type->kind == TYPE_INT &&
        t_plus_one->binary.left->literal.as_int == 1) {
        one_side = t_plus_one->binary.left;
        other_side = t_plus_one->binary.right;
    } else if (t_plus_one->binary.right->kind == EXPR_LITERAL &&
               t_plus_one->binary.right->type && t_plus_one->binary.right->type->kind == TYPE_INT &&
               t_plus_one->binary.right->literal.as_int == 1) {
        one_side = t_plus_one->binary.right;
        other_side = t_plus_one->binary.left;
    }
    (void)one_side;
    if (!other_side || !expr_equals(other_side, t_expr)) return false;

    int a_local = resolve_local(comp, a_in_t->identifier);
    int b_local = resolve_local(comp, b_in_t->identifier);
    if (a_local < 0 || b_local < 0 || a_local > 0xff || b_local > 0xff) return false;

    *out_a_slot = (uint8_t)a_local;
    *out_b_slot = (uint8_t)b_local;
    return true;
}

static void compile_binary(Compiler* comp, Expr* expr) {
    int line = expr->line;
    TokenType op = expr->binary.op;

    Expr* folded = fold_expression_recursive(comp, expr);
    if (folded) {
        compile_expression(comp, folded);
        expr_free(folded);
        return;
    }

    // Logical operators must short-circuit:
    //   a && b : evaluate b only when a is true
    //   a || b : evaluate b only when a is false
    if (op == TOKEN_AND) {
        compile_expression(comp, expr->binary.left);
        int end_jump = emit_jump(comp, OP_JUMP_IF_FALSE, line);
        emit_byte(comp, OP_POP, line);
        compile_expression(comp, expr->binary.right);
        patch_jump(comp, end_jump);
        return;
    }

    if (op == TOKEN_OR) {
        compile_expression(comp, expr->binary.left);
        int rhs_jump = emit_jump(comp, OP_JUMP_IF_FALSE, line);
        int end_jump = emit_jump(comp, OP_JUMP, line);
        patch_jump(comp, rhs_jump);
        emit_byte(comp, OP_POP, line);
        compile_expression(comp, expr->binary.right);
        patch_jump(comp, end_jump);
        return;
    }

    // Hot-path: 1.0 / (x as double) where x is an int expression.
    // Emits a single opcode that performs the cast and reciprocal.
    if (op == TOKEN_SLASH &&
        expr->type && expr->type->kind == TYPE_DOUBLE &&
        expr->binary.left && expr->binary.right &&
        expr->binary.left->kind == EXPR_LITERAL &&
        expr->binary.left->type && expr->binary.left->type->kind == TYPE_DOUBLE &&
        expr->binary.left->literal.as_double == 1.0 &&
        expr->binary.right->kind == EXPR_CAST &&
        expr->binary.right->cast.target_type &&
        expr->binary.right->cast.target_type->kind == TYPE_DOUBLE &&
        expr->binary.right->cast.value &&
        expr->binary.right->cast.value->type &&
        expr->binary.right->cast.value->type->kind == TYPE_INT) {
        uint8_t a_slot = 0;
        uint8_t b_slot = 0;
        if (match_evalA_denom_locals(comp, expr->binary.right->cast.value, &a_slot, &b_slot)) {
            emit_byte(comp, OP_EVALA_RECIP_LOCALS_DOUBLE, line);
            emit_byte(comp, a_slot, line);
            emit_byte(comp, b_slot, line);
            return;
        }

        compile_expression(comp, expr->binary.right->cast.value);
        emit_byte(comp, OP_RECIP_INT_TO_DOUBLE, line);
        return;
    }

    // Hot-path: int multiplication patterns that allow sharing a pure subexpression.
    // x * (x + c)  =>  x; DUP; CONST c; ADD_INT; MUL_INT
    bool is_int_bin =
        expr->type && expr->type->kind == TYPE_INT &&
        expr->binary.left && expr->binary.right &&
        expr->binary.left->type && expr->binary.right->type &&
        expr->binary.left->type->kind == TYPE_INT &&
        expr->binary.right->type->kind == TYPE_INT;

    if (op == TOKEN_STAR && is_int_bin) {
        Expr* a = expr->binary.left;
        Expr* b = expr->binary.right;
        int64_t add_c = 0;

        Expr* base = NULL;
        Expr* other = NULL;
        if (a && b) {
            // Prefer matching b == a + c
            if (b->kind == EXPR_BINARY && b->binary.op == TOKEN_PLUS &&
                expr_inline_eligible(a)) {
                Expr* l = b->binary.left;
                Expr* r = b->binary.right;
                if (l && r && expr_equals(l, a) &&
                    r->kind == EXPR_LITERAL && r->type && r->type->kind == TYPE_INT) {
                    base = a;
                    other = b;
                    add_c = r->literal.as_int;
                } else if (l && r && expr_equals(r, a) &&
                           l->kind == EXPR_LITERAL && l->type && l->type->kind == TYPE_INT) {
                    base = a;
                    other = b;
                    add_c = l->literal.as_int;
                }
            }

            // Or match a == b + c
            if (!base && a->kind == EXPR_BINARY && a->binary.op == TOKEN_PLUS &&
                expr_inline_eligible(b)) {
                Expr* l = a->binary.left;
                Expr* r = a->binary.right;
                if (l && r && expr_equals(l, b) &&
                    r->kind == EXPR_LITERAL && r->type && r->type->kind == TYPE_INT) {
                    base = b;
                    other = a;
                    add_c = r->literal.as_int;
                } else if (l && r && expr_equals(r, b) &&
                           l->kind == EXPR_LITERAL && l->type && l->type->kind == TYPE_INT) {
                    base = b;
                    other = a;
                    add_c = l->literal.as_int;
                }
            }
        }

        (void)other;
        if (base && add_c == 1) {
            compile_expression(comp, base);
            emit_byte(comp, OP_DUP, line);
            emit_constant(comp, (Constant){ .as_int = add_c, .type_index = 0 }, line);
            emit_byte(comp, OP_ADD_INT, line);
            emit_byte(comp, OP_MUL_INT, line);
            return;
        }
    }

    // Fix 11: Push left first, then right (correct operand order)
    compile_expression(comp, expr->binary.left);
    compile_expression(comp, expr->binary.right);

    bool is_double_bin =
        expr->type && expr->type->kind == TYPE_DOUBLE &&
        expr->binary.left && expr->binary.right &&
        expr->binary.left->type && expr->binary.right->type &&
        expr->binary.left->type->kind == TYPE_DOUBLE &&
        expr->binary.right->type->kind == TYPE_DOUBLE;

    if (is_double_bin) {
        switch (op) {
            case TOKEN_PLUS: emit_byte(comp, OP_ADD_DOUBLE, line); return;
            case TOKEN_MINUS: emit_byte(comp, OP_SUB_DOUBLE, line); return;
            case TOKEN_STAR: emit_byte(comp, OP_MUL_DOUBLE, line); return;
            case TOKEN_SLASH: emit_byte(comp, OP_DIV_DOUBLE, line); return;
            default: break;
        }
    }

    is_int_bin =
        expr->type && expr->type->kind == TYPE_INT &&
        expr->binary.left && expr->binary.right &&
        expr->binary.left->type && expr->binary.right->type &&
        expr->binary.left->type->kind == TYPE_INT &&
        expr->binary.right->type->kind == TYPE_INT;

    if (is_int_bin) {
        switch (op) {
            case TOKEN_PLUS: emit_byte(comp, OP_ADD_INT, line); return;
            case TOKEN_MINUS: emit_byte(comp, OP_SUB_INT, line); return;
            case TOKEN_STAR: emit_byte(comp, OP_MUL_INT, line); return;
            case TOKEN_SLASH: emit_byte(comp, OP_DIV_INT, line); return;
            case TOKEN_PERCENT: emit_byte(comp, OP_MOD_INT, line); return;
            case TOKEN_BIT_AND: emit_byte(comp, OP_BIT_AND_INT, line); return;
            case TOKEN_BIT_OR: emit_byte(comp, OP_BIT_OR_INT, line); return;
            case TOKEN_BIT_XOR: emit_byte(comp, OP_BIT_XOR_INT, line); return;
            default: break;
        }
    }

    switch (op) {
        case TOKEN_PLUS: emit_byte(comp, OP_ADD, line); break;
        case TOKEN_MINUS: emit_byte(comp, OP_SUB, line); break;
        case TOKEN_STAR: emit_byte(comp, OP_MUL, line); break;
        case TOKEN_SLASH: emit_byte(comp, OP_DIV, line); break;
        case TOKEN_PERCENT: emit_byte(comp, OP_MOD, line); break;
        case TOKEN_AND: emit_byte(comp, OP_AND, line); break;
        case TOKEN_OR: emit_byte(comp, OP_OR, line); break;
        case TOKEN_BIT_AND: emit_byte(comp, OP_BIT_AND, line); break;
        case TOKEN_BIT_OR: emit_byte(comp, OP_BIT_OR, line); break;
        case TOKEN_BIT_XOR: emit_byte(comp, OP_BIT_XOR, line); break;
        case TOKEN_EQ_EQ: emit_byte(comp, OP_EQ, line); break;
        case TOKEN_BANG_EQ: emit_byte(comp, OP_NE, line); break;
        case TOKEN_LT: emit_byte(comp, OP_LT, line); break;
        case TOKEN_LT_EQ: emit_byte(comp, OP_LE, line); break;
        case TOKEN_GT: emit_byte(comp, OP_GT, line); break;
        case TOKEN_GT_EQ: emit_byte(comp, OP_GE, line); break;
        default: break;
    }
}

static void compile_unary(Compiler* comp, Expr* expr) {
    int line = expr->line;
    TokenType op = expr->unary.op;

    Expr* folded = fold_expression_recursive(comp, expr);
    if (folded) {
        compile_expression(comp, folded);
        expr_free(folded);
        return;
    }

    compile_expression(comp, expr->unary.operand);

    if (op == TOKEN_MINUS) {
        if (expr->type && expr->type->kind == TYPE_DOUBLE &&
            expr->unary.operand && expr->unary.operand->type &&
            expr->unary.operand->type->kind == TYPE_DOUBLE) {
            emit_byte(comp, OP_NEG_DOUBLE, line);
        } else if (expr->type && expr->type->kind == TYPE_INT &&
                   expr->unary.operand && expr->unary.operand->type &&
                   expr->unary.operand->type->kind == TYPE_INT) {
            emit_byte(comp, OP_NEG_INT, line);
        } else {
            emit_byte(comp, OP_NEG, line);
        }
    } else if (op == TOKEN_NOT) {
        emit_byte(comp, OP_NOT, line);
    } else if (op == TOKEN_BIT_NOT) {
        if (expr->type && expr->type->kind == TYPE_INT &&
            expr->unary.operand && expr->unary.operand->type &&
            expr->unary.operand->type->kind == TYPE_INT) {
            emit_byte(comp, OP_BIT_NOT_INT, line);
        } else {
            emit_byte(comp, OP_BIT_NOT, line);
        }
    }
}

static void compile_call(Compiler* comp, Expr* expr) {
    int line = expr->line;
    if (expr->call.arg_count > 255) {
        compiler_set_error(comp, "Too many call arguments (max 255)", expr->file, expr->line, expr->column);
        return;
    }

    // Fix 10: For print/println, only push args, no callee
    if (expr->call.callee->kind == EXPR_IDENTIFIER) {
        const char* callee_name = expr->call.callee->identifier;

        Stmt* enum_stmt = NULL;
        int member_index = -1;
        if (find_enum_member_decl(comp, callee_name, &enum_stmt, &member_index) &&
            enum_stmt && enum_stmt->enum_decl.has_payload_members) {
            (void)emit_enum_payload_value(comp,
                                          enum_stmt,
                                          member_index,
                                          expr->call.args,
                                          expr->call.arg_count,
                                          expr->line,
                                          expr->file,
                                          expr->column);
            return;
        }

        if (expr->call.arg_count > 0 &&
            expr->call.args &&
            expr->call.args[0] &&
            expr->call.args[0]->type &&
            expr->call.args[0]->type->kind == TYPE_INTERFACE &&
            expr->call.args[0]->type->interface_def &&
            expr->call.args[0]->type->interface_def->name &&
            resolve_local(comp, callee_name) < 0) {
            int interface_name_idx = make_constant_string(comp, expr->call.args[0]->type->interface_def->name, line);
            int method_name_idx = make_constant_string(comp, callee_name, line);
            if (interface_name_idx < 0 || method_name_idx < 0) {
                return;
            }

            for (int i = 0; i < expr->call.arg_count; i++) {
                compile_expression(comp, expr->call.args[i]);
            }
            emit_call_interface(comp,
                                interface_name_idx,
                                method_name_idx,
                                (uint8_t)expr->call.arg_count,
                                line);
            return;
        }

        if (strcmp(callee_name, "print") == 0) {
            for (int i = 0; i < expr->call.arg_count; i++) {
                compile_expression(comp, expr->call.args[i]);
            }
            emit_byte(comp, OP_PRINT, line);
            return;
        }
        if (strcmp(callee_name, "println") == 0) {
            for (int i = 0; i < expr->call.arg_count; i++) {
                compile_expression(comp, expr->call.args[i]);
            }
            emit_byte(comp, OP_PRINTLN, line);
            return;
        }

        // Common builtins (compiled to dedicated opcodes).
        if (expr->call.arg_count == 1 && strcmp(callee_name, "len") == 0) {
            compile_expression(comp, expr->call.args[0]);
            emit_byte(comp, OP_ARRAY_LEN, line);
            return;
        }

        if (expr->call.arg_count == 1 && strcmp(callee_name, "sqrt") == 0) {
            compile_expression(comp, expr->call.args[0]);
            emit_byte(comp, OP_SQRT, line);
            return;
        }

        // High-level array intrinsics (compiled to dedicated opcodes, no return value).
        if (expr->call.arg_count == 2 && strcmp(callee_name, "copyInto") == 0) {
            Expr* dst = expr->call.args[0];
            Expr* src = expr->call.args[1];
            if (dst && src && dst->kind == EXPR_IDENTIFIER && src->kind == EXPR_IDENTIFIER) {
                int dst_local = resolve_local(comp, dst->identifier);
                int src_local = resolve_local(comp, src->identifier);
                if (dst_local >= 0 && src_local >= 0) {
                    emit_byte(comp, OP_ARRAY_COPY_LOCAL_LOCAL, line);
                    emit_byte(comp, (uint8_t)dst_local, line);
                    emit_byte(comp, (uint8_t)src_local, line);
                    return;
                }
            }

            // Stack convention for OP_ARRAY_COPY is [..., dst, src] (src on top).
            compile_expression(comp, dst);
            compile_expression(comp, src);
            emit_byte(comp, OP_ARRAY_COPY, line);
            return;
        }

        if (expr->call.arg_count == 2 && strcmp(callee_name, "reversePrefix") == 0) {
            Expr* arr = expr->call.args[0];
            Expr* hi = expr->call.args[1];
            if (arr && hi && arr->kind == EXPR_IDENTIFIER && hi->kind == EXPR_IDENTIFIER) {
                int arr_local = resolve_local(comp, arr->identifier);
                int hi_local = resolve_local(comp, hi->identifier);
                if (arr_local >= 0 && hi_local >= 0) {
                    emit_byte(comp, OP_ARRAY_REVERSE_PREFIX_LOCAL_LOCAL, line);
                    emit_byte(comp, (uint8_t)arr_local, line);
                    emit_byte(comp, (uint8_t)hi_local, line);
                    return;
                }
            }

            // Stack convention for OP_ARRAY_REVERSE_PREFIX is [..., arr, hi] (hi on top).
            compile_expression(comp, arr);
            compile_expression(comp, hi);
            emit_byte(comp, OP_ARRAY_REVERSE_PREFIX, line);
            return;
        }

        if (expr->call.arg_count == 2 && strcmp(callee_name, "rotatePrefixLeft") == 0) {
            Expr* arr = expr->call.args[0];
            Expr* hi = expr->call.args[1];
            if (arr && hi && arr->kind == EXPR_IDENTIFIER && hi->kind == EXPR_IDENTIFIER) {
                int arr_local = resolve_local(comp, arr->identifier);
                int hi_local = resolve_local(comp, hi->identifier);
                if (arr_local >= 0 && hi_local >= 0) {
                    emit_byte(comp, OP_ARRAY_ROTATE_PREFIX_LEFT_LOCAL_LOCAL, line);
                    emit_byte(comp, (uint8_t)arr_local, line);
                    emit_byte(comp, (uint8_t)hi_local, line);
                    return;
                }
            }

            // Stack convention for OP_ARRAY_ROTATE_PREFIX_LEFT is [..., arr, hi] (hi on top).
            compile_expression(comp, arr);
            compile_expression(comp, hi);
            emit_byte(comp, OP_ARRAY_ROTATE_PREFIX_LEFT, line);
            return;
        }

        if (expr->call.arg_count == 2 && strcmp(callee_name, "rotatePrefixRight") == 0) {
            Expr* arr = expr->call.args[0];
            Expr* hi = expr->call.args[1];
            if (arr && hi && arr->kind == EXPR_IDENTIFIER && hi->kind == EXPR_IDENTIFIER) {
                int arr_local = resolve_local(comp, arr->identifier);
                int hi_local = resolve_local(comp, hi->identifier);
                if (arr_local >= 0 && hi_local >= 0) {
                    emit_byte(comp, OP_ARRAY_ROTATE_PREFIX_RIGHT_LOCAL_LOCAL, line);
                    emit_byte(comp, (uint8_t)arr_local, line);
                    emit_byte(comp, (uint8_t)hi_local, line);
                    return;
                }
            }

            // Stack convention for OP_ARRAY_ROTATE_PREFIX_RIGHT is [..., arr, hi] (hi on top).
            compile_expression(comp, arr);
            compile_expression(comp, hi);
            emit_byte(comp, OP_ARRAY_ROTATE_PREFIX_RIGHT, line);
            return;
        }

        // Fast-path: direct global call without pushing callee onto the stack.
        if (resolve_local(comp, callee_name) < 0) {
            Stmt* decl = find_function_decl(comp, callee_name);
            if (decl && compile_inline_call(comp, decl, expr)) {
                return;
            }

            int name_idx = make_constant_string(comp, callee_name, line);
            if (name_idx < 0) return;
            for (int i = 0; i < expr->call.arg_count; i++) {
                compile_expression(comp, expr->call.args[i]);
            }

            emit_call_global_name(comp, name_idx, (uint8_t)expr->call.arg_count, line);
            return;
        }
    }

    // Fix 10: For other calls, push callee first, then args in order
    compile_expression(comp, expr->call.callee);
    for (int i = 0; i < expr->call.arg_count; i++) {
        compile_expression(comp, expr->call.args[i]);
    }

    emit_byte2(comp, OP_CALL, (uint8_t)expr->call.arg_count, line);
}

static void compile_func_literal(Compiler* comp, Expr* expr) {
    int line = expr->line;
    int capture_count = expr->func_literal.capture_count;

    const char* compiled_name = expr->func_literal.compiled_name;
    if (!compiled_name || compiled_name[0] == '\0') {
        int anon_id = next_anonymous_function_id(comp);
        char generated_name[64];
        snprintf(generated_name, sizeof(generated_name), "__lambda$%d", anon_id);

        ObjFunction* func_obj = obj_function_create();
        chunk_init(&func_obj->chunk);
        constant_pool_init(&func_obj->constants);
        func_obj->param_count = expr->func_literal.param_count;
        func_obj->param_names = (char**)safe_malloc((size_t)expr->func_literal.param_count * sizeof(char*));
        for (int i = 0; i < expr->func_literal.param_count; i++) {
            func_obj->param_names[i] = safe_strdup(expr->func_literal.params[i]);
        }
        func_obj->capture_count = capture_count;
        if (capture_count > 0) {
            func_obj->capture_local_slots = (int*)safe_malloc((size_t)capture_count * sizeof(int));
            for (int i = 0; i < capture_count; i++) {
                func_obj->capture_local_slots[i] = -1;
            }
        } else {
            func_obj->capture_local_slots = NULL;
        }
        func_obj->local_count = 0;
        func_obj->local_names = NULL;
        func_obj->debug_local_names = NULL;
        func_obj->local_types = NULL;
        func_obj->is_async = expr->func_literal.is_async;
        func_obj->defer_handler_ip = -1;
        func_obj->defer_return_slot = -1;
        func_obj->name = safe_strdup(generated_name);
        if (expr->file && expr->file[0] != '\0') {
            func_obj->source_file = safe_strdup(expr->file);
        } else if (comp && comp->file && comp->file[0] != '\0') {
            func_obj->source_file = safe_strdup(comp->file);
        }
        func_obj->ref_count = 1;
        compiler_try_assign_jit_hint(func_obj,
                                     expr->func_literal.return_type,
                                     expr->func_literal.params,
                                     expr->func_literal.param_types,
                                     expr->func_literal.param_count,
                                     expr->func_literal.body);

        Compiler func_compiler;
        func_compiler.chunk = &func_obj->chunk;
        func_compiler.function = func_obj;
        func_compiler.globals = comp->globals;
        func_compiler.locals = NULL;
        func_compiler.local_count = 0;
        func_compiler.local_capacity = 0;
        func_compiler.depth = 0;
        func_compiler.is_top_level = false;
        func_compiler.current_function_is_async = expr->func_literal.is_async;
        func_compiler.had_error = false;
        func_compiler.error = NULL;
        func_compiler.file = expr->file ? expr->file : comp->file;
        func_compiler.vm = NULL;
        func_compiler.loop_stack = NULL;
        func_compiler.loop_stack_count = 0;
        func_compiler.loop_stack_capacity = 0;
        func_compiler.record_decls = comp->record_decls;
        func_compiler.record_decl_count = comp->record_decl_count;
        func_compiler.enum_decls = comp->enum_decls;
        func_compiler.enum_decl_count = comp->enum_decl_count;
        func_compiler.function_decls = comp->function_decls;
        func_compiler.function_decl_count = comp->function_decl_count;
        func_compiler.defer_enabled = stmt_contains_defer(expr->func_literal.body);
        func_compiler.defer_return_slot = -1;
        func_compiler.defer_return_jumps = NULL;
        func_compiler.defer_return_jump_count = 0;
        func_compiler.defer_return_jump_capacity = 0;
        func_compiler.shared_anon_func_counter = comp->shared_anon_func_counter;

        for (int i = 0; i < expr->func_literal.param_count; i++) {
            if (add_local(&func_compiler, expr->func_literal.params[i], expr->line) < 0) {
                break;
            }
        }
        for (int i = 0; i < capture_count; i++) {
            int slot = add_local(&func_compiler, expr->func_literal.capture_names[i], expr->line);
            if (slot < 0) {
                break;
            }
            func_obj->capture_local_slots[i] = slot;
        }

        if (func_compiler.defer_enabled) {
            func_compiler.defer_return_slot = add_local_anon(&func_compiler, expr->line);
            if (func_compiler.defer_return_slot < 0) {
                func_compiler.defer_enabled = false;
            }
        }

        if (expr->func_literal.body) {
            if (expr->func_literal.body->kind == STMT_BLOCK) {
                compile_block(&func_compiler, expr->func_literal.body);
            } else {
                compile_statement(&func_compiler, expr->func_literal.body);
            }
        }

        if (func_compiler.defer_enabled) {
            emit_byte(&func_compiler, OP_CONST, expr->line);
            emit_byte(&func_compiler, 0xff, expr->line);
            emit_byte2(&func_compiler, OP_STORE_LOCAL, (uint8_t)func_compiler.defer_return_slot, expr->line);

            for (int i = 0; i < func_compiler.defer_return_jump_count; i++) {
                patch_jump(&func_compiler, func_compiler.defer_return_jumps[i]);
            }

            int epilogue_start = func_compiler.chunk->code_count;
            func_obj->defer_handler_ip = epilogue_start;
            func_obj->defer_return_slot = func_compiler.defer_return_slot;
            emit_byte(&func_compiler, OP_DEFER_HAS, expr->line);
            int exit_jump = emit_jump(&func_compiler, OP_JUMP_IF_FALSE, expr->line);
            emit_byte(&func_compiler, OP_POP, expr->line);
            emit_byte(&func_compiler, OP_DEFER_CALL, expr->line);
            emit_byte(&func_compiler, OP_POP, expr->line);
            emit_loop(&func_compiler, epilogue_start, expr->line);
            patch_jump(&func_compiler, exit_jump);
            emit_byte(&func_compiler, OP_POP, expr->line);

            emit_byte2(&func_compiler, OP_LOAD_LOCAL, (uint8_t)func_compiler.defer_return_slot, expr->line);
            emit_byte(&func_compiler, OP_RET, expr->line);
        } else {
            emit_byte(&func_compiler, OP_CONST, expr->line);
            emit_byte(&func_compiler, 0xff, expr->line);
            emit_byte(&func_compiler, OP_RET, expr->line);
        }

        comp->had_error = comp->had_error || func_compiler.had_error;
        if (func_compiler.error && !comp->error) {
            comp->error = func_compiler.error;
        }
        compiler_refresh_jit_profile_metadata(func_obj);

        if (func_compiler.loop_stack) free(func_compiler.loop_stack);
        if (func_compiler.defer_return_jumps) free(func_compiler.defer_return_jumps);

        Type* return_type = expr->func_literal.return_type
            ? type_clone(expr->func_literal.return_type)
            : type_void();
        if (expr->func_literal.is_async) {
            return_type = type_future(return_type ? return_type : type_void());
        }
        Type** param_types = NULL;
        if (expr->func_literal.param_count > 0) {
            param_types = (Type**)safe_malloc((size_t)expr->func_literal.param_count * sizeof(Type*));
            for (int i = 0; i < expr->func_literal.param_count; i++) {
                param_types[i] = type_clone(expr->func_literal.param_types[i]);
            }
        }

        Type* func_type = type_function(return_type, param_types, expr->func_literal.param_count);
        Symbol* sym = symbol_create(func_type, generated_name, false);
        sym->function_obj = (void*)func_obj;
        symbol_table_add(comp->globals, sym);

        if (expr->func_literal.compiled_name) free(expr->func_literal.compiled_name);
        expr->func_literal.compiled_name = safe_strdup(generated_name);
        compiled_name = expr->func_literal.compiled_name;
    }

    int name_idx = make_constant_string(comp, compiled_name, line);
    if (name_idx < 0) return;
    emit_load_global_name(comp, name_idx, line);

    if (capture_count > 0) {
        for (int i = 0; i < capture_count; i++) {
            const char* capture_name = expr->func_literal.capture_names[i];
            int local_slot = resolve_local(comp, capture_name);
            if (local_slot >= 0) {
                emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)local_slot, line);
            } else {
                int capture_name_idx = make_constant_string(comp, capture_name, line);
                if (capture_name_idx < 0) return;
                emit_load_global_name(comp, capture_name_idx, line);
            }
        }

        if (capture_count > 255) {
            compiler_set_error(comp, "Too many captured variables in function literal (max 255)", expr->file, expr->line, expr->column);
            return;
        }
        emit_byte2(comp, OP_MAKE_CLOSURE, (uint8_t)capture_count, line);
    }
}

static void compile_index(Compiler* comp, Expr* expr) {
    int line = expr->line;

    Expr* array_expr = expr->index.array;
    Expr* index_expr = expr->index.index;

    bool is_array_double = false;
    bool is_array_int = false;
    if (array_expr && array_expr->type && array_expr->type->kind == TYPE_ARRAY && array_expr->type->element_type) {
        is_array_double = array_expr->type->element_type->kind == TYPE_DOUBLE;
        is_array_int = array_expr->type->element_type->kind == TYPE_INT;
    }

    // Fast path: local array indexing. Avoid pushing the array value (and the
    // retain/release churn that comes with OP_LOAD_LOCAL + OP_ARRAY_GET).
    if (array_expr && array_expr->kind == EXPR_IDENTIFIER) {
        int array_local = resolve_local(comp, array_expr->identifier);
        if (array_local >= 0) {
            if (index_expr && index_expr->kind == EXPR_LITERAL &&
                index_expr->type && index_expr->type->kind == TYPE_INT) {
                int64_t idx = index_expr->literal.as_int;
                if (idx >= 0 && idx <= 0xff) {
                    uint8_t op = OP_ARRAY_GET_LOCAL_CONST;
                    if (is_array_double) op = OP_ARRAY_GET_LOCAL_CONST_DOUBLE;
                    else if (is_array_int) op = OP_ARRAY_GET_LOCAL_CONST_INT;
                    emit_byte(comp, op, line);
                    emit_byte(comp, (uint8_t)array_local, line);
                    emit_byte(comp, (uint8_t)idx, line);
                    return;
                }
            }

            if (index_expr && index_expr->kind == EXPR_IDENTIFIER) {
                int idx_local = resolve_local(comp, index_expr->identifier);
                if (idx_local >= 0) {
                    uint8_t op = OP_ARRAY_GET_LOCAL_LOCAL;
                    if (is_array_double) op = OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE;
                    else if (is_array_int) op = OP_ARRAY_GET_LOCAL_LOCAL_INT;
                    emit_byte(comp, op, line);
                    emit_byte(comp, (uint8_t)array_local, line);
                    emit_byte(comp, (uint8_t)idx_local, line);
                    return;
                }
            }

            // Stack convention for OP_ARRAY_GET_LOCAL is [..., index] (index on top).
            compile_expression(comp, index_expr);
            emit_byte2(comp, OP_ARRAY_GET_LOCAL, (uint8_t)array_local, line);
            return;
        }
    }

    // Fallback: generic indexing. Stack convention is [..., array, index].
    compile_expression(comp, array_expr);
    compile_expression(comp, index_expr);
    emit_byte(comp, OP_ARRAY_GET, line);
}

static void compile_cast(Compiler* comp, Expr* expr) {
    int line = expr->line;

    if (expr->cast.value &&
        expr->cast.value->type &&
        expr->cast.target_type &&
        is_noop_primitive_cast(expr->cast.value->type, expr->cast.target_type)) {
        compile_expression(comp, expr->cast.value);
        return;
    }

    compile_expression(comp, expr->cast.value);

    if (expr->cast.target_type) {
        switch (expr->cast.target_type->kind) {
            case TYPE_INT:
                emit_byte(comp, OP_CAST_INT, line);
                break;
            case TYPE_BOOL:
                emit_byte(comp, OP_CAST_BOOL, line);
                break;
            case TYPE_DOUBLE:
                emit_byte(comp, OP_CAST_DOUBLE, line);
                break;
            case TYPE_STRING:
                emit_byte(comp, OP_CAST_STRING, line);
                break;
            case TYPE_BIGINT:
                emit_byte(comp, OP_CAST_BIGINT, line);
                break;
            default:
                break;
        }
    }
}

static const char* compiler_type_test_target_name(Type* type) {
    if (!type) return NULL;

    switch (type->kind) {
        case TYPE_INT: return "int";
        case TYPE_BOOL: return "bool";
        case TYPE_DOUBLE: return "double";
        case TYPE_BIGINT: return "bigint";
        case TYPE_STRING: return "string";
        case TYPE_BYTES: return "bytes";
        case TYPE_NIL: return "nil";
        case TYPE_RECORD:
            if (type->record_def && type->record_def->name && type->record_def->name[0] != '\0') {
                return type->record_def->name;
            }
            return "record";
        default:
            return NULL;
    }
}

static void compile_type_test(Compiler* comp, Expr* expr) {
    int line = expr->line;
    Type* target_type = expr->type_test.target_type;
    if (target_type && target_type->kind == TYPE_INTERFACE &&
        target_type->interface_def && target_type->interface_def->name) {
        int interface_name_idx = make_constant_string(comp, target_type->interface_def->name, line);
        if (interface_name_idx < 0) return;

        int method_count = target_type->interface_def->method_count;
        if (method_count == 0) {
            compile_expression(comp, expr->type_test.value);
            emit_type_test_interface_method(comp, interface_name_idx, 0xffff, line);
            return;
        }

        for (int i = 0; i < method_count; i++) {
            InterfaceMethod* method = interface_def_get_method(target_type->interface_def, i);
            if (!method || !method->name || method->name[0] == '\0') {
                compiler_set_error(comp,
                                   "Interface type case is missing runtime method metadata",
                                   expr->file,
                                   expr->line,
                                   expr->column);
                return;
            }

            int method_name_idx = make_constant_string(comp, method->name, line);
            if (method_name_idx < 0) return;

            compile_expression(comp, expr->type_test.value);
            emit_type_test_interface_method(comp, interface_name_idx, method_name_idx, line);
            if (i > 0) {
                emit_byte(comp, OP_AND, line);
            }
        }
        return;
    }

    const char* type_name = compiler_type_test_target_name(target_type);
    if (!type_name) {
        compiler_set_error(comp,
                           "Unsupported target type in switch type case",
                           expr->file,
                           expr->line,
                           expr->column);
        return;
    }

    compile_expression(comp, expr->type_test.value);
    emit_byte(comp, OP_TYPEOF, line);
    emit_constant(comp, (Constant){ .as_string = (char*)type_name, .type_index = 2 }, line);
    emit_byte(comp, OP_EQ, line);
}

static void compile_try(Compiler* comp, Expr* expr) {
    int line = expr->line;

    compile_expression(comp, expr->try_expr.expr);

    emit_byte(comp, OP_DUP, line);
    emit_byte(comp, OP_TUPLE_GET, line);
    emit_byte(comp, 1, line);

    emit_byte(comp, OP_CONST, line);
    emit_byte(comp, 0xff, line);
    emit_byte(comp, OP_NE, line);

    int ok_jump = emit_jump(comp, OP_JUMP_IF_FALSE, line);

    emit_byte(comp, OP_POP, line);
    if (comp->defer_enabled && !comp->is_top_level && comp->defer_return_slot >= 0) {
        emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)comp->defer_return_slot, line);
        int j = emit_jump(comp, OP_JUMP, line);
        loop_add_jump(&comp->defer_return_jumps, &comp->defer_return_jump_count, &comp->defer_return_jump_capacity, j);
    } else {
        emit_byte(comp, OP_RET, line);
    }

    patch_jump(comp, ok_jump);

    emit_byte(comp, OP_POP, line);
    emit_byte(comp, OP_TUPLE_GET, line);
    emit_byte(comp, 0, line);
}

static void compile_expression(Compiler* comp, Expr* expr) {
    if (!expr) return;

    switch (expr->kind) {
        case EXPR_LITERAL:
        case EXPR_NIL:
        case EXPR_ARRAY_LITERAL:
            compile_literal(comp, expr);
            break;
        case EXPR_IDENTIFIER:
            compile_identifier(comp, expr);
            break;
        case EXPR_BINARY:
            compile_binary(comp, expr);
            break;
        case EXPR_UNARY:
            compile_unary(comp, expr);
            break;
        case EXPR_CALL:
            compile_call(comp, expr);
            break;
        case EXPR_FUNC_LITERAL:
            compile_func_literal(comp, expr);
            break;
        case EXPR_INDEX:
            compile_index(comp, expr);
            break;
        case EXPR_CAST:
            compile_cast(comp, expr);
            break;
        case EXPR_TRY:
            compile_try(comp, expr);
            break;
        case EXPR_AWAIT:
            compile_expression(comp, expr->await_expr.expr);
            emit_byte(comp, OP_AWAIT, expr->line);
            break;
        case EXPR_TYPE_TEST:
            compile_type_test(comp, expr);
            break;
        case EXPR_IF:
            compile_if_expression(comp, expr);
            break;
        case EXPR_MATCH:
            compile_match_expression(comp, expr);
            break;
        case EXPR_BLOCK:
            compile_block_expression(comp, expr);
            break;
        case EXPR_RECORD_LITERAL:
            compile_record_literal(comp, expr);
            break;
        case EXPR_FIELD_ACCESS:
            compile_field_access(comp, expr);
            break;
        case EXPR_TUPLE_LITERAL:
            compile_tuple_literal(comp, expr);
            break;
        case EXPR_TUPLE_ACCESS:
            compile_tuple_access(comp, expr);
            break;
        case EXPR_MAP_LITERAL:
            compile_map_literal(comp, expr);
            break;
        case EXPR_SET_LITERAL:
            compile_set_literal(comp, expr);
            break;
        default:
            break;
    }
}

static void compile_record_literal(Compiler* comp, Expr* expr) {
    int line = expr->line;

    Type* record_type = expr->record_literal.record_type;
    if (!record_type && expr->type && expr->type->kind == TYPE_RECORD) {
        record_type = expr->type;
    }

    int field_count = expr->record_literal.field_count;
    RecordDef* def = NULL;
    Stmt* decl = NULL;
    if (record_type && record_type->kind == TYPE_RECORD) {
        def = record_type->record_def;
        if (def && def->field_count > 0) {
            field_count = def->field_count;
        } else if (def && def->name) {
            decl = find_record_decl(comp, def->name);
            if (decl) {
                field_count = decl->record_decl.field_count;
            }
        }
    }

    if (field_count > 255) {
        compiler_set_error(comp, "Record has too many fields for bytecode encoding (max 255)", expr->file, expr->line, expr->column);
        return;
    }

    // Create a new record.
    const char* record_name = (def && def->name) ? def->name : NULL;
    if (record_name && record_name[0] != '\0') {
        int name_idx = make_constant_string(comp, record_name, line);
        if (name_idx < 0) return;
        emit_byte(comp, OP_RECORD_NEW_NAMED, line);
        emit_u16_operand(comp, (uint16_t)name_idx, line);
        emit_byte(comp, (uint8_t)field_count, line);
    } else {
        emit_byte(comp, OP_RECORD_NEW, line);
        emit_byte(comp, (uint8_t)field_count, line);
    }

    // Set each field
    for (int i = 0; i < expr->record_literal.field_count; i++) {
        int field_idx = i;
        if (def && def->field_count > 0) {
            int idx = record_def_get_field_index(def, expr->record_literal.field_names[i]);
            if (idx >= 0) field_idx = idx;
        } else if (decl) {
            for (int j = 0; j < decl->record_decl.field_count; j++) {
                if (strcmp(decl->record_decl.field_names[j], expr->record_literal.field_names[i]) == 0) {
                    field_idx = j;
                    break;
                }
            }
        }

        // The record is already on the stack
        compile_expression(comp, expr->record_literal.field_values[i]);
        emit_byte(comp, OP_RECORD_SET_FIELD, line);
        emit_byte(comp, (uint8_t)field_idx, line);
    }
}

static void compile_field_access(Compiler* comp, Expr* expr) {
    int line = expr->line;

    int field_idx = expr->field_access.field_index;
    if (field_idx < 0) {
        field_idx = resolve_record_field_index(comp, expr->field_access.object, expr->field_access.field_name);
    }
    if (field_idx < 0) field_idx = 0;
    if (field_idx > 255) {
        compiler_set_error(comp, "Record field index exceeds bytecode operand range (max 255)", expr->file, expr->line, expr->column);
        return;
    }

    // Fast path: local array index followed by record field access (b[i].x).
    // Emit a single opcode that reads the field directly from the record stored in the array.
    Expr* obj = expr->field_access.object;
    if (obj && obj->kind == EXPR_INDEX) {
        Expr* arr_expr = obj->index.array;
        Expr* idx_expr = obj->index.index;
        if (arr_expr && arr_expr->kind == EXPR_IDENTIFIER) {
            int array_local = resolve_local(comp, arr_expr->identifier);
            if (array_local >= 0) {
                if (idx_expr && idx_expr->kind == EXPR_LITERAL &&
                    idx_expr->type && idx_expr->type->kind == TYPE_INT) {
                    int64_t idx = idx_expr->literal.as_int;
                    if (idx >= 0 && idx <= 0xff) {
                        emit_byte(comp, OP_ARRAY_GET_FIELD_LOCAL_CONST, line);
                        emit_byte(comp, (uint8_t)array_local, line);
                        emit_byte(comp, (uint8_t)idx, line);
                        emit_byte(comp, (uint8_t)field_idx, line);
                        return;
                    }
                }

                if (idx_expr && idx_expr->kind == EXPR_IDENTIFIER) {
                    int idx_local = resolve_local(comp, idx_expr->identifier);
                    if (idx_local >= 0) {
                        emit_byte(comp, OP_ARRAY_GET_FIELD_LOCAL_LOCAL, line);
                        emit_byte(comp, (uint8_t)array_local, line);
                        emit_byte(comp, (uint8_t)idx_local, line);
                        emit_byte(comp, (uint8_t)field_idx, line);
                        return;
                    }
                }
            }
        }
    }

    // General case: materialize the record value and then extract the field.
    compile_expression(comp, expr->field_access.object);

    emit_byte(comp, OP_RECORD_GET_FIELD, line);
    emit_byte(comp, (uint8_t)field_idx, line);
}

static void compile_tuple_literal(Compiler* comp, Expr* expr) {
    int line = expr->line;
    if (expr->tuple_literal.element_count > 255) {
        compiler_set_error(comp, "Tuple literal has too many elements for bytecode encoding (max 255)", expr->file, expr->line, expr->column);
        return;
    }

    // Create a new tuple
    emit_byte(comp, OP_TUPLE_NEW, line);
    emit_byte(comp, (uint8_t)expr->tuple_literal.element_count, line);

    // Set each element
    for (int i = 0; i < expr->tuple_literal.element_count; i++) {
        // The tuple is already on the stack
        compile_expression(comp, expr->tuple_literal.elements[i]);
        emit_byte(comp, OP_TUPLE_SET, line);
        emit_byte(comp, (uint8_t)i, line);
    }
}

static void compile_tuple_access(Compiler* comp, Expr* expr) {
    int line = expr->line;
    if (expr->tuple_access.index < 0 || expr->tuple_access.index > 255) {
        compiler_set_error(comp, "Tuple index out of bytecode operand range (0..255)", expr->file, expr->line, expr->column);
        return;
    }

    // Compile the tuple expression
    compile_expression(comp, expr->tuple_access.tuple);

    // Get the element at the specified index
    emit_byte(comp, OP_TUPLE_GET, line);
    emit_byte(comp, (uint8_t)expr->tuple_access.index, line);
}

static void compile_map_literal(Compiler* comp, Expr* expr) {
    int line = expr->line;

    // Create a new map
    emit_byte(comp, OP_MAP_NEW, line);

    // Set each entry
    for (int i = 0; i < expr->map_literal.entry_count; i++) {
        // The map is already on the stack
        compile_expression(comp, expr->map_literal.keys[i]);
        compile_expression(comp, expr->map_literal.values[i]);
        emit_byte(comp, OP_MAP_SET, line);
    }
}

static void compile_set_literal(Compiler* comp, Expr* expr) {
    int line = expr->line;

    // Create a new set
    emit_byte(comp, OP_SET_NEW, line);

    // Add each element
    for (int i = 0; i < expr->set_literal.element_count; i++) {
        // The set is already on the stack
        compile_expression(comp, expr->set_literal.elements[i]);
        emit_byte(comp, OP_SET_ADD, line);
    }
}

static void compile_var_decl(Compiler* comp, Stmt* stmt) {
    int line = stmt->line;

    if (stmt->var_decl.initializer) {
        compile_expression(comp, stmt->var_decl.initializer);
    } else {
        emit_byte(comp, OP_CONST, line);
        emit_byte(comp, 0xff, line);
    }

    if (comp->is_top_level && comp->depth == 0) {
        int name_idx = make_constant_string(comp, stmt->var_decl.name, line);
        if (name_idx < 0) return;
        emit_store_global_name(comp, name_idx, line);
        return;
    }

    int local = add_local(comp, stmt->var_decl.name, line);
    if (local < 0) return;
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)local, line);
}

static void compile_var_tuple_decl(Compiler* comp, Stmt* stmt) {
    int line = stmt->line;

    if (stmt->var_tuple_decl.initializer) {
        compile_expression(comp, stmt->var_tuple_decl.initializer);
    } else {
        emit_byte(comp, OP_CONST, line);
        emit_byte(comp, 0xff, line);
    }

    int name_count = stmt->var_tuple_decl.name_count;
    if (name_count > 255) {
        compiler_set_error(comp, "Tuple destructuring has too many names (max 255)", stmt->file, stmt->line, stmt->column);
        emit_byte(comp, OP_POP, line);
        return;
    }
    for (int i = 0; i < name_count; i++) {
        const char* name = stmt->var_tuple_decl.names ? stmt->var_tuple_decl.names[i] : NULL;
        if (!name) continue;
        if (strcmp(name, "_") == 0) continue;

        emit_byte(comp, OP_DUP, line);
        emit_byte(comp, OP_TUPLE_GET, line);
        emit_byte(comp, (uint8_t)i, line);

        if (comp->is_top_level && comp->depth == 0) {
            int name_idx = make_constant_string(comp, name, line);
            if (name_idx < 0) {
                emit_byte(comp, OP_POP, line);
                return;
            }
            emit_store_global_name(comp, name_idx, line);
            continue;
        }

        int local = add_local(comp, name, line);
        if (local < 0) {
            emit_byte(comp, OP_POP, line);
            return;
        }
        emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)local, line);
    }

    emit_byte(comp, OP_POP, line);
}

static bool expr_statement_leaves_stack_value(Expr* expr) {
    if (!expr || expr->kind != EXPR_CALL || !expr->call.callee) {
        return true;
    }

    Expr* callee = expr->call.callee;
    if (callee->kind != EXPR_IDENTIFIER || !callee->identifier) {
        return true;
    }

    const char* name = callee->identifier;
    if (expr->call.arg_count == 1 &&
        (strcmp(name, "print") == 0 || strcmp(name, "println") == 0)) {
        return false;
    }

    if (expr->call.arg_count == 2 &&
        (strcmp(name, "copyInto") == 0 ||
         strcmp(name, "reversePrefix") == 0 ||
         strcmp(name, "rotatePrefixLeft") == 0 ||
         strcmp(name, "rotatePrefixRight") == 0)) {
        return false;
    }

    return true;
}

static void compile_expr_stmt(Compiler* comp, Stmt* stmt) {
    compile_expression(comp, stmt->expr_stmt);
    if (expr_statement_leaves_stack_value(stmt->expr_stmt)) {
        emit_byte(comp, OP_POP, stmt->line);
    }
}

static void compile_assign(Compiler* comp, Stmt* stmt) {
    int line = stmt->line;

    int local = resolve_local(comp, stmt->assign.name);
    if (local >= 0) {
        Expr* value = stmt->assign.value;

        // Fast path: x = x +/- <literal>  (int/double only)
        if (value && value->kind == EXPR_BINARY &&
            (value->binary.op == TOKEN_PLUS || value->binary.op == TOKEN_MINUS) &&
            value->binary.left && value->binary.left->kind == EXPR_IDENTIFIER &&
            strcmp(value->binary.left->identifier, stmt->assign.name) == 0 &&
            value->binary.right && value->binary.right->kind == EXPR_LITERAL &&
            value->type && value->binary.right->type &&
            value->type->kind == value->binary.right->type->kind &&
            (value->type->kind == TYPE_INT || value->type->kind == TYPE_DOUBLE)) {

            int const_idx = -1;
            if (value->type->kind == TYPE_INT) {
                const_idx = make_constant_int(comp, value->binary.right->literal.as_int, line);
            } else {
                const_idx = make_constant_double(comp, value->binary.right->literal.as_double, line);
            }
            if (const_idx < 0) return;

            emit_byte(comp, value->binary.op == TOKEN_PLUS ? OP_ADD_LOCAL_CONST : OP_SUB_LOCAL_CONST, line);
            emit_byte(comp, (uint8_t)local, line);
            emit_byte(comp, (uint8_t)const_idx, line);
            return;
        }

        // Fast path: x = -x  (int/double only)
        if (value && value->kind == EXPR_UNARY &&
            value->unary.op == TOKEN_MINUS &&
            value->unary.operand && value->unary.operand->kind == EXPR_IDENTIFIER &&
            strcmp(value->unary.operand->identifier, stmt->assign.name) == 0 &&
            value->type && (value->type->kind == TYPE_INT || value->type->kind == TYPE_DOUBLE)) {
            emit_byte2(comp, OP_NEGATE_LOCAL, (uint8_t)local, line);
            return;
        }

        // Fast path: x = x + (a / b)  (double locals only)
        if (value && value->kind == EXPR_BINARY &&
            value->binary.op == TOKEN_PLUS &&
            value->binary.left && value->binary.left->kind == EXPR_IDENTIFIER &&
            strcmp(value->binary.left->identifier, stmt->assign.name) == 0 &&
            value->binary.right && value->binary.right->kind == EXPR_BINARY &&
            value->binary.right->binary.op == TOKEN_SLASH &&
            value->binary.right->binary.left && value->binary.right->binary.left->kind == EXPR_IDENTIFIER &&
            value->binary.right->binary.right && value->binary.right->binary.right->kind == EXPR_IDENTIFIER &&
            value->type && value->binary.left->type && value->binary.right->type &&
            value->type->kind == TYPE_DOUBLE &&
            value->binary.left->type->kind == TYPE_DOUBLE &&
            value->binary.right->type->kind == TYPE_DOUBLE &&
            value->binary.right->binary.left->type && value->binary.right->binary.right->type &&
            value->binary.right->binary.left->type->kind == TYPE_DOUBLE &&
            value->binary.right->binary.right->type->kind == TYPE_DOUBLE) {

            int num_local = resolve_local(comp, value->binary.right->binary.left->identifier);
            int den_local = resolve_local(comp, value->binary.right->binary.right->identifier);
            if (num_local >= 0 && den_local >= 0) {
                emit_byte(comp, OP_ADD_LOCAL_DIV_LOCALS, line);
                emit_byte(comp, (uint8_t)local, line);
                emit_byte(comp, (uint8_t)num_local, line);
                emit_byte(comp, (uint8_t)den_local, line);
                return;
            }
        }

        // Fast path: x = x + (scalar * arr[idx])  (int/double locals only)
        // Avoids emitting ARRAY_GET + MUL_* + ADD_LOCAL_STACK_*.
        if (value && value->kind == EXPR_BINARY &&
            value->binary.op == TOKEN_PLUS &&
            value->binary.left && value->binary.left->kind == EXPR_IDENTIFIER &&
            strcmp(value->binary.left->identifier, stmt->assign.name) == 0 &&
            value->binary.right && value->binary.right->kind == EXPR_BINARY &&
            value->binary.right->binary.op == TOKEN_STAR &&
            value->type && (value->type->kind == TYPE_INT || value->type->kind == TYPE_DOUBLE) &&
            value->binary.right->type && value->binary.right->type->kind == value->type->kind &&
            value->binary.right->binary.left &&
            value->binary.right->binary.left->type && value->binary.right->binary.left->type->kind == value->type->kind &&
            value->binary.right->binary.right &&
            value->binary.right->binary.right->kind == EXPR_INDEX &&
            value->binary.right->binary.right->type && value->binary.right->binary.right->type->kind == value->type->kind) {

            Expr* scalar = value->binary.right->binary.left;
            Expr* index = value->binary.right->binary.right;
            Expr* arr_expr = index->index.array;
            Expr* idx_expr = index->index.index;
            if (arr_expr && idx_expr &&
                arr_expr->kind == EXPR_IDENTIFIER &&
                idx_expr->kind == EXPR_IDENTIFIER &&
                arr_expr->type && arr_expr->type->kind == TYPE_ARRAY &&
                arr_expr->type->element_type && arr_expr->type->element_type->kind == value->type->kind &&
                idx_expr->type && idx_expr->type->kind == TYPE_INT) {

                int array_local = resolve_local(comp, arr_expr->identifier);
                int idx_local = resolve_local(comp, idx_expr->identifier);
                if (array_local >= 0 && idx_local >= 0) {
                    compile_expression(comp, scalar);
                    uint8_t op = value->type->kind == TYPE_INT ? OP_MADD_LOCAL_ARRAY_LOCAL_INT : OP_MADD_LOCAL_ARRAY_LOCAL_DOUBLE;
                    emit_byte(comp, op, line);
                    emit_byte(comp, (uint8_t)local, line);
                    emit_byte(comp, (uint8_t)array_local, line);
                    emit_byte(comp, (uint8_t)idx_local, line);
                    return;
                }
            }
        }

        // Fast path: x = x +/- <expr> (int/double locals only). Avoids emitting
        // LOAD_LOCAL x; <expr>; OP_ADD_*; STORE_LOCAL x.
        if (value && value->kind == EXPR_BINARY &&
            (value->binary.op == TOKEN_PLUS || value->binary.op == TOKEN_MINUS) &&
            value->binary.left && value->binary.left->kind == EXPR_IDENTIFIER &&
            strcmp(value->binary.left->identifier, stmt->assign.name) == 0 &&
            value->binary.right &&
            value->type && value->binary.left->type && value->binary.right->type &&
            value->type->kind == value->binary.left->type->kind &&
            value->type->kind == value->binary.right->type->kind &&
            (value->type->kind == TYPE_INT || value->type->kind == TYPE_DOUBLE)) {

            compile_expression(comp, value->binary.right);

            uint8_t op = OP_ADD_LOCAL_STACK_INT;
            if (value->type->kind == TYPE_INT) {
                op = (value->binary.op == TOKEN_PLUS) ? OP_ADD_LOCAL_STACK_INT : OP_SUB_LOCAL_STACK_INT;
            } else {
                op = (value->binary.op == TOKEN_PLUS) ? OP_ADD_LOCAL_STACK_DOUBLE : OP_SUB_LOCAL_STACK_DOUBLE;
            }

            emit_byte2(comp, op, (uint8_t)local, line);
            return;
        }

        compile_expression(comp, value);
        emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)local, line);
        return;
    }

    compile_expression(comp, stmt->assign.value);
    int name_idx = make_constant_string(comp, stmt->assign.name, line);
    if (name_idx < 0) return;
    emit_store_global_name(comp, name_idx, line);
}

static void compile_compound_assign_index(Compiler* comp, Stmt* stmt) {
    int line = stmt->line;

    Expr* target = stmt->assign_index.target;
    Expr* index_expr = stmt->assign_index.index;
    Expr* rhs_expr = stmt->assign_index.value;

    TokenType binary_op = compound_assign_to_binary_op(stmt->assign_index.op);
    if (binary_op == TOKEN_ERROR) {
        comp->had_error = true;
        if (!comp->error) {
            comp->error = error_create(ERROR_COMPILE, "Invalid compound assignment operator", stmt->file, stmt->line, stmt->column);
        }
        return;
    }

    bool is_array_double = false;
    bool is_array_int = false;
    Type* elem_type = NULL;
    if (target && target->type && target->type->kind == TYPE_ARRAY && target->type->element_type) {
        elem_type = target->type->element_type;
        is_array_double = elem_type->kind == TYPE_DOUBLE;
        is_array_int = elem_type->kind == TYPE_INT;
    }

    Type* rhs_type = rhs_expr ? rhs_expr->type : NULL;

    bool array_is_temp = false;
    int array_slot = -1;
    if (target && target->kind == EXPR_IDENTIFIER) {
        array_slot = resolve_local(comp, target->identifier);
    }
    if (array_slot < 0) {
        compile_expression(comp, target);
        array_slot = add_local_anon(comp, line);
        if (array_slot < 0) return;
        emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)array_slot, line);
        array_is_temp = true;
    }

    bool idx_is_const = false;
    uint8_t idx_const = 0;
    int idx_slot = -1;

    if (index_expr && index_expr->kind == EXPR_LITERAL &&
        index_expr->type && index_expr->type->kind == TYPE_INT) {
        int64_t idx64 = index_expr->literal.as_int;
        if (idx64 >= 0 && idx64 <= 0xff) {
            idx_is_const = true;
            idx_const = (uint8_t)idx64;
        }
    }

    if (!idx_is_const) {
        if (index_expr && index_expr->kind == EXPR_IDENTIFIER) {
            idx_slot = resolve_local(comp, index_expr->identifier);
        }
        if (idx_slot < 0) {
            compile_expression(comp, index_expr);
            idx_slot = add_local_anon(comp, line);
            if (idx_slot < 0) return;
            emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)idx_slot, line);
        }
    }

    // Load the current element value (bounds checked) before evaluating the RHS.
    if (idx_is_const) {
        uint8_t op = OP_ARRAY_GET_LOCAL_CONST;
        if (is_array_double) op = OP_ARRAY_GET_LOCAL_CONST_DOUBLE;
        else if (is_array_int) op = OP_ARRAY_GET_LOCAL_CONST_INT;
        emit_byte(comp, op, line);
        emit_byte(comp, (uint8_t)array_slot, line);
        emit_byte(comp, idx_const, line);
    } else {
        uint8_t op = OP_ARRAY_GET_LOCAL_LOCAL;
        if (is_array_double) op = OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE;
        else if (is_array_int) op = OP_ARRAY_GET_LOCAL_LOCAL_INT;
        emit_byte(comp, op, line);
        emit_byte(comp, (uint8_t)array_slot, line);
        emit_byte(comp, (uint8_t)idx_slot, line);
    }

    compile_expression(comp, rhs_expr);
    emit_arithmetic_op(comp, binary_op, elem_type, rhs_type, line);

    if (idx_is_const) {
        uint8_t op = OP_ARRAY_SET_LOCAL_CONST;
        if (is_array_double) op = OP_ARRAY_SET_LOCAL_CONST_DOUBLE;
        else if (is_array_int) op = OP_ARRAY_SET_LOCAL_CONST_INT;
        emit_byte(comp, op, line);
        emit_byte(comp, (uint8_t)array_slot, line);
        emit_byte(comp, idx_const, line);
    } else {
        uint8_t op = OP_ARRAY_SET_LOCAL_LOCAL;
        if (is_array_double) op = OP_ARRAY_SET_LOCAL_LOCAL_DOUBLE;
        else if (is_array_int) op = OP_ARRAY_SET_LOCAL_LOCAL_INT;
        emit_byte(comp, op, line);
        emit_byte(comp, (uint8_t)array_slot, line);
        emit_byte(comp, (uint8_t)idx_slot, line);
    }

    // Avoid keeping temporary arrays alive beyond this statement.
    if (array_is_temp) {
        emit_byte2(comp, OP_CONST, 0xff, line);
        emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)array_slot, line);
    }
}

static void compile_assign_index(Compiler* comp, Stmt* stmt) {
    int line = stmt->line;

    if (stmt->assign_index.op != TOKEN_ASSIGN) {
        compile_compound_assign_index(comp, stmt);
        return;
    }

    Expr* target = stmt->assign_index.target;
    Expr* index_expr = stmt->assign_index.index;
    Expr* value_expr = stmt->assign_index.value;

    bool is_array_double = false;
    bool is_array_int = false;
    if (target && target->type && target->type->kind == TYPE_ARRAY && target->type->element_type) {
        is_array_double = target->type->element_type->kind == TYPE_DOUBLE;
        is_array_int = target->type->element_type->kind == TYPE_INT;
    }

    // Fast path: local array element assignment. Since element assignment is a
    // statement in TabloLang, we can transfer ownership of the value from the
    // stack to the array (no result on stack).
    if (target && target->kind == EXPR_IDENTIFIER) {
        int array_local = resolve_local(comp, target->identifier);
        if (array_local >= 0) {
            if (index_expr && index_expr->kind == EXPR_LITERAL &&
                index_expr->type && index_expr->type->kind == TYPE_INT) {
                int64_t idx = index_expr->literal.as_int;
                if (idx >= 0 && idx <= 0xff) {
                    compile_expression(comp, value_expr);
                    uint8_t op = OP_ARRAY_SET_LOCAL_CONST;
                    if (is_array_double) op = OP_ARRAY_SET_LOCAL_CONST_DOUBLE;
                    else if (is_array_int) op = OP_ARRAY_SET_LOCAL_CONST_INT;
                    emit_byte(comp, op, line);
                    emit_byte(comp, (uint8_t)array_local, line);
                    emit_byte(comp, (uint8_t)idx, line);
                    return;
                }
            }

            if (index_expr && index_expr->kind == EXPR_IDENTIFIER) {
                int idx_local = resolve_local(comp, index_expr->identifier);
                if (idx_local >= 0) {
                    compile_expression(comp, value_expr);
                    uint8_t op = OP_ARRAY_SET_LOCAL_LOCAL;
                    if (is_array_double) op = OP_ARRAY_SET_LOCAL_LOCAL_DOUBLE;
                    else if (is_array_int) op = OP_ARRAY_SET_LOCAL_LOCAL_INT;
                    emit_byte(comp, op, line);
                    emit_byte(comp, (uint8_t)array_local, line);
                    emit_byte(comp, (uint8_t)idx_local, line);
                    return;
                }
            }

            // General local-indexed form: evaluate index and value, then store.
            compile_expression(comp, index_expr);
            compile_expression(comp, value_expr);
            emit_byte2(comp, OP_ARRAY_SET_LOCAL, (uint8_t)array_local, line);
            return;
        }
    }

    // Fallback: generic assignment. Stack convention is [..., array, index, value].
    compile_expression(comp, target);
    compile_expression(comp, index_expr);
    compile_expression(comp, value_expr);
    emit_byte(comp, OP_ARRAY_SET, line);
    // OP_ARRAY_SET leaves the assigned value on the stack; assignment is a statement, so pop it.
    emit_byte(comp, OP_POP, line);
}

static void compile_assign_field(Compiler* comp, Stmt* stmt) {
    int line = stmt->line;

    int field_idx = stmt->assign_field.field_index;
    if (field_idx < 0) {
        field_idx = resolve_record_field_index(comp, stmt->assign_field.object, stmt->assign_field.field_name);
    }
    if (field_idx < 0) {
        comp->had_error = true;
        if (!comp->error) {
            comp->error = error_create(ERROR_COMPILE, "Unknown record field", stmt->file, stmt->line, stmt->column);
        }
        return;
    }
    if (field_idx > 255) {
        compiler_set_error(comp, "Record field index exceeds bytecode operand range (max 255)", stmt->file, stmt->line, stmt->column);
        return;
    }

    if (stmt->assign_field.op == TOKEN_ASSIGN) {
        // Fast path: local array index followed by record field assignment (b[i].x = value;).
        // Preserve the out-of-bounds error ordering by emitting a bounds check before value eval.
        Expr* obj = stmt->assign_field.object;
        if (obj && obj->kind == EXPR_INDEX) {
            Expr* arr_expr = obj->index.array;
            Expr* idx_expr = obj->index.index;
            if (arr_expr && arr_expr->kind == EXPR_IDENTIFIER) {
                int array_local = resolve_local(comp, arr_expr->identifier);
                if (array_local >= 0) {
                    if (idx_expr && idx_expr->kind == EXPR_LITERAL &&
                        idx_expr->type && idx_expr->type->kind == TYPE_INT) {
                        int64_t idx = idx_expr->literal.as_int;
                        if (idx >= 0 && idx <= 0xff) {
                            compile_expression(comp, stmt->assign_field.value);
                            emit_byte(comp, OP_ARRAY_SET_FIELD_LOCAL_CONST, line);
                            emit_byte(comp, (uint8_t)array_local, line);
                            emit_byte(comp, (uint8_t)idx, line);
                            emit_byte(comp, (uint8_t)field_idx, line);
                            return;
                        }
                    }

                    if (idx_expr && idx_expr->kind == EXPR_IDENTIFIER) {
                        int idx_local = resolve_local(comp, idx_expr->identifier);
                        if (idx_local >= 0) {
                            compile_expression(comp, stmt->assign_field.value);
                            emit_byte(comp, OP_ARRAY_SET_FIELD_LOCAL_LOCAL, line);
                            emit_byte(comp, (uint8_t)array_local, line);
                            emit_byte(comp, (uint8_t)idx_local, line);
                            emit_byte(comp, (uint8_t)field_idx, line);
                            return;
                        }
                    }
                }
            }
        }

        compile_expression(comp, stmt->assign_field.object);
        compile_expression(comp, stmt->assign_field.value);
        emit_byte(comp, OP_RECORD_SET_FIELD, line);
        emit_byte(comp, (uint8_t)field_idx, line);
        emit_byte(comp, OP_POP, line);
        return;
    }

    TokenType binary_op = compound_assign_to_binary_op(stmt->assign_field.op);
    if (binary_op == TOKEN_ERROR) {
        comp->had_error = true;
        if (!comp->error) {
            comp->error = error_create(ERROR_COMPILE, "Invalid compound assignment operator", stmt->file, stmt->line, stmt->column);
        }
        return;
    }

    Type* field_type = NULL;
    if (stmt->assign_field.object && stmt->assign_field.object->type &&
        stmt->assign_field.object->type->kind == TYPE_RECORD &&
        stmt->assign_field.object->type->record_def) {
        RecordField* field = record_def_get_field(stmt->assign_field.object->type->record_def, field_idx);
        if (field) field_type = field->type;
    }
    Type* rhs_type = stmt->assign_field.value ? stmt->assign_field.value->type : NULL;

    // Fast path: local array index followed by record field compound assignment (b[i].x += value;).
    // Use the array+field opcode pair to avoid materializing the record.
    Expr* obj = stmt->assign_field.object;
    if (obj && obj->kind == EXPR_INDEX) {
        Expr* arr_expr = obj->index.array;
        Expr* idx_expr = obj->index.index;
        if (arr_expr && arr_expr->kind == EXPR_IDENTIFIER) {
            int array_local = resolve_local(comp, arr_expr->identifier);
            if (array_local >= 0) {
                bool idx_is_const = false;
                uint8_t idx_const = 0;
                int idx_local = -1;

                if (idx_expr && idx_expr->kind == EXPR_LITERAL &&
                    idx_expr->type && idx_expr->type->kind == TYPE_INT) {
                    int64_t idx64 = idx_expr->literal.as_int;
                    if (idx64 >= 0 && idx64 <= 0xff) {
                        idx_is_const = true;
                        idx_const = (uint8_t)idx64;
                    }
                }

                if (!idx_is_const) {
                    if (idx_expr && idx_expr->kind == EXPR_IDENTIFIER) {
                        idx_local = resolve_local(comp, idx_expr->identifier);
                    }
                    if (idx_local < 0) {
                        compile_expression(comp, idx_expr);
                        idx_local = add_local_anon(comp, line);
                        if (idx_local < 0) return;
                        emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)idx_local, line);
                    }
                }

                if (idx_is_const) {
                    emit_byte(comp, OP_ARRAY_GET_FIELD_LOCAL_CONST, line);
                    emit_byte(comp, (uint8_t)array_local, line);
                    emit_byte(comp, idx_const, line);
                    emit_byte(comp, (uint8_t)field_idx, line);
                } else {
                    emit_byte(comp, OP_ARRAY_GET_FIELD_LOCAL_LOCAL, line);
                    emit_byte(comp, (uint8_t)array_local, line);
                    emit_byte(comp, (uint8_t)idx_local, line);
                    emit_byte(comp, (uint8_t)field_idx, line);
                }

                compile_expression(comp, stmt->assign_field.value);
                emit_arithmetic_op(comp, binary_op, field_type, rhs_type, line);

                if (idx_is_const) {
                    emit_byte(comp, OP_ARRAY_SET_FIELD_LOCAL_CONST, line);
                    emit_byte(comp, (uint8_t)array_local, line);
                    emit_byte(comp, idx_const, line);
                    emit_byte(comp, (uint8_t)field_idx, line);
                } else {
                    emit_byte(comp, OP_ARRAY_SET_FIELD_LOCAL_LOCAL, line);
                    emit_byte(comp, (uint8_t)array_local, line);
                    emit_byte(comp, (uint8_t)idx_local, line);
                    emit_byte(comp, (uint8_t)field_idx, line);
                }
                return;
            }
        }
    }

    // General case: evaluate the object once and update the field in-place.
    compile_expression(comp, stmt->assign_field.object);
    emit_byte(comp, OP_DUP, line);
    emit_byte(comp, OP_RECORD_GET_FIELD, line);
    emit_byte(comp, (uint8_t)field_idx, line);
    compile_expression(comp, stmt->assign_field.value);
    emit_arithmetic_op(comp, binary_op, field_type, rhs_type, line);
    emit_byte(comp, OP_RECORD_SET_FIELD, line);
    emit_byte(comp, (uint8_t)field_idx, line);
    emit_byte(comp, OP_POP, line);
}

static void compile_block(Compiler* comp, Stmt* stmt) {
    comp->depth++;
    int scope_start = comp->function->local_count;
    compile_statement_list_with_ir(comp,
                                   stmt->block.statements,
                                   stmt->block.stmt_count,
                                   true);
    end_scope(comp, scope_start);
    comp->depth--;
}

static void compile_if(Compiler* comp, Stmt* stmt) {
    int line = stmt->line;

    int else_jump = -1;
    bool uses_fast_condition = false;

    Expr* cond = stmt->if_stmt.condition;

    // Fast path: if (a == b) / (a != b) where a and b are locals of the same
    // primitive numeric type. Emit a conditional jump directly (no bool on the stack).
    if (cond && cond->kind == EXPR_BINARY &&
        (cond->binary.op == TOKEN_EQ_EQ || cond->binary.op == TOKEN_BANG_EQ) &&
        cond->binary.left && cond->binary.right &&
        cond->binary.left->kind == EXPR_IDENTIFIER &&
        cond->binary.right->kind == EXPR_IDENTIFIER &&
        cond->binary.left->type && cond->binary.right->type &&
        cond->binary.left->type->kind == cond->binary.right->type->kind &&
        (cond->binary.left->type->kind == TYPE_INT || cond->binary.left->type->kind == TYPE_DOUBLE)) {

        const char* left_name = cond->binary.left->identifier;
        const char* right_name = cond->binary.right->identifier;

        int left_local = resolve_local(comp, left_name);
        int right_local = resolve_local(comp, right_name);

        uint8_t local_jump_op = OP_JUMP_IF_LOCAL_NE;
        uint8_t global_jump_op = OP_JUMP_IF_LOCAL_NE_GLOBAL;
        switch (cond->binary.op) {
            case TOKEN_EQ_EQ:
                local_jump_op = OP_JUMP_IF_LOCAL_NE;          // !(a == b) => a != b
                global_jump_op = OP_JUMP_IF_LOCAL_NE_GLOBAL;  // !(a == g) => a != g
                break;
            case TOKEN_BANG_EQ:
                local_jump_op = OP_JUMP_IF_LOCAL_EQ;          // !(a != b) => a == b
                global_jump_op = OP_JUMP_IF_LOCAL_EQ_GLOBAL;  // !(a != g) => a == g
                break;
            default:
                break;
        }

        if (left_local >= 0 && right_local >= 0) {
            else_jump = emit_jump_local_compare(comp, local_jump_op, (uint8_t)left_local, (uint8_t)right_local, line);
            uses_fast_condition = true;
        } else if (left_local >= 0 && right_local < 0) {
            int name_idx = make_constant_string(comp, right_name, line);
            if (name_idx >= 0 && name_idx <= 0xff) {
                else_jump = emit_jump_local_compare_const(comp, global_jump_op, (uint8_t)left_local, (uint8_t)name_idx, line);
                uses_fast_condition = true;
            }
        } else if (left_local < 0 && right_local >= 0) {
            int name_idx = make_constant_string(comp, left_name, line);
            if (name_idx >= 0 && name_idx <= 0xff) {
                else_jump = emit_jump_local_compare_const(comp, global_jump_op, (uint8_t)right_local, (uint8_t)name_idx, line);
                uses_fast_condition = true;
            }
        }
    }

    // Fast path: if (a == <const>) / ... where a is a local numeric and const is a literal.
    if (!uses_fast_condition &&
        cond && cond->kind == EXPR_BINARY &&
        (cond->binary.op == TOKEN_EQ_EQ || cond->binary.op == TOKEN_BANG_EQ) &&
        cond->binary.left && cond->binary.right) {

        Expr* left = cond->binary.left;
        Expr* right = cond->binary.right;
        TokenType op = cond->binary.op;

        // Normalize to: <identifier> <op> <literal>
        if (left->kind == EXPR_LITERAL && right->kind == EXPR_IDENTIFIER) {
            Expr* tmp = left;
            left = right;
            right = tmp;
        }

        if (left->kind == EXPR_IDENTIFIER &&
            right->kind == EXPR_LITERAL &&
            left->type && right->type &&
            left->type->kind == right->type->kind &&
            (left->type->kind == TYPE_INT || left->type->kind == TYPE_DOUBLE)) {

            int a_local = resolve_local(comp, left->identifier);
            if (a_local >= 0) {
                int const_idx_i = -1;
                if (right->type->kind == TYPE_INT) {
                    const_idx_i = make_constant_int(comp, right->literal.as_int, line);
                } else {
                    const_idx_i = make_constant_double(comp, right->literal.as_double, line);
                }

                if (const_idx_i >= 0 && const_idx_i <= 0xff) {
                    uint8_t jump_op = OP_JUMP_IF_LOCAL_NE_CONST;
                    switch (op) {
                        case TOKEN_EQ_EQ: jump_op = OP_JUMP_IF_LOCAL_NE_CONST; break;    // !(a == c) => a != c
                        case TOKEN_BANG_EQ: jump_op = OP_JUMP_IF_LOCAL_EQ_CONST; break;  // !(a != c) => a == c
                        default: break;
                    }

                    else_jump = emit_jump_local_compare_const(comp, jump_op, (uint8_t)a_local, (uint8_t)const_idx_i, line);
                    uses_fast_condition = true;
                }
            }
        }
    }

    // Fast path: if (a < b) / (a <= b) / (a > b) / (a >= b) where a and b are
    // locals of the same primitive numeric type. Emit a conditional jump
    // directly (no bool on the stack), like compile_while().
    if (!uses_fast_condition &&
        cond && cond->kind == EXPR_BINARY &&
        (cond->binary.op == TOKEN_LT || cond->binary.op == TOKEN_LT_EQ ||
         cond->binary.op == TOKEN_GT || cond->binary.op == TOKEN_GT_EQ) &&
        cond->binary.left && cond->binary.right &&
        cond->binary.left->kind == EXPR_IDENTIFIER &&
        cond->binary.right->kind == EXPR_IDENTIFIER &&
        cond->binary.left->type && cond->binary.right->type &&
        cond->binary.left->type->kind == cond->binary.right->type->kind &&
        (cond->binary.left->type->kind == TYPE_INT || cond->binary.left->type->kind == TYPE_DOUBLE)) {

        int a_local = resolve_local(comp, cond->binary.left->identifier);
        int b_local = resolve_local(comp, cond->binary.right->identifier);
        if (a_local >= 0 && b_local >= 0) {
            uint8_t jump_op = OP_JUMP_IF_LOCAL_GE;
            switch (cond->binary.op) {
                case TOKEN_LT: jump_op = OP_JUMP_IF_LOCAL_GE; break;   // !(a < b)  => a >= b
                case TOKEN_LT_EQ: jump_op = OP_JUMP_IF_LOCAL_GT; break; // !(a <= b) => a > b
                case TOKEN_GT: jump_op = OP_JUMP_IF_LOCAL_LE; break;   // !(a > b)  => a <= b
                case TOKEN_GT_EQ: jump_op = OP_JUMP_IF_LOCAL_LT; break; // !(a >= b) => a < b
                default: break;
            }
            else_jump = emit_jump_local_compare(comp, jump_op, (uint8_t)a_local, (uint8_t)b_local, line);
            uses_fast_condition = true;
        }
    }

    // Fast path: if (a < <const>) / ... where a is a local numeric and const is a literal.
    if (!uses_fast_condition &&
        cond && cond->kind == EXPR_BINARY &&
        (cond->binary.op == TOKEN_LT || cond->binary.op == TOKEN_LT_EQ ||
         cond->binary.op == TOKEN_GT || cond->binary.op == TOKEN_GT_EQ) &&
        cond->binary.left && cond->binary.right) {

        Expr* left = cond->binary.left;
        Expr* right = cond->binary.right;
        TokenType op = cond->binary.op;

        // Normalize to: <identifier> <op> <literal>
        if (left->kind == EXPR_LITERAL && right->kind == EXPR_IDENTIFIER) {
            Expr* tmp = left;
            left = right;
            right = tmp;
            switch (op) {
                case TOKEN_LT: op = TOKEN_GT; break;
                case TOKEN_LT_EQ: op = TOKEN_GT_EQ; break;
                case TOKEN_GT: op = TOKEN_LT; break;
                case TOKEN_GT_EQ: op = TOKEN_LT_EQ; break;
                default: break;
            }
        }

        if (left->kind == EXPR_IDENTIFIER &&
            right->kind == EXPR_LITERAL &&
            left->type && right->type &&
            left->type->kind == right->type->kind &&
            (left->type->kind == TYPE_INT || left->type->kind == TYPE_DOUBLE)) {

            int a_local = resolve_local(comp, left->identifier);
            if (a_local >= 0) {
                int const_idx_i = -1;
                if (right->type->kind == TYPE_INT) {
                    const_idx_i = make_constant_int(comp, right->literal.as_int, line);
                } else {
                    const_idx_i = make_constant_double(comp, right->literal.as_double, line);
                }

                if (const_idx_i >= 0 && const_idx_i <= 0xff) {
                    uint8_t jump_op = OP_JUMP_IF_LOCAL_GE_CONST;
                    switch (op) {
                        case TOKEN_LT: jump_op = OP_JUMP_IF_LOCAL_GE_CONST; break;    // !(a < c)  => a >= c
                        case TOKEN_LT_EQ: jump_op = OP_JUMP_IF_LOCAL_GT_CONST; break; // !(a <= c) => a > c
                        case TOKEN_GT: jump_op = OP_JUMP_IF_LOCAL_LE_CONST; break;    // !(a > c)  => a <= c
                        case TOKEN_GT_EQ: jump_op = OP_JUMP_IF_LOCAL_LT_CONST; break; // !(a >= c) => a < c
                        default: break;
                    }

                    else_jump = emit_jump_local_compare_const(comp, jump_op, (uint8_t)a_local, (uint8_t)const_idx_i, line);
                    uses_fast_condition = true;
                }
            }
        }
    }

    // Fast path: if (<expr> <op> <local/const>) where <expr> yields a primitive numeric.
    // Emit a jump that compares the computed stack value directly (no bool on the stack).
    if (!uses_fast_condition &&
        cond && cond->kind == EXPR_BINARY &&
        (cond->binary.op == TOKEN_LT || cond->binary.op == TOKEN_LT_EQ ||
         cond->binary.op == TOKEN_GT || cond->binary.op == TOKEN_GT_EQ) &&
        cond->binary.left && cond->binary.right &&
        cond->binary.left->type && cond->binary.right->type &&
        cond->binary.left->type->kind == cond->binary.right->type->kind &&
        (cond->binary.left->type->kind == TYPE_INT || cond->binary.left->type->kind == TYPE_DOUBLE)) {

        Expr* left = cond->binary.left;
        Expr* right = cond->binary.right;
        TokenType op = cond->binary.op;

        // Normalize to: <expr> <op> (<identifier>|<literal>)
        if ((right->kind != EXPR_IDENTIFIER && right->kind != EXPR_LITERAL) &&
            (left->kind == EXPR_IDENTIFIER || left->kind == EXPR_LITERAL)) {
            Expr* tmp = left;
            left = right;
            right = tmp;
            switch (op) {
                case TOKEN_LT: op = TOKEN_GT; break;
                case TOKEN_LT_EQ: op = TOKEN_GT_EQ; break;
                case TOKEN_GT: op = TOKEN_LT; break;
                case TOKEN_GT_EQ: op = TOKEN_LT_EQ; break;
                default: break;
            }
        }

        if (right->kind == EXPR_IDENTIFIER) {
            int b_local = resolve_local(comp, right->identifier);
            if (b_local >= 0) {
                compile_expression(comp, left);

                uint8_t jump_op = OP_JUMP_IF_STACK_GE_LOCAL;
                switch (op) {
                    case TOKEN_LT: jump_op = OP_JUMP_IF_STACK_GE_LOCAL; break;     // !(a < b)  => a >= b
                    case TOKEN_LT_EQ: jump_op = OP_JUMP_IF_STACK_GT_LOCAL; break;  // !(a <= b) => a > b
                    case TOKEN_GT: jump_op = OP_JUMP_IF_STACK_LE_LOCAL; break;     // !(a > b)  => a <= b
                    case TOKEN_GT_EQ: jump_op = OP_JUMP_IF_STACK_LT_LOCAL; break;  // !(a >= b) => a < b
                    default: break;
                }

                else_jump = emit_jump_stack_compare_local(comp, jump_op, (uint8_t)b_local, line);
                uses_fast_condition = true;
            }
        } else if (right->kind == EXPR_LITERAL) {
            int const_idx_i = -1;
            if (right->type->kind == TYPE_INT) {
                const_idx_i = make_constant_int(comp, right->literal.as_int, line);
            } else {
                const_idx_i = make_constant_double(comp, right->literal.as_double, line);
            }

            if (const_idx_i >= 0 && const_idx_i <= 0xff) {
                compile_expression(comp, left);

                uint8_t jump_op = OP_JUMP_IF_STACK_GE_CONST;
                switch (op) {
                    case TOKEN_LT: jump_op = OP_JUMP_IF_STACK_GE_CONST; break;     // !(a < c)  => a >= c
                    case TOKEN_LT_EQ: jump_op = OP_JUMP_IF_STACK_GT_CONST; break;  // !(a <= c) => a > c
                    case TOKEN_GT: jump_op = OP_JUMP_IF_STACK_LE_CONST; break;     // !(a > c)  => a <= c
                    case TOKEN_GT_EQ: jump_op = OP_JUMP_IF_STACK_LT_CONST; break;  // !(a >= c) => a < c
                    default: break;
                }

                else_jump = emit_jump_stack_compare_const(comp, jump_op, (uint8_t)const_idx_i, line);
                uses_fast_condition = true;
            }
        }
    }

    // Fast path: if (arr[idx]) where arr is a local array<bool> and idx is a local or small constant int.
    if (!uses_fast_condition &&
        cond && cond->kind == EXPR_INDEX &&
        cond->type && cond->type->kind == TYPE_BOOL) {

        Expr* array_expr = cond->index.array;
        Expr* index_expr = cond->index.index;
        if (array_expr && index_expr &&
            array_expr->kind == EXPR_IDENTIFIER &&
            array_expr->type && array_expr->type->kind == TYPE_ARRAY &&
            array_expr->type->element_type && array_expr->type->element_type->kind == TYPE_BOOL) {

            int array_local = resolve_local(comp, array_expr->identifier);
            if (array_local >= 0) {
                if (index_expr->kind == EXPR_LITERAL &&
                    index_expr->type && index_expr->type->kind == TYPE_INT) {
                    int64_t idx64 = index_expr->literal.as_int;
                    if (idx64 >= 0 && idx64 <= 0xff) {
                        else_jump = emit_jump_array_false_local_const(comp, (uint8_t)array_local, (uint8_t)idx64, line);
                        uses_fast_condition = true;
                    }
                } else if (index_expr->kind == EXPR_IDENTIFIER) {
                    int idx_local = resolve_local(comp, index_expr->identifier);
                    if (idx_local >= 0) {
                        else_jump = emit_jump_array_false_local_local(comp, (uint8_t)array_local, (uint8_t)idx_local, line);
                        uses_fast_condition = true;
                    }
                }
            }
        }
    }

    if (!uses_fast_condition) {
        compile_expression(comp, stmt->if_stmt.condition);
        else_jump = emit_jump(comp, OP_JUMP_IF_FALSE, line);
        emit_byte(comp, OP_POP, line);
    }

    compile_statement(comp, stmt->if_stmt.then_branch);
    bool then_terminates = ir_stmt_guarantees_termination(stmt->if_stmt.then_branch);

    // If there is no else branch and the condition didn't materialize a bool on the
    // stack, we can jump directly to the end without emitting an unconditional jump.
    if (uses_fast_condition && !stmt->if_stmt.else_branch) {
        patch_jump(comp, else_jump);
        return;
    }

    // If the then-branch always terminates, control cannot fall through into the
    // else entry path, so we can skip emitting an unconditional OP_JUMP.
    if (then_terminates) {
        patch_jump(comp, else_jump);
        if (!uses_fast_condition) {
            emit_byte(comp, OP_POP, line);
        }
        if (stmt->if_stmt.else_branch) {
            compile_statement(comp, stmt->if_stmt.else_branch);
        }
        return;
    }

    int end_jump = emit_jump(comp, OP_JUMP, line);

    patch_jump(comp, else_jump);
    if (!uses_fast_condition) {
        emit_byte(comp, OP_POP, line);
    }

    if (stmt->if_stmt.else_branch) {
        compile_statement(comp, stmt->if_stmt.else_branch);
    }

    patch_jump(comp, end_jump);
}

static void compile_match(Compiler* comp, Stmt* stmt) {
    if (!stmt) return;

    int line = stmt->line;
    int scope_start = comp->function->local_count;

    compile_expression(comp, stmt->match_stmt.subject);
    int subject_slot = add_local_anon(comp, line);
    if (subject_slot < 0) {
        emit_byte(comp, OP_POP, line);
        return;
    }
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)subject_slot, line);

    int* end_jumps = NULL;
    int end_count = 0;
    int end_capacity = 0;

    for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
        Expr* pattern_expr = stmt->match_stmt.patterns ? stmt->match_stmt.patterns[i] : NULL;
        Expr* guard_expr = stmt->match_stmt.guards ? stmt->match_stmt.guards[i] : NULL;
        MatchArmJumpList fail_jumps = { 0 };
        int arm_scope_start = comp->function->local_count;

        compile_match_pattern_dispatch(comp,
                                       pattern_expr,
                                       subject_slot,
                                       false,
                                       &fail_jumps,
                                       line);

        if (guard_expr) {
            int guard_line = guard_expr->line > 0 ? guard_expr->line : line;
            compile_expression(comp, guard_expr);
            int guard_fail_jump = emit_jump(comp, OP_JUMP_IF_FALSE, guard_line);
            match_arm_jump_list_add(&fail_jumps, guard_fail_jump);
            emit_byte(comp, OP_POP, guard_line);
        }

        compile_statement(comp, stmt->match_stmt.bodies[i]);
        bool arm_terminates = ir_stmt_guarantees_termination(stmt->match_stmt.bodies[i]);
        end_scope(comp, arm_scope_start);

        if (!arm_terminates) {
            int end_jump = emit_jump(comp, OP_JUMP, line);
            loop_add_jump(&end_jumps, &end_count, &end_capacity, end_jump);
        }

        match_arm_jump_list_patch_and_pop(comp, &fail_jumps, line);
        match_arm_jump_list_free(&fail_jumps);
    }

    if (stmt->match_stmt.else_branch) {
        compile_statement(comp, stmt->match_stmt.else_branch);
    }

    for (int i = 0; i < end_count; i++) {
        patch_jump(comp, end_jumps[i]);
    }
    if (end_jumps) free(end_jumps);

    end_scope(comp, scope_start);
}

static void compile_match_expression(Compiler* comp, Expr* expr) {
    if (!expr) return;

    int line = expr->line;
    int scope_start = comp->function->local_count;

    emit_byte(comp, OP_CONST, line);
    emit_byte(comp, 0xff, line);
    int result_slot = add_local_anon(comp, line);
    if (result_slot < 0) {
        emit_byte(comp, OP_POP, line);
        return;
    }
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)result_slot, line);

    compile_expression(comp, expr->match_expr.subject);
    int subject_slot = add_local_anon(comp, line);
    if (subject_slot < 0) {
        emit_byte(comp, OP_POP, line);
        return;
    }
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)subject_slot, line);

    int* end_jumps = NULL;
    int end_count = 0;
    int end_capacity = 0;

    for (int i = 0; i < expr->match_expr.arm_count; i++) {
        Expr* pattern_expr = expr->match_expr.patterns ? expr->match_expr.patterns[i] : NULL;
        Expr* guard_expr = expr->match_expr.guards ? expr->match_expr.guards[i] : NULL;
        Expr* value_expr = expr->match_expr.values ? expr->match_expr.values[i] : NULL;
        MatchArmJumpList fail_jumps = { 0 };
        int arm_scope_start = comp->function->local_count;

        compile_match_pattern_dispatch(comp,
                                       pattern_expr,
                                       subject_slot,
                                       false,
                                       &fail_jumps,
                                       line);

        if (guard_expr) {
            int guard_line = guard_expr->line > 0 ? guard_expr->line : line;
            compile_expression(comp, guard_expr);
            int guard_fail_jump = emit_jump(comp, OP_JUMP_IF_FALSE, guard_line);
            match_arm_jump_list_add(&fail_jumps, guard_fail_jump);
            emit_byte(comp, OP_POP, guard_line);
        }

        compile_expression(comp, value_expr);
        emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)result_slot, line);
        int end_jump = emit_jump(comp, OP_JUMP, line);
        loop_add_jump(&end_jumps, &end_count, &end_capacity, end_jump);

        end_scope(comp, arm_scope_start);
        match_arm_jump_list_patch_and_pop(comp, &fail_jumps, line);
        match_arm_jump_list_free(&fail_jumps);
    }

    if (expr->match_expr.else_expr) {
        compile_expression(comp, expr->match_expr.else_expr);
        emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)result_slot, line);
    }

    for (int i = 0; i < end_count; i++) {
        patch_jump(comp, end_jumps[i]);
    }
    if (end_jumps) free(end_jumps);

    end_scope(comp, scope_start);
    emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)result_slot, line);
}

static void compile_if_expression(Compiler* comp, Expr* expr) {
    if (!expr) return;

    int line = expr->line;
    int scope_start = comp->function->local_count;

    emit_byte(comp, OP_CONST, line);
    emit_byte(comp, 0xff, line);
    int result_slot = add_local_anon(comp, line);
    if (result_slot < 0) {
        emit_byte(comp, OP_POP, line);
        return;
    }
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)result_slot, line);

    compile_expression(comp, expr->if_expr.condition);
    int else_jump = emit_jump(comp, OP_JUMP_IF_FALSE, line);
    emit_byte(comp, OP_POP, line);

    compile_expression(comp, expr->if_expr.then_expr);
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)result_slot, line);
    int end_jump = emit_jump(comp, OP_JUMP, line);

    patch_jump(comp, else_jump);
    emit_byte(comp, OP_POP, line);

    compile_expression(comp, expr->if_expr.else_expr);
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)result_slot, line);

    patch_jump(comp, end_jump);

    end_scope(comp, scope_start);
    emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)result_slot, line);
}

static void compile_block_expression(Compiler* comp, Expr* expr) {
    if (!expr) return;

    comp->depth++;
    int scope_start = comp->function->local_count;

    compile_statement_list_with_ir(comp,
                                   expr->block_expr.statements,
                                   expr->block_expr.stmt_count,
                                   true);

    compile_expression(comp, expr->block_expr.value);

    end_scope(comp, scope_start);
    comp->depth--;
}

static void compile_while(Compiler* comp, Stmt* stmt) {
    int line = stmt->line;

    int exit_jump = -1;
    bool uses_fast_condition = false;
    bool uses_do_while = false;
    uint8_t a_slot = 0;
    uint8_t b_slot = 0;
    uint8_t b_const_idx = 0;
    bool b_is_const = false;
    uint8_t exit_jump_op = 0;
    uint8_t loop_jump_op = 0;

    Expr* cond = stmt->while_stmt.condition;

    // Fast path: while (a == b) / (a != b) where a and b are locals of the same
    // primitive numeric type. Emit a conditional jump directly (no bool on the stack).
    if (cond && cond->kind == EXPR_BINARY &&
        (cond->binary.op == TOKEN_EQ_EQ || cond->binary.op == TOKEN_BANG_EQ) &&
        cond->binary.left && cond->binary.right &&
        cond->binary.left->kind == EXPR_IDENTIFIER &&
        cond->binary.right->kind == EXPR_IDENTIFIER &&
        cond->binary.left->type && cond->binary.right->type &&
        cond->binary.left->type->kind == cond->binary.right->type->kind &&
        (cond->binary.left->type->kind == TYPE_INT || cond->binary.left->type->kind == TYPE_DOUBLE)) {

        const char* left_name = cond->binary.left->identifier;
        const char* right_name = cond->binary.right->identifier;

        int left_local = resolve_local(comp, left_name);
        int right_local = resolve_local(comp, right_name);

        if (left_local >= 0 && right_local >= 0) {
            a_slot = (uint8_t)left_local;
            b_slot = (uint8_t)right_local;
            switch (cond->binary.op) {
                case TOKEN_EQ_EQ:
                    exit_jump_op = OP_JUMP_IF_LOCAL_NE;   // !(a == b) => a != b
                    loop_jump_op = OP_JUMP_IF_LOCAL_EQ;   // (a == b)
                    break;
                case TOKEN_BANG_EQ:
                    exit_jump_op = OP_JUMP_IF_LOCAL_EQ;   // !(a != b) => a == b
                    loop_jump_op = OP_JUMP_IF_LOCAL_NE;   // (a != b)
                    break;
                default:
                    break;
            }

            if (exit_jump_op != 0 && loop_jump_op != 0) {
                // Lower to:
                //   if !(cond) goto exit;
                //   do { body } while (cond);
                exit_jump = emit_jump_local_compare(comp, exit_jump_op, a_slot, b_slot, line);
                uses_fast_condition = true;
                uses_do_while = true;
            }
        } else if (left_local >= 0 && right_local < 0) {
            int name_idx = make_constant_string(comp, right_name, line);
            if (name_idx >= 0 && name_idx <= 0xff) {
                a_slot = (uint8_t)left_local;
                b_const_idx = (uint8_t)name_idx;
                b_is_const = true;
                switch (cond->binary.op) {
                    case TOKEN_EQ_EQ:
                        exit_jump_op = OP_JUMP_IF_LOCAL_NE_GLOBAL;   // !(a == g) => a != g
                        loop_jump_op = OP_JUMP_IF_LOCAL_EQ_GLOBAL;   // (a == g)
                        break;
                    case TOKEN_BANG_EQ:
                        exit_jump_op = OP_JUMP_IF_LOCAL_EQ_GLOBAL;   // !(a != g) => a == g
                        loop_jump_op = OP_JUMP_IF_LOCAL_NE_GLOBAL;   // (a != g)
                        break;
                    default:
                        break;
                }

                if (exit_jump_op != 0 && loop_jump_op != 0) {
                    // Lower to:
                    //   if !(cond) goto exit;
                    //   do { body } while (cond);
                    exit_jump = emit_jump_local_compare_const(comp, exit_jump_op, a_slot, b_const_idx, line);
                    uses_fast_condition = true;
                    uses_do_while = true;
                }
            }
        } else if (left_local < 0 && right_local >= 0) {
            int name_idx = make_constant_string(comp, left_name, line);
            if (name_idx >= 0 && name_idx <= 0xff) {
                a_slot = (uint8_t)right_local;
                b_const_idx = (uint8_t)name_idx;
                b_is_const = true;
                switch (cond->binary.op) {
                    case TOKEN_EQ_EQ:
                        exit_jump_op = OP_JUMP_IF_LOCAL_NE_GLOBAL;   // !(a == g) => a != g
                        loop_jump_op = OP_JUMP_IF_LOCAL_EQ_GLOBAL;   // (a == g)
                        break;
                    case TOKEN_BANG_EQ:
                        exit_jump_op = OP_JUMP_IF_LOCAL_EQ_GLOBAL;   // !(a != g) => a == g
                        loop_jump_op = OP_JUMP_IF_LOCAL_NE_GLOBAL;   // (a != g)
                        break;
                    default:
                        break;
                }

                if (exit_jump_op != 0 && loop_jump_op != 0) {
                    // Lower to:
                    //   if !(cond) goto exit;
                    //   do { body } while (cond);
                    exit_jump = emit_jump_local_compare_const(comp, exit_jump_op, a_slot, b_const_idx, line);
                    uses_fast_condition = true;
                    uses_do_while = true;
                }
            }
        }
    }

    // Fast path: while (a == <const>) / ... where a is a local numeric and const is a literal.
    if (!uses_fast_condition &&
        cond && cond->kind == EXPR_BINARY &&
        (cond->binary.op == TOKEN_EQ_EQ || cond->binary.op == TOKEN_BANG_EQ) &&
        cond->binary.left && cond->binary.right) {

        Expr* left = cond->binary.left;
        Expr* right = cond->binary.right;
        TokenType op = cond->binary.op;

        // Normalize to: <identifier> <op> <literal>
        if (left->kind == EXPR_LITERAL && right->kind == EXPR_IDENTIFIER) {
            Expr* tmp = left;
            left = right;
            right = tmp;
        }

        if (left->kind == EXPR_IDENTIFIER &&
            right->kind == EXPR_LITERAL &&
            left->type && right->type &&
            left->type->kind == right->type->kind &&
            (left->type->kind == TYPE_INT || left->type->kind == TYPE_DOUBLE)) {

            int a_local = resolve_local(comp, left->identifier);
            if (a_local >= 0) {
                int const_idx_i = -1;
                if (right->type->kind == TYPE_INT) {
                    const_idx_i = make_constant_int(comp, right->literal.as_int, line);
                } else {
                    const_idx_i = make_constant_double(comp, right->literal.as_double, line);
                }

                if (const_idx_i >= 0 && const_idx_i <= 0xff) {
                    a_slot = (uint8_t)a_local;
                    b_const_idx = (uint8_t)const_idx_i;
                    b_is_const = true;
                    switch (op) {
                        case TOKEN_EQ_EQ:
                            exit_jump_op = OP_JUMP_IF_LOCAL_NE_CONST;   // !(a == c) => a != c
                            loop_jump_op = OP_JUMP_IF_LOCAL_EQ_CONST;   // (a == c)
                            break;
                        case TOKEN_BANG_EQ:
                            exit_jump_op = OP_JUMP_IF_LOCAL_EQ_CONST;   // !(a != c) => a == c
                            loop_jump_op = OP_JUMP_IF_LOCAL_NE_CONST;   // (a != c)
                            break;
                        default:
                            break;
                    }

                    if (exit_jump_op != 0 && loop_jump_op != 0) {
                        // Lower to:
                        //   if !(cond) goto exit;
                        //   do { body } while (cond);
                        exit_jump = emit_jump_local_compare_const(comp, exit_jump_op, a_slot, b_const_idx, line);
                        uses_fast_condition = true;
                        uses_do_while = true;
                    }
                }
            }
        }
    }

    // Fast path: while (a < b) / (a <= b) / (a > b) / (a >= b) where a and b
    // are locals of the same primitive numeric type. Emit a conditional jump
    // directly (no bool on the stack).
    if (!uses_fast_condition &&
        cond && cond->kind == EXPR_BINARY &&
        (cond->binary.op == TOKEN_LT || cond->binary.op == TOKEN_LT_EQ ||
         cond->binary.op == TOKEN_GT || cond->binary.op == TOKEN_GT_EQ) &&
        cond->binary.left && cond->binary.right &&
        cond->binary.left->kind == EXPR_IDENTIFIER &&
        cond->binary.right->kind == EXPR_IDENTIFIER &&
        cond->binary.left->type && cond->binary.right->type &&
        cond->binary.left->type->kind == cond->binary.right->type->kind &&
        (cond->binary.left->type->kind == TYPE_INT || cond->binary.left->type->kind == TYPE_DOUBLE)) {

        int a_local = resolve_local(comp, cond->binary.left->identifier);
        int b_local = resolve_local(comp, cond->binary.right->identifier);
        if (a_local >= 0 && b_local >= 0) {
            a_slot = (uint8_t)a_local;
            b_slot = (uint8_t)b_local;
            switch (cond->binary.op) {
                case TOKEN_LT:
                    exit_jump_op = OP_JUMP_IF_LOCAL_GE;   // !(a < b)  => a >= b
                    loop_jump_op = OP_JUMP_IF_LOCAL_LT;   // (a < b)
                    break;
                case TOKEN_LT_EQ:
                    exit_jump_op = OP_JUMP_IF_LOCAL_GT;   // !(a <= b) => a > b
                    loop_jump_op = OP_JUMP_IF_LOCAL_LE;   // (a <= b)
                    break;
                case TOKEN_GT:
                    exit_jump_op = OP_JUMP_IF_LOCAL_LE;   // !(a > b)  => a <= b
                    loop_jump_op = OP_JUMP_IF_LOCAL_GT;   // (a > b)
                    break;
                case TOKEN_GT_EQ:
                    exit_jump_op = OP_JUMP_IF_LOCAL_LT;   // !(a >= b) => a < b
                    loop_jump_op = OP_JUMP_IF_LOCAL_GE;   // (a >= b)
                    break;
                default:
                    break;
            }

            if (exit_jump_op != 0 && loop_jump_op != 0) {
                // Lower to:
                //   if !(cond) goto exit;
                //   do { body } while (cond);
                exit_jump = emit_jump_local_compare(comp, exit_jump_op, a_slot, b_slot, line);
                uses_fast_condition = true;
                uses_do_while = true;
            }
        }
    }

    // Fast path: while (a < <const>) / ... where a is a local numeric and const is a literal.
    if (!uses_fast_condition &&
        cond && cond->kind == EXPR_BINARY &&
        (cond->binary.op == TOKEN_LT || cond->binary.op == TOKEN_LT_EQ ||
         cond->binary.op == TOKEN_GT || cond->binary.op == TOKEN_GT_EQ) &&
        cond->binary.left && cond->binary.right) {

        Expr* left = cond->binary.left;
        Expr* right = cond->binary.right;
        TokenType op = cond->binary.op;

        // Normalize to: <identifier> <op> <literal>
        if (left->kind == EXPR_LITERAL && right->kind == EXPR_IDENTIFIER) {
            Expr* tmp = left;
            left = right;
            right = tmp;
            switch (op) {
                case TOKEN_LT: op = TOKEN_GT; break;
                case TOKEN_LT_EQ: op = TOKEN_GT_EQ; break;
                case TOKEN_GT: op = TOKEN_LT; break;
                case TOKEN_GT_EQ: op = TOKEN_LT_EQ; break;
                default: break;
            }
        }

        if (left->kind == EXPR_IDENTIFIER &&
            right->kind == EXPR_LITERAL &&
            left->type && right->type &&
            left->type->kind == right->type->kind &&
            (left->type->kind == TYPE_INT || left->type->kind == TYPE_DOUBLE)) {

            int a_local = resolve_local(comp, left->identifier);
            if (a_local >= 0) {
                int const_idx_i = -1;
                if (right->type->kind == TYPE_INT) {
                    const_idx_i = make_constant_int(comp, right->literal.as_int, line);
                } else {
                    const_idx_i = make_constant_double(comp, right->literal.as_double, line);
                }

                if (const_idx_i >= 0 && const_idx_i <= 0xff) {
                    a_slot = (uint8_t)a_local;
                    b_const_idx = (uint8_t)const_idx_i;
                    b_is_const = true;
                    switch (op) {
                        case TOKEN_LT:
                            exit_jump_op = OP_JUMP_IF_LOCAL_GE_CONST;   // !(a < c)  => a >= c
                            loop_jump_op = OP_JUMP_IF_LOCAL_LT_CONST;   // (a < c)
                            break;
                        case TOKEN_LT_EQ:
                            exit_jump_op = OP_JUMP_IF_LOCAL_GT_CONST;   // !(a <= c) => a > c
                            loop_jump_op = OP_JUMP_IF_LOCAL_LE_CONST;   // (a <= c)
                            break;
                        case TOKEN_GT:
                            exit_jump_op = OP_JUMP_IF_LOCAL_LE_CONST;   // !(a > c)  => a <= c
                            loop_jump_op = OP_JUMP_IF_LOCAL_GT_CONST;   // (a > c)
                            break;
                        case TOKEN_GT_EQ:
                            exit_jump_op = OP_JUMP_IF_LOCAL_LT_CONST;   // !(a >= c) => a < c
                            loop_jump_op = OP_JUMP_IF_LOCAL_GE_CONST;   // (a >= c)
                            break;
                        default:
                            break;
                    }

                    if (exit_jump_op != 0 && loop_jump_op != 0) {
                        // Lower to:
                        //   if !(cond) goto exit;
                        //   do { body } while (cond);
                        exit_jump = emit_jump_local_compare_const(comp, exit_jump_op, a_slot, b_const_idx, line);
                        uses_fast_condition = true;
                        uses_do_while = true;
                    }
                }
            }
        }
    }

    if (!uses_fast_condition) {
        int loop_start = comp->chunk->code_count;
        compiler_push_loop(comp, loop_start, true, loop_start);
        compile_expression(comp, stmt->while_stmt.condition);
        exit_jump = emit_jump(comp, OP_JUMP_IF_FALSE, line);
        emit_byte(comp, OP_POP, line);
        compile_statement(comp, stmt->while_stmt.body);

        emit_loop(comp, loop_start, line);
        patch_jump(comp, exit_jump);
        emit_byte(comp, OP_POP, line);

        // Patch breaks to jump past any loop cleanup.
        LoopContext* ctx = current_loop(comp);
        if (ctx) {
            for (int i = 0; i < ctx->break_count; i++) {
                patch_jump(comp, ctx->break_jumps[i]);
            }
        }

        compiler_pop_loop(comp);
        return;
    }

    if (uses_do_while) {
        int body_start = comp->chunk->code_count;
        compiler_push_loop(comp, body_start, false, -1);

        compile_statement(comp, stmt->while_stmt.body);

        LoopContext* ctx = current_loop(comp);
        if (ctx) {
            ctx->continue_target = comp->chunk->code_count;
            ctx->continue_target_known = true;
            for (int i = 0; i < ctx->continue_count; i++) {
                patch_jump(comp, ctx->continue_jumps[i]);
            }
        }

        if (b_is_const) {
            emit_loop_local_compare_const(comp, loop_jump_op, a_slot, b_const_idx, body_start, line);
        } else {
            emit_loop_local_compare(comp, loop_jump_op, a_slot, b_slot, body_start, line);
        }
        patch_jump(comp, exit_jump);

        // Patch breaks to jump past the loop.
        if (ctx) {
            for (int i = 0; i < ctx->break_count; i++) {
                patch_jump(comp, ctx->break_jumps[i]);
            }
        }

        compiler_pop_loop(comp);
        return;
    }
}

static void compile_foreach(Compiler* comp, Stmt* stmt) {
    int line = stmt->line;

    int scope_start = comp->function->local_count;

    // Evaluate iterable and store in an anonymous local slot.
    compile_expression(comp, stmt->foreach.iterable);
    int iterable_slot = add_local_anon(comp, line);
    if (iterable_slot < 0) return;
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)iterable_slot, line);

    // Initialize loop index to 0 in an anonymous local slot.
    int zero_idx = make_constant_int(comp, 0, line);
    if (zero_idx < 0) return;
    emit_constant_index(comp, zero_idx, line);
    int index_slot = add_local_anon(comp, line);
    if (index_slot < 0) return;
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)index_slot, line);

    int loop_start = comp->chunk->code_count;

    // Condition: index < len(iterable)
    emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)index_slot, line);
    emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)iterable_slot, line);
    emit_byte(comp, OP_ARRAY_LEN, line);
    emit_byte(comp, OP_LT, line);

    int exit_jump = emit_jump(comp, OP_JUMP_IF_FALSE, line);
    emit_byte(comp, OP_POP, line); // true-path: pop condition result

    // loop variable = iterable[index]
    emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)iterable_slot, line);
    emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)index_slot, line);
    emit_byte(comp, OP_ARRAY_GET, line);
    int elem_slot = add_local(comp, stmt->foreach.var_name, line);
    if (elem_slot < 0) return;
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)elem_slot, line);

    // Loop body. Continue jumps should land at the increment step, which is emitted after the body.
    compiler_push_loop(comp, loop_start, false, -1);
    compile_statement(comp, stmt->foreach.body);

    LoopContext* ctx = current_loop(comp);
    if (ctx) {
        ctx->continue_target = comp->chunk->code_count;
        ctx->continue_target_known = true;
        for (int i = 0; i < ctx->continue_count; i++) {
            patch_jump(comp, ctx->continue_jumps[i]);
        }
    }

    // index = index + 1
    emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)index_slot, line);
    int one_idx = make_constant_int(comp, 1, line);
    if (one_idx < 0) return;
    emit_constant_index(comp, one_idx, line);
    emit_byte(comp, OP_ADD, line);
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)index_slot, line);

    emit_loop(comp, loop_start, line);

    patch_jump(comp, exit_jump);
    emit_byte(comp, OP_POP, line); // false-path: pop condition result

    // Patch breaks to jump past the loop cleanup POP.
    if (ctx) {
        for (int i = 0; i < ctx->break_count; i++) {
            patch_jump(comp, ctx->break_jumps[i]);
        }
    }

    compiler_pop_loop(comp);
    end_scope(comp, scope_start);
}

static void compile_for_range(Compiler* comp, Stmt* stmt) {
    int line = stmt->line;
    int scope_start = comp->function->local_count;

    // Evaluate range bounds once before entering the loop.
    compile_expression(comp, stmt->for_range.start);
    int iter_slot = add_local(comp, stmt->for_range.var_name, line);
    if (iter_slot < 0) return;
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)iter_slot, line);

    compile_expression(comp, stmt->for_range.end);
    int end_slot = add_local_anon(comp, line);
    if (end_slot < 0) return;
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)end_slot, line);

    int loop_start = comp->chunk->code_count;

    // Condition: iter < end (start inclusive, end exclusive).
    emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)iter_slot, line);
    emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)end_slot, line);
    emit_byte(comp, OP_LT, line);
    int exit_jump = emit_jump(comp, OP_JUMP_IF_FALSE, line);
    emit_byte(comp, OP_POP, line);

    compiler_push_loop(comp, loop_start, false, -1);
    compile_statement(comp, stmt->for_range.body);

    LoopContext* ctx = current_loop(comp);
    if (ctx) {
        ctx->continue_target = comp->chunk->code_count;
        ctx->continue_target_known = true;
        for (int i = 0; i < ctx->continue_count; i++) {
            patch_jump(comp, ctx->continue_jumps[i]);
        }
    }

    // iter = iter + 1
    emit_byte2(comp, OP_LOAD_LOCAL, (uint8_t)iter_slot, line);
    int one_idx = make_constant_int(comp, 1, line);
    if (one_idx < 0) return;
    emit_constant_index(comp, one_idx, line);
    emit_byte(comp, OP_ADD, line);
    emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)iter_slot, line);

    emit_loop(comp, loop_start, line);

    patch_jump(comp, exit_jump);
    emit_byte(comp, OP_POP, line);

    if (ctx) {
        for (int i = 0; i < ctx->break_count; i++) {
            patch_jump(comp, ctx->break_jumps[i]);
        }
    }

    compiler_pop_loop(comp);
    end_scope(comp, scope_start);
}

static void compile_return(Compiler* comp, Stmt* stmt) {
    int line = stmt->line;

    if (comp->is_top_level) {
        comp->had_error = true;
        if (!comp->error) {
            comp->error = error_create(ERROR_COMPILE, "return used outside of a function", stmt->file, stmt->line, stmt->column);
        }
        return;
    }

    if (stmt->return_value) {
        compile_expression(comp, stmt->return_value);
    } else {
        emit_byte(comp, OP_CONST, line);
        emit_byte(comp, 0xff, line);
    }

    if (comp->defer_enabled && comp->defer_return_slot >= 0) {
        emit_byte2(comp, OP_STORE_LOCAL, (uint8_t)comp->defer_return_slot, line);
        int j = emit_jump(comp, OP_JUMP, line);
        loop_add_jump(&comp->defer_return_jumps, &comp->defer_return_jump_count, &comp->defer_return_jump_capacity, j);
        return;
    }

    emit_byte(comp, OP_RET, line);
}

ObjFunction** compiler_collect_functions(SymbolTable* table, int* count) {
    *count = 0;

    for (int i = 0; i < table->symbol_count; i++) {
        Symbol* sym = table->symbols[i];
        if (sym->type->kind == TYPE_FUNCTION && sym->function_obj) {
            (*count)++;
        }
    }

    if (*count == 0) return NULL;

    ObjFunction** functions = (ObjFunction**)safe_malloc(*count * sizeof(ObjFunction*));
    int idx = 0;

    for (int i = 0; i < table->symbol_count; i++) {
        Symbol* sym = table->symbols[i];
        if (sym->type->kind == TYPE_FUNCTION && sym->function_obj) {
            functions[idx++] = sym->function_obj;
        }
    }

    return functions;
}

static void compile_defer(Compiler* comp, Stmt* stmt) {
    int line = stmt->line;

    if (comp->is_top_level) {
        comp->had_error = true;
        if (!comp->error) {
            comp->error = error_create(ERROR_COMPILE, "defer used outside of a function", stmt->file, stmt->line, stmt->column);
        }
        return;
    }

    Expr* expr = stmt->defer_expr;
    if (!expr || expr->kind != EXPR_CALL) {
        comp->had_error = true;
        if (!comp->error) {
            comp->error = error_create(ERROR_COMPILE, "defer expects a function call", stmt->file, stmt->line, stmt->column);
        }
        return;
    }
    if (expr->call.arg_count > 255) {
        compiler_set_error(comp, "Too many deferred call arguments (max 255)", stmt->file, stmt->line, stmt->column);
        return;
    }

    // Evaluate callee + args at defer-time (Go semantics).
    compile_expression(comp, expr->call.callee);
    for (int i = 0; i < expr->call.arg_count; i++) {
        compile_expression(comp, expr->call.args[i]);
    }

    emit_byte2(comp, OP_DEFER, (uint8_t)expr->call.arg_count, line);
}

static bool stmt_contains_defer(Stmt* stmt) {
    if (!stmt) return false;

    switch (stmt->kind) {
        case STMT_DEFER:
            return true;
        case STMT_BLOCK:
            for (int i = 0; i < stmt->block.stmt_count; i++) {
                if (stmt_contains_defer(stmt->block.statements[i])) return true;
            }
            return false;
        case STMT_IF:
            if (stmt_contains_defer(stmt->if_stmt.then_branch)) return true;
            if (stmt_contains_defer(stmt->if_stmt.else_branch)) return true;
            return false;
        case STMT_MATCH:
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                if (stmt_contains_defer(stmt->match_stmt.bodies[i])) return true;
            }
            if (stmt_contains_defer(stmt->match_stmt.else_branch)) return true;
            return false;
        case STMT_WHILE:
            return stmt_contains_defer(stmt->while_stmt.body);
        case STMT_FOREACH:
            return stmt_contains_defer(stmt->foreach.body);
        case STMT_FOR_RANGE:
            return stmt_contains_defer(stmt->for_range.body);
        default:
            return false;
    }
}

static void compile_func_decl(Compiler* comp, Stmt* stmt) {
    ObjFunction* func_obj = obj_function_create();
    chunk_init(&func_obj->chunk);
    constant_pool_init(&func_obj->constants);
    func_obj->param_count = stmt->func_decl.param_count;
    func_obj->param_names = (char**)safe_malloc(stmt->func_decl.param_count * sizeof(char*));
    for (int i = 0; i < stmt->func_decl.param_count; i++) {
        func_obj->param_names[i] = safe_strdup(stmt->func_decl.params[i]);
    }
    func_obj->local_count = 0;
    func_obj->local_names = NULL;
    func_obj->debug_local_names = NULL;
    func_obj->local_types = NULL;
    func_obj->is_async = stmt->func_decl.is_async;
    func_obj->defer_handler_ip = -1;
    func_obj->defer_return_slot = -1;
    func_obj->name = safe_strdup(stmt->func_decl.name);
    const char* fn_source_file = NULL;
    if (stmt->file && stmt->file[0] != '\0') {
        fn_source_file = stmt->file;
    } else if (comp && comp->file && comp->file[0] != '\0') {
        fn_source_file = comp->file;
    }
    if (fn_source_file) {
        func_obj->source_file = safe_strdup(fn_source_file);
    }
    func_obj->ref_count = 1;
    compiler_try_assign_jit_hint(func_obj,
                                 stmt->func_decl.return_type,
                                 stmt->func_decl.params,
                                 stmt->func_decl.param_types,
                                 stmt->func_decl.param_count,
                                 stmt->func_decl.body);

    Compiler func_compiler;
    func_compiler.chunk = &func_obj->chunk;
    func_compiler.function = func_obj;
    func_compiler.globals = comp->globals;
    func_compiler.locals = NULL;
    func_compiler.local_count = 0;
    func_compiler.local_capacity = 0;
    func_compiler.depth = 0;
    func_compiler.is_top_level = false;
    func_compiler.current_function_is_async = stmt->func_decl.is_async;
    func_compiler.had_error = false;
    func_compiler.error = NULL;
    func_compiler.file = stmt->file;
    func_compiler.vm = NULL;
    func_compiler.loop_stack = NULL;
    func_compiler.loop_stack_count = 0;
    func_compiler.loop_stack_capacity = 0;
    func_compiler.record_decls = comp->record_decls;
    func_compiler.record_decl_count = comp->record_decl_count;
    func_compiler.enum_decls = comp->enum_decls;
    func_compiler.enum_decl_count = comp->enum_decl_count;
    func_compiler.function_decls = comp->function_decls;
    func_compiler.function_decl_count = comp->function_decl_count;
    func_compiler.defer_enabled = stmt_contains_defer(stmt->func_decl.body);
    func_compiler.defer_return_slot = -1;
    func_compiler.defer_return_jumps = NULL;
    func_compiler.defer_return_jump_count = 0;
    func_compiler.defer_return_jump_capacity = 0;
    func_compiler.shared_anon_func_counter = comp->shared_anon_func_counter;

    for (int i = 0; i < stmt->func_decl.param_count; i++) {
        if (add_local(&func_compiler, stmt->func_decl.params[i], stmt->line) < 0) {
            break;
        }
    }

    if (func_compiler.defer_enabled) {
        func_compiler.defer_return_slot = add_local_anon(&func_compiler, stmt->line);
        if (func_compiler.defer_return_slot < 0) {
            func_compiler.defer_enabled = false;
        }
    }

    if (stmt->func_decl.body && stmt->func_decl.body->kind == STMT_BLOCK) {
        compile_block(&func_compiler, stmt->func_decl.body);
    }

    if (func_compiler.defer_enabled) {
        // Implicit return nil.
        emit_byte(&func_compiler, OP_CONST, stmt->line);
        emit_byte(&func_compiler, 0xff, stmt->line);
        emit_byte2(&func_compiler, OP_STORE_LOCAL, (uint8_t)func_compiler.defer_return_slot, stmt->line);

        // Patch all returns/try-returns to jump to the shared epilogue.
        for (int i = 0; i < func_compiler.defer_return_jump_count; i++) {
            patch_jump(&func_compiler, func_compiler.defer_return_jumps[i]);
        }

        // Epilogue: run defers in LIFO order, then return stored value.
        int epilogue_start = func_compiler.chunk->code_count;
        func_obj->defer_handler_ip = epilogue_start;
        func_obj->defer_return_slot = func_compiler.defer_return_slot;
        emit_byte(&func_compiler, OP_DEFER_HAS, stmt->line);
        int exit_jump = emit_jump(&func_compiler, OP_JUMP_IF_FALSE, stmt->line);
        emit_byte(&func_compiler, OP_POP, stmt->line);
        emit_byte(&func_compiler, OP_DEFER_CALL, stmt->line);
        emit_byte(&func_compiler, OP_POP, stmt->line);
        emit_loop(&func_compiler, epilogue_start, stmt->line);
        patch_jump(&func_compiler, exit_jump);
        emit_byte(&func_compiler, OP_POP, stmt->line);

        emit_byte2(&func_compiler, OP_LOAD_LOCAL, (uint8_t)func_compiler.defer_return_slot, stmt->line);
        emit_byte(&func_compiler, OP_RET, stmt->line);
    } else {
        emit_byte(&func_compiler, OP_CONST, stmt->line);
        emit_byte(&func_compiler, 0xff, stmt->line);
        emit_byte(&func_compiler, OP_RET, stmt->line);
    }

    comp->had_error = comp->had_error || func_compiler.had_error;
    if (func_compiler.error && !comp->error) {
        comp->error = func_compiler.error;
    }
    compiler_refresh_jit_profile_metadata(func_obj);

    if (func_compiler.loop_stack) free(func_compiler.loop_stack);
    if (func_compiler.defer_return_jumps) free(func_compiler.defer_return_jumps);

    // Fix 2: Add function symbol to globals so compiler_collect_functions can find it
    Type* return_type = type_clone(stmt->func_decl.return_type);
    if (stmt->func_decl.is_async) {
        return_type = type_future(return_type ? return_type : type_void());
    }
    Type** param_types = NULL;
    if (stmt->func_decl.param_count > 0) {
        param_types = (Type**)safe_malloc(stmt->func_decl.param_count * sizeof(Type*));
        for (int i = 0; i < stmt->func_decl.param_count; i++) {
            param_types[i] = type_clone(stmt->func_decl.param_types[i]);
        }
    }
    Type* func_type = type_function(return_type, param_types, stmt->func_decl.param_count);
    type_function_set_type_params(func_type,
                                  stmt->func_decl.type_params,
                                  stmt->func_decl.type_param_constraints,
                                  stmt->func_decl.type_param_count);
    Symbol* sym = symbol_create(func_type, stmt->func_decl.name, false);
    sym->function_obj = (void*)func_obj;
    symbol_table_add(comp->globals, sym);
}

static void compile_enum_decl(Compiler* comp, Stmt* stmt) {
    if (!comp || !stmt || stmt->kind != STMT_ENUM_DECL) return;
    int line = stmt->line;

    if (stmt->enum_decl.has_payload_members) {
        // Payload enum constructors are lowered at identifier/call use sites.
        return;
    }

    for (int i = 0; i < stmt->enum_decl.member_count; i++) {
        char* symbol_name = compiler_enum_member_symbol_name(stmt->enum_decl.name, stmt->enum_decl.member_names[i]);
        if (!symbol_name) continue;

        emit_constant(comp, (Constant){ .as_int = stmt->enum_decl.member_values[i], .type_index = 0 }, line);
        int name_idx = make_constant_string(comp, symbol_name, line);
        free(symbol_name);
        if (name_idx < 0) return;
        emit_store_global_name(comp, name_idx, line);
    }
}

static void compile_statement(Compiler* comp, Stmt* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            compile_var_decl(comp, stmt);
            break;
        case STMT_VAR_TUPLE_DECL:
            compile_var_tuple_decl(comp, stmt);
            break;
        case STMT_EXPR:
            compile_expr_stmt(comp, stmt);
            break;
        case STMT_ASSIGN:
            compile_assign(comp, stmt);
            break;
        case STMT_ASSIGN_INDEX:
            compile_assign_index(comp, stmt);
            break;
        case STMT_ASSIGN_FIELD:
            compile_assign_field(comp, stmt);
            break;
        case STMT_BLOCK:
            compile_block(comp, stmt);
            break;
        case STMT_IF:
            compile_if(comp, stmt);
            break;
        case STMT_MATCH:
            compile_match(comp, stmt);
            break;
        case STMT_WHILE:
            compile_while(comp, stmt);
            break;
        case STMT_FOREACH:
            compile_foreach(comp, stmt);
            break;
        case STMT_FOR_RANGE:
            compile_for_range(comp, stmt);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE: {
            LoopContext* ctx = current_loop(comp);
            if (!ctx) {
                comp->had_error = true;
                if (!comp->error) {
                    comp->error = error_create(ERROR_COMPILE, "break/continue used outside of a loop", stmt->file, stmt->line, stmt->column);
                }
                break;
            }

            if (stmt->kind == STMT_BREAK) {
                int j = emit_jump(comp, OP_JUMP, stmt->line);
                loop_add_jump(&ctx->break_jumps, &ctx->break_count, &ctx->break_capacity, j);
                break;
            }

            // continue
            if (ctx->continue_target_known && ctx->continue_target <= comp->chunk->code_count) {
                emit_loop(comp, ctx->continue_target, stmt->line);
            } else {
                int j = emit_jump(comp, OP_JUMP, stmt->line);
                loop_add_jump(&ctx->continue_jumps, &ctx->continue_count, &ctx->continue_capacity, j);
            }
            break;
        }
        case STMT_DEFER:
            compile_defer(comp, stmt);
            break;
        case STMT_RETURN:
            compile_return(comp, stmt);
            break;
        case STMT_FUNC_DECL:
            compile_func_decl(comp, stmt);
            break;
        case STMT_RECORD_DECL:
            // Record declarations are handled at the type checking phase
            // No bytecode is generated for record type declarations
            break;
        case STMT_INTERFACE_DECL:
            // Interface declarations are compile-time only and emit no bytecode.
            break;
        case STMT_IMPL_DECL:
            // Interface impl declarations are compile-time only and emit no bytecode.
            break;
        case STMT_TYPE_ALIAS:
            // Type aliases are compile-time only and emit no bytecode.
            break;
        case STMT_ENUM_DECL:
            // Enums lower to global int constants at runtime initialization.
            compile_enum_decl(comp, stmt);
            break;
        default:
            break;
    }
}

CompileResult compile(Program* program) {
    CompileResult result;
    result.error = NULL;
    result.functions = NULL;
    result.function_count = 0;

    // Collect record declarations for field index resolution fallback.
    Stmt** record_decls = NULL;
    int record_decl_count = 0;

    // Collect enum declarations for payload constructor lowering.
    Stmt** enum_decls = NULL;
    int enum_decl_count = 0;

    // Collect function declarations for compile-time inlining.
    Stmt** function_decls = NULL;
    int function_decl_count = 0;
    if (program && program->stmt_count > 0) {
        for (int i = 0; i < program->stmt_count; i++) {
            Stmt* stmt = program->statements[i];
            if (stmt && stmt->kind == STMT_RECORD_DECL) {
                record_decl_count++;
                record_decls = (Stmt**)safe_realloc(record_decls, record_decl_count * sizeof(Stmt*));
                record_decls[record_decl_count - 1] = stmt;
            }
            if (stmt && stmt->kind == STMT_ENUM_DECL) {
                enum_decl_count++;
                enum_decls = (Stmt**)safe_realloc(enum_decls, enum_decl_count * sizeof(Stmt*));
                enum_decls[enum_decl_count - 1] = stmt;
            }
            if (stmt && stmt->kind == STMT_FUNC_DECL) {
                function_decl_count++;
                function_decls = (Stmt**)safe_realloc(function_decls, function_decl_count * sizeof(Stmt*));
                function_decls[function_decl_count - 1] = stmt;
            }
        }
    }

    ObjFunction* main_func = obj_function_create();
    chunk_init(&main_func->chunk);
    constant_pool_init(&main_func->constants);
    main_func->param_count = 0;
    main_func->param_names = NULL;
    main_func->local_count = 0;
    main_func->local_names = NULL;
    main_func->debug_local_names = NULL;
    main_func->local_types = NULL;
    main_func->is_async = false;
    if (program && program->source_file && program->source_file[0] != '\0') {
        main_func->source_file = safe_strdup(program->source_file);
    }
    main_func->ref_count = 1;

    int anon_func_counter = 0;

    Compiler compiler;
    compiler.chunk = &main_func->chunk;
    compiler.function = main_func;
    compiler.globals = symbol_table_create();
    compiler.locals = NULL;
    compiler.local_count = 0;
    compiler.local_capacity = 0;
    compiler.depth = 0;
    compiler.is_top_level = true;
    compiler.current_function_is_async = false;
    compiler.had_error = false;
    compiler.error = NULL;
    compiler.file = (program && program->source_file) ? program->source_file : NULL;
    compiler.vm = NULL;
    compiler.loop_stack = NULL;
    compiler.loop_stack_count = 0;
    compiler.loop_stack_capacity = 0;
    compiler.record_decls = record_decls;
    compiler.record_decl_count = record_decl_count;
    compiler.enum_decls = enum_decls;
    compiler.enum_decl_count = enum_decl_count;
    compiler.function_decls = function_decls;
    compiler.function_decl_count = function_decl_count;
    compiler.defer_enabled = false;
    compiler.defer_return_slot = -1;
    compiler.defer_return_jumps = NULL;
    compiler.defer_return_jump_count = 0;
    compiler.defer_return_jump_capacity = 0;
    compiler.shared_anon_func_counter = &anon_func_counter;

    compile_statement_list_with_ir(&compiler,
                                   program ? program->statements : NULL,
                                   program ? program->stmt_count : 0,
                                   true);

    emit_byte(&compiler, OP_CONST, 0);
    emit_byte(&compiler, 0xff, 0);
    emit_byte(&compiler, OP_RET, 0);
    compiler_refresh_jit_profile_metadata(main_func);

    peephole_optimize(&main_func->chunk);

    result.function = main_func;
    result.globals = compiler.globals;

    result.functions = compiler_collect_functions(compiler.globals, &result.function_count);
    superinstruction_optimize(&main_func->chunk);
    for (int i = 0; i < result.function_count; i++) {
        ObjFunction* func = result.functions ? result.functions[i] : NULL;
        if (func) {
            superinstruction_optimize(&func->chunk);
        }
    }
    result.error = compiler.had_error ? compiler.error : NULL;

    if (compiler.loop_stack) free(compiler.loop_stack);
    if (record_decls) free(record_decls);
    if (enum_decls) free(enum_decls);
    if (function_decls) free(function_decls);

    return result;
}

static int peephole_instruction_len(uint8_t op) {
    switch (op) {
        case OP_JUMP_IF_LOCAL_LT:
        case OP_JUMP_IF_LOCAL_LE:
        case OP_JUMP_IF_LOCAL_GT:
        case OP_JUMP_IF_LOCAL_GE:
        case OP_JUMP_IF_LOCAL_EQ:
        case OP_JUMP_IF_LOCAL_NE:
        case OP_JUMP_IF_LOCAL_EQ_GLOBAL:
        case OP_JUMP_IF_LOCAL_NE_GLOBAL:
        case OP_JUMP_IF_LOCAL_EQ_GLOBAL_SLOT:
        case OP_JUMP_IF_LOCAL_NE_GLOBAL_SLOT:
        case OP_JUMP_IF_LOCAL_LT_CONST:
        case OP_JUMP_IF_LOCAL_LE_CONST:
        case OP_JUMP_IF_LOCAL_GT_CONST:
        case OP_JUMP_IF_LOCAL_GE_CONST:
        case OP_JUMP_IF_LOCAL_EQ_CONST:
        case OP_JUMP_IF_LOCAL_NE_CONST:
        case OP_JUMP_IF_ARRAY_FALSE_LOCAL_CONST:
        case OP_JUMP_IF_ARRAY_FALSE_LOCAL_LOCAL:
        case OP_ADD_LOCALS_INT:
        case OP_SUB_LOCALS_INT:
        case OP_MUL_LOCALS_INT:
        case OP_DIV_LOCALS_INT:
        case OP_MOD_LOCALS_INT:
        case OP_BIT_AND_LOCALS_INT:
        case OP_BIT_OR_LOCALS_INT:
        case OP_BIT_XOR_LOCALS_INT:
        case OP_ADD_LOCALS_DOUBLE:
        case OP_SUB_LOCALS_DOUBLE:
        case OP_MUL_LOCALS_DOUBLE:
        case OP_DIV_LOCALS_DOUBLE:
        case OP_ADD_LOCAL_CONST_INT:
        case OP_SUB_LOCAL_CONST_INT:
        case OP_MUL_LOCAL_CONST_INT:
        case OP_DIV_LOCAL_CONST_INT:
        case OP_MOD_LOCAL_CONST_INT:
        case OP_BIT_AND_LOCAL_CONST_INT:
        case OP_BIT_OR_LOCAL_CONST_INT:
        case OP_BIT_XOR_LOCAL_CONST_INT:
        case OP_ADD_LOCAL_CONST_DOUBLE:
        case OP_SUB_LOCAL_CONST_DOUBLE:
        case OP_MUL_LOCAL_CONST_DOUBLE:
        case OP_DIV_LOCAL_CONST_DOUBLE:
        case OP_ARRAY_GET_LOCAL_CONST_INT_TO_LOCAL:
        case OP_ARRAY_GET_LOCAL_LOCAL_INT_TO_LOCAL:
        case OP_ARRAY_GET_LOCAL_CONST_DOUBLE_TO_LOCAL:
        case OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE_TO_LOCAL:
            return 5;
        case OP_ARRAY_GET_FIELD_LOCAL_CONST:
        case OP_ARRAY_GET_FIELD_LOCAL_LOCAL:
        case OP_ARRAY_SET_FIELD_LOCAL_CONST:
        case OP_ARRAY_SET_FIELD_LOCAL_LOCAL:
        case OP_ADD_LOCAL_DIV_LOCALS:
        case OP_MADD_LOCAL_ARRAY_LOCAL_INT:
        case OP_MADD_LOCAL_ARRAY_LOCAL_DOUBLE:
        case OP_RECORD_NEW_NAMED:
        case OP_JUMP_IF_STACK_LT_LOCAL:
        case OP_JUMP_IF_STACK_LE_LOCAL:
        case OP_JUMP_IF_STACK_GT_LOCAL:
        case OP_JUMP_IF_STACK_GE_LOCAL:
        case OP_JUMP_IF_STACK_LT_CONST:
        case OP_JUMP_IF_STACK_LE_CONST:
        case OP_JUMP_IF_STACK_GT_CONST:
        case OP_JUMP_IF_STACK_GE_CONST:
            return 4;
        case OP_JUMP_IF_FALSE_POP:
            return 4;
        case OP_EVALA_MADD_LOCAL_ARRAY_LOCAL_DOUBLE:
        case OP_MUL_LOCALS_INT_TO_LOCAL:
        case OP_MUL_LOCALS_DOUBLE_TO_LOCAL:
        case OP_ADD_GLOBAL_GLOBAL_TO_GLOBAL:
        case OP_SUB_GLOBAL_GLOBAL_TO_GLOBAL:
        case OP_MUL_GLOBAL_GLOBAL_TO_GLOBAL:
        case OP_DIV_GLOBAL_GLOBAL_TO_GLOBAL:
        case OP_ADD_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
        case OP_SUB_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
        case OP_MUL_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
        case OP_DIV_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
            return 7;
        case OP_ADD2_LOCAL_CONST:
        case OP_CALL_INTERFACE:
            return 6;
        case OP_TYPE_TEST_INTERFACE_METHOD:
            return 5;
        case OP_CALL_GLOBAL16:
            return 4;
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_ADD_LOCAL_CONST:
        case OP_SUB_LOCAL_CONST:
        case OP_CALL_GLOBAL:
        case OP_ARRAY_GET_LOCAL_CONST:
        case OP_ARRAY_GET_LOCAL_LOCAL:
        case OP_ARRAY_SET_LOCAL_CONST:
        case OP_ARRAY_SET_LOCAL_LOCAL:
        case OP_ARRAY_GET_LOCAL_CONST_INT:
        case OP_ARRAY_GET_LOCAL_LOCAL_INT:
        case OP_ARRAY_SET_LOCAL_CONST_INT:
        case OP_ARRAY_SET_LOCAL_LOCAL_INT:
        case OP_ARRAY_GET_LOCAL_CONST_DOUBLE:
        case OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE:
        case OP_ARRAY_SET_LOCAL_CONST_DOUBLE:
        case OP_ARRAY_SET_LOCAL_LOCAL_DOUBLE:
        case OP_ARRAY_BOUNDS_CHECK_LOCAL_CONST:
        case OP_ARRAY_BOUNDS_CHECK_LOCAL_LOCAL:
        case OP_SQRT_LOCAL_DOUBLE:
        case OP_ARRAY_LEN_LOCAL:
        case OP_ADD_STACK_LOCAL_INT:
        case OP_SUB_STACK_LOCAL_INT:
        case OP_MUL_STACK_LOCAL_INT:
        case OP_DIV_STACK_LOCAL_INT:
        case OP_MOD_STACK_LOCAL_INT:
        case OP_BIT_AND_STACK_LOCAL_INT:
        case OP_BIT_OR_STACK_LOCAL_INT:
        case OP_BIT_XOR_STACK_LOCAL_INT:
        case OP_ADD_STACK_CONST_INT:
        case OP_SUB_STACK_CONST_INT:
        case OP_MUL_STACK_CONST_INT:
        case OP_DIV_STACK_CONST_INT:
        case OP_MOD_STACK_CONST_INT:
        case OP_BIT_AND_STACK_CONST_INT:
        case OP_BIT_OR_STACK_CONST_INT:
        case OP_BIT_XOR_STACK_CONST_INT:
        case OP_ADD_STACK_CONST_DOUBLE:
        case OP_SUB_STACK_CONST_DOUBLE:
        case OP_MUL_STACK_CONST_DOUBLE:
        case OP_DIV_STACK_CONST_DOUBLE:
        case OP_ADD_STACK_LOCAL_DOUBLE:
        case OP_SUB_STACK_LOCAL_DOUBLE:
        case OP_MUL_STACK_LOCAL_DOUBLE:
        case OP_DIV_STACK_LOCAL_DOUBLE:
        case OP_ARRAY_COPY_LOCAL_LOCAL:
        case OP_ARRAY_REVERSE_PREFIX_LOCAL_LOCAL:
        case OP_ARRAY_ROTATE_PREFIX_LEFT_LOCAL_LOCAL:
        case OP_ARRAY_ROTATE_PREFIX_RIGHT_LOCAL_LOCAL:
        case OP_CONST16:
        case OP_LOAD_GLOBAL16:
        case OP_STORE_GLOBAL16:
        case OP_EVALA_RECIP_LOCALS_DOUBLE:
            return 3;
        case OP_CONST:
        case OP_LOAD_LOCAL:
        case OP_STORE_LOCAL:
        case OP_NEGATE_LOCAL:
        case OP_LOAD_GLOBAL:
        case OP_STORE_GLOBAL:
        case OP_CALL:
        case OP_MAKE_CLOSURE:
        case OP_ARRAY_NEW:
        case OP_ARRAY_GET_LOCAL:
        case OP_ARRAY_SET_LOCAL:
        case OP_RECORD_NEW:
        case OP_RECORD_SET_FIELD:
        case OP_RECORD_GET_FIELD:
        case OP_TUPLE_NEW:
        case OP_TUPLE_GET:
        case OP_TUPLE_SET:
        case OP_DEFER:
        case OP_ADD_LOCAL_STACK_INT:
        case OP_SUB_LOCAL_STACK_INT:
        case OP_ADD_LOCAL_STACK_DOUBLE:
        case OP_SUB_LOCAL_STACK_DOUBLE:
            return 2;
        default:
            return 1;
    }
}

static void peephole_remove_bytes(Chunk* chunk, int start, int count) {
    if (!chunk || count <= 0) return;
    if (start < 0 || start + count > chunk->code_count) return;

    for (int j = start; j < chunk->code_count - count; j++) {
        chunk->code[j] = chunk->code[j + count];
        if (chunk->debug_info) {
            chunk->debug_info[j] = chunk->debug_info[j + count];
        }
    }

    chunk->code_count -= count;
    if (chunk->debug_info) {
        chunk->debug_info = (DebugInfo*)safe_realloc(chunk->debug_info, chunk->code_count * sizeof(DebugInfo));
    }
}

void peephole_optimize(Chunk* chunk) {
    if (!chunk || chunk->code_count < 2) return;

    // Important: This optimizer must be instruction-aware. Operands can be 0x00,
    // which equals OP_NOP, so scanning byte-by-byte will corrupt bytecode.
    bool changed = true;
    int iterations = 0;
    const int MAX_ITERATIONS = 10;

    while (changed && iterations < MAX_ITERATIONS) {
        changed = false;
        iterations++;

        int pc = 0;
        while (pc < chunk->code_count) {
            uint8_t op = chunk->code[pc];
            int len = peephole_instruction_len(op);
            if (pc + len > chunk->code_count) break;

            // Pattern 1: Remove explicit NOP instructions.
            if (op == OP_NOP) {
                peephole_remove_bytes(chunk, pc, 1);
                changed = true;
                continue;
            }

            // Pattern 2: OP_LOAD_LOCAL slot; OP_STORE_LOCAL slot  => remove both.
            if (op == OP_LOAD_LOCAL && pc + 4 <= chunk->code_count) {
                int next_pc = pc + 2;
                if (next_pc + 2 <= chunk->code_count && chunk->code[next_pc] == OP_STORE_LOCAL) {
                    uint8_t slot1 = chunk->code[pc + 1];
                    uint8_t slot2 = chunk->code[next_pc + 1];
                    if (slot1 == slot2) {
                        peephole_remove_bytes(chunk, pc, 4);
                        changed = true;
                        continue;
                    }
                }
            }

            pc += len;
        }
    }
}

static void superinstruction_optimize(Chunk* chunk) {
    if (!chunk || chunk->code_count < 3) return;

    bool changed = true;
    int iterations = 0;
    const int MAX_ITERATIONS = 3;

    while (changed && iterations < MAX_ITERATIONS) {
        changed = false;
        iterations++;

        int pc = 0;
        while (pc < chunk->code_count) {
            uint8_t op = chunk->code[pc];

            // Pattern J: OP_JUMP_IF_FALSE off; OP_POP => OP_JUMP_IF_FALSE_POP off (4 bytes total).
            // This keeps jump offset encoding and preserves the original false-path POP at the jump target.
            if (op == OP_JUMP_IF_FALSE &&
                pc + 4 <= chunk->code_count &&
                chunk->code[pc + 3] == OP_POP) {
                uint8_t hi = chunk->code[pc + 1];
                uint8_t lo = chunk->code[pc + 2];
                chunk->code[pc] = OP_JUMP_IF_FALSE_POP;
                chunk->code[pc + 1] = hi;
                chunk->code[pc + 2] = lo;
                chunk->code[pc + 3] = 0;
                changed = true;
                pc += 4;
                continue;
            }

            // Global binary op fusion:
            // OP_LOAD_GLOBAL a; OP_LOAD_GLOBAL b; {OP_ADD,OP_SUB,OP_MUL,OP_DIV}; OP_STORE_GLOBAL dst
            // => OP_*_GLOBAL_GLOBAL_TO_GLOBAL dst a b (7 bytes total).
            if (op == OP_LOAD_GLOBAL &&
                pc + 7 <= chunk->code_count &&
                chunk->code[pc + 2] == OP_LOAD_GLOBAL &&
                chunk->code[pc + 5] == OP_STORE_GLOBAL) {

                uint8_t a_name_idx = chunk->code[pc + 1];
                uint8_t b_name_idx = chunk->code[pc + 3];
                uint8_t op3 = chunk->code[pc + 4];
                uint8_t dst_name_idx = chunk->code[pc + 6];

                uint8_t replacement = 0;
                switch (op3) {
                    case OP_ADD: replacement = OP_ADD_GLOBAL_GLOBAL_TO_GLOBAL; break;
                    case OP_SUB: replacement = OP_SUB_GLOBAL_GLOBAL_TO_GLOBAL; break;
                    case OP_MUL: replacement = OP_MUL_GLOBAL_GLOBAL_TO_GLOBAL; break;
                    case OP_DIV: replacement = OP_DIV_GLOBAL_GLOBAL_TO_GLOBAL; break;
                    default: break;
                }

                if (replacement != 0) {
                    chunk->code[pc] = replacement;
                    chunk->code[pc + 1] = dst_name_idx;
                    chunk->code[pc + 2] = a_name_idx;
                    chunk->code[pc + 3] = b_name_idx;
                    chunk->code[pc + 4] = 0;
                    chunk->code[pc + 5] = 0;
                    chunk->code[pc + 6] = 0;
                    changed = true;
                    pc += 7;
                    continue;
                }
            }

            // Pattern K: OP_ARRAY_GET_LOCAL_{CONST,LOCAL}_{INT,DOUBLE} ...; OP_STORE_LOCAL dst
            // => OP_ARRAY_GET_LOCAL_{CONST,LOCAL}_{INT,DOUBLE}_TO_LOCAL ... dst (5 bytes total).
            if ((op == OP_ARRAY_GET_LOCAL_LOCAL_INT ||
                 op == OP_ARRAY_GET_LOCAL_CONST_INT ||
                 op == OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE ||
                 op == OP_ARRAY_GET_LOCAL_CONST_DOUBLE) &&
                pc + 5 <= chunk->code_count &&
                chunk->code[pc + 3] == OP_STORE_LOCAL) {

                uint8_t array_slot = chunk->code[pc + 1];
                uint8_t idx_or_slot = chunk->code[pc + 2];
                uint8_t dst_slot = chunk->code[pc + 4];

                uint8_t replacement = 0;
                if (op == OP_ARRAY_GET_LOCAL_LOCAL_INT) replacement = OP_ARRAY_GET_LOCAL_LOCAL_INT_TO_LOCAL;
                if (op == OP_ARRAY_GET_LOCAL_CONST_INT) replacement = OP_ARRAY_GET_LOCAL_CONST_INT_TO_LOCAL;
                if (op == OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE) replacement = OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE_TO_LOCAL;
                if (op == OP_ARRAY_GET_LOCAL_CONST_DOUBLE) replacement = OP_ARRAY_GET_LOCAL_CONST_DOUBLE_TO_LOCAL;

                if (replacement != 0) {
                    chunk->code[pc] = replacement;
                    chunk->code[pc + 1] = array_slot;
                    chunk->code[pc + 2] = idx_or_slot;
                    chunk->code[pc + 3] = dst_slot;
                    chunk->code[pc + 4] = 0;
                    changed = true;
                    pc += 5;
                    continue;
                }
            }

            // Pattern M: OP_MUL_LOCALS_{INT,DOUBLE} a b; OP_STORE_LOCAL dst
            // => OP_MUL_LOCALS_{INT,DOUBLE}_TO_LOCAL dst a b (7 bytes total).
            if ((op == OP_MUL_LOCALS_INT || op == OP_MUL_LOCALS_DOUBLE) &&
                pc + 7 <= chunk->code_count &&
                chunk->code[pc + 5] == OP_STORE_LOCAL) {

                uint8_t a_slot = chunk->code[pc + 1];
                uint8_t b_slot = chunk->code[pc + 2];
                uint8_t dst_slot = chunk->code[pc + 6];

                chunk->code[pc] = (op == OP_MUL_LOCALS_INT) ? OP_MUL_LOCALS_INT_TO_LOCAL : OP_MUL_LOCALS_DOUBLE_TO_LOCAL;
                chunk->code[pc + 1] = dst_slot;
                chunk->code[pc + 2] = a_slot;
                chunk->code[pc + 3] = b_slot;
                chunk->code[pc + 4] = 0;
                chunk->code[pc + 5] = 0;
                chunk->code[pc + 6] = 0;
                changed = true;
                pc += 7;
                continue;
            }

            // Spectral norm fusion: evalA reciprocal followed by MADD.
            // OP_EVALA_RECIP_LOCALS_DOUBLE a b; OP_MADD_LOCAL_ARRAY_LOCAL_DOUBLE acc arr idx
            // => OP_EVALA_MADD_LOCAL_ARRAY_LOCAL_DOUBLE acc arr idx a b (7 bytes total).
            if (op == OP_EVALA_RECIP_LOCALS_DOUBLE &&
                pc + 7 <= chunk->code_count &&
                chunk->code[pc + 3] == OP_MADD_LOCAL_ARRAY_LOCAL_DOUBLE) {
                uint8_t a_slot = chunk->code[pc + 1];
                uint8_t b_slot = chunk->code[pc + 2];
                uint8_t acc_slot = chunk->code[pc + 4];
                uint8_t arr_slot = chunk->code[pc + 5];
                uint8_t idx_slot = chunk->code[pc + 6];

                chunk->code[pc] = OP_EVALA_MADD_LOCAL_ARRAY_LOCAL_DOUBLE;
                chunk->code[pc + 1] = acc_slot;
                chunk->code[pc + 2] = arr_slot;
                chunk->code[pc + 3] = idx_slot;
                chunk->code[pc + 4] = a_slot;
                chunk->code[pc + 5] = b_slot;
                chunk->code[pc + 6] = 0;
                changed = true;
                pc += 7;
                continue;
            }

            // Pattern L: OP_ADD_LOCAL_CONST a c1; OP_ADD_LOCAL_CONST b c2
            // Replace with a single length-preserving superinstruction (6 bytes total).
            if (op == OP_ADD_LOCAL_CONST &&
                pc + 6 <= chunk->code_count &&
                chunk->code[pc + 3] == OP_ADD_LOCAL_CONST) {
                uint8_t a_slot = chunk->code[pc + 1];
                uint8_t c1 = chunk->code[pc + 2];
                uint8_t b_slot = chunk->code[pc + 4];
                uint8_t c2 = chunk->code[pc + 5];

                chunk->code[pc] = OP_ADD2_LOCAL_CONST;
                chunk->code[pc + 1] = a_slot;
                chunk->code[pc + 2] = c1;
                chunk->code[pc + 3] = b_slot;
                chunk->code[pc + 4] = c2;
                chunk->code[pc + 5] = 0;
                changed = true;
                pc += 6;
                continue;
            }

            // Pattern A: OP_LOAD_LOCAL a; OP_LOAD_LOCAL b; <typed numeric op>
            // Replace with a single length-preserving superinstruction (5 bytes total).
            if (op == OP_LOAD_LOCAL && pc + 5 <= chunk->code_count && chunk->code[pc + 2] == OP_LOAD_LOCAL) {
                uint8_t a_slot = chunk->code[pc + 1];
                uint8_t b_slot = chunk->code[pc + 3];
                uint8_t op3 = chunk->code[pc + 4];

                uint8_t replacement = 0;
                switch (op3) {
                    case OP_ADD_INT: replacement = OP_ADD_LOCALS_INT; break;
                    case OP_SUB_INT: replacement = OP_SUB_LOCALS_INT; break;
                    case OP_MUL_INT: replacement = OP_MUL_LOCALS_INT; break;
                    case OP_DIV_INT: replacement = OP_DIV_LOCALS_INT; break;
                    case OP_MOD_INT: replacement = OP_MOD_LOCALS_INT; break;
                    case OP_BIT_AND_INT: replacement = OP_BIT_AND_LOCALS_INT; break;
                    case OP_BIT_OR_INT: replacement = OP_BIT_OR_LOCALS_INT; break;
                    case OP_BIT_XOR_INT: replacement = OP_BIT_XOR_LOCALS_INT; break;
                    case OP_ADD_DOUBLE: replacement = OP_ADD_LOCALS_DOUBLE; break;
                    case OP_SUB_DOUBLE: replacement = OP_SUB_LOCALS_DOUBLE; break;
                    case OP_MUL_DOUBLE: replacement = OP_MUL_LOCALS_DOUBLE; break;
                    case OP_DIV_DOUBLE: replacement = OP_DIV_LOCALS_DOUBLE; break;
                    default: break;
                }

                if (replacement != 0) {
                    chunk->code[pc] = replacement;
                    chunk->code[pc + 1] = a_slot;
                    chunk->code[pc + 2] = b_slot;
                    chunk->code[pc + 3] = 0;
                    chunk->code[pc + 4] = 0;
                    changed = true;
                    pc += 5;
                    continue;
                }
            }

            // Pattern A2: OP_LOAD_LOCAL a; OP_CONST c; <typed numeric op>
            // Replace with a single length-preserving superinstruction (5 bytes total).
            if (op == OP_LOAD_LOCAL && pc + 5 <= chunk->code_count && chunk->code[pc + 2] == OP_CONST) {
                uint8_t a_slot = chunk->code[pc + 1];
                uint8_t const_idx = chunk->code[pc + 3];
                uint8_t op3 = chunk->code[pc + 4];

                uint8_t replacement = 0;
                switch (op3) {
                    case OP_ADD_INT: replacement = OP_ADD_LOCAL_CONST_INT; break;
                    case OP_SUB_INT: replacement = OP_SUB_LOCAL_CONST_INT; break;
                    case OP_MUL_INT: replacement = OP_MUL_LOCAL_CONST_INT; break;
                    case OP_DIV_INT: replacement = OP_DIV_LOCAL_CONST_INT; break;
                    case OP_MOD_INT: replacement = OP_MOD_LOCAL_CONST_INT; break;
                    case OP_BIT_AND_INT: replacement = OP_BIT_AND_LOCAL_CONST_INT; break;
                    case OP_BIT_OR_INT: replacement = OP_BIT_OR_LOCAL_CONST_INT; break;
                    case OP_BIT_XOR_INT: replacement = OP_BIT_XOR_LOCAL_CONST_INT; break;
                    case OP_ADD_DOUBLE: replacement = OP_ADD_LOCAL_CONST_DOUBLE; break;
                    case OP_SUB_DOUBLE: replacement = OP_SUB_LOCAL_CONST_DOUBLE; break;
                    case OP_MUL_DOUBLE: replacement = OP_MUL_LOCAL_CONST_DOUBLE; break;
                    case OP_DIV_DOUBLE: replacement = OP_DIV_LOCAL_CONST_DOUBLE; break;
                    default: break;
                }

                if (replacement != 0) {
                    chunk->code[pc] = replacement;
                    chunk->code[pc + 1] = a_slot;
                    chunk->code[pc + 2] = const_idx;
                    chunk->code[pc + 3] = 0;
                    chunk->code[pc + 4] = 0;
                    changed = true;
                    pc += 5;
                    continue;
                }
            }

            // Pattern B: OP_LOAD_LOCAL x; OP_SQRT => OP_SQRT_LOCAL_DOUBLE x (3 bytes total).
            if (op == OP_LOAD_LOCAL && pc + 3 <= chunk->code_count && chunk->code[pc + 2] == OP_SQRT) {
                uint8_t x_slot = chunk->code[pc + 1];
                chunk->code[pc] = OP_SQRT_LOCAL_DOUBLE;
                chunk->code[pc + 1] = x_slot;
                chunk->code[pc + 2] = 0;
                changed = true;
                pc += 3;
                continue;
            }

            // Pattern C: OP_LOAD_LOCAL arr; OP_ARRAY_LEN => OP_ARRAY_LEN_LOCAL arr (3 bytes total).
            if (op == OP_LOAD_LOCAL && pc + 3 <= chunk->code_count && chunk->code[pc + 2] == OP_ARRAY_LEN) {
                uint8_t arr_slot = chunk->code[pc + 1];
                chunk->code[pc] = OP_ARRAY_LEN_LOCAL;
                chunk->code[pc + 1] = arr_slot;
                chunk->code[pc + 2] = 0;
                changed = true;
                pc += 3;
                continue;
            }

            // Pattern D: OP_LOAD_LOCAL b; <typed numeric op>  => <stack+local numeric op> (3 bytes total).
            // Replaces the LOAD_LOCAL with an immediate local operand.
            if (op == OP_LOAD_LOCAL && pc + 3 <= chunk->code_count) {
                uint8_t b_slot = chunk->code[pc + 1];
                uint8_t op2 = chunk->code[pc + 2];

                uint8_t replacement = 0;
                switch (op2) {
                    case OP_ADD_INT: replacement = OP_ADD_STACK_LOCAL_INT; break;
                    case OP_SUB_INT: replacement = OP_SUB_STACK_LOCAL_INT; break;
                    case OP_MUL_INT: replacement = OP_MUL_STACK_LOCAL_INT; break;
                    case OP_DIV_INT: replacement = OP_DIV_STACK_LOCAL_INT; break;
                    case OP_MOD_INT: replacement = OP_MOD_STACK_LOCAL_INT; break;
                    case OP_BIT_AND_INT: replacement = OP_BIT_AND_STACK_LOCAL_INT; break;
                    case OP_BIT_OR_INT: replacement = OP_BIT_OR_STACK_LOCAL_INT; break;
                    case OP_BIT_XOR_INT: replacement = OP_BIT_XOR_STACK_LOCAL_INT; break;
                    case OP_ADD_DOUBLE: replacement = OP_ADD_STACK_LOCAL_DOUBLE; break;
                    case OP_SUB_DOUBLE: replacement = OP_SUB_STACK_LOCAL_DOUBLE; break;
                    case OP_MUL_DOUBLE: replacement = OP_MUL_STACK_LOCAL_DOUBLE; break;
                    case OP_DIV_DOUBLE: replacement = OP_DIV_STACK_LOCAL_DOUBLE; break;
                    default: break;
                }

                if (replacement != 0) {
                    chunk->code[pc] = replacement;
                    chunk->code[pc + 1] = b_slot;
                    chunk->code[pc + 2] = 0;
                    changed = true;
                    pc += 3;
                    continue;
                }
            }

            // Pattern E: OP_CONST c; <typed numeric op> => <stack+const op> (3 bytes total).
            if (op == OP_CONST && pc + 3 <= chunk->code_count) {
                uint8_t const_idx = chunk->code[pc + 1];
                uint8_t op2 = chunk->code[pc + 2];

                uint8_t replacement = 0;
                switch (op2) {
                    case OP_ADD_INT: replacement = OP_ADD_STACK_CONST_INT; break;
                    case OP_SUB_INT: replacement = OP_SUB_STACK_CONST_INT; break;
                    case OP_MUL_INT: replacement = OP_MUL_STACK_CONST_INT; break;
                    case OP_DIV_INT: replacement = OP_DIV_STACK_CONST_INT; break;
                    case OP_MOD_INT: replacement = OP_MOD_STACK_CONST_INT; break;
                    case OP_BIT_AND_INT: replacement = OP_BIT_AND_STACK_CONST_INT; break;
                    case OP_BIT_OR_INT: replacement = OP_BIT_OR_STACK_CONST_INT; break;
                    case OP_BIT_XOR_INT: replacement = OP_BIT_XOR_STACK_CONST_INT; break;
                    case OP_ADD_DOUBLE: replacement = OP_ADD_STACK_CONST_DOUBLE; break;
                    case OP_SUB_DOUBLE: replacement = OP_SUB_STACK_CONST_DOUBLE; break;
                    case OP_MUL_DOUBLE: replacement = OP_MUL_STACK_CONST_DOUBLE; break;
                    case OP_DIV_DOUBLE: replacement = OP_DIV_STACK_CONST_DOUBLE; break;
                    default: break;
                }

                if (replacement != 0) {
                    chunk->code[pc] = replacement;
                    chunk->code[pc + 1] = const_idx;
                    chunk->code[pc + 2] = 0;
                    changed = true;
                    pc += 3;
                    continue;
                }
            }

            // Pattern N: OP_CONST c; <stack+local commutative op> => <local+const op> (5 bytes total).
            // This removes the OP_CONST dispatch in expressions like (c * local[x]).
            if (op == OP_CONST && pc + 5 <= chunk->code_count) {
                uint8_t const_idx = chunk->code[pc + 1];
                uint8_t op2 = chunk->code[pc + 2];
                uint8_t b_slot = chunk->code[pc + 3];

                uint8_t replacement = 0;
                switch (op2) {
                    case OP_ADD_STACK_LOCAL_INT: replacement = OP_ADD_LOCAL_CONST_INT; break;
                    case OP_MUL_STACK_LOCAL_INT: replacement = OP_MUL_LOCAL_CONST_INT; break;
                    case OP_BIT_AND_STACK_LOCAL_INT: replacement = OP_BIT_AND_LOCAL_CONST_INT; break;
                    case OP_BIT_OR_STACK_LOCAL_INT: replacement = OP_BIT_OR_LOCAL_CONST_INT; break;
                    case OP_BIT_XOR_STACK_LOCAL_INT: replacement = OP_BIT_XOR_LOCAL_CONST_INT; break;
                    case OP_ADD_STACK_LOCAL_DOUBLE: replacement = OP_ADD_LOCAL_CONST_DOUBLE; break;
                    case OP_MUL_STACK_LOCAL_DOUBLE: replacement = OP_MUL_LOCAL_CONST_DOUBLE; break;
                    default: break;
                }

                if (replacement != 0) {
                    chunk->code[pc] = replacement;
                    chunk->code[pc + 1] = b_slot;
                    chunk->code[pc + 2] = const_idx;
                    chunk->code[pc + 3] = 0;
                    chunk->code[pc + 4] = 0;
                    changed = true;
                    pc += 5;
                    continue;
                }
            }


            int len = peephole_instruction_len(op);
            if (len <= 0) len = 1;
            pc += len;
        }
    }
}

void compiler_free(Compiler* comp) {
    if (comp->file) free(comp->file);
}
