/**
 * @file    sync_template.c
 * @brief   开发者快速同步：补充缺失的目录和默认文件
 * @version LN-B-3.4.0.2
 * @changes 新增 /AH 相关目录支持
 */

#include "sync_template.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include "../common/string_no_sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

static const char *required_dirs[] = {
    "/system",
    "/system/config",
    "/system/config/custom",
    "/system/modules",
    "/system/backups",
    "/skills",
    "/skills/builtin",
    "/skills/custom",
    "/skills/store",
    "/data",
    "/data/ai_memory",
    "/data/ai_memory/ai_smemory",
    "/data/ai_memory/ai_mmemory",
    "/data/ai_memory/ai_lmemory",
    "/data/logs",
    "/data/shared",
    "/Ensystem",
    "/Ensystem/private",
    "/Ensystem/baseline",
    "/Ensystem/backups",
    "/Debug",
    "/Debug/log",
    "/state",
    "/state/components",
    "/apps",
    "/bin",
    "/run",
    "/backups",
    /* ====== 新增：帮助系统目录 ====== */
    "/AH",
    "/AH/builtin",
    "/AH/ai_generated",
    "/AH/user_created",
    NULL
};

static void ensure_version_file(const char *root) {
    char path[512];
    snprintf(path, sizeof(path), "%s/version", root);
    if (access(path, F_OK) != 0) {
        FILE *fp = fopen(path, "w");
        if (fp) {
#ifdef LINGOS_VERSION
            fprintf(fp, "%s\n", LINGOS_VERSION);
#else
            fprintf(fp, "LN-B-3.4.0.2\n");
#endif
            fclose(fp);
            LOG_DEBUG_T("Sync", "Version", "Create", "created %s", path);
        } else {
            LOG_ERROR_T("Sync", "Version", "Fail", "cannot create %s: %s", path, strerror(errno));
        }
    }
}

static void ensure_skill_index(const char *root) {
    char path[512];
    snprintf(path, sizeof(path), "%s/skills/index.json", root);
    if (access(path, F_OK) != 0) {
        FILE *fp = fopen(path, "w");
        if (fp) {
            /* 技能索引由Python管理，C端仅创建空占位 */
            fprintf(fp, "[]\n");
            fclose(fp);
            LOG_DEBUG_T("Sync", "SkillIndex", "Create", "created empty placeholder %s", path);
        } else {
            LOG_ERROR_T("Sync", "SkillIndex", "Fail", "cannot create %s: %s", path, strerror(errno));
        }
    }
}

static void ensure_memory_registry(const char *root) {
    char path[512];
    snprintf(path, sizeof(path), "%s/data/ai_memory/memory_registry.json", root);
    if (access(path, F_OK) != 0) {
        FILE *fp = fopen(path, "w");
        if (fp) {
            fprintf(fp, "{\"version\":\"1.0\",\"entries\":[]}\n");
            fclose(fp);
            LOG_DEBUG_T("Sync", "MemoryReg", "Create", "created %s", path);
        } else {
            LOG_ERROR_T("Sync", "MemoryReg", "Fail", "cannot create %s: %s", path, strerror(errno));
        }
    }
}

static void ensure_state_file(const char *root) {
    char path[512];
    snprintf(path, sizeof(path), "%s/Ensystem/state.json", root);
    if (access(path, F_OK) != 0) {
        FILE *fp = fopen(path, "w");
        if (fp) {
            fprintf(fp, "{\"system_configured\":false,\"last_config_time\":\"\",\"mode\":\"app\"}\n");
            fclose(fp);
            LOG_DEBUG_T("Sync", "StateFile", "Create", "created %s", path);
        } else {
            LOG_ERROR_T("Sync", "StateFile", "Fail", "cannot create %s: %s", path, strerror(errno));
        }
    }
}

static void ensure_passwd_file(const char *root) {
    char path[512];
    snprintf(path, sizeof(path), "%s/Ensystem/passwd", root);
    if (access(path, F_OK) != 0) {
        FILE *fp = fopen(path, "w");
        if (fp) {
            fprintf(fp, "root:\n");
            fclose(fp);
            LOG_DEBUG_T("Sync", "PasswdFile", "Create", "created %s", path);
        } else {
            LOG_ERROR_T("Sync", "PasswdFile", "Fail", "cannot create %s: %s", path, strerror(errno));
        }
    }
}

