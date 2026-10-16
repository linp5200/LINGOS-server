/**
 * @file    alert_cmds.c
 * @brief   alert 子命令实现（预警 = 生命线：宁误报不漏报）
 * @version LN-0.9.0
 *
 * 【2026-10-16 第 4 期收尾·审计修正】
 *   ① 数组越界（高危）：alert_type_t 现有 9 个取值（含 ALERT_TYPE_HEALTH /
 *      ALERT_TYPE_SECURITY），原 type_names[] 仅 7 项 → ev->type 为 7/8 时
 *      越界读取并把野指针交给 %s（生命线命令上属高危）。改为 switch 取名，
 *      含全部枚举 + default 兜底，并补齐中英双语。
 *   ② localtime() 未判空即传 strftime（空指针崩溃风险）→ 显式防护。
 *   ③ 空态诚实化：本进程无活跃预警 ≠ 无预警数据——预警由独立守护 lingos_alertd
 *      维护，本进程 g_last_events 恒空。原实现直接打印"没有活跃的预警"= 谎报平安。
 *      现查历史（跨进程文件）区分「本进程无数据源」与「确实无预警」，并明确提示
 *      以 App/Web 与 lingos_alertd 为准（宁误报不漏报）。
 *   ④ 接线：alert_dispatch 此前无任何调用者（造好没接线）→ shell 已接 alert 命令。
 */

#include "alert_cmds.h"
#include "../alert/alert_manager.h"
#include "../alert/alert_config.h"
#include "../alert/alert_history.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../common/safe_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================
 * 类型/级别 名称（switch：不会越界；覆盖全部枚举）
 * ============================================================ */
static const char* alert_type_name(int type) {
    switch (type) {
        case ALERT_TYPE_UNKNOWN:    return tr("Unknown", "未知");
        case ALERT_TYPE_TYPHOON:    return tr("Typhoon", "台风");
        case ALERT_TYPE_EARTHQUAKE: return tr("Earthquake", "地震");
        case ALERT_TYPE_RAIN:       return tr("Rain", "暴雨");
        case ALERT_TYPE_HIGH_TEMP:  return tr("High Temp", "高温");
        case ALERT_TYPE_STORM:      return tr("Storm", "风暴");
        case ALERT_TYPE_FIRE:       return tr("Fire", "火灾");
        case ALERT_TYPE_HEALTH:     return tr("Health", "系统健康");
        case ALERT_TYPE_SECURITY:   return tr("Security", "安全威胁");
        default:                    return tr("Unknown", "未知");
    }
}

static const char* alert_level_name(int level) {
    switch (level) {
        case 0:  return tr("Info", "信息");
        case 1:  return tr("Blue", "蓝色");
        case 2:  return tr("Yellow", "黄色");
        case 3:  return tr("Orange", "橙色");
        case 4:  return tr("Red", "红色");
        case 5:  return tr("Critical", "紧急");
        default: return tr("Unknown", "未知");
    }
}

/* 时间戳 → 文本（localtime 失败不崩溃） */
static void alert_time_str(time_t ts, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    struct tm *tm = localtime(&ts);
    if (!tm) {
        safe_strncpy(out, tr("(invalid time)", "（时间无效）"), out_size);
        return;
    }
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S", tm);
}

/* ============================================================
 * 显示预警状态
 * ============================================================ */
static void cmd_alert_status(void) {
    alert_event_t events[8];
    int count = alert_manager_get_latest(events, 8);

    uart_puts(tr("\n=== Alert Status ===\n", "\n=== 预警状态 ===\n"));

    if (count <= 0) {
        /* 诚实空态：本进程无数据 ≠ 无风险（预警由 lingos_alertd 独立维护） */
        alert_event_t hist[8];
        int hcount = alert_history_query(NULL, NULL, 24 * 30, hist, 8);
        uart_puts(tr("No active alert in this process.\n", "本进程内没有活跃预警。\n"));
        if (hcount > 0) {
            char buf[256];
            char tbuf[40];
            alert_time_str(hist[0].timestamp, tbuf, sizeof(tbuf));
            safe_snprintf(buf, sizeof(buf),
                          tr("  History: %d record(s) in last 30 days (latest %s: %s).\n",
                             "  历史：近 30 天 %d 条记录（最近 %s：%s）。\n"),
                          hcount, tbuf, alert_type_name(hist[0].type));
            uart_puts(buf);
        } else {
            /* 生命线原则：无数据源时不得输出"平安"结论 */
            uart_puts(tr("  WARNING: no alert data available in this process (alerts are maintained by\n"
                         "  the standalone lingos_alertd daemon). 'No data' does NOT mean 'no risk'.\n"
                         "  Check the App/Web alert page or lingos_alertd logs for authoritative status.\n",
                         "  警告：本进程未取到预警数据（预警由独立守护 lingos_alertd 维护）。\n"
                         "  「无数据」不等于「无风险」——请以 App/Web 预警页或 lingos_alertd 日志为准。\n"));
        }
        return;
    }

    for (int i = 0; i < count; i++) {
        alert_event_t *ev = &events[i];
        char time_buf[40];
        alert_time_str(ev->timestamp, time_buf, sizeof(time_buf));

        char buf[320];
        safe_snprintf(buf, sizeof(buf),
                      "  [%s] %s: %s (%s %s)\n"
                      "    %s: %s, %s: %s\n",
                      time_buf,
                      alert_type_name(ev->type),
                      ev->description,
                      tr("Level", "级别"), alert_level_name(ev->level),
                      tr("Location", "位置"), ev->location,
                      tr("Source", "来源"), ev->source);
        uart_puts(buf);
        uart_puts("\n");
    }
}

