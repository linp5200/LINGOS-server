/**
 * @file    secure_memory.c
 * @brief   锁定内存分配和安全清除
 * @version 2.0.0.0
 */

#include "secure_memory.h"
#include "crypto_core.h"
#include "log_extra.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

void *secure_alloc(size_t size) {
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
    if (ptr == MAP_FAILED) {
        LOG_ERROR_T("SecureMem", "Alloc", "Fail", "mmap failed for size %zu", size);
        return NULL;
    }
    LOG_DEBUG_T("SecureMem", "Alloc", "OK", "size=%zu ptr=%p", size, ptr);
    return ptr;
}

void secure_free(void *ptr, size_t size) {
    if (!ptr) return;
    explicit_bzero(ptr, size);
    munlock(ptr, size);
    munmap(ptr, size);
    LOG_DEBUG_T("SecureMem", "Free", "OK", "size=%zu", size);
}

ssize_t secure_decrypt_field(const uint8_t *ciphertext, size_t ciphertext_len,
                             const uint8_t *key, const uint8_t *nonce, const uint8_t *tag,
                             const char *caller_uid,
                             const char * const authorized_uids[],
                             uint8_t **plaintext_out) {
    if (!caller_uid || !authorized_uids || !plaintext_out) return -1;

    int authorized = 0;
    for (int i = 0; authorized_uids[i] != NULL; i++) {
        if (strcmp(caller_uid, authorized_uids[i]) == 0) {
            authorized = 1;
            break;
        }
    }
    if (!authorized) {
        LOG_WARN_T("SecureMem", "Decrypt", "Unauthorized", "caller=%s", caller_uid);
        return -1;
    }

    uint8_t *plain = secure_alloc(ciphertext_len);
    if (!plain) return -1;

    if (crypto_aead_decrypt(plain, ciphertext, ciphertext_len, key, nonce, tag) != 0) {
        LOG_ERROR_T("SecureMem", "Decrypt", "AuthFail", "AEAD decryption failed");
        secure_free(plain, ciphertext_len);
        return -1;
    }

    *plaintext_out = plain;
    LOG_DEBUG_T("SecureMem", "Decrypt", "OK", "len=%zu", ciphertext_len);
    return ciphertext_len;
}