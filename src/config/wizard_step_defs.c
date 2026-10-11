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
 * 内置步骤定义（硬编码）
 * 【2026-10-11 第 3 期·配置收官】正式启用声明式清单 16 字段：
 *   group / desc双 / advanced / skippable / secret / validate_hint双 /
 *   requires / restart_required / order / since_version
 *   —— 三端同源渲染（C Clack 向导 / Web / App）读取同一批字段；
 *   旧字段保持原样，旧调用方零改动。
 * ============================================================ */
static wizard_step_def_t g_builtin_steps[] = {
    {
        .id = "language",
        .title_en = "Language Selection",
        .title_zh = "语言选择",
        .desc_en = "Interface and AI reply language. Changeable later in settings.",
        .desc_zh = "界面与 AI 回复语言。之后可在设置中修改。",
        .type = STEP_TYPE_SELECT,
        .option_count = 2,
        .options = {
            {"en", "English", "English", "", "mode", 0, 0, 0},
            {"zh", "中文", "中文", "", "mode", 0, 0, 0}
        },
        .next_step = "mode",
        .group = "ui",
        .order = 10,
        .since_version = "0.4.3"
    },
    {
        .id = "mode",
        .title_en = "System Mode",
        .title_zh = "系统模式",
        .desc_en = "APP mode: the phone app is the main console. SYSTEM mode: this host runs as an always-on service.",
        .desc_zh = "APP 模式：以手机 App 为主控。SYSTEM 模式：本机作为常驻服务运行。",
        .type = STEP_TYPE_SELECT,
        .option_count = 2,
        .options = {
            {"app", "APP Mode (recommended)", "APP 模式（推荐）", "", "ai_backend", 0, 0, 0},
            {"system", "SYSTEM Mode", "SYSTEM 模式", "", "ai_backend", 0, 0, 0}
        },
        .next_step = "ai_backend",
        .group = "runtime",
        .order = 20,
        .restart_required = 1,
        .since_version = "0.4.3"
    },
    {
        .id = "ai_backend",
        .title_en = "AI Backend",
        .title_zh = "AI 后端",
        .desc_en = "Ollama runs models locally (privacy first); DeepSeek goes to the cloud (needs an API key).",
        .desc_zh = "Ollama 本地推理（隐私优先）；DeepSeek 走云端（需要 API Key）。",
        .type = STEP_TYPE_SELECT,
        .option_count = 2,
        .options = {
            {"ollama", "Ollama (local)", "Ollama（本地）", "", "startup", 0, 0, 0},
            {"deepseek", "DeepSeek (cloud)", "DeepSeek（云端）", "", "deepseek_api_key", 0, 0, 0}
        },
        .next_step = "startup",
        .group = "ai",
        .order = 30,
        .restart_required = 1,
        .since_version = "0.4.3"
    },
    {
        .id = "deepseek_api_key",
        .title_en = "DeepSeek API Key",
        .title_zh = "DeepSeek API Key",
        .desc_en = "Stored on this machine only, with value-level encryption.",
        .desc_zh = "仅在本机保存（值级加密），不上传到其他位置。",
        .type = STEP_TYPE_INPUT,
        .input_prompt_en = "Enter API Key (starts with sk-):",
        .input_prompt_zh = "请输入 API Key（以 sk- 开头）：",
        .validate_rule = "api_key",
        .validate_hint_en = "API Key must start with sk- and be at least 20 characters.",
        .validate_hint_zh = "API Key 需以 sk- 开头且不少于 20 个字符。",
        .next_step = "deepseek_model",
        .parent_step = "ai_backend",
        .group = "ai",
        .secret = 1,
        .order = 40,
        .since_version = "0.4.3"
    },
    {
        .id = "deepseek_model",
        .title_en = "DeepSeek Model",
        .title_zh = "DeepSeek 模型",
        .desc_en = "Pro is stronger; Flash is faster and cheaper. Switchable at runtime.",
        .desc_zh = "Pro 能力更强；Flash 更快更省。运行时可切换。",
        .type = STEP_TYPE_SELECT,
        .option_count = 2,
        .options = {
            {"deepseek-v4-pro", "DeepSeek Pro", "DeepSeek Pro", "", "deepseek_base_url", 0, 0, 0},
            {"deepseek-v4-flash", "DeepSeek Flash", "DeepSeek Flash", "", "deepseek_base_url", 0, 0, 0}
        },
        .next_step = "deepseek_base_url",
        .parent_step = "ai_backend",
        .group = "ai",
        .order = 50,
        .since_version = "0.4.3"
    },
    {
        .id = "deepseek_base_url",
        .title_en = "DeepSeek Base URL",
        .title_zh = "DeepSeek Base URL",
        .desc_en = "Change only when using a proxy or a self-hosted gateway.",
        .desc_zh = "仅在代理或自建网关时才需修改。",
        .type = STEP_TYPE_INPUT,
        .input_prompt_en = "Base URL (or use default):",
        .input_prompt_zh = "Base URL（或使用默认）：",
        .validate_rule = "url",
        .validate_hint_en = "Must be an http(s) URL, for example https://api.deepseek.com",
        .validate_hint_zh = "需为 http(s) 链接，例如 https://api.deepseek.com",
        .default_value = "https://api.deepseek.com",
        .next_step = "startup",
        .parent_step = "ai_backend",
        .group = "ai",
        .advanced = 1,
        .skippable = 1,
        .order = 60,
        .since_version = "0.4.3"
    },
    {
        .id = "startup",
        .title_en = "Startup Option",
        .title_zh = "启动选项",
        .desc_en = "Which interface comes up after boot: the shell console or the TUI desktop.",
        .desc_zh = "开机后进入哪个界面：Shell 控制台或 TUI 桌面。",
        .type = STEP_TYPE_SELECT,
        .option_count = 2,
        .options = {
            {"shell", "Shell first", "先进入 Shell", "", "", 0, 0, 0},
            {"tui", "TUI Desktop first", "先进入 TUI 桌面", "", "", 0, 0, 0}
        },
        .next_step = "",
        .group = "runtime",
        .order = 70,
        .restart_required = 1,
        .since_version = "0.4.3"
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

        /* ---- 【2026-10-11】声明式清单 16 字段：外部清单同样可携带 ----
         * 缺省值 = 0/空 = 旧行为（兼容铁律：老三端清单零改动可继续跑） */
        cJSON *group = cJSON_GetObjectItem(item, "group");
        if (cJSON_IsString(group)) safe_strncpy(step->group, group->valuestring, sizeof(step->group));
        cJSON *desc_en = cJSON_GetObjectItem(item, "desc_en");
        if (cJSON_IsString(desc_en)) safe_strncpy(step->desc_en, desc_en->valuestring, sizeof(step->desc_en));
        cJSON *desc_zh = cJSON_GetObjectItem(item, "desc_zh");
        if (cJSON_IsString(desc_zh)) safe_strncpy(step->desc_zh, desc_zh->valuestring, sizeof(step->desc_zh));
        cJSON *advanced = cJSON_GetObjectItem(item, "advanced");
        if (cJSON_IsBool(advanced)) step->advanced = cJSON_IsTrue(advanced);
        cJSON *skippable = cJSON_GetObjectItem(item, "skippable");
        if (cJSON_IsBool(skippable)) step->skippable = cJSON_IsTrue(skippable);
        cJSON *secret = cJSON_GetObjectItem(item, "secret");
        if (cJSON_IsBool(secret)) step->secret = cJSON_IsTrue(secret);
        cJSON *optional = cJSON_GetObjectItem(item, "optional");
        if (cJSON_IsBool(optional)) step->optional = cJSON_IsTrue(optional);
        cJSON *restart_req = cJSON_GetObjectItem(item, "restart_required");
        if (cJSON_IsBool(restart_req)) step->restart_required = cJSON_IsTrue(restart_req);
        cJSON *order = cJSON_GetObjectItem(item, "order");
        if (cJSON_IsNumber(order)) step->order = order->valueint;
        cJSON *since = cJSON_GetObjectItem(item, "since_version");
        if (cJSON_IsString(since)) safe_strncpy(step->since_version, since->valuestring, sizeof(step->since_version));
        cJSON *requires = cJSON_GetObjectItem(item, "requires");
        if (cJSON_IsString(requires)) safe_strncpy(step->requires, requires->valuestring, sizeof(step->requires));
        cJSON *vh_en = cJSON_GetObjectItem(item, "validate_hint_en");
        if (cJSON_IsString(vh_en)) safe_strncpy(step->validate_hint_en, vh_en->valuestring, sizeof(step->validate_hint_en));
        cJSON *vh_zh = cJSON_GetObjectItem(item, "validate_hint_zh");
        if (cJSON_IsString(vh_zh)) safe_strncpy(step->validate_hint_zh, vh_zh->valuestring, sizeof(step->validate_hint_zh));
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