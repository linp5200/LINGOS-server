/**
 * @file    env_detect.c
 * @brief   环境检测与依赖安装（优化版）
 * @version LN-B-4.3.0.0
 * @changes 增加缓存检查，避免重复安装；统一使用 install_helpers
 */

#include "env_detect.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/init_cache.h"
#include "../lib/log_extra.h"
#include "../shell/install_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dlfcn.h>

/* ============================================================
 * 运行命令检查
 * ============================================================ */
static int run_cmd(const char *cmd) {
    if (!cmd) return -1;
    return system(cmd);
}

/* ============================================================
 * 原有检测函数（保持不变）
 * ============================================================ */

int ensure_python_environment(void) {
    LOG_DEBUG_T("EnvDetect", "PythonEnv", "Enter", "checking Python environment");

    int ret = 0;
    if (run_cmd("python3 --version > /dev/null 2>&1") != 0) {
        LOG_WARN_T("EnvDetect", "PythonEnv", "Missing", "python3 not found");
        ret = -1;
    }

    if (run_cmd("python3 -c 'import flask' > /dev/null 2>&1") != 0) {
        LOG_WARN_T("EnvDetect", "PythonEnv", "MissingFlask", "flask module not found");
        ret = -1;
    }

    if (run_cmd("python3 -c 'import requests' > /dev/null 2>&1") != 0) {
        LOG_WARN_T("EnvDetect", "PythonEnv", "MissingRequests", "requests module not found");
        ret = -1;
    }

    return ret;
}

int ensure_libcurl(void) {
    LOG_DEBUG_T("EnvDetect", "LibCurl", "Enter", "checking libcurl");
    void *handle = dlopen("libcurl.so", RTLD_NOW);
    if (!handle) {
        LOG_WARN_T("EnvDetect", "LibCurl", "Missing", "libcurl not found");
        return -1;
    }
    dlclose(handle);
    LOG_DEBUG_T("EnvDetect", "LibCurl", "OK", "libcurl found");
    return 0;
}

int ensure_microhttpd(void) {
    LOG_DEBUG_T("EnvDetect", "Microhttpd", "Enter", "checking libmicrohttpd");
    void *handle = dlopen("libmicrohttpd.so", RTLD_NOW);
    if (!handle) {
        LOG_WARN_T("EnvDetect", "Microhttpd", "Missing", "libmicrohttpd not found");
        return -1;
    }
    dlclose(handle);
    LOG_DEBUG_T("EnvDetect", "Microhttpd", "OK", "libmicrohttpd found");
    return 0;
}

int ensure_utf8_locale(void) {
    LOG_DEBUG_T("EnvDetect", "UTF8Locale", "Enter", "checking UTF-8 locale");
    const char *lang = getenv("LANG");
    if (lang && strstr(lang, "UTF-8")) {
        LOG_DEBUG_T("EnvDetect", "UTF8Locale", "OK", "UTF-8 locale: %s", lang);
        return 0;
    }
    LOG_WARN_T("EnvDetect", "UTF8Locale", "Missing", "UTF-8 locale not set");
    return -1;
}

/* ============================================================
 * Python 模块检测
 * ============================================================ */
int check_python_module_installed(const char *module_name) {
    if (!module_name) return 0;
    char cmd[512];
    safe_snprintf(cmd, sizeof(cmd), "python3 -c 'import %s' > /dev/null 2>&1", module_name);
    return (system(cmd) == 0) ? 1 : 0;
}

/* ============================================================
 * 系统依赖检测（使用 install_helpers）
 * ============================================================ */
int check_system_library(const char *lib_name) {
    if (!lib_name) return 0;
    void *handle = dlopen(lib_name, RTLD_NOW);
    if (handle) {
        dlclose(handle);
        return 1;
    }
    char path[512];
    const char *dirs[] = {"/usr/lib", "/usr/lib64", "/usr/local/lib", "/lib", NULL};
    for (int i = 0; dirs[i]; i++) {
        safe_snprintf(path, sizeof(path), "%s/%s", dirs[i], lib_name);
        if (access(path, F_OK) == 0) return 1;
    }
    return 0;
}

