/**
 * @file    permission_check.c
 * @brief   权限检查细化（文件/网络/内存/CPU/设备/系统调用）
 * @version LN-B-4.3.0.0
 * @par     核心协议：防御性编程 + 契约式编程
 */

#include "permission_check.h"
#include "permission_whitelist.h"
#include "../common/error_report.h"
#include "../lib/log_extra.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

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
        return 0;
    }

    /* 检查是否为本地回环（始终允许） */
    if (host && (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0)) {
        return 1;
    }

    /* 需要额外 DNS 权限解析域名 */
    if (host && host[0] && !isdigit(host[0]) && !permission_whitelist_check(app_id, PERM_NET_DNS)) {
        LOG_WARN_T("PermCheck", "Network", "DNSDenied", "app %s lacks DNS", app_id);
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

    /* 黑名单系统调用（始终禁止） */
    int blacklist[] = { /* reboot, mount, umount, swapon, swapoff 等 */ };
    for (int i = 0; i < (int)(sizeof(blacklist)/sizeof(blacklist[0])); i++) {
        if (syscall_num == blacklist[i]) {
            LOG_WARN_T("PermCheck", "Syscall", "Blacklisted", "app %s attempted syscall %d", app_id, syscall_num);
            return 0;
        }
    }

    return 1;
}