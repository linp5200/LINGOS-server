/**
 * @file    proot_detect.c
 * @brief   proot 环境检测实现
 * @version LN-B-3.8.0.0
 */

#include "proot_detect.h"
#include "../common/safe_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

int is_in_proot(void) {
    /* 【修复】原来只判断 /proc/self/root 是否存在——真实 Linux 上它必然存在
     *   （指向 "/"），于是恒返回 1。受害方：crash_dump.c:205 因此永远走 proot
     *   分支，用 /proc/version 代替 dmesg，真机崩溃转储丢失内核日志。
     *   正确判据：proot 会把根伪装成别处（如 termux 的 rootfs 路径），
     *   即 readlink(/proc/self/root) != "/"。 */
    char root_link[512];
    ssize_t n = readlink("/proc/self/root", root_link, sizeof(root_link) - 1);
    if (n > 0) {
        root_link[n] = '\0';
        if (strcmp(root_link, "/") != 0) return 1;
    } else if (access("/proc/self/root", F_OK) != 0) {
        /* 连 /proc 都不可读：环境异常，按 proot/受限环境处理 */
        return 1;
    }
    /* 检查 Termux 特征（proot-distro 常用） */
    if (access("/data/data/com.termux", F_OK) == 0) {
        return 1;
    }
    /* 检查环境变量 */
    if (getenv("PROOT") != NULL) {
        return 1;
    }
    return 0;
}

int get_proot_kernel_version(char *buf, size_t size) {
    if (!buf || size == 0) return -1;
    FILE *fp = fopen("/proc/version", "r");
    if (fp) {
        if (fgets(buf, size, fp)) {
            fclose(fp);
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            return 0;
        }
        fclose(fp);
    }
    safe_snprintf(buf, size, "Unknown (proot environment)");
    return -1;
}

int safe_run_command(const char *cmd, char *output, size_t size) {
    if (!cmd || !output || size == 0) return -1;
    char full_cmd[1024];
    safe_snprintf(full_cmd, sizeof(full_cmd), "%s 2>/dev/null", cmd);
    FILE *pipe = popen(full_cmd, "r");
    if (!pipe) return -1;
    size_t len = fread(output, 1, size - 1, pipe);
    output[len] = '\0';
    pclose(pipe);
    return (len > 0) ? 0 : -1;
}