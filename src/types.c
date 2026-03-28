#include "types.h"
#include "safe_alloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void record_def_retain(RecordDef* def);
void record_def_release(RecordDef* def);
void interface_def_retain(InterfaceDef* def);
void interface_def_release(InterfaceDef* def);

Type* type_int(void) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_INT;
    t->nullable = false;
    // Union member - initializing element_type sets all union members to NULL
    t->element_type = NULL;
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

Type* type_bool(void) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_BOOL;
    t->nullable = false;
    // Union member - initializing element_type sets all union members to NULL
    t->element_type = NULL;
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

Type* type_double(void) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_DOUBLE;
    t->nullable = false;
    // Union member - initializing element_type sets all union members to NULL
    t->element_type = NULL;
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

Type* type_bigint(void) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_BIGINT;
    t->nullable = false;
    // Union member - initializing element_type sets all union members to NULL
    t->element_type = NULL;
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

Type* type_string(void) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_STRING;
    t->nullable = false;
    // Union member - initializing element_type sets all union members to NULL
    t->element_type = NULL;
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

Type* type_bytes(void) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_BYTES;
    t->nullable = false;
    // Union member - initializing element_type sets all union members to NULL
    t->element_type = NULL;
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

Type* type_array(Type* element_type) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_ARRAY;
    t->nullable = false;
    // Union member: element_type shares memory with return_type, record_def, etc.
    t->element_type = element_type;
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

Type* type_nil(void) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_NIL;
    t->nullable = false;
    // Union member - initializing element_type sets all union members to NULL
    t->element_type = NULL;
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

Type* type_void(void) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_VOID;
    t->nullable = false;
    // Union member - initializing element_type sets all union members to NULL
    t->element_type = NULL;
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

Type* type_function(Type* return_type, Type** param_types, int param_count) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_FUNCTION;
    t->nullable = false;
    // Union member: return_type shares memory with element_type, record_def, etc.
    t->return_type = return_type;
    t->param_types = param_types;
    t->param_count = param_count;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

Type* type_future(Type* value_type) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_FUTURE;
    t->nullable = false;
    t->element_type = value_type;
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

Type* type_any(void) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_ANY;
    t->nullable = false;
    // Union member - initializing element_type sets all union members to NULL
    t->element_type = NULL;
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

Type* type_type_param(const char* name) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_TYPE_PARAM;
    t->nullable = false;
    t->type_param_name = name ? safe_strdup(name) : NULL;
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

void type_function_set_type_params(Type* function_type,
                                   char** type_param_names,
                                   Type** type_param_constraints,
                                   int type_param_count) {
    if (!function_type || function_type->kind != TYPE_FUNCTION) return;

    if (function_type->type_param_names) {
        for (int i = 0; i < function_type->type_param_count; i++) {
            if (function_type->type_param_names[i]) free(function_type->type_param_names[i]);
        }
        free(function_type->type_param_names);
        function_type->type_param_names = NULL;
    }
    if (function_type->type_param_constraints) {
        for (int i = 0; i < function_type->type_param_count; i++) {
            if (function_type->type_param_constraints[i]) {
                type_free(function_type->type_param_constraints[i]);
            }
        }
        free(function_type->type_param_constraints);
        function_type->type_param_constraints = NULL;
    }
    function_type->type_param_count = 0;

    if (!type_param_names || type_param_count <= 0) return;

    function_type->type_param_names = (char**)safe_malloc((size_t)type_param_count * sizeof(char*));
    function_type->type_param_constraints = (Type**)safe_calloc((size_t)type_param_count, sizeof(Type*));
    function_type->type_param_count = type_param_count;
    for (int i = 0; i < type_param_count; i++) {
        function_type->type_param_names[i] = type_param_names[i] ? safe_strdup(type_param_names[i]) : safe_strdup("");
        function_type->type_param_constraints[i] = (type_param_constraints && type_param_constraints[i])
            ? type_clone(type_param_constraints[i])
            : NULL;
    }
}

