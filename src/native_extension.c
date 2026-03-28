#include "native_extension.h"
#include "safe_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
typedef HMODULE NativeExtensionLibraryHandle;
#else
#include <dlfcn.h>
typedef void* NativeExtensionLibraryHandle;
#endif

typedef struct {
    char* name;
    TabloExtHandleDestroyFn destroy;
} NativeExtensionHandleType;

typedef struct {
    char* name;
    TabloExtTypeDesc result_type;
    TabloExtTypeDesc* param_types;
    int param_count;
    TabloExtFunctionCallback callback;
} NativeExtensionFunction;

typedef struct {
    char* path;
    NativeExtensionLibraryHandle library;
    TabloExtShutdownFn shutdown_fn;
    bool shutdown_called;
    NativeExtensionHandleType* handle_types;
    int handle_type_count;
    int handle_type_capacity;
    NativeExtensionFunction* functions;
    int function_count;
    int function_capacity;
} NativeExtension;

struct TabloExtCallback {
    mtx_t mutex;
    int ref_count;
    int posted_pending_count;
    VM* vm;
    VmPostedEventQueue* posted_event_queue;
    TabloExtTypeDesc type;
    Value callable;
};

typedef struct {
    TabloExtCallback* callback;
    TabloExtValue* args;
    int arg_count;
    TabloExtCallbackGate* gate;
    uint64_t gate_generation;
    bool counted_pending;
} NativeExtensionPostedCallbackEvent;

struct TabloExtCallbackGate {
    mtx_t mutex;
    int ref_count;
    uint64_t generation;
    int64_t invalidated_count;
};

struct NativeExtensionRegistry {
    NativeExtension* extensions;
    int extension_count;
    int extension_capacity;
};

typedef struct {
    NativeExtensionRegistry* registry;
    NativeExtension* extension;
} NativeExtensionLoadContext;

typedef struct {
    TabloExtValue* items;
    int count;
} NativeExtensionTempArray;

typedef struct {
    TabloExtMapEntry* entries;
    int count;
} NativeExtensionTempMap;

typedef struct {
    VM* vm;
    NativeExtensionRegistry* registry;
    NativeExtensionFunction* function;
    int result_slot;
    int arg_count;
    bool result_set;
    NativeExtensionTempArray* temp_arrays;
    int temp_array_count;
    int temp_array_capacity;
    NativeExtensionTempMap* temp_maps;
    int temp_map_count;
    int temp_map_capacity;
    TabloExtCallback** temp_callbacks;
    int temp_callback_count;
    int temp_callback_capacity;
    Value* temp_values;
    int temp_value_count;
    int temp_value_capacity;
} NativeExtensionCallState;

static void native_extension_set_error(char* error_buf, size_t error_buf_size, const char* message) {
    if (!error_buf || error_buf_size == 0) return;
    snprintf(error_buf, error_buf_size, "%s", message ? message : "Unknown extension error");
}

static char* native_extension_strdup(const char* text) {
    return text ? safe_strdup(text) : NULL;
}

static bool native_extension_type_is_valid(const TabloExtTypeDesc* type, char* error_buf, size_t error_buf_size);
static void native_extension_callback_retain_impl(const TabloExtCallback* callback);
static void native_extension_callback_release_impl(const TabloExtCallback* callback);
static void native_extension_callback_increment_pending_impl(const TabloExtCallback* callback);
static void native_extension_callback_decrement_pending_impl(const TabloExtCallback* callback);
static bool native_extension_post_callback_impl(const TabloExtCallback* callback,
                                                const TabloExtValue* args,
                                                int arg_count,
                                                char* error_buf,
                                                size_t error_buf_size);
static TabloExtCallbackGate* native_extension_create_callback_gate_impl(void);
static void native_extension_callback_gate_retain_impl(TabloExtCallbackGate* gate);
static void native_extension_callback_gate_release_impl(TabloExtCallbackGate* gate);
static uint64_t native_extension_callback_gate_reset_impl(TabloExtCallbackGate* gate);
static int64_t native_extension_callback_gate_get_invalidated_count_impl(const TabloExtCallbackGate* gate);
static int64_t native_extension_callback_gate_reset_invalidated_count_impl(TabloExtCallbackGate* gate);
static bool native_extension_post_callback_gated_impl(const TabloExtCallback* callback,
                                                      const TabloExtValue* args,
                                                      int arg_count,
                                                      const TabloExtCallbackGate* gate,
                                                      uint64_t gate_generation,
                                                      char* error_buf,
                                                      size_t error_buf_size);
static int native_extension_get_posted_callback_pending_count_impl(const TabloExtCallback* callback);
static bool native_extension_is_posted_callback_queue_open_impl(const TabloExtCallback* callback);
static bool native_extension_convert_callback_arg_to_value(NativeExtensionCallState* state,
                                                           const TabloExtValue* ext_value,
                                                           const TabloExtTypeDesc* expected_type,
                                                           Value* out_value);
static void native_extension_call_state_free_temp_values(NativeExtensionCallState* state);
static void native_extension_temp_map_value_free(TabloExtValue* value);
static void native_extension_posted_callback_value_free(TabloExtValue* value);

static bool native_extension_callback_value_type_is_valid(const TabloExtTypeDesc* type,
                                                          bool allow_void,
                                                          bool allow_array,
                                                          bool allow_tuple,
                                                          bool allow_map,
                                                          char* error_buf,
                                                          size_t error_buf_size) {
    if (!type) {
        native_extension_set_error(error_buf, error_buf_size, "Missing callback type descriptor");
        return false;
    }

    if (type->tag == TABLO_EXT_TYPE_VOID) {
        if (!allow_void) {
            native_extension_set_error(error_buf, error_buf_size, "Callback parameter types cannot be void");
            return false;
        }
        return true;
    }

    if (type->tag == TABLO_EXT_TYPE_ARRAY) {
        if (!allow_array) {
            native_extension_set_error(error_buf,
                                       error_buf_size,
                                       "Callback signatures do not support array values in this position");
            return false;
        }
        return native_extension_type_is_valid(type, error_buf, error_buf_size);
    }

    if (type->tag == TABLO_EXT_TYPE_CALLBACK) {
        native_extension_set_error(error_buf,
                                   error_buf_size,
                                   "Callback signatures do not support nested callback values");
        return false;
    }

    if (type->tag == TABLO_EXT_TYPE_TUPLE) {
        if (!allow_tuple) {
            native_extension_set_error(error_buf,
                                       error_buf_size,
                                       "Callback results do not support tuple values");
            return false;
        }
        if (type->tuple_element_count < 0) {
            native_extension_set_error(error_buf, error_buf_size, "Callback tuple types require a non-negative element count");
            return false;
        }
        if (type->tuple_element_count > 0 && !type->tuple_element_types) {
            native_extension_set_error(error_buf, error_buf_size, "Callback tuple types require element types");
            return false;
        }
        for (int i = 0; i < type->tuple_element_count; i++) {
            const TabloExtTypeDesc* element_type = &type->tuple_element_types[i];
            if (!native_extension_callback_value_type_is_valid(element_type,
                                                               false,
                                                               false,
                                                               false,
                                                               false,
                                                               error_buf,
                                                               error_buf_size)) {
                return false;
            }
        }
        return true;
    }

    if (type->tag == TABLO_EXT_TYPE_MAP) {
        if (!allow_map) {
            native_extension_set_error(error_buf,
                                       error_buf_size,
                                       "Callback results do not support map values");
            return false;
        }
        return true;
    }

    if (type->tag == TABLO_EXT_TYPE_VOID) {
        native_extension_set_error(error_buf,
                                   error_buf_size,
                                   "Callback signatures only support scalar values, opaque handles, flat tuples, and flat maps");
        return false;
    }

    return native_extension_type_is_valid(type, error_buf, error_buf_size);
}

static bool native_extension_type_is_valid(const TabloExtTypeDesc* type, char* error_buf, size_t error_buf_size) {
    if (!type) {
        native_extension_set_error(error_buf, error_buf_size, "Missing extension type descriptor");
        return false;
    }

    switch (type->tag) {
        case TABLO_EXT_TYPE_VOID:
            if (type->nullable) {
                native_extension_set_error(error_buf, error_buf_size, "Void extension types cannot be nullable");
                return false;
            }
            return true;
        case TABLO_EXT_TYPE_INT:
        case TABLO_EXT_TYPE_BOOL:
        case TABLO_EXT_TYPE_DOUBLE:
        case TABLO_EXT_TYPE_STRING:
        case TABLO_EXT_TYPE_BYTES:
            return true;
        case TABLO_EXT_TYPE_HANDLE:
            if (!type->handle_type_name || type->handle_type_name[0] == '\0') {
                native_extension_set_error(error_buf, error_buf_size, "Handle type descriptors require a handle type name");
                return false;
            }
            return true;
        case TABLO_EXT_TYPE_ARRAY:
            if (!type->element_type) {
                native_extension_set_error(error_buf, error_buf_size, "Array type descriptors require an element type");
                return false;
            }
            if (type->element_type->tag == TABLO_EXT_TYPE_VOID) {
                native_extension_set_error(error_buf, error_buf_size, "Array element types cannot be void");
                return false;
            }
            if (type->element_type->tag == TABLO_EXT_TYPE_ARRAY ||
                type->element_type->tag == TABLO_EXT_TYPE_TUPLE ||
                type->element_type->tag == TABLO_EXT_TYPE_CALLBACK) {
                native_extension_set_error(error_buf, error_buf_size, "Nested extension array/tuple/callback types are not supported yet");
                return false;
            }
            return native_extension_type_is_valid(type->element_type, error_buf, error_buf_size);
        case TABLO_EXT_TYPE_TUPLE:
            if (type->tuple_element_count < 0) {
                native_extension_set_error(error_buf, error_buf_size, "Tuple type descriptors require a non-negative element count");
                return false;
            }
            if (type->tuple_element_count > 0 && !type->tuple_element_types) {
                native_extension_set_error(error_buf, error_buf_size, "Tuple type descriptors require element types");
                return false;
            }
            for (int i = 0; i < type->tuple_element_count; i++) {
                const TabloExtTypeDesc* element_type = &type->tuple_element_types[i];
                if (element_type->tag == TABLO_EXT_TYPE_VOID) {
                    native_extension_set_error(error_buf, error_buf_size, "Tuple element types cannot be void");
                    return false;
                }
                if (element_type->tag == TABLO_EXT_TYPE_ARRAY ||
                    element_type->tag == TABLO_EXT_TYPE_TUPLE ||
                    element_type->tag == TABLO_EXT_TYPE_MAP ||
                    element_type->tag == TABLO_EXT_TYPE_CALLBACK) {
                    native_extension_set_error(error_buf, error_buf_size, "Nested extension array/tuple/map/callback types are not supported yet");
                    return false;
                }
                if (!native_extension_type_is_valid(element_type, error_buf, error_buf_size)) {
                    return false;
                }
            }
            return true;
        case TABLO_EXT_TYPE_MAP:
            return true;
        case TABLO_EXT_TYPE_CALLBACK:
            if (type->callback_param_count < 0) {
                native_extension_set_error(error_buf, error_buf_size, "Callback type descriptors require a non-negative parameter count");
                return false;
            }
            if (type->callback_param_count > 0 && !type->callback_param_types) {
                native_extension_set_error(error_buf, error_buf_size, "Callback type descriptors require parameter types");
                return false;
            }
            if (!type->callback_result_type) {
                native_extension_set_error(error_buf, error_buf_size, "Callback type descriptors require a result type");
                return false;
            }
            if (!native_extension_callback_value_type_is_valid(type->callback_result_type,
                                                               true,
                                                               true,
                                                               true,
                                                               true,
                                                               error_buf,
                                                               error_buf_size)) {
                return false;
            }
            for (int i = 0; i < type->callback_param_count; i++) {
                if (!native_extension_callback_value_type_is_valid(&type->callback_param_types[i],
                                                                   false,
                                                                   true,
                                                                   true,
                                                                   true,
                                                                   error_buf,
                                                                   error_buf_size)) {
                    return false;
                }
            }
            return true;
        default:
            native_extension_set_error(error_buf, error_buf_size, "Unsupported extension type tag");
            return false;
    }
}

static void native_extension_type_clone(TabloExtTypeDesc* dst, const TabloExtTypeDesc* src) {
    dst->tag = src->tag;
    dst->nullable = src->nullable;
    dst->handle_type_name = NULL;
    dst->element_type = NULL;
    dst->tuple_element_types = NULL;
    dst->tuple_element_count = 0;
    dst->callback_param_types = NULL;
    dst->callback_param_count = 0;
    dst->callback_result_type = NULL;
    if (src->tag == TABLO_EXT_TYPE_HANDLE && src->handle_type_name) {
        dst->handle_type_name = native_extension_strdup(src->handle_type_name);
    } else if (src->tag == TABLO_EXT_TYPE_ARRAY && src->element_type) {
        TabloExtTypeDesc* cloned_element = (TabloExtTypeDesc*)safe_calloc(1, sizeof(TabloExtTypeDesc));
        native_extension_type_clone(cloned_element, src->element_type);
        dst->element_type = cloned_element;
    } else if (src->tag == TABLO_EXT_TYPE_TUPLE && src->tuple_element_count > 0 && src->tuple_element_types) {
        TabloExtTypeDesc* cloned_elements = (TabloExtTypeDesc*)safe_calloc((size_t)src->tuple_element_count,
                                                                       sizeof(TabloExtTypeDesc));
        for (int i = 0; i < src->tuple_element_count; i++) {
            native_extension_type_clone(&cloned_elements[i], &src->tuple_element_types[i]);
        }
        dst->tuple_element_types = cloned_elements;
        dst->tuple_element_count = src->tuple_element_count;
    } else if (src->tag == TABLO_EXT_TYPE_CALLBACK) {
        if (src->callback_param_count > 0 && src->callback_param_types) {
            TabloExtTypeDesc* cloned_params =
                (TabloExtTypeDesc*)safe_calloc((size_t)src->callback_param_count, sizeof(TabloExtTypeDesc));
            for (int i = 0; i < src->callback_param_count; i++) {
                native_extension_type_clone(&cloned_params[i], &src->callback_param_types[i]);
            }
            dst->callback_param_types = cloned_params;
            dst->callback_param_count = src->callback_param_count;
        }
        if (src->callback_result_type) {
            TabloExtTypeDesc* cloned_result = (TabloExtTypeDesc*)safe_calloc(1, sizeof(TabloExtTypeDesc));
            native_extension_type_clone(cloned_result, src->callback_result_type);
            dst->callback_result_type = cloned_result;
        }
    }
}

static void native_extension_type_free(TabloExtTypeDesc* type) {
    if (!type) return;
    if (type->tag == TABLO_EXT_TYPE_HANDLE && type->handle_type_name) {
        free((void*)type->handle_type_name);
        type->handle_type_name = NULL;
    } else if (type->tag == TABLO_EXT_TYPE_ARRAY && type->element_type) {
        TabloExtTypeDesc* element_type = (TabloExtTypeDesc*)type->element_type;
        native_extension_type_free(element_type);
        free(element_type);
        type->element_type = NULL;
    } else if (type->tag == TABLO_EXT_TYPE_TUPLE && type->tuple_element_types) {
        TabloExtTypeDesc* element_types = (TabloExtTypeDesc*)type->tuple_element_types;
        for (int i = 0; i < type->tuple_element_count; i++) {
            native_extension_type_free(&element_types[i]);
        }
        free(element_types);
        type->tuple_element_types = NULL;
        type->tuple_element_count = 0;
    } else if (type->tag == TABLO_EXT_TYPE_CALLBACK) {
        if (type->callback_param_types) {
            TabloExtTypeDesc* param_types = (TabloExtTypeDesc*)type->callback_param_types;
            for (int i = 0; i < type->callback_param_count; i++) {
                native_extension_type_free(&param_types[i]);
            }
            free(param_types);
            type->callback_param_types = NULL;
            type->callback_param_count = 0;
        }
        if (type->callback_result_type) {
            TabloExtTypeDesc* result_type = (TabloExtTypeDesc*)type->callback_result_type;
            native_extension_type_free(result_type);
            free(result_type);
            type->callback_result_type = NULL;
        }
    }
}

static void native_extension_function_free(NativeExtensionFunction* function) {
    if (!function) return;
    if (function->name) free(function->name);
    native_extension_type_free(&function->result_type);
    if (function->param_types) {
        for (int i = 0; i < function->param_count; i++) {
            native_extension_type_free(&function->param_types[i]);
        }
        free(function->param_types);
    }
    memset(function, 0, sizeof(*function));
}

static void native_extension_shutdown(NativeExtension* extension) {
    if (!extension || extension->shutdown_called) return;
    extension->shutdown_called = true;
    if (extension->shutdown_fn) {
        extension->shutdown_fn();
    }
}

static void native_extension_free(NativeExtension* extension) {
    if (!extension) return;
    native_extension_shutdown(extension);
    if (extension->functions) {
        for (int i = 0; i < extension->function_count; i++) {
            native_extension_function_free(&extension->functions[i]);
        }
        free(extension->functions);
    }
    if (extension->handle_types) {
        for (int i = 0; i < extension->handle_type_count; i++) {
            if (extension->handle_types[i].name) free(extension->handle_types[i].name);
        }
        free(extension->handle_types);
    }
    if (extension->library) {
#ifdef _WIN32
        FreeLibrary(extension->library);
#else
        dlclose(extension->library);
#endif
    }
    if (extension->path) free(extension->path);
    memset(extension, 0, sizeof(*extension));
}

NativeExtensionRegistry* native_extension_registry_create(void) {
    return (NativeExtensionRegistry*)safe_calloc(1, sizeof(NativeExtensionRegistry));
}

void native_extension_registry_shutdown(NativeExtensionRegistry* registry) {
    if (!registry || !registry->extensions) return;
    for (int i = 0; i < registry->extension_count; i++) {
        native_extension_shutdown(&registry->extensions[i]);
    }
}

void native_extension_registry_free(NativeExtensionRegistry* registry) {
    if (!registry) return;
    if (registry->extensions) {
        for (int i = 0; i < registry->extension_count; i++) {
            native_extension_free(&registry->extensions[i]);
        }
        free(registry->extensions);
    }
    free(registry);
}

