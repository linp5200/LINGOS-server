/**
 * @file    alert_notify.c
 * @brief   预警通知分发（Shell/TUI/MQTT）
 * @version LN-B-5.0.0.0
 * @par     核心协议：容错编程（推送失败不影响主流程）
 * @changes MQTT 状态检查（mqtt_client_is_connected 前置）
 */

#include "alert_notify.h"
#include "alert_config.h"
#include "alert_utils.h"     /* 【第9期】alert_type_label —— 类型名单一事实来源 */
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../net/mqtt/mqtt_client.h"
#include "../net/http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static alert_config_t g_notify_config;
static int g_mqtt_available = 0;

/* 【第9期·生命线投递可见性】
 *   问题：alert_report_ws 失败只打 DEBUG（默认级别不可见）、force_send 无论成败都
 *   打 INFO "forced alert sent"（假成功）→ 远程通道全挂时用户以为已送达。
 *   现：记录最近一次投递结果（1=成功 0=失败/跳过），失败按级别升到 WARN/ERROR；
 *       远程（WS/MQTT）全失败时额外打一条 ERROR，并在本地横幅后追一行可见提示。 */
static int g_last_ws_ok = 0;      /* 最近一次 WS 上报（App/Qt/Web）结果 */
static int g_last_mqtt_ok = 0;    /* 最近一次 MQTT 发布结果（未启用/未连接记 0） */
static int g_remote_fail_streak = 0;  /* 远程连续失败次数（诊断用） */

/* ============================================================
 * 【0.4.3】WS 实时广播上报（alert_event——仿 visiond /api/vision_event）
 * alertd 检测到新预警 → libcurl POST ai_server /api/alert_event → WS 广播 App/Qt/Web
 * 容错：失败静默（不影响主流程）；ai_server 未起时自动跳过
 * ============================================================ */

/* 返回：1=上报成功（HTTP 200），0=失败。level>=3（生命线）失败时重试一次。 */
static int alert_report_ws(const alert_event_t *event) {
    if (!event) return 0;

    /* 用 json-c 或手拼——此处手拼 JSON（防弹：超长 desc 截断） */
    char desc[192];
    safe_strncpy(desc, event->description, sizeof(desc));

    /* 简易 JSON 转义（引号/反斜杠/控制符） */
    char esc[512];
    memset(esc, 0, sizeof(esc));
    int ei = 0;
    for (int i = 0; desc[i] != '\0' && ei < (int)sizeof(esc) - 6; i++) {
        char c = desc[i];
        if (c == '"' || c == '\\') {
            esc[ei++] = '\\';
            esc[ei++] = c;
        } else if (c == '\n') {
            esc[ei++] = '\\';
            esc[ei++] = 'n';
        } else if ((unsigned char)c < 0x20) {
            /* 跳过控制符 */
        } else {
            esc[ei++] = c;
        }
    }
    esc[ei] = '\0';

    /* 【第9期】source 同样转义（原先裸插入 JSON——含引号/反斜杠时
     *   App/Qt/Web 的 jsonDecode 直接失败 → 预警被静默丢弃）；
     *   title 改用真实类型名（原为 "<source> alert" 硬编码英文），并带上级别便于分级展示。 */
    char src_esc[128];
    memset(src_esc, 0, sizeof(src_esc));
    {
        int si = 0;
        for (int i = 0; event->source[i] != '\0' && si < (int)sizeof(src_esc) - 2; i++) {
            char c = event->source[i];
            if (c == '"' || c == '\\') { src_esc[si++] = '\\'; src_esc[si++] = c; }
            else if ((unsigned char)c < 0x20) { /* 丢弃控制符 */ }
            else { src_esc[si++] = c; }
        }
        src_esc[si] = '\0';
    }
    char title_esc[96];
    memset(title_esc, 0, sizeof(title_esc));
    safe_snprintf(title_esc, sizeof(title_esc), "%s", alert_type_label((int)event->type));

    char json[896];
    safe_snprintf(json, sizeof(json),
                  "{\"type\":%d,\"level\":%d,\"level_label\":\"%s\",\"title\":\"%s\","
                  "\"description\":\"%s\",\"source\":\"%s\",\"timestamp\":%ld}",
                  event->type, event->level, alert_level_label(event->level),
                  title_esc, esc, src_esc, (long)event->timestamp);

    /* 【0.5.0】改用内置 socket HTTP（去掉 libcurl 硬依赖 —— 先生要求适配 22.04~25.10）
     * 原实现用 libcurl → 传递依赖 libldap-2.5、librtmp、libav 系列、libunistring 等
     * 22.04 专属 soname，在 25.10 上不存在 → 二进制无法启动。
     * 本上报为**内网 POST**，纯 socket 完全满足。 */
    char resp[256];
    int rc = http_post_json("http://127.0.0.1:8088/api/alert_event",
                            json, resp, sizeof(resp), 2);

    /* 【第9期】生命线（level>=3）：失败立即重试一次（8088 语音 HTTP 服务刚起/抖动时最常见） */
    if (rc != 0 && event->level >= 3) {
        LOG_WARN_T("AlertNotify", "WSReport", "Retry", "lifeline report failed rc=%d — retrying once", rc);
        rc = http_post_json("http://127.0.0.1:8088/api/alert_event",
                            json, resp, sizeof(resp), 2);
    }

    if (rc == 0) {
        g_last_ws_ok = 1;
        LOG_DEBUG_T("AlertNotify", "WSReport", "OK", "alert event reported (level=%d)", event->level);
        return 1;
    }

    /* 失败不再吞在 DEBUG：生命线级 ERROR，低级别 WARN（用户/排障可见） */
    g_last_ws_ok = 0;
    g_remote_fail_streak++;
    if (event->level >= 3) {
        LOG_ERROR_T("AlertNotify", "WSReport", "LifelineFail",
                    "LIFELINE push FAILED rc=%d (level=%d) — App/Qt/Web will NOT see this alert; local banner only",
                    rc, event->level);
    } else {
        LOG_WARN_T("AlertNotify", "WSReport", "Fail",
                   "alert report failed rc=%d (level=%d) — App/Qt/Web missed this event", rc, event->level);
    }
    return 0;
}

