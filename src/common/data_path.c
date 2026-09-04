#include "data_path.h"
#include "log_extra.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

#define LINGOS_ROOT_DEFAULT "/LINGOS"

/* 【0.4.3】路径集中化第一步：根可被 LINGOS_ROOT 环境变量覆盖
 * 全捆包 start.sh 已 export LINGOS_ROOT=<包目录> —— 包内 share/webui 等随包生效，
 * 无需把文件另行部署到 /LINGOS（先生环境/部署方零额外步骤） */

const char *lingos_data_root(void) {
    static const char *root = NULL;
    static int checked = 0;
    if (!checked) {
        const char *env = getenv("LINGOS_ROOT");
        root = (env && env[0]) ? env : LINGOS_ROOT_DEFAULT;
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