static bool native_extension_registry_name_exists(const NativeExtensionRegistry* registry,
                                                  const NativeExtension* current_extension,
                                                  const char* name) {
    if (!name || name[0] == '\0') return false;
    if (current_extension) {
        for (int i = 0; i < current_extension->handle_type_count; i++) {
            if (current_extension->handle_types[i].name && strcmp(current_extension->handle_types[i].name, name) == 0) {
                return true;
            }
        }
        for (int i = 0; i < current_extension->function_count; i++) {
            if (current_extension->functions[i].name && strcmp(current_extension->functions[i].name, name) == 0) {
                return true;
            }
        }
    }
    if (!registry) return false;
    for (int i = 0; i < registry->extension_count; i++) {
        const NativeExtension* extension = &registry->extensions[i];
        for (int j = 0; j < extension->handle_type_count; j++) {
            if (extension->handle_types[j].name && strcmp(extension->handle_types[j].name, name) == 0) {
                return true;
            }
        }
        for (int j = 0; j < extension->function_count; j++) {
            if (extension->functions[j].name && strcmp(extension->functions[j].name, name) == 0) {
                return true;
            }
        }
    }
    return false;
}

static const NativeExtensionHandleType* native_extension_lookup_handle_type(const NativeExtensionRegistry* registry,
                                                                            const NativeExtension* current_extension,
                                                                            const char* type_name) {
    if (!type_name || type_name[0] == '\0') return NULL;
    if (current_extension) {
        for (int i = 0; i < current_extension->handle_type_count; i++) {
            if (current_extension->handle_types[i].name && strcmp(current_extension->handle_types[i].name, type_name) == 0) {
                return &current_extension->handle_types[i];
            }
        }
    }
    if (!registry) return NULL;
    for (int i = 0; i < registry->extension_count; i++) {
        const NativeExtension* extension = &registry->extensions[i];
        for (int j = 0; j < extension->handle_type_count; j++) {
            if (extension->handle_types[j].name && strcmp(extension->handle_types[j].name, type_name) == 0) {
                return &extension->handle_types[j];
            }
        }
    }
    return NULL;
}

static bool native_extension_validate_type_handle_references(const NativeExtensionRegistry* registry,
                                                             const NativeExtension* extension,
                                                             const TabloExtTypeDesc* type,
                                                             char* error_buf,
                                                             size_t error_buf_size,
                                                             const char* label) {
    if (!type) {
        native_extension_set_error(error_buf, error_buf_size, "Extension type descriptor is not initialized");
        return false;
    }

    if (type->tag == TABLO_EXT_TYPE_HANDLE &&
        !native_extension_lookup_handle_type(registry, extension, type->handle_type_name)) {
        char buf[192];
        snprintf(buf, sizeof(buf), "%s references an unknown handle type", label ? label : "Extension type");
        native_extension_set_error(error_buf, error_buf_size, buf);
        return false;
    }

    if (type->tag == TABLO_EXT_TYPE_ARRAY) {
        return native_extension_validate_type_handle_references(registry,
                                                                extension,
                                                                type->element_type,
                                                                error_buf,
                                                                error_buf_size,
                                                                label);
    }

    if (type->tag == TABLO_EXT_TYPE_TUPLE) {
        for (int i = 0; i < type->tuple_element_count; i++) {
            if (!native_extension_validate_type_handle_references(registry,
                                                                  extension,
                                                                  &type->tuple_element_types[i],
                                                                  error_buf,
                                                                  error_buf_size,
                                                                  label)) {
                return false;
            }
        }
    }

    if (type->tag == TABLO_EXT_TYPE_CALLBACK) {
        for (int i = 0; i < type->callback_param_count; i++) {
            if (!native_extension_validate_type_handle_references(registry,
                                                                  extension,
                                                                  &type->callback_param_types[i],
                                                                  error_buf,
                                                                  error_buf_size,
                                                                  label)) {
                return false;
            }
        }
        if (type->callback_result_type &&
            !native_extension_validate_type_handle_references(registry,
                                                              extension,
                                                              type->callback_result_type,
                                                              error_buf,
                                                              error_buf_size,
                                                              label)) {
            return false;
        }
    }

    return true;
}

static bool native_extension_register_handle_type_impl(void* host_context,
                                                       const TabloExtHandleTypeDef* def,
                                                       char* error_buf,
                                                       size_t error_buf_size) {
    NativeExtensionLoadContext* ctx = (NativeExtensionLoadContext*)host_context;
    if (!ctx || !ctx->extension) {
        native_extension_set_error(error_buf, error_buf_size, "Extension loader context is not initialized");
        return false;
    }
    if (!def || !def->name || def->name[0] == '\0') {
        native_extension_set_error(error_buf, error_buf_size, "Handle types require a non-empty name");
        return false;
    }
    if (native_extension_registry_name_exists(ctx->registry, ctx->extension, def->name)) {
        native_extension_set_error(error_buf, error_buf_size, "Duplicate extension function or handle type name");
        return false;
    }

    if (ctx->extension->handle_type_count + 1 > ctx->extension->handle_type_capacity) {
        int new_capacity = ctx->extension->handle_type_capacity > 0 ? ctx->extension->handle_type_capacity * 2 : 4;
        ctx->extension->handle_types = (NativeExtensionHandleType*)safe_realloc(
            ctx->extension->handle_types,
            (size_t)new_capacity * sizeof(NativeExtensionHandleType));
        ctx->extension->handle_type_capacity = new_capacity;
    }

    NativeExtensionHandleType* handle_type = &ctx->extension->handle_types[ctx->extension->handle_type_count++];
    memset(handle_type, 0, sizeof(*handle_type));
    handle_type->name = native_extension_strdup(def->name);
    handle_type->destroy = def->destroy;
    return true;
}

static bool native_extension_register_function_impl(void* host_context,
                                                    const TabloExtFunctionDef* def,
                                                    char* error_buf,
                                                    size_t error_buf_size) {
    NativeExtensionLoadContext* ctx = (NativeExtensionLoadContext*)host_context;
    if (!ctx || !ctx->extension) {
        native_extension_set_error(error_buf, error_buf_size, "Extension loader context is not initialized");
        return false;
    }
    if (!def || !def->name || def->name[0] == '\0') {
        native_extension_set_error(error_buf, error_buf_size, "Extension functions require a non-empty name");
        return false;
    }
    if (!def->callback) {
        native_extension_set_error(error_buf, error_buf_size, "Extension functions require a callback");
        return false;
    }
    if (def->param_count < 0) {
        native_extension_set_error(error_buf, error_buf_size, "Extension function parameter count must be non-negative");
        return false;
    }
    if (def->param_count > 0 && !def->param_types) {
        native_extension_set_error(error_buf, error_buf_size, "Extension function parameter types are required");
        return false;
    }
    if (!native_extension_type_is_valid(&def->result_type, error_buf, error_buf_size)) {
        return false;
    }
    if (def->result_type.tag == TABLO_EXT_TYPE_CALLBACK) {
        native_extension_set_error(error_buf, error_buf_size, "Extension functions cannot return callbacks yet");
        return false;
    }
    for (int i = 0; i < def->param_count; i++) {
        if (!native_extension_type_is_valid(&def->param_types[i], error_buf, error_buf_size)) {
            return false;
        }
        if (def->param_types[i].tag == TABLO_EXT_TYPE_VOID) {
            native_extension_set_error(error_buf, error_buf_size, "Extension function parameters cannot be void");
            return false;
        }
    }
    if (native_extension_registry_name_exists(ctx->registry, ctx->extension, def->name)) {
        native_extension_set_error(error_buf, error_buf_size, "Duplicate extension function or handle type name");
        return false;
    }

    if (ctx->extension->function_count + 1 > ctx->extension->function_capacity) {
        int new_capacity = ctx->extension->function_capacity > 0 ? ctx->extension->function_capacity * 2 : 8;
        ctx->extension->functions = (NativeExtensionFunction*)safe_realloc(
            ctx->extension->functions,
            (size_t)new_capacity * sizeof(NativeExtensionFunction));
        ctx->extension->function_capacity = new_capacity;
    }

    NativeExtensionFunction* function = &ctx->extension->functions[ctx->extension->function_count++];
    memset(function, 0, sizeof(*function));
    function->name = native_extension_strdup(def->name);
    native_extension_type_clone(&function->result_type, &def->result_type);
    function->param_count = def->param_count;
    function->callback = def->callback;
    if (def->param_count > 0) {
        function->param_types = (TabloExtTypeDesc*)safe_calloc((size_t)def->param_count, sizeof(TabloExtTypeDesc));
        for (int i = 0; i < def->param_count; i++) {
            native_extension_type_clone(&function->param_types[i], &def->param_types[i]);
        }
    }
    return true;
}

static bool native_extension_validate_handle_references(const NativeExtensionRegistry* registry,
                                                        const NativeExtension* extension,
                                                        char* error_buf,
                                                        size_t error_buf_size) {
    for (int i = 0; i < extension->function_count; i++) {
        const NativeExtensionFunction* function = &extension->functions[i];
        if (!native_extension_validate_type_handle_references(registry,
                                                              extension,
                                                              &function->result_type,
                                                              error_buf,
                                                              error_buf_size,
                                                              "Extension function result")) {
            return false;
        }
        for (int j = 0; j < function->param_count; j++) {
            if (!native_extension_validate_type_handle_references(registry,
                                                                  extension,
                                                                  &function->param_types[j],
                                                                  error_buf,
                                                                  error_buf_size,
                                                                  "Extension function parameter")) {
                return false;
            }
        }
    }
    return true;
}

static NativeExtensionLibraryHandle native_extension_open_library(const char* path,
                                                                 char* error_buf,
                                                                 size_t error_buf_size) {
#ifdef _WIN32
    HMODULE library = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!library) {
        char message[256];
        snprintf(message, sizeof(message), "Failed to load extension library: %s", path);
        native_extension_set_error(error_buf, error_buf_size, message);
    }
    return library;
#else
    void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        native_extension_set_error(error_buf, error_buf_size, dlerror());
    }
    return library;
#endif
}

static void* native_extension_find_symbol(NativeExtensionLibraryHandle library, const char* symbol_name) {
    if (!library || !symbol_name) return NULL;
#ifdef _WIN32
    return (void*)GetProcAddress(library, symbol_name);
#else
    return dlsym(library, symbol_name);
#endif
}

static bool native_extension_registry_append(NativeExtensionRegistry* registry, NativeExtension* extension) {
    if (!registry || !extension) return false;
    if (registry->extension_count + 1 > registry->extension_capacity) {
        int new_capacity = registry->extension_capacity > 0 ? registry->extension_capacity * 2 : 4;
        registry->extensions = (NativeExtension*)safe_realloc(
            registry->extensions,
            (size_t)new_capacity * sizeof(NativeExtension));
        registry->extension_capacity = new_capacity;
    }
    registry->extensions[registry->extension_count++] = *extension;
    memset(extension, 0, sizeof(*extension));
    return true;
}

static bool native_extension_registry_load_path(NativeExtensionRegistry* registry,
                                                const char* path,
                                                char* error_buf,
                                                size_t error_buf_size) {
    if (!registry || !path || path[0] == '\0') {
        native_extension_set_error(error_buf, error_buf_size, "Extension path is required");
        return false;
    }

    NativeExtension extension;
    TabloExtShutdownFn shutdown_fn = NULL;
    memset(&extension, 0, sizeof(extension));
    extension.path = native_extension_strdup(path);
    extension.library = native_extension_open_library(path, error_buf, error_buf_size);
    if (!extension.library) {
        native_extension_free(&extension);
        return false;
    }
    shutdown_fn = (TabloExtShutdownFn)native_extension_find_symbol(extension.library, TABLO_EXT_SHUTDOWN_SYMBOL);

    TabloExtInitFn init_fn = (TabloExtInitFn)native_extension_find_symbol(extension.library, TABLO_EXT_INIT_SYMBOL);
    if (!init_fn) {
        native_extension_set_error(error_buf, error_buf_size, "Extension library is missing tablo_extension_init");
        native_extension_free(&extension);
        return false;
    }

    NativeExtensionLoadContext load_ctx;
    load_ctx.registry = registry;
    load_ctx.extension = &extension;

    TabloExtRegistrar registrar;
    registrar.abi_version = TABLO_EXT_ABI_VERSION;
    registrar.host_context = &load_ctx;
    registrar.register_handle_type = native_extension_register_handle_type_impl;
    registrar.register_function = native_extension_register_function_impl;
    registrar.retain_callback = native_extension_callback_retain_impl;
    registrar.release_callback = native_extension_callback_release_impl;
    registrar.post_callback = native_extension_post_callback_impl;
    registrar.create_callback_gate = native_extension_create_callback_gate_impl;
    registrar.retain_callback_gate = native_extension_callback_gate_retain_impl;
    registrar.release_callback_gate = native_extension_callback_gate_release_impl;
    registrar.reset_callback_gate = native_extension_callback_gate_reset_impl;
    registrar.get_callback_gate_invalidated_count = native_extension_callback_gate_get_invalidated_count_impl;
    registrar.reset_callback_gate_invalidated_count = native_extension_callback_gate_reset_invalidated_count_impl;
    registrar.post_callback_gated = native_extension_post_callback_gated_impl;
    registrar.get_posted_callback_pending_count = native_extension_get_posted_callback_pending_count_impl;
    registrar.is_posted_callback_queue_open = native_extension_is_posted_callback_queue_open_impl;

    if (!init_fn(&registrar, error_buf, error_buf_size)) {
        if (error_buf && error_buf[0] == '\0') {
            native_extension_set_error(error_buf, error_buf_size, "Extension init failed");
        }
        native_extension_free(&extension);
        return false;
    }

    if (!native_extension_validate_handle_references(registry, &extension, error_buf, error_buf_size)) {
        native_extension_free(&extension);
        return false;
    }

    extension.shutdown_fn = shutdown_fn;

    return native_extension_registry_append(registry, &extension);
}

bool native_extension_registry_load_paths(NativeExtensionRegistry* registry,
                                          const char* const* paths,
                                          int path_count,
                                          char* error_buf,
                                          size_t error_buf_size) {
    if (!registry) {
        native_extension_set_error(error_buf, error_buf_size, "Extension registry is not initialized");
        return false;
    }
    if (path_count <= 0) return true;
    for (int i = 0; i < path_count; i++) {
        if (!native_extension_registry_load_path(registry, paths[i], error_buf, error_buf_size)) {
            return false;
        }
    }
    return true;
}

bool native_extension_registry_has_extensions(const NativeExtensionRegistry* registry) {
    return registry && registry->extension_count > 0;
}

static const NativeExtensionHandleType* native_extension_registry_handle_at(const NativeExtensionRegistry* registry,
                                                                            int index) {
    if (!registry || index < 0) return NULL;
    int offset = index;
    for (int i = 0; i < registry->extension_count; i++) {
        const NativeExtension* extension = &registry->extensions[i];
        if (offset < extension->handle_type_count) {
            return &extension->handle_types[offset];
        }
        offset -= extension->handle_type_count;
    }
    return NULL;
}

static const NativeExtensionFunction* native_extension_registry_function_at(const NativeExtensionRegistry* registry,
                                                                            int index) {
    if (!registry || index < 0) return NULL;
    int offset = index;
    for (int i = 0; i < registry->extension_count; i++) {
        const NativeExtension* extension = &registry->extensions[i];
        if (offset < extension->function_count) {
            return &extension->functions[offset];
        }
        offset -= extension->function_count;
    }
    return NULL;
}

int native_extension_registry_handle_type_count(const NativeExtensionRegistry* registry) {
    if (!registry) return 0;
    int total = 0;
    for (int i = 0; i < registry->extension_count; i++) {
        total += registry->extensions[i].handle_type_count;
    }
    return total;
}

const char* native_extension_registry_handle_type_name(const NativeExtensionRegistry* registry, int index) {
    const NativeExtensionHandleType* handle_type = native_extension_registry_handle_at(registry, index);
    return handle_type ? handle_type->name : NULL;
}

int native_extension_registry_function_count(const NativeExtensionRegistry* registry) {
    if (!registry) return 0;
    int total = 0;
    for (int i = 0; i < registry->extension_count; i++) {
        total += registry->extensions[i].function_count;
    }
    return total;
}

const char* native_extension_registry_function_name(const NativeExtensionRegistry* registry, int index) {
    const NativeExtensionFunction* function = native_extension_registry_function_at(registry, index);
    return function ? function->name : NULL;
}

TabloExtTypeDesc native_extension_registry_function_result_type(const NativeExtensionRegistry* registry, int index) {
    static const TabloExtTypeDesc zero = {0};
    const NativeExtensionFunction* function = native_extension_registry_function_at(registry, index);
    return function ? function->result_type : zero;
}

int native_extension_registry_function_param_count(const NativeExtensionRegistry* registry, int index) {
    const NativeExtensionFunction* function = native_extension_registry_function_at(registry, index);
    return function ? function->param_count : 0;
}

TabloExtTypeDesc native_extension_registry_function_param_type(const NativeExtensionRegistry* registry,
                                                            int function_index,
                                                            int param_index) {
    static const TabloExtTypeDesc zero = {0};
    const NativeExtensionFunction* function = native_extension_registry_function_at(registry, function_index);
    if (!function || param_index < 0 || param_index >= function->param_count) {
        return zero;
    }
    return function->param_types[param_index];
}

bool native_extension_registry_register_vm_globals(NativeExtensionRegistry* registry,
                                                   VM* vm,
                                                   char* error_buf,
                                                   size_t error_buf_size) {
    if (!registry || !vm) {
        native_extension_set_error(error_buf, error_buf_size, "Extension registry or VM is not initialized");
        return false;
    }

    for (int i = 0; i < registry->extension_count; i++) {
        NativeExtension* extension = &registry->extensions[i];
        for (int j = 0; j < extension->function_count; j++) {
            NativeExtensionFunction* function = &extension->functions[j];
            Value existing = vm_get_global(vm, function->name);
            if (value_get_type(&existing) != VAL_NIL) {
                native_extension_set_error(error_buf, error_buf_size, "Extension function name conflicts with an existing global");
                return false;
            }
            if (!vm_register_native_extension(vm, function->name, function, function->param_count)) {
                native_extension_set_error(error_buf, error_buf_size, "Failed to register extension function");
                return false;
            }
        }
    }

    return true;
}

