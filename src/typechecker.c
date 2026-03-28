#include "typechecker.h"
#include "native_extension.h"
#include "safe_alloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct GenericRecordDeclEntry {
    Stmt* stmt;
};

struct GenericTypeAliasDeclEntry {
    Stmt* stmt;
};

struct GenericEnumDeclEntry {
    Stmt* stmt;
};

struct GenericRecordInstanceEntry {
    char* key;
    Type* type;
};

struct GenericEnumInstanceEntry {
    char* key;
    Type* type;
};

typedef enum {
    MATCH_PATTERN_CONST_NONE,
    MATCH_PATTERN_CONST_INT,
    MATCH_PATTERN_CONST_BOOL,
    MATCH_PATTERN_CONST_DOUBLE,
    MATCH_PATTERN_CONST_STRING,
    MATCH_PATTERN_CONST_BIGINT,
    MATCH_PATTERN_CONST_NIL
} MatchPatternConstKind;

typedef struct {
    MatchPatternConstKind kind;
    int64_t as_int;
    double as_double;
    const char* as_string;
} MatchPatternConst;

typedef struct MatchPatternRow MatchPatternRow;

static void typecheck_statement(TypeChecker* tc, Stmt* stmt);
static void typechecker_error(TypeChecker* tc, const char* message, const char* file, int line, int column);
static void typechecker_warn(TypeChecker* tc, const char* message, const char* file, int line, int column);
static void typechecker_print_source_context(const char* file, int line, int column);
static void symbol_set_visibility_metadata(Symbol* sym, const char* decl_file, bool is_public);
static bool symbol_is_accessible_from_file(const Symbol* sym, const char* use_file);
static bool typechecker_types_assignable(TypeChecker* tc, Type* to, Type* from, const char* file, int line, int column);
static Type* typechecker_resolve_type(TypeChecker* tc, Type* type);
static void typechecker_predeclare_type_alias(TypeChecker* tc, Stmt* stmt);
static void typechecker_predeclare_interface(TypeChecker* tc, Stmt* stmt);
static void typechecker_predeclare_impl(TypeChecker* tc, Stmt* stmt);
static void typechecker_predeclare_enum(TypeChecker* tc, Stmt* stmt);
static bool is_builtin_name(const char* name);
static bool type_is_match_comparable(Type* type);
static bool match_expr_is_bool_literal(const Expr* expr, bool* out_value);
static bool match_expr_constant_key(TypeChecker* tc, const Expr* expr, MatchPatternConst* out_key);
static bool match_pattern_constant_equals(const MatchPatternConst* a, const MatchPatternConst* b);
static void match_pattern_constant_format(const MatchPatternConst* key, char* out, size_t out_size);
static const char* type_enum_name(Type* type);
static bool symbol_is_enum_member_for(const Symbol* sym, const char* enum_name, const char** out_member_name);
static const char* match_expr_enum_member_name(TypeChecker* tc, const Expr* expr, const char* enum_name);
static Type* typecheck_block_expression(TypeChecker* tc, Expr* expr);
static char* match_pattern_call_symbol_name(const Expr* pattern_expr);
static char* typechecker_call_target_name(TypeChecker* tc, const Expr* callee_expr);
static void typechecker_format_generic_type_param_list(char** type_param_names,
                                                       int type_param_count,
                                                       char* out,
                                                       size_t out_size);
static void typechecker_format_named_type_with_type_args(const char* type_name,
                                                         Type** type_args,
                                                         int type_arg_count,
                                                         char* out,
                                                         size_t out_size);
static bool match_binding_name_looks_destructure(const char* name);
static void match_stmt_clear_payload_bindings(Stmt* stmt);
static void match_expr_clear_payload_bindings(Expr* expr);
static void match_pattern_free_payload_bindings(char** names, Type** types, int count);
static bool match_pattern_resolve_enum_payload_types(TypeChecker* tc,
                                                     Expr* pattern_expr,
                                                     Type* subject_type,
                                                     Type*** out_types,
                                                     int* out_count);
static bool match_enum_member_payload_types(TypeChecker* tc,
                                            Type* subject_type,
                                            const char* member_name,
                                            Type*** out_types,
                                            int* out_count);
static bool match_pattern_covers_enum_member(TypeChecker* tc,
                                             Expr* pattern_expr,
                                             Type* subject_type);
static bool match_enum_member_patterns_exhaustive(TypeChecker* tc,
                                                  Expr** patterns,
                                                  int pattern_count,
                                                  Type* subject_type,
                                                  const char* member_name);
static bool match_pattern_matrix_exhaustive(TypeChecker* tc,
                                            MatchPatternRow* rows,
                                            int row_count,
                                            Type** column_types,
                                            int column_count);
static bool match_patterns_exhaustive(TypeChecker* tc,
                                      Expr** patterns,
                                      int pattern_count,
                                      Type* subject_type);
static void match_pattern_apply_context_types(TypeChecker* tc,
                                              Expr* pattern_expr,
                                              Type* subject_type,
                                              bool allow_identifier_binding);
static Type* match_merge_result_type(TypeChecker* tc,
                                     Type* current,
                                     Type* candidate,
                                     const char* file,
                                     int line,
                                     int column);
static bool match_pattern_collect_payload_bindings(TypeChecker* tc,
                                                   Expr* pattern_expr,
                                                   Type* subject_type,
                                                   char*** out_names,
                                                   Type*** out_types,
                                                   int* out_count);
static bool match_enum_member_is_covered(const char** covered_members, int covered_count, const char* member_name);
static int match_enum_member_count(TypeChecker* tc, const char* enum_name);
static bool typechecker_enum_member_value(TypeChecker* tc, const char* enum_name, const char* member_name, int64_t* out_value);
static bool typechecker_call_allows_interface_receiver(TypeChecker* tc,
                                                       Expr* call_expr,
                                                       Type* callee_type,
                                                       int arg_index,
                                                       Type* param_type,
                                                       Type* arg_type,
                                                       const char* file,
                                                       int line,
                                                       int column);
static int typechecker_type_param_binding_index(char** type_param_names, int type_param_count, const char* name);
static bool typechecker_infer_generic_bindings(TypeChecker* tc,
                                               Type* param_type,
                                               Type* arg_type,
                                               char** type_param_names,
                                               Type** bound_types,
                                               int type_param_count,
                                               const char* file,
                                               int line,
                                               int column,
                                               const char* inference_context);
static Type* typechecker_substitute_generic_type(Type* type,
                                                 char** type_param_names,
                                                 Type** bound_types,
                                                 int type_param_count,
                                                 bool* out_unresolved);
static Type* typechecker_type_param_constraint(TypeChecker* tc, const char* type_param_name);
static Type* typechecker_resolve_type_param_constraint(TypeChecker* tc, Type* type_param_type);
static Type* typecheck_func_literal(TypeChecker* tc, Expr* expr);
static Symbol* typechecker_lookup(TypeChecker* tc, const char* name);
static Symbol* typechecker_lookup_with_scope(TypeChecker* tc, const char* name, int* out_scope_index);
static FuncLiteralCaptureContext* typechecker_current_capture_context(TypeChecker* tc);
static void typechecker_capture_context_push(TypeChecker* tc, Expr* expr, int outer_local_count);
static void typechecker_capture_context_add(TypeChecker* tc, const char* name);
static FuncLiteralCaptureContext typechecker_capture_context_pop(TypeChecker* tc);
static void typechecker_capture_context_free(FuncLiteralCaptureContext* ctx);
static void typechecker_impl_mapping_free(InterfaceImplMapping* mapping);
static InterfaceImplMapping* typechecker_find_impl_mapping_exact(TypeChecker* tc, const char* interface_name, const char* record_name);
static InterfaceImplMapping* typechecker_find_impl_mapping(TypeChecker* tc, const char* interface_name, const char* record_name);
static const char* typechecker_impl_lookup_function(const InterfaceImplMapping* mapping, const char* method_name);
static const char* typechecker_impl_lookup_method(const InterfaceImplMapping* mapping, const char* function_name);
static Stmt* typechecker_find_generic_record_decl(TypeChecker* tc, const char* name);
static Stmt* typechecker_find_generic_type_alias_decl(TypeChecker* tc, const char* name);
static Stmt* typechecker_find_generic_enum_decl(TypeChecker* tc, const char* name);
static void typechecker_register_generic_record_decl(TypeChecker* tc, Stmt* stmt);
static void typechecker_register_generic_type_alias_decl(TypeChecker* tc, Stmt* stmt);
static void typechecker_register_generic_enum_decl(TypeChecker* tc, Stmt* stmt);
static GenericRecordInstanceEntry* typechecker_find_generic_record_instance(TypeChecker* tc, const char* key);
static GenericEnumInstanceEntry* typechecker_find_generic_enum_instance(TypeChecker* tc, const char* key);
static void typechecker_register_generic_enum_instance(TypeChecker* tc, const char* key, Type* type);
static void typechecker_seed_generic_bindings_from_enum_subject(TypeChecker* tc,
                                                                const char* subject_enum,
                                                                Type* member_fn_type,
                                                                Type** bound_types,
                                                                int type_param_count);
static bool typechecker_type_contains_type_param(Type* type);
static char* typechecker_build_generic_record_key(const char* name, Type** type_args, int type_arg_count);
static char* typechecker_build_generic_enum_key(const char* name, Type** type_args, int type_arg_count);
static bool typechecker_record_base_name_equals(const char* a, const char* b);
static Type* typechecker_instantiate_generic_record(TypeChecker* tc,
                                                    Stmt* decl,
                                                    Type** type_args,
                                                    int type_arg_count,
                                                    bool nullable);
static Type* typechecker_instantiate_generic_type_alias(TypeChecker* tc,
                                                        Stmt* decl,
                                                        Type** type_args,
                                                        int type_arg_count,
                                                        bool nullable);
static Type* typechecker_contextual_expected_type_for_inference(TypeChecker* tc, Type* expected_type);
static bool typechecker_has_unbound_generic_type_params(Type** bound_types, int type_param_count);
static void typecheck_impl_decl(TypeChecker* tc, Stmt* stmt);
static void typechecker_refresh_function_signature(TypeChecker* tc, Stmt* stmt);
static bool typechecker_interface_has_method_named(TypeChecker* tc, const char* method_name);
static Type* typecheck_await(TypeChecker* tc, Expr* expr);

static void typechecker_declare_builtin_error_type(TypeChecker* tc) {
    if (!tc || !tc->globals) return;
    if (symbol_table_has(tc->globals, "Error")) return;

    Type* error_type = type_record("Error");
    if (error_type && error_type->record_def) {
        Type* code_type = type_int();
        record_def_add_field(error_type->record_def, "code", code_type);
        type_free(code_type);

        Type* message_type = type_string();
        record_def_add_field(error_type->record_def, "message", message_type);
        type_free(message_type);

        Type* data_type = type_any();
        record_def_add_field(error_type->record_def, "data", data_type);
        type_free(data_type);
    }

    Symbol* sym = symbol_create(error_type, "Error", false);
    symbol_table_add(tc->globals, sym);
}

static void typechecker_declare_builtin_error_code_constants(TypeChecker* tc) {
    if (!tc || !tc->globals) return;

    static const char* names[] = {
        "ERR_INVALID_ARGUMENT",
        "ERR_PARSE",
        "ERR_PERMISSION",
        "ERR_IO",
        "ERR_LIMIT",
        "ERR_UNSUPPORTED",
        "ERR_NETWORK",
        "ERR_HTTP",
        "ERR_CRYPTO",
        "ERR_INTERNAL",
        NULL
    };

    for (int i = 0; names[i]; i++) {
        if (symbol_table_has(tc->globals, names[i])) continue;
        Type* t = type_int();
        Symbol* sym = symbol_create(t, names[i], false);
        symbol_table_add(tc->globals, sym);
    }
}

static size_t typechecker_record_base_name_len(const char* name) {
    if (!name) return 0;
    const char* bracket = strchr(name, '[');
    return bracket ? (size_t)(bracket - name) : strlen(name);
}

static bool typechecker_record_base_name_equals(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t a_len = typechecker_record_base_name_len(a);
    size_t b_len = typechecker_record_base_name_len(b);
    if (a_len != b_len) return false;
    return strncmp(a, b, a_len) == 0;
}

static Stmt* typechecker_find_generic_record_decl(TypeChecker* tc, const char* name) {
    if (!tc || !name) return NULL;
    for (int i = 0; i < tc->generic_record_decl_count; i++) {
        Stmt* stmt = tc->generic_record_decls[i].stmt;
        if (!stmt || stmt->kind != STMT_RECORD_DECL || !stmt->record_decl.name) continue;
        if (strcmp(stmt->record_decl.name, name) == 0) {
            return stmt;
        }
    }
    return NULL;
}

static Stmt* typechecker_find_generic_type_alias_decl(TypeChecker* tc, const char* name) {
    if (!tc || !name) return NULL;
    for (int i = 0; i < tc->generic_type_alias_decl_count; i++) {
        Stmt* stmt = tc->generic_type_alias_decls[i].stmt;
        if (!stmt || stmt->kind != STMT_TYPE_ALIAS || !stmt->type_alias.name) continue;
        if (strcmp(stmt->type_alias.name, name) == 0) {
            return stmt;
        }
    }
    return NULL;
}

static Stmt* typechecker_find_generic_enum_decl(TypeChecker* tc, const char* name) {
    if (!tc || !name) return NULL;
    for (int i = 0; i < tc->generic_enum_decl_count; i++) {
        Stmt* stmt = tc->generic_enum_decls[i].stmt;
        if (!stmt || stmt->kind != STMT_ENUM_DECL || !stmt->enum_decl.name) continue;
        if (strcmp(stmt->enum_decl.name, name) == 0) {
            return stmt;
        }
    }
    return NULL;
}

static void typechecker_register_generic_record_decl(TypeChecker* tc, Stmt* stmt) {
    if (!tc || !stmt || stmt->kind != STMT_RECORD_DECL) return;

    tc->generic_record_decl_count++;
    if (tc->generic_record_decl_count > tc->generic_record_decl_capacity) {
        tc->generic_record_decl_capacity = tc->generic_record_decl_count * 2;
        tc->generic_record_decls =
            (GenericRecordDeclEntry*)safe_realloc(tc->generic_record_decls,
                                                  (size_t)tc->generic_record_decl_capacity * sizeof(GenericRecordDeclEntry));
    }
    tc->generic_record_decls[tc->generic_record_decl_count - 1].stmt = stmt;
}

static void typechecker_register_generic_type_alias_decl(TypeChecker* tc, Stmt* stmt) {
    if (!tc || !stmt || stmt->kind != STMT_TYPE_ALIAS) return;

    tc->generic_type_alias_decl_count++;
    if (tc->generic_type_alias_decl_count > tc->generic_type_alias_decl_capacity) {
        tc->generic_type_alias_decl_capacity = tc->generic_type_alias_decl_count * 2;
        tc->generic_type_alias_decls =
            (GenericTypeAliasDeclEntry*)safe_realloc(tc->generic_type_alias_decls,
                                                     (size_t)tc->generic_type_alias_decl_capacity * sizeof(GenericTypeAliasDeclEntry));
    }
    tc->generic_type_alias_decls[tc->generic_type_alias_decl_count - 1].stmt = stmt;
}

static void typechecker_register_generic_enum_decl(TypeChecker* tc, Stmt* stmt) {
    if (!tc || !stmt || stmt->kind != STMT_ENUM_DECL) return;

    tc->generic_enum_decl_count++;
    if (tc->generic_enum_decl_count > tc->generic_enum_decl_capacity) {
        tc->generic_enum_decl_capacity = tc->generic_enum_decl_count * 2;
        tc->generic_enum_decls =
            (GenericEnumDeclEntry*)safe_realloc(tc->generic_enum_decls,
                                                (size_t)tc->generic_enum_decl_capacity * sizeof(GenericEnumDeclEntry));
    }
    tc->generic_enum_decls[tc->generic_enum_decl_count - 1].stmt = stmt;
}

static bool typechecker_type_contains_type_param(Type* type) {
    if (!type) return false;

    switch (type->kind) {
        case TYPE_TYPE_PARAM:
            return true;
        case TYPE_ARRAY:
            return typechecker_type_contains_type_param(type->element_type);
        case TYPE_FUTURE:
            return typechecker_type_contains_type_param(type->element_type);
        case TYPE_FUNCTION:
            if (typechecker_type_contains_type_param(type->return_type)) return true;
            for (int i = 0; i < type->param_count; i++) {
                if (typechecker_type_contains_type_param(type->param_types[i])) return true;
            }
            if (type->type_param_constraints) {
                for (int i = 0; i < type->type_param_count; i++) {
                    if (typechecker_type_contains_type_param(type->type_param_constraints[i])) return true;
                }
            }
            return false;
        case TYPE_TUPLE:
            for (int i = 0; i < tuple_type_get_arity(type); i++) {
                if (typechecker_type_contains_type_param(tuple_type_get_element(type, i))) return true;
            }
            return false;
        case TYPE_MAP:
            if (!type->map_def) return false;
            return typechecker_type_contains_type_param(type->map_def->key_type) ||
                   typechecker_type_contains_type_param(type->map_def->value_type);
        case TYPE_SET:
            if (!type->set_def) return false;
            return typechecker_type_contains_type_param(type->set_def->element_type);
        case TYPE_RECORD:
            for (int i = 0; i < type->param_count; i++) {
                if (typechecker_type_contains_type_param(type->param_types[i])) return true;
            }
            return false;
        default:
            return false;
    }
}

static void typechecker_append_text(char** buffer, size_t* len, size_t* capacity, const char* text) {
    if (!buffer || !len || !capacity || !text) return;

    size_t text_len = strlen(text);
    if (*capacity == 0) {
        *capacity = text_len + 32;
        *buffer = (char*)safe_malloc(*capacity);
        (*buffer)[0] = '\0';
        *len = 0;
    }

    if ((*len + text_len + 1) > *capacity) {
        while ((*len + text_len + 1) > *capacity) {
            *capacity *= 2;
        }
        *buffer = (char*)safe_realloc(*buffer, *capacity);
    }

    memcpy(*buffer + *len, text, text_len);
    *len += text_len;
    (*buffer)[*len] = '\0';
}

static char* typechecker_build_generic_record_key(const char* name, Type** type_args, int type_arg_count) {
    size_t len = 0;
    size_t capacity = 0;
    char* key = NULL;

    typechecker_append_text(&key, &len, &capacity, name ? name : "record");
    typechecker_append_text(&key, &len, &capacity, "[");
    for (int i = 0; i < type_arg_count; i++) {
        if (i > 0) typechecker_append_text(&key, &len, &capacity, ",");
        char type_buf[256];
        type_buf[0] = '\0';
        type_to_string(type_args ? type_args[i] : NULL, type_buf, sizeof(type_buf));
        typechecker_append_text(&key, &len, &capacity, type_buf[0] != '\0' ? type_buf : "any");
    }
    typechecker_append_text(&key, &len, &capacity, "]");
    return key;
}

static char* typechecker_build_generic_enum_key(const char* name, Type** type_args, int type_arg_count) {
    size_t len = 0;
    size_t capacity = 0;
    char* key = NULL;

    typechecker_append_text(&key, &len, &capacity, name ? name : "enum");
    typechecker_append_text(&key, &len, &capacity, "[");
    for (int i = 0; i < type_arg_count; i++) {
        if (i > 0) typechecker_append_text(&key, &len, &capacity, ",");
        char type_buf[256];
        type_buf[0] = '\0';
        type_to_string(type_args ? type_args[i] : NULL, type_buf, sizeof(type_buf));
        typechecker_append_text(&key, &len, &capacity, type_buf[0] != '\0' ? type_buf : "any");
    }
    typechecker_append_text(&key, &len, &capacity, "]");
    return key;
}

static GenericRecordInstanceEntry* typechecker_find_generic_record_instance(TypeChecker* tc, const char* key) {
    if (!tc || !key) return NULL;
    for (int i = 0; i < tc->generic_record_instance_count; i++) {
        GenericRecordInstanceEntry* inst = &tc->generic_record_instances[i];
        if (inst->key && strcmp(inst->key, key) == 0) {
            return inst;
        }
    }
    return NULL;
}

static GenericEnumInstanceEntry* typechecker_find_generic_enum_instance(TypeChecker* tc, const char* key) {
    if (!tc || !key) return NULL;
    for (int i = 0; i < tc->generic_enum_instance_count; i++) {
        GenericEnumInstanceEntry* inst = &tc->generic_enum_instances[i];
        if (inst->key && strcmp(inst->key, key) == 0) {
            return inst;
        }
    }
    return NULL;
}

static void typechecker_register_generic_enum_instance(TypeChecker* tc, const char* key, Type* type) {
    if (!tc || !key || !type) return;

    GenericEnumInstanceEntry* existing = typechecker_find_generic_enum_instance(tc, key);
    if (existing) {
        if (existing->type) type_free(existing->type);
        existing->type = type_clone(type);
        return;
    }

    tc->generic_enum_instance_count++;
    if (tc->generic_enum_instance_count > tc->generic_enum_instance_capacity) {
        tc->generic_enum_instance_capacity = tc->generic_enum_instance_count * 2;
        tc->generic_enum_instances =
            (GenericEnumInstanceEntry*)safe_realloc(tc->generic_enum_instances,
                                                    (size_t)tc->generic_enum_instance_capacity * sizeof(GenericEnumInstanceEntry));
    }

    GenericEnumInstanceEntry* slot = &tc->generic_enum_instances[tc->generic_enum_instance_count - 1];
    slot->key = safe_strdup(key);
    slot->type = type_clone(type);
}

static Type* typechecker_contextual_expected_type_for_inference(TypeChecker* tc, Type* expected_type) {
    if (!expected_type) return NULL;

    if (expected_type->kind == TYPE_INT &&
        expected_type->type_param_name &&
        expected_type->type_param_name[0] != '\0') {
        GenericEnumInstanceEntry* enum_inst =
            typechecker_find_generic_enum_instance(tc, expected_type->type_param_name);
        if (enum_inst && enum_inst->type) {
            return type_clone(enum_inst->type);
        }
    }

    return type_clone(expected_type);
}

static bool typechecker_has_unbound_generic_type_params(Type** bound_types, int type_param_count) {
    if (!bound_types || type_param_count <= 0) return false;
    for (int i = 0; i < type_param_count; i++) {
        if (!bound_types[i] || bound_types[i]->kind == TYPE_ANY) return true;
    }
    return false;
}

static void typechecker_seed_generic_bindings_from_enum_subject(TypeChecker* tc,
                                                                const char* subject_enum,
                                                                Type* member_fn_type,
                                                                Type** bound_types,
                                                                int type_param_count) {
    if (!tc || !subject_enum || !member_fn_type || !bound_types || type_param_count <= 0) return;
    if (member_fn_type->kind != TYPE_FUNCTION || !member_fn_type->type_param_names) return;

    GenericEnumInstanceEntry* enum_inst = typechecker_find_generic_enum_instance(tc, subject_enum);
    if (!enum_inst || !enum_inst->type) return;
    if (enum_inst->type->kind != TYPE_RECORD || !enum_inst->type->record_def || !enum_inst->type->record_def->name) {
        return;
    }

    Type* enum_return = member_fn_type->return_type;
    if (!enum_return || enum_return->kind != TYPE_RECORD || !enum_return->record_def || !enum_return->record_def->name) {
        return;
    }

    if (!typechecker_record_base_name_equals(enum_inst->type->record_def->name, enum_return->record_def->name)) {
        return;
    }

    if (enum_inst->type->param_count <= 0 || !enum_inst->type->param_types) return;

    int limit = type_param_count;
    if (enum_inst->type->param_count < limit) {
        limit = enum_inst->type->param_count;
    }

    for (int i = 0; i < limit; i++) {
        if (bound_types[i]) continue;
        Type* inst_arg = enum_inst->type->param_types[i];
        if (!inst_arg) continue;
        bound_types[i] = type_clone(inst_arg);
    }
}

static Type* typechecker_instantiate_generic_record(TypeChecker* tc,
                                                    Stmt* decl,
                                                    Type** type_args,
                                                    int type_arg_count,
                                                    bool nullable) {
    if (!tc || !decl || decl->kind != STMT_RECORD_DECL) return type_any();

    bool has_unresolved_type_param = false;
    for (int i = 0; i < type_arg_count; i++) {
        if (typechecker_type_contains_type_param(type_args ? type_args[i] : NULL)) {
            has_unresolved_type_param = true;
            break;
        }
    }

    if (has_unresolved_type_param) {
        Type* unresolved = type_record(decl->record_decl.name);
        Symbol* sym = symbol_table_get(tc->globals, decl->record_decl.name);
        if (sym && sym->type && sym->type->kind == TYPE_RECORD && sym->type->record_def &&
            unresolved && unresolved->record_def && unresolved->record_def != sym->type->record_def) {
            record_def_release(unresolved->record_def);
            unresolved->record_def = sym->type->record_def;
            record_def_retain(unresolved->record_def);
        }
        if (unresolved && type_arg_count > 0 && type_args) {
            unresolved->param_types = (Type**)safe_malloc((size_t)type_arg_count * sizeof(Type*));
            unresolved->param_count = type_arg_count;
            for (int i = 0; i < type_arg_count; i++) {
                unresolved->param_types[i] = type_clone(type_args[i]);
            }
        }
        if (unresolved) unresolved->nullable = nullable;
        return unresolved ? unresolved : type_any();
    }

    char* key = typechecker_build_generic_record_key(decl->record_decl.name, type_args, type_arg_count);
    GenericRecordInstanceEntry* existing = typechecker_find_generic_record_instance(tc, key);
    if (existing && existing->type) {
        Type* resolved = type_clone(existing->type);
        if (resolved) resolved->nullable = resolved->nullable || nullable;
        free(key);
        return resolved ? resolved : type_any();
    }

    Type* instance_type = type_record(key);
    if (instance_type && type_arg_count > 0 && type_args) {
        instance_type->param_types = (Type**)safe_malloc((size_t)type_arg_count * sizeof(Type*));
        instance_type->param_count = type_arg_count;
        for (int i = 0; i < type_arg_count; i++) {
            instance_type->param_types[i] = type_clone(type_args[i]);
        }
    }

    tc->generic_record_instance_count++;
    if (tc->generic_record_instance_count > tc->generic_record_instance_capacity) {
        tc->generic_record_instance_capacity = tc->generic_record_instance_count * 2;
        tc->generic_record_instances =
            (GenericRecordInstanceEntry*)safe_realloc(tc->generic_record_instances,
                                                      (size_t)tc->generic_record_instance_capacity * sizeof(GenericRecordInstanceEntry));
    }
    GenericRecordInstanceEntry* slot = &tc->generic_record_instances[tc->generic_record_instance_count - 1];
    slot->key = key;
    slot->type = instance_type;

    if (instance_type && instance_type->record_def) {
        for (int i = 0; i < decl->record_decl.field_count; i++) {
            bool unresolved = false;
            Type* substituted = typechecker_substitute_generic_type(decl->record_decl.field_types[i],
                                                                    decl->record_decl.type_params,
                                                                    type_args,
                                                                    decl->record_decl.type_param_count,
                                                                    &unresolved);
            (void)unresolved;
            Type* field_type = typechecker_resolve_type(tc, substituted);
            if (!field_type) field_type = type_any();
            record_def_add_field(instance_type->record_def, decl->record_decl.field_names[i], field_type);
            type_free(field_type);
        }
    }

    Type* out = type_clone(instance_type);
    if (out) out->nullable = out->nullable || nullable;
    return out ? out : type_any();
}

static Type* typechecker_instantiate_generic_type_alias(TypeChecker* tc,
                                                        Stmt* decl,
                                                        Type** type_args,
                                                        int type_arg_count,
                                                        bool nullable) {
    if (!tc || !decl || decl->kind != STMT_TYPE_ALIAS || !decl->type_alias.target_type) {
        Type* fallback = type_any();
        if (fallback) fallback->nullable = nullable;
        return fallback;
    }

    bool unresolved = false;
    Type* substituted = typechecker_substitute_generic_type(decl->type_alias.target_type,
                                                            decl->type_alias.type_params,
                                                            type_args,
                                                            type_arg_count,
                                                            &unresolved);
    (void)unresolved;
    Type* resolved = typechecker_resolve_type(tc, substituted);
    if (!resolved) {
        resolved = type_any();
    }
    if (resolved) resolved->nullable = resolved->nullable || nullable;
    return resolved ? resolved : type_any();
}

static Type* typechecker_resolve_type(TypeChecker* tc, Type* type) {
    if (!type) return NULL;

    switch (type->kind) {
        case TYPE_ARRAY:
            type->element_type = typechecker_resolve_type(tc, type->element_type);
            break;
        case TYPE_FUNCTION:
            type->return_type = typechecker_resolve_type(tc, type->return_type);
            for (int i = 0; i < type->param_count; i++) {
                type->param_types[i] = typechecker_resolve_type(tc, type->param_types[i]);
            }
            break;
        case TYPE_TUPLE:
            if (type->tuple_def) {
                for (int i = 0; i < type->tuple_def->element_count; i++) {
                    type->tuple_def->element_types[i] = typechecker_resolve_type(tc, type->tuple_def->element_types[i]);
                }
            }
            break;
        case TYPE_MAP:
            if (type->map_def) {
                type->map_def->key_type = typechecker_resolve_type(tc, type->map_def->key_type);
                type->map_def->value_type = typechecker_resolve_type(tc, type->map_def->value_type);
            }
            break;
        case TYPE_SET:
            if (type->set_def) {
                type->set_def->element_type = typechecker_resolve_type(tc, type->set_def->element_type);
            }
            break;
        case TYPE_RECORD:
            for (int i = 0; i < type->param_count; i++) {
                type->param_types[i] = typechecker_resolve_type(tc, type->param_types[i]);
            }

            if (type->record_def && type->record_def->name) {
                if (strcmp(type->record_def->name, "Future") == 0) {
                    if (type->param_count != 1) {
                        typechecker_error(tc,
                                          "Future type requires exactly one type argument",
                                          tc->current_file,
                                          0,
                                          0);
                        Type* fallback = type_any();
                        if (fallback) fallback->nullable = type->nullable;
                        type_free(type);
                        type = fallback ? fallback : type_any();
                        break;
                    }

                    bool nullable = type->nullable;
                    Type* value_type = (type->param_types && type->param_types[0])
                        ? type_clone(type->param_types[0])
                        : type_any();
                    type_free(type);
                    type = type_future(value_type ? value_type : type_any());
                    if (type) type->nullable = nullable;
                    break;
                }

                Symbol* sym = symbol_table_get(tc->globals, type->record_def->name);
                if (sym && !symbol_is_accessible_from_file(sym, tc->current_file)) {
                    char message[512];
                    snprintf(message, sizeof(message), "Symbol '%s' is private to its module", type->record_def->name);
                    typechecker_error(tc, message, tc->current_file, 0, 0);
                    break;
                }

                Stmt* generic_enum_decl = typechecker_find_generic_enum_decl(tc, type->record_def->name);
                if (generic_enum_decl) {
                    int expected = generic_enum_decl->enum_decl.type_param_count;
                    if (type->param_count != expected) {
                        const char* enum_name =
                            generic_enum_decl->enum_decl.name ? generic_enum_decl->enum_decl.name : "<enum>";
                        char declared_params[160];
                        char declared_signature[224];
                        char used_signature[224];
                        typechecker_format_generic_type_param_list(generic_enum_decl->enum_decl.type_params,
                                                                   expected,
                                                                   declared_params,
                                                                   sizeof(declared_params));
                        snprintf(declared_signature,
                                 sizeof(declared_signature),
                                 "%.*s%.*s",
                                 96,
                                 enum_name,
                                 80,
                                 declared_params);
                        typechecker_format_named_type_with_type_args(enum_name,
                                                                     type->param_types,
                                                                     type->param_count,
                                                                     used_signature,
                                                                     sizeof(used_signature));
                        char message[512];
                        snprintf(message,
                                 sizeof(message),
                                 "Wrong number of type arguments for '%.*s': expected %d, got %d; declared as %.*s; used as %.*s",
                                 96,
                                 enum_name,
                                 expected,
                                 type->param_count,
                                 150,
                                 declared_signature,
                                 150,
                                 used_signature);
                        typechecker_error(tc, message, tc->current_file, 0, 0);
                        Type* fallback = type_any();
                        if (fallback) fallback->nullable = type->nullable;
                        type_free(type);
                        type = fallback ? fallback : type_any();
                        break;
                    }

                    bool nullable = type->nullable;
                    Type* resolved_enum_type = type_int();
                    char* enum_key = typechecker_build_generic_enum_key(generic_enum_decl->enum_decl.name,
                                                                        type->param_types,
                                                                        type->param_count);
                    if (enum_key) {
                        typechecker_register_generic_enum_instance(tc, enum_key, type);
                    }
                    if (resolved_enum_type) {
                        resolved_enum_type->type_param_name = enum_key ? enum_key : safe_strdup(generic_enum_decl->enum_decl.name);
                        resolved_enum_type->nullable = nullable;
                    } else if (enum_key) {
                        free(enum_key);
                    }

                    type_free(type);
                    type = resolved_enum_type ? resolved_enum_type : type_any();
                    break;
                }

                Stmt* generic_alias_decl = typechecker_find_generic_type_alias_decl(tc, type->record_def->name);
                if (generic_alias_decl) {
                    int expected = generic_alias_decl->type_alias.type_param_count;
                    if (type->param_count != expected) {
                        const char* alias_name =
                            generic_alias_decl->type_alias.name ? generic_alias_decl->type_alias.name : "<alias>";
                        char declared_params[160];
                        char declared_signature[224];
                        char used_signature[224];
                        typechecker_format_generic_type_param_list(generic_alias_decl->type_alias.type_params,
                                                                   expected,
                                                                   declared_params,
                                                                   sizeof(declared_params));
                        snprintf(declared_signature,
                                 sizeof(declared_signature),
                                 "%.*s%.*s",
                                 96,
                                 alias_name,
                                 80,
                                 declared_params);
                        typechecker_format_named_type_with_type_args(alias_name,
                                                                     type->param_types,
                                                                     type->param_count,
                                                                     used_signature,
                                                                     sizeof(used_signature));
                        char message[512];
                        snprintf(message,
                                 sizeof(message),
                                 "Wrong number of type arguments for '%.*s': expected %d, got %d; declared as %.*s; used as %.*s",
                                 96,
                                 alias_name,
                                 expected,
                                 type->param_count,
                                 150,
                                 declared_signature,
                                 150,
                                 used_signature);
                        typechecker_error(tc, message, tc->current_file, 0, 0);
                        Type* fallback = type_any();
                        if (fallback) fallback->nullable = type->nullable;
                        type_free(type);
                        type = fallback ? fallback : type_any();
                        break;
                    }

                    bool nullable = type->nullable;
                    Type* resolved = typechecker_instantiate_generic_type_alias(tc,
                                                                                generic_alias_decl,
                                                                                type->param_types,
                                                                                type->param_count,
                                                                                nullable);
                    type_free(type);
                    type = resolved ? resolved : type_any();
                    break;
                }

                Stmt* generic_record_decl = typechecker_find_generic_record_decl(tc, type->record_def->name);
                if (generic_record_decl) {
                    int expected = generic_record_decl->record_decl.type_param_count;
                    if (type->param_count != expected) {
                        const char* record_name =
                            generic_record_decl->record_decl.name ? generic_record_decl->record_decl.name : "<record>";
                        char declared_params[160];
                        char declared_signature[224];
                        char used_signature[224];
                        typechecker_format_generic_type_param_list(generic_record_decl->record_decl.type_params,
                                                                   expected,
                                                                   declared_params,
                                                                   sizeof(declared_params));
                        snprintf(declared_signature,
                                 sizeof(declared_signature),
                                 "%.*s%.*s",
                                 96,
                                 record_name,
                                 80,
                                 declared_params);
                        typechecker_format_named_type_with_type_args(record_name,
                                                                     type->param_types,
                                                                     type->param_count,
                                                                     used_signature,
                                                                     sizeof(used_signature));
                        char message[512];
                        snprintf(message,
                                 sizeof(message),
                                 "Wrong number of type arguments for '%.*s': expected %d, got %d; declared as %.*s; used as %.*s",
                                 96,
                                 record_name,
                                 expected,
                                 type->param_count,
                                 150,
                                 declared_signature,
                                 150,
                                 used_signature);
                        typechecker_error(tc, message, tc->current_file, 0, 0);
                        Type* fallback = type_any();
                        if (fallback) fallback->nullable = type->nullable;
                        type_free(type);
                        type = fallback ? fallback : type_any();
                        break;
                    }

                    bool nullable = type->nullable;
                    Type* resolved = typechecker_instantiate_generic_record(tc,
                                                                            generic_record_decl,
                                                                            type->param_types,
                                                                            type->param_count,
                                                                            nullable);
                    type_free(type);
                    type = resolved ? resolved : type_any();
                    break;
                }

                if (type->param_count > 0 &&
                    sym &&
                    ((sym->is_type_alias && !typechecker_find_generic_type_alias_decl(tc, type->record_def->name)) ||
                     (sym->type && sym->type->kind == TYPE_RECORD &&
                      !typechecker_find_generic_record_decl(tc, type->record_def->name)))) {
                    char used_signature[224];
                    const char* type_name = type->record_def->name ? type->record_def->name : "<type>";
                    typechecker_format_named_type_with_type_args(type_name,
                                                                 type->param_types,
                                                                 type->param_count,
                                                                 used_signature,
                                                                 sizeof(used_signature));
                    char message[512];
                    snprintf(message,
                             sizeof(message),
                             "Type '%.*s' is not generic; remove type arguments (used as %.*s)",
                             96,
                             type_name,
                             150,
                             used_signature);
                    typechecker_error(tc, message, tc->current_file, 0, 0);
                    Type* fallback = type_any();
                    if (fallback) fallback->nullable = type->nullable;
                    type_free(type);
                    type = fallback ? fallback : type_any();
                    break;
                }

                if (sym && sym->type && sym->type->kind == TYPE_INTERFACE) {
                    bool nullable = type->nullable;
                    Type* resolved = type_clone(sym->type);
                    if (resolved) {
                        resolved->nullable = resolved->nullable || nullable;
                        type_free(type);
                        type = resolved;
                    }
                } else if (sym && sym->is_type_alias && sym->type) {
                    bool nullable = type->nullable;
                    Type* resolved = type_clone(sym->type);
                    if (resolved) {
                        resolved->nullable = resolved->nullable || nullable;
                        type_free(type);
                        type = resolved;
                    }
                } else if (sym && sym->type && sym->type->kind == TYPE_RECORD && sym->type->record_def) {
                    if (type->record_def != sym->type->record_def) {
                        record_def_release(type->record_def);
                        type->record_def = sym->type->record_def;
                        record_def_retain(type->record_def);
                    }
                }
            }
            break;
        default:
            break;
    }

    return type;
}

static void typechecker_predeclare_record(TypeChecker* tc, Stmt* stmt) {
    if (!stmt || stmt->kind != STMT_RECORD_DECL) return;

    for (int i = 0; i < stmt->record_decl.type_param_count; i++) {
        const char* type_param_name = stmt->record_decl.type_params ? stmt->record_decl.type_params[i] : NULL;
        if (!type_param_name || type_param_name[0] == '\0') {
            typechecker_error(tc, "Invalid generic type parameter name", stmt->file, stmt->line, 0);
            return;
        }
        if (is_builtin_name(type_param_name)) {
            typechecker_error(tc, "Generic type parameter cannot use a built-in name", stmt->file, stmt->line, 0);
            return;
        }
        for (int j = i + 1; j < stmt->record_decl.type_param_count; j++) {
            const char* other = stmt->record_decl.type_params ? stmt->record_decl.type_params[j] : NULL;
            if (other && strcmp(type_param_name, other) == 0) {
                typechecker_error(tc, "Duplicate generic type parameter name", stmt->file, stmt->line, 0);
                return;
            }
        }
    }

    if (symbol_table_has(tc->globals, stmt->record_decl.name)) {
        typechecker_error(tc, "Record type already declared", stmt->file, stmt->line, 0);
        return;
    }

    Type* record_type = type_record(stmt->record_decl.name);
    Symbol* sym = symbol_create(record_type, stmt->record_decl.name, false);
    symbol_set_visibility_metadata(sym, stmt->file, stmt->is_public);
    symbol_table_add(tc->globals, sym);

    if (stmt->record_decl.type_param_count > 0) {
        typechecker_register_generic_record_decl(tc, stmt);
    }
}

static void typechecker_predeclare_interface(TypeChecker* tc, Stmt* stmt) {
    if (!stmt || stmt->kind != STMT_INTERFACE_DECL) return;

    if (is_builtin_name(stmt->interface_decl.name)) {
        typechecker_error(tc, "Cannot declare an interface with a built-in name", stmt->file, stmt->line, 0);
        return;
    }

    if (symbol_table_has(tc->globals, stmt->interface_decl.name)) {
        typechecker_error(tc, "Interface type already declared", stmt->file, stmt->line, 0);
        return;
    }

    Type* interface_type = type_interface(stmt->interface_decl.name);
    Symbol* sym = symbol_create(interface_type, stmt->interface_decl.name, false);
    symbol_set_visibility_metadata(sym, stmt->file, stmt->is_public);
    symbol_table_add(tc->globals, sym);
}

static void typechecker_impl_mapping_free(InterfaceImplMapping* mapping) {
    if (!mapping) return;

    if (mapping->interface_name) free(mapping->interface_name);
    if (mapping->record_name) free(mapping->record_name);
    for (int i = 0; i < mapping->method_count; i++) {
        if (mapping->method_names) free(mapping->method_names[i]);
        if (mapping->function_names) free(mapping->function_names[i]);
    }
    if (mapping->method_names) free(mapping->method_names);
    if (mapping->function_names) free(mapping->function_names);

    mapping->interface_name = NULL;
    mapping->record_name = NULL;
    mapping->method_names = NULL;
    mapping->function_names = NULL;
    mapping->method_count = 0;
    mapping->decl_file = NULL;
    mapping->decl_line = 0;
    mapping->decl_column = 0;
}

static InterfaceImplMapping* typechecker_find_impl_mapping_exact(TypeChecker* tc, const char* interface_name, const char* record_name) {
    if (!tc || !interface_name || !record_name) return NULL;

    for (int i = 0; i < tc->impl_mapping_count; i++) {
        InterfaceImplMapping* mapping = &tc->impl_mappings[i];
        if (!mapping->interface_name || !mapping->record_name) continue;
        if (strcmp(mapping->interface_name, interface_name) == 0 &&
            strcmp(mapping->record_name, record_name) == 0) {
            return mapping;
        }
    }

    return NULL;
}

static InterfaceImplMapping* typechecker_find_impl_mapping(TypeChecker* tc, const char* interface_name, const char* record_name) {
    if (!tc || !interface_name || !record_name) return NULL;

    InterfaceImplMapping* exact = typechecker_find_impl_mapping_exact(tc, interface_name, record_name);
    if (exact) return exact;

    // Generic fallback: allow impl entries declared for the base record name
    // (e.g., "Box") to apply to concrete specializations like "Box[int]".
    for (int i = 0; i < tc->impl_mapping_count; i++) {
        InterfaceImplMapping* mapping = &tc->impl_mappings[i];
        if (!mapping->interface_name || !mapping->record_name) continue;
        if (strcmp(mapping->interface_name, interface_name) != 0) continue;
        if (strchr(mapping->record_name, '[') != NULL) continue;
        if (typechecker_record_base_name_equals(mapping->record_name, record_name)) {
            return mapping;
        }
    }

    return NULL;
}

static const char* typechecker_impl_lookup_function(const InterfaceImplMapping* mapping, const char* method_name) {
    if (!mapping || !method_name) return NULL;

    for (int i = 0; i < mapping->method_count; i++) {
        if (mapping->method_names &&
            mapping->method_names[i] &&
            strcmp(mapping->method_names[i], method_name) == 0) {
            return mapping->function_names ? mapping->function_names[i] : NULL;
        }
    }

    return NULL;
}

static const char* typechecker_impl_lookup_method(const InterfaceImplMapping* mapping, const char* function_name) {
    if (!mapping || !function_name) return NULL;

    for (int i = 0; i < mapping->method_count; i++) {
        if (mapping->function_names &&
            mapping->function_names[i] &&
            strcmp(mapping->function_names[i], function_name) == 0) {
            return mapping->method_names ? mapping->method_names[i] : NULL;
        }
    }

    return NULL;
}

static void typechecker_predeclare_impl(TypeChecker* tc, Stmt* stmt) {
    if (!stmt || stmt->kind != STMT_IMPL_DECL) return;

    if (!stmt->impl_decl.interface_name || !stmt->impl_decl.record_name) {
        typechecker_error(tc, "Invalid impl declaration", stmt->file, stmt->line, 0);
        return;
    }

    if (typechecker_find_impl_mapping_exact(tc, stmt->impl_decl.interface_name, stmt->impl_decl.record_name)) {
        typechecker_error(tc, "impl already declared for this interface and record", stmt->file, stmt->line, 0);
        return;
    }

    for (int i = 0; i < stmt->impl_decl.method_count; i++) {
        const char* method_name = stmt->impl_decl.method_names ? stmt->impl_decl.method_names[i] : NULL;
        const char* function_name = stmt->impl_decl.function_names ? stmt->impl_decl.function_names[i] : NULL;
        if (!method_name || !function_name) {
            typechecker_error(tc, "Invalid impl method mapping", stmt->file, stmt->line, 0);
            return;
        }

        for (int j = i + 1; j < stmt->impl_decl.method_count; j++) {
            const char* other_method = stmt->impl_decl.method_names ? stmt->impl_decl.method_names[j] : NULL;
            if (other_method && strcmp(method_name, other_method) == 0) {
                typechecker_error(tc, "Duplicate method mapping in impl declaration", stmt->file, stmt->line, 0);
                return;
            }
        }
    }

    tc->impl_mapping_count++;
    if (tc->impl_mapping_count > tc->impl_mapping_capacity) {
        tc->impl_mapping_capacity = tc->impl_mapping_count * 2;
        tc->impl_mappings = (InterfaceImplMapping*)safe_realloc(tc->impl_mappings,
                                                                tc->impl_mapping_capacity * sizeof(InterfaceImplMapping));
    }

    InterfaceImplMapping* mapping = &tc->impl_mappings[tc->impl_mapping_count - 1];
    mapping->interface_name = safe_strdup(stmt->impl_decl.interface_name);
    mapping->record_name = safe_strdup(stmt->impl_decl.record_name);
    mapping->method_count = stmt->impl_decl.method_count;
    mapping->decl_file = stmt->file;
    mapping->decl_line = stmt->line;
    mapping->decl_column = stmt->column;
    mapping->method_names = NULL;
    mapping->function_names = NULL;

    if (mapping->method_count > 0) {
        mapping->method_names = (char**)safe_malloc((size_t)mapping->method_count * sizeof(char*));
        mapping->function_names = (char**)safe_malloc((size_t)mapping->method_count * sizeof(char*));
        for (int i = 0; i < mapping->method_count; i++) {
            mapping->method_names[i] = safe_strdup(stmt->impl_decl.method_names[i]);
            mapping->function_names[i] = safe_strdup(stmt->impl_decl.function_names[i]);
        }
    }
}

static void typechecker_predeclare_type_alias(TypeChecker* tc, Stmt* stmt) {
    if (!stmt || stmt->kind != STMT_TYPE_ALIAS) return;

    for (int i = 0; i < stmt->type_alias.type_param_count; i++) {
        const char* type_param_name = stmt->type_alias.type_params ? stmt->type_alias.type_params[i] : NULL;
        if (!type_param_name || type_param_name[0] == '\0') {
            typechecker_error(tc, "Invalid generic type parameter name", stmt->file, stmt->line, 0);
            return;
        }
        if (is_builtin_name(type_param_name)) {
            typechecker_error(tc, "Generic type parameter cannot use a built-in name", stmt->file, stmt->line, 0);
            return;
        }
        for (int j = i + 1; j < stmt->type_alias.type_param_count; j++) {
            const char* other = stmt->type_alias.type_params ? stmt->type_alias.type_params[j] : NULL;
            if (other && strcmp(type_param_name, other) == 0) {
                typechecker_error(tc, "Duplicate generic type parameter name", stmt->file, stmt->line, 0);
                return;
            }
        }
    }

    if (is_builtin_name(stmt->type_alias.name)) {
        typechecker_error(tc, "Cannot declare a type alias with a built-in name", stmt->file, stmt->line, 0);
        return;
    }

    if (symbol_table_has(tc->globals, stmt->type_alias.name)) {
        typechecker_error(tc, "Type alias already declared", stmt->file, stmt->line, 0);
        return;
    }

    Type* alias_type = NULL;
    if (stmt->type_alias.type_param_count > 0) {
        alias_type = type_any();
    } else {
        alias_type = stmt->type_alias.target_type ? type_clone(stmt->type_alias.target_type) : type_any();
    }
    Symbol* sym = symbol_create(alias_type, stmt->type_alias.name, false);
    sym->is_type_alias = true;
    symbol_set_visibility_metadata(sym, stmt->file, stmt->is_public);
    symbol_table_add(tc->globals, sym);

    if (stmt->type_alias.type_param_count > 0) {
        typechecker_register_generic_type_alias_decl(tc, stmt);
    }
}

static char* enum_member_symbol_name(const char* enum_name, const char* member_name) {
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

static void typechecker_predeclare_enum(TypeChecker* tc, Stmt* stmt) {
    if (!stmt || stmt->kind != STMT_ENUM_DECL) return;

    for (int i = 0; i < stmt->enum_decl.type_param_count; i++) {
        const char* type_param_name = stmt->enum_decl.type_params ? stmt->enum_decl.type_params[i] : NULL;
        if (!type_param_name || type_param_name[0] == '\0') {
            typechecker_error(tc, "Invalid generic type parameter name", stmt->file, stmt->line, 0);
            return;
        }
        if (is_builtin_name(type_param_name)) {
            typechecker_error(tc, "Generic type parameter cannot use a built-in name", stmt->file, stmt->line, 0);
            return;
        }
        for (int j = i + 1; j < stmt->enum_decl.type_param_count; j++) {
            const char* other = stmt->enum_decl.type_params ? stmt->enum_decl.type_params[j] : NULL;
            if (other && strcmp(type_param_name, other) == 0) {
                typechecker_error(tc, "Duplicate generic type parameter name", stmt->file, stmt->line, 0);
                return;
            }
        }
    }

    if (is_builtin_name(stmt->enum_decl.name)) {
        typechecker_error(tc, "Cannot declare an enum with a built-in name", stmt->file, stmt->line, 0);
        return;
    }

    if (symbol_table_has(tc->globals, stmt->enum_decl.name)) {
        typechecker_error(tc, "Enum already declared", stmt->file, stmt->line, 0);
        return;
    }

    bool is_generic_enum = stmt->enum_decl.type_param_count > 0;
    Type* alias_type = NULL;
    if (is_generic_enum) {
        alias_type = type_any();
    } else {
        alias_type = type_int();
        alias_type->type_param_name = safe_strdup(stmt->enum_decl.name);
    }
    Symbol* enum_sym = symbol_create(alias_type, stmt->enum_decl.name, false);
    enum_sym->is_type_alias = true;
    symbol_set_visibility_metadata(enum_sym, stmt->file, stmt->is_public);
    symbol_table_add(tc->globals, enum_sym);

    if (is_generic_enum) {
        typechecker_register_generic_enum_decl(tc, stmt);
    }

    char** old_type_param_names = tc->current_type_param_names;
    Type** old_type_param_constraints = tc->current_type_param_constraints;
    int old_type_param_count = tc->current_type_param_count;
    if (is_generic_enum) {
        tc->current_type_param_names = stmt->enum_decl.type_params;
        tc->current_type_param_constraints = NULL;
        tc->current_type_param_count = stmt->enum_decl.type_param_count;
    }

    for (int i = 0; i < stmt->enum_decl.member_count; i++) {
        const char* member_name = stmt->enum_decl.member_names[i];
        if (!member_name) continue;

        char* symbol_name = enum_member_symbol_name(stmt->enum_decl.name, member_name);
        if (!symbol_name) continue;

        if (is_builtin_name(symbol_name)) {
            typechecker_error(tc, "Enum member conflicts with a built-in name", stmt->file, stmt->line, 0);
            free(symbol_name);
            continue;
        }

        if (symbol_table_has(tc->globals, symbol_name)) {
            typechecker_error(tc, "Enum member already declared", stmt->file, stmt->line, 0);
            free(symbol_name);
            continue;
        }

        int payload_count = (stmt->enum_decl.member_payload_counts &&
                             i < stmt->enum_decl.member_count)
                                ? stmt->enum_decl.member_payload_counts[i]
                                : 0;

        Type** ctor_param_types = NULL;
        if (payload_count > 0 && stmt->enum_decl.member_payload_types &&
            stmt->enum_decl.member_payload_types[i]) {
            ctor_param_types = (Type**)safe_malloc((size_t)payload_count * sizeof(Type*));
            for (int j = 0; j < payload_count; j++) {
                Type* payload_type = stmt->enum_decl.member_payload_types[i][j];
                Type* resolved_payload_type = typechecker_resolve_type(tc, payload_type);
                stmt->enum_decl.member_payload_types[i][j] = resolved_payload_type;
                ctor_param_types[j] = resolved_payload_type ? type_clone(resolved_payload_type) : type_any();
            }
        }

        Type* member_type = NULL;
        if (is_generic_enum) {
            Type* ctor_return_type = type_record(stmt->enum_decl.name);
            if (ctor_return_type && stmt->enum_decl.type_param_count > 0) {
                ctor_return_type->param_count = stmt->enum_decl.type_param_count;
                ctor_return_type->param_types = (Type**)safe_malloc((size_t)ctor_return_type->param_count * sizeof(Type*));
                for (int j = 0; j < ctor_return_type->param_count; j++) {
                    const char* param_name = stmt->enum_decl.type_params ? stmt->enum_decl.type_params[j] : NULL;
                    ctor_return_type->param_types[j] = type_type_param(param_name ? param_name : "T");
                }
            }

            member_type = type_function(ctor_return_type, ctor_param_types, payload_count);
            type_function_set_type_params(member_type,
                                          stmt->enum_decl.type_params,
                                          NULL,
                                          stmt->enum_decl.type_param_count);
        } else if (payload_count > 0) {
            Type* ctor_return_type = type_int();
            ctor_return_type->type_param_name = safe_strdup(stmt->enum_decl.name);
            member_type = type_function(ctor_return_type, ctor_param_types, payload_count);
        } else {
            member_type = type_int();
            member_type->type_param_name = safe_strdup(stmt->enum_decl.name);
            if (ctor_param_types) {
                for (int j = 0; j < payload_count; j++) {
                    if (ctor_param_types[j]) type_free(ctor_param_types[j]);
                }
                free(ctor_param_types);
            }
        }
        Symbol* member_sym = symbol_create(member_type, symbol_name, false);
        symbol_set_visibility_metadata(member_sym, stmt->file, stmt->is_public);
        symbol_table_add(tc->globals, member_sym);
        free(symbol_name);
    }

    tc->current_type_param_names = old_type_param_names;
    tc->current_type_param_constraints = old_type_param_constraints;
    tc->current_type_param_count = old_type_param_count;
}

static bool is_builtin_name(const char* name) {
    if (!name) return false;

    static const char* builtins[] = {
        "print", "println", "panic", "must", "wrapError", "len", "str", "formatDouble", "toInt", "toDouble", "toBigInt", "toHexBigInt", "bytesToHex", "hexToBytes", "typeOf",
        "futurePending", "futureResolved", "futureIsReady", "futureComplete", "futureGet",
        "extPostedCallbackPendingCount", "extDrainPostedCallbacks", "extSetPostedCallbackAutoDrain", "asyncSleep",
        "asyncChannelSend", "asyncChannelSendTyped", "asyncChannelRecv", "asyncChannelRecvTyped",
        "stringToBytes", "bytesToString", "sha256Bytes", "hmacSha256Bytes", "pbkdf2HmacSha256Bytes", "hkdfHmacSha256Bytes", "constantTimeBytesEqual", "aesCtrBytes", "aesGcmSealBytes", "aesGcmOpenBytes", "bytesJoin", "urlEncode", "urlDecode",
        "jsonParse", "jsonStringify", "jsonStringifyPretty", "jsonDecode",
        "substring", "find", "split", "trim", "startsWith", "endsWith", "replace",
        "absBigInt", "signBigInt", "digitsBigInt", "isEvenBigInt", "isOddBigInt",
        "powBigInt", "gcdBigInt", "lcmBigInt", "modPowBigInt", "modInverseBigInt", "isProbablePrimeBigInt",
        "compareBigInt", "absCmpBigInt", "clampBigInt", "isZeroBigInt", "isNegativeBigInt",
        "absInt", "absDouble", "min", "max", "floor", "ceil", "round", "sqrt", "pow",
        "arrayWithSize", "bytesWithSize", "push", "pop", "keys", "values",
        "copyInto", "reversePrefix", "rotatePrefixLeft", "rotatePrefixRight",
        "read_line", "read_all", "write_line", "write_all",
        "file_open", "file_read_line", "file_close", "ioReadLine", "ioReadAll", "ioReadChunk", "ioReadChunkBytes", "ioReadExactlyBytes",
        "ioWriteAll", "ioWriteBytesAll", "ioCopy",
        "readBytes", "writeBytes", "appendBytes", "stdoutWriteBytes", "envGet", "exists", "delete",
        "mapGet", "mapSet", "mapHas", "mapGetString", "mapSetString", "mapHasString", "mapDeleteString", "mapDelete", "mapCount",
        "setAdd", "setAddString", "setHas", "setHasString", "setRemove", "setRemoveString", "setCount", "setToArray",
        "sort", "reverse", "findArray", "contains", "slice", "join",
        "httpGet", "httpGetWithHeaders", "httpPost", "httpPostWithHeaders",
        "httpRequest", "httpRequestHead", "httpRequestWithOptions", "httpRequestHeadWithOptions",
        "httpReadRequest", "httpWriteResponse",
        "tcpListen", "tcpAccept", "tcpConnect", "tcpSend", "tcpReceive", "tcpClose",
        "tlsIsAvailable", "tlsConnect", "tlsSend", "tlsReceive", "tlsClose",
        "syncChannelCreate", "syncChannelSend", "syncChannelSendTyped", "syncChannelRecv", "syncChannelRecvTyped", "syncChannelClose",
        "syncSharedCreate", "syncSharedCreateTyped", "syncSharedGet", "syncSharedGetTyped", "syncSharedSet", "syncSharedSetTyped",
        "syncThreadSpawn", "syncThreadSpawnTyped", "syncThreadJoin", "syncThreadJoinTyped", "syncThreadInbox", "syncThreadOutbox", "syncThreadArgTyped",
        "syncArcCreate", "syncArcClone", "syncArcGuardAcquire", "syncArcGuardRead", "syncArcGuardWrite", "syncArcGuardRelease",
        "sqliteIsAvailable", "sqliteOpen", "sqliteClose", "sqliteExec", "sqliteQuery", "sqlitePrepare",
        "sqliteBindInt", "sqliteBindDouble", "sqliteBindString", "sqliteBindBytes", "sqliteBindNull",
        "sqliteReset", "sqliteClearBindings", "sqliteChanges", "sqliteLastInsertRowId",
        "sqliteStep", "sqliteFinalize",
        "processSpawn", "processWriteStdin", "processCloseStdin", "processReadStdout", "processReadStderr", "processWait", "processKill",
        "timeNowMillis", "timeNowNanos", "timeMonotonicMillis", "timeSinceMillis", "utcDateTime", "localDateTime",
        "logJson",
        "random", "randomSeed", "randomInt", "randomDouble", "randomBigIntBits", "randomBigIntRange",
        "randomFillInt", "randomFillDouble", "randomFillBigIntBits", "randomFillBigIntRange",
        "secureRandom", "secureRandomInt", "secureRandomDouble", "secureRandomBigIntBits", "secureRandomBigIntRange",
        "secureRandomFillInt", "secureRandomFillDouble", "secureRandomFillBigIntBits", "secureRandomFillBigIntRange",
        "ERR_INVALID_ARGUMENT", "ERR_PARSE", "ERR_PERMISSION", "ERR_IO", "ERR_LIMIT",
        "ERR_UNSUPPORTED", "ERR_NETWORK", "ERR_HTTP", "ERR_CRYPTO", "ERR_INTERNAL",
        "argv",
        NULL
    };

    for (int i = 0; builtins[i]; i++) {
        if (strcmp(name, builtins[i]) == 0) return true;
    }
    return false;
}

static Type* type_error_nullable(TypeChecker* tc) {
    if (tc && tc->globals) {
        Symbol* sym = symbol_table_get(tc->globals, "Error");
        if (sym && sym->type && sym->type->kind == TYPE_RECORD) {
            Type* t = type_clone(sym->type);
            if (t) t->nullable = true;
            return t ? t : type_any();
        }
    }

    Type* t = type_record("Error");
    if (t) t->nullable = true;
    return t ? t : type_any();
}

static bool expr_is_empty_map_literal(const Expr* expr) {
    return expr &&
           expr->kind == EXPR_MAP_LITERAL &&
           expr->map_literal.entry_count == 0;
}

static bool expr_is_compile_time_constant(const Expr* expr) {
    if (!expr) return false;

    switch (expr->kind) {
        case EXPR_LITERAL:
        case EXPR_NIL:
        case EXPR_FUNC_LITERAL:
            return true;
        case EXPR_UNARY:
            if (!expr->unary.operand || !expr_is_compile_time_constant(expr->unary.operand)) {
                return false;
            }
            switch (expr->unary.op) {
                case TOKEN_MINUS:
                case TOKEN_NOT:
                case TOKEN_BIT_NOT:
                    return true;
                default:
                    return false;
            }
        case EXPR_BINARY:
            if (!expr->binary.left || !expr->binary.right) return false;
            if (!expr_is_compile_time_constant(expr->binary.left) ||
                !expr_is_compile_time_constant(expr->binary.right)) {
                return false;
            }
            switch (expr->binary.op) {
                case TOKEN_PLUS:
                case TOKEN_MINUS:
                case TOKEN_STAR:
                case TOKEN_SLASH:
                case TOKEN_PERCENT:
                case TOKEN_EQ_EQ:
                case TOKEN_BANG_EQ:
                case TOKEN_LT:
                case TOKEN_LT_EQ:
                case TOKEN_GT:
                case TOKEN_GT_EQ:
                case TOKEN_AND:
                case TOKEN_OR:
                case TOKEN_BIT_AND:
                case TOKEN_BIT_OR:
                case TOKEN_BIT_XOR:
                    return true;
                default:
                    return false;
            }
        case EXPR_CAST:
            return expr->cast.value &&
                   expr->cast.target_type &&
                   expr_is_compile_time_constant(expr->cast.value);
        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->array_literal.element_count; i++) {
                if (!expr_is_compile_time_constant(expr->array_literal.elements[i])) {
                    return false;
                }
            }
            return true;
        case EXPR_RECORD_LITERAL:
            for (int i = 0; i < expr->record_literal.field_count; i++) {
                if (!expr_is_compile_time_constant(expr->record_literal.field_values[i])) {
                    return false;
                }
            }
            return true;
        case EXPR_TUPLE_LITERAL:
            for (int i = 0; i < expr->tuple_literal.element_count; i++) {
                if (!expr_is_compile_time_constant(expr->tuple_literal.elements[i])) {
                    return false;
                }
            }
            return true;
        case EXPR_MAP_LITERAL:
            for (int i = 0; i < expr->map_literal.entry_count; i++) {
                if (!expr_is_compile_time_constant(expr->map_literal.keys[i]) ||
                    !expr_is_compile_time_constant(expr->map_literal.values[i])) {
                    return false;
                }
            }
            return true;
        case EXPR_SET_LITERAL:
            for (int i = 0; i < expr->set_literal.element_count; i++) {
                if (!expr_is_compile_time_constant(expr->set_literal.elements[i])) {
                    return false;
                }
            }
            return true;
        default:
            return false;
    }
}

static void expr_coerce_empty_map_literal_to_empty_set_literal(Expr* expr) {
    if (!expr_is_empty_map_literal(expr)) return;

    if (expr->type) {
        type_free(expr->type);
        expr->type = NULL;
    }

    if (expr->map_literal.map_type) {
        type_free(expr->map_literal.map_type);
        expr->map_literal.map_type = NULL;
    }

    if (expr->map_literal.keys) {
        free(expr->map_literal.keys);
        expr->map_literal.keys = NULL;
    }

    if (expr->map_literal.values) {
        free(expr->map_literal.values);
        expr->map_literal.values = NULL;
    }

    expr->kind = EXPR_SET_LITERAL;
    expr->set_literal.elements = NULL;
    expr->set_literal.element_count = 0;
    expr->set_literal.set_type = NULL;
}

static Type* type_result_tuple(TypeChecker* tc, Type* ok_type) {
    if (!ok_type) return type_any();
    Type* err_type = type_error_nullable(tc);
    Type* elements[2] = { ok_type, err_type };
    Type* tuple = type_tuple(elements, 2);
    type_free(ok_type);
    type_free(err_type);
    return tuple ? tuple : type_any();
}

SymbolTable* symbol_table_create(void) {
    SymbolTable* table = (SymbolTable*)safe_malloc(sizeof(SymbolTable));
    table->symbols = NULL;
    table->symbol_count = 0;
    table->symbol_capacity = 0;
    return table;
}

void symbol_table_free(SymbolTable* table) {
    if (!table) return;
    for (int i = 0; i < table->symbol_count; i++) {
        symbol_free(table->symbols[i]);
    }
    if (table->symbols) free(table->symbols);
    free(table);
}

void symbol_table_add(SymbolTable* table, Symbol* sym) {
    table->symbol_count++;
    if (table->symbol_count > table->symbol_capacity) {
        table->symbol_capacity = table->symbol_count * 2;
        table->symbols = (Symbol**)safe_realloc(table->symbols, table->symbol_capacity * sizeof(Symbol*));
    }
    table->symbols[table->symbol_count - 1] = sym;
}

Symbol* symbol_table_get(SymbolTable* table, const char* name) {
    for (int i = 0; i < table->symbol_count; i++) {
        if (strcmp(table->symbols[i]->name, name) == 0) {
            return table->symbols[i];
        }
    }
    return NULL;
}

bool symbol_table_has(SymbolTable* table, const char* name) {
    return symbol_table_get(table, name) != NULL;
}

void typechecker_init(TypeChecker* tc) {
    tc->globals = symbol_table_create();
    tc->locals = NULL;
    tc->local_count = 0;
    tc->local_capacity = 0;
    tc->had_error = false;
    tc->error = NULL;
    tc->current_function = NULL;
    tc->current_function_is_async = false;
    tc->current_return_type = NULL;
    tc->current_type_param_names = NULL;
    tc->current_type_param_constraints = NULL;
    tc->current_type_param_count = 0;
    tc->current_file = NULL;
    tc->program = NULL;
    tc->local_lookup_floor = 0;
    tc->capture_contexts = NULL;
    tc->capture_context_count = 0;
    tc->capture_context_capacity = 0;
    tc->impl_mappings = NULL;
    tc->impl_mapping_count = 0;
    tc->impl_mapping_capacity = 0;
    tc->generic_record_decls = NULL;
    tc->generic_record_decl_count = 0;
    tc->generic_record_decl_capacity = 0;
    tc->generic_type_alias_decls = NULL;
    tc->generic_type_alias_decl_count = 0;
    tc->generic_type_alias_decl_capacity = 0;
    tc->generic_enum_decls = NULL;
    tc->generic_enum_decl_count = 0;
    tc->generic_enum_decl_capacity = 0;
    tc->generic_record_instances = NULL;
    tc->generic_record_instance_count = 0;
    tc->generic_record_instance_capacity = 0;
    tc->generic_enum_instances = NULL;
    tc->generic_enum_instance_count = 0;
    tc->generic_enum_instance_capacity = 0;
    tc->expected_expr_type = NULL;
    tc->options.warn_unused_error = false;
    tc->options.strict_errors = false;
    tc->options.report_diagnostics = true;
    tc->options.extension_registry = NULL;

    typechecker_declare_builtin_error_type(tc);
    typechecker_declare_builtin_error_code_constants(tc);
}

void typechecker_free(TypeChecker* tc) {
    symbol_table_free(tc->globals);
    for (int i = 0; i < tc->local_count; i++) {
        symbol_table_free(tc->locals[i]);
    }
    if (tc->locals) free(tc->locals);
    for (int i = 0; i < tc->capture_context_count; i++) {
        typechecker_capture_context_free(&tc->capture_contexts[i]);
    }
    if (tc->capture_contexts) free(tc->capture_contexts);
    for (int i = 0; i < tc->impl_mapping_count; i++) {
        typechecker_impl_mapping_free(&tc->impl_mappings[i]);
    }
    if (tc->impl_mappings) free(tc->impl_mappings);
    if (tc->generic_record_decls) free(tc->generic_record_decls);
    if (tc->generic_type_alias_decls) free(tc->generic_type_alias_decls);
    if (tc->generic_enum_decls) free(tc->generic_enum_decls);
    for (int i = 0; i < tc->generic_record_instance_count; i++) {
        if (tc->generic_record_instances[i].key) free(tc->generic_record_instances[i].key);
        if (tc->generic_record_instances[i].type) type_free(tc->generic_record_instances[i].type);
    }
    if (tc->generic_record_instances) free(tc->generic_record_instances);
    for (int i = 0; i < tc->generic_enum_instance_count; i++) {
        if (tc->generic_enum_instances[i].key) free(tc->generic_enum_instances[i].key);
        if (tc->generic_enum_instances[i].type) type_free(tc->generic_enum_instances[i].type);
    }
    if (tc->generic_enum_instances) free(tc->generic_enum_instances);
    error_free(tc->error);
    if (tc->current_function) free(tc->current_function);
}

static void typechecker_error(TypeChecker* tc, const char* message, const char* file, int line, int column) {
    if (!tc->had_error) {
        tc->had_error = true;
        char buffer[512];
        snprintf(buffer, sizeof(buffer), "%s:%d:%d: %s", file ? file : "<unknown>", line, column, message);
        tc->error = error_create(ERROR_TYPE, buffer, file, line, column);
        if (tc->options.report_diagnostics) {
            fprintf(stderr, "Type error: %s\n", buffer);
            typechecker_print_source_context(file, line, column);
        }
    }
}

static void typechecker_print_source_context(const char* file, int line, int column) {
    if (!file || file[0] == '\0' || line <= 0) return;

    FILE* fp = fopen(file, "rb");
    if (!fp) return;

    char line_buffer[2048];
    int current_line = 1;
    bool found = false;

    while (fgets(line_buffer, sizeof(line_buffer), fp)) {
        if (current_line == line) {
            found = true;
            break;
        }
        current_line++;
    }
    fclose(fp);

    if (!found) return;

    size_t line_len = strlen(line_buffer);
    while (line_len > 0 &&
           (line_buffer[line_len - 1] == '\n' || line_buffer[line_len - 1] == '\r')) {
        line_buffer[line_len - 1] = '\0';
        line_len--;
    }

    int caret_col = column > 0 ? column : 1;
    if (caret_col > (int)line_len + 1) {
        caret_col = (int)line_len + 1;
    }

    fprintf(stderr, "    %s\n", line_buffer);
    fprintf(stderr, "    %*s^\n", caret_col - 1, "");
}

static void typechecker_warn(TypeChecker* tc, const char* message, const char* file, int line, int column) {
    if (!tc || !message) return;
    if (tc->options.strict_errors) {
        typechecker_error(tc, message, file, line, column);
        return;
    }
    if (!tc->options.report_diagnostics) {
        return;
    }

    fprintf(stderr, "Warning: %s:%d:%d: %s\n",
            file ? file : "<unknown>",
            line,
            column,
            message);
}

static void typechecker_error_expected_got(TypeChecker* tc,
                                           const char* context,
                                           Type* expected,
                                           Type* got,
                                           const char* file,
                                           int line,
                                           int column) {
    char expected_buf[256];
    char got_buf[256];
    expected_buf[0] = '\0';
    got_buf[0] = '\0';

    if (expected) {
        type_to_string(expected, expected_buf, sizeof(expected_buf));
    } else {
        snprintf(expected_buf, sizeof(expected_buf), "unknown");
    }

    if (got) {
        type_to_string(got, got_buf, sizeof(got_buf));
    } else {
        snprintf(got_buf, sizeof(got_buf), "unknown");
    }

    char message[768];
    snprintf(message, sizeof(message), "%s: expected %s, got %s",
             context ? context : "Type mismatch",
             expected_buf,
             got_buf);
    typechecker_error(tc, message, file, line, column);
}

static void symbol_set_visibility_metadata(Symbol* sym, const char* decl_file, bool is_public) {
    if (!sym) return;
    sym->is_public = is_public;
    if (sym->decl_file) {
        free(sym->decl_file);
        sym->decl_file = NULL;
    }
    if (decl_file && decl_file[0] != '\0') {
        sym->decl_file = safe_strdup(decl_file);
    }
}

static bool symbol_is_accessible_from_file(const Symbol* sym, const char* use_file) {
    if (!sym) return false;
    if (sym->is_public) return true;
    if (!sym->decl_file || sym->decl_file[0] == '\0') return true;
    if (!use_file || use_file[0] == '\0') return false;
    return strcmp(sym->decl_file, use_file) == 0;
}

static bool typechecker_function_matches_interface_method(Type* impl_fn_type, Type* iface_fn_type, Type* receiver_type) {
    if (!impl_fn_type || !iface_fn_type || !receiver_type) return false;
    if (impl_fn_type->kind != TYPE_FUNCTION || iface_fn_type->kind != TYPE_FUNCTION) return false;
    if (receiver_type->kind != TYPE_RECORD || !receiver_type->record_def) return false;

    if (impl_fn_type->param_count != iface_fn_type->param_count + 1) return false;
    if (impl_fn_type->param_count <= 0 || !impl_fn_type->param_types) return false;
    if (iface_fn_type->param_count > 0 && !iface_fn_type->param_types) return false;

    Type* receiver_param_type = impl_fn_type->param_types[0];
    if (!receiver_param_type || receiver_param_type->kind != TYPE_RECORD || !receiver_param_type->record_def) {
        return false;
    }
    if (receiver_param_type->record_def != receiver_type->record_def) {
        const char* impl_receiver_name = receiver_param_type->record_def->name;
        const char* concrete_receiver_name = receiver_type->record_def->name;
        if (!impl_receiver_name ||
            !concrete_receiver_name ||
            !typechecker_record_base_name_equals(impl_receiver_name, concrete_receiver_name)) {
            return false;
        }
    }

    int impl_receiver_param_count = receiver_param_type->param_count;
    int concrete_receiver_param_count = receiver_type->param_count;
    if (concrete_receiver_param_count == 0) {
        // When matching an impl against a generic record declaration (e.g. impl as Box[T]),
        // require the receiver specialization to remain generic.
        if (impl_receiver_param_count > 0) {
            if (!receiver_param_type->param_types) return false;
            for (int i = 0; i < impl_receiver_param_count; i++) {
                if (!typechecker_type_contains_type_param(receiver_param_type->param_types[i])) {
                    return false;
                }
            }
        }
    } else {
        if (impl_receiver_param_count != concrete_receiver_param_count) return false;
        if (!receiver_param_type->param_types || !receiver_type->param_types) return false;
        for (int i = 0; i < impl_receiver_param_count; i++) {
            Type* impl_arg = receiver_param_type->param_types[i];
            Type* concrete_arg = receiver_type->param_types[i];
            if (typechecker_type_contains_type_param(impl_arg)) continue;
            if (!type_assignable(impl_arg, concrete_arg) ||
                !type_assignable(concrete_arg, impl_arg)) {
                return false;
            }
        }
    }

    for (int i = 0; i < iface_fn_type->param_count; i++) {
        Type* expected = iface_fn_type->param_types[i];
        Type* got = impl_fn_type->param_types[i + 1];
        if (!type_assignable(expected, got) || !type_assignable(got, expected)) {
            return false;
        }
    }

    if (!impl_fn_type->return_type || !iface_fn_type->return_type) return false;
    if (!type_assignable(iface_fn_type->return_type, impl_fn_type->return_type) ||
        !type_assignable(impl_fn_type->return_type, iface_fn_type->return_type)) {
        return false;
    }

    return true;
}

static bool typechecker_record_implements_interface(TypeChecker* tc,
                                                    Type* record_type,
                                                    Type* interface_type,
                                                    const char* use_file,
                                                    int line,
                                                    int column) {
    (void)line;
    (void)column;
    if (!tc || !record_type || !interface_type) return false;
    if (record_type->kind != TYPE_RECORD || interface_type->kind != TYPE_INTERFACE) return false;
    if (!record_type->record_def || !interface_type->interface_def) return false;

    const char* interface_name = interface_type->interface_def->name;
    const char* record_name = record_type->record_def->name;
    InterfaceImplMapping* mapping = NULL;
    if (interface_name && record_name) {
        mapping = typechecker_find_impl_mapping(tc, interface_name, record_name);
    }

    InterfaceDef* iface = interface_type->interface_def;
    for (int i = 0; i < iface->method_count; i++) {
        InterfaceMethod* method = interface_def_get_method(iface, i);
        if (!method || !method->name || !method->type) return false;

        const char* function_name = method->name;
        if (mapping) {
            function_name = typechecker_impl_lookup_function(mapping, method->name);
            if (!function_name) return false;
        }

        Symbol* method_sym = symbol_table_get(tc->globals, function_name);
        if (!method_sym || !method_sym->type) return false;
        if (!symbol_is_accessible_from_file(method_sym, use_file)) return false;

        if (!typechecker_function_matches_interface_method(method_sym->type, method->type, record_type)) {
            return false;
        }
    }

    return true;
}

static bool typechecker_interface_has_method_named(TypeChecker* tc, const char* method_name) {
    if (!tc || !tc->globals || !method_name || method_name[0] == '\0') return false;

    for (int i = 0; i < tc->globals->symbol_count; i++) {
        Symbol* sym = tc->globals->symbols ? tc->globals->symbols[i] : NULL;
        if (!sym || !sym->type || sym->type->kind != TYPE_INTERFACE || !sym->type->interface_def) {
            continue;
        }

        if (interface_def_get_method_type(sym->type->interface_def, method_name)) {
            return true;
        }
    }

    return false;
}

static Type* typechecker_type_param_constraint(TypeChecker* tc, const char* type_param_name) {
    if (!tc || !type_param_name || !tc->current_type_param_names || !tc->current_type_param_constraints) {
        return NULL;
    }

    for (int i = 0; i < tc->current_type_param_count; i++) {
        const char* name = tc->current_type_param_names[i];
        if (name && strcmp(name, type_param_name) == 0) {
            return tc->current_type_param_constraints[i];
        }
    }
    return NULL;
}

static Type* typechecker_resolve_type_param_constraint(TypeChecker* tc, Type* type_param_type) {
    if (!type_param_type || type_param_type->kind != TYPE_TYPE_PARAM) return NULL;

    const char* name = type_param_type->type_param_name;
    if (!name) return NULL;

    int safety_limit = tc ? (tc->current_type_param_count + 1) : 8;
    if (safety_limit < 1) safety_limit = 1;

    Type* constraint = typechecker_type_param_constraint(tc, name);
    while (constraint && constraint->kind == TYPE_TYPE_PARAM && safety_limit-- > 0) {
        if (!constraint->type_param_name || strcmp(constraint->type_param_name, name) == 0) {
            break;
        }
        constraint = typechecker_type_param_constraint(tc, constraint->type_param_name);
    }
    return constraint;
}

static bool typechecker_types_assignable(TypeChecker* tc, Type* to, Type* from, const char* file, int line, int column) {
    if (type_assignable(to, from)) return true;
    if (!to || !from) return false;

    if (to->kind == TYPE_INTERFACE && from->kind == TYPE_TYPE_PARAM) {
        Type* constraint = typechecker_resolve_type_param_constraint(tc, from);
        if (constraint) {
            return typechecker_types_assignable(tc, to, constraint, file, line, column);
        }
    }

    if (to->kind == TYPE_INTERFACE && from->kind == TYPE_RECORD) {
        return typechecker_record_implements_interface(tc, from, to, file, line, column);
    }

    return false;
}

static bool typechecker_call_allows_interface_receiver(TypeChecker* tc,
                                                       Expr* call_expr,
                                                       Type* callee_type,
                                                       int arg_index,
                                                       Type* param_type,
                                                       Type* arg_type,
                                                       const char* file,
                                                       int line,
                                                       int column) {
    (void)line;
    (void)column;
    if (!tc || !call_expr || !callee_type || !param_type || !arg_type) return false;
    if (arg_index != 0) return false;
    if (param_type->kind != TYPE_RECORD || arg_type->kind != TYPE_INTERFACE) return false;
    if (!call_expr->call.callee || call_expr->call.callee->kind != EXPR_IDENTIFIER ||
        !call_expr->call.callee->identifier) {
        return false;
    }
    if (!arg_type->interface_def) return false;

    const char* callee_name = call_expr->call.callee->identifier;
    const char* method_name = callee_name;
    if (param_type->record_def && param_type->record_def->name &&
        arg_type->interface_def->name) {
        InterfaceImplMapping* mapping = typechecker_find_impl_mapping(tc,
                                                                      arg_type->interface_def->name,
                                                                      param_type->record_def->name);
        if (mapping) {
            const char* mapped_method_name = typechecker_impl_lookup_method(mapping, callee_name);
            if (!mapped_method_name) return false;
            method_name = mapped_method_name;
        }
    }

    Type* iface_method_type = interface_def_get_method_type(arg_type->interface_def, method_name);
    if (!iface_method_type) return false;

    if (!typechecker_function_matches_interface_method(callee_type, iface_method_type, param_type)) {
        return false;
    }

    return typechecker_record_implements_interface(tc, param_type, arg_type, file, line, column);
}

static int typechecker_type_param_binding_index(char** type_param_names, int type_param_count, const char* name) {
    if (!type_param_names || type_param_count <= 0 || !name) return -1;
    for (int i = 0; i < type_param_count; i++) {
        if (type_param_names[i] && strcmp(type_param_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool typechecker_infer_generic_bindings(TypeChecker* tc,
                                               Type* param_type,
                                               Type* arg_type,
                                               char** type_param_names,
                                               Type** bound_types,
                                               int type_param_count,
                                               const char* file,
                                               int line,
                                               int column,
                                               const char* inference_context) {
    if (!param_type || !arg_type || !bound_types) return true;

    if (param_type->kind == TYPE_TYPE_PARAM) {
        int idx = typechecker_type_param_binding_index(type_param_names,
                                                       type_param_count,
                                                       param_type->type_param_name);
        if (idx < 0) return true;
        if (!bound_types[idx]) {
            bound_types[idx] = type_clone(arg_type);
            return true;
        }

        if (bound_types[idx]->kind == TYPE_ANY && arg_type->kind != TYPE_ANY) {
            type_free(bound_types[idx]);
            bound_types[idx] = type_clone(arg_type);
            return true;
        }
        if (arg_type->kind == TYPE_ANY && bound_types[idx]->kind != TYPE_ANY) {
            return true;
        }

        if (!typechecker_types_assignable(tc, bound_types[idx], arg_type, file, line, column) ||
            !typechecker_types_assignable(tc, arg_type, bound_types[idx], file, line, column)) {
            char bound_buf[128];
            char arg_buf[128];
            bound_buf[0] = '\0';
            arg_buf[0] = '\0';
            type_to_string(bound_types[idx], bound_buf, sizeof(bound_buf));
            type_to_string(arg_type, arg_buf, sizeof(arg_buf));

            const char* type_param_name =
                (type_param_names && idx < type_param_count && type_param_names[idx] && type_param_names[idx][0] != '\0')
                    ? type_param_names[idx]
                    : (param_type->type_param_name ? param_type->type_param_name : "T");
            char msg[640];
            if (inference_context && inference_context[0] != '\0') {
                snprintf(msg,
                         sizeof(msg),
                         "Generic inference mismatch for type parameter '%s' while %.*s: inferred both %s and %s",
                         type_param_name,
                         200,
                         inference_context,
                         bound_buf,
                         arg_buf);
            } else {
                snprintf(msg,
                         sizeof(msg),
                         "Generic inference mismatch for type parameter '%s': inferred both %s and %s",
                         type_param_name,
                         bound_buf,
                         arg_buf);
            }
            typechecker_error(tc, msg, file, line, column);
            return false;
        }
        return true;
    }

    switch (param_type->kind) {
        case TYPE_ARRAY:
            if (arg_type->kind == TYPE_ARRAY) {
                return typechecker_infer_generic_bindings(tc,
                                                          param_type->element_type,
                                                          arg_type->element_type,
                                                          type_param_names,
                                                          bound_types,
                                                          type_param_count,
                                                          file,
                                                          line,
                                                          column,
                                                          inference_context);
            }
            break;
        case TYPE_FUTURE:
            if (arg_type->kind == TYPE_FUTURE) {
                return typechecker_infer_generic_bindings(tc,
                                                          param_type->element_type,
                                                          arg_type->element_type,
                                                          type_param_names,
                                                          bound_types,
                                                          type_param_count,
                                                          file,
                                                          line,
                                                          column,
                                                          inference_context);
            }
            break;
        case TYPE_SET:
            if (arg_type->kind == TYPE_SET && param_type->set_def && arg_type->set_def) {
                return typechecker_infer_generic_bindings(tc,
                                                          param_type->set_def->element_type,
                                                          arg_type->set_def->element_type,
                                                          type_param_names,
                                                          bound_types,
                                                          type_param_count,
                                                          file,
                                                          line,
                                                          column,
                                                          inference_context);
            }
            break;
        case TYPE_MAP:
            if (arg_type->kind == TYPE_MAP && param_type->map_def && arg_type->map_def) {
                if (!typechecker_infer_generic_bindings(tc,
                                                        param_type->map_def->key_type,
                                                        arg_type->map_def->key_type,
                                                        type_param_names,
                                                        bound_types,
                                                        type_param_count,
                                                        file,
                                                        line,
                                                        column,
                                                        inference_context)) {
                    return false;
                }
                return typechecker_infer_generic_bindings(tc,
                                                          param_type->map_def->value_type,
                                                          arg_type->map_def->value_type,
                                                          type_param_names,
                                                          bound_types,
                                                          type_param_count,
                                                          file,
                                                          line,
                                                          column,
                                                          inference_context);
            }
            break;
        case TYPE_TUPLE:
            if (arg_type->kind == TYPE_TUPLE &&
                tuple_type_get_arity(param_type) == tuple_type_get_arity(arg_type)) {
                for (int i = 0; i < tuple_type_get_arity(param_type); i++) {
                    if (!typechecker_infer_generic_bindings(tc,
                                                            tuple_type_get_element(param_type, i),
                                                            tuple_type_get_element(arg_type, i),
                                                            type_param_names,
                                                            bound_types,
                                                            type_param_count,
                                                            file,
                                                            line,
                                                            column,
                                                            inference_context)) {
                        return false;
                    }
                }
            }
            break;
        case TYPE_FUNCTION:
            if (arg_type->kind == TYPE_FUNCTION &&
                param_type->param_count == arg_type->param_count) {
                if (!typechecker_infer_generic_bindings(tc,
                                                        param_type->return_type,
                                                        arg_type->return_type,
                                                        type_param_names,
                                                        bound_types,
                                                        type_param_count,
                                                        file,
                                                        line,
                                                        column,
                                                        inference_context)) {
                    return false;
                }
                for (int i = 0; i < param_type->param_count; i++) {
                    if (!typechecker_infer_generic_bindings(tc,
                                                            param_type->param_types[i],
                                                            arg_type->param_types[i],
                                                            type_param_names,
                                                            bound_types,
                                                            type_param_count,
                                                            file,
                                                            line,
                                                            column,
                                                            inference_context)) {
                        return false;
                    }
                }
            }
            break;
        case TYPE_RECORD:
            if (arg_type->kind == TYPE_RECORD) {
                const char* param_name =
                    (param_type->record_def && param_type->record_def->name) ? param_type->record_def->name : NULL;
                const char* arg_name =
                    (arg_type->record_def && arg_type->record_def->name) ? arg_type->record_def->name : NULL;
                if (!typechecker_record_base_name_equals(param_name, arg_name)) {
                    break;
                }
                if (param_type->param_count != arg_type->param_count) {
                    break;
                }
                for (int i = 0; i < param_type->param_count; i++) {
                    if (!typechecker_infer_generic_bindings(tc,
                                                            param_type->param_types[i],
                                                            arg_type->param_types[i],
                                                            type_param_names,
                                                            bound_types,
                                                            type_param_count,
                                                            file,
                                                            line,
                                                            column,
                                                            inference_context)) {
                        return false;
                    }
                }
            }
            break;
        default:
            break;
    }

    return true;
}

static Type* typechecker_substitute_generic_type(Type* type,
                                                 char** type_param_names,
                                                 Type** bound_types,
                                                 int type_param_count,
                                                 bool* out_unresolved) {
    if (!type) return NULL;

    if (type->kind == TYPE_TYPE_PARAM) {
        int idx = typechecker_type_param_binding_index(type_param_names,
                                                       type_param_count,
                                                       type->type_param_name);
        if (idx >= 0 && bound_types && bound_types[idx]) {
            Type* resolved = type_clone(bound_types[idx]);
            if (resolved && type->nullable) resolved->nullable = true;
            return resolved ? resolved : type_any();
        }
        if (out_unresolved) *out_unresolved = true;
        Type* fallback = type_any();
        if (fallback) fallback->nullable = type->nullable;
        return fallback;
    }

    switch (type->kind) {
        case TYPE_ARRAY: {
            Type* elem = typechecker_substitute_generic_type(type->element_type,
                                                             type_param_names,
                                                             bound_types,
                                                             type_param_count,
                                                             out_unresolved);
            Type* out = type_array(elem ? elem : type_any());
            if (out) out->nullable = type->nullable;
            return out ? out : type_any();
        }
        case TYPE_FUTURE: {
            Type* elem = typechecker_substitute_generic_type(type->element_type,
                                                             type_param_names,
                                                             bound_types,
                                                             type_param_count,
                                                             out_unresolved);
            Type* out = type_future(elem ? elem : type_any());
            if (out) out->nullable = type->nullable;
            return out ? out : type_any();
        }
        case TYPE_FUNCTION: {
            Type* ret = typechecker_substitute_generic_type(type->return_type,
                                                            type_param_names,
                                                            bound_types,
                                                            type_param_count,
                                                            out_unresolved);
            Type** params = NULL;
            if (type->param_count > 0) {
                params = (Type**)safe_malloc((size_t)type->param_count * sizeof(Type*));
                for (int i = 0; i < type->param_count; i++) {
                    params[i] = typechecker_substitute_generic_type(type->param_types[i],
                                                                    type_param_names,
                                                                    bound_types,
                                                                    type_param_count,
                                                                    out_unresolved);
                }
            }
            Type* out = type_function(ret ? ret : type_any(), params, type->param_count);
            if (out) {
                out->nullable = type->nullable;
                type_function_set_type_params(out,
                                             type->type_param_names,
                                             type->type_param_constraints,
                                             type->type_param_count);
            }
            return out ? out : type_any();
        }
        case TYPE_TUPLE: {
            int arity = tuple_type_get_arity(type);
            Type** elems = NULL;
            if (arity > 0) {
                elems = (Type**)safe_malloc((size_t)arity * sizeof(Type*));
                for (int i = 0; i < arity; i++) {
                    elems[i] = typechecker_substitute_generic_type(tuple_type_get_element(type, i),
                                                                   type_param_names,
                                                                   bound_types,
                                                                   type_param_count,
                                                                   out_unresolved);
                }
            }
            Type* out = type_tuple(elems, arity);
            if (out) out->nullable = type->nullable;
            if (elems) {
                for (int i = 0; i < arity; i++) {
                    if (elems[i]) type_free(elems[i]);
                }
                free(elems);
            }
            return out ? out : type_any();
        }
        case TYPE_MAP: {
            Type* key_type = typechecker_substitute_generic_type(type->map_def ? type->map_def->key_type : NULL,
                                                                 type_param_names,
                                                                 bound_types,
                                                                 type_param_count,
                                                                 out_unresolved);
            Type* value_type = typechecker_substitute_generic_type(type->map_def ? type->map_def->value_type : NULL,
                                                                   type_param_names,
                                                                   bound_types,
                                                                   type_param_count,
                                                                   out_unresolved);
            Type* out = type_map(key_type ? key_type : type_any(), value_type ? value_type : type_any());
            if (out) out->nullable = type->nullable;
            if (key_type) type_free(key_type);
            if (value_type) type_free(value_type);
            return out ? out : type_any();
        }
        case TYPE_SET: {
            Type* elem = typechecker_substitute_generic_type(type->set_def ? type->set_def->element_type : NULL,
                                                             type_param_names,
                                                             bound_types,
                                                             type_param_count,
                                                             out_unresolved);
            Type* out = type_set(elem ? elem : type_any());
            if (out) out->nullable = type->nullable;
            if (elem) type_free(elem);
            return out ? out : type_any();
        }
        case TYPE_RECORD: {
            Type* out = type_clone(type);
            if (!out) return type_any();

            for (int i = 0; i < out->param_count; i++) {
                Type* substituted = typechecker_substitute_generic_type(type->param_types[i],
                                                                        type_param_names,
                                                                        bound_types,
                                                                        type_param_count,
                                                                        out_unresolved);
                if (out->param_types && out->param_types[i]) {
                    type_free(out->param_types[i]);
                }
                if (out->param_types) {
                    out->param_types[i] = substituted ? substituted : type_any();
                } else if (substituted) {
                    type_free(substituted);
                }
            }
            out->nullable = type->nullable;
            return out;
        }
        default:
            return type_clone(type);
    }
}

static bool type_is_match_comparable(Type* type) {
    if (!type) return false;
    switch (type->kind) {
        case TYPE_INT:
        case TYPE_BOOL:
        case TYPE_DOUBLE:
        case TYPE_BIGINT:
        case TYPE_STRING:
        case TYPE_NIL:
        case TYPE_TUPLE:
        case TYPE_RECORD:
        case TYPE_ANY:
            return true;
        default:
            return false;
    }
}

static bool match_expr_is_bool_literal(const Expr* expr, bool* out_value) {
    if (!expr || expr->kind != EXPR_LITERAL || !expr->type || expr->type->kind != TYPE_BOOL) {
        return false;
    }
    if (out_value) *out_value = expr->literal.as_int != 0;
    return true;
}

static size_t enum_base_name_len(const char* enum_name) {
    return typechecker_record_base_name_len(enum_name);
}

static bool enum_name_base_equals(const char* a, const char* b) {
    return typechecker_record_base_name_equals(a, b);
}

static bool enum_symbol_name_has_base_prefix(const char* symbol_name, const char* enum_name) {
    if (!symbol_name || !enum_name) return false;
    size_t enum_len = enum_base_name_len(enum_name);
    if (enum_len == 0) return false;
    if (strncmp(symbol_name, enum_name, enum_len) != 0) return false;
    if (symbol_name[enum_len] != '_') return false;
    if (symbol_name[enum_len + 1] == '\0') return false;
    return true;
}

static bool match_expr_constant_key(TypeChecker* tc, const Expr* expr, MatchPatternConst* out_key) {
    if (!expr || !out_key) return false;

    out_key->kind = MATCH_PATTERN_CONST_NONE;
    out_key->as_int = 0;
    out_key->as_double = 0.0;
    out_key->as_string = NULL;

    if (expr->kind == EXPR_NIL) {
        out_key->kind = MATCH_PATTERN_CONST_NIL;
        return true;
    }

    if (expr->kind == EXPR_IDENTIFIER &&
        expr->identifier &&
        expr->type &&
        expr->type->kind == TYPE_INT) {
        const char* enum_name = type_enum_name(expr->type);
        if (enum_name) {
            size_t enum_len = enum_base_name_len(enum_name);
            if (enum_symbol_name_has_base_prefix(expr->identifier, enum_name)) {
                const char* member_name = expr->identifier + enum_len + 1;
                int64_t member_value = 0;
                if (typechecker_enum_member_value(tc, enum_name, member_name, &member_value)) {
                    out_key->kind = MATCH_PATTERN_CONST_INT;
                    out_key->as_int = member_value;
                    return true;
                }
            }
        }
    }

    if (expr->kind != EXPR_LITERAL || !expr->type) {
        return false;
    }

    switch (expr->type->kind) {
        case TYPE_INT:
            out_key->kind = MATCH_PATTERN_CONST_INT;
            out_key->as_int = expr->literal.as_int;
            return true;
        case TYPE_BOOL:
            out_key->kind = MATCH_PATTERN_CONST_BOOL;
            out_key->as_int = expr->literal.as_int != 0 ? 1 : 0;
            return true;
        case TYPE_DOUBLE:
            out_key->kind = MATCH_PATTERN_CONST_DOUBLE;
            out_key->as_double = expr->literal.as_double;
            return true;
        case TYPE_STRING:
            out_key->kind = MATCH_PATTERN_CONST_STRING;
            out_key->as_string = expr->literal.as_string ? expr->literal.as_string : "";
            return true;
        case TYPE_BIGINT:
            out_key->kind = MATCH_PATTERN_CONST_BIGINT;
            out_key->as_string = expr->literal.as_string ? expr->literal.as_string : "";
            return true;
        default:
            return false;
    }
}

static bool match_pattern_constant_equals(const MatchPatternConst* a, const MatchPatternConst* b) {
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        case MATCH_PATTERN_CONST_INT:
        case MATCH_PATTERN_CONST_BOOL:
            return a->as_int == b->as_int;
        case MATCH_PATTERN_CONST_DOUBLE:
            return memcmp(&a->as_double, &b->as_double, sizeof(double)) == 0;
        case MATCH_PATTERN_CONST_STRING:
        case MATCH_PATTERN_CONST_BIGINT:
            if (!a->as_string || !b->as_string) return a->as_string == b->as_string;
            return strcmp(a->as_string, b->as_string) == 0;
        case MATCH_PATTERN_CONST_NIL:
            return true;
        default:
            return false;
    }
}

static void match_pattern_constant_format(const MatchPatternConst* key, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    if (!key) {
        snprintf(out, out_size, "<unknown>");
        return;
    }

    switch (key->kind) {
        case MATCH_PATTERN_CONST_INT:
            snprintf(out, out_size, "%lld", (long long)key->as_int);
            return;
        case MATCH_PATTERN_CONST_BOOL:
            snprintf(out, out_size, "%s", key->as_int != 0 ? "true" : "false");
            return;
        case MATCH_PATTERN_CONST_DOUBLE:
            snprintf(out, out_size, "%.17g", key->as_double);
            return;
        case MATCH_PATTERN_CONST_STRING:
            snprintf(out, out_size, "\"%s\"", key->as_string ? key->as_string : "");
            return;
        case MATCH_PATTERN_CONST_BIGINT:
            snprintf(out, out_size, "%s", key->as_string ? key->as_string : "0n");
            return;
        case MATCH_PATTERN_CONST_NIL:
            snprintf(out, out_size, "nil");
            return;
        default:
            snprintf(out, out_size, "<unknown>");
            return;
    }
}

static const char* type_enum_name(Type* type) {
    if (!type || type->kind != TYPE_INT || !type->type_param_name || type->type_param_name[0] == '\0') {
        return NULL;
    }
    return type->type_param_name;
}

static const char* symbol_enum_name(const Symbol* sym) {
    if (!sym || !sym->type) return NULL;

    const char* enum_name = type_enum_name(sym->type);
    if (enum_name) return enum_name;

    if (sym->type->kind == TYPE_FUNCTION && sym->type->return_type) {
        const char* ret_enum_name = type_enum_name(sym->type->return_type);
        if (ret_enum_name) return ret_enum_name;

        if (sym->type->return_type->kind == TYPE_RECORD &&
            sym->type->return_type->record_def &&
            sym->type->return_type->record_def->name) {
            return sym->type->return_type->record_def->name;
        }
    }

    return NULL;
}

static bool symbol_is_enum_member_for(const Symbol* sym, const char* enum_name, const char** out_member_name) {
    if (out_member_name) *out_member_name = NULL;
    if (!sym || !enum_name || !sym->name || sym->is_type_alias || !sym->type) return false;

    const char* sym_enum_name = symbol_enum_name(sym);
    if (!sym_enum_name || !enum_name_base_equals(sym_enum_name, enum_name)) return false;
    if (!enum_symbol_name_has_base_prefix(sym->name, enum_name)) return false;

    size_t enum_len = enum_base_name_len(enum_name);
    if (out_member_name) *out_member_name = sym->name + enum_len + 1;
    return true;
}

static const char* match_expr_enum_member_name(TypeChecker* tc, const Expr* expr, const char* enum_name) {
    if (!tc || !expr || !enum_name) {
        return NULL;
    }

    const char* symbol_name = NULL;
    const char* member_name = NULL;
    if (expr->kind == EXPR_IDENTIFIER && expr->identifier) {
        symbol_name = expr->identifier;
    } else if (expr->kind == EXPR_FIELD_ACCESS &&
               expr->field_access.object &&
               expr->field_access.object->kind == EXPR_IDENTIFIER &&
               expr->field_access.object->identifier &&
               expr->field_access.field_name) {
        if (!enum_name_base_equals(expr->field_access.object->identifier, enum_name)) {
            return NULL;
        }
        member_name = expr->field_access.field_name;
        return member_name;
    } else if (expr->kind == EXPR_CALL &&
               expr->call.callee) {
        if (expr->call.callee->kind == EXPR_IDENTIFIER &&
            expr->call.callee->identifier) {
            symbol_name = expr->call.callee->identifier;
        } else if (expr->call.callee->kind == EXPR_FIELD_ACCESS &&
                   expr->call.callee->field_access.object &&
                   expr->call.callee->field_access.object->kind == EXPR_IDENTIFIER &&
                   expr->call.callee->field_access.object->identifier &&
                   expr->call.callee->field_access.field_name) {
            if (!enum_name_base_equals(expr->call.callee->field_access.object->identifier, enum_name)) {
                return NULL;
            }
            member_name = expr->call.callee->field_access.field_name;
            return member_name;
        } else {
            return NULL;
        }
    } else {
        return NULL;
    }

    Symbol* sym = typechecker_lookup(tc, symbol_name);
    member_name = NULL;
    if (!symbol_is_enum_member_for(sym, enum_name, &member_name)) {
        return NULL;
    }
    return member_name;
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
        return enum_member_symbol_name(callee->field_access.object->identifier,
                                       callee->field_access.field_name);
    }

    return NULL;
}

static void typechecker_append_list_item(char* buffer, size_t buffer_size, const char* item) {
    if (!buffer || buffer_size == 0 || !item || item[0] == '\0') return;

    size_t offset = strlen(buffer);
    if (offset >= buffer_size - 1) return;

    int written = snprintf(buffer + offset,
                           buffer_size - offset,
                           "%s%s",
                           offset > 0 ? ", " : "",
                           item);
    if (written < 0) {
        buffer[buffer_size - 1] = '\0';
    }
}

static void typechecker_format_generic_type_param_list(char** type_param_names,
                                                       int type_param_count,
                                                       char* out,
                                                       size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    if (!type_param_names || type_param_count <= 0) return;

    size_t offset = 0;
    int written = snprintf(out + offset, out_size - offset, "<");
    if (written < 0) {
        out[0] = '\0';
        return;
    }
    offset += (size_t)written;
    if (offset >= out_size) {
        out[out_size - 1] = '\0';
        return;
    }

    for (int i = 0; i < type_param_count; i++) {
        const char* name = (type_param_names[i] && type_param_names[i][0] != '\0') ? type_param_names[i] : "T";
        written = snprintf(out + offset,
                           out_size - offset,
                           "%s%s",
                           i > 0 ? ", " : "",
                           name);
        if (written < 0) {
            out[out_size - 1] = '\0';
            return;
        }
        offset += (size_t)written;
        if (offset >= out_size) {
            out[out_size - 1] = '\0';
            return;
        }
    }

    written = snprintf(out + offset, out_size - offset, ">");
    if (written < 0) {
        out[out_size - 1] = '\0';
    }
}

static void typechecker_format_named_type_with_type_args(const char* type_name,
                                                         Type** type_args,
                                                         int type_arg_count,
                                                         char* out,
                                                         size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    const char* base_name = (type_name && type_name[0] != '\0') ? type_name : "<type>";
    size_t offset = 0;
    int written = snprintf(out + offset, out_size - offset, "%.*s", 96, base_name);
    if (written < 0) {
        out[0] = '\0';
        return;
    }
    offset += (size_t)written;
    if (offset >= out_size) {
        out[out_size - 1] = '\0';
        return;
    }

    if (!type_args || type_arg_count <= 0) return;

    written = snprintf(out + offset, out_size - offset, "<");
    if (written < 0) {
        out[out_size - 1] = '\0';
        return;
    }
    offset += (size_t)written;
    if (offset >= out_size) {
        out[out_size - 1] = '\0';
        return;
    }

    for (int i = 0; i < type_arg_count; i++) {
        char arg_buf[96];
        arg_buf[0] = '\0';
        if (type_args[i]) {
            type_to_string(type_args[i], arg_buf, sizeof(arg_buf));
        } else {
            snprintf(arg_buf, sizeof(arg_buf), "?");
        }

        written = snprintf(out + offset,
                           out_size - offset,
                           "%s%.*s",
                           i > 0 ? ", " : "",
                           80,
                           arg_buf);
        if (written < 0) {
            out[out_size - 1] = '\0';
            return;
        }
        offset += (size_t)written;
        if (offset >= out_size) {
            out[out_size - 1] = '\0';
            return;
        }
    }

    written = snprintf(out + offset, out_size - offset, ">");
    if (written < 0) {
        out[out_size - 1] = '\0';
    }
}

static char* typechecker_call_target_name(TypeChecker* tc, const Expr* callee_expr) {
    if (!callee_expr) {
        return safe_strdup("<call>");
    }

    if (callee_expr->kind == EXPR_FIELD_ACCESS &&
        callee_expr->field_access.object &&
        callee_expr->field_access.object->kind == EXPR_IDENTIFIER &&
        callee_expr->field_access.object->identifier &&
        callee_expr->field_access.field_name) {
        const char* object_name = callee_expr->field_access.object->identifier;
        const char* field_name = callee_expr->field_access.field_name;
        size_t len = strlen(object_name) + 1 + strlen(field_name) + 1;
        char* full_name = (char*)safe_malloc(len);
        snprintf(full_name, len, "%s.%s", object_name, field_name);
        return full_name;
    }

    if (callee_expr->kind == EXPR_IDENTIFIER && callee_expr->identifier) {
        const char* identifier = callee_expr->identifier;
        Symbol* sym = typechecker_lookup(tc, identifier);
        const char* enum_name = symbol_enum_name(sym);
        if (enum_name && enum_symbol_name_has_base_prefix(identifier, enum_name)) {
            size_t enum_len = enum_base_name_len(enum_name);
            const char* member_name = identifier + enum_len + 1;
            size_t len = enum_len + 1 + strlen(member_name) + 1;
            char* full_name = (char*)safe_malloc(len);
            snprintf(full_name, len, "%.*s.%s", (int)enum_len, enum_name, member_name);
            return full_name;
        }
        return safe_strdup(identifier);
    }

    return safe_strdup("<call>");
}

static bool match_binding_name_looks_destructure(const char* name) {
    if (!name || name[0] == '\0') return false;
    if (strcmp(name, "_") == 0) return true;

    char first = name[0];
    return first >= 'a' && first <= 'z';
}

static void match_stmt_clear_payload_bindings(Stmt* stmt) {
    if (!stmt || stmt->kind != STMT_MATCH) return;

    if (stmt->match_stmt.payload_binding_names) {
        for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
            char** names = stmt->match_stmt.payload_binding_names[i];
            int count = (stmt->match_stmt.payload_binding_counts &&
                         i < stmt->match_stmt.arm_count)
                            ? stmt->match_stmt.payload_binding_counts[i]
                            : 0;
            if (!names) continue;
            for (int j = 0; j < count; j++) {
                if (names[j]) free(names[j]);
            }
            free(names);
        }
        free(stmt->match_stmt.payload_binding_names);
        stmt->match_stmt.payload_binding_names = NULL;
    }

    if (stmt->match_stmt.payload_binding_counts) {
        free(stmt->match_stmt.payload_binding_counts);
        stmt->match_stmt.payload_binding_counts = NULL;
    }
}

static void match_expr_clear_payload_bindings(Expr* expr) {
    if (!expr || expr->kind != EXPR_MATCH) return;

    if (expr->match_expr.payload_binding_names) {
        for (int i = 0; i < expr->match_expr.arm_count; i++) {
            char** names = expr->match_expr.payload_binding_names[i];
            int count = (expr->match_expr.payload_binding_counts &&
                         i < expr->match_expr.arm_count)
                            ? expr->match_expr.payload_binding_counts[i]
                            : 0;
            if (!names) continue;
            for (int j = 0; j < count; j++) {
                if (names[j]) free(names[j]);
            }
            free(names);
        }
        free(expr->match_expr.payload_binding_names);
        expr->match_expr.payload_binding_names = NULL;
    }

    if (expr->match_expr.payload_binding_counts) {
        free(expr->match_expr.payload_binding_counts);
        expr->match_expr.payload_binding_counts = NULL;
    }
}

static void match_pattern_free_payload_bindings(char** names, Type** types, int count) {
    if (names) {
        for (int i = 0; i < count; i++) {
            if (names[i]) free(names[i]);
        }
        free(names);
    }
    if (types) {
        for (int i = 0; i < count; i++) {
            if (types[i]) type_free(types[i]);
        }
        free(types);
    }
}

typedef struct {
    char** names;
    Type** types;
    int count;
    bool initialized;
} MatchAlternationBindingGroup;

static int match_named_binding_count(char** payload_bind_names, int payload_bind_count) {
    if (!payload_bind_names || payload_bind_count <= 0) return 0;

    int named_count = 0;
    for (int i = 0; i < payload_bind_count; i++) {
        if (payload_bind_names[i]) {
            named_count++;
        }
    }
    return named_count;
}

static int match_named_binding_index(char** names, int count, const char* name) {
    if (!names || count <= 0 || !name) return -1;
    for (int i = 0; i < count; i++) {
        if (names[i] && strcmp(names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static void match_alternation_binding_group_capture(MatchAlternationBindingGroup* group,
                                                    char** payload_bind_names,
                                                    Type** payload_bind_types,
                                                    int payload_bind_count) {
    if (!group) return;

    int named_count = match_named_binding_count(payload_bind_names, payload_bind_count);
    group->count = named_count;
    group->initialized = true;

    if (named_count <= 0) return;

    group->names = (char**)safe_calloc((size_t)named_count, sizeof(char*));
    group->types = (Type**)safe_calloc((size_t)named_count, sizeof(Type*));

    int write_index = 0;
    for (int i = 0; i < payload_bind_count; i++) {
        if (!payload_bind_names || !payload_bind_names[i]) continue;

        group->names[write_index] = safe_strdup(payload_bind_names[i]);
        group->types[write_index] =
            (payload_bind_types && payload_bind_types[i]) ? type_clone(payload_bind_types[i]) : type_any();
        write_index++;
    }
}

static void match_alternation_binding_group_free(MatchAlternationBindingGroup* group) {
    if (!group) return;

    if (group->names) {
        for (int i = 0; i < group->count; i++) {
            if (group->names[i]) free(group->names[i]);
        }
        free(group->names);
    }
    if (group->types) {
        for (int i = 0; i < group->count; i++) {
            if (group->types[i]) type_free(group->types[i]);
        }
        free(group->types);
    }

    group->names = NULL;
    group->types = NULL;
    group->count = 0;
    group->initialized = false;
}

static void match_validate_alternation_bindings(TypeChecker* tc,
                                                MatchAlternationBindingGroup* group,
                                                char** payload_bind_names,
                                                Type** payload_bind_types,
                                                int payload_bind_count,
                                                const char* file,
                                                int line,
                                                int column) {
    if (!tc || !group || !group->initialized) return;

    int named_count = match_named_binding_count(payload_bind_names, payload_bind_count);
    if (named_count != group->count) {
        typechecker_error(tc,
                          "Pattern alternatives in the same arm must bind the same names",
                          file,
                          line,
                          column);
        return;
    }

    for (int i = 0; i < payload_bind_count; i++) {
        const char* bind_name = (payload_bind_names && i < payload_bind_count)
            ? payload_bind_names[i]
            : NULL;
        if (!bind_name) continue;

        int expected_index = match_named_binding_index(group->names, group->count, bind_name);
        if (expected_index < 0) {
            typechecker_error(tc,
                              "Pattern alternatives in the same arm must bind the same names",
                              file,
                              line,
                              column);
            return;
        }

        Type* expected_type = (group->types && expected_index < group->count)
            ? group->types[expected_index]
            : NULL;
        Type* actual_type = (payload_bind_types && i < payload_bind_count)
            ? payload_bind_types[i]
            : NULL;
        if (!expected_type || !actual_type) continue;

        if (!typechecker_types_assignable(tc, expected_type, actual_type, file, line, column) ||
            !typechecker_types_assignable(tc, actual_type, expected_type, file, line, column)) {
            char expected_buf[128];
            char actual_buf[128];
            char message[384];
            type_to_string(expected_type, expected_buf, sizeof(expected_buf));
            type_to_string(actual_type, actual_buf, sizeof(actual_buf));
            snprintf(message,
                     sizeof(message),
                     "Pattern alternatives in the same arm must bind '%s' with compatible types (got %s and %s)",
                     bind_name,
                     expected_buf,
                     actual_buf);
            typechecker_error(tc, message, file, line, column);
            return;
        }
    }
}

static void match_pattern_append_binding(char*** out_names,
                                         Type*** out_types,
                                         int* out_count,
                                         const char* name,
                                         Type* type) {
    if (!out_names || !out_types || !out_count) return;

    int count = *out_count + 1;
    *out_names = (char**)safe_realloc(*out_names, (size_t)count * sizeof(char*));
    *out_types = (Type**)safe_realloc(*out_types, (size_t)count * sizeof(Type*));
    (*out_names)[count - 1] = name ? safe_strdup(name) : NULL;
    (*out_types)[count - 1] = type ? type_clone(type) : type_any();
    *out_count = count;
}

static bool match_resolve_enum_payload_types_from_symbol(TypeChecker* tc,
                                                         Symbol* sym,
                                                         Type* subject_type,
                                                         Type** explicit_type_args,
                                                         int explicit_type_arg_count,
                                                         Type*** out_types,
                                                         int* out_count) {
    if (out_types) *out_types = NULL;
    if (out_count) *out_count = 0;
    if (!tc || !sym || !subject_type) return false;

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    if (!resolved_subject_type) return false;

    const char* subject_enum = type_enum_name(resolved_subject_type);
    const char* member_name = NULL;
    if (!subject_enum || !symbol_is_enum_member_for(sym, subject_enum, &member_name)) {
        return false;
    }

    if (!sym->type) {
        return false;
    }

    if (sym->type->kind != TYPE_FUNCTION) {
        return true;
    }

    int payload_count = sym->type->param_count;
    int type_param_count = sym->type->type_param_count;
    Type** bound_types = NULL;
    if (type_param_count > 0) {
        bound_types = (Type**)safe_calloc((size_t)type_param_count, sizeof(Type*));
        if (explicit_type_arg_count > 0) {
            if (explicit_type_arg_count != type_param_count) {
                match_pattern_free_payload_bindings(NULL, bound_types, type_param_count);
                return false;
            }

            for (int i = 0; i < type_param_count; i++) {
                Type* explicit_type_arg = (explicit_type_args && explicit_type_args[i])
                    ? explicit_type_args[i]
                    : NULL;
                Type* resolved_type_arg = explicit_type_arg
                    ? typechecker_resolve_type(tc, explicit_type_arg)
                    : type_any();
                if (explicit_type_args && explicit_type_args[i]) {
                    explicit_type_args[i] = resolved_type_arg;
                }
                bound_types[i] = resolved_type_arg ? type_clone(resolved_type_arg) : type_any();
                if (!explicit_type_arg && resolved_type_arg) {
                    type_free(resolved_type_arg);
                }
            }
        } else {
            typechecker_seed_generic_bindings_from_enum_subject(tc,
                                                                subject_enum,
                                                                sym->type,
                                                                bound_types,
                                                                type_param_count);
        }
    }

    Type** payload_types = (Type**)safe_calloc((size_t)payload_count, sizeof(Type*));
    for (int i = 0; i < payload_count; i++) {
        if (sym->type->param_types && i < sym->type->param_count && sym->type->param_types[i]) {
            Type* payload_type = sym->type->param_types[i];
            if (type_param_count > 0 && sym->type->type_param_names) {
                bool unresolved = false;
                Type* substituted = typechecker_substitute_generic_type(payload_type,
                                                                        sym->type->type_param_names,
                                                                        bound_types,
                                                                        type_param_count,
                                                                        &unresolved);
                substituted = typechecker_resolve_type(tc, substituted);
                payload_types[i] = substituted ? substituted : type_any();
            } else {
                payload_types[i] = type_clone(payload_type);
            }
        } else {
            payload_types[i] = type_any();
        }
    }

    if (bound_types) {
        match_pattern_free_payload_bindings(NULL, bound_types, type_param_count);
    }

    if (out_types) {
        *out_types = payload_types;
    } else {
        match_pattern_free_payload_bindings(NULL, payload_types, payload_count);
    }
    if (out_count) *out_count = payload_count;
    return true;
}

static bool match_pattern_resolve_enum_payload_types(TypeChecker* tc,
                                                     Expr* pattern_expr,
                                                     Type* subject_type,
                                                     Type*** out_types,
                                                     int* out_count) {
    if (out_types) *out_types = NULL;
    if (out_count) *out_count = 0;
    if (!tc || !pattern_expr || !subject_type) return false;
    if (pattern_expr->kind != EXPR_CALL || !pattern_expr->call.callee) return false;

    char* symbol_name = match_pattern_call_symbol_name(pattern_expr);
    if (!symbol_name) return false;

    Symbol* sym = typechecker_lookup(tc, symbol_name);
    free(symbol_name);

    if (!sym || !sym->type || sym->type->kind != TYPE_FUNCTION) {
        return false;
    }
    if (pattern_expr->call.arg_count != sym->type->param_count) {
        return false;
    }

    return match_resolve_enum_payload_types_from_symbol(tc,
                                                        sym,
                                                        subject_type,
                                                        pattern_expr->call.type_args,
                                                        pattern_expr->call.type_arg_count,
                                                        out_types,
                                                        out_count);
}

static bool match_enum_member_payload_types(TypeChecker* tc,
                                            Type* subject_type,
                                            const char* member_name,
                                            Type*** out_types,
                                            int* out_count) {
    if (out_types) *out_types = NULL;
    if (out_count) *out_count = 0;
    if (!tc || !subject_type || !member_name) return false;

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    if (!resolved_subject_type) return false;

    const char* subject_enum = type_enum_name(resolved_subject_type);
    if (!subject_enum || !tc || !tc->globals) return false;

    Symbol* sym = NULL;
    for (int i = 0; i < tc->globals->symbol_count; i++) {
        Symbol* candidate = tc->globals->symbols ? tc->globals->symbols[i] : NULL;
        const char* candidate_member_name = NULL;
        if (!symbol_is_enum_member_for(candidate, subject_enum, &candidate_member_name)) {
            continue;
        }
        if (candidate_member_name && strcmp(candidate_member_name, member_name) == 0) {
            sym = candidate;
            break;
        }
    }

    return match_resolve_enum_payload_types_from_symbol(tc,
                                                        sym,
                                                        resolved_subject_type,
                                                        NULL,
                                                        0,
                                                        out_types,
                                                        out_count);
}

static bool expr_is_record_pattern(const Expr* expr) {
    return expr &&
           expr->kind == EXPR_RECORD_LITERAL &&
           expr->record_literal.is_pattern;
}

static void match_pattern_store_record_type(Expr* pattern_expr, Type* record_type) {
    if (!pattern_expr || pattern_expr->kind != EXPR_RECORD_LITERAL || !record_type) return;
    if (pattern_expr->record_literal.record_type &&
        pattern_expr->record_literal.record_type->kind == TYPE_RECORD) {
        return;
    }
    pattern_expr->record_literal.record_type = type_clone(record_type);
}

static Type* match_pattern_resolve_record_type(TypeChecker* tc,
                                               Expr* pattern_expr,
                                               Type* subject_type) {
    if (!tc || !pattern_expr || pattern_expr->kind != EXPR_RECORD_LITERAL) return NULL;

    Type* explicit_type = pattern_expr->record_literal.pattern_type;
    if (explicit_type) {
        explicit_type = typechecker_resolve_type(tc, explicit_type);
        pattern_expr->record_literal.pattern_type = explicit_type;
        if (explicit_type && explicit_type->kind == TYPE_RECORD) {
            return explicit_type;
        }
    }

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    if (resolved_subject_type && resolved_subject_type->kind == TYPE_RECORD) {
        return resolved_subject_type;
    }

    return NULL;
}

static bool match_pattern_is_structurally_covering(TypeChecker* tc,
                                                   Expr* pattern_expr,
                                                   Type* subject_type,
                                                   bool allow_identifier_binding) {
    if (!tc || !pattern_expr || !subject_type) return false;

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    if (!resolved_subject_type) return false;

    if (allow_identifier_binding &&
        pattern_expr->kind == EXPR_IDENTIFIER &&
        pattern_expr->identifier &&
        match_binding_name_looks_destructure(pattern_expr->identifier)) {
        return true;
    }

    if (pattern_expr->kind == EXPR_TUPLE_LITERAL &&
        resolved_subject_type->kind == TYPE_TUPLE &&
        tuple_type_get_arity(resolved_subject_type) == pattern_expr->tuple_literal.element_count) {
        for (int i = 0; i < pattern_expr->tuple_literal.element_count; i++) {
            if (!match_pattern_is_structurally_covering(tc,
                                                        pattern_expr->tuple_literal.elements[i],
                                                        tuple_type_get_element(resolved_subject_type, i),
                                                        true)) {
                return false;
            }
        }
        return true;
    }

    if (pattern_expr->kind == EXPR_RECORD_LITERAL) {
        Type* record_type = match_pattern_resolve_record_type(tc, pattern_expr, resolved_subject_type);
        if (!record_type || record_type->kind != TYPE_RECORD || !record_type->record_def) {
            return false;
        }

        match_pattern_store_record_type(pattern_expr, record_type);
        if (!pattern_expr->record_literal.allows_rest &&
            pattern_expr->record_literal.field_count != record_type->record_def->field_count) {
            return false;
        }

        for (int i = 0; i < pattern_expr->record_literal.field_count; i++) {
            const char* field_name = pattern_expr->record_literal.field_names
                ? pattern_expr->record_literal.field_names[i]
                : NULL;
            Type* field_type = field_name
                ? record_def_get_field_type(record_type->record_def, field_name)
                : NULL;
            if (!field_type) {
                return false;
            }
            if (!match_pattern_is_structurally_covering(tc,
                                                        pattern_expr->record_literal.field_values[i],
                                                        field_type,
                                                        true)) {
                return false;
            }
        }

        return true;
    }

    return false;
}

static bool match_pattern_is_binding_wildcard(const Expr* pattern_expr,
                                              bool allow_identifier_binding) {
    return allow_identifier_binding &&
           pattern_expr &&
           pattern_expr->kind == EXPR_IDENTIFIER &&
           pattern_expr->identifier &&
           match_binding_name_looks_destructure(pattern_expr->identifier);
}

static void match_patterns_append(Expr*** out_patterns,
                                  int* out_count,
                                  int* out_capacity,
                                  Expr* pattern_expr) {
    if (!out_patterns || !out_count || !out_capacity) return;

    if (*out_count + 1 > *out_capacity) {
        int new_capacity = *out_capacity > 0 ? (*out_capacity * 2) : 8;
        *out_patterns = (Expr**)safe_realloc(*out_patterns, (size_t)new_capacity * sizeof(Expr*));
        *out_capacity = new_capacity;
    }

    (*out_patterns)[*out_count] = pattern_expr;
    (*out_count)++;
}

static bool match_enum_patterns_exhaustive(TypeChecker* tc,
                                           Expr** patterns,
                                           int pattern_count,
                                           Type* subject_type) {
    const char* subject_enum = type_enum_name(subject_type);
    if (!tc || !patterns || pattern_count <= 0 || !subject_enum) return false;

    for (int i = 0; i < pattern_count; i++) {
        Expr* pattern_expr = patterns[i];
        if (!pattern_expr || match_pattern_is_binding_wildcard(pattern_expr, true)) {
            return true;
        }
    }

    bool saw_member = false;
    if (tc && tc->globals) {
        for (int i = 0; i < tc->globals->symbol_count; i++) {
            Symbol* sym = tc->globals->symbols ? tc->globals->symbols[i] : NULL;
            const char* member_name = NULL;
            if (!symbol_is_enum_member_for(sym, subject_enum, &member_name)) {
                continue;
            }

            saw_member = true;
            if (!match_enum_member_patterns_exhaustive(tc,
                                                       patterns,
                                                       pattern_count,
                                                       subject_type,
                                                       member_name)) {
                return false;
            }
        }
    }

    return saw_member;
}

struct MatchPatternRow {
    Expr** columns;
    int column_count;
};

typedef struct {
    char** columns;
    int column_count;
} MatchWitness;

static void match_pattern_rows_free(MatchPatternRow* rows, int row_count) {
    if (!rows) return;
    for (int i = 0; i < row_count; i++) {
        if (rows[i].columns) {
            free(rows[i].columns);
        }
    }
    free(rows);
}

static MatchWitness* match_witness_create(int column_count) {
    MatchWitness* witness = (MatchWitness*)safe_calloc(1, sizeof(MatchWitness));
    witness->column_count = column_count;
    if (column_count > 0) {
        witness->columns = (char**)safe_calloc((size_t)column_count, sizeof(char*));
    }
    return witness;
}

static void match_witness_free(MatchWitness* witness) {
    if (!witness) return;
    if (witness->columns) {
        for (int i = 0; i < witness->column_count; i++) {
            free(witness->columns[i]);
        }
        free(witness->columns);
    }
    free(witness);
}

static char* match_join_pattern_strings(char** patterns, int count) {
    if (count <= 0) {
        return safe_strdup("");
    }

    size_t total = 1;
    for (int i = 0; i < count; i++) {
        total += strlen(patterns[i] ? patterns[i] : "_");
        if (i + 1 < count) {
            total += 2;
        }
    }

    char* out = (char*)safe_malloc(total);
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        const char* pattern = patterns[i] ? patterns[i] : "_";
        size_t len = strlen(pattern);
        memcpy(out + pos, pattern, len);
        pos += len;
        if (i + 1 < count) {
            out[pos++] = ',';
            out[pos++] = ' ';
        }
    }
    out[pos] = '\0';
    return out;
}

static char* match_format_tuple_pattern_strings(char** patterns, int count) {
    char* joined = match_join_pattern_strings(patterns, count);
    size_t total = strlen(joined) + 3;
    char* out = (char*)safe_malloc(total);
    snprintf(out, total, "(%s)", joined);
    free(joined);
    return out;
}

static char* match_format_record_pattern_strings(Type* record_type,
                                                 char** patterns,
                                                 int count) {
    Type* resolved_record_type = record_type;
    if (!resolved_record_type || resolved_record_type->kind != TYPE_RECORD || !resolved_record_type->record_def) {
        return safe_strdup("_");
    }

    RecordDef* record_def = resolved_record_type->record_def;
    size_t total = 5;
    const char* record_name = record_def->name;
    if (record_name && record_name[0] != '\0') {
        total += strlen(record_name) + 1;
    }

    for (int i = 0; i < count; i++) {
        const char* field_name = (record_def->fields && i < record_def->field_count &&
                                  record_def->fields[i].name)
            ? record_def->fields[i].name
            : "_";
        const char* pattern = patterns[i] ? patterns[i] : "_";
        total += strlen(field_name) + 2 + strlen(pattern);
        if (i + 1 < count) {
            total += 2;
        }
    }

    char* out = (char*)safe_malloc(total);
    size_t pos = 0;
    if (record_name && record_name[0] != '\0') {
        size_t name_len = strlen(record_name);
        memcpy(out + pos, record_name, name_len);
        pos += name_len;
        out[pos++] = ' ';
    }
    out[pos++] = '{';
    out[pos++] = ' ';
    for (int i = 0; i < count; i++) {
        const char* field_name = (record_def->fields && i < record_def->field_count &&
                                  record_def->fields[i].name)
            ? record_def->fields[i].name
            : "_";
        const char* pattern = patterns[i] ? patterns[i] : "_";
        size_t field_len = strlen(field_name);
        memcpy(out + pos, field_name, field_len);
        pos += field_len;
        out[pos++] = ':';
        out[pos++] = ' ';
        size_t pattern_len = strlen(pattern);
        memcpy(out + pos, pattern, pattern_len);
        pos += pattern_len;
        if (i + 1 < count) {
            out[pos++] = ',';
            out[pos++] = ' ';
        }
    }
    out[pos++] = ' ';
    out[pos++] = '}';
    out[pos] = '\0';
    return out;
}

static char* match_format_enum_member_pattern_strings(const char* enum_name,
                                                      const char* member_name,
                                                      char** payload_patterns,
                                                      int payload_count) {
    const char* safe_enum_name = enum_name ? enum_name : "_";
    const char* safe_member_name = member_name ? member_name : "_";
    if (payload_count <= 0) {
        size_t total = strlen(safe_enum_name) + 1 + strlen(safe_member_name) + 1;
        char* out = (char*)safe_malloc(total);
        snprintf(out, total, "%s.%s", safe_enum_name, safe_member_name);
        return out;
    }

    char* joined = match_join_pattern_strings(payload_patterns, payload_count);
    size_t total = strlen(safe_enum_name) + 1 + strlen(safe_member_name) + 1 + strlen(joined) + 2;
    char* out = (char*)safe_malloc(total);
    snprintf(out, total, "%s.%s(%s)", safe_enum_name, safe_member_name, joined);
    free(joined);
    return out;
}

static char* match_default_pattern_for_type(TypeChecker* tc, Type* type) {
    Type* resolved_type = typechecker_resolve_type(tc, type);
    if (!resolved_type) {
        return safe_strdup("_");
    }

    if (type_enum_name(resolved_type)) {
        return safe_strdup("_");
    }

    if (resolved_type->kind == TYPE_BOOL) {
        return safe_strdup("false");
    }

    if (resolved_type->kind == TYPE_NIL) {
        return safe_strdup("nil");
    }

    if (resolved_type->kind == TYPE_TUPLE) {
        int arity = tuple_type_get_arity(resolved_type);
        char** parts = NULL;
        if (arity > 0) {
            parts = (char**)safe_calloc((size_t)arity, sizeof(char*));
            for (int i = 0; i < arity; i++) {
                parts[i] = safe_strdup("_");
            }
        }
        char* out = match_format_tuple_pattern_strings(parts, arity);
        if (parts) {
            for (int i = 0; i < arity; i++) {
                free(parts[i]);
            }
            free(parts);
        }
        return out;
    }

    if (resolved_type->kind == TYPE_RECORD && resolved_type->record_def) {
        int field_count = resolved_type->record_def->field_count;
        char** parts = NULL;
        if (field_count > 0) {
            parts = (char**)safe_calloc((size_t)field_count, sizeof(char*));
            for (int i = 0; i < field_count; i++) {
                parts[i] = safe_strdup("_");
            }
        }
        char* out = match_format_record_pattern_strings(resolved_type, parts, field_count);
        if (parts) {
            for (int i = 0; i < field_count; i++) {
                free(parts[i]);
            }
            free(parts);
        }
        return out;
    }

    return safe_strdup("_");
}

static bool match_pattern_matches_constant_candidate(TypeChecker* tc,
                                                     Expr* pattern_expr,
                                                     Type* head_type,
                                                     const MatchPatternConst* candidate) {
    if (!candidate) {
        return false;
    }
    if (!pattern_expr ||
        match_pattern_is_structurally_covering(tc, pattern_expr, head_type, true)) {
        return true;
    }

    MatchPatternConst pattern_const;
    if (!match_expr_constant_key(tc, pattern_expr, &pattern_const)) {
        return false;
    }
    return match_pattern_constant_equals(&pattern_const, candidate);
}

static int64_t match_missing_int_candidate(int attempt) {
    if (attempt <= 0) return 0;
    int64_t magnitude = (int64_t)((attempt + 1) / 2);
    return (attempt % 2) != 0 ? magnitude : -magnitude;
}

static void match_candidate_clear_owned(MatchPatternConst* candidate, bool owned_string) {
    if (!candidate) return;
    if (owned_string &&
        (candidate->kind == MATCH_PATTERN_CONST_STRING ||
         candidate->kind == MATCH_PATTERN_CONST_BIGINT) &&
        candidate->as_string) {
        free((char*)candidate->as_string);
    }
    candidate->as_string = NULL;
}

static bool match_build_constant_candidate(TypeChecker* tc,
                                           Type* head_type,
                                           int attempt,
                                           MatchPatternConst* out_candidate,
                                           bool* out_owned_string,
                                           char** out_text) {
    if (out_owned_string) *out_owned_string = false;
    if (out_text) *out_text = NULL;
    if (!out_candidate) return false;

    memset(out_candidate, 0, sizeof(*out_candidate));
    Type* resolved_head_type = typechecker_resolve_type(tc, head_type);
    if (!resolved_head_type) return false;

    switch (resolved_head_type->kind) {
        case TYPE_INT:
            out_candidate->kind = MATCH_PATTERN_CONST_INT;
            out_candidate->as_int = match_missing_int_candidate(attempt);
            break;
        case TYPE_DOUBLE:
            out_candidate->kind = MATCH_PATTERN_CONST_DOUBLE;
            out_candidate->as_double = (double)match_missing_int_candidate(attempt);
            break;
        case TYPE_STRING:
            out_candidate->kind = MATCH_PATTERN_CONST_STRING;
            if (attempt == 0) {
                out_candidate->as_string = safe_strdup("");
            } else if (attempt == 1) {
                out_candidate->as_string = safe_strdup("__missing__");
            } else {
                char buf[64];
                snprintf(buf, sizeof(buf), "__missing%d__", attempt);
                out_candidate->as_string = safe_strdup(buf);
            }
            if (out_owned_string) *out_owned_string = true;
            break;
        case TYPE_BIGINT: {
            out_candidate->kind = MATCH_PATTERN_CONST_BIGINT;
            int64_t value = match_missing_int_candidate(attempt);
            char buf[64];
            snprintf(buf, sizeof(buf), "%lldn", (long long)value);
            out_candidate->as_string = safe_strdup(buf);
            if (out_owned_string) *out_owned_string = true;
            break;
        }
        default:
            return false;
    }

    if (out_text) {
        char literal_buf[256];
        match_pattern_constant_format(out_candidate, literal_buf, sizeof(literal_buf));
        *out_text = safe_strdup(literal_buf);
    }
    return true;
}

static MatchWitness* match_default_witness(TypeChecker* tc,
                                           Type** column_types,
                                           int column_count) {
    MatchWitness* witness = match_witness_create(column_count);
    for (int i = 0; i < column_count; i++) {
        witness->columns[i] = match_default_pattern_for_type(tc, column_types[i]);
    }
    return witness;
}

static MatchWitness* match_witness_prepend(const char* head_pattern,
                                           MatchWitness* tail) {
    int tail_count = tail ? tail->column_count : 0;
    MatchWitness* witness = match_witness_create(tail_count + 1);
    witness->columns[0] = safe_strdup(head_pattern ? head_pattern : "_");
    for (int i = 0; i < tail_count; i++) {
        witness->columns[i + 1] = safe_strdup(
            (tail->columns && tail->columns[i]) ? tail->columns[i] : "_");
    }
    return witness;
}

static MatchWitness* match_witness_collapse_head(const char* head_pattern,
                                                 MatchWitness* expanded_witness,
                                                 int consumed_columns) {
    if (!expanded_witness || expanded_witness->column_count < consumed_columns) {
        return NULL;
    }

    int tail_count = expanded_witness->column_count - consumed_columns;
    MatchWitness* witness = match_witness_create(tail_count + 1);
    witness->columns[0] = safe_strdup(head_pattern ? head_pattern : "_");
    for (int i = 0; i < tail_count; i++) {
        witness->columns[i + 1] =
            safe_strdup(expanded_witness->columns[consumed_columns + i]
                            ? expanded_witness->columns[consumed_columns + i]
                            : "_");
    }
    return witness;
}

static bool match_pattern_row_is_covering(TypeChecker* tc,
                                          const MatchPatternRow* row,
                                          Type** column_types,
                                          int column_count) {
    if (!tc || !row || !column_types || row->column_count != column_count) {
        return false;
    }

    for (int i = 0; i < column_count; i++) {
        Expr* pattern_expr = row->columns ? row->columns[i] : NULL;
        if (!pattern_expr) {
            continue;
        }
        if (!match_pattern_is_structurally_covering(tc,
                                                    pattern_expr,
                                                    column_types[i],
                                                    true)) {
            return false;
        }
    }

    return true;
}

static MatchWitness* match_pattern_matrix_missing_witness(TypeChecker* tc,
                                                          MatchPatternRow* rows,
                                                          int row_count,
                                                          Type** column_types,
                                                          int column_count) {
    if (!tc || !rows || row_count < 0 || !column_types || column_count < 0) {
        return NULL;
    }

    if (column_count == 0) {
        return row_count > 0 ? NULL : match_witness_create(0);
    }

    for (int i = 0; i < row_count; i++) {
        if (match_pattern_row_is_covering(tc, &rows[i], column_types, column_count)) {
            return NULL;
        }
    }

    Type* head_type = typechecker_resolve_type(tc, column_types[0]);
    if (!head_type) {
        return NULL;
    }

    const char* head_enum = type_enum_name(head_type);
    if (head_enum) {
        if (!tc->globals) {
            return NULL;
        }

        for (int i = 0; i < tc->globals->symbol_count; i++) {
            Symbol* sym = tc->globals->symbols ? tc->globals->symbols[i] : NULL;
            const char* member_name = NULL;
            if (!symbol_is_enum_member_for(sym, head_enum, &member_name)) {
                continue;
            }

            Type** payload_types = NULL;
            int payload_count = 0;
            if (!match_enum_member_payload_types(tc,
                                                 head_type,
                                                 member_name,
                                                 &payload_types,
                                                 &payload_count)) {
                return NULL;
            }

            int next_column_count = payload_count + column_count - 1;
            Type** next_column_types = NULL;
            if (next_column_count > 0) {
                next_column_types =
                    (Type**)safe_calloc((size_t)next_column_count, sizeof(Type*));
                for (int j = 0; j < payload_count; j++) {
                    next_column_types[j] = payload_types[j];
                }
                for (int j = 1; j < column_count; j++) {
                    next_column_types[payload_count + j - 1] = column_types[j];
                }
            }

            MatchPatternRow* next_rows =
                (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                              sizeof(MatchPatternRow));
            int next_row_count = 0;
            for (int j = 0; j < row_count; j++) {
                Expr* head_pattern = rows[j].columns ? rows[j].columns[0] : NULL;
                bool use_wildcards =
                    !head_pattern ||
                    match_pattern_is_structurally_covering(tc, head_pattern, head_type, true);
                if (!use_wildcards) {
                    const char* pattern_member =
                        match_expr_enum_member_name(tc, head_pattern, head_enum);
                    if (!pattern_member || strcmp(pattern_member, member_name) != 0) {
                        continue;
                    }
                    if (payload_count > 0 &&
                        (head_pattern->kind != EXPR_CALL ||
                         head_pattern->call.arg_count != payload_count)) {
                        continue;
                    }
                }

                MatchPatternRow* next_row = &next_rows[next_row_count++];
                next_row->column_count = next_column_count;
                if (next_column_count > 0) {
                    next_row->columns =
                        (Expr**)safe_calloc((size_t)next_column_count, sizeof(Expr*));
                    if (!use_wildcards) {
                        for (int k = 0; k < payload_count; k++) {
                            next_row->columns[k] =
                                head_pattern->call.args ? head_pattern->call.args[k] : NULL;
                        }
                    }
                    for (int k = 1; k < column_count; k++) {
                        next_row->columns[payload_count + k - 1] =
                            rows[j].columns ? rows[j].columns[k] : NULL;
                    }
                }
            }

            MatchWitness* expanded_witness = NULL;
            if (next_row_count == 0) {
                expanded_witness =
                    match_default_witness(tc, next_column_types, next_column_count);
            } else {
                expanded_witness = match_pattern_matrix_missing_witness(tc,
                                                                        next_rows,
                                                                        next_row_count,
                                                                        next_column_types,
                                                                        next_column_count);
            }

            match_pattern_rows_free(next_rows, next_row_count);
            if (next_column_types) {
                free(next_column_types);
            }
            match_pattern_free_payload_bindings(NULL, payload_types, payload_count);

            if (expanded_witness) {
                char* head_pattern = match_format_enum_member_pattern_strings(head_enum,
                                                                              member_name,
                                                                              expanded_witness->columns,
                                                                              payload_count);
                MatchWitness* witness =
                    match_witness_collapse_head(head_pattern, expanded_witness, payload_count);
                free(head_pattern);
                match_witness_free(expanded_witness);
                return witness;
            }
        }

        return NULL;
    }

    if (head_type->kind == TYPE_BOOL) {
        for (int branch = 0; branch < 2; branch++) {
            bool branch_value = branch != 0;
            MatchPatternRow* branch_rows =
                (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                              sizeof(MatchPatternRow));
            int branch_row_count = 0;

            for (int i = 0; i < row_count; i++) {
                Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
                bool pattern_matches = false;
                bool literal_value = false;
                if (!head_pattern ||
                    match_pattern_is_structurally_covering(tc, head_pattern, head_type, true)) {
                    pattern_matches = true;
                } else if (match_expr_is_bool_literal(head_pattern, &literal_value) &&
                           literal_value == branch_value) {
                    pattern_matches = true;
                }

                if (!pattern_matches) {
                    continue;
                }

                MatchPatternRow* next_row = &branch_rows[branch_row_count++];
                next_row->column_count = column_count - 1;
                if (next_row->column_count > 0) {
                    next_row->columns =
                        (Expr**)safe_calloc((size_t)next_row->column_count, sizeof(Expr*));
                    for (int j = 1; j < column_count; j++) {
                        next_row->columns[j - 1] = rows[i].columns ? rows[i].columns[j] : NULL;
                    }
                }
            }

            MatchWitness* tail_witness = NULL;
            if (branch_row_count == 0) {
                tail_witness = match_default_witness(tc, column_types + 1, column_count - 1);
            } else {
                tail_witness = match_pattern_matrix_missing_witness(tc,
                                                                    branch_rows,
                                                                    branch_row_count,
                                                                    column_types + 1,
                                                                    column_count - 1);
            }
            match_pattern_rows_free(branch_rows, branch_row_count);
            if (tail_witness) {
                MatchWitness* witness =
                    match_witness_prepend(branch_value ? "true" : "false", tail_witness);
                match_witness_free(tail_witness);
                return witness;
            }
        }
        return NULL;
    }

    if (head_type->kind == TYPE_NIL) {
        MatchPatternRow* nil_rows =
            (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                          sizeof(MatchPatternRow));
        int nil_row_count = 0;

        for (int i = 0; i < row_count; i++) {
            Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
            if (head_pattern &&
                !match_pattern_is_structurally_covering(tc, head_pattern, head_type, true) &&
                head_pattern->kind != EXPR_NIL) {
                continue;
            }

            MatchPatternRow* next_row = &nil_rows[nil_row_count++];
            next_row->column_count = column_count - 1;
            if (next_row->column_count > 0) {
                next_row->columns =
                    (Expr**)safe_calloc((size_t)next_row->column_count, sizeof(Expr*));
                for (int j = 1; j < column_count; j++) {
                    next_row->columns[j - 1] = rows[i].columns ? rows[i].columns[j] : NULL;
                }
            }
        }

        MatchWitness* tail_witness = NULL;
        if (nil_row_count == 0) {
            tail_witness = match_default_witness(tc, column_types + 1, column_count - 1);
        } else {
            tail_witness = match_pattern_matrix_missing_witness(tc,
                                                                nil_rows,
                                                                nil_row_count,
                                                                column_types + 1,
                                                                column_count - 1);
        }
        match_pattern_rows_free(nil_rows, nil_row_count);
        if (!tail_witness) {
            return NULL;
        }
        MatchWitness* witness = match_witness_prepend("nil", tail_witness);
        match_witness_free(tail_witness);
        return witness;
    }

    if (head_type->kind == TYPE_TUPLE) {
        int arity = tuple_type_get_arity(head_type);
        int next_column_count = arity + column_count - 1;
        Type** next_column_types =
            (Type**)safe_calloc((size_t)next_column_count, sizeof(Type*));
        for (int i = 0; i < arity; i++) {
            next_column_types[i] = tuple_type_get_element(head_type, i);
        }
        for (int i = 1; i < column_count; i++) {
            next_column_types[arity + i - 1] = column_types[i];
        }

        MatchPatternRow* next_rows =
            (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                          sizeof(MatchPatternRow));
        int next_row_count = 0;
        for (int i = 0; i < row_count; i++) {
            Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
            bool use_wildcards =
                !head_pattern ||
                match_pattern_is_structurally_covering(tc, head_pattern, head_type, true);
            if (!use_wildcards &&
                (head_pattern->kind != EXPR_TUPLE_LITERAL ||
                 head_pattern->tuple_literal.element_count != arity)) {
                continue;
            }

            MatchPatternRow* next_row = &next_rows[next_row_count++];
            next_row->column_count = next_column_count;
            if (next_column_count > 0) {
                next_row->columns =
                    (Expr**)safe_calloc((size_t)next_column_count, sizeof(Expr*));
                for (int j = 0; j < arity; j++) {
                    next_row->columns[j] = use_wildcards
                        ? NULL
                        : head_pattern->tuple_literal.elements[j];
                }
                for (int j = 1; j < column_count; j++) {
                    next_row->columns[arity + j - 1] =
                        rows[i].columns ? rows[i].columns[j] : NULL;
                }
            }
        }

        MatchWitness* expanded_witness = NULL;
        if (next_row_count == 0) {
            expanded_witness = match_default_witness(tc, next_column_types, next_column_count);
        } else {
            expanded_witness = match_pattern_matrix_missing_witness(tc,
                                                                    next_rows,
                                                                    next_row_count,
                                                                    next_column_types,
                                                                    next_column_count);
        }
        match_pattern_rows_free(next_rows, next_row_count);
        free(next_column_types);
        if (!expanded_witness) {
            return NULL;
        }

        char* head_pattern =
            match_format_tuple_pattern_strings(expanded_witness->columns, arity);
        MatchWitness* witness = match_witness_collapse_head(head_pattern, expanded_witness, arity);
        free(head_pattern);
        match_witness_free(expanded_witness);
        return witness;
    }

    if (head_type->kind == TYPE_RECORD && head_type->record_def) {
        RecordDef* record_def = head_type->record_def;
        int field_count = record_def->field_count;
        int next_column_count = field_count + column_count - 1;
        Type** next_column_types =
            (Type**)safe_calloc((size_t)next_column_count, sizeof(Type*));
        for (int i = 0; i < field_count; i++) {
            next_column_types[i] = record_def->fields[i].type;
        }
        for (int i = 1; i < column_count; i++) {
            next_column_types[field_count + i - 1] = column_types[i];
        }

        MatchPatternRow* next_rows =
            (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                          sizeof(MatchPatternRow));
        int next_row_count = 0;
        for (int i = 0; i < row_count; i++) {
            Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
            bool use_wildcards =
                !head_pattern ||
                match_pattern_is_structurally_covering(tc, head_pattern, head_type, true);
            if (!use_wildcards && head_pattern->kind != EXPR_RECORD_LITERAL) {
                continue;
            }

            if (!use_wildcards) {
                Type* pattern_record_type =
                    match_pattern_resolve_record_type(tc, head_pattern, head_type);
                if (!pattern_record_type ||
                    pattern_record_type->kind != TYPE_RECORD ||
                    pattern_record_type->record_def != record_def) {
                    continue;
                }
            }

            MatchPatternRow* next_row = &next_rows[next_row_count++];
            next_row->column_count = next_column_count;
            if (next_column_count > 0) {
                next_row->columns =
                    (Expr**)safe_calloc((size_t)next_column_count, sizeof(Expr*));
            }

            if (!use_wildcards) {
                bool invalid_pattern = false;
                for (int j = 0; j < head_pattern->record_literal.field_count; j++) {
                    const char* field_name = head_pattern->record_literal.field_names
                        ? head_pattern->record_literal.field_names[j]
                        : NULL;
                    Expr* field_value = head_pattern->record_literal.field_values
                        ? head_pattern->record_literal.field_values[j]
                        : NULL;
                    int field_index =
                        field_name ? record_def_get_field_index(record_def, field_name) : -1;
                    if (field_index < 0) {
                        invalid_pattern = true;
                        break;
                    }
                    next_row->columns[field_index] = field_value;
                }
                if (invalid_pattern) {
                    free(next_row->columns);
                    next_row->columns = NULL;
                    next_row->column_count = 0;
                    next_row_count--;
                    continue;
                }
            }

            for (int j = 1; j < column_count; j++) {
                next_row->columns[field_count + j - 1] =
                    rows[i].columns ? rows[i].columns[j] : NULL;
            }
        }

        MatchWitness* expanded_witness = NULL;
        if (next_row_count == 0) {
            expanded_witness = match_default_witness(tc, next_column_types, next_column_count);
        } else {
            expanded_witness = match_pattern_matrix_missing_witness(tc,
                                                                    next_rows,
                                                                    next_row_count,
                                                                    next_column_types,
                                                                    next_column_count);
        }
        match_pattern_rows_free(next_rows, next_row_count);
        free(next_column_types);
        if (!expanded_witness) {
            return NULL;
        }

        char* head_pattern =
            match_format_record_pattern_strings(head_type, expanded_witness->columns, field_count);
        MatchWitness* witness =
            match_witness_collapse_head(head_pattern, expanded_witness, field_count);
        free(head_pattern);
        match_witness_free(expanded_witness);
        return witness;
    }

    if (head_type->kind == TYPE_INT ||
        head_type->kind == TYPE_DOUBLE ||
        head_type->kind == TYPE_STRING ||
        head_type->kind == TYPE_BIGINT) {
        int max_attempts = row_count * 2 + 5;
        for (int attempt = 0; attempt < max_attempts; attempt++) {
            MatchPatternConst candidate;
            bool owned_string = false;
            char* candidate_text = NULL;
            if (!match_build_constant_candidate(tc,
                                                head_type,
                                                attempt,
                                                &candidate,
                                                &owned_string,
                                                &candidate_text)) {
                break;
            }

            MatchPatternRow* branch_rows =
                (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                              sizeof(MatchPatternRow));
            int branch_row_count = 0;
            for (int i = 0; i < row_count; i++) {
                Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
                if (!match_pattern_matches_constant_candidate(tc,
                                                              head_pattern,
                                                              head_type,
                                                              &candidate)) {
                    continue;
                }

                MatchPatternRow* next_row = &branch_rows[branch_row_count++];
                next_row->column_count = column_count - 1;
                if (next_row->column_count > 0) {
                    next_row->columns =
                        (Expr**)safe_calloc((size_t)next_row->column_count, sizeof(Expr*));
                    for (int j = 1; j < column_count; j++) {
                        next_row->columns[j - 1] =
                            rows[i].columns ? rows[i].columns[j] : NULL;
                    }
                }
            }

            MatchWitness* tail_witness = NULL;
            if (branch_row_count == 0) {
                tail_witness = match_default_witness(tc, column_types + 1, column_count - 1);
            } else {
                tail_witness = match_pattern_matrix_missing_witness(tc,
                                                                    branch_rows,
                                                                    branch_row_count,
                                                                    column_types + 1,
                                                                    column_count - 1);
            }
            match_pattern_rows_free(branch_rows, branch_row_count);

            if (tail_witness) {
                MatchWitness* witness = match_witness_prepend(candidate_text, tail_witness);
                match_witness_free(tail_witness);
                free(candidate_text);
                match_candidate_clear_owned(&candidate, owned_string);
                return witness;
            }

            free(candidate_text);
            match_candidate_clear_owned(&candidate, owned_string);
        }
        return NULL;
    }

    MatchPatternRow* wildcard_rows =
        (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                      sizeof(MatchPatternRow));
    int wildcard_row_count = 0;
    for (int i = 0; i < row_count; i++) {
        Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
        if (head_pattern &&
            !match_pattern_is_structurally_covering(tc, head_pattern, head_type, true)) {
            continue;
        }

        MatchPatternRow* next_row = &wildcard_rows[wildcard_row_count++];
        next_row->column_count = column_count - 1;
        if (next_row->column_count > 0) {
            next_row->columns =
                (Expr**)safe_calloc((size_t)next_row->column_count, sizeof(Expr*));
            for (int j = 1; j < column_count; j++) {
                next_row->columns[j - 1] = rows[i].columns ? rows[i].columns[j] : NULL;
            }
        }
    }

    MatchWitness* tail_witness = NULL;
    if (wildcard_row_count == 0) {
        tail_witness = match_default_witness(tc, column_types + 1, column_count - 1);
    } else {
        tail_witness = match_pattern_matrix_missing_witness(tc,
                                                            wildcard_rows,
                                                            wildcard_row_count,
                                                            column_types + 1,
                                                            column_count - 1);
    }
    match_pattern_rows_free(wildcard_rows, wildcard_row_count);
    if (!tail_witness) {
        return NULL;
    }

    char* head_pattern = match_default_pattern_for_type(tc, head_type);
    MatchWitness* witness = match_witness_prepend(head_pattern, tail_witness);
    free(head_pattern);
    match_witness_free(tail_witness);
    return witness;
}

static bool match_required_pattern_is_wildcard(TypeChecker* tc,
                                               Expr* pattern_expr,
                                               Type* subject_type) {
    return !pattern_expr ||
           match_pattern_is_structurally_covering(tc, pattern_expr, subject_type, true);
}

static MatchWitness* match_pattern_matrix_missing_witness_in_space(TypeChecker* tc,
                                                                   MatchPatternRow* rows,
                                                                   int row_count,
                                                                   Type** column_types,
                                                                   Expr** required_columns,
                                                                   int column_count) {
    if (!tc || !rows || row_count < 0 || !column_types || !required_columns || column_count < 0) {
        return NULL;
    }

    if (column_count == 0) {
        return row_count > 0 ? NULL : match_witness_create(0);
    }

    bool all_required_wildcards = true;
    for (int i = 0; i < column_count; i++) {
        Type* required_type = typechecker_resolve_type(tc, column_types[i]);
        if (!match_required_pattern_is_wildcard(tc, required_columns[i], required_type)) {
            all_required_wildcards = false;
            break;
        }
    }
    if (all_required_wildcards) {
        if (row_count == 0) {
            return match_default_witness(tc, column_types, column_count);
        }
        return match_pattern_matrix_missing_witness(tc,
                                                    rows,
                                                    row_count,
                                                    column_types,
                                                    column_count);
    }

    Type* head_type = typechecker_resolve_type(tc, column_types[0]);
    if (!head_type) {
        return NULL;
    }

    Expr* required_head = required_columns[0];
    bool required_wildcard =
        match_required_pattern_is_wildcard(tc, required_head, head_type);

    const char* head_enum = type_enum_name(head_type);
    if (head_enum) {
        if (!tc->globals) {
            return NULL;
        }

        const char* required_member = NULL;
        if (!required_wildcard) {
            required_member = match_expr_enum_member_name(tc, required_head, head_enum);
            if (!required_member) {
                return NULL;
            }
        }

        for (int i = 0; i < tc->globals->symbol_count; i++) {
            Symbol* sym = tc->globals->symbols ? tc->globals->symbols[i] : NULL;
            const char* member_name = NULL;
            if (!symbol_is_enum_member_for(sym, head_enum, &member_name)) {
                continue;
            }
            if (required_member && strcmp(required_member, member_name) != 0) {
                continue;
            }

            Type** payload_types = NULL;
            int payload_count = 0;
            if (!match_enum_member_payload_types(tc,
                                                 head_type,
                                                 member_name,
                                                 &payload_types,
                                                 &payload_count)) {
                return NULL;
            }

            int next_column_count = payload_count + column_count - 1;
            Type** next_column_types = NULL;
            Expr** next_required_columns = NULL;
            if (next_column_count > 0) {
                next_column_types =
                    (Type**)safe_calloc((size_t)next_column_count, sizeof(Type*));
                next_required_columns =
                    (Expr**)safe_calloc((size_t)next_column_count, sizeof(Expr*));
                for (int j = 0; j < payload_count; j++) {
                    next_column_types[j] = payload_types[j];
                    if (!required_wildcard &&
                        required_head &&
                        required_head->kind == EXPR_CALL &&
                        required_head->call.arg_count == payload_count) {
                        next_required_columns[j] =
                            required_head->call.args ? required_head->call.args[j] : NULL;
                    }
                }
                for (int j = 1; j < column_count; j++) {
                    next_column_types[payload_count + j - 1] = column_types[j];
                    next_required_columns[payload_count + j - 1] = required_columns[j];
                }
            }

            MatchPatternRow* next_rows =
                (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                              sizeof(MatchPatternRow));
            int next_row_count = 0;
            for (int j = 0; j < row_count; j++) {
                Expr* head_pattern = rows[j].columns ? rows[j].columns[0] : NULL;
                bool use_wildcards =
                    !head_pattern ||
                    match_pattern_is_structurally_covering(tc, head_pattern, head_type, true);
                if (!use_wildcards) {
                    const char* pattern_member =
                        match_expr_enum_member_name(tc, head_pattern, head_enum);
                    if (!pattern_member || strcmp(pattern_member, member_name) != 0) {
                        continue;
                    }
                    if (payload_count > 0 &&
                        (head_pattern->kind != EXPR_CALL ||
                         head_pattern->call.arg_count != payload_count)) {
                        continue;
                    }
                }

                MatchPatternRow* next_row = &next_rows[next_row_count++];
                next_row->column_count = next_column_count;
                if (next_column_count > 0) {
                    next_row->columns =
                        (Expr**)safe_calloc((size_t)next_column_count, sizeof(Expr*));
                    if (!use_wildcards) {
                        for (int k = 0; k < payload_count; k++) {
                            next_row->columns[k] =
                                head_pattern->call.args ? head_pattern->call.args[k] : NULL;
                        }
                    }
                    for (int k = 1; k < column_count; k++) {
                        next_row->columns[payload_count + k - 1] =
                            rows[j].columns ? rows[j].columns[k] : NULL;
                    }
                }
            }

            MatchWitness* expanded_witness = match_pattern_matrix_missing_witness_in_space(
                tc,
                next_rows,
                next_row_count,
                next_column_types,
                next_required_columns,
                next_column_count);

            match_pattern_rows_free(next_rows, next_row_count);
            if (next_column_types) {
                free(next_column_types);
            }
            if (next_required_columns) {
                free(next_required_columns);
            }
            match_pattern_free_payload_bindings(NULL, payload_types, payload_count);

            if (expanded_witness) {
                char* head_pattern = match_format_enum_member_pattern_strings(head_enum,
                                                                              member_name,
                                                                              expanded_witness->columns,
                                                                              payload_count);
                MatchWitness* witness =
                    match_witness_collapse_head(head_pattern, expanded_witness, payload_count);
                free(head_pattern);
                match_witness_free(expanded_witness);
                return witness;
            }
        }

        return NULL;
    }

    if (head_type->kind == TYPE_BOOL) {
        bool branch_values[2] = {false, true};
        int branch_count = 2;
        if (!required_wildcard) {
            if (!match_expr_is_bool_literal(required_head, &branch_values[0])) {
                return NULL;
            }
            branch_count = 1;
        }

        for (int branch = 0; branch < branch_count; branch++) {
            bool branch_value = branch_values[branch];
            MatchPatternRow* branch_rows =
                (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                              sizeof(MatchPatternRow));
            int branch_row_count = 0;

            for (int i = 0; i < row_count; i++) {
                Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
                bool pattern_matches = false;
                bool literal_value = false;
                if (!head_pattern ||
                    match_pattern_is_structurally_covering(tc, head_pattern, head_type, true)) {
                    pattern_matches = true;
                } else if (match_expr_is_bool_literal(head_pattern, &literal_value) &&
                           literal_value == branch_value) {
                    pattern_matches = true;
                }

                if (!pattern_matches) {
                    continue;
                }

                MatchPatternRow* next_row = &branch_rows[branch_row_count++];
                next_row->column_count = column_count - 1;
                if (next_row->column_count > 0) {
                    next_row->columns =
                        (Expr**)safe_calloc((size_t)next_row->column_count, sizeof(Expr*));
                    for (int j = 1; j < column_count; j++) {
                        next_row->columns[j - 1] = rows[i].columns ? rows[i].columns[j] : NULL;
                    }
                }
            }

            MatchWitness* tail_witness = match_pattern_matrix_missing_witness_in_space(
                tc,
                branch_rows,
                branch_row_count,
                column_types + 1,
                required_columns + 1,
                column_count - 1);
            match_pattern_rows_free(branch_rows, branch_row_count);
            if (tail_witness) {
                MatchWitness* witness =
                    match_witness_prepend(branch_value ? "true" : "false", tail_witness);
                match_witness_free(tail_witness);
                return witness;
            }
        }
        return NULL;
    }

    if (head_type->kind == TYPE_NIL) {
        if (!required_wildcard && required_head->kind != EXPR_NIL) {
            return NULL;
        }

        MatchPatternRow* nil_rows =
            (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                          sizeof(MatchPatternRow));
        int nil_row_count = 0;

        for (int i = 0; i < row_count; i++) {
            Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
            if (head_pattern &&
                !match_pattern_is_structurally_covering(tc, head_pattern, head_type, true) &&
                head_pattern->kind != EXPR_NIL) {
                continue;
            }

            MatchPatternRow* next_row = &nil_rows[nil_row_count++];
            next_row->column_count = column_count - 1;
            if (next_row->column_count > 0) {
                next_row->columns =
                    (Expr**)safe_calloc((size_t)next_row->column_count, sizeof(Expr*));
                for (int j = 1; j < column_count; j++) {
                    next_row->columns[j - 1] = rows[i].columns ? rows[i].columns[j] : NULL;
                }
            }
        }

        MatchWitness* tail_witness = match_pattern_matrix_missing_witness_in_space(
            tc,
            nil_rows,
            nil_row_count,
            column_types + 1,
            required_columns + 1,
            column_count - 1);
        match_pattern_rows_free(nil_rows, nil_row_count);
        if (!tail_witness) {
            return NULL;
        }
        MatchWitness* witness = match_witness_prepend("nil", tail_witness);
        match_witness_free(tail_witness);
        return witness;
    }

    if (head_type->kind == TYPE_TUPLE) {
        int arity = tuple_type_get_arity(head_type);
        if (!required_wildcard &&
            (required_head->kind != EXPR_TUPLE_LITERAL ||
             required_head->tuple_literal.element_count != arity)) {
            return NULL;
        }

        int next_column_count = arity + column_count - 1;
        Type** next_column_types =
            (Type**)safe_calloc((size_t)next_column_count, sizeof(Type*));
        Expr** next_required_columns =
            (Expr**)safe_calloc((size_t)next_column_count, sizeof(Expr*));
        for (int i = 0; i < arity; i++) {
            next_column_types[i] = tuple_type_get_element(head_type, i);
            if (!required_wildcard) {
                next_required_columns[i] =
                    required_head->tuple_literal.elements
                        ? required_head->tuple_literal.elements[i]
                        : NULL;
            }
        }
        for (int i = 1; i < column_count; i++) {
            next_column_types[arity + i - 1] = column_types[i];
            next_required_columns[arity + i - 1] = required_columns[i];
        }

        MatchPatternRow* next_rows =
            (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                          sizeof(MatchPatternRow));
        int next_row_count = 0;
        for (int i = 0; i < row_count; i++) {
            Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
            bool use_wildcards =
                !head_pattern ||
                match_pattern_is_structurally_covering(tc, head_pattern, head_type, true);
            if (!use_wildcards &&
                (head_pattern->kind != EXPR_TUPLE_LITERAL ||
                 head_pattern->tuple_literal.element_count != arity)) {
                continue;
            }

            MatchPatternRow* next_row = &next_rows[next_row_count++];
            next_row->column_count = next_column_count;
            if (next_column_count > 0) {
                next_row->columns =
                    (Expr**)safe_calloc((size_t)next_column_count, sizeof(Expr*));
                for (int j = 0; j < arity; j++) {
                    next_row->columns[j] = use_wildcards
                        ? NULL
                        : head_pattern->tuple_literal.elements[j];
                }
                for (int j = 1; j < column_count; j++) {
                    next_row->columns[arity + j - 1] =
                        rows[i].columns ? rows[i].columns[j] : NULL;
                }
            }
        }

        MatchWitness* expanded_witness = match_pattern_matrix_missing_witness_in_space(
            tc,
            next_rows,
            next_row_count,
            next_column_types,
            next_required_columns,
            next_column_count);
        match_pattern_rows_free(next_rows, next_row_count);
        free(next_column_types);
        free(next_required_columns);
        if (!expanded_witness) {
            return NULL;
        }

        char* head_pattern =
            match_format_tuple_pattern_strings(expanded_witness->columns, arity);
        MatchWitness* witness = match_witness_collapse_head(head_pattern, expanded_witness, arity);
        free(head_pattern);
        match_witness_free(expanded_witness);
        return witness;
    }

    if (head_type->kind == TYPE_RECORD && head_type->record_def) {
        RecordDef* record_def = head_type->record_def;
        int field_count = record_def->field_count;
        if (!required_wildcard) {
            if (required_head->kind != EXPR_RECORD_LITERAL) {
                return NULL;
            }
            Type* required_record_type =
                match_pattern_resolve_record_type(tc, required_head, head_type);
            if (!required_record_type ||
                required_record_type->kind != TYPE_RECORD ||
                required_record_type->record_def != record_def) {
                return NULL;
            }
        }

        int next_column_count = field_count + column_count - 1;
        Type** next_column_types =
            (Type**)safe_calloc((size_t)next_column_count, sizeof(Type*));
        Expr** next_required_columns =
            (Expr**)safe_calloc((size_t)next_column_count, sizeof(Expr*));
        for (int i = 0; i < field_count; i++) {
            next_column_types[i] = record_def->fields[i].type;
        }
        if (!required_wildcard) {
            for (int i = 0; i < required_head->record_literal.field_count; i++) {
                const char* field_name = required_head->record_literal.field_names
                    ? required_head->record_literal.field_names[i]
                    : NULL;
                int field_index = field_name
                    ? record_def_get_field_index(record_def, field_name)
                    : -1;
                if (field_index >= 0) {
                    next_required_columns[field_index] =
                        required_head->record_literal.field_values
                            ? required_head->record_literal.field_values[i]
                            : NULL;
                }
            }
        }
        for (int i = 1; i < column_count; i++) {
            next_column_types[field_count + i - 1] = column_types[i];
            next_required_columns[field_count + i - 1] = required_columns[i];
        }

        MatchPatternRow* next_rows =
            (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                          sizeof(MatchPatternRow));
        int next_row_count = 0;
        for (int i = 0; i < row_count; i++) {
            Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
            bool use_wildcards =
                !head_pattern ||
                match_pattern_is_structurally_covering(tc, head_pattern, head_type, true);
            if (!use_wildcards && head_pattern->kind != EXPR_RECORD_LITERAL) {
                continue;
            }

            if (!use_wildcards) {
                Type* pattern_record_type =
                    match_pattern_resolve_record_type(tc, head_pattern, head_type);
                if (!pattern_record_type ||
                    pattern_record_type->kind != TYPE_RECORD ||
                    pattern_record_type->record_def != record_def) {
                    continue;
                }
            }

            MatchPatternRow* next_row = &next_rows[next_row_count++];
            next_row->column_count = next_column_count;
            if (next_column_count > 0) {
                next_row->columns =
                    (Expr**)safe_calloc((size_t)next_column_count, sizeof(Expr*));
            }

            if (!use_wildcards) {
                bool invalid_pattern = false;
                for (int j = 0; j < head_pattern->record_literal.field_count; j++) {
                    const char* field_name = head_pattern->record_literal.field_names
                        ? head_pattern->record_literal.field_names[j]
                        : NULL;
                    Expr* field_value = head_pattern->record_literal.field_values
                        ? head_pattern->record_literal.field_values[j]
                        : NULL;
                    int field_index =
                        field_name ? record_def_get_field_index(record_def, field_name) : -1;
                    if (field_index < 0) {
                        invalid_pattern = true;
                        break;
                    }
                    next_row->columns[field_index] = field_value;
                }
                if (invalid_pattern) {
                    free(next_row->columns);
                    next_row->columns = NULL;
                    next_row->column_count = 0;
                    next_row_count--;
                    continue;
                }
            }

            for (int j = 1; j < column_count; j++) {
                next_row->columns[field_count + j - 1] =
                    rows[i].columns ? rows[i].columns[j] : NULL;
            }
        }

        MatchWitness* expanded_witness = match_pattern_matrix_missing_witness_in_space(
            tc,
            next_rows,
            next_row_count,
            next_column_types,
            next_required_columns,
            next_column_count);
        match_pattern_rows_free(next_rows, next_row_count);
        free(next_column_types);
        free(next_required_columns);
        if (!expanded_witness) {
            return NULL;
        }

        char* head_pattern =
            match_format_record_pattern_strings(head_type, expanded_witness->columns, field_count);
        MatchWitness* witness =
            match_witness_collapse_head(head_pattern, expanded_witness, field_count);
        free(head_pattern);
        match_witness_free(expanded_witness);
        return witness;
    }

    if (head_type->kind == TYPE_INT ||
        head_type->kind == TYPE_DOUBLE ||
        head_type->kind == TYPE_STRING ||
        head_type->kind == TYPE_BIGINT) {
        int max_attempts = required_wildcard ? (row_count * 2 + 5) : 1;
        for (int attempt = 0; attempt < max_attempts; attempt++) {
            MatchPatternConst candidate;
            bool owned_string = false;
            char* candidate_text = NULL;

            if (required_wildcard) {
                if (!match_build_constant_candidate(tc,
                                                    head_type,
                                                    attempt,
                                                    &candidate,
                                                    &owned_string,
                                                    &candidate_text)) {
                    break;
                }
            } else {
                if (!match_expr_constant_key(tc, required_head, &candidate) ||
                    candidate.kind == MATCH_PATTERN_CONST_NONE) {
                    return NULL;
                }
                char literal_buf[256];
                match_pattern_constant_format(&candidate, literal_buf, sizeof(literal_buf));
                candidate_text = safe_strdup(literal_buf);
            }

            MatchPatternRow* branch_rows =
                (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                              sizeof(MatchPatternRow));
            int branch_row_count = 0;
            for (int i = 0; i < row_count; i++) {
                Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
                if (!match_pattern_matches_constant_candidate(tc,
                                                              head_pattern,
                                                              head_type,
                                                              &candidate)) {
                    continue;
                }

                MatchPatternRow* next_row = &branch_rows[branch_row_count++];
                next_row->column_count = column_count - 1;
                if (next_row->column_count > 0) {
                    next_row->columns =
                        (Expr**)safe_calloc((size_t)next_row->column_count, sizeof(Expr*));
                    for (int j = 1; j < column_count; j++) {
                        next_row->columns[j - 1] =
                            rows[i].columns ? rows[i].columns[j] : NULL;
                    }
                }
            }

            MatchWitness* tail_witness = match_pattern_matrix_missing_witness_in_space(
                tc,
                branch_rows,
                branch_row_count,
                column_types + 1,
                required_columns + 1,
                column_count - 1);
            match_pattern_rows_free(branch_rows, branch_row_count);

            if (tail_witness) {
                MatchWitness* witness = match_witness_prepend(candidate_text, tail_witness);
                match_witness_free(tail_witness);
                free(candidate_text);
                match_candidate_clear_owned(&candidate, owned_string);
                return witness;
            }

            free(candidate_text);
            match_candidate_clear_owned(&candidate, owned_string);
        }
        return NULL;
    }

    if (!required_wildcard) {
        return NULL;
    }

    MatchPatternRow* wildcard_rows =
        (MatchPatternRow*)safe_calloc((size_t)(row_count > 0 ? row_count : 1),
                                      sizeof(MatchPatternRow));
    int wildcard_row_count = 0;
    for (int i = 0; i < row_count; i++) {
        Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
        if (head_pattern &&
            !match_pattern_is_structurally_covering(tc, head_pattern, head_type, true)) {
            continue;
        }

        MatchPatternRow* next_row = &wildcard_rows[wildcard_row_count++];
        next_row->column_count = column_count - 1;
        if (next_row->column_count > 0) {
            next_row->columns =
                (Expr**)safe_calloc((size_t)next_row->column_count, sizeof(Expr*));
            for (int j = 1; j < column_count; j++) {
                next_row->columns[j - 1] = rows[i].columns ? rows[i].columns[j] : NULL;
            }
        }
    }

    MatchWitness* tail_witness = match_pattern_matrix_missing_witness_in_space(tc,
                                                                                wildcard_rows,
                                                                                wildcard_row_count,
                                                                                column_types + 1,
                                                                                required_columns + 1,
                                                                                column_count - 1);
    match_pattern_rows_free(wildcard_rows, wildcard_row_count);
    if (!tail_witness) {
        return NULL;
    }

    char* head_pattern = match_default_pattern_for_type(tc, head_type);
    MatchWitness* witness = match_witness_prepend(head_pattern, tail_witness);
    free(head_pattern);
    match_witness_free(tail_witness);
    return witness;
}

static char* match_missing_witness_within_pattern(TypeChecker* tc,
                                                  Expr** patterns,
                                                  int pattern_count,
                                                  Type* subject_type,
                                                  Expr* required_pattern) {
    if (!tc || !subject_type || !required_pattern) {
        return NULL;
    }

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    if (!resolved_subject_type) {
        return NULL;
    }

    MatchPatternRow* rows =
        (MatchPatternRow*)safe_calloc((size_t)((pattern_count > 0) ? pattern_count : 1),
                                      sizeof(MatchPatternRow));
    for (int i = 0; i < pattern_count; i++) {
        rows[i].column_count = 1;
        rows[i].columns = (Expr**)safe_calloc(1, sizeof(Expr*));
        rows[i].columns[0] = patterns[i];
    }

    Type* column_types[1];
    Expr* required_columns[1];
    column_types[0] = resolved_subject_type;
    required_columns[0] = required_pattern;
    MatchWitness* witness = match_pattern_matrix_missing_witness_in_space(tc,
                                                                          rows,
                                                                          pattern_count,
                                                                          column_types,
                                                                          required_columns,
                                                                          1);
    match_pattern_rows_free(rows, pattern_count);

    if (!witness || witness->column_count <= 0 || !witness->columns || !witness->columns[0]) {
        match_witness_free(witness);
        return NULL;
    }

    char* out = safe_strdup(witness->columns[0]);
    match_witness_free(witness);
    return out;
}

static char* match_enum_member_missing_witness(TypeChecker* tc,
                                               Expr** patterns,
                                               int pattern_count,
                                               Type* subject_type,
                                               const char* member_name) {
    if (!tc || !subject_type || !member_name) {
        return NULL;
    }

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    const char* subject_enum = type_enum_name(resolved_subject_type);
    if (!resolved_subject_type || !subject_enum) {
        return NULL;
    }

    Type** payload_types = NULL;
    int payload_count = 0;
    if (!match_enum_member_payload_types(tc,
                                         resolved_subject_type,
                                         member_name,
                                         &payload_types,
                                         &payload_count)) {
        return NULL;
    }

    MatchPatternRow* rows =
        (MatchPatternRow*)safe_calloc((size_t)((pattern_count > 0) ? pattern_count : 1),
                                      sizeof(MatchPatternRow));
    int row_count = 0;

    for (int i = 0; i < pattern_count; i++) {
        Expr* pattern_expr = patterns[i];
        bool use_wildcards =
            !pattern_expr ||
            match_pattern_is_structurally_covering(tc, pattern_expr, resolved_subject_type, true);
        if (!use_wildcards) {
            const char* pattern_member = match_expr_enum_member_name(tc, pattern_expr, subject_enum);
            if (!pattern_member || strcmp(pattern_member, member_name) != 0) {
                continue;
            }
            if (payload_count > 0 &&
                (pattern_expr->kind != EXPR_CALL || pattern_expr->call.arg_count != payload_count)) {
                continue;
            }
        }

        MatchPatternRow* row = &rows[row_count++];
        row->column_count = payload_count;
        if (payload_count > 0) {
            row->columns = (Expr**)safe_calloc((size_t)payload_count, sizeof(Expr*));
            if (!use_wildcards) {
                for (int j = 0; j < payload_count; j++) {
                    row->columns[j] = pattern_expr->call.args ? pattern_expr->call.args[j] : NULL;
                }
            }
        }
    }

    MatchWitness* payload_witness = NULL;
    if (payload_count == 0) {
        if (row_count == 0) {
            payload_witness = match_witness_create(0);
        }
    } else if (row_count == 0) {
        payload_witness = match_default_witness(tc, payload_types, payload_count);
    } else {
        payload_witness = match_pattern_matrix_missing_witness(tc,
                                                               rows,
                                                               row_count,
                                                               payload_types,
                                                               payload_count);
    }

    char* witness = NULL;
    if (payload_witness) {
        witness = match_format_enum_member_pattern_strings(subject_enum,
                                                           member_name,
                                                           payload_witness->columns,
                                                           payload_count);
    }

    match_witness_free(payload_witness);
    match_pattern_rows_free(rows, row_count);
    match_pattern_free_payload_bindings(NULL, payload_types, payload_count);
    return witness;
}

static char* match_enum_non_exhaustive_message(TypeChecker* tc,
                                               Expr** patterns,
                                               int pattern_count,
                                               Type* subject_type) {
    if (!tc || !subject_type) {
        return NULL;
    }

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    const char* subject_enum = type_enum_name(resolved_subject_type);
    if (!resolved_subject_type || !subject_enum || !tc->globals) {
        return NULL;
    }

    char** missing_patterns = NULL;
    int missing_count = 0;
    size_t missing_chars = 0;

    for (int i = 0; i < tc->globals->symbol_count; i++) {
        Symbol* sym = tc->globals->symbols ? tc->globals->symbols[i] : NULL;
        const char* member_name = NULL;
        if (!symbol_is_enum_member_for(sym, subject_enum, &member_name)) {
            continue;
        }

        if (match_enum_member_patterns_exhaustive(tc,
                                                  patterns,
                                                  pattern_count,
                                                  resolved_subject_type,
                                                  member_name)) {
            continue;
        }

        char* missing_pattern = match_enum_member_missing_witness(tc,
                                                                  patterns,
                                                                  pattern_count,
                                                                  resolved_subject_type,
                                                                  member_name);
        if (!missing_pattern) {
            missing_pattern = match_format_enum_member_pattern_strings(subject_enum,
                                                                      member_name,
                                                                      NULL,
                                                                      0);
        }

        missing_patterns = (char**)safe_realloc(missing_patterns,
                                                (size_t)(missing_count + 1) * sizeof(char*));
        missing_patterns[missing_count++] = missing_pattern;
        missing_chars += strlen(missing_pattern) + 2;
    }

    if (missing_count == 0) {
        free(missing_patterns);
        return NULL;
    }

    const char* prefix = "Non-exhaustive enum match";
    size_t prefix_len = strlen(prefix);
    size_t msg_cap = prefix_len + strlen(subject_enum) + missing_chars + 20;
    char* msg = (char*)safe_malloc(msg_cap);
    size_t pos = (size_t)snprintf(msg, msg_cap, "%s for '%s': missing ", prefix, subject_enum);
    for (int i = 0; i < missing_count; i++) {
        if (i > 0) {
            msg[pos++] = ',';
            msg[pos++] = ' ';
        }
        size_t len = strlen(missing_patterns[i]);
        memcpy(msg + pos, missing_patterns[i], len);
        pos += len;
        free(missing_patterns[i]);
    }
    msg[pos] = '\0';
    free(missing_patterns);
    return msg;
}

static char* match_structural_non_exhaustive_message(TypeChecker* tc,
                                                     Expr** patterns,
                                                     int pattern_count,
                                                     Type* subject_type) {
    if (!tc || !subject_type) {
        return NULL;
    }

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    if (!resolved_subject_type ||
        (resolved_subject_type->kind != TYPE_TUPLE &&
         resolved_subject_type->kind != TYPE_RECORD)) {
        return NULL;
    }

    MatchPatternRow* rows =
        (MatchPatternRow*)safe_calloc((size_t)((pattern_count > 0) ? pattern_count : 1),
                                      sizeof(MatchPatternRow));
    for (int i = 0; i < pattern_count; i++) {
        rows[i].column_count = 1;
        rows[i].columns = (Expr**)safe_calloc(1, sizeof(Expr*));
        rows[i].columns[0] = patterns[i];
    }

    Type* column_types[1];
    column_types[0] = resolved_subject_type;
    MatchWitness* witness = NULL;
    if (pattern_count > 0) {
        witness = match_pattern_matrix_missing_witness(tc,
                                                       rows,
                                                       pattern_count,
                                                       column_types,
                                                       1);
    } else {
        witness = match_default_witness(tc, column_types, 1);
    }
    match_pattern_rows_free(rows, pattern_count);

    if (!witness || witness->column_count <= 0 || !witness->columns || !witness->columns[0]) {
        match_witness_free(witness);
        return NULL;
    }

    const char* prefix = "Non-exhaustive match expression";
    const char* suffix = " or an else branch";
    size_t total = strlen(prefix) + strlen(": missing ") + strlen(witness->columns[0]) + strlen(suffix) + 1;
    char* msg = (char*)safe_malloc(total);
    snprintf(msg,
             total,
             "%s: missing %s%s",
             prefix,
             witness->columns[0],
             suffix);
    match_witness_free(witness);
    return msg;
}

static char* match_first_missing_witness(TypeChecker* tc,
                                         Expr** patterns,
                                         int pattern_count,
                                         Type* subject_type) {
    if (!tc || !subject_type) {
        return NULL;
    }

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    if (!resolved_subject_type) {
        return NULL;
    }

    MatchPatternRow* rows =
        (MatchPatternRow*)safe_calloc((size_t)((pattern_count > 0) ? pattern_count : 1),
                                      sizeof(MatchPatternRow));
    for (int i = 0; i < pattern_count; i++) {
        rows[i].column_count = 1;
        rows[i].columns = (Expr**)safe_calloc(1, sizeof(Expr*));
        rows[i].columns[0] = patterns[i];
    }

    Type* column_types[1];
    column_types[0] = resolved_subject_type;
    MatchWitness* witness = NULL;
    if (pattern_count > 0) {
        witness = match_pattern_matrix_missing_witness(tc,
                                                       rows,
                                                       pattern_count,
                                                       column_types,
                                                       1);
    } else {
        witness = match_default_witness(tc, column_types, 1);
    }
    match_pattern_rows_free(rows, pattern_count);

    if (!witness || witness->column_count <= 0 || !witness->columns || !witness->columns[0]) {
        match_witness_free(witness);
        return NULL;
    }

    char* out = safe_strdup(witness->columns[0]);
    match_witness_free(witness);
    return out;
}

static char* match_guarded_missing_witness(TypeChecker* tc,
                                           Expr** unguarded_patterns,
                                           int unguarded_pattern_count,
                                           Expr** guarded_patterns,
                                           int guarded_pattern_count,
                                           Type* subject_type) {
    if (tc && subject_type && guarded_patterns && guarded_pattern_count > 0) {
        Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < guarded_pattern_count; i++) {
                Expr* guarded_pattern = guarded_patterns[i];
                bool is_covering = guarded_pattern &&
                    match_pattern_is_structurally_covering(tc,
                                                           guarded_pattern,
                                                           resolved_subject_type,
                                                           true);
                if ((pass == 0 && is_covering) || (pass == 1 && !is_covering)) {
                    continue;
                }

                char* witness = match_missing_witness_within_pattern(tc,
                                                                     unguarded_patterns,
                                                                     unguarded_pattern_count,
                                                                     subject_type,
                                                                     guarded_pattern);
                if (witness) {
                    return witness;
                }
            }
        }
    }

    return match_first_missing_witness(tc,
                                       unguarded_patterns,
                                       unguarded_pattern_count,
                                       subject_type);
}

static char* match_guarded_non_exhaustive_message(TypeChecker* tc,
                                                  Expr** unguarded_patterns,
                                                  int unguarded_pattern_count,
                                                  Expr** guarded_patterns,
                                                  int guarded_pattern_count,
                                                  Type* subject_type,
                                                  bool is_expression) {
    char* witness = match_guarded_missing_witness(tc,
                                                  unguarded_patterns,
                                                  unguarded_pattern_count,
                                                  guarded_patterns,
                                                  guarded_pattern_count,
                                                  subject_type);
    if (!witness) {
        return NULL;
    }

    const char* prefix = is_expression
        ? "Non-exhaustive match expression"
        : "Non-exhaustive match";
    const char* suffix = " when a guard is false or an else branch";
    size_t total = strlen(prefix) + strlen(": missing ") + strlen(witness) + strlen(suffix) + 1;
    char* msg = (char*)safe_malloc(total);
    snprintf(msg,
             total,
             "%s: missing %s%s",
             prefix,
             witness,
             suffix);
    free(witness);
    return msg;
}

static bool match_enum_member_patterns_exhaustive(TypeChecker* tc,
                                                  Expr** patterns,
                                                  int pattern_count,
                                                  Type* subject_type,
                                                  const char* member_name) {
    if (!tc || !patterns || pattern_count <= 0 || !subject_type || !member_name) {
        return false;
    }

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    const char* subject_enum = type_enum_name(resolved_subject_type);
    if (!resolved_subject_type || !subject_enum) {
        return false;
    }

    Type** payload_types = NULL;
    int payload_count = 0;
    if (!match_enum_member_payload_types(tc,
                                         resolved_subject_type,
                                         member_name,
                                         &payload_types,
                                         &payload_count)) {
        return false;
    }

    MatchPatternRow* rows =
        (MatchPatternRow*)safe_calloc((size_t)pattern_count, sizeof(MatchPatternRow));
    int row_count = 0;

    for (int i = 0; i < pattern_count; i++) {
        Expr* pattern_expr = patterns[i];
        bool use_wildcards =
            !pattern_expr ||
            match_pattern_is_structurally_covering(tc, pattern_expr, resolved_subject_type, true);
        if (!use_wildcards) {
            const char* pattern_member = match_expr_enum_member_name(tc, pattern_expr, subject_enum);
            if (!pattern_member || strcmp(pattern_member, member_name) != 0) {
                continue;
            }
            if (payload_count > 0 &&
                (pattern_expr->kind != EXPR_CALL || pattern_expr->call.arg_count != payload_count)) {
                continue;
            }
        }

        MatchPatternRow* row = &rows[row_count++];
        row->column_count = payload_count;
        if (payload_count > 0) {
            row->columns = (Expr**)safe_calloc((size_t)payload_count, sizeof(Expr*));
            if (!use_wildcards) {
                for (int j = 0; j < payload_count; j++) {
                    row->columns[j] = pattern_expr->call.args ? pattern_expr->call.args[j] : NULL;
                }
            }
        }
    }

    bool exhaustive = false;
    if (row_count > 0) {
        exhaustive = (payload_count == 0) ||
                     match_pattern_matrix_exhaustive(tc,
                                                     rows,
                                                     row_count,
                                                     payload_types,
                                                     payload_count);
    }

    match_pattern_rows_free(rows, row_count);
    match_pattern_free_payload_bindings(NULL, payload_types, payload_count);
    return exhaustive;
}

static bool match_pattern_matrix_exhaustive(TypeChecker* tc,
                                            MatchPatternRow* rows,
                                            int row_count,
                                            Type** column_types,
                                            int column_count) {
    if (!tc || !rows || row_count <= 0 || !column_types || column_count < 0) {
        return false;
    }
    if (column_count == 0) {
        return row_count > 0;
    }

    for (int i = 0; i < row_count; i++) {
        if (match_pattern_row_is_covering(tc, &rows[i], column_types, column_count)) {
            return true;
        }
    }

    Type* head_type = typechecker_resolve_type(tc, column_types[0]);
    if (!head_type) {
        return false;
    }

    const char* head_enum = type_enum_name(head_type);
    if (head_enum) {
        bool saw_member = false;
        if (!tc || !tc->globals) {
            return false;
        }

        for (int i = 0; i < tc->globals->symbol_count; i++) {
            Symbol* sym = tc->globals->symbols ? tc->globals->symbols[i] : NULL;
            const char* member_name = NULL;
            if (!symbol_is_enum_member_for(sym, head_enum, &member_name)) {
                continue;
            }

            saw_member = true;

            Type** payload_types = NULL;
            int payload_count = 0;
            if (!match_enum_member_payload_types(tc,
                                                 head_type,
                                                 member_name,
                                                 &payload_types,
                                                 &payload_count)) {
                return false;
            }

            int next_column_count = payload_count + column_count - 1;
            Type** next_column_types = NULL;
            if (next_column_count > 0) {
                next_column_types =
                    (Type**)safe_calloc((size_t)next_column_count, sizeof(Type*));
                for (int j = 0; j < payload_count; j++) {
                    next_column_types[j] = payload_types[j];
                }
                for (int j = 1; j < column_count; j++) {
                    next_column_types[payload_count + j - 1] = column_types[j];
                }
            }

            MatchPatternRow* next_rows =
                (MatchPatternRow*)safe_calloc((size_t)row_count, sizeof(MatchPatternRow));
            int next_row_count = 0;

            for (int j = 0; j < row_count; j++) {
                Expr* head_pattern = rows[j].columns ? rows[j].columns[0] : NULL;
                bool use_wildcards =
                    !head_pattern ||
                    match_pattern_is_structurally_covering(tc, head_pattern, head_type, true);
                if (!use_wildcards) {
                    const char* pattern_member =
                        match_expr_enum_member_name(tc, head_pattern, head_enum);
                    if (!pattern_member || strcmp(pattern_member, member_name) != 0) {
                        continue;
                    }
                    if (payload_count > 0 &&
                        (head_pattern->kind != EXPR_CALL ||
                         head_pattern->call.arg_count != payload_count)) {
                        continue;
                    }
                }

                MatchPatternRow* next_row = &next_rows[next_row_count++];
                next_row->column_count = next_column_count;
                if (next_column_count > 0) {
                    next_row->columns =
                        (Expr**)safe_calloc((size_t)next_column_count, sizeof(Expr*));
                    if (!use_wildcards) {
                        for (int k = 0; k < payload_count; k++) {
                            next_row->columns[k] =
                                head_pattern->call.args ? head_pattern->call.args[k] : NULL;
                        }
                    }
                    for (int k = 1; k < column_count; k++) {
                        next_row->columns[payload_count + k - 1] =
                            rows[j].columns ? rows[j].columns[k] : NULL;
                    }
                }
            }

            bool exhaustive =
                next_row_count > 0 &&
                match_pattern_matrix_exhaustive(tc,
                                                next_rows,
                                                next_row_count,
                                                next_column_types,
                                                next_column_count);
            match_pattern_rows_free(next_rows, next_row_count);
            if (next_column_types) {
                free(next_column_types);
            }
            match_pattern_free_payload_bindings(NULL, payload_types, payload_count);
            if (!exhaustive) {
                return false;
            }
        }

        return saw_member;
    }

    if (head_type->kind == TYPE_BOOL) {
        for (int branch = 0; branch < 2; branch++) {
            bool branch_value = branch != 0;
            MatchPatternRow* branch_rows =
                (MatchPatternRow*)safe_calloc((size_t)row_count, sizeof(MatchPatternRow));
            int branch_row_count = 0;

            for (int i = 0; i < row_count; i++) {
                Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
                bool pattern_matches = false;
                bool literal_value = false;
                if (!head_pattern ||
                    match_pattern_is_structurally_covering(tc, head_pattern, head_type, true)) {
                    pattern_matches = true;
                } else if (match_expr_is_bool_literal(head_pattern, &literal_value) &&
                           literal_value == branch_value) {
                    pattern_matches = true;
                }

                if (!pattern_matches) {
                    continue;
                }

                MatchPatternRow* next_row = &branch_rows[branch_row_count++];
                next_row->column_count = column_count - 1;
                if (next_row->column_count > 0) {
                    next_row->columns =
                        (Expr**)safe_calloc((size_t)next_row->column_count, sizeof(Expr*));
                    for (int j = 1; j < column_count; j++) {
                        next_row->columns[j - 1] = rows[i].columns ? rows[i].columns[j] : NULL;
                    }
                }
            }

            bool exhaustive =
                branch_row_count > 0 &&
                match_pattern_matrix_exhaustive(tc,
                                                branch_rows,
                                                branch_row_count,
                                                column_types + 1,
                                                column_count - 1);
            match_pattern_rows_free(branch_rows, branch_row_count);
            if (!exhaustive) {
                return false;
            }
        }
        return true;
    }

    if (head_type->kind == TYPE_NIL) {
        MatchPatternRow* nil_rows =
            (MatchPatternRow*)safe_calloc((size_t)row_count, sizeof(MatchPatternRow));
        int nil_row_count = 0;

        for (int i = 0; i < row_count; i++) {
            Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
            if (head_pattern &&
                !match_pattern_is_structurally_covering(tc, head_pattern, head_type, true) &&
                head_pattern->kind != EXPR_NIL) {
                continue;
            }

            MatchPatternRow* next_row = &nil_rows[nil_row_count++];
            next_row->column_count = column_count - 1;
            if (next_row->column_count > 0) {
                next_row->columns =
                    (Expr**)safe_calloc((size_t)next_row->column_count, sizeof(Expr*));
                for (int j = 1; j < column_count; j++) {
                    next_row->columns[j - 1] = rows[i].columns ? rows[i].columns[j] : NULL;
                }
            }
        }

        bool exhaustive =
            nil_row_count > 0 &&
            match_pattern_matrix_exhaustive(tc,
                                            nil_rows,
                                            nil_row_count,
                                            column_types + 1,
                                            column_count - 1);
        match_pattern_rows_free(nil_rows, nil_row_count);
        return exhaustive;
    }

    if (head_type->kind == TYPE_TUPLE) {
        int arity = tuple_type_get_arity(head_type);
        int next_column_count = arity + column_count - 1;
        Type** next_column_types =
            (Type**)safe_calloc((size_t)next_column_count, sizeof(Type*));
        for (int i = 0; i < arity; i++) {
            next_column_types[i] = tuple_type_get_element(head_type, i);
        }
        for (int i = 1; i < column_count; i++) {
            next_column_types[arity + i - 1] = column_types[i];
        }

        MatchPatternRow* next_rows =
            (MatchPatternRow*)safe_calloc((size_t)row_count, sizeof(MatchPatternRow));
        int next_row_count = 0;
        for (int i = 0; i < row_count; i++) {
            Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
            bool use_wildcards =
                !head_pattern ||
                match_pattern_is_structurally_covering(tc, head_pattern, head_type, true);
            if (!use_wildcards &&
                (head_pattern->kind != EXPR_TUPLE_LITERAL ||
                 head_pattern->tuple_literal.element_count != arity)) {
                continue;
            }

            MatchPatternRow* next_row = &next_rows[next_row_count++];
            next_row->column_count = next_column_count;
            next_row->columns =
                (Expr**)safe_calloc((size_t)next_column_count, sizeof(Expr*));
            for (int j = 0; j < arity; j++) {
                next_row->columns[j] = use_wildcards
                    ? NULL
                    : head_pattern->tuple_literal.elements[j];
            }
            for (int j = 1; j < column_count; j++) {
                next_row->columns[arity + j - 1] = rows[i].columns ? rows[i].columns[j] : NULL;
            }
        }

        bool exhaustive =
            next_row_count > 0 &&
            match_pattern_matrix_exhaustive(tc,
                                            next_rows,
                                            next_row_count,
                                            next_column_types,
                                            next_column_count);
        match_pattern_rows_free(next_rows, next_row_count);
        free(next_column_types);
        return exhaustive;
    }

    if (head_type->kind == TYPE_RECORD && head_type->record_def) {
        RecordDef* record_def = head_type->record_def;
        int field_count = record_def->field_count;
        int next_column_count = field_count + column_count - 1;
        Type** next_column_types =
            (Type**)safe_calloc((size_t)next_column_count, sizeof(Type*));
        for (int i = 0; i < field_count; i++) {
            next_column_types[i] = record_def->fields[i].type;
        }
        for (int i = 1; i < column_count; i++) {
            next_column_types[field_count + i - 1] = column_types[i];
        }

        MatchPatternRow* next_rows =
            (MatchPatternRow*)safe_calloc((size_t)row_count, sizeof(MatchPatternRow));
        int next_row_count = 0;
        for (int i = 0; i < row_count; i++) {
            Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
            bool use_wildcards =
                !head_pattern ||
                match_pattern_is_structurally_covering(tc, head_pattern, head_type, true);
            if (!use_wildcards && head_pattern->kind != EXPR_RECORD_LITERAL) {
                continue;
            }

            if (!use_wildcards) {
                Type* pattern_record_type =
                    match_pattern_resolve_record_type(tc, head_pattern, head_type);
                if (!pattern_record_type ||
                    pattern_record_type->kind != TYPE_RECORD ||
                    pattern_record_type->record_def != record_def) {
                    continue;
                }
                match_pattern_store_record_type(head_pattern, pattern_record_type);
            }

            MatchPatternRow* next_row = &next_rows[next_row_count++];
            next_row->column_count = next_column_count;
            next_row->columns =
                (Expr**)safe_calloc((size_t)next_column_count, sizeof(Expr*));

            if (!use_wildcards) {
                bool invalid_pattern = false;
                for (int j = 0; j < head_pattern->record_literal.field_count; j++) {
                    const char* field_name = head_pattern->record_literal.field_names
                        ? head_pattern->record_literal.field_names[j]
                        : NULL;
                    Expr* field_value = head_pattern->record_literal.field_values
                        ? head_pattern->record_literal.field_values[j]
                        : NULL;
                    int field_index =
                        field_name ? record_def_get_field_index(record_def, field_name) : -1;
                    if (field_index < 0) {
                        invalid_pattern = true;
                        break;
                    }
                    next_row->columns[field_index] = field_value;
                }
                if (invalid_pattern) {
                    free(next_row->columns);
                    next_row->columns = NULL;
                    next_row->column_count = 0;
                    next_row_count--;
                    continue;
                }
            }

            for (int j = 1; j < column_count; j++) {
                next_row->columns[field_count + j - 1] = rows[i].columns ? rows[i].columns[j] : NULL;
            }
        }

        bool exhaustive =
            next_row_count > 0 &&
            match_pattern_matrix_exhaustive(tc,
                                            next_rows,
                                            next_row_count,
                                            next_column_types,
                                            next_column_count);
        match_pattern_rows_free(next_rows, next_row_count);
        free(next_column_types);
        return exhaustive;
    }

    MatchPatternRow* wildcard_rows =
        (MatchPatternRow*)safe_calloc((size_t)row_count, sizeof(MatchPatternRow));
    int wildcard_row_count = 0;
    for (int i = 0; i < row_count; i++) {
        Expr* head_pattern = rows[i].columns ? rows[i].columns[0] : NULL;
        if (head_pattern &&
            !match_pattern_is_structurally_covering(tc, head_pattern, head_type, true)) {
            continue;
        }

        MatchPatternRow* next_row = &wildcard_rows[wildcard_row_count++];
        next_row->column_count = column_count - 1;
        if (next_row->column_count > 0) {
            next_row->columns =
                (Expr**)safe_calloc((size_t)next_row->column_count, sizeof(Expr*));
            for (int j = 1; j < column_count; j++) {
                next_row->columns[j - 1] = rows[i].columns ? rows[i].columns[j] : NULL;
            }
        }
    }

    bool exhaustive =
        wildcard_row_count > 0 &&
        match_pattern_matrix_exhaustive(tc,
                                        wildcard_rows,
                                        wildcard_row_count,
                                        column_types + 1,
                                        column_count - 1);
    match_pattern_rows_free(wildcard_rows, wildcard_row_count);
    return exhaustive;
}

static bool match_tuple_patterns_exhaustive(TypeChecker* tc,
                                            Expr** patterns,
                                            int pattern_count,
                                            Type* subject_type) {
    if (!tc || !patterns || pattern_count <= 0 || !subject_type || subject_type->kind != TYPE_TUPLE) {
        return false;
    }

    MatchPatternRow* rows =
        (MatchPatternRow*)safe_calloc((size_t)pattern_count, sizeof(MatchPatternRow));
    for (int i = 0; i < pattern_count; i++) {
        rows[i].column_count = 1;
        rows[i].columns = (Expr**)safe_calloc(1, sizeof(Expr*));
        rows[i].columns[0] = patterns[i];
    }

    Type* column_types[1];
    column_types[0] = subject_type;
    bool exhaustive = match_pattern_matrix_exhaustive(tc, rows, pattern_count, column_types, 1);
    match_pattern_rows_free(rows, pattern_count);
    return exhaustive;
}

static bool match_record_patterns_exhaustive(TypeChecker* tc,
                                             Expr** patterns,
                                             int pattern_count,
                                             Type* subject_type) {
    if (!tc || !patterns || pattern_count <= 0 ||
        !subject_type || subject_type->kind != TYPE_RECORD || !subject_type->record_def) {
        return false;
    }

    MatchPatternRow* rows =
        (MatchPatternRow*)safe_calloc((size_t)pattern_count, sizeof(MatchPatternRow));
    for (int i = 0; i < pattern_count; i++) {
        rows[i].column_count = 1;
        rows[i].columns = (Expr**)safe_calloc(1, sizeof(Expr*));
        rows[i].columns[0] = patterns[i];
    }

    Type* column_types[1];
    column_types[0] = subject_type;
    bool exhaustive = match_pattern_matrix_exhaustive(tc, rows, pattern_count, column_types, 1);
    match_pattern_rows_free(rows, pattern_count);
    return exhaustive;
}

static bool match_patterns_exhaustive(TypeChecker* tc,
                                      Expr** patterns,
                                      int pattern_count,
                                      Type* subject_type) {
    if (!tc || !patterns || pattern_count <= 0 || !subject_type) return false;

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    if (!resolved_subject_type) return false;

    for (int i = 0; i < pattern_count; i++) {
        Expr* pattern_expr = patterns[i];
        if (!pattern_expr ||
            match_pattern_is_structurally_covering(tc,
                                                   pattern_expr,
                                                   resolved_subject_type,
                                                   true)) {
            return true;
        }
    }

    const char* subject_enum = type_enum_name(resolved_subject_type);
    if (subject_enum) {
        return match_enum_patterns_exhaustive(tc,
                                              patterns,
                                              pattern_count,
                                              resolved_subject_type);
    }

    if (resolved_subject_type->kind == TYPE_BOOL) {
        bool has_true_pattern = false;
        bool has_false_pattern = false;
        for (int i = 0; i < pattern_count; i++) {
            bool pattern_value = false;
            if (!patterns[i] || !match_expr_is_bool_literal(patterns[i], &pattern_value)) {
                continue;
            }
            if (pattern_value) {
                has_true_pattern = true;
            } else {
                has_false_pattern = true;
            }
        }
        return has_true_pattern && has_false_pattern;
    }

    if (resolved_subject_type->kind == TYPE_TUPLE) {
        return match_tuple_patterns_exhaustive(tc,
                                               patterns,
                                               pattern_count,
                                               resolved_subject_type);
    }

    if (resolved_subject_type->kind == TYPE_RECORD) {
        return match_record_patterns_exhaustive(tc,
                                                patterns,
                                                pattern_count,
                                                resolved_subject_type);
    }

    return false;
}

static bool match_pattern_covers_enum_member(TypeChecker* tc,
                                             Expr* pattern_expr,
                                             Type* subject_type) {
    if (!tc || !pattern_expr || !subject_type) return false;

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    if (!resolved_subject_type) return false;

    if (pattern_expr->kind != EXPR_CALL) {
        return true;
    }

    Type** payload_types = NULL;
    int payload_count = 0;
    if (!match_pattern_resolve_enum_payload_types(tc,
                                                  pattern_expr,
                                                  resolved_subject_type,
                                                  &payload_types,
                                                  &payload_count)) {
        return false;
    }

    bool covers_payload = true;
    for (int i = 0; i < payload_count; i++) {
        Expr* arg = pattern_expr->call.args ? pattern_expr->call.args[i] : NULL;
        if (!match_pattern_is_structurally_covering(tc, arg, payload_types[i], true)) {
            covers_payload = false;
            break;
        }
    }

    match_pattern_free_payload_bindings(NULL, payload_types, payload_count);
    return covers_payload;
}

static void match_pattern_apply_context_types(TypeChecker* tc,
                                              Expr* pattern_expr,
                                              Type* subject_type,
                                              bool allow_identifier_binding) {
    if (!tc || !pattern_expr || !subject_type) return;

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    if (!resolved_subject_type) return;

    if (allow_identifier_binding &&
        pattern_expr->kind == EXPR_IDENTIFIER &&
        pattern_expr->identifier &&
        match_binding_name_looks_destructure(pattern_expr->identifier)) {
        return;
    }

    if (pattern_expr->kind == EXPR_TUPLE_LITERAL &&
        resolved_subject_type->kind == TYPE_TUPLE &&
        tuple_type_get_arity(resolved_subject_type) == pattern_expr->tuple_literal.element_count) {
        for (int i = 0; i < pattern_expr->tuple_literal.element_count; i++) {
            match_pattern_apply_context_types(tc,
                                              pattern_expr->tuple_literal.elements[i],
                                              tuple_type_get_element(resolved_subject_type, i),
                                              true);
        }
        return;
    }

    if (pattern_expr->kind == EXPR_RECORD_LITERAL) {
        Type* record_type = match_pattern_resolve_record_type(tc, pattern_expr, resolved_subject_type);
        if (record_type && record_type->kind == TYPE_RECORD && record_type->record_def) {
            match_pattern_store_record_type(pattern_expr, record_type);
            for (int i = 0; i < pattern_expr->record_literal.field_count; i++) {
                const char* field_name = pattern_expr->record_literal.field_names
                    ? pattern_expr->record_literal.field_names[i]
                    : NULL;
                Type* field_type = field_name
                    ? record_def_get_field_type(record_type->record_def, field_name)
                    : NULL;
                if (!field_type) continue;
                match_pattern_apply_context_types(tc,
                                                  pattern_expr->record_literal.field_values[i],
                                                  field_type,
                                                  true);
            }
            return;
        }
    }

    Type** payload_types = NULL;
    int payload_count = 0;
    if (match_pattern_resolve_enum_payload_types(tc,
                                                 pattern_expr,
                                                 resolved_subject_type,
                                                 &payload_types,
                                                 &payload_count)) {
        for (int i = 0; i < payload_count; i++) {
            Expr* arg = pattern_expr->call.args ? pattern_expr->call.args[i] : NULL;
            if (!arg) continue;
            match_pattern_apply_context_types(tc, arg, payload_types[i], true);
        }
        match_pattern_free_payload_bindings(NULL, payload_types, payload_count);
    }
}

static Type* match_merge_result_type(TypeChecker* tc,
                                     Type* current,
                                     Type* candidate,
                                     const char* file,
                                     int line,
                                     int column) {
    Type* normalized_candidate = candidate ? candidate : type_any();
    if (!current) {
        return type_clone(normalized_candidate);
    }
    if (typechecker_types_assignable(tc, current, normalized_candidate, file, line, column)) {
        return current;
    }
    if (typechecker_types_assignable(tc, normalized_candidate, current, file, line, column)) {
        Type* widened = type_clone(normalized_candidate);
        type_free(current);
        return widened;
    }

    char current_buf[256];
    char candidate_buf[256];
    char message[512];
    type_to_string(current, current_buf, sizeof(current_buf));
    type_to_string(normalized_candidate, candidate_buf, sizeof(candidate_buf));
    snprintf(message,
             sizeof(message),
             "match expression arm type mismatch: expected %s, got %s",
             current_buf,
             candidate_buf);
    typechecker_error(tc, message, file, line, column);
    return current;
}

static Type* if_merge_result_type(TypeChecker* tc,
                                  Type* current,
                                  Type* candidate,
                                  const char* file,
                                  int line,
                                  int column) {
    Type* normalized_candidate = candidate ? candidate : type_any();
    if (!current) {
        return type_clone(normalized_candidate);
    }
    if (typechecker_types_assignable(tc, current, normalized_candidate, file, line, column)) {
        return current;
    }
    if (typechecker_types_assignable(tc, normalized_candidate, current, file, line, column)) {
        Type* widened = type_clone(normalized_candidate);
        type_free(current);
        return widened;
    }

    char current_buf[256];
    char candidate_buf[256];
    char message[512];
    type_to_string(current, current_buf, sizeof(current_buf));
    type_to_string(normalized_candidate, candidate_buf, sizeof(candidate_buf));
    snprintf(message,
             sizeof(message),
             "if expression branch type mismatch: expected %s, got %s",
             current_buf,
             candidate_buf);
    typechecker_error(tc, message, file, line, column);
    return current;
}

static bool match_pattern_collect_payload_bindings_recursive(TypeChecker* tc,
                                                             Expr* pattern_expr,
                                                             Type* subject_type,
                                                             bool allow_identifier_binding,
                                                             char*** out_names,
                                                             Type*** out_types,
                                                             int* out_count) {
    if (!tc || !pattern_expr || !subject_type) return false;

    Type* resolved_subject_type = typechecker_resolve_type(tc, subject_type);
    if (!resolved_subject_type) return false;

    if (allow_identifier_binding &&
        pattern_expr->kind == EXPR_IDENTIFIER &&
        pattern_expr->identifier &&
        match_binding_name_looks_destructure(pattern_expr->identifier)) {
        match_pattern_append_binding(out_names,
                                     out_types,
                                     out_count,
                                     strcmp(pattern_expr->identifier, "_") == 0
                                         ? NULL
                                         : pattern_expr->identifier,
                                     resolved_subject_type);
        return true;
    }

    if (pattern_expr->kind == EXPR_TUPLE_LITERAL &&
        resolved_subject_type->kind == TYPE_TUPLE &&
        tuple_type_get_arity(resolved_subject_type) == pattern_expr->tuple_literal.element_count) {
        bool found = false;
        for (int i = 0; i < pattern_expr->tuple_literal.element_count; i++) {
            Expr* element = pattern_expr->tuple_literal.elements
                ? pattern_expr->tuple_literal.elements[i]
                : NULL;
            Type* element_type = tuple_type_get_element(resolved_subject_type, i);
            found |= match_pattern_collect_payload_bindings_recursive(tc,
                                                                      element,
                                                                      element_type,
                                                                      true,
                                                                      out_names,
                                                                      out_types,
                                                                      out_count);
        }
        return found;
    }

    if (pattern_expr->kind == EXPR_RECORD_LITERAL) {
        Type* record_type = match_pattern_resolve_record_type(tc, pattern_expr, resolved_subject_type);
        if (record_type && record_type->kind == TYPE_RECORD && record_type->record_def) {
            bool found = false;
            match_pattern_store_record_type(pattern_expr, record_type);
            for (int i = 0; i < pattern_expr->record_literal.field_count; i++) {
                const char* field_name = pattern_expr->record_literal.field_names
                    ? pattern_expr->record_literal.field_names[i]
                    : NULL;
                Type* field_type = field_name
                    ? record_def_get_field_type(record_type->record_def, field_name)
                    : NULL;
                if (!field_type) return false;
                found |= match_pattern_collect_payload_bindings_recursive(tc,
                                                                          pattern_expr->record_literal.field_values[i],
                                                                          field_type,
                                                                          true,
                                                                          out_names,
                                                                          out_types,
                                                                          out_count);
            }
            return found;
        }
    }

    Type** payload_types = NULL;
    int payload_count = 0;
    if (match_pattern_resolve_enum_payload_types(tc,
                                                 pattern_expr,
                                                 resolved_subject_type,
                                                 &payload_types,
                                                 &payload_count)) {
        bool found = false;
        for (int i = 0; i < payload_count; i++) {
            Expr* arg = pattern_expr->call.args ? pattern_expr->call.args[i] : NULL;
            found |= match_pattern_collect_payload_bindings_recursive(tc,
                                                                      arg,
                                                                      payload_types[i],
                                                                      true,
                                                                      out_names,
                                                                      out_types,
                                                                      out_count);
        }
        match_pattern_free_payload_bindings(NULL, payload_types, payload_count);
        return found;
    }

    return false;
}

static bool match_pattern_collect_payload_bindings(TypeChecker* tc,
                                                   Expr* pattern_expr,
                                                   Type* subject_type,
                                                   char*** out_names,
                                                   Type*** out_types,
                                                   int* out_count) {
    if (out_names) *out_names = NULL;
    if (out_types) *out_types = NULL;
    if (out_count) *out_count = 0;
    return match_pattern_collect_payload_bindings_recursive(tc,
                                                            pattern_expr,
                                                            subject_type,
                                                            false,
                                                            out_names,
                                                            out_types,
                                                            out_count);
}

static bool match_enum_member_is_covered(const char** covered_members, int covered_count, const char* member_name) {
    if (!covered_members || covered_count <= 0 || !member_name) return false;
    for (int i = 0; i < covered_count; i++) {
        if (covered_members[i] && strcmp(covered_members[i], member_name) == 0) {
            return true;
        }
    }
    return false;
}

static int match_enum_member_count(TypeChecker* tc, const char* enum_name) {
    if (!tc || !tc->globals || !enum_name) return 0;
    int count = 0;
    for (int i = 0; i < tc->globals->symbol_count; i++) {
        Symbol* sym = tc->globals->symbols ? tc->globals->symbols[i] : NULL;
        const char* member_name = NULL;
        if (symbol_is_enum_member_for(sym, enum_name, &member_name)) {
            count++;
        }
    }
    return count;
}

static bool typechecker_enum_member_value(TypeChecker* tc,
                                          const char* enum_name,
                                          const char* member_name,
                                          int64_t* out_value) {
    if (!tc || !tc->program || !enum_name || !member_name || !out_value) {
        return false;
    }

    for (int i = 0; i < tc->program->stmt_count; i++) {
        Stmt* stmt = tc->program->statements ? tc->program->statements[i] : NULL;
        if (!stmt || stmt->kind != STMT_ENUM_DECL || !stmt->enum_decl.name) continue;
        if (!enum_name_base_equals(stmt->enum_decl.name, enum_name)) continue;

        for (int j = 0; j < stmt->enum_decl.member_count; j++) {
            const char* name = stmt->enum_decl.member_names ? stmt->enum_decl.member_names[j] : NULL;
            if (name && strcmp(name, member_name) == 0) {
                *out_value = stmt->enum_decl.member_values ? stmt->enum_decl.member_values[j] : 0;
                return true;
            }
        }
        return false;
    }

    return false;
}

static bool type_is_result_tuple(TypeChecker* tc, Type* type) {
    if (!tc || !type || type->kind != TYPE_TUPLE || tuple_type_get_arity(type) != 2) return false;
    Type* err_elem = tuple_type_get_element(type, 1);
    if (!err_elem || err_elem->kind == TYPE_ANY) return false;

    Type* expected_err = type_error_nullable(tc);
    bool ok = expected_err && type_assignable(expected_err, err_elem);
    if (expected_err) type_free(expected_err);
    return ok;
}

static void typechecker_push_scope(TypeChecker* tc) {
    tc->local_count++;
    if (tc->local_count > tc->local_capacity) {
        tc->local_capacity = tc->local_count * 2;
        tc->locals = (SymbolTable**)safe_realloc(tc->locals, tc->local_capacity * sizeof(SymbolTable*));
    }
    tc->locals[tc->local_count - 1] = symbol_table_create();
}

static void typechecker_pop_scope(TypeChecker* tc) {
    if (tc->local_count > 0) {
        symbol_table_free(tc->locals[tc->local_count - 1]);
        tc->local_count--;
    }
}

static Symbol* typechecker_lookup(TypeChecker* tc, const char* name) {
    return typechecker_lookup_with_scope(tc, name, NULL);
}

static Symbol* typechecker_lookup_with_scope(TypeChecker* tc, const char* name, int* out_scope_index) {
    if (out_scope_index) *out_scope_index = -1;

    int floor = tc->local_lookup_floor;
    if (floor < 0) floor = 0;
    if (floor > tc->local_count) floor = tc->local_count;

    for (int i = tc->local_count - 1; i >= floor; i--) {
        Symbol* sym = symbol_table_get(tc->locals[i], name);
        if (sym) {
            if (out_scope_index) *out_scope_index = i;
            return sym;
        }
    }
    return symbol_table_get(tc->globals, name);
}

static FuncLiteralCaptureContext* typechecker_current_capture_context(TypeChecker* tc) {
    if (!tc || tc->capture_context_count <= 0) return NULL;
    return &tc->capture_contexts[tc->capture_context_count - 1];
}

static void typechecker_capture_context_push(TypeChecker* tc, Expr* expr, int outer_local_count) {
    if (!tc) return;

    tc->capture_context_count++;
    if (tc->capture_context_count > tc->capture_context_capacity) {
        tc->capture_context_capacity = tc->capture_context_count * 2;
        tc->capture_contexts = (FuncLiteralCaptureContext*)safe_realloc(
            tc->capture_contexts,
            (size_t)tc->capture_context_capacity * sizeof(FuncLiteralCaptureContext));
    }

    FuncLiteralCaptureContext* ctx = &tc->capture_contexts[tc->capture_context_count - 1];
    ctx->expr = expr;
    ctx->outer_local_count = outer_local_count;
    ctx->names = NULL;
    ctx->name_count = 0;
    ctx->name_capacity = 0;
}

static void typechecker_capture_context_add(TypeChecker* tc, const char* name) {
    FuncLiteralCaptureContext* ctx = typechecker_current_capture_context(tc);
    if (!ctx || !name) return;

    for (int i = 0; i < ctx->name_count; i++) {
        if (ctx->names[i] && strcmp(ctx->names[i], name) == 0) {
            return;
        }
    }

    ctx->name_count++;
    if (ctx->name_count > ctx->name_capacity) {
        ctx->name_capacity = ctx->name_count * 2;
        ctx->names = (char**)safe_realloc(ctx->names, (size_t)ctx->name_capacity * sizeof(char*));
    }
    ctx->names[ctx->name_count - 1] = safe_strdup(name);
}

static FuncLiteralCaptureContext typechecker_capture_context_pop(TypeChecker* tc) {
    FuncLiteralCaptureContext empty_ctx;
    empty_ctx.expr = NULL;
    empty_ctx.outer_local_count = 0;
    empty_ctx.names = NULL;
    empty_ctx.name_count = 0;
    empty_ctx.name_capacity = 0;

    if (!tc || tc->capture_context_count <= 0) return empty_ctx;

    tc->capture_context_count--;
    return tc->capture_contexts[tc->capture_context_count];
}

static void typechecker_capture_context_free(FuncLiteralCaptureContext* ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->name_count; i++) {
        if (ctx->names && ctx->names[i]) free(ctx->names[i]);
    }
    if (ctx->names) free(ctx->names);
    ctx->names = NULL;
    ctx->name_count = 0;
    ctx->name_capacity = 0;
}

static bool typechecker_declare(TypeChecker* tc, Symbol* sym) {
    if (tc->local_count > 0) {
        SymbolTable* top = tc->locals[tc->local_count - 1];
        if (symbol_table_has(top, sym->name)) {
            return false;
        }
        symbol_table_add(top, sym);
    } else {
        if (symbol_table_has(tc->globals, sym->name)) {
            return false;
        }
        symbol_table_add(tc->globals, sym);
    }
    return true;
}

static Type* typecheck_expression(TypeChecker* tc, Expr* expr);
static Type* typecheck_if_expression(TypeChecker* tc, Expr* expr);
static Type* typecheck_match_expression(TypeChecker* tc, Expr* expr);

static Type* typecheck_expression_with_expected(TypeChecker* tc, Expr* expr, Type* expected_type) {
    if (!expr) return type_any();
    Type* previous_expected = tc ? tc->expected_expr_type : NULL;
    if (tc) tc->expected_expr_type = expected_type;
    Type* result = typecheck_expression(tc, expr);
    if (tc) tc->expected_expr_type = previous_expected;
    return result;
}

static Type* typecheck_literal(TypeChecker* tc, Expr* expr) {
    (void)tc;
    // If the type is already set (e.g., from literal creation), keep it
    if (expr->type) return expr->type;

    switch (expr->kind) {
        case EXPR_LITERAL:
            expr->type = type_int();
            break;
        case EXPR_NIL:
            expr->type = type_nil();
            break;
        default:
            expr->type = type_any();
            break;
    }

    return expr->type;
}

static Type* typecheck_identifier(TypeChecker* tc, Expr* expr) {
    if (!expr->identifier) {
        expr->type = type_any();
        return expr->type;
    }

    // Built-in functions
    if (strcmp(expr->identifier, "print") == 0 || strcmp(expr->identifier, "println") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_void(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "panic") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        expr->type = type_function(type_void(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "must") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_any(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "wrapError") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_error_nullable(tc);
        param_types[1] = type_string();
        expr->type = type_function(type_error_nullable(tc), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "len") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_int(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "gcCollect") == 0) {
        expr->type = type_function(type_int(), NULL, 0);
        return expr->type;
    }

    if (strcmp(expr->identifier, "str") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_string(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "formatDouble") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_int();
        expr->type = type_function(type_string(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "toInt") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_int(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "toDouble") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_double(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "toBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_bigint()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "toHexBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_string(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "bytesToHex") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_bytes();
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "hexToBytes") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_bytes()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "stringToBytes") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_bytes()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "bytesToString") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_bytes();
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sha256Bytes") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_bytes();
        expr->type = type_function(type_result_tuple(tc, type_bytes()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "hmacSha256Bytes") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_bytes();
        param_types[1] = type_bytes();
        expr->type = type_function(type_result_tuple(tc, type_bytes()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "pbkdf2HmacSha256Bytes") == 0) {
        Type** param_types = (Type**)safe_malloc(4 * sizeof(Type*));
        param_types[0] = type_bytes();
        param_types[1] = type_bytes();
        param_types[2] = type_int();
        param_types[3] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bytes()), param_types, 4);
        return expr->type;
    }

    if (strcmp(expr->identifier, "hkdfHmacSha256Bytes") == 0) {
        Type** param_types = (Type**)safe_malloc(4 * sizeof(Type*));
        param_types[0] = type_bytes();
        param_types[1] = type_bytes();
        param_types[2] = type_bytes();
        param_types[3] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bytes()), param_types, 4);
        return expr->type;
    }

    if (strcmp(expr->identifier, "constantTimeBytesEqual") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_bytes();
        param_types[1] = type_bytes();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "aesCtrBytes") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_bytes();
        param_types[1] = type_bytes();
        param_types[2] = type_bytes();
        expr->type = type_function(type_result_tuple(tc, type_bytes()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "aesGcmSealBytes") == 0 ||
        strcmp(expr->identifier, "aesGcmOpenBytes") == 0) {
        Type** param_types = (Type**)safe_malloc(4 * sizeof(Type*));
        param_types[0] = type_bytes();
        param_types[1] = type_bytes();
        param_types[2] = type_bytes();
        param_types[3] = type_bytes();
        expr->type = type_function(type_result_tuple(tc, type_bytes()), param_types, 4);
        return expr->type;
    }

    if (strcmp(expr->identifier, "bytesWithSize") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bytes()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "bytesJoin") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_array(type_bytes());
        expr->type = type_function(type_result_tuple(tc, type_bytes()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "urlEncode") == 0 || strcmp(expr->identifier, "urlDecode") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 1);
        return expr->type;
    }

    // String builtins
    if (strcmp(expr->identifier, "substring") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_int();
        param_types[2] = type_int();
        expr->type = type_function(type_string(), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "find") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        expr->type = type_function(type_int(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "split") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        expr->type = type_function(type_array(type_string()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "trim") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        expr->type = type_function(type_string(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "startsWith") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        expr->type = type_function(type_bool(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "endsWith") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        expr->type = type_function(type_bool(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "replace") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        param_types[2] = type_string();
        expr->type = type_function(type_string(), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "absBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_bigint();
        expr->type = type_function(type_bigint(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "signBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_bigint();
        expr->type = type_function(type_int(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "digitsBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_bigint();
        expr->type = type_function(type_int(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "isEvenBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_bigint();
        expr->type = type_function(type_bool(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "isOddBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_bigint();
        expr->type = type_function(type_bool(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "powBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_bigint();
        param_types[1] = type_int();
        expr->type = type_function(type_bigint(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "gcdBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_bigint();
        param_types[1] = type_bigint();
        expr->type = type_function(type_bigint(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "lcmBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_bigint();
        param_types[1] = type_bigint();
        expr->type = type_function(type_bigint(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "modPowBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_bigint();
        param_types[1] = type_any();
        param_types[2] = type_bigint();
        expr->type = type_function(type_bigint(), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "modInverseBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_bigint();
        param_types[1] = type_bigint();
        expr->type = type_function(type_bigint(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "isProbablePrimeBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_bigint();
        param_types[1] = type_int();
        expr->type = type_function(type_bool(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "compareBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_bigint();
        param_types[1] = type_bigint();
        expr->type = type_function(type_int(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "absCmpBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_bigint();
        param_types[1] = type_bigint();
        expr->type = type_function(type_int(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "clampBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_bigint();
        param_types[1] = type_bigint();
        param_types[2] = type_bigint();
        expr->type = type_function(type_bigint(), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "isZeroBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_bigint();
        expr->type = type_function(type_bool(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "isNegativeBigInt") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_bigint();
        expr->type = type_function(type_bool(), param_types, 1);
        return expr->type;
    }

    // Math builtins
    if (strcmp(expr->identifier, "absInt") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_int(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "absDouble") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_double(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "min") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_any(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "max") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_any(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "floor") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_double(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "ceil") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_double(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "round") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_double(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqrt") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_double(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "pow") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_double(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "typeOf") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_string(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "futurePending") == 0) {
        Type* return_type = type_future(type_type_param("T"));
        expr->type = type_function(return_type, NULL, 0);
        char* type_params[] = { "T" };
        type_function_set_type_params(expr->type, type_params, NULL, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "futureResolved") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_type_param("T");
        expr->type = type_function(type_future(type_type_param("T")), param_types, 1);
        char* type_params[] = { "T" };
        type_function_set_type_params(expr->type, type_params, NULL, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "futureIsReady") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_future(type_type_param("T"));
        expr->type = type_function(type_bool(), param_types, 1);
        char* type_params[] = { "T" };
        type_function_set_type_params(expr->type, type_params, NULL, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "futureComplete") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_future(type_type_param("T"));
        param_types[1] = type_type_param("T");
        expr->type = type_function(type_bool(), param_types, 2);
        char* type_params[] = { "T" };
        type_function_set_type_params(expr->type, type_params, NULL, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "futureGet") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_future(type_type_param("T"));
        expr->type = type_function(type_type_param("T"), param_types, 1);
        char* type_params[] = { "T" };
        type_function_set_type_params(expr->type, type_params, NULL, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "extPostedCallbackPendingCount") == 0) {
        expr->type = type_function(type_int(), NULL, 0);
        return expr->type;
    }

    if (strcmp(expr->identifier, "extDrainPostedCallbacks") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_int(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "extSetPostedCallbackAutoDrain") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_bool();
        expr->type = type_function(type_bool(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "asyncSleep") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_future(type_void()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "asyncChannelSend") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_any();
        expr->type = type_function(type_future(type_result_tuple(tc, type_bool())), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "asyncChannelSendTyped") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_any();
        param_types[2] = type_any();
        expr->type = type_function(type_future(type_result_tuple(tc, type_bool())), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "asyncChannelRecv") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_future(type_result_tuple(tc, type_any())), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "asyncChannelRecvTyped") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_any();
        expr->type = type_function(type_future(type_result_tuple(tc, type_any())), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "jsonParse") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "jsonStringify") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "jsonStringifyPretty") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "jsonDecode") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "timeNowMillis") == 0) {
        expr->type = type_function(type_int(), NULL, 0);
        return expr->type;
    }

    if (strcmp(expr->identifier, "timeNowNanos") == 0) {
        expr->type = type_function(type_int(), NULL, 0);
        return expr->type;
    }

    if (strcmp(expr->identifier, "timeMonotonicMillis") == 0) {
        expr->type = type_function(type_int(), NULL, 0);
        return expr->type;
    }

    if (strcmp(expr->identifier, "timeSinceMillis") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_int(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "utcDateTime") == 0) {
        expr->type = type_function(type_array(type_int()), NULL, 0);
        return expr->type;
    }

    if (strcmp(expr->identifier, "localDateTime") == 0) {
        expr->type = type_function(type_array(type_int()), NULL, 0);
        return expr->type;
    }

    if (strcmp(expr->identifier, "logJson") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        param_types[2] = type_map(type_string(), type_any());
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "random") == 0) {
        expr->type = type_function(type_double(), NULL, 0);
        return expr->type;
    }

    if (strcmp(expr->identifier, "randomSeed") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_void(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "randomInt") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_int();
        expr->type = type_function(type_int(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "randomDouble") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_double(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "randomBigIntBits") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_bigint(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "randomBigIntRange") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_bigint();
        param_types[1] = type_bigint();
        expr->type = type_function(type_bigint(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "randomFillInt") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_array(type_int());
        param_types[1] = type_int();
        param_types[2] = type_int();
        expr->type = type_function(type_array(type_int()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "randomFillDouble") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_array(type_double());
        param_types[1] = type_any();
        param_types[2] = type_any();
        expr->type = type_function(type_array(type_double()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "randomFillBigIntBits") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_array(type_bigint());
        param_types[1] = type_int();
        expr->type = type_function(type_array(type_bigint()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "randomFillBigIntRange") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_array(type_bigint());
        param_types[1] = type_bigint();
        param_types[2] = type_bigint();
        expr->type = type_function(type_array(type_bigint()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "secureRandom") == 0) {
        expr->type = type_function(type_result_tuple(tc, type_double()), NULL, 0);
        return expr->type;
    }

    if (strcmp(expr->identifier, "secureRandomInt") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "secureRandomDouble") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_double()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "secureRandomBigIntBits") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bigint()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "secureRandomBigIntRange") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_bigint();
        param_types[1] = type_bigint();
        expr->type = type_function(type_result_tuple(tc, type_bigint()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "secureRandomFillInt") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_array(type_int());
        param_types[1] = type_int();
        param_types[2] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_array(type_int())), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "secureRandomFillDouble") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_array(type_double());
        param_types[1] = type_any();
        param_types[2] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_array(type_double())), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "secureRandomFillBigIntBits") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_array(type_bigint());
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_array(type_bigint())), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "secureRandomFillBigIntRange") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_array(type_bigint());
        param_types[1] = type_bigint();
        param_types[2] = type_bigint();
        expr->type = type_function(type_result_tuple(tc, type_array(type_bigint())), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "arrayWithSize") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_array(type_any())), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "push") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_void(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "copyInto") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_void(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "reversePrefix") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_int();
        expr->type = type_function(type_void(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "rotatePrefixLeft") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_int();
        expr->type = type_function(type_void(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "rotatePrefixRight") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_int();
        expr->type = type_function(type_void(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "pop") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_any(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "keys") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        // keys() supports arrays and maps at runtime, so key element type can vary.
        expr->type = type_function(type_array(type_any()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "values") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_array(type_any()), param_types, 1);
        return expr->type;
    }

    // Array utilities
    if (strcmp(expr->identifier, "sort") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_array(type_any());
        expr->type = type_function(type_array(type_any()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "reverse") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_array(type_any());
        expr->type = type_function(type_array(type_any()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "findArray") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_array(type_any());
        param_types[1] = type_any();
        expr->type = type_function(type_int(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "contains") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_array(type_any());
        param_types[1] = type_any();
        expr->type = type_function(type_bool(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "slice") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_int();
        param_types[2] = type_int();
        expr->type = type_function(type_any(), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "join") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_array(type_any());
        param_types[1] = type_string();
        expr->type = type_function(type_string(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "read_line") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "read_all") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "write_line") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "write_all") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "file_open") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "file_read_line") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        Type* ok_type = type_string();
        if (ok_type) ok_type->nullable = true;
        expr->type = type_function(type_result_tuple(tc, ok_type), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "file_close") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_void(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "ioReadLine") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        Type* ok_type = type_string();
        if (ok_type) ok_type->nullable = true;
        expr->type = type_function(type_result_tuple(tc, ok_type), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "ioReadAll") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "ioReadChunk") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_int();
        Type* ok_type = type_string();
        if (ok_type) ok_type->nullable = true;
        expr->type = type_function(type_result_tuple(tc, ok_type), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "ioReadChunkBytes") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_int();
        Type* ok_type = type_bytes();
        if (ok_type) ok_type->nullable = true;
        expr->type = type_function(type_result_tuple(tc, ok_type), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "ioReadExactlyBytes") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bytes()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "ioWriteAll") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "ioWriteBytesAll") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_bytes();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "ioCopy") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        param_types[2] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "readBytes") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_bytes()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "writeBytes") == 0 || strcmp(expr->identifier, "appendBytes") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_bytes();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "stdoutWriteBytes") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_bytes();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "envGet") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        Type* ok_type = type_string();
        if (ok_type) ok_type->nullable = true;
        expr->type = type_function(type_result_tuple(tc, ok_type), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "exists") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "delete") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "httpGet") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "httpGetWithHeaders") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_map(type_string(), type_string());
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "httpPost") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "httpPostWithHeaders") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        param_types[2] = type_map(type_string(), type_string());
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "httpRequest") == 0) {
        Type** param_types = (Type**)safe_malloc(5 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        param_types[2] = type_string();
        param_types[2]->nullable = true;
        param_types[3] = type_map(type_string(), type_string());
        param_types[3]->nullable = true;
        param_types[4] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_map(type_string(), type_any())), param_types, 5);
        return expr->type;
    }

    if (strcmp(expr->identifier, "httpRequestHead") == 0) {
        Type** param_types = (Type**)safe_malloc(5 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        param_types[2] = type_string();
        param_types[2]->nullable = true;
        param_types[3] = type_map(type_string(), type_string());
        param_types[3]->nullable = true;
        param_types[4] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_map(type_string(), type_any())), param_types, 5);
        return expr->type;
    }

    if (strcmp(expr->identifier, "httpRequestWithOptions") == 0) {
        Type** param_types = (Type**)safe_malloc(6 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        param_types[2] = type_string();
        param_types[2]->nullable = true;
        param_types[3] = type_map(type_string(), type_string());
        param_types[3]->nullable = true;
        param_types[4] = type_int();
        param_types[5] = type_map(type_string(), type_any());
        param_types[5]->nullable = true;
        expr->type = type_function(type_result_tuple(tc, type_map(type_string(), type_any())), param_types, 6);
        return expr->type;
    }

    if (strcmp(expr->identifier, "httpRequestHeadWithOptions") == 0) {
        Type** param_types = (Type**)safe_malloc(6 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_string();
        param_types[2] = type_string();
        param_types[2]->nullable = true;
        param_types[3] = type_map(type_string(), type_string());
        param_types[3]->nullable = true;
        param_types[4] = type_int();
        param_types[5] = type_map(type_string(), type_any());
        param_types[5]->nullable = true;
        expr->type = type_function(type_result_tuple(tc, type_map(type_string(), type_any())), param_types, 6);
        return expr->type;
    }

    if (strcmp(expr->identifier, "httpReadRequest") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_map(type_string(), type_any())), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "httpWriteResponse") == 0) {
        Type** param_types = (Type**)safe_malloc(4 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_int();
        param_types[2] = type_string();
        param_types[3] = type_map(type_string(), type_string());
        param_types[3]->nullable = true;
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 4);
        return expr->type;
    }

    // TCP socket builtins
    if (strcmp(expr->identifier, "tcpConnect") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "tcpListen") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "tcpAccept") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "tcpSend") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "tcpReceive") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "tcpClose") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_void(), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "tlsIsAvailable") == 0) {
        expr->type = type_function(type_bool(), NULL, 0);
        return expr->type;
    }

    if (strcmp(expr->identifier, "tlsConnect") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_int();
        param_types[2] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "tlsSend") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "tlsReceive") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_string()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "tlsClose") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteIsAvailable") == 0) {
        expr->type = type_function(type_bool(), NULL, 0);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteOpen") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteClose") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteExec") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteQuery") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_string();
        Type* row_type = type_map(type_string(), type_any());
        Type* ok_type = type_array(row_type);
        expr->type = type_function(type_result_tuple(tc, ok_type), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqlitePrepare") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteBindInt") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_int();
        param_types[2] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteBindDouble") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_int();
        param_types[2] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteBindString") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_int();
        param_types[2] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteBindBytes") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_int();
        param_types[2] = type_bytes();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteBindNull") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteReset") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteClearBindings") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteChanges") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteLastInsertRowId") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteStep") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        Type* ok_type = type_map(type_string(), type_any());
        ok_type->nullable = true;
        expr->type = type_function(type_result_tuple(tc, ok_type), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "sqliteFinalize") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "processSpawn") == 0) {
        Type** param_types = (Type**)safe_malloc(4 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_array(type_string());
        param_types[2] = type_bool();
        param_types[3] = type_bool();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 4);
        return expr->type;
    }

    if (strcmp(expr->identifier, "processWriteStdin") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_string();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "processCloseStdin") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "processReadStdout") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_int();
        Type* ok_type = type_string();
        ok_type->nullable = true;
        expr->type = type_function(type_result_tuple(tc, ok_type), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "processReadStderr") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_int();
        Type* ok_type = type_string();
        ok_type->nullable = true;
        expr->type = type_function(type_result_tuple(tc, ok_type), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "processWait") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_map(type_string(), type_any())), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "processKill") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncChannelCreate") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncChannelSend") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_any();
        param_types[2] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncChannelSendTyped") == 0) {
        Type** param_types = (Type**)safe_malloc(4 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_any();
        param_types[2] = type_any();
        param_types[3] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 4);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncChannelRecv") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncChannelRecvTyped") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_any();
        param_types[2] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncChannelClose") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncSharedCreate") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncSharedCreateTyped") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncSharedGet") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncSharedGetTyped") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncSharedSet") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncSharedSetTyped") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_any();
        param_types[2] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncThreadSpawn") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_int();
        param_types[2] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncThreadSpawnTyped") == 0) {
        Type** param_types = (Type**)safe_malloc(5 * sizeof(Type*));
        param_types[0] = type_string();
        param_types[1] = type_any();
        param_types[2] = type_any();
        param_types[3] = type_int();
        param_types[4] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 5);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncThreadJoin") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncThreadJoinTyped") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_any();
        param_types[2] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncThreadInbox") == 0) {
        expr->type = type_function(type_int(), NULL, 0);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncThreadOutbox") == 0) {
        expr->type = type_function(type_int(), NULL, 0);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncThreadArgTyped") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncArcCreate") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncArcClone") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncArcGuardAcquire") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_int()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncArcGuardRead") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_any()), param_types, 1);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncArcGuardWrite") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_int();
        param_types[1] = type_any();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "syncArcGuardRelease") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_int();
        expr->type = type_function(type_result_tuple(tc, type_bool()), param_types, 1);
        return expr->type;
    }
    
    // Map builtins
    if (strcmp(expr->identifier, "mapGet") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_any(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "mapGetString") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_string();
        expr->type = type_function(type_any(), param_types, 2);
        return expr->type;
    }
    
    if (strcmp(expr->identifier, "mapSet") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        param_types[2] = type_any();
        expr->type = type_function(type_void(), param_types, 3);
        return expr->type;
    }

    if (strcmp(expr->identifier, "mapSetString") == 0) {
        Type** param_types = (Type**)safe_malloc(3 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_string();
        param_types[2] = type_any();
        expr->type = type_function(type_void(), param_types, 3);
        return expr->type;
    }
    
    if (strcmp(expr->identifier, "mapHas") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_bool(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "mapHasString") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_string();
        expr->type = type_function(type_bool(), param_types, 2);
        return expr->type;
    }
    
    if (strcmp(expr->identifier, "mapDelete") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_void(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "mapDeleteString") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_string();
        expr->type = type_function(type_void(), param_types, 2);
        return expr->type;
    }
    
    if (strcmp(expr->identifier, "mapCount") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_int(), param_types, 1);
        return expr->type;
    }
    
    // Set builtins
    if (strcmp(expr->identifier, "setAdd") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_void(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "setAddString") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_string();
        expr->type = type_function(type_void(), param_types, 2);
        return expr->type;
    }
    
    if (strcmp(expr->identifier, "setHas") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_bool(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "setHasString") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_string();
        expr->type = type_function(type_bool(), param_types, 2);
        return expr->type;
    }
    
    if (strcmp(expr->identifier, "setRemove") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_any();
        expr->type = type_function(type_void(), param_types, 2);
        return expr->type;
    }

    if (strcmp(expr->identifier, "setRemoveString") == 0) {
        Type** param_types = (Type**)safe_malloc(2 * sizeof(Type*));
        param_types[0] = type_any();
        param_types[1] = type_string();
        expr->type = type_function(type_void(), param_types, 2);
        return expr->type;
    }
    
    if (strcmp(expr->identifier, "setCount") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_int(), param_types, 1);
        return expr->type;
    }
    
    if (strcmp(expr->identifier, "setToArray") == 0) {
        Type** param_types = (Type**)safe_malloc(sizeof(Type*));
        param_types[0] = type_any();
        expr->type = type_function(type_array(type_any()), param_types, 1);
        return expr->type;
    }

    // Built-in global variable: argv (command-line arguments)
    if (strcmp(expr->identifier, "argv") == 0) {
        expr->type = type_array(type_string());
        return expr->type;
    }

    int scope_index = -1;
    Symbol* sym = typechecker_lookup_with_scope(tc, expr->identifier, &scope_index);
    if (!sym) {
        // Report undeclared identifier but assign type_any to prevent cascading errors
        char error_msg[512];
        snprintf(error_msg, 512, "Undeclared identifier: %s", expr->identifier);
        typechecker_error(tc, error_msg, expr->file, expr->line, expr->column);
        expr->type = type_any();
        return expr->type;
    }

    if (!symbol_is_accessible_from_file(sym, expr->file)) {
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg), "Symbol '%s' is private to its module", expr->identifier);
        typechecker_error(tc, error_msg, expr->file, expr->line, expr->column);
        if (expr->type) type_free(expr->type);
        expr->type = type_any();
        return expr->type;
    }

    if (sym->is_type_alias) {
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg), "Type alias '%s' cannot be used as a value", expr->identifier);
        typechecker_error(tc, error_msg, expr->file, expr->line, expr->column);
        if (expr->type) type_free(expr->type);
        expr->type = type_any();
        return expr->type;
    }

    if (scope_index >= 0) {
        FuncLiteralCaptureContext* capture_ctx = typechecker_current_capture_context(tc);
        if (capture_ctx && scope_index < capture_ctx->outer_local_count) {
            typechecker_capture_context_add(tc, expr->identifier);
        }
    }

    if (expr->type) type_free(expr->type);
    expr->type = sym->type ? type_clone(sym->type) : type_any();
    return expr->type;
}

static Type* typecheck_binary(TypeChecker* tc, Expr* expr) {
    Type* left = typecheck_expression(tc, expr->binary.left);
    Type* right = typecheck_expression(tc, expr->binary.right);
    
    TokenType op = expr->binary.op;

    bool left_any = left->kind == TYPE_ANY;
    bool right_any = right->kind == TYPE_ANY;
    bool left_bigint = left->kind == TYPE_BIGINT;
    bool right_bigint = right->kind == TYPE_BIGINT;

    // Arithmetic
    if (op == TOKEN_PLUS || op == TOKEN_MINUS || op == TOKEN_STAR || op == TOKEN_SLASH || op == TOKEN_PERCENT) {
        if (!left_any && left->kind != TYPE_INT && left->kind != TYPE_DOUBLE &&
            left->kind != TYPE_BIGINT && left->kind != TYPE_STRING) {
            typechecker_error(tc, "Left operand must be int/double/bigint/string", expr->file, expr->line, 0);
        }
        if (!right_any && right->kind != TYPE_INT && right->kind != TYPE_DOUBLE &&
            right->kind != TYPE_BIGINT && right->kind != TYPE_STRING) {
            typechecker_error(tc, "Right operand must be int/double/bigint/string", expr->file, expr->line, 0);
        }

        if (expr->type) type_free(expr->type);

        // `%` is int-only.
        if (op == TOKEN_PERCENT) {
            if (!left_any && left->kind != TYPE_INT && left->kind != TYPE_BIGINT) {
                typechecker_error(tc, "Left operand must be int/bigint for %", expr->file, expr->line, 0);
            }
            if (!right_any && right->kind != TYPE_INT && right->kind != TYPE_BIGINT) {
                typechecker_error(tc, "Right operand must be int/bigint for %", expr->file, expr->line, 0);
            }
            if (left_bigint || right_bigint) {
                expr->type = type_bigint();
            } else {
                expr->type = type_int();
            }
            return expr->type;
        }

        // `+` supports string concatenation (string + string).
        if (op == TOKEN_PLUS && !left_any && !right_any && left->kind == TYPE_STRING && right->kind == TYPE_STRING) {
            expr->type = type_string();
            return expr->type;
        }

        if (!left_any && !right_any &&
            (left->kind == TYPE_STRING || right->kind == TYPE_STRING)) {
            expr->type = type_any();
            return expr->type;
        }

        if ((left_bigint && right->kind == TYPE_DOUBLE) || (right_bigint && left->kind == TYPE_DOUBLE)) {
            typechecker_error(tc, "Cannot mix bigint and double", expr->file, expr->line, 0);
            expr->type = type_any();
            return expr->type;
        }

        if (left_bigint || right_bigint) {
            expr->type = type_bigint();
            return expr->type;
        }

        // Numeric ops require matching numeric types at runtime (no implicit promotion).
        if (!left_any && !right_any) {
            if ((left->kind == TYPE_INT && right->kind == TYPE_DOUBLE) ||
                (left->kind == TYPE_DOUBLE && right->kind == TYPE_INT)) {
                typechecker_error(tc, "Mixed int/double operands require an explicit cast", expr->file, expr->line, 0);
                expr->type = type_any();
                return expr->type;
            }
        }

        if (left->kind == TYPE_DOUBLE || right->kind == TYPE_DOUBLE) {
            expr->type = type_double();
        } else if (left->kind == TYPE_STRING || right->kind == TYPE_STRING) {
            // Non-string concatenation isn't supported at runtime.
            expr->type = type_any();
        } else {
            expr->type = type_int();
        }

        return expr->type;
    }

    // Boolean ops (bool-only)
    if (op == TOKEN_AND || op == TOKEN_OR) {
        if (!left_any && left->kind != TYPE_BOOL) {
            typechecker_error(tc, "Left operand must be bool for logical operation", expr->file, expr->line, 0);
        }
        if (!right_any && right->kind != TYPE_BOOL) {
            typechecker_error(tc, "Right operand must be bool for logical operation", expr->file, expr->line, 0);
        }
        if (expr->type) type_free(expr->type);
        expr->type = type_bool();
        return expr->type;
    }

    // Bitwise ops (int/bigint only)
    if (op == TOKEN_BIT_AND || op == TOKEN_BIT_OR || op == TOKEN_BIT_XOR) {
        if (!left_any && left->kind != TYPE_INT && left->kind != TYPE_BIGINT) {
            typechecker_error(tc, "Left operand must be int/bigint for bitwise operation", expr->file, expr->line, 0);
        }
        if (!right_any && right->kind != TYPE_INT && right->kind != TYPE_BIGINT) {
            typechecker_error(tc, "Right operand must be int/bigint for bitwise operation", expr->file, expr->line, 0);
        }

        if (expr->type) type_free(expr->type);
        if (left_bigint || right_bigint) {
            expr->type = type_bigint();
        } else {
            expr->type = type_int();
        }
        return expr->type;
    }

    // Equality / comparisons always return bool
    if (op == TOKEN_EQ_EQ || op == TOKEN_BANG_EQ ||
        op == TOKEN_LT || op == TOKEN_LT_EQ || op == TOKEN_GT || op == TOKEN_GT_EQ) {
        if (op == TOKEN_LT || op == TOKEN_LT_EQ || op == TOKEN_GT || op == TOKEN_GT_EQ) {
            // Runtime supports int-int, double-double, and bigint comparisons.
            if (!left_any && left->kind != TYPE_INT && left->kind != TYPE_DOUBLE && left->kind != TYPE_BIGINT) {
                typechecker_error(tc, "Left operand must be numeric for comparison", expr->file, expr->line, 0);
            }
            if (!right_any && right->kind != TYPE_INT && right->kind != TYPE_DOUBLE && right->kind != TYPE_BIGINT) {
                typechecker_error(tc, "Right operand must be numeric for comparison", expr->file, expr->line, 0);
            }
            if (!left_any && !right_any) {
                if ((left->kind == TYPE_INT && right->kind == TYPE_DOUBLE) ||
                    (left->kind == TYPE_DOUBLE && right->kind == TYPE_INT)) {
                    typechecker_error(tc, "Mixed int/double comparison requires an explicit cast", expr->file, expr->line, 0);
                }
                if ((left_bigint && right->kind == TYPE_DOUBLE) ||
                    (right_bigint && left->kind == TYPE_DOUBLE)) {
                    typechecker_error(tc, "Mixed bigint/double comparison requires an explicit cast", expr->file, expr->line, 0);
                }
            }
        }

        if (expr->type) type_free(expr->type);
        expr->type = type_bool();
        return expr->type;
    }

    if (expr->type) type_free(expr->type);
    expr->type = type_any();
    
    return expr->type;
}

static Type* typecheck_unary(TypeChecker* tc, Expr* expr) {
    Type* operand = typecheck_expression(tc, expr->unary.operand);
    
    TokenType op = expr->unary.op;

    bool operand_any = operand->kind == TYPE_ANY;

    if (op == TOKEN_MINUS) {
        if (!operand_any && operand->kind != TYPE_INT && operand->kind != TYPE_DOUBLE && operand->kind != TYPE_BIGINT) {
            typechecker_error(tc, "Operand must be numeric", expr->file, expr->line, 0);
        }
        if (expr->type) type_free(expr->type);
        if (operand->kind == TYPE_BIGINT) {
            expr->type = type_bigint();
        } else {
            expr->type = operand->kind == TYPE_DOUBLE ? type_double() : type_int();
        }
        return expr->type;
    }

    if (op == TOKEN_NOT) {
        if (!operand_any && operand->kind != TYPE_BOOL) {
            typechecker_error(tc, "Operand must be bool for logical not", expr->file, expr->line, 0);
        }
        if (expr->type) type_free(expr->type);
        expr->type = type_bool();
        return expr->type;
    }

    if (op == TOKEN_BIT_NOT) {
        if (!operand_any && operand->kind != TYPE_INT && operand->kind != TYPE_BIGINT) {
            typechecker_error(tc, "Operand must be int/bigint for bitwise not", expr->file, expr->line, 0);
        }
        if (expr->type) type_free(expr->type);
        expr->type = operand->kind == TYPE_BIGINT ? type_bigint() : type_int();
        return expr->type;
    }

    if (expr->type) type_free(expr->type);
    expr->type = type_any();
    return expr->type;
}

static Type* typecheck_call(TypeChecker* tc, Expr* expr) {
    Type* prechecked_arg0_type = NULL;
    bool has_prechecked_arg0_type = false;
    if (expr->call.callee &&
        expr->call.callee->kind == EXPR_IDENTIFIER &&
        expr->call.callee->identifier &&
        typechecker_interface_has_method_named(tc, expr->call.callee->identifier) &&
        expr->call.arg_count > 0 &&
        expr->call.args &&
        expr->call.args[0]) {
        int callee_scope = -1;
        (void)typechecker_lookup_with_scope(tc, expr->call.callee->identifier, &callee_scope);
        if (callee_scope < 0) {
            prechecked_arg0_type = typecheck_expression(tc, expr->call.args[0]);
            has_prechecked_arg0_type = true;
            Type* receiver_type = prechecked_arg0_type;
            Type* receiver_constraint = NULL;
            if (receiver_type && receiver_type->kind == TYPE_TYPE_PARAM) {
                receiver_constraint = typechecker_resolve_type_param_constraint(tc, receiver_type);
                if (receiver_constraint) {
                    receiver_type = receiver_constraint;
                }
            }
            if (receiver_type &&
                receiver_type->kind == TYPE_INTERFACE &&
                receiver_type->interface_def) {
                if (receiver_constraint && expr->call.args[0]) {
                    if (expr->call.args[0]->type) {
                        type_free(expr->call.args[0]->type);
                    }
                    expr->call.args[0]->type = type_clone(receiver_type);
                    prechecked_arg0_type = expr->call.args[0]->type;
                }
                const char* method_name = expr->call.callee->identifier;
                Type* iface_method_type = interface_def_get_method_type(receiver_type->interface_def, method_name);
                if (iface_method_type && iface_method_type->kind == TYPE_FUNCTION) {
                    if (expr->call.arg_count != iface_method_type->param_count + 1) {
                        char message[192];
                        snprintf(message,
                                 sizeof(message),
                                 "Wrong number of arguments for interface method '%s': expected %d, got %d",
                                 method_name,
                                 iface_method_type->param_count + 1,
                                 expr->call.arg_count);
                        typechecker_error(tc, message, expr->file, expr->line, expr->column);
                    }

                    for (int i = 1; i < expr->call.arg_count; i++) {
                        Type* expected = NULL;
                        if ((i - 1) < iface_method_type->param_count) {
                            expected = iface_method_type->param_types[i - 1];
                        }

                        if (expected && expected->kind == TYPE_RECORD &&
                            expr->call.args[i] && expr->call.args[i]->kind == EXPR_RECORD_LITERAL) {
                            expr->call.args[i]->record_literal.record_type = expected;
                        }
                        if (expected && expected->kind == TYPE_SET &&
                            expr_is_empty_map_literal(expr->call.args[i])) {
                            expr_coerce_empty_map_literal_to_empty_set_literal(expr->call.args[i]);
                        }
                        if (expected && expected->kind == TYPE_ARRAY && expected->element_type &&
                            expected->element_type->kind == TYPE_RECORD &&
                            expr->call.args[i] && expr->call.args[i]->kind == EXPR_ARRAY_LITERAL) {
                            for (int j = 0; j < expr->call.args[i]->array_literal.element_count; j++) {
                                Expr* elem = expr->call.args[i]->array_literal.elements[j];
                                if (elem && elem->kind == EXPR_RECORD_LITERAL) {
                                    elem->record_literal.record_type = expected->element_type;
                                }
                            }
                        }

                        Type* arg_type = typecheck_expression_with_expected(tc,
                                                                            expr->call.args[i],
                                                                            expected);
                        if (expected &&
                            !typechecker_types_assignable(tc,
                                                          expected,
                                                          arg_type,
                                                          expr->file,
                                                          expr->line,
                                                          expr->column)) {
                            typechecker_error_expected_got(tc,
                                                           "Argument type mismatch",
                                                           expected,
                                                           arg_type,
                                                           expr->file,
                                                           expr->line,
                                                           0);
                        }
                    }

                    if (expr->type) type_free(expr->type);
                    expr->type = iface_method_type->return_type
                        ? type_clone(iface_method_type->return_type)
                        : type_any();
                    return expr->type;
                }
            }
        }
    }

    Type* callee_type = typecheck_expression(tc, expr->call.callee);
    Type* arg0_type = NULL;
    Type* arg1_type = NULL;
    bool is_generic_function_call = false;
    Type** generic_bound_types = NULL;
    
    if (callee_type->kind != TYPE_FUNCTION && callee_type->kind != TYPE_ANY) {
        typechecker_error(tc, "Can only call functions", expr->file, expr->line, 0);
        if (expr->type) type_free(expr->type);
        expr->type = type_any();
        return expr->type;
    }

    // For TYPE_ANY callee (runtime builtins), typecheck args but return any
    if (callee_type->kind == TYPE_ANY) {
        if (expr->call.type_arg_count > 0) {
            typechecker_error(tc,
                              "Cannot use explicit generic type arguments with untyped call target",
                              expr->file,
                              expr->line,
                              expr->column);
        }
        for (int i = 0; i < expr->call.arg_count; i++) {
            typecheck_expression(tc, expr->call.args[i]);
        }
        expr->type = type_any();
        return expr->type;
    }
    
    if (expr->call.arg_count != callee_type->param_count) {
        char message[160];
        snprintf(message,
                 sizeof(message),
                 "Wrong number of arguments: expected %d, got %d",
                 callee_type->param_count,
                 expr->call.arg_count);
        typechecker_error(tc, message, expr->file, expr->line, expr->column);
    }

    if (callee_type->type_param_count > 0 && callee_type->type_param_names) {
        is_generic_function_call = true;
        generic_bound_types = (Type**)safe_calloc((size_t)callee_type->type_param_count, sizeof(Type*));
    }

    if (expr->call.type_arg_count > 0) {
        if (!is_generic_function_call) {
            typechecker_error(tc,
                              "Explicit generic type arguments require a generic function callee",
                              expr->file,
                              expr->line,
                              expr->column);
        } else {
            if (expr->call.type_arg_count != callee_type->type_param_count) {
                char message[192];
                snprintf(message,
                         sizeof(message),
                         "Wrong number of generic type arguments: expected %d, got %d",
                         callee_type->type_param_count,
                         expr->call.type_arg_count);
                typechecker_error(tc, message, expr->file, expr->line, expr->column);
            }

            int explicit_count = expr->call.type_arg_count;
            if (explicit_count > callee_type->type_param_count) {
                explicit_count = callee_type->type_param_count;
            }

            for (int i = 0; i < explicit_count; i++) {
                Type* explicit_type_arg = (expr->call.type_args && expr->call.type_args[i])
                    ? expr->call.type_args[i]
                    : NULL;
                Type* resolved_type_arg = explicit_type_arg
                    ? typechecker_resolve_type(tc, explicit_type_arg)
                    : type_any();
                if (expr->call.type_args && expr->call.type_args[i]) {
                    expr->call.type_args[i] = resolved_type_arg;
                }
                if (generic_bound_types[i]) {
                    type_free(generic_bound_types[i]);
                }
                generic_bound_types[i] = resolved_type_arg ? type_clone(resolved_type_arg) : type_any();
                if (!explicit_type_arg && resolved_type_arg) {
                    type_free(resolved_type_arg);
                }
            }
        }
    }
    
    for (int i = 0; i < expr->call.arg_count && i < callee_type->param_count; i++) {
        Type* declared_param_type = callee_type->param_types[i];
        bool arg_was_empty_map_literal = expr_is_empty_map_literal(expr->call.args[i]);

        Type* contextual_param_type = declared_param_type;
        bool free_contextual_param_type = false;
        if (is_generic_function_call) {
            bool unresolved_context = false;
            contextual_param_type = typechecker_substitute_generic_type(declared_param_type,
                                                                        callee_type->type_param_names,
                                                                        generic_bound_types,
                                                                        callee_type->type_param_count,
                                                                        &unresolved_context);
            contextual_param_type = typechecker_resolve_type(tc, contextual_param_type);
            free_contextual_param_type = true;
        }

        Type* effective_param_type = contextual_param_type ? contextual_param_type : declared_param_type;
        if (effective_param_type && effective_param_type->kind == TYPE_RECORD &&
            expr->call.args[i] && expr->call.args[i]->kind == EXPR_RECORD_LITERAL) {
            expr->call.args[i]->record_literal.record_type = effective_param_type;
        }
        if (effective_param_type && effective_param_type->kind == TYPE_SET &&
            arg_was_empty_map_literal) {
            expr_coerce_empty_map_literal_to_empty_set_literal(expr->call.args[i]);
        }
        if (effective_param_type && effective_param_type->kind == TYPE_ARRAY && effective_param_type->element_type &&
            effective_param_type->element_type->kind == TYPE_RECORD &&
            expr->call.args[i] && expr->call.args[i]->kind == EXPR_ARRAY_LITERAL) {
            for (int j = 0; j < expr->call.args[i]->array_literal.element_count; j++) {
                Expr* elem = expr->call.args[i]->array_literal.elements[j];
                if (elem && elem->kind == EXPR_RECORD_LITERAL) {
                    elem->record_literal.record_type = effective_param_type->element_type;
                }
            }
        }

        bool reuse_prechecked_arg0 = (i == 0 && has_prechecked_arg0_type);
        if (reuse_prechecked_arg0) {
            // Arg0 precheck is only safe when contextual typing did not change
            // how the argument should be analyzed.
            if ((effective_param_type && effective_param_type->kind == TYPE_RECORD &&
                 expr->call.args[i] && expr->call.args[i]->kind == EXPR_RECORD_LITERAL) ||
                (effective_param_type && effective_param_type->kind == TYPE_SET &&
                 arg_was_empty_map_literal) ||
                (effective_param_type && effective_param_type->kind == TYPE_ARRAY && effective_param_type->element_type &&
                 effective_param_type->element_type->kind == TYPE_RECORD &&
                 expr->call.args[i] && expr->call.args[i]->kind == EXPR_ARRAY_LITERAL)) {
                reuse_prechecked_arg0 = false;
            }
            if (contextual_param_type &&
                expr->call.args[i] &&
                expr->call.args[i]->kind == EXPR_CALL) {
                reuse_prechecked_arg0 = false;
            }
        }

        Type* arg_type = reuse_prechecked_arg0
            ? prechecked_arg0_type
            : typecheck_expression_with_expected(tc, expr->call.args[i], contextual_param_type);
        if (i == 0) arg0_type = arg_type;
        if (i == 1) arg1_type = arg_type;

        if (is_generic_function_call) {
            char* infer_call_target = typechecker_call_target_name(tc, expr->call.callee);
            const char* call_display = infer_call_target ? infer_call_target : "<call>";
            char infer_context[224];
            snprintf(infer_context,
                     sizeof(infer_context),
                     "inferring from argument #%d of call to '%.*s'",
                     i + 1,
                     96,
                     call_display);
            (void)typechecker_infer_generic_bindings(tc,
                                                     declared_param_type,
                                                     arg_type,
                                                     callee_type->type_param_names,
                                                     generic_bound_types,
                                                     callee_type->type_param_count,
                                                     expr->file,
                                                     expr->line,
                                                     expr->column,
                                                     infer_context);
            if (infer_call_target) free(infer_call_target);
        }

        Type* expected_param_type = declared_param_type;
        bool free_expected_param_type = false;
        if (is_generic_function_call) {
            bool unresolved = false;
            expected_param_type = typechecker_substitute_generic_type(declared_param_type,
                                                                      callee_type->type_param_names,
                                                                      generic_bound_types,
                                                                      callee_type->type_param_count,
                                                                      &unresolved);
            expected_param_type = typechecker_resolve_type(tc, expected_param_type);
            free_expected_param_type = true;
        }

        bool allow_interface_receiver = typechecker_call_allows_interface_receiver(tc,
                                                                                   expr,
                                                                                   callee_type,
                                                                                   i,
                                                                                   expected_param_type,
                                                                                   arg_type,
                                                                                   expr->file,
                                                                                   expr->line,
                                                                                   expr->column);
        if (!allow_interface_receiver &&
            !typechecker_types_assignable(tc,
                                          expected_param_type,
                                          arg_type,
                                          expr->file,
                                          expr->line,
                                          expr->column)) {
            typechecker_error_expected_got(tc,
                                           "Argument type mismatch",
                                           expected_param_type,
                                           arg_type,
                                           expr->file,
                                           expr->line,
                                           0);
        }

        if (free_expected_param_type && expected_param_type) {
            type_free(expected_param_type);
        }
        if (free_contextual_param_type && contextual_param_type) {
            type_free(contextual_param_type);
        }
    }
    
    if (expr->type) type_free(expr->type);

    if (expr->call.callee && expr->call.callee->kind == EXPR_IDENTIFIER &&
        strcmp(expr->call.callee->identifier, "must") == 0 &&
        arg0_type) {
        if (expr->call.arg_count != 1) {
            if (generic_bound_types) {
                for (int i = 0; i < callee_type->type_param_count; i++) {
                    if (generic_bound_types[i]) type_free(generic_bound_types[i]);
                }
                free(generic_bound_types);
            }
            expr->type = type_any();
            return expr->type;
        }
        if (arg0_type->kind != TYPE_TUPLE || tuple_type_get_arity(arg0_type) != 2) {
            typechecker_error(tc, "must expects (value, Error?) tuple", expr->file, expr->line, 0);
            if (generic_bound_types) {
                for (int i = 0; i < callee_type->type_param_count; i++) {
                    if (generic_bound_types[i]) type_free(generic_bound_types[i]);
                }
                free(generic_bound_types);
            }
            expr->type = type_any();
            return expr->type;
        }

        Type* err_elem = tuple_type_get_element(arg0_type, 1);
        Type* expected_err = type_error_nullable(tc);
        if (expected_err && err_elem && !type_assignable(expected_err, err_elem)) {
            typechecker_error(tc, "must expects (value, Error?) tuple", expr->file, expr->line, 0);
        }
        if (expected_err) type_free(expected_err);

        Type* ok_elem = tuple_type_get_element(arg0_type, 0);
        if (generic_bound_types) {
            for (int i = 0; i < callee_type->type_param_count; i++) {
                if (generic_bound_types[i]) type_free(generic_bound_types[i]);
            }
            free(generic_bound_types);
        }
        expr->type = ok_elem ? type_clone(ok_elem) : type_any();
        return expr->type;
    }

    if (is_generic_function_call &&
        expr->call.type_arg_count == 0 &&
        callee_type->type_param_count > 0 &&
        callee_type->type_param_names &&
        callee_type->return_type &&
        typechecker_has_unbound_generic_type_params(generic_bound_types, callee_type->type_param_count) &&
        tc->expected_expr_type) {
        Type* contextual_expected =
            typechecker_contextual_expected_type_for_inference(tc, tc->expected_expr_type);
        if (contextual_expected) {
            char* infer_call_target = typechecker_call_target_name(tc, expr->call.callee);
            const char* call_display = infer_call_target ? infer_call_target : "<call>";
            char infer_context[224];
            snprintf(infer_context,
                     sizeof(infer_context),
                     "inferring from expected return type of call to '%.*s'",
                     96,
                     call_display);
            (void)typechecker_infer_generic_bindings(tc,
                                                     callee_type->return_type,
                                                     contextual_expected,
                                                     callee_type->type_param_names,
                                                     generic_bound_types,
                                                     callee_type->type_param_count,
                                                     expr->file,
                                                     expr->line,
                                                     expr->column,
                                                     infer_context);
            if (infer_call_target) free(infer_call_target);
            type_free(contextual_expected);
        }
    }

    if (is_generic_function_call && callee_type->type_param_count > 0 && callee_type->type_param_names) {
        char unresolved_params[256];
        char inferred_params[256];
        unresolved_params[0] = '\0';
        inferred_params[0] = '\0';

        for (int i = 0; i < callee_type->type_param_count; i++) {
            const char* param_name =
                (callee_type->type_param_names[i] && callee_type->type_param_names[i][0] != '\0')
                    ? callee_type->type_param_names[i]
                    : "T";
            if (!generic_bound_types[i]) {
                typechecker_append_list_item(unresolved_params, sizeof(unresolved_params), param_name);
                continue;
            }

            char bound_buf[96];
            bound_buf[0] = '\0';
            type_to_string(generic_bound_types[i], bound_buf, sizeof(bound_buf));
            char assignment[128];
            snprintf(assignment, sizeof(assignment), "%s=%s", param_name, bound_buf);
            typechecker_append_list_item(inferred_params, sizeof(inferred_params), assignment);
        }

        if (unresolved_params[0] != '\0') {
            char* call_target = typechecker_call_target_name(tc, expr->call.callee);
            char type_param_list[160];
            typechecker_format_generic_type_param_list(callee_type->type_param_names,
                                                       callee_type->type_param_count,
                                                       type_param_list,
                                                       sizeof(type_param_list));

            const char* call_display = call_target ? call_target : "<call>";
            char msg[768];
            if (inferred_params[0] != '\0') {
                snprintf(msg,
                         sizeof(msg),
                         "Cannot infer generic type parameter(s) %.*s for call to '%.*s'; inferred so far: %.*s; add explicit type arguments, e.g. %.*s%.*s(...)",
                         120,
                         unresolved_params,
                         96,
                         call_display,
                         180,
                         inferred_params,
                         96,
                         call_display,
                         80,
                         type_param_list);
            } else {
                snprintf(msg,
                         sizeof(msg),
                         "Cannot infer generic type parameter(s) %.*s for call to '%.*s'; add explicit type arguments, e.g. %.*s%.*s(...)",
                         120,
                         unresolved_params,
                         96,
                         call_display,
                         96,
                         call_display,
                         80,
                         type_param_list);
            }
            typechecker_error(tc, msg, expr->file, expr->line, expr->column);
            if (call_target) free(call_target);
        }

        if (callee_type->type_param_constraints) {
            for (int i = 0; i < callee_type->type_param_count; i++) {
                Type* constraint = callee_type->type_param_constraints[i];
                Type* bound = generic_bound_types[i];
                if (!constraint || !bound) continue;
                bool unresolved_constraint = false;
                Type* concrete_constraint = typechecker_substitute_generic_type(constraint,
                                                                                callee_type->type_param_names,
                                                                                generic_bound_types,
                                                                                callee_type->type_param_count,
                                                                                &unresolved_constraint);
                (void)unresolved_constraint;
                Type* check_constraint = concrete_constraint ? concrete_constraint : constraint;
                if (!typechecker_types_assignable(tc,
                                                  check_constraint,
                                                  bound,
                                                  expr->file,
                                                  expr->line,
                                                  expr->column)) {
                    char constraint_buf[128];
                    char bound_buf[128];
                    type_to_string(check_constraint, constraint_buf, sizeof(constraint_buf));
                    type_to_string(bound, bound_buf, sizeof(bound_buf));
                    char msg[320];
                    snprintf(msg,
                             sizeof(msg),
                             "Generic type argument for '%s' does not satisfy constraint: expected %s, got %s",
                             callee_type->type_param_names[i] ? callee_type->type_param_names[i] : "T",
                             constraint_buf,
                             bound_buf);
                    typechecker_error(tc, msg, expr->file, expr->line, expr->column);
                }
                if (concrete_constraint) {
                    type_free(concrete_constraint);
                }
            }
        }
    }

    if (expr->call.callee && expr->call.callee->kind == EXPR_IDENTIFIER &&
        strcmp(expr->call.callee->identifier, "arrayWithSize") == 0 &&
        arg1_type) {
        if (generic_bound_types) {
            for (int i = 0; i < callee_type->type_param_count; i++) {
                if (generic_bound_types[i]) type_free(generic_bound_types[i]);
            }
            free(generic_bound_types);
        }
        expr->type = type_result_tuple(tc, type_array(type_clone(arg1_type)));
        return expr->type;
    }

    if (expr->call.callee && expr->call.callee->kind == EXPR_IDENTIFIER &&
        strcmp(expr->call.callee->identifier, "slice") == 0 &&
        arg0_type) {
        if (expr->call.arg_count != 3) {
            if (generic_bound_types) {
                for (int i = 0; i < callee_type->type_param_count; i++) {
                    if (generic_bound_types[i]) type_free(generic_bound_types[i]);
                }
                free(generic_bound_types);
            }
            expr->type = type_any();
            return expr->type;
        }

        if (arg0_type->kind == TYPE_ARRAY) {
            Type* elem_type = arg0_type->element_type ? type_clone(arg0_type->element_type) : type_any();
            if (generic_bound_types) {
                for (int i = 0; i < callee_type->type_param_count; i++) {
                    if (generic_bound_types[i]) type_free(generic_bound_types[i]);
                }
                free(generic_bound_types);
            }
            expr->type = type_array(elem_type);
            return expr->type;
        }

        if (arg0_type->kind == TYPE_BYTES) {
            if (generic_bound_types) {
                for (int i = 0; i < callee_type->type_param_count; i++) {
                    if (generic_bound_types[i]) type_free(generic_bound_types[i]);
                }
                free(generic_bound_types);
            }
            expr->type = type_bytes();
            return expr->type;
        }

        if (arg0_type->kind != TYPE_ANY) {
            typechecker_error(tc, "slice expects array/bytes", expr->file, expr->line, 0);
        }

        if (generic_bound_types) {
            for (int i = 0; i < callee_type->type_param_count; i++) {
                if (generic_bound_types[i]) type_free(generic_bound_types[i]);
            }
            free(generic_bound_types);
        }
        expr->type = type_any();
        return expr->type;
    }

    if (is_generic_function_call) {
        bool unresolved = false;
        expr->type = typechecker_substitute_generic_type(callee_type->return_type,
                                                         callee_type->type_param_names,
                                                         generic_bound_types,
                                                         callee_type->type_param_count,
                                                         &unresolved);
        expr->type = typechecker_resolve_type(tc, expr->type);
        if (unresolved) {
            typechecker_error(tc, "Could not fully infer generic return type", expr->file, expr->line, expr->column);
        }
    } else {
        expr->type = callee_type->return_type ? type_clone(callee_type->return_type) : type_any();
    }

    if (generic_bound_types) {
        for (int i = 0; i < callee_type->type_param_count; i++) {
            if (generic_bound_types[i]) type_free(generic_bound_types[i]);
        }
        free(generic_bound_types);
    }
    return expr->type;
}

static Type* typecheck_func_literal(TypeChecker* tc, Expr* expr) {
    if (!expr) return type_any();

    if (expr->func_literal.return_type) {
        expr->func_literal.return_type = typechecker_resolve_type(tc, expr->func_literal.return_type);
    } else {
        expr->func_literal.return_type = type_void();
    }

    for (int i = 0; i < expr->func_literal.param_count; i++) {
        if (expr->func_literal.param_types[i]) {
            expr->func_literal.param_types[i] = typechecker_resolve_type(tc, expr->func_literal.param_types[i]);
        } else {
            expr->func_literal.param_types[i] = type_any();
        }
    }

    char* old_func = tc->current_function;
    bool old_function_is_async = tc->current_function_is_async;
    Type* old_return_type = tc->current_return_type;
    int old_lookup_floor = tc->local_lookup_floor;
    int outer_local_count = tc->local_count;

    tc->current_function = safe_strdup("<anonymous>");
    tc->current_function_is_async = expr->func_literal.is_async;
    tc->current_return_type = expr->func_literal.return_type;
    tc->local_lookup_floor = old_lookup_floor;
    typechecker_capture_context_push(tc, expr, outer_local_count);

    typechecker_push_scope(tc);
    for (int i = 0; i < expr->func_literal.param_count; i++) {
        Symbol* param = symbol_create(type_clone(expr->func_literal.param_types[i]), expr->func_literal.params[i], false);
        if (!typechecker_declare(tc, param)) {
            symbol_free(param);
            typechecker_error(tc, "Parameter already declared", expr->file, expr->line, expr->column);
        }
    }

    if (expr->func_literal.body) {
        if (expr->func_literal.body->kind == STMT_BLOCK) {
            for (int i = 0; i < expr->func_literal.body->block.stmt_count; i++) {
                typecheck_statement(tc, expr->func_literal.body->block.statements[i]);
            }
        } else {
            typecheck_statement(tc, expr->func_literal.body);
        }
    }
    typechecker_pop_scope(tc);

    FuncLiteralCaptureContext capture_ctx = typechecker_capture_context_pop(tc);
    for (int i = 0; i < expr->func_literal.capture_count; i++) {
        if (expr->func_literal.capture_names && expr->func_literal.capture_names[i]) {
            free(expr->func_literal.capture_names[i]);
        }
    }
    if (expr->func_literal.capture_names) free(expr->func_literal.capture_names);
    expr->func_literal.capture_names = capture_ctx.names;
    expr->func_literal.capture_count = capture_ctx.name_count;
    capture_ctx.names = NULL;
    capture_ctx.name_count = 0;
    capture_ctx.name_capacity = 0;
    typechecker_capture_context_free(&capture_ctx);

    if (tc->current_function) free(tc->current_function);
    tc->current_function = old_func;
    tc->current_function_is_async = old_function_is_async;
    tc->current_return_type = old_return_type;
    tc->local_lookup_floor = old_lookup_floor;

    Type* return_type = type_clone(expr->func_literal.return_type);
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

    if (expr->type) type_free(expr->type);
    expr->type = type_function(return_type, param_types, expr->func_literal.param_count);
    return expr->type;
}

static Type* typecheck_await(TypeChecker* tc, Expr* expr) {
    Type* operand_type = typecheck_expression(tc, expr->await_expr.expr);

    if (!tc->current_function_is_async) {
        typechecker_error(tc, "await is only allowed inside async functions", expr->file, expr->line, expr->column);
    }

    if (expr->type) type_free(expr->type);

    if (!operand_type || operand_type->kind == TYPE_ANY) {
        expr->type = type_any();
        return expr->type;
    }

    if (operand_type->kind != TYPE_FUTURE) {
        Type* expected = type_future(type_any());
        typechecker_error_expected_got(tc,
                                       "await expects Future<T>",
                                       expected,
                                       operand_type,
                                       expr->file,
                                       expr->line,
                                       expr->column);
        type_free(expected);
        expr->type = type_any();
        return expr->type;
    }

    expr->type = operand_type->element_type ? type_clone(operand_type->element_type) : type_any();
    return expr->type;
}

static Type* typecheck_index(TypeChecker* tc, Expr* expr) {
    Type* array_type = typecheck_expression(tc, expr->index.array);
    Type* index_type = typecheck_expression(tc, expr->index.index);
    
    if (array_type->kind != TYPE_ARRAY && array_type->kind != TYPE_BYTES) {
        typechecker_error(tc, "Can only index arrays/bytes", expr->file, expr->line, 0);
    }
    
    if (index_type->kind != TYPE_INT) {
        typechecker_error(tc, "Index must be integer", expr->file, expr->line, 0);
    }
    
    if (expr->type) type_free(expr->type);
    if (array_type->kind == TYPE_BYTES) {
        expr->type = type_int();
    } else {
        expr->type = array_type->element_type;
    }
    return expr->type;
}

static Type* typecheck_array_literal(TypeChecker* tc, Expr* expr) {
    Type* element_type = NULL;
    
    for (int i = 0; i < expr->array_literal.element_count; i++) {
        Type* elem = typecheck_expression(tc, expr->array_literal.elements[i]);
        if (!element_type) {
            element_type = elem;
        } else if (!type_equals(element_type, elem)) {
            element_type = type_any();
        }
    }
    
    if (!element_type) {
        element_type = type_any();
    }
    
    if (expr->type) type_free(expr->type);
    expr->type = type_array(element_type);
    return expr->type;
}

static Type* typecheck_cast(TypeChecker* tc, Expr* expr) {
    typecheck_expression(tc, expr->cast.value);

    Type* previous_expr_type = expr->type;
    Type* previous_target_type = expr->cast.target_type;

    if (expr->cast.target_type) {
        expr->cast.target_type = typechecker_resolve_type(tc, expr->cast.target_type);
    }

    // Cast expressions are created with expr->type pointing at cast.target_type.
    // Resolving interface/alias targets can replace and free that original type.
    if (previous_expr_type &&
        previous_expr_type != previous_target_type &&
        previous_expr_type != expr->cast.target_type) {
        type_free(previous_expr_type);
    }
    expr->type = expr->cast.target_type ? expr->cast.target_type : type_any();
    return expr->type;
}

static Type* typecheck_type_test(TypeChecker* tc, Expr* expr) {
    typecheck_expression(tc, expr->type_test.value);

    if (expr->type_test.target_type) {
        expr->type_test.target_type = typechecker_resolve_type(tc, expr->type_test.target_type);
    }

    Type* target_type = expr->type_test.target_type;
    if (!target_type) {
        typechecker_error(tc, "switch type case requires a target type", expr->file, expr->line, expr->column);
    } else if (target_type->nullable) {
        typechecker_error(tc,
                          "switch type cases do not support nullable target types",
                          expr->file,
                          expr->line,
                          expr->column);
    } else if (typechecker_type_contains_type_param(target_type)) {
        typechecker_error(tc,
                          "switch type cases require fully resolved target types",
                          expr->file,
                          expr->line,
                          expr->column);
    } else {
        switch (target_type->kind) {
            case TYPE_INT:
            case TYPE_BOOL:
            case TYPE_DOUBLE:
            case TYPE_BIGINT:
            case TYPE_STRING:
            case TYPE_BYTES:
            case TYPE_NIL:
                break;
            case TYPE_RECORD:
                if (!target_type->record_def || !target_type->record_def->name ||
                    target_type->record_def->name[0] == '\0') {
                    typechecker_error(tc,
                                      "switch type cases require a concrete record type name",
                                      expr->file,
                                      expr->line,
                                      expr->column);
                }
                break;
            case TYPE_INTERFACE:
                if (!target_type->interface_def || !target_type->interface_def->name ||
                    target_type->interface_def->name[0] == '\0') {
                    typechecker_error(tc,
                                      "switch type cases require a concrete interface name",
                                      expr->file,
                                      expr->line,
                                      expr->column);
                }
                break;
            default:
                typechecker_error(tc,
                                  "switch type cases currently support only primitive, nil, record, and interface types",
                                  expr->file,
                                  expr->line,
                                  expr->column);
                break;
        }
    }

    if (expr->type) type_free(expr->type);
    expr->type = type_bool();
    return expr->type;
}

static Type* typechecker_current_function_return_type(TypeChecker* tc) {
    if (!tc) return NULL;
    if (tc->current_return_type) return tc->current_return_type;
    if (!tc->globals || !tc->current_function) return NULL;

    for (int i = 0; i < tc->globals->symbol_count; i++) {
        Symbol* sym = tc->globals->symbols[i];
        if (!sym || !sym->name) continue;
        if (strcmp(sym->name, tc->current_function) != 0) continue;
        if (!sym->type || sym->type->kind != TYPE_FUNCTION) return NULL;
        return sym->type->return_type;
    }

    return NULL;
}

static Type* typecheck_try(TypeChecker* tc, Expr* expr) {
    Type* operand_type = typecheck_expression(tc, expr->try_expr.expr);

    if (operand_type->kind != TYPE_TUPLE || tuple_type_get_arity(operand_type) != 2) {
        typechecker_error(tc, "? expects (value, Error?) tuple", expr->file, expr->line, 0);
        if (expr->type) type_free(expr->type);
        expr->type = type_any();
        return expr->type;
    }

    Type* err_elem = tuple_type_get_element(operand_type, 1);
    Type* expected_err = type_error_nullable(tc);
    if (err_elem && err_elem->kind == TYPE_ANY) {
        typechecker_error(tc, "? expects (value, Error?) tuple", expr->file, expr->line, 0);
    } else if (expected_err && err_elem && !type_assignable(expected_err, err_elem)) {
        typechecker_error(tc, "? expects (value, Error?) tuple", expr->file, expr->line, 0);
    }

    if (!tc->current_return_type) {
        typechecker_error(tc, "? used outside of a function", expr->file, expr->line, 0);
        if (expected_err) type_free(expected_err);
        if (expr->type) type_free(expr->type);
        expr->type = type_any();
        return expr->type;
    }

    Type* return_type = typechecker_current_function_return_type(tc);
    if (!return_type || return_type->kind != TYPE_TUPLE || tuple_type_get_arity(return_type) != 2) {
        typechecker_error(tc, "? requires a function returning (value, Error?)", expr->file, expr->line, 0);
        if (expected_err) type_free(expected_err);
        if (expr->type) type_free(expr->type);
        expr->type = type_any();
        return expr->type;
    }

    Type* fn_err_elem = tuple_type_get_element(return_type, 1);
    if (fn_err_elem && fn_err_elem->kind == TYPE_ANY) {
        typechecker_error(tc, "? requires a function returning (value, Error?)", expr->file, expr->line, 0);
    } else if (expected_err && fn_err_elem && !type_assignable(expected_err, fn_err_elem)) {
        typechecker_error(tc, "? requires a function returning (value, Error?)", expr->file, expr->line, 0);
    }

    if (return_type &&
        !typechecker_types_assignable(tc, return_type, operand_type, expr->file, expr->line, expr->column)) {
        typechecker_error(tc, "? operand type must match function return type", expr->file, expr->line, 0);
    }

    if (expected_err) type_free(expected_err);

    Type* ok_elem = tuple_type_get_element(operand_type, 0);
    if (expr->type) type_free(expr->type);
    expr->type = ok_elem ? type_clone(ok_elem) : type_any();
    return expr->type;
}

static Type* typecheck_record_literal(TypeChecker* tc, Expr* expr) {
    bool is_pattern = expr_is_record_pattern(expr);
    if (expr->record_literal.pattern_type) {
        expr->record_literal.pattern_type =
            typechecker_resolve_type(tc, expr->record_literal.pattern_type);
        if (expr->record_literal.pattern_type &&
            expr->record_literal.pattern_type->kind == TYPE_RECORD) {
            expr->record_literal.record_type = expr->record_literal.pattern_type;
        } else if (is_pattern) {
            typechecker_error(tc,
                              "Typed record pattern requires a record type",
                              expr->file,
                              expr->line,
                              expr->column);
        }
    }

    if (expr->record_literal.record_type && expr->record_literal.record_type->kind == TYPE_RECORD) {
        Type* record_type = expr->record_literal.record_type;
        RecordDef* def = record_type->record_def;
        if (def && def->is_native_opaque) {
            typechecker_error(tc,
                              is_pattern
                                  ? "Opaque native handle types cannot be pattern-matched directly"
                                  : "Opaque native handle types cannot be constructed directly",
                              expr->file,
                              expr->line,
                              expr->column);
            if (expr->type) type_free(expr->type);
            expr->type = type_clone(record_type);
            return expr->type;
        }
        bool* fields_present = NULL;
        int def_field_count = def ? def->field_count : 0;
        if (def_field_count > 0) {
            fields_present = (bool*)safe_calloc((size_t)def_field_count, sizeof(bool));
        }

        for (int i = 0; i < expr->record_literal.field_count; i++) {
            const char* field_name = expr->record_literal.field_names[i];
            Expr* value_expr = expr->record_literal.field_values[i];

            if (def) {
                int field_index = record_def_get_field_index(def, field_name);
                if (field_index < 0) {
                    typechecker_error(tc,
                                      is_pattern
                                          ? "Unknown field in record pattern"
                                          : "Unknown field in record literal",
                                      expr->file,
                                      expr->line,
                                      0);
                    continue;
                }
                if (fields_present && field_index < def_field_count) {
                    fields_present[field_index] = true;
                }
                Type* field_type = def->fields[field_index].type;
                if (field_type && value_expr) {
                    if (field_type->kind == TYPE_RECORD && value_expr->kind == EXPR_RECORD_LITERAL) {
                        value_expr->record_literal.record_type = field_type;
                    }
                    if (field_type->kind == TYPE_ARRAY && field_type->element_type &&
                        field_type->element_type->kind == TYPE_RECORD &&
                        value_expr->kind == EXPR_ARRAY_LITERAL) {
                        for (int j = 0; j < value_expr->array_literal.element_count; j++) {
                            Expr* elem = value_expr->array_literal.elements[j];
                            if (elem && elem->kind == EXPR_RECORD_LITERAL) {
                                elem->record_literal.record_type = field_type->element_type;
                            }
                        }
                    }
                }
                Type* value_type = typecheck_expression(tc, value_expr);
                if (field_type &&
                    !typechecker_types_assignable(tc, field_type, value_type, expr->file, expr->line, expr->column)) {
                    typechecker_error_expected_got(tc,
                                                   "Record field type mismatch",
                                                   field_type,
                                                   value_type,
                                                   expr->file,
                                                   expr->line,
                                                   0);
                }
            } else {
                typecheck_expression(tc, value_expr);
            }
        }

        if (fields_present) {
            int missing_count = 0;
            size_t missing_len = 0;
            for (int i = 0; i < def_field_count; i++) {
                if (!fields_present[i]) {
                    const char* name = def->fields[i].name ? def->fields[i].name : "<unknown>";
                    missing_len += strlen(name) + 2;
                    missing_count++;
                }
            }
            if (missing_count > 0 && !(is_pattern && expr->record_literal.allows_rest)) {
                const char* prefix = is_pattern
                    ? "Missing fields in record pattern: "
                    : "Missing fields in record literal: ";
                size_t prefix_len = strlen(prefix);
                size_t msg_len = prefix_len + missing_len + 1;
                char* msg = (char*)safe_malloc(msg_len);
                memcpy(msg, prefix, prefix_len);
                msg[prefix_len] = '\0';
                size_t pos = prefix_len;
                int appended = 0;
                for (int i = 0; i < def_field_count; i++) {
                    if (!fields_present[i]) {
                        const char* name = def->fields[i].name ? def->fields[i].name : "<unknown>";
                        if (appended > 0) {
                            msg[pos++] = ',';
                            msg[pos++] = ' ';
                        }
                        size_t name_len = strlen(name);
                        memcpy(msg + pos, name, name_len);
                        pos += name_len;
                        appended++;
                    }
                }
                msg[pos] = '\0';
                typechecker_error(tc, msg, expr->file, expr->line, 0);
                free(msg);
            }
            free(fields_present);
        }

        expr->type = record_type;
        return expr->type;
    }

    // For now, create a generic record type
    // In a full implementation, we'd match against a declared record type
    if (is_pattern) {
        typechecker_error(tc,
                          "Record pattern requires a concrete record type",
                          expr->file,
                          expr->line,
                          expr->column);
    }
    Type* record_type = type_record(NULL);
    
    for (int i = 0; i < expr->record_literal.field_count; i++) {
        Type* field_type = typecheck_expression(tc, expr->record_literal.field_values[i]);
        if (record_type->record_def) {
            record_def_add_field(record_type->record_def, expr->record_literal.field_names[i], field_type);
        }
    }
    
    expr->type = record_type;
    return expr->type;
}

static Type* typecheck_field_access(TypeChecker* tc, Expr* expr) {
    if (expr->field_access.object &&
        expr->field_access.object->kind == EXPR_IDENTIFIER &&
        expr->field_access.object->identifier) {
        Symbol* enum_sym = typechecker_lookup(tc, expr->field_access.object->identifier);
        if (enum_sym && enum_sym->is_type_alias) {
            if (!symbol_is_accessible_from_file(enum_sym, expr->file)) {
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "Symbol '%s' is private to its module", expr->field_access.object->identifier);
                typechecker_error(tc, error_msg, expr->file, expr->line, expr->column);
                if (expr->type) type_free(expr->type);
                expr->type = type_any();
                return expr->type;
            }

            char* member_symbol = enum_member_symbol_name(expr->field_access.object->identifier,
                                                          expr->field_access.field_name);
            Symbol* member_sym = member_symbol ? typechecker_lookup(tc, member_symbol) : NULL;
            if (!member_sym) {
                char error_msg[512];
                snprintf(error_msg,
                         sizeof(error_msg),
                         "Unknown enum member '%s.%s'",
                         expr->field_access.object->identifier,
                         expr->field_access.field_name ? expr->field_access.field_name : "");
                typechecker_error(tc, error_msg, expr->file, expr->line, expr->column);
                if (member_symbol) free(member_symbol);
                if (expr->type) type_free(expr->type);
                expr->type = type_any();
                return expr->type;
            }

            if (!symbol_is_accessible_from_file(member_sym, expr->file)) {
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "Symbol '%s' is private to its module", member_symbol);
                typechecker_error(tc, error_msg, expr->file, expr->line, expr->column);
                free(member_symbol);
                if (expr->type) type_free(expr->type);
                expr->type = type_any();
                return expr->type;
            }

            expr_free(expr->field_access.object);
            expr->field_access.object = NULL;
            if (expr->field_access.field_name) {
                free(expr->field_access.field_name);
                expr->field_access.field_name = NULL;
            }

            expr->kind = EXPR_IDENTIFIER;
            expr->identifier = safe_strdup(member_symbol);
            if (expr->type) type_free(expr->type);
            expr->type = member_sym->type ? type_clone(member_sym->type) : type_any();
            free(member_symbol);
            return expr->type;
        }
    }

    Type* object_type = typecheck_expression(tc, expr->field_access.object);
    
    if (object_type->kind != TYPE_RECORD) {
        typechecker_error(tc, "Can only access fields on record types", expr->file, expr->line, 0);
        expr->type = type_any();
        return expr->type;
    }
    
    if (object_type->record_def) {
        int field_index = record_def_get_field_index(object_type->record_def, expr->field_access.field_name);
        expr->field_access.field_index = field_index;
        if (field_index >= 0) {
            Type* field_type = record_def_get_field(object_type->record_def, field_index)->type;
            expr->type = field_type ? type_clone(field_type) : type_any();
        } else {
            typechecker_error(tc, "Unknown field", expr->file, expr->line, 0);
            expr->type = type_any();
        }
    } else {
        expr->type = type_any();
    }
    
    return expr->type;
}

static Type* typecheck_tuple_literal(TypeChecker* tc, Expr* expr) {
    Type** element_types = NULL;
    int element_count = expr->tuple_literal.element_count;
    
    if (element_count > 0) {
        element_types = (Type**)safe_malloc(element_count * sizeof(Type*));
        for (int i = 0; i < element_count; i++) {
            element_types[i] = typecheck_expression(tc, expr->tuple_literal.elements[i]);
        }
    }
    
    expr->type = type_tuple(element_types, element_count);
    return expr->type;
}

static Type* typecheck_tuple_access(TypeChecker* tc, Expr* expr) {
    Type* tuple_type = typecheck_expression(tc, expr->tuple_access.tuple);
    
    if (tuple_type->kind != TYPE_TUPLE) {
        typechecker_error(tc, "Can only access elements on tuple types", expr->file, expr->line, 0);
        expr->type = type_any();
        return expr->type;
    }
    
    int index = expr->tuple_access.index;
    if (index < 0 || index >= tuple_type_get_arity(tuple_type)) {
        typechecker_error(tc, "Tuple index out of bounds", expr->file, expr->line, 0);
        expr->type = type_any();
        return expr->type;
    }
    
    Type* element_type = tuple_type_get_element(tuple_type, index);
    if (element_type) {
        expr->type = type_clone(element_type);
    } else {
        expr->type = type_any();
    }
    
    return expr->type;
}

static Type* typecheck_map_literal(TypeChecker* tc, Expr* expr) {
    Type* key_type = NULL;
    Type* value_type = NULL;
    
    for (int i = 0; i < expr->map_literal.entry_count; i++) {
        Type* kt = typecheck_expression(tc, expr->map_literal.keys[i]);
        Type* vt = typecheck_expression(tc, expr->map_literal.values[i]);
        
        if (!key_type) {
            key_type = kt;
        } else if (!type_equals(key_type, kt)) {
            key_type = type_any();
        }
        
        if (!value_type) {
            value_type = vt;
        } else if (!type_equals(value_type, vt)) {
            value_type = type_any();
        }
    }
    
    if (!key_type) key_type = type_any();
    if (!value_type) value_type = type_any();
    
    // Validate key type (only int and string are supported)
    if (key_type->kind != TYPE_INT && key_type->kind != TYPE_STRING && key_type->kind != TYPE_ANY) {
        typechecker_error(tc, "Map keys must be int or string", expr->file, expr->line, 0);
        key_type = type_any();
    }
    
    expr->type = type_map(key_type, value_type);
    return expr->type;
}

static Type* typecheck_set_literal(TypeChecker* tc, Expr* expr) {
    Type* element_type = NULL;
    
    for (int i = 0; i < expr->set_literal.element_count; i++) {
        Type* et = typecheck_expression(tc, expr->set_literal.elements[i]);
        if (!element_type) {
            element_type = et;
        } else if (!type_equals(element_type, et)) {
            element_type = type_any();
        }
    }
    
    if (!element_type) element_type = type_any();
    
    // Validate element type (only int and string are supported)
    if (element_type->kind != TYPE_INT && element_type->kind != TYPE_STRING && element_type->kind != TYPE_ANY) {
        typechecker_error(tc, "Set elements must be int or string", expr->file, expr->line, 0);
        element_type = type_any();
    }
    
    expr->type = type_set(element_type);
    return expr->type;
}

static Type* typecheck_block_expression(TypeChecker* tc, Expr* expr) {
    typechecker_push_scope(tc);
    for (int i = 0; i < expr->block_expr.stmt_count; i++) {
        typecheck_statement(tc, expr->block_expr.statements[i]);
    }
    Type* value_type = typecheck_expression_with_expected(tc,
                                                          expr->block_expr.value,
                                                          tc ? tc->expected_expr_type : NULL);
    typechecker_pop_scope(tc);
    expr->type = value_type ? value_type : type_any();
    return expr->type;
}

static Type* typecheck_if_expression(TypeChecker* tc, Expr* expr) {
    Type* cond_type = typecheck_expression(tc, expr->if_expr.condition);
    if (cond_type->kind != TYPE_BOOL && cond_type->kind != TYPE_ANY) {
        typechecker_error(tc,
                          "if expression condition must be bool",
                          expr->if_expr.condition && expr->if_expr.condition->file ? expr->if_expr.condition->file : expr->file,
                          expr->if_expr.condition ? expr->if_expr.condition->line : expr->line,
                          expr->if_expr.condition ? expr->if_expr.condition->column : expr->column);
    }

    Type* result_type = NULL;
    Type* branch_expected = tc ? tc->expected_expr_type : NULL;

    Type* then_type = typecheck_expression_with_expected(tc, expr->if_expr.then_expr, branch_expected);
    if (then_type && then_type->kind == TYPE_VOID) {
        typechecker_error(tc,
                          "if expression branches must produce a value",
                          expr->if_expr.then_expr && expr->if_expr.then_expr->file ? expr->if_expr.then_expr->file : expr->file,
                          expr->if_expr.then_expr ? expr->if_expr.then_expr->line : expr->line,
                          expr->if_expr.then_expr ? expr->if_expr.then_expr->column : expr->column);
    }
    result_type = if_merge_result_type(tc,
                                       result_type,
                                       then_type,
                                       expr->if_expr.then_expr && expr->if_expr.then_expr->file ? expr->if_expr.then_expr->file : expr->file,
                                       expr->if_expr.then_expr ? expr->if_expr.then_expr->line : expr->line,
                                       expr->if_expr.then_expr ? expr->if_expr.then_expr->column : expr->column);

    Type* else_type = typecheck_expression_with_expected(tc, expr->if_expr.else_expr, branch_expected);
    if (else_type && else_type->kind == TYPE_VOID) {
        typechecker_error(tc,
                          "if expression branches must produce a value",
                          expr->if_expr.else_expr && expr->if_expr.else_expr->file ? expr->if_expr.else_expr->file : expr->file,
                          expr->if_expr.else_expr ? expr->if_expr.else_expr->line : expr->line,
                          expr->if_expr.else_expr ? expr->if_expr.else_expr->column : expr->column);
    }
    result_type = if_merge_result_type(tc,
                                       result_type,
                                       else_type,
                                       expr->if_expr.else_expr && expr->if_expr.else_expr->file ? expr->if_expr.else_expr->file : expr->file,
                                       expr->if_expr.else_expr ? expr->if_expr.else_expr->line : expr->line,
                                       expr->if_expr.else_expr ? expr->if_expr.else_expr->column : expr->column);

    if (!result_type) {
        result_type = type_any();
    }
    expr->type = result_type;
    return expr->type;
}

static Type* typecheck_expression(TypeChecker* tc, Expr* expr) {
    switch (expr->kind) {
        case EXPR_LITERAL:
            return typecheck_literal(tc, expr);
        case EXPR_IDENTIFIER:
            return typecheck_identifier(tc, expr);
        case EXPR_BINARY:
            return typecheck_binary(tc, expr);
        case EXPR_UNARY:
            return typecheck_unary(tc, expr);
        case EXPR_CALL:
            return typecheck_call(tc, expr);
        case EXPR_FUNC_LITERAL:
            return typecheck_func_literal(tc, expr);
        case EXPR_INDEX:
            return typecheck_index(tc, expr);
        case EXPR_ARRAY_LITERAL:
            return typecheck_array_literal(tc, expr);
        case EXPR_NIL:
            return typecheck_literal(tc, expr);
        case EXPR_CAST:
            return typecheck_cast(tc, expr);
        case EXPR_TRY:
            return typecheck_try(tc, expr);
        case EXPR_AWAIT:
            return typecheck_await(tc, expr);
        case EXPR_TYPE_TEST:
            return typecheck_type_test(tc, expr);
        case EXPR_IF:
            return typecheck_if_expression(tc, expr);
        case EXPR_MATCH:
            return typecheck_match_expression(tc, expr);
        case EXPR_BLOCK:
            return typecheck_block_expression(tc, expr);
        case EXPR_RECORD_LITERAL:
            return typecheck_record_literal(tc, expr);
        case EXPR_FIELD_ACCESS:
            return typecheck_field_access(tc, expr);
        case EXPR_TUPLE_LITERAL:
            return typecheck_tuple_literal(tc, expr);
        case EXPR_TUPLE_ACCESS:
            return typecheck_tuple_access(tc, expr);
        case EXPR_MAP_LITERAL:
            return typecheck_map_literal(tc, expr);
        case EXPR_SET_LITERAL:
            return typecheck_set_literal(tc, expr);
        default:
            return type_any();
    }
}

static void typecheck_var_decl(TypeChecker* tc, Stmt* stmt) {
    if (is_builtin_name(stmt->var_decl.name)) {
        typechecker_error(tc, "Cannot declare a variable with a built-in name", stmt->file, stmt->line, 0);
        if (stmt->var_decl.initializer) {
            typecheck_expression_with_expected(tc, stmt->var_decl.initializer, stmt->var_decl.type_annotation);
        }
        return;
    }

    if (stmt->var_decl.type_annotation) {
        stmt->var_decl.type_annotation = typechecker_resolve_type(tc, stmt->var_decl.type_annotation);
    }

    if (stmt->var_decl.initializer && stmt->var_decl.type_annotation) {
        Type* annotated = stmt->var_decl.type_annotation;
        if (annotated->kind == TYPE_RECORD && stmt->var_decl.initializer->kind == EXPR_RECORD_LITERAL) {
            stmt->var_decl.initializer->record_literal.record_type = annotated;
        }
        if (annotated->kind == TYPE_ARRAY && annotated->element_type &&
            annotated->element_type->kind == TYPE_RECORD &&
            stmt->var_decl.initializer->kind == EXPR_ARRAY_LITERAL) {
            for (int i = 0; i < stmt->var_decl.initializer->array_literal.element_count; i++) {
                Expr* elem = stmt->var_decl.initializer->array_literal.elements[i];
                if (elem && elem->kind == EXPR_RECORD_LITERAL) {
                    elem->record_literal.record_type = annotated->element_type;
                }
            }
        }
    }

    Type* init_type = NULL;
    if (stmt->var_decl.initializer) {
        if (stmt->var_decl.type_annotation &&
            stmt->var_decl.type_annotation->kind == TYPE_SET &&
            expr_is_empty_map_literal(stmt->var_decl.initializer)) {
            expr_coerce_empty_map_literal_to_empty_set_literal(stmt->var_decl.initializer);
        }
        init_type = typecheck_expression_with_expected(tc,
                                                       stmt->var_decl.initializer,
                                                       stmt->var_decl.type_annotation);
    }
    
    Type* var_type = stmt->var_decl.type_annotation;
    if (var_type) {
        if (!stmt->var_decl.is_mutable && !init_type) {
            typechecker_error(tc, "Const declarations require an initializer", stmt->file, stmt->line, 0);
        }
        if (!stmt->var_decl.is_mutable &&
            stmt->var_decl.initializer &&
            !expr_is_compile_time_constant(stmt->var_decl.initializer)) {
            typechecker_error(tc, "Const initializer must be a compile-time constant expression", stmt->file, stmt->line, 0);
        }
        if (!init_type && !var_type->nullable && var_type->kind != TYPE_ANY && var_type->kind != TYPE_NIL) {
            typechecker_error(tc, "Non-nullable variables require an initializer", stmt->file, stmt->line, 0);
        }
        if (init_type &&
            !typechecker_types_assignable(tc, var_type, init_type, stmt->file, stmt->line, stmt->column)) {
            typechecker_error_expected_got(tc,
                                           "Cannot assign initializer to variable type",
                                           var_type,
                                           init_type,
                                           stmt->file,
                                           stmt->line,
                                           0);
        }
    } else if (init_type) {
        var_type = init_type;
    } else {
        if (!stmt->var_decl.is_mutable) {
            typechecker_error(tc, "Const declarations require an initializer", stmt->file, stmt->line, 0);
        }
        var_type = type_any();
    }
    
    Symbol* sym = symbol_create(type_clone(var_type), stmt->var_decl.name, stmt->var_decl.is_mutable);
    symbol_set_visibility_metadata(sym,
                                   stmt->file,
                                   tc->local_count > 0 ? true : stmt->is_public);
    if (!typechecker_declare(tc, sym)) {
        symbol_free(sym);
        typechecker_error(tc, "Variable already declared", stmt->file, stmt->line, 0);
    }
}

static void typecheck_var_tuple_decl(TypeChecker* tc, Stmt* stmt) {
    if (!stmt) return;

    if (stmt->var_tuple_decl.type_annotation) {
        stmt->var_tuple_decl.type_annotation = typechecker_resolve_type(tc, stmt->var_tuple_decl.type_annotation);
    }

    Type* init_type = NULL;
    if (stmt->var_tuple_decl.initializer) {
        init_type = typecheck_expression_with_expected(tc,
                                                       stmt->var_tuple_decl.initializer,
                                                       stmt->var_tuple_decl.type_annotation);
    } else {
        typechecker_error(tc, "Tuple destructuring requires an initializer", stmt->file, stmt->line, 0);
        return;
    }

    Type* tuple_type = stmt->var_tuple_decl.type_annotation ? stmt->var_tuple_decl.type_annotation : init_type;
    if (!tuple_type || tuple_type->kind != TYPE_TUPLE) {
        typechecker_error(tc, "Tuple destructuring requires a tuple initializer", stmt->file, stmt->line, 0);
        return;
    }

    if (!stmt->var_tuple_decl.is_mutable &&
        !expr_is_compile_time_constant(stmt->var_tuple_decl.initializer)) {
        typechecker_error(tc, "Const tuple destructuring requires a compile-time constant initializer", stmt->file, stmt->line, 0);
    }

    int name_count = stmt->var_tuple_decl.name_count;
    if (name_count <= 0) {
        typechecker_error(tc, "Tuple destructuring requires at least one binding", stmt->file, stmt->line, 0);
        return;
    }

    if (tuple_type_get_arity(tuple_type) != name_count) {
        typechecker_error(tc, "Tuple arity does not match binding count", stmt->file, stmt->line, 0);
        return;
    }

    if (stmt->var_tuple_decl.type_annotation && init_type &&
        !typechecker_types_assignable(tc,
                                      stmt->var_tuple_decl.type_annotation,
                                      init_type,
                                      stmt->file,
                                      stmt->line,
                                      stmt->column)) {
        typechecker_error_expected_got(tc,
                                       "Cannot assign initializer to variable type",
                                       stmt->var_tuple_decl.type_annotation,
                                       init_type,
                                       stmt->file,
                                       stmt->line,
                                       0);
    }

    for (int i = 0; i < name_count; i++) {
        const char* name = stmt->var_tuple_decl.names ? stmt->var_tuple_decl.names[i] : NULL;
        if (!name) continue;
        if (strcmp(name, "_") == 0) continue;

        if (is_builtin_name(name)) {
            typechecker_error(tc, "Cannot declare a variable with a built-in name", stmt->file, stmt->line, 0);
            continue;
        }

        Type* elem_type = tuple_type_get_element(tuple_type, i);
        Type* binding_type = elem_type ? type_clone(elem_type) : type_any();

        Symbol* sym = symbol_create(binding_type, name, stmt->var_tuple_decl.is_mutable);
        symbol_set_visibility_metadata(sym,
                                       stmt->file,
                                       tc->local_count > 0 ? true : stmt->is_public);
        if (!typechecker_declare(tc, sym)) {
            symbol_free(sym);
            typechecker_error(tc, "Variable already declared", stmt->file, stmt->line, 0);
        }
    }
}

static void typecheck_type_alias_decl(TypeChecker* tc, Stmt* stmt) {
    if (!stmt) return;
    if (tc->local_count > 0) {
        typechecker_error(tc, "type alias must appear at top-level", stmt->file, stmt->line, 0);
        return;
    }

    if (!stmt->type_alias.name) {
        typechecker_error(tc, "Invalid type alias declaration", stmt->file, stmt->line, 0);
        return;
    }
    if (is_builtin_name(stmt->type_alias.name)) {
        typechecker_error(tc, "Cannot declare a type alias with a built-in name", stmt->file, stmt->line, 0);
        return;
    }

    char** old_type_param_names = tc->current_type_param_names;
    Type** old_type_param_constraints = tc->current_type_param_constraints;
    int old_type_param_count = tc->current_type_param_count;
    if (stmt->type_alias.type_param_count > 0) {
        tc->current_type_param_names = stmt->type_alias.type_params;
        tc->current_type_param_constraints = NULL;
        tc->current_type_param_count = stmt->type_alias.type_param_count;
    }

    stmt->type_alias.target_type = typechecker_resolve_type(tc, stmt->type_alias.target_type);
    if (!stmt->type_alias.target_type) {
        stmt->type_alias.target_type = type_any();
    }

    tc->current_type_param_names = old_type_param_names;
    tc->current_type_param_constraints = old_type_param_constraints;
    tc->current_type_param_count = old_type_param_count;

    if (stmt->type_alias.target_type->kind == TYPE_RECORD &&
        stmt->type_alias.target_type->record_def &&
        stmt->type_alias.target_type->record_def->name &&
        strcmp(stmt->type_alias.name, stmt->type_alias.target_type->record_def->name) == 0) {
        typechecker_error(tc, "Recursive type aliases are not supported", stmt->file, stmt->line, 0);
    }

    Symbol* sym = symbol_table_get(tc->globals, stmt->type_alias.name);
    if (sym) {
        if (!sym->is_type_alias) {
            typechecker_error(tc, "Type alias already declared", stmt->file, stmt->line, 0);
            return;
        }
        if (stmt->type_alias.type_param_count > 0) {
            if (sym->type) type_free(sym->type);
            sym->type = type_any();
        } else {
            if (sym->type) type_free(sym->type);
            sym->type = type_clone(stmt->type_alias.target_type);
        }
        symbol_set_visibility_metadata(sym, stmt->file, stmt->is_public);
        return;
    }

    Type* alias_type = stmt->type_alias.type_param_count > 0
        ? type_any()
        : type_clone(stmt->type_alias.target_type);
    Symbol* alias_sym = symbol_create(alias_type, stmt->type_alias.name, false);
    alias_sym->is_type_alias = true;
    symbol_set_visibility_metadata(alias_sym, stmt->file, stmt->is_public);
    if (!typechecker_declare(tc, alias_sym)) {
        symbol_free(alias_sym);
        typechecker_error(tc, "Type alias already declared", stmt->file, stmt->line, 0);
    }
}

static void typecheck_interface_decl(TypeChecker* tc, Stmt* stmt) {
    if (!stmt) return;
    if (tc->local_count > 0) {
        typechecker_error(tc, "interface must appear at top-level", stmt->file, stmt->line, 0);
        return;
    }

    if (!stmt->interface_decl.name) {
        typechecker_error(tc, "Invalid interface declaration", stmt->file, stmt->line, 0);
        return;
    }

    Symbol* sym = symbol_table_get(tc->globals, stmt->interface_decl.name);
    if (!sym || !sym->type || sym->type->kind != TYPE_INTERFACE || !sym->type->interface_def) {
        typechecker_error(tc, "Interface type already declared", stmt->file, stmt->line, 0);
        return;
    }

    InterfaceDef* def = sym->type->interface_def;
    if (def->method_count > 0) {
        typechecker_error(tc, "Interface type already declared", stmt->file, stmt->line, 0);
        return;
    }

    for (int i = 0; i < stmt->interface_decl.method_count; i++) {
        const char* method_name = stmt->interface_decl.method_names[i];
        Type* method_type = typechecker_resolve_type(tc, stmt->interface_decl.method_types[i]);
        stmt->interface_decl.method_types[i] = method_type;

        if (!method_type || method_type->kind != TYPE_FUNCTION) {
            typechecker_error(tc, "Interface methods must be function signatures", stmt->file, stmt->line, 0);
            continue;
        }

        if (interface_def_get_method_index(def, method_name) >= 0) {
            typechecker_error(tc, "Duplicate interface method", stmt->file, stmt->line, 0);
            continue;
        }

        interface_def_add_method(def, method_name, method_type);
    }
}

static void typecheck_impl_decl(TypeChecker* tc, Stmt* stmt) {
    if (!stmt) return;
    if (tc->local_count > 0) {
        typechecker_error(tc, "impl must appear at top-level", stmt->file, stmt->line, 0);
        return;
    }

    if (!stmt->impl_decl.interface_name || !stmt->impl_decl.record_name) {
        typechecker_error(tc, "Invalid impl declaration", stmt->file, stmt->line, 0);
        return;
    }

    Symbol* iface_sym = symbol_table_get(tc->globals, stmt->impl_decl.interface_name);
    if (!iface_sym || !iface_sym->type || iface_sym->type->kind != TYPE_INTERFACE || !iface_sym->type->interface_def) {
        typechecker_error(tc, "impl references unknown interface type", stmt->file, stmt->line, 0);
        return;
    }
    if (!symbol_is_accessible_from_file(iface_sym, stmt->file)) {
        char message[512];
        snprintf(message, sizeof(message), "Symbol '%s' is private to its module", stmt->impl_decl.interface_name);
        typechecker_error(tc, message, stmt->file, stmt->line, 0);
        return;
    }

    Symbol* record_sym = symbol_table_get(tc->globals, stmt->impl_decl.record_name);
    if (!record_sym || !record_sym->type || record_sym->type->kind != TYPE_RECORD || !record_sym->type->record_def) {
        typechecker_error(tc, "impl references unknown record type", stmt->file, stmt->line, 0);
        return;
    }
    if (!symbol_is_accessible_from_file(record_sym, stmt->file)) {
        char message[512];
        snprintf(message, sizeof(message), "Symbol '%s' is private to its module", stmt->impl_decl.record_name);
        typechecker_error(tc, message, stmt->file, stmt->line, 0);
        return;
    }

    InterfaceImplMapping* mapping = typechecker_find_impl_mapping_exact(tc,
                                                                        stmt->impl_decl.interface_name,
                                                                        stmt->impl_decl.record_name);
    if (!mapping) {
        typechecker_error(tc, "Invalid impl declaration", stmt->file, stmt->line, 0);
        return;
    }

    InterfaceDef* iface = iface_sym->type->interface_def;

    for (int i = 0; i < mapping->method_count; i++) {
        const char* method_name = mapping->method_names ? mapping->method_names[i] : NULL;
        if (!method_name) continue;
        if (interface_def_get_method_type(iface, method_name) == NULL) {
            char message[512];
            snprintf(message, sizeof(message), "impl maps unknown interface method '%s'", method_name);
            typechecker_error(tc, message, stmt->file, stmt->line, 0);
            return;
        }
    }

    for (int i = 0; i < iface->method_count; i++) {
        InterfaceMethod* method = interface_def_get_method(iface, i);
        if (!method || !method->name || !method->type) continue;

        const char* function_name = typechecker_impl_lookup_function(mapping, method->name);
        if (!function_name) {
            char message[512];
            snprintf(message, sizeof(message), "impl is missing mapping for interface method '%s'", method->name);
            typechecker_error(tc, message, stmt->file, stmt->line, 0);
            return;
        }

        Symbol* fn_sym = symbol_table_get(tc->globals, function_name);
        if (!fn_sym || !fn_sym->type || fn_sym->type->kind != TYPE_FUNCTION) {
            char message[512];
            snprintf(message, sizeof(message), "impl maps method '%s' to unknown function '%s'", method->name, function_name);
            typechecker_error(tc, message, stmt->file, stmt->line, 0);
            return;
        }
        if (!symbol_is_accessible_from_file(fn_sym, stmt->file)) {
            char message[512];
            snprintf(message, sizeof(message), "Symbol '%s' is private to its module", function_name);
            typechecker_error(tc, message, stmt->file, stmt->line, 0);
            return;
        }

        if (!typechecker_function_matches_interface_method(fn_sym->type, method->type, record_sym->type)) {
            char message[512];
            snprintf(message,
                     sizeof(message),
                     "impl function '%s' does not match interface method '%s' signature",
                     function_name,
                     method->name);
            typechecker_error(tc, message, stmt->file, stmt->line, 0);
            return;
        }
    }
}

static void typecheck_expr_stmt(TypeChecker* tc, Stmt* stmt) {
    Type* expr_type = typecheck_expression(tc, stmt->expr_stmt);
    if (tc->options.warn_unused_error && type_is_result_tuple(tc, expr_type)) {
        typechecker_warn(tc, "Unused (value, Error?) result; consider '?', must(), or tuple destructuring", stmt->file, stmt->line, stmt->column);
    }
}

static void typecheck_assign(TypeChecker* tc, Stmt* stmt) {
    Symbol* sym = typechecker_lookup(tc, stmt->assign.name);
    if (!sym) {
        typechecker_error(tc, "Undefined variable", stmt->file, stmt->line, 0);
        typecheck_expression_with_expected(tc, stmt->assign.value, NULL);
        return;
    }

    if (!symbol_is_accessible_from_file(sym, stmt->file)) {
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg), "Symbol '%s' is private to its module", stmt->assign.name);
        typechecker_error(tc, error_msg, stmt->file, stmt->line, 0);
        typecheck_expression_with_expected(tc, stmt->assign.value, sym->type);
        return;
    }
    
    if (!sym->is_mutable) {
        typechecker_error(tc, "Cannot assign to immutable variable", stmt->file, stmt->line, 0);
    }
    
    if (sym->type && sym->type->kind == TYPE_RECORD && stmt->assign.value &&
        stmt->assign.value->kind == EXPR_RECORD_LITERAL) {
        stmt->assign.value->record_literal.record_type = sym->type;
    }

    if (sym->type && sym->type->kind == TYPE_SET && stmt->assign.value &&
        expr_is_empty_map_literal(stmt->assign.value)) {
        expr_coerce_empty_map_literal_to_empty_set_literal(stmt->assign.value);
    }

    Type* value_type = typecheck_expression_with_expected(tc, stmt->assign.value, sym->type);
    if (!typechecker_types_assignable(tc, sym->type, value_type, stmt->file, stmt->line, stmt->column)) {
        typechecker_error_expected_got(tc,
                                       "Type mismatch in assignment",
                                       sym->type,
                                       value_type,
                                       stmt->file,
                                       stmt->line,
                                       0);
    }
}

static void typecheck_block(TypeChecker* tc, Stmt* stmt) {
    typechecker_push_scope(tc);
    for (int i = 0; i < stmt->block.stmt_count; i++) {
        typecheck_statement(tc, stmt->block.statements[i]);
    }
    typechecker_pop_scope(tc);
}

static void typecheck_if(TypeChecker* tc, Stmt* stmt) {
    Type* cond_type = typecheck_expression(tc, stmt->if_stmt.condition);
    if (cond_type->kind != TYPE_BOOL && cond_type->kind != TYPE_ANY) {
        typechecker_error(tc, "If condition must be bool", stmt->file, stmt->line, 0);
    }
    typecheck_statement(tc, stmt->if_stmt.then_branch);
    if (stmt->if_stmt.else_branch) {
        typecheck_statement(tc, stmt->if_stmt.else_branch);
    }
}

static void typecheck_match(TypeChecker* tc, Stmt* stmt) {
    match_stmt_clear_payload_bindings(stmt);
    if (stmt->match_stmt.arm_count > 0) {
        stmt->match_stmt.payload_binding_names =
            (char***)safe_calloc((size_t)stmt->match_stmt.arm_count, sizeof(char**));
        stmt->match_stmt.payload_binding_counts =
            (int*)safe_calloc((size_t)stmt->match_stmt.arm_count, sizeof(int));
    }
    MatchAlternationBindingGroup* alternation_groups = NULL;
    if (stmt->match_stmt.arm_group_ids && stmt->match_stmt.arm_count > 0) {
        alternation_groups = (MatchAlternationBindingGroup*)safe_calloc(
            (size_t)stmt->match_stmt.arm_count + 1,
            sizeof(MatchAlternationBindingGroup));
    }

    Type* subject_type = typecheck_expression(tc, stmt->match_stmt.subject);
    if (!type_is_match_comparable(subject_type)) {
        typechecker_error(tc,
                          "match subject must be int/bool/double/bigint/string/nil/tuple/record (or any)",
                          stmt->file,
                          stmt->line,
                          0);
    }

    const char* subject_enum = type_enum_name(subject_type);
    bool subject_is_bool = subject_type && subject_type->kind == TYPE_BOOL;
    bool subject_is_structural =
        subject_type &&
        (subject_type->kind == TYPE_TUPLE || subject_type->kind == TYPE_RECORD);
    bool has_true_pattern = false;
    bool has_false_pattern = false;
    MatchPatternConst* seen_pattern_consts = NULL;
    int seen_pattern_const_count = 0;
    int seen_pattern_const_capacity = 0;
    int enum_missing_count = -1;
    bool enum_patterns_exhaustive = false;
    bool has_guarded_arm = false;
    bool has_covering_arm = false;
    Expr** seen_all_patterns = NULL;
    int seen_all_pattern_count = 0;
    int seen_all_pattern_capacity = 0;
    Expr** seen_guarded_patterns = NULL;
    int seen_guarded_pattern_count = 0;
    int seen_guarded_pattern_capacity = 0;
    Expr** seen_unguarded_patterns = NULL;
    int seen_unguarded_pattern_count = 0;
    int seen_unguarded_pattern_capacity = 0;
    Expr** seen_enum_patterns = NULL;
    int seen_enum_pattern_count = 0;
    int seen_enum_pattern_capacity = 0;
    Expr** seen_structural_patterns = NULL;
    int seen_structural_pattern_count = 0;
    int seen_structural_pattern_capacity = 0;

    for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
        Expr* pattern_expr = stmt->match_stmt.patterns[i];
        Expr* guard_expr = stmt->match_stmt.guards ? stmt->match_stmt.guards[i] : NULL;
        Type* pattern_type = NULL;
        bool body_binding_scope_pushed = false;
        char** payload_bind_names = NULL;
        Type** payload_bind_types = NULL;
        int payload_bind_count = 0;
        bool has_payload_bindings = false;
        bool arm_is_covering = (guard_expr == NULL);
        bool pattern_needs_typed_subject =
            pattern_expr &&
            (pattern_expr->kind == EXPR_TUPLE_LITERAL || pattern_expr->kind == EXPR_RECORD_LITERAL);

        if (guard_expr) {
            has_guarded_arm = true;
        }

        if (pattern_expr && subject_type) {
            match_pattern_apply_context_types(tc, pattern_expr, subject_type, false);
        }
        if (subject_type && subject_type->kind == TYPE_ANY && pattern_needs_typed_subject) {
            typechecker_error(tc,
                              "Tuple and record patterns require a non-any subject type",
                              pattern_expr ? pattern_expr->file : stmt->file,
                              pattern_expr ? pattern_expr->line : stmt->line,
                              pattern_expr ? pattern_expr->column : stmt->column);
        }
        has_payload_bindings = match_pattern_collect_payload_bindings(tc,
                                                                      pattern_expr,
                                                                      subject_type,
                                                                      &payload_bind_names,
                                                                      &payload_bind_types,
                                                                      &payload_bind_count);
        int alternation_group_id =
            (stmt->match_stmt.arm_group_ids && i < stmt->match_stmt.arm_count)
                ? stmt->match_stmt.arm_group_ids[i]
                : 0;
        if (alternation_groups && alternation_group_id > 0) {
            MatchAlternationBindingGroup* group = &alternation_groups[alternation_group_id];
            if (!group->initialized) {
                match_alternation_binding_group_capture(group,
                                                        payload_bind_names,
                                                        payload_bind_types,
                                                        payload_bind_count);
            } else {
                match_validate_alternation_bindings(tc,
                                                    group,
                                                    payload_bind_names,
                                                    payload_bind_types,
                                                    payload_bind_count,
                                                    pattern_expr ? pattern_expr->file : stmt->file,
                                                    pattern_expr ? pattern_expr->line : stmt->line,
                                                    pattern_expr ? pattern_expr->column : stmt->column);
            }
        }

        enum_patterns_exhaustive =
            subject_enum &&
            seen_enum_pattern_count > 0 &&
            match_patterns_exhaustive(tc,
                                      seen_enum_patterns,
                                      seen_enum_pattern_count,
                                      subject_type);

        if (has_covering_arm) {
            typechecker_warn(tc,
                             "Unreachable match arm: previous patterns already cover all values",
                             pattern_expr && pattern_expr->file ? pattern_expr->file : stmt->file,
                             pattern_expr ? pattern_expr->line : stmt->line,
                             pattern_expr ? pattern_expr->column : stmt->column);
        } else if (subject_is_bool && has_true_pattern && has_false_pattern) {
            typechecker_warn(tc,
                             "Unreachable match arm: bool match already covers true and false",
                             pattern_expr && pattern_expr->file ? pattern_expr->file : stmt->file,
                             pattern_expr ? pattern_expr->line : stmt->line,
                             pattern_expr ? pattern_expr->column : stmt->column);
        }
        if (!has_covering_arm && subject_enum && enum_patterns_exhaustive) {
            char message[320];
            snprintf(message,
                     sizeof(message),
                     "Unreachable match arm: enum match already covers all members of '%s'",
                     subject_enum);
            typechecker_warn(tc,
                             message,
                             pattern_expr && pattern_expr->file ? pattern_expr->file : stmt->file,
                             pattern_expr ? pattern_expr->line : stmt->line,
                             pattern_expr ? pattern_expr->column : stmt->column);
        }

        if (has_payload_bindings) {
            for (int j = 0; j < payload_bind_count; j++) {
                const char* bind_name = payload_bind_names ? payload_bind_names[j] : NULL;
                if (!bind_name) continue;

                if (is_builtin_name(bind_name)) {
                    typechecker_error(tc,
                                      "Cannot declare a variable with a built-in name",
                                      pattern_expr ? pattern_expr->file : stmt->file,
                                      pattern_expr ? pattern_expr->line : stmt->line,
                                      pattern_expr ? pattern_expr->column : stmt->column);
                }

                for (int k = 0; k < j; k++) {
                    const char* prev = payload_bind_names ? payload_bind_names[k] : NULL;
                    if (prev && strcmp(prev, bind_name) == 0) {
                        typechecker_error(tc,
                                          "Duplicate enum payload binding name in match pattern",
                                          pattern_expr ? pattern_expr->file : stmt->file,
                                          pattern_expr ? pattern_expr->line : stmt->line,
                                          pattern_expr ? pattern_expr->column : stmt->column);
                        break;
                    }
                }
            }

            typechecker_push_scope(tc);
            bool discard_declared = false;
            for (int j = 0; j < payload_bind_count; j++) {
                const char* bind_name = payload_bind_names ? payload_bind_names[j] : NULL;
                Type* bind_type = (payload_bind_types && payload_bind_types[j])
                                     ? payload_bind_types[j]
                                     : NULL;

                if (!bind_name) {
                    if (!discard_declared) {
                        Symbol* discard_sym = symbol_create(type_any(), "_", false);
                        symbol_set_visibility_metadata(discard_sym, stmt->file, true);
                        if (!typechecker_declare(tc, discard_sym)) {
                            symbol_free(discard_sym);
                        }
                        discard_declared = true;
                    }
                    continue;
                }

                Type* bind_type_clone = bind_type ? type_clone(bind_type) : type_any();
                Symbol* bind_sym = symbol_create(bind_type_clone, bind_name, false);
                if (is_builtin_name(bind_name)) {
                    symbol_free(bind_sym);
                    continue;
                }
                symbol_set_visibility_metadata(bind_sym, stmt->file, true);
                if (!typechecker_declare(tc, bind_sym)) {
                    symbol_free(bind_sym);
                    typechecker_error(tc,
                                      "Variable already declared",
                                      pattern_expr ? pattern_expr->file : stmt->file,
                                      pattern_expr ? pattern_expr->line : stmt->line,
                                      pattern_expr ? pattern_expr->column : stmt->column);
                }
            }

            pattern_type = typecheck_expression_with_expected(tc, pattern_expr, subject_type);
            typechecker_pop_scope(tc);

            if (stmt->match_stmt.payload_binding_names &&
                stmt->match_stmt.payload_binding_counts &&
                i < stmt->match_stmt.arm_count) {
                stmt->match_stmt.payload_binding_names[i] = payload_bind_names;
                stmt->match_stmt.payload_binding_counts[i] = payload_bind_count;
                payload_bind_names = NULL;
            }

            int named_binding_count = 0;
            for (int j = 0; j < payload_bind_count; j++) {
                if (stmt->match_stmt.payload_binding_names &&
                    stmt->match_stmt.payload_binding_names[i] &&
                    stmt->match_stmt.payload_binding_names[i][j]) {
                    named_binding_count++;
                }
            }

            if (named_binding_count > 0) {
                typechecker_push_scope(tc);
                body_binding_scope_pushed = true;
                for (int j = 0; j < payload_bind_count; j++) {
                    const char* bind_name = stmt->match_stmt.payload_binding_names[i][j];
                    if (!bind_name) continue;
                    Type* bind_type = (payload_bind_types && payload_bind_types[j])
                                         ? payload_bind_types[j]
                                         : NULL;

                    Type* bind_type_clone = bind_type ? type_clone(bind_type) : type_any();
                    Symbol* bind_sym = symbol_create(bind_type_clone, bind_name, false);
                    if (is_builtin_name(bind_name)) {
                        symbol_free(bind_sym);
                        continue;
                    }
                    symbol_set_visibility_metadata(bind_sym, stmt->file, true);
                    if (!typechecker_declare(tc, bind_sym)) {
                        symbol_free(bind_sym);
                        typechecker_error(tc,
                                          "Variable already declared",
                                          pattern_expr ? pattern_expr->file : stmt->file,
                                          pattern_expr ? pattern_expr->line : stmt->line,
                                          pattern_expr ? pattern_expr->column : stmt->column);
                    }
                }
            }
        } else {
            pattern_type = typecheck_expression_with_expected(tc, stmt->match_stmt.patterns[i], subject_type);
        }

        if (!type_is_match_comparable(pattern_type)) {
            typechecker_error(tc,
                              "match pattern must be int/bool/double/bigint/string/nil/tuple/record (or any)",
                              stmt->file,
                              stmt->line,
                              0);
        }

        if (subject_type->kind != TYPE_ANY &&
            pattern_type->kind != TYPE_ANY &&
            !typechecker_types_assignable(tc, subject_type, pattern_type, stmt->file, stmt->line, stmt->column) &&
            !typechecker_types_assignable(tc, pattern_type, subject_type, stmt->file, stmt->line, stmt->column)) {
            typechecker_error(tc, "match pattern type is incompatible with subject type", stmt->file, stmt->line, 0);
        }

        const char* pattern_enum = type_enum_name(pattern_type);
        if (subject_enum && pattern_enum && strcmp(subject_enum, pattern_enum) != 0) {
            char message[256];
            snprintf(message,
                     sizeof(message),
                     "match pattern enum '%s' is incompatible with subject enum '%s'",
                     pattern_enum,
                     subject_enum);
            typechecker_error(tc,
                              message,
                              pattern_expr->file ? pattern_expr->file : stmt->file,
                              pattern_expr->line,
                              pattern_expr->column);
        }

        if (subject_is_bool && arm_is_covering) {
            bool pattern_value = false;
            if (match_expr_is_bool_literal(pattern_expr, &pattern_value)) {
                if (pattern_value) {
                    if (has_true_pattern) {
                        typechecker_error(tc,
                                          "Duplicate 'true' match pattern",
                                          pattern_expr->file ? pattern_expr->file : stmt->file,
                                          pattern_expr->line,
                                          pattern_expr->column);
                    }
                    has_true_pattern = true;
                } else {
                    if (has_false_pattern) {
                        typechecker_error(tc,
                                          "Duplicate 'false' match pattern",
                                          pattern_expr->file ? pattern_expr->file : stmt->file,
                                          pattern_expr->line,
                                          pattern_expr->column);
                    }
                    has_false_pattern = true;
                }
            }
        }

        match_patterns_append(&seen_all_patterns,
                              &seen_all_pattern_count,
                              &seen_all_pattern_capacity,
                              pattern_expr);
        if (guard_expr) {
            match_patterns_append(&seen_guarded_patterns,
                                  &seen_guarded_pattern_count,
                                  &seen_guarded_pattern_capacity,
                                  pattern_expr);
        }
        if (subject_enum && arm_is_covering) {
            match_patterns_append(&seen_unguarded_patterns,
                                  &seen_unguarded_pattern_count,
                                  &seen_unguarded_pattern_capacity,
                                  pattern_expr);
            const char* member_name = match_expr_enum_member_name(tc, pattern_expr, subject_enum);
            if (member_name &&
                match_enum_member_patterns_exhaustive(tc,
                                                      seen_enum_patterns,
                                                      seen_enum_pattern_count,
                                                      subject_type,
                                                      member_name)) {
                char message[320];
                snprintf(message,
                         sizeof(message),
                         "Duplicate match pattern '%s.%s'",
                         subject_enum,
                         member_name);
                typechecker_error(tc,
                                  message,
                                  pattern_expr->file ? pattern_expr->file : stmt->file,
                                  pattern_expr->line,
                                  pattern_expr->column);
            }
            match_patterns_append(&seen_enum_patterns,
                                  &seen_enum_pattern_count,
                                  &seen_enum_pattern_capacity,
                                  pattern_expr);
        }

        if (arm_is_covering && subject_is_structural) {
            match_patterns_append(&seen_unguarded_patterns,
                                  &seen_unguarded_pattern_count,
                                  &seen_unguarded_pattern_capacity,
                                  pattern_expr);
            match_patterns_append(&seen_structural_patterns,
                                  &seen_structural_pattern_count,
                                  &seen_structural_pattern_capacity,
                                  pattern_expr);
        } else if (arm_is_covering && !subject_enum) {
            match_patterns_append(&seen_unguarded_patterns,
                                  &seen_unguarded_pattern_count,
                                  &seen_unguarded_pattern_capacity,
                                  pattern_expr);
        }
        if (arm_is_covering &&
            match_pattern_is_structurally_covering(tc, pattern_expr, subject_type, true)) {
            has_covering_arm = true;
        } else if (arm_is_covering &&
                   subject_is_structural &&
                   !has_covering_arm &&
                   match_patterns_exhaustive(tc,
                                             seen_structural_patterns,
                                             seen_structural_pattern_count,
                                             subject_type)) {
            has_covering_arm = true;
        }

        MatchPatternConst pattern_const;
        if (arm_is_covering &&
            match_expr_constant_key(tc, pattern_expr, &pattern_const) &&
            pattern_const.kind != MATCH_PATTERN_CONST_BOOL) {
            for (int j = 0; j < seen_pattern_const_count; j++) {
                if (match_pattern_constant_equals(&pattern_const, &seen_pattern_consts[j])) {
                    char literal_buf[256];
                    char message[320];
                    match_pattern_constant_format(&pattern_const, literal_buf, sizeof(literal_buf));
                    snprintf(message, sizeof(message), "Duplicate match pattern '%s'", literal_buf);
                    typechecker_error(tc,
                                      message,
                                      pattern_expr->file ? pattern_expr->file : stmt->file,
                                      pattern_expr->line,
                                      pattern_expr->column);
                    break;
                }
            }

            seen_pattern_const_count++;
            if (seen_pattern_const_count > seen_pattern_const_capacity) {
                seen_pattern_const_capacity = seen_pattern_const_count * 2;
                seen_pattern_consts = (MatchPatternConst*)safe_realloc(seen_pattern_consts,
                                                                       (size_t)seen_pattern_const_capacity * sizeof(MatchPatternConst));
            }
            seen_pattern_consts[seen_pattern_const_count - 1] = pattern_const;
        }

        if (guard_expr) {
            Type* guard_type = typecheck_expression(tc, guard_expr);
            if (guard_type->kind != TYPE_BOOL && guard_type->kind != TYPE_ANY) {
                typechecker_error(tc,
                                  "Match guard must be bool",
                                  guard_expr->file ? guard_expr->file : stmt->file,
                                  guard_expr->line,
                                  guard_expr->column);
            }
        }

        typecheck_statement(tc, stmt->match_stmt.bodies[i]);
        if (body_binding_scope_pushed) {
            typechecker_pop_scope(tc);
        }
        match_pattern_free_payload_bindings(payload_bind_names, payload_bind_types, payload_bind_count);
    }

    if (subject_enum) {
        enum_missing_count = 0;
        if (has_covering_arm) {
            enum_missing_count = 0;
        } else if (tc && tc->globals) {
            for (int i = 0; i < tc->globals->symbol_count; i++) {
                Symbol* sym = tc->globals->symbols ? tc->globals->symbols[i] : NULL;
                const char* member_name = NULL;
                if (!symbol_is_enum_member_for(sym, subject_enum, &member_name)) {
                    continue;
                }

                if (!match_enum_member_patterns_exhaustive(tc,
                                                           seen_enum_patterns,
                                                           seen_enum_pattern_count,
                                                           subject_type,
                                                           member_name)) {
                    enum_missing_count++;
                }
            }
        }
    }

    if (stmt->match_stmt.else_branch) {
        typecheck_statement(tc, stmt->match_stmt.else_branch);
        if (has_covering_arm) {
            typechecker_warn(tc,
                             "Unreachable else branch: previous match patterns already cover all values",
                             stmt->match_stmt.else_branch->file ? stmt->match_stmt.else_branch->file : stmt->file,
                             stmt->match_stmt.else_branch->line,
                             stmt->match_stmt.else_branch->column);
        } else if (subject_is_bool && has_true_pattern && has_false_pattern) {
            typechecker_warn(tc,
                             "Unreachable else branch: bool match already covers true and false",
                             stmt->match_stmt.else_branch->file ? stmt->match_stmt.else_branch->file : stmt->file,
                             stmt->match_stmt.else_branch->line,
                             stmt->match_stmt.else_branch->column);
        }
        if (subject_enum && !has_covering_arm && enum_missing_count == 0) {
            char message[320];
            snprintf(message,
                     sizeof(message),
                     "Unreachable else branch: enum match already covers all members of '%s'",
                     subject_enum);
            typechecker_warn(tc,
                             message,
                             stmt->match_stmt.else_branch->file ? stmt->match_stmt.else_branch->file : stmt->file,
                             stmt->match_stmt.else_branch->line,
                             stmt->match_stmt.else_branch->column);
        }
    } else if (has_guarded_arm &&
               seen_all_pattern_count > 0 &&
               match_patterns_exhaustive(tc,
                                         seen_all_patterns,
                                         seen_all_pattern_count,
                                         subject_type) &&
               !match_patterns_exhaustive(tc,
                                          seen_unguarded_patterns,
                                          seen_unguarded_pattern_count,
                                          subject_type)) {
        char* msg = match_guarded_non_exhaustive_message(tc,
                                                         seen_unguarded_patterns,
                                                         seen_unguarded_pattern_count,
                                                         seen_guarded_patterns,
                                                         seen_guarded_pattern_count,
                                                         subject_type,
                                                         false);
        if (!msg) {
            msg = safe_strdup("Non-exhaustive match");
        }
        typechecker_error(tc, msg, stmt->file, stmt->line, stmt->column);
        free(msg);
    } else if (subject_is_bool) {
        if (!has_true_pattern && !has_false_pattern) {
            typechecker_error(tc,
                              "Non-exhaustive bool match: missing 'true' and 'false' patterns or an else branch",
                              stmt->file,
                              stmt->line,
                              stmt->column);
        } else if (!has_true_pattern) {
            typechecker_error(tc,
                              "Non-exhaustive bool match: missing 'true' pattern or an else branch",
                              stmt->file,
                              stmt->line,
                              stmt->column);
        } else if (!has_false_pattern) {
            typechecker_error(tc,
                              "Non-exhaustive bool match: missing 'false' pattern or an else branch",
                              stmt->file,
                              stmt->line,
                              stmt->column);
        }
    } else if (subject_enum) {
        int missing_count = enum_missing_count < 0 ? 0 : enum_missing_count;
        if (missing_count > 0) {
            char* msg = match_enum_non_exhaustive_message(tc,
                                                          seen_enum_patterns,
                                                          seen_enum_pattern_count,
                                                          subject_type);
            if (!msg) {
                msg = safe_strdup("Non-exhaustive enum match");
            }
            typechecker_error(tc, msg, stmt->file, stmt->line, stmt->column);
            free(msg);
        }
    }

    if (seen_pattern_consts) {
        free(seen_pattern_consts);
    }
    if (seen_all_patterns) {
        free(seen_all_patterns);
    }
    if (seen_guarded_patterns) {
        free(seen_guarded_patterns);
    }
    if (seen_unguarded_patterns) {
        free(seen_unguarded_patterns);
    }
    if (seen_enum_patterns) {
        free(seen_enum_patterns);
    }
    if (alternation_groups) {
        for (int i = 0; i <= stmt->match_stmt.arm_count; i++) {
            match_alternation_binding_group_free(&alternation_groups[i]);
        }
        free(alternation_groups);
    }
    if (seen_structural_patterns) {
        free(seen_structural_patterns);
    }
}

static Type* typecheck_match_expression(TypeChecker* tc, Expr* expr) {
    match_expr_clear_payload_bindings(expr);
    if (expr->match_expr.arm_count > 0) {
        expr->match_expr.payload_binding_names =
            (char***)safe_calloc((size_t)expr->match_expr.arm_count, sizeof(char**));
        expr->match_expr.payload_binding_counts =
            (int*)safe_calloc((size_t)expr->match_expr.arm_count, sizeof(int));
    }
    MatchAlternationBindingGroup* alternation_groups = NULL;
    if (expr->match_expr.arm_group_ids && expr->match_expr.arm_count > 0) {
        alternation_groups = (MatchAlternationBindingGroup*)safe_calloc(
            (size_t)expr->match_expr.arm_count + 1,
            sizeof(MatchAlternationBindingGroup));
    }

    Type* subject_type = typecheck_expression(tc, expr->match_expr.subject);
    if (!type_is_match_comparable(subject_type)) {
        typechecker_error(tc,
                          "match subject must be int/bool/double/bigint/string/nil/tuple/record (or any)",
                          expr->file,
                          expr->line,
                          0);
    }

    const char* subject_enum = type_enum_name(subject_type);
    bool subject_is_bool = subject_type && subject_type->kind == TYPE_BOOL;
    bool subject_is_structural =
        subject_type &&
        (subject_type->kind == TYPE_TUPLE || subject_type->kind == TYPE_RECORD);
    bool has_true_pattern = false;
    bool has_false_pattern = false;
    MatchPatternConst* seen_pattern_consts = NULL;
    int seen_pattern_const_count = 0;
    int seen_pattern_const_capacity = 0;
    int enum_missing_count = -1;
    Type* result_type = NULL;
    bool enum_patterns_exhaustive = false;
    bool has_guarded_arm = false;
    bool has_covering_arm = false;
    Expr** seen_all_patterns = NULL;
    int seen_all_pattern_count = 0;
    int seen_all_pattern_capacity = 0;
    Expr** seen_guarded_patterns = NULL;
    int seen_guarded_pattern_count = 0;
    int seen_guarded_pattern_capacity = 0;
    Expr** seen_unguarded_patterns = NULL;
    int seen_unguarded_pattern_count = 0;
    int seen_unguarded_pattern_capacity = 0;
    Expr** seen_enum_patterns = NULL;
    int seen_enum_pattern_count = 0;
    int seen_enum_pattern_capacity = 0;
    Expr** seen_structural_patterns = NULL;
    int seen_structural_pattern_count = 0;
    int seen_structural_pattern_capacity = 0;

    for (int i = 0; i < expr->match_expr.arm_count; i++) {
        Expr* pattern_expr = expr->match_expr.patterns[i];
        Expr* guard_expr = expr->match_expr.guards ? expr->match_expr.guards[i] : NULL;
        Expr* value_expr = expr->match_expr.values ? expr->match_expr.values[i] : NULL;
        Type* pattern_type = NULL;
        bool value_binding_scope_pushed = false;
        char** payload_bind_names = NULL;
        Type** payload_bind_types = NULL;
        int payload_bind_count = 0;
        bool has_payload_bindings = false;
        bool arm_is_covering = (guard_expr == NULL);
        bool pattern_needs_typed_subject =
            pattern_expr &&
            (pattern_expr->kind == EXPR_TUPLE_LITERAL || pattern_expr->kind == EXPR_RECORD_LITERAL);

        if (guard_expr) {
            has_guarded_arm = true;
        }

        if (pattern_expr && subject_type) {
            match_pattern_apply_context_types(tc, pattern_expr, subject_type, false);
        }
        if (subject_type && subject_type->kind == TYPE_ANY && pattern_needs_typed_subject) {
            typechecker_error(tc,
                              "Tuple and record patterns require a non-any subject type",
                              pattern_expr ? pattern_expr->file : expr->file,
                              pattern_expr ? pattern_expr->line : expr->line,
                              pattern_expr ? pattern_expr->column : expr->column);
        }
        has_payload_bindings = match_pattern_collect_payload_bindings(tc,
                                                                      pattern_expr,
                                                                      subject_type,
                                                                      &payload_bind_names,
                                                                      &payload_bind_types,
                                                                      &payload_bind_count);
        int alternation_group_id =
            (expr->match_expr.arm_group_ids && i < expr->match_expr.arm_count)
                ? expr->match_expr.arm_group_ids[i]
                : 0;
        if (alternation_groups && alternation_group_id > 0) {
            MatchAlternationBindingGroup* group = &alternation_groups[alternation_group_id];
            if (!group->initialized) {
                match_alternation_binding_group_capture(group,
                                                        payload_bind_names,
                                                        payload_bind_types,
                                                        payload_bind_count);
            } else {
                match_validate_alternation_bindings(tc,
                                                    group,
                                                    payload_bind_names,
                                                    payload_bind_types,
                                                    payload_bind_count,
                                                    pattern_expr ? pattern_expr->file : expr->file,
                                                    pattern_expr ? pattern_expr->line : expr->line,
                                                    pattern_expr ? pattern_expr->column : expr->column);
            }
        }

        enum_patterns_exhaustive =
            subject_enum &&
            seen_enum_pattern_count > 0 &&
            match_patterns_exhaustive(tc,
                                      seen_enum_patterns,
                                      seen_enum_pattern_count,
                                      subject_type);

        if (has_covering_arm) {
            typechecker_warn(tc,
                             "Unreachable match arm: previous patterns already cover all values",
                             pattern_expr && pattern_expr->file ? pattern_expr->file : expr->file,
                             pattern_expr ? pattern_expr->line : expr->line,
                             pattern_expr ? pattern_expr->column : expr->column);
        } else if (subject_is_bool && has_true_pattern && has_false_pattern) {
            typechecker_warn(tc,
                             "Unreachable match arm: bool match already covers true and false",
                             pattern_expr && pattern_expr->file ? pattern_expr->file : expr->file,
                             pattern_expr ? pattern_expr->line : expr->line,
                             pattern_expr ? pattern_expr->column : expr->column);
        }
        if (!has_covering_arm && subject_enum && enum_patterns_exhaustive) {
            char message[320];
            snprintf(message,
                     sizeof(message),
                     "Unreachable match arm: enum match already covers all members of '%s'",
                     subject_enum);
            typechecker_warn(tc,
                             message,
                             pattern_expr && pattern_expr->file ? pattern_expr->file : expr->file,
                             pattern_expr ? pattern_expr->line : expr->line,
                             pattern_expr ? pattern_expr->column : expr->column);
        }

        if (has_payload_bindings) {
            for (int j = 0; j < payload_bind_count; j++) {
                const char* bind_name = payload_bind_names ? payload_bind_names[j] : NULL;
                if (!bind_name) continue;

                if (is_builtin_name(bind_name)) {
                    typechecker_error(tc,
                                      "Cannot declare a variable with a built-in name",
                                      pattern_expr ? pattern_expr->file : expr->file,
                                      pattern_expr ? pattern_expr->line : expr->line,
                                      pattern_expr ? pattern_expr->column : expr->column);
                }

                for (int k = 0; k < j; k++) {
                    const char* prev = payload_bind_names ? payload_bind_names[k] : NULL;
                    if (prev && strcmp(prev, bind_name) == 0) {
                        typechecker_error(tc,
                                          "Duplicate enum payload binding name in match pattern",
                                          pattern_expr ? pattern_expr->file : expr->file,
                                          pattern_expr ? pattern_expr->line : expr->line,
                                          pattern_expr ? pattern_expr->column : expr->column);
                        break;
                    }
                }
            }

            typechecker_push_scope(tc);
            bool discard_declared = false;
            for (int j = 0; j < payload_bind_count; j++) {
                const char* bind_name = payload_bind_names ? payload_bind_names[j] : NULL;
                Type* bind_type = (payload_bind_types && payload_bind_types[j])
                                     ? payload_bind_types[j]
                                     : NULL;

                if (!bind_name) {
                    if (!discard_declared) {
                        Symbol* discard_sym = symbol_create(type_any(), "_", false);
                        symbol_set_visibility_metadata(discard_sym, expr->file, true);
                        if (!typechecker_declare(tc, discard_sym)) {
                            symbol_free(discard_sym);
                        }
                        discard_declared = true;
                    }
                    continue;
                }

                Type* bind_type_clone = bind_type ? type_clone(bind_type) : type_any();
                Symbol* bind_sym = symbol_create(bind_type_clone, bind_name, false);
                if (is_builtin_name(bind_name)) {
                    symbol_free(bind_sym);
                    continue;
                }
                symbol_set_visibility_metadata(bind_sym, expr->file, true);
                if (!typechecker_declare(tc, bind_sym)) {
                    symbol_free(bind_sym);
                    typechecker_error(tc,
                                      "Variable already declared",
                                      pattern_expr ? pattern_expr->file : expr->file,
                                      pattern_expr ? pattern_expr->line : expr->line,
                                      pattern_expr ? pattern_expr->column : expr->column);
                }
            }

            pattern_type = typecheck_expression_with_expected(tc, pattern_expr, subject_type);
            typechecker_pop_scope(tc);

            if (expr->match_expr.payload_binding_names &&
                expr->match_expr.payload_binding_counts &&
                i < expr->match_expr.arm_count) {
                expr->match_expr.payload_binding_names[i] = payload_bind_names;
                expr->match_expr.payload_binding_counts[i] = payload_bind_count;
                payload_bind_names = NULL;
            }

            int named_binding_count = 0;
            for (int j = 0; j < payload_bind_count; j++) {
                if (expr->match_expr.payload_binding_names &&
                    expr->match_expr.payload_binding_names[i] &&
                    expr->match_expr.payload_binding_names[i][j]) {
                    named_binding_count++;
                }
            }

            if (named_binding_count > 0) {
                typechecker_push_scope(tc);
                value_binding_scope_pushed = true;
                for (int j = 0; j < payload_bind_count; j++) {
                    const char* bind_name = expr->match_expr.payload_binding_names[i][j];
                    if (!bind_name) continue;
                    Type* bind_type = (payload_bind_types && payload_bind_types[j])
                                         ? payload_bind_types[j]
                                         : NULL;

                    Type* bind_type_clone = bind_type ? type_clone(bind_type) : type_any();
                    Symbol* bind_sym = symbol_create(bind_type_clone, bind_name, false);
                    if (is_builtin_name(bind_name)) {
                        symbol_free(bind_sym);
                        continue;
                    }
                    symbol_set_visibility_metadata(bind_sym, expr->file, true);
                    if (!typechecker_declare(tc, bind_sym)) {
                        symbol_free(bind_sym);
                        typechecker_error(tc,
                                          "Variable already declared",
                                          pattern_expr ? pattern_expr->file : expr->file,
                                          pattern_expr ? pattern_expr->line : expr->line,
                                          pattern_expr ? pattern_expr->column : expr->column);
                    }
                }
            }
        } else {
            pattern_type = typecheck_expression_with_expected(tc, pattern_expr, subject_type);
        }

        if (!type_is_match_comparable(pattern_type)) {
            typechecker_error(tc,
                              "match pattern must be int/bool/double/bigint/string/nil/tuple/record (or any)",
                              expr->file,
                              expr->line,
                              0);
        }

        if (subject_type->kind != TYPE_ANY &&
            pattern_type->kind != TYPE_ANY &&
            !typechecker_types_assignable(tc, subject_type, pattern_type, expr->file, expr->line, expr->column) &&
            !typechecker_types_assignable(tc, pattern_type, subject_type, expr->file, expr->line, expr->column)) {
            typechecker_error(tc,
                              "match pattern type is incompatible with subject type",
                              expr->file,
                              expr->line,
                              0);
        }

        const char* pattern_enum = type_enum_name(pattern_type);
        if (subject_enum && pattern_enum && strcmp(subject_enum, pattern_enum) != 0) {
            char message[256];
            snprintf(message,
                     sizeof(message),
                     "match pattern enum '%s' is incompatible with subject enum '%s'",
                     pattern_enum,
                     subject_enum);
            typechecker_error(tc,
                              message,
                              pattern_expr->file ? pattern_expr->file : expr->file,
                              pattern_expr->line,
                              pattern_expr->column);
        }

        if (subject_is_bool && arm_is_covering) {
            bool pattern_value = false;
            if (match_expr_is_bool_literal(pattern_expr, &pattern_value)) {
                if (pattern_value) {
                    if (has_true_pattern) {
                        typechecker_error(tc,
                                          "Duplicate 'true' match pattern",
                                          pattern_expr->file ? pattern_expr->file : expr->file,
                                          pattern_expr->line,
                                          pattern_expr->column);
                    }
                    has_true_pattern = true;
                } else {
                    if (has_false_pattern) {
                        typechecker_error(tc,
                                          "Duplicate 'false' match pattern",
                                          pattern_expr->file ? pattern_expr->file : expr->file,
                                          pattern_expr->line,
                                          pattern_expr->column);
                    }
                    has_false_pattern = true;
                }
            }
        }

        match_patterns_append(&seen_all_patterns,
                              &seen_all_pattern_count,
                              &seen_all_pattern_capacity,
                              pattern_expr);
        if (guard_expr) {
            match_patterns_append(&seen_guarded_patterns,
                                  &seen_guarded_pattern_count,
                                  &seen_guarded_pattern_capacity,
                                  pattern_expr);
        }
        if (subject_enum && arm_is_covering) {
            match_patterns_append(&seen_unguarded_patterns,
                                  &seen_unguarded_pattern_count,
                                  &seen_unguarded_pattern_capacity,
                                  pattern_expr);
            const char* member_name = match_expr_enum_member_name(tc, pattern_expr, subject_enum);
            if (member_name &&
                match_enum_member_patterns_exhaustive(tc,
                                                      seen_enum_patterns,
                                                      seen_enum_pattern_count,
                                                      subject_type,
                                                      member_name)) {
                char message[320];
                snprintf(message,
                         sizeof(message),
                         "Duplicate match pattern '%s.%s'",
                         subject_enum,
                         member_name);
                typechecker_error(tc,
                                  message,
                                  pattern_expr->file ? pattern_expr->file : expr->file,
                                  pattern_expr->line,
                                  pattern_expr->column);
            }
            match_patterns_append(&seen_enum_patterns,
                                  &seen_enum_pattern_count,
                                  &seen_enum_pattern_capacity,
                                  pattern_expr);
        }

        if (arm_is_covering && subject_is_structural) {
            match_patterns_append(&seen_unguarded_patterns,
                                  &seen_unguarded_pattern_count,
                                  &seen_unguarded_pattern_capacity,
                                  pattern_expr);
            match_patterns_append(&seen_structural_patterns,
                                  &seen_structural_pattern_count,
                                  &seen_structural_pattern_capacity,
                                  pattern_expr);
        } else if (arm_is_covering && !subject_enum) {
            match_patterns_append(&seen_unguarded_patterns,
                                  &seen_unguarded_pattern_count,
                                  &seen_unguarded_pattern_capacity,
                                  pattern_expr);
        }
        if (arm_is_covering &&
            match_pattern_is_structurally_covering(tc, pattern_expr, subject_type, true)) {
            has_covering_arm = true;
        } else if (arm_is_covering &&
                   subject_is_structural &&
                   !has_covering_arm &&
                   match_patterns_exhaustive(tc,
                                             seen_structural_patterns,
                                             seen_structural_pattern_count,
                                             subject_type)) {
            has_covering_arm = true;
        }

        MatchPatternConst pattern_const;
        if (arm_is_covering &&
            match_expr_constant_key(tc, pattern_expr, &pattern_const) &&
            pattern_const.kind != MATCH_PATTERN_CONST_BOOL) {
            for (int j = 0; j < seen_pattern_const_count; j++) {
                if (match_pattern_constant_equals(&pattern_const, &seen_pattern_consts[j])) {
                    char literal_buf[256];
                    char message[320];
                    match_pattern_constant_format(&pattern_const, literal_buf, sizeof(literal_buf));
                    snprintf(message, sizeof(message), "Duplicate match pattern '%s'", literal_buf);
                    typechecker_error(tc,
                                      message,
                                      pattern_expr->file ? pattern_expr->file : expr->file,
                                      pattern_expr->line,
                                      pattern_expr->column);
                    break;
                }
            }

            seen_pattern_const_count++;
            if (seen_pattern_const_count > seen_pattern_const_capacity) {
                seen_pattern_const_capacity = seen_pattern_const_count * 2;
                seen_pattern_consts = (MatchPatternConst*)safe_realloc(
                    seen_pattern_consts,
                    (size_t)seen_pattern_const_capacity * sizeof(MatchPatternConst));
            }
            seen_pattern_consts[seen_pattern_const_count - 1] = pattern_const;
        }

        if (guard_expr) {
            Type* guard_type = typecheck_expression(tc, guard_expr);
            if (guard_type->kind != TYPE_BOOL && guard_type->kind != TYPE_ANY) {
                typechecker_error(tc,
                                  "Match guard must be bool",
                                  guard_expr->file ? guard_expr->file : expr->file,
                                  guard_expr->line,
                                  guard_expr->column);
            }
        }

        Type* value_type = typecheck_expression(tc, value_expr);
        if (value_type && value_type->kind == TYPE_VOID) {
            typechecker_error(tc,
                              "match expression arms must produce a value",
                              value_expr->file ? value_expr->file : expr->file,
                              value_expr ? value_expr->line : expr->line,
                              value_expr ? value_expr->column : expr->column);
        }
        result_type = match_merge_result_type(tc,
                                              result_type,
                                              value_type,
                                              value_expr && value_expr->file ? value_expr->file : expr->file,
                                              value_expr ? value_expr->line : expr->line,
                                              value_expr ? value_expr->column : expr->column);

        if (value_binding_scope_pushed) {
            typechecker_pop_scope(tc);
        }
        match_pattern_free_payload_bindings(payload_bind_names, payload_bind_types, payload_bind_count);
    }

    if (subject_enum) {
        enum_missing_count = 0;
        if (has_covering_arm) {
            enum_missing_count = 0;
        } else if (tc && tc->globals) {
            for (int i = 0; i < tc->globals->symbol_count; i++) {
                Symbol* sym = tc->globals->symbols ? tc->globals->symbols[i] : NULL;
                const char* member_name = NULL;
                if (!symbol_is_enum_member_for(sym, subject_enum, &member_name)) {
                    continue;
                }

                if (!match_enum_member_patterns_exhaustive(tc,
                                                           seen_enum_patterns,
                                                           seen_enum_pattern_count,
                                                           subject_type,
                                                           member_name)) {
                    enum_missing_count++;
                }
            }
        }
    }

    if (expr->match_expr.else_expr) {
        Type* else_type = typecheck_expression(tc, expr->match_expr.else_expr);
        if (else_type && else_type->kind == TYPE_VOID) {
            typechecker_error(tc,
                              "match expression arms must produce a value",
                              expr->match_expr.else_expr->file ? expr->match_expr.else_expr->file : expr->file,
                              expr->match_expr.else_expr->line,
                              expr->match_expr.else_expr->column);
        }
        result_type = match_merge_result_type(tc,
                                              result_type,
                                              else_type,
                                              expr->match_expr.else_expr->file ? expr->match_expr.else_expr->file : expr->file,
                                              expr->match_expr.else_expr->line,
                                              expr->match_expr.else_expr->column);
        if (has_covering_arm) {
            typechecker_warn(tc,
                             "Unreachable else branch: previous match patterns already cover all values",
                             expr->match_expr.else_expr->file ? expr->match_expr.else_expr->file : expr->file,
                             expr->match_expr.else_expr->line,
                             expr->match_expr.else_expr->column);
        } else if (subject_is_bool && has_true_pattern && has_false_pattern) {
            typechecker_warn(tc,
                             "Unreachable else branch: bool match already covers true and false",
                             expr->match_expr.else_expr->file ? expr->match_expr.else_expr->file : expr->file,
                             expr->match_expr.else_expr->line,
                             expr->match_expr.else_expr->column);
        }
        if (subject_enum && !has_covering_arm && enum_missing_count == 0) {
            char message[320];
            snprintf(message,
                     sizeof(message),
                     "Unreachable else branch: enum match already covers all members of '%s'",
                     subject_enum);
            typechecker_warn(tc,
                             message,
                             expr->match_expr.else_expr->file ? expr->match_expr.else_expr->file : expr->file,
                             expr->match_expr.else_expr->line,
                             expr->match_expr.else_expr->column);
        }
    } else if (has_covering_arm) {
        // Covered by an unguarded catch-all or structural destructuring arm.
    } else if (has_guarded_arm &&
               seen_all_pattern_count > 0 &&
               match_patterns_exhaustive(tc,
                                         seen_all_patterns,
                                         seen_all_pattern_count,
                                         subject_type) &&
               !match_patterns_exhaustive(tc,
                                          seen_unguarded_patterns,
                                          seen_unguarded_pattern_count,
                                          subject_type)) {
        char* msg = match_guarded_non_exhaustive_message(tc,
                                                         seen_unguarded_patterns,
                                                         seen_unguarded_pattern_count,
                                                         seen_guarded_patterns,
                                                         seen_guarded_pattern_count,
                                                         subject_type,
                                                         true);
        if (!msg) {
            msg = safe_strdup("Non-exhaustive match expression");
        }
        typechecker_error(tc, msg, expr->file, expr->line, expr->column);
        free(msg);
    } else if (subject_is_bool) {
        if (!has_true_pattern && !has_false_pattern) {
            typechecker_error(tc,
                              "Non-exhaustive bool match: missing 'true' and 'false' patterns or an else branch",
                              expr->file,
                              expr->line,
                              expr->column);
        } else if (!has_true_pattern) {
            typechecker_error(tc,
                              "Non-exhaustive bool match: missing 'true' pattern or an else branch",
                              expr->file,
                              expr->line,
                              expr->column);
        } else if (!has_false_pattern) {
            typechecker_error(tc,
                              "Non-exhaustive bool match: missing 'false' pattern or an else branch",
                              expr->file,
                              expr->line,
                              expr->column);
        }
    } else if (subject_enum) {
        int missing_count = enum_missing_count < 0 ? 0 : enum_missing_count;
        if (missing_count > 0) {
            char* msg = match_enum_non_exhaustive_message(tc,
                                                          seen_enum_patterns,
                                                          seen_enum_pattern_count,
                                                          subject_type);
            if (!msg) {
                msg = safe_strdup("Non-exhaustive enum match");
            }
            typechecker_error(tc, msg, expr->file, expr->line, expr->column);
            free(msg);
        }
    } else if (subject_is_structural) {
        char* msg = match_structural_non_exhaustive_message(tc,
                                                            seen_structural_patterns,
                                                            seen_structural_pattern_count,
                                                            subject_type);
        if (!msg) {
            msg = safe_strdup("match expression requires an else branch unless the subject is exhaustively covered");
        }
        typechecker_error(tc, msg, expr->file, expr->line, expr->column);
        free(msg);
    } else {
        typechecker_error(tc,
                          "match expression requires an else branch unless the subject is exhaustively covered",
                          expr->file,
                          expr->line,
                          expr->column);
    }

    if (!result_type) {
        result_type = type_any();
    }
    expr->type = result_type;

    if (seen_pattern_consts) {
        free(seen_pattern_consts);
    }
    if (seen_all_patterns) {
        free(seen_all_patterns);
    }
    if (seen_guarded_patterns) {
        free(seen_guarded_patterns);
    }
    if (seen_unguarded_patterns) {
        free(seen_unguarded_patterns);
    }
    if (seen_enum_patterns) {
        free(seen_enum_patterns);
    }
    if (alternation_groups) {
        for (int i = 0; i <= expr->match_expr.arm_count; i++) {
            match_alternation_binding_group_free(&alternation_groups[i]);
        }
        free(alternation_groups);
    }
    if (seen_structural_patterns) {
        free(seen_structural_patterns);
    }

    return expr->type;
}

static void typecheck_while(TypeChecker* tc, Stmt* stmt) {
    Type* cond_type = typecheck_expression(tc, stmt->while_stmt.condition);
    if (cond_type->kind != TYPE_BOOL && cond_type->kind != TYPE_ANY) {
        typechecker_error(tc, "While condition must be bool", stmt->file, stmt->line, 0);
    }
    typecheck_statement(tc, stmt->while_stmt.body);
}

static void typecheck_foreach(TypeChecker* tc, Stmt* stmt) {
    Type* iterable_type = typecheck_expression(tc, stmt->foreach.iterable);
    
    if (iterable_type->kind != TYPE_ARRAY && iterable_type->kind != TYPE_BYTES && iterable_type->kind != TYPE_ANY) {
        typechecker_error(tc, "Foreach requires an array/bytes", stmt->file, stmt->line, 0);
    }
    
    typechecker_push_scope(tc);
    
    Type* elem_type = type_any();
    if (iterable_type->kind == TYPE_ARRAY) {
        elem_type = type_clone(iterable_type->element_type);
    } else if (iterable_type->kind == TYPE_BYTES) {
        elem_type = type_int();
    }
    Symbol* sym = symbol_create(elem_type, stmt->foreach.var_name, true);
    typechecker_declare(tc, sym);
    
    typecheck_statement(tc, stmt->foreach.body);
    typechecker_pop_scope(tc);
}

static void typecheck_for_range(TypeChecker* tc, Stmt* stmt) {
    Type* start_type = typecheck_expression(tc, stmt->for_range.start);
    Type* end_type = typecheck_expression(tc, stmt->for_range.end);

    if (start_type->kind != TYPE_INT && start_type->kind != TYPE_ANY) {
        typechecker_error(tc, "Range loop start must be int", stmt->file, stmt->line, 0);
    }
    if (end_type->kind != TYPE_INT && end_type->kind != TYPE_ANY) {
        typechecker_error(tc, "Range loop end must be int", stmt->file, stmt->line, 0);
    }

    typechecker_push_scope(tc);
    Symbol* sym = symbol_create(type_int(), stmt->for_range.var_name, true);
    typechecker_declare(tc, sym);

    typecheck_statement(tc, stmt->for_range.body);
    typechecker_pop_scope(tc);
}

static void typecheck_return(TypeChecker* tc, Stmt* stmt) {
    if (!tc->current_return_type) {
        typechecker_error(tc, "return used outside of a function", stmt->file, stmt->line, 0);
        if (stmt->return_value) {
            typecheck_expression_with_expected(tc, stmt->return_value, NULL);
        }
        return;
    }

    Type* return_type = typechecker_current_function_return_type(tc);
    if (!return_type) return_type = type_any();
    
    if (stmt->return_value) {
        if (stmt->return_value->kind == EXPR_RECORD_LITERAL &&
            return_type && return_type->kind == TYPE_RECORD) {
            stmt->return_value->record_literal.record_type = return_type;
        }
        if (return_type && return_type->kind == TYPE_SET &&
            expr_is_empty_map_literal(stmt->return_value)) {
            expr_coerce_empty_map_literal_to_empty_set_literal(stmt->return_value);
        }
        if (stmt->return_value->kind == EXPR_ARRAY_LITERAL &&
            return_type && return_type->kind == TYPE_ARRAY &&
            return_type->element_type && return_type->element_type->kind == TYPE_RECORD) {
            for (int i = 0; i < stmt->return_value->array_literal.element_count; i++) {
                Expr* elem = stmt->return_value->array_literal.elements[i];
                if (elem && elem->kind == EXPR_RECORD_LITERAL) {
                    elem->record_literal.record_type = return_type->element_type;
                }
            }
        }
        Type* value_type = typecheck_expression_with_expected(tc, stmt->return_value, return_type);
        if (!typechecker_types_assignable(tc, return_type, value_type, stmt->file, stmt->line, stmt->column)) {
            typechecker_error_expected_got(tc,
                                           "Return type mismatch",
                                           return_type,
                                           value_type,
                                           stmt->file,
                                           stmt->line,
                                           0);
        }
    } else if (return_type->kind != TYPE_VOID && return_type->kind != TYPE_NIL) {
        typechecker_error(tc, "Must return a value", stmt->file, stmt->line, 0);
    }
}

static void typechecker_predeclare_func(TypeChecker* tc, Stmt* stmt) {
    if (is_builtin_name(stmt->func_decl.name)) {
        typechecker_error(tc, "Cannot declare a function with a built-in name", stmt->file, stmt->line, 0);
        return;
    }

    for (int i = 0; i < stmt->func_decl.type_param_count; i++) {
        const char* type_param_name = stmt->func_decl.type_params ? stmt->func_decl.type_params[i] : NULL;
        if (!type_param_name || type_param_name[0] == '\0') {
            typechecker_error(tc, "Invalid generic type parameter name", stmt->file, stmt->line, 0);
            return;
        }
        if (is_builtin_name(type_param_name)) {
            typechecker_error(tc, "Generic type parameter cannot use a built-in name", stmt->file, stmt->line, 0);
            return;
        }
        for (int j = i + 1; j < stmt->func_decl.type_param_count; j++) {
            const char* other = stmt->func_decl.type_params ? stmt->func_decl.type_params[j] : NULL;
            if (other && strcmp(type_param_name, other) == 0) {
                typechecker_error(tc, "Duplicate generic type parameter name", stmt->file, stmt->line, 0);
                return;
            }
        }

        if (stmt->func_decl.type_param_constraints &&
            stmt->func_decl.type_param_constraints[i]) {
            stmt->func_decl.type_param_constraints[i] =
                typechecker_resolve_type(tc, stmt->func_decl.type_param_constraints[i]);

            Type* constraint = stmt->func_decl.type_param_constraints[i];
            if (constraint &&
                constraint->kind == TYPE_TYPE_PARAM &&
                constraint->type_param_name &&
                strcmp(constraint->type_param_name, type_param_name) == 0) {
                typechecker_error(tc,
                                  "Generic type parameter constraint cannot reference itself",
                                  stmt->file,
                                  stmt->line,
                                  0);
                return;
            }
        }
    }

    stmt->func_decl.return_type = typechecker_resolve_type(tc, stmt->func_decl.return_type);
    for (int i = 0; i < stmt->func_decl.param_count; i++) {
        stmt->func_decl.param_types[i] = typechecker_resolve_type(tc, stmt->func_decl.param_types[i]);
    }
    if (stmt->func_decl.return_type) {
    }
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
    symbol_set_visibility_metadata(sym, stmt->file, stmt->is_public);
    int declare_result = typechecker_declare(tc, sym);
    if (!declare_result) {
        symbol_free(sym);
        typechecker_error(tc, "Function already declared", stmt->file, stmt->line, 0);
        return;
    }
}

static void typechecker_refresh_function_signature(TypeChecker* tc, Stmt* stmt) {
    if (!tc || !stmt || stmt->kind != STMT_FUNC_DECL || !stmt->func_decl.name) return;

    stmt->func_decl.return_type = typechecker_resolve_type(tc, stmt->func_decl.return_type);
    for (int i = 0; i < stmt->func_decl.param_count; i++) {
        stmt->func_decl.param_types[i] = typechecker_resolve_type(tc, stmt->func_decl.param_types[i]);
    }

    Symbol* existing = symbol_table_get(tc->globals, stmt->func_decl.name);
    if (!existing || !existing->type || existing->type->kind != TYPE_FUNCTION) {
        return;
    }

    Type* return_type = stmt->func_decl.return_type
        ? type_clone(stmt->func_decl.return_type)
        : type_void();
    if (stmt->func_decl.is_async) {
        return_type = type_future(return_type ? return_type : type_void());
    }
    Type** param_types = NULL;
    if (stmt->func_decl.param_count > 0) {
        param_types = (Type**)safe_malloc((size_t)stmt->func_decl.param_count * sizeof(Type*));
        for (int i = 0; i < stmt->func_decl.param_count; i++) {
            param_types[i] = type_clone(stmt->func_decl.param_types[i]);
        }
    }

    Type* refreshed_fn = type_function(return_type, param_types, stmt->func_decl.param_count);
    type_function_set_type_params(refreshed_fn,
                                  stmt->func_decl.type_params,
                                  stmt->func_decl.type_param_constraints,
                                  stmt->func_decl.type_param_count);

    type_free(existing->type);
    existing->type = refreshed_fn;
}

static void typecheck_func_decl(TypeChecker* tc, Stmt* stmt) {
    char* old_func = tc->current_function;
    bool old_function_is_async = tc->current_function_is_async;
    Type* old_return_type = tc->current_return_type;
    char** old_type_param_names = tc->current_type_param_names;
    Type** old_type_param_constraints = tc->current_type_param_constraints;
    int old_type_param_count = tc->current_type_param_count;
    int old_lookup_floor = tc->local_lookup_floor;
    tc->current_function = safe_strdup(stmt->func_decl.name);
    tc->current_function_is_async = stmt->func_decl.is_async;
    tc->current_return_type = stmt->func_decl.return_type;
    tc->current_type_param_names = stmt->func_decl.type_params;
    tc->current_type_param_constraints = stmt->func_decl.type_param_constraints;
    tc->current_type_param_count = stmt->func_decl.type_param_count;
    tc->local_lookup_floor = tc->local_count;

    typechecker_push_scope(tc);

    for (int i = 0; i < stmt->func_decl.param_count; i++) {
        Symbol* param = symbol_create(type_clone(stmt->func_decl.param_types[i]), stmt->func_decl.params[i], false);
        if (!typechecker_declare(tc, param)) {
            symbol_free(param);
            typechecker_error(tc, "Parameter already declared", stmt->file, stmt->line, 0);
        }
    }

    if (stmt->func_decl.body && stmt->func_decl.body->kind == STMT_BLOCK) {
        for (int i = 0; i < stmt->func_decl.body->block.stmt_count; i++) {
            typecheck_statement(tc, stmt->func_decl.body->block.statements[i]);
        }
    }

    typechecker_pop_scope(tc);

    if (tc->current_function) free(tc->current_function);
    tc->current_function = old_func;
    tc->current_function_is_async = old_function_is_async;
    tc->current_return_type = old_return_type;
    tc->current_type_param_names = old_type_param_names;
    tc->current_type_param_constraints = old_type_param_constraints;
    tc->current_type_param_count = old_type_param_count;
    tc->local_lookup_floor = old_lookup_floor;
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

static Type* typecheck_arithmetic_types(TypeChecker* tc, TokenType op, Type* left, Type* right, char* file, int line) {
    if (!left || !right) return type_any();

    bool left_any = left->kind == TYPE_ANY;
    bool right_any = right->kind == TYPE_ANY;
    bool left_bigint = left->kind == TYPE_BIGINT;
    bool right_bigint = right->kind == TYPE_BIGINT;

    if (op != TOKEN_PLUS && op != TOKEN_MINUS && op != TOKEN_STAR && op != TOKEN_SLASH && op != TOKEN_PERCENT) {
        typechecker_error(tc, "Invalid arithmetic operator", file, line, 0);
        return type_any();
    }

    if (!left_any && left->kind != TYPE_INT && left->kind != TYPE_DOUBLE &&
        left->kind != TYPE_BIGINT && left->kind != TYPE_STRING) {
        typechecker_error(tc, "Left operand must be int/double/bigint/string", file, line, 0);
    }
    if (!right_any && right->kind != TYPE_INT && right->kind != TYPE_DOUBLE &&
        right->kind != TYPE_BIGINT && right->kind != TYPE_STRING) {
        typechecker_error(tc, "Right operand must be int/double/bigint/string", file, line, 0);
    }

    if (op == TOKEN_PERCENT) {
        if (!left_any && left->kind != TYPE_INT && left->kind != TYPE_BIGINT) {
            typechecker_error(tc, "Left operand must be int/bigint for %", file, line, 0);
        }
        if (!right_any && right->kind != TYPE_INT && right->kind != TYPE_BIGINT) {
            typechecker_error(tc, "Right operand must be int/bigint for %", file, line, 0);
        }
        if (left_bigint || right_bigint) {
            return type_bigint();
        }
        return type_int();
    }

    if (op == TOKEN_PLUS && !left_any && !right_any && left->kind == TYPE_STRING && right->kind == TYPE_STRING) {
        return type_string();
    }

    if (!left_any && !right_any &&
        (left->kind == TYPE_STRING || right->kind == TYPE_STRING)) {
        return type_any();
    }

    if ((left_bigint && right->kind == TYPE_DOUBLE) || (right_bigint && left->kind == TYPE_DOUBLE)) {
        typechecker_error(tc, "Cannot mix bigint and double", file, line, 0);
        return type_any();
    }

    if (left_bigint || right_bigint) {
        return type_bigint();
    }

    if (!left_any && !right_any) {
        if ((left->kind == TYPE_INT && right->kind == TYPE_DOUBLE) ||
            (left->kind == TYPE_DOUBLE && right->kind == TYPE_INT)) {
            typechecker_error(tc, "Mixed int/double operands require an explicit cast", file, line, 0);
            return type_any();
        }
    }

    if (left->kind == TYPE_DOUBLE || right->kind == TYPE_DOUBLE) {
        return type_double();
    }
    if (left->kind == TYPE_STRING || right->kind == TYPE_STRING) {
        return type_any();
    }
    return type_int();
}

static void typecheck_assign_index(TypeChecker* tc, Stmt* stmt) {
    Type* target_type = typecheck_expression(tc, stmt->assign_index.target);
    Type* index_type = typecheck_expression(tc, stmt->assign_index.index);

    if (target_type->kind == TYPE_ARRAY && target_type->element_type &&
        target_type->element_type->kind == TYPE_RECORD &&
        stmt->assign_index.value && stmt->assign_index.value->kind == EXPR_RECORD_LITERAL) {
        stmt->assign_index.value->record_literal.record_type = target_type->element_type;
    }

    Type* index_value_expected = NULL;
    Type* bytes_value_expected = NULL;
    if (target_type->kind == TYPE_BYTES) {
        bytes_value_expected = type_int();
        index_value_expected = bytes_value_expected;
    } else {
        index_value_expected = target_type->element_type;
    }
    Type* value_type = typecheck_expression_with_expected(tc,
                                                          stmt->assign_index.value,
                                                          index_value_expected);
    if (bytes_value_expected) {
        type_free(bytes_value_expected);
    }
    
    if (target_type->kind != TYPE_ARRAY && target_type->kind != TYPE_BYTES) {
        typechecker_error(tc, "Can only assign to array/bytes elements", stmt->file, stmt->line, 0);
    }
    
    if (index_type->kind != TYPE_INT) {
        typechecker_error(tc, "Index must be integer", stmt->file, stmt->line, 0);
    }

    if (stmt->assign_index.op == TOKEN_ASSIGN) {
        if (target_type->kind == TYPE_BYTES) {
            if (value_type->kind != TYPE_INT && value_type->kind != TYPE_ANY) {
                Type* int_type = type_int();
                typechecker_error_expected_got(tc,
                                               "Bytes element type mismatch",
                                               int_type,
                                               value_type,
                                               stmt->file,
                                               stmt->line,
                                               0);
                type_free(int_type);
            }
        } else if (target_type->element_type &&
                   !typechecker_types_assignable(tc,
                                                 target_type->element_type,
                                                 value_type,
                                                 stmt->file,
                                                 stmt->line,
                                                 stmt->column)) {
            typechecker_error_expected_got(tc,
                                           "Array element type mismatch",
                                           target_type->element_type,
                                           value_type,
                                           stmt->file,
                                           stmt->line,
                                           0);
        }
        return;
    }

    TokenType binary_op = compound_assign_to_binary_op(stmt->assign_index.op);
    if (binary_op == TOKEN_ERROR) {
        typechecker_error(tc, "Invalid compound assignment operator", stmt->file, stmt->line, 0);
        return;
    }

    if (target_type->kind == TYPE_BYTES) {
        Type* int_type = type_int();
        Type* result_type = typecheck_arithmetic_types(tc, binary_op, int_type, value_type, stmt->file, stmt->line);
        if (!typechecker_types_assignable(tc, int_type, result_type, stmt->file, stmt->line, stmt->column)) {
            typechecker_error_expected_got(tc,
                                           "Bytes element type mismatch in compound assignment",
                                           int_type,
                                           result_type,
                                           stmt->file,
                                           stmt->line,
                                           0);
        }
        type_free(int_type);
        type_free(result_type);
    } else if (target_type->element_type) {
        Type* result_type = typecheck_arithmetic_types(tc, binary_op, target_type->element_type, value_type, stmt->file, stmt->line);
        if (!typechecker_types_assignable(tc,
                                          target_type->element_type,
                                          result_type,
                                          stmt->file,
                                          stmt->line,
                                          stmt->column)) {
            typechecker_error_expected_got(tc,
                                           "Array element type mismatch in compound assignment",
                                           target_type->element_type,
                                           result_type,
                                           stmt->file,
                                           stmt->line,
                                           0);
        }
        type_free(result_type);
    }
}

static void typecheck_defer(TypeChecker* tc, Stmt* stmt) {
    if (!stmt) return;
    if (!tc->current_return_type) {
        typechecker_error(tc, "defer used outside of a function", stmt->file, stmt->line, 0);
        return;
    }
    if (!stmt->defer_expr) return;
    if (stmt->defer_expr->kind != EXPR_CALL) {
        typechecker_error(tc, "defer expects a function call", stmt->file, stmt->line, 0);
        typecheck_expression(tc, stmt->defer_expr);
        return;
    }
    typecheck_expression(tc, stmt->defer_expr);
}

static void typecheck_statement(TypeChecker* tc, Stmt* stmt) {
    if (!stmt) return;
    const char* prev_file = tc->current_file;
    tc->current_file = stmt->file;
    
    switch (stmt->kind) {
        case STMT_VAR_DECL:
            typecheck_var_decl(tc, stmt);
            break;
        case STMT_VAR_TUPLE_DECL:
            typecheck_var_tuple_decl(tc, stmt);
            break;
        case STMT_EXPR:
            typecheck_expr_stmt(tc, stmt);
            break;
        case STMT_ASSIGN:
            typecheck_assign(tc, stmt);
            break;
        case STMT_ASSIGN_INDEX:
            typecheck_assign_index(tc, stmt);
            break;
        case STMT_ASSIGN_FIELD: {
            Type* object_type = typecheck_expression(tc, stmt->assign_field.object);
            if (object_type->kind != TYPE_RECORD) {
                typechecker_error(tc, "Can only assign fields on record types", stmt->file, stmt->line, 0);
                typecheck_expression_with_expected(tc, stmt->assign_field.value, NULL);
                break;
            }

            Type* field_type = NULL;
            if (object_type->record_def) {
                int field_index = record_def_get_field_index(object_type->record_def, stmt->assign_field.field_name);
                stmt->assign_field.field_index = field_index;
                if (field_index >= 0) {
                    field_type = record_def_get_field(object_type->record_def, field_index)->type;
                }
                if (!field_type) {
                    typechecker_error(tc, "Unknown field", stmt->file, stmt->line, 0);
                }
            }

            if (stmt->assign_field.value && stmt->assign_field.value->kind == EXPR_RECORD_LITERAL &&
                field_type && field_type->kind == TYPE_RECORD) {
                stmt->assign_field.value->record_literal.record_type = field_type;
            }

            Type* value_type = typecheck_expression_with_expected(tc,
                                                                  stmt->assign_field.value,
                                                                  field_type);

            if (stmt->assign_field.op == TOKEN_ASSIGN) {
                if (field_type &&
                    !typechecker_types_assignable(tc, field_type, value_type, stmt->file, stmt->line, stmt->column)) {
                    typechecker_error(tc, "Type mismatch in record field assignment", stmt->file, stmt->line, 0);
                }
                break;
            }

            TokenType binary_op = compound_assign_to_binary_op(stmt->assign_field.op);
            if (binary_op == TOKEN_ERROR) {
                typechecker_error(tc, "Invalid compound assignment operator", stmt->file, stmt->line, 0);
                break;
            }

            if (field_type) {
                Type* result_type = typecheck_arithmetic_types(tc, binary_op, field_type, value_type, stmt->file, stmt->line);
                if (!typechecker_types_assignable(tc, field_type, result_type, stmt->file, stmt->line, stmt->column)) {
                    typechecker_error(tc, "Type mismatch in record field compound assignment", stmt->file, stmt->line, 0);
                }
                type_free(result_type);
            }
            break;
        }
        case STMT_BLOCK:
            typecheck_block(tc, stmt);
            break;
        case STMT_IF:
            typecheck_if(tc, stmt);
            break;
        case STMT_MATCH:
            typecheck_match(tc, stmt);
            break;
        case STMT_WHILE:
            typecheck_while(tc, stmt);
            break;
        case STMT_FOREACH:
            typecheck_foreach(tc, stmt);
            break;
        case STMT_FOR_RANGE:
            typecheck_for_range(tc, stmt);
            break;
        case STMT_IMPORT:
            if (tc->local_count > 0) {
                typechecker_error(tc, "import must appear at top-level", stmt->file, stmt->line, 0);
            }
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            break;
        case STMT_RETURN:
            typecheck_return(tc, stmt);
            break;
        case STMT_DEFER:
            typecheck_defer(tc, stmt);
            break;
        case STMT_FUNC_DECL:
            if (tc->local_count > 0) {
                typechecker_error(tc, "Nested function declarations are not supported", stmt->file, stmt->line, 0);
                break;
            }
            typechecker_refresh_function_signature(tc, stmt);
            typecheck_func_decl(tc, stmt);
            break;
        case STMT_RECORD_DECL: {
            char** old_type_param_names = tc->current_type_param_names;
            Type** old_type_param_constraints = tc->current_type_param_constraints;
            int old_type_param_count = tc->current_type_param_count;
            if (stmt->record_decl.type_param_count > 0) {
                tc->current_type_param_names = stmt->record_decl.type_params;
                tc->current_type_param_constraints = NULL;
                tc->current_type_param_count = stmt->record_decl.type_param_count;
            }

            for (int i = 0; i < stmt->record_decl.field_count; i++) {
                Type* field_type = typechecker_resolve_type(tc, stmt->record_decl.field_types[i]);
                stmt->record_decl.field_types[i] = field_type ? field_type : type_any();
            }

            tc->current_type_param_names = old_type_param_names;
            tc->current_type_param_constraints = old_type_param_constraints;
            tc->current_type_param_count = old_type_param_count;

            if (stmt->record_decl.type_param_count > 0) {
                break;
            }

            Symbol* sym = symbol_table_get(tc->globals, stmt->record_decl.name);
            Type* record_type = NULL;
            if (sym && sym->type && sym->type->kind == TYPE_RECORD) {
                record_type = sym->type;
                if (record_type->record_def && record_type->record_def->field_count > 0) {
                    typechecker_error(tc, "Record type already declared", stmt->file, stmt->line, 0);
                    break;
                }
            } else {
                record_type = type_record(stmt->record_decl.name);
                sym = symbol_create(record_type, stmt->record_decl.name, false);
                symbol_set_visibility_metadata(sym, stmt->file, stmt->is_public);
                if (!typechecker_declare(tc, sym)) {
                    symbol_free(sym);
                    typechecker_error(tc, "Record type already declared", stmt->file, stmt->line, 0);
                    break;
                }
            }
            
            // Add fields to the record definition
            for (int i = 0; i < stmt->record_decl.field_count; i++) {
                Type* field_type = stmt->record_decl.field_types[i];
                record_def_add_field(record_type->record_def, stmt->record_decl.field_names[i], field_type);
            }
            break;
        }
        case STMT_TYPE_ALIAS:
            typecheck_type_alias_decl(tc, stmt);
            break;
        case STMT_INTERFACE_DECL:
            typecheck_interface_decl(tc, stmt);
            break;
        case STMT_IMPL_DECL:
            typecheck_impl_decl(tc, stmt);
            break;
        case STMT_ENUM_DECL:
            if (tc->local_count > 0) {
                typechecker_error(tc, "enum must appear at top-level", stmt->file, stmt->line, 0);
            }
            break;
        default:
            break;
    }

    tc->current_file = prev_file;
}

TypeCheckResult typecheck(Program* program) {
    TypeCheckOptions options = {0};
    options.report_diagnostics = true;
    return typecheck_with_options(program, options);
}

static Type* typechecker_extension_type_from_desc(TypeChecker* tc, TabloExtTypeDesc desc) {
    Type* type = NULL;
    switch (desc.tag) {
        case TABLO_EXT_TYPE_VOID:
            type = type_void();
            break;
        case TABLO_EXT_TYPE_INT:
            type = type_int();
            break;
        case TABLO_EXT_TYPE_BOOL:
            type = type_bool();
            break;
        case TABLO_EXT_TYPE_DOUBLE:
            type = type_double();
            break;
        case TABLO_EXT_TYPE_STRING:
            type = type_string();
            break;
        case TABLO_EXT_TYPE_BYTES:
            type = type_bytes();
            break;
        case TABLO_EXT_TYPE_HANDLE: {
            Symbol* handle_sym = symbol_table_get(tc->globals, desc.handle_type_name);
            if (!handle_sym || !handle_sym->type || handle_sym->type->kind != TYPE_RECORD) {
                typechecker_error(tc,
                                  "Extension function references an unknown handle type",
                                  NULL,
                                  0,
                                  0);
                return type_any();
            }
            type = type_clone(handle_sym->type);
            break;
        }
        case TABLO_EXT_TYPE_ARRAY:
            if (!desc.element_type) {
                typechecker_error(tc, "Extension array type is missing an element type", NULL, 0, 0);
                return type_any();
            }
            type = type_array(typechecker_extension_type_from_desc(tc, *desc.element_type));
            break;
        case TABLO_EXT_TYPE_TUPLE: {
            Type** element_types = NULL;
            if (desc.tuple_element_count < 0) {
                typechecker_error(tc, "Extension tuple type has an invalid arity", NULL, 0, 0);
                return type_any();
            }
            if (desc.tuple_element_count > 0 && !desc.tuple_element_types) {
                typechecker_error(tc, "Extension tuple type is missing element types", NULL, 0, 0);
                return type_any();
            }
            if (desc.tuple_element_count > 0) {
                element_types = (Type**)safe_calloc((size_t)desc.tuple_element_count, sizeof(Type*));
                for (int i = 0; i < desc.tuple_element_count; i++) {
                    element_types[i] = typechecker_extension_type_from_desc(tc, desc.tuple_element_types[i]);
                }
            }
            type = type_tuple(element_types, desc.tuple_element_count);
            if (element_types) {
                for (int i = 0; i < desc.tuple_element_count; i++) {
                    type_free(element_types[i]);
                }
                free(element_types);
            }
            break;
        }
        case TABLO_EXT_TYPE_MAP:
            type = type_map(type_string(), type_any());
            break;
        case TABLO_EXT_TYPE_CALLBACK: {
            Type** param_types = NULL;
            Type* return_type = NULL;
            if (desc.callback_param_count < 0) {
                typechecker_error(tc, "Extension callback type has an invalid arity", NULL, 0, 0);
                return type_any();
            }
            if (desc.callback_param_count > 0 && !desc.callback_param_types) {
                typechecker_error(tc, "Extension callback type is missing parameter types", NULL, 0, 0);
                return type_any();
            }
            if (!desc.callback_result_type) {
                typechecker_error(tc, "Extension callback type is missing a result type", NULL, 0, 0);
                return type_any();
            }
            if (desc.callback_param_count > 0) {
                param_types = (Type**)safe_calloc((size_t)desc.callback_param_count, sizeof(Type*));
                for (int i = 0; i < desc.callback_param_count; i++) {
                    param_types[i] = typechecker_extension_type_from_desc(tc, desc.callback_param_types[i]);
                }
            }
            return_type = typechecker_extension_type_from_desc(tc, *desc.callback_result_type);
            type = type_function(return_type, param_types, desc.callback_param_count);
            break;
        }
        default:
            typechecker_error(tc, "Extension function uses an unsupported type descriptor", NULL, 0, 0);
            return type_any();
    }

    if (desc.nullable && type && !type->nullable) {
        Type* nullable_type = type_nullable(type);
        type_free(type);
        type = nullable_type;
    }
    return type;
}

static void typechecker_declare_extension_symbols(TypeChecker* tc) {
    if (!tc || !tc->options.extension_registry) return;

    int handle_count = native_extension_registry_handle_type_count(tc->options.extension_registry);
    for (int i = 0; i < handle_count; i++) {
        const char* handle_name = native_extension_registry_handle_type_name(tc->options.extension_registry, i);
        if (!handle_name || handle_name[0] == '\0') {
            typechecker_error(tc, "Extension handle type requires a non-empty name", NULL, 0, 0);
            continue;
        }
        if (is_builtin_name(handle_name)) {
            typechecker_error(tc, "Extension handle type name conflicts with a built-in name", NULL, 0, 0);
            continue;
        }
        if (symbol_table_has(tc->globals, handle_name)) {
            typechecker_error(tc, "Extension handle type name conflicts with an existing symbol", NULL, 0, 0);
            continue;
        }

        Type* handle_type = type_record(handle_name);
        if (handle_type && handle_type->record_def) {
            handle_type->record_def->is_native_opaque = true;
        }
        Symbol* handle_sym = symbol_create(handle_type, handle_name, false);
        symbol_table_add(tc->globals, handle_sym);
    }

    int function_count = native_extension_registry_function_count(tc->options.extension_registry);
    for (int i = 0; i < function_count; i++) {
        const char* function_name = native_extension_registry_function_name(tc->options.extension_registry, i);
        if (!function_name || function_name[0] == '\0') {
            typechecker_error(tc, "Extension function requires a non-empty name", NULL, 0, 0);
            continue;
        }
        if (is_builtin_name(function_name)) {
            typechecker_error(tc, "Extension function name conflicts with a built-in name", NULL, 0, 0);
            continue;
        }
        if (symbol_table_has(tc->globals, function_name)) {
            typechecker_error(tc, "Extension function name conflicts with an existing symbol", NULL, 0, 0);
            continue;
        }

        int param_count = native_extension_registry_function_param_count(tc->options.extension_registry, i);
        Type** param_types = NULL;
        if (param_count > 0) {
            param_types = (Type**)safe_calloc((size_t)param_count, sizeof(Type*));
            for (int j = 0; j < param_count; j++) {
                param_types[j] = typechecker_extension_type_from_desc(
                    tc,
                    native_extension_registry_function_param_type(tc->options.extension_registry, i, j));
            }
        }
        Type* return_type = typechecker_extension_type_from_desc(
            tc,
            native_extension_registry_function_result_type(tc->options.extension_registry, i));
        Type* function_type = type_function(return_type, param_types, param_count);
        Symbol* function_sym = symbol_create(function_type, function_name, false);
        symbol_table_add(tc->globals, function_sym);
    }
}

TypeCheckResult typecheck_with_options(Program* program, TypeCheckOptions options) {
    TypeChecker tc;
    typechecker_init(&tc);
    tc.options = options;
    tc.program = program;
    typechecker_declare_extension_symbols(&tc);

    // Pass 0a: predeclare record types so aliases and signatures can reference them.
    for (int i = 0; i < program->stmt_count; i++) {
        Stmt* stmt = program->statements[i];
        if (stmt && stmt->kind == STMT_RECORD_DECL) {
            tc.current_file = stmt->file;
            typechecker_predeclare_record(&tc, stmt);
        }
    }

    // Pass 0b: predeclare type aliases so signatures can reference them.
    for (int i = 0; i < program->stmt_count; i++) {
        Stmt* stmt = program->statements[i];
        if (stmt && stmt->kind == STMT_ENUM_DECL) {
            tc.current_file = stmt->file;
            typechecker_predeclare_enum(&tc, stmt);
        }
    }

    // Pass 0c: predeclare interfaces so aliases/signatures can reference them.
    for (int i = 0; i < program->stmt_count; i++) {
        Stmt* stmt = program->statements[i];
        if (stmt && stmt->kind == STMT_INTERFACE_DECL) {
            tc.current_file = stmt->file;
            typechecker_predeclare_interface(&tc, stmt);
        }
    }

    // Pass 0d: predeclare type aliases so signatures can reference them.
    for (int i = 0; i < program->stmt_count; i++) {
        Stmt* stmt = program->statements[i];
        if (stmt && stmt->kind == STMT_TYPE_ALIAS) {
            tc.current_file = stmt->file;
            typechecker_predeclare_type_alias(&tc, stmt);
        }
    }

    // Pass 0e: predeclare explicit interface impl mappings.
    for (int i = 0; i < program->stmt_count; i++) {
        Stmt* stmt = program->statements[i];
        if (stmt && stmt->kind == STMT_IMPL_DECL) {
            tc.current_file = stmt->file;
            typechecker_predeclare_impl(&tc, stmt);
        }
    }

    // Pass 1: predeclare top-level functions so they can be referenced before their definition.
    for (int i = 0; i < program->stmt_count; i++) {
        Stmt* stmt = program->statements[i];
        if (stmt && stmt->kind == STMT_FUNC_DECL) {
            tc.current_file = stmt->file;
            typechecker_predeclare_func(&tc, stmt);
        }
    }

    // Pass 1b: refresh predeclared function signatures after generic type resolution.
    for (int i = 0; i < program->stmt_count; i++) {
        Stmt* stmt = program->statements[i];
        if (stmt && stmt->kind == STMT_FUNC_DECL) {
            tc.current_file = stmt->file;
            typechecker_refresh_function_signature(&tc, stmt);
        }
    }

    // Pass 2: typecheck statements (including function bodies).
    for (int i = 0; i < program->stmt_count; i++) {
        tc.current_file = program->statements[i] ? program->statements[i]->file : NULL;
        typecheck_statement(&tc, program->statements[i]);
    }

    tc.current_file = NULL;
    
    TypeCheckResult result;
    result.program = program;
    result.globals = tc.globals;
    result.error = tc.had_error
        ? error_create(ERROR_TYPE,
                       tc.error ? tc.error->message : "Type error",
                       (tc.error && tc.error->file) ? tc.error->file : NULL,
                       tc.error ? tc.error->line : 0,
                       tc.error ? tc.error->column : 0)
        : NULL;

    // Fix 7: Transfer ownership of globals to result before freeing tc
    tc.globals = NULL;
    typechecker_free(&tc);

    return result;
}
