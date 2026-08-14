/**
 * @file    nook_repair.c
 * @brief   Nook 自主修复系统（接收触发，与 Python 端通信）
 * @version LN-B-5.0.0.0
 * @changes 安全字符串替换；日志标准化；双文支持
 */

#include "nook_repair.h"
#include "log_extra.h"
#include "data_path.h"
#include "uart.h"
#include "safe_string.h"
#include "lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <time.h>

#define REPAIR_SOCKET_PATH "/LINGOS/run/repair.sock"   /* 兼容引用；实际路径由 lingos_data_root() 动态拼接 */

static repair_status_t repair_status = REPAIR_STATUS_IDLE;
static char last_error[256] = {0};
static char last_fingerprint[64] = {0};

/* ============================================================
 * 内部辅助：连接到 Python 修复引擎
 * ============================================================ */
static int connect_repair_engine(void) {
    LOG_DEBUG_T("NookRepair", "Connect", "Enter", "Attempting to connect to %s", REPAIR_SOCKET_PATH);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR_T("NookRepair", "Connect", "SocketFail", "socket() error: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* 【修复】动态拼接 socket 路径，避免硬编码 /LINGOS（UD-HM#R1） */
    const char *root = lingos_data_root();
    char sock_path[sizeof(addr.sun_path)];
    safe_snprintf(sock_path, sizeof(sock_path), "%s/run/repair.sock", root);
    safe_strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR_T("NookRepair", "Connect", "ConnectFail", "connect to %s failed: %s (errno=%d)", REPAIR_SOCKET_PATH, strerror(errno), errno);
        close(fd);
        return -1;
    }
    LOG_DEBUG_T("NookRepair", "Connect", "Success", "Connected to repair engine, fd=%d", fd);
    return fd;
}

/* ============================================================
 * 内部辅助：发送命令并接收响应
 * ============================================================ */