static NativeExtensionCallState* native_extension_call_state(TabloExtCallContext* ctx) {
    return (NativeExtensionCallState*)(ctx ? ctx->host_context : NULL);
}

static Value* native_extension_arg_slot(NativeExtensionCallState* state, int index) {
    if (!state || !state->vm) return NULL;
    if (index < 0 || index >= state->arg_count) return NULL;
    return &state->vm->stack.values[state->result_slot + index];
}

static const TabloExtTypeDesc* native_extension_expected_param_type(NativeExtensionCallState* state, int index) {
    if (!state || !state->function) return NULL;
    if (index < 0 || index >= state->function->param_count) return NULL;
    return &state->function->param_types[index];
}

static const TabloExtTypeDesc* native_extension_expected_result_type(NativeExtensionCallState* state) {
    if (!state || !state->function) return NULL;
    return &state->function->result_type;
}

static void native_extension_set_runtime_error_message(NativeExtensionCallState* state, const char* message) {
    if (!state || !state->vm) return;
    vm_runtime_error(state->vm, message ? message : "Extension runtime error");
}

static bool native_extension_validate_arg_tag(NativeExtensionCallState* state,
                                              int index,
                                              TabloExtTypeTag expected_tag,
                                              const char* label) {
    const TabloExtTypeDesc* type = native_extension_expected_param_type(state, index);
    if (!type || type->tag != expected_tag) {
        char buf[160];
        snprintf(buf, sizeof(buf), "Extension callback requested %s for incompatible parameter %d", label, index);
        native_extension_set_runtime_error_message(state, buf);
        return false;
    }
    return true;
}

static bool native_extension_write_result_value(NativeExtensionCallState* state, Value* result) {
    if (!state || !state->vm || !result) return false;
    if (state->result_slot < 0 || state->result_slot >= state->vm->stack.count) {
        native_extension_set_runtime_error_message(state, "Extension attempted to write an invalid result slot");
        value_free(result);
        return false;
    }
    Value* slot = &state->vm->stack.values[state->result_slot];
    value_free(slot);
    *slot = *result;
    state->result_set = true;
    return true;
}

static void native_extension_callback_free(TabloExtCallback* callback) {
    if (!callback) return;
    native_extension_type_free(&callback->type);
    if (callback->posted_event_queue) {
        vm_posted_event_queue_release(callback->posted_event_queue);
        callback->posted_event_queue = NULL;
    }
    value_free(&callback->callable);
    mtx_destroy(&callback->mutex);
    free(callback);
}

static void native_extension_callback_retain_impl(const TabloExtCallback* callback) {
    TabloExtCallback* writable = (TabloExtCallback*)callback;
    if (!writable) return;
    mtx_lock(&writable->mutex);
    writable->ref_count++;
    mtx_unlock(&writable->mutex);
}

static void native_extension_callback_release_impl(const TabloExtCallback* callback) {
    TabloExtCallback* writable = (TabloExtCallback*)callback;
    bool destroy = false;
    if (!writable) return;
    mtx_lock(&writable->mutex);
    writable->ref_count--;
    destroy = writable->ref_count <= 0;
    mtx_unlock(&writable->mutex);
    if (destroy) {
        native_extension_callback_free(writable);
    }
}

static void native_extension_callback_increment_pending_impl(const TabloExtCallback* callback) {
    TabloExtCallback* writable = (TabloExtCallback*)callback;
    if (!writable) return;
    mtx_lock(&writable->mutex);
    writable->posted_pending_count++;
    mtx_unlock(&writable->mutex);
}

static void native_extension_callback_decrement_pending_impl(const TabloExtCallback* callback) {
    TabloExtCallback* writable = (TabloExtCallback*)callback;
    if (!writable) return;
    mtx_lock(&writable->mutex);
    if (writable->posted_pending_count > 0) {
        writable->posted_pending_count--;
    }
    mtx_unlock(&writable->mutex);
}

static TabloExtCallbackGate* native_extension_create_callback_gate_impl(void) {
    TabloExtCallbackGate* gate = (TabloExtCallbackGate*)calloc(1, sizeof(TabloExtCallbackGate));
    if (!gate) return NULL;
    if (mtx_init(&gate->mutex, mtx_plain) != thrd_success) {
        free(gate);
        return NULL;
    }
    gate->ref_count = 1;
    gate->generation = 1;
    return gate;
}

static void native_extension_callback_gate_retain_impl(TabloExtCallbackGate* gate) {
    if (!gate) return;
    mtx_lock(&gate->mutex);
    gate->ref_count++;
    mtx_unlock(&gate->mutex);
}

static void native_extension_callback_gate_release_impl(TabloExtCallbackGate* gate) {
    bool destroy = false;
    if (!gate) return;
    mtx_lock(&gate->mutex);
    gate->ref_count--;
    destroy = gate->ref_count <= 0;
    mtx_unlock(&gate->mutex);
    if (!destroy) return;
    mtx_destroy(&gate->mutex);
    free(gate);
}

static uint64_t native_extension_callback_gate_reset_impl(TabloExtCallbackGate* gate) {
    uint64_t generation = 0;
    if (!gate) return 0;
    mtx_lock(&gate->mutex);
    if (gate->generation == UINT64_MAX) {
        gate->generation = 1;
    } else {
        gate->generation++;
    }
    generation = gate->generation;
    mtx_unlock(&gate->mutex);
    return generation;
}

static int64_t native_extension_callback_gate_get_invalidated_count_impl(const TabloExtCallbackGate* gate) {
    int64_t invalidated_count = 0;
    TabloExtCallbackGate* writable = (TabloExtCallbackGate*)gate;
    if (!writable) return 0;
    mtx_lock(&writable->mutex);
    invalidated_count = writable->invalidated_count;
    mtx_unlock(&writable->mutex);
    return invalidated_count;
}

static int64_t native_extension_callback_gate_reset_invalidated_count_impl(TabloExtCallbackGate* gate) {
    int64_t invalidated_count = 0;
    if (!gate) return 0;
    mtx_lock(&gate->mutex);
    invalidated_count = gate->invalidated_count;
    gate->invalidated_count = 0;
    mtx_unlock(&gate->mutex);
    return invalidated_count;
}

static bool native_extension_callback_gate_matches(const TabloExtCallbackGate* gate, uint64_t generation) {
    bool matches = false;
    TabloExtCallbackGate* writable = (TabloExtCallbackGate*)gate;
    if (!writable) return true;
    mtx_lock(&writable->mutex);
    matches = writable->generation == generation;
    if (!matches && writable->invalidated_count < INT64_MAX) {
        writable->invalidated_count++;
    }
    mtx_unlock(&writable->mutex);
    return matches;
}

static int native_extension_get_posted_callback_pending_count_impl(const TabloExtCallback* callback) {
    const TabloExtCallback* readable = callback;
    int pending_count = 0;
    if (!readable) {
        return 0;
    }
    mtx_lock((mtx_t*)&readable->mutex);
    pending_count = readable->posted_pending_count;
    mtx_unlock((mtx_t*)&readable->mutex);
    return pending_count;
}

static bool native_extension_is_posted_callback_queue_open_impl(const TabloExtCallback* callback) {
    if (!callback || !callback->posted_event_queue) {
        return false;
    }
    return vm_posted_event_queue_is_open(callback->posted_event_queue);
}

static TabloExtCallback* native_extension_callback_create(VM* vm,
                                                        const Value* callable,
                                                        const TabloExtTypeDesc* callback_type) {
    TabloExtCallback* callback = NULL;
    if (!vm || !callable || !callback_type || callback_type->tag != TABLO_EXT_TYPE_CALLBACK) return NULL;

    callback = (TabloExtCallback*)safe_calloc(1, sizeof(TabloExtCallback));
    if (mtx_init(&callback->mutex, mtx_plain) != thrd_success) {
        free(callback);
        return NULL;
    }
    callback->ref_count = 1;
    callback->vm = vm;
    callback->posted_event_queue = vm_get_posted_event_queue(vm);
    native_extension_type_clone(&callback->type, callback_type);
    callback->callable = *callable;
    value_retain(&callback->callable);
    return callback;
}

static char* native_extension_strdup_fallible(const char* text) {
    size_t len = 0;
    char* out = NULL;
    if (!text) return NULL;
    len = strlen(text);
    out = (char*)malloc(len + 1u);
    if (!out) return NULL;
    memcpy(out, text, len + 1u);
    return out;
}

static bool native_extension_callback_post_type_supported(const TabloExtTypeDesc* type) {
    if (!type) return false;
    if (type->nullable && type->tag == TABLO_EXT_TYPE_VOID) return false;
    switch (type->tag) {
        case TABLO_EXT_TYPE_INT:
        case TABLO_EXT_TYPE_BOOL:
        case TABLO_EXT_TYPE_DOUBLE:
        case TABLO_EXT_TYPE_STRING:
        case TABLO_EXT_TYPE_BYTES:
        case TABLO_EXT_TYPE_HANDLE:
            return true;
        case TABLO_EXT_TYPE_ARRAY:
            if (!type->element_type) {
                return false;
            }
            return native_extension_callback_post_type_supported(type->element_type);
        case TABLO_EXT_TYPE_TUPLE:
            if (type->tuple_element_count < 0) {
                return false;
            }
            if (type->tuple_element_count > 0 && !type->tuple_element_types) {
                return false;
            }
            for (int i = 0; i < type->tuple_element_count; i++) {
                if (!native_extension_callback_post_type_supported(&type->tuple_element_types[i])) {
                    return false;
                }
            }
            return true;
        case TABLO_EXT_TYPE_MAP:
            return true;
        default:
            return false;
    }
}

static bool native_extension_clone_posted_map_item_value(const TabloExtValue* src,
                                                         TabloExtValue* dst,
                                                         char* error_buf,
                                                         size_t error_buf_size) {
    if (!src || !dst) return false;

    memset(dst, 0, sizeof(*dst));
    dst->tag = src->tag;
    dst->is_nil = src->is_nil;
    dst->handle_type_name = src->handle_type_name;
    if (src->is_nil) {
        return true;
    }

    switch (src->tag) {
        case TABLO_EXT_TYPE_INT:
            dst->as.int_value = src->as.int_value;
            return true;
        case TABLO_EXT_TYPE_BOOL:
            dst->as.bool_value = src->as.bool_value;
            return true;
        case TABLO_EXT_TYPE_DOUBLE:
            dst->as.double_value = src->as.double_value;
            return true;
        case TABLO_EXT_TYPE_STRING: {
            const char* chars = src->as.string_value.chars ? src->as.string_value.chars : "";
            int length = src->as.string_value.length;
            char* copy = NULL;
            if (length < 0) {
                length = (int)strlen(chars);
            }
            copy = (char*)malloc((size_t)length + 1u);
            if (!copy) {
                native_extension_set_error(error_buf, error_buf_size, "Out of memory while cloning queued map string value");
                return false;
            }
            if (length > 0) {
                memcpy(copy, chars, (size_t)length);
            }
            copy[length] = '\0';
            dst->as.string_value.chars = copy;
            dst->as.string_value.length = length;
            return true;
        }
        case TABLO_EXT_TYPE_BYTES: {
            const uint8_t* bytes = src->as.bytes_value.bytes;
            int length = src->as.bytes_value.length;
            uint8_t* copy = NULL;
            if (length < 0) {
                native_extension_set_error(error_buf, error_buf_size, "Queued map bytes value length is invalid");
                return false;
            }
            if (length > 0) {
                copy = (uint8_t*)malloc((size_t)length);
                if (!copy) {
                    native_extension_set_error(error_buf, error_buf_size, "Out of memory while cloning queued map bytes value");
                    return false;
                }
                memcpy(copy, bytes, (size_t)length);
            }
            dst->as.bytes_value.bytes = copy;
            dst->as.bytes_value.length = length;
            return true;
        }
        case TABLO_EXT_TYPE_HANDLE:
            if (!src->handle_type_name || src->handle_type_name[0] == '\0') {
                native_extension_set_error(error_buf, error_buf_size, "Queued map handle value is missing a handle type");
                return false;
            }
            if (!src->as.handle_value) {
                native_extension_set_error(error_buf, error_buf_size, "Queued map handle value cannot be null");
                return false;
            }
            dst->as.handle_value = src->as.handle_value;
            return true;
        default:
            native_extension_set_error(error_buf,
                                       error_buf_size,
                                       "Queued extension map container items currently support only nil, scalars, bytes, and opaque handles");
            return false;
    }
}

static bool native_extension_clone_posted_map_entry_value(const TabloExtValue* src,
                                                          TabloExtValue* dst,
                                                          bool allow_nested_map,
                                                          char* error_buf,
                                                          size_t error_buf_size) {
    if (!src || !dst) return false;

    if (src->tag == TABLO_EXT_TYPE_ARRAY) {
        TabloExtValue* items = NULL;
        int count = src->as.array_value.count;
        memset(dst, 0, sizeof(*dst));
        dst->tag = TABLO_EXT_TYPE_ARRAY;
        dst->is_nil = src->is_nil;
        if (dst->is_nil) return true;
        if (count < 0) {
            native_extension_set_error(error_buf, error_buf_size, "Queued extension callback map array value count is invalid");
            return false;
        }
        if (count > 0 && !src->as.array_value.items) {
            native_extension_set_error(error_buf, error_buf_size, "Queued extension callback map array items are not initialized");
            return false;
        }
        if (count > 0) {
            items = (TabloExtValue*)calloc((size_t)count, sizeof(TabloExtValue));
            if (!items) {
                native_extension_set_error(error_buf, error_buf_size, "Out of memory while cloning queued map array value");
                return false;
            }
            for (int i = 0; i < count; i++) {
                if (!native_extension_clone_posted_map_item_value(&src->as.array_value.items[i],
                                                                  &items[i],
                                                                  error_buf,
                                                                  error_buf_size)) {
                    for (int j = 0; j < i; j++) {
                        native_extension_posted_callback_value_free(&items[j]);
                    }
                    free(items);
                    return false;
                }
            }
        }
        dst->as.array_value.items = items;
        dst->as.array_value.count = count;
        return true;
    }

    if (src->tag == TABLO_EXT_TYPE_TUPLE) {
        TabloExtValue* items = NULL;
        int count = src->as.tuple_value.count;
        memset(dst, 0, sizeof(*dst));
        dst->tag = TABLO_EXT_TYPE_TUPLE;
        dst->is_nil = src->is_nil;
        if (dst->is_nil) return true;
        if (count < 0) {
            native_extension_set_error(error_buf, error_buf_size, "Queued extension callback map tuple value count is invalid");
            return false;
        }
        if (count > 0 && !src->as.tuple_value.items) {
            native_extension_set_error(error_buf, error_buf_size, "Queued extension callback map tuple items are not initialized");
            return false;
        }
        if (count > 0) {
            items = (TabloExtValue*)calloc((size_t)count, sizeof(TabloExtValue));
            if (!items) {
                native_extension_set_error(error_buf, error_buf_size, "Out of memory while cloning queued map tuple value");
                return false;
            }
            for (int i = 0; i < count; i++) {
                if (!native_extension_clone_posted_map_item_value(&src->as.tuple_value.items[i],
                                                                  &items[i],
                                                                  error_buf,
                                                                  error_buf_size)) {
                    for (int j = 0; j < i; j++) {
                        native_extension_posted_callback_value_free(&items[j]);
                    }
                    free(items);
                    return false;
                }
            }
        }
        dst->as.tuple_value.items = items;
        dst->as.tuple_value.count = count;
        return true;
    }

    if (src->tag == TABLO_EXT_TYPE_MAP) {
        TabloExtMapEntry* entries = NULL;
        int count = src->as.map_value.count;
        if (!allow_nested_map) {
            native_extension_set_error(error_buf,
                                       error_buf_size,
                                       "Queued extension callback map values only support one direct nested map layer");
            return false;
        }
        memset(dst, 0, sizeof(*dst));
        dst->tag = TABLO_EXT_TYPE_MAP;
        dst->is_nil = src->is_nil;
        if (dst->is_nil) return true;
        if (count < 0) {
            native_extension_set_error(error_buf, error_buf_size, "Queued extension callback nested map count is invalid");
            return false;
        }
        if (count > 0 && !src->as.map_value.entries) {
            native_extension_set_error(error_buf, error_buf_size, "Queued extension callback nested map entries are not initialized");
            return false;
        }
        if (count > 0) {
            entries = (TabloExtMapEntry*)calloc((size_t)count, sizeof(TabloExtMapEntry));
            if (!entries) {
                native_extension_set_error(error_buf, error_buf_size, "Out of memory while cloning queued nested map value");
                return false;
            }
            for (int i = 0; i < count; i++) {
                const TabloExtMapEntry* src_entry = &src->as.map_value.entries[i];
                char* key_copy = NULL;
                int key_length = src_entry->key_length;
                if (!src_entry->key_chars) {
                    native_extension_set_error(error_buf, error_buf_size, "Queued extension callback nested map key is not initialized");
                    for (int j = 0; j < i; j++) {
                        if (entries[j].key_chars) free((void*)entries[j].key_chars);
                        native_extension_posted_callback_value_free(&entries[j].value);
                    }
                    free(entries);
                    return false;
                }
                if (key_length < 0) {
                    key_length = (int)strlen(src_entry->key_chars);
                }
                key_copy = (char*)malloc((size_t)key_length + 1u);
                if (!key_copy) {
                    native_extension_set_error(error_buf, error_buf_size, "Out of memory while cloning queued nested map key");
                    for (int j = 0; j < i; j++) {
                        if (entries[j].key_chars) free((void*)entries[j].key_chars);
                        native_extension_posted_callback_value_free(&entries[j].value);
                    }
                    free(entries);
                    return false;
                }
                if (key_length > 0) {
                    memcpy(key_copy, src_entry->key_chars, (size_t)key_length);
                }
                key_copy[key_length] = '\0';
                entries[i].key_chars = key_copy;
                entries[i].key_length = key_length;
                if (!native_extension_clone_posted_map_entry_value(&src_entry->value,
                                                                   &entries[i].value,
                                                                   false,
                                                                   error_buf,
                                                                   error_buf_size)) {
                    for (int j = 0; j <= i; j++) {
                        if (entries[j].key_chars) free((void*)entries[j].key_chars);
                        native_extension_posted_callback_value_free(&entries[j].value);
                    }
                    free(entries);
                    return false;
                }
            }
        }
        dst->as.map_value.entries = entries;
        dst->as.map_value.count = count;
        return true;
    }

    return native_extension_clone_posted_map_item_value(src, dst, error_buf, error_buf_size);
}