Type* type_clone(Type* type) {
    if (!type) return NULL;

    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = type->kind;
    t->nullable = type->nullable;
    // Union member: setting element_type to NULL initializes all union members
    t->element_type = NULL;
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;

    switch (type->kind) {
        case TYPE_INT:
            t->type_param_name = type->type_param_name ? safe_strdup(type->type_param_name) : NULL;
            break;
        case TYPE_ARRAY:
            t->element_type = type_clone(type->element_type);
            break;
        case TYPE_FUTURE:
            t->element_type = type_clone(type->element_type);
            break;
        case TYPE_FUNCTION:
            t->return_type = type_clone(type->return_type);
            t->param_count = type->param_count;
            if (type->param_count > 0) {
                t->param_types = (Type**)safe_malloc(type->param_count * sizeof(Type*));
                for (int i = 0; i < type->param_count; i++) {
                    t->param_types[i] = type_clone(type->param_types[i]);
                }
            }
            if (type->type_param_count > 0 && type->type_param_names) {
                t->type_param_names = (char**)safe_malloc((size_t)type->type_param_count * sizeof(char*));
                t->type_param_constraints = (Type**)safe_calloc((size_t)type->type_param_count, sizeof(Type*));
                t->type_param_count = type->type_param_count;
                for (int i = 0; i < type->type_param_count; i++) {
                    t->type_param_names[i] = type->type_param_names[i]
                        ? safe_strdup(type->type_param_names[i])
                        : safe_strdup("");
                    t->type_param_constraints[i] = (type->type_param_constraints && type->type_param_constraints[i])
                        ? type_clone(type->type_param_constraints[i])
                        : NULL;
                }
            }
            break;
        case TYPE_TYPE_PARAM:
            t->type_param_name = type->type_param_name ? safe_strdup(type->type_param_name) : NULL;
            break;
        case TYPE_TUPLE:
            if (type->tuple_def) {
                t->tuple_def = tuple_type_create(type->tuple_def->element_types, type->tuple_def->element_count);
            }
            break;
        case TYPE_MAP:
            if (type->map_def) {
                t->map_def = map_type_create(type->map_def->key_type, type->map_def->value_type);
            }
            break;
        case TYPE_SET:
            if (type->set_def) {
                t->set_def = set_type_create(type->set_def->element_type);
            }
            break;
        case TYPE_RECORD:
            if (type->record_def) {
                t->record_def = type->record_def;
                record_def_retain(t->record_def);
            }
            t->param_count = type->param_count;
            if (type->param_count > 0 && type->param_types) {
                t->param_types = (Type**)safe_malloc((size_t)type->param_count * sizeof(Type*));
                for (int i = 0; i < type->param_count; i++) {
                    t->param_types[i] = type_clone(type->param_types[i]);
                }
            }
            break;
        case TYPE_INTERFACE:
            if (type->interface_def) {
                t->interface_def = type->interface_def;
                interface_def_retain(t->interface_def);
            }
            break;
        default:
            break;
    }

    return t;
}

Type* type_nullable(Type* type) {
    Type* t = type_clone(type);
    if (t) t->nullable = true;
    return t;
}

bool type_equals(Type* a, Type* b) {
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    if (a->nullable != b->nullable) return false;
    
    switch (a->kind) {
        case TYPE_ARRAY:
            return type_equals(a->element_type, b->element_type);
        case TYPE_FUTURE:
            return type_equals(a->element_type, b->element_type);
        case TYPE_FUNCTION:
            if (a->param_count != b->param_count) return false;
            if (a->type_param_count != b->type_param_count) return false;
            for (int i = 0; i < a->type_param_count; i++) {
                const char* an = (a->type_param_names && a->type_param_names[i]) ? a->type_param_names[i] : "";
                const char* bn = (b->type_param_names && b->type_param_names[i]) ? b->type_param_names[i] : "";
                if (strcmp(an, bn) != 0) return false;
                Type* ac = (a->type_param_constraints && i < a->type_param_count)
                    ? a->type_param_constraints[i]
                    : NULL;
                Type* bc = (b->type_param_constraints && i < b->type_param_count)
                    ? b->type_param_constraints[i]
                    : NULL;
                if (ac || bc) {
                    if (!ac || !bc) return false;
                    if (!type_equals(ac, bc)) return false;
                }
            }
            if (!type_equals(a->return_type, b->return_type)) return false;
            for (int i = 0; i < a->param_count; i++) {
                if (!type_equals(a->param_types[i], b->param_types[i])) return false;
            }
            return true;
        case TYPE_TYPE_PARAM: {
            const char* an = a->type_param_name ? a->type_param_name : "";
            const char* bn = b->type_param_name ? b->type_param_name : "";
            return strcmp(an, bn) == 0;
        }
        case TYPE_RECORD:
            // Two record types are equal if they have the same nominal definition
            // and equal type arguments (for generic records).
            if (a->record_def != b->record_def) return false;
            if (a->param_count != b->param_count) return false;
            if (a->param_count > 0 && (!a->param_types || !b->param_types)) return false;
            for (int i = 0; i < a->param_count; i++) {
                if (!type_equals(a->param_types[i], b->param_types[i])) return false;
            }
            return true;
        case TYPE_INTERFACE:
            // Two interface types are equal if they have the same definition
            return a->interface_def == b->interface_def;
        case TYPE_TUPLE:
            if (tuple_type_get_arity(a) != tuple_type_get_arity(b)) return false;
            for (int i = 0; i < tuple_type_get_arity(a); i++) {
                if (!type_equals(tuple_type_get_element(a, i), tuple_type_get_element(b, i))) return false;
            }
            return true;
        case TYPE_MAP:
            if (!type_equals(a->map_def->key_type, b->map_def->key_type)) return false;
            if (!type_equals(a->map_def->value_type, b->map_def->value_type)) return false;
            return true;
        case TYPE_SET:
            if (!type_equals(a->set_def->element_type, b->set_def->element_type)) return false;
            return true;
        default:
            return true;
    }
}

