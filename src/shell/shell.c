/**
 * @file    src/shell/shell.c
 * @brief   LING OS 命令行核心 - 完整版（含 --force 支持 + 提示符增强）
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C3, C-C, AI-CTL
 * @changes 增加 --force 参数支持（配置强制覆盖）；
 *          增强提示符（支持 LINGOS_PROMPT 环境变量自定义）；
 *          统一使用标准注释格式。
 */

#include "shell.h"
#include "app_cmds.h"
#include "config_loader.h"
#include "skill_store.h"
#include "syswatch.h"
#include "commands.h"
#include "chat_terminal.h"
#include "system_config.h"
#include "host_cmd.h"
#include "history.h"
#include "alias.h"
#include "completion.h"
#include "types.h"
#include "string_no_sys.h"
#include "lang.h"
#include "mode.h"
#include "version.h"
#include "interactive.h"
#include "uart.h"
#include "timer.h"
#include "perm_debug.h"
#include "audit.h"
#include "api_core.h"
#include "nook.h"
#include "nook_repair.h"
#include "defense.h"
#include "ai_master.h"
#include "nook_idle.h"
#include "system_update.h"
#include "rollback_cmd.h"
#include "component_version.h"
#include "test_framework.h"
#include "scan_daemon.h"
#include "scan_config.h"
#include "scan_analyzer.h"
#include "ipc_core.h"
#include "data_path.h"
#include "libling.h"
#include "ai_config_cmd.h"
#include "system_debug.h"
#include "basic_cmds.h"
#include "log_extra.h"
#include "system_health.h"
#include "health_trend.h"
#include "health_watchdog.h"
#include "shell_config.h"
#include "ai_config.h"
#include "ai_server_protocol.h"
#include "debug_cmd.h"
#include "system_info.h"
#include "self_check.h"
#include "safe_string.h"
#include "config_cmd.h"
#include "backup.h"
#include "connection_handler.h"
#include "startup_mode.h"
#include "../config/wizard_engine.h"
#include "../config/config_renderer.h"
#include "../config/config_core.h"   /* 用于 config_core_save_force */
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/time.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

/* ============================================================
 * 外部函数声明
 * ============================================================ */
extern void defense_dispatch(const char *args);
extern void privilege_dispatch(const char *args);
extern void security_dispatch(const char *args);
extern void behavior_dispatch(const char *args);
extern void registry_dispatch(const char *args);

/* ============================================================
 * 静态函数声明
 * ============================================================ */
static void ai_status_command(void);
static void log_level_dispatch(const char *args);
static void sync_log_level_to_python(const char *level_str);

/* ============================================================
 * 常量
 * ============================================================ */
#define AUTH_SOCKET_PATH "/LINGOS/run/auth.sock"
#define INPUT_BUF_SIZE  SHELL_MAX_INPUT_LEN
#define DISK_WARN_THRESHOLD_MB 100

/* FTF[检查授权服务是否有待处理请求] */
static int auth_service_pending(char *request_id, size_t id_len) {
    LOG_DEBUG_T("Shell", "AuthPending", "enter", "id_len=%zu", id_len);
    if (!request_id || id_len == 0) {
        LOG_ERROR_T("Shell", "AuthPending", "invalid", "request_id=%p, id_len=%zu", (void*)request_id, id_len);
        return 0;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_WARN_T("Shell", "AuthPending", "SocketFail", "socket() error: %s (errno=%d)", strerror(errno), errno);
        return 0;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, AUTH_SOCKET_PATH, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK) {
            fd_set writefds;
            FD_ZERO(&writefds);
            FD_SET(fd, &writefds);
            struct timeval tv = {0, 100000};
            if (select(fd + 1, NULL, &writefds, NULL, &tv) <= 0) {
                close(fd);
                LOG_DEBUG_T("Shell", "AuthPending", "ConnectTimeout", "connection timeout");
                return 0;
            }
            int err;
            socklen_t errlen = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0 || err != 0) {
                close(fd);
                LOG_DEBUG_T("Shell", "AuthPending", "ConnectFail", "connection failed: %s", strerror(err));
                return 0;
            }
        } else {
            close(fd);
            LOG_DEBUG_T("Shell", "AuthPending", "ConnectFail", "connect failed: %s", strerror(errno));
            return 0;
        }
    }

    const char *query = "{\"cmd\":\"pending\"}\n";
    if (write(fd, query, strlen(query)) < 0) {
        LOG_WARN_T("Shell", "AuthPending", "WriteFail", "write failed: %s", strerror(errno));
        close(fd);
        return 0;
    }

    char buf[256];
    int n = read(fd, buf, sizeof(buf)-1);
    close(fd);

    if (n <= 0) {
        LOG_DEBUG_T("Shell", "AuthPending", "NoData", "no pending request");
        return 0;
    }
    buf[n] = '\0';

    const char *p = strstr(buf, "\"request_id\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < (int)id_len-1) {
        request_id[i++] = *p++;
    }
    request_id[i] = '\0';
    return (i > 0) ? 1 : 0;
}

/* FTF[响应授权请求] */
static void auth_service_respond(const char *request_id, int approved) {
    LOG_DEBUG_T("Shell", "AuthRespond", "Enter", "request_id='%s', approved=%d", request_id ? request_id : "(null)", approved);
    if (!request_id || !*request_id) return;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR_T("Shell", "AuthRespond", "SocketFail", "socket() error: %s", strerror(errno));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, AUTH_SOCKET_PATH, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR_T("Shell", "AuthRespond", "ConnectFail", "connect to %s failed: %s", AUTH_SOCKET_PATH, strerror(errno));
        close(fd);
        return;
    }

    char msg[256];
    safe_snprintf(msg, sizeof(msg), "{\"cmd\":\"respond\",\"request_id\":\"%s\",\"decision\":%s}\n",
                  request_id, approved ? "true" : "false");
    write(fd, msg, strlen(msg));
    close(fd);
}

/* FTF[发送命令到 AI 服务器] */
static int send_to_ai_server(const char *cmd_json, char *resp_buf, size_t buf_len) {
    LOG_DEBUG_T("Shell", "SendToAI", "Enter", "cmd='%s'", cmd_json ? cmd_json : "(null)");
    if (!cmd_json || !resp_buf || buf_len == 0) {
        LOG_ERROR_T("Shell", "SendToAI", "Invalid", "cmd_json=%p, resp_buf=%p, buf_len=%zu",
                    (void*)cmd_json, (void*)resp_buf, buf_len);
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR_T("Shell", "SendToAI", "SocketFail", "socket() error: %s", strerror(errno));
        safe_snprintf(resp_buf, buf_len, tr("Error: Cannot connect to AI server", "错误：无法连接到 AI 服务器"));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, AI_SOCKET_PATH, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR_T("Shell", "SendToAI", "ConnectFail", "connect to %s failed: %s", AI_SOCKET_PATH, strerror(errno));
        close(fd);
        safe_snprintf(resp_buf, buf_len, tr("Error: AI server not reachable", "错误：AI 服务器不可达"));
        return -1;
    }

    if (write(fd, cmd_json, strlen(cmd_json)) < 0 || write(fd, "\n", 1) < 0) {
        LOG_ERROR_T("Shell", "SendToAI", "WriteFail", "write failed: %s", strerror(errno));
        close(fd);
        safe_snprintf(resp_buf, buf_len, tr("Error: Write failed", "错误：写入失败"));
        return -1;
    }

    int pos = 0;
    while (pos < (int)buf_len - 1) {
        ssize_t n = read(fd, resp_buf + pos, 1);
        if (n <= 0) break;
        if (resp_buf[pos] == '\n') {
            resp_buf[pos] = '\0';
            close(fd);
            return 0;
        }
        pos++;
    }
    resp_buf[pos] = '\0';
    close(fd);
    return (pos > 0) ? 0 : -1;
}

/* FTF[查询 AI 服务状态] */
int ai_status_query(void) {
    LOG_DEBUG_T("Shell", "AIStatusQuery", "enter", "");
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_WARN_T("Shell", "AIStatusQuery", "SocketFail", "socket() error: %s", strerror(errno));
        return 0;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, AI_SOCKET_PATH, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return 0;
    }

    const char *ping = "{\"cmd\":\"ping\"}\n";
    if (write(fd, ping, strlen(ping)) < 0) {
        close(fd);
        return 0;
    }

    char buf[64];
    int n = read(fd, buf, sizeof(buf)-1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return (strstr(buf, "\"pong\"") != NULL) ? 1 : 0;
}

/* FTF[获取后台任务数量] */
static int get_background_task_count(void) {
    LOG_DEBUG_T("Shell", "GetBgTaskCount", "enter", "");
    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/state/tasks.json", root);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("Shell", "GetBgTaskCount", "NoFile", "tasks.json not found");
        return 0;
    }
    int count = 0;
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "\"status\"")) count++;
    }
    fclose(fp);
    return count;
}

/* FTF[处理 agent 命令（子 AI 对话协作状态）] */
static void handle_agent_command(const char *args) {
    LOG_DEBUG_T("Shell", "AgentCommand", "Enter", "args='%s'", args ? args : "(null)");
    if (args && *args && strcmp(args, "status") != 0 && strcmp(args, "list") != 0) {
        uart_puts(tr("Usage: agent status\n", "用法：agent status\n"));
        return;
    }

    char cmd_json[64];
    safe_snprintf(cmd_json, sizeof(cmd_json), "{\"cmd\":\"agent_status\"}");
    char resp_buf[4096];
    int ret = send_to_ai_server(cmd_json, resp_buf, sizeof(resp_buf));
    if (ret != 0) {
        uart_puts(tr("AI server not reachable.\n", "AI 服务器不可达。\n"));
        return;
    }

    /* 解析响应：{"status":"ok","agents":[{task_id,role,status,round,max_rounds},...]} */
    cJSON *root = cJSON_Parse(resp_buf);
    if (!root) {
        uart_puts(tr("Invalid response.\n", "无效响应。\n"));
        return;
    }
    cJSON *agents = cJSON_GetObjectItem(root, "agents");
    if (!cJSON_IsArray(agents)) {
        uart_puts(tr("No agent sessions.\n", "无子 AI 会话。\n"));
        cJSON_Delete(root);
        return;
    }
    uart_puts(COLOR_CYAN);
    uart_puts(tr("=== Sub-Agent Sessions (Dialogue Collaboration) ===\n",
                 "=== 子 AI 会话（对话协作） ===\n"));
    uart_puts(COLOR_RESET);
    int n = cJSON_GetArraySize(agents);
    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_GetArrayItem(agents, i);
        cJSON *role = cJSON_GetObjectItem(a, "role");
        cJSON *status = cJSON_GetObjectItem(a, "status");
        cJSON *task_id = cJSON_GetObjectItem(a, "task_id");
        cJSON *round = cJSON_GetObjectItem(a, "round");
        cJSON *max_rounds = cJSON_GetObjectItem(a, "max_rounds");

        const char *status_str = "";
        if (status && cJSON_IsString(status)) {
            if (strcmp(status->valuestring, "running") == 0)
                status_str = tr("⟳ running", "⟳ 执行中");
            else if (strcmp(status->valuestring, "pending") == 0)
                status_str = tr("⏳ pending", "⏳ 等待中");
            else if (strcmp(status->valuestring, "waiting_main") == 0)
                status_str = tr("⏸ waiting main AI", "⏸ 等待主 AI");
            else if (strcmp(status->valuestring, "completed") == 0)
                status_str = tr("✅ completed", "✅ 已完成");
            else if (strcmp(status->valuestring, "failed") == 0)
                status_str = tr("❌ failed", "❌ 失败");
            else
                status_str = status->valuestring;
        }

        char line[256];
        safe_snprintf(line, sizeof(line), "  %s  |  %s  |  %s  |  round %d/%d\n",
                      role && cJSON_IsString(role) ? role->valuestring : "?",
                      status_str,
                      task_id && cJSON_IsString(task_id) ? task_id->valuestring : "?",
                      round && cJSON_IsNumber(round) ? round->valueint : 0,
                      max_rounds && cJSON_IsNumber(max_rounds) ? max_rounds->valueint : 20);
        uart_puts(line);
    }
    cJSON_Delete(root);
}

