/**
 * @file    security_wire.c
 * @brief   安全模块接线中枢实现（第5期·死代码接线上岗）
 * @version LN-0.9.0-p5
 *
 * 本文件是「零调用者」安全模块的统一上岗点：
 *   每个被接线的模块在此都有**真实调用**（非空调用、非恒真返回），
 *   并由 security_wire_self_test() 逐项验证语义，接线断了就报 ERROR。
 */

#include "security_wire.h"
#include "input_filter.h"
#include "output_filter.h"
#include "execution_gate.h"
#include "defense.h"         /* defense_check_anomaly（真实评分，非占位） */
#include "defense_mode.h"     /* shadow_mode_should_return_empty / shadow_mode_get_empty_data */
#include "dark_mode.h"
#include "secure_memory.h"
#include "audit.h"
#include "virus_scanner.h"
#include "security_config.h"
#include "verify/second_verify.h"
#include "../firewall/firewall.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define WIRE_REASON_MAX 192

static pthread_mutex_t g_wire_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_wire_initialized = 0;
static int g_wire_selftest_failures = -1;

/* ============================================================
 * 来源标签 → 执行门来源枚举
 * ============================================================ */

static gate_source_t wire_map_source(const char *source) {
    if (!source || !*source)                 return GATE_SOURCE_API_REQUEST;
    if (strcmp(source, "shell") == 0)        return GATE_SOURCE_SHELL;
    if (strcmp(source, "ai") == 0)           return GATE_SOURCE_AI_PROMPT;
    if (strcmp(source, "ai_prompt") == 0)    return GATE_SOURCE_AI_PROMPT;
    if (strcmp(source, "ai_tool") == 0)      return GATE_SOURCE_AI_TOOL;
    if (strcmp(source, "skill") == 0)        return GATE_SOURCE_AI_TOOL;
    if (strcmp(source, "wizard") == 0)       return GATE_SOURCE_CONFIG_WIZARD;
    if (strcmp(source, "tui") == 0)          return GATE_SOURCE_TUI_TERMINAL;
    if (strcmp(source, "rule") == 0)         return GATE_SOURCE_RULE_ENGINE;
    if (strcmp(source, "host") == 0)         return GATE_SOURCE_HOST_CMD;
    if (strcmp(source, "mqtt") == 0)         return GATE_SOURCE_MQTT;
    if (strcmp(source, "file") == 0)         return GATE_SOURCE_FILE;
    if (strcmp(source, "plugin") == 0)       return GATE_SOURCE_PLUGIN;
    return GATE_SOURCE_API_REQUEST;
}

/* ============================================================
 * 高危命令判定（用于二次验证触发，非拒绝——拒绝由执行门负责）
 * ============================================================ */

static int wire_is_high_risk(const char *cmdline) {
    if (!cmdline) return 0;

    static const char *risky[] = {
        "reboot", "shutdown", "poweroff", "halt", "mkfs", "fdisk", "parted",
        "iptables", "nft ", "useradd", "userdel", "passwd", "visudo",
        "chown root", "chmod 777", "dd if=", "rm -rf", "kexec", "mount ",
        "umount ", "insmod", "rmmod", "systemctl", "flash_erase", "swapon",
        NULL
    };

    for (int i = 0; risky[i]; i++) {
        if (strstr(cmdline, risky[i]) != NULL) return 1;
    }
    return 0;
}

/* ============================================================
 * 初始化 + 自检
 * ============================================================ */

