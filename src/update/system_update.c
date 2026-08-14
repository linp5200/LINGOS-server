/**
 * @file    system_update.c
 * @brief   系统更新核心：内核更新（.sub）、组件更新（.latp）、修复包、回滚
 * @version LN-B-5.0.0.0
 * @changes fork+execvp 替代 system()；配置保留（security.json/privilege.json）；
 *          注册表备份；安全字符串替换；双文支持
 */

#include "system_update.h"
#include "log_extra.h"
#include "data_path.h"
#include "version.h"
#include "audit.h"
#include "web_update.h"
#include "manifest.h"
#include "backup.h"
#include "safe_string.h"
#include "lang.h"
#include "uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <dirent.h>
#include <fcntl.h>

#define BACKUP_DIR "/LINGOS/backups/bin_"
#define TEMP_DIR "/tmp/lingos_update_XXXXXX"
#define KERNEL_BIN_MAIN "/usr/bin/lingos_linux"
#define KERNEL_BIN_DAEMON "/usr/bin/lingosd"

/* ============================================================
 * 内部辅助：安全执行命令（fork+execvp 替代 system）
 * ============================================================ */
static int safe_exec(const char *cmd, char *const argv[]) {
    LOG_DEBUG_T("Update", "SafeExec", "Enter", "cmd='%s'", cmd ? cmd : "(null)");

    if (!cmd || !argv) {
        LOG_ERROR_T("Update", "SafeExec", "Invalid", "cmd or argv is NULL");
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        LOG_ERROR_T("Update", "SafeExec", "ForkFail", "fork failed: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }

    if (pid == 0) {
        execvp(cmd, argv);
        _exit(1);
    } else {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            LOG_DEBUG_T("Update", "SafeExec", "OK", "command executed successfully");
            return 0;
        } else {
            LOG_WARN_T("Update", "SafeExec", "Fail", "command failed with status %d", status);
            return -1;
        }
    }
}

static int safe_exec_sh(const char *cmd_str) {
    char *argv[] = {"/bin/sh", "-c", (char*)cmd_str, NULL};
    return safe_exec("/bin/sh", argv);
}

/* ============================================================
 * 内部辅助：递归创建目录
 * ============================================================ */
static int mkdir_p(const char *path) {
    LOG_DEBUG_T("Update", "MkdirP", "Enter", "path='%s'", path ? path : "(null)");
    if (!path) {
        LOG_ERROR_T("Update", "MkdirP", "Invalid", "path is NULL");
        return -1;
    }

    char tmp[512];
    char *p = NULL;
    size_t len;

    safe_snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                LOG_ERROR_T("Update", "MkdirP", "Fail", "mkdir %s failed: %s (errno=%d)", tmp, strerror(errno), errno);
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        LOG_ERROR_T("Update", "MkdirP", "Fail", "mkdir %s failed: %s (errno=%d)", tmp, strerror(errno), errno);
        return -1;
    }

    LOG_DEBUG_T("Update", "MkdirP", "OK", "created directory '%s'", path);
    return 0;
}

/* ============================================================
 * 内部辅助：从备份目录名提取时间戳
 * ============================================================ */
static long long get_timestamp_from_name(const char *name) {
    LOG_DEBUG_T("Update", "GetTimestamp", "Enter", "name='%s'", name ? name : "(null)");
    if (!name || !*name) {
        LOG_WARN_T("Update", "GetTimestamp", "Invalid", "name is NULL or empty");
        return 0;
    }

    const char *prefix = "bin_";
    if (strncmp(name, prefix, strlen(prefix)) != 0) {
        LOG_DEBUG_T("Update", "GetTimestamp", "NoPrefix", "name does not start with 'bin_'");
        return 0;
    }

    const char *ts_str = name + strlen(prefix);
    if (strlen(ts_str) < 15) {
        LOG_DEBUG_T("Update", "GetTimestamp", "TooShort", "timestamp string too short: '%s'", ts_str);
        return 0;
    }

    char num_str[32] = {0};
    int idx = 0;
    for (int i = 0; ts_str[i] && i < 15; i++) {
        if (ts_str[i] >= '0' && ts_str[i] <= '9') {
            num_str[idx++] = ts_str[i];
        }
    }
    num_str[idx] = '\0';

    if (idx < 14) {
        LOG_DEBUG_T("Update", "GetTimestamp", "Invalid", "only %d digits found in '%s'", idx, ts_str);
        return 0;
    }

    long long ts = atoll(num_str);
    LOG_DEBUG_T("Update", "GetTimestamp", "OK", "timestamp=%lld from '%s'", ts, name);
    return ts;
}

