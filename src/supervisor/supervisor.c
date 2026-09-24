/**
 * @file    src/supervisor/supervisor.c
 * @brief   LING OS 监督者进程（独立恢复）
 * @version LN-0.4.3
 * @par     核心协议：防弹编程（独立进程，最小依赖）
 * @changes 移除空闲超时逻辑，仅依赖心跳监控；心跳间隔改为 2 秒。
 */

#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../lib/lingos_config.h"   /* 【2026-09-19】LINGOS_RUN_DIR / 锁常量 / EXIT_ALREADY_RUNNING */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/file.h>
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
    {EXIT_ALREADY_RUNNING, "Another instance already running", "已有实例在运行（单实例锁）"},
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
        "heartbeat_timeout = 60\n");
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
        /* 【2026-09-19 修复】原直接 SIGKILL 子进程——子进程来不及记"干净退出"，
         *   下次启动误入修复模式，且注册表来不及保存。改为 SIGTERM 优先
         *   （子进程优雅收尾），最多等 2s，仍未退出再强杀兜底。 */
        kill(g_child_pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            int st;
            pid_t r = waitpid(g_child_pid, &st, WNOHANG);
            if (r == g_child_pid) {
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

/* ============================================================
 * 【2026-09-19 新增】单实例锁（防双 supervisor → 双叉子互抢端口）
 *   返回 0 成功（fd 保持打开）；-2 已被占用；-1 无法建锁（不阻塞）
 * ============================================================ */
static int acquire_supervisor_lock(void) {
    const char *root = lingos_data_root();
    char dir[512], path[512];
    safe_snprintf(dir, sizeof(dir), "%s/run", root);
    mkdir(dir, 0755);
    safe_snprintf(path, sizeof(path), "%s/run/supervisor.lock", root);

    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOG_WARN_T("Supervisor", "Lock", "OpenFail", "cannot open %s (%s) — continue without lock",
                   path, strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        char buf[64] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        long other = (n > 0) ? atol(buf) : 0;
        LOG_WARN_T("Supervisor", "Lock", "AlreadyRunning",
                   "another supervisor holds the lock (pid=%ld) — exiting", other);
        close(fd);
        return -2;
    }
    if (ftruncate(fd, 0) == 0) {
        char pidbuf[32];
        int plen = safe_snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)getpid());
        if (plen > 0) { ssize_t w = write(fd, pidbuf, (size_t)plen); (void)w; }
    }
    LOG_INFO_T("Supervisor", "Lock", "OK", "supervisor lock acquired (pid=%d)", (int)getpid());
    return fd;
}

/* ============================================================
 * 【2026-09-19 新增】子进程路径解析
 *   原为相对路径 "./lingos_linux"——依赖 cwd；脚本从任意目录启动、
 *   全捆部署在 bin/ 下时均会 execl 失败。改为多级探测（与主程序同款模式）。
 * ============================================================ */
