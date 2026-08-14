/**
 * @file    token_verify.c
 * @brief   物理令牌管理（生成/验证/刷新/锁定）
 * @version LN-B-5.0.0.0
 * @changes 添加 fail_count 锁保护；修复 fopen 返回值检查；修复 token_verify 中变量名错误
 */

#include "token_verify.h"
#include "common/safe_string.h"
#include "common/data_path.h"
#include "common/lang.h"
#include "lib/log_extra.h"
#include "drivers/uart.h"
#include "security/crypto/crypto_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#define TOKEN_LENGTH 12
#define MAX_ATTEMPTS 3
#define TOKEN_FILE "token.hash"
#define LOCK_FILE "token.lock"

/* 失败计数静态变量 + 锁 */
static int g_fail_count = 0;
static pthread_mutex_t g_fail_lock = PTHREAD_MUTEX_INITIALIZER;

static const char* get_token_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s/Ensystem/%s", root, TOKEN_FILE);
    }
    return path;
}

static const char* get_lock_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s/Ensystem/%s", root, LOCK_FILE);
    }
    return path;
}

static int read_stored_hash(unsigned char *hash_out) {
    const char *path = get_token_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_WARN_T("Token", "ReadHash", "NotFound", "no stored token hash");
        return -1;
    }

    char hex[65];
    if (fgets(hex, sizeof(hex), fp) == NULL) {
        fclose(fp);
        LOG_WARN_T("Token", "ReadHash", "ReadFail", "failed to read hash");
        return -1;
    }
    fclose(fp);

    int len = strlen(hex);
    if (len < 64) {
        LOG_WARN_T("Token", "ReadHash", "Invalid", "hash length %d < 64", len);
        return -1;
    }

    for (int i = 0; i < 32; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%02x", &byte) != 1) {
            LOG_WARN_T("Token", "ReadHash", "ParseFail", "failed to parse hex at index %d", i);
            return -1;
        }
        hash_out[i] = (unsigned char)byte;
    }
    return 0;
}

int token_generate(void) {
    LOG_INFO_T("Token", "Generate", "Enter", "generating new physical token");

    const char *lock_path = get_lock_path();
    if (access(lock_path, F_OK) == 0) {
        LOG_ERROR_T("Token", "Generate", "Locked", "token system is permanently locked");
        uart_puts(tr("Token system is permanently locked. Cannot generate new token.\n",
                     "令牌系统已永久锁定，无法生成新令牌。\n"));
        return -1;
    }

    char rand_buf[TOKEN_LENGTH];
    crypto_random_bytes(rand_buf, TOKEN_LENGTH);

    char token[TOKEN_LENGTH + 1];
    for (int i = 0; i < TOKEN_LENGTH; i++) {
        token[i] = '0' + (rand_buf[i] % 10);
    }
    token[TOKEN_LENGTH] = '\0';

    unsigned char hash[64];   /* 注意：crypto_hash 输出 64 字节，缓冲区应足够 */
    crypto_hash(hash, (const unsigned char*)token, TOKEN_LENGTH);

    const char *path = get_token_path();
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("Token", "Generate", "WriteFail", "cannot open %s for writing", path);
        return -1;
    }
    for (int i = 0; i < 64; i++) {
        fprintf(fp, "%02x", hash[i]);
    }
    fprintf(fp, "\n");
    fclose(fp);

    LOG_INFO_T("Token", "Generate", "OK", "token generated and saved");

    /* 重置失败计数 */
    pthread_mutex_lock(&g_fail_lock);
    g_fail_count = 0;
    pthread_mutex_unlock(&g_fail_lock);

    uart_puts("\n");
    uart_puts(
        "  ╔══════════════════════════════════════════════════════════════╗\n"
        "  ║  Your system physical token has been generated:            ║\n"
        "  ║  您的系统物理令牌已生成：                                  ║\n"
        "  ║                                                            ║\n"
        "  ║  ╔══════════════════════════════════════════════════════════╗ ║\n"
        "  ║  ║  "
    );
    uart_puts(token);
    uart_puts(
        "                                                    ║ ║\n"
        "  ║  ║                                                      ║ ║\n"
        "  ║  ╚══════════════════════════════════════════════════════════╝ ║\n"
        "  ║                                                            ║\n"
        "  ║  ⚠️ Please record this token immediately and keep it safe  ║\n"
        "  ║  ⚠️ 请立即记录此令牌，并妥善保管                          ║\n"
        "  ║                                                            ║\n"
        "  ║  This token is used for manual unlocking of absolute       ║\n"
        "  ║  protect mode.                                             ║\n"
        "  ║  此令牌用于绝对保护模式的手动解锁。                        ║\n"
        "  ║                                                            ║\n"
        "  ║  This token will not be displayed again in the system.     ║\n"
        "  ║  该令牌不会在系统中再次显示。                              ║\n"
        "  ╚══════════════════════════════════════════════════════════════╝\n"
    );
    uart_puts("\n");

    return 0;
}

