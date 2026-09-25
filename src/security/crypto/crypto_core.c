/**
 * @file    src/security/crypto/crypto_core.c
 * @brief   核心密码学原语实现（基于 Monocypher）
 * @version LN-0.5.0
 *
 * 【0.5.0 关键修复】先生 S6 —— 随机源不降级
 *   旧实现：LCG 伪随机 + 硬编码种子 0xDEADBEEF
 *           → 每次启动"随机"序列完全相同 → nonce/密钥可预测 → AEAD 形同虚设
 *   新实现：getrandom(2) → /dev/urandom 兜底 → **全部失败则返回 -1（拒绝）**
 */

#include "crypto_core.h"
#include "../../lib/crypto/monocypher.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

/* ============================================================
 * 安全随机源（先生 S6）
 * ============================================================ */

/* 优先 getrandom(2)（Linux 3.17+，无需打开文件描述符） */
static int rand_getrandom(uint8_t *buf, size_t len) {
#if defined(__linux__) && defined(SYS_getrandom)
    size_t got = 0;
    while (got < len) {
        long r = syscall(SYS_getrandom, buf + got, len - got, 0 /* flags=0：阻塞直到熵可用 */);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
#else
    (void)buf; (void)len;
    return -1;
#endif
}

/* 兜底：/dev/urandom（仍为内核 CSPRNG，安全） */
static int rand_urandom(uint8_t *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, buf + got, len - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            close(fd);
            return -1;
        }
        got += (size_t)r;
    }
    close(fd);
    return 0;
}

int crypto_random_bytes(uint8_t *buf, size_t len) {
    if (!buf || len == 0) return 0;
    /* ① getrandom（首选） */
    if (rand_getrandom(buf, len) == 0) return 0;
    /* ② /dev/urandom（仍安全：内核 CSPRNG） */
    if (rand_urandom(buf, len) == 0) return 0;
    /* ③ 【先生 S6】不降级 —— 明确失败，由调用方拒绝操作 */
    memset(buf, 0, len);
    return -1;
}

/* 兼容旧签名（仅非安全场景；安全场景必须用 crypto_random_bytes 并检查返回值） */
void crypto_random_bytes_compat(uint8_t *buf, size_t len) {
    (void)crypto_random_bytes(buf, len);
}

/* ============================================================
 * AEAD（XChaCha20-Poly1305）
 * ============================================================ */
int crypto_aead_encrypt(uint8_t *ciphertext, const uint8_t *plaintext, size_t plaintext_len,
                        const uint8_t *key, const uint8_t *nonce,
                        uint8_t *tag) {
    if (!ciphertext || !plaintext || !key || !nonce || !tag) return -1;
    /* crypto_aead_lock 支持原地；此处使用分离缓冲以明确语义 */
    crypto_aead_lock(ciphertext, tag, key, nonce, NULL, 0, plaintext, plaintext_len);
    return 0;
}

int crypto_aead_decrypt(uint8_t *plaintext, const uint8_t *ciphertext, size_t ciphertext_len,
                        const uint8_t *key, const uint8_t *nonce,
                        const uint8_t *tag) {
    if (!plaintext || !ciphertext || !key || !nonce || !tag) return -1;
    /* 认证失败返回非 0 —— 此时 plaintext 内容不可信 */
    return (crypto_aead_unlock(plaintext, tag, key, nonce, NULL, 0,
                               ciphertext, ciphertext_len) == 0) ? 0 : -1;
}

/* ============================================================
 * 哈希
 * ============================================================ */
void crypto_hash(uint8_t *hash, const uint8_t *data, size_t data_len) {
    if (!hash) return;
    crypto_blake2b(hash, CRYPTO_BLAKE2B_HASH_SIZE, data, data_len);
}

void crypto_hash_keyed(uint8_t *hash, size_t hash_size,
                       const uint8_t *key, size_t key_size,
                       const uint8_t *data, size_t data_len) {
    if (!hash || !key) return;
    crypto_blake2b_keyed(hash, hash_size, key, key_size, data, data_len);
}

/* ============================================================
 * Ed25519 签名（S12 更新签名）
 * ============================================================ */
