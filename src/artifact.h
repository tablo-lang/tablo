#ifndef ARTIFACT_H
#define ARTIFACT_H

#include "vm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char* path;
    int64_t mtime;
} ArtifactDependencyInfo;

typedef struct {
    char* path;
    int64_t mtime;
} LoadedArtifactDependency;

typedef struct {
    ObjFunction* init_function;
    ObjFunction** functions;
    int function_count;
    int main_index;
    uint32_t typecheck_flags;
    LoadedArtifactDependency* dependencies;
    int dependency_count;
    InterfaceDispatchEntry* interface_dispatch_entries;
    int interface_dispatch_count;
} LoadedBytecodeArtifact;

bool artifact_file_is_bytecode(const char* path);

bool artifact_write_file(const char* path,
                         ObjFunction* init_function,
                         ObjFunction** functions,
                         int function_count,
                         int main_index,
                         uint32_t typecheck_flags,
                         const ArtifactDependencyInfo* dependencies,
                         int dependency_count,
                         const InterfaceDispatchEntry* interface_dispatch_entries,
                         int interface_dispatch_count,
                         char* error_buf,
                         size_t error_buf_size);

bool artifact_load_file(const char* path,
                        LoadedBytecodeArtifact* out,
                        char* error_buf,
                        size_t error_buf_size);
bool artifact_load_bytes(const uint8_t* data,
                         size_t size,
                         LoadedBytecodeArtifact* out,
                         char* error_buf,
                         size_t error_buf_size);

void artifact_loaded_free(LoadedBytecodeArtifact* artifact);

#endif