static void native_extension_posted_callback_value_free(TabloExtValue* value) {
    if (!value) return;
    if (value->is_nil) return;
    switch (value->tag) {
        case TABLO_EXT_TYPE_STRING:
            if (value->as.string_value.chars) {
                free((void*)value->as.string_value.chars);
                value->as.string_value.chars = NULL;
            }
            value->as.string_value.length = 0;
            break;
        case TABLO_EXT_TYPE_BYTES:
            if (value->as.bytes_value.bytes) {
                free((void*)value->as.bytes_value.bytes);
                value->as.bytes_value.bytes = NULL;
            }
            value->as.bytes_value.length = 0;
            break;
        case TABLO_EXT_TYPE_ARRAY:
            if (value->as.array_value.items) {
                TabloExtValue* items = (TabloExtValue*)value->as.array_value.items;
                for (int i = 0; i < value->as.array_value.count; i++) {
                    native_extension_posted_callback_value_free(&items[i]);
                }
                free(items);
                value->as.array_value.items = NULL;
            }
            value->as.array_value.count = 0;
            break;
        case TABLO_EXT_TYPE_TUPLE:
            if (value->as.tuple_value.items) {
                TabloExtValue* items = (TabloExtValue*)value->as.tuple_value.items;
                for (int i = 0; i < value->as.tuple_value.count; i++) {
                    native_extension_posted_callback_value_free(&items[i]);
                }
                free(items);
                value->as.tuple_value.items = NULL;
            }
            value->as.tuple_value.count = 0;
            break;
        case TABLO_EXT_TYPE_MAP:
            if (value->as.map_value.entries) {
                TabloExtMapEntry* entries = (TabloExtMapEntry*)value->as.map_value.entries;
                for (int i = 0; i < value->as.map_value.count; i++) {
                    if (entries[i].key_chars) {
                        free((void*)entries[i].key_chars);
                        entries[i].key_chars = NULL;
                    }
                    native_extension_posted_callback_value_free(&entries[i].value);
                }
                free(entries);
                value->as.map_value.entries = NULL;
            }
            value->as.map_value.count = 0;
            break;
        default:
            break;
    }
}

static bool native_extension_clone_posted_callback_value(const TabloExtTypeDesc* expected_type,
                                                         const TabloExtValue* src,
                                                         TabloExtValue* dst,
                                                         char* error_buf,
                                                         size_t error_buf_size) {
    if (!expected_type || !src || !dst) return false;

    memset(dst, 0, sizeof(*dst));
    if (!native_extension_callback_post_type_supported(expected_type)) {
        native_extension_set_error(error_buf,
                                   error_buf_size,
                                   "Queued extension callbacks currently support scalars, opaque handles, one-dimensional arrays, flat tuples, and flat maps");
        return false;
    }
    if (src->tag != expected_type->tag) {
        native_extension_set_error(error_buf,
                                   error_buf_size,
                                   "Queued extension callback argument tag does not match the callback signature");
        return false;
    }

    dst->tag = src->tag;
    dst->is_nil = src->is_nil;
    if (dst->is_nil) {
        if (!expected_type->nullable) {
            native_extension_set_error(error_buf,
                                       error_buf_size,
                                       "Queued extension callback cannot pass nil to a non-nullable parameter");
            return false;
        }
        return true;
    }

    switch (src->tag) {
        case TABLO_EXT_TYPE_INT:
            dst->as.int_value = src->as.int_value;
            return true;
        case TABLO_EXT_TYPE_BOOL:
            dst->as.bool_value = src->as.bool_value;
            return true;
        case TABLO_EXT_TYPE_DOUBLE:
            dst->as.double_value = src->as.double_value;
            return true;
        case TABLO_EXT_TYPE_STRING: {
            const char* chars = src->as.string_value.chars ? src->as.string_value.chars : "";
            int length = src->as.string_value.length;
            char* copy = NULL;
            if (length < 0) {
                length = (int)strlen(chars);
            }
            copy = (char*)malloc((size_t)length + 1u);
            if (!copy) {
                native_extension_set_error(error_buf, error_buf_size, "Out of memory while cloning queued string callback argument");
                return false;
            }
            if (length > 0) {
                memcpy(copy, chars, (size_t)length);
            }
            copy[length] = '\0';
            dst->as.string_value.chars = copy;
            dst->as.string_value.length = length;
            return true;
        }
        case TABLO_EXT_TYPE_BYTES: {
            const uint8_t* bytes = src->as.bytes_value.bytes;
            int length = src->as.bytes_value.length;
            uint8_t* copy = NULL;
            if (length < 0) {
                native_extension_set_error(error_buf, error_buf_size, "Queued extension callback bytes argument length is invalid");
                return false;
            }
            if (length > 0) {
                copy = (uint8_t*)malloc((size_t)length);
                if (!copy) {
                    native_extension_set_error(error_buf, error_buf_size, "Out of memory while cloning queued bytes callback argument");
                    return false;
                }
                memcpy(copy, bytes, (size_t)length);
            }
            dst->as.bytes_value.bytes = copy;
            dst->as.bytes_value.length = length;
            return true;
        }
        case TABLO_EXT_TYPE_HANDLE:
            dst->handle_type_name = src->handle_type_name ? src->handle_type_name : expected_type->handle_type_name;
            if (!expected_type->handle_type_name || !dst->handle_type_name ||
                strcmp(dst->handle_type_name, expected_type->handle_type_name) != 0) {
                native_extension_set_error(error_buf,
                                           error_buf_size,
                                           "Queued extension callback handle argument type does not match the callback signature");
                return false;
            }
            if (!src->as.handle_value) {
                native_extension_set_error(error_buf,
                                           error_buf_size,
                                           "Queued extension callback cannot pass a null handle payload");
                return false;
            }
            dst->as.handle_value = src->as.handle_value;
            return true;
        case TABLO_EXT_TYPE_ARRAY: {
            TabloExtValue* items = NULL;
            int count = src->as.array_value.count;
            if (!expected_type->element_type) {
                native_extension_set_error(error_buf,
                                           error_buf_size,
                                           "Queued extension callback array argument is missing an element type");
                return false;
            }
            if (count < 0) {
                native_extension_set_error(error_buf,
                                           error_buf_size,
                                           "Queued extension callback array argument count is invalid");
                return false;
            }
            if (count > 0 && !src->as.array_value.items) {
                native_extension_set_error(error_buf,
                                           error_buf_size,
                                           "Queued extension callback array argument items are not initialized");
                return false;
            }
            if (count > 0) {
                items = (TabloExtValue*)calloc((size_t)count, sizeof(TabloExtValue));
                if (!items) {
                    native_extension_set_error(error_buf,
                                               error_buf_size,
                                               "Out of memory while cloning queued array callback argument");
                    return false;
                }
                for (int i = 0; i < count; i++) {
                    if (!native_extension_clone_posted_callback_value(expected_type->element_type,
                                                                      &src->as.array_value.items[i],
                                                                      &items[i],
                                                                      error_buf,
                                                                      error_buf_size)) {
                        for (int j = 0; j < i; j++) {
                            native_extension_posted_callback_value_free(&items[j]);
                        }
                        free(items);
                        return false;
                    }
                }
            }
            dst->as.array_value.items = items;
            dst->as.array_value.count = count;
            return true;
        }
        case TABLO_EXT_TYPE_TUPLE: {
            TabloExtValue* items = NULL;
            int count = src->as.tuple_value.count;
            if (count != expected_type->tuple_element_count) {
                native_extension_set_error(error_buf,
                                           error_buf_size,
                                           "Queued extension callback tuple arity does not match the callback signature");
                return false;
            }
            if (count > 0 && !src->as.tuple_value.items) {
                native_extension_set_error(error_buf,
                                           error_buf_size,
                                           "Queued extension callback tuple argument items are not initialized");
                return false;
            }
            if (count > 0) {
                items = (TabloExtValue*)calloc((size_t)count, sizeof(TabloExtValue));
                if (!items) {
                    native_extension_set_error(error_buf,
                                               error_buf_size,
                                               "Out of memory while cloning queued tuple callback argument");
                    return false;
                }
                for (int i = 0; i < count; i++) {
                    if (!native_extension_clone_posted_callback_value(&expected_type->tuple_element_types[i],
                                                                      &src->as.tuple_value.items[i],
                                                                      &items[i],
                                                                      error_buf,
                                                                      error_buf_size)) {
                        for (int j = 0; j < i; j++) {
                            native_extension_posted_callback_value_free(&items[j]);
                        }
                        free(items);
                        return false;
                    }
                }
            }
            dst->as.tuple_value.items = items;
            dst->as.tuple_value.count = count;
            return true;
        }
        case TABLO_EXT_TYPE_MAP: {
            TabloExtMapEntry* entries = NULL;
            int count = src->as.map_value.count;
            if (count < 0) {
                native_extension_set_error(error_buf,
                                           error_buf_size,
                                           "Queued extension callback map argument count is invalid");
                return false;
            }
            if (count > 0 && !src->as.map_value.entries) {
                native_extension_set_error(error_buf,
                                           error_buf_size,
                                           "Queued extension callback map argument entries are not initialized");
                return false;
            }
            if (count > 0) {
                entries = (TabloExtMapEntry*)calloc((size_t)count, sizeof(TabloExtMapEntry));
                if (!entries) {
                    native_extension_set_error(error_buf,
                                               error_buf_size,
                                               "Out of memory while cloning queued map callback argument");
                    return false;
                }
                for (int i = 0; i < count; i++) {
                    const TabloExtMapEntry* src_entry = &src->as.map_value.entries[i];
                    char* key_copy = NULL;
                    int key_length = src_entry->key_length;
                    if (!src_entry->key_chars) {
                        native_extension_set_error(error_buf,
                                                   error_buf_size,
                                                   "Queued extension callback map key is not initialized");
                        for (int j = 0; j < i; j++) {
                            if (entries[j].key_chars) free((void*)entries[j].key_chars);
                            native_extension_posted_callback_value_free(&entries[j].value);
                        }
                        free(entries);
                        return false;
                    }
                    if (key_length < 0) {
                        key_length = (int)strlen(src_entry->key_chars);
                    }
                    key_copy = (char*)malloc((size_t)key_length + 1u);
                    if (!key_copy) {
                        native_extension_set_error(error_buf,
                                                   error_buf_size,
                                                   "Out of memory while cloning queued map callback key");
                        for (int j = 0; j < i; j++) {
                            if (entries[j].key_chars) free((void*)entries[j].key_chars);
                            native_extension_posted_callback_value_free(&entries[j].value);
                        }
                        free(entries);
                        return false;
                    }
                    if (key_length > 0) {
                        memcpy(key_copy, src_entry->key_chars, (size_t)key_length);
                    }
                    key_copy[key_length] = '\0';
                    entries[i].key_chars = key_copy;
                    entries[i].key_length = key_length;
                    if (!native_extension_clone_posted_map_entry_value(&src_entry->value,
                                                                       &entries[i].value,
                                                                       true,
                                                                       error_buf,
                                                                       error_buf_size)) {
                        for (int j = 0; j <= i; j++) {
                            if (entries[j].key_chars) free((void*)entries[j].key_chars);
                            native_extension_posted_callback_value_free(&entries[j].value);
                        }
                        free(entries);
                        return false;
                    }
                }
            }
            dst->as.map_value.entries = entries;
            dst->as.map_value.count = count;
            return true;
        }
        default:
            native_extension_set_error(error_buf,
                                       error_buf_size,
                                       "Queued extension callbacks currently support scalars, opaque handles, one-dimensional arrays, flat tuples, and flat maps");
            return false;
    }
}

static void native_extension_posted_callback_event_free(void* payload) {
    NativeExtensionPostedCallbackEvent* event = (NativeExtensionPostedCallbackEvent*)payload;
    if (!event) return;
    if (event->args) {
        for (int i = 0; i < event->arg_count; i++) {
            native_extension_posted_callback_value_free(&event->args[i]);
        }
        free(event->args);
    }
    if (event->callback) {
        if (event->counted_pending) {
            native_extension_callback_decrement_pending_impl(event->callback);
            event->counted_pending = false;
        }
        native_extension_callback_release_impl(event->callback);
    }
    if (event->gate) {
        native_extension_callback_gate_release_impl(event->gate);
    }
    free(event);
}

static bool native_extension_dispatch_posted_callback_event(VM* vm, void* payload) {
    NativeExtensionPostedCallbackEvent* event = (NativeExtensionPostedCallbackEvent*)payload;
    NativeExtensionCallState state;
    Value* arg_values = NULL;
    Value result_value;
    int converted_count = 0;
    int rc = 0;

    if (!vm || !event || !event->callback) return false;
    if (event->gate && !native_extension_callback_gate_matches(event->gate, event->gate_generation)) {
        return true;
    }

    memset(&state, 0, sizeof(state));
    state.vm = vm;

    if (event->arg_count > 0) {
        arg_values = (Value*)calloc((size_t)event->arg_count, sizeof(Value));
        if (!arg_values) {
            vm_runtime_error(vm, "Out of memory while dispatching queued extension callback");
            return false;
        }
        for (int i = 0; i < event->arg_count; i++) {
            if (!native_extension_convert_callback_arg_to_value(&state,
                                                                &event->args[i],
                                                                &event->callback->type.callback_param_types[i],
                                                                &arg_values[i])) {
                goto cleanup;
            }
            converted_count++;
        }
    }

    value_init_nil(&result_value);
    rc = vm_call_value_sync(vm, &event->callback->callable, arg_values, event->arg_count, &result_value);
    value_free(&result_value);

cleanup:
    if (arg_values) {
        for (int i = 0; i < converted_count; i++) {
            value_free(&arg_values[i]);
        }
        free(arg_values);
    }
    native_extension_call_state_free_temp_values(&state);
    return rc == 0 && !vm_has_error(vm);
}

static void native_extension_call_state_free_temp_arrays(NativeExtensionCallState* state) {
    if (!state || !state->temp_arrays) return;
    for (int i = 0; i < state->temp_array_count; i++) {
        if (state->temp_arrays[i].items) {
            for (int j = 0; j < state->temp_arrays[i].count; j++) {
                native_extension_temp_map_value_free(&state->temp_arrays[i].items[j]);
            }
            free(state->temp_arrays[i].items);
        }
    }
    free(state->temp_arrays);
    state->temp_arrays = NULL;
    state->temp_array_count = 0;
    state->temp_array_capacity = 0;
}

static void native_extension_call_state_free_temp_maps(NativeExtensionCallState* state) {
    if (!state || !state->temp_maps) return;
    for (int i = 0; i < state->temp_map_count; i++) {
        if (state->temp_maps[i].entries) {
            for (int j = 0; j < state->temp_maps[i].count; j++) {
                native_extension_temp_map_value_free(&state->temp_maps[i].entries[j].value);
            }
            free(state->temp_maps[i].entries);
        }
    }
    free(state->temp_maps);
    state->temp_maps = NULL;
    state->temp_map_count = 0;
    state->temp_map_capacity = 0;
}

static void native_extension_temp_map_value_free(TabloExtValue* value) {
    if (!value || value->is_nil) return;

    switch (value->tag) {
        case TABLO_EXT_TYPE_ARRAY:
            if (value->as.array_value.items) {
                TabloExtValue* items = (TabloExtValue*)value->as.array_value.items;
                for (int i = 0; i < value->as.array_value.count; i++) {
                    native_extension_temp_map_value_free(&items[i]);
                }
                free(items);
                value->as.array_value.items = NULL;
            }
            value->as.array_value.count = 0;
            break;
        case TABLO_EXT_TYPE_TUPLE:
            if (value->as.tuple_value.items) {
                TabloExtValue* items = (TabloExtValue*)value->as.tuple_value.items;
                for (int i = 0; i < value->as.tuple_value.count; i++) {
                    native_extension_temp_map_value_free(&items[i]);
                }
                free(items);
                value->as.tuple_value.items = NULL;
            }
            value->as.tuple_value.count = 0;
            break;
        case TABLO_EXT_TYPE_MAP:
            if (value->as.map_value.entries) {
                TabloExtMapEntry* entries = (TabloExtMapEntry*)value->as.map_value.entries;
                for (int i = 0; i < value->as.map_value.count; i++) {
                    native_extension_temp_map_value_free(&entries[i].value);
                }
                free(entries);
                value->as.map_value.entries = NULL;
            }
            value->as.map_value.count = 0;
            break;
        default:
            break;
    }
}

static void native_extension_call_state_free_temp_callbacks(NativeExtensionCallState* state) {
    if (!state || !state->temp_callbacks) return;
    for (int i = 0; i < state->temp_callback_count; i++) {
        native_extension_callback_release_impl(state->temp_callbacks[i]);
    }
    free(state->temp_callbacks);
    state->temp_callbacks = NULL;
    state->temp_callback_count = 0;
    state->temp_callback_capacity = 0;
}

static bool native_extension_call_state_push_temp_callback(NativeExtensionCallState* state,
                                                           TabloExtCallback* callback) {
    if (!state) {
        native_extension_callback_release_impl(callback);
        return false;
    }
    if (state->temp_callback_count + 1 > state->temp_callback_capacity) {
        int new_capacity = state->temp_callback_capacity > 0 ? state->temp_callback_capacity * 2 : 4;
        state->temp_callbacks = (TabloExtCallback**)safe_realloc(
            state->temp_callbacks,
            (size_t)new_capacity * sizeof(TabloExtCallback*));
        state->temp_callback_capacity = new_capacity;
    }
    state->temp_callbacks[state->temp_callback_count++] = callback;
    return true;
}

