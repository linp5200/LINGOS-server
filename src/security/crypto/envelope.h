/**
 * @file    envelope.h
 * @brief   信封加密：KEK 包裹 DEK + 文件/缓冲级 AEAD
 * @version LN-0.5.0
 *
 * 修复记录（先生 2026-09-12）：
 *   · 原 envelope_derive_kek 直接用 crypto_hash 把 **64 字节**写入
 *     **32 字节** 的 kek 缓冲 → **缓冲区溢出**（严重内存安全缺陷）
 *   · 原实现未检查 crypto_random_bytes 返回值（随机源失败仍继续）
 *   · 原实现无版本/魔数，无法演进
 *   · 先生 S14（按敏感性分级加密）/ S15（备份加密）/ S5（Key 加密）需要可用底座
 *
 * 文件格式（v1）：
 *   [magic(4)="LNEV"] [ver(1)] [flags(1)] [rsv(2)]
 *   [salt(16)] [enc_dek(32)] [dek_nonce(24)] [dek_tag(16)]
 *   [content_nonce(24)] [content_tag(16)] [ciphertext(N)]
 */

#ifndef SECURITY_ENVELOPE_H
#define SECURITY_ENVELOPE_H

#include <stdint.h>
#include <stddef.h>

#define ENVELOPE_MAGIC          "LNEV"
#define ENVELOPE_VERSION        1
#define ENVELOPE_SALT_SIZE      16
#define ENVELOPE_DEK_SIZE       32
#define ENVELOPE_KEK_SIZE       32
#define ENVELOPE_NONCE_SIZE     24   /* XChaCha20 */
#define ENVELOPE_TAG_SIZE       16
#define ENVELOPE_KDF_ROUNDS     20000  /* 简化 KDF 迭代次数（无 Argon2 依赖时的折中）*/

/** 文件头总长（不含密文） */
#define ENVELOPE_HEADER_SIZE (4 + 1 + 1 + 2 + ENVELOPE_SALT_SIZE + \
                              ENVELOPE_DEK_SIZE + ENVELOPE_NONCE_SIZE + ENVELOPE_TAG_SIZE + \
                              ENVELOPE_NONCE_SIZE + ENVELOPE_TAG_SIZE)

/**
 * @brief 从口令派生 KEK（BLAKE2b + salt + 多次迭代）
 * @param passphrase 口令
 * @param salt       盐（≥16 字节）
 * @param salt_len
 * @param kek        输出 32 字节 KEK
 * @return 0 成功；-1 参数错误
 * @note  无 Argon2 依赖时的折中方案：迭代 2 万次 BLAKE2b-keyed。
 *        生产环境建议引入 Argon2id（见 TODO）。
 */
int envelope_derive_kek(const char *passphrase,
                        const uint8_t *salt, size_t salt_len,
                        uint8_t *kek);

/* ============================================================
 * 缓冲级（用于配置 / Key / 小数据 —— S5/S14）
 * ============================================================ */

/**
 * @brief 加密内存缓冲
 * @param out     输出（需调用方分配；建议 *out_len 初始给出容量）
 * @param out_len 出参：实际输出长度
 * @return 0 成功；-1 失败（随机源/内存/参数）
 */
int envelope_encrypt_buffer(const uint8_t *plain, size_t plain_len,
                            const char *passphrase,
                            uint8_t **out, size_t *out_len);

/**
 * @brief 解密内存缓冲
 * @return 0 成功；-1 失败（格式错误/认证失败/口令错误）
 */
int envelope_decrypt_buffer(const uint8_t *in, size_t in_len,
                            const char *passphrase,
                            uint8_t **out, size_t *out_len);

/* ============================================================
 * 文件级（用于备份 —— S15）
 * ============================================================ */

/**
 * @brief 加密文件
 * @param passphrase 口令；NULL → 使用系统密钥（/LINGOS/system/config/.syskey）
 * @return 0 成功；-1 失败
 */
int envelope_encrypt_file(const char *input_path, const char *output_path,
                          const char *passphrase);

/**
 * @brief 解密文件
 * @return 0 成功；-1 失败
 */
int envelope_decrypt_file(const char *input_path, const char *output_path,
                          const char *passphrase);

/**
 * @brief 判断文件是否为本模块加密（魔数检测）
 * @return 1 是；0 不是
 */
int envelope_is_encrypted_file(const char *path);

/* ============================================================
 * 系统密钥（S5：API Key / S14：敏感数据 均可用）
 * ============================================================ */

/**
 * @brief 获取/创建系统密钥（存于 /LINGOS/system/config/.syskey，权限 0600）
 * @param out 输出 32 字节
 * @return 0 成功；-1 失败（随机源不可用）
 */
int envelope_system_key(uint8_t out[ENVELOPE_KEK_SIZE]);

#endif /* SECURITY_ENVELOPE_H */
