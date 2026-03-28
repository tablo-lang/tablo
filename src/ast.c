#include "ast.h"
#include "safe_alloc.h"
#include <stdlib.h>
#include <string.h>

Expr* expr_create_literal_int(int64_t value, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_LITERAL;
    expr->literal.as_int = value;
    expr->type = type_int();
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_literal_bool(bool value, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_LITERAL;
    expr->literal.as_int = value ? 1 : 0;
    expr->type = type_bool();
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_literal_double(double value, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_LITERAL;
    expr->literal.as_double = value;
    expr->type = type_double();
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_literal_bigint(const char* value, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_LITERAL;
    expr->literal.as_string = value ? safe_strdup(value) : NULL;
    expr->type = type_bigint();
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_literal_string(char* value, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_LITERAL;
    expr->literal.as_string = value ? safe_strdup(value) : NULL;
    expr->type = type_string();
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_nil(char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_NIL;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_identifier(char* name, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_IDENTIFIER;
    expr->identifier = name ? safe_strdup(name) : NULL;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_binary(TokenType op, Expr* left, Expr* right, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_BINARY;
    expr->binary.op = op;
    expr->binary.left = left;
    expr->binary.right = right;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_unary(TokenType op, Expr* operand, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_UNARY;
    expr->unary.op = op;
    expr->unary.operand = operand;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_call(Expr* callee, Expr** args, int arg_count, Type** type_args, int type_arg_count, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_CALL;
    expr->call.callee = callee;
    expr->call.args = args;
    expr->call.arg_count = arg_count;
    expr->call.type_args = type_args;
    expr->call.type_arg_count = type_arg_count;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_func_literal(Type* return_type, char** params, Type** param_types, int param_count, Stmt* body, bool is_async, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_FUNC_LITERAL;
    expr->func_literal.return_type = return_type;
    expr->func_literal.params = params;
    expr->func_literal.param_types = param_types;
    expr->func_literal.param_count = param_count;
    expr->func_literal.capture_names = NULL;
    expr->func_literal.capture_count = 0;
    expr->func_literal.body = body;
    expr->func_literal.compiled_name = NULL;
    expr->func_literal.is_async = is_async;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_array(Expr* array, Expr* index, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_INDEX;
    expr->index.array = array;
    expr->index.index = index;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_array_literal(Expr** elements, int element_count, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_ARRAY_LITERAL;
    expr->array_literal.elements = elements;
    expr->array_literal.element_count = element_count;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_cast(Expr* value, Type* target_type, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_CAST;
    expr->cast.value = value;
    expr->cast.target_type = target_type;
    expr->type = target_type;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_try(Expr* operand, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_TRY;
    expr->try_expr.expr = operand;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_await(Expr* operand, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_AWAIT;
    expr->await_expr.expr = operand;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_type_test(Expr* value, Type* target_type, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_TYPE_TEST;
    expr->type_test.value = value;
    expr->type_test.target_type = target_type;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_if(Expr* condition,
                     Expr* then_expr,
                     Expr* else_expr,
                     char* file,
                     int line,
                     int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_IF;
    expr->if_expr.condition = condition;
    expr->if_expr.then_expr = then_expr;
    expr->if_expr.else_expr = else_expr;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_match(Expr* subject,
                        Expr** patterns,
                        Expr** guards,
                        Expr** values,
                        int arm_count,
                        Expr* else_expr,
                        char* file,
                        int line,
                        int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_MATCH;
    expr->match_expr.subject = subject;
    expr->match_expr.patterns = patterns;
    expr->match_expr.guards = guards;
    expr->match_expr.values = values;
    expr->match_expr.arm_group_ids = NULL;
    expr->match_expr.payload_binding_names = NULL;
    expr->match_expr.payload_binding_counts = NULL;
    expr->match_expr.arm_count = arm_count;
    expr->match_expr.else_expr = else_expr;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_block(Stmt** statements,
                        int stmt_count,
                        Expr* value,
                        char* file,
                        int line,
                        int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_BLOCK;
    expr->block_expr.statements = statements;
    expr->block_expr.stmt_count = stmt_count;
    expr->block_expr.value = value;
    expr->type = NULL;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Stmt* stmt_create_var_decl(char* name, Type* type, Expr* init, bool is_mutable, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_VAR_DECL;
    stmt->is_public = true;
    stmt->var_decl.name = name ? safe_strdup(name) : NULL;
    stmt->var_decl.type_annotation = type;
    stmt->var_decl.initializer = init;
    stmt->var_decl.is_mutable = is_mutable;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_var_tuple_decl(char** names, int name_count, Type* type, Expr* init, bool is_mutable, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_VAR_TUPLE_DECL;
    stmt->is_public = true;
    stmt->var_tuple_decl.names = names;
    stmt->var_tuple_decl.name_count = name_count;
    stmt->var_tuple_decl.type_annotation = type;
    stmt->var_tuple_decl.initializer = init;
    stmt->var_tuple_decl.is_mutable = is_mutable;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_expr(Expr* expr, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_EXPR;
    stmt->is_public = true;
    stmt->expr_stmt = expr;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_assign(char* name, Expr* value, TokenType op, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_ASSIGN;
    stmt->is_public = true;
    stmt->assign.name = name ? safe_strdup(name) : NULL;
    stmt->assign.value = value;
    stmt->assign.op = op;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_assign_index(Expr* target, Expr* index, Expr* value, TokenType op, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_ASSIGN_INDEX;
    stmt->is_public = true;
    stmt->assign_index.target = target;
    stmt->assign_index.index = index;
    stmt->assign_index.value = value;
    stmt->assign_index.op = op;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_assign_field(Expr* object, char* field_name, Expr* value, TokenType op, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_ASSIGN_FIELD;
    stmt->is_public = true;
    stmt->assign_field.object = object;
    stmt->assign_field.field_name = field_name ? safe_strdup(field_name) : NULL;
    stmt->assign_field.value = value;
    stmt->assign_field.op = op;
    stmt->assign_field.field_index = -1;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_block(Stmt** stmts, int stmt_count, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_BLOCK;
    stmt->is_public = true;
    stmt->block.statements = stmts;
    stmt->block.stmt_count = stmt_count;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_if(Expr* cond, Stmt* then_branch, Stmt* else_branch, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_IF;
    stmt->is_public = true;
    stmt->if_stmt.condition = cond;
    stmt->if_stmt.then_branch = then_branch;
    stmt->if_stmt.else_branch = else_branch;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_match(Expr* subject,
                        Expr** patterns,
                        Expr** guards,
                        Stmt** bodies,
                        int arm_count,
                        Stmt* else_branch,
                        char* file,
                        int line,
                        int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_MATCH;
    stmt->is_public = true;
    stmt->match_stmt.subject = subject;
    stmt->match_stmt.patterns = patterns;
    stmt->match_stmt.guards = guards;
    stmt->match_stmt.bodies = bodies;
    stmt->match_stmt.arm_group_ids = NULL;
    stmt->match_stmt.payload_binding_names = NULL;
    stmt->match_stmt.payload_binding_counts = NULL;
    stmt->match_stmt.arm_count = arm_count;
    stmt->match_stmt.else_branch = else_branch;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_while(Expr* cond, Stmt* body, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_WHILE;
    stmt->is_public = true;
    stmt->while_stmt.condition = cond;
    stmt->while_stmt.body = body;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_foreach(char* var_name, Expr* iterable, Stmt* body, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_FOREACH;
    stmt->is_public = true;
    stmt->foreach.var_name = var_name ? safe_strdup(var_name) : NULL;
    stmt->foreach.iterable = iterable;
    stmt->foreach.body = body;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_for_range(char* var_name, Expr* start, Expr* end, Stmt* body, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_FOR_RANGE;
    stmt->is_public = true;
    stmt->for_range.var_name = var_name ? safe_strdup(var_name) : NULL;
    stmt->for_range.start = start;
    stmt->for_range.end = end;
    stmt->for_range.body = body;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_break(char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_BREAK;
    stmt->is_public = true;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_continue(char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_CONTINUE;
    stmt->is_public = true;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_return(Expr* value, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_RETURN;
    stmt->is_public = true;
    stmt->return_value = value;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_defer(Expr* expr, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_DEFER;
    stmt->is_public = true;
    stmt->defer_expr = expr;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_func_decl(Type* return_type,
                            char* name,
                            char** type_params,
                            Type** type_param_constraints,
                            int type_param_count,
                            char** params,
                            Type** param_types,
                            int param_count,
                            Stmt* body,
                            bool is_async,
                            char* file,
                            int line,
                            int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_FUNC_DECL;
    stmt->is_public = true;
    stmt->func_decl.return_type = return_type;
    stmt->func_decl.name = name ? safe_strdup(name) : NULL;
    stmt->func_decl.type_params = type_params;
    stmt->func_decl.type_param_constraints = type_param_constraints;
    stmt->func_decl.type_param_count = type_param_count;
    stmt->func_decl.params = params;
    stmt->func_decl.param_types = param_types;
    stmt->func_decl.param_count = param_count;
    stmt->func_decl.body = body;
    stmt->func_decl.is_async = is_async;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_import(char* path, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_IMPORT;
    stmt->is_public = true;
    stmt->import_path = path ? safe_strdup(path) : NULL;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Expr* expr_create_record_literal(char** field_names, Expr** field_values, int field_count, Type* record_type, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_RECORD_LITERAL;
    expr->record_literal.field_names = field_names;
    expr->record_literal.field_values = field_values;
    expr->record_literal.field_count = field_count;
    expr->record_literal.record_type = record_type;
    expr->record_literal.pattern_type = NULL;
    expr->record_literal.is_pattern = false;
    expr->record_literal.allows_rest = false;
    expr->type = record_type;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_field_access(Expr* object, char* field_name, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_FIELD_ACCESS;
    expr->field_access.object = object;
    expr->field_access.field_name = field_name ? safe_strdup(field_name) : NULL;
    expr->field_access.field_index = -1;
    expr->type = NULL;  // Will be set by type checker
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_tuple_literal(Expr** elements, int element_count, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_TUPLE_LITERAL;
    expr->tuple_literal.elements = elements;
    expr->tuple_literal.element_count = element_count;
    expr->type = NULL;  // Will be set by type checker
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_tuple_access(Expr* tuple, int index, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_TUPLE_ACCESS;
    expr->tuple_access.tuple = tuple;
    expr->tuple_access.index = index;
    expr->type = NULL;  // Will be set by type checker
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_map_literal(Expr** keys, Expr** values, int entry_count, Type* map_type, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_MAP_LITERAL;
    expr->map_literal.keys = keys;
    expr->map_literal.values = values;
    expr->map_literal.entry_count = entry_count;
    expr->map_literal.map_type = map_type;
    expr->type = map_type;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Expr* expr_create_set_literal(Expr** elements, int element_count, Type* set_type, char* file, int line, int col) {
    Expr* expr = (Expr*)safe_malloc(sizeof(Expr));
    expr->kind = EXPR_SET_LITERAL;
    expr->set_literal.elements = elements;
    expr->set_literal.element_count = element_count;
    expr->set_literal.set_type = set_type;
    expr->type = set_type;
    expr->file = file ? safe_strdup(file) : NULL;
    expr->line = line;
    expr->column = col;
    return expr;
}

Stmt* stmt_create_record_decl(char* name,
                              char** type_params,
                              int type_param_count,
                              char** field_names,
                              Type** field_types,
                              int field_count,
                              char* file,
                              int line,
                              int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_RECORD_DECL;
    stmt->is_public = true;
    stmt->record_decl.name = name ? safe_strdup(name) : NULL;
    stmt->record_decl.type_params = type_params;
    stmt->record_decl.type_param_count = type_param_count;
    stmt->record_decl.field_names = field_names;
    stmt->record_decl.field_types = field_types;
    stmt->record_decl.field_count = field_count;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_interface_decl(char* name, char** method_names, Type** method_types, int method_count, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_INTERFACE_DECL;
    stmt->is_public = true;
    stmt->interface_decl.name = name ? safe_strdup(name) : NULL;
    stmt->interface_decl.method_names = method_names;
    stmt->interface_decl.method_types = method_types;
    stmt->interface_decl.method_count = method_count;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_impl_decl(char* interface_name, char* record_name, char** method_names, char** function_names, int method_count, char* file, int line, int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_IMPL_DECL;
    stmt->is_public = true;
    stmt->impl_decl.interface_name = interface_name ? safe_strdup(interface_name) : NULL;
    stmt->impl_decl.record_name = record_name ? safe_strdup(record_name) : NULL;
    stmt->impl_decl.method_names = method_names;
    stmt->impl_decl.function_names = function_names;
    stmt->impl_decl.method_count = method_count;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_type_alias(char* name,
                             char** type_params,
                             int type_param_count,
                             Type* target_type,
                             char* file,
                             int line,
                             int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_TYPE_ALIAS;
    stmt->is_public = true;
    stmt->type_alias.name = name ? safe_strdup(name) : NULL;
    stmt->type_alias.type_params = type_params;
    stmt->type_alias.type_param_count = type_param_count;
    stmt->type_alias.target_type = target_type;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* stmt_create_enum_decl(char* name,
                            char** type_params,
                            int type_param_count,
                            char** member_names,
                            int64_t* member_values,
                            Type*** member_payload_types,
                            int* member_payload_counts,
                            int member_count,
                            bool has_payload_members,
                            char* file,
                            int line,
                            int col) {
    Stmt* stmt = (Stmt*)safe_malloc(sizeof(Stmt));
    stmt->kind = STMT_ENUM_DECL;
    stmt->is_public = true;
    stmt->enum_decl.name = name ? safe_strdup(name) : NULL;
    stmt->enum_decl.type_params = type_params;
    stmt->enum_decl.type_param_count = type_param_count;
    stmt->enum_decl.member_names = member_names;
    stmt->enum_decl.member_values = member_values;
    stmt->enum_decl.member_payload_types = member_payload_types;
    stmt->enum_decl.member_payload_counts = member_payload_counts;
    stmt->enum_decl.member_count = member_count;
    stmt->enum_decl.has_payload_members = has_payload_members;
    stmt->file = file ? safe_strdup(file) : NULL;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Program* program_create(const char* source_file) {
    Program* prog = (Program*)safe_malloc(sizeof(Program));
    prog->statements = NULL;
    prog->stmt_count = 0;
    prog->source_file = source_file ? safe_strdup(source_file) : NULL;
    return prog;
}

void program_add_stmt(Program* prog, Stmt* stmt) {
    prog->stmt_count++;
    prog->statements = (Stmt**)safe_realloc(prog->statements, prog->stmt_count * sizeof(Stmt*));
    prog->statements[prog->stmt_count - 1] = stmt;
}

void expr_free(Expr* expr) {
    if (!expr) return;
    
    switch (expr->kind) {
        case EXPR_LITERAL:
            if (expr->type &&
                (expr->type->kind == TYPE_STRING || expr->type->kind == TYPE_BIGINT) &&
                expr->literal.as_string) {
                free(expr->literal.as_string);
            }
            break;
        case EXPR_IDENTIFIER:
            free(expr->identifier);
            break;
        case EXPR_BINARY:
            expr_free(expr->binary.left);
            expr_free(expr->binary.right);
            break;
        case EXPR_UNARY:
            expr_free(expr->unary.operand);
            break;
        case EXPR_CALL:
            expr_free(expr->call.callee);
            for (int i = 0; i < expr->call.arg_count; i++) {
                expr_free(expr->call.args[i]);
            }
            if (expr->call.args) free(expr->call.args);
            if (expr->call.type_args) free(expr->call.type_args);
            break;
        case EXPR_FUNC_LITERAL:
            for (int i = 0; i < expr->func_literal.param_count; i++) {
                free(expr->func_literal.params[i]);
            }
            if (expr->func_literal.params) free(expr->func_literal.params);
            if (expr->func_literal.param_types) free(expr->func_literal.param_types);
            for (int i = 0; i < expr->func_literal.capture_count; i++) {
                free(expr->func_literal.capture_names[i]);
            }
            if (expr->func_literal.capture_names) free(expr->func_literal.capture_names);
            stmt_free(expr->func_literal.body);
            if (expr->func_literal.compiled_name) free(expr->func_literal.compiled_name);
            break;
        case EXPR_INDEX:
            expr_free(expr->index.array);
            expr_free(expr->index.index);
            break;
        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->array_literal.element_count; i++) {
                expr_free(expr->array_literal.elements[i]);
            }
            if (expr->array_literal.elements) free(expr->array_literal.elements);
            break;
        case EXPR_CAST:
            expr_free(expr->cast.value);
            // Fix 8: Don't free cast.target_type here; it's the same as expr->type
            // and types are shared with symbol tables
            break;
        case EXPR_TRY:
            expr_free(expr->try_expr.expr);
            break;
        case EXPR_AWAIT:
            expr_free(expr->await_expr.expr);
            break;
        case EXPR_TYPE_TEST:
            expr_free(expr->type_test.value);
            break;
        case EXPR_IF:
            expr_free(expr->if_expr.condition);
            expr_free(expr->if_expr.then_expr);
            expr_free(expr->if_expr.else_expr);
            break;
        case EXPR_MATCH:
            expr_free(expr->match_expr.subject);
            for (int i = 0; i < expr->match_expr.arm_count; i++) {
                expr_free(expr->match_expr.patterns[i]);
                if (expr->match_expr.guards) {
                    expr_free(expr->match_expr.guards[i]);
                }
                expr_free(expr->match_expr.values[i]);
                if (expr->match_expr.payload_binding_names &&
                    expr->match_expr.payload_binding_names[i]) {
                    int bind_count = (expr->match_expr.payload_binding_counts &&
                                      i < expr->match_expr.arm_count)
                                         ? expr->match_expr.payload_binding_counts[i]
                                         : 0;
                    for (int j = 0; j < bind_count; j++) {
                        if (expr->match_expr.payload_binding_names[i][j]) {
                            free(expr->match_expr.payload_binding_names[i][j]);
                        }
                    }
                    free(expr->match_expr.payload_binding_names[i]);
                }
            }
            if (expr->match_expr.patterns) free(expr->match_expr.patterns);
            if (expr->match_expr.guards) free(expr->match_expr.guards);
            if (expr->match_expr.values) free(expr->match_expr.values);
            if (expr->match_expr.arm_group_ids) free(expr->match_expr.arm_group_ids);
            if (expr->match_expr.payload_binding_names) free(expr->match_expr.payload_binding_names);
            if (expr->match_expr.payload_binding_counts) free(expr->match_expr.payload_binding_counts);
            expr_free(expr->match_expr.else_expr);
            break;
        case EXPR_BLOCK:
            for (int i = 0; i < expr->block_expr.stmt_count; i++) {
                stmt_free(expr->block_expr.statements[i]);
            }
            if (expr->block_expr.statements) free(expr->block_expr.statements);
            expr_free(expr->block_expr.value);
            break;
        case EXPR_RECORD_LITERAL:
            for (int i = 0; i < expr->record_literal.field_count; i++) {
                free(expr->record_literal.field_names[i]);
                expr_free(expr->record_literal.field_values[i]);
            }
            if (expr->record_literal.field_names) free(expr->record_literal.field_names);
            if (expr->record_literal.field_values) free(expr->record_literal.field_values);
            break;
        case EXPR_FIELD_ACCESS:
            expr_free(expr->field_access.object);
            free(expr->field_access.field_name);
            break;
        case EXPR_TUPLE_LITERAL:
            for (int i = 0; i < expr->tuple_literal.element_count; i++) {
                expr_free(expr->tuple_literal.elements[i]);
            }
            if (expr->tuple_literal.elements) free(expr->tuple_literal.elements);
            break;
        case EXPR_TUPLE_ACCESS:
            expr_free(expr->tuple_access.tuple);
            break;
        case EXPR_MAP_LITERAL:
            for (int i = 0; i < expr->map_literal.entry_count; i++) {
                expr_free(expr->map_literal.keys[i]);
                expr_free(expr->map_literal.values[i]);
            }
            if (expr->map_literal.keys) free(expr->map_literal.keys);
            if (expr->map_literal.values) free(expr->map_literal.values);
            break;
        case EXPR_SET_LITERAL:
            for (int i = 0; i < expr->set_literal.element_count; i++) {
                expr_free(expr->set_literal.elements[i]);
            }
            if (expr->set_literal.elements) free(expr->set_literal.elements);
            break;
        default:
            break;
    }
    
    if (expr->file) free(expr->file);
    // Fix 8: Don't free expr->type here; types are shared with symbol tables
    // and would cause double-frees
    free(expr);
}

void stmt_free(Stmt* stmt) {
    if (!stmt) return;
    
    switch (stmt->kind) {
        case STMT_VAR_DECL:
            free(stmt->var_decl.name);
            expr_free(stmt->var_decl.initializer);
            break;
        case STMT_VAR_TUPLE_DECL:
            for (int i = 0; i < stmt->var_tuple_decl.name_count; i++) {
                free(stmt->var_tuple_decl.names[i]);
            }
            if (stmt->var_tuple_decl.names) free(stmt->var_tuple_decl.names);
            expr_free(stmt->var_tuple_decl.initializer);
            break;
        case STMT_EXPR:
            expr_free(stmt->expr_stmt);
            break;
        case STMT_ASSIGN:
            free(stmt->assign.name);
            expr_free(stmt->assign.value);
            break;
        case STMT_ASSIGN_INDEX:
            expr_free(stmt->assign_index.target);
            expr_free(stmt->assign_index.index);
            expr_free(stmt->assign_index.value);
            break;
        case STMT_ASSIGN_FIELD:
            expr_free(stmt->assign_field.object);
            free(stmt->assign_field.field_name);
            expr_free(stmt->assign_field.value);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < stmt->block.stmt_count; i++) {
                stmt_free(stmt->block.statements[i]);
            }
            if (stmt->block.statements) free(stmt->block.statements);
            break;
        case STMT_IF:
            expr_free(stmt->if_stmt.condition);
            stmt_free(stmt->if_stmt.then_branch);
            stmt_free(stmt->if_stmt.else_branch);
            break;
        case STMT_MATCH:
            expr_free(stmt->match_stmt.subject);
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                expr_free(stmt->match_stmt.patterns[i]);
                if (stmt->match_stmt.guards) {
                    expr_free(stmt->match_stmt.guards[i]);
                }
                stmt_free(stmt->match_stmt.bodies[i]);
                if (stmt->match_stmt.payload_binding_names &&
                    stmt->match_stmt.payload_binding_names[i]) {
                    int bind_count = (stmt->match_stmt.payload_binding_counts &&
                                      i < stmt->match_stmt.arm_count)
                                         ? stmt->match_stmt.payload_binding_counts[i]
                                         : 0;
                    for (int j = 0; j < bind_count; j++) {
                        if (stmt->match_stmt.payload_binding_names[i][j]) {
                            free(stmt->match_stmt.payload_binding_names[i][j]);
                        }
                    }
                    free(stmt->match_stmt.payload_binding_names[i]);
                }
            }
            if (stmt->match_stmt.patterns) free(stmt->match_stmt.patterns);
            if (stmt->match_stmt.guards) free(stmt->match_stmt.guards);
            if (stmt->match_stmt.bodies) free(stmt->match_stmt.bodies);
            if (stmt->match_stmt.arm_group_ids) free(stmt->match_stmt.arm_group_ids);
            if (stmt->match_stmt.payload_binding_names) free(stmt->match_stmt.payload_binding_names);
            if (stmt->match_stmt.payload_binding_counts) free(stmt->match_stmt.payload_binding_counts);
            stmt_free(stmt->match_stmt.else_branch);
            break;
        case STMT_WHILE:
            expr_free(stmt->while_stmt.condition);
            stmt_free(stmt->while_stmt.body);
            break;
        case STMT_FOREACH:
            free(stmt->foreach.var_name);
            expr_free(stmt->foreach.iterable);
            stmt_free(stmt->foreach.body);
            break;
        case STMT_FOR_RANGE:
            free(stmt->for_range.var_name);
            expr_free(stmt->for_range.start);
            expr_free(stmt->for_range.end);
            stmt_free(stmt->for_range.body);
            break;
        case STMT_RETURN:
            expr_free(stmt->return_value);
            break;
        case STMT_DEFER:
            expr_free(stmt->defer_expr);
            break;
        case STMT_FUNC_DECL:
            free(stmt->func_decl.name);
            for (int i = 0; i < stmt->func_decl.type_param_count; i++) {
                free(stmt->func_decl.type_params[i]);
            }
            if (stmt->func_decl.type_params) free(stmt->func_decl.type_params);
            if (stmt->func_decl.type_param_constraints) free(stmt->func_decl.type_param_constraints);
            for (int i = 0; i < stmt->func_decl.param_count; i++) {
                free(stmt->func_decl.params[i]);
                // Don't free param_types - they are shared with symbol tables
                // and would cause double-frees (similar to Fix 8 in expr_free)
            }
            if (stmt->func_decl.params) free(stmt->func_decl.params);
            // Don't free param_types array - it's owned by symbol tables
            stmt_free(stmt->func_decl.body);
            break;
        case STMT_IMPORT:
            free(stmt->import_path);
            break;
        case STMT_RECORD_DECL:
            free(stmt->record_decl.name);
            for (int i = 0; i < stmt->record_decl.type_param_count; i++) {
                free(stmt->record_decl.type_params[i]);
            }
            if (stmt->record_decl.type_params) free(stmt->record_decl.type_params);
            for (int i = 0; i < stmt->record_decl.field_count; i++) {
                free(stmt->record_decl.field_names[i]);
                // Don't free field_types - they are shared with symbol tables
                // and would cause double-frees (similar to Fix 8 in expr_free)
            }
            if (stmt->record_decl.field_names) free(stmt->record_decl.field_names);
            // Don't free field_types array - it's owned by symbol tables
            break;
        case STMT_INTERFACE_DECL:
            free(stmt->interface_decl.name);
            for (int i = 0; i < stmt->interface_decl.method_count; i++) {
                free(stmt->interface_decl.method_names[i]);
                // Don't free method_types - they are shared with symbol tables.
            }
            if (stmt->interface_decl.method_names) free(stmt->interface_decl.method_names);
            // Don't free method_types array - it's owned by symbol tables.
            break;
        case STMT_IMPL_DECL:
            free(stmt->impl_decl.interface_name);
            free(stmt->impl_decl.record_name);
            for (int i = 0; i < stmt->impl_decl.method_count; i++) {
                free(stmt->impl_decl.method_names[i]);
                free(stmt->impl_decl.function_names[i]);
            }
            if (stmt->impl_decl.method_names) free(stmt->impl_decl.method_names);
            if (stmt->impl_decl.function_names) free(stmt->impl_decl.function_names);
            break;
        case STMT_TYPE_ALIAS:
            free(stmt->type_alias.name);
            for (int i = 0; i < stmt->type_alias.type_param_count; i++) {
                free(stmt->type_alias.type_params[i]);
            }
            if (stmt->type_alias.type_params) free(stmt->type_alias.type_params);
            // Don't free target_type - it may be shared with symbol tables.
            break;
        case STMT_ENUM_DECL:
            free(stmt->enum_decl.name);
            for (int i = 0; i < stmt->enum_decl.type_param_count; i++) {
                if (stmt->enum_decl.type_params && stmt->enum_decl.type_params[i]) {
                    free(stmt->enum_decl.type_params[i]);
                }
            }
            if (stmt->enum_decl.type_params) free(stmt->enum_decl.type_params);
            for (int i = 0; i < stmt->enum_decl.member_count; i++) {
                free(stmt->enum_decl.member_names[i]);
                int payload_count = (stmt->enum_decl.member_payload_counts &&
                                     i < stmt->enum_decl.member_count)
                                        ? stmt->enum_decl.member_payload_counts[i]
                                        : 0;
                if (payload_count > 0 && stmt->enum_decl.member_payload_types &&
                    stmt->enum_decl.member_payload_types[i]) {
                    // Payload type nodes may be shared with type-checker symbols, so only free the container.
                    free(stmt->enum_decl.member_payload_types[i]);
                }
            }
            if (stmt->enum_decl.member_names) free(stmt->enum_decl.member_names);
            if (stmt->enum_decl.member_values) free(stmt->enum_decl.member_values);
            if (stmt->enum_decl.member_payload_types) free(stmt->enum_decl.member_payload_types);
            if (stmt->enum_decl.member_payload_counts) free(stmt->enum_decl.member_payload_counts);
            break;
        default:
            break;
    }
    
    if (stmt->file) free(stmt->file);
    free(stmt);
}

void program_free(Program* prog) {
    if (!prog) return;
    for (int i = 0; i < prog->stmt_count; i++) {
        stmt_free(prog->statements[i]);
    }
    if (prog->statements) free(prog->statements);
    if (prog->source_file) free(prog->source_file);
    free(prog);
}
