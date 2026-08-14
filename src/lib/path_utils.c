#include "path_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *expand_tilde(const char *path) {
    if (!path || path[0] != '~') return path;

    const char *home = getenv("HOME");
    if (!home) return path;  /* 无家目录，不展开 */

    static char expanded[1024];
    if (strlen(path) == 1) {
        snprintf(expanded, sizeof(expanded), "%s", home);
    } else if (path[1] == '/') {
        snprintf(expanded, sizeof(expanded), "%s/%s", home, path + 2);
    } else {
        /* ~user 形式暂不支持，保持原样 */
        return path;
    }
    return expanded;
}