bool type_assignable(Type* to, Type* from) {
    if (!to || !from) return false;
    if (to->nullable && from->kind == TYPE_NIL) return true;
    if (to->kind == TYPE_ANY || from->kind == TYPE_ANY) return true;
    if (from->kind == TYPE_NIL) return to->nullable;
    if (to->kind != from->kind) return false;
    if (!to->nullable && from->nullable) return false;
    
    switch (to->kind) {
        case TYPE_ARRAY:
            return type_assignable(to->element_type, from->element_type);
        case TYPE_FUTURE:
            return type_assignable(to->element_type, from->element_type);
        case TYPE_FUNCTION:
            if (to->param_count != from->param_count) return false;
            if (to->type_param_count != from->type_param_count) return false;
            for (int i = 0; i < to->type_param_count; i++) {
                const char* tn = (to->type_param_names && to->type_param_names[i]) ? to->type_param_names[i] : "";
                const char* fn = (from->type_param_names && from->type_param_names[i]) ? from->type_param_names[i] : "";
                if (strcmp(tn, fn) != 0) return false;
                Type* tc = (to->type_param_constraints && i < to->type_param_count)
                    ? to->type_param_constraints[i]
                    : NULL;
                Type* fc = (from->type_param_constraints && i < from->type_param_count)
                    ? from->type_param_constraints[i]
                    : NULL;
                if (tc || fc) {
                    if (!tc || !fc) return false;
                    if (!type_assignable(tc, fc)) return false;
                }
            }
            if (!type_assignable(to->return_type, from->return_type)) return false;
            for (int i = 0; i < to->param_count; i++) {
                if (!type_assignable(to->param_types[i], from->param_types[i])) return false;
            }
            return true;
        case TYPE_TYPE_PARAM: {
            const char* tn = to->type_param_name ? to->type_param_name : "";
            const char* fn = from->type_param_name ? from->type_param_name : "";
            return strcmp(tn, fn) == 0;
        }
        case TYPE_RECORD:
            // Records are assignable if they have the same definition (reference equality)
            // or if the target is a supertype (not implemented yet)
            if (to->record_def != from->record_def) return false;
            if (to->param_count != from->param_count) return false;
            if (to->param_count > 0 && (!to->param_types || !from->param_types)) return false;
            for (int i = 0; i < to->param_count; i++) {
                if (!type_assignable(to->param_types[i], from->param_types[i])) return false;
            }
            return true;
        case TYPE_INTERFACE:
            // Interface to interface assignment is nominal for now.
            return to->interface_def == from->interface_def;
        case TYPE_TUPLE:
            if (tuple_type_get_arity(to) != tuple_type_get_arity(from)) return false;
            for (int i = 0; i < tuple_type_get_arity(to); i++) {
                if (!type_assignable(tuple_type_get_element(to, i), tuple_type_get_element(from, i))) return false;
            }
            return true;
        case TYPE_MAP:
            if (!type_assignable(to->map_def->key_type, from->map_def->key_type)) return false;
            if (!type_assignable(to->map_def->value_type, from->map_def->value_type)) return false;
            return true;
        case TYPE_SET:
            if (!type_assignable(to->set_def->element_type, from->set_def->element_type)) return false;
            return true;
        default:
            return true;
    }
}

