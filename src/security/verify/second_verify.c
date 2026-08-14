/**
 * @file    second_verify.c
 * @brief   敏感操作二次验证实现（密码 + Y/N 双重确认）
 * @version LN-B-4.2.0.0
 * @changes 优化无密码处理：提示设置密码或跳过并二次确认风险
 */

#include "second_verify.h"
#include "data_path.h"
#include "safe_string.h"
#include "log_extra.h"
#include "uart.h"
#include "lang.h"
#include "audit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>

#define PASSWD_PATH "/Ensystem/passwd"
#define DEFAULT_TIMEOUT 30
#define MAX_PASSWORD_LEN 256

/* ============================================================
 * 内部辅助：获取密码文件路径
 * ============================================================ */

static const char* get_passwd_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, PASSWD_PATH);
    }
    return path;
}

/* ============================================================
 * 内部辅助：确保密码文件目录存在
 * ============================================================ */

static int ensure_passwd_dir(void) {
    const char *root = lingos_data_root();
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/Ensystem", root);
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0) {
            LOG_ERROR_T("SecondVerify", "EnsureDir", "Fail", "cannot create %s: %s", dir, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/* ============================================================
 * 内部辅助：读取密码（不回显）
 * ============================================================ */

static int read_password_noecho(char *buf, size_t size) {
    struct termios old, new;
    int fd = STDIN_FILENO;

    if (tcgetattr(fd, &old) != 0) {
        LOG_WARN_T("SecondVerify", "ReadPass", "TcgetattrFail", "cannot get terminal attributes");
        /* 降级：使用普通输入（回显） */
        if (fgets(buf, size, stdin) == NULL) return -1;
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        return 0;
    }

    new = old;
    new.c_lflag &= ~(ECHO | ICANON);
    if (tcsetattr(fd, TCSANOW, &new) != 0) {
        LOG_WARN_T("SecondVerify", "ReadPass", "TcsetattrFail", "cannot set terminal attributes");
        tcsetattr(fd, TCSANOW, &old);
        return -1;
    }

    if (fgets(buf, size, stdin) == NULL) {
        tcsetattr(fd, TCSANOW, &old);
        return -1;
    }

    tcsetattr(fd, TCSANOW, &old);

    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return 0;
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

int second_verify_init(void) {
    LOG_INFO_T("SecondVerify", "Init", "Enter", "initializing second verification system");
    ensure_passwd_dir();
    LOG_INFO_T("SecondVerify", "Init", "OK", "second verification system ready");
    return 0;
}

int second_verify_has_password(void) {
    LOG_DEBUG_T("SecondVerify", "HasPassword", "Enter", "checking if root password is set");

    const char *path = get_passwd_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("SecondVerify", "HasPassword", "NoFile", "password file not found");
        return 0;
    }

    char line[256];
    int has = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "root:", 5) == 0) {
            char *p = line + 5;
            while (*p && *p != '\n') p++;
            if (p - (line + 5) > 0) {
                has = 1;
            }
            break;
        }
    }
    fclose(fp);

    LOG_DEBUG_T("SecondVerify", "HasPassword", "Result", "has_password=%d", has);
    return has;
}

int second_verify_set_password(const char *password) {
    LOG_INFO_T("SecondVerify", "SetPassword", "Enter", "setting root password");

    if (!password) {
        LOG_ERROR_T("SecondVerify", "SetPassword", "Invalid", "password is NULL");
        return -1;
    }

    if (ensure_passwd_dir() != 0) {
        LOG_ERROR_T("SecondVerify", "SetPassword", "EnsureDirFail", "cannot ensure passwd directory");
        return -1;
    }

    const char *path = get_passwd_path();
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("SecondVerify", "SetPassword", "OpenFail", "cannot write %s: %s", path, strerror(errno));
        return -1;
    }

    fprintf(fp, "root:%s\n", password);
    fclose(fp);

    /* 安全清除密码缓冲区（调用者应自行清除） */
    LOG_INFO_T("SecondVerify", "SetPassword", "OK", "root password set");
    return 0;
}

int second_verify_check_password(const char *password) {
    LOG_DEBUG_T("SecondVerify", "CheckPassword", "Enter", "verifying password");

    if (!password) {
        LOG_WARN_T("SecondVerify", "CheckPassword", "Invalid", "password is NULL");
        return -1;
    }

    const char *path = get_passwd_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_WARN_T("SecondVerify", "CheckPassword", "NoFile", "password file not found");
        return -1;
    }

    char line[256];
    int matched = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "root:", 5) == 0) {
            char *p = line + 5;
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            if (strcmp(p, password) == 0) {
                matched = 1;
            }
            break;
        }
    }
    fclose(fp);

    LOG_DEBUG_T("SecondVerify", "CheckPassword", "Result", "matched=%d", matched);
    return matched ? 1 : 0;
}

