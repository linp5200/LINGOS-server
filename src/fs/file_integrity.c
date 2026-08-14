#include "file_integrity.h"
#include "../common/data_path.h"
#include "../core/version.h"
#include "../common/mode.h"
#include "log_extra.h"
#include "uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_KEY_FILES 32

typedef struct {
    const char *path;
    const char *marker;
    int (*recreate)(void);
} key_file_t;

static int recreate_version_file(void) {
    const char *root = lingos_data_root();
    char path[512];
    snprintf(path, sizeof(path), "%s/version", root);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%s\n", LINGOS_VERSION);
    fclose(fp);
    return 0;
}

static int recreate_skill_index(void) {
    const char *root = lingos_data_root();
    char path[512];
    snprintf(path, sizeof(path), "%s/skills/index.json", root);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    /* 技能索引由Python管理，C端仅创建空占位 */
    fprintf(fp, "[]\n");
    fclose(fp);
    return 0;
}

static int recreate_memory_registry(void) {
    const char *root = lingos_data_root();
    char path[512];
    snprintf(path, sizeof(path), "%s/data/ai_memory/memory_registry.json", root);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "[]\n");
    fclose(fp);
    return 0;
}

static int recreate_state_file(void) {
    const char *root = lingos_data_root();
    char path[512];
    snprintf(path, sizeof(path), "%s/Ensystem/state.json", root);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "{\"system_configured\":0,\"mode\":\"app\"}\n");
    fclose(fp);
    return 0;
}

static key_file_t key_files[] = {
    {"/version", NULL, recreate_version_file},
    {"/skills/index.json", "skills", recreate_skill_index},
    {"/data/ai_memory/memory_registry.json", "memory_registry", recreate_memory_registry},
    {"/Ensystem/state.json", "state", recreate_state_file},
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
        if (fp) {
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

int integrity_check_required(void) {
    const char *root = lingos_data_root();
    char state_path[512];
    snprintf(state_path, sizeof(state_path), "%s/Ensystem/state.json", root);
    FILE *fp = fopen(state_path, "r");
    if (!fp) return 1;
    char line[256];
    int configured = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "\"system_configured\": 1")) {
            configured = 1;
            break;
        }
    }
    fclose(fp);
    return !configured;
}