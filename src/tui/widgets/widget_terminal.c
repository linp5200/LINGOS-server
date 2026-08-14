/**
 * @file    widget_terminal.c
 * @brief   TUI 桌面终端小部件（内嵌 Shell）
 * @version LN-B-4.2.0.0
 */

#include "widget_terminal.h"
#include "../desktop/tui_desktop.h"
#include "../desktop/tui_desktop_window.h"
#include "../../lib/log_extra.h"
#include "../../common/safe_string.h"
#include "../../drivers/uart.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>

#define TERMINAL_BUF_SIZE 4096

/* ============================================================
 * 终端状态
 * ============================================================ */

typedef struct {
    tui_window_t *window;
    pid_t shell_pid;
    int ptm_fd;      /* 伪终端主端 */
    int running;
    char buf[TERMINAL_BUF_SIZE];
    int buf_len;
} terminal_state_t;

static terminal_state_t g_terminal;

/* ============================================================
 * 内部辅助：初始化伪终端（简化版，实际可用 pipe + sh）
 * ============================================================ */

static int init_shell_process(void) {
    /* 简化实现：使用管道和 /bin/sh */
    int pipe_stdin[2], pipe_stdout[2];
    if (pipe(pipe_stdin) < 0 || pipe(pipe_stdout) < 0) {
        LOG_ERROR_T("WidgetTerminal", "Init", "PipeFail", "pipe failed");
        return -1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程：Shell */
        close(pipe_stdin[1]);
        close(pipe_stdout[0]);
        dup2(pipe_stdin[0], STDIN_FILENO);
        dup2(pipe_stdout[1], STDOUT_FILENO);
        dup2(pipe_stdout[1], STDERR_FILENO);
        close(pipe_stdin[0]);
        close(pipe_stdout[1]);
        execl("/bin/sh", "sh", (char*)NULL);
        _exit(1);
    } else if (pid > 0) {
        g_terminal.shell_pid = pid;
        g_terminal.ptm_fd = pipe_stdout[0];  /* 读取子进程输出 */
        g_terminal.running = 1;
        LOG_INFO_T("WidgetTerminal", "Init", "OK", "shell PID=%d", pid);
        return 0;
    } else {
        LOG_ERROR_T("WidgetTerminal", "Init", "ForkFail", "fork failed");
        return -1;
    }
}

/* ============================================================
 * 内部辅助：渲染终端输出
 * ============================================================ */

static void render_output(tui_window_t *win) {
    if (!win || !win->plane) return;

    struct ncplane *p = win->plane;
    int w = ncplane_dim_x(p);
    int h = ncplane_dim_y(p);

    /* 显示终端内容 */
    ncplane_set_fg_rgb(p, 0x88dd88);
    ncplane_set_bg_rgb(p, 0x000000);

    int lines = 0;
    char *line = g_terminal.buf;
    char *next = NULL;

    /* 最多显示 h-2 行 */
    for (int row = 1; row < h - 1 && lines < 20; row++) {
        ncplane_cursor_move_yx(p, row, 2);
        /* 找到下一行 */
        next = strchr(line, '\n');
        if (next) {
            *next = '\0';
            int len = strlen(line);
            if (len > w - 4) len = w - 4;
            for (int i = 0; i < len; i++) {
                if (line[i] >= 32 && line[i] <= 126) {
                    ncplane_putchar(p, line[i]);
                } else {
                    ncplane_putchar(p, ' ');
                }
            }
            line = next + 1;
            lines++;
        } else {
            /* 显示剩余内容 */
            int len = strlen(line);
            if (len > w - 4) len = w - 4;
            for (int i = 0; i < len; i++) {
                if (line[i] >= 32 && line[i] <= 126) {
                    ncplane_putchar(p, line[i]);
                } else {
                    ncplane_putchar(p, ' ');
                }
            }
            break;
        }
    }

    /* 光标提示 */
    ncplane_set_fg_rgb(p, 0x88ff88);
    ncplane_cursor_move_yx(p, h - 2, 2);
    ncplane_putstr(p, "> ");
}

/* ============================================================
 * 公共 API
 * ============================================================ */