/* ============================================================
 * 【先生设计】token 命令族（令牌系统扩展）
 * token login again <code>                    —— 主机输入验证码（等待 App 匹配——60s）
 * token remove login <ip/uid> [time] [--NReverify] —— 移除登录验证（免验证——受限）
 * token remove <ip/uid>                       —— 移除某 ip/设备令牌
 * token add <ip/uid> [token]                  —— 添加某 ip/设备令牌（可指定）
 * token add login <ip/uid>                    —— 添加某设备登录验证
 * token add <token>                           —— 添加令牌（不指明 ip/设备）
 * token remove <token>                        —— 移除令牌（不指明 ip/设备）
 * ============================================================ */
static void handle_token_command(const char *args) {
    if (!args || !*args) {
        uart_puts(tr("Usage: token login again <code> | token remove login <ip> [time] [--NReverify] | token add <ip/uid> [token] | token remove <ip/uid/token> | token add login <ip> | token remove login <ip> [time] [--NReverify]\n",
                     "用法：token login again <验证码> | token remove login <ip> [时间] [--NReverify] | token add <ip/uid> [令牌] | token remove <ip/uid/令牌> | token add login <ip> | token remove login <ip> [时间] [--NReverify]\n"));
        return;
    }
    char arg_buf[256];
    safe_strncpy(arg_buf, args, sizeof(arg_buf));
    arg_buf[sizeof(arg_buf)-1] = '\0';
    char *saveptr;
    char *sub = strtok_r(arg_buf, " ", &saveptr);
    if (!sub) return;
    if (strcmp(sub, "login") == 0) {
        char *action = strtok_r(NULL, " ", &saveptr);
        if (action && strcmp(action, "again") == 0) {
            char *code = strtok_r(NULL, " ", &saveptr);
            if (code) {
                connection_pending_set(code, NULL, 60);
                uart_puts(tr("等待 App 输入验证码（60 秒有效）...\n", "等待 App 输入验证码（60 秒有效）...\n"));
            } else {
                uart_puts(tr("缺少验证码：token login again <code>\n", "缺少验证码：token login again <验证码>\n"));
            }
        } else if (action) {
            /* token remove login <ip/uid> [time] [--NReverify] */
            char *ip = action;
            char *time_str = strtok_r(NULL, " ", &saveptr);
            char *flag = NULL;
            int permanent = 0, reverify = 0;
            if (time_str) {
                if (strcmp(time_str, "no") == 0) permanent = 1;
                else if (strcmp(time_str, "--NReverify") == 0) { reverify = 1; time_str = NULL; }
                else {
                    flag = strtok_r(NULL, " ", &saveptr);
                    if (flag && strcmp(flag, "--NReverify") == 0) reverify = 1;
                }
            }
            if (reverify) {
                connection_no_verify_remove(ip);
                uart_puts(tr("已移除登录验证（重新验证）\n", "已移除登录验证（重新验证）\n"));
            } else {
                connection_no_verify_set(ip, time_str, permanent);
                char msg[128];
                safe_snprintf(msg, sizeof(msg), "已设置 %s 免登录验证（受限模式——危险操作拒绝）\n", ip);
                uart_puts(tr(msg, msg));
            }
        } else {
            uart_puts(tr("token login again <code> | token remove login <ip> [time]\n", "token login again <验证码> | token remove login <ip> [时间]\n"));
        }
    } else if (strcmp(sub, "add") == 0) {
        char *arg1 = strtok_r(NULL, " ", &saveptr);
        if (!arg1) {
            uart_puts(tr("token add <ip/uid> [token] | token add <token>\n", "token add <ip/uid> [令牌] | token add <令牌>\n"));
            return;
        }
        char *arg2 = strtok_r(NULL, " ", &saveptr);
        if (arg2) {
            /* token add <ip/uid> <token> */
            connection_token_add(arg2, arg1, arg1, 2592000);
            uart_puts(tr("令牌已添加\n", "令牌已添加\n"));
        } else {
            /* token add <token>（或 <ip/uid>——无 token——自动生成并绑定） */
            connection_token_add(NULL, arg1, arg1, 2592000);
            uart_puts(tr("令牌已添加（自动生成）\n", "令牌已添加（自动生成）\n"));
        }
    } else if (strcmp(sub, "remove") == 0) {
        char *arg1 = strtok_r(NULL, " ", &saveptr);
        if (arg1) {
            connection_token_remove(arg1);
            uart_puts(tr("令牌已移除\n", "令牌已移除\n"));
        } else {
            uart_puts(tr("token remove <ip/uid/token>\n", "token remove <ip/uid/令牌>\n"));
        }
    } else {
        uart_puts(tr("未知 token 子命令\n", "未知 token 子命令\n"));
    }
}

/* FTF[处理子 AI 命令] */
static void handle_subai_command(const char *args) {
    LOG_DEBUG_T("Shell", "SubAICommand", "Enter", "args='%s'", args ? args : "(null)");
    if (!args || !*args) {
        uart_puts(tr("Usage: sub_ai status [<task_id>] | sub_ai status on|off\n",
                     "用法：sub_ai status [<任务ID>] | sub_ai status on|off\n"));
        return;
    }

    char arg_buf[128];
    safe_strncpy(arg_buf, args, sizeof(arg_buf));
    arg_buf[sizeof(arg_buf)-1] = '\0';

    char *saveptr;
    char *subcmd = strtok_r(arg_buf, " ", &saveptr);

    if (!subcmd) {
        uart_puts(tr("Missing subcommand\n", "缺少子命令\n"));
        return;
    }

    if (strcmp(subcmd, "status") == 0) {
        char *param = strtok_r(NULL, " ", &saveptr);
        if (param && (strcmp(param, "on") == 0 || strcmp(param, "off") == 0)) {
            int enable = (strcmp(param, "on") == 0);
            char cmd_json[128];
            safe_snprintf(cmd_json, sizeof(cmd_json),
                          "{\"cmd\":\"sub_ai_notification\",\"enable\":%s}",
                          enable ? "true" : "false");
            char resp_buf[256];
            int ret = send_to_ai_server(cmd_json, resp_buf, sizeof(resp_buf));
            if (ret == 0) {
                uart_puts(tr("Sub-AI notification ", "子AI通知 "));
                uart_puts(enable ? tr("enabled\n", "已启用\n") : tr("disabled\n", "已禁用\n"));
            } else {
                uart_puts(tr("Failed to set notification\n", "设置通知失败\n"));
            }
            return;
        }

        if (param && *param) {
            char cmd_json[128];
            safe_snprintf(cmd_json, sizeof(cmd_json),
                          "{\"cmd\":\"get_task_status\",\"task_id\":\"%s\"}", param);
            char resp_buf[4096];
            int ret = send_to_ai_server(cmd_json, resp_buf, sizeof(resp_buf));
            if (ret == 0) {
                log_draw_box(tr("Task Status", "任务状态"), resp_buf,
                             COLOR_CYAN, COLOR_DIM, COLOR_WHITE);
            } else {
                uart_puts(tr("Failed to get task status\n", "获取任务状态失败\n"));
            }
        } else {
            char *cmd_json = "{\"cmd\":\"get_task_status\"}";
            char resp_buf[8192];
            int ret = send_to_ai_server(cmd_json, resp_buf, sizeof(resp_buf));
            if (ret == 0) {
                log_draw_box(tr("Sub-AI Task List", "子AI任务列表"),
                             resp_buf, COLOR_CYAN, COLOR_DIM, COLOR_WHITE);
            } else {
                uart_puts(tr("Failed to get task list\n", "获取任务列表失败\n"));
            }
        }
    } else {
        uart_puts(tr("Unknown sub-command\n", "未知子命令\n"));
        uart_puts(tr("Available: status [<task_id>] | status on|off\n",
                     "可用：status [<任务ID>] | status on|off\n"));
    }
}

/* FTF[处理配置重载] */
static void handle_config_reload(const char *category) {
    LOG_DEBUG_T("Shell", "ConfigReload", "Enter", "category='%s'", category ? category : "(null)");
    if (!category || !*category) {
        uart_puts(tr("Usage: config reload <category>\n", "用法：config reload <分类>\n"));
        uart_puts(tr("Categories: ai, defense, permission, health, scan, all\n",
                     "分类：ai, defense, permission, health, scan, all\n"));
        return;
    }

    /* ---- 1. C 端热加载（无需重启，全量刷新核心配置） ---- */
    int c_ret = config_reload_all();
    if (c_ret == 0) {
        uart_puts(tr("C-side configs reloaded: ", "C 端配置已重载："));
        uart_puts(category);
        uart_puts("\n");
        LOG_INFO_T("Shell", "ConfigReload", "COK", "C-side reloaded category '%s'", category);
    } else {
        LOG_WARN_T("Shell", "ConfigReload", "CFail", "C-side reload returned %d", c_ret);
    }

    /* ---- 2. Python AI Server 同步（保持原有机制） ---- */
    char cmd_json[128];
    safe_snprintf(cmd_json, sizeof(cmd_json),
                  "{\"cmd\":\"reload_config\",\"category\":\"%s\"}", category);
    char resp_buf[512];
    int ret = send_to_ai_server(cmd_json, resp_buf, sizeof(resp_buf));
    if (ret == 0) {
        uart_puts(tr("Config reloaded: ", "配置已重载："));
        uart_puts(category);
        uart_puts("\n");
        LOG_INFO_T("Shell", "ConfigReload", "OK", "reloaded category '%s'", category);
    } else {
        uart_puts(tr("C-side reloaded, Python sync failed (AI server offline?)\n",
                     "C 端已重载，Python 同步失败（AI 服务器离线？）\n"));
        LOG_WARN_T("Shell", "ConfigReload", "PyFail", "send_to_ai_server returned %d", ret);
    }
}

/* FTF[处理 system startup 命令] */
static void handle_startup_command(const char *args) {
    LOG_DEBUG_T("Shell", "StartupCmd", "Enter", "args='%s'", args ? args : "(null)");
    if (!args || !*args) {
        startup_mode_show();
        return;
    }

    char arg_buf[32];
    safe_strncpy(arg_buf, args, sizeof(arg_buf));
    arg_buf[sizeof(arg_buf)-1] = '\0';

    if (strcmp(arg_buf, "shell") == 0) {
        if (startup_mode_set(STARTUP_MODE_SHELL) == 0) {
            uart_puts(tr("Startup mode set to: Shell (CLI)\n", "启动模式设置为：Shell（命令行）\n"));
        } else {
            uart_puts(tr("Failed to set startup mode\n", "设置启动模式失败\n"));
        }
    } else if (strcmp(arg_buf, "tui") == 0) {
        if (startup_mode_set(STARTUP_MODE_TUI) == 0) {
            uart_puts(tr("Startup mode set to: TUI Desktop\n", "启动模式设置为：TUI 桌面\n"));
        } else {
            uart_puts(tr("Failed to set startup mode\n", "设置启动模式失败\n"));
        }
    } else if (strcmp(arg_buf, "show") == 0) {
        startup_mode_show();
    } else {
        uart_puts(tr("Usage: system startup shell|tui|show\n", "用法：system startup shell|tui|show\n"));
    }
}

/* FTF[处理 desktop 命令] */
static void handle_desktop_command(void) {
    uart_puts(tr("Switching to TUI Desktop...\n", "正在切换到 TUI 桌面...\n"));
    LOG_INFO_T("Shell", "Desktop", "Enter", "User requested TUI desktop");
    extern int tui_desktop_run(void);
    int ret = tui_desktop_run();
    if (ret == 0) {
        LOG_INFO_T("Shell", "Desktop", "OK", "TUI desktop exited normally");
    } else {
        LOG_WARN_T("Shell", "Desktop", "Fail", "TUI desktop exited with error %d", ret);
    }
}

