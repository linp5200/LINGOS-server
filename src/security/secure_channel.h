/**
 * @file    src/security/secure_channel.h
 * @brief   安全通道（先生 2026-09-12 裁决 S1「真加密」落地）
 * @version LN-0.5.0
 *
 * ⚠️ 背景（审计发现）：
 *   原 connection_handler.c 返回 `"encrypted":true`、日志写 "with encryption"，
 *   但**服务端根本没有 TLS**，crypto_core/envelope/monocypher **从未被调用**，
 *   g_encryption_enabled 只设置不判断 —— 即**虚假加密声明**。
 *   同时 App 默认构造 `wss://`，与服务端实际能力不符。
 *
 * ✅ 本模块提供**真实可用**的应用层加密：
 *   1. X25519 ECDH 临时密钥协商（前向保密）
 *   2. HKDF(BLAKE2b) 派生会话密钥
 *   3. XChaCha20-Poly1305 逐帧 AEAD（含序列号防重放）
 *   4. **能力协商**：双方都支持才加密 → 保证新旧版本互通（先生要求）
 *
 * 与 TLS 的关系：本模块**不替代 TLS**，而是提供：
 *   · 纯内网/无证书场景下的加密（无需 PKI）
 *   · 与 TLS 可叠加（双保险）
 */

#ifndef SECURE_CHANNEL_H
#define SECURE_CHANNEL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 协议常量 */
#define SC_PUBKEY_SIZE     32          /* X25519 公钥 */
#define SC_KEY_SIZE        32          /* 会话密钥 */
#define SC_NONCE_SIZE      24          /* XChaCha20 nonce */
#define SC_TAG_SIZE        16          /* Poly1305 tag */
#define SC_MAX_PAYLOAD     16384       /* 单帧最大明文（与 TCP 帧一致） */

/* 会话状态 */
typedef enum {
    SC_STATE_INIT = 0,      /* 未协商 */
    SC_STATE_HANDSHAKE,     /* 已发本端公钥，等对端 */
    SC_STATE_READY,         /* 密钥就绪，可加解密 */
    SC_STATE_FAILED         /* 协商失败 */
} sc_state_t;

typedef struct secure_channel secure_channel_t;

/* ============================================================
 * 生命周期
 * ============================================================ */

/**
 * @brief 创建通道（分配临时 X25519 密钥对）
 * @return 通道对象；NULL 表示随机源不可用（**必须拒绝继续**）
 */
secure_channel_t *sc_create(void);

void sc_destroy(secure_channel_t *ch);

/* ============================================================
 * 握手（双向：各自发送 32 字节公钥）
 * ============================================================ */

/** 取本端公钥（32 字节，用于发送给对端） */
const uint8_t *sc_local_public(const secure_channel_t *ch);

/**
 * @brief 载入对端公钥 → 派生会话密钥
 * @param salt 可选盐（建议用双方 nonce 拼接）
 * @return 0 成功；-1 失败（弱公钥/随机源问题）
 */
int sc_handshake(secure_channel_t *ch, const uint8_t *peer_public,
                 const uint8_t *salt, size_t salt_len);

/** 状态查询 */
sc_state_t sc_state(const secure_channel_t *ch);
int sc_is_ready(const secure_channel_t *ch);

/* ============================================================
 * 加解密（逐帧，含序列号防重放）
 * ============================================================ */

/**
 * @brief 加密一帧
 * @param out     输出缓冲（需 ≥ in_len + SC_TAG_SIZE）
 * @param out_len 输出长度
 * @return 0 成功；-1 失败（未就绪/缓冲不足/随机源问题）
 */
int sc_encrypt(secure_channel_t *ch, const uint8_t *in, size_t in_len,
               uint8_t *out, size_t *out_len, size_t out_cap);

/**
 * @brief 解密一帧
 * @return 0 成功；-1 失败（认证失败/重放/未就绪）
 */
int sc_decrypt(secure_channel_t *ch, const uint8_t *in, size_t in_len,
               uint8_t *out, size_t *out_len, size_t out_cap);

/* ============================================================
 * 能力协商（先生要求：新旧版本互通）
 * ============================================================ */

/** 是否支持加密（本端能力） */
#define SC_CAP_ENCRYPT  0x01
/** 是否支持 TLS（服务端有证书时） */
#define SC_CAP_TLS      0x02

/**
 * @brief 协商决策
 * @param local_caps  本端能力位
 * @param peer_caps   对端能力位（旧客户端不发 → 0）
 * @return 最终采用的能力位（交集）；0 = 明文（旧对端）
 */
uint32_t sc_negotiate(uint32_t local_caps, uint32_t peer_caps);

/**
 * @brief 是否应启用加密
 * @param negotiated 协商结果
 */
int sc_should_encrypt(uint32_t negotiated);

/* ============================================================
 * 统计与诊断（用于「encrypted」字段真实上报）
 * ============================================================ */
typedef struct {
    int      active;              /* 通道是否启用 */
    uint64_t frames_encrypted;    /* 已加密帧数 */
    uint64_t frames_decrypted;    /* 已解密帧数 */
    uint64_t auth_failures;       /* 认证失败次数 */
    uint64_t replays_blocked;     /* 重放拦截次数 */
} sc_stats_t;

void sc_get_stats(const secure_channel_t *ch, sc_stats_t *out);
void sc_reset_stats(secure_channel_t *ch);

#ifdef __cplusplus
}
#endif

#endif /* SECURE_CHANNEL_H */
