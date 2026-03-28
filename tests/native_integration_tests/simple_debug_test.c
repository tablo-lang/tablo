#include "../src/runtime.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("DEBUG: Starting integration test...\n");
    fflush(stdout);
    
    printf("DEBUG: About to create runtime...\n");
    fflush(stdout);
    
    Runtime* rt = runtime_create("../tests/basic_test.tblo");
    
    printf("DEBUG: Runtime created: %p\n", (void*)rt);
    fflush(stdout);
    
    if (!rt) {
        printf("ERROR: Runtime is NULL\n");
        return 1;
    }
    
    printf("DEBUG: Checking for errors...\n");
    fflush(stdout);
    
    if (runtime_has_error(rt)) {
        const char* error = runtime_get_error(rt);
        printf("ERROR: Runtime error: %s\n", error ? error : "(null)");
        printf("ERROR: rt->error=%p, rt->main_function=%p, rt->globals=%p\n",
               (void*)rt->error, (void*)rt->main_function, (void*)rt->globals);
        runtime_free(rt);
        return 1;
    }
    
    printf("DEBUG: No errors, preparing to run...\n");
    fflush(stdout);
    
    printf("DEBUG: About to call runtime_run...\n");
    fflush(stdout);
    
    int exit_code = runtime_run(rt);
    
    printf("DEBUG: runtime_run returned: %d\n", exit_code);
    fflush(stdout);
    
    if (runtime_has_error(rt)) {
        const char* error = runtime_get_error(rt);
        printf("ERROR: Runtime error after run: %s\n", error ? error : "(null)");
    }
    
    printf("DEBUG: About to free runtime...\n");
    fflush(stdout);
    
    runtime_free(rt);
    
    printf("DEBUG: Runtime freed\n");
    fflush(stdout);
    
    return exit_code;
}
