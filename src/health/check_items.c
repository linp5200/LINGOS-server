/**
 * @file    src/health/check_items.c
 * @brief   自检检查项实现（适配 check_manager 接口模型）
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, CM
 * @changes 重写以对齐 check_manager.h/c 接口：
 *          - 检查函数改为无参，返回 check_result_t（0/1/2/4）
 *          - 结果消息通过 check_cache_set() 存储
 *          - 注册使用 check_item_t 结构体数组
 *          - config 检查项：若任一配置文件存在则直接通过（防覆盖触发向导）
 */

#include "check_items.h"
#include "check_cache.h"
#include "../config/config_core.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include "../common/lang.h"
#include "../health/repair/active_repair.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <errno.h>

/* ============================================================
 * 检查项：语言配置
 * ============================================================ */
int check_item_language(void) {
    LOG_DEBUG_T("CheckItems", "Language", "Enter", "checking language configuration");

    const wizard_config_t *cfg = config_core_get();
    if (!cfg || cfg->language[0] == '\0') {
        check_cache_set("language",
                        tr("Language not configured, using default 'en'", "语言未配置，使用默认 'en'"),
                        CHECK_RESULT_WARN);
        LOG_WARN_T("CheckItems", "Language", "NotConfigured", "language not set");
        return CHECK_RESULT_WARN;
    }

    if (strcmp(cfg->language, "en") != 0 && strcmp(cfg->language, "zh") != 0) {
        check_cache_set("language",
                        tr("Invalid language, using default", "无效语言，使用默认"),
                        CHECK_RESULT_WARN);
        LOG_WARN_T("CheckItems", "Language", "Invalid", "invalid language: %s", cfg->language);
        return CHECK_RESULT_WARN;
    }

    check_cache_set("language", tr("Language OK", "语言正常"), CHECK_RESULT_PASS);
    LOG_DEBUG_T("CheckItems", "Language", "OK", "language=%s", cfg->language);
    return CHECK_RESULT_PASS;
}

/* ============================================================
 * 修复触发（A2 裁决：不新建 fix_func 链——失败走 active_repair 现有引擎）
 * ============================================================ */
static void deps_trigger_repair(const char *msg) {
    repair_result_t result;
    memset(&result, 0, sizeof(result));
    int ret = active_repair_trigger(msg, "self_check_dependencies", &result);
    if (ret == 0 && result.success) {
        LOG_INFO_T("CheckItems", "Dependencies", "RepairOK", "repair succeeded: %s", result.action_used);
    } else {
        LOG_WARN_T("CheckItems", "Dependencies", "RepairFail",
                   "repair failed (ret=%d): %s", ret,
                   result.error_msg[0] ? result.error_msg : "engine unavailable");
    }
}

/* ============================================================
 * 检查项：依赖
 * 【0.2.1 全捆优化】捆绑形态（包内 lib/+python/ 存在）：
 *   查 lib/manifest.json 齐全性 + dlopen 试加载 + venv import 验证
 *   传统形态（依赖宿主）：保留原逻辑兜底
 * ============================================================ */
static int check_bundled_form(void) {
    /* 捆绑形态判定：二进制同目录 ../lib/manifest.json 存在（/proc/self/exe 定位——任意解压路径可靠） */
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return 0;
    exe[n] = '\0';
    /* 取 dirname */
    char *slash = strrchr(exe, '/');
    if (!slash) return 0;
    *slash = '\0';
    char manifest[512];
    safe_snprintf(manifest, sizeof(manifest), "%s/../lib/manifest.json", exe);
    if (access(manifest, F_OK) != 0) {
        /* 也检查 /LINGOS/lib（解压到数据根形态） */
        safe_snprintf(manifest, sizeof(manifest), "/LINGOS/lib/manifest.json");
        if (access(manifest, F_OK) != 0) {
            return 0;  /* 非捆绑形态 */
        }
    }
    return 1;
}

