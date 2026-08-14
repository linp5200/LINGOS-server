/**
 * @file    model_router.c
 * @brief   多模型路由实现（完整版）
 * @version LN-B-5.0.0.0
 * @changes 安全字符串替换；双文支持；日志标准化
 */

#include "model_router.h"
#include "../../common/data_path.h"
#include "../../common/safe_string.h"
#include "../../common/lang.h"
#include "../../lib/log_extra.h"
#include "../../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>

#define MAX_RULES 32
#define CONFIG_PATH "/system/config/model_routing.json"

/* ============================================================
 * 全局状态
 * ============================================================ */

static routing_rule_t g_rules[MAX_RULES];
static int g_rule_count = 0;
static int g_initialized = 0;
static pthread_mutex_t g_rule_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * 默认路由规则（全部使用 DeepSeek，四选项）
 * ============================================================ */

static const routing_rule_t default_rules[] = {
    {
        .intent = INTENT_CHAT,
        .model = MODEL_DEEPSEEK,
        .variant = "flash",
        .thinking_enabled = 0,
        .keywords = {"你好", "hello", "hi", "hey", "greeting", "聊天", "闲聊", "最近怎么样"},
        .keyword_count = 8,
        .priority = 10
    },
    {
        .intent = INTENT_REASONING,
        .model = MODEL_DEEPSEEK,
        .variant = "pro",
        .thinking_enabled = 1,
        .keywords = {"分析", "推理", "规划", "方案", "策略", "为什么", "如何解决", "深层"},
        .keyword_count = 8,
        .priority = 20
    },
    {
        .intent = INTENT_CODE,
        .model = MODEL_DEEPSEEK,
        .variant = "pro",
        .thinking_enabled = 0,
        .keywords = {"代码", "写", "实现", "函数", "算法", "bug", "调试", "编程"},
        .keyword_count = 8,
        .priority = 20
    },
    {
        .intent = INTENT_SIMPLE_QA,
        .model = MODEL_DEEPSEEK,
        .variant = "flash",
        .thinking_enabled = 0,
        .keywords = {"什么是", "是什么", "解释", "定义", "简单", "基础", "教程"},
        .keyword_count = 7,
        .priority = 10
    },
    {
        .intent = INTENT_SYSTEM,
        .model = MODEL_DEEPSEEK,
        .variant = "flash",
        .thinking_enabled = 0,
        .keywords = {"系统状态", "查看", "列出", "显示", "运行", "状态", "内存", "磁盘"},
        .keyword_count = 8,
        .priority = 10
    }
};

/* ============================================================
 * 内部辅助：获取配置文件路径
 * ============================================================ */

static const char* get_config_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, CONFIG_PATH);
    }
    return path;
}

/* ============================================================
 * 内部辅助：确保目录存在
 * ============================================================ */

static void ensure_config_dir(void) {
    const char *root = lingos_data_root();
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    if (access(dir, F_OK) != 0) {
        mkdir(dir, 0755);
    }
}

/* ============================================================
 * 内部辅助：加载默认规则
 * ============================================================ */

static void load_default_rules(void) {
    LOG_DEBUG_T("ModelRouter", "LoadDefaults", "Enter", "loading default rules");

    pthread_mutex_lock(&g_rule_lock);

    g_rule_count = sizeof(default_rules) / sizeof(default_rules[0]);
    for (int i = 0; i < g_rule_count && i < MAX_RULES; i++) {
        memcpy(&g_rules[i], &default_rules[i], sizeof(routing_rule_t));
        LOG_DEBUG_T("ModelRouter", "LoadDefaults", "Rule", "intent=%d variant=%s thinking=%d",
                    g_rules[i].intent, g_rules[i].variant, g_rules[i].thinking_enabled);
    }

    pthread_mutex_unlock(&g_rule_lock);
    LOG_INFO_T("ModelRouter", "LoadDefaults", "OK", "loaded %d default rules", g_rule_count);
}

/* ============================================================
 * 内部辅助：文本预处理（小写、去标点）
 * ============================================================ */

static void preprocess_text(const char *input, char *output, size_t out_size) {
    if (!input || !output || out_size == 0) {
        if (output && out_size > 0) output[0] = '\0';
        return;
    }

    char *p = output;
    const char *s = input;
    size_t remaining = out_size - 1;

    while (*s && remaining > 0) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') {
            *p++ = c + 32;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ') {
            *p++ = c;
        }
        s++;
        remaining--;
    }
    *p = '\0';
}

/* ============================================================
 * 内部辅助：检查关键词匹配
 * ============================================================ */

