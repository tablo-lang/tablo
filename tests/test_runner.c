#include "vm.h"
#include "bytecode.h"
#include "artifact.h"
#include "lexer.h"
#include "parser.h"
#include "typechecker.h"
#include "compiler.h"
#include "jit.h"
#include "lsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

static int test_instruction_len(uint8_t op) {
    switch (op) {
        case OP_JUMP_IF_LOCAL_LT:
        case OP_JUMP_IF_LOCAL_LE:
        case OP_JUMP_IF_LOCAL_GT:
        case OP_JUMP_IF_LOCAL_GE:
        case OP_JUMP_IF_LOCAL_EQ:
        case OP_JUMP_IF_LOCAL_NE:
        case OP_JUMP_IF_LOCAL_EQ_GLOBAL:
        case OP_JUMP_IF_LOCAL_NE_GLOBAL:
        case OP_JUMP_IF_LOCAL_EQ_GLOBAL_SLOT:
        case OP_JUMP_IF_LOCAL_NE_GLOBAL_SLOT:
        case OP_JUMP_IF_LOCAL_LT_CONST:
        case OP_JUMP_IF_LOCAL_LE_CONST:
        case OP_JUMP_IF_LOCAL_GT_CONST:
        case OP_JUMP_IF_LOCAL_GE_CONST:
        case OP_JUMP_IF_LOCAL_EQ_CONST:
        case OP_JUMP_IF_LOCAL_NE_CONST:
        case OP_JUMP_IF_ARRAY_FALSE_LOCAL_CONST:
        case OP_JUMP_IF_ARRAY_FALSE_LOCAL_LOCAL:
        case OP_ADD_LOCALS_INT:
        case OP_SUB_LOCALS_INT:
        case OP_MUL_LOCALS_INT:
        case OP_DIV_LOCALS_INT:
        case OP_MOD_LOCALS_INT:
        case OP_BIT_AND_LOCALS_INT:
        case OP_BIT_OR_LOCALS_INT:
        case OP_BIT_XOR_LOCALS_INT:
        case OP_ADD_LOCALS_DOUBLE:
        case OP_SUB_LOCALS_DOUBLE:
        case OP_MUL_LOCALS_DOUBLE:
        case OP_DIV_LOCALS_DOUBLE:
        case OP_ADD_LOCAL_CONST_INT:
        case OP_SUB_LOCAL_CONST_INT:
        case OP_MUL_LOCAL_CONST_INT:
        case OP_DIV_LOCAL_CONST_INT:
        case OP_MOD_LOCAL_CONST_INT:
        case OP_BIT_AND_LOCAL_CONST_INT:
        case OP_BIT_OR_LOCAL_CONST_INT:
        case OP_BIT_XOR_LOCAL_CONST_INT:
        case OP_ADD_LOCAL_CONST_DOUBLE:
        case OP_SUB_LOCAL_CONST_DOUBLE:
        case OP_MUL_LOCAL_CONST_DOUBLE:
        case OP_DIV_LOCAL_CONST_DOUBLE:
        case OP_ARRAY_GET_LOCAL_CONST_INT_TO_LOCAL:
        case OP_ARRAY_GET_LOCAL_LOCAL_INT_TO_LOCAL:
        case OP_ARRAY_GET_LOCAL_CONST_DOUBLE_TO_LOCAL:
        case OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE_TO_LOCAL:
            return 5;
        case OP_ARRAY_GET_FIELD_LOCAL_CONST:
        case OP_ARRAY_GET_FIELD_LOCAL_LOCAL:
        case OP_ARRAY_SET_FIELD_LOCAL_CONST:
        case OP_ARRAY_SET_FIELD_LOCAL_LOCAL:
        case OP_ADD_LOCAL_DIV_LOCALS:
        case OP_MADD_LOCAL_ARRAY_LOCAL_INT:
        case OP_MADD_LOCAL_ARRAY_LOCAL_DOUBLE:
        case OP_RECORD_NEW_NAMED:
        case OP_JUMP_IF_STACK_LT_LOCAL:
        case OP_JUMP_IF_STACK_LE_LOCAL:
        case OP_JUMP_IF_STACK_GT_LOCAL:
        case OP_JUMP_IF_STACK_GE_LOCAL:
        case OP_JUMP_IF_STACK_LT_CONST:
        case OP_JUMP_IF_STACK_LE_CONST:
        case OP_JUMP_IF_STACK_GT_CONST:
        case OP_JUMP_IF_STACK_GE_CONST:
        case OP_JUMP_IF_FALSE_POP:
        case OP_CALL_GLOBAL16:
            return 4;
        case OP_EVALA_MADD_LOCAL_ARRAY_LOCAL_DOUBLE:
        case OP_MUL_LOCALS_INT_TO_LOCAL:
        case OP_MUL_LOCALS_DOUBLE_TO_LOCAL:
        case OP_ADD_GLOBAL_GLOBAL_TO_GLOBAL:
        case OP_SUB_GLOBAL_GLOBAL_TO_GLOBAL:
        case OP_MUL_GLOBAL_GLOBAL_TO_GLOBAL:
        case OP_DIV_GLOBAL_GLOBAL_TO_GLOBAL:
        case OP_ADD_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
        case OP_SUB_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
        case OP_MUL_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
        case OP_DIV_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
            return 7;
        case OP_ADD2_LOCAL_CONST:
        case OP_CALL_INTERFACE:
            return 6;
        case OP_TYPE_TEST_INTERFACE_METHOD:
            return 5;
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_ADD_LOCAL_CONST:
        case OP_SUB_LOCAL_CONST:
        case OP_CALL_GLOBAL:
        case OP_ARRAY_GET_LOCAL_CONST:
        case OP_ARRAY_GET_LOCAL_LOCAL:
        case OP_ARRAY_SET_LOCAL_CONST:
        case OP_ARRAY_SET_LOCAL_LOCAL:
        case OP_ARRAY_GET_LOCAL_CONST_INT:
        case OP_ARRAY_GET_LOCAL_LOCAL_INT:
        case OP_ARRAY_SET_LOCAL_CONST_INT:
        case OP_ARRAY_SET_LOCAL_LOCAL_INT:
        case OP_ARRAY_GET_LOCAL_CONST_DOUBLE:
        case OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE:
        case OP_ARRAY_SET_LOCAL_CONST_DOUBLE:
        case OP_ARRAY_SET_LOCAL_LOCAL_DOUBLE:
        case OP_ARRAY_BOUNDS_CHECK_LOCAL_CONST:
        case OP_ARRAY_BOUNDS_CHECK_LOCAL_LOCAL:
        case OP_SQRT_LOCAL_DOUBLE:
        case OP_ARRAY_LEN_LOCAL:
        case OP_ADD_STACK_LOCAL_INT:
        case OP_SUB_STACK_LOCAL_INT:
        case OP_MUL_STACK_LOCAL_INT:
        case OP_DIV_STACK_LOCAL_INT:
        case OP_MOD_STACK_LOCAL_INT:
        case OP_BIT_AND_STACK_LOCAL_INT:
        case OP_BIT_OR_STACK_LOCAL_INT:
        case OP_BIT_XOR_STACK_LOCAL_INT:
        case OP_ADD_STACK_CONST_INT:
        case OP_SUB_STACK_CONST_INT:
        case OP_MUL_STACK_CONST_INT:
        case OP_DIV_STACK_CONST_INT:
        case OP_MOD_STACK_CONST_INT:
        case OP_BIT_AND_STACK_CONST_INT:
        case OP_BIT_OR_STACK_CONST_INT:
        case OP_BIT_XOR_STACK_CONST_INT:
        case OP_ADD_STACK_CONST_DOUBLE:
        case OP_SUB_STACK_CONST_DOUBLE:
        case OP_MUL_STACK_CONST_DOUBLE:
        case OP_DIV_STACK_CONST_DOUBLE:
        case OP_ADD_STACK_LOCAL_DOUBLE:
        case OP_SUB_STACK_LOCAL_DOUBLE:
        case OP_MUL_STACK_LOCAL_DOUBLE:
        case OP_DIV_STACK_LOCAL_DOUBLE:
        case OP_ARRAY_COPY_LOCAL_LOCAL:
        case OP_ARRAY_REVERSE_PREFIX_LOCAL_LOCAL:
        case OP_ARRAY_ROTATE_PREFIX_LEFT_LOCAL_LOCAL:
        case OP_ARRAY_ROTATE_PREFIX_RIGHT_LOCAL_LOCAL:
        case OP_CONST16:
        case OP_LOAD_GLOBAL16:
        case OP_STORE_GLOBAL16:
        case OP_EVALA_RECIP_LOCALS_DOUBLE:
            return 3;
        case OP_CONST:
        case OP_LOAD_LOCAL:
        case OP_STORE_LOCAL:
        case OP_NEGATE_LOCAL:
        case OP_LOAD_GLOBAL:
        case OP_STORE_GLOBAL:
        case OP_CALL:
        case OP_MAKE_CLOSURE:
        case OP_ARRAY_NEW:
        case OP_ARRAY_GET_LOCAL:
        case OP_ARRAY_SET_LOCAL:
        case OP_RECORD_NEW:
        case OP_RECORD_SET_FIELD:
        case OP_RECORD_GET_FIELD:
        case OP_TUPLE_NEW:
        case OP_TUPLE_GET:
        case OP_TUPLE_SET:
        case OP_DEFER:
        case OP_ADD_LOCAL_STACK_INT:
        case OP_SUB_LOCAL_STACK_INT:
        case OP_ADD_LOCAL_STACK_DOUBLE:
        case OP_SUB_LOCAL_STACK_DOUBLE:
            return 2;
        default:
            return 1;
    }
}

static bool chunk_contains_opcode(const Chunk* chunk, uint8_t opcode) {
    if (!chunk || !chunk->code) return false;
    for (int i = 0; i < chunk->code_count;) {
        uint8_t op = chunk->code[i];
        if (op == opcode) return true;
        i += test_instruction_len(op);
    }
    return false;
}

static int chunk_count_opcode(const Chunk* chunk, uint8_t opcode) {
    if (!chunk || !chunk->code) return 0;
    int count = 0;
    for (int i = 0; i < chunk->code_count;) {
        uint8_t op = chunk->code[i];
        if (op == opcode) count++;
        i += test_instruction_len(op);
    }
    return count;
}

static int chunk_count_add_like_ops(const Chunk* chunk) {
    if (!chunk) return 0;
    return chunk_count_opcode(chunk, OP_ADD) +
           chunk_count_opcode(chunk, OP_ADD_INT) +
           chunk_count_opcode(chunk, OP_ADD_LOCALS_INT) +
           chunk_count_opcode(chunk, OP_ADD_LOCAL_CONST_INT) +
           chunk_count_opcode(chunk, OP_ADD_STACK_CONST_INT);
}

static bool chunk_contains_add_locals_int_operands(const Chunk* chunk, uint8_t a, uint8_t b) {
    if (!chunk || !chunk->code) return false;
    for (int i = 0; i < chunk->code_count;) {
        uint8_t op = chunk->code[i];
        if (op == OP_ADD_LOCALS_INT &&
            i + 4 < chunk->code_count &&
            chunk->code[i + 2] == a &&
            chunk->code[i + 3] == b) {
            return true;
        }
        i += test_instruction_len(op);
    }
    return false;
}

static const Chunk* find_compiled_function_chunk(const CompileResult* result, const char* name) {
    if (!result || !name || !result->functions) return NULL;
    for (int i = 0; i < result->function_count; i++) {
        ObjFunction* fn = result->functions[i];
        if (!fn || !fn->name) continue;
        if (strcmp(fn->name, name) == 0) {
            return &fn->chunk;
        }
    }
    return NULL;
}

static ObjFunction* find_compiled_function_object(const CompileResult* result, const char* name) {
    if (!result || !name || !result->functions) return NULL;
    for (int i = 0; i < result->function_count; i++) {
        ObjFunction* fn = result->functions[i];
        if (!fn || !fn->name) continue;
        if (strcmp(fn->name, name) == 0) {
            return fn;
        }
    }
    return NULL;
}

static void reset_function_for_jit_queue_test(ObjFunction* function) {
    if (!function) return;
    function->jit_entry_count = 0;
    function->jit_hot = false;
    function->jit_state = JIT_FUNC_STATE_COLD;
    function->jit_reason = JIT_REASON_NONE;
    function->jit_compile_attempts = 0;
    function->jit_compiled_call_count = 0;
    function->jit_compiled_entry = NULL;
    memset(&function->jit_compiled_plan, 0, sizeof(function->jit_compiled_plan));
    function->jit_compiled_plan.kind = JIT_COMPILED_KIND_NONE;
}

static void reset_function_for_exact_jit_match_test(ObjFunction* function) {
    reset_function_for_jit_queue_test(function);
    memset(&function->jit_profile.summary, 0, sizeof(function->jit_profile.summary));
    function->jit_profile.summary.kind = JIT_SUMMARY_KIND_NONE;
    memset(&function->jit_hint_plan, 0, sizeof(function->jit_hint_plan));
    function->jit_hint_plan.kind = JIT_COMPILED_KIND_NONE;
}

static void test_value_creation(void) {
    printf("Testing value creation...\n");

    Value val1;
    value_init_int(&val1, 42);
    if (value_get_type(&val1) == VAL_INT && value_get_int(&val1) == 42) {
        tests_passed++;
        printf("  PASS: int value creation\n");
    } else {
        tests_failed++;
        printf("  FAIL: int value creation\n");
    }

    Value val_bool;
    value_init_bool(&val_bool, true);
    if (value_get_type(&val_bool) == VAL_BOOL && value_get_bool(&val_bool)) {
        tests_passed++;
        printf("  PASS: bool value creation\n");
    } else {
        tests_failed++;
        printf("  FAIL: bool value creation\n");
    }

    Value val2;
    value_init_double(&val2, 3.14);
    if (value_get_type(&val2) == VAL_DOUBLE && value_get_double(&val2) == 3.14) {
        tests_passed++;
        printf("  PASS: double value creation\n");
    } else {
        tests_failed++;
        printf("  FAIL: double value creation\n");
    }

    Value val3;
    value_init_nil(&val3);
    if (value_get_type(&val3) == VAL_NIL) {
        tests_passed++;
        printf("  PASS: nil value creation\n");
    } else {
        tests_failed++;
        printf("  FAIL: nil value creation\n");
    }
}

static void test_lexer(void) {
    printf("Testing lexer...\n");

    const char* source = "var x: int = 42;";
    Lexer lexer;
    lexer_init(&lexer, source, "test.tblo");

    Token token1 = lexer_next_token(&lexer);
    if (token1.type == TOKEN_KEYWORD_VAR) {
        tests_passed++;
        printf("  PASS: var keyword\n");
    } else {
        tests_failed++;
        printf("  FAIL: var keyword\n");
    }

    Token token2 = lexer_next_token(&lexer);
    if (token2.type == TOKEN_IDENTIFIER && strcmp(token2.lexeme, "x") == 0) {
        tests_passed++;
        printf("  PASS: identifier\n");
    } else {
        tests_failed++;
        printf("  FAIL: identifier\n");
    }

    Token token3 = lexer_next_token(&lexer);
    if (token3.type == TOKEN_COLON) {
        tests_passed++;
        printf("  PASS: colon token\n");
    } else {
        tests_failed++;
        printf("  FAIL: colon token\n");
    }

    Token token4 = lexer_next_token(&lexer);
    printf("Token 4: type=%d, lexeme='%s'\n", token4.type, token4.lexeme ? token4.lexeme : "null");
    fflush(stdout);
    if (token4.type == TOKEN_KEYWORD_INT) {
        tests_passed++;
        printf("  PASS: int type\n");
    } else {
        tests_failed++;
        printf("  FAIL: int type (got token type: %d)\n", token4.type);
    }
    fflush(stdout);

    token_free(&token1);
    token_free(&token2);
    token_free(&token3);
    token_free(&token4);
    if (lexer.file) {
        free(lexer.file);
        lexer.file = NULL;
    }

    const char* const_type_source = "const LIMIT: int = 10; type UserId = int;";
    Lexer const_type_lexer;
    lexer_init(&const_type_lexer, const_type_source, "test.tblo");

    Token ct1 = lexer_next_token(&const_type_lexer);
    Token ct2 = lexer_next_token(&const_type_lexer);
    Token ct3 = lexer_next_token(&const_type_lexer);
    Token ct4 = lexer_next_token(&const_type_lexer);
    Token ct5 = lexer_next_token(&const_type_lexer);
    Token ct6 = lexer_next_token(&const_type_lexer);
    Token ct7 = lexer_next_token(&const_type_lexer);
    Token ct8 = lexer_next_token(&const_type_lexer);
    Token ct9 = lexer_next_token(&const_type_lexer);
    Token ct10 = lexer_next_token(&const_type_lexer);
    Token ct11 = lexer_next_token(&const_type_lexer);
    Token ct12 = lexer_next_token(&const_type_lexer);

    if (ct1.type == TOKEN_KEYWORD_CONST &&
        ct2.type == TOKEN_IDENTIFIER &&
        ct3.type == TOKEN_COLON &&
        ct4.type == TOKEN_KEYWORD_INT &&
        ct5.type == TOKEN_ASSIGN &&
        ct6.type == TOKEN_NUMBER_INT &&
        ct7.type == TOKEN_SEMICOLON &&
        ct8.type == TOKEN_KEYWORD_TYPE &&
        ct9.type == TOKEN_IDENTIFIER &&
        ct10.type == TOKEN_ASSIGN &&
        ct11.type == TOKEN_KEYWORD_INT &&
        ct12.type == TOKEN_SEMICOLON) {
        tests_passed++;
        printf("  PASS: const/type keyword tokens\n");
    } else {
        tests_failed++;
        printf("  FAIL: const/type keyword tokens\n");
    }

    token_free(&ct1);
    token_free(&ct2);
    token_free(&ct3);
    token_free(&ct4);
    token_free(&ct5);
    token_free(&ct6);
    token_free(&ct7);
    token_free(&ct8);
    token_free(&ct9);
    token_free(&ct10);
    token_free(&ct11);
    token_free(&ct12);
    if (const_type_lexer.file) {
        free(const_type_lexer.file);
        const_type_lexer.file = NULL;
    }

    const char* interface_source = "interface Runner { run(): int; };";
    Lexer interface_lexer;
    lexer_init(&interface_lexer, interface_source, "test.tblo");

    Token if1 = lexer_next_token(&interface_lexer);
    Token if2 = lexer_next_token(&interface_lexer);
    if (if1.type == TOKEN_KEYWORD_INTERFACE && if2.type == TOKEN_IDENTIFIER) {
        tests_passed++;
        printf("  PASS: interface keyword token\n");
    } else {
        tests_failed++;
        printf("  FAIL: interface keyword token\n");
    }

    token_free(&if1);
    token_free(&if2);
    if (interface_lexer.file) {
        free(interface_lexer.file);
        interface_lexer.file = NULL;
    }

    const char* impl_source = "impl Runner as SprintRunner { run = sprintRun; };";
    Lexer impl_lexer;
    lexer_init(&impl_lexer, impl_source, "test.tblo");

    Token im1 = lexer_next_token(&impl_lexer);
    Token im2 = lexer_next_token(&impl_lexer);
    Token im3 = lexer_next_token(&impl_lexer);
    Token im4 = lexer_next_token(&impl_lexer);
    if (im1.type == TOKEN_KEYWORD_IMPL &&
        im2.type == TOKEN_IDENTIFIER &&
        im3.type == TOKEN_AS &&
        im4.type == TOKEN_IDENTIFIER) {
        tests_passed++;
        printf("  PASS: impl keyword token\n");
    } else {
        tests_failed++;
        printf("  FAIL: impl keyword token\n");
    }

    token_free(&im1);
    token_free(&im2);
    token_free(&im3);
    token_free(&im4);
    if (impl_lexer.file) {
        free(impl_lexer.file);
        impl_lexer.file = NULL;
    }

    const char* visibility_source = "public const X: int = 1; private type Hidden = int;";
    Lexer visibility_lexer;
    lexer_init(&visibility_lexer, visibility_source, "test.tblo");

    Token vs1 = lexer_next_token(&visibility_lexer);
    Token vs2 = lexer_next_token(&visibility_lexer);
    Token vs3 = lexer_next_token(&visibility_lexer);
    Token vs4 = lexer_next_token(&visibility_lexer);
    Token vs5 = lexer_next_token(&visibility_lexer);
    Token vs6 = lexer_next_token(&visibility_lexer);
    Token vs7 = lexer_next_token(&visibility_lexer);
    Token vs8 = lexer_next_token(&visibility_lexer);
    Token vs9 = lexer_next_token(&visibility_lexer);
    Token vs10 = lexer_next_token(&visibility_lexer);
    Token vs11 = lexer_next_token(&visibility_lexer);
    Token vs12 = lexer_next_token(&visibility_lexer);

    if (vs1.type == TOKEN_KEYWORD_PUBLIC &&
        vs2.type == TOKEN_KEYWORD_CONST &&
        vs3.type == TOKEN_IDENTIFIER &&
        vs4.type == TOKEN_COLON &&
        vs5.type == TOKEN_KEYWORD_INT &&
        vs6.type == TOKEN_ASSIGN &&
        vs7.type == TOKEN_NUMBER_INT &&
        vs8.type == TOKEN_SEMICOLON &&
        vs9.type == TOKEN_KEYWORD_PRIVATE &&
        vs10.type == TOKEN_KEYWORD_TYPE &&
        vs11.type == TOKEN_IDENTIFIER &&
        vs12.type == TOKEN_ASSIGN) {
        tests_passed++;
        printf("  PASS: public/private keyword tokens\n");
    } else {
        tests_failed++;
        printf("  FAIL: public/private keyword tokens\n");
    }

    token_free(&vs1);
    token_free(&vs2);
    token_free(&vs3);
    token_free(&vs4);
    token_free(&vs5);
    token_free(&vs6);
    token_free(&vs7);
    token_free(&vs8);
    token_free(&vs9);
    token_free(&vs10);
    token_free(&vs11);
    token_free(&vs12);
    if (visibility_lexer.file) {
        free(visibility_lexer.file);
        visibility_lexer.file = NULL;
    }

    const char* enum_source = "enum Status { Ok = 1, Err = 2 };";
    Lexer enum_lexer;
    lexer_init(&enum_lexer, enum_source, "test.tblo");

    Token en1 = lexer_next_token(&enum_lexer);
    Token en2 = lexer_next_token(&enum_lexer);
    if (en1.type == TOKEN_KEYWORD_ENUM && en2.type == TOKEN_IDENTIFIER) {
        tests_passed++;
        printf("  PASS: enum keyword token\n");
    } else {
        tests_failed++;
        printf("  FAIL: enum keyword token\n");
    }

    token_free(&en1);
    token_free(&en2);
    if (enum_lexer.file) {
        free(enum_lexer.file);
        enum_lexer.file = NULL;
    }

    const char* match_source = "match (1) { 1: println(\"ok\"); else: println(\"other\"); }";
    Lexer match_lexer;
    lexer_init(&match_lexer, match_source, "test.tblo");

    Token mt1 = lexer_next_token(&match_lexer);
    Token mt2 = lexer_next_token(&match_lexer);
    if (mt1.type == TOKEN_KEYWORD_MATCH && mt2.type == TOKEN_LPAREN) {
        tests_passed++;
        printf("  PASS: match keyword token\n");
    } else {
        tests_failed++;
        printf("  FAIL: match keyword token\n");
    }

    token_free(&mt1);
    token_free(&mt2);
    if (match_lexer.file) {
        free(match_lexer.file);
        match_lexer.file = NULL;
    }

    const char* switch_source = "switch (flag) { case true: println(\"yes\"); default: println(\"no\"); }";
    Lexer switch_lexer;
    lexer_init(&switch_lexer, switch_source, "test.tblo");

    Token sw_tokens[14];
    for (int i = 0; i < 14; i++) {
        sw_tokens[i] = lexer_next_token(&switch_lexer);
    }

    if (sw_tokens[0].type == TOKEN_KEYWORD_SWITCH &&
        sw_tokens[1].type == TOKEN_LPAREN &&
        sw_tokens[5].type == TOKEN_KEYWORD_CASE &&
        sw_tokens[6].type == TOKEN_TRUE &&
        sw_tokens[13].type == TOKEN_KEYWORD_DEFAULT) {
        tests_passed++;
        printf("  PASS: switch/case/default keyword tokens\n");
    } else {
        tests_failed++;
        printf("  FAIL: switch/case/default keyword tokens\n");
    }

    for (int i = 0; i < 14; i++) {
        token_free(&sw_tokens[i]);
    }
    if (switch_lexer.file) {
        free(switch_lexer.file);
        switch_lexer.file = NULL;
    }

    const char* let_source = "if let Result.Ok(value) = result { println(value as string); }";
    Lexer let_lexer;
    lexer_init(&let_lexer, let_source, "test.tblo");

    Token let_if = lexer_next_token(&let_lexer);
    Token let_kw = lexer_next_token(&let_lexer);
    if (let_if.type == TOKEN_KEYWORD_IF && let_kw.type == TOKEN_KEYWORD_LET) {
        tests_passed++;
        printf("  PASS: let keyword token\n");
    } else {
        tests_failed++;
        printf("  FAIL: let keyword token\n");
    }

    token_free(&let_if);
    token_free(&let_kw);
    if (let_lexer.file) {
        free(let_lexer.file);
        let_lexer.file = NULL;
    }

    const char* async_source = "public async func load(): int { return await fetch(); }";
    Lexer async_lexer;
    lexer_init(&async_lexer, async_source, "test.tblo");

    Token async_tokens[16];
    for (int i = 0; i < 16; i++) {
        async_tokens[i] = lexer_next_token(&async_lexer);
    }

    if (async_tokens[0].type == TOKEN_KEYWORD_PUBLIC &&
        async_tokens[1].type == TOKEN_KEYWORD_ASYNC &&
        async_tokens[2].type == TOKEN_KEYWORD_FUNC &&
        async_tokens[3].type == TOKEN_IDENTIFIER &&
        async_tokens[9].type == TOKEN_KEYWORD_RETURN &&
        async_tokens[10].type == TOKEN_KEYWORD_AWAIT) {
        tests_passed++;
        printf("  PASS: async/await keyword tokens\n");
    } else {
        tests_failed++;
        printf("  FAIL: async/await keyword tokens\n");
    }

    for (int i = 0; i < 16; i++) {
        token_free(&async_tokens[i]);
    }
    if (async_lexer.file) {
        free(async_lexer.file);
        async_lexer.file = NULL;
    }

    const char* bitwise_source = "1 & 2 | 3 ^ ~4";
    Lexer bitwise_lexer;
    lexer_init(&bitwise_lexer, bitwise_source, "test.tblo");

    Token bw1 = lexer_next_token(&bitwise_lexer);
    Token bw_and = lexer_next_token(&bitwise_lexer);
    Token bw2 = lexer_next_token(&bitwise_lexer);
    Token bw_or = lexer_next_token(&bitwise_lexer);
    Token bw3 = lexer_next_token(&bitwise_lexer);
    Token bw_xor = lexer_next_token(&bitwise_lexer);
    Token bw_not = lexer_next_token(&bitwise_lexer);
    Token bw4 = lexer_next_token(&bitwise_lexer);

    if (bw1.type == TOKEN_NUMBER_INT &&
        bw_and.type == TOKEN_BIT_AND &&
        bw2.type == TOKEN_NUMBER_INT &&
        bw_or.type == TOKEN_BIT_OR &&
        bw3.type == TOKEN_NUMBER_INT &&
        bw_xor.type == TOKEN_BIT_XOR &&
        bw_not.type == TOKEN_BIT_NOT &&
        bw4.type == TOKEN_NUMBER_INT) {
        tests_passed++;
        printf("  PASS: bitwise operator tokens\n");
    } else {
        tests_failed++;
        printf("  FAIL: bitwise operator tokens\n");
    }

    token_free(&bw1);
    token_free(&bw_and);
    token_free(&bw2);
    token_free(&bw_or);
    token_free(&bw3);
    token_free(&bw_xor);
    token_free(&bw_not);
    token_free(&bw4);
    if (bitwise_lexer.file) {
        free(bitwise_lexer.file);
        bitwise_lexer.file = NULL;
    }

    const char* range_token_source = "foreach (i in 0..5) { }";
    Lexer range_token_lexer;
    lexer_init(&range_token_lexer, range_token_source, "test.tblo");

    bool saw_dot_dot = false;
    while (true) {
        Token tok = lexer_next_token(&range_token_lexer);
        TokenType tok_type = tok.type;
        if (tok_type == TOKEN_DOT_DOT) {
            saw_dot_dot = true;
        }
        token_free(&tok);
        if (tok_type == TOKEN_EOF || tok_type == TOKEN_ERROR) break;
    }

    if (saw_dot_dot) {
        tests_passed++;
        printf("  PASS: range token '..'\n");
    } else {
        tests_failed++;
        printf("  FAIL: range token '..'\n");
    }

    if (range_token_lexer.file) {
        free(range_token_lexer.file);
        range_token_lexer.file = NULL;
    }

    const char* logical_source = "true && false || !true";
    Lexer logical_lexer;
    lexer_init(&logical_lexer, logical_source, "test.tblo");

    Token lg1 = lexer_next_token(&logical_lexer);
    Token lg_and = lexer_next_token(&logical_lexer);
    Token lg2 = lexer_next_token(&logical_lexer);
    Token lg_or = lexer_next_token(&logical_lexer);
    Token lg_not = lexer_next_token(&logical_lexer);
    Token lg3 = lexer_next_token(&logical_lexer);

    if (lg1.type == TOKEN_TRUE &&
        lg_and.type == TOKEN_AND &&
        lg2.type == TOKEN_FALSE &&
        lg_or.type == TOKEN_OR &&
        lg_not.type == TOKEN_NOT &&
        lg3.type == TOKEN_TRUE) {
        tests_passed++;
        printf("  PASS: logical operator tokens\n");
    } else {
        tests_failed++;
        printf("  FAIL: logical operator tokens\n");
    }

    token_free(&lg1);
    token_free(&lg_and);
    token_free(&lg2);
    token_free(&lg_or);
    token_free(&lg_not);
    token_free(&lg3);
    if (logical_lexer.file) {
        free(logical_lexer.file);
        logical_lexer.file = NULL;
    }

    const char* multiline_comment_source =
        "var x: int = 1; /* block comment\n"
        "still in comment */ var y: int = 2;";
    Lexer multiline_comment_lexer;
    lexer_init(&multiline_comment_lexer, multiline_comment_source, "test.tblo");

    bool saw_x = false;
    bool saw_y = false;
    bool saw_comment_error = false;
    while (true) {
        Token tok = lexer_next_token(&multiline_comment_lexer);
        TokenType tok_type = tok.type;
        if (tok_type == TOKEN_IDENTIFIER && tok.lexeme) {
            if (strcmp(tok.lexeme, "x") == 0) saw_x = true;
            if (strcmp(tok.lexeme, "y") == 0) saw_y = true;
        }
        if (tok_type == TOKEN_ERROR) {
            saw_comment_error = true;
        }
        token_free(&tok);
        if (tok_type == TOKEN_EOF || tok_type == TOKEN_ERROR) break;
    }

    if (saw_x && saw_y && !saw_comment_error) {
        tests_passed++;
        printf("  PASS: multi-line comment skipping\n");
    } else {
        tests_failed++;
        printf("  FAIL: multi-line comment skipping\n");
    }

    if (multiline_comment_lexer.file) {
        free(multiline_comment_lexer.file);
        multiline_comment_lexer.file = NULL;
    }

    const char* unterminated_multiline_comment_source = "var x: int = 1; /* unterminated";
    Lexer unterminated_multiline_comment_lexer;
    lexer_init(&unterminated_multiline_comment_lexer, unterminated_multiline_comment_source, "test.tblo");

    bool got_unterminated_comment_error = false;
    while (true) {
        Token tok = lexer_next_token(&unterminated_multiline_comment_lexer);
        TokenType tok_type = tok.type;
        if (tok_type == TOKEN_ERROR && tok.lexeme &&
            strstr(tok.lexeme, "Unterminated multi-line comment") != NULL) {
            got_unterminated_comment_error = true;
        }
        token_free(&tok);
        if (tok_type == TOKEN_EOF || tok_type == TOKEN_ERROR) break;
    }

    if (got_unterminated_comment_error) {
        tests_passed++;
        printf("  PASS: unterminated multi-line comment error\n");
    } else {
        tests_failed++;
        printf("  FAIL: unterminated multi-line comment error\n");
    }

    if (unterminated_multiline_comment_lexer.file) {
        free(unterminated_multiline_comment_lexer.file);
        unterminated_multiline_comment_lexer.file = NULL;
    }

    const char* unicode_identifier_source = "var \xE5\x90\x8D: int = 7;";
    Lexer unicode_identifier_lexer;
    lexer_init(&unicode_identifier_lexer, unicode_identifier_source, "test.tblo");

    Token uid_var = lexer_next_token(&unicode_identifier_lexer);
    Token uid_name = lexer_next_token(&unicode_identifier_lexer);
    if (uid_var.type == TOKEN_KEYWORD_VAR &&
        uid_name.type == TOKEN_IDENTIFIER &&
        uid_name.lexeme &&
        strcmp(uid_name.lexeme, "\xE5\x90\x8D") == 0) {
        tests_passed++;
        printf("  PASS: UTF-8 identifier tokenization\n");
    } else {
        tests_failed++;
        printf("  FAIL: UTF-8 identifier tokenization\n");
    }

    token_free(&uid_var);
    token_free(&uid_name);
    if (unicode_identifier_lexer.file) {
        free(unicode_identifier_lexer.file);
        unicode_identifier_lexer.file = NULL;
    }

    const char* unicode_escape_source = "var s: string = \"Smile: \\u263A\";";
    Lexer unicode_escape_lexer;
    lexer_init(&unicode_escape_lexer, unicode_escape_source, "test.tblo");

    Token us1 = lexer_next_token(&unicode_escape_lexer);
    Token us2 = lexer_next_token(&unicode_escape_lexer);
    Token us3 = lexer_next_token(&unicode_escape_lexer);
    Token us4 = lexer_next_token(&unicode_escape_lexer);
    Token us5 = lexer_next_token(&unicode_escape_lexer);
    Token us6 = lexer_next_token(&unicode_escape_lexer);
    if (us1.type == TOKEN_KEYWORD_VAR &&
        us2.type == TOKEN_IDENTIFIER &&
        us3.type == TOKEN_COLON &&
        us4.type == TOKEN_KEYWORD_STRING &&
        us5.type == TOKEN_ASSIGN &&
        us6.type == TOKEN_STRING &&
        us6.as_string &&
        strcmp(us6.as_string, "Smile: \xE2\x98\xBA") == 0) {
        tests_passed++;
        printf("  PASS: Unicode string escape decoding\n");
    } else {
        tests_failed++;
        printf("  FAIL: Unicode string escape decoding\n");
    }

    token_free(&us1);
    token_free(&us2);
    token_free(&us3);
    token_free(&us4);
    token_free(&us5);
    token_free(&us6);
    if (unicode_escape_lexer.file) {
        free(unicode_escape_lexer.file);
        unicode_escape_lexer.file = NULL;
    }

    const char* invalid_unicode_escape_source = "var s: string = \"\\u12G4\";";
    Lexer invalid_unicode_escape_lexer;
    lexer_init(&invalid_unicode_escape_lexer, invalid_unicode_escape_source, "test.tblo");

    bool got_invalid_unicode_escape = false;
    while (true) {
        Token tok = lexer_next_token(&invalid_unicode_escape_lexer);
        TokenType tok_type = tok.type;
        if (tok_type == TOKEN_ERROR && tok.lexeme &&
            strstr(tok.lexeme, "Invalid Unicode escape sequence") != NULL) {
            got_invalid_unicode_escape = true;
        }
        token_free(&tok);
        if (tok_type == TOKEN_EOF || tok_type == TOKEN_ERROR) break;
    }

    if (got_invalid_unicode_escape) {
        tests_passed++;
        printf("  PASS: invalid Unicode escape error\n");
    } else {
        tests_failed++;
        printf("  FAIL: invalid Unicode escape error\n");
    }

    if (invalid_unicode_escape_lexer.file) {
        free(invalid_unicode_escape_lexer.file);
        invalid_unicode_escape_lexer.file = NULL;
    }
}

static void test_parser(void) {
    printf("Testing parser...\n");

    const char* source = "var x: int = 42;";
    ParseResult result = parser_parse(source, "test.tblo");

    if (result.program && result.program->stmt_count == 1) {
        Stmt* stmt = result.program->statements[0];
        if (stmt->kind == STMT_VAR_DECL && strcmp(stmt->var_decl.name, "x") == 0) {
            tests_passed++;
            printf("  PASS: var declaration parsing\n");
        } else {
            tests_failed++;
            printf("  FAIL: var declaration parsing\n");
        }
    } else {
        tests_failed++;
        printf("  FAIL: var declaration parsing\n");
    }
    parser_free_result(&result);

    const char* async_parse_source =
        "async func load(flag: bool): int {\n"
        "    return await if (flag) { await getTrue() } else { await getFalse() };\n"
        "}\n";
    ParseResult async_parse = parser_parse(async_parse_source, "test.tblo");
    if (!async_parse.error &&
        async_parse.program &&
        async_parse.program->stmt_count == 1 &&
        async_parse.program->statements[0] &&
        async_parse.program->statements[0]->kind == STMT_FUNC_DECL) {
        Stmt* async_func = async_parse.program->statements[0];
        Stmt* async_return =
            async_func->func_decl.body &&
            async_func->func_decl.body->kind == STMT_BLOCK &&
            async_func->func_decl.body->block.stmt_count == 1
                ? async_func->func_decl.body->block.statements[0]
                : NULL;
        Expr* top_await =
            async_return &&
            async_return->kind == STMT_RETURN
                ? async_return->return_value
                : NULL;
        bool parsed_async_shape =
            async_func->func_decl.is_async &&
            top_await &&
            top_await->kind == EXPR_AWAIT &&
            top_await->await_expr.expr &&
            top_await->await_expr.expr->kind == EXPR_IF &&
            top_await->await_expr.expr->if_expr.then_expr &&
            top_await->await_expr.expr->if_expr.then_expr->kind == EXPR_BLOCK &&
            top_await->await_expr.expr->if_expr.then_expr->block_expr.value &&
            top_await->await_expr.expr->if_expr.then_expr->block_expr.value->kind == EXPR_AWAIT &&
            top_await->await_expr.expr->if_expr.else_expr &&
            top_await->await_expr.expr->if_expr.else_expr->kind == EXPR_BLOCK &&
            top_await->await_expr.expr->if_expr.else_expr->block_expr.value &&
            top_await->await_expr.expr->if_expr.else_expr->block_expr.value->kind == EXPR_AWAIT;

        if (parsed_async_shape) {
            tests_passed++;
            printf("  PASS: async function declaration and await parsing\n");
        } else {
            tests_failed++;
            printf("  FAIL: async function declaration and await parsing\n");
        }
    } else {
        tests_failed++;
        printf("  FAIL: async function declaration and await parsing\n");
    }
    parser_free_result(&async_parse);

    const char* await_top_level_source = "await work();";
    ParseResult await_top_level_parse = parser_parse(await_top_level_source, "test.tblo");
    if (await_top_level_parse.error &&
        await_top_level_parse.error->message &&
        strstr(await_top_level_parse.error->message,
               "await is only allowed inside async functions") != NULL) {
        tests_passed++;
        printf("  PASS: top-level await rejection\n");
    } else {
        tests_failed++;
        printf("  FAIL: top-level await rejection\n");
    }
    parser_free_result(&await_top_level_parse);

    const char* await_sync_source =
        "func load(): int {\n"
        "    return await work();\n"
        "}\n";
    ParseResult await_sync_parse = parser_parse(await_sync_source, "test.tblo");
    if (await_sync_parse.error &&
        await_sync_parse.error->message &&
        strstr(await_sync_parse.error->message,
               "await is only allowed inside async functions") != NULL) {
        tests_passed++;
        printf("  PASS: await in sync function rejection\n");
    } else {
        tests_failed++;
        printf("  FAIL: await in sync function rejection\n");
    }
    parser_free_result(&await_sync_parse);

    const char* async_malformed_source = "async var value: int = 1;";
    ParseResult async_malformed_parse = parser_parse(async_malformed_source, "test.tblo");
    if (async_malformed_parse.error &&
        async_malformed_parse.error->message &&
        strstr(async_malformed_parse.error->message, "Expected 'func' after 'async'") != NULL) {
        tests_passed++;
        printf("  PASS: malformed async declaration rejection\n");
    } else {
        tests_failed++;
        printf("  FAIL: malformed async declaration rejection\n");
    }
    parser_free_result(&async_malformed_parse);

    const char* async_literal_source =
        "func outer() {\n"
        "    var thunk = async func(): int { return 1; };\n"
        "}\n";
    ParseResult async_literal_parse = parser_parse(async_literal_source, "test.tblo");
    if (async_literal_parse.error &&
        async_literal_parse.error->message &&
        strstr(async_literal_parse.error->message,
               "Async function literals are not supported yet") != NULL) {
        tests_passed++;
        printf("  PASS: async function literal deferral diagnostic\n");
    } else {
        tests_failed++;
        printf("  FAIL: async function literal deferral diagnostic\n");
    }
    parser_free_result(&async_literal_parse);

    const char* async_typecheck_source =
        "async func load(): int { return 1; }\n"
        "async func pair(): (int, Error?) { return (1, nil); }\n"
        "func identity(task: Future[int]): Future[int] { return task; }\n"
        "async func unwrap(task: Future[int]): int {\n"
        "    var value: int = await identity(task);\n"
        "    return value;\n"
        "}\n"
        "async func useLoad(): int {\n"
        "    var value: int = await load();\n"
        "    return value;\n"
        "}\n";
    ParseResult async_typecheck_parse = parser_parse(async_typecheck_source, "test.tblo");
    if (!async_typecheck_parse.error) {
        TypeCheckResult async_typecheck_tc = typecheck(async_typecheck_parse.program);
        Symbol* load_sym = symbol_table_get(async_typecheck_tc.globals, "load");
        bool async_signature_ok =
            !async_typecheck_tc.error &&
            load_sym &&
            load_sym->type &&
            load_sym->type->kind == TYPE_FUNCTION &&
            load_sym->type->return_type &&
            load_sym->type->return_type->kind == TYPE_FUTURE &&
            load_sym->type->return_type->element_type &&
            load_sym->type->return_type->element_type->kind == TYPE_INT;
        if (async_signature_ok) {
            tests_passed++;
            printf("  PASS: async function type lowering and await unwrapping\n");
        } else {
            tests_failed++;
            printf("  FAIL: async function type lowering and await unwrapping\n");
        }
        symbol_table_free(async_typecheck_tc.globals);
        error_free(async_typecheck_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: async function type lowering parse\n");
    }
    parser_free_result(&async_typecheck_parse);

    const char* async_compile_ready_source =
        "async func load(): int { return 1; }\n"
        "func main(): void {\n"
        "    var task = load();\n"
        "    println(str(futureGet(task)));\n"
        "}\n";
    ParseResult async_compile_ready_parse = parser_parse(async_compile_ready_source, "test.tblo");
    if (!async_compile_ready_parse.error) {
        TypeCheckResult async_compile_ready_tc = typecheck(async_compile_ready_parse.program);
        if (!async_compile_ready_tc.error) {
            CompileResult async_compile_ready_compile = compile(async_compile_ready_parse.program);
            if (!async_compile_ready_compile.error && async_compile_ready_compile.function) {
                tests_passed++;
                printf("  PASS: async function without await compiles to future runtime core\n");
            } else {
                tests_failed++;
                printf("  FAIL: async function without await compiles to future runtime core\n");
            }
        } else {
            tests_failed++;
            printf("  FAIL: async function without await typecheck\n");
        }
        symbol_table_free(async_compile_ready_tc.globals);
        error_free(async_compile_ready_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: async function without await parse\n");
    }
    parser_free_result(&async_compile_ready_parse);

    const char* async_await_compile_source =
        "async func unwrap(task: Future[int]): int {\n"
        "    var base: int = 2;\n"
        "    return base + await task;\n"
        "}\n";
    ParseResult async_await_compile_parse = parser_parse(async_await_compile_source, "test.tblo");
    if (!async_await_compile_parse.error) {
        TypeCheckResult async_await_compile_tc = typecheck(async_await_compile_parse.program);
        if (!async_await_compile_tc.error) {
            CompileResult async_await_compile = compile(async_await_compile_parse.program);
            bool found_await = false;
            if (!async_await_compile.error && async_await_compile.function) {
                if (chunk_contains_opcode(&async_await_compile.function->chunk, OP_AWAIT)) {
                    found_await = true;
                } else {
                    for (int i = 0; i < async_await_compile.function_count; i++) {
                        if (async_await_compile.functions[i] &&
                            chunk_contains_opcode(&async_await_compile.functions[i]->chunk, OP_AWAIT)) {
                            found_await = true;
                            break;
                        }
                    }
                }
            }
            if (!async_await_compile.error && found_await) {
                tests_passed++;
                printf("  PASS: async await lowers to runtime await opcode\n");
            } else {
                tests_failed++;
                printf("  FAIL: async await lowers to runtime await opcode\n");
            }
        } else {
            tests_failed++;
            printf("  FAIL: async await lowering typecheck\n");
        }
        symbol_table_free(async_await_compile_tc.globals);
        error_free(async_await_compile_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: async await lowering parse\n");
    }
    parser_free_result(&async_await_compile_parse);

    const char* async_control_flow_compile_source =
        "async func branchBefore(flag: bool, left: Future[int], right: Future[int]): int {\n"
        "    var chosen: Future[int] = left;\n"
        "    if (!flag) {\n"
        "        chosen = right;\n"
        "    }\n"
        "    return 10 + await chosen;\n"
        "}\n"
        "async func nestedBlock(task: Future[int]): int {\n"
        "    var outer: int = 2;\n"
        "    {\n"
        "        var inner: int = 3;\n"
        "        outer = outer + inner + await task;\n"
        "    }\n"
        "    return outer;\n"
        "}\n"
        "async func loopAccumulate(first: Future[int], second: Future[int], third: Future[int]): int {\n"
        "    var i: int = 0;\n"
        "    var total: int = 0;\n"
        "    while (i < 3) {\n"
        "        var current: int = 0;\n"
        "        if (i == 0) {\n"
        "            current = await first;\n"
        "        } else if (i == 1) {\n"
        "            current = await second;\n"
        "        } else {\n"
        "            current = await third;\n"
        "        }\n"
        "        total = total + current;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return total;\n"
        "}\n"
        "async func loopContinue(taskA: Future[int], taskB: Future[int]): int {\n"
        "    var i: int = 0;\n"
        "    var total: int = 0;\n"
        "    while (i < 3) {\n"
        "        i = i + 1;\n"
        "        if (i == 1) {\n"
        "            total = total + await taskA;\n"
        "            continue;\n"
        "        }\n"
        "        if (i == 2) {\n"
        "            total = total + 5;\n"
        "            continue;\n"
        "        }\n"
        "        total = total + await taskB;\n"
        "    }\n"
        "    return total;\n"
        "}\n";
    ParseResult async_control_flow_compile_parse =
        parser_parse(async_control_flow_compile_source, "test.tblo");
    if (!async_control_flow_compile_parse.error) {
        TypeCheckResult async_control_flow_compile_tc =
            typecheck(async_control_flow_compile_parse.program);
        if (!async_control_flow_compile_tc.error) {
            CompileResult async_control_flow_compile = compile(async_control_flow_compile_parse.program);
            const Chunk* branch_chunk =
                find_compiled_function_chunk(&async_control_flow_compile, "branchBefore");
            const Chunk* nested_chunk =
                find_compiled_function_chunk(&async_control_flow_compile, "nestedBlock");
            const Chunk* loop_chunk =
                find_compiled_function_chunk(&async_control_flow_compile, "loopAccumulate");
            const Chunk* continue_chunk =
                find_compiled_function_chunk(&async_control_flow_compile, "loopContinue");

            bool control_flow_ok =
                !async_control_flow_compile.error &&
                branch_chunk &&
                nested_chunk &&
                loop_chunk &&
                continue_chunk &&
                chunk_count_opcode(branch_chunk, OP_AWAIT) == 1 &&
                chunk_count_opcode(nested_chunk, OP_AWAIT) == 1 &&
                chunk_count_opcode(loop_chunk, OP_AWAIT) == 3 &&
                chunk_count_opcode(continue_chunk, OP_AWAIT) == 2 &&
                !chunk_contains_opcode(loop_chunk, OP_CONST16) &&
                !chunk_contains_opcode(continue_chunk, OP_CONST16);

            if (control_flow_ok) {
                tests_passed++;
                printf("  PASS: async control-flow lowering keeps await bytecode stable\n");
            } else {
                tests_failed++;
                printf("  FAIL: async control-flow lowering keeps await bytecode stable\n");
            }
        } else {
            tests_failed++;
            printf("  FAIL: async control-flow lowering typecheck\n");
        }
        symbol_table_free(async_control_flow_compile_tc.globals);
        error_free(async_control_flow_compile_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: async control-flow lowering parse\n");
    }
    parser_free_result(&async_control_flow_compile_parse);

    const char* async_defer_metadata_source =
        "async func guarded(task: Future[int]): int {\n"
        "    defer println(\"cleanup\");\n"
        "    return await task;\n"
        "}\n";
    ParseResult async_defer_metadata_parse = parser_parse(async_defer_metadata_source, "test.tblo");
    if (!async_defer_metadata_parse.error) {
        TypeCheckResult async_defer_metadata_tc = typecheck(async_defer_metadata_parse.program);
        if (!async_defer_metadata_tc.error) {
            CompileResult async_defer_metadata_compile = compile(async_defer_metadata_parse.program);
            bool metadata_ok = false;
            if (!async_defer_metadata_compile.error) {
                for (int i = 0; i < async_defer_metadata_compile.function_count; i++) {
                    ObjFunction* fn = async_defer_metadata_compile.functions
                        ? async_defer_metadata_compile.functions[i]
                        : NULL;
                    if (fn &&
                        fn->name &&
                        strcmp(fn->name, "guarded") == 0 &&
                        fn->is_async &&
                        fn->defer_handler_ip >= 0 &&
                        fn->defer_return_slot >= 0) {
                        metadata_ok = true;
                        break;
                    }
                }
            }
            if (metadata_ok) {
                tests_passed++;
                printf("  PASS: async defer metadata emitted for panic unwind\n");
            } else {
                tests_failed++;
                printf("  FAIL: async defer metadata emitted for panic unwind\n");
            }
        } else {
            tests_failed++;
            printf("  FAIL: async defer metadata typecheck\n");
        }
        symbol_table_free(async_defer_metadata_tc.globals);
        error_free(async_defer_metadata_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: async defer metadata parse\n");
    }
    parser_free_result(&async_defer_metadata_parse);

    const char* await_non_future_source =
        "async func bad(): int {\n"
        "    return await 1;\n"
        "}\n";
    ParseResult await_non_future_parse = parser_parse(await_non_future_source, "test.tblo");
    if (!await_non_future_parse.error) {
        TypeCheckResult await_non_future_tc = typecheck(await_non_future_parse.program);
        if (await_non_future_tc.error &&
            await_non_future_tc.error->message &&
            strstr(await_non_future_tc.error->message, "await expects Future<T>") != NULL) {
            tests_passed++;
            printf("  PASS: await non-future rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: await non-future rejection\n");
        }
        symbol_table_free(await_non_future_tc.globals);
        error_free(await_non_future_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: await non-future parse\n");
    }
    parser_free_result(&await_non_future_parse);

    const char* future_assignment_source =
        "async func load(): int { return 1; }\n"
        "func bad(): int {\n"
        "    var value: int = load();\n"
        "    return value;\n"
        "}\n";
    ParseResult future_assignment_parse = parser_parse(future_assignment_source, "test.tblo");
    if (!future_assignment_parse.error) {
        TypeCheckResult future_assignment_tc = typecheck(future_assignment_parse.program);
        if (future_assignment_tc.error &&
            future_assignment_tc.error->message &&
            strstr(future_assignment_tc.error->message, "expected int, got Future<int>") != NULL) {
            tests_passed++;
            printf("  PASS: missing await future-to-int mismatch\n");
        } else {
            tests_failed++;
            printf("  FAIL: missing await future-to-int mismatch\n");
        }
        symbol_table_free(future_assignment_tc.globals);
        error_free(future_assignment_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: future assignment parse\n");
    }
    parser_free_result(&future_assignment_parse);

    const char* async_sync_fn_mismatch_source =
        "async func load(): int { return 1; }\n"
        "func bad() {\n"
        "    var cb = func(): int { return 1; };\n"
        "    cb = load;\n"
        "}\n";
    ParseResult async_sync_fn_mismatch_parse =
        parser_parse(async_sync_fn_mismatch_source, "test.tblo");
    if (!async_sync_fn_mismatch_parse.error) {
        TypeCheckResult async_sync_fn_mismatch_tc =
            typecheck(async_sync_fn_mismatch_parse.program);
        if (async_sync_fn_mismatch_tc.error &&
            async_sync_fn_mismatch_tc.error->message &&
            strstr(async_sync_fn_mismatch_tc.error->message,
                   "expected func(): int, got func(): Future<int>") != NULL) {
            tests_passed++;
            printf("  PASS: async function rejected where sync function is required\n");
        } else {
            tests_failed++;
            printf("  FAIL: async function rejected where sync function is required\n");
        }
        symbol_table_free(async_sync_fn_mismatch_tc.globals);
        error_free(async_sync_fn_mismatch_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: async/sync function mismatch parse\n");
    }
    parser_free_result(&async_sync_fn_mismatch_parse);

    const char* future_builtin_source =
        "func buildPending(): Future[int] { return futurePending<int>(); }\n"
        "func buildReady(): Future[string] { return futureResolved(\"ok\"); }\n"
        "func complete(task: Future[int]): int {\n"
        "    var wasReady: bool = futureIsReady(task);\n"
        "    if (!wasReady) {\n"
        "        var completed: bool = futureComplete(task, 7);\n"
        "    }\n"
        "    return futureGet(task);\n"
        "}\n";
    ParseResult future_builtin_parse = parser_parse(future_builtin_source, "test.tblo");
    if (!future_builtin_parse.error) {
        TypeCheckResult future_builtin_tc = typecheck(future_builtin_parse.program);
        if (!future_builtin_tc.error) {
            CompileResult future_builtin_compile = compile(future_builtin_parse.program);
            if (!future_builtin_compile.error && future_builtin_compile.function) {
                tests_passed++;
                printf("  PASS: future runtime builtin signatures compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: future runtime builtin signatures compile\n");
            }
        } else {
            tests_failed++;
            printf("  FAIL: future runtime builtin signatures typecheck\n");
        }
        symbol_table_free(future_builtin_tc.globals);
        error_free(future_builtin_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: future runtime builtin signatures parse\n");
    }
    parser_free_result(&future_builtin_parse);

    const char* future_pending_inference_source =
        "func build(): Future[int] {\n"
        "    return futurePending();\n"
        "}\n";
    ParseResult future_pending_inference_parse =
        parser_parse(future_pending_inference_source, "test.tblo");
    if (!future_pending_inference_parse.error) {
        TypeCheckResult future_pending_inference_tc =
            typecheck(future_pending_inference_parse.program);
        Symbol* build_sym = symbol_table_get(future_pending_inference_tc.globals, "build");
        bool inferred_pending_ok =
            !future_pending_inference_tc.error &&
            build_sym &&
            build_sym->type &&
            build_sym->type->kind == TYPE_FUNCTION &&
            build_sym->type->return_type &&
            build_sym->type->return_type->kind == TYPE_FUTURE &&
            build_sym->type->return_type->element_type &&
            build_sym->type->return_type->element_type->kind == TYPE_INT;
        if (inferred_pending_ok) {
            tests_passed++;
            printf("  PASS: futurePending infers from contextual Future<T> return type\n");
        } else {
            tests_failed++;
            printf("  FAIL: futurePending infers from contextual Future<T> return type\n");
        }
        symbol_table_free(future_pending_inference_tc.globals);
        error_free(future_pending_inference_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: futurePending inference parse\n");
    }
    parser_free_result(&future_pending_inference_parse);

    const char* future_complete_mismatch_source =
        "func bad(task: Future[int]): bool {\n"
        "    return futureComplete(task, \"oops\");\n"
        "}\n";
    ParseResult future_complete_mismatch_parse =
        parser_parse(future_complete_mismatch_source, "test.tblo");
    if (!future_complete_mismatch_parse.error) {
        TypeCheckResult future_complete_mismatch_tc =
            typecheck(future_complete_mismatch_parse.program);
        if (future_complete_mismatch_tc.error &&
            future_complete_mismatch_tc.error->message &&
            strstr(future_complete_mismatch_tc.error->message,
                   "Generic inference mismatch for type parameter 'T'") != NULL &&
            strstr(future_complete_mismatch_tc.error->message, "futureComplete") != NULL) {
            tests_passed++;
            printf("  PASS: futureComplete enforces payload type consistency\n");
        } else {
            tests_failed++;
            printf("  FAIL: futureComplete enforces payload type consistency\n");
        }
        symbol_table_free(future_complete_mismatch_tc.globals);
        error_free(future_complete_mismatch_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: futureComplete mismatch parse\n");
    }
    parser_free_result(&future_complete_mismatch_parse);

    const char* async_sleep_builtin_source =
        "func schedule(): Future[void] {\n"
        "    return asyncSleep(0);\n"
        "}\n"
        "async func napThen(): int {\n"
        "    await asyncSleep(1);\n"
        "    return 7;\n"
        "}\n";
    ParseResult async_sleep_builtin_parse = parser_parse(async_sleep_builtin_source, "test.tblo");
    if (!async_sleep_builtin_parse.error) {
        TypeCheckResult async_sleep_builtin_tc = typecheck(async_sleep_builtin_parse.program);
        if (!async_sleep_builtin_tc.error) {
            CompileResult async_sleep_builtin_compile = compile(async_sleep_builtin_parse.program);
            if (!async_sleep_builtin_compile.error && async_sleep_builtin_compile.function) {
                tests_passed++;
                printf("  PASS: asyncSleep builtin signatures compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: asyncSleep builtin signatures compile\n");
            }
        } else {
            tests_failed++;
            printf("  FAIL: asyncSleep builtin signatures typecheck\n");
        }
        symbol_table_free(async_sleep_builtin_tc.globals);
        error_free(async_sleep_builtin_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: asyncSleep builtin signatures parse\n");
    }
    parser_free_result(&async_sleep_builtin_parse);

    const char* async_sleep_void_source =
        "async func bad(): int {\n"
        "    var value: int = await asyncSleep(1);\n"
        "    return value;\n"
        "}\n";
    ParseResult async_sleep_void_parse = parser_parse(async_sleep_void_source, "test.tblo");
    if (!async_sleep_void_parse.error) {
        TypeCheckResult async_sleep_void_tc = typecheck(async_sleep_void_parse.program);
        if (async_sleep_void_tc.error &&
            async_sleep_void_tc.error->message &&
            strstr(async_sleep_void_tc.error->message, "expected int, got void") != NULL) {
            tests_passed++;
            printf("  PASS: asyncSleep await yields void\n");
        } else {
            tests_failed++;
            printf("  FAIL: asyncSleep await yields void\n");
        }
        symbol_table_free(async_sleep_void_tc.globals);
        error_free(async_sleep_void_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: asyncSleep void parse\n");
    }
    parser_free_result(&async_sleep_void_parse);

    const char* async_channel_builtin_source =
        "func sendLater(channelId: int): Future[(bool, Error?)] {\n"
        "    return asyncChannelSend(channelId, 1 as any);\n"
        "}\n"
        "func sendLaterTyped(channelId: int, schema: any): Future[(bool, Error?)] {\n"
        "    return asyncChannelSendTyped(channelId, {\"x\": 1} as any, schema);\n"
        "}\n"
        "func recvLater(channelId: int): Future[(any, Error?)] {\n"
        "    return asyncChannelRecv(channelId);\n"
        "}\n"
        "func recvLaterTyped(channelId: int, schema: any): Future[(any, Error?)] {\n"
        "    return asyncChannelRecvTyped(channelId, schema);\n"
        "}\n"
        "async func flow(channelId: int): int {\n"
        "    var sent: (bool, Error?) = await asyncChannelSend(channelId, 7 as any);\n"
        "    var recv: (any, Error?) = await asyncChannelRecv(channelId);\n"
        "    if (sent.1 != nil || recv.1 != nil) {\n"
        "        return -1;\n"
        "    }\n"
        "    return recv.0 as int;\n"
        "}\n";
    ParseResult async_channel_builtin_parse = parser_parse(async_channel_builtin_source, "test.tblo");
    if (!async_channel_builtin_parse.error) {
        TypeCheckResult async_channel_builtin_tc = typecheck(async_channel_builtin_parse.program);
        if (!async_channel_builtin_tc.error) {
            CompileResult async_channel_builtin_compile = compile(async_channel_builtin_parse.program);
            if (!async_channel_builtin_compile.error && async_channel_builtin_compile.function) {
                tests_passed++;
                printf("  PASS: async channel builtin signatures compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: async channel builtin signatures compile\n");
            }
        } else {
            tests_failed++;
            printf("  FAIL: async channel builtin signatures typecheck\n");
        }
        symbol_table_free(async_channel_builtin_tc.globals);
        error_free(async_channel_builtin_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: async channel builtin signatures parse\n");
    }
    parser_free_result(&async_channel_builtin_parse);

    const char* async_channel_tuple_source =
        "async func bad(channelId: int): bool {\n"
        "    var sent: bool = await asyncChannelSend(channelId, 1 as any);\n"
        "    return sent;\n"
        "}\n";
    ParseResult async_channel_tuple_parse = parser_parse(async_channel_tuple_source, "test.tblo");
    if (!async_channel_tuple_parse.error) {
        TypeCheckResult async_channel_tuple_tc = typecheck(async_channel_tuple_parse.program);
        if (async_channel_tuple_tc.error &&
            async_channel_tuple_tc.error->message &&
            strstr(async_channel_tuple_tc.error->message, "expected bool, got (bool, record Error?)") != NULL) {
            tests_passed++;
            printf("  PASS: async channel await yields result tuple\n");
        } else {
            tests_failed++;
            printf("  FAIL: async channel await yields result tuple\n");
        }
        symbol_table_free(async_channel_tuple_tc.globals);
        error_free(async_channel_tuple_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: async channel tuple parse\n");
    }
    parser_free_result(&async_channel_tuple_parse);

    const char* async_stdlib_helper_source =
        "record AsyncAwaitAnyResult { index: int, ready: bool, timedOut: bool, value: any, error: Error? };\n"
        "record AsyncAwaitWithTimeoutResult { ready: bool, timedOut: bool, value: any, error: Error? };\n"
        "func asyncErr(message: string): Error {\n"
        "    var err: Error = { code: ERR_INVALID_ARGUMENT, message: message, data: nil };\n"
        "    return err;\n"
        "}\n"
        "func asyncAwaitAnyError(message: string): AsyncAwaitAnyResult {\n"
        "    var out: AsyncAwaitAnyResult = { index: -1, ready: false, timedOut: false, value: nil, error: asyncErr(message) };\n"
        "    return out;\n"
        "}\n"
        "func asyncAwaitWithTimeoutError(message: string): AsyncAwaitWithTimeoutResult {\n"
        "    var out: AsyncAwaitWithTimeoutResult = { ready: false, timedOut: false, value: nil, error: asyncErr(message) };\n"
        "    return out;\n"
        "}\n"
        "async func asyncAwaitAll[T](tasks: array<Future[T]>): array<T> {\n"
        "    var out: array<T> = [];\n"
        "    var i: int = 0;\n"
        "    while (i < len(tasks)) {\n"
        "        push(out, await tasks[i]);\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return out;\n"
        "}\n"
        "async func asyncAwaitAll2[A, B](first: Future[A], second: Future[B]): (A, B) {\n"
        "    return (await first, await second);\n"
        "}\n"
        "async func asyncAwaitAny[T](tasks: array<Future[T]>, pollIntervalMs: int): AsyncAwaitAnyResult {\n"
        "    if (pollIntervalMs < 1) {\n"
        "        return asyncAwaitAnyError(\"bad interval\");\n"
        "    }\n"
        "    if (len(tasks) == 0) {\n"
        "        return asyncAwaitAnyError(\"empty\");\n"
        "    }\n"
        "    while (true) {\n"
        "        var i: int = 0;\n"
        "        while (i < len(tasks)) {\n"
        "            if (futureIsReady(tasks[i])) {\n"
        "                var value: T = await tasks[i];\n"
        "                var ready: AsyncAwaitAnyResult = { index: i, ready: true, timedOut: false, value: value as any, error: nil };\n"
        "                return ready;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        await asyncSleep(pollIntervalMs);\n"
        "    }\n"
        "}\n"
        "async func asyncAwaitWithTimeout[T](task: Future[T], timeoutMs: int, pollIntervalMs: int): AsyncAwaitWithTimeoutResult {\n"
        "    if (timeoutMs < 0) {\n"
        "        return asyncAwaitWithTimeoutError(\"bad timeout\");\n"
        "    }\n"
        "    if (pollIntervalMs < 1) {\n"
        "        return asyncAwaitWithTimeoutError(\"bad interval\");\n"
        "    }\n"
        "    var remaining: int = timeoutMs;\n"
        "    while (true) {\n"
        "        if (futureIsReady(task)) {\n"
        "            var value: T = await task;\n"
        "            var ready: AsyncAwaitWithTimeoutResult = { ready: true, timedOut: false, value: value as any, error: nil };\n"
        "            return ready;\n"
        "        }\n"
        "        if (remaining == 0) {\n"
        "            return { ready: false, timedOut: true, value: nil, error: nil };\n"
        "        }\n"
        "        var step: int = pollIntervalMs;\n"
        "        if (step > remaining) {\n"
        "            step = remaining;\n"
        "        }\n"
        "        await asyncSleep(step);\n"
        "        remaining = remaining - step;\n"
        "    }\n"
        "}\n"
        "async func collect(tasks: array<Future[int]>): array<int> {\n"
        "    return await asyncAwaitAll(tasks);\n"
        "}\n"
        "async func pair(): (int, string) {\n"
        "    return await asyncAwaitAll2(futureResolved(7), futureResolved(\"z\"));\n"
        "}\n"
        "async func pick(tasks: array<Future[int]>): int {\n"
        "    var result: AsyncAwaitAnyResult = await asyncAwaitAny(tasks, 1);\n"
        "    if (result.error != nil) {\n"
        "        return -1;\n"
        "    }\n"
        "    return result.value as int;\n"
        "}\n"
        "async func pickTimeout(task: Future[int]): int {\n"
        "    var result: AsyncAwaitWithTimeoutResult = await asyncAwaitWithTimeout(task, 1, 1);\n"
        "    if (result.timedOut) {\n"
        "        return -1;\n"
        "    }\n"
        "    return result.value as int;\n"
        "}\n";
    ParseResult async_stdlib_helper_parse =
        parser_parse(async_stdlib_helper_source, "test.tblo");
    if (!async_stdlib_helper_parse.error) {
        TypeCheckResult async_stdlib_helper_tc =
            typecheck(async_stdlib_helper_parse.program);
        if (!async_stdlib_helper_tc.error) {
            CompileResult async_stdlib_helper_compile =
                compile(async_stdlib_helper_parse.program);
            if (!async_stdlib_helper_compile.error &&
                async_stdlib_helper_compile.function) {
                tests_passed++;
                printf("  PASS: async stdlib helper generics compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: async stdlib helper generics compile\n");
            }
        } else {
            tests_failed++;
            printf("  FAIL: async stdlib helper generics typecheck\n");
        }
        symbol_table_free(async_stdlib_helper_tc.globals);
        error_free(async_stdlib_helper_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: async stdlib helper generics parse\n");
    }
    parser_free_result(&async_stdlib_helper_parse);

    const char* alias_source =
        "type UserId = int;\n"
        "const DEFAULT_ID: UserId = 7;\n"
        "func main(): void {\n"
        "    var id: UserId = DEFAULT_ID;\n"
        "    println(id);\n"
        "}\n";
    ParseResult alias_parse = parser_parse(alias_source, "test.tblo");
    if (!alias_parse.error && alias_parse.program && alias_parse.program->stmt_count >= 2) {
        TypeCheckResult alias_tc = typecheck(alias_parse.program);
        if (!alias_tc.error) {
            tests_passed++;
            printf("  PASS: type alias + const typecheck\n");
        } else {
            tests_failed++;
            printf("  FAIL: type alias + const typecheck\n");
        }
        symbol_table_free(alias_tc.globals);
        error_free(alias_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: type alias + const parse\n");
    }
    parser_free_result(&alias_parse);

    const char* const_assign_source =
        "const LIMIT: int = 1;\n"
        "func main(): void {\n"
        "    LIMIT = 2;\n"
        "}\n";
    ParseResult const_assign_parse = parser_parse(const_assign_source, "test.tblo");
    if (!const_assign_parse.error) {
        TypeCheckResult const_assign_tc = typecheck(const_assign_parse.program);
        if (const_assign_tc.error &&
            const_assign_tc.error->message &&
            strstr(const_assign_tc.error->message, "immutable") != NULL) {
            tests_passed++;
            printf("  PASS: const assignment rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: const assignment rejection\n");
        }
        symbol_table_free(const_assign_tc.globals);
        error_free(const_assign_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: const assignment parse\n");
    }
    parser_free_result(&const_assign_parse);

    const char* const_non_constexpr_source =
        "var base: int = 2;\n"
        "const BAD: int = base + 1;\n";
    ParseResult const_non_constexpr_parse = parser_parse(const_non_constexpr_source, "test.tblo");
    if (!const_non_constexpr_parse.error) {
        TypeCheckResult const_non_constexpr_tc = typecheck(const_non_constexpr_parse.program);
        if (const_non_constexpr_tc.error &&
            const_non_constexpr_tc.error->message &&
            strstr(const_non_constexpr_tc.error->message, "compile-time constant") != NULL) {
            tests_passed++;
            printf("  PASS: const compile-time initializer enforcement\n");
        } else {
            tests_failed++;
            printf("  FAIL: const compile-time initializer enforcement\n");
        }
        symbol_table_free(const_non_constexpr_tc.globals);
        error_free(const_non_constexpr_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: const compile-time initializer parse\n");
    }
    parser_free_result(&const_non_constexpr_parse);

    const char* interpolation_source =
        "var name: string = \"TabloLang\";\n"
        "var msg: string = \"Hello ${name}, v=${1 + 2}\";\n";
    ParseResult interpolation_parse = parser_parse(interpolation_source, "test.tblo");
    if (!interpolation_parse.error) {
        TypeCheckResult interpolation_tc = typecheck(interpolation_parse.program);
        if (!interpolation_tc.error) {
            tests_passed++;
            printf("  PASS: string interpolation parse+typecheck\n");
        } else {
            tests_failed++;
            printf("  FAIL: string interpolation parse+typecheck\n");
        }
        symbol_table_free(interpolation_tc.globals);
        error_free(interpolation_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: string interpolation parse+typecheck\n");
    }
    parser_free_result(&interpolation_parse);

    const char* bad_interpolation_source = "var msg: string = \"Hello ${name\";";
    ParseResult bad_interpolation_parse = parser_parse(bad_interpolation_source, "test.tblo");
    if (bad_interpolation_parse.error &&
        bad_interpolation_parse.error->message &&
        strstr(bad_interpolation_parse.error->message, "Unterminated string interpolation") != NULL) {
        tests_passed++;
        printf("  PASS: unterminated string interpolation error\n");
    } else {
        tests_failed++;
        printf("  FAIL: unterminated string interpolation error\n");
    }
    parser_free_result(&bad_interpolation_parse);

    const char* escaped_interpolation_source = "var msg: string = \"Price: \\${cost}\";";
    ParseResult escaped_interpolation_parse = parser_parse(escaped_interpolation_source, "test.tblo");
    if (!escaped_interpolation_parse.error) {
        TypeCheckResult escaped_interpolation_tc = typecheck(escaped_interpolation_parse.program);
        if (!escaped_interpolation_tc.error) {
            tests_passed++;
            printf("  PASS: escaped interpolation remains literal\n");
        } else {
            tests_failed++;
            printf("  FAIL: escaped interpolation remains literal\n");
        }
        symbol_table_free(escaped_interpolation_tc.globals);
        error_free(escaped_interpolation_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: escaped interpolation remains literal\n");
    }
    parser_free_result(&escaped_interpolation_parse);

    const char* expected_got_source = "var value: int = \"oops\";";
    ParseResult expected_got_parse = parser_parse(expected_got_source, "test.tblo");
    if (!expected_got_parse.error) {
        TypeCheckResult expected_got_tc = typecheck(expected_got_parse.program);
        if (expected_got_tc.error &&
            expected_got_tc.error->message &&
            strstr(expected_got_tc.error->message, "expected int, got string") != NULL) {
            tests_passed++;
            printf("  PASS: type mismatch expected/got diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: type mismatch expected/got diagnostics\n");
        }
        symbol_table_free(expected_got_tc.globals);
        error_free(expected_got_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: type mismatch expected/got diagnostics parse\n");
    }
    parser_free_result(&expected_got_parse);

    const char* arg_count_source =
        "func add(a: int, b: int): int { return a + b; }\n"
        "func main(): void {\n"
        "    var value: int = add(1);\n"
        "}\n";
    ParseResult arg_count_parse = parser_parse(arg_count_source, "test.tblo");
    if (!arg_count_parse.error) {
        TypeCheckResult arg_count_tc = typecheck(arg_count_parse.program);
        if (arg_count_tc.error &&
            arg_count_tc.error->message &&
            strstr(arg_count_tc.error->message, "Wrong number of arguments: expected 2, got 1") != NULL) {
            tests_passed++;
            printf("  PASS: argument count expected/got diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: argument count expected/got diagnostics\n");
        }
        symbol_table_free(arg_count_tc.globals);
        error_free(arg_count_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: argument count expected/got diagnostics parse\n");
    }
    parser_free_result(&arg_count_parse);

    const char* visibility_same_file_source =
        "private const SECRET: int = 7;\n"
        "var localUse: int = SECRET;\n";
    ParseResult visibility_same_file_parse = parser_parse(visibility_same_file_source, "same.tblo");
    if (!visibility_same_file_parse.error) {
        TypeCheckResult visibility_same_file_tc = typecheck(visibility_same_file_parse.program);
        if (!visibility_same_file_tc.error) {
            tests_passed++;
            printf("  PASS: private symbol allowed within same file\n");
        } else {
            tests_failed++;
            printf("  FAIL: private symbol allowed within same file\n");
        }
        symbol_table_free(visibility_same_file_tc.globals);
        error_free(visibility_same_file_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: private symbol same-file parse\n");
    }
    parser_free_result(&visibility_same_file_parse);

    const char* visibility_lib_source =
        "private const SECRET: int = 7;\n"
        "public const OPEN: int = 11;\n";
    const char* visibility_main_source =
        "var ok: int = OPEN;\n"
        "var denied: int = SECRET;\n";
    ParseResult visibility_lib_parse = parser_parse(visibility_lib_source, "lib.tblo");
    ParseResult visibility_main_parse = parser_parse(visibility_main_source, "main.tblo");
    if (!visibility_lib_parse.error && !visibility_main_parse.error) {
        Program* combined = program_create("main.tblo");
        for (int i = 0; i < visibility_lib_parse.program->stmt_count; i++) {
            program_add_stmt(combined, stmt_clone(visibility_lib_parse.program->statements[i]));
        }
        for (int i = 0; i < visibility_main_parse.program->stmt_count; i++) {
            program_add_stmt(combined, stmt_clone(visibility_main_parse.program->statements[i]));
        }

        TypeCheckResult visibility_cross_file_tc = typecheck(combined);
        if (visibility_cross_file_tc.error &&
            visibility_cross_file_tc.error->message &&
            strstr(visibility_cross_file_tc.error->message, "private to its module") != NULL) {
            tests_passed++;
            printf("  PASS: private symbol blocked across files\n");
        } else {
            tests_failed++;
            printf("  FAIL: private symbol blocked across files\n");
        }
        symbol_table_free(visibility_cross_file_tc.globals);
        error_free(visibility_cross_file_tc.error);
        program_free(combined);
    } else {
        tests_failed++;
        printf("  FAIL: visibility cross-file parse\n");
    }
    parser_free_result(&visibility_lib_parse);
    parser_free_result(&visibility_main_parse);

    const char* interface_ok_source =
        "record Point { x: int, y: int };\n"
        "interface Mover {\n"
        "    move(dx: int, dy: int): int;\n"
        "};\n"
        "func move(p: Point, dx: int, dy: int): int {\n"
        "    return p.x + p.y + dx + dy;\n"
        "}\n"
        "func takesMover(m: Mover): int {\n"
        "    return 1;\n"
        "}\n"
        "func main(): void {\n"
        "    var p: Point = { x: 1, y: 2 };\n"
        "    var n: int = takesMover(p);\n"
        "}\n";
    ParseResult interface_ok_parse = parser_parse(interface_ok_source, "test.tblo");
    if (!interface_ok_parse.error) {
        TypeCheckResult interface_ok_tc = typecheck(interface_ok_parse.program);
        if (!interface_ok_tc.error) {
            CompileResult interface_ok_compile = compile(interface_ok_parse.program);
            if (!interface_ok_compile.error && interface_ok_compile.function) {
                tests_passed++;
                printf("  PASS: interface declaration and record conformance\n");
            } else {
                tests_failed++;
                printf("  FAIL: interface declaration and record conformance\n");
            }

            for (int i = 0; i < interface_ok_compile.function_count; i++) {
                if (interface_ok_compile.functions && interface_ok_compile.functions[i]) {
                    obj_function_free(interface_ok_compile.functions[i]);
                }
            }
            if (interface_ok_compile.functions) free(interface_ok_compile.functions);
            if (interface_ok_compile.function) obj_function_free(interface_ok_compile.function);
            symbol_table_free(interface_ok_compile.globals);
            error_free(interface_ok_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: interface declaration and record conformance\n");
        }
        symbol_table_free(interface_ok_tc.globals);
        error_free(interface_ok_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: interface declaration parse\n");
    }
    parser_free_result(&interface_ok_parse);

    const char* interface_impl_mapping_source =
        "record Point { x: int, y: int };\n"
        "interface Mover {\n"
        "    move(dx: int, dy: int): int;\n"
        "};\n"
        "func pointMove(p: Point, dx: int, dy: int): int {\n"
        "    return p.x + p.y + dx + dy;\n"
        "}\n"
        "impl Mover as Point {\n"
        "    move = pointMove;\n"
        "};\n"
        "func takesMover(m: Mover): int {\n"
        "    return 1;\n"
        "}\n"
        "func main(): void {\n"
        "    var p: Point = { x: 1, y: 2 };\n"
        "    var n: int = takesMover(p);\n"
        "}\n";
    ParseResult interface_impl_mapping_parse = parser_parse(interface_impl_mapping_source, "test.tblo");
    if (!interface_impl_mapping_parse.error) {
        TypeCheckResult interface_impl_mapping_tc = typecheck(interface_impl_mapping_parse.program);
        if (!interface_impl_mapping_tc.error) {
            CompileResult interface_impl_mapping_compile = compile(interface_impl_mapping_parse.program);
            if (!interface_impl_mapping_compile.error && interface_impl_mapping_compile.function) {
                tests_passed++;
                printf("  PASS: explicit impl mapping for interface conformance\n");
            } else {
                tests_failed++;
                printf("  FAIL: explicit impl mapping for interface conformance\n");
            }

            for (int i = 0; i < interface_impl_mapping_compile.function_count; i++) {
                if (interface_impl_mapping_compile.functions && interface_impl_mapping_compile.functions[i]) {
                    obj_function_free(interface_impl_mapping_compile.functions[i]);
                }
            }
            if (interface_impl_mapping_compile.functions) free(interface_impl_mapping_compile.functions);
            if (interface_impl_mapping_compile.function) obj_function_free(interface_impl_mapping_compile.function);
            symbol_table_free(interface_impl_mapping_compile.globals);
            error_free(interface_impl_mapping_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: explicit impl mapping for interface conformance\n");
        }
        symbol_table_free(interface_impl_mapping_tc.globals);
        error_free(interface_impl_mapping_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: explicit impl mapping parse\n");
    }
    parser_free_result(&interface_impl_mapping_parse);

    const char* generic_impl_mapping_source =
        "record Box[T] { value: T };\n"
        "interface Formatter {\n"
        "    format(): string;\n"
        "};\n"
        "func boxRender[T](b: Box[T]): string {\n"
        "    return \"box\";\n"
        "}\n"
        "impl Formatter as Box[T] {\n"
        "    format = boxRender;\n"
        "};\n"
        "func useFormatter(f: Formatter): string {\n"
        "    return f.format();\n"
        "}\n"
        "func main(): void {\n"
        "    var i: Box[int] = { value: 7 };\n"
        "    var s: string = useFormatter(i);\n"
        "    println(s);\n"
        "}\n";
    ParseResult generic_impl_mapping_parse = parser_parse(generic_impl_mapping_source, "test.tblo");
    if (!generic_impl_mapping_parse.error) {
        TypeCheckResult generic_impl_mapping_tc = typecheck(generic_impl_mapping_parse.program);
        if (!generic_impl_mapping_tc.error) {
            CompileResult generic_impl_mapping_compile = compile(generic_impl_mapping_parse.program);
            bool has_interface_call = false;
            if (!generic_impl_mapping_compile.error && generic_impl_mapping_compile.function) {
                if (chunk_contains_opcode(&generic_impl_mapping_compile.function->chunk, OP_CALL_INTERFACE)) {
                    has_interface_call = true;
                }
                for (int i = 0; i < generic_impl_mapping_compile.function_count; i++) {
                    if (generic_impl_mapping_compile.functions &&
                        generic_impl_mapping_compile.functions[i] &&
                        chunk_contains_opcode(&generic_impl_mapping_compile.functions[i]->chunk, OP_CALL_INTERFACE)) {
                        has_interface_call = true;
                        break;
                    }
                }
            }

            if (!generic_impl_mapping_compile.error &&
                generic_impl_mapping_compile.function &&
                has_interface_call) {
                tests_passed++;
                printf("  PASS: generic impl mapping for record specializations\n");
            } else {
                tests_failed++;
                printf("  FAIL: generic impl mapping for record specializations\n");
            }

            for (int i = 0; i < generic_impl_mapping_compile.function_count; i++) {
                if (generic_impl_mapping_compile.functions && generic_impl_mapping_compile.functions[i]) {
                    obj_function_free(generic_impl_mapping_compile.functions[i]);
                }
            }
            if (generic_impl_mapping_compile.functions) free(generic_impl_mapping_compile.functions);
            if (generic_impl_mapping_compile.function) obj_function_free(generic_impl_mapping_compile.function);
            symbol_table_free(generic_impl_mapping_compile.globals);
            error_free(generic_impl_mapping_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: generic impl mapping for record specializations\n");
        }
        symbol_table_free(generic_impl_mapping_tc.globals);
        error_free(generic_impl_mapping_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic impl mapping parse\n");
    }
    parser_free_result(&generic_impl_mapping_parse);

    const char* generic_impl_bad_signature_source =
        "record Box[T] { value: T };\n"
        "interface Formatter {\n"
        "    format(): string;\n"
        "};\n"
        "func boxRenderStringOnly(b: Box[string]): string {\n"
        "    return b.value;\n"
        "}\n"
        "impl Formatter as Box[T] {\n"
        "    format = boxRenderStringOnly;\n"
        "};\n";
    ParseResult generic_impl_bad_signature_parse = parser_parse(generic_impl_bad_signature_source, "test.tblo");
    if (!generic_impl_bad_signature_parse.error) {
        TypeCheckResult generic_impl_bad_signature_tc = typecheck(generic_impl_bad_signature_parse.program);
        if (generic_impl_bad_signature_tc.error &&
            generic_impl_bad_signature_tc.error->message &&
            strstr(generic_impl_bad_signature_tc.error->message, "does not match interface method") != NULL) {
            tests_passed++;
            printf("  PASS: generic impl signature mismatch rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: generic impl signature mismatch rejection\n");
        }
        symbol_table_free(generic_impl_bad_signature_tc.globals);
        error_free(generic_impl_bad_signature_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic impl signature mismatch parse\n");
    }
    parser_free_result(&generic_impl_bad_signature_parse);

    const char* interface_impl_missing_map_source =
        "record Point { x: int, y: int };\n"
        "interface Mover {\n"
        "    move(dx: int, dy: int): int;\n"
        "};\n"
        "impl Mover as Point { };\n";
    ParseResult interface_impl_missing_map_parse = parser_parse(interface_impl_missing_map_source, "test.tblo");
    if (!interface_impl_missing_map_parse.error) {
        TypeCheckResult interface_impl_missing_map_tc = typecheck(interface_impl_missing_map_parse.program);
        if (interface_impl_missing_map_tc.error &&
            interface_impl_missing_map_tc.error->message &&
            strstr(interface_impl_missing_map_tc.error->message, "missing mapping") != NULL) {
            tests_passed++;
            printf("  PASS: impl missing mapping rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: impl missing mapping rejection\n");
        }
        symbol_table_free(interface_impl_missing_map_tc.globals);
        error_free(interface_impl_missing_map_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: impl missing mapping parse\n");
    }
    parser_free_result(&interface_impl_missing_map_parse);

    const char* interface_impl_bad_signature_source =
        "record Point { x: int, y: int };\n"
        "interface Mover {\n"
        "    move(dx: int, dy: int): int;\n"
        "};\n"
        "func pointMoveBad(p: Point, dx: int): int {\n"
        "    return p.x + dx;\n"
        "}\n"
        "impl Mover as Point {\n"
        "    move = pointMoveBad;\n"
        "};\n";
    ParseResult interface_impl_bad_signature_parse = parser_parse(interface_impl_bad_signature_source, "test.tblo");
    if (!interface_impl_bad_signature_parse.error) {
        TypeCheckResult interface_impl_bad_signature_tc = typecheck(interface_impl_bad_signature_parse.program);
        if (interface_impl_bad_signature_tc.error &&
            interface_impl_bad_signature_tc.error->message &&
            strstr(interface_impl_bad_signature_tc.error->message, "does not match interface method") != NULL) {
            tests_passed++;
            printf("  PASS: impl signature mismatch rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: impl signature mismatch rejection\n");
        }
        symbol_table_free(interface_impl_bad_signature_tc.globals);
        error_free(interface_impl_bad_signature_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: impl signature mismatch parse\n");
    }
    parser_free_result(&interface_impl_bad_signature_parse);

    const char* interface_missing_method_source =
        "record Point { x: int, y: int };\n"
        "interface Formatter {\n"
        "    format(): string;\n"
        "};\n"
        "func takesFormatter(f: Formatter): int {\n"
        "    return 1;\n"
        "}\n"
        "func main(): void {\n"
        "    var p: Point = { x: 1, y: 2 };\n"
        "    var n: int = takesFormatter(p);\n"
        "}\n";
    ParseResult interface_missing_method_parse = parser_parse(interface_missing_method_source, "test.tblo");
    if (!interface_missing_method_parse.error) {
        TypeCheckResult interface_missing_method_tc = typecheck(interface_missing_method_parse.program);
        if (interface_missing_method_tc.error &&
            interface_missing_method_tc.error->message &&
            strstr(interface_missing_method_tc.error->message, "Argument type mismatch") != NULL) {
            tests_passed++;
            printf("  PASS: interface conformance rejection when method missing\n");
        } else {
            tests_failed++;
            printf("  FAIL: interface conformance rejection when method missing\n");
        }
        symbol_table_free(interface_missing_method_tc.globals);
        error_free(interface_missing_method_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: interface missing method parse\n");
    }
    parser_free_result(&interface_missing_method_parse);

    const char* interface_method_dispatch_source =
        "record Point { x: int, y: int };\n"
        "interface Mover {\n"
        "    move(dx: int, dy: int): int;\n"
        "};\n"
        "func move(p: Point, dx: int, dy: int): int {\n"
        "    return p.x + p.y + dx + dy;\n"
        "}\n"
        "func useMover(m: Mover): int {\n"
        "    return m.move(1, 2);\n"
        "}\n"
        "func main(): void {\n"
        "    var p: Point = { x: 1, y: 2 };\n"
        "    var out: int = useMover(p);\n"
        "}\n";
    ParseResult interface_method_dispatch_parse = parser_parse(interface_method_dispatch_source, "test.tblo");
    if (!interface_method_dispatch_parse.error) {
        TypeCheckResult interface_method_dispatch_tc = typecheck(interface_method_dispatch_parse.program);
        if (!interface_method_dispatch_tc.error) {
            CompileResult interface_method_dispatch_compile = compile(interface_method_dispatch_parse.program);
            bool has_interface_call = false;
            if (!interface_method_dispatch_compile.error && interface_method_dispatch_compile.function) {
                if (chunk_contains_opcode(&interface_method_dispatch_compile.function->chunk, OP_CALL_INTERFACE)) {
                    has_interface_call = true;
                }
                for (int i = 0; i < interface_method_dispatch_compile.function_count; i++) {
                    if (interface_method_dispatch_compile.functions &&
                        interface_method_dispatch_compile.functions[i] &&
                        chunk_contains_opcode(&interface_method_dispatch_compile.functions[i]->chunk, OP_CALL_INTERFACE)) {
                        has_interface_call = true;
                        break;
                    }
                }
            }

            if (!interface_method_dispatch_compile.error &&
                interface_method_dispatch_compile.function &&
                has_interface_call) {
                tests_passed++;
                printf("  PASS: interface receiver method syntax dispatch\n");
            } else {
                tests_failed++;
                printf("  FAIL: interface receiver method syntax dispatch\n");
            }

            for (int i = 0; i < interface_method_dispatch_compile.function_count; i++) {
                if (interface_method_dispatch_compile.functions && interface_method_dispatch_compile.functions[i]) {
                    obj_function_free(interface_method_dispatch_compile.functions[i]);
                }
            }
            if (interface_method_dispatch_compile.functions) free(interface_method_dispatch_compile.functions);
            if (interface_method_dispatch_compile.function) obj_function_free(interface_method_dispatch_compile.function);
            symbol_table_free(interface_method_dispatch_compile.globals);
            error_free(interface_method_dispatch_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: interface receiver method syntax dispatch\n");
        }
        symbol_table_free(interface_method_dispatch_tc.globals);
        error_free(interface_method_dispatch_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: interface receiver method syntax dispatch parse\n");
    }
    parser_free_result(&interface_method_dispatch_parse);

    const char* func_literal_source =
        "var add = func(a: int, b: int): int { return a + b; };\n"
        "var answer: int = add(40, 2);\n"
        "var direct: int = (func(v: int): int { return v; })(answer);\n";
    ParseResult func_literal_parse = parser_parse(func_literal_source, "test.tblo");
    if (!func_literal_parse.error) {
        TypeCheckResult func_literal_tc = typecheck(func_literal_parse.program);
        if (!func_literal_tc.error) {
            CompileResult func_literal_compile = compile(func_literal_parse.program);
            if (!func_literal_compile.error && func_literal_compile.function) {
                tests_passed++;
                printf("  PASS: anonymous function literal parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: anonymous function literal parse+typecheck+compile\n");
            }

            for (int i = 0; i < func_literal_compile.function_count; i++) {
                if (func_literal_compile.functions && func_literal_compile.functions[i]) {
                    obj_function_free(func_literal_compile.functions[i]);
                }
            }
            if (func_literal_compile.functions) free(func_literal_compile.functions);
            if (func_literal_compile.function) obj_function_free(func_literal_compile.function);
            symbol_table_free(func_literal_compile.globals);
            error_free(func_literal_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: anonymous function literal parse+typecheck+compile\n");
        }
        symbol_table_free(func_literal_tc.globals);
        error_free(func_literal_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: anonymous function literal parse+typecheck+compile\n");
    }
    parser_free_result(&func_literal_parse);

    const char* func_capture_source =
        "func maker(base: int): int {\n"
        "    var x: int = base;\n"
        "    var f = func(y: int): int { return x + y; };\n"
        "    return f(1);\n"
        "}\n";
    ParseResult func_capture_parse = parser_parse(func_capture_source, "test.tblo");
    if (!func_capture_parse.error) {
        TypeCheckResult func_capture_tc = typecheck(func_capture_parse.program);
        if (!func_capture_tc.error) {
            CompileResult func_capture_compile = compile(func_capture_parse.program);
            bool has_make_closure = false;
            if (!func_capture_compile.error && func_capture_compile.function) {
                if (chunk_contains_opcode(&func_capture_compile.function->chunk, OP_MAKE_CLOSURE)) {
                    has_make_closure = true;
                }
                for (int i = 0; i < func_capture_compile.function_count; i++) {
                    if (func_capture_compile.functions &&
                        func_capture_compile.functions[i] &&
                        chunk_contains_opcode(&func_capture_compile.functions[i]->chunk, OP_MAKE_CLOSURE)) {
                        has_make_closure = true;
                        break;
                    }
                }
            }

            if (!func_capture_compile.error && func_capture_compile.function && has_make_closure) {
                tests_passed++;
                printf("  PASS: function literal captures outer locals\n");
            } else {
                tests_failed++;
                printf("  FAIL: function literal captures outer locals\n");
            }

            for (int i = 0; i < func_capture_compile.function_count; i++) {
                if (func_capture_compile.functions && func_capture_compile.functions[i]) {
                    obj_function_free(func_capture_compile.functions[i]);
                }
            }
            if (func_capture_compile.functions) free(func_capture_compile.functions);
            if (func_capture_compile.function) obj_function_free(func_capture_compile.function);
            symbol_table_free(func_capture_compile.globals);
            error_free(func_capture_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: function literal captures outer locals\n");
        }
        symbol_table_free(func_capture_tc.globals);
        error_free(func_capture_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: function literal capture parse\n");
    }
    parser_free_result(&func_capture_parse);

    const char* range_loop_source =
        "func sumRange(n: int): int {\n"
        "    var total: int = 0;\n"
        "    foreach (i in 0..n) {\n"
        "        if (i == 2) { continue; }\n"
        "        if (i == 4) { break; }\n"
        "        total = total + i;\n"
        "    }\n"
        "    return total;\n"
        "}\n";
    ParseResult range_loop_parse = parser_parse(range_loop_source, "test.tblo");
    if (!range_loop_parse.error) {
        bool parsed_for_range = false;
        if (range_loop_parse.program &&
            range_loop_parse.program->stmt_count > 0 &&
            range_loop_parse.program->statements[0] &&
            range_loop_parse.program->statements[0]->kind == STMT_FUNC_DECL &&
            range_loop_parse.program->statements[0]->func_decl.body &&
            range_loop_parse.program->statements[0]->func_decl.body->kind == STMT_BLOCK &&
            range_loop_parse.program->statements[0]->func_decl.body->block.stmt_count > 1 &&
            range_loop_parse.program->statements[0]->func_decl.body->block.statements[1] &&
            range_loop_parse.program->statements[0]->func_decl.body->block.statements[1]->kind == STMT_FOR_RANGE) {
            parsed_for_range = true;
        }

        TypeCheckResult range_loop_tc = typecheck(range_loop_parse.program);
        if (!range_loop_tc.error && parsed_for_range) {
            CompileResult range_loop_compile = compile(range_loop_parse.program);
            if (!range_loop_compile.error && range_loop_compile.function) {
                tests_passed++;
                printf("  PASS: foreach range loop parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: foreach range loop parse+typecheck+compile\n");
            }

            for (int i = 0; i < range_loop_compile.function_count; i++) {
                if (range_loop_compile.functions && range_loop_compile.functions[i]) {
                    obj_function_free(range_loop_compile.functions[i]);
                }
            }
            if (range_loop_compile.functions) free(range_loop_compile.functions);
            if (range_loop_compile.function) obj_function_free(range_loop_compile.function);
            symbol_table_free(range_loop_compile.globals);
            error_free(range_loop_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: foreach range loop parse+typecheck+compile\n");
        }
        symbol_table_free(range_loop_tc.globals);
        error_free(range_loop_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: foreach range loop parse+typecheck+compile\n");
    }
    parser_free_result(&range_loop_parse);

    const char* method_syntax_source =
        "record Point { x: int, y: int };\n"
        "func translate(p: Point, dx: int, dy: int): Point {\n"
        "    return { x: p.x + dx, y: p.y + dy };\n"
        "}\n"
        "func magnitudeSquared(p: Point): int {\n"
        "    return p.x * p.x + p.y * p.y;\n"
        "}\n"
        "func main(): void {\n"
        "    var p: Point = { x: 3, y: 4 };\n"
        "    var moved: Point = p.translate(1, 2);\n"
        "    var mag: int = moved.magnitudeSquared();\n"
        "    println(str(mag));\n"
        "}\n";
    ParseResult method_syntax_parse = parser_parse(method_syntax_source, "test.tblo");
    if (!method_syntax_parse.error) {
        TypeCheckResult method_syntax_tc = typecheck(method_syntax_parse.program);
        if (!method_syntax_tc.error) {
            CompileResult method_syntax_compile = compile(method_syntax_parse.program);
            if (!method_syntax_compile.error && method_syntax_compile.function) {
                tests_passed++;
                printf("  PASS: method syntax parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: method syntax parse+typecheck+compile\n");
            }

            for (int i = 0; i < method_syntax_compile.function_count; i++) {
                if (method_syntax_compile.functions && method_syntax_compile.functions[i]) {
                    obj_function_free(method_syntax_compile.functions[i]);
                }
            }
            if (method_syntax_compile.functions) free(method_syntax_compile.functions);
            if (method_syntax_compile.function) obj_function_free(method_syntax_compile.function);
            symbol_table_free(method_syntax_compile.globals);
            error_free(method_syntax_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: method syntax parse+typecheck+compile\n");
        }
        symbol_table_free(method_syntax_tc.globals);
        error_free(method_syntax_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: method syntax parse+typecheck+compile\n");
    }
    parser_free_result(&method_syntax_parse);

    const char* keyword_field_source =
        "record KeyPair {\n"
        "    public: int,\n"
        "    private: int\n"
        "};\n"
        "func readPublic(k: KeyPair): int {\n"
        "    return k.public;\n"
        "}\n"
        "func main(): void {\n"
        "    var k: KeyPair = { public: 7, private: 11 };\n"
        "    println(readPublic(k) == 7);\n"
        "}\n";
    ParseResult keyword_field_parse = parser_parse(keyword_field_source, "test.tblo");
    if (!keyword_field_parse.error) {
        TypeCheckResult keyword_field_tc = typecheck(keyword_field_parse.program);
        if (!keyword_field_tc.error) {
            CompileResult keyword_field_compile = compile(keyword_field_parse.program);
            if (!keyword_field_compile.error && keyword_field_compile.function) {
                tests_passed++;
                printf("  PASS: keyword-like record field names parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: keyword-like record field names parse+typecheck+compile\n");
            }

            for (int i = 0; i < keyword_field_compile.function_count; i++) {
                if (keyword_field_compile.functions && keyword_field_compile.functions[i]) {
                    obj_function_free(keyword_field_compile.functions[i]);
                }
            }
            if (keyword_field_compile.functions) free(keyword_field_compile.functions);
            if (keyword_field_compile.function) obj_function_free(keyword_field_compile.function);
            symbol_table_free(keyword_field_compile.globals);
            error_free(keyword_field_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: keyword-like record field names parse+typecheck+compile\n");
        }
        symbol_table_free(keyword_field_tc.globals);
        error_free(keyword_field_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: keyword-like record field names parse+typecheck+compile\n");
    }
    parser_free_result(&keyword_field_parse);

    const char* generic_func_source =
        "func identity[T](value: T): T {\n"
        "    return value;\n"
        "}\n"
        "func first[T](items: array<T>): T {\n"
        "    return items[0];\n"
        "}\n"
        "func main(): void {\n"
        "    var a: int = identity(42);\n"
        "    var b: string = identity(\"ok\");\n"
        "    var c: int = identity<int>(11);\n"
        "    var d: string = identity<string>(\"explicit\");\n"
        "    var nums: array<int> = [1, 2, 3];\n"
        "    var f: int = first(nums);\n"
        "    var g: int = first<int>(nums);\n"
        "    println(str(a) + b + str(c) + d + str(f + g));\n"
        "}\n";
    ParseResult generic_func_parse = parser_parse(generic_func_source, "test.tblo");
    if (!generic_func_parse.error) {
        TypeCheckResult generic_func_tc = typecheck(generic_func_parse.program);
        if (!generic_func_tc.error) {
            CompileResult generic_func_compile = compile(generic_func_parse.program);
            if (!generic_func_compile.error && generic_func_compile.function) {
                tests_passed++;
                printf("  PASS: generic function declaration/inference parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: generic function declaration/inference parse+typecheck+compile\n");
            }

            for (int i = 0; i < generic_func_compile.function_count; i++) {
                if (generic_func_compile.functions && generic_func_compile.functions[i]) {
                    obj_function_free(generic_func_compile.functions[i]);
                }
            }
            if (generic_func_compile.functions) free(generic_func_compile.functions);
            if (generic_func_compile.function) obj_function_free(generic_func_compile.function);
            symbol_table_free(generic_func_compile.globals);
            error_free(generic_func_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: generic function declaration/inference parse+typecheck+compile\n");
        }
        symbol_table_free(generic_func_tc.globals);
        error_free(generic_func_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic function declaration/inference parse+typecheck+compile\n");
    }
    parser_free_result(&generic_func_parse);

    const char* generic_infer_error_source =
        "func keep[T, U](value: T): T {\n"
        "    return value;\n"
        "}\n"
        "func main(): void {\n"
        "    var x: int = keep(1);\n"
        "    println(str(x));\n"
        "}\n";
    ParseResult generic_infer_error_parse = parser_parse(generic_infer_error_source, "test.tblo");
    if (!generic_infer_error_parse.error) {
        TypeCheckResult generic_infer_error_tc = typecheck(generic_infer_error_parse.program);
        if (generic_infer_error_tc.error &&
            generic_infer_error_tc.error->message &&
            strstr(generic_infer_error_tc.error->message, "Cannot infer generic type parameter(s) U") != NULL &&
            strstr(generic_infer_error_tc.error->message, "call to 'keep'") != NULL &&
            strstr(generic_infer_error_tc.error->message, "keep<T, U>(...)") != NULL) {
            tests_passed++;
            printf("  PASS: generic inference failure diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: generic inference failure diagnostics\n");
        }
        symbol_table_free(generic_infer_error_tc.globals);
        error_free(generic_infer_error_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic inference failure parse\n");
    }
    parser_free_result(&generic_infer_error_parse);

    const char* generic_infer_mismatch_source =
        "func pickEither[T](a: T, b: T): T {\n"
        "    return a;\n"
        "}\n"
        "func main(): void {\n"
        "    var x: int = pickEither(1, \"oops\");\n"
        "    println(str(x));\n"
        "}\n";
    ParseResult generic_infer_mismatch_parse = parser_parse(generic_infer_mismatch_source, "test.tblo");
    if (!generic_infer_mismatch_parse.error) {
        TypeCheckResult generic_infer_mismatch_tc = typecheck(generic_infer_mismatch_parse.program);
        if (generic_infer_mismatch_tc.error &&
            generic_infer_mismatch_tc.error->message &&
            strstr(generic_infer_mismatch_tc.error->message, "Generic inference mismatch for type parameter 'T'") != NULL &&
            strstr(generic_infer_mismatch_tc.error->message, "argument #2 of call to 'pickEither'") != NULL &&
            strstr(generic_infer_mismatch_tc.error->message, "inferred both int and string") != NULL) {
            tests_passed++;
            printf("  PASS: generic inference mismatch diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: generic inference mismatch diagnostics\n");
        }
        symbol_table_free(generic_infer_mismatch_tc.globals);
        error_free(generic_infer_mismatch_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic inference mismatch parse\n");
    }
    parser_free_result(&generic_infer_mismatch_parse);

    const char* generic_explicit_arity_error_source =
        "func pick[T](value: T): T {\n"
        "    return value;\n"
        "}\n"
        "func main(): void {\n"
        "    var x: int = pick<int, string>(1);\n"
        "    println(str(x));\n"
        "}\n";
    ParseResult generic_explicit_arity_error_parse = parser_parse(generic_explicit_arity_error_source, "test.tblo");
    if (!generic_explicit_arity_error_parse.error) {
        TypeCheckResult generic_explicit_arity_error_tc = typecheck(generic_explicit_arity_error_parse.program);
        if (generic_explicit_arity_error_tc.error &&
            generic_explicit_arity_error_tc.error->message &&
            strstr(generic_explicit_arity_error_tc.error->message, "Wrong number of generic type arguments") != NULL) {
            tests_passed++;
            printf("  PASS: explicit generic type argument arity diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: explicit generic type argument arity diagnostics\n");
        }
        symbol_table_free(generic_explicit_arity_error_tc.globals);
        error_free(generic_explicit_arity_error_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: explicit generic type argument arity parse\n");
    }
    parser_free_result(&generic_explicit_arity_error_parse);

    const char* generic_data_type_source =
        "record Box[T] {\n"
        "    value: T\n"
        "};\n"
        "type Pair[T] = (T, T);\n"
        "func identity[T](value: T): T {\n"
        "    return value;\n"
        "}\n"
        "func readBox(box: Box[int]): int {\n"
        "    return box.value;\n"
        "}\n"
        "func main(): void {\n"
        "    var intBox: Box[int] = { value: 41 };\n"
        "    var cloned: Box[int] = identity<Box[int]>(intBox);\n"
        "    var textBox: Box[string] = { value: \"ok\" };\n"
        "    var pair: Pair[int] = (readBox(cloned), 1);\n"
        "    var total: int = pair.0 + pair.1;\n"
        "    println(str(total) + textBox.value);\n"
        "}\n";
    ParseResult generic_data_type_parse = parser_parse(generic_data_type_source, "test.tblo");
    if (!generic_data_type_parse.error) {
        TypeCheckResult generic_data_type_tc = typecheck(generic_data_type_parse.program);
        if (!generic_data_type_tc.error) {
            CompileResult generic_data_type_compile = compile(generic_data_type_parse.program);
            if (!generic_data_type_compile.error && generic_data_type_compile.function) {
                tests_passed++;
                printf("  PASS: generic record/type alias parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: generic record/type alias parse+typecheck+compile\n");
            }

            for (int i = 0; i < generic_data_type_compile.function_count; i++) {
                if (generic_data_type_compile.functions && generic_data_type_compile.functions[i]) {
                    obj_function_free(generic_data_type_compile.functions[i]);
                }
            }
            if (generic_data_type_compile.functions) free(generic_data_type_compile.functions);
            if (generic_data_type_compile.function) obj_function_free(generic_data_type_compile.function);
            symbol_table_free(generic_data_type_compile.globals);
            error_free(generic_data_type_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: generic record/type alias parse+typecheck+compile\n");
        }
        symbol_table_free(generic_data_type_tc.globals);
        error_free(generic_data_type_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic record/type alias parse\n");
    }
    parser_free_result(&generic_data_type_parse);

    const char* generic_type_arity_error_source =
        "record Box[T] { value: T };\n"
        "func main(): void {\n"
        "    var bad: Box[int, string] = { value: 1 };\n"
        "    println(str(bad.value));\n"
        "}\n";
    ParseResult generic_type_arity_error_parse = parser_parse(generic_type_arity_error_source, "test.tblo");
    if (!generic_type_arity_error_parse.error) {
        TypeCheckResult generic_type_arity_error_tc = typecheck(generic_type_arity_error_parse.program);
        if (generic_type_arity_error_tc.error &&
            generic_type_arity_error_tc.error->message &&
            strstr(generic_type_arity_error_tc.error->message, "Wrong number of type arguments") != NULL &&
            strstr(generic_type_arity_error_tc.error->message, "declared as Box<T>") != NULL &&
            strstr(generic_type_arity_error_tc.error->message, "used as Box<int, string>") != NULL) {
            tests_passed++;
            printf("  PASS: generic data type argument arity diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: generic data type argument arity diagnostics\n");
        }
        symbol_table_free(generic_type_arity_error_tc.globals);
        error_free(generic_type_arity_error_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic data type argument arity parse\n");
    }
    parser_free_result(&generic_type_arity_error_parse);

    const char* nongeneric_type_arg_error_source =
        "record User {\n"
        "    id: int\n"
        "};\n"
        "func main(): void {\n"
        "    var bad: User[int] = { id: 1 };\n"
        "    println(str(bad.id));\n"
        "}\n";
    ParseResult nongeneric_type_arg_error_parse = parser_parse(nongeneric_type_arg_error_source, "test.tblo");
    if (!nongeneric_type_arg_error_parse.error) {
        TypeCheckResult nongeneric_type_arg_error_tc = typecheck(nongeneric_type_arg_error_parse.program);
        if (nongeneric_type_arg_error_tc.error &&
            nongeneric_type_arg_error_tc.error->message &&
            strstr(nongeneric_type_arg_error_tc.error->message, "Type 'User' is not generic") != NULL &&
            strstr(nongeneric_type_arg_error_tc.error->message, "remove type arguments") != NULL &&
            strstr(nongeneric_type_arg_error_tc.error->message, "used as User<int>") != NULL) {
            tests_passed++;
            printf("  PASS: non-generic type argument diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: non-generic type argument diagnostics\n");
        }
        symbol_table_free(nongeneric_type_arg_error_tc.globals);
        error_free(nongeneric_type_arg_error_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: non-generic type argument parse\n");
    }
    parser_free_result(&nongeneric_type_arg_error_parse);

    const char* generic_constraint_source =
        "record Point { x: int, y: int };\n"
        "interface Mover {\n"
        "    move(dx: int, dy: int): int;\n"
        "};\n"
        "func move(p: Point, dx: int, dy: int): int {\n"
        "    return p.x + p.y + dx + dy;\n"
        "}\n"
        "func passthroughMover[T: Mover](value: T): T {\n"
        "    return value;\n"
        "}\n"
        "func main(): void {\n"
        "    var p: Point = { x: 1, y: 2 };\n"
        "    var q: Point = passthroughMover(p);\n"
        "    var r: Point = passthroughMover<Point>(q);\n"
        "    println(str(r.x + r.y));\n"
        "}\n";
    ParseResult generic_constraint_parse = parser_parse(generic_constraint_source, "test.tblo");
    if (!generic_constraint_parse.error) {
        TypeCheckResult generic_constraint_tc = typecheck(generic_constraint_parse.program);
        if (!generic_constraint_tc.error) {
            CompileResult generic_constraint_compile = compile(generic_constraint_parse.program);
            if (!generic_constraint_compile.error && generic_constraint_compile.function) {
                tests_passed++;
                printf("  PASS: generic type-parameter constraints parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: generic type-parameter constraints parse+typecheck+compile\n");
            }

            for (int i = 0; i < generic_constraint_compile.function_count; i++) {
                if (generic_constraint_compile.functions && generic_constraint_compile.functions[i]) {
                    obj_function_free(generic_constraint_compile.functions[i]);
                }
            }
            if (generic_constraint_compile.functions) free(generic_constraint_compile.functions);
            if (generic_constraint_compile.function) obj_function_free(generic_constraint_compile.function);
            symbol_table_free(generic_constraint_compile.globals);
            error_free(generic_constraint_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: generic type-parameter constraints parse+typecheck+compile\n");
        }
        symbol_table_free(generic_constraint_tc.globals);
        error_free(generic_constraint_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic type-parameter constraints parse\n");
    }
    parser_free_result(&generic_constraint_parse);

    const char* generic_constraint_error_source =
        "interface Mover {\n"
        "    move(dx: int, dy: int): int;\n"
        "};\n"
        "func passthroughMover[T: Mover](value: T): T {\n"
        "    return value;\n"
        "}\n"
        "func main(): void {\n"
        "    var x: int = passthroughMover(1);\n"
        "    println(str(x));\n"
        "}\n";
    ParseResult generic_constraint_error_parse = parser_parse(generic_constraint_error_source, "test.tblo");
    if (!generic_constraint_error_parse.error) {
        TypeCheckResult generic_constraint_error_tc = typecheck(generic_constraint_error_parse.program);
        if (generic_constraint_error_tc.error &&
            generic_constraint_error_tc.error->message &&
            strstr(generic_constraint_error_tc.error->message, "does not satisfy constraint") != NULL) {
            tests_passed++;
            printf("  PASS: generic constraint violation diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: generic constraint violation diagnostics\n");
        }
        symbol_table_free(generic_constraint_error_tc.globals);
        error_free(generic_constraint_error_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic constraint violation parse\n");
    }
    parser_free_result(&generic_constraint_error_parse);

    const char* generic_constraint_body_usage_source =
        "record Point { x: int, y: int };\n"
        "interface Mover {\n"
        "    move(dx: int, dy: int): int;\n"
        "};\n"
        "func pointMove(p: Point, dx: int, dy: int): int {\n"
        "    return p.x + p.y + dx + dy;\n"
        "}\n"
        "impl Mover as Point {\n"
        "    move = pointMove;\n"
        "};\n"
        "func invoke(m: Mover): int {\n"
        "    return m.move(1, 2);\n"
        "}\n"
        "func useConstraint[T: Mover](m: T): int {\n"
        "    var base: int = invoke(m);\n"
        "    return base + m.move(3, 4);\n"
        "}\n"
        "func main(): void {\n"
        "    var p: Point = { x: 10, y: 20 };\n"
        "    var total: int = useConstraint(p);\n"
        "    println(str(total));\n"
        "}\n";
    ParseResult generic_constraint_body_usage_parse = parser_parse(generic_constraint_body_usage_source, "test.tblo");
    if (!generic_constraint_body_usage_parse.error) {
        TypeCheckResult generic_constraint_body_usage_tc = typecheck(generic_constraint_body_usage_parse.program);
        if (!generic_constraint_body_usage_tc.error) {
            CompileResult generic_constraint_body_usage_compile = compile(generic_constraint_body_usage_parse.program);
            if (!generic_constraint_body_usage_compile.error &&
                generic_constraint_body_usage_compile.function) {
                tests_passed++;
                printf("  PASS: constrained generic body interface usage\n");
            } else {
                tests_failed++;
                printf("  FAIL: constrained generic body interface usage\n");
            }

            for (int i = 0; i < generic_constraint_body_usage_compile.function_count; i++) {
                if (generic_constraint_body_usage_compile.functions && generic_constraint_body_usage_compile.functions[i]) {
                    obj_function_free(generic_constraint_body_usage_compile.functions[i]);
                }
            }
            if (generic_constraint_body_usage_compile.functions) free(generic_constraint_body_usage_compile.functions);
            if (generic_constraint_body_usage_compile.function) obj_function_free(generic_constraint_body_usage_compile.function);
            symbol_table_free(generic_constraint_body_usage_compile.globals);
            error_free(generic_constraint_body_usage_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: constrained generic body interface usage\n");
        }
        symbol_table_free(generic_constraint_body_usage_tc.globals);
        error_free(generic_constraint_body_usage_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: constrained generic body interface usage parse\n");
    }
    parser_free_result(&generic_constraint_body_usage_parse);

    const char* enum_parse_source =
        "enum Status {\n"
        "    Ok = 200,\n"
        "    NotFound = 404,\n"
        "    Retry\n"
        "};\n"
        "func main(): void {\n"
        "    var ok: int = Status.Ok;\n"
        "    var nf: int = Status.NotFound;\n"
        "    var retry: int = Status.Retry;\n"
        "    println(ok + nf + retry);\n"
        "}\n";
    ParseResult enum_parse = parser_parse(enum_parse_source, "test.tblo");
    if (!enum_parse.error) {
        TypeCheckResult enum_tc = typecheck(enum_parse.program);
        if (!enum_tc.error) {
            CompileResult enum_compile = compile(enum_parse.program);
            if (!enum_compile.error && enum_compile.function) {
                tests_passed++;
                printf("  PASS: enum declaration/member parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: enum declaration/member parse+typecheck+compile\n");
            }

            for (int i = 0; i < enum_compile.function_count; i++) {
                if (enum_compile.functions && enum_compile.functions[i]) {
                    obj_function_free(enum_compile.functions[i]);
                }
            }
            if (enum_compile.functions) free(enum_compile.functions);
            if (enum_compile.function) obj_function_free(enum_compile.function);
            symbol_table_free(enum_compile.globals);
            error_free(enum_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: enum declaration/member parse+typecheck+compile\n");
        }
        symbol_table_free(enum_tc.globals);
        error_free(enum_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: enum declaration/member parse+typecheck+compile\n");
    }
    parser_free_result(&enum_parse);

    const char* enum_payload_parse_source =
        "enum Response {\n"
        "    Ok(string),\n"
        "    RetryAfter(int, string),\n"
        "    Empty\n"
        "};\n";
    ParseResult enum_payload_parse = parser_parse(enum_payload_parse_source, "test.tblo");
    if (!enum_payload_parse.error &&
        enum_payload_parse.program &&
        enum_payload_parse.program->stmt_count > 0 &&
        enum_payload_parse.program->statements[0] &&
        enum_payload_parse.program->statements[0]->kind == STMT_ENUM_DECL) {
        Stmt* enum_stmt = enum_payload_parse.program->statements[0];
        bool payload_shape_ok =
            enum_stmt->enum_decl.has_payload_members &&
            enum_stmt->enum_decl.member_count == 3 &&
            enum_stmt->enum_decl.member_payload_counts &&
            enum_stmt->enum_decl.member_payload_counts[0] == 1 &&
            enum_stmt->enum_decl.member_payload_counts[1] == 2 &&
            enum_stmt->enum_decl.member_payload_counts[2] == 0;
        if (payload_shape_ok) {
            tests_passed++;
            printf("  PASS: enum payload variant parse metadata\n");
        } else {
            tests_failed++;
            printf("  FAIL: enum payload variant parse metadata\n");
        }
    } else {
        tests_failed++;
        printf("  FAIL: enum payload variant parse\n");
    }
    parser_free_result(&enum_payload_parse);

    const char* enum_generic_parse_source =
        "enum Result[T, E] {\n"
        "    Ok(T),\n"
        "    Err(E)\n"
        "};\n";
    ParseResult enum_generic_parse = parser_parse(enum_generic_parse_source, "test.tblo");
    if (!enum_generic_parse.error &&
        enum_generic_parse.program &&
        enum_generic_parse.program->stmt_count > 0 &&
        enum_generic_parse.program->statements[0] &&
        enum_generic_parse.program->statements[0]->kind == STMT_ENUM_DECL) {
        Stmt* enum_stmt = enum_generic_parse.program->statements[0];
        bool generic_shape_ok =
            enum_stmt->enum_decl.type_param_count == 2 &&
            enum_stmt->enum_decl.type_params &&
            enum_stmt->enum_decl.type_params[0] &&
            enum_stmt->enum_decl.type_params[1] &&
            strcmp(enum_stmt->enum_decl.type_params[0], "T") == 0 &&
            strcmp(enum_stmt->enum_decl.type_params[1], "E") == 0 &&
            enum_stmt->enum_decl.member_count == 2 &&
            enum_stmt->enum_decl.member_payload_counts &&
            enum_stmt->enum_decl.member_payload_counts[0] == 1 &&
            enum_stmt->enum_decl.member_payload_counts[1] == 1 &&
            enum_stmt->enum_decl.member_payload_types &&
            enum_stmt->enum_decl.member_payload_types[0] &&
            enum_stmt->enum_decl.member_payload_types[1] &&
            enum_stmt->enum_decl.member_payload_types[0][0] &&
            enum_stmt->enum_decl.member_payload_types[1][0] &&
            enum_stmt->enum_decl.member_payload_types[0][0]->kind == TYPE_TYPE_PARAM &&
            enum_stmt->enum_decl.member_payload_types[1][0]->kind == TYPE_TYPE_PARAM &&
            enum_stmt->enum_decl.member_payload_types[0][0]->type_param_name &&
            enum_stmt->enum_decl.member_payload_types[1][0]->type_param_name &&
            strcmp(enum_stmt->enum_decl.member_payload_types[0][0]->type_param_name, "T") == 0 &&
            strcmp(enum_stmt->enum_decl.member_payload_types[1][0]->type_param_name, "E") == 0;

        if (generic_shape_ok) {
            tests_passed++;
            printf("  PASS: generic enum payload type-parameter parse metadata\n");
        } else {
            tests_failed++;
            printf("  FAIL: generic enum payload type-parameter parse metadata\n");
        }
    } else {
        tests_failed++;
        printf("  FAIL: generic enum parse\n");
    }
    parser_free_result(&enum_generic_parse);

    const char* enum_generic_runtime_source =
        "enum Result[T, E] {\n"
        "    Ok(T),\n"
        "    Err(E)\n"
        "};\n"
        "var r1: Result[int, string] = Result.Ok<int, string>(7);\n"
        "var r2: Result[int, string] = Result.Err<int, string>(\"boom\");\n"
        "var out1: int = -1;\n"
        "var out2: string = \"\";\n"
        "match (r1) {\n"
        "    Result.Ok<int, string>(value): out1 = value;\n"
        "    Result.Err<int, string>(msg): out1 = -2;\n"
        "}\n"
        "match (r2) {\n"
        "    Result.Ok<int, string>(value): out2 = \"ok\";\n"
        "    Result.Err<int, string>(msg): out2 = msg;\n"
        "}\n";
    ParseResult enum_generic_runtime_parse = parser_parse(enum_generic_runtime_source, "test.tblo");
    if (!enum_generic_runtime_parse.error) {
        TypeCheckResult enum_generic_runtime_tc = typecheck(enum_generic_runtime_parse.program);
        if (!enum_generic_runtime_tc.error) {
            CompileResult enum_generic_runtime_compile = compile(enum_generic_runtime_parse.program);
            if (!enum_generic_runtime_compile.error && enum_generic_runtime_compile.function) {
                VM vm;
                vm_init(&vm);
                int exec_rc = vm_execute(&vm, enum_generic_runtime_compile.function);
                if (exec_rc == 0) {
                    Value out1 = vm_get_global(&vm, "out1");
                    Value out2 = vm_get_global(&vm, "out2");
                    ObjString* out2_str = value_get_string_obj(&out2);
                    bool out1_ok = value_get_type(&out1) == VAL_INT && value_get_int(&out1) == 7;
                    bool out2_ok = value_get_type(&out2) == VAL_STRING && out2_str &&
                                   out2_str->chars &&
                                   strcmp(out2_str->chars, "boom") == 0;
                    if (out1_ok && out2_ok) {
                        tests_passed++;
                        printf("  PASS: generic enum constructor+match runtime execution\n");
                    } else {
                        tests_failed++;
                        printf("  FAIL: generic enum constructor+match runtime execution\n");
                    }
                } else {
                    tests_failed++;
                    printf("  FAIL: generic enum constructor+match runtime\n");
                }
                vm_free(&vm);
            } else {
                tests_failed++;
                printf("  FAIL: generic enum constructor+match compile\n");
            }

            for (int i = 0; i < enum_generic_runtime_compile.function_count; i++) {
                if (enum_generic_runtime_compile.functions && enum_generic_runtime_compile.functions[i]) {
                    obj_function_free(enum_generic_runtime_compile.functions[i]);
                }
            }
            if (enum_generic_runtime_compile.functions) free(enum_generic_runtime_compile.functions);
            if (enum_generic_runtime_compile.function) obj_function_free(enum_generic_runtime_compile.function);
            symbol_table_free(enum_generic_runtime_compile.globals);
            error_free(enum_generic_runtime_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: generic enum constructor+match typecheck\n");
        }
        symbol_table_free(enum_generic_runtime_tc.globals);
        error_free(enum_generic_runtime_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic enum constructor+match parse\n");
    }
    parser_free_result(&enum_generic_runtime_parse);

    const char* enum_generic_contextual_infer_source =
        "enum Result[T, E] {\n"
        "    Ok(T),\n"
        "    Err(E)\n"
        "};\n"
        "var inferredDecl: Result[int, string] = Result.Ok(7);\n"
        "var inferredAssign: Result[int, string] = Result.Err(\"boom\");\n"
        "inferredAssign = Result.Ok(9);\n"
        "var outA: int = -1;\n"
        "var outB: int = -1;\n"
        "match (inferredDecl) {\n"
        "    Result.Ok<int, string>(v): outA = v;\n"
        "    else: outA = -2;\n"
        "}\n"
        "match (inferredAssign) {\n"
        "    Result.Ok<int, string>(v): outB = v;\n"
        "    else: outB = -2;\n"
        "}\n";
    ParseResult enum_generic_contextual_infer_parse = parser_parse(enum_generic_contextual_infer_source, "test.tblo");
    if (!enum_generic_contextual_infer_parse.error) {
        TypeCheckResult enum_generic_contextual_infer_tc = typecheck(enum_generic_contextual_infer_parse.program);
        if (!enum_generic_contextual_infer_tc.error) {
            CompileResult enum_generic_contextual_infer_compile = compile(enum_generic_contextual_infer_parse.program);
            if (!enum_generic_contextual_infer_compile.error && enum_generic_contextual_infer_compile.function) {
                VM vm;
                vm_init(&vm);
                int exec_rc = vm_execute(&vm, enum_generic_contextual_infer_compile.function);
                if (exec_rc == 0) {
                    Value outA = vm_get_global(&vm, "outA");
                    Value outB = vm_get_global(&vm, "outB");
                    if (value_get_type(&outA) == VAL_INT && value_get_int(&outA) == 7 &&
                        value_get_type(&outB) == VAL_INT && value_get_int(&outB) == 9) {
                        tests_passed++;
                        printf("  PASS: generic enum constructor contextual inference\n");
                    } else {
                        tests_failed++;
                        printf("  FAIL: generic enum constructor contextual inference\n");
                    }
                } else {
                    tests_failed++;
                    printf("  FAIL: generic enum constructor contextual inference runtime\n");
                }
                vm_free(&vm);
            } else {
                tests_failed++;
                printf("  FAIL: generic enum constructor contextual inference compile\n");
            }

            for (int i = 0; i < enum_generic_contextual_infer_compile.function_count; i++) {
                if (enum_generic_contextual_infer_compile.functions && enum_generic_contextual_infer_compile.functions[i]) {
                    obj_function_free(enum_generic_contextual_infer_compile.functions[i]);
                }
            }
            if (enum_generic_contextual_infer_compile.functions) free(enum_generic_contextual_infer_compile.functions);
            if (enum_generic_contextual_infer_compile.function) obj_function_free(enum_generic_contextual_infer_compile.function);
            symbol_table_free(enum_generic_contextual_infer_compile.globals);
            error_free(enum_generic_contextual_infer_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: generic enum constructor contextual inference typecheck\n");
        }
        symbol_table_free(enum_generic_contextual_infer_tc.globals);
        error_free(enum_generic_contextual_infer_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic enum constructor contextual inference parse\n");
    }
    parser_free_result(&enum_generic_contextual_infer_parse);

    const char* enum_generic_contextual_call_return_source =
        "enum Result[T, E] {\n"
        "    Ok(T),\n"
        "    Err(E)\n"
        "};\n"
        "func unwrapOrZero(value: Result[int, string]): int {\n"
        "    match (value) {\n"
        "        Result.Ok(v): return v;\n"
        "        Result.Err(_): return 0;\n"
        "    }\n"
        "}\n"
        "func makeOk(): Result[int, string] {\n"
        "    return Result.Ok(9);\n"
        "}\n"
        "var outCall: int = unwrapOrZero(Result.Ok(7));\n"
        "var outErr: int = unwrapOrZero(Result.Err(\"boom\"));\n"
        "var outRet: int = unwrapOrZero(makeOk());\n"
        "func main(): void {\n"
        "}\n";
    ParseResult enum_generic_contextual_call_return_parse =
        parser_parse(enum_generic_contextual_call_return_source, "test.tblo");
    if (!enum_generic_contextual_call_return_parse.error) {
        TypeCheckResult enum_generic_contextual_call_return_tc =
            typecheck(enum_generic_contextual_call_return_parse.program);
        if (!enum_generic_contextual_call_return_tc.error) {
            CompileResult enum_generic_contextual_call_return_compile =
                compile(enum_generic_contextual_call_return_parse.program);
            if (!enum_generic_contextual_call_return_compile.error &&
                enum_generic_contextual_call_return_compile.function) {
                tests_passed++;
                printf("  PASS: generic enum contextual inference across calls, returns, and '_' match bindings\n");
            } else {
                tests_failed++;
                printf("  FAIL: generic enum contextual inference compile\n");
            }

            for (int i = 0; i < enum_generic_contextual_call_return_compile.function_count; i++) {
                if (enum_generic_contextual_call_return_compile.functions &&
                    enum_generic_contextual_call_return_compile.functions[i]) {
                    obj_function_free(enum_generic_contextual_call_return_compile.functions[i]);
                }
            }
            if (enum_generic_contextual_call_return_compile.functions) {
                free(enum_generic_contextual_call_return_compile.functions);
            }
            if (enum_generic_contextual_call_return_compile.function) {
                obj_function_free(enum_generic_contextual_call_return_compile.function);
            }
            symbol_table_free(enum_generic_contextual_call_return_compile.globals);
            error_free(enum_generic_contextual_call_return_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: generic enum contextual inference typecheck\n");
        }
        symbol_table_free(enum_generic_contextual_call_return_tc.globals);
        error_free(enum_generic_contextual_call_return_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic enum contextual inference parse\n");
    }
    parser_free_result(&enum_generic_contextual_call_return_parse);

    const char* enum_generic_match_infer_source =
        "enum Result[T, E] {\n"
        "    Ok(T),\n"
        "    Err(E)\n"
        "};\n"
        "var r1: Result[int, string] = Result.Ok(7);\n"
        "var r2: Result[int, string] = Result.Err(\"boom\");\n"
        "var out1: int = -1;\n"
        "var out2: string = \"\";\n"
        "match (r1) {\n"
        "    Result.Ok(value): out1 = value;\n"
        "    Result.Err(msg): out1 = -2;\n"
        "}\n"
        "match (r2) {\n"
        "    Result.Ok(value): out2 = \"ok\";\n"
        "    Result.Err(msg): out2 = msg;\n"
        "}\n";
    ParseResult enum_generic_match_infer_parse = parser_parse(enum_generic_match_infer_source, "test.tblo");
    if (!enum_generic_match_infer_parse.error) {
        TypeCheckResult enum_generic_match_infer_tc = typecheck(enum_generic_match_infer_parse.program);
        if (!enum_generic_match_infer_tc.error) {
            CompileResult enum_generic_match_infer_compile = compile(enum_generic_match_infer_parse.program);
            if (!enum_generic_match_infer_compile.error && enum_generic_match_infer_compile.function) {
                VM vm;
                vm_init(&vm);
                int exec_rc = vm_execute(&vm, enum_generic_match_infer_compile.function);
                if (exec_rc == 0) {
                    Value out1 = vm_get_global(&vm, "out1");
                    Value out2 = vm_get_global(&vm, "out2");
                    ObjString* out2_str = value_get_string_obj(&out2);
                    bool out1_ok = value_get_type(&out1) == VAL_INT && value_get_int(&out1) == 7;
                    bool out2_ok = value_get_type(&out2) == VAL_STRING && out2_str &&
                                   out2_str->chars &&
                                   strcmp(out2_str->chars, "boom") == 0;
                    if (out1_ok && out2_ok) {
                        tests_passed++;
                        printf("  PASS: generic enum match payload inference without explicit type args\n");
                    } else {
                        tests_failed++;
                        printf("  FAIL: generic enum match payload inference without explicit type args\n");
                    }
                } else {
                    tests_failed++;
                    printf("  FAIL: generic enum match payload inference runtime\n");
                }
                vm_free(&vm);
            } else {
                tests_failed++;
                printf("  FAIL: generic enum match payload inference compile\n");
            }

            for (int i = 0; i < enum_generic_match_infer_compile.function_count; i++) {
                if (enum_generic_match_infer_compile.functions && enum_generic_match_infer_compile.functions[i]) {
                    obj_function_free(enum_generic_match_infer_compile.functions[i]);
                }
            }
            if (enum_generic_match_infer_compile.functions) free(enum_generic_match_infer_compile.functions);
            if (enum_generic_match_infer_compile.function) obj_function_free(enum_generic_match_infer_compile.function);
            symbol_table_free(enum_generic_match_infer_compile.globals);
            error_free(enum_generic_match_infer_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: generic enum match payload inference typecheck\n");
        }
        symbol_table_free(enum_generic_match_infer_tc.globals);
        error_free(enum_generic_match_infer_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic enum match payload inference parse\n");
    }
    parser_free_result(&enum_generic_match_infer_parse);

    const char* enum_generic_infer_error_source =
        "enum Result[T, E] {\n"
        "    Ok(T),\n"
        "    Err(E)\n"
        "};\n"
        "var bad = Result.Ok(7);\n";
    ParseResult enum_generic_infer_error_parse = parser_parse(enum_generic_infer_error_source, "test.tblo");
    if (!enum_generic_infer_error_parse.error) {
        TypeCheckResult enum_generic_infer_error_tc = typecheck(enum_generic_infer_error_parse.program);
        if (enum_generic_infer_error_tc.error &&
            enum_generic_infer_error_tc.error->message &&
            strstr(enum_generic_infer_error_tc.error->message, "Cannot infer generic type parameter(s) E") != NULL &&
            strstr(enum_generic_infer_error_tc.error->message, "call to 'Result.Ok'") != NULL &&
            strstr(enum_generic_infer_error_tc.error->message, "Result.Ok<T, E>(...)") != NULL) {
            tests_passed++;
            printf("  PASS: generic enum constructor inference failure diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: generic enum constructor inference failure diagnostics\n");
        }
        symbol_table_free(enum_generic_infer_error_tc.globals);
        error_free(enum_generic_infer_error_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic enum constructor inference parse\n");
    }
    parser_free_result(&enum_generic_infer_error_parse);

    const char* enum_generic_match_mismatch_source =
        "enum Result[T, E] {\n"
        "    Ok(T),\n"
        "    Err(E)\n"
        "};\n"
        "func classify(r: Result[int, string]): int {\n"
        "    match (r) {\n"
        "        Result.Ok<int, int>(1): return 1;\n"
        "        else: return 0;\n"
        "    }\n"
        "}\n";
    ParseResult enum_generic_match_mismatch_parse = parser_parse(enum_generic_match_mismatch_source, "test.tblo");
    if (!enum_generic_match_mismatch_parse.error) {
        TypeCheckResult enum_generic_match_mismatch_tc = typecheck(enum_generic_match_mismatch_parse.program);
        if (enum_generic_match_mismatch_tc.error &&
            enum_generic_match_mismatch_tc.error->message &&
            strstr(enum_generic_match_mismatch_tc.error->message,
                   "incompatible with subject enum 'Result[int,string]'") != NULL) {
            tests_passed++;
            printf("  PASS: generic enum match instantiation compatibility rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: generic enum match instantiation compatibility rejection\n");
        }
        symbol_table_free(enum_generic_match_mismatch_tc.globals);
        error_free(enum_generic_match_mismatch_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: generic enum match instantiation parse\n");
    }
    parser_free_result(&enum_generic_match_mismatch_parse);

    const char* enum_payload_typecheck_source =
        "enum Response {\n"
        "    Ok(string),\n"
        "    Err(int),\n"
        "    Empty\n"
        "};\n"
        "func choose(flag: bool): Response {\n"
        "    if (flag) {\n"
        "        return Response.Ok(\"yes\");\n"
        "    }\n"
        "    return Response.Err(5);\n"
        "}\n"
        "func classify(r: Response): int {\n"
        "    match (r) {\n"
        "        Response.Ok(text): return 1;\n"
        "        Response.Err(code): return 2;\n"
        "        Response.Empty: return 3;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    ParseResult enum_payload_typecheck_parse = parser_parse(enum_payload_typecheck_source, "test.tblo");
    if (!enum_payload_typecheck_parse.error) {
        TypeCheckResult enum_payload_typecheck_tc = typecheck(enum_payload_typecheck_parse.program);
        if (!enum_payload_typecheck_tc.error) {
            tests_passed++;
            printf("  PASS: enum payload variant constructor+match typecheck\n");
        } else {
            tests_failed++;
            printf("  FAIL: enum payload variant constructor+match typecheck\n");
        }
        symbol_table_free(enum_payload_typecheck_tc.globals);
        error_free(enum_payload_typecheck_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: enum payload typecheck parse\n");
    }
    parser_free_result(&enum_payload_typecheck_parse);

    const char* enum_payload_arity_source =
        "enum Response {\n"
        "    Ok(string)\n"
        "};\n"
        "var bad: Response = Response.Ok();\n";
    ParseResult enum_payload_arity_parse = parser_parse(enum_payload_arity_source, "test.tblo");
    if (!enum_payload_arity_parse.error) {
        TypeCheckResult enum_payload_arity_tc = typecheck(enum_payload_arity_parse.program);
        if (enum_payload_arity_tc.error &&
            enum_payload_arity_tc.error->message &&
            strstr(enum_payload_arity_tc.error->message, "Wrong number of arguments") != NULL) {
            tests_passed++;
            printf("  PASS: enum payload constructor arity diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: enum payload constructor arity diagnostics\n");
        }
        symbol_table_free(enum_payload_arity_tc.globals);
        error_free(enum_payload_arity_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: enum payload arity parse\n");
    }
    parser_free_result(&enum_payload_arity_parse);

    const char* enum_payload_compile_source =
        "enum Response {\n"
        "    Ok(string),\n"
        "    Err(int),\n"
        "    Empty\n"
        "};\n"
        "var r1: Response = Response.Ok(\"x\");\n"
        "var r2: Response = Response.Err(5);\n"
        "var r3: Response = Response.Empty;\n"
        "var out1: int = -1;\n"
        "var out2: int = -1;\n"
        "var out3: int = -1;\n"
        "match (r1) {\n"
        "    Response.Ok(\"x\"): out1 = 11;\n"
        "    Response.Err(5): out1 = 22;\n"
        "    else: out1 = 99;\n"
        "}\n"
        "match (r2) {\n"
        "    Response.Ok(\"x\"): out2 = 11;\n"
        "    Response.Err(5): out2 = 22;\n"
        "    else: out2 = 99;\n"
        "}\n"
        "match (r3) {\n"
        "    Response.Empty: out3 = 33;\n"
        "    else: out3 = 99;\n"
        "}\n"
        "\n";
    ParseResult enum_payload_compile_parse = parser_parse(enum_payload_compile_source, "test.tblo");
    if (!enum_payload_compile_parse.error) {
        TypeCheckResult enum_payload_compile_tc = typecheck(enum_payload_compile_parse.program);
        if (!enum_payload_compile_tc.error) {
            CompileResult enum_payload_compile = compile(enum_payload_compile_parse.program);
            if (!enum_payload_compile.error && enum_payload_compile.function) {
                VM vm;
                vm_init(&vm);
                int exec_rc = vm_execute(&vm, enum_payload_compile.function);
                if (exec_rc == 0) {
                    Value out1 = vm_get_global(&vm, "out1");
                    Value out2 = vm_get_global(&vm, "out2");
                    Value out3 = vm_get_global(&vm, "out3");
                    if (value_get_type(&out1) == VAL_INT && value_get_int(&out1) == 11 &&
                        value_get_type(&out2) == VAL_INT && value_get_int(&out2) == 22 &&
                        value_get_type(&out3) == VAL_INT && value_get_int(&out3) == 33) {
                        tests_passed++;
                        printf("  PASS: enum payload compile+runtime match execution\n");
                    } else {
                        tests_failed++;
                        printf("  FAIL: enum payload compile+runtime match execution\n");
                    }
                } else {
                    tests_failed++;
                    printf("  FAIL: enum payload compile+runtime execution\n");
                }
                vm_free(&vm);
            } else {
                tests_failed++;
                printf("  FAIL: enum payload compile-stage compile\n");
            }

            for (int i = 0; i < enum_payload_compile.function_count; i++) {
                if (enum_payload_compile.functions && enum_payload_compile.functions[i]) {
                    obj_function_free(enum_payload_compile.functions[i]);
                }
            }
            if (enum_payload_compile.functions) free(enum_payload_compile.functions);
            if (enum_payload_compile.function) obj_function_free(enum_payload_compile.function);
            symbol_table_free(enum_payload_compile.globals);
            error_free(enum_payload_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: enum payload compile-stage typecheck\n");
        }
        symbol_table_free(enum_payload_compile_tc.globals);
        error_free(enum_payload_compile_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: enum payload compile-stage parse\n");
    }
    parser_free_result(&enum_payload_compile_parse);

    const char* enum_payload_bindings_source =
        "enum Response {\n"
        "    Ok(string),\n"
        "    Err(int),\n"
        "    Empty\n"
        "};\n"
        "var r1: Response = Response.Ok(\"hello\");\n"
        "var r2: Response = Response.Err(7);\n"
        "var outMsg: string = \"\";\n"
        "var outCode: int = -1;\n"
        "var outDiscard: int = 0;\n"
        "match (r1) {\n"
        "    Response.Ok(msg): outMsg = msg;\n"
        "    else: outMsg = \"bad\";\n"
        "}\n"
        "match (r2) {\n"
        "    Response.Err(code): outCode = code;\n"
        "    else: outCode = -2;\n"
        "}\n"
        "match (r1) {\n"
        "    Response.Ok(_): outDiscard = 1;\n"
        "    else: outDiscard = 0;\n"
        "}\n";
    ParseResult enum_payload_bindings_parse = parser_parse(enum_payload_bindings_source, "test.tblo");
    if (!enum_payload_bindings_parse.error) {
        TypeCheckResult enum_payload_bindings_tc = typecheck(enum_payload_bindings_parse.program);
        if (!enum_payload_bindings_tc.error) {
            CompileResult enum_payload_bindings_compile = compile(enum_payload_bindings_parse.program);
            if (!enum_payload_bindings_compile.error && enum_payload_bindings_compile.function) {
                VM vm;
                vm_init(&vm);
                int exec_rc = vm_execute(&vm, enum_payload_bindings_compile.function);
                if (exec_rc == 0) {
                    Value out_msg = vm_get_global(&vm, "outMsg");
                    Value out_code = vm_get_global(&vm, "outCode");
                    Value out_discard = vm_get_global(&vm, "outDiscard");
                    ObjString* out_msg_str = value_get_string_obj(&out_msg);
                    bool msg_ok = value_get_type(&out_msg) == VAL_STRING &&
                                  out_msg_str &&
                                  out_msg_str->chars &&
                                  strcmp(out_msg_str->chars, "hello") == 0;
                    bool code_ok = value_get_type(&out_code) == VAL_INT && value_get_int(&out_code) == 7;
                    bool discard_ok = value_get_type(&out_discard) == VAL_INT &&
                                      value_get_int(&out_discard) == 1;
                    if (msg_ok && code_ok && discard_ok) {
                        tests_passed++;
                        printf("  PASS: enum payload match destructuring bindings execution\n");
                    } else {
                        tests_failed++;
                        printf("  FAIL: enum payload match destructuring bindings execution\n");
                    }
                } else {
                    tests_failed++;
                    printf("  FAIL: enum payload match destructuring runtime execution\n");
                }
                vm_free(&vm);
            } else {
                tests_failed++;
                printf("  FAIL: enum payload match destructuring compile\n");
            }

            for (int i = 0; i < enum_payload_bindings_compile.function_count; i++) {
                if (enum_payload_bindings_compile.functions && enum_payload_bindings_compile.functions[i]) {
                    obj_function_free(enum_payload_bindings_compile.functions[i]);
                }
            }
            if (enum_payload_bindings_compile.functions) free(enum_payload_bindings_compile.functions);
            if (enum_payload_bindings_compile.function) obj_function_free(enum_payload_bindings_compile.function);
            symbol_table_free(enum_payload_bindings_compile.globals);
            error_free(enum_payload_bindings_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: enum payload match destructuring typecheck\n");
        }
        symbol_table_free(enum_payload_bindings_tc.globals);
        error_free(enum_payload_bindings_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: enum payload match destructuring parse\n");
    }
    parser_free_result(&enum_payload_bindings_parse);

    const char* enum_payload_duplicate_binding_source =
        "enum Response {\n"
        "    Pair(int, int)\n"
        "};\n"
        "func bad(r: Response): int {\n"
        "    match (r) {\n"
        "        Response.Pair(v, v): return v;\n"
        "        else: return 0;\n"
        "    }\n"
        "}\n";
    ParseResult enum_payload_duplicate_binding_parse = parser_parse(enum_payload_duplicate_binding_source, "test.tblo");
    if (!enum_payload_duplicate_binding_parse.error) {
        TypeCheckResult enum_payload_duplicate_binding_tc = typecheck(enum_payload_duplicate_binding_parse.program);
        if (enum_payload_duplicate_binding_tc.error &&
            enum_payload_duplicate_binding_tc.error->message &&
            strstr(enum_payload_duplicate_binding_tc.error->message,
                   "Duplicate enum payload binding name in match pattern") != NULL) {
            tests_passed++;
            printf("  PASS: enum payload duplicate binding rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: enum payload duplicate binding rejection\n");
        }
        symbol_table_free(enum_payload_duplicate_binding_tc.globals);
        error_free(enum_payload_duplicate_binding_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: enum payload duplicate binding parse\n");
    }
    parser_free_result(&enum_payload_duplicate_binding_parse);

    const char* enum_payload_mixed_numeric_source =
        "enum Mixed {\n"
        "    Ok(string),\n"
        "    Legacy = 7\n"
        "};\n";
    ParseResult enum_payload_mixed_numeric_parse = parser_parse(enum_payload_mixed_numeric_source, "test.tblo");
    if (enum_payload_mixed_numeric_parse.error &&
        enum_payload_mixed_numeric_parse.error->message &&
        strstr(enum_payload_mixed_numeric_parse.error->message,
               "Cannot mix payload variants with explicit numeric enum assignments") != NULL) {
        tests_passed++;
        printf("  PASS: enum payload/numeric mixed declaration rejection\n");
    } else {
        tests_failed++;
        printf("  FAIL: enum payload/numeric mixed declaration rejection\n");
    }
    parser_free_result(&enum_payload_mixed_numeric_parse);

    const char* enum_missing_member_source =
        "enum Status { Ok = 1 };\n"
        "var bad: int = Status.Missing;\n";
    ParseResult enum_missing_member_parse = parser_parse(enum_missing_member_source, "test.tblo");
    if (!enum_missing_member_parse.error) {
        TypeCheckResult enum_missing_member_tc = typecheck(enum_missing_member_parse.program);
        if (enum_missing_member_tc.error &&
            enum_missing_member_tc.error->message &&
            strstr(enum_missing_member_tc.error->message, "Unknown enum member") != NULL) {
            tests_passed++;
            printf("  PASS: unknown enum member rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: unknown enum member rejection\n");
        }
        symbol_table_free(enum_missing_member_tc.globals);
        error_free(enum_missing_member_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: enum missing member parse\n");
    }
    parser_free_result(&enum_missing_member_parse);

    const char* match_parse_source =
        "enum Status { Ok = 200, NotFound = 404, Retry = 429 };\n"
        "func classify(code: int): int {\n"
        "    var out: int = -1;\n"
        "    match (code) {\n"
        "        Status.Ok: out = 1;\n"
        "        Status.NotFound: out = 2;\n"
        "        500: out = 3;\n"
        "        else: out = 0;\n"
        "    }\n"
        "    return out;\n"
        "}\n";
    ParseResult match_parse = parser_parse(match_parse_source, "test.tblo");
    if (!match_parse.error) {
        TypeCheckResult match_tc = typecheck(match_parse.program);
        if (!match_tc.error) {
            CompileResult match_compile = compile(match_parse.program);
            if (!match_compile.error && match_compile.function) {
                tests_passed++;
                printf("  PASS: match statement parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: match statement parse+typecheck+compile\n");
            }

            for (int i = 0; i < match_compile.function_count; i++) {
                if (match_compile.functions && match_compile.functions[i]) {
                    obj_function_free(match_compile.functions[i]);
                }
            }
            if (match_compile.functions) free(match_compile.functions);
            if (match_compile.function) obj_function_free(match_compile.function);
            symbol_table_free(match_compile.globals);
            error_free(match_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: match statement parse+typecheck+compile\n");
        }
        symbol_table_free(match_tc.globals);
        error_free(match_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: match statement parse+typecheck+compile\n");
    }
    parser_free_result(&match_parse);

    const char* match_guard_source =
        "enum Result[T, E] { Ok(T), Err(E) };\n"
        "func classify(result: Result[int, string]): int {\n"
        "    match (result) {\n"
        "        Result.Ok(value) if value > 0: return value;\n"
        "        Result.Ok(_): return 0;\n"
        "        Result.Err(_): return -1;\n"
        "    }\n"
        "}\n";
    ParseResult match_guard_parse = parser_parse(match_guard_source, "test.tblo");
    if (!match_guard_parse.error &&
        match_guard_parse.program &&
        match_guard_parse.program->stmt_count >= 2 &&
        match_guard_parse.program->statements[1] &&
        match_guard_parse.program->statements[1]->kind == STMT_FUNC_DECL) {
        Stmt* match_guard_func = match_guard_parse.program->statements[1];
        Stmt* guard_stmt =
            match_guard_func->func_decl.body &&
            match_guard_func->func_decl.body->kind == STMT_BLOCK &&
            match_guard_func->func_decl.body->block.stmt_count > 0
                ? match_guard_func->func_decl.body->block.statements[0]
                : NULL;
        bool parsed_guard =
            guard_stmt &&
            guard_stmt->kind == STMT_MATCH &&
            guard_stmt->match_stmt.arm_count == 3 &&
            guard_stmt->match_stmt.guards &&
            guard_stmt->match_stmt.guards[0] != NULL &&
            guard_stmt->match_stmt.guards[1] == NULL;

        TypeCheckResult match_guard_tc = typecheck(match_guard_parse.program);
        if (!match_guard_tc.error && parsed_guard) {
            CompileResult match_guard_compile = compile(match_guard_parse.program);
            if (!match_guard_compile.error && match_guard_compile.function) {
                tests_passed++;
                printf("  PASS: match guards parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: match guards parse+typecheck+compile\n");
            }

            for (int i = 0; i < match_guard_compile.function_count; i++) {
                if (match_guard_compile.functions && match_guard_compile.functions[i]) {
                    obj_function_free(match_guard_compile.functions[i]);
                }
            }
            if (match_guard_compile.functions) free(match_guard_compile.functions);
            if (match_guard_compile.function) obj_function_free(match_guard_compile.function);
            symbol_table_free(match_guard_compile.globals);
            error_free(match_guard_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: match guards parse+typecheck+compile\n");
        }
        symbol_table_free(match_guard_tc.globals);
        error_free(match_guard_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: match guards parse\n");
    }
    parser_free_result(&match_guard_parse);

    const char* match_guard_non_bool_source =
        "func classify(code: int): int {\n"
        "    match (code) {\n"
        "        1 if 2: return 1;\n"
        "        else: return 0;\n"
        "    }\n"
        "}\n";
    ParseResult match_guard_non_bool_parse = parser_parse(match_guard_non_bool_source, "test.tblo");
    if (!match_guard_non_bool_parse.error) {
        TypeCheckResult match_guard_non_bool_tc = typecheck(match_guard_non_bool_parse.program);
        if (match_guard_non_bool_tc.error &&
            match_guard_non_bool_tc.error->message &&
            strstr(match_guard_non_bool_tc.error->message, "Match guard must be bool") != NULL) {
            tests_passed++;
            printf("  PASS: non-bool match guard rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: non-bool match guard rejection\n");
        }
        symbol_table_free(match_guard_non_bool_tc.globals);
        error_free(match_guard_non_bool_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: non-bool match guard parse\n");
    }
    parser_free_result(&match_guard_non_bool_parse);

    const char* match_expr_source =
        "enum Result[T, E] { Ok(T), Err(E) };\n"
        "func classify(result: Result[int, string]): string {\n"
        "    return match (result) {\n"
        "        Result.Ok(value) if value > 0: \"positive\",\n"
        "        Result.Ok(value) if value == 0: \"zero\",\n"
        "        Result.Ok(_): \"negative\",\n"
        "        Result.Err(message): message\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_parse = parser_parse(match_expr_source, "test.tblo");
    if (!match_expr_parse.error &&
        match_expr_parse.program &&
        match_expr_parse.program->stmt_count >= 2 &&
        match_expr_parse.program->statements[1] &&
        match_expr_parse.program->statements[1]->kind == STMT_FUNC_DECL) {
        Stmt* match_expr_func = match_expr_parse.program->statements[1];
        Stmt* return_stmt =
            match_expr_func->func_decl.body &&
            match_expr_func->func_decl.body->kind == STMT_BLOCK &&
            match_expr_func->func_decl.body->block.stmt_count > 0
                ? match_expr_func->func_decl.body->block.statements[0]
                : NULL;
        bool parsed_match_expr =
            return_stmt &&
            return_stmt->kind == STMT_RETURN &&
            return_stmt->return_value &&
            return_stmt->return_value->kind == EXPR_MATCH &&
            return_stmt->return_value->match_expr.arm_count == 4 &&
            return_stmt->return_value->match_expr.guards &&
            return_stmt->return_value->match_expr.guards[0] != NULL &&
            return_stmt->return_value->match_expr.else_expr == NULL;

        TypeCheckResult match_expr_tc = typecheck(match_expr_parse.program);
        if (!match_expr_tc.error && parsed_match_expr) {
            CompileResult match_expr_compile = compile(match_expr_parse.program);
            if (!match_expr_compile.error && match_expr_compile.function) {
                tests_passed++;
                printf("  PASS: match expression parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: match expression parse+typecheck+compile\n");
            }

            for (int i = 0; i < match_expr_compile.function_count; i++) {
                if (match_expr_compile.functions && match_expr_compile.functions[i]) {
                    obj_function_free(match_expr_compile.functions[i]);
                }
            }
            if (match_expr_compile.functions) free(match_expr_compile.functions);
            if (match_expr_compile.function) obj_function_free(match_expr_compile.function);
            symbol_table_free(match_expr_compile.globals);
            error_free(match_expr_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: match expression parse+typecheck+compile\n");
        }
        symbol_table_free(match_expr_tc.globals);
        error_free(match_expr_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: match expression parse\n");
    }
    parser_free_result(&match_expr_parse);

    const char* match_expr_requires_else_source =
        "func classify(code: int): int {\n"
        "    return match (code) {\n"
        "        1: 10,\n"
        "        2: 20\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_requires_else_parse = parser_parse(match_expr_requires_else_source, "test.tblo");
    if (!match_expr_requires_else_parse.error) {
        TypeCheckResult match_expr_requires_else_tc = typecheck(match_expr_requires_else_parse.program);
        if (match_expr_requires_else_tc.error &&
            match_expr_requires_else_tc.error->message &&
            strstr(match_expr_requires_else_tc.error->message, "match expression requires an else branch") != NULL) {
            tests_passed++;
            printf("  PASS: match expression else requirement\n");
        } else {
            tests_failed++;
            printf("  FAIL: match expression else requirement\n");
        }
        symbol_table_free(match_expr_requires_else_tc.globals);
        error_free(match_expr_requires_else_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: match expression else requirement parse\n");
    }
    parser_free_result(&match_expr_requires_else_parse);

    const char* match_expr_tuple_witness_source =
        "func classify(pair: (bool, int)): int {\n"
        "    return match (pair) {\n"
        "        (true, value): value\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_tuple_witness_parse =
        parser_parse(match_expr_tuple_witness_source, "test.tblo");
    if (!match_expr_tuple_witness_parse.error) {
        TypeCheckResult match_expr_tuple_witness_tc =
            typecheck(match_expr_tuple_witness_parse.program);
        if (match_expr_tuple_witness_tc.error &&
            match_expr_tuple_witness_tc.error->message &&
            strstr(match_expr_tuple_witness_tc.error->message,
                   "Non-exhaustive match expression: missing (false, _)") != NULL) {
            tests_passed++;
            printf("  PASS: tuple match expression witness diagnostic\n");
        } else {
            tests_failed++;
            printf("  FAIL: tuple match expression witness diagnostic\n");
        }
        symbol_table_free(match_expr_tuple_witness_tc.globals);
        error_free(match_expr_tuple_witness_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: tuple match expression witness parse\n");
    }
    parser_free_result(&match_expr_tuple_witness_parse);

    const char* match_expr_record_witness_source =
        "record Point {\n"
        "    x: int,\n"
        "    y: int\n"
        "};\n"
        "func classify(point: Point): int {\n"
        "    return match (point) {\n"
        "        Point { x: 1, .. }: 1\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_record_witness_parse =
        parser_parse(match_expr_record_witness_source, "test.tblo");
    if (!match_expr_record_witness_parse.error) {
        TypeCheckResult match_expr_record_witness_tc =
            typecheck(match_expr_record_witness_parse.program);
        if (match_expr_record_witness_tc.error &&
            match_expr_record_witness_tc.error->message &&
            strstr(match_expr_record_witness_tc.error->message,
                   "Non-exhaustive match expression: missing Point { x: 0, y: _ }") != NULL) {
            tests_passed++;
            printf("  PASS: record match expression witness diagnostic\n");
        } else {
            tests_failed++;
            printf("  FAIL: record match expression witness diagnostic\n");
        }
        symbol_table_free(match_expr_record_witness_tc.globals);
        error_free(match_expr_record_witness_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: record match expression witness parse\n");
    }
    parser_free_result(&match_expr_record_witness_parse);

    const char* match_expr_guarded_tuple_witness_source =
        "func classify(pair: (bool, int)): int {\n"
        "    return match (pair) {\n"
        "        (true, value) if value > 0: value,\n"
        "        (false, value): value + 10\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_guarded_tuple_witness_parse =
        parser_parse(match_expr_guarded_tuple_witness_source, "test.tblo");
    if (!match_expr_guarded_tuple_witness_parse.error) {
        TypeCheckResult match_expr_guarded_tuple_witness_tc =
            typecheck(match_expr_guarded_tuple_witness_parse.program);
        if (match_expr_guarded_tuple_witness_tc.error &&
            match_expr_guarded_tuple_witness_tc.error->message &&
            strstr(match_expr_guarded_tuple_witness_tc.error->message,
                   "Non-exhaustive match expression: missing (true, _) when a guard is false") != NULL) {
            tests_passed++;
            printf("  PASS: guarded tuple match expression witness diagnostic\n");
        } else {
            tests_failed++;
            printf("  FAIL: guarded tuple match expression witness diagnostic\n");
        }
        symbol_table_free(match_expr_guarded_tuple_witness_tc.globals);
        error_free(match_expr_guarded_tuple_witness_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: guarded tuple match expression witness parse\n");
    }
    parser_free_result(&match_expr_guarded_tuple_witness_parse);

    const char* match_expr_guarded_tuple_specific_witness_source =
        "func classify(pair: (bool, int)): int {\n"
        "    return match (pair) {\n"
        "        (true, 1) if true: 1,\n"
        "        (true, value) if true: value,\n"
        "        (false, value): value + 10\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_guarded_tuple_specific_witness_parse =
        parser_parse(match_expr_guarded_tuple_specific_witness_source, "test.tblo");
    if (!match_expr_guarded_tuple_specific_witness_parse.error) {
        TypeCheckResult match_expr_guarded_tuple_specific_witness_tc =
            typecheck(match_expr_guarded_tuple_specific_witness_parse.program);
        if (match_expr_guarded_tuple_specific_witness_tc.error &&
            match_expr_guarded_tuple_specific_witness_tc.error->message &&
            strstr(match_expr_guarded_tuple_specific_witness_tc.error->message,
                   "Non-exhaustive match expression: missing (true, 1) when a guard is false") != NULL) {
            tests_passed++;
            printf("  PASS: guarded tuple match expression prefers specific witness\n");
        } else {
            tests_failed++;
            printf("  FAIL: guarded tuple match expression prefers specific witness\n");
        }
        symbol_table_free(match_expr_guarded_tuple_specific_witness_tc.globals);
        error_free(match_expr_guarded_tuple_specific_witness_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: guarded tuple specific witness parse\n");
    }
    parser_free_result(&match_expr_guarded_tuple_specific_witness_parse);

    const char* match_expr_guarded_record_specific_witness_source =
        "record Point {\n"
        "    x: int,\n"
        "    y: int\n"
        "};\n"
        "func classify(point: Point): int {\n"
        "    return match (point) {\n"
        "        Point { x: 1, .. } if true: 1,\n"
        "        Point { x, .. } if true: x\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_guarded_record_specific_witness_parse =
        parser_parse(match_expr_guarded_record_specific_witness_source, "test.tblo");
    if (!match_expr_guarded_record_specific_witness_parse.error) {
        TypeCheckResult match_expr_guarded_record_specific_witness_tc =
            typecheck(match_expr_guarded_record_specific_witness_parse.program);
        if (match_expr_guarded_record_specific_witness_tc.error &&
            match_expr_guarded_record_specific_witness_tc.error->message &&
            strstr(match_expr_guarded_record_specific_witness_tc.error->message,
                   "Non-exhaustive match expression: missing Point { x: 1, y: _ } when a guard is false") != NULL) {
            tests_passed++;
            printf("  PASS: guarded record match expression prefers specific witness\n");
        } else {
            tests_failed++;
            printf("  FAIL: guarded record match expression prefers specific witness\n");
        }
        symbol_table_free(match_expr_guarded_record_specific_witness_tc.globals);
        error_free(match_expr_guarded_record_specific_witness_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: guarded record specific witness parse\n");
    }
    parser_free_result(&match_expr_guarded_record_specific_witness_parse);

    const char* match_expr_guarded_enum_witness_source =
        "enum Result {\n"
        "    Ok(int),\n"
        "    Err\n"
        "};\n"
        "func classify(result: Result): int {\n"
        "    return match (result) {\n"
        "        Result.Ok(value) if value > 0: value,\n"
        "        Result.Err: 0\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_guarded_enum_witness_parse =
        parser_parse(match_expr_guarded_enum_witness_source, "test.tblo");
    if (!match_expr_guarded_enum_witness_parse.error) {
        TypeCheckResult match_expr_guarded_enum_witness_tc =
            typecheck(match_expr_guarded_enum_witness_parse.program);
        if (match_expr_guarded_enum_witness_tc.error &&
            match_expr_guarded_enum_witness_tc.error->message &&
            strstr(match_expr_guarded_enum_witness_tc.error->message,
                   "Non-exhaustive match expression: missing Result.Ok(_) when a guard is false") != NULL) {
            tests_passed++;
            printf("  PASS: guarded enum match expression witness diagnostic\n");
        } else {
            tests_failed++;
            printf("  FAIL: guarded enum match expression witness diagnostic\n");
        }
        symbol_table_free(match_expr_guarded_enum_witness_tc.globals);
        error_free(match_expr_guarded_enum_witness_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: guarded enum match expression witness parse\n");
    }
    parser_free_result(&match_expr_guarded_enum_witness_parse);

    const char* match_expr_guarded_enum_specific_witness_source =
        "enum Result {\n"
        "    Ok(bool),\n"
        "    Err\n"
        "};\n"
        "func classify(result: Result): int {\n"
        "    return match (result) {\n"
        "        Result.Ok(true) if true: 1,\n"
        "        Result.Ok(value) if true: 2,\n"
        "        Result.Err: 0\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_guarded_enum_specific_witness_parse =
        parser_parse(match_expr_guarded_enum_specific_witness_source, "test.tblo");
    if (!match_expr_guarded_enum_specific_witness_parse.error) {
        TypeCheckResult match_expr_guarded_enum_specific_witness_tc =
            typecheck(match_expr_guarded_enum_specific_witness_parse.program);
        if (match_expr_guarded_enum_specific_witness_tc.error &&
            match_expr_guarded_enum_specific_witness_tc.error->message &&
            strstr(match_expr_guarded_enum_specific_witness_tc.error->message,
                   "Non-exhaustive match expression: missing Result.Ok(true) when a guard is false") != NULL) {
            tests_passed++;
            printf("  PASS: guarded enum match expression prefers specific witness\n");
        } else {
            tests_failed++;
            printf("  FAIL: guarded enum match expression prefers specific witness\n");
        }
        symbol_table_free(match_expr_guarded_enum_specific_witness_tc.globals);
        error_free(match_expr_guarded_enum_specific_witness_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: guarded enum specific witness parse\n");
    }
    parser_free_result(&match_expr_guarded_enum_specific_witness_parse);

    const char* match_expr_bool_source =
        "func pick(flag: bool): int {\n"
        "    return match (flag) {\n"
        "        true: 1,\n"
        "        false: 2\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_bool_parse = parser_parse(match_expr_bool_source, "test.tblo");
    if (!match_expr_bool_parse.error) {
        TypeCheckResult match_expr_bool_tc = typecheck(match_expr_bool_parse.program);
        if (!match_expr_bool_tc.error) {
            CompileResult match_expr_bool_compile = compile(match_expr_bool_parse.program);
            if (!match_expr_bool_compile.error && match_expr_bool_compile.function) {
                tests_passed++;
                printf("  PASS: exhaustive bool match expression without else\n");
            } else {
                tests_failed++;
                printf("  FAIL: exhaustive bool match expression without else\n");
            }

            for (int i = 0; i < match_expr_bool_compile.function_count; i++) {
                if (match_expr_bool_compile.functions && match_expr_bool_compile.functions[i]) {
                    obj_function_free(match_expr_bool_compile.functions[i]);
                }
            }
            if (match_expr_bool_compile.functions) free(match_expr_bool_compile.functions);
            if (match_expr_bool_compile.function) obj_function_free(match_expr_bool_compile.function);
            symbol_table_free(match_expr_bool_compile.globals);
            error_free(match_expr_bool_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: exhaustive bool match expression without else\n");
        }
        symbol_table_free(match_expr_bool_tc.globals);
        error_free(match_expr_bool_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: exhaustive bool match expression parse\n");
    }
    parser_free_result(&match_expr_bool_parse);

    const char* match_expr_structural_exhaustive_source =
        "record Point {\n"
        "    x: int,\n"
        "    y: int\n"
        "};\n"
        "func sum(pair: (int, int)): int {\n"
        "    return match (pair) {\n"
        "        (left, right): left + right\n"
        "    };\n"
        "}\n"
        "func head(point: Point): int {\n"
        "    return match (point) {\n"
        "        Point { x, .. }: x\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_structural_exhaustive_parse =
        parser_parse(match_expr_structural_exhaustive_source, "test.tblo");
    if (!match_expr_structural_exhaustive_parse.error) {
        TypeCheckResult match_expr_structural_exhaustive_tc =
            typecheck(match_expr_structural_exhaustive_parse.program);
        if (!match_expr_structural_exhaustive_tc.error) {
            CompileResult match_expr_structural_exhaustive_compile =
                compile(match_expr_structural_exhaustive_parse.program);
            if (!match_expr_structural_exhaustive_compile.error &&
                match_expr_structural_exhaustive_compile.function) {
                tests_passed++;
                printf("  PASS: exhaustive tuple/record match expression without else\n");
            } else {
                tests_failed++;
                printf("  FAIL: exhaustive tuple/record match expression without else\n");
            }

            for (int i = 0; i < match_expr_structural_exhaustive_compile.function_count; i++) {
                if (match_expr_structural_exhaustive_compile.functions &&
                    match_expr_structural_exhaustive_compile.functions[i]) {
                    obj_function_free(match_expr_structural_exhaustive_compile.functions[i]);
                }
            }
            if (match_expr_structural_exhaustive_compile.functions) {
                free(match_expr_structural_exhaustive_compile.functions);
            }
            if (match_expr_structural_exhaustive_compile.function) {
                obj_function_free(match_expr_structural_exhaustive_compile.function);
            }
            symbol_table_free(match_expr_structural_exhaustive_compile.globals);
            error_free(match_expr_structural_exhaustive_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: exhaustive tuple/record match expression without else\n");
        }
        symbol_table_free(match_expr_structural_exhaustive_tc.globals);
        error_free(match_expr_structural_exhaustive_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: exhaustive tuple/record match expression parse\n");
    }
    parser_free_result(&match_expr_structural_exhaustive_parse);

    const char* match_expr_structural_partition_source =
        "record Point {\n"
        "    x: int,\n"
        "    y: int\n"
        "};\n"
        "func classify(pair: (bool, int)): int {\n"
        "    return match (pair) {\n"
        "        (true, value): value,\n"
        "        (false, value): value + 10\n"
        "    };\n"
        "}\n"
        "func pointCode(point: Point): int {\n"
        "    return match (point) {\n"
        "        Point { x: 1, .. }: 100,\n"
        "        Point { x, .. }: x\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_structural_partition_parse =
        parser_parse(match_expr_structural_partition_source, "test.tblo");
    if (!match_expr_structural_partition_parse.error) {
        TypeCheckResult match_expr_structural_partition_tc =
            typecheck(match_expr_structural_partition_parse.program);
        if (!match_expr_structural_partition_tc.error) {
            CompileResult match_expr_structural_partition_compile =
                compile(match_expr_structural_partition_parse.program);
            if (!match_expr_structural_partition_compile.error &&
                match_expr_structural_partition_compile.function) {
                tests_passed++;
                printf("  PASS: partial tuple/record partition match expression without else\n");
            } else {
                tests_failed++;
                printf("  FAIL: partial tuple/record partition match expression without else\n");
            }

            for (int i = 0; i < match_expr_structural_partition_compile.function_count; i++) {
                if (match_expr_structural_partition_compile.functions &&
                    match_expr_structural_partition_compile.functions[i]) {
                    obj_function_free(match_expr_structural_partition_compile.functions[i]);
                }
            }
            if (match_expr_structural_partition_compile.functions) {
                free(match_expr_structural_partition_compile.functions);
            }
            if (match_expr_structural_partition_compile.function) {
                obj_function_free(match_expr_structural_partition_compile.function);
            }
            symbol_table_free(match_expr_structural_partition_compile.globals);
            error_free(match_expr_structural_partition_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: partial tuple/record partition match expression without else\n");
        }
        symbol_table_free(match_expr_structural_partition_tc.globals);
        error_free(match_expr_structural_partition_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: partial tuple/record partition match expression parse\n");
    }
    parser_free_result(&match_expr_structural_partition_parse);

    const char* match_expr_structural_matrix_source =
        "record Flagged {\n"
        "    flag: bool,\n"
        "    code: int\n"
        "};\n"
        "func classifyTuple(pair: (bool, int)): int {\n"
        "    return match (pair) {\n"
        "        (true, 1): 11,\n"
        "        (true, value): value,\n"
        "        (false, 1): 21,\n"
        "        (false, value): value + 10\n"
        "    };\n"
        "}\n"
        "func classifyRecord(value: Flagged): int {\n"
        "    return match (value) {\n"
        "        Flagged { flag: true, code: 1 }: 11,\n"
        "        Flagged { flag: true, code }: code,\n"
        "        Flagged { flag: false, code: 1 }: 21,\n"
        "        Flagged { flag: false, code }: code + 10\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_structural_matrix_parse =
        parser_parse(match_expr_structural_matrix_source, "test.tblo");
    if (!match_expr_structural_matrix_parse.error) {
        TypeCheckResult match_expr_structural_matrix_tc =
            typecheck(match_expr_structural_matrix_parse.program);
        if (!match_expr_structural_matrix_tc.error) {
            CompileResult match_expr_structural_matrix_compile =
                compile(match_expr_structural_matrix_parse.program);
            if (!match_expr_structural_matrix_compile.error &&
                match_expr_structural_matrix_compile.function) {
                tests_passed++;
                printf("  PASS: recursive tuple/record partition match expression without else\n");
            } else {
                tests_failed++;
                printf("  FAIL: recursive tuple/record partition match expression without else\n");
            }

            for (int i = 0; i < match_expr_structural_matrix_compile.function_count; i++) {
                if (match_expr_structural_matrix_compile.functions &&
                    match_expr_structural_matrix_compile.functions[i]) {
                    obj_function_free(match_expr_structural_matrix_compile.functions[i]);
                }
            }
            if (match_expr_structural_matrix_compile.functions) {
                free(match_expr_structural_matrix_compile.functions);
            }
            if (match_expr_structural_matrix_compile.function) {
                obj_function_free(match_expr_structural_matrix_compile.function);
            }
            symbol_table_free(match_expr_structural_matrix_compile.globals);
            error_free(match_expr_structural_matrix_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: recursive tuple/record partition match expression without else\n");
        }
        symbol_table_free(match_expr_structural_matrix_tc.globals);
        error_free(match_expr_structural_matrix_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: recursive tuple/record partition match expression parse\n");
    }
    parser_free_result(&match_expr_structural_matrix_parse);

    const char* match_expr_type_mismatch_source =
        "func bad(flag: bool): int {\n"
        "    return match (flag) {\n"
        "        true: 1,\n"
        "        false: \"nope\"\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_type_mismatch_parse = parser_parse(match_expr_type_mismatch_source, "test.tblo");
    if (!match_expr_type_mismatch_parse.error) {
        TypeCheckResult match_expr_type_mismatch_tc = typecheck(match_expr_type_mismatch_parse.program);
        if (match_expr_type_mismatch_tc.error &&
            match_expr_type_mismatch_tc.error->message &&
            strstr(match_expr_type_mismatch_tc.error->message, "match expression arm type mismatch") != NULL) {
            tests_passed++;
            printf("  PASS: match expression arm type mismatch rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: match expression arm type mismatch rejection\n");
        }
        symbol_table_free(match_expr_type_mismatch_tc.globals);
        error_free(match_expr_type_mismatch_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: match expression arm type mismatch parse\n");
    }
    parser_free_result(&match_expr_type_mismatch_parse);

    const char* match_expr_block_source =
        "func choose(flag: bool): int {\n"
        "    return match (flag) {\n"
        "        true: { var base: int = 1; base + 4 },\n"
        "        false: { var base: int = 2; if (base > 1) { base = base + 3; } base }\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_block_parse = parser_parse(match_expr_block_source, "test.tblo");
    if (!match_expr_block_parse.error &&
        match_expr_block_parse.program &&
        match_expr_block_parse.program->stmt_count > 0 &&
        match_expr_block_parse.program->statements[0] &&
        match_expr_block_parse.program->statements[0]->kind == STMT_FUNC_DECL) {
        Stmt* block_func = match_expr_block_parse.program->statements[0];
        Stmt* return_stmt =
            block_func->func_decl.body &&
            block_func->func_decl.body->kind == STMT_BLOCK &&
            block_func->func_decl.body->block.stmt_count > 0
                ? block_func->func_decl.body->block.statements[0]
                : NULL;
        Expr* match_expr = return_stmt && return_stmt->kind == STMT_RETURN
            ? return_stmt->return_value
            : NULL;
        bool parsed_block_arms =
            match_expr &&
            match_expr->kind == EXPR_MATCH &&
            match_expr->match_expr.arm_count == 2 &&
            match_expr->match_expr.values &&
            match_expr->match_expr.values[0] &&
            match_expr->match_expr.values[0]->kind == EXPR_BLOCK &&
            match_expr->match_expr.values[0]->block_expr.stmt_count == 1 &&
            match_expr->match_expr.values[1] &&
            match_expr->match_expr.values[1]->kind == EXPR_BLOCK &&
            match_expr->match_expr.values[1]->block_expr.stmt_count == 2;

        TypeCheckResult match_expr_block_tc = typecheck(match_expr_block_parse.program);
        if (!match_expr_block_tc.error && parsed_block_arms) {
            CompileResult match_expr_block_compile = compile(match_expr_block_parse.program);
            if (!match_expr_block_compile.error && match_expr_block_compile.function) {
                tests_passed++;
                printf("  PASS: match expression block arms parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: match expression block arms parse+typecheck+compile\n");
            }

            for (int i = 0; i < match_expr_block_compile.function_count; i++) {
                if (match_expr_block_compile.functions && match_expr_block_compile.functions[i]) {
                    obj_function_free(match_expr_block_compile.functions[i]);
                }
            }
            if (match_expr_block_compile.functions) free(match_expr_block_compile.functions);
            if (match_expr_block_compile.function) obj_function_free(match_expr_block_compile.function);
            symbol_table_free(match_expr_block_compile.globals);
            error_free(match_expr_block_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: match expression block arms parse+typecheck+compile\n");
        }
        symbol_table_free(match_expr_block_tc.globals);
        error_free(match_expr_block_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: match expression block arms parse\n");
    }
    parser_free_result(&match_expr_block_parse);

    const char* match_expr_block_missing_value_source =
        "func bad(flag: bool): int {\n"
        "    return match (flag) {\n"
        "        true: { var base: int = 1; },\n"
        "        false: 0\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_block_missing_value_parse =
        parser_parse(match_expr_block_missing_value_source, "test.tblo");
    if (match_expr_block_missing_value_parse.error &&
        match_expr_block_missing_value_parse.error->message &&
        strstr(match_expr_block_missing_value_parse.error->message,
               "Block expression requires a trailing value expression") != NULL) {
        tests_passed++;
        printf("  PASS: block expression missing trailing value rejection\n");
    } else {
        tests_failed++;
        printf("  FAIL: block expression missing trailing value rejection\n");
    }
    parser_free_result(&match_expr_block_missing_value_parse);

    const char* match_expr_record_literal_source =
        "func pick(flag: bool) {\n"
        "    var value = match (flag) {\n"
        "        true: { answer: 1 },\n"
        "        false: { answer: 2 }\n"
        "    };\n"
        "}\n";
    ParseResult match_expr_record_literal_parse =
        parser_parse(match_expr_record_literal_source, "test.tblo");
    if (!match_expr_record_literal_parse.error &&
        match_expr_record_literal_parse.program &&
        match_expr_record_literal_parse.program->stmt_count > 0 &&
        match_expr_record_literal_parse.program->statements[0] &&
        match_expr_record_literal_parse.program->statements[0]->kind == STMT_FUNC_DECL) {
        Stmt* record_func = match_expr_record_literal_parse.program->statements[0];
        Stmt* decl_stmt =
            record_func->func_decl.body &&
            record_func->func_decl.body->kind == STMT_BLOCK &&
            record_func->func_decl.body->block.stmt_count > 0
                ? record_func->func_decl.body->block.statements[0]
                : NULL;
        Expr* init_expr =
            decl_stmt &&
            decl_stmt->kind == STMT_VAR_DECL
                ? decl_stmt->var_decl.initializer
                : NULL;
        bool parsed_record_literal_arms =
            init_expr &&
            init_expr->kind == EXPR_MATCH &&
            init_expr->match_expr.arm_count == 2 &&
            init_expr->match_expr.values &&
            init_expr->match_expr.values[0] &&
            init_expr->match_expr.values[0]->kind == EXPR_RECORD_LITERAL &&
            init_expr->match_expr.values[1] &&
            init_expr->match_expr.values[1]->kind == EXPR_RECORD_LITERAL;

        if (parsed_record_literal_arms) {
            tests_passed++;
            printf("  PASS: match expression brace literal disambiguation\n");
        } else {
            tests_failed++;
            printf("  FAIL: match expression brace literal disambiguation\n");
        }
    } else {
        tests_failed++;
        printf("  FAIL: match expression brace literal disambiguation parse\n");
    }
    parser_free_result(&match_expr_record_literal_parse);

    const char* if_expr_source =
        "func choose(flag: bool): int {\n"
        "    return if (flag) { var base: int = 1; base + 4 } else if (false) { 0 } else { var base: int = 3; base * 2 };\n"
        "}\n";
    ParseResult if_expr_parse = parser_parse(if_expr_source, "test.tblo");
    if (!if_expr_parse.error &&
        if_expr_parse.program &&
        if_expr_parse.program->stmt_count > 0 &&
        if_expr_parse.program->statements[0] &&
        if_expr_parse.program->statements[0]->kind == STMT_FUNC_DECL) {
        Stmt* if_func = if_expr_parse.program->statements[0];
        Stmt* return_stmt =
            if_func->func_decl.body &&
            if_func->func_decl.body->kind == STMT_BLOCK &&
            if_func->func_decl.body->block.stmt_count > 0
                ? if_func->func_decl.body->block.statements[0]
                : NULL;
        Expr* if_expr =
            return_stmt && return_stmt->kind == STMT_RETURN
                ? return_stmt->return_value
                : NULL;
        bool parsed_if_expr =
            if_expr &&
            if_expr->kind == EXPR_IF &&
            if_expr->if_expr.then_expr &&
            if_expr->if_expr.then_expr->kind == EXPR_BLOCK &&
            if_expr->if_expr.else_expr &&
            if_expr->if_expr.else_expr->kind == EXPR_IF;

        TypeCheckResult if_expr_tc = typecheck(if_expr_parse.program);
        if (!if_expr_tc.error && parsed_if_expr) {
            CompileResult if_expr_compile = compile(if_expr_parse.program);
            if (!if_expr_compile.error && if_expr_compile.function) {
                tests_passed++;
                printf("  PASS: if expression parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: if expression parse+typecheck+compile\n");
            }

            for (int i = 0; i < if_expr_compile.function_count; i++) {
                if (if_expr_compile.functions && if_expr_compile.functions[i]) {
                    obj_function_free(if_expr_compile.functions[i]);
                }
            }
            if (if_expr_compile.functions) free(if_expr_compile.functions);
            if (if_expr_compile.function) obj_function_free(if_expr_compile.function);
            symbol_table_free(if_expr_compile.globals);
            error_free(if_expr_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: if expression parse+typecheck+compile\n");
        }
        symbol_table_free(if_expr_tc.globals);
        error_free(if_expr_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: if expression parse\n");
    }
    parser_free_result(&if_expr_parse);

    const char* if_expr_missing_else_source =
        "func choose(flag: bool): int {\n"
        "    return if (flag) { 1 };\n"
        "}\n";
    ParseResult if_expr_missing_else_parse = parser_parse(if_expr_missing_else_source, "test.tblo");
    if (if_expr_missing_else_parse.error &&
        if_expr_missing_else_parse.error->message &&
        strstr(if_expr_missing_else_parse.error->message, "if expression requires an else branch") != NULL) {
        tests_passed++;
        printf("  PASS: if expression else requirement\n");
    } else {
        tests_failed++;
        printf("  FAIL: if expression else requirement\n");
    }
    parser_free_result(&if_expr_missing_else_parse);

    const char* if_expr_type_mismatch_source =
        "func choose(flag: bool): int {\n"
        "    return if (flag) { 1 } else { \"nope\" };\n"
        "}\n";
    ParseResult if_expr_type_mismatch_parse = parser_parse(if_expr_type_mismatch_source, "test.tblo");
    if (!if_expr_type_mismatch_parse.error) {
        TypeCheckResult if_expr_type_mismatch_tc = typecheck(if_expr_type_mismatch_parse.program);
        if (if_expr_type_mismatch_tc.error &&
            if_expr_type_mismatch_tc.error->message &&
            strstr(if_expr_type_mismatch_tc.error->message, "if expression branch type mismatch") != NULL) {
            tests_passed++;
            printf("  PASS: if expression branch type mismatch rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: if expression branch type mismatch rejection\n");
        }
        symbol_table_free(if_expr_type_mismatch_tc.globals);
        error_free(if_expr_type_mismatch_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: if expression branch type mismatch parse\n");
    }
    parser_free_result(&if_expr_type_mismatch_parse);

    const char* if_expr_condition_source =
        "func choose(): int {\n"
        "    return if (1) { 1 } else { 2 };\n"
        "}\n";
    ParseResult if_expr_condition_parse = parser_parse(if_expr_condition_source, "test.tblo");
    if (!if_expr_condition_parse.error) {
        TypeCheckResult if_expr_condition_tc = typecheck(if_expr_condition_parse.program);
        if (if_expr_condition_tc.error &&
            if_expr_condition_tc.error->message &&
            strstr(if_expr_condition_tc.error->message, "if expression condition must be bool") != NULL) {
            tests_passed++;
            printf("  PASS: if expression bool condition enforcement\n");
        } else {
            tests_failed++;
            printf("  FAIL: if expression bool condition enforcement\n");
        }
        symbol_table_free(if_expr_condition_tc.globals);
        error_free(if_expr_condition_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: if expression bool condition parse\n");
    }
    parser_free_result(&if_expr_condition_parse);

    const char* if_expr_literal_source =
        "func choose(flag: bool) {\n"
        "    var value = if (flag) ({ answer: 1 }) else ({ answer: 2 });\n"
        "}\n";
    ParseResult if_expr_literal_parse = parser_parse(if_expr_literal_source, "test.tblo");
    if (!if_expr_literal_parse.error &&
        if_expr_literal_parse.program &&
        if_expr_literal_parse.program->stmt_count > 0 &&
        if_expr_literal_parse.program->statements[0] &&
        if_expr_literal_parse.program->statements[0]->kind == STMT_FUNC_DECL) {
        Stmt* literal_func = if_expr_literal_parse.program->statements[0];
        Stmt* decl_stmt =
            literal_func->func_decl.body &&
            literal_func->func_decl.body->kind == STMT_BLOCK &&
            literal_func->func_decl.body->block.stmt_count > 0
                ? literal_func->func_decl.body->block.statements[0]
                : NULL;
        Expr* init_expr =
            decl_stmt && decl_stmt->kind == STMT_VAR_DECL
                ? decl_stmt->var_decl.initializer
                : NULL;
        bool parsed_literal_branches =
            init_expr &&
            init_expr->kind == EXPR_IF &&
            init_expr->if_expr.then_expr &&
            init_expr->if_expr.then_expr->kind == EXPR_RECORD_LITERAL &&
            init_expr->if_expr.else_expr &&
            init_expr->if_expr.else_expr->kind == EXPR_RECORD_LITERAL;

        if (parsed_literal_branches) {
            tests_passed++;
            printf("  PASS: if expression parenthesized brace literal branches\n");
        } else {
            tests_failed++;
            printf("  FAIL: if expression parenthesized brace literal branches\n");
        }
    } else {
        tests_failed++;
        printf("  FAIL: if expression parenthesized brace literal parse\n");
    }
    parser_free_result(&if_expr_literal_parse);

    const char* pattern_alt_match_source =
        "func classify(value: int): int {\n"
        "    match (value) {\n"
        "        1 | 2: return 10;\n"
        "        else: return 0;\n"
        "    }\n"
        "}\n";
    ParseResult pattern_alt_match_parse = parser_parse(pattern_alt_match_source, "test.tblo");
    if (!pattern_alt_match_parse.error &&
        pattern_alt_match_parse.program &&
        pattern_alt_match_parse.program->stmt_count > 0 &&
        pattern_alt_match_parse.program->statements[0] &&
        pattern_alt_match_parse.program->statements[0]->kind == STMT_FUNC_DECL) {
        Stmt* alt_func = pattern_alt_match_parse.program->statements[0];
        Stmt* match_stmt =
            alt_func->func_decl.body &&
            alt_func->func_decl.body->kind == STMT_BLOCK &&
            alt_func->func_decl.body->block.stmt_count > 0
                ? alt_func->func_decl.body->block.statements[0]
                : NULL;
        bool expanded_alt_patterns =
            match_stmt &&
            match_stmt->kind == STMT_MATCH &&
            match_stmt->match_stmt.arm_count == 2;

        TypeCheckResult pattern_alt_match_tc = typecheck(pattern_alt_match_parse.program);
        if (!pattern_alt_match_tc.error && expanded_alt_patterns) {
            CompileResult pattern_alt_match_compile = compile(pattern_alt_match_parse.program);
            if (!pattern_alt_match_compile.error && pattern_alt_match_compile.function) {
                tests_passed++;
                printf("  PASS: match pattern alternation parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: match pattern alternation parse+typecheck+compile\n");
            }

            for (int i = 0; i < pattern_alt_match_compile.function_count; i++) {
                if (pattern_alt_match_compile.functions && pattern_alt_match_compile.functions[i]) {
                    obj_function_free(pattern_alt_match_compile.functions[i]);
                }
            }
            if (pattern_alt_match_compile.functions) free(pattern_alt_match_compile.functions);
            if (pattern_alt_match_compile.function) obj_function_free(pattern_alt_match_compile.function);
            symbol_table_free(pattern_alt_match_compile.globals);
            error_free(pattern_alt_match_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: match pattern alternation parse+typecheck+compile\n");
        }
        symbol_table_free(pattern_alt_match_tc.globals);
        error_free(pattern_alt_match_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: match pattern alternation parse\n");
    }
    parser_free_result(&pattern_alt_match_parse);

    const char* pattern_alt_bit_or_source =
        "func classify(value: int): int {\n"
        "    match (value) {\n"
        "        (1 | 2): return 10;\n"
        "        else: return 0;\n"
        "    }\n"
        "}\n";
    ParseResult pattern_alt_bit_or_parse = parser_parse(pattern_alt_bit_or_source, "test.tblo");
    if (!pattern_alt_bit_or_parse.error &&
        pattern_alt_bit_or_parse.program &&
        pattern_alt_bit_or_parse.program->stmt_count > 0 &&
        pattern_alt_bit_or_parse.program->statements[0] &&
        pattern_alt_bit_or_parse.program->statements[0]->kind == STMT_FUNC_DECL) {
        Stmt* bit_or_func = pattern_alt_bit_or_parse.program->statements[0];
        Stmt* match_stmt =
            bit_or_func->func_decl.body &&
            bit_or_func->func_decl.body->kind == STMT_BLOCK &&
            bit_or_func->func_decl.body->block.stmt_count > 0
                ? bit_or_func->func_decl.body->block.statements[0]
                : NULL;
        bool preserved_bit_or_pattern =
            match_stmt &&
            match_stmt->kind == STMT_MATCH &&
            match_stmt->match_stmt.arm_count == 1 &&
            match_stmt->match_stmt.patterns &&
            match_stmt->match_stmt.patterns[0] &&
            match_stmt->match_stmt.patterns[0]->kind == EXPR_BINARY &&
            match_stmt->match_stmt.patterns[0]->binary.op == TOKEN_BIT_OR;

        if (preserved_bit_or_pattern) {
            tests_passed++;
            printf("  PASS: parenthesized bitwise or remains a single pattern\n");
        } else {
            tests_failed++;
            printf("  FAIL: parenthesized bitwise or remains a single pattern\n");
        }
    } else {
        tests_failed++;
        printf("  FAIL: parenthesized bitwise or pattern parse\n");
    }
    parser_free_result(&pattern_alt_bit_or_parse);

    const char* pattern_alt_let_source =
        "func classify(value: int): int {\n"
        "    if let 1 | 2 = value {\n"
        "        return 10;\n"
        "    } else {\n"
        "        return 0;\n"
        "    }\n"
        "}\n"
        "func countLoop(): int {\n"
        "    var value: int = 1;\n"
        "    var count: int = 0;\n"
        "    while let 1 | 2 = value {\n"
        "        count = count + 1;\n"
        "        value = value + 1;\n"
        "    }\n"
        "    return count;\n"
        "}\n";
    ParseResult pattern_alt_let_parse = parser_parse(pattern_alt_let_source, "test.tblo");
    if (!pattern_alt_let_parse.error) {
        TypeCheckResult pattern_alt_let_tc = typecheck(pattern_alt_let_parse.program);
        if (!pattern_alt_let_tc.error) {
            CompileResult pattern_alt_let_compile = compile(pattern_alt_let_parse.program);
            if (!pattern_alt_let_compile.error && pattern_alt_let_compile.function) {
                tests_passed++;
                printf("  PASS: if let / while let pattern alternation parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: if let / while let pattern alternation parse+typecheck+compile\n");
            }

            for (int i = 0; i < pattern_alt_let_compile.function_count; i++) {
                if (pattern_alt_let_compile.functions && pattern_alt_let_compile.functions[i]) {
                    obj_function_free(pattern_alt_let_compile.functions[i]);
                }
            }
            if (pattern_alt_let_compile.functions) free(pattern_alt_let_compile.functions);
            if (pattern_alt_let_compile.function) obj_function_free(pattern_alt_let_compile.function);
            symbol_table_free(pattern_alt_let_compile.globals);
            error_free(pattern_alt_let_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: if let / while let pattern alternation parse+typecheck+compile\n");
        }
        symbol_table_free(pattern_alt_let_tc.globals);
        error_free(pattern_alt_let_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: if let / while let pattern alternation parse\n");
    }
    parser_free_result(&pattern_alt_let_parse);

    const char* pattern_alt_payload_source =
        "enum Packet {\n"
        "    Left(int, string),\n"
        "    Right(string, int)\n"
        "};\n"
        "func unwrap(packet: Packet): int {\n"
        "    return match (packet) {\n"
        "        Packet.Left(value, _) | Packet.Right(_, value): value\n"
        "    };\n"
        "}\n";
    ParseResult pattern_alt_payload_parse = parser_parse(pattern_alt_payload_source, "test.tblo");
    if (!pattern_alt_payload_parse.error) {
        TypeCheckResult pattern_alt_payload_tc = typecheck(pattern_alt_payload_parse.program);
        if (!pattern_alt_payload_tc.error) {
            CompileResult pattern_alt_payload_compile = compile(pattern_alt_payload_parse.program);
            if (!pattern_alt_payload_compile.error && pattern_alt_payload_compile.function) {
                tests_passed++;
                printf("  PASS: pattern alternation payload bindings support shared names across positions\n");
            } else {
                tests_failed++;
                printf("  FAIL: pattern alternation payload bindings support shared names across positions\n");
            }

            for (int i = 0; i < pattern_alt_payload_compile.function_count; i++) {
                if (pattern_alt_payload_compile.functions && pattern_alt_payload_compile.functions[i]) {
                    obj_function_free(pattern_alt_payload_compile.functions[i]);
                }
            }
            if (pattern_alt_payload_compile.functions) free(pattern_alt_payload_compile.functions);
            if (pattern_alt_payload_compile.function) obj_function_free(pattern_alt_payload_compile.function);
            symbol_table_free(pattern_alt_payload_compile.globals);
            error_free(pattern_alt_payload_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: pattern alternation payload bindings support shared names across positions\n");
        }
        symbol_table_free(pattern_alt_payload_tc.globals);
        error_free(pattern_alt_payload_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: pattern alternation payload bindings parse\n");
    }
    parser_free_result(&pattern_alt_payload_parse);

    const char* pattern_alt_binding_name_mismatch_source =
        "enum Packet {\n"
        "    Left(int),\n"
        "    Right(int)\n"
        "};\n"
        "func unwrap(packet: Packet): int {\n"
        "    return match (packet) {\n"
        "        Packet.Left(left) | Packet.Right(right): 0\n"
        "    };\n"
        "}\n";
    ParseResult pattern_alt_binding_name_mismatch_parse =
        parser_parse(pattern_alt_binding_name_mismatch_source, "test.tblo");
    if (!pattern_alt_binding_name_mismatch_parse.error) {
        TypeCheckResult pattern_alt_binding_name_mismatch_tc =
            typecheck(pattern_alt_binding_name_mismatch_parse.program);
        if (pattern_alt_binding_name_mismatch_tc.error &&
            pattern_alt_binding_name_mismatch_tc.error->message &&
            strstr(pattern_alt_binding_name_mismatch_tc.error->message,
                   "Pattern alternatives in the same arm must bind the same names") != NULL) {
            tests_passed++;
            printf("  PASS: pattern alternation binding name mismatch diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: pattern alternation binding name mismatch diagnostics\n");
        }
        symbol_table_free(pattern_alt_binding_name_mismatch_tc.globals);
        error_free(pattern_alt_binding_name_mismatch_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: pattern alternation binding name mismatch parse\n");
    }
    parser_free_result(&pattern_alt_binding_name_mismatch_parse);

    const char* pattern_alt_binding_type_mismatch_source =
        "enum Packet {\n"
        "    Left(int),\n"
        "    Right(string)\n"
        "};\n"
        "func unwrap(packet: Packet): int {\n"
        "    return match (packet) {\n"
        "        Packet.Left(value) | Packet.Right(value): 0\n"
        "    };\n"
        "}\n";
    ParseResult pattern_alt_binding_type_mismatch_parse =
        parser_parse(pattern_alt_binding_type_mismatch_source, "test.tblo");
    if (!pattern_alt_binding_type_mismatch_parse.error) {
        TypeCheckResult pattern_alt_binding_type_mismatch_tc =
            typecheck(pattern_alt_binding_type_mismatch_parse.program);
        if (pattern_alt_binding_type_mismatch_tc.error &&
            pattern_alt_binding_type_mismatch_tc.error->message &&
            strstr(pattern_alt_binding_type_mismatch_tc.error->message,
                   "Pattern alternatives in the same arm must bind 'value' with compatible types") != NULL) {
            tests_passed++;
            printf("  PASS: pattern alternation binding type mismatch diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: pattern alternation binding type mismatch diagnostics\n");
        }
        symbol_table_free(pattern_alt_binding_type_mismatch_tc.globals);
        error_free(pattern_alt_binding_type_mismatch_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: pattern alternation binding type mismatch parse\n");
    }
    parser_free_result(&pattern_alt_binding_type_mismatch_parse);

    const char* structural_pattern_source =
        "record Point {\n"
        "    x: int,\n"
        "    y: int\n"
        "};\n"
        "enum Wrapped {\n"
        "    Pair((int, int)),\n"
        "    Pointed(Point),\n"
        "    Empty\n"
        "};\n"
        "func tupleSum(pair: (int, int)): int {\n"
        "    match (pair) {\n"
        "        (left, right): return left + right;\n"
        "    }\n"
        "}\n"
        "func pointSum(point: Point): int {\n"
        "    match (point) {\n"
        "        { x: px, y: py }: return px + py;\n"
        "    }\n"
        "}\n"
        "func pointX(point: Point): int {\n"
        "    match (point) {\n"
        "        Point { x, .. }: return x;\n"
        "    }\n"
        "}\n"
        "func unwrapPair(value: Wrapped): int {\n"
        "    if let Wrapped.Pair((left, right)) = value {\n"
        "        return left * right;\n"
        "    } else {\n"
        "        return 0;\n"
        "    }\n"
        "}\n"
        "func unwrapPoint(value: Wrapped): int {\n"
        "    return match (value) {\n"
        "        Wrapped.Pointed({ x: x, y: y }): x - y,\n"
        "        else: 0\n"
        "    };\n"
        "}\n"
        "func unwrapTypedPoint(value: Wrapped): int {\n"
        "    return match (value) {\n"
        "        Wrapped.Pointed(Point { x, .. }): x,\n"
        "        else: 0\n"
        "    };\n"
        "}\n";
    ParseResult structural_pattern_parse = parser_parse(structural_pattern_source, "test.tblo");
    if (!structural_pattern_parse.error) {
        TypeCheckResult structural_pattern_tc = typecheck(structural_pattern_parse.program);
        if (!structural_pattern_tc.error) {
            CompileResult structural_pattern_compile = compile(structural_pattern_parse.program);
            if (!structural_pattern_compile.error && structural_pattern_compile.function) {
                tests_passed++;
                printf("  PASS: tuple and record destructuring patterns parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: tuple and record destructuring patterns parse+typecheck+compile\n");
            }

            for (int i = 0; i < structural_pattern_compile.function_count; i++) {
                if (structural_pattern_compile.functions && structural_pattern_compile.functions[i]) {
                    obj_function_free(structural_pattern_compile.functions[i]);
                }
            }
            if (structural_pattern_compile.functions) free(structural_pattern_compile.functions);
            if (structural_pattern_compile.function) obj_function_free(structural_pattern_compile.function);
            symbol_table_free(structural_pattern_compile.globals);
            error_free(structural_pattern_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: tuple and record destructuring patterns parse+typecheck+compile\n");
        }
        symbol_table_free(structural_pattern_tc.globals);
        error_free(structural_pattern_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: tuple and record destructuring patterns parse\n");
    }
    parser_free_result(&structural_pattern_parse);

    const char* structural_pattern_any_source =
        "func bad(value: any): int {\n"
        "    match (value) {\n"
        "        (left, right): return left;\n"
        "        else: return 0;\n"
        "    }\n"
        "}\n";
    ParseResult structural_pattern_any_parse = parser_parse(structural_pattern_any_source, "test.tblo");
    if (!structural_pattern_any_parse.error) {
        TypeCheckResult structural_pattern_any_tc = typecheck(structural_pattern_any_parse.program);
        if (structural_pattern_any_tc.error &&
            structural_pattern_any_tc.error->message &&
            strstr(structural_pattern_any_tc.error->message,
                   "Tuple and record patterns require a non-any subject type") != NULL) {
            tests_passed++;
            printf("  PASS: tuple pattern on any subject diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: tuple pattern on any subject diagnostics\n");
        }
        symbol_table_free(structural_pattern_any_tc.globals);
        error_free(structural_pattern_any_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: tuple pattern on any subject parse\n");
    }
    parser_free_result(&structural_pattern_any_parse);

    const char* structural_pattern_missing_fields_source =
        "record Point {\n"
        "    x: int,\n"
        "    y: int\n"
        "};\n"
        "func bad(point: Point): int {\n"
        "    match (point) {\n"
        "        Point { x }: return x;\n"
        "    }\n"
        "}\n";
    ParseResult structural_pattern_missing_fields_parse =
        parser_parse(structural_pattern_missing_fields_source, "test.tblo");
    if (!structural_pattern_missing_fields_parse.error) {
        TypeCheckResult structural_pattern_missing_fields_tc =
            typecheck(structural_pattern_missing_fields_parse.program);
        if (structural_pattern_missing_fields_tc.error &&
            structural_pattern_missing_fields_tc.error->message &&
            strstr(structural_pattern_missing_fields_tc.error->message,
                   "Missing fields in record pattern: y") != NULL) {
            tests_passed++;
            printf("  PASS: partial record pattern requires '..' for omitted fields\n");
        } else {
            tests_failed++;
            printf("  FAIL: partial record pattern requires '..' for omitted fields\n");
        }
        symbol_table_free(structural_pattern_missing_fields_tc.globals);
        error_free(structural_pattern_missing_fields_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: partial record pattern missing '..' parse\n");
    }
    parser_free_result(&structural_pattern_missing_fields_parse);

    const char* switch_parse_source =
        "func classify(code: int): int {\n"
        "    switch (code) {\n"
        "        case 1, 2: return 10;\n"
        "        case 3: return 20;\n"
        "        default: return 0;\n"
        "    }\n"
        "    return -1;\n"
        "}\n";
    ParseResult switch_parse = parser_parse(switch_parse_source, "test.tblo");
    if (!switch_parse.error &&
        switch_parse.program &&
        switch_parse.program->stmt_count > 0 &&
        switch_parse.program->statements[0] &&
        switch_parse.program->statements[0]->kind == STMT_FUNC_DECL) {
        Stmt* switch_func = switch_parse.program->statements[0];
        bool lowered_to_match =
            switch_func->func_decl.body &&
            switch_func->func_decl.body->kind == STMT_BLOCK &&
            switch_func->func_decl.body->block.stmt_count > 0 &&
            switch_func->func_decl.body->block.statements[0] &&
            switch_func->func_decl.body->block.statements[0]->kind == STMT_MATCH &&
            switch_func->func_decl.body->block.statements[0]->match_stmt.arm_count == 3 &&
            switch_func->func_decl.body->block.statements[0]->match_stmt.else_branch != NULL;

        TypeCheckResult switch_tc = typecheck(switch_parse.program);
        if (!switch_tc.error && lowered_to_match) {
            CompileResult switch_compile = compile(switch_parse.program);
            if (!switch_compile.error && switch_compile.function) {
                tests_passed++;
                printf("  PASS: switch statement lowers to match parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: switch statement lowers to match parse+typecheck+compile\n");
            }

            for (int i = 0; i < switch_compile.function_count; i++) {
                if (switch_compile.functions && switch_compile.functions[i]) {
                    obj_function_free(switch_compile.functions[i]);
                }
            }
            if (switch_compile.functions) free(switch_compile.functions);
            if (switch_compile.function) obj_function_free(switch_compile.function);
            symbol_table_free(switch_compile.globals);
            error_free(switch_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: switch statement lowers to match parse+typecheck+compile\n");
        }
        symbol_table_free(switch_tc.globals);
        error_free(switch_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: switch statement lowers to match parse+typecheck+compile\n");
    }
    parser_free_result(&switch_parse);

    const char* switch_runtime_source =
        "enum Status { Ok = 200, NotFound = 404, Retry = 429 };\n"
        "func classifyCode(code: int): int {\n"
        "    switch (code) {\n"
        "        case 200, 201: return 7;\n"
        "        case 404: return 8;\n"
        "        default: return 9;\n"
        "    }\n"
        "    return -1;\n"
        "}\n"
        "func classifyLabel(label: string): int {\n"
        "    switch (label) {\n"
        "        case \"ready\", \"set\": return 1;\n"
        "        default: return 0;\n"
        "    }\n"
        "    return -1;\n"
        "}\n"
        "func classifyFlag(flag: bool): int {\n"
        "    switch (flag) {\n"
        "        case true: return 1;\n"
        "        default: return 2;\n"
        "    }\n"
        "    return -1;\n"
        "}\n"
        "func classifyStatus(status: Status): int {\n"
        "    switch (status) {\n"
        "        case Status.Ok: return 7;\n"
        "        case Status.NotFound: return 9;\n"
        "        default: return 0;\n"
        "    }\n"
        "    return -1;\n"
        "}\n"
        "var outCodeOk: int = classifyCode(200);\n"
        "var outCodeOther: int = classifyCode(500);\n"
        "var outLabel: int = classifyLabel(\"ready\");\n"
        "var outFlag: int = classifyFlag(false);\n"
        "var outStatus: int = classifyStatus(Status.NotFound);\n";
    ParseResult switch_runtime_parse = parser_parse(switch_runtime_source, "test.tblo");
    if (!switch_runtime_parse.error) {
        TypeCheckResult switch_runtime_tc = typecheck(switch_runtime_parse.program);
        if (!switch_runtime_tc.error) {
            CompileResult switch_runtime_compile = compile(switch_runtime_parse.program);
            if (!switch_runtime_compile.error && switch_runtime_compile.function) {
                tests_passed++;
                printf("  PASS: switch subject coverage typecheck+compile across int/string/bool/enum\n");
            } else {
                tests_failed++;
                printf("  FAIL: switch subject coverage typecheck+compile across int/string/bool/enum\n");
            }

            for (int i = 0; i < switch_runtime_compile.function_count; i++) {
                if (switch_runtime_compile.functions && switch_runtime_compile.functions[i]) {
                    obj_function_free(switch_runtime_compile.functions[i]);
                }
            }
            if (switch_runtime_compile.functions) free(switch_runtime_compile.functions);
            if (switch_runtime_compile.function) obj_function_free(switch_runtime_compile.function);
            symbol_table_free(switch_runtime_compile.globals);
            error_free(switch_runtime_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: switch subject coverage typecheck+compile across int/string/bool/enum\n");
        }
        symbol_table_free(switch_runtime_tc.globals);
        error_free(switch_runtime_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: switch subject coverage typecheck+compile across int/string/bool/enum\n");
    }
    parser_free_result(&switch_runtime_parse);

    const char* switch_type_source =
        "record Point { x: int };\n"
        "record Box[T] { value: T };\n"
        "type Count = int;\n"
        "func classify(value: any): int {\n"
        "    switch (value) {\n"
        "        case type Count, string: return 1;\n"
        "        case type Point: return 2;\n"
        "        case type Box[int]: return 3;\n"
        "        default: return 0;\n"
        "    }\n"
        "    return -1;\n"
        "}\n"
        "var p: Point = { x: 1 };\n"
        "var b: Box[int] = { value: 2 };\n"
        "var outInt: int = classify(7);\n"
        "var outString: int = classify(\"hi\");\n"
        "var outPoint: int = classify(p);\n"
        "var outBox: int = classify(b);\n"
        "var outOther: int = classify(false);\n";
    ParseResult switch_type_parse = parser_parse(switch_type_source, "test.tblo");
    if (!switch_type_parse.error &&
        switch_type_parse.program &&
        switch_type_parse.program->stmt_count >= 4 &&
        switch_type_parse.program->statements[2] &&
        switch_type_parse.program->statements[2]->kind == STMT_TYPE_ALIAS &&
        switch_type_parse.program->statements[3] &&
        switch_type_parse.program->statements[3]->kind == STMT_FUNC_DECL) {
        Stmt* switch_type_func = switch_type_parse.program->statements[3];
        Stmt* lowered_switch =
            switch_type_func->func_decl.body &&
            switch_type_func->func_decl.body->kind == STMT_BLOCK &&
            switch_type_func->func_decl.body->block.stmt_count > 0
                ? switch_type_func->func_decl.body->block.statements[0]
                : NULL;
        bool lowered_to_if_chain =
            lowered_switch &&
            lowered_switch->kind == STMT_BLOCK &&
            lowered_switch->block.stmt_count > 1 &&
            lowered_switch->block.statements[0] &&
            lowered_switch->block.statements[0]->kind == STMT_VAR_DECL &&
            lowered_switch->block.statements[1] &&
            lowered_switch->block.statements[1]->kind == STMT_IF;

        TypeCheckResult switch_type_tc = typecheck(switch_type_parse.program);
        if (!switch_type_tc.error && lowered_to_if_chain) {
            CompileResult switch_type_compile = compile(switch_type_parse.program);
            if (!switch_type_compile.error && switch_type_compile.function) {
                tests_passed++;
                printf("  PASS: switch type branches lower to if-chain parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: switch type branches lower to if-chain parse+typecheck+compile\n");
            }

            for (int i = 0; i < switch_type_compile.function_count; i++) {
                if (switch_type_compile.functions && switch_type_compile.functions[i]) {
                    obj_function_free(switch_type_compile.functions[i]);
                }
            }
            if (switch_type_compile.functions) free(switch_type_compile.functions);
            if (switch_type_compile.function) obj_function_free(switch_type_compile.function);
            symbol_table_free(switch_type_compile.globals);
            error_free(switch_type_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: switch type branches lower to if-chain parse+typecheck+compile\n");
        }
        symbol_table_free(switch_type_tc.globals);
        error_free(switch_type_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: switch type branches lower to if-chain parse+typecheck+compile\n");
    }
    parser_free_result(&switch_type_parse);

    const char* switch_type_interface_source =
        "record Point { x: int };\n"
        "record Box[T] { value: T };\n"
        "interface Named { name(): string; };\n"
        "interface Renderable { render(): string; };\n"
        "func name(point: Point): string { return \"point\"; }\n"
        "func renderBox[T](box: Box[T]): string { return \"box\"; }\n"
        "impl Renderable as Box[T] { render = renderBox; };\n"
        "func classify(value: any): int {\n"
        "    switch (value) {\n"
        "        case type Renderable: return 1;\n"
        "        case type Named: return 2;\n"
        "        default: return 0;\n"
        "    }\n"
        "    return -1;\n"
        "}\n"
        "var p: Point = { x: 1 };\n"
        "var b: Box[int] = { value: 2 };\n"
        "var outP: int = classify(p);\n"
        "var outB: int = classify(b);\n"
        "var outOther: int = classify(false);\n";
    ParseResult switch_type_interface_parse = parser_parse(switch_type_interface_source, "test.tblo");
    if (!switch_type_interface_parse.error &&
        switch_type_interface_parse.program &&
        switch_type_interface_parse.program->stmt_count > 0) {
        TypeCheckResult switch_type_interface_tc = typecheck(switch_type_interface_parse.program);
        if (!switch_type_interface_tc.error) {
            CompileResult switch_type_interface_compile = compile(switch_type_interface_parse.program);
            if (!switch_type_interface_compile.error && switch_type_interface_compile.function) {
                tests_passed++;
                printf("  PASS: switch type interface branches typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: switch type interface branches typecheck+compile\n");
            }

            for (int i = 0; i < switch_type_interface_compile.function_count; i++) {
                if (switch_type_interface_compile.functions && switch_type_interface_compile.functions[i]) {
                    obj_function_free(switch_type_interface_compile.functions[i]);
                }
            }
            if (switch_type_interface_compile.functions) free(switch_type_interface_compile.functions);
            if (switch_type_interface_compile.function) obj_function_free(switch_type_interface_compile.function);
            symbol_table_free(switch_type_interface_compile.globals);
            error_free(switch_type_interface_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: switch type interface branches typecheck+compile\n");
        }
        symbol_table_free(switch_type_interface_tc.globals);
        error_free(switch_type_interface_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: switch type interface branches parse\n");
    }
    parser_free_result(&switch_type_interface_parse);

    const char* switch_type_binding_source =
        "record Point { x: int };\n"
        "record Box[T] { value: T };\n"
        "interface Named { name(): string; };\n"
        "interface Renderable { render(): string; };\n"
        "func name(point: Point): string { return \"point\"; }\n"
        "func renderBox[T](box: Box[T]): string { return \"box\"; }\n"
        "impl Renderable as Box[T] { render = renderBox; };\n"
        "func classify(value: any): string {\n"
        "    switch (value) {\n"
        "        case type Point as point: return point.x as string;\n"
        "        case type Renderable as renderable: return renderable.render();\n"
        "        case type Named as named: return named.name();\n"
        "        default: return \"other\";\n"
        "    }\n"
        "    return \"bad\";\n"
        "}\n"
        "var p: Point = { x: 7 };\n"
        "var b: Box[int] = { value: 2 };\n"
        "var outP: string = classify(p);\n"
        "var outB: string = classify(b);\n"
        "var outOther: string = classify(false);\n";
    ParseResult switch_type_binding_parse = parser_parse(switch_type_binding_source, "test.tblo");
    if (!switch_type_binding_parse.error) {
        TypeCheckResult switch_type_binding_tc = typecheck(switch_type_binding_parse.program);
        if (!switch_type_binding_tc.error) {
            CompileResult switch_type_binding_compile = compile(switch_type_binding_parse.program);
            if (!switch_type_binding_compile.error && switch_type_binding_compile.function) {
                tests_passed++;
                printf("  PASS: switch type branch bindings typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: switch type branch bindings typecheck+compile\n");
            }

            for (int i = 0; i < switch_type_binding_compile.function_count; i++) {
                if (switch_type_binding_compile.functions && switch_type_binding_compile.functions[i]) {
                    obj_function_free(switch_type_binding_compile.functions[i]);
                }
            }
            if (switch_type_binding_compile.functions) free(switch_type_binding_compile.functions);
            if (switch_type_binding_compile.function) obj_function_free(switch_type_binding_compile.function);
            symbol_table_free(switch_type_binding_compile.globals);
            error_free(switch_type_binding_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: switch type branch bindings typecheck+compile\n");
        }
        symbol_table_free(switch_type_binding_tc.globals);
        error_free(switch_type_binding_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: switch type branch bindings parse\n");
    }
    parser_free_result(&switch_type_binding_parse);

    const char* switch_type_composite_source =
        "func classify(value: any): int {\n"
        "    switch (value) {\n"
        "        case type array<int>: return 1;\n"
        "        default: return 0;\n"
        "    }\n"
        "    return -1;\n"
        "}\n";
    ParseResult switch_type_composite_parse = parser_parse(switch_type_composite_source, "test.tblo");
    if (!switch_type_composite_parse.error) {
        TypeCheckResult switch_type_composite_tc = typecheck(switch_type_composite_parse.program);
        if (switch_type_composite_tc.error &&
            switch_type_composite_tc.error->message &&
            strstr(switch_type_composite_tc.error->message,
                   "switch type cases currently support only primitive, nil, record, and interface types") != NULL) {
            tests_passed++;
            printf("  PASS: switch type composite diagnostics\n");
        } else {
            tests_failed++;
            printf("  FAIL: switch type composite diagnostics\n");
        }
        symbol_table_free(switch_type_composite_tc.globals);
        error_free(switch_type_composite_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: switch type composite diagnostics parse\n");
    }
    parser_free_result(&switch_type_composite_parse);

    const char* switch_type_binding_multi_source =
        "func classify(value: any): int {\n"
        "    switch (value) {\n"
        "        case type int, string as scalar: return 1;\n"
        "        default: return 0;\n"
        "    }\n"
        "    return -1;\n"
        "}\n";
    ParseResult switch_type_binding_multi_parse = parser_parse(switch_type_binding_multi_source, "test.tblo");
    if (switch_type_binding_multi_parse.error &&
        switch_type_binding_multi_parse.error->message &&
        strstr(switch_type_binding_multi_parse.error->message,
               "switch type case binding requires exactly one target type") != NULL) {
        tests_passed++;
        printf("  PASS: switch type binding arity diagnostics\n");
    } else {
        tests_failed++;
        printf("  FAIL: switch type binding arity diagnostics\n");
    }
    parser_free_result(&switch_type_binding_multi_parse);

    const char* if_let_source =
        "enum Result[T, E] { Ok(T), Err(E) };\n"
        "func unwrap(result: Result[int, string]): int {\n"
        "    if let Result.Ok(okPayload) = result {\n"
        "        return okPayload;\n"
        "    } else {\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    ParseResult if_let_parse = parser_parse(if_let_source, "test.tblo");
    if (!if_let_parse.error &&
        if_let_parse.program &&
        if_let_parse.program->stmt_count >= 2 &&
        if_let_parse.program->statements[1] &&
        if_let_parse.program->statements[1]->kind == STMT_FUNC_DECL) {
        Stmt* if_let_func = if_let_parse.program->statements[1];
        Stmt* lowered_if_let =
            if_let_func->func_decl.body &&
            if_let_func->func_decl.body->kind == STMT_BLOCK &&
            if_let_func->func_decl.body->block.stmt_count > 0
                ? if_let_func->func_decl.body->block.statements[0]
                : NULL;
        bool lowered_to_match =
            lowered_if_let &&
            lowered_if_let->kind == STMT_MATCH &&
            lowered_if_let->match_stmt.arm_count == 1 &&
            lowered_if_let->match_stmt.else_branch != NULL;

        TypeCheckResult if_let_tc = typecheck(if_let_parse.program);
        if (!if_let_tc.error && lowered_to_match) {
            CompileResult if_let_compile = compile(if_let_parse.program);
            if (!if_let_compile.error && if_let_compile.function) {
                tests_passed++;
                printf("  PASS: if let lowers to match parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: if let lowers to match parse+typecheck+compile\n");
            }

            for (int i = 0; i < if_let_compile.function_count; i++) {
                if (if_let_compile.functions && if_let_compile.functions[i]) {
                    obj_function_free(if_let_compile.functions[i]);
                }
            }
            if (if_let_compile.functions) free(if_let_compile.functions);
            if (if_let_compile.function) obj_function_free(if_let_compile.function);
            symbol_table_free(if_let_compile.globals);
            error_free(if_let_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: if let lowers to match parse+typecheck+compile\n");
        }
        symbol_table_free(if_let_tc.globals);
        error_free(if_let_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: if let lowers to match parse+typecheck+compile\n");
    }
    parser_free_result(&if_let_parse);

    const char* while_let_source =
        "enum Result[T, E] { Ok(T), Err(E) };\n"
        "func consume(items: array<Result[int, string]>): int {\n"
        "    var i: int = 0;\n"
        "    while let Result.Ok(itemPayload) = items[i] {\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return i;\n"
        "}\n";
    ParseResult while_let_parse = parser_parse(while_let_source, "test.tblo");
    if (!while_let_parse.error &&
        while_let_parse.program &&
        while_let_parse.program->stmt_count >= 2 &&
        while_let_parse.program->statements[1] &&
        while_let_parse.program->statements[1]->kind == STMT_FUNC_DECL) {
        Stmt* while_let_func = while_let_parse.program->statements[1];
        Stmt* lowered_while_let =
            while_let_func->func_decl.body &&
            while_let_func->func_decl.body->kind == STMT_BLOCK &&
            while_let_func->func_decl.body->block.stmt_count > 1
                ? while_let_func->func_decl.body->block.statements[1]
                : NULL;
        bool lowered_to_loop_match =
            lowered_while_let &&
            lowered_while_let->kind == STMT_WHILE &&
            lowered_while_let->while_stmt.body &&
            lowered_while_let->while_stmt.body->kind == STMT_MATCH &&
            lowered_while_let->while_stmt.body->match_stmt.arm_count == 1 &&
            lowered_while_let->while_stmt.body->match_stmt.else_branch &&
            lowered_while_let->while_stmt.body->match_stmt.else_branch->kind == STMT_BREAK;

        TypeCheckResult while_let_tc = typecheck(while_let_parse.program);
        if (!while_let_tc.error && lowered_to_loop_match) {
            CompileResult while_let_compile = compile(while_let_parse.program);
            if (!while_let_compile.error && while_let_compile.function) {
                tests_passed++;
                printf("  PASS: while let lowers to loop+match parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: while let lowers to loop+match parse+typecheck+compile\n");
            }

            for (int i = 0; i < while_let_compile.function_count; i++) {
                if (while_let_compile.functions && while_let_compile.functions[i]) {
                    obj_function_free(while_let_compile.functions[i]);
                }
            }
            if (while_let_compile.functions) free(while_let_compile.functions);
            if (while_let_compile.function) obj_function_free(while_let_compile.function);
            symbol_table_free(while_let_compile.globals);
            error_free(while_let_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: while let lowers to loop+match parse+typecheck+compile\n");
        }
        symbol_table_free(while_let_tc.globals);
        error_free(while_let_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: while let lowers to loop+match parse+typecheck+compile\n");
    }
    parser_free_result(&while_let_parse);

    const char* if_let_missing_eq_source =
        "enum Result[T, E] { Ok(T), Err(E) };\n"
        "func main(): void {\n"
        "    var result: Result[int, string] = Result.Ok(1);\n"
        "    if let Result.Ok(okPayload) result {\n"
        "        return;\n"
        "    }\n"
        "}\n";
    ParseResult if_let_missing_eq_parse = parser_parse(if_let_missing_eq_source, "test.tblo");
    if (if_let_missing_eq_parse.error &&
        if_let_missing_eq_parse.error->message &&
        strstr(if_let_missing_eq_parse.error->message,
               "Expected '=' after if let pattern") != NULL) {
        tests_passed++;
        printf("  PASS: if let missing '=' diagnostics\n");
    } else {
        tests_failed++;
        printf("  FAIL: if let missing '=' diagnostics\n");
    }
    parser_free_result(&if_let_missing_eq_parse);

    const char* match_enum_exhaustive_source =
        "enum Status { Ok = 200, NotFound = 404, Retry = 429 };\n"
        "func classify(status: Status): int {\n"
        "    match (status) {\n"
        "        Status.Ok: return 1;\n"
        "        Status.NotFound: return 2;\n"
        "        Status.Retry: return 3;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    ParseResult match_enum_exhaustive_parse = parser_parse(match_enum_exhaustive_source, "test.tblo");
    if (!match_enum_exhaustive_parse.error) {
        TypeCheckResult match_enum_exhaustive_tc = typecheck(match_enum_exhaustive_parse.program);
        if (!match_enum_exhaustive_tc.error) {
            CompileResult match_enum_exhaustive_compile = compile(match_enum_exhaustive_parse.program);
            if (!match_enum_exhaustive_compile.error && match_enum_exhaustive_compile.function) {
                tests_passed++;
                printf("  PASS: exhaustive enum match parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: exhaustive enum match parse+typecheck+compile\n");
            }

            for (int i = 0; i < match_enum_exhaustive_compile.function_count; i++) {
                if (match_enum_exhaustive_compile.functions && match_enum_exhaustive_compile.functions[i]) {
                    obj_function_free(match_enum_exhaustive_compile.functions[i]);
                }
            }
            if (match_enum_exhaustive_compile.functions) free(match_enum_exhaustive_compile.functions);
            if (match_enum_exhaustive_compile.function) obj_function_free(match_enum_exhaustive_compile.function);
            symbol_table_free(match_enum_exhaustive_compile.globals);
            error_free(match_enum_exhaustive_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: exhaustive enum match parse+typecheck+compile\n");
        }
        symbol_table_free(match_enum_exhaustive_tc.globals);
        error_free(match_enum_exhaustive_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: exhaustive enum match parse\n");
    }
    parser_free_result(&match_enum_exhaustive_parse);

    const char* match_enum_non_exhaustive_source =
        "enum Status { Ok = 200, NotFound = 404, Retry = 429 };\n"
        "func classify(status: Status): int {\n"
        "    match (status) {\n"
        "        Status.Ok: return 1;\n"
        "        Status.NotFound: return 2;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    ParseResult match_enum_non_exhaustive_parse = parser_parse(match_enum_non_exhaustive_source, "test.tblo");
    if (!match_enum_non_exhaustive_parse.error) {
        TypeCheckResult match_enum_non_exhaustive_tc = typecheck(match_enum_non_exhaustive_parse.program);
        if (match_enum_non_exhaustive_tc.error &&
            match_enum_non_exhaustive_tc.error->message &&
            strstr(match_enum_non_exhaustive_tc.error->message, "Non-exhaustive enum match for 'Status'") != NULL) {
            tests_passed++;
            printf("  PASS: non-exhaustive enum match rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: non-exhaustive enum match rejection\n");
        }
        symbol_table_free(match_enum_non_exhaustive_tc.globals);
        error_free(match_enum_non_exhaustive_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: non-exhaustive enum match parse\n");
    }
    parser_free_result(&match_enum_non_exhaustive_parse);

    const char* match_enum_unreachable_else_source =
        "enum Status { Ok = 200, NotFound = 404, Retry = 429 };\n"
        "func classify(status: Status): int {\n"
        "    match (status) {\n"
        "        Status.Ok: return 1;\n"
        "        Status.NotFound: return 2;\n"
        "        Status.Retry: return 3;\n"
        "        else: return 0;\n"
        "    }\n"
        "}\n";
    ParseResult match_enum_unreachable_else_parse = parser_parse(match_enum_unreachable_else_source, "test.tblo");
    if (!match_enum_unreachable_else_parse.error) {
        TypeCheckOptions strict_opts = {0};
        strict_opts.strict_errors = true;
        TypeCheckResult match_enum_unreachable_else_tc = typecheck_with_options(match_enum_unreachable_else_parse.program, strict_opts);
        if (match_enum_unreachable_else_tc.error &&
            match_enum_unreachable_else_tc.error->message &&
            strstr(match_enum_unreachable_else_tc.error->message, "Unreachable else branch: enum match") != NULL) {
            tests_passed++;
            printf("  PASS: unreachable else warning for exhaustive enum match\n");
        } else {
            tests_failed++;
            printf("  FAIL: unreachable else warning for exhaustive enum match\n");
        }
        symbol_table_free(match_enum_unreachable_else_tc.globals);
        error_free(match_enum_unreachable_else_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: unreachable else enum match parse\n");
    }
    parser_free_result(&match_enum_unreachable_else_parse);

    const char* match_enum_unreachable_arm_source =
        "enum Status { Ok = 200, NotFound = 404, Retry = 429 };\n"
        "func classify(status: Status): int {\n"
        "    match (status) {\n"
        "        Status.Ok: return 1;\n"
        "        Status.NotFound: return 2;\n"
        "        Status.Retry: return 3;\n"
        "        Status.Ok: return 4;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    ParseResult match_enum_unreachable_arm_parse = parser_parse(match_enum_unreachable_arm_source, "test.tblo");
    if (!match_enum_unreachable_arm_parse.error) {
        TypeCheckOptions strict_opts = {0};
        strict_opts.strict_errors = true;
        TypeCheckResult match_enum_unreachable_arm_tc = typecheck_with_options(match_enum_unreachable_arm_parse.program, strict_opts);
        if (match_enum_unreachable_arm_tc.error &&
            match_enum_unreachable_arm_tc.error->message &&
            strstr(match_enum_unreachable_arm_tc.error->message, "Unreachable match arm: enum match") != NULL) {
            tests_passed++;
            printf("  PASS: unreachable arm warning for exhaustive enum match\n");
        } else {
            tests_failed++;
            printf("  FAIL: unreachable arm warning for exhaustive enum match\n");
        }
        symbol_table_free(match_enum_unreachable_arm_tc.globals);
        error_free(match_enum_unreachable_arm_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: unreachable arm enum match parse\n");
    }
    parser_free_result(&match_enum_unreachable_arm_parse);

    const char* match_enum_payload_non_exhaustive_source =
        "enum Result {\n"
        "    Ok(int),\n"
        "    Err\n"
        "};\n"
        "func classify(result: Result): int {\n"
        "    return match (result) {\n"
        "        Result.Ok(1): 1,\n"
        "        Result.Err: 0\n"
        "    };\n"
        "}\n";
    ParseResult match_enum_payload_non_exhaustive_parse =
        parser_parse(match_enum_payload_non_exhaustive_source, "test.tblo");
    if (!match_enum_payload_non_exhaustive_parse.error) {
        TypeCheckResult match_enum_payload_non_exhaustive_tc =
            typecheck(match_enum_payload_non_exhaustive_parse.program);
        if (match_enum_payload_non_exhaustive_tc.error &&
            match_enum_payload_non_exhaustive_tc.error->message &&
            strstr(match_enum_payload_non_exhaustive_tc.error->message,
                   "missing Result.Ok(_)") != NULL) {
            tests_passed++;
            printf("  PASS: payload-constrained enum arm remains non-exhaustive with witness\n");
        } else {
            tests_failed++;
            printf("  FAIL: payload-constrained enum arm remains non-exhaustive with witness\n");
        }
        symbol_table_free(match_enum_payload_non_exhaustive_tc.globals);
        error_free(match_enum_payload_non_exhaustive_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: payload-constrained enum match parse\n");
    }
    parser_free_result(&match_enum_payload_non_exhaustive_parse);

    const char* match_enum_payload_bool_witness_source =
        "enum Response {\n"
        "    Ok(bool),\n"
        "    Err\n"
        "};\n"
        "func classify(response: Response): int {\n"
        "    return match (response) {\n"
        "        Response.Ok(true): 1,\n"
        "        Response.Err: 0\n"
        "    };\n"
        "}\n";
    ParseResult match_enum_payload_bool_witness_parse =
        parser_parse(match_enum_payload_bool_witness_source, "test.tblo");
    if (!match_enum_payload_bool_witness_parse.error) {
        TypeCheckResult match_enum_payload_bool_witness_tc =
            typecheck(match_enum_payload_bool_witness_parse.program);
        if (match_enum_payload_bool_witness_tc.error &&
            match_enum_payload_bool_witness_tc.error->message &&
            strstr(match_enum_payload_bool_witness_tc.error->message,
                   "missing Response.Ok(false)") != NULL) {
            tests_passed++;
            printf("  PASS: payload enum bool witness diagnostic\n");
        } else {
            tests_failed++;
            printf("  FAIL: payload enum bool witness diagnostic\n");
        }
        symbol_table_free(match_enum_payload_bool_witness_tc.globals);
        error_free(match_enum_payload_bool_witness_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: payload enum bool witness parse\n");
    }
    parser_free_result(&match_enum_payload_bool_witness_parse);

    const char* match_enum_payload_partition_exhaustive_source =
        "enum Result[T, E] {\n"
        "    Ok(T),\n"
        "    Err(E)\n"
        "};\n"
        "func classify(result: Result[int, string]): int {\n"
        "    return match (result) {\n"
        "        Result.Ok(1): 101,\n"
        "        Result.Ok(value): value,\n"
        "        Result.Err(message): 0\n"
        "    };\n"
        "}\n";
    ParseResult match_enum_payload_partition_exhaustive_parse =
        parser_parse(match_enum_payload_partition_exhaustive_source, "test.tblo");
    if (!match_enum_payload_partition_exhaustive_parse.error) {
        TypeCheckResult match_enum_payload_partition_exhaustive_tc =
            typecheck(match_enum_payload_partition_exhaustive_parse.program);
        if (!match_enum_payload_partition_exhaustive_tc.error) {
            CompileResult match_enum_payload_partition_exhaustive_compile =
                compile(match_enum_payload_partition_exhaustive_parse.program);
            if (!match_enum_payload_partition_exhaustive_compile.error &&
                match_enum_payload_partition_exhaustive_compile.function) {
                tests_passed++;
                printf("  PASS: payload-partition enum match parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: payload-partition enum match parse+typecheck+compile\n");
            }

            for (int i = 0; i < match_enum_payload_partition_exhaustive_compile.function_count; i++) {
                if (match_enum_payload_partition_exhaustive_compile.functions &&
                    match_enum_payload_partition_exhaustive_compile.functions[i]) {
                    obj_function_free(match_enum_payload_partition_exhaustive_compile.functions[i]);
                }
            }
            if (match_enum_payload_partition_exhaustive_compile.functions) {
                free(match_enum_payload_partition_exhaustive_compile.functions);
            }
            if (match_enum_payload_partition_exhaustive_compile.function) {
                obj_function_free(match_enum_payload_partition_exhaustive_compile.function);
            }
            symbol_table_free(match_enum_payload_partition_exhaustive_compile.globals);
            error_free(match_enum_payload_partition_exhaustive_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: payload-partition enum match parse+typecheck+compile\n");
        }
        symbol_table_free(match_enum_payload_partition_exhaustive_tc.globals);
        error_free(match_enum_payload_partition_exhaustive_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: payload-partition enum match parse\n");
    }
    parser_free_result(&match_enum_payload_partition_exhaustive_parse);

    const char* match_enum_payload_partition_unreachable_else_source =
        "enum Result[T, E] {\n"
        "    Ok(T),\n"
        "    Err(E)\n"
        "};\n"
        "func classify(result: Result[int, string]): int {\n"
        "    return match (result) {\n"
        "        Result.Ok(1): 101,\n"
        "        Result.Ok(value): value,\n"
        "        Result.Err(message): 0,\n"
        "        else: -1\n"
        "    };\n"
        "}\n";
    ParseResult match_enum_payload_partition_unreachable_else_parse =
        parser_parse(match_enum_payload_partition_unreachable_else_source, "test.tblo");
    if (!match_enum_payload_partition_unreachable_else_parse.error) {
        TypeCheckOptions strict_opts = {0};
        strict_opts.strict_errors = true;
        TypeCheckResult match_enum_payload_partition_unreachable_else_tc =
            typecheck_with_options(match_enum_payload_partition_unreachable_else_parse.program,
                                   strict_opts);
        if (match_enum_payload_partition_unreachable_else_tc.error &&
            match_enum_payload_partition_unreachable_else_tc.error->message &&
            strstr(match_enum_payload_partition_unreachable_else_tc.error->message,
                   "Unreachable else branch: enum match already covers all members of 'Result[int,string]'") != NULL) {
            tests_passed++;
            printf("  PASS: unreachable else warning for payload-partition enum match\n");
        } else {
            tests_failed++;
            printf("  FAIL: unreachable else warning for payload-partition enum match\n");
        }
        symbol_table_free(match_enum_payload_partition_unreachable_else_tc.globals);
        error_free(match_enum_payload_partition_unreachable_else_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: unreachable else payload-partition enum match parse\n");
    }
    parser_free_result(&match_enum_payload_partition_unreachable_else_parse);

    const char* match_enum_payload_partition_duplicate_member_source =
        "enum Result[T, E] {\n"
        "    Ok(T),\n"
        "    Err(E)\n"
        "};\n"
        "func classify(result: Result[int, string]): int {\n"
        "    match (result) {\n"
        "        Result.Ok(1): return 101;\n"
        "        Result.Ok(value): return value;\n"
        "        Result.Ok(2): return 2;\n"
        "        Result.Err(message): return 0;\n"
        "    }\n"
        "    return -1;\n"
        "}\n";
    ParseResult match_enum_payload_partition_duplicate_member_parse =
        parser_parse(match_enum_payload_partition_duplicate_member_source, "test.tblo");
    if (!match_enum_payload_partition_duplicate_member_parse.error) {
        TypeCheckResult match_enum_payload_partition_duplicate_member_tc =
            typecheck(match_enum_payload_partition_duplicate_member_parse.program);
        if (match_enum_payload_partition_duplicate_member_tc.error &&
            match_enum_payload_partition_duplicate_member_tc.error->message &&
            strstr(match_enum_payload_partition_duplicate_member_tc.error->message,
                   "Duplicate match pattern 'Result[int,string].Ok'") != NULL) {
            tests_passed++;
            printf("  PASS: payload-partition enum member duplicate rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: payload-partition enum member duplicate rejection\n");
        }
        symbol_table_free(match_enum_payload_partition_duplicate_member_tc.globals);
        error_free(match_enum_payload_partition_duplicate_member_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: payload-partition enum member duplicate parse\n");
    }
    parser_free_result(&match_enum_payload_partition_duplicate_member_parse);

    const char* match_cross_enum_pattern_source =
        "enum A { One = 1 };\n"
        "enum B { One = 1 };\n"
        "func classify(value: A): int {\n"
        "    match (value) {\n"
        "        B.One: return 1;\n"
        "        else: return 0;\n"
        "    }\n"
        "}\n";
    ParseResult match_cross_enum_pattern_parse = parser_parse(match_cross_enum_pattern_source, "test.tblo");
    if (!match_cross_enum_pattern_parse.error) {
        TypeCheckResult match_cross_enum_pattern_tc = typecheck(match_cross_enum_pattern_parse.program);
        if (match_cross_enum_pattern_tc.error &&
            match_cross_enum_pattern_tc.error->message &&
            strstr(match_cross_enum_pattern_tc.error->message, "incompatible with subject enum 'A'") != NULL) {
            tests_passed++;
            printf("  PASS: cross-enum match pattern rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: cross-enum match pattern rejection\n");
        }
        symbol_table_free(match_cross_enum_pattern_tc.globals);
        error_free(match_cross_enum_pattern_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: cross-enum match pattern parse\n");
    }
    parser_free_result(&match_cross_enum_pattern_parse);

    const char* match_bool_source =
        "func classify(flag: bool): int {\n"
        "    var out: int = 0;\n"
        "    match (flag) {\n"
        "        true: out = 1;\n"
        "        false: out = 2;\n"
        "    }\n"
        "    return out;\n"
        "}\n";
    ParseResult match_bool_parse = parser_parse(match_bool_source, "test.tblo");
    if (!match_bool_parse.error) {
        TypeCheckResult match_bool_tc = typecheck(match_bool_parse.program);
        if (!match_bool_tc.error) {
            CompileResult match_bool_compile = compile(match_bool_parse.program);
            if (!match_bool_compile.error && match_bool_compile.function) {
                tests_passed++;
                printf("  PASS: match bool patterns parse+typecheck+compile\n");
            } else {
                tests_failed++;
                printf("  FAIL: match bool patterns parse+typecheck+compile\n");
            }

            for (int i = 0; i < match_bool_compile.function_count; i++) {
                if (match_bool_compile.functions && match_bool_compile.functions[i]) {
                    obj_function_free(match_bool_compile.functions[i]);
                }
            }
            if (match_bool_compile.functions) free(match_bool_compile.functions);
            if (match_bool_compile.function) obj_function_free(match_bool_compile.function);
            symbol_table_free(match_bool_compile.globals);
            error_free(match_bool_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: match bool patterns parse+typecheck+compile\n");
        }
        symbol_table_free(match_bool_tc.globals);
        error_free(match_bool_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: match bool patterns parse\n");
    }
    parser_free_result(&match_bool_parse);

    const char* match_bool_unreachable_else_source =
        "func classify(flag: bool): int {\n"
        "    match (flag) {\n"
        "        true: return 1;\n"
        "        false: return 2;\n"
        "        else: return 3;\n"
        "    }\n"
        "}\n";
    ParseResult match_bool_unreachable_else_parse = parser_parse(match_bool_unreachable_else_source, "test.tblo");
    if (!match_bool_unreachable_else_parse.error) {
        TypeCheckOptions strict_opts = {0};
        strict_opts.strict_errors = true;
        TypeCheckResult match_bool_unreachable_else_tc = typecheck_with_options(match_bool_unreachable_else_parse.program, strict_opts);
        if (match_bool_unreachable_else_tc.error &&
            match_bool_unreachable_else_tc.error->message &&
            strstr(match_bool_unreachable_else_tc.error->message, "Unreachable else branch: bool match") != NULL) {
            tests_passed++;
            printf("  PASS: unreachable else warning for exhaustive bool match\n");
        } else {
            tests_failed++;
            printf("  FAIL: unreachable else warning for exhaustive bool match\n");
        }
        symbol_table_free(match_bool_unreachable_else_tc.globals);
        error_free(match_bool_unreachable_else_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: unreachable else bool match parse\n");
    }
    parser_free_result(&match_bool_unreachable_else_parse);

    const char* match_bool_unreachable_arm_source =
        "func classify(flag: bool): int {\n"
        "    match (flag) {\n"
        "        true: return 1;\n"
        "        false: return 2;\n"
        "        true: return 3;\n"
        "    }\n"
        "}\n";
    ParseResult match_bool_unreachable_arm_parse = parser_parse(match_bool_unreachable_arm_source, "test.tblo");
    if (!match_bool_unreachable_arm_parse.error) {
        TypeCheckOptions strict_opts = {0};
        strict_opts.strict_errors = true;
        TypeCheckResult match_bool_unreachable_arm_tc = typecheck_with_options(match_bool_unreachable_arm_parse.program, strict_opts);
        if (match_bool_unreachable_arm_tc.error &&
            match_bool_unreachable_arm_tc.error->message &&
            strstr(match_bool_unreachable_arm_tc.error->message, "Unreachable match arm: bool match") != NULL) {
            tests_passed++;
            printf("  PASS: unreachable arm warning for exhaustive bool match\n");
        } else {
            tests_failed++;
            printf("  FAIL: unreachable arm warning for exhaustive bool match\n");
        }
        symbol_table_free(match_bool_unreachable_arm_tc.globals);
        error_free(match_bool_unreachable_arm_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: unreachable arm bool match parse\n");
    }
    parser_free_result(&match_bool_unreachable_arm_parse);

    const char* match_record_unreachable_else_source =
        "record Point {\n"
        "    x: int,\n"
        "    y: int\n"
        "};\n"
        "func head(point: Point): int {\n"
        "    match (point) {\n"
        "        Point { x, .. }: return x;\n"
        "        else: return 0;\n"
        "    }\n"
        "}\n";
    ParseResult match_record_unreachable_else_parse =
        parser_parse(match_record_unreachable_else_source, "test.tblo");
    if (!match_record_unreachable_else_parse.error) {
        TypeCheckOptions strict_opts = {0};
        strict_opts.strict_errors = true;
        TypeCheckResult match_record_unreachable_else_tc =
            typecheck_with_options(match_record_unreachable_else_parse.program, strict_opts);
        if (match_record_unreachable_else_tc.error &&
            match_record_unreachable_else_tc.error->message &&
            strstr(match_record_unreachable_else_tc.error->message,
                   "Unreachable else branch: previous match patterns already cover all values") != NULL) {
            tests_passed++;
            printf("  PASS: unreachable else warning for covering record pattern\n");
        } else {
            tests_failed++;
            printf("  FAIL: unreachable else warning for covering record pattern\n");
        }
        symbol_table_free(match_record_unreachable_else_tc.globals);
        error_free(match_record_unreachable_else_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: unreachable else covering record pattern parse\n");
    }
    parser_free_result(&match_record_unreachable_else_parse);

    const char* match_record_partition_unreachable_else_source =
        "record Point {\n"
        "    x: int,\n"
        "    y: int\n"
        "};\n"
        "func head(point: Point): int {\n"
        "    match (point) {\n"
        "        Point { x: 1, .. }: return 100;\n"
        "        Point { x, .. }: return x;\n"
        "        else: return 0;\n"
        "    }\n"
        "}\n";
    ParseResult match_record_partition_unreachable_else_parse =
        parser_parse(match_record_partition_unreachable_else_source, "test.tblo");
    if (!match_record_partition_unreachable_else_parse.error) {
        TypeCheckOptions strict_opts = {0};
        strict_opts.strict_errors = true;
        TypeCheckResult match_record_partition_unreachable_else_tc =
            typecheck_with_options(match_record_partition_unreachable_else_parse.program, strict_opts);
        if (match_record_partition_unreachable_else_tc.error &&
            match_record_partition_unreachable_else_tc.error->message &&
            strstr(match_record_partition_unreachable_else_tc.error->message,
                   "Unreachable else branch: previous match patterns already cover all values") != NULL) {
            tests_passed++;
            printf("  PASS: unreachable else warning for partial record partition\n");
        } else {
            tests_failed++;
            printf("  FAIL: unreachable else warning for partial record partition\n");
        }
        symbol_table_free(match_record_partition_unreachable_else_tc.globals);
        error_free(match_record_partition_unreachable_else_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: unreachable else partial record partition parse\n");
    }
    parser_free_result(&match_record_partition_unreachable_else_parse);

    const char* match_tuple_unreachable_arm_source =
        "func classify(pair: (int, int)): int {\n"
        "    match (pair) {\n"
        "        (left, right): return left + right;\n"
        "        (1, 2): return 0;\n"
        "    }\n"
        "}\n";
    ParseResult match_tuple_unreachable_arm_parse =
        parser_parse(match_tuple_unreachable_arm_source, "test.tblo");
    if (!match_tuple_unreachable_arm_parse.error) {
        TypeCheckOptions strict_opts = {0};
        strict_opts.strict_errors = true;
        TypeCheckResult match_tuple_unreachable_arm_tc =
            typecheck_with_options(match_tuple_unreachable_arm_parse.program, strict_opts);
        if (match_tuple_unreachable_arm_tc.error &&
            match_tuple_unreachable_arm_tc.error->message &&
            strstr(match_tuple_unreachable_arm_tc.error->message,
                   "Unreachable match arm: previous patterns already cover all values") != NULL) {
            tests_passed++;
            printf("  PASS: unreachable arm warning for covering tuple pattern\n");
        } else {
            tests_failed++;
            printf("  FAIL: unreachable arm warning for covering tuple pattern\n");
        }
        symbol_table_free(match_tuple_unreachable_arm_tc.globals);
        error_free(match_tuple_unreachable_arm_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: unreachable arm covering tuple pattern parse\n");
    }
    parser_free_result(&match_tuple_unreachable_arm_parse);

    const char* match_tuple_partition_unreachable_arm_source =
        "func classify(pair: (bool, int)): int {\n"
        "    match (pair) {\n"
        "        (true, value): return value;\n"
        "        (false, value): return value + 10;\n"
        "        (flag, other): return 0;\n"
        "    }\n"
        "}\n";
    ParseResult match_tuple_partition_unreachable_arm_parse =
        parser_parse(match_tuple_partition_unreachable_arm_source, "test.tblo");
    if (!match_tuple_partition_unreachable_arm_parse.error) {
        TypeCheckOptions strict_opts = {0};
        strict_opts.strict_errors = true;
        TypeCheckResult match_tuple_partition_unreachable_arm_tc =
            typecheck_with_options(match_tuple_partition_unreachable_arm_parse.program, strict_opts);
        if (match_tuple_partition_unreachable_arm_tc.error &&
            match_tuple_partition_unreachable_arm_tc.error->message &&
            strstr(match_tuple_partition_unreachable_arm_tc.error->message,
                   "Unreachable match arm: previous patterns already cover all values") != NULL) {
            tests_passed++;
            printf("  PASS: unreachable arm warning for tuple bool partition\n");
        } else {
            tests_failed++;
            printf("  FAIL: unreachable arm warning for tuple bool partition\n");
        }
        symbol_table_free(match_tuple_partition_unreachable_arm_tc.globals);
        error_free(match_tuple_partition_unreachable_arm_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: unreachable arm tuple bool partition parse\n");
    }
    parser_free_result(&match_tuple_partition_unreachable_arm_parse);

    const char* match_record_matrix_unreachable_else_source =
        "record Flagged {\n"
        "    flag: bool,\n"
        "    code: int\n"
        "};\n"
        "func classify(value: Flagged): int {\n"
        "    match (value) {\n"
        "        Flagged { flag: true, code: 1 }: return 11;\n"
        "        Flagged { flag: true, code }: return code;\n"
        "        Flagged { flag: false, code: 1 }: return 21;\n"
        "        Flagged { flag: false, code }: return code + 10;\n"
        "        else: return 0;\n"
        "    }\n"
        "}\n";
    ParseResult match_record_matrix_unreachable_else_parse =
        parser_parse(match_record_matrix_unreachable_else_source, "test.tblo");
    if (!match_record_matrix_unreachable_else_parse.error) {
        TypeCheckOptions strict_opts = {0};
        strict_opts.strict_errors = true;
        TypeCheckResult match_record_matrix_unreachable_else_tc =
            typecheck_with_options(match_record_matrix_unreachable_else_parse.program, strict_opts);
        if (match_record_matrix_unreachable_else_tc.error &&
            match_record_matrix_unreachable_else_tc.error->message &&
            strstr(match_record_matrix_unreachable_else_tc.error->message,
                   "Unreachable else branch: previous match patterns already cover all values") != NULL) {
            tests_passed++;
            printf("  PASS: unreachable else warning for recursive record partition\n");
        } else {
            tests_failed++;
            printf("  FAIL: unreachable else warning for recursive record partition\n");
        }
        symbol_table_free(match_record_matrix_unreachable_else_tc.globals);
        error_free(match_record_matrix_unreachable_else_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: unreachable else recursive record partition parse\n");
    }
    parser_free_result(&match_record_matrix_unreachable_else_parse);

    const char* match_tuple_matrix_unreachable_arm_source =
        "func classify(pair: (bool, int)): int {\n"
        "    match (pair) {\n"
        "        (true, 1): return 11;\n"
        "        (true, value): return value;\n"
        "        (false, 1): return 21;\n"
        "        (false, value): return value + 10;\n"
        "        (flag, other): return 0;\n"
        "    }\n"
        "}\n";
    ParseResult match_tuple_matrix_unreachable_arm_parse =
        parser_parse(match_tuple_matrix_unreachable_arm_source, "test.tblo");
    if (!match_tuple_matrix_unreachable_arm_parse.error) {
        TypeCheckOptions strict_opts = {0};
        strict_opts.strict_errors = true;
        TypeCheckResult match_tuple_matrix_unreachable_arm_tc =
            typecheck_with_options(match_tuple_matrix_unreachable_arm_parse.program, strict_opts);
        if (match_tuple_matrix_unreachable_arm_tc.error &&
            match_tuple_matrix_unreachable_arm_tc.error->message &&
            strstr(match_tuple_matrix_unreachable_arm_tc.error->message,
                   "Unreachable match arm: previous patterns already cover all values") != NULL) {
            tests_passed++;
            printf("  PASS: unreachable arm warning for recursive tuple partition\n");
        } else {
            tests_failed++;
            printf("  FAIL: unreachable arm warning for recursive tuple partition\n");
        }
        symbol_table_free(match_tuple_matrix_unreachable_arm_tc.globals);
        error_free(match_tuple_matrix_unreachable_arm_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: unreachable arm recursive tuple partition parse\n");
    }
    parser_free_result(&match_tuple_matrix_unreachable_arm_parse);

    const char* match_guard_non_exhaustive_source =
        "func classify(flag: bool): int {\n"
        "    var out: int = 0;\n"
        "    match (flag) {\n"
        "        true if true: out = 1;\n"
        "        false: out = 2;\n"
        "    }\n"
        "    return out;\n"
        "}\n";
    ParseResult match_guard_non_exhaustive_parse = parser_parse(match_guard_non_exhaustive_source, "test.tblo");
    if (!match_guard_non_exhaustive_parse.error) {
        TypeCheckResult match_guard_non_exhaustive_tc = typecheck(match_guard_non_exhaustive_parse.program);
        if (match_guard_non_exhaustive_tc.error &&
            match_guard_non_exhaustive_tc.error->message &&
            strstr(match_guard_non_exhaustive_tc.error->message,
                   "Non-exhaustive match: missing true when a guard is false") != NULL) {
            tests_passed++;
            printf("  PASS: guarded bool match witness diagnostic\n");
        } else {
            tests_failed++;
            printf("  FAIL: guarded bool match witness diagnostic\n");
        }
        symbol_table_free(match_guard_non_exhaustive_tc.globals);
        error_free(match_guard_non_exhaustive_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: guarded bool match non-exhaustive parse\n");
    }
    parser_free_result(&match_guard_non_exhaustive_parse);

    const char* match_guard_else_reachable_source =
        "func classify(flag: bool): int {\n"
        "    match (flag) {\n"
        "        true if true: return 1;\n"
        "        false if true: return 2;\n"
        "        else: return 3;\n"
        "    }\n"
        "}\n";
    ParseResult match_guard_else_reachable_parse = parser_parse(match_guard_else_reachable_source, "test.tblo");
    if (!match_guard_else_reachable_parse.error) {
        TypeCheckOptions strict_opts = {0};
        strict_opts.strict_errors = true;
        TypeCheckResult match_guard_else_reachable_tc =
            typecheck_with_options(match_guard_else_reachable_parse.program, strict_opts);
        if (!match_guard_else_reachable_tc.error) {
            tests_passed++;
            printf("  PASS: guarded match else remains reachable\n");
        } else {
            tests_failed++;
            printf("  FAIL: guarded match else remains reachable\n");
        }
        symbol_table_free(match_guard_else_reachable_tc.globals);
        error_free(match_guard_else_reachable_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: guarded match else reachability parse\n");
    }
    parser_free_result(&match_guard_else_reachable_parse);

    const char* match_bool_non_exhaustive_source =
        "func classify(flag: bool): int {\n"
        "    var out: int = 0;\n"
        "    match (flag) {\n"
        "        true: out = 1;\n"
        "    }\n"
        "    return out;\n"
        "}\n";
    ParseResult match_bool_non_exhaustive_parse = parser_parse(match_bool_non_exhaustive_source, "test.tblo");
    if (!match_bool_non_exhaustive_parse.error) {
        TypeCheckResult match_bool_non_exhaustive_tc = typecheck(match_bool_non_exhaustive_parse.program);
        if (match_bool_non_exhaustive_tc.error &&
            match_bool_non_exhaustive_tc.error->message &&
            strstr(match_bool_non_exhaustive_tc.error->message, "Non-exhaustive bool match") != NULL) {
            tests_passed++;
            printf("  PASS: non-exhaustive bool match rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: non-exhaustive bool match rejection\n");
        }
        symbol_table_free(match_bool_non_exhaustive_tc.globals);
        error_free(match_bool_non_exhaustive_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: non-exhaustive bool match parse\n");
    }
    parser_free_result(&match_bool_non_exhaustive_parse);

    const char* match_bool_duplicate_source =
        "func classify(flag: bool): int {\n"
        "    var out: int = 0;\n"
        "    match (flag) {\n"
        "        true: out = 1;\n"
        "        true: out = 2;\n"
        "        false: out = 3;\n"
        "    }\n"
        "    return out;\n"
        "}\n";
    ParseResult match_bool_duplicate_parse = parser_parse(match_bool_duplicate_source, "test.tblo");
    if (!match_bool_duplicate_parse.error) {
        TypeCheckResult match_bool_duplicate_tc = typecheck(match_bool_duplicate_parse.program);
        if (match_bool_duplicate_tc.error &&
            match_bool_duplicate_tc.error->message &&
            strstr(match_bool_duplicate_tc.error->message, "Duplicate 'true' match pattern") != NULL) {
            tests_passed++;
            printf("  PASS: duplicate bool match pattern rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: duplicate bool match pattern rejection\n");
        }
        symbol_table_free(match_bool_duplicate_tc.globals);
        error_free(match_bool_duplicate_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: duplicate bool match parse\n");
    }
    parser_free_result(&match_bool_duplicate_parse);

    const char* match_duplicate_int_source =
        "func classify(code: int): int {\n"
        "    var out: int = 0;\n"
        "    match (code) {\n"
        "        200: out = 1;\n"
        "        200: out = 2;\n"
        "        else: out = 3;\n"
        "    }\n"
        "    return out;\n"
        "}\n";
    ParseResult match_duplicate_int_parse = parser_parse(match_duplicate_int_source, "test.tblo");
    if (!match_duplicate_int_parse.error) {
        TypeCheckResult match_duplicate_int_tc = typecheck(match_duplicate_int_parse.program);
        if (match_duplicate_int_tc.error &&
            match_duplicate_int_tc.error->message &&
            strstr(match_duplicate_int_tc.error->message, "Duplicate match pattern '200'") != NULL) {
            tests_passed++;
            printf("  PASS: duplicate int match pattern rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: duplicate int match pattern rejection\n");
        }
        symbol_table_free(match_duplicate_int_tc.globals);
        error_free(match_duplicate_int_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: duplicate int match parse\n");
    }
    parser_free_result(&match_duplicate_int_parse);

    const char* match_duplicate_enum_member_value_source =
        "enum Status { Ok = 200, NotFound = 404 };\n"
        "func classify(code: int): int {\n"
        "    match (code) {\n"
        "        Status.Ok: return 1;\n"
        "        200: return 2;\n"
        "        else: return 0;\n"
        "    }\n"
        "}\n";
    ParseResult match_duplicate_enum_member_value_parse = parser_parse(match_duplicate_enum_member_value_source, "test.tblo");
    if (!match_duplicate_enum_member_value_parse.error) {
        TypeCheckResult match_duplicate_enum_member_value_tc = typecheck(match_duplicate_enum_member_value_parse.program);
        if (match_duplicate_enum_member_value_tc.error &&
            match_duplicate_enum_member_value_tc.error->message &&
            strstr(match_duplicate_enum_member_value_tc.error->message, "Duplicate match pattern '200'") != NULL) {
            tests_passed++;
            printf("  PASS: duplicate enum-member/int match pattern rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: duplicate enum-member/int match pattern rejection\n");
        }
        symbol_table_free(match_duplicate_enum_member_value_tc.globals);
        error_free(match_duplicate_enum_member_value_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: duplicate enum-member/int match parse\n");
    }
    parser_free_result(&match_duplicate_enum_member_value_parse);

    const char* match_incompatible_pattern_source =
        "func main(): void {\n"
        "    var name: string = \"vml\";\n"
        "    match (name) {\n"
        "        1: println(\"bad\");\n"
        "        else: println(\"ok\");\n"
        "    }\n"
        "}\n";
    ParseResult match_incompatible_pattern_parse = parser_parse(match_incompatible_pattern_source, "test.tblo");
    if (!match_incompatible_pattern_parse.error) {
        TypeCheckResult match_incompatible_pattern_tc = typecheck(match_incompatible_pattern_parse.program);
        if (match_incompatible_pattern_tc.error &&
            match_incompatible_pattern_tc.error->message &&
            strstr(match_incompatible_pattern_tc.error->message, "incompatible with subject type") != NULL) {
            tests_passed++;
            printf("  PASS: match incompatible pattern rejection\n");
        } else {
            tests_failed++;
            printf("  FAIL: match incompatible pattern rejection\n");
        }
        symbol_table_free(match_incompatible_pattern_tc.globals);
        error_free(match_incompatible_pattern_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: match incompatible pattern parse\n");
    }
    parser_free_result(&match_incompatible_pattern_parse);

    parser_free_result(&result);

    const char* invalid_source = "func main(): void { var x: int = ; }";
    ParseResult invalid = parser_parse(invalid_source, "test.tblo");
    if (invalid.error &&
        invalid.error->message &&
        strstr(invalid.error->message, "Expected expression") != NULL &&
        strstr(invalid.error->message, "got") != NULL) {
        tests_passed++;
        printf("  PASS: parser detailed expected/got diagnostics\n");
    } else {
        tests_failed++;
        printf("  FAIL: parser detailed expected/got diagnostics\n");
    }
    parser_free_result(&invalid);

    const char* switch_default_order_source =
        "func main(): void {\n"
        "    switch (1) {\n"
        "        default: println(\"other\");\n"
        "        case 2: println(\"two\");\n"
        "    }\n"
        "}\n";
    ParseResult switch_default_order = parser_parse(switch_default_order_source, "test.tblo");
    if (switch_default_order.error &&
        switch_default_order.error->message &&
        strstr(switch_default_order.error->message,
               "default branch must be the last branch in switch statement") != NULL) {
        tests_passed++;
        printf("  PASS: switch default branch ordering diagnostics\n");
    } else {
        tests_failed++;
        printf("  FAIL: switch default branch ordering diagnostics\n");
    }
    parser_free_result(&switch_default_order);

    const char* switch_mixed_case_kinds_source =
        "func main(): void {\n"
        "    switch (1) {\n"
        "        case 1: println(\"one\");\n"
        "        case type int: println(\"int\");\n"
        "    }\n"
        "}\n";
    ParseResult switch_mixed_case_kinds = parser_parse(switch_mixed_case_kinds_source, "test.tblo");
    if (switch_mixed_case_kinds.error &&
        switch_mixed_case_kinds.error->message &&
        strstr(switch_mixed_case_kinds.error->message,
               "switch cannot mix value cases with 'case type' branches") != NULL) {
        tests_passed++;
        printf("  PASS: switch mixed value/type diagnostics\n");
    } else {
        tests_failed++;
        printf("  FAIL: switch mixed value/type diagnostics\n");
    }
    parser_free_result(&switch_mixed_case_kinds);
}

static void test_constant_folding(void) {
    printf("Testing constant folding...\n");

    const char* source =
        "const A: int = 1 + 2 * 3 - (8 / 2);\n"
        "const B: bool = ((1 + 2) * (3 - 1) == 6) && ((9 / 3) < 4);\n"
        "const S: string = \"vm\" + \"-\" + \"lang\";\n"
        "println(1 + 2 * 3);\n"
        "println(A);\n"
        "println(B);\n"
        "println(S);\n";

    ParseResult parse_result = parser_parse(source, "test.tblo");
    if (parse_result.error || !parse_result.program) {
        tests_failed++;
        printf("  FAIL: constant folding parse\n");
        parser_free_result(&parse_result);
        return;
    }

    TypeCheckResult tc_result = typecheck(parse_result.program);
    if (tc_result.error) {
        tests_failed++;
        printf("  FAIL: constant folding typecheck\n");
        symbol_table_free(tc_result.globals);
        error_free(tc_result.error);
        parser_free_result(&parse_result);
        return;
    }

    CompileResult compile_result = compile(parse_result.program);
    if (compile_result.error || !compile_result.function) {
        tests_failed++;
        printf("  FAIL: constant folding compile\n");
    } else {
        const Chunk* chunk = &compile_result.function->chunk;
        bool has_foldable_ops =
            chunk_contains_opcode(chunk, OP_ADD) ||
            chunk_contains_opcode(chunk, OP_ADD_INT) ||
            chunk_contains_opcode(chunk, OP_ADD_STACK_CONST_INT) ||
            chunk_contains_opcode(chunk, OP_MUL) ||
            chunk_contains_opcode(chunk, OP_MUL_INT) ||
            chunk_contains_opcode(chunk, OP_MUL_STACK_CONST_INT) ||
            chunk_contains_opcode(chunk, OP_SUB) ||
            chunk_contains_opcode(chunk, OP_SUB_INT) ||
            chunk_contains_opcode(chunk, OP_SUB_STACK_CONST_INT) ||
            chunk_contains_opcode(chunk, OP_DIV) ||
            chunk_contains_opcode(chunk, OP_DIV_INT) ||
            chunk_contains_opcode(chunk, OP_DIV_STACK_CONST_INT) ||
            chunk_contains_opcode(chunk, OP_LT) ||
            chunk_contains_opcode(chunk, OP_GT) ||
            chunk_contains_opcode(chunk, OP_EQ) ||
            chunk_contains_opcode(chunk, OP_AND);

        if (!has_foldable_ops) {
            tests_passed++;
            printf("  PASS: folded literal arithmetic/bool/string expressions\n");
        } else {
            tests_failed++;
            printf("  FAIL: folded literal arithmetic/bool/string expressions\n");
        }
    }

    for (int i = 0; i < compile_result.function_count; i++) {
        if (compile_result.functions && compile_result.functions[i]) {
            obj_function_free(compile_result.functions[i]);
        }
    }
    if (compile_result.functions) free(compile_result.functions);
    if (compile_result.function) obj_function_free(compile_result.function);
    symbol_table_free(compile_result.globals);
    error_free(compile_result.error);

    symbol_table_free(tc_result.globals);
    error_free(tc_result.error);
    parser_free_result(&parse_result);

    const char* identity_source =
        "func addIdentity(x: int): int {\n"
        "    return x + 0;\n"
        "}\n"
        "func mulIdentity(x: int): int {\n"
        "    return 1 * x;\n"
        "}\n"
        "func andIdentity(flag: bool): bool {\n"
        "    return flag && true;\n"
        "}\n"
        "func orIdentity(flag: bool): bool {\n"
        "    return flag || false;\n"
        "}\n"
        "func controlAdd(x: int): int {\n"
        "    return x + 7;\n"
        "}\n"
        "func controlAnd(a: bool, b: bool): bool {\n"
        "    return a && b;\n"
        "}\n";

    ParseResult identity_parse = parser_parse(identity_source, "test.tblo");
    if (identity_parse.error || !identity_parse.program) {
        tests_failed++;
        printf("  FAIL: identity-folding parse\n");
        parser_free_result(&identity_parse);
        return;
    }

    TypeCheckResult identity_tc = typecheck(identity_parse.program);
    if (identity_tc.error) {
        tests_failed++;
        printf("  FAIL: identity-folding typecheck\n");
        symbol_table_free(identity_tc.globals);
        error_free(identity_tc.error);
        parser_free_result(&identity_parse);
        return;
    }

    CompileResult identity_compile = compile(identity_parse.program);
    if (identity_compile.error || !identity_compile.function) {
        tests_failed++;
        printf("  FAIL: identity-folding compile\n");
    } else {
        const Chunk* add_identity_chunk = find_compiled_function_chunk(&identity_compile, "addIdentity");
        const Chunk* mul_identity_chunk = find_compiled_function_chunk(&identity_compile, "mulIdentity");
        const Chunk* and_identity_chunk = find_compiled_function_chunk(&identity_compile, "andIdentity");
        const Chunk* or_identity_chunk = find_compiled_function_chunk(&identity_compile, "orIdentity");
        const Chunk* control_add_chunk = find_compiled_function_chunk(&identity_compile, "controlAdd");
        const Chunk* control_and_chunk = find_compiled_function_chunk(&identity_compile, "controlAnd");

        bool folded_identities =
            add_identity_chunk && mul_identity_chunk &&
            and_identity_chunk && or_identity_chunk &&
            control_add_chunk && control_and_chunk &&
            (add_identity_chunk->code_count + 2 <= control_add_chunk->code_count) &&
            (mul_identity_chunk->code_count + 2 <= control_add_chunk->code_count) &&
            (and_identity_chunk->code_count + 3 <= control_and_chunk->code_count) &&
            (or_identity_chunk->code_count + 3 <= control_and_chunk->code_count);

        if (folded_identities) {
            tests_passed++;
            printf("  PASS: folds algebraic and boolean identity expressions\n");
        } else {
            tests_failed++;
            printf("  FAIL: folds algebraic and boolean identity expressions\n");
        }
    }

    for (int i = 0; i < identity_compile.function_count; i++) {
        if (identity_compile.functions && identity_compile.functions[i]) {
            obj_function_free(identity_compile.functions[i]);
        }
    }
    if (identity_compile.functions) free(identity_compile.functions);
    if (identity_compile.function) obj_function_free(identity_compile.function);
    symbol_table_free(identity_compile.globals);
    error_free(identity_compile.error);

    symbol_table_free(identity_tc.globals);
    error_free(identity_tc.error);
    parser_free_result(&identity_parse);

    const char* cast_identity_source =
        "func castIntNoop(x: int): int {\n"
        "    return x as int;\n"
        "}\n"
        "func castBoolNoop(flag: bool): bool {\n"
        "    return flag as bool;\n"
        "}\n"
        "func castDoubleNoop(v: double): double {\n"
        "    return v as double;\n"
        "}\n"
        "func castStringNoop(s: string): string {\n"
        "    return s as string;\n"
        "}\n";

    ParseResult cast_identity_parse = parser_parse(cast_identity_source, "test.tblo");
    if (cast_identity_parse.error || !cast_identity_parse.program) {
        tests_failed++;
        printf("  FAIL: cast-noop folding parse\n");
        parser_free_result(&cast_identity_parse);
        return;
    }

    TypeCheckResult cast_identity_tc = typecheck(cast_identity_parse.program);
    if (cast_identity_tc.error) {
        tests_failed++;
        printf("  FAIL: cast-noop folding typecheck\n");
        symbol_table_free(cast_identity_tc.globals);
        error_free(cast_identity_tc.error);
        parser_free_result(&cast_identity_parse);
        return;
    }

    CompileResult cast_identity_compile = compile(cast_identity_parse.program);
    if (cast_identity_compile.error || !cast_identity_compile.function) {
        tests_failed++;
        printf("  FAIL: cast-noop folding compile\n");
    } else {
        const Chunk* cast_int_noop_chunk =
            find_compiled_function_chunk(&cast_identity_compile, "castIntNoop");
        const Chunk* cast_bool_noop_chunk =
            find_compiled_function_chunk(&cast_identity_compile, "castBoolNoop");
        const Chunk* cast_double_noop_chunk =
            find_compiled_function_chunk(&cast_identity_compile, "castDoubleNoop");
        const Chunk* cast_string_noop_chunk =
            find_compiled_function_chunk(&cast_identity_compile, "castStringNoop");

        bool removed_noop_casts =
            cast_int_noop_chunk && cast_bool_noop_chunk &&
            cast_double_noop_chunk && cast_string_noop_chunk &&
            !chunk_contains_opcode(cast_int_noop_chunk, OP_CAST_INT) &&
            !chunk_contains_opcode(cast_bool_noop_chunk, OP_CAST_BOOL) &&
            !chunk_contains_opcode(cast_double_noop_chunk, OP_CAST_DOUBLE) &&
            !chunk_contains_opcode(cast_string_noop_chunk, OP_CAST_STRING);

        if (removed_noop_casts) {
            tests_passed++;
            printf("  PASS: removes no-op casts during expression folding\n");
        } else {
            tests_failed++;
            printf("  FAIL: removes no-op casts during expression folding\n");
        }
    }

    for (int i = 0; i < cast_identity_compile.function_count; i++) {
        if (cast_identity_compile.functions && cast_identity_compile.functions[i]) {
            obj_function_free(cast_identity_compile.functions[i]);
        }
    }
    if (cast_identity_compile.functions) free(cast_identity_compile.functions);
    if (cast_identity_compile.function) obj_function_free(cast_identity_compile.function);
    symbol_table_free(cast_identity_compile.globals);
    error_free(cast_identity_compile.error);

    symbol_table_free(cast_identity_tc.globals);
    error_free(cast_identity_tc.error);
    parser_free_result(&cast_identity_parse);

    const char* local_cse_source =
        "func duplicateExpr(a: int, b: int): int {\n"
        "    var first: int = a + b;\n"
        "    var second: int = a + b;\n"
        "    return second;\n"
        "}\n"
        "func manualReuse(a: int, b: int): int {\n"
        "    var first: int = a + b;\n"
        "    var second: int = first;\n"
        "    return second;\n"
        "}\n"
        "func barrierNoReuse(a: int, b: int): int {\n"
        "    var first: int = a + b;\n"
        "    println(\"barrier\");\n"
        "    var second: int = a + b;\n"
        "    return second;\n"
        "}\n";

    ParseResult local_cse_parse = parser_parse(local_cse_source, "test.tblo");
    if (local_cse_parse.error || !local_cse_parse.program) {
        tests_failed++;
        printf("  FAIL: lightweight IR local-CSE parse\n");
        parser_free_result(&local_cse_parse);
        return;
    }

    TypeCheckResult local_cse_tc = typecheck(local_cse_parse.program);
    if (local_cse_tc.error) {
        tests_failed++;
        printf("  FAIL: lightweight IR local-CSE typecheck\n");
        symbol_table_free(local_cse_tc.globals);
        error_free(local_cse_tc.error);
        parser_free_result(&local_cse_parse);
        return;
    }

    CompileResult local_cse_compile = compile(local_cse_parse.program);
    if (local_cse_compile.error || !local_cse_compile.function) {
        tests_failed++;
        printf("  FAIL: lightweight IR local-CSE compile\n");
    } else {
        const Chunk* duplicate_expr_chunk =
            find_compiled_function_chunk(&local_cse_compile, "duplicateExpr");
        const Chunk* manual_reuse_chunk =
            find_compiled_function_chunk(&local_cse_compile, "manualReuse");
        const Chunk* barrier_no_reuse_chunk =
            find_compiled_function_chunk(&local_cse_compile, "barrierNoReuse");

        int duplicate_add_ops = chunk_count_add_like_ops(duplicate_expr_chunk);
        int manual_add_ops = chunk_count_add_like_ops(manual_reuse_chunk);
        int barrier_add_ops = chunk_count_add_like_ops(barrier_no_reuse_chunk);

        bool local_cse_applied =
            duplicate_expr_chunk && manual_reuse_chunk &&
            (duplicate_expr_chunk->code_count <= manual_reuse_chunk->code_count + 3) &&
            (duplicate_add_ops <= manual_add_ops);

        bool local_cse_is_conservative =
            barrier_no_reuse_chunk && duplicate_expr_chunk &&
            (duplicate_expr_chunk->code_count + 2 <= barrier_no_reuse_chunk->code_count) &&
            (barrier_add_ops >= duplicate_add_ops);

        if (local_cse_applied && local_cse_is_conservative) {
            tests_passed++;
            printf("  PASS: lightweight IR reuses repeated pure expressions within straight-line blocks\n");
        } else {
            tests_failed++;
            int dup_count = duplicate_expr_chunk ? duplicate_expr_chunk->code_count : -1;
            int manual_count = manual_reuse_chunk ? manual_reuse_chunk->code_count : -1;
            int barrier_count = barrier_no_reuse_chunk ? barrier_no_reuse_chunk->code_count : -1;
            printf("  FAIL: lightweight IR reuses repeated pure expressions within straight-line blocks (dup=%d manual=%d barrier=%d addDup=%d addManual=%d addBarrier=%d)\n",
                   dup_count,
                   manual_count,
                   barrier_count,
                   duplicate_add_ops,
                   manual_add_ops,
                   barrier_add_ops);
        }
    }

    for (int i = 0; i < local_cse_compile.function_count; i++) {
        if (local_cse_compile.functions && local_cse_compile.functions[i]) {
            obj_function_free(local_cse_compile.functions[i]);
        }
    }
    if (local_cse_compile.functions) free(local_cse_compile.functions);
    if (local_cse_compile.function) obj_function_free(local_cse_compile.function);
    symbol_table_free(local_cse_compile.globals);
    error_free(local_cse_compile.error);

    symbol_table_free(local_cse_tc.globals);
    error_free(local_cse_tc.error);
    parser_free_result(&local_cse_parse);

    const char* ir_copy_prop_dse_source =
        "func copyPropChain(a: int, b: int): int {\n"
        "    var x: int = a;\n"
        "    var y: int = x;\n"
        "    var z: int = y + b;\n"
        "    return z;\n"
        "}\n"
        "func copyPropManual(a: int, b: int): int {\n"
        "    var z: int = a + b;\n"
        "    return z;\n"
        "}\n"
        "func copyPropBarrier(a: int, b: int): int {\n"
        "    var x: int = a;\n"
        "    println(\"barrier\");\n"
        "    var z: int = x + b;\n"
        "    return z;\n"
        "}\n"
        "func deadStoreChain(a: int): int {\n"
        "    var trash: int = 0;\n"
        "    trash = a;\n"
        "    trash = 1;\n"
        "    var keep: int = a;\n"
        "    return keep;\n"
        "}\n"
        "func deadStoreManual(a: int): int {\n"
        "    var keep: int = a;\n"
        "    return keep;\n"
        "}\n"
        "func deadStoreConservative(a: int, b: int): int {\n"
        "    var trash: int = a + b;\n"
        "    return a;\n"
        "}\n";
    ParseResult ir_copy_prop_dse_parse = parser_parse(ir_copy_prop_dse_source, "test.tblo");
    if (ir_copy_prop_dse_parse.error || !ir_copy_prop_dse_parse.program) {
        tests_failed++;
        printf("  FAIL: lightweight IR copy-prop/dead-store parse\n");
        parser_free_result(&ir_copy_prop_dse_parse);
        return;
    }

    TypeCheckResult ir_copy_prop_dse_tc = typecheck(ir_copy_prop_dse_parse.program);
    if (ir_copy_prop_dse_tc.error) {
        tests_failed++;
        printf("  FAIL: lightweight IR copy-prop/dead-store typecheck\n");
        symbol_table_free(ir_copy_prop_dse_tc.globals);
        error_free(ir_copy_prop_dse_tc.error);
        parser_free_result(&ir_copy_prop_dse_parse);
        return;
    }

    CompileResult ir_copy_prop_dse_compile = compile(ir_copy_prop_dse_parse.program);
    if (ir_copy_prop_dse_compile.error || !ir_copy_prop_dse_compile.function) {
        tests_failed++;
        printf("  FAIL: lightweight IR copy-prop/dead-store compile\n");
    } else {
        const Chunk* copy_prop_chain_chunk =
            find_compiled_function_chunk(&ir_copy_prop_dse_compile, "copyPropChain");
        const Chunk* copy_prop_manual_chunk =
            find_compiled_function_chunk(&ir_copy_prop_dse_compile, "copyPropManual");
        const Chunk* copy_prop_barrier_chunk =
            find_compiled_function_chunk(&ir_copy_prop_dse_compile, "copyPropBarrier");
        const Chunk* dead_store_chain_chunk =
            find_compiled_function_chunk(&ir_copy_prop_dse_compile, "deadStoreChain");
        const Chunk* dead_store_manual_chunk =
            find_compiled_function_chunk(&ir_copy_prop_dse_compile, "deadStoreManual");
        const Chunk* dead_store_conservative_chunk =
            find_compiled_function_chunk(&ir_copy_prop_dse_compile, "deadStoreConservative");

        int copy_chain_add_ops = chunk_count_add_like_ops(copy_prop_chain_chunk);
        int copy_manual_add_ops = chunk_count_add_like_ops(copy_prop_manual_chunk);
        int copy_barrier_add_ops = chunk_count_add_like_ops(copy_prop_barrier_chunk);

        int dead_chain_store_ops = chunk_count_opcode(dead_store_chain_chunk, OP_STORE_LOCAL);
        int dead_manual_store_ops = chunk_count_opcode(dead_store_manual_chunk, OP_STORE_LOCAL);
        int dead_conservative_store_ops = chunk_count_opcode(dead_store_conservative_chunk, OP_STORE_LOCAL);

        bool propagated_copy_chain =
            copy_prop_chain_chunk && copy_prop_manual_chunk &&
            (copy_chain_add_ops <= copy_manual_add_ops) &&
            (copy_prop_chain_chunk->code_count <= copy_prop_manual_chunk->code_count + 4);
        bool copy_prop_barrier_conservative =
            copy_prop_barrier_chunk && copy_prop_chain_chunk &&
            (copy_prop_barrier_chunk->code_count >= copy_prop_chain_chunk->code_count + 2) &&
            (copy_barrier_add_ops >= copy_chain_add_ops);
        bool eliminated_dead_simple_stores =
            dead_store_chain_chunk && dead_store_manual_chunk &&
            (dead_chain_store_ops <= dead_manual_store_ops + 1) &&
            (dead_store_chain_chunk->code_count <= dead_store_manual_chunk->code_count + 4);
        bool retained_nontrivial_dead_store =
            dead_store_conservative_chunk && dead_store_manual_chunk &&
            (dead_conservative_store_ops >= dead_manual_store_ops + 1) &&
            (dead_store_conservative_chunk->code_count >= dead_store_manual_chunk->code_count + 2);

        if (propagated_copy_chain &&
            copy_prop_barrier_conservative &&
            eliminated_dead_simple_stores &&
            retained_nontrivial_dead_store) {
            tests_passed++;
            printf("  PASS: lightweight IR copy propagation and dead-store elimination on straight-line locals\n");
        } else {
            tests_failed++;
            int copy_chain_count = copy_prop_chain_chunk ? copy_prop_chain_chunk->code_count : -1;
            int copy_manual_count = copy_prop_manual_chunk ? copy_prop_manual_chunk->code_count : -1;
            int copy_barrier_count = copy_prop_barrier_chunk ? copy_prop_barrier_chunk->code_count : -1;
            int dead_chain_count = dead_store_chain_chunk ? dead_store_chain_chunk->code_count : -1;
            int dead_manual_count = dead_store_manual_chunk ? dead_store_manual_chunk->code_count : -1;
            int dead_conservative_count = dead_store_conservative_chunk ? dead_store_conservative_chunk->code_count : -1;
            printf("  FAIL: lightweight IR copy propagation and dead-store elimination on straight-line locals (copyChain=%d copyManual=%d copyBarrier=%d addChain=%d addManual=%d addBarrier=%d deadChain=%d deadManual=%d deadConservative=%d storeDeadChain=%d storeDeadManual=%d storeDeadConservative=%d)\n",
                   copy_chain_count,
                   copy_manual_count,
                   copy_barrier_count,
                   copy_chain_add_ops,
                   copy_manual_add_ops,
                   copy_barrier_add_ops,
                   dead_chain_count,
                   dead_manual_count,
                   dead_conservative_count,
                   dead_chain_store_ops,
                   dead_manual_store_ops,
                   dead_conservative_store_ops);
        }
    }

    for (int i = 0; i < ir_copy_prop_dse_compile.function_count; i++) {
        if (ir_copy_prop_dse_compile.functions && ir_copy_prop_dse_compile.functions[i]) {
            obj_function_free(ir_copy_prop_dse_compile.functions[i]);
        }
    }
    if (ir_copy_prop_dse_compile.functions) free(ir_copy_prop_dse_compile.functions);
    if (ir_copy_prop_dse_compile.function) obj_function_free(ir_copy_prop_dse_compile.function);
    symbol_table_free(ir_copy_prop_dse_compile.globals);
    error_free(ir_copy_prop_dse_compile.error);

    symbol_table_free(ir_copy_prop_dse_tc.globals);
    error_free(ir_copy_prop_dse_tc.error);
    parser_free_result(&ir_copy_prop_dse_parse);

    const char* ir_nested_block_source =
        "func nestedCopy(a: int, b: int): int {\n"
        "    {\n"
        "        var x: int = a;\n"
        "        var y: int = x + b;\n"
        "        return y;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "func nestedManual(a: int, b: int): int {\n"
        "    {\n"
        "        return a + b;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "func nestedBarrier(a: int, b: int): int {\n"
        "    {\n"
        "        var x: int = a;\n"
        "        println(\"barrier\");\n"
        "        var y: int = x + b;\n"
        "        return y;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    ParseResult ir_nested_block_parse = parser_parse(ir_nested_block_source, "test.tblo");
    if (ir_nested_block_parse.error || !ir_nested_block_parse.program) {
        tests_failed++;
        printf("  FAIL: recursive lightweight IR nested-block parse\n");
        parser_free_result(&ir_nested_block_parse);
        return;
    }

    TypeCheckResult ir_nested_block_tc = typecheck(ir_nested_block_parse.program);
    if (ir_nested_block_tc.error) {
        tests_failed++;
        printf("  FAIL: recursive lightweight IR nested-block typecheck\n");
        symbol_table_free(ir_nested_block_tc.globals);
        error_free(ir_nested_block_tc.error);
        parser_free_result(&ir_nested_block_parse);
        return;
    }

    CompileResult ir_nested_block_compile = compile(ir_nested_block_parse.program);
    if (ir_nested_block_compile.error || !ir_nested_block_compile.function) {
        tests_failed++;
        printf("  FAIL: recursive lightweight IR nested-block compile\n");
    } else {
        const Chunk* nested_copy_chunk =
            find_compiled_function_chunk(&ir_nested_block_compile, "nestedCopy");
        const Chunk* nested_manual_chunk =
            find_compiled_function_chunk(&ir_nested_block_compile, "nestedManual");
        const Chunk* nested_barrier_chunk =
            find_compiled_function_chunk(&ir_nested_block_compile, "nestedBarrier");

        bool nested_copy_optimized =
            nested_copy_chunk &&
            chunk_contains_add_locals_int_operands(nested_copy_chunk, 0, 1);
        bool nested_manual_matches =
            nested_manual_chunk &&
            chunk_contains_add_locals_int_operands(nested_manual_chunk, 0, 1);
        bool nested_barrier_conservative =
            nested_barrier_chunk &&
            chunk_contains_add_locals_int_operands(nested_barrier_chunk, 2, 1);

        if (nested_copy_optimized && nested_manual_matches && nested_barrier_conservative) {
            tests_passed++;
            printf("  PASS: recursive lightweight IR propagates copies within nested blocks\n");
        } else {
            tests_failed++;
            int nested_copy_count = nested_copy_chunk ? nested_copy_chunk->code_count : -1;
            int nested_manual_count = nested_manual_chunk ? nested_manual_chunk->code_count : -1;
            int nested_barrier_count = nested_barrier_chunk ? nested_barrier_chunk->code_count : -1;
            printf("  FAIL: recursive lightweight IR propagates copies within nested blocks (copy=%d manual=%d barrier=%d copyAB=%d manualAB=%d barrierXB=%d)\n",
                   nested_copy_count,
                   nested_manual_count,
                   nested_barrier_count,
                   nested_copy_optimized ? 1 : 0,
                   nested_manual_matches ? 1 : 0,
                   nested_barrier_conservative ? 1 : 0);
        }
    }

    for (int i = 0; i < ir_nested_block_compile.function_count; i++) {
        if (ir_nested_block_compile.functions && ir_nested_block_compile.functions[i]) {
            obj_function_free(ir_nested_block_compile.functions[i]);
        }
    }
    if (ir_nested_block_compile.functions) free(ir_nested_block_compile.functions);
    if (ir_nested_block_compile.function) obj_function_free(ir_nested_block_compile.function);
    symbol_table_free(ir_nested_block_compile.globals);
    error_free(ir_nested_block_compile.error);

    symbol_table_free(ir_nested_block_tc.globals);
    error_free(ir_nested_block_tc.error);
    parser_free_result(&ir_nested_block_parse);

    const char* ir_block_value_numbering_source =
        "func commuteBlock(a: int, b: int): int {\n"
        "    var first: int = a + b;\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func commuteAlias(a: int, b: int): int {\n"
        "    var alias: int = a;\n"
        "    var first: int = alias + b;\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func commuteManual(a: int, b: int): int {\n"
        "    var first: int = a + b;\n"
        "    var second: int = first;\n"
        "    return second;\n"
        "}\n"
        "func commuteBarrier(a: int, b: int, allow: bool): int {\n"
        "    var first: int = a + b;\n"
        "    if (allow) {\n"
        "        println(\"x\");\n"
        "    }\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n";
    ParseResult ir_block_value_numbering_parse =
        parser_parse(ir_block_value_numbering_source, "test.tblo");
    if (ir_block_value_numbering_parse.error || !ir_block_value_numbering_parse.program) {
        tests_failed++;
        printf("  FAIL: block-local value numbering parse\n");
        parser_free_result(&ir_block_value_numbering_parse);
        return;
    }

    TypeCheckResult ir_block_value_numbering_tc = typecheck(ir_block_value_numbering_parse.program);
    if (ir_block_value_numbering_tc.error) {
        tests_failed++;
        printf("  FAIL: block-local value numbering typecheck\n");
        symbol_table_free(ir_block_value_numbering_tc.globals);
        error_free(ir_block_value_numbering_tc.error);
        parser_free_result(&ir_block_value_numbering_parse);
        return;
    }

    CompileResult ir_block_value_numbering_compile = compile(ir_block_value_numbering_parse.program);
    if (ir_block_value_numbering_compile.error || !ir_block_value_numbering_compile.function) {
        tests_failed++;
        printf("  FAIL: block-local value numbering compile\n");
    } else {
        const Chunk* commute_block_chunk =
            find_compiled_function_chunk(&ir_block_value_numbering_compile, "commuteBlock");
        const Chunk* commute_alias_chunk =
            find_compiled_function_chunk(&ir_block_value_numbering_compile, "commuteAlias");
        const Chunk* commute_manual_chunk =
            find_compiled_function_chunk(&ir_block_value_numbering_compile, "commuteManual");
        const Chunk* commute_barrier_chunk =
            find_compiled_function_chunk(&ir_block_value_numbering_compile, "commuteBarrier");

        int commute_block_add_ops = chunk_count_add_like_ops(commute_block_chunk);
        int commute_alias_add_ops = chunk_count_add_like_ops(commute_alias_chunk);
        int commute_manual_add_ops = chunk_count_add_like_ops(commute_manual_chunk);
        int commute_barrier_add_ops = chunk_count_add_like_ops(commute_barrier_chunk);

        bool reused_commutative_value =
            commute_block_chunk &&
            commute_manual_chunk &&
            commute_block_add_ops == commute_manual_add_ops &&
            commute_block_add_ops == 1;
        bool reused_alias_normalized_value =
            commute_alias_chunk &&
            commute_manual_chunk &&
            commute_alias_add_ops == commute_manual_add_ops;
        bool stayed_block_local =
            commute_barrier_chunk &&
            commute_manual_chunk &&
            commute_barrier_add_ops >= commute_manual_add_ops + 1;

        if (reused_commutative_value && reused_alias_normalized_value && stayed_block_local) {
            tests_passed++;
            printf("  PASS: block-local value numbering reuses alias-normalized commutative expressions conservatively\n");
        } else {
            tests_failed++;
            printf("  FAIL: block-local value numbering reuses alias-normalized commutative expressions conservatively (block=%d alias=%d manual=%d barrier=%d)\n",
                   commute_block_add_ops,
                   commute_alias_add_ops,
                   commute_manual_add_ops,
                   commute_barrier_add_ops);
        }
    }

    for (int i = 0; i < ir_block_value_numbering_compile.function_count; i++) {
        if (ir_block_value_numbering_compile.functions &&
            ir_block_value_numbering_compile.functions[i]) {
            obj_function_free(ir_block_value_numbering_compile.functions[i]);
        }
    }
    if (ir_block_value_numbering_compile.functions) free(ir_block_value_numbering_compile.functions);
    if (ir_block_value_numbering_compile.function) obj_function_free(ir_block_value_numbering_compile.function);
    symbol_table_free(ir_block_value_numbering_compile.globals);
    error_free(ir_block_value_numbering_compile.error);

    symbol_table_free(ir_block_value_numbering_tc.globals);
    error_free(ir_block_value_numbering_tc.error);
    parser_free_result(&ir_block_value_numbering_parse);

    const char* ir_fallthrough_copy_flow_source =
        "func flowBlock(a: int, b: int): int {\n"
        "    var x: int = a;\n"
        "    {\n"
        "        var tmp: int = 1;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowBlockManual(a: int, b: int): int {\n"
        "    {\n"
        "        var tmp: int = 1;\n"
        "    }\n"
        "    return a + b;\n"
        "}\n"
        "func flowIf(a: int, b: int, cond: bool): int {\n"
        "    var x: int = a;\n"
        "    if (cond) {\n"
        "        var tmp: int = 1;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowIfManual(a: int, b: int, cond: bool): int {\n"
        "    if (cond) {\n"
        "        var tmp: int = 1;\n"
        "    }\n"
        "    return a + b;\n"
        "}\n"
        "func flowIfClobber(a: int, b: int, cond: bool): int {\n"
        "    var x: int = a;\n"
        "    if (cond) {\n"
        "        x = 1;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowIfJoin(a: int, b: int, cond: bool): int {\n"
        "    var x: int = 0;\n"
        "    if (cond) {\n"
        "        x = a;\n"
        "    } else {\n"
        "        x = a;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowIfJoinClobber(a: int, b: int, cond: bool): int {\n"
        "    var x: int = 0;\n"
        "    if (cond) {\n"
        "        x = a;\n"
        "    } else {\n"
        "        x = b;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowMatchJoin(a: int, b: int, tag: int): int {\n"
        "    var x: int = 0;\n"
        "    match (tag) {\n"
        "        0: { x = a; }\n"
        "        1: { x = a; }\n"
        "        else: { x = a; }\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowMatchJoinClobber(a: int, b: int, tag: int): int {\n"
        "    var x: int = 0;\n"
        "    match (tag) {\n"
        "        0: { x = a; }\n"
        "        1: { x = a; }\n"
        "        else: { x = b; }\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowChainedJoins(a: int, b: int, cond: bool, tag: int): int {\n"
        "    var x: int = 0;\n"
        "    if (cond) {\n"
        "        x = a;\n"
        "    } else {\n"
        "        x = a;\n"
        "    }\n"
        "    match (tag) {\n"
        "        0: { x = a; }\n"
        "        1: { x = a; }\n"
        "        else: { x = a; }\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowChainedJoinsClobber(a: int, b: int, cond: bool, tag: int): int {\n"
        "    var x: int = 0;\n"
        "    if (cond) {\n"
        "        x = a;\n"
        "    } else {\n"
        "        x = a;\n"
        "    }\n"
        "    match (tag) {\n"
        "        0: { x = a; }\n"
        "        1: { x = a; }\n"
        "        else: { x = b; }\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopExit(a: int, b: int, cond: bool): int {\n"
        "    var x: int = a;\n"
        "    while (cond) {\n"
        "        var tmp: int = 1;\n"
        "        break;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopExitClobber(a: int, b: int, cond: bool): int {\n"
        "    var x: int = a;\n"
        "    while (cond) {\n"
        "        x = b;\n"
        "        break;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopBreakIf(a: int, b: int, cond: bool, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        if (cond) {\n"
        "            var tmp: int = 1;\n"
        "            break;\n"
        "        } else {\n"
        "            var tmp2: int = 2;\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopBreakIfClobber(a: int, b: int, cond: bool, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        if (cond) {\n"
        "            x = b;\n"
        "            break;\n"
        "        } else {\n"
        "            var tmp: int = 1;\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopBreakMatch(a: int, b: int, tag: int, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        match (tag) {\n"
        "            0: { var tmp: int = 1; break; }\n"
        "            else: { var tmp2: int = 2; break; }\n"
        "        }\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopBreakMatchClobber(a: int, b: int, tag: int, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        match (tag) {\n"
        "            0: { x = b; break; }\n"
        "            else: { var tmp: int = 1; break; }\n"
        "        }\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopBreakContinue(a: int, b: int, cond: bool, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        if (cond) {\n"
        "            current = current - 1;\n"
        "            continue;\n"
        "        }\n"
        "        break;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopBreakContinueClobber(a: int, b: int, cond: bool, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        if (cond) {\n"
        "            x = b;\n"
        "            current = current - 1;\n"
        "            continue;\n"
        "        }\n"
        "        break;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowNestedLoop(a: int, b: int, cond: bool, outer: int, inner_seed: int): int {\n"
        "    var x: int = a;\n"
        "    var outerCurrent: int = outer;\n"
        "    while (outerCurrent > 0) {\n"
        "        var inner: int = inner_seed;\n"
        "        while (inner > 0) {\n"
        "            if (cond) {\n"
        "                var tmp: int = 1;\n"
        "            } else {\n"
        "                var tmp2: int = 2;\n"
        "            }\n"
        "            inner = inner - 1;\n"
        "        }\n"
        "        outerCurrent = outerCurrent - 1;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowNestedLoopClobber(a: int, b: int, cond: bool, outer: int, inner_seed: int): int {\n"
        "    var x: int = a;\n"
        "    var outerCurrent: int = outer;\n"
        "    while (outerCurrent > 0) {\n"
        "        var inner: int = inner_seed;\n"
        "        while (inner > 0) {\n"
        "            if (cond) {\n"
        "                x = b;\n"
        "            } else {\n"
        "                var tmp: int = 1;\n"
        "            }\n"
        "            inner = inner - 1;\n"
        "        }\n"
        "        outerCurrent = outerCurrent - 1;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopBackedge(a: int, b: int, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopBackedgeClobber(a: int, b: int, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        x = b;\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopIfBackedge(a: int, b: int, cond: bool, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        if (cond) {\n"
        "            var tmp: int = 1;\n"
        "        } else {\n"
        "            var tmp2: int = 2;\n"
        "        }\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopIfBackedgeClobber(a: int, b: int, cond: bool, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        if (cond) {\n"
        "            x = b;\n"
        "        } else {\n"
            "            var tmp: int = 1;\n"
        "        }\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopMatchBackedge(a: int, b: int, tag: int, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        match (tag) {\n"
        "            0: { var tmp: int = 1; }\n"
        "            else: { var tmp2: int = 2; }\n"
        "        }\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopMatchBackedgeClobber(a: int, b: int, tag: int, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        match (tag) {\n"
        "            0: { x = b; }\n"
        "            else: { var tmp: int = 1; }\n"
        "        }\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopContinueBackedge(a: int, b: int, cond: bool, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        if (cond) {\n"
        "            current = current - 1;\n"
        "            continue;\n"
        "        }\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n"
        "func flowLoopContinueBackedgeClobber(a: int, b: int, cond: bool, remaining: int): int {\n"
        "    var x: int = a;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        if (cond) {\n"
        "            x = b;\n"
        "            current = current - 1;\n"
        "            continue;\n"
        "        }\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var y: int = x + b;\n"
        "    return y;\n"
        "}\n";
    ParseResult ir_fallthrough_copy_flow_parse =
        parser_parse(ir_fallthrough_copy_flow_source, "test.tblo");
    if (ir_fallthrough_copy_flow_parse.error || !ir_fallthrough_copy_flow_parse.program) {
        tests_failed++;
        printf("  FAIL: forward fallthrough copy-flow parse\n");
        parser_free_result(&ir_fallthrough_copy_flow_parse);
        return;
    }

    TypeCheckResult ir_fallthrough_copy_flow_tc = typecheck(ir_fallthrough_copy_flow_parse.program);
    if (ir_fallthrough_copy_flow_tc.error) {
        tests_failed++;
        printf("  FAIL: forward fallthrough copy-flow typecheck\n");
        symbol_table_free(ir_fallthrough_copy_flow_tc.globals);
        error_free(ir_fallthrough_copy_flow_tc.error);
        parser_free_result(&ir_fallthrough_copy_flow_parse);
        return;
    }

    CompileResult ir_fallthrough_copy_flow_compile = compile(ir_fallthrough_copy_flow_parse.program);
    if (ir_fallthrough_copy_flow_compile.error || !ir_fallthrough_copy_flow_compile.function) {
        tests_failed++;
        printf("  FAIL: forward fallthrough copy-flow compile\n");
    } else {
        const Chunk* flow_block_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowBlock");
        const Chunk* flow_block_manual_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowBlockManual");
        const Chunk* flow_if_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowIf");
        const Chunk* flow_if_manual_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowIfManual");
        const Chunk* flow_if_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowIfClobber");
        const Chunk* flow_if_join_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowIfJoin");
        const Chunk* flow_if_join_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowIfJoinClobber");
        const Chunk* flow_match_join_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowMatchJoin");
        const Chunk* flow_match_join_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowMatchJoinClobber");
        const Chunk* flow_chained_join_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowChainedJoins");
        const Chunk* flow_chained_join_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowChainedJoinsClobber");
        const Chunk* flow_loop_exit_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopExit");
        const Chunk* flow_loop_exit_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopExitClobber");
        const Chunk* flow_loop_break_if_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopBreakIf");
        const Chunk* flow_loop_break_if_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopBreakIfClobber");
        const Chunk* flow_loop_break_match_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopBreakMatch");
        const Chunk* flow_loop_break_match_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopBreakMatchClobber");
        const Chunk* flow_loop_break_continue_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopBreakContinue");
        const Chunk* flow_loop_break_continue_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopBreakContinueClobber");
        const Chunk* flow_nested_loop_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowNestedLoop");
        const Chunk* flow_nested_loop_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowNestedLoopClobber");
        const Chunk* flow_loop_backedge_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopBackedge");
        const Chunk* flow_loop_backedge_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopBackedgeClobber");
        const Chunk* flow_loop_if_backedge_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopIfBackedge");
        const Chunk* flow_loop_if_backedge_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopIfBackedgeClobber");
        const Chunk* flow_loop_match_backedge_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopMatchBackedge");
        const Chunk* flow_loop_match_backedge_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopMatchBackedgeClobber");
        const Chunk* flow_loop_continue_backedge_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopContinueBackedge");
        const Chunk* flow_loop_continue_backedge_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_copy_flow_compile, "flowLoopContinueBackedgeClobber");

        bool block_carried_alias =
            flow_block_chunk &&
            flow_block_manual_chunk &&
            chunk_contains_add_locals_int_operands(flow_block_chunk, 0, 1) &&
            chunk_contains_add_locals_int_operands(flow_block_manual_chunk, 0, 1);
        bool if_carried_alias =
            flow_if_chunk &&
            flow_if_manual_chunk &&
            chunk_contains_add_locals_int_operands(flow_if_chunk, 0, 1) &&
            chunk_contains_add_locals_int_operands(flow_if_manual_chunk, 0, 1);
        bool clobber_stayed_conservative =
            flow_if_clobber_chunk &&
            chunk_contains_add_locals_int_operands(flow_if_clobber_chunk, 3, 1);
        bool join_carried_alias =
            flow_if_join_chunk &&
            chunk_contains_add_locals_int_operands(flow_if_join_chunk, 0, 1);
        bool join_clobber_stayed_conservative =
            flow_if_join_clobber_chunk &&
            chunk_contains_add_locals_int_operands(flow_if_join_clobber_chunk, 3, 1);
        bool match_join_carried_alias =
            flow_match_join_chunk &&
            chunk_contains_add_locals_int_operands(flow_match_join_chunk, 0, 1);
        bool match_join_clobber_stayed_conservative =
            flow_match_join_clobber_chunk &&
            chunk_contains_add_locals_int_operands(flow_match_join_clobber_chunk, 3, 1);
        bool chained_join_carried_alias =
            flow_chained_join_chunk &&
            chunk_contains_add_locals_int_operands(flow_chained_join_chunk, 0, 1);
        bool chained_join_clobber_stayed_conservative =
            flow_chained_join_clobber_chunk &&
            chunk_contains_add_locals_int_operands(flow_chained_join_clobber_chunk, 4, 1);
        bool loop_exit_carried_alias =
            flow_loop_exit_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_exit_chunk, 0, 1);
        bool loop_exit_clobber_stayed_conservative =
            flow_loop_exit_clobber_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_exit_clobber_chunk, 3, 1);
        bool loop_break_if_carried_alias =
            flow_loop_break_if_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_break_if_chunk, 0, 1);
        bool loop_break_if_clobber_stayed_conservative =
            flow_loop_break_if_clobber_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_break_if_clobber_chunk, 4, 1);
        bool loop_break_match_carried_alias =
            flow_loop_break_match_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_break_match_chunk, 0, 1);
        bool loop_break_match_clobber_stayed_conservative =
            flow_loop_break_match_clobber_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_break_match_clobber_chunk, 4, 1);
        bool loop_break_continue_carried_alias =
            flow_loop_break_continue_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_break_continue_chunk, 0, 1);
        bool loop_break_continue_clobber_stayed_conservative =
            flow_loop_break_continue_clobber_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_break_continue_clobber_chunk, 4, 1);
        bool nested_loop_carried_alias =
            flow_nested_loop_chunk &&
            chunk_contains_add_locals_int_operands(flow_nested_loop_chunk, 0, 1);
        bool nested_loop_clobber_stayed_conservative =
            flow_nested_loop_clobber_chunk &&
            chunk_contains_add_locals_int_operands(flow_nested_loop_clobber_chunk, 5, 1);
        bool loop_backedge_carried_alias =
            flow_loop_backedge_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_backedge_chunk, 0, 1);
        bool loop_backedge_clobber_stayed_conservative =
            flow_loop_backedge_clobber_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_backedge_clobber_chunk, 3, 1);
        bool loop_if_backedge_carried_alias =
            flow_loop_if_backedge_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_if_backedge_chunk, 0, 1);
        bool loop_if_backedge_clobber_stayed_conservative =
            flow_loop_if_backedge_clobber_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_if_backedge_clobber_chunk, 4, 1);
        bool loop_match_backedge_carried_alias =
            flow_loop_match_backedge_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_match_backedge_chunk, 0, 1);
        bool loop_match_backedge_clobber_stayed_conservative =
            flow_loop_match_backedge_clobber_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_match_backedge_clobber_chunk, 4, 1);
        bool loop_continue_backedge_carried_alias =
            flow_loop_continue_backedge_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_continue_backedge_chunk, 0, 1);
        bool loop_continue_backedge_clobber_stayed_conservative =
            flow_loop_continue_backedge_clobber_chunk &&
            chunk_contains_add_locals_int_operands(flow_loop_continue_backedge_clobber_chunk, 4, 1);

        if (block_carried_alias &&
            if_carried_alias &&
            clobber_stayed_conservative &&
            join_carried_alias &&
            join_clobber_stayed_conservative &&
            match_join_carried_alias &&
            match_join_clobber_stayed_conservative &&
            chained_join_carried_alias &&
            chained_join_clobber_stayed_conservative &&
            loop_exit_carried_alias &&
            loop_exit_clobber_stayed_conservative &&
            loop_break_if_carried_alias &&
            loop_break_if_clobber_stayed_conservative &&
            loop_break_match_carried_alias &&
            loop_break_match_clobber_stayed_conservative &&
            loop_break_continue_carried_alias &&
            loop_break_continue_clobber_stayed_conservative &&
            nested_loop_carried_alias &&
            nested_loop_clobber_stayed_conservative &&
            loop_backedge_carried_alias &&
            loop_backedge_clobber_stayed_conservative &&
            loop_if_backedge_carried_alias &&
            loop_if_backedge_clobber_stayed_conservative &&
            loop_match_backedge_carried_alias &&
            loop_match_backedge_clobber_stayed_conservative &&
            loop_continue_backedge_carried_alias &&
            loop_continue_backedge_clobber_stayed_conservative) {
            tests_passed++;
            printf("  PASS: forward copy-flow carries aliases only across compatible non-clobbering joins, loop exits, loop backedges, and nested loops\n");
        } else {
            tests_failed++;
            printf("  FAIL: forward copy-flow carries aliases only across compatible non-clobbering joins, loop exits, loop backedges, and nested loops (block=%d if=%d clobber=%d join=%d joinClobber=%d matchJoin=%d matchJoinClobber=%d chained=%d chainedClobber=%d loop=%d loopClobber=%d breakIf=%d breakIfClobber=%d breakMatch=%d breakMatchClobber=%d breakContinue=%d breakContinueClobber=%d nested=%d nestedClobber=%d backedge=%d backedgeClobber=%d loopIf=%d loopIfClobber=%d loopMatch=%d loopMatchClobber=%d loopContinue=%d loopContinueClobber=%d)\n",
                   block_carried_alias ? 1 : 0,
                   if_carried_alias ? 1 : 0,
                   clobber_stayed_conservative ? 1 : 0,
                   join_carried_alias ? 1 : 0,
                   join_clobber_stayed_conservative ? 1 : 0,
                   match_join_carried_alias ? 1 : 0,
                   match_join_clobber_stayed_conservative ? 1 : 0,
                   chained_join_carried_alias ? 1 : 0,
                   chained_join_clobber_stayed_conservative ? 1 : 0,
                   loop_exit_carried_alias ? 1 : 0,
                   loop_exit_clobber_stayed_conservative ? 1 : 0,
                   loop_break_if_carried_alias ? 1 : 0,
                   loop_break_if_clobber_stayed_conservative ? 1 : 0,
                   loop_break_match_carried_alias ? 1 : 0,
                   loop_break_match_clobber_stayed_conservative ? 1 : 0,
                   loop_break_continue_carried_alias ? 1 : 0,
                   loop_break_continue_clobber_stayed_conservative ? 1 : 0,
                   nested_loop_carried_alias ? 1 : 0,
                   nested_loop_clobber_stayed_conservative ? 1 : 0,
                   loop_backedge_carried_alias ? 1 : 0,
                   loop_backedge_clobber_stayed_conservative ? 1 : 0,
                   loop_if_backedge_carried_alias ? 1 : 0,
                   loop_if_backedge_clobber_stayed_conservative ? 1 : 0,
                   loop_match_backedge_carried_alias ? 1 : 0,
                   loop_match_backedge_clobber_stayed_conservative ? 1 : 0,
                   loop_continue_backedge_carried_alias ? 1 : 0,
                   loop_continue_backedge_clobber_stayed_conservative ? 1 : 0);
        }
    }

    for (int i = 0; i < ir_fallthrough_copy_flow_compile.function_count; i++) {
        if (ir_fallthrough_copy_flow_compile.functions &&
            ir_fallthrough_copy_flow_compile.functions[i]) {
            obj_function_free(ir_fallthrough_copy_flow_compile.functions[i]);
        }
    }
    if (ir_fallthrough_copy_flow_compile.functions) free(ir_fallthrough_copy_flow_compile.functions);
    if (ir_fallthrough_copy_flow_compile.function) obj_function_free(ir_fallthrough_copy_flow_compile.function);
    symbol_table_free(ir_fallthrough_copy_flow_compile.globals);
    error_free(ir_fallthrough_copy_flow_compile.error);

    symbol_table_free(ir_fallthrough_copy_flow_tc.globals);
    error_free(ir_fallthrough_copy_flow_tc.error);
    parser_free_result(&ir_fallthrough_copy_flow_parse);

    const char* ir_fallthrough_value_flow_source =
        "func valueFlowBlock(a: int, b: int): int {\n"
        "    var first: int = a + b;\n"
        "    {\n"
        "        var tmp: int = 1;\n"
        "    }\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowBlockManual(a: int, b: int): int {\n"
        "    {\n"
        "        var tmp: int = 1;\n"
        "    }\n"
        "    var first: int = a + b;\n"
        "    return first;\n"
        "}\n"
        "func valueFlowIf(a: int, b: int, cond: bool): int {\n"
        "    var first: int = a + b;\n"
        "    if (cond) {\n"
        "        var tmp: int = 1;\n"
        "    }\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowIfManual(a: int, b: int, cond: bool): int {\n"
        "    if (cond) {\n"
        "        var tmp: int = 1;\n"
        "    }\n"
        "    var first: int = a + b;\n"
        "    return first;\n"
        "}\n"
        "func valueFlowIfClobber(a: int, b: int, cond: bool): int {\n"
        "    var right: int = b;\n"
        "    var first: int = a + right;\n"
        "    if (cond) {\n"
        "        right = 1;\n"
        "    }\n"
        "    var second: int = right + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowIfJoin(a: int, b: int, cond: bool): int {\n"
        "    var first: int = 0;\n"
        "    if (cond) {\n"
        "        first = a + b;\n"
        "    } else {\n"
        "        first = b + a;\n"
        "    }\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowIfJoinClobber(a: int, b: int, cond: bool): int {\n"
        "    var first: int = 0;\n"
        "    if (cond) {\n"
        "        first = a + b;\n"
        "    } else {\n"
        "        first = a + 1;\n"
        "    }\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowMatchJoin(a: int, b: int, tag: int): int {\n"
        "    var first: int = 0;\n"
        "    match (tag) {\n"
        "        0: { first = a + b; }\n"
        "        1: { first = b + a; }\n"
        "        else: { first = a + b; }\n"
        "    }\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowMatchJoinClobber(a: int, b: int, tag: int): int {\n"
        "    var first: int = 0;\n"
        "    match (tag) {\n"
        "        0: { first = a + b; }\n"
        "        1: { first = b + a; }\n"
        "        else: { first = a + 1; }\n"
        "    }\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowChainedJoins(a: int, b: int, cond: bool, tag: int): int {\n"
        "    var first: int = 0;\n"
        "    if (cond) {\n"
        "        first = a + b;\n"
        "    } else {\n"
        "        first = b + a;\n"
        "    }\n"
        "    match (tag) {\n"
        "        0: { first = a + b; }\n"
        "        1: { first = b + a; }\n"
        "        else: { first = a + b; }\n"
        "    }\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowChainedJoinsClobber(a: int, b: int, cond: bool, tag: int): int {\n"
        "    var first: int = 0;\n"
        "    if (cond) {\n"
        "        first = a + b;\n"
        "    } else {\n"
        "        first = b + a;\n"
        "    }\n"
        "    match (tag) {\n"
        "        0: { first = a + b; }\n"
        "        1: { first = b + a; }\n"
        "        else: { first = a + 1; }\n"
        "    }\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowLoopExit(a: int, b: int, cond: bool): int {\n"
        "    var first: int = a + b;\n"
        "    while (cond) {\n"
        "        var tmp: int = 1;\n"
        "        break;\n"
        "    }\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowLoopExitClobber(a: int, b: int, cond: bool): int {\n"
        "    var right: int = b;\n"
        "    var first: int = a + right;\n"
        "    while (cond) {\n"
        "        right = 1;\n"
        "        break;\n"
        "    }\n"
        "    var second: int = right + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowLoopBackedge(a: int, b: int, remaining: int): int {\n"
        "    var first: int = a + b;\n"
        "    while (remaining > 0) {\n"
        "        var second: int = b + a;\n"
        "        println(second);\n"
        "        remaining = remaining - 1;\n"
        "    }\n"
        "    return first;\n"
        "}\n"
        "func valueFlowLoopBackedgeClobber(a: int, b: int, remaining: int): int {\n"
        "    var right: int = b;\n"
        "    var first: int = a + right;\n"
        "    while (remaining > 0) {\n"
        "        right = remaining;\n"
        "        var second: int = right + a;\n"
        "        println(second);\n"
        "        remaining = remaining - 1;\n"
        "    }\n"
        "    return first;\n"
        "}\n"
        "func valueFlowLoopIfBackedge(a: int, b: int, cond: bool, remaining: int): int {\n"
        "    var first: int = a + b;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        if (cond) {\n"
        "            var tmp: int = 1;\n"
        "        } else {\n"
        "            var tmp2: int = 2;\n"
        "        }\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowLoopIfBackedgeClobber(a: int, b: int, cond: bool, remaining: int): int {\n"
        "    var right: int = b;\n"
        "    var first: int = a + right;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        if (cond) {\n"
        "            right = current;\n"
        "        } else {\n"
        "            var tmp: int = 1;\n"
        "        }\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var second: int = right + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowLoopMatchBackedge(a: int, b: int, tag: int, remaining: int): int {\n"
        "    var first: int = a + b;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        match (tag) {\n"
        "            0: { var tmp: int = 1; }\n"
        "            else: { var tmp2: int = 2; }\n"
        "        }\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowLoopMatchBackedgeClobber(a: int, b: int, tag: int, remaining: int): int {\n"
        "    var right: int = b;\n"
        "    var first: int = a + right;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        match (tag) {\n"
        "            0: { right = current; }\n"
        "            else: { var tmp: int = 1; }\n"
        "        }\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var second: int = right + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowLoopContinueBackedge(a: int, b: int, cond: bool, remaining: int): int {\n"
        "    var first: int = a + b;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        if (cond) {\n"
        "            current = current - 1;\n"
        "            continue;\n"
        "        }\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var second: int = b + a;\n"
        "    return second;\n"
        "}\n"
        "func valueFlowLoopContinueBackedgeClobber(a: int, b: int, cond: bool, remaining: int): int {\n"
        "    var right: int = b;\n"
        "    var first: int = a + right;\n"
        "    var current: int = remaining;\n"
        "    while (current > 0) {\n"
        "        if (cond) {\n"
        "            right = current;\n"
        "            current = current - 1;\n"
        "            continue;\n"
        "        }\n"
        "        current = current - 1;\n"
        "    }\n"
        "    var second: int = right + a;\n"
        "    return second;\n"
        "}\n";
    ParseResult ir_fallthrough_value_flow_parse =
        parser_parse(ir_fallthrough_value_flow_source, "test.tblo");
    if (ir_fallthrough_value_flow_parse.error || !ir_fallthrough_value_flow_parse.program) {
        tests_failed++;
        printf("  FAIL: forward fallthrough value-flow parse\n");
        parser_free_result(&ir_fallthrough_value_flow_parse);
        return;
    }

    TypeCheckResult ir_fallthrough_value_flow_tc = typecheck(ir_fallthrough_value_flow_parse.program);
    if (ir_fallthrough_value_flow_tc.error) {
        tests_failed++;
        printf("  FAIL: forward fallthrough value-flow typecheck\n");
        symbol_table_free(ir_fallthrough_value_flow_tc.globals);
        error_free(ir_fallthrough_value_flow_tc.error);
        parser_free_result(&ir_fallthrough_value_flow_parse);
        return;
    }

    CompileResult ir_fallthrough_value_flow_compile = compile(ir_fallthrough_value_flow_parse.program);
    if (ir_fallthrough_value_flow_compile.error || !ir_fallthrough_value_flow_compile.function) {
        tests_failed++;
        printf("  FAIL: forward fallthrough value-flow compile\n");
    } else {
        const Chunk* value_flow_block_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowBlock");
        const Chunk* value_flow_block_manual_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowBlockManual");
        const Chunk* value_flow_if_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowIf");
        const Chunk* value_flow_if_manual_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowIfManual");
        const Chunk* value_flow_if_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowIfClobber");
        const Chunk* value_flow_if_join_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowIfJoin");
        const Chunk* value_flow_if_join_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowIfJoinClobber");
        const Chunk* value_flow_match_join_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowMatchJoin");
        const Chunk* value_flow_match_join_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowMatchJoinClobber");
        const Chunk* value_flow_chained_join_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowChainedJoins");
        const Chunk* value_flow_chained_join_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowChainedJoinsClobber");
        const Chunk* value_flow_loop_exit_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowLoopExit");
        const Chunk* value_flow_loop_exit_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowLoopExitClobber");
        const Chunk* value_flow_loop_backedge_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowLoopBackedge");
        const Chunk* value_flow_loop_backedge_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowLoopBackedgeClobber");
        const Chunk* value_flow_loop_if_backedge_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowLoopIfBackedge");
        const Chunk* value_flow_loop_if_backedge_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowLoopIfBackedgeClobber");
        const Chunk* value_flow_loop_match_backedge_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowLoopMatchBackedge");
        const Chunk* value_flow_loop_match_backedge_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowLoopMatchBackedgeClobber");
        const Chunk* value_flow_loop_continue_backedge_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowLoopContinueBackedge");
        const Chunk* value_flow_loop_continue_backedge_clobber_chunk =
            find_compiled_function_chunk(&ir_fallthrough_value_flow_compile, "valueFlowLoopContinueBackedgeClobber");

        int value_flow_block_add_ops = chunk_count_add_like_ops(value_flow_block_chunk);
        int value_flow_block_manual_add_ops = chunk_count_add_like_ops(value_flow_block_manual_chunk);
        int value_flow_if_add_ops = chunk_count_add_like_ops(value_flow_if_chunk);
        int value_flow_if_manual_add_ops = chunk_count_add_like_ops(value_flow_if_manual_chunk);
        int value_flow_if_clobber_add_ops = chunk_count_add_like_ops(value_flow_if_clobber_chunk);
        int value_flow_if_join_add_ops = chunk_count_add_like_ops(value_flow_if_join_chunk);
        int value_flow_if_join_clobber_add_ops = chunk_count_add_like_ops(value_flow_if_join_clobber_chunk);
        int value_flow_match_join_add_ops = chunk_count_add_like_ops(value_flow_match_join_chunk);
        int value_flow_match_join_clobber_add_ops =
            chunk_count_add_like_ops(value_flow_match_join_clobber_chunk);
        int value_flow_chained_join_add_ops =
            chunk_count_add_like_ops(value_flow_chained_join_chunk);
        int value_flow_chained_join_clobber_add_ops =
            chunk_count_add_like_ops(value_flow_chained_join_clobber_chunk);
        int value_flow_loop_exit_add_ops =
            chunk_count_add_like_ops(value_flow_loop_exit_chunk);
        int value_flow_loop_exit_clobber_add_ops =
            chunk_count_add_like_ops(value_flow_loop_exit_clobber_chunk);
        int value_flow_loop_backedge_add_ops =
            chunk_count_add_like_ops(value_flow_loop_backedge_chunk);
        int value_flow_loop_backedge_clobber_add_ops =
            chunk_count_add_like_ops(value_flow_loop_backedge_clobber_chunk);
        int value_flow_loop_if_backedge_add_ops =
            chunk_count_add_like_ops(value_flow_loop_if_backedge_chunk);
        int value_flow_loop_if_backedge_clobber_add_ops =
            chunk_count_add_like_ops(value_flow_loop_if_backedge_clobber_chunk);
        int value_flow_loop_match_backedge_add_ops =
            chunk_count_add_like_ops(value_flow_loop_match_backedge_chunk);
        int value_flow_loop_match_backedge_clobber_add_ops =
            chunk_count_add_like_ops(value_flow_loop_match_backedge_clobber_chunk);
        int value_flow_loop_continue_backedge_add_ops =
            chunk_count_add_like_ops(value_flow_loop_continue_backedge_chunk);
        int value_flow_loop_continue_backedge_clobber_add_ops =
            chunk_count_add_like_ops(value_flow_loop_continue_backedge_clobber_chunk);

        bool block_carried_value =
            value_flow_block_chunk &&
            value_flow_block_manual_chunk &&
            value_flow_block_add_ops == value_flow_block_manual_add_ops &&
            value_flow_block_add_ops == 1;
        bool if_carried_value =
            value_flow_if_chunk &&
            value_flow_if_manual_chunk &&
            value_flow_if_add_ops == value_flow_if_manual_add_ops &&
            value_flow_if_add_ops == 1;
        bool clobber_stayed_conservative =
            value_flow_if_clobber_chunk &&
            value_flow_if_clobber_add_ops >= 2;
        bool join_carried_value =
            value_flow_if_join_chunk &&
            value_flow_if_join_add_ops == 2;
        bool join_clobber_stayed_conservative =
            value_flow_if_join_clobber_chunk &&
            value_flow_if_join_clobber_add_ops >= 3;
        bool match_join_carried_value =
            value_flow_match_join_chunk &&
            value_flow_match_join_add_ops == 3;
        bool match_join_clobber_stayed_conservative =
            value_flow_match_join_clobber_chunk &&
            value_flow_match_join_clobber_add_ops >= 4;
        bool chained_join_carried_value =
            value_flow_chained_join_chunk &&
            value_flow_chained_join_add_ops == 5;
        bool chained_join_clobber_stayed_conservative =
            value_flow_chained_join_clobber_chunk &&
            value_flow_chained_join_clobber_add_ops >= 6;
        bool loop_exit_carried_value =
            value_flow_loop_exit_chunk &&
            value_flow_loop_exit_add_ops == 1;
        bool loop_exit_clobber_stayed_conservative =
            value_flow_loop_exit_clobber_chunk &&
            value_flow_loop_exit_clobber_add_ops >= 2;
        bool loop_backedge_carried_value =
            value_flow_loop_backedge_chunk &&
            value_flow_loop_backedge_add_ops == 1;
        bool loop_backedge_clobber_stayed_conservative =
            value_flow_loop_backedge_clobber_chunk &&
            value_flow_loop_backedge_clobber_add_ops >= 2;
        bool loop_if_backedge_carried_value =
            value_flow_loop_if_backedge_chunk &&
            value_flow_loop_if_backedge_add_ops == 1;
        bool loop_if_backedge_clobber_stayed_conservative =
            value_flow_loop_if_backedge_clobber_chunk &&
            value_flow_loop_if_backedge_clobber_add_ops >= 2;
        bool loop_match_backedge_carried_value =
            value_flow_loop_match_backedge_chunk &&
            value_flow_loop_match_backedge_add_ops == 1;
        bool loop_match_backedge_clobber_stayed_conservative =
            value_flow_loop_match_backedge_clobber_chunk &&
            value_flow_loop_match_backedge_clobber_add_ops >= 2;
        bool loop_continue_backedge_carried_value =
            value_flow_loop_continue_backedge_chunk &&
            value_flow_loop_continue_backedge_add_ops == 1;
        bool loop_continue_backedge_clobber_stayed_conservative =
            value_flow_loop_continue_backedge_clobber_chunk &&
            value_flow_loop_continue_backedge_clobber_add_ops >= 2;

        if (block_carried_value &&
            if_carried_value &&
            clobber_stayed_conservative &&
            join_carried_value &&
            join_clobber_stayed_conservative &&
            match_join_carried_value &&
            match_join_clobber_stayed_conservative &&
            chained_join_carried_value &&
            chained_join_clobber_stayed_conservative &&
            loop_exit_carried_value &&
            loop_exit_clobber_stayed_conservative &&
            loop_backedge_carried_value &&
            loop_backedge_clobber_stayed_conservative &&
            loop_if_backedge_carried_value &&
            loop_if_backedge_clobber_stayed_conservative &&
            loop_match_backedge_carried_value &&
            loop_match_backedge_clobber_stayed_conservative &&
            loop_continue_backedge_carried_value &&
            loop_continue_backedge_clobber_stayed_conservative) {
            tests_passed++;
            printf("  PASS: forward value-flow carries available expressions only across compatible non-clobbering joins, loop exits, and loop backedges\n");
        } else {
            tests_failed++;
            printf("  FAIL: forward value-flow carries available expressions only across compatible non-clobbering joins, loop exits, and loop backedges (block=%d manualBlock=%d if=%d manualIf=%d clobber=%d join=%d joinClobber=%d matchJoin=%d matchJoinClobber=%d chained=%d chainedClobber=%d loop=%d loopClobber=%d backedge=%d backedgeClobber=%d loopIf=%d loopIfClobber=%d loopMatch=%d loopMatchClobber=%d loopContinue=%d loopContinueClobber=%d)\n",
                   value_flow_block_add_ops,
                   value_flow_block_manual_add_ops,
                   value_flow_if_add_ops,
                   value_flow_if_manual_add_ops,
                   value_flow_if_clobber_add_ops,
                   value_flow_if_join_add_ops,
                   value_flow_if_join_clobber_add_ops,
                   value_flow_match_join_add_ops,
                   value_flow_match_join_clobber_add_ops,
                   value_flow_chained_join_add_ops,
                   value_flow_chained_join_clobber_add_ops,
                   value_flow_loop_exit_add_ops,
                   value_flow_loop_exit_clobber_add_ops,
                   value_flow_loop_backedge_add_ops,
                   value_flow_loop_backedge_clobber_add_ops,
                   value_flow_loop_if_backedge_add_ops,
                   value_flow_loop_if_backedge_clobber_add_ops,
                   value_flow_loop_match_backedge_add_ops,
                   value_flow_loop_match_backedge_clobber_add_ops,
                   value_flow_loop_continue_backedge_add_ops,
                   value_flow_loop_continue_backedge_clobber_add_ops);
        }
    }

    for (int i = 0; i < ir_fallthrough_value_flow_compile.function_count; i++) {
        if (ir_fallthrough_value_flow_compile.functions &&
            ir_fallthrough_value_flow_compile.functions[i]) {
            obj_function_free(ir_fallthrough_value_flow_compile.functions[i]);
        }
    }
    if (ir_fallthrough_value_flow_compile.functions) free(ir_fallthrough_value_flow_compile.functions);
    if (ir_fallthrough_value_flow_compile.function) obj_function_free(ir_fallthrough_value_flow_compile.function);
    symbol_table_free(ir_fallthrough_value_flow_compile.globals);
    error_free(ir_fallthrough_value_flow_compile.error);

    symbol_table_free(ir_fallthrough_value_flow_tc.globals);
    error_free(ir_fallthrough_value_flow_tc.error);
    parser_free_result(&ir_fallthrough_value_flow_parse);

    const char* ir_nested_dead_store_source =
        "func nestedDeadStore(a: int): int {\n"
        "    {\n"
        "        var trash: int = 0;\n"
        "        trash = a;\n"
        "        trash = 1;\n"
        "        var keep: int = a;\n"
        "        return keep;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "func nestedDeadStoreManual(a: int): int {\n"
        "    {\n"
        "        var keep: int = a;\n"
        "        return keep;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "func nestedDeadStoreConservative(a: int, b: int): int {\n"
        "    {\n"
        "        var trash: int = a + b;\n"
        "        return a;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    ParseResult ir_nested_dead_store_parse =
        parser_parse(ir_nested_dead_store_source, "test.tblo");
    if (ir_nested_dead_store_parse.error || !ir_nested_dead_store_parse.program) {
        tests_failed++;
        printf("  FAIL: recursive lightweight IR nested dead-store parse\n");
        parser_free_result(&ir_nested_dead_store_parse);
        return;
    }

    TypeCheckResult ir_nested_dead_store_tc = typecheck(ir_nested_dead_store_parse.program);
    if (ir_nested_dead_store_tc.error) {
        tests_failed++;
        printf("  FAIL: recursive lightweight IR nested dead-store typecheck\n");
        symbol_table_free(ir_nested_dead_store_tc.globals);
        error_free(ir_nested_dead_store_tc.error);
        parser_free_result(&ir_nested_dead_store_parse);
        return;
    }

    CompileResult ir_nested_dead_store_compile = compile(ir_nested_dead_store_parse.program);
    if (ir_nested_dead_store_compile.error || !ir_nested_dead_store_compile.function) {
        tests_failed++;
        printf("  FAIL: recursive lightweight IR nested dead-store compile\n");
    } else {
        const Chunk* nested_dead_store_chunk =
            find_compiled_function_chunk(&ir_nested_dead_store_compile, "nestedDeadStore");
        const Chunk* nested_dead_store_manual_chunk =
            find_compiled_function_chunk(&ir_nested_dead_store_compile, "nestedDeadStoreManual");
        const Chunk* nested_dead_store_conservative_chunk =
            find_compiled_function_chunk(&ir_nested_dead_store_compile, "nestedDeadStoreConservative");

        int nested_dead_store_store_ops =
            chunk_count_opcode(nested_dead_store_chunk, OP_STORE_LOCAL);
        int nested_dead_store_manual_store_ops =
            chunk_count_opcode(nested_dead_store_manual_chunk, OP_STORE_LOCAL);
        int nested_dead_store_conservative_store_ops =
            chunk_count_opcode(nested_dead_store_conservative_chunk, OP_STORE_LOCAL);

        bool eliminated_nested_dead_stores =
            nested_dead_store_chunk && nested_dead_store_manual_chunk &&
            (nested_dead_store_store_ops <= nested_dead_store_manual_store_ops + 1) &&
            (nested_dead_store_chunk->code_count <= nested_dead_store_manual_chunk->code_count + 4);
        bool retained_nested_nontrivial_store =
            nested_dead_store_conservative_chunk && nested_dead_store_manual_chunk &&
            (nested_dead_store_conservative_store_ops >= nested_dead_store_manual_store_ops + 1) &&
            (nested_dead_store_conservative_chunk->code_count >=
             nested_dead_store_manual_chunk->code_count + 2);

        if (eliminated_nested_dead_stores && retained_nested_nontrivial_store) {
            tests_passed++;
            printf("  PASS: recursive lightweight IR eliminates nested dead local stores conservatively\n");
        } else {
            tests_failed++;
            int nested_dead_store_count =
                nested_dead_store_chunk ? nested_dead_store_chunk->code_count : -1;
            int nested_dead_store_manual_count =
                nested_dead_store_manual_chunk ? nested_dead_store_manual_chunk->code_count : -1;
            int nested_dead_store_conservative_count =
                nested_dead_store_conservative_chunk
                    ? nested_dead_store_conservative_chunk->code_count
                    : -1;
            printf("  FAIL: recursive lightweight IR eliminates nested dead local stores conservatively (dead=%d manual=%d conservative=%d storeDead=%d storeManual=%d storeConservative=%d)\n",
                   nested_dead_store_count,
                   nested_dead_store_manual_count,
                   nested_dead_store_conservative_count,
                   nested_dead_store_store_ops,
                   nested_dead_store_manual_store_ops,
                   nested_dead_store_conservative_store_ops);
        }
    }

    for (int i = 0; i < ir_nested_dead_store_compile.function_count; i++) {
        if (ir_nested_dead_store_compile.functions &&
            ir_nested_dead_store_compile.functions[i]) {
            obj_function_free(ir_nested_dead_store_compile.functions[i]);
        }
    }
    if (ir_nested_dead_store_compile.functions) {
        free(ir_nested_dead_store_compile.functions);
    }
    if (ir_nested_dead_store_compile.function) {
        obj_function_free(ir_nested_dead_store_compile.function);
    }
    symbol_table_free(ir_nested_dead_store_compile.globals);
    error_free(ir_nested_dead_store_compile.error);

    symbol_table_free(ir_nested_dead_store_tc.globals);
    error_free(ir_nested_dead_store_tc.error);
    parser_free_result(&ir_nested_dead_store_parse);

    const char* ir_nested_match_prune_source =
        "func nestedConstMatch(): void {\n"
        "    {\n"
        "        match (1 + 2) {\n"
        "            1: {\n"
        "                println(\"a\");\n"
        "            }\n"
        "            3: {\n"
        "                println(\"b\");\n"
        "            }\n"
        "            else: {\n"
        "                println(\"c\");\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func nestedDynMatch(v: int): void {\n"
        "    {\n"
        "        match (v) {\n"
        "            1: {\n"
        "                println(\"a\");\n"
        "            }\n"
        "            3: {\n"
        "                println(\"b\");\n"
        "            }\n"
        "            else: {\n"
        "                println(\"c\");\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n";
    ParseResult ir_nested_match_prune_parse =
        parser_parse(ir_nested_match_prune_source, "test.tblo");
    if (ir_nested_match_prune_parse.error || !ir_nested_match_prune_parse.program) {
        tests_failed++;
        printf("  FAIL: recursive lightweight IR nested-match parse\n");
        parser_free_result(&ir_nested_match_prune_parse);
        return;
    }

    TypeCheckResult ir_nested_match_prune_tc = typecheck(ir_nested_match_prune_parse.program);
    if (ir_nested_match_prune_tc.error) {
        tests_failed++;
        printf("  FAIL: recursive lightweight IR nested-match typecheck\n");
        symbol_table_free(ir_nested_match_prune_tc.globals);
        error_free(ir_nested_match_prune_tc.error);
        parser_free_result(&ir_nested_match_prune_parse);
        return;
    }

    CompileResult ir_nested_match_prune_compile = compile(ir_nested_match_prune_parse.program);
    if (ir_nested_match_prune_compile.error || !ir_nested_match_prune_compile.function) {
        tests_failed++;
        printf("  FAIL: recursive lightweight IR nested-match compile\n");
    } else {
        const Chunk* nested_const_match_chunk =
            find_compiled_function_chunk(&ir_nested_match_prune_compile, "nestedConstMatch");
        const Chunk* nested_dyn_match_chunk =
            find_compiled_function_chunk(&ir_nested_match_prune_compile, "nestedDynMatch");

        bool nested_match_pruned =
            nested_const_match_chunk && nested_dyn_match_chunk &&
            (nested_const_match_chunk->code_count + 10 <= nested_dyn_match_chunk->code_count);

        if (nested_match_pruned) {
            tests_passed++;
            printf("  PASS: recursive lightweight IR prunes nested constant-match scaffolding\n");
        } else {
            tests_failed++;
            int nested_const_count =
                nested_const_match_chunk ? nested_const_match_chunk->code_count : -1;
            int nested_dyn_count =
                nested_dyn_match_chunk ? nested_dyn_match_chunk->code_count : -1;
            printf("  FAIL: recursive lightweight IR prunes nested constant-match scaffolding (const=%d dyn=%d)\n",
                   nested_const_count,
                   nested_dyn_count);
        }
    }

    for (int i = 0; i < ir_nested_match_prune_compile.function_count; i++) {
        if (ir_nested_match_prune_compile.functions &&
            ir_nested_match_prune_compile.functions[i]) {
            obj_function_free(ir_nested_match_prune_compile.functions[i]);
        }
    }
    if (ir_nested_match_prune_compile.functions) {
        free(ir_nested_match_prune_compile.functions);
    }
    if (ir_nested_match_prune_compile.function) {
        obj_function_free(ir_nested_match_prune_compile.function);
    }
    symbol_table_free(ir_nested_match_prune_compile.globals);
    error_free(ir_nested_match_prune_compile.error);

    symbol_table_free(ir_nested_match_prune_tc.globals);
    error_free(ir_nested_match_prune_tc.error);
    parser_free_result(&ir_nested_match_prune_parse);

    const char* ir_dce_source =
        "func main(): void {\n"
        "    if (false) {\n"
        "        var z: int = 8;\n"
        "        var q: int = z / 2;\n"
        "        println(str(q));\n"
        "    } else {\n"
        "        println(\"live\");\n"
        "    }\n"
        "    while (false) {\n"
        "        println(\"dead-loop\");\n"
        "    }\n"
        "    foreach (i in 7..3) {\n"
        "        println(str(i));\n"
        "    }\n"
        "}\n";
    ParseResult ir_dce_parse = parser_parse(ir_dce_source, "test.tblo");
    if (ir_dce_parse.error || !ir_dce_parse.program) {
        tests_failed++;
        printf("  FAIL: lightweight IR dead-code elimination parse\n");
        parser_free_result(&ir_dce_parse);
        return;
    }

    TypeCheckResult ir_dce_tc = typecheck(ir_dce_parse.program);
    if (ir_dce_tc.error) {
        tests_failed++;
        printf("  FAIL: lightweight IR dead-code elimination typecheck\n");
        symbol_table_free(ir_dce_tc.globals);
        error_free(ir_dce_tc.error);
        parser_free_result(&ir_dce_parse);
        return;
    }

    CompileResult ir_dce_compile = compile(ir_dce_parse.program);
    if (ir_dce_compile.error || !ir_dce_compile.function) {
        tests_failed++;
        printf("  FAIL: lightweight IR dead-code elimination compile\n");
    } else {
        const Chunk* chunk = &ir_dce_compile.function->chunk;
        bool has_dead_branch_ops =
            chunk_contains_opcode(chunk, OP_DIV) ||
            chunk_contains_opcode(chunk, OP_DIV_INT) ||
            chunk_contains_opcode(chunk, OP_DIV_LOCALS_INT) ||
            chunk_contains_opcode(chunk, OP_DIV_LOCAL_CONST_INT) ||
            chunk_contains_opcode(chunk, OP_JUMP_IF_FALSE) ||
            chunk_contains_opcode(chunk, OP_JUMP);

        if (!has_dead_branch_ops) {
            tests_passed++;
            printf("  PASS: lightweight IR dead-code elimination for constant if/while/range\n");
        } else {
            tests_failed++;
            printf("  FAIL: lightweight IR dead-code elimination for constant if/while/range\n");
        }
    }

    for (int i = 0; i < ir_dce_compile.function_count; i++) {
        if (ir_dce_compile.functions && ir_dce_compile.functions[i]) {
            obj_function_free(ir_dce_compile.functions[i]);
        }
    }
    if (ir_dce_compile.functions) free(ir_dce_compile.functions);
    if (ir_dce_compile.function) obj_function_free(ir_dce_compile.function);
    symbol_table_free(ir_dce_compile.globals);
    error_free(ir_dce_compile.error);

    symbol_table_free(ir_dce_tc.globals);
    error_free(ir_dce_tc.error);
    parser_free_result(&ir_dce_parse);

    const char* ir_short_circuit_dce_source =
        "func branchConst(v: int): void {\n"
        "    if (false && (v > 0)) {\n"
        "        println(\"then\");\n"
        "    } else {\n"
        "        println(\"else\");\n"
        "    }\n"
        "}\n"
        "func branchDynamic(flag: bool, v: int): void {\n"
        "    if (flag && (v > 0)) {\n"
        "        println(\"then\");\n"
        "    } else {\n"
        "        println(\"else\");\n"
        "    }\n"
        "}\n";
    ParseResult ir_short_circuit_dce_parse =
        parser_parse(ir_short_circuit_dce_source, "test.tblo");
    if (ir_short_circuit_dce_parse.error || !ir_short_circuit_dce_parse.program) {
        tests_failed++;
        printf("  FAIL: lightweight IR short-circuit condition pruning parse\n");
        parser_free_result(&ir_short_circuit_dce_parse);
        return;
    }

    TypeCheckResult ir_short_circuit_dce_tc = typecheck(ir_short_circuit_dce_parse.program);
    if (ir_short_circuit_dce_tc.error) {
        tests_failed++;
        printf("  FAIL: lightweight IR short-circuit condition pruning typecheck\n");
        symbol_table_free(ir_short_circuit_dce_tc.globals);
        error_free(ir_short_circuit_dce_tc.error);
        parser_free_result(&ir_short_circuit_dce_parse);
        return;
    }

    CompileResult ir_short_circuit_dce_compile = compile(ir_short_circuit_dce_parse.program);
    if (ir_short_circuit_dce_compile.error || !ir_short_circuit_dce_compile.function) {
        tests_failed++;
        printf("  FAIL: lightweight IR short-circuit condition pruning compile\n");
    } else {
        const Chunk* branch_const_chunk =
            find_compiled_function_chunk(&ir_short_circuit_dce_compile, "branchConst");
        const Chunk* branch_dynamic_chunk =
            find_compiled_function_chunk(&ir_short_circuit_dce_compile, "branchDynamic");

        bool pruned_short_circuit_condition =
            branch_const_chunk && branch_dynamic_chunk &&
            (branch_const_chunk->code_count + 6 <= branch_dynamic_chunk->code_count);

        if (pruned_short_circuit_condition) {
            tests_passed++;
            printf("  PASS: lightweight IR prunes short-circuit constant conditions\n");
        } else {
            tests_failed++;
            printf("  FAIL: lightweight IR prunes short-circuit constant conditions\n");
        }
    }

    for (int i = 0; i < ir_short_circuit_dce_compile.function_count; i++) {
        if (ir_short_circuit_dce_compile.functions &&
            ir_short_circuit_dce_compile.functions[i]) {
            obj_function_free(ir_short_circuit_dce_compile.functions[i]);
        }
    }
    if (ir_short_circuit_dce_compile.functions) free(ir_short_circuit_dce_compile.functions);
    if (ir_short_circuit_dce_compile.function) obj_function_free(ir_short_circuit_dce_compile.function);
    symbol_table_free(ir_short_circuit_dce_compile.globals);
    error_free(ir_short_circuit_dce_compile.error);

    symbol_table_free(ir_short_circuit_dce_tc.globals);
    error_free(ir_short_circuit_dce_tc.error);
    parser_free_result(&ir_short_circuit_dce_parse);

    const char* ir_nonblock_if_call_return_source =
        "func localBool(): bool { return true; }\n"
        "func setup(): int {\n"
        "    if (!localBool()) return -1;\n"
        "    return 1;\n"
        "}\n"
        "func main(): void {}\n";
    ParseResult ir_nonblock_if_call_return_parse =
        parser_parse(ir_nonblock_if_call_return_source, "test.tblo");
    if (ir_nonblock_if_call_return_parse.error || !ir_nonblock_if_call_return_parse.program) {
        tests_failed++;
        printf("  FAIL: lightweight IR non-block if-call-return regression parse\n");
        parser_free_result(&ir_nonblock_if_call_return_parse);
        return;
    }

    TypeCheckResult ir_nonblock_if_call_return_tc =
        typecheck(ir_nonblock_if_call_return_parse.program);
    if (ir_nonblock_if_call_return_tc.error) {
        tests_failed++;
        printf("  FAIL: lightweight IR non-block if-call-return regression typecheck\n");
        symbol_table_free(ir_nonblock_if_call_return_tc.globals);
        error_free(ir_nonblock_if_call_return_tc.error);
        parser_free_result(&ir_nonblock_if_call_return_parse);
        return;
    }

    CompileResult ir_nonblock_if_call_return_compile =
        compile(ir_nonblock_if_call_return_parse.program);
    if (ir_nonblock_if_call_return_compile.error ||
        !find_compiled_function_object(&ir_nonblock_if_call_return_compile, "setup")) {
        tests_failed++;
        printf("  FAIL: lightweight IR non-block if-call-return regression compile\n");
    } else {
        tests_passed++;
        printf("  PASS: lightweight IR handles non-block if-call-return branches\n");
    }

    for (int i = 0; i < ir_nonblock_if_call_return_compile.function_count; i++) {
        if (ir_nonblock_if_call_return_compile.functions &&
            ir_nonblock_if_call_return_compile.functions[i]) {
            obj_function_free(ir_nonblock_if_call_return_compile.functions[i]);
        }
    }
    if (ir_nonblock_if_call_return_compile.functions) {
        free(ir_nonblock_if_call_return_compile.functions);
    }
    if (ir_nonblock_if_call_return_compile.function) {
        obj_function_free(ir_nonblock_if_call_return_compile.function);
    }
    symbol_table_free(ir_nonblock_if_call_return_compile.globals);
    error_free(ir_nonblock_if_call_return_compile.error);

    symbol_table_free(ir_nonblock_if_call_return_tc.globals);
    error_free(ir_nonblock_if_call_return_tc.error);
    parser_free_result(&ir_nonblock_if_call_return_parse);

    const char* ir_foreach_empty_iterable_source =
        "func emptyArray(): void {\n"
        "    foreach (x in []) {\n"
        "        println(str(x));\n"
        "    }\n"
        "}\n"
        "func dynamicArray(items: array<int>): void {\n"
        "    foreach (x in items) {\n"
        "        println(str(x));\n"
        "    }\n"
        "}\n"
        "func emptyBytes(): void {\n"
        "    foreach (b in ([] as bytes)) {\n"
        "        println(str(b));\n"
        "    }\n"
        "}\n"
        "func dynamicBytes(data: bytes): void {\n"
        "    foreach (b in data) {\n"
        "        println(str(b));\n"
        "    }\n"
        "}\n";
    ParseResult ir_foreach_empty_iterable_parse =
        parser_parse(ir_foreach_empty_iterable_source, "test.tblo");
    if (ir_foreach_empty_iterable_parse.error || !ir_foreach_empty_iterable_parse.program) {
        tests_failed++;
        printf("  FAIL: lightweight IR empty-foreach-iterable pruning parse\n");
        parser_free_result(&ir_foreach_empty_iterable_parse);
        return;
    }

    TypeCheckResult ir_foreach_empty_iterable_tc = typecheck(ir_foreach_empty_iterable_parse.program);
    if (ir_foreach_empty_iterable_tc.error) {
        tests_failed++;
        printf("  FAIL: lightweight IR empty-foreach-iterable pruning typecheck\n");
        symbol_table_free(ir_foreach_empty_iterable_tc.globals);
        error_free(ir_foreach_empty_iterable_tc.error);
        parser_free_result(&ir_foreach_empty_iterable_parse);
        return;
    }

    CompileResult ir_foreach_empty_iterable_compile = compile(ir_foreach_empty_iterable_parse.program);
    if (ir_foreach_empty_iterable_compile.error || !ir_foreach_empty_iterable_compile.function) {
        tests_failed++;
        printf("  FAIL: lightweight IR empty-foreach-iterable pruning compile\n");
    } else {
        const Chunk* empty_array_chunk =
            find_compiled_function_chunk(&ir_foreach_empty_iterable_compile, "emptyArray");
        const Chunk* dynamic_array_chunk =
            find_compiled_function_chunk(&ir_foreach_empty_iterable_compile, "dynamicArray");
        const Chunk* empty_bytes_chunk =
            find_compiled_function_chunk(&ir_foreach_empty_iterable_compile, "emptyBytes");
        const Chunk* dynamic_bytes_chunk =
            find_compiled_function_chunk(&ir_foreach_empty_iterable_compile, "dynamicBytes");

        bool pruned_empty_foreach_iterables =
            empty_array_chunk && dynamic_array_chunk &&
            empty_bytes_chunk && dynamic_bytes_chunk &&
            (empty_array_chunk->code_count + 8 <= dynamic_array_chunk->code_count) &&
            (empty_bytes_chunk->code_count + 8 <= dynamic_bytes_chunk->code_count);

        if (pruned_empty_foreach_iterables) {
            tests_passed++;
            printf("  PASS: lightweight IR prunes foreach over compile-time empty iterables\n");
        } else {
            tests_failed++;
            printf("  FAIL: lightweight IR prunes foreach over compile-time empty iterables\n");
        }
    }

    for (int i = 0; i < ir_foreach_empty_iterable_compile.function_count; i++) {
        if (ir_foreach_empty_iterable_compile.functions &&
            ir_foreach_empty_iterable_compile.functions[i]) {
            obj_function_free(ir_foreach_empty_iterable_compile.functions[i]);
        }
    }
    if (ir_foreach_empty_iterable_compile.functions) {
        free(ir_foreach_empty_iterable_compile.functions);
    }
    if (ir_foreach_empty_iterable_compile.function) {
        obj_function_free(ir_foreach_empty_iterable_compile.function);
    }
    symbol_table_free(ir_foreach_empty_iterable_compile.globals);
    error_free(ir_foreach_empty_iterable_compile.error);

    symbol_table_free(ir_foreach_empty_iterable_tc.globals);
    error_free(ir_foreach_empty_iterable_tc.error);
    parser_free_result(&ir_foreach_empty_iterable_parse);

    const char* ir_terminator_dce_source =
        "func main(): void {\n"
        "    return;\n"
        "    var dead: array<int> = [1, 2, 3];\n"
        "    println(str(len(dead)));\n"
        "}\n";
    ParseResult ir_terminator_dce_parse = parser_parse(ir_terminator_dce_source, "test.tblo");
    if (ir_terminator_dce_parse.error || !ir_terminator_dce_parse.program) {
        tests_failed++;
        printf("  FAIL: lightweight IR terminator-pruning parse\n");
        parser_free_result(&ir_terminator_dce_parse);
        return;
    }

    TypeCheckResult ir_terminator_dce_tc = typecheck(ir_terminator_dce_parse.program);
    if (ir_terminator_dce_tc.error) {
        tests_failed++;
        printf("  FAIL: lightweight IR terminator-pruning typecheck\n");
        symbol_table_free(ir_terminator_dce_tc.globals);
        error_free(ir_terminator_dce_tc.error);
        parser_free_result(&ir_terminator_dce_parse);
        return;
    }

    CompileResult ir_terminator_dce_compile = compile(ir_terminator_dce_parse.program);
    if (ir_terminator_dce_compile.error || !ir_terminator_dce_compile.function) {
        tests_failed++;
        printf("  FAIL: lightweight IR terminator-pruning compile\n");
    } else {
        const Chunk* main_chunk = find_compiled_function_chunk(&ir_terminator_dce_compile, "main");
        bool has_dead_after_return_ops =
            (main_chunk && (
                chunk_contains_opcode(main_chunk, OP_ARRAY_NEW) ||
                chunk_contains_opcode(main_chunk, OP_ARRAY_PUSH)
            ));

        if (main_chunk && !has_dead_after_return_ops) {
            tests_passed++;
            printf("  PASS: lightweight IR prunes statements after guaranteed terminators\n");
        } else {
            tests_failed++;
            printf("  FAIL: lightweight IR prunes statements after guaranteed terminators\n");
        }
    }

    for (int i = 0; i < ir_terminator_dce_compile.function_count; i++) {
        if (ir_terminator_dce_compile.functions && ir_terminator_dce_compile.functions[i]) {
            obj_function_free(ir_terminator_dce_compile.functions[i]);
        }
    }
    if (ir_terminator_dce_compile.functions) free(ir_terminator_dce_compile.functions);
    if (ir_terminator_dce_compile.function) obj_function_free(ir_terminator_dce_compile.function);
    symbol_table_free(ir_terminator_dce_compile.globals);
    error_free(ir_terminator_dce_compile.error);

    symbol_table_free(ir_terminator_dce_tc.globals);
    error_free(ir_terminator_dce_tc.error);
    parser_free_result(&ir_terminator_dce_parse);

    const char* ir_if_jump_prune_source =
        "func branch(v: bool): void {\n"
        "    if (v) {\n"
        "        return;\n"
        "    } else {\n"
        "        println(\"x\");\n"
        "    }\n"
        "}\n";
    ParseResult ir_if_jump_prune_parse = parser_parse(ir_if_jump_prune_source, "test.tblo");
    if (ir_if_jump_prune_parse.error || !ir_if_jump_prune_parse.program) {
        tests_failed++;
        printf("  FAIL: lightweight IR if-jump-pruning parse\n");
        parser_free_result(&ir_if_jump_prune_parse);
        return;
    }

    TypeCheckResult ir_if_jump_prune_tc = typecheck(ir_if_jump_prune_parse.program);
    if (ir_if_jump_prune_tc.error) {
        tests_failed++;
        printf("  FAIL: lightweight IR if-jump-pruning typecheck\n");
        symbol_table_free(ir_if_jump_prune_tc.globals);
        error_free(ir_if_jump_prune_tc.error);
        parser_free_result(&ir_if_jump_prune_parse);
        return;
    }

    CompileResult ir_if_jump_prune_compile = compile(ir_if_jump_prune_parse.program);
    if (ir_if_jump_prune_compile.error || !ir_if_jump_prune_compile.function) {
        tests_failed++;
        printf("  FAIL: lightweight IR if-jump-pruning compile\n");
    } else {
        const Chunk* branch_chunk = find_compiled_function_chunk(&ir_if_jump_prune_compile, "branch");
        bool has_conditional_jump =
            chunk_contains_opcode(branch_chunk, OP_JUMP_IF_FALSE) ||
            chunk_contains_opcode(branch_chunk, OP_JUMP_IF_FALSE_POP);

        const char* ir_if_jump_control_source =
            "func branch(v: bool): void {\n"
            "    if (v) {\n"
            "        println(\"y\");\n"
            "    } else {\n"
            "        println(\"x\");\n"
            "    }\n"
            "}\n";
        ParseResult ir_if_jump_control_parse = parser_parse(ir_if_jump_control_source, "test.tblo");
        bool control_ok = false;
        const Chunk* control_chunk = NULL;
        CompileResult ir_if_jump_control_compile = {0};
        TypeCheckResult ir_if_jump_control_tc = {0};

        if (!ir_if_jump_control_parse.error && ir_if_jump_control_parse.program) {
            ir_if_jump_control_tc = typecheck(ir_if_jump_control_parse.program);
            if (!ir_if_jump_control_tc.error) {
                ir_if_jump_control_compile = compile(ir_if_jump_control_parse.program);
                if (!ir_if_jump_control_compile.error && ir_if_jump_control_compile.function) {
                    control_chunk = find_compiled_function_chunk(&ir_if_jump_control_compile, "branch");
                    control_ok = (control_chunk != NULL);
                }
            }
        }

        bool shorter_when_then_terminates =
            branch_chunk && control_chunk &&
            (branch_chunk->code_count + 3 <= control_chunk->code_count);

        if (branch_chunk && has_conditional_jump && control_ok && shorter_when_then_terminates) {
            tests_passed++;
            printf("  PASS: lightweight IR removes redundant branch jumps after terminating then-arm\n");
        } else {
            tests_failed++;
            printf("  FAIL: lightweight IR removes redundant branch jumps after terminating then-arm\n");
        }

        for (int i = 0; i < ir_if_jump_control_compile.function_count; i++) {
            if (ir_if_jump_control_compile.functions && ir_if_jump_control_compile.functions[i]) {
                obj_function_free(ir_if_jump_control_compile.functions[i]);
            }
        }
        if (ir_if_jump_control_compile.functions) free(ir_if_jump_control_compile.functions);
        if (ir_if_jump_control_compile.function) obj_function_free(ir_if_jump_control_compile.function);
        symbol_table_free(ir_if_jump_control_compile.globals);
        error_free(ir_if_jump_control_compile.error);

        symbol_table_free(ir_if_jump_control_tc.globals);
        error_free(ir_if_jump_control_tc.error);
        parser_free_result(&ir_if_jump_control_parse);
    }

    for (int i = 0; i < ir_if_jump_prune_compile.function_count; i++) {
        if (ir_if_jump_prune_compile.functions && ir_if_jump_prune_compile.functions[i]) {
            obj_function_free(ir_if_jump_prune_compile.functions[i]);
        }
    }
    if (ir_if_jump_prune_compile.functions) free(ir_if_jump_prune_compile.functions);
    if (ir_if_jump_prune_compile.function) obj_function_free(ir_if_jump_prune_compile.function);
    symbol_table_free(ir_if_jump_prune_compile.globals);
    error_free(ir_if_jump_prune_compile.error);

    symbol_table_free(ir_if_jump_prune_tc.globals);
    error_free(ir_if_jump_prune_tc.error);
    parser_free_result(&ir_if_jump_prune_parse);

    const char* ir_match_jump_prune_source =
        "func route(v: int): void {\n"
        "    match (v) {\n"
        "        1: {\n"
        "            return;\n"
        "        }\n"
        "        else: {\n"
        "            println(\"x\");\n"
        "        }\n"
        "    }\n"
        "}\n";
    ParseResult ir_match_jump_prune_parse = parser_parse(ir_match_jump_prune_source, "test.tblo");
    if (ir_match_jump_prune_parse.error || !ir_match_jump_prune_parse.program) {
        tests_failed++;
        printf("  FAIL: lightweight IR match-jump-pruning parse\n");
        parser_free_result(&ir_match_jump_prune_parse);
        return;
    }

    TypeCheckResult ir_match_jump_prune_tc = typecheck(ir_match_jump_prune_parse.program);
    if (ir_match_jump_prune_tc.error) {
        tests_failed++;
        printf("  FAIL: lightweight IR match-jump-pruning typecheck\n");
        symbol_table_free(ir_match_jump_prune_tc.globals);
        error_free(ir_match_jump_prune_tc.error);
        parser_free_result(&ir_match_jump_prune_parse);
        return;
    }

    CompileResult ir_match_jump_prune_compile = compile(ir_match_jump_prune_parse.program);
    if (ir_match_jump_prune_compile.error || !ir_match_jump_prune_compile.function) {
        tests_failed++;
        printf("  FAIL: lightweight IR match-jump-pruning compile\n");
    } else {
        const Chunk* route_chunk = find_compiled_function_chunk(&ir_match_jump_prune_compile, "route");

        const char* ir_match_jump_control_source =
            "func route(v: int): void {\n"
            "    match (v) {\n"
            "        1: {\n"
            "            println(\"y\");\n"
            "        }\n"
            "        else: {\n"
            "            println(\"x\");\n"
            "        }\n"
            "    }\n"
            "}\n";
        ParseResult ir_match_jump_control_parse = parser_parse(ir_match_jump_control_source, "test.tblo");
        bool control_ok = false;
        const Chunk* control_chunk = NULL;
        CompileResult ir_match_jump_control_compile = {0};
        TypeCheckResult ir_match_jump_control_tc = {0};

        if (!ir_match_jump_control_parse.error && ir_match_jump_control_parse.program) {
            ir_match_jump_control_tc = typecheck(ir_match_jump_control_parse.program);
            if (!ir_match_jump_control_tc.error) {
                ir_match_jump_control_compile = compile(ir_match_jump_control_parse.program);
                if (!ir_match_jump_control_compile.error && ir_match_jump_control_compile.function) {
                    control_chunk = find_compiled_function_chunk(&ir_match_jump_control_compile, "route");
                    control_ok = (control_chunk != NULL);
                }
            }
        }

        bool shorter_when_arm_terminates =
            route_chunk && control_chunk &&
            (route_chunk->code_count + 3 <= control_chunk->code_count);

        if (route_chunk && control_ok && shorter_when_arm_terminates) {
            tests_passed++;
            printf("  PASS: lightweight IR removes redundant match-arm jumps after terminating arms\n");
        } else {
            tests_failed++;
            printf("  FAIL: lightweight IR removes redundant match-arm jumps after terminating arms\n");
        }

        for (int i = 0; i < ir_match_jump_control_compile.function_count; i++) {
            if (ir_match_jump_control_compile.functions && ir_match_jump_control_compile.functions[i]) {
                obj_function_free(ir_match_jump_control_compile.functions[i]);
            }
        }
        if (ir_match_jump_control_compile.functions) free(ir_match_jump_control_compile.functions);
        if (ir_match_jump_control_compile.function) obj_function_free(ir_match_jump_control_compile.function);
        symbol_table_free(ir_match_jump_control_compile.globals);
        error_free(ir_match_jump_control_compile.error);

        symbol_table_free(ir_match_jump_control_tc.globals);
        error_free(ir_match_jump_control_tc.error);
        parser_free_result(&ir_match_jump_control_parse);
    }

    for (int i = 0; i < ir_match_jump_prune_compile.function_count; i++) {
        if (ir_match_jump_prune_compile.functions && ir_match_jump_prune_compile.functions[i]) {
            obj_function_free(ir_match_jump_prune_compile.functions[i]);
        }
    }
    if (ir_match_jump_prune_compile.functions) free(ir_match_jump_prune_compile.functions);
    if (ir_match_jump_prune_compile.function) obj_function_free(ir_match_jump_prune_compile.function);
    symbol_table_free(ir_match_jump_prune_compile.globals);
    error_free(ir_match_jump_prune_compile.error);

    symbol_table_free(ir_match_jump_prune_tc.globals);
    error_free(ir_match_jump_prune_tc.error);
    parser_free_result(&ir_match_jump_prune_parse);

    const char* ir_match_const_subject_source =
        "func route(): void {\n"
        "    match (1 + 2) {\n"
        "        1: {\n"
        "            println(\"a\");\n"
        "        }\n"
        "        3: {\n"
        "            println(\"b\");\n"
        "        }\n"
        "        else: {\n"
        "            println(\"c\");\n"
        "        }\n"
        "    }\n"
        "}\n";
    ParseResult ir_match_const_subject_parse = parser_parse(ir_match_const_subject_source, "test.tblo");
    if (ir_match_const_subject_parse.error || !ir_match_const_subject_parse.program) {
        tests_failed++;
        printf("  FAIL: lightweight IR constant-match-subject pruning parse\n");
        parser_free_result(&ir_match_const_subject_parse);
        return;
    }

    TypeCheckResult ir_match_const_subject_tc = typecheck(ir_match_const_subject_parse.program);
    if (ir_match_const_subject_tc.error) {
        tests_failed++;
        printf("  FAIL: lightweight IR constant-match-subject pruning typecheck\n");
        symbol_table_free(ir_match_const_subject_tc.globals);
        error_free(ir_match_const_subject_tc.error);
        parser_free_result(&ir_match_const_subject_parse);
        return;
    }

    CompileResult ir_match_const_subject_compile = compile(ir_match_const_subject_parse.program);
    if (ir_match_const_subject_compile.error || !ir_match_const_subject_compile.function) {
        tests_failed++;
        printf("  FAIL: lightweight IR constant-match-subject pruning compile\n");
    } else {
        const Chunk* const_route_chunk = find_compiled_function_chunk(&ir_match_const_subject_compile, "route");

        const char* ir_match_const_subject_control_source =
            "func route(v: int): void {\n"
            "    match (v) {\n"
            "        1: {\n"
            "            println(\"a\");\n"
            "        }\n"
            "        3: {\n"
            "            println(\"b\");\n"
            "        }\n"
            "        else: {\n"
            "            println(\"c\");\n"
            "        }\n"
            "    }\n"
            "}\n";
        ParseResult ir_match_const_subject_control_parse =
            parser_parse(ir_match_const_subject_control_source, "test.tblo");
        bool control_ok = false;
        const Chunk* control_chunk = NULL;
        CompileResult ir_match_const_subject_control_compile = {0};
        TypeCheckResult ir_match_const_subject_control_tc = {0};

        if (!ir_match_const_subject_control_parse.error && ir_match_const_subject_control_parse.program) {
            ir_match_const_subject_control_tc = typecheck(ir_match_const_subject_control_parse.program);
            if (!ir_match_const_subject_control_tc.error) {
                ir_match_const_subject_control_compile = compile(ir_match_const_subject_control_parse.program);
                if (!ir_match_const_subject_control_compile.error &&
                    ir_match_const_subject_control_compile.function) {
                    control_chunk =
                        find_compiled_function_chunk(&ir_match_const_subject_control_compile, "route");
                    control_ok = (control_chunk != NULL);
                }
            }
        }

        bool pruned_match_scaffolding =
            const_route_chunk && control_chunk &&
            (const_route_chunk->code_count + 10 <= control_chunk->code_count);

        if (const_route_chunk && control_ok && pruned_match_scaffolding) {
            tests_passed++;
            printf("  PASS: lightweight IR prunes match scaffolding for constant subjects\n");
        } else {
            tests_failed++;
            printf("  FAIL: lightweight IR prunes match scaffolding for constant subjects\n");
        }

        for (int i = 0; i < ir_match_const_subject_control_compile.function_count; i++) {
            if (ir_match_const_subject_control_compile.functions &&
                ir_match_const_subject_control_compile.functions[i]) {
                obj_function_free(ir_match_const_subject_control_compile.functions[i]);
            }
        }
        if (ir_match_const_subject_control_compile.functions) {
            free(ir_match_const_subject_control_compile.functions);
        }
        if (ir_match_const_subject_control_compile.function) {
            obj_function_free(ir_match_const_subject_control_compile.function);
        }
        symbol_table_free(ir_match_const_subject_control_compile.globals);
        error_free(ir_match_const_subject_control_compile.error);

        symbol_table_free(ir_match_const_subject_control_tc.globals);
        error_free(ir_match_const_subject_control_tc.error);
        parser_free_result(&ir_match_const_subject_control_parse);
    }

    for (int i = 0; i < ir_match_const_subject_compile.function_count; i++) {
        if (ir_match_const_subject_compile.functions && ir_match_const_subject_compile.functions[i]) {
            obj_function_free(ir_match_const_subject_compile.functions[i]);
        }
    }
    if (ir_match_const_subject_compile.functions) free(ir_match_const_subject_compile.functions);
    if (ir_match_const_subject_compile.function) obj_function_free(ir_match_const_subject_compile.function);
    symbol_table_free(ir_match_const_subject_compile.globals);
    error_free(ir_match_const_subject_compile.error);

    symbol_table_free(ir_match_const_subject_tc.globals);
    error_free(ir_match_const_subject_tc.error);
    parser_free_result(&ir_match_const_subject_parse);

    const char* ir_match_enum_ctor_subject_source =
        "enum Result[T, E] {\n"
        "    Ok(T),\n"
        "    Err(E)\n"
        "}\n"
        "func routeConst(): void {\n"
        "    match (Result.Ok<int, string>(7)) {\n"
        "        Result.Err<int, string>(\"boom\"): {\n"
        "            println(\"bad\");\n"
        "        }\n"
        "        Result.Ok<int, string>(7): {\n"
        "            println(\"good\");\n"
        "        }\n"
        "        else: {\n"
        "            println(\"other\");\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func routeDyn(v: int): void {\n"
        "    var r: Result[int, string] = Result.Ok<int, string>(v);\n"
        "    match (r) {\n"
        "        Result.Err<int, string>(\"boom\"): {\n"
        "            println(\"bad\");\n"
        "        }\n"
        "        Result.Ok<int, string>(7): {\n"
        "            println(\"good\");\n"
        "        }\n"
        "        else: {\n"
        "            println(\"other\");\n"
        "        }\n"
        "    }\n"
        "}\n";
    ParseResult ir_match_enum_ctor_subject_parse =
        parser_parse(ir_match_enum_ctor_subject_source, "test.tblo");
    if (ir_match_enum_ctor_subject_parse.error || !ir_match_enum_ctor_subject_parse.program) {
        tests_failed++;
        printf("  FAIL: lightweight IR enum-constructor match pruning parse\n");
        parser_free_result(&ir_match_enum_ctor_subject_parse);
        return;
    }

    TypeCheckResult ir_match_enum_ctor_subject_tc = typecheck(ir_match_enum_ctor_subject_parse.program);
    if (ir_match_enum_ctor_subject_tc.error) {
        tests_failed++;
        printf("  FAIL: lightweight IR enum-constructor match pruning typecheck\n");
        symbol_table_free(ir_match_enum_ctor_subject_tc.globals);
        error_free(ir_match_enum_ctor_subject_tc.error);
        parser_free_result(&ir_match_enum_ctor_subject_parse);
        return;
    }

    CompileResult ir_match_enum_ctor_subject_compile = compile(ir_match_enum_ctor_subject_parse.program);
    if (ir_match_enum_ctor_subject_compile.error || !ir_match_enum_ctor_subject_compile.function) {
        tests_failed++;
        printf("  FAIL: lightweight IR enum-constructor match pruning compile\n");
    } else {
        const Chunk* route_const_chunk =
            find_compiled_function_chunk(&ir_match_enum_ctor_subject_compile, "routeConst");
        const Chunk* route_dyn_chunk =
            find_compiled_function_chunk(&ir_match_enum_ctor_subject_compile, "routeDyn");

        bool pruned_enum_ctor_match =
            route_const_chunk && route_dyn_chunk &&
            (route_const_chunk->code_count + 10 <= route_dyn_chunk->code_count);

        if (pruned_enum_ctor_match) {
            tests_passed++;
            printf("  PASS: lightweight IR prunes enum-constructor match arms with constant payload patterns\n");
        } else {
            tests_failed++;
            printf("  FAIL: lightweight IR prunes enum-constructor match arms with constant payload patterns\n");
        }
    }

    for (int i = 0; i < ir_match_enum_ctor_subject_compile.function_count; i++) {
        if (ir_match_enum_ctor_subject_compile.functions &&
            ir_match_enum_ctor_subject_compile.functions[i]) {
            obj_function_free(ir_match_enum_ctor_subject_compile.functions[i]);
        }
    }
    if (ir_match_enum_ctor_subject_compile.functions) {
        free(ir_match_enum_ctor_subject_compile.functions);
    }
    if (ir_match_enum_ctor_subject_compile.function) {
        obj_function_free(ir_match_enum_ctor_subject_compile.function);
    }
    symbol_table_free(ir_match_enum_ctor_subject_compile.globals);
    error_free(ir_match_enum_ctor_subject_compile.error);

    symbol_table_free(ir_match_enum_ctor_subject_tc.globals);
    error_free(ir_match_enum_ctor_subject_tc.error);
    parser_free_result(&ir_match_enum_ctor_subject_parse);

    const char* ir_match_enum_binding_else_source =
        "enum Result[T, E] {\n"
        "    Ok(T),\n"
        "    Err(E)\n"
        "}\n"
        "func routeConstElse(): void {\n"
        "    match (Result.Err<int, string>(\"boom\")) {\n"
        "        Result.Ok<int, string>(v): {\n"
        "            println(v);\n"
        "        }\n"
        "        Result.Err<int, string>(\"ok\"): {\n"
        "            println(\"ok\");\n"
        "        }\n"
        "        else: {\n"
        "            println(\"fallback\");\n"
        "        }\n"
        "    }\n"
        "}\n"
        "func routeDynElse(v: string): void {\n"
        "    var r: Result[int, string] = Result.Err<int, string>(v);\n"
        "    match (r) {\n"
        "        Result.Ok<int, string>(x): {\n"
        "            println(x);\n"
        "        }\n"
        "        Result.Err<int, string>(\"ok\"): {\n"
        "            println(\"ok\");\n"
        "        }\n"
        "        else: {\n"
        "            println(\"fallback\");\n"
        "        }\n"
        "    }\n"
        "}\n";
    ParseResult ir_match_enum_binding_else_parse =
        parser_parse(ir_match_enum_binding_else_source, "test.tblo");
    if (ir_match_enum_binding_else_parse.error || !ir_match_enum_binding_else_parse.program) {
        tests_failed++;
        printf("  FAIL: lightweight IR enum payload-binding else-pruning parse\n");
        parser_free_result(&ir_match_enum_binding_else_parse);
        return;
    }

    TypeCheckResult ir_match_enum_binding_else_tc = typecheck(ir_match_enum_binding_else_parse.program);
    if (ir_match_enum_binding_else_tc.error) {
        tests_failed++;
        printf("  FAIL: lightweight IR enum payload-binding else-pruning typecheck\n");
        symbol_table_free(ir_match_enum_binding_else_tc.globals);
        error_free(ir_match_enum_binding_else_tc.error);
        parser_free_result(&ir_match_enum_binding_else_parse);
        return;
    }

    CompileResult ir_match_enum_binding_else_compile = compile(ir_match_enum_binding_else_parse.program);
    if (ir_match_enum_binding_else_compile.error || !ir_match_enum_binding_else_compile.function) {
        tests_failed++;
        printf("  FAIL: lightweight IR enum payload-binding else-pruning compile\n");
    } else {
        const Chunk* route_const_else_chunk =
            find_compiled_function_chunk(&ir_match_enum_binding_else_compile, "routeConstElse");
        const Chunk* route_dyn_else_chunk =
            find_compiled_function_chunk(&ir_match_enum_binding_else_compile, "routeDynElse");

        bool pruned_payload_binding_else =
            route_const_else_chunk && route_dyn_else_chunk &&
            (route_const_else_chunk->code_count + 10 <= route_dyn_else_chunk->code_count);

        if (pruned_payload_binding_else) {
            tests_passed++;
            printf("  PASS: lightweight IR prunes constant enum-match else via tag-incompatible payload bindings\n");
        } else {
            tests_failed++;
            printf("  FAIL: lightweight IR prunes constant enum-match else via tag-incompatible payload bindings\n");
        }
    }

    for (int i = 0; i < ir_match_enum_binding_else_compile.function_count; i++) {
        if (ir_match_enum_binding_else_compile.functions &&
            ir_match_enum_binding_else_compile.functions[i]) {
            obj_function_free(ir_match_enum_binding_else_compile.functions[i]);
        }
    }
    if (ir_match_enum_binding_else_compile.functions) {
        free(ir_match_enum_binding_else_compile.functions);
    }
    if (ir_match_enum_binding_else_compile.function) {
        obj_function_free(ir_match_enum_binding_else_compile.function);
    }
    symbol_table_free(ir_match_enum_binding_else_compile.globals);
    error_free(ir_match_enum_binding_else_compile.error);

    symbol_table_free(ir_match_enum_binding_else_tc.globals);
    error_free(ir_match_enum_binding_else_tc.error);
    parser_free_result(&ir_match_enum_binding_else_parse);

    const char* ir_while_true_return_source =
        "func loopRet(): void {\n"
        "    while (true) {\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "func loopDyn(v: bool): void {\n"
        "    while (v) {\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "func loopBreak(): void {\n"
        "    while (true) {\n"
        "        break;\n"
        "        return;\n"
        "    }\n"
        "}\n";
    ParseResult ir_while_true_return_parse = parser_parse(ir_while_true_return_source, "test.tblo");
    if (ir_while_true_return_parse.error || !ir_while_true_return_parse.program) {
        tests_failed++;
        printf("  FAIL: lightweight IR while(true)-return pruning parse\n");
        parser_free_result(&ir_while_true_return_parse);
        return;
    }

    TypeCheckResult ir_while_true_return_tc = typecheck(ir_while_true_return_parse.program);
    if (ir_while_true_return_tc.error) {
        tests_failed++;
        printf("  FAIL: lightweight IR while(true)-return pruning typecheck\n");
        symbol_table_free(ir_while_true_return_tc.globals);
        error_free(ir_while_true_return_tc.error);
        parser_free_result(&ir_while_true_return_parse);
        return;
    }

    CompileResult ir_while_true_return_compile = compile(ir_while_true_return_parse.program);
    if (ir_while_true_return_compile.error || !ir_while_true_return_compile.function) {
        tests_failed++;
        printf("  FAIL: lightweight IR while(true)-return pruning compile\n");
    } else {
        const Chunk* loop_ret_chunk = find_compiled_function_chunk(&ir_while_true_return_compile, "loopRet");
        const Chunk* loop_dyn_chunk = find_compiled_function_chunk(&ir_while_true_return_compile, "loopDyn");
        const Chunk* loop_break_chunk = find_compiled_function_chunk(&ir_while_true_return_compile, "loopBreak");

        bool removed_loop_scaffolding =
            loop_ret_chunk && loop_dyn_chunk &&
            (loop_ret_chunk->code_count + 3 <= loop_dyn_chunk->code_count);
        bool preserved_break_loop_context =
            loop_break_chunk && loop_ret_chunk &&
            (loop_ret_chunk->code_count + 3 <= loop_break_chunk->code_count);

        if (removed_loop_scaffolding && preserved_break_loop_context) {
            tests_passed++;
            printf("  PASS: lightweight IR lowers while(true) returning bodies to straight-line return\n");
        } else {
            tests_failed++;
            printf("  FAIL: lightweight IR lowers while(true) returning bodies to straight-line return\n");
        }
    }

    for (int i = 0; i < ir_while_true_return_compile.function_count; i++) {
        if (ir_while_true_return_compile.functions && ir_while_true_return_compile.functions[i]) {
            obj_function_free(ir_while_true_return_compile.functions[i]);
        }
    }
    if (ir_while_true_return_compile.functions) free(ir_while_true_return_compile.functions);
    if (ir_while_true_return_compile.function) obj_function_free(ir_while_true_return_compile.function);
    symbol_table_free(ir_while_true_return_compile.globals);
    error_free(ir_while_true_return_compile.error);

    symbol_table_free(ir_while_true_return_tc.globals);
    error_free(ir_while_true_return_tc.error);
    parser_free_result(&ir_while_true_return_parse);
}

static void test_compiler_jit_leaf_hints(void) {
    printf("Testing compiler JIT leaf hints...\n");

    const char* source =
        "func plainMin(a: int, b: int): int {\n"
        "    if (a < b) {\n"
        "        return a;\n"
        "    }\n"
        "    return b;\n"
        "}\n"
        "func guardedMin(a: int, b: int): int {\n"
        "    if (a < 0) {\n"
        "        return a;\n"
        "    }\n"
        "    if (a < b) {\n"
        "        return a;\n"
        "    }\n"
        "    return b;\n"
        "}\n"
        "func reversedMax(a: int, b: int): int {\n"
        "    if (b > a) {\n"
        "        return b;\n"
        "    }\n"
        "    return a;\n"
        "}\n"
        "func ltBool(a: int, b: int): bool {\n"
        "    return a < b;\n"
        "}\n"
        "func guardedLtBool(a: int, b: int): bool {\n"
        "    if (a < 0) {\n"
        "        return false;\n"
        "    }\n"
        "    return a < b;\n"
        "}\n"
        "func ltConst(n: int): bool {\n"
        "    return n < 5;\n"
        "}\n"
        "func minConst(n: int): int {\n"
        "    if (n < 5) {\n"
        "        return n;\n"
        "    }\n"
        "    return 5;\n"
        "}\n"
        "func maxConst(n: int): int {\n"
        "    if (n < 5) {\n"
        "        return 5;\n"
        "    }\n"
        "    return n;\n"
        "}\n"
        "func guardedMinConst(n: int): int {\n"
        "    if (n < 0) {\n"
        "        return n;\n"
        "    }\n"
        "    if (n < 5) {\n"
        "        return n;\n"
        "    }\n"
        "    return 5;\n"
        "}\n"
        "func addConst(n: int): int {\n"
        "    return n + 5;\n"
        "}\n"
        "func guardedAddConst(n: int): int {\n"
        "    if (n < 0) {\n"
        "        return n;\n"
        "    }\n"
        "    return n + 5;\n"
        "}\n"
        "func add2(a: int, b: int): int {\n"
        "    return a + b;\n"
        "}\n"
        "func guardedAdd2(a: int, b: int): int {\n"
        "    if (a < 0) {\n"
        "        return a;\n"
        "    }\n"
        "    return a + b;\n"
        "}\n"
        "async func asyncLeaf(n: int): int {\n"
        "    return n + 1;\n"
        "}\n"
        "func captureOuter(base: int): int {\n"
        "    var x: int = base;\n"
        "    var f = func(y: int): int { return x + y; };\n"
        "    return f(1);\n"
        "}\n"
        "func main(): void {}\n";

    ParseResult parse_result = parser_parse(source, "test.tblo");
    if (parse_result.error || !parse_result.program) {
        tests_failed++;
        printf("  FAIL: compiler JIT leaf hints parse\n");
        parser_free_result(&parse_result);
        return;
    }

    TypeCheckResult tc_result = typecheck(parse_result.program);
    if (tc_result.error) {
        tests_failed++;
        printf("  FAIL: compiler JIT leaf hints typecheck\n");
        symbol_table_free(tc_result.globals);
        error_free(tc_result.error);
        parser_free_result(&parse_result);
        return;
    }

    CompileResult compile_result = compile(parse_result.program);
    if (compile_result.error || !compile_result.function) {
        tests_failed++;
        printf("  FAIL: compiler JIT leaf hints compile\n");
    } else {
        ObjFunction* plain_min = find_compiled_function_object(&compile_result, "plainMin");
        ObjFunction* guarded_min = find_compiled_function_object(&compile_result, "guardedMin");
        ObjFunction* reversed_max = find_compiled_function_object(&compile_result, "reversedMax");
        ObjFunction* lt_bool = find_compiled_function_object(&compile_result, "ltBool");
        ObjFunction* guarded_lt_bool = find_compiled_function_object(&compile_result, "guardedLtBool");
        ObjFunction* lt_const = find_compiled_function_object(&compile_result, "ltConst");
        ObjFunction* min_const = find_compiled_function_object(&compile_result, "minConst");
        ObjFunction* max_const = find_compiled_function_object(&compile_result, "maxConst");
        ObjFunction* guarded_min_const = find_compiled_function_object(&compile_result, "guardedMinConst");
        ObjFunction* add_const = find_compiled_function_object(&compile_result, "addConst");
        ObjFunction* guarded_add_const = find_compiled_function_object(&compile_result, "guardedAddConst");
        ObjFunction* add2 = find_compiled_function_object(&compile_result, "add2");
        ObjFunction* guarded_add2 = find_compiled_function_object(&compile_result, "guardedAdd2");
        ObjFunction* async_leaf = find_compiled_function_object(&compile_result, "asyncLeaf");
        ObjFunction* captured_lambda = NULL;
        for (int i = 0; i < compile_result.function_count; i++) {
            if (compile_result.functions && compile_result.functions[i] &&
                compile_result.functions[i]->capture_count > 0) {
                captured_lambda = compile_result.functions[i];
                break;
            }
        }

        if (plain_min &&
            plain_min->jit_profile.summary.kind == JIT_SUMMARY_KIND_INT_TWOARG_SELECTOR &&
            plain_min->jit_profile.summary.op == JIT_SUMMARY_OP_LT &&
            plain_min->jit_profile.summary.slot0 == 0 &&
            plain_min->jit_profile.summary.slot1 == 1 &&
            plain_min->jit_hint_plan.kind == JIT_COMPILED_KIND_INT_SELECTOR_LOCALS_GENERIC &&
            plain_min->jit_hint_plan.op == JIT_SUMMARY_OP_LT &&
            plain_min->jit_hint_plan.flags == JIT_PLAN_FLAG_NONE &&
            plain_min->jit_hint_plan.local_slot == 0 &&
            plain_min->jit_hint_plan.local_slot_b == 1) {
            tests_passed++;
            printf("  PASS: compiler emits selector JIT hints for plain min-style leaves\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler emits selector JIT hints for plain min-style leaves\n");
        }

        if (guarded_min &&
            guarded_min->jit_profile.summary.kind == JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_SELECTOR &&
            guarded_min->jit_profile.summary.op == JIT_SUMMARY_OP_LT &&
            guarded_min->jit_profile.summary.slot0 == 0 &&
            guarded_min->jit_profile.summary.slot1 == 1 &&
            guarded_min->jit_profile.summary.int_const0 == 0 &&
            guarded_min->jit_hint_plan.kind == JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCALS_GENERIC &&
            guarded_min->jit_hint_plan.op == JIT_SUMMARY_OP_LT &&
            guarded_min->jit_hint_plan.flags == JIT_PLAN_FLAG_NONE &&
            guarded_min->jit_hint_plan.local_slot == 0 &&
            guarded_min->jit_hint_plan.local_slot_b == 1 &&
            guarded_min->jit_hint_plan.int_const0 == 0) {
            tests_passed++;
            printf("  PASS: compiler emits selector JIT hints for guarded selector leaves\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler emits selector JIT hints for guarded selector leaves\n");
        }

        if (reversed_max &&
            reversed_max->jit_profile.summary.kind == JIT_SUMMARY_KIND_INT_TWOARG_SELECTOR &&
            reversed_max->jit_profile.summary.op == JIT_SUMMARY_OP_LT &&
            reversed_max->jit_profile.summary.slot0 == 1 &&
            reversed_max->jit_profile.summary.slot1 == 0 &&
            reversed_max->jit_hint_plan.kind == JIT_COMPILED_KIND_INT_SELECTOR_LOCALS_GENERIC &&
            reversed_max->jit_hint_plan.op == JIT_SUMMARY_OP_LT &&
            reversed_max->jit_hint_plan.flags == JIT_PLAN_FLAG_NONE &&
            reversed_max->jit_hint_plan.local_slot == 1 &&
            reversed_max->jit_hint_plan.local_slot_b == 0) {
            tests_passed++;
            printf("  PASS: compiler normalizes reversed selector comparisons into native hints\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler normalizes reversed selector comparisons into native hints\n");
        }

        if (lt_bool &&
            lt_bool->jit_profile.summary.kind == JIT_SUMMARY_KIND_BOOL_TWOARG_COMPARE &&
            lt_bool->jit_profile.summary.op == JIT_SUMMARY_OP_LT &&
            lt_bool->jit_profile.summary.slot0 == 0 &&
            lt_bool->jit_profile.summary.slot1 == 1 &&
            lt_bool->jit_hint_plan.kind == JIT_COMPILED_KIND_BOOL_COMPARE_LOCALS_GENERIC &&
            lt_bool->jit_hint_plan.op == JIT_SUMMARY_OP_LT &&
            lt_bool->jit_hint_plan.flags == JIT_PLAN_FLAG_NONE &&
            lt_bool->jit_hint_plan.local_slot == 0 &&
            lt_bool->jit_hint_plan.local_slot_b == 1) {
            tests_passed++;
            printf("  PASS: compiler routes compare leaves through the JIT hint summary path\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler routes compare leaves through the JIT hint summary path\n");
        }

        if (lt_const &&
            lt_const->jit_profile.summary.kind == JIT_SUMMARY_KIND_BOOL_LOCAL_CONST_COMPARE &&
            lt_const->jit_profile.summary.op == JIT_SUMMARY_OP_LT &&
            lt_const->jit_profile.summary.slot0 == 0 &&
            lt_const->jit_profile.summary.int_const0 == 5 &&
            lt_const->jit_hint_plan.kind == JIT_COMPILED_KIND_BOOL_COMPARE_LOCAL_CONST_GENERIC &&
            lt_const->jit_hint_plan.op == JIT_SUMMARY_OP_LT &&
            lt_const->jit_hint_plan.flags == JIT_PLAN_FLAG_NONE &&
            lt_const->jit_hint_plan.local_slot == 0 &&
            lt_const->jit_hint_plan.int_const0 == 5) {
            tests_passed++;
            printf("  PASS: compiler emits local-const bool compare summaries for native JIT hints\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler emits local-const bool compare summaries for native JIT hints\n");
        }

        if (guarded_lt_bool &&
            guarded_lt_bool->jit_profile.summary.kind == JIT_SUMMARY_KIND_BOOL_GUARDED_TWOARG_COMPARE &&
            guarded_lt_bool->jit_profile.summary.op == JIT_SUMMARY_OP_LT &&
            guarded_lt_bool->jit_profile.summary.slot0 == 0 &&
            guarded_lt_bool->jit_profile.summary.slot1 == 1 &&
            guarded_lt_bool->jit_profile.summary.int_const0 == 0 &&
            guarded_lt_bool->jit_profile.summary.int_const1 == 0 &&
            guarded_lt_bool->jit_hint_plan.kind == JIT_COMPILED_KIND_BOOL_COMPARE_GUARDED_LOCALS_GENERIC &&
            guarded_lt_bool->jit_hint_plan.op == JIT_SUMMARY_OP_LT &&
            guarded_lt_bool->jit_hint_plan.flags == JIT_PLAN_FLAG_NONE &&
            guarded_lt_bool->jit_hint_plan.local_slot == 0 &&
            guarded_lt_bool->jit_hint_plan.local_slot_b == 1 &&
            guarded_lt_bool->jit_hint_plan.int_const0 == 0 &&
            guarded_lt_bool->jit_hint_plan.int_const1 == 0) {
            tests_passed++;
            printf("  PASS: compiler emits guarded bool compare summaries for native JIT hints\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler emits guarded bool compare summaries for native JIT hints\n");
        }

        if (min_const &&
            min_const->jit_profile.summary.kind == JIT_SUMMARY_KIND_INT_LOCAL_CONST_SELECTOR &&
            min_const->jit_profile.summary.op == JIT_SUMMARY_OP_LT &&
            min_const->jit_profile.summary.slot0 == 0 &&
            min_const->jit_profile.summary.slot1 == 1 &&
            min_const->jit_profile.summary.int_const0 == 5 &&
            min_const->jit_hint_plan.kind == JIT_COMPILED_KIND_INT_SELECTOR_LOCAL_CONST_GENERIC &&
            min_const->jit_hint_plan.op == JIT_SUMMARY_OP_LT &&
            min_const->jit_hint_plan.flags == JIT_PLAN_FLAG_RETURN_LOCAL_ON_TRUE &&
            min_const->jit_hint_plan.local_slot == 0 &&
            min_const->jit_hint_plan.int_const0 == 5) {
            tests_passed++;
            printf("  PASS: compiler emits local-const min-style selector summaries for native JIT hints\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler emits local-const min-style selector summaries for native JIT hints\n");
        }

        if (max_const &&
            max_const->jit_profile.summary.kind == JIT_SUMMARY_KIND_INT_LOCAL_CONST_SELECTOR &&
            max_const->jit_profile.summary.op == JIT_SUMMARY_OP_LT &&
            max_const->jit_profile.summary.slot0 == 0 &&
            max_const->jit_profile.summary.slot1 == 0 &&
            max_const->jit_profile.summary.int_const0 == 5 &&
            max_const->jit_hint_plan.kind == JIT_COMPILED_KIND_INT_SELECTOR_LOCAL_CONST_GENERIC &&
            max_const->jit_hint_plan.op == JIT_SUMMARY_OP_LT &&
            max_const->jit_hint_plan.flags == JIT_PLAN_FLAG_NONE &&
            max_const->jit_hint_plan.local_slot == 0 &&
            max_const->jit_hint_plan.int_const0 == 5) {
            tests_passed++;
            printf("  PASS: compiler emits local-const max-style selector summaries for native JIT hints\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler emits local-const max-style selector summaries for native JIT hints\n");
        }

        if (guarded_min_const &&
            guarded_min_const->jit_profile.summary.kind == JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_SELECTOR &&
            guarded_min_const->jit_profile.summary.op == JIT_SUMMARY_OP_LT &&
            guarded_min_const->jit_profile.summary.slot0 == 0 &&
            guarded_min_const->jit_profile.summary.slot1 == 1 &&
            guarded_min_const->jit_profile.summary.int_const0 == 0 &&
            guarded_min_const->jit_profile.summary.int_const1 == 5 &&
            guarded_min_const->jit_hint_plan.kind == JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCAL_CONST_GENERIC &&
            guarded_min_const->jit_hint_plan.op == JIT_SUMMARY_OP_LT &&
            guarded_min_const->jit_hint_plan.flags == JIT_PLAN_FLAG_RETURN_LOCAL_ON_TRUE &&
            guarded_min_const->jit_hint_plan.local_slot == 0 &&
            guarded_min_const->jit_hint_plan.int_const0 == 0 &&
            guarded_min_const->jit_hint_plan.int_const1 == 5) {
            tests_passed++;
            printf("  PASS: compiler emits guarded local-const selector summaries for native JIT hints\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler emits guarded local-const selector summaries for native JIT hints\n");
        }

        if (add_const &&
            add_const->jit_profile.summary.kind == JIT_SUMMARY_KIND_INT_LOCAL_CONST_BINARY &&
            add_const->jit_profile.summary.op == JIT_SUMMARY_OP_ADD &&
            add_const->jit_profile.summary.slot0 == 0 &&
            add_const->jit_profile.summary.int_const0 == 5 &&
            add_const->jit_hint_plan.kind == JIT_COMPILED_KIND_INT_BINARY_LOCAL_CONST_GENERIC &&
            add_const->jit_hint_plan.op == JIT_SUMMARY_OP_ADD &&
            add_const->jit_hint_plan.flags == JIT_PLAN_FLAG_NONE &&
            add_const->jit_hint_plan.local_slot == 0 &&
            add_const->jit_hint_plan.int_const0 == 5) {
            tests_passed++;
            printf("  PASS: compiler emits local-const arithmetic summaries for generic native JIT hints\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler emits local-const arithmetic summaries for generic native JIT hints\n");
        }

        if (guarded_add_const &&
            guarded_add_const->jit_profile.summary.kind == JIT_SUMMARY_KIND_INT_GUARDED_LOCAL_CONST_BINARY &&
            guarded_add_const->jit_profile.summary.op == JIT_SUMMARY_OP_ADD &&
            guarded_add_const->jit_profile.summary.slot0 == 0 &&
            guarded_add_const->jit_profile.summary.int_const0 == 0 &&
            guarded_add_const->jit_profile.summary.int_const1 == 5 &&
            guarded_add_const->jit_hint_plan.kind == JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCAL_CONST_GENERIC &&
            guarded_add_const->jit_hint_plan.op == JIT_SUMMARY_OP_ADD &&
            guarded_add_const->jit_hint_plan.flags == JIT_PLAN_FLAG_NONE &&
            guarded_add_const->jit_hint_plan.local_slot == 0 &&
            guarded_add_const->jit_hint_plan.int_const0 == 0 &&
            guarded_add_const->jit_hint_plan.int_const1 == 5) {
            tests_passed++;
            printf("  PASS: compiler emits guarded local-const arithmetic summaries for generic native JIT hints\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler emits guarded local-const arithmetic summaries for generic native JIT hints\n");
        }

        if (add2 &&
            add2->jit_profile.summary.kind == JIT_SUMMARY_KIND_INT_TWOARG_BINARY &&
            add2->jit_profile.summary.op == JIT_SUMMARY_OP_ADD &&
            add2->jit_profile.summary.slot0 == 0 &&
            add2->jit_profile.summary.slot1 == 1 &&
            add2->jit_hint_plan.kind == JIT_COMPILED_KIND_INT_BINARY_LOCALS_GENERIC &&
            add2->jit_hint_plan.op == JIT_SUMMARY_OP_ADD &&
            add2->jit_hint_plan.flags == JIT_PLAN_FLAG_NONE &&
            add2->jit_hint_plan.local_slot == 0 &&
            add2->jit_hint_plan.local_slot_b == 1) {
            tests_passed++;
            printf("  PASS: compiler emits two-arg arithmetic summaries for generic native JIT hints\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler emits two-arg arithmetic summaries for generic native JIT hints\n");
        }

        if (guarded_add2 &&
            guarded_add2->jit_profile.summary.kind == JIT_SUMMARY_KIND_INT_GUARDED_TWOARG_BINARY &&
            guarded_add2->jit_profile.summary.op == JIT_SUMMARY_OP_ADD &&
            guarded_add2->jit_profile.summary.slot0 == 0 &&
            guarded_add2->jit_profile.summary.slot1 == 1 &&
            guarded_add2->jit_profile.summary.int_const0 == 0 &&
            guarded_add2->jit_hint_plan.kind == JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCALS_GENERIC &&
            guarded_add2->jit_hint_plan.op == JIT_SUMMARY_OP_ADD &&
            guarded_add2->jit_hint_plan.flags == JIT_PLAN_FLAG_NONE &&
            guarded_add2->jit_hint_plan.local_slot == 0 &&
            guarded_add2->jit_hint_plan.local_slot_b == 1 &&
            guarded_add2->jit_hint_plan.int_const0 == 0) {
            tests_passed++;
            printf("  PASS: compiler emits guarded two-arg arithmetic summaries for generic native JIT hints\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler emits guarded two-arg arithmetic summaries for generic native JIT hints\n");
        }

        if (plain_min &&
            plain_min->jit_profile.flags == JIT_PROFILE_FLAG_NONE &&
            plain_min->jit_profile.support_mask ==
                (JIT_PROFILE_SUPPORT_STUB | JIT_PROFILE_SUPPORT_NATIVE_SUMMARY) &&
            plain_min->jit_profile.native_family_mask == JIT_PROFILE_NATIVE_FAMILY_SELECTOR &&
            plain_min->jit_profile.param_count == plain_min->param_count &&
            plain_min->jit_profile.local_count == plain_min->local_count &&
            plain_min->jit_profile.capture_count == plain_min->capture_count) {
            tests_passed++;
            printf("  PASS: compiler records sync function JIT profile metadata\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler records sync function JIT profile metadata\n");
        }

        if (async_leaf &&
            async_leaf->jit_profile.flags == JIT_PROFILE_FLAG_ASYNC &&
            async_leaf->jit_profile.support_mask == JIT_PROFILE_SUPPORT_NONE &&
            async_leaf->jit_profile.native_family_mask == JIT_PROFILE_NATIVE_FAMILY_NONE &&
            async_leaf->jit_profile.param_count == async_leaf->param_count &&
            async_leaf->jit_profile.local_count == async_leaf->local_count &&
            async_leaf->jit_profile.capture_count == async_leaf->capture_count &&
            async_leaf->jit_profile.summary.kind == JIT_SUMMARY_KIND_NONE &&
            async_leaf->jit_hint_plan.kind == JIT_COMPILED_KIND_NONE) {
            tests_passed++;
            printf("  PASS: compiler records async function JIT profile metadata without native hints\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler records async function JIT profile metadata without native hints\n");
        }

        if (captured_lambda &&
            captured_lambda->jit_profile.flags == JIT_PROFILE_FLAG_HAS_CAPTURES &&
            captured_lambda->jit_profile.support_mask == JIT_PROFILE_SUPPORT_STUB &&
            captured_lambda->jit_profile.native_family_mask == JIT_PROFILE_NATIVE_FAMILY_NONE &&
            captured_lambda->jit_profile.param_count == captured_lambda->param_count &&
            captured_lambda->jit_profile.local_count == captured_lambda->local_count &&
            captured_lambda->jit_profile.capture_count == captured_lambda->capture_count &&
            captured_lambda->jit_profile.capture_count > 0 &&
            captured_lambda->jit_profile.summary.kind == JIT_SUMMARY_KIND_NONE &&
            captured_lambda->jit_hint_plan.kind == JIT_COMPILED_KIND_NONE) {
            tests_passed++;
            printf("  PASS: compiler records closure-capture JIT profile metadata without native hints\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler records closure-capture JIT profile metadata without native hints\n");
        }

        if (add_const &&
            add_const->jit_profile.native_family_mask == JIT_PROFILE_NATIVE_FAMILY_ARITHMETIC &&
            lt_bool &&
            lt_bool->jit_profile.native_family_mask == JIT_PROFILE_NATIVE_FAMILY_COMPARE) {
            tests_passed++;
            printf("  PASS: compiler records JIT native family masks for arithmetic and compare leaves\n");
        } else {
            tests_failed++;
            printf("  FAIL: compiler records JIT native family masks for arithmetic and compare leaves\n");
        }

        {
            VM vm;
            vm_init(&vm);
            jit_set_hot_threshold(&vm, 1);

            JitFunctionSummary add_const_summary = add_const ? add_const->jit_profile.summary : (JitFunctionSummary){0};
            JitCompiledPlan add_const_hint = add_const ? add_const->jit_hint_plan : (JitCompiledPlan){0};

            reset_function_for_jit_queue_test(add_const);
            if (add_const) {
                add_const->jit_profile.summary = add_const_summary;
                memset(&add_const->jit_hint_plan, 0, sizeof(add_const->jit_hint_plan));
                add_const->jit_hint_plan.kind = JIT_COMPILED_KIND_NONE;
            }
            int add_const_summary_only_processed =
                add_const ? (jit_record_function_entry(&vm, add_const), jit_drain_work_queue(&vm, 1)) : 0;
            if (add_const &&
                add_const_summary_only_processed == 1 &&
                add_const->jit_state == JIT_FUNC_STATE_COMPILED_NATIVE &&
                add_const->jit_reason == JIT_REASON_NATIVE_HINT &&
                add_const->jit_compiled_plan.kind == JIT_COMPILED_KIND_INT_BINARY_LOCAL_CONST_GENERIC &&
                add_const->jit_compiled_plan.op == JIT_SUMMARY_OP_ADD &&
                add_const->jit_compiled_plan.local_slot == 0 &&
                add_const->jit_compiled_plan.int_const0 == 5) {
                tests_passed++;
                printf("  PASS: compiler summary metadata takes priority over exact native rematching\n");
            } else {
                tests_failed++;
                printf("  FAIL: compiler summary metadata takes priority over exact native rematching\n");
            }

            reset_function_for_jit_queue_test(add_const);
            if (add_const) {
                memset(&add_const->jit_profile.summary, 0, sizeof(add_const->jit_profile.summary));
                add_const->jit_profile.summary.kind = JIT_SUMMARY_KIND_NONE;
                add_const->jit_hint_plan = add_const_hint;
            }
            int add_const_hint_only_processed =
                add_const ? (jit_record_function_entry(&vm, add_const), jit_drain_work_queue(&vm, 1)) : 0;
            if (add_const &&
                add_const_hint_only_processed == 1 &&
                add_const->jit_state == JIT_FUNC_STATE_COMPILED_NATIVE &&
                add_const->jit_reason == JIT_REASON_NATIVE_HINT &&
                add_const->jit_compiled_plan.kind == JIT_COMPILED_KIND_INT_BINARY_LOCAL_CONST_GENERIC &&
                add_const->jit_compiled_plan.op == JIT_SUMMARY_OP_ADD &&
                add_const->jit_compiled_plan.local_slot == 0 &&
                add_const->jit_compiled_plan.int_const0 == 5) {
                tests_passed++;
                printf("  PASS: compiler hint metadata takes priority over exact native rematching\n");
            } else {
                tests_failed++;
                printf("  FAIL: compiler hint metadata takes priority over exact native rematching\n");
            }

            reset_function_for_exact_jit_match_test(add_const);
            int add_const_processed = add_const ? (jit_record_function_entry(&vm, add_const), jit_drain_work_queue(&vm, 1)) : 0;
            if (add_const &&
                add_const_processed == 1 &&
                add_const->jit_state == JIT_FUNC_STATE_COMPILED_NATIVE &&
                add_const->jit_reason == JIT_REASON_NATIVE_EXACT &&
                add_const->jit_compiled_plan.kind == JIT_COMPILED_KIND_INT_BINARY_LOCAL_CONST_GENERIC &&
                add_const->jit_compiled_plan.op == JIT_SUMMARY_OP_ADD &&
                add_const->jit_compiled_plan.local_slot == 0 &&
                add_const->jit_compiled_plan.int_const0 == 5) {
                tests_passed++;
                printf("  PASS: exact local-const arithmetic rematcher lowers to generic native plans\n");
            } else {
                tests_failed++;
                printf("  FAIL: exact local-const arithmetic rematcher lowers to generic native plans\n");
            }

            reset_function_for_exact_jit_match_test(guarded_add_const);
            int guarded_add_const_processed =
                guarded_add_const ? (jit_record_function_entry(&vm, guarded_add_const), jit_drain_work_queue(&vm, 1)) : 0;
            if (guarded_add_const &&
                guarded_add_const_processed == 1 &&
                guarded_add_const->jit_state == JIT_FUNC_STATE_COMPILED_NATIVE &&
                guarded_add_const->jit_reason == JIT_REASON_NATIVE_EXACT &&
                guarded_add_const->jit_compiled_plan.kind == JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCAL_CONST_GENERIC &&
                guarded_add_const->jit_compiled_plan.op == JIT_SUMMARY_OP_ADD &&
                guarded_add_const->jit_compiled_plan.local_slot == 0 &&
                guarded_add_const->jit_compiled_plan.int_const0 == 0 &&
                guarded_add_const->jit_compiled_plan.int_const1 == 5) {
                tests_passed++;
                printf("  PASS: exact guarded local-const arithmetic rematcher lowers to generic native plans\n");
            } else {
                tests_failed++;
                printf("  FAIL: exact guarded local-const arithmetic rematcher lowers to generic native plans\n");
            }

            reset_function_for_exact_jit_match_test(add2);
            int add2_processed = add2 ? (jit_record_function_entry(&vm, add2), jit_drain_work_queue(&vm, 1)) : 0;
            if (add2 &&
                add2_processed == 1 &&
                add2->jit_state == JIT_FUNC_STATE_COMPILED_NATIVE &&
                add2->jit_reason == JIT_REASON_NATIVE_EXACT &&
                add2->jit_compiled_plan.kind == JIT_COMPILED_KIND_INT_BINARY_LOCALS_GENERIC &&
                add2->jit_compiled_plan.op == JIT_SUMMARY_OP_ADD &&
                add2->jit_compiled_plan.local_slot == 0 &&
                add2->jit_compiled_plan.local_slot_b == 1) {
                tests_passed++;
                printf("  PASS: exact two-arg arithmetic rematcher lowers to generic native plans\n");
            } else {
                tests_failed++;
                printf("  FAIL: exact two-arg arithmetic rematcher lowers to generic native plans\n");
            }

            reset_function_for_exact_jit_match_test(guarded_add2);
            int guarded_add2_processed =
                guarded_add2 ? (jit_record_function_entry(&vm, guarded_add2), jit_drain_work_queue(&vm, 1)) : 0;
            if (guarded_add2 &&
                guarded_add2_processed == 1 &&
                guarded_add2->jit_state == JIT_FUNC_STATE_COMPILED_NATIVE &&
                guarded_add2->jit_reason == JIT_REASON_NATIVE_EXACT &&
                guarded_add2->jit_compiled_plan.kind == JIT_COMPILED_KIND_INT_BINARY_GUARDED_LOCALS_GENERIC &&
                guarded_add2->jit_compiled_plan.op == JIT_SUMMARY_OP_ADD &&
                guarded_add2->jit_compiled_plan.local_slot == 0 &&
                guarded_add2->jit_compiled_plan.local_slot_b == 1 &&
                guarded_add2->jit_compiled_plan.int_const0 == 0) {
                tests_passed++;
                printf("  PASS: exact guarded two-arg arithmetic rematcher lowers to generic native plans\n");
            } else {
                tests_failed++;
                printf("  FAIL: exact guarded two-arg arithmetic rematcher lowers to generic native plans\n");
            }

            reset_function_for_exact_jit_match_test(lt_bool);
            int lt_bool_processed = lt_bool ? (jit_record_function_entry(&vm, lt_bool), jit_drain_work_queue(&vm, 1)) : 0;
            if (lt_bool &&
                lt_bool_processed == 1 &&
                lt_bool->jit_state == JIT_FUNC_STATE_COMPILED_NATIVE &&
                lt_bool->jit_reason == JIT_REASON_NATIVE_EXACT &&
                lt_bool->jit_compiled_plan.kind == JIT_COMPILED_KIND_BOOL_COMPARE_LOCALS_GENERIC &&
                lt_bool->jit_compiled_plan.op == JIT_SUMMARY_OP_LT &&
                lt_bool->jit_compiled_plan.local_slot == 0 &&
                lt_bool->jit_compiled_plan.local_slot_b == 1) {
                tests_passed++;
                printf("  PASS: exact two-arg bool compare rematcher lowers through recovered summaries\n");
            } else {
                tests_failed++;
                printf("  FAIL: exact two-arg bool compare rematcher lowers through recovered summaries\n");
            }

            reset_function_for_exact_jit_match_test(guarded_lt_bool);
            int guarded_lt_bool_processed =
                guarded_lt_bool ? (jit_record_function_entry(&vm, guarded_lt_bool), jit_drain_work_queue(&vm, 1)) : 0;
            if (guarded_lt_bool &&
                guarded_lt_bool_processed == 1 &&
                guarded_lt_bool->jit_state == JIT_FUNC_STATE_COMPILED_NATIVE &&
                guarded_lt_bool->jit_reason == JIT_REASON_NATIVE_EXACT &&
                guarded_lt_bool->jit_compiled_plan.kind == JIT_COMPILED_KIND_BOOL_COMPARE_GUARDED_LOCALS_GENERIC &&
                guarded_lt_bool->jit_compiled_plan.op == JIT_SUMMARY_OP_LT &&
                guarded_lt_bool->jit_compiled_plan.local_slot == 0 &&
                guarded_lt_bool->jit_compiled_plan.local_slot_b == 1 &&
                guarded_lt_bool->jit_compiled_plan.int_const0 == 0 &&
                guarded_lt_bool->jit_compiled_plan.int_const1 == 0) {
                tests_passed++;
                printf("  PASS: exact guarded bool compare rematcher lowers through recovered summaries\n");
            } else {
                tests_failed++;
                printf("  FAIL: exact guarded bool compare rematcher lowers through recovered summaries\n");
            }

            reset_function_for_exact_jit_match_test(plain_min);
            int plain_min_processed = plain_min ? (jit_record_function_entry(&vm, plain_min), jit_drain_work_queue(&vm, 1)) : 0;
            if (plain_min &&
                plain_min_processed == 1 &&
                plain_min->jit_state == JIT_FUNC_STATE_COMPILED_NATIVE &&
                plain_min->jit_reason == JIT_REASON_NATIVE_EXACT &&
                plain_min->jit_compiled_plan.kind == JIT_COMPILED_KIND_INT_SELECTOR_LOCALS_GENERIC &&
                plain_min->jit_compiled_plan.op == JIT_SUMMARY_OP_LT &&
                plain_min->jit_compiled_plan.local_slot == 0 &&
                plain_min->jit_compiled_plan.local_slot_b == 1) {
                tests_passed++;
                printf("  PASS: exact two-arg selector rematcher lowers through recovered summaries\n");
            } else {
                tests_failed++;
                printf("  FAIL: exact two-arg selector rematcher lowers through recovered summaries\n");
            }

            reset_function_for_exact_jit_match_test(guarded_min);
            int guarded_min_processed =
                guarded_min ? (jit_record_function_entry(&vm, guarded_min), jit_drain_work_queue(&vm, 1)) : 0;
            if (guarded_min &&
                guarded_min_processed == 1 &&
                guarded_min->jit_state == JIT_FUNC_STATE_COMPILED_NATIVE &&
                guarded_min->jit_reason == JIT_REASON_NATIVE_EXACT &&
                guarded_min->jit_compiled_plan.kind == JIT_COMPILED_KIND_INT_SELECTOR_GUARDED_LOCALS_GENERIC &&
                guarded_min->jit_compiled_plan.op == JIT_SUMMARY_OP_LT &&
                guarded_min->jit_compiled_plan.local_slot == 0 &&
                guarded_min->jit_compiled_plan.local_slot_b == 1 &&
                guarded_min->jit_compiled_plan.int_const0 == 0) {
                tests_passed++;
                printf("  PASS: exact guarded selector rematcher lowers through recovered summaries\n");
            } else {
                tests_failed++;
                printf("  FAIL: exact guarded selector rematcher lowers through recovered summaries\n");
            }

            reset_function_for_exact_jit_match_test(add_const);
            if (add_const) add_const->jit_profile.support_mask = JIT_PROFILE_SUPPORT_STUB;
            int add_const_stub_only_processed =
                add_const ? (jit_record_function_entry(&vm, add_const), jit_drain_work_queue(&vm, 1)) : 0;
            if (add_const &&
                add_const_stub_only_processed == 1 &&
                add_const->jit_state == JIT_FUNC_STATE_COMPILED_STUB &&
                add_const->jit_reason == JIT_REASON_STUB_FALLBACK &&
                add_const->jit_compiled_plan.kind == JIT_COMPILED_KIND_STUB) {
                tests_passed++;
                printf("  PASS: stub-only JIT profile support bypasses native exact rematching\n");
            } else {
                tests_failed++;
                printf("  FAIL: stub-only JIT profile support bypasses native exact rematching\n");
            }

            reset_function_for_exact_jit_match_test(add_const);
            if (add_const) add_const->jit_profile.support_mask = JIT_PROFILE_SUPPORT_NONE;
            int add_const_unsupported_processed =
                add_const ? (jit_record_function_entry(&vm, add_const), jit_drain_work_queue(&vm, 1)) : 0;
            if (add_const &&
                add_const_unsupported_processed == 1 &&
                add_const->jit_state == JIT_FUNC_STATE_FAILED &&
                add_const->jit_reason == JIT_REASON_UNSUPPORTED_SHAPE &&
                add_const->jit_compiled_plan.kind == JIT_COMPILED_KIND_NONE) {
                tests_passed++;
                printf("  PASS: unsupported JIT profile support bypasses native exact rematching\n");
            } else {
                tests_failed++;
                printf("  FAIL: unsupported JIT profile support bypasses native exact rematching\n");
            }

            reset_function_for_exact_jit_match_test(add_const);
            if (add_const) add_const->jit_profile.native_family_mask = JIT_PROFILE_NATIVE_FAMILY_COMPARE;
            int add_const_wrong_family_processed =
                add_const ? (jit_record_function_entry(&vm, add_const), jit_drain_work_queue(&vm, 1)) : 0;
            if (add_const &&
                add_const_wrong_family_processed == 1 &&
                add_const->jit_state == JIT_FUNC_STATE_COMPILED_STUB &&
                add_const->jit_reason == JIT_REASON_STUB_FALLBACK &&
                add_const->jit_compiled_plan.kind == JIT_COMPILED_KIND_STUB) {
                tests_passed++;
                printf("  PASS: exact rematcher skips arithmetic recovery when the JIT family mask excludes it\n");
            } else {
                tests_failed++;
                printf("  FAIL: exact rematcher skips arithmetic recovery when the JIT family mask excludes it\n");
            }

            reset_function_for_exact_jit_match_test(captured_lambda);
            int captured_lambda_processed =
                captured_lambda ? (jit_record_function_entry(&vm, captured_lambda), jit_drain_work_queue(&vm, 1)) : 0;
            if (captured_lambda &&
                captured_lambda_processed == 1 &&
                captured_lambda->jit_state == JIT_FUNC_STATE_COMPILED_STUB &&
                captured_lambda->jit_reason == JIT_REASON_STUB_FALLBACK &&
                captured_lambda->jit_compiled_plan.kind == JIT_COMPILED_KIND_STUB) {
                tests_passed++;
                printf("  PASS: closure-capture JIT profile support routes sync closures to stub fallback\n");
            } else {
                tests_failed++;
                printf("  FAIL: closure-capture JIT profile support routes sync closures to stub fallback\n");
            }

            reset_function_for_exact_jit_match_test(async_leaf);
            int async_leaf_processed =
                async_leaf ? (jit_record_function_entry(&vm, async_leaf), jit_drain_work_queue(&vm, 1)) : 0;
            if (async_leaf &&
                async_leaf_processed == 1 &&
                async_leaf->jit_state == JIT_FUNC_STATE_FAILED &&
                async_leaf->jit_reason == JIT_REASON_UNSUPPORTED_ASYNC) {
                tests_passed++;
                printf("  PASS: async JIT profile support rejects async functions before stub fallback\n");
            } else {
                tests_failed++;
                printf("  FAIL: async JIT profile support rejects async functions before stub fallback\n");
            }

            vm_free(&vm);
        }
    }

    for (int i = 0; i < compile_result.function_count; i++) {
        if (compile_result.functions && compile_result.functions[i]) {
            obj_function_free(compile_result.functions[i]);
        }
    }
    if (compile_result.functions) free(compile_result.functions);
    if (compile_result.function) obj_function_free(compile_result.function);
    symbol_table_free(compile_result.globals);
    error_free(compile_result.error);

    symbol_table_free(tc_result.globals);
    error_free(tc_result.error);
    parser_free_result(&parse_result);
}

static void test_bytecode(void) {
    printf("Testing bytecode...\n");

    Chunk chunk;
    chunk_init(&chunk);

    int pos1 = chunk_emit(&chunk, OP_CONST, 1);
    chunk_emit(&chunk, 0, 1);

    int pos2 = chunk_emit(&chunk, OP_ADD, 2);
    (void)pos1;
    (void)pos2;

    if (chunk.code_count == 3 && chunk.code[0] == OP_CONST && chunk.code[2] == OP_ADD) {
        tests_passed++;
        printf("  PASS: bytecode emission\n");
    } else {
        tests_failed++;
        printf("  FAIL: bytecode emission\n");
    }

    chunk_free(&chunk);
}

static void test_vm(void) {
    printf("Testing VM...\n");

    VM vm;
    vm_init(&vm);

    Value val1;
    value_init_int(&val1, 10);
    vm_set_global(&vm, "x", val1);

    Value retrieved = vm_get_global(&vm, "x");
    if (value_get_type(&retrieved) == VAL_INT && value_get_int(&retrieved) == 10) {
        tests_passed++;
        printf("  PASS: VM global storage\n");
    } else {
        tests_failed++;
        printf("  FAIL: VM global storage\n");
    }

    vm_free(&vm);
}

static void test_vm_hash_table_resize(void) {
    printf("Testing VM hash table resize + intern stability...\n");

    VM vm;
    vm_init(&vm);

    const int num_globals = 4096;
    for (int i = 0; i < num_globals; i++) {
        char name[64];
        snprintf(name, sizeof(name), "global_resize_%d", i);
        Value val;
        value_init_int(&val, (int64_t)(i * 3 + 1));
        vm_set_global(&vm, name, val);
    }

    int globals_ok = 1;
    for (int i = 0; i < num_globals; i++) {
        char name[64];
        snprintf(name, sizeof(name), "global_resize_%d", i);
        Value got = vm_get_global(&vm, name);
        if (value_get_type(&got) != VAL_INT || value_get_int(&got) != (int64_t)(i * 3 + 1)) {
            globals_ok = 0;
            break;
        }
    }

    if (globals_ok) {
        tests_passed++;
        printf("  PASS: global lookup after repeated resize\n");
    } else {
        tests_failed++;
        printf("  FAIL: global lookup after repeated resize\n");
    }

    for (int i = 0; i < num_globals; i += 2) {
        char name[64];
        snprintf(name, sizeof(name), "global_resize_%d", i);
        Value val;
        value_init_int(&val, (int64_t)(i * 11 + 7));
        vm_set_global(&vm, name, val);
    }

    int updates_ok = 1;
    for (int i = 0; i < num_globals; i++) {
        char name[64];
        snprintf(name, sizeof(name), "global_resize_%d", i);
        Value got = vm_get_global(&vm, name);
        int64_t expected = (i % 2 == 0) ? (int64_t)(i * 11 + 7) : (int64_t)(i * 3 + 1);
        if (value_get_type(&got) != VAL_INT || value_get_int(&got) != expected) {
            updates_ok = 0;
            break;
        }
    }

    if (updates_ok) {
        tests_passed++;
        printf("  PASS: global update stability after resize\n");
    } else {
        tests_failed++;
        printf("  FAIL: global update stability after resize\n");
    }

    ObjString* intern_a = vm_intern_string(&vm, "intern_key", 10);
    ObjString* intern_b = vm_intern_string(&vm, "intern_key", 10);
    if (intern_a == intern_b) {
        tests_passed++;
        printf("  PASS: string interning identity for duplicate keys\n");
    } else {
        tests_failed++;
        printf("  FAIL: string interning identity for duplicate keys\n");
    }
    obj_string_release(intern_a);
    obj_string_release(intern_b);

    int intern_bulk_ok = 1;
    for (int i = 0; i < 2048; i++) {
        char key[64];
        snprintf(key, sizeof(key), "bulk_intern_%d", i);
        ObjString* s1 = vm_intern_string(&vm, key, (int)strlen(key));
        ObjString* s2 = vm_intern_string(&vm, key, (int)strlen(key));
        if (s1 != s2) {
            intern_bulk_ok = 0;
            obj_string_release(s1);
            obj_string_release(s2);
            break;
        }
        obj_string_release(s1);
        obj_string_release(s2);
    }

    if (intern_bulk_ok) {
        tests_passed++;
        printf("  PASS: bulk string interning across table growth\n");
    } else {
        tests_failed++;
        printf("  FAIL: bulk string interning across table growth\n");
    }

    vm_free(&vm);
}

static void test_compiler_regressions(void) {
    printf("Testing compiler regressions...\n");

    const char* nested_block_assign_source =
        "var blockResult: int = 0;\n"
        "var loopResult: int = 0;\n"
        "var outer: int = 0;\n"
        "{\n"
        "    var inner: int = 7;\n"
        "    outer = inner;\n"
        "}\n"
        "blockResult = outer;\n"
        "var loopOuter: int = 0;\n"
        "var i: int = 0;\n"
        "while (i < 1) {\n"
        "    var p: int = 9;\n"
        "    loopOuter = p;\n"
        "    break;\n"
        "}\n"
        "loopResult = loopOuter;\n";

    ParseResult regression_parse = parser_parse(nested_block_assign_source, "test.tblo");
    if (!regression_parse.error) {
        TypeCheckResult regression_tc = typecheck(regression_parse.program);
        if (!regression_tc.error) {
            CompileResult regression_compile = compile(regression_parse.program);
            if (!regression_compile.error && regression_compile.function) {
                VM vm;
                vm_init(&vm);
                int exec_rc = vm_execute(&vm, regression_compile.function);
                if (exec_rc == 0) {
                    Value block_result = vm_get_global(&vm, "blockResult");
                    Value loop_result = vm_get_global(&vm, "loopResult");
                    if (value_get_type(&block_result) == VAL_INT &&
                        value_get_int(&block_result) == 7 &&
                        value_get_type(&loop_result) == VAL_INT &&
                        value_get_int(&loop_result) == 9) {
                        tests_passed++;
                        printf("  PASS: outer local assignments survive nested block DSE\n");
                    } else {
                        tests_failed++;
                        printf("  FAIL: outer local assignments survive nested block DSE\n");
                    }
                } else {
                    tests_failed++;
                    printf("  FAIL: nested block DSE regression runtime\n");
                }
                vm_free(&vm);
            } else {
                tests_failed++;
                printf("  FAIL: nested block DSE regression compile\n");
            }

            for (int i = 0; i < regression_compile.function_count; i++) {
                if (regression_compile.functions && regression_compile.functions[i]) {
                    obj_function_free(regression_compile.functions[i]);
                }
            }
            if (regression_compile.functions) free(regression_compile.functions);
            if (regression_compile.function) obj_function_free(regression_compile.function);
            symbol_table_free(regression_compile.globals);
            error_free(regression_compile.error);
        } else {
            tests_failed++;
            printf("  FAIL: nested block DSE regression typecheck\n");
        }
        symbol_table_free(regression_tc.globals);
        error_free(regression_tc.error);
    } else {
        tests_failed++;
        printf("  FAIL: nested block DSE regression parse\n");
    }
    parser_free_result(&regression_parse);
}

static void test_artifact_roundtrip(void) {
    printf("Testing bytecode artifact roundtrip...\n");

    ObjFunction* func = obj_function_create();
    func->name = (char*)malloc(5);
    if (!func->name) {
        tests_failed++;
        printf("  FAIL: allocation failure\n");
        return;
    }
    memcpy(func->name, "main", 5);
    func->is_async = true;
    func->defer_handler_ip = 2;
    func->defer_return_slot = 1;

    chunk_emit(&func->chunk, OP_CONST, 1);
    chunk_emit(&func->chunk, 0, 1);
    chunk_emit(&func->chunk, OP_RET, 1);

    Constant c;
    c.type_index = 0;
    c.as_int = 42;
    constant_pool_add(&func->constants, c);

    ObjFunction* funcs[1];
    funcs[0] = func;

    const char* tmp_path = "artifact_roundtrip_tmp.tbc";
    char err[256];
    int ok = artifact_write_file(tmp_path,
                                 NULL,
                                 funcs,
                                 1,
                                 0,
                                 0,
                                 NULL,
                                 0,
                                 NULL,
                                 0,
                                 err,
                                 sizeof(err));
    if (!ok) {
        tests_failed++;
        printf("  FAIL: write artifact (%s)\n", err[0] ? err : "unknown");
        obj_function_free(func);
        return;
    }

    LoadedBytecodeArtifact loaded;
    ok = artifact_load_file(tmp_path, &loaded, err, sizeof(err));
    if (!ok) {
        tests_failed++;
        printf("  FAIL: load artifact (%s)\n", err[0] ? err : "unknown");
        obj_function_free(func);
        remove(tmp_path);
        return;
    }

    int pass = 1;
    if (loaded.function_count != 1 || loaded.main_index != 0 || !loaded.functions || !loaded.functions[0]) {
        pass = 0;
    } else {
        ObjFunction* f = loaded.functions[0];
        if (!f->name || strcmp(f->name, "main") != 0) pass = 0;
        if (!f->is_async) pass = 0;
        if (f->defer_handler_ip != 2) pass = 0;
        if (f->defer_return_slot != 1) pass = 0;
        if (f->chunk.code_count != 3) pass = 0;
        if (f->constants.constant_count != 1) pass = 0;
        if (f->constants.constant_count == 1) {
            Constant rc = f->constants.constants[0];
            if (rc.type_index != 0 || rc.as_int != 42) pass = 0;
        }
    }

    if (pass) {
        tests_passed++;
        printf("  PASS: artifact write+read roundtrip\n");
    } else {
        tests_failed++;
        printf("  FAIL: artifact write+read roundtrip\n");
    }

    artifact_loaded_free(&loaded);

    FILE* artifact_file = fopen(tmp_path, "rb");
    if (!artifact_file) {
        tests_failed++;
        printf("  FAIL: reopen artifact for bytes roundtrip\n");
        obj_function_free(func);
        remove(tmp_path);
        return;
    }
    if (fseek(artifact_file, 0, SEEK_END) != 0) {
        fclose(artifact_file);
        tests_failed++;
        printf("  FAIL: size artifact for bytes roundtrip\n");
        obj_function_free(func);
        remove(tmp_path);
        return;
    }
    long artifact_size = ftell(artifact_file);
    if (artifact_size <= 0 || fseek(artifact_file, 0, SEEK_SET) != 0) {
        fclose(artifact_file);
        tests_failed++;
        printf("  FAIL: rewind artifact for bytes roundtrip\n");
        obj_function_free(func);
        remove(tmp_path);
        return;
    }
    uint8_t* artifact_bytes = (uint8_t*)malloc((size_t)artifact_size);
    if (!artifact_bytes || fread(artifact_bytes, 1, (size_t)artifact_size, artifact_file) != (size_t)artifact_size) {
        fclose(artifact_file);
        free(artifact_bytes);
        tests_failed++;
        printf("  FAIL: read artifact bytes roundtrip fixture\n");
        obj_function_free(func);
        remove(tmp_path);
        return;
    }
    fclose(artifact_file);

    LoadedBytecodeArtifact loaded_from_bytes;
    ok = artifact_load_bytes(artifact_bytes, (size_t)artifact_size, &loaded_from_bytes, err, sizeof(err));
    free(artifact_bytes);
    if (!ok) {
        tests_failed++;
        printf("  FAIL: load artifact bytes (%s)\n", err[0] ? err : "unknown");
        obj_function_free(func);
        remove(tmp_path);
        return;
    }

    pass = 1;
    if (loaded_from_bytes.function_count != 1 ||
        loaded_from_bytes.main_index != 0 ||
        !loaded_from_bytes.functions ||
        !loaded_from_bytes.functions[0]) {
        pass = 0;
    } else {
        ObjFunction* f = loaded_from_bytes.functions[0];
        if (!f->name || strcmp(f->name, "main") != 0) pass = 0;
        if (!f->is_async) pass = 0;
        if (f->defer_handler_ip != 2) pass = 0;
        if (f->defer_return_slot != 1) pass = 0;
        if (f->chunk.code_count != 3) pass = 0;
        if (f->constants.constant_count != 1) pass = 0;
        if (f->constants.constant_count == 1) {
            Constant rc = f->constants.constants[0];
            if (rc.type_index != 0 || rc.as_int != 42) pass = 0;
        }
    }

    if (pass) {
        tests_passed++;
        printf("  PASS: artifact bytes load roundtrip\n");
    } else {
        tests_failed++;
        printf("  FAIL: artifact bytes load roundtrip\n");
    }

    artifact_loaded_free(&loaded_from_bytes);
    obj_function_free(func);
    remove(tmp_path);
}

static void test_lsp_document_symbols(void) {
    Error* error = NULL;
    char* json = NULL;

    printf("Testing LSP document symbols...\n");

    json = lsp_build_document_symbols_json("tablo_tests/lsp_symbols_test.tblo", &error);
    if (json && !error &&
        strstr(json, "\"name\":\"Point\"") != NULL &&
        strstr(json, "\"name\":\"Formatter\"") != NULL &&
        strstr(json, "\"name\":\"UserId\"") != NULL &&
        strstr(json, "\"name\":\"Status\"") != NULL &&
        strstr(json, "\"name\":\"Box\"") != NULL &&
        strstr(json, "\"name\":\"Maybe\"") != NULL &&
        strstr(json, "\"name\":\"Outcome\"") != NULL &&
        strstr(json, "\"name\":\"LIMIT\"") != NULL &&
        strstr(json, "\"name\":\"load\"") != NULL &&
        strstr(json, "\"name\":\"identity\"") != NULL &&
        strstr(json, "\"name\":\"pointX\"") != NULL &&
        strstr(json, "\"name\":\"localSum\"") != NULL &&
        strstr(json, "\"name\":\"convert\"") != NULL &&
        strstr(json, "\"name\":\"statusOk\"") != NULL &&
        strstr(json, "\"name\":\"render\"") != NULL &&
        strstr(json, "\"name\":\"x\"") != NULL &&
        strstr(json, "\"name\":\"Ok\"") != NULL &&
        strstr(json, "\"detail\":\"async func load(user_id: UserId): int\"") != NULL) {
        tests_passed++;
        printf("  PASS: LSP document symbols JSON includes top-level declarations and children\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP document symbols JSON includes top-level declarations and children\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_document_symbols_json("tablo_tests/does_not_exist.tblo", &error);
    if (!json && error && error->message &&
        strstr(error->message, "Failed to read") != NULL) {
        tests_passed++;
        printf("  PASS: LSP document symbols reports missing-file errors\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP document symbols reports missing-file errors\n");
    }
    if (json) free(json);
    error_free(error);
}

static void test_lsp_hover(void) {
    Error* error = NULL;
    char* json = NULL;

    printf("Testing LSP hover...\n");

    json = lsp_build_hover_json("tablo_tests/lsp_symbols_test.tblo", 19, 11, &error);
    if (json && !error &&
        strstr(json, "\"kind\":\"plaintext\"") != NULL &&
        strstr(json, "async func load(user_id: UserId): int") != NULL) {
        tests_passed++;
        printf("  PASS: LSP hover returns function declaration details\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP hover returns function declaration details\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_hover_json("tablo_tests/lsp_symbols_test.tblo", 20, 11, &error);
    if (json && !error &&
        strstr(json, "const LIMIT: int") != NULL) {
        tests_passed++;
        printf("  PASS: LSP hover returns identifier reference details\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP hover returns identifier reference details\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_hover_json("tablo_tests/lsp_symbols_test.tblo", 24, 16, &error);
    if (json && !error &&
        strstr(json, "\"value\":\"x: int\"") != NULL) {
        tests_passed++;
        printf("  PASS: LSP hover returns field access details\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP hover returns field access details\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_hover_json("tablo_tests/lsp_symbols_test.tblo", 36, 22, &error);
    if (json && !error &&
        strstr(json, "\"value\":\"point: Point\"") != NULL) {
        tests_passed++;
        printf("  PASS: LSP hover returns function parameter details\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP hover returns function parameter details\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_hover_json("tablo_tests/lsp_symbols_test.tblo", 38, 12, &error);
    if (json && !error &&
        strstr(json, "\"value\":\"var total: int\"") != NULL) {
        tests_passed++;
        printf("  PASS: LSP hover returns local variable details\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP hover returns local variable details\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_hover_json("tablo_tests/lsp_symbols_test.tblo", 37, 16, &error);
    if (json && !error &&
        strstr(json, "\"value\":\"record Point\"") != NULL) {
        tests_passed++;
        printf("  PASS: LSP hover returns named type reference details\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP hover returns named type reference details\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_hover_json("tablo_tests/lsp_symbols_test.tblo", 47, 11, &error);
    if (json && !error &&
        strstr(json, "\"value\":\"type T\"") != NULL) {
        tests_passed++;
        printf("  PASS: LSP hover returns generic record type-parameter details\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP hover returns generic record type-parameter details\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_hover_json("tablo_tests/lsp_symbols_test.tblo", 53, 9, &error);
    if (json && !error &&
        strstr(json, "\"value\":\"type T\"") != NULL) {
        tests_passed++;
        printf("  PASS: LSP hover returns generic enum type-parameter details\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP hover returns generic enum type-parameter details\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_hover_json("tablo_tests/lsp_symbols_test.tblo", 57, 14, &error);
    if (json && !error &&
        strstr(json, "\"value\":\"type T: Formatter\"") != NULL) {
        tests_passed++;
        printf("  PASS: LSP hover returns generic function type-parameter details\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP hover returns generic function type-parameter details\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_hover_json("tablo_tests/lsp_symbols_test.tblo", 4, 0, &error);
    if (json && !error && strcmp(json, "null") == 0) {
        tests_passed++;
        printf("  PASS: LSP hover returns null when no symbol is under the cursor\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP hover returns null when no symbol is under the cursor\n");
    }
    if (json) free(json);
    error_free(error);
}

static void test_lsp_definition(void) {
    Error* error = NULL;
    char* json = NULL;

    printf("Testing LSP definition...\n");

    json = lsp_build_definition_json("tablo_tests/lsp_symbols_test.tblo", 20, 11, &error);
    if (json && !error &&
        strstr(json, "\"uri\":\"file://") != NULL &&
        strstr(json, "\"start\":{\"line\":16,\"character\":6}") != NULL) {
        tests_passed++;
        printf("  PASS: LSP definition resolves top-level identifiers\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP definition resolves top-level identifiers\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_definition_json("tablo_tests/lsp_symbols_test.tblo", 24, 16, &error);
    if (json && !error &&
        strstr(json, "\"start\":{\"line\":1,\"character\":4}") != NULL) {
        tests_passed++;
        printf("  PASS: LSP definition resolves record field accesses\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP definition resolves record field accesses\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_definition_json("tablo_tests/lsp_symbols_test.tblo", 28, 17, &error);
    if (json && !error &&
        strstr(json, "\"start\":{\"line\":12,\"character\":4}") != NULL) {
        tests_passed++;
        printf("  PASS: LSP definition resolves enum member accesses\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP definition resolves enum member accesses\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_definition_json("tablo_tests/lsp_symbols_test.tblo", 36, 22, &error);
    if (json && !error &&
        strstr(json, "\"start\":{\"line\":35,\"character\":14}") != NULL) {
        tests_passed++;
        printf("  PASS: LSP definition resolves function parameter references\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP definition resolves function parameter references\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_definition_json("tablo_tests/lsp_symbols_test.tblo", 38, 20, &error);
    if (json && !error &&
        strstr(json, "\"start\":{\"line\":37,\"character\":8}") != NULL) {
        tests_passed++;
        printf("  PASS: LSP definition resolves local variable references\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP definition resolves local variable references\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_definition_json("tablo_tests/lsp_symbols_test.tblo", 41, 23, &error);
    if (json && !error &&
        strstr(json, "\"start\":{\"line\":9,\"character\":5}") != NULL) {
        tests_passed++;
        printf("  PASS: LSP definition resolves named type references in annotations\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP definition resolves named type references in annotations\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_definition_json("tablo_tests/lsp_symbols_test.tblo", 47, 11, &error);
    if (json && !error &&
        strstr(json, "\"start\":{\"line\":46,\"character\":11}") != NULL) {
        tests_passed++;
        printf("  PASS: LSP definition resolves generic record type-parameter references\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP definition resolves generic record type-parameter references\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_definition_json("tablo_tests/lsp_symbols_test.tblo", 50, 20, &error);
    if (json && !error &&
        strstr(json, "\"start\":{\"line\":50,\"character\":11}") != NULL) {
        tests_passed++;
        printf("  PASS: LSP definition resolves generic type-alias type-parameter references\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP definition resolves generic type-alias type-parameter references\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_definition_json("tablo_tests/lsp_symbols_test.tblo", 53, 9, &error);
    if (json && !error &&
        strstr(json, "\"start\":{\"line\":52,\"character\":13}") != NULL) {
        tests_passed++;
        printf("  PASS: LSP definition resolves generic enum type-parameter references\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP definition resolves generic enum type-parameter references\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_definition_json("tablo_tests/lsp_symbols_test.tblo", 58, 17, &error);
    if (json && !error &&
        strstr(json, "\"start\":{\"line\":57,\"character\":14}") != NULL) {
        tests_passed++;
        printf("  PASS: LSP definition resolves generic function type-parameter references\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP definition resolves generic function type-parameter references\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_definition_json("tablo_tests/lsp_symbols_test.tblo", 4, 0, &error);
    if (json && !error && strcmp(json, "null") == 0) {
        tests_passed++;
        printf("  PASS: LSP definition returns null when no symbol is under the cursor\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP definition returns null when no symbol is under the cursor\n");
    }
    if (json) free(json);
    error_free(error);
}

static void test_lsp_diagnostics(void) {
    Error* error = NULL;
    char* json = NULL;
    const char* parse_source =
        "func main(: void {\n"
        "    println(1);\n"
        "}\n";
    const char* type_source =
        "func main(): void {\n"
        "    var value: int = \"oops\";\n"
        "}\n";
    const char* clean_source =
        "func main(): void {\n"
        "    var value: int = 1;\n"
        "    println(value);\n"
        "}\n";

    printf("Testing LSP diagnostics...\n");

    json = lsp_build_diagnostics_json("tablo_tests/lsp_parse_diag_test.tblo", parse_source, &error);
    if (json && !error &&
        strstr(json, "\"severity\":1") != NULL &&
        strstr(json, "\"source\":\"tablo\"") != NULL &&
        strstr(json, "\"message\":") != NULL &&
        strcmp(json, "[]") != 0) {
        tests_passed++;
        printf("  PASS: LSP diagnostics returns parser diagnostics for invalid source text\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP diagnostics returns parser diagnostics for invalid source text\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_diagnostics_json("tablo_tests/lsp_type_diag_test.tblo", type_source, &error);
    if (json && !error &&
        strstr(json, "\"severity\":1") != NULL &&
        strstr(json, "string") != NULL &&
        strstr(json, "int") != NULL &&
        strcmp(json, "[]") != 0) {
        tests_passed++;
        printf("  PASS: LSP diagnostics returns typechecker diagnostics for invalid source text\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP diagnostics returns typechecker diagnostics for invalid source text\n");
    }
    if (json) free(json);
    error_free(error);

    error = NULL;
    json = lsp_build_diagnostics_json("tablo_tests/lsp_clean_diag_test.tblo", clean_source, &error);
    if (json && !error && strcmp(json, "[]") == 0) {
        tests_passed++;
        printf("  PASS: LSP diagnostics returns an empty array for clean source text\n");
    } else {
        tests_failed++;
        printf("  FAIL: LSP diagnostics returns an empty array for clean source text\n");
    }
    if (json) free(json);
    error_free(error);
}

int main(void) {
    printf("Running TabloLang tests...\n\n");

    test_value_creation();
    test_lexer();
    test_parser();
    test_constant_folding();
    test_compiler_jit_leaf_hints();
    test_bytecode();
    test_vm();
    test_vm_hash_table_resize();
    test_compiler_regressions();
    test_artifact_roundtrip();
    test_lsp_document_symbols();
    test_lsp_hover();
    test_lsp_definition();
    test_lsp_diagnostics();

    printf("\nTest Results:\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    fflush(stdout);

    return tests_failed > 0 ? 1 : 0;
}