static void resolve_child_binary(void) {
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            char cand[512];
            safe_snprintf(cand, sizeof(cand), "%s/lingos_linux", exe);
            if (access(cand, X_OK) == 0) {
                safe_strncpy(g_child_binary, cand, sizeof(g_child_binary));
                LOG_INFO_T("Supervisor", "ResolveChild", "OK", "child binary: %s", g_child_binary);
                return;
            }
        }
    }
    if (access("/LINGOS/bin/lingos_linux", X_OK) == 0) {
        safe_strncpy(g_child_binary, "/LINGOS/bin/lingos_linux", sizeof(g_child_binary));
    } else if (access("/LINGOS/lingos_linux", X_OK) == 0) {
        safe_strncpy(g_child_binary, "/LINGOS/lingos_linux", sizeof(g_child_binary));
    } else {
        LOG_WARN_T("Supervisor", "ResolveChild", "Fallback",
                   "no absolute path found — keeping default: %s", g_child_binary);
        return;
    }
    LOG_INFO_T("Supervisor", "ResolveChild", "OK", "child binary: %s", g_child_binary);
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
            char hb[512] = {0};
            if (fgets(hb, sizeof(hb), fp)) {
                long ts = 0;
                /* 【2026-09-24 心跳增强】新格式 JSON：{"ts":...,"services":{...}}
                 *   兼容旧格式（纯数字时间戳）——双解析 */
                char *p = strstr(hb, "\"ts\":");
                if (p) {
                    ts = atol(p + 5);
                } else {
                    ts = atol(hb);
                }
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

static int g_restart_streak = 0;   /* 【2026-09-24】连续快速重启计数（稳定运行 300s 后清零） */

static void restart_child(void) {
    if (g_shutdown_requested) return;
    if (g_user_initiated_exit) {
        LOG_DEBUG_T("Supervisor", "Restart", "Skip", "user-initiated exit, not restarting");
        return;
    }
    if (g_config.auto_restart_delay > 0) {
        /* 【2026-09-24 退避升级】固定 3s → 指数退避（3/6/12/24/48s，封顶 60s）——
         *   对齐 systemd RestartSteps 思路：连续崩溃时先快速恢复、越频繁等待越久 */
        int shift = g_restart_streak < 4 ? g_restart_streak : 4;
        long delay = (long)g_config.auto_restart_delay << shift;
        if (delay > 60) delay = 60;
        LOG_INFO_T("Supervisor", "Restart", "Backoff",
                   "streak=%d, waiting %ld seconds (exponential backoff)", g_restart_streak, delay);
        sleep((unsigned)delay);
    }

    if (is_throttled()) {
        LOG_WARN_T("Supervisor", "Restart", "Throttled", "too many restarts, entering recovery shell");
        start_recovery_shell();
        return;
    }

    g_restart_streak++;
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

    /* 【2026-09-19】单实例锁（防双 supervisor → 双叉子互抢端口） */
    {
        int sup_lock = acquire_supervisor_lock();
        if (sup_lock == -2) {
            LOG_WARN_T("Supervisor", "Main", "AlreadyRunning",
                       "another supervisor instance is running — exiting");
            return EXIT_ALREADY_RUNNING;
        }
        (void)sup_lock;   /* 进程存活期间保持打开 */
    }

    load_config();

    /* 【2026-09-19】子进程路径解析（修复相对路径 "./lingos_linux" 的 cwd 依赖） */
    resolve_child_binary();

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

        /* 【2026-09-24】退避复位：子进程稳定运行 >300s → 清空连续重启计数 */
        if (ret == 0 && g_child_pid > 0 && g_restart_streak > 0 &&
            (time(NULL) - start_time) > 300) {
            LOG_INFO_T("Supervisor", "Restart", "StreakReset",
                       "child stable for >300s — restart streak reset (was %d)", g_restart_streak);
            g_restart_streak = 0;
        }

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
                /* 【2026-09-19】子进程报告"已有实例在运行"（单实例锁失败）→
                 *   本监督者停止（防：双实例互抢端口 + 重启风暴） */
                if (exit_code == EXIT_ALREADY_RUNNING) {
                    LOG_WARN_T("Supervisor", "Main", "AlreadyRunning",
                               "child reports another instance is running — supervisor stopping");
                    uart_puts(tr("\n⚠ Another LING OS instance is already running.\n"
                                 "  This supervisor stops to avoid port conflicts.\n",
                                 "\n⚠ 检测到另一个 LING OS 实例正在运行。\n"
                                 "  本监督者停止以避免端口冲突。\n"));
                    break;
                }
                handle_crash(status);
                /* 【2026-09-19】用户主动退出/停机请求且未被重启（start_child 会重置
                 * 该标志）→ 监督者干净收尾退出（避免 waitpid ECHILD 噪音与空转） */
                if (g_user_initiated_exit || g_shutdown_requested) break;
                if (g_child_pid > 0) start_time = time(NULL);
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
                if (g_user_initiated_exit || g_shutdown_requested) break;   /* 【2026-09-19】同上 */
                if (g_child_pid > 0) start_time = time(NULL);
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
        /* 【2026-09-19】先探活再发信号（防 PID 复用误伤——子进程可能已被回收） */
        if (kill(g_child_pid, 0) == 0) {
            kill(g_child_pid, SIGTERM);
            sleep(1);
            kill(g_child_pid, SIGKILL);
        }
        waitpid(g_child_pid, NULL, 0);
    }

    return 0;
}