/* ============================================================
 * 内部辅助：获取最新备份目录
 * ============================================================ */
static int get_latest_backup(char *out_path, size_t path_len) {
    LOG_DEBUG_T("Update", "GetLatestBackup", "Enter", "out_path=%p, path_len=%zu", (void*)out_path, path_len);
    if (!out_path || path_len == 0) {
        LOG_ERROR_T("Update", "GetLatestBackup", "Invalid", "out_path=%p, path_len=%zu", (void*)out_path, path_len);
        return -1;
    }

    DIR *d = opendir(BACKUP_DIR);
    if (!d) {
        LOG_WARN_T("Update", "GetLatestBackup", "OpenFail", "cannot open %s: %s (errno=%d)", BACKUP_DIR, strerror(errno), errno);
        return -1;
    }

    struct dirent *entry;
    char latest_dir[512] = {0};
    long long latest_ts = 0;

    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (strncmp(entry->d_name, "bin_", 4) != 0) continue;

        char full_path[512];
        safe_snprintf(full_path, sizeof(full_path), "%s%s", BACKUP_DIR, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) {
            LOG_WARN_T("Update", "GetLatestBackup", "StatFail", "stat(%s) failed: %s", full_path, strerror(errno));
            continue;
        }
        if (!S_ISDIR(st.st_mode)) continue;

        long long ts = get_timestamp_from_name(entry->d_name);
        if (ts == 0) {
            ts = (long long)st.st_mtime;
        }

        if (ts > latest_ts) {
            latest_ts = ts;
            safe_snprintf(latest_dir, sizeof(latest_dir), "%s", full_path);
            LOG_DEBUG_T("Update", "GetLatestBackup", "NewLatest", "new latest: %s (ts=%lld)", full_path, ts);
        }
    }
    closedir(d);

    if (latest_dir[0] == '\0') {
        LOG_WARN_T("Update", "GetLatestBackup", "NotFound", "no backup directories found");
        return -1;
    }

    safe_snprintf(out_path, path_len, "%s", latest_dir);
    LOG_INFO_T("Update", "GetLatestBackup", "OK", "latest backup: %s", out_path);
    return 0;
}

/* ============================================================
 * 【修改】备份当前二进制（使用 fork+execvp）
 * ============================================================ */
