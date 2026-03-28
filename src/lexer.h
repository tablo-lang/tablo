#ifndef LEXER_H
#define LEXER_H

#include "common.h"

typedef enum {
    TOKEN_EOF,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER_INT,
    TOKEN_NUMBER_BIGINT,
    TOKEN_NUMBER_DOUBLE,
    TOKEN_STRING,
    TOKEN_KEYWORD_INT,
    TOKEN_KEYWORD_BOOL,
    TOKEN_KEYWORD_DOUBLE,
    TOKEN_KEYWORD_BIGINT,
    TOKEN_KEYWORD_STRING,
    TOKEN_KEYWORD_BYTES,
    TOKEN_KEYWORD_ARRAY,
    TOKEN_KEYWORD_ANY,
    TOKEN_KEYWORD_NIL,
    TOKEN_KEYWORD_VAR,
    TOKEN_KEYWORD_CONST,
    TOKEN_KEYWORD_PUBLIC,
    TOKEN_KEYWORD_PRIVATE,
    TOKEN_KEYWORD_TYPE,
    TOKEN_KEYWORD_INTERFACE,
    TOKEN_KEYWORD_IMPL,
    TOKEN_KEYWORD_ASYNC,
    TOKEN_KEYWORD_AWAIT,
    TOKEN_KEYWORD_FUNC,
    TOKEN_KEYWORD_IF,
    TOKEN_KEYWORD_ELSE,
    TOKEN_KEYWORD_LET,
    TOKEN_KEYWORD_WHILE,
    TOKEN_KEYWORD_FOREACH,
    TOKEN_KEYWORD_BREAK,
    TOKEN_KEYWORD_CONTINUE,
    TOKEN_KEYWORD_RETURN,
    TOKEN_KEYWORD_DEFER,
    TOKEN_KEYWORD_IMPORT,
    TOKEN_KEYWORD_RECORD,
    TOKEN_KEYWORD_ENUM,
    TOKEN_KEYWORD_MATCH,
    TOKEN_KEYWORD_SWITCH,
    TOKEN_KEYWORD_CASE,
    TOKEN_KEYWORD_DEFAULT,
    TOKEN_KEYWORD_MAP,
    TOKEN_KEYWORD_SET,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_PLUS,
    TOKEN_PLUS_EQ,
    TOKEN_MINUS,
    TOKEN_MINUS_EQ,
    TOKEN_STAR,
    TOKEN_STAR_EQ,
    TOKEN_SLASH,
    TOKEN_SLASH_EQ,
    TOKEN_PERCENT,
    TOKEN_PERCENT_EQ,
    TOKEN_EQ_EQ,
    TOKEN_BANG_EQ,
    TOKEN_LT,
    TOKEN_LT_EQ,
    TOKEN_GT,
    TOKEN_GT_EQ,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_NOT,
    TOKEN_BIT_AND,
    TOKEN_BIT_OR,
    TOKEN_BIT_XOR,
    TOKEN_BIT_NOT,
    TOKEN_ASSIGN,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_COMMA,
    TOKEN_COLON,
    TOKEN_SEMICOLON,
    TOKEN_DOT,
    TOKEN_DOT_DOT,
    TOKEN_QUESTION,
    TOKEN_AS,
    TOKEN_IN,
    TOKEN_ERROR
} TokenType;

typedef struct {
    TokenType type;
    char* lexeme;
    int line;
    int column;
    union {
        int64_t as_int;
        double as_double;
        char* as_string;
    };
} Token;

typedef struct {
    const char* source;
    char* file;
    const char* pending_error;
    int length;
    int position;
    int line;
    int column;
} Lexer;

void lexer_init(Lexer* lexer, const char* source, const char* file);
Token lexer_next_token(Lexer* lexer);
Token lexer_peek_token(Lexer* lexer);
void token_free(Token* token);
const char* token_type_to_string(TokenType type);

#endif
