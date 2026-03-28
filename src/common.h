#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ERROR_NONE,
    ERROR_SYNTAX,
    ERROR_TYPE,
    ERROR_RUNTIME,
    ERROR_IMPORT,
    ERROR_COMPILE
} ErrorCode;

typedef struct {
    ErrorCode code;
    char* message;
    char* file;
    int line;
    int column;
} Error;

Error* error_create(ErrorCode code, const char* message, const char* file, int line, int column);
void error_free(Error* err);
const char* error_to_string(ErrorCode code);

#endif
