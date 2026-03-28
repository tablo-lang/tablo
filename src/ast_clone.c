#include "ast.h"
#include "safe_alloc.h"
#include <string.h>

// Forward declarations
static Expr* expr_clone_internal(Expr* expr);
static Stmt* stmt_clone_internal(Stmt* stmt);

// Deep clone an expression
Expr* expr_clone(Expr* expr) {
    return expr_clone_internal(expr);
}

static Expr* expr_clone_internal(Expr* expr) {
    if (!expr) return NULL;
    
    Expr* clone = (Expr*)safe_malloc(sizeof(Expr));
    memcpy(clone, expr, sizeof(Expr));
    
    // Deep copy strings
    if (expr->file) {
        clone->file = safe_strdup(expr->file);
    }
    
    // Note: Don't clone types - they are shared with symbol tables
    // See Fix 8 in expr_free and stmt_free
    // The type pointer should be shared, not copied
    clone->type = expr->type;
    
    // Clone based on kind
    switch (expr->kind) {
        case EXPR_LITERAL:
            if (expr->type &&
                (expr->type->kind == TYPE_STRING || expr->type->kind == TYPE_BIGINT) &&
                expr->literal.as_string) {
                clone->literal.as_string = safe_strdup(expr->literal.as_string);
            }
            break;
            
        case EXPR_IDENTIFIER:
            clone->identifier = safe_strdup(expr->identifier);
            break;
            
        case EXPR_BINARY:
            clone->binary.left = expr_clone_internal(expr->binary.left);
            clone->binary.right = expr_clone_internal(expr->binary.right);
            break;
            
        case EXPR_UNARY:
            clone->unary.operand = expr_clone_internal(expr->unary.operand);
            break;
            
        case EXPR_CALL:
            clone->call.callee = expr_clone_internal(expr->call.callee);
            if (expr->call.arg_count > 0) {
                clone->call.args = (Expr**)safe_malloc(expr->call.arg_count * sizeof(Expr*));
                for (int i = 0; i < expr->call.arg_count; i++) {
                    clone->call.args[i] = expr_clone_internal(expr->call.args[i]);
                }
            }
            if (expr->call.type_arg_count > 0 && expr->call.type_args) {
                clone->call.type_args = (Type**)safe_malloc((size_t)expr->call.type_arg_count * sizeof(Type*));
                for (int i = 0; i < expr->call.type_arg_count; i++) {
                    // Share type nodes to match other AST clone behavior.
                    clone->call.type_args[i] = expr->call.type_args[i];
                }
            }
            break;

        case EXPR_FUNC_LITERAL:
            clone->func_literal.return_type = expr->func_literal.return_type;  // Share type
            clone->func_literal.param_count = expr->func_literal.param_count;
            if (expr->func_literal.param_count > 0) {
                clone->func_literal.params = (char**)safe_malloc(expr->func_literal.param_count * sizeof(char*));
                clone->func_literal.param_types = (Type**)safe_malloc(expr->func_literal.param_count * sizeof(Type*));
                for (int i = 0; i < expr->func_literal.param_count; i++) {
                    clone->func_literal.params[i] = safe_strdup(expr->func_literal.params[i]);
                    clone->func_literal.param_types[i] = expr->func_literal.param_types[i];  // Share type
                }
            } else {
                clone->func_literal.params = NULL;
                clone->func_literal.param_types = NULL;
            }
            clone->func_literal.capture_count = expr->func_literal.capture_count;
            if (expr->func_literal.capture_count > 0) {
                clone->func_literal.capture_names = (char**)safe_malloc((size_t)expr->func_literal.capture_count * sizeof(char*));
                for (int i = 0; i < expr->func_literal.capture_count; i++) {
                    clone->func_literal.capture_names[i] = safe_strdup(expr->func_literal.capture_names[i]);
                }
            } else {
                clone->func_literal.capture_names = NULL;
            }
            clone->func_literal.body = stmt_clone_internal(expr->func_literal.body);
            clone->func_literal.compiled_name = expr->func_literal.compiled_name
                ? safe_strdup(expr->func_literal.compiled_name)
                : NULL;
            clone->func_literal.is_async = expr->func_literal.is_async;
            break;
            
        case EXPR_INDEX:
            clone->index.array = expr_clone_internal(expr->index.array);
            clone->index.index = expr_clone_internal(expr->index.index);
            break;
            
        case EXPR_ARRAY_LITERAL:
            if (expr->array_literal.element_count > 0) {
                clone->array_literal.elements = (Expr**)safe_malloc(expr->array_literal.element_count * sizeof(Expr*));
                for (int i = 0; i < expr->array_literal.element_count; i++) {
                    clone->array_literal.elements[i] = expr_clone_internal(expr->array_literal.elements[i]);
                }
            }
            break;
            
        case EXPR_CAST:
            clone->cast.value = expr_clone_internal(expr->cast.value);
            clone->cast.target_type = expr->cast.target_type;  // Share type
            break;

        case EXPR_TRY:
            clone->try_expr.expr = expr_clone_internal(expr->try_expr.expr);
            break;

        case EXPR_AWAIT:
            clone->await_expr.expr = expr_clone_internal(expr->await_expr.expr);
            break;

        case EXPR_TYPE_TEST:
            clone->type_test.value = expr_clone_internal(expr->type_test.value);
            clone->type_test.target_type = expr->type_test.target_type;
            break;

        case EXPR_IF:
            clone->if_expr.condition = expr_clone_internal(expr->if_expr.condition);
            clone->if_expr.then_expr = expr_clone_internal(expr->if_expr.then_expr);
            clone->if_expr.else_expr = expr_clone_internal(expr->if_expr.else_expr);
            break;

        case EXPR_MATCH:
            clone->match_expr.subject = expr_clone_internal(expr->match_expr.subject);
            clone->match_expr.arm_count = expr->match_expr.arm_count;
            clone->match_expr.patterns = NULL;
            clone->match_expr.guards = NULL;
            clone->match_expr.values = NULL;
            clone->match_expr.arm_group_ids = NULL;
            clone->match_expr.payload_binding_names = NULL;
            clone->match_expr.payload_binding_counts = NULL;
            clone->match_expr.else_expr = expr_clone_internal(expr->match_expr.else_expr);
            if (expr->match_expr.arm_count > 0) {
                int arm_count = expr->match_expr.arm_count;
                clone->match_expr.patterns = (Expr**)safe_malloc((size_t)arm_count * sizeof(Expr*));
                clone->match_expr.values = (Expr**)safe_malloc((size_t)arm_count * sizeof(Expr*));
                if (expr->match_expr.guards) {
                    clone->match_expr.guards = (Expr**)safe_calloc((size_t)arm_count, sizeof(Expr*));
                }
                if (expr->match_expr.arm_group_ids) {
                    clone->match_expr.arm_group_ids =
                        (int*)safe_malloc((size_t)arm_count * sizeof(int));
                    memcpy(clone->match_expr.arm_group_ids,
                           expr->match_expr.arm_group_ids,
                           (size_t)arm_count * sizeof(int));
                }
                if (expr->match_expr.payload_binding_names) {
                    clone->match_expr.payload_binding_names =
                        (char***)safe_calloc((size_t)arm_count, sizeof(char**));
                    clone->match_expr.payload_binding_counts =
                        (int*)safe_calloc((size_t)arm_count, sizeof(int));
                }
                for (int i = 0; i < arm_count; i++) {
                    clone->match_expr.patterns[i] = expr_clone_internal(expr->match_expr.patterns[i]);
                    clone->match_expr.values[i] = expr_clone_internal(expr->match_expr.values[i]);
                    if (clone->match_expr.guards) {
                        clone->match_expr.guards[i] = expr_clone_internal(expr->match_expr.guards[i]);
                    }
                    int bind_count = (expr->match_expr.payload_binding_counts &&
                                      i < arm_count)
                                         ? expr->match_expr.payload_binding_counts[i]
                                         : 0;
                    if (clone->match_expr.payload_binding_counts) {
                        clone->match_expr.payload_binding_counts[i] = bind_count;
                    }
                    if (bind_count > 0 &&
                        clone->match_expr.payload_binding_names &&
                        expr->match_expr.payload_binding_names &&
                        expr->match_expr.payload_binding_names[i]) {
                        clone->match_expr.payload_binding_names[i] =
                            (char**)safe_calloc((size_t)bind_count, sizeof(char*));
                        for (int j = 0; j < bind_count; j++) {
                            const char* name = expr->match_expr.payload_binding_names[i][j];
                            if (name) clone->match_expr.payload_binding_names[i][j] = safe_strdup(name);
                        }
                    }
                }
            }
            break;

        case EXPR_BLOCK:
            clone->block_expr.stmt_count = expr->block_expr.stmt_count;
            clone->block_expr.statements = NULL;
            clone->block_expr.value = expr_clone_internal(expr->block_expr.value);
            if (expr->block_expr.stmt_count > 0) {
                clone->block_expr.statements =
                    (Stmt**)safe_malloc((size_t)expr->block_expr.stmt_count * sizeof(Stmt*));
                for (int i = 0; i < expr->block_expr.stmt_count; i++) {
                    clone->block_expr.statements[i] = stmt_clone_internal(expr->block_expr.statements[i]);
                }
            }
            break;
             
        case EXPR_RECORD_LITERAL:
            if (expr->record_literal.field_count > 0) {
                clone->record_literal.field_names = (char**)safe_malloc(expr->record_literal.field_count * sizeof(char*));
                clone->record_literal.field_values = (Expr**)safe_malloc(expr->record_literal.field_count * sizeof(Expr*));
                for (int i = 0; i < expr->record_literal.field_count; i++) {
                    clone->record_literal.field_names[i] = safe_strdup(expr->record_literal.field_names[i]);
                    clone->record_literal.field_values[i] = expr_clone_internal(expr->record_literal.field_values[i]);
                }
            }
            break;
            
        case EXPR_FIELD_ACCESS:
            clone->field_access.object = expr_clone_internal(expr->field_access.object);
            clone->field_access.field_name = safe_strdup(expr->field_access.field_name);
            break;
            
        case EXPR_TUPLE_LITERAL:
            if (expr->tuple_literal.element_count > 0) {
                clone->tuple_literal.elements = (Expr**)safe_malloc(expr->tuple_literal.element_count * sizeof(Expr*));
                for (int i = 0; i < expr->tuple_literal.element_count; i++) {
                    clone->tuple_literal.elements[i] = expr_clone_internal(expr->tuple_literal.elements[i]);
                }
            }
            break;
            
        case EXPR_TUPLE_ACCESS:
            clone->tuple_access.tuple = expr_clone_internal(expr->tuple_access.tuple);
            break;
            
        case EXPR_MAP_LITERAL:
            if (expr->map_literal.entry_count > 0) {
                clone->map_literal.keys = (Expr**)safe_malloc(expr->map_literal.entry_count * sizeof(Expr*));
                clone->map_literal.values = (Expr**)safe_malloc(expr->map_literal.entry_count * sizeof(Expr*));
                for (int i = 0; i < expr->map_literal.entry_count; i++) {
                    clone->map_literal.keys[i] = expr_clone_internal(expr->map_literal.keys[i]);
                    clone->map_literal.values[i] = expr_clone_internal(expr->map_literal.values[i]);
                }
            }
            break;
            
        case EXPR_SET_LITERAL:
            if (expr->set_literal.element_count > 0) {
                clone->set_literal.elements = (Expr**)safe_malloc(expr->set_literal.element_count * sizeof(Expr*));
                for (int i = 0; i < expr->set_literal.element_count; i++) {
                    clone->set_literal.elements[i] = expr_clone_internal(expr->set_literal.elements[i]);
                }
            }
            break;
            
        default:
            break;
    }
    
    return clone;
}