/* FTF[显示帮助信息] */
static void show_help_topic(const char *topic) {
    LOG_DEBUG_T("Shell", "Help", "Enter", "topic='%s'", topic ? topic : "(null)");
    if (!topic || !*topic) {
        uart_puts(tr(COLOR_BOLD COLOR_CYAN "┌─────────────── LING OS Help ───────────────┐\n" COLOR_RESET,
                     COLOR_BOLD COLOR_CYAN "┌─────────────── LING OS 帮助 ───────────────┐\n" COLOR_RESET));
        uart_puts(tr("│ Use: help <topic> to view detailed help       │\n",
                     "│ 使用: help <主题> 查看详细帮助              │\n"));
        uart_puts(tr("│  Topics: basic  ai  system  debug  app  cmd   │\n",
                     "│  主题: basic  ai  system  debug  app  cmd   │\n"));
        uart_puts("└─────────────────────────────────────────────┘\n");
        return;
    }

    const char *help_text = NULL;
    if (strcmp(topic, "cmd") == 0) {
        /* 【2026-08-22 定稿】动词子命令帮助（git/docker 风格——先生裁决）
         * 形式一（领域 动作 值）=设置；形式二（动作 领域）=列出/查询 */
        help_text = tr(
            "Verb sub-commands (human readable):\n"
            "  list model|session|skill|permission|mcp|alert|usage|personality|ha|memory\n"
            "  view status|model|session|ha\n"
            "  show status|model|ha\n"
            "  query balance|usage|voice|ha\n"
            "  model list|switch <id>\n"
            "  session list|create <title>|delete <id>|history <id>\n"
            "  skill list|enable <name>|disable <name>\n"
            "  memory search <kw>|write <text>\n"
            "  voice usage|query\n"
            "  ha status|states\n"
            "  provider list\n"
            "  log level <debug|info|warn|error>\n"
            "  config advanced   - advanced config (dedicated commands)\n"
            "  ?                 - context hint (e.g. log ?)\n",
            "动词子命令（人类可读——先生裁决）：\n"
            "  list model|session|skill|permission|mcp|alert|usage|personality|ha|memory\n"
            "  view status|model|session|ha\n"
            "  show status|model|ha\n"
            "  query balance|usage|voice|ha\n"
            "  model list|switch <id>\n"
            "  session list|create <标题>|delete <id>|history <id>\n"
            "  skill list|enable <名称>|disable <名称>\n"
            "  memory search <关键词>|write <内容>\n"
            "  voice usage|query\n"
            "  ha status|states\n"
            "  provider list\n"
            "  log level <debug|info|warn|error>\n"
            "  config advanced   - 高级配置（特定指令）\n"
            "  ?                 - 上下文提示（如 log ?）\n"
        );
    } else if (strcmp(topic, "basic") == 0) {
        help_text = tr(
            "Basic commands:\n"
            "  help [topic]   - Show help\n"
            "  clear          - Clear screen\n"
            "  reboot/poweroff - Reboot/shutdown\n"
            "  logdump        - Show logs\n"
            "  audit          - Audit log\n"
            "  alias/unalias  - Alias management\n"
            "  history        - Command history\n",
            "基础命令:\n"
            "  help [主题]   - 显示帮助\n"
            "  clear          - 清屏\n"
            "  reboot/poweroff - 重启/关机\n"
            "  logdump        - 显示日志\n"
            "  audit          - 审计日志\n"
            "  alias/unalias  - 别名管理\n"
            "  history        - 命令历史\n"
        );
    } else if (strcmp(topic, "ai") == 0) {
        help_text = tr(
            "AI commands:\n"
            "  nook ask <msg>      - Ask Nook\n"
            "  nook chat           - Enter chat terminal\n"
            "  nook allow-high-risk - Enable high-risk auto-auth\n"
            "  nook disallow-high-risk - Disable\n"
            "  nook callme <name>  - Set name\n"
            "  nook status         - Show status\n"
            "  nook model set/show - Model management\n"
            "  ai config           - AI config\n"
            "  ai status           - AI service status\n",
            "AI 命令:\n"
            "  nook ask <消息>      - 向 Nook 提问\n"
            "  nook chat           - 进入对话终端\n"
            "  nook allow-high-risk - 启用高风险自动授权\n"
            "  nook disallow-high-risk - 禁用\n"
            "  nook callme <名称>  - 设置称呼\n"
            "  nook status         - 显示状态\n"
            "  nook model set/show - 模型管理\n"
            "  ai config           - AI 配置\n"
            "  ai status           - AI 服务状态\n"
        );
    } else if (strcmp(topic, "system") == 0) {
        help_text = tr(
            "System management:\n"
            "  system configuration - Manual configuration\n"
            "  system update <pkg>  - Update system\n"
            "  system rollback     - Rollback\n"
            "  system health       - Health status\n"
            "  system status       - Version info\n"
            "  system debug info   - Debug info\n"
            "  system startup shell|tui|show - Startup mode\n"
            "  system defense status|shadow|dark|absolute - Defense modes\n"
            "  system privilege status|developer - Privilege modes\n"
            "  system security status|input - Security settings\n"
            "  system behavior status|algorithm|suggest - Behavior monitoring\n"
            "  app list/install/run/stop/logs - App management\n"
            "  config reload <cat> - Hot reload config\n"
            "  sub_ai status       - Sub-AI task status\n"
            "  registry list/show/reload - Registry management\n"
            "  log level [debug|info|warn|error] - Set log level\n",
            "系统管理:\n"
            "  system configuration - 手动配置\n"
            "  system update <包>   - 更新系统\n"
            "  system rollback     - 回滚\n"
            "  system health       - 健康状态\n"
            "  system status       - 版本信息\n"
            "  system debug info   - 调试信息\n"
            "  system startup shell|tui|show - 启动模式\n"
            "  system defense status|shadow|dark|absolute - 防御模式\n"
            "  system privilege status|developer - 权限模式\n"
            "  system security status|input - 安全设置\n"
            "  system behavior status|algorithm|suggest - 行为监控\n"
            "  app list/install/run/stop/logs - 应用管理\n"
            "  config reload <分类> - 热重载配置\n"
            "  sub_ai status       - 子AI任务状态\n"
            "  registry list/show/reload - 注册表管理\n"
            "  log level [debug|info|warn|error] - 设置日志级别\n"
        );
    } else if (strcmp(topic, "debug") == 0) {
        help_text = tr(
            "Debug commands:\n"
            "  debug set module=<m> level=<l> - Log level\n"
            "  debug list/show   - View module logs\n"
            "  test list/run     - Test cases\n"
            "  scan status/pause/resume - Scan control\n",
            "调试命令:\n"
            "  debug set module=<模块> level=<级别> - 日志级别\n"
            "  debug list/show   - 查看模块日志\n"
            "  test list/run     - 测试用例\n"
            "  scan status/pause/resume - 扫描控制\n"
        );
    } else if (strcmp(topic, "app") == 0) {
        help_text = tr(
            "App management:\n"
            "  app install <path>   - Install .lapt/.deb\n"
            "  app uninstall <name> - Uninstall app\n"
            "  app list            - List installed\n"
            "  app run <name>      - Start app\n"
            "  app stop <name>     - Stop app\n"
            "  app logs <name>     - View logs\n"
            "  app search <keyword> - Search repository\n"
            "  app update <name>   - Update app\n"
            "  app upgrade         - Upgrade all\n",
            "应用管理:\n"
            "  app install <路径>   - 安装 .lapt/.deb\n"
            "  app uninstall <名称> - 卸载应用\n"
            "  app list            - 列出已安装\n"
            "  app run <名称>      - 启动应用\n"
            "  app stop <名称>     - 停止应用\n"
            "  app logs <名称>     - 查看日志\n"
            "  app search <关键词> - 搜索仓库\n"
            "  app update <名称>   - 更新应用\n"
            "  app upgrade         - 升级全部\n"
        );
    } else {
        help_text = tr("Unknown topic. Available: basic, ai, system, debug, app\n",
                       "未知主题，可用主题: basic, ai, system, debug, app\n");
    }

    log_draw_box(tr("Help", "帮助"), help_text, COLOR_CYAN, COLOR_DIM, COLOR_WHITE);
}

/* FTF[检查磁盘空间] */
static int check_disk_space(void) {
    LOG_DEBUG_T("Shell", "CheckDisk", "enter", "");
    const char *root = lingos_data_root();
    struct statvfs stfs;
    if (statvfs(root, &stfs) != 0) {
        LOG_WARN_T("Shell", "CheckDisk", "StatvfsFail", "statvfs failed: %s", strerror(errno));
        return 0;
    }
    unsigned long long free_space = (unsigned long long)stfs.f_bsize * stfs.f_bavail;
    unsigned long long free_mb = free_space / (1024 * 1024);
    if (free_mb < DISK_WARN_THRESHOLD_MB) {
        uart_puts(COLOR_YELLOW);
        uart_puts(tr("\n⚠ Disk space is low (", "\n⚠ 磁盘空间不足 ("));
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
                                if (unlink(full_path) == 0) {
                                    LOG_INFO_T("Shell", "Cleanup", "Deleted", "log file: %s", entry->d_name);
                                }
                            }
                        }
                    }
                }
                closedir(d);
            }
            char cache_dir[512];
            safe_snprintf(cache_dir, sizeof(cache_dir), "%s/cache", root2);
            if (access(cache_dir, F_OK) == 0) {
                char cmd[512];
                safe_snprintf(cmd, sizeof(cmd), "rm -rf '%s'/* 2>/dev/null", cache_dir);
                system(cmd);
                LOG_INFO_T("Shell", "Cleanup", "Cache", "cache cleared");
            }
            char smemory_dir[512];
            safe_snprintf(smemory_dir, sizeof(smemory_dir), "%s/data/ai_memory/ai_smemory", root2);
            if (access(smemory_dir, F_OK) == 0) {
                char cmd[512];
                safe_snprintf(cmd, sizeof(cmd), "rm -rf '%s'/* 2>/dev/null", smemory_dir);
                system(cmd);
                LOG_INFO_T("Shell", "Cleanup", "SMemory", "short-term memory cleared");
            }
            uart_puts(tr("Cleanup completed.\n", "清理完成。\n"));
            LOG_INFO_T("Shell", "Cleanup", "Done", "disk cleanup completed");
        } else {
            uart_puts(tr("Cleanup skipped.\n", "已跳过清理。\n"));
        }
        return 1;
    }
    return 0;
}

/* FTF[信号处理] */
static void sigquit_handler(int sig) {
    (void)sig;
    host_handle_sigquit();
}

/* FTF[判断是否为 host 命令] */
static int is_host_command(const char *cmd) {
    return (strncmp(cmd, "host ", 5) == 0);
}

/* FTF[展开别名] */
static void expand_alias(char *cmd, size_t cmd_size) {
    char *first_space = strchr(cmd, ' ');
    size_t name_len;
    char alias_name[64];
    const char *alias_cmd;

    if (first_space) {
        name_len = first_space - cmd;
        if (name_len >= sizeof(alias_name)) name_len = sizeof(alias_name) - 1;
        strncpy(alias_name, cmd, name_len);
        alias_name[name_len] = '\0';
        alias_cmd = alias_get(alias_name);
        if (alias_cmd) {
            char new_cmd[INPUT_BUF_SIZE];
            safe_snprintf(new_cmd, sizeof(new_cmd), "%s %s", alias_cmd, first_space + 1);
            strncpy(cmd, new_cmd, cmd_size - 1);
            cmd[cmd_size - 1] = '\0';
        }
    } else {
        alias_cmd = alias_get(cmd);
        if (alias_cmd) {
            strncpy(cmd, alias_cmd, cmd_size - 1);
            cmd[cmd_size - 1] = '\0';
        }
    }
}