int token_verify(const char *input) {
    LOG_INFO_T("Token", "Verify", "Enter", "verifying token");

    if (!input) {
        LOG_WARN_T("Token", "Verify", "Invalid", "input is NULL");
        return 0;
    }

    const char *lock_path = get_lock_path();
    if (access(lock_path, F_OK) == 0) {
        LOG_ERROR_T("Token", "Verify", "Locked", "token system is permanently locked");
        uart_puts(tr("Token system is permanently locked. Contact developer.\n",
                     "令牌系统已永久锁定。请联系开发者。\n"));
        return -2;
    }

    unsigned char stored_hash[64];
    if (read_stored_hash(stored_hash) != 0) {
        LOG_WARN_T("Token", "Verify", "NoHash", "no token has been generated yet");
        uart_puts(tr("No token has been generated. Please generate one first.\n",
                     "尚未生成令牌，请先生成令牌。\n"));
        return -1;
    }

    char cleaned[TOKEN_LENGTH + 1];
    int i = 0, j = 0;
    while (input[i] == ' ') i++;
    while (input[i] && j < TOKEN_LENGTH) {
        cleaned[j++] = input[i++];
    }
    cleaned[j] = '\0';

    if (strlen(cleaned) != TOKEN_LENGTH) {
        LOG_WARN_T("Token", "Verify", "InvalidLength", "token length %zu, expected %d",
                   strlen(cleaned), TOKEN_LENGTH);
        uart_puts(tr("Invalid token length. Expected 12 digits.\n",
                     "令牌长度无效，应为12位数字。\n"));
        return 0;
    }

    for (int i = 0; i < TOKEN_LENGTH; i++) {
        if (cleaned[i] < '0' || cleaned[i] > '9') {
            LOG_WARN_T("Token", "Verify", "NonDigit", "token contains non-digit char");
            uart_puts(tr("Token must contain only digits.\n",
                         "令牌只能包含数字。\n"));
            return 0;
        }
    }

    /* 计算输入令牌的哈希 */
    unsigned char input_hash[64];
    crypto_hash(input_hash, (const unsigned char*)cleaned, TOKEN_LENGTH);

    /* 比较哈希（前 32 字节，与存储格式一致） */
    if (memcmp(input_hash, stored_hash, 32) == 0) {
        LOG_INFO_T("Token", "Verify", "OK", "token verified successfully");
        uart_puts(tr("✅ Token verified successfully.\n", "✅ 令牌验证成功。\n"));

        pthread_mutex_lock(&g_fail_lock);
        g_fail_count = 0;
        pthread_mutex_unlock(&g_fail_lock);

        int refresh_ret = token_refresh();
        if (refresh_ret == 0) {
            LOG_INFO_T("Token", "Verify", "Refreshed", "token refreshed after successful verification");
        }

        return 1;
    } else {
        pthread_mutex_lock(&g_fail_lock);
        g_fail_count++;
        int fail_count = g_fail_count;
        int remaining = MAX_ATTEMPTS - fail_count;
        pthread_mutex_unlock(&g_fail_lock);

        LOG_WARN_T("Token", "Verify", "Fail", "invalid token, attempt %d/%d", fail_count, MAX_ATTEMPTS);

        if (remaining > 0) {
            uart_puts(tr("❌ Invalid token. ", "❌ 令牌无效。"));
            char buf[64];
            safe_snprintf(buf, sizeof(buf), tr("%d attempt(s) remaining before lock.\n",
                                               "锁定前还有 %d 次尝试机会。\n"), remaining);
            uart_puts(buf);
        }

        if (fail_count >= MAX_ATTEMPTS) {
            FILE *fp = fopen(lock_path, "w");
            if (fp) {
                fprintf(fp, "locked\n");
                fclose(fp);
                LOG_ERROR_T("Token", "Verify", "Locked", "token system permanently locked");
                uart_puts(tr("❌ Token system permanently locked due to too many failed attempts.\n",
                             "❌ 令牌系统因尝试次数过多已永久锁定。\n"));
                return -2;
            }
        }

        return 0;
    }
}

int token_refresh(void) {
    LOG_INFO_T("Token", "Refresh", "Enter", "refreshing token");

    const char *lock_path = get_lock_path();
    if (access(lock_path, F_OK) == 0) {
        LOG_ERROR_T("Token", "Refresh", "Locked", "token system is permanently locked");
        uart_puts(tr("Token system is permanently locked. Cannot refresh.\n",
                     "令牌系统已永久锁定，无法刷新。\n"));
        return -1;
    }

    const char *path = get_token_path();
    if (unlink(path) != 0 && errno != ENOENT) {
        LOG_WARN_T("Token", "Refresh", "UnlinkFail", "failed to unlink %s: %s", path, strerror(errno));
    }

    pthread_mutex_lock(&g_fail_lock);
    g_fail_count = 0;
    pthread_mutex_unlock(&g_fail_lock);

    uart_puts(tr("Token refreshed. A new token will be generated.\n",
                 "令牌已刷新。将生成新的令牌。\n"));

    return token_generate();
}