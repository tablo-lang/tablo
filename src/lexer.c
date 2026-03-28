#include "lexer.h"
#include "safe_alloc.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>

typedef struct {
    const char* keyword;
    TokenType type;
} KeywordRule;

#define MAX_TOKEN_LENGTH 65536

static KeywordRule keywords[] = {
    {"int", TOKEN_KEYWORD_INT},
    {"bool", TOKEN_KEYWORD_BOOL},
    {"double", TOKEN_KEYWORD_DOUBLE},
    {"bigint", TOKEN_KEYWORD_BIGINT},
    {"string", TOKEN_KEYWORD_STRING},
    {"bytes", TOKEN_KEYWORD_BYTES},
    {"array", TOKEN_KEYWORD_ARRAY},
    {"any", TOKEN_KEYWORD_ANY},
    {"nil", TOKEN_KEYWORD_NIL},
    {"var", TOKEN_KEYWORD_VAR},
    {"const", TOKEN_KEYWORD_CONST},
    {"public", TOKEN_KEYWORD_PUBLIC},
    {"private", TOKEN_KEYWORD_PRIVATE},
    {"type", TOKEN_KEYWORD_TYPE},
    {"interface", TOKEN_KEYWORD_INTERFACE},
    {"impl", TOKEN_KEYWORD_IMPL},
    {"async", TOKEN_KEYWORD_ASYNC},
    {"await", TOKEN_KEYWORD_AWAIT},
    {"func", TOKEN_KEYWORD_FUNC},
    {"if", TOKEN_KEYWORD_IF},
    {"else", TOKEN_KEYWORD_ELSE},
    {"let", TOKEN_KEYWORD_LET},
    {"while", TOKEN_KEYWORD_WHILE},
    {"foreach", TOKEN_KEYWORD_FOREACH},
    {"break", TOKEN_KEYWORD_BREAK},
    {"continue", TOKEN_KEYWORD_CONTINUE},
    {"return", TOKEN_KEYWORD_RETURN},
    {"defer", TOKEN_KEYWORD_DEFER},
    {"import", TOKEN_KEYWORD_IMPORT},
    {"record", TOKEN_KEYWORD_RECORD},
    {"enum", TOKEN_KEYWORD_ENUM},
    {"match", TOKEN_KEYWORD_MATCH},
    {"switch", TOKEN_KEYWORD_SWITCH},
    {"case", TOKEN_KEYWORD_CASE},
    {"default", TOKEN_KEYWORD_DEFAULT},
    {"map", TOKEN_KEYWORD_MAP},
    {"set", TOKEN_KEYWORD_SET},
    {"true", TOKEN_TRUE},
    {"false", TOKEN_FALSE},
    {"as", TOKEN_AS},
    {"in", TOKEN_IN},
    {NULL, TOKEN_EOF}
};

static char lexer_peek(Lexer* lexer) {
    return lexer->source[lexer->position];
}

static char lexer_advance(Lexer* lexer) {
    char c = lexer->source[lexer->position];
    lexer->position++;
    lexer->column++;
    return c;
}

static bool lexer_match(Lexer* lexer, char expected) {
    if (lexer->position >= lexer->length) return false;
    if (lexer->source[lexer->position] != expected) return false;
    lexer->position++;
    lexer->column++;
    return true;
}

static bool lexer_is_utf8_continuation(unsigned char c) {
    return (c & 0xC0u) == 0x80u;
}

static int lexer_hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool lexer_append_utf8(char* out, int capacity, int* out_len, uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        if (*out_len + 1 > capacity) return false;
        out[(*out_len)++] = (char)codepoint;
        return true;
    }
    if (codepoint <= 0x7FF) {
        if (*out_len + 2 > capacity) return false;
        out[(*out_len)++] = (char)(0xC0u | (codepoint >> 6));
        out[(*out_len)++] = (char)(0x80u | (codepoint & 0x3Fu));
        return true;
    }
    if (codepoint <= 0xFFFF) {
        if (*out_len + 3 > capacity) return false;
        out[(*out_len)++] = (char)(0xE0u | (codepoint >> 12));
        out[(*out_len)++] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[(*out_len)++] = (char)(0x80u | (codepoint & 0x3Fu));
        return true;
    }
    if (codepoint <= 0x10FFFF) {
        if (*out_len + 4 > capacity) return false;
        out[(*out_len)++] = (char)(0xF0u | (codepoint >> 18));
        out[(*out_len)++] = (char)(0x80u | ((codepoint >> 12) & 0x3Fu));
        out[(*out_len)++] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[(*out_len)++] = (char)(0x80u | (codepoint & 0x3Fu));
        return true;
    }

    return false;
}

