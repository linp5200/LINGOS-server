/**
 * @file    src/update/update_verify.c
 * @brief   更新包签名校验实现（Ed25519）
 * @version LN-0.5.0
 */

#include "update_verify.h"
#include "../security/crypto/crypto_core.h"
#include "../lib/crypto/monocypher.h"
#include "../lib/log_extra.h"
#include "../common/safe_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PUBKEY_PATH "/LINGOS/system/config/update_pubkey"

/* ============================================================
 * 内部：读整个文件
 * ============================================================ */
static int read_all(const char *path, uint8_t **out, size_t *out_len) {
    if (!path || !out || !out_len) return -1;
    *out = NULL; *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return -1; }
    buf[sz] = '\0';
    *out = buf;
    *out_len = (size_t)sz;
    return 0;
}

/* ============================================================
 * 公钥
 * ============================================================ */
int update_load_pubkey(uint8_t out[UPDATE_PUBKEY_SIZE]) {
    if (!out) return -1;
    FILE *f = fopen(DEFAULT_PUBKEY_PATH, "rb");
    if (!f) {
        LOG_WARN_T("UpdateVerify", "LoadPubkey", "Missing",
                   "%s 不存在（未配置更新公钥）", DEFAULT_PUBKEY_PATH);
        return -1;
    }
    size_t r = fread(out, 1, UPDATE_PUBKEY_SIZE, f);
    fclose(f);
    if (r != UPDATE_PUBKEY_SIZE) {
        LOG_WARN_T("UpdateVerify", "LoadPubkey", "BadLen", "%zu != %d", r, UPDATE_PUBKEY_SIZE);
        return -1;
    }
    return 0;
}

/* ============================================================
 * 校验
 * ============================================================ */
int update_verify_buffer(const uint8_t *data, size_t data_len,
                         const uint8_t *sig, size_t sig_len,
                         const uint8_t *pubkey, size_t pubkey_len) {
    if (!data || !sig || !pubkey) return -1;
    if (sig_len != UPDATE_SIG_SIZE || pubkey_len != UPDATE_PUBKEY_SIZE) return -1;
    /* crypto_verify: 0=通过；Monocypher eddsa_check 返回 0 表示有效 */
    return (crypto_verify(sig, data, data_len, pubkey) == 0) ? 0 : -1;
}

int update_verify_file(const char *file_path, const char *sig_path,
                       const char *pubkey_path) {
    if (!file_path || !sig_path) return -1;

    uint8_t *data = NULL; size_t data_len = 0;
    if (read_all(file_path, &data, &data_len) != 0) {
        LOG_WARN_T("UpdateVerify", "VerifyFile", "NoFile", "%s", file_path);
        return -1;
    }

    uint8_t *sig = NULL; size_t sig_len = 0;
    if (read_all(sig_path, &sig, &sig_len) != 0) {
        free(data);
        LOG_WARN_T("UpdateVerify", "VerifyFile", "NoSig", "%s", sig_path);
        return -1;
    }

    uint8_t pub[UPDATE_PUBKEY_SIZE];
    if (pubkey_path) {
        uint8_t *pk = NULL; size_t pkl = 0;
        if (read_all(pubkey_path, &pk, &pkl) != 0 || pkl < UPDATE_PUBKEY_SIZE) {
            free(data); free(sig);
            if (pk) free(pk);
            return -1;
        }
        memcpy(pub, pk, UPDATE_PUBKEY_SIZE);
        free(pk);
    } else {
        if (update_load_pubkey(pub) != 0) {
            free(data); free(sig);
            return -1;
        }
    }

    int rc = update_verify_buffer(data, data_len, sig, sig_len, pub, sizeof(pub));
    free(data);
    free(sig);

    if (rc == 0)
        LOG_INFO_T("UpdateVerify", "VerifyFile", "OK", "%s 签名有效", file_path);
    else
        LOG_ERROR_T("UpdateVerify", "VerifyFile", "Invalid",
                    "%s 签名无效 —— 拒绝应用更新", file_path);
    return rc;
}

/* ============================================================
 * 签发（发布工具；运行时不用）
 * ============================================================ */
int update_sign_file(const char *file_path, const char *privkey_path,
                     const char *sig_out) {
    if (!file_path || !privkey_path || !sig_out) return -1;

    uint8_t *data = NULL; size_t data_len = 0;
    if (read_all(file_path, &data, &data_len) != 0) return -1;

    uint8_t priv[32];
    FILE *f = fopen(privkey_path, "rb");
    if (!f) { free(data); return -1; }
    size_t r = fread(priv, 1, 32, f);
    fclose(f);
    if (r != 32) { free(data); return -1; }

    uint8_t sig[UPDATE_SIG_SIZE];
    crypto_sign(sig, data, data_len, priv);
    crypto_wipe(priv, sizeof(priv));
    free(data);

    FILE *o = fopen(sig_out, "wb");
    if (!o) return -1;
    size_t w = fwrite(sig, 1, sizeof(sig), o);
    fclose(o);
    return (w == sizeof(sig)) ? 0 : -1;
}

/* ============================================================
 * 更新前置门（受 option 控制）
 * ============================================================ */
int update_signature_gate(const char *manifest_path) {
    if (!manifest_path) return 1;   /* 无 manifest → 交由其他检查 */

    /* 选项：sec.update_signature（默认开） */
    extern int options_get(const char *key);
    int enforce = options_get("sec.update_signature");
    if (enforce < 0) enforce = 1;   /* 选项系统不可用 → 按默认（开启）处理 */

    /* 查找签名文件：<manifest>.sig */
    char sig_path[1024];
    safe_snprintf(sig_path, sizeof(sig_path), "%s.sig", manifest_path);

    if (access(sig_path, F_OK) != 0) {
        if (enforce) {
            LOG_ERROR_T("UpdateVerify", "Gate", "NoSignature",
                        "%s 无签名文件 —— 拒绝应用更新（可关闭 sec.update_signature 放行）",
                        manifest_path);
            return 0;
        }
        LOG_WARN_T("UpdateVerify", "Gate", "Unsigned",
                   "%s 无签名（校验已关闭，放行但风险自负）", manifest_path);
        return 1;
    }

    if (update_verify_file(manifest_path, sig_path, NULL) == 0) return 1;

    if (enforce) {
        LOG_ERROR_T("UpdateVerify", "Gate", "BadSignature",
                    "%s 签名校验失败 —— 拒绝应用更新", manifest_path);
        return 0;
    }
    LOG_WARN_T("UpdateVerify", "Gate", "BadSignatureLenient",
               "%s 签名无效（校验已关闭，放行）", manifest_path);
    return 1;
}
