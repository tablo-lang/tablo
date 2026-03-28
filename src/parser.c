#include "parser.h"
#include "safe_alloc.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    Type** items;
    int count;
    int capacity;
} ParserTypeRootList;

static void parser_type_root_list_add(ParserTypeRootList* list, Type* type);
static void parser_collect_expr_type_roots(ParserTypeRootList* list, Expr* expr);
static void parser_collect_stmt_type_roots(ParserTypeRootList* list, Stmt* stmt);
static void parser_collect_program_type_roots(ParserTypeRootList* list, Program* program);
static void parser_release_parse_only_expr_type_arrays(Expr* expr);
static void parser_release_parse_only_stmt_type_arrays(Stmt* stmt);
static void parser_release_parse_only_program_type_arrays(Program* program);
static ParseResult parser_parse_internal(const char* source, const char* file, bool report_diagnostics);
static void parser_discard_parse_only_expr(Expr* expr);
static void parser_discard_parse_only_stmt(Stmt* stmt);

static void parser_describe_token(const Token* token, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    if (!token) {
        snprintf(out, out_size, "unknown token");
        return;
    }

    if (token->type == TOKEN_EOF) {
        snprintf(out, out_size, "end of file");
        return;
    }

    const char* token_name = token_type_to_string(token->type);
    if (token->lexeme && token->lexeme[0] != '\0') {
        char snippet[48];
        int max_chars = 32;
        int lexeme_len = (int)strlen(token->lexeme);
        if (lexeme_len > max_chars) {
            snprintf(snippet, sizeof(snippet), "%.*s...", max_chars, token->lexeme);
        } else {
            snprintf(snippet, sizeof(snippet), "%s", token->lexeme);
        }
        snprintf(out, out_size, "%s '%s'", token_name, snippet);
        return;
    }

    snprintf(out, out_size, "%s", token_name);
}

static void parser_print_source_context(Parser* parser, int line, int column) {
    if (!parser || !parser->lexer.source || line <= 0) return;

    const char* p = parser->lexer.source;
    const char* line_start = p;
    int current_line = 1;
    while (*p && current_line < line) {
        if (*p == '\n') {
            current_line++;
            line_start = p + 1;
        }
        p++;
    }
    if (current_line != line) return;

    const char* line_end = line_start;
    while (*line_end && *line_end != '\n' && *line_end != '\r') {
        line_end++;
    }

    int line_len = (int)(line_end - line_start);
    int max_len = 200;
    if (line_len > max_len) {
        line_len = max_len;
    }
    if (line_len > 0) {
        fprintf(stderr, "    %.*s\n", line_len, line_start);
    }

    int caret_col = column > 0 ? column : 1;
    if (caret_col > line_len + 1) {
        caret_col = line_len + 1;
    }
    fprintf(stderr, "    %*s^\n", caret_col - 1, "");
}

static void parser_record_first_error(Parser* parser, const char* message) {
    if (!parser || parser->first_error_message) return;

    const char* file = parser->lexer.file ? parser->lexer.file : "<unknown>";
    int line = parser->current.line;
    int column = parser->current.column;
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "%s:%d:%d: %s", file, line, column, message ? message : "Syntax error");

    parser->first_error_message = safe_strdup(buffer);
    parser->error_line = line;
    parser->error_column = column;
}

static void parser_error_at(Parser* parser, const char* message, int line, int column) {
    if (!parser || parser->panic_mode) return;

    parser->had_error = true;
    parser->panic_mode = true;

    const char* file = parser->lexer.file ? parser->lexer.file : "<unknown>";
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "%s:%d:%d: %s", file, line, column, message ? message : "Syntax error");

    if (!parser->first_error_message) {
        parser->first_error_message = safe_strdup(buffer);
        parser->error_line = line;
        parser->error_column = column;
    }

    if (parser->report_diagnostics) {
        fprintf(stderr, "Syntax error at %s:%d:%d: %s\n", file, line, column, message ? message : "Syntax error");
        parser_print_source_context(parser, line, column);
    }
}

static void parser_error(Parser* parser, const char* message) {
    if (!parser || parser->panic_mode) return;
    parser_record_first_error(parser, message);
    parser_error_at(parser, message, parser->current.line, parser->current.column);
}

static void parser_discard_parse_only_expr(Expr* expr) {
    if (!expr) return;

    ParserTypeRootList type_roots = {0};
    parser_collect_expr_type_roots(&type_roots, expr);
    parser_release_parse_only_expr_type_arrays(expr);
    expr_free(expr);

    for (int i = 0; i < type_roots.count; i++) {
        type_free(type_roots.items[i]);
    }
    free(type_roots.items);
}

static void parser_discard_parse_only_stmt(Stmt* stmt) {
    if (!stmt) return;

    ParserTypeRootList type_roots = {0};
    parser_collect_stmt_type_roots(&type_roots, stmt);
    parser_release_parse_only_stmt_type_arrays(stmt);
    stmt_free(stmt);

    for (int i = 0; i < type_roots.count; i++) {
        type_free(type_roots.items[i]);
    }
    free(type_roots.items);
}

static void parser_advance(Parser* parser) {
    if (parser->current.type != TOKEN_EOF) {
        // Free the previous token we're about to overwrite.
        token_free(&parser->previous);

        parser->previous = parser->current;
        parser->current = lexer_next_token(&parser->lexer);
        
        if (parser->current.type == TOKEN_ERROR) {
            parser_error(parser, parser->current.lexeme);
        }
    }
}

static void parser_consume(Parser* parser, TokenType type, const char* message) {
    if (parser->current.type == type) {
        parser_advance(parser);
        return;
    }
    char got[128];
    parser_describe_token(&parser->current, got, sizeof(got));

    char detailed[512];
    snprintf(detailed, sizeof(detailed), "%s (got %s)", message, got);
    parser_error(parser, detailed);
}

static bool parser_match(Parser* parser, TokenType type) {
    if (parser->current.type == type) {
        parser_advance(parser);
        return true;
    }
    return false;
}

static bool parser_token_is_identifier_like(TokenType type) {
    if (type == TOKEN_IDENTIFIER) return true;
    if (type >= TOKEN_KEYWORD_INT && type <= TOKEN_KEYWORD_SET) return true;
    if (type == TOKEN_TRUE || type == TOKEN_FALSE) return true;
    if (type == TOKEN_AS || type == TOKEN_IN) return true;
    return false;
}

static bool parser_token_starts_type(TokenType type) {
    switch (type) {
        case TOKEN_IDENTIFIER:
        case TOKEN_KEYWORD_INT:
        case TOKEN_KEYWORD_BOOL:
        case TOKEN_KEYWORD_DOUBLE:
        case TOKEN_KEYWORD_BIGINT:
        case TOKEN_KEYWORD_STRING:
        case TOKEN_KEYWORD_BYTES:
        case TOKEN_KEYWORD_ARRAY:
        case TOKEN_KEYWORD_ANY:
        case TOKEN_KEYWORD_NIL:
        case TOKEN_KEYWORD_FUNC:
        case TOKEN_KEYWORD_MAP:
        case TOKEN_KEYWORD_SET:
            return true;
        default:
            return false;
    }
}

static bool parser_token_starts_declaration(TokenType type) {
    switch (type) {
        case TOKEN_KEYWORD_PUBLIC:
        case TOKEN_KEYWORD_PRIVATE:
        case TOKEN_KEYWORD_VAR:
        case TOKEN_KEYWORD_CONST:
        case TOKEN_KEYWORD_TYPE:
        case TOKEN_KEYWORD_INTERFACE:
        case TOKEN_KEYWORD_IMPL:
        case TOKEN_KEYWORD_ASYNC:
        case TOKEN_KEYWORD_FUNC:
        case TOKEN_KEYWORD_RECORD:
        case TOKEN_KEYWORD_ENUM:
        case TOKEN_KEYWORD_IMPORT:
            return true;
        default:
            return false;
    }
}

static bool parser_token_starts_non_expression_statement(TokenType type) {
    switch (type) {
        case TOKEN_KEYWORD_IF:
        case TOKEN_KEYWORD_MATCH:
        case TOKEN_KEYWORD_SWITCH:
        case TOKEN_KEYWORD_WHILE:
        case TOKEN_KEYWORD_FOREACH:
        case TOKEN_KEYWORD_BREAK:
        case TOKEN_KEYWORD_CONTINUE:
        case TOKEN_KEYWORD_RETURN:
        case TOKEN_KEYWORD_DEFER:
            return true;
        default:
            return false;
    }
}

static bool parser_token_starts_expression(TokenType type) {
    switch (type) {
        case TOKEN_NUMBER_INT:
        case TOKEN_NUMBER_BIGINT:
        case TOKEN_NUMBER_DOUBLE:
        case TOKEN_STRING:
        case TOKEN_TRUE:
        case TOKEN_FALSE:
        case TOKEN_KEYWORD_NIL:
        case TOKEN_KEYWORD_FUNC:
        case TOKEN_KEYWORD_ASYNC:
        case TOKEN_KEYWORD_IF:
        case TOKEN_KEYWORD_MATCH:
        case TOKEN_IDENTIFIER:
        case TOKEN_LPAREN:
        case TOKEN_LBRACKET:
        case TOKEN_LBRACE:
        case TOKEN_KEYWORD_AWAIT:
        case TOKEN_MINUS:
        case TOKEN_NOT:
        case TOKEN_BIT_NOT:
            return true;
        default:
            return false;
    }
}

static bool parser_match_value_starts_block_expression(Parser* parser) {
    if (!parser || parser->current.type != TOKEN_LBRACE) return false;

    Lexer lookahead = parser->lexer;
    Token first = lexer_next_token(&lookahead);
    bool is_block = false;

    if (parser_token_starts_declaration(first.type) ||
        parser_token_starts_non_expression_statement(first.type)) {
        is_block = true;
    } else if (first.type != TOKEN_RBRACE && first.type != TOKEN_EOF) {
        Token second = lexer_peek_token(&lookahead);
        bool looks_like_record_literal =
            parser_token_is_identifier_like(first.type) && second.type == TOKEN_COLON;
        token_free(&second);

        if (!looks_like_record_literal) {
            Lexer scan = parser->lexer;
            int brace_depth = 1;

            while (true) {
                Token token = lexer_next_token(&scan);
                if (token.type == TOKEN_EOF) {
                    token_free(&token);
                    break;
                }

                if (brace_depth == 1 && token.type == TOKEN_SEMICOLON) {
                    is_block = true;
                    token_free(&token);
                    break;
                }

                if (token.type == TOKEN_LBRACE) {
                    brace_depth++;
                } else if (token.type == TOKEN_RBRACE) {
                    brace_depth--;
                    if (brace_depth == 0) {
                        token_free(&token);
                        break;
                    }
                }

                token_free(&token);
            }
        }
    }

    token_free(&first);
    return is_block;
}

static bool parser_identifier_looks_type_name(const char* name) {
    if (!name || name[0] == '\0') return false;
    return name[0] >= 'A' && name[0] <= 'Z';
}

static bool parser_pattern_looks_like_type_qualified_record(Parser* parser) {
    if (!parser || parser->current.type != TOKEN_IDENTIFIER ||
        !parser_identifier_looks_type_name(parser->current.lexeme)) {
        return false;
    }

    Lexer lookahead = parser->lexer;
    Token next = lexer_next_token(&lookahead);
    bool matches = false;

    if (next.type == TOKEN_LBRACE) {
        matches = true;
    } else if (next.type == TOKEN_LBRACKET) {
        int bracket_depth = 1;
        while (bracket_depth > 0) {
            token_free(&next);
            next = lexer_next_token(&lookahead);
            if (next.type == TOKEN_EOF || next.type == TOKEN_ERROR) {
                matches = false;
                break;
            }
            if (next.type == TOKEN_LBRACKET) {
                bracket_depth++;
            } else if (next.type == TOKEN_RBRACKET) {
                bracket_depth--;
            }
        }

        if (bracket_depth == 0) {
            token_free(&next);
            next = lexer_next_token(&lookahead);
            matches = next.type == TOKEN_LBRACE;
        }
    }

    token_free(&next);
    return matches;
}

static void parser_consume_identifier_like(Parser* parser, const char* message) {
    if (parser_token_is_identifier_like(parser->current.type)) {
        parser_advance(parser);
        return;
    }

    char got[128];
    parser_describe_token(&parser->current, got, sizeof(got));

    char detailed[512];
    snprintf(detailed, sizeof(detailed), "%s (got %s)", message, got);
    parser_error(parser, detailed);
}

static void parser_synchronize(Parser* parser, bool stop_at_rbrace) {
    parser->panic_mode = false;
    
    while (parser->current.type != TOKEN_EOF) {
        if (parser->previous.type == TOKEN_SEMICOLON) {
            if ((stop_at_rbrace && parser->current.type == TOKEN_RBRACE) ||
                parser_token_starts_declaration(parser->current.type) ||
                parser_token_starts_non_expression_statement(parser->current.type) ||
                parser_token_starts_expression(parser->current.type)) {
                return;
            }
        }
        
        switch (parser->current.type) {
            case TOKEN_KEYWORD_FUNC:
            case TOKEN_KEYWORD_VAR:
            case TOKEN_KEYWORD_CONST:
            case TOKEN_KEYWORD_PUBLIC:
            case TOKEN_KEYWORD_PRIVATE:
            case TOKEN_KEYWORD_TYPE:
            case TOKEN_KEYWORD_INTERFACE:
            case TOKEN_KEYWORD_IMPL:
            case TOKEN_KEYWORD_ASYNC:
            case TOKEN_KEYWORD_IF:
            case TOKEN_KEYWORD_MATCH:
            case TOKEN_KEYWORD_SWITCH:
            case TOKEN_KEYWORD_WHILE:
            case TOKEN_KEYWORD_FOREACH:
            case TOKEN_KEYWORD_RETURN:
            case TOKEN_KEYWORD_RECORD:
            case TOKEN_KEYWORD_ENUM:
            case TOKEN_KEYWORD_IMPORT:
                return;
            case TOKEN_RBRACE:
                if (stop_at_rbrace) {
                    return;
                }
                break;
            default:
                break;
        }
        
        parser_advance(parser);
    }
}

static Expr* parse_expression(Parser* parser);
static Expr* parse_or(Parser* parser);
static Expr* parse_and(Parser* parser);
static Expr* parse_equality(Parser* parser);
static Expr* parse_comparison(Parser* parser);
static Expr* parse_term(Parser* parser);
static Expr* parse_factor(Parser* parser);
static Expr* parse_unary(Parser* parser);
static Expr* parse_call(Parser* parser);
static Expr* parse_primary(Parser* parser);
static Expr* parse_cast(Parser* parser);
static Expr* parse_pattern_primary(Parser* parser);
static Expr* parse_pattern_call(Parser* parser);
static Expr* parse_pattern_cast(Parser* parser);
static Expr* parse_pattern_unary(Parser* parser);
static Expr* parse_pattern_factor(Parser* parser);
static Expr* parse_pattern_term(Parser* parser);
static Expr* parse_pattern_bit_and(Parser* parser);
static Expr* parse_pattern_bit_xor(Parser* parser);
static Expr* parse_pattern_record_literal(Parser* parser, Type* explicit_type, int line, int column);
static Expr* parse_pattern_comparison(Parser* parser);
static Expr* parse_pattern_equality(Parser* parser);
static Expr* parse_pattern_and(Parser* parser);
static Expr* parse_pattern_or(Parser* parser);
static void parse_pattern_list(Parser* parser, Expr*** out_patterns, int* out_count);
static Expr* parse_if_expression(Parser* parser);
static Expr* parse_if_expression_branch_value(Parser* parser);
static Expr* parse_match_expression(Parser* parser);
static Expr* parse_match_arm_value(Parser* parser);
static Expr* parse_block_expression(Parser* parser);
static Expr* parse_func_literal(Parser* parser, int line, int column, bool is_async);
static Expr* parse_string_literal_or_interpolation(Parser* parser, const Token* string_token);
static Stmt* parse_declaration(Parser* parser);
static Stmt* parse_statement(Parser* parser);
static Stmt* parse_var_declaration(Parser* parser, bool is_mutable);
static Stmt* parse_func_declaration(Parser* parser, bool is_async);
static Stmt* parse_if_statement(Parser* parser);
static Stmt* parse_match_statement(Parser* parser);
static Stmt* parse_switch_statement(Parser* parser);
static Stmt* parse_while_statement(Parser* parser);
static Stmt* parse_foreach_statement(Parser* parser);
static Stmt* parse_block_statement(Parser* parser);
static Stmt* parse_expression_statement(Parser* parser);
static Stmt* parse_import_statement(Parser* parser);
static Stmt* parse_type_alias_declaration(Parser* parser);
static Stmt* parse_interface_declaration(Parser* parser);
static Stmt* parse_impl_declaration(Parser* parser);
static Stmt* parse_enum_declaration(Parser* parser);
static Type* parse_type(Parser* parser);
static Type* parse_function_type(Parser* parser);
static void parse_function_signature(Parser* parser,
                                     char*** out_params,
                                     Type*** out_param_types,
                                     int* out_param_count,
                                     Type** out_return_type);
static void parse_declaration_type_params(Parser* parser,
                                          char*** out_type_params,
                                          int* out_type_param_count);
static void parse_function_type_params(Parser* parser,
                                       char*** out_type_params,
                                       Type*** out_type_param_constraints,
                                       int* out_type_param_count);
static bool parser_can_parse_call_type_args(Parser* parser);
static void parse_call_type_arguments(Parser* parser, Type*** out_type_args, int* out_type_arg_count);
static void parser_apply_visibility_modifier(Parser* parser, Stmt* stmt, bool has_modifier, bool is_public);
static Stmt* parser_finish_expression_statement(Parser* parser,
                                                Expr* expr,
                                                bool allow_trailing_value,
                                                Expr** out_trailing_value);

typedef struct {
    Type** types;
    int type_count;
    char* binding_name;
    Stmt* body;
    int line;
    int column;
} SwitchTypeArm;

static Type* parse_type(Parser* parser);

