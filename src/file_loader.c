#include "file_loader.h"
#include "parser.h"
#include "typechecker.h"
#include "compiler.h"
#include "cJSON.h"
#include "path_utils.h"
#include "safe_alloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#define MAX_PATH_LEN 4096

static bool is_path_sep(char c) {
    return c == '/' || c == '\\';
}

static bool is_safe_path(const char* path) {
    if (!path) return false;

    const char* p = path;
    while (*p) {
        while (*p && is_path_sep(*p)) p++;
        const char* start = p;
        while (*p && !is_path_sep(*p)) p++;
        size_t len = (size_t)(p - start);
        if (len == 0) break;

        if (len == 2 && start[0] == '.' && start[1] == '.') {
            return false;
        }
    }

    return true;
}

static bool path_is_regular_file(const char* path) {
    if (!path || path[0] == '\0') return false;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return false;
    return (st.st_mode & _S_IFREG) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode);
#endif
}

static char* read_file_all_alloc(const char* path) {
    if (!path) return NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    char* data = (char*)safe_malloc((size_t)size + 1);
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(data);
        return NULL;
    }
    data[got] = '\0';
    return data;
}

static InterfaceDispatchEntry* collect_interface_dispatch_entries(Program* program, int* out_count) {
    if (out_count) *out_count = 0;
    if (!program || program->stmt_count <= 0) return NULL;

    InterfaceDispatchEntry* entries = NULL;
    int count = 0;

    for (int i = 0; i < program->stmt_count; i++) {
        Stmt* stmt = program->statements[i];
        if (!stmt || stmt->kind != STMT_IMPL_DECL) continue;
        if (!stmt->impl_decl.interface_name || !stmt->impl_decl.record_name) continue;

        for (int j = 0; j < stmt->impl_decl.method_count; j++) {
            const char* method_name = stmt->impl_decl.method_names ? stmt->impl_decl.method_names[j] : NULL;
            const char* function_name = stmt->impl_decl.function_names ? stmt->impl_decl.function_names[j] : NULL;
            if (!method_name || !function_name) continue;

            count++;
            entries = (InterfaceDispatchEntry*)safe_realloc(entries, (size_t)count * sizeof(InterfaceDispatchEntry));
            InterfaceDispatchEntry* entry = &entries[count - 1];
            entry->interface_name = safe_strdup(stmt->impl_decl.interface_name);
            entry->record_name = safe_strdup(stmt->impl_decl.record_name);
            entry->method_name = safe_strdup(method_name);
            entry->function_name = safe_strdup(function_name);
        }
    }

    if (out_count) *out_count = count;
    return entries;
}

static bool module_loader_lock_dep_push_unique(ModuleLoader* loader, const char* dep_name) {
    if (!loader || !dep_name || dep_name[0] == '\0') return false;
    for (int i = 0; i < loader->lock_dep_count; i++) {
        if (strcmp(loader->lock_dep_names[i], dep_name) == 0) {
            return true;
        }
    }

    if (loader->lock_dep_count >= loader->lock_dep_capacity) {
        int new_capacity = loader->lock_dep_capacity <= 0 ? 8 : (loader->lock_dep_capacity * 2);
        char** resized = (char**)safe_realloc(loader->lock_dep_names, (size_t)new_capacity * sizeof(char*));
        loader->lock_dep_names = resized;
        loader->lock_dep_capacity = new_capacity;
    }

    loader->lock_dep_names[loader->lock_dep_count++] = safe_strdup(dep_name);
    return true;
}

static void module_loader_load_lock_deps(ModuleLoader* loader) {
    if (!loader || loader->lock_deps_loaded) return;
    loader->lock_deps_loaded = true;

    const char* err = NULL;
    char* lock_path = path_sandbox_join_alloc(loader->sandbox_root, "tablo.lock", &err);
    if (!lock_path) return;
    if (!path_is_regular_file(lock_path)) {
        free(lock_path);
        return;
    }

    char* text = read_file_all_alloc(lock_path);
    free(lock_path);
    if (!text) return;

    cJSON* root = cJSON_Parse(text);
    free(text);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }

    cJSON* deps = cJSON_GetObjectItemCaseSensitive(root, "dependencies");
    if (!deps || !cJSON_IsArray(deps)) {
        cJSON_Delete(root);
        return;
    }

    cJSON* dep = NULL;
    cJSON_ArrayForEach(dep, deps) {
        if (!cJSON_IsObject(dep)) continue;
        cJSON* name = cJSON_GetObjectItemCaseSensitive(dep, "name");
        if (!name || !cJSON_IsString(name) || !name->valuestring || name->valuestring[0] == '\0') continue;
        module_loader_lock_dep_push_unique(loader, name->valuestring);
    }

    cJSON_Delete(root);
}

