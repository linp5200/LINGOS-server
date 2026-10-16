/**
 * @file    registry_cmd.c
 * @brief   registry 命令实现（类目 ↔ 系统 ↔ 命令前缀 可见化）
 * @version LN-0.9.0
 *
 * 【2026-10-16 第 4 期收尾·类目与命令一致性】
 *   · registry list       按类目分组显示（类目/命令前缀取自 registry_builtin.c 单一事实源）
 *   · registry categories 类目 ↔ 系统 ↔ 命令前缀 总表（扩展系统诚实标注，未擅自归类）
 *   · registry check      真实一致性校验（可失败；含运行期注册缺失与命令面前缀归属）
 *   · registry show       补 类目/命令前缀/职责/状态/类型（原先缺状态与类型，文案与数据不符）
 *   审计修正：
 *     ① strtok_r(NULL, "", ...) 空分隔符（非预期用法）→ " "；
 *     ② localtime() 未判空即 strftime（空指针崩溃风险）→ 显式防护；
 *     ③ 空注册表提示不含诊断入口 → 指向 registry check（禁止静默空态）。
 */

#include "../registry/registry.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define MAX_ENTRY_DISPLAY 64
#define MAX_GROUPS 24
#define GROUP_NAME_LEN 32
#define CHECK_REPORT_SIZE 16384

/* ============================================================
 * 条目 → 系统名 / 类目 / 命令前缀（去掉 module:/component: 前缀）
 * ============================================================ */
static const char* entry_system_id(const registry_entry_t *e) {
    if (!e || !e->id[0]) return NULL;
    const char *colon = strchr(e->id, ':');
    return colon ? colon + 1 : e->id;
}

static const char* entry_category(const registry_entry_t *e) {
    const char *sid = entry_system_id(e);
    return sid ? registry_builtin_category_of(sid) : NULL;
}

static const char* type_name(registry_type_t t) {
    switch (t) {
        case REG_TYPE_MODULE:    return tr("module", "模块");
        case REG_TYPE_COMPONENT: return tr("component", "组件");
        case REG_TYPE_CONFIG:    return tr("config", "配置");
        case REG_TYPE_FEATURE:   return tr("feature", "特性");
        case REG_TYPE_SKILL:     return tr("skill", "技能");
        case REG_TYPE_PLUGIN:    return tr("plugin", "插件");
        case REG_TYPE_HOOK:      return tr("hook", "钩子");
        case REG_TYPE_SELFCHECK: return tr("selfcheck", "自检");
        default:                 return tr("unknown", "未知");
    }
}

static const char* status_name(registry_status_t s) {
    switch (s) {
        case REG_STATUS_ACTIVE:     return tr("active", "活跃");
        case REG_STATUS_INACTIVE:   return tr("inactive", "非活跃");
        case REG_STATUS_DEPRECATED: return tr("deprecated", "已弃用");
        case REG_STATUS_ERROR:      return tr("error", "错误");
        default:                    return tr("unknown", "未知");
    }
}

/* 打印单条（含命令前缀——类目文档 §三：命令前缀=系统名） */
static void print_entry_line(int idx, const registry_entry_t *e) {
    char buf[256];
    const char *sid = entry_system_id(e);
    const char *cat = entry_category(e);
    char prefix[64];
    if (sid && registry_builtin_prefix_known(sid)) {
        safe_snprintf(prefix, sizeof(prefix), "%s.*", sid);
    } else {
        safe_strncpy(prefix, "-", sizeof(prefix));
    }
    safe_snprintf(buf, sizeof(buf), "  %2d. %-22s %-12s [%s] %s %s\n",
                  idx,
                  e->id,
                  prefix,
                  status_name(e->status),
                  e->version[0] ? e->version : "-",
                  cat ? "" : tr("(uncategorized)", "（未归类）"));
    uart_puts(buf);
}

/* ============================================================
 * registry list [type] —— 按类目分组
 * ============================================================ */
