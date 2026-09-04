/**
 * @file    src/config/wizard_step_defs.c
 * @brief   向导步骤定义加载器实现
 * @version LN-0.4.3
 * @par     修正内置步骤初始化列表字段顺序
 */

#include "wizard_step_defs.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>

/* ============================================================
 * 内置步骤定义（硬编码）- 修正所有初始化列表
 * ============================================================ */
static wizard_step_def_t g_builtin_steps[] = {
    {
        .id = "language",
        .title_en = "Language Selection",
        .title_zh = "语言选择",
        .type = STEP_TYPE_SELECT,
        .option_count = 2,
        .options = {
            {"en", "English", "English", "", "mode", 0, 0, 0},
            {"zh", "中文", "中文", "", "mode", 0, 0, 0}
        },
        .next_step = "mode"
    },
    {
        .id = "mode",
        .title_en = "System Mode",
        .title_zh = "系统模式",
        .type = STEP_TYPE_SELECT,
        .option_count = 2,
        .options = {
            {"app", "APP Mode (recommended)", "APP 模式（推荐）", "", "ai_backend", 0, 0, 0},
            {"system", "SYSTEM Mode", "SYSTEM 模式", "", "ai_backend", 0, 0, 0}
        },
        .next_step = "ai_backend"
    },
    {
        .id = "ai_backend",
        .title_en = "AI Backend",
        .title_zh = "AI 后端",
        .type = STEP_TYPE_SELECT,
        .option_count = 2,
        .options = {
            {"ollama", "Ollama (local)", "Ollama（本地）", "", "startup", 0, 0, 0},
            {"deepseek", "DeepSeek (cloud)", "DeepSeek（云端）", "", "deepseek_api_key", 0, 0, 0}
        },
        .next_step = "startup"
    },
    {
        .id = "deepseek_api_key",
        .title_en = "DeepSeek API Key",
        .title_zh = "DeepSeek API Key",
        .type = STEP_TYPE_INPUT,
        .input_prompt_en = "Enter API Key (starts with sk-):",
        .input_prompt_zh = "请输入 API Key（以 sk- 开头）：",
        .validate_rule = "api_key",
        .next_step = "deepseek_model",
        .parent_step = "ai_backend"
    },
    {
        .id = "deepseek_model",
        .title_en = "DeepSeek Model",
        .title_zh = "DeepSeek 模型",
        .type = STEP_TYPE_SELECT,
        .option_count = 2,
        .options = {
            {"deepseek-v4-pro", "DeepSeek Pro", "DeepSeek Pro", "", "deepseek_base_url", 0, 0, 0},
            {"deepseek-v4-flash", "DeepSeek Flash", "DeepSeek Flash", "", "deepseek_base_url", 0, 0, 0}
        },
        .next_step = "deepseek_base_url",
        .parent_step = "ai_backend"
    },
    {
        .id = "deepseek_base_url",
        .title_en = "DeepSeek Base URL",
        .title_zh = "DeepSeek Base URL",
        .type = STEP_TYPE_INPUT,
        .input_prompt_en = "Base URL (or use default):",
        .input_prompt_zh = "Base URL（或使用默认）：",
        .default_value = "https://api.deepseek.com",
        .validate_rule = "url",
        .next_step = "startup",
        .parent_step = "ai_backend"
    },
    {
        .id = "startup",
        .title_en = "Startup Option",
        .title_zh = "启动选项",
        .type = STEP_TYPE_SELECT,
        .option_count = 2,
        .options = {
            {"shell", "Shell first", "先进入 Shell", "", "", 0, 0, 0},
            {"tui", "TUI Desktop first", "先进入 TUI 桌面", "", "", 0, 0, 0}
        },
        .next_step = ""
    }
};

static const int g_builtin_count = sizeof(g_builtin_steps) / sizeof(g_builtin_steps[0]);

/* ============================================================
 * 从动态模块加载
 * ============================================================ */
int wizard_step_defs_load_module(const char *path, wizard_step_def_t **steps, int *count) {
    if (!path || !steps || !count) return -1;

    if (access(path, F_OK) != 0) {
        LOG_DEBUG_T("StepDefs", "Module", "NotFound", "module not found at %s", path);
        return -1;
    }

    void *handle = dlopen(path, RTLD_LAZY);
    if (!handle) {
        LOG_WARN_T("StepDefs", "Module", "DlopenFail", "%s", dlerror());
        return -1;
    }

    wizard_step_def_t* (*get_steps)(int *count) =
        (wizard_step_def_t*(*)(int*))dlsym(handle, "get_wizard_steps");
    if (!get_steps) {
        LOG_WARN_T("StepDefs", "Module", "SymbolFail", "get_wizard_steps not found");
        dlclose(handle);
        return -1;
    }

    int cnt = 0;
    wizard_step_def_t *mod_steps = get_steps(&cnt);
    if (!mod_steps || cnt <= 0) {
        LOG_WARN_T("StepDefs", "Module", "NoSteps", "module returned no steps");
        dlclose(handle);
        return -1;
    }

    wizard_step_def_t *copy = malloc(sizeof(wizard_step_def_t) * cnt);
    if (!copy) {
        dlclose(handle);
        return -1;
    }
    memcpy(copy, mod_steps, sizeof(wizard_step_def_t) * cnt);
    *steps = copy;
    *count = cnt;

    LOG_INFO_T("StepDefs", "Module", "OK", "loaded %d steps from module", cnt);
    return 0;
}

/* ============================================================
 * 从 JSON 加载
 * ============================================================ */