static int match_keywords(const char *text, const routing_rule_t *rule) {
    char preprocessed[1024];
    preprocess_text(text, preprocessed, sizeof(preprocessed));

    for (int i = 0; i < rule->keyword_count; i++) {
        const char *kw = rule->keywords[i];
        /* 【修复】含非 ASCII（中文/UTF-8）关键词直接在原文匹配；
         * 纯 ASCII 关键词用预处理（小写+去标点）文本匹配 */
        int has_utf8 = 0;
        for (const unsigned char *p = (const unsigned char*)kw; *p; p++) {
            if (*p >= 0x80) { has_utf8 = 1; break; }
        }
        if (has_utf8) {
            if (strstr(text, kw) != NULL) {
                return 1;
            }
        } else {
            if (strstr(preprocessed, kw) != NULL) {
                return 1;
            }
        }
    }
    return 0;
}

/* ============================================================
 * 内部辅助：从 cJSON 加载规则
 * ============================================================ */

static int load_rule_from_json(cJSON *item) {
    if (!item || !cJSON_IsObject(item)) return -1;

    routing_rule_t rule;
    memset(&rule, 0, sizeof(rule));

    cJSON *intent = cJSON_GetObjectItem(item, "intent");
    cJSON *model = cJSON_GetObjectItem(item, "model");
    cJSON *variant = cJSON_GetObjectItem(item, "variant");
    cJSON *thinking = cJSON_GetObjectItem(item, "thinking_enabled");
    cJSON *keywords = cJSON_GetObjectItem(item, "keywords");
    cJSON *priority = cJSON_GetObjectItem(item, "priority");

    if (!intent || !cJSON_IsNumber(intent)) return -1;
    if (!model || !cJSON_IsNumber(model)) return -1;
    if (!variant || !cJSON_IsString(variant)) return -1;
    if (!keywords || !cJSON_IsArray(keywords)) return -1;

    rule.intent = (intent_type_t)intent->valueint;
    rule.model = (model_type_t)model->valueint;
    safe_strncpy(rule.variant, variant->valuestring, sizeof(rule.variant));
    rule.thinking_enabled = (thinking && cJSON_IsBool(thinking)) ? cJSON_IsTrue(thinking) : 0;

    int kw_count = cJSON_GetArraySize(keywords);
    if (kw_count > 10) kw_count = 10;
    for (int i = 0; i < kw_count; i++) {
        cJSON *kw = cJSON_GetArrayItem(keywords, i);
        if (kw && cJSON_IsString(kw) && kw->valuestring) {
            safe_strncpy(rule.keywords[i], kw->valuestring, sizeof(rule.keywords[i]));
            rule.keyword_count++;
        }
    }

    if (priority && cJSON_IsNumber(priority)) {
        rule.priority = priority->valueint;
    } else {
        rule.priority = 10;
    }

    if (rule.keyword_count == 0) return -1;

    pthread_mutex_lock(&g_rule_lock);

    if (g_rule_count >= MAX_RULES) {
        pthread_mutex_unlock(&g_rule_lock);
        LOG_WARN_T("ModelRouter", "LoadJSON", "Overflow", tr("max rules reached", "已达最大规则数"));
        return -1;
    }

    memcpy(&g_rules[g_rule_count], &rule, sizeof(rule));
    g_rule_count++;

    pthread_mutex_unlock(&g_rule_lock);

    LOG_DEBUG_T("ModelRouter", "LoadJSON", "OK", "loaded rule intent=%d variant=%s thinking=%d",
                rule.intent, rule.variant, rule.thinking_enabled);
    return 0;
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

int model_router_init(void) {
    LOG_INFO_T("ModelRouter", "Init", "Enter", "initializing model router");

    if (g_initialized) {
        LOG_DEBUG_T("ModelRouter", "Init", "Already", "already initialized");
        return 0;
    }

    pthread_mutex_lock(&g_rule_lock);
    memset(g_rules, 0, sizeof(g_rules));
    g_rule_count = 0;
    pthread_mutex_unlock(&g_rule_lock);

    const char *path = get_config_path();
    int ret = model_router_load_config(path);
    if (ret != 0) {
        LOG_DEBUG_T("ModelRouter", "Init", "NoConfig", "config not found, loading defaults");
        load_default_rules();
    } else {
        LOG_INFO_T("ModelRouter", "Init", "ConfigLoaded", "loaded rules from %s", path);
    }

    g_initialized = 1;
    LOG_INFO_T("ModelRouter", "Init", "OK", "model router initialized with %d rules", g_rule_count);
    return 0;
}

int model_router_route(const char *message, const char *session_id, routing_result_t *result) {
    LOG_INFO_T("ModelRouter", "Route", "Enter", "message='%.100s...', session='%s'",
               message ? message : "(null)", session_id ? session_id : "(null)");

    if (!message || !result) {
        LOG_ERROR_T("ModelRouter", "Route", "Invalid", "message=%p, result=%p", (void*)message, (void*)result);
        return -1;
    }

    if (!g_initialized) {
        if (model_router_init() != 0) {
            LOG_ERROR_T("ModelRouter", "Route", "InitFail", "router not initialized");
            return -1;
        }
    }

    memset(result, 0, sizeof(routing_result_t));
    result->intent = INTENT_UNKNOWN;
    result->model = MODEL_DEEPSEEK;
    safe_strncpy(result->variant, "flash", sizeof(result->variant));
    result->thinking_enabled = 0;
    result->confidence = 0.0f;
    result->reason[0] = '\0';

    size_t msg_len = strlen(message);
    if (msg_len < 2) {
        result->intent = INTENT_CHAT;
        result->model = MODEL_DEEPSEEK;
        safe_strncpy(result->variant, "flash", sizeof(result->variant));
        result->thinking_enabled = 0;
        result->confidence = 0.8f;
        safe_strncpy(result->reason, tr("Very short message, treated as chat", "消息很短，视为闲聊"), sizeof(result->reason));
        LOG_DEBUG_T("ModelRouter", "Route", "ShortMsg", "treating as chat");
        return 0;
    }

    pthread_mutex_lock(&g_rule_lock);

    int best_match = -1;
    int best_priority = -1;
    float best_confidence = 0.0f;

    for (int i = 0; i < g_rule_count; i++) {
        if (match_keywords(message, &g_rules[i])) {
            if (g_rules[i].priority > best_priority) {
                best_match = i;
                best_priority = g_rules[i].priority;
                best_confidence = 0.8f + (g_rules[i].keyword_count * 0.02f);
                if (best_confidence > 1.0f) best_confidence = 1.0f;
            }
        }
    }

    pthread_mutex_unlock(&g_rule_lock);

    if (best_match >= 0) {
        result->intent = g_rules[best_match].intent;
        result->model = g_rules[best_match].model;
        safe_strncpy(result->variant, g_rules[best_match].variant, sizeof(result->variant));
        result->thinking_enabled = g_rules[best_match].thinking_enabled;
        result->confidence = best_confidence;
        safe_snprintf(result->reason, sizeof(result->reason),
                      tr("matched %d keywords in rule #%d", "匹配规则 #%d 中的 %d 个关键词"),
                      g_rules[best_match].keyword_count, best_match);
        LOG_INFO_T("ModelRouter", "Route", "Matched", "intent=%s, model=%s, variant=%s, thinking=%d, confidence=%.2f",
                   model_router_intent_name(result->intent),
                   model_router_model_name(result->model),
                   result->variant,
                   result->thinking_enabled,
                   result->confidence);
    } else {
        result->intent = INTENT_UNKNOWN;
        result->model = MODEL_DEEPSEEK;
        safe_strncpy(result->variant, "flash", sizeof(result->variant));
        result->thinking_enabled = 0;
        result->confidence = 0.5f;
        safe_strncpy(result->reason, tr("No rule matched, using fallback (flash+no thinking)",
                                        "未匹配到规则，使用降级方案 (flash+无思考)"),
                     sizeof(result->reason));
        LOG_DEBUG_T("ModelRouter", "Route", "NoMatch", "using fallback");
    }

    return 0;
}

const char* model_router_intent_name(intent_type_t intent) {
    switch (intent) {
        case INTENT_CHAT:       return tr("chat", "闲聊");
        case INTENT_REASONING:  return tr("reasoning", "推理");
        case INTENT_CODE:       return tr("code", "代码");
        case INTENT_SIMPLE_QA:  return tr("simple_qa", "简单问答");
        case INTENT_SYSTEM:     return tr("system", "系统");
        case INTENT_UNKNOWN:    return tr("unknown", "未知");
        default:                return tr("unknown", "未知");
    }
}

const char* model_router_model_name(model_type_t model) {
    switch (model) {
        case MODEL_OLLAMA:      return "ollama";
        case MODEL_DEEPSEEK:    return "deepseek";
        case MODEL_PLUGIN:      return "plugin";
        case MODEL_DEFAULT:     return "default";
        default:                return "unknown";
    }
}

int model_router_rule_count(void) {
    pthread_mutex_lock(&g_rule_lock);
    int count = g_rule_count;
    pthread_mutex_unlock(&g_rule_lock);
    return count;
}

int model_router_add_rule(const routing_rule_t *rule) {
    if (!rule) {
        LOG_ERROR_T("ModelRouter", "AddRule", "Invalid", "rule is NULL");
        return -1;
    }
    if (rule->keyword_count == 0) {
        LOG_ERROR_T("ModelRouter", "AddRule", "Invalid", "no keywords");
        return -1;
    }

    pthread_mutex_lock(&g_rule_lock);

    if (g_rule_count >= MAX_RULES) {
        pthread_mutex_unlock(&g_rule_lock);
        LOG_WARN_T("ModelRouter", "AddRule", "Overflow", tr("max rules reached", "已达最大规则数"));
        return -1;
    }

    memcpy(&g_rules[g_rule_count], rule, sizeof(routing_rule_t));
    g_rule_count++;

    pthread_mutex_unlock(&g_rule_lock);

    LOG_INFO_T("ModelRouter", "AddRule", "OK", "added rule intent=%d", rule->intent);
    return 0;
}

int model_router_load_config(const char *path) {
    LOG_INFO_T("ModelRouter", "LoadConfig", "Enter", "path='%s'", path ? path : "(null)");

    if (!path) {
        path = get_config_path();
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("ModelRouter", "LoadConfig", "NotFound", "config file not found");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(fp);
        LOG_ERROR_T("ModelRouter", "LoadConfig", "MallocFail", "malloc failed");
        return -1;
    }

    fread(buf, 1, size, fp);
    buf[size] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        LOG_ERROR_T("ModelRouter", "LoadConfig", "ParseFail", "invalid JSON");
        return -1;
    }

    cJSON *rules = cJSON_GetObjectItem(root, "rules");
    if (!rules || !cJSON_IsArray(rules)) {
        LOG_WARN_T("ModelRouter", "LoadConfig", "NoRules", "no rules array");
        cJSON_Delete(root);
        return -1;
    }

    int loaded = 0;
    int size_arr = cJSON_GetArraySize(rules);
    for (int i = 0; i < size_arr; i++) {
        cJSON *item = cJSON_GetArrayItem(rules, i);
        if (load_rule_from_json(item) == 0) {
            loaded++;
        }
    }

    cJSON_Delete(root);

    LOG_INFO_T("ModelRouter", "LoadConfig", "OK", "loaded %d rules from %s", loaded, path);
    return 0;
}