void crypto_sign_keypair(uint8_t *public_key, uint8_t *private_key) {
    if (!public_key || !private_key) return;
    /* 【修复】原实现忽略返回值且参数语义混乱：crypto_eddsa_key_pair 需要 64 字节
     * secret_key = seed(32) || public(32)。先生成 seed 再展开。 */
    uint8_t seed[32];
    if (crypto_random_bytes(seed, sizeof(seed)) != 0) {
        /* 随机源失败 → 不生成密钥（调用方应拒绝继续） */
        memset(public_key, 0, CRYPTO_ED25519_PUBLIC_KEY_SIZE);
        memset(private_key, 0, CRYPTO_ED25519_PRIVATE_KEY_SIZE);
        return;
    }
    uint8_t secret_key[64];
    crypto_eddsa_key_pair(secret_key, public_key, seed);
    memcpy(private_key, secret_key, CRYPTO_ED25519_PRIVATE_KEY_SIZE);
    crypto_wipe(secret_key, sizeof(secret_key));
    crypto_wipe(seed, sizeof(seed));
}

void crypto_sign(uint8_t *signature, const uint8_t *message, size_t message_len,
                 const uint8_t *private_key) {
    if (!signature || !private_key) return;
    /* 【0.7.0-hf2 修正】private_key 为 32 字节 seed（crypto_sign_keypair 存的是
     *   secret_key[64] 的前 32 字节 = seed）——必须先重建 64 字节 secret_key（seed||public）
     *   才能签名。直接传 32 字节给 crypto_eddsa_sign 会越界读 64 字节（危险）。
     *   注：monocypher key_pair 的 seed 参数非 const → 局部拷贝后调用。 */
    uint8_t secret_key[64];
    uint8_t public_key[32];
    uint8_t seed[32];
    memcpy(seed, private_key, 32);
    crypto_eddsa_key_pair(secret_key, public_key, seed);   /* seed → 完整 keypair */
    crypto_eddsa_sign(signature, secret_key, message, message_len);
    crypto_wipe(secret_key, sizeof(secret_key));
    crypto_wipe(public_key, sizeof(public_key));
    crypto_wipe(seed, sizeof(seed));
}

int crypto_verify(const uint8_t *signature, const uint8_t *message, size_t message_len,
                  const uint8_t *public_key) {
    if (!signature || !public_key) return -1;
    return (crypto_eddsa_check(signature, public_key, message, message_len) == 0) ? 0 : -1;
}

/* ============================================================
 * X25519 密钥交换（先生 S1 真加密基础）
 * ============================================================ */
int crypto_x25519_keypair(uint8_t *public_key, uint8_t *secret_key) {
    if (!public_key || !secret_key) return -1;
    if (crypto_random_bytes(secret_key, CRYPTO_X25519_KEY_SIZE) != 0) return -1;
    crypto_x25519_public_key(public_key, secret_key);
    return 0;
}

int crypto_x25519_shared(uint8_t *shared, const uint8_t *my_secret,
                         const uint8_t *their_public) {
    if (!shared || !my_secret || !their_public) return -1;
    crypto_x25519(shared, my_secret, their_public);
    /* 弱公钥检查：全零共享密钥 = 攻击者发送低阶点 → 拒绝
     * 【0.6.0 关键修复】原判断逻辑颠倒（crypto_verify32 相等返回 0）：
     *   原 `!= 0 → 拒绝` 实际拒绝**所有正常握手**、放行**全零弱密钥**——
     *   该代码路径从未被调用过（审计：零调用），测试时才暴露。 */
    uint8_t zero[32];
    memset(zero, 0, sizeof(zero));
    if (crypto_verify32(shared, zero) == 0) {
        crypto_wipe(shared, 32);
        return -1;
    }
    return 0;
}

void crypto_kdf_session_key(uint8_t out[CRYPTO_AEAD_KEY_SIZE],
                            const uint8_t *shared, size_t shared_len,
                            const uint8_t *salt, size_t salt_len,
                            const char *info) {
    /* HKDF-Extract 风格：BLAKE2b_keyed(shared, salt|info) */
    uint8_t ctx[512];
    size_t n = 0;
    if (salt && salt_len) {
        memcpy(ctx + n, salt, salt_len > 200 ? 200 : salt_len);
        n += (salt_len > 200 ? 200 : salt_len);
    }
    if (info) {
        size_t il = strlen(info);
        if (n + il > sizeof(ctx)) il = sizeof(ctx) - n;
        memcpy(ctx + n, info, il);
        n += il;
    }
    crypto_blake2b_keyed(out, CRYPTO_AEAD_KEY_SIZE, shared, shared_len, ctx, n);
    crypto_wipe(ctx, sizeof(ctx));
}

/* ============================================================
 * 恒定时间比较
 * ============================================================ */
int crypto_secure_compare(const void *a, const void *b, size_t len) {
    if (!a || !b) return 0;
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}