/* ============================================================
 * 通知方式枚举
 * ============================================================ */

typedef enum {
    NOTIFY_SHELL = 1 << 0,
    NOTIFY_TUI = 1 << 1,
    NOTIFY_MQTT = 1 << 2
} notify_method_t;

/* ============================================================
 * 初始化
 * ============================================================ */

void alert_notify_init(const alert_config_t *config) {
    if (config) {
        g_notify_config = *config;
    }

    /* 【修改】MQTT 状态检查（init 快照；后续由 notify_mqtt 按真实连接状态自愈） */
    if (mqtt_client_is_connected()) {
        g_mqtt_available = 1;
        LOG_DEBUG_T("AlertNotify", "Init", "MQTT", "MQTT available");
    } else {
        g_mqtt_available = 0;
        /* 【第9期】不再只打 DEBUG——"启动时 MQTT 未连"是用户需要知道的生命线通道状态 */
        LOG_WARN_T("AlertNotify", "Init", "MQTT",
                   "MQTT not connected at init (mqtt_enabled=%d) — will re-check on each alert",
                   g_notify_config.mqtt_enabled);
    }
}

/* ============================================================
 * Shell 通知（横幅）
 * ============================================================ */

static void notify_shell(const alert_event_t *event) {
    if (!event) return;

    const char *level_colors[] = {
        "\033[37m",
        "\033[34m",
        "\033[33m",
        "\033[38;5;214m",
        "\033[31m",
        "\033[35m"
    };
    /* 【第9期】类型名走 alert_utils.alert_type_label（越界安全 + 与历史 JSON 同源；
     *   原先此处内联一份数组，history/notify 两处名称会各自漂移） */
    const char *color = (event->level >= 0 && event->level <= 5) ? level_colors[event->level] : "\033[37m";

    uart_puts("\n");
    uart_puts(color);
    uart_puts("╔══════════════════════════════════════════════════════════════════╗\n");
    uart_puts("║  ⚠️  ");
    uart_puts(tr("ALERT", "预警"));
    uart_puts(": ");
    uart_puts(alert_type_label((int)event->type));
    uart_puts(" (");
    uart_puts(tr("Level", "等级"));
    uart_puts(" ");
    char level_str[8];
    safe_snprintf(level_str, sizeof(level_str), "%d", event->level);
    uart_puts(level_str);
    uart_puts(")  ║\n");
    uart_puts("║  ");
    uart_puts(event->description);
    int len = strlen(event->description);
    for (int i = len; i < 58; i++) uart_puts(" ");
    uart_puts(" ║\n");
    if (event->distance_km >= 0) {
        char dist_str[32];
        safe_snprintf(dist_str, sizeof(dist_str), "║  %s: %d km",
                      tr("Distance", "距离"), event->distance_km);
        uart_puts(dist_str);
        int dlen = strlen(dist_str);
        for (int i = dlen; i < 58; i++) uart_puts(" ");
        uart_puts(" ║\n");
    }
    uart_puts("╚══════════════════════════════════════════════════════════════════╝\n");
    uart_puts("\033[0m");
}