static void lexer_skip_whitespace(Lexer* lexer) {
    while (1) {
        char c = lexer_peek(lexer);
        if (c == ' ' || c == '\t' || c == '\r') {
            lexer_advance(lexer);
        } else if (c == '\n') {
            lexer->line++;
            lexer->column = 0;
            lexer->position++;
        } else if (c == '/' && lexer->position + 1 < lexer->length && lexer->source[lexer->position + 1] == '/') {
            // Skip single-line comments
            while (lexer->position < lexer->length && lexer_peek(lexer) != '\n') {
                lexer->position++;
                lexer->column++;
            }
        } else if (c == '/' && lexer->position + 1 < lexer->length && lexer->source[lexer->position + 1] == '*') {
            // Skip multi-line comments
            lexer->position += 2;
            lexer->column += 2;
            bool terminated = false;

            while (lexer->position < lexer->length) {
                char cc = lexer_peek(lexer);
                if (cc == '\n') {
                    lexer->line++;
                    lexer->column = 0;
                    lexer->position++;
                    continue;
                }

                if (cc == '*' &&
                    lexer->position + 1 < lexer->length &&
                    lexer->source[lexer->position + 1] == '/') {
                    lexer->position += 2;
                    lexer->column += 2;
                    terminated = true;
                    break;
                }

                lexer->position++;
                lexer->column++;
            }

            if (!terminated) {
                lexer->pending_error = "Unterminated multi-line comment";
                return;
            }
        } else {
            break;
        }
    }
}

static Token lexer_make_token(TokenType type, Lexer* lexer, int start_pos) {
    Token token;
    token.type = type;
    token.line = lexer->line;
    token.column = lexer->column - (lexer->position - start_pos);
    
    int token_len = lexer->position - start_pos;
    if (token_len > MAX_TOKEN_LENGTH) {
        token.type = TOKEN_ERROR;
        token.lexeme = safe_strdup("Token too long");
        return token;
    }
    
    token.lexeme = (char*)safe_malloc(token_len + 1);
    memcpy(token.lexeme, &lexer->source[start_pos], token_len);
    token.lexeme[token_len] = '\0';
    
    if (type == TOKEN_NUMBER_INT) {
        char* endptr;
        errno = 0;
        token.as_int = strtoll(token.lexeme, &endptr, 10);
        if (errno == ERANGE || *endptr != '\0') {
            token.type = TOKEN_ERROR;
            free(token.lexeme);
            token.lexeme = safe_strdup("Integer overflow or invalid number");
            return token;
        }
    } else if (type == TOKEN_NUMBER_DOUBLE) {
        char* endptr;
        errno = 0;
        token.as_double = strtod(token.lexeme, &endptr);
        if (errno == ERANGE || *endptr != '\0') {
            token.type = TOKEN_ERROR;
            free(token.lexeme);
            token.lexeme = safe_strdup("Double overflow or invalid number");
            return token;
        }
    } else if (type == TOKEN_STRING) {
        int raw_len = lexer->position - start_pos;
        if (raw_len < 2) {
            token.type = TOKEN_ERROR;
            free(token.lexeme);
            token.lexeme = safe_strdup("Invalid string token");
            return token;
        }
        int content_len = raw_len - 2;
        const char* raw = &lexer->source[start_pos + 1];
        // Decode a minimal set of C-style escapes so JSON strings are usable.
        // Unknown escapes keep the escaped character (e.g. "\q" => "q").
        char* out = (char*)safe_malloc((size_t)content_len + 1);
        int out_len = 0;
        for (int i = 0; i < content_len; ) {
            char c = raw[i];
            if (c == '\\' && i + 1 < content_len) {
                char esc = raw[i + 1];
                switch (esc) {
                    case 'n': out[out_len++] = '\n'; break;
                    case 'r': out[out_len++] = '\r'; break;
                    case 't': out[out_len++] = '\t'; break;
                    case 'b': out[out_len++] = '\b'; break;
                    case 'f': out[out_len++] = '\f'; break;
                    case '\\': out[out_len++] = '\\'; break;
                    case '"': out[out_len++] = '"'; break;
                    case 'u': {
                        if (i + 5 >= content_len) {
                            token.type = TOKEN_ERROR;
                            free(token.lexeme);
                            token.lexeme = safe_strdup("Invalid Unicode escape sequence");
                            free(out);
                            return token;
                        }

                        int h0 = lexer_hex_value(raw[i + 2]);
                        int h1 = lexer_hex_value(raw[i + 3]);
                        int h2 = lexer_hex_value(raw[i + 4]);
                        int h3 = lexer_hex_value(raw[i + 5]);
                        if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
                            token.type = TOKEN_ERROR;
                            free(token.lexeme);
                            token.lexeme = safe_strdup("Invalid Unicode escape sequence");
                            free(out);
                            return token;
                        }

                        uint32_t codepoint = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                        if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
                            token.type = TOKEN_ERROR;
                            free(token.lexeme);
                            token.lexeme = safe_strdup("Invalid Unicode surrogate in string escape");
                            free(out);
                            return token;
                        }

                        if (!lexer_append_utf8(out, content_len, &out_len, codepoint)) {
                            token.type = TOKEN_ERROR;
                            free(token.lexeme);
                            token.lexeme = safe_strdup("Failed to decode Unicode escape");
                            free(out);
                            return token;
                        }

                        i += 6;
                        continue;
                    }
                    default:
                        out[out_len++] = esc;
                        break;
                }
                i += 2;
            } else {
                out[out_len++] = c;
                i++;
            }
        }
        out[out_len] = '\0';
        token.as_string = out;
    }
    
    return token;
}