int model_router_save_config(const char *path) {
    LOG_INFO_T("ModelRouter", "SaveConfig", "Enter", "path='%s'", path ? path : "(null)");

    if (!path) {
        path = get_config_path();
    }

    ensure_config_dir();

    cJSON *root = cJSON_CreateObject();
    cJSON *rules = cJSON_CreateArray();

    pthread_mutex_lock(&g_rule_lock);

    for (int i = 0; i < g_rule_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "intent", g_rules[i].intent);
        cJSON_AddNumberToObject(item, "model", g_rules[i].model);
        cJSON_AddStringToObject(item, "variant", g_rules[i].variant);
        cJSON_AddBoolToObject(item, "thinking_enabled", g_rules[i].thinking_enabled);
        cJSON *kw_array = cJSON_CreateArray();
        for (int j = 0; j < g_rules[i].keyword_count; j++) {
            cJSON_AddItemToArray(kw_array, cJSON_CreateString(g_rules[i].keywords[j]));
        }
        cJSON_AddItemToObject(item, "keywords", kw_array);
        cJSON_AddNumberToObject(item, "priority", g_rules[i].priority);
        cJSON_AddItemToArray(rules, item);
    }

    pthread_mutex_unlock(&g_rule_lock);

    cJSON_AddItemToObject(root, "rules", rules);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        LOG_ERROR_T("ModelRouter", "SaveConfig", "PrintFail", "cJSON_PrintUnformatted failed");
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        free(json_str);
        LOG_ERROR_T("ModelRouter", "SaveConfig", "OpenFail", "cannot write %s", path);
        return -1;
    }

    fprintf(fp, "%s\n", json_str);
    fclose(fp);
    free(json_str);

    LOG_INFO_T("ModelRouter", "SaveConfig", "OK", "saved %d rules to %s", g_rule_count, path);
    return 0;
}

void model_router_cleanup(void) {
    LOG_INFO_T("ModelRouter", "Cleanup", "Enter", "cleaning up model router");

    pthread_mutex_lock(&g_rule_lock);
    memset(g_rules, 0, sizeof(g_rules));
    g_rule_count = 0;
    g_initialized = 0;
    pthread_mutex_unlock(&g_rule_lock);

    LOG_INFO_T("ModelRouter", "Cleanup", "OK", "model router cleaned up");
}