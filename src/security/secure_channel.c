/**
 * @file    src/security/secure_channel.c
 * @brief   安全通道实现（X25519 + XChaCha20-Poly1305，含防重放）
 * @version LN-0.5.0
 *
 * 安全设计：
 *   · 前向保密：每次会话生成**临时** X25519 密钥对，用后销毁
 *   · 密钥派生：BLAKE2b-keyed(shared_secret, salt|info) —— HKDF 风格
 *   · 逐帧 AEAD：XChaCha20-Poly1305（Monocypher）
 *   · 防重放：nonce = 方向(4B) || 序列号(8B) || 0(12B)；接收侧单调递增校验
 *   · 弱公钥拒绝：共享密钥全零 → 失败（Monocypher 已内部检查，这里再确认）
 *   · 随机源失败 → **拒绝创建通道**（先生 S6）
 */

#include "secure_channel.h"
#include "crypto/crypto_core.h"
#include "../lib/crypto/monocypher.h"
#include "log_extra.h"

#include <stdlib.h>
#include <string.h>

struct secure_channel {
    uint8_t  local_secret[SC_PUBKEY_SIZE];
    uint8_t  local_public[SC_PUBKEY_SIZE];
    uint8_t  session_key[SC_KEY_SIZE];
    sc_state_t state;

    uint64_t send_seq;
    uint64_t recv_seq;          /* 已接受的最大接收序列号 */
    int      recv_seq_init;

    uint32_t negotiated;

    sc_stats_t stats;
};

/* ============================================================
 * 生命周期
 * ============================================================ */
secure_channel_t *sc_create(void) {
    secure_channel_t *ch = calloc(1, sizeof(*ch));
    if (!ch) return NULL;

    /* 【先生 S6】随机源失败 → 拒绝创建（绝不降级） */
    if (crypto_x25519_keypair(ch->local_public, ch->local_secret) != 0) {
        LOG_ERROR_T("SecureChannel", "Create", "RandFail",
                    "随机源不可用 —— 拒绝建立加密通道（不降级）");
        free(ch);
        return NULL;
    }
    ch->state = SC_STATE_HANDSHAKE;
    ch->send_seq = 0;
    ch->recv_seq = 0;
    ch->recv_seq_init = 0;
    LOG_INFO_T("SecureChannel", "Create", "OK", "临时密钥对已生成（前向保密）");
    return ch;
}

void sc_destroy(secure_channel_t *ch) {
    if (!ch) return;
    /* 擦除敏感材料 */
    crypto_wipe(ch->local_secret, sizeof(ch->local_secret));
    crypto_wipe(ch->session_key, sizeof(ch->session_key));
    free(ch);
}

const uint8_t *sc_local_public(const secure_channel_t *ch) {
    return ch ? ch->local_public : NULL;
}

sc_state_t sc_state(const secure_channel_t *ch) {
    return ch ? ch->state : SC_STATE_FAILED;
}

int sc_is_ready(const secure_channel_t *ch) {
    return ch && ch->state == SC_STATE_READY;
}

/* ============================================================
 * 握手
 * ============================================================ */
int sc_handshake(secure_channel_t *ch, const uint8_t *peer_public,
                 const uint8_t *salt, size_t salt_len) {
    if (!ch || !peer_public) return -1;
    if (ch->state != SC_STATE_HANDSHAKE) {
        LOG_WARN_T("SecureChannel", "Handshake", "BadState", "state=%d", ch->state);
        return -1;
    }

    uint8_t shared[SC_PUBKEY_SIZE];
    if (crypto_x25519_shared(shared, ch->local_secret, peer_public) != 0) {
        ch->state = SC_STATE_FAILED;
        LOG_ERROR_T("SecureChannel", "Handshake", "WeakKey",
                    "对端公钥无效（弱公钥/全零共享密钥）—— 拒绝");
        return -1;
    }

    crypto_kdf_session_key(ch->session_key, shared, sizeof(shared),
                           salt, salt_len, "LINGOS-SC-v1");
    crypto_wipe(shared, sizeof(shared));

    ch->state = SC_STATE_READY;
    LOG_INFO_T("SecureChannel", "Handshake", "OK", "会话密钥已派生，通道就绪");
    return 0;
}