static int backup_current_binaries(char *backup_path, size_t path_len) {
    LOG_DEBUG_T("Update", "BackupBin", "Enter", "backup_path=%p, path_len=%zu", (void*)backup_path, path_len);
    if (!backup_path || path_len == 0) {
        LOG_ERROR_T("Update", "BackupBin", "Invalid", "backup_path=%p, path_len=%zu", (void*)backup_path, path_len);
        return -1;
    }

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
    safe_snprintf(backup_path, path_len, "%s%s", BACKUP_DIR, ts);
    LOG_DEBUG_T("Update", "BackupBin", "BackupDir", "backup_dir='%s'", backup_path);

    if (mkdir_p(backup_path) != 0) {
        LOG_ERROR_T("Update", "BackupBin", "MkdirFail", "cannot create backup dir %s", backup_path);
        return -1;
    }

    char src[256], dst[256];
    char *cp_argv[5];

    safe_snprintf(src, sizeof(src), "%s", KERNEL_BIN_MAIN);
    safe_snprintf(dst, sizeof(dst), "%s/lingos_linux", backup_path);
    if (access(src, F_OK) == 0) {
        LOG_DEBUG_T("Update", "BackupBin", "BackupMain", "copying %s to %s", src, dst);
        cp_argv[0] = "/bin/cp";
        cp_argv[1] = (char*)src;
        cp_argv[2] = (char*)dst;
        cp_argv[3] = NULL;
        if (safe_exec("/bin/cp", cp_argv) != 0) {
            LOG_ERROR_T("Update", "BackupBin", "CopyMainFail", "cp %s failed", src);
            return -1;
        }
    } else {
        LOG_DEBUG_T("Update", "BackupBin", "MainNotExist", "%s not found, skipping", src);
    }

    safe_snprintf(src, sizeof(src), "%s", KERNEL_BIN_DAEMON);
    safe_snprintf(dst, sizeof(dst), "%s/lingosd", backup_path);
    if (access(src, F_OK) == 0) {
        LOG_DEBUG_T("Update", "BackupBin", "BackupDaemon", "copying %s to %s", src, dst);
        cp_argv[0] = "/bin/cp";
        cp_argv[1] = (char*)src;
        cp_argv[2] = (char*)dst;
        cp_argv[3] = NULL;
        if (safe_exec("/bin/cp", cp_argv) != 0) {
            LOG_ERROR_T("Update", "BackupBin", "CopyDaemonFail", "cp %s failed", src);
            return -1;
        }
    } else {
        LOG_DEBUG_T("Update", "BackupBin", "DaemonNotExist", "%s not found, skipping", src);
    }

    LOG_INFO_T("Update", "BackupBin", "OK", "binary backup to %s", backup_path);
    return 0;
}

/* ============================================================
 * 内部辅助：提取更新包（使用 fork+execvp）
 * ============================================================ */
