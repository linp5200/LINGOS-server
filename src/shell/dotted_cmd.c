/**
 * @file    src/shell/dotted_cmd.c
 * @brief   点分命令（系统.动作）别名层 —— 先生决策 5【第 4 期收尾】
 * @version LN-0.9.0
 *
 * 接线点（谁调用它）：
 *   · src/shell/shell.c:handle_builtin_command()   → dotted_cmd_translate()
 *   · src/shell/shell.c:show_help_topic("dotted")  → dotted_cmd_list()
 *   · src/shell/shell.c:show_help_topic("systems") → dotted_cmd_list_systems()
 *
 * 铁律遵守：
 *   · 只增不减——旧命令（空格写法）全部保留，翻译后走同一实现；
 *   · 表中每条 legacy 命令都对应代码中真实存在的分发分支（注释标注）；
 *   · 未收录的点分命令原样透传（不报假成功、不吞命令）；
 *   · 【2026-10-16】类目/系统/命令前缀一致性：前缀取自 registry_builtin.c
 *     单一事实源；无 CLI 命令面的系统如实标注为缺口，不编造命令。
 */

#include "dotted_cmd.h"
#include "../registry/registry.h"   /* 类目/系统/命令前缀 单一事实源 */
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
    /* —— core 核心系统（类目文档前缀 core.*） —— */
    {"core.status",             "system status",        "system status",              "系统状态"},
    {"core.health",             "system health",        "system health check",        "系统健康检查"},
    {"core.dashboard",          "dashboard",            "dashboard / status",         "仪表板"},
    {"core.log_level",          "log level",            "set log level",              "设置日志等级"},
    {"core.logdump",            "logdump",              "dump logs",                  "导出日志"},
    {"core.debug",              "debug",                "debug commands",             "调试命令"},
    {"core.registry_list",      "registry list",        "list registry entries",      "列出注册表条目"},
    {"core.registry_show",      "registry show",        "show registry entry",        "查看注册表条目"},
    {"core.registry_reload",    "registry reload",      "reload registry",            "重载注册表"},
    {"core.registry_categories","registry categories",  "category/system/prefix table", "类目/系统/前缀总表"},
    {"core.registry_check",     "registry check",       "consistency check",          "一致性校验"},
    {"core.control",            "control",              "run Control URL / action",   "执行 Control URL"},

    /* —— 旧命令组保留（system.* 历史前缀；兼容铁律不删） —— */
    {"system.status",   "system status",   "system status",              "系统状态"},
    {"system.health",   "system health",   "system health check",        "系统健康检查"},
    {"system.update",   "system update",   "install system update",      "安装系统更新"},
    {"system.rollback", "system rollback", "rollback system update",     "回滚系统更新"},
    {"system.privilege","system privilege","privilege mode status",      "权限模式状态"},
    {"system.security", "system security", "security switches",          "安全开关"},
    {"system.defense",  "system defense",  "defense mode control",       "防御模式控制"},

    /* —— comm 通讯系统 —— */
    {"comm.port_list",   "port list",     "show ports (ws/http/tcp)",   "查看端口"},
    {"comm.port_set",    "port set",      "set port value",             "设置端口"},
    {"comm.server_mode", "server mode",   "server mode on/off/stop/status", "服务器模式"},

    /* —— voice 语音系统 —— */
    {"voice.usage", "voice usage", "voice usage stats", "语音用量"},
    {"voice.query", "voice query", "query voice usage", "查询语音用量"},

    /* —— config 配置系统（src/shell/config/config_cmd.c:237-242） —— */
    {"config.reload",   "config reload",   "reload config category",     "重载配置分类"},
    {"config.get",      "config get",      "get config item",            "读取配置项"},
    {"config.set",      "config set",      "set config item",            "写入配置项"},
    {"config.list",     "config list",     "list config items",          "列出配置项"},
    {"config.advanced", "config advanced", "advanced config keys",       "高级配置键"},

    /* —— data 数据系统（会话/记忆/模型数据） —— */
    {"data.memory_search", "memory search", "search memory",             "记忆检索"},
    {"data.memory_write",  "memory write",  "write memory",              "写入记忆"},
    {"data.memory_list",   "list memory",   "list memories",             "列出记忆"},
    {"data.session_list",  "list session",  "list sessions",             "列出会话"},
    {"data.model_list",    "list model",    "list models/providers",     "列出模型"},

    /* —— plugin 插件系统（src/shell/cmd_plugin.c:166-182） —— */
    {"plugin.list",   "plugin list",   "list plugins",    "列出插件"},
    {"plugin.load",   "plugin load",   "load plugin",     "加载插件"},
    {"plugin.unload", "plugin unload", "unload plugin",   "卸载插件"},

    /* —— security 安全系统 —— */
    {"security.perm_list",        "list permission",       "list permissions",   "列出权限"},
    {"security.privilege_status", "system privilege status","privilege mode status", "权限模式状态"},
    {"security.status",           "system security status","security switches",  "安全开关"},
    {"security.input",            "system security input", "input filter mode",  "输入过滤模式"},
    {"security.audit",            "audit",                 "audit log",          "审计日志"},
    {"security.behavior",         "system behavior",       "behavior monitoring","行为监控"},

    /* —— home 家居系统 —— */
    {"home.status", "ha status", "home assistant status", "家居状态"},
    {"home.states", "ha states", "home assistant states", "家居实体状态"},

    /* —— alert 预警系统（生命线；2026-10-16 接线 shell alert 命令） —— */
    {"alert.status",  "alert status",  "current alerts",       "当前预警"},
    {"alert.config",  "alert config",  "alert configuration",  "预警配置"},
    {"alert.history", "alert history", "alert history",        "预警历史"},

    /* —— ai AI 系统 —— */
    {"ai.status",        "ai status",     "AI backend status",     "AI 后端状态"},
    {"ai.config",        "ai config",     "AI config wizard",      "AI 配置向导"},
    {"ai.nook_status",   "nook status",   "nook status",           "Nook 状态"},
    {"ai.nook_ask",      "nook ask",      "ask nook (AI)",         "询问 Nook"},
    {"ai.repair",        "nook repair",   "nook repair",           "Nook 修复"},
    {"ai.provider_list", "provider list", "list providers",        "列出模型提供方"},

    /* —— skill 技能系统（两级：分类浏览 + 关键词搜索） —— */
    {"skill.list",      "skill browse",     "skills by category",     "技能分类浏览"},
    {"skill.browse",    "skill browse",     "skills by category",     "技能分类浏览"},
    {"skill.search",    "skill search",     "search skills by keyword","技能关键词搜索"},
    {"skill.install",   "skill install",    "install skill",          "安装技能"},
    {"skill.enable",    "skill enable",     "enable skill",           "启用技能"},
    {"skill.disable",   "skill disable",    "disable skill",          "禁用技能"},
    {"skill.uninstall", "skill uninstall",  "uninstall skill",        "卸载技能"},

    /* —— 扩展系统（类目文档之外，见 registry_builtin.c 尾注） —— */
    {"rules.list",   "rules list",   "list rules",            "列出规则"},
    {"rules.add",    "rules add",    "add rule",              "添加规则"},
    {"rules.toggle", "rules toggle", "enable/disable rule",   "启用/停用规则"},
    {"update.install",  "system update",   "install update",   "安装更新"},
    {"update.rollback", "system rollback", "rollback update",  "回滚更新"},
    {"install.app",   "app install",  "install app package",  "安装应用包"},
    {"install.skill", "skill install","install skill",        "安装技能"},
    {"repair.run",    "nook repair",  "run repair",           "执行修复"},
    {"memory.search", "memory search","search memory",        "记忆检索"},
    {"memory.write",  "memory write", "write memory",         "写入记忆"},
    {"memory.list",   "list memory",  "list memories",        "列出记忆"},
    {"ui.desktop",    "desktop",      "desktop UI",           "桌面界面"},

    /* —— 旧命令组保留（log/health/snapshot/remind/registry/control） —— */
    {"log.level",       "log level",       "set log level",           "设置日志等级"},
    {"health.trend",    "health trend",    "health trend analysis",   "健康趋势分析"},
    {"health.watchdog", "health watchdog", "health watchdog on|off|status", "健康看门狗"},
    {"snapshot.list",   "snapshot list",   "list snapshots",          "列出快照"},
    {"snapshot.create", "snapshot create", "create snapshot",         "创建快照"},
    {"remind.list",     "remind list",     "list reminders",          "列出提醒"},
    {"reminder.list",   "remind list",     "list reminders (dotted alias)", "列出提醒（点分别名）"},
    {"registry.list",       "registry list",       "list registry entries",  "列出注册表条目"},
    {"registry.show",       "registry show",       "show registry entry",    "查看注册表条目"},
    {"registry.reload",     "registry reload",     "reload registry",        "重载注册表"},
    {"registry.categories", "registry categories", "category/system/prefix", "类目/系统/前缀"},
    {"registry.check",      "registry check",      "consistency check",      "一致性校验"},
    {"control.run",         "control",             "run Control URL",        "执行 Control URL"},
    {NULL, NULL, NULL, NULL}
};

