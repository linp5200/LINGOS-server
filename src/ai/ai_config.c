/**
 * @file    src/ai/ai_config.c
 * @brief   AI 配置管理（统一从 config_core 读取）
 * @version LN-0.4.3
 * @par     核心协议：C1, C3, CM, UD-DR#S1, UD-SYS#W2, AI-CTL
 * @changes 对齐 ai_config.h 接口契约，修复编译错误：
 *          - 结构体成员改用 deepseek_api_key / deepseek_model / deepseek_base_url /
 *            socket_timeout / stream_style
 *          - ai_config_load() 返回类型 void -> int（匹配头文件声明）
 *          - getter 重命名：get_api_key -> get_current_api_key 等
 *          - 补齐头文件声明的 ai_config_save() / setter 族 / stream_style / language 接口
 *          - C3 修正：相对路径 include 改为文件名直接引用（需 Makefile -I 配合）
 *          - 假设：config_core 的 stream_enabled(int) 映射为 stream_style("color"/"plain")
 */

#include "ai_config.h"
#include "config_core.h"
#include "safe_string.h"
#include "log_extra.h"
#include <string.h>

/* ============================================================
 * 静态配置缓存
 * ============================================================ */
static ai_config_t g_ai_config = {
    .backend = AI_BACKEND_OLLAMA,
    .thinking_enabled = 1,
    .show_thinking = 1,
    .stream_style = "color",
    .socket_timeout = 60,
    .language = "en",
    .ollama_url = "http://127.0.0.1:8080",
    .ollama_model = "glm-4.6:cloud",
    .deepseek_api_key = "",
    .deepseek_model = "deepseek-v4-pro",
    .deepseek_base_url = "https://api.deepseek.com",
    /* 【批次A】AI 高级配置默认值 */
    .temperature = 0.7,
    .creativity = 0.8,
    .max_agents = 3,
    .search_backend = "searxng"
};
static int g_ai_config_loaded = 0;

/* ============================================================
 * 内部辅助：字符串安全复制
 * ============================================================ */
static void safe_str_copy(char *dest, const char *src, size_t size) {
    if (dest && src) {
        safe_strncpy(dest, src, size);
    }
}

/* ============================================================
 * FTF[加载AI配置，从config_core读取并缓存]
 * ============================================================ */
int ai_config_load(void) {
    /* FF[src/config/config_core.c]-CFN[config_core_get]-FTF[获取全局配置单例] */
    const wizard_config_t *cfg = config_core_get();
    if (!cfg) {
        LOG_WARN_T("AIConfig", "Load", "NoConfig", "config_core not available, using defaults");
        g_ai_config_loaded = 1;
        return 0;
    }

    /* ---- 从 config_core 读取配置 ---- */
    /* 后端类型 */
    if (strcmp(cfg->ai_backend, "deepseek") == 0) {
        g_ai_config.backend = AI_BACKEND_DEEPSEEK;
    } else {
        g_ai_config.backend = AI_BACKEND_OLLAMA;
    }

    /* 语言 */
    safe_str_copy(g_ai_config.language, cfg->language, sizeof(g_ai_config.language));

    /* 通用配置 */
    g_ai_config.thinking_enabled = cfg->thinking_enabled;
    g_ai_config.show_thinking = cfg->show_thinking;
    g_ai_config.socket_timeout = cfg->socket_timeout;

    /* stream_enabled(int) -> stream_style(char[16]) 映射（假设：1=color, 0=plain） */
    if (cfg->stream_enabled) {
        safe_str_copy(g_ai_config.stream_style, "color", sizeof(g_ai_config.stream_style));
    } else {
        safe_str_copy(g_ai_config.stream_style, "plain", sizeof(g_ai_config.stream_style));
    }

    /* Ollama 配置 */
    safe_str_copy(g_ai_config.ollama_url, cfg->ollama_url, sizeof(g_ai_config.ollama_url));
    safe_str_copy(g_ai_config.ollama_model, cfg->ollama_model, sizeof(g_ai_config.ollama_model));

    /* DeepSeek 配置 */
    safe_str_copy(g_ai_config.deepseek_api_key, cfg->api_key, sizeof(g_ai_config.deepseek_api_key));
    safe_str_copy(g_ai_config.deepseek_model, cfg->model, sizeof(g_ai_config.deepseek_model));
    safe_str_copy(g_ai_config.deepseek_base_url, cfg->base_url, sizeof(g_ai_config.deepseek_base_url));

    /* 【批次A】AI 高级配置 */
    g_ai_config.temperature = cfg->temperature;
    g_ai_config.creativity = cfg->creativity;
    g_ai_config.max_agents = cfg->max_agents;
    safe_str_copy(g_ai_config.search_backend, cfg->search_backend, sizeof(g_ai_config.search_backend));
    g_ai_config.search_max_urls = cfg->search_max_urls;
    g_ai_config.search_rate_limit = cfg->search_rate_limit;
    safe_str_copy(g_ai_config.personality_file, cfg->personality_file, sizeof(g_ai_config.personality_file));
    safe_str_copy(g_ai_config.assistant_file, cfg->assistant_file, sizeof(g_ai_config.assistant_file));
    safe_str_copy(g_ai_config.thinking_display, cfg->thinking_display, sizeof(g_ai_config.thinking_display));

    g_ai_config_loaded = 1;

    LOG_INFO_T("AIConfig", "Load", "OK", "backend=%s language=%s stream_style=%s (from config_core)",
               g_ai_config.backend == AI_BACKEND_DEEPSEEK ? "deepseek" : "ollama",
               g_ai_config.language, g_ai_config.stream_style);
    return 0;
}

