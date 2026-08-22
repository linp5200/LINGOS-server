/**
 * @file    src/config/wizard_engine.c
 * @brief   向导引擎实现
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C-C, AI-CTL
 * @changes 添加 #include "config_renderer.h"；
 *          修正 builtin 步骤初始化列表字段顺序；
 *          在 handle_input 中集成健康检查（renderer_is_healthy）；
 *          支持自动降级触发（返回 -2）。
 */

#include "wizard_engine.h"
#include "wizard_step_defs.h"
#include "config_validator.h"
#include "config_saver.h"
#include "config_renderer.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../common/data_path.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>

/* ============================================================
 * 静态函数声明
 * ============================================================ */
static int engine_load_from_module(wizard_engine_ctx_t *ctx);
static int engine_load_from_json(wizard_engine_ctx_t *ctx);
static int engine_load_builtin(wizard_engine_ctx_t *ctx);
static wizard_step_def_t* find_step(wizard_engine_ctx_t *ctx, const char *id);
static void engine_reset_stack(wizard_engine_ctx_t *ctx);
static int engine_push_stack(wizard_engine_ctx_t *ctx, int index);
static int engine_pop_stack(wizard_engine_ctx_t *ctx);
static int engine_top_stack(wizard_engine_ctx_t *ctx);

/* ============================================================
 * FTF[健康检查辅助]
 * ============================================================ */
static int engine_check_health(wizard_engine_ctx_t *ctx) {
    if (!ctx || !ctx->renderer) return 1;

    /* FF[src/config/config_renderer.h]-CFN[renderer_is_healthy]-FTF[检查渲染器健康状态] */
    int healthy = renderer_is_healthy(ctx->renderer);
    if (!healthy) {
        LOG_WARN_T("WizardEngine", "Health", "Anomaly", "renderer health check failed");
        static int health_fail_count = 0;
        health_fail_count++;
        if (health_fail_count >= 3) {
            LOG_WARN_T("WizardEngine", "Health", "Trigger", "health fail count >= 3, triggering recovery");
            health_fail_count = 0;
            return 0;
        }
        return 1;
    }
    return 1;
}

/* ============================================================
 * FTF[初始化向导引擎]
 * ============================================================ */
int wizard_engine_init(wizard_engine_ctx_t *ctx, int renderer_type) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(wizard_engine_ctx_t));
    config_core_set_defaults(&ctx->config);
    ctx->renderer_type = renderer_type;
    ctx->stack_capacity = 16;
    ctx->stack = malloc(sizeof(int) * ctx->stack_capacity);
    if (!ctx->stack) return -1;
    ctx->stack_size = 0;
    ctx->force_skip_verify = 0;
    LOG_INFO_T("WizardEngine", "Init", "OK", "renderer_type=%d", renderer_type);
    return 0;
}

/* ============================================================
 * FTF[加载步骤（三级降级）]
 * ============================================================ */
/* 【先生要求】设置向导模式（0=快速 1=完整）——load_steps 前调用 */
void wizard_engine_set_mode(wizard_engine_ctx_t *ctx, int mode) {
    if (ctx) ctx->wizard_mode = (mode == 1) ? 1 : 0;
}

/* 【先生要求】快速模式过滤（跳过 optional 步骤——必要项配置） */
static void filter_quick_steps(wizard_engine_ctx_t *ctx) {
    if (!ctx || ctx->wizard_mode != 0) return;
    int kept = 0;
    for (int i = 0; i < ctx->step_count; i++)
        if (!ctx->steps[i].optional) kept++;
    if (kept == ctx->step_count || kept <= 0) return;
    wizard_step_def_t *new_steps = malloc(sizeof(wizard_step_def_t) * kept);
    if (!new_steps) return;
    int j = 0;
    for (int i = 0; i < ctx->step_count; i++)
        if (!ctx->steps[i].optional) new_steps[j++] = ctx->steps[i];
    free(ctx->steps);
    ctx->steps = new_steps;
    ctx->step_count = kept;
    LOG_INFO_T("WizardEngine", "QuickMode", "OK", "quick mode: %d steps (kept necessary only)", kept);
}

