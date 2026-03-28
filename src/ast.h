#ifndef AST_H
#define AST_H
#include "types.h"
#include "common.h"
#include "lexer.h"

typedef enum {
    EXPR_LITERAL,
    EXPR_IDENTIFIER,
    EXPR_BINARY,
    EXPR_UNARY,
    EXPR_CALL,
    EXPR_FUNC_LITERAL,    // func(param: type, ...): returnType { ... }
    EXPR_ARRAY,
    EXPR_INDEX,
    EXPR_NIL,
    EXPR_ARRAY_LITERAL,
    EXPR_CAST,
    EXPR_TRY,            // expr?
    EXPR_AWAIT,          // await expr
    EXPR_TYPE_TEST,      // expr is Type
    EXPR_IF,             // if (cond) thenExpr else elseExpr
    EXPR_MATCH,          // match (value) { pattern: expr, else: expr }
    EXPR_BLOCK,          // { stmt; ... expr }
    EXPR_RECORD_LITERAL,   // { field: value, ... }
    EXPR_FIELD_ACCESS,     // obj.field
    EXPR_TUPLE_LITERAL,    // (value1, value2, ...)
    EXPR_TUPLE_ACCESS,     // tuple.0, tuple.1, etc.
    EXPR_MAP_LITERAL,      // { key: value, ... }
    EXPR_SET_LITERAL       // { value1, value2, ... }
} ExprKind;

struct Stmt;

typedef struct Expr {
    ExprKind kind;
    Type* type;
    char* file;
    int line;
    int column;
    union {
        struct {
            union { int64_t as_int; double as_double; char* as_string; };
        } literal;
        char* identifier;
        struct {
            struct Expr* left;
            struct Expr* right;
            TokenType op;
        } binary;
        struct {
            struct Expr* operand;
            TokenType op;
        } unary;
        struct {
            struct Expr* callee;
            struct Expr** args;
            int arg_count;
            Type** type_args;
            int type_arg_count;
        } call;
        struct {
            Type* return_type;
            char** params;
            Type** param_types;
            int param_count;
            char** capture_names;
            int capture_count;
            struct Stmt* body;
            char* compiled_name;
            bool is_async;
        } func_literal;
        struct {
            struct Expr* array;
            struct Expr* index;
        } index;
        struct {
            struct Expr** elements;
            int element_count;
        } array_literal;
        struct {
            struct Expr* value;
            Type* target_type;
        } cast;
        struct {
            struct Expr* expr;
        } try_expr;
        struct {
            struct Expr* expr;
        } await_expr;
        struct {
            struct Expr* value;
            Type* target_type;
        } type_test;
        struct {
            struct Expr* condition;
            struct Expr* then_expr;
            struct Expr* else_expr;
        } if_expr;
        struct {
            struct Expr* subject;
            struct Expr** patterns;
            struct Expr** guards;
            struct Expr** values;
            int* arm_group_ids;
            char*** payload_binding_names; // per-arm destructuring binding names; NULL entry means discard '_'
            int* payload_binding_counts;   // per-arm destructuring binding count
            int arm_count;
            struct Expr* else_expr;
        } match_expr;
        struct {
            struct Stmt** statements;
            int stmt_count;
            struct Expr* value;
        } block_expr;
        struct {
            char** field_names;
            struct Expr** field_values;
            int field_count;
            Type* record_type;  // Inferred or explicit type
            Type* pattern_type; // Optional explicit type qualifier for record patterns
            bool is_pattern;
            bool allows_rest;
        } record_literal;
        struct {
            struct Expr* object;
            char* field_name;
            int field_index;
        } field_access;
        struct {
            struct Expr** elements;
            int element_count;
        } tuple_literal;
        struct {
            struct Expr* tuple;
            int index;
        } tuple_access;
        struct {
            struct Expr** keys;
            struct Expr** values;
            int entry_count;
            Type* map_type;  // Inferred or explicit type
        } map_literal;
        struct {
            struct Expr** elements;
            int element_count;
            Type* set_type;  // Inferred or explicit type
        } set_literal;
    };
} Expr;

typedef enum {
    STMT_VAR_DECL,
    STMT_VAR_TUPLE_DECL,
    STMT_EXPR,
    STMT_ASSIGN,
    STMT_BLOCK,
    STMT_IF,
    STMT_MATCH,
    STMT_WHILE,
    STMT_FOREACH,
    STMT_FOR_RANGE,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_RETURN,
    STMT_DEFER,
    STMT_FUNC_DECL,
    STMT_IMPORT,
    STMT_ASSIGN_INDEX,
    STMT_ASSIGN_FIELD,
    STMT_RECORD_DECL,   // record TypeName { field: type, ... }
    STMT_INTERFACE_DECL, // interface Name { method(...): type; ... }
    STMT_IMPL_DECL,     // impl Interface as Record { method = function; ... }
    STMT_TYPE_ALIAS,    // type Name = ExistingType;
    STMT_ENUM_DECL      // enum Name { Variant = int, Variant(T, U), ... };
} StmtKind;

