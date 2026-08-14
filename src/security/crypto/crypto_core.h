/**
 * @file    crypto_core.h
 * @brief   Core cryptographic primitives using Monocypher
 */

#ifndef SECURITY_CRYPTO_CORE_H
#define SECURITY_CRYPTO_CORE_H

#include <stdint.h>
#include <stddef.h>

/* Key sizes */
#define CRYPTO_AEAD_KEY_SIZE    32
#define CRYPTO_AEAD_NONCE_SIZE  12
#define CRYPTO_AEAD_TAG_SIZE    16
#define CRYPTO_BLAKE2B_HASH_SIZE 64
#define CRYPTO_ED25519_PUBLIC_KEY_SIZE 32
#define CRYPTO_ED25519_PRIVATE_KEY_SIZE 32
#define CRYPTO_ED25519_SIGNATURE_SIZE 64

/**
 * @brief Encrypt/decrypt using AES-256-GCM (simulated via Monocypher's lock/unlock)
 *        Monocypher provides XChaCha20-Poly1305, which we use as AEAD.
 *        We abstract the names for clarity.
 */
int crypto_aead_encrypt(uint8_t *ciphertext, const uint8_t *plaintext, size_t plaintext_len,
                        const uint8_t *key, const uint8_t *nonce,
                        uint8_t *tag);
int crypto_aead_decrypt(uint8_t *plaintext, const uint8_t *ciphertext, size_t ciphertext_len,
                        const uint8_t *key, const uint8_t *nonce,
                        const uint8_t *tag);

/* Hash using BLAKE2b */
void crypto_hash(uint8_t *hash, const uint8_t *data, size_t data_len);

/* Ed25519 sign/verify */
void crypto_sign_keypair(uint8_t *public_key, uint8_t *private_key);
void crypto_sign(uint8_t *signature, const uint8_t *message, size_t message_len,
                 const uint8_t *public_key, const uint8_t *private_key);
int crypto_verify(const uint8_t *signature, const uint8_t *message, size_t message_len,
                  const uint8_t *public_key);

/* Random bytes */
void crypto_random_bytes(uint8_t *buf, size_t len);

#endif