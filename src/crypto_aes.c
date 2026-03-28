#include "crypto_aes.h"

#include <string.h>

typedef struct {
    uint8_t round_key[240];
    int round_count;
} CryptoAesCtx;

static const uint8_t crypto_aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static const uint8_t crypto_aes_rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

static uint8_t crypto_aes_xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ (((x >> 7) & 1u) * 0x1bu));
}

static void crypto_aes_sub_word(uint8_t word[4]) {
    for (int i = 0; i < 4; i++) {
        word[i] = crypto_aes_sbox[word[i]];
    }
}

static void crypto_aes_rot_word(uint8_t word[4]) {
    uint8_t tmp = word[0];
    word[0] = word[1];
    word[1] = word[2];
    word[2] = word[3];
    word[3] = tmp;
}

static bool crypto_aes_init(CryptoAesCtx* ctx, const uint8_t* key, size_t key_len) {
    if (!ctx || !key) return false;

    int nk = 0;
    int nr = 0;
    if (key_len == 16) {
        nk = 4;
        nr = 10;
    } else if (key_len == 24) {
        nk = 6;
        nr = 12;
    } else if (key_len == 32) {
        nk = 8;
        nr = 14;
    } else {
        return false;
    }

    ctx->round_count = nr;
    memcpy(ctx->round_key, key, key_len);

    int total_words = 4 * (nr + 1);
    uint8_t temp[4];
    for (int i = nk; i < total_words; i++) {
        temp[0] = ctx->round_key[(i - 1) * 4];
        temp[1] = ctx->round_key[(i - 1) * 4 + 1];
        temp[2] = ctx->round_key[(i - 1) * 4 + 2];
        temp[3] = ctx->round_key[(i - 1) * 4 + 3];

        if (i % nk == 0) {
            crypto_aes_rot_word(temp);
            crypto_aes_sub_word(temp);
            temp[0] ^= crypto_aes_rcon[i / nk];
        } else if (nk > 6 && i % nk == 4) {
            crypto_aes_sub_word(temp);
        }

        ctx->round_key[i * 4] = ctx->round_key[(i - nk) * 4] ^ temp[0];
        ctx->round_key[i * 4 + 1] = ctx->round_key[(i - nk) * 4 + 1] ^ temp[1];
        ctx->round_key[i * 4 + 2] = ctx->round_key[(i - nk) * 4 + 2] ^ temp[2];
        ctx->round_key[i * 4 + 3] = ctx->round_key[(i - nk) * 4 + 3] ^ temp[3];
    }

    return true;
}

static void crypto_aes_add_round_key(uint8_t state[16], const uint8_t* round_key) {
    for (int i = 0; i < 16; i++) {
        state[i] ^= round_key[i];
    }
}

static void crypto_aes_sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) {
        state[i] = crypto_aes_sbox[state[i]];
    }
}

static void crypto_aes_shift_rows(uint8_t state[16]) {
    uint8_t tmp;

    tmp = state[1];
    state[1] = state[5];
    state[5] = state[9];
    state[9] = state[13];
    state[13] = tmp;

    tmp = state[2];
    state[2] = state[10];
    state[10] = tmp;
    tmp = state[6];
    state[6] = state[14];
    state[14] = tmp;

    tmp = state[15];
    state[15] = state[11];
    state[11] = state[7];
    state[7] = state[3];
    state[3] = tmp;
}

static void crypto_aes_mix_columns(uint8_t state[16]) {
    for (int i = 0; i < 4; i++) {
        int col = i * 4;
        uint8_t a0 = state[col];
        uint8_t a1 = state[col + 1];
        uint8_t a2 = state[col + 2];
        uint8_t a3 = state[col + 3];
        uint8_t sum = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);

        state[col] ^= sum ^ crypto_aes_xtime((uint8_t)(a0 ^ a1));
        state[col + 1] ^= sum ^ crypto_aes_xtime((uint8_t)(a1 ^ a2));
        state[col + 2] ^= sum ^ crypto_aes_xtime((uint8_t)(a2 ^ a3));
        state[col + 3] ^= sum ^ crypto_aes_xtime((uint8_t)(a3 ^ a0));
    }
}

