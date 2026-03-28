#ifndef TABLO_CRYPTO_HASH_H
#define TABLO_CRYPTO_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void crypto_sha256(const uint8_t* data, size_t data_len, uint8_t out[32]);
void crypto_hmac_sha256(const uint8_t* key,
                        size_t key_len,
                        const uint8_t* data,
                        size_t data_len,
                        uint8_t out[32]);
bool crypto_constant_time_equal(const uint8_t* a, const uint8_t* b, size_t len);
bool crypto_hkdf_hmac_sha256(const uint8_t* ikm,
                             size_t ikm_len,
                             const uint8_t* salt,
                             size_t salt_len,
                             const uint8_t* info,
                             size_t info_len,
                             uint8_t* out,
                             size_t out_len);
bool crypto_pbkdf2_hmac_sha256(const uint8_t* password,
                               size_t password_len,
                               const uint8_t* salt,
                               size_t salt_len,
                               uint32_t iterations,
                               uint8_t* out,
                               size_t out_len);

#endif