/* 明确保留的旧命令组前缀（非系统名；兼容铁律 → 一致性校验中不计违规，仅统计） */
static const char *g_legacy_prefixes[] = {
    "system", "log", "health", "snapshot", "remind", "reminder", "registry", "control", "port", NULL
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
    uart_puts(tr("See also: help systems (category/system/prefix/coverage)\n",
                 "另见：help systems（类目/系统/前缀/覆盖情况）\n"));
}

/* 点分名所属前缀是否等于给定系统前缀 */
static int alias_prefix_is(const char *dotted, const char *prefix) {
    if (!dotted || !prefix || !*prefix) return 0;
    size_t plen = strlen(prefix);
    return (strncmp(dotted, prefix, plen) == 0 && dotted[plen] == '.') ? 1 : 0;
}

/* 提取点分名前缀（buf 需 >= 64） */
static const char* alias_prefix(const char *dotted, char *buf, size_t buf_size) {
    if (!dotted || !buf || buf_size < 2) return NULL;
    const char *dot = strchr(dotted, '.');
    if (!dot) return NULL;
    size_t len = (size_t)(dot - dotted);
    if (len == 0 || len >= buf_size) return NULL;
    memcpy(buf, dotted, len);
    buf[len] = '\0';
    return buf;
}

static int is_legacy_prefix(const char *prefix) {
    for (int i = 0; g_legacy_prefixes[i]; i++) {
        if (strcmp(g_legacy_prefixes[i], prefix) == 0) return 1;
    }
    return 0;
}