// Deep clone a statement
Stmt* stmt_clone(Stmt* stmt) {
    return stmt_clone_internal(stmt);
}

static Stmt* stmt_clone_internal(Stmt* stmt) {
    if (!stmt) return NULL;
    
    Stmt* clone = NULL;
    char* file_copy = stmt->file ? safe_strdup(stmt->file) : NULL;
    
    switch (stmt->kind) {
        case STMT_VAR_DECL:
            clone = stmt_create_var_decl(
                safe_strdup(stmt->var_decl.name),
                stmt->var_decl.type_annotation,  // Share type, don't clone
                expr_clone_internal(stmt->var_decl.initializer),
                stmt->var_decl.is_mutable,
                file_copy,
                stmt->line,
                stmt->column
            );
            break;

        case STMT_VAR_TUPLE_DECL: {
            int name_count = stmt->var_tuple_decl.name_count;
            char** names = NULL;
            if (name_count > 0) {
                names = (char**)safe_malloc(name_count * sizeof(char*));
                for (int i = 0; i < name_count; i++) {
                    names[i] = safe_strdup(stmt->var_tuple_decl.names[i]);
                }
            }
            clone = stmt_create_var_tuple_decl(
                names,
                name_count,
                stmt->var_tuple_decl.type_annotation,  // Share type, don't clone
                expr_clone_internal(stmt->var_tuple_decl.initializer),
                stmt->var_tuple_decl.is_mutable,
                file_copy,
                stmt->line,
                stmt->column
            );
            break;
        }
             
        case STMT_EXPR:
            clone = stmt_create_expr(
                expr_clone_internal(stmt->expr_stmt),
                file_copy,
                stmt->line,
                stmt->column
            );
            break;
            
        case STMT_ASSIGN:
            clone = stmt_create_assign(
                safe_strdup(stmt->assign.name),
                expr_clone_internal(stmt->assign.value),
                stmt->assign.op,
                file_copy,
                stmt->line,
                stmt->column
            );
            break;
            
        case STMT_ASSIGN_INDEX:
            clone = stmt_create_assign_index(
                expr_clone_internal(stmt->assign_index.target),
                expr_clone_internal(stmt->assign_index.index),
                expr_clone_internal(stmt->assign_index.value),
                stmt->assign_index.op,
                file_copy,
                stmt->line,
                stmt->column
            );
            break;
            
        case STMT_ASSIGN_FIELD:
            clone = stmt_create_assign_field(
                expr_clone_internal(stmt->assign_field.object),
                safe_strdup(stmt->assign_field.field_name),
                expr_clone_internal(stmt->assign_field.value),
                stmt->assign_field.op,
                file_copy,
                stmt->line,
                stmt->column
            );
            if (clone) {
                clone->assign_field.field_index = stmt->assign_field.field_index;
            }
            break;
            
        case STMT_BLOCK: {
            int count = stmt->block.stmt_count;
            Stmt** stmts = NULL;
            if (count > 0) {
                stmts = (Stmt**)safe_malloc(count * sizeof(Stmt*));
                for (int i = 0; i < count; i++) {
                    stmts[i] = stmt_clone_internal(stmt->block.statements[i]);
                }
            }
            clone = stmt_create_block(stmts, count, file_copy, stmt->line, stmt->column);
            break;
        }
            
        case STMT_IF:
            clone = stmt_create_if(
                expr_clone_internal(stmt->if_stmt.condition),
                stmt_clone_internal(stmt->if_stmt.then_branch),
                stmt_clone_internal(stmt->if_stmt.else_branch),
                file_copy,
                stmt->line,
                stmt->column
            );
            break;

        case STMT_MATCH: {
            int arm_count = stmt->match_stmt.arm_count;
            Expr** patterns = NULL;
            Expr** guards = NULL;
            Stmt** bodies = NULL;
            int* arm_group_ids = NULL;
            char*** payload_binding_names = NULL;
            int* payload_binding_counts = NULL;
            if (arm_count > 0) {
                patterns = (Expr**)safe_malloc((size_t)arm_count * sizeof(Expr*));
                if (stmt->match_stmt.guards) {
                    guards = (Expr**)safe_calloc((size_t)arm_count, sizeof(Expr*));
                }
                bodies = (Stmt**)safe_malloc((size_t)arm_count * sizeof(Stmt*));
                if (stmt->match_stmt.arm_group_ids) {
                    arm_group_ids = (int*)safe_malloc((size_t)arm_count * sizeof(int));
                    memcpy(arm_group_ids,
                           stmt->match_stmt.arm_group_ids,
                           (size_t)arm_count * sizeof(int));
                }
                payload_binding_names = (char***)safe_calloc((size_t)arm_count, sizeof(char**));
                payload_binding_counts = (int*)safe_calloc((size_t)arm_count, sizeof(int));
                for (int i = 0; i < arm_count; i++) {
                    patterns[i] = expr_clone_internal(stmt->match_stmt.patterns[i]);
                    if (guards) {
                        guards[i] = expr_clone_internal(stmt->match_stmt.guards[i]);
                    }
                    bodies[i] = stmt_clone_internal(stmt->match_stmt.bodies[i]);

                    int bind_count = (stmt->match_stmt.payload_binding_counts &&
                                      i < stmt->match_stmt.arm_count)
                                         ? stmt->match_stmt.payload_binding_counts[i]
                                         : 0;
                    payload_binding_counts[i] = bind_count;
                    if (bind_count > 0 && stmt->match_stmt.payload_binding_names &&
                        stmt->match_stmt.payload_binding_names[i]) {
                        payload_binding_names[i] = (char**)safe_calloc((size_t)bind_count, sizeof(char*));
                        for (int j = 0; j < bind_count; j++) {
                            const char* name = stmt->match_stmt.payload_binding_names[i][j];
                            if (name) payload_binding_names[i][j] = safe_strdup(name);
                        }
                    }
                }
            }
            clone = stmt_create_match(
                expr_clone_internal(stmt->match_stmt.subject),
                patterns,
                guards,
                bodies,
                arm_count,
                stmt_clone_internal(stmt->match_stmt.else_branch),
                file_copy,
                stmt->line,
                stmt->column
            );
            if (clone) {
                clone->match_stmt.arm_group_ids = arm_group_ids;
                clone->match_stmt.payload_binding_names = payload_binding_names;
                clone->match_stmt.payload_binding_counts = payload_binding_counts;
                arm_group_ids = NULL;
                payload_binding_names = NULL;
                payload_binding_counts = NULL;
            }

            if (arm_group_ids) free(arm_group_ids);
            if (payload_binding_names) {
                for (int i = 0; i < arm_count; i++) {
                    if (!payload_binding_names[i]) continue;
                    int bind_count = payload_binding_counts ? payload_binding_counts[i] : 0;
                    for (int j = 0; j < bind_count; j++) {
                        if (payload_binding_names[i][j]) free(payload_binding_names[i][j]);
                    }
                    free(payload_binding_names[i]);
                }
                free(payload_binding_names);
            }
            if (payload_binding_counts) free(payload_binding_counts);
            break;
        }
            
        case STMT_WHILE:
            clone = stmt_create_while(
                expr_clone_internal(stmt->while_stmt.condition),
                stmt_clone_internal(stmt->while_stmt.body),
                file_copy,
                stmt->line,
                stmt->column
            );
            break;
            
        case STMT_FOREACH:
            clone = stmt_create_foreach(
                safe_strdup(stmt->foreach.var_name),
                expr_clone_internal(stmt->foreach.iterable),
                stmt_clone_internal(stmt->foreach.body),
                file_copy,
                stmt->line,
                stmt->column
            );
            break;

        case STMT_FOR_RANGE:
            clone = stmt_create_for_range(
                safe_strdup(stmt->for_range.var_name),
                expr_clone_internal(stmt->for_range.start),
                expr_clone_internal(stmt->for_range.end),
                stmt_clone_internal(stmt->for_range.body),
                file_copy,
                stmt->line,
                stmt->column
            );
            break;
            
        case STMT_FUNC_DECL: {
            int type_param_count = stmt->func_decl.type_param_count;
            char** type_params = NULL;
            Type** type_param_constraints = NULL;
            if (type_param_count > 0) {
                type_params = (char**)safe_malloc((size_t)type_param_count * sizeof(char*));
                type_param_constraints = (Type**)safe_malloc((size_t)type_param_count * sizeof(Type*));
                for (int i = 0; i < type_param_count; i++) {
                    type_params[i] = safe_strdup(stmt->func_decl.type_params[i]);
                    type_param_constraints[i] = stmt->func_decl.type_param_constraints
                        ? stmt->func_decl.type_param_constraints[i]
                        : NULL;
                }
            }

            int param_count = stmt->func_decl.param_count;
            char** params = NULL;
            Type** param_types = NULL;
            if (param_count > 0) {
                params = (char**)safe_malloc(param_count * sizeof(char*));
                param_types = (Type**)safe_malloc(param_count * sizeof(Type*));
                for (int i = 0; i < param_count; i++) {
                    params[i] = safe_strdup(stmt->func_decl.params[i]);
                    param_types[i] = stmt->func_decl.param_types[i];  // Share type
                }
            }
            clone = stmt_create_func_decl(
                stmt->func_decl.return_type,  // Share return type
                safe_strdup(stmt->func_decl.name),
                type_params,
                type_param_constraints,
                type_param_count,
                params,
                param_types,
                param_count,
                stmt_clone_internal(stmt->func_decl.body),
                stmt->func_decl.is_async,
                file_copy,
                stmt->line,
                stmt->column
            );
            break;
        }
            
        case STMT_IMPORT:
            clone = stmt_create_import(
                safe_strdup(stmt->import_path),
                file_copy,
                stmt->line,
                stmt->column
            );
            break;
            
        case STMT_RETURN:
            clone = stmt_create_return(
                expr_clone_internal(stmt->return_value),
                file_copy,
                stmt->line,
                stmt->column
            );
            break;

        case STMT_DEFER:
            clone = stmt_create_defer(
                expr_clone_internal(stmt->defer_expr),
                file_copy,
                stmt->line,
                stmt->column
            );
            break;

        case STMT_BREAK:
            clone = stmt_create_break(file_copy, stmt->line, stmt->column);
            break;
            
        case STMT_CONTINUE:
            clone = stmt_create_continue(file_copy, stmt->line, stmt->column);
            break;
            
        case STMT_RECORD_DECL: {
            int type_param_count = stmt->record_decl.type_param_count;
            char** type_params = NULL;
            if (type_param_count > 0) {
                type_params = (char**)safe_malloc((size_t)type_param_count * sizeof(char*));
                for (int i = 0; i < type_param_count; i++) {
                    type_params[i] = safe_strdup(stmt->record_decl.type_params[i]);
                }
            }

            int field_count = stmt->record_decl.field_count;
            char** field_names = NULL;
            Type** field_types = NULL;
            if (field_count > 0) {
                field_names = (char**)safe_malloc(field_count * sizeof(char*));
                field_types = (Type**)safe_malloc(field_count * sizeof(Type*));
                for (int i = 0; i < field_count; i++) {
                    field_names[i] = safe_strdup(stmt->record_decl.field_names[i]);
                    field_types[i] = stmt->record_decl.field_types[i];  // Share type
                }
            }
            clone = stmt_create_record_decl(
                safe_strdup(stmt->record_decl.name),
                type_params,
                type_param_count,
                field_names,
                field_types,
                field_count,
                file_copy,
                stmt->line,
                stmt->column
            );
            break;
        }

        case STMT_INTERFACE_DECL: {
            int method_count = stmt->interface_decl.method_count;
            char** method_names = NULL;
            Type** method_types = NULL;
            if (method_count > 0) {
                method_names = (char**)safe_malloc((size_t)method_count * sizeof(char*));
                method_types = (Type**)safe_malloc((size_t)method_count * sizeof(Type*));
                for (int i = 0; i < method_count; i++) {
                    method_names[i] = safe_strdup(stmt->interface_decl.method_names[i]);
                    method_types[i] = stmt->interface_decl.method_types[i];  // Share type
                }
            }
            clone = stmt_create_interface_decl(
                safe_strdup(stmt->interface_decl.name),
                method_names,
                method_types,
                method_count,
                file_copy,
                stmt->line,
                stmt->column
            );
            break;
        }

        case STMT_IMPL_DECL: {
            int method_count = stmt->impl_decl.method_count;
            char** method_names = NULL;
            char** function_names = NULL;
            if (method_count > 0) {
                method_names = (char**)safe_malloc((size_t)method_count * sizeof(char*));
                function_names = (char**)safe_malloc((size_t)method_count * sizeof(char*));
                for (int i = 0; i < method_count; i++) {
                    method_names[i] = safe_strdup(stmt->impl_decl.method_names[i]);
                    function_names[i] = safe_strdup(stmt->impl_decl.function_names[i]);
                }
            }
            clone = stmt_create_impl_decl(
                safe_strdup(stmt->impl_decl.interface_name),
                safe_strdup(stmt->impl_decl.record_name),
                method_names,
                function_names,
                method_count,
                file_copy,
                stmt->line,
                stmt->column
            );
            break;
        }

        case STMT_TYPE_ALIAS:
            clone = stmt_create_type_alias(
                safe_strdup(stmt->type_alias.name),
                NULL,
                0,
                stmt->type_alias.target_type,  // Share type
                file_copy,
                stmt->line,
                stmt->column
            );
            if (stmt->type_alias.type_param_count > 0) {
                clone->type_alias.type_params =
                    (char**)safe_malloc((size_t)stmt->type_alias.type_param_count * sizeof(char*));
                clone->type_alias.type_param_count = stmt->type_alias.type_param_count;
                for (int i = 0; i < stmt->type_alias.type_param_count; i++) {
                    clone->type_alias.type_params[i] = safe_strdup(stmt->type_alias.type_params[i]);
                }
            }
            break;

        case STMT_ENUM_DECL: {
            int type_param_count = stmt->enum_decl.type_param_count;
            char** type_params = NULL;
            if (type_param_count > 0) {
                type_params = (char**)safe_malloc((size_t)type_param_count * sizeof(char*));
                for (int i = 0; i < type_param_count; i++) {
                    const char* param_name = (stmt->enum_decl.type_params && stmt->enum_decl.type_params[i])
                        ? stmt->enum_decl.type_params[i]
                        : "";
                    type_params[i] = safe_strdup(param_name);
                }
            }

            int member_count = stmt->enum_decl.member_count;
            char** member_names = NULL;
            int64_t* member_values = NULL;
            Type*** member_payload_types = NULL;
            int* member_payload_counts = NULL;
            if (member_count > 0) {
                member_names = (char**)safe_malloc((size_t)member_count * sizeof(char*));
                member_values = (int64_t*)safe_malloc((size_t)member_count * sizeof(int64_t));
                member_payload_types = (Type***)safe_malloc((size_t)member_count * sizeof(Type**));
                member_payload_counts = (int*)safe_malloc((size_t)member_count * sizeof(int));
                for (int i = 0; i < member_count; i++) {
                    member_names[i] = safe_strdup(stmt->enum_decl.member_names[i]);
                    member_values[i] = stmt->enum_decl.member_values[i];
                    int payload_count = (stmt->enum_decl.member_payload_counts &&
                                         i < stmt->enum_decl.member_count)
                                            ? stmt->enum_decl.member_payload_counts[i]
                                            : 0;
                    member_payload_counts[i] = payload_count;
                    if (payload_count > 0 &&
                        stmt->enum_decl.member_payload_types &&
                        stmt->enum_decl.member_payload_types[i]) {
                        member_payload_types[i] = (Type**)safe_malloc((size_t)payload_count * sizeof(Type*));
                        for (int j = 0; j < payload_count; j++) {
                            // Share type nodes to match other AST clone behavior.
                            member_payload_types[i][j] = stmt->enum_decl.member_payload_types[i][j];
                        }
                    } else {
                        member_payload_types[i] = NULL;
                    }
                }
            }
            clone = stmt_create_enum_decl(
                safe_strdup(stmt->enum_decl.name),
                type_params,
                type_param_count,
                member_names,
                member_values,
                member_payload_types,
                member_payload_counts,
                member_count,
                stmt->enum_decl.has_payload_members,
                file_copy,
                stmt->line,
                stmt->column
            );
            break;
        }
            
        default:
            // Should not happen
            free(file_copy);
            return NULL;
    }
    
    if (clone) {
        clone->is_public = stmt->is_public;
    }
    return clone;
}
