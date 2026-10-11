/**
 * @file    permission_check.c
 * @brief   权限检查细化（文件/网络/内存/CPU/设备/系统调用）
 * @version LN-B-4.3.0.0
 * @par     核心协议：防御性编程 + 契约式编程
 */

#include "permission_check.h"
#include "permission_whitelist.h"
#include "../firewall/firewall.h"   /* 【P5 接线】网络权限拒绝 → 防火墙真实封堵 */
#include "../common/error_report.h"
#include "../lib/log_extra.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <pwd.h>
#include <sys/syscall.h>   /* 【2026-10-10】黑名单用 __NR_* 宏（跨架构正确） */

/* ============================================================
 * 【P5 接线上岗】权限拒绝 → 防火墙封堵（firewall.c 此前零调用者）
 *
 * 语义：连接被权限系统拒绝时，仅靠「拒绝本次调用」挡不住 App 换路径重试；
 *       此处把被拒目标下成 INPUT DROP 规则，实现「一次拒绝，持续封堵」。
 * 约束：① 仅对合法 IPv4/IPv6 字面量+C（域名不做 -s 规则，避免 nft 报错）
 *       ② 同一目标只下一次规则（避免每次连接都 fork iptables）
 *       ③ 防火墙失败不影响权限判定结果（fail-closed 仍由返回值 0 保证）
 * ============================================================ */

#define FW_BLOCK_MAX 16

static char g_fw_blocked[FW_BLOCK_MAX][64];
static int g_fw_blocked_count = 0;
static pthread_mutex_t g_fw_lock = PTHREAD_MUTEX_INITIALIZER;

static void firewall_enforce_deny(const char *host, int port) {
    if (!host || !firewall_is_valid_target(host)) return;

    pthread_mutex_lock(&g_fw_lock);
    int seen = 0;
    for (int i = 0; i < g_fw_blocked_count; i++) {
        if (strcmp(g_fw_blocked[i], host) == 0) { seen = 1; break; }
    }
    if (!seen && g_fw_blocked_count < FW_BLOCK_MAX) {
        snprintf(g_fw_blocked[g_fw_blocked_count], sizeof(g_fw_blocked[0]), "%s", host);
        g_fw_blocked_count++;
    }
    pthread_mutex_unlock(&g_fw_lock);

    if (seen) return;

    char port_str[8];
    if (port > 0 && port <= 65535) snprintf(port_str, sizeof(port_str), "%d", port);
    else port_str[0] = '\0';

    int rc = firewall_deny(host, port_str[0] ? port_str : NULL);
    LOG_WARN_T("PermCheck", "Firewall", rc == 0 ? "Blocked" : "BlockFail",
               "denied target %s port %d enforced via firewall (rc=%d)",
               host, port, rc);
}

/* ============================================================
 * 文件权限检查
 * ============================================================ */

int permission_check_file(const char *app_id, const char *path, int mode) {
    LOG_DEBUG_T("PermCheck", "File", "Enter", "app=%s, path=%s, mode=0x%x",
                app_id ? app_id : "(null)", path ? path : "(null)", mode);

    if (!app_id || !*app_id || !path || !*path) {
        return 0;
    }

    /* 检查是否有对应文件权限 */
    permission_type_t needed_perm;
    if (mode & W_OK) {
        needed_perm = PERM_FILE_WRITE;
    } else if (mode & X_OK) {
        needed_perm = PERM_FILE_EXEC;
    } else {
        needed_perm = PERM_FILE_READ;
    }

    if (!permission_whitelist_check(app_id, needed_perm)) {
        LOG_WARN_T("PermCheck", "File", "Denied", "app %s lacks %s for %s",
                   app_id, permission_type_to_string(needed_perm), path);
        return 0;
    }

    /* 路径白名单检查（防止访问 /LINGOS/system/config/ 以外） */
    if (strstr(path, "/LINGOS/system/config/") &&
        !permission_whitelist_check(app_id, PERM_SYSCALL)) {
        LOG_WARN_T("PermCheck", "File", "ConfigAccess", "app %s cannot access config", app_id);
        return 0;
    }

    return 1;
}