void dotted_cmd_list_systems(void) {
    int n = registry_builtin_system_count();
    if (n <= 0) {
        uart_puts(tr("System/category table unavailable.\n", "系统类目表不可用。\n"));
        return;
    }

    uart_puts(tr("\n=== Categories / Systems / Command prefixes / CLI coverage ===\n",
                 "\n=== 类目 / 系统 / 命令前缀 / CLI 覆盖 ===\n"));

    /* 按表内类目顺序收集 */
    char cats[24][32];
    int cn = 0;
    for (int i = 0; i < n; i++) {
        const char *cat = NULL;
        registry_builtin_system_at(i, NULL, NULL, &cat, NULL, NULL);
        if (!cat) continue;
        int seen = 0;
        for (int g = 0; g < cn; g++) {
            if (strcmp(cats[g], cat) == 0) { seen = 1; break; }
        }
        if (!seen && cn < 24) {
            safe_strncpy(cats[cn], cat, sizeof(cats[0]));
            cn++;
        }
    }

    int covered = 0, gaps = 0;
    for (int g = 0; g < cn; g++) {
        char head[96];
        safe_snprintf(head, sizeof(head), "\n[%s]\n", cats[g]);
        uart_puts(head);
        for (int i = 0; i < n; i++) {
            const char *id = NULL, *name = NULL, *cat = NULL, *prefix = NULL;
            registry_builtin_system_at(i, &id, &name, &cat, &prefix, NULL);
            if (!cat || strcmp(cat, cats[g]) != 0) continue;

            char line[256];
            safe_snprintf(line, sizeof(line), "  %-11s %-13s %s.*\n",
                          id ? id : "?", name ? name : "?", prefix ? prefix : "?");
            uart_puts(line);

            int cnt = 0;
            for (int k = 0; g_dotted[k].dotted; k++) {
                if (!alias_prefix_is(g_dotted[k].dotted, prefix)) continue;
                char aline[256];
                safe_snprintf(aline, sizeof(aline), "      %-24s -> %s\n",
                              g_dotted[k].dotted, g_dotted[k].legacy);
                uart_puts(aline);
                cnt++;
            }
            if (cnt == 0) {
                uart_puts(tr("      (no CLI command surface — service/daemon/API side; honest gap)\n",
                             "      （CLI 无命令面——服务/守护/API 侧；诚实标注缺口）\n"));
                gaps++;
            } else {
                covered++;
            }
        }
    }

    char tail[320];
    safe_snprintf(tail, sizeof(tail),
                  tr("\nSystems with CLI commands: %d | without: %d | dotted commands: %d\n"
                     "Legacy command groups kept as aliases: system/log/health/snapshot/remind/registry/control\n"
                     "Usage: <system>.<action> [args]  (same implementation as the legacy form)\n",
                     "\n有 CLI 命令的系统：%d | 无：%d | 点分命令：%d\n"
                     "保留的旧命令组别名：system/log/health/snapshot/remind/registry/control\n"
                     "用法：<系统>.<动作> [参数]（与旧写法同一实现）\n"),
                  covered, gaps, dotted_cmd_count());
    uart_puts(tail);
}

