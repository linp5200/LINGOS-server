#include "mode.h"
#include "data_path.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define MODE_CONFIG_PATH "/system/config/mode.conf"

static lingos_mode_t cached_mode = -1;
static int cache_valid = 0;

static const char *get_mode_config_file(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        snprintf(path, sizeof(path), "%s%s", root, MODE_CONFIG_PATH);
    }
    return path;
}

lingos_mode_t lingos_get_mode(void) {
    if (cache_valid) return cached_mode;

    const char *path = get_mode_config_file();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("Mode", "Get", "NoConfig", "Mode config not found, defaulting to APP");
        cached_mode = MODE_APP;
        cache_valid = 1;
        return MODE_APP;
    }

    char line[64];
    if (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "MODE=system") || strstr(line, "MODE=SYSTEM")) {
            cached_mode = MODE_SYSTEM;
        } else {
            cached_mode = MODE_APP;
        }
    } else {
        cached_mode = MODE_APP;
    }
    fclose(fp);
    cache_valid = 1;
    LOG_DEBUG_T("Mode", "Get", "OK", "mode=%s", lingos_mode_name(cached_mode));
    return cached_mode;
}

int lingos_set_mode(lingos_mode_t mode) {
    const char *path = get_mode_config_file();
    char dir[512];
    const char *root = lingos_data_root();
    snprintf(dir, sizeof(dir), "%s/system/config", root);
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            LOG_ERROR_T("Mode", "Set", "MkdirFail", "cannot create %s", dir);
            return -1;
        }
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("Mode", "Set", "OpenFail", "cannot write %s", path);
        return -1;
    }
    if (mode == MODE_SYSTEM) {
        fprintf(fp, "MODE=system\n");
    } else {
        fprintf(fp, "MODE=app\n");
    }
    fclose(fp);
    cached_mode = mode;
    LOG_INFO_T("Mode", "Set", "OK", "mode set to %s", lingos_mode_name(mode));
    return 0;
}

int lingos_mode_config_valid(void) {
    const char *path = get_mode_config_file();
    return (access(path, F_OK) == 0);
}

const char *lingos_mode_name(lingos_mode_t mode) {
    return (mode == MODE_SYSTEM) ? "system" : "app";
}