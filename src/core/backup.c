/**
 * @file    src/core/backup.c
 * @brief   系统备份与恢复管理（含注册表和安全配置备份）
 * @version LN-0.4.3
 * @changes 增加 backup_list() 和 backup_restore_latest() 函数；
 *          使用 fork+execvp 替代 system；
 *          新增注册表和 security.json 备份
 */

#include "backup.h"
#include "log_extra.h"
#include "data_path.h"
#include "safe_string.h"
#include "lang.h"
#include "uart.h"
#include "envelope.h"
#include "crypto_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>

#define BACKUP_ROOT "/LINGOS/backups"
#define PRE_REPAIR_PREFIX "pre_repair_"
#define MANUAL_PREFIX "manual_"

static int safe_exec(const char *cmd, char *const argv[]) {
    pid_t pid = fork();
    if (pid == -1) return -1;
    if (pid == 0) {
        execvp(cmd, argv);
        _exit(1);
    } else {
        int status;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
}

static int safe_cp(const char *src, const char *dst) {
    char *argv[] = {"cp", "-r", (char*)src, (char*)dst, NULL};
    return safe_exec("cp", argv);
}

static int safe_rm_rf(const char *path) {
    char *argv[] = {"rm", "-rf", (char*)path, NULL};
    return safe_exec("rm", argv);
}

static int mkdir_p(const char *path) {
    if (!path) return -1;
    char tmp[512];
    char *p;
    safe_snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static long long get_timestamp_from_name(const char *name, const char *prefix) {
    if (!name || !prefix) return 0;
    size_t plen = strlen(prefix);
    if (strncmp(name, prefix, plen) != 0) return 0;
    const char *ts = name + plen;
    long long val = 0;
    while (*ts && *ts >= '0' && *ts <= '9') {
        val = val * 10 + (*ts - '0');
        ts++;
    }
    return val;
}

static int get_backup_dirs(const char *prefix, char dirs[][512], int max_count) {
    char backup_root[512];
    safe_snprintf(backup_root, sizeof(backup_root), "%s", BACKUP_ROOT);
    DIR *d = opendir(backup_root);
    if (!d) return 0;
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(d)) != NULL && count < max_count) {
        if (entry->d_name[0] == '.') continue;
        if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0) continue;
        char full[512];
        safe_snprintf(full, sizeof(full), "%s/%s", backup_root, entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        safe_snprintf(dirs[count], 512, "%s", full);
        count++;
    }
    closedir(d);
    return count;
}

/* ============================================================
 * 原有函数
 * ============================================================ */
int backup_system(char *backup_path, size_t path_len, int is_manual) {
    LOG_INFO_T("Backup", "BackupSystem", "Enter", "is_manual=%d", is_manual);
    if (!backup_path || path_len == 0) return -1;

    const char *root = lingos_data_root();
    char backup_root[512];
    safe_snprintf(backup_root, sizeof(backup_root), "%s", BACKUP_ROOT);
    mkdir_p(backup_root);

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);

    const char *prefix = is_manual ? MANUAL_PREFIX : PRE_REPAIR_PREFIX;
    safe_snprintf(backup_path, path_len, "%s/%s%s", backup_root, prefix, ts);
    mkdir_p(backup_path);

    /* 备份整个 /LINGOS（排除 backups 自身） */
    char cmd[2048];
    safe_snprintf(cmd, sizeof(cmd), "find '%s' -mindepth 1 -maxdepth 1 -not -path '%s/backups' -exec cp -r {} '%s' \\; 2>/dev/null",
                  root, root, backup_path);
    system(cmd);

    /* 强制备份注册表和安全配置 */
    char reg_src[512], sec_src[512];
    safe_snprintf(reg_src, sizeof(reg_src), "%s/registry", root);
    safe_snprintf(sec_src, sizeof(sec_src), "%s/system/config/security.json", root);
    char reg_dst[512], sec_dst[512];
    safe_snprintf(reg_dst, sizeof(reg_dst), "%s/registry", backup_path);
    safe_snprintf(sec_dst, sizeof(sec_dst), "%s/system/config/", backup_path);
    mkdir_p(reg_dst);
    mkdir_p(sec_dst);
    safe_cp(reg_src, backup_path);
    safe_cp(sec_src, sec_dst);

    /* 【0.6.0 S15】敏感文件加密（provider.json / ha_config / passwd / device.key）
     *   + 移除备份中的 backup.key（密钥不入备份——备份泄露时仍受保护） */
    backup_encrypt_sensitive(backup_path);
    {
        char bkex[640];
        safe_snprintf(bkex, sizeof(bkex), "%s/Ensystem/backup.key", backup_path);
        unlink(bkex);
    }

    LOG_INFO_T("Backup", "BackupSystem", "OK", "backup created at %s", backup_path);
    return 0;
}

