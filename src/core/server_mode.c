/**
 * @file    src/core/server_mode.c
 * @brief   server mode 实现（P2——先生设定 2026-09-19）
 * @version LN-0.7.0
 *
 * 存储：/LINGOS/system/config/server_mode.json
 *   {"enabled":1,"package_mode":0,"since":1758...}
 * 危机联动：/LINGOS/system/config/crisis_state.json（crisis.py 维护）
 *   {"active":true,...} → 严禁退出
 *
 * 输入门控（先生设定"控制键唯一"）：
 *   · 只接受 Ctrl-Q(0x11) 与 q/Q —— 停掉服务器（危机时拒绝）
 *   · 其余一切按键忽略
 *   · 终端进 raw-ish 模式（关 ICANON/ECHO/IXON——否则 Ctrl-Q 会被流控吞掉）
 */

#include "server_mode.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ============================================================
 * 状态
 * ============================================================ */
static int g_enabled = 0;
static int g_package_mode = 0;
static int g_inited = 0;

static const char *state_path(void) {
    static char p[512];
    if (p[0] == '\0') {
        safe_snprintf(p, sizeof(p), "%s/system/config/server_mode.json", lingos_data_root());
    }
    return p;
}

static const char *crisis_path(void) {
    static char p[512];
    if (p[0] == '\0') {
        safe_snprintf(p, sizeof(p), "%s/system/config/crisis_state.json", lingos_data_root());
    }
    return p;
}

/* ============================================================
 * 状态文件读写（手写小 JSON——避免依赖）
 * ============================================================ */
static int json_bool_field(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    if (!p) return 0;
    const char *colon = strchr(p, ':');
    if (!colon) return 0;
    colon++;
    while (*colon == ' ' || *colon == '\t') colon++;
    if (*colon == 't' || *colon == 'T' || *colon == '1') return 1;
    if (*colon == 'f' || *colon == 'F' || *colon == '0') return 0;
    return atoi(colon) ? 1 : 0;
}

static void state_load(void) {
    g_enabled = 0;
    g_package_mode = 0;
    FILE *fp = fopen(state_path(), "r");
    if (!fp) return;
    char buf[1024];
    size_t r = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[r] = '\0';
    g_enabled = json_bool_field(buf, "\"enabled\"");
    g_package_mode = json_bool_field(buf, "\"package_mode\"");
}

static void state_save(void) {
    /* 确保目录 */
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/system/config", lingos_data_root());
    mkdir(dir, 0755);

    FILE *fp = fopen(state_path(), "w");
    if (!fp) {
        LOG_ERROR_T("ServerMode", "Save", "OpenFail", "cannot write %s", state_path());
        return;
    }
    fprintf(fp, "{\"enabled\":%d,\"package_mode\":%d,\"since\":%ld}\n",
            g_enabled, g_package_mode, (long)time(NULL));
    fclose(fp);
}

/* ============================================================
 * 公共 API
 * ============================================================ */
int server_mode_init(void) {
    if (g_inited) return 0;
    g_inited = 1;
    state_load();
    LOG_INFO_T("ServerMode", "Init", "OK", "enabled=%d package=%d", g_enabled, g_package_mode);
    return 0;
}

int server_mode_is_active(void) {
    if (!g_inited) server_mode_init();
    return g_enabled;
}

int server_mode_is_package(void) {
    if (!g_inited) server_mode_init();
    return g_package_mode;
}

int server_mode_set_enabled(int enabled) {
    if (!g_inited) server_mode_init();
    g_enabled = enabled ? 1 : 0;
    state_save();
    LOG_WARN_T("ServerMode", "SetEnabled", enabled ? "ON" : "OFF", "persisted to %s", state_path());
    return 0;
}

int server_mode_crisis_active(void) {
    FILE *fp = fopen(crisis_path(), "r");
    if (!fp) return 0;
    char buf[2048];
    size_t r = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[r] = '\0';
    return json_bool_field(buf, "\"active\"");
}

int server_mode_request_stop(const char *source) {
    /* 危机铁律：一切行为为人身安全让路——严禁退出 */
    if (server_mode_crisis_active()) {
        uart_puts(tr("\n[server mode] 危机进行中——退出已禁用（人身安全让路）\n",
                     "\n[server mode] 危机进行中——退出已禁用（人身安全让路）\n"));
        LOG_WARN_T("ServerMode", "Stop", "CrisisBlocked", "stop refused during crisis (source=%s)",
                   source ? source : "-");
        return -1;
    }
    LOG_WARN_T("ServerMode", "Stop", "Graceful", "graceful shutdown requested (source=%s)",
               source ? source : "-");
    uart_puts(tr("[server mode] 停止服务器（优雅退出）...\n",
                 "[server mode] 停止服务器（优雅退出）...\n"));
    fflush(stdout);
    /* 复用主程序信号链：SIGTERM → normal_exit → 收尾子守护 + 通知 supervisor */
    raise(SIGTERM);
    return 0;
}

/* ============================================================
 * 主循环：日志尾随显示 + 输入门控
 * ============================================================ */

