#ifndef AI_CONFIG_H
#define AI_CONFIG_H

#include <stdint.h>

typedef enum {
    AI_BACKEND_OLLAMA,
    AI_BACKEND_DEEPSEEK,
    AI_BACKEND_PLUGIN
} ai_backend_t;

typedef struct {
    ai_backend_t backend;
    char ollama_url[256];
    char ollama_model[64];
    char deepseek_api_key[256];
    char deepseek_model[64];
    char deepseek_base_url[256];
    char deepseek_reasoning_effort[16];
    int  deepseek_enable_tools;
    int  deepseek_parallel_tools;
    int  thinking_enabled;
    char user_id[64];
    char plugin_name[64];
    char plugin_params[1024];
    int  socket_timeout;
    char stream_style[16];
    /* 子AI独立配置 */
    char sub_ai_api_key[256];
    char sub_ai_model[64];
    char sub_ai_base_url[256];
    int  show_thinking;
    int  show_tool_calls;
    int  show_tool_results;
    /* 【新增】语言配置 */
    char language[16];
    /* 【批次A】AI 高级配置 */
    double temperature;            /* 温度 0-2 */
    double creativity;             /* 创造性 0-1 */
    int max_agents;                /* 可并行子AI数 */
    char search_backend[16];       /* "searxng"/"html" */
    int search_max_urls;           /* 并行搜索最多 URL 数 */
    int search_rate_limit;         /* 搜索频率限制 次/分钟 */
    char personality_file[256];    /* 人格文件路径 */
    char assistant_file[256];      /* 助手提示词文件路径 */
    char thinking_display[16];     /* 思考显示："off"/"hidden"/"visible" */
} ai_config_t;

int ai_config_load(void);
int ai_config_save(void);
const ai_config_t* ai_config_get(void);

int ai_config_set_backend(ai_backend_t backend);
int ai_config_set_ollama_model(const char *model);
int ai_config_set_ollama_url(const char *url);
int ai_config_set_deepseek_model(const char *model);
int ai_config_set_deepseek_api_key(const char *key);
int ai_config_set_deepseek_base_url(const char *url);
int ai_config_set_deepseek_reasoning_effort(const char *effort);
int ai_config_set_deepseek_enable_tools(int enable);
int ai_config_set_deepseek_parallel_tools(int enable);
int ai_config_set_thinking_enabled(int enable);
int ai_config_set_user_id(const char *uid);
int ai_config_set_socket_timeout(int timeout);
const char* ai_config_get_stream_style(void);
int ai_config_set_stream_style(const char *style);

/* 子AI配置 setter（新增） */
int ai_config_set_sub_ai_api_key(const char *key);
int ai_config_set_sub_ai_model(const char *model);
int ai_config_set_sub_ai_base_url(const char *url);

const char* ai_config_get_current_model(void);
const char* ai_config_get_current_url(void);
const char* ai_config_get_current_api_key(void);

/* ============================================================
 * 兼容接口（早期版本暴露，保留声明避免隐式声明）
 * ============================================================ */
ai_backend_t ai_config_get_backend(void);
const char* ai_config_get_ollama_url(void);
const char* ai_config_get_ollama_model(void);
int ai_config_is_loaded(void);

/* ============================================================
 * 【批次A】AI 高级配置 setter
 * ============================================================ */
int ai_config_set_temperature(double value);
int ai_config_set_creativity(double value);
int ai_config_set_max_agents(int value);
int ai_config_set_search_backend(const char *backend);
int ai_config_set_search_max_urls(int value);
int ai_config_set_search_rate_limit(int value);
int ai_config_set_personality_file(const char *path);
int ai_config_set_assistant_file(const char *path);
int ai_config_set_thinking_display(const char *mode);

/**
 * @brief 获取当前系统语言配置
 * @return 语言字符串 ("en-US" 或 "zh-CN")
 */
const char* ai_config_get_language(void);

/**
 * @brief 设置系统语言
 * @param lang 语言字符串 ("en-US" 或 "zh-CN")
 * @return 0 成功，-1 失败
 */
int ai_config_set_language(const char *lang);

#endif