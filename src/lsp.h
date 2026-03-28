#ifndef LSP_H
#define LSP_H

#include "common.h"

char* lsp_build_document_symbols_json(const char* file_path, Error** out_error);
char* lsp_build_hover_json(const char* file_path, int zero_based_line, int zero_based_character, Error** out_error);
char* lsp_build_definition_json(const char* file_path, int zero_based_line, int zero_based_character, Error** out_error);
char* lsp_build_diagnostics_json(const char* file_path, const char* source_text, Error** out_error);

#endif
