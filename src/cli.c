#include "cli.h"
#include "artifact.h"
#include "jit.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>

#ifndef TABLO_VERSION
#define TABLO_VERSION "dev"
#endif

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <sys/types.h>
#include <sys/stat.h>
typedef void* HANDLE;
typedef unsigned long DWORD;
#ifndef WAIT_OBJECT_0
#define WAIT_OBJECT_0 0UL
#endif
#ifndef WAIT_TIMEOUT
#define WAIT_TIMEOUT 258UL
#endif
#ifndef INFINITE
#define INFINITE 0xFFFFFFFFUL
#endif
__declspec(dllimport) DWORD __stdcall WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
__declspec(dllimport) int __stdcall TerminateProcess(HANDLE hProcess, unsigned int uExitCode);
__declspec(dllimport) int __stdcall GetExitCodeProcess(HANDLE hProcess, DWORD* lpExitCode);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE hObject);
__declspec(dllimport) void __stdcall Sleep(DWORD dwMilliseconds);
__declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#endif

typedef struct {
    uint8_t opcode;
    uint64_t count;
} OpcodeCount;

static int opcode_count_cmp_desc(const void* a, const void* b) {
    const OpcodeCount* aa = (const OpcodeCount*)a;
    const OpcodeCount* bb = (const OpcodeCount*)b;
    if (aa->count < bb->count) return 1;
    if (aa->count > bb->count) return -1;
    return (int)aa->opcode - (int)bb->opcode;
}

static void dump_opcode_profile(VM* vm) {
    if (!vm) return;

    OpcodeCount counts[256];
    uint64_t total = 0;
    for (int i = 0; i < 256; i++) {
        counts[i].opcode = (uint8_t)i;
        counts[i].count = vm->opcode_counts[i];
        total += vm->opcode_counts[i];
    }

    qsort(counts, 256, sizeof(counts[0]), opcode_count_cmp_desc);

    printf("\nOpcode profile (total=%" PRIu64 "):\n", total);
    int printed = 0;
    for (int i = 0; i < 256 && printed < 24; i++) {
        if (counts[i].count == 0) break;
        printf("  %-32s %" PRIu64 "\n",
               op_code_to_string((OpCode)counts[i].opcode),
               counts[i].count);
        printed++;
    }
}

static int read_i16_be(const uint8_t* code, int offset, int code_count, int16_t* out) {
    if (!code || !out) return 0;
    if (offset < 0 || (offset + 1) >= code_count) return 0;
    uint8_t hi = code[offset];
    uint8_t lo = code[offset + 1];
    *out = (int16_t)((hi << 8) | lo);
    return 1;
}