int security_wire_self_test(char *report, size_t report_len) {
    if (report && report_len) report[0] = '\0';

    char buf[1024];
    size_t pos = 0;
    int fail = 0;

#define WIRE_APPEND(...)                                                        \
    do {                                                                        \
        int _n = snprintf(buf + pos, sizeof(buf) - pos, __VA_ARGS__);           \
        if (_n > 0 && (size_t)_n < sizeof(buf) - pos) pos += (size_t)_n;        \
    } while (0)

#define WIRE_CHECK(cond, name)                                                  \
    do {                                                                        \
        if (cond) {                                                             \
            WIRE_APPEND("[OK]   %s\n", name);                                  \
        } else {                                                                \
            fail++;                                                             \
            WIRE_APPEND("[FAIL] %s\n", name);                                  \
        }                                                                       \
    } while (0)

    /* 1. input_filter.c：注入模式必须被拦下 */
    {
        char r[128] = {0};
        int blocked = (input_filter_check("echo $(id)", r, sizeof(r)) != 0);
        blocked = blocked && (input_filter_check("rm -rf /", r, sizeof(r)) != 0);
        WIRE_CHECK(blocked, "input_filter.c blocks injection/dangerous patterns");
    }

    /* 2. execution_gate.c：空输入必须拒绝（与配置模式无关的确定性语义） */
    {
        char r[128] = {0};
        WIRE_CHECK(execution_gate_check(GATE_SOURCE_AI_TOOL, NULL, "selftest", r, sizeof(r))
                       == GATE_RESULT_DENY,
                   "execution_gate.c denies empty input");
    }

    /* 3. output_filter.c：IP / 邮箱必须被打码，且不越界 */
    {
        const char *red = output_filter_redact("ip=10.1.2.3 mail=user@example.com end");
        WIRE_CHECK(red && strstr(red, "[REDACTED_IP]") && strstr(red, "[REDACTED_EMAIL]")
                       && strstr(red, "end") != NULL,
                   "output_filter.c redacts IP/email and keeps tail");
    }

    /* 4. secure_memory.c：锁定内存可分配、可归零释放 */
    {
        char *p = (char *)secure_alloc(64);
        int ok = (p != NULL);
        if (ok) {
            memset(p, 'A', 64);
            secure_free(p, 64);
        }
        WIRE_CHECK(ok, "secure_memory.c alloc/free round trip");
    }

    /* 5. shadow_mode.c：空数据语义 + 判定函数可调用（返回 0/1） */
    {
        int se = shadow_mode_should_return_empty("__wire_selftest__", "__wire_selftest_perm__");
        WIRE_CHECK((se == 0 || se == 1) &&
                       strcmp(shadow_mode_get_empty_data("list"), "[]") == 0 &&
                       strcmp(shadow_mode_get_empty_data("object"), "{}") == 0,
                   "shadow_mode.c empty-data semantics");
    }

    /* 6. dark_mode.c：功能封锁判定可调用；非敏感硬件不受影响 */
    {
        int blocked = dark_mode_should_block_feature("__wire_selftest__");
        int dev = dark_mode_simulate_hardware_disable("__wire_selftest_device__");
        WIRE_CHECK((blocked == 0 || blocked == 1) && dev == 0,
                   "dark_mode.c feature/device gates");
    }

    /* 7. firewall.c：目标校验（注入串必须被拒） */
    {
        WIRE_CHECK(firewall_is_valid_target("10.0.0.1") == 1 &&
                       firewall_is_valid_target("192.168.1.0/24") == 1 &&
                       firewall_is_valid_target("10.0.0.1; rm -rf /") == 0 &&
                       firewall_is_valid_target("8.8.8.8/33") == 0,
                   "firewall.c target validation rejects injection");
    }

    /* 8. verify/second_verify.c：结果名映射（模块已链接且语义正确） */
    {
        WIRE_CHECK(strcmp(second_verify_result_str(VERIFY_RESULT_APPROVED), "approved") == 0 &&
                       strcmp(second_verify_result_str(VERIFY_RESULT_DENIED), "denied") == 0,
                   "second_verify.c result mapping");
    }

    /* 9. virus_scanner.c：结果名映射 */
    {
        WIRE_CHECK(strcmp(virus_scanner_result_name(SCAN_RESULT_MALICIOUS), "Malicious") == 0 &&
                       strcmp(virus_scanner_result_name(SCAN_RESULT_CLEAN), "Clean") == 0,
                   "virus_scanner.c result mapping");
    }

#undef WIRE_CHECK
#undef WIRE_APPEND

    if (report && report_len) safe_strncpy(report, buf, report_len);
    return fail;
}

int security_wire_init(void) {
    pthread_mutex_lock(&g_wire_lock);
    if (g_wire_initialized) {
        int known = g_wire_selftest_failures;
        pthread_mutex_unlock(&g_wire_lock);
        return (known == 0) ? 0 : -1;
    }
    g_wire_initialized = 1;
    pthread_mutex_unlock(&g_wire_lock);

    LOG_INFO_T("SecurityWire", "Init", "Enter", "wiring security modules");

    input_filter_init();                 /* input_filter.c */
    int ofr = output_filter_reload();    /* output_filter.c（编译脱敏正则 + 清空工作区） */
    int svi = second_verify_init();      /* verify/second_verify.c */

    char report[1024] = {0};
    int fails = security_wire_self_test(report, sizeof(report));
    g_wire_selftest_failures = fails;

    LOG_DEBUG_T("SecurityWire", "Init", "SelfTest", "\n%s", report);
    if (fails != 0) {
        LOG_ERROR_T("SecurityWire", "Init", "WiringBroken",
                    "%d module self-check(s) failed — 安全模块接线可能被破坏", fails);
    }
    LOG_INFO_T("SecurityWire", "Init", "OK",
               "filters=%d, output_filter=%d, second_verify=%d, self_test_failures=%d",
               1, ofr, svi, fails);

    return (fails == 0 && ofr == 0 && svi == 0) ? 0 : -1;
}