static bool parser_is_active_type_param(Parser* parser, const char* name) {
    if (!parser || !name) return false;
    for (int i = 0; i < parser->active_type_param_count; i++) {
        if (parser->active_type_params &&
            parser->active_type_params[i] &&
            strcmp(parser->active_type_params[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static Type* parse_function_type(Parser* parser) {
    char** type_params = NULL;
    Type** type_param_constraints = NULL;
    int type_param_count = 0;
    Type** param_types = NULL;
    int param_count = 0;
    Type* return_type = NULL;

    parse_function_type_params(parser, &type_params, &type_param_constraints, &type_param_count);

    char** old_active_type_params = parser->active_type_params;
    int old_active_type_param_count = parser->active_type_param_count;
    char** combined_active_type_params = NULL;
    int combined_active_type_param_count = old_active_type_param_count + type_param_count;
    if (combined_active_type_param_count > 0) {
        combined_active_type_params =
            (char**)safe_malloc((size_t)combined_active_type_param_count * sizeof(char*));
        for (int i = 0; i < old_active_type_param_count; i++) {
            combined_active_type_params[i] = old_active_type_params[i];
        }
        for (int i = 0; i < type_param_count; i++) {
            combined_active_type_params[old_active_type_param_count + i] = type_params[i];
        }
        parser->active_type_params = combined_active_type_params;
        parser->active_type_param_count = combined_active_type_param_count;
    }

    parser_consume(parser, TOKEN_LPAREN, "Expected '(' after func in function type");
    if (parser->current.type != TOKEN_RPAREN) {
        do {
            Type* param_type = parse_type(parser);
            param_count++;
            param_types = (Type**)safe_realloc(param_types, (size_t)param_count * sizeof(Type*));
            param_types[param_count - 1] = param_type;
        } while (parser_match(parser, TOKEN_COMMA));
    }
    parser_consume(parser, TOKEN_RPAREN, "Expected ')' after function type parameters");

    if (parser_match(parser, TOKEN_COLON)) {
        return_type = parse_type(parser);
    } else {
        return_type = type_void();
    }

    parser->active_type_params = old_active_type_params;
    parser->active_type_param_count = old_active_type_param_count;
    if (combined_active_type_params) {
        free(combined_active_type_params);
    }

    Type* function_type = type_function(return_type, param_types, param_count);
    type_function_set_type_params(function_type,
                                  type_params,
                                  type_param_constraints,
                                  type_param_count);

    for (int i = 0; i < type_param_count; i++) {
        if (type_params && type_params[i]) {
            free(type_params[i]);
        }
        if (type_param_constraints && type_param_constraints[i]) {
            type_free(type_param_constraints[i]);
        }
    }
    free(type_params);
    free(type_param_constraints);

    return function_type;
}

static Type* parse_type_internal(Parser* parser) {
    if (parser_match(parser, TOKEN_KEYWORD_INT)) {
        return type_int();
    } else if (parser_match(parser, TOKEN_KEYWORD_BOOL)) {
        return type_bool();
    } else if (parser_match(parser, TOKEN_KEYWORD_DOUBLE)) {
        return type_double();
    } else if (parser_match(parser, TOKEN_KEYWORD_BIGINT)) {
        return type_bigint();
    } else if (parser_match(parser, TOKEN_KEYWORD_STRING)) {
        return type_string();
    } else if (parser_match(parser, TOKEN_KEYWORD_BYTES)) {
        return type_bytes();
    } else if (parser_match(parser, TOKEN_KEYWORD_ARRAY)) {
        parser_consume(parser, TOKEN_LT, "Expected '<' after array");
        Type* element_type = parse_type(parser);
        parser_consume(parser, TOKEN_GT, "Expected '>' after array element type");
        return type_array(element_type);
    } else if (parser_match(parser, TOKEN_KEYWORD_ANY)) {
        return type_any();
    } else if (parser_match(parser, TOKEN_KEYWORD_MAP)) {
        parser_consume(parser, TOKEN_LT, "Expected '<' after map");
        Type* key_type = parse_type(parser);
        parser_consume(parser, TOKEN_COMMA, "Expected ',' after map key type");
        Type* value_type = parse_type(parser);
        parser_consume(parser, TOKEN_GT, "Expected '>' after map value type");
        return type_map(key_type, value_type);
    } else if (parser_match(parser, TOKEN_KEYWORD_SET)) {
        parser_consume(parser, TOKEN_LT, "Expected '<' after set");
        Type* element_type = parse_type(parser);
        parser_consume(parser, TOKEN_GT, "Expected '>' after set element type");
        return type_set(element_type);
    } else if (parser_match(parser, TOKEN_KEYWORD_NIL)) {
        return type_nil();
    } else if (parser_match(parser, TOKEN_KEYWORD_FUNC)) {
        return parse_function_type(parser);
    } else if (parser_match(parser, TOKEN_IDENTIFIER)) {
        if (strcmp(parser->previous.lexeme, "void") == 0) {
            return type_void();
        }
        if (parser_is_active_type_param(parser, parser->previous.lexeme)) {
            return type_type_param(parser->previous.lexeme);
        }
        Type* named_type = type_record(parser->previous.lexeme);
        if (parser_match(parser, TOKEN_LBRACKET)) {
            Type** type_args = NULL;
            int type_arg_count = 0;

            if (parser->current.type == TOKEN_RBRACKET) {
                parser_error(parser, "Expected at least one type argument");
            } else {
                do {
                    Type* arg_type = parse_type(parser);
                    type_arg_count++;
                    type_args = (Type**)safe_realloc(type_args, (size_t)type_arg_count * sizeof(Type*));
                    type_args[type_arg_count - 1] = arg_type;
                } while (parser_match(parser, TOKEN_COMMA));
            }

            parser_consume(parser, TOKEN_RBRACKET, "Expected ']' after type arguments");
            named_type->param_types = type_args;
            named_type->param_count = type_arg_count;
        }
        return named_type;
    }
    return type_any();
}

static Type* parse_type(Parser* parser) {
    // Check for tuple type: (type1, type2, ...)
    if (parser->current.type == TOKEN_LPAREN) {
        Token next = lexer_peek_token(&parser->lexer);
        bool is_tuple_type = parser_token_starts_type(next.type);
        token_free(&next);

        if (is_tuple_type) {
            parser_advance(parser); // consume '('
            Type** element_types = NULL;
            int element_count = 0;
            
            if (parser->current.type != TOKEN_RPAREN) {
                do {
                    Type* elem_type = parse_type(parser);
                    element_count++;
                    element_types = (Type**)safe_realloc(element_types, element_count * sizeof(Type*));
                    element_types[element_count - 1] = elem_type;
                } while (parser_match(parser, TOKEN_COMMA));
            }
            
            parser_consume(parser, TOKEN_RPAREN, "Expected ')' after tuple type");
            Type* t = type_tuple(element_types, element_count);
            if (parser_match(parser, TOKEN_QUESTION) && t) {
                t->nullable = true;
            }
            return t;
        }
    }
    
    Type* t = parse_type_internal(parser);
    if (parser_match(parser, TOKEN_QUESTION) && t) {
        t->nullable = true;
    }
    return t;
}

static bool parser_token_can_start_type_arg(TokenType type) {
    switch (type) {
        case TOKEN_IDENTIFIER:
        case TOKEN_KEYWORD_INT:
        case TOKEN_KEYWORD_BOOL:
        case TOKEN_KEYWORD_DOUBLE:
        case TOKEN_KEYWORD_BIGINT:
        case TOKEN_KEYWORD_STRING:
        case TOKEN_KEYWORD_BYTES:
        case TOKEN_KEYWORD_ARRAY:
        case TOKEN_KEYWORD_ANY:
        case TOKEN_KEYWORD_MAP:
        case TOKEN_KEYWORD_SET:
        case TOKEN_KEYWORD_NIL:
        case TOKEN_LPAREN:
            return true;
        default:
            return false;
    }
}

static bool parser_can_parse_call_type_args(Parser* parser) {
    if (!parser || parser->current.type != TOKEN_LT) return false;

    Lexer lookahead = parser->lexer;
    int angle_depth = 1;
    int tuple_paren_depth = 0;
    int type_app_bracket_depth = 0;
    bool saw_type = false;
    bool expect_type = true;

    while (angle_depth > 0) {
        Token token = lexer_next_token(&lookahead);
        if (token.type == TOKEN_EOF || token.type == TOKEN_ERROR) {
            token_free(&token);
            return false;
        }

        switch (token.type) {
            case TOKEN_LT:
                if (expect_type) {
                    token_free(&token);
                    return false;
                }
                angle_depth++;
                expect_type = true;
                break;

            case TOKEN_GT:
                if (expect_type || tuple_paren_depth != 0 || type_app_bracket_depth != 0) {
                    token_free(&token);
                    return false;
                }
                angle_depth--;
                break;

            case TOKEN_COMMA:
                if (expect_type) {
                    token_free(&token);
                    return false;
                }
                expect_type = true;
                break;

            case TOKEN_LPAREN:
                if (!expect_type) {
                    token_free(&token);
                    return false;
                }
                tuple_paren_depth++;
                expect_type = true;
                break;

            case TOKEN_RPAREN:
                if (tuple_paren_depth <= 0 || expect_type) {
                    token_free(&token);
                    return false;
                }
                tuple_paren_depth--;
                break;

            case TOKEN_LBRACKET:
                if (expect_type) {
                    token_free(&token);
                    return false;
                }
                type_app_bracket_depth++;
                expect_type = true;
                break;

            case TOKEN_RBRACKET:
                if (type_app_bracket_depth <= 0 || expect_type) {
                    token_free(&token);
                    return false;
                }
                type_app_bracket_depth--;
                expect_type = false;
                break;

            case TOKEN_QUESTION:
                if (expect_type) {
                    token_free(&token);
                    return false;
                }
                break;

            default:
                if (!parser_token_can_start_type_arg(token.type) || !expect_type) {
                    token_free(&token);
                    return false;
                }
                saw_type = true;
                expect_type = false;
                break;
        }

        token_free(&token);
    }

    if (!saw_type || expect_type || tuple_paren_depth != 0 || type_app_bracket_depth != 0) {
        return false;
    }

    Token after = lexer_next_token(&lookahead);
    bool is_call = after.type == TOKEN_LPAREN;
    token_free(&after);
    return is_call;
}

static void parse_call_type_arguments(Parser* parser, Type*** out_type_args, int* out_type_arg_count) {
    if (!out_type_args || !out_type_arg_count) return;
    *out_type_args = NULL;
    *out_type_arg_count = 0;

    parser_consume(parser, TOKEN_LT, "Expected '<' before generic type arguments");
    do {
        Type* arg_type = parse_type(parser);
        (*out_type_arg_count)++;
        *out_type_args = (Type**)safe_realloc(*out_type_args, (size_t)(*out_type_arg_count) * sizeof(Type*));
        (*out_type_args)[*out_type_arg_count - 1] = arg_type;
    } while (parser_match(parser, TOKEN_COMMA));
    parser_consume(parser, TOKEN_GT, "Expected '>' after generic type arguments");
}

static bool string_has_unescaped_interpolation(const char* raw_quoted) {
    if (!raw_quoted) return false;

    int len = (int)strlen(raw_quoted);
    if (len < 2) return false;

    int i = 1;
    int end = len - 1;
    while (i < end) {
        char c = raw_quoted[i];
        if (c == '\\' && i + 1 < end) {
            i += 2;
            continue;
        }
        if (c == '$' && i + 1 < end && raw_quoted[i + 1] == '{') {
            return true;
        }
        i++;
    }

    return false;
}

static char* decode_raw_string_segment(const char* raw_segment, int segment_len, const char* file) {
    if (!raw_segment || segment_len < 0) return NULL;

    char* wrapped = (char*)safe_malloc((size_t)segment_len + 3);
    wrapped[0] = '"';
    if (segment_len > 0) {
        memcpy(wrapped + 1, raw_segment, (size_t)segment_len);
    }
    wrapped[segment_len + 1] = '"';
    wrapped[segment_len + 2] = '\0';

    Lexer lx;
    lexer_init(&lx, wrapped, file);
    Token tok = lexer_next_token(&lx);
    Token eof = lexer_next_token(&lx);

    char* decoded = NULL;
    if (tok.type == TOKEN_STRING && tok.as_string && eof.type == TOKEN_EOF) {
        decoded = safe_strdup(tok.as_string);
    } else if (tok.type == TOKEN_STRING && eof.type == TOKEN_EOF) {
        decoded = safe_strdup("");
    }

    token_free(&tok);
    token_free(&eof);
    if (lx.file) free(lx.file);
    free(wrapped);

    return decoded;
}

static Expr* parse_embedded_interpolation_expr(Parser* parser, const char* source, int source_len, int line, int column) {
    if (!parser || !source || source_len <= 0) {
        parser_error_at(parser, "Empty interpolation expression", line, column);
        return NULL;
    }

    char* expr_source = (char*)safe_malloc((size_t)source_len + 1);
    memcpy(expr_source, source, (size_t)source_len);
    expr_source[source_len] = '\0';

    Parser inner;
    lexer_init(&inner.lexer, expr_source, parser->lexer.file);
    inner.current = lexer_next_token(&inner.lexer);
    inner.previous.type = TOKEN_ERROR;
    inner.previous.lexeme = NULL;
    inner.had_error = false;
    inner.panic_mode = false;
    inner.report_diagnostics = parser->report_diagnostics;
    inner.first_error_message = NULL;
    inner.error_line = 0;
    inner.error_column = 0;
    inner.depth = 0;
    inner.max_depth = PARSER_MAX_DEPTH;
    inner.active_type_params = NULL;
    inner.active_type_param_count = 0;
    inner.synthetic_counter = 0;
    inner.current_function_is_async = false;

    Expr* expr = parse_expression(&inner);
    if (!inner.had_error && inner.current.type != TOKEN_EOF) {
        parser_error(&inner, "Unexpected token after interpolation expression");
    }

    if (inner.had_error || !expr || inner.current.type != TOKEN_EOF) {
        if (expr) parser_discard_parse_only_expr(expr);
        expr = NULL;
        parser_error_at(parser, "Invalid interpolation expression", line, column);
    }

    token_free(&inner.current);
    token_free(&inner.previous);
    if (inner.lexer.file) free(inner.lexer.file);
    if (inner.first_error_message) free(inner.first_error_message);
    free(expr_source);

    return expr;
}

static Expr* parse_string_literal_or_interpolation(Parser* parser, const Token* string_token) {
    if (!string_token) return NULL;

    char* decoded = string_token->as_string ? string_token->as_string : "";
    if (!string_token->lexeme || !string_has_unescaped_interpolation(string_token->lexeme)) {
        return expr_create_literal_string(decoded, parser->lexer.file, string_token->line, string_token->column);
    }

    const char* raw = string_token->lexeme;
    int raw_len = (int)strlen(raw);
    if (raw_len < 2) {
        parser_error_at(parser, "Invalid string interpolation", string_token->line, string_token->column);
        return NULL;
    }

    int content_start = 1;
    int content_end = raw_len - 1;
    int segment_start = content_start;

    Expr** parts = NULL;
    int part_count = 0;

    int i = content_start;
    while (i < content_end) {
        if (raw[i] == '\\' && i + 1 < content_end) {
            i += 2;
            continue;
        }

        if (raw[i] == '$' && i + 1 < content_end && raw[i + 1] == '{') {
            int literal_len = i - segment_start;
            if (literal_len > 0) {
                char* literal_text = decode_raw_string_segment(raw + segment_start, literal_len, parser->lexer.file);
                if (!literal_text) {
                    parser_error_at(parser, "Invalid string interpolation segment", string_token->line, string_token->column);
                    goto fail;
                }
                Expr* literal_expr = expr_create_literal_string(literal_text, parser->lexer.file, string_token->line, string_token->column);
                free(literal_text);

                part_count++;
                parts = (Expr**)safe_realloc(parts, (size_t)part_count * sizeof(Expr*));
                parts[part_count - 1] = literal_expr;
            }

            i += 2; // Skip '${'
            int expr_start = i;
            int brace_depth = 0;
            bool in_string = false;
            bool escaped = false;

            while (i < content_end) {
                char ch = raw[i];
                if (in_string) {
                    if (escaped) {
                        escaped = false;
                        i++;
                        continue;
                    }
                    if (ch == '\\') {
                        escaped = true;
                        i++;
                        continue;
                    }
                    if (ch == '"') {
                        in_string = false;
                    }
                    i++;
                    continue;
                }

                if (ch == '"') {
                    in_string = true;
                    i++;
                    continue;
                }
                if (ch == '{') {
                    brace_depth++;
                    i++;
                    continue;
                }
                if (ch == '}') {
                    if (brace_depth == 0) break;
                    brace_depth--;
                    i++;
                    continue;
                }
                i++;
            }

            if (i >= content_end || raw[i] != '}') {
                parser_error_at(parser, "Unterminated string interpolation; expected '}'", string_token->line, string_token->column);
                goto fail;
            }

            int expr_len = i - expr_start;
            Expr* embedded_expr = parse_embedded_interpolation_expr(parser, raw + expr_start, expr_len, string_token->line, string_token->column);
            if (!embedded_expr) {
                goto fail;
            }

            Expr* casted_expr = expr_create_cast(embedded_expr, type_string(), parser->lexer.file, string_token->line, string_token->column);
            part_count++;
            parts = (Expr**)safe_realloc(parts, (size_t)part_count * sizeof(Expr*));
            parts[part_count - 1] = casted_expr;

            i++; // Skip closing '}'
            segment_start = i;
            continue;
        }

        i++;
    }

    if (segment_start < content_end) {
        int tail_len = content_end - segment_start;
        char* tail_text = decode_raw_string_segment(raw + segment_start, tail_len, parser->lexer.file);
        if (!tail_text) {
            parser_error_at(parser, "Invalid string interpolation segment", string_token->line, string_token->column);
            goto fail;
        }
        Expr* tail_expr = expr_create_literal_string(tail_text, parser->lexer.file, string_token->line, string_token->column);
        free(tail_text);

        part_count++;
        parts = (Expr**)safe_realloc(parts, (size_t)part_count * sizeof(Expr*));
        parts[part_count - 1] = tail_expr;
    }

    if (part_count == 0) {
        if (parts) free(parts);
        return expr_create_literal_string("", parser->lexer.file, string_token->line, string_token->column);
    }

    Expr* combined = parts[0];
    for (int j = 1; j < part_count; j++) {
        combined = expr_create_binary(TOKEN_PLUS, combined, parts[j], parser->lexer.file, string_token->line, string_token->column);
    }
    free(parts);
    return combined;

fail:
    for (int j = 0; j < part_count; j++) {
        parser_discard_parse_only_expr(parts[j]);
    }
    if (parts) free(parts);
    return NULL;
}

static Expr* parse_primary(Parser* parser) {
    PARSER_ENTER(parser);
    
    if (parser_match(parser, TOKEN_NUMBER_INT)) {
        Expr* result = expr_create_literal_int(parser->previous.as_int, parser->lexer.file, parser->previous.line, parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }

    if (parser_match(parser, TOKEN_NUMBER_BIGINT)) {
        Expr* result = expr_create_literal_bigint(parser->previous.as_string, parser->lexer.file, parser->previous.line, parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }
    
    if (parser_match(parser, TOKEN_NUMBER_DOUBLE)) {
        Expr* result = expr_create_literal_double(parser->previous.as_double, parser->lexer.file, parser->previous.line, parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }
    
    if (parser_match(parser, TOKEN_STRING)) {
        Expr* result = parse_string_literal_or_interpolation(parser, &parser->previous);
        PARSER_LEAVE(parser);
        return result;
    }
    
    if (parser_match(parser, TOKEN_TRUE)) {
        Expr* result = expr_create_literal_bool(true, parser->lexer.file, parser->previous.line, parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }
    
    if (parser_match(parser, TOKEN_FALSE)) {
        Expr* result = expr_create_literal_bool(false, parser->lexer.file, parser->previous.line, parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }
    
    if (parser_match(parser, TOKEN_KEYWORD_NIL)) {
        Expr* result = expr_create_nil(parser->lexer.file, parser->previous.line, parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }

    if (parser_match(parser, TOKEN_KEYWORD_FUNC)) {
        int line = parser->previous.line;
        int column = parser->previous.column;
        Expr* result = parse_func_literal(parser, line, column, false);
        PARSER_LEAVE(parser);
        return result;
    }

    if (parser_match(parser, TOKEN_KEYWORD_ASYNC)) {
        int line = parser->previous.line;
        int column = parser->previous.column;
        if (parser_match(parser, TOKEN_KEYWORD_FUNC)) {
            Expr* result = parse_func_literal(parser, line, column, true);
            parser_error_at(parser, "Async function literals are not supported yet", line, column);
            PARSER_LEAVE(parser);
            return result;
        }
        parser_error_at(parser, "Expected 'func' after 'async'", line, column);
        PARSER_LEAVE(parser);
        return NULL;
    }

    if (parser_match(parser, TOKEN_KEYWORD_IF)) {
        Expr* result = parse_if_expression(parser);
        PARSER_LEAVE(parser);
        return result;
    }

    if (parser_match(parser, TOKEN_KEYWORD_MATCH)) {
        Expr* result = parse_match_expression(parser);
        PARSER_LEAVE(parser);
        return result;
    }
    
    if (parser_match(parser, TOKEN_IDENTIFIER)) {
        Expr* result = expr_create_identifier(parser->previous.lexeme, parser->lexer.file, parser->previous.line, parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }
    
    if (parser_match(parser, TOKEN_LPAREN)) {
        // Check for empty tuple: ()
        if (parser_match(parser, TOKEN_RPAREN)) {
            // Empty tuple
            Expr* result = expr_create_tuple_literal(NULL, 0, parser->lexer.file, parser->previous.line, parser->previous.column);
            PARSER_LEAVE(parser);
            return result;
        }
        
        // Check if this is a tuple literal: multiple comma-separated expressions
        Expr* first_expr = parse_expression(parser);
        
        if (parser_match(parser, TOKEN_COMMA)) {
            // This is a tuple literal
            Expr** elements = (Expr**)safe_malloc(sizeof(Expr*));
            elements[0] = first_expr;
            int element_count = 1;
            
            do {
                Expr* element = parse_expression(parser);
                element_count++;
                elements = (Expr**)safe_realloc(elements, element_count * sizeof(Expr*));
                elements[element_count - 1] = element;
            } while (parser_match(parser, TOKEN_COMMA));
            
            parser_consume(parser, TOKEN_RPAREN, "Expected ')' after tuple elements");
            Expr* result = expr_create_tuple_literal(elements, element_count, parser->lexer.file, parser->previous.line, parser->previous.column);
            PARSER_LEAVE(parser);
            return result;
        } else {
            // This is just a grouped expression
            parser_consume(parser, TOKEN_RPAREN, "Expected ')' after expression");
            PARSER_LEAVE(parser);
            return first_expr;
        }
    }
    
    if (parser_match(parser, TOKEN_LBRACKET)) {
        Expr** elements = NULL;
        int element_count = 0;
        
        if (parser->current.type != TOKEN_RBRACKET) {
            do {
                Expr* element = parse_expression(parser);
                element_count++;
                elements = (Expr**)safe_realloc(elements, element_count * sizeof(Expr*));
                elements[element_count -1] = element;
            } while (parser_match(parser, TOKEN_COMMA));
        }
        
        parser_consume(parser, TOKEN_RBRACKET, "Expected ']' after array elements");
        Expr* result = expr_create_array_literal(elements, element_count, parser->lexer.file, parser->previous.line, parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }
    
    // Record literal: { field: value, ... }
    // Map literal: { key: value, ... } where key is an expression
    // Set literal: { value1, value2, ... }
    if (parser_match(parser, TOKEN_LBRACE)) {
        // Check if this is a record literal by looking for identifier: value pattern
        Token lookahead = lexer_peek_token(&parser->lexer);
        bool is_record_literal =
            parser_token_is_identifier_like(parser->current.type) &&
            lookahead.type == TOKEN_COLON;
        token_free(&lookahead);

        if (is_record_literal) {
            char** field_names = NULL;
            Expr** field_values = NULL;
            int field_count = 0;
            
            do {
                parser_consume_identifier_like(parser, "Expected field name");
                char* field_name = safe_strdup(parser->previous.lexeme);
                
                parser_consume(parser, TOKEN_COLON, "Expected ':' after field name");
                Expr* field_value = parse_expression(parser);
                
                field_count++;
                field_names = (char**)safe_realloc(field_names, field_count * sizeof(char*));
                field_values = (Expr**)safe_realloc(field_values, field_count * sizeof(Expr*));
                field_names[field_count - 1] = field_name;
                field_values[field_count - 1] = field_value;
            } while (parser_match(parser, TOKEN_COMMA));
            
            parser_consume(parser, TOKEN_RBRACE, "Expected '}' after record fields");
            Expr* result = expr_create_record_literal(field_names, field_values, field_count, NULL, parser->lexer.file, parser->previous.line, parser->previous.column);
            PARSER_LEAVE(parser);
            return result;
        } else if (parser->current.type != TOKEN_RBRACE) {
            // Could be map or set literal
            // Parse first element to determine which
            Expr* first_expr = parse_expression(parser);
            
            if (parser_match(parser, TOKEN_COLON)) {
                // Map literal: key: value pairs
                Expr** keys = (Expr**)safe_malloc(sizeof(Expr*));
                Expr** values = (Expr**)safe_malloc(sizeof(Expr*));
                keys[0] = first_expr;
                values[0] = parse_expression(parser);
                int entry_count = 1;
                
                while (parser_match(parser, TOKEN_COMMA)) {
                    Expr* key = parse_expression(parser);
                    parser_consume(parser, TOKEN_COLON, "Expected ':' after map key");
                    Expr* value = parse_expression(parser);
                    entry_count++;
                    keys = (Expr**)safe_realloc(keys, entry_count * sizeof(Expr*));
                    values = (Expr**)safe_realloc(values, entry_count * sizeof(Expr*));
                    keys[entry_count - 1] = key;
                    values[entry_count - 1] = value;
                }
                
                parser_consume(parser, TOKEN_RBRACE, "Expected '}' after map entries");
                Expr* result = expr_create_map_literal(keys, values, entry_count, NULL, parser->lexer.file, parser->previous.line, parser->previous.column);
                PARSER_LEAVE(parser);
                return result;
            } else {
                // Set literal: just values
                Expr** elements = (Expr**)safe_malloc(sizeof(Expr*));
                elements[0] = first_expr;
                int element_count = 1;
                
                while (parser_match(parser, TOKEN_COMMA)) {
                    Expr* element = parse_expression(parser);
                    element_count++;
                    elements = (Expr**)safe_realloc(elements, element_count * sizeof(Expr*));
                    elements[element_count - 1] = element;
                }
                
                parser_consume(parser, TOKEN_RBRACE, "Expected '}' after set elements");
                Expr* result = expr_create_set_literal(elements, element_count, NULL, parser->lexer.file, parser->previous.line, parser->previous.column);
                PARSER_LEAVE(parser);
                return result;
            }
        } else {
            // Empty braces - could be empty map or set
            // Default to empty map for now
            parser_consume(parser, TOKEN_RBRACE, "Expected '}'");
            Expr* result = expr_create_map_literal(NULL, NULL, 0, NULL, parser->lexer.file, parser->previous.line, parser->previous.column);
            PARSER_LEAVE(parser);
            return result;
        }
    }
    
    char got[128];
    parser_describe_token(&parser->current, got, sizeof(got));
    char detailed[512];
    snprintf(detailed, sizeof(detailed), "Expected expression (got %s)", got);
    parser_error(parser, detailed);
    PARSER_LEAVE(parser);
    return NULL;
}

static Expr* parse_pattern_record_literal(Parser* parser, Type* explicit_type, int line, int column) {
    char** field_names = NULL;
    Expr** field_values = NULL;
    int field_count = 0;
    bool allows_rest = false;

    while (parser->current.type != TOKEN_RBRACE && parser->current.type != TOKEN_EOF) {
        if (parser_match(parser, TOKEN_DOT_DOT)) {
            allows_rest = true;
            if (parser->current.type != TOKEN_RBRACE) {
                parser_error(parser, "Record pattern '..' must be the last entry");
            }
            break;
        }

        parser_consume_identifier_like(parser, "Expected record pattern field name");
        char* field_name = safe_strdup(parser->previous.lexeme);
        int field_line = parser->previous.line;
        int field_column = parser->previous.column;
        Expr* field_value = NULL;

        if (parser_match(parser, TOKEN_COLON)) {
            field_value = parse_pattern_or(parser);
        } else {
            field_value = expr_create_identifier(field_name,
                                                 parser->lexer.file,
                                                 field_line,
                                                 field_column);
        }

        field_count++;
        field_names = (char**)safe_realloc(field_names, (size_t)field_count * sizeof(char*));
        field_values = (Expr**)safe_realloc(field_values, (size_t)field_count * sizeof(Expr*));
        field_names[field_count - 1] = field_name;
        field_values[field_count - 1] = field_value;

        if (!parser_match(parser, TOKEN_COMMA)) {
            break;
        }
    }

    parser_consume(parser, TOKEN_RBRACE, "Expected '}' after record pattern");

    Expr* result = expr_create_record_literal(field_names,
                                              field_values,
                                              field_count,
                                              NULL,
                                              parser->lexer.file,
                                              line,
                                              column);
    if (result) {
        result->record_literal.is_pattern = true;
        result->record_literal.allows_rest = allows_rest;
        result->record_literal.pattern_type = explicit_type;
    }
    return result;
}

static Expr* parse_pattern_primary(Parser* parser) {
    PARSER_ENTER(parser);

    if (parser_match(parser, TOKEN_NUMBER_INT)) {
        Expr* result = expr_create_literal_int(parser->previous.as_int,
                                               parser->lexer.file,
                                               parser->previous.line,
                                               parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }

    if (parser_match(parser, TOKEN_NUMBER_BIGINT)) {
        Expr* result = expr_create_literal_bigint(parser->previous.as_string,
                                                  parser->lexer.file,
                                                  parser->previous.line,
                                                  parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }

    if (parser_match(parser, TOKEN_NUMBER_DOUBLE)) {
        Expr* result = expr_create_literal_double(parser->previous.as_double,
                                                  parser->lexer.file,
                                                  parser->previous.line,
                                                  parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }

    if (parser_match(parser, TOKEN_STRING)) {
        Expr* result = parse_string_literal_or_interpolation(parser, &parser->previous);
        PARSER_LEAVE(parser);
        return result;
    }

    if (parser_match(parser, TOKEN_TRUE)) {
        Expr* result = expr_create_literal_bool(true,
                                                parser->lexer.file,
                                                parser->previous.line,
                                                parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }

    if (parser_match(parser, TOKEN_FALSE)) {
        Expr* result = expr_create_literal_bool(false,
                                                parser->lexer.file,
                                                parser->previous.line,
                                                parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }

    if (parser_match(parser, TOKEN_KEYWORD_NIL)) {
        Expr* result = expr_create_nil(parser->lexer.file,
                                       parser->previous.line,
                                       parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }

    if (parser_pattern_looks_like_type_qualified_record(parser)) {
        int pattern_line = parser->current.line;
        int pattern_column = parser->current.column;
        Type* explicit_type = parse_type(parser);
        parser_consume(parser, TOKEN_LBRACE, "Expected '{' after record pattern type");
        Expr* result = parse_pattern_record_literal(parser, explicit_type, pattern_line, pattern_column);
        PARSER_LEAVE(parser);
        return result;
    }

    if (parser_match(parser, TOKEN_IDENTIFIER)) {
        Expr* result = expr_create_identifier(parser->previous.lexeme,
                                              parser->lexer.file,
                                              parser->previous.line,
                                              parser->previous.column);
        PARSER_LEAVE(parser);
        return result;
    }

    if (parser_match(parser, TOKEN_LPAREN)) {
        int tuple_line = parser->previous.line;
        int tuple_column = parser->previous.column;

        if (parser_match(parser, TOKEN_RPAREN)) {
            Expr* result = expr_create_tuple_literal(NULL, 0, parser->lexer.file, tuple_line, tuple_column);
            PARSER_LEAVE(parser);
            return result;
        }

        Expr* first_expr = parse_pattern_or(parser);
        if (parser_match(parser, TOKEN_COMMA)) {
            Expr** elements = (Expr**)safe_malloc(sizeof(Expr*));
            elements[0] = first_expr;
            int element_count = 1;

            do {
                Expr* element = parse_pattern_or(parser);
                element_count++;
                elements = (Expr**)safe_realloc(elements, (size_t)element_count * sizeof(Expr*));
                elements[element_count - 1] = element;
            } while (parser_match(parser, TOKEN_COMMA));

            parser_consume(parser, TOKEN_RPAREN, "Expected ')' after tuple pattern");
            Expr* result = expr_create_tuple_literal(elements,
                                                     element_count,
                                                     parser->lexer.file,
                                                     tuple_line,
                                                     tuple_column);
            PARSER_LEAVE(parser);
            return result;
        }

        while (parser_match(parser, TOKEN_BIT_OR)) {
            Expr* right = parse_pattern_or(parser);
            first_expr = expr_create_binary(TOKEN_BIT_OR,
                                            first_expr,
                                            right,
                                            parser->lexer.file,
                                            parser->previous.line,
                                            parser->previous.column);
        }

        parser_consume(parser, TOKEN_RPAREN, "Expected ')' after grouped pattern");
        PARSER_LEAVE(parser);
        return first_expr;
    }

    if (parser_match(parser, TOKEN_LBRACE)) {
        int pattern_line = parser->previous.line;
        int pattern_column = parser->previous.column;
        Expr* result = parse_pattern_record_literal(parser, NULL, pattern_line, pattern_column);
        PARSER_LEAVE(parser);
        return result;
    }

    char got[128];
    parser_describe_token(&parser->current, got, sizeof(got));
    char detailed[512];
    snprintf(detailed, sizeof(detailed), "Expected pattern (got %s)", got);
    parser_error(parser, detailed);
    PARSER_LEAVE(parser);
    return NULL;
}

static Expr* parse_call(Parser* parser) {
    Expr* expr = parse_primary(parser);
    Type** pending_type_args = NULL;
    int pending_type_arg_count = 0;
    
    while (1) {
        if (parser->current.type == TOKEN_LT && parser_can_parse_call_type_args(parser)) {
            if (pending_type_args) {
                free(pending_type_args);
                pending_type_args = NULL;
                pending_type_arg_count = 0;
            }
            parse_call_type_arguments(parser, &pending_type_args, &pending_type_arg_count);
        } else if (parser_match(parser, TOKEN_LPAREN)) {
            Expr** args = NULL;
            int arg_count = 0;
            
            if (parser->current.type != TOKEN_RPAREN) {
                do {
                    Expr* arg = parse_expression(parser);
                    arg_count++;
                    args = (Expr**)safe_realloc(args, arg_count * sizeof(Expr*));
                    args[arg_count - 1] = arg;
                } while (parser_match(parser, TOKEN_COMMA));
            }
            
            parser_consume(parser, TOKEN_RPAREN, "Expected ')' after arguments");
            expr = expr_create_call(expr,
                                    args,
                                    arg_count,
                                    pending_type_args,
                                    pending_type_arg_count,
                                    parser->lexer.file,
                                    parser->previous.line,
                                    parser->previous.column);
            pending_type_args = NULL;
            pending_type_arg_count = 0;
        } else if (parser_match(parser, TOKEN_LBRACKET)) {
            if (pending_type_args) {
                free(pending_type_args);
                pending_type_args = NULL;
                pending_type_arg_count = 0;
            }
            Expr* index = parse_expression(parser);
            parser_consume(parser, TOKEN_RBRACKET, "Expected ']' after index");
            expr = expr_create_array(expr, index, parser->lexer.file, parser->previous.line, parser->previous.column);
        } else if (parser->current.type == TOKEN_DOT) {
            if (pending_type_args) {
                free(pending_type_args);
                pending_type_args = NULL;
                pending_type_arg_count = 0;
            }
            // Could be method call (obj.method(...)), field access (obj.field),
            // or tuple access (tuple.0).
            parser_advance(parser);
            if (parser->current.type == TOKEN_NUMBER_INT) {
                // Tuple access: tuple.0, tuple.1, etc.
                int index = (int)parser->current.as_int;
                parser_advance(parser);
                expr = expr_create_tuple_access(expr, index, parser->lexer.file, parser->previous.line, parser->previous.column);
            } else {
                // Field access / method call: obj.field / obj.method(...)
                parser_consume_identifier_like(parser, "Expected field name or number after '.'");
                int member_line = parser->previous.line;
                int member_column = parser->previous.column;
                char* member_name = safe_strdup(parser->previous.lexeme);
                Expr* object_expr = expr;
                Type** method_type_args = NULL;
                int method_type_arg_count = 0;

                if (parser->current.type == TOKEN_LT && parser_can_parse_call_type_args(parser)) {
                    parse_call_type_arguments(parser, &method_type_args, &method_type_arg_count);
                }

                if (parser_match(parser, TOKEN_LPAREN)) {
                    bool treat_as_static_access =
                        object_expr &&
                        object_expr->kind == EXPR_IDENTIFIER &&
                        object_expr->identifier &&
                        parser_identifier_looks_type_name(object_expr->identifier);

                    Expr** args = NULL;
                    int arg_count = 0;
                    if (!treat_as_static_access) {
                        // Method syntax sugar: obj.method(a, b) => method(obj, a, b)
                        arg_count = 1;
                        args = (Expr**)safe_malloc(sizeof(Expr*));
                        args[0] = object_expr;
                    }

                    if (parser->current.type != TOKEN_RPAREN) {
                        do {
                            Expr* arg = parse_expression(parser);
                            arg_count++;
                            args = (Expr**)safe_realloc(args, arg_count * sizeof(Expr*));
                            args[arg_count - 1] = arg;
                        } while (parser_match(parser, TOKEN_COMMA));
                    }

                    parser_consume(parser, TOKEN_RPAREN, "Expected ')' after method arguments");

                    Expr* callee = NULL;
                    if (treat_as_static_access) {
                        callee = expr_create_field_access(object_expr, member_name, parser->lexer.file, member_line, member_column);
                    } else {
                        callee = expr_create_identifier(member_name, parser->lexer.file, member_line, member_column);
                    }

                    expr = expr_create_call(callee,
                                            args,
                                            arg_count,
                                            method_type_args,
                                            method_type_arg_count,
                                            parser->lexer.file,
                                            member_line,
                                            member_column);
                } else {
                    if (method_type_args) free(method_type_args);
                    expr = expr_create_field_access(object_expr, member_name, parser->lexer.file, member_line, member_column);
                }
                free(member_name);
            }
        } else if (parser_match(parser, TOKEN_QUESTION)) {
            if (pending_type_args) {
                free(pending_type_args);
                pending_type_args = NULL;
                pending_type_arg_count = 0;
            }
            expr = expr_create_try(expr, parser->lexer.file, parser->previous.line, parser->previous.column);
        } else {
            break;
        }
    }

    if (pending_type_args) free(pending_type_args);

    return expr;
}

static Expr* parse_pattern_call(Parser* parser) {
    Expr* expr = parse_pattern_primary(parser);
    Type** pending_type_args = NULL;
    int pending_type_arg_count = 0;

    while (1) {
        if (parser->current.type == TOKEN_LT && parser_can_parse_call_type_args(parser)) {
            if (pending_type_args) {
                free(pending_type_args);
                pending_type_args = NULL;
                pending_type_arg_count = 0;
            }
            parse_call_type_arguments(parser, &pending_type_args, &pending_type_arg_count);
        } else if (parser_match(parser, TOKEN_LPAREN)) {
            Expr** args = NULL;
            int arg_count = 0;

            if (parser->current.type != TOKEN_RPAREN) {
                do {
                    Expr* arg = parse_pattern_or(parser);
                    arg_count++;
                    args = (Expr**)safe_realloc(args, (size_t)arg_count * sizeof(Expr*));
                    args[arg_count - 1] = arg;
                } while (parser_match(parser, TOKEN_COMMA));
            }

            parser_consume(parser, TOKEN_RPAREN, "Expected ')' after pattern arguments");
            expr = expr_create_call(expr,
                                    args,
                                    arg_count,
                                    pending_type_args,
                                    pending_type_arg_count,
                                    parser->lexer.file,
                                    parser->previous.line,
                                    parser->previous.column);
            pending_type_args = NULL;
            pending_type_arg_count = 0;
        } else if (parser_match(parser, TOKEN_LBRACKET)) {
            if (pending_type_args) {
                free(pending_type_args);
                pending_type_args = NULL;
                pending_type_arg_count = 0;
            }
            Expr* index = parse_expression(parser);
            parser_consume(parser, TOKEN_RBRACKET, "Expected ']' after pattern index");
            expr = expr_create_array(expr, index, parser->lexer.file, parser->previous.line, parser->previous.column);
        } else if (parser->current.type == TOKEN_DOT) {
            if (pending_type_args) {
                free(pending_type_args);
                pending_type_args = NULL;
                pending_type_arg_count = 0;
            }

            parser_advance(parser);
            if (parser->current.type == TOKEN_NUMBER_INT) {
                int index = (int)parser->current.as_int;
                parser_advance(parser);
                expr = expr_create_tuple_access(expr,
                                                index,
                                                parser->lexer.file,
                                                parser->previous.line,
                                                parser->previous.column);
            } else {
                parser_consume_identifier_like(parser, "Expected field name or number after '.'");
                int member_line = parser->previous.line;
                int member_column = parser->previous.column;
                char* member_name = safe_strdup(parser->previous.lexeme);
                Expr* object_expr = expr;
                Type** method_type_args = NULL;
                int method_type_arg_count = 0;

                if (parser->current.type == TOKEN_LT && parser_can_parse_call_type_args(parser)) {
                    parse_call_type_arguments(parser, &method_type_args, &method_type_arg_count);
                }

                if (parser_match(parser, TOKEN_LPAREN)) {
                    bool treat_as_static_access =
                        object_expr &&
                        object_expr->kind == EXPR_IDENTIFIER &&
                        object_expr->identifier &&
                        parser_identifier_looks_type_name(object_expr->identifier);

                    Expr** args = NULL;
                    int arg_count = 0;
                    if (!treat_as_static_access) {
                        arg_count = 1;
                        args = (Expr**)safe_malloc(sizeof(Expr*));
                        args[0] = object_expr;
                    }

                    if (parser->current.type != TOKEN_RPAREN) {
                        do {
                            Expr* arg = parse_pattern_or(parser);
                            arg_count++;
                            args = (Expr**)safe_realloc(args, (size_t)arg_count * sizeof(Expr*));
                            args[arg_count - 1] = arg;
                        } while (parser_match(parser, TOKEN_COMMA));
                    }

                    parser_consume(parser, TOKEN_RPAREN, "Expected ')' after pattern method arguments");

                    Expr* callee = NULL;
                    if (treat_as_static_access) {
                        callee = expr_create_field_access(object_expr,
                                                          member_name,
                                                          parser->lexer.file,
                                                          member_line,
                                                          member_column);
                    } else {
                        callee = expr_create_identifier(member_name,
                                                        parser->lexer.file,
                                                        member_line,
                                                        member_column);
                    }

                    expr = expr_create_call(callee,
                                            args,
                                            arg_count,
                                            method_type_args,
                                            method_type_arg_count,
                                            parser->lexer.file,
                                            member_line,
                                            member_column);
                } else {
                    if (method_type_args) free(method_type_args);
                    expr = expr_create_field_access(object_expr,
                                                    member_name,
                                                    parser->lexer.file,
                                                    member_line,
                                                    member_column);
                }
                free(member_name);
            }
        } else if (parser_match(parser, TOKEN_QUESTION)) {
            if (pending_type_args) {
                free(pending_type_args);
                pending_type_args = NULL;
                pending_type_arg_count = 0;
            }
            expr = expr_create_try(expr,
                                   parser->lexer.file,
                                   parser->previous.line,
                                   parser->previous.column);
        } else {
            break;
        }
    }

    if (pending_type_args) free(pending_type_args);
    return expr;
}

static Expr* parse_cast(Parser* parser) {
    Expr* expr = parse_call(parser);
    
    if (parser_match(parser, TOKEN_AS)) {
        Type* target_type = parse_type(parser);
        expr = expr_create_cast(expr, target_type, parser->lexer.file, parser->previous.line, parser->previous.column);
    }
    
    return expr;
}

static Expr* parse_pattern_cast(Parser* parser) {
    Expr* expr = parse_pattern_call(parser);

    if (parser_match(parser, TOKEN_AS)) {
        Type* target_type = parse_type(parser);
        expr = expr_create_cast(expr, target_type, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    return expr;
}

static Expr* parse_unary(Parser* parser) {
    if (parser_match(parser, TOKEN_KEYWORD_AWAIT)) {
        int line = parser->previous.line;
        int column = parser->previous.column;
        if (!parser->current_function_is_async) {
            parser_error_at(parser, "await is only allowed inside async functions", line, column);
        }
        Expr* operand = parse_unary(parser);
        return expr_create_await(operand, parser->lexer.file, line, column);
    }

    if (parser_match(parser, TOKEN_MINUS) || parser_match(parser, TOKEN_NOT) || parser_match(parser, TOKEN_BIT_NOT)) {
        TokenType op = parser->previous.type;
        Expr* operand = parse_unary(parser);
        return expr_create_unary(op, operand, parser->lexer.file, parser->previous.line, parser->previous.column);
    }
    
    return parse_cast(parser);
}

static Expr* parse_pattern_unary(Parser* parser) {
    if (parser_match(parser, TOKEN_MINUS) || parser_match(parser, TOKEN_NOT) || parser_match(parser, TOKEN_BIT_NOT)) {
        TokenType op = parser->previous.type;
        Expr* operand = parse_pattern_unary(parser);
        return expr_create_unary(op, operand, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    return parse_pattern_cast(parser);
}

static Expr* parse_factor(Parser* parser) {
    PARSER_ENTER(parser);
    
    Expr* expr = parse_unary(parser);
    
    while (parser_match(parser, TOKEN_STAR) || parser_match(parser, TOKEN_SLASH) || parser_match(parser, TOKEN_PERCENT)) {
        TokenType op = parser->previous.type;
        Expr* right = parse_unary(parser);
        expr = expr_create_binary(op, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }
    
    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_pattern_factor(Parser* parser) {
    PARSER_ENTER(parser);

    Expr* expr = parse_pattern_unary(parser);

    while (parser_match(parser, TOKEN_STAR) || parser_match(parser, TOKEN_SLASH) || parser_match(parser, TOKEN_PERCENT)) {
        TokenType op = parser->previous.type;
        Expr* right = parse_pattern_unary(parser);
        expr = expr_create_binary(op, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_term(Parser* parser) {
    PARSER_ENTER(parser);
    
    Expr* expr = parse_factor(parser);
    
    while (parser_match(parser, TOKEN_PLUS) || parser_match(parser, TOKEN_MINUS)) {
        TokenType op = parser->previous.type;
        Expr* right = parse_factor(parser);
        expr = expr_create_binary(op, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }
    
    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_pattern_term(Parser* parser) {
    PARSER_ENTER(parser);

    Expr* expr = parse_pattern_factor(parser);

    while (parser_match(parser, TOKEN_PLUS) || parser_match(parser, TOKEN_MINUS)) {
        TokenType op = parser->previous.type;
        Expr* right = parse_pattern_factor(parser);
        expr = expr_create_binary(op, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_bit_and(Parser* parser) {
    PARSER_ENTER(parser);

    Expr* expr = parse_term(parser);

    while (parser_match(parser, TOKEN_BIT_AND)) {
        Expr* right = parse_term(parser);
        expr = expr_create_binary(TOKEN_BIT_AND, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_pattern_bit_and(Parser* parser) {
    PARSER_ENTER(parser);

    Expr* expr = parse_pattern_term(parser);

    while (parser_match(parser, TOKEN_BIT_AND)) {
        Expr* right = parse_pattern_term(parser);
        expr = expr_create_binary(TOKEN_BIT_AND, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_bit_xor(Parser* parser) {
    PARSER_ENTER(parser);

    Expr* expr = parse_bit_and(parser);

    while (parser_match(parser, TOKEN_BIT_XOR)) {
        Expr* right = parse_bit_and(parser);
        expr = expr_create_binary(TOKEN_BIT_XOR, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_pattern_bit_xor(Parser* parser) {
    PARSER_ENTER(parser);

    Expr* expr = parse_pattern_bit_and(parser);

    while (parser_match(parser, TOKEN_BIT_XOR)) {
        Expr* right = parse_pattern_bit_and(parser);
        expr = expr_create_binary(TOKEN_BIT_XOR, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_bit_or(Parser* parser) {
    PARSER_ENTER(parser);

    Expr* expr = parse_bit_xor(parser);

    while (parser_match(parser, TOKEN_BIT_OR)) {
        Expr* right = parse_bit_xor(parser);
        expr = expr_create_binary(TOKEN_BIT_OR, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_pattern_comparison(Parser* parser) {
    PARSER_ENTER(parser);

    Expr* expr = parse_pattern_bit_xor(parser);

    while (parser_match(parser, TOKEN_LT) || parser_match(parser, TOKEN_LT_EQ) ||
           parser_match(parser, TOKEN_GT) || parser_match(parser, TOKEN_GT_EQ)) {
        TokenType op = parser->previous.type;
        Expr* right = parse_pattern_bit_xor(parser);
        expr = expr_create_binary(op, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_pattern_equality(Parser* parser) {
    PARSER_ENTER(parser);

    Expr* expr = parse_pattern_comparison(parser);

    while (parser_match(parser, TOKEN_EQ_EQ) || parser_match(parser, TOKEN_BANG_EQ)) {
        TokenType op = parser->previous.type;
        Expr* right = parse_pattern_comparison(parser);
        expr = expr_create_binary(op, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_pattern_and(Parser* parser) {
    PARSER_ENTER(parser);

    Expr* expr = parse_pattern_equality(parser);

    while (parser_match(parser, TOKEN_AND)) {
        Expr* right = parse_pattern_equality(parser);
        expr = expr_create_binary(TOKEN_AND, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_pattern_or(Parser* parser) {
    PARSER_ENTER(parser);

    Expr* expr = parse_pattern_and(parser);

    while (parser_match(parser, TOKEN_OR)) {
        Expr* right = parse_pattern_and(parser);
        expr = expr_create_binary(TOKEN_OR, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    PARSER_LEAVE(parser);
    return expr;
}

static void parse_pattern_list(Parser* parser, Expr*** out_patterns, int* out_count) {
    Expr** patterns = NULL;
    int pattern_count = 0;

    do {
        Expr* pattern = parse_pattern_or(parser);
        pattern_count++;
        patterns = (Expr**)safe_realloc(patterns, (size_t)pattern_count * sizeof(Expr*));
        patterns[pattern_count - 1] = pattern;
    } while (parser_match(parser, TOKEN_BIT_OR));

    if (out_patterns) *out_patterns = patterns;
    if (out_count) *out_count = pattern_count;
}

static Expr* parse_comparison(Parser* parser) {
    PARSER_ENTER(parser);
    
    Expr* expr = parse_bit_or(parser);
    
    while (parser_match(parser, TOKEN_LT) || parser_match(parser, TOKEN_LT_EQ) ||
           parser_match(parser, TOKEN_GT) || parser_match(parser, TOKEN_GT_EQ)) {
        TokenType op = parser->previous.type;
        Expr* right = parse_bit_or(parser);
        expr = expr_create_binary(op, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }
    
    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_equality(Parser* parser) {
    PARSER_ENTER(parser);
    
    Expr* expr = parse_comparison(parser);
    
    while (parser_match(parser, TOKEN_EQ_EQ) || parser_match(parser, TOKEN_BANG_EQ)) {
        TokenType op = parser->previous.type;
        Expr* right = parse_comparison(parser);
        expr = expr_create_binary(op, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }
    
    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_and(Parser* parser) {
    PARSER_ENTER(parser);
    
    Expr* expr = parse_equality(parser);
    
    while (parser_match(parser, TOKEN_AND)) {
        Expr* right = parse_equality(parser);
        expr = expr_create_binary(TOKEN_AND, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }
    
    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_or(Parser* parser) {
    PARSER_ENTER(parser);
    
    Expr* expr = parse_and(parser);
    
    while (parser_match(parser, TOKEN_OR)) {
        Expr* right = parse_and(parser);
        expr = expr_create_binary(TOKEN_OR, expr, right, parser->lexer.file, parser->previous.line, parser->previous.column);
    }
    
    PARSER_LEAVE(parser);
    return expr;
}

static Expr* parse_expression(Parser* parser) {
    return parse_or(parser);
}

static Stmt* parse_expression_statement(Parser* parser) {
    Expr* expr = parse_expression(parser);
    return parser_finish_expression_statement(parser, expr, false, NULL);
}

static Stmt* parser_finish_expression_statement(Parser* parser,
                                                Expr* expr,
                                                bool allow_trailing_value,
                                                Expr** out_trailing_value) {
    if (out_trailing_value) {
        *out_trailing_value = NULL;
    }
    if (!expr) return NULL;

    // Assignment statements: `<ident> = <expr>;`, `<index> = <expr>;`, or `<field> = <expr>;`
    // Compound assignment: `<ident> += <expr>;`, `<index> += <expr>;`, `<field> += <expr>;`, ...
    TokenType assign_op = TOKEN_ERROR;
    if (parser_match(parser, TOKEN_ASSIGN) ||
        parser_match(parser, TOKEN_PLUS_EQ) ||
        parser_match(parser, TOKEN_MINUS_EQ) ||
        parser_match(parser, TOKEN_STAR_EQ) ||
        parser_match(parser, TOKEN_SLASH_EQ) ||
        parser_match(parser, TOKEN_PERCENT_EQ)) {
        assign_op = parser->previous.type;
        Expr* value = parse_expression(parser);
        parser_consume(parser, TOKEN_SEMICOLON, "Expected ';' after assignment");

        if (!expr || !value) {
            parser_discard_parse_only_expr(expr);
            parser_discard_parse_only_expr(value);
            return NULL;
        }

        if (assign_op != TOKEN_ASSIGN) {
            TokenType binary_op = TOKEN_ERROR;
            switch (assign_op) {
                case TOKEN_PLUS_EQ: binary_op = TOKEN_PLUS; break;
                case TOKEN_MINUS_EQ: binary_op = TOKEN_MINUS; break;
                case TOKEN_STAR_EQ: binary_op = TOKEN_STAR; break;
                case TOKEN_SLASH_EQ: binary_op = TOKEN_SLASH; break;
                case TOKEN_PERCENT_EQ: binary_op = TOKEN_PERCENT; break;
                default: break;
            }

            if (binary_op == TOKEN_ERROR) {
                parser_error(parser, "Invalid compound assignment operator");
                parser_discard_parse_only_expr(expr);
                parser_discard_parse_only_expr(value);
                return NULL;
            }

            // Identifiers are safe to expand (no side effects).
            if (expr->kind == EXPR_IDENTIFIER) {
                Expr* lhs = expr_create_identifier(expr->identifier, parser->lexer.file, expr->line, expr->column);
                Expr* expanded = expr_create_binary(binary_op, lhs, value, parser->lexer.file, expr->line, expr->column);
                Stmt* stmt = stmt_create_assign(expr->identifier, expanded, TOKEN_ASSIGN, parser->lexer.file, expr->line, expr->column);
                parser_discard_parse_only_expr(expr);
                return stmt;
            }

            // For index/field compound assignment, preserve single-evaluation semantics by
            // keeping the operator on the statement node.
            if (expr->kind == EXPR_INDEX) {
                Expr* target = expr->index.array;
                Expr* index = expr->index.index;
                expr->index.array = NULL;
                expr->index.index = NULL;

                Stmt* stmt = stmt_create_assign_index(target, index, value, assign_op, parser->lexer.file, expr->line, expr->column);
                parser_discard_parse_only_expr(expr);
                return stmt;
            }

            if (expr->kind == EXPR_FIELD_ACCESS) {
                Expr* object = expr->field_access.object;
                char* field_name = expr->field_access.field_name;
                expr->field_access.object = NULL;
                expr->field_access.field_name = NULL;

                Stmt* stmt = stmt_create_assign_field(object, field_name, value, assign_op, parser->lexer.file, expr->line, expr->column);
                if (field_name) free(field_name);
                parser_discard_parse_only_expr(expr);
                return stmt;
            }

            parser_error(parser, "Invalid compound assignment target");
            parser_discard_parse_only_expr(expr);
            parser_discard_parse_only_expr(value);
            return NULL;
        }

        if (expr->kind == EXPR_IDENTIFIER) {
            Stmt* stmt = stmt_create_assign(expr->identifier, value, TOKEN_ASSIGN, parser->lexer.file, expr->line, expr->column);
            parser_discard_parse_only_expr(expr);
            return stmt;
        }

        if (expr->kind == EXPR_INDEX) {
            Expr* target = expr->index.array;
            Expr* index = expr->index.index;
            expr->index.array = NULL;
            expr->index.index = NULL;

            Stmt* stmt = stmt_create_assign_index(target, index, value, TOKEN_ASSIGN, parser->lexer.file, expr->line, expr->column);
            parser_discard_parse_only_expr(expr);
            return stmt;
        }

        if (expr->kind == EXPR_FIELD_ACCESS) {
            Expr* object = expr->field_access.object;
            char* field_name = expr->field_access.field_name;
            expr->field_access.object = NULL;
            expr->field_access.field_name = NULL;

            Stmt* stmt = stmt_create_assign_field(object, field_name, value, TOKEN_ASSIGN, parser->lexer.file, expr->line, expr->column);
            if (field_name) free(field_name);
            parser_discard_parse_only_expr(expr);
            return stmt;
        }

        parser_error(parser, "Invalid assignment target");
        parser_discard_parse_only_expr(expr);
        parser_discard_parse_only_expr(value);
        return NULL;
    }

    if (parser_match(parser, TOKEN_SEMICOLON)) {
        return stmt_create_expr(expr, parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    if (allow_trailing_value && parser->current.type == TOKEN_RBRACE) {
        if (out_trailing_value) {
            *out_trailing_value = expr;
            return NULL;
        }
    }

    parser_error(parser, "Expected ';' after expression");
    parser_discard_parse_only_expr(expr);
    return NULL;
}

static Stmt* parse_block_statement(Parser* parser) {
    Stmt** statements = NULL;
    int stmt_count = 0;
    
    while (parser->current.type != TOKEN_RBRACE && parser->current.type != TOKEN_EOF) {
        Stmt* stmt = parse_declaration(parser);
        if (stmt) {
            stmt_count++;
            statements = (Stmt**)safe_realloc(statements, (size_t)stmt_count * sizeof(Stmt*));
            statements[stmt_count - 1] = stmt;
            continue;
        }

        parser_synchronize(parser, true);
    }
    
    parser_consume(parser, TOKEN_RBRACE, "Expected '}' after block");
    return stmt_create_block(statements, stmt_count, parser->lexer.file, parser->previous.line, parser->previous.column);
}

static Stmt* parse_if_statement(Parser* parser) {
    int if_line = parser->previous.line;
    int if_column = parser->previous.column;

    if (parser_match(parser, TOKEN_KEYWORD_LET)) {
        Expr** patterns = NULL;
        int pattern_count = 0;
        parse_pattern_list(parser, &patterns, &pattern_count);
        parser_consume(parser, TOKEN_ASSIGN, "Expected '=' after if let pattern");
        Expr* subject = parse_expression(parser);

        Stmt* then_branch = parse_statement(parser);
        Stmt* else_branch = NULL;
        if (parser_match(parser, TOKEN_KEYWORD_ELSE)) {
            else_branch = parse_statement(parser);
        }

        Expr** guards = (Expr**)safe_calloc((size_t)pattern_count, sizeof(Expr*));
        Stmt** bodies = (Stmt**)safe_malloc((size_t)pattern_count * sizeof(Stmt*));
        for (int i = 0; i < pattern_count; i++) {
            bodies[i] = (i == pattern_count - 1) ? then_branch : stmt_clone(then_branch);
        }
        if (!else_branch) {
            else_branch = stmt_create_block(NULL, 0, parser->lexer.file, if_line, if_column);
        }

        Stmt* match_stmt = stmt_create_match(subject,
                                             patterns,
                                             guards,
                                             bodies,
                                             pattern_count,
                                             else_branch,
                                             parser->lexer.file,
                                             if_line,
                                             if_column);
        if (match_stmt && pattern_count > 1) {
            match_stmt->match_stmt.arm_group_ids =
                (int*)safe_malloc((size_t)pattern_count * sizeof(int));
            for (int i = 0; i < pattern_count; i++) {
                match_stmt->match_stmt.arm_group_ids[i] = 1;
            }
        }
        return match_stmt;
    }

    parser_consume(parser, TOKEN_LPAREN, "Expected '(' after 'if'");
    Expr* condition = parse_expression(parser);
    parser_consume(parser, TOKEN_RPAREN, "Expected ')' after condition");
    
    Stmt* then_branch = parse_statement(parser);
    Stmt* else_branch = NULL;
    
    if (parser_match(parser, TOKEN_KEYWORD_ELSE)) {
        else_branch = parse_statement(parser);
    }
    
    return stmt_create_if(condition, then_branch, else_branch, parser->lexer.file, if_line, if_column);
}

static Stmt* parse_match_statement(Parser* parser) {
    parser_consume(parser, TOKEN_LPAREN, "Expected '(' after 'match'");
    Expr* subject = parse_expression(parser);
    parser_consume(parser, TOKEN_RPAREN, "Expected ')' after match subject");
    parser_consume(parser, TOKEN_LBRACE, "Expected '{' after match subject");

    Expr** patterns = NULL;
    Expr** guards = NULL;
    Stmt** bodies = NULL;
    int* arm_group_ids = NULL;
    int arm_count = 0;
    int next_arm_group_id = 1;
    Stmt* else_branch = NULL;

    while (parser->current.type != TOKEN_RBRACE && parser->current.type != TOKEN_EOF) {
        if (parser_match(parser, TOKEN_KEYWORD_ELSE)) {
            if (else_branch) {
                parser_error(parser, "match can only contain one else branch");
                break;
            }
            parser_consume(parser, TOKEN_COLON, "Expected ':' after else in match statement");
            else_branch = parse_statement(parser);
            if (parser->current.type != TOKEN_RBRACE) {
                parser_error(parser, "else branch must be the last branch in match statement");
            }
            continue;
        }

        Expr** arm_patterns = NULL;
        int arm_pattern_count = 0;
        parse_pattern_list(parser, &arm_patterns, &arm_pattern_count);
        Expr* guard = NULL;
        if (parser_match(parser, TOKEN_KEYWORD_IF)) {
            guard = parse_expression(parser);
        }
        parser_consume(parser, TOKEN_COLON, "Expected ':' after match pattern");
        Stmt* body = parse_statement(parser);

        int arm_base = arm_count;
        arm_count += arm_pattern_count;
        patterns = (Expr**)safe_realloc(patterns, (size_t)arm_count * sizeof(Expr*));
        guards = (Expr**)safe_realloc(guards, (size_t)arm_count * sizeof(Expr*));
        bodies = (Stmt**)safe_realloc(bodies, (size_t)arm_count * sizeof(Stmt*));
        arm_group_ids = (int*)safe_realloc(arm_group_ids, (size_t)arm_count * sizeof(int));
        int arm_group_id = (arm_pattern_count > 1) ? next_arm_group_id++ : 0;
        for (int i = 0; i < arm_pattern_count; i++) {
            patterns[arm_base + i] = arm_patterns[i];
            guards[arm_base + i] = (i == arm_pattern_count - 1) ? guard : expr_clone(guard);
            bodies[arm_base + i] = (i == arm_pattern_count - 1) ? body : stmt_clone(body);
            arm_group_ids[arm_base + i] = arm_group_id;
        }
        free(arm_patterns);
    }

    parser_consume(parser, TOKEN_RBRACE, "Expected '}' after match branches");

    if (arm_count == 0 && !else_branch) {
        parser_error(parser, "match statement requires at least one branch");
    }

    Stmt* match_stmt = stmt_create_match(subject,
                                         patterns,
                                         guards,
                                         bodies,
                                         arm_count,
                                         else_branch,
                                         parser->lexer.file,
                                         parser->previous.line,
                                         parser->previous.column);
    if (match_stmt) {
        match_stmt->match_stmt.arm_group_ids = arm_group_ids;
        arm_group_ids = NULL;
    }
    if (arm_group_ids) free(arm_group_ids);
    return match_stmt;
}

static Expr* parse_if_expression_branch_value(Parser* parser) {
    if (parser && parser->current.type == TOKEN_LBRACE) {
        return parse_block_expression(parser);
    }
    return parse_expression(parser);
}

static Expr* parse_if_expression(Parser* parser) {
    int if_line = parser->previous.line;
    int if_column = parser->previous.column;

    parser_consume(parser, TOKEN_LPAREN, "Expected '(' after 'if'");
    Expr* condition = parse_expression(parser);
    parser_consume(parser, TOKEN_RPAREN, "Expected ')' after if expression condition");

    Expr* then_expr = parse_if_expression_branch_value(parser);
    Expr* else_expr = NULL;

    if (!parser_match(parser, TOKEN_KEYWORD_ELSE)) {
        parser_error(parser, "if expression requires an else branch");
        else_expr = expr_create_nil(parser->lexer.file, if_line, if_column);
    } else if (parser_match(parser, TOKEN_KEYWORD_IF)) {
        else_expr = parse_if_expression(parser);
    } else {
        else_expr = parse_if_expression_branch_value(parser);
    }

    return expr_create_if(condition,
                          then_expr,
                          else_expr,
                          parser->lexer.file,
                          if_line,
                          if_column);
}

static Expr* parse_match_expression(Parser* parser) {
    int match_line = parser->previous.line;
    int match_column = parser->previous.column;

    parser_consume(parser, TOKEN_LPAREN, "Expected '(' after 'match'");
    Expr* subject = parse_expression(parser);
    parser_consume(parser, TOKEN_RPAREN, "Expected ')' after match subject");
    parser_consume(parser, TOKEN_LBRACE, "Expected '{' after match subject");

    Expr** patterns = NULL;
    Expr** guards = NULL;
    Expr** values = NULL;
    int* arm_group_ids = NULL;
    int arm_count = 0;
    int next_arm_group_id = 1;
    Expr* else_expr = NULL;

    while (parser->current.type != TOKEN_RBRACE && parser->current.type != TOKEN_EOF) {
        if (parser_match(parser, TOKEN_KEYWORD_ELSE)) {
            if (else_expr) {
                parser_error(parser, "match expression can only contain one else branch");
                break;
            }
            parser_consume(parser, TOKEN_COLON, "Expected ':' after else in match expression");
            else_expr = parse_match_arm_value(parser);
            if (parser_match(parser, TOKEN_COMMA)) {
                if (parser->current.type != TOKEN_RBRACE) {
                    parser_error(parser, "else branch must be the last branch in match expression");
                }
            } else if (parser->current.type != TOKEN_RBRACE) {
                parser_error(parser, "Expected ',' or '}' after match expression else branch");
            }
            continue;
        }

        Expr** arm_patterns = NULL;
        int arm_pattern_count = 0;
        parse_pattern_list(parser, &arm_patterns, &arm_pattern_count);
        Expr* guard = NULL;
        if (parser_match(parser, TOKEN_KEYWORD_IF)) {
            guard = parse_expression(parser);
        }
        parser_consume(parser, TOKEN_COLON, "Expected ':' after match expression pattern");
        Expr* value = parse_match_arm_value(parser);

        int arm_base = arm_count;
        arm_count += arm_pattern_count;
        patterns = (Expr**)safe_realloc(patterns, (size_t)arm_count * sizeof(Expr*));
        guards = (Expr**)safe_realloc(guards, (size_t)arm_count * sizeof(Expr*));
        values = (Expr**)safe_realloc(values, (size_t)arm_count * sizeof(Expr*));
        arm_group_ids = (int*)safe_realloc(arm_group_ids, (size_t)arm_count * sizeof(int));
        int arm_group_id = (arm_pattern_count > 1) ? next_arm_group_id++ : 0;
        for (int i = 0; i < arm_pattern_count; i++) {
            patterns[arm_base + i] = arm_patterns[i];
            guards[arm_base + i] = (i == arm_pattern_count - 1) ? guard : expr_clone(guard);
            values[arm_base + i] = (i == arm_pattern_count - 1) ? value : expr_clone(value);
            arm_group_ids[arm_base + i] = arm_group_id;
        }
        free(arm_patterns);

        if (parser_match(parser, TOKEN_COMMA)) {
            continue;
        }
        if (parser->current.type != TOKEN_RBRACE) {
            parser_error(parser, "Expected ',' or '}' after match expression arm");
        }
    }

    parser_consume(parser, TOKEN_RBRACE, "Expected '}' after match expression branches");

    if (arm_count == 0 && !else_expr) {
        parser_error(parser, "match expression requires at least one branch");
    }

    Expr* match_expr = expr_create_match(subject,
                                         patterns,
                                         guards,
                                         values,
                                         arm_count,
                                         else_expr,
                                         parser->lexer.file,
                                         match_line,
                                         match_column);
    if (match_expr) {
        match_expr->match_expr.arm_group_ids = arm_group_ids;
        arm_group_ids = NULL;
    }
    if (arm_group_ids) free(arm_group_ids);
    return match_expr;
}

static Expr* parse_match_arm_value(Parser* parser) {
    if (parser && parser->current.type == TOKEN_LBRACE &&
        parser_match_value_starts_block_expression(parser)) {
        return parse_block_expression(parser);
    }
    return parse_expression(parser);
}

static Expr* parse_block_expression(Parser* parser) {
    int block_line = parser->current.line;
    int block_column = parser->current.column;
    parser_consume(parser, TOKEN_LBRACE, "Expected '{' to start block expression");

    Stmt** statements = NULL;
    int stmt_count = 0;
    Expr* value = NULL;

    while (parser->current.type != TOKEN_RBRACE && parser->current.type != TOKEN_EOF) {
        if (parser_token_starts_declaration(parser->current.type)) {
            Stmt* stmt = parse_declaration(parser);
            if (!stmt) break;
            stmt_count++;
            statements = (Stmt**)safe_realloc(statements, (size_t)stmt_count * sizeof(Stmt*));
            statements[stmt_count - 1] = stmt;
            continue;
        }

        if (parser->current.type == TOKEN_LBRACE ||
            parser_token_starts_non_expression_statement(parser->current.type)) {
            Stmt* stmt = parse_statement(parser);
            if (!stmt) break;
            stmt_count++;
            statements = (Stmt**)safe_realloc(statements, (size_t)stmt_count * sizeof(Stmt*));
            statements[stmt_count - 1] = stmt;
            continue;
        }

        Expr* expr = parse_expression(parser);
        Stmt* stmt = parser_finish_expression_statement(parser, expr, true, &value);
        if (value) {
            break;
        }
        if (!stmt) {
            break;
        }

        stmt_count++;
        statements = (Stmt**)safe_realloc(statements, (size_t)stmt_count * sizeof(Stmt*));
        statements[stmt_count - 1] = stmt;
    }

    parser_consume(parser, TOKEN_RBRACE, "Expected '}' after block expression");
    if (!value) {
        parser_error_at(parser,
                        "Block expression requires a trailing value expression",
                        parser->previous.line,
                        parser->previous.column);
        value = expr_create_nil(parser->lexer.file, block_line, block_column);
    }

    return expr_create_block(statements,
                             stmt_count,
                             value,
                             parser->lexer.file,
                             block_line,
                             block_column);
}

static char* parser_make_synthetic_name(Parser* parser, const char* prefix) {
    char buffer[96];
    int next_id = parser ? parser->synthetic_counter++ : 0;
    snprintf(buffer, sizeof(buffer), "__%s_%d", prefix ? prefix : "tmp", next_id);
    return safe_strdup(buffer);
}

static void parser_free_type_case_labels(Type** types, int type_count) {
    if (!types) return;
    for (int i = 0; i < type_count; i++) {
        type_free(types[i]);
    }
    free(types);
}

static void parser_free_type_switch_arms(SwitchTypeArm* arms, int arm_count) {
    if (!arms) return;
    for (int i = 0; i < arm_count; i++) {
        parser_free_type_case_labels(arms[i].types, arms[i].type_count);
        if (arms[i].binding_name) free(arms[i].binding_name);
        parser_discard_parse_only_stmt(arms[i].body);
    }
    free(arms);
}

static Expr* parser_build_type_switch_condition(Parser* parser,
                                                const char* subject_name,
                                                Type** case_types,
                                                int case_type_count,
                                                int line,
                                                int column) {
    Expr* condition = NULL;

    for (int i = 0; i < case_type_count; i++) {
        Expr* subject_expr = expr_create_identifier((char*)subject_name, parser->lexer.file, line, column);
        Expr* test_expr = expr_create_type_test(subject_expr, case_types[i], parser->lexer.file, line, column);
        if (!condition) {
            condition = test_expr;
        } else {
            condition = expr_create_binary(TOKEN_OR, condition, test_expr, parser->lexer.file, line, column);
        }
    }

    return condition;
}

static Stmt* parser_lower_type_switch(Parser* parser,
                                      Expr* subject,
                                      SwitchTypeArm* arms,
                                      int arm_count,
                                      Stmt* else_branch,
                                      int line,
                                      int column) {
    char* temp_name = parser_make_synthetic_name(parser, "switch_type_subject");
    Stmt* temp_decl = stmt_create_var_decl(temp_name, NULL, subject, true,
                                           parser->lexer.file, line, column);
    free(temp_name);

    Stmt* branch_chain = else_branch;
    for (int i = arm_count - 1; i >= 0; i--) {
        Stmt* branch_body = arms[i].body;
        if (arms[i].binding_name && arms[i].types && arms[i].type_count == 1) {
            Type* binding_annotation = type_clone(arms[i].types[0]);
            Type* binding_cast_type = type_clone(arms[i].types[0]);
            Expr* binding_source = expr_create_identifier(temp_decl->var_decl.name,
                                                          parser->lexer.file,
                                                          arms[i].line,
                                                          arms[i].column);
            Expr* binding_init = expr_create_cast(binding_source,
                                                  binding_cast_type,
                                                  parser->lexer.file,
                                                  arms[i].line,
                                                  arms[i].column);
            Stmt* binding_decl = stmt_create_var_decl(arms[i].binding_name,
                                                      binding_annotation,
                                                      binding_init,
                                                      true,
                                                      parser->lexer.file,
                                                      arms[i].line,
                                                      arms[i].column);

            Stmt** bound_statements = (Stmt**)safe_malloc(2 * sizeof(Stmt*));
            bound_statements[0] = binding_decl;
            bound_statements[1] = branch_body;
            branch_body = stmt_create_block(bound_statements,
                                            2,
                                            parser->lexer.file,
                                            arms[i].line,
                                            arms[i].column);
        }

        Expr* condition = parser_build_type_switch_condition(parser,
                                                             temp_decl->var_decl.name,
                                                             arms[i].types,
                                                             arms[i].type_count,
                                                             arms[i].line,
                                                             arms[i].column);
        branch_chain = stmt_create_if(condition,
                                      branch_body,
                                      branch_chain,
                                      parser->lexer.file,
                                      arms[i].line,
                                      arms[i].column);
        if (arms[i].binding_name) free(arms[i].binding_name);
        free(arms[i].types);
    }

    free(arms);

    int stmt_count = branch_chain ? 2 : 1;
    Stmt** statements = (Stmt**)safe_malloc((size_t)stmt_count * sizeof(Stmt*));
    statements[0] = temp_decl;
    if (branch_chain) {
        statements[1] = branch_chain;
    }

    return stmt_create_block(statements, stmt_count, parser->lexer.file, line, column);
}

static Stmt* parse_switch_statement(Parser* parser) {
    int switch_line = parser->previous.line;
    int switch_column = parser->previous.column;
    parser_consume(parser, TOKEN_LPAREN, "Expected '(' after 'switch'");
    Expr* subject = parse_expression(parser);
    parser_consume(parser, TOKEN_RPAREN, "Expected ')' after switch subject");
    parser_consume(parser, TOKEN_LBRACE, "Expected '{' after switch subject");

    Expr** patterns = NULL;
    Stmt** bodies = NULL;
    int arm_count = 0;
    SwitchTypeArm* type_arms = NULL;
    int type_arm_count = 0;
    Stmt* else_branch = NULL;
    bool saw_type_case = false;
    bool saw_value_case = false;

    while (parser->current.type != TOKEN_RBRACE && parser->current.type != TOKEN_EOF) {
        if (parser_match(parser, TOKEN_KEYWORD_DEFAULT)) {
            if (else_branch) {
                parser_error(parser, "switch can only contain one default branch");
                break;
            }
            parser_consume(parser, TOKEN_COLON, "Expected ':' after default in switch statement");
            else_branch = parse_statement(parser);
            if (parser->current.type != TOKEN_RBRACE) {
                parser_error(parser, "default branch must be the last branch in switch statement");
            }
            continue;
        }

        if (!parser_match(parser, TOKEN_KEYWORD_CASE)) {
            parser_error(parser, "Expected 'case' or 'default' in switch statement");
            break;
        }

        bool is_type_case = parser_match(parser, TOKEN_KEYWORD_TYPE);
        int case_line = parser->current.line;
        int case_column = parser->current.column;

        if (is_type_case) {
            saw_type_case = true;
            if (saw_value_case) {
                parser_error(parser, "switch cannot mix value cases with 'case type' branches");
            }

            Type** case_types = NULL;
            int case_type_count = 0;
            char* binding_name = NULL;
            do {
                if (!parser_token_can_start_type_arg(parser->current.type)) {
                    parser_error(parser, "Expected type after 'case type'");
                    break;
                }
                Type* case_type = parse_type(parser);
                case_type_count++;
                case_types = (Type**)safe_realloc(case_types, (size_t)case_type_count * sizeof(Type*));
                case_types[case_type_count - 1] = case_type;
            } while (parser_match(parser, TOKEN_COMMA));

            if (parser_match(parser, TOKEN_AS)) {
                if (case_type_count != 1) {
                    parser_error(parser, "switch type case binding requires exactly one target type");
                }
                parser_consume(parser, TOKEN_IDENTIFIER, "Expected binding name after 'as' in switch type case");
                binding_name = safe_strdup(parser->previous.lexeme);
            }

            parser_consume(parser, TOKEN_COLON, "Expected ':' after switch type case");
            Stmt* body = parse_statement(parser);

            if (saw_value_case) {
                parser_free_type_case_labels(case_types, case_type_count);
                if (binding_name) free(binding_name);
                parser_discard_parse_only_stmt(body);
                continue;
            }

            type_arm_count++;
            type_arms = (SwitchTypeArm*)safe_realloc(type_arms, (size_t)type_arm_count * sizeof(SwitchTypeArm));
            type_arms[type_arm_count - 1].types = case_types;
            type_arms[type_arm_count - 1].type_count = case_type_count;
            type_arms[type_arm_count - 1].binding_name = binding_name;
            type_arms[type_arm_count - 1].body = body;
            type_arms[type_arm_count - 1].line = case_line;
            type_arms[type_arm_count - 1].column = case_column;
            continue;
        }

        saw_value_case = true;
        if (saw_type_case) {
            parser_error(parser, "switch cannot mix value cases with 'case type' branches");
        }

        Expr** case_patterns = NULL;
        int case_pattern_count = 0;
        do {
            Expr* pattern = parse_expression(parser);
            case_pattern_count++;
            case_patterns = (Expr**)safe_realloc(case_patterns, (size_t)case_pattern_count * sizeof(Expr*));
            case_patterns[case_pattern_count - 1] = pattern;
        } while (parser_match(parser, TOKEN_COMMA));

        parser_consume(parser, TOKEN_COLON, "Expected ':' after switch case pattern");
        Stmt* body = parse_statement(parser);

        if (saw_type_case) {
            for (int i = 0; i < case_pattern_count; i++) {
                parser_discard_parse_only_expr(case_patterns[i]);
            }
            free(case_patterns);
            parser_discard_parse_only_stmt(body);
            continue;
        }

        int arm_base = arm_count;
        arm_count += case_pattern_count;
        patterns = (Expr**)safe_realloc(patterns, (size_t)arm_count * sizeof(Expr*));
        bodies = (Stmt**)safe_realloc(bodies, (size_t)arm_count * sizeof(Stmt*));

        for (int i = 0; i < case_pattern_count; i++) {
            patterns[arm_base + i] = case_patterns[i];
            bodies[arm_base + i] = (i == case_pattern_count - 1) ? body : stmt_clone(body);
        }
        free(case_patterns);
    }

    parser_consume(parser, TOKEN_RBRACE, "Expected '}' after switch branches");

    if (arm_count == 0 && type_arm_count == 0 && !else_branch) {
        parser_error(parser, "switch statement requires at least one case or default branch");
    }

    if (saw_type_case) {
        if (patterns || bodies) {
            for (int i = 0; i < arm_count; i++) {
                parser_discard_parse_only_expr(patterns ? patterns[i] : NULL);
                parser_discard_parse_only_stmt(bodies ? bodies[i] : NULL);
            }
            if (patterns) free(patterns);
            if (bodies) free(bodies);
        }
        return parser_lower_type_switch(parser,
                                        subject,
                                        type_arms,
                                        type_arm_count,
                                        else_branch,
                                        switch_line,
                                        switch_column);
    }

    if (type_arms) {
        parser_free_type_switch_arms(type_arms, type_arm_count);
    }

    return stmt_create_match(subject, patterns, NULL, bodies, arm_count, else_branch,
                             parser->lexer.file, switch_line, switch_column);
}

static Stmt* parse_while_statement(Parser* parser) {
    int while_line = parser->previous.line;
    int while_column = parser->previous.column;

    if (parser_match(parser, TOKEN_KEYWORD_LET)) {
        Expr** patterns = NULL;
        int pattern_count = 0;
        parse_pattern_list(parser, &patterns, &pattern_count);
        parser_consume(parser, TOKEN_ASSIGN, "Expected '=' after while let pattern");
        Expr* subject = parse_expression(parser);
        Stmt* body = parse_statement(parser);

        Expr** guards = (Expr**)safe_calloc((size_t)pattern_count, sizeof(Expr*));
        Stmt** bodies = (Stmt**)safe_malloc((size_t)pattern_count * sizeof(Stmt*));
        for (int i = 0; i < pattern_count; i++) {
            bodies[i] = (i == pattern_count - 1) ? body : stmt_clone(body);
        }

        Stmt* else_branch = stmt_create_break(parser->lexer.file, while_line, while_column);
        Stmt* match_stmt = stmt_create_match(subject,
                                             patterns,
                                             guards,
                                             bodies,
                                             pattern_count,
                                             else_branch,
                                             parser->lexer.file,
                                             while_line,
                                             while_column);
        if (match_stmt && pattern_count > 1) {
            match_stmt->match_stmt.arm_group_ids =
                (int*)safe_malloc((size_t)pattern_count * sizeof(int));
            for (int i = 0; i < pattern_count; i++) {
                match_stmt->match_stmt.arm_group_ids[i] = 1;
            }
        }
        Expr* loop_condition = expr_create_literal_bool(true, parser->lexer.file, while_line, while_column);
        return stmt_create_while(loop_condition, match_stmt, parser->lexer.file, while_line, while_column);
    }

    parser_consume(parser, TOKEN_LPAREN, "Expected '(' after 'while'");
    Expr* condition = parse_expression(parser);
    parser_consume(parser, TOKEN_RPAREN, "Expected ')' after condition");
    
    Stmt* body = parse_statement(parser);
    return stmt_create_while(condition, body, parser->lexer.file, while_line, while_column);
}

static Stmt* parse_foreach_statement(Parser* parser) {
    parser_consume(parser, TOKEN_LPAREN, "Expected '(' after 'foreach'");

    // Allow optional 'var' in foreach (foreach (var x in arr) ...)
    parser_match(parser, TOKEN_KEYWORD_VAR);

    parser_consume(parser, TOKEN_IDENTIFIER, "Expected variable name in foreach");
    char* var_name = safe_strdup(parser->previous.lexeme);
    
    parser_consume(parser, TOKEN_IN, "Expected 'in' after variable name");
    
    Expr* start_or_iterable = parse_expression(parser);
    Expr* range_end = NULL;
    bool is_range_loop = false;
    if (parser_match(parser, TOKEN_DOT_DOT)) {
        is_range_loop = true;
        range_end = parse_expression(parser);
    }
    parser_consume(parser, TOKEN_RPAREN, "Expected ')' after iterable");
    
    Stmt* body = parse_statement(parser);
    Stmt* stmt = NULL;
    if (is_range_loop) {
        stmt = stmt_create_for_range(var_name, start_or_iterable, range_end, body,
                                     parser->lexer.file, parser->previous.line, parser->previous.column);
    } else {
        stmt = stmt_create_foreach(var_name, start_or_iterable, body,
                                   parser->lexer.file, parser->previous.line, parser->previous.column);
    }
    free(var_name);
    return stmt;
}

static Stmt* parse_var_declaration(Parser* parser, bool is_mutable) {
    if (parser_match(parser, TOKEN_LPAREN)) {
        char** names = NULL;
        int name_count = 0;

        if (parser->current.type != TOKEN_RPAREN) {
            do {
                parser_consume(parser, TOKEN_IDENTIFIER, "Expected variable name");
                name_count++;
                names = (char**)safe_realloc(names, name_count * sizeof(char*));
                names[name_count - 1] = safe_strdup(parser->previous.lexeme);
            } while (parser_match(parser, TOKEN_COMMA));
        }

        parser_consume(parser, TOKEN_RPAREN, "Expected ')' after variable list");

        Type* type_annotation = NULL;
        if (parser_match(parser, TOKEN_COLON)) {
            type_annotation = parse_type(parser);
        }

        parser_consume(parser, TOKEN_ASSIGN, "Expected '=' after tuple variable declaration");
        Expr* initializer = parse_expression(parser);

        parser_consume(parser, TOKEN_SEMICOLON, "Expected ';' after variable declaration");
        return stmt_create_var_tuple_decl(names, name_count, type_annotation, initializer, is_mutable,
                                          parser->lexer.file, parser->previous.line, parser->previous.column);
    }

    parser_consume(parser, TOKEN_IDENTIFIER, "Expected variable name");
    char* name = safe_strdup(parser->previous.lexeme);

    Type* type_annotation = NULL;
    if (parser_match(parser, TOKEN_COLON)) {
        type_annotation = parse_type(parser);
    }

    Expr* initializer = NULL;
    if (parser_match(parser, TOKEN_ASSIGN)) {
        initializer = parse_expression(parser);
    }
    if (!is_mutable && !initializer) {
        parser_error(parser, "Const declarations require an initializer");
    }

    parser_consume(parser, TOKEN_SEMICOLON, "Expected ';' after variable declaration");
    Stmt* stmt = stmt_create_var_decl(name, type_annotation, initializer, is_mutable,
                                      parser->lexer.file, parser->previous.line, parser->previous.column);
    free(name);
    return stmt;
}

static void parse_function_signature(Parser* parser,
                                     char*** out_params,
                                     Type*** out_param_types,
                                     int* out_param_count,
                                     Type** out_return_type) {
    char** params = NULL;
    Type** param_types = NULL;
    int param_count = 0;

    parser_consume(parser, TOKEN_LPAREN, "Expected '(' after function keyword");

    if (parser->current.type != TOKEN_RPAREN) {
        do {
            parser_consume(parser, TOKEN_IDENTIFIER, "Expected parameter name");
            char* param_name = safe_strdup(parser->previous.lexeme);

            parser_consume(parser, TOKEN_COLON, "Expected ':' after parameter name");
            Type* param_type = parse_type(parser);

            param_count++;
            params = (char**)safe_realloc(params, param_count * sizeof(char*));
            param_types = (Type**)safe_realloc(param_types, param_count * sizeof(Type*));
            params[param_count - 1] = param_name;
            param_types[param_count - 1] = param_type;
        } while (parser_match(parser, TOKEN_COMMA));
    }

    parser_consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");

    Type* return_type = NULL;
    if (parser_match(parser, TOKEN_COLON)) {
        return_type = parse_type(parser);
    } else {
        return_type = type_void();
    }

    *out_params = params;
    *out_param_types = param_types;
    *out_param_count = param_count;
    *out_return_type = return_type;
}

static void parse_declaration_type_params(Parser* parser,
                                          char*** out_type_params,
                                          int* out_type_param_count) {
    char** type_params = NULL;
    int type_param_count = 0;

    if (parser_match(parser, TOKEN_LBRACKET)) {
        do {
            parser_consume(parser, TOKEN_IDENTIFIER, "Expected type parameter name");
            char* type_param_name = safe_strdup(parser->previous.lexeme);
            bool duplicate = false;

            for (int i = 0; i < type_param_count; i++) {
                if (strcmp(type_params[i], type_param_name) == 0) {
                    parser_error(parser, "Duplicate type parameter name");
                    duplicate = true;
                    break;
                }
            }

            if (duplicate) {
                free(type_param_name);
                continue;
            }

            type_param_count++;
            type_params = (char**)safe_realloc(type_params, (size_t)type_param_count * sizeof(char*));
            type_params[type_param_count - 1] = type_param_name;
        } while (parser_match(parser, TOKEN_COMMA));

        parser_consume(parser, TOKEN_RBRACKET, "Expected ']' after type parameters");
    }

    *out_type_params = type_params;
    *out_type_param_count = type_param_count;
}

static void parse_function_type_params(Parser* parser,
                                       char*** out_type_params,
                                       Type*** out_type_param_constraints,
                                       int* out_type_param_count) {
    char** type_params = NULL;
    Type** type_param_constraints = NULL;
    int type_param_count = 0;

    if (parser_match(parser, TOKEN_LBRACKET)) {
        do {
            parser_consume(parser, TOKEN_IDENTIFIER, "Expected type parameter name");
            char* type_param_name = safe_strdup(parser->previous.lexeme);
            bool duplicate = false;

            for (int i = 0; i < type_param_count; i++) {
                if (strcmp(type_params[i], type_param_name) == 0) {
                    parser_error(parser, "Duplicate type parameter name");
                    duplicate = true;
                    break;
                }
            }

            if (duplicate) {
                free(type_param_name);
                if (parser_match(parser, TOKEN_COLON)) {
                    (void)parse_type(parser);
                }
                continue;
            }

            type_param_count++;
            type_params = (char**)safe_realloc(type_params, (size_t)type_param_count * sizeof(char*));
            type_param_constraints = (Type**)safe_realloc(type_param_constraints, (size_t)type_param_count * sizeof(Type*));
            type_params[type_param_count - 1] = type_param_name;
            type_param_constraints[type_param_count - 1] = NULL;

            char** old_active_type_params = parser->active_type_params;
            int old_active_type_param_count = parser->active_type_param_count;
            parser->active_type_params = type_params;
            parser->active_type_param_count = type_param_count;

            if (parser_match(parser, TOKEN_COLON)) {
                type_param_constraints[type_param_count - 1] = parse_type(parser);
            }

            parser->active_type_params = old_active_type_params;
            parser->active_type_param_count = old_active_type_param_count;
        } while (parser_match(parser, TOKEN_COMMA));

        parser_consume(parser, TOKEN_RBRACKET, "Expected ']' after type parameters");
    }

    *out_type_params = type_params;
    *out_type_param_constraints = type_param_constraints;
    *out_type_param_count = type_param_count;
}

static Expr* parse_func_literal(Parser* parser, int line, int column, bool is_async) {
    char** params = NULL;
    Type** param_types = NULL;
    int param_count = 0;
    Type* return_type = NULL;

    parse_function_signature(parser, &params, &param_types, &param_count, &return_type);
    parser_consume(parser, TOKEN_LBRACE, "Expected '{' after function literal signature");
    bool old_current_function_is_async = parser->current_function_is_async;
    parser->current_function_is_async = is_async;
    Stmt* body = parse_block_statement(parser);
    parser->current_function_is_async = old_current_function_is_async;

    return expr_create_func_literal(return_type,
                                    params,
                                    param_types,
                                    param_count,
                                    body,
                                    is_async,
                                    parser->lexer.file,
                                    line,
                                    column);
}

static Stmt* parse_func_declaration(Parser* parser, bool is_async) {
    parser_consume(parser, TOKEN_IDENTIFIER, "Expected function name");
    char* name = safe_strdup(parser->previous.lexeme);

    char** type_params = NULL;
    Type** type_param_constraints = NULL;
    int type_param_count = 0;
    parse_function_type_params(parser, &type_params, &type_param_constraints, &type_param_count);

    char** old_active_type_params = parser->active_type_params;
    int old_active_type_param_count = parser->active_type_param_count;
    parser->active_type_params = type_params;
    parser->active_type_param_count = type_param_count;

    char** params = NULL;
    Type** param_types = NULL;
    int param_count = 0;
    Type* return_type = NULL;
    parse_function_signature(parser, &params, &param_types, &param_count, &return_type);
    
    parser_consume(parser, TOKEN_LBRACE, "Expected '{' after function signature");
    bool old_current_function_is_async = parser->current_function_is_async;
    parser->current_function_is_async = is_async;
    Stmt* body = parse_block_statement(parser);
    parser->current_function_is_async = old_current_function_is_async;

    parser->active_type_params = old_active_type_params;
    parser->active_type_param_count = old_active_type_param_count;

    Stmt* stmt = stmt_create_func_decl(return_type,
                                       name,
                                       type_params,
                                       type_param_constraints,
                                       type_param_count,
                                       params,
                                       param_types,
                                       param_count,
                                       body,
                                       is_async,
                                       parser->lexer.file,
                                       parser->previous.line,
                                       parser->previous.column);
    free(name);
    return stmt;
}

static Stmt* parse_import_statement(Parser* parser) {
    parser_consume(parser, TOKEN_STRING, "Expected string path for import");
    char* path = safe_strdup(parser->previous.as_string);
    parser_consume(parser, TOKEN_SEMICOLON, "Expected ';' after import");
    Stmt* stmt = stmt_create_import(path, parser->lexer.file, parser->previous.line, parser->previous.column);
    free(path);
    return stmt;
}

static Stmt* parse_type_alias_declaration(Parser* parser) {
    parser_consume(parser, TOKEN_IDENTIFIER, "Expected type alias name");
    char* name = safe_strdup(parser->previous.lexeme);

    char** type_params = NULL;
    int type_param_count = 0;
    parse_declaration_type_params(parser, &type_params, &type_param_count);

    char** old_active_type_params = parser->active_type_params;
    int old_active_type_param_count = parser->active_type_param_count;
    parser->active_type_params = type_params;
    parser->active_type_param_count = type_param_count;

    parser_consume(parser, TOKEN_ASSIGN, "Expected '=' in type alias declaration");
    Type* target_type = parse_type(parser);

    parser->active_type_params = old_active_type_params;
    parser->active_type_param_count = old_active_type_param_count;

    parser_consume(parser, TOKEN_SEMICOLON, "Expected ';' after type alias declaration");

    Stmt* stmt = stmt_create_type_alias(name,
                                        type_params,
                                        type_param_count,
                                        target_type,
                                        parser->lexer.file,
                                        parser->previous.line,
                                        parser->previous.column);
    free(name);
    return stmt;
}

static Stmt* parse_record_declaration(Parser* parser) {
    parser_consume(parser, TOKEN_IDENTIFIER, "Expected record name");
    char* name = safe_strdup(parser->previous.lexeme);

    char** type_params = NULL;
    int type_param_count = 0;
    parse_declaration_type_params(parser, &type_params, &type_param_count);

    char** old_active_type_params = parser->active_type_params;
    int old_active_type_param_count = parser->active_type_param_count;
    parser->active_type_params = type_params;
    parser->active_type_param_count = type_param_count;
    
    parser_consume(parser, TOKEN_LBRACE, "Expected '{' after record name");
    
    char** field_names = NULL;
    Type** field_types = NULL;
    int field_count = 0;
    
    while (parser->current.type != TOKEN_RBRACE && parser->current.type != TOKEN_EOF) {
        parser_consume_identifier_like(parser, "Expected field name");
        char* field_name = safe_strdup(parser->previous.lexeme);
        
        parser_consume(parser, TOKEN_COLON, "Expected ':' after field name");
        Type* field_type = parse_type(parser);
        
        field_count++;
        field_names = (char**)safe_realloc(field_names, field_count * sizeof(char*));
        field_types = (Type**)safe_realloc(field_types, field_count * sizeof(Type*));
        field_names[field_count - 1] = field_name;
        field_types[field_count - 1] = field_type;

        if (parser_match(parser, TOKEN_COMMA) || parser_match(parser, TOKEN_SEMICOLON)) {
            continue;
        }

        if (parser->current.type != TOKEN_RBRACE) {
            parser_error(parser, "Expected ',' or ';' after record field");
            break;
        }
    }
    
    parser->active_type_params = old_active_type_params;
    parser->active_type_param_count = old_active_type_param_count;

    parser_consume(parser, TOKEN_RBRACE, "Expected '}' after record fields");
    parser_match(parser, TOKEN_SEMICOLON);
    
    Stmt* stmt = stmt_create_record_decl(name,
                                         type_params,
                                         type_param_count,
                                         field_names,
                                         field_types,
                                         field_count,
                                         parser->lexer.file,
                                         parser->previous.line,
                                         parser->previous.column);
    free(name);
    return stmt;
}

static Stmt* parse_interface_declaration(Parser* parser) {
    parser_consume(parser, TOKEN_IDENTIFIER, "Expected interface name");
    char* name = safe_strdup(parser->previous.lexeme);

    parser_consume(parser, TOKEN_LBRACE, "Expected '{' after interface name");

    char** method_names = NULL;
    Type** method_types = NULL;
    int method_count = 0;

    while (parser->current.type != TOKEN_RBRACE && parser->current.type != TOKEN_EOF) {
        parser_consume(parser, TOKEN_IDENTIFIER, "Expected method name");
        char* method_name = safe_strdup(parser->previous.lexeme);

        char** params = NULL;
        Type** param_types = NULL;
        int param_count = 0;
        Type* return_type = NULL;
        parse_function_signature(parser, &params, &param_types, &param_count, &return_type);

        for (int i = 0; i < param_count; i++) {
            free(params[i]);
        }
        if (params) free(params);

        Type* method_type = type_function(return_type, param_types, param_count);

        method_count++;
        method_names = (char**)safe_realloc(method_names, (size_t)method_count * sizeof(char*));
        method_types = (Type**)safe_realloc(method_types, (size_t)method_count * sizeof(Type*));
        method_names[method_count - 1] = method_name;
        method_types[method_count - 1] = method_type;

        if (parser_match(parser, TOKEN_COMMA) || parser_match(parser, TOKEN_SEMICOLON)) {
            continue;
        }

        if (parser->current.type != TOKEN_RBRACE) {
            parser_error(parser, "Expected ',' or ';' after interface method");
            break;
        }
    }

    parser_consume(parser, TOKEN_RBRACE, "Expected '}' after interface methods");
    parser_match(parser, TOKEN_SEMICOLON);

    Stmt* stmt = stmt_create_interface_decl(name, method_names, method_types, method_count, parser->lexer.file, parser->previous.line, parser->previous.column);
    free(name);
    return stmt;
}

static Stmt* parse_impl_declaration(Parser* parser) {
    parser_consume(parser, TOKEN_IDENTIFIER, "Expected interface name");
    char* interface_name = safe_strdup(parser->previous.lexeme);

    parser_consume(parser, TOKEN_AS, "Expected 'as' after interface name in impl declaration");

    parser_consume(parser, TOKEN_IDENTIFIER, "Expected record name in impl declaration");
    char* record_name = safe_strdup(parser->previous.lexeme);

    // Optional generic record parameter list for impl declarations, e.g.:
    // impl Formatter as Box[T] { ... }
    if (parser_match(parser, TOKEN_LBRACKET)) {
        do {
            parser_consume(parser, TOKEN_IDENTIFIER, "Expected generic type parameter name in impl record target");
        } while (parser_match(parser, TOKEN_COMMA));
        parser_consume(parser, TOKEN_RBRACKET, "Expected ']' after impl record generic parameters");
    }

    parser_consume(parser, TOKEN_LBRACE, "Expected '{' after impl record name");

    char** method_names = NULL;
    char** function_names = NULL;
    int method_count = 0;

    while (parser->current.type != TOKEN_RBRACE && parser->current.type != TOKEN_EOF) {
        parser_consume(parser, TOKEN_IDENTIFIER, "Expected interface method name in impl declaration");
        char* method_name = safe_strdup(parser->previous.lexeme);

        parser_consume(parser, TOKEN_ASSIGN, "Expected '=' after interface method name in impl declaration");

        parser_consume(parser, TOKEN_IDENTIFIER, "Expected function name in impl declaration");
        char* function_name = safe_strdup(parser->previous.lexeme);

        method_count++;
        method_names = (char**)safe_realloc(method_names, (size_t)method_count * sizeof(char*));
        function_names = (char**)safe_realloc(function_names, (size_t)method_count * sizeof(char*));
        method_names[method_count - 1] = method_name;
        function_names[method_count - 1] = function_name;

        if (parser_match(parser, TOKEN_COMMA) || parser_match(parser, TOKEN_SEMICOLON)) {
            continue;
        }

        if (parser->current.type != TOKEN_RBRACE) {
            parser_error(parser, "Expected ',' or ';' after impl method mapping");
            break;
        }
    }

    parser_consume(parser, TOKEN_RBRACE, "Expected '}' after impl mappings");
    parser_match(parser, TOKEN_SEMICOLON);

    Stmt* stmt = stmt_create_impl_decl(interface_name,
                                       record_name,
                                       method_names,
                                       function_names,
                                       method_count,
                                       parser->lexer.file,
                                       parser->previous.line,
                                       parser->previous.column);
    free(interface_name);
    free(record_name);
    return stmt;
}

static Stmt* parse_enum_declaration(Parser* parser) {
    parser_consume(parser, TOKEN_IDENTIFIER, "Expected enum name");
    char* name = safe_strdup(parser->previous.lexeme);

    char** type_params = NULL;
    int type_param_count = 0;
    parse_declaration_type_params(parser, &type_params, &type_param_count);

    char** old_active_type_params = parser->active_type_params;
    int old_active_type_param_count = parser->active_type_param_count;
    parser->active_type_params = type_params;
    parser->active_type_param_count = type_param_count;

    parser_consume(parser, TOKEN_LBRACE, "Expected '{' after enum name");

    char** member_names = NULL;
    int64_t* member_values = NULL;
    Type*** member_payload_types = NULL;
    int* member_payload_counts = NULL;
    int member_count = 0;
    int64_t next_value = 0;
    bool has_payload_members = false;
    bool has_explicit_numeric_value = false;

    while (parser->current.type != TOKEN_RBRACE && parser->current.type != TOKEN_EOF) {
        parser_consume(parser, TOKEN_IDENTIFIER, "Expected enum member name");
        char* member_name = safe_strdup(parser->previous.lexeme);

        Type** payload_types = NULL;
        int payload_count = 0;
        int64_t member_value = next_value;

        if (parser_match(parser, TOKEN_LPAREN)) {
            has_payload_members = true;

            if (parser_match(parser, TOKEN_RPAREN)) {
                parser_error(parser, "Enum payload variant must include at least one payload type");
            } else {
                do {
                    Type* payload_type = parse_type(parser);
                    payload_count++;
                    payload_types = (Type**)safe_realloc(payload_types, (size_t)payload_count * sizeof(Type*));
                    payload_types[payload_count - 1] = payload_type;
                } while (parser_match(parser, TOKEN_COMMA));

                parser_consume(parser, TOKEN_RPAREN, "Expected ')' after enum payload types");
            }
        }

        if (parser_match(parser, TOKEN_ASSIGN)) {
            bool negative = parser_match(parser, TOKEN_MINUS);
            parser_consume(parser, TOKEN_NUMBER_INT, "Expected integer value in enum member assignment");
            member_value = parser->previous.as_int;
            if (negative) member_value = -member_value;
            has_explicit_numeric_value = true;
            if (payload_count > 0) {
                parser_error(parser, "Enum payload variants cannot use explicit integer assignments");
            }
        }
        next_value = member_value + 1;

        member_count++;
        member_names = (char**)safe_realloc(member_names, (size_t)member_count * sizeof(char*));
        member_values = (int64_t*)safe_realloc(member_values, (size_t)member_count * sizeof(int64_t));
        member_payload_types = (Type***)safe_realloc(member_payload_types, (size_t)member_count * sizeof(Type**));
        member_payload_counts = (int*)safe_realloc(member_payload_counts, (size_t)member_count * sizeof(int));
        member_names[member_count - 1] = member_name;
        member_values[member_count - 1] = member_value;
        member_payload_types[member_count - 1] = payload_types;
        member_payload_counts[member_count - 1] = payload_count;

        if (parser_match(parser, TOKEN_COMMA) || parser_match(parser, TOKEN_SEMICOLON)) {
            continue;
        }

        if (parser->current.type != TOKEN_RBRACE) {
            parser_error(parser, "Expected ',' or ';' after enum member");
            break;
        }
    }

    parser->active_type_params = old_active_type_params;
    parser->active_type_param_count = old_active_type_param_count;

    parser_consume(parser, TOKEN_RBRACE, "Expected '}' after enum members");
    parser_match(parser, TOKEN_SEMICOLON);

    if (has_payload_members && has_explicit_numeric_value) {
        parser_error(parser, "Cannot mix payload variants with explicit numeric enum assignments");
    }

    Stmt* stmt = stmt_create_enum_decl(name,
                                       type_params,
                                       type_param_count,
                                       member_names,
                                       member_values,
                                       member_payload_types,
                                       member_payload_counts,
                                       member_count,
                                       has_payload_members,
                                       parser->lexer.file,
                                       parser->previous.line,
                                       parser->previous.column);
    free(name);
    return stmt;
}

static void parser_apply_visibility_modifier(Parser* parser, Stmt* stmt, bool has_modifier, bool is_public) {
    if (!has_modifier || !stmt) return;

    switch (stmt->kind) {
        case STMT_VAR_DECL:
        case STMT_VAR_TUPLE_DECL:
        case STMT_FUNC_DECL:
        case STMT_RECORD_DECL:
        case STMT_INTERFACE_DECL:
        case STMT_TYPE_ALIAS:
        case STMT_ENUM_DECL:
            stmt->is_public = is_public;
            return;
        default:
            parser_error(parser, "Visibility modifier can only be used with var/const/type/interface/func/record/enum declarations");
            return;
    }
}

static Stmt* parse_declaration(Parser* parser) {
    bool has_visibility_modifier = false;
    bool declaration_is_public = true;

    if (parser_match(parser, TOKEN_KEYWORD_PUBLIC)) {
        has_visibility_modifier = true;
        declaration_is_public = true;
    } else if (parser_match(parser, TOKEN_KEYWORD_PRIVATE)) {
        has_visibility_modifier = true;
        declaration_is_public = false;
    }

    if (parser_match(parser, TOKEN_KEYWORD_VAR)) {
        Stmt* stmt = parse_var_declaration(parser, true);
        parser_apply_visibility_modifier(parser, stmt, has_visibility_modifier, declaration_is_public);
        return stmt;
    }
    if (parser_match(parser, TOKEN_KEYWORD_CONST)) {
        Stmt* stmt = parse_var_declaration(parser, false);
        parser_apply_visibility_modifier(parser, stmt, has_visibility_modifier, declaration_is_public);
        return stmt;
    }
    if (parser_match(parser, TOKEN_KEYWORD_TYPE)) {
        Stmt* stmt = parse_type_alias_declaration(parser);
        parser_apply_visibility_modifier(parser, stmt, has_visibility_modifier, declaration_is_public);
        return stmt;
    }
    if (parser_match(parser, TOKEN_KEYWORD_INTERFACE)) {
        Stmt* stmt = parse_interface_declaration(parser);
        parser_apply_visibility_modifier(parser, stmt, has_visibility_modifier, declaration_is_public);
        return stmt;
    }
    if (parser_match(parser, TOKEN_KEYWORD_IMPL)) {
        if (has_visibility_modifier) {
            parser_error(parser, "Visibility modifier cannot be used with impl declarations");
        }
        return parse_impl_declaration(parser);
    }
    if (parser_match(parser, TOKEN_KEYWORD_ASYNC)) {
        if (!parser_match(parser, TOKEN_KEYWORD_FUNC)) {
            parser_error(parser, "Expected 'func' after 'async'");
            return NULL;
        }
        Stmt* stmt = parse_func_declaration(parser, true);
        parser_apply_visibility_modifier(parser, stmt, has_visibility_modifier, declaration_is_public);
        return stmt;
    }
    if (parser_match(parser, TOKEN_KEYWORD_FUNC)) {
        Stmt* stmt = parse_func_declaration(parser, false);
        parser_apply_visibility_modifier(parser, stmt, has_visibility_modifier, declaration_is_public);
        return stmt;
    }
    if (parser_match(parser, TOKEN_KEYWORD_RECORD)) {
        Stmt* stmt = parse_record_declaration(parser);
        parser_apply_visibility_modifier(parser, stmt, has_visibility_modifier, declaration_is_public);
        return stmt;
    }
    if (parser_match(parser, TOKEN_KEYWORD_ENUM)) {
        Stmt* stmt = parse_enum_declaration(parser);
        parser_apply_visibility_modifier(parser, stmt, has_visibility_modifier, declaration_is_public);
        return stmt;
    }
    if (parser_match(parser, TOKEN_KEYWORD_IMPORT)) {
        Stmt* stmt = parse_import_statement(parser);
        parser_apply_visibility_modifier(parser, stmt, has_visibility_modifier, declaration_is_public);
        return stmt;
    }

    if (has_visibility_modifier) {
        parser_error(parser, "Visibility modifier must be followed by a declaration");
    }
    return parse_statement(parser);
}

static Stmt* parse_statement(Parser* parser) {
    if (parser_match(parser, TOKEN_LBRACE)) {
        return parse_block_statement(parser);
    }
    if (parser_match(parser, TOKEN_KEYWORD_IF)) {
        return parse_if_statement(parser);
    }
    if (parser_match(parser, TOKEN_KEYWORD_MATCH)) {
        return parse_match_statement(parser);
    }
    if (parser_match(parser, TOKEN_KEYWORD_SWITCH)) {
        return parse_switch_statement(parser);
    }
    if (parser_match(parser, TOKEN_KEYWORD_WHILE)) {
        return parse_while_statement(parser);
    }
    if (parser_match(parser, TOKEN_KEYWORD_FOREACH)) {
        return parse_foreach_statement(parser);
    }
    if (parser_match(parser, TOKEN_KEYWORD_BREAK)) {
        parser_consume(parser, TOKEN_SEMICOLON, "Expected ';' after break");
        return stmt_create_break(parser->lexer.file, parser->previous.line, parser->previous.column);
    }
    if (parser_match(parser, TOKEN_KEYWORD_CONTINUE)) {
        parser_consume(parser, TOKEN_SEMICOLON, "Expected ';' after continue");
        return stmt_create_continue(parser->lexer.file, parser->previous.line, parser->previous.column);
    }
    if (parser_match(parser, TOKEN_KEYWORD_DEFER)) {
        Expr* expr = parse_expression(parser);
        parser_consume(parser, TOKEN_SEMICOLON, "Expected ';' after defer");
        return stmt_create_defer(expr, parser->lexer.file, parser->previous.line, parser->previous.column);
    }
    if (parser_match(parser, TOKEN_KEYWORD_RETURN)) {
        Expr* value = NULL;
        if (parser->current.type != TOKEN_SEMICOLON) {
            value = parse_expression(parser);
        }
        parser_consume(parser, TOKEN_SEMICOLON, "Expected ';' after return");
        return stmt_create_return(value, parser->lexer.file, parser->previous.line, parser->previous.column);
    }
    return parse_expression_statement(parser);
}

static ParseResult parser_parse_internal(const char* source, const char* file, bool report_diagnostics) {
    Parser parser;
    lexer_init(&parser.lexer, source, file);
    parser.current = lexer_next_token(&parser.lexer);
    parser.previous.type = TOKEN_ERROR;
    parser.previous.lexeme = NULL;
    parser.had_error = false;
    parser.panic_mode = false;
    parser.report_diagnostics = report_diagnostics;
    parser.first_error_message = NULL;
    parser.error_line = 0;
    parser.error_column = 0;
    parser.depth = 0;
    parser.max_depth = PARSER_MAX_DEPTH;
    parser.active_type_params = NULL;
    parser.active_type_param_count = 0;
    parser.synthetic_counter = 0;
    parser.current_function_is_async = false;
    
    Program* program = program_create(file);
    
    while (parser.current.type != TOKEN_EOF) {
        Stmt* stmt = parse_declaration(&parser);
        if (stmt) {
            program_add_stmt(program, stmt);
        }
        if (parser.panic_mode) {
            parser_synchronize(&parser, false);
        }
    }
    
    ParseResult result;
    result.program = program;
    if (parser.had_error) {
        const char* parse_message = parser.first_error_message ? parser.first_error_message : "Syntax error in source";
        result.error = error_create(ERROR_SYNTAX, parse_message, file, parser.error_line, parser.error_column);
    } else {
        result.error = NULL;
    }

    token_free(&parser.current);
    token_free(&parser.previous);
    if (parser.lexer.file) free(parser.lexer.file);
    parser.lexer.file = NULL;
    if (parser.first_error_message) free(parser.first_error_message);
    parser.first_error_message = NULL;

    return result;
}

ParseResult parser_parse(const char* source, const char* file) {
    return parser_parse_internal(source, file, true);
}

ParseResult parser_parse_quiet(const char* source, const char* file) {
    return parser_parse_internal(source, file, false);
}

static void parser_type_root_list_add(ParserTypeRootList* list, Type* type) {
    if (!list || !type) return;

    for (int i = 0; i < list->count; i++) {
        if (list->items[i] == type) {
            return;
        }
    }

    if (list->count >= list->capacity) {
        int new_capacity = list->capacity > 0 ? list->capacity * 2 : 32;
        list->items = (Type**)safe_realloc(list->items, (size_t)new_capacity * sizeof(Type*));
        list->capacity = new_capacity;
    }

    list->items[list->count++] = type;
}

static void parser_collect_expr_type_roots(ParserTypeRootList* list, Expr* expr) {
    if (!list || !expr) return;

    parser_type_root_list_add(list, expr->type);

    switch (expr->kind) {
        case EXPR_LITERAL:
        case EXPR_IDENTIFIER:
        case EXPR_NIL:
            break;
        case EXPR_BINARY:
            parser_collect_expr_type_roots(list, expr->binary.left);
            parser_collect_expr_type_roots(list, expr->binary.right);
            break;
        case EXPR_UNARY:
            parser_collect_expr_type_roots(list, expr->unary.operand);
            break;
        case EXPR_CALL:
            parser_collect_expr_type_roots(list, expr->call.callee);
            for (int i = 0; i < expr->call.arg_count; i++) {
                parser_collect_expr_type_roots(list, expr->call.args[i]);
            }
            for (int i = 0; i < expr->call.type_arg_count; i++) {
                parser_type_root_list_add(list, expr->call.type_args ? expr->call.type_args[i] : NULL);
            }
            break;
        case EXPR_FUNC_LITERAL:
            parser_type_root_list_add(list, expr->func_literal.return_type);
            for (int i = 0; i < expr->func_literal.param_count; i++) {
                parser_type_root_list_add(list, expr->func_literal.param_types ? expr->func_literal.param_types[i] : NULL);
            }
            parser_collect_stmt_type_roots(list, expr->func_literal.body);
            break;
        case EXPR_INDEX:
            parser_collect_expr_type_roots(list, expr->index.array);
            parser_collect_expr_type_roots(list, expr->index.index);
            break;
        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->array_literal.element_count; i++) {
                parser_collect_expr_type_roots(list, expr->array_literal.elements[i]);
            }
            break;
        case EXPR_CAST:
            parser_collect_expr_type_roots(list, expr->cast.value);
            parser_type_root_list_add(list, expr->cast.target_type);
            break;
        case EXPR_TRY:
            parser_collect_expr_type_roots(list, expr->try_expr.expr);
            break;
        case EXPR_AWAIT:
            parser_collect_expr_type_roots(list, expr->await_expr.expr);
            break;
        case EXPR_TYPE_TEST:
            parser_collect_expr_type_roots(list, expr->type_test.value);
            parser_type_root_list_add(list, expr->type_test.target_type);
            break;
        case EXPR_IF:
            parser_collect_expr_type_roots(list, expr->if_expr.condition);
            parser_collect_expr_type_roots(list, expr->if_expr.then_expr);
            parser_collect_expr_type_roots(list, expr->if_expr.else_expr);
            break;
        case EXPR_MATCH:
            parser_collect_expr_type_roots(list, expr->match_expr.subject);
            for (int i = 0; i < expr->match_expr.arm_count; i++) {
                parser_collect_expr_type_roots(list, expr->match_expr.patterns ? expr->match_expr.patterns[i] : NULL);
                parser_collect_expr_type_roots(list, expr->match_expr.guards ? expr->match_expr.guards[i] : NULL);
                parser_collect_expr_type_roots(list, expr->match_expr.values ? expr->match_expr.values[i] : NULL);
            }
            parser_collect_expr_type_roots(list, expr->match_expr.else_expr);
            break;
        case EXPR_BLOCK:
            for (int i = 0; i < expr->block_expr.stmt_count; i++) {
                parser_collect_stmt_type_roots(list, expr->block_expr.statements[i]);
            }
            parser_collect_expr_type_roots(list, expr->block_expr.value);
            break;
        case EXPR_RECORD_LITERAL:
            parser_type_root_list_add(list, expr->record_literal.record_type);
            parser_type_root_list_add(list, expr->record_literal.pattern_type);
            for (int i = 0; i < expr->record_literal.field_count; i++) {
                parser_collect_expr_type_roots(list, expr->record_literal.field_values[i]);
            }
            break;
        case EXPR_FIELD_ACCESS:
            parser_collect_expr_type_roots(list, expr->field_access.object);
            break;
        case EXPR_TUPLE_LITERAL:
            for (int i = 0; i < expr->tuple_literal.element_count; i++) {
                parser_collect_expr_type_roots(list, expr->tuple_literal.elements[i]);
            }
            break;
        case EXPR_TUPLE_ACCESS:
            parser_collect_expr_type_roots(list, expr->tuple_access.tuple);
            break;
        case EXPR_MAP_LITERAL:
            parser_type_root_list_add(list, expr->map_literal.map_type);
            for (int i = 0; i < expr->map_literal.entry_count; i++) {
                parser_collect_expr_type_roots(list, expr->map_literal.keys[i]);
                parser_collect_expr_type_roots(list, expr->map_literal.values[i]);
            }
            break;
        case EXPR_SET_LITERAL:
            parser_type_root_list_add(list, expr->set_literal.set_type);
            for (int i = 0; i < expr->set_literal.element_count; i++) {
                parser_collect_expr_type_roots(list, expr->set_literal.elements[i]);
            }
            break;
        case EXPR_ARRAY:
            break;
    }
}

static void parser_collect_stmt_type_roots(ParserTypeRootList* list, Stmt* stmt) {
    if (!list || !stmt) return;

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            parser_type_root_list_add(list, stmt->var_decl.type_annotation);
            parser_collect_expr_type_roots(list, stmt->var_decl.initializer);
            break;
        case STMT_VAR_TUPLE_DECL:
            parser_type_root_list_add(list, stmt->var_tuple_decl.type_annotation);
            parser_collect_expr_type_roots(list, stmt->var_tuple_decl.initializer);
            break;
        case STMT_EXPR:
            parser_collect_expr_type_roots(list, stmt->expr_stmt);
            break;
        case STMT_ASSIGN:
            parser_collect_expr_type_roots(list, stmt->assign.value);
            break;
        case STMT_ASSIGN_INDEX:
            parser_collect_expr_type_roots(list, stmt->assign_index.target);
            parser_collect_expr_type_roots(list, stmt->assign_index.index);
            parser_collect_expr_type_roots(list, stmt->assign_index.value);
            break;
        case STMT_ASSIGN_FIELD:
            parser_collect_expr_type_roots(list, stmt->assign_field.object);
            parser_collect_expr_type_roots(list, stmt->assign_field.value);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < stmt->block.stmt_count; i++) {
                parser_collect_stmt_type_roots(list, stmt->block.statements[i]);
            }
            break;
        case STMT_IF:
            parser_collect_expr_type_roots(list, stmt->if_stmt.condition);
            parser_collect_stmt_type_roots(list, stmt->if_stmt.then_branch);
            parser_collect_stmt_type_roots(list, stmt->if_stmt.else_branch);
            break;
        case STMT_MATCH:
            parser_collect_expr_type_roots(list, stmt->match_stmt.subject);
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                parser_collect_expr_type_roots(list, stmt->match_stmt.patterns ? stmt->match_stmt.patterns[i] : NULL);
                parser_collect_expr_type_roots(list, stmt->match_stmt.guards ? stmt->match_stmt.guards[i] : NULL);
                parser_collect_stmt_type_roots(list, stmt->match_stmt.bodies ? stmt->match_stmt.bodies[i] : NULL);
            }
            parser_collect_stmt_type_roots(list, stmt->match_stmt.else_branch);
            break;
        case STMT_WHILE:
            parser_collect_expr_type_roots(list, stmt->while_stmt.condition);
            parser_collect_stmt_type_roots(list, stmt->while_stmt.body);
            break;
        case STMT_FOREACH:
            parser_collect_expr_type_roots(list, stmt->foreach.iterable);
            parser_collect_stmt_type_roots(list, stmt->foreach.body);
            break;
        case STMT_FOR_RANGE:
            parser_collect_expr_type_roots(list, stmt->for_range.start);
            parser_collect_expr_type_roots(list, stmt->for_range.end);
            parser_collect_stmt_type_roots(list, stmt->for_range.body);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
        case STMT_IMPORT:
        case STMT_IMPL_DECL:
            break;
        case STMT_RETURN:
            parser_collect_expr_type_roots(list, stmt->return_value);
            break;
        case STMT_DEFER:
            parser_collect_expr_type_roots(list, stmt->defer_expr);
            break;
        case STMT_FUNC_DECL:
            parser_type_root_list_add(list, stmt->func_decl.return_type);
            for (int i = 0; i < stmt->func_decl.type_param_count; i++) {
                parser_type_root_list_add(list,
                                          stmt->func_decl.type_param_constraints
                                              ? stmt->func_decl.type_param_constraints[i]
                                              : NULL);
            }
            for (int i = 0; i < stmt->func_decl.param_count; i++) {
                parser_type_root_list_add(list, stmt->func_decl.param_types ? stmt->func_decl.param_types[i] : NULL);
            }
            parser_collect_stmt_type_roots(list, stmt->func_decl.body);
            break;
        case STMT_RECORD_DECL:
            for (int i = 0; i < stmt->record_decl.field_count; i++) {
                parser_type_root_list_add(list, stmt->record_decl.field_types ? stmt->record_decl.field_types[i] : NULL);
            }
            break;
        case STMT_INTERFACE_DECL:
            for (int i = 0; i < stmt->interface_decl.method_count; i++) {
                parser_type_root_list_add(list, stmt->interface_decl.method_types ? stmt->interface_decl.method_types[i] : NULL);
            }
            break;
        case STMT_TYPE_ALIAS:
            parser_type_root_list_add(list, stmt->type_alias.target_type);
            break;
        case STMT_ENUM_DECL:
            for (int i = 0; i < stmt->enum_decl.member_count; i++) {
                int payload_count = stmt->enum_decl.member_payload_counts
                    ? stmt->enum_decl.member_payload_counts[i]
                    : 0;
                for (int j = 0; j < payload_count; j++) {
                    parser_type_root_list_add(list,
                                              (stmt->enum_decl.member_payload_types &&
                                               stmt->enum_decl.member_payload_types[i])
                                                  ? stmt->enum_decl.member_payload_types[i][j]
                                                  : NULL);
                }
            }
            break;
    }
}

static void parser_collect_program_type_roots(ParserTypeRootList* list, Program* program) {
    if (!list || !program) return;

    for (int i = 0; i < program->stmt_count; i++) {
        parser_collect_stmt_type_roots(list, program->statements[i]);
    }
}

static void parser_release_parse_only_expr_type_arrays(Expr* expr) {
    if (!expr) return;

    switch (expr->kind) {
        case EXPR_LITERAL:
        case EXPR_IDENTIFIER:
        case EXPR_NIL:
        case EXPR_ARRAY:
            break;
        case EXPR_BINARY:
            parser_release_parse_only_expr_type_arrays(expr->binary.left);
            parser_release_parse_only_expr_type_arrays(expr->binary.right);
            break;
        case EXPR_UNARY:
            parser_release_parse_only_expr_type_arrays(expr->unary.operand);
            break;
        case EXPR_CALL:
            parser_release_parse_only_expr_type_arrays(expr->call.callee);
            for (int i = 0; i < expr->call.arg_count; i++) {
                parser_release_parse_only_expr_type_arrays(expr->call.args[i]);
            }
            break;
        case EXPR_FUNC_LITERAL:
            parser_release_parse_only_stmt_type_arrays(expr->func_literal.body);
            break;
        case EXPR_INDEX:
            parser_release_parse_only_expr_type_arrays(expr->index.array);
            parser_release_parse_only_expr_type_arrays(expr->index.index);
            break;
        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->array_literal.element_count; i++) {
                parser_release_parse_only_expr_type_arrays(expr->array_literal.elements[i]);
            }
            break;
        case EXPR_CAST:
            parser_release_parse_only_expr_type_arrays(expr->cast.value);
            break;
        case EXPR_TRY:
            parser_release_parse_only_expr_type_arrays(expr->try_expr.expr);
            break;
        case EXPR_AWAIT:
            parser_release_parse_only_expr_type_arrays(expr->await_expr.expr);
            break;
        case EXPR_TYPE_TEST:
            parser_release_parse_only_expr_type_arrays(expr->type_test.value);
            break;
        case EXPR_IF:
            parser_release_parse_only_expr_type_arrays(expr->if_expr.condition);
            parser_release_parse_only_expr_type_arrays(expr->if_expr.then_expr);
            parser_release_parse_only_expr_type_arrays(expr->if_expr.else_expr);
            break;
        case EXPR_MATCH:
            parser_release_parse_only_expr_type_arrays(expr->match_expr.subject);
            for (int i = 0; i < expr->match_expr.arm_count; i++) {
                parser_release_parse_only_expr_type_arrays(expr->match_expr.patterns ? expr->match_expr.patterns[i] : NULL);
                parser_release_parse_only_expr_type_arrays(expr->match_expr.guards ? expr->match_expr.guards[i] : NULL);
                parser_release_parse_only_expr_type_arrays(expr->match_expr.values ? expr->match_expr.values[i] : NULL);
            }
            parser_release_parse_only_expr_type_arrays(expr->match_expr.else_expr);
            break;
        case EXPR_BLOCK:
            for (int i = 0; i < expr->block_expr.stmt_count; i++) {
                parser_release_parse_only_stmt_type_arrays(expr->block_expr.statements[i]);
            }
            parser_release_parse_only_expr_type_arrays(expr->block_expr.value);
            break;
        case EXPR_RECORD_LITERAL:
            for (int i = 0; i < expr->record_literal.field_count; i++) {
                parser_release_parse_only_expr_type_arrays(expr->record_literal.field_values[i]);
            }
            break;
        case EXPR_FIELD_ACCESS:
            parser_release_parse_only_expr_type_arrays(expr->field_access.object);
            break;
        case EXPR_TUPLE_LITERAL:
            for (int i = 0; i < expr->tuple_literal.element_count; i++) {
                parser_release_parse_only_expr_type_arrays(expr->tuple_literal.elements[i]);
            }
            break;
        case EXPR_TUPLE_ACCESS:
            parser_release_parse_only_expr_type_arrays(expr->tuple_access.tuple);
            break;
        case EXPR_MAP_LITERAL:
            for (int i = 0; i < expr->map_literal.entry_count; i++) {
                parser_release_parse_only_expr_type_arrays(expr->map_literal.keys[i]);
                parser_release_parse_only_expr_type_arrays(expr->map_literal.values[i]);
            }
            break;
        case EXPR_SET_LITERAL:
            for (int i = 0; i < expr->set_literal.element_count; i++) {
                parser_release_parse_only_expr_type_arrays(expr->set_literal.elements[i]);
            }
            break;
    }
}

static void parser_release_parse_only_stmt_type_arrays(Stmt* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            parser_release_parse_only_expr_type_arrays(stmt->var_decl.initializer);
            break;
        case STMT_VAR_TUPLE_DECL:
            parser_release_parse_only_expr_type_arrays(stmt->var_tuple_decl.initializer);
            break;
        case STMT_EXPR:
            parser_release_parse_only_expr_type_arrays(stmt->expr_stmt);
            break;
        case STMT_ASSIGN:
            parser_release_parse_only_expr_type_arrays(stmt->assign.value);
            break;
        case STMT_ASSIGN_INDEX:
            parser_release_parse_only_expr_type_arrays(stmt->assign_index.target);
            parser_release_parse_only_expr_type_arrays(stmt->assign_index.index);
            parser_release_parse_only_expr_type_arrays(stmt->assign_index.value);
            break;
        case STMT_ASSIGN_FIELD:
            parser_release_parse_only_expr_type_arrays(stmt->assign_field.object);
            parser_release_parse_only_expr_type_arrays(stmt->assign_field.value);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < stmt->block.stmt_count; i++) {
                parser_release_parse_only_stmt_type_arrays(stmt->block.statements[i]);
            }
            break;
        case STMT_IF:
            parser_release_parse_only_expr_type_arrays(stmt->if_stmt.condition);
            parser_release_parse_only_stmt_type_arrays(stmt->if_stmt.then_branch);
            parser_release_parse_only_stmt_type_arrays(stmt->if_stmt.else_branch);
            break;
        case STMT_MATCH:
            parser_release_parse_only_expr_type_arrays(stmt->match_stmt.subject);
            for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
                parser_release_parse_only_expr_type_arrays(stmt->match_stmt.patterns ? stmt->match_stmt.patterns[i] : NULL);
                parser_release_parse_only_expr_type_arrays(stmt->match_stmt.guards ? stmt->match_stmt.guards[i] : NULL);
                parser_release_parse_only_stmt_type_arrays(stmt->match_stmt.bodies ? stmt->match_stmt.bodies[i] : NULL);
            }
            parser_release_parse_only_stmt_type_arrays(stmt->match_stmt.else_branch);
            break;
        case STMT_WHILE:
            parser_release_parse_only_expr_type_arrays(stmt->while_stmt.condition);
            parser_release_parse_only_stmt_type_arrays(stmt->while_stmt.body);
            break;
        case STMT_FOREACH:
            parser_release_parse_only_expr_type_arrays(stmt->foreach.iterable);
            parser_release_parse_only_stmt_type_arrays(stmt->foreach.body);
            break;
        case STMT_FOR_RANGE:
            parser_release_parse_only_expr_type_arrays(stmt->for_range.start);
            parser_release_parse_only_expr_type_arrays(stmt->for_range.end);
            parser_release_parse_only_stmt_type_arrays(stmt->for_range.body);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
        case STMT_IMPORT:
        case STMT_IMPL_DECL:
            break;
        case STMT_RETURN:
            parser_release_parse_only_expr_type_arrays(stmt->return_value);
            break;
        case STMT_DEFER:
            parser_release_parse_only_expr_type_arrays(stmt->defer_expr);
            break;
        case STMT_FUNC_DECL:
            parser_release_parse_only_stmt_type_arrays(stmt->func_decl.body);
            free(stmt->func_decl.param_types);
            stmt->func_decl.param_types = NULL;
            break;
        case STMT_RECORD_DECL:
            free(stmt->record_decl.field_types);
            stmt->record_decl.field_types = NULL;
            break;
        case STMT_INTERFACE_DECL:
            free(stmt->interface_decl.method_types);
            stmt->interface_decl.method_types = NULL;
            break;
        case STMT_TYPE_ALIAS:
            break;
        case STMT_ENUM_DECL:
            break;
    }
}

static void parser_release_parse_only_program_type_arrays(Program* program) {
    if (!program) return;

    for (int i = 0; i < program->stmt_count; i++) {
        parser_release_parse_only_stmt_type_arrays(program->statements[i]);
    }
}

void parser_free_result(ParseResult* result) {
    if (!result) return;
    program_free(result->program);
    error_free(result->error);
}

void parser_free_parse_only_result(ParseResult* result) {
    ParserTypeRootList type_roots = {0};

    if (!result) return;

    parser_collect_program_type_roots(&type_roots, result->program);
    parser_release_parse_only_program_type_arrays(result->program);
    program_free(result->program);
    result->program = NULL;

    for (int i = 0; i < type_roots.count; i++) {
        type_free(type_roots.items[i]);
    }
    free(type_roots.items);

    error_free(result->error);
    result->error = NULL;
}