static void native_extension_call_state_free_temp_values(NativeExtensionCallState* state) {
    if (!state || !state->temp_values) return;
    for (int i = 0; i < state->temp_value_count; i++) {
        value_free(&state->temp_values[i]);
    }
    free(state->temp_values);
    state->temp_values = NULL;
    state->temp_value_count = 0;
    state->temp_value_capacity = 0;
}

static bool native_extension_call_state_push_temp_value(NativeExtensionCallState* state, Value* value) {
    if (!state || !value) {
        if (value) value_free(value);
        return false;
    }
    if (state->temp_value_count + 1 > state->temp_value_capacity) {
        int new_capacity = state->temp_value_capacity > 0 ? state->temp_value_capacity * 2 : 4;
        state->temp_values = (Value*)safe_realloc(state->temp_values, (size_t)new_capacity * sizeof(Value));
        state->temp_value_capacity = new_capacity;
    }
    state->temp_values[state->temp_value_count++] = *value;
    value_init_nil(value);
    return true;
}

static bool native_extension_call_state_push_temp_array(NativeExtensionCallState* state,
                                                        TabloExtValue* items,
                                                        int count) {
    if (!state) {
        if (items) free(items);
        return false;
    }
    if (state->temp_array_count + 1 > state->temp_array_capacity) {
        int new_capacity = state->temp_array_capacity > 0 ? state->temp_array_capacity * 2 : 4;
        state->temp_arrays = (NativeExtensionTempArray*)safe_realloc(
            state->temp_arrays,
            (size_t)new_capacity * sizeof(NativeExtensionTempArray));
        state->temp_array_capacity = new_capacity;
    }
    state->temp_arrays[state->temp_array_count].items = items;
    state->temp_arrays[state->temp_array_count].count = count;
    state->temp_array_count++;
    return true;
}

static bool native_extension_call_state_push_temp_map(NativeExtensionCallState* state,
                                                      TabloExtMapEntry* entries,
                                                      int count) {
    if (!state) {
        if (entries) free(entries);
        return false;
    }
    if (state->temp_map_count + 1 > state->temp_map_capacity) {
        int new_capacity = state->temp_map_capacity > 0 ? state->temp_map_capacity * 2 : 4;
        state->temp_maps = (NativeExtensionTempMap*)safe_realloc(
            state->temp_maps,
            (size_t)new_capacity * sizeof(NativeExtensionTempMap));
        state->temp_map_capacity = new_capacity;
    }
    state->temp_maps[state->temp_map_count].entries = entries;
    state->temp_maps[state->temp_map_count].count = count;
    state->temp_map_count++;
    return true;
}

static bool native_extension_convert_value_to_dynamic_map_value_impl(NativeExtensionCallState* state,
                                                                     const Value* value,
                                                                     bool allow_containers,
                                                                     bool allow_nested_map,
                                                                     TabloExtValue* out_value) {
    ObjRecord* record = NULL;
    if (!state || !value || !out_value) return false;

    memset(out_value, 0, sizeof(*out_value));
    if (value_get_type(value) == VAL_NIL) {
        out_value->tag = TABLO_EXT_TYPE_VOID;
        out_value->is_nil = true;
        return true;
    }

    switch (value_get_type(value)) {
        case VAL_INT:
            out_value->tag = TABLO_EXT_TYPE_INT;
            out_value->as.int_value = value_get_int(value);
            return true;
        case VAL_BOOL:
            out_value->tag = TABLO_EXT_TYPE_BOOL;
            out_value->as.bool_value = value_get_bool(value);
            return true;
        case VAL_DOUBLE:
            out_value->tag = TABLO_EXT_TYPE_DOUBLE;
            out_value->as.double_value = value_get_double(value);
            return true;
        case VAL_STRING: {
            ObjString* str = value_get_string_obj(value);
            if (!str) break;
            out_value->tag = TABLO_EXT_TYPE_STRING;
            out_value->as.string_value.chars = str->chars;
            out_value->as.string_value.length = str->length;
            return true;
        }
        case VAL_BYTES: {
            ObjBytes* bytes = value_get_bytes_obj(value);
            if (!bytes) break;
            out_value->tag = TABLO_EXT_TYPE_BYTES;
            out_value->as.bytes_value.bytes = obj_bytes_data(bytes);
            out_value->as.bytes_value.length = bytes->length;
            return true;
        }
        case VAL_ARRAY:
            if (allow_containers) {
                ObjArray* array_obj = value_get_array_obj(value);
                TabloExtValue* items = NULL;
                if (!array_obj) break;
                out_value->tag = TABLO_EXT_TYPE_ARRAY;
                if (array_obj->count > 0) {
                    items = (TabloExtValue*)safe_calloc((size_t)array_obj->count, sizeof(TabloExtValue));
                    for (int i = 0; i < array_obj->count; i++) {
                        Value element_value;
                        obj_array_get(array_obj, i, &element_value);
                        if (!native_extension_convert_value_to_dynamic_map_value_impl(state,
                                                                                      &element_value,
                                                                                      false,
                                                                                      false,
                                                                                      &items[i])) {
                            for (int j = 0; j < i; j++) {
                                native_extension_posted_callback_value_free(&items[j]);
                            }
                            free(items);
                            return false;
                        }
                    }
                }
                out_value->as.array_value.items = items;
                out_value->as.array_value.count = array_obj->count;
                return true;
            }
            break;
        case VAL_TUPLE:
            if (allow_containers) {
                ObjTuple* tuple_obj = value_get_tuple_obj(value);
                TabloExtValue* items = NULL;
                if (!tuple_obj) break;
                out_value->tag = TABLO_EXT_TYPE_TUPLE;
                if (tuple_obj->element_count > 0) {
                    items = (TabloExtValue*)safe_calloc((size_t)tuple_obj->element_count, sizeof(TabloExtValue));
                    for (int i = 0; i < tuple_obj->element_count; i++) {
                        Value element_value;
                        obj_tuple_get(tuple_obj, i, &element_value);
                        if (!native_extension_convert_value_to_dynamic_map_value_impl(state,
                                                                                      &element_value,
                                                                                      false,
                                                                                      false,
                                                                                      &items[i])) {
                            for (int j = 0; j < i; j++) {
                                native_extension_posted_callback_value_free(&items[j]);
                            }
                            free(items);
                            return false;
                        }
                    }
                }
                out_value->as.tuple_value.items = items;
                out_value->as.tuple_value.count = tuple_obj->element_count;
                return true;
            }
            break;
        case VAL_MAP:
            if (allow_nested_map) {
                ObjMap* map_obj = value_get_map_obj(value);
                TabloExtMapEntry* entries = NULL;
                int entry_index = 0;
                if (!map_obj) break;
                out_value->tag = TABLO_EXT_TYPE_MAP;
                if (map_obj->count > 0) {
                    entries = (TabloExtMapEntry*)safe_calloc((size_t)map_obj->count, sizeof(TabloExtMapEntry));
                    for (int i = 0; i < map_obj->capacity; i++) {
                        MapSlot* map_slot = &map_obj->slots[i];
                        ObjString* key = NULL;
                        if (map_slot->hash < 2) continue;
                        key = value_get_string_obj(&map_slot->key);
                        if (value_get_type(&map_slot->key) != VAL_STRING || !key || !key->chars) {
                            free(entries);
                            native_extension_set_runtime_error_message(state, "TabloLang callback map results currently require string keys");
                            return false;
                        }
                        entries[entry_index].key_chars = key->chars;
                        entries[entry_index].key_length = key->length;
                        if (!native_extension_convert_value_to_dynamic_map_value_impl(state,
                                                                                      &map_slot->value,
                                                                                      true,
                                                                                      false,
                                                                                      &entries[entry_index].value)) {
                            for (int j = 0; j < entry_index; j++) {
                                native_extension_temp_map_value_free(&entries[j].value);
                            }
                            free(entries);
                            return false;
                        }
                        entry_index++;
                    }
                }
                out_value->as.map_value.entries = entries;
                out_value->as.map_value.count = entry_index;
                return true;
            }
            break;
        case VAL_RECORD:
            record = value_get_record_obj(value);
            if (!record || !record->is_native_opaque || !record->type_name) {
                break;
            }
            out_value->tag = TABLO_EXT_TYPE_HANDLE;
            out_value->handle_type_name = record->type_name;
            out_value->as.handle_value = record->native_opaque_payload;
            return true;
        default:
            break;
    }

    native_extension_set_runtime_error_message(state,
                                               allow_containers
                                                   ? "Extension map values currently support nil, scalars, bytes, opaque handles, flat array/tuple values, and one nested map layer"
                                                   : "Extension map array/tuple items currently support only nil, scalars, bytes, and opaque handles");
    return false;
}

static bool native_extension_convert_value_to_dynamic_map_value(NativeExtensionCallState* state,
                                                                const Value* value,
                                                                TabloExtValue* out_value) {
    return native_extension_convert_value_to_dynamic_map_value_impl(state, value, true, true, out_value);
}

static bool native_extension_convert_dynamic_map_value_to_value_impl(NativeExtensionCallState* state,
                                                                     const TabloExtValue* ext_value,
                                                                     bool transfer_handle_ownership,
                                                                     bool allow_containers,
                                                                     bool allow_nested_map,
                                                                     Value* out_value) {
    const NativeExtensionHandleType* handle_type = NULL;
    const char* type_name = NULL;
    if (!state || !ext_value || !out_value) return false;

    if (ext_value->is_nil) {
        value_init_nil(out_value);
        return true;
    }

    switch (ext_value->tag) {
        case TABLO_EXT_TYPE_INT:
            value_init_int(out_value, ext_value->as.int_value);
            return true;
        case TABLO_EXT_TYPE_BOOL:
            value_init_bool(out_value, ext_value->as.bool_value);
            return true;
        case TABLO_EXT_TYPE_DOUBLE:
            value_init_double(out_value, ext_value->as.double_value);
            return true;
        case TABLO_EXT_TYPE_STRING: {
            const char* chars = ext_value->as.string_value.chars;
            int length = ext_value->as.string_value.length;
            if (length < 0) {
                if (!chars) {
                    native_extension_set_runtime_error_message(state, "Extension map string value length is invalid");
                    return false;
                }
                length = (int)strlen(chars);
            }
            value_init_string_n(out_value, chars ? chars : "", length);
            return true;
        }
        case TABLO_EXT_TYPE_BYTES: {
            const uint8_t* bytes = ext_value->as.bytes_value.bytes;
            int length = ext_value->as.bytes_value.length;
            if (length < 0) {
                native_extension_set_runtime_error_message(state, "Extension map bytes value length is invalid");
                return false;
            }
            value_init_bytes(out_value, obj_bytes_create_copy(bytes ? bytes : (const uint8_t*)"", length));
            return true;
        }
        case TABLO_EXT_TYPE_ARRAY:
            if (allow_containers) {
                ObjArray* array_obj = NULL;
                int count = ext_value->as.array_value.count;
                const TabloExtValue* items = ext_value->as.array_value.items;
                if (count < 0) {
                    native_extension_set_runtime_error_message(state, "Extension map array value count is invalid");
                    return false;
                }
                if (count > 0 && !items) {
                    native_extension_set_runtime_error_message(state, "Extension map array items are not initialized");
                    return false;
                }
                array_obj = obj_array_create(state->vm, count > 0 ? count : 4);
                for (int i = 0; i < count; i++) {
                    Value element_value;
                    if (!native_extension_convert_dynamic_map_value_to_value_impl(state,
                                                                                 &items[i],
                                                                                 transfer_handle_ownership,
                                                                                 false,
                                                                                 false,
                                                                                 &element_value)) {
                        obj_array_release(array_obj);
                        return false;
                    }
                    obj_array_push(array_obj, element_value);
                }
                value_init_array(out_value, array_obj);
                return true;
            }
            break;
        case TABLO_EXT_TYPE_TUPLE:
            if (allow_containers) {
                ObjTuple* tuple_obj = NULL;
                int count = ext_value->as.tuple_value.count;
                const TabloExtValue* items = ext_value->as.tuple_value.items;
                if (count < 0) {
                    native_extension_set_runtime_error_message(state, "Extension map tuple value count is invalid");
                    return false;
                }
                if (count > 0 && !items) {
                    native_extension_set_runtime_error_message(state, "Extension map tuple items are not initialized");
                    return false;
                }
                tuple_obj = obj_tuple_create(state->vm, count);
                if (!tuple_obj) {
                    native_extension_set_runtime_error_message(state, "Out of memory while converting extension map tuple value");
                    return false;
                }
                for (int i = 0; i < count; i++) {
                    Value element_value;
                    if (!native_extension_convert_dynamic_map_value_to_value_impl(state,
                                                                                 &items[i],
                                                                                 transfer_handle_ownership,
                                                                                 false,
                                                                                 false,
                                                                                 &element_value)) {
                        obj_tuple_release(tuple_obj);
                        return false;
                    }
                    obj_tuple_set(tuple_obj, i, element_value);
                }
                value_init_tuple(out_value, tuple_obj);
                return true;
            }
            break;
        case TABLO_EXT_TYPE_MAP:
            if (allow_nested_map) {
                ObjMap* map_obj = NULL;
                const TabloExtMapEntry* entries = ext_value->as.map_value.entries;
                int count = ext_value->as.map_value.count;
                if (count < 0) {
                    native_extension_set_runtime_error_message(state, "Extension nested map value count is invalid");
                    return false;
                }
                if (count > 0 && !entries) {
                    native_extension_set_runtime_error_message(state, "Extension nested map entries are not initialized");
                    return false;
                }

                map_obj = obj_map_create(state->vm);
                if (!map_obj) {
                    native_extension_set_runtime_error_message(state, "Out of memory while converting extension nested map value");
                    return false;
                }

                for (int i = 0; i < count; i++) {
                    Value map_value;
                    int key_length = entries[i].key_length;
                    if (!entries[i].key_chars) {
                        obj_map_release(map_obj);
                        native_extension_set_runtime_error_message(state, "Extension nested map key is not initialized");
                        return false;
                    }
                    if (key_length < 0) {
                        key_length = (int)strlen(entries[i].key_chars);
                    }
                    if (!native_extension_convert_dynamic_map_value_to_value_impl(state,
                                                                                 &entries[i].value,
                                                                                 transfer_handle_ownership,
                                                                                 true,
                                                                                 false,
                                                                                 &map_value)) {
                        obj_map_release(map_obj);
                        return false;
                    }
                    obj_map_set_cstr_n(map_obj, entries[i].key_chars, key_length, map_value);
                    value_free(&map_value);
                }

                value_init_map(out_value, map_obj);
                return true;
            }
            native_extension_set_runtime_error_message(state, "Extension nested map values currently support only one direct nested map layer");
            return false;
        case TABLO_EXT_TYPE_HANDLE:
            type_name = ext_value->handle_type_name;
            if (!type_name || type_name[0] == '\0') {
                native_extension_set_runtime_error_message(state, "Extension map handle value is missing a handle type");
                return false;
            }
            if (!ext_value->as.handle_value) {
                native_extension_set_runtime_error_message(state, "Extension map handle value cannot be null");
                return false;
            }
            handle_type = native_extension_lookup_handle_type(state->registry, NULL, type_name);
            if (!handle_type) {
                native_extension_set_runtime_error_message(state, "Extension map value references an unknown handle type");
                return false;
            }
            value_init_record(out_value,
                              obj_record_create_opaque(state->vm,
                                                       type_name,
                                                       ext_value->as.handle_value,
                                                       transfer_handle_ownership ? handle_type->destroy : NULL));
            return true;
        default:
            native_extension_set_runtime_error_message(state,
                                                       allow_containers
                                                           ? "Extension map values currently support nil, scalars, bytes, opaque handles, flat array/tuple values, and one nested map layer"
                                                           : "Extension map array/tuple items currently support only nil, scalars, bytes, and opaque handles");
            return false;
    }

    return false;
}

static bool native_extension_convert_dynamic_map_value_to_value(NativeExtensionCallState* state,
                                                                const TabloExtValue* ext_value,
                                                                bool transfer_handle_ownership,
                                                                Value* out_value) {
    return native_extension_convert_dynamic_map_value_to_value_impl(state,
                                                                    ext_value,
                                                                    transfer_handle_ownership,
                                                                    true,
                                                                    true,
                                                                    out_value);
}

static bool native_extension_convert_map_value_to_ext_map_value(NativeExtensionCallState* state,
                                                                const Value* value,
                                                                TabloExtValue* out_value) {
    ObjMap* map_obj = NULL;
    TabloExtMapEntry* entries = NULL;
    int entry_index = 0;

    if (!state || !value || !out_value) return false;
    if (value_get_type(value) != VAL_MAP) {
        native_extension_set_runtime_error_message(state, "Extension array element expected map<string, any>");
        return false;
    }

    map_obj = value_get_map_obj(value);
    if (!map_obj) {
        native_extension_set_runtime_error_message(state, "Extension array element expected map<string, any>");
        return false;
    }

    memset(out_value, 0, sizeof(*out_value));
    out_value->tag = TABLO_EXT_TYPE_MAP;

    if (map_obj->count > 0) {
        entries = (TabloExtMapEntry*)safe_calloc((size_t)map_obj->count, sizeof(TabloExtMapEntry));
        for (int i = 0; i < map_obj->capacity; i++) {
            MapSlot* map_slot = &map_obj->slots[i];
            ObjString* key = NULL;
            if (map_slot->hash < 2) continue;
            key = value_get_string_obj(&map_slot->key);
            if (value_get_type(&map_slot->key) != VAL_STRING || !key || !key->chars) {
                free(entries);
                native_extension_set_runtime_error_message(state, "Extension maps currently require string keys");
                return false;
            }
            entries[entry_index].key_chars = key->chars;
            entries[entry_index].key_length = key->length;
            if (!native_extension_convert_value_to_dynamic_map_value(state,
                                                                    &map_slot->value,
                                                                    &entries[entry_index].value)) {
                for (int j = 0; j < entry_index; j++) {
                    native_extension_temp_map_value_free(&entries[j].value);
                }
                free(entries);
                return false;
            }
            entry_index++;
        }
    }

    out_value->as.map_value.entries = entries;
    out_value->as.map_value.count = entry_index;
    return true;
}

