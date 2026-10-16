#include "file_integrity.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../core/version.h"
#include "../common/mode.h"
#include "log_extra.h"
#include "uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define MAX_KEY_FILES 32

typedef struct {
    const char *path;
    const char *marker;
    int (*recreate)(void);
} key_file_t;

/* 【修复】写盘必须校验结果：磁盘满/只读时原实现照样返回 0，
 *   integrity_check_all() 会打印 "recreated ... OK"（假成功），
 *   用户以为已修复，实际文件仍是坏的。 */
static int write_placeholder(const char *path, const char *content) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    int ok = (fputs(content, fp) >= 0);
    if (fflush(fp) != 0) ok = 0;
    if (fclose(fp) != 0) ok = 0;
    return ok ? 0 : -1;
}

static int recreate_version_file(void) {
    const char *root = lingos_data_root();
    char path[512];
    snprintf(path, sizeof(path), "%s/version", root);

    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    int ok = (fprintf(fp, "%s\n", LINGOS_VERSION) > 0);
    if (fflush(fp) != 0) ok = 0;
    if (fclose(fp) != 0) ok = 0;
    return ok ? 0 : -1;
}

static int recreate_skill_index(void) {
    const char *root = lingos_data_root();
    char path[512];
    /* 【0.6.0 修复】路径对齐（原 /skills/ 与读取方 /registry/skills/ 分歧——
     *   lingosd registry_list、ai_server.py SKILL_INDEX_PATH、fs_layout 均读
     *   /registry/skills/index.json；本处为唯一分歧点，已统一） */
    snprintf(path, sizeof(path), "%s/registry/skills/index.json", root);
    /* 技能索引由 C/Python 管理，此处仅创建空占位（真实内容由
     * registry_skill_write_index 导出）；写盘结果必须校验（防假成功） */
    return write_placeholder(path, "[]\n");
}

static int recreate_memory_registry(void) {
    const char *root = lingos_data_root();
    char path[512];
    snprintf(path, sizeof(path), "%s/data/ai_memory/memory_registry.json", root);
    return write_placeholder(path, "[]\n");
}

static int recreate_state_file(void) {
    const char *root = lingos_data_root();
    char path[512];
    snprintf(path, sizeof(path), "%s/Ensystem/state.json", root);
    return write_placeholder(path, "{\"system_configured\":0,\"mode\":\"app\"}\n");
}

static key_file_t key_files[] = {
    {"/version", NULL, recreate_version_file},
    /* 【0.6.0】marker=NULL——仅缺失时补占位，绝不覆盖已有索引（防误擦除技能） */
    {"/registry/skills/index.json", NULL, recreate_skill_index},
    /* 【修复】marker 必须置 NULL：真实写入方从不写 "memory_registry" 字面量
     *   （syscall_handler 写 {"entries":[...],"version":"1.0"}、fs_layout 写
     *   {"version":"1.0","entries":[]}）→ 原 marker 检查必然失败 → 用空 [] 覆盖
     *   真实记忆注册表（AI 记忆索引丢失）。宁可漏检不可误删。 */
    {"/data/ai_memory/memory_registry.json", NULL, recreate_memory_registry},
    /* 【修复】同上：config_saver 写 "system_configured": true、fs_layout 首行为
     *   {"system_configured":false,...}，都不含字面量 "state" → 原检查必然失败
     *   → recreate_state_file() 用未配置占位覆盖用户已配置的 state.json。 */
    {"/Ensystem/state.json", NULL, recreate_state_file},
    {NULL, NULL, NULL}
};

int integrity_check_file(const char *relpath, const char *marker) {
    const char *root = lingos_data_root();
    char fullpath[512];
    snprintf(fullpath, sizeof(fullpath), "%s%s", root, relpath);
    if (access(fullpath, F_OK) != 0) {
        LOG_WARN_T("Integrity", "Check", "Missing", "%s missing", relpath);
        return -1;
    }
    if (marker) {
        FILE *fp = fopen(fullpath, "r");
        if (!fp) {
            /* 【修复】文件存在但打不开（权限/IO 错误）时原来直接 return 0：
             *   既没校验内容，又对外宣称"完好"，是假成功。 */
            LOG_ERROR_T("Integrity", "Check", "OpenFail", "%s exists but cannot be read: %s",
                        relpath, strerror(errno));
            return -1;
        }
        char buf[256];
        if (fgets(buf, sizeof(buf), fp)) {
            /* 检查标记，但对于空占位文件不再严格检查 */
            if (strstr(buf, marker) == NULL && strstr(buf, "skills") == NULL) {
                LOG_WARN_T("Integrity", "Check", "Marker", "%s missing marker '%s'", relpath, marker);
                fclose(fp);
                return -1;
            }
        }
        fclose(fp);
    }
    return 0;
}

int integrity_check_all(void) {
    int all_ok = 1;
    for (int i = 0; key_files[i].path; i++) {
        if (integrity_check_file(key_files[i].path, key_files[i].marker) != 0) {
            if (key_files[i].recreate) {
                LOG_INFO_T("Integrity", "Fix", "Recreating", "recreating %s", key_files[i].path);
                if (key_files[i].recreate() == 0) {
                    LOG_INFO_T("Integrity", "Fix", "OK", "recreated %s", key_files[i].path);
                } else {
                    LOG_ERROR_T("Integrity", "Fix", "Fail", "failed to recreate %s", key_files[i].path);
                    all_ok = 0;
                }
            } else {
                all_ok = 0;
            }
        }
    }
    return all_ok;
}

/* 【修复】解析 state.json 的 system_configured，兼容真实写入格式：
 *   config_saver.c 写 "system_configured": true、
 *   system_config.c 写 "system_configured": 1。
 *   原实现只匹配字面量 "\"system_configured\": 1" → 已配置系统被误判为"未配置"
 *   → main.c:1569 进而调用 integrity_check_all()，把 state.json/记忆注册表
 *   覆盖成空占位（配置被清空、每次启动重新配置、AI 记忆索引丢失）。
 *   触发条件：任何一次正常配置之后的下一次启动。 */
static int state_is_configured(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    const char *key = strstr(buf, "\"system_configured\"");
    if (!key) return 0;

    const char *p = key + strlen("\"system_configured\"");
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    if (*p == '1') return 1;              /* 1 */
    if (*p == '"') p++;
    if (strncmp(p, "true", 4) == 0) return 1;   /* true / "true" */
    return 0;                              /* false / 0 / "false" / 缺失 */
}

int integrity_check_required(void) {
    const char *root = lingos_data_root();
    char state_path[512];
    safe_snprintf(state_path, sizeof(state_path), "%s/Ensystem/state.json", root);

    /* 文件不存在 → 需要配置；存在 → 只读判断，绝不删改用户文件 */
    return state_is_configured(state_path) ? 0 : 1;
}