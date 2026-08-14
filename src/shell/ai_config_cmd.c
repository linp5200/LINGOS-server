/**
 * @file    ai_config_cmd.c
 * @brief   AI 配置交互命令（支持 Ollama/DeepSeek，含子AI独立配置）
 * @version 2.1.0.0
 */

#include "ai_config_cmd.h"
#include "lang.h"
#include "uart.h"
#include "log_extra.h"
#include "ai_config.h"
#include "safe_string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int read_line(char *buf, size_t size) {
    if (!buf || size == 0) return 0;
    int idx = 0;
    while (1) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') {
            uart_puts("\r\n");
            buf[idx] = '\0';
            break;
        } else if (c == '\b' || c == 127) {
            /* 【修复】UTF-8 感知退格 */
            safe_backspace_echo(buf, &idx);
        } else if (c == 0x03) {   /* ^C：清空输入行 */
            uart_puts("\r\033[K");
            uart_puts(tr(": ", ": "));
            idx = 0;
            buf[0] = '\0';
        } else if (c == 0x15) {   /* ^U：删除整行 */
            while (idx > 0) {
                safe_backspace_echo(buf, &idx);
            }
        } else if (idx < (int)size - 1) {
            buf[idx++] = c;
            uart_putc(c);
        }
    }
    return idx;
}

static void read_string(const char *prompt_en, const char *prompt_zh, char *buf, size_t size, const char *default_val) {
    uart_puts(tr(prompt_en, prompt_zh));
    if (default_val && default_val[0]) {
        uart_puts(tr(" [", " ["));
        uart_puts(default_val);
        uart_puts("]");
    }
    uart_puts(tr(": ", ": "));
    read_line(buf, size);
    if (buf[0] == '\0' && default_val) {
        strncpy(buf, default_val, size - 1);
        buf[size - 1] = '\0';
    }
}

static void read_password(const char *prompt_en, const char *prompt_zh, char *buf, size_t size) {
    uart_puts(tr(prompt_en, prompt_zh));
    uart_puts(tr(": ", ": "));
    int idx = 0;
    while (1) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') {
            uart_puts("\r\n");
            buf[idx] = '\0';
            break;
        } else if (c == '\b' || c == 127) {
            /* 【修复】UTF-8 感知退格（密码输入同样处理） */
            safe_backspace_echo(buf, &idx);
        } else if (c == 0x03) {   /* ^C：清空输入行 */
            uart_puts("\r\033[K");
            uart_puts(tr(": ", ": "));
            idx = 0;
            buf[0] = '\0';
        } else if (c == 0x15) {   /* ^U：删除整行 */
            while (idx > 0) {
                safe_backspace_echo(buf, &idx);
            }
        } else if (idx < (int)size - 1) {
            buf[idx++] = c;
            uart_putc('*');
        }
    }
}

static int select_backend(void) {
    uart_puts(tr("\nSelect AI backend:\n", "\n选择 AI 后端：\n"));
    uart_puts(tr("  1. Ollama (local)\n", "  1. Ollama（本地）\n"));
    uart_puts(tr("  2. DeepSeek (cloud)\n", "  2. DeepSeek（云端）\n"));
    uart_puts(tr("Enter choice [1-2]: ", "输入选项 [1-2]: "));
    char c = uart_getc();
    uart_putc(c);
    uart_puts("\n");
    if (c == '2') return 1;
    return 0;
}

static int select_deepseek_model(char *model_buf, size_t buf_size) {
    const char *models[] = {
        "deepseek-v4-pro",
        "deepseek-v4-flash"
    };
    int count = sizeof(models) / sizeof(models[0]);
    uart_puts(tr("\nSelect DeepSeek model:\n", "\n选择 DeepSeek 模型：\n"));
    for (int i = 0; i < count; i++) {
        char line[64];
        snprintf(line, sizeof(line), "  %d. %s\n", i + 1, models[i]);
        uart_puts(line);
    }
    uart_puts(tr("Enter choice [1-", "输入选项 [1-"));
    char count_str[8];
    snprintf(count_str, sizeof(count_str), "%d", count);
    uart_puts(count_str);
    uart_puts(tr("]: ", "]: "));
    char c = uart_getc();
    uart_putc(c);
    uart_puts("\n");
    int choice = c - '0';
    if (choice < 1 || choice > count) {
        uart_puts(tr("Invalid choice, using default.\n", "无效选项，使用默认。\n"));
        strncpy(model_buf, models[0], buf_size - 1);
        return 0;
    }
    strncpy(model_buf, models[choice - 1], buf_size - 1);
    model_buf[buf_size - 1] = '\0';
    return 0;
}