static int dump_chunk_disasm(Chunk* chunk) {
    if (!chunk || !chunk->code) return 0;

    int ip = 0;
    while (ip >= 0 && ip < chunk->code_count) {
        uint8_t opcode = chunk->code[ip];
        int line = -1;
        if (chunk->debug_info && ip < chunk->code_count) {
            line = chunk->debug_info[ip].line;
        }

        printf("%04d: %s", ip, op_code_to_string((OpCode)opcode));
        if (line >= 0) {
            printf(" (line %d)", line);
        }

        switch (opcode) {
            case OP_TYPE_TEST_INTERFACE_METHOD: {
                if ((ip + 4) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                unsigned interface_idx = ((unsigned)chunk->code[ip + 1] << 8) | (unsigned)chunk->code[ip + 2];
                unsigned method_idx = ((unsigned)chunk->code[ip + 3] << 8) | (unsigned)chunk->code[ip + 4];
                if (method_idx == 0xffffu) {
                    printf(" interface=%u method=<record-check>\n", interface_idx);
                } else {
                    printf(" interface=%u method=%u\n", interface_idx, method_idx);
                }
                ip += 5;
                break;
            }

            case OP_CALL_INTERFACE: {
                if ((ip + 5) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                unsigned interface_idx = ((unsigned)chunk->code[ip + 1] << 8) | (unsigned)chunk->code[ip + 2];
                unsigned method_idx = ((unsigned)chunk->code[ip + 3] << 8) | (unsigned)chunk->code[ip + 4];
                printf(" interface=%u method=%u argc=%u\n",
                       interface_idx,
                       method_idx,
                       (unsigned)chunk->code[ip + 5]);
                ip += 6;
                break;
            }

            case OP_CALL_GLOBAL: {
                if ((ip + 2) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" name=%u argc=%u\n", (unsigned)chunk->code[ip + 1], (unsigned)chunk->code[ip + 2]);
                ip += 3;
                break;
            }

            case OP_CALL_GLOBAL16: {
                if ((ip + 3) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                unsigned name_idx = ((unsigned)chunk->code[ip + 1] << 8) | (unsigned)chunk->code[ip + 2];
                printf(" name=%u argc=%u\n", name_idx, (unsigned)chunk->code[ip + 3]);
                ip += 4;
                break;
            }

            case OP_CALL_GLOBAL_SLOT: {
                if ((ip + 2) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" slot=%u argc=%u\n", (unsigned)chunk->code[ip + 1], (unsigned)chunk->code[ip + 2]);
                ip += 3;
                break;
            }

            case OP_LOAD_GLOBAL_SLOT: {
                if ((ip + 1) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" slot=%u\n", (unsigned)chunk->code[ip + 1]);
                ip += 2;
                break;
            }

            case OP_STORE_GLOBAL_SLOT: {
                if ((ip + 1) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" slot=%u\n", (unsigned)chunk->code[ip + 1]);
                ip += 2;
                break;
            }

            case OP_ADD_GLOBAL_GLOBAL_TO_GLOBAL:
            case OP_SUB_GLOBAL_GLOBAL_TO_GLOBAL:
            case OP_MUL_GLOBAL_GLOBAL_TO_GLOBAL:
            case OP_DIV_GLOBAL_GLOBAL_TO_GLOBAL: {
                if ((ip + 6) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" dst_name=%u a_name=%u b_name=%u\n",
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (unsigned)chunk->code[ip + 3]);
                ip += 7;
                break;
            }

            case OP_ADD_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
            case OP_SUB_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
            case OP_MUL_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT:
            case OP_DIV_GLOBAL_SLOT_SLOT_TO_GLOBAL_SLOT: {
                if ((ip + 6) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" dst_slot=%u a_slot=%u b_slot=%u\n",
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (unsigned)chunk->code[ip + 3]);
                ip += 7;
                break;
            }

            case OP_CONST16:
            case OP_LOAD_GLOBAL16:
            case OP_STORE_GLOBAL16: {
                if ((ip + 2) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                unsigned operand = ((unsigned)chunk->code[ip + 1] << 8) | (unsigned)chunk->code[ip + 2];
                printf(" %u\n", operand);
                ip += 3;
                break;
            }

            case OP_MAKE_CLOSURE: {
                if ((ip + 1) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" captures=%u\n", (unsigned)chunk->code[ip + 1]);
                ip += 2;
                break;
            }

            case OP_RECORD_NEW_NAMED: {
                if ((ip + 3) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                unsigned record_name_idx = ((unsigned)chunk->code[ip + 1] << 8) | (unsigned)chunk->code[ip + 2];
                printf(" name=%u fields=%u\n",
                       record_name_idx,
                       (unsigned)chunk->code[ip + 3]);
                ip += 4;
                break;
            }

            case OP_CONST:
            case OP_LOAD_LOCAL:
            case OP_STORE_LOCAL:
            case OP_NEGATE_LOCAL:
            case OP_LOAD_GLOBAL:
            case OP_STORE_GLOBAL:
            case OP_CALL:
            case OP_ARRAY_NEW:
            case OP_ARRAY_GET_LOCAL:
            case OP_ARRAY_SET_LOCAL:
            case OP_RECORD_NEW:
            case OP_RECORD_SET_FIELD:
            case OP_RECORD_GET_FIELD:
            case OP_TUPLE_NEW:
            case OP_TUPLE_GET:
            case OP_TUPLE_SET:
            case OP_ADD_LOCAL_STACK_INT:
            case OP_SUB_LOCAL_STACK_INT:
            case OP_ADD_LOCAL_STACK_DOUBLE:
            case OP_SUB_LOCAL_STACK_DOUBLE: {
                if ((ip + 1) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" %u\n", (unsigned)chunk->code[ip + 1]);
                ip += 2;
                break;
            }

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
            case OP_DIV_LOCALS_DOUBLE: {
                if ((ip + 4) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" a=%u b=%u\n", (unsigned)chunk->code[ip + 1], (unsigned)chunk->code[ip + 2]);
                ip += 5;
                break;
            }

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
            case OP_DIV_LOCAL_CONST_DOUBLE: {
                if ((ip + 4) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" slot=%u const=%u\n", (unsigned)chunk->code[ip + 1], (unsigned)chunk->code[ip + 2]);
                ip += 5;
                break;
            }

            case OP_SQRT_LOCAL_DOUBLE:
            case OP_ARRAY_LEN_LOCAL: {
                if ((ip + 2) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" %u\n", (unsigned)chunk->code[ip + 1]);
                ip += 3;
                break;
            }

            case OP_ADD_STACK_LOCAL_DOUBLE:
            case OP_SUB_STACK_LOCAL_DOUBLE:
            case OP_MUL_STACK_LOCAL_DOUBLE:
            case OP_DIV_STACK_LOCAL_DOUBLE:
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
            case OP_DIV_STACK_CONST_DOUBLE: {
                if ((ip + 2) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                if (opcode == OP_ADD_STACK_CONST_INT || opcode == OP_SUB_STACK_CONST_INT ||
                    opcode == OP_MUL_STACK_CONST_INT || opcode == OP_DIV_STACK_CONST_INT ||
                    opcode == OP_MOD_STACK_CONST_INT || opcode == OP_BIT_AND_STACK_CONST_INT ||
                    opcode == OP_BIT_OR_STACK_CONST_INT || opcode == OP_BIT_XOR_STACK_CONST_INT ||
                    opcode == OP_ADD_STACK_CONST_DOUBLE ||
                    opcode == OP_SUB_STACK_CONST_DOUBLE || opcode == OP_MUL_STACK_CONST_DOUBLE ||
                    opcode == OP_DIV_STACK_CONST_DOUBLE) {
                    printf(" const=%u\n", (unsigned)chunk->code[ip + 1]);
                } else {
                    printf(" %u\n", (unsigned)chunk->code[ip + 1]);
                }
                ip += 3;
                break;
            }

            case OP_ADD_LOCAL_CONST:
            case OP_SUB_LOCAL_CONST: {
                if ((ip + 2) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" slot=%u const=%u\n", (unsigned)chunk->code[ip + 1], (unsigned)chunk->code[ip + 2]);
                ip += 3;
                break;
            }

            case OP_ADD2_LOCAL_CONST: {
                if ((ip + 5) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" a=%u c1=%u b=%u c2=%u\n",
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (unsigned)chunk->code[ip + 3],
                       (unsigned)chunk->code[ip + 4]);
                ip += 6;
                break;
            }

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
            case OP_ARRAY_SET_LOCAL_LOCAL_DOUBLE: {
                if ((ip + 2) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                const char* fmt = " %u %u\n";
                if (opcode == OP_ARRAY_GET_LOCAL_CONST || opcode == OP_ARRAY_SET_LOCAL_CONST ||
                    opcode == OP_ARRAY_GET_LOCAL_CONST_INT || opcode == OP_ARRAY_SET_LOCAL_CONST_INT ||
                    opcode == OP_ARRAY_GET_LOCAL_CONST_DOUBLE || opcode == OP_ARRAY_SET_LOCAL_CONST_DOUBLE) {
                    fmt = " arr=%u idx=%u\n";
                } else {
                    fmt = " arr=%u slot=%u\n";
                }
                printf(fmt, (unsigned)chunk->code[ip + 1], (unsigned)chunk->code[ip + 2]);
                ip += 3;
                break;
            }

            case OP_ARRAY_GET_LOCAL_CONST_INT_TO_LOCAL:
            case OP_ARRAY_GET_LOCAL_LOCAL_INT_TO_LOCAL:
            case OP_ARRAY_GET_LOCAL_CONST_DOUBLE_TO_LOCAL:
            case OP_ARRAY_GET_LOCAL_LOCAL_DOUBLE_TO_LOCAL: {
                if ((ip + 4) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                const char* fmt = " arr=%u slot=%u dst=%u\n";
                if (opcode == OP_ARRAY_GET_LOCAL_CONST_INT_TO_LOCAL || opcode == OP_ARRAY_GET_LOCAL_CONST_DOUBLE_TO_LOCAL) {
                    fmt = " arr=%u idx=%u dst=%u\n";
                }
                printf(fmt,
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (unsigned)chunk->code[ip + 3]);
                ip += 5;
                break;
            }

            case OP_MUL_LOCALS_INT_TO_LOCAL:
            case OP_MUL_LOCALS_DOUBLE_TO_LOCAL: {
                if ((ip + 6) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" dst=%u a=%u b=%u\n",
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (unsigned)chunk->code[ip + 3]);
                ip += 7;
                break;
            }

            case OP_ARRAY_BOUNDS_CHECK_LOCAL_CONST:
            case OP_ARRAY_BOUNDS_CHECK_LOCAL_LOCAL: {
                if ((ip + 2) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                const char* fmt = (opcode == OP_ARRAY_BOUNDS_CHECK_LOCAL_CONST) ? " arr=%u idx=%u\n" : " arr=%u slot=%u\n";
                printf(fmt, (unsigned)chunk->code[ip + 1], (unsigned)chunk->code[ip + 2]);
                ip += 3;
                break;
            }

            case OP_ARRAY_COPY_LOCAL_LOCAL: {
                if ((ip + 2) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" dst=%u src=%u\n", (unsigned)chunk->code[ip + 1], (unsigned)chunk->code[ip + 2]);
                ip += 3;
                break;
            }

            case OP_ARRAY_REVERSE_PREFIX_LOCAL_LOCAL:
            case OP_ARRAY_ROTATE_PREFIX_LEFT_LOCAL_LOCAL: {
                if ((ip + 2) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" arr=%u hi=%u\n", (unsigned)chunk->code[ip + 1], (unsigned)chunk->code[ip + 2]);
                ip += 3;
                break;
            }

            case OP_ARRAY_ROTATE_PREFIX_RIGHT_LOCAL_LOCAL: {
                if ((ip + 2) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" arr=%u hi=%u\n", (unsigned)chunk->code[ip + 1], (unsigned)chunk->code[ip + 2]);
                ip += 3;
                break;
            }

            case OP_ADD_LOCAL_DIV_LOCALS: {
                if ((ip + 3) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" dst=%u num=%u den=%u\n",
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (unsigned)chunk->code[ip + 3]);
                ip += 4;
                break;
            }

            case OP_MADD_LOCAL_ARRAY_LOCAL_DOUBLE: {
                if ((ip + 3) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" acc=%u arr=%u idx=%u\n",
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (unsigned)chunk->code[ip + 3]);
                ip += 4;
                break;
            }

            case OP_MADD_LOCAL_ARRAY_LOCAL_INT: {
                if ((ip + 3) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" acc=%u arr=%u idx=%u\n",
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (unsigned)chunk->code[ip + 3]);
                ip += 4;
                break;
            }

            case OP_EVALA_RECIP_LOCALS_DOUBLE: {
                if ((ip + 2) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" a=%u b=%u\n", (unsigned)chunk->code[ip + 1], (unsigned)chunk->code[ip + 2]);
                ip += 3;
                break;
            }

            case OP_EVALA_MADD_LOCAL_ARRAY_LOCAL_DOUBLE: {
                if ((ip + 6) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" acc=%u arr=%u idx=%u a=%u b=%u\n",
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (unsigned)chunk->code[ip + 3],
                       (unsigned)chunk->code[ip + 4],
                       (unsigned)chunk->code[ip + 5]);
                ip += 7;
                break;
            }

            case OP_ARRAY_GET_FIELD_LOCAL_CONST:
            case OP_ARRAY_GET_FIELD_LOCAL_LOCAL:
            case OP_ARRAY_SET_FIELD_LOCAL_CONST:
            case OP_ARRAY_SET_FIELD_LOCAL_LOCAL: {
                if ((ip + 3) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                printf(" arr=%u idx=%u field=%u\n",
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (unsigned)chunk->code[ip + 3]);
                ip += 4;
                break;
            }

            case OP_JUMP:
            case OP_JUMP_IF_FALSE: {
                int16_t off = 0;
                if (!read_i16_be(chunk->code, ip + 1, chunk->code_count, &off)) {
                    printf(" <truncated>\n");
                    return 0;
                }
                int target = (ip + 3) + off;
                printf(" off=%d -> %d\n", (int)off, target);
                ip += 3;
                break;
            }

            case OP_JUMP_IF_FALSE_POP: {
                if ((ip + 3) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                int16_t off = 0;
                if (!read_i16_be(chunk->code, ip + 1, chunk->code_count, &off)) {
                    printf(" <truncated>\n");
                    return 0;
                }
                int target = (ip + 3) + off;
                printf(" off=%d -> %d\n", (int)off, target);
                ip += 4;
                break;
            }

            case OP_JUMP_IF_LOCAL_LT:
            case OP_JUMP_IF_LOCAL_LE:
            case OP_JUMP_IF_LOCAL_GT:
            case OP_JUMP_IF_LOCAL_GE:
            case OP_JUMP_IF_LOCAL_EQ:
            case OP_JUMP_IF_LOCAL_NE: {
                if ((ip + 4) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                int16_t off = 0;
                if (!read_i16_be(chunk->code, ip + 3, chunk->code_count, &off)) {
                    printf(" <truncated>\n");
                    return 0;
                }
                int target = (ip + 5) + off;
                printf(" a=%u b=%u off=%d -> %d\n",
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (int)off,
                       target);
                ip += 5;
                break;
            }

            case OP_JUMP_IF_LOCAL_LT_CONST:
            case OP_JUMP_IF_LOCAL_LE_CONST:
            case OP_JUMP_IF_LOCAL_GT_CONST:
            case OP_JUMP_IF_LOCAL_GE_CONST:
            case OP_JUMP_IF_LOCAL_EQ_CONST:
            case OP_JUMP_IF_LOCAL_NE_CONST: {
                if ((ip + 4) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                int16_t off = 0;
                if (!read_i16_be(chunk->code, ip + 3, chunk->code_count, &off)) {
                    printf(" <truncated>\n");
                    return 0;
                }
                int target = (ip + 5) + off;
                printf(" a=%u c=%u off=%d -> %d\n",
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (int)off,
                       target);
                ip += 5;
                break;
            }

            case OP_JUMP_IF_LOCAL_EQ_GLOBAL:
            case OP_JUMP_IF_LOCAL_NE_GLOBAL:
            case OP_JUMP_IF_LOCAL_EQ_GLOBAL_SLOT:
            case OP_JUMP_IF_LOCAL_NE_GLOBAL_SLOT: {
                if ((ip + 4) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                int16_t off = 0;
                if (!read_i16_be(chunk->code, ip + 3, chunk->code_count, &off)) {
                    printf(" <truncated>\n");
                    return 0;
                }
                int target = (ip + 5) + off;
                printf(" a=%u g=%u off=%d -> %d\n",
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (int)off,
                       target);
                ip += 5;
                break;
            }

            case OP_JUMP_IF_ARRAY_FALSE_LOCAL_CONST:
            case OP_JUMP_IF_ARRAY_FALSE_LOCAL_LOCAL: {
                if ((ip + 4) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                int16_t off = 0;
                if (!read_i16_be(chunk->code, ip + 3, chunk->code_count, &off)) {
                    printf(" <truncated>\n");
                    return 0;
                }
                int target = (ip + 5) + off;
                const char* fmt = (opcode == OP_JUMP_IF_ARRAY_FALSE_LOCAL_CONST) ? " arr=%u idx=%u off=%d -> %d\n" : " arr=%u slot=%u off=%d -> %d\n";
                printf(fmt,
                       (unsigned)chunk->code[ip + 1],
                       (unsigned)chunk->code[ip + 2],
                       (int)off,
                       target);
                ip += 5;
                break;
            }

            case OP_JUMP_IF_STACK_LT_LOCAL:
            case OP_JUMP_IF_STACK_LE_LOCAL:
            case OP_JUMP_IF_STACK_GT_LOCAL:
            case OP_JUMP_IF_STACK_GE_LOCAL:
            case OP_JUMP_IF_STACK_LT_CONST:
            case OP_JUMP_IF_STACK_LE_CONST:
            case OP_JUMP_IF_STACK_GT_CONST:
            case OP_JUMP_IF_STACK_GE_CONST: {
                if ((ip + 3) >= chunk->code_count) {
                    printf(" <truncated>\n");
                    return 0;
                }
                int16_t off = 0;
                if (!read_i16_be(chunk->code, ip + 2, chunk->code_count, &off)) {
                    printf(" <truncated>\n");
                    return 0;
                }
                int target = (ip + 4) + off;
                const char* kind = "b";
                if (opcode == OP_JUMP_IF_STACK_LT_CONST || opcode == OP_JUMP_IF_STACK_LE_CONST ||
                    opcode == OP_JUMP_IF_STACK_GT_CONST || opcode == OP_JUMP_IF_STACK_GE_CONST) {
                    kind = "c";
                }
                printf(" %s=%u off=%d -> %d\n", kind, (unsigned)chunk->code[ip + 1], (int)off, target);
                ip += 4;
                break;
            }

            default:
                printf("\n");
                ip += 1;
                break;
        }
    }

    return 1;
}

typedef struct {
    char** items;
    int count;
    int capacity;
} StringList;

static char* cli_strdup(const char* text) {
    if (!text) return NULL;
    size_t len = strlen(text);
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, text, len + 1);
    return out;
}

static void string_list_init(StringList* list) {
    if (!list) return;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void string_list_free(StringList* list) {
    if (!list) return;
    if (list->items) {
        for (int i = 0; i < list->count; i++) {
            free(list->items[i]);
        }
        free(list->items);
    }
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int string_list_contains(const StringList* list, const char* value) {
    if (!list || !value) return 0;
    for (int i = 0; i < list->count; i++) {
        if (list->items[i] && strcmp(list->items[i], value) == 0) {
            return 1;
        }
    }
    return 0;
}

static int string_list_push_unique(StringList* list, const char* value) {
    if (!list || !value) return 0;
    if (string_list_contains(list, value)) return 1;

    if (list->count >= list->capacity) {
        int new_capacity = (list->capacity <= 0) ? 16 : (list->capacity * 2);
        char** resized = (char**)realloc(list->items, (size_t)new_capacity * sizeof(char*));
        if (!resized) return 0;
        list->items = resized;
        list->capacity = new_capacity;
    }

    char* copy = cli_strdup(value);
    if (!copy) return 0;

    list->items[list->count++] = copy;
    return 1;
}

static int cli_parse_extension_option(int argc,
                                      char** argv,
                                      int* io_index,
                                      StringList* extension_paths,
                                      const char** out_error) {
    if (!argv || !io_index || !extension_paths) return 0;
    const char* arg = argv[*io_index];
    if (!arg) return 0;

    const char* value = NULL;
    if (strcmp(arg, "--ext") == 0) {
        if ((*io_index + 1) >= argc) {
            if (out_error) *out_error = "Error: --ext requires a shared library path";
            return -1;
        }
        value = argv[++(*io_index)];
    } else if (strncmp(arg, "--ext=", 6) == 0) {
        value = arg + 6;
        if (!value[0]) {
            if (out_error) *out_error = "Error: --ext requires a shared library path";
            return -1;
        }
    } else {
        return 0;
    }

    if (!string_list_push_unique(extension_paths, value)) {
        if (out_error) *out_error = "Error: out of memory while storing --ext";
        return -1;
    }
    return 1;
}

static int string_ptr_cmp(const void* a, const void* b) {
    const char* const* aa = (const char* const*)a;
    const char* const* bb = (const char* const*)b;
    if (!aa || !bb) return 0;
    if (!*aa && !*bb) return 0;
    if (!*aa) return -1;
    if (!*bb) return 1;
    return strcmp(*aa, *bb);
}

static void string_list_sort(StringList* list) {
    if (!list || list->count <= 1) return;
    qsort(list->items, (size_t)list->count, sizeof(char*), string_ptr_cmp);
}

typedef struct {
    char* file_path;
    char* function_name;
    char* id;
    char* discovery_error;
    int legacy_main;
} CliTestCase;

typedef struct {
    CliTestCase* items;
    int count;
    int capacity;
} CliTestCaseList;

typedef enum {
    CLI_TEST_RUN_PASS = 0,
    CLI_TEST_RUN_FAIL = 1,
    CLI_TEST_RUN_TIMEOUT = 2,
    CLI_TEST_RUN_CRASH = 3,
    CLI_TEST_RUN_SKIPPED = 4
} CliTestRunStatus;

typedef struct {
    char* id;
    char* file_path;
    char* function_name;
    CliTestRunStatus status;
    int test_index;
    int child_exit_code;
    double elapsed_ms;
    char* error_text;
} CliRunResult;

typedef struct {
    CliRunResult* items;
    int count;
    int capacity;
} CliRunResultList;

typedef struct {
    int active;
    const CliTestCase* test_case;
    char* result_file;
    double start_ms;
#ifdef _WIN32
    HANDLE process_handle;
#else
    pid_t pid;
#endif
} CliChildProcess;

static char* cli_build_test_id(const char* file_path, const char* function_name) {
    if (!file_path || !function_name) return NULL;
    size_t file_len = strlen(file_path);
    size_t fn_len = strlen(function_name);
    size_t total = file_len + 2 + fn_len + 1;
    char* out = (char*)malloc(total);
    if (!out) return NULL;
    snprintf(out, total, "%s::%s", file_path, function_name);
    out[total - 1] = '\0';
    return out;
}

static void cli_test_case_list_init(CliTestCaseList* list) {
    if (!list) return;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void cli_test_case_list_free(CliTestCaseList* list) {
    if (!list) return;
    if (list->items) {
        for (int i = 0; i < list->count; i++) {
            CliTestCase* item = &list->items[i];
            free(item->file_path);
            free(item->function_name);
            free(item->id);
            free(item->discovery_error);
        }
        free(list->items);
    }
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int cli_test_case_list_push(CliTestCaseList* list,
                                   const char* file_path,
                                   const char* function_name,
                                   const char* discovery_error,
                                   int legacy_main) {
    if (!list || !file_path || !function_name) return 0;

    char* id = cli_build_test_id(file_path, function_name);
    if (!id) return 0;

    for (int i = 0; i < list->count; i++) {
        if (list->items[i].id && strcmp(list->items[i].id, id) == 0) {
            free(id);
            return 1;
        }
    }

    if (list->count >= list->capacity) {
        int new_capacity = (list->capacity <= 0) ? 16 : (list->capacity * 2);
        CliTestCase* resized = (CliTestCase*)realloc(list->items, (size_t)new_capacity * sizeof(CliTestCase));
        if (!resized) {
            free(id);
            return 0;
        }
        list->items = resized;
        list->capacity = new_capacity;
    }

    CliTestCase item;
    memset(&item, 0, sizeof(item));
    item.file_path = cli_strdup(file_path);
    item.function_name = cli_strdup(function_name);
    item.id = id;
    item.discovery_error = discovery_error ? cli_strdup(discovery_error) : NULL;
    item.legacy_main = legacy_main;

    if (!item.file_path || !item.function_name || !item.id ||
        (discovery_error && !item.discovery_error)) {
        free(item.file_path);
        free(item.function_name);
        free(item.id);
        free(item.discovery_error);
        return 0;
    }

    list->items[list->count++] = item;
    return 1;
}

static void cli_run_result_list_init(CliRunResultList* list) {
    if (!list) return;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void cli_run_result_list_free(CliRunResultList* list) {
    if (!list) return;
    if (list->items) {
        for (int i = 0; i < list->count; i++) {
            CliRunResult* item = &list->items[i];
            free(item->id);
            free(item->file_path);
            free(item->function_name);
            free(item->error_text);
        }
        free(list->items);
    }
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int cli_run_result_list_push(CliRunResultList* list,
                                    const char* id,
                                    const char* file_path,
                                    const char* function_name,
                                    CliTestRunStatus status,
                                    int test_index,
                                    int child_exit_code,
                                    double elapsed_ms,
                                    const char* error_text) {
    if (!list || !id || !file_path || !function_name) return 0;

    if (list->count >= list->capacity) {
        int new_capacity = (list->capacity <= 0) ? 16 : (list->capacity * 2);
        CliRunResult* resized = (CliRunResult*)realloc(list->items, (size_t)new_capacity * sizeof(CliRunResult));
        if (!resized) return 0;
        list->items = resized;
        list->capacity = new_capacity;
    }

    CliRunResult item;
    memset(&item, 0, sizeof(item));
    item.id = cli_strdup(id);
    item.file_path = cli_strdup(file_path);
    item.function_name = cli_strdup(function_name);
    item.status = status;
    item.test_index = test_index;
    item.child_exit_code = child_exit_code;
    item.elapsed_ms = elapsed_ms;
    item.error_text = (error_text && error_text[0] != '\0') ? cli_strdup(error_text) : NULL;

    if (!item.id || !item.file_path || !item.function_name ||
        ((error_text && error_text[0] != '\0') && !item.error_text)) {
        free(item.id);
        free(item.file_path);
        free(item.function_name);
        free(item.error_text);
        return 0;
    }

    list->items[list->count++] = item;
    return 1;
}

static int cli_test_case_matches_pattern(const CliTestCase* test_case, const char* pattern) {
    if (!test_case) return 0;
    if (!pattern || pattern[0] == '\0') return 1;
    if (test_case->id && strstr(test_case->id, pattern) != NULL) return 1;
    if (test_case->file_path && strstr(test_case->file_path, pattern) != NULL) return 1;
    if (test_case->function_name && strstr(test_case->function_name, pattern) != NULL) return 1;
    return 0;
}

static uint64_t cli_hash_string64(const char* text) {
    if (!text) return 0;
    uint64_t h = 1469598103934665603ull; // FNV-1a 64-bit offset basis
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        h ^= (uint64_t)(*p);
        h *= 1099511628211ull; // FNV prime
    }
    return h;
}

static void cli_json_write_string(const char* text) {
    if (!text) text = "";
    putchar('"');
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        unsigned char c = *p;
        switch (c) {
            case '\"': printf("\\\""); break;
            case '\\': printf("\\\\"); break;
            case '\b': printf("\\b"); break;
            case '\f': printf("\\f"); break;
            case '\n': printf("\\n"); break;
            case '\r': printf("\\r"); break;
            case '\t': printf("\\t"); break;
            default:
                if (c < 0x20) {
                    printf("\\u%04x", (unsigned)c);
                } else {
                    putchar((int)c);
                }
                break;
        }
    }
    putchar('"');
}

static void cli_xml_write_escaped(FILE* out, const char* text) {
    if (!out) return;
    if (!text) text = "";
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        unsigned char c = *p;
        switch (c) {
            case '&': fputs("&amp;", out); break;
            case '<': fputs("&lt;", out); break;
            case '>': fputs("&gt;", out); break;
            case '"': fputs("&quot;", out); break;
            case '\'': fputs("&apos;", out); break;
            default:
                if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
                    fprintf(out, "&#x%02X;", (unsigned)c);
                } else {
                    fputc((int)c, out);
                }
                break;
        }
    }
}

static int cli_write_junit_report(const char* junit_path,
                                  const CliTestCaseList* selected_cases,
                                  const CliRunResultList* run_results,
                                  const int* final_last_idx,
                                  const int* attempt_counts,
                                  double total_ms,
                                  int rerun_failed,
                                  int shard_index,
                                  int shard_total) {
    if (!junit_path || !selected_cases || !run_results || !final_last_idx || !attempt_counts) return 0;

    FILE* out = fopen(junit_path, "wb");
    if (!out) return 0;

    int tests = selected_cases->count;
    int failures = 0;
    int errors = 0;
    int skipped = 0;

    for (int i = 0; i < selected_cases->count; i++) {
        int ridx = final_last_idx[i];
        if (ridx < 0 || ridx >= run_results->count) {
            skipped++;
            continue;
        }
        CliTestRunStatus status = run_results->items[ridx].status;
        if (status == CLI_TEST_RUN_FAIL) failures++;
        else if (status == CLI_TEST_RUN_TIMEOUT || status == CLI_TEST_RUN_CRASH) errors++;
    }

    fprintf(out, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(out, "<testsuites>\n");
    fprintf(out,
            "  <testsuite name=\"tablo.test\" tests=\"%d\" failures=\"%d\" errors=\"%d\" skipped=\"%d\" time=\"%.6f\">\n",
            tests,
            failures,
            errors,
            skipped,
            total_ms / 1000.0);
    fprintf(out, "    <properties>\n");
    fprintf(out, "      <property name=\"rerunFailed\" value=\"%d\"/>\n", rerun_failed);
    fprintf(out, "      <property name=\"shard\" value=\"%d/%d\"/>\n", shard_index, shard_total);
    fprintf(out, "    </properties>\n");

    for (int i = 0; i < selected_cases->count; i++) {
        const CliTestCase* test_case = &selected_cases->items[i];
        int ridx = final_last_idx[i];
        int attempts = attempt_counts[i];
        if (attempts < 0) attempts = 0;

        fprintf(out, "    <testcase classname=\"");
        cli_xml_write_escaped(out, test_case->file_path);
        fprintf(out, "\" name=\"");
        cli_xml_write_escaped(out, test_case->function_name);
        if (ridx >= 0 && ridx < run_results->count) {
            const CliRunResult* r = &run_results->items[ridx];
            fprintf(out, "\" time=\"%.6f\">\n", r->elapsed_ms / 1000.0);

            if (r->status == CLI_TEST_RUN_FAIL) {
                fprintf(out, "      <failure message=\"");
                cli_xml_write_escaped(out, r->error_text && r->error_text[0] ? r->error_text : "test failed");
                fprintf(out, "\">");
                cli_xml_write_escaped(out, r->error_text && r->error_text[0] ? r->error_text : "test failed");
                fprintf(out, "</failure>\n");
            } else if (r->status == CLI_TEST_RUN_TIMEOUT || r->status == CLI_TEST_RUN_CRASH) {
                fprintf(out, "      <error type=\"%s\" message=\"",
                        r->status == CLI_TEST_RUN_TIMEOUT ? "timeout" : "crash");
                cli_xml_write_escaped(out, r->error_text && r->error_text[0] ? r->error_text : "test process error");
                fprintf(out, "\">");
                cli_xml_write_escaped(out, r->error_text && r->error_text[0] ? r->error_text : "test process error");
                fprintf(out, "</error>\n");
            }

            fprintf(out, "      <system-out>attempts=%d exitCode=%d</system-out>\n", attempts, r->child_exit_code);
            fprintf(out, "    </testcase>\n");
        } else {
            fprintf(out, "\" time=\"0.000000\">\n");
            fprintf(out, "      <skipped message=\"Not executed\"/>\n");
            fprintf(out, "      <system-out>attempts=0 exitCode=0</system-out>\n");
            fprintf(out, "    </testcase>\n");
        }
    }

    fprintf(out, "  </testsuite>\n");
    fprintf(out, "</testsuites>\n");
    fclose(out);
    return 1;
}

static int cli_has_suffix(const char* text, const char* suffix) {
    if (!text || !suffix) return 0;
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > text_len) return 0;
    return strcmp(text + (text_len - suffix_len), suffix) == 0;
}

static int cli_path_is_directory(const char* path) {
    if (!path || path[0] == '\0') return 0;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return 0;
    return (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
#endif
}

static int cli_path_is_file(const char* path) {
    if (!path || path[0] == '\0') return 0;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return 0;
    return (st.st_mode & _S_IFREG) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode);
#endif
}

static int cli_is_tablo_test_filename(const char* path) {
    return cli_has_suffix(path, "_test.tblo");
}

static int cli_is_tablo_source_filename(const char* path) {
    return cli_has_suffix(path, ".tblo");
}

static char* cli_path_join(const char* base, const char* leaf) {
    if (!base || !leaf) return NULL;
    size_t base_len = strlen(base);
    size_t leaf_len = strlen(leaf);

    int need_sep = 1;
    if (base_len == 0) {
        need_sep = 0;
    } else {
        char last = base[base_len - 1];
        if (last == '/' || last == '\\') {
            need_sep = 0;
        }
    }

    size_t total = base_len + (need_sep ? 1 : 0) + leaf_len + 1;
    char* out = (char*)malloc(total);
    if (!out) return NULL;

    size_t pos = 0;
    if (base_len > 0) {
        memcpy(out + pos, base, base_len);
        pos += base_len;
    }
    if (need_sep) {
#ifdef _WIN32
        out[pos++] = '\\';
#else
        out[pos++] = '/';
#endif
    }
    if (leaf_len > 0) {
        memcpy(out + pos, leaf, leaf_len);
        pos += leaf_len;
    }
    out[pos] = '\0';
    return out;
}

static double cli_now_ms(void) {
#ifdef _WIN32
    return (double)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec * 1000.0 + ((double)ts.tv_nsec / 1000000.0);
#endif
}

static void cli_sleep_ms(int ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000u);
#endif
}

static const char* cli_test_status_to_text(CliTestRunStatus status) {
    switch (status) {
        case CLI_TEST_RUN_PASS: return "pass";
        case CLI_TEST_RUN_FAIL: return "fail";
        case CLI_TEST_RUN_TIMEOUT: return "timeout";
        case CLI_TEST_RUN_CRASH: return "crash";
        case CLI_TEST_RUN_SKIPPED: return "skipped";
        default: return "fail";
    }
}

static const char* cli_test_status_to_label(CliTestRunStatus status) {
    switch (status) {
        case CLI_TEST_RUN_PASS: return "PASS";
        case CLI_TEST_RUN_FAIL: return "FAIL";
        case CLI_TEST_RUN_TIMEOUT: return "TIMEOUT";
        case CLI_TEST_RUN_CRASH: return "CRASH";
        case CLI_TEST_RUN_SKIPPED: return "SKIPPED";
        default: return "FAIL";
    }
}

static char* cli_get_temp_dir(void) {
#ifdef _WIN32
    const char* env_tmp = getenv("TEMP");
    if (!env_tmp || env_tmp[0] == '\0') env_tmp = getenv("TMP");
    if (!env_tmp || env_tmp[0] == '\0') env_tmp = ".";
    return cli_strdup(env_tmp);
#else
    const char* env_tmp = getenv("TMPDIR");
    if (!env_tmp || env_tmp[0] == '\0') env_tmp = "/tmp";
    return cli_strdup(env_tmp);
#endif
}

static char* cli_make_result_file_path(const char* seed) {
    char* temp_dir = cli_get_temp_dir();
    if (!temp_dir) return NULL;

#ifdef _WIN32
    int pid = _getpid();
#else
    int pid = (int)getpid();
#endif
    long long stamp = (long long)time(NULL);
    int rnd = rand() & 0x7fffffff;

    char leaf[256];
    snprintf(leaf,
             sizeof(leaf),
             "tablo_test_result_%d_%lld_%d_%s.tmp",
             pid,
             stamp,
             rnd,
             seed ? seed : "case");
    leaf[sizeof(leaf) - 1] = '\0';

    char* full = cli_path_join(temp_dir, leaf);
    free(temp_dir);
    return full;
}

static int cli_write_child_result_file(const char* result_file, int ok, const char* error_text) {
    if (!result_file) return 0;
    FILE* f = fopen(result_file, "wb");
    if (!f) return 0;

    fputc(ok ? '1' : '0', f);
    fputc('\n', f);
    if (!ok && error_text && error_text[0] != '\0') {
        fwrite(error_text, 1, strlen(error_text), f);
    }
    fclose(f);
    return 1;
}

static int cli_read_child_result_file(const char* result_file,
                                      int* ok,
                                      char* error_text,
                                      size_t error_text_size) {
    if (!result_file || !ok) return 0;
    FILE* f = fopen(result_file, "rb");
    if (!f) return 0;

    int first = fgetc(f);
    int second = fgetc(f);
    if ((first != '0' && first != '1') || second != '\n') {
        fclose(f);
        return 0;
    }

    *ok = (first == '1') ? 1 : 0;
    if (error_text && error_text_size > 0) {
        size_t read_len = fread(error_text, 1, error_text_size - 1, f);
        error_text[read_len] = '\0';
    } else {
        char sink[256];
        while (fread(sink, 1, sizeof(sink), f) > 0) {
        }
    }
    fclose(f);
    return 1;
}

static void cli_child_process_init(CliChildProcess* child) {
    if (!child) return;
    memset(child, 0, sizeof(*child));
}

static void cli_child_process_free(CliChildProcess* child) {
    if (!child) return;
    if (child->result_file) {
        remove(child->result_file);
        free(child->result_file);
        child->result_file = NULL;
    }
#ifdef _WIN32
    if (child->process_handle) {
        CloseHandle(child->process_handle);
        child->process_handle = NULL;
    }
#endif
    child->test_case = NULL;
    child->active = 0;
}

static void cli_set_error_text(char* dst, size_t dst_size, const char* message) {
    if (!dst || dst_size == 0) return;
    if (!message) message = "";
    snprintf(dst, dst_size, "%s", message);
    dst[dst_size - 1] = '\0';
}

static int cli_collect_tests_from_dir(const char* dir_path, StringList* files, int* scan_errors) {
    if (!dir_path || !files) return 0;

#ifdef _WIN32
    char* pattern = cli_path_join(dir_path, "*");
    if (!pattern) {
        if (scan_errors) (*scan_errors)++;
        return 0;
    }

    struct _finddata_t find_data;
    intptr_t h = _findfirst(pattern, &find_data);
    free(pattern);
    if (h == -1) {
        if (scan_errors) (*scan_errors)++;
        fprintf(stderr, "Warning: Failed to read directory '%s'\n", dir_path);
        return 0;
    }

    int ok = 1;
    do {
        const char* name = find_data.name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char* child = cli_path_join(dir_path, name);
        if (!child) {
            ok = 0;
            if (scan_errors) (*scan_errors)++;
            continue;
        }

        if ((find_data.attrib & _A_SUBDIR) != 0) {
            if (!cli_collect_tests_from_dir(child, files, scan_errors)) {
                ok = 0;
            }
        } else if (cli_is_tablo_test_filename(name)) {
            if (!string_list_push_unique(files, child)) {
                ok = 0;
                if (scan_errors) (*scan_errors)++;
            }
        }

        free(child);
    } while (_findnext(h, &find_data) == 0);

    _findclose(h);
    return ok;
#else
    DIR* dir = opendir(dir_path);
    if (!dir) {
        if (scan_errors) (*scan_errors)++;
        fprintf(stderr, "Warning: Failed to read directory '%s': %s\n", dir_path, strerror(errno));
        return 0;
    }

    int ok = 1;
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char* child = cli_path_join(dir_path, name);
        if (!child) {
            ok = 0;
            if (scan_errors) (*scan_errors)++;
            continue;
        }

        struct stat st;
        if (stat(child, &st) != 0) {
            ok = 0;
            if (scan_errors) (*scan_errors)++;
            free(child);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!cli_collect_tests_from_dir(child, files, scan_errors)) {
                ok = 0;
            }
        } else if (S_ISREG(st.st_mode) && cli_is_tablo_test_filename(name)) {
            if (!string_list_push_unique(files, child)) {
                ok = 0;
                if (scan_errors) (*scan_errors)++;
            }
        }

        free(child);
    }

    closedir(dir);
    return ok;
#endif
}

static void cli_collect_tests_from_target(const char* target,
                                          StringList* files,
                                          int* scan_errors,
                                          int* skipped_non_tablo) {
    if (!target || !files) return;

    if (cli_path_is_directory(target)) {
        cli_collect_tests_from_dir(target, files, scan_errors);
        return;
    }

    if (cli_path_is_file(target)) {
        if (cli_is_tablo_source_filename(target)) {
            if (!string_list_push_unique(files, target)) {
                if (scan_errors) (*scan_errors)++;
            }
        } else {
            if (skipped_non_tablo) (*skipped_non_tablo)++;
            fprintf(stderr, "Warning: skipping non-TabloLang source file '%s'\n", target);
        }
        return;
    }

    if (scan_errors) (*scan_errors)++;
    fprintf(stderr, "Warning: Path not found '%s'\n", target);
}

static int cli_is_test_function_name(const char* function_name) {
    if (!function_name) return 0;
    return strncmp(function_name, "test", 4) == 0;
}

static char* cli_normalize_path_for_compare(const char* path) {
    if (!path) return NULL;
    size_t len = strlen(path);
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;

    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        char c = path[i];
        if (c == '\\' || c == '/') c = '/';
#ifdef _WIN32
        c = (char)tolower((unsigned char)c);
#endif
        out[pos++] = c;
    }
    out[pos] = '\0';

    while (pos > 1 && out[pos - 1] == '/') {
#ifdef _WIN32
        if (pos == 3 && out[1] == ':' && out[2] == '/') break;
#endif
        out[--pos] = '\0';
    }

    while (out[0] == '.' && out[1] == '/') {
        memmove(out, out + 2, strlen(out + 2) + 1);
    }

    return out;
}

static int cli_path_is_suffix_component(const char* text, const char* suffix) {
    if (!text || !suffix) return 0;
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > text_len) return 0;
    if (strcmp(text + (text_len - suffix_len), suffix) != 0) return 0;
    if (suffix_len == text_len) return 1;
    return text[text_len - suffix_len - 1] == '/';
}

static int cli_paths_match_loose(const char* a, const char* b) {
    if (!a || !b) return 0;
    char* aa = cli_normalize_path_for_compare(a);
    char* bb = cli_normalize_path_for_compare(b);
    if (!aa || !bb) {
        free(aa);
        free(bb);
        return 0;
    }

    int match = 0;
    if (strcmp(aa, bb) == 0) {
        match = 1;
    } else if (cli_path_is_suffix_component(aa, bb) || cli_path_is_suffix_component(bb, aa)) {
        match = 1;
    }

    free(aa);
    free(bb);
    return match;
}

static int cli_discover_test_functions(const char* path,
                                       RuntimeOptions options,
                                       int allow_legacy_main,
                                       StringList* function_names,
                                       int* used_legacy_main,
                                       int* skipped_bad_signature,
                                       char* error_text,
                                       size_t error_text_size) {
    if (!path || !function_names) {
        cli_set_error_text(error_text, error_text_size, "Invalid test discovery input");
        return 0;
    }
    if (used_legacy_main) *used_legacy_main = 0;
    if (skipped_bad_signature) *skipped_bad_signature = 0;

    Runtime* rt = runtime_create_with_options(path, options);
    if (!rt) {
        cli_set_error_text(error_text, error_text_size, "Failed to create runtime");
        return 0;
    }

    int missing_main_only = 0;
    if (runtime_has_error(rt)) {
        const char* runtime_error = runtime_get_error(rt);
        missing_main_only = runtime_error != NULL &&
                            strcmp(runtime_error, "No main() function found") == 0;
    }

    if (runtime_has_error(rt) && !missing_main_only) {
        cli_set_error_text(error_text, error_text_size, runtime_get_error(rt));
        runtime_free(rt);
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < rt->function_count; i++) {
        ObjFunction* func = rt->functions[i];
        if (!func || !func->name) continue;
        if (!cli_is_test_function_name(func->name)) continue;
        if (!cli_paths_match_loose(func->source_file, path)) continue;

        if (func->param_count != 0) {
            if (skipped_bad_signature) (*skipped_bad_signature)++;
            continue;
        }

        if (!string_list_push_unique(function_names, func->name)) {
            ok = 0;
            cli_set_error_text(error_text, error_text_size, "Out of memory while collecting test functions");
            break;
        }
    }

    if (allow_legacy_main &&
        ok && function_names->count == 0 &&
        rt->main_function && rt->main_function->name &&
        strcmp(rt->main_function->name, "main") == 0 &&
        rt->main_function->param_count == 0 &&
        (rt->main_function->source_file == NULL || cli_paths_match_loose(rt->main_function->source_file, path))) {
        if (!string_list_push_unique(function_names, "main")) {
            ok = 0;
            cli_set_error_text(error_text, error_text_size, "Out of memory while adding legacy main test");
        } else if (used_legacy_main) {
            *used_legacy_main = 1;
        }
    }

    if (ok && function_names->count == 0 && missing_main_only && allow_legacy_main) {
        cli_set_error_text(error_text, error_text_size, runtime_get_error(rt));
        ok = 0;
    }

    runtime_free(rt);
    return ok;
}

static int cli_run_single_tablo_test_function(const char* path,
                                            const char* function_name,
                                            RuntimeOptions options,
                                            int timeout_ms,
                                            double* elapsed_ms,
                                            char* error_text,
                                            size_t error_text_size) {
    if (!path || !function_name) {
        cli_set_error_text(error_text, error_text_size, "Invalid test target");
        if (elapsed_ms) *elapsed_ms = 0.0;
        return 0;
    }

    double start_ms = cli_now_ms();

    Runtime* rt = runtime_create_with_options(path, options);
    if (!rt) {
        cli_set_error_text(error_text, error_text_size, "Failed to create runtime");
        if (elapsed_ms) *elapsed_ms = cli_now_ms() - start_ms;
        return 0;
    }

    if (runtime_has_error(rt)) {
        cli_set_error_text(error_text, error_text_size, runtime_get_error(rt));
        runtime_free(rt);
        if (elapsed_ms) *elapsed_ms = cli_now_ms() - start_ms;
        return 0;
    }

    runtime_set_argv(rt, 0, NULL);
    int rc = runtime_run_function(rt, function_name);
    if (rc != 0 || runtime_has_error(rt)) {
        if (runtime_has_error(rt)) {
            cli_set_error_text(error_text, error_text_size, runtime_get_error(rt));
        } else {
            char buf[160];
            snprintf(buf, sizeof(buf), "Test function '%s' exited with code %d", function_name, rc);
            cli_set_error_text(error_text, error_text_size, buf);
        }
        runtime_free(rt);
        if (elapsed_ms) *elapsed_ms = cli_now_ms() - start_ms;
        return 0;
    }

    runtime_free(rt);
    if (elapsed_ms) *elapsed_ms = cli_now_ms() - start_ms;

    if (timeout_ms > 0 && elapsed_ms && *elapsed_ms > (double)timeout_ms) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "Test exceeded timeout of %d ms (actual %.2f ms)",
                 timeout_ms,
                 *elapsed_ms);
        cli_set_error_text(error_text, error_text_size, buf);
        return 0;
    }

    return 1;
}

static int cli_spawn_test_child_process(const char* program_path,
                                        const CliTestCase* test_case,
                                        RuntimeOptions options,
                                        CliChildProcess* child,
                                        char* error_text,
                                        size_t error_text_size) {
    if (!program_path || !test_case || !child) {
        cli_set_error_text(error_text, error_text_size, "Invalid child-process spawn arguments");
        return 0;
    }

    cli_child_process_init(child);
    child->test_case = test_case;
    child->start_ms = cli_now_ms();
    child->result_file = cli_make_result_file_path(test_case->function_name);
    if (!child->result_file) {
        cli_set_error_text(error_text, error_text_size, "Failed to allocate child result path");
        return 0;
    }

    int child_argc_max = 24 + (options.extension_path_count * 2);
    const char** child_argv = (const char**)calloc((size_t)child_argc_max, sizeof(const char*));
    char max_open_files_buf[32];
    char max_open_sockets_buf[32];
    if (!child_argv) {
        cli_set_error_text(error_text, error_text_size, "Out of memory while building child arguments");
        cli_child_process_free(child);
        return 0;
    }
    max_open_files_buf[0] = '\0';
    max_open_sockets_buf[0] = '\0';
    int argc_idx = 0;
    child_argv[argc_idx++] = program_path;
    child_argv[argc_idx++] = "__run-test-case";
    child_argv[argc_idx++] = "--file";
    child_argv[argc_idx++] = test_case->file_path;
    child_argv[argc_idx++] = "--function";
    child_argv[argc_idx++] = test_case->function_name;
    child_argv[argc_idx++] = "--result-file";
    child_argv[argc_idx++] = child->result_file;
    for (int ext_i = 0; ext_i < options.extension_path_count; ext_i++) {
        child_argv[argc_idx++] = "--ext";
        child_argv[argc_idx++] = options.extension_paths[ext_i];
    }
    if (options.typecheck.warn_unused_error) {
        child_argv[argc_idx++] = "--warn-unused-error";
    }
    if (options.typecheck.strict_errors) {
        child_argv[argc_idx++] = "--strict-errors";
    }
    if (options.capabilities.deny_file_io) {
        child_argv[argc_idx++] = "--deny-file-io";
    }
    if (options.capabilities.deny_network) {
        child_argv[argc_idx++] = "--deny-network";
    }
    if (options.capabilities.deny_process) {
        child_argv[argc_idx++] = "--deny-process";
    }
    if (options.capabilities.deny_sqlite) {
        child_argv[argc_idx++] = "--deny-sqlite";
    }
    if (options.capabilities.deny_threading) {
        child_argv[argc_idx++] = "--deny-threading";
    }
    if (options.max_open_files > 0) {
        snprintf(max_open_files_buf, sizeof(max_open_files_buf), "%d", options.max_open_files);
        child_argv[argc_idx++] = "--max-open-files";
        child_argv[argc_idx++] = max_open_files_buf;
    }
    if (options.max_open_sockets > 0) {
        snprintf(max_open_sockets_buf, sizeof(max_open_sockets_buf), "%d", options.max_open_sockets);
        child_argv[argc_idx++] = "--max-open-sockets";
        child_argv[argc_idx++] = max_open_sockets_buf;
    }
    child_argv[argc_idx] = NULL;

#ifdef _WIN32
    intptr_t proc = _spawnvp(_P_NOWAIT, program_path, child_argv);
    free(child_argv);
    if (proc == -1) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Failed to spawn test child process: %s", strerror(errno));
        cli_set_error_text(error_text, error_text_size, buf);
        cli_child_process_free(child);
        return 0;
    }
    child->process_handle = (HANDLE)proc;
    child->active = 1;
    return 1;
#else
    pid_t pid = fork();
    if (pid < 0) {
        free(child_argv);
        char buf[256];
        snprintf(buf, sizeof(buf), "Failed to fork test child process: %s", strerror(errno));
        cli_set_error_text(error_text, error_text_size, buf);
        cli_child_process_free(child);
        return 0;
    }
    if (pid == 0) {
        execvp(program_path, (char* const*)child_argv);
        _exit(127);
    }
    free(child_argv);
    child->pid = pid;
    child->active = 1;
    return 1;
#endif
}

static void cli_terminate_test_child_process(CliChildProcess* child) {
    if (!child || !child->active) return;
#ifdef _WIN32
    if (child->process_handle) {
        TerminateProcess(child->process_handle, 124u);
        WaitForSingleObject(child->process_handle, 5000);
    }
#else
    if (child->pid > 0) {
        kill(child->pid, SIGKILL);
        waitpid(child->pid, NULL, 0);
    }
#endif
    cli_child_process_free(child);
}

static int cli_finish_child_process_result(CliChildProcess* child,
                                           CliTestRunStatus* status_out,
                                           int* child_exit_code_out,
                                           double* elapsed_ms_out,
                                           char* error_text,
                                           size_t error_text_size) {
    if (!child || !status_out || !child_exit_code_out || !elapsed_ms_out) return 0;

    double elapsed = cli_now_ms() - child->start_ms;
    if (elapsed < 0.0) elapsed = 0.0;
    *elapsed_ms_out = elapsed;

    int child_ok = 0;
    char child_error[2048];
    child_error[0] = '\0';
    int has_result = cli_read_child_result_file(child->result_file,
                                                &child_ok,
                                                child_error,
                                                sizeof(child_error));

#ifdef _WIN32
    DWORD raw_code = 0;
    if (!GetExitCodeProcess(child->process_handle, &raw_code)) {
        raw_code = 1;
    }
    int exit_code = (raw_code > INT_MAX) ? INT_MAX : (int)raw_code;
#else
    int exit_code = *child_exit_code_out;
#endif

    *child_exit_code_out = exit_code;
    if (has_result) {
        if (child_ok && exit_code == 0) {
            *status_out = CLI_TEST_RUN_PASS;
            cli_set_error_text(error_text, error_text_size, "");
        } else {
            *status_out = CLI_TEST_RUN_FAIL;
            if (child_error[0] != '\0') {
                cli_set_error_text(error_text, error_text_size, child_error);
            } else {
                char buf[160];
                snprintf(buf, sizeof(buf), "Test failed with child exit code %d", exit_code);
                cli_set_error_text(error_text, error_text_size, buf);
            }
        }
    } else {
        if (exit_code == 0) {
            *status_out = CLI_TEST_RUN_FAIL;
            cli_set_error_text(error_text, error_text_size, "Test child exited without writing result");
        } else {
            *status_out = CLI_TEST_RUN_CRASH;
            char buf[192];
            snprintf(buf, sizeof(buf), "Test child crashed or aborted (exit code %d)", exit_code);
            cli_set_error_text(error_text, error_text_size, buf);
        }
    }

    cli_child_process_free(child);
    return 1;
}

static int cli_poll_test_child_process(CliChildProcess* child,
                                       int timeout_ms,
                                       int* finished,
                                       CliTestRunStatus* status_out,
                                       int* child_exit_code_out,
                                       double* elapsed_ms_out,
                                       char* error_text,
                                       size_t error_text_size) {
    if (!child || !finished || !status_out || !child_exit_code_out || !elapsed_ms_out) return 0;
    *finished = 0;

    if (!child->active) {
        return 1;
    }

    double elapsed = cli_now_ms() - child->start_ms;
    if (elapsed < 0.0) elapsed = 0.0;
    if (timeout_ms > 0 && elapsed > (double)timeout_ms) {
        *finished = 1;
        *status_out = CLI_TEST_RUN_TIMEOUT;
        *child_exit_code_out = 124;
        *elapsed_ms_out = elapsed;
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "Test exceeded timeout of %d ms (actual %.2f ms)",
                 timeout_ms,
                 elapsed);
        cli_set_error_text(error_text, error_text_size, buf);
        cli_terminate_test_child_process(child);
        return 1;
    }

#ifdef _WIN32
    DWORD wait_rc = WaitForSingleObject(child->process_handle, 0);
    if (wait_rc == WAIT_TIMEOUT) {
        return 1;
    }
    if (wait_rc != WAIT_OBJECT_0) {
        *finished = 1;
        *status_out = CLI_TEST_RUN_CRASH;
        *child_exit_code_out = 1;
        *elapsed_ms_out = elapsed;
        cli_set_error_text(error_text, error_text_size, "Failed while waiting for test child process");
        cli_child_process_free(child);
        return 1;
    }
    *finished = 1;
    *child_exit_code_out = 0;
    return cli_finish_child_process_result(child,
                                           status_out,
                                           child_exit_code_out,
                                           elapsed_ms_out,
                                           error_text,
                                           error_text_size);
#else
    int wstatus = 0;
    pid_t r = waitpid(child->pid, &wstatus, WNOHANG);
    if (r == 0) {
        return 1;
    }
    if (r < 0) {
        *finished = 1;
        *status_out = CLI_TEST_RUN_CRASH;
        *child_exit_code_out = 1;
        *elapsed_ms_out = elapsed;
        cli_set_error_text(error_text, error_text_size, "Failed while waiting for test child process");
        cli_child_process_free(child);
        return 1;
    }

    *finished = 1;
    if (WIFSIGNALED(wstatus)) {
        int sig = WTERMSIG(wstatus);
        *status_out = CLI_TEST_RUN_CRASH;
        *child_exit_code_out = 128 + sig;
        *elapsed_ms_out = elapsed;
        char buf[160];
        snprintf(buf, sizeof(buf), "Test child terminated by signal %d", sig);
        cli_set_error_text(error_text, error_text_size, buf);
        cli_child_process_free(child);
        return 1;
    }

    if (WIFEXITED(wstatus)) {
        *child_exit_code_out = WEXITSTATUS(wstatus);
    } else {
        *child_exit_code_out = 1;
    }

    return cli_finish_child_process_result(child,
                                           status_out,
                                           child_exit_code_out,
                                           elapsed_ms_out,
                                           error_text,
                                           error_text_size);
#endif
}

static int cli_parse_runtime_capability_option(const char* arg, RuntimeOptions* options) {
    if (!arg || !options) return 0;

    if (strcmp(arg, "--deny-file-io") == 0) {
        options->capabilities.deny_file_io = true;
        return 1;
    }
    if (strcmp(arg, "--deny-network") == 0) {
        options->capabilities.deny_network = true;
        return 1;
    }
    if (strcmp(arg, "--deny-process") == 0) {
        options->capabilities.deny_process = true;
        return 1;
    }
    if (strcmp(arg, "--deny-sqlite") == 0) {
        options->capabilities.deny_sqlite = true;
        return 1;
    }
    if (strcmp(arg, "--deny-threading") == 0) {
        options->capabilities.deny_threading = true;
        return 1;
    }

    return 0;
}

static int cli_parse_runtime_limit_value(const char* value, int* out_value) {
    if (!value || !out_value || value[0] == '\0') return 0;
    char* end_ptr = NULL;
    long long parsed = strtoll(value, &end_ptr, 10);
    if (!end_ptr || *end_ptr != '\0' || parsed < 0 || parsed > INT_MAX) {
        return 0;
    }
    *out_value = (int)parsed;
    return 1;
}

static int cli_parse_runtime_limit_option(int argc,
                                          char** argv,
                                          int* index,
                                          RuntimeOptions* options,
                                          const char** out_error) {
    if (out_error) *out_error = NULL;
    if (!argv || !index || !options) return 0;

    const char* arg = argv[*index];
    if (!arg) return 0;

    if (strcmp(arg, "--max-open-files") == 0 || strncmp(arg, "--max-open-files=", 17) == 0) {
        const char* value = NULL;
        if (strcmp(arg, "--max-open-files") == 0) {
            if ((*index + 1) >= argc) {
                if (out_error) *out_error = "Error: --max-open-files requires an integer value";
                return -1;
            }
            value = argv[++(*index)];
        } else {
            value = arg + 17;
        }
        int parsed = 0;
        if (!cli_parse_runtime_limit_value(value, &parsed)) {
            if (out_error) *out_error = "Error: invalid --max-open-files value";
            return -1;
        }
        options->max_open_files = parsed;
        return 1;
    }

    if (strcmp(arg, "--max-open-sockets") == 0 || strncmp(arg, "--max-open-sockets=", 19) == 0) {
        const char* value = NULL;
        if (strcmp(arg, "--max-open-sockets") == 0) {
            if ((*index + 1) >= argc) {
                if (out_error) *out_error = "Error: --max-open-sockets requires an integer value";
                return -1;
            }
            value = argv[++(*index)];
        } else {
            value = arg + 19;
        }
        int parsed = 0;
        if (!cli_parse_runtime_limit_value(value, &parsed)) {
            if (out_error) *out_error = "Error: invalid --max-open-sockets value";
            return -1;
        }
        options->max_open_sockets = parsed;
        return 1;
    }

    return 0;
}

static int cli_internal_run_test_case(int argc, char** argv) {
    const char* file_path = NULL;
    const char* function_name = NULL;
    const char* result_file = NULL;
    bool warn_unused_error = false;
    bool strict_errors = false;
    RuntimeOptions options = {0};
    StringList extension_paths;
    int exit_code = 2;

    string_list_init(&extension_paths);

    for (int i = 2; i < argc; i++) {
        const char* arg = argv[i];
        if (strcmp(arg, "--file") == 0) {
            if (i + 1 < argc) {
                file_path = argv[++i];
            } else {
                fprintf(stderr, "Error: --file requires a value\n");
                goto cleanup;
            }
        } else if (strcmp(arg, "--function") == 0) {
            if (i + 1 < argc) {
                function_name = argv[++i];
            } else {
                fprintf(stderr, "Error: --function requires a value\n");
                goto cleanup;
            }
        } else if (strcmp(arg, "--result-file") == 0) {
            if (i + 1 < argc) {
                result_file = argv[++i];
            } else {
                fprintf(stderr, "Error: --result-file requires a value\n");
                goto cleanup;
            }
        } else {
            const char* extension_error = NULL;
            int parsed_extension = cli_parse_extension_option(argc, argv, &i, &extension_paths, &extension_error);
            if (parsed_extension > 0) {
                continue;
            }
            if (parsed_extension < 0) {
                fprintf(stderr, "%s\n", extension_error ? extension_error : "Error: invalid --ext option");
                goto cleanup;
            }
            if (strcmp(arg, "--warn-unused-error") == 0) {
                warn_unused_error = true;
            } else if (strcmp(arg, "--strict-errors") == 0) {
                strict_errors = true;
                warn_unused_error = true;
            } else if (cli_parse_runtime_capability_option(arg, &options)) {
                continue;
            } else {
                const char* parse_err = NULL;
                int parsed_limit = cli_parse_runtime_limit_option(argc, argv, &i, &options, &parse_err);
                if (parsed_limit > 0) {
                    continue;
                }
                if (parsed_limit < 0) {
                    fprintf(stderr, "%s: '%s'\n", parse_err ? parse_err : "Error: invalid runtime limit option", arg);
                    goto cleanup;
                }
                fprintf(stderr, "Error: unknown internal option '%s'\n", arg);
                goto cleanup;
            }
        }
    }

    if (!file_path || !function_name || !result_file) {
        fprintf(stderr, "Error: internal test invocation missing --file/--function/--result-file\n");
        goto cleanup;
    }

    // Test-only hook: force child crash path for CLI runner regression tests.
    const char* force_child_crash = getenv("TABLO_TEST_FORCE_CHILD_CRASH");
    if (force_child_crash && strcmp(force_child_crash, "1") == 0) {
        _exit(86);
    }

    options.typecheck.warn_unused_error = warn_unused_error;
    options.typecheck.strict_errors = strict_errors;
    options.extension_paths = (const char* const*)extension_paths.items;
    options.extension_path_count = extension_paths.count;

    double elapsed_ms = 0.0;
    char error_text[4096];
    error_text[0] = '\0';
    int ok = cli_run_single_tablo_test_function(file_path,
                                              function_name,
                                              options,
                                              0,
                                              &elapsed_ms,
                                              error_text,
                                              sizeof(error_text));

    if (!cli_write_child_result_file(result_file, ok, error_text)) {
        fprintf(stderr, "Error: failed to write internal result file '%s'\n", result_file);
        goto cleanup;
    }

    exit_code = ok ? 0 : 1;

cleanup:
    string_list_free(&extension_paths);
    return exit_code;
}

static int cli_test_via_ctest(void) {
    int result = system("ctest --output-on-failure");
    if (result == 0) return 0;

    if (cli_path_is_directory("build")) {
#ifdef _WIN32
        result = system("ctest --test-dir build -C Debug --output-on-failure");
#else
        result = system("ctest --test-dir build --output-on-failure");
#endif
        if (result == 0) return 0;
    }

    fprintf(stderr, "CTest failed. Run 'cmake --build build --config Debug' first.\n");
    return 1;
}

static void print_version(void) {
    printf("TabloLang %s\n", TABLO_VERSION);
}

void print_usage(const char* program_name) {
    printf("Usage: %s <command> [options]\n", program_name);
    printf("Commands:\n");
    printf("  run [options] <file> [args...]  Run a TabloLang program\n");
    printf("  debug [options] <file> [args...] Run until a source-line breakpoint hits\n");
    printf("  compile [options] <file>         Compile a TabloLang program to a bytecode artifact\n");
    printf("  test [options] [path...]         Run Tablo tests (_test.tblo discovery)\n");
    printf("  mod <subcommand> [options]       Manage Tablo dependencies (init/add/update/fetch/verify/tidy/vendor)\n");
    printf("  lsp <subcommand>                 Run the LSP server or symbol tooling\n");
    printf("  dap <subcommand>                 Run the DAP server for debugger clients\n");
    printf("Options:\n");
    printf("  -o <file>        Output file (for compile)\n");
    printf("  --ext <library>  Load a native extension shared library; repeatable (run/compile/debug/test)\n");
    printf("  --dump-bytecode  Dump generated bytecode\n");
    printf("  --break <spec>   Break on <line> in the entry file or <file>:<line>; repeatable (for debug)\n");
    printf("  --continue       Resume after the current stop until the next stop or exit (for debug)\n");
    printf("  --step-in        Step into the next source location after the current stop (for debug)\n");
    printf("  --step-over      Step over calls to the next source location in the current frame (for debug)\n");
    printf("  --step-out       Step out to the caller frame and stop there (for debug)\n");
    printf("  --profile-opcodes   Dump opcode profile after execution\n");
    printf("  --profile-jit       Dump per-function hotness profile after execution\n");
    printf("  --dump-jit-queue    Dump queued hot functions selected for future JIT compilation\n");
    printf("  --drain-jit-queue   Drain the queued hot functions through the current stub JIT backend\n");
    printf("  --jit-auto-compile  Auto-drain newly hot functions through the current stub JIT backend\n");
    printf("  --jit-hot-threshold <n> Mark functions hot once they reach <n> entries (for --profile-jit)\n");
    printf("  --warn-unused-error  Warn on unused (value, Error?) results\n");
    printf("  --strict-errors      Treat unused error results as type errors\n");
    printf("  --deny-file-io       Disable file I/O builtins (read/write/open/delete)\n");
    printf("  --deny-network       Disable network builtins (http/tcp/tls)\n");
    printf("  --deny-process       Disable process execution builtins\n");
    printf("  --deny-sqlite        Disable SQLite builtins\n");
    printf("  --deny-threading     Disable threading/channel/shared-state builtins\n");
    printf("  --max-open-files <n> Limit concurrently open file handles (0 = unlimited)\n");
    printf("  --max-open-sockets <n> Limit concurrently open sockets (0 = unlimited)\n");
    printf("  --fail-fast         Stop test run after first failing test (for test)\n");
    printf("  --list              List discovered tests without executing\n");
    printf("  --match <pattern>   Run/list only tests whose id matches pattern\n");
    printf("  --json              Emit machine-readable JSON summary\n");
    printf("  --timeout-ms <ms>   Hard timeout per test process (kills on exceed)\n");
    printf("  --jobs <n>          Run up to n test processes in parallel\n");
    printf("  --rerun-failed <n>  Retry failing tests up to n additional attempts\n");
    printf("  --shard <i>/<n>     Run deterministic shard i of n over selected tests\n");
    printf("  --junit <file>      Write JUnit XML report for test execution\n");
    printf("  --ctest             Delegate test command to ctest --output-on-failure\n");
    printf("  --version        Show version information\n");
    printf("  --help           Show this help message\n");
    printf("\n");
    printf("For 'test': if no path is provided, recursively discovers '*_test.tblo' under 'tests/tablo_tests' and runs zero-arg test* functions only.\n");
    printf("Explicit file targets also allow legacy main() fallback when no test* functions are present.\n");
    printf("Arguments after <file> are passed to the program as argv.\n");
}

typedef struct {
    char* source_file;
    int line;
} CliDebugBreakpoint;

typedef struct {
    CliDebugBreakpoint* items;
    int count;
    int capacity;
} CliDebugBreakpointList;

typedef enum {
    CLI_DEBUG_ACTION_CONTINUE = 0,
    CLI_DEBUG_ACTION_STEP_IN = 1,
    CLI_DEBUG_ACTION_STEP_OVER = 2,
    CLI_DEBUG_ACTION_STEP_OUT = 3
} CliDebugActionKind;

typedef struct {
    CliDebugActionKind* items;
    int count;
    int capacity;
} CliDebugActionList;

static void cli_debug_breakpoint_list_init(CliDebugBreakpointList* list) {
    if (!list) return;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void cli_debug_breakpoint_list_free(CliDebugBreakpointList* list) {
    if (!list) return;
    if (list->items) {
        for (int i = 0; i < list->count; i++) {
            if (list->items[i].source_file) {
                free(list->items[i].source_file);
            }
        }
        free(list->items);
    }
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void cli_debug_action_list_init(CliDebugActionList* list) {
    if (!list) return;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void cli_debug_action_list_free(CliDebugActionList* list) {
    if (!list) return;
    if (list->items) free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int cli_debug_action_list_push(CliDebugActionList* list, CliDebugActionKind action) {
    if (!list) return 0;
    if (list->count >= list->capacity) {
        int new_capacity = list->capacity > 0 ? list->capacity * 2 : 4;
        CliDebugActionKind* items =
            (CliDebugActionKind*)realloc(list->items, (size_t)new_capacity * sizeof(CliDebugActionKind));
        if (!items) return 0;
        list->items = items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = action;
    return 1;
}

static char* cli_strdup_alloc(const char* text) {
    if (!text) return NULL;
    size_t len = strlen(text);
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, text, len + 1);
    return out;
}

static int cli_debug_breakpoint_list_push(CliDebugBreakpointList* list, char* source_file, int line) {
    if (!list || line <= 0) {
        if (source_file) free(source_file);
        return 0;
    }

    if (list->count >= list->capacity) {
        int new_capacity = list->capacity > 0 ? list->capacity * 2 : 4;
        CliDebugBreakpoint* items =
            (CliDebugBreakpoint*)realloc(list->items, (size_t)new_capacity * sizeof(CliDebugBreakpoint));
        if (!items) {
            if (source_file) free(source_file);
            return 0;
        }
        list->items = items;
        list->capacity = new_capacity;
    }

    list->items[list->count].source_file = source_file;
    list->items[list->count].line = line;
    list->count++;
    return 1;
}

static int cli_parse_breakpoint_spec_value(const char* value,
                                           char** out_source_file,
                                           int* out_line,
                                           const char** out_error) {
    if (out_error) *out_error = NULL;
    if (out_source_file) *out_source_file = NULL;
    if (out_line) *out_line = 0;
    if (!value || !out_line) return 0;

    const char* colon = strrchr(value, ':');
    const char* line_text = value;
    char* source_file = NULL;

    if (colon) {
        line_text = colon + 1;
        if (colon == value) {
            if (out_error) *out_error = "Error: invalid --break value";
            return 0;
        }
        source_file = (char*)malloc((size_t)(colon - value) + 1);
        if (!source_file) {
            if (out_error) *out_error = "Error: out of memory while parsing --break";
            return 0;
        }
        memcpy(source_file, value, (size_t)(colon - value));
        source_file[colon - value] = '\0';
    }

    char* end_ptr = NULL;
    long long parsed = strtoll(line_text, &end_ptr, 10);
    if (!end_ptr || *end_ptr != '\0' || parsed <= 0 || parsed > INT_MAX) {
        if (source_file) free(source_file);
        if (out_error) *out_error = "Error: invalid --break value";
        return 0;
    }

    if (out_source_file) *out_source_file = source_file;
    *out_line = (int)parsed;
    return 1;
}

static int cli_parse_breakpoint_option(int argc,
                                       char** argv,
                                       int* index,
                                       CliDebugBreakpointList* breakpoints,
                                       const char** out_error) {
    if (out_error) *out_error = NULL;
    if (!argv || !index || !breakpoints) return 0;

    const char* arg = argv[*index];
    if (!arg) return 0;
    if (strcmp(arg, "--break") != 0 && strncmp(arg, "--break=", 8) != 0) {
        return 0;
    }

    const char* value = NULL;
    if (strcmp(arg, "--break") == 0) {
        if ((*index + 1) >= argc) {
            if (out_error) *out_error = "Error: --break requires <line> or <file>:<line>";
            return -1;
        }
        value = argv[++(*index)];
    } else {
        value = arg + 8;
    }

    if (!value || value[0] == '\0') {
        if (out_error) *out_error = "Error: --break requires <line> or <file>:<line>";
        return -1;
    }

    char* source_file = NULL;
    int line = 0;
    if (!cli_parse_breakpoint_spec_value(value, &source_file, &line, out_error)) {
        return -1;
    }
    if (!cli_debug_breakpoint_list_push(breakpoints, source_file, line)) {
        if (out_error) *out_error = "Error: out of memory while storing --break";
        return -1;
    }
    return 1;
}

static int cli_parse_debug_action_option(const char* arg,
                                         CliDebugActionList* actions,
                                         const char** out_error) {
    if (out_error) *out_error = NULL;
    if (!arg || !actions) return 0;

    CliDebugActionKind action;
    if (strcmp(arg, "--continue") == 0) {
        action = CLI_DEBUG_ACTION_CONTINUE;
    } else if (strcmp(arg, "--step-in") == 0) {
        action = CLI_DEBUG_ACTION_STEP_IN;
    } else if (strcmp(arg, "--step-over") == 0) {
        action = CLI_DEBUG_ACTION_STEP_OVER;
    } else if (strcmp(arg, "--step-out") == 0) {
        action = CLI_DEBUG_ACTION_STEP_OUT;
    } else {
        return 0;
    }

    if (!cli_debug_action_list_push(actions, action)) {
        if (out_error) *out_error = "Error: out of memory while storing debug action";
        return -1;
    }
    return 1;
}

static void cli_trim_trailing_newline(char* text) {
    if (!text) return;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[--len] = '\0';
    }
}

static int cli_read_source_line(const char* path, int line, char* out, size_t out_size) {
    if (!path || !out || out_size == 0 || line <= 0) return 0;

    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    int current_line = 1;
    int ok = 0;
    while (fgets(out, (int)out_size, f)) {
        if (current_line == line) {
            cli_trim_trailing_newline(out);
            ok = 1;
            break;
        }
        current_line++;
    }

    fclose(f);
    return ok;
}

static void cli_print_debug_stop_report(Runtime* rt) {
    if (!rt || !rt->vm) return;

    const VmDebugStopInfo* stop = vm_debug_get_stop_info(rt->vm);
    if (!stop || stop->kind == VM_DEBUG_STOP_NONE) return;

    const char* label = "Stopped";
    if (stop->kind == VM_DEBUG_STOP_BREAKPOINT) {
        label = "Breakpoint hit";
    } else if (stop->kind == VM_DEBUG_STOP_STEP) {
        label = "Step hit";
    }

    printf("%s at %s:%d in %s\n",
           label,
           (stop->source_file && stop->source_file[0] != '\0') ? stop->source_file : "<unknown>",
           stop->line,
           (stop->function_name && stop->function_name[0] != '\0') ? stop->function_name : "<anon>");

    char source_line[512];
    if (stop->source_file && cli_read_source_line(stop->source_file, stop->line, source_line, sizeof(source_line))) {
        printf("Source: %s\n", source_line);
    }

    int frame_count = vm_debug_frame_count(rt->vm);
    if (frame_count > 0) {
        printf("Call stack:\n");
        for (int i = 0; i < frame_count; i++) {
            VmDebugFrameInfo frame_info;
            if (!vm_debug_get_frame_info(rt->vm, i, &frame_info)) {
                continue;
            }
            const char* function_name =
                (frame_info.function_name && frame_info.function_name[0] != '\0')
                    ? frame_info.function_name
                    : "<anon>";
            const char* source_file =
                (frame_info.source_file && frame_info.source_file[0] != '\0')
                    ? frame_info.source_file
                    : "<unknown>";
            if (frame_info.line > 0) {
                printf("  #%d %s at %s:%d\n", i, function_name, source_file, frame_info.line);
            } else {
                printf("  #%d %s at %s\n", i, function_name, source_file);
            }
        }
    }
}

int cli_debug(int argc, char** argv) {
    bool warn_unused_error = false;
    bool strict_errors = false;
    RuntimeOptions options = {0};
    const char* file_path = NULL;
    int file_index = -1;
    StringList extension_paths;
    CliDebugBreakpointList breakpoints;
    CliDebugActionList actions;
    string_list_init(&extension_paths);
    cli_debug_breakpoint_list_init(&breakpoints);
    cli_debug_action_list_init(&actions);

    for (int i = 2; i < argc; i++) {
        int parsed_break = 0;
        const char* break_error = NULL;
        parsed_break = cli_parse_breakpoint_option(argc, argv, &i, &breakpoints, &break_error);
        if (parsed_break > 0) {
            continue;
        }
        if (parsed_break < 0) {
            fprintf(stderr, "%s: '%s'\n", break_error ? break_error : "Error: invalid --break option", argv[i]);
            string_list_free(&extension_paths);
            cli_debug_breakpoint_list_free(&breakpoints);
            cli_debug_action_list_free(&actions);
            return 1;
        }

        const char* action_error = NULL;
        int parsed_action = cli_parse_debug_action_option(argv[i], &actions, &action_error);
        if (parsed_action > 0) {
            continue;
        }
        if (parsed_action < 0) {
            fprintf(stderr, "%s: '%s'\n", action_error ? action_error : "Error: invalid debug action", argv[i]);
            string_list_free(&extension_paths);
            cli_debug_breakpoint_list_free(&breakpoints);
            cli_debug_action_list_free(&actions);
            return 1;
        }

        const char* extension_error = NULL;
        int parsed_extension = cli_parse_extension_option(argc, argv, &i, &extension_paths, &extension_error);
        if (parsed_extension > 0) {
            continue;
        }
        if (parsed_extension < 0) {
            fprintf(stderr, "%s\n", extension_error ? extension_error : "Error: invalid --ext option");
            string_list_free(&extension_paths);
            cli_debug_breakpoint_list_free(&breakpoints);
            cli_debug_action_list_free(&actions);
            return 1;
        }

        if (strcmp(argv[i], "--warn-unused-error") == 0) {
            warn_unused_error = true;
        } else if (strcmp(argv[i], "--strict-errors") == 0) {
            strict_errors = true;
            warn_unused_error = true;
        } else if (cli_parse_runtime_capability_option(argv[i], &options)) {
            continue;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            string_list_free(&extension_paths);
            cli_debug_breakpoint_list_free(&breakpoints);
            cli_debug_action_list_free(&actions);
            return 0;
        } else if (strcmp(argv[i], "--") == 0) {
            if (i + 1 < argc) {
                file_path = argv[i + 1];
                file_index = i + 1;
            }
            break;
        } else if (argv[i][0] != '-') {
            file_path = argv[i];
            file_index = i;
            break;
        } else {
            const char* parse_err = NULL;
            int parsed_limit = cli_parse_runtime_limit_option(argc, argv, &i, &options, &parse_err);
            if (parsed_limit > 0) {
                continue;
            }
            if (parsed_limit < 0) {
                fprintf(stderr, "%s: '%s'\n", parse_err ? parse_err : "Error: invalid runtime limit option", argv[i]);
                string_list_free(&extension_paths);
                cli_debug_breakpoint_list_free(&breakpoints);
                cli_debug_action_list_free(&actions);
                return 1;
            }
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            string_list_free(&extension_paths);
            cli_debug_breakpoint_list_free(&breakpoints);
            cli_debug_action_list_free(&actions);
            return 1;
        }
    }

    if (!file_path) {
        fprintf(stderr, "Error: No file specified\n");
        print_usage(argv[0]);
        string_list_free(&extension_paths);
        cli_debug_breakpoint_list_free(&breakpoints);
        cli_debug_action_list_free(&actions);
        return 1;
    }
    if (breakpoints.count <= 0) {
        fprintf(stderr, "Error: debug requires at least one --break <line|file:line>\n");
        string_list_free(&extension_paths);
        cli_debug_breakpoint_list_free(&breakpoints);
        cli_debug_action_list_free(&actions);
        return 1;
    }

    for (int i = 0; i < breakpoints.count; i++) {
            if (!breakpoints.items[i].source_file) {
                breakpoints.items[i].source_file = cli_strdup_alloc(file_path);
                if (!breakpoints.items[i].source_file) {
                    fprintf(stderr, "Error: out of memory while resolving entry-file breakpoints\n");
                    string_list_free(&extension_paths);
                    cli_debug_breakpoint_list_free(&breakpoints);
                    cli_debug_action_list_free(&actions);
                    return 1;
                }
            }
    }

    options.typecheck.warn_unused_error = warn_unused_error;
    options.typecheck.strict_errors = strict_errors;
    options.extension_paths = (const char* const*)extension_paths.items;
    options.extension_path_count = extension_paths.count;

    Runtime* rt = runtime_create_with_options(file_path, options);
    string_list_free(&extension_paths);
    if (!rt) {
        fprintf(stderr, "Error: Failed to create runtime\n");
        cli_debug_breakpoint_list_free(&breakpoints);
        cli_debug_action_list_free(&actions);
        return 1;
    }
    if (runtime_has_error(rt)) {
        fprintf(stderr, "Error: %s\n", runtime_get_error(rt));
        runtime_free(rt);
        cli_debug_breakpoint_list_free(&breakpoints);
        cli_debug_action_list_free(&actions);
        return 1;
    }

    int script_argc = argc - file_index - 1;
    char** script_argv = (script_argc > 0) ? &argv[file_index + 1] : NULL;
    runtime_set_argv(rt, script_argc, script_argv);
    vm_debug_clear_line_breakpoints(rt->vm);
    for (int i = 0; i < breakpoints.count; i++) {
        if (!vm_debug_add_line_breakpoint(rt->vm, breakpoints.items[i].source_file, breakpoints.items[i].line)) {
            fprintf(stderr, "Error: failed to register debug breakpoint\n");
            runtime_free(rt);
            cli_debug_breakpoint_list_free(&breakpoints);
            cli_debug_action_list_free(&actions);
            return 1;
        }
    }

    int rc = runtime_run(rt);
    int action_index = 0;
    bool hit_any_stop = false;

    while (1) {
        const VmDebugStopInfo* stop = vm_debug_get_stop_info(rt->vm);

        if (runtime_has_error(rt)) {
            fprintf(stderr, "Error: %s\n", runtime_get_error(rt));
            runtime_free(rt);
            cli_debug_breakpoint_list_free(&breakpoints);
            cli_debug_action_list_free(&actions);
            return 1;
        }

        if (rc == 1 && stop && stop->kind != VM_DEBUG_STOP_NONE) {
            hit_any_stop = true;
            cli_print_debug_stop_report(rt);

            if (action_index >= actions.count) {
                runtime_free(rt);
                cli_debug_breakpoint_list_free(&breakpoints);
                cli_debug_action_list_free(&actions);
                return 0;
            }

            CliDebugActionKind action = actions.items[action_index++];
            switch (action) {
                case CLI_DEBUG_ACTION_CONTINUE:
                    vm_debug_prepare_continue(rt->vm);
                    break;
                case CLI_DEBUG_ACTION_STEP_IN:
                    vm_debug_prepare_step_in(rt->vm);
                    break;
                case CLI_DEBUG_ACTION_STEP_OVER:
                    vm_debug_prepare_step_over(rt->vm);
                    break;
                case CLI_DEBUG_ACTION_STEP_OUT:
                    vm_debug_prepare_step_out(rt->vm);
                    break;
            }
            rc = runtime_resume(rt);
            continue;
        }

        if (rc == 0) {
            if (!hit_any_stop) {
                if (breakpoints.count == 1) {
                    fprintf(stderr, "Breakpoint not hit: %s:%d\n",
                            breakpoints.items[0].source_file,
                            breakpoints.items[0].line);
                } else {
                    fprintf(stderr, "Breakpoints not hit (%d configured)\n", breakpoints.count);
                    for (int i = 0; i < breakpoints.count; i++) {
                        fprintf(stderr, "  - %s:%d\n",
                                breakpoints.items[i].source_file,
                                breakpoints.items[i].line);
                    }
                    }
                runtime_free(rt);
                cli_debug_breakpoint_list_free(&breakpoints);
                cli_debug_action_list_free(&actions);
                return 1;
            }

            if (action_index > 0) {
                printf("Program exited normally\n");
            }
            runtime_free(rt);
            cli_debug_breakpoint_list_free(&breakpoints);
            cli_debug_action_list_free(&actions);
            return 0;
        }

        fprintf(stderr, "Error: debug session ended unexpectedly\n");
        runtime_free(rt);
        cli_debug_breakpoint_list_free(&breakpoints);
        cli_debug_action_list_free(&actions);
        return 1;
    }
}

int cli_run(int argc, char** argv) {
    bool dump_bytecode = false;
    bool profile_opcodes = false;
    bool profile_jit = false;
    bool dump_jit_queue = false;
    bool drain_jit_queue = false;
    bool jit_auto_compile = false;
    bool warn_unused_error = false;
    bool strict_errors = false;
    uint64_t jit_hot_threshold = 0;
    RuntimeOptions options = {0};
    const char* file_path = NULL;
    int file_index = -1;
    StringList extension_paths;
    string_list_init(&extension_paths);

    // Parse options until we find the file (first non-option argument)
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--dump-bytecode") == 0) {
            dump_bytecode = true;
        } else if (strcmp(argv[i], "--profile-opcodes") == 0) {
            profile_opcodes = true;
        } else if (strcmp(argv[i], "--profile-jit") == 0) {
            profile_jit = true;
        } else if (strcmp(argv[i], "--dump-jit-queue") == 0) {
            dump_jit_queue = true;
        } else if (strcmp(argv[i], "--drain-jit-queue") == 0) {
            drain_jit_queue = true;
        } else if (strcmp(argv[i], "--jit-auto-compile") == 0) {
            jit_auto_compile = true;
        } else if (strcmp(argv[i], "--jit-hot-threshold") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --jit-hot-threshold requires a positive integer value\n");
                string_list_free(&extension_paths);
                return 1;
            }
            char* end = NULL;
            unsigned long long parsed = strtoull(argv[++i], &end, 10);
            if (!end || *end != '\0' || parsed == 0) {
                fprintf(stderr, "Error: --jit-hot-threshold requires a positive integer value\n");
                string_list_free(&extension_paths);
                return 1;
            }
            jit_hot_threshold = (uint64_t)parsed;
        } else if (strcmp(argv[i], "--warn-unused-error") == 0) {
            warn_unused_error = true;
        } else if (strcmp(argv[i], "--strict-errors") == 0) {
            strict_errors = true;
            warn_unused_error = true;
        } else if (cli_parse_runtime_capability_option(argv[i], &options)) {
            continue;
        } else {
            const char* extension_error = NULL;
            int parsed_extension = cli_parse_extension_option(argc, argv, &i, &extension_paths, &extension_error);
            if (parsed_extension > 0) {
                continue;
            }
            if (parsed_extension < 0) {
                fprintf(stderr, "%s\n", extension_error ? extension_error : "Error: invalid --ext option");
                string_list_free(&extension_paths);
                return 1;
            }
            if (strcmp(argv[i], "--help") == 0) {
                print_usage(argv[0]);
                string_list_free(&extension_paths);
                return 0;
            } else if (strcmp(argv[i], "--") == 0) {
                // Explicit end of options
                if (i + 1 < argc) {
                    file_path = argv[i + 1];
                    file_index = i + 1;
                }
                break;
            } else if (argv[i][0] != '-') {
                file_path = argv[i];
                file_index = i;
                break;
            } else {
                const char* parse_err = NULL;
                int parsed_limit = cli_parse_runtime_limit_option(argc, argv, &i, &options, &parse_err);
                if (parsed_limit > 0) {
                    continue;
                }
                if (parsed_limit < 0) {
                    fprintf(stderr, "%s: '%s'\n", parse_err ? parse_err : "Error: invalid runtime limit option", argv[i]);
                    string_list_free(&extension_paths);
                    return 1;
                }
                fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
                string_list_free(&extension_paths);
                return 1;
            }
        }
    }

    if (!file_path) {
        fprintf(stderr, "Error: No file specified\n");
        print_usage(argv[0]);
        string_list_free(&extension_paths);
        return 1;
    }

    options.typecheck.warn_unused_error = warn_unused_error;
    options.typecheck.strict_errors = strict_errors;
    options.extension_paths = (const char* const*)extension_paths.items;
    options.extension_path_count = extension_paths.count;

    Runtime* rt = runtime_create_with_options(file_path, options);
    string_list_free(&extension_paths);
    if (!rt) {
        fprintf(stderr, "Error: Failed to create runtime\n");
        return 1;
    }

    if (runtime_has_error(rt)) {
        fprintf(stderr, "Error: %s\n", runtime_get_error(rt));
        runtime_free(rt);
        return 1;
    }

    // Pass script arguments (everything after the file) to the runtime
    int script_argc = argc - file_index - 1;
    char** script_argv = (script_argc > 0) ? &argv[file_index + 1] : NULL;
    runtime_set_argv(rt, script_argc, script_argv);

    if (profile_opcodes && rt->vm) {
        rt->vm->profile_opcodes = true;
        memset(rt->vm->opcode_counts, 0, sizeof(rt->vm->opcode_counts));
    }
    if (rt->vm) {
        if (profile_jit) {
            jit_set_profile_enabled(rt->vm, true);
        }
        if (jit_auto_compile) {
            jit_set_auto_compile_enabled(rt->vm, true);
        }
        if (jit_hot_threshold > 0) {
            jit_set_hot_threshold(rt->vm, jit_hot_threshold);
        }
    }

    if (dump_bytecode) {
        if (rt->init_function) {
            printf("Bytecode (init):\n");
            dump_chunk_disasm(&rt->init_function->chunk);
        }
        if (rt->functions && rt->function_count > 0) {
            for (int i = 0; i < rt->function_count; i++) {
                ObjFunction* func = rt->functions[i];
                if (!func) continue;
                printf("Bytecode (%s):\n", func->name ? func->name : "<anon>");
                dump_chunk_disasm(&func->chunk);
            }
        } else if (rt->main_function) {
            printf("Bytecode (main):\n");
            dump_chunk_disasm(&rt->main_function->chunk);
        }
    }

    int exit_code = runtime_run(rt);

    if (runtime_has_error(rt)) {
        fprintf(stderr, "Error: %s\n", runtime_get_error(rt));
        runtime_free(rt);
        return 1;
    }

    if (profile_opcodes && rt->vm) {
        dump_opcode_profile(rt->vm);
    }
    if (drain_jit_queue && rt->vm) {
        int drained = jit_drain_work_queue(rt->vm, 0);
        printf("\nJIT drain report: processed=%d\n", drained);
    }
    if (profile_jit && rt->vm) {
        jit_dump_profile(stdout,
                         rt->vm,
                         rt->init_function,
                         rt->main_function,
                         rt->functions,
                         rt->function_count);
    }
    if (dump_jit_queue && rt->vm) {
        jit_dump_work_queue(stdout, rt->vm);
    }

    runtime_free(rt);
    return exit_code;
}

int cli_compile(int argc, char** argv) {
    bool dump_bytecode = false;
    bool warn_unused_error = false;
    bool strict_errors = false;
    RuntimeOptions options = {0};
    const char* file_path = NULL;
    const char* output_path = NULL;
    StringList extension_paths;
    string_list_init(&extension_paths);
    
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--dump-bytecode") == 0) {
            dump_bytecode = true;
        } else if (strcmp(argv[i], "--warn-unused-error") == 0) {
            warn_unused_error = true;
        } else if (strcmp(argv[i], "--strict-errors") == 0) {
            strict_errors = true;
            warn_unused_error = true;
        } else if (cli_parse_runtime_capability_option(argv[i], &options)) {
            continue;
        } else {
            const char* extension_error = NULL;
            int parsed_extension = cli_parse_extension_option(argc, argv, &i, &extension_paths, &extension_error);
            if (parsed_extension > 0) {
                continue;
            }
            if (parsed_extension < 0) {
                fprintf(stderr, "%s\n", extension_error ? extension_error : "Error: invalid --ext option");
                string_list_free(&extension_paths);
                return 1;
            }
            if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
                if (i + 1 < argc) {
                    output_path = argv[++i];
                }
            } else if (strcmp(argv[i], "--help") == 0) {
                print_usage(argv[0]);
                string_list_free(&extension_paths);
                return 0;
            } else if (argv[i][0] != '-') {
                file_path = argv[i];
            } else {
                const char* parse_err = NULL;
                int parsed_limit = cli_parse_runtime_limit_option(argc, argv, &i, &options, &parse_err);
                if (parsed_limit > 0) {
                    continue;
                }
                if (parsed_limit < 0) {
                    fprintf(stderr, "%s: '%s'\n", parse_err ? parse_err : "Error: invalid runtime limit option", argv[i]);
                    string_list_free(&extension_paths);
                    return 1;
                }
                fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
                string_list_free(&extension_paths);
                return 1;
            }
        }
    }
    
    if (!file_path) {
        fprintf(stderr, "Error: No file specified\n");
        print_usage(argv[0]);
        string_list_free(&extension_paths);
        return 1;
    }
    
    options.typecheck.warn_unused_error = warn_unused_error;
    options.typecheck.strict_errors = strict_errors;
    options.extension_paths = (const char* const*)extension_paths.items;
    options.extension_path_count = extension_paths.count;

    Runtime* rt = runtime_create_with_options(file_path, options);
    string_list_free(&extension_paths);
    if (!rt) {
        fprintf(stderr, "Error: Failed to create runtime\n");
        return 1;
    }
    
    if (runtime_has_error(rt)) {
        fprintf(stderr, "Error: %s\n", runtime_get_error(rt));
        runtime_free(rt);
        return 1;
    }
    
    if (dump_bytecode && rt->main_function) {
        printf("Bytecode:\n");
        dump_chunk_disasm(&rt->main_function->chunk);
    }

    int main_index = -1;
    if (rt->functions && rt->main_function) {
        for (int i = 0; i < rt->function_count; i++) {
            if (rt->functions[i] == rt->main_function) {
                main_index = i;
                break;
            }
        }
    }

    if (output_path) {
        uint32_t typecheck_flags = 0;
        if (warn_unused_error) typecheck_flags |= 1u;
        if (strict_errors) typecheck_flags |= 2u;

        char err[256];
        if (!artifact_write_file(output_path,
                                 rt->init_function,
                                 rt->functions,
                                 rt->function_count,
                                 main_index,
                                 typecheck_flags,
                                 NULL,
                                 0,
                                 rt->interface_dispatch_entries,
                                 rt->interface_dispatch_count,
                                 err,
                                 sizeof(err))) {
            fprintf(stderr, "Error: %s\n", err[0] ? err : "Failed to write bytecode artifact");
            runtime_free(rt);
            return 1;
        }
    }
    
    if (!dump_bytecode) {
        if (output_path) {
            printf("Compilation successful. Wrote bytecode artifact to '%s'.\n", output_path);
        } else {
            printf("Compilation successful. Main function has %d bytes of bytecode.\n", rt->main_function->chunk.code_count);
        }
    }
    runtime_free(rt);
    return 0;
}

int cli_test(int argc, char** argv) {
    bool fail_fast = false;
    bool warn_unused_error = false;
    bool strict_errors = false;
    bool use_ctest = false;
    bool list_only = false;
    bool json_output = false;
    const char* match_pattern = NULL;
    const char* junit_path = NULL;
    int timeout_ms = 0;
    int jobs = 1;
    int rerun_failed = 0;
    int shard_index = 1;
    int shard_total = 1;
    int exit_code = 1;
    const char* program_path = (argc > 0 && argv && argv[0]) ? argv[0] : "tablo";
    RuntimeOptions options = {0};

    StringList targets;
    StringList files;
    StringList extension_paths;
    CliTestCaseList discovered_cases;
    CliTestCaseList matched_cases;
    CliTestCaseList selected_cases;
    CliRunResultList run_results;
    CliChildProcess* active_children = NULL;
    int active_children_capacity = 0;
    int* final_last_idx = NULL;
    int* attempt_counts = NULL;
    string_list_init(&targets);
    string_list_init(&files);
    string_list_init(&extension_paths);
    cli_test_case_list_init(&discovered_cases);
    cli_test_case_list_init(&matched_cases);
    cli_test_case_list_init(&selected_cases);
    cli_run_result_list_init(&run_results);

    for (int i = 2; i < argc; i++) {
        const char* arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            exit_code = 0;
            goto cleanup;
        } else if (strcmp(arg, "--fail-fast") == 0) {
            fail_fast = true;
        } else if (strcmp(arg, "--warn-unused-error") == 0) {
            warn_unused_error = true;
        } else if (strcmp(arg, "--strict-errors") == 0) {
            strict_errors = true;
            warn_unused_error = true;
        } else {
            const char* extension_error = NULL;
            int parsed_extension = cli_parse_extension_option(argc, argv, &i, &extension_paths, &extension_error);
            if (parsed_extension > 0) {
                continue;
            }
            if (parsed_extension < 0) {
                fprintf(stderr, "%s\n", extension_error ? extension_error : "Error: invalid --ext option");
                goto cleanup;
            }
            if (cli_parse_runtime_capability_option(arg, &options)) {
                continue;
            }
            if (strcmp(arg, "--ctest") == 0) {
                use_ctest = true;
            } else if (strcmp(arg, "--list") == 0) {
                list_only = true;
            } else if (strcmp(arg, "--json") == 0) {
                json_output = true;
            } else if (strcmp(arg, "--junit") == 0 || strncmp(arg, "--junit=", 8) == 0) {
                const char* value = NULL;
                if (strcmp(arg, "--junit") == 0) {
                    if ((i + 1) >= argc) {
                        fprintf(stderr, "Error: --junit requires an output file path\n");
                        goto cleanup;
                    }
                    value = argv[++i];
                } else {
                    value = arg + 8;
                }
                if (!value || value[0] == '\0') {
                    fprintf(stderr, "Error: --junit requires a non-empty output file path\n");
                    goto cleanup;
                }
                junit_path = value;
            } else if (strcmp(arg, "--match") == 0 || strncmp(arg, "--match=", 8) == 0) {
                const char* value = NULL;
                if (strcmp(arg, "--match") == 0) {
                    if ((i + 1) >= argc) {
                        fprintf(stderr, "Error: --match requires a pattern value\n");
                        goto cleanup;
                    }
                    value = argv[++i];
                } else {
                    value = arg + 8;
                }
                if (!value || value[0] == '\0') {
                    fprintf(stderr, "Error: --match requires a non-empty pattern\n");
                    goto cleanup;
                }
                match_pattern = value;
            } else if (strcmp(arg, "--timeout-ms") == 0 || strncmp(arg, "--timeout-ms=", 13) == 0) {
                const char* value = NULL;
                if (strcmp(arg, "--timeout-ms") == 0) {
                    if ((i + 1) >= argc) {
                        fprintf(stderr, "Error: --timeout-ms requires an integer value\n");
                        goto cleanup;
                    }
                    value = argv[++i];
                } else {
                    value = arg + 13;
                }
                if (!value || value[0] == '\0') {
                    fprintf(stderr, "Error: --timeout-ms requires an integer value\n");
                    goto cleanup;
                }
                char* end_ptr = NULL;
                long long parsed = strtoll(value, &end_ptr, 10);
                if (!end_ptr || *end_ptr != '\0' || parsed < 0 || parsed > INT_MAX) {
                    fprintf(stderr, "Error: invalid --timeout-ms value '%s'\n", value);
                    goto cleanup;
                }
                timeout_ms = (int)parsed;
            } else if (strcmp(arg, "--jobs") == 0 || strncmp(arg, "--jobs=", 7) == 0) {
                const char* value = NULL;
                if (strcmp(arg, "--jobs") == 0) {
                    if ((i + 1) >= argc) {
                        fprintf(stderr, "Error: --jobs requires an integer value\n");
                        goto cleanup;
                    }
                    value = argv[++i];
                } else {
                    value = arg + 7;
                }
                if (!value || value[0] == '\0') {
                    fprintf(stderr, "Error: --jobs requires an integer value\n");
                    goto cleanup;
                }
                char* end_ptr = NULL;
                long long parsed = strtoll(value, &end_ptr, 10);
                if (!end_ptr || *end_ptr != '\0' || parsed < 1 || parsed > 256) {
                    fprintf(stderr, "Error: invalid --jobs value '%s' (expected 1..256)\n", value);
                    goto cleanup;
                }
                jobs = (int)parsed;
            } else if (strcmp(arg, "--rerun-failed") == 0 || strncmp(arg, "--rerun-failed=", 15) == 0) {
                const char* value = NULL;
                if (strcmp(arg, "--rerun-failed") == 0) {
                    if ((i + 1) >= argc) {
                        fprintf(stderr, "Error: --rerun-failed requires an integer value\n");
                        goto cleanup;
                    }
                    value = argv[++i];
                } else {
                    value = arg + 15;
                }
                if (!value || value[0] == '\0') {
                    fprintf(stderr, "Error: --rerun-failed requires an integer value\n");
                    goto cleanup;
                }
                char* end_ptr = NULL;
                long long parsed = strtoll(value, &end_ptr, 10);
                if (!end_ptr || *end_ptr != '\0' || parsed < 0 || parsed > 32) {
                    fprintf(stderr, "Error: invalid --rerun-failed value '%s' (expected 0..32)\n", value);
                    goto cleanup;
                }
                rerun_failed = (int)parsed;
            } else if (strcmp(arg, "--shard") == 0 || strncmp(arg, "--shard=", 8) == 0) {
                const char* value = NULL;
                if (strcmp(arg, "--shard") == 0) {
                    if ((i + 1) >= argc) {
                        fprintf(stderr, "Error: --shard requires '<index>/<total>'\n");
                        goto cleanup;
                    }
                    value = argv[++i];
                } else {
                    value = arg + 8;
                }
                if (!value || value[0] == '\0') {
                    fprintf(stderr, "Error: --shard requires '<index>/<total>'\n");
                    goto cleanup;
                }

                const char* slash = strchr(value, '/');
                if (!slash || slash == value || slash[1] == '\0') {
                    fprintf(stderr, "Error: invalid --shard value '%s' (expected '<index>/<total>')\n", value);
                    goto cleanup;
                }

                char index_buf[32];
                size_t index_len = (size_t)(slash - value);
                if (index_len >= sizeof(index_buf)) {
                    fprintf(stderr, "Error: invalid --shard value '%s'\n", value);
                    goto cleanup;
                }
                memcpy(index_buf, value, index_len);
                index_buf[index_len] = '\0';
                const char* total_str = slash + 1;

                char* end_a = NULL;
                char* end_b = NULL;
                long long parsed_index = strtoll(index_buf, &end_a, 10);
                long long parsed_total = strtoll(total_str, &end_b, 10);
                if (!end_a || *end_a != '\0' || !end_b || *end_b != '\0' ||
                    parsed_total < 1 || parsed_total > INT_MAX ||
                    parsed_index < 1 || parsed_index > parsed_total) {
                    fprintf(stderr, "Error: invalid --shard value '%s' (index must be 1..total)\n", value);
                    goto cleanup;
                }
                shard_index = (int)parsed_index;
                shard_total = (int)parsed_total;
            } else {
                const char* parse_err = NULL;
                int parsed_limit = cli_parse_runtime_limit_option(argc, argv, &i, &options, &parse_err);
                if (parsed_limit > 0) {
                    continue;
                }
                if (parsed_limit < 0) {
                    fprintf(stderr, "%s: '%s'\n", parse_err ? parse_err : "Error: invalid runtime limit option", arg);
                    goto cleanup;
                }
                if (arg[0] == '-') {
                    fprintf(stderr, "Error: Unknown test option '%s'\n", arg);
                    goto cleanup;
                }
                if (!string_list_push_unique(&targets, arg)) {
                    fprintf(stderr, "Error: Out of memory while parsing test paths\n");
                    goto cleanup;
                }
                continue;
            }
            continue;
        }
    }

    if (use_ctest) {
        if (targets.count > 0) {
            fprintf(stderr, "Error: '--ctest' does not accept explicit test file paths\n");
            goto cleanup;
        }
        if (list_only || json_output || match_pattern != NULL || timeout_ms > 0 || jobs != 1 ||
            rerun_failed > 0 || shard_total != 1 || junit_path != NULL || extension_paths.count > 0 ||
            options.capabilities.deny_file_io || options.capabilities.deny_network ||
            options.capabilities.deny_process || options.capabilities.deny_sqlite ||
            options.capabilities.deny_threading ||
            options.max_open_files > 0 || options.max_open_sockets > 0) {
            fprintf(stderr, "Error: '--ctest' cannot be combined with test-runner specific options\n");
            goto cleanup;
        }
        exit_code = cli_test_via_ctest();
        goto cleanup;
    }

    if (list_only && junit_path != NULL) {
        fprintf(stderr, "Error: --junit cannot be combined with --list\n");
        goto cleanup;
    }

    int scan_errors = 0;
    int skipped_non_tablo = 0;
    int skipped_bad_signature = 0;
    int legacy_main_count = 0;
    int allow_legacy_main = targets.count != 0;

    if (targets.count == 0) {
        const char* default_dir = NULL;
        if (cli_path_is_directory("tests/tablo_tests")) {
            default_dir = "tests/tablo_tests";
        } else if (cli_path_is_directory("tablo_tests")) {
            default_dir = "tablo_tests";
        } else if (cli_path_is_directory("tests")) {
            default_dir = "tests";
        }

        if (!default_dir) {
            fprintf(stderr, "Error: default test directory 'tests/tablo_tests' not found\n");
            goto cleanup;
        }
        cli_collect_tests_from_dir(default_dir, &files, &scan_errors);
    } else {
        for (int i = 0; i < targets.count; i++) {
            cli_collect_tests_from_target(targets.items[i], &files, &scan_errors, &skipped_non_tablo);
        }
    }

    string_list_sort(&files);

    if (files.count == 0) {
        fprintf(stderr, "No Tablo test files found. Use '*_test.tblo' naming or pass explicit .tblo paths.\n");
        goto cleanup;
    }

    options.typecheck.warn_unused_error = warn_unused_error;
    options.typecheck.strict_errors = strict_errors;
    options.extension_paths = (const char* const*)extension_paths.items;
    options.extension_path_count = extension_paths.count;

    for (int i = 0; i < files.count; i++) {
        const char* path = files.items[i];
        StringList function_names;
        string_list_init(&function_names);

        int used_legacy_main = 0;
        int file_bad_signature = 0;
        char discovery_error[1024];
        discovery_error[0] = '\0';

        int discovered_ok = cli_discover_test_functions(path,
                                                        options,
                                                        allow_legacy_main,
                                                        &function_names,
                                                        &used_legacy_main,
                                                        &file_bad_signature,
                                                        discovery_error,
                                                        sizeof(discovery_error));

        skipped_bad_signature += file_bad_signature;
        if (used_legacy_main) legacy_main_count++;

        if (!discovered_ok) {
            if (!cli_test_case_list_push(&discovered_cases, path, "<load>", discovery_error, 0)) {
                fprintf(stderr, "Error: Out of memory while tracking discovered tests\n");
                string_list_free(&function_names);
                goto cleanup;
            }
            string_list_free(&function_names);
            continue;
        }

        if (function_names.count == 0) {
            if (!json_output && allow_legacy_main) {
                fprintf(stderr, "Warning: No runnable tests found in '%s'\n", path);
            }
            string_list_free(&function_names);
            continue;
        }

        string_list_sort(&function_names);
        for (int fn_i = 0; fn_i < function_names.count; fn_i++) {
            const char* function_name = function_names.items[fn_i];
            int is_legacy_main = used_legacy_main && strcmp(function_name, "main") == 0;
            if (!cli_test_case_list_push(&discovered_cases, path, function_name, NULL, is_legacy_main)) {
                fprintf(stderr, "Error: Out of memory while tracking discovered tests\n");
                string_list_free(&function_names);
                goto cleanup;
            }
        }

        string_list_free(&function_names);
    }

    if (discovered_cases.count == 0) {
        fprintf(stderr, "No runnable Tablo tests discovered.\n");
        goto cleanup;
    }

    for (int i = 0; i < discovered_cases.count; i++) {
        CliTestCase* test_case = &discovered_cases.items[i];
        if (!cli_test_case_matches_pattern(test_case, match_pattern)) continue;
        if (!cli_test_case_list_push(&matched_cases,
                                     test_case->file_path,
                                     test_case->function_name,
                                     test_case->discovery_error,
                                     test_case->legacy_main)) {
            fprintf(stderr, "Error: Out of memory while selecting matched tests\n");
            goto cleanup;
        }
    }

    if (matched_cases.count == 0) {
        if (match_pattern && match_pattern[0] != '\0') {
            fprintf(stderr, "No tests matched pattern '%s'.\n", match_pattern);
        } else {
            fprintf(stderr, "No tests selected.\n");
        }
        goto cleanup;
    }

    for (int i = 0; i < matched_cases.count; i++) {
        CliTestCase* test_case = &matched_cases.items[i];
        if (shard_total > 1) {
            uint64_t h = cli_hash_string64(test_case->id);
            int bucket = (int)(h % (uint64_t)shard_total) + 1;
            if (bucket != shard_index) continue;
        }
        if (!cli_test_case_list_push(&selected_cases,
                                     test_case->file_path,
                                     test_case->function_name,
                                     test_case->discovery_error,
                                     test_case->legacy_main)) {
            fprintf(stderr, "Error: Out of memory while applying shard selection\n");
            goto cleanup;
        }
    }

    if (selected_cases.count == 0) {
        if (shard_total > 1 && matched_cases.count > 0) {
            if (json_output) {
                printf("{\"mode\":\"%s\",", list_only ? "list" : "run");
                printf("\"testsDiscovered\":%d,", discovered_cases.count);
                printf("\"testsSelected\":0,");
                printf("\"scanErrors\":%d,", scan_errors);
                printf("\"skipped\":%d,", skipped_non_tablo + skipped_bad_signature + matched_cases.count);
                printf("\"shard\":\"%d/%d\"", shard_index, shard_total);
                if (!list_only) {
                    printf(",\"testsExecuted\":0,\"passed\":0,\"failed\":0,\"timedOut\":0,\"crashed\":0,\"durationMs\":0.00,\"timeoutMs\":%d,\"jobs\":%d,\"rerunFailed\":%d,\"results\":[]",
                           timeout_ms,
                           jobs,
                           rerun_failed);
                } else {
                    printf(",\"tests\":[]");
                }
                printf("}\n");
            } else {
                printf("No tests assigned to shard %d/%d.\n", shard_index, shard_total);
            }
            exit_code = (scan_errors > 0) ? 1 : 0;
            goto cleanup;
        }

        if (match_pattern && match_pattern[0] != '\0') {
            fprintf(stderr, "No tests matched pattern '%s'.\n", match_pattern);
        } else {
            fprintf(stderr, "No tests selected.\n");
        }
        goto cleanup;
    }

    int filtered_out = discovered_cases.count - selected_cases.count;
    if (filtered_out < 0) filtered_out = 0;

    if (list_only) {
        int selected_errors = 0;
        if (json_output) {
            printf("{\"mode\":\"list\",");
            printf("\"testsDiscovered\":%d,", discovered_cases.count);
            printf("\"testsSelected\":%d,", selected_cases.count);
            printf("\"scanErrors\":%d,", scan_errors);
            printf("\"skipped\":%d,", skipped_non_tablo + skipped_bad_signature + filtered_out);
            printf("\"shard\":\"%d/%d\",", shard_index, shard_total);
            printf("\"tests\":[");
            for (int i = 0; i < selected_cases.count; i++) {
                CliTestCase* test_case = &selected_cases.items[i];
                if (test_case->discovery_error) selected_errors++;
                if (i > 0) printf(",");
                printf("{\"id\":");
                cli_json_write_string(test_case->id);
                printf(",\"file\":");
                cli_json_write_string(test_case->file_path);
                printf(",\"function\":");
                cli_json_write_string(test_case->function_name);
                printf(",\"legacyMain\":%s", test_case->legacy_main ? "true" : "false");
                if (test_case->discovery_error) {
                    printf(",\"status\":\"error\",\"error\":");
                    cli_json_write_string(test_case->discovery_error);
                } else {
                    printf(",\"status\":\"listed\"");
                }
                printf("}");
            }
            printf("]}\n");
        } else {
            printf("Discovered %d Tablo test(s), selected %d:\n",
                   discovered_cases.count,
                   selected_cases.count);
            if (shard_total > 1) {
                printf("Shard: %d/%d\n", shard_index, shard_total);
            }
            for (int i = 0; i < selected_cases.count; i++) {
                CliTestCase* test_case = &selected_cases.items[i];
                if (test_case->discovery_error) {
                    selected_errors++;
                    printf("  [ERROR] %s\n", test_case->id);
                    printf("          %s\n", test_case->discovery_error);
                } else if (test_case->legacy_main) {
                    printf("  %s (legacy main fallback)\n", test_case->id);
                } else {
                    printf("  %s\n", test_case->id);
                }
            }
            if (scan_errors > 0) {
                printf("Scan errors: %d\n", scan_errors);
            }
        }

        exit_code = (selected_errors > 0 || scan_errors > 0) ? 1 : 0;
        goto cleanup;
    }

    if (!json_output) {
        printf("Running %d Tablo test(s) from %d file(s) with %d job(s)...\n",
               selected_cases.count,
               files.count,
               jobs);
    }

    active_children = (CliChildProcess*)calloc((size_t)jobs, sizeof(CliChildProcess));
    if (!active_children) {
        fprintf(stderr, "Error: Out of memory while allocating child process slots\n");
        goto cleanup;
    }
    active_children_capacity = jobs;
    for (int i = 0; i < jobs; i++) {
        cli_child_process_init(&active_children[i]);
    }

    int* pending_indices = (int*)malloc((size_t)selected_cases.count * sizeof(int));
    int* retry_indices = (int*)malloc((size_t)selected_cases.count * sizeof(int));
    int* failed_this_attempt = (int*)calloc((size_t)selected_cases.count, sizeof(int));
    if (!pending_indices || !retry_indices || !failed_this_attempt) {
        free(pending_indices);
        free(retry_indices);
        free(failed_this_attempt);
        fprintf(stderr, "Error: Out of memory while scheduling test runs\n");
        goto cleanup;
    }

    for (int i = 0; i < selected_cases.count; i++) {
        pending_indices[i] = i;
    }
    int pending_count = selected_cases.count;

    double suite_start_ms = cli_now_ms();
    for (int attempt = 0; attempt <= rerun_failed && pending_count > 0; attempt++) {
        int next_case = 0;
        int active_count = 0;
        int stop_launch = 0;
        int attempt_non_pass = 0;
        memset(failed_this_attempt, 0, (size_t)selected_cases.count * sizeof(int));

        if (attempt > 0 && !json_output) {
            printf("Retry attempt %d/%d: rerunning %d failing test(s)...\n",
                   attempt,
                   rerun_failed,
                   pending_count);
        }

        while (next_case < pending_count || active_count > 0) {
            while (!stop_launch && next_case < pending_count && active_count < jobs) {
                int test_index = pending_indices[next_case++];
                CliTestCase* test_case = &selected_cases.items[test_index];

                if (test_case->discovery_error) {
                    if (!cli_run_result_list_push(&run_results,
                                                  test_case->id,
                                                  test_case->file_path,
                                                  test_case->function_name,
                                                  CLI_TEST_RUN_FAIL,
                                                  test_index,
                                                  1,
                                                  0.0,
                                                  test_case->discovery_error)) {
                        free(pending_indices);
                        free(retry_indices);
                        free(failed_this_attempt);
                        fprintf(stderr, "Error: Out of memory while recording test results\n");
                        goto cleanup;
                    }
                    attempt_non_pass++;
                    failed_this_attempt[test_index] = 1;
                    if (!json_output) {
                        printf("  [FAIL] %s (0.00 ms)\n", test_case->id);
                        printf("         %s\n", test_case->discovery_error);
                    }
                    if (fail_fast) {
                        stop_launch = 1;
                    }
                    continue;
                }

                int slot = -1;
                for (int s = 0; s < jobs; s++) {
                    if (!active_children[s].active) {
                        slot = s;
                        break;
                    }
                }
                if (slot < 0) break;

                char spawn_error[512];
                spawn_error[0] = '\0';
                if (!cli_spawn_test_child_process(program_path,
                                                  test_case,
                                                  options,
                                                  &active_children[slot],
                                                  spawn_error,
                                                  sizeof(spawn_error))) {
                    if (!cli_run_result_list_push(&run_results,
                                                  test_case->id,
                                                  test_case->file_path,
                                                  test_case->function_name,
                                                  CLI_TEST_RUN_FAIL,
                                                  test_index,
                                                  1,
                                                  0.0,
                                                  spawn_error)) {
                        free(pending_indices);
                        free(retry_indices);
                        free(failed_this_attempt);
                        fprintf(stderr, "Error: Out of memory while recording test results\n");
                        goto cleanup;
                    }
                    attempt_non_pass++;
                    failed_this_attempt[test_index] = 1;
                    if (!json_output) {
                        printf("  [FAIL] %s (0.00 ms)\n", test_case->id);
                        if (spawn_error[0] != '\0') {
                            printf("         %s\n", spawn_error);
                        }
                    }
                    if (fail_fast) {
                        stop_launch = 1;
                    }
                    continue;
                }
                active_count++;
            }

            if (active_count == 0) {
                break;
            }

            int any_finished = 0;
            for (int s = 0; s < jobs; s++) {
                CliChildProcess* child = &active_children[s];
                if (!child->active) continue;
                const CliTestCase* completed_case = child->test_case;
                int test_index = -1;
                if (completed_case &&
                    completed_case >= selected_cases.items &&
                    completed_case < (selected_cases.items + selected_cases.count)) {
                    test_index = (int)(completed_case - selected_cases.items);
                }

                int finished = 0;
                CliTestRunStatus status = CLI_TEST_RUN_FAIL;
                int child_exit_code = 0;
                double elapsed_ms = 0.0;
                char error_text[2048];
                error_text[0] = '\0';

                if (!cli_poll_test_child_process(child,
                                                 timeout_ms,
                                                 &finished,
                                                 &status,
                                                 &child_exit_code,
                                                 &elapsed_ms,
                                                 error_text,
                                                 sizeof(error_text))) {
                    finished = 1;
                    status = CLI_TEST_RUN_CRASH;
                    child_exit_code = 1;
                    elapsed_ms = 0.0;
                    cli_set_error_text(error_text, sizeof(error_text), "Internal error while polling child process");
                    cli_child_process_free(child);
                }

                if (!finished) {
                    continue;
                }
                any_finished = 1;
                active_count--;

                if (!cli_run_result_list_push(&run_results,
                                              completed_case ? completed_case->id : "<unknown>",
                                              completed_case ? completed_case->file_path : "<unknown>",
                                              completed_case ? completed_case->function_name : "<unknown>",
                                              status,
                                              test_index,
                                              child_exit_code,
                                              elapsed_ms,
                                              error_text)) {
                    for (int k = 0; k < jobs; k++) {
                        cli_terminate_test_child_process(&active_children[k]);
                    }
                    free(pending_indices);
                    free(retry_indices);
                    free(failed_this_attempt);
                    fprintf(stderr, "Error: Out of memory while recording test results\n");
                    goto cleanup;
                }

                if (test_index >= 0 && test_index < selected_cases.count && status != CLI_TEST_RUN_PASS) {
                    failed_this_attempt[test_index] = 1;
                    attempt_non_pass++;
                }

                if (!json_output) {
                    printf("  [%s] %s (%.2f ms)\n",
                           cli_test_status_to_label(status),
                           completed_case ? completed_case->id : "<unknown>",
                           elapsed_ms);
                    if (status != CLI_TEST_RUN_PASS && error_text[0] != '\0') {
                        printf("         %s\n", error_text);
                    }
                }

                if (fail_fast && status != CLI_TEST_RUN_PASS) {
                    stop_launch = 1;
                    for (int k = 0; k < jobs; k++) {
                        if (active_children[k].active) {
                            cli_terminate_test_child_process(&active_children[k]);
                        }
                    }
                    active_count = 0;
                    break;
                }
            }

            if (!any_finished && active_count > 0) {
                cli_sleep_ms(2);
            }
        }

        if (attempt_non_pass == 0) {
            break;
        }
        if (fail_fast) {
            break;
        }
        if (attempt >= rerun_failed) {
            break;
        }

        int retry_count = 0;
        for (int i = 0; i < pending_count; i++) {
            int idx = pending_indices[i];
            if (idx >= 0 && idx < selected_cases.count && failed_this_attempt[idx]) {
                retry_indices[retry_count++] = idx;
            }
        }
        pending_count = retry_count;
        if (pending_count > 0) {
            memcpy(pending_indices, retry_indices, (size_t)pending_count * sizeof(int));
        }
    }

    free(pending_indices);
    free(retry_indices);
    free(failed_this_attempt);

    free(active_children);
    active_children = NULL;
    active_children_capacity = 0;

    final_last_idx = (int*)malloc((size_t)selected_cases.count * sizeof(int));
    attempt_counts = (int*)malloc((size_t)selected_cases.count * sizeof(int));
    if (!final_last_idx || !attempt_counts) {
        fprintf(stderr, "Error: Out of memory while finalizing test results\n");
        goto cleanup;
    }
    for (int i = 0; i < selected_cases.count; i++) {
        final_last_idx[i] = -1;
        attempt_counts[i] = 0;
    }

    for (int i = 0; i < run_results.count; i++) {
        int idx = run_results.items[i].test_index;
        if (idx < 0 || idx >= selected_cases.count) continue;
        final_last_idx[idx] = i;
        attempt_counts[idx]++;
    }

    int passed = 0;
    int failed = 0;
    int timed_out = 0;
    int crashed = 0;
    int executed = 0;
    for (int i = 0; i < selected_cases.count; i++) {
        int ridx = final_last_idx[i];
        if (ridx < 0 || ridx >= run_results.count) {
            continue;
        }
        executed++;
        CliTestRunStatus status = run_results.items[ridx].status;
        if (status == CLI_TEST_RUN_PASS) {
            passed++;
        } else if (status == CLI_TEST_RUN_TIMEOUT) {
            timed_out++;
        } else if (status == CLI_TEST_RUN_CRASH) {
            crashed++;
        } else {
            failed++;
        }
    }

    int skipped = skipped_non_tablo + skipped_bad_signature + filtered_out;
    if (executed < selected_cases.count) {
        skipped += (selected_cases.count - executed);
    }

    double suite_end_ms = cli_now_ms();
    double total_ms = (suite_end_ms >= suite_start_ms) ? (suite_end_ms - suite_start_ms) : 0.0;

    if (json_output) {
        printf("{\"mode\":\"run\",");
        printf("\"testsDiscovered\":%d,", discovered_cases.count);
        printf("\"testsSelected\":%d,", selected_cases.count);
        printf("\"testsExecuted\":%d,", executed);
        printf("\"passed\":%d,", passed);
        printf("\"failed\":%d,", failed);
        printf("\"timedOut\":%d,", timed_out);
        printf("\"crashed\":%d,", crashed);
        printf("\"skipped\":%d,", skipped);
        printf("\"scanErrors\":%d,", scan_errors);
        printf("\"durationMs\":%.2f,", total_ms);
        printf("\"timeoutMs\":%d,", timeout_ms);
        printf("\"jobs\":%d,", jobs);
        printf("\"rerunFailed\":%d,", rerun_failed);
        printf("\"shard\":\"%d/%d\",", shard_index, shard_total);
        printf("\"results\":[");
        for (int i = 0; i < selected_cases.count; i++) {
            CliTestCase* test_case = &selected_cases.items[i];
            int ridx = final_last_idx[i];
            int attempts = attempt_counts[i];
            if (i > 0) printf(",");
            printf("{\"id\":");
            cli_json_write_string(test_case->id);
            printf(",\"file\":");
            cli_json_write_string(test_case->file_path);
            printf(",\"function\":");
            cli_json_write_string(test_case->function_name);
            if (ridx < 0 || ridx >= run_results.count) {
                printf(",\"status\":\"%s\"", cli_test_status_to_text(CLI_TEST_RUN_SKIPPED));
                printf(",\"exitCode\":0");
                printf(",\"durationMs\":0.00");
                printf(",\"attempts\":%d", attempts);
                printf(",\"error\":");
                cli_json_write_string("Not executed");
                printf("}");
                continue;
            }

            CliRunResult* result = &run_results.items[ridx];
            printf(",\"status\":\"%s\"", cli_test_status_to_text(result->status));
            printf(",\"exitCode\":%d", result->child_exit_code);
            printf(",\"durationMs\":%.2f", result->elapsed_ms);
            printf(",\"attempts\":%d", attempts);
            if (result->error_text) {
                printf(",\"error\":");
                cli_json_write_string(result->error_text);
            }
            printf("}");
        }
        printf("]}\n");
    } else {
        printf("\nTablo test summary:\n");
        printf("  Total discovered: %d\n", discovered_cases.count);
        printf("  Selected: %d\n", selected_cases.count);
        printf("  Executed: %d\n", executed);
        printf("  Passed: %d\n", passed);
        printf("  Failed: %d\n", failed);
        printf("  Timed out: %d\n", timed_out);
        printf("  Crashed: %d\n", crashed);
        printf("  Skipped: %d\n", skipped);
        printf("  Duration: %.2f ms\n", total_ms);
        printf("  Jobs: %d\n", jobs);
        printf("  Rerun failed: %d\n", rerun_failed);
        printf("  Shard: %d/%d\n", shard_index, shard_total);
        if (timeout_ms > 0) {
            printf("  Timeout per test: %d ms\n", timeout_ms);
        }
        if (legacy_main_count > 0) {
            printf("  Legacy main fallback files: %d\n", legacy_main_count);
        }
        if (scan_errors > 0) {
            printf("  Scan errors: %d\n", scan_errors);
        }
        if (skipped_bad_signature > 0) {
            printf("  Skipped invalid test signatures: %d\n", skipped_bad_signature);
        }
    }

    if (junit_path != NULL) {
        if (!cli_write_junit_report(junit_path,
                                    &selected_cases,
                                    &run_results,
                                    final_last_idx,
                                    attempt_counts,
                                    total_ms,
                                    rerun_failed,
                                    shard_index,
                                    shard_total)) {
            fprintf(stderr, "Error: Failed to write JUnit report '%s'\n", junit_path);
            exit_code = 1;
            goto cleanup;
        }
    }

    exit_code = (failed > 0 || timed_out > 0 || crashed > 0 || scan_errors > 0) ? 1 : 0;

cleanup:
    if (active_children) {
        for (int i = 0; i < active_children_capacity; i++) {
            cli_terminate_test_child_process(&active_children[i]);
        }
        free(active_children);
        active_children = NULL;
    }
    string_list_free(&targets);
    string_list_free(&files);
    string_list_free(&extension_paths);
    cli_test_case_list_free(&discovered_cases);
    cli_test_case_list_free(&matched_cases);
    cli_test_case_list_free(&selected_cases);
    cli_run_result_list_free(&run_results);
    if (final_last_idx) free(final_last_idx);
    if (attempt_counts) free(attempt_counts);
    return exit_code;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        print_version();
        return 0;
    }

    if (strcmp(argv[1], "__run-test-case") == 0) {
        return cli_internal_run_test_case(argc, argv);
    }
    
    if (strcmp(argv[1], "run") == 0) {
        return cli_run(argc, argv);
    } else if (strcmp(argv[1], "debug") == 0) {
        return cli_debug(argc, argv);
    } else if (strcmp(argv[1], "compile") == 0) {
        return cli_compile(argc, argv);
    } else if (strcmp(argv[1], "test") == 0) {
        return cli_test(argc, argv);
    } else if (strcmp(argv[1], "mod") == 0) {
        return cli_mod(argc, argv);
    } else if (strcmp(argv[1], "lsp") == 0) {
        return cli_lsp(argc, argv);
    } else if (strcmp(argv[1], "dap") == 0) {
        return cli_dap(argc, argv);
    } else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    } else {
        fprintf(stderr, "Error: Unknown command '%s'\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }
}