int ensure_system_dependencies(void) {
    LOG_INFO_T("EnvDetect", "SystemDeps", "Enter", "checking system dependencies");

    /* 检查缓存 */
    init_cache_t cache;
    if (init_cache_load(&cache) == 0 && init_cache_is_valid(&cache)) {
        if (cache.notcurses_ok && cache.sqlite3_ok && cache.mosquitto_ok) {
            LOG_DEBUG_T("EnvDetect", "SystemDeps", "CacheHit", "system deps already satisfied");
            return 0;
        }
    }

    int ret = 0;
    const char *libs[] = {"libmosquitto.so", "libnotcurses.so", "libsqlite3.so", "libmicrohttpd.so"};
    const char *pkg_names[] = {"libmosquitto-dev", "libnotcurses-dev", "libsqlite3-dev", "libmicrohttpd-dev"};

    for (int i = 0; i < 4; i++) {
        if (!check_system_library(libs[i])) {
            LOG_WARN_T("EnvDetect", "SystemDeps", "Missing", "%s", libs[i]);
            if (install_package(pkg_names[i], 2) != 0) {
                LOG_WARN_T("EnvDetect", "SystemDeps", "InstallFail", "failed to install %s", pkg_names[i]);
                ret = -1;
            } else {
                LOG_INFO_T("EnvDetect", "SystemDeps", "Installed", "%s", pkg_names[i]);
            }
        }
    }

    if (system("command -v mosquitto_pub > /dev/null 2>&1") != 0) {
        LOG_WARN_T("EnvDetect", "SystemDeps", "MissingMosquitto", "mosquitto client not found");
        install_package("mosquitto", 2);
    }

    /* 更新缓存 */
    init_cache_t new_cache;
    if (init_cache_load(&new_cache) == 0) {
        new_cache.notcurses_ok = check_system_library("libnotcurses.so");
        new_cache.sqlite3_ok = check_system_library("libsqlite3.so");
        new_cache.mosquitto_ok = check_system_library("libmosquitto.so");
        new_cache.timestamp = time(NULL);
        init_cache_save(&new_cache);
    }

    return ret;
}

int ensure_python_dependencies(void) {
    LOG_INFO_T("EnvDetect", "PythonDeps", "Enter", "checking Python dependencies");

    init_cache_t cache;
    if (init_cache_load(&cache) == 0 && init_cache_is_valid(&cache)) {
        if (cache.python_ok) {
            LOG_DEBUG_T("EnvDetect", "PythonDeps", "CacheHit", "Python deps already satisfied");
            return 0;
        }
    }

    int ret = 0;
    const char *modules[] = {"requests", "flask", "sentence_transformers", "tiktoken", "numpy"};
    const char *pip_names[] = {"requests", "flask", "sentence-transformers", "tiktoken", "numpy"};

    for (int i = 0; i < 5; i++) {
        if (!check_python_module_installed(modules[i])) {
            LOG_WARN_T("EnvDetect", "PythonDeps", "Missing", "python module: %s", modules[i]);
            if (install_python_module(pip_names[i]) != 0) {
                LOG_WARN_T("EnvDetect", "PythonDeps", "InstallFail", "failed to install %s", pip_names[i]);
                ret = -1;
            } else {
                LOG_INFO_T("EnvDetect", "PythonDeps", "Installed", "%s", pip_names[i]);
            }
        }
    }

    if (ret == 0) {
        init_cache_t new_cache;
        if (init_cache_load(&new_cache) == 0) {
            new_cache.python_ok = 1;
            new_cache.timestamp = time(NULL);
            init_cache_save(&new_cache);
        }
    }

    return ret;
}

int ensure_all_dependencies(void) {
    LOG_INFO_T("EnvDetect", "AllDeps", "Enter", "ensuring all dependencies");

    init_cache_t cache;
    if (init_cache_load(&cache) == 0 && init_cache_is_valid(&cache)) {
        if (cache.python_ok && cache.libcurl_ok && cache.microhttpd_ok &&
            cache.notcurses_ok && cache.sqlite3_ok && cache.mosquitto_ok) {
            LOG_DEBUG_T("EnvDetect", "AllDeps", "CacheHit", "all dependencies satisfied from cache");
            return 0;
        }
    }

    int ret = 0;
    if (ensure_system_dependencies() != 0) {
        LOG_WARN_T("EnvDetect", "AllDeps", "SystemFail", "system dependencies have issues");
        ret = -1;
    }

    if (ensure_python_dependencies() != 0) {
        LOG_WARN_T("EnvDetect", "AllDeps", "PythonFail", "Python dependencies have issues");
        ret = -1;
    }

    if (ensure_python_environment() != 0) {
        LOG_WARN_T("EnvDetect", "AllDeps", "PythonEnvFail", "Python environment has issues");
        ret = -1;
    }

    if (ensure_libcurl() != 0) {
        LOG_WARN_T("EnvDetect", "AllDeps", "LibCurlFail", "libcurl not available");
        ret = -1;
    }

    if (ensure_utf8_locale() != 0) {
        LOG_WARN_T("EnvDetect", "AllDeps", "LocaleFail", "UTF-8 locale not set");
        ret = -1;
    }

    /* 保存缓存（即使部分失败，也保存已成功的项） */
    init_cache_t new_cache;
    if (init_cache_load(&new_cache) == 0) {
        new_cache.python_ok = (ret == 0);
        new_cache.libcurl_ok = (ensure_libcurl() == 0);
        new_cache.microhttpd_ok = (ensure_microhttpd() == 0);
        new_cache.notcurses_ok = check_system_library("libnotcurses.so");
        new_cache.sqlite3_ok = check_system_library("libsqlite3.so");
        new_cache.mosquitto_ok = check_system_library("libmosquitto.so");
        new_cache.timestamp = time(NULL);
        init_cache_save(&new_cache);
    }

    if (ret == 0) {
        LOG_INFO_T("EnvDetect", "AllDeps", "OK", "all dependencies satisfied");
    } else {
        LOG_WARN_T("EnvDetect", "AllDeps", "Partial", "some dependencies are missing");
    }

    return ret;
}