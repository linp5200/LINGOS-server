/**
 * @file    src/install/install_python.c
 * @brief   Python 包安装实现（pipx → venv → pip）
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 */

#include "install_python.h"
#include "install_speed.h"
#include "install_progress.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
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
static char g_pypi_mirror[256] = "https://pypi.tuna.tsinghua.edu.cn/simple";

/* ============================================================
 * 检测 Python 模块是否可导入
 * ============================================================ */
int install_python_is_installed(const char *module_name) {
    if (!module_name) return 0;
    char cmd[256];
    safe_snprintf(cmd, sizeof(cmd), "python3 -c 'import %s' 2>/dev/null", module_name);
    return (system(cmd) == 0);
}

/* ============================================================
 * 检查 pipx 是否可用
 * ============================================================ */
static int pipx_available(void) {
    return (system("command -v pipx >/dev/null 2>&1") == 0);
}

/* ============================================================
 * 检查 venv 是否可用
 * ============================================================ */
static int venv_available(void) {
    return (system("python3 -c 'import venv' 2>/dev/null") == 0);
}

/* ============================================================
 * 检查 pip 是否可用
 * ============================================================ */
static int pip_available(void) {
    return (system("command -v pip3 >/dev/null 2>&1") == 0);
}

/* ============================================================
 * 执行 pipx 安装
 * ============================================================ */
static int install_with_pipx(const char *module_name) {
    char cmd[512];
    safe_snprintf(cmd, sizeof(cmd), "pipx install %s --index-url %s 2>/dev/null",
                  module_name, g_pypi_mirror);
    LOG_DEBUG_T("InstallPython", "Pipx", "Cmd", "%s", cmd);
    int ret = system(cmd);
    if (ret == 0) return 0;
    // pipx 失败，尝试强制 reinstall
    safe_snprintf(cmd, sizeof(cmd), "pipx install --force %s --index-url %s 2>/dev/null",
                  module_name, g_pypi_mirror);
    return system(cmd);
}

/* ============================================================
 * 执行 venv 安装
 * ============================================================ */
static int install_with_venv(const char *module_name) {
    const char *venv_path = "/LINGOS/venv";
    char cmd[512];

    // 创建 venv（如果不存在）
    if (access(venv_path, F_OK) != 0) {
        safe_snprintf(cmd, sizeof(cmd), "python3 -m venv %s 2>/dev/null", venv_path);
        if (system(cmd) != 0) return -1;
    }

    safe_snprintf(cmd, sizeof(cmd), "%s/bin/pip3 install %s -i %s 2>/dev/null",
                  venv_path, module_name, g_pypi_mirror);
    LOG_DEBUG_T("InstallPython", "Venv", "Cmd", "%s", cmd);
    return system(cmd);
}

/* ============================================================
 * 执行 pip（带 --break-system-packages）
 * ============================================================ */
static int install_with_pip_break(const char *module_name) {
    char cmd[512];
    safe_snprintf(cmd, sizeof(cmd), "pip3 install --break-system-packages %s -i %s 2>/dev/null",
                  module_name, g_pypi_mirror);
    LOG_DEBUG_T("InstallPython", "PipBreak", "Cmd", "%s", cmd);
    return system(cmd);
}

/* ============================================================
 * 执行普通 pip
 * ============================================================ */
static int install_with_pip_normal(const char *module_name) {
    char cmd[512];
    safe_snprintf(cmd, sizeof(cmd), "pip3 install %s -i %s 2>/dev/null",
                  module_name, g_pypi_mirror);
    LOG_DEBUG_T("InstallPython", "PipNormal", "Cmd", "%s", cmd);
    return system(cmd);
}

/* ============================================================
 * 执行 apt 回退
 * ============================================================ */
static int install_with_apt_fallback(const char *module_name) {
    char cmd[512];
    safe_snprintf(cmd, sizeof(cmd), "apt install -y python3-%s 2>/dev/null", module_name);
    LOG_DEBUG_T("InstallPython", "AptFallback", "Cmd", "%s", cmd);
    return system(cmd);
}