int wizard_engine_load_steps(wizard_engine_ctx_t *ctx) {
    if (!ctx) return -1;
    int ret = -1;
    if (engine_load_from_module(ctx) == 0) {
        LOG_INFO_T("WizardEngine", "LoadSteps", "Module", "loaded from dynamic module");
        ret = 0;
    } else if (engine_load_from_json(ctx) == 0) {
        LOG_INFO_T("WizardEngine", "LoadSteps", "JSON", "loaded from JSON file");
        ret = 0;
    } else if (engine_load_builtin(ctx) == 0) {
        LOG_INFO_T("WizardEngine", "LoadSteps", "Builtin", "loaded from built-in defaults");
        ret = 0;
    } else {
        LOG_ERROR_T("WizardEngine", "LoadSteps", "AllFail", "all loading methods failed");
        return -1;
    }
    /* 快速模式过滤（默认快速——先生要求） */
    filter_quick_steps(ctx);
    return ret;
}

/* ============================================================
 * 动态加载模块（保持不变）
 * ============================================================ */
static int engine_load_from_module(wizard_engine_ctx_t *ctx) {
    const char *plugin_dir = "/LINGOS/plugins/wizard";
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/step_module.so", plugin_dir);
    if (access(path, F_OK) != 0) {
        LOG_DEBUG_T("WizardEngine", "Module", "NotFound", "no module at %s", path);
        return -1;
    }

    void *handle = dlopen(path, RTLD_LAZY);
    if (!handle) {
        LOG_WARN_T("WizardEngine", "Module", "DlopenFail", "%s", dlerror());
        return -1;
    }

    wizard_step_def_t* (*get_steps)(int *count) =
        (wizard_step_def_t*(*)(int*))dlsym(handle, "get_wizard_steps");
    if (!get_steps) {
        LOG_WARN_T("WizardEngine", "Module", "SymbolFail", "get_wizard_steps not found");
        dlclose(handle);
        return -1;
    }

    int count = 0;
    wizard_step_def_t *steps = get_steps(&count);
    if (!steps || count <= 0) {
        LOG_WARN_T("WizardEngine", "Module", "NoSteps", "module returned no steps");
        dlclose(handle);
        return -1;
    }

    ctx->steps = malloc(sizeof(wizard_step_def_t) * count);
    if (!ctx->steps) {
        dlclose(handle);
        return -1;
    }
    memcpy(ctx->steps, steps, sizeof(wizard_step_def_t) * count);
    ctx->step_count = count;
    ctx->current_index = 0;
    engine_reset_stack(ctx);

    LOG_INFO_T("WizardEngine", "Module", "OK", "loaded %d steps from module", count);
    return 0;
}

/* ============================================================
 * 从 JSON 加载（保持不变）
 * ============================================================ */
static int engine_load_from_json(wizard_engine_ctx_t *ctx) {
    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/system/config/wizard_steps.json", root);
    if (access(path, F_OK) != 0) {
        LOG_DEBUG_T("WizardEngine", "JSON", "NotFound", "no JSON at %s", path);
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

    cJSON *root_json = cJSON_Parse(content);
    free(content);
    if (!root_json) {
        LOG_WARN_T("WizardEngine", "JSON", "ParseFail", "invalid JSON");
        return -1;
    }

    cJSON *steps_array = cJSON_GetObjectItem(root_json, "steps");
    if (!cJSON_IsArray(steps_array)) {
        cJSON_Delete(root_json);
        return -1;
    }

    int count = cJSON_GetArraySize(steps_array);
    if (count <= 0) {
        cJSON_Delete(root_json);
        return -1;
    }

    ctx->steps = malloc(sizeof(wizard_step_def_t) * count);
    if (!ctx->steps) {
        cJSON_Delete(root_json);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(steps_array, i);
        wizard_step_def_t *step = &ctx->steps[i];
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
            int opt_count = cJSON_GetArraySize(options);
            if (opt_count > 16) opt_count = 16;
            step->option_count = opt_count;
            for (int j = 0; j < opt_count; j++) {
                cJSON *opt = cJSON_GetArrayItem(options, j);
                cJSON *oid = cJSON_GetObjectItem(opt, "id");
                if (cJSON_IsString(oid)) safe_strncpy(step->options[j].id,
                                                       oid->valuestring,
                                                       sizeof(step->options[j].id));
                cJSON *label_en = cJSON_GetObjectItem(opt, "label_en");
                if (cJSON_IsString(label_en)) safe_strncpy(step->options[j].label_en,
                                                            label_en->valuestring,
                                                            sizeof(step->options[j].label_en));
                cJSON *label_zh = cJSON_GetObjectItem(opt, "label_zh");
                if (cJSON_IsString(label_zh)) safe_strncpy(step->options[j].label_zh,
                                                            label_zh->valuestring,
                                                            sizeof(step->options[j].label_zh));
                cJSON *next = cJSON_GetObjectItem(opt, "next");
                if (cJSON_IsString(next)) safe_strncpy(step->options[j].next_step,
                                                        next->valuestring,
                                                        sizeof(step->options[j].next_step));
            }
        }

        cJSON *input_prompt_en = cJSON_GetObjectItem(item, "input_prompt_en");
        if (cJSON_IsString(input_prompt_en)) safe_strncpy(step->input_prompt_en,
                                                           input_prompt_en->valuestring,
                                                           sizeof(step->input_prompt_en));
        cJSON *input_prompt_zh = cJSON_GetObjectItem(item, "input_prompt_zh");
        if (cJSON_IsString(input_prompt_zh)) safe_strncpy(step->input_prompt_zh,
                                                           input_prompt_zh->valuestring,
                                                           sizeof(step->input_prompt_zh));
        cJSON *validate = cJSON_GetObjectItem(item, "validate");
        if (cJSON_IsString(validate)) safe_strncpy(step->validate_rule,
                                                     validate->valuestring,
                                                     sizeof(step->validate_rule));
        cJSON *default_val = cJSON_GetObjectItem(item, "default");
        if (cJSON_IsString(default_val)) safe_strncpy(step->default_value,
                                                       default_val->valuestring,
                                                       sizeof(step->default_value));
        cJSON *next_step = cJSON_GetObjectItem(item, "next");
        if (cJSON_IsString(next_step)) safe_strncpy(step->next_step,
                                                     next_step->valuestring,
                                                     sizeof(step->next_step));
        cJSON *parent = cJSON_GetObjectItem(item, "parent");
        if (cJSON_IsString(parent)) safe_strncpy(step->parent_step,
                                                  parent->valuestring,
                                                  sizeof(step->parent_step));
    }

    ctx->step_count = count;
    ctx->current_index = 0;
    engine_reset_stack(ctx);
    cJSON_Delete(root_json);
    LOG_INFO_T("WizardEngine", "JSON", "OK", "loaded %d steps from JSON", count);
    return 0;
}

