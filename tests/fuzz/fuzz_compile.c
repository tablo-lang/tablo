#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "compiler.h"
#include "fuzz_targets.h"
#include "parser.h"
#include "safe_alloc.h"
#include "typechecker.h"

static void fuzz_compile_free_result(CompileResult* result) {
    if (!result) return;

    error_free(result->error);
    result->error = NULL;

    if (result->function) {
        obj_function_free(result->function);
        result->function = NULL;
    }
    if (result->globals) {
        symbol_table_free(result->globals);
        result->globals = NULL;
    }
    if (result->functions) {
        for (int i = 0; i < result->function_count; i++) {
            if (result->functions[i]) {
                obj_function_release(result->functions[i]);
            }
        }
        free(result->functions);
        result->functions = NULL;
    }
    result->function_count = 0;
}

int fuzz_compile_one_input(const uint8_t* data, size_t size) {
    if (!data) return 0;
    if (size > (1u << 20)) return 0;

    char* input = (char*)malloc(size + 1);
    if (!input) return 0;
    if (size > 0) {
        memcpy(input, data, size);
    }
    input[size] = '\0';

    SafeAllocJmpContext alloc_ctx;
    jmp_buf alloc_env;
    char alloc_message[256] = {0};
    safe_alloc_push_jmp_context(&alloc_ctx, &alloc_env, alloc_message, sizeof(alloc_message));

    if (setjmp(alloc_env) != 0) {
        safe_alloc_pop_jmp_context(&alloc_ctx);
        free(input);
        return 0;
    }

    ParseResult parse_result = parser_parse(input, "fuzz_compile.tblo");
    if (!parse_result.error && parse_result.program) {
        TypeCheckResult typecheck_result = typecheck(parse_result.program);
        if (!typecheck_result.error) {
            CompileResult compile_result = compile(parse_result.program);
            fuzz_compile_free_result(&compile_result);
        }
        symbol_table_free(typecheck_result.globals);
        error_free(typecheck_result.error);
    }
    parser_free_result(&parse_result);

    safe_alloc_pop_jmp_context(&alloc_ctx);
    free(input);
    return 0;
}

#ifdef TABLO_LIBFUZZER_ENTRYPOINT
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_compile_one_input(data, size);
}
#endif
