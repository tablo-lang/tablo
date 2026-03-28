#ifndef HTTP_FUZZ_H
#define HTTP_FUZZ_H

#include "vm.h"

#include <stdbool.h>
#include <stddef.h>

// Fuzz/test helper wrappers around HTTP parser paths.
// Returns true when parsing succeeds and false when it fails with a handled parse/runtime error.
bool tablo_http_fuzz_parse_request(VM* vm, const char* raw, size_t raw_len);
bool tablo_http_fuzz_parse_response(VM* vm, const char* raw, size_t raw_len);
// Expects raw chunked body bytes only, without status line or headers.
bool tablo_http_fuzz_parse_chunked_body(VM* vm, const char* raw, size_t raw_len);

#endif
