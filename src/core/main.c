/**
 * @file    src/core/main.c
 * @brief   LING OS 主入口
 * @version LN-0.4.3
 * @changes 调整 lang_init() 调用时机至 config_core 加载后；
 *          配置向导保存后调用 lang_reload()；
 *          增加 config_core.h 包含；
 *          新增配置存在性检查与询问逻辑（#3）；
 *          增加 after_wizard 标签跳过向导。
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/prctl.h>   /* 【0.7.0 S1-2】PR_SET_PDEATHSIG（父死子收 TERM——防孤儿） */

#include "data_path.h"
#include "linux_io.h"
#include "linux_timer.h"
#include "tcp_client.h"
#include "log_extra.h"
#include "lang.h"
#include "version.h"
#include "shell.h"
#include "syswatch.h"
#include "permission.h"
#include "fs_layout.h"
#include "api_core.h"
#include "nook.h"
#include "defense.h"
#include "ai_master.h"
#include "nook_repair.h"
#include "nook_idle.h"
#include "mode.h"
#include "component_version.h"
#include "file_integrity.h"
#include "self_check.h"
#include "error_shell.h"
#include "test_framework.h"
#include "crash_handler.h"
#include "ai_config.h"
#include "audit.h"
#include "health_trend.h"
#include "health_watchdog.h"
#include "install.h"
#include "state.h"
#include "lingos_config.h"
#include "config_loader.h"
#include "env_bootstrap.h"
#include "connection_handler.h"
#include "discovery_server.h"
#include "dependency_check.h"
#include "safe_string.h"
#include "backup.h"
#include "startup_mode.h"
#include "env_detect.h"
#include "ai_server_protocol.h"
#include "init_cache.h"
#include "registry.h"
#include "security_config.h"
#include "../config/options.h"   /* 【0.7.0 P2】dev 日志选项（dev.debug_log） */
#include "defense_mode.h"
#include "network.h"
#include "startup_ui.h"
#include "background_init.h"
#include "exit_status.h"
#include "repair_mode.h"

/* 新增：自检、安装、配置引擎 */
#include "../health/check_manager.h"
#include "../health/check_items.h"
#include "../install/install_manager.h"
#include "../config/wizard_engine.h"
#include "../config/config_renderer.h"
#include "../config/config_core.h"   /* 用于配置加载和语言同步 */

extern int tui_desktop_run(void);
extern int ensure_daemon_running(void);
extern int ensure_ai_server_running(void);

/* lang_reload 声明（由 lang.h 提供，但为了确保可见） */
extern void lang_reload(void);

#define DISK_WARN_THRESHOLD_MB 100

/* ============================================================
 * AI 看门狗相关
 * ============================================================ */
static volatile int g_ai_watchdog_stop = 0;
static volatile int g_ai_watchdog_running = 0;
static volatile int g_ai_available = 0;
static pthread_t g_ai_watchdog_thread;
static volatile int g_ai_restart_attempts = 0;
static volatile int g_ai_restart_failed = 0;

/* 【2026-09-24】argv 选项（--fast / --safe / --diagnose——server mode 前置框架） */
static int g_opt_fast = 0;      /* 跳过非关键后台检查（快速启动） */
static int g_opt_safe = 0;      /* 最小启动：不含 AI 与 aux 守护 */
static int g_opt_diagnose = 0;  /* 诊断模式：打印环境+就绪后退出（不进 Shell） */

extern int ai_status_query(void);
extern int ensure_ai_server_running(void);

static void* ai_watchdog_thread_func(void *arg) {
    (void)arg;
    LOG_INFO_T("Main", "AIWatchdog", "Start", "AI watchdog thread started");
    g_ai_watchdog_running = 1;
    g_ai_restart_attempts = 0;
    g_ai_restart_failed = 0;

    while (!g_ai_watchdog_stop) {
        sleep(3);
        if (g_ai_watchdog_stop) break;

        int healthy = ai_status_query();

        if (healthy) {
            g_ai_available = 1;
            g_ai_restart_attempts = 0;
            g_ai_restart_failed = 0;
            continue;
        }

        LOG_WARN_T("Main", "AIWatchdog", "Unhealthy", "AI service unhealthy, attempting restart (%d/3)",
                   g_ai_restart_attempts + 1);

        if (g_ai_restart_attempts >= 2) {
            g_ai_restart_failed = 1;
            g_ai_available = 0;
            uart_puts(COLOR_YELLOW);
            uart_puts(tr(
                "\n⚠ AI service is unavailable after 3 restart attempts.\n"
                "  AI features will be disabled.\n"
                "  Please restart the system or manually start AI server.\n",
                "\n⚠ AI 服务不可用，已尝试 3 次重启。\n"
                "  AI 功能将被禁用。\n"
                "  请重启系统或手动启动 AI 服务器。\n"
            ));
            uart_puts(COLOR_RESET);
            LOG_ERROR_T("Main", "AIWatchdog", "GiveUp", "AI service restart failed after 3 attempts");
            g_ai_watchdog_stop = 1;
            break;
        }

        int ret = ensure_ai_server_running();
        g_ai_restart_attempts++;

        if (ret == 0) {
            LOG_INFO_T("Main", "AIWatchdog", "RestartOK", "AI service restarted successfully");
            g_ai_available = 1;
            uart_puts(tr("✅ AI service recovered.\n", "✅ AI 服务已恢复。\n"));
        } else {
            LOG_WARN_T("Main", "AIWatchdog", "RestartFail", "AI service restart failed (attempt %d/3)",
                       g_ai_restart_attempts);
            uart_puts(tr("⚠ AI service restart failed, retrying...\n",
                         "⚠ AI 服务重启失败，正在重试...\n"));
        }
    }

    g_ai_watchdog_running = 0;
    LOG_INFO_T("Main", "AIWatchdog", "Stop", "AI watchdog thread stopped");
    return NULL;
}

static void start_ai_watchdog(void) {
    if (g_ai_watchdog_running) return;
    g_ai_watchdog_stop = 0;
    if (pthread_create(&g_ai_watchdog_thread, NULL, ai_watchdog_thread_func, NULL) != 0) {
        LOG_WARN_T("Main", "AIWatchdog", "ThreadFail", "failed to start AI watchdog");
        return;
    }
    LOG_INFO_T("Main", "AIWatchdog", "Started", "AI watchdog thread started");
}

static void stop_ai_watchdog(void) {
    if (!g_ai_watchdog_running) return;
    g_ai_watchdog_stop = 1;
    pthread_join(g_ai_watchdog_thread, NULL);
    LOG_DEBUG_T("Main", "AIWatchdog", "Stopped", "AI watchdog stopped");
}

/* ============================================================
 * 发送 SIGUSR1 通知监督者
 * ============================================================ */
static void send_exit_signal_to_supervisor(void) {
    pid_t ppid = getppid();
    if (ppid > 1) {
        /* 【2026-09-19 修复】仅当父进程确为 lingos_supervisor 才发 SIGUSR1——
         * 原实现直接 kill(ppid)：终端直跑（./lingos_linux）时父进程是 shell，
         * SIGUSR1 默认动作会误杀 shell。现按 /proc/<pid>/comm 严格判定。 */
        char comm_path[64], comm[64] = {0};
        safe_snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)ppid);
        FILE *cf = fopen(comm_path, "r");
        if (cf) {
            if (fgets(comm, sizeof(comm), cf)) {
                char *nl = strchr(comm, '\n');
                if (nl) *nl = '\0';
            }
            fclose(cf);
        }
        if (strcmp(comm, "lingos_supervisor") != 0) {
            LOG_DEBUG_T("Main", "Exit", "NotifySupervisor", "parent '%s' is not supervisor, skip",
                        comm[0] ? comm : "?");
            return;
        }
        if (kill(ppid, SIGUSR1) == 0) {
            LOG_INFO_T("Main", "Exit", "NotifySupervisor", "Sent SIGUSR1 to supervisor (PID=%d)", ppid);
        } else {
            LOG_WARN_T("Main", "Exit", "NotifyFail", "Failed to send SIGUSR1 to supervisor: %s", strerror(errno));
        }
        usleep(100000);
    }
}

/* ============================================================
 * 心跳写入线程
 * ============================================================ */
static volatile int g_heartbeat_stop = 0;
static pthread_t g_heartbeat_thread;

/* 【2026-09-24】前向声明（心跳内附带 aux 状态采样用） */
static int aux_daemon_running(const char *name);

/* 【2026-09-24 心跳增强】JSON 格式：ts/pid/uptime + 服务状态（每 10s 采样一次）
 *   旧 supervisor 兼容：其解析失败时回退纯数字（本文件同时保留数字回读防御）。
 *   服务状态采样用"文件存在 + /proc 扫描"轻检查——不阻塞心跳。 */