static const char* module_loader_match_lock_dep_prefix(ModuleLoader* loader, const char* import_path) {
    if (!loader || !import_path || import_path[0] == '\0') return NULL;
    module_loader_load_lock_deps(loader);
    if (loader->lock_dep_count <= 0) return NULL;

    const char* best = NULL;
    size_t best_len = 0;
    size_t import_len = strlen(import_path);

    for (int i = 0; i < loader->lock_dep_count; i++) {
        const char* dep_name = loader->lock_dep_names[i];
        if (!dep_name || dep_name[0] == '\0') continue;
        size_t dep_len = strlen(dep_name);
        if (dep_len > import_len) continue;
        if (strncmp(import_path, dep_name, dep_len) != 0) continue;
        if (import_path[dep_len] != '\0' && !is_path_sep(import_path[dep_len])) continue;
        if (dep_len > best_len) {
            best = dep_name;
            best_len = dep_len;
        }
    }

    return best;
}

static char* try_resolve_import_via_vendor(ModuleLoader* loader, const char* import_path) {
    if (!loader || !import_path) return NULL;
    if (import_path[0] == '.' || is_path_sep(import_path[0])) return NULL;

    const char* dep_name = module_loader_match_lock_dep_prefix(loader, import_path);
    if (!dep_name) return NULL;
    (void)dep_name;

    size_t import_len = strlen(import_path);
    size_t joined_len = strlen("vendor/") + import_len + 1;
    if (joined_len > MAX_PATH_LEN) return NULL;

    char* joined = (char*)safe_malloc(joined_len);
    memcpy(joined, "vendor/", strlen("vendor/"));
    memcpy(joined + strlen("vendor/"), import_path, import_len);
    joined[joined_len - 1] = '\0';

    const char* err = NULL;
    char* resolved = path_sandbox_resolve_alloc(loader->sandbox_root, joined, false, &err);
    free(joined);
    if (!resolved) return NULL;

    if (!path_is_regular_file(resolved)) {
        free(resolved);
        return NULL;
    }

    return resolved;
}

static void module_loader_push_loading(ModuleLoader* loader, const char* file_path) {
    loader->loading_count++;
    if (loader->loading_count > loader->loading_capacity) {
        int old_capacity = loader->loading_capacity;
        loader->loading_capacity = loader->loading_count * 2;
        loader->loading_stack = (char**)safe_realloc(loader->loading_stack, loader->loading_capacity * sizeof(char*));
        for (int i = old_capacity; i < loader->loading_capacity; i++) {
            loader->loading_stack[i] = NULL;
        }
    }
    loader->loading_stack[loader->loading_count - 1] = safe_strdup(file_path);
}

static void module_loader_pop_loading(ModuleLoader* loader) {
    if (loader->loading_count <= 0) return;
    char* top = loader->loading_stack[loader->loading_count - 1];
    if (top) free(top);
    loader->loading_stack[loader->loading_count - 1] = NULL;
    loader->loading_count--;
}