int wizard_step_defs_load_json(const char *path, wizard_step_def_t **steps, int *count) {
    if (!path || !steps || !count) return -1;

    if (access(path, F_OK) != 0) {
        LOG_DEBUG_T("StepDefs", "JSON", "NotFound", "JSON not found at %s", path);
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *content = malloc(size + 1);
    if (!content) { fclose(fp); return -1; }
    fread(content, 1, size, fp);
    content[size] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(content);
    free(content);
    if (!root) {
        LOG_WARN_T("StepDefs", "JSON", "ParseFail", "invalid JSON");
        return -1;
    }

    cJSON *steps_arr = cJSON_GetObjectItem(root, "steps");
    if (!cJSON_IsArray(steps_arr)) {
        cJSON_Delete(root);
        return -1;
    }

    int cnt = cJSON_GetArraySize(steps_arr);
    if (cnt <= 0) {
        cJSON_Delete(root);
        return -1;
    }

    wizard_step_def_t *defs = malloc(sizeof(wizard_step_def_t) * cnt);
    if (!defs) {
        cJSON_Delete(root);
        return -1;
    }

    for (int i = 0; i < cnt; i++) {
        cJSON *item = cJSON_GetArrayItem(steps_arr, i);
        wizard_step_def_t *step = &defs[i];
        memset(step, 0, sizeof(wizard_step_def_t));

        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(id)) safe_strncpy(step->id, id->valuestring, sizeof(step->id));
        cJSON *title_en = cJSON_GetObjectItem(item, "title_en");
        if (cJSON_IsString(title_en)) safe_strncpy(step->title_en, title_en->valuestring, sizeof(step->title_en));
        cJSON *title_zh = cJSON_GetObjectItem(item, "title_zh");
        if (cJSON_IsString(title_zh)) safe_strncpy(step->title_zh, title_zh->valuestring, sizeof(step->title_zh));

        cJSON *type = cJSON_GetObjectItem(item, "type");
        if (cJSON_IsString(type)) {
            if (strcmp(type->valuestring, "input") == 0) step->type = STEP_TYPE_INPUT;
            else step->type = STEP_TYPE_SELECT;
        }

        cJSON *options = cJSON_GetObjectItem(item, "options");
        if (cJSON_IsArray(options)) {
            int opt_cnt = cJSON_GetArraySize(options);
            if (opt_cnt > 16) opt_cnt = 16;
            step->option_count = opt_cnt;
            for (int j = 0; j < opt_cnt; j++) {
                cJSON *opt = cJSON_GetArrayItem(options, j);
                cJSON *oid = cJSON_GetObjectItem(opt, "id");
                if (cJSON_IsString(oid)) safe_strncpy(step->options[j].id, oid->valuestring, sizeof(step->options[j].id));
                cJSON *label_en = cJSON_GetObjectItem(opt, "label_en");
                if (cJSON_IsString(label_en)) safe_strncpy(step->options[j].label_en, label_en->valuestring, sizeof(step->options[j].label_en));
                cJSON *label_zh = cJSON_GetObjectItem(opt, "label_zh");
                if (cJSON_IsString(label_zh)) safe_strncpy(step->options[j].label_zh, label_zh->valuestring, sizeof(step->options[j].label_zh));
                cJSON *next = cJSON_GetObjectItem(opt, "next");
                if (cJSON_IsString(next)) safe_strncpy(step->options[j].next_step, next->valuestring, sizeof(step->options[j].next_step));
            }
        }

        cJSON *input_prompt_en = cJSON_GetObjectItem(item, "input_prompt_en");
        if (cJSON_IsString(input_prompt_en)) safe_strncpy(step->input_prompt_en, input_prompt_en->valuestring, sizeof(step->input_prompt_en));
        cJSON *input_prompt_zh = cJSON_GetObjectItem(item, "input_prompt_zh");
        if (cJSON_IsString(input_prompt_zh)) safe_strncpy(step->input_prompt_zh, input_prompt_zh->valuestring, sizeof(step->input_prompt_zh));
        cJSON *validate = cJSON_GetObjectItem(item, "validate");
        if (cJSON_IsString(validate)) safe_strncpy(step->validate_rule, validate->valuestring, sizeof(step->validate_rule));
        cJSON *default_val = cJSON_GetObjectItem(item, "default");
        if (cJSON_IsString(default_val)) safe_strncpy(step->default_value, default_val->valuestring, sizeof(step->default_value));
        cJSON *next_step = cJSON_GetObjectItem(item, "next");
        if (cJSON_IsString(next_step)) safe_strncpy(step->next_step, next_step->valuestring, sizeof(step->next_step));
        cJSON *parent = cJSON_GetObjectItem(item, "parent");
        if (cJSON_IsString(parent)) safe_strncpy(step->parent_step, parent->valuestring, sizeof(step->parent_step));
    }

    cJSON_Delete(root);
    *steps = defs;
    *count = cnt;
    LOG_INFO_T("StepDefs", "JSON", "OK", "loaded %d steps from JSON", cnt);
    return 0;
}

/* ============================================================
 * 获取内置步骤
 * ============================================================ */
int wizard_step_defs_get_builtin(wizard_step_def_t **steps, int *count) {
    if (!steps || !count) return -1;
    *steps = g_builtin_steps;
    *count = g_builtin_count;
    LOG_DEBUG_T("StepDefs", "Builtin", "OK", "returning %d built-in steps", g_builtin_count);
    return 0;
}

/* ============================================================
 * 释放动态步骤
 * ============================================================ */
void wizard_step_defs_free(wizard_step_def_t *steps, int count) {
    (void)count;
    if (steps) {
        if (steps == g_builtin_steps) return;
        free(steps);
    }
}

/* ============================================================
 * 查找步骤
 * ============================================================ */
wizard_step_def_t* wizard_step_defs_find(wizard_step_def_t *steps, int count, const char *id) {
    if (!steps || !id) return NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(steps[i].id, id) == 0) {
            return &steps[i];
        }
    }
    return NULL;
}