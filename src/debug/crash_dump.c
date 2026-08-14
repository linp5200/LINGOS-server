/**
 * @file    crash_dump.c
 * @brief   崩溃转储收集完整实现（proot 兼容）
 * @version LN-B-5.0.0.0
 * @changes 解决 safe_run_command 函数冲突，重命名为 safe_run_cmd_to_file
 */

#include "crash_dump.h"
#include "proot_detect.h"
#include "common/data_path.h"
#include "common/safe_string.h"
#include "common/lang.h"
#include "lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

#define DUMP_BASE_DIR "/LINGOS/Dump"
#define DUMP_POLICY_FILE "/LINGOS/system/config/dump_policy.conf"

/* ============================================================
 * 辅助函数
 * ============================================================ */

static void get_timestamp(char *buf, size_t size) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, size, "%Y%m%d_%H%M%S", &tm);
}

/* ============================================================
 * 安全执行命令并写入文件（重命名避免与 proot_detect.h 冲突）
 * ============================================================ */
static int safe_run_cmd_to_file(const char *cmd, const char *output_file) {
    LOG_DEBUG_T("CrashDump", "RunCmd", "Enter", "cmd='%s', output='%s'", cmd, output_file);

    if (!cmd || !output_file) {
        LOG_ERROR_T("CrashDump", "RunCmd", "Invalid", "cmd or output_file is NULL");
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        LOG_ERROR_T("CrashDump", "RunCmd", "ForkFail", "fork failed: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }

    if (pid == 0) {
        FILE *fp = fopen(output_file, "w");
        if (fp) {
            dup2(fileno(fp), STDOUT_FILENO);
            dup2(fileno(fp), STDERR_FILENO);
            fclose(fp);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(1);
    } else {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            LOG_DEBUG_T("CrashDump", "RunCmd", "OK", "command executed successfully");
            return 0;
        } else {
            LOG_WARN_T("CrashDump", "RunCmd", "Fail", "command failed with status %d", status);
            return -1;
        }
    }
}

static void create_dir(const char *path) {
    if (!path) return;
    char tmp[512];
    char *p = NULL;
    size_t len;

    safe_snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void copy_dir(const char *src, const char *dst) {
    if (!src || !dst) return;
    char cmd[1024];
    safe_snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s' 2>/dev/null", src, dst);
    safe_run_cmd_to_file(cmd, "/dev/null");
}

static int load_dump_policy(void) {
    FILE *fp = fopen(DUMP_POLICY_FILE, "r");
    if (!fp) return -1;
    char line[256];
    int result = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "include_user_data=", 18) == 0) {
            char *val = line + 18;
            while (*val == ' ' || *val == '\t') val++;
            if (*val == '1' || *val == 'y' || *val == 'Y' ||
                strncmp(val, "true", 4) == 0 || strncmp(val, "on", 2) == 0) {
                result = 1;
            } else if (*val == '0' || *val == 'n' || *val == 'N' ||
                       strncmp(val, "false", 5) == 0 || strncmp(val, "off", 3) == 0) {
                result = 0;
            }
            break;
        }
    }
    fclose(fp);
    return result;
}

static int should_include_user_data(const char *signal_name, const char *reason) {
    int policy = load_dump_policy();
    if (policy == 1) return 1;
    if (policy == 0) return 0;

    if (signal_name) {
        if (strstr(signal_name, "SEGV") || strstr(signal_name, "BUS") ||
            strstr(signal_name, "ABRT") || strstr(signal_name, "FPE") ||
            strstr(signal_name, "ILL")) {
        } else {
            if (reason && strstr(reason, "user")) {
                return 1;
            }
        }
    }

    if (reason) {
        const char *keywords[] = {
            "memory", "session", "user_data", "conversation",
            "memory_write", "memory_read", "memory_search",
            "user", "nook ask", "chat", "skill"
        };
        for (int i = 0; i < (int)(sizeof(keywords) / sizeof(keywords[0])); i++) {
            if (strstr(reason, keywords[i])) {
                return 1;
            }
        }
    }
    return 0;
}

static void create_index_file(const char *dir, const char *signal_name, const char *reason) {
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/crash.info", dir);
    FILE *fp = fopen(path, "w");
    if (!fp) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(fp, "=== LING OS Crash Dump ===\n");
    fprintf(fp, "Timestamp: %s\n", time_str);
    fprintf(fp, "PID: %d\n", getpid());
    fprintf(fp, "Signal: %s\n", signal_name ? signal_name : "UNKNOWN");
    fprintf(fp, "Reason: %s\n", reason ? reason : "Unknown");
    fprintf(fp, "Include user data: %s\n",
            should_include_user_data(signal_name, reason) ? "Yes" : "No");
    fprintf(fp, "==========================\n");
    fclose(fp);
}