static int check_bundled_libs(void) {
    /* 捆绑形态：解析 manifest.json 逐个检查 .so 存在 + dlopen 试加载
       依赖 RPATH（$ORIGIN/../lib）或 LD_LIBRARY_PATH 指向包内 lib/ */
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return CHECK_RESULT_FAIL;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) return CHECK_RESULT_FAIL;
    *slash = '\0';
    char manifest[512];
    safe_snprintf(manifest, sizeof(manifest), "%s/../lib/manifest.json", exe);
    if (access(manifest, F_OK) != 0) {
        safe_snprintf(manifest, sizeof(manifest), "/LINGOS/lib/manifest.json");
    }
    FILE *fp = fopen(manifest, "r");
    if (!fp) {
        check_cache_set("dependencies",
                        tr("Bundle manifest not found", "捆绑清单未找到"),
                        CHECK_RESULT_FAIL);
        deps_trigger_repair("bundle manifest not found");
        return CHECK_RESULT_FAIL;
    }
    /* 解析 {"libs": [{"name": "libcurl.so.4", ...}]} */
    char buf[8192];
    size_t rd = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[rd] = '\0';
    if (strstr(buf, "\"libs\"") == NULL) {
        check_cache_set("dependencies",
                        tr("Bundle manifest invalid", "捆绑清单格式无效"),
                        CHECK_RESULT_FAIL);
        deps_trigger_repair("bundle manifest invalid");
        return CHECK_RESULT_FAIL;
    }
    int missing = 0;
    const char *p = buf;
    while ((p = strstr(p, "\"name\"")) != NULL) {
        const char *q = strchr(p + 7, '"');
        if (!q) break;
        const char *r = strchr(q + 1, '"');
        if (!r) break;
        size_t len = (size_t)(r - q - 1);
        if (len == 0 || len > 255) { p = r + 1; continue; }
        char libname[256];
        memcpy(libname, q + 1, len);
        libname[len] = '\0';
        /* dlopen 试加载（RTLD_NOLOAD 语义——只看能否解析，不执行） */
        void *h = dlopen(libname, RTLD_LAZY | RTLD_LOCAL);
        if (!h) {
            /* 尝试从 lib/ 目录显式加载（RPATH 未命中时的兜底） */
            char libpath[512];
            safe_snprintf(libpath, sizeof(libpath), "%s/../lib/%s", exe, libname);
            if (access(libpath, F_OK) != 0) {
                safe_snprintf(libpath, sizeof(libpath), "/LINGOS/lib/%s", libname);
            }
            void *h2 = dlopen(libpath, RTLD_LAZY | RTLD_LOCAL);
            if (!h2) {
                LOG_WARN_T("CheckItems", "Dependencies", "LibMissing", "%s load failed: %s", libname, dlerror());
                missing++;
            } else {
                dlclose(h2);
            }
        } else {
            dlclose(h);
        }
        p = r + 1;
    }
    if (missing > 0) {
        check_cache_set("dependencies",
                        tr("Bundled libraries incomplete", "捆绑库缺失"),
                        CHECK_RESULT_FAIL);
        deps_trigger_repair("bundled libraries incomplete");
        return CHECK_RESULT_FAIL;
    }
    return CHECK_RESULT_PASS;
}

static int check_bundled_python(void) {
    /* 捆绑形态：venv 解释器可执行 + requests 可导入 */
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return CHECK_RESULT_FAIL;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) return CHECK_RESULT_FAIL;
    *slash = '\0';
    char pybin[512];
    safe_snprintf(pybin, sizeof(pybin), "%s/../python/bin/python3", exe);
    if (access(pybin, X_OK) != 0) {
        safe_snprintf(pybin, sizeof(pybin), "/LINGOS/python/bin/python3");
        if (access(pybin, X_OK) != 0) {
            check_cache_set("dependencies",
                            tr("Bundled venv missing", "捆绑 venv 缺失"),
                            CHECK_RESULT_FAIL);
            deps_trigger_repair("bundled venv missing");
            return CHECK_RESULT_FAIL;
        }
    }
    char cmd[640];
    safe_snprintf(cmd, sizeof(cmd), "%s -c 'import requests' >/dev/null 2>&1", pybin);
    if (system(cmd) != 0) {
        check_cache_set("dependencies",
                        tr("Bundled venv broken (requests missing)", "捆绑 venv 异常（缺 requests）"),
                        CHECK_RESULT_FAIL);
        deps_trigger_repair("bundled venv broken");
        return CHECK_RESULT_FAIL;
    }
    return CHECK_RESULT_PASS;
}

int check_item_dependencies(void) {
    LOG_DEBUG_T("CheckItems", "Dependencies", "Enter", "checking dependencies");

    /* 【0.2.1】捆绑形态优先（全捆包自包含——不依赖宿主安装） */
    if (check_bundled_form()) {
        int lib_ok = check_bundled_libs();
        if (lib_ok != CHECK_RESULT_PASS) return lib_ok;
        int py_ok = check_bundled_python();
        if (py_ok != CHECK_RESULT_PASS) return py_ok;
        check_cache_set("dependencies",
                        tr("Bundled dependencies OK", "捆绑依赖正常"),
                        CHECK_RESULT_PASS);
        return CHECK_RESULT_PASS;
    }

    /* 传统形态：宿主 python3（保留原逻辑） */
    if (system("command -v python3 >/dev/null 2>&1") != 0) {
        check_cache_set("dependencies",
                        tr("Python3 not found, AI features may be unavailable",
                           "未找到 Python3，AI 功能可能不可用"),
                        CHECK_RESULT_FAIL);
        LOG_WARN_T("CheckItems", "Dependencies", "PythonMissing", "python3 not found");
        deps_trigger_repair("python3 not found");
        return CHECK_RESULT_FAIL;
    }

    check_cache_set("dependencies", tr("All dependencies OK", "所有依赖正常"), CHECK_RESULT_PASS);
    LOG_DEBUG_T("CheckItems", "Dependencies", "OK", "dependencies OK");
    return CHECK_RESULT_PASS;
}