static void crypto_aes_encrypt_block(const CryptoAesCtx* ctx, const uint8_t input[16], uint8_t out[16]) {
    uint8_t state[16];
    memcpy(state, input, 16);

    crypto_aes_add_round_key(state, ctx->round_key);

    for (int round = 1; round < ctx->round_count; round++) {
        crypto_aes_sub_bytes(state);
        crypto_aes_shift_rows(state);
        crypto_aes_mix_columns(state);
        crypto_aes_add_round_key(state, ctx->round_key + round * 16);
    }

    crypto_aes_sub_bytes(state);
    crypto_aes_shift_rows(state);
    crypto_aes_add_round_key(state, ctx->round_key + ctx->round_count * 16);

    memcpy(out, state, 16);
}

static void crypto_aes_increment_counter(uint8_t counter[16]) {
    for (int i = 15; i >= 0; i--) {
        counter[i]++;
        if (counter[i] != 0) {
            break;
        }
    }
}

bool crypto_aes_ctr(const uint8_t* key,
                    size_t key_len,
                    const uint8_t counter[16],
                    const uint8_t* input,
                    size_t input_len,
                    uint8_t* out) {
    if (!key || !counter) return false;
    if (!out && input_len != 0) return false;
    if (!input && input_len != 0) return false;

    CryptoAesCtx ctx;
    if (!crypto_aes_init(&ctx, key, key_len)) {
        return false;
    }

    uint8_t stream_block[16];
    uint8_t counter_block[16];
    memcpy(counter_block, counter, sizeof(counter_block));

    size_t offset = 0;
    while (offset < input_len) {
        crypto_aes_encrypt_block(&ctx, counter_block, stream_block);
        size_t chunk = input_len - offset;
        if (chunk > 16u) chunk = 16u;

        for (size_t i = 0; i < chunk; i++) {
            out[offset + i] = input[offset + i] ^ stream_block[i];
        }

        crypto_aes_increment_counter(counter_block);
        offset += chunk;
    }

    return true;
}

static void crypto_aes_increment_counter32(uint8_t counter[16]) {
    for (int i = 15; i >= 12; i--) {
        counter[i]++;
        if (counter[i] != 0) {
            break;
        }
    }
}

static void crypto_aes_xor_block(uint8_t dst[16], const uint8_t src[16]) {
    for (int i = 0; i < 16; i++) {
        dst[i] ^= src[i];
    }
}

static void crypto_aes_shift_right_one(uint8_t block[16]) {
    uint8_t carry = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t next_carry = (uint8_t)(block[i] & 1u);
        block[i] = (uint8_t)((block[i] >> 1) | (carry ? 0x80u : 0u));
        carry = next_carry;
    }
}

static void crypto_aes_gcm_multiply(const uint8_t x[16], const uint8_t y[16], uint8_t out[16]) {
    uint8_t z[16] = {0};
    uint8_t v[16];
    memcpy(v, y, sizeof(v));

    for (int i = 0; i < 128; i++) {
        uint8_t bit = (uint8_t)((x[i / 8] >> (7 - (i % 8))) & 1u);
        if (bit) {
            crypto_aes_xor_block(z, v);
        }

        uint8_t lsb = (uint8_t)(v[15] & 1u);
        crypto_aes_shift_right_one(v);
        if (lsb) {
            v[0] ^= 0xe1u;
        }
    }

    memcpy(out, z, sizeof(z));
}

static void crypto_aes_gcm_ghash_update(uint8_t y[16],
                                        const uint8_t h[16],
                                        const uint8_t* data,
                                        size_t data_len) {
    uint8_t block[16];
    while (data_len >= 16u) {
        crypto_aes_xor_block(y, data);
        crypto_aes_gcm_multiply(y, h, block);
        memcpy(y, block, sizeof(block));
        data += 16;
        data_len -= 16u;
    }

    if (data_len > 0u) {
        memset(block, 0, sizeof(block));
        memcpy(block, data, data_len);
        crypto_aes_xor_block(y, block);
        crypto_aes_gcm_multiply(y, h, block);
        memcpy(y, block, sizeof(block));
    }
}

static void crypto_aes_store_u64_be(uint8_t out[8], uint64_t value) {
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(value & 0xffu);
        value >>= 8;
    }
}

