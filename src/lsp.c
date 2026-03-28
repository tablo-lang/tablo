#include "lsp.h"

#include "cli.h"
#include "cJSON.h"
#include "parser.h"
#include "safe_alloc.h"
#include "typechecker.h"
#include "types.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TABLO_VERSION
#define TABLO_VERSION "dev"
#endif

enum {
    LSP_SYMBOL_KIND_CLASS = 5,
    LSP_SYMBOL_KIND_METHOD = 6,
    LSP_SYMBOL_KIND_FIELD = 8,
    LSP_SYMBOL_KIND_ENUM = 10,
    LSP_SYMBOL_KIND_INTERFACE = 11,
    LSP_SYMBOL_KIND_FUNCTION = 12,
    LSP_SYMBOL_KIND_VARIABLE = 13,
    LSP_SYMBOL_KIND_CONSTANT = 14,
    LSP_SYMBOL_KIND_ENUM_MEMBER = 22,
    LSP_SYMBOL_KIND_STRUCT = 23
};

typedef struct {
    char* uri;
    char* file_path;
    char* text;
} LspOpenDocument;

typedef struct {
    LspOpenDocument* docs;
    int count;
    int capacity;
} LspDocumentStore;

typedef enum {
    LSP_BINDING_PARAM,
    LSP_BINDING_VAR,
    LSP_BINDING_CONST,
    LSP_BINDING_FOREACH,
    LSP_BINDING_FOR_RANGE,
    LSP_BINDING_TYPE_PARAM
} LspBindingKind;

typedef struct LspBinding {
    const char* name;
    Type* type;
    int line;
    int column;
    LspBindingKind kind;
    const struct LspBinding* next;
} LspBinding;

typedef enum {
    LSP_RESOLVE_HOVER,
    LSP_RESOLVE_DEFINITION
} LspResolveMode;

static Error* lsp_error_clone(const Error* err) {
    if (!err) return NULL;
    return error_create(err->code,
                        err->message ? err->message : "",
                        err->file ? err->file : "",
                        err->line,
                        err->column);
}

static Error* lsp_make_error(ErrorCode code,
                             const char* message,
                             const char* file,
                             int line,
                             int column) {
    return error_create(code,
                        message ? message : "",
                        file ? file : "",
                        line,
                        column);
}

static void lsp_document_store_init(LspDocumentStore* store) {
    if (!store) return;
    store->docs = NULL;
    store->count = 0;
    store->capacity = 0;
}

static void lsp_document_store_free(LspDocumentStore* store) {
    if (!store) return;
    for (int i = 0; i < store->count; i++) {
        free(store->docs[i].uri);
        free(store->docs[i].file_path);
        free(store->docs[i].text);
    }
    free(store->docs);
    store->docs = NULL;
    store->count = 0;
    store->capacity = 0;
}

static int lsp_document_store_find(const LspDocumentStore* store, const char* uri) {
    if (!store || !uri) return -1;
    for (int i = 0; i < store->count; i++) {
        if (store->docs[i].uri && strcmp(store->docs[i].uri, uri) == 0) {
            return i;
        }
    }
    return -1;
}

static void lsp_document_store_set(LspDocumentStore* store,
                                   const char* uri,
                                   const char* file_path,
                                   const char* text) {
    int index = -1;
    if (!store || !uri || !file_path || !text) return;

    index = lsp_document_store_find(store, uri);
    if (index >= 0) {
        free(store->docs[index].file_path);
        free(store->docs[index].text);
        store->docs[index].file_path = safe_strdup(file_path);
        store->docs[index].text = safe_strdup(text);
        return;
    }

    if (store->count >= store->capacity) {
        store->capacity = store->capacity > 0 ? store->capacity * 2 : 4;
        store->docs = (LspOpenDocument*)safe_realloc(store->docs,
                                                     sizeof(LspOpenDocument) * (size_t)store->capacity);
    }

    store->docs[store->count].uri = safe_strdup(uri);
    store->docs[store->count].file_path = safe_strdup(file_path);
    store->docs[store->count].text = safe_strdup(text);
    store->count++;
}

static void lsp_document_store_remove(LspDocumentStore* store, const char* uri) {
    int index = -1;
    if (!store || !uri) return;

    index = lsp_document_store_find(store, uri);
    if (index < 0) return;

    free(store->docs[index].uri);
    free(store->docs[index].file_path);
    free(store->docs[index].text);

    for (int i = index + 1; i < store->count; i++) {
        store->docs[i - 1] = store->docs[i];
    }
    store->count--;
}

static const LspOpenDocument* lsp_document_store_get(const LspDocumentStore* store, const char* uri) {
    int index = lsp_document_store_find(store, uri);
    if (index < 0) return NULL;
    return &store->docs[index];
}

static char* lsp_read_file_text(const char* file_path, Error** out_error) {
    FILE* f = NULL;
    long size = 0;
    size_t read_size = 0;
    char* data = NULL;

    if (out_error) *out_error = NULL;
    if (!file_path || file_path[0] == '\0') {
        if (out_error) {
            *out_error = lsp_make_error(ERROR_IMPORT, "Missing source file path", NULL, 0, 0);
        }
        return NULL;
    }

    f = fopen(file_path, "rb");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to read '%s': %s", file_path, strerror(errno));
        if (out_error) *out_error = lsp_make_error(ERROR_IMPORT, msg, file_path, 0, 0);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to seek '%s'", file_path);
        fclose(f);
        if (out_error) *out_error = lsp_make_error(ERROR_IMPORT, msg, file_path, 0, 0);
        return NULL;
    }

    size = ftell(f);
    if (size < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to size '%s'", file_path);
        fclose(f);
        if (out_error) *out_error = lsp_make_error(ERROR_IMPORT, msg, file_path, 0, 0);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to rewind '%s'", file_path);
        fclose(f);
        if (out_error) *out_error = lsp_make_error(ERROR_IMPORT, msg, file_path, 0, 0);
        return NULL;
    }

    data = (char*)safe_malloc((size_t)size + 1);
    read_size = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[read_size] = '\0';
    return data;
}

static void lsp_append_text(char* buffer, size_t capacity, size_t* offset, const char* text) {
    size_t written = 0;
    if (!buffer || !offset || capacity == 0 || !text) return;
    if (*offset >= capacity - 1) return;
    written = (size_t)snprintf(buffer + *offset, capacity - *offset, "%s", text);
    if (written >= capacity - *offset) {
        *offset = capacity - 1;
    } else {
        *offset += written;
    }
}

static void lsp_type_to_source_string(Type* type, char* buffer, size_t buffer_size) {
    if (!type || !buffer || buffer_size == 0) return;
    buffer[0] = '\0';

    switch (type->kind) {
        case TYPE_INT: snprintf(buffer, buffer_size, "int%s", type->nullable ? "?" : ""); break;
        case TYPE_BOOL: snprintf(buffer, buffer_size, "bool%s", type->nullable ? "?" : ""); break;
        case TYPE_DOUBLE: snprintf(buffer, buffer_size, "double%s", type->nullable ? "?" : ""); break;
        case TYPE_BIGINT: snprintf(buffer, buffer_size, "bigint%s", type->nullable ? "?" : ""); break;
        case TYPE_STRING: snprintf(buffer, buffer_size, "string%s", type->nullable ? "?" : ""); break;
        case TYPE_BYTES: snprintf(buffer, buffer_size, "bytes%s", type->nullable ? "?" : ""); break;
        case TYPE_NIL: snprintf(buffer, buffer_size, "nil"); break;
        case TYPE_VOID: snprintf(buffer, buffer_size, "void"); break;
        case TYPE_ANY: snprintf(buffer, buffer_size, "any"); break;
        case TYPE_TYPE_PARAM:
            snprintf(buffer,
                     buffer_size,
                     "%s%s",
                     type->type_param_name ? type->type_param_name : "T",
                     type->nullable ? "?" : "");
            break;
        case TYPE_ARRAY: {
            char elem_buf[256];
            lsp_type_to_source_string(type->element_type, elem_buf, sizeof(elem_buf));
            snprintf(buffer, buffer_size, "array<%s>%s", elem_buf, type->nullable ? "?" : "");
            break;
        }
        case TYPE_FUTURE: {
            char elem_buf[256];
            lsp_type_to_source_string(type->element_type, elem_buf, sizeof(elem_buf));
            snprintf(buffer, buffer_size, "Future<%s>%s", elem_buf, type->nullable ? "?" : "");
            break;
        }
        case TYPE_FUNCTION: {
            char params[512] = "";
            char return_buf[256] = "";
            for (int i = 0; i < type->param_count; i++) {
                char param_buf[128];
                lsp_type_to_source_string(type->param_types[i], param_buf, sizeof(param_buf));
                if (i > 0) strncat(params, ", ", sizeof(params) - strlen(params) - 1);
                strncat(params, param_buf, sizeof(params) - strlen(params) - 1);
            }
            lsp_type_to_source_string(type->return_type, return_buf, sizeof(return_buf));
            snprintf(buffer, buffer_size, "func(%s): %s", params, return_buf);
            break;
        }
        case TYPE_RECORD: {
            char type_args[512] = "";
            const char* name = (type->record_def && type->record_def->name) ? type->record_def->name : NULL;
            if (type->param_count > 0 && type->param_types) {
                strncat(type_args, "[", sizeof(type_args) - strlen(type_args) - 1);
                for (int i = 0; i < type->param_count; i++) {
                    char arg_buf[128];
                    lsp_type_to_source_string(type->param_types[i], arg_buf, sizeof(arg_buf));
                    if (i > 0) strncat(type_args, ", ", sizeof(type_args) - strlen(type_args) - 1);
                    strncat(type_args, arg_buf, sizeof(type_args) - strlen(type_args) - 1);
                }
                strncat(type_args, "]", sizeof(type_args) - strlen(type_args) - 1);
            }
            if (!name) name = "record";
            snprintf(buffer, buffer_size, "%s%s%s", name, type_args, type->nullable ? "?" : "");
            break;
        }
        case TYPE_INTERFACE: {
            const char* name = (type->interface_def && type->interface_def->name) ? type->interface_def->name : "interface";
            snprintf(buffer, buffer_size, "%s%s", name, type->nullable ? "?" : "");
            break;
        }
        case TYPE_TUPLE: {
            char elems[512] = "";
            for (int i = 0; i < tuple_type_get_arity(type); i++) {
                char elem_buf[128];
                lsp_type_to_source_string(tuple_type_get_element(type, i), elem_buf, sizeof(elem_buf));
                if (i > 0) strncat(elems, ", ", sizeof(elems) - strlen(elems) - 1);
                strncat(elems, elem_buf, sizeof(elems) - strlen(elems) - 1);
            }
            snprintf(buffer, buffer_size, "(%s)%s", elems, type->nullable ? "?" : "");
            break;
        }
        case TYPE_MAP: {
            char key_buf[128];
            char val_buf[128];
            lsp_type_to_source_string(type->map_def->key_type, key_buf, sizeof(key_buf));
            lsp_type_to_source_string(type->map_def->value_type, val_buf, sizeof(val_buf));
            snprintf(buffer, buffer_size, "map<%s, %s>%s", key_buf, val_buf, type->nullable ? "?" : "");
            break;
        }
        case TYPE_SET: {
            char elem_buf[128];
            lsp_type_to_source_string(type->set_def->element_type, elem_buf, sizeof(elem_buf));
            snprintf(buffer, buffer_size, "set<%s>%s", elem_buf, type->nullable ? "?" : "");
            break;
        }
        default:
            snprintf(buffer, buffer_size, "unknown");
            break;
    }
}

static void lsp_append_type_string(char* buffer, size_t capacity, size_t* offset, Type* type) {
    char type_buf[512];
    if (!type) return;
    lsp_type_to_source_string(type, type_buf, sizeof(type_buf));
    lsp_append_text(buffer, capacity, offset, type_buf);
}

static void lsp_append_type_param_list(char* buffer,
                                       size_t capacity,
                                       size_t* offset,
                                       char** type_params,
                                       int type_param_count) {
    if (!buffer || !offset || !type_params || type_param_count <= 0) return;
    lsp_append_text(buffer, capacity, offset, "[");
    for (int i = 0; i < type_param_count; i++) {
        if (i > 0) lsp_append_text(buffer, capacity, offset, ", ");
        lsp_append_text(buffer, capacity, offset, type_params[i] ? type_params[i] : "_");
    }
    lsp_append_text(buffer, capacity, offset, "]");
}

static char* lsp_build_var_detail(const Stmt* stmt) {
    char buf[1024];
    size_t offset = 0;
    const char* prefix = (stmt && stmt->kind == STMT_VAR_DECL && stmt->var_decl.is_mutable) ? "var " : "const ";

    buf[0] = '\0';
    if (!stmt || stmt->kind != STMT_VAR_DECL) return safe_strdup("");

    lsp_append_text(buf, sizeof(buf), &offset, prefix);
    lsp_append_text(buf, sizeof(buf), &offset, stmt->var_decl.name ? stmt->var_decl.name : "_");
    if (stmt->var_decl.type_annotation) {
        lsp_append_text(buf, sizeof(buf), &offset, ": ");
        lsp_append_type_string(buf, sizeof(buf), &offset, stmt->var_decl.type_annotation);
    }
    return safe_strdup(buf);
}

static char* lsp_build_type_alias_detail(const Stmt* stmt) {
    char buf[1024];
    size_t offset = 0;

    buf[0] = '\0';
    if (!stmt || stmt->kind != STMT_TYPE_ALIAS) return safe_strdup("");

    lsp_append_text(buf, sizeof(buf), &offset, "type ");
    lsp_append_text(buf, sizeof(buf), &offset, stmt->type_alias.name ? stmt->type_alias.name : "_");
    lsp_append_type_param_list(buf,
                               sizeof(buf),
                               &offset,
                               stmt->type_alias.type_params,
                               stmt->type_alias.type_param_count);
    lsp_append_text(buf, sizeof(buf), &offset, " = ");
    lsp_append_type_string(buf, sizeof(buf), &offset, stmt->type_alias.target_type);
    return safe_strdup(buf);
}

static char* lsp_build_record_detail(const Stmt* stmt) {
    char buf[512];
    size_t offset = 0;

    buf[0] = '\0';
    if (!stmt || stmt->kind != STMT_RECORD_DECL) return safe_strdup("");

    lsp_append_text(buf, sizeof(buf), &offset, "record ");
    lsp_append_text(buf, sizeof(buf), &offset, stmt->record_decl.name ? stmt->record_decl.name : "_");
    lsp_append_type_param_list(buf,
                               sizeof(buf),
                               &offset,
                               stmt->record_decl.type_params,
                               stmt->record_decl.type_param_count);
    return safe_strdup(buf);
}

static char* lsp_build_interface_detail(const Stmt* stmt) {
    char buf[512];
    size_t offset = 0;

    buf[0] = '\0';
    if (!stmt || stmt->kind != STMT_INTERFACE_DECL) return safe_strdup("");

    lsp_append_text(buf, sizeof(buf), &offset, "interface ");
    lsp_append_text(buf, sizeof(buf), &offset, stmt->interface_decl.name ? stmt->interface_decl.name : "_");
    return safe_strdup(buf);
}

static char* lsp_build_enum_detail(const Stmt* stmt) {
    char buf[512];
    size_t offset = 0;

    buf[0] = '\0';
    if (!stmt || stmt->kind != STMT_ENUM_DECL) return safe_strdup("");

    lsp_append_text(buf, sizeof(buf), &offset, "enum ");
    lsp_append_text(buf, sizeof(buf), &offset, stmt->enum_decl.name ? stmt->enum_decl.name : "_");
    lsp_append_type_param_list(buf,
                               sizeof(buf),
                               &offset,
                               stmt->enum_decl.type_params,
                               stmt->enum_decl.type_param_count);
    return safe_strdup(buf);
}

static char* lsp_build_function_detail(const Stmt* stmt) {
    char buf[2048];
    size_t offset = 0;

    buf[0] = '\0';
    if (!stmt || stmt->kind != STMT_FUNC_DECL) return safe_strdup("");

    if (stmt->func_decl.is_async) {
        lsp_append_text(buf, sizeof(buf), &offset, "async ");
    }
    lsp_append_text(buf, sizeof(buf), &offset, "func ");
    lsp_append_text(buf, sizeof(buf), &offset, stmt->func_decl.name ? stmt->func_decl.name : "_");
    lsp_append_type_param_list(buf,
                               sizeof(buf),
                               &offset,
                               stmt->func_decl.type_params,
                               stmt->func_decl.type_param_count);
    lsp_append_text(buf, sizeof(buf), &offset, "(");
    for (int i = 0; i < stmt->func_decl.param_count; i++) {
        if (i > 0) lsp_append_text(buf, sizeof(buf), &offset, ", ");
        lsp_append_text(buf, sizeof(buf), &offset, stmt->func_decl.params[i] ? stmt->func_decl.params[i] : "_");
        lsp_append_text(buf, sizeof(buf), &offset, ": ");
        lsp_append_type_string(buf, sizeof(buf), &offset, stmt->func_decl.param_types[i]);
    }
    lsp_append_text(buf, sizeof(buf), &offset, ")");
    if (stmt->func_decl.return_type) {
        lsp_append_text(buf, sizeof(buf), &offset, ": ");
        lsp_append_type_string(buf, sizeof(buf), &offset, stmt->func_decl.return_type);
    }
    return safe_strdup(buf);
}

