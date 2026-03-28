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

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TABLO_FUZZ_HAS_LSAN 1
#endif
#endif

#if !defined(TABLO_FUZZ_HAS_LSAN) && defined(__SANITIZE_ADDRESS__)
#define TABLO_FUZZ_HAS_LSAN 1
#endif

#if defined(TABLO_FUZZ_HAS_LSAN)
void __lsan_disable(void);
void __lsan_enable(void);

static void fuzz_compile_lsan_disable(void) {
    __lsan_disable();
}

static void fuzz_compile_lsan_enable(void) {
    __lsan_enable();
}
#else
static void fuzz_compile_lsan_disable(void) {
}

static void fuzz_compile_lsan_enable(void) {
}
#endif

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
    volatile int lsan_disabled = 0;

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
        if (lsan_disabled) {
            fuzz_compile_lsan_enable();
        }
        safe_alloc_pop_jmp_context(&alloc_ctx);
        free(input);
        return 0;
    }

    fuzz_compile_lsan_disable();
    lsan_disabled = 1;

    ParseResult parse_result = parser_parse_quiet(input, "fuzz_compile.tblo");
    if (parse_result.error) {
        parser_free_parse_only_result(&parse_result);
        fuzz_compile_lsan_enable();
        safe_alloc_pop_jmp_context(&alloc_ctx);
        free(input);
        return 0;
    }

    if (parse_result.program) {
        TypeCheckOptions typecheck_options = {0};
        typecheck_options.report_diagnostics = false;
        TypeCheckResult typecheck_result = typecheck_with_options(parse_result.program, typecheck_options);
        if (!typecheck_result.error) {
            CompileResult compile_result = compile(parse_result.program);
            fuzz_compile_free_result(&compile_result);
        }
        symbol_table_free(typecheck_result.globals);
        error_free(typecheck_result.error);
    }
    parser_free_result(&parse_result);

    fuzz_compile_lsan_enable();
    safe_alloc_pop_jmp_context(&alloc_ctx);
    free(input);
    return 0;
}

#ifdef TABLO_LIBFUZZER_ENTRYPOINT
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_compile_one_input(data, size);
}
#endif
