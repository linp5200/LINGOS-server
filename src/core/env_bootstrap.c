/**
 * @file    src/core/env_bootstrap.c
 * @brief   运行时环境自举：目录创建、脚本复制、默认配置生成
 * @version LN-B-5.1.2.6-rc
 * @changes 防止默认配置覆盖已有配置；若 state.json 缺失但 installed.state 存在则自动重建。
 */

#include "env_bootstrap.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../fs/fs_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

/* ============================================================
 * 内部函数声明
 * ============================================================ */
static void ensure_directories(void);
static void copy_python_scripts(void);
static void ensure_default_configs(void);
static void ensure_state_json(void);
static void create_minimal_state_file(void);

/* ============================================================
 * 主入口
 * ============================================================ */
int ensure_runtime_environment(void) {
    LOG_INFO_T("EnvBootstrap", "Ensure", "Start", "Creating runtime environment");

    /* 1. 创建目录结构 */
    ensure_directories();

    /* 2. 复制 Python 脚本 */
    copy_python_scripts();

    /* 3. 确保 state.json 存在（如果可能） */
    ensure_state_json();

    /* 4. 生成默认配置文件（仅在不存在时） */
    ensure_default_configs();

    LOG_INFO_T("EnvBootstrap", "Ensure", "OK", "Runtime environment ready");
    return 0;
}

/* ============================================================
 * 创建目录结构
 * ============================================================ */
static void ensure_directories(void) {
    const char *root = lingos_data_root();
    const char *dirs[] = {
        "/bin", "/run", "/Debug", "/system/config",
        "/state", "/data", "/Ensystem", "/apps",
        "/backups", "/cache", "/Dump", "/repairs",
        "/AH", "/snapshots", "/plugins", "/skills",
        "/models", "/registry", "/registry/core",
        "/registry/modules", "/registry/components",
        "/registry/configs", "/registry/features",
        "/registry/skills", "/registry/plugins",
        "/registry/hooks", "/registry/selfcheck",
        "/Debug/backups", "/system/backups"
    };

    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
        char path[512];
        safe_snprintf(path, sizeof(path), "%s%s", root, dirs[i]);
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            LOG_WARN_T("EnvBootstrap", "Dir", "MkdirFail", "mkdir %s: %s", path, strerror(errno));
        }
    }
}

/* ============================================================
 * 复制 Python 脚本
 * ============================================================ */
static void copy_python_scripts(void) {
    /* 在 Linux 环境中，脚本由 make install_python_script 复制 */
    /* 此处仅确保 bin 目录存在 */
    const char *root = lingos_data_root();
    char bin_path[512];
    safe_snprintf(bin_path, sizeof(bin_path), "%s/bin", root);
    mkdir(bin_path, 0755);

    /* 如果 /LINGOS/bin 为空，尝试从源码目录复制 */
    char src_path[512];
    safe_snprintf(src_path, sizeof(src_path), "%s/../src/python", root);
    if (access(src_path, F_OK) == 0) {
        char cmd[512];
        safe_snprintf(cmd, sizeof(cmd), "cp %s/*.py %s/bin/ 2>/dev/null", src_path, root);
        int ret = system(cmd);
        if (ret != 0) {
            LOG_WARN_T("EnvBootstrap", "Python", "CopyFail", "failed to copy Python scripts");
        } else {
            LOG_INFO_T("EnvBootstrap", "Python", "OK", "Python scripts copied");
        }
    }
}

/* ============================================================
 * 确保 state.json 存在
 * ============================================================ */
static void ensure_state_json(void) {
    const char *root = lingos_data_root();
    char state_path[512];
    char installed_path[512];
    safe_snprintf(state_path, sizeof(state_path), "%s/system/config/state.json", root);
    safe_snprintf(installed_path, sizeof(installed_path), "%s/Ensystem/installed.state", root);

    /* 如果 state.json 已存在，检查其内容 */
    if (access(state_path, F_OK) == 0) {
        LOG_DEBUG_T("EnvBootstrap", "State", "Exists", "state.json found");
        return;
    }

    LOG_WARN_T("EnvBootstrap", "State", "Missing", "state.json not found");

    /* 检查是否已安装 */
    if (access(installed_path, F_OK) == 0) {
        LOG_INFO_T("EnvBootstrap", "State", "Recreate", "installed.state exists, recreating state.json");

        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", tm);

        FILE *fp = fopen(state_path, "w");
        if (fp) {
            fprintf(fp, "{\n");
            fprintf(fp, "  \"system_configured\": true,\n");
            fprintf(fp, "  \"last_config_time\": \"%s\"\n", time_str);
            fprintf(fp, "}\n");
            fclose(fp);
            LOG_INFO_T("EnvBootstrap", "State", "OK", "state.json recreated");
        } else {
            LOG_ERROR_T("EnvBootstrap", "State", "WriteFail", "failed to recreate state.json: %s", strerror(errno));
        }
    } else {
        LOG_DEBUG_T("EnvBootstrap", "State", "NotInstalled", "system not installed yet, state.json will be created later");
        /* 创建最小 state.json（未配置状态） */
        create_minimal_state_file();
    }
}

/* ============================================================
 * 创建最小状态文件
 * ============================================================ */
static void create_minimal_state_file(void) {
    const char *root = lingos_data_root();
    char state_path[512];
    safe_snprintf(state_path, sizeof(state_path), "%s/system/config/state.json", root);

    /* 确保目录存在 */
    char dir_path[512];
    safe_snprintf(dir_path, sizeof(dir_path), "%s/system/config", root);
    mkdir(dir_path, 0755);

    if (access(state_path, F_OK) != 0) {
        FILE *fp = fopen(state_path, "w");
        if (fp) {
            fprintf(fp, "{\n");
            fprintf(fp, "  \"system_configured\": false,\n");
            fprintf(fp, "  \"last_config_time\": null\n");
            fprintf(fp, "}\n");
            fclose(fp);
            LOG_DEBUG_T("EnvBootstrap", "State", "Created", "minimal state.json created");
        }
    }
}