static const char* lsp_stmt_name(const Stmt* stmt) {
    if (!stmt) return NULL;

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            return stmt->var_decl.name;
        case STMT_FUNC_DECL:
            return stmt->func_decl.name;
        case STMT_RECORD_DECL:
            return stmt->record_decl.name;
        case STMT_INTERFACE_DECL:
            return stmt->interface_decl.name;
        case STMT_TYPE_ALIAS:
            return stmt->type_alias.name;
        case STMT_ENUM_DECL:
            return stmt->enum_decl.name;
        default:
            return NULL;
    }
}

static char* lsp_build_stmt_detail(const Stmt* stmt) {
    if (!stmt) return safe_strdup("");

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            return lsp_build_var_detail(stmt);
        case STMT_FUNC_DECL:
            return lsp_build_function_detail(stmt);
        case STMT_RECORD_DECL:
            return lsp_build_record_detail(stmt);
        case STMT_INTERFACE_DECL:
            return lsp_build_interface_detail(stmt);
        case STMT_TYPE_ALIAS:
            return lsp_build_type_alias_detail(stmt);
        case STMT_ENUM_DECL:
            return lsp_build_enum_detail(stmt);
        default:
            return safe_strdup("");
    }
}

static char* lsp_build_interface_method_detail(Type* method_type) {
    char buf[1024];
    size_t offset = 0;

    buf[0] = '\0';
    lsp_append_text(buf, sizeof(buf), &offset, "(");
    if (method_type && method_type->kind == TYPE_FUNCTION) {
        for (int i = 0; i < method_type->param_count; i++) {
            if (i > 0) lsp_append_text(buf, sizeof(buf), &offset, ", ");
            lsp_append_type_string(buf, sizeof(buf), &offset, method_type->param_types[i]);
        }
        lsp_append_text(buf, sizeof(buf), &offset, ")");
        if (method_type->return_type) {
            lsp_append_text(buf, sizeof(buf), &offset, ": ");
            lsp_append_type_string(buf, sizeof(buf), &offset, method_type->return_type);
        }
    } else {
        lsp_append_text(buf, sizeof(buf), &offset, ")");
    }
    return safe_strdup(buf);
}

static char* lsp_build_enum_member_detail(const Stmt* stmt, int index) {
    char buf[1024];
    size_t offset = 0;

    buf[0] = '\0';
    if (!stmt || stmt->kind != STMT_ENUM_DECL || index < 0 || index >= stmt->enum_decl.member_count) {
        return safe_strdup("");
    }

    lsp_append_text(buf,
                    sizeof(buf),
                    &offset,
                    stmt->enum_decl.member_names[index] ? stmt->enum_decl.member_names[index] : "_");
    if (stmt->enum_decl.member_payload_counts &&
        stmt->enum_decl.member_payload_counts[index] > 0 &&
        stmt->enum_decl.member_payload_types &&
        stmt->enum_decl.member_payload_types[index]) {
        lsp_append_text(buf, sizeof(buf), &offset, "(");
        for (int i = 0; i < stmt->enum_decl.member_payload_counts[index]; i++) {
            if (i > 0) lsp_append_text(buf, sizeof(buf), &offset, ", ");
            lsp_append_type_string(buf,
                                   sizeof(buf),
                                   &offset,
                                   stmt->enum_decl.member_payload_types[index][i]);
        }
        lsp_append_text(buf, sizeof(buf), &offset, ")");
    } else if (stmt->enum_decl.member_values) {
        char value_buf[64];
        snprintf(value_buf, sizeof(value_buf), " = %" PRId64, stmt->enum_decl.member_values[index]);
        lsp_append_text(buf, sizeof(buf), &offset, value_buf);
    }
    return safe_strdup(buf);
}

static cJSON* lsp_make_position(int line, int column) {
    cJSON* pos = cJSON_CreateObject();
    int line_zero = line > 0 ? line - 1 : 0;
    int column_zero = column > 0 ? column - 1 : 0;
    cJSON_AddNumberToObject(pos, "line", line_zero);
    cJSON_AddNumberToObject(pos, "character", column_zero);
    return pos;
}

static void lsp_add_range(cJSON* parent,
                          const char* key,
                          int line,
                          int column,
                          int end_line,
                          int end_column) {
    cJSON* range = NULL;
    if (!parent || !key) return;
    range = cJSON_CreateObject();
    cJSON_AddItemToObject(range, "start", lsp_make_position(line, column));
    cJSON_AddItemToObject(range, "end", lsp_make_position(end_line, end_column));
    cJSON_AddItemToObject(parent, key, range);
}

static cJSON* lsp_create_symbol(const char* name,
                                int kind,
                                int line,
                                int column,
                                const char* detail) {
    cJSON* symbol = cJSON_CreateObject();
    int end_column = column > 0 ? column + (int)strlen(name ? name : "") : 1;

    cJSON_AddStringToObject(symbol, "name", name ? name : "");
    cJSON_AddNumberToObject(symbol, "kind", kind);
    if (detail && detail[0] != '\0') {
        cJSON_AddStringToObject(symbol, "detail", detail);
    }
    lsp_add_range(symbol, "range", line, column, line, end_column);
    lsp_add_range(symbol, "selectionRange", line, column, line, end_column);
    return symbol;
}

static char* lsp_build_hover_payload(const char* value,
                                     int line,
                                     int column,
                                     const char* token_text) {
    cJSON* hover = NULL;
    cJSON* contents = NULL;
    char* rendered = NULL;
    int end_column = column > 0 ? column + (int)strlen(token_text ? token_text : "") : 1;

    hover = cJSON_CreateObject();
    contents = cJSON_CreateObject();
    cJSON_AddStringToObject(contents, "kind", "plaintext");
    cJSON_AddStringToObject(contents, "value", value ? value : "");
    cJSON_AddItemToObject(hover, "contents", contents);
    lsp_add_range(hover, "range", line, column, line, end_column);
    rendered = cJSON_PrintUnformatted(hover);
    cJSON_Delete(hover);
    return rendered;
}

static int lsp_position_in_name_span(int target_line,
                                     int target_column,
                                     int line,
                                     int column,
                                     const char* name) {
    size_t name_len = 0;
    if (!name) return 0;
    if (target_line != line) return 0;
    if (column <= 0) return 0;
    name_len = strlen(name);
    return target_column >= column && target_column < column + (int)name_len;
}

static char* lsp_build_named_type_detail(const char* name, Type* type) {
    char buf[1024];
    size_t offset = 0;

    buf[0] = '\0';
    if (name && name[0] != '\0') {
        lsp_append_text(buf, sizeof(buf), &offset, name);
    } else {
        lsp_append_text(buf, sizeof(buf), &offset, "_");
    }
    if (type) {
        lsp_append_text(buf, sizeof(buf), &offset, ": ");
        lsp_append_type_string(buf, sizeof(buf), &offset, type);
    }
    return safe_strdup(buf);
}

static int lsp_is_ident_char(char c);
static int lsp_find_stmt_start(const char* source, const Stmt* stmt, int* out_line, int* out_column);
static char* lsp_build_location_json(const char* file_path,
                                     int line,
                                     int column,
                                     const char* token_text);

static char* lsp_build_binding_detail(const LspBinding* binding) {
    char buf[1024];
    size_t offset = 0;

    if (!binding || !binding->name) return safe_strdup("");
    buf[0] = '\0';

    switch (binding->kind) {
        case LSP_BINDING_PARAM:
            lsp_append_text(buf, sizeof(buf), &offset, binding->name);
            break;
        case LSP_BINDING_VAR:
            lsp_append_text(buf, sizeof(buf), &offset, "var ");
            lsp_append_text(buf, sizeof(buf), &offset, binding->name);
            break;
        case LSP_BINDING_CONST:
            lsp_append_text(buf, sizeof(buf), &offset, "const ");
            lsp_append_text(buf, sizeof(buf), &offset, binding->name);
            break;
        case LSP_BINDING_FOREACH:
        case LSP_BINDING_FOR_RANGE:
            lsp_append_text(buf, sizeof(buf), &offset, binding->name);
            break;
        case LSP_BINDING_TYPE_PARAM:
            lsp_append_text(buf, sizeof(buf), &offset, "type ");
            lsp_append_text(buf, sizeof(buf), &offset, binding->name);
            break;
        default:
            lsp_append_text(buf, sizeof(buf), &offset, binding->name);
            break;
    }

    if (binding->type) {
        lsp_append_text(buf, sizeof(buf), &offset, ": ");
        lsp_append_type_string(buf, sizeof(buf), &offset, binding->type);
    }
    return safe_strdup(buf);
}

static const LspBinding* lsp_scope_lookup(const LspBinding* scope, const char* name) {
    const LspBinding* current = scope;
    while (current) {
        if (current->name && name && strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

static void lsp_offset_to_line_column(const char* source,
                                      const char* position,
                                      int* out_line,
                                      int* out_column) {
    int line = 1;
    int column = 1;

    if (!source || !position || !out_line || !out_column) return;
    for (const char* p = source; p < position && *p; p++) {
        if (*p == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
    }

    *out_line = line;
    *out_column = column;
}

static const char* lsp_source_pointer_from_line_column(const char* source, int line, int column) {
    int current_line = 1;
    int current_column = 1;
    const char* cursor = source;

    if (!source || line <= 0 || column <= 0) return source;
    while (cursor && *cursor) {
        if (current_line == line && current_column == column) {
            return cursor;
        }
        if (*cursor == '\n') {
            current_line++;
            current_column = 1;
        } else {
            current_column++;
        }
        cursor++;
    }
    return cursor;
}

static int lsp_find_identifier_span_at_position(const char* source,
                                                int target_line,
                                                int target_column,
                                                const char** out_start,
                                                const char** out_end,
                                                int* out_line,
                                                int* out_column) {
    const char* cursor = NULL;
    const char* start = NULL;
    const char* end = NULL;
    int line = 0;
    int column = 0;

    if (!source || !out_start || !out_end || target_line <= 0 || target_column <= 0) return 0;

    cursor = lsp_source_pointer_from_line_column(source, target_line, target_column);
    if (!cursor || *cursor == '\0') return 0;
    if (!lsp_is_ident_char(*cursor)) return 0;

    start = cursor;
    end = cursor;
    while (start > source && lsp_is_ident_char(start[-1])) start--;
    while (*end && lsp_is_ident_char(*end)) end++;

    lsp_offset_to_line_column(source, start, &line, &column);
    *out_start = start;
    *out_end = end;
    if (out_line) *out_line = line;
    if (out_column) *out_column = column;
    return 1;
}

static int lsp_find_stmt_start(const char* source, const Stmt* stmt, int* out_line, int* out_column) {
    char pattern[256];
    const char* found = NULL;

    if (!source || !stmt || !out_line || !out_column) return 0;
    pattern[0] = '\0';

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            snprintf(pattern,
                     sizeof(pattern),
                     "%s %s",
                     stmt->var_decl.is_mutable ? "var" : "const",
                     stmt->var_decl.name ? stmt->var_decl.name : "_");
            break;
        case STMT_FUNC_DECL:
            if (stmt->func_decl.is_async) {
                snprintf(pattern, sizeof(pattern), "async func %s", stmt->func_decl.name ? stmt->func_decl.name : "_");
            } else {
                snprintf(pattern, sizeof(pattern), "func %s", stmt->func_decl.name ? stmt->func_decl.name : "_");
            }
            break;
        case STMT_RECORD_DECL:
            snprintf(pattern, sizeof(pattern), "record %s", stmt->record_decl.name ? stmt->record_decl.name : "_");
            break;
        case STMT_INTERFACE_DECL:
            snprintf(pattern, sizeof(pattern), "interface %s", stmt->interface_decl.name ? stmt->interface_decl.name : "_");
            break;
        case STMT_TYPE_ALIAS:
            snprintf(pattern, sizeof(pattern), "type %s", stmt->type_alias.name ? stmt->type_alias.name : "_");
            break;
        case STMT_ENUM_DECL:
            snprintf(pattern, sizeof(pattern), "enum %s", stmt->enum_decl.name ? stmt->enum_decl.name : "_");
            break;
        default:
            return 0;
    }

    found = strstr(source, pattern);
    if (!found) return 0;

    lsp_offset_to_line_column(source, found, out_line, out_column);
    return 1;
}

static int lsp_find_stmt_name_location(const char* source,
                                       const Stmt* stmt,
                                       int* out_line,
                                       int* out_column) {
    int line = 0;
    int column = 0;
    int name_column = 0;

    if (!stmt || !out_line || !out_column) return 0;
    if (!lsp_find_stmt_start(source, stmt, &line, &column)) {
        return 0;
    }

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            name_column = column + (stmt->var_decl.is_mutable ? 4 : 6);
            break;
        case STMT_FUNC_DECL:
            name_column = column + (stmt->func_decl.is_async ? 11 : 5);
            break;
        case STMT_RECORD_DECL:
            name_column = column + 7;
            break;
        case STMT_INTERFACE_DECL:
            name_column = column + 10;
            break;
        case STMT_TYPE_ALIAS:
            name_column = column + 5;
            break;
        case STMT_ENUM_DECL:
            name_column = column + 5;
            break;
        default:
            return 0;
    }

    *out_line = line;
    *out_column = name_column;
    return 1;
}

static int lsp_is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static const char* lsp_find_named_token_in_region(const char* region_start,
                                                  const char* region_end,
                                                  const char* name) {
    size_t name_len = 0;
    const char* cursor = NULL;

    if (!region_start || !region_end || !name || region_end <= region_start) return NULL;
    name_len = strlen(name);
    if (name_len == 0) return NULL;

    cursor = region_start;
    while (cursor && cursor < region_end) {
        const char* found = strstr(cursor, name);
        if (!found || found >= region_end) return NULL;
        if ((found == region_start || !lsp_is_ident_char(found[-1])) &&
            (found + name_len >= region_end || !lsp_is_ident_char(found[name_len]))) {
            return found;
        }
        cursor = found + 1;
    }

    return NULL;
}

static int lsp_find_braced_member_location(const char* source,
                                           const Stmt* stmt,
                                           const char* member_name,
                                           int* out_line,
                                           int* out_column) {
    int stmt_line = 0;
    int stmt_column = 0;
    const char* stmt_start = NULL;
    const char* brace_start = NULL;
    const char* brace_end = NULL;
    const char* member = NULL;
    int depth = 0;

    if (!source || !stmt || !member_name || !out_line || !out_column) return 0;
    if (!lsp_find_stmt_start(source, stmt, &stmt_line, &stmt_column)) return 0;

    stmt_start = source;
    {
        int current_line = 1;
        int current_column = 1;
        while (*stmt_start) {
            if (current_line == stmt_line && current_column == stmt_column) break;
            if (*stmt_start == '\n') {
                current_line++;
                current_column = 1;
            } else {
                current_column++;
            }
            stmt_start++;
        }
    }

    brace_start = strchr(stmt_start, '{');
    if (!brace_start) return 0;

    for (const char* p = brace_start; *p; p++) {
        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) {
                brace_end = p;
                break;
            }
        }
    }
    if (!brace_end) return 0;

    member = lsp_find_named_token_in_region(brace_start, brace_end, member_name);
    if (!member) return 0;

    lsp_offset_to_line_column(source, member, out_line, out_column);
    return 1;
}

static const Stmt* lsp_find_top_level_decl_by_name(const Program* program, const char* name) {
    if (!program || !name) return NULL;
    for (int i = 0; i < program->stmt_count; i++) {
        Stmt* stmt = program->statements[i];
        const char* stmt_name = lsp_stmt_name(stmt);
        if (stmt_name && strcmp(stmt_name, name) == 0) {
            return stmt;
        }
    }
    return NULL;
}

