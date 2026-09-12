/**
 * @file    src/security/crypto/envelope.c
 * @brief   信封加密实现（KEK 包裹 DEK + 文件/缓冲级 AEAD）
 * @version LN-0.5.0
 *
 * 关键修复（先生 2026-09-12）：
 *   ① 缓冲区溢出：原 envelope_derive_kek 用 crypto_hash（输出 64B）
 *      写入 32B 的 kek → 溢出 32 字节。现改用 **定长输出** 的 KDF。
 *   ② 未检查随机源返回值 → 现全部检查，失败即失败（先生 S6）。
 *   ③ 增加魔数/版本 → 可演进、可识别。
 *   ④ 增加内存加解密 API（配置/Key/小数据）。
 *   ⑤ 增加系统密钥（0600 权限）供 S5/S14 使用。
 */

#include "envelope.h"
#include "crypto_core.h"
#include "../../lib/crypto/monocypher.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>

/* ============================================================
 * KEK 派生（修复溢出：定长输出 + 迭代）
 * ============================================================ */
int envelope_derive_kek(const char *passphrase,
                        const uint8_t *salt, size_t salt_len,
                        uint8_t *kek) {
    if (!passphrase || !kek) return -1;

    /* 首轮：BLAKE2b-keyed(key=password, msg=salt) → 32 字节（定长，不溢出） */
    uint8_t cur[ENVELOPE_KEK_SIZE];
    crypto_hash_keyed(cur, sizeof(cur),
                      (const uint8_t *)passphrase, strlen(passphrase),
                      salt ? salt : (const uint8_t *)"LINGOS-ENV", salt ? salt_len : 10);

    /* 迭代强化（无 Argon2 依赖时的折中 —— 增加暴力破解成本） */
    for (int i = 0; i < ENVELOPE_KDF_ROUNDS; i++) {
        uint8_t next[ENVELOPE_KEK_SIZE];
        crypto_hash_keyed(next, sizeof(next), cur, sizeof(cur), cur, sizeof(cur));
        memcpy(cur, next, sizeof(cur));
    }

    memcpy(kek, cur, ENVELOPE_KEK_SIZE);
    crypto_wipe(cur, sizeof(cur));
    return 0;
}

/* ============================================================
 * 缓冲级加解密
 * ============================================================ */
int envelope_encrypt_buffer(const uint8_t *plain, size_t plain_len,
                            const char *passphrase,
                            uint8_t **out, size_t *out_len) {
    if (!plain || !out || !out_len) return -1;
    *out = NULL;
    *out_len = 0;

    size_t total = ENVELOPE_HEADER_SIZE + plain_len;
    uint8_t *buf = malloc(total);
    if (!buf) return -1;
    memset(buf, 0, ENVELOPE_HEADER_SIZE);

    size_t p = 0;
    /* 魔数 + 版本 */
    memcpy(buf + p, ENVELOPE_MAGIC, 4); p += 4;
    buf[p++] = ENVELOPE_VERSION;
    buf[p++] = 0;                       /* flags（保留） */
    buf[p++] = 0; buf[p++] = 0;          /* reserved */

    /* 盐 */
    uint8_t *salt = buf + p; p += ENVELOPE_SALT_SIZE;
    if (crypto_random_bytes(salt, ENVELOPE_SALT_SIZE) != 0) {
        LOG_ERROR_T("Envelope", "EncryptBuf", "RandFail", "随机源不可用（不降级）");
        free(buf); return -1;
    }

    /* KEK */
    uint8_t kek[ENVELOPE_KEK_SIZE];
    if (envelope_derive_kek(passphrase ? passphrase : "LINGOS_DEFAULT_KEK",
                            salt, ENVELOPE_SALT_SIZE, kek) != 0) {
        free(buf); return -1;
    }

    /* DEK（随机） */
    uint8_t dek[ENVELOPE_DEK_SIZE];
    if (crypto_random_bytes(dek, sizeof(dek)) != 0) {
        crypto_wipe(kek, sizeof(kek));
        free(buf); return -1;
    }

    /* 包裹 DEK */
    uint8_t *enc_dek = buf + p; p += ENVELOPE_DEK_SIZE;
    uint8_t *dek_nonce = buf + p; p += ENVELOPE_NONCE_SIZE;
    uint8_t *dek_tag = buf + p; p += ENVELOPE_TAG_SIZE;
    if (crypto_random_bytes(dek_nonce, ENVELOPE_NONCE_SIZE) != 0) {
        crypto_wipe(kek, sizeof(kek)); crypto_wipe(dek, sizeof(dek));
        free(buf); return -1;
    }
    if (crypto_aead_encrypt(enc_dek, dek, ENVELOPE_DEK_SIZE,
                            kek, dek_nonce, dek_tag) != 0) {
        crypto_wipe(kek, sizeof(kek)); crypto_wipe(dek, sizeof(dek));
        free(buf); return -1;
    }

    /* 加密内容（用 DEK） */
    uint8_t *ct_nonce = buf + p; p += ENVELOPE_NONCE_SIZE;
    uint8_t *ct_tag = buf + p; p += ENVELOPE_TAG_SIZE;
    if (crypto_random_bytes(ct_nonce, ENVELOPE_NONCE_SIZE) != 0) {
        crypto_wipe(kek, sizeof(kek)); crypto_wipe(dek, sizeof(dek));
        free(buf); return -1;
    }
    if (crypto_aead_encrypt(buf + p, plain, plain_len, dek, ct_nonce, ct_tag) != 0) {
        crypto_wipe(kek, sizeof(kek)); crypto_wipe(dek, sizeof(dek));
        free(buf); return -1;
    }

    crypto_wipe(kek, sizeof(kek));
    crypto_wipe(dek, sizeof(dek));

    *out = buf;
    *out_len = total;
    return 0;
}