void type_to_string(Type* type, char* buffer, size_t buffer_size) {
    if (!type || !buffer || buffer_size == 0) return;
    buffer[0] = '\0';
    
    switch (type->kind) {
        case TYPE_INT: snprintf(buffer, buffer_size, "int%s", type->nullable ? "?" : ""); break;
        case TYPE_BOOL: snprintf(buffer, buffer_size, "bool%s", type->nullable ? "?" : ""); break;
        case TYPE_DOUBLE: snprintf(buffer, buffer_size, "double%s", type->nullable ? "?" : ""); break;
        case TYPE_BIGINT: snprintf(buffer, buffer_size, "bigint%s", type->nullable ? "?" : ""); break;
        case TYPE_STRING: snprintf(buffer, buffer_size, "string%s", type->nullable ? "?" : ""); break;
        case TYPE_BYTES: snprintf(buffer, buffer_size, "bytes%s", type->nullable ? "?" : ""); break;
        case TYPE_ARRAY: {
            char elem_buf[256];
            type_to_string(type->element_type, elem_buf, sizeof(elem_buf));
            snprintf(buffer, buffer_size, "array<%s>%s", elem_buf, type->nullable ? "?" : "");
            break;
        }
        case TYPE_NIL: snprintf(buffer, buffer_size, "nil"); break;
        case TYPE_VOID: snprintf(buffer, buffer_size, "void"); break;
        case TYPE_FUTURE: {
            char elem_buf[256];
            type_to_string(type->element_type, elem_buf, sizeof(elem_buf));
            snprintf(buffer, buffer_size, "Future<%s>%s", elem_buf, type->nullable ? "?" : "");
            break;
        }
        case TYPE_FUNCTION: {
            char type_params[256] = "";
            if (type->type_param_count > 0 && type->type_param_names) {
                strncat(type_params, "[", sizeof(type_params) - strlen(type_params) - 1);
                for (int i = 0; i < type->type_param_count; i++) {
                    if (i > 0) strncat(type_params, ", ", sizeof(type_params) - strlen(type_params) - 1);
                    if (type->type_param_names[i]) {
                        strncat(type_params, type->type_param_names[i], sizeof(type_params) - strlen(type_params) - 1);
                    }
                    if (type->type_param_constraints &&
                        type->type_param_constraints[i]) {
                        char constraint_buf[128];
                        type_to_string(type->type_param_constraints[i], constraint_buf, sizeof(constraint_buf));
                        strncat(type_params, ": ", sizeof(type_params) - strlen(type_params) - 1);
                        strncat(type_params, constraint_buf, sizeof(type_params) - strlen(type_params) - 1);
                    }
                }
                strncat(type_params, "]", sizeof(type_params) - strlen(type_params) - 1);
            }
            char params[512] = "";
            for (int i = 0; i < type->param_count; i++) {
                char param_buf[256];
                type_to_string(type->param_types[i], param_buf, sizeof(param_buf));
                if (i > 0) strncat(params, ", ", sizeof(params) - strlen(params) - 1);
                strncat(params, param_buf, sizeof(params) - strlen(params) - 1);
            }
            char return_buf[256];
            type_to_string(type->return_type, return_buf, sizeof(return_buf));
            snprintf(buffer, buffer_size, "func%s(%s): %s", type_params, params, return_buf);
            break;
        }
        case TYPE_TYPE_PARAM:
            snprintf(buffer, buffer_size, "%s%s", type->type_param_name ? type->type_param_name : "T", type->nullable ? "?" : "");
            break;
        case TYPE_ANY: snprintf(buffer, buffer_size, "any"); break;
        case TYPE_RECORD: {
            char type_args[512] = "";
            if (type->param_count > 0 && type->param_types) {
                strncat(type_args, "[", sizeof(type_args) - strlen(type_args) - 1);
                for (int i = 0; i < type->param_count; i++) {
                    char arg_buf[256];
                    type_to_string(type->param_types[i], arg_buf, sizeof(arg_buf));
                    if (i > 0) strncat(type_args, ", ", sizeof(type_args) - strlen(type_args) - 1);
                    strncat(type_args, arg_buf, sizeof(type_args) - strlen(type_args) - 1);
                }
                strncat(type_args, "]", sizeof(type_args) - strlen(type_args) - 1);
            }

            if (type->record_def && type->record_def->name) {
                bool name_already_specialized = strchr(type->record_def->name, '[') != NULL;
                if (name_already_specialized || type_args[0] == '\0') {
                    snprintf(buffer, buffer_size, "record %s%s", type->record_def->name, type->nullable ? "?" : "");
                } else {
                    snprintf(buffer, buffer_size, "record %s%s%s", type->record_def->name, type_args, type->nullable ? "?" : "");
                }
            } else {
                snprintf(buffer, buffer_size, "record%s%s", type_args, type->nullable ? "?" : "");
            }
            break;
        }
        case TYPE_INTERFACE: {
            if (type->interface_def && type->interface_def->name) {
                snprintf(buffer, buffer_size, "interface %s%s", type->interface_def->name, type->nullable ? "?" : "");
            } else {
                snprintf(buffer, buffer_size, "interface%s", type->nullable ? "?" : "");
            }
            break;
        }
        case TYPE_TUPLE: {
            char elements[512] = "";
            for (int i = 0; i < tuple_type_get_arity(type); i++) {
                char elem_buf[256];
                type_to_string(tuple_type_get_element(type, i), elem_buf, sizeof(elem_buf));
                if (i > 0) strncat(elements, ", ", sizeof(elements) - strlen(elements) - 1);
                strncat(elements, elem_buf, sizeof(elements) - strlen(elements) - 1);
            }
            snprintf(buffer, buffer_size, "(%s)%s", elements, type->nullable ? "?" : "");
            break;
        }
        case TYPE_MAP: {
            char key_buf[256], val_buf[256];
            type_to_string(type->map_def->key_type, key_buf, sizeof(key_buf));
            type_to_string(type->map_def->value_type, val_buf, sizeof(val_buf));
            snprintf(buffer, buffer_size, "map<%s, %s>%s", key_buf, val_buf, type->nullable ? "?" : "");
            break;
        }
        case TYPE_SET: {
            char elem_buf[256];
            type_to_string(type->set_def->element_type, elem_buf, sizeof(elem_buf));
            snprintf(buffer, buffer_size, "set<%s>%s", elem_buf, type->nullable ? "?" : "");
            break;
        }
        default: snprintf(buffer, buffer_size, "unknown"); break;
    }
}

