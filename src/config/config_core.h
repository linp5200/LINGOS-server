/**
 * @file    src/config/config_core.h
 * @brief   配置核心：数据模型、加载/保存、热重载
 * @version LN-0.4.3
 * @changes 新增 config_core_save_force() 声明。
 */

#ifndef CONFIG_CORE_H
#define CONFIG_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 配置数据类型
 * ============================================================ */

typedef struct wizard_config {
    char language[8];              /* "en" 或 "zh" */
    char system_mode[16];          /* "app" 或 "system" */
    char ai_backend[16];           /* "ollama" 或 "deepseek" */
    char api_key[256];             /* DeepSeek API Key */
    char model[64];                /* 模型名称 */
    char base_url[256];            /* Base URL */
    char startup_option[16];       /* "shell" 或 "tui" */
    char user_name[64];            /* 用户称呼 */
    char ollama_url[128];          /* Ollama URL */
    char ollama_model[64];         /* Ollama 模型名 */
    int shadow_mode_enabled;       /* 1=启用, 0=禁用 */
    int auto_allow_high_risk;      /* 1=启用, 0=禁用 */
    int thinking_enabled;          /* 1=启用, 0=禁用 */
    int stream_enabled;            /* 1=启用, 0=禁用 */
    int show_thinking;             /* 1=显示思考链, 0=不显示 */
    int meta_info_enabled;         /* 1=启用元信息, 0=禁用 */
    int max_context_tokens;        /* 最大上下文 token 数 */
    int socket_timeout;            /* Socket 超时（秒） */
    int auth_timeout;              /* 授权超时（秒） */
    char log_level[8];             /* "debug"/"info"/"warn"/"error" */
    time_t configured_at;          /* 配置时间戳 */

    /* 【批次A】AI 高级配置 */
    double temperature;            /* 温度 0-2（默认 0.7） */
    double creativity;             /* 创造性 0-1（默认 0.8，映射 temperature） */
    int max_agents;                /* 可并行子AI数 1-8（默认 3） */
    char search_backend[16];       /* "searxng"/"html"（默认 searxng） */
    int search_max_urls;           /* 并行搜索最多 URL 数（默认 50） */
    int search_rate_limit;         /* 搜索频率限制 次/分钟（默认 10） */
    char personality_file[256];    /* 人格文件路径（json/md/txt） */
    char assistant_file[256];      /* 助手提示词文件路径（json/md/txt） */
    char thinking_display[16];     /* 思考显示："off"/"hidden"/"visible"（默认 visible） */
} wizard_config_t;

/* ============================================================
 * 配置加载/保存 API
 * ============================================================ */

int config_core_load(wizard_config_t *cfg);
int config_core_save(const wizard_config_t *cfg);
int config_core_save_force(const wizard_config_t *cfg);   /* 新增：强制保存 */

const wizard_config_t* config_core_get(void);
wizard_config_t* config_core_get_mutable(void);
int config_core_reload(void);
void config_core_set_defaults(wizard_config_t *cfg);
int config_core_is_configured(void);
int config_core_mark_configured(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_CORE_H */