/* ============================================================
 * FTF[获取当前AI配置（只读）]
 * ============================================================ */
const ai_config_t* ai_config_get(void) {
    if (!g_ai_config_loaded) {
        ai_config_load();
    }
    return &g_ai_config;
}

/* ============================================================
 * FTF[保存AI配置：缓存回写 config_core 并强制持久化]
 * ============================================================ */
int ai_config_save(void) {
    /* FF[src/config/config_core.c]-CFN[config_core_get_mutable]-FTF[获取可变配置单例] */
    wizard_config_t *cfg = config_core_get_mutable();
    if (!cfg) return -1;

    switch (g_ai_config.backend) {
        case AI_BACKEND_DEEPSEEK:
            safe_str_copy(cfg->ai_backend, "deepseek", sizeof(cfg->ai_backend));
            break;
        case AI_BACKEND_OLLAMA:
            safe_str_copy(cfg->ai_backend, "ollama", sizeof(cfg->ai_backend));
            break;
        default:
            break;
    }
    safe_str_copy(cfg->language, g_ai_config.language, sizeof(cfg->language));
    cfg->thinking_enabled = g_ai_config.thinking_enabled;
    cfg->show_thinking = g_ai_config.show_thinking;
    cfg->socket_timeout = g_ai_config.socket_timeout;
    cfg->stream_enabled = (strcmp(g_ai_config.stream_style, "plain") != 0);

    safe_str_copy(cfg->ollama_url, g_ai_config.ollama_url, sizeof(cfg->ollama_url));
    safe_str_copy(cfg->ollama_model, g_ai_config.ollama_model, sizeof(cfg->ollama_model));
    safe_str_copy(cfg->api_key, g_ai_config.deepseek_api_key, sizeof(cfg->api_key));
    safe_str_copy(cfg->model, g_ai_config.deepseek_model, sizeof(cfg->model));
    safe_str_copy(cfg->base_url, g_ai_config.deepseek_base_url, sizeof(cfg->base_url));

    /* 【批次A】AI 高级配置回写 */
    cfg->temperature = g_ai_config.temperature;
    cfg->creativity = g_ai_config.creativity;
    cfg->max_agents = g_ai_config.max_agents;
    safe_str_copy(cfg->search_backend, g_ai_config.search_backend, sizeof(cfg->search_backend));
    cfg->search_max_urls = g_ai_config.search_max_urls;
    cfg->search_rate_limit = g_ai_config.search_rate_limit;
    safe_str_copy(cfg->personality_file, g_ai_config.personality_file, sizeof(cfg->personality_file));
    safe_str_copy(cfg->assistant_file, g_ai_config.assistant_file, sizeof(cfg->assistant_file));

    /* FF[src/config/config_core.c]-CFN[config_core_save_force]-FTF[强制保存配置到文件] */
    return config_core_save_force(cfg);
}