/* ============================================================
 * 本地内置（硬编码）- 修正所有初始化列表
 * ============================================================ */
static int engine_load_builtin(wizard_engine_ctx_t *ctx) {
    static wizard_step_def_t builtin_steps[] = {
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
    int count = sizeof(builtin_steps) / sizeof(builtin_steps[0]);
    ctx->steps = malloc(sizeof(wizard_step_def_t) * count);
    if (!ctx->steps) return -1;
    memcpy(ctx->steps, builtin_steps, sizeof(builtin_steps));
    ctx->step_count = count;
    ctx->current_index = 0;
    engine_reset_stack(ctx);
    LOG_INFO_T("WizardEngine", "Builtin", "OK", "loaded %d built-in steps", count);
    return 0;
}

/* ============================================================
 * FTF[查找步骤]
 * ============================================================ */
static wizard_step_def_t* find_step(wizard_engine_ctx_t *ctx, const char *id) {
    if (!ctx || !id) return NULL;
    for (int i = 0; i < ctx->step_count; i++) {
        if (strcmp(ctx->steps[i].id, id) == 0) {
            return &ctx->steps[i];
        }
    }
    return NULL;
}

/* ============================================================
 * 栈管理
 * ============================================================ */
static void engine_reset_stack(wizard_engine_ctx_t *ctx) {
    ctx->stack_size = 0;
}

static int engine_push_stack(wizard_engine_ctx_t *ctx, int index) {
    if (ctx->stack_size >= ctx->stack_capacity) {
        int new_cap = ctx->stack_capacity * 2;
        int *new_stack = realloc(ctx->stack, sizeof(int) * new_cap);
        if (!new_stack) return -1;
        ctx->stack = new_stack;
        ctx->stack_capacity = new_cap;
    }
    ctx->stack[ctx->stack_size++] = index;
    return 0;
}

static int engine_pop_stack(wizard_engine_ctx_t *ctx) {
    if (ctx->stack_size <= 0) return -1;
    return ctx->stack[--ctx->stack_size];
}

static int engine_top_stack(wizard_engine_ctx_t *ctx) {
    if (ctx->stack_size <= 0) return -1;
    return ctx->stack[ctx->stack_size - 1];
}

/* ============================================================
 * FTF[运行向导]
 * ============================================================ */
int wizard_engine_run(wizard_engine_ctx_t *ctx) {
    if (!ctx || ctx->step_count == 0) {
        LOG_ERROR_T("WizardEngine", "Run", "NoSteps", "no steps loaded");
        return -1;
    }
    ctx->current_index = 0;
    ctx->cancelled = 0;
    ctx->force_skip_verify = 0;
    engine_reset_stack(ctx);
    LOG_INFO_T("WizardEngine", "Run", "Start", "running wizard with %d steps", ctx->step_count);
    return 0;
}

/* ============================================================
 * FTF[处理输入（含健康检查与自动降级）]
 * ============================================================ */
int wizard_engine_handle_input(wizard_engine_ctx_t *ctx, const char *input) {
    if (!ctx || ctx->cancelled) return -1;
    if (ctx->current_index >= ctx->step_count) return 1;

    /* 健康检查（触发降级） */
    if (!engine_check_health(ctx) && !ctx->force_skip_verify) {
        LOG_WARN_T("WizardEngine", "Health", "Block", "renderer unhealthy, blocking input");
        return -2;
    }

    wizard_step_def_t *step = &ctx->steps[ctx->current_index];
    if (!step) return -1;

    if (input && (strcmp(input, "q") == 0 || strcmp(input, "Q") == 0)) {
        ctx->cancelled = 1;
        LOG_INFO_T("WizardEngine", "Input", "Cancel", "user cancelled");
        return -1;
    }

    /* ESC 返回父步骤 */
    if (input && strcmp(input, "ESC") == 0) {
        if (step->parent_step[0] != '\0') {
            wizard_step_def_t *parent = find_step(ctx, step->parent_step);
            if (parent) {
                ctx->current_index = parent - ctx->steps;
                return 0;
            }
        }
        if (ctx->stack_size > 0) {
            ctx->current_index = engine_pop_stack(ctx);
            return 0;
        }
        ctx->cancelled = 1;
        return -1;
    }

    if (step->type == STEP_TYPE_SELECT) {
        /* 【2026-08-22 定稿】回车默认值逻辑：
         * - 有默认(default_value=选项id) → 回车=采用默认
         * - 无默认 → 回车提示"尚未选择"，不进入下一阶段 */
        int choice = 0;
        if (!input || input[0] == '\0') {
            if (step->default_value[0] != '\0') {
                for (int i = 0; i < step->option_count; i++) {
                    if (strcmp(step->options[i].id, step->default_value) == 0) {
                        choice = i + 1;
                        uart_puts(tr("\n[Using default]\n", "\n[采用默认]\n"));
                        break;
                    }
                }
                if (choice == 0) {
                    uart_puts(tr("\n⚠ Not selected yet. Please choose.\n", "\n⚠ 尚未选择。请选择。\n"));
                    return 0;
                }
            } else {
                uart_puts(tr("\n⚠ Not selected yet. Please choose.\n", "\n⚠ 尚未选择。请选择。\n"));
                return 0;
            }
        } else {
            choice = atoi(input);
        }
        if (choice < 1 || choice > step->option_count) {
            LOG_WARN_T("WizardEngine", "Input", "InvalidChoice", "choice=%d out of range", choice);
            return 0;
        }
        wizard_option_t *opt = &step->options[choice - 1];
        if (opt->is_disabled) return 0;

        if (strcmp(step->id, "language") == 0) {
            safe_strncpy(ctx->config.language, opt->id, sizeof(ctx->config.language));
        } else if (strcmp(step->id, "mode") == 0) {
            safe_strncpy(ctx->config.system_mode, opt->id, sizeof(ctx->config.system_mode));
        } else if (strcmp(step->id, "ai_backend") == 0) {
            safe_strncpy(ctx->config.ai_backend, opt->id, sizeof(ctx->config.ai_backend));
        } else if (strcmp(step->id, "deepseek_model") == 0) {
            safe_strncpy(ctx->config.model, opt->id, sizeof(ctx->config.model));
        } else if (strcmp(step->id, "startup") == 0) {
            safe_strncpy(ctx->config.startup_option, opt->id, sizeof(ctx->config.startup_option));
        }

        const char *next_id = opt->next_step[0] ? opt->next_step : step->next_step;
        if (next_id && next_id[0] != '\0') {
            wizard_step_def_t *next_step = find_step(ctx, next_id);
            if (next_step) {
                if (step->parent_step[0] != '\0' &&
                    strcmp(next_id, step->parent_step) != 0) {
                    engine_push_stack(ctx, ctx->current_index);
                }
                ctx->current_index = next_step - ctx->steps;
                return 0;
            }
        }
        ctx->current_index = ctx->step_count;
        return 1;
    } else if (step->type == STEP_TYPE_INPUT) {
        char value[256];
        /* 【2026-08-22 定稿】回车默认值逻辑：有默认→回车=默认；无默认→提示尚未选择 */
        if (!input || input[0] == '\0') {
            if (step->default_value[0] != '\0') {
                safe_strncpy(value, step->default_value, sizeof(value));
                uart_puts(tr("\n[Using default]\n", "\n[采用默认]\n"));
            } else {
                uart_puts(tr("\n⚠ Not selected yet. Please choose.\n", "\n⚠ 尚未选择。请选择。\n"));
                return 0;
            }
        } else {
            safe_strncpy(value, input, sizeof(value));
        }
        char err_msg[256];
        if (config_validate(value, step->validate_rule, err_msg, sizeof(err_msg)) != 0) {
            LOG_WARN_T("WizardEngine", "Input", "ValidationFail", "%s", err_msg);
            return 0;
        }

        if (strcmp(step->id, "deepseek_api_key") == 0) {
            safe_strncpy(ctx->config.api_key, value, sizeof(ctx->config.api_key));
        } else if (strcmp(step->id, "deepseek_base_url") == 0) {
            safe_strncpy(ctx->config.base_url, value, sizeof(ctx->config.base_url));
        }

        const char *next_id = step->next_step;
        if (next_id && next_id[0] != '\0') {
            wizard_step_def_t *next_step = find_step(ctx, next_id);
            if (next_step) {
                ctx->current_index = next_step - ctx->steps;
                return 0;
            }
        }
        ctx->current_index = ctx->step_count;
        return 1;
    }

    return 0;
}

/* ============================================================
 * 获取当前步骤
 * ============================================================ */
wizard_step_def_t* wizard_engine_current_step(wizard_engine_ctx_t *ctx) {
    if (!ctx || ctx->current_index >= ctx->step_count) return NULL;
    return &ctx->steps[ctx->current_index];
}

/* ============================================================
 * 跳转步骤
 * ============================================================ */
int wizard_engine_goto(wizard_engine_ctx_t *ctx, const char *step_id) {
    wizard_step_def_t *step = find_step(ctx, step_id);
    if (!step) return -1;
    ctx->current_index = step - ctx->steps;
    return 0;
}

/* ============================================================
 * 返回父步骤
 * ============================================================ */
int wizard_engine_back(wizard_engine_ctx_t *ctx) {
    if (ctx->stack_size > 0) {
        ctx->current_index = engine_pop_stack(ctx);
        return 0;
    }
    wizard_step_def_t *step = wizard_engine_current_step(ctx);
    if (step && step->parent_step[0] != '\0') {
        wizard_step_def_t *parent = find_step(ctx, step->parent_step);
        if (parent) {
            ctx->current_index = parent - ctx->steps;
            return 0;
        }
    }
    return -1;
}

/* ============================================================
 * 获取选项
 * ============================================================ */
int wizard_engine_get_options(wizard_engine_ctx_t *ctx, wizard_option_t *options, int max_count) {
    wizard_step_def_t *step = wizard_engine_current_step(ctx);
    if (!step || step->type != STEP_TYPE_SELECT) return 0;
    int count = step->option_count;
    if (count > max_count) count = max_count;
    memcpy(options, step->options, sizeof(wizard_option_t) * count);
    return count;
}

/* ============================================================
 * 获取输入提示
 * ============================================================ */
const char* wizard_engine_get_input_prompt(wizard_engine_ctx_t *ctx) {
    wizard_step_def_t *step = wizard_engine_current_step(ctx);
    if (!step || step->type != STEP_TYPE_INPUT) return NULL;
    const char *lang = ctx->config.language;
    if (strcmp(lang, "zh") == 0) return step->input_prompt_zh;
    return step->input_prompt_en;
}

/* ============================================================
 * 设置值
 * ============================================================ */
int wizard_engine_set_value(wizard_engine_ctx_t *ctx, const char *option_id,
                            const char *input_value) {
    (void)ctx; (void)option_id; (void)input_value;
    return 0;
}

/* ============================================================
 * FTF[保存配置]
 * ============================================================ */
int wizard_engine_save_config(wizard_engine_ctx_t *ctx) {
    if (!ctx) return -1;
    /* FF[src/config/config_saver.c]-CFN[config_saver_save]-FTF[保存配置到文件] */
    return config_saver_save(&ctx->config);
}

/* ============================================================
 * FTF[设置强制跳过验证]
 * ============================================================ */
void wizard_engine_set_force_skip(wizard_engine_ctx_t *ctx, int enable) {
    if (ctx) {
        ctx->force_skip_verify = enable;
        LOG_INFO_T("WizardEngine", "ForceSkip", "Set", "force_skip_verify=%d", enable);
    }
}