#include "data_path.h"
#include "log_extra.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

#define LINGOS_ROOT_DEFAULT "/LINGOS"

/* 【0.4.3】路径集中化：数据根统一 /LINGOS（先生架构——config/state/data/models 全部在 /LINGOS）
 * LINGOS_ROOT env 仅作显式覆盖（测试/特殊部署），全捆包默认即 /LINGOS（不再指包内） */
static const char *effective_root(void) {
    const char *env = getenv("LINGOS_ROOT");
    return (env && env[0]) ? env : LINGOS_ROOT_DEFAULT;
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