/* ============================================================
 * 显示配置
 * ============================================================ */
static void cmd_alert_config_show(void) {
    alert_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int loaded = alert_config_load(&cfg);
    alert_config_validate(&cfg);

    uart_puts(tr("\n=== Alert Configuration ===\n", "\n=== 预警配置 ===\n"));
    if (loaded != 0) {
        /* 配置读取失败不能伪装成默认值正常 */
        uart_puts(tr("  WARNING: config file not loaded — values below are defaults after validation.\n",
                     "  警告：配置文件未加载——以下为校验后的默认值。\n"));
    }
    char buf[128];
    safe_snprintf(buf, sizeof(buf), tr("Base interval: %ds\n", "基础间隔：%d 秒\n"), cfg.base_interval);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), tr("Exception interval: %ds\n", "异常间隔：%d 秒\n"), cfg.exception_interval);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), tr("Realtime interval: %ds\n", "实时间隔：%d 秒\n"), cfg.realtime_interval);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), tr("Typhoon distance threshold: %dkm\n", "台风距离阈值：%d 公里\n"),
                  cfg.typhoon_distance_threshold);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), tr("Typhoon level threshold: %d\n", "台风等级阈值：%d\n"),
                  cfg.typhoon_level_threshold);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), tr("Earthquake magnitude threshold: %.1f\n", "地震震级阈值：%.1f\n"),
                  cfg.earthquake_magnitude_threshold);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), tr("Rainfall threshold: %.1fmm/24h\n", "降雨阈值：%.1f 毫米/24小时\n"),
                  cfg.rainfall_threshold);
    uart_puts(buf);
}

/* ============================================================
 * 查看历史
 * ============================================================ */
static void cmd_alert_history(int limit) {
    alert_event_t events[32];
    int count = alert_history_query(NULL, NULL, 24 * 30, events, 32); /* 最近30天 */
    if (count > limit) count = limit;

    uart_puts(tr("\n=== Alert History ===\n", "\n=== 预警历史 ===\n"));

    if (count <= 0) {
        uart_puts(tr("No history record in the last 30 days.\n", "最近 30 天没有历史记录。\n"));
        uart_puts(tr("  (History is written by lingos_alertd; an empty list here can also mean the\n"
                     "   daemon has not written yet — it does not prove 'no risk'.)\n",
                     "  （历史由 lingos_alertd 写入；此处为空也可能是守护尚未写入——不代表「无风险」。）\n"));
        return;
    }

    for (int i = 0; i < count; i++) {
        alert_event_t *ev = &events[i];
        char time_buf[40];
        alert_time_str(ev->timestamp, time_buf, sizeof(time_buf));

        char buf[320];
        safe_snprintf(buf, sizeof(buf), "  %s: %s (%s %s) - %s\n",
                      time_buf,
                      alert_type_name(ev->type),
                      tr("Level", "级别"), alert_level_name(ev->level),
                      ev->description);
        uart_puts(buf);
    }
}

/* ============================================================
 * 主分发函数
 * ============================================================ */
void alert_dispatch(const char *args) {
    LOG_DEBUG_T("AlertCmds", "Dispatch", "Enter", "args=%s", args ? args : "(null)");

    if (!args || !*args) {
        uart_puts(tr("Usage: alert <subcommand>\n", "用法：alert <子命令>\n"));
        uart_puts(tr("  status       - Show current alerts\n", "  status       - 显示当前预警\n"));
        uart_puts(tr("  config       - Show configuration\n", "  config       - 显示配置\n"));
        uart_puts(tr("  history [N]  - Show history (last N entries)\n", "  history [N]  - 显示历史（最近 N 条）\n"));
        return;
    }

    if (strcmp(args, "status") == 0) {
        cmd_alert_status();
    } else if (strcmp(args, "config") == 0) {
        cmd_alert_config_show();
    } else if (strncmp(args, "history", 7) == 0) {
        int limit = 10;
        if (args[7] == ' ') {
            limit = atoi(args + 8);
            if (limit < 1) limit = 10;
        }
        cmd_alert_history(limit);
    } else {
        uart_puts(tr("Unknown alert subcommand.\n", "未知的 alert 子命令。\n"));
        uart_puts(tr("Available: status, config, history [N]\n", "可用：status、config、history [N]\n"));
    }
}