static bool native_extension_convert_ext_map_value_to_value(NativeExtensionCallState* state,
                                                            const TabloExtValue* ext_value,
                                                            bool transfer_handle_ownership,
                                                            Value* out_value) {
    ObjMap* map_obj = NULL;
    const TabloExtMapEntry* entries = NULL;
    int count = 0;

    if (!state || !ext_value || !out_value) return false;
    if (ext_value->tag != TABLO_EXT_TYPE_MAP) {
        native_extension_set_runtime_error_message(state, "Extension array result element tag does not match the declared element type");
        return false;
    }

    if (ext_value->is_nil) {
        value_init_nil(out_value);
        return true;
    }

    entries = ext_value->as.map_value.entries;
    count = ext_value->as.map_value.count;
    if (count < 0) {
        native_extension_set_runtime_error_message(state, "Extension map result entry count is invalid");
        return false;
    }
    if (count > 0 && !entries) {
        native_extension_set_runtime_error_message(state, "Extension map result entries are not initialized");
        return false;
    }

    map_obj = obj_map_create(state->vm);
    if (!map_obj) {
        native_extension_set_runtime_error_message(state, "Out of memory while converting extension map result");
        return false;
    }

    for (int i = 0; i < count; i++) {
        Value entry_value;
        int key_length = entries[i].key_length;
        if (!entries[i].key_chars) {
            obj_map_release(map_obj);
            native_extension_set_runtime_error_message(state, "Extension map result key is not initialized");
            return false;
        }
        if (key_length < 0) {
            key_length = (int)strlen(entries[i].key_chars);
        }
        if (!native_extension_convert_dynamic_map_value_to_value(state,
                                                                 &entries[i].value,
                                                                 transfer_handle_ownership,
                                                                 &entry_value)) {
            obj_map_release(map_obj);
            return false;
        }
        obj_map_set_cstr_n(map_obj, entries[i].key_chars, key_length, entry_value);
        value_free(&entry_value);
    }

    value_init_map(out_value, map_obj);
    return true;
}

static bool native_extension_convert_value_to_ext_value(NativeExtensionCallState* state,
                                                        const Value* value,
                                                        const TabloExtTypeDesc* expected_type,
                                                        TabloExtValue* out_value) {
    if (!state || !value || !expected_type || !out_value) return false;

    memset(out_value, 0, sizeof(*out_value));
    out_value->tag = expected_type->tag;
    out_value->handle_type_name = expected_type->handle_type_name;

    if (value_get_type(value) == VAL_NIL) {
        if (!expected_type->nullable) {
            native_extension_set_runtime_error_message(state, "Extension array element cannot be nil for a non-nullable type");
            return false;
        }
        out_value->is_nil = true;
        return true;
    }

    switch (expected_type->tag) {
        case TABLO_EXT_TYPE_INT:
            if (value_get_type(value) != VAL_INT) {
                native_extension_set_runtime_error_message(state, "Extension array element expected int");
                return false;
            }
            out_value->as.int_value = value_get_int(value);
            return true;
        case TABLO_EXT_TYPE_BOOL:
            if (value_get_type(value) != VAL_BOOL) {
                native_extension_set_runtime_error_message(state, "Extension array element expected bool");
                return false;
            }
            out_value->as.bool_value = value_get_bool(value);
            return true;
        case TABLO_EXT_TYPE_DOUBLE:
            if (value_get_type(value) != VAL_DOUBLE) {
                native_extension_set_runtime_error_message(state, "Extension array element expected double");
                return false;
            }
            out_value->as.double_value = value_get_double(value);
            return true;
        case TABLO_EXT_TYPE_STRING: {
            ObjString* str = value_get_string_obj(value);
            if (value_get_type(value) != VAL_STRING || !str) {
                native_extension_set_runtime_error_message(state, "Extension array element expected string");
                return false;
            }
            out_value->as.string_value.chars = str->chars;
            out_value->as.string_value.length = str->length;
            return true;
        }
        case TABLO_EXT_TYPE_BYTES: {
            ObjBytes* bytes = value_get_bytes_obj(value);
            if (value_get_type(value) != VAL_BYTES || !bytes) {
                native_extension_set_runtime_error_message(state, "Extension array element expected bytes");
                return false;
            }
            out_value->as.bytes_value.bytes = obj_bytes_data(bytes);
            out_value->as.bytes_value.length = bytes->length;
            return true;
        }
        case TABLO_EXT_TYPE_HANDLE: {
            ObjRecord* record = value_get_record_obj(value);
            if (value_get_type(value) != VAL_RECORD || !record || !record->is_native_opaque) {
                native_extension_set_runtime_error_message(state, "Extension array element expected an opaque handle");
                return false;
            }
            if (!expected_type->handle_type_name || !record->type_name ||
                strcmp(record->type_name, expected_type->handle_type_name) != 0) {
                native_extension_set_runtime_error_message(state, "Extension handle array element type mismatch");
                return false;
            }
            out_value->handle_type_name = expected_type->handle_type_name;
            out_value->as.handle_value = record->native_opaque_payload;
            return true;
        }
        case TABLO_EXT_TYPE_MAP:
            return native_extension_convert_map_value_to_ext_map_value(state, value, out_value);
        default:
            native_extension_set_runtime_error_message(state, "Extension array element uses an unsupported type");
            return false;
    }
}

static bool native_extension_convert_ext_value_to_value_impl(NativeExtensionCallState* state,
                                                             const TabloExtValue* ext_value,
                                                             const TabloExtTypeDesc* expected_type,
                                                             bool transfer_handle_ownership,
                                                             Value* out_value) {
    if (!state || !ext_value || !expected_type || !out_value) return false;

    if (ext_value->tag != expected_type->tag) {
        native_extension_set_runtime_error_message(state, "Extension array result element tag does not match the declared element type");
        return false;
    }

    if (ext_value->is_nil) {
        if (!expected_type->nullable) {
            native_extension_set_runtime_error_message(state, "Extension cannot return nil in a non-nullable array element");
            return false;
        }
        value_init_nil(out_value);
        return true;
    }

    switch (expected_type->tag) {
        case TABLO_EXT_TYPE_INT:
            value_init_int(out_value, ext_value->as.int_value);
            return true;
        case TABLO_EXT_TYPE_BOOL:
            value_init_bool(out_value, ext_value->as.bool_value);
            return true;
        case TABLO_EXT_TYPE_DOUBLE:
            value_init_double(out_value, ext_value->as.double_value);
            return true;
        case TABLO_EXT_TYPE_STRING: {
            const char* chars = ext_value->as.string_value.chars;
            int length = ext_value->as.string_value.length;
            if (length < 0) {
                if (!chars) {
                    native_extension_set_runtime_error_message(state, "Extension array string element length is invalid");
                    return false;
                }
                length = (int)strlen(chars);
            }
            value_init_string_n(out_value, chars ? chars : "", length);
            return true;
        }
        case TABLO_EXT_TYPE_BYTES: {
            const uint8_t* bytes = ext_value->as.bytes_value.bytes;
            int length = ext_value->as.bytes_value.length;
            if (length < 0) {
                native_extension_set_runtime_error_message(state, "Extension array bytes element length is invalid");
                return false;
            }
            value_init_bytes(out_value, obj_bytes_create_copy(bytes ? bytes : (const uint8_t*)"", length));
            return true;
        }
        case TABLO_EXT_TYPE_HANDLE: {
            const char* type_name = ext_value->handle_type_name ? ext_value->handle_type_name : expected_type->handle_type_name;
            const NativeExtensionHandleType* handle_type = NULL;
            if (!expected_type->handle_type_name || !type_name ||
                strcmp(type_name, expected_type->handle_type_name) != 0) {
                native_extension_set_runtime_error_message(state, "Extension handle array result type mismatch");
                return false;
            }
            if (!ext_value->as.handle_value) {
                native_extension_set_runtime_error_message(state, "Extension cannot return a null handle payload in an array");
                return false;
            }
            handle_type = native_extension_lookup_handle_type(state->registry, NULL, expected_type->handle_type_name);
            if (!handle_type) {
                native_extension_set_runtime_error_message(state, "Extension returned an unknown handle type in an array");
                return false;
            }
            value_init_record(out_value,
                              obj_record_create_opaque(state->vm,
                                                       expected_type->handle_type_name,
                                                       ext_value->as.handle_value,
                                                       transfer_handle_ownership ? handle_type->destroy : NULL));
            return true;
        }
        case TABLO_EXT_TYPE_MAP:
            return native_extension_convert_ext_map_value_to_value(state,
                                                                   ext_value,
                                                                   transfer_handle_ownership,
                                                                   out_value);
        default:
            native_extension_set_runtime_error_message(state, "Extension array result uses an unsupported element type");
            return false;
    }
}

static bool native_extension_convert_ext_value_to_value(NativeExtensionCallState* state,
                                                        const TabloExtValue* ext_value,
                                                        const TabloExtTypeDesc* expected_type,
                                                        Value* out_value) {
    return native_extension_convert_ext_value_to_value_impl(state,
                                                            ext_value,
                                                            expected_type,
                                                            true,
                                                            out_value);
}

static bool native_extension_convert_callback_arg_to_value(NativeExtensionCallState* state,
                                                           const TabloExtValue* ext_value,
                                                           const TabloExtTypeDesc* expected_type,
                                                           Value* out_value) {
    const char* type_name = NULL;
    if (!state || !ext_value || !expected_type || !out_value) return false;

    if (ext_value->tag != expected_type->tag) {
        native_extension_set_runtime_error_message(state, "Extension callback argument tag does not match the declared callback parameter type");
        return false;
    }

    if (ext_value->is_nil) {
        if (!expected_type->nullable) {
            native_extension_set_runtime_error_message(state, "Extension callback cannot pass nil for a non-nullable parameter");
            return false;
        }
        value_init_nil(out_value);
        return true;
    }

    switch (expected_type->tag) {
        case TABLO_EXT_TYPE_INT:
            value_init_int(out_value, ext_value->as.int_value);
            return true;
        case TABLO_EXT_TYPE_BOOL:
            value_init_bool(out_value, ext_value->as.bool_value);
            return true;
        case TABLO_EXT_TYPE_DOUBLE:
            value_init_double(out_value, ext_value->as.double_value);
            return true;
        case TABLO_EXT_TYPE_STRING: {
            const char* chars = ext_value->as.string_value.chars;
            int length = ext_value->as.string_value.length;
            if (length < 0) {
                if (!chars) {
                    native_extension_set_runtime_error_message(state, "Extension callback string argument length is invalid");
                    return false;
                }
                length = (int)strlen(chars);
            }
            value_init_string_n(out_value, chars ? chars : "", length);
            return true;
        }
        case TABLO_EXT_TYPE_BYTES: {
            const uint8_t* bytes = ext_value->as.bytes_value.bytes;
            int length = ext_value->as.bytes_value.length;
            if (length < 0) {
                native_extension_set_runtime_error_message(state, "Extension callback bytes argument length is invalid");
                return false;
            }
            value_init_bytes(out_value, obj_bytes_create_copy(bytes ? bytes : (const uint8_t*)"", length));
            return true;
        }
        case TABLO_EXT_TYPE_HANDLE:
            type_name = ext_value->handle_type_name ? ext_value->handle_type_name : expected_type->handle_type_name;
            if (!expected_type->handle_type_name || !type_name ||
                strcmp(type_name, expected_type->handle_type_name) != 0) {
                native_extension_set_runtime_error_message(state, "Extension callback handle argument type mismatch");
                return false;
            }
            if (!ext_value->as.handle_value) {
                native_extension_set_runtime_error_message(state, "Extension callback cannot pass a null handle payload");
                return false;
            }
            value_init_record(out_value,
                              obj_record_create_opaque(state->vm,
                                                       expected_type->handle_type_name,
                                                       ext_value->as.handle_value,
                                                       NULL));
            return true;
        case TABLO_EXT_TYPE_ARRAY: {
            ObjArray* array_obj = NULL;
            const TabloExtValue* items = ext_value->as.array_value.items;
            int count = ext_value->as.array_value.count;
            if (!expected_type->element_type) {
                native_extension_set_runtime_error_message(state, "Extension callback array argument is missing an element type");
                return false;
            }
            if (count < 0) {
                native_extension_set_runtime_error_message(state, "Extension callback array argument count is invalid");
                return false;
            }
            if (count > 0 && !items) {
                native_extension_set_runtime_error_message(state, "Extension callback array argument items are not initialized");
                return false;
            }

            array_obj = obj_array_create(state->vm, count > 0 ? count : 4);
            if (!array_obj) {
                native_extension_set_runtime_error_message(state, "Out of memory while converting extension callback array argument");
                return false;
            }

            for (int i = 0; i < count; i++) {
                Value element_value;
                if (!native_extension_convert_ext_value_to_value_impl(state,
                                                                      &items[i],
                                                                      expected_type->element_type,
                                                                      false,
                                                                      &element_value)) {
                    obj_array_release(array_obj);
                    return false;
                }
                obj_array_push(array_obj, element_value);
            }

            value_init_array(out_value, array_obj);
            return true;
        }
        case TABLO_EXT_TYPE_TUPLE: {
            ObjTuple* tuple = NULL;
            const TabloExtValue* items = ext_value->as.tuple_value.items;
            int count = ext_value->as.tuple_value.count;
            if (count != expected_type->tuple_element_count) {
                native_extension_set_runtime_error_message(state, "Extension callback tuple argument arity mismatch");
                return false;
            }
            if (count > 0 && !items) {
                native_extension_set_runtime_error_message(state, "Extension callback tuple argument items are not initialized");
                return false;
            }

            tuple = obj_tuple_create(state->vm, count);
            if (!tuple) {
                native_extension_set_runtime_error_message(state, "Out of memory while converting extension callback tuple argument");
                return false;
            }

            for (int i = 0; i < count; i++) {
                Value element_value;
                if (!native_extension_convert_callback_arg_to_value(state,
                                                                    &items[i],
                                                                    &expected_type->tuple_element_types[i],
                                                                    &element_value)) {
                    obj_tuple_release(tuple);
                    return false;
                }
                obj_tuple_set(tuple, i, element_value);
                value_free(&element_value);
            }

            value_init_tuple(out_value, tuple);
            return true;
        }
        case TABLO_EXT_TYPE_MAP: {
            ObjMap* map = NULL;
            const TabloExtMapEntry* entries = ext_value->as.map_value.entries;
            int count = ext_value->as.map_value.count;
            if (count < 0) {
                native_extension_set_runtime_error_message(state, "Extension callback map argument count is invalid");
                return false;
            }
            if (count > 0 && !entries) {
                native_extension_set_runtime_error_message(state, "Extension callback map argument entries are not initialized");
                return false;
            }

            map = obj_map_create(state->vm);
            if (!map) {
                native_extension_set_runtime_error_message(state, "Out of memory while converting extension callback map argument");
                return false;
            }

            for (int i = 0; i < count; i++) {
                Value map_value;
                int key_length = entries[i].key_length;
                if (!entries[i].key_chars) {
                    obj_map_release(map);
                    native_extension_set_runtime_error_message(state, "Extension callback map key is not initialized");
                    return false;
                }
                if (key_length < 0) {
                    key_length = (int)strlen(entries[i].key_chars);
                }
                if (!native_extension_convert_dynamic_map_value_to_value(state,
                                                                        &entries[i].value,
                                                                        false,
                                                                        &map_value)) {
                    obj_map_release(map);
                    return false;
                }
                obj_map_set_cstr_n(map, entries[i].key_chars, key_length, map_value);
                value_free(&map_value);
            }

            value_init_map(out_value, map);
            return true;
        }
        default:
            native_extension_set_runtime_error_message(state, "Extension callback argument uses an unsupported type");
            return false;
    }
}