/* ============================================================
 * TUI 通知（弹窗）
 * ============================================================ */

static void notify_tui(const alert_event_t *event) {
    LOG_INFO_T("AlertNotify", "TUI", "Alert", "type=%d, level=%d, desc=%s",
               event->type, event->level, event->description);
}

/* ============================================================
 * 【修改】MQTT 推送（含状态检查）
 * ============================================================ */

/* 返回：1=已发布，0=未发送（未连接/未启用/失败） */
static int notify_mqtt(const alert_event_t *event) {
    if (!event) return 0;

    /* MQTT 状态检查 */
    if (!mqtt_client_is_connected()) {
        if (g_mqtt_available) {
            LOG_WARN_T("AlertNotify", "MQTT", "Lost", "MQTT connection lost — alerts will not be published");
        }
        g_mqtt_available = 0;
        g_last_mqtt_ok = 0;
        LOG_DEBUG_T("AlertNotify", "MQTT", "Skip", "MQTT not connected");
        return 0;
    }

    /* 【第9期·静默失败修复】原实现：g_mqtt_available 只在 alert_notify_init 里置 1。
     *   init 时 MQTT 尚未连上（启动顺序常见）→ 该标志永远为 0 →
     *   即使之后 MQTT 已连上，每次预警都在此被静默 return（用户开了 MQTT 也永远收不到）。
     *   现：连接状态即真源，发现已连接就自愈置位并告知（不再永久静默跳过）。 */
    if (!g_mqtt_available) {
        g_mqtt_available = 1;
        LOG_INFO_T("AlertNotify", "MQTT", "Recovered",
                   "MQTT connected after init — alert publishing re-enabled");
    }

    /* 【第9期】desc/source JSON 转义（原裸插：描述含引号 → 订阅端解析失败 = 静默丢预警） */
    char m_desc[384], m_src[128];
    memset(m_desc, 0, sizeof(m_desc));
    memset(m_src, 0, sizeof(m_src));
    {
        int oi = 0;
        for (int i = 0; event->description[i] != '\0' && oi < (int)sizeof(m_desc) - 2; i++) {
            char c = event->description[i];
            if (c == '"' || c == '\\') { m_desc[oi++] = '\\'; m_desc[oi++] = c; }
            else if (c == '\n') { m_desc[oi++] = '\\'; m_desc[oi++] = 'n'; }
            else if ((unsigned char)c < 0x20) { /* 丢弃 */ }
            else { m_desc[oi++] = c; }
        }
        m_desc[oi] = '\0';
        oi = 0;
        for (int i = 0; event->source[i] != '\0' && oi < (int)sizeof(m_src) - 2; i++) {
            char c = event->source[i];
            if (c == '"' || c == '\\') { m_src[oi++] = '\\'; m_src[oi++] = c; }
            else if ((unsigned char)c < 0x20) { /* 丢弃 */ }
            else { m_src[oi++] = c; }
        }
        m_src[oi] = '\0';
    }
    char json[896];
    safe_snprintf(json, sizeof(json),
                  "{\"type\":%d,\"level\":%d,\"level_label\":\"%s\",\"title\":\"%s\","
                  "\"desc\":\"%s\",\"source\":\"%s\",\"timestamp\":%ld}",
                  event->type, event->level, alert_level_label(event->level),
                  alert_type_label((int)event->type), m_desc, m_src, (long)event->timestamp);

    int ret = mqtt_client_publish("lingos/alert", json, strlen(json), 1, 0);
    if (ret == 0) {
        g_last_mqtt_ok = 1;
        LOG_DEBUG_T("AlertNotify", "MQTT", "Sent", "alert published");
        return 1;
    }
    g_last_mqtt_ok = 0;
    g_remote_fail_streak++;
    if (event->level >= 3) {
        LOG_ERROR_T("AlertNotify", "MQTT", "LifelineFail",
                    "LIFELINE MQTT publish FAILED ret=%d (level=%d)", ret, event->level);
    } else {
        LOG_WARN_T("AlertNotify", "MQTT", "Fail", "publish failed, ret=%d", ret);
    }
    return 0;
}

