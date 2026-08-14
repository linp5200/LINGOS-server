#ifndef SECURITY_SECURE_MEMORY_H
#define SECURITY_SECURE_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

void *secure_alloc(size_t size);
void secure_free(void *ptr, size_t size);
ssize_t secure_decrypt_field(const uint8_t *ciphertext, size_t ciphertext_len,
                             const uint8_t *key, const uint8_t *nonce, const uint8_t *tag,
                             const char *caller_uid,
                             const char * const authorized_uids[],
                             uint8_t **plaintext_out);

#endif