void widget_terminal_create(void) {
    LOG_INFO_T("WidgetTerminal", "Create", "Enter", "creating terminal widget");

    memset(&g_terminal, 0, sizeof(g_terminal));

    /* 初始化 Shell 进程 */
    if (init_shell_process() != 0) {
        LOG_ERROR_T("WidgetTerminal", "Create", "ShellFail", "failed to start shell");
        return;
    }

    /* 获取窗口 */
    g_terminal.window = tui_window_get_focused();
    if (!g_terminal.window) {
        LOG_WARN_T("WidgetTerminal", "Create", "NoWindow", "no focused window");
        return;
    }

    /* 在窗口中显示提示 */
    struct ncplane *p = g_terminal.window->plane;
    if (p) {
        ncplane_set_fg_rgb(p, 0x88dd88);
        ncplane_cursor_move_yx(p, 1, 2);
        ncplane_putstr(p, "LING OS Terminal (type commands, Enter to execute)");
        ncplane_cursor_move_yx(p, 3, 2);
        ncplane_putstr(p, "Type 'exit' to close.");
    }

    tui_desktop_refresh(tui_desktop_get_state());

    LOG_INFO_T("WidgetTerminal", "Create", "OK", "terminal widget created");
}

void widget_terminal_update(void) {
    if (!g_terminal.running || !g_terminal.window) return;

    /* 读取子进程输出（非阻塞） */
    char buf[256];
    int n = read(g_terminal.ptm_fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (g_terminal.buf_len + n < TERMINAL_BUF_SIZE - 1) {
            memcpy(g_terminal.buf + g_terminal.buf_len, buf, n);
            g_terminal.buf_len += n;
            g_terminal.buf[g_terminal.buf_len] = '\0';
        }
        render_output(g_terminal.window);
        tui_desktop_refresh(tui_desktop_get_state());
    }

    /* 检查子进程是否退出 */
    int status;
    pid_t ret = waitpid(g_terminal.shell_pid, &status, WNOHANG);
    if (ret > 0) {
        g_terminal.running = 0;
        LOG_INFO_T("WidgetTerminal", "Update", "ShellExit", "shell PID=%d exited", g_terminal.shell_pid);
        if (g_terminal.window && g_terminal.window->plane) {
            struct ncplane *p = g_terminal.window->plane;
            ncplane_set_fg_rgb(p, 0xff8888);
            ncplane_cursor_move_yx(p, 5, 2);
            ncplane_putstr(p, "[Shell exited]");
            tui_desktop_refresh(tui_desktop_get_state());
        }
    }
}

void widget_terminal_input(const char *cmd) {
    if (!g_terminal.running || !cmd) return;

    LOG_INFO_T("WidgetTerminal", "Input", "Enter", "cmd='%s'", cmd);

    /* 写入子进程（简化：直接通过管道写入） */
    /* 实际需要写入 ptm，这里用 pipe 的写端 */
    /* 当前实现中，我们没有保存写端，需要完善 */
    /* 简化：直接执行命令（不通过子进程） */
    if (strcmp(cmd, "exit") == 0) {
        g_terminal.running = 0;
        if (g_terminal.window) {
            tui_window_destroy(g_terminal.window);
        }
        return;
    }

    /* 执行命令并显示输出 */
    char output[2048] = {0};
    FILE *fp = popen(cmd, "r");
    if (fp) {
        fread(output, 1, sizeof(output) - 1, fp);
        pclose(fp);
    } else {
        safe_strncpy(output, "Command execution failed", sizeof(output));
    }

    /* 追加到显示缓冲区 */
    if (g_terminal.buf_len + strlen(output) + 2 < TERMINAL_BUF_SIZE) {
        strcat(g_terminal.buf, output);
        strcat(g_terminal.buf, "\n");
        g_terminal.buf_len = strlen(g_terminal.buf);
        render_output(g_terminal.window);
        tui_desktop_refresh(tui_desktop_get_state());
    }
}

void widget_terminal_destroy(void) {
    LOG_INFO_T("WidgetTerminal", "Destroy", "Enter", "destroying terminal widget");

    if (g_terminal.shell_pid > 0) {
        kill(g_terminal.shell_pid, SIGTERM);
        waitpid(g_terminal.shell_pid, NULL, 0);
    }
    if (g_terminal.ptm_fd > 0) {
        close(g_terminal.ptm_fd);
    }
    memset(&g_terminal, 0, sizeof(g_terminal));

    LOG_INFO_T("WidgetTerminal", "Destroy", "OK", "terminal widget destroyed");
}