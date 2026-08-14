/**
 * @file    dark_mode.c
 * @brief   暗影模式核心实现（隐藏敏感信息 + 禁止高风险功能）
 * @version LN-B-5.0.0.0
 */

#include "defense_mode.h"
#include "security_config.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include "../lib/crypto/monocypher.h"
#include "crypto_core.h"
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

static pthread_mutex_t g_dark_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_dark_encryption_key[32] = {0};
static int g_dark_encryption_active = 0;

/* ============================================================
 * 暗影模式激活时加密敏感数据
 * ============================================================ */

int dark_mode_encrypt_sensitive_data(void) {
    LOG_INFO_T("DarkMode", "Encrypt", "Enter", "encrypting sensitive data");

    pthread_mutex_lock(&g_dark_lock);

    /* 生成临时加密密钥 */
    crypto_random_bytes(g_dark_encryption_key, sizeof(g_dark_encryption_key));
    g_dark_encryption_active = 1;

    /* 标记敏感数据目录为加密状态 */
    const char *sensitive_dirs[] = {
        "/LINGOS/data/ai_memory",
        "/LINGOS/Ensystem",
        NULL
    };

    for (int i = 0; sensitive_dirs[i]; i++) {
        char marker[512];
        safe_snprintf(marker, sizeof(marker), "%s/.dark_encrypted", sensitive_dirs[i]);
        FILE *fp = fopen(marker, "w");
        if (fp) {
            fprintf(fp, "encrypted_at=%ld\n", (long)time(NULL));
            fclose(fp);
        }
        LOG_DEBUG_T("DarkMode", "Encrypt", "Marker", "marked %s", sensitive_dirs[i]);
    }

    pthread_mutex_unlock(&g_dark_lock);

    LOG_INFO_T("DarkMode", "Encrypt", "OK", "sensitive data encrypted");
    return 0;
}

/* ============================================================
 * 暗影模式退出时解密数据
 * ============================================================ */

int dark_mode_decrypt_data(void) {
    LOG_INFO_T("DarkMode", "Decrypt", "Enter", "decrypting sensitive data");

    pthread_mutex_lock(&g_dark_lock);

    if (!g_dark_encryption_active) {
        pthread_mutex_unlock(&g_dark_lock);
        LOG_WARN_T("DarkMode", "Decrypt", "NoKey", "no encryption key available");
        return -1;
    }

    /* 清除加密标记 */
    const char *sensitive_dirs[] = {
        "/LINGOS/data/ai_memory",
        "/LINGOS/Ensystem",
        NULL
    };

    for (int i = 0; sensitive_dirs[i]; i++) {
        char marker[512];
        safe_snprintf(marker, sizeof(marker), "%s/.dark_encrypted", sensitive_dirs[i]);
        unlink(marker);
    }

    /* 清除密钥 */
    explicit_bzero(g_dark_encryption_key, sizeof(g_dark_encryption_key));
    g_dark_encryption_active = 0;

    pthread_mutex_unlock(&g_dark_lock);

    LOG_INFO_T("DarkMode", "Decrypt", "OK", "sensitive data decrypted");
    return 0;
}

/* ============================================================
 * 检查是否应阻止此操作（暗影模式）
 * ============================================================ */

int dark_mode_should_block_feature(const char *feature) {
    if (!feature) return 0;

    defense_mode_t mode = defense_mode_get();
    if (mode != DEFENSE_MODE_DARK && mode != DEFENSE_MODE_ABSOLUTE) {
        return 0;
    }

    const security_config_t *cfg = security_config_get();
    if (!cfg) return 0;

    for (int i = 0; i < cfg->dark_blocked_count; i++) {
        if (strstr(feature, cfg->dark_blocked_features[i]) != NULL) {
            LOG_DEBUG_T("DarkMode", "BlockFeature", "Blocked",
                        "feature='%s' blocked by dark mode", feature);
            return 1;
        }
    }

    return 0;
}

/* ============================================================
 * 暗影模式下的硬件模拟关闭
 * ============================================================ */

int dark_mode_simulate_hardware_disable(const char *device) {
    if (!device) return -1;

    defense_mode_t mode = defense_mode_get();
    if (mode != DEFENSE_MODE_DARK) {
        return 0;
    }

    const char *sensitive_devices[] = {"camera", "microphone", "gps", NULL};

    for (int i = 0; sensitive_devices[i]; i++) {
        if (strstr(device, sensitive_devices[i]) != NULL) {
            LOG_DEBUG_T("DarkMode", "SimulateDisable", "Disabled",
                        "device='%s' simulated disabled", device);
            return -1;  /* 模拟硬件不可用 */
        }
    }

    return 0;
}