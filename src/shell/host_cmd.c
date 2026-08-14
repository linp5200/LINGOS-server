#include "host_cmd.h"
#include "lang.h"
#include "log_extra.h"
#include "audit.h"
#include "uart.h"
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

int host_child_pid = 0;
volatile int host_running = 0;

void host_handle_sigquit(void) {
    if (host_running && host_child_pid > 0) {
        kill(host_child_pid, SIGTERM);
        LOG_DEBUG_T("HostCmd", "Signal", "Kill", "Sent SIGTERM to PID %d", host_child_pid);
    }
}

int exec_host_command(const char *cmd) {
    if (!cmd || !*cmd) {
        uart_puts(tr("Usage: host <command>\n", "用法：host <命令>\n"));
        return -1;
    }
    pid_t pid = fork();
    if (pid == -1) {
        LOG_ERROR_T("HostCmd", "Exec", "ForkFail", "fork failed");
        uart_puts(tr("fork failed\n", "fork 失败\n"));
        return -1;
    }
    if (pid == 0) {
        setsid();
        execlp("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        perror("execlp");
        _exit(1);
    } else {
        host_child_pid = pid;
        host_running = 1;
        LOG_DEBUG_T("HostCmd", "Exec", "Start", "PID=%d cmd=%s", pid, cmd);
        int status;
        waitpid(pid, &status, 0);
        host_running = 0;
        host_child_pid = 0;
        audit_log("shell", "host", cmd, "", "", status, "low", 1);
        LOG_DEBUG_T("HostCmd", "Exec", "Done", "exit status=%d", status);
    }
    return 0;
}