static bool crypto_aes_gcm_build_tag(const CryptoAesCtx* ctx,
                                     const uint8_t nonce[12],
                                     const uint8_t* aad,
                                     size_t aad_len,
                                     const uint8_t* ciphertext,
                                     size_t ciphertext_len,
                                     uint8_t out_tag[16]) {
    if (aad_len > (UINT64_MAX / 8u) || ciphertext_len > (UINT64_MAX / 8u)) {
        return false;
    }

    uint8_t h[16] = {0};
    uint8_t zero[16] = {0};
    crypto_aes_encrypt_block(ctx, zero, h);

    uint8_t y[16] = {0};
    crypto_aes_gcm_ghash_update(y, h, aad, aad_len);
    crypto_aes_gcm_ghash_update(y, h, ciphertext, ciphertext_len);

    uint8_t len_block[16];
    crypto_aes_store_u64_be(len_block, (uint64_t)aad_len * 8u);
    crypto_aes_store_u64_be(len_block + 8, (uint64_t)ciphertext_len * 8u);
    crypto_aes_xor_block(y, len_block);
    crypto_aes_gcm_multiply(y, h, len_block);
    memcpy(y, len_block, sizeof(len_block));

    uint8_t j0[16];
    memcpy(j0, nonce, 12);
    j0[12] = 0x00;
    j0[13] = 0x00;
    j0[14] = 0x00;
    j0[15] = 0x01;

    uint8_t s[16];
    crypto_aes_encrypt_block(ctx, j0, s);
    for (int i = 0; i < 16; i++) {
        out_tag[i] = (uint8_t)(s[i] ^ y[i]);
    }

    return true;
}

static bool crypto_aes_constant_time_equal(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool crypto_aes_gcm_seal(const uint8_t* key,
                         size_t key_len,
                         const uint8_t nonce[12],
                         const uint8_t* aad,
                         size_t aad_len,
                         const uint8_t* plaintext,
                         size_t plaintext_len,
                         uint8_t* out_ciphertext,
                         uint8_t out_tag[16]) {
    if (!key || !nonce || !out_tag) return false;
    if (!out_ciphertext && plaintext_len != 0u) return false;
    if (!plaintext && plaintext_len != 0u) return false;
    if (!aad && aad_len != 0u) return false;

    CryptoAesCtx ctx;
    if (!crypto_aes_init(&ctx, key, key_len)) {
        return false;
    }

    uint8_t counter[16];
    memcpy(counter, nonce, 12);
    counter[12] = 0x00;
    counter[13] = 0x00;
    counter[14] = 0x00;
    counter[15] = 0x01;
    crypto_aes_increment_counter32(counter);

    uint8_t stream_block[16];
    size_t offset = 0;
    while (offset < plaintext_len) {
        crypto_aes_encrypt_block(&ctx, counter, stream_block);
        size_t chunk = plaintext_len - offset;
        if (chunk > 16u) chunk = 16u;
        for (size_t i = 0; i < chunk; i++) {
            out_ciphertext[offset + i] = (uint8_t)(plaintext[offset + i] ^ stream_block[i]);
        }
        crypto_aes_increment_counter32(counter);
        offset += chunk;
    }

    return crypto_aes_gcm_build_tag(&ctx, nonce, aad, aad_len, out_ciphertext, plaintext_len, out_tag);
}

bool crypto_aes_gcm_open(const uint8_t* key,
                         size_t key_len,
                         const uint8_t nonce[12],
                         const uint8_t* aad,
                         size_t aad_len,
                         const uint8_t* ciphertext,
                         size_t ciphertext_len,
                         const uint8_t tag[16],
                         uint8_t* out_plaintext) {
    if (!key || !nonce || !tag) return false;
    if (!out_plaintext && ciphertext_len != 0u) return false;
    if (!ciphertext && ciphertext_len != 0u) return false;
    if (!aad && aad_len != 0u) return false;

    CryptoAesCtx ctx;
    if (!crypto_aes_init(&ctx, key, key_len)) {
        return false;
    }

    uint8_t computed_tag[16];
    if (!crypto_aes_gcm_build_tag(&ctx, nonce, aad, aad_len, ciphertext, ciphertext_len, computed_tag)) {
        return false;
    }
    if (!crypto_aes_constant_time_equal(computed_tag, tag, sizeof(computed_tag))) {
        return false;
    }

    uint8_t counter[16];
    memcpy(counter, nonce, 12);
    counter[12] = 0x00;
    counter[13] = 0x00;
    counter[14] = 0x00;
    counter[15] = 0x01;
    crypto_aes_increment_counter32(counter);

    uint8_t stream_block[16];
    size_t offset = 0;
    while (offset < ciphertext_len) {
        crypto_aes_encrypt_block(&ctx, counter, stream_block);
        size_t chunk = ciphertext_len - offset;
        if (chunk > 16u) chunk = 16u;
        for (size_t i = 0; i < chunk; i++) {
            out_plaintext[offset + i] = (uint8_t)(ciphertext[offset + i] ^ stream_block[i]);
        }
        crypto_aes_increment_counter32(counter);
        offset += chunk;
    }

    return true;
}
