#include "crypto_hash.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} CryptoSha256Ctx;

static uint32_t crypto_sha256_rotr(uint32_t value, uint32_t amount) {
    return (value >> amount) | (value << (32u - amount));
}

static uint32_t crypto_sha256_ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

static uint32_t crypto_sha256_maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t crypto_sha256_sigma0(uint32_t x) {
    return crypto_sha256_rotr(x, 2) ^ crypto_sha256_rotr(x, 13) ^ crypto_sha256_rotr(x, 22);
}

static uint32_t crypto_sha256_sigma1(uint32_t x) {
    return crypto_sha256_rotr(x, 6) ^ crypto_sha256_rotr(x, 11) ^ crypto_sha256_rotr(x, 25);
}

static uint32_t crypto_sha256_gamma0(uint32_t x) {
    return crypto_sha256_rotr(x, 7) ^ crypto_sha256_rotr(x, 18) ^ (x >> 3);
}

static uint32_t crypto_sha256_gamma1(uint32_t x) {
    return crypto_sha256_rotr(x, 17) ^ crypto_sha256_rotr(x, 19) ^ (x >> 10);
}

static void crypto_sha256_transform(CryptoSha256Ctx* ctx, const uint8_t data[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };

    uint32_t m[64];
    for (int i = 0; i < 16; i++) {
        m[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        m[i] = crypto_sha256_gamma1(m[i - 2]) + m[i - 7] + crypto_sha256_gamma0(m[i - 15]) + m[i - 16];
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + crypto_sha256_sigma1(e) + crypto_sha256_ch(e, f, g) + k[i] + m[i];
        uint32_t t2 = crypto_sha256_sigma0(a) + crypto_sha256_maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void crypto_sha256_init(CryptoSha256Ctx* ctx) {
    if (!ctx) return;
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

static void crypto_sha256_update_ctx(CryptoSha256Ctx* ctx, const uint8_t* data, size_t len) {
    if (!ctx || (!data && len != 0)) return;
    for (size_t i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            crypto_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void crypto_sha256_final(CryptoSha256Ctx* ctx, uint8_t out[32]) {
    if (!ctx || !out) return;

    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        crypto_sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8u;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    crypto_sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; i++) {
        out[i] = (uint8_t)((ctx->state[0] >> (24 - i * 8)) & 0xff);
        out[i + 4] = (uint8_t)((ctx->state[1] >> (24 - i * 8)) & 0xff);
        out[i + 8] = (uint8_t)((ctx->state[2] >> (24 - i * 8)) & 0xff);
        out[i + 12] = (uint8_t)((ctx->state[3] >> (24 - i * 8)) & 0xff);
        out[i + 16] = (uint8_t)((ctx->state[4] >> (24 - i * 8)) & 0xff);
        out[i + 20] = (uint8_t)((ctx->state[5] >> (24 - i * 8)) & 0xff);
        out[i + 24] = (uint8_t)((ctx->state[6] >> (24 - i * 8)) & 0xff);
        out[i + 28] = (uint8_t)((ctx->state[7] >> (24 - i * 8)) & 0xff);
    }
}

void crypto_sha256(const uint8_t* data, size_t data_len, uint8_t out[32]) {
    CryptoSha256Ctx ctx;
    crypto_sha256_init(&ctx);
    crypto_sha256_update_ctx(&ctx, data, data_len);
    crypto_sha256_final(&ctx, out);
}

void crypto_hmac_sha256(const uint8_t* key,
                        size_t key_len,
                        const uint8_t* data,
                        size_t data_len,
                        uint8_t out[32]) {
    uint8_t key_block[64];
    memset(key_block, 0, sizeof(key_block));

    if (key_len > 64) {
        uint8_t hashed_key[32];
        crypto_sha256(key, key_len, hashed_key);
        memcpy(key_block, hashed_key, sizeof(hashed_key));
    } else if (key && key_len > 0) {
        memcpy(key_block, key, key_len);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(key_block[i] ^ 0x36u);
        opad[i] = (uint8_t)(key_block[i] ^ 0x5cu);
    }

    uint8_t inner_hash[32];
    CryptoSha256Ctx inner_ctx;
    crypto_sha256_init(&inner_ctx);
    crypto_sha256_update_ctx(&inner_ctx, ipad, sizeof(ipad));
    crypto_sha256_update_ctx(&inner_ctx, data, data_len);
    crypto_sha256_final(&inner_ctx, inner_hash);

    CryptoSha256Ctx outer_ctx;
    crypto_sha256_init(&outer_ctx);
    crypto_sha256_update_ctx(&outer_ctx, opad, sizeof(opad));
    crypto_sha256_update_ctx(&outer_ctx, inner_hash, sizeof(inner_hash));
    crypto_sha256_final(&outer_ctx, out);
}

bool crypto_constant_time_equal(const uint8_t* a, const uint8_t* b, size_t len) {
    if ((!a || !b) && len != 0u) return false;

    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool crypto_hkdf_hmac_sha256(const uint8_t* ikm,
                             size_t ikm_len,
                             const uint8_t* salt,
                             size_t salt_len,
                             const uint8_t* info,
                             size_t info_len,
                             uint8_t* out,
                             size_t out_len) {
    if (!out && out_len != 0u) return false;
    if (!ikm && ikm_len != 0u) return false;
    if (!salt && salt_len != 0u) return false;
    if (!info && info_len != 0u) return false;
    if (out_len == 0u) return true;
    if (out_len > 255u * 32u) return false;
    if (info_len > SIZE_MAX - 33u) return false;

    uint8_t prk[32];
    if (salt_len == 0u) {
        uint8_t zero_salt[32] = {0};
        crypto_hmac_sha256(zero_salt, sizeof(zero_salt), ikm, ikm_len, prk);
    } else {
        crypto_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    }

    uint8_t* block = (uint8_t*)malloc(info_len + 33u);
    if (!block) {
        memset(prk, 0, sizeof(prk));
        return false;
    }

    uint8_t t[32];
    size_t t_len = 0u;
    size_t offset = 0u;
    uint8_t counter = 1u;
    while (offset < out_len) {
        if (t_len > 0u) {
            memcpy(block, t, t_len);
        }
        if (info_len > 0u) {
            memcpy(block + t_len, info, info_len);
        }
        block[t_len + info_len] = counter;

        crypto_hmac_sha256(prk, sizeof(prk), block, t_len + info_len + 1u, t);
        t_len = sizeof(t);

        size_t take = out_len - offset;
        if (take > sizeof(t)) take = sizeof(t);
        memcpy(out + offset, t, take);
        offset += take;
        counter++;
    }

    memset(t, 0, sizeof(t));
    memset(prk, 0, sizeof(prk));
    free(block);
    return true;
}

bool crypto_pbkdf2_hmac_sha256(const uint8_t* password,
                               size_t password_len,
                               const uint8_t* salt,
                               size_t salt_len,
                               uint32_t iterations,
                               uint8_t* out,
                               size_t out_len) {
    if (iterations == 0) return false;
    if (!out && out_len != 0) return false;
    if (out_len == 0) return true;
    if (salt_len > SIZE_MAX - 4) return false;

    size_t block_count = (out_len + 31u) / 32u;
    if (block_count > UINT32_MAX) return false;

    uint8_t* salt_block = (uint8_t*)malloc(salt_len + 4u);
    if (!salt_block) return false;

    if (salt_len > 0 && salt) {
        memcpy(salt_block, salt, salt_len);
    }

    uint8_t u[32];
    uint8_t t[32];
    size_t out_offset = 0;

    for (uint32_t block_index = 1; block_index <= (uint32_t)block_count; block_index++) {
        salt_block[salt_len] = (uint8_t)(block_index >> 24);
        salt_block[salt_len + 1] = (uint8_t)(block_index >> 16);
        salt_block[salt_len + 2] = (uint8_t)(block_index >> 8);
        salt_block[salt_len + 3] = (uint8_t)(block_index);

        crypto_hmac_sha256(password, password_len, salt_block, salt_len + 4u, u);
        memcpy(t, u, sizeof(t));

        for (uint32_t iter = 1; iter < iterations; iter++) {
            crypto_hmac_sha256(password, password_len, u, sizeof(u), u);
            for (size_t i = 0; i < sizeof(t); i++) {
                t[i] ^= u[i];
            }
        }

        size_t take = out_len - out_offset;
        if (take > sizeof(t)) take = sizeof(t);
        memcpy(out + out_offset, t, take);
        out_offset += take;
    }

    free(salt_block);
    return true;
}