/* ============================================================
 * 网络权限检查
 * ============================================================ */

int permission_check_network(const char *app_id, const char *host, int port, int is_outgoing) {
    LOG_DEBUG_T("PermCheck", "Network", "Enter", "app=%s, host=%s, port=%d, outgoing=%d",
                app_id ? app_id : "(null)", host ? host : "(null)", port, is_outgoing);

    if (!app_id || !*app_id) return 0;

    permission_type_t needed_perm = is_outgoing ? PERM_NET_CONNECT : PERM_NET_BIND;
    if (!permission_whitelist_check(app_id, needed_perm)) {
        LOG_WARN_T("PermCheck", "Network", "Denied", "app %s lacks %s",
                   app_id, permission_type_to_string(needed_perm));
        firewall_enforce_deny(host, port);   /* 【P5 接线】拒绝即封堵 */
        return 0;
    }

    /* 检查是否为本地回环（始终允许） */
    if (host && (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0)) {
        return 1;
    }

    /* 需要额外 DNS 权限解析域名 */
    if (host && host[0] && !isdigit(host[0]) && !permission_whitelist_check(app_id, PERM_NET_DNS)) {
        LOG_WARN_T("PermCheck", "Network", "DNSDenied", "app %s lacks DNS", app_id);
        firewall_enforce_deny(host, port);   /* 【P5 接线】拒绝即封堵（域名会被校验拒绝，非 IP 直接跳过） */
        return 0;
    }

    return 1;
}

/* ============================================================
 * 内存权限检查
 * ============================================================ */

int permission_check_memory(const char *app_id, size_t size, int is_mmap) {
    LOG_DEBUG_T("PermCheck", "Memory", "Enter", "app=%s, size=%zu, mmap=%d",
                app_id ? app_id : "(null)", size, is_mmap);

    if (!app_id || !*app_id) return 0;

    permission_type_t needed_perm = is_mmap ? PERM_MEM_MAP : PERM_MEM_ALLOC;
    if (!permission_whitelist_check(app_id, needed_perm)) {
        LOG_WARN_T("PermCheck", "Memory", "Denied", "app %s lacks %s",
                   app_id, permission_type_to_string(needed_perm));
        return 0;
    }

    /* 限制最大分配量（从配置读取，这里使用默认值） */
    if (size > 256 * 1024 * 1024) { /* 256MB */
        LOG_WARN_T("PermCheck", "Memory", "SizeExceeded", "app %s requested %zu MB",
                   app_id, size / (1024 * 1024));
        return 0;
    }

    return 1;
}

/* ============================================================
 * CPU 权限检查
 * ============================================================ */

int permission_check_cpu(const char *app_id, int cpu_percent, int priority) {
    LOG_DEBUG_T("PermCheck", "CPU", "Enter", "app=%s, percent=%d, priority=%d",
                app_id ? app_id : "(null)", cpu_percent, priority);

    if (!app_id || !*app_id) return 0;

    if (!permission_whitelist_check(app_id, PERM_CPU_QUOTA)) {
        LOG_WARN_T("PermCheck", "CPU", "Denied", "app %s lacks CPU_QUOTA", app_id);
        return 0;
    }

    if (cpu_percent > 80) {
        LOG_WARN_T("PermCheck", "CPU", "QuotaExceeded", "app %s requested %d%% CPU", app_id, cpu_percent);
        return 0;
    }

    if (priority < -10 && !permission_whitelist_check(app_id, PERM_CPU_PRIORITY)) {
        LOG_WARN_T("PermCheck", "CPU", "PriorityDenied", "app %s lacks CPU_PRIORITY", app_id);
        return 0;
    }

    return 1;
}

/* ============================================================
 * 设备权限检查
 * ============================================================ */

