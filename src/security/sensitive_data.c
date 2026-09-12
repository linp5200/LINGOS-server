/**
 * @file    src/security/sensitive_data.c
 * @brief   敏感数据分级实现（S14）
 * @version LN-0.5.0
 */

#include "sensitive_data.h"
#include "safe_string.h"
#include "log_extra.h"
#include "crypto/envelope.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

/* ============================================================
 * 路径 → 敏感级别
 * ============================================================ */
typedef struct {
    const char   *prefix;   /* 路径前缀 */
    sensitivity_t level;
} path_rule_t;

static const path_rule_t g_path_rules[] = {
    /* —— 高敏 —— */
    { "/LINGOS/system/config/provider.json",   SENS_HIGH },
    { "/LINGOS/system/config/ai_config.json",  SENS_HIGH },
    { "/LINGOS/system/config/ha_config.json",  SENS_HIGH },
    { "/LINGOS/system/config/.syskey",         SENS_HIGH },
    { "/LINGOS/system/config/",                SENS_MEDIUM },  /* 其余配置：中敏 */
    { "/LINGOS/data/tokens",                   SENS_HIGH },
    { "/LINGOS/state/tokens",                  SENS_HIGH },
    /* —— 中敏 —— */
    { "/LINGOS/data/memory",                   SENS_MEDIUM },
    { "/LINGOS/data/sessions",                 SENS_MEDIUM },
    { "/LINGOS/data/agent_tasks",              SENS_MEDIUM },
    { "/LINGOS/data/session_archives",         SENS_MEDIUM },
    { "/LINGOS/AH",                            SENS_MEDIUM },  /* Help AI 档案 */
    { "/LINGOS/backups",                       SENS_MEDIUM },
    { "/LINGOS/state",                         SENS_MEDIUM },
    /* —— 低敏 —— */
    { "/LINGOS/log",                           SENS_LOW },
    { "/LINGOS/logs",                          SENS_LOW },
    { "/LINGOS/cache",                         SENS_LOW },
    { "/LINGOS/data/weather_cache.json",       SENS_LOW },
    /* —— 公开 —— */
    { "/LINGOS/share",                         SENS_PUBLIC },
    { "/LINGOS/models",                        SENS_PUBLIC },
    { "/LINGOS/skills",                        SENS_PUBLIC },
    { NULL, SENS_LOW }
};

sensitivity_t sensitive_classify_path(const char *path) {
    if (!path || !*path) return SENS_MEDIUM;   /* 未知 → 保守当敏感 */
    for (int i = 0; g_path_rules[i].prefix; i++) {
        if (strncmp(path, g_path_rules[i].prefix, strlen(g_path_rules[i].prefix)) == 0)
            return g_path_rules[i].level;
    }
    return SENS_LOW;   /* 未匹配 → 低敏 */
}

/* ============================================================
 * 类型名 → 敏感级别
 * ============================================================ */
typedef struct {
    const char   *kind;
    sensitivity_t level;
} kind_rule_t;

static const kind_rule_t g_kind_rules[] = {
    { "api_key",     SENS_HIGH },
    { "token",       SENS_HIGH },
    { "password",    SENS_HIGH },
    { "credential",  SENS_HIGH },
    { "secret",      SENS_HIGH },
    { "session",     SENS_MEDIUM },
    { "memory",      SENS_MEDIUM },
    { "ha_archive",  SENS_MEDIUM },
    { "backup",      SENS_MEDIUM },
    { "config",      SENS_MEDIUM },
    { "log",         SENS_LOW },
    { "sysinfo",     SENS_LOW },
    { "weather",     SENS_LOW },
    { "icon",        SENS_PUBLIC },
    { "static",      SENS_PUBLIC },
    { NULL, SENS_LOW }
};

sensitivity_t sensitive_classify_kind(const char *kind) {
    if (!kind) return SENS_LOW;
    for (int i = 0; g_kind_rules[i].kind; i++) {
        if (strcasecmp(kind, g_kind_rules[i].kind) == 0)
            return g_kind_rules[i].level;
    }
    return SENS_LOW;
}

