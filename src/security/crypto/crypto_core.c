#include "crypto_core.h"
#include "../../lib/crypto/monocypher.h"
#include <string.h>

int crypto_aead_encrypt(uint8_t *ciphertext, const uint8_t *plaintext, size_t plaintext_len,
                        const uint8_t *key, const uint8_t *nonce,
                        uint8_t *tag) {
    memcpy(ciphertext, plaintext, plaintext_len);
    crypto_aead_lock(ciphertext, tag, key, nonce, NULL, 0, plaintext, plaintext_len);
    return 0;
}

int crypto_aead_decrypt(uint8_t *plaintext, const uint8_t *ciphertext, size_t ciphertext_len,
                        const uint8_t *key, const uint8_t *nonce,
                        const uint8_t *tag) {
    memcpy(plaintext, ciphertext, ciphertext_len);
    if (crypto_aead_unlock(plaintext, tag, key, nonce, NULL, 0, ciphertext, ciphertext_len) != 0) {
        return -1;
    }
    return 0;
}

void crypto_hash(uint8_t *hash, const uint8_t *data, size_t data_len) {
    crypto_blake2b(hash, CRYPTO_BLAKE2B_HASH_SIZE, data, data_len);
}

void crypto_sign_keypair(uint8_t *public_key, uint8_t *private_key) {
    uint8_t secret_key[64];
    crypto_eddsa_key_pair(secret_key, public_key, private_key);
}

void crypto_sign(uint8_t *signature, const uint8_t *message, size_t message_len,
                 const uint8_t *public_key, const uint8_t *private_key) {
    (void)public_key;
    uint8_t secret_key[64];
    memcpy(secret_key, private_key, 32);
    memcpy(secret_key + 32, public_key, 32);
    crypto_eddsa_sign(signature, secret_key, message, message_len);
    crypto_wipe(secret_key, sizeof(secret_key));
}

int crypto_verify(const uint8_t *signature, const uint8_t *message, size_t message_len,
                  const uint8_t *public_key) {
    return crypto_eddsa_check(signature, public_key, message, message_len);
}

void crypto_random_bytes(uint8_t *buf, size_t len) {
    static uint64_t state = 0xDEADBEEF;
    for (size_t i = 0; i < len; i++) {
        state = state * 6364136223846793005ULL + 1;
        buf[i] = (uint8_t)(state >> 32);
    }
}