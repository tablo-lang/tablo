#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct {
    Lexer lexer;
    Token current;
    Token previous;
    bool had_error;
    bool panic_mode;
    bool report_diagnostics;
    char* first_error_message;
    int error_line;
    int error_column;
    int depth;
    int max_depth;
    char** active_type_params;
    int active_type_param_count;
    int synthetic_counter;
    bool current_function_is_async;
} Parser;

#define PARSER_MAX_DEPTH 256

#define PARSER_ENTER(p) \
    do { \
        (p)->depth++; \
        if ((p)->depth > (p)->max_depth) { \
            parser_error((p), "Maximum recursion depth exceeded"); \
            return NULL; \
        } \
    } while (0)

#define PARSER_LEAVE(p) \
    do { \
        (p)->depth--; \
    } while (0)

typedef struct {
    Program* program;
    Error* error;
} ParseResult;

ParseResult parser_parse(const char* source, const char* file);
ParseResult parser_parse_quiet(const char* source, const char* file);
void parser_free_result(ParseResult* result);
void parser_free_parse_only_result(ParseResult* result);

#endif
