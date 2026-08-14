/**
 * @file    src/supervisor/supervisor.c
 * @brief   LING OS 监督者进程（独立恢复）
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：防弹编程（独立进程，最小依赖）
 * @changes 移除空闲超时逻辑，仅依赖心跳监控；心跳间隔改为 2 秒。
 */

#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>

#define WATCHDOG_CONF_PATH "/system/config/watchdog.conf"

#define EXIT_CONFIG_MISSING 10
#define EXIT_NORMAL         0

typedef struct {
    int code;
    const char *meaning_en;
    const char *meaning_zh;
} exit_code_map_t;

static exit_code_map_t g_exit_code_map[] = {
    {0,   "Normal exit", "正常退出"},
    {10,  "Configuration missing", "配置缺失"},
    {130, "Interrupted by user (Ctrl+C)", "用户中断 (Ctrl+C)"},
    {137, "Killed by OOM killer", "被 OOM Killer 杀死"},
    {139, "Segmentation fault", "段错误"},
    {134, "Aborted", "程序终止"},
    {127, "Command not found", "命令未找到"},
    {126, "Permission denied", "权限不足"},
    {-1,  NULL, NULL}
};

static const char* get_exit_meaning(int code, const char *lang) {
    for (int i = 0; g_exit_code_map[i].meaning_en != NULL; i++) {
        if (g_exit_code_map[i].code == code) {
            if (lang && strcmp(lang, "zh") == 0) {
                return g_exit_code_map[i].meaning_zh;
            }
            return g_exit_code_map[i].meaning_en;
        }
    }
    return lang && strcmp(lang, "zh") == 0 ? "未知退出码" : "Unknown exit code";
}

typedef struct {
    char strategy[32];
    int auto_restart_delay;
    int max_restart_per_hour;
    int enable_core_dump;
    int fallback_to_offline;
    int heartbeat_timeout;
} supervisor_config_t;

static supervisor_config_t g_config = {
    .strategy = "auto_restart",
    .auto_restart_delay = 3,
    .max_restart_per_hour = 5,
    .enable_core_dump = 0,
    .fallback_to_offline = 1,
    .heartbeat_timeout = 60
};

static pid_t g_child_pid = -1;
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_user_initiated_exit = 0;
static time_t g_restart_timestamps[60];
static int g_restart_index = 0;
static int g_restart_count = 0;
static int g_running = 0;
static char g_child_binary[256] = "./lingos_linux";

static volatile time_t g_last_heartbeat = 0;
static pthread_t g_heartbeat_thread;
static volatile int g_heartbeat_running = 0;
static volatile int g_heartbeat_stop = 0;

static const char* get_config_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, WATCHDOG_CONF_PATH);
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
    if (!fp) {
        LOG_WARN_T("Supervisor", "CreateConfig", "Fail", "cannot create %s", path);
        return;
    }
    fprintf(fp,
        "# LING OS Watchdog Configuration\n"
        "shell_crash_strategy = auto_restart\n"
        "auto_restart_delay_seconds = 3\n"
        "max_restart_per_hour = 5\n"
        "enable_core_dump = 0\n"
        "fallback_to_offline = 1\n"
        "heartbeat_timeout = 180\n");
    fclose(fp);
    LOG_INFO_T("Supervisor", "CreateConfig", "OK", "created %s", path);
}

static void load_config(void) {
    LOG_DEBUG_T("Supervisor", "LoadConfig", "enter", "loading config from %s", get_config_path());

    const char *path = get_config_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        create_default_config();
        fp = fopen(path, "r");
        if (!fp) {
            LOG_WARN_T("Supervisor", "LoadConfig", "Fail", "using defaults");
            return;
        }
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
            if (strcmp(key, "shell_crash_strategy") == 0) {
                safe_strncpy(g_config.strategy, val, sizeof(g_config.strategy));
            } else if (strcmp(key, "auto_restart_delay_seconds") == 0) {
                g_config.auto_restart_delay = atoi(val);
                if (g_config.auto_restart_delay < 1) g_config.auto_restart_delay = 1;
            } else if (strcmp(key, "max_restart_per_hour") == 0) {
                g_config.max_restart_per_hour = atoi(val);
                if (g_config.max_restart_per_hour < 1) g_config.max_restart_per_hour = 1;
            } else if (strcmp(key, "enable_core_dump") == 0) {
                g_config.enable_core_dump = atoi(val);
            } else if (strcmp(key, "fallback_to_offline") == 0) {
                g_config.fallback_to_offline = atoi(val);
            } else if (strcmp(key, "heartbeat_timeout") == 0) {
                g_config.heartbeat_timeout = atoi(val);
                if (g_config.heartbeat_timeout < 10) g_config.heartbeat_timeout = 10;
            }
        }
    }
    fclose(fp);

    LOG_DEBUG_T("Supervisor", "LoadConfig", "OK", "strategy=%s, delay=%d, max=%d, heartbeat=%d",
                g_config.strategy, g_config.auto_restart_delay, g_config.max_restart_per_hour,
                g_config.heartbeat_timeout);
}

