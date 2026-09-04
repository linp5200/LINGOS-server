/**
 * @file    src/shell/install_helpers.c
 * @brief   多发行版安装辅助函数实现（扩展版）
 * @version LN-0.4.3
 * @changes 增加多发行版预检查（dpkg/rpm/pacman/apk）；
 *          新增包管理器可用性检测函数；
 *          增加 safe_exec_with_timeout_output 返回完整输出
 */

#include "install_helpers.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include "../common/distro_detect.h"
#include "../lib/log_extra.h"
#include "../drivers/uart.h"
#include "../lib/libling.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

/* ============================================================
 * 安装命令映射表（增加 apk）
 * ============================================================ */
typedef struct {
    const char *tool_name;
    const char *apt_cmd;
    const char *dnf_cmd;
    const char *yum_cmd;
    const char *pacman_cmd;
    const char *zypper_cmd;
    const char *apk_cmd;
} install_entry_t;

static const install_entry_t install_table[] = {
    {"systemd",   "apt install -y systemd",           "dnf install -y systemd",      "yum install -y systemd",      "pacman -S --noconfirm systemd",        "zypper install -y systemd",        "apk add systemd"},
    {"arp-scan",  "apt install -y arp-scan",         "dnf install -y arp-scan",     "yum install -y arp-scan",     "pacman -S --noconfirm arp-scan",       "zypper install -y arp-scan",       "apk add arp-scan"},
    {"nmap",      "apt install -y nmap",             "dnf install -y nmap",         "yum install -y nmap",         "pacman -S --noconfirm nmap",           "zypper install -y nmap",           "apk add nmap"},
    {"mosquitto", "apt install -y mosquitto",        "dnf install -y mosquitto",    "yum install -y mosquitto",    "pacman -S --noconfirm mosquitto",      "zypper install -y mosquitto",      "apk add mosquitto"},
    {"libmosquitto-dev", "apt install -y libmosquitto-dev", "dnf install -y libmosquitto-devel", "yum install -y libmosquitto-devel", "pacman -S --noconfirm libmosquitto", "zypper install -y libmosquitto-devel", "apk add libmosquitto-dev"},
    {"libnotcurses-dev", "apt install -y libnotcurses-dev", "dnf install -y notcurses-devel", "yum install -y notcurses-devel", "pacman -S --noconfirm notcurses", "zypper install -y notcurses-devel", "apk add notcurses-dev"},
    {"libsqlite3-dev", "apt install -y libsqlite3-dev", "dnf install -y sqlite-devel", "yum install -y sqlite-devel", "pacman -S --noconfirm sqlite", "zypper install -y sqlite-devel", "apk add sqlite-dev"},
    {"libmicrohttpd-dev", "apt install -y libmicrohttpd-dev", "dnf install -y libmicrohttpd-devel", "yum install -y libmicrohttpd-devel", "pacman -S --noconfirm libmicrohttpd", "zypper install -y libmicrohttpd-devel", "apk add libmicrohttpd-dev"},
    {"libcurl-dev", "apt install -y libcurl4-openssl-dev", "dnf install -y libcurl-devel", "yum install -y libcurl-devel", "pacman -S --noconfirm curl", "zypper install -y libcurl-devel", "apk add curl-dev"},
    {"libseccomp-dev", "apt install -y libseccomp-dev", "dnf install -y libseccomp-devel", "yum install -y libseccomp-devel", "pacman -S --noconfirm libseccomp", "zypper install -y libseccomp-devel", "apk add libseccomp-dev"},
    {"bleak",     "pip3 install bleak",              "pip3 install bleak",          "pip3 install bleak",          "pip3 install bleak",                   "pip3 install bleak",               "pip3 install bleak"},
    {"sentence-transformers", "pip3 install sentence-transformers", "pip3 install sentence-transformers", "pip3 install sentence-transformers", "pip3 install sentence-transformers", "pip3 install sentence-transformers", "pip3 install sentence-transformers"},
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL}
};

/* ============================================================
 * 获取安装命令（扩展版）
 * ============================================================ */
