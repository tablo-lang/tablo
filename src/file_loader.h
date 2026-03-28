#ifndef FILE_LOADER_H
#define FILE_LOADER_H

#include "parser.h"
#include "typechecker.h"
#include "compiler.h"
#include <stdbool.h>

typedef struct {
    char* path;
    ParseResult parse_result;
    TypeCheckResult typecheck_result;
    CompileResult compile_result;
    bool is_loaded;
    SymbolTable* exports;
} Module;

typedef struct {
    Module** modules;
    int module_count;
    int module_capacity;
    char** loading_stack;
    int loading_count;
    int loading_capacity;
    SymbolTable* root_symbols;
    Program* root_program;
    Error* error;
    char* sandbox_root;
    char** lock_dep_names;
    int lock_dep_count;
    int lock_dep_capacity;
    bool lock_deps_loaded;
} ModuleLoader;

typedef struct {
    ObjFunction* init_function;
    ObjFunction* main_function;
    SymbolTable* globals;
    ObjFunction** functions;
    int function_count;
    InterfaceDispatchEntry* interface_dispatch_entries;
    int interface_dispatch_count;
    Error* error;
} LoadResult;

ModuleLoader* module_loader_create(const char* sandbox_root);
void module_loader_free(ModuleLoader* loader);
LoadResult module_loader_load_main(ModuleLoader* loader, const char* file_path);
LoadResult module_loader_load_main_with_options(ModuleLoader* loader, const char* file_path, TypeCheckOptions options);
void module_free(Module* mod);

#endif