static void log_crash(pid_t pid, int status, int signal) {
    const char *root = lingos_data_root();
    char crash_path[512];
    safe_snprintf(crash_path, sizeof(crash_path), "%s/Debug/crash.bin", root);

    FILE *fp = fopen(crash_path, "ab");
    if (!fp) {
        LOG_WARN_T("Supervisor", "LogCrash", "OpenFail", "cannot write %s", crash_path);
        return;
    }

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(fp, "timestamp=%s pid=%d status=%d signal=%d\n", time_str, pid, status, signal);
    fclose(fp);

    LOG_DEBUG_T("Supervisor", "LogCrash", "OK", "crash logged");
}

static int is_throttled(void) {
    if (g_restart_count == 0) return 0;

    time_t now = time(NULL);
    int recent = 0;
    time_t cutoff = now - 3600;

    for (int i = 0; i < g_restart_count && i < 60; i++) {
        if (g_restart_timestamps[i] > cutoff) {
            recent++;
        }
    }

    if (recent >= g_config.max_restart_per_hour) {
        LOG_WARN_T("Supervisor", "Throttle", "Trigger", "%d restarts in last hour, throttling", recent);
        return 1;
    }
    return 0;
}

static void record_restart(void) {
    g_restart_timestamps[g_restart_index] = time(NULL);
    g_restart_index = (g_restart_index + 1) % 60;
    if (g_restart_count < 60) g_restart_count++;
}

static void sigchld_handler(int sig) {
    (void)sig;
    LOG_DEBUG_T("Supervisor", "SigChld", "Received", "child exited signal");
}

static void sigterm_handler(int sig) {
    (void)sig;
    LOG_WARN_T("Supervisor", "SigTerm", "Received", "shutdown requested");
    g_shutdown_requested = 1;
    if (g_child_pid > 0) {
        kill(g_child_pid, SIGKILL);
        int status;
        waitpid(g_child_pid, &status, 0);
        g_child_pid = -1;
    }
    exit(0);
}

static void sigusr1_handler(int sig) {
    (void)sig;
    g_user_initiated_exit = 1;
    LOG_DEBUG_T("Supervisor", "SigUsr1", "Received", "user-initiated exit signal");
}

static void sigint_handler(int sig) {
    (void)sig;
    LOG_WARN_T("Supervisor", "SigInt", "Received", "interrupt signal, graceful shutdown");
    g_shutdown_requested = 1;
    /* 【修复】优雅关闭：先 SIGTERM 子进程并等待（避免 TUI 模式下粗暴 SIGKILL 崩溃），
     * 子进程未退出时再强杀兜底 */
    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            int status;
            pid_t ret = waitpid(g_child_pid, &status, WNOHANG);
            if (ret == g_child_pid) {
                g_child_pid = -1;
                break;
            }
            usleep(100000);
        }
        if (g_child_pid > 0) {
            kill(g_child_pid, SIGKILL);
            waitpid(g_child_pid, NULL, 0);
            g_child_pid = -1;
        }
    }
    exit(0);
}

