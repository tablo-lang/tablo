#ifndef TABLO_CRYPTO_AES_H
#define TABLO_CRYPTO_AES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool crypto_aes_ctr(const uint8_t* key,
                    size_t key_len,
                    const uint8_t counter[16],
                    const uint8_t* input,
                    size_t input_len,
                    uint8_t* out);
bool crypto_aes_gcm_seal(const uint8_t* key,
                         size_t key_len,
                         const uint8_t nonce[12],
                         const uint8_t* aad,
                         size_t aad_len,
                         const uint8_t* plaintext,
                         size_t plaintext_len,
                         uint8_t* out_ciphertext,
                         uint8_t out_tag[16]);
bool crypto_aes_gcm_open(const uint8_t* key,
                         size_t key_len,
                         const uint8_t nonce[12],
                         const uint8_t* aad,
                         size_t aad_len,
                         const uint8_t* ciphertext,
                         size_t ciphertext_len,
                         const uint8_t tag[16],
                         uint8_t* out_plaintext);

#endif