static bool native_extension_convert_value_to_callback_result(NativeExtensionCallState* state,
                                                              Value* value,
                                                              const TabloExtTypeDesc* expected_type,
                                                              TabloExtValue* out_value) {
    if (!state || !value || !expected_type || !out_value) return false;

    memset(out_value, 0, sizeof(*out_value));
    out_value->tag = expected_type->tag;
    out_value->handle_type_name = expected_type->handle_type_name;

    if (expected_type->tag == TABLO_EXT_TYPE_VOID) {
        out_value->is_nil = true;
        value_free(value);
        return true;
    }

    if (value_get_type(value) == VAL_NIL) {
        if (!expected_type->nullable) {
            native_extension_set_runtime_error_message(state, "TabloLang callback returned nil for a non-nullable result");
            return false;
        }
        out_value->is_nil = true;
        value_free(value);
        return true;
    }

    switch (expected_type->tag) {
        case TABLO_EXT_TYPE_INT:
            if (value_get_type(value) != VAL_INT) {
                native_extension_set_runtime_error_message(state, "TabloLang callback result expected int");
                return false;
            }
            out_value->as.int_value = value_get_int(value);
            value_free(value);
            return true;
        case TABLO_EXT_TYPE_BOOL:
            if (value_get_type(value) != VAL_BOOL) {
                native_extension_set_runtime_error_message(state, "TabloLang callback result expected bool");
                return false;
            }
            out_value->as.bool_value = value_get_bool(value);
            value_free(value);
            return true;
        case TABLO_EXT_TYPE_DOUBLE:
            if (value_get_type(value) != VAL_DOUBLE) {
                native_extension_set_runtime_error_message(state, "TabloLang callback result expected double");
                return false;
            }
            out_value->as.double_value = value_get_double(value);
            value_free(value);
            return true;
        case TABLO_EXT_TYPE_STRING: {
            ObjString* str = NULL;
            if (value_get_type(value) != VAL_STRING || !(str = value_get_string_obj(value))) {
                native_extension_set_runtime_error_message(state, "TabloLang callback result expected string");
                return false;
            }
            out_value->as.string_value.chars = str->chars;
            out_value->as.string_value.length = str->length;
            return native_extension_call_state_push_temp_value(state, value);
        }
        case TABLO_EXT_TYPE_BYTES: {
            ObjBytes* bytes = NULL;
            if (value_get_type(value) != VAL_BYTES || !(bytes = value_get_bytes_obj(value))) {
                native_extension_set_runtime_error_message(state, "TabloLang callback result expected bytes");
                return false;
            }
            out_value->as.bytes_value.bytes = obj_bytes_data(bytes);
            out_value->as.bytes_value.length = bytes->length;
            return native_extension_call_state_push_temp_value(state, value);
        }
        case TABLO_EXT_TYPE_HANDLE: {
            ObjRecord* record = NULL;
            if (value_get_type(value) != VAL_RECORD || !(record = value_get_record_obj(value)) || !record->is_native_opaque) {
                native_extension_set_runtime_error_message(state, "TabloLang callback result expected an opaque handle");
                return false;
            }
            if (!record->type_name ||
                !expected_type->handle_type_name ||
                strcmp(record->type_name, expected_type->handle_type_name) != 0) {
                native_extension_set_runtime_error_message(state, "TabloLang callback result handle type mismatch");
                return false;
            }
            out_value->handle_type_name = expected_type->handle_type_name;
            out_value->as.handle_value = record->native_opaque_payload;
            return native_extension_call_state_push_temp_value(state, value);
        }
        case TABLO_EXT_TYPE_ARRAY: {
            ObjArray* array_obj = NULL;
            TabloExtValue* items = NULL;
            int count = 0;
            if (value_get_type(value) != VAL_ARRAY || !(array_obj = value_get_array_obj(value))) {
                native_extension_set_runtime_error_message(state, "TabloLang callback result expected an array");
                return false;
            }
            if (!expected_type->element_type) {
                native_extension_set_runtime_error_message(state, "TabloLang callback result array is missing an element type");
                return false;
            }
            count = array_obj->count;
            if (count > 0) {
                items = (TabloExtValue*)safe_calloc((size_t)count, sizeof(TabloExtValue));
                for (int i = 0; i < count; i++) {
                    Value element_value;
                    obj_array_get(array_obj, i, &element_value);
                    if (!native_extension_convert_value_to_ext_value(state,
                                                                     &element_value,
                                                                     expected_type->element_type,
                                                                     &items[i])) {
                        free(items);
                        return false;
                    }
                }
            }
            if (!native_extension_call_state_push_temp_array(state, items, count)) {
                free(items);
                return false;
            }
            out_value->as.array_value.items = items;
            out_value->as.array_value.count = count;
            return native_extension_call_state_push_temp_value(state, value);
        }
        case TABLO_EXT_TYPE_TUPLE: {
            ObjTuple* tuple = NULL;
            TabloExtValue* items = NULL;
            int count = 0;
            if (value_get_type(value) != VAL_TUPLE || !(tuple = value_get_tuple_obj(value))) {
                native_extension_set_runtime_error_message(state, "TabloLang callback result expected a tuple");
                return false;
            }
            count = tuple->element_count;
            if (count != expected_type->tuple_element_count) {
                native_extension_set_runtime_error_message(state, "TabloLang callback result tuple arity mismatch");
                return false;
            }
            if (count > 0) {
                items = (TabloExtValue*)safe_calloc((size_t)count, sizeof(TabloExtValue));
                for (int i = 0; i < count; i++) {
                    Value element_value;
                    obj_tuple_get(tuple, i, &element_value);
                    if (!native_extension_convert_value_to_ext_value(state,
                                                                     &element_value,
                                                                     &expected_type->tuple_element_types[i],
                                                                     &items[i])) {
                        free(items);
                        return false;
                    }
                }
            }
            if (!native_extension_call_state_push_temp_array(state, items, count)) {
                free(items);
                return false;
            }
            out_value->as.tuple_value.items = items;
            out_value->as.tuple_value.count = count;
            return native_extension_call_state_push_temp_value(state, value);
        }
        case TABLO_EXT_TYPE_MAP: {
            ObjMap* map_obj = NULL;
            TabloExtMapEntry* entries = NULL;
            int entry_index = 0;
            if (value_get_type(value) != VAL_MAP || !(map_obj = value_get_map_obj(value))) {
                native_extension_set_runtime_error_message(state, "TabloLang callback result expected map<string, any>");
                return false;
            }
            if (map_obj->count > 0) {
                entries = (TabloExtMapEntry*)safe_calloc((size_t)map_obj->count, sizeof(TabloExtMapEntry));
                for (int i = 0; i < map_obj->capacity; i++) {
                    MapSlot* map_slot = &map_obj->slots[i];
                    ObjString* key = NULL;
                    if (map_slot->hash < 2) continue;
                    key = value_get_string_obj(&map_slot->key);
                    if (value_get_type(&map_slot->key) != VAL_STRING || !key || !key->chars) {
                        free(entries);
                        native_extension_set_runtime_error_message(state, "TabloLang callback map results currently require string keys");
                        return false;
                    }
                    entries[entry_index].key_chars = key->chars;
                    entries[entry_index].key_length = key->length;
                    if (!native_extension_convert_value_to_dynamic_map_value(state,
                                                                            &map_slot->value,
                                                                            &entries[entry_index].value)) {
                        for (int j = 0; j < entry_index; j++) {
                            native_extension_temp_map_value_free(&entries[j].value);
                        }
                        free(entries);
                        return false;
                    }
                    entry_index++;
                }
            }
            if (!native_extension_call_state_push_temp_map(state, entries, entry_index)) {
                for (int j = 0; j < entry_index; j++) {
                    native_extension_temp_map_value_free(&entries[j].value);
                }
                free(entries);
                return false;
            }
            out_value->as.map_value.entries = entries;
            out_value->as.map_value.count = entry_index;
            return native_extension_call_state_push_temp_value(state, value);
        }
        default:
            native_extension_set_runtime_error_message(state, "TabloLang callback result uses an unsupported type");
            return false;
    }
}

static int native_extension_api_arg_count(TabloExtCallContext* ctx) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    return state ? state->arg_count : 0;
}

static bool native_extension_api_arg_is_nil(TabloExtCallContext* ctx, int index) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    Value* slot = native_extension_arg_slot(state, index);
    return slot && value_get_type(slot) == VAL_NIL;
}

static bool native_extension_api_get_int_arg(TabloExtCallContext* ctx, int index, int64_t* out_value) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    if (!out_value || !native_extension_validate_arg_tag(state, index, TABLO_EXT_TYPE_INT, "int")) {
        return false;
    }
    Value* slot = native_extension_arg_slot(state, index);
    if (!slot || value_get_type(slot) != VAL_INT) {
        native_extension_set_runtime_error_message(state, "Extension argument expected int");
        return false;
    }
    *out_value = value_get_int(slot);
    return true;
}

static bool native_extension_api_get_bool_arg(TabloExtCallContext* ctx, int index, bool* out_value) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    if (!out_value || !native_extension_validate_arg_tag(state, index, TABLO_EXT_TYPE_BOOL, "bool")) {
        return false;
    }
    Value* slot = native_extension_arg_slot(state, index);
    if (!slot || value_get_type(slot) != VAL_BOOL) {
        native_extension_set_runtime_error_message(state, "Extension argument expected bool");
        return false;
    }
    *out_value = value_get_bool(slot);
    return true;
}

static bool native_extension_api_get_double_arg(TabloExtCallContext* ctx, int index, double* out_value) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    if (!out_value || !native_extension_validate_arg_tag(state, index, TABLO_EXT_TYPE_DOUBLE, "double")) {
        return false;
    }
    Value* slot = native_extension_arg_slot(state, index);
    if (!slot || value_get_type(slot) != VAL_DOUBLE) {
        native_extension_set_runtime_error_message(state, "Extension argument expected double");
        return false;
    }
    *out_value = value_get_double(slot);
    return true;
}

static bool native_extension_api_get_string_arg(TabloExtCallContext* ctx,
                                                int index,
                                                const char** out_chars,
                                                int* out_length) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    if (!out_chars || !out_length || !native_extension_validate_arg_tag(state, index, TABLO_EXT_TYPE_STRING, "string")) {
        return false;
    }
    Value* slot = native_extension_arg_slot(state, index);
    if (!slot || value_get_type(slot) != VAL_STRING || !value_get_string_obj(slot)) {
        native_extension_set_runtime_error_message(state, "Extension argument expected string");
        return false;
    }
    ObjString* str = value_get_string_obj(slot);
    *out_chars = str->chars;
    *out_length = str->length;
    return true;
}

static bool native_extension_api_get_bytes_arg(TabloExtCallContext* ctx,
                                               int index,
                                               const uint8_t** out_bytes,
                                               int* out_length) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    if (!out_bytes || !out_length || !native_extension_validate_arg_tag(state, index, TABLO_EXT_TYPE_BYTES, "bytes")) {
        return false;
    }
    Value* slot = native_extension_arg_slot(state, index);
    if (!slot || value_get_type(slot) != VAL_BYTES || !value_get_bytes_obj(slot)) {
        native_extension_set_runtime_error_message(state, "Extension argument expected bytes");
        return false;
    }
    ObjBytes* bytes = value_get_bytes_obj(slot);
    *out_bytes = obj_bytes_data(bytes);
    *out_length = bytes ? bytes->length : 0;
    return true;
}

static bool native_extension_api_get_handle_arg(TabloExtCallContext* ctx,
                                                int index,
                                                const char* expected_type_name,
                                                void** out_payload) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    if (!out_payload || !native_extension_validate_arg_tag(state, index, TABLO_EXT_TYPE_HANDLE, "handle")) {
        return false;
    }

    const TabloExtTypeDesc* param_type = native_extension_expected_param_type(state, index);
    if (!param_type || !param_type->handle_type_name) {
        native_extension_set_runtime_error_message(state, "Extension handle parameter is missing a handle type");
        return false;
    }
    if (expected_type_name && expected_type_name[0] != '\0' &&
        strcmp(expected_type_name, param_type->handle_type_name) != 0) {
        native_extension_set_runtime_error_message(state, "Extension requested a handle type that does not match the function signature");
        return false;
    }

    Value* slot = native_extension_arg_slot(state, index);
    if (!slot || value_get_type(slot) != VAL_RECORD || !value_get_record_obj(slot)) {
        native_extension_set_runtime_error_message(state, "Extension argument expected an opaque handle");
        return false;
    }

    ObjRecord* record = value_get_record_obj(slot);
    if (!record->is_native_opaque ||
        !record->type_name ||
        strcmp(record->type_name, param_type->handle_type_name) != 0) {
        native_extension_set_runtime_error_message(state, "Extension handle argument type mismatch");
        return false;
    }

    *out_payload = record->native_opaque_payload;
    return true;
}

static bool native_extension_api_get_array_arg(TabloExtCallContext* ctx,
                                               int index,
                                               const TabloExtValue** out_items,
                                               int* out_count) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    const TabloExtTypeDesc* param_type = native_extension_expected_param_type(state, index);
    Value* slot = NULL;
    ObjArray* array_obj = NULL;
    TabloExtValue* items = NULL;

    if (!out_items || !out_count || !native_extension_validate_arg_tag(state, index, TABLO_EXT_TYPE_ARRAY, "array")) {
        return false;
    }
    if (!param_type || !param_type->element_type) {
        native_extension_set_runtime_error_message(state, "Extension array parameter is missing an element type");
        return false;
    }

    slot = native_extension_arg_slot(state, index);
    if (!slot || value_get_type(slot) != VAL_ARRAY) {
        native_extension_set_runtime_error_message(state, "Extension argument expected array");
        return false;
    }

    array_obj = value_get_array_obj(slot);
    if (!array_obj) {
        native_extension_set_runtime_error_message(state, "Extension argument expected array");
        return false;
    }

    if (array_obj->count > 0) {
        items = (TabloExtValue*)safe_calloc((size_t)array_obj->count, sizeof(TabloExtValue));
        for (int i = 0; i < array_obj->count; i++) {
            Value element_value;
            obj_array_get(array_obj, i, &element_value);
            if (!native_extension_convert_value_to_ext_value(state,
                                                             &element_value,
                                                             param_type->element_type,
                                                             &items[i])) {
                free(items);
                return false;
            }
        }
    }

    if (!native_extension_call_state_push_temp_array(state, items, array_obj->count)) {
        native_extension_set_runtime_error_message(state, "Failed to store temporary extension array data");
        return false;
    }

    *out_items = items;
    *out_count = array_obj->count;
    return true;
}

static bool native_extension_api_get_tuple_arg(TabloExtCallContext* ctx,
                                               int index,
                                               const TabloExtValue** out_items,
                                               int* out_count) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    const TabloExtTypeDesc* param_type = native_extension_expected_param_type(state, index);
    Value* slot = NULL;
    ObjTuple* tuple_obj = NULL;
    TabloExtValue* items = NULL;

    if (!out_items || !out_count || !native_extension_validate_arg_tag(state, index, TABLO_EXT_TYPE_TUPLE, "tuple")) {
        return false;
    }
    if (!param_type) {
        native_extension_set_runtime_error_message(state, "Extension tuple parameter is not initialized");
        return false;
    }

    slot = native_extension_arg_slot(state, index);
    if (!slot || value_get_type(slot) != VAL_TUPLE) {
        native_extension_set_runtime_error_message(state, "Extension argument expected tuple");
        return false;
    }

    tuple_obj = value_get_tuple_obj(slot);
    if (!tuple_obj) {
        native_extension_set_runtime_error_message(state, "Extension argument expected tuple");
        return false;
    }
    if (tuple_obj->element_count != param_type->tuple_element_count) {
        native_extension_set_runtime_error_message(state, "Extension tuple argument arity mismatch");
        return false;
    }

    if (tuple_obj->element_count > 0) {
        items = (TabloExtValue*)safe_calloc((size_t)tuple_obj->element_count, sizeof(TabloExtValue));
        for (int i = 0; i < tuple_obj->element_count; i++) {
            Value element_value;
            obj_tuple_get(tuple_obj, i, &element_value);
            if (!native_extension_convert_value_to_ext_value(state,
                                                             &element_value,
                                                             &param_type->tuple_element_types[i],
                                                             &items[i])) {
                free(items);
                return false;
            }
        }
    }

    if (!native_extension_call_state_push_temp_array(state, items, tuple_obj->element_count)) {
        native_extension_set_runtime_error_message(state, "Failed to store temporary extension tuple data");
        return false;
    }

    *out_items = items;
    *out_count = tuple_obj->element_count;
    return true;
}

static bool native_extension_api_get_map_arg(TabloExtCallContext* ctx,
                                             int index,
                                             const TabloExtMapEntry** out_entries,
                                             int* out_count) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    Value* slot = NULL;
    ObjMap* map_obj = NULL;
    TabloExtMapEntry* entries = NULL;
    int entry_index = 0;

    if (!out_entries || !out_count || !native_extension_validate_arg_tag(state, index, TABLO_EXT_TYPE_MAP, "map")) {
        return false;
    }

    slot = native_extension_arg_slot(state, index);
    if (!slot || value_get_type(slot) != VAL_MAP) {
        native_extension_set_runtime_error_message(state, "Extension argument expected map<string, any>");
        return false;
    }

    map_obj = value_get_map_obj(slot);
    if (!map_obj) {
        native_extension_set_runtime_error_message(state, "Extension argument expected map<string, any>");
        return false;
    }

    if (map_obj->count > 0) {
        entries = (TabloExtMapEntry*)safe_calloc((size_t)map_obj->count, sizeof(TabloExtMapEntry));
        for (int i = 0; i < map_obj->capacity; i++) {
            MapSlot* map_slot = &map_obj->slots[i];
            ObjString* key = NULL;
            if (map_slot->hash < 2) continue;
            key = value_get_string_obj(&map_slot->key);
            if (value_get_type(&map_slot->key) != VAL_STRING || !key || !key->chars) {
                free(entries);
                native_extension_set_runtime_error_message(state, "Extension maps currently require string keys");
                return false;
            }
            entries[entry_index].key_chars = key->chars;
            entries[entry_index].key_length = key->length;
            if (!native_extension_convert_value_to_dynamic_map_value(state,
                                                                    &map_slot->value,
                                                                    &entries[entry_index].value)) {
                for (int j = 0; j < entry_index; j++) {
                    native_extension_temp_map_value_free(&entries[j].value);
                }
                free(entries);
                return false;
            }
            entry_index++;
        }
    }

    if (!native_extension_call_state_push_temp_map(state, entries, entry_index)) {
        native_extension_set_runtime_error_message(state, "Failed to store temporary extension map data");
        return false;
    }

    *out_entries = entries;
    *out_count = entry_index;
    return true;
}

static bool native_extension_api_get_callback_arg(TabloExtCallContext* ctx,
                                                  int index,
                                                  const TabloExtCallback** out_callback) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    const TabloExtTypeDesc* param_type = native_extension_expected_param_type(state, index);
    Value* slot = NULL;
    TabloExtCallback* callback = NULL;

    if (!out_callback || !native_extension_validate_arg_tag(state, index, TABLO_EXT_TYPE_CALLBACK, "callback")) {
        return false;
    }
    *out_callback = NULL;
    if (!param_type) {
        native_extension_set_runtime_error_message(state, "Extension callback parameter is not initialized");
        return false;
    }

    slot = native_extension_arg_slot(state, index);
    if (!slot) {
        native_extension_set_runtime_error_message(state, "Extension callback argument is missing");
        return false;
    }
    if (value_get_type(slot) == VAL_NIL) {
        if (!param_type->nullable) {
            native_extension_set_runtime_error_message(state, "Extension callback argument cannot be nil");
            return false;
        }
        return true;
    }
    if (value_get_type(slot) != VAL_FUNCTION && value_get_type(slot) != VAL_NATIVE) {
        native_extension_set_runtime_error_message(state, "Extension argument expected callback");
        return false;
    }

    callback = native_extension_callback_create(state->vm, slot, param_type);
    if (!callback) {
        native_extension_set_runtime_error_message(state, "Failed to capture extension callback argument");
        return false;
    }
    if (!native_extension_call_state_push_temp_callback(state, callback)) {
        native_extension_set_runtime_error_message(state, "Failed to retain temporary extension callback");
        return false;
    }

    *out_callback = callback;
    return true;
}