/* ============================================================
 * 主发送函数
 * ============================================================ */

void alert_notify_send(const alert_event_t *event, const alert_config_t *config) {
    if (!event || !config) return;

    int forced = (event->level >= 3);

    /* 【0.4.3】WS 实时广播上报（新预警 → App/Qt/Web 实时弹条） */
    int ws_ok = alert_report_ws(event);

    /* Shell 通知（始终启用——本地通道，不受配置限制） */
    notify_shell(event);

    /* TUI 通知 */
    notify_tui(event);

    /* 【修改】MQTT 通知（含状态检查）；mqtt_enabled=0 → 用户选择不出网（数据不出家门） */
    int mqtt_ok = 0;
    if (config->mqtt_enabled) {
        mqtt_ok = notify_mqtt(event);
    } else {
        g_last_mqtt_ok = 0;
        LOG_DEBUG_T("AlertNotify", "MQTT", "Disabled",
                    "mqtt_enabled=0 — local-only mode, no MQTT egress");
    }

    if (config->sound_enabled || forced) {
        uart_puts("\a");
    }

    /* 【第9期·假成功修复】远程通道（App/Qt/Web + MQTT）全失败时给出可见结论，
     *   不再让用户以为预警已经送达。 */
    if (!ws_ok && !mqtt_ok) {
        g_remote_fail_streak++;
        LOG_ERROR_T("AlertNotify", "Send", "NoRemoteChannel",
                    "alert level=%d reached NO remote channel (App/Qt/Web + MQTT both failed) — local banner only",
                    event->level);
        uart_puts(tr("[!] alert NOT delivered to App/Web (local only)\n",
                     "[!] 预警未能送达 App/Web（仅本机提示）\n"));
    } else {
        g_remote_fail_streak = 0;
    }
}

/* ============================================================
 * 强制发送（不受配置限制）
 * ============================================================ */

void alert_notify_force_send(const alert_event_t *event) {
    if (!event) return;

    int ws_ok = alert_report_ws(event);
    notify_shell(event);
    notify_tui(event);

    /* 【第9期】MQTT 尊重用户开关（数据不出家门）——原实现无条件调用 notify_mqtt，
     *   但受 g_mqtt_available 陈旧标志影响实际几乎从未发送（既没送达也没人知道）。
     *   现语义明确：mqtt_enabled=1 才出网；本机横幅/TUI/WS→App/Qt/Web 不受配置限制。 */
    int mqtt_ok = 0;
    if (g_notify_config.mqtt_enabled) {
        mqtt_ok = notify_mqtt(event);
    } else {
        g_last_mqtt_ok = 0;
        LOG_DEBUG_T("AlertNotify", "ForceSend", "MqttOff",
                    "mqtt_enabled=0 — force-send stays local (no egress)");
    }

    uart_puts("\a");

    /* 【第9期·假成功修复】原实现无论投递结果都打 INFO "forced alert sent"（假成功）。
     *   现在只有真的有远程通道成功才叫 sent；否则打 ERROR + 本机可见提示。 */
    if (ws_ok || mqtt_ok) {
        LOG_INFO_T("AlertNotify", "ForceSend", "OK", "forced alert sent (ws=%d mqtt=%d)", ws_ok, mqtt_ok);
    } else {
        LOG_ERROR_T("AlertNotify", "ForceSend", "NoRemoteChannel",
                    "LIFELINE alert level=%d reached NO remote channel (local banner only)", event->level);
        uart_puts(tr("[!] lifeline alert NOT delivered remotely (local only)\n",
                     "[!] 生命线预警未能远程送达（仅本机提示）\n"));
    }
}

/* ============================================================
 * 清理
 * ============================================================ */

void alert_notify_cleanup(void) {
    g_mqtt_available = 0;
    g_last_ws_ok = 0;
    g_last_mqtt_ok = 0;
    g_remote_fail_streak = 0;
    LOG_DEBUG_T("AlertNotify", "Cleanup", "OK", "notification system cleaned up");
}