int security_wire_last_self_test_failures(void) {
    return g_wire_selftest_failures;
}

/* ============================================================
 * 输入面
 * ============================================================ */

int security_wire_check_input(const char *input, const char *source,
                              char *reason, size_t reason_len) {
    if (reason && reason_len) reason[0] = '\0';
    if (!input) {
        if (reason && reason_len) safe_strncpy(reason, "null input", reason_len);
        return SECURITY_WIRE_DENY;
    }

    char why[WIRE_REASON_MAX] = {0};
    gate_source_t gs = wire_map_source(source);
    gate_result_t g = execution_gate_check(gs, input, source ? source : "wire",
                                           why, sizeof(why));

    if (g == GATE_RESULT_DENY) {
        if (reason && reason_len) {
            safe_strncpy(reason, why[0] ? why : "denied by execution gate", reason_len);
        }
        LOG_WARN_T("SecurityWire", "CheckInput", "Denied", "source=%s | %s",
                   source ? source : "unknown", why);
        /* 【接线】审计模块记录拒绝事件（audit.c 已是活跃模块） */
        audit_log("system", "security_wire", "input_denied",
                  source ? source : "unknown", why[0] ? why : "denied", -1, "high", 0);
        return SECURITY_WIRE_DENY;
    }

    if (g == GATE_RESULT_NEED_CONFIRM) return SECURITY_WIRE_NEED_CONFIRM;

    /* 影子模式：调用方应返回结构正确的空数据（假成功，无副作用） */
    if (defense_mode_get() == DEFENSE_MODE_SHADOW) {
        LOG_INFO_T("SecurityWire", "CheckInput", "Shadow",
                   "shadow mode active — caller should return empty data");
        return SECURITY_WIRE_SHADOW;
    }

    return SECURITY_WIRE_ALLOW;
}

/* ============================================================
 * 命令执行面
 * ============================================================ */

int security_wire_command_allowed(const char *cmdline, char *reason, size_t reason_len) {
    if (reason && reason_len) reason[0] = '\0';
    if (!cmdline || !*cmdline) {
        if (reason && reason_len) safe_strncpy(reason, "empty command", reason_len);
        return SECURITY_WIRE_DENY;
    }

    /* 1) 暗影/绝对保护模式的功能黑名单（dark_mode.c） */
    if (dark_mode_should_block_feature(cmdline)) {
        if (reason && reason_len) safe_strncpy(reason, "blocked by dark mode policy", reason_len);
        LOG_WARN_T("SecurityWire", "Command", "DarkBlocked", "%.100s", cmdline);
        audit_log("system", "security_wire", "command_dark_blocked", cmdline, "denied", -1, "high", 0);
        return SECURITY_WIRE_DENY;
    }

    /* 2) 执行门（execution_gate.c：注入过滤 / 绝对保护 / 配置模式） */
    char why[WIRE_REASON_MAX] = {0};
    gate_result_t g = execution_gate_check(GATE_SOURCE_AI_TOOL, cmdline, "security_wire",
                                           why, sizeof(why));
    if (g == GATE_RESULT_DENY) {
        if (reason && reason_len) {
            safe_strncpy(reason, why[0] ? why : "denied by execution gate", reason_len);
        }
        audit_log("system", "security_wire", "command_denied", cmdline, why[0] ? why : "denied",
                  -1, "high", 0);
        return SECURITY_WIRE_DENY;
    }

    /* 3) 异常行为升级：AI 行为评分 ≥ 阈值 → 强制二次确认，绝不静默放行
     *    （defense_check_anomaly ← behavior_monitor 实时 EWMA 评分） */
    char anomaly_report[192] = {0};
    int anomalous = (defense_check_anomaly("ai", "command",
                                           anomaly_report, sizeof(anomaly_report)) == 1);
    if (anomalous) {
        LOG_WARN_T("SecurityWire", "Command", "Anomaly", "%s | %.80s", anomaly_report, cmdline);
    }

    /* 4) 高危命令 / 执行门要求确认 / 行为异常 → 二次验证（verify/second_verify.c） */
    if (g == GATE_RESULT_NEED_CONFIRM || wire_is_high_risk(cmdline) || anomalous) {
        if (!security_wire_confirm_high_risk(cmdline)) {
            if (reason && reason_len) safe_strncpy(reason, "second verification denied", reason_len);
            return SECURITY_WIRE_DENY;
        }
        return SECURITY_WIRE_ALLOW;
    }

    return SECURITY_WIRE_ALLOW;
}