static Token lexer_error_token(Lexer* lexer, const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.line = lexer->line;
    token.column = lexer->column;
    token.lexeme = safe_strdup(message);
    return token;
}

static Token lexer_string(Lexer* lexer, int start_pos) {
    while (lexer->position < lexer->length) {
        char c = lexer_peek(lexer);
        if (c == '"') {
            lexer_advance(lexer);
            return lexer_make_token(TOKEN_STRING, lexer, start_pos);
        }
        if (c == '\\') {
            // Skip escaped character so \" doesn't terminate the string.
            lexer_advance(lexer);
            if (lexer->position >= lexer->length) {
                return lexer_error_token(lexer, "Unterminated string");
            }
            // Treat escaped newlines as consuming a newline in the source.
            if (lexer_peek(lexer) == '\n') {
                lexer->line++;
                lexer->column = 0;
            }
            lexer_advance(lexer);
            continue;
        }
        if (c == '\n') {
            lexer->line++;
            lexer->column = 0;
        }
        lexer_advance(lexer);
    }

    return lexer_error_token(lexer, "Unterminated string");
}

static bool lexer_is_digit(char c) {
    return isdigit((unsigned char)c) != 0;
}

static bool lexer_is_hex_digit(char c) {
    return isxdigit((unsigned char)c) != 0;
}

static bool lexer_is_alpha(char c) {
    unsigned char uc = (unsigned char)c;
    if (isalpha(uc) != 0) return true;
    return uc >= 0x80u && !lexer_is_utf8_continuation(uc);
}

static bool lexer_is_alnum(char c) {
    unsigned char uc = (unsigned char)c;
    if (isalnum(uc) != 0) return true;
    return uc >= 0x80u;
}

