#include "common.h"
#include "safe_alloc.h"
#include <stdlib.h>
#include <string.h>

Error* error_create(ErrorCode code, const char* message, const char* file, int line, int column) {
    Error* err = (Error*)safe_malloc(sizeof(Error));
    if (!err) return NULL;
    
    err->code = code;
    err->message = message ? safe_strdup(message) : NULL;
    err->file = file ? safe_strdup(file) : NULL;
    err->line = line;
    err->column = column;
    
    return err;
}

void error_free(Error* err) {
    if (!err) return;
    if (err->message) free(err->message);
    if (err->file) free(err->file);
    free(err);
}

const char* error_to_string(ErrorCode code) {
    switch (code) {
        case ERROR_NONE: return "No error";
        case ERROR_SYNTAX: return "Syntax error";
        case ERROR_TYPE: return "Type error";
        case ERROR_RUNTIME: return "Runtime error";
        case ERROR_IMPORT: return "Import error";
        case ERROR_COMPILE: return "Compilation error";
        default: return "Unknown error";
    }
}