static char* resolve_import_path(ModuleLoader* loader, const char* importer_path, const Stmt* import_stmt) {
    if (!loader || !loader->sandbox_root || !importer_path || !import_stmt || !import_stmt->import_path) {
        return NULL;
    }

    const char* import_path = import_stmt->import_path;
    if (!import_path || import_path[0] == '\0') {
        loader->error = error_create(ERROR_IMPORT, "Empty import path", import_stmt->file, import_stmt->line, import_stmt->column);
        return NULL;
    }

    if (path_is_absolute(import_path)) {
        loader->error = error_create(ERROR_IMPORT, "Absolute import paths are not allowed", import_stmt->file, import_stmt->line, import_stmt->column);
        return NULL;
    }
#ifdef _WIN32
    if (strchr(import_path, ':') != NULL) {
        loader->error = error_create(ERROR_IMPORT, "Drive paths are not allowed in imports", import_stmt->file, import_stmt->line, import_stmt->column);
        return NULL;
    }
#endif

    char* importer_dir = path_dirname_alloc(importer_path);
    if (!importer_dir) {
        loader->error = error_create(ERROR_IMPORT, "Failed to resolve import base directory", import_stmt->file, import_stmt->line, import_stmt->column);
        return NULL;
    }

    const char* root = loader->sandbox_root;
    size_t root_len = strlen(root);
    const char* rel = "";
    if (strncmp(importer_dir, root, root_len) == 0) {
        rel = importer_dir + root_len;
        while (*rel && is_path_sep(*rel)) rel++;
    } else {
        free(importer_dir);
        loader->error = error_create(ERROR_IMPORT, "Import base escapes sandbox", import_stmt->file, import_stmt->line, import_stmt->column);
        return NULL;
    }

    size_t rel_len = strlen(rel);
    size_t import_len = strlen(import_path);
    size_t joined_len = rel_len + (rel_len ? 1 : 0) + import_len + 1;
    if (joined_len > MAX_PATH_LEN) {
        free(importer_dir);
        loader->error = error_create(ERROR_IMPORT, "Import path too long", import_stmt->file, import_stmt->line, import_stmt->column);
        return NULL;
    }

    char* joined = (char*)safe_malloc(joined_len);
    if (rel_len) {
        memcpy(joined, rel, rel_len);
        joined[rel_len] = '/';
        memcpy(joined + rel_len + 1, import_path, import_len);
        joined[rel_len + 1 + import_len] = '\0';
    } else {
        memcpy(joined, import_path, import_len);
        joined[import_len] = '\0';
    }

    free(importer_dir);

    const char* err = NULL;
    char* resolved = path_sandbox_resolve_alloc(root, joined, false, &err);
    free(joined);
    if (!resolved) {
        char* via_vendor = try_resolve_import_via_vendor(loader, import_path);
        if (via_vendor) {
            return via_vendor;
        }
        loader->error = error_create(ERROR_IMPORT, err ? err : "Import path not allowed", import_stmt->file, import_stmt->line, import_stmt->column);
        return NULL;
    }

    if (path_is_regular_file(resolved)) {
        return resolved;
    }

    char* via_vendor = try_resolve_import_via_vendor(loader, import_path);
    if (via_vendor) {
        free(resolved);
        return via_vendor;
    }

    free(resolved);
    loader->error = error_create(ERROR_IMPORT, "Import path is not a regular file", import_stmt->file, import_stmt->line, import_stmt->column);
    return NULL;
}

ModuleLoader* module_loader_create(const char* sandbox_root) {
    ModuleLoader* loader = (ModuleLoader*)safe_malloc(sizeof(ModuleLoader));
    loader->modules = NULL;
    loader->module_count = 0;
    loader->module_capacity = 0;
    loader->loading_stack = NULL;
    loader->loading_count = 0;
    loader->loading_capacity = 0;
    loader->root_symbols = symbol_table_create();
    loader->root_program = NULL;
    loader->error = NULL;
    loader->sandbox_root = sandbox_root ? safe_strdup(sandbox_root) : NULL;
    loader->lock_dep_names = NULL;
    loader->lock_dep_count = 0;
    loader->lock_dep_capacity = 0;
    loader->lock_deps_loaded = false;
    return loader;
}

void module_free(Module* mod) {
    if (!mod) return;
    if (mod->path) free(mod->path);
    if (mod->is_loaded) {
        program_free(mod->parse_result.program);
        error_free(mod->parse_result.error);
        error_free(mod->typecheck_result.error);
        error_free(mod->compile_result.error);
        // Fix 5: Only free if not transferred to LoadResult
        if (mod->compile_result.function) obj_function_free(mod->compile_result.function);
        if (mod->compile_result.globals) symbol_table_free(mod->compile_result.globals);
        if (mod->compile_result.functions) {
            for (int i = 0; i < mod->compile_result.function_count; i++) {
                if (mod->compile_result.functions[i])
                    obj_function_release(mod->compile_result.functions[i]);
            }
            free(mod->compile_result.functions);
        }
    }
    if (mod->exports) symbol_table_free(mod->exports);
    free(mod);
}

void module_loader_free(ModuleLoader* loader) {
    if (!loader) return;

    for (int i = 0; i < loader->module_count; i++) {
        module_free(loader->modules[i]);
    }
    if (loader->modules) free(loader->modules);
    if (loader->loading_stack) {
        while (loader->loading_count > 0) {
            module_loader_pop_loading(loader);
        }
        free(loader->loading_stack);
        loader->loading_stack = NULL;
        loader->loading_capacity = 0;
    }
    symbol_table_free(loader->root_symbols);
    // Fix 4: Don't free root_program here; module_free already handles it
    loader->root_program = NULL;
    // Fix 6: Only free error if not transferred
    error_free(loader->error);
    if (loader->sandbox_root) free(loader->sandbox_root);
    if (loader->lock_dep_names) {
        for (int i = 0; i < loader->lock_dep_count; i++) {
            free(loader->lock_dep_names[i]);
        }
        free(loader->lock_dep_names);
    }
    free(loader);
}