static Token lexer_number(Lexer* lexer, int start_pos) {
    bool is_hex = false;
    if (lexer_peek(lexer) == '0' &&
        lexer->position + 1 < lexer->length &&
        (lexer->source[lexer->position + 1] == 'x' || lexer->source[lexer->position + 1] == 'X')) {
        is_hex = true;
        lexer_advance(lexer); // 0
        lexer_advance(lexer); // x
        if (!lexer_is_hex_digit(lexer_peek(lexer))) {
            return lexer_error_token(lexer, "Invalid hexadecimal literal");
        }
        while (lexer_is_hex_digit(lexer_peek(lexer))) {
            lexer_advance(lexer);
        }
    } else {
        while (lexer_is_digit(lexer_peek(lexer))) {
            lexer_advance(lexer);
        }

        if (lexer_peek(lexer) == '.' && lexer_is_digit(lexer->source[lexer->position + 1])) {
            lexer_advance(lexer);
            while (lexer_is_digit(lexer_peek(lexer))) {
                lexer_advance(lexer);
            }
            return lexer_make_token(TOKEN_NUMBER_DOUBLE, lexer, start_pos);
        }
    }

    bool force_bigint = false;
    if (lexer_peek(lexer) == 'n' || lexer_peek(lexer) == 'N') {
        force_bigint = true;
        lexer_advance(lexer);
    }

    int end_pos = lexer->position - (force_bigint ? 1 : 0);
    int token_len = end_pos - start_pos;
    if (token_len > MAX_TOKEN_LENGTH) {
        return lexer_error_token(lexer, "Token too long");
    }

    Token token;
    token.type = TOKEN_NUMBER_INT;
    token.line = lexer->line;
    token.column = lexer->column - (lexer->position - start_pos);
    token.lexeme = (char*)safe_malloc(token_len + 1);
    memcpy(token.lexeme, &lexer->source[start_pos], token_len);
    token.lexeme[token_len] = '\0';

    if (force_bigint) {
        token.type = TOKEN_NUMBER_BIGINT;
        token.as_string = safe_strdup(token.lexeme);
        return token;
    }

    char* endptr;
    errno = 0;
    token.as_int = strtoll(token.lexeme, &endptr, is_hex ? 16 : 10);
    if (errno == ERANGE || *endptr != '\0') {
        token.type = TOKEN_NUMBER_BIGINT;
        token.as_string = safe_strdup(token.lexeme);
    }

    return token;
}

static Token lexer_identifier(Lexer* lexer, int start_pos) {
    while (lexer_is_alnum(lexer_peek(lexer)) || lexer_peek(lexer) == '_') {
        lexer_advance(lexer);
    }
    
    char* lexeme = (char*)safe_malloc(lexer->position - start_pos + 1);
    memcpy(lexeme, &lexer->source[start_pos], lexer->position - start_pos);
    lexeme[lexer->position - start_pos] = '\0';
    
    for (int i = 0; keywords[i].keyword != NULL; i++) {
        if (strcmp(lexeme, keywords[i].keyword) == 0) {
            free(lexeme);
            return lexer_make_token(keywords[i].type, lexer, start_pos);
        }
    }
    
    free(lexeme);
    return lexer_make_token(TOKEN_IDENTIFIER, lexer, start_pos);
}

void lexer_init(Lexer* lexer, const char* source, const char* file) {
    lexer->source = source;
    lexer->file = file ? safe_strdup(file) : NULL;
    lexer->pending_error = NULL;
    size_t len = strlen(source);
    if (len > (size_t)INT_MAX) {
        len = (size_t)INT_MAX;
    }
    lexer->length = (int)len;
    lexer->position = 0;
    lexer->line = 1;
    lexer->column = 1;
}