/* FTF[显示 AI 状态] */
static void ai_status_command(void) {
    LOG_DEBUG_T("Shell", "AIStatusCmd", "enter", "");
    int ai_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int ai_running = 0;
    int daemon_running = 0;

    if (ai_fd >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        safe_strncpy(addr.sun_path, AI_SOCKET_PATH, sizeof(addr.sun_path));
        addr.sun_path[sizeof(addr.sun_path)-1] = '\0';
        if (connect(ai_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            ai_running = 1;
        }
        close(ai_fd);
    }

    int daemon_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (daemon_fd >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        safe_strncpy(addr.sun_path, DAEMON_SOCKET_PATH, sizeof(addr.sun_path));
        addr.sun_path[sizeof(addr.sun_path)-1] = '\0';
        if (connect(daemon_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            daemon_running = 1;
        }
        close(daemon_fd);
    }

    uart_puts(tr("\n=== AI Service Status ===\n", "\n=== AI 服务状态 ===\n"));
    if (ai_running) {
        uart_puts(tr("AI Server: \033[32mrunning\033[0m\n", "AI 服务器: \033[32m运行中\033[0m\n"));
    } else {
        uart_puts(tr("AI Server: \033[31mstopped\033[0m (start with: python3 /LINGOS/bin/ai_server.py &)\n",
                     "AI 服务器: \033[31m已停止\033[0m（启动命令: python3 /LINGOS/bin/ai_server.py &）\n"));
    }
    if (daemon_running) {
        uart_puts(tr("Daemon (lingosd): \033[32mrunning\033[0m\n", "守护进程 (lingosd): \033[32m运行中\033[0m\n"));
    } else {
        uart_puts(tr("Daemon (lingosd): \033[31mstopped\033[0m (start with: ./lingosd &)\n",
                     "守护进程 (lingosd): \033[31m已停止\033[0m（启动命令: ./lingosd &）\n"));
    }

    /* FF[src/ai/ai_config.c]-CFN[ai_config_get]-FTF[获取AI配置] */
    const ai_config_t *cfg = ai_config_get();
    if (cfg) {
        uart_puts(tr("Backend: ", "后端: "));
        uart_puts(cfg->backend == AI_BACKEND_OLLAMA ? "Ollama\n" : "DeepSeek\n");
        uart_puts(tr("Thinking mode: ", "思考模式: "));
        uart_puts(cfg->thinking_enabled ? "enabled\n" : "disabled\n");
    }
}

/* FTF[同步日志级别到 Python] */
static void sync_log_level_to_python(const char *level_str) {
    if (!level_str || !*level_str) return;

    char sync_cmd[256];
    safe_snprintf(sync_cmd, sizeof(sync_cmd),
                  "{\"cmd\":\"set_log_level\",\"level\":\"%s\"}", level_str);

    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        LOG_WARN_T("Shell", "LogLevelSync", "SocketFail", "cannot create socket for sync");
        uart_puts(tr("Warning: Could not sync log level to Python (socket error).\n",
                     "警告：无法同步日志级别到 Python（socket 错误）。\n"));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, DAEMON_SOCKET_PATH, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_WARN_T("Shell", "LogLevelSync", "ConnectFail", "cannot connect to daemon for sync");
        uart_puts(tr("Warning: Could not sync log level to Python (daemon not reachable).\n",
                     "警告：无法同步日志级别到 Python（守护进程不可达）。\n"));
        close(sock_fd);
        return;
    }

    if (write(sock_fd, sync_cmd, strlen(sync_cmd)) < 0 || write(sock_fd, "\n", 1) < 0) {
        LOG_WARN_T("Shell", "LogLevelSync", "WriteFail", "write to daemon failed");
        uart_puts(tr("Warning: Could not sync log level to Python (write error).\n",
                     "警告：无法同步日志级别到 Python（写入错误）。\n"));
        close(sock_fd);
        return;
    }

    char resp[256];
    ssize_t n = read(sock_fd, resp, sizeof(resp)-1);
    if (n > 0) {
        resp[n] = '\0';
        const char *json_start = strstr(resp, "{\"status\"");
        if (json_start) {
            if (strstr(json_start, "\"status\":\"ok\"")) {
                uart_puts(tr("✅ Python log level synced.\n", "✅ Python 日志级别已同步。\n"));
            } else if (strstr(json_start, "\"status\":\"error\"")) {
                uart_puts(tr("⚠ Python sync failed (check log).\n", "⚠ Python 同步失败（请查看日志）。\n"));
            } else {
                uart_puts(tr("⚠ Python sync response unknown.\n", "⚠ Python 同步响应未知。\n"));
                LOG_WARN_T("Shell", "LogLevelSync", "UnknownResp", "received: %s", resp);
            }
        } else {
            uart_puts(tr("⚠ Invalid response from daemon.\n", "⚠ 守护进程响应无效。\n"));
            LOG_WARN_T("Shell", "LogLevelSync", "InvalidResp", "no JSON found: %s", resp);
        }
    } else {
        uart_puts(tr("⚠ No response from daemon for sync.\n", "⚠ 守护进程无响应。\n"));
    }
    close(sock_fd);
}

/* FTF[处理 log level 命令] */
static void log_level_dispatch(const char *args) {
    LOG_DEBUG_T("Shell", "LogLevel", "Enter", "args='%s'", args ? args : "(null)");

    if (!args || !*args) {
        int global = log_get_global_level();
        uart_puts(tr("\n=== Log Level Status ===\n", "\n=== 日志级别状态 ===\n"));
        char buf[128];
        safe_snprintf(buf, sizeof(buf), "Global level: %s\n", log_level_to_string(global));
        uart_puts(buf);
        log_dump_module_levels();
        return;
    }

    char arg_buf[256];
    safe_strncpy(arg_buf, args, sizeof(arg_buf));
    arg_buf[sizeof(arg_buf)-1] = '\0';

    if (strncmp(arg_buf, "module=", 7) == 0) {
        char *module = arg_buf + 7;
        char *level_str = strchr(module, ' ');
        if (!level_str) {
            uart_puts(tr("Usage: log level module=<module> debug|info|warn|error\n",
                         "用法：log level module=<模块> debug|info|warn|error\n"));
            return;
        }
        *level_str = '\0';
        level_str++;
        while (*level_str == ' ') level_str++;

        int level = log_level_from_string(level_str);
        if (level < 0) {
            uart_puts(tr("Invalid log level. Use: debug, info, warn, error\n",
                         "无效日志级别。使用：debug, info, warn, error\n"));
            return;
        }

        log_set_module_level(module, level);
        char buf[128];
        safe_snprintf(buf, sizeof(buf), tr("Module '%s' log level set to: %s\n",
                                            "模块 '%s' 日志级别已设置为：%s\n"),
                      module, log_level_to_string(level));
        uart_puts(buf);
        LOG_INFO_T("Shell", "LogLevel", "ModuleSet", "module='%s', level=%s", module, log_level_to_string(level));
        return;
    }

    if (strncmp(arg_buf, "reset", 5) == 0) {
        char *rest = arg_buf + 5;
        while (*rest == ' ') rest++;

        if (strncmp(rest, "module=", 7) == 0) {
            char *module = rest + 7;
            if (!module || !*module) {
                uart_puts(tr("Usage: log level reset module=<module>\n",
                             "用法：log level reset module=<模块>\n"));
                return;
            }
            if (log_reset_module_level(module) == 0) {
                char buf[128];
                safe_snprintf(buf, sizeof(buf), tr("Module '%s' reset to default (global level)\n",
                                                    "模块 '%s' 已恢复为默认（全局级别）\n"), module);
                uart_puts(buf);
                LOG_INFO_T("Shell", "LogLevel", "ModuleReset", "module='%s' reset", module);
            } else {
                uart_puts(tr("Module not found or already at default level\n",
                             "模块未找到或已是默认级别\n"));
            }
            return;
        }

        if (strcmp(rest, "all") == 0) {
            log_reset_all_modules();
            int default_level = log_get_default_level();
            log_set_global_level(default_level);
            uart_puts(tr("All modules reset to default (global level)\n",
                         "所有模块已恢复为默认（全局级别）\n"));
            LOG_INFO_T("Shell", "LogLevel", "ResetAll", "all modules reset");
            sync_log_level_to_python(log_level_to_string(default_level));
            return;
        }

        int default_level = log_get_default_level();
        log_set_global_level(default_level);
        uart_puts(tr("Global level reset to default (debug)\n",
                     "全局级别已恢复为默认（debug）\n"));
        LOG_INFO_T("Shell", "LogLevel", "ResetGlobal", "global level reset to default");
        sync_log_level_to_python(log_level_to_string(default_level));
        return;
    }

    int level = log_level_from_string(arg_buf);
    if (level < 0) {
        uart_puts(tr("Invalid log level. Use: debug, info, warn, error, reset, or module=<module> <level>\n",
                     "无效日志级别。使用：debug, info, warn, error, reset 或 module=<模块> <级别>\n"));
        return;
    }

    log_set_global_level(level);
    char buf[128];
    safe_snprintf(buf, sizeof(buf), tr("Global log level set to: %s\n",
                                        "全局日志级别已设置为：%s\n"), log_level_to_string(level));
    uart_puts(buf);
    LOG_INFO_T("Shell", "LogLevel", "GlobalSet", "level=%s", log_level_to_string(level));
    sync_log_level_to_python(log_level_to_string(level));
}

/* FTF[处理内置命令（核心）] */

/* ============================================================
 * 【2026-08-22 定稿】指令翻译层——动词子命令（git/docker 风格）
 * 形式一（领域 动作 值）：log level error —— 设置/修改类（原逻辑处理）
 * 形式二（动作 领域）：list model / list session —— 列出/查询类（本层）
 * WS 机器命令（session_list 等 JSON 字面量）保留原样；本层做终端→协议映射
 * 难适配指令（token 族/allow-high-risk/logdump 等专名）保留原样
 * ============================================================ */

/* 通过 daemon socket 发送 AI JSON 命令并显示响应（select 5s 超时，防阻塞） */
static int send_ai_command(const char *json_cmd, const char *label) {
    if (!json_cmd || !*json_cmd) return 0;
    (void)label;

    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        uart_puts(tr("Warning: socket error.\n", "警告：socket 错误。\n"));
        return 1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, DAEMON_SOCKET_PATH, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        uart_puts(tr("Warning: daemon not reachable (lingosd running?).\n",
                     "警告：守护进程不可达（lingosd 是否在运行？）。\n"));
        close(sock_fd);
        return 1;
    }
    if (write(sock_fd, json_cmd, strlen(json_cmd)) < 0 || write(sock_fd, "\n", 1) < 0) {
        uart_puts(tr("Warning: write to daemon failed.\n", "警告：写入守护进程失败。\n"));
        close(sock_fd);
        return 1;
    }

    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(sock_fd, &rfds);
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    if (select(sock_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
        char buf[4096];
        ssize_t n = read(sock_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            uart_puts(buf);
            uart_puts("\n");
        }
    } else {
        uart_puts(tr("(timeout waiting for AI response)\n", "(等待 AI 响应超时)\n"));
    }
    close(sock_fd);
    return 1;
}

/* 动词子命令翻译表：返回 1=已处理，0=不匹配交给原逻辑 */
static int handle_verb_command(const char *cmd) {
    if (!cmd || !*cmd) return 0;
    char buf[300];
    safe_strncpy(buf, cmd, sizeof(buf));
    char *sp = strchr(buf, ' ');
    if (!sp) return 0; /* 单词命令交给原逻辑 */
    *sp = '\0';
    const char *verb = buf;
    const char *rest = sp + 1;
    while (*rest == ' ') rest++;
    if (!*rest) return 0;

    char j[320];

    /* ----- 形式二：动作 领域（查询/列出） ----- */
    if (strcmp(verb, "list") == 0) {
        if (strcmp(rest, "model") == 0 || strcmp(rest, "models") == 0 ||
            strcmp(rest, "provider") == 0 || strcmp(rest, "providers") == 0)
            return send_ai_command("{\"cmd\":\"provider_list\"}", "list model");
        if (strcmp(rest, "session") == 0 || strcmp(rest, "sessions") == 0)
            return send_ai_command("{\"cmd\":\"session_list\"}", "list session");
        if (strcmp(rest, "skill") == 0 || strcmp(rest, "skills") == 0)
            return send_ai_command("{\"cmd\":\"skill_list_full\"}", "list skill");
        if (strcmp(rest, "permission") == 0 || strcmp(rest, "permissions") == 0)
            return send_ai_command("{\"cmd\":\"permission_list\"}", "list permission");
        if (strcmp(rest, "mcp") == 0)
            return send_ai_command("{\"cmd\":\"mcp_list\"}", "list mcp");
        if (strcmp(rest, "alert") == 0)
            return send_ai_command("{\"cmd\":\"alert_query\"}", "list alert");
        if (strcmp(rest, "usage") == 0 || strcmp(rest, "token") == 0)
            return send_ai_command("{\"cmd\":\"token_usage_query\"}", "list usage");
        if (strcmp(rest, "personality") == 0)
            return send_ai_command("{\"cmd\":\"personality_get\"}", "list personality");
        if (strcmp(rest, "ha") == 0 || strcmp(rest, "home") == 0)
            return send_ai_command("{\"cmd\":\"ha_status\"}", "list ha");
        if (strcmp(rest, "memory") == 0)
            return send_ai_command("{\"cmd\":\"memory_search\",\"keyword\":\"\"}", "list memory");
        return 0;
    }
    if (strcmp(verb, "view") == 0) {
        if (strcmp(rest, "model") == 0) return send_ai_command("{\"cmd\":\"provider_list\"}", "view model");
        if (strcmp(rest, "status") == 0) return send_ai_command("{\"cmd\":\"system_info\"}", "view status");
        if (strcmp(rest, "session") == 0) return send_ai_command("{\"cmd\":\"session_list\"}", "view session");
        if (strcmp(rest, "ha") == 0) return send_ai_command("{\"cmd\":\"ha_status\"}", "view ha");
        return 0;
    }
    if (strcmp(verb, "show") == 0) {
        if (strcmp(rest, "status") == 0) return send_ai_command("{\"cmd\":\"system_info\"}", "show status");
        if (strcmp(rest, "model") == 0) return send_ai_command("{\"cmd\":\"provider_list\"}", "show model");
        if (strcmp(rest, "ha") == 0) return send_ai_command("{\"cmd\":\"ha_status\"}", "show ha");
        return 0;
    }
    if (strcmp(verb, "query") == 0) {
        if (strcmp(rest, "balance") == 0) return send_ai_command("{\"cmd\":\"balance_query\"}", "query balance");
        if (strcmp(rest, "usage") == 0) return send_ai_command("{\"cmd\":\"token_usage_query\"}", "query usage");
        if (strcmp(rest, "voice") == 0) return send_ai_command("{\"cmd\":\"voice_usage_query\"}", "query voice");
        if (strcmp(rest, "ha") == 0) return send_ai_command("{\"cmd\":\"ha_status\"}", "query ha");
        return 0;
    }

    /* ----- 形式一变体：领域 动作（设置/操作类子命令） ----- */
    if (strcmp(verb, "model") == 0) {
        if (strcmp(rest, "list") == 0) return send_ai_command("{\"cmd\":\"provider_list\"}", "model list");
        if (strncmp(rest, "switch ", 7) == 0) {
            safe_snprintf(j, sizeof(j), "{\"cmd\":\"model_switch\",\"model_id\":\"%s\"}", rest + 7);
            return send_ai_command(j, "model switch");
        }
        return 0;
    }
    if (strcmp(verb, "session") == 0) {
        if (strcmp(rest, "list") == 0) return send_ai_command("{\"cmd\":\"session_list\"}", "session list");
        if (strncmp(rest, "create ", 7) == 0) {
            safe_snprintf(j, sizeof(j), "{\"cmd\":\"session_create\",\"title\":\"%s\"}", rest + 7);
            return send_ai_command(j, "session create");
        }
        if (strncmp(rest, "delete ", 7) == 0) {
            safe_snprintf(j, sizeof(j), "{\"cmd\":\"session_delete\",\"sid\":\"%s\"}", rest + 7);
            return send_ai_command(j, "session delete");
        }
        if (strncmp(rest, "history ", 8) == 0) {
            safe_snprintf(j, sizeof(j), "{\"cmd\":\"session_history\",\"sid\":\"%s\",\"limit\":50}", rest + 8);
            return send_ai_command(j, "session history");
        }
        return 0;
    }
    if (strcmp(verb, "skill") == 0) {
        if (strcmp(rest, "list") == 0) return send_ai_command("{\"cmd\":\"skill_list_full\"}", "skill list");
        if (strncmp(rest, "enable ", 7) == 0) {
            safe_snprintf(j, sizeof(j), "{\"cmd\":\"skill_enable\",\"name\":\"%s\",\"enabled\":true}", rest + 7);
            return send_ai_command(j, "skill enable");
        }
        if (strncmp(rest, "disable ", 8) == 0) {
            safe_snprintf(j, sizeof(j), "{\"cmd\":\"skill_enable\",\"name\":\"%s\",\"enabled\":false}", rest + 8);
            return send_ai_command(j, "skill disable");
        }
        return 0;
    }
    if (strcmp(verb, "memory") == 0) {
        if (strncmp(rest, "search ", 7) == 0) {
            safe_snprintf(j, sizeof(j), "{\"cmd\":\"memory_search\",\"keyword\":\"%s\"}", rest + 7);
            return send_ai_command(j, "memory search");
        }
        if (strncmp(rest, "write ", 6) == 0) {
            safe_snprintf(j, sizeof(j), "{\"cmd\":\"memory_write\",\"content\":\"%s\"}", rest + 6);
            return send_ai_command(j, "memory write");
        }
        return 0;
    }
    if (strcmp(verb, "voice") == 0) {
        if (strcmp(rest, "usage") == 0 || strcmp(rest, "query") == 0)
            return send_ai_command("{\"cmd\":\"voice_usage_query\"}", "voice usage");
        return 0;
    }
    if (strcmp(verb, "ha") == 0) {
        if (strcmp(rest, "status") == 0) return send_ai_command("{\"cmd\":\"ha_status\"}", "ha status");
        if (strcmp(rest, "states") == 0) return send_ai_command("{\"cmd\":\"ha_states\"}", "ha states");
        return 0;
    }
    if (strcmp(verb, "provider") == 0) {
        if (strcmp(rest, "list") == 0) return send_ai_command("{\"cmd\":\"provider_list\"}", "provider list");
        return 0;
    }
    if (strcmp(verb, "config") == 0) {
        if (strcmp(rest, "advanced") == 0) {
            uart_puts(tr("Advanced config is managed via dedicated commands (e.g. log level, model switch).\n",
                         "高级配置通过特定指令管理（如 log level、model switch）。\n"));
            return 1;
        }
        return 0;
    }
    return 0;
}

static int handle_builtin_command(const char *cmd) {
    LOG_DEBUG_T("Shell", "BuiltinCmd", "Enter", "cmd='%s'", cmd ? cmd : "(null)");
    if (!cmd || !*cmd) return 0;

    /* ----- 【2026-08-22 定稿】动词子命令翻译层（list/view/show/query/model/session/...） ----- */
    if (handle_verb_command(cmd)) return 1;

    /* ----- help ----- */
    if (strncmp(cmd, "help", 4) == 0) {
        const char *topic = cmd + 4;
        while (*topic == ' ') topic++;
        show_help_topic(topic);
        return 1;
    }

    /* ----- token 命令族（先生设计） ----- */
    if (strcmp(cmd, "token") == 0) {
        handle_token_command(" ");
        return 1;
    }
    if (strncmp(cmd, "token ", 6) == 0) {
        handle_token_command(cmd + 6);
        return 1;
    }
    if (strcmp(cmd, "clear") == 0) {
        uart_puts("\033[2J\033[H");
        return 1;
    }

    /* ----- reboot/poweroff ----- */
    if (strcmp(cmd, "reboot") == 0) {
        uart_puts(tr(COLOR_YELLOW "System rebooting...\n" COLOR_RESET,
                     COLOR_YELLOW "系统重启中...\n" COLOR_RESET));
        audit_log("shell", "reboot", "", "", "", 0, "high", 1);
        sync();
        system("reboot");
        return 1;
    }
    if (strcmp(cmd, "poweroff") == 0) {
        uart_puts(tr(COLOR_YELLOW "System powering off...\n" COLOR_RESET,
                     COLOR_YELLOW "系统关机中...\n" COLOR_RESET));
        audit_log("shell", "poweroff", "", "", "", 0, "high", 1);
        sync();
        system("poweroff");
        return 1;
    }

    /* ----- logdump ----- */
    if (strcmp(cmd, "logdump") == 0) {
        log_dump_all();
        return 1;
    }

    /* ----- audit ----- */
    if (strcmp(cmd, "audit") == 0) {
        char buf[8192];
        audit_dump(buf, sizeof(buf));
        log_draw_box(tr("Audit Log", "审计日志"), buf, COLOR_CYAN, COLOR_DIM, COLOR_WHITE);
        return 1;
    }

    /* ============================================================
     * system configuration（包含 --force 支持）
     * ============================================================ */
    if (strncmp(cmd, "system configuration", 20) == 0) {
        const char *args = cmd + 20;
        while (*args == ' ') args++;

        /* ---- 解析 --force 参数 ---- */
        int force_override = 0;
        char clean_args[256];
        safe_strncpy(clean_args, args, sizeof(clean_args));
        char *force_pos = strstr(clean_args, "--force");
        if (force_pos) {
            force_override = 1;
            /* 移除 --force 及其前后的空格 */
            char *end = force_pos + strlen("--force");
            while (*end == ' ') end++;
            memmove(force_pos, end, strlen(end) + 1);
            /* 去除多余空格 */
            char *trim = clean_args;
            while (*trim == ' ') trim++;
            memmove(clean_args, trim, strlen(trim) + 1);
        }
        const char *effective_args = clean_args;

        /* ---- 渲染器类型判断 ---- */
        int renderer_type = RENDERER_TYPE_TUI;
        if (strcmp(effective_args, "--cli") == 0) renderer_type = RENDERER_TYPE_CLI;
        else if (strcmp(effective_args, "--raw") == 0) renderer_type = RENDERER_TYPE_RAW;
        else if (strcmp(effective_args, "--auto") == 0) renderer_type = RENDERER_TYPE_CLI;
        else if (strcmp(effective_args, "--quick") == 0 || strcmp(effective_args, "") == 0) renderer_type = RENDERER_TYPE_TUI;
        else renderer_type = RENDERER_TYPE_TUI;

        /* 【先生要求】向导模式：默认快速（必要项）——--full 完整模式（所有可配置） */
        int wizard_full_mode = (strcmp(effective_args, "--full") == 0);

        /* ---- 初始化引擎 ---- */
        wizard_engine_ctx_t ctx;
        renderer_ctx_t renderer;

        if (wizard_engine_init(&ctx, renderer_type) != 0) {
            uart_puts(tr("Failed to initialize configuration wizard.\n",
                         "初始化配置向导失败。\n"));
            return 1;
        }

        /* 【先生要求】设置模式（--full=完整——默认快速） */
        wizard_engine_set_mode(&ctx, wizard_full_mode ? 1 : 0);

        if (wizard_engine_load_steps(&ctx) != 0) {
            uart_puts(tr("Failed to load configuration steps.\n",
                         "加载配置步骤失败。\n"));
            free(ctx.steps);
            free(ctx.stack);
            return 1;
        }

        /* ---- 创建渲染器（带降级） ---- */
        int renderer_ret = -1;
        switch (renderer_type) {
            case RENDERER_TYPE_TUI:
                renderer_ret = renderer_tui_create(&renderer);
                if (renderer_ret != 0) {
                    uart_puts(tr("TUI not available, falling back to CLI.\n",
                                 "TUI 不可用，降级到 CLI。\n"));
                    renderer_type = RENDERER_TYPE_CLI;
                    renderer_ret = renderer_cli_create(&renderer);
                }
                break;
            case RENDERER_TYPE_CLI:
                renderer_ret = renderer_cli_create(&renderer);
                if (renderer_ret != 0) {
                    uart_puts(tr("CLI not available, falling back to RAW.\n",
                                 "CLI 不可用，降级到 RAW。\n"));
                    renderer_type = RENDERER_TYPE_RAW;
                    renderer_ret = renderer_raw_create(&renderer);
                }
                break;
            case RENDERER_TYPE_RAW:
                renderer_ret = renderer_raw_create(&renderer);
                break;
            default:
                renderer_ret = renderer_tui_create(&renderer);
                break;
        }

        if (renderer_ret != 0) {
            uart_puts(tr("Failed to create renderer.\n", "创建渲染器失败。\n"));
            free(ctx.steps);
            free(ctx.stack);
            return 1;
        }

        ctx.renderer = &renderer;

        /* ---- 运行向导 ---- */
        if (wizard_engine_run(&ctx) != 0) {
            uart_puts(tr("Failed to start wizard.\n", "启动向导失败。\n"));
            renderer_destroy(&renderer);
            free(ctx.steps);
            free(ctx.stack);
            return 1;
        }

        /* ---- 事件循环 ---- */
        char input_buf[256];
        int wizard_result = 0;

        while (1) {
            wizard_step_def_t *step = wizard_engine_current_step(&ctx);
            if (!step) {
                wizard_result = 1;
                break;
            }

            renderer.render_header(&renderer, step, ctx.current_index + 1, ctx.step_count);

            if (step->type == STEP_TYPE_SELECT) {
                wizard_option_t opts[16];
                int count = wizard_engine_get_options(&ctx, opts, 16);
                renderer.render_options(&renderer, opts, count, 0);
            } else if (step->type == STEP_TYPE_INPUT) {
                const char *prompt = wizard_engine_get_input_prompt(&ctx);
                renderer.render_input(&renderer, prompt, input_buf, sizeof(input_buf));
            }

            memset(input_buf, 0, sizeof(input_buf));
            int get_ret = renderer.get_input(&renderer, input_buf, sizeof(input_buf));
            if (get_ret == -3) {
                uart_puts(tr("\n⚠ Auto-downgrading to next mode...\n",
                             "\n⚠ 自动降级到下一模式...\n"));
                renderer_destroy(&renderer);
                int next_type = renderer_type_next((renderer_type_t)renderer_type);
                if (next_type >= 0) {
                    renderer_type = next_type;
                    renderer_create((renderer_type_t)renderer_type, &renderer);
                    ctx.renderer = &renderer;
                    continue;
                }
                break;
            } else if (get_ret == -2) {
                uart_puts(tr("\n⚠ Render anomaly detected. Use ^L/^R/^Q to switch.\n",
                             "\n⚠ 检测到渲染异常。使用 ^L/^R/^Q 切换。\n"));
                continue;
            } else if (get_ret == -1) {
                ctx.cancelled = 1;
                break;
            } else if (get_ret == 2) {
                uart_puts(tr("\n⚠ Switching to CLI mode...\n", "\n⚠ 切换到 CLI 模式...\n"));
                renderer_destroy(&renderer);
                renderer_type = RENDERER_TYPE_CLI;
                if (renderer_create(RENDERER_TYPE_CLI, &renderer) == 0) {
                    ctx.renderer = &renderer;
                    continue;
                }
                break;
            } else if (get_ret == 3) {
                uart_puts(tr("\n⚠ Switching to RAW mode...\n", "\n⚠ 切换到 RAW 模式...\n"));
                renderer_destroy(&renderer);
                renderer_type = RENDERER_TYPE_RAW;
                if (renderer_create(RENDERER_TYPE_RAW, &renderer) == 0) {
                    ctx.renderer = &renderer;
                    continue;
                }
                break;
            } else if (get_ret == 4) {
                ctx.cancelled = 1;
                break;
            }

            int handle_ret = wizard_engine_handle_input(&ctx, input_buf);
            if (handle_ret == 1) {
                wizard_result = 1;
                break;
            } else if (handle_ret == -1) {
                ctx.cancelled = 1;
                break;
            }
        }

        /* ---- 保存配置（根据 force_override 决定） ---- */
        if (ctx.cancelled) {
            renderer.render_message(&renderer, tr("Wizard cancelled.\n", "向导已取消。\n"), 1);
        } else if (wizard_result == 1) {
            int save_ret;
            if (force_override) {
                /* FF[src/config/config_core.c]-CFN[config_core_save_force]-FTF[强制保存配置，无视文件存在性] */
                save_ret = config_core_save_force(&ctx.config);
            } else {
                save_ret = wizard_engine_save_config(&ctx);
            }
            if (save_ret == 0) {
                renderer.render_complete(&renderer, 1);
            } else {
                renderer.render_complete(&renderer, 0);
            }
        }

        renderer_destroy(&renderer);
        free(ctx.steps);
        free(ctx.stack);

        uart_puts(tr("\nConfiguration wizard finished.\n", "\n配置向导完成。\n"));
        return 1;
    }

    /* ----- system startup ----- */
    if (strncmp(cmd, "system startup", 14) == 0) {
        const char *args = cmd + 14;
        while (*args == ' ') args++;
        handle_startup_command(args);
        return 1;
    }

    /* ----- desktop ----- */
    if (strcmp(cmd, "desktop") == 0) {
        handle_desktop_command();
        return 1;
    }

    /* ----- config reload ----- */
    if (strncmp(cmd, "config reload", 13) == 0) {
        const char *args = cmd + 13;
        while (*args == ' ') args++;
        if (*args) {
            handle_config_reload(args);
        } else {
            uart_puts(tr("Usage: config reload <category>\n", "用法：config reload <分类>\n"));
        }
        return 1;
    }

    /* ----- config get/set/list ----- */
    if (strncmp(cmd, "config ", 7) == 0) {
        const char *args = cmd + 7;
        while (*args == ' ') args++;
        config_dispatch(args);
        return 1;
    }
    if (strcmp(cmd, "config") == 0) {
        config_dispatch("");
        return 1;
    }

    /* ----- system health ----- */
    if (strcmp(cmd, "system health") == 0) {
        system_health_command();
        return 1;
    }

    /* ----- system status ----- */
    if (strcmp(cmd, "system status") == 0) {
        component_show_status();
        const char *mode = lingos_mode_name(lingos_get_mode());
        char buf[256];
        safe_snprintf(buf, sizeof(buf), "%s: %s\n%s: %s\n",
                      tr("System mode", "系统模式"), mode,
                      tr("Version", "版本"), version_get());
        log_draw_box(tr("System Status", "系统状态"), buf, COLOR_CYAN, COLOR_DIM, COLOR_WHITE);
        return 1;
    }

    /* ----- system update ----- */
    if (strncmp(cmd, "system update ", 14) == 0) {
        const char *path = cmd + 14;
        while (*path == ' ') path++;
        if (*path) system_update_install(path);
        else uart_puts(tr("Usage: system update <path>\n", "用法：system update <路径>\n"));
        return 1;
    }

    /* ----- system rollback ----- */
    if (strncmp(cmd, "system rollback", 15) == 0) {
        const char *arg = cmd + 15;
        while (*arg == ' ') arg++;
        system_rollback_command(arg);
        return 1;
    }

    /* ----- system debug update / info ----- */
    if (strncmp(cmd, "system debug update", 19) == 0) {
        system_debug_update();
        return 1;
    }
    if (strcmp(cmd, "system debug info") == 0) {
        system_debug_info_command();
        return 1;
    }

    /* ----- system defense ----- */
    if (strncmp(cmd, "system defense", 14) == 0) {
        const char *args = cmd + 14;
        while (*args == ' ') args++;
        defense_dispatch(args);
        return 1;
    }

    /* ----- system privilege ----- */
    if (strncmp(cmd, "system privilege", 16) == 0) {
        const char *args = cmd + 16;
        while (*args == ' ') args++;
        privilege_dispatch(args);
        return 1;
    }

    /* ----- system security ----- */
    if (strncmp(cmd, "system security", 15) == 0) {
        const char *args = cmd + 15;
        while (*args == ' ') args++;
        security_dispatch(args);
        return 1;
    }

    /* ----- system behavior ----- */
    if (strncmp(cmd, "system behavior", 15) == 0) {
        const char *args = cmd + 15;
        while (*args == ' ') args++;
        behavior_dispatch(args);
        return 1;
    }

    /* ----- registry ----- */
    if (strncmp(cmd, "registry", 8) == 0) {
        const char *args = cmd + 8;
        while (*args == ' ') args++;
        registry_dispatch(args);
        return 1;
    }

    /* ----- log level ----- */
    if (strncmp(cmd, "log level", 9) == 0) {
        const char *args = cmd + 9;
        while (*args == ' ') args++;
        log_level_dispatch(args);
        return 1;
    }

    /* ----- debug ----- */
    if (strncmp(cmd, "debug", 5) == 0) {
        const char *args = cmd + 5;
        debug_command(args);
        return 1;
    }

    /* ----- health trend ----- */
    if (strcmp(cmd, "health trend") == 0) {
        char trend_buf[4096];
        if (health_trend_analyze(trend_buf, sizeof(trend_buf)) == 0) {
            log_draw_box(tr("Health Trend", "健康趋势"), trend_buf, COLOR_CYAN, COLOR_DIM, COLOR_WHITE);
        } else {
            uart_puts(tr("Failed to analyze health trend.\n", "分析健康趋势失败。\n"));
        }
        return 1;
    }

    /* ----- health watchdog ----- */
    if (strncmp(cmd, "health watchdog ", 16) == 0) {
        const char *sub = cmd + 16;
        while (*sub == ' ') sub++;
        if (strcmp(sub, "on") == 0) health_watchdog_start();
        else if (strcmp(sub, "off") == 0) health_watchdog_stop();
        else if (strcmp(sub, "status") == 0) {
            char buf[128];
            safe_snprintf(buf, sizeof(buf), "%s: %s, %s: %d %s\n",
                          tr("Health watchdog status", "健康看门狗状态"),
                          health_watchdog_is_running() ? tr("running", "运行中") : tr("stopped", "已停止"),
                          tr("Interval", "间隔"),
                          health_watchdog_get_interval(),
                          tr("seconds", "秒"));
            log_draw_box(tr("Watchdog", "看门狗"), buf, COLOR_YELLOW, COLOR_DIM, COLOR_WHITE);
        } else uart_puts(tr("Usage: health watchdog on|off|status\n", "用法：health watchdog on|off|status\n"));
        return 1;
    }

    /* ----- test ----- */
    if (strncmp(cmd, "test list", 9) == 0) {
        test_list();
        return 1;
    }
    if (strncmp(cmd, "test run ", 9) == 0) {
        const char *range = cmd + 9;
        while (*range == ' ') range++;
        if (*range) {
            int total, passed;
            int fails = test_run_range(range, &total, &passed);
            char buf[64];
            safe_snprintf(buf, sizeof(buf), "%s: %d, %s: %d, %s: %d\n",
                          tr("Passed", "通过"), passed,
                          tr("Failed", "失败"), fails,
                          tr("Total", "总计"), total);
            log_draw_box(tr("Test Results", "测试结果"), buf, COLOR_GREEN, COLOR_DIM, COLOR_WHITE);
        } else uart_puts(tr("Usage: test run <range>\n", "用法：test run <范围>\n"));
        return 1;
    }

    /* ----- snapshot ----- */
    if (strncmp(cmd, "snapshot ", 9) == 0) {
        extern void snapshot_dispatch(const char *args);
        const char *args = cmd + 9;
        while (*args == ' ') args++;
        snapshot_dispatch(args);
        return 1;
    }

    /* ----- remind ----- */
    if (strncmp(cmd, "remind ", 7) == 0) {
        extern void reminder_dispatch(const char *args);
        const char *args = cmd + 7;
        while (*args == ' ') args++;
        reminder_dispatch(args);
        return 1;
    }

    /* ----- plugin ----- */
    if (strncmp(cmd, "plugin", 6) == 0) {
        extern void plugin_dispatch(const char *args);
        const char *args = cmd + 6;
        while (*args == ' ') args++;
        plugin_dispatch(args);
        return 1;
    }

    /* ----- scan ----- */
    if (strncmp(cmd, "scan ", 5) == 0) {
        const char *sub = cmd + 5;
        while (*sub == ' ') sub++;
        if (strncmp(sub, "frequency set ", 14) == 0) {
            int freq = atoi(sub + 14);
            if (freq >= 10) {
                scan_set_interval(freq);
                char buf[64];
                safe_snprintf(buf, sizeof(buf), "%s %d %s\n",
                              tr("Scan frequency set to", "扫描频率已设置为"), freq, tr("seconds", "秒"));
                uart_puts(buf);
            } else uart_puts(tr("Frequency must be at least 10 seconds.\n", "频率至少为10秒。\n"));
        } else if (strcmp(sub, "frequency show") == 0) {
            int freq = scan_get_interval();
            char buf[64];
            safe_snprintf(buf, sizeof(buf), "%s: %d %s\n",
                          tr("Current scan frequency", "当前扫描频率"), freq, tr("seconds", "秒"));
            uart_puts(buf);
        } else if (strcmp(sub, "status") == 0) {
            uart_puts(tr("Scan daemon status: ", "扫描守护状态: "));
            uart_puts(scan_daemon_is_running() ? tr("running\n", "运行中\n") : tr("stopped\n", "已停止\n"));
            uint64_t last = scan_get_last_completed();
            if (last) {
                char timebuf[64];
                struct tm *tm = localtime((time_t*)&last);
                strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);
                uart_puts(tr("Last scan completed: ", "上次扫描完成: "));
                uart_puts(timebuf);
                uart_puts("\n");
            }
            const scan_result_t *res = scan_get_last_result();
            if (res && res->total_skills > 0) {
                char sum[256];
                safe_snprintf(sum, sizeof(sum), "%s: %d, %s: %d, %s: %d, %s: %d\n",
                              tr("Total skills", "技能总数"), res->total_skills,
                              tr("High risk", "高风险"), res->high_risk_count,
                              tr("Medium risk", "中风险"), res->medium_risk_count,
                              tr("Low risk", "低风险"), res->low_risk_count);
                uart_puts(sum);
            }
        } else if (strcmp(sub, "pause") == 0) {
            scan_set_enabled(0);
            uart_puts(tr("Background scan paused.\n", "后台扫描已暂停。\n"));
        } else if (strcmp(sub, "resume") == 0) {
            scan_set_enabled(1);
            uart_puts(tr("Background scan resumed.\n", "后台扫描已恢复。\n"));
        } else {
            uart_puts(tr("scan: frequency set <seconds> | frequency show | status | pause | resume\n",
                         "scan: frequency set <秒数> | frequency show | status | pause | resume\n"));
        }
        return 1;
    }

    /* ----- ai config / status ----- */
    if (strncmp(cmd, "ai ", 3) == 0) {
        const char *sub = cmd + 3;
        while (*sub == ' ') sub++;
        if (strcmp(sub, "config") == 0) {
            ai_config_interactive();
        } else if (strcmp(sub, "status") == 0) {
            ai_status_command();
        } else {
            uart_puts(tr("ai: config | status\n", "ai: config | status\n"));
        }
        return 1;
    }

    /* ----- alias ----- */
    if (strncmp(cmd, "alias", 5) == 0) {
        char *arg = (char*)cmd + 5;
        while (*arg == ' ') arg++;
        if (*arg == '\0') {
            alias_list();
        } else {
            char *eq = strchr(arg, '=');
            if (eq) {
                *eq = '\0';
                alias_set(arg, eq + 1);
                uart_puts(tr("Alias set.\n", "别名已设置。\n"));
            } else {
                const char *val = alias_get(arg);
                if (val) {
                    char buf[256];
                    safe_snprintf(buf, sizeof(buf), "alias %s='%s'\n", arg, val);
                    uart_puts(buf);
                } else {
                    uart_puts(tr("Alias not found.\n", "别名未找到。\n"));
                }
            }
        }
        return 1;
    }
    if (strncmp(cmd, "unalias ", 8) == 0) {
        char *arg = (char*)cmd + 8;
        while (*arg == ' ') arg++;
        if (*arg) {
            alias_unset(arg);
            uart_puts(tr("Alias removed.\n", "别名已移除。\n"));
        } else {
            uart_puts(tr("Usage: unalias <name>\n", "用法：unalias <名称>\n"));
        }
        return 1;
    }

    /* ----- Nook 命令 ----- */
    if (strncmp(cmd, "nook ", 5) == 0) {
        const char *sub = cmd + 5;
        while (*sub == ' ') sub++;

        if (strcmp(sub, "chat") == 0) {
            chat_terminal_run();
            return 1;
        }

        if (strncmp(sub, "ask ", 4) == 0) {
            const char *msg = sub + 4;
            while (*msg == ' ') msg++;
            int timeout = 60;
            if (strncmp(msg, "-t ", 3) == 0) {
                msg += 3;
                timeout = atoi(msg);
                while (*msg && *msg != ' ') msg++;
                while (*msg == ' ') msg++;
            } else if (strncmp(msg, "-y ", 3) == 0) {
                timeout = 0;
                msg += 3;
                while (*msg == ' ') msg++;
            }
            if (*msg) {
                char response[393216];
                uart_puts(tr(COLOR_DIM "Nook is thinking...\n" COLOR_RESET,
                             COLOR_DIM "Nook 思考中...\n" COLOR_RESET));
                /* 【批次F】流式对话：过程事件 + 最终回复逐块实时显示 */
                int ret = nook_ask_stream(msg, NULL, response, sizeof(response), timeout);
                if (ret == 0) {
                    /* 内容已流式实时显示；仅当无输出时绘制盒兜底 */
                    if (response[0] == '\0') {
                        log_draw_box("Nook", tr("(empty response)", "(空响应)"),
                                     COLOR_CYAN, COLOR_DIM, COLOR_WHITE);
                    }
                } else if (ret == -2) {
                    uart_puts(tr(COLOR_RED "Nook request timed out.\n" COLOR_RESET,
                                 COLOR_RED "Nook 请求超时。\n" COLOR_RESET));
                } else {
                    uart_puts(tr(COLOR_RED "Nook AI service unavailable.\n" COLOR_RESET,
                                 COLOR_RED "Nook AI 服务不可用。\n" COLOR_RESET));
                }
            }
            return 1;
        }

        if (strncmp(sub, "repair ", 7) == 0) {
            const char *rsub = sub + 7;
            while (*rsub == ' ') rsub++;
            if (strncmp(rsub, "start ", 6) == 0) {
                const char *desc = rsub + 6;
                while (*desc == ' ') desc++;
                if (*desc) nook_repair_start(desc, REPAIR_ERR_MEDIUM, 0);
            } else if (strcmp(rsub, "status") == 0) {
                const char *desc = nook_repair_get_status_desc();
                uart_puts(desc);
                uart_puts("\n");
            } else if (strncmp(rsub, "history", 7) == 0) {
                const char *id = rsub + 7;
                while (*id == ' ') id++;
                char result[4096];
                nook_repair_history(id, result, sizeof(result));
                log_draw_box(tr("Repair History", "修复详情"), result, COLOR_CYAN, COLOR_DIM, COLOR_WHITE);
            } else {
                uart_puts(tr("nook repair: start|status|history [id]\n",
                             "nook repair: start|status|history [id]\n"));
            }
            return 1;
        }

        if (strncmp(sub, "defense ", 8) == 0) {
            const char *dsub = sub + 8;
            while (*dsub == ' ') dsub++;
            if (strcmp(dsub, "status") == 0) {
                defense_show_status();
            } else if (strncmp(dsub, "shadow ", 7) == 0) {
                const char *val = dsub + 7;
                while (*val == ' ') val++;
                if (strcmp(val, "on") == 0) defense_shadow_mode(1);
                else if (strcmp(val, "off") == 0) defense_shadow_mode(0);
            } else if (strncmp(dsub, "dark ", 5) == 0) {
                const char *val = dsub + 5;
                while (*val == ' ') val++;
                if (strcmp(val, "on") == 0) defense_dark_mode(1);
                else if (strcmp(val, "off") == 0) defense_dark_mode(0);
            } else if (strcmp(dsub, "absolute") == 0) {
                defense_absolute_protect();
            } else if (strncmp(dsub, "anomaly_threshold ", 18) == 0) {
                int th = atoi(dsub + 18);
                if (th < 0) th = 0;
                if (th > 100) th = 100;
                defense_set_anomaly_threshold(th);
                char buf[8];
                int_to_str(th, buf);
                uart_puts(tr("Anomaly threshold set to ", "异常阈值设置为 "));
                uart_puts(buf);
                uart_puts("\n");
            } else if (strcmp(dsub, "anomaly_threshold") == 0) {
                int th = defense_get_anomaly_threshold();
                char buf[8];
                int_to_str(th, buf);
                uart_puts(tr("Current anomaly threshold: ", "当前异常阈值: "));
                uart_puts(buf);
                uart_puts("\n");
            }
            return 1;
        }

        if (strncmp(sub, "callme ", 7) == 0) {
            const char *name = sub + 7;
            while (*name == ' ') name++;
            if (*name) nook_set_user_name(name);
            return 1;
        }

        if (strcmp(sub, "status") == 0) {
            nook_show_status();
            return 1;
        }

        if (strncmp(sub, "idle ", 5) == 0) {
            const char *isub = sub + 5;
            if (strcmp(isub, "start") == 0) nook_idle_set_enabled(1);
            else if (strcmp(isub, "stop") == 0) nook_idle_set_enabled(0);
            else if (strcmp(isub, "status") == 0) nook_idle_show_status();
            return 1;
        }

        if (strcmp(sub, "clear") == 0) {
            ipc_send("{\"type\":\"clear\"}", 16);
            uart_puts(tr("Nook context cleared.\n", "Nook 上下文已清除。\n"));
            return 1;
        }

        if (strncmp(sub, "model ", 6) == 0) {
            const char *model_sub = sub + 6;
            while (*model_sub == ' ') model_sub++;
            if (strncmp(model_sub, "set ", 4) == 0) {
                const char *model_name = model_sub + 4;
                while (*model_name == ' ') model_name++;
                if (*model_name) {
                    ai_config_set_ollama_model(model_name);
                    uart_puts(tr("Model set to: ", "模型已设置为: "));
                    uart_puts(model_name);
                    uart_puts("\n");
                } else uart_puts(tr("Usage: nook model set <model>\n", "用法：nook model set <模型名>\n"));
            } else if (strcmp(model_sub, "show") == 0) {
                const char *model = ai_config_get_current_model();
                uart_puts(tr("Current model: ", "当前模型: "));
                uart_puts(model);
                uart_puts("\n");
            } else uart_puts(tr("nook model: set <name> | show\n", "nook model: set <名称> | show\n"));
            return 1;
        }

        if (strcmp(sub, "allow-high-risk") == 0) {
            char resp_buf[256];
            const char *json_cmd = "{\"cmd\":\"set_auto_allow\",\"value\":true,\"persist\":false}\n";
            int ret = send_to_ai_server(json_cmd, resp_buf, sizeof(resp_buf));
            if (ret == 0) {
                uart_puts(tr("High-risk auto-authorization enabled (critical still requires confirmation)\n",
                             "高风险操作自动授权已启用（极高风险仍需要确认）\n"));
            } else {
                uart_puts(tr("Failed to enable: ", "启用失败: "));
                uart_puts(resp_buf);
                uart_puts("\n");
            }
            return 1;
        }
        if (strcmp(sub, "disallow-high-risk") == 0) {
            char resp_buf[256];
            const char *json_cmd = "{\"cmd\":\"set_auto_allow\",\"value\":false,\"persist\":false}\n";
            int ret = send_to_ai_server(json_cmd, resp_buf, sizeof(resp_buf));
            if (ret == 0) {
                uart_puts(tr("High-risk auto-authorization disabled\n",
                             "高风险操作自动授权已禁用\n"));
            } else {
                uart_puts(tr("Failed to disable: ", "禁用失败: "));
                uart_puts(resp_buf);
                uart_puts("\n");
            }
            return 1;
        }

        uart_puts(tr("nook: ask|chat|repair|defense|callme|status|idle|clear|model set|model show|allow-high-risk|disallow-high-risk\n",
                     "nook: ask|chat|repair|defense|callme|status|idle|clear|model set|model show|allow-high-risk|disallow-high-risk\n"));
        return 1;
    }

    /* ----- sub_ai ----- */
    if (strncmp(cmd, "sub_ai ", 7) == 0) {
        const char *args = cmd + 7;
        while (*args == ' ') args++;
        handle_subai_command(args);
        return 1;
    }

    /* ----- skill（本地技能商店） ----- */
    if (strncmp(cmd, "skill ", 6) == 0 || strcmp(cmd, "skill") == 0) {
        const char *sub = cmd + 6;
        while (*sub == ' ') sub++;
        if (strncmp(sub, "install ", 8) == 0) {
            skill_store_install(sub + 8);
        } else if (strncmp(sub, "enable ", 7) == 0) {
            skill_store_enable(sub + 7);
        } else if (strncmp(sub, "disable ", 8) == 0) {
            skill_store_disable(sub + 8);
        } else if (strncmp(sub, "uninstall ", 10) == 0) {
            skill_store_uninstall(sub + 10);
        } else if (strncmp(sub, "search ", 7) == 0) {
            skill_store_list(sub + 7);
        } else if (strcmp(sub, "list") == 0 || *sub == '\0') {
            skill_store_list(NULL);
        } else if (strncmp(sub, "create ", 7) == 0) {
            char cmdline[512];
            safe_snprintf(cmdline, sizeof(cmdline), "python3 %s/skill_dev.py create %s",
                          lingos_data_root(), sub + 7);
            (void)system(cmdline);
        } else if (strncmp(sub, "test ", 5) == 0) {
            char cmdline[640];
            safe_snprintf(cmdline, sizeof(cmdline), "python3 %s/skill_dev.py test %s",
                          lingos_data_root(), sub + 5);
            (void)system(cmdline);
        } else {
            uart_puts(tr("skill: install <name> | enable <name> | disable <name> | uninstall <name> | list | search <kw>\n",
                         "skill: install <名称> | enable <名称> | disable <名称> | uninstall <名称> | list | search <关键词>\n"));
        }
        return 1;
    }

    /* ----- agent（子 AI 对话协作状态） ----- */
    if (strcmp(cmd, "agent") == 0 || strncmp(cmd, "agent ", 6) == 0) {
        const char *args = cmd + 6;
        while (*args == ' ') args++;
        handle_agent_command(args);
        return 1;
    }

    /* ----- app 管理 ----- */
    if (strncmp(cmd, "app ", 4) == 0) {
        app_dispatch(cmd + 4);
        return 1;
    }

    /* ----- dashboard / status ----- */
    if (strcmp(cmd, "dashboard") == 0 || strcmp(cmd, "status") == 0) {
        char dash_buf[1024];
        int ai_ok = ai_status_query();
        int task_cnt = get_background_task_count();
        const char *mode = lingos_mode_name(lingos_get_mode());
        safe_snprintf(dash_buf, sizeof(dash_buf),
                      "%s: %s\n%s: %s\n%s: %s\n%s: %d\n",
                      tr("Version", "版本"), version_get(),
                      tr("Mode", "模式"), mode,
                      tr("AI Status", "AI 状态"), ai_ok ? tr("● Running", "● 运行中") : tr("○ Stopped", "○ 已停止"),
                      tr("Background tasks", "后台任务"), task_cnt);
        log_draw_box(tr("Dashboard", "仪表板"), dash_buf, COLOR_CYAN, COLOR_DIM, COLOR_WHITE);
        return 1;
    }

    return 0;
}