static int select_reasoning_effort(char *effort_buf, size_t buf_size) {
    uart_puts(tr("\nReasoning effort (controls thinking depth):\n", "\n推理强度（控制思考深度）：\n"));
    uart_puts(tr("  1. high (balanced, default)\n", "  1. high（平衡，默认）\n"));
    uart_puts(tr("  2. max (maximum reasoning quality)\n", "  2. max（最大化推理质量）\n"));
    uart_puts(tr("Enter choice [1-2]: ", "输入选项 [1-2]: "));
    char c = uart_getc();
    uart_putc(c);
    uart_puts("\n");
    if (c == '2') {
        strncpy(effort_buf, "max", buf_size - 1);
    } else {
        strncpy(effort_buf, "high", buf_size - 1);
    }
    return 0;
}

static int select_thinking_mode(void) {
    uart_puts(tr("\nEnable thinking mode (reasoning content visible in logs)?\n", "\n启用思考模式（日志中显示推理内容）？\n"));
    uart_puts(tr("  1. Yes (default)\n", "  1. 是（默认）\n"));
    uart_puts(tr("  2. No\n", "  2. 否\n"));
    uart_puts(tr("Enter choice [1-2]: ", "输入选项 [1-2]: "));
    char c = uart_getc();
    uart_putc(c);
    uart_puts("\n");
    return (c == '2') ? 0 : 1;
}