/* ============================================================
 * 检查项：硬件
 * ============================================================ */
int check_item_hardware(void) {
    LOG_DEBUG_T("CheckItems", "Hardware", "Enter", "checking hardware");

    if (!isatty(STDIN_FILENO) && !isatty(STDOUT_FILENO)) {
        check_cache_set("hardware",
                        tr("No terminal detected, some features may not work",
                           "未检测到终端，部分功能可能不可用"),
                        CHECK_RESULT_WARN);
        LOG_WARN_T("CheckItems", "Hardware", "NoTTY", "no terminal detected");
        return CHECK_RESULT_WARN;
    }

    check_cache_set("hardware", tr("Hardware OK", "硬件正常"), CHECK_RESULT_PASS);
    LOG_DEBUG_T("CheckItems", "Hardware", "OK", "hardware OK");
    return CHECK_RESULT_PASS;
}

/* ============================================================
 * 检查项：配置完整性（核心修复 + 防覆盖防护）
 * ============================================================ */
int check_item_config(void) {
    LOG_DEBUG_T("CheckItems", "Config", "Enter", "checking configuration completeness");

    /* ---- 防护：检查任何配置文件是否存在 ---- */
    const char *root = lingos_data_root();
    const char *config_files[] = {
        "ai_config.json", "security.json", "privilege.json",
        "startup.conf", "defense.conf", "health.conf",
        "watchdog.conf", "sandbox.conf", "network.conf",
        "user_profile.json"
    };

    int any_exists = 0;
    for (size_t i = 0; i < sizeof(config_files)/sizeof(config_files[0]); i++) {
        char path[512];
        safe_snprintf(path, sizeof(path), "%s/system/config/%s", root, config_files[i]);
        if (access(path, F_OK) == 0) {
            any_exists = 1;
            LOG_DEBUG_T("CheckItems", "Config", "Exists", "%s exists", config_files[i]);
            break;
        }
    }

    if (any_exists) {
        /* 有配置文件存在 → 尝试加载，但不触发向导 */
        wizard_config_t tmp_cfg;
        if (config_core_load(&tmp_cfg) == 0) {
            check_cache_set("config",
                            tr("Configuration files exist", "配置文件存在"),
                            CHECK_RESULT_PASS);
            LOG_INFO_T("CheckItems", "Config", "FilesExist", "config files exist, skipping wizard");
            return CHECK_RESULT_PASS;
        }
        /* 文件存在但加载失败 → 记录警告但不触发向导 */
        check_cache_set("config",
                        tr("Config files exist but load failed, manual inspection needed",
                           "配置文件存在但加载失败，请手动检查"),
                        CHECK_RESULT_WARN);
        LOG_WARN_T("CheckItems", "Config", "LoadFail", "config exists but load failed");
        return CHECK_RESULT_WARN;
    }

    /* ---- 文件不存在 → 正常检查（可能触发向导） ---- */
    const wizard_config_t *cfg = config_core_get();
    if (!cfg) {
        check_cache_set("config",
                        tr("Configuration not loaded", "配置未加载"),
                        CHECK_RESULT_FAIL);
        LOG_WARN_T("CheckItems", "Config", "NotLoaded", "config_core not loaded");
        return CHECK_RESULT_FAIL;
    }

    int missing = 0;
    if (cfg->language[0] == '\0') missing = 1;
    if (cfg->ai_backend[0] == '\0') missing = 1;
    if (cfg->configured_at == 0) missing = 1;

    if (missing) {
        check_cache_set("config",
                        tr("Configuration incomplete, wizard required",
                           "配置不完整，需要运行配置向导"),
                        CHECK_RESULT_FAIL);
        LOG_WARN_T("CheckItems", "Config", "Incomplete", "configuration incomplete");
        return CHECK_RESULT_FAIL;
    }

    check_cache_set("config", tr("Configuration complete", "配置完整"), CHECK_RESULT_PASS);
    LOG_DEBUG_T("CheckItems", "Config", "OK", "configuration complete");
    return CHECK_RESULT_PASS;
}

/* ============================================================
 * 检查项：网络
 * ============================================================ */