/* 【0.7.0-hf】终端状态恢复：Ctrl-Q → raise(SIGTERM) → exit() 时
 *   不会走函数尾部还原——用 atexit 兜底（否则退出后终端残留 raw 模式：
 *   无回显/无行编辑——用户需手动 `reset` 才能恢复）。 */
static struct termios g_saved_term;
static int g_tty_saved = 0;
static void server_mode_restore_term(void) {
    if (g_tty_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_term);
        g_tty_saved = 0;
    }
}

static void tail_open(FILE **fp, long *pos) {
    char logpath[512];
    safe_snprintf(logpath, sizeof(logpath), "%s/log/lingos.log", lingos_data_root());
    *fp = fopen(logpath, "r");
    *pos = 0;
    if (!*fp) return;

    /* 先显示最后 ~8KB（滚动到尾部） */
    fseek(*fp, 0, SEEK_END);
    long sz = ftell(*fp);
    long start = (sz > 8192) ? (sz - 8192) : 0;
    fseek(*fp, start, SEEK_SET);
    /* 丢弃半行 */
    if (start > 0) {
        int ch;
        while ((ch = fgetc(*fp)) != EOF && ch != '\n') { }
    }
    char buf[1024];
    while (fgets(buf, sizeof(buf), *fp)) {
        uart_puts(buf);
    }
    *pos = ftell(*fp);
}

/* 【0.7.0 P2-B】api.log 尾随（API 日志——server mode 专属查看渠道） */
static void tail_open_named(const char *name, FILE **fp, long *pos) {
    char logpath[512];
    safe_snprintf(logpath, sizeof(logpath), "%s/log/%s", lingos_data_root(), name);
    *fp = fopen(logpath, "r");
    *pos = 0;
    if (!*fp) return;
    fseek(*fp, 0, SEEK_END);
    long sz = ftell(*fp);
    long start = (sz > 4096) ? (sz - 4096) : 0;
    fseek(*fp, start, SEEK_SET);
    if (start > 0) {
        int ch;
        while ((ch = fgetc(*fp)) != EOF && ch != '\n') { }
    }
    char buf[1024];
    while (fgets(buf, sizeof(buf), *fp)) {
        uart_puts(buf);
    }
    *pos = ftell(*fp);
}

static void tail_pump_named(FILE *fp, long *pos) {
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < *pos) *pos = 0;
    if (fseek(fp, *pos, SEEK_SET) != 0) return;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) {
        uart_puts(buf);
    }
    long np = ftell(fp);
    if (np >= 0) *pos = np;
    fflush(stdout);
}

static void tail_pump(FILE *fp, long *pos) {
    if (!fp) return;
    /* 文件被轮转/清空检测 */
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < *pos) *pos = 0;

    if (fseek(fp, *pos, SEEK_SET) != 0) return;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) {
        uart_puts(buf);
    }
    long np = ftell(fp);
    if (np >= 0) *pos = np;
    fflush(stdout);
}