static int extract_package(const char *pkg_path, char *extract_dir, size_t dir_len) {
    LOG_DEBUG_T("Update", "ExtractPackage", "Enter", "pkg_path='%s', dir_len=%zu", pkg_path ? pkg_path : "(null)", dir_len);
    if (!pkg_path || !extract_dir || dir_len == 0) {
        LOG_ERROR_T("Update", "ExtractPackage", "Invalid", "pkg_path=%p, extract_dir=%p, dir_len=%zu",
                    (void*)pkg_path, (void*)extract_dir, dir_len);
        return -1;
    }

    char tmp_pattern[] = TEMP_DIR;
    char *tmp = mkdtemp(tmp_pattern);
    if (!tmp) {
        LOG_ERROR_T("Update", "ExtractPackage", "MkdtempFail", "mkdtemp failed: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }
    LOG_DEBUG_T("Update", "ExtractPackage", "TempDir", "temp_dir='%s'", tmp);

    safe_strncpy(extract_dir, tmp, dir_len);
    extract_dir[dir_len - 1] = '\0';

    char *tar_argv[] = {"/bin/tar", "-xzf", (char*)pkg_path, "-C", extract_dir, NULL};
    if (safe_exec("/bin/tar", tar_argv) != 0) {
        LOG_ERROR_T("Update", "ExtractPackage", "TarFail", "tar -xzf %s to %s failed", pkg_path, extract_dir);
        rmdir(extract_dir);
        return -1;
    }

    LOG_INFO_T("Update", "ExtractPackage", "OK", "extracted to %s", extract_dir);
    return 0;
}

/* ============================================================
 * 【修改】安装二进制（使用 fork+execvp）
 * ============================================================ */
static int install_binaries(const char *dir, const char *source_type) {
    LOG_DEBUG_T("Update", "InstallBin", "Enter", "dir='%s', source_type='%s'", dir ? dir : "(null)", source_type ? source_type : "(null)");
    if (!dir || !source_type) {
        LOG_ERROR_T("Update", "InstallBin", "Invalid", "dir=%p, source_type=%p", (void*)dir, (void*)source_type);
        return -1;
    }

    char src_path[512], dst_path[512];
    char *cp_argv[5];

    if (strcmp(source_type, "binary") == 0) {
        LOG_DEBUG_T("Update", "InstallBin", "TypeBinary", "installing pre-built binaries");

        safe_snprintf(src_path, sizeof(src_path), "%s/lingos_linux", dir);
        safe_snprintf(dst_path, sizeof(dst_path), "%s", KERNEL_BIN_MAIN);
        if (access(src_path, F_OK) == 0) {
            LOG_DEBUG_T("Update", "InstallBin", "InstallMain", "copying %s to %s", src_path, dst_path);
            cp_argv[0] = "/bin/cp";
            cp_argv[1] = src_path;
            cp_argv[2] = dst_path;
            cp_argv[3] = NULL;
            if (safe_exec("/bin/cp", cp_argv) != 0) {
                LOG_ERROR_T("Update", "InstallBin", "MainFail", "install lingos_linux failed");
                return -1;
            }
            chmod(dst_path, 0755);
        } else {
            LOG_DEBUG_T("Update", "InstallBin", "MainNotExist", "%s not found", src_path);
        }

        safe_snprintf(src_path, sizeof(src_path), "%s/lingosd", dir);
        safe_snprintf(dst_path, sizeof(dst_path), "%s", KERNEL_BIN_DAEMON);
        if (access(src_path, F_OK) == 0) {
            LOG_DEBUG_T("Update", "InstallBin", "InstallDaemon", "copying %s to %s", src_path, dst_path);
            cp_argv[0] = "/bin/cp";
            cp_argv[1] = src_path;
            cp_argv[2] = dst_path;
            cp_argv[3] = NULL;
            if (safe_exec("/bin/cp", cp_argv) != 0) {
                LOG_ERROR_T("Update", "InstallBin", "DaemonFail", "install lingosd failed");
                return -1;
            }
            chmod(dst_path, 0755);
        } else {
            LOG_DEBUG_T("Update", "InstallBin", "DaemonNotExist", "%s not found", src_path);
        }

        LOG_INFO_T("Update", "InstallBin", "OK", "binary installation completed");
        return 0;

    } else if (strcmp(source_type, "source") == 0) {
        LOG_DEBUG_T("Update", "InstallBin", "TypeSource", "building from source");

        char build_script[512];
        safe_snprintf(build_script, sizeof(build_script), "%s/build.sh", dir);
        if (access(build_script, F_OK) == 0) {
            LOG_DEBUG_T("Update", "InstallBin", "BuildScript", "executing build.sh");
            char *sh_argv[] = {"/bin/sh", build_script, NULL};
            if (safe_exec("/bin/sh", sh_argv) != 0) {
                LOG_ERROR_T("Update", "InstallBin", "BuildScriptFail", "build.sh failed");
                return -1;
            }
        } else {
            LOG_DEBUG_T("Update", "InstallBin", "MakeBuild", "executing make");
            char *make_argv[] = {"/usr/bin/make", "lingos_linux", "lingosd", NULL};
            if (safe_exec("/usr/bin/make", make_argv) != 0) {
                LOG_ERROR_T("Update", "InstallBin", "MakeFail", "make failed");
                return -1;
            }
        }

        safe_snprintf(src_path, sizeof(src_path), "%s/lingos_linux", dir);
        safe_snprintf(dst_path, sizeof(dst_path), "%s", KERNEL_BIN_MAIN);
        if (access(src_path, F_OK) == 0) {
            cp_argv[0] = "/bin/cp";
            cp_argv[1] = src_path;
            cp_argv[2] = dst_path;
            cp_argv[3] = NULL;
            if (safe_exec("/bin/cp", cp_argv) != 0) {
                LOG_ERROR_T("Update", "InstallBin", "MainFail", "install lingos_linux failed");
                return -1;
            }
            chmod(dst_path, 0755);
        }

        safe_snprintf(src_path, sizeof(src_path), "%s/lingosd", dir);
        safe_snprintf(dst_path, sizeof(dst_path), "%s", KERNEL_BIN_DAEMON);
        if (access(src_path, F_OK) == 0) {
            cp_argv[0] = "/bin/cp";
            cp_argv[1] = src_path;
            cp_argv[2] = dst_path;
            cp_argv[3] = NULL;
            if (safe_exec("/bin/cp", cp_argv) != 0) {
                LOG_ERROR_T("Update", "InstallBin", "DaemonFail", "install lingosd failed");
                return -1;
            }
            chmod(dst_path, 0755);
        }

        LOG_INFO_T("Update", "InstallBin", "OK", "source build and installation completed");
        return 0;

    } else {
        LOG_ERROR_T("Update", "InstallBin", "UnknownType", "unknown source_type='%s'", source_type);
        return -1;
    }
}

/* ============================================================
 * 【新增】备份注册表和关键配置
 * ============================================================ */
static int backup_registry_and_config(const char *backup_dir) {
    LOG_DEBUG_T("Update", "BackupRegistry", "Enter", "backup_dir='%s'", backup_dir);

    const char *root = lingos_data_root();
    char src_registry[512], dst_registry[512];
    char src_security[512], dst_security[512];

    safe_snprintf(src_registry, sizeof(src_registry), "%s/registry/core/registry.json", root);
    safe_snprintf(dst_registry, sizeof(dst_registry), "%s/registry.json", backup_dir);
    safe_snprintf(src_security, sizeof(src_security), "%s/system/config/security.json", root);
    safe_snprintf(dst_security, sizeof(dst_security), "%s/security.json", backup_dir);

    char *cp_argv[5];

    if (access(src_registry, F_OK) == 0) {
        cp_argv[0] = "/bin/cp";
        cp_argv[1] = src_registry;
        cp_argv[2] = dst_registry;
        cp_argv[3] = NULL;
        if (safe_exec("/bin/cp", cp_argv) != 0) {
            LOG_WARN_T("Update", "BackupRegistry", "RegistryFail", "failed to backup registry");
        } else {
            LOG_DEBUG_T("Update", "BackupRegistry", "RegistryOK", "registry backed up to %s", dst_registry);
        }
    }

    if (access(src_security, F_OK) == 0) {
        cp_argv[0] = "/bin/cp";
        cp_argv[1] = src_security;
        cp_argv[2] = dst_security;
        cp_argv[3] = NULL;
        if (safe_exec("/bin/cp", cp_argv) != 0) {
            LOG_WARN_T("Update", "BackupRegistry", "SecurityFail", "failed to backup security.json");
        } else {
            LOG_DEBUG_T("Update", "BackupRegistry", "SecurityOK", "security.json backed up to %s", dst_security);
        }
    }

    return 0;
}

/* ============================================================
 * 内部辅助：重启守护进程
 * ============================================================ */
static int restart_daemon(void) {
    LOG_DEBUG_T("Update", "RestartDaemon", "Enter", "restarting lingosd");

    char *pkill_argv[] = {"/usr/bin/pkill", "lingosd", NULL};
    safe_exec("/usr/bin/pkill", pkill_argv);

    char *daemon_argv[] = {"./lingosd", NULL};
    if (safe_exec("./lingosd", daemon_argv) != 0) {
        LOG_ERROR_T("Update", "RestartDaemon", "StartFail", "cannot start lingosd");
        return -1;
    }

    LOG_INFO_T("Update", "RestartDaemon", "OK", "lingosd restarted");
    return 0;
}

/* ============================================================
 * 【修改】公共 API：安装更新包（含配置保留）
 * ============================================================ */
int system_update_install(const char *pkg_path) {
    LOG_INFO_T("Update", "Install", "Enter", "pkg_path='%s'", pkg_path ? pkg_path : "(null)");
    if (!pkg_path) {
        LOG_ERROR_T("Update", "Install", "Invalid", "pkg_path is NULL");
        return -1;
    }

    if (access(pkg_path, F_OK) != 0) {
        LOG_ERROR_T("Update", "Install", "NoPkg", "package not found: %s (errno=%d)", pkg_path, errno);
        return -1;
    }

    const char *ext = strrchr(pkg_path, '.');
    if (!ext) {
        LOG_ERROR_T("Update", "Install", "NoExt", "no extension in '%s'", pkg_path);
        return -1;
    }
    LOG_DEBUG_T("Update", "Install", "Extension", "extension='%s'", ext);

    char extract_dir[256];
    char version[64] = {0};
    char source_type[32] = {0};
    repair_meta_t repair_meta = {0};
    int is_repair = 0;

    if (strcmp(ext, ".sub") == 0) {
        LOG_INFO_T("Update", "Install", "Kernel", "processing kernel update: %s", pkg_path);

        if (extract_package(pkg_path, extract_dir, sizeof(extract_dir)) != 0) {
            LOG_ERROR_T("Update", "Install", "ExtractFail", "extract failed for %s", pkg_path);
            return -1;
        }
        LOG_DEBUG_T("Update", "Install", "Extracted", "extract_dir='%s'", extract_dir);

        if (manifest_parse_with_repair(extract_dir, version, sizeof(version),
                                       source_type, sizeof(source_type),
                                       &repair_meta) != 0) {
            LOG_ERROR_T("Update", "Install", "ParseFail", "parse system.json failed in %s", extract_dir);
            rmdir(extract_dir);
            return -1;
        }
        LOG_DEBUG_T("Update", "Install", "ParseOK", "version='%s', source_type='%s'", version, source_type);

        if (repair_meta.fingerprint[0] != '\0') {
            is_repair = 1;
            LOG_INFO_T("Update", "Install", "Repair", "repair package: reason='%s', fingerprint='%s'",
                       repair_meta.reason, repair_meta.fingerprint);
        }

        /* 备份整个系统 */
        char backup_path[256];
        if (backup_system(backup_path, sizeof(backup_path), 0) != 0) {
            LOG_ERROR_T("Update", "Install", "BackupSystemFail", "full system backup failed");
            rmdir(extract_dir);
            return -1;
        }
        LOG_INFO_T("Update", "Install", "BackupSystemOK", "system backup to %s", backup_path);

        /* 【新增】备份注册表和关键配置 */
        backup_registry_and_config(backup_path);

        /* 备份二进制 */
        char bin_backup_path[256];
        if (backup_current_binaries(bin_backup_path, sizeof(bin_backup_path)) != 0) {
            LOG_ERROR_T("Update", "Install", "BackupBinFail", "binary backup failed");
            LOG_WARN_T("Update", "Install", "RestoreSystem", "attempting to restore system backup");
            restore_backup(backup_path);
            rmdir(extract_dir);
            return -1;
        }
        LOG_INFO_T("Update", "Install", "BackupBinOK", "binary backup to %s", bin_backup_path);

        if (install_binaries(extract_dir, source_type) != 0) {
            LOG_ERROR_T("Update", "Install", "InstallFail", "binary installation failed");
            LOG_WARN_T("Update", "Install", "Rollback", "attempting rollback from binary backup");

            char src_main[512], dst_main[512];
            char src_daemon[512], dst_daemon[512];
            char *cp_argv[5];

            safe_snprintf(src_main, sizeof(src_main), "%s/lingos_linux", bin_backup_path);
            safe_snprintf(dst_main, sizeof(dst_main), "%s", KERNEL_BIN_MAIN);
            safe_snprintf(src_daemon, sizeof(src_daemon), "%s/lingosd", bin_backup_path);
            safe_snprintf(dst_daemon, sizeof(dst_daemon), "%s", KERNEL_BIN_DAEMON);

            int rollback_ok = 1;
            if (access(src_main, F_OK) == 0) {
                cp_argv[0] = "/bin/cp";
                cp_argv[1] = src_main;
                cp_argv[2] = dst_main;
                cp_argv[3] = NULL;
                if (safe_exec("/bin/cp", cp_argv) != 0) {
                    rollback_ok = 0;
                }
            }
            if (access(src_daemon, F_OK) == 0) {
                cp_argv[0] = "/bin/cp";
                cp_argv[1] = src_daemon;
                cp_argv[2] = dst_daemon;
                cp_argv[3] = NULL;
                if (safe_exec("/bin/cp", cp_argv) != 0) {
                    rollback_ok = 0;
                }
            }

            if (!rollback_ok) {
                LOG_ERROR_T("Update", "Install", "RollbackFail", "rollback failed, attempting system restore");
                restore_backup(backup_path);
            } else {
                LOG_INFO_T("Update", "Install", "RollbackOK", "rolled back to previous version");
            }
            rmdir(extract_dir);
            return -1;
        }

        /* 安装 Web UI 组件 */
        if (install_web_component(extract_dir) != 0) {
            LOG_WARN_T("Update", "Install", "WebFail", "web component update failed, but continuing");
        } else {
            LOG_DEBUG_T("Update", "Install", "WebOK", "web component installed");
        }

        /* 更新版本文件 */
        if (version_set(version) != 0) {
            LOG_WARN_T("Update", "Install", "VersionSetFail", "cannot update /LINGOS/version");
        } else {
            LOG_DEBUG_T("Update", "Install", "VersionSetOK", "version set to %s", version);
        }

        /* 【新增】恢复注册表和关键配置（保留用户配置） */
        const char *root = lingos_data_root();
        char dst_registry[512], dst_security[512];
        char src_registry[512], src_security[512];
        safe_snprintf(src_registry, sizeof(src_registry), "%s/registry.json", backup_path);
        safe_snprintf(dst_registry, sizeof(dst_registry), "%s/registry/core/registry.json", root);
        safe_snprintf(src_security, sizeof(src_security), "%s/security.json", backup_path);
        safe_snprintf(dst_security, sizeof(dst_security), "%s/system/config/security.json", root);

        char *cp_argv[5];

        if (access(src_registry, F_OK) == 0) {
            cp_argv[0] = "/bin/cp";
            cp_argv[1] = src_registry;
            cp_argv[2] = dst_registry;
            cp_argv[3] = NULL;
            safe_exec("/bin/cp", cp_argv);
            LOG_DEBUG_T("Update", "Install", "RegistryRestore", "registry restored");
        }

        if (access(src_security, F_OK) == 0) {
            cp_argv[0] = "/bin/cp";
            cp_argv[1] = src_security;
            cp_argv[2] = dst_security;
            cp_argv[3] = NULL;
            safe_exec("/bin/cp", cp_argv);
            LOG_DEBUG_T("Update", "Install", "SecurityRestore", "security.json restored");
        }

        if (restart_daemon() != 0) {
            LOG_WARN_T("Update", "Install", "DaemonRestartFail", "daemon restart failed, please restart manually");
        }

        rmdir(extract_dir);
        audit_log("system", "update", "kernel", pkg_path, "success", 0, "high", 1);
        LOG_INFO_T("Update", "Install", "Success", "kernel updated to %s", version);

        if (is_repair) {
            LOG_INFO_T("Update", "Install", "RepairSuccess", "repair applied successfully");
            char notify_path[256];
            safe_snprintf(notify_path, sizeof(notify_path), "/LINGOS/state/repair_success_%s", repair_meta.fingerprint);
            FILE *fp = fopen(notify_path, "w");
            if (fp) {
                fprintf(fp, "success\n");
                fclose(fp);
                LOG_DEBUG_T("Update", "Install", "NotifyWritten", "notify file: %s", notify_path);
            } else {
                LOG_WARN_T("Update", "Install", "NotifyFail", "cannot write notify file %s", notify_path);
            }
        }

        return 0;

    } else if (strcmp(ext, ".latp") == 0) {
        LOG_INFO_T("Update", "Install", "Component", "processing component update: %s", pkg_path);

        if (extract_package(pkg_path, extract_dir, sizeof(extract_dir)) != 0) {
            LOG_ERROR_T("Update", "Install", "ExtractFail", "extract failed for %s", pkg_path);
            return -1;
        }
        LOG_DEBUG_T("Update", "Install", "Extracted", "extract_dir='%s'", extract_dir);

        char *cp_argv[] = {"/bin/cp", "-r", NULL, "/LINGOS/", NULL};
        char src_arg[512];
        safe_snprintf(src_arg, sizeof(src_arg), "%s/*", extract_dir);
        cp_argv[2] = src_arg;
        if (safe_exec("/bin/cp", cp_argv) != 0) {
            LOG_ERROR_T("Update", "Component", "CopyFail", "copy to /LINGOS failed");
            rmdir(extract_dir);
            return -1;
        }
        LOG_DEBUG_T("Update", "Install", "CopyOK", "component files copied to /LINGOS");

        if (install_web_component(extract_dir) != 0) {
            LOG_WARN_T("Update", "Install", "WebFail", "web component update failed, but continuing");
        } else {
            LOG_DEBUG_T("Update", "Install", "WebOK", "web component installed");
        }

        rmdir(extract_dir);
        audit_log("system", "update", "component", pkg_path, "success", 0, "low", 1);
        LOG_INFO_T("Update", "Install", "Success", "component updated from %s", pkg_path);
        return 0;

    } else {
        LOG_ERROR_T("Update", "Install", "UnknownExt", "unknown extension '%s' in '%s'", ext, pkg_path);
        return -1;
    }
}

/* ============================================================
 * 【修改】公共 API：回滚（使用 fork+execvp）
 * ============================================================ */
int system_rollback(void) {
    LOG_INFO_T("Update", "Rollback", "Enter", "auto-selecting latest backup");

    char chosen_path[512] = {0};

    if (get_latest_backup(chosen_path, sizeof(chosen_path)) != 0) {
        LOG_ERROR_T("Update", "Rollback", "NoBackup", "no backup found for auto-rollback");
        return -1;
    }
    LOG_INFO_T("Update", "Rollback", "AutoSelected", "selected backup: %s", chosen_path);

    char src[512], dst[512];
    char *cp_argv[5];

    safe_snprintf(src, sizeof(src), "%s/lingos_linux", chosen_path);
    safe_snprintf(dst, sizeof(dst), "%s", KERNEL_BIN_MAIN);
    if (access(src, F_OK) == 0) {
        LOG_DEBUG_T("Update", "Rollback", "RestoreMain", "copying %s to %s", src, dst);
        cp_argv[0] = "/bin/cp";
        cp_argv[1] = src;
        cp_argv[2] = dst;
        cp_argv[3] = NULL;
        if (safe_exec("/bin/cp", cp_argv) != 0) {
            LOG_ERROR_T("Update", "Rollback", "RestoreMainFail", "restore lingos_linux failed");
            return -1;
        }
        chmod(dst, 0755);
    } else {
        LOG_DEBUG_T("Update", "Rollback", "MainNotExist", "lingos_linux not found in backup, skipping");
    }

    safe_snprintf(src, sizeof(src), "%s/lingosd", chosen_path);
    safe_snprintf(dst, sizeof(dst), "%s", KERNEL_BIN_DAEMON);
    if (access(src, F_OK) == 0) {
        LOG_DEBUG_T("Update", "Rollback", "RestoreDaemon", "copying %s to %s", src, dst);
        cp_argv[0] = "/bin/cp";
        cp_argv[1] = src;
        cp_argv[2] = dst;
        cp_argv[3] = NULL;
        if (safe_exec("/bin/cp", cp_argv) != 0) {
            LOG_ERROR_T("Update", "Rollback", "RestoreDaemonFail", "restore lingosd failed");
            return -1;
        }
        chmod(dst, 0755);
    } else {
        LOG_DEBUG_T("Update", "Rollback", "DaemonNotExist", "lingosd not found in backup, skipping");
    }

    if (restart_daemon() != 0) {
        LOG_WARN_T("Update", "Rollback", "DaemonRestartFail", "daemon restart failed, please restart manually");
    }

    audit_log("system", "rollback", "kernel", chosen_path, "success", 0, "high", 1);
    LOG_INFO_T("Update", "Rollback", "Success", "rolled back from %s", chosen_path);

    uart_puts(tr("\n✅ Rollback completed successfully.\n", "\n✅ 回滚成功完成。\n"));
    uart_puts(tr("📌 Please restart LING OS for the changes to take effect.\n",
                 "📌 请重启 LING OS 以使更改生效。\n"));
    uart_puts(tr("   You can restart by typing: reboot\n", "   您可以输入 reboot 重启。\n"));

    return 0;
}