void type_free(Type* type) {
    if (!type) return;
    // CRITICAL: element_type, return_type, record_def, tuple_def, map_def, and set_def
    // are all in a union and share the same memory location.
    // We MUST only access the member corresponding to the actual type kind,
    // otherwise we would access garbage data or cause double-free errors.
    switch (type->kind) {
        case TYPE_ARRAY:
            if (type->element_type) type_free(type->element_type);
            break;
        case TYPE_FUTURE:
            if (type->element_type) type_free(type->element_type);
            break;
        case TYPE_FUNCTION:
            if (type->return_type) type_free(type->return_type);
            break;
        case TYPE_INT:
            if (type->type_param_name) free(type->type_param_name);
            break;
        case TYPE_TYPE_PARAM:
            if (type->type_param_name) free(type->type_param_name);
            break;
        default:
            // Primitive types, records, tuples, maps, sets don't have element_type/return_type
            break;
    }
    if (type->param_types) {
        for (int i = 0; i < type->param_count; i++) {
            type_free(type->param_types[i]);
        }
        free(type->param_types);
    }
    if (type->kind == TYPE_RECORD && type->record_def) {
        record_def_release(type->record_def);
    }
    if (type->kind == TYPE_INTERFACE && type->interface_def) {
        interface_def_release(type->interface_def);
    }
    if (type->kind == TYPE_TUPLE && type->tuple_def) {
        tuple_type_free(type->tuple_def);
    }
    if (type->kind == TYPE_MAP && type->map_def) {
        map_type_free(type->map_def);
    }
    if (type->kind == TYPE_SET && type->set_def) {
        set_type_free(type->set_def);
    }
    if (type->type_param_names) {
        for (int i = 0; i < type->type_param_count; i++) {
            if (type->type_param_names[i]) free(type->type_param_names[i]);
        }
        free(type->type_param_names);
    }
    if (type->type_param_constraints) {
        for (int i = 0; i < type->type_param_count; i++) {
            if (type->type_param_constraints[i]) {
                type_free(type->type_param_constraints[i]);
            }
        }
        free(type->type_param_constraints);
    }
    free(type);
}