int restore_backup(const char *backup_path) {
    LOG_INFO_T("Backup", "Restore", "Enter", "backup_path='%s'", backup_path ? backup_path : "(null)");
    if (!backup_path || access(backup_path, F_OK) != 0) return -1;
    const char *root = lingos_data_root();
    safe_rm_rf((char*)root);
    safe_cp(backup_path, root);
    /* 【0.6.0 S15】还原敏感文件（*.enc → 明文——用本机 backup.key 解密） */
    backup_decrypt_sensitive(root);
    LOG_INFO_T("Backup", "Restore", "OK", "restored from %s", backup_path);
    return 0;
}

/* ============================================================
 * 【0.6.0 S15】备份敏感文件加密 / 还原（envelope + backup.key）
 *
 * 设计：
 *   · 密钥 = /LINGOS/Ensystem/backup.key（独立随机密钥——与 device.key 分离）
 *   · 备份内**不含** backup.key（备份泄露时敏感文件仍受保护）
 *   · 敏感文件清单（备份目录内相对路径）：
 *       system/config/provider.json  （API Keys——S5 已加密，此层再加）
 *       system/config/ha_config.json （HA 访问令牌）
 *       Ensystem/passwd              （root 凭据）
 *       Ensystem/device.key          （设备主密钥）
 *   · 处理：<file> → <file>.enc（envelope XChaCha20-Poly1305），删除明文
 *   · 还原：restore 后 <file>.enc → <file>（需本机 backup.key）
 *   · 灾难恢复提示：整机重装场景，用户需离线保存 backup.key；
 *     无 key 时敏感文件保留 .enc（不丢失，待提供 key 再解）
 * ============================================================ */

static const char *SENSITIVE_FILES[] = {
    "system/config/provider.json",
    "system/config/ha_config.json",
    "Ensystem/passwd",
    "Ensystem/device.key",
    NULL
};

static int backup_get_passphrase(char *out, size_t out_len) {
    const char *root = lingos_data_root();
    char kpath[512];
    safe_snprintf(kpath, sizeof(kpath), "%s/Ensystem/backup.key", root);

    FILE *kf = fopen(kpath, "r");
    if (kf) {
        if (!fgets(out, (int)out_len, kf)) out[0] = '\0';
        fclose(kf);
        size_t pl = strlen(out);
        while (pl > 0 && (out[pl-1] == '\n' || out[pl-1] == '\r')) out[--pl] = '\0';
        if (pl >= 16) return 0;
    }
    /* 首次：生成 32 字节真随机（失败即拒绝——S6 不降级） */
    uint8_t raw[32];
    if (crypto_random_bytes(raw, sizeof(raw)) != 0) return -1;
    char hexk[80];
    for (int i = 0; i < 32; i++) sprintf(hexk + i*2, "%02x", raw[i]);
    hexk[64] = '\0';
    char edir[512];
    safe_snprintf(edir, sizeof(edir), "%s/Ensystem", root);
    mkdir(edir, 0700);
    FILE *kfw = fopen(kpath, "w");
    if (!kfw) return -1;
    fprintf(kfw, "%s\n", hexk);
    fclose(kfw);
    chmod(kpath, 0600);
    safe_strncpy(out, hexk, out_len);
    LOG_INFO_T("Backup", "Passphrase", "Gen", "backup.key generated (%s)", kpath);
    return 0;
}