static void* heartbeat_write_thread(void *arg) {
    (void)arg;
    const char *root = lingos_data_root();
    char heartbeat_path[512];
    safe_snprintf(heartbeat_path, sizeof(heartbeat_path), "%s/run/heartbeat", root);

    char run_dir[512];
    safe_snprintf(run_dir, sizeof(run_dir), "%s/run", root);
    mkdir(run_dir, 0755);

    time_t start = time(NULL);
    int beat = 0;
    int svc_lingosd = 0, svc_ai = 0, svc_alertd = 0;
    while (!g_heartbeat_stop) {
        time_t now = time(NULL);
        if (beat % 10 == 0) {
            char p[512];
            safe_snprintf(p, sizeof(p), "%s/run/daemon.sock", root);
            svc_lingosd = (access(p, F_OK) == 0);
            safe_snprintf(p, sizeof(p), "%s/run/ai.sock", root);
            svc_ai = (access(p, F_OK) == 0);
            svc_alertd = aux_daemon_running("lingos_alertd");
        }
        FILE *fp = fopen(heartbeat_path, "w");
        if (fp) {
            fprintf(fp, "{\"ts\":%ld,\"pid\":%d,\"uptime\":%ld,"
                        "\"services\":{\"lingosd\":%d,\"ai\":%d,\"alertd\":%d}}\n",
                    (long)now, (int)getpid(), (long)(now - start),
                    svc_lingosd, svc_ai, svc_alertd);
            fclose(fp);
        }
        beat++;
        sleep(1);
    }
    return NULL;
}

static void start_heartbeat_writer(void) {
    if (pthread_create(&g_heartbeat_thread, NULL, heartbeat_write_thread, NULL) != 0) {
        LOG_WARN_T("Main", "Heartbeat", "ThreadFail", "failed to start heartbeat writer");
        return;
    }
    LOG_DEBUG_T("Main", "Heartbeat", "Started", "heartbeat writer thread started");
}

static void stop_heartbeat_writer(void) {
    if (!g_heartbeat_thread) return;
    g_heartbeat_stop = 1;
    pthread_join(g_heartbeat_thread, NULL);
    LOG_DEBUG_T("Main", "Heartbeat", "Stopped", "heartbeat writer stopped");
}

/* ============================================================
 * 磁盘空间检查
 * ============================================================ */
static int check_disk_space_and_prompt(void) {
    LOG_DEBUG_T("Main", "CheckDisk", "enter", "checking disk space");
    const char *root = lingos_data_root();
    struct statvfs stvfs;
    if (statvfs(root, &stvfs) != 0) {
        LOG_WARN_T("Main", "CheckDisk", "StatvfsFail", "statvfs failed: %s", strerror(errno));
        return 0;
    }

    unsigned long long free_space = (unsigned long long)stvfs.f_bsize * stvfs.f_bavail;
    unsigned long long free_mb = free_space / (1024 * 1024);
    if (free_mb < DISK_WARN_THRESHOLD_MB) {
        uart_puts(COLOR_YELLOW);
        uart_puts(tr(
            "\n⚠ Disk space is low (",
            "\n⚠ 磁盘空间不足 ("
        ));
        char buf[32];
        safe_snprintf(buf, sizeof(buf), "%llu", free_mb);
        uart_puts(buf);
        uart_puts(tr(" MB available).\n", " MB 可用)。\n"));
        uart_puts(tr("Run cleanup now? (y/N): ", "立即清理？(y/N): "));
        uart_puts(COLOR_RESET);
        char c = uart_getc();
        uart_putc(c);
        uart_puts("\n");

        if (c == 'y' || c == 'Y') {
            uart_puts(tr("Cleaning up...\n", "正在清理...\n"));
            const char *root2 = lingos_data_root();
            char log_dir[512];
            safe_snprintf(log_dir, sizeof(log_dir), "%s/Debug", root2);
            DIR *d = opendir(log_dir);
            if (d) {
                struct dirent *entry;
                time_t now = time(NULL);
                while ((entry = readdir(d)) != NULL) {
                    if (entry->d_name[0] == '.') continue;
                    if (strncmp(entry->d_name, "lingos_", 7) == 0) {
                        char full_path[512];
                        safe_snprintf(full_path, sizeof(full_path), "%s/%s", log_dir, entry->d_name);
                        struct stat st;
                        if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
                            if (now - st.st_mtime > 86400) {
                                unlink(full_path);
                            }
                        }
                    }
                }
                closedir(d);
            }
            uart_puts(tr("Cleanup completed.\n", "清理完成。\n"));
            return 1;
        } else {
            uart_puts(tr("Cleanup skipped.\n", "已跳过清理。\n"));
        }
    }
    return 0;
}

/* ============================================================
 * 服务健康检查
 * ============================================================ */
static int read_line_timeout(int fd, char *buf, size_t buf_size, int timeout_sec) {
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t pos = 0;
    while (pos < buf_size - 1) {
        ssize_t n = read(fd, buf + pos, 1);
        if (n <= 0) return -1;
        if (buf[pos] == '\n') {
            buf[pos] = '\0';
            return pos;
        }
        pos++;
    }
    buf[pos] = '\0';
    return pos;
}

/* 【0.7.0 P2】开发调试日志判定（先生设定：版本号 + 内部变量——内部变量优先）
 *   判定链：
 *     ① 内部变量 build_channel（更新包写入 /LINGOS/state/build_channel）
 *        = "release" → 强制关（正式版）
 *        = "dev"     → 开
 *     ② 版本号：0.x → 开；≥1.0 → 关
 *     ③ 快捷开关：选项 dev.debug_log=0（用户在 App/Web 手动关）→ 关（手动覆盖向下生效）
 *   落地：log_set_global_level(DEBUG/INFO)；文件全量写入不受影响（"日志不被清除"方向） */
static void apply_dev_log_policy(void) {
    int enable;
    const char *v = version_get();
    const char *num = (v && strncmp(v, "LN-", 3) == 0) ? v + 3 : (v ? v : "0");
    enable = (atoi(num) == 0) ? 1 : 0;   /* ② 版本号 */

    FILE *fp = fopen("/LINGOS/state/build_channel", "r");   /* ① 内部变量（优先） */
    if (fp) {
        char b[32] = {0};
        if (fgets(b, sizeof(b), fp)) {
            if (strncmp(b, "release", 7) == 0) enable = 0;
            else if (strncmp(b, "dev", 3) == 0) enable = 1;
        }
        fclose(fp);
    }

    if (options_get("dev.debug_log") == 0) enable = 0;      /* ③ 手动关 */

    log_set_global_level(enable ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);
    LOG_INFO_T("Main", "DevLog", "Policy", "debug logging %s (version=%s) — 判定：内部变量>版本>手动关",
               enable ? "ON" : "OFF", v ? v : "?");
}

static int is_service_healthy(const char *socket_path) {
    if (!socket_path) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return 0;
    }
    const char *ping_msg = "{\"cmd\":\"ping\"}\n";
    if (write(fd, ping_msg, strlen(ping_msg)) < 0) {
        close(fd);
        return 0;
    }

    char buf[256];
    if (read_line_timeout(fd, buf, sizeof(buf), 3) <= 0) {
        close(fd);
        return 0;
    }
    close(fd);
    return (strstr(buf, "\"pong\"") != NULL);
}

/* 【0.7.0 S1-4 修复】等待 registry.sock **可连接**（非仅文件存在）
 *   旧行为：文件已存在但 lingosd 监听未就绪 → ai_server 连接拒绝 →
 *   技能表退回内置（先生真机日志："Registry connection refused" 刷屏根因）。
 *   每 500ms 重试一次；超时返回 -1（不阻断启动，仅告警）。 */
static int wait_registry_connectable(const char *path, int timeout_sec) {
    if (!path) return -1;
    for (int i = 0; i < timeout_sec * 2; i++) {
        if (access(path, F_OK) == 0) {
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd >= 0) {
                struct sockaddr_un addr;
                memset(&addr, 0, sizeof(addr));
                addr.sun_family = AF_UNIX;
                safe_strncpy(addr.sun_path, path, sizeof(addr.sun_path));
                addr.sun_path[sizeof(addr.sun_path)-1] = '\0';
                int ok = (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
                close(fd);
                if (ok) return 0;
            }
        }
        usleep(500000);
    }
    return -1;
}

