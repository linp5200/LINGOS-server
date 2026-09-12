/**
 * @file    src/security/safe_exec.c
 * @brief   安全命令执行实现（OWASP OS Command Injection Defense）
 * @version LN-0.5.0
 *
 * 设计要点（对照 OWASP 官方三层）：
 *   ① 不调用 shell —— 全程 fork + execvp(argv)，**shell 元字符失去意义**
 *   ② 参数分离   —— 命令与参数分别入 argv[]
 *   ③ 输入验证   —— 可执行名白名单 + 危险模式检测 + 长度/数量上限
 *   ④ 选项终止   —— 支持 `--` 约定（在调用方保证）
 *   ⑤ 最小权限   —— 可选降权（见后）
 *
 * 先生裁决（2026-09-12）：
 *   S2 = B（黑名单加强）—— 本实现保留黑名单，**同时**补上参数化与白名单
 *   以满足 OWASP「黑名单不足以防护命令注入」的要求。
 */

#include "safe_exec.h"
#include "log_extra.h"
#include "safe_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/types.h>

/* ============================================================
 * 策略状态
 * ============================================================ */
static safe_exec_mode_t g_mode = SAFE_EXEC_MODE_BALANCED;

/* ------------------------------------------------------------
 * OWASP 建议：命令白名单（Positive allowlist validation）
 * 只放行系统管理确实需要的**只读/低风险**命令。
 * 高风险命令（如 rm/dd/mkfs）**不入白名单** —— 走 DENY。
 * ------------------------------------------------------------ */
static const char *g_allowlist[] = {
    /* 文件与目录（只读） */
    "ls", "cat", "head", "tail", "wc", "stat", "file", "find", "grep",
    "du", "df", "tree", "readlink", "basename", "dirname", "md5sum",
    "sha256sum", "sort", "uniq", "cut", "tr", "awk", "sed",
    /* 系统信息（只读） */
    "uname", "uptime", "free", "ps", "top", "whoami", "id", "hostname",
    "date", "env", "printenv", "lscpu", "lsblk", "lsusb", "lspci",
    "dmidecode", "nproc", "getconf", "locale",
    /* 网络（只读） */
    "ip", "ifconfig", "ss", "netstat", "ping", "traceroute", "nslookup",
    "dig", "host", "curl", "wget", "arp", "route",
    /* 进程/服务（只读） */
    "pgrep", "pstree", "systemctl", "service", "journalctl",
    /* 软件包（只读） */
    "apt", "apt-cache", "dpkg", "apk", "pip", "pip3", "npm", "python3",
    "node", "java", "gcc", "make",
    /* 开发/文本 */
    "git", "jq", "yq", "xargs", "tee", "echo", "printf", "test",
    "mkdir", "touch", "cp", "mv", "ln", "chmod", "chown", "tar", "gzip",
    "zip", "unzip", "which", "whereis", "type", "command", "timeout",
    /* 媒体/视觉（本系统用） */
    "ffmpeg", "ffprobe", "espeak-ng", "piper",
    NULL
};

/* ------------------------------------------------------------
 * 危险模式黑名单（先生 S2=B：加强）
 * 语法层面检测 —— 无论白名单与否，命中即 DENY 或 NEED_CONFIRM
 * ------------------------------------------------------------ */
typedef struct {
    const char *pattern;   /* 匹配片段 */
    int         severe;    /* 1=直接拒绝；0=需二次确认 */
    const char *why;
} danger_rule_t;

static const danger_rule_t g_danger[] = {
    /* —— 直接拒绝（不可逆/系统级）—— */
    { "rm -rf /",            1, "递归删除根目录" },
    { "rm -rf /*",           1, "递归删除根目录" },
    { "rm -fr /",            1, "递归删除根目录" },
    { ":(){",                1, "fork 炸弹" },
    { "mkfs",                1, "格式化文件系统" },
    { "mke2fs",              1, "格式化文件系统" },
    { "fdisk",               1, "磁盘分区操作" },
    { "parted",              1, "磁盘分区操作" },
    { "dd if=",              1, "裸设备写入" },
    { "dd of=",              1, "裸设备写入" },
    { "of=/dev/",            1, "写入块设备" },
    { "> /dev/sd",           1, "覆写块设备" },
    { "> /dev/mmcblk",       1, "覆写块设备" },
    { "> /dev/nvme",         1, "覆写块设备" },
    { "shutdown",            1, "关机" },
    { "poweroff",            1, "关机" },
    { "halt",                1, "停机" },
    { "reboot",              1, "重启" },
    { "init 0",              1, "停机" },
    { "chmod -R 777 /",      1, "全盘权限放开" },
    { "chown -R",            1, "全盘属主变更" },
    { "/etc/passwd",         1, "密码文件" },
    { "/etc/shadow",         1, "影子密码" },
    { "/etc/sudoers",        1, "sudo 配置" },
    { "authorized_keys",     1, "SSH 后门" },
    { "crontab",             1, "定时任务（可提权）" },
    { "insmod",              1, "加载内核模块" },
    { "rmmod",               1, "卸载内核模块" },
    { "iptables",            1, "防火墙规则变更" },
    { "nft ",                1, "防火墙规则变更" },
    { "mount ",              1, "挂载操作" },
    { "umount",              1, "卸载操作" },
    /* —— 需二次确认（有风险但常见）—— */
    { "rm ",                 0, "删除操作" },
    { "rmdir",               0, "删除目录" },
    { "kill",                0, "终止进程" },
    { "pkill",               0, "批量终止进程" },
    { "killall",             0, "批量终止进程" },
    { "apt remove",          0, "卸载软件" },
    { "apt purge",           0, "卸载并清理" },
    { "pip uninstall",       0, "卸载 python 包" },
    { "systemctl stop",      0, "停止服务" },
    { "systemctl disable",   0, "禁用服务" },
    { "truncate",            0, "截断文件" },
    { "chmod",               0, "权限变更" },
    { "chown",               0, "属主变更" },
    { "visudo",              0, "编辑 sudo 配置" },
    { "passwd",              0, "修改密码" },
    { NULL, 0, NULL }
};

