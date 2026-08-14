/**
 * @file    src/core/main.c
 * @brief   LING OS 主入口
 * @version LN-B-5.1.2.6-rc
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
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>

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

static void* heartbeat_write_thread(void *arg) {
    (void)arg;
    const char *root = lingos_data_root();
    char heartbeat_path[512];
    safe_snprintf(heartbeat_path, sizeof(heartbeat_path), "%s/run/heartbeat", root);

    char run_dir[512];
    safe_snprintf(run_dir, sizeof(run_dir), "%s/run", root);
    mkdir(run_dir, 0755);

    while (!g_heartbeat_stop) {
        time_t now = time(NULL);
        FILE *fp = fopen(heartbeat_path, "w");
        if (fp) {
            fprintf(fp, "%ld\n", now);
            fclose(fp);
        }
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

int ensure_daemon_running(void) {
    LOG_INFO_T("Main", "EnsureDaemon", "Enter", "starting lingosd");
    const char *daemon_path = "./lingosd";
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
            char cmd[1024];
            safe_snprintf(cmd, sizeof(cmd), "cp src/python/ai_server.py '%s' && chmod +x '%s'", script_path, script_path);
            system(cmd);
        } else {
            LOG_ERROR_T("Main", "EnsureAI", "NoScript", "ai_server.py not found");
            return -1;
        }
    }

    cleanup_stale_processes(pid_path, socket_path);

    for (int attempt = 1; attempt <= max_retries; attempt++) {
        if (is_service_healthy(socket_path)) {
            LOG_INFO_T("Main", "EnsureAI", "AlreadyHealthy", "AI server is healthy");
            return 0;
        }

        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            execlp("python3", "python3", "-u", script_path, (char*)NULL);
            perror("execlp python3");
            _exit(1);
        } else if (pid > 0) {
            FILE *fp = fopen(pid_path, "w");
            if (fp) {
                fprintf(fp, "%d\n", pid);
                fclose(fp);
            }
            int wait_time = 3 << (attempt - 1);
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
 * 正常退出函数
 * ============================================================ */
static void normal_exit(int exit_code) {
    LOG_INFO_T("Main", "Exit", "Normal", "Exiting with code %d", exit_code);

    exit_status_mark_clean(exit_code, "Normal exit");
    stop_ai_watchdog();
    stop_background_initialization();
    stop_heartbeat_writer();
    send_exit_signal_to_supervisor();

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

/* ============================================================
 * 信号处理
 * ============================================================ */
static void signal_exit_handler(int sig) {
    LOG_WARN_T("Main", "Signal", "Received", "signal=%d, exiting gracefully", sig);
    exit_status_mark_abnormal(sig, "Signal received");
    normal_exit(128 + sig);
}

static void emergency_output(const char *msg) {
    if (!msg) return;
    write(STDERR_FILENO, msg, strlen(msg));
}

/* ============================================================
 * 主函数
 * ============================================================ */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    show_startup_banner();
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

    if (system_install_check_and_run() != 0) {
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

        if (wizard_engine_init(&wiz_ctx, RENDERER_TYPE_TUI) == 0) {
            if (wizard_engine_load_steps(&wiz_ctx) == 0) {
                if (renderer_tui_create(&renderer) == 0) {
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
    if (!is_first_start) {
        config_core_load(config_core_get_mutable());
    }
    /* 初始化语言（必须在 config_core_load 之后） */
    lang_init();

    /* --- 依赖安装（使用新的 install_manager） --- */
    if (!install_manager_check_all()) {
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

    if (ensure_ai_server_running() != 0) {
        error_shell_run();
        return 1;
    }

    connection_load_config(NULL);
    if (connection_server_start(NULL) != 0) {
        LOG_WARN_T("Main", "ConnectionServer", "StartFail", "connection server failed to start");
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

    start_background_initialization();
    start_ai_watchdog();

    if (async_self_check() != 0) {
        LOG_WARN_T("Main", "AsyncSelfCheck", "Failed", "background check could not be started");
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