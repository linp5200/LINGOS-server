#include "data_path.h"
#include "safe_string.h"
#include "log_extra.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

#define LINGOS_ROOT_DEFAULT "/LINGOS"

static const char *effective_root(void) {
    const char *env = getenv("LINGOS_ROOT");
    return (env && env[0]) ? env : LINGOS_ROOT_DEFAULT;
}

/* 各 id 相对根的子路径（先生 FHS 布局——全并入 /LINGOS） */
static const char *rel_of(path_id_t id) {
    switch (id) {
        case P_ROOT:      return "";
        case P_BIN:       return "/bin";
        case P_ETC:       return "/system/config";   /* 配置（兼容旧 system/config） */
        case P_RUN:       return "/run";
        case P_LOG:       return "/log";
        case P_DATA:      return "/data";
        case P_STATE:     return "/state";
        case P_MODELS:    return "/models";
        case P_SKILLS:    return "/skills";
        case P_SHARE:     return "/share";
        case P_WEBUI:     return "/share/webui";
        case P_REGISTRY:  return "/registry";
        case P_PLUGINS:   return "/plugins";
        case P_SNAPSHOTS: return "/snapshots";
        case P_EN:        return "/Ensystem";
        default:          return "";
    }
}

const char *lingos_path(path_id_t id) {
    static char buf[P_COUNT][512];
    static int once = 0;
    if (!once) {
        /* 确保根存在（一次） */
        const char *r = effective_root();
        if (access(r, F_OK) != 0) mkdir(r, 0755);
        once = 1;
    }
    safe_snprintf(buf[id], sizeof(buf[id]), "%s%s", effective_root(), rel_of(id));
    return buf[id];
}

void lingos_path_join(const char *rel, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (rel && rel[0] == '/')
        safe_snprintf(out, out_sz, "%s%s", lingos_data_root(), rel);
    else
        safe_snprintf(out, out_sz, "%s/%s", lingos_data_root(), rel ? rel : "");
}

const char *lingos_data_root(void) {
    static const char *root = NULL;
    static int checked = 0;
    if (!checked) {
        root = effective_root();
        if (access(root, F_OK) != 0) {
            if (mkdir(root, 0755) == 0) {
                LOG_INFO_T("DataPath", "Root", "Created", "created %s", root);
            } else {
                LOG_WARN_T("DataPath", "Root", "Missing", "%s does not exist and cannot be created", root);
            }
        }
        checked = 1;
    }
    return root;
}
