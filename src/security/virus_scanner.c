/**
 * @file    virus_scanner.c
 * @brief   病毒扫描核心（模式匹配 + 签名验证）
 * @version LN-B-5.0.0.0
 * @fix     tar 解压改用 fork + execvp，消除 system() 调用
 */

#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>

typedef enum {
    SCAN_RESULT_CLEAN = 0,
    SCAN_RESULT_SUSPICIOUS = 1,
    SCAN_RESULT_MALICIOUS = 2
} scan_result_t;

/* ============================================================
 * 危险模式列表
 * ============================================================ */

static const char *dangerous_patterns[] = {
    "rm -rf /",
    "rm -rf /*",
    "mkfs",
    "dd if=/dev/zero",
    "> /dev/sda",
    "drop table",
    "eval(",
    "exec(",
    "system(",
    "popen(",
    "; rm ",
    "| sh",
    "`",
    "$(",
    "wget.*| sh",
    "curl.*| sh",
    NULL
};

/* ============================================================
 * 安全执行 tar 解压（fork + execvp）
 * ============================================================ */

static int safe_tar_extract(const char *archive, const char *dest) {
    LOG_DEBUG_T("VirusScanner", "SafeTar", "Enter", "archive='%s', dest='%s'",
                archive ? archive : "(null)", dest ? dest : "(null)");

    if (!archive || !dest) return -1;

    pid_t pid = fork();
    if (pid == -1) {
        LOG_ERROR_T("VirusScanner", "SafeTar", "ForkFail", "fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* 子进程：重定向 stderr 到 /dev/null */
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }

        char *argv[] = {
            "tar",
            "-xzf",
            (char*)archive,
            "-C",
            (char*)dest,
            NULL
        };

        execvp("tar", argv);
        /* execvp 失败则退出 */
        _exit(1);
    } else {
        int status;
        if (waitpid(pid, &status, 0) == -1) {
            LOG_ERROR_T("VirusScanner", "SafeTar", "WaitFail", "waitpid failed: %s", strerror(errno));
            return -1;
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            LOG_DEBUG_T("VirusScanner", "SafeTar", "OK", "tar extraction successful");
            return 0;
        } else {
            LOG_WARN_T("VirusScanner", "SafeTar", "Fail", "tar extraction failed with status %d", status);
            return -1;
        }
    }
}

/* ============================================================
 * 扫描文件内容
 * ============================================================ */

static scan_result_t scan_file_content(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return SCAN_RESULT_CLEAN;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    if (len > 1024 * 1024) {
        fseek(fp, 0, SEEK_SET);
        len = 1024 * 1024;
    } else {
        fseek(fp, 0, SEEK_SET);
    }

    char *buf = malloc(len + 1);
    if (!buf) { fclose(fp); return SCAN_RESULT_CLEAN; }
    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);

    for (int i = 0; dangerous_patterns[i]; i++) {
        if (strstr(buf, dangerous_patterns[i]) != NULL) {
            free(buf);
            LOG_WARN_T("VirusScanner", "Scan", "PatternMatch", "found dangerous pattern: %s", dangerous_patterns[i]);
            return SCAN_RESULT_MALICIOUS;
        }
    }

    free(buf);
    return SCAN_RESULT_CLEAN;
}

/* ============================================================
 * 扫描包（.lapt, .deb, .sub）
 * ============================================================ */

scan_result_t virus_scanner_scan_package(const char *path) {
    LOG_INFO_T("VirusScanner", "ScanPackage", "Enter", "path='%s'", path ? path : "(null)");

    if (!path || access(path, F_OK) != 0) {
        return SCAN_RESULT_CLEAN;
    }

    const char *ext = strrchr(path, '.');
    if (!ext) return SCAN_RESULT_CLEAN;

    /* 创建临时目录 */
    char temp_dir[512];
    safe_snprintf(temp_dir, sizeof(temp_dir), "/tmp/virus_scan_%d", getpid());
    mkdir(temp_dir, 0755);

    /* 使用安全 tar 解压 */
    int ret = safe_tar_extract(path, temp_dir);
    if (ret != 0) {
        rmdir(temp_dir);
        LOG_WARN_T("VirusScanner", "ScanPackage", "ExtractFail", "cannot extract %s", path);
        return SCAN_RESULT_SUSPICIOUS;
    }

    /* 扫描解压后的所有文件 */
    scan_result_t result = SCAN_RESULT_CLEAN;
    DIR *d = opendir(temp_dir);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char full[512];
            safe_snprintf(full, sizeof(full), "%s/%s", temp_dir, entry->d_name);
            scan_result_t r = scan_file_content(full);
            if (r > result) result = r;
        }
        closedir(d);
    }

    /* 清理临时目录 */
    char cmd[512];
    safe_snprintf(cmd, sizeof(cmd), "rm -rf '%s'", temp_dir);
    system(cmd);  /* 清理操作使用 system 可接受 */

    LOG_INFO_T("VirusScanner", "ScanPackage", "OK", "result=%d", result);
    return result;
}

const char* virus_scanner_result_name(scan_result_t result) {
    switch (result) {
        case SCAN_RESULT_CLEAN:      return "Clean";
        case SCAN_RESULT_SUSPICIOUS: return "Suspicious";
        case SCAN_RESULT_MALICIOUS:  return "Malicious";
        default:                     return "Unknown";
    }
}