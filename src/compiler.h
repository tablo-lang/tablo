#ifndef COMPILER_H
#define COMPILER_H

#include "ast.h"
#include "bytecode.h"
#include "vm.h"
#include "typechecker.h"

typedef struct {
    int loop_start;
    int continue_target;
    bool continue_target_known;

    int* break_jumps;
    int break_count;
    int break_capacity;

    int* continue_jumps;
    int continue_count;
    int continue_capacity;
} LoopContext;

typedef struct {
    Chunk* chunk;
    ObjFunction* function;
    SymbolTable* globals;
    SymbolTable** locals;
    int local_count;
    int local_capacity;
    int depth;
    bool is_top_level;
    bool current_function_is_async;
    bool had_error;
    Error* error;
    char* file;
    VM* vm;

    LoopContext* loop_stack;
    int loop_stack_count;
    int loop_stack_capacity;

    Stmt** record_decls;
    int record_decl_count;

    Stmt** enum_decls;
    int enum_decl_count;

    // Function declarations (AST) for compile-time inlining.
    Stmt** function_decls;
    int function_decl_count;

    // Defer support (Go-style): compile returns via a shared epilogue.
    bool defer_enabled;
    int defer_return_slot;
    int* defer_return_jumps;
    int defer_return_jump_count;
    int defer_return_jump_capacity;

    int* shared_anon_func_counter;
} Compiler;

typedef struct {
    ObjFunction* function;
    SymbolTable* globals;
    ObjFunction** functions;
    int function_count;
    Error* error;
} CompileResult; 

CompileResult compile(Program* program);
void compiler_init(Compiler* comp, SymbolTable* globals, const char* file);
void compiler_free(Compiler* comp);
void peephole_optimize(Chunk* chunk);
ObjFunction** compiler_collect_functions(SymbolTable* table, int* count);

#endif
