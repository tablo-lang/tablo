#ifndef NATIVE_EXTENSION_H
#define NATIVE_EXTENSION_H

#include "tablo_ext.h"
#include "vm.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct NativeExtensionRegistry NativeExtensionRegistry;

NativeExtensionRegistry* native_extension_registry_create(void);
void native_extension_registry_shutdown(NativeExtensionRegistry* registry);
void native_extension_registry_free(NativeExtensionRegistry* registry);

bool native_extension_registry_load_paths(NativeExtensionRegistry* registry,
                                          const char* const* paths,
                                          int path_count,
                                          char* error_buf,
                                          size_t error_buf_size);

bool native_extension_registry_has_extensions(const NativeExtensionRegistry* registry);

int native_extension_registry_handle_type_count(const NativeExtensionRegistry* registry);
const char* native_extension_registry_handle_type_name(const NativeExtensionRegistry* registry, int index);

int native_extension_registry_function_count(const NativeExtensionRegistry* registry);
const char* native_extension_registry_function_name(const NativeExtensionRegistry* registry, int index);
TabloExtTypeDesc native_extension_registry_function_result_type(const NativeExtensionRegistry* registry, int index);
int native_extension_registry_function_param_count(const NativeExtensionRegistry* registry, int index);
TabloExtTypeDesc native_extension_registry_function_param_type(const NativeExtensionRegistry* registry,
                                                            int function_index,
                                                            int param_index);

bool native_extension_registry_register_vm_globals(NativeExtensionRegistry* registry,
                                                   VM* vm,
                                                   char* error_buf,
                                                   size_t error_buf_size);

int native_extension_invoke_function(void* function_userdata, VM* vm);

#endif