int envelope_decrypt_buffer(const uint8_t *in, size_t in_len,
                            const char *passphrase,
                            uint8_t **out, size_t *out_len) {
    if (!in || !out || !out_len) return -1;
    *out = NULL; *out_len = 0;
    if (in_len < ENVELOPE_HEADER_SIZE) return -1;
    if (memcmp(in, ENVELOPE_MAGIC, 4) != 0) return -1;
    if (in[4] != ENVELOPE_VERSION) {
        LOG_WARN_T("Envelope", "DecryptBuf", "BadVersion", "ver=%d", in[4]);
        return -1;
    }

    size_t p = 8;
    const uint8_t *salt = in + p; p += ENVELOPE_SALT_SIZE;
    const uint8_t *enc_dek = in + p; p += ENVELOPE_DEK_SIZE;
    const uint8_t *dek_nonce = in + p; p += ENVELOPE_NONCE_SIZE;
    const uint8_t *dek_tag = in + p; p += ENVELOPE_TAG_SIZE;
    const uint8_t *ct_nonce = in + p; p += ENVELOPE_NONCE_SIZE;
    const uint8_t *ct_tag = in + p; p += ENVELOPE_TAG_SIZE;

    uint8_t kek[ENVELOPE_KEK_SIZE];
    if (envelope_derive_kek(passphrase ? passphrase : "LINGOS_DEFAULT_KEK",
                            salt, ENVELOPE_SALT_SIZE, kek) != 0) return -1;

    uint8_t dek[ENVELOPE_DEK_SIZE];
    if (crypto_aead_decrypt(dek, enc_dek, ENVELOPE_DEK_SIZE,
                            kek, dek_nonce, dek_tag) != 0) {
        crypto_wipe(kek, sizeof(kek));
        LOG_WARN_T("Envelope", "DecryptBuf", "DekUnwrapFail", "口令错误或数据损坏");
        return -1;
    }
    crypto_wipe(kek, sizeof(kek));

    size_t ct_len = in_len - ENVELOPE_HEADER_SIZE;
    uint8_t *plain = malloc(ct_len + 1);
    if (!plain) { crypto_wipe(dek, sizeof(dek)); return -1; }
    if (crypto_aead_decrypt(plain, in + p, ct_len, dek, ct_nonce, ct_tag) != 0) {
        crypto_wipe(dek, sizeof(dek));
        free(plain);
        LOG_WARN_T("Envelope", "DecryptBuf", "AuthFail", "内容认证失败");
        return -1;
    }
    crypto_wipe(dek, sizeof(dek));
    plain[ct_len] = '\0';
    *out = plain;
    *out_len = ct_len;
    return 0;
}

/* ============================================================
 * 文件级
 * ============================================================ */
