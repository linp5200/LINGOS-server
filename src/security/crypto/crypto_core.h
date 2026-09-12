/**
 * @file    src/security/crypto/crypto_core.h
 * @brief   核心密码学原语（基于 Monocypher）
 * @version LN-0.5.0
 *
 * 修复记录（先生 2026-09-12 · S6「随机源不降级」）：
 *   原 crypto_random_bytes 使用 LCG 伪随机 + 硬编码种子 0xDEADBEEF，
 *   导致**每次启动生成完全相同的"随机"序列** → 密钥/IV/nonce 可预测。
 *   现改为系统真随机源（getrandom/urandom），**失败即返回错误，绝不降级**。
 */

#ifndef SECURITY_CRYPTO_CORE_H
#define SECURITY_CRYPTO_CORE_H

#include <stdint.h>
#include <stddef.h>

/* Key sizes */
#define CRYPTO_AEAD_KEY_SIZE    32
#define CRYPTO_AEAD_NONCE_SIZE  24   /* XChaCha20：24 字节 nonce（原 12 是 IETF 变体）*/
#define CRYPTO_AEAD_TAG_SIZE    16
#define CRYPTO_BLAKE2B_HASH_SIZE 64
#define CRYPTO_ED25519_PUBLIC_KEY_SIZE 32
#define CRYPTO_ED25519_PRIVATE_KEY_SIZE 32
#define CRYPTO_ED25519_SIGNATURE_SIZE 64
/* X25519 密钥交换（先生 S1 真加密用） */
#define CRYPTO_X25519_KEY_SIZE   32

/**
 * @brief AEAD 加密（XChaCha20-Poly1305，Monocypher crypto_aead_lock）
 * @note  密文与明文缓冲**不可重叠**（本函数内部处理）
 */
int crypto_aead_encrypt(uint8_t *ciphertext, const uint8_t *plaintext, size_t plaintext_len,
                        const uint8_t *key, const uint8_t *nonce,
                        uint8_t *tag);

/**
 * @brief AEAD 解密（失败返回 -1 —— 认证失败，切勿使用输出）
 */
int crypto_aead_decrypt(uint8_t *plaintext, const uint8_t *ciphertext, size_t ciphertext_len,
                        const uint8_t *key, const uint8_t *nonce,
                        const uint8_t *tag);

/* Hash using BLAKE2b */
void crypto_hash(uint8_t *hash, const uint8_t *data, size_t data_len);

/* 带密钥的哈希（用于 KEK 派生） */
void crypto_hash_keyed(uint8_t *hash, size_t hash_size,
                       const uint8_t *key, size_t key_size,
                       const uint8_t *data, size_t data_len);

/* Ed25519 签名（先生 S12 更新签名用） */
void crypto_sign_keypair(uint8_t *public_key, uint8_t *private_key);
void crypto_sign(uint8_t *signature, const uint8_t *message, size_t message_len,
                 const uint8_t *private_key);
int  crypto_verify(const uint8_t *signature, const uint8_t *message, size_t message_len,
                   const uint8_t *public_key);

/* ============================================================
 * X25519 密钥交换（先生 S1「真加密」的基础）
 * ============================================================ */

/**
 * @brief 生成 X25519 密钥对
 * @return 0 成功；-1 随机源不可用（**拒绝，不降级**）
 */
int crypto_x25519_keypair(uint8_t *public_key, uint8_t *secret_key);

/**
 * @brief 计算共享密钥（发送方）
 * @return 0 成功；-1 失败（弱公钥/全零共享密钥）
 */
int crypto_x25519_shared(uint8_t *shared, const uint8_t *my_secret,
                         const uint8_t *their_public);

/**
 * @brief 从共享密钥 + 双方公钥 + salt 派生会话密钥（HKDF 风格，BLAKE2b 实现）
 */
void crypto_kdf_session_key(uint8_t out[CRYPTO_AEAD_KEY_SIZE],
                            const uint8_t *shared, size_t shared_len,
                            const uint8_t *salt, size_t salt_len,
                            const char *info);

/* ============================================================
 * 随机数（先生 S6：失败即拒绝，不降级）
 * ============================================================ */

/**
 * @brief 填充安全随机字节
 * @return 0 成功；-1 失败（**调用方必须处理失败，不得继续用弱随机**）
 */
int crypto_random_bytes(uint8_t *buf, size_t len);

/**
 * @brief 兼容旧签名的包装（失败时静默返回，仅用于非安全场景）
 * @deprecated 安全场景请用 crypto_random_bytes()
 */
void crypto_random_bytes_compat(uint8_t *buf, size_t len);

/**
 * @brief 安全比较（恒定时间），防时序侧信道
 * @return 1 相等；0 不等
 */
int crypto_secure_compare(const void *a, const void *b, size_t len);

#endif