static const char* get_install_command_internal(distro_type_t type, const char *tool_name) {
    for (int i = 0; install_table[i].tool_name != NULL; i++) {
        if (strcmp(install_table[i].tool_name, tool_name) == 0) {
            switch (type) {
                case DISTRO_DEBIAN:   return install_table[i].apt_cmd;
                case DISTRO_FEDORA:   return install_table[i].dnf_cmd;
                case DISTRO_ARCH:     return install_table[i].pacman_cmd;
                case DISTRO_OPENSUSE: return install_table[i].zypper_cmd;
                case DISTRO_ALPINE:   return install_table[i].apk_cmd;
                default:              return NULL;
            }
        }
    }
    return NULL;
}

/* ============================================================
 * 安全执行命令（fork + execvp，保留原有接口）
 * ============================================================ */
static int safe_exec_sh(const char *cmd_str) {
    pid_t pid = fork();
    if (pid == -1) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd_str, (char*)NULL);
        _exit(1);
    } else {
        int status;
        waitpid(pid, &status, 0);
        return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
    }
}

/* ============================================================
 * 安装包（原有，保持不变）
 * ============================================================ */
int install_package(const char *tool_name, int max_retries) {
    LOG_INFO_T("InstallHelpers", "Install", "Enter", "tool='%s', max_retries=%d", tool_name, max_retries);

    if (!tool_name || !*tool_name) {
        LOG_ERROR_T("InstallHelpers", "Install", "InvalidArg", "tool_name is empty");
        return -1;
    }

    distro_info_t info = distro_detect();
    if (info.distro_type == DISTRO_UNKNOWN) {
        LOG_WARN_T("InstallHelpers", "Install", "UnknownDistro", "distro detection failed, falling back to apt");
        info.distro_type = DISTRO_DEBIAN;
    }

    const char *cmd = get_install_command_internal(info.distro_type, tool_name);
    if (!cmd) {
        cmd = get_install_command_internal(DISTRO_DEBIAN, tool_name);
        if (!cmd) {
            uart_puts(COLOR_YELLOW);
            uart_puts(tr("\n⚠ No predefined install command for: ", "\n⚠ 该工具没有预定义的安装命令："));
            uart_puts(tool_name);
            uart_puts(tr(" on ", " 在 "));
            uart_puts(distro_type_name(info.distro_type));
            uart_puts("\n");
            uart_puts(tr("Please install manually: ", "请手动安装："));
            uart_puts(tool_name);
            uart_puts("\n");
            uart_puts(COLOR_RESET);
            return -1;
        }
        LOG_INFO_T("InstallHelpers", "Install", "Fallback", "using apt fallback for %s", tool_name);
    }

    char label[128];
    safe_snprintf(label, sizeof(label), "%s %s", tr("Installing", "安装中"), tool_name);
    log_draw_progress(0, label, PROGRESS_RUNNING);
    uart_puts("\n");

    int retry = 0;
    while (retry < max_retries) {
        LOG_DEBUG_T("InstallHelpers", "Install", "Attempt", "attempt %d/%d", retry + 1, max_retries);
        int ret = safe_exec_sh(cmd);

        if (ret == 0) {
            log_draw_progress(100, label, PROGRESS_DONE);
            uart_puts("\n");
            uart_puts(COLOR_GREEN);
            uart_puts(tr("✅ ", "✅ "));
            uart_puts(tool_name);
            uart_puts(tr(" installed successfully.\n", " 安装成功。\n"));
            uart_puts(COLOR_RESET);
            return 0;
        }

        retry++;
        if (retry < max_retries) {
            uart_puts(COLOR_YELLOW);
            uart_puts(tr("⚠ Installation failed, retrying ", "⚠ 安装失败，正在重试 "));
            uart_puts(tool_name);
            uart_puts(tr(" (", " ("));
            char buf[8];
            safe_snprintf(buf, sizeof(buf), "%d", retry);
            uart_puts(buf);
            uart_puts(tr("/", "/"));
            safe_snprintf(buf, sizeof(buf), "%d", max_retries);
            uart_puts(buf);
            uart_puts(tr(")...\n", ")...\n"));
            uart_puts(COLOR_RESET);
            sleep(2);
        }
    }

    log_draw_progress(100, label, PROGRESS_FAILED);
    uart_puts("\n");
    uart_puts(COLOR_RED);
    uart_puts(tr("❌ ", "❌ "));
    uart_puts(tool_name);
    uart_puts(tr(" installation failed after ", " 安装失败，已尝试 "));
    char buf[8];
    safe_snprintf(buf, sizeof(buf), "%d", max_retries);
    uart_puts(buf);
    uart_puts(tr(" attempts.\n", " 次。\n"));
    uart_puts(COLOR_RESET);

    return -1;
}

