#include "../src/runtime.h"
#include <stdio.h>

int main() {
    printf("Testing basic TabloLang execution...\n");
    
    Runtime* rt = runtime_create("../tests/native_integration_tests/io_tests.tblo");
    
    if (runtime_has_error(rt)) {
        fprintf(stderr, "Error loading program: %s\n", runtime_get_error(rt));
        runtime_free(rt);
        return 1;
    }
    
    printf("Running program...\n");
    int exit_code = runtime_run(rt);
    printf("Exit code: %d\n", exit_code);
    
    if (runtime_has_error(rt)) {
        fprintf(stderr, "Runtime error: %s\n", runtime_get_error(rt));
    }
    
    runtime_free(rt);
    return exit_code;
}