void ai_config_interactive(void) {
    LOG_INFO_T("AIConfigCmd", "Interactive", "Start", "user initiated AI configuration");
    uart_puts(tr("\n========== AI Configuration ==========\n", "\n========== AI 配置 ==========\n"));

    /* 【批次A】展示当前配置，允许跳过/使用当前 */
    const ai_config_t *cur = ai_config_get();
    if (cur) {
        char cur_line[256];
        safe_snprintf(cur_line, sizeof(cur_line),
                      tr("  Backend: %s | Language: %s | Temp: %.2f | Creativity: %.2f | Agents: %d | Search: %s\n",
                         "  后端：%s | 语言：%s | 温度：%.2f | 创造性：%.2f | 子AI数：%d | 搜索：%s\n"),
                      cur->backend == AI_BACKEND_DEEPSEEK ? "deepseek" : "ollama",
                      cur->language[0] ? cur->language : "en",
                      cur->temperature, cur->creativity, cur->max_agents,
                      cur->search_backend[0] ? cur->search_backend : "searxng");
        uart_puts(tr("Current configuration:\n", "当前配置：\n"));
        uart_puts(cur_line);
        if (cur->personality_file[0]) {
            uart_puts(tr("  Personality: ", "  人格："));
            uart_puts(cur->personality_file);
            uart_puts("\n");
        }
        uart_puts(tr("Use current configuration? (Y/n): ", "使用当前配置？(Y/n): "));
        char c = uart_getc();
        uart_putc(c);
        uart_puts("\n");
        if (c == 'y' || c == 'Y' || c == '\n' || c == '\r') {
            uart_puts(tr("✅ Using current configuration.\n", "✅ 使用当前配置。\n"));
            LOG_INFO_T("AIConfigCmd", "Interactive", "KeepCurrent", "user kept current config");
            return;
        }
        uart_puts(tr("Proceeding with reconfiguration.\n", "开始重新配置。\n"));
    }

    int backend_choice = select_backend();
    ai_backend_t new_backend = (backend_choice == 1) ? AI_BACKEND_DEEPSEEK : AI_BACKEND_OLLAMA;

    ai_config_load();
    const ai_config_t *old_cfg = ai_config_get();

    if (new_backend == AI_BACKEND_OLLAMA) {
        uart_puts(tr("\n--- Ollama Configuration ---\n", "\n--- Ollama 配置 ---\n"));
        char url[256];
        char model[64];
        read_string("Ollama URL", "Ollama URL", url, sizeof(url), old_cfg->ollama_url);
        read_string("Ollama model (e.g., llama3, qwen2, glm-4.6:cloud)",
                    "Ollama 模型（例如 llama3, qwen2, glm-4.6:cloud）",
                    model, sizeof(model), old_cfg->ollama_model);
        ai_config_set_ollama_url(url);
        ai_config_set_ollama_model(model);
    } else if (new_backend == AI_BACKEND_DEEPSEEK) {
        uart_puts(tr("\n--- DeepSeek Configuration ---\n", "\n--- DeepSeek 配置 ---\n"));
        char api_key[256];
        read_password("DeepSeek API Key", "DeepSeek API Key", api_key, sizeof(api_key));
        if (strlen(api_key) == 0 && old_cfg->deepseek_api_key[0] != '\0') {
            strncpy(api_key, old_cfg->deepseek_api_key, sizeof(api_key)-1);
        }
        ai_config_set_deepseek_api_key(api_key);

        char model[64];
        select_deepseek_model(model, sizeof(model));
        ai_config_set_deepseek_model(model);

        char base_url[256];
        read_string("Base URL (optional)", "Base URL（可选）", base_url, sizeof(base_url), old_cfg->deepseek_base_url);
        if (strlen(base_url) > 0) ai_config_set_deepseek_base_url(base_url);

        char effort[16];
        select_reasoning_effort(effort, sizeof(effort));
        ai_config_set_deepseek_reasoning_effort(effort);

        int thinking = select_thinking_mode();
        ai_config_set_thinking_enabled(thinking);

        // ========== 新增：子AI独立配置 ==========
        uart_puts(tr("\n--- Sub-AI Configuration (optional) ---\n", "\n--- 子AI配置（可选） ---\n"));
        uart_puts(tr("Do you want to configure a dedicated API key for sub-AI?\n", "是否要为子AI配置独立的 API Key？\n"));
        uart_puts(tr("If not, the sub-AI will use the main DeepSeek configuration.\n", "若不配置，子AI将使用主 DeepSeek 配置。\n"));
        uart_puts(tr("Configure sub-AI? (y/N): ", "配置子AI？(y/N): "));
        char c = uart_getc();
        uart_putc(c);
        uart_puts("\n");
        if (c == 'y' || c == 'Y') {
            char sub_api_key[256];
            read_password("Sub-AI API Key (optional, leave empty to use main key)",
                          "子AI API Key（可选，留空则使用主Key）",
                          sub_api_key, sizeof(sub_api_key));
            ai_config_set_sub_ai_api_key(sub_api_key);

            char sub_model[64];
            read_string("Sub-AI model (optional, default: deepseek-v4-pro)",
                        "子AI 模型（可选，默认 deepseek-v4-pro）",
                        sub_model, sizeof(sub_model), old_cfg->sub_ai_model[0] ? old_cfg->sub_ai_model : "deepseek-v4-pro");
            ai_config_set_sub_ai_model(sub_model);

            char sub_base_url[256];
            read_string("Sub-AI Base URL (optional, default: https://api.deepseek.com)",
                        "子AI Base URL（可选，默认 https://api.deepseek.com）",
                        sub_base_url, sizeof(sub_base_url), old_cfg->sub_ai_base_url[0] ? old_cfg->sub_ai_base_url : "https://api.deepseek.com");
            ai_config_set_sub_ai_base_url(sub_base_url);
        } else {
            // 用户选择不配置，清空子AI的API Key，使其回退到主配置
            ai_config_set_sub_ai_api_key("");
            // 模型和URL保持默认或已有值
        }
        // ===========================================
    }

    char user_id[64];
    read_string("User ID (for API identification)", "用户 ID（用于 API 标识）", user_id, sizeof(user_id), old_cfg->user_id);
    ai_config_set_user_id(user_id);

    ai_config_set_backend(new_backend);

    /* ========== 【批次A】AI 高级配置 ========== */
    uart_puts(tr("\n--- Advanced AI Configuration ---\n", "\n--- AI 高级配置 ---\n"));
    uart_puts(tr("(Leave empty to keep current value)\n", "（留空保持当前值）\n"));

    /* 温度 */
    char temp_buf[16];
    read_string("Temperature (0-2)", "温度 (0-2)", temp_buf, sizeof(temp_buf), NULL);
    if (temp_buf[0]) {
        double t = atof(temp_buf);
        if (t >= 0.0 && t <= 2.0) ai_config_set_temperature(t);
        else uart_puts(tr("  Invalid range, keeping current.\n", "  无效范围，保持当前。\n"));
    }

    /* 创造性 */
    char creat_buf[16];
    read_string("Creativity (0-1)", "创造性 (0-1)", creat_buf, sizeof(creat_buf), NULL);
    if (creat_buf[0]) {
        double c = atof(creat_buf);
        if (c >= 0.0 && c <= 1.0) ai_config_set_creativity(c);
        else uart_puts(tr("  Invalid range, keeping current.\n", "  无效范围，保持当前。\n"));
    }

    /* 可调用子AI数 */
    char agents_buf[16];
    read_string("Max sub-AI agents (1-8)", "可调用子AI数 (1-8)", agents_buf, sizeof(agents_buf), NULL);
    if (agents_buf[0]) {
        int n = atoi(agents_buf);
        if (n >= 1 && n <= 8) ai_config_set_max_agents(n);
        else uart_puts(tr("  Invalid range, keeping current.\n", "  无效范围，保持当前。\n"));
    }

    /* 搜索后端 */
    uart_puts(tr("\nSearch backend:\n", "\n搜索后端：\n"));
    uart_puts(tr("  1. searxng (local aggregator, recommended)\n", "  1. searxng（本地聚合，推荐）\n"));
    uart_puts(tr("  2. html (direct parsing, fallback)\n", "  2. html（直接解析，降级）\n"));
    uart_puts(tr("Enter choice (1-2, empty to keep): ", "输入选择 (1-2，留空保持): "));
    char sc = uart_getc();
    if (sc == '1') ai_config_set_search_backend("searxng");
    else if (sc == '2') ai_config_set_search_backend("html");
    uart_puts("\n");

    /* 人格文件 */
    char pers_buf[256];
    read_string("Personality file (json/md/txt, empty to keep)",
                "人格文件 (json/md/txt，留空保持)",
                pers_buf, sizeof(pers_buf), NULL);
    if (pers_buf[0]) {
        if (strcmp(pers_buf, "-") == 0) ai_config_set_personality_file("");
        else ai_config_set_personality_file(pers_buf);
    }

    /* 助手提示词文件 */
    char asst_buf[256];
    read_string("Assistant prompt file (json/md/txt, empty to keep)",
                "助手提示词文件 (json/md/txt，留空保持)",
                asst_buf, sizeof(asst_buf), NULL);
    if (asst_buf[0]) {
        if (strcmp(asst_buf, "-") == 0) ai_config_set_assistant_file("");
        else ai_config_set_assistant_file(asst_buf);
    }

    /* 思考显示模式（三选项） */
    uart_puts(tr("\nThinking display mode:\n", "\n思考显示模式：\n"));
    uart_puts(tr("  1. off      - never show thinking\n", "  1. off      - 不显示思考\n"));
    uart_puts(tr("  2. hidden   - show then hide after finish\n", "  2. hidden   - 显示，结束后隐藏\n"));
    uart_puts(tr("  3. visible  - show and keep\n", "  3. visible  - 显示并保留\n"));
    uart_puts(tr("Enter choice (1-3, empty to keep): ", "输入选择 (1-3，留空保持): "));
    char tc = uart_getc();
    if (tc == '1') ai_config_set_thinking_display("off");
    else if (tc == '2') ai_config_set_thinking_display("hidden");
    else if (tc == '3') ai_config_set_thinking_display("visible");
    uart_puts("\n");

    /* 【批次A】持久化：缓存回写 config_core 并强制保存 */
    if (ai_config_save() == 0) {
        uart_puts(tr("✅ AI configuration saved.\n", "✅ AI 配置已保存。\n"));
    } else {
        uart_puts(tr("❌ Failed to save AI configuration.\n", "❌ AI 配置保存失败。\n"));
    }
    uart_puts(tr("You may need to restart LING OS for some changes to take effect.\n",
                 "部分更改可能需要重启 LING OS 才能生效。\n"));
    LOG_INFO_T("AIConfigCmd", "Interactive", "Done", "configuration saved, backend=%d", new_backend);
}