/* ============================================================
 * FTF[获取当前API Key（DeepSeek）]
 * ============================================================ */
const char* ai_config_get_current_api_key(void) {
    if (!g_ai_config_loaded) {
        ai_config_load();
    }
    return g_ai_config.deepseek_api_key;
}

/* ============================================================
 * FTF[获取当前模型名称（DeepSeek）]
 * ============================================================ */
const char* ai_config_get_current_model(void) {
    if (!g_ai_config_loaded) {
        ai_config_load();
    }
    return g_ai_config.deepseek_model;
}

/* ============================================================
 * FTF[获取当前Base URL（DeepSeek）]
 * ============================================================ */
const char* ai_config_get_current_url(void) {
    if (!g_ai_config_loaded) {
        ai_config_load();
    }
    return g_ai_config.deepseek_base_url;
}

/* ============================================================
 * FTF[获取Ollama URL]
 * ============================================================ */
const char* ai_config_get_ollama_url(void) {
    if (!g_ai_config_loaded) {
        ai_config_load();
    }
    return g_ai_config.ollama_url;
}

/* ============================================================
 * FTF[获取Ollama模型名称]
 * ============================================================ */
const char* ai_config_get_ollama_model(void) {
    if (!g_ai_config_loaded) {
        ai_config_load();
    }
    return g_ai_config.ollama_model;
}

/* ============================================================
 * FTF[获取当前AI后端类型]
 * ============================================================ */
ai_backend_t ai_config_get_backend(void) {
    if (!g_ai_config_loaded) {
        ai_config_load();
    }
    return g_ai_config.backend;
}

/* ============================================================
 * FTF[检查是否已加载配置]
 * ============================================================ */
int ai_config_is_loaded(void) {
    return g_ai_config_loaded;
}

/* ============================================================
 * FTF[获取流式/显示风格]
 * ============================================================ */
const char* ai_config_get_stream_style(void) {
    if (!g_ai_config_loaded) {
        ai_config_load();
    }
    return g_ai_config.stream_style;
}

/* ============================================================
 * FTF[设置流式/显示风格]
 * ============================================================ */
int ai_config_set_stream_style(const char *style) {
    if (!style || !*style) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.stream_style, style, sizeof(g_ai_config.stream_style));
    return 0;
}

/* ============================================================
 * FTF[获取当前语言配置]
 * ============================================================ */
const char* ai_config_get_language(void) {
    if (!g_ai_config_loaded) {
        ai_config_load();
    }
    return g_ai_config.language;
}

/* ============================================================
 * FTF[设置系统语言]
 * ============================================================ */
int ai_config_set_language(const char *lang) {
    if (!lang) return -1;
    if (strcmp(lang, "en") != 0 && strcmp(lang, "zh") != 0 &&
        strcmp(lang, "en-US") != 0 && strcmp(lang, "zh-CN") != 0) {
        return -1;
    }
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.language, lang, sizeof(g_ai_config.language));
    return 0;
}

/* ============================================================
 * setter 族（仅修改缓存，持久化需调用 ai_config_save()）
 * ============================================================ */
int ai_config_set_backend(ai_backend_t backend) {
    if (backend != AI_BACKEND_OLLAMA && backend != AI_BACKEND_DEEPSEEK &&
        backend != AI_BACKEND_PLUGIN) {
        return -1;
    }
    if (!g_ai_config_loaded) ai_config_load();
    g_ai_config.backend = backend;
    return 0;
}

int ai_config_set_ollama_model(const char *model) {
    if (!model) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.ollama_model, model, sizeof(g_ai_config.ollama_model));
    return 0;
}

int ai_config_set_ollama_url(const char *url) {
    if (!url) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.ollama_url, url, sizeof(g_ai_config.ollama_url));
    return 0;
}

