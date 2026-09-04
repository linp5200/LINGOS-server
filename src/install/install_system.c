/**
 * @file    src/install/install_system.c
 * @brief   系统包安装实现
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 */

#include "install_system.h"
#include "install_speed.h"
#include "install_progress.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../common/distro_detect.h"
#include "../lib/log_extra.h"
#include "../drivers/uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

static char g_last_error[256] = {0};
static char g_system_mirror[256] = "";

/* ============================================================
 * 包名映射表
 * ============================================================ */
typedef struct pkg_map {
    const char *logical_name;
    const char *apt_name;
    const char *dnf_name;
    const char *pacman_name;
    const char *zypper_name;
    const char *apk_name;
} pkg_map_t;

static pkg_map_t g_pkg_maps[] = {
    {"libmosquitto-dev", "libmosquitto-dev", "libmosquitto-devel", "libmosquitto", "libmosquitto-devel", "libmosquitto-dev"},
    {"libsqlite3-dev", "libsqlite3-dev", "sqlite-devel", "sqlite", "sqlite-devel", "sqlite-dev"},
    {"libmicrohttpd-dev", "libmicrohttpd-dev", "libmicrohttpd-devel", "libmicrohttpd", "libmicrohttpd-devel", "libmicrohttpd-dev"},
    {"libcurl4-openssl-dev", "libcurl4-openssl-dev", "libcurl-devel", "curl", "libcurl-devel", "curl-dev"},
    {"libseccomp-dev", "libseccomp-dev", "libseccomp-devel", "libseccomp", "libseccomp-devel", "libseccomp-dev"},
    {"libnotcurses-dev", "libnotcurses-dev", "notcurses-devel", "notcurses", "notcurses-devel", "notcurses-dev"},
    {NULL, NULL, NULL, NULL, NULL, NULL}
};

static const char* get_real_pkg_name(const char *logical_name) {
    distro_info_t info = distro_detect();
    for (int i = 0; g_pkg_maps[i].logical_name; i++) {
        if (strcmp(g_pkg_maps[i].logical_name, logical_name) == 0) {
            switch (info.distro_type) {
                case DISTRO_DEBIAN:   return g_pkg_maps[i].apt_name;
                case DISTRO_FEDORA:   return g_pkg_maps[i].dnf_name;
                case DISTRO_ARCH:     return g_pkg_maps[i].pacman_name;
                case DISTRO_OPENSUSE: return g_pkg_maps[i].zypper_name;
                case DISTRO_ALPINE:   return g_pkg_maps[i].apk_name;
                default:              return g_pkg_maps[i].apt_name;
            }
        }
    }
    return NULL;
}

/* ============================================================
 * 检测包是否已安装
 * ============================================================ */
int install_system_is_installed(const char *pkg_name) {
    if (!pkg_name) return 0;
    distro_info_t info = distro_detect();
    char cmd[256];

    switch (info.distro_type) {
        case DISTRO_DEBIAN:
            safe_snprintf(cmd, sizeof(cmd), "dpkg -s %s 2>/dev/null | grep -q '^Status:.*installed'", pkg_name);
            break;
        case DISTRO_FEDORA:
        case DISTRO_OPENSUSE:
            safe_snprintf(cmd, sizeof(cmd), "rpm -q %s 2>/dev/null", pkg_name);
            break;
        case DISTRO_ARCH:
            safe_snprintf(cmd, sizeof(cmd), "pacman -Q %s 2>/dev/null", pkg_name);
            break;
        case DISTRO_ALPINE:
            safe_snprintf(cmd, sizeof(cmd), "apk info -e %s 2>/dev/null", pkg_name);
            break;
        default:
            return 0;
    }
    return (system(cmd) == 0);
}

/* ============================================================
 * 执行安装命令（带进度）
 * ============================================================ */
static int exec_install_cmd(const char *cmd, const char *pkg_name) {
    LOG_DEBUG_T("InstallSystem", "Exec", "Cmd", "%s", cmd);

    speed_calc_t speed;
    speed_calc_init(&speed);

    int pipefd[2];
    if (pipe(pipefd) == -1) return -1;

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(1);
    }

    close(pipefd[1]);
    char buffer[4096];
    size_t total_read = 0;
    int status;
    int progress = 0;

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(pipefd[0], &readfds);
        struct timeval tv = {0, 500000};
        int sel_ret = select(pipefd[0] + 1, &readfds, NULL, NULL, &tv);
        if (sel_ret > 0) {
            ssize_t n = read(pipefd[0], buffer, sizeof(buffer) - 1);
            if (n > 0) {
                buffer[n] = '\0';
                total_read += n;
                // 解析进度
                if (strstr(buffer, "Unpacking")) progress = 40;
                else if (strstr(buffer, "Setting up")) progress = 70;
                else if (strstr(buffer, "Processing triggers")) progress = 90;
                else if (progress < 20 && total_read > 1024) progress = 20;
                else if (progress < 30 && total_read > 4096) progress = 30;
            }
        }

        pid_t wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            close(pipefd[0]);
            double speed_val = speed_calc_update(&speed, total_read);
            install_progress_update(100, speed_val, total_read / (1024*1024.0), 0, pkg_name);
            install_progress_finish_item(1);
            return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
        }

        double speed_val = speed_calc_update(&speed, total_read);
        install_progress_update(progress, speed_val, total_read / (1024*1024.0), 0, pkg_name);
    }
}

