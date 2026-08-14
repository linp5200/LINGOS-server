#include "envelope.h"
#include "crypto_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void envelope_derive_kek(const char *passphrase, uint8_t *kek) {
    /* Simulate key derivation: hash the passphrase.
       In production, replace with Argon2id. */
    crypto_hash(kek, (const uint8_t*)passphrase, strlen(passphrase));
    /* Truncate to 32 bytes (already BLAKE2b outputs 64, but we take first 32) */
    (void)passphrase;
}

static int generate_dek(uint8_t *dek) {
    crypto_random_bytes(dek, ENVELOPE_DEK_SIZE);
    return 0;
}

int envelope_encrypt_file(const char *input_path, const char *output_path,
                          const char *passphrase) {
    /* Read input file */
    FILE *fin = fopen(input_path, "rb");
    if (!fin) return -1;
    fseek(fin, 0, SEEK_END);
    long plain_len = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    uint8_t *plain = malloc(plain_len);
    if (!plain) { fclose(fin); return -1; }
    fread(plain, 1, plain_len, fin);
    fclose(fin);

    /* Generate DEK */
    uint8_t dek[ENVELOPE_DEK_SIZE];
    generate_dek(dek);

    /* Derive KEK */
    uint8_t kek[ENVELOPE_KEK_SIZE];
    envelope_derive_kek(passphrase ? passphrase : "LINGOS_DEFAULT_KEK", kek);

    /* Encrypt DEK with KEK */
    uint8_t nonce[ENVELOPE_NONCE_SIZE];
    crypto_random_bytes(nonce, sizeof(nonce));
    uint8_t tag[ENVELOPE_TAG_SIZE];
    uint8_t encrypted_dek[ENVELOPE_DEK_SIZE];

    crypto_aead_encrypt(encrypted_dek, dek, sizeof(dek), kek, nonce, tag);

    /* Encrypt plaintext with DEK */
    uint8_t content_nonce[ENVELOPE_NONCE_SIZE];
    crypto_random_bytes(content_nonce, sizeof(content_nonce));
    uint8_t content_tag[ENVELOPE_TAG_SIZE];
    uint8_t *ciphertext = malloc(plain_len);
    crypto_aead_encrypt(ciphertext, plain, plain_len, dek, content_nonce, content_tag);

    /* Write output */
    FILE *fout = fopen(output_path, "wb");
    if (!fout) { free(plain); free(ciphertext); return -1; }
    fwrite(encrypted_dek, 1, sizeof(encrypted_dek), fout);
    fwrite(nonce, 1, sizeof(nonce), fout);
    fwrite(tag, 1, sizeof(tag), fout);
    fwrite(content_nonce, 1, sizeof(content_nonce), fout);
    fwrite(content_tag, 1, sizeof(content_tag), fout);
    fwrite(ciphertext, 1, plain_len, fout);
    fclose(fout);

    free(plain);
    free(ciphertext);
    return 0;
}

int envelope_decrypt_file(const char *input_path, const char *output_path,
                          const char *passphrase) {
    FILE *fin = fopen(input_path, "rb");
    if (!fin) return -1;

    /* Read header */
    uint8_t encrypted_dek[ENVELOPE_DEK_SIZE];
    uint8_t nonce[ENVELOPE_NONCE_SIZE];
    uint8_t tag[ENVELOPE_TAG_SIZE];
    uint8_t content_nonce[ENVELOPE_NONCE_SIZE];
    uint8_t content_tag[ENVELOPE_TAG_SIZE];

    fread(encrypted_dek, 1, sizeof(encrypted_dek), fin);
    fread(nonce, 1, sizeof(nonce), fin);
    fread(tag, 1, sizeof(tag), fin);
    fread(content_nonce, 1, sizeof(content_nonce), fin);
    fread(content_tag, 1, sizeof(content_tag), fin);

    /* Derive KEK */
    uint8_t kek[ENVELOPE_KEK_SIZE];
    envelope_derive_kek(passphrase ? passphrase : "LINGOS_DEFAULT_KEK", kek);

    /* Decrypt DEK */
    uint8_t dek[ENVELOPE_DEK_SIZE];
    if (crypto_aead_decrypt(dek, encrypted_dek, sizeof(dek), kek, nonce, tag) != 0) {
        fclose(fin);
        return -1; /* authentication failed */
    }

    /* Read ciphertext */
    fseek(fin, 0, SEEK_END);
    long total_len = ftell(fin);
    long cipher_len = total_len - (sizeof(encrypted_dek) + sizeof(nonce) + sizeof(tag)
                                   + sizeof(content_nonce) + sizeof(content_tag));
    fseek(fin, sizeof(encrypted_dek) + sizeof(nonce) + sizeof(tag)
                + sizeof(content_nonce) + sizeof(content_tag), SEEK_SET);
    uint8_t *ciphertext = malloc(cipher_len);
    fread(ciphertext, 1, cipher_len, fin);
    fclose(fin);

    /* Decrypt */
    uint8_t *plaintext = malloc(cipher_len);
    if (crypto_aead_decrypt(plaintext, ciphertext, cipher_len, dek, content_nonce, content_tag) != 0) {
        free(ciphertext); free(plaintext); return -1;
    }

    FILE *fout = fopen(output_path, "wb");
    if (!fout) { free(ciphertext); free(plaintext); return -1; }
    fwrite(plaintext, 1, cipher_len, fout);
    fclose(fout);

    free(ciphertext);
    free(plaintext);
    return 0;
}