int ai_config_set_deepseek_model(const char *model) {
    if (!model) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.deepseek_model, model, sizeof(g_ai_config.deepseek_model));
    return 0;
}

int ai_config_set_deepseek_api_key(const char *key) {
    if (!key) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.deepseek_api_key, key, sizeof(g_ai_config.deepseek_api_key));
    return 0;
}

int ai_config_set_deepseek_base_url(const char *url) {
    if (!url) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.deepseek_base_url, url, sizeof(g_ai_config.deepseek_base_url));
    return 0;
}

int ai_config_set_deepseek_reasoning_effort(const char *effort) {
    if (!effort) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.deepseek_reasoning_effort, effort,
                  sizeof(g_ai_config.deepseek_reasoning_effort));
    return 0;
}

int ai_config_set_deepseek_enable_tools(int enable) {
    if (!g_ai_config_loaded) ai_config_load();
    g_ai_config.deepseek_enable_tools = enable ? 1 : 0;
    return 0;
}

int ai_config_set_deepseek_parallel_tools(int enable) {
    if (!g_ai_config_loaded) ai_config_load();
    g_ai_config.deepseek_parallel_tools = enable ? 1 : 0;
    return 0;
}

int ai_config_set_thinking_enabled(int enable) {
    if (!g_ai_config_loaded) ai_config_load();
    g_ai_config.thinking_enabled = enable ? 1 : 0;
    return 0;
}

int ai_config_set_user_id(const char *uid) {
    if (!uid) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.user_id, uid, sizeof(g_ai_config.user_id));
    return 0;
}

int ai_config_set_socket_timeout(int timeout) {
    if (timeout <= 0) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    g_ai_config.socket_timeout = timeout;
    return 0;
}

int ai_config_set_sub_ai_api_key(const char *key) {
    if (!key) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.sub_ai_api_key, key, sizeof(g_ai_config.sub_ai_api_key));
    return 0;
}

int ai_config_set_sub_ai_model(const char *model) {
    if (!model) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.sub_ai_model, model, sizeof(g_ai_config.sub_ai_model));
    return 0;
}

int ai_config_set_sub_ai_base_url(const char *url) {
    if (!url) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.sub_ai_base_url, url, sizeof(g_ai_config.sub_ai_base_url));
    return 0;
}

/* ============================================================
 * 【批次A】AI 高级配置 setter
 * ============================================================ */
int ai_config_set_temperature(double value) {
    if (value < 0 || value > 2) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    g_ai_config.temperature = value;
    return 0;
}

int ai_config_set_creativity(double value) {
    if (value < 0 || value > 1) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    g_ai_config.creativity = value;
    return 0;
}

int ai_config_set_max_agents(int value) {
    if (value < 1 || value > 8) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    g_ai_config.max_agents = value;
    return 0;
}

int ai_config_set_search_backend(const char *backend) {
    if (!backend) return -1;
    if (strcmp(backend, "searxng") != 0 && strcmp(backend, "html") != 0) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.search_backend, backend, sizeof(g_ai_config.search_backend));
    return 0;
}

int ai_config_set_search_max_urls(int value) {
    if (value < 1 || value > 100) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    g_ai_config.search_max_urls = value;
    return 0;
}

int ai_config_set_search_rate_limit(int value) {
    if (value < 1) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    g_ai_config.search_rate_limit = value;
    return 0;
}

int ai_config_set_personality_file(const char *path) {
    if (!path) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.personality_file, path, sizeof(g_ai_config.personality_file));
    return 0;
}

int ai_config_set_assistant_file(const char *path) {
    if (!path) return -1;
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.assistant_file, path, sizeof(g_ai_config.assistant_file));
    return 0;
}

int ai_config_set_thinking_display(const char *mode) {
    if (!mode) return -1;
    if (strcmp(mode, "off") != 0 && strcmp(mode, "hidden") != 0 && strcmp(mode, "visible") != 0) {
        return -1;
    }
    if (!g_ai_config_loaded) ai_config_load();
    safe_str_copy(g_ai_config.thinking_display, mode, sizeof(g_ai_config.thinking_display));
    return 0;
}