static void cmd_registry_list(const char *type_filter) {
    registry_entry_t *entries[MAX_ENTRY_DISPLAY];
    int type = -1;

    if (type_filter && *type_filter) {
        if (strcmp(type_filter, "module") == 0) type = REG_TYPE_MODULE;
        else if (strcmp(type_filter, "component") == 0) type = REG_TYPE_COMPONENT;
        else if (strcmp(type_filter, "config") == 0) type = REG_TYPE_CONFIG;
        else if (strcmp(type_filter, "feature") == 0) type = REG_TYPE_FEATURE;
        else if (strcmp(type_filter, "skill") == 0) type = REG_TYPE_SKILL;
        else if (strcmp(type_filter, "plugin") == 0) type = REG_TYPE_PLUGIN;
        else if (strcmp(type_filter, "hook") == 0) type = REG_TYPE_HOOK;
        else if (strcmp(type_filter, "selfcheck") == 0) type = REG_TYPE_SELFCHECK;
        else {
            uart_puts(tr("Unknown registry type. Available: module, component, config, feature, skill, plugin, hook, selfcheck\n",
                         "未知的注册表类型。可用：module, component, config, feature, skill, plugin, hook, selfcheck\n"));
            return;
        }
    }

    int count = registry_list(type, entries, MAX_ENTRY_DISPLAY);

    if (count == 0) {
        /* 禁止静默空态：给出类型口径与诊断入口 */
        uart_puts(tr("No entries found in registry.\n", "注册表无匹配条目。\n"));
        if (type >= 0) {
            char buf[128];
            safe_snprintf(buf, sizeof(buf), tr("  (type filter: %s)\n", "  （类型过滤：%s）\n"),
                          type_filter ? type_filter : "-");
            uart_puts(buf);
        }
        uart_puts(tr("  Hint: run 'registry check' to diagnose missing builtin systems.\n",
                     "  提示：运行 registry check 诊断内置系统是否注册缺失。\n"));
        return;
    }

    uart_puts(tr("\n=== Registry Entries (grouped by category) ===\n",
                 "\n=== 注册表条目（按类目分组）===\n"));

    /* 收集类目（按出现顺序，避免额外内存） */
    char groups[MAX_GROUPS][GROUP_NAME_LEN];
    int gcount = 0;
    for (int i = 0; i < count; i++) {
        const char *cat = entry_category(entries[i]);
        if (!cat || !*cat) continue;
        int seen = 0;
        for (int g = 0; g < gcount; g++) {
            if (strcmp(groups[g], cat) == 0) { seen = 1; break; }
        }
        if (!seen && gcount < MAX_GROUPS) {
            safe_strncpy(groups[gcount], cat, GROUP_NAME_LEN);
            gcount++;
        }
    }

    int printed = 0;
    for (int g = 0; g < gcount; g++) {
        char head[96];
        int gcnt = 0;
        for (int i = 0; i < count; i++) {
            const char *cat = entry_category(entries[i]);
            if (cat && strcmp(cat, groups[g]) == 0) gcnt++;
        }
        safe_snprintf(head, sizeof(head), "[%s] (%d)\n", groups[g], gcnt);
        uart_puts(head);
        for (int i = 0; i < count; i++) {
            const char *cat = entry_category(entries[i]);
            if (cat && strcmp(cat, groups[g]) == 0) {
                print_entry_line(printed + 1, entries[i]);
                printed++;
            }
        }
    }

    /* 未归类条目（技能/插件/外部注册等） */
    int uncat = 0;
    for (int i = 0; i < count; i++) {
        if (!entry_category(entries[i])) uncat++;
    }
    if (uncat > 0) {
        char head[96];
        safe_snprintf(head, sizeof(head), "[%s] (%d)\n", tr("uncategorized", "未归类"), uncat);
        uart_puts(head);
        for (int i = 0; i < count; i++) {
            if (!entry_category(entries[i])) {
                print_entry_line(printed + 1, entries[i]);
                printed++;
            }
        }
    }

    char buf[128];
    safe_snprintf(buf, sizeof(buf), tr("\nTotal: %d entries\n", "\n总计：%d 个条目\n"), count);
    uart_puts(buf);
    uart_puts(tr("  Next: registry categories (category/system/prefix) | registry check (consistency)\n",
                 "  下一步：registry categories（类目/系统/前缀）| registry check（一致性校验）\n"));
}