static int lsp_find_enum_member_by_generated_name(const Program* program,
                                                  const char* generated_name,
                                                  const Stmt** out_enum_decl,
                                                  const char** out_member_name) {
    const char* sep = NULL;
    char enum_name[128];
    size_t enum_len = 0;
    const Stmt* enum_decl = NULL;

    if (!program || !generated_name || !out_enum_decl || !out_member_name) return 0;
    sep = strchr(generated_name, '_');
    if (!sep) return 0;
    enum_len = (size_t)(sep - generated_name);
    if (enum_len == 0 || enum_len >= sizeof(enum_name)) return 0;

    memcpy(enum_name, generated_name, enum_len);
    enum_name[enum_len] = '\0';
    enum_decl = lsp_find_top_level_decl_by_name(program, enum_name);
    if (!enum_decl || enum_decl->kind != STMT_ENUM_DECL) return 0;

    for (int i = 0; i < enum_decl->enum_decl.member_count; i++) {
        if (enum_decl->enum_decl.member_names[i] &&
            strcmp(enum_decl->enum_decl.member_names[i], sep + 1) == 0) {
            *out_enum_decl = enum_decl;
            *out_member_name = enum_decl->enum_decl.member_names[i];
            return 1;
        }
    }

    return 0;
}

static int lsp_is_type_like_decl(const Stmt* stmt) {
    if (!stmt) return 0;
    return stmt->kind == STMT_RECORD_DECL ||
           stmt->kind == STMT_INTERFACE_DECL ||
           stmt->kind == STMT_TYPE_ALIAS ||
           stmt->kind == STMT_ENUM_DECL;
}

