#ifndef TABLO_FUZZ_TARGETS_H
#define TABLO_FUZZ_TARGETS_H

#include <stddef.h>
#include <stdint.h>

int fuzz_lexer_one_input(const uint8_t* data, size_t size);
int fuzz_parser_one_input(const uint8_t* data, size_t size);
int fuzz_compile_one_input(const uint8_t* data, size_t size);
int fuzz_http_one_input(const uint8_t* data, size_t size);
int fuzz_artifact_one_input(const uint8_t* data, size_t size);

#endif
