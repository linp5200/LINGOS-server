/**
 * @file    app_sandbox_cgroup.c
 * @brief   cgroup v2 资源限制（CPU、内存、Swap、进程数）
 * @version LN-B-4.2.0.0
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>

#include "log_extra.h"
#include "data_path.h"
#include "safe_string.h"

/* ============================================================
 * 默认资源限制配置
 * ============================================================ */

#define CGROUP_BASE_PATH "/sys/fs/cgroup"
#define CGROUP_LINGOS_PATH "/sys/fs/cgroup/lingos"
#define DEFAULT_CPU_PERCENT 50
#define DEFAULT_MEMORY_MB 256
#define DEFAULT_SWAP_MB 512
#define DEFAULT_PIDS_MAX 10

static int g_cgroup_initialized = 0;

/* ============================================================
 * 内部辅助：递归创建目录
 * ============================================================ */

static int mkdir_p(const char *path) {
    LOG_DEBUG_T("Cgroup", "MkdirP", "Enter", "path='%s'", path ? path : "(null)");

    if (!path) {
        LOG_ERROR_T("Cgroup", "MkdirP", "Invalid", "path is NULL");
        return -1;
    }

    char tmp[512];
    char *p = NULL;
    size_t len;

    safe_snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                LOG_WARN_T("Cgroup", "MkdirP", "Fail", "mkdir %s: %s (errno=%d)",
                           tmp, strerror(errno), errno);
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        LOG_WARN_T("Cgroup", "MkdirP", "Fail", "mkdir %s: %s (errno=%d)",
                   tmp, strerror(errno), errno);
        return -1;
    }

    LOG_DEBUG_T("Cgroup", "MkdirP", "OK", "created '%s'", path);
    return 0;
}

/* ============================================================
 * 内部辅助：写入 cgroup 控制文件
 * ============================================================ */

static int write_cgroup_file(const char *path, const char *value) {
    LOG_DEBUG_T("Cgroup", "WriteFile", "Enter", "path='%s', value='%s'",
                path ? path : "(null)", value ? value : "(null)");

    if (!path || !value) {
        LOG_ERROR_T("Cgroup", "WriteFile", "Invalid", "path=%p, value=%p", (void*)path, (void*)value);
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_WARN_T("Cgroup", "WriteFile", "OpenFail", "cannot open %s: %s (errno=%d)",
                   path, strerror(errno), errno);
        return -1;
    }

    fprintf(fp, "%s\n", value);
    fclose(fp);

    LOG_DEBUG_T("Cgroup", "WriteFile", "OK", "wrote '%s' to %s", value, path);
    return 0;
}

/* ============================================================
 * 内部辅助：检查 cgroup v2 是否可用
 * ============================================================ */

static int is_cgroup_v2_available(void) {
    LOG_DEBUG_T("Cgroup", "CheckV2", "Enter", "checking cgroup v2 availability");

    /* 检查 /sys/fs/cgroup/cgroup.controllers 是否存在 */
    if (access("/sys/fs/cgroup/cgroup.controllers", F_OK) != 0) {
        LOG_DEBUG_T("Cgroup", "CheckV2", "NotAvailable", "cgroup v2 not available");
        return 0;
    }

    /* 检查是否包含 cpu 和 memory 控制器 */
    FILE *fp = fopen("/sys/fs/cgroup/cgroup.controllers", "r");
    if (!fp) {
        LOG_WARN_T("Cgroup", "CheckV2", "OpenFail", "cannot read cgroup.controllers");
        return 0;
    }

    char line[256];
    int has_cpu = 0, has_memory = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "cpu")) has_cpu = 1;
        if (strstr(line, "memory")) has_memory = 1;
    }
    fclose(fp);

    int available = (has_cpu && has_memory);
    LOG_DEBUG_T("Cgroup", "CheckV2", "Result", "cpu=%d, memory=%d, available=%d",
                has_cpu, has_memory, available);
    return available;
}

/* ============================================================
 * 公共 API：初始化 cgroup
 * ============================================================ */

int app_sandbox_cgroup_init(void) {
    LOG_INFO_T("Cgroup", "Init", "Enter", "initializing cgroup v2");

    if (g_cgroup_initialized) {
        LOG_DEBUG_T("Cgroup", "Init", "Already", "cgroup already initialized");
        return 0;
    }

    if (!is_cgroup_v2_available()) {
        LOG_WARN_T("Cgroup", "Init", "NotAvailable", "cgroup v2 not available, skipping");
        return 0;
    }

    /* 创建 lingos 根 cgroup */
    if (mkdir_p(CGROUP_LINGOS_PATH) != 0) {
        LOG_ERROR_T("Cgroup", "Init", "MkdirFail", "cannot create %s", CGROUP_LINGOS_PATH);
        return -1;
    }

    g_cgroup_initialized = 1;
    LOG_INFO_T("Cgroup", "Init", "OK", "cgroup v2 initialized at %s", CGROUP_LINGOS_PATH);
    return 0;
}

/* ============================================================
 * 公共 API：应用 cgroup 资源限制
 * ============================================================ */