static bool is_loading(ModuleLoader* loader, const char* path) {
    for (int i = 0; i < loader->loading_count; i++) {
        if (strcmp(loader->loading_stack[i], path) == 0) {
            return true;
        }
    }
    return false;
}

static Module* module_loader_load_module(ModuleLoader* loader, const char* file_path) {
    for (int i = 0; i < loader->module_count; i++) {
        if (strcmp(loader->modules[i]->path, file_path) == 0) {
            return loader->modules[i];
        }
    }
    
    if (is_loading(loader, file_path)) {
        loader->error = error_create(ERROR_IMPORT, "Circular import detected", file_path, 0, 0);
        return NULL;
    }

    module_loader_push_loading(loader, file_path);
    
    if (!is_safe_path(file_path)) {
        loader->error = error_create(ERROR_IMPORT, "Path traversal not allowed", file_path, 0, 0);
        module_loader_pop_loading(loader);
        return NULL;
    }
    
    char* source = read_file_all_alloc(file_path);
    if (!source) {
        loader->error = error_create(ERROR_IMPORT, "Failed to read file", file_path, 0, 0);
        module_loader_pop_loading(loader);
        return NULL;
    }
    
    ParseResult parse_result = parser_parse(source, file_path);
    free(source);
    
    if (parse_result.error) {
        loader->error = parse_result.error;
        // Ensure we don't double-free when freeing the parse result program.
        parse_result.error = NULL;
        parser_free_parse_only_result(&parse_result);
        module_loader_pop_loading(loader);
        return NULL;
    }

    // Load imports before registering the module, so imports appear before importers.
    for (int i = 0; i < parse_result.program->stmt_count; i++) {
        Stmt* s = parse_result.program->statements[i];
        if (s && s->kind == STMT_IMPORT) {
            char* resolved = resolve_import_path(loader, file_path, s);
            if (!resolved) {
                parser_free_parse_only_result(&parse_result);
                module_loader_pop_loading(loader);
                return NULL;
            }

            Module* imported = module_loader_load_module(loader, resolved);
            free(resolved);
            if (!imported) {
                parser_free_parse_only_result(&parse_result);
                module_loader_pop_loading(loader);
                return NULL;
            }
        }
    }
    
    Module* mod = (Module*)safe_malloc(sizeof(Module));
    mod->path = safe_strdup(file_path);
    mod->parse_result = parse_result;
    mod->typecheck_result.program = NULL;
    mod->typecheck_result.globals = NULL;
    mod->typecheck_result.error = NULL;
    mod->compile_result.function = NULL;
    mod->compile_result.globals = NULL;
    mod->compile_result.functions = NULL;
    mod->compile_result.function_count = 0;
    mod->compile_result.error = NULL;
    mod->is_loaded = true;
    mod->exports = symbol_table_create();
    
    loader->module_count++;
    if (loader->module_count > loader->module_capacity) {
        loader->module_capacity = loader->module_count * 2;
        loader->modules = (Module**)safe_realloc(loader->modules, loader->module_capacity * sizeof(Module*));
    }
    loader->modules[loader->module_count - 1] = mod;
    
    module_loader_pop_loading(loader);
    
    return mod;
}

LoadResult module_loader_load_main(ModuleLoader* loader, const char* file_path) {
    TypeCheckOptions options = {0};
    options.report_diagnostics = true;
    return module_loader_load_main_with_options(loader, file_path, options);
}

