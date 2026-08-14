/**
 * @file    src/install/install_manager.c
 * @brief   安装管理器实现
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C-C
 */

#include "install_manager.h"
#include "install_system.h"
#include "install_python.h"
#include "install_model.h"
#include "install_progress.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../drivers/uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static install_summary_t g_last_summary;
static int g_offline_mode = 0;
static char g_apt_mirror[256] = "";
static char g_pypi_mirror[256] = "";

/* ============================================================
 * 内部辅助
 * ============================================================ */
static void init_summary(install_summary_t *summary) {
    if (!summary) return;
    memset(summary, 0, sizeof(install_summary_t));
    summary->log_file[0] = '\0';
    summary->results[0].name[0] = '\0';
}

static void record_result(install_summary_t *summary, const char *name,
                          int success, const char *error_msg) {
    if (!summary || !name) return;
    if (summary->result_count >= 64) return;

    install_result_t *r = &summary->results[summary->result_count];
    safe_strncpy(r->name, name, sizeof(r->name));
    r->success = success;
    if (!success && error_msg) {
        safe_strncpy(r->error_msg, error_msg, sizeof(r->error_msg));
    } else {
        r->error_msg[0] = '\0';
    }
    r->timestamp = time(NULL);
    summary->result_count++;
    summary->total++;
    if (success) summary->success_count++;
    else summary->failed_count++;
}

/* ============================================================
 * 运行所有安装
 * ============================================================ */
int install_manager_run_all(install_summary_t *summary) {
    LOG_INFO_T("InstallManager", "RunAll", "Start", "starting full installation");

    install_summary_t local_summary;
    if (!summary) {
        summary = &local_summary;
    }
    init_summary(summary);

    install_progress_init(tr("Installing dependencies...", "正在安装依赖..."));

    // 1. 系统包
    install_progress_set_stage(tr("System packages", "系统包"), 0, 3);
    int sys_ret = install_manager_run_system(summary);

    // 2. Python 包
    install_progress_set_stage(tr("Python packages", "Python 包"), 1, 3);
    int py_ret = install_manager_run_python(summary);

    // 3. 模型下载
    install_progress_set_stage(tr("Models", "模型"), 2, 3);
    int model_ret = install_manager_run_models(summary);

    install_progress_finish(summary);

    g_last_summary = *summary;
    LOG_INFO_T("InstallManager", "RunAll", "Complete", "total=%d, success=%d, failed=%d",
               summary->total, summary->success_count, summary->failed_count);

    return (summary->failed_count > 0) ? -1 : 0;
}

/* ============================================================
 * 运行系统依赖
 * ============================================================ */
int install_manager_run_system(install_summary_t *summary) {
    LOG_INFO_T("InstallManager", "RunSystem", "Start", "installing system packages");

    // 系统包列表
    const char *pkgs[] = {
        "libmosquitto-dev",
        "libsqlite3-dev",
        "libmicrohttpd-dev",
        "libcurl4-openssl-dev",
        "libseccomp-dev",
        "libnotcurses-dev",
        NULL
    };

    int total_failures = 0;
    for (int i = 0; pkgs[i]; i++) {
        char msg[256];
        safe_snprintf(msg, sizeof(msg), "%s (%d/%d)",
                      pkgs[i], i + 1, (int)(sizeof(pkgs)/sizeof(pkgs[0]) - 1));
        install_progress_update(0, 0, 0, 0, msg);

        int ret = install_system_install(pkgs[i]);
        if (ret == 0) {
            record_result(summary, pkgs[i], 1, NULL);
        } else {
            record_result(summary, pkgs[i], 0, install_system_get_last_error());
            total_failures++;
        }
    }

    LOG_INFO_T("InstallManager", "RunSystem", "Complete", "failures=%d", total_failures);
    return (total_failures > 0) ? -1 : 0;
}

/* ============================================================
 * 运行 Python 依赖
 * ============================================================ */