/* ============================================================
 * 【0.7.0 S1-2/S1-3 修复】子守护 fork 后统一准备
 *   ① PR_SET_PDEATHSIG=SIGTERM —— 父进程死亡时内核自动向子进程发 TERM
 *      （旧行为：主程序退出后 lingosd/alertd/voiced/ai_server 残留 → 端口占用/刷屏，
 *        先生真机 2026-09-24：Ctrl-C 后 voiced 仍在"🎤 我在"）
 *   ② stdout/stderr 重定向到 /LINGOS/log/<name>.log
 *      （旧行为：守护输出继承终端 → [LogExtra] 初始化×N/matplotlib/唤醒词框 污染）
 *   ③ 防竞态：设置 PDEATHSIG 后复查父进程是否已死（若已死主动退出）
 * ============================================================ */
static void child_daemon_prepare(const char *name) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1) _exit(0);   /* 设置前父已死 → 立即退出 */

    char logpath[512];
    safe_snprintf(logpath, sizeof(logpath), "%s/log/%s.log",
                  lingos_data_root(), (name && *name) ? name : "child");
    int lfd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (lfd >= 0) {
        dup2(lfd, STDOUT_FILENO);
        dup2(lfd, STDERR_FILENO);
        if (lfd > STDERR_FILENO) close(lfd);
    } else {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
            if (dn > STDERR_FILENO) close(dn);
        }
    }
}

/* 【0.7.0 S1-2】主程序退出时收尾子守护（读 pid 文件 → TERM → 800ms → KILL）
 *   双保险之一（另一保险 = PR_SET_PDEATHSIG 内核级）；防"Ctrl-C 后仍在跑"。 */
static void terminate_children(void) {
    static const char *files[] = {
        LINGOS_RUN_DIR "/lingosd.pid",
        LINGOS_RUN_DIR "/lingos_alertd.pid",
        LINGOS_RUN_DIR "/lingos_visiond.pid",
        LINGOS_RUN_DIR "/lingos_voiced.pid",
        LINGOS_RUN_DIR "/ai_server.pid",
    };
    pid_t pids[8];
    int n = 0;
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]) && n < 8; i++) {
        FILE *fp = fopen(files[i], "r");
        if (!fp) continue;
        int pid = 0;
        if (fscanf(fp, "%d", &pid) == 1 && pid > 1) pids[n++] = pid;
        fclose(fp);
        unlink(files[i]);
    }
    if (n == 0) return;
    for (int i = 0; i < n; i++) kill(pids[i], SIGTERM);
    usleep(800000);
    for (int i = 0; i < n; i++) {
        if (kill(pids[i], 0) == 0) kill(pids[i], SIGKILL);
    }
    LOG_INFO_T("Main", "Exit", "ChildrenStopped", "terminated %d child process(es)", n);
}

static void cleanup_stale_processes(const char *pid_file, const char *socket_path) {
    if (!pid_file || !socket_path) return;
    FILE *fp = fopen(pid_file, "r");
    if (fp) {
        int pid;
        if (fscanf(fp, "%d", &pid) == 1) {
            if (kill(pid, 0) == 0) {
                LOG_WARN_T("Main", "CleanupStale", "KillStale", "killing stale process PID=%d", pid);
                kill(pid, SIGTERM);
                sleep(1);
                if (kill(pid, 0) == 0) kill(pid, SIGKILL);
            }
        }
        fclose(fp);
        unlink(pid_file);
    }
    if (access(socket_path, F_OK) == 0) unlink(socket_path);
}

/* ============================================================
 * 【0.6.0】辅助守护进程拉起（alertd=生命线 / visiond / voiced）
 *   背景：三守护此前从不被打包也不被拉起（预警系统整体停摆根因之一）。
 *   策略：软启动——失败仅告警不阻塞主程序（兼容旧包缺二进制场景）。
 *   健康判断：/proc 扫描进程名（三守护无 socket 协议）。
 * ============================================================ */
static int aux_daemon_running(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char cpath[64];
        safe_snprintf(cpath, sizeof(cpath), "/proc/%s/comm", e->d_name);
        FILE *cf = fopen(cpath, "r");
        if (cf) {
            char comm[64] = {0};
            if (fgets(comm, sizeof(comm), cf)) {
                size_t l = strlen(comm);
                if (l && comm[l - 1] == '\n') comm[l - 1] = '\0';
                if (strcmp(comm, name) == 0) found = 1;
            }
            fclose(cf);
        }
        if (found) break;
    }
    closedir(d);
    return found;
}

static void ensure_aux_daemon(const char *name) {
    if (!name) return;
    if (aux_daemon_running(name)) {
        LOG_DEBUG_T("Main", "AuxDaemon", "AlreadyRunning", "%s is running", name);
        return;
    }
    /* 路径探测：与 lingosd 同优先级（同目录 → /LINGOS/bin → /LINGOS） */
    static char aux_path_buf[512];
    char cand[512];
    const char *path = NULL;
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            safe_snprintf(cand, sizeof(cand), "%s/%s", exe, name);
            if (access(cand, X_OK) == 0) {
                safe_snprintf(aux_path_buf, sizeof(aux_path_buf), "%s", cand);
                path = aux_path_buf;
            }
        }
    }
    if (!path) {
        safe_snprintf(cand, sizeof(cand), "/LINGOS/bin/%s", name);
        if (access(cand, X_OK) == 0) { safe_snprintf(aux_path_buf, sizeof(aux_path_buf), "%s", cand); path = aux_path_buf; }
    }
    if (!path) {
        safe_snprintf(cand, sizeof(cand), "/LINGOS/%s", name);
        if (access(cand, X_OK) == 0) { safe_snprintf(aux_path_buf, sizeof(aux_path_buf), "%s", cand); path = aux_path_buf; }
    }
    if (!path) {
        LOG_WARN_T("Main", "AuxDaemon", "NotFound", "%s not found (skip)", name);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        child_daemon_prepare(name);   /* 【0.7.0 S1-2/S1-3】PDEATHSIG + 输出重定向 */
        execl(path, path, (char*)NULL);
        _exit(1);
    } else if (pid > 0) {
        char pfile[256];
        safe_snprintf(pfile, sizeof(pfile), "%s/%s.pid", LINGOS_RUN_DIR, name);
        FILE *fp = fopen(pfile, "w");
        if (fp) { fprintf(fp, "%d\n", pid); fclose(fp); }
        sleep(1);
        if (kill(pid, 0) == 0) {
            LOG_INFO_T("Main", "AuxDaemon", "Started", "%s PID=%d", name, pid);
        } else {
            LOG_WARN_T("Main", "AuxDaemon", "DiedImmediately",
                       "%s exited right after start (missing deps?), continuing", name);
        }
    }
}

int ensure_daemon_running(void) {
    LOG_INFO_T("Main", "EnsureDaemon", "Enter", "starting lingosd");
    /*
     * 【0.4.4 修复】原为相对路径 "./lingosd" —— 强依赖当前工作目录。
     * 现象（先生 2026-09-12 实测）：从 ~ 启动时 cwd 不是 /LINGOS →
     *   execl("./lingosd") 失败 → lingosd 起不来 → 8080/2939 端口全不通。
     * 修法：按「同目录(相对 /proc/self/exe) → /LINGOS/bin → /LINGOS → ./」优先级探测。
     */
    static char daemon_buf[512];
    const char *daemon_path = "./lingosd";
    {
        /* 1) 二进制同目录（/LINGOS/bin → /LINGOS/bin/lingosd） */
        char exe[512];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            char *slash = strrchr(exe, '/');
            if (slash) {
                *slash = '\0';
                safe_snprintf(daemon_buf, sizeof(daemon_buf), "%s/lingosd", exe);
                if (access(daemon_buf, X_OK) == 0) daemon_path = daemon_buf;
            }
        }
        /* 2) /LINGOS/bin/lingosd */
        if (daemon_path[0] == '.' && access("/LINGOS/bin/lingosd", X_OK) == 0) {
            daemon_path = "/LINGOS/bin/lingosd";
        }
        /* 3) /LINGOS/lingosd（历史软链位置） */
        else if (daemon_path[0] == '.' && access("/LINGOS/lingosd", X_OK) == 0) {
            daemon_path = "/LINGOS/lingosd";
        }
        LOG_INFO_T("Main", "EnsureDaemon", "Path", "using %s", daemon_path);
    }
    const char *pid_path = LINGOS_RUN_DIR "/lingosd.pid";
    const char *socket_path = DAEMON_SOCKET_PATH;
    int max_retries = 3;

    cleanup_stale_processes(pid_path, socket_path);

    for (int attempt = 1; attempt <= max_retries; attempt++) {
        if (is_service_healthy(socket_path)) {
            LOG_INFO_T("Main", "EnsureDaemon", "AlreadyHealthy", "lingosd is healthy");
            return 0;
        }

        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            child_daemon_prepare("lingosd");   /* 【0.7.0 S1-2/S1-3】PDEATHSIG + 输出重定向 */
            execl(daemon_path, daemon_path, (char*)NULL);
            perror("execl lingosd");
            _exit(1);
        } else if (pid > 0) {
            FILE *fp = fopen(pid_path, "w");
            if (fp) {
                fprintf(fp, "%d\n", pid);
                fclose(fp);
            }
            for (int wait_sec = 0; wait_sec < 3; wait_sec++) {
                if (is_service_healthy(socket_path)) {
                    LOG_INFO_T("Main", "EnsureDaemon", "Started", "lingosd PID=%d", pid);
                    return 0;
                }
                sleep(1);
            }
            LOG_WARN_T("Main", "EnsureDaemon", "AttemptFail", "attempt %d/%d failed", attempt, max_retries);
            kill(pid, SIGTERM);
            sleep(1);
            if (kill(pid, 0) == 0) kill(pid, SIGKILL);
            unlink(pid_path);
        }
    }

    uart_puts(COLOR_RED);
    uart_puts(tr(
        "\n[FATAL] Failed to start lingosd daemon after 3 attempts.\n",
        "\n[致命错误] 启动 lingosd 守护进程失败，已尝试 3 次。\n"
    ));
    uart_puts(tr(
        "Please manually start it: ./lingosd &\n",
        "请手动启动：./lingosd &\n"
    ));
    uart_puts(COLOR_RESET);
    return -1;
}

