/**
 * @file    pkg_deps.c
 * @brief   依赖解析（递归读取 .deb 的 Depends 字段）
 * @version 2.0.0.0
 */

#include "pkg_deps.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define TEMP_DIR_TEMPLATE "/tmp/deps_XXXXXX"

/* 提取单个 .deb 的依赖列表 */
static char *extract_depends(const char *deb_path) {
    char temp_dir[] = TEMP_DIR_TEMPLATE;
    if (!mkdtemp(temp_dir)) {
        LOG_ERROR_T("PkgDeps", "Extract", "MkdtempFail", "");
        return NULL;
    }
    /* 解压 control 部分 */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "ar x '%s' --output='%s' 2>/dev/null", deb_path, temp_dir);
    if (system(cmd) != 0) {
        rmdir(temp_dir);
        return NULL;
    }
    /* 查找 control.tar.* */
    char control_tar[1024] = {0};
    snprintf(cmd, sizeof(cmd), "find '%s' -name 'control.tar.*' -type f | head -1", temp_dir);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        if (fgets(control_tar, sizeof(control_tar), fp)) {
            char *nl = strchr(control_tar, '\n');
            if (nl) *nl = '\0';
        }
        pclose(fp);
    }
    if (control_tar[0] == '\0') {
        rmdir(temp_dir);
        return NULL;
    }
    /* 解压 control.tar.* */
    char control_extract[1024];
    snprintf(control_extract, sizeof(control_extract), "%s/control_extract", temp_dir);
    mkdir(control_extract, 0755);
    snprintf(cmd, sizeof(cmd), "tar -xf '%s' -C '%s' 2>/dev/null", control_tar, control_extract);
    system(cmd);
    /* 读取 control 文件 */
    char control_file[1024];
    snprintf(control_file, sizeof(control_file), "%s/control", control_extract);
    fp = fopen(control_file, "r");
    if (!fp) {
        rmdir(temp_dir);
        return NULL;
    }
    char line[512];
    char *depends = NULL;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Depends:", 8) == 0) {
            char *p = line + 8;
            while (*p == ' ') p++;
            int len = strlen(p);
            if (len > 0 && p[len-1] == '\n') p[len-1] = '\0';
            depends = strdup(p);
            break;
        }
    }
    fclose(fp);
    /* 清理 */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", temp_dir);
    system(cmd);
    return depends;
}

/* 递归解析依赖（简单实现，不解决循环依赖）*/
char *pkg_resolve_deps(const char *package_path) {
    if (!package_path) return NULL;
    char *depends = extract_depends(package_path);
    if (!depends) return NULL;
    /* 简化：返回原始依赖字符串，不递归下载（后续由下载函数处理）*/
    LOG_DEBUG_T("PkgDeps", "Resolve", "Deps", "%s", depends);
    return depends;
}

/* 下载依赖包（使用 apt-get download）*/
int pkg_download_deps(const char *deps_list) {
    if (!deps_list || !*deps_list) return 0;
    /* 创建临时目录 */
    char temp_dir[] = TEMP_DIR_TEMPLATE;
    if (!mkdtemp(temp_dir)) return -1;
    /* 解析依赖列表（逗号分隔）*/
    char *deps = strdup(deps_list);
    char *saveptr;
    char *pkg = strtok_r(deps, ",", &saveptr);
    int success = 0;
    while (pkg) {
        /* 跳过版本约束（如 "libc6 (>= 2.0)" 只取包名）*/
        char *space = strchr(pkg, ' ');
        if (space) *space = '\0';
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "cd '%s' && apt-get download '%s' 2>/dev/null", temp_dir, pkg);
        if (system(cmd) == 0) {
            success++;
        } else {
            LOG_WARN_T("PkgDeps", "Download", "Fail", "package=%s", pkg);
        }
        pkg = strtok_r(NULL, ",", &saveptr);
    }
    free(deps);
    /* 清理临时目录 */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", temp_dir);
    system(cmd);
    return success > 0 ? 0 : -1;
}