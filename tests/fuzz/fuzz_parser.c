#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "fuzz_targets.h"
#include "parser.h"
#include "safe_alloc.h"

int fuzz_parser_one_input(const uint8_t* data, size_t size) {
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

    ParseResult result = parser_parse_quiet(input, "fuzz_parser.tblo");
    parser_free_parse_only_result(&result);

    safe_alloc_pop_jmp_context(&alloc_ctx);
    free(input);
    return 0;
}

#ifdef TABLO_LIBFUZZER_ENTRYPOINT
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_parser_one_input(data, size);
}
#endif