/* ====== 新增：确保AH帮助目录存在 ====== */
static void ensure_ah_builtin_files(const char *root) {
    /* 创建内置帮助文件 */
    char path[512];
    snprintf(path, sizeof(path), "%s/AH/builtin/common_issues.md", root);
    if (access(path, F_OK) != 0) {
        FILE *fp = fopen(path, "w");
        if (fp) {
            fprintf(fp,
                "# LING OS 常见问题\n\n"
                "## 如何查看系统日志？\n"
                "使用 `logdump` 命令；或通过 `read_log` 技能查看 ai_server.log、sub_ai_worker_*.log。\n\n"
                "## 技能调用错误怎么办？\n"
                "1. 检查 skill_help.json 确认参数格式\n"
                "2. 调用 query_knowledge_base 技能匹配知识库\n"
                "3. 若仍失败，查看 ai_server.log 中的详细错误\n\n"
                "## AI 服务不可用如何排查？\n"
                "1. `ai status` 查看服务状态\n"
                "2. 检查 /LINGOS/run/ai.sock 是否存在\n"
                "3. 查看 ai_server.log 是否有异常\n\n"
                "## 高风险操作被拒绝？\n"
                "1. 使用 `nook allow-high-risk` 启用自动授权\n"
                "2. 或在 Shell 中手动确认 Y/N 授权\n\n"
                "---\n"
                "生成时间: 自动生成\n"
            );
            fclose(fp);
            LOG_DEBUG_T("Sync", "AHBuiltin", "Create", "created %s", path);
        }
    }

    snprintf(path, sizeof(path), "%s/AH/builtin/how_to_view_logs.md", root);
    if (access(path, F_OK) != 0) {
        FILE *fp = fopen(path, "w");
        if (fp) {
            fprintf(fp,
                "# 如何查看日志\n\n"
                "## 系统日志\n"
                "```bash\n"
                "logdump\n"
                "```\n\n"
                "## AI服务器日志\n"
                "```bash\n"
                "tail -f /LINGOS/Debug/ai_server.log\n"
                "```\n\n"
                "## 子AI Worker日志\n"
                "```bash\n"
                "tail -f /LINGOS/Debug/sub_ai_worker_*.log\n"
                "```\n\n"
                "## 授权服务日志\n"
                "```bash\n"
                "tail -f /LINGOS/Debug/authorization.log\n"
                "```\n"
            );
            fclose(fp);
            LOG_DEBUG_T("Sync", "AHBuiltin", "Create", "created %s", path);
        }
    }
}

static void mkdir_p(const char *path, mode_t mode) {
    char tmp[1024];
    char *p = NULL;
    size_t len;
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len-1] == '/') tmp[len-1] = 0;
    for (p = tmp+1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (access(tmp, F_OK) != 0) mkdir(tmp, mode);
            *p = '/';
        }
    }
    if (access(tmp, F_OK) != 0) mkdir(tmp, mode);
}

int sync_system_to_template(void) {
    const char *root = lingos_data_root();
    if (!root) {
        LOG_ERROR_T("Sync", "Start", "NoRoot", "lingos_data_root() returned NULL");
        return -1;
    }
    LOG_DEBUG_T("Sync", "Start", "Begin", "synchronizing system directories with template");

    if (access(root, F_OK) != 0) {
        if (mkdir(root, 0755) == 0) {
            LOG_DEBUG_T("Sync", "Root", "Create", "created root %s", root);
        } else {
            LOG_ERROR_T("Sync", "Root", "Fail", "cannot create root %s: %s", root, strerror(errno));
            return -1;
        }
    }

    for (int i = 0; required_dirs[i]; i++) {
        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), "%s%s", root, required_dirs[i]);
        if (access(fullpath, F_OK) != 0) {
            mkdir_p(fullpath, 0755);
            LOG_DEBUG_T("Sync", "Dir", "Create", "created %s", fullpath);
        }
    }

    ensure_version_file(root);
    ensure_skill_index(root);
    ensure_memory_registry(root);
    ensure_state_file(root);
    ensure_passwd_file(root);
    ensure_ah_builtin_files(root);  /* 新增 */

    LOG_DEBUG_T("Sync", "End", "Success", "system directories synchronized");
    return 0;
}