verify_result_t second_verify_check(const char *operation, verify_mode_t mode, int timeout_sec) {
    LOG_INFO_T("SecondVerify", "Check", "Enter", "operation='%s', mode=%d, timeout=%d",
               operation ? operation : "(null)", mode, timeout_sec);

    if (!operation || !*operation) {
        operation = "Unknown operation";
    }

    if (timeout_sec <= 0) {
        timeout_sec = DEFAULT_TIMEOUT;
    }

    /* ============================================================
     * 循环处理无密码情况，允许用户设置密码或跳过
     * ============================================================ */
    while (1) {
        int has_password = second_verify_has_password();

        if (!has_password) {
            /* ----- 无密码处理 ----- */
            uart_puts(COLOR_YELLOW);
            uart_puts(tr("\n[SECURITY] Root password is not set.\n",
                         "\n[安全] root 密码尚未设置。\n"));
            uart_puts(tr("For your security, it is recommended to set a root password.\n",
                         "为了您的安全，建议设置 root 密码。\n"));
            uart_puts(tr("Would you like to set a root password now? (y/N): ",
                         "是否现在设置 root 密码？(y/N): "));
            uart_puts(COLOR_RESET);

            char c = uart_getc();
            uart_putc(c);
            uart_puts("\n");

            if (c == 'y' || c == 'Y') {
                /* 用户选择设置密码 */
                char pass1[MAX_PASSWORD_LEN] = {0};
                char pass2[MAX_PASSWORD_LEN] = {0};

                uart_puts(tr("Enter new root password: ", "输入新 root 密码："));
                if (read_password_noecho(pass1, sizeof(pass1)) != 0) {
                    explicit_bzero(pass1, sizeof(pass1));
                    explicit_bzero(pass2, sizeof(pass2));
                    LOG_WARN_T("SecondVerify", "Check", "ReadPassFail", "failed to read password");
                    return VERIFY_RESULT_CANCELLED;
                }
                uart_puts("\n");

                uart_puts(tr("Confirm password: ", "确认密码："));
                if (read_password_noecho(pass2, sizeof(pass2)) != 0) {
                    explicit_bzero(pass1, sizeof(pass1));
                    explicit_bzero(pass2, sizeof(pass2));
                    LOG_WARN_T("SecondVerify", "Check", "ReadPassFail", "failed to read confirmation");
                    return VERIFY_RESULT_CANCELLED;
                }
                uart_puts("\n");

                if (strlen(pass1) == 0) {
                    uart_puts(tr("Password cannot be empty. Operation cancelled.\n", "密码不能为空。操作已取消。\n"));
                    explicit_bzero(pass1, sizeof(pass1));
                    explicit_bzero(pass2, sizeof(pass2));
                    return VERIFY_RESULT_CANCELLED;
                }

                if (strcmp(pass1, pass2) == 0) {
                    if (second_verify_set_password(pass1) == 0) {
                        uart_puts(tr("Password set successfully.\n", "密码设置成功。\n"));
                        explicit_bzero(pass1, sizeof(pass1));
                        explicit_bzero(pass2, sizeof(pass2));
                        /* 密码设置成功，重新开始验证（循环） */
                        LOG_INFO_T("SecondVerify", "Check", "PasswordSet", "password set, re-verifying");
                        continue;  /* 回到循环开头，现在有密码了 */
                    } else {
                        uart_puts(tr("Failed to save password. Operation cancelled.\n", "保存密码失败。操作已取消。\n"));
                        explicit_bzero(pass1, sizeof(pass1));
                        explicit_bzero(pass2, sizeof(pass2));
                        return VERIFY_RESULT_DENIED;
                    }
                } else {
                    uart_puts(tr("Passwords do not match. Operation cancelled.\n", "密码不匹配。操作已取消。\n"));
                    explicit_bzero(pass1, sizeof(pass1));
                    explicit_bzero(pass2, sizeof(pass2));
                    return VERIFY_RESULT_DENIED;
                }
            } else {
                /* 用户选择不设置密码 */
                uart_puts(COLOR_YELLOW);
                uart_puts(tr("\n[WARNING] Proceeding without a root password is not recommended.\n",
                             "\n[警告] 在没有 root 密码的情况下继续操作不被推荐。\n"));
                uart_puts(tr("Please ensure this system is not exposed to external threats,\n",
                             "请确保当前系统不受外部侵害，\n"));
                uart_puts(tr("otherwise your privacy and data may be at risk.\n",
                             "否则您的隐私和数据将面临风险。\n"));
                uart_puts(tr("Do you still want to run this operation? (y/N): ",
                             "是否仍然运行该操作？(y/N): "));
                uart_puts(COLOR_RESET);

                c = uart_getc();
                uart_putc(c);
                uart_puts("\n");

                if (c == 'y' || c == 'Y') {
                    LOG_WARN_T("SecondVerify", "Check", "SkipPassword", "user skipped password and approved operation '%s'", operation);
                    audit_log("system", "second_verify", "skip_password", operation, "approved", 0, "high", 1);
                    return VERIFY_RESULT_APPROVED;
                } else {
                    LOG_INFO_T("SecondVerify", "Check", "Denied", "operation '%s' denied after skip", operation);
                    audit_log("system", "second_verify", "skip_password", operation, "denied", -1, "high", 0);
                    return VERIFY_RESULT_DENIED;
                }
            }
        }

        /* ----- 有密码：正常验证流程 ----- */
        /* ====== 步骤1：密码验证 ====== */
        char password[MAX_PASSWORD_LEN] = {0};
        uart_puts(COLOR_BOLD COLOR_YELLOW);
        char prompt[256];
        safe_snprintf(prompt, sizeof(prompt),
                      tr("\n[SECURITY] Operation '%s' requires root password.\n",
                         "\n[安全] 操作 '%s' 需要 root 密码。\n"),
                      operation);
        uart_puts(prompt);
        uart_puts(tr("Enter password: ", "请输入密码："));
        uart_puts(COLOR_RESET);

        if (read_password_noecho(password, sizeof(password)) != 0) {
            LOG_WARN_T("SecondVerify", "Check", "ReadPassFail", "failed to read password");
            return VERIFY_RESULT_CANCELLED;
        }
        uart_puts("\n");

        int pass_ok = second_verify_check_password(password);
        explicit_bzero(password, sizeof(password));

        if (!pass_ok) {
            LOG_WARN_T("SecondVerify", "Check", "PassFail", "incorrect password for operation '%s'", operation);
            audit_log("system", "second_verify", "password", operation, "incorrect", -1, "high", 0);
            uart_puts(COLOR_RED);
            uart_puts(tr("Incorrect password. Operation denied.\n", "密码错误。操作已拒绝。\n"));
            uart_puts(COLOR_RESET);
            return VERIFY_RESULT_DENIED;
        }

        LOG_INFO_T("SecondVerify", "Check", "PassOK", "password verified for operation '%s'", operation);
        audit_log("system", "second_verify", "password", operation, "correct", 0, "high", 1);

        /* ====== 步骤2：Y/N 确认（如果模式要求） ====== */
        if (mode == VERIFY_MODE_PASSWORD_ONLY) {
            LOG_DEBUG_T("SecondVerify", "Check", "Mode", "password only, skipping Y/N");
            return VERIFY_RESULT_APPROVED;
        }

        uart_puts(COLOR_BOLD COLOR_YELLOW);
        char confirm_prompt[256];
        safe_snprintf(confirm_prompt, sizeof(confirm_prompt),
                      tr("\nConfirm operation '%s'? (Y/N): ",
                         "\n确认执行操作 '%s'？(Y/N): "),
                      operation);
        uart_puts(confirm_prompt);
        uart_puts(COLOR_RESET);

        char c = 0;
        time_t start = time(NULL);
        while (1) {
            if (time(NULL) - start > timeout_sec) {
                uart_puts(tr("\nTimeout. Operation cancelled.\n", "\n超时。操作已取消。\n"));
                LOG_WARN_T("SecondVerify", "Check", "Timeout", "Y/N timeout for operation '%s'", operation);
                audit_log("system", "second_verify", "yn", operation, "timeout", 0, "high", 0);
                return VERIFY_RESULT_TIMEOUT;
            }

            c = uart_getc();
            if (c == '\r' || c == '\n') continue;
            if (c == 'Y' || c == 'y' || c == 'N' || c == 'n') {
                uart_putc(c);
                uart_puts("\n");
                break;
            }
            /* 其他按键忽略 */
        }

        if (c == 'Y' || c == 'y') {
            LOG_INFO_T("SecondVerify", "Check", "Approved", "operation '%s' approved", operation);
            audit_log("system", "second_verify", "yn", operation, "approved", 0, "high", 1);
            uart_puts(tr("Operation approved.\n", "操作已批准。\n"));
            return VERIFY_RESULT_APPROVED;
        } else {
            LOG_INFO_T("SecondVerify", "Check", "Denied", "operation '%s' denied by user", operation);
            audit_log("system", "second_verify", "yn", operation, "denied", -1, "high", 0);
            uart_puts(tr("Operation denied.\n", "操作已拒绝。\n"));
            return VERIFY_RESULT_DENIED;
        }
    }
}

verify_result_t second_verify_quick(const char *operation) {
    return second_verify_check(operation, VERIFY_MODE_PASSWORD_AND_YN, DEFAULT_TIMEOUT);
}

const char* second_verify_mode_name(verify_mode_t mode) {
    switch (mode) {
        case VERIFY_MODE_PASSWORD_ONLY:     return "password_only";
        case VERIFY_MODE_PASSWORD_AND_YN:   return "password_and_yn";
        default:                            return "unknown";
    }
}

const char* second_verify_result_str(verify_result_t result) {
    switch (result) {
        case VERIFY_RESULT_DENIED:    return "denied";
        case VERIFY_RESULT_APPROVED:  return "approved";
        case VERIFY_RESULT_TIMEOUT:   return "timeout";
        case VERIFY_RESULT_CANCELLED: return "cancelled";
        default:                      return "unknown";
    }
}