/* ============================================================
 * 确保默认配置文件（仅在不存在时创建）
 * ============================================================ */
static void ensure_default_configs(void) {
    const char *root = lingos_data_root();
    char path[512];

    /* 配置文件列表：仅当文件不存在时才创建默认值 */
    const char *config_files[] = {
        "ai_config.json",
        "user_profile.json",
        "startup.conf",
        "security.json",
        "privilege.json",
        "network.conf",
        "defense.conf",
        "health.conf",
        "sandbox.conf",
        "watchdog.conf",
        "repair_strategies.json",
        "common_issues.json"
    };

    for (size_t i = 0; i < sizeof(config_files)/sizeof(config_files[0]); i++) {
        safe_snprintf(path, sizeof(path), "%s/system/config/%s", root, config_files[i]);

        /* 如果文件已存在，跳过，不覆盖 */
        if (access(path, F_OK) == 0) {
            LOG_DEBUG_T("EnvBootstrap", "Config", "Skip", "%s already exists", config_files[i]);
            continue;
        }

        LOG_INFO_T("EnvBootstrap", "Config", "Create", "creating default %s", config_files[i]);

        /* 根据文件类型创建默认内容 */
        if (strcmp(config_files[i], "ai_config.json") == 0) {
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "{\n");
                fprintf(fp, "  \"backend\": \"ollama\",\n");
                fprintf(fp, "  \"language\": \"en\",\n");
                fprintf(fp, "  \"thinking_enabled\": true,\n");
                fprintf(fp, "  \"stream_enabled\": true,\n");
                fprintf(fp, "  \"show_thinking\": true,\n");
                fprintf(fp, "  \"meta_info_enabled\": true,\n");
                fprintf(fp, "  \"max_context_tokens\": 32768,\n");
                fprintf(fp, "  \"socket_timeout\": 60,\n");
                fprintf(fp, "  \"auth_timeout\": 60,\n");
                fprintf(fp, "  \"log_level\": \"info\",\n");
                fprintf(fp, "  \"ollama\": {\n");
                fprintf(fp, "    \"url\": \"http://127.0.0.1:8080\",\n");
                fprintf(fp, "    \"model\": \"glm-4.6:cloud\"\n");
                fprintf(fp, "  },\n");
                fprintf(fp, "  \"deepseek\": {\n");
                fprintf(fp, "    \"api_key\": \"\",\n");
                fprintf(fp, "    \"model\": \"deepseek-v4-pro\",\n");
                fprintf(fp, "    \"base_url\": \"https://api.deepseek.com\"\n");
                fprintf(fp, "  }\n");
                fprintf(fp, "}\n");
                fclose(fp);
            }
        } else if (strcmp(config_files[i], "startup.conf") == 0) {
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "# LING OS Startup Configuration\n");
                fprintf(fp, "mode = shell\n");
                fclose(fp);
            }
        } else if (strcmp(config_files[i], "security.json") == 0) {
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "{\n");
                fprintf(fp, "  \"shadow_mode\": { \"enabled\": true },\n");
                fprintf(fp, "  \"dark_mode\": { \"enabled\": false }\n");
                fprintf(fp, "}\n");
                fclose(fp);
            }
        } else if (strcmp(config_files[i], "privilege.json") == 0) {
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "{\n");
                fprintf(fp, "  \"auto_allow_high_risk\": false\n");
                fprintf(fp, "}\n");
                fclose(fp);
            }
        } else if (strcmp(config_files[i], "defense.conf") == 0) {
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "anomaly_algorithm = ewma\n");
                fprintf(fp, "behavior_monitoring = 1\n");
                fprintf(fp, "shadow_mode_default = 1\n");
                fprintf(fp, "dark_mode_default = 0\n");
                fclose(fp);
            }
        } else if (strcmp(config_files[i], "watchdog.conf") == 0) {
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "# LING OS Watchdog Configuration\n");
                fprintf(fp, "shell_crash_strategy = auto_restart\n");
                fprintf(fp, "auto_restart_delay_seconds = 3\n");
                fprintf(fp, "max_restart_per_hour = 5\n");
                fprintf(fp, "enable_core_dump = 0\n");
                fprintf(fp, "fallback_to_offline = 1\n");
                fprintf(fp, "heartbeat_timeout = 180\n");
                fclose(fp);
            }
        } else if (strcmp(config_files[i], "user_profile.json") == 0) {
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "{\n");
                fprintf(fp, "  \"user_name\": \"Sir\"\n");
                fprintf(fp, "}\n");
                fclose(fp);
            }
        } else if (strcmp(config_files[i], "common_issues.json") == 0) {
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "{\n");
                fprintf(fp, "  \"issues\": []\n");
                fprintf(fp, "}\n");
                fclose(fp);
            }
        } else if (strcmp(config_files[i], "repair_strategies.json") == 0) {
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "{\n");
                fprintf(fp, "  \"strategies\": []\n");
                fprintf(fp, "}\n");
                fclose(fp);
            }
        } else {
            /* 其他配置文件：创建空文件或简单默认 */
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "# LING OS Configuration: %s\n", config_files[i]);
                fprintf(fp, "# Auto-generated default\n");
                fclose(fp);
            }
        }
    }

    LOG_INFO_T("EnvBootstrap", "Config", "OK", "Default configs ensured");
}