static void start_child(void) {
    LOG_DEBUG_T("Supervisor", "StartChild", "enter", "forking %s", g_child_binary);

    pid_t pid = fork();
    if (pid == -1) {
        LOG_ERROR_T("Supervisor", "StartChild", "ForkFail", "fork failed: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        setsid();
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGUSR1, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        execl(g_child_binary, g_child_binary, (char*)NULL);
        LOG_ERROR_T("Supervisor", "StartChild", "ExecFail", "execl %s failed: %s", g_child_binary, strerror(errno));
        _exit(1);
    }

    g_child_pid = pid;
    g_user_initiated_exit = 0;
    g_last_heartbeat = time(NULL);
    LOG_INFO_T("Supervisor", "StartChild", "OK", "child PID=%d", pid);
}

static void* heartbeat_thread_func(void *arg) {
    (void)arg;
    LOG_DEBUG_T("Supervisor", "Heartbeat", "Started", "heartbeat monitor thread started");
    g_heartbeat_running = 1;

    const char *root = lingos_data_root();
    char heartbeat_path[512];
    safe_snprintf(heartbeat_path, sizeof(heartbeat_path), "%s/run/heartbeat", root);

    while (!g_heartbeat_stop && !g_shutdown_requested) {
        sleep(2);
        if (g_shutdown_requested || g_heartbeat_stop) break;
        if (g_child_pid <= 0) continue;

        time_t now = time(NULL);
        FILE *fp = fopen(heartbeat_path, "r");
        if (fp) {
            long ts;
            if (fscanf(fp, "%ld", &ts) == 1) {
                if (ts > g_last_heartbeat) g_last_heartbeat = ts;
            }
            fclose(fp);
        }

        time_t elapsed = now - g_last_heartbeat;
        if (elapsed > g_config.heartbeat_timeout && g_child_pid > 0) {
            LOG_WARN_T("Supervisor", "Heartbeat", "Timeout",
                       "heartbeat timeout (%ld seconds), killing child", elapsed);
            kill(g_child_pid, SIGKILL);
            g_child_pid = -1;
        }
    }

    g_heartbeat_running = 0;
    LOG_DEBUG_T("Supervisor", "Heartbeat", "Stopped", "heartbeat monitor thread stopped");
    return NULL;
}

static void start_heartbeat_monitor(void) {
    if (g_heartbeat_running) return;
    g_heartbeat_stop = 0;
    if (pthread_create(&g_heartbeat_thread, NULL, heartbeat_thread_func, NULL) != 0) {
        LOG_ERROR_T("Supervisor", "Heartbeat", "ThreadFail", "failed to create heartbeat thread");
        return;
    }
    LOG_INFO_T("Supervisor", "Heartbeat", "Started", "heartbeat monitor thread started");
}

static void stop_heartbeat_monitor(void) {
    if (!g_heartbeat_running) return;
    g_heartbeat_stop = 1;
    pthread_join(g_heartbeat_thread, NULL);
    LOG_DEBUG_T("Supervisor", "Heartbeat", "Stopped", "heartbeat monitor stopped");
}

static void start_recovery_shell(void) {
    LOG_INFO_T("Supervisor", "Recovery", "Start", "Starting recovery shell");
    uart_puts(tr(
        "\n=== LING OS Recovery Shell ===\n",
        "\n=== LING OS 恢复 Shell ===\n"
    ));
    uart_puts(tr(
        "The system is in recovery mode.\n"
        "You can run 'system configuration' to set up your system.\n"
        "Type 'exit' to return to supervisor and retry.\n",
        "系统处于恢复模式。\n"
        "您可以运行 'system configuration' 来设置您的系统。\n"
        "输入 'exit' 返回监督者并重试。\n"
    ));
    execl("/bin/sh", "sh", (char*)NULL);
    LOG_ERROR_T("Supervisor", "Recovery", "ExecFail", "failed to start shell: %s", strerror(errno));
    _exit(1);
}

static void restart_child(void) {
    if (g_shutdown_requested) return;
    if (g_user_initiated_exit) {
        LOG_DEBUG_T("Supervisor", "Restart", "Skip", "user-initiated exit, not restarting");
        return;
    }
    if (g_config.auto_restart_delay > 0) {
        LOG_DEBUG_T("Supervisor", "Restart", "Delay", "waiting %d seconds", g_config.auto_restart_delay);
        sleep(g_config.auto_restart_delay);
    }

    if (is_throttled()) {
        LOG_WARN_T("Supervisor", "Restart", "Throttled", "too many restarts, entering recovery shell");
        start_recovery_shell();
        return;
    }

    record_restart();
    start_child();
}

static void handle_crash(int status) {
    int signal = WTERMSIG(status);
    int exit_code = WEXITSTATUS(status);

    LOG_WARN_T("Supervisor", "Crash", "Detected", "child PID=%d died, status=%d, signal=%d, exit=%d",
               g_child_pid, status, signal, exit_code);

    const char *meaning = get_exit_meaning(exit_code, "en");
    const char *meaning_zh = get_exit_meaning(exit_code, "zh");
    LOG_WARN_T("Supervisor", "Crash", "ExitMeaning", "exit_code=%d: %s / %s", exit_code, meaning, meaning_zh);

    log_crash(g_child_pid, status, signal);

    if (g_user_initiated_exit) {
        LOG_DEBUG_T("Supervisor", "Crash", "UserExit", "user-initiated exit, not restarting");
        return;
    }

    if (g_shutdown_requested) {
        LOG_DEBUG_T("Supervisor", "Crash", "Shutdown", "shutdown requested, not restarting");
        return;
    }

    if (WIFEXITED(status) && exit_code == EXIT_CONFIG_MISSING) {
        LOG_WARN_T("Supervisor", "Crash", "ConfigMissing", "child exited with CONFIG_MISSING (10)");
        LOG_INFO_T("Supervisor", "Crash", "Recovery", "Entering recovery shell for configuration");
        start_recovery_shell();
        restart_child();
        return;
    }

    if (strcmp(g_config.strategy, "manual") == 0) {
        LOG_INFO_T("Supervisor", "Crash", "Manual", "manual mode, waiting for user input");
        return;
    }

    if (strcmp(g_config.strategy, "recovery_shell") == 0) {
        LOG_INFO_T("Supervisor", "Crash", "Recovery", "entering recovery shell");
        start_recovery_shell();
        restart_child();
        return;
    }

    restart_child();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    log_system_init();
    LOG_INFO_T("Supervisor", "Main", "Start", "LING OS Supervisor v%s starting", LINGOS_VERSION);

    load_config();

    signal(SIGCHLD, sigchld_handler);
    signal(SIGTERM, sigterm_handler);
    signal(SIGUSR1, sigusr1_handler);
    signal(SIGINT, sigint_handler);

    start_child();

    if (g_child_pid < 0) {
        LOG_ERROR_T("Supervisor", "Main", "NoChild", "failed to start child, exiting");
        return 1;
    }

    start_heartbeat_monitor();

    g_running = 1;
    time_t start_time = time(NULL);

    while (!g_shutdown_requested) {
        int status;
        pid_t ret = waitpid(g_child_pid, &status, WNOHANG);

        if (ret == g_child_pid) {
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                LOG_INFO_T("Supervisor", "Main", "ChildExited", "child exited with code %d", exit_code);
                const char *meaning = get_exit_meaning(exit_code, "en");
                LOG_INFO_T("Supervisor", "Main", "ChildExited", "exit meaning: %s", meaning);
                if (exit_code == 0 && g_user_initiated_exit) {
                    LOG_DEBUG_T("Supervisor", "Main", "CleanExit", "normal exit, supervisor stopping");
                    break;
                }
                handle_crash(status);
                if (g_child_pid > 0 && !g_shutdown_requested) start_time = time(NULL);
            } else if (WIFSIGNALED(status)) {
                int signal = WTERMSIG(status);
                LOG_WARN_T("Supervisor", "Main", "ChildSignaled", "child killed by signal %d", signal);
                if (signal == SIGKILL || signal == SIGTERM) {
                    if (g_user_initiated_exit) {
                        LOG_DEBUG_T("Supervisor", "Main", "UserKill", "user-initiated kill, stopping");
                        break;
                    }
                }
                handle_crash(status);
                if (g_child_pid > 0 && !g_shutdown_requested) start_time = time(NULL);
            } else {
                LOG_WARN_T("Supervisor", "Main", "ChildUnknown", "child exited with unknown status");
                handle_crash(status);
                if (g_child_pid > 0 && !g_shutdown_requested) start_time = time(NULL);
            }
        } else if (ret == -1) {
            if (errno == EINTR) continue;
            LOG_ERROR_T("Supervisor", "Main", "WaitFail", "waitpid error: %s", strerror(errno));
            break;
        }

        sleep(1);
    }

    g_running = 0;
    LOG_INFO_T("Supervisor", "Main", "Exit", "supervisor exiting");

    stop_heartbeat_monitor();

    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
        sleep(1);
        kill(g_child_pid, SIGKILL);
        waitpid(g_child_pid, NULL, 0);
    }

    return 0;
}