/**
 * @file    src/shell/dotted_cmd.c
 * @brief   点分命令（系统.动作）别名层 —— 先生决策 5【第 4 期收尾】
 * @version LN-0.8.0
 *
 * 接线点（谁调用它）：
 *   · src/shell/shell.c:handle_builtin_command() 入口调用 dotted_cmd_translate()
 *   · src/shell/shell.c:show_help_topic("dotted") 调用 dotted_cmd_list()
 *
 * 铁律遵守：
 *   · 只增不减——旧命令（空格写法）全部保留，翻译后走同一实现；
 *   · 表中每条 legacy 命令都对应代码中真实存在的分发分支（注释给出 文件:行）；
 *   · 未收录的点分命令原样透传（不报假成功、不吞命令）。
 */

#include "dotted_cmd.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <string.h>

/* 点分规范名 → 旧命令（保留别名）；legacy 均为已验证存在的真实分发点 */
typedef struct {
    const char *dotted;      /* 新：点分规范名 */
    const char *legacy;      /* 旧：空格命令（永久保留） */
    const char *desc_en;
    const char *desc_zh;
} dotted_alias_t;

static const dotted_alias_t g_dotted[] = {
    /* —— 系统域（src/shell/shell.c 既有分支） —— */
    {"system.status",   "system status",   "system status",              "系统状态"},
    {"system.health",   "system health",   "system health check",        "系统健康检查"},
    {"system.update",   "system update",   "install system update",      "安装系统更新"},
    {"system.rollback", "system rollback", "rollback system update",     "回滚系统更新"},
    {"system.privilege","system privilege","privilege mode status",      "权限模式状态"},
    {"system.security", "system security", "security switches",          "安全开关"},
    {"system.defense",  "system defense",  "defense mode control",       "防御模式控制"},
    /* —— 配置域（src/shell/config/config_cmd.c:237-242） —— */
    {"config.reload",   "config reload",   "reload config category",     "重载配置分类"},
    {"config.get",      "config get",      "get config item",            "读取配置项"},
    {"config.set",      "config set",      "set config item",            "写入配置项"},
    {"config.list",     "config list",     "list config items",          "列出配置项"},
    /* —— 权限/安全（真实路径：shell.c 动词层 list permission → ai permission_list） —— */
    {"security.perm_list", "list permission", "list permissions",        "列出权限"},
    {"security.privilege_status", "system privilege status", "privilege mode status", "权限模式状态"},
    /* —— 技能（本地技能商店两级呈现：分类浏览 + 关键词搜索） —— */
    {"skill.list",      "skill browse",    "skills by category",         "技能分类浏览"},
    {"skill.search",    "skill search",    "search skills by keyword",   "技能关键词搜索"},
    /* —— 注册表 / 规则 / 快照 / 插件 / 提醒（各自 dispatch 已验证） —— */
    {"registry.list",   "registry list",   "list registry entries",      "列出注册表条目"},
    {"registry.show",   "registry show",   "show registry entry",        "查看注册表条目"},
    {"registry.reload", "registry reload", "reload registry",            "重载注册表"},
    {"rules.list",      "rules list",      "list rules",                 "列出规则"},
    {"snapshot.list",   "snapshot list",   "list snapshots",             "列出快照"},
    {"snapshot.create", "snapshot create", "create snapshot",            "创建快照"},
    {"plugin.list",     "plugin list",     "list plugins",               "列出插件"},
    {"reminder.list",   "remind list",     "list reminders",             "列出提醒"},
    /* —— 日志 / 健康 / AI（src/shell/shell.c 既有分支） —— */
    {"log.level",       "log level",       "set log level",              "设置日志等级"},
    {"health.trend",    "health trend",    "health trend analysis",      "健康趋势分析"},
    {"health.watchdog", "health watchdog", "health watchdog on|off|status", "健康看门狗"},
    {"ai.status",       "ai status",       "AI backend status",          "AI 后端状态"},
    /* —— 统一控制入口（Control URL 本地入口，见 src/api/control_url.c） —— */
    {"control.run",     "control",         "run Control URL / action",   "执行 Control URL"},
    {NULL, NULL, NULL, NULL}
};

int dotted_cmd_count(void) {
    int n = 0;
    while (g_dotted[n].dotted) n++;
    return n;
}

/* 取首 token 长度（空格/制表符分隔） */
static size_t first_token_len(const char *s) {
    size_t n = 0;
    while (s[n] && s[n] != ' ' && s[n] != '\t') n++;
    return n;
}

int dotted_cmd_translate(const char *input, char *out, size_t out_size) {
    if (!input || !out || out_size == 0) return 0;

    size_t tok = first_token_len(input);
    if (tok == 0 || tok >= 128) return 0;
    /* 无点号 → 不是点分命令，交回既有分发链 */
    if (memchr(input, '.', tok) == NULL) return 0;

    char token[128];
    memcpy(token, input, tok);
    token[tok] = '\0';

    for (int i = 0; g_dotted[i].dotted; i++) {
        if (strcmp(token, g_dotted[i].dotted) != 0) continue;
        /* 命中：旧命令 + 原样保留的剩余参数 */
        int n = safe_snprintf(out, out_size, "%s%s", g_dotted[i].legacy, input + tok);
        if (n < 0 || (size_t)n >= out_size) {
            LOG_WARN_T("DottedCmd", "Translate", "Truncated",
                       "dotted=%s -> '%s' truncated (out_size=%zu)", token, g_dotted[i].legacy, out_size);
            return 0;   /* 缓冲不足 → 不翻译（安全降级，交给旧链原样处理） */
        }
        LOG_DEBUG_T("DottedCmd", "Translate", "OK", "%s -> %s", token, out);
        return 1;
    }

    /* 未收录的点分命令：原样透传（不吞命令、不假成功） */
    LOG_DEBUG_T("DottedCmd", "Translate", "Unmapped", "token=%s (passthrough)", token);
    return 0;
}

void dotted_cmd_list(void) {
    uart_puts(tr("Dotted commands (new canonical form; legacy alias stays):\n",
                 "点分命令（新规范名；旧命令别名永久保留）：\n"));
    for (int i = 0; g_dotted[i].dotted; i++) {
        uart_puts("  ");
        uart_puts(g_dotted[i].dotted);
        uart_puts("  =  ");
        uart_puts(g_dotted[i].legacy);
        uart_puts(tr("   - ", "   - "));
        uart_puts(tr(g_dotted[i].desc_en, g_dotted[i].desc_zh));
        uart_puts("\n");
    }
    char cnt[64];
    safe_snprintf(cnt, sizeof(cnt), "%s: %d\n",
                  tr("Total dotted commands", "点分命令总数"), dotted_cmd_count());
    uart_puts(cnt);
    uart_puts(tr("Unknown dotted names pass through unchanged.\n",
                 "未收录的点分命令原样透传。\n"));
}