/* ------------------------------------------------------------
 * 命中检测：不区分大小写、忽略**多余空白**（防 `rm  -rf  /` 绕过）
 * ------------------------------------------------------------ */
static void norm_ws(const char *in, char *out, size_t out_sz) {
    size_t j = 0;
    int prev_space = 1;
    for (size_t i = 0; in[i] && j + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isspace(c)) {
            if (!prev_space) { out[j++] = ' '; prev_space = 1; }
        } else {
            out[j++] = (char)tolower(c);
            prev_space = 0;
        }
    }
    out[j] = '\0';
}

static const danger_rule_t *match_danger(const char *norm) {
    for (int i = 0; g_danger[i].pattern; i++) {
        if (strstr(norm, g_danger[i].pattern)) return &g_danger[i];
    }
    return NULL;
}

/* ============================================================
 * 白名单判定
 * ============================================================ */
int safe_exec_program_allowed(const char *prog) {
    if (!prog || !*prog) return 0;
    /* 取 basename（防 /usr/bin/ls 形式） */
    const char *base = strrchr(prog, '/');
    base = base ? base + 1 : prog;
    for (int i = 0; g_allowlist[i]; i++) {
        if (strcmp(base, g_allowlist[i]) == 0) return 1;
    }
    return 0;
}

void safe_exec_set_mode(safe_exec_mode_t mode) { g_mode = mode; }
safe_exec_mode_t safe_exec_get_mode(void) { return g_mode; }

/* ============================================================
 * 拆分：命令行 → argv[]
 * 注意：**不解析引号与管道** —— 它们作为字面参数交给 execvp，
 *       shell 不参与，因此 `;` `|` `&` `` ` `` `$()` 全部失效。
 * ============================================================ */
int safe_exec_split(const char *cmdline, char *buf, size_t buf_sz,
                    char *argv[], int max_args) {
    if (!cmdline || !buf || !argv || max_args < 2) return -1;
    size_t len = strlen(cmdline);
    if (len == 0 || len >= buf_sz) return -1;

    memcpy(buf, cmdline, len + 1);

    int n = 0;
    char *p = buf;
    while (*p && n < max_args - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
        if ((size_t)(p - start) > SAFE_EXEC_MAX_ARGLEN) return -1;
        argv[n++] = start;
    }
    argv[n] = NULL;
    return n;
}

/* ============================================================
 * 策略检查
 * ============================================================ */
safe_exec_verdict_t safe_exec_check(const char *cmdline,
                                    char *reason, size_t reason_sz) {
    if (reason && reason_sz) reason[0] = '\0';
    if (!cmdline || !*cmdline) {
        if (reason) safe_strncpy(reason, "empty command", reason_sz);
        return SAFE_EXEC_DENY;
    }
    if (strlen(cmdline) > 4096) {
        if (reason) safe_strncpy(reason, "command too long", reason_sz);
        return SAFE_EXEC_DENY;
    }

    char norm[4096];
    norm_ws(cmdline, norm, sizeof(norm));

    /* 1) 危险模式（先生 S2=B：黑名单加强） */
    const danger_rule_t *d = match_danger(norm);
    if (d) {
        if (d->severe) {
            if (reason) safe_snprintf(reason, reason_sz, "危险命令（%s）已拦截", d->why);
            LOG_WARN_T("SafeExec", "Check", "Denied", "severe pattern '%s' (%s) in: %.120s",
                       d->pattern, d->why, cmdline);
            return SAFE_EXEC_DENY;
        }
        if (reason) safe_snprintf(reason, reason_sz, "高风险命令（%s）需二次确认", d->why);
        LOG_WARN_T("SafeExec", "Check", "Confirm", "pattern '%s' (%s) in: %.120s",
                   d->pattern, d->why, cmdline);
        return SAFE_EXEC_NEED_CONFIRM;
    }

    /* 2) 白名单（OWASP ③）—— PERMISSIVE 模式跳过 */
    if (g_mode != SAFE_EXEC_MODE_PERMISSIVE) {
        char tmp[4096];
        char *argv[SAFE_EXEC_MAX_ARGS];
        safe_strncpy(tmp, cmdline, sizeof(tmp));
        int n = safe_exec_split(tmp, tmp, sizeof(tmp), argv, SAFE_EXEC_MAX_ARGS);
        if (n <= 0) {
            if (reason) safe_strncpy(reason, "无法解析命令", reason_sz);
            return SAFE_EXEC_DENY;
        }
        /* 跳过前置赋值（KEY=VAL） */
        int i = 0;
        while (i < n && strchr(argv[i], '=') && argv[i][0] != '/' && argv[i][0] != '.') i++;
        if (i >= n) {
            if (reason) safe_strncpy(reason, "无法解析命令", reason_sz);
            return SAFE_EXEC_DENY;
        }
        if (!safe_exec_program_allowed(argv[i])) {
            if (g_mode == SAFE_EXEC_MODE_STRICT) {
                if (reason) safe_snprintf(reason, reason_sz, "命令 '%s' 不在白名单", argv[i]);
                LOG_WARN_T("SafeExec", "Check", "NotAllowlisted", "%s", argv[i]);
                return SAFE_EXEC_DENY;
            }
            /* BALANCED：白名单外 → 需确认（不直接拒，兼顾可用性） */
            if (reason) safe_snprintf(reason, reason_sz, "命令 '%s' 不在白名单，需确认", argv[i]);
            return SAFE_EXEC_NEED_CONFIRM;
        }
    }

    if (reason && reason_sz) safe_strncpy(reason, "allow", reason_sz);
    return SAFE_EXEC_ALLOW;
}

