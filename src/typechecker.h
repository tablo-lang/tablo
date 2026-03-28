#ifndef TYPECHECKER_H
#define TYPECHECKER_H

#include "ast.h"

typedef struct NativeExtensionRegistry NativeExtensionRegistry;

typedef struct {
    bool warn_unused_error;
    bool strict_errors;
    bool report_diagnostics;
    NativeExtensionRegistry* extension_registry;
} TypeCheckOptions;

typedef struct {
    Symbol** symbols;
    int symbol_count;
    int symbol_capacity;
} SymbolTable;

typedef struct {
    struct Expr* expr;
    int outer_local_count;
    char** names;
    int name_count;
    int name_capacity;
} FuncLiteralCaptureContext;

typedef struct {
    char* interface_name;
    char* record_name;
    char** method_names;
    char** function_names;
    int method_count;
    const char* decl_file;
    int decl_line;
    int decl_column;
} InterfaceImplMapping;

typedef struct GenericRecordDeclEntry GenericRecordDeclEntry;
typedef struct GenericTypeAliasDeclEntry GenericTypeAliasDeclEntry;
typedef struct GenericEnumDeclEntry GenericEnumDeclEntry;
typedef struct GenericRecordInstanceEntry GenericRecordInstanceEntry;
typedef struct GenericEnumInstanceEntry GenericEnumInstanceEntry;

typedef struct {
    SymbolTable* globals;
    SymbolTable** locals;
    int local_count;
    int local_capacity;
    bool had_error;
    Error* error;
    char* current_function;
    bool current_function_is_async;
    Type* current_return_type;
    char** current_type_param_names;
    Type** current_type_param_constraints;
    int current_type_param_count;
    const char* current_file;
    Program* program;
    int local_lookup_floor;
    FuncLiteralCaptureContext* capture_contexts;
    int capture_context_count;
    int capture_context_capacity;
    InterfaceImplMapping* impl_mappings;
    int impl_mapping_count;
    int impl_mapping_capacity;
    GenericRecordDeclEntry* generic_record_decls;
    int generic_record_decl_count;
    int generic_record_decl_capacity;
    GenericTypeAliasDeclEntry* generic_type_alias_decls;
    int generic_type_alias_decl_count;
    int generic_type_alias_decl_capacity;
    GenericEnumDeclEntry* generic_enum_decls;
    int generic_enum_decl_count;
    int generic_enum_decl_capacity;
    GenericRecordInstanceEntry* generic_record_instances;
    int generic_record_instance_count;
    int generic_record_instance_capacity;
    GenericEnumInstanceEntry* generic_enum_instances;
    int generic_enum_instance_count;
    int generic_enum_instance_capacity;
    Type* expected_expr_type;
    TypeCheckOptions options;
} TypeChecker;

typedef struct {
    Program* program;
    SymbolTable* globals;
    Error* error;
} TypeCheckResult;

TypeCheckResult typecheck(Program* program);
TypeCheckResult typecheck_with_options(Program* program, TypeCheckOptions options);
SymbolTable* symbol_table_create(void);
void symbol_table_free(SymbolTable* table);
void symbol_table_add(SymbolTable* table, Symbol* sym);
Symbol* symbol_table_get(SymbolTable* table, const char* name);
bool symbol_table_has(SymbolTable* table, const char* name);
void typechecker_init(TypeChecker* tc);
void typechecker_free(TypeChecker* tc);

#endif