int app_sandbox_apply_cgroup(pid_t pid) {
    LOG_INFO_T("Cgroup", "Apply", "Enter", "pid=%d", pid);

    if (pid <= 0) {
        LOG_ERROR_T("Cgroup", "Apply", "Invalid", "pid=%d", pid);
        return -1;
    }

    if (!g_cgroup_initialized) {
        if (app_sandbox_cgroup_init() != 0) {
            LOG_ERROR_T("Cgroup", "Apply", "InitFail", "cgroup initialization failed");
            return -1;
        }
    }

    if (!is_cgroup_v2_available()) {
        LOG_DEBUG_T("Cgroup", "Apply", "Skip", "cgroup v2 not available");
        return -1;
    }

    /* 为每个进程创建独立的子 cgroup（基于 PID） */
    char cgroup_path[512];
    char app_name[64];
    safe_snprintf(app_name, sizeof(app_name), "app_%d", pid);
    safe_snprintf(cgroup_path, sizeof(cgroup_path), "%s/%s", CGROUP_LINGOS_PATH, app_name);

    LOG_DEBUG_T("Cgroup", "Apply", "Cgroup", "creating %s", cgroup_path);

    if (mkdir_p(cgroup_path) != 0) {
        LOG_ERROR_T("Cgroup", "Apply", "MkdirFail", "cannot create %s", cgroup_path);
        return -1;
    }

    /* ====== 1. CPU 限制 ====== */
    char cpu_max_path[512];
    safe_snprintf(cpu_max_path, sizeof(cpu_max_path), "%s/cpu.max", cgroup_path);
    char cpu_value[32];
    safe_snprintf(cpu_value, sizeof(cpu_value), "%d0000 100000", DEFAULT_CPU_PERCENT);

    if (write_cgroup_file(cpu_max_path, cpu_value) != 0) {
        LOG_WARN_T("Cgroup", "Apply", "CPUWarn", "failed to set CPU limit");
    } else {
        LOG_DEBUG_T("Cgroup", "Apply", "CPU", "CPU limit: %d%%", DEFAULT_CPU_PERCENT);
    }

    /* ====== 2. 内存限制 ====== */
    char mem_max_path[512];
    safe_snprintf(mem_max_path, sizeof(mem_max_path), "%s/memory.max", cgroup_path);
    char mem_value[32];
    safe_snprintf(mem_value, sizeof(mem_value), "%d", DEFAULT_MEMORY_MB * 1024 * 1024);

    if (write_cgroup_file(mem_max_path, mem_value) != 0) {
        LOG_WARN_T("Cgroup", "Apply", "MemWarn", "failed to set memory limit");
    } else {
        LOG_DEBUG_T("Cgroup", "Apply", "Memory", "memory limit: %d MB", DEFAULT_MEMORY_MB);
    }

    /* ====== 3. Swap 限制 ====== */
    char swap_max_path[512];
    safe_snprintf(swap_max_path, sizeof(swap_max_path), "%s/memory.swap.max", cgroup_path);
    char swap_value[32];
    safe_snprintf(swap_value, sizeof(swap_value), "%d", DEFAULT_SWAP_MB * 1024 * 1024);

    if (write_cgroup_file(swap_max_path, swap_value) != 0) {
        LOG_WARN_T("Cgroup", "Apply", "SwapWarn", "failed to set swap limit (may not be supported)");
    } else {
        LOG_DEBUG_T("Cgroup", "Apply", "Swap", "swap limit: %d MB", DEFAULT_SWAP_MB);
    }

    /* ====== 4. 进程数限制 ====== */
    char pids_max_path[512];
    safe_snprintf(pids_max_path, sizeof(pids_max_path), "%s/pids.max", cgroup_path);
    char pids_value[16];
    safe_snprintf(pids_value, sizeof(pids_value), "%d", DEFAULT_PIDS_MAX);

    if (write_cgroup_file(pids_max_path, pids_value) != 0) {
        LOG_WARN_T("Cgroup", "Apply", "PidsWarn", "failed to set pids limit");
    } else {
        LOG_DEBUG_T("Cgroup", "Apply", "Pids", "pids limit: %d", DEFAULT_PIDS_MAX);
    }

    /* ====== 5. 将进程添加到 cgroup ====== */
    char procs_path[512];
    safe_snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);
    char pid_str[16];
    safe_snprintf(pid_str, sizeof(pid_str), "%d", pid);

    if (write_cgroup_file(procs_path, pid_str) != 0) {
        LOG_ERROR_T("Cgroup", "Apply", "AddProcsFail", "failed to add pid %d to cgroup", pid);
        return -1;
    }

    LOG_INFO_T("Cgroup", "Apply", "OK", "cgroup limits applied for pid %d (CPU=%d%%, MEM=%dMB, SWAP=%dMB, PIDS=%d)",
               pid, DEFAULT_CPU_PERCENT, DEFAULT_MEMORY_MB, DEFAULT_SWAP_MB, DEFAULT_PIDS_MAX);
    return 0;
}