/* ============================================================
 * 安装 Python 包（多方法尝试）
 * ============================================================ */
int install_python_install(const char *module_name) {
    if (!module_name) {
        safe_strncpy(g_last_error, "Invalid module name", sizeof(g_last_error));
        return -1;
    }

    LOG_INFO_T("InstallPython", "Install", "Start", "module=%s", module_name);

    // 检查是否已安装
    if (install_python_is_installed(module_name)) {
        LOG_DEBUG_T("InstallPython", "Install", "AlreadyInstalled", "%s", module_name);
        install_progress_finish_item(1);
        return 0;
    }

    install_progress_set_item(module_name, 1, 1);

    int ret = -1;

    // 方法1: pipx（优先）
    if (pipx_available()) {
        ret = install_with_pipx(module_name);
        if (ret == 0) {
            LOG_INFO_T("InstallPython", "Install", "OK", "%s installed via pipx", module_name);
            install_progress_finish_item(1);
            return 0;
        }
        LOG_DEBUG_T("InstallPython", "Install", "PipxFail", "pipx failed, trying next");
    }

    // 方法2: venv
    if (venv_available()) {
        ret = install_with_venv(module_name);
        if (ret == 0) {
            LOG_INFO_T("InstallPython", "Install", "OK", "%s installed via venv", module_name);
            install_progress_finish_item(1);
            return 0;
        }
        LOG_DEBUG_T("InstallPython", "Install", "VenvFail", "venv failed, trying next");
    }

    // 方法3: pip --break-system-packages
    if (pip_available()) {
        ret = install_with_pip_break(module_name);
        if (ret == 0) {
            LOG_INFO_T("InstallPython", "Install", "OK", "%s installed via pip(break)", module_name);
            install_progress_finish_item(1);
            return 0;
        }
        LOG_DEBUG_T("InstallPython", "Install", "PipBreakFail", "pip(break) failed, trying next");
    }

    // 方法4: 普通 pip
    if (pip_available()) {
        ret = install_with_pip_normal(module_name);
        if (ret == 0) {
            LOG_INFO_T("InstallPython", "Install", "OK", "%s installed via pip", module_name);
            install_progress_finish_item(1);
            return 0;
        }
        LOG_DEBUG_T("InstallPython", "Install", "PipNormalFail", "pip failed, trying apt fallback");
    }

    // 方法5: apt 回退
    ret = install_with_apt_fallback(module_name);
    if (ret == 0) {
        LOG_INFO_T("InstallPython", "Install", "OK", "%s installed via apt", module_name);
        install_progress_finish_item(1);
        return 0;
    }

    // 全部失败
    safe_snprintf(g_last_error, sizeof(g_last_error), "All methods failed for %s", module_name);
    LOG_ERROR_T("InstallPython", "Install", "AllFail", "%s", g_last_error);
    install_progress_finish_item(0);
    return -1;
}

/* ============================================================
 * 检查所有 Python 依赖
 * ============================================================ */
int install_python_check_all(void) {
    const char *modules[] = {"requests", "numpy", "tiktoken", "vosk", NULL};
    int missing = 0;
    for (int i = 0; modules[i]; i++) {
        if (!install_python_is_installed(modules[i])) {
            missing++;
            LOG_DEBUG_T("InstallPython", "CheckAll", "Missing", "%s", modules[i]);
        }
    }
    return missing > 0 ? -1 : 0;
}

/* ============================================================
 * 获取错误信息
 * ============================================================ */
const char* install_python_get_last_error(void) {
    return g_last_error;
}

/* ============================================================
 * 设置 PyPI 镜像源
 * ============================================================ */
void install_python_set_mirror(const char *mirror) {
    if (mirror && *mirror) {
        safe_strncpy(g_pypi_mirror, mirror, sizeof(g_pypi_mirror));
    }
}

/* ============================================================
 * 清空缓存
 * ============================================================ */
void install_python_clear_cache(void) {
    g_last_error[0] = '\0';
}