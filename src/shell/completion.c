#include "completion.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

static const char *builtin_commands[] = {
    "help", "reboot", "poweroff", "clear", "logdump", "audit",
    "system", "health", "nook", "skill", "config", "test", "scan",
    "ai", "subai", "app", "firewall", "weather", "api", "host",
    NULL
};

static char *get_app_names(void) {
    static char apps[4096] = {0};
    apps[0] = '\0';
    const char *root = lingos_data_root();
    char apps_dir[512];
    snprintf(apps_dir, sizeof(apps_dir), "%s/apps", root);
    DIR *d = opendir(apps_dir);
    if (!d) return apps;
    struct dirent *entry;
    char *ptr = apps;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        ptr += snprintf(ptr, apps + sizeof(apps) - ptr, "%s\n", entry->d_name);
    }
    closedir(d);
    return apps;
}

const char *completion_try(const char *input) {
    static char result[256];
    result[0] = '\0';
    if (!input || !*input) return NULL;
    for (int i = 0; builtin_commands[i]; i++) {
        if (strncmp(input, builtin_commands[i], strlen(input)) == 0) {
            strcpy(result, builtin_commands[i]);
            return result;
        }
    }
    char *apps = get_app_names();
    if (apps && *apps) {
        char *line = apps;
        while (*line) {
            char app[64];
            sscanf(line, "%63s", app);
            if (strncmp(input, app, strlen(input)) == 0) {
                strcpy(result, app);
                return result;
            }
            line += strlen(app) + 1;
        }
    }
    return NULL;
}

void completion_dump(void) {
    for (int i = 0; builtin_commands[i]; i++) {
        printf("%s ", builtin_commands[i]);
    }
    printf("\n");
}