/* ============================================================
 * registry categories —— 类目 ↔ 系统 ↔ 命令前缀（单一事实源输出）
 * ============================================================ */
static void cmd_registry_categories(void) {
    int n = registry_builtin_system_count();
    if (n <= 0) {
        uart_puts(tr("No builtin systems defined.\n", "未定义内置系统。\n"));
        return;
    }

    uart_puts(tr("\n=== Categories / Systems / Command prefixes ===\n",
                 "\n=== 类目 / 系统 / 命令前缀 ===\n"));

    char cats[MAX_GROUPS][GROUP_NAME_LEN];
    int cn = 0;
    for (int i = 0; i < n; i++) {
        const char *cat = NULL;
        registry_builtin_system_at(i, NULL, NULL, &cat, NULL, NULL);
        if (!cat) continue;
        int seen = 0;
        for (int g = 0; g < cn; g++) {
            if (strcmp(cats[g], cat) == 0) { seen = 1; break; }
        }
        if (!seen && cn < MAX_GROUPS) {
            safe_strncpy(cats[cn], cat, GROUP_NAME_LEN);
            cn++;
        }
    }

    for (int g = 0; g < cn; g++) {
        int cnt = 0;
        for (int i = 0; i < n; i++) {
            const char *cat = NULL;
            registry_builtin_system_at(i, NULL, NULL, &cat, NULL, NULL);
            if (cat && strcmp(cat, cats[g]) == 0) cnt++;
        }
        char head[128];
        safe_snprintf(head, sizeof(head), "\n[%s] %s %d\n", cats[g], tr("systems", "系统"), cnt);
        uart_puts(head);
        for (int i = 0; i < n; i++) {
            const char *id = NULL, *name = NULL, *cat = NULL, *prefix = NULL, *desc = NULL;
            registry_builtin_system_at(i, &id, &name, &cat, &prefix, &desc);
            if (!cat || strcmp(cat, cats[g]) != 0) continue;
            char line[320];
            safe_snprintf(line, sizeof(line), "  %-12s %-14s %-12s %s\n",
                          id ? id : "?",
                          name ? name : "?",
                          prefix ? prefix : "?",
                          (desc && *desc) ? desc : "");
            uart_puts(line);
        }
    }

    /* 扩展系统诚实标注（不擅自归入文档 8 类目） */
    int ext = 0;
    for (int i = 0; i < n; i++) {
        const char *cat = NULL;
        registry_builtin_system_at(i, NULL, NULL, &cat, NULL, NULL);
        if (cat && strcmp(cat, "扩展") == 0) ext++;
    }
    char tail[256];
    safe_snprintf(tail, sizeof(tail),
                  tr("\nTotal: %d systems (%d from docs/ARCHITECTURE_系统类目.md + %d extensions).\n"
                     "Note: '扩展' systems are registered beyond the category doc (not re-classified here);\n"
                     "      ask the owner to extend the doc before assigning a category.\n",
                     "\n总计：%d 个系统（类目文档定义 16 + 扩展 %d）。\n"
                     "说明：「扩展」为类目文档之外已登记系统，未擅自归类；\n"
                     "      待先生补充类目文档后再归类。\n"),
                  n, 16, ext);
    uart_puts(tail);
}

/* ============================================================
 * registry check —— 一致性校验（真实可失败）
 * ============================================================ */