Type* type_record(const char* name) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_RECORD;
    t->nullable = false;
    // Union member: record_def shares memory with element_type, return_type, etc.
    t->record_def = record_def_create(name);
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

Type* type_interface(const char* name) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_INTERFACE;
    t->nullable = false;
    // Union member: interface_def shares memory with element_type, return_type, etc.
    t->interface_def = interface_def_create(name);
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

RecordDef* record_def_create(const char* name) {
    RecordDef* def = (RecordDef*)safe_malloc(sizeof(RecordDef));
    def->name = name ? safe_strdup(name) : NULL;
    def->fields = NULL;
    def->field_count = 0;
    def->field_capacity = 0;
    def->is_native_opaque = false;
    def->ref_count = 1;
    return def;
}

void record_def_retain(RecordDef* def) {
    if (!def) return;
    def->ref_count++;
}

void record_def_release(RecordDef* def) {
    if (!def) return;
    def->ref_count--;
    if (def->ref_count <= 0) {
        record_def_free(def);
    }
}

void record_def_free(RecordDef* def) {
    if (!def) return;
    if (def->name) free(def->name);
    if (def->fields) {
        for (int i = 0; i < def->field_count; i++) {
            if (def->fields[i].name) free(def->fields[i].name);
            if (def->fields[i].type) type_free(def->fields[i].type);
        }
        free(def->fields);
    }
    free(def);
}

void record_def_add_field(RecordDef* def, const char* name, Type* type) {
    if (!def || !name || !type) return;
    
    def->field_count++;
    if (def->field_count > def->field_capacity) {
        def->field_capacity = def->field_count * 2;
        def->fields = (RecordField*)safe_realloc(def->fields, def->field_capacity * sizeof(RecordField));
    }
    
    RecordField* field = &def->fields[def->field_count - 1];
    field->name = safe_strdup(name);
    field->type = type_clone(type);
    field->index = def->field_count - 1;
}