void server_mode_run(void) {
    /* 【0.7.0-hf】终端能力探测：
     *   · stdin 非 tty（后台/nohup/dev/null）→ 不轮询按键（防 /dev/null 空转 100% CPU）
     *   · stdout 非 tty（输出进文件）→ 不尾随显示（日志已在文件里；防重复写入） */
    int stdin_tty  = isatty(STDIN_FILENO);
    int stdout_tty = isatty(STDOUT_FILENO);

    /* ---- 头部（有终端才显示） ---- */
    if (stdout_tty) {
        uart_puts("\033[2J\033[H");
        uart_puts("\033[1;36m");
        uart_puts("┌────────────────────────────────────────────────────────────┐\n");
        uart_puts("│  LING OS · SERVER MODE                                     │\n");
        uart_puts("│  只显示日志 · 控制键：Ctrl-Q / Q（停止服务器）              │\n");
        uart_puts("└────────────────────────────────────────────────────────────┘\n");
        uart_puts("\033[0m\n");
        fflush(stdout);
    }

    LOG_INFO_T("ServerMode", "Run", "Enter",
               "server mode session started (stdin_tty=%d, stdout_tty=%d)",
               stdin_tty, stdout_tty);

    /* 【0.7.2】服务器模式：自动切换日志等级 → INFO（先生定稿）
     *   服务器模式长期显示日志——INFO 显示重要节点，避免 DEBUG 细节刷屏。
     *   需要 DEBUG 调试：终端敲 'log level debug' 或设 LINGOS_LOG_LEVEL=debug。 */
    {
        static int s_prev_level = -1;
        if (s_prev_level < 0) {
            s_prev_level = log_get_global_level();
        }
        log_set_global_level(LOG_LEVEL_INFO);
        LOG_INFO_T("ServerMode", "LogLevel", "Auto",
                   "log level auto-switched to INFO (was %d) — set 'log level debug' for verbose",
                   s_prev_level);
    }

    /* ---- 终端：raw-ish（捕获 Ctrl-Q，逐字符；关闭 ISIG——控制键唯一，危机不可被 Ctrl-C 绕过） ---- */
    struct termios oldt, newt;
    int tty_ok = (stdin_tty && tcgetattr(STDIN_FILENO, &oldt) == 0);
    if (tty_ok) {
        g_saved_term = oldt;
        g_tty_saved = 1;
        atexit(server_mode_restore_term);   /* 兜底：信号退出路径也恢复 */
        newt = oldt;
        newt.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
        newt.c_iflag &= ~(tcflag_t)(IXON | IXOFF);
        newt.c_cc[VMIN] = 0;
        newt.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    }

    /* ---- 日志尾随初始（仅终端显示模式） ---- */
    FILE *fp = NULL;
    long pos = 0;
    FILE *afp = NULL;
    long apos = 0;
    if (stdout_tty) {
        tail_open(&fp, &pos);
        tail_open_named("api.log", &afp, &apos);
    }

    /* ---- 循环 ---- */
    while (server_mode_is_active()) {
        if (stdin_tty) {
            struct pollfd pfd;
            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;
            pfd.revents = 0;

            int pr = poll(&pfd, 1, 500);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                char c = 0;
                ssize_t rn = read(STDIN_FILENO, &c, 1);
                if (rn == 1) {
                    if (c == 0x11 || c == 'q' || c == 'Q') {
                        /* 控制键：停止服务器（危机时拒绝） */
                        if (server_mode_request_stop("ctrl-q/local") == 0) {
                            break;   /* 已被 raise(SIGTERM)——若信号未即时退出则循环结束 */
                        }
                        /* 危机被拒 → 继续显示日志 */
                    }
                    /* 其余按键：忽略（先生设定：控制键唯一） */
                } else if (rn == 0) {
                    usleep(300000);   /* 终端半闭（EOF 就绪）——防空转 */
                }
            }
        } else {
            /* 后台/无终端：无按键通道——1s 节拍（防 /dev/null 空转） */
            sleep(1);
        }

        /* 外部停止请求（客户端 server mode stop → 标志文件） */
        {
            char req[512];
            safe_snprintf(req, sizeof(req), "%s/run/server_mode_stop_request", lingos_data_root());
            if (access(req, F_OK) == 0) {
                unlink(req);
                if (server_mode_request_stop("client") == 0) break;
            }
        }

        if (stdout_tty) {
            tail_pump(fp, &pos);
            tail_pump_named(afp, &apos);
        }
    }

    if (fp) fclose(fp);
    if (afp) fclose(afp);
    if (g_tty_saved) server_mode_restore_term();   /* 正常路径直接恢复（atexit 兜底二次调用安全） */
    LOG_INFO_T("ServerMode", "Run", "Exit", "server mode session ended");
}

/* ============================================================
 * shell 命令：server mode [on|off|stop|status]
 * ============================================================ */
int server_mode_command(const char *args) {
    if (!g_inited) server_mode_init();

    /* 【0.7.1-hf3】每次命令前重读状态文件——App/Web 的 server_mode_on/off 直接改文件，
     *   C 进程内存须对齐（先生真机 2026-09-25：App 已 on，终端敲 on 仍走"首次开启"路径）。 */
    state_load();

    if (!args || !*args || strcmp(args, "status") == 0) {
        char line[256];
        safe_snprintf(line, sizeof(line),
            "server mode: %s%s\n",
            g_enabled ? tr("ON (log-only, Ctrl-Q to stop)", "开启（只显示日志，Ctrl-Q 停止）")
                      : tr("OFF", "关闭"),
            g_package_mode ? tr(" [package]", " [服务器模式包]") : "");
        uart_puts(line);
        return 1;
    }

    if (strcmp(args, "on") == 0) {
        if (g_enabled) {
            uart_puts(tr("server mode is already ON — re-entering log-only session\n",
                         "server mode 已处于开启状态——重新进入只显示日志会话\n"));
        } else {
            server_mode_set_enabled(1);
            uart_puts(tr("server mode ON — entering log-only mode (Ctrl-Q / Q to stop)\n",
                         "server mode 已开启——进入只显示日志模式（Ctrl-Q / Q 停止）\n"));
        }
        /* 进入 server mode 会话（无论如何——保证可进入） */
        server_mode_run();
        return 1;
    }

    if (strcmp(args, "off") == 0) {
        /* 【服务器模式包】常规关闭无效 */
        if (g_package_mode) {
            uart_puts(tr("[server mode] 服务器模式包：常规关闭无效（停止请用 server mode stop）\n",
                         "[server mode] 服务器模式包：常规关闭无效（停止请用 server mode stop）\n"));
            return 1;
        }
        server_mode_set_enabled(0);
        uart_puts(tr("server mode OFF\n", "server mode 已关闭（下次启动回普通模式）\n"));
        return 1;
    }

    if (strcmp(args, "stop") == 0) {
        server_mode_request_stop("shell");
        return 1;
    }

    uart_puts(tr("Usage: server mode [on|off|stop|status]\n",
                 "用法：server mode [on|off|stop|status]\n"));
    return 1;
}