int install_manager_run_python(install_summary_t *summary) {
    LOG_INFO_T("InstallManager", "RunPython", "Start", "installing Python packages");

    const char *modules[] = {
        "vosk",
        "paho-mqtt",
        "sentence_transformers",
        "tiktoken",
        "requests",
        "numpy",
        "Pillow",
        "ultralytics",
        "flask",
        "flask_socketio",
        "flask_cors",
        NULL
    };

    int total_failures = 0;
    for (int i = 0; modules[i]; i++) {
        char msg[256];
        safe_snprintf(msg, sizeof(msg), "%s (%d/%d)",
                      modules[i], i + 1, (int)(sizeof(modules)/sizeof(modules[0]) - 1));
        install_progress_update(0, 0, 0, 0, msg);

        int ret = install_python_install(modules[i]);
        if (ret == 0) {
            record_result(summary, modules[i], 1, NULL);
        } else {
            record_result(summary, modules[i], 0, install_python_get_last_error());
            total_failures++;
        }
    }

    LOG_INFO_T("InstallManager", "RunPython", "Complete", "failures=%d", total_failures);
    return (total_failures > 0) ? -1 : 0;
}

/* ============================================================
 * 运行模型下载
 * ============================================================ */
int install_manager_run_models(install_summary_t *summary) {
    LOG_INFO_T("InstallManager", "RunModels", "Start", "downloading models");

    const char *models[] = {
        "yolov8n",
        "vosk-model-small-cn-0.22",
        NULL
    };

    int total_failures = 0;
    for (int i = 0; models[i]; i++) {
        char msg[256];
        safe_snprintf(msg, sizeof(msg), "%s (%d/%d)",
                      models[i], i + 1, (int)(sizeof(models)/sizeof(models[0]) - 1));
        install_progress_update(0, 0, 0, 0, msg);

        int ret = install_model_download(models[i]);
        if (ret == 0) {
            record_result(summary, models[i], 1, NULL);
        } else {
            record_result(summary, models[i], 0, install_model_get_last_error());
            total_failures++;
        }
    }

    LOG_INFO_T("InstallManager", "RunModels", "Complete", "failures=%d", total_failures);
    return (total_failures > 0) ? -1 : 0;
}

/* ============================================================
 * 检查所有依赖
 * ============================================================ */
int install_manager_check_all(void) {
    LOG_DEBUG_T("InstallManager", "CheckAll", "Enter", "checking all dependencies");

    int missing = 0;
    if (install_system_check_all() != 0) missing++;
    if (install_python_check_all() != 0) missing++;
    if (install_model_check_all() != 0) missing++;

    LOG_DEBUG_T("InstallManager", "CheckAll", "Result", "missing=%d", missing);
    return (missing == 0) ? 1 : 0;
}

/* ============================================================
 * 获取上次汇总
 * ============================================================ */
const install_summary_t* install_manager_get_last_summary(void) {
    return &g_last_summary;
}

/* ============================================================
 * 清空缓存
 * ============================================================ */
void install_manager_clear_cache(void) {
    install_system_clear_cache();
    install_python_clear_cache();
    install_model_clear_cache();
    LOG_INFO_T("InstallManager", "ClearCache", "OK", "all install caches cleared");
}

/* ============================================================
 * 离线模式管理
 * ============================================================ */
void install_manager_set_offline(int offline) {
    g_offline_mode = offline ? 1 : 0;
    LOG_INFO_T("InstallManager", "SetOffline", "OK", "offline mode = %d", g_offline_mode);
}

int install_manager_is_offline(void) {
    return g_offline_mode;
}

/* ============================================================
 * 设置镜像源
 * ============================================================ */
void install_manager_set_mirrors(const char *apt_mirror, const char *pypi_mirror) {
    if (apt_mirror) {
        safe_strncpy(g_apt_mirror, apt_mirror, sizeof(g_apt_mirror));
        install_system_set_mirror(apt_mirror);
    }
    if (pypi_mirror) {
        safe_strncpy(g_pypi_mirror, pypi_mirror, sizeof(g_pypi_mirror));
        install_python_set_mirror(pypi_mirror);
    }
    LOG_INFO_T("InstallManager", "SetMirrors", "OK", "mirrors set");
}