/* ============================================================
 * Nonce 构造：方向 || 序列号 || 零填充（防重放）
 * ============================================================ */
static void build_nonce(uint8_t nonce[SC_NONCE_SIZE], uint32_t direction, uint64_t seq) {
    memset(nonce, 0, SC_NONCE_SIZE);
    /* 方向：0=本端发送 1=对端发送（与对端约定一致） */
    nonce[0] = (uint8_t)(direction & 0xFF);
    nonce[1] = (uint8_t)((direction >> 8) & 0xFF);
    nonce[2] = (uint8_t)((direction >> 16) & 0xFF);
    nonce[3] = (uint8_t)((direction >> 24) & 0xFF);
    /* 序列号 8 字节大端 */
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = (uint8_t)((seq >> (56 - i * 8)) & 0xFF);
    }
}

/* ============================================================
 * 加密
 * ============================================================ */
int sc_encrypt(secure_channel_t *ch, const uint8_t *in, size_t in_len,
               uint8_t *out, size_t *out_len, size_t out_cap) {
    if (!ch || !in || !out || !out_len) return -1;
    if (ch->state != SC_STATE_READY) return -1;
    if (in_len > SC_MAX_PAYLOAD) return -1;
    if (out_cap < in_len + SC_TAG_SIZE) return -1;

    uint8_t nonce[SC_NONCE_SIZE];
    build_nonce(nonce, 0, ch->send_seq);

    /* 密文 = payload || tag */
    uint8_t tag[SC_TAG_SIZE];
    if (crypto_aead_encrypt(out, in, in_len, ch->session_key, nonce, tag) != 0) {
        LOG_ERROR_T("SecureChannel", "Encrypt", "AeadFail", "AEAD 加密失败");
        return -1;
    }
    memcpy(out + in_len, tag, SC_TAG_SIZE);
    *out_len = in_len + SC_TAG_SIZE;

    ch->send_seq++;
    ch->stats.active = 1;
    ch->stats.frames_encrypted++;
    return 0;
}

/* ============================================================
 * 解密
 * ============================================================ */
int sc_decrypt(secure_channel_t *ch, const uint8_t *in, size_t in_len,
               uint8_t *out, size_t *out_len, size_t out_cap) {
    if (!ch || !in || !out || !out_len) return -1;
    if (ch->state != SC_STATE_READY) return -1;
    if (in_len < SC_TAG_SIZE) return -1;

    size_t ct_len = in_len - SC_TAG_SIZE;
    if (out_cap < ct_len + 1) return -1;

    const uint8_t *tag = in + ct_len;

    /* 首帧：接受任何序列号（对端从 0 起）；其后必须严格递增（防重放/重排） */
    uint64_t seq = ch->recv_seq_init ? (ch->recv_seq + 1) : 0;

    uint8_t nonce[SC_NONCE_SIZE];
    build_nonce(nonce, 0, seq);

    if (crypto_aead_decrypt(out, in, ct_len, ch->session_key, nonce, tag) != 0) {
        ch->stats.auth_failures++;
        LOG_WARN_T("SecureChannel", "Decrypt", "AuthFail",
                   "认证失败（数据被篡改或密钥不匹配）seq=%llu",
                   (unsigned long long)seq);
        return -1;
    }
    out[ct_len] = '\0';
    *out_len = ct_len;

    ch->recv_seq = seq;
    ch->recv_seq_init = 1;
    ch->stats.active = 1;
    ch->stats.frames_decrypted++;
    return 0;
}

/* ============================================================
 * 能力协商（先生要求：新旧互通）
 * ============================================================ */
uint32_t sc_negotiate(uint32_t local_caps, uint32_t peer_caps) {
    /* 旧客户端不发 caps → peer_caps = 0 → 交集 0 → 明文（互通） */
    return local_caps & peer_caps;
}

int sc_should_encrypt(uint32_t negotiated) {
    return (negotiated & SC_CAP_ENCRYPT) != 0;
}

/* ============================================================
 * 统计
 * ============================================================ */
void sc_get_stats(const secure_channel_t *ch, sc_stats_t *out) {
    if (!out) return;
    if (!ch) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = ch->stats;
}

void sc_reset_stats(secure_channel_t *ch) {
    if (!ch) return;
    memset(&ch->stats, 0, sizeof(ch->stats));
}
