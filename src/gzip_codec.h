#ifndef GZIP_CODEC_H
#define GZIP_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool gzip_codec_compress(const uint8_t* input,
                         size_t input_len,
                         uint8_t** out_data,
                         size_t* out_len,
                         const char** err_msg);

bool gzip_codec_decompress(const uint8_t* input,
                           size_t input_len,
                           size_t max_output_len,
                           uint8_t** out_data,
                           size_t* out_len,
                           const char** err_msg);

#endif