/* ============================================================
 * 主函数
 * ============================================================ */

void collect_system_dump(const char *signal_name, const char *reason) {
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    char temp_dir[512];
    safe_snprintf(temp_dir, sizeof(temp_dir), "/tmp/dump_%d_%s", getpid(), timestamp);
    create_dir(temp_dir);

    char filepath[512];

    safe_snprintf(filepath, sizeof(filepath), "%s/ps_aux.txt", temp_dir);
    safe_run_cmd_to_file("ps aux", filepath);

    safe_snprintf(filepath, sizeof(filepath), "%s/free.txt", temp_dir);
    safe_run_cmd_to_file("free -h", filepath);

    safe_snprintf(filepath, sizeof(filepath), "%s/df.txt", temp_dir);
    safe_run_cmd_to_file("df -h", filepath);

    safe_snprintf(filepath, sizeof(filepath), "%s/uptime.txt", temp_dir);
    safe_run_cmd_to_file("uptime", filepath);

    safe_snprintf(filepath, sizeof(filepath), "%s/dmesg.txt", temp_dir);
    if (is_in_proot()) {
        safe_run_cmd_to_file("cat /proc/version 2>/dev/null || echo 'proot: dmesg unavailable'", filepath);
    } else {
        safe_run_cmd_to_file("dmesg | tail -100 2>/dev/null || echo 'dmesg unavailable'", filepath);
    }

    safe_snprintf(filepath, sizeof(filepath), "%s/meminfo.txt", temp_dir);
    safe_run_cmd_to_file("cat /proc/meminfo", filepath);

    safe_snprintf(filepath, sizeof(filepath), "%s/cpuinfo.txt", temp_dir);
    safe_run_cmd_to_file("cat /proc/cpuinfo", filepath);

    safe_snprintf(filepath, sizeof(filepath), "%s/lingos_ls.txt", temp_dir);
    safe_run_cmd_to_file("ls -la /LINGOS", filepath);

    safe_snprintf(filepath, sizeof(filepath), "%s/logs", temp_dir);
    copy_dir("/LINGOS/Debug", filepath);

    safe_snprintf(filepath, sizeof(filepath), "%s/config", temp_dir);
    copy_dir("/LINGOS/system/config", filepath);

    if (should_include_user_data(signal_name, reason)) {
        LOG_INFO_T("CrashDump", "UserData", "Included", "User data included in dump");
        safe_snprintf(filepath, sizeof(filepath), "%s/user_data/ai_memory", temp_dir);
        copy_dir("/LINGOS/data/ai_memory", filepath);
        safe_snprintf(filepath, sizeof(filepath), "%s/user_data/sessions", temp_dir);
        copy_dir("/LINGOS/data/sessions", filepath);
    } else {
        LOG_INFO_T("CrashDump", "UserData", "Excluded", "User data excluded from dump");
    }

    create_index_file(temp_dir, signal_name, reason);

    char dump_dir[512];
    safe_snprintf(dump_dir, sizeof(dump_dir), "%s", DUMP_BASE_DIR);
    create_dir(dump_dir);

    char tar_file[512];
    safe_snprintf(tar_file, sizeof(tar_file), "%s/crash_%s.tar.gz", dump_dir, timestamp);

    /* 使用 fork+execvp 打包 */
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {
            "tar", "-czf", tar_file,
            "-C", temp_dir, ".",
            (char *)NULL
        };
        execvp("tar", argv);
        _exit(1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }

    /* 清理临时目录 */
    char cmd[512];
    safe_snprintf(cmd, sizeof(cmd), "rm -rf '%s'", temp_dir);
    safe_run_cmd_to_file(cmd, "/dev/null");

    if (access(tar_file, F_OK) == 0) {
        LOG_INFO_T("CrashDump", "Complete", "OK", "Dump saved to %s", tar_file);
    } else {
        LOG_WARN_T("CrashDump", "Complete", "Fail", "tar failed, saving minimal dump");
        char fallback_file[512];
        safe_snprintf(fallback_file, sizeof(fallback_file), "%s/crash_%s_fallback.txt", dump_dir, timestamp);
        FILE *fp = fopen(fallback_file, "w");
        if (fp) {
            fprintf(fp, "=== LING OS Crash Dump (Fallback) ===\n");
            fprintf(fp, "Timestamp: %s\n", timestamp);
            fprintf(fp, "PID: %d\n", getpid());
            fprintf(fp, "Signal: %s\n", signal_name ? signal_name : "UNKNOWN");
            fprintf(fp, "Reason: %s\n", reason ? reason : "Unknown");
            fprintf(fp, "====================================\n");
            fclose(fp);
            LOG_INFO_T("CrashDump", "Fallback", "OK", "Fallback dump saved to %s", fallback_file);
        }
    }
}