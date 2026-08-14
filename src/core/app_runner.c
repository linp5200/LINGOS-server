#include "app_runner.h"
#include "data_path.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>

#define APPS_DIR "/apps"
#define STATE_DIR "/state/apps"
#define PID_FILE "pid"

const char *get_app_dir(const char *app_name) {
    static char path[1024];
    const char *root = lingos_data_root();
    snprintf(path, sizeof(path), "%s%s/%s", root, APPS_DIR, app_name);
    return path;
}

static const char *get_state_file(const char *app_name) {
    static char path[1024];
    const char *root = lingos_data_root();
    snprintf(path, sizeof(path), "%s%s/%s.json", root, STATE_DIR, app_name);
    return path;
}

char *read_entry_point(const char *app_name) {
    char state_path[1024];
    strcpy(state_path, get_state_file(app_name));
    FILE *fp = fopen(state_path, "r");
    if (!fp) return NULL;
    char line[512];
    char *entry = NULL;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "\"entry_point\":")) {
            char *p = strchr(line, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\"') p++;
                int i = 0;
                entry = malloc(512);
                while (*p && *p != '\"' && *p != '\n' && i < 511) entry[i++] = *p++;
                entry[i] = '\0';
            }
            break;
        }
    }
    fclose(fp);
    return entry;
}

int app_start(const char *app_name) {
    if (!app_name) return -1;
    if (app_is_running(app_name)) {
        LOG_WARN_T("AppRunner", "Start", "AlreadyRunning", "%s", app_name);
        return -1;
    }
    const char *app_dir = get_app_dir(app_name);
    if (access(app_dir, F_OK) != 0) {
        LOG_ERROR_T("AppRunner", "Start", "NoApp", "%s", app_name);
        return -1;
    }
    char *entry_point = read_entry_point(app_name);
    if (!entry_point) {
        LOG_ERROR_T("AppRunner", "Start", "NoEntry", "%s", app_name);
        return -1;
    }
    char script_path[1024];
    snprintf(script_path, sizeof(script_path), "%s/%s", app_dir, entry_point);
    if (access(script_path, X_OK) != 0) {
        LOG_ERROR_T("AppRunner", "Start", "EntryNotExec", "%s", script_path);
        free(entry_point);
        return -1;
    }
    pid_t pid = fork();
    if (pid == -1) {
        LOG_ERROR_T("AppRunner", "Start", "ForkFail", "%s", app_name);
        free(entry_point);
        return -1;
    }
    if (pid == 0) {
        setsid();
        char log_dir[1024];
        snprintf(log_dir, sizeof(log_dir), "%s/logs", app_dir);
        mkdir(log_dir, 0755);
        char log_path[1024];
        snprintf(log_path, sizeof(log_path), "%s/stdout.log", log_dir);
        freopen(log_path, "a", stdout);
        freopen(log_path, "a", stderr);
        execl(script_path, script_path, (char*)NULL);
        perror("execl");
        _exit(1);
    } else {
        char pid_path[1024];
        snprintf(pid_path, sizeof(pid_path), "%s/%s", app_dir, PID_FILE);
        FILE *fp = fopen(pid_path, "w");
        if (fp) {
            fprintf(fp, "%d\n", pid);
            fclose(fp);
        }
        LOG_INFO_T("AppRunner", "Start", "Success", "%s PID=%d", app_name, pid);
        free(entry_point);
        return 0;
    }
}

int app_stop(const char *app_name) {
    if (!app_name) return -1;
    const char *app_dir = get_app_dir(app_name);
    char pid_path[1024];
    snprintf(pid_path, sizeof(pid_path), "%s/%s", app_dir, PID_FILE);
    FILE *fp = fopen(pid_path, "r");
    if (!fp) return -1;
    int pid;
    if (fscanf(fp, "%d", &pid) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    if (kill(pid, SIGTERM) == 0) {
        for (int i = 0; i < 5; i++) {
            if (kill(pid, 0) != 0) break;
            sleep(1);
        }
        if (kill(pid, 0) == 0) kill(pid, SIGKILL);
        unlink(pid_path);
        LOG_INFO_T("AppRunner", "Stop", "Success", "%s", app_name);
        return 0;
    }
    return -1;
}

int app_is_running(const char *app_name) {
    const char *app_dir = get_app_dir(app_name);
    char pid_path[1024];
    snprintf(pid_path, sizeof(pid_path), "%s/%s", app_dir, PID_FILE);
    FILE *fp = fopen(pid_path, "r");
    if (!fp) return 0;
    int pid;
    if (fscanf(fp, "%d", &pid) != 1) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return (kill(pid, 0) == 0);
}

char *app_get_logs(const char *app_name) {
    const char *app_dir = get_app_dir(app_name);
    char log_path[1024];
    snprintf(log_path, sizeof(log_path), "%s/logs/stdout.log", app_dir);
    FILE *fp = fopen(log_path, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size <= 0) {
        fclose(fp);
        return strdup("");
    }
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    fread(buf, 1, size, fp);
    buf[size] = '\0';
    fclose(fp);
    return buf;
}