static void cmd_registry_check(void) {
    char *report = malloc(CHECK_REPORT_SIZE);
    if (!report) {
        uart_puts(tr("registry check: out of memory\n", "registry check：内存不足\n"));
        return;
    }
    int violations = registry_builtin_consistency_report(report, CHECK_REPORT_SIZE);
    if (violations < 0) {
        uart_puts(tr("registry check failed (report error)\n", "registry check 失败（报告生成错误）\n"));
        free(report);
        return;
    }
    uart_puts(report);
    if (strlen(report) >= CHECK_REPORT_SIZE - 1) {
        uart_puts(tr("(report truncated — increase CHECK_REPORT_SIZE)\n",
                     "（报告被截断——请增大 CHECK_REPORT_SIZE）\n"));
    }
    if (violations == 0) {
        uart_puts(COLOR_GREEN);
        uart_puts(tr("OK: category/system/command-prefix consistency passed.\n",
                     "通过：类目/系统/命令前缀一致性校验全部通过。\n"));
        uart_puts(COLOR_RESET);
    } else {
        uart_puts(COLOR_YELLOW);
        char buf[160];
        safe_snprintf(buf, sizeof(buf),
                      tr("WARN: %d violation(s) above — see registry list / categories.\n",
                         "警告：上方共 %d 项违规——见 registry list / registry categories。\n"),
                      violations);
        uart_puts(buf);
        uart_puts(COLOR_RESET);
    }
    free(report);
}

/* ============================================================
 * registry show <id>
 * ============================================================ */
static void cmd_registry_show(const char *id) {
    if (!id || !*id) {
        uart_puts(tr("Usage: registry show <id>\n", "用法：registry show <ID>\n"));
        uart_puts(tr("  Hint: ids look like module:core / component:alert\n",
                     "  提示：ID 形如 module:core / component:alert\n"));
        return;
    }

    const registry_entry_t *entry = registry_get(id);
    if (!entry) {
        uart_puts(tr("Entry not found.\n", "未找到该条目。\n"));
        uart_puts(tr("  Hint: registry list — then copy the exact id.\n",
                     "  提示：先 registry list，再复制完整 ID。\n"));
        return;
    }

    uart_puts(tr("\n=== Registry Entry ===\n", "\n=== 注册表条目详情 ===\n"));

    const char *sid = entry_system_id(entry);
    const char *cat = entry_category(entry);
    char buf[384];
    safe_snprintf(buf, sizeof(buf),
                  "  ID: %s\n"
                  "  Name: %s\n"
                  "  Type: %s\n"
                  "  Status: %s\n"
                  "  Version: %s\n"
                  "  Path: %s\n",
                  entry->id,
                  entry->name,
                  type_name(entry->type),
                  status_name(entry->status),
                  entry->version,
                  entry->path[0] ? entry->path : "-");
    uart_puts(buf);

    /* 【2026-10-16】类目/命令前缀（类目文档 §三：命令前缀=系统名） */
    safe_snprintf(buf, sizeof(buf),
                  tr("  Category: %s\n  Command prefix: %s\n",
                     "  类目：%s\n  命令前缀：%s\n"),
                  (cat && *cat) ? cat : tr("(none)", "（无）"),
                  (sid && registry_builtin_prefix_known(sid)) ? sid : tr("(none)", "（无）"));
    uart_puts(buf);

    /* 约定时间显示：localtime 可能返回 NULL（审计修正：原先直接传入 strftime） */
    char time_buf[40];
    struct tm *tm = localtime(&entry->created_at);
    if (tm) {
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);
    } else {
        safe_strncpy(time_buf, tr("(invalid time)", "（时间无效）"), sizeof(time_buf));
    }
    safe_snprintf(buf, sizeof(buf), tr("  Created: %s\n", "  创建：%s\n"), time_buf);
    uart_puts(buf);

    tm = localtime(&entry->updated_at);
    if (tm) {
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);
    } else {
        safe_strncpy(time_buf, tr("(invalid time)", "（时间无效）"), sizeof(time_buf));
    }
    safe_snprintf(buf, sizeof(buf), tr("  Updated: %s\n", "  更新：%s\n"), time_buf);
    uart_puts(buf);

    if (entry->metadata) {
        uart_puts(tr("  Metadata: present (category/command_prefix/responsibility for builtin systems)\n",
                     "  元数据：存在（内置系统含 类目/命令前缀/职责）\n"));
    }
}

