/**
 * @file    src/security/sensitive_data.h
 * @brief   敏感数据分级（先生 2026-09-12 裁决 S14）
 * @version LN-0.5.0
 *
 * 先生裁决：
 *   「应该只加密敏感内容，记忆等敏感，不敏感可以不加密」
 *   隐私分级四档（高敏/中敏/低敏/公开）—— 认可
 *
 * 设计依据（GDPR Art.25）：
 *   · Data protection by design / by default
 *   · Data minimisation（只加密该加密的）
 *
 * 分级与处置：
 *   ┌────────┬──────────────────────────────┬──────────┬────────────┐
 *   │ 级别   │ 数据                          │ 静态加密  │ 日志脱敏    │
 *   ├────────┼──────────────────────────────┼──────────┼────────────┤
 *   │ HIGH   │ API Key / token / 密码 / 云凭据 │ ✅ 必须   │ ✅ 必须     │
 *   │ MEDIUM │ 会话 / 记忆 / HA 档案 / 备份    │ ✅ 必须   │ ✅ 必须     │
 *   │ LOW    │ 系统信息 / 天气缓存 / 运行日志   │ ❌ 可明文 │ ❌ 可完整   │
 *   │ PUBLIC │ 图标 / 静态资源 / 网页          │ ❌ 明文   │ ❌ 不适用   │
 *   └────────┴──────────────────────────────┴──────────┴────────────┘
 */

#ifndef SENSITIVE_DATA_H
#define SENSITIVE_DATA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SENS_PUBLIC = 0,     /* 公开 */
    SENS_LOW,            /* 低敏 */
    SENS_MEDIUM,         /* 中敏 */
    SENS_HIGH            /* 高敏 */
} sensitivity_t;

/** 按文件路径判定敏感级别 */
sensitivity_t sensitive_classify_path(const char *path);

/** 按数据类型名判定（如 "api_key" / "session" / "memory" / "log"） */
sensitivity_t sensitive_classify_kind(const char *kind);

/** 该级别是否需要静态加密 */
int sensitive_needs_encryption(sensitivity_t lv);

/** 该级别在日志中是否必须脱敏 */
int sensitive_needs_redaction(sensitivity_t lv);

/** 级别名（中文/英文按语言） */
const char *sensitive_level_name(sensitivity_t lv);

/* ============================================================
 * 安全存储（自动按级别加解密）
 * ============================================================ */

/**
 * @brief 安全写入（高敏/中敏自动加密，低敏/公开直接写）
 * @param path 目标路径
 * @param data 数据
 * @param len  长度
 * @param passphrase 口令；NULL → 使用系统密钥
 * @return 0 成功；-1 失败
 */
int secure_write(const char *path, const void *data, size_t len, const char *passphrase);

/**
 * @brief 安全读取（自动识别加密文件并解密）
 * @param out     输出缓冲（调用方 malloc，需 free）
 * @param out_len 出参
 * @return 0 成功；-1 失败
 */
int secure_read(const char *path, void **out, size_t *out_len, const char *passphrase);

/**
 * @brief 文本版便捷封装（自动判级别 + 加解密 + NUL 结尾）
 */
int secure_write_text(const char *path, const char *text, const char *passphrase);
int secure_read_text(const char *path, char **out, const char *passphrase);

/**
 * @brief 日志脱敏（保留前 keep 位）
 * @param s    原始串
 * @param keep 保留位数
 * @param buf  输出
 * @param buf_sz
 */
void sensitive_redact(const char *s, int keep, char *buf, size_t buf_sz);

#ifdef __cplusplus
}
#endif

#endif /* SENSITIVE_DATA_H */