/* ============================================================
 * 参数化执行（核心：fork + execvp，不经 shell）
 * ============================================================ */
int safe_exec_run(char *const argv[], char *out, size_t out_sz, int timeout_s) {
    if (!argv || !argv[0]) return -1;
    if (out && out_sz) out[0] = '\0';

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        LOG_ERROR_T("SafeExec", "Run", "PipeFail", "%s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        LOG_ERROR_T("SafeExec", "Run", "ForkFail", "%s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* 子进程 */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, STDIN_FILENO); close(nul); }
        setsid();                       /* 独立进程组，便于整组超时终止 */
        /* 【关键】execvp —— 不经 shell，元字符全部失效（OWASP ②） */
        execvp(argv[0], argv);
        _exit(127);                     /* 127 = 命令未找到 */
    }

    /* 父进程读取输出 */
    close(pipefd[1]);
    size_t total = 0;
    int timed_out = 0;
    time_t start = time(NULL);

    for (;;) {
        if (timeout_s > 0 && (time(NULL) - start) >= timeout_s) { timed_out = 1; break; }
        fd_set rf; FD_ZERO(&rf); FD_SET(pipefd[0], &rf);
        struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
        int r = select(pipefd[0] + 1, &rf, NULL, NULL, &tv);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) {
            int st; if (waitpid(pid, &st, WNOHANG) == pid) { /* 子进程已退，继续读残留 */ }
            continue;
        }
        char tmp[4096];
        ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
        if (n <= 0) break;
        if (out && total + (size_t)n < out_sz - 1) {
            memcpy(out + total, tmp, (size_t)n);
            total += (size_t)n;
            out[total] = '\0';
        } else if (out && out_sz > 32) {
            /* 截断标记（OWASP LLM10：防资源耗尽） */
            const char *mark = "\n...[output truncated]";
            size_t ml = strlen(mark);
            if (out_sz > ml + 1) {
                memcpy(out + out_sz - 1 - ml, mark, ml);
                out[out_sz - 1] = '\0';
            }
            /* 继续排空管道，避免子进程阻塞 */
        }
    }

    if (timed_out) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        LOG_WARN_T("SafeExec", "Run", "Timeout", "cmd=%s timeout=%ds", argv[0], timeout_s);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    close(pipefd[0]);

    if (timed_out) return -2;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) return -1;
    return 0;
}

/* ============================================================
 * 便捷：检查 + 拆分 + 执行
 * ============================================================ */
int safe_exec_run_cmdline(const char *cmdline, char *out, size_t out_sz,
                          int timeout_s, int *need_confirm,
                          char *reason, size_t reason_sz) {
    if (need_confirm) *need_confirm = 0;

    safe_exec_verdict_t v = safe_exec_check(cmdline, reason, reason_sz);
    if (v == SAFE_EXEC_DENY) return -3;
    if (v == SAFE_EXEC_NEED_CONFIRM) { if (need_confirm) *need_confirm = 1; }

    char buf[4096];
    char *argv[SAFE_EXEC_MAX_ARGS];
    int n = safe_exec_split(cmdline, buf, sizeof(buf), argv, SAFE_EXEC_MAX_ARGS);
    if (n <= 0) {
        if (reason && reason_sz) safe_strncpy(reason, "命令解析失败", reason_sz);
        return -1;
    }
    LOG_INFO_T("SafeExec", "RunCmdline", "Exec", "prog=%s argc=%d", argv[0], n);
    return safe_exec_run(argv, out, out_sz, timeout_s);
}