int ensure_ai_server_running(void) {
    LOG_INFO_T("Main", "EnsureAI", "Enter", "starting AI server");
    const char *script_path = "/LINGOS/bin/ai_server.py";
    const char *pid_path = LINGOS_RUN_DIR "/ai_server.pid";
    const char *socket_path = AI_SOCKET_PATH;
    int max_retries = 4;

    if (access(script_path, F_OK) != 0) {
        if (access("src/python/ai_server.py", F_OK) == 0) {
            /* 【0.7.0 S0-1 修复】全量复制（此前仅复制 ai_server.py 单文件 →
             * 其余 37 个模块缺失/过旧 → App 命令 Unknown。先生真机取证根因） */
            char cmd[1200];
            safe_snprintf(cmd, sizeof(cmd),
                "mkdir -p /LINGOS/bin/plugin && cp src/python/*.py /LINGOS/bin/ 2>/dev/null; "
                "cp src/python/plugin/*.py /LINGOS/bin/plugin/ 2>/dev/null; "
                "rm -rf /LINGOS/bin/__pycache__; chmod +x /LINGOS/bin/*.py 2>/dev/null; true");
            int rc = system(cmd);
            LOG_INFO_T("Main", "EnsureAI", "SyncScripts", "deployed python scripts to /LINGOS/bin (rc=%d)", rc);
        } else {
            LOG_ERROR_T("Main", "EnsureAI", "NoScript", "ai_server.py not found");
            return -1;
        }
    } else if (access("src/python/ai_server.py", F_OK) == 0) {
        /* 【0.7.0 S0-1】已存在时：源更新则全量同步（防"装好但跑老版"） */
        struct stat st_src, st_dst;
        if (stat("src/python/ai_server.py", &st_src) == 0 && stat(script_path, &st_dst) == 0
            && st_src.st_mtime > st_dst.st_mtime) {
            char cmd[1200];
            safe_snprintf(cmd, sizeof(cmd),
                "mkdir -p /LINGOS/bin/plugin && cp src/python/*.py /LINGOS/bin/ 2>/dev/null; "
                "cp src/python/plugin/*.py /LINGOS/bin/plugin/ 2>/dev/null; "
                "rm -rf /LINGOS/bin/__pycache__; chmod +x /LINGOS/bin/*.py 2>/dev/null; true");
            int rc = system(cmd);
            LOG_INFO_T("Main", "EnsureAI", "ResyncScripts", "source newer → resynced python scripts (rc=%d)", rc);
        }
    }

    cleanup_stale_processes(pid_path, socket_path);

    /* 【0.7.0 S1-4】先等 registry.sock 可连接（技能表加载依赖）——最多 10s，超时不阻断 */
    if (wait_registry_connectable("/LINGOS/run/registry.sock", 10) == 0) {
        LOG_INFO_T("Main", "EnsureAI", "RegistryReady", "registry.sock connectable");
    } else {
        LOG_WARN_T("Main", "EnsureAI", "RegistrySlow", "registry.sock not connectable after 10s (AI will use builtin skill schemas)");
    }

    for (int attempt = 1; attempt <= max_retries; attempt++) {
        if (is_service_healthy(socket_path)) {
            LOG_INFO_T("Main", "EnsureAI", "AlreadyHealthy", "AI server is healthy");
            return 0;
        }

        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            child_daemon_prepare("ai_server");   /* 【0.7.0 S1-2/S1-3】PDEATHSIG + 输出重定向 */
            /*
             * 【0.4.4 修复】LD_LIBRARY_PATH 污染 → python SSL 不可用
             * 现象：allbin 包 start.sh 全局 export LD_LIBRARY_PATH=<pkg>/lib，
             *      子进程继承后，python3 的 _ssl 模块优先加载包内老 libcrypto
             *      （缺 OPENSSL_3.3.0 符号）→ import ssl 失败 →
             *      余额查询/DeepSeek 连接全挂 → AI 回复空白。
             * 修法：C 端自身已由 rpath($ORIGIN/../lib) 完成动态库加载，
             *      启动 python 子进程前清掉该变量，让 python 用系统库。
             */
            unsetenv("LD_LIBRARY_PATH");
            /* 优先用显式解释器（避免 PATH 解析到 venv/proot loader 的坏 python） */
            const char *py = getenv("LINGOS_PYTHON");
            if (!py || !*py) py = "/usr/bin/python3";
            if (access(py, X_OK) != 0) py = "python3";
            execlp(py, py, "-u", script_path, (char*)NULL);
            perror("execlp python3");
            _exit(1);
        } else if (pid > 0) {
            FILE *fp = fopen(pid_path, "w");
            if (fp) {
                fprintf(fp, "%d\n", pid);
                fclose(fp);
            }
            /* 【0.7.0 S1-5 修复】首轮等待 3s→8s
             *   Python 冷启动实测 3~5s+（首次 import 大量模块）——先生真机
             *   "attempt 1/4 failed" 即差 0.2s 误杀刚起来的实例（后续重试成功）。 */
            int wait_time = (attempt == 1) ? 8 : (3 << (attempt - 1));
            if (wait_time > 16) wait_time = 16;
            for (int w = 0; w < wait_time; w++) {
                if (is_service_healthy(socket_path)) {
                    LOG_INFO_T("Main", "EnsureAI", "Started", "PID=%d", pid);
                    return 0;
                }
                sleep(1);
            }
            LOG_WARN_T("Main", "EnsureAI", "AttemptFail", "attempt %d/%d failed", attempt, max_retries);
            kill(pid, SIGTERM);
            sleep(1);
            if (kill(pid, 0) == 0) kill(pid, SIGKILL);
            unlink(pid_path);
        }
    }

    uart_puts(COLOR_RED);
    uart_puts(tr(
        "\n[FATAL] Failed to start AI server after 4 attempts.\n",
        "\n[致命错误] 启动 AI 服务器失败，已尝试 4 次。\n"
    ));
    uart_puts(tr(
        "Please manually start it: python3 /LINGOS/bin/ai_server.py &\n",
        "请手动启动：python3 /LINGOS/bin/ai_server.py &\n"
    ));
    uart_puts(COLOR_RESET);
    return -1;
}

/* ============================================================
 * 【2026-09-24 新增】服务守护线程（监督树补全——第二层 liveness）
 *   · lingosd（每 5s）：socket 健康检查 → 失败重拉（复用 ensure_daemon_running）
 *   · aux 守护（每 30s）：alertd/visiond/voiced /proc 检查 → 缺失软拉回
 *   （AI 服务另有专用 AI watchdog——本线程不含 AI）
 * ============================================================ */
static volatile int g_svc_watchdog_stop = 0;
static volatile int g_svc_watchdog_running = 0;
static pthread_t g_svc_watchdog_thread;