static bool native_extension_api_invoke_callback(TabloExtCallContext* ctx,
                                                 const TabloExtCallback* callback,
                                                 const TabloExtValue* args,
                                                 int arg_count,
                                                 TabloExtValue* out_result) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    Value* arg_values = NULL;
    Value result_value;
    int converted_count = 0;

    if (!state || !out_result) return false;
    if (!callback) {
        native_extension_set_runtime_error_message(state, "Extension attempted to invoke a nil callback");
        return false;
    }
    if (arg_count < 0) {
        native_extension_set_runtime_error_message(state, "Extension callback argument count is invalid");
        return false;
    }
    if (arg_count > 0 && !args) {
        native_extension_set_runtime_error_message(state, "Extension callback arguments are not initialized");
        return false;
    }
    if (callback->vm != state->vm) {
        native_extension_set_runtime_error_message(state, "Extension callback belongs to a different VM instance");
        return false;
    }
    if (callback->type.callback_param_count != arg_count) {
        native_extension_set_runtime_error_message(state, "Extension callback arity does not match the declared callback type");
        return false;
    }

    if (arg_count > 0) {
        arg_values = (Value*)safe_calloc((size_t)arg_count, sizeof(Value));
        for (int i = 0; i < arg_count; i++) {
            if (!native_extension_convert_callback_arg_to_value(state,
                                                                &args[i],
                                                                &callback->type.callback_param_types[i],
                                                                &arg_values[i])) {
                goto cleanup;
            }
            converted_count++;
        }
    }

    value_init_nil(&result_value);
    if (vm_call_value_sync(state->vm, &callback->callable, arg_values, arg_count, &result_value) != 0) {
        goto cleanup;
    }

    if (!native_extension_convert_value_to_callback_result(state,
                                                           &result_value,
                                                           callback->type.callback_result_type,
                                                           out_result)) {
        value_free(&result_value);
        goto cleanup;
    }

    if (arg_values) {
        for (int i = 0; i < converted_count; i++) {
            value_free(&arg_values[i]);
        }
        free(arg_values);
    }
    return true;

cleanup:
    if (arg_values) {
        for (int i = 0; i < converted_count; i++) {
            value_free(&arg_values[i]);
        }
        free(arg_values);
    }
    return false;
}

static bool native_extension_post_callback_impl(const TabloExtCallback* callback,
                                                const TabloExtValue* args,
                                                int arg_count,
                                                char* error_buf,
                                                size_t error_buf_size) {
    return native_extension_post_callback_gated_impl(callback, args, arg_count, NULL, 0, error_buf, error_buf_size);
}

static bool native_extension_post_callback_gated_impl(const TabloExtCallback* callback,
                                                      const TabloExtValue* args,
                                                      int arg_count,
                                                      const TabloExtCallbackGate* gate,
                                                      uint64_t gate_generation,
                                                      char* error_buf,
                                                      size_t error_buf_size) {
    NativeExtensionPostedCallbackEvent* event = NULL;

    if (!callback) {
        native_extension_set_error(error_buf, error_buf_size, "Queued extension callback is not initialized");
        return false;
    }
    if (!callback->posted_event_queue) {
        native_extension_set_error(error_buf, error_buf_size, "Queued extension callback is not attached to a VM event queue");
        return false;
    }
    if (arg_count < 0) {
        native_extension_set_error(error_buf, error_buf_size, "Queued extension callback argument count is invalid");
        return false;
    }
    if (callback->type.callback_param_count != arg_count) {
        native_extension_set_error(error_buf,
                                   error_buf_size,
                                   "Queued extension callback arity does not match the callback signature");
        return false;
    }
    if (arg_count > 0 && !args) {
        native_extension_set_error(error_buf, error_buf_size, "Queued extension callback arguments are not initialized");
        return false;
    }

    event = (NativeExtensionPostedCallbackEvent*)calloc(1, sizeof(NativeExtensionPostedCallbackEvent));
    if (!event) {
        native_extension_set_error(error_buf, error_buf_size, "Out of memory while queuing extension callback");
        return false;
    }

    event->arg_count = arg_count;
    if (arg_count > 0) {
        event->args = (TabloExtValue*)calloc((size_t)arg_count, sizeof(TabloExtValue));
        if (!event->args) {
            native_extension_set_error(error_buf, error_buf_size, "Out of memory while cloning queued extension callback arguments");
            native_extension_posted_callback_event_free(event);
            return false;
        }
        for (int i = 0; i < arg_count; i++) {
            if (!native_extension_clone_posted_callback_value(&callback->type.callback_param_types[i],
                                                              &args[i],
                                                              &event->args[i],
                                                              error_buf,
                                                              error_buf_size)) {
                native_extension_posted_callback_event_free(event);
                return false;
            }
        }
    }

    event->callback = (TabloExtCallback*)callback;
    native_extension_callback_retain_impl(callback);
    native_extension_callback_increment_pending_impl(callback);
    event->counted_pending = true;
    if (gate) {
        event->gate = (TabloExtCallbackGate*)gate;
        event->gate_generation = gate_generation;
        native_extension_callback_gate_retain_impl(event->gate);
    }
    if (!vm_posted_event_queue_enqueue(callback->posted_event_queue,
                                       native_extension_dispatch_posted_callback_event,
                                       native_extension_posted_callback_event_free,
                                       event)) {
        native_extension_set_error(error_buf, error_buf_size, "Extension callback queue is closed");
        native_extension_callback_decrement_pending_impl(callback);
        event->counted_pending = false;
        native_extension_posted_callback_event_free(event);
        return false;
    }

    return true;
}

static bool native_extension_api_set_nil_result(TabloExtCallContext* ctx) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    const TabloExtTypeDesc* result_type = native_extension_expected_result_type(state);
    if (!result_type) {
        native_extension_set_runtime_error_message(state, "Extension result type is not initialized");
        return false;
    }
    if (result_type->tag != TABLO_EXT_TYPE_VOID && !result_type->nullable) {
        native_extension_set_runtime_error_message(state, "Extension cannot return nil for a non-nullable result");
        return false;
    }
    Value result;
    value_init_nil(&result);
    return native_extension_write_result_value(state, &result);
}

static bool native_extension_api_set_int_result(TabloExtCallContext* ctx, int64_t value) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    const TabloExtTypeDesc* result_type = native_extension_expected_result_type(state);
    if (!result_type || result_type->tag != TABLO_EXT_TYPE_INT) {
        native_extension_set_runtime_error_message(state, "Extension result expected int");
        return false;
    }
    Value result;
    value_init_int(&result, value);
    return native_extension_write_result_value(state, &result);
}

static bool native_extension_api_set_bool_result(TabloExtCallContext* ctx, bool value) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    const TabloExtTypeDesc* result_type = native_extension_expected_result_type(state);
    if (!result_type || result_type->tag != TABLO_EXT_TYPE_BOOL) {
        native_extension_set_runtime_error_message(state, "Extension result expected bool");
        return false;
    }
    Value result;
    value_init_bool(&result, value);
    return native_extension_write_result_value(state, &result);
}

static bool native_extension_api_set_double_result(TabloExtCallContext* ctx, double value) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    const TabloExtTypeDesc* result_type = native_extension_expected_result_type(state);
    if (!result_type || result_type->tag != TABLO_EXT_TYPE_DOUBLE) {
        native_extension_set_runtime_error_message(state, "Extension result expected double");
        return false;
    }
    Value result;
    value_init_double(&result, value);
    return native_extension_write_result_value(state, &result);
}

static bool native_extension_api_set_string_result(TabloExtCallContext* ctx, const char* chars, int length) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    const TabloExtTypeDesc* result_type = native_extension_expected_result_type(state);
    if (!result_type || result_type->tag != TABLO_EXT_TYPE_STRING) {
        native_extension_set_runtime_error_message(state, "Extension result expected string");
        return false;
    }
    if (length < 0) {
        if (!chars) {
            native_extension_set_runtime_error_message(state, "Extension string result length is invalid");
            return false;
        }
        length = (int)strlen(chars);
    }
    Value result;
    value_init_string_n(&result, chars ? chars : "", length);
    return native_extension_write_result_value(state, &result);
}

static bool native_extension_api_set_bytes_result(TabloExtCallContext* ctx, const uint8_t* bytes, int length) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    const TabloExtTypeDesc* result_type = native_extension_expected_result_type(state);
    if (!result_type || result_type->tag != TABLO_EXT_TYPE_BYTES) {
        native_extension_set_runtime_error_message(state, "Extension result expected bytes");
        return false;
    }
    if (length < 0) {
        native_extension_set_runtime_error_message(state, "Extension bytes result length is invalid");
        return false;
    }
    ObjBytes* bytes_obj = obj_bytes_create_copy(bytes ? bytes : (const uint8_t*)"", length);
    Value result;
    value_init_bytes(&result, bytes_obj);
    return native_extension_write_result_value(state, &result);
}

static bool native_extension_api_set_handle_result(TabloExtCallContext* ctx, const char* type_name, void* payload) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    const TabloExtTypeDesc* result_type = native_extension_expected_result_type(state);
    if (!result_type || result_type->tag != TABLO_EXT_TYPE_HANDLE) {
        native_extension_set_runtime_error_message(state, "Extension result expected an opaque handle");
        return false;
    }
    if (!result_type->handle_type_name) {
        native_extension_set_runtime_error_message(state, "Extension handle result type is missing a handle name");
        return false;
    }
    if (type_name && type_name[0] != '\0' &&
        strcmp(type_name, result_type->handle_type_name) != 0) {
        native_extension_set_runtime_error_message(state, "Extension returned a handle type that does not match the function signature");
        return false;
    }
    if (!payload) {
        if (result_type->nullable) {
            return native_extension_api_set_nil_result(ctx);
        }
        native_extension_set_runtime_error_message(state, "Extension cannot return a null handle payload");
        return false;
    }

    const NativeExtensionHandleType* handle_type =
        native_extension_lookup_handle_type(state ? state->registry : NULL, NULL, result_type->handle_type_name);
    if (!handle_type) {
        native_extension_set_runtime_error_message(state, "Extension returned an unknown handle type");
        return false;
    }

    ObjRecord* record = obj_record_create_opaque(state->vm,
                                                 result_type->handle_type_name,
                                                 payload,
                                                 handle_type->destroy);
    Value result;
    value_init_record(&result, record);
    return native_extension_write_result_value(state, &result);
}

static bool native_extension_api_set_array_result(TabloExtCallContext* ctx,
                                                  const TabloExtValue* items,
                                                  int count) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    const TabloExtTypeDesc* result_type = native_extension_expected_result_type(state);
    ObjArray* array_obj = NULL;

    if (!result_type || result_type->tag != TABLO_EXT_TYPE_ARRAY) {
        native_extension_set_runtime_error_message(state, "Extension result expected array");
        return false;
    }
    if (!result_type->element_type) {
        native_extension_set_runtime_error_message(state, "Extension array result is missing an element type");
        return false;
    }
    if (count < 0) {
        native_extension_set_runtime_error_message(state, "Extension array result length is invalid");
        return false;
    }
    if (count > 0 && !items) {
        native_extension_set_runtime_error_message(state, "Extension array result items are not initialized");
        return false;
    }

    array_obj = obj_array_create(state->vm, count > 0 ? count : 4);
    for (int i = 0; i < count; i++) {
        Value element_value;
        if (!native_extension_convert_ext_value_to_value(state, &items[i], result_type->element_type, &element_value)) {
            obj_array_release(array_obj);
            return false;
        }
        obj_array_push(array_obj, element_value);
    }

    {
        Value result;
        value_init_array(&result, array_obj);
        return native_extension_write_result_value(state, &result);
    }
}

static bool native_extension_api_set_tuple_result(TabloExtCallContext* ctx,
                                                  const TabloExtValue* items,
                                                  int count) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    const TabloExtTypeDesc* result_type = native_extension_expected_result_type(state);
    ObjTuple* tuple_obj = NULL;

    if (!result_type || result_type->tag != TABLO_EXT_TYPE_TUPLE) {
        native_extension_set_runtime_error_message(state, "Extension result expected tuple");
        return false;
    }
    if (count != result_type->tuple_element_count) {
        native_extension_set_runtime_error_message(state, "Extension tuple result arity does not match the declared result type");
        return false;
    }
    if (count > 0 && !items) {
        native_extension_set_runtime_error_message(state, "Extension tuple result items are not initialized");
        return false;
    }

    tuple_obj = obj_tuple_create(state->vm, count);
    for (int i = 0; i < count; i++) {
        Value element_value;
        if (!native_extension_convert_ext_value_to_value(state,
                                                         &items[i],
                                                         &result_type->tuple_element_types[i],
                                                         &element_value)) {
            obj_tuple_release(tuple_obj);
            return false;
        }
        obj_tuple_set(tuple_obj, i, element_value);
    }

    {
        Value result;
        value_init_tuple(&result, tuple_obj);
        return native_extension_write_result_value(state, &result);
    }
}

static bool native_extension_api_set_map_result(TabloExtCallContext* ctx,
                                                const TabloExtMapEntry* entries,
                                                int count) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    const TabloExtTypeDesc* result_type = native_extension_expected_result_type(state);
    ObjMap* map_obj = NULL;

    if (!result_type || result_type->tag != TABLO_EXT_TYPE_MAP) {
        native_extension_set_runtime_error_message(state, "Extension result expected map<string, any>");
        return false;
    }
    if (count < 0) {
        native_extension_set_runtime_error_message(state, "Extension map result entry count is invalid");
        return false;
    }
    if (count > 0 && !entries) {
        native_extension_set_runtime_error_message(state, "Extension map result entries are not initialized");
        return false;
    }

    map_obj = obj_map_create(state->vm);
    for (int i = 0; i < count; i++) {
        Value entry_value;
        int key_length = entries[i].key_length;
        if (!entries[i].key_chars) {
            obj_map_release(map_obj);
            native_extension_set_runtime_error_message(state, "Extension map result key is not initialized");
            return false;
        }
        if (key_length < 0) {
            key_length = (int)strlen(entries[i].key_chars);
        }
        if (!native_extension_convert_dynamic_map_value_to_value(state,
                                                                 &entries[i].value,
                                                                 true,
                                                                 &entry_value)) {
            obj_map_release(map_obj);
            return false;
        }
        obj_map_set_cstr_n(map_obj, entries[i].key_chars, key_length, entry_value);
        value_free(&entry_value);
    }

    {
        Value result;
        value_init_map(&result, map_obj);
        return native_extension_write_result_value(state, &result);
    }
}

static void native_extension_api_set_runtime_error(TabloExtCallContext* ctx, const char* message) {
    NativeExtensionCallState* state = native_extension_call_state(ctx);
    native_extension_set_runtime_error_message(state, message ? message : "Extension runtime error");
}

static const TabloExtApi NATIVE_EXTENSION_API = {
    TABLO_EXT_ABI_VERSION,
    native_extension_api_arg_count,
    native_extension_api_arg_is_nil,
    native_extension_api_get_int_arg,
    native_extension_api_get_bool_arg,
    native_extension_api_get_double_arg,
    native_extension_api_get_string_arg,
    native_extension_api_get_bytes_arg,
    native_extension_api_get_handle_arg,
    native_extension_api_get_array_arg,
    native_extension_api_get_tuple_arg,
    native_extension_api_get_map_arg,
    native_extension_api_get_callback_arg,
    native_extension_api_set_nil_result,
    native_extension_api_set_int_result,
    native_extension_api_set_bool_result,
    native_extension_api_set_double_result,
    native_extension_api_set_string_result,
    native_extension_api_set_bytes_result,
    native_extension_api_set_handle_result,
    native_extension_api_set_array_result,
    native_extension_api_set_tuple_result,
    native_extension_api_set_map_result,
    native_extension_api_invoke_callback,
    native_extension_api_set_runtime_error
};

int native_extension_invoke_function(void* function_userdata, VM* vm) {
    NativeExtensionFunction* function = (NativeExtensionFunction*)function_userdata;
    int status = -1;
    if (!function || !vm) {
        if (vm) {
            vm_runtime_error(vm, "Invalid extension native function");
        }
        return -1;
    }

    int arg_count = function->param_count;
    int result_slot = vm->stack.count - (arg_count > 0 ? arg_count : 1);
    if (result_slot < 0 || result_slot >= vm->stack.count) {
        vm_runtime_error(vm, "Extension call stack layout is invalid");
        return -1;
    }

    NativeExtensionCallState state;
    state.vm = vm;
    state.registry = vm->extension_registry;
    state.function = function;
    state.result_slot = result_slot;
    state.arg_count = arg_count;
    state.result_set = false;
    state.temp_arrays = NULL;
    state.temp_array_count = 0;
    state.temp_array_capacity = 0;
    state.temp_maps = NULL;
    state.temp_map_count = 0;
    state.temp_map_capacity = 0;
    state.temp_callbacks = NULL;
    state.temp_callback_count = 0;
    state.temp_callback_capacity = 0;
    state.temp_values = NULL;
    state.temp_value_count = 0;
    state.temp_value_capacity = 0;

    TabloExtCallContext ctx;
    ctx.api = &NATIVE_EXTENSION_API;
    ctx.host_context = &state;

    bool ok = function->callback(&ctx);
    if (!ok) {
        if (!vm_has_error(vm)) {
            vm_runtime_error(vm, "Extension callback failed");
        }
        goto cleanup;
    }

    if (!state.result_set) {
        if (function->result_type.tag == TABLO_EXT_TYPE_VOID) {
            if (!native_extension_api_set_nil_result(&ctx)) {
                goto cleanup;
            }
        } else {
            vm_runtime_error(vm, "Extension callback returned without setting a result");
            goto cleanup;
        }
    }

    status = vm_has_error(vm) ? -1 : 0;

cleanup:
    native_extension_call_state_free_temp_arrays(&state);
    native_extension_call_state_free_temp_maps(&state);
    native_extension_call_state_free_temp_callbacks(&state);
    native_extension_call_state_free_temp_values(&state);
    return status;
}