int security_wire_confirm_high_risk(const char *operation) {
    const char *op = (operation && *operation) ? operation : "high-risk operation";

    if (!isatty(STDIN_FILENO)) {
        /* 非交互（daemon / 管道 / cron）：无法弹确认框。
         *   · 未设置 root 密码 → 视为未配置二次验证：放行 + 审计；
         *   · 已设置 root 密码 → fail-closed 拒绝（有策略却无法满足）。 */
        int has_pw = second_verify_has_password();
        LOG_WARN_T("SecurityWire", "Confirm", "NonInteractive",
                   "op='%s' has_password=%d → %s", op, has_pw, has_pw ? "DENY" : "ALLOW");
        audit_log("system", "security_wire", "confirm_noninteractive", op,
                  has_pw ? "denied_no_tty" : "allowed_no_password",
                  has_pw ? -1 : 0, "high", has_pw ? 0 : 1);
        return has_pw ? 0 : 1;
    }

    verify_result_t vr = second_verify_quick(op);
    int approved = (vr == VERIFY_RESULT_APPROVED);
    audit_log("system", "security_wire", "confirm", op, second_verify_result_str(vr),
              approved ? 0 : -1, "high", approved ? 1 : 0);
    return approved;
}

/* ============================================================
 * 输出面 / 影子模式数据闸门
 * ============================================================ */

const char *security_wire_redact_output(const char *output) {
    /* 线程局部缓冲：本函数可能被多个 handler 线程并发调用 */
    static __thread char tls_buf[8192];

    if (!output) return "";

    /* 影子模式：对外数据一律空（与 shadow_mode.c 语义一致，避免真实数据外泄） */
    if (defense_mode_get() == DEFENSE_MODE_SHADOW) {
        LOG_DEBUG_T("SecurityWire", "Redact", "Shadow", "shadow mode — empty output");
        return shadow_mode_get_empty_data("string");
    }

    if (output_filter_redact_r(output, tls_buf, sizeof(tls_buf)) != 0) {
        return "";
    }
    return tls_buf;
}

int security_wire_shadow_query(const char *app_id, const char *perm,
                               const char *type, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';

    if (!shadow_mode_should_return_empty(app_id, perm)) return 0;

    const char *empty = shadow_mode_get_empty_data(type);
    safe_strncpy(out, empty, out_len);
    LOG_INFO_T("SecurityWire", "ShadowQuery", "Empty",
               "app='%s' perm='%s' type='%s' → '%s'",
               app_id ? app_id : "(null)", perm ? perm : "(null)",
               type ? type : "(null)", out);
    audit_log("system", "security_wire", "shadow_empty_data",
              perm ? perm : "(null)", "shadow", 0, "medium", 1);
    return 1;
}

/* ============================================================
 * 防火墙 / 病毒扫描
 * ============================================================ */

int security_wire_firewall_block(const char *host, const char *port) {
    if (!firewall_is_valid_target(host)) {
        LOG_WARN_T("SecurityWire", "Firewall", "InvalidTarget",
                   "rejected '%s'", host ? host : "(null)");
        return -1;
    }
    int rc = firewall_deny(host, port);
    audit_log("system", "security_wire", "firewall_block", host,
              rc == 0 ? "success" : "failed", rc, "high", rc == 0 ? 1 : 0);
    return rc;
}

int security_wire_scan_package(const char *path, char *reason, size_t reason_len) {
    if (reason && reason_len) reason[0] = '\0';
    if (!path || !*path) {
        if (reason && reason_len) safe_strncpy(reason, "missing package path", reason_len);
        return -1;
    }

    scan_result_t r = virus_scanner_scan_package(path);
    if (reason && reason_len) safe_strncpy(reason, virus_scanner_result_name(r), reason_len);

    LOG_INFO_T("SecurityWire", "ScanPackage", "Result", "path='%s' result=%s",
               path, virus_scanner_result_name(r));
    audit_log("system", "security_wire", "package_scan", path,
              virus_scanner_result_name(r), r == SCAN_RESULT_MALICIOUS ? -1 : 0,
              "high", r == SCAN_RESULT_CLEAN ? 1 : 0);

    if (r == SCAN_RESULT_MALICIOUS) return -1;
    if (r == SCAN_RESULT_SUSPICIOUS) return 1;
    return 0;
}

/* ============================================================
 * 状态
 * ============================================================ */

const char *security_wire_mode_name(void) {
    return defense_mode_name(defense_mode_get());
}