int permission_check_device(const char *app_id, const char *device_type) {
    LOG_DEBUG_T("PermCheck", "Device", "Enter", "app=%s, device=%s",
                app_id ? app_id : "(null)", device_type ? device_type : "(null)");

    if (!app_id || !*app_id || !device_type || !*device_type) return 0;

    permission_type_t needed_perm = PERM_UNKNOWN;

    if (strcmp(device_type, "camera") == 0) needed_perm = PERM_CAMERA;
    else if (strcmp(device_type, "mic") == 0 || strcmp(device_type, "microphone") == 0) {
        needed_perm = PERM_MICROPHONE;
    } else if (strcmp(device_type, "bluetooth") == 0) needed_perm = PERM_BLUETOOTH;
    else if (strcmp(device_type, "usb") == 0) needed_perm = PERM_USB;
    else if (strcmp(device_type, "serial") == 0) needed_perm = PERM_SERIAL;
    else return 0;

    if (!permission_whitelist_check(app_id, needed_perm)) {
        LOG_WARN_T("PermCheck", "Device", "Denied", "app %s lacks %s for %s",
                   app_id, permission_type_to_string(needed_perm), device_type);
        return 0;
    }

    return 1;
}

/* ============================================================
 * 系统调用权限检查（seccomp 细化）
 * ============================================================ */

int permission_check_syscall(const char *app_id, int syscall_num) {
    LOG_DEBUG_T("PermCheck", "Syscall", "Enter", "app=%s, syscall=%d",
                app_id ? app_id : "(null)", syscall_num);

    if (!app_id || !*app_id) return 0;

    /* 只有拥有 SYSCALL 权限才能执行敏感系统调用 */
    if (!permission_whitelist_check(app_id, PERM_SYSCALL)) {
        LOG_WARN_T("PermCheck", "Syscall", "Denied", "app %s lacks SYSCALL", app_id);
        return 0;
    }

    /* 【2026-10-10 P0 补全】黑名单系统调用（始终禁止）
     * 此前 blacklist[] 为空数组——即使接上执行点也拦不住任何 syscall。
     * 用 __NR_* 宏（跨架构正确；seccomp 为第一道防线，此处为纵深第二道）。
     * 刻意只列"高危且正常业务不使用"的持久化/破坏类：*/
#ifdef __NR_reboot
    if (syscall_num == __NR_reboot)        { LOG_WARN_T("PermCheck","Syscall","Blacklisted","app %s attempted reboot", app_id);        return 0; }
#endif
#ifdef __NR_kexec_load
    if (syscall_num == __NR_kexec_load)    { LOG_WARN_T("PermCheck","Syscall","Blacklisted","app %s attempted kexec_load", app_id);    return 0; }
#endif
#ifdef __NR_init_module
    if (syscall_num == __NR_init_module)   { LOG_WARN_T("PermCheck","Syscall","Blacklisted","app %s attempted init_module", app_id);   return 0; }
#endif
#ifdef __NR_finit_module
    if (syscall_num == __NR_finit_module)  { LOG_WARN_T("PermCheck","Syscall","Blacklisted","app %s attempted finit_module", app_id);  return 0; }
#endif
#ifdef __NR_delete_module
    if (syscall_num == __NR_delete_module) { LOG_WARN_T("PermCheck","Syscall","Blacklisted","app %s attempted delete_module", app_id); return 0; }
#endif
#ifdef __NR_mount
    if (syscall_num == __NR_mount)         { LOG_WARN_T("PermCheck","Syscall","Blacklisted","app %s attempted mount", app_id);         return 0; }
#endif
#ifdef __NR_umount2
    if (syscall_num == __NR_umount2)       { LOG_WARN_T("PermCheck","Syscall","Blacklisted","app %s attempted umount2", app_id);       return 0; }
#endif
#ifdef __NR_pivot_root
    if (syscall_num == __NR_pivot_root)    { LOG_WARN_T("PermCheck","Syscall","Blacklisted","app %s attempted pivot_root", app_id);    return 0; }
#endif
#ifdef __NR_swapon
    if (syscall_num == __NR_swapon)        { LOG_WARN_T("PermCheck","Syscall","Blacklisted","app %s attempted swapon", app_id);        return 0; }
#endif
#ifdef __NR_swapoff
    if (syscall_num == __NR_swapoff)       { LOG_WARN_T("PermCheck","Syscall","Blacklisted","app %s attempted swapoff", app_id);       return 0; }
#endif

    return 1;
}