static int lsp_find_function_param_location(const char* source,
                                            const Stmt* stmt,
                                            int param_index,
                                            int* out_line,
                                            int* out_column) {
    const char* cursor = NULL;
    int depth = 0;
    int seen_param_count = 0;
    int start_line = 0;
    int start_column = 0;

    if (!source || !stmt || stmt->kind != STMT_FUNC_DECL || param_index < 0 ||
        param_index >= stmt->func_decl.param_count || !out_line || !out_column) {
        return 0;
    }

    if (!lsp_find_stmt_start(source, stmt, &start_line, &start_column)) {
        return 0;
    }
    cursor = lsp_source_pointer_from_line_column(source, start_line, start_column);
    if (!cursor) return 0;
    while (*cursor && *cursor != '(') cursor++;
    if (*cursor != '(') return 0;

    for (; *cursor; cursor++) {
        if (*cursor == '(') {
            depth++;
            continue;
        }
        if (*cursor == ')') {
            depth--;
            if (depth <= 0) break;
            continue;
        }
        if (depth == 1 && (isalpha((unsigned char)*cursor) || *cursor == '_')) {
            const char* start = cursor;
            const char* after = cursor;
            while (*after && lsp_is_ident_char(*after)) after++;
            while (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n') after++;
            if (*after == ':') {
                if (seen_param_count == param_index) {
                    lsp_offset_to_line_column(source, start, out_line, out_column);
                    return 1;
                }
                seen_param_count++;
            }
            cursor = after > cursor ? after - 1 : cursor;
        }
    }

    return 0;
}

static int lsp_find_foreach_var_location(const char* source,
                                         const Stmt* stmt,
                                         int* out_line,
                                         int* out_column) {
    const char* cursor = NULL;
    int start_line = 0;
    int start_column = 0;

    if (!source || !stmt || !out_line || !out_column) return 0;
    if (stmt->kind != STMT_FOREACH && stmt->kind != STMT_FOR_RANGE) return 0;

    if (!lsp_find_stmt_start(source, stmt, &start_line, &start_column)) {
        return 0;
    }
    cursor = lsp_source_pointer_from_line_column(source, start_line, start_column);
    if (!cursor) return 0;
    while (*cursor && *cursor != '(') cursor++;
    if (*cursor != '(') return 0;
    cursor++;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') cursor++;
    if (strncmp(cursor, "var", 3) == 0 && !lsp_is_ident_char(cursor[3])) {
        cursor += 3;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') cursor++;
    }
    if (!lsp_is_ident_char(*cursor)) return 0;

    lsp_offset_to_line_column(source, cursor, out_line, out_column);
    return 1;
}

static int lsp_emit_binding_result(LspResolveMode mode,
                                   const char* file_path,
                                   const LspBinding* binding,
                                   char** out_json) {
    char* detail = NULL;

    if (!binding || !out_json) return 0;
    if (mode == LSP_RESOLVE_HOVER) {
        detail = lsp_build_binding_detail(binding);
        *out_json = lsp_build_hover_payload(detail,
                                            binding->line,
                                            binding->column,
                                            binding->name);
        free(detail);
    } else {
        *out_json = lsp_build_location_json(file_path,
                                            binding->line,
                                            binding->column,
                                            binding->name);
    }
    return *out_json != NULL;
}

static int lsp_make_var_binding(const char* source,
                                const Stmt* stmt,
                                const LspBinding* next,
                                LspBinding* out_binding) {
    if (!source || !stmt || stmt->kind != STMT_VAR_DECL || !out_binding) return 0;
    if (!lsp_find_stmt_name_location(source, stmt, &out_binding->line, &out_binding->column)) return 0;
    out_binding->name = stmt->var_decl.name;
    out_binding->type = stmt->var_decl.type_annotation ? stmt->var_decl.type_annotation : stmt->var_decl.initializer ? stmt->var_decl.initializer->type : NULL;
    out_binding->kind = stmt->var_decl.is_mutable ? LSP_BINDING_VAR : LSP_BINDING_CONST;
    out_binding->next = next;
    return 1;
}

static int lsp_make_param_binding(const char* source,
                                  const Stmt* stmt,
                                  int param_index,
                                  const LspBinding* next,
                                  LspBinding* out_binding) {
    if (!source || !stmt || stmt->kind != STMT_FUNC_DECL || !out_binding) return 0;
    if (!lsp_find_function_param_location(source, stmt, param_index, &out_binding->line, &out_binding->column)) return 0;
    out_binding->name = stmt->func_decl.params[param_index];
    out_binding->type = stmt->func_decl.param_types ? stmt->func_decl.param_types[param_index] : NULL;
    out_binding->kind = LSP_BINDING_PARAM;
    out_binding->next = next;
    return 1;
}

static int lsp_make_loop_binding(const char* source,
                                 const Stmt* stmt,
                                 const LspBinding* next,
                                 LspBinding* out_binding) {
    if (!source || !stmt || !out_binding) return 0;
    if (stmt->kind != STMT_FOREACH && stmt->kind != STMT_FOR_RANGE) return 0;
    if (!lsp_find_foreach_var_location(source, stmt, &out_binding->line, &out_binding->column)) return 0;
    out_binding->name = stmt->kind == STMT_FOREACH ? stmt->foreach.var_name : stmt->for_range.var_name;
    out_binding->type = stmt->kind == STMT_FOREACH &&
                        stmt->foreach.iterable &&
                        stmt->foreach.iterable->type &&
                        stmt->foreach.iterable->type->kind == TYPE_ARRAY
        ? stmt->foreach.iterable->type->element_type
        : NULL;
    out_binding->kind = stmt->kind == STMT_FOREACH ? LSP_BINDING_FOREACH : LSP_BINDING_FOR_RANGE;
    out_binding->next = next;
    return 1;
}

static int lsp_stmt_type_param_arrays(const Stmt* stmt,
                                      char*** out_type_params,
                                      Type*** out_constraints,
                                      int* out_type_param_count) {
    if (out_type_params) *out_type_params = NULL;
    if (out_constraints) *out_constraints = NULL;
    if (out_type_param_count) *out_type_param_count = 0;
    if (!stmt) return 0;

    switch (stmt->kind) {
        case STMT_FUNC_DECL:
            if (out_type_params) *out_type_params = stmt->func_decl.type_params;
            if (out_constraints) *out_constraints = stmt->func_decl.type_param_constraints;
            if (out_type_param_count) *out_type_param_count = stmt->func_decl.type_param_count;
            return stmt->func_decl.type_param_count > 0;
        case STMT_RECORD_DECL:
            if (out_type_params) *out_type_params = stmt->record_decl.type_params;
            if (out_type_param_count) *out_type_param_count = stmt->record_decl.type_param_count;
            return stmt->record_decl.type_param_count > 0;
        case STMT_TYPE_ALIAS:
            if (out_type_params) *out_type_params = stmt->type_alias.type_params;
            if (out_type_param_count) *out_type_param_count = stmt->type_alias.type_param_count;
            return stmt->type_alias.type_param_count > 0;
        case STMT_ENUM_DECL:
            if (out_type_params) *out_type_params = stmt->enum_decl.type_params;
            if (out_type_param_count) *out_type_param_count = stmt->enum_decl.type_param_count;
            return stmt->enum_decl.type_param_count > 0;
        default:
            return 0;
    }
}

static int lsp_find_stmt_type_param_location(const char* source,
                                             const Stmt* stmt,
                                             int type_param_index,
                                             int* out_line,
                                             int* out_column) {
    char** type_params = NULL;
    int type_param_count = 0;
    const char* stmt_name = NULL;
    const char* cursor = NULL;
    int name_line = 0;
    int name_column = 0;
    int depth = 0;
    int seen = 0;
    int expect_name = 0;

    if (!source || !stmt || !out_line || !out_column || type_param_index < 0) return 0;
    if (!lsp_stmt_type_param_arrays(stmt, &type_params, NULL, &type_param_count)) return 0;
    if (type_param_index >= type_param_count) return 0;
    stmt_name = lsp_stmt_name(stmt);
    if (!stmt_name) return 0;
    if (!lsp_find_stmt_name_location(source, stmt, &name_line, &name_column)) return 0;

    cursor = lsp_source_pointer_from_line_column(source, name_line, name_column);
    if (!cursor) return 0;
    cursor += strlen(stmt_name);
    while (*cursor && isspace((unsigned char)*cursor)) cursor++;
    if (*cursor != '[') return 0;

    depth = 1;
    expect_name = 1;
    cursor++;
    while (*cursor && depth > 0) {
        if (depth == 1 && expect_name) {
            while (*cursor && isspace((unsigned char)*cursor)) cursor++;
            if (lsp_is_ident_char(*cursor)) {
                const char* ident_start = cursor;
                while (*cursor && lsp_is_ident_char(*cursor)) cursor++;
                if (seen == type_param_index) {
                    lsp_offset_to_line_column(source, ident_start, out_line, out_column);
                    return 1;
                }
                seen++;
                expect_name = 0;
                continue;
            }
        }

        if (*cursor == '[') {
            depth++;
            cursor++;
            continue;
        }
        if (*cursor == ']') {
            depth--;
            cursor++;
            continue;
        }
        if (depth == 1 && *cursor == ',') {
            expect_name = 1;
            cursor++;
            continue;
        }
        cursor++;
    }

    return 0;
}

static int lsp_make_type_param_binding(const char* source,
                                       const Stmt* stmt,
                                       int type_param_index,
                                       const LspBinding* next,
                                       LspBinding* out_binding) {
    char** type_params = NULL;
    Type** constraints = NULL;
    int type_param_count = 0;

    if (!source || !stmt || !out_binding || type_param_index < 0) return 0;
    if (!lsp_stmt_type_param_arrays(stmt, &type_params, &constraints, &type_param_count)) return 0;
    if (type_param_index >= type_param_count || !type_params || !type_params[type_param_index]) return 0;
    if (!lsp_find_stmt_type_param_location(source, stmt, type_param_index, &out_binding->line, &out_binding->column)) {
        return 0;
    }

    out_binding->name = type_params[type_param_index];
    out_binding->type = constraints ? constraints[type_param_index] : NULL;
    out_binding->kind = LSP_BINDING_TYPE_PARAM;
    out_binding->next = next;
    return 1;
}

static char* lsp_path_to_file_uri_alloc(const char* file_path) {
    char* absolute = NULL;
    char* normalized = NULL;
    char* uri = NULL;
    size_t absolute_len = 0;

    if (!file_path) return NULL;

#ifdef _WIN32
    absolute = _fullpath(NULL, file_path, 0);
    if (!absolute) absolute = safe_strdup(file_path);
#else
    absolute = realpath(file_path, NULL);
    if (!absolute) absolute = safe_strdup(file_path);
#endif

    absolute_len = strlen(absolute);
    normalized = safe_strdup(absolute);
#ifdef _WIN32
    for (size_t i = 0; i < absolute_len; i++) {
        if (normalized[i] == '\\') normalized[i] = '/';
    }
    uri = (char*)safe_malloc(absolute_len + 9);
    snprintf(uri, absolute_len + 9, "file:///%s", normalized);
#else
    uri = (char*)safe_malloc(absolute_len + 8);
    snprintf(uri, absolute_len + 8, "file://%s", normalized);
#endif

#ifdef _WIN32
    free(absolute);
#else
    free(absolute);
#endif
    free(normalized);
    return uri;
}

static char* lsp_build_location_json(const char* file_path,
                                     int line,
                                     int column,
                                     const char* token_text) {
    cJSON* location = NULL;
    char* uri = NULL;
    char* rendered = NULL;
    int end_column = column > 0 ? column + (int)strlen(token_text ? token_text : "") : 1;

    location = cJSON_CreateObject();
    uri = lsp_path_to_file_uri_alloc(file_path);
    cJSON_AddStringToObject(location, "uri", uri ? uri : "");
    lsp_add_range(location, "range", line, column, line, end_column);
    rendered = cJSON_PrintUnformatted(location);
    cJSON_Delete(location);
    free(uri);
    return rendered;
}

static void lsp_add_children_if_nonempty(cJSON* symbol, cJSON* children) {
    if (!symbol || !children) return;
    if (cJSON_GetArraySize(children) > 0) {
        cJSON_AddItemToObject(symbol, "children", children);
    } else {
        cJSON_Delete(children);
    }
}

static int lsp_find_local_result_in_stmt(const char* source,
                                         const Program* program,
                                         const Stmt* stmt,
                                         int target_line,
                                         int target_column,
                                         const char* file_path,
                                         const LspBinding* scope,
                                         LspResolveMode mode,
                                         char** out_json);

static int lsp_find_local_result_in_expr(const char* source,
                                         const Program* program,
                                         const Expr* expr,
                                         int target_line,
                                         int target_column,
                                         const char* file_path,
                                         const LspBinding* scope,
                                         LspResolveMode mode,
                                         char** out_json) {
    const LspBinding* binding = NULL;

    if (!expr || !out_json) return 0;

    switch (expr->kind) {
        case EXPR_IDENTIFIER:
            if (!lsp_position_in_name_span(target_line,
                                           target_column,
                                           expr->line,
                                           expr->column,
                                           expr->identifier)) {
                return 0;
            }
            binding = lsp_scope_lookup(scope, expr->identifier);
            if (!binding) return 0;
            if (mode == LSP_RESOLVE_HOVER) {
                char* detail = lsp_build_binding_detail(binding);
                *out_json = lsp_build_hover_payload(detail, expr->line, expr->column, expr->identifier);
                free(detail);
                return *out_json != NULL;
            }
            return lsp_emit_binding_result(mode, file_path, binding, out_json);

        case EXPR_FIELD_ACCESS:
            return lsp_find_local_result_in_expr(source,
                                                 program,
                                                 expr->field_access.object,
                                                 target_line,
                                                 target_column,
                                                 file_path,
                                                 scope,
                                                 mode,
                                                 out_json);

        case EXPR_BINARY:
            return lsp_find_local_result_in_expr(source, program, expr->binary.left, target_line, target_column, file_path, scope, mode, out_json) ||
                   lsp_find_local_result_in_expr(source, program, expr->binary.right, target_line, target_column, file_path, scope, mode, out_json);

        case EXPR_UNARY:
            return lsp_find_local_result_in_expr(source, program, expr->unary.operand, target_line, target_column, file_path, scope, mode, out_json);

        case EXPR_CALL:
            if (lsp_find_local_result_in_expr(source, program, expr->call.callee, target_line, target_column, file_path, scope, mode, out_json)) {
                return 1;
            }
            for (int i = 0; i < expr->call.arg_count; i++) {
                if (lsp_find_local_result_in_expr(source,
                                                  program,
                                                  expr->call.args[i],
                                                  target_line,
                                                  target_column,
                                                  file_path,
                                                  scope,
                                                  mode,
                                                  out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_INDEX:
            return lsp_find_local_result_in_expr(source, program, expr->index.array, target_line, target_column, file_path, scope, mode, out_json) ||
                   lsp_find_local_result_in_expr(source, program, expr->index.index, target_line, target_column, file_path, scope, mode, out_json);

        case EXPR_FUNC_LITERAL:
            return lsp_find_local_result_in_stmt(source, program, expr->func_literal.body, target_line, target_column, file_path, scope, mode, out_json);

        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->array_literal.element_count; i++) {
                if (lsp_find_local_result_in_expr(source,
                                                  program,
                                                  expr->array_literal.elements[i],
                                                  target_line,
                                                  target_column,
                                                  file_path,
                                                  scope,
                                                  mode,
                                                  out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_CAST:
            return lsp_find_local_result_in_expr(source, program, expr->cast.value, target_line, target_column, file_path, scope, mode, out_json);

        case EXPR_TRY:
            return lsp_find_local_result_in_expr(source, program, expr->try_expr.expr, target_line, target_column, file_path, scope, mode, out_json);

        case EXPR_AWAIT:
            return lsp_find_local_result_in_expr(source, program, expr->await_expr.expr, target_line, target_column, file_path, scope, mode, out_json);

        case EXPR_TYPE_TEST:
            return lsp_find_local_result_in_expr(source, program, expr->type_test.value, target_line, target_column, file_path, scope, mode, out_json);

        case EXPR_IF:
            return lsp_find_local_result_in_expr(source, program, expr->if_expr.condition, target_line, target_column, file_path, scope, mode, out_json) ||
                   lsp_find_local_result_in_expr(source, program, expr->if_expr.then_expr, target_line, target_column, file_path, scope, mode, out_json) ||
                   lsp_find_local_result_in_expr(source, program, expr->if_expr.else_expr, target_line, target_column, file_path, scope, mode, out_json);

        case EXPR_MATCH:
            if (lsp_find_local_result_in_expr(source, program, expr->match_expr.subject, target_line, target_column, file_path, scope, mode, out_json)) {
                return 1;
            }
            for (int i = 0; i < expr->match_expr.arm_count; i++) {
                if (lsp_find_local_result_in_expr(source, program, expr->match_expr.patterns[i], target_line, target_column, file_path, scope, mode, out_json) ||
                    lsp_find_local_result_in_expr(source, program, expr->match_expr.guards ? expr->match_expr.guards[i] : NULL, target_line, target_column, file_path, scope, mode, out_json) ||
                    lsp_find_local_result_in_expr(source, program, expr->match_expr.values[i], target_line, target_column, file_path, scope, mode, out_json)) {
                    return 1;
                }
            }
            return lsp_find_local_result_in_expr(source, program, expr->match_expr.else_expr, target_line, target_column, file_path, scope, mode, out_json);

        case EXPR_BLOCK:
            for (int i = 0; i < expr->block_expr.stmt_count; i++) {
                if (lsp_find_local_result_in_stmt(source, program, expr->block_expr.statements[i], target_line, target_column, file_path, scope, mode, out_json)) {
                    return 1;
                }
            }
            return lsp_find_local_result_in_expr(source, program, expr->block_expr.value, target_line, target_column, file_path, scope, mode, out_json);

        case EXPR_RECORD_LITERAL:
            for (int i = 0; i < expr->record_literal.field_count; i++) {
                if (lsp_find_local_result_in_expr(source, program, expr->record_literal.field_values[i], target_line, target_column, file_path, scope, mode, out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_TUPLE_LITERAL:
            for (int i = 0; i < expr->tuple_literal.element_count; i++) {
                if (lsp_find_local_result_in_expr(source, program, expr->tuple_literal.elements[i], target_line, target_column, file_path, scope, mode, out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_TUPLE_ACCESS:
            return lsp_find_local_result_in_expr(source, program, expr->tuple_access.tuple, target_line, target_column, file_path, scope, mode, out_json);

        case EXPR_MAP_LITERAL:
            for (int i = 0; i < expr->map_literal.entry_count; i++) {
                if (lsp_find_local_result_in_expr(source, program, expr->map_literal.keys[i], target_line, target_column, file_path, scope, mode, out_json) ||
                    lsp_find_local_result_in_expr(source, program, expr->map_literal.values[i], target_line, target_column, file_path, scope, mode, out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_SET_LITERAL:
            for (int i = 0; i < expr->set_literal.element_count; i++) {
                if (lsp_find_local_result_in_expr(source, program, expr->set_literal.elements[i], target_line, target_column, file_path, scope, mode, out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_LITERAL:
        case EXPR_NIL:
        default:
            return 0;
    }
}

static int lsp_find_local_result_in_stmt_list(const char* source,
                                              const Program* program,
                                              Stmt** statements,
                                              int stmt_count,
                                              int index,
                                              int target_line,
                                              int target_column,
                                              const char* file_path,
                                              const LspBinding* scope,
                                              LspResolveMode mode,
                                              char** out_json) {
    LspBinding binding;
    Stmt* stmt = NULL;

    if (!statements || index >= stmt_count) return 0;
    stmt = statements[index];
    if (lsp_find_local_result_in_stmt(source, program, stmt, target_line, target_column, file_path, scope, mode, out_json)) {
        return 1;
    }

    if (stmt && stmt->kind == STMT_VAR_DECL &&
        lsp_make_var_binding(source, stmt, scope, &binding)) {
        return lsp_find_local_result_in_stmt_list(source,
                                                  program,
                                                  statements,
                                                  stmt_count,
                                                  index + 1,
                                                  target_line,
                                                  target_column,
                                                  file_path,
                                                  &binding,
                                                  mode,
                                                  out_json);
    }
    return lsp_find_local_result_in_stmt_list(source,
                                              program,
                                              statements,
                                              stmt_count,
                                              index + 1,
                                              target_line,
                                              target_column,
                                              file_path,
                                              scope,
                                              mode,
                                              out_json);
}

static int lsp_find_local_result_in_function(const char* source,
                                             const Program* program,
                                             const Stmt* stmt,
                                             int param_index,
                                             int target_line,
                                             int target_column,
                                             const char* file_path,
                                             const LspBinding* scope,
                                             LspResolveMode mode,
                                             char** out_json) {
    LspBinding binding;

    if (!stmt || stmt->kind != STMT_FUNC_DECL) return 0;
    if (param_index >= stmt->func_decl.param_count) {
        return lsp_find_local_result_in_stmt(source,
                                             program,
                                             stmt->func_decl.body,
                                             target_line,
                                             target_column,
                                             file_path,
                                             scope,
                                             mode,
                                             out_json);
    }

    if (lsp_make_param_binding(source, stmt, param_index, scope, &binding)) {
        if (lsp_position_in_name_span(target_line,
                                      target_column,
                                      binding.line,
                                      binding.column,
                                      binding.name)) {
            return lsp_emit_binding_result(mode, file_path, &binding, out_json);
        }
        return lsp_find_local_result_in_function(source,
                                                 program,
                                                 stmt,
                                                 param_index + 1,
                                                 target_line,
                                                 target_column,
                                                 file_path,
                                                 &binding,
                                                 mode,
                                                 out_json);
    }

    return lsp_find_local_result_in_function(source,
                                             program,
                                             stmt,
                                             param_index + 1,
                                             target_line,
                                             target_column,
                                             file_path,
                                             scope,
                                             mode,
                                             out_json);
}

static int lsp_find_local_result_in_stmt(const char* source,
                                         const Program* program,
                                         const Stmt* stmt,
                                         int target_line,
                                         int target_column,
                                         const char* file_path,
                                         const LspBinding* scope,
                                         LspResolveMode mode,
                                         char** out_json) {
    LspBinding binding;

    if (!stmt || !out_json) return 0;

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            if (lsp_make_var_binding(source, stmt, scope, &binding) &&
                lsp_position_in_name_span(target_line, target_column, binding.line, binding.column, binding.name)) {
                return lsp_emit_binding_result(mode, file_path, &binding, out_json);
            }
            return lsp_find_local_result_in_expr(source,
                                                 program,
                                                 stmt->var_decl.initializer,
                                                 target_line,
                                                 target_column,
                                                 file_path,
                                                 scope,
                                                 mode,
                                                 out_json);

        case STMT_VAR_TUPLE_DECL:
            return lsp_find_local_result_in_expr(source,
                                                 program,
                                                 stmt->var_tuple_decl.initializer,
                                                 target_line,
                                                 target_column,
                                                 file_path,
                                                 scope,
                                                 mode,
                                                 out_json);

        case STMT_EXPR:
            return lsp_find_local_result_in_expr(source, program, stmt->expr_stmt, target_line, target_column, file_path, scope, mode, out_json);

        case STMT_ASSIGN:
            return lsp_find_local_result_in_expr(source, program, stmt->assign.value, target_line, target_column, file_path, scope, mode, out_json);

        case STMT_ASSIGN_INDEX:
            return lsp_find_local_result_in_expr(source, program, stmt->assign_index.target, target_line, target_column, file_path, scope, mode, out_json) ||
                   lsp_find_local_result_in_expr(source, program, stmt->assign_index.index, target_line, target_column, file_path, scope, mode, out_json) ||
                   lsp_find_local_result_in_expr(source, program, stmt->assign_index.value, target_line, target_column, file_path, scope, mode, out_json);

        case STMT_ASSIGN_FIELD:
            return lsp_find_local_result_in_expr(source, program, stmt->assign_field.object, target_line, target_column, file_path, scope, mode, out_json) ||
                   lsp_find_local_result_in_expr(source, program, stmt->assign_field.value, target_line, target_column, file_path, scope, mode, out_json);

        case STMT_BLOCK:
            return lsp_find_local_result_in_stmt_list(source,
                                                      program,
                                                      stmt->block.statements,
                                                      stmt->block.stmt_count,
                                                      0,
                                                      target_line,
                                                      target_column,
                                                      file_path,
                                                      scope,
                                                      mode,
                                                      out_json);

        case STMT_IF:
            return lsp_find_local_result_in_expr(source, program, stmt->if_stmt.condition, target_line, target_column, file_path, scope, mode, out_json) ||
                   lsp_find_local_result_in_stmt(source, program, stmt->if_stmt.then_branch, target_line, target_column, file_path, scope, mode, out_json) ||
                   lsp_find_local_result_in_stmt(source, program, stmt->if_stmt.else_branch, target_line, target_column, file_path, scope, mode, out_json);

        case STMT_MATCH:
            if (lsp_find_local_result_in_expr(source, program, stmt->match_stmt.subject, target_line, target_column, file_path, scope, mode, out_json)) {
                return 1;
            }
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                if (lsp_find_local_result_in_expr(source, program, stmt->match_stmt.patterns[i], target_line, target_column, file_path, scope, mode, out_json) ||
                    lsp_find_local_result_in_expr(source, program, stmt->match_stmt.guards ? stmt->match_stmt.guards[i] : NULL, target_line, target_column, file_path, scope, mode, out_json) ||
                    lsp_find_local_result_in_stmt(source, program, stmt->match_stmt.bodies[i], target_line, target_column, file_path, scope, mode, out_json)) {
                    return 1;
                }
            }
            return lsp_find_local_result_in_stmt(source, program, stmt->match_stmt.else_branch, target_line, target_column, file_path, scope, mode, out_json);

        case STMT_WHILE:
            return lsp_find_local_result_in_expr(source, program, stmt->while_stmt.condition, target_line, target_column, file_path, scope, mode, out_json) ||
                   lsp_find_local_result_in_stmt(source, program, stmt->while_stmt.body, target_line, target_column, file_path, scope, mode, out_json);

        case STMT_FOREACH:
            if (lsp_make_loop_binding(source, stmt, scope, &binding) &&
                lsp_position_in_name_span(target_line, target_column, binding.line, binding.column, binding.name)) {
                return lsp_emit_binding_result(mode, file_path, &binding, out_json);
            }
            if (lsp_find_local_result_in_expr(source, program, stmt->foreach.iterable, target_line, target_column, file_path, scope, mode, out_json)) {
                return 1;
            }
            if (lsp_make_loop_binding(source, stmt, scope, &binding)) {
                return lsp_find_local_result_in_stmt(source, program, stmt->foreach.body, target_line, target_column, file_path, &binding, mode, out_json);
            }
            return lsp_find_local_result_in_stmt(source, program, stmt->foreach.body, target_line, target_column, file_path, scope, mode, out_json);

        case STMT_FOR_RANGE:
            if (lsp_make_loop_binding(source, stmt, scope, &binding) &&
                lsp_position_in_name_span(target_line, target_column, binding.line, binding.column, binding.name)) {
                return lsp_emit_binding_result(mode, file_path, &binding, out_json);
            }
            if (lsp_find_local_result_in_expr(source, program, stmt->for_range.start, target_line, target_column, file_path, scope, mode, out_json) ||
                lsp_find_local_result_in_expr(source, program, stmt->for_range.end, target_line, target_column, file_path, scope, mode, out_json)) {
                return 1;
            }
            if (lsp_make_loop_binding(source, stmt, scope, &binding)) {
                return lsp_find_local_result_in_stmt(source, program, stmt->for_range.body, target_line, target_column, file_path, &binding, mode, out_json);
            }
            return lsp_find_local_result_in_stmt(source, program, stmt->for_range.body, target_line, target_column, file_path, scope, mode, out_json);

        case STMT_RETURN:
            return lsp_find_local_result_in_expr(source, program, stmt->return_value, target_line, target_column, file_path, scope, mode, out_json);

        case STMT_DEFER:
            return lsp_find_local_result_in_expr(source, program, stmt->defer_expr, target_line, target_column, file_path, scope, mode, out_json);

        case STMT_FUNC_DECL:
            return lsp_find_local_result_in_function(source,
                                                     program,
                                                     stmt,
                                                     0,
                                                     target_line,
                                                     target_column,
                                                     file_path,
                                                     scope,
                                                     mode,
                                                     out_json);

        case STMT_BREAK:
        case STMT_CONTINUE:
        case STMT_IMPORT:
        case STMT_RECORD_DECL:
        case STMT_INTERFACE_DECL:
        case STMT_IMPL_DECL:
        case STMT_TYPE_ALIAS:
        case STMT_ENUM_DECL:
        default:
            return 0;
    }
}

static int lsp_find_local_result_in_program(const char* source,
                                            const Program* program,
                                            int target_line,
                                            int target_column,
                                            const char* file_path,
                                            LspResolveMode mode,
                                            char** out_json) {
    if (!program) return 0;
    for (int i = 0; i < program->stmt_count; i++) {
        if (lsp_find_local_result_in_stmt(source,
                                          program,
                                          program->statements[i],
                                          target_line,
                                          target_column,
                                          file_path,
                                          NULL,
                                          mode,
                                          out_json)) {
            return 1;
        }
    }
    return 0;
}

static int lsp_named_type_context_matches(const char* source, const char* start, const char* end) {
    const char* left = start;
    const char* right = end;
    char before = '\0';
    char after = '\0';

    if (!source || !start || !end || end <= start) return 0;

    while (left > source) {
        left--;
        if (!isspace((unsigned char)*left)) {
            before = *left;
            break;
        }
    }
    while (right && *right) {
        if (!isspace((unsigned char)*right)) {
            after = *right;
            break;
        }
        right++;
    }

    if (before != ':' && before != '[' && before != ',' && before != '=') {
        return 0;
    }

    if (after == '\0' || after == '\n' || after == '\r' ||
        after == ',' || after == ')' || after == ']' ||
        after == '=' || after == ';' || after == '?' ||
        after == '{') {
        return 1;
    }
    return 0;
}

static int lsp_find_named_type_reference_result(const char* source,
                                                const Program* program,
                                                int target_line,
                                                int target_column,
                                                const char* file_path,
                                                LspResolveMode mode,
                                                char** out_json) {
    const char* start = NULL;
    const char* end = NULL;
    const Stmt* decl = NULL;
    char name_buf[256];
    int token_line = 0;
    int token_column = 0;
    int decl_line = 0;
    int decl_column = 0;
    char* detail = NULL;

    if (!source || !program || !out_json) return 0;
    if (!lsp_find_identifier_span_at_position(source,
                                              target_line,
                                              target_column,
                                              &start,
                                              &end,
                                              &token_line,
                                              &token_column)) {
        return 0;
    }
    if ((size_t)(end - start) >= sizeof(name_buf)) return 0;
    memcpy(name_buf, start, (size_t)(end - start));
    name_buf[end - start] = '\0';

    decl = lsp_find_top_level_decl_by_name(program, name_buf);
    if (!lsp_is_type_like_decl(decl)) return 0;
    if (!lsp_named_type_context_matches(source, start, end)) return 0;

    if (mode == LSP_RESOLVE_HOVER) {
        detail = lsp_build_stmt_detail(decl);
        *out_json = lsp_build_hover_payload(detail, token_line, token_column, name_buf);
        free(detail);
        return *out_json != NULL;
    }

    if (!lsp_find_stmt_name_location(source, decl, &decl_line, &decl_column)) {
        return 0;
    }
    *out_json = lsp_build_location_json(file_path, decl_line, decl_column, name_buf);
    return *out_json != NULL;
}

static int lsp_find_type_param_reference_result(const char* source,
                                                const Program* program,
                                                int target_line,
                                                int target_column,
                                                const char* file_path,
                                                LspResolveMode mode,
                                                char** out_json) {
    const char* start = NULL;
    const char* end = NULL;
    const char* stmt_start = NULL;
    const char* stmt_end = NULL;
    char name_buf[256];
    int token_line = 0;
    int token_column = 0;
    int target_decl_match = 0;

    if (!source || !program || !out_json) return 0;
    if (!lsp_find_identifier_span_at_position(source,
                                              target_line,
                                              target_column,
                                              &start,
                                              &end,
                                              &token_line,
                                              &token_column)) {
        return 0;
    }
    if ((size_t)(end - start) >= sizeof(name_buf)) return 0;
    memcpy(name_buf, start, (size_t)(end - start));
    name_buf[end - start] = '\0';

    for (int i = 0; i < program->stmt_count; i++) {
        Stmt* stmt = program->statements[i];
        int stmt_line = 0;
        int stmt_column = 0;
        int next_line = 0;
        int next_column = 0;
        char** type_params = NULL;
        int type_param_count = 0;

        if (!stmt) continue;
        if (!lsp_stmt_type_param_arrays(stmt, &type_params, NULL, &type_param_count)) continue;
        if (!lsp_find_stmt_start(source, stmt, &stmt_line, &stmt_column)) continue;
        stmt_start = lsp_source_pointer_from_line_column(source, stmt_line, stmt_column);
        if (!stmt_start) continue;

        if (i + 1 < program->stmt_count &&
            lsp_find_stmt_start(source, program->statements[i + 1], &next_line, &next_column)) {
            stmt_end = lsp_source_pointer_from_line_column(source, next_line, next_column);
        } else {
            stmt_end = source + strlen(source);
        }
        if (start < stmt_start || start >= stmt_end) continue;

        for (int type_param_index = 0; type_param_index < type_param_count; type_param_index++) {
            LspBinding binding;
            char* detail = NULL;

            if (!type_params || !type_params[type_param_index]) continue;
            if (strcmp(type_params[type_param_index], name_buf) != 0) continue;
            if (!lsp_make_type_param_binding(source, stmt, type_param_index, NULL, &binding)) continue;

            target_decl_match = binding.line == token_line && binding.column == token_column;
            if (!target_decl_match && !lsp_named_type_context_matches(source, start, end)) {
                continue;
            }

            if (mode == LSP_RESOLVE_HOVER) {
                detail = lsp_build_binding_detail(&binding);
                *out_json = lsp_build_hover_payload(detail, token_line, token_column, name_buf);
                free(detail);
                return *out_json != NULL;
            }

            *out_json = lsp_build_location_json(file_path, binding.line, binding.column, binding.name);
            return *out_json != NULL;
        }
    }

    return 0;
}

static int lsp_find_hover_in_stmt(const char* source,
                                  const Program* program,
                                  const Stmt* stmt,
                                  int target_line,
                                  int target_column,
                                  char** out_json);

static char* lsp_build_field_hover_detail(const Program* program, const Expr* expr) {
    const Stmt* top_level_decl = NULL;
    if (!expr || expr->kind != EXPR_FIELD_ACCESS) return NULL;

    if (expr->field_access.object &&
        expr->field_access.object->kind == EXPR_IDENTIFIER &&
        expr->field_access.object->identifier) {
        top_level_decl = lsp_find_top_level_decl_by_name(program, expr->field_access.object->identifier);
        if (top_level_decl && top_level_decl->kind == STMT_ENUM_DECL) {
            for (int i = 0; i < top_level_decl->enum_decl.member_count; i++) {
                if (top_level_decl->enum_decl.member_names[i] &&
                    strcmp(top_level_decl->enum_decl.member_names[i], expr->field_access.field_name) == 0) {
                    return lsp_build_enum_member_detail(top_level_decl, i);
                }
            }
        }
    }

    return lsp_build_named_type_detail(expr->field_access.field_name, expr->type);
}

static int lsp_find_hover_in_expr(const char* source,
                                  const Program* program,
                                  const Expr* expr,
                                  int target_line,
                                  int target_column,
                                  char** out_json) {
    char* detail = NULL;
    (void)source;

    if (!expr || !out_json) return 0;

    switch (expr->kind) {
        case EXPR_IDENTIFIER: {
            const Stmt* top_level_decl = NULL;
            if (!lsp_position_in_name_span(target_line,
                                           target_column,
                                           expr->line,
                                           expr->column,
                                           expr->identifier)) {
                return 0;
            }

            top_level_decl = lsp_find_top_level_decl_by_name(program, expr->identifier);
            if (top_level_decl) {
                detail = lsp_build_stmt_detail(top_level_decl);
            } else {
                detail = lsp_build_named_type_detail(expr->identifier, expr->type);
            }
            *out_json = lsp_build_hover_payload(detail, expr->line, expr->column, expr->identifier);
            free(detail);
            return *out_json != NULL;
        }

        case EXPR_FIELD_ACCESS:
            if (lsp_find_hover_in_expr(source,
                                       program,
                                       expr->field_access.object,
                                       target_line,
                                       target_column,
                                       out_json)) {
                return 1;
            }
            if (!lsp_position_in_name_span(target_line,
                                           target_column,
                                           expr->line,
                                           expr->column,
                                           expr->field_access.field_name)) {
                return 0;
            }
            detail = lsp_build_field_hover_detail(program, expr);
            *out_json = lsp_build_hover_payload(detail,
                                                expr->line,
                                                expr->column,
                                                expr->field_access.field_name);
            free(detail);
            return *out_json != NULL;

        case EXPR_BINARY:
            return lsp_find_hover_in_expr(source, program, expr->binary.left, target_line, target_column, out_json) ||
                   lsp_find_hover_in_expr(source, program, expr->binary.right, target_line, target_column, out_json);

        case EXPR_UNARY:
            return lsp_find_hover_in_expr(source, program, expr->unary.operand, target_line, target_column, out_json);

        case EXPR_CALL:
            if (lsp_find_hover_in_expr(source, program, expr->call.callee, target_line, target_column, out_json)) {
                return 1;
            }
            for (int i = 0; i < expr->call.arg_count; i++) {
                if (lsp_find_hover_in_expr(source, program, expr->call.args[i], target_line, target_column, out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_INDEX:
            return lsp_find_hover_in_expr(source, program, expr->index.array, target_line, target_column, out_json) ||
                   lsp_find_hover_in_expr(source, program, expr->index.index, target_line, target_column, out_json);

        case EXPR_FUNC_LITERAL:
            return lsp_find_hover_in_stmt(source, program, expr->func_literal.body, target_line, target_column, out_json);

        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->array_literal.element_count; i++) {
                if (lsp_find_hover_in_expr(source,
                                           program,
                                           expr->array_literal.elements[i],
                                           target_line,
                                           target_column,
                                           out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_CAST:
            return lsp_find_hover_in_expr(source, program, expr->cast.value, target_line, target_column, out_json);

        case EXPR_TRY:
            return lsp_find_hover_in_expr(source, program, expr->try_expr.expr, target_line, target_column, out_json);

        case EXPR_AWAIT:
            return lsp_find_hover_in_expr(source, program, expr->await_expr.expr, target_line, target_column, out_json);

        case EXPR_TYPE_TEST:
            return lsp_find_hover_in_expr(source, program, expr->type_test.value, target_line, target_column, out_json);

        case EXPR_IF:
            return lsp_find_hover_in_expr(source, program, expr->if_expr.condition, target_line, target_column, out_json) ||
                   lsp_find_hover_in_expr(source, program, expr->if_expr.then_expr, target_line, target_column, out_json) ||
                   lsp_find_hover_in_expr(source, program, expr->if_expr.else_expr, target_line, target_column, out_json);

        case EXPR_MATCH:
            if (lsp_find_hover_in_expr(source, program, expr->match_expr.subject, target_line, target_column, out_json)) {
                return 1;
            }
            for (int i = 0; i < expr->match_expr.arm_count; i++) {
                if (lsp_find_hover_in_expr(source,
                                           program,
                                           expr->match_expr.patterns[i],
                                           target_line,
                                           target_column,
                                           out_json) ||
                    lsp_find_hover_in_expr(source,
                                           program,
                                           expr->match_expr.guards ? expr->match_expr.guards[i] : NULL,
                                           target_line,
                                           target_column,
                                           out_json) ||
                    lsp_find_hover_in_expr(source,
                                           program,
                                           expr->match_expr.values[i],
                                           target_line,
                                           target_column,
                                           out_json)) {
                    return 1;
                }
            }
            return lsp_find_hover_in_expr(source,
                                          program,
                                          expr->match_expr.else_expr,
                                          target_line,
                                          target_column,
                                          out_json);

        case EXPR_BLOCK:
            for (int i = 0; i < expr->block_expr.stmt_count; i++) {
                if (lsp_find_hover_in_stmt(source,
                                           program,
                                           expr->block_expr.statements[i],
                                           target_line,
                                           target_column,
                                           out_json)) {
                    return 1;
                }
            }
            return lsp_find_hover_in_expr(source, program, expr->block_expr.value, target_line, target_column, out_json);

        case EXPR_RECORD_LITERAL:
            for (int i = 0; i < expr->record_literal.field_count; i++) {
                if (lsp_find_hover_in_expr(source,
                                           program,
                                           expr->record_literal.field_values[i],
                                           target_line,
                                           target_column,
                                           out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_TUPLE_LITERAL:
            for (int i = 0; i < expr->tuple_literal.element_count; i++) {
                if (lsp_find_hover_in_expr(source,
                                           program,
                                           expr->tuple_literal.elements[i],
                                           target_line,
                                           target_column,
                                           out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_TUPLE_ACCESS:
            return lsp_find_hover_in_expr(source, program, expr->tuple_access.tuple, target_line, target_column, out_json);

        case EXPR_MAP_LITERAL:
            for (int i = 0; i < expr->map_literal.entry_count; i++) {
                if (lsp_find_hover_in_expr(source, program, expr->map_literal.keys[i], target_line, target_column, out_json) ||
                    lsp_find_hover_in_expr(source, program, expr->map_literal.values[i], target_line, target_column, out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_SET_LITERAL:
            for (int i = 0; i < expr->set_literal.element_count; i++) {
                if (lsp_find_hover_in_expr(source, program, expr->set_literal.elements[i], target_line, target_column, out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_LITERAL:
        case EXPR_NIL:
        default:
            return 0;
    }
}

static int lsp_find_hover_in_stmt(const char* source,
                                  const Program* program,
                                  const Stmt* stmt,
                                  int target_line,
                                  int target_column,
                                  char** out_json) {
    char* detail = NULL;
    int name_line = 0;
    int name_column = 0;
    const char* stmt_name = NULL;

    if (!stmt || !out_json) return 0;

    stmt_name = lsp_stmt_name(stmt);
    if (stmt_name &&
        lsp_find_stmt_name_location(source, stmt, &name_line, &name_column) &&
        lsp_position_in_name_span(target_line, target_column, name_line, name_column, stmt_name)) {
        detail = lsp_build_stmt_detail(stmt);
        *out_json = lsp_build_hover_payload(detail, name_line, name_column, stmt_name);
        free(detail);
        return *out_json != NULL;
    }

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            return lsp_find_hover_in_expr(source, program, stmt->var_decl.initializer, target_line, target_column, out_json);

        case STMT_VAR_TUPLE_DECL:
            return lsp_find_hover_in_expr(source, program, stmt->var_tuple_decl.initializer, target_line, target_column, out_json);

        case STMT_EXPR:
            return lsp_find_hover_in_expr(source, program, stmt->expr_stmt, target_line, target_column, out_json);

        case STMT_ASSIGN:
            return lsp_find_hover_in_expr(source, program, stmt->assign.value, target_line, target_column, out_json);

        case STMT_ASSIGN_INDEX:
            return lsp_find_hover_in_expr(source, program, stmt->assign_index.target, target_line, target_column, out_json) ||
                   lsp_find_hover_in_expr(source, program, stmt->assign_index.index, target_line, target_column, out_json) ||
                   lsp_find_hover_in_expr(source, program, stmt->assign_index.value, target_line, target_column, out_json);

        case STMT_ASSIGN_FIELD:
            return lsp_find_hover_in_expr(source, program, stmt->assign_field.object, target_line, target_column, out_json) ||
                   lsp_find_hover_in_expr(source, program, stmt->assign_field.value, target_line, target_column, out_json);

        case STMT_BLOCK:
            for (int i = 0; i < stmt->block.stmt_count; i++) {
                if (lsp_find_hover_in_stmt(source, program, stmt->block.statements[i], target_line, target_column, out_json)) {
                    return 1;
                }
            }
            return 0;

        case STMT_IF:
            return lsp_find_hover_in_expr(source, program, stmt->if_stmt.condition, target_line, target_column, out_json) ||
                   lsp_find_hover_in_stmt(source, program, stmt->if_stmt.then_branch, target_line, target_column, out_json) ||
                   lsp_find_hover_in_stmt(source, program, stmt->if_stmt.else_branch, target_line, target_column, out_json);

        case STMT_MATCH:
            if (lsp_find_hover_in_expr(source, program, stmt->match_stmt.subject, target_line, target_column, out_json)) {
                return 1;
            }
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                if (lsp_find_hover_in_expr(source,
                                           program,
                                           stmt->match_stmt.patterns[i],
                                           target_line,
                                           target_column,
                                           out_json) ||
                    lsp_find_hover_in_expr(source,
                                           program,
                                           stmt->match_stmt.guards ? stmt->match_stmt.guards[i] : NULL,
                                           target_line,
                                           target_column,
                                           out_json) ||
                    lsp_find_hover_in_stmt(source,
                                           program,
                                           stmt->match_stmt.bodies[i],
                                           target_line,
                                           target_column,
                                           out_json)) {
                    return 1;
                }
            }
            return lsp_find_hover_in_stmt(source,
                                          program,
                                          stmt->match_stmt.else_branch,
                                          target_line,
                                          target_column,
                                          out_json);

        case STMT_WHILE:
            return lsp_find_hover_in_expr(source, program, stmt->while_stmt.condition, target_line, target_column, out_json) ||
                   lsp_find_hover_in_stmt(source, program, stmt->while_stmt.body, target_line, target_column, out_json);

        case STMT_FOREACH:
            return lsp_find_hover_in_expr(source, program, stmt->foreach.iterable, target_line, target_column, out_json) ||
                   lsp_find_hover_in_stmt(source, program, stmt->foreach.body, target_line, target_column, out_json);

        case STMT_FOR_RANGE:
            return lsp_find_hover_in_expr(source, program, stmt->for_range.start, target_line, target_column, out_json) ||
                   lsp_find_hover_in_expr(source, program, stmt->for_range.end, target_line, target_column, out_json) ||
                   lsp_find_hover_in_stmt(source, program, stmt->for_range.body, target_line, target_column, out_json);

        case STMT_RETURN:
            return lsp_find_hover_in_expr(source, program, stmt->return_value, target_line, target_column, out_json);

        case STMT_DEFER:
            return lsp_find_hover_in_expr(source, program, stmt->defer_expr, target_line, target_column, out_json);

        case STMT_FUNC_DECL:
            return lsp_find_hover_in_stmt(source, program, stmt->func_decl.body, target_line, target_column, out_json);

        case STMT_BREAK:
        case STMT_CONTINUE:
        case STMT_IMPORT:
        case STMT_RECORD_DECL:
        case STMT_INTERFACE_DECL:
        case STMT_IMPL_DECL:
        case STMT_TYPE_ALIAS:
        case STMT_ENUM_DECL:
        default:
            return 0;
    }
}

static int lsp_find_definition_in_stmt(const char* source,
                                       const Program* program,
                                       const Stmt* stmt,
                                       int target_line,
                                       int target_column,
                                       const char* file_path,
                                       char** out_json);

static int lsp_find_definition_in_expr(const char* source,
                                       const Program* program,
                                       const Expr* expr,
                                       int target_line,
                                       int target_column,
                                       const char* file_path,
                                       char** out_json) {
    const Stmt* decl = NULL;
    int line = 0;
    int column = 0;

    if (!expr || !out_json) return 0;

    switch (expr->kind) {
        case EXPR_IDENTIFIER:
            if (!lsp_position_in_name_span(target_line,
                                           target_column,
                                           expr->line,
                                           expr->column,
                                           expr->identifier)) {
                return 0;
            }
            decl = lsp_find_top_level_decl_by_name(program, expr->identifier);
            if (decl) {
                if (!lsp_find_stmt_name_location(source, decl, &line, &column)) return 0;
                *out_json = lsp_build_location_json(file_path, line, column, expr->identifier);
                return *out_json != NULL;
            }
            {
                const Stmt* enum_decl = NULL;
                const char* member_name = NULL;
                if (lsp_find_enum_member_by_generated_name(program,
                                                           expr->identifier,
                                                           &enum_decl,
                                                           &member_name) &&
                    lsp_find_braced_member_location(source, enum_decl, member_name, &line, &column)) {
                    *out_json = lsp_build_location_json(file_path, line, column, member_name);
                    return *out_json != NULL;
                }
            }
            return 0;

        case EXPR_FIELD_ACCESS:
            if (lsp_find_definition_in_expr(source,
                                            program,
                                            expr->field_access.object,
                                            target_line,
                                            target_column,
                                            file_path,
                                            out_json)) {
                return 1;
            }
            if (!lsp_position_in_name_span(target_line,
                                           target_column,
                                           expr->line,
                                           expr->column,
                                           expr->field_access.field_name)) {
                return 0;
            }
            if (expr->field_access.object &&
                expr->field_access.object->kind == EXPR_IDENTIFIER &&
                expr->field_access.object->identifier) {
                const Stmt* enum_decl = lsp_find_top_level_decl_by_name(program, expr->field_access.object->identifier);
                if (enum_decl && enum_decl->kind == STMT_ENUM_DECL &&
                    lsp_find_braced_member_location(source,
                                                    enum_decl,
                                                    expr->field_access.field_name,
                                                    &line,
                                                    &column)) {
                    *out_json = lsp_build_location_json(file_path, line, column, expr->field_access.field_name);
                    return *out_json != NULL;
                }
            }
            if (expr->field_access.object &&
                expr->field_access.object->type &&
                expr->field_access.object->type->kind == TYPE_RECORD &&
                expr->field_access.object->type->record_def &&
                expr->field_access.object->type->record_def->name) {
                const Stmt* record_decl = lsp_find_top_level_decl_by_name(program,
                                                                          expr->field_access.object->type->record_def->name);
                if (record_decl && record_decl->kind == STMT_RECORD_DECL &&
                    lsp_find_braced_member_location(source,
                                                    record_decl,
                                                    expr->field_access.field_name,
                                                    &line,
                                                    &column)) {
                    *out_json = lsp_build_location_json(file_path, line, column, expr->field_access.field_name);
                    return *out_json != NULL;
                }
            }
            return 0;

        case EXPR_BINARY:
            return lsp_find_definition_in_expr(source, program, expr->binary.left, target_line, target_column, file_path, out_json) ||
                   lsp_find_definition_in_expr(source, program, expr->binary.right, target_line, target_column, file_path, out_json);

        case EXPR_UNARY:
            return lsp_find_definition_in_expr(source, program, expr->unary.operand, target_line, target_column, file_path, out_json);

        case EXPR_CALL:
            if (lsp_find_definition_in_expr(source, program, expr->call.callee, target_line, target_column, file_path, out_json)) {
                return 1;
            }
            for (int i = 0; i < expr->call.arg_count; i++) {
                if (lsp_find_definition_in_expr(source,
                                                program,
                                                expr->call.args[i],
                                                target_line,
                                                target_column,
                                                file_path,
                                                out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_INDEX:
            return lsp_find_definition_in_expr(source, program, expr->index.array, target_line, target_column, file_path, out_json) ||
                   lsp_find_definition_in_expr(source, program, expr->index.index, target_line, target_column, file_path, out_json);

        case EXPR_FUNC_LITERAL:
            return lsp_find_definition_in_stmt(source, program, expr->func_literal.body, target_line, target_column, file_path, out_json);

        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->array_literal.element_count; i++) {
                if (lsp_find_definition_in_expr(source,
                                                program,
                                                expr->array_literal.elements[i],
                                                target_line,
                                                target_column,
                                                file_path,
                                                out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_CAST:
            return lsp_find_definition_in_expr(source, program, expr->cast.value, target_line, target_column, file_path, out_json);

        case EXPR_TRY:
            return lsp_find_definition_in_expr(source, program, expr->try_expr.expr, target_line, target_column, file_path, out_json);

        case EXPR_AWAIT:
            return lsp_find_definition_in_expr(source, program, expr->await_expr.expr, target_line, target_column, file_path, out_json);

        case EXPR_TYPE_TEST:
            return lsp_find_definition_in_expr(source, program, expr->type_test.value, target_line, target_column, file_path, out_json);

        case EXPR_IF:
            return lsp_find_definition_in_expr(source, program, expr->if_expr.condition, target_line, target_column, file_path, out_json) ||
                   lsp_find_definition_in_expr(source, program, expr->if_expr.then_expr, target_line, target_column, file_path, out_json) ||
                   lsp_find_definition_in_expr(source, program, expr->if_expr.else_expr, target_line, target_column, file_path, out_json);

        case EXPR_MATCH:
            if (lsp_find_definition_in_expr(source, program, expr->match_expr.subject, target_line, target_column, file_path, out_json)) {
                return 1;
            }
            for (int i = 0; i < expr->match_expr.arm_count; i++) {
                if (lsp_find_definition_in_expr(source,
                                                program,
                                                expr->match_expr.patterns[i],
                                                target_line,
                                                target_column,
                                                file_path,
                                                out_json) ||
                    lsp_find_definition_in_expr(source,
                                                program,
                                                expr->match_expr.guards ? expr->match_expr.guards[i] : NULL,
                                                target_line,
                                                target_column,
                                                file_path,
                                                out_json) ||
                    lsp_find_definition_in_expr(source,
                                                program,
                                                expr->match_expr.values[i],
                                                target_line,
                                                target_column,
                                                file_path,
                                                out_json)) {
                    return 1;
                }
            }
            return lsp_find_definition_in_expr(source,
                                               program,
                                               expr->match_expr.else_expr,
                                               target_line,
                                               target_column,
                                               file_path,
                                               out_json);

        case EXPR_BLOCK:
            for (int i = 0; i < expr->block_expr.stmt_count; i++) {
                if (lsp_find_definition_in_stmt(source,
                                                program,
                                                expr->block_expr.statements[i],
                                                target_line,
                                                target_column,
                                                file_path,
                                                out_json)) {
                    return 1;
                }
            }
            return lsp_find_definition_in_expr(source, program, expr->block_expr.value, target_line, target_column, file_path, out_json);

        case EXPR_RECORD_LITERAL:
            for (int i = 0; i < expr->record_literal.field_count; i++) {
                if (lsp_find_definition_in_expr(source,
                                                program,
                                                expr->record_literal.field_values[i],
                                                target_line,
                                                target_column,
                                                file_path,
                                                out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_TUPLE_LITERAL:
            for (int i = 0; i < expr->tuple_literal.element_count; i++) {
                if (lsp_find_definition_in_expr(source,
                                                program,
                                                expr->tuple_literal.elements[i],
                                                target_line,
                                                target_column,
                                                file_path,
                                                out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_TUPLE_ACCESS:
            return lsp_find_definition_in_expr(source, program, expr->tuple_access.tuple, target_line, target_column, file_path, out_json);

        case EXPR_MAP_LITERAL:
            for (int i = 0; i < expr->map_literal.entry_count; i++) {
                if (lsp_find_definition_in_expr(source, program, expr->map_literal.keys[i], target_line, target_column, file_path, out_json) ||
                    lsp_find_definition_in_expr(source, program, expr->map_literal.values[i], target_line, target_column, file_path, out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_SET_LITERAL:
            for (int i = 0; i < expr->set_literal.element_count; i++) {
                if (lsp_find_definition_in_expr(source, program, expr->set_literal.elements[i], target_line, target_column, file_path, out_json)) {
                    return 1;
                }
            }
            return 0;

        case EXPR_LITERAL:
        case EXPR_NIL:
        default:
            return 0;
    }
}

static int lsp_find_definition_in_stmt(const char* source,
                                       const Program* program,
                                       const Stmt* stmt,
                                       int target_line,
                                       int target_column,
                                       const char* file_path,
                                       char** out_json) {
    int name_line = 0;
    int name_column = 0;
    const char* stmt_name = NULL;

    if (!stmt || !out_json) return 0;

    stmt_name = lsp_stmt_name(stmt);
    if (stmt_name &&
        lsp_find_stmt_name_location(source, stmt, &name_line, &name_column) &&
        lsp_position_in_name_span(target_line, target_column, name_line, name_column, stmt_name)) {
        *out_json = lsp_build_location_json(file_path, name_line, name_column, stmt_name);
        return *out_json != NULL;
    }

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            return lsp_find_definition_in_expr(source, program, stmt->var_decl.initializer, target_line, target_column, file_path, out_json);

        case STMT_VAR_TUPLE_DECL:
            return lsp_find_definition_in_expr(source, program, stmt->var_tuple_decl.initializer, target_line, target_column, file_path, out_json);

        case STMT_EXPR:
            return lsp_find_definition_in_expr(source, program, stmt->expr_stmt, target_line, target_column, file_path, out_json);

        case STMT_ASSIGN:
            return lsp_find_definition_in_expr(source, program, stmt->assign.value, target_line, target_column, file_path, out_json);

        case STMT_ASSIGN_INDEX:
            return lsp_find_definition_in_expr(source, program, stmt->assign_index.target, target_line, target_column, file_path, out_json) ||
                   lsp_find_definition_in_expr(source, program, stmt->assign_index.index, target_line, target_column, file_path, out_json) ||
                   lsp_find_definition_in_expr(source, program, stmt->assign_index.value, target_line, target_column, file_path, out_json);

        case STMT_ASSIGN_FIELD:
            return lsp_find_definition_in_expr(source, program, stmt->assign_field.object, target_line, target_column, file_path, out_json) ||
                   lsp_find_definition_in_expr(source, program, stmt->assign_field.value, target_line, target_column, file_path, out_json);

        case STMT_BLOCK:
            for (int i = 0; i < stmt->block.stmt_count; i++) {
                if (lsp_find_definition_in_stmt(source, program, stmt->block.statements[i], target_line, target_column, file_path, out_json)) {
                    return 1;
                }
            }
            return 0;

        case STMT_IF:
            return lsp_find_definition_in_expr(source, program, stmt->if_stmt.condition, target_line, target_column, file_path, out_json) ||
                   lsp_find_definition_in_stmt(source, program, stmt->if_stmt.then_branch, target_line, target_column, file_path, out_json) ||
                   lsp_find_definition_in_stmt(source, program, stmt->if_stmt.else_branch, target_line, target_column, file_path, out_json);

        case STMT_MATCH:
            if (lsp_find_definition_in_expr(source, program, stmt->match_stmt.subject, target_line, target_column, file_path, out_json)) {
                return 1;
            }
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                if (lsp_find_definition_in_expr(source, program, stmt->match_stmt.patterns[i], target_line, target_column, file_path, out_json) ||
                    lsp_find_definition_in_expr(source, program, stmt->match_stmt.guards ? stmt->match_stmt.guards[i] : NULL, target_line, target_column, file_path, out_json) ||
                    lsp_find_definition_in_stmt(source, program, stmt->match_stmt.bodies[i], target_line, target_column, file_path, out_json)) {
                    return 1;
                }
            }
            return lsp_find_definition_in_stmt(source, program, stmt->match_stmt.else_branch, target_line, target_column, file_path, out_json);

        case STMT_WHILE:
            return lsp_find_definition_in_expr(source, program, stmt->while_stmt.condition, target_line, target_column, file_path, out_json) ||
                   lsp_find_definition_in_stmt(source, program, stmt->while_stmt.body, target_line, target_column, file_path, out_json);

        case STMT_FOREACH:
            return lsp_find_definition_in_expr(source, program, stmt->foreach.iterable, target_line, target_column, file_path, out_json) ||
                   lsp_find_definition_in_stmt(source, program, stmt->foreach.body, target_line, target_column, file_path, out_json);

        case STMT_FOR_RANGE:
            return lsp_find_definition_in_expr(source, program, stmt->for_range.start, target_line, target_column, file_path, out_json) ||
                   lsp_find_definition_in_expr(source, program, stmt->for_range.end, target_line, target_column, file_path, out_json) ||
                   lsp_find_definition_in_stmt(source, program, stmt->for_range.body, target_line, target_column, file_path, out_json);

        case STMT_RETURN:
            return lsp_find_definition_in_expr(source, program, stmt->return_value, target_line, target_column, file_path, out_json);

        case STMT_DEFER:
            return lsp_find_definition_in_expr(source, program, stmt->defer_expr, target_line, target_column, file_path, out_json);

        case STMT_FUNC_DECL:
            return lsp_find_definition_in_stmt(source, program, stmt->func_decl.body, target_line, target_column, file_path, out_json);

        case STMT_BREAK:
        case STMT_CONTINUE:
        case STMT_IMPORT:
        case STMT_RECORD_DECL:
        case STMT_INTERFACE_DECL:
        case STMT_IMPL_DECL:
        case STMT_TYPE_ALIAS:
        case STMT_ENUM_DECL:
        default:
            return 0;
    }
}

static cJSON* lsp_symbol_from_stmt(const char* source, const Stmt* stmt) {
    cJSON* symbol = NULL;
    cJSON* children = NULL;
    char* detail = NULL;
    int child_line = 0;
    int child_column = 0;
    int line = 0;
    int column = 0;

    if (!stmt) return NULL;
    if (!lsp_find_stmt_start(source, stmt, &line, &column)) {
        line = stmt->line;
        column = stmt->column;
    }

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            detail = lsp_build_var_detail(stmt);
            symbol = lsp_create_symbol(stmt->var_decl.name,
                                       stmt->var_decl.is_mutable ? LSP_SYMBOL_KIND_VARIABLE : LSP_SYMBOL_KIND_CONSTANT,
                                       line,
                                       column,
                                       detail);
            break;

        case STMT_FUNC_DECL:
            detail = lsp_build_function_detail(stmt);
            symbol = lsp_create_symbol(stmt->func_decl.name,
                                       LSP_SYMBOL_KIND_FUNCTION,
                                       line,
                                       column,
                                       detail);
            break;

        case STMT_RECORD_DECL:
            detail = lsp_build_record_detail(stmt);
            symbol = lsp_create_symbol(stmt->record_decl.name,
                                       LSP_SYMBOL_KIND_STRUCT,
                                       line,
                                       column,
                                       detail);
            children = cJSON_CreateArray();
            child_line = line;
            child_column = column > 0 ? column + 2 : 1;
            for (int i = 0; i < stmt->record_decl.field_count; i++) {
                char field_detail[512];
                cJSON* field_symbol = NULL;
                field_detail[0] = '\0';
                if (stmt->record_decl.field_types[i]) {
                    char type_buf[256];
                    type_to_string(stmt->record_decl.field_types[i], type_buf, sizeof(type_buf));
                    snprintf(field_detail,
                             sizeof(field_detail),
                             "%s: %s",
                             stmt->record_decl.field_names[i] ? stmt->record_decl.field_names[i] : "_",
                             type_buf);
                } else {
                    snprintf(field_detail,
                             sizeof(field_detail),
                             "%s",
                             stmt->record_decl.field_names[i] ? stmt->record_decl.field_names[i] : "_");
                }
                field_symbol = lsp_create_symbol(stmt->record_decl.field_names[i],
                                                 LSP_SYMBOL_KIND_FIELD,
                                                 child_line,
                                                 child_column,
                                                 field_detail);
                cJSON_AddItemToArray(children, field_symbol);
            }
            lsp_add_children_if_nonempty(symbol, children);
            break;

        case STMT_INTERFACE_DECL:
            detail = lsp_build_interface_detail(stmt);
            symbol = lsp_create_symbol(stmt->interface_decl.name,
                                       LSP_SYMBOL_KIND_INTERFACE,
                                       line,
                                       column,
                                       detail);
            children = cJSON_CreateArray();
            child_line = line;
            child_column = column > 0 ? column + 2 : 1;
            for (int i = 0; i < stmt->interface_decl.method_count; i++) {
                char* method_detail = lsp_build_interface_method_detail(stmt->interface_decl.method_types[i]);
                cJSON* method_symbol = lsp_create_symbol(stmt->interface_decl.method_names[i],
                                                         LSP_SYMBOL_KIND_METHOD,
                                                         child_line,
                                                         child_column,
                                                         method_detail);
                cJSON_AddItemToArray(children, method_symbol);
                free(method_detail);
            }
            lsp_add_children_if_nonempty(symbol, children);
            break;

        case STMT_TYPE_ALIAS:
            detail = lsp_build_type_alias_detail(stmt);
            symbol = lsp_create_symbol(stmt->type_alias.name,
                                       LSP_SYMBOL_KIND_CLASS,
                                       line,
                                       column,
                                       detail);
            break;

        case STMT_ENUM_DECL:
            detail = lsp_build_enum_detail(stmt);
            symbol = lsp_create_symbol(stmt->enum_decl.name,
                                       LSP_SYMBOL_KIND_ENUM,
                                       line,
                                       column,
                                       detail);
            children = cJSON_CreateArray();
            child_line = line;
            child_column = column > 0 ? column + 2 : 1;
            for (int i = 0; i < stmt->enum_decl.member_count; i++) {
                char* member_detail = lsp_build_enum_member_detail(stmt, i);
                cJSON* member_symbol = lsp_create_symbol(stmt->enum_decl.member_names[i],
                                                         LSP_SYMBOL_KIND_ENUM_MEMBER,
                                                         child_line,
                                                         child_column,
                                                         member_detail);
                cJSON_AddItemToArray(children, member_symbol);
                free(member_detail);
            }
            lsp_add_children_if_nonempty(symbol, children);
            break;

        default:
            break;
    }

    if (detail) free(detail);
    return symbol;
}

static char* lsp_acquire_source_text(const char* file_path,
                                     const char* source_text,
                                     Error** out_error);

static char* lsp_build_document_symbols_json_from_source(const char* file_path,
                                                         const char* source_text,
                                                         Error** out_error) {
    char* source = NULL;
    ParseResult parse_result;
    cJSON* symbols = NULL;
    char* rendered = NULL;

    if (out_error) *out_error = NULL;
    source = lsp_acquire_source_text(file_path, source_text, out_error);
    if (!source) return NULL;

    parse_result = parser_parse(source, file_path);
    if (parse_result.error) {
        if (out_error) *out_error = lsp_error_clone(parse_result.error);
        free(source);
        parser_free_result(&parse_result);
        return NULL;
    }

    symbols = cJSON_CreateArray();
    for (int i = 0; i < parse_result.program->stmt_count; i++) {
        cJSON* symbol = lsp_symbol_from_stmt(source, parse_result.program->statements[i]);
        if (symbol) {
            cJSON_AddItemToArray(symbols, symbol);
        }
    }

    rendered = cJSON_PrintUnformatted(symbols);
    cJSON_Delete(symbols);
    free(source);
    parser_free_result(&parse_result);
    return rendered;
}

char* lsp_build_document_symbols_json(const char* file_path, Error** out_error) {
    return lsp_build_document_symbols_json_from_source(file_path, NULL, out_error);
}

static char* lsp_acquire_source_text(const char* file_path,
                                     const char* source_text,
                                     Error** out_error) {
    Error* read_error = NULL;
    char* source = NULL;

    if (out_error) *out_error = NULL;
    if (source_text) {
        return safe_strdup(source_text);
    }

    source = lsp_read_file_text(file_path, &read_error);
    if (!source) {
        if (out_error) *out_error = read_error;
        else error_free(read_error);
        return NULL;
    }
    return source;
}

static char* lsp_build_hover_json_from_source(const char* file_path,
                                              const char* source_text,
                                              int zero_based_line,
                                              int zero_based_character,
                                              Error** out_error) {
    char* source = NULL;
    ParseResult parse_result;
    TypeCheckResult typecheck_result;
    char* rendered = NULL;
    int target_line = zero_based_line + 1;
    int target_column = zero_based_character + 1;

    if (out_error) *out_error = NULL;
    source = lsp_acquire_source_text(file_path, source_text, out_error);
    if (!source) return NULL;

    parse_result = parser_parse(source, file_path);
    if (parse_result.error) {
        if (out_error) *out_error = lsp_error_clone(parse_result.error);
        free(source);
        parser_free_result(&parse_result);
        return NULL;
    }

    typecheck_result = typecheck(parse_result.program);
    if (typecheck_result.error) {
        if (out_error) *out_error = lsp_error_clone(typecheck_result.error);
        error_free(typecheck_result.error);
        symbol_table_free(typecheck_result.globals);
        free(source);
        parser_free_result(&parse_result);
        return NULL;
    }

    if (lsp_find_local_result_in_program(source,
                                         parse_result.program,
                                         target_line,
                                         target_column,
                                         file_path,
                                         LSP_RESOLVE_HOVER,
                                         &rendered)) {
        symbol_table_free(typecheck_result.globals);
        free(source);
        parser_free_result(&parse_result);
        return rendered;
    }

    for (int i = 0; i < parse_result.program->stmt_count; i++) {
        if (lsp_find_hover_in_stmt(source,
                                   parse_result.program,
                                   parse_result.program->statements[i],
                                   target_line,
                                   target_column,
                                   &rendered)) {
            break;
        }
    }

    if (!rendered) {
        lsp_find_type_param_reference_result(source,
                                             parse_result.program,
                                             target_line,
                                             target_column,
                                             file_path,
                                             LSP_RESOLVE_HOVER,
                                             &rendered);
    }

    if (!rendered) {
        lsp_find_named_type_reference_result(source,
                                             parse_result.program,
                                             target_line,
                                             target_column,
                                             file_path,
                                             LSP_RESOLVE_HOVER,
                                             &rendered);
    }

    if (!rendered) {
        rendered = safe_strdup("null");
    }

    symbol_table_free(typecheck_result.globals);
    free(source);
    parser_free_result(&parse_result);
    return rendered;
}

char* lsp_build_hover_json(const char* file_path,
                           int zero_based_line,
                           int zero_based_character,
                           Error** out_error) {
    return lsp_build_hover_json_from_source(file_path,
                                            NULL,
                                            zero_based_line,
                                            zero_based_character,
                                            out_error);
}

static char* lsp_build_definition_json_from_source(const char* file_path,
                                                   const char* source_text,
                                                   int zero_based_line,
                                                   int zero_based_character,
                                                   Error** out_error) {
    char* source = NULL;
    ParseResult parse_result;
    TypeCheckResult typecheck_result;
    char* rendered = NULL;
    int target_line = zero_based_line + 1;
    int target_column = zero_based_character + 1;

    if (out_error) *out_error = NULL;
    source = lsp_acquire_source_text(file_path, source_text, out_error);
    if (!source) return NULL;

    parse_result = parser_parse(source, file_path);
    if (parse_result.error) {
        if (out_error) *out_error = lsp_error_clone(parse_result.error);
        free(source);
        parser_free_result(&parse_result);
        return NULL;
    }

    typecheck_result = typecheck(parse_result.program);
    if (typecheck_result.error) {
        if (out_error) *out_error = lsp_error_clone(typecheck_result.error);
        error_free(typecheck_result.error);
        symbol_table_free(typecheck_result.globals);
        free(source);
        parser_free_result(&parse_result);
        return NULL;
    }

    if (lsp_find_local_result_in_program(source,
                                         parse_result.program,
                                         target_line,
                                         target_column,
                                         file_path,
                                         LSP_RESOLVE_DEFINITION,
                                         &rendered)) {
        symbol_table_free(typecheck_result.globals);
        free(source);
        parser_free_result(&parse_result);
        return rendered;
    }

    for (int i = 0; i < parse_result.program->stmt_count; i++) {
        if (lsp_find_definition_in_stmt(source,
                                        parse_result.program,
                                        parse_result.program->statements[i],
                                        target_line,
                                        target_column,
                                        file_path,
                                        &rendered)) {
            break;
        }
    }

    if (!rendered) {
        lsp_find_type_param_reference_result(source,
                                             parse_result.program,
                                             target_line,
                                             target_column,
                                             file_path,
                                             LSP_RESOLVE_DEFINITION,
                                             &rendered);
    }

    if (!rendered) {
        lsp_find_named_type_reference_result(source,
                                             parse_result.program,
                                             target_line,
                                             target_column,
                                             file_path,
                                             LSP_RESOLVE_DEFINITION,
                                             &rendered);
    }

    if (!rendered) {
        rendered = safe_strdup("null");
    }

    symbol_table_free(typecheck_result.globals);
    free(source);
    parser_free_result(&parse_result);
    return rendered;
}

char* lsp_build_definition_json(const char* file_path,
                                int zero_based_line,
                                int zero_based_character,
                                Error** out_error) {
    return lsp_build_definition_json_from_source(file_path,
                                                 NULL,
                                                 zero_based_line,
                                                 zero_based_character,
                                                 out_error);
}

static int lsp_find_line_start_offset(const char* source, int target_line) {
    int line = 1;
    int offset = 0;
    if (!source || target_line <= 1) return 0;
    while (source[offset] != '\0' && line < target_line) {
        if (source[offset] == '\n') {
            line++;
        }
        offset++;
    }
    return offset;
}

static int lsp_guess_error_end_column(const char* source, int line, int column) {
    int offset = 0;
    int current_column = 1;

    if (!source || line <= 0 || column <= 0) return column > 0 ? column + 1 : 1;

    offset = lsp_find_line_start_offset(source, line);
    while (source[offset] != '\0' && source[offset] != '\n' && current_column < column) {
        offset++;
        current_column++;
    }

    if (source[offset] == '\0' || source[offset] == '\n') {
        return column + 1;
    }

    if (isspace((unsigned char)source[offset])) {
        return column + 1;
    }

    while (source[offset] != '\0' &&
           source[offset] != '\n' &&
           !isspace((unsigned char)source[offset]) &&
           source[offset] != ',' &&
           source[offset] != ':' &&
           source[offset] != ';' &&
           source[offset] != '(' &&
           source[offset] != ')' &&
           source[offset] != '{' &&
           source[offset] != '}' &&
           source[offset] != '[' &&
           source[offset] != ']') {
        offset++;
        current_column++;
    }

    return current_column > column ? current_column : column + 1;
}

static Error* lsp_analyze_source_error(const char* file_path, const char* source_text) {
    ParseResult parse_result;
    TypeCheckResult typecheck_result;
    Error* result_error = NULL;

    parse_result = parser_parse(source_text, file_path);
    if (parse_result.error) {
        result_error = lsp_error_clone(parse_result.error);
        parser_free_result(&parse_result);
        return result_error;
    }

    typecheck_result = typecheck(parse_result.program);
    if (typecheck_result.error) {
        result_error = lsp_error_clone(typecheck_result.error);
    }

    error_free(typecheck_result.error);
    symbol_table_free(typecheck_result.globals);
    parser_free_result(&parse_result);
    return result_error;
}

static cJSON* lsp_build_diagnostics_array_from_error(const char* file_path,
                                                     const char* source_text,
                                                     const Error* error) {
    cJSON* diagnostics = NULL;
    cJSON* diagnostic = NULL;
    int line = 1;
    int column = 1;
    int end_column = 2;

    diagnostics = cJSON_CreateArray();
    if (!error) {
        return diagnostics;
    }

    line = error->line > 0 ? error->line : 1;
    column = error->column > 0 ? error->column : 1;
    end_column = lsp_guess_error_end_column(source_text, line, column);

    diagnostic = cJSON_CreateObject();
    lsp_add_range(diagnostic, "range", line, column, line, end_column);
    cJSON_AddNumberToObject(diagnostic, "severity", 1);
    cJSON_AddStringToObject(diagnostic, "source", "tablo");
    cJSON_AddStringToObject(diagnostic, "message", error->message ? error->message : "Unknown error");
    cJSON_AddStringToObject(diagnostic, "code", error_to_string(error->code));
    if (file_path && file_path[0] != '\0') {
        cJSON_AddStringToObject(diagnostic, "data", file_path);
    }
    cJSON_AddItemToArray(diagnostics, diagnostic);
    return diagnostics;
}

char* lsp_build_diagnostics_json(const char* file_path,
                                 const char* source_text,
                                 Error** out_error) {
    Error* read_error = NULL;
    Error* analysis_error = NULL;
    char* owned_source = NULL;
    cJSON* diagnostics = NULL;
    char* rendered = NULL;

    if (out_error) *out_error = NULL;
    if (!file_path || file_path[0] == '\0') {
        if (out_error) {
            *out_error = lsp_make_error(ERROR_IMPORT, "Missing source file path", NULL, 0, 0);
        }
        return NULL;
    }

    if (source_text) {
        owned_source = safe_strdup(source_text);
    } else {
        owned_source = lsp_read_file_text(file_path, &read_error);
        if (!owned_source) {
            if (out_error) *out_error = read_error;
            else error_free(read_error);
            return NULL;
        }
    }

    analysis_error = lsp_analyze_source_error(file_path, owned_source);
    diagnostics = lsp_build_diagnostics_array_from_error(file_path, owned_source, analysis_error);
    rendered = cJSON_PrintUnformatted(diagnostics);

    cJSON_Delete(diagnostics);
    error_free(analysis_error);
    free(owned_source);

    if (!rendered && out_error) {
        *out_error = lsp_make_error(ERROR_IMPORT, "Failed to encode diagnostics", file_path, 0, 0);
    }
    return rendered;
}

static int lsp_ascii_starts_with_ignore_case(const char* text, const char* prefix) {
    if (!text || !prefix) return 0;
    while (*prefix) {
        if (*text == '\0') return 0;
        if (tolower((unsigned char)*text) != tolower((unsigned char)*prefix)) return 0;
        text++;
        prefix++;
    }
    return 1;
}

static int lsp_hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static char* lsp_uri_decode_alloc(const char* text) {
    size_t len = 0;
    size_t out_len = 0;
    char* out = NULL;
    if (!text) return NULL;
    len = strlen(text);
    out = (char*)safe_malloc(len + 1);
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '%' && i + 2 < len) {
            int hi = lsp_hex_value(text[i + 1]);
            int lo = lsp_hex_value(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[out_len++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[out_len++] = text[i];
    }
    out[out_len] = '\0';
    return out;
}

static char* lsp_file_uri_to_path_alloc(const char* uri) {
    const char* path_part = NULL;
    char* decoded = NULL;
    if (!uri) return NULL;
    if (!lsp_ascii_starts_with_ignore_case(uri, "file://")) {
        return safe_strdup(uri);
    }

    path_part = uri + 7;
    while (*path_part == '/') {
#ifdef _WIN32
        if (isalpha((unsigned char)path_part[1]) && path_part[2] == ':') break;
#endif
        if (path_part[1] == '\0') break;
        if (path_part[1] == '/') {
            path_part++;
            continue;
        }
        break;
    }

    decoded = lsp_uri_decode_alloc(path_part);
#ifdef _WIN32
    for (char* p = decoded; p && *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif
    return decoded;
}

static int lsp_write_jsonrpc_response(cJSON* id, cJSON* result, cJSON* error_obj) {
    cJSON* response = NULL;
    char* payload = NULL;
    int payload_len = 0;

    response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(response, "id", cJSON_Duplicate(id, 1));
    } else {
        cJSON_AddNullToObject(response, "id");
    }
    if (error_obj) {
        cJSON_AddItemToObject(response, "error", error_obj);
    } else if (result) {
        cJSON_AddItemToObject(response, "result", result);
    } else {
        cJSON_AddNullToObject(response, "result");
    }

    payload = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (!payload) return 0;

    payload_len = (int)strlen(payload);
    printf("Content-Length: %d\r\n\r\n%s", payload_len, payload);
    fflush(stdout);
    free(payload);
    return 1;
}

static int lsp_write_jsonrpc_notification(const char* method, cJSON* params) {
    cJSON* message = NULL;
    char* payload = NULL;
    int payload_len = 0;

    if (!method) return 0;

    message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "jsonrpc", "2.0");
    cJSON_AddStringToObject(message, "method", method);
    if (params) {
        cJSON_AddItemToObject(message, "params", params);
    } else {
        cJSON_AddItemToObject(message, "params", cJSON_CreateObject());
    }

    payload = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (!payload) return 0;

    payload_len = (int)strlen(payload);
    printf("Content-Length: %d\r\n\r\n%s", payload_len, payload);
    fflush(stdout);
    free(payload);
    return 1;
}

static cJSON* lsp_make_jsonrpc_error(int code, const char* message) {
    cJSON* error_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(error_obj, "code", code);
    cJSON_AddStringToObject(error_obj, "message", message ? message : "Unknown error");
    return error_obj;
}

static int lsp_publish_diagnostics(const char* uri, const char* file_path, const char* source_text) {
    Error* error = NULL;
    char* json = NULL;
    cJSON* diagnostics = NULL;
    cJSON* params = NULL;

    if (!uri || !file_path) return 0;

    json = lsp_build_diagnostics_json(file_path, source_text, &error);
    if (!json) {
        error_free(error);
        return 0;
    }

    diagnostics = cJSON_Parse(json);
    free(json);
    if (!diagnostics) {
        return 0;
    }

    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "uri", uri);
    cJSON_AddItemToObject(params, "diagnostics", diagnostics);
    return lsp_write_jsonrpc_notification("textDocument/publishDiagnostics", params);
}

static int lsp_publish_empty_diagnostics(const char* uri) {
    cJSON* params = NULL;
    if (!uri) return 0;
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "uri", uri);
    cJSON_AddItemToObject(params, "diagnostics", cJSON_CreateArray());
    return lsp_write_jsonrpc_notification("textDocument/publishDiagnostics", params);
}

static int lsp_handle_initialize(cJSON* id) {
    cJSON* result = cJSON_CreateObject();
    cJSON* capabilities = cJSON_CreateObject();
    cJSON* text_document_sync = cJSON_CreateObject();
    cJSON* server_info = cJSON_CreateObject();

    cJSON_AddBoolToObject(capabilities, "documentSymbolProvider", 1);
    cJSON_AddBoolToObject(capabilities, "hoverProvider", 1);
    cJSON_AddBoolToObject(capabilities, "definitionProvider", 1);
    cJSON_AddBoolToObject(text_document_sync, "openClose", 1);
    cJSON_AddNumberToObject(text_document_sync, "change", 1);
    cJSON_AddBoolToObject(text_document_sync, "save", 1);
    cJSON_AddItemToObject(capabilities, "textDocumentSync", text_document_sync);
    cJSON_AddItemToObject(result, "capabilities", capabilities);
    cJSON_AddStringToObject(server_info, "name", "tablo-lsp");
    cJSON_AddStringToObject(server_info, "version", TABLO_VERSION);
    cJSON_AddItemToObject(result, "serverInfo", server_info);
    return lsp_write_jsonrpc_response(id, result, NULL);
}

static void lsp_handle_did_open(LspDocumentStore* store, cJSON* params) {
    cJSON* text_document = NULL;
    cJSON* uri_item = NULL;
    cJSON* text_item = NULL;
    char* file_path = NULL;

    if (!store || !params || !cJSON_IsObject(params)) return;
    text_document = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    uri_item = text_document ? cJSON_GetObjectItemCaseSensitive(text_document, "uri") : NULL;
    text_item = text_document ? cJSON_GetObjectItemCaseSensitive(text_document, "text") : NULL;
    if (!uri_item || !cJSON_IsString(uri_item) || !uri_item->valuestring ||
        !text_item || !cJSON_IsString(text_item) || !text_item->valuestring) {
        return;
    }

    file_path = lsp_file_uri_to_path_alloc(uri_item->valuestring);
    lsp_document_store_set(store, uri_item->valuestring, file_path, text_item->valuestring);
    lsp_publish_diagnostics(uri_item->valuestring, file_path, text_item->valuestring);
    free(file_path);
}

static void lsp_handle_did_change(LspDocumentStore* store, cJSON* params) {
    cJSON* text_document = NULL;
    cJSON* uri_item = NULL;
    cJSON* changes = NULL;
    cJSON* change = NULL;
    cJSON* text_item = NULL;
    const LspOpenDocument* open_doc = NULL;
    const char* updated_text = NULL;
    char* file_path = NULL;

    if (!store || !params || !cJSON_IsObject(params)) return;
    text_document = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    changes = cJSON_GetObjectItemCaseSensitive(params, "contentChanges");
    uri_item = text_document ? cJSON_GetObjectItemCaseSensitive(text_document, "uri") : NULL;
    if (!uri_item || !cJSON_IsString(uri_item) || !uri_item->valuestring || !changes || !cJSON_IsArray(changes)) {
        return;
    }

    cJSON_ArrayForEach(change, changes) {
        cJSON* candidate_text = cJSON_GetObjectItemCaseSensitive(change, "text");
        if (candidate_text && cJSON_IsString(candidate_text) && candidate_text->valuestring) {
            text_item = candidate_text;
        }
    }
    if (!text_item || !text_item->valuestring) return;

    open_doc = lsp_document_store_get(store, uri_item->valuestring);
    file_path = open_doc && open_doc->file_path
        ? safe_strdup(open_doc->file_path)
        : lsp_file_uri_to_path_alloc(uri_item->valuestring);
    updated_text = text_item->valuestring;
    lsp_document_store_set(store, uri_item->valuestring, file_path, updated_text);
    lsp_publish_diagnostics(uri_item->valuestring, file_path, updated_text);
    free(file_path);
}

static void lsp_handle_did_close(LspDocumentStore* store, cJSON* params) {
    cJSON* text_document = NULL;
    cJSON* uri_item = NULL;

    if (!store || !params || !cJSON_IsObject(params)) return;
    text_document = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    uri_item = text_document ? cJSON_GetObjectItemCaseSensitive(text_document, "uri") : NULL;
    if (!uri_item || !cJSON_IsString(uri_item) || !uri_item->valuestring) return;

    lsp_document_store_remove(store, uri_item->valuestring);
    lsp_publish_empty_diagnostics(uri_item->valuestring);
}

static void lsp_handle_did_save(LspDocumentStore* store, cJSON* params) {
    cJSON* text_document = NULL;
    cJSON* uri_item = NULL;
    cJSON* text_item = NULL;
    const LspOpenDocument* open_doc = NULL;
    const char* source_text = NULL;
    char* file_path = NULL;

    if (!store || !params || !cJSON_IsObject(params)) return;
    text_document = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    uri_item = text_document ? cJSON_GetObjectItemCaseSensitive(text_document, "uri") : NULL;
    text_item = cJSON_GetObjectItemCaseSensitive(params, "text");
    if (!uri_item || !cJSON_IsString(uri_item) || !uri_item->valuestring) return;

    open_doc = lsp_document_store_get(store, uri_item->valuestring);
    file_path = open_doc && open_doc->file_path
        ? safe_strdup(open_doc->file_path)
        : lsp_file_uri_to_path_alloc(uri_item->valuestring);

    if (text_item && cJSON_IsString(text_item) && text_item->valuestring) {
        source_text = text_item->valuestring;
        lsp_document_store_set(store, uri_item->valuestring, file_path, source_text);
    } else if (open_doc && open_doc->text) {
        source_text = open_doc->text;
    }

    lsp_publish_diagnostics(uri_item->valuestring, file_path, source_text);
    free(file_path);
}

static int lsp_handle_hover(LspDocumentStore* store, cJSON* id, cJSON* params) {
    cJSON* text_document = NULL;
    cJSON* position = NULL;
    cJSON* uri_item = NULL;
    cJSON* line_item = NULL;
    cJSON* char_item = NULL;
    const LspOpenDocument* open_doc = NULL;
    const char* source_text = NULL;
    char* file_path = NULL;
    Error* error = NULL;
    char* json = NULL;
    cJSON* result = NULL;

    if (!params || !cJSON_IsObject(params)) {
        return lsp_write_jsonrpc_response(id, NULL, lsp_make_jsonrpc_error(-32602, "Missing params"));
    }

    text_document = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    position = cJSON_GetObjectItemCaseSensitive(params, "position");
    uri_item = text_document ? cJSON_GetObjectItemCaseSensitive(text_document, "uri") : NULL;
    line_item = position ? cJSON_GetObjectItemCaseSensitive(position, "line") : NULL;
    char_item = position ? cJSON_GetObjectItemCaseSensitive(position, "character") : NULL;
    if (!uri_item || !cJSON_IsString(uri_item) || !uri_item->valuestring) {
        return lsp_write_jsonrpc_response(id,
                                          NULL,
                                          lsp_make_jsonrpc_error(-32602, "Missing textDocument.uri"));
    }
    if (!line_item || !cJSON_IsNumber(line_item) || !char_item || !cJSON_IsNumber(char_item)) {
        return lsp_write_jsonrpc_response(id,
                                          NULL,
                                          lsp_make_jsonrpc_error(-32602, "Missing position.line/character"));
    }

    open_doc = lsp_document_store_get(store, uri_item->valuestring);
    file_path = open_doc && open_doc->file_path
        ? safe_strdup(open_doc->file_path)
        : lsp_file_uri_to_path_alloc(uri_item->valuestring);
    source_text = open_doc ? open_doc->text : NULL;
    json = lsp_build_hover_json_from_source(file_path,
                                            source_text,
                                            (int)cJSON_GetNumberValue(line_item),
                                            (int)cJSON_GetNumberValue(char_item),
                                            &error);
    free(file_path);

    if (!json) {
        const char* message = (error && error->message) ? error->message : "Failed to build hover result";
        error_free(error);
        return lsp_write_jsonrpc_response(id, NULL, lsp_make_jsonrpc_error(-32003, message));
    }

    result = cJSON_Parse(json);
    free(json);
    if (!result) {
        return lsp_write_jsonrpc_response(id, NULL, lsp_make_jsonrpc_error(-32004, "Failed to encode hover result"));
    }

    return lsp_write_jsonrpc_response(id, result, NULL);
}

static int lsp_handle_definition(LspDocumentStore* store, cJSON* id, cJSON* params) {
    cJSON* text_document = NULL;
    cJSON* position = NULL;
    cJSON* uri_item = NULL;
    cJSON* line_item = NULL;
    cJSON* char_item = NULL;
    const LspOpenDocument* open_doc = NULL;
    const char* source_text = NULL;
    char* file_path = NULL;
    Error* error = NULL;
    char* json = NULL;
    cJSON* result = NULL;

    if (!params || !cJSON_IsObject(params)) {
        return lsp_write_jsonrpc_response(id, NULL, lsp_make_jsonrpc_error(-32602, "Missing params"));
    }

    text_document = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    position = cJSON_GetObjectItemCaseSensitive(params, "position");
    uri_item = text_document ? cJSON_GetObjectItemCaseSensitive(text_document, "uri") : NULL;
    line_item = position ? cJSON_GetObjectItemCaseSensitive(position, "line") : NULL;
    char_item = position ? cJSON_GetObjectItemCaseSensitive(position, "character") : NULL;
    if (!uri_item || !cJSON_IsString(uri_item) || !uri_item->valuestring) {
        return lsp_write_jsonrpc_response(id,
                                          NULL,
                                          lsp_make_jsonrpc_error(-32602, "Missing textDocument.uri"));
    }
    if (!line_item || !cJSON_IsNumber(line_item) || !char_item || !cJSON_IsNumber(char_item)) {
        return lsp_write_jsonrpc_response(id,
                                          NULL,
                                          lsp_make_jsonrpc_error(-32602, "Missing position.line/character"));
    }

    open_doc = lsp_document_store_get(store, uri_item->valuestring);
    file_path = open_doc && open_doc->file_path
        ? safe_strdup(open_doc->file_path)
        : lsp_file_uri_to_path_alloc(uri_item->valuestring);
    source_text = open_doc ? open_doc->text : NULL;
    json = lsp_build_definition_json_from_source(file_path,
                                                 source_text,
                                                 (int)cJSON_GetNumberValue(line_item),
                                                 (int)cJSON_GetNumberValue(char_item),
                                                 &error);
    free(file_path);

    if (!json) {
        const char* message = (error && error->message) ? error->message : "Failed to build definition result";
        error_free(error);
        return lsp_write_jsonrpc_response(id, NULL, lsp_make_jsonrpc_error(-32005, message));
    }

    result = cJSON_Parse(json);
    free(json);
    if (!result) {
        return lsp_write_jsonrpc_response(id, NULL, lsp_make_jsonrpc_error(-32006, "Failed to encode definition result"));
    }

    return lsp_write_jsonrpc_response(id, result, NULL);
}

static int lsp_handle_document_symbol(LspDocumentStore* store, cJSON* id, cJSON* params) {
    cJSON* text_document = NULL;
    cJSON* uri_item = NULL;
    const LspOpenDocument* open_doc = NULL;
    const char* source_text = NULL;
    char* file_path = NULL;
    Error* error = NULL;
    char* json = NULL;
    cJSON* result = NULL;

    if (!params || !cJSON_IsObject(params)) {
        return lsp_write_jsonrpc_response(id, NULL, lsp_make_jsonrpc_error(-32602, "Missing params"));
    }

    text_document = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    uri_item = text_document ? cJSON_GetObjectItemCaseSensitive(text_document, "uri") : NULL;
    if (!uri_item || !cJSON_IsString(uri_item) || !uri_item->valuestring) {
        return lsp_write_jsonrpc_response(id,
                                          NULL,
                                          lsp_make_jsonrpc_error(-32602, "Missing textDocument.uri"));
    }

    open_doc = lsp_document_store_get(store, uri_item->valuestring);
    file_path = open_doc && open_doc->file_path
        ? safe_strdup(open_doc->file_path)
        : lsp_file_uri_to_path_alloc(uri_item->valuestring);
    source_text = open_doc ? open_doc->text : NULL;
    json = lsp_build_document_symbols_json_from_source(file_path, source_text, &error);
    free(file_path);

    if (!json) {
        const char* message = (error && error->message) ? error->message : "Failed to build document symbols";
        error_free(error);
        return lsp_write_jsonrpc_response(id, NULL, lsp_make_jsonrpc_error(-32001, message));
    }

    result = cJSON_Parse(json);
    free(json);
    if (!result) {
        return lsp_write_jsonrpc_response(id, NULL, lsp_make_jsonrpc_error(-32002, "Failed to encode document symbols"));
    }

    return lsp_write_jsonrpc_response(id, result, NULL);
}

static int lsp_read_content_length(FILE* input, int* out_length) {
    char line[1024];
    int content_length = -1;

    if (!out_length) return 0;
    while (fgets(line, sizeof(line), input)) {
        if (strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0) {
            break;
        }
        if (lsp_ascii_starts_with_ignore_case(line, "Content-Length:")) {
            const char* value = line + strlen("Content-Length:");
            while (*value == ' ' || *value == '\t') value++;
            content_length = atoi(value);
        }
    }

    if (content_length < 0) return 0;
    *out_length = content_length;
    return 1;
}

static int lsp_run_stdio_server(void) {
    int shutdown_requested = 0;
    int exit_code = 1;
    LspDocumentStore docs;
    lsp_document_store_init(&docs);
    for (;;) {
        int content_length = 0;
        char* payload = NULL;
        cJSON* message = NULL;
        cJSON* id = NULL;
        cJSON* method = NULL;
        cJSON* params = NULL;

        if (!lsp_read_content_length(stdin, &content_length)) {
            exit_code = shutdown_requested ? 0 : 1;
            break;
        }
        if (content_length <= 0) {
            exit_code = 1;
            break;
        }

        payload = (char*)safe_malloc((size_t)content_length + 1);
        if ((int)fread(payload, 1, (size_t)content_length, stdin) != content_length) {
            free(payload);
            exit_code = 1;
            break;
        }
        payload[content_length] = '\0';

        message = cJSON_Parse(payload);
        free(payload);
        if (!message) {
            lsp_write_jsonrpc_response(NULL, NULL, lsp_make_jsonrpc_error(-32700, "Invalid JSON"));
            continue;
        }

        id = cJSON_GetObjectItemCaseSensitive(message, "id");
        method = cJSON_GetObjectItemCaseSensitive(message, "method");
        params = cJSON_GetObjectItemCaseSensitive(message, "params");

        if (!method || !cJSON_IsString(method) || !method->valuestring) {
            if (id) {
                lsp_write_jsonrpc_response(id, NULL, lsp_make_jsonrpc_error(-32600, "Invalid request"));
            }
            cJSON_Delete(message);
            continue;
        }

        if (strcmp(method->valuestring, "initialize") == 0) {
            lsp_handle_initialize(id);
        } else if (strcmp(method->valuestring, "initialized") == 0) {
        } else if (strcmp(method->valuestring, "textDocument/didOpen") == 0) {
            lsp_handle_did_open(&docs, params);
        } else if (strcmp(method->valuestring, "textDocument/didChange") == 0) {
            lsp_handle_did_change(&docs, params);
        } else if (strcmp(method->valuestring, "textDocument/didClose") == 0) {
            lsp_handle_did_close(&docs, params);
        } else if (strcmp(method->valuestring, "textDocument/didSave") == 0) {
            lsp_handle_did_save(&docs, params);
        } else if (strcmp(method->valuestring, "shutdown") == 0) {
            shutdown_requested = 1;
            lsp_write_jsonrpc_response(id, NULL, NULL);
        } else if (strcmp(method->valuestring, "exit") == 0) {
            cJSON_Delete(message);
            exit_code = shutdown_requested ? 0 : 1;
            break;
        } else if (strcmp(method->valuestring, "textDocument/documentSymbol") == 0) {
            lsp_handle_document_symbol(&docs, id, params);
        } else if (strcmp(method->valuestring, "textDocument/hover") == 0) {
            lsp_handle_hover(&docs, id, params);
        } else if (strcmp(method->valuestring, "textDocument/definition") == 0) {
            lsp_handle_definition(&docs, id, params);
        } else if (id) {
            lsp_write_jsonrpc_response(id, NULL, lsp_make_jsonrpc_error(-32601, "Method not found"));
        }

        cJSON_Delete(message);
    }

    lsp_document_store_free(&docs);
    return exit_code;
}

static void print_lsp_usage(const char* program_name) {
    printf("Usage: %s lsp <subcommand>\n", program_name);
    printf("Subcommands:\n");
    printf("  --stdio           Run a stdio LSP server (initialize/shutdown/documentSymbol/hover/definition/diagnostics)\n");
    printf("  symbols <file>    Print document symbols JSON for a source file\n");
}

int cli_lsp(int argc, char** argv) {
    if (argc < 3 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0) {
        print_lsp_usage(argv[0]);
        return argc < 3 ? 1 : 0;
    }

    if (strcmp(argv[2], "--stdio") == 0) {
        return lsp_run_stdio_server();
    }

    if (strcmp(argv[2], "symbols") == 0) {
        Error* error = NULL;
        char* json = NULL;
        if (argc < 4) {
            fprintf(stderr, "Error: missing file path for 'lsp symbols'\n");
            print_lsp_usage(argv[0]);
            return 1;
        }
        json = lsp_build_document_symbols_json(argv[3], &error);
        if (!json) {
            fprintf(stderr, "Error: %s\n", (error && error->message) ? error->message : "failed to build symbols");
            error_free(error);
            return 1;
        }
        printf("%s\n", json);
        free(json);
        return 0;
    }

    fprintf(stderr, "Error: unknown lsp subcommand '%s'\n", argv[2]);
    print_lsp_usage(argv[0]);
    return 1;
}
