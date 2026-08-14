#include "history.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_HISTORY 100
#define HISTORY_FILE "/.lingos_history"

static char history[MAX_HISTORY][256];
static int head = 0;
static int count = 0;
static int current = -1;

void history_init(void) {
    history_load();
}

void history_add(const char *cmd) {
    if (!cmd || !*cmd) return;
    if (count > 0 && strcmp(history[(head - 1 + MAX_HISTORY) % MAX_HISTORY], cmd) == 0) return;
    strncpy(history[head], cmd, sizeof(history[0])-1);
    head = (head + 1) % MAX_HISTORY;
    if (count < MAX_HISTORY) count++;
    current = head;
    history_save();
}

const char *history_prev(void) {
    if (count == 0) return NULL;
    int prev = (current - 1 + MAX_HISTORY) % MAX_HISTORY;
    if (prev == head && count < MAX_HISTORY) return NULL;
    current = prev;
    return history[current];
}

const char *history_next(void) {
    if (count == 0) return NULL;
    int next = (current + 1) % MAX_HISTORY;
    if (next == head) {
        current = head;
        return NULL;
    }
    current = next;
    return history[current];
}

void history_save(void) {
    const char *root = lingos_data_root();
    char path[512];
    snprintf(path, sizeof(path), "%s%s", root, HISTORY_FILE);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_WARN_T("History", "Save", "OpenFail", "cannot write %s", path);
        return;
    }
    int start = (head - count + MAX_HISTORY) % MAX_HISTORY;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % MAX_HISTORY;
        fprintf(fp, "%s\n", history[idx]);
    }
    fclose(fp);
}

void history_load(void) {
    const char *root = lingos_data_root();
    char path[512];
    snprintf(path, sizeof(path), "%s%s", root, HISTORY_FILE);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[256];
    int idx = 0;
    while (fgets(line, sizeof(line), fp) && idx < MAX_HISTORY) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        strncpy(history[idx], line, sizeof(history[0])-1);
        idx++;
    }
    count = idx;
    head = count;
    current = head;
    fclose(fp);
}