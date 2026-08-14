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

int is_in_proot(void) {
    /* 检查 proot 特征文件 */
    if (access("/proc/self/root", F_OK) == 0) {
        /* proot 中 /proc/self/root 通常指向容器根目录 */
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