int check_item_network(void) {
    LOG_DEBUG_T("CheckItems", "Network", "Enter", "checking network");

    if (system("ping -c 1 -W 1 8.8.8.8 >/dev/null 2>&1") != 0 &&
        system("ping -c 1 -W 1 114.114.114.114 >/dev/null 2>&1") != 0) {
        check_cache_set("network",
                        tr("Network unreachable, AI features may be limited",
                           "网络不可达，AI 功能可能受限"),
                        CHECK_RESULT_WARN);
        LOG_WARN_T("CheckItems", "Network", "Unreachable", "network unreachable");
        return CHECK_RESULT_WARN;
    }

    check_cache_set("network", tr("Network OK", "网络正常"), CHECK_RESULT_PASS);
    LOG_DEBUG_T("CheckItems", "Network", "OK", "network OK");
    return CHECK_RESULT_PASS;
}

/* ============================================================
 * 检查项：权限
 * ============================================================ */
int check_item_permissions(void) {
    LOG_DEBUG_T("CheckItems", "Permissions", "Enter", "checking permissions");

    const char *root = lingos_data_root();
    if (access(root, W_OK) != 0) {
        check_cache_set("permissions",
                        tr("No write permission to data root", "无数据根目录写入权限"),
                        CHECK_RESULT_FAIL);
        LOG_WARN_T("CheckItems", "Permissions", "NoWrite", "no write permission to %s", root);
        return CHECK_RESULT_FAIL;
    }

    check_cache_set("permissions", tr("Permissions OK", "权限正常"), CHECK_RESULT_PASS);
    LOG_DEBUG_T("CheckItems", "Permissions", "OK", "permissions OK");
    return CHECK_RESULT_PASS;
}

/* ============================================================
 * 检查项：版本
 * ============================================================ */
int check_item_version(void) {
    LOG_DEBUG_T("CheckItems", "Version", "Enter", "checking version");

    extern const char* version_get(void);
    const char *ver = version_get();
    if (!ver || ver[0] == '\0') {
        check_cache_set("version",
                        tr("Version not found", "版本未找到"),
                        CHECK_RESULT_WARN);
        LOG_WARN_T("CheckItems", "Version", "NotFound", "version not found");
        return CHECK_RESULT_WARN;
    }

    check_cache_set("version", tr("Version OK", "版本正常"), CHECK_RESULT_PASS);
    LOG_DEBUG_T("CheckItems", "Version", "OK", "version=%s", ver);
    return CHECK_RESULT_PASS;
}

/* ============================================================
 * 内置检查项注册表（对齐 check_manager 的 check_item_t 模型）
 * ============================================================ */
static check_item_t g_builtin_checks[] = {
    {
        .id = "language",
        .name_en = "Language Configuration",
        .name_zh = "语言配置",
        .priority = CHECK_PRIORITY_NORMAL,
        .func = check_item_language,
        .fix_func = NULL,
        .enabled = 1
    },
    {
        .id = "dependencies",
        .name_en = "Dependencies",
        .name_zh = "依赖检查",
        .priority = CHECK_PRIORITY_NORMAL,
        .func = check_item_dependencies,
        .fix_func = NULL,
        .enabled = 1
    },
    {
        .id = "hardware",
        .name_en = "Hardware",
        .name_zh = "硬件检查",
        .priority = CHECK_PRIORITY_LOW,
        .func = check_item_hardware,
        .fix_func = NULL,
        .enabled = 1
    },
    {
        .id = "config",
        .name_en = "Configuration Completeness",
        .name_zh = "配置完整性",
        .priority = CHECK_PRIORITY_CRITICAL,
        .func = check_item_config,
        .fix_func = NULL,
        .enabled = 1
    },
    {
        .id = "network",
        .name_en = "Network",
        .name_zh = "网络检查",
        .priority = CHECK_PRIORITY_LOW,
        .func = check_item_network,
        .fix_func = NULL,
        .enabled = 1
    },
    {
        .id = "permissions",
        .name_en = "Permissions",
        .name_zh = "权限检查",
        .priority = CHECK_PRIORITY_NORMAL,
        .func = check_item_permissions,
        .fix_func = NULL,
        .enabled = 1
    },
    {
        .id = "version",
        .name_en = "Version",
        .name_zh = "版本检查",
        .priority = CHECK_PRIORITY_LOW,
        .func = check_item_version,
        .fix_func = NULL,
        .enabled = 1
    }
};

/* ============================================================
 * 注册所有检查项
 * ============================================================ */
int check_items_register_all(void) {
    int count = (int)(sizeof(g_builtin_checks) / sizeof(g_builtin_checks[0]));
    int registered = 0;

    for (int i = 0; i < count; i++) {
        if (check_manager_register(&g_builtin_checks[i]) == 0) {
            registered++;
        } else {
            LOG_WARN_T("CheckItems", "Register", "Fail", "failed to register '%s'", g_builtin_checks[i].id);
        }
    }

    LOG_INFO_T("CheckItems", "Register", "OK", "registered %d/%d check items", registered, count);
    return (registered > 0) ? 0 : -1;
}