LoadResult module_loader_load_main_with_options(ModuleLoader* loader, const char* file_path, TypeCheckOptions options) {

    LoadResult result;
    result.init_function = NULL;
    result.main_function = NULL;
    result.globals = NULL;
    result.functions = NULL;
    result.function_count = 0;
    result.interface_dispatch_entries = NULL;
    result.interface_dispatch_count = 0;
    result.error = NULL;

    Module* mod = module_loader_load_module(loader, file_path);
    if (!mod) {
        result.error = loader->error;
        // Fix 6: NULL out loader->error after transferring to result
        loader->error = NULL;
        return result;
    }

    loader->root_program = mod->parse_result.program;

    // Disallow defining main() in imported modules.
    for (int i = 0; i < loader->module_count; i++) {
        Module* m = loader->modules[i];
        if (!m) {
            continue;
        }
        if (!m->parse_result.program) {
            continue;
        }
        if (strcmp(m->path, file_path) == 0) {
            continue;
        }

        for (int s = 0; s < m->parse_result.program->stmt_count; s++) {
            Stmt* st = m->parse_result.program->statements[s];
            if (st && st->kind == STMT_FUNC_DECL && st->func_decl.name && strcmp(st->func_decl.name, "main") == 0) {
                result.error = error_create(ERROR_IMPORT, "Imported modules must not define main()", st->file, st->line, st->column);
                return result;
            }
        }
    }

    // Build a combined program in load order (imports first), then typecheck+compile once.
    Program* combined = program_create(file_path);
    for (int i = 0; i < loader->module_count; i++) {
        Module* m = loader->modules[i];
        if (!m) {
            continue;
        }
        if (!m->parse_result.program) {
            continue;
        }
        // Clone statements instead of transferring ownership
        for (int s = 0; s < m->parse_result.program->stmt_count; s++) {
            Stmt* stmt = m->parse_result.program->statements[s];
            // Deep clone the statement so both module and combined program have their own copy
            Stmt* cloned_stmt = stmt_clone(stmt);
            if (!cloned_stmt) {
                // Handle clone failure - this shouldn't happen with valid statements
                fprintf(stderr, "ERROR: Failed to clone statement %d\n", s);
                // Clean up already cloned statements
                for (int j = 0; j < combined->stmt_count; j++) {
                    stmt_free(combined->statements[j]);
                }
                free(combined->statements);
                combined->statements = NULL;
                combined->stmt_count = 0;
                program_free(combined);
                result.error = error_create(ERROR_IMPORT, "Failed to clone statement", file_path, 0, 0);
                return result;
            }
            program_add_stmt(combined, cloned_stmt);
        }
        // Don't free module's statements - let module_free handle it normally
        // The combined program now has its own independent copies
    }

    TypeCheckResult tc = typecheck_with_options(combined, options);
    if (tc.error) {
        result.error = tc.error;
        // Prevent double-free: error transferred to result.
        tc.error = NULL;
        if (tc.globals) symbol_table_free(tc.globals);
        program_free(combined);
        return result;
    }
    SymbolTable* typecheck_globals = tc.globals;
    tc.globals = NULL;

    CompileResult cr = compile(combined);
    if (typecheck_globals) {
        symbol_table_free(typecheck_globals);
        typecheck_globals = NULL;
    }
    if (cr.error) {
        program_free(combined);
        result.error = cr.error;
        // Prevent module_free from freeing this error; it isn't stored in any module.
        cr.error = NULL;
        if (cr.globals) symbol_table_free(cr.globals);
        if (cr.functions) free(cr.functions);
        if (cr.function) obj_function_free(cr.function);
        return result;
    }

    Symbol* main_sym = symbol_table_get(cr.globals, "main");
    if (!main_sym) {
        program_free(combined);
        result.error = error_create(ERROR_IMPORT, "No main() function found", file_path, 0, 0);
        if (cr.globals) symbol_table_free(cr.globals);
        if (cr.functions) free(cr.functions);
        if (cr.function) obj_function_free(cr.function);
        return result;
    }

    if (!main_sym->type || main_sym->type->kind != TYPE_FUNCTION) {
        program_free(combined);
        result.error = error_create(ERROR_IMPORT, "main must be a function", file_path, 0, 0);
        if (cr.globals) symbol_table_free(cr.globals);
        if (cr.functions) free(cr.functions);
        if (cr.function) obj_function_free(cr.function);
        return result;
    }

    if (main_sym->type->param_count != 0) {
        program_free(combined);
        result.error = error_create(ERROR_IMPORT, "main must not take parameters", file_path, 0, 0);
        if (cr.globals) symbol_table_free(cr.globals);
        if (cr.functions) free(cr.functions);
        if (cr.function) obj_function_free(cr.function);
        return result;
    }

    if (main_sym->type->kind != TYPE_FUNCTION ||
        !main_sym->type->return_type ||
        main_sym->type->return_type->kind != TYPE_VOID) {
        program_free(combined);
        result.error = error_create(ERROR_IMPORT, "main must return void", file_path, 0, 0);
        if (cr.globals) symbol_table_free(cr.globals);
        if (cr.functions) free(cr.functions);
        if (cr.function) obj_function_free(cr.function);
        return result;
    }

    result.interface_dispatch_entries = collect_interface_dispatch_entries(combined,
                                                                           &result.interface_dispatch_count);
    program_free(combined);

    result.main_function = (ObjFunction*)main_sym->function_obj;
    result.globals = cr.globals;
    result.functions = cr.functions;
    result.function_count = cr.function_count;

    // The returned init_function executes module-level statements (e.g. global initializers)
    // before main().
    result.init_function = cr.function;

    return result;
}