/* ============================================================
 * 安装 Python 模块（保留）
 * ============================================================ */
int install_python_module(const char *module_name) {
    LOG_INFO_T("InstallHelpers", "PythonMod", "Enter", "module='%s'", module_name);

    if (!module_name || !*module_name) {
        LOG_ERROR_T("InstallHelpers", "PythonMod", "Invalid", "module_name is empty");
        return -1;
    }

    char cmd[256];
    safe_snprintf(cmd, sizeof(cmd), "pip3 install %s 2>&1", module_name);

    uart_puts(tr("Installing Python module: ", "正在安装 Python 模块："));
    uart_puts(module_name);
    uart_puts(" ...\n");

    int ret = safe_exec_sh(cmd);
    if (ret == 0) {
        uart_puts(COLOR_GREEN);
        uart_puts(tr("✅ ", "✅ "));
        uart_puts(module_name);
        uart_puts(tr(" installed successfully.\n", " 安装成功。\n"));
        uart_puts(COLOR_RESET);
        return 0;
    } else {
        uart_puts(COLOR_RED);
        uart_puts(tr("❌ ", "❌ "));
        uart_puts(module_name);
        uart_puts(tr(" installation failed.\n", " 安装失败。\n"));
        uart_puts(tr("Try: pip3 install ", "尝试：pip3 install "));
        uart_puts(module_name);
        uart_puts("\n");
        uart_puts(COLOR_RESET);
        return -1;
    }
}

/* ============================================================
 * 预检查和包管理器检测函数（新增）
 * ============================================================ */

/**
 * @brief 检查系统包是否已安装（多发行版支持）
 */
int is_system_package_installed(const char *pkg_name) {
    if (!pkg_name || !*pkg_name) return 0;

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
            LOG_DEBUG_T("InstallHelpers", "CheckSys", "UnknownDistro", "distro unknown, assuming not installed");
            return 0;
    }

    int ret = (system(cmd) == 0);
    LOG_DEBUG_T("InstallHelpers", "CheckSys", "Result", "pkg=%s, installed=%d", pkg_name, ret);
    return ret;
}

/**
 * @brief 检查 Python 模块是否可导入
 */
int is_python_module_installed(const char *module_name) {
    if (!module_name || !*module_name) return 0;
    char cmd[256];
    safe_snprintf(cmd, sizeof(cmd), "python3 -c 'import %s' 2>/dev/null", module_name);
    return (system(cmd) == 0);
}

/**
 * @brief 获取当前发行版对应的包名（用于 env_bootstrap.c）
 */
const char* get_system_package_name(const char *logical_name) {
    if (!logical_name) return NULL;
    distro_info_t info = distro_detect();
    for (int i = 0; install_table[i].tool_name != NULL; i++) {
        if (strcmp(install_table[i].tool_name, logical_name) == 0) {
            switch (info.distro_type) {
                case DISTRO_DEBIAN:   return install_table[i].apt_cmd;
                case DISTRO_FEDORA:   return install_table[i].dnf_cmd;
                case DISTRO_ARCH:     return install_table[i].pacman_cmd;
                case DISTRO_OPENSUSE: return install_table[i].zypper_cmd;
                case DISTRO_ALPINE:   return install_table[i].apk_cmd;
                default:              return NULL;
            }
        }
    }
    return NULL;
}

/* ============================================================
 * 包管理器可用性检测（供 env_bootstrap.c 使用）
 * ============================================================ */
int check_apt_available(void) {
    return (system("command -v apt >/dev/null 2>&1") == 0);
}

int check_dnf_available(void) {
    return (system("command -v dnf >/dev/null 2>&1") == 0);
}

int check_yum_available(void) {
    return (system("command -v yum >/dev/null 2>&1") == 0);
}

int check_pacman_available(void) {
    return (system("command -v pacman >/dev/null 2>&1") == 0);
}

int check_zypper_available(void) {
    return (system("command -v zypper >/dev/null 2>&1") == 0);
}

int check_apk_available(void) {
    return (system("command -v apk >/dev/null 2>&1") == 0);
}

int check_pip_available(void) {
    return (system("command -v pip3 >/dev/null 2>&1") == 0);
}