static void* service_watchdog_thread_func(void *arg) {
    (void)arg;
    LOG_INFO_T("Main", "ServiceWatchdog", "Start", "service watchdog started (lingosd + aux)");
    g_svc_watchdog_running = 1;

    int ticks = 0;
    int lingosd_fail = 0;
    int lingosd_gave_up = 0;

    while (!g_svc_watchdog_stop) {
        sleep(5);
        if (g_svc_watchdog_stop) break;
        ticks++;

        /* ---- lingosd（每 5s 检查；2 次失败后重拉，最多 3 次重试） ---- */
        if (!is_service_healthy(DAEMON_SOCKET_PATH)) {
            lingosd_fail++;
            LOG_WARN_T("Main", "ServiceWatchdog", "LingosdDown",
                       "lingosd unhealthy (streak=%d)", lingosd_fail);
            if (lingosd_fail >= 2 && lingosd_fail <= 4 && !lingosd_gave_up) {
                if (ensure_daemon_running() == 0) {
                    LOG_INFO_T("Main", "ServiceWatchdog", "LingosdRecovered",
                               "lingosd restarted successfully");
                    uart_puts(tr("✅ lingosd recovered (service watchdog).\n",
                                 "✅ lingosd 已自动恢复（服务守护）。\n"));
                    lingosd_fail = 0;
                } else {
                    LOG_WARN_T("Main", "ServiceWatchdog", "LingosdRestartFail",
                               "lingosd restart failed (attempt %d)", lingosd_fail - 1);
                }
            } else if (lingosd_fail == 5) {
                lingosd_gave_up = 1;
                LOG_ERROR_T("Main", "ServiceWatchdog", "LingosdGiveUp",
                            "lingosd recovery failed after retries — giving up (manual fix needed)");
                uart_puts(COLOR_RED);
                uart_puts(tr("\n⚠ lingosd (API core 8080/2939) unavailable — auto-recovery failed.\n"
                             "  Try: bash lingos.sh doctor  (or restart the system)\n",
                             "\n⚠ lingosd（API 核心 8080/2939）不可用——自动恢复失败。\n"
                             "  排查：bash lingos.sh doctor（或重启系统）\n"));
                uart_puts(COLOR_RESET);
            }
        } else {
            if (lingosd_fail >= 2) {
                LOG_INFO_T("Main", "ServiceWatchdog", "LingosdBack", "lingosd back to healthy");
            }
            lingosd_fail = 0;
            lingosd_gave_up = 0;
        }

        /* ---- aux 守护（每 30s = 6 ticks；--safe 模式跳过） ---- */
        if (ticks % 6 == 0 && !g_opt_safe) {
            static const char *aux_names[] = { "lingos_alertd", "lingos_visiond", "lingos_voiced" };
            for (size_t i = 0; i < sizeof(aux_names) / sizeof(aux_names[0]); i++) {
                if (!aux_daemon_running(aux_names[i])) {
                    LOG_WARN_T("Main", "ServiceWatchdog", "AuxMissing",
                               "%s missing — relaunching", aux_names[i]);
                    ensure_aux_daemon(aux_names[i]);
                }
            }
        }
    }

    g_svc_watchdog_running = 0;
    LOG_INFO_T("Main", "ServiceWatchdog", "Stop", "service watchdog stopped");
    return NULL;
}

static void start_service_watchdog(void) {
    if (g_svc_watchdog_running) return;
    g_svc_watchdog_stop = 0;
    if (pthread_create(&g_svc_watchdog_thread, NULL, service_watchdog_thread_func, NULL) != 0) {
        LOG_WARN_T("Main", "ServiceWatchdog", "ThreadFail", "failed to start service watchdog");
        return;
    }
    LOG_INFO_T("Main", "ServiceWatchdog", "Started", "service watchdog thread started");
}

static void stop_service_watchdog(void) {
    if (!g_svc_watchdog_thread) return;
    g_svc_watchdog_stop = 1;
    pthread_join(g_svc_watchdog_thread, NULL);
}

/* ============================================================
 * 正常退出函数
 * ============================================================ */
/* 【2026-09-19】normal_exit 参数化：支持自定义退出原因（信号停止等场景）；
 * 原实现所有路径统一记 "Normal exit"——信号退出会覆盖异常标记（修复模式死机制根因） */
static void normal_exit_with_reason(int exit_code, const char *reason) {
    LOG_INFO_T("Main", "Exit", "Normal", "Exiting with code %d (reason: %s)",
               exit_code, reason ? reason : "Normal exit");

    exit_status_mark_clean(exit_code, reason ? reason : "Normal exit");
    stop_service_watchdog();
    stop_ai_watchdog();
    stop_background_initialization();
    stop_heartbeat_writer();
    send_exit_signal_to_supervisor();

    /* 【0.7.0 S1-2】收尾子守护（防残留——Ctrl-C 后 lingosd/alertd/voiced 仍在跑） */
    terminate_children();

    /* 【0.7.0 S3-1】删除就绪文件（防停止后仍显示"服务=1"过期状态） */
    {
        char rpath2[512];
        safe_snprintf(rpath2, sizeof(rpath2), "%s/run/ready", lingos_data_root());
        unlink(rpath2);
    }

    LOG_DEBUG_T("Main", "Exit", "Cleanup", "Saving registry with timeout");
    pid_t pid = fork();
    if (pid == 0) {
        registry_save();
        exit(0);
    } else if (pid > 0) {
        int status;
        int waited = 0;
        while (waited < 5) {
            if (waitpid(pid, &status, WNOHANG) == pid) {
                break;
            }
            sleep(1);
            waited++;
        }
        if (waited >= 5) {
            kill(pid, SIGKILL);
            LOG_WARN_T("Main", "Exit", "RegistrySaveTimeout", "registry_save timed out, killed");
        }
    } else {
        LOG_WARN_T("Main", "Exit", "ForkFail", "fork for registry_save failed, skipping");
    }

    security_config_save();
    audit_save_to_file(NULL);
    exit(exit_code);
}

/* 【2026-09-19】默认正常退出（包装） */
static void normal_exit(int exit_code) {
    normal_exit_with_reason(exit_code, "Normal exit");
}

/* ============================================================
 * 信号处理
 * ============================================================ */
static void signal_exit_handler(int sig) {
    LOG_WARN_T("Main", "Signal", "Received", "signal=%d, exiting gracefully", sig);
    /* 【2026-09-19 修复】原实现：mark_abnormal(sig) → normal_exit(→mark_clean)，
     * 异常标记被立即覆盖 → 修复模式永不触发（死机制根因之二）。
     * 现语义定稿：
     *   · 信号退出（TERM/INT）= 优雅停止 → 记 clean（原因写明信号，可追溯）
     *   · 崩溃/断电/强杀 = 无机会标记 → 运行脏标记残留 → 下次启动检出异常 */
    char reason[64];
    safe_snprintf(reason, sizeof(reason), "Stopped (signal %d)", sig);
    normal_exit_with_reason(128 + sig, reason);
}

static void emergency_output(const char *msg) {
    if (!msg) return;
    write(STDERR_FILENO, msg, strlen(msg));
}

/* ============================================================
 * 【2026-09-19 新增】单实例锁（flock——防双实例端口冲突）
 *   背景：先生环境日志实证一天 4 次端口冲突（2937/2939/8080/8088 全 BindFail）
 *   语义：锁被占 → 读锁内 PID → 明确提示并返回 -2（main 退出码 75）
 *         supervisor 见 75 码停止重启循环（防互抢）
 *   说明：O_CLOEXEC——fork+exec 的子进程自动释放 fd，不误持锁
 * ============================================================ */
static int acquire_instance_lock(void) {
    const char *root = lingos_data_root();
    char dir[512], path[512];
    safe_snprintf(dir, sizeof(dir), "%s/run", root);
    mkdir(dir, 0755);
    safe_snprintf(path, sizeof(path), "%s/run/lingos.lock", root);

    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOG_WARN_T("Main", "InstanceLock", "OpenFail",
                   "cannot open %s (%s) — continuing without lock", path, strerror(errno));
        return -1;   /* 无法建锁不阻塞启动（防弹——如只读环境） */
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        char buf[64] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        long other = (n > 0) ? atol(buf) : 0;
        char msg[512];
        if (other > 0) {
            safe_snprintf(msg, sizeof(msg),
                "\n[LING OS] 检测到已有实例正在运行（PID %ld）——本次启动退出。\n"
                "[LING OS] Another instance is already running (PID %ld) — exiting.\n"
                "[LING OS] 提示：如需重启请用  bash lingos.sh restart\n\n",
                other, other);
        } else {
            safe_strncpy(msg,
                "\n[LING OS] 检测到已有实例正在运行——本次启动退出。\n"
                "[LING OS] Another instance is already running — exiting.\n"
                "[LING OS] 提示：如需重启请用  bash lingos.sh restart\n\n",
                sizeof(msg));
        }
        write(STDERR_FILENO, msg, strlen(msg));
        LOG_WARN_T("Main", "InstanceLock", "AlreadyRunning",
                   "another instance holds the lock (pid=%ld)", other);
        close(fd);
        return -2;
    }

    /* 写入本进程 PID（供诊断/脚本读取） */
    if (ftruncate(fd, 0) == 0) {
        char pidbuf[32];
        int plen = safe_snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)getpid());
        if (plen > 0) {
            ssize_t w = write(fd, pidbuf, (size_t)plen);
            (void)w;
        }
    }
    LOG_INFO_T("Main", "InstanceLock", "OK", "instance lock acquired (pid=%d)", (int)getpid());
    return fd;   /* 保持打开——进程存活期间持锁；退出自动释放 */
}