static int send_command(const char *cmd, char *resp, size_t resp_len) {
    LOG_DEBUG_T("NookRepair", "SendCmd", "Enter", "cmd=%.100s...", cmd);
    int fd = connect_repair_engine();
    if (fd < 0) {
        LOG_WARN_T("NookRepair", "SendCmd", "ConnectFail", "Cannot connect to repair engine");
        return -1;
    }
    ssize_t written = write(fd, cmd, strlen(cmd));
    if (written < 0) {
        LOG_ERROR_T("NookRepair", "SendCmd", "WriteFail", "write() error: %s (errno=%d)", strerror(errno), errno);
        close(fd);
        return -1;
    }
    if (write(fd, "\n", 1) < 0) {
        LOG_ERROR_T("NookRepair", "SendCmd", "NewlineFail", "write newline error: %s", strerror(errno));
        close(fd);
        return -1;
    }
    LOG_DEBUG_T("NookRepair", "SendCmd", "WriteDone", "Sent %zd bytes", written);

    size_t pos = 0;
    while (pos < resp_len - 1) {
        ssize_t n = read(fd, resp + pos, 1);
        if (n <= 0) {
            LOG_WARN_T("NookRepair", "SendCmd", "ReadFail", "read() returned %zd, errno=%d", n, errno);
            break;
        }
        if (resp[pos] == '\n') {
            resp[pos] = '\0';
            LOG_DEBUG_T("NookRepair", "SendCmd", "ReadDone", "Response: %s", resp);
            close(fd);
            return 0;
        }
        pos++;
    }
    resp[pos] = '\0';
    LOG_WARN_T("NookRepair", "SendCmd", "Incomplete", "Incomplete response: %s", resp);
    close(fd);
    return -1;
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

void nook_repair_init(void) {
    LOG_INFO_T("NookRepair", "Init", "Enter", "Initializing Nook repair system");
    repair_status = REPAIR_STATUS_IDLE;
    last_error[0] = '\0';
    last_fingerprint[0] = '\0';
    LOG_INFO_T("NookRepair", "Init", "OK", "Repair system initialized (connects to Python repair engine at %s)", REPAIR_SOCKET_PATH);
}

int nook_repair_start(const char *error_desc, repair_severity_t severity, int auto_confirm) {
    LOG_INFO_T("NookRepair", "Start", "Enter", "error_desc='%s', severity=%d, auto_confirm=%d",
               error_desc ? error_desc : "(null)", severity, auto_confirm);
    if (!error_desc) {
        LOG_ERROR_T("NookRepair", "Start", "Invalid", "error_desc is NULL");
        return -1;
    }
    repair_status = REPAIR_STATUS_RUNNING;
    safe_strncpy(last_error, error_desc, sizeof(last_error));
    last_error[sizeof(last_error)-1] = '\0';
    LOG_DEBUG_T("NookRepair", "Start", "State", "repair_status set to RUNNING, last_error updated");

    const char *err_type = "manual";
    switch (severity) {
        case REPAIR_ERR_LOW:      err_type = "low"; break;
        case REPAIR_ERR_MEDIUM:   err_type = "medium"; break;
        case REPAIR_ERR_HIGH:     err_type = "high"; break;
        case REPAIR_ERR_CRITICAL: err_type = "critical"; break;
        default: err_type = "unknown"; break;
    }
    LOG_DEBUG_T("NookRepair", "Start", "ErrType", "Mapped severity to err_type='%s'", err_type);

    char cmd[512];
    safe_snprintf(cmd, sizeof(cmd),
                  "{\"cmd\":\"trigger_repair\",\"error_type\":\"%s\",\"error_msg\":\"%s\",\"fingerprint\":\"manual_%ld\"}",
                  err_type, error_desc, time(NULL));
    LOG_DEBUG_T("NookRepair", "Start", "Cmd", "Sending command: %s", cmd);

    char resp[256];
    int ret = send_command(cmd, resp, sizeof(resp));
    if (ret != 0) {
        LOG_ERROR_T("NookRepair", "Start", "SendFail", "send_command returned %d", ret);
        repair_status = REPAIR_STATUS_FAILED;
        LOG_DEBUG_T("NookRepair", "Start", "State", "repair_status set to FAILED");
        return -1;
    }
    LOG_DEBUG_T("NookRepair", "Start", "Response", "Received: %s", resp);
    if (strstr(resp, "accepted") != NULL) {
        repair_status = REPAIR_STATUS_RUNNING;
        LOG_INFO_T("NookRepair", "Start", "Accepted", "Repair task accepted by engine");
        uart_puts(tr("[Repair] Task accepted by repair engine.\n", "[修复] 修复引擎已接受任务。\n"));
        return 0;
    } else {
        repair_status = REPAIR_STATUS_IDLE;
        LOG_WARN_T("NookRepair", "Start", "Rejected", "Repair task rejected: %s", resp);
        uart_puts(tr("[Repair] Task rejected by repair engine.\n", "[修复] 修复引擎拒绝了任务。\n"));
        return -1;
    }
}

int nook_repair_trigger(const char *error_type, const char *error_msg, const char *fingerprint) {
    LOG_INFO_T("NookRepair", "Trigger", "Enter", "error_type='%s', error_msg='%.100s', fingerprint='%s'",
               error_type ? error_type : "(null)", error_msg ? error_msg : "(null)", fingerprint ? fingerprint : "(null)");
    if (!error_type || !error_msg) {
        LOG_ERROR_T("NookRepair", "Trigger", "Invalid", "error_type or error_msg is NULL");
        return -1;
    }
    repair_status = REPAIR_STATUS_RUNNING;
    safe_strncpy(last_error, error_msg, sizeof(last_error));
    last_error[sizeof(last_error)-1] = '\0';
    if (fingerprint) {
        safe_strncpy(last_fingerprint, fingerprint, sizeof(last_fingerprint));
        last_fingerprint[sizeof(last_fingerprint)-1] = '\0';
    }
    LOG_DEBUG_T("NookRepair", "Trigger", "State", "repair_status=RUNNING, last_error='%s', last_fingerprint='%s'",
                last_error, last_fingerprint);

    char cmd[512];
    safe_snprintf(cmd, sizeof(cmd),
                  "{\"cmd\":\"trigger_repair\",\"error_type\":\"%s\",\"error_msg\":\"%s\",\"fingerprint\":\"%s\"}",
                  error_type, error_msg, fingerprint ? fingerprint : "");
    LOG_DEBUG_T("NookRepair", "Trigger", "Cmd", "Sending command: %s", cmd);

    char resp[256];
    int ret = send_command(cmd, resp, sizeof(resp));
    if (ret != 0) {
        LOG_ERROR_T("NookRepair", "Trigger", "SendFail", "send_command returned %d", ret);
        repair_status = REPAIR_STATUS_FAILED;
        LOG_DEBUG_T("NookRepair", "Trigger", "State", "repair_status set to FAILED");
        return -1;
    }
    LOG_DEBUG_T("NookRepair", "Trigger", "Response", "Received: %s", resp);
    if (strstr(resp, "accepted") != NULL) {
        repair_status = REPAIR_STATUS_RUNNING;
        LOG_INFO_T("NookRepair", "Trigger", "Accepted", "Repair triggered successfully");
        uart_puts(tr("[Repair] Repair triggered successfully.\n", "[修复] 修复已成功触发。\n"));
        return 0;
    } else {
        repair_status = REPAIR_STATUS_IDLE;
        LOG_WARN_T("NookRepair", "Trigger", "Rejected", "Repair rejected by engine: %s", resp);
        uart_puts(tr("[Repair] Repair rejected by engine.\n", "[修复] 修复引擎拒绝了修复。\n"));
        return -1;
    }
}

int nook_repair_history(const char *id, char *out, size_t out_len) {
    LOG_DEBUG_T("NookRepair", "History", "Enter", "id='%s', out_len=%zu", id ? id : "(null)", out_len);
    if (!out || out_len == 0) {
        LOG_ERROR_T("NookRepair", "History", "Invalid", "out buffer is NULL or length 0");
        return -1;
    }
    char cmd[128];
    if (id && id[0] != '\0') {
        safe_snprintf(cmd, sizeof(cmd), "{\"cmd\":\"history_detail\",\"id\":\"%s\"}", id);
        LOG_DEBUG_T("NookRepair", "History", "CmdDetail", "Querying detail for id=%s", id);
    } else {
        safe_snprintf(cmd, sizeof(cmd), "{\"cmd\":\"history_list\"}");
        LOG_DEBUG_T("NookRepair", "History", "CmdList", "Querying history list");
    }
    int ret = send_command(cmd, out, out_len);
    if (ret != 0) {
        LOG_ERROR_T("NookRepair", "History", "SendFail", "send_command returned %d", ret);
        safe_snprintf(out, out_len, "{\"error\":\"Failed to query history\"}");
        return -1;
    }
    LOG_DEBUG_T("NookRepair", "History", "Success", "History query returned %zu bytes", strlen(out));
    return 0;
}

repair_status_t nook_repair_get_status(void) {
    LOG_DEBUG_T("NookRepair", "GetStatus", "Enter", "Current status=%d", repair_status);
    return repair_status;
}

const char* nook_repair_status_str(repair_status_t status) {
    const char *s;
    switch (status) {
        case REPAIR_STATUS_IDLE:      s = tr("idle", "空闲"); break;
        case REPAIR_STATUS_RUNNING:   s = tr("running", "运行中"); break;
        case REPAIR_STATUS_SUCCESS:   s = tr("success", "成功"); break;
        case REPAIR_STATUS_FAILED:    s = tr("failed", "失败"); break;
        case REPAIR_STATUS_ROLLBACK:  s = tr("rollback", "已回滚"); break;
        default:                      s = tr("unknown", "未知"); break;
    }
    LOG_DEBUG_T("NookRepair", "StatusStr", "status", "Status=%d -> '%s'", status, s);
    return s;
}

const char* nook_repair_get_last_error(void) {
    LOG_DEBUG_T("NookRepair", "GetLastError", "last_error='%s'", last_error);
    return last_error;
}

const char* nook_repair_get_status_desc(void) {
    static char desc[512];
    repair_status_t st = repair_status;
    LOG_DEBUG_T("NookRepair", "GetStatusDesc", "build", "Building description for status=%d", st);
    switch (st) {
        case REPAIR_STATUS_IDLE:
            safe_snprintf(desc, sizeof(desc), tr("Repair system is idle. No recent errors.",
                                                "修复系统空闲，无最近的错误。"));
            break;
        case REPAIR_STATUS_RUNNING:
            safe_snprintf(desc, sizeof(desc),
                          tr("Repair is in progress. Last error: %s",
                             "修复进行中。最后的错误：%s"),
                          last_error[0] ? last_error : tr("None", "无"));
            break;
        case REPAIR_STATUS_SUCCESS:
            safe_snprintf(desc, sizeof(desc),
                          tr("Repair completed successfully. Last error: %s",
                             "修复成功完成。最后的错误：%s"),
                          last_error[0] ? last_error : tr("None", "无"));
            break;
        case REPAIR_STATUS_FAILED:
            safe_snprintf(desc, sizeof(desc),
                          tr("Repair failed. Error: %s",
                             "修复失败。错误：%s"),
                          last_error[0] ? last_error : tr("Unknown", "未知"));
            break;
        case REPAIR_STATUS_ROLLBACK:
            safe_snprintf(desc, sizeof(desc),
                          tr("System rolled back after repair failure. Last error: %s",
                             "修复失败后系统已回滚。最后的错误：%s"),
                          last_error[0] ? last_error : tr("None", "无"));
            break;
        default:
            safe_snprintf(desc, sizeof(desc), tr("Unknown repair status (%d)", "未知的修复状态 (%d)"), st);
            break;
    }
    LOG_DEBUG_T("NookRepair", "GetStatusDesc", "Description: %s", desc);
    return desc;
}

void nook_repair_cleanup(void) {
    LOG_INFO_T("NookRepair", "Cleanup", "Enter", "Cleaning up repair system");
    repair_status = REPAIR_STATUS_IDLE;
    last_error[0] = '\0';
    last_fingerprint[0] = '\0';
    LOG_DEBUG_T("NookRepair", "Cleanup", "Done", "State reset");
}