typedef struct Stmt {
    StmtKind kind;
    char* file;
    int line;
    int column;
    bool is_public;
    union {
        struct {
            char* name;
            Type* type_annotation;
            struct Expr* initializer;
            bool is_mutable;
        } var_decl;
        struct {
            char** names;
            int name_count;
            Type* type_annotation;
            struct Expr* initializer;
            bool is_mutable;
        } var_tuple_decl;
        struct Expr* expr_stmt;
        struct {
            char* name;
            struct Expr* value;
            TokenType op; // TOKEN_ASSIGN or a compound assignment operator (TOKEN_PLUS_EQ, ...)
        } assign;
        struct {
            struct Stmt** statements;
            int stmt_count;
        } block;
        struct {
            struct Expr* condition;
            struct Stmt* then_branch;
            struct Stmt* else_branch;
        } if_stmt;
        struct {
            struct Expr* subject;
            struct Expr** patterns;
            struct Expr** guards;
            struct Stmt** bodies;
            int* arm_group_ids;
            char*** payload_binding_names; // per-arm destructuring binding names; NULL entry means discard '_'
            int* payload_binding_counts;   // per-arm destructuring binding count
            int arm_count;
            struct Stmt* else_branch;
        } match_stmt;
        struct {
            struct Expr* condition;
            struct Stmt* body;
        } while_stmt;
        struct {
            char* var_name;
            struct Expr* iterable;
            struct Stmt* body;
        } foreach;
        struct {
            char* var_name;
            struct Expr* start;
            struct Expr* end;
            struct Stmt* body;
        } for_range;
        struct {
            struct Expr* target;
            struct Expr* index;
            struct Expr* value;
            TokenType op; // TOKEN_ASSIGN or a compound assignment operator
        } assign_index;
        struct {
            struct Expr* object;
            char* field_name;
            struct Expr* value;
            TokenType op; // TOKEN_ASSIGN or a compound assignment operator
            int field_index;
        } assign_field;
        struct {
            Type* return_type;
            char* name;
            char** type_params;
            Type** type_param_constraints;
            int type_param_count;
            char** params;
            Type** param_types;
            int param_count;
            struct Stmt* body;
            bool is_async;
        } func_decl;
        char* import_path;
        struct Expr* return_value;
        struct Expr* defer_expr;
        struct {
            char* name;
            char** type_params;
            int type_param_count;
            char** field_names;
            Type** field_types;
            int field_count;
        } record_decl;
        struct {
            char* name;
            char** method_names;
            Type** method_types; // each method type is TYPE_FUNCTION (without receiver)
            int method_count;
        } interface_decl;
        struct {
            char* interface_name;
            char* record_name;
            char** method_names;
            char** function_names;
            int method_count;
        } impl_decl;
        struct {
            char* name;
            char** type_params;
            int type_param_count;
            Type* target_type;
        } type_alias;
        struct {
            char* name;
            char** type_params;
            int type_param_count;
            char** member_names;
            int64_t* member_values;
            Type*** member_payload_types; // per-member payload type list (may be NULL)
            int* member_payload_counts;    // per-member payload arity
            int member_count;
            bool has_payload_members;
        } enum_decl;
    };
} Stmt;

typedef struct {
    Stmt** statements;
    int stmt_count;
    char* source_file;
} Program;

Program* program_create(const char* source_file);
void program_free(Program* prog);
void program_add_stmt(Program* prog, Stmt* stmt);
void expr_free(Expr* expr);
void stmt_free(Stmt* stmt);

// Clone functions for deep copying AST nodes
Expr* expr_clone(Expr* expr);
Stmt* stmt_clone(Stmt* stmt);