int envelope_encrypt_file(const char *input_path, const char *output_path,
                          const char *passphrase) {
    if (!input_path || !output_path) return -1;
    FILE *fin = fopen(input_path, "rb");
    if (!fin) return -1;
    if (fseek(fin, 0, SEEK_END) != 0) { fclose(fin); return -1; }
    long plain_len = ftell(fin);
    if (plain_len < 0) { fclose(fin); return -1; }
    if (fseek(fin, 0, SEEK_SET) != 0) { fclose(fin); return -1; }

    uint8_t *plain = malloc((size_t)plain_len + 1);
    if (!plain) { fclose(fin); return -1; }
    size_t rd = fread(plain, 1, (size_t)plain_len, fin);
    fclose(fin);
    if (rd != (size_t)plain_len) { free(plain); return -1; }

    uint8_t *enc = NULL; size_t enc_len = 0;
    int rc = envelope_encrypt_buffer(plain, (size_t)plain_len, passphrase, &enc, &enc_len);
    crypto_wipe(plain, (size_t)plain_len);
    free(plain);
    if (rc != 0) return -1;

    /* 输出文件权限 0600（敏感数据） */
    int fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { free(enc); return -1; }
    ssize_t w = write(fd, enc, enc_len);
    close(fd);
    crypto_wipe(enc, enc_len);
    free(enc);
    return (w == (ssize_t)enc_len) ? 0 : -1;
}

int envelope_decrypt_file(const char *input_path, const char *output_path,
                          const char *passphrase) {
    if (!input_path || !output_path) return -1;
    FILE *fin = fopen(input_path, "rb");
    if (!fin) return -1;
    if (fseek(fin, 0, SEEK_END) != 0) { fclose(fin); return -1; }
    long in_len = ftell(fin);
    if (in_len < 0) { fclose(fin); return -1; }
    if (fseek(fin, 0, SEEK_SET) != 0) { fclose(fin); return -1; }

    uint8_t *buf = malloc((size_t)in_len);
    if (!buf) { fclose(fin); return -1; }
    size_t rd = fread(buf, 1, (size_t)in_len, fin);
    fclose(fin);
    if (rd != (size_t)in_len) { free(buf); return -1; }

    uint8_t *plain = NULL; size_t plain_len = 0;
    int rc = envelope_decrypt_buffer(buf, (size_t)in_len, passphrase, &plain, &plain_len);
    free(buf);
    if (rc != 0) return -1;

    int fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { free(plain); return -1; }
    ssize_t w = write(fd, plain, plain_len);
    close(fd);
    crypto_wipe(plain, plain_len);
    free(plain);
    return (w == (ssize_t)plain_len) ? 0 : -1;
}

int envelope_is_encrypted_file(const char *path) {
    if (!path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char m[4] = {0};
    size_t r = fread(m, 1, 4, f);
    fclose(f);
    return (r == 4 && memcmp(m, ENVELOPE_MAGIC, 4) == 0) ? 1 : 0;
}

/* ============================================================
 * 系统密钥（0600，首次自动生成）
 * ============================================================ */
int envelope_system_key(uint8_t out[ENVELOPE_KEK_SIZE]) {
    if (!out) return -1;
    const char *keypath = "/LINGOS/system/config/.syskey";

    /* 已存在 → 读取 */
    FILE *f = fopen(keypath, "rb");
    if (f) {
        size_t r = fread(out, 1, ENVELOPE_KEK_SIZE, f);
        fclose(f);
        if (r == ENVELOPE_KEK_SIZE) return 0;
        LOG_WARN_T("Envelope", "SysKey", "Truncated", "系统密钥文件损坏，将重建");
    }

    /* 生成新密钥 */
    if (crypto_random_bytes(out, ENVELOPE_KEK_SIZE) != 0) {
        LOG_ERROR_T("Envelope", "SysKey", "RandFail", "随机源不可用 → 拒绝生成（不降级）");
        return -1;
    }
    int fd = open(keypath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        LOG_ERROR_T("Envelope", "SysKey", "WriteFail", "%s: %s", keypath, strerror(errno));
        return -1;
    }
    ssize_t w = write(fd, out, ENVELOPE_KEK_SIZE);
    close(fd);
    if (w != ENVELOPE_KEK_SIZE) {
        LOG_ERROR_T("Envelope", "SysKey", "ShortWrite", "%zd", w);
        return -1;
    }
    LOG_INFO_T("Envelope", "SysKey", "Created", "%s (0600)", keypath);
    return 0;
}