/* ============================================================
 * 【2026-09-19 新增】等待 registry.sock 就绪
 *   背景：ai_server 经 registry.sock 加载技能注册表；此前主程序不等它 →
 *   首启/慢环境 AI 回退内置技能表（自定义技能缺席）。超时仅告警不阻塞。
 * ============================================================ */
static void wait_for_registry_ready(int timeout_sec) {
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/run/registry.sock", lingos_data_root());
    for (int i = 0; i < timeout_sec * 2; i++) {
        if (access(path, F_OK) == 0) {
            LOG_DEBUG_T("Main", "RegistryWait", "Ready", "registry.sock present after %d ms", i * 500);
            return;
        }
        usleep(500000);
    }
    LOG_WARN_T("Main", "RegistryWait", "Timeout",
               "registry.sock not ready after %ds — AI may fall back to built-in skills", timeout_sec);
}

/* ============================================================
 * 【2026-09-19 新增】启动就绪报告（到 Shell 前打印 + 写 run/ready 文件）
 *   对应主流（systemd Type=notify / K8s readiness）的轻量对应物：
 *   用户一眼可见"哪些服务就绪、哪些降级"，不再靠猜。
 * ============================================================ */
static int check_local_port(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    /* 非阻塞 + 500ms 超时——启动路径绝不因端口探测挂起 */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int ok = 0;
    int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (r == 0) {
        ok = 1;
    } else if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        if (select(fd + 1, NULL, &wfds, NULL, &tv) > 0) {
            int soerr = 0;
            socklen_t slen = sizeof(soerr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 && soerr == 0) {
                ok = 1;
            }
        }
    }
    close(fd);
    return ok;
}

static void rd_line(const char *label, int ok) {
    uart_puts(ok ? COLOR_GREEN : COLOR_YELLOW);
    uart_puts("  ");
    uart_puts(label);
    uart_puts(ok ? "  [ OK ]\n" : "  [ -- ]\n");
    uart_puts(COLOR_RESET);
}

static void print_readiness_report(void) {
    int ok_daemon  = is_service_healthy(DAEMON_SOCKET_PATH);
    int ok_ai      = is_service_healthy(AI_SOCKET_PATH);
    int ok_alertd  = aux_daemon_running("lingos_alertd");
    int ok_visiond = aux_daemon_running("lingos_visiond");
    int ok_voiced  = aux_daemon_running("lingos_voiced");
    int ok_tcp     = check_local_port(2937);
    int ok_http    = check_local_port(8080);
    int ok_audio   = check_local_port(8088);

    uart_puts("\n");
    uart_puts(COLOR_BOLD);
    uart_puts(tr("  ── LING OS readiness report ─────────────────────────\n",
                 "  ── LING OS 就绪报告 ─────────────────────────────────\n"));
    uart_puts(COLOR_RESET);
    rd_line(tr("lingosd   (8080/2939)", "lingosd   (8080/2939)"), ok_daemon);
    rd_line(tr("ai_server (ai.sock/8088)", "ai_server (ai.sock/8088)"), ok_ai);
    rd_line(tr("alertd    (lifeline)", "alertd    (生命线)"), ok_alertd);
    rd_line(tr("visiond   (vision)", "visiond   (视觉)"), ok_visiond);
    rd_line(tr("voiced    (voice)", "voiced    (语音)"), ok_voiced);
    {
        char lb[96];
        safe_snprintf(lb, sizeof(lb), "%s", tr("tcp 2937 (app channel)", "tcp 2937 (App 主通道)"));
        rd_line(lb, ok_tcp);
        safe_snprintf(lb, sizeof(lb), "%s", tr("http 8080 (web ui)", "http 8080 (Web UI)"));
        rd_line(lb, ok_http);
        safe_snprintf(lb, sizeof(lb), "%s", tr("audio 8088 (tts/stt)", "audio 8088 (语音 REST)"));
        rd_line(lb, ok_audio);
    }
    if (!ok_daemon || !ok_tcp) {
        uart_puts(COLOR_YELLOW);
        uart_puts(tr("  ⚠ core service degraded — see notes: bash lingos.sh doctor\n",
                     "  ⚠ 核心服务有降级——排查提示：bash lingos.sh doctor\n"));
        uart_puts(COLOR_RESET);
    }
    uart_puts("\n");

    /* 写 run/ready（单行 JSON——供脚本/supervisor/Web 读取） */
    {
        const char *root = lingos_data_root();
        char rpath[512];
        safe_snprintf(rpath, sizeof(rpath), "%s/run/ready", root);
        FILE *rf = fopen(rpath, "w");
        if (rf) {
            char tsbuf[64];
            time_t now = time(NULL);
            struct tm *tmv = localtime(&now);
            if (tmv) strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%S", tmv);
            else safe_strncpy(tsbuf, "?", sizeof(tsbuf));
            fprintf(rf, "{\"version\":\"%s\",\"ts\":%ld,\"time\":\"%s\","
                        "\"services\":{\"lingosd\":%d,\"ai\":%d,\"alertd\":%d,\"visiond\":%d,"
                        "\"voiced\":%d,\"tcp2937\":%d,\"http8080\":%d,\"audio8088\":%d}}\n",
                    version_get(), (long)now, tsbuf,
                    ok_daemon, ok_ai, ok_alertd, ok_visiond, ok_voiced,
                    ok_tcp, ok_http, ok_audio);
            fclose(rf);
            LOG_INFO_T("Main", "Readiness", "Report",
                       "daemon=%d ai=%d alertd=%d visiond=%d voiced=%d tcp=%d http=%d audio=%d",
                       ok_daemon, ok_ai, ok_alertd, ok_visiond, ok_voiced,
                       ok_tcp, ok_http, ok_audio);
        }
    }
}

/* ============================================================
 * 主函数
 * ============================================================ */