Expr* expr_create_literal_int(int64_t value, char* file, int line, int col);
Expr* expr_create_literal_bool(bool value, char* file, int line, int col);
Expr* expr_create_literal_double(double value, char* file, int line, int col);
Expr* expr_create_literal_bigint(const char* value, char* file, int line, int col);
Expr* expr_create_literal_string(char* value, char* file, int line, int col);
Expr* expr_create_nil(char* file, int line, int col);
Expr* expr_create_identifier(char* name, char* file, int line, int col);
Expr* expr_create_binary(TokenType op, Expr* left, Expr* right, char* file, int line, int col);
Expr* expr_create_unary(TokenType op, Expr* operand, char* file, int line, int col);
Expr* expr_create_call(Expr* callee, Expr** args, int arg_count, Type** type_args, int type_arg_count, char* file, int line, int col);
Expr* expr_create_func_literal(Type* return_type, char** params, Type** param_types, int param_count, Stmt* body, bool is_async, char* file, int line, int col);
Expr* expr_create_array(Expr* array, Expr* index, char* file, int line, int col);
Expr* expr_create_array_literal(Expr** elements, int element_count, char* file, int line, int col);
Expr* expr_create_cast(Expr* value, Type* target_type, char* file, int line, int col);
Expr* expr_create_try(Expr* operand, char* file, int line, int col);
Expr* expr_create_await(Expr* operand, char* file, int line, int col);
Expr* expr_create_type_test(Expr* value, Type* target_type, char* file, int line, int col);
Expr* expr_create_if(Expr* condition,
                     Expr* then_expr,
                     Expr* else_expr,
                     char* file,
                     int line,
                     int col);
Expr* expr_create_match(Expr* subject,
                        Expr** patterns,
                        Expr** guards,
                        Expr** values,
                        int arm_count,
                        Expr* else_expr,
                        char* file,
                        int line,
                        int col);
Expr* expr_create_block(Stmt** statements,
                        int stmt_count,
                        Expr* value,
                        char* file,
                        int line,
                        int col);
Expr* expr_create_record_literal(char** field_names, Expr** field_values, int field_count, Type* record_type, char* file, int line, int col);
Expr* expr_create_field_access(Expr* object, char* field_name, char* file, int line, int col);
Expr* expr_create_tuple_literal(Expr** elements, int element_count, char* file, int line, int col);
Expr* expr_create_tuple_access(Expr* tuple, int index, char* file, int line, int col);
Expr* expr_create_map_literal(Expr** keys, Expr** values, int entry_count, Type* map_type, char* file, int line, int col);
Expr* expr_create_set_literal(Expr** elements, int element_count, Type* set_type, char* file, int line, int col);

Stmt* stmt_create_var_decl(char* name, Type* type, Expr* init, bool is_mutable, char* file, int line, int col);
Stmt* stmt_create_var_tuple_decl(char** names, int name_count, Type* type, Expr* init, bool is_mutable, char* file, int line, int col);
Stmt* stmt_create_expr(Expr* expr, char* file, int line, int col);
Stmt* stmt_create_assign(char* name, Expr* value, TokenType op, char* file, int line, int col);
Stmt* stmt_create_assign_index(Expr* target, Expr* index, Expr* value, TokenType op, char* file, int line, int col);
Stmt* stmt_create_assign_field(Expr* object, char* field_name, Expr* value, TokenType op, char* file, int line, int col);
Stmt* stmt_create_block(Stmt** statements, int stmt_count, char* file, int line, int col);
Stmt* stmt_create_if(Expr* condition, Stmt* then_branch, Stmt* else_branch, char* file, int line, int col);
Stmt* stmt_create_while(Expr* condition, Stmt* body, char* file, int line, int col);
Stmt* stmt_create_foreach(char* var_name, Expr* iterable, Stmt* body, char* file, int line, int col);
Stmt* stmt_create_for_range(char* var_name, Expr* start, Expr* end, Stmt* body, char* file, int line, int col);
Stmt* stmt_create_break(char* file, int line, int col);
Stmt* stmt_create_continue(char* file, int line, int col);
Stmt* stmt_create_return(Expr* value, char* file, int line, int col);
Stmt* stmt_create_defer(Expr* expr, char* file, int line, int col);
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
                            int col);
Stmt* stmt_create_import(char* path, char* file, int line, int col);
Stmt* stmt_create_record_decl(char* name,
                              char** type_params,
                              int type_param_count,
                              char** field_names,
                              Type** field_types,
                              int field_count,
                              char* file,
                              int line,
                              int col);
Stmt* stmt_create_interface_decl(char* name, char** method_names, Type** method_types, int method_count, char* file, int line, int col);
Stmt* stmt_create_impl_decl(char* interface_name, char* record_name, char** method_names, char** function_names, int method_count, char* file, int line, int col);
Stmt* stmt_create_type_alias(char* name,
                             char** type_params,
                             int type_param_count,
                             Type* target_type,
                             char* file,
                             int line,
                             int col);
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
                            int col);
Stmt* stmt_create_match(Expr* subject, Expr** patterns, Expr** guards, Stmt** bodies, int arm_count, Stmt* else_branch, char* file, int line, int col);
#endif
