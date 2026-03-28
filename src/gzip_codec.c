#include "gzip_codec.h"

#include "safe_alloc.h"
#include "third_party/miniz.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    GZIP_FIXED_HEADER_LEN = 10,
    GZIP_TRAILER_LEN = 8,
    GZIP_FLAG_TEXT = 0x01,
    GZIP_FLAG_HCRC = 0x02,
    GZIP_FLAG_EXTRA = 0x04,
    GZIP_FLAG_NAME = 0x08,
    GZIP_FLAG_COMMENT = 0x10,
    GZIP_FLAG_RESERVED = 0xE0
};

static void gzip_write_u32_le(uint8_t* dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint16_t gzip_read_u16_le(const uint8_t* src) {
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t gzip_read_u32_le(const uint8_t* src) {
    return ((uint32_t)src[0]) |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static bool gzip_skip_cstring(const uint8_t* input,
                              size_t input_len,
                              size_t* cursor,
                              const char** err_msg) {
    if (!cursor) return false;
    while (*cursor < input_len) {
        if (input[*cursor] == 0) {
            (*cursor)++;
            return true;
        }
        (*cursor)++;
    }
    if (err_msg) *err_msg = "Malformed gzip header";
    return false;
}

static bool gzip_parse_member_header(const uint8_t* input,
                                     size_t input_len,
                                     size_t member_offset,
                                     size_t* out_deflate_offset,
                                     const char** err_msg) {
    if (!input || !out_deflate_offset || member_offset >= input_len) {
        if (err_msg) *err_msg = "Malformed gzip stream";
        return false;
    }
    if (input_len - member_offset < GZIP_FIXED_HEADER_LEN) {
        if (err_msg) *err_msg = "Truncated gzip header";
        return false;
    }

    const uint8_t* header = input + member_offset;
    if (header[0] != 0x1Fu || header[1] != 0x8Bu) {
        if (err_msg) *err_msg = "Invalid gzip header";
        return false;
    }
    if (header[2] != 8u) {
        if (err_msg) *err_msg = "Unsupported gzip compression method";
        return false;
    }

    uint8_t flags = header[3];
    if ((flags & GZIP_FLAG_RESERVED) != 0) {
        if (err_msg) *err_msg = "Unsupported gzip header flags";
        return false;
    }

    size_t cursor = member_offset + GZIP_FIXED_HEADER_LEN;
    if ((flags & GZIP_FLAG_EXTRA) != 0) {
        if (input_len - cursor < 2) {
            if (err_msg) *err_msg = "Truncated gzip extra field";
            return false;
        }
        uint16_t extra_len = gzip_read_u16_le(input + cursor);
        cursor += 2;
        if (input_len - cursor < extra_len) {
            if (err_msg) *err_msg = "Truncated gzip extra field";
            return false;
        }
        cursor += extra_len;
    }
    if ((flags & GZIP_FLAG_NAME) != 0 &&
        !gzip_skip_cstring(input, input_len, &cursor, err_msg)) {
        return false;
    }
    if ((flags & GZIP_FLAG_COMMENT) != 0 &&
        !gzip_skip_cstring(input, input_len, &cursor, err_msg)) {
        return false;
    }
    if ((flags & GZIP_FLAG_HCRC) != 0) {
        if (input_len - cursor < 2) {
            if (err_msg) *err_msg = "Truncated gzip header CRC";
            return false;
        }
        cursor += 2;
    }

    *out_deflate_offset = cursor;
    return true;
}

static bool gzip_buffer_append(uint8_t** data,
                               size_t* len,
                               size_t* cap,
                               const uint8_t* chunk,
                               size_t chunk_len,
                               size_t max_output_len,
                               const char** err_msg) {
    if (!data || !len || !cap) return false;
    if (chunk_len == 0) return true;
    if (*len > max_output_len || chunk_len > max_output_len - *len) {
        if (err_msg) *err_msg = "Gzip payload exceeds configured output limit";
        return false;
    }

    size_t needed = *len + chunk_len;
    if (needed > *cap) {
        size_t next_cap = *cap > 0 ? *cap : 1;
        while (next_cap < needed) {
            size_t grown = next_cap * 2;
            if (grown <= next_cap) {
                next_cap = needed;
                break;
            }
            next_cap = grown;
        }
        if (next_cap > max_output_len) {
            next_cap = needed;
        }
        *data = (uint8_t*)safe_realloc(*data, next_cap);
        *cap = next_cap;
    }

    memcpy(*data + *len, chunk, chunk_len);
    *len += chunk_len;
    return true;
}

static bool gzip_inflate_raw_member(const uint8_t* input,
                                    size_t input_len,
                                    size_t max_output_len,
                                    uint8_t** out_data,
                                    size_t* out_len,
                                    size_t* out_consumed,
                                    const char** err_msg) {
    if (out_data) *out_data = NULL;
    if (out_len) *out_len = 0;
    if (out_consumed) *out_consumed = 0;
    if (!out_data || !out_len || !out_consumed) return false;
    if (input_len > (size_t)UINT_MAX) {
        if (err_msg) *err_msg = "Gzip member exceeds supported size";
        return false;
    }

    mz_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
        if (err_msg) *err_msg = "Failed to initialize gzip decompressor";
        return false;
    }

    uint8_t* out = NULL;
    size_t out_cap = max_output_len > 0 && max_output_len < 4096 ? max_output_len : 4096;
    if (out_cap == 0) out_cap = 1;
    out = (uint8_t*)safe_malloc(out_cap);

    stream.next_in = (const unsigned char*)(input_len > 0 ? input : (const uint8_t*)"\0");
    stream.avail_in = (unsigned int)input_len;

    int status = MZ_OK;
    size_t produced = 0;
    while (true) {
        if (produced >= out_cap) {
            if (produced >= max_output_len) {
                free(out);
                mz_inflateEnd(&stream);
                if (err_msg) *err_msg = "Gzip payload exceeds configured output limit";
                return false;
            }
            size_t next_cap = out_cap * 2;
            if (next_cap <= out_cap || next_cap > max_output_len) {
                next_cap = max_output_len;
            }
            if (next_cap <= out_cap) next_cap = out_cap + 1;
            out = (uint8_t*)safe_realloc(out, next_cap);
            out_cap = next_cap;
        }

        stream.next_out = out + produced;
        stream.avail_out = (unsigned int)(out_cap - produced);
        status = mz_inflate(&stream, MZ_NO_FLUSH);
        produced = (size_t)stream.total_out;

        if (produced > max_output_len) {
            free(out);
            mz_inflateEnd(&stream);
            if (err_msg) *err_msg = "Gzip payload exceeds configured output limit";
            return false;
        }

        if (status == MZ_STREAM_END) {
            break;
        }
        if (status != MZ_OK) {
            free(out);
            mz_inflateEnd(&stream);
            if (err_msg) *err_msg = "Failed to inflate gzip payload";
            return false;
        }
        if (stream.avail_in == 0) {
            free(out);
            mz_inflateEnd(&stream);
            if (err_msg) *err_msg = "Truncated gzip payload";
            return false;
        }
    }

    *out_data = out;
    *out_len = produced;
    *out_consumed = (size_t)stream.total_in;
    mz_inflateEnd(&stream);
    return true;
}

bool gzip_codec_compress(const uint8_t* input,
                         size_t input_len,
                         uint8_t** out_data,
                         size_t* out_len,
                         const char** err_msg) {
    if (out_data) *out_data = NULL;
    if (out_len) *out_len = 0;
    if (!out_data || !out_len) return false;
    if (input_len > (size_t)UINT_MAX) {
        if (err_msg) *err_msg = "Gzip payload exceeds supported size";
        return false;
    }

    mz_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (mz_deflateInit2(&stream,
                        MZ_DEFAULT_COMPRESSION,
                        MZ_DEFLATED,
                        -MZ_DEFAULT_WINDOW_BITS,
                        9,
                        MZ_DEFAULT_STRATEGY) != MZ_OK) {
        if (err_msg) *err_msg = "Failed to initialize gzip compressor";
        return false;
    }

    mz_ulong bound = mz_deflateBound(&stream, (mz_ulong)input_len);
    if ((size_t)bound > SIZE_MAX - (GZIP_FIXED_HEADER_LEN + GZIP_TRAILER_LEN)) {
        mz_deflateEnd(&stream);
        if (err_msg) *err_msg = "Gzip payload exceeds supported size";
        return false;
    }
    if (bound > (mz_ulong)UINT_MAX) {
        mz_deflateEnd(&stream);
        if (err_msg) *err_msg = "Gzip payload exceeds supported size";
        return false;
    }

    size_t total_cap = GZIP_FIXED_HEADER_LEN + (size_t)bound + GZIP_TRAILER_LEN;
    uint8_t* out = (uint8_t*)safe_malloc(total_cap);
    memset(out, 0, GZIP_FIXED_HEADER_LEN);
    out[0] = 0x1F;
    out[1] = 0x8B;
    out[2] = 8;
    out[3] = 0;
    out[8] = 0;
    out[9] = 255;

    stream.next_in = (const unsigned char*)(input_len > 0 ? input : (const uint8_t*)"\0");
    stream.avail_in = (unsigned int)input_len;
    stream.next_out = out + GZIP_FIXED_HEADER_LEN;
    stream.avail_out = (unsigned int)bound;

    int status = mz_deflate(&stream, MZ_FINISH);
    if (status != MZ_STREAM_END) {
        free(out);
        mz_deflateEnd(&stream);
        if (err_msg) *err_msg = "Failed to deflate gzip payload";
        return false;
    }

    size_t deflated_len = (size_t)stream.total_out;
    mz_deflateEnd(&stream);

    mz_ulong crc = mz_crc32(MZ_CRC32_INIT, input, input_len);
    gzip_write_u32_le(out + GZIP_FIXED_HEADER_LEN + deflated_len, (uint32_t)crc);
    gzip_write_u32_le(out + GZIP_FIXED_HEADER_LEN + deflated_len + 4, (uint32_t)(input_len & 0xFFFFFFFFu));

    *out_data = out;
    *out_len = GZIP_FIXED_HEADER_LEN + deflated_len + GZIP_TRAILER_LEN;
    return true;
}

bool gzip_codec_decompress(const uint8_t* input,
                           size_t input_len,
                           size_t max_output_len,
                           uint8_t** out_data,
                           size_t* out_len,
                           const char** err_msg) {
    if (out_data) *out_data = NULL;
    if (out_len) *out_len = 0;
    if (!out_data || !out_len) return false;
    if (!input || input_len == 0) {
        if (err_msg) *err_msg = "Invalid gzip payload";
        return false;
    }

    uint8_t* output = NULL;
    size_t output_len = 0;
    size_t output_cap = 0;
    size_t cursor = 0;

    while (cursor < input_len) {
        while (cursor < input_len && input[cursor] == 0) cursor++;
        if (cursor >= input_len) break;

        size_t deflate_offset = 0;
        if (!gzip_parse_member_header(input, input_len, cursor, &deflate_offset, err_msg)) {
            free(output);
            return false;
        }

        size_t remaining_limit = max_output_len >= output_len ? (max_output_len - output_len) : 0;
        uint8_t* member_out = NULL;
        size_t member_len = 0;
        size_t member_consumed = 0;
        if (!gzip_inflate_raw_member(input + deflate_offset,
                                     input_len - deflate_offset,
                                     remaining_limit,
                                     &member_out,
                                     &member_len,
                                     &member_consumed,
                                     err_msg)) {
            free(output);
            return false;
        }

        size_t trailer_offset = deflate_offset + member_consumed;
        if (input_len - trailer_offset < GZIP_TRAILER_LEN) {
            free(member_out);
            free(output);
            if (err_msg) *err_msg = "Truncated gzip trailer";
            return false;
        }

        uint32_t expected_crc = gzip_read_u32_le(input + trailer_offset);
        uint32_t expected_isize = gzip_read_u32_le(input + trailer_offset + 4);
        uint32_t actual_crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, member_out, member_len);
        if (actual_crc != expected_crc) {
            free(member_out);
            free(output);
            if (err_msg) *err_msg = "Invalid gzip CRC32";
            return false;
        }
        if ((uint32_t)(member_len & 0xFFFFFFFFu) != expected_isize) {
            free(member_out);
            free(output);
            if (err_msg) *err_msg = "Invalid gzip size trailer";
            return false;
        }

        if (!gzip_buffer_append(&output,
                                &output_len,
                                &output_cap,
                                member_out,
                                member_len,
                                max_output_len,
                                err_msg)) {
            free(member_out);
            free(output);
            return false;
        }
        free(member_out);
        cursor = trailer_offset + GZIP_TRAILER_LEN;
    }

    if (!output) {
        output = (uint8_t*)safe_malloc(1);
    }

    *out_data = output;
    *out_len = output_len;
    return true;
}
