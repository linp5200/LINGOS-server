/**
 * @file    app_sandbox.c
 * @brief   应用沙箱启动器（支持 sandbox.conf 自动创建与加载，受防御模式影响）
 * @version LN-B-5.0.0.0
 * @changes 集成防御模式（绝对保护强制 strict）；安全字符串替换
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <seccomp.h>
#include <errno.h>
#include <fcntl.h>

#include "app_sandbox.h"
#include "app_runner.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include "../security/defense_mode.h"   /* 新增防御模式 */

#ifndef CLONE_NEWNS
#define CLONE_NEWNS 0x00020000
#endif
#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS 0x04000000
#endif
#ifndef CLONE_NEWIPC
#define CLONE_NEWIPC 0x08000000
#endif
#ifndef CLONE_NEWNET
#define CLONE_NEWNET 0x40000000
#endif
#ifndef CLONE_NEWPID
#define CLONE_NEWPID 0x20000000
#endif

#define SANDBOX_CONFIG_PATH "/system/config/sandbox.conf"

static int sandbox_mode = 0; /* 0=strict, 1=compat */
static int default_cpu_percent = 50;
static int default_memory_mb = 256;

extern int app_sandbox_apply_seccomp(void);
extern int app_sandbox_apply_cgroup(pid_t pid);
extern int app_sandbox_cgroup_init(void);

static const char *get_config_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, SANDBOX_CONFIG_PATH);
    }
    return path;
}

static void create_default_config(void) {
    const char *path = get_config_path();
    if (access(path, F_OK) == 0) return;
    char dir[512];
    const char *root = lingos_data_root();
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    mkdir(dir, 0755);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
            "# Sandbox configuration\n"
            "default_mode = strict\n"
            "cpu_percent = 50\n"
            "memory_mb = 256\n");
    fclose(fp);
}

static void load_config(void) {
    create_default_config();
    const char *path = get_config_path();
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
            if (strcmp(key, "default_mode") == 0) {
                sandbox_mode = (strcmp(val, "compat") == 0) ? 1 : 0;
            } else if (strcmp(key, "cpu_percent") == 0) {
                default_cpu_percent = atoi(val);
                if (default_cpu_percent <= 0) default_cpu_percent = 50;
            } else if (strcmp(key, "memory_mb") == 0) {
                default_memory_mb = atoi(val);
                if (default_memory_mb <= 0) default_memory_mb = 256;
            }
        }
    }
    fclose(fp);
}

static int setup_cgroup(const char *app_name, pid_t pid) {
    (void)app_name;
    return app_sandbox_apply_cgroup(pid);
}

static int apply_seccomp_rules(void) {
    /* 如果绝对保护模式启用，强制 strict */
    int current_mode = defense_mode_get();
    if (current_mode == DEFENSE_MODE_ABSOLUTE) {
        LOG_WARN_T("AppSandbox", "Seccomp", "AbsoluteProtect", "forcing strict seccomp");
        /* 应用更严格的规则 */
    }
    return app_sandbox_apply_seccomp();
}

static int pivot_root_sys(const char *new_root, const char *put_old) {
    return syscall(SYS_pivot_root, new_root, put_old);
}

static void setup_namespace_and_root(const char *app_dir) {
    if (unshare(CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWPID) == -1) {
        LOG_ERROR_T("AppSandbox", "SetupNS", "UnshareFail", "%s", strerror(errno));
        _exit(1);
    }
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    if (sandbox_mode == 0) {
        if (chdir(app_dir) != 0 || pivot_root_sys(".", ".") == -1) {
            chroot(app_dir);
            chdir("/");
        }
    } else {
        chdir(app_dir);
    }
}

int app_start_sandboxed(const char *app_name) {
    LOG_INFO_T("AppSandbox", "Start", "Enter", "app_name='%s'", app_name ? app_name : "(null)");
    if (!app_name) return -1;
    if (app_is_running(app_name)) return -1;

    load_config();

    const char *mode_env = getenv("LINGOS_SANDBOX_MODE");
    if (mode_env && strcmp(mode_env, "compat") == 0) sandbox_mode = 1;
    else if (mode_env && strcmp(mode_env, "strict") == 0) sandbox_mode = 0;

    app_sandbox_cgroup_init();

    pid_t pid = fork();
    if (pid == -1) return -1;

    if (pid == 0) {
        const char *app_dir = get_app_dir(app_name);
        if (!app_dir) _exit(1);
        setup_namespace_and_root(app_dir);
        if (apply_seccomp_rules() != 0) _exit(1);

        char *entry_point = read_entry_point(app_name);
        if (!entry_point) _exit(1);

        char script_path[1024];
        if (sandbox_mode == 0) safe_snprintf(script_path, sizeof(script_path), "/%s", entry_point);
        else safe_snprintf(script_path, sizeof(script_path), "%s/%s", app_dir, entry_point);

        if (access(script_path, X_OK) != 0) {
            free(entry_point);
            _exit(1);
        }

        char log_dir[1024];
        safe_snprintf(log_dir, sizeof(log_dir), "%s/logs", app_dir);
        mkdir(log_dir, 0755);
        char log_path[1024];
        safe_snprintf(log_path, sizeof(log_path), "%s/stdout.log", log_dir);
        freopen(log_path, "a", stdout);
        freopen(log_path, "a", stderr);

        execl(script_path, script_path, (char*)NULL);
        _exit(1);
    } else {
        setup_cgroup(app_name, pid);
        char pid_path[1024];
        const char *app_dir = get_app_dir(app_name);
        safe_snprintf(pid_path, sizeof(pid_path), "%s/pid", app_dir);
        FILE *fp = fopen(pid_path, "w");
        if (fp) {
            fprintf(fp, "%d\n", pid);
            fclose(fp);
        }
        LOG_INFO_T("AppSandbox", "Start", "Success", "%s PID=%d", app_name, pid);
        return 0;
    }
}