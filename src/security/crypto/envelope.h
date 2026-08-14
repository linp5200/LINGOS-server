/**
 * @file    envelope.h
 * @brief   Envelope encryption: KEK-wrapped DEK, file-level AEAD
 */

#ifndef SECURITY_ENVELOPE_H
#define SECURITY_ENVELOPE_H

#include <stdint.h>
#include <stddef.h>

#define ENVELOPE_DEK_SIZE      32
#define ENVELOPE_KEK_SIZE      32
#define ENVELOPE_NONCE_SIZE    12
#define ENVELOPE_TAG_SIZE      16
#define ENVELOPE_HEADER_SIZE   (ENVELOPE_KEK_SIZE + ENVELOPE_NONCE_SIZE + ENVELOPE_TAG_SIZE)

/**
 * @brief Derive a KEK from a user passphrase using BLAKE2b (simulated).
 *        In production, use Argon2id. Currently uses simple hash.
 * @param passphrase  User secret
 * @param kek         Output KEK (32 bytes)
 */
void envelope_derive_kek(const char *passphrase, uint8_t *kek);

/**
 * @brief Encrypt a file with envelope encryption.
 *        The encrypted file format:
 *          [encrypted DEK (32 bytes)] [nonce (12 bytes)] [AEAD tag (16 bytes)] [ciphertext]
 * @param input_path   Path to plaintext file
 * @param output_path  Path to write ciphertext file
 * @param passphrase   User passphrase (or NULL to use system KEK)
 * @return 0 on success, -1 on failure
 */
int envelope_encrypt_file(const char *input_path, const char *output_path,
                          const char *passphrase);

/**
 * @brief Decrypt a file encrypted with envelope encryption.
 * @param input_path   Ciphertext file path
 * @param output_path   Plaintext output file path
 * @param passphrase    User passphrase
 * @return 0 on success, -1 on failure
 */
int envelope_decrypt_file(const char *input_path, const char *output_path,
                          const char *passphrase);

#endif