int record_def_get_field_index(RecordDef* def, const char* name) {
    if (!def || !name) return -1;
    for (int i = 0; i < def->field_count; i++) {
        if (strcmp(def->fields[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

RecordField* record_def_get_field(RecordDef* def, int index) {
    if (!def || index < 0 || index >= def->field_count) return NULL;
    return &def->fields[index];
}

Type* record_def_get_field_type(RecordDef* def, const char* name) {
    int index = record_def_get_field_index(def, name);
    if (index < 0) return NULL;
    return def->fields[index].type;
}

InterfaceDef* interface_def_create(const char* name) {
    InterfaceDef* def = (InterfaceDef*)safe_malloc(sizeof(InterfaceDef));
    def->name = name ? safe_strdup(name) : NULL;
    def->methods = NULL;
    def->method_count = 0;
    def->method_capacity = 0;
    def->ref_count = 1;
    return def;
}

void interface_def_retain(InterfaceDef* def) {
    if (!def) return;
    def->ref_count++;
}

void interface_def_release(InterfaceDef* def) {
    if (!def) return;
    def->ref_count--;
    if (def->ref_count <= 0) {
        interface_def_free(def);
    }
}

void interface_def_free(InterfaceDef* def) {
    if (!def) return;
    if (def->name) free(def->name);
    if (def->methods) {
        for (int i = 0; i < def->method_count; i++) {
            if (def->methods[i].name) free(def->methods[i].name);
            if (def->methods[i].type) type_free(def->methods[i].type);
        }
        free(def->methods);
    }
    free(def);
}

void interface_def_add_method(InterfaceDef* def, const char* name, Type* type) {
    if (!def || !name || !type) return;

    def->method_count++;
    if (def->method_count > def->method_capacity) {
        def->method_capacity = def->method_count * 2;
        def->methods = (InterfaceMethod*)safe_realloc(def->methods, (size_t)def->method_capacity * sizeof(InterfaceMethod));
    }

    InterfaceMethod* method = &def->methods[def->method_count - 1];
    method->name = safe_strdup(name);
    method->type = type_clone(type);
}

int interface_def_get_method_index(InterfaceDef* def, const char* name) {
    if (!def || !name) return -1;
    for (int i = 0; i < def->method_count; i++) {
        if (strcmp(def->methods[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

InterfaceMethod* interface_def_get_method(InterfaceDef* def, int index) {
    if (!def || index < 0 || index >= def->method_count) return NULL;
    return &def->methods[index];
}

Type* interface_def_get_method_type(InterfaceDef* def, const char* name) {
    int index = interface_def_get_method_index(def, name);
    if (index < 0) return NULL;
    return def->methods[index].type;
}

Symbol* symbol_create(Type* type, const char* name, bool is_mutable) {
    Symbol* sym = (Symbol*)safe_malloc(sizeof(Symbol));
    sym->type = type;
    sym->name = name ? safe_strdup(name) : NULL;
    sym->is_mutable = is_mutable;
    sym->is_type_alias = false;
    sym->is_public = true;
    sym->decl_file = NULL;
    sym->function_obj = NULL;
    return sym;
}

void symbol_free(Symbol* sym) {
    if (!sym) return;
    if (sym->name) free(sym->name);
    if (sym->decl_file) free(sym->decl_file);
    if (sym->type) type_free(sym->type);
    free(sym);
}

// Map type implementation
Type* type_map(Type* key_type, Type* value_type) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_MAP;
    t->nullable = false;
    // Union member: map_def shares memory with element_type, return_type, etc.
    t->map_def = map_type_create(key_type, value_type);
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

MapType* map_type_create(Type* key_type, Type* value_type) {
    MapType* map = (MapType*)safe_malloc(sizeof(MapType));
    map->key_type = type_clone(key_type);
    map->value_type = type_clone(value_type);
    return map;
}

void map_type_free(MapType* map) {
    if (!map) return;
    if (map->key_type) type_free(map->key_type);
    if (map->value_type) type_free(map->value_type);
    free(map);
}

// Set type implementation
Type* type_set(Type* element_type) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_SET;
    t->nullable = false;
    // Union member: set_def shares memory with element_type, return_type, etc.
    t->set_def = set_type_create(element_type);
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

SetType* set_type_create(Type* element_type) {
    SetType* set = (SetType*)safe_malloc(sizeof(SetType));
    set->element_type = type_clone(element_type);
    return set;
}

void set_type_free(SetType* set) {
    if (!set) return;
    if (set->element_type) type_free(set->element_type);
    free(set);
}

// Tuple type implementation
Type* type_tuple(Type** element_types, int element_count) {
    Type* t = (Type*)safe_malloc(sizeof(Type));
    t->kind = TYPE_TUPLE;
    t->nullable = false;
    // Union member: tuple_def shares memory with element_type, return_type, etc.
    t->tuple_def = tuple_type_create(element_types, element_count);
    t->param_types = NULL;
    t->param_count = 0;
    t->type_param_names = NULL;
    t->type_param_constraints = NULL;
    t->type_param_count = 0;
    return t;
}

TupleType* tuple_type_create(Type** element_types, int element_count) {
    TupleType* tuple = (TupleType*)safe_malloc(sizeof(TupleType));
    tuple->element_count = element_count;
    if (element_count > 0 && element_types) {
        tuple->element_types = (Type**)safe_malloc(element_count * sizeof(Type*));
        for (int i = 0; i < element_count; i++) {
            tuple->element_types[i] = type_clone(element_types[i]);
        }
    } else {
        tuple->element_types = NULL;
    }
    return tuple;
}

void tuple_type_free(TupleType* tuple) {
    if (!tuple) return;
    if (tuple->element_types) {
        for (int i = 0; i < tuple->element_count; i++) {
            if (tuple->element_types[i]) {
                type_free(tuple->element_types[i]);
            }
        }
        free(tuple->element_types);
    }
    free(tuple);
}

int tuple_type_get_arity(Type* type) {
    if (!type || type->kind != TYPE_TUPLE || !type->tuple_def) return 0;
    return type->tuple_def->element_count;
}

Type* tuple_type_get_element(Type* type, int index) {
    if (!type || type->kind != TYPE_TUPLE || !type->tuple_def) return NULL;
    if (index < 0 || index >= type->tuple_def->element_count) return NULL;
    return type->tuple_def->element_types[index];
}


