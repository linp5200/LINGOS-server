#include "alias.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include "uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_ALIAS 64
#define ALIAS_FILE "/.lingos_aliases"

static struct {
    char name[64];
    char cmd[256];
} aliases[MAX_ALIAS];
static int alias_count = 0;

void alias_set(const char *name, const char *cmd) {
    if (!name || !*name || !cmd) return;
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            strncpy(aliases[i].cmd, cmd, sizeof(aliases[i].cmd)-1);
            alias_save();
            return;
        }
    }
    if (alias_count < MAX_ALIAS) {
        strncpy(aliases[alias_count].name, name, sizeof(aliases[0].name)-1);
        strncpy(aliases[alias_count].cmd, cmd, sizeof(aliases[0].cmd)-1);
        alias_count++;
        alias_save();
    }
}

void alias_unset(const char *name) {
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            for (int j = i; j < alias_count - 1; j++) aliases[j] = aliases[j+1];
            alias_count--;
            alias_save();
            return;
        }
    }
}

const char *alias_get(const char *name) {
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) return aliases[i].cmd;
    }
    return NULL;
}

void alias_list(void) {
    for (int i = 0; i < alias_count; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "alias %s='%s'\n", aliases[i].name, aliases[i].cmd);
        uart_puts(buf);
    }
}

void alias_save(void) {
    const char *root = lingos_data_root();
    char path[512];
    snprintf(path, sizeof(path), "%s%s", root, ALIAS_FILE);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_WARN_T("Alias", "Save", "OpenFail", "cannot write %s", path);
        return;
    }
    for (int i = 0; i < alias_count; i++) fprintf(fp, "%s=%s\n", aliases[i].name, aliases[i].cmd);
    fclose(fp);
}

void alias_load(void) {
    const char *root = lingos_data_root();
    char path[512];
    snprintf(path, sizeof(path), "%s%s", root, ALIAS_FILE);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[512];
    while (fgets(line, sizeof(line), fp) && alias_count < MAX_ALIAS) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            strncpy(aliases[alias_count].name, line, sizeof(aliases[0].name)-1);
            strncpy(aliases[alias_count].cmd, eq+1, sizeof(aliases[0].cmd)-1);
            alias_count++;
        }
    }
    fclose(fp);
}