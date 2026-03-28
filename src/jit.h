#ifndef JIT_H
#define JIT_H

#include "vm.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define JIT_DEFAULT_HOT_THRESHOLD UINT64_C(100)

typedef struct {
    ObjFunction* function;
    uint64_t entry_count;
    bool hot;
    JitFunctionState state;
    JitFunctionReason reason;
    uint32_t compile_attempts;
    uint64_t compiled_calls;
} JitProfileEntry;

void jit_vm_init(VM* vm);
void jit_vm_free(VM* vm);
void jit_set_profile_enabled(VM* vm, bool enabled);
bool jit_is_profile_enabled(const VM* vm);
void jit_set_auto_compile_enabled(VM* vm, bool enabled);
bool jit_is_auto_compile_enabled(const VM* vm);
void jit_set_hot_threshold(VM* vm, uint64_t threshold);
uint64_t jit_get_hot_threshold(const VM* vm);
const char* jit_function_state_name(JitFunctionState state);
const char* jit_function_reason_name(JitFunctionReason reason);
void jit_record_function_entry(VM* vm, ObjFunction* function);
bool jit_function_has_compiled_entry(const ObjFunction* function);
int jit_drain_work_queue(VM* vm, int max_items);
int jit_collect_profile_entries(ObjFunction* init_function,
                                ObjFunction* main_function,
                                ObjFunction** functions,
                                int function_count,
                                JitProfileEntry** out_entries);
void jit_free_profile_entries(JitProfileEntry* entries);
void jit_dump_profile(FILE* out,
                      const VM* vm,
                      ObjFunction* init_function,
                      ObjFunction* main_function,
                      ObjFunction** functions,
                      int function_count);
int jit_get_work_queue_count(const VM* vm);
ObjFunction* jit_get_work_queue_function(const VM* vm, int index);
void jit_dump_work_queue(FILE* out, const VM* vm);

#endif