int backup_encrypt_sensitive(const char *backup_path) {
    LOG_INFO_T("Backup", "EncSensitive", "Enter", "backup='%s'", backup_path ? backup_path : "(null)");
    if (!backup_path || !backup_path[0]) return -1;

    char pass[80];
    if (backup_get_passphrase(pass, sizeof(pass)) != 0) {
        LOG_WARN_T("Backup", "EncSensitive", "NoKey", "cannot obtain backup key — sensitive files left plaintext");
        return -1;
    }

    int done = 0;
    for (int i = 0; SENSITIVE_FILES[i]; i++) {
        char src[640], dst[680];
        safe_snprintf(src, sizeof(src), "%s/%s", backup_path, SENSITIVE_FILES[i]);
        if (access(src, F_OK) != 0) continue;
        safe_snprintf(dst, sizeof(dst), "%s.enc", src);
        if (envelope_encrypt_file(src, dst, pass) == 0) {
            unlink(src);
            chmod(dst, 0600);
            done++;
            LOG_DEBUG_T("Backup", "EncSensitive", "OK", "encrypted %s", SENSITIVE_FILES[i]);
        } else {
            LOG_WARN_T("Backup", "EncSensitive", "Fail", "cannot encrypt %s", SENSITIVE_FILES[i]);
        }
    }
    LOG_INFO_T("Backup", "EncSensitive", "Done", "%d sensitive files encrypted in backup", done);
    return done;
}

int backup_decrypt_sensitive(const char *backup_root) {
    LOG_INFO_T("Backup", "DecSensitive", "Enter", "root='%s'", backup_root ? backup_root : "(null)");
    if (!backup_root || !backup_root[0]) return -1;

    char pass[80];
    if (backup_get_passphrase(pass, sizeof(pass)) != 0) return -1;

    int done = 0;
    for (int i = 0; SENSITIVE_FILES[i]; i++) {
        char enc[680], plain[640];
        safe_snprintf(enc, sizeof(enc), "%s/%s.enc", backup_root, SENSITIVE_FILES[i]);
        if (access(enc, F_OK) != 0) continue;
        safe_snprintf(plain, sizeof(plain), "%s/%s", backup_root, SENSITIVE_FILES[i]);
        if (envelope_decrypt_file(enc, plain, pass) == 0) {
            unlink(enc);
            chmod(plain, 0600);
            done++;
            LOG_DEBUG_T("Backup", "DecSensitive", "OK", "restored %s", SENSITIVE_FILES[i]);
        } else {
            LOG_WARN_T("Backup", "DecSensitive", "Fail",
                       "cannot decrypt %s (backup.key mismatch? file kept as .enc)", SENSITIVE_FILES[i]);
        }
    }
    LOG_INFO_T("Backup", "DecSensitive", "Done", "%d sensitive files restored", done);
    return done;
}

int cleanup_backups(void) {
    LOG_INFO_T("Backup", "Cleanup", "Enter", "Starting backup cleanup");
    char backup_root[512];
    safe_snprintf(backup_root, sizeof(backup_root), "%s", BACKUP_ROOT);
    if (access(backup_root, F_OK) != 0) return 0;

    char dirs[128][512];
    int count = get_backup_dirs(PRE_REPAIR_PREFIX, dirs, 128);
    time_t now = time(NULL);
    time_t cutoff = now - 86400;
    for (int i = 0; i < count; i++) {
        struct stat st;
        if (stat(dirs[i], &st) == 0 && st.st_mtime < cutoff) {
            safe_rm_rf(dirs[i]);
            LOG_INFO_T("Backup", "Cleanup", "Removed", "AI backup %s", dirs[i]);
        }
    }

    count = get_backup_dirs(MANUAL_PREFIX, dirs, 128);
    if (count > 2) {
        for (int i = 2; i < count; i++) {
            safe_rm_rf(dirs[i]);
            LOG_INFO_T("Backup", "Cleanup", "Removed", "manual backup %s", dirs[i]);
        }
    }

    return 0;
}