int main(int argc, char **argv) {
    /* 【2026-09-24】argv 框架（--fast / --safe / --diagnose / --version / --help）
     *   ——启动参数统一入口（server mode 的 --server 未来接入点） */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fast") == 0) {
            g_opt_fast = 1;
        } else if (strcmp(argv[i], "--safe") == 0) {
            g_opt_safe = 1;
        } else if (strcmp(argv[i], "--diagnose") == 0) {
            g_opt_diagnose = 1;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("LING OS %s\n", version_get());
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("LING OS %s\n", version_get());
            printf("用法: lingos_linux [选项]\n");
            printf("  --fast      快速启动（跳过非关键后台检查）\n");
            printf("  --safe      最小启动（不含 AI 与辅助守护）\n");
            printf("  --diagnose  诊断模式（打印环境与就绪报告后退出）\n");
            printf("  --version   显示版本\n");
            printf("  --help      显示帮助\n");
            return 0;
        }
    }

    show_startup_banner();

    /* 【2026-09-19】单实例锁（flock——防双实例端口冲突；日志实证的根因修复） */
    int instance_lock_fd = acquire_instance_lock();
    if (instance_lock_fd == -2) {
        /* 已有实例在运行：明确提示后退出（supervisor 见 75 码将停止重启循环） */
        return EXIT_ALREADY_RUNNING;
    }
    (void)instance_lock_fd;   /* 进程存活期间保持打开（锁随 fd 释放） */

    start_heartbeat_writer();

    exit_status_t exit_status;
    exit_status_init(&exit_status);

    if (exit_status_check_abnormal(&exit_status)) {
        int repair_ret = repair_mode_run();
        if (repair_ret != 0) {
            emergency_output(tr("Repair cancelled, exiting.\n", "修复已取消，退出。\n"));
            return 1;
        }
    }

    /* 【2026-09-19】写运行脏标记：崩溃/断电/强杀后由下次启动的修复模式检出
     *   （修复旧 BUG：异常标记被覆盖 → 修复模式永不触发） */
    exit_status_mark_running();

    LOG_INFO_T("Main", "Startup", "Entry", "LING OS Version: %s", version_get());

    crash_handler_init();

    signal(SIGINT, signal_exit_handler);
    signal(SIGTERM, signal_exit_handler);

    if (ensure_runtime_environment() != 0) {
        emergency_output(tr(
            "Failed to initialize runtime environment. Exiting.\n",
            "运行时环境初始化失败。退出。\n"
        ));
        return 1;
    }

    linux_io_init();
    // lang_init();   /* 移除：移到底部配置加载后 */
    log_system_init();

    int net_ok = network_check_online(3);
    if (net_ok != 0) {
        ui_show_network_error(tr("Network unavailable", "网络不可用"));
        LOG_WARN_T("Main", "Startup", "Network", "network unavailable, dependencies will be skipped");
    }

    // 检查是否首次启动
    // 【修复】统一使用 config_core_is_configured()（读 /LINGOS/system/config/state.json），
    // 此前误读 /LINGOS/Ensystem/state.json 导致已配置后仍显示 "first start"
    int is_first_start = !config_core_is_configured();

    if (is_first_start) {
        uart_puts(COLOR_BOLD COLOR_YELLOW);
        uart_puts(tr(
            "\n════════════════════════════════════════════════════════════\n"
            "  🎉  Welcome to LING OS!  \n"
            "  This is your first start.\n"
            "  ⚠  Configuration wizard will start automatically.\n"
            "════════════════════════════════════════════════════════════\n\n",
            "\n════════════════════════════════════════════════════════════\n"
            "  🎉  欢迎使用 LING OS！\n"
            "  这是您首次启动。\n"
            "  ⚠  配置向导将自动启动。\n"
            "════════════════════════════════════════════════════════════\n\n"
        ));
        uart_puts(COLOR_RESET);
        sleep(1);
    }

    if (system_install_check_and_run() < 0) {
        emergency_output(tr(
            "First installation setup failed. Exiting.\n",
            "首次安装设置失败。退出。\n"
        ));
        return 1;
    }

    linux_timer_init();
    tcp_client_init();

    audit_init();
    audit_log("system", "main", "startup", "{}", "LING OS started", 0, "info", 1);
    permission_init();
    fs_layout_init();

    check_disk_space_and_prompt();

    /* --- 自检系统（使用新的 check_manager） --- */
    check_manager_init();
    check_items_register_all();

    check_summary_t check_summary;   /* 定义变量 */
    int check_ret = check_manager_run_quick(&check_summary);

    if (check_ret != 0 && check_summary.failed > 0) {
        if (check_summary.need_configuration) {
            uart_puts(COLOR_BOLD COLOR_YELLOW);
            uart_puts(tr(
                "\n================================================================\n"
                "  ⚠  LING OS configuration is incomplete.\n"
                "  Configuration wizard will start automatically.\n"
                "================================================================\n\n",
                "\n================================================================\n"
                "  ⚠  LING OS 配置不完整。\n"
                "  配置向导将自动启动。\n"
                "================================================================\n\n"
            ));
            uart_puts(COLOR_RESET);
        } else {
            LOG_ERROR_T("Main", "SelfCheck", "Failed", "self check failed, entering error shell");
            error_shell_run();
            return 1;
        }
    }

    /* ============================================================
     * 配置文件存在检查与询问（#3）
     * ============================================================ */
    int use_existing = 0;
    wizard_config_t tmp_cfg;
    /* 【修复】已配置过（state.json system_configured=true）→ 静默使用，不再询问 */
    int already_configured = config_core_is_configured();
    /* FF[src/config/config_core.c]-CFN[config_core_load]-FTF[加载配置文件到临时结构] */
    if (config_core_load(&tmp_cfg) == 0) {
        /* 检查必要字段是否存在 */
        if (tmp_cfg.language[0] && tmp_cfg.ai_backend[0]) {
            if (already_configured) {
                /* 已配置：静默采用，不打扰用户 */
                use_existing = 1;
                wizard_config_t *global_cfg = config_core_get_mutable();
                *global_cfg = tmp_cfg;
                lang_reload();
                LOG_INFO_T("Main", "Config", "UsingExisting", "already configured, using existing silently");
            } else {
            uart_puts(COLOR_CYAN);
            uart_puts(tr("\nExisting configuration found:\n", "\n检测到已有配置：\n"));
            uart_puts(tr("  Backend: ", "  后端："));
            uart_puts(tmp_cfg.ai_backend);
            uart_puts("\n");
            uart_puts(tr("  Language: ", "  语言："));
            uart_puts(tmp_cfg.language);
            uart_puts("\n");
            if (strcmp(tmp_cfg.ai_backend, "deepseek") == 0) {
                uart_puts(tr("  Model: ", "  模型："));
                uart_puts(tmp_cfg.model);
                uart_puts("\n");
            }
            uart_puts(tr("\nUse this configuration? (Y/n): ", "\n使用此配置？(Y/n): "));
            char choice = uart_getc();
            uart_putc(choice);
            uart_puts("\n");
            if (choice == 'y' || choice == 'Y' || choice == '\n' || choice == '\r') {
                use_existing = 1;
                /* FF[src/config/config_core.c]-CFN[config_core_get_mutable]-FTF[获取全局配置指针] */
                wizard_config_t *global_cfg = config_core_get_mutable();
                *global_cfg = tmp_cfg;
                config_core_mark_configured();
                uart_puts(tr("✅ Using existing configuration.\n", "✅ 将使用已有配置。\n"));
                /* 重载语言 */
                lang_reload();
            } else {
                uart_puts(tr("Proceeding with new configuration.\n", "将进行全新配置。\n"));
            }
            }
        }
    }

    if (use_existing) {
        /* 跳过配置向导，直接进入后续启动 */
        goto after_wizard;
    }

    /* --- 首次启动或配置不完整时自动进入配置向导 --- */
    if (is_first_start || check_summary.need_configuration) {
        uart_puts(tr("\nStarting configuration wizard...\n", "\n启动配置向导...\n"));

        wizard_engine_ctx_t wiz_ctx;
        renderer_ctx_t renderer;

        /* 【0.4.3 修复】先生环境向导叠加：LINGOS_WIZARD=cli 强制文本向导；
         * 非 tty/无 TERM 时也自动用 CLI（notcurses 全屏 TUI 在 proot/远程终端半失败——
         * 不重绘导致方向键叠加）；真终端仍可用 TUI（LINGOS_WIZARD=tui 强制或默认 tty 走 TUI） */
        const char *wiz_mode = getenv("LINGOS_WIZARD");
        const char *term = getenv("TERM");
        int has_tty = (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)
                       && term && strstr(term, "xterm"));
        int want_tui = !(wiz_mode && strcmp(wiz_mode, "cli") == 0)
                       && !(wiz_mode && strcmp(wiz_mode, "raw") == 0)
                       && (has_tty || (wiz_mode && strcmp(wiz_mode, "tui") == 0));
        int use_cli_first = !want_tui;

        if (wizard_engine_init(&wiz_ctx, use_cli_first ? RENDERER_TYPE_CLI : RENDERER_TYPE_TUI) == 0) {
            if (wizard_engine_load_steps(&wiz_ctx) == 0) {
                if (!use_cli_first && renderer_tui_create(&renderer) == 0) {
                    wiz_ctx.renderer = &renderer;
                    if (wizard_engine_run(&wiz_ctx) == 0) {
                        wizard_engine_save_config(&wiz_ctx);
                        /* 配置保存后重新加载语言 */
                        lang_reload();
                        renderer.render_complete(&renderer, 1);
                    }
                    renderer_destroy(&renderer);
                } else {
                    LOG_WARN_T("Main", "Wizard", "RenderFail", "TUI renderer failed, trying CLI");
                    if (renderer_cli_create(&renderer) == 0) {
                        wiz_ctx.renderer = &renderer;
                        if (wizard_engine_run(&wiz_ctx) == 0) {
                            wizard_engine_save_config(&wiz_ctx);
                            lang_reload();
                            renderer.render_complete(&renderer, 1);
                        }
                        renderer_destroy(&renderer);
                    }
                }
                free(wiz_ctx.steps);
                free(wiz_ctx.stack);
            } else {
                uart_puts(tr("Failed to load configuration steps.\n", "加载配置步骤失败。\n"));
            }
        } else {
            uart_puts(tr("Failed to initialize configuration wizard.\n", "初始化配置向导失败。\n"));
        }
    }