Token lexer_next_token(Lexer* lexer) {
    lexer_skip_whitespace(lexer);

    if (lexer->pending_error) {
        const char* message = lexer->pending_error;
        lexer->pending_error = NULL;
        return lexer_error_token(lexer, message);
    }
    
    int start_pos = lexer->position;
    char c = lexer_advance(lexer);
    
    if (c == '\0') {
        return lexer_make_token(TOKEN_EOF, lexer, start_pos);
    }
    
    switch (c) {
        case '(': return lexer_make_token(TOKEN_LPAREN, lexer, start_pos);
        case ')': return lexer_make_token(TOKEN_RPAREN, lexer, start_pos);
        case '{': return lexer_make_token(TOKEN_LBRACE, lexer, start_pos);
        case '}': return lexer_make_token(TOKEN_RBRACE, lexer, start_pos);
        case '[': return lexer_make_token(TOKEN_LBRACKET, lexer, start_pos);
        case ']': return lexer_make_token(TOKEN_RBRACKET, lexer, start_pos);
        case ';': return lexer_make_token(TOKEN_SEMICOLON, lexer, start_pos);
        case ',': return lexer_make_token(TOKEN_COMMA, lexer, start_pos);
        case '.':
            if (lexer_match(lexer, '.')) {
                return lexer_make_token(TOKEN_DOT_DOT, lexer, start_pos);
            }
            return lexer_make_token(TOKEN_DOT, lexer, start_pos);
        case '?': return lexer_make_token(TOKEN_QUESTION, lexer, start_pos);
        case ':': return lexer_make_token(TOKEN_COLON, lexer, start_pos);
        case '+':
            if (lexer_match(lexer, '=')) {
                return lexer_make_token(TOKEN_PLUS_EQ, lexer, start_pos);
            }
            return lexer_make_token(TOKEN_PLUS, lexer, start_pos);
        case '-':
            if (lexer_match(lexer, '=')) {
                return lexer_make_token(TOKEN_MINUS_EQ, lexer, start_pos);
            }
            return lexer_make_token(TOKEN_MINUS, lexer, start_pos);
        case '*':
            if (lexer_match(lexer, '=')) {
                return lexer_make_token(TOKEN_STAR_EQ, lexer, start_pos);
            }
            return lexer_make_token(TOKEN_STAR, lexer, start_pos);
        case '%':
            if (lexer_match(lexer, '=')) {
                return lexer_make_token(TOKEN_PERCENT_EQ, lexer, start_pos);
            }
            return lexer_make_token(TOKEN_PERCENT, lexer, start_pos);
        case '/':
            if (lexer_match(lexer, '=')) {
                return lexer_make_token(TOKEN_SLASH_EQ, lexer, start_pos);
            }
            return lexer_make_token(TOKEN_SLASH, lexer, start_pos);
        case '!':
            if (lexer_match(lexer, '=')) {
                return lexer_make_token(TOKEN_BANG_EQ, lexer, start_pos);
            }
            return lexer_make_token(TOKEN_NOT, lexer, start_pos);
        case '=':
            if (lexer_match(lexer, '=')) {
                return lexer_make_token(TOKEN_EQ_EQ, lexer, start_pos);
            }
            return lexer_make_token(TOKEN_ASSIGN, lexer, start_pos);
        case '<':
            if (lexer_match(lexer, '=')) {
                return lexer_make_token(TOKEN_LT_EQ, lexer, start_pos);
            }
            return lexer_make_token(TOKEN_LT, lexer, start_pos);
        case '>':
            if (lexer_match(lexer, '=')) {
                return lexer_make_token(TOKEN_GT_EQ, lexer, start_pos);
            }
            return lexer_make_token(TOKEN_GT, lexer, start_pos);
        case '&':
            if (lexer_match(lexer, '&')) {
                return lexer_make_token(TOKEN_AND, lexer, start_pos);
            }
            return lexer_make_token(TOKEN_BIT_AND, lexer, start_pos);
        case '|':
            if (lexer_match(lexer, '|')) {
                return lexer_make_token(TOKEN_OR, lexer, start_pos);
            }
            return lexer_make_token(TOKEN_BIT_OR, lexer, start_pos);
        case '^': return lexer_make_token(TOKEN_BIT_XOR, lexer, start_pos);
        case '~': return lexer_make_token(TOKEN_BIT_NOT, lexer, start_pos);
        case '"': return lexer_string(lexer, start_pos);
    }
    
    if (lexer_is_digit(c)) {
        lexer->position--;
        lexer->column--;
        return lexer_number(lexer, start_pos);
    }
    
    if (lexer_is_alpha(c) || c == '_') {
        lexer->position--;
        lexer->column--;
        return lexer_identifier(lexer, start_pos);
    }
    
    return lexer_error_token(lexer, "Unexpected character");
}

Token lexer_peek_token(Lexer* lexer) {
    Lexer temp = *lexer;
    Token token = lexer_next_token(&temp);
    return token;
}

void token_free(Token* token) {
    if (!token) return;
    if ((token->type == TOKEN_STRING || token->type == TOKEN_NUMBER_BIGINT) && token->as_string) {
        free(token->as_string);
        token->as_string = NULL;
    }
    if (token->lexeme) free(token->lexeme);
    token->lexeme = NULL;
}