/* ============================================================
 * registry reload
 * ============================================================ */
static void cmd_registry_reload(void) {
    uart_puts(tr("Reloading registry...\n", "正在重载注册表...\n"));
    int ret = registry_reload();
    if (ret == 0) {
        uart_puts(tr("Registry reloaded successfully.\n", "注册表重载成功。\n"));
    } else {
        /* 诚实失败：给出可能原因，不伪装成功 */
        uart_puts(tr("Failed to reload registry (file missing or invalid JSON).\n",
                     "重载注册表失败（文件缺失或 JSON 无效）。\n"));
    }
}

/* ============================================================
 * 主分发函数
 * ============================================================ */
void registry_dispatch(const char *args) {
    LOG_DEBUG_T("RegistryCmd", "Dispatch", "Enter", "args='%s'", args ? args : "(null)");

    if (!args || !*args) {
        uart_puts(tr("Usage: registry <subcommand> [args]\n", "用法：registry <子命令> [参数]\n"));
        uart_puts(tr("Subcommands:\n", "子命令：\n"));
        uart_puts(tr("  list [type]    - List entries grouped by category (optional type filter)\n",
                     "  list [类型]    - 按类目分组列出条目（可选类型过滤）\n"));
        uart_puts(tr("  categories     - Categories / systems / command prefixes\n",
                     "  categories     - 类目 / 系统 / 命令前缀\n"));
        uart_puts(tr("  check          - Consistency check (prefix==system, registration, command surface)\n",
                     "  check          - 一致性校验（前缀=系统名/注册完整性/命令面）\n"));
        uart_puts(tr("  show <id>      - Show entry details\n",
                     "  show <ID>      - 显示条目详情\n"));
        uart_puts(tr("  reload         - Hot reload registry\n",
                     "  reload         - 热重载注册表\n"));
        return;
    }

    char cmd_buf[256];
    safe_strncpy(cmd_buf, args, sizeof(cmd_buf));
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    char *saveptr = NULL;
    /* 【2026-10-16 审计修正】原第二参数分隔符为 ""（空串，非预期用法）→ 统一 " " */
    char *subcmd = strtok_r(cmd_buf, " ", &saveptr);
    char *param = strtok_r(NULL, " ", &saveptr);

    if (!subcmd) {
        uart_puts(tr("Missing subcommand.\n", "缺少子命令。\n"));
        return;
    }

    if (strcmp(subcmd, "list") == 0) {
        cmd_registry_list(param);
        return;
    }

    if (strcmp(subcmd, "categories") == 0 || strcmp(subcmd, "cat") == 0) {
        cmd_registry_categories();
        return;
    }

    if (strcmp(subcmd, "check") == 0 || strcmp(subcmd, "verify") == 0) {
        cmd_registry_check();
        return;
    }

    if (strcmp(subcmd, "show") == 0) {
        if (param) {
            while (*param == ' ') param++;
            cmd_registry_show(param);
        } else {
            uart_puts(tr("Usage: registry show <id>\n", "用法：registry show <ID>\n"));
        }
        return;
    }

    if (strcmp(subcmd, "reload") == 0) {
        cmd_registry_reload();
        return;
    }

    /* 未知子命令：指出可用项（不静默失败） */
    uart_puts(tr("Unknown registry subcommand.\n", "未知的 registry 子命令。\n"));
    uart_puts(tr("Available: list [type], categories, check, show <id>, reload\n",
                 "可用：list [类型], categories, check, show <ID>, reload\n"));
}
