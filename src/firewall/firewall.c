/**
 * @file    firewall.c
 * @brief   防火墙管理（封装 iptables/nftables 命令）
 * @version 2.0.0.0
 */

#include "firewall.h"
#include "../common/lang.h"
#include "log_extra.h"
#include "uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <arpa/inet.h>

/* ============================================================
 * 【P5 接线上岗 + 加固】目标校验
 *   本模块此前零调用者，且把 host 原样拼进 system() 命令串：
 *   firewall_deny("8.8.8.8; rm -rf /", ...) 即命令注入（OWASP A03）。
 *   现接入真实调用链（permission_check_network 拒绝 → 封堵）前先做严格校验：
 *   · 地址：inet_pton 解析（IPv4/IPv6），可选 "/前缀"（0..32 / 0..128）
 *   · 端口：纯数字且 1..65535
 * ============================================================ */

int firewall_is_valid_target(const char *ip_or_cidr) {
    if (!ip_or_cidr || !*ip_or_cidr) return 0;

    char buf[128];
    if (strlen(ip_or_cidr) >= sizeof(buf)) return 0;
    strcpy(buf, ip_or_cidr);

    int prefix = -1;
    char *slash = strchr(buf, '/');
    if (slash) {
        *slash = '\0';
        const char *digits = slash + 1;
        if (!*digits) return 0;
        for (const char *q = digits; *q; q++) {
            if (!isdigit((unsigned char)*q)) return 0;
        }
        prefix = atoi(digits);
        if (prefix < 0) return 0;
    }
    if (!*buf) return 0;

    int family = strchr(buf, ':') ? AF_INET6 : AF_INET;
    unsigned char addr[16];
    if (inet_pton(family, buf, addr) != 1) return 0;

    int max_prefix = (family == AF_INET) ? 32 : 128;
    if (prefix > max_prefix) return 0;
    return 1;
}

static int is_valid_port(const char *port) {
    if (!port || !*port) return 0;
    size_t len = strlen(port);
    if (len > 5) return 0;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)port[i])) return 0;
    }
    long p = strtol(port, NULL, 10);
    return (p >= 1 && p <= 65535);
}

static int run_command(const char *cmd) {
    int ret = system(cmd);
    if (ret != 0) {
        LOG_WARN_T("Firewall", "Cmd", "Fail", "cmd=%s ret=%d", cmd, ret);
    }
    return ret;
}

int firewall_allow(const char *ip_or_cidr, const char *port) {
    if (!firewall_is_valid_target(ip_or_cidr)) {
        LOG_WARN_T("Firewall", "Allow", "InvalidTarget", "rejected target '%s'",
                   ip_or_cidr ? ip_or_cidr : "(null)");
        return -1;
    }
    if (port && *port && !is_valid_port(port)) {
        LOG_WARN_T("Firewall", "Allow", "InvalidPort", "rejected port '%s'", port);
        return -1;
    }
    char cmd[512];
    if (port && *port) {
        snprintf(cmd, sizeof(cmd), "iptables -A INPUT -s %s -p tcp --dport %s -j ACCEPT 2>/dev/null", ip_or_cidr, port);
    } else {
        snprintf(cmd, sizeof(cmd), "iptables -A INPUT -s %s -j ACCEPT 2>/dev/null", ip_or_cidr);
    }
    int ret = run_command(cmd);
    if (ret == 0) {
        LOG_INFO_T("Firewall", "Allow", "OK", "%s port=%s", ip_or_cidr, port ? port : "any");
        uart_puts(tr("Firewall rule added.\n", "防火墙规则已添加。\n"));
    } else {
        /* 尝试 nftables */
        if (port && *port) {
            snprintf(cmd, sizeof(cmd), "nft add rule ip filter input ip saddr %s tcp dport %s accept 2>/dev/null", ip_or_cidr, port);
        } else {
            snprintf(cmd, sizeof(cmd), "nft add rule ip filter input ip saddr %s accept 2>/dev/null", ip_or_cidr);
        }
        ret = run_command(cmd);
        if (ret == 0) {
            LOG_INFO_T("Firewall", "Allow", "OK (nft)", "%s", ip_or_cidr);
            uart_puts(tr("Firewall rule added (nftables).\n", "防火墙规则已添加（nftables）。\n"));
        } else {
            uart_puts(tr("Failed to add firewall rule. Please check iptables/nftables.\n", "添加防火墙规则失败。请检查 iptables/nftables。\n"));
        }
    }
    return ret;
}

int firewall_deny(const char *ip_or_cidr, const char *port) {
    if (!firewall_is_valid_target(ip_or_cidr)) {
        LOG_WARN_T("Firewall", "Deny", "InvalidTarget", "rejected target '%s'",
                   ip_or_cidr ? ip_or_cidr : "(null)");
        return -1;
    }
    if (port && *port && !is_valid_port(port)) {
        LOG_WARN_T("Firewall", "Deny", "InvalidPort", "rejected port '%s'", port);
        return -1;
    }
    char cmd[512];
    if (port && *port) {
        snprintf(cmd, sizeof(cmd), "iptables -A INPUT -s %s -p tcp --dport %s -j DROP 2>/dev/null", ip_or_cidr, port);
    } else {
        snprintf(cmd, sizeof(cmd), "iptables -A INPUT -s %s -j DROP 2>/dev/null", ip_or_cidr);
    }
    int ret = run_command(cmd);
    if (ret == 0) {
        LOG_INFO_T("Firewall", "Deny", "OK", "%s", ip_or_cidr);
        uart_puts(tr("Firewall deny rule added.\n", "防火墙拒绝规则已添加。\n"));
    } else {
        if (port && *port) {
            snprintf(cmd, sizeof(cmd), "nft add rule ip filter input ip saddr %s tcp dport %s drop 2>/dev/null", ip_or_cidr, port);
        } else {
            snprintf(cmd, sizeof(cmd), "nft add rule ip filter input ip saddr %s drop 2>/dev/null", ip_or_cidr);
        }
        ret = run_command(cmd);
        if (ret == 0) {
            LOG_INFO_T("Firewall", "Deny", "OK (nft)", "%s", ip_or_cidr);
            uart_puts(tr("Firewall deny rule added (nftables).\n", "防火墙拒绝规则已添加（nftables）。\n"));
        } else {
            uart_puts(tr("Failed to add deny rule.\n", "添加拒绝规则失败。\n"));
        }
    }
    return ret;
}

void firewall_list(void) {
    uart_puts(tr("=== iptables INPUT chain ===\n", "=== iptables INPUT 链 ===\n"));
    system("iptables -L INPUT -n 2>/dev/null | head -20");
    uart_puts(tr("=== nftables (if available) ===\n", "=== nftables（如果可用）===\n"));
    system("nft list ruleset 2>/dev/null | head -20");
}

int firewall_flush(void) {
    int ret = run_command("iptables -F INPUT 2>/dev/null");
    if (ret != 0) {
        ret = run_command("nft flush ruleset 2>/dev/null");
    }
    if (ret == 0) {
        LOG_INFO_T("Firewall", "Flush", "OK", "all rules cleared");
        uart_puts(tr("All firewall rules cleared.\n", "所有防火墙规则已清除。\n"));
    } else {
        uart_puts(tr("Failed to clear firewall rules.\n", "清除防火墙规则失败。\n"));
    }
    return ret;
}