const char* token_type_to_string(TokenType type) {
    switch (type) {
        case TOKEN_EOF: return "EOF";
        case TOKEN_IDENTIFIER: return "IDENTIFIER";
        case TOKEN_NUMBER_INT: return "NUMBER_INT";
        case TOKEN_NUMBER_BIGINT: return "NUMBER_BIGINT";
        case TOKEN_NUMBER_DOUBLE: return "NUMBER_DOUBLE";
        case TOKEN_STRING: return "STRING";
        case TOKEN_KEYWORD_INT: return "int";
        case TOKEN_KEYWORD_BOOL: return "bool";
        case TOKEN_KEYWORD_DOUBLE: return "double";
        case TOKEN_KEYWORD_BIGINT: return "bigint";
        case TOKEN_KEYWORD_STRING: return "string";
        case TOKEN_KEYWORD_BYTES: return "bytes";
        case TOKEN_KEYWORD_ARRAY: return "array";
        case TOKEN_KEYWORD_ANY: return "any";
        case TOKEN_KEYWORD_NIL: return "nil";
        case TOKEN_KEYWORD_VAR: return "var";
        case TOKEN_KEYWORD_CONST: return "const";
        case TOKEN_KEYWORD_PUBLIC: return "public";
        case TOKEN_KEYWORD_PRIVATE: return "private";
        case TOKEN_KEYWORD_TYPE: return "type";
        case TOKEN_KEYWORD_INTERFACE: return "interface";
        case TOKEN_KEYWORD_IMPL: return "impl";
        case TOKEN_KEYWORD_ASYNC: return "async";
        case TOKEN_KEYWORD_AWAIT: return "await";
        case TOKEN_KEYWORD_FUNC: return "func";
        case TOKEN_KEYWORD_IF: return "if";
        case TOKEN_KEYWORD_ELSE: return "else";
        case TOKEN_KEYWORD_LET: return "let";
        case TOKEN_KEYWORD_WHILE: return "while";
        case TOKEN_KEYWORD_FOREACH: return "foreach";
        case TOKEN_KEYWORD_BREAK: return "break";
        case TOKEN_KEYWORD_CONTINUE: return "continue";
        case TOKEN_KEYWORD_RETURN: return "return";
        case TOKEN_KEYWORD_DEFER: return "defer";
        case TOKEN_KEYWORD_IMPORT: return "import";
        case TOKEN_KEYWORD_RECORD: return "record";
        case TOKEN_KEYWORD_ENUM: return "enum";
        case TOKEN_KEYWORD_MATCH: return "match";
        case TOKEN_KEYWORD_SWITCH: return "switch";
        case TOKEN_KEYWORD_CASE: return "case";
        case TOKEN_KEYWORD_DEFAULT: return "default";
        case TOKEN_KEYWORD_MAP: return "map";
        case TOKEN_KEYWORD_SET: return "set";
        case TOKEN_TRUE: return "true";
        case TOKEN_FALSE: return "false";
        case TOKEN_PLUS: return "+";
        case TOKEN_PLUS_EQ: return "+=";
        case TOKEN_MINUS: return "-";
        case TOKEN_MINUS_EQ: return "-=";
        case TOKEN_STAR: return "*";
        case TOKEN_STAR_EQ: return "*=";
        case TOKEN_SLASH: return "/";
        case TOKEN_SLASH_EQ: return "/=";
        case TOKEN_PERCENT: return "%";
        case TOKEN_PERCENT_EQ: return "%=";
        case TOKEN_EQ_EQ: return "==";
        case TOKEN_BANG_EQ: return "!=";
        case TOKEN_LT: return "<";
        case TOKEN_LT_EQ: return "<=";
        case TOKEN_GT: return ">";
        case TOKEN_GT_EQ: return ">=";
        case TOKEN_AND: return "&&";
        case TOKEN_OR: return "||";
        case TOKEN_NOT: return "!";
        case TOKEN_BIT_AND: return "&";
        case TOKEN_BIT_OR: return "|";
        case TOKEN_BIT_XOR: return "^";
        case TOKEN_BIT_NOT: return "~";
        case TOKEN_ASSIGN: return "=";
        case TOKEN_LPAREN: return "(";
        case TOKEN_RPAREN: return ")";
        case TOKEN_LBRACE: return "{";
        case TOKEN_RBRACE: return "}";
        case TOKEN_LBRACKET: return "[";
        case TOKEN_RBRACKET: return "]";
        case TOKEN_COMMA: return ",";
        case TOKEN_COLON: return ":";
        case TOKEN_SEMICOLON: return ";";
        case TOKEN_DOT: return ".";
        case TOKEN_DOT_DOT: return "..";
        case TOKEN_QUESTION: return "?";
        case TOKEN_AS: return "as";
        case TOKEN_IN: return "in";
        case TOKEN_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