/* ============================================================
 * 安装系统包
 * ============================================================ */
int install_system_install(const char *pkg_name) {
    if (!pkg_name) {
        safe_strncpy(g_last_error, "Invalid package name", sizeof(g_last_error));
        return -1;
    }

    LOG_INFO_T("InstallSystem", "Install", "Start", "pkg=%s", pkg_name);

    // 检查是否已安装
    if (install_system_is_installed(pkg_name)) {
        LOG_DEBUG_T("InstallSystem", "Install", "AlreadyInstalled", "%s", pkg_name);
        install_progress_finish_item(1);
        return 0;
    }

    const char *real_pkg = get_real_pkg_name(pkg_name);
    if (!real_pkg) {
        safe_snprintf(g_last_error, sizeof(g_last_error), "No package mapping for '%s'", pkg_name);
        LOG_ERROR_T("InstallSystem", "Install", "NoMapping", "%s", g_last_error);
        install_progress_finish_item(0);
        return -1;
    }

    distro_info_t info = distro_detect();
    char cmd[512];
    const char *installer = "apt";

    switch (info.distro_type) {
        case DISTRO_DEBIAN:
            safe_snprintf(cmd, sizeof(cmd), "apt install -y %s", real_pkg);
            installer = "apt";
            break;
        case DISTRO_FEDORA:
            safe_snprintf(cmd, sizeof(cmd), "dnf install -y %s", real_pkg);
            installer = "dnf";
            break;
        case DISTRO_ARCH:
            safe_snprintf(cmd, sizeof(cmd), "pacman -S --noconfirm %s", real_pkg);
            installer = "pacman";
            break;
        case DISTRO_OPENSUSE:
            safe_snprintf(cmd, sizeof(cmd), "zypper install -y %s", real_pkg);
            installer = "zypper";
            break;
        case DISTRO_ALPINE:
            safe_snprintf(cmd, sizeof(cmd), "apk add %s", real_pkg);
            installer = "apk";
            break;
        default:
            safe_snprintf(cmd, sizeof(cmd), "apt install -y %s", real_pkg);
            installer = "apt";
            break;
    }

    install_progress_set_item(pkg_name, 1, 1);

    int ret = exec_install_cmd(cmd, pkg_name);
    if (ret == 0) {
        LOG_INFO_T("InstallSystem", "Install", "OK", "%s installed via %s", pkg_name, installer);
        return 0;
    } else {
        safe_snprintf(g_last_error, sizeof(g_last_error), "Install failed: %s", pkg_name);
        LOG_ERROR_T("InstallSystem", "Install", "Fail", "%s", g_last_error);
        install_progress_finish_item(0);
        return -1;
    }
}

/* ============================================================
 * 检查所有系统依赖
 * ============================================================ */
int install_system_check_all(void) {
    const char *pkgs[] = {"libmosquitto-dev", "libsqlite3-dev", "libmicrohttpd-dev",
                          "libcurl4-openssl-dev", "libseccomp-dev", "libnotcurses-dev", NULL};
    int missing = 0;
    for (int i = 0; pkgs[i]; i++) {
        if (!install_system_is_installed(pkgs[i])) {
            missing++;
            LOG_DEBUG_T("InstallSystem", "CheckAll", "Missing", "%s", pkgs[i]);
        }
    }
    return missing > 0 ? -1 : 0;
}

/* ============================================================
 * 获取错误信息
 * ============================================================ */
const char* install_system_get_last_error(void) {
    return g_last_error;
}

/* ============================================================
 * 设置镜像源
 * ============================================================ */
void install_system_set_mirror(const char *mirror) {
    if (mirror) {
        safe_strncpy(g_system_mirror, mirror, sizeof(g_system_mirror));
    }
}

/* ============================================================
 * 清空缓存
 * ============================================================ */
void install_system_clear_cache(void) {
    // 系统包没有缓存，只清空错误信息
    g_last_error[0] = '\0';
}