int dotted_cmd_consistency_report(char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    int violations = 0;
    size_t used = 0;
    out[0] = '\0';

#define DREP_APPEND(...) do { \
        int _w = safe_snprintf(out + used, out_size - used, __VA_ARGS__); \
        if (_w < 0) { return violations; } \
        if ((size_t)_w >= out_size - used) { used = out_size - 1; } else { used += (size_t)_w; } \
    } while (0)

    DREP_APPEND("%s\n", tr("=== Dotted-command prefix consistency ===",
                            "=== 点分命令前缀一致性校验 ==="));

    int sys_ok = 0, legacy_ok = 0;
    for (int k = 0; g_dotted[k].dotted; k++) {
        char pbuf[64];
        const char *pfx = alias_prefix(g_dotted[k].dotted, pbuf, sizeof(pbuf));
        if (!pfx) {
            violations++;
            DREP_APPEND("  [%s] %s: '%s'\n", tr("FAIL", "失败"),
                        tr("malformed dotted name", "点分名格式错误"), g_dotted[k].dotted);
            continue;
        }
        if (registry_builtin_prefix_known(pfx)) {
            sys_ok++;
        } else if (is_legacy_prefix(pfx)) {
            legacy_ok++;
        } else {
            violations++;
            DREP_APPEND("  [%s] %s: '%s' (%s '%s')\n", tr("FAIL", "失败"),
                        tr("prefix is not a registered system nor a kept legacy group",
                           "前缀既非已登记系统，也不在保留的旧命令组中"),
                        g_dotted[k].dotted, tr("prefix", "前缀"), pfx);
        }
    }
    DREP_APPEND("%s: %s (%d %s / %d %s)\n", tr("Check prefix", "检查 前缀"),
                violations ? tr("FAIL", "失败") : tr("PASS", "通过"),
                sys_ok, tr("system prefixes", "系统前缀"),
                legacy_ok, tr("legacy groups", "旧命令组"));

    /* 覆盖情况（仅报告，不计违规——无 CLI 命令面属服务/守护侧，不编造命令） */
    int n = registry_builtin_system_count();
    int covered = 0, gaps = 0;
    for (int i = 0; i < n; i++) {
        const char *id = NULL, *prefix = NULL;
        registry_builtin_system_at(i, &id, NULL, NULL, &prefix, NULL);
        int cnt = 0;
        for (int k = 0; g_dotted[k].dotted; k++) {
            if (alias_prefix_is(g_dotted[k].dotted, prefix)) cnt++;
        }
        if (cnt > 0) {
            covered++;
        } else {
            gaps++;
            DREP_APPEND("  [%s] %s: %s\n", tr("GAP", "缺口"),
                        tr("system has no CLI dotted command", "系统无 CLI 点分命令"),
                        id ? id : "?");
        }
    }
    DREP_APPEND("%s: %s (%d/%d %s, %d %s)\n", tr("Check coverage", "检查 覆盖"),
                (gaps == 0) ? tr("FULL", "全覆盖") : tr("PARTIAL", "部分覆盖"),
                covered, n, tr("systems covered", "系统有命令"),
                gaps, tr("honest gaps", "诚实缺口"));
    DREP_APPEND("%s: %d\n", tr("Violations total", "违规项合计"), violations);
#undef DREP_APPEND
    return violations;
}