int sync_to_cloud(const char *backup_path) {
    LOG_DEBUG_T("Backup", "CloudSync", "Enter", "backup_path='%s'", backup_path ? backup_path : "(null)");
    (void)backup_path;
    return 0;
}

/* ============================================================
 * 新增函数：列出所有备份
 * ============================================================ */
void backup_list(void) {
    LOG_DEBUG_T("Backup", "List", "Enter", "listing backups");

    char backup_root[512];
    safe_snprintf(backup_root, sizeof(backup_root), "%s", BACKUP_ROOT);

    if (access(backup_root, F_OK) != 0) {
        uart_puts(tr("  No backups found.\n", "  未找到备份。\n"));
        return;
    }

    char dirs[128][512];
    int total = 0;

    /* 列出 AI 自动备份 */
    int pre_count = get_backup_dirs(PRE_REPAIR_PREFIX, dirs, 64);
    for (int i = 0; i < pre_count; i++) {
        struct stat st;
        if (stat(dirs[i], &st) == 0) {
            char time_str[32];
            struct tm *tm = localtime(&st.st_mtime);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
            char *name = strrchr(dirs[i], '/');
            name = name ? name + 1 : dirs[i];
            uart_puts(tr("  [Auto] ", "[自动] "));
            uart_puts(name);
            uart_puts("  (");
            uart_puts(time_str);
            uart_puts(")\n");
            total++;
        }
    }

    /* 列出手动备份 */
    int manual_count = get_backup_dirs(MANUAL_PREFIX, dirs, 64);
    for (int i = 0; i < manual_count; i++) {
        struct stat st;
        if (stat(dirs[i], &st) == 0) {
            char time_str[32];
            struct tm *tm = localtime(&st.st_mtime);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
            char *name = strrchr(dirs[i], '/');
            name = name ? name + 1 : dirs[i];
            uart_puts(tr("  [Manual] ", "[手动] "));
            uart_puts(name);
            uart_puts("  (");
            uart_puts(time_str);
            uart_puts(")\n");
            total++;
        }
    }

    if (total == 0) {
        uart_puts(tr("  No backups found.\n", "  未找到备份。\n"));
    } else {
        char buf[32];
        safe_snprintf(buf, sizeof(buf), "%d", total);
        uart_puts(tr("Total: ", "总计："));
        uart_puts(buf);
        uart_puts(tr(" backups\n", " 个备份\n"));
    }
}

/* ============================================================
 * 新增函数：恢复最新备份
 * ============================================================ */
int backup_restore_latest(void) {
    LOG_INFO_T("Backup", "RestoreLatest", "Enter", "restoring latest backup");

    char backup_root[512];
    safe_snprintf(backup_root, sizeof(backup_root), "%s", BACKUP_ROOT);

    if (access(backup_root, F_OK) != 0) {
        LOG_WARN_T("Backup", "RestoreLatest", "NoBackups", "no backup directory found");
        return -1;
    }

    char dirs[128][512];
    int count = 0;

    /* 收集所有备份 */
    int pre_count = get_backup_dirs(PRE_REPAIR_PREFIX, dirs, 64);
    int manual_count = get_backup_dirs(MANUAL_PREFIX, dirs + pre_count, 64);
    count = pre_count + manual_count;

    if (count == 0) {
        LOG_WARN_T("Backup", "RestoreLatest", "NoBackups", "no backups found");
        return -1;
    }

    /* 找最新的备份 */
    char latest[512] = {0};
    time_t latest_time = 0;

    for (int i = 0; i < count; i++) {
        struct stat st;
        if (stat(dirs[i], &st) == 0) {
            if (st.st_mtime > latest_time) {
                latest_time = st.st_mtime;
                safe_strncpy(latest, dirs[i], sizeof(latest));
            }
        }
    }

    if (latest[0] == '\0') {
        LOG_WARN_T("Backup", "RestoreLatest", "NoValid", "no valid backup found");
        return -1;
    }

    LOG_INFO_T("Backup", "RestoreLatest", "Found", "latest backup: %s", latest);
    return restore_backup(latest);
}