/* ============================================================
 * FTF[Shell 主循环（含增强提示符）]
 * ============================================================ */
void shell_run(void) {
    /* 【批次C】初始化本地技能商店（确保市场/启用目录存在） */
    skill_store_init();

    LOG_INFO_T("Shell", "Run", "Enter", "Starting shell main loop");
    signal(SIGQUIT, sigquit_handler);
    lingos_is_interactive = 1;

    history_init();
    alias_load();

    check_disk_space();

    uart_puts("\033[2J\033[H");
    uart_puts(COLOR_BOLD COLOR_CYAN);
    uart_puts("┌────────────────────────────────────────────────────────────┐\n");
    uart_puts("│  LING OS v");
    uart_puts(version_get());
    uart_puts("                    │\n");
    uart_puts("│  Welcome to LING OS                                     │\n");
    uart_puts("└────────────────────────────────────────────────────────────┘\n");
    uart_puts(COLOR_RESET);
    uart_puts("\n");
    uart_puts(tr("Type 'help' to see available commands\n",
                 "输入 'help' 查看可用命令\n"));
    uart_puts("\n");

    char cmd[INPUT_BUF_SIZE];
    int idx = 0;
    char prompt[128];
    int last_status_bar_update = 0;
    time_t last_disk_check = 0;

    while (1) {
        static int loop_count = 0;
        loop_count++;

        if (loop_count % 10 == 0 || last_status_bar_update == 0) {
            int ai_ok = ai_status_query();
            int task_cnt = get_background_task_count();
            const char *mode = startup_mode_name(startup_mode_get());
            log_draw_status_bar(version_get(), ai_ok, mode, task_cnt);
            last_status_bar_update = 1;
        }

        time_t now = time(NULL);
        if (now - last_disk_check > 300) {
            check_disk_space();
            last_disk_check = now;
        }

        /* 授权阻塞检查 */
        char auth_req_id[64];
        if (auth_service_pending(auth_req_id, sizeof(auth_req_id))) {
            LOG_INFO_T("Shell", "AuthBlock", "Enter", "request_id='%s'", auth_req_id);
            log_draw_box(tr("Authorization Request", "授权请求"),
                         tr("High-risk operation. Enter Y/N: ",
                            "高风险操作，输入 Y/N: "),
                         COLOR_YELLOW, COLOR_RED, COLOR_WHITE);
            uart_puts(COLOR_BOLD COLOR_YELLOW);
            uart_puts(tr("[Authorization] Enter Y(approve) or N(deny): ",
                         "[授权] 输入 Y(批准) 或 N(拒绝): "));
            uart_puts(COLOR_RESET);

            char c = 0;
            while (1) {
                c = uart_getc();
                if (c == '\r' || c == '\n') continue;
                if (c == 'Y' || c == 'y' || c == 'N' || c == 'n') {
                    uart_putc(c);
                    uart_puts("\n");
                    break;
                }
            }

            if (c == 'Y' || c == 'y') {
                auth_service_respond(auth_req_id, 1);
                log_draw_box(tr("Authorization Result", "授权结果"),
                             tr("Approved", "已批准"),
                             COLOR_GREEN, COLOR_DIM, COLOR_WHITE);
                LOG_INFO_T("Shell", "AuthBlock", "Approved", "request_id='%s'", auth_req_id);
            } else {
                auth_service_respond(auth_req_id, 0);
                log_draw_box(tr("Authorization Result", "授权结果"),
                             tr("Denied", "已拒绝"),
                             COLOR_RED, COLOR_DIM, COLOR_WHITE);
                LOG_INFO_T("Shell", "AuthBlock", "Denied", "request_id='%s'", auth_req_id);
            }
            continue;
        }

        syswatch_feed();
        nook_idle_poll(timer_get_ticks());

        /* ---- 构建增强提示符（含自定义支持） ---- */
        int ai_ok = ai_status_query();
        const char *status_str = ai_ok ? COLOR_GREEN "●" COLOR_RESET : COLOR_RED "○" COLOR_RESET;
        int task_cnt = get_background_task_count();
        const char *mode = startup_mode_name(startup_mode_get());
        char hostname[64] = "unknown";
        if (gethostname(hostname, sizeof(hostname)) != 0) {
            safe_strncpy(hostname, "unknown", sizeof(hostname));
        }

        const char *custom_prompt = getenv("LINGOS_PROMPT");
        if (custom_prompt && custom_prompt[0]) {
            /* 简单占位符替换 */
            char tmp[256];
            safe_strncpy(tmp, custom_prompt, sizeof(tmp));
            char *p = strstr(tmp, "%AI_STATUS%");
            if (p) {
                char before[128], after[128];
                safe_strncpy(before, tmp, p - tmp + 1);
                safe_strncpy(after, p + 11, sizeof(after));
                safe_snprintf(prompt, sizeof(prompt), "%s%s%s", before, status_str, after);
            } else {
                safe_strncpy(prompt, tmp, sizeof(prompt));
            }
        } else {
            /* 默认格式：AI状态 [模式] 主机名:任务数> */
            safe_snprintf(prompt, sizeof(prompt), "%s [%s] %s:%d> ",
                          status_str, mode, hostname, task_cnt);
        }
        uart_puts(prompt);

        /* ---- 读取用户输入 ---- */
        idx = 0;
        while (1) {
            char c = uart_getc();
            if (c == '\r' || c == '\n') {
                uart_puts("\r\n");
                cmd[idx] = '\0';
                break;
            } else if (c == '\b' || c == 127) {
                /* 【修复】UTF-8 感知退格（中文/全角按宽度擦除，保护提示符） */
                safe_backspace_echo(cmd, &idx);
            } else if (c == '\t') {
                const char *completed = completion_try(cmd);
                if (completed && completed[0]) {
                    while (idx > 0) { uart_puts("\b \b"); idx--; }
                    strcpy(cmd, completed);
                    idx = strlen(cmd);
                    uart_puts(cmd);
                }
            } else if (c == 0x03) {   /* ^C：清空当前输入行 */
                cmd[0] = '\0';
                idx = 0;
                uart_puts("\r\033[K");
                uart_puts(prompt);
            } else if (c == 0x15) {   /* ^U：删除整行 */
                while (idx > 0) {
                    safe_backspace_echo(cmd, &idx);
                }
            } else if (c == 0x0C) {   /* ^L：清屏 */
                uart_puts("\033[2J\033[H");
                uart_puts(prompt);
            } else if (c == 0x01) {   /* ^A：行首（无光标移动，忽略） */
                /* no-op */
            } else if (c == 27) {
                char c2 = uart_getc();
                if (c2 == '[') {
                    char c3 = uart_getc();
                    if (c3 == 'A') {
                        const char *prev = history_prev();
                        if (prev) {
                            while (idx > 0) { uart_puts("\b \b"); idx--; }
                            strcpy(cmd, prev);
                            idx = strlen(cmd);
                            uart_puts(cmd);
                        }
                    } else if (c3 == 'B') {
                        const char *next = history_next();
                        if (next) {
                            while (idx > 0) { uart_puts("\b \b"); idx--; }
                            strcpy(cmd, next);
                            idx = strlen(cmd);
                            uart_puts(cmd);
                        }
                    }
                }
            } else {
                if (idx < INPUT_BUF_SIZE - 1) {
                    cmd[idx++] = c;
                    uart_putc(c);
                }
            }
        }

        if (idx == 0) continue;

        if (strspn(cmd, " \t\r\n") == strlen(cmd)) {
            continue;
        }

        if (cmd[0] != '\0' && strcmp(cmd, "history") != 0) {
            history_add(cmd);
        }

        expand_alias(cmd, sizeof(cmd));

        syswatch_start_command();

        if (is_host_command(cmd)) {
            exec_host_command(cmd + 5);
        } else {
            if (!handle_builtin_command(cmd)) {
                uart_puts(tr("Unknown command. Type 'help' for available commands.\n",
                             "未知命令。输入 'help' 查看可用命令。\n"));
            }
        }

        syswatch_end_command();
        nook_idle_feed(timer_get_ticks());
    }
}