after_wizard:
    /* 加载配置（供语言初始化使用） */
    /* 【2026-09-24 配置单载收口】无论首启与否统一刷新全局配置——
     *   原实现仅非首启加载：首启向导保存后全局配置依赖副作用同步（脆弱）。
     *   config_core_load 幂等（同文件重读），此处一次性收口（向导保存 → 全局一致）。 */
    config_core_load(config_core_get_mutable());
    /* 初始化语言（必须在 config_core_load 之后） */
    lang_init();

    /* --- 依赖安装（使用新的 install_manager） ---
     * 【0.6.0】全捆运行模式（start.sh 设 LINGOS_BUNDLED=1）：二进制与运行库
     * 全随包分发——不再检查/尝试安装开发包依赖。原逻辑每次启动检查
     * lib*-dev 包并尝试 apt 安装（0.5.0 起全捆已不需要——慢且必失败）。 */
    const char *lingos_bundled = getenv("LINGOS_BUNDLED");
    if (lingos_bundled && lingos_bundled[0] == '1') {
        LOG_INFO_T("Main", "Startup", "Install", "bundled runtime mode — system dependency check skipped");
    } else if (!install_manager_check_all()) {
        LOG_INFO_T("Main", "Startup", "Install", "installing dependencies");
        install_summary_t install_summary;
        int install_ret = install_manager_run_all(&install_summary);
        if (install_ret != 0) {
            LOG_WARN_T("Main", "Startup", "Install", "some dependencies failed");
        }
    } else {
        LOG_DEBUG_T("Main", "Startup", "Install", "all dependencies already installed");
    }

    if (security_config_load() != 0) {
        LOG_WARN_T("Main", "Startup", "SecurityConfig", "security_config_load failed, using defaults");
        security_config_set_defaults();
    }
    LOG_INFO_T("Main", "Startup", "Security", "Security config loaded");

    defense_mode_apply_current();

    if (ensure_daemon_running() != 0) {
        error_shell_run();
        return 1;
    }

    if (g_opt_safe) {
        /* 【2026-09-24】--safe 最小启动：仅核心（lingosd + Shell），不含 AI 与 aux 守护 */
        uart_puts(COLOR_YELLOW);
        uart_puts(tr("\n⚠ Safe mode (--safe): AI and auxiliary daemons are skipped.\n",
                     "\n⚠ 安全模式（--safe）：AI 与辅助守护已跳过。\n"));
        uart_puts(COLOR_RESET);
        LOG_WARN_T("Main", "Startup", "SafeMode", "safe mode: AI + aux daemons skipped");
    } else {
        /* 【2026-09-19 顺序修正】生命线先行：alertd 不依赖 AI——
         *   原顺序 ai_server → alertd：AI 最坏 ~40s 启动期间预警守护缺席，
         *   且 AI 失败曾连带生命线永不启动（违反"生命线不依赖 AI"定位） */
        ensure_aux_daemon("lingos_alertd");

        /* 【2026-09-19 竞态修正】等待 registry.sock（ai_server 用它加载技能注册表）——
         *   此前不等：首启/慢环境技能表回退内置，自定义技能缺席（日志实证 fallback） */
        wait_for_registry_ready(10);

        /* 【2026-09-19 软降级】AI 启动失败不再终止系统（AI watchdog 后台持续重试）——
         *   AI 不可用不影响生命线与核心服务（跛脚范式） */
        if (ensure_ai_server_running() != 0) {
            uart_puts(COLOR_YELLOW);
            uart_puts(tr(
                "\n⚠ AI service failed to start — system continues without AI for now.\n"
                "  AI watchdog will keep retrying in the background.\n",
                "\n⚠ AI 服务启动失败——系统先继续启动（AI 暂不可用）。\n"
                "  AI watchdog 将在后台持续重试。\n"));
            uart_puts(COLOR_RESET);
            LOG_WARN_T("Main", "Startup", "AISoftDegrade",
                       "AI server failed to start; continuing (soft degrade, watchdog will retry)");
        }

        /* 视觉/语音内核（软启动——缺失不阻塞主程序） */
        ensure_aux_daemon("lingos_visiond");
        ensure_aux_daemon("lingos_voiced");
    }

    connection_load_config(NULL);
    if (connection_server_start(NULL) != 0) {
        LOG_WARN_T("Main", "ConnectionServer", "StartFail", "connection server failed to start");
        /* 【2026-09-24 错误策略统一】关键通道失败不再静默——
         *   原仅 WARN 藏日志（用户遭遇"shell 正常但 App 连不上"的迷惑体验） */
        uart_puts(COLOR_RED);
        uart_puts(tr(
            "\n╔══════════════════════════════════════════════════════════╗\n"
            "║  ⚠ [CRITICAL] TCP 2937 failed to start!                   ║\n"
            "║  ⚠ [严重] TCP 2937 主通道启动失败——App 将无法连接！         ║\n"
            "║                                                          ║\n"
            "║  Possible cause: another instance is occupying the port. ║\n"
            "║  可能原因：另一实例占用端口。                              ║\n"
            "║  Try: bash lingos.sh doctor  /  bash lingos.sh restart   ║\n"
            "╚══════════════════════════════════════════════════════════╝\n",
            "\n╔══════════════════════════════════════════════════════════╗\n"
            "║  ⚠ [严重] TCP 2937 主通道启动失败——App 将无法连接！         ║\n"
            "║                                                          ║\n"
            "║  可能原因：另一实例占用端口。                              ║\n"
            "║  排查：bash lingos.sh doctor  或  bash lingos.sh restart  ║\n"
            "╚══════════════════════════════════════════════════════════╝\n"));
        uart_puts(COLOR_RESET);
    } else {
        LOG_INFO_T("Main", "ConnectionServer", "Started", "listening on ports %d/%d",
                   connection_get_config()->primary_port,
                   connection_get_config()->backup_port);
    }

    if (discovery_server_start() != 0) {
        LOG_WARN_T("Main", "DiscoveryServer", "StartFail", "UDP discovery server failed to start");
    }

    api_core_init(0);  /* R1: 主程序让位 lingosd */
    component_state_init();
    component_version_init();

    if (integrity_check_required()) {
        LOG_WARN_T("Main", "Integrity", "NotConfigured", "System not configured.");
    } else {
        integrity_check_all();
    }

    ai_config_load();
    nook_init();
    defense_init();
    ai_master_init();
    nook_repair_init();
    nook_idle_init();

    test_init();
    register_all_test_cases();
    syswatch_init(10);
    version_ensure();

    health_trend_init();
    health_watchdog_start();

    config_load_all();

    /* 【0.7.0 P2】开发调试日志判定（版本+内部变量——先生设定） */
    apply_dev_log_policy();

    start_background_initialization();
    if (g_opt_safe) {
        LOG_WARN_T("Main", "Startup", "SafeMode", "AI watchdog skipped (safe mode)");
    } else {
        start_ai_watchdog();
        /* 【2026-09-24】服务守护（监督树补全）：lingosd 5s 检查 + aux 30s 检查 */
        start_service_watchdog();
    }

    if (g_opt_fast) {
        LOG_INFO_T("Main", "Startup", "FastMode", "fast mode: async self-check skipped");
    } else if (async_self_check() != 0) {
        LOG_WARN_T("Main", "AsyncSelfCheck", "Failed", "background check could not be started");
    }

    /* 【2026-09-19】启动就绪报告 + run/ready 就绪文件（到达 Shell 前） */
    print_readiness_report();

    /* 【2026-09-24】--diagnose 诊断模式：打印环境信息后退出（不进 Shell） */
    if (g_opt_diagnose) {
        uart_puts(tr("\n--diagnose: environment summary --\n", "\n--diagnose：环境摘要 --\n"));
        uart_puts(tr("  version   : ", "  版本      : "));
        uart_puts(version_get());
        uart_puts("\n");
        uart_puts(tr("  data root : ", "  数据根    : "));
        uart_puts(lingos_data_root());
        uart_puts("\n");
        char logp[512];
        safe_snprintf(logp, sizeof(logp), "%s/log/lingos.log", lingos_data_root());
        uart_puts(tr("  log file  : ", "  日志文件  : "));
        uart_puts(logp);
        uart_puts("\n");
        uart_puts(tr("  flags     : ", "  旗标      : "));
        uart_puts(g_opt_fast ? "fast " : "");
        uart_puts(g_opt_safe ? "safe " : "");
        uart_puts("diagnose\n");
        uart_puts(tr("--diagnose done (shell not entered) --\n", "--diagnose 完成（未进入 Shell）--\n"));
        normal_exit(0);
    }

    startup_mode_t mode = startup_mode_get();
    LOG_INFO_T("Main", "Startup", "Mode", "startup mode: %s", startup_mode_name(mode));

    if (mode == STARTUP_MODE_TUI) {
        LOG_INFO_T("Main", "Startup", "Mode", "Starting TUI Desktop");
        uart_puts(tr("Starting TUI Desktop...\n", "正在启动 TUI 桌面...\n"));
        int tui_ret = tui_desktop_run();
        if (tui_ret == 0) {
            LOG_INFO_T("Main", "Exit", "TUI", "TUI Desktop exited normally");
        } else {
            LOG_WARN_T("Main", "Exit", "TUI", "TUI Desktop exited with error %d", tui_ret);
        }
        shell_run();
    } else {
        LOG_INFO_T("Main", "Init", "Shell", "Entering LING Shell");
        shell_run();
    }

    normal_exit(0);
    return 0;
}