int sensitive_needs_encryption(sensitivity_t lv) {
    return lv == SENS_HIGH || lv == SENS_MEDIUM;
}

int sensitive_needs_redaction(sensitivity_t lv) {
    return lv == SENS_HIGH || lv == SENS_MEDIUM;
}

const char *sensitive_level_name(sensitivity_t lv) {
    switch (lv) {
        case SENS_HIGH:   return "HIGH(高敏)";
        case SENS_MEDIUM: return "MEDIUM(中敏)";
        case SENS_LOW:    return "LOW(低敏)";
        case SENS_PUBLIC: return "PUBLIC(公开)";
        default:          return "UNKNOWN";
    }
}

/* ============================================================
 * 日志脱敏
 * ============================================================ */
void sensitive_redact(const char *s, int keep, char *buf, size_t buf_sz) {
    if (!buf || buf_sz == 0) return;
    if (!s) { buf[0] = '\0'; return; }
    size_t len = strlen(s);
    if (len == 0) { buf[0] = '\0'; return; }
    if ((int)len <= keep) {
        safe_snprintf(buf, buf_sz, "****(%zu)", len);
    } else {
        safe_snprintf(buf, buf_sz, "%.*s…(%zu 位)", keep, s, len);
    }
}

/* ============================================================
 * 安全写（按级别自动加密）
 * ============================================================ */
int secure_write(const char *path, const void *data, size_t len, const char *passphrase) {
    if (!path || !data) return -1;

    sensitivity_t lv = sensitive_classify_path(path);
    if (!sensitive_needs_encryption(lv)) {
        /* 低敏/公开 → 直接写 */
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            LOG_ERROR_T("SensitiveData", "Write", "OpenFail", "%s: %s", path, strerror(errno));
            return -1;
        }
        ssize_t w = write(fd, data, len);
        close(fd);
        return (w == (ssize_t)len) ? 0 : -1;
    }

    /* 高敏/中敏 → 加密后写 */
    uint8_t *enc = NULL; size_t enc_len = 0;
    if (envelope_encrypt_buffer((const uint8_t *)data, len, passphrase, &enc, &enc_len) != 0) {
        LOG_ERROR_T("SensitiveData", "Write", "EncryptFail", "%s (%s)",
                    path, sensitive_level_name(lv));
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        memset(enc, 0, enc_len);
        free(enc);
        return -1;
    }
    ssize_t w = write(fd, enc, enc_len);
    close(fd);
    size_t n = enc_len;
    memset(enc, 0, n);
    free(enc);
    if (w != (ssize_t)enc_len) return -1;
    LOG_INFO_T("SensitiveData", "Write", "Encrypted", "%s (%s, %zu→%zu)",
               path, sensitive_level_name(lv), len, enc_len);
    return 0;
}

int secure_read(const char *path, void **out, size_t *out_len, const char *passphrase) {
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

    /* 识别是否加密（魔数） */
    if (envelope_is_encrypted_file(path)) {
        uint8_t *plain = NULL; size_t plain_len = 0;
        if (envelope_decrypt_buffer(buf, (size_t)sz, passphrase, &plain, &plain_len) != 0) {
            free(buf);
            LOG_WARN_T("SensitiveData", "Read", "DecryptFail", "%s", path);
            return -1;
        }
        free(buf);
        *out = plain;
        *out_len = plain_len;
        return 0;
    }

    buf[sz] = '\0';
    *out = buf;
    *out_len = (size_t)sz;
    return 0;
}

int secure_write_text(const char *path, const char *text, const char *passphrase) {
    if (!text) return -1;
    return secure_write(path, text, strlen(text), passphrase);
}

int secure_read_text(const char *path, char **out, const char *passphrase) {
    void *buf = NULL; size_t len = 0;
    if (secure_read(path, &buf, &len, passphrase) != 0) return -1;
    *out = (char *)buf;
    return 0;
}
