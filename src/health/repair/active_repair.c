/**
 * @file    src/health/repair/active_repair.c
 * @brief   主动健康修复（自愈）实现
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C, AI-CTL
 * @changes 集成 Python 诊断引擎调用；
 *          修复 Socket 超时处理；
 *          增加诊断引擎降级备选。
 */

#include "active_repair.h"
#include "../../common/data_path.h"
#include "../../common/safe_string.h"
#include "../../common/lang.h"
#include "../../lib/log_extra.h"
#include "../../drivers/uart.h"
#include "../../security/audit.h"
#include "../../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>
#include <regex.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

#define MAX_STRATEGIES 16
#define HISTORY_FILE "/LINGOS/state/repair_history.json"
#define DEFAULT_STRATEGIES_PATH "/system/config/repair_strategies.json"
#define REPAIR_SOCKET_PATH "/LINGOS/run/repair.sock"

/* ============================================================
 * 全局状态
 * ============================================================ */

static repair_strategy_t g_strategies[MAX_STRATEGIES];
static int g_strategy_count = 0;
static int g_initialized = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * 内部辅助：连接 Python 修复引擎（带超时）
 * ============================================================ */

static int connect_repair_engine(void) {
    LOG_DEBUG_T("ActiveRepair", "ConnectEngine", "Enter", "connecting to %s", REPAIR_SOCKET_PATH);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR_T("ActiveRepair", "ConnectEngine", "SocketFail", "socket() error: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, REPAIR_SOCKET_PATH, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    /* 设置非阻塞模式，实现超时连接 */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        close(fd);
        LOG_ERROR_T("ActiveRepair", "ConnectEngine", "FcntlFail", "fcntl F_GETFL failed: %s", strerror(errno));
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        close(fd);
        LOG_ERROR_T("ActiveRepair", "ConnectEngine", "FcntlSetFail", "fcntl F_SETFL failed: %s", strerror(errno));
        return -1;
    }

    int ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(fd);
        LOG_DEBUG_T("ActiveRepair", "ConnectEngine", "ConnectFail", "connect failed: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }

    if (ret < 0 && errno == EINPROGRESS) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int poll_ret = poll(&pfd, 1, 3000);  /* 3 秒超时 */
        if (poll_ret < 0) {
            close(fd);
            LOG_ERROR_T("ActiveRepair", "ConnectEngine", "PollFail", "poll error: %s", strerror(errno));
            return -1;
        }
        if (poll_ret == 0) {
            close(fd);
            LOG_WARN_T("ActiveRepair", "ConnectEngine", "Timeout", "connection timeout");
            return -1;
        }
        int err;
        socklen_t errlen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0 || err != 0) {
            close(fd);
            LOG_WARN_T("ActiveRepair", "ConnectEngine", "ConnectError", "connection error: %s", strerror(err));
            return -1;
        }
    }

    /* 恢复阻塞模式 */
    fcntl(fd, F_SETFL, flags);

    LOG_DEBUG_T("ActiveRepair", "ConnectEngine", "OK", "connected to repair engine, fd=%d", fd);
    return fd;
}

/* ============================================================
 * 内部辅助：向 Python 修复引擎发送请求（带超时）
 * ============================================================ */

static int send_repair_request(const char *cmd_json, char *resp_buf, size_t buf_len) {
    LOG_DEBUG_T("ActiveRepair", "SendRequest", "Enter", "cmd='%s'", cmd_json ? cmd_json : "(null)");

    if (!cmd_json || !resp_buf || buf_len == 0) {
        LOG_ERROR_T("ActiveRepair", "SendRequest", "Invalid", "cmd_json=%p, resp_buf=%p", (void*)cmd_json, (void*)resp_buf);
        return -1;
    }

    int fd = connect_repair_engine();
    if (fd < 0) {
        LOG_DEBUG_T("ActiveRepair", "SendRequest", "ConnectFail", "cannot connect to repair engine");
        return -1;
    }

    ssize_t written = write(fd, cmd_json, strlen(cmd_json));
    if (written < 0) {
        LOG_ERROR_T("ActiveRepair", "SendRequest", "WriteFail", "write error: %s (errno=%d)", strerror(errno), errno);
        close(fd);
        return -1;
    }
    if (write(fd, "\n", 1) < 0) {
        LOG_ERROR_T("ActiveRepair", "SendRequest", "NewlineFail", "write newline error: %s", strerror(errno));
        close(fd);
        return -1;
    }
    LOG_DEBUG_T("ActiveRepair", "SendRequest", "Written", "sent %zd bytes", written);

    /* 使用 poll 实现读超时 */
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    size_t pos = 0;
    while (pos < buf_len - 1) {
        int poll_ret = poll(&pfd, 1, 5000);  /* 5 秒超时 */
        if (poll_ret < 0) {
            LOG_ERROR_T("ActiveRepair", "SendRequest", "PollFail", "poll error: %s", strerror(errno));
            close(fd);
            return -1;
        }
        if (poll_ret == 0) {
            LOG_WARN_T("ActiveRepair", "SendRequest", "Timeout", "response timeout");
            close(fd);
            return -1;
        }
        ssize_t n = read(fd, resp_buf + pos, 1);
        if (n <= 0) {
            LOG_WARN_T("ActiveRepair", "SendRequest", "ReadFail", "read() returned %zd", n);
            break;
        }
        if (resp_buf[pos] == '\n') {
            resp_buf[pos] = '\0';
            LOG_DEBUG_T("ActiveRepair", "SendRequest", "Response", "received: %s", resp_buf);
            close(fd);
            return 0;
        }
        pos++;
    }
    resp_buf[pos] = '\0';
    LOG_WARN_T("ActiveRepair", "SendRequest", "Incomplete", "incomplete response");
    close(fd);
    return -1;
}

/* ============================================================
 * 获取配置路径
 * ============================================================ */

static const char* get_default_config_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, DEFAULT_STRATEGIES_PATH);
    }
    return path;
}

/* ============================================================
 * 解析动作类型
 * ============================================================ */

static repair_action_type_t parse_action_type(const char *type_str) {
    if (!type_str) return ACTION_UNKNOWN;
    if (strcmp(type_str, "clean_cache") == 0) return ACTION_CLEAN_CACHE;
    if (strcmp(type_str, "clean_logs") == 0) return ACTION_CLEAN_LOGS;
    if (strcmp(type_str, "restart_ai_server") == 0) return ACTION_RESTART_AI_SERVER;
    if (strcmp(type_str, "restart_daemon") == 0) return ACTION_RESTART_DAEMON;
    if (strcmp(type_str, "restart_service") == 0) return ACTION_RESTART_SERVICE;
    if (strcmp(type_str, "notify_user") == 0) return ACTION_NOTIFY_USER;
    if (strcmp(type_str, "rollback") == 0) return ACTION_ROLLBACK;
    if (strcmp(type_str, "repair_config") == 0) return ACTION_REPAIR_CONFIG;
    if (strcmp(type_str, "generate_repair_pack") == 0) return ACTION_REPAIR_PACK;
    return ACTION_UNKNOWN;
}

/* ============================================================
 * 从 cJSON 加载策略
 * ============================================================ */

static int load_strategy_from_json(cJSON *item) {
    if (!item || !cJSON_IsObject(item)) return -1;

    repair_strategy_t strategy;
    memset(&strategy, 0, sizeof(strategy));

    cJSON *pattern = cJSON_GetObjectItem(item, "error_pattern");
    cJSON *severity = cJSON_GetObjectItem(item, "severity");
    cJSON *actions = cJSON_GetObjectItem(item, "actions");
    cJSON *fallback = cJSON_GetObjectItem(item, "fallback");

    if (!pattern || !cJSON_IsString(pattern)) return -1;
    if (!severity || !cJSON_IsNumber(severity)) return -1;
    if (!actions || !cJSON_IsArray(actions)) return -1;

    safe_strncpy(strategy.error_pattern, pattern->valuestring, sizeof(strategy.error_pattern));
    strategy.severity = severity->valueint;

    int action_count = cJSON_GetArraySize(actions);
    if (action_count > 4) action_count = 4;

    for (int i = 0; i < action_count; i++) {
        cJSON *act = cJSON_GetArrayItem(actions, i);
        if (!act || !cJSON_IsObject(act)) continue;

        cJSON *type = cJSON_GetObjectItem(act, "type");
        cJSON *priority = cJSON_GetObjectItem(act, "priority");
        cJSON *service = cJSON_GetObjectItem(act, "service");

        if (!type || !cJSON_IsString(type)) continue;

        strategy.actions[i].type = parse_action_type(type->valuestring);
        strategy.actions[i].priority = (priority && cJSON_IsNumber(priority)) ? priority->valueint : i + 1;
        if (service && cJSON_IsString(service)) {
            safe_strncpy(strategy.actions[i].service_name, service->valuestring,
                         sizeof(strategy.actions[i].service_name));
        }
        strategy.actions[i].timeout_sec = 30;
        strategy.action_count++;
    }

    if (strategy.action_count == 0) {
        LOG_WARN_T("ActiveRepair", "LoadStrategy", "NoActions", "no actions in strategy");
        return -1;
    }

    if (fallback && cJSON_IsString(fallback)) {
        safe_strncpy(strategy.fallback, fallback->valuestring, sizeof(strategy.fallback));
    }

    pthread_mutex_lock(&g_lock);

    if (g_strategy_count >= MAX_STRATEGIES) {
        pthread_mutex_unlock(&g_lock);
        LOG_WARN_T("ActiveRepair", "LoadStrategy", "Overflow", "max strategies reached");
        return -1;
    }

    memcpy(&g_strategies[g_strategy_count], &strategy, sizeof(strategy));
    g_strategy_count++;

    pthread_mutex_unlock(&g_lock);

    LOG_DEBUG_T("ActiveRepair", "LoadStrategy", "OK", "pattern='%s', actions=%d",
                strategy.error_pattern, strategy.action_count);
    return 0;
}

/* ============================================================
 * 匹配错误模式
 * ============================================================ */

static int match_error_pattern(const char *error_msg, const char *pattern) {
    if (!error_msg || !pattern) return 0;

    regex_t regex;
    int ret = regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB);
    if (ret != 0) {
        if (strstr(error_msg, pattern) != NULL) {
            return 1;
        }
        return 0;
    }

    ret = regexec(&regex, error_msg, 0, NULL, 0);
    regfree(&regex);
    return (ret == 0) ? 1 : 0;
}

/* ============================================================
 * 执行修复动作
 * ============================================================ */

static int execute_action(const repair_action_t *action, char *out_msg, size_t out_len) {
    LOG_INFO_T("ActiveRepair", "Execute", "Enter", "action=%d", action->type);

    if (!action || !out_msg) {
        if (out_msg) safe_strncpy(out_msg, tr("Invalid action", "无效操作"), out_len);
        return -1;
    }

    safe_strncpy(out_msg, tr("OK", "成功"), out_len);

    switch (action->type) {
        case ACTION_CLEAN_CACHE: {
            const char *root = lingos_data_root();
            char cache_dir[512];
            safe_snprintf(cache_dir, sizeof(cache_dir), "%s/cache", root);
            if (access(cache_dir, F_OK) == 0) {
                char cmd[512];
                safe_snprintf(cmd, sizeof(cmd), "rm -rf '%s'/* 2>/dev/null", cache_dir);
                system(cmd);
                LOG_INFO_T("ActiveRepair", "Execute", "Cache", "cache cleaned");
                safe_snprintf(out_msg, out_len, tr("Cache cleaned", "缓存已清理"));
            } else {
                safe_snprintf(out_msg, out_len, tr("Cache directory not found", "缓存目录不存在"));
            }
            return 0;
        }

        case ACTION_CLEAN_LOGS: {
            const char *root = lingos_data_root();
            char log_dir[512];
            safe_snprintf(log_dir, sizeof(log_dir), "%s/Debug", root);
            if (access(log_dir, F_OK) == 0) {
                char cmd[512];
                safe_snprintf(cmd, sizeof(cmd), "find '%s' -name '*.log' -mtime +1 -delete 2>/dev/null", log_dir);
                system(cmd);
                LOG_INFO_T("ActiveRepair", "Execute", "Logs", "old logs cleaned");
                safe_snprintf(out_msg, out_len, tr("Old logs cleaned", "旧日志已清理"));
            } else {
                safe_snprintf(out_msg, out_len, tr("Log directory not found", "日志目录不存在"));
            }
            return 0;
        }

        case ACTION_RESTART_AI_SERVER: {
            system("pkill -f ai_server.py || true");
            usleep(500000);
            /* 【2026-08-22 定稿】全捆包 Python 同步长期修复：
             * 优先用包内 venv（$LINGOS_ROOT/python/bin/python $LINGOS_ROOT/python/server/ai_server.py）
             * ——部署零脚本（历史 Bug：/LINGOS/bin 是旧文件，需 fix_python_sync.sh 手动同步）
             * 无包内 venv 时回落 /LINGOS/bin（旧环境兼容——跛脚） */
            const char *broot = getenv("LINGOS_ROOT");
            char venv_py[512], venv_server[512], cmd[1024];
            int use_venv = 0;
            if (broot && broot[0]) {
                safe_snprintf(venv_py, sizeof(venv_py), "%s/python/bin/python", broot);
                safe_snprintf(venv_server, sizeof(venv_server), "%s/python/server/ai_server.py", broot);
                if (access(venv_py, X_OK) == 0 && access(venv_server, F_OK) == 0) use_venv = 1;
            }
            if (use_venv) {
                safe_snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" &", venv_py, venv_server);
            } else {
                safe_snprintf(cmd, sizeof(cmd), "python3 /LINGOS/bin/ai_server.py &");
            }
            system(cmd);
            LOG_INFO_T("ActiveRepair", "Execute", "AIServer", "AI server restarted (venv=%d cmd=%s)", use_venv, cmd);
            safe_snprintf(out_msg, out_len, tr("AI server restarted", "AI 服务器已重启"));
            return 0;
        }

        case ACTION_RESTART_DAEMON: {
            system("pkill lingosd || true");
            usleep(500000);
            system("./lingosd &");
            LOG_INFO_T("ActiveRepair", "Execute", "Daemon", "daemon restarted");
            safe_snprintf(out_msg, out_len, tr("Daemon restarted", "守护进程已重启"));
            return 0;
        }

        case ACTION_RESTART_SERVICE: {
            if (action->service_name[0] == '\0') {
                safe_snprintf(out_msg, out_len, tr("No service name specified", "未指定服务名称"));
                return -1;
            }
            char cmd[256];
            safe_snprintf(cmd, sizeof(cmd), "systemctl restart %s 2>&1", action->service_name);
            int ret = system(cmd);
            if (ret == 0) {
                LOG_INFO_T("ActiveRepair", "Execute", "Service", "%s restarted", action->service_name);
                safe_snprintf(out_msg, out_len, tr("Service '%s' restarted", "服务 '%s' 已重启"), action->service_name);
                return 0;
            } else {
                safe_snprintf(out_msg, out_len, tr("Failed to restart service '%s'", "重启服务 '%s' 失败"), action->service_name);
                return -1;
            }
        }

        case ACTION_NOTIFY_USER: {
            uart_puts(COLOR_YELLOW);
            uart_puts(tr("\n[REPAIR] System has performed auto-repair.\n",
                         "\n[修复] 系统已执行自动修复。\n"));
            uart_puts(tr("Check logs for details: /LINGOS/Debug/lingos_*.log\n",
                         "查看日志获取详情：/LINGOS/Debug/lingos_*.log\n"));
            uart_puts(COLOR_RESET);
            safe_snprintf(out_msg, out_len, tr("User notified", "已通知用户"));
            return 0;
        }

        case ACTION_ROLLBACK: {
            system("system rollback");
            LOG_WARN_T("ActiveRepair", "Execute", "Rollback", "system rollback triggered");
            safe_snprintf(out_msg, out_len, tr("System rollback triggered", "系统回滚已触发"));
            return 0;
        }

        case ACTION_REPAIR_CONFIG: {
            safe_snprintf(out_msg, out_len, tr("Config repair (stub)", "配置修复（占位）"));
            return 0;
        }

        case ACTION_REPAIR_PACK: {
            safe_snprintf(out_msg, out_len, tr("Repair pack generation (stub)", "修复包生成（占位）"));
            return 0;
        }

        default: {
            safe_snprintf(out_msg, out_len, tr("Unknown action type: %d", "未知操作类型：%d"), action->type);
            return -1;
        }
    }
}

/* ============================================================
 * 记录修复历史
 * ============================================================ */

static void record_history(const char *action, const char *result, int success) {
    LOG_INFO_T("ActiveRepair", "History", "Record", "action=%s, result=%s, success=%d", action, result, success);
    audit_log("system", "active_repair", "repair", action, result, success ? 0 : -1, "high", success);
}

/* ============================================================
 * 公共 API
 * ============================================================ */

int active_repair_init(void) {
    LOG_INFO_T("ActiveRepair", "Init", "Enter", "initializing active repair system");

    if (g_initialized) {
        LOG_DEBUG_T("ActiveRepair", "Init", "Already", "already initialized");
        return 0;
    }

    pthread_mutex_lock(&g_lock);
    memset(g_strategies, 0, sizeof(g_strategies));
    g_strategy_count = 0;
    pthread_mutex_unlock(&g_lock);

    const char *path = get_default_config_path();
    int ret = active_repair_load_strategies(path);
    if (ret != 0) {
        LOG_WARN_T("ActiveRepair", "Init", "NoConfig", "using built-in fallback strategies");
        pthread_mutex_lock(&g_lock);
        g_strategy_count = 4;
        safe_strncpy(g_strategies[0].error_pattern, "memory.*[89][0-9]%", sizeof(g_strategies[0].error_pattern));
        g_strategies[0].severity = 4;
        g_strategies[0].actions[0].type = ACTION_CLEAN_CACHE;
        g_strategies[0].actions[0].priority = 1;
        g_strategies[0].action_count = 1;

        safe_strncpy(g_strategies[1].error_pattern, "disk.*[89][0-9]%", sizeof(g_strategies[1].error_pattern));
        g_strategies[1].severity = 3;
        g_strategies[1].actions[0].type = ACTION_CLEAN_LOGS;
        g_strategies[1].actions[0].priority = 1;
        g_strategies[1].action_count = 1;

        safe_strncpy(g_strategies[2].error_pattern, "ai_server.*crash", sizeof(g_strategies[2].error_pattern));
        g_strategies[2].severity = 5;
        g_strategies[2].actions[0].type = ACTION_RESTART_AI_SERVER;
        g_strategies[2].actions[0].priority = 1;
        g_strategies[2].actions[1].type = ACTION_NOTIFY_USER;
        g_strategies[2].actions[1].priority = 2;
        g_strategies[2].action_count = 2;

        safe_strncpy(g_strategies[3].error_pattern, "lingosd.*crash", sizeof(g_strategies[3].error_pattern));
        g_strategies[3].severity = 5;
        g_strategies[3].actions[0].type = ACTION_RESTART_DAEMON;
        g_strategies[3].actions[0].priority = 1;
        g_strategies[3].actions[1].type = ACTION_NOTIFY_USER;
        g_strategies[3].actions[1].priority = 2;
        g_strategies[3].action_count = 2;
        pthread_mutex_unlock(&g_lock);
    }

    g_initialized = 1;
    LOG_INFO_T("ActiveRepair", "Init", "OK", "active repair system ready with %d strategies", g_strategy_count);
    return 0;
}

int active_repair_load_strategies(const char *path) {
    LOG_INFO_T("ActiveRepair", "LoadStrategies", "Enter", "path='%s'", path ? path : "(null)");

    if (!path) {
        path = get_default_config_path();
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("ActiveRepair", "LoadStrategies", "NotFound", "config file not found");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(fp);
        LOG_ERROR_T("ActiveRepair", "LoadStrategies", "MallocFail", "malloc failed");
        return -1;
    }

    fread(buf, 1, size, fp);
    buf[size] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        LOG_ERROR_T("ActiveRepair", "LoadStrategies", "ParseFail", "invalid JSON");
        return -1;
    }

    cJSON *strategies = cJSON_GetObjectItem(root, "strategies");
    if (!strategies || !cJSON_IsArray(strategies)) {
        LOG_WARN_T("ActiveRepair", "LoadStrategies", "NoStrategies", "no strategies array");
        cJSON_Delete(root);
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    memset(g_strategies, 0, sizeof(g_strategies));
    g_strategy_count = 0;
    pthread_mutex_unlock(&g_lock);

    int loaded = 0;
    int size_arr = cJSON_GetArraySize(strategies);
    for (int i = 0; i < size_arr && loaded < MAX_STRATEGIES; i++) {
        cJSON *item = cJSON_GetArrayItem(strategies, i);
        if (load_strategy_from_json(item) == 0) {
            loaded++;
        }
    }

    cJSON_Delete(root);

    LOG_INFO_T("ActiveRepair", "LoadStrategies", "OK", "loaded %d strategies from %s", loaded, path);
    return 0;
}

/* ============================================================
 * 触发修复（集成诊断引擎）
 * ============================================================ */

int active_repair_trigger(const char *error_msg, const char *error_source, repair_result_t *result) {
    LOG_INFO_T("ActiveRepair", "Trigger", "Enter", "error_msg='%.100s...', source='%s'",
               error_msg ? error_msg : "(null)", error_source ? error_source : "(null)");

    if (!error_msg || !result) {
        LOG_ERROR_T("ActiveRepair", "Trigger", "Invalid", "error_msg=%p, result=%p",
                    (void*)error_msg, (void*)result);
        return -1;
    }

    if (!g_initialized) {
        if (active_repair_init() != 0) {
            LOG_ERROR_T("ActiveRepair", "Trigger", "InitFail", "repair system not initialized");
            return -1;
        }
    }

    memset(result, 0, sizeof(repair_result_t));
    result->success = 0;
    safe_strncpy(result->error_msg, tr("No matching strategy", "未匹配到策略"), sizeof(result->error_msg));

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* ---- 1. 优先尝试 Socket 连接 Python 诊断引擎 ---- */
    char cmd_json[512];
    safe_snprintf(cmd_json, sizeof(cmd_json),
                  "{\"cmd\":\"diagnose_and_repair\",\"error\":\"%s\",\"source\":\"%s\"}",
                  error_msg, error_source ? error_source : "unknown");

    char resp_buf[2048];
    int socket_ret = send_repair_request(cmd_json, resp_buf, sizeof(resp_buf));

    if (socket_ret == 0) {
        /* Python 引擎成功响应 */
        cJSON *resp = cJSON_Parse(resp_buf);
        if (resp) {
            cJSON *status = cJSON_GetObjectItem(resp, "status");
            cJSON *result_json = cJSON_GetObjectItem(resp, "result");
            cJSON *action = cJSON_GetObjectItem(resp, "action");
            cJSON *msg = cJSON_GetObjectItem(resp, "message");

            if (status && cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) {
                result->success = 1;
                if (action && cJSON_IsString(action)) {
                    safe_strncpy(result->action_used, action->valuestring, sizeof(result->action_used));
                } else {
                    safe_strncpy(result->action_used, tr("repair_engine", "修复引擎"), sizeof(result->action_used));
                }
                if (msg && cJSON_IsString(msg)) {
                    safe_strncpy(result->error_msg, msg->valuestring, sizeof(result->error_msg));
                } else {
                    safe_strncpy(result->error_msg, tr("Repair handled by Python engine", "修复已由 Python 引擎处理"),
                                 sizeof(result->error_msg));
                }
                LOG_INFO_T("ActiveRepair", "Trigger", "PythonOK", "repair handled by Python engine: %s",
                           result->action_used);
                cJSON_Delete(resp);
                clock_gettime(CLOCK_MONOTONIC, &end);
                result->duration_ms = (int64_t)((end.tv_sec - start.tv_sec) * 1000 +
                                                (end.tv_nsec - start.tv_nsec) / 1000000);
                record_history("python_engine", result->action_used, 1);
                return 0;
            }
            /* 如果 Python 返回了错误，尝试降级 */
            if (msg && cJSON_IsString(msg)) {
                LOG_WARN_T("ActiveRepair", "Trigger", "PythonError", "Python engine: %s", msg->valuestring);
            }
            cJSON_Delete(resp);
        } else {
            LOG_WARN_T("ActiveRepair", "Trigger", "PythonParseFail", "failed to parse Python response");
        }
    } else {
        LOG_DEBUG_T("ActiveRepair", "Trigger", "PythonUnavailable", "Python engine unavailable, using C fallback");
    }

    /* ---- 2. 降级：使用 C 端内置策略 ---- */
    int matched = -1;
    int highest_severity = -1;

    pthread_mutex_lock(&g_lock);

    for (int i = 0; i < g_strategy_count; i++) {
        if (match_error_pattern(error_msg, g_strategies[i].error_pattern)) {
            if (g_strategies[i].severity > highest_severity) {
                matched = i;
                highest_severity = g_strategies[i].severity;
            }
        }
    }

    if (matched < 0) {
        pthread_mutex_unlock(&g_lock);
        safe_strncpy(result->error_msg, tr("No matching strategy found", "未找到匹配策略"), sizeof(result->error_msg));
        LOG_DEBUG_T("ActiveRepair", "Trigger", "NoMatch", "no strategy matched");
        clock_gettime(CLOCK_MONOTONIC, &end);
        result->duration_ms = (int64_t)((end.tv_sec - start.tv_sec) * 1000 +
                                        (end.tv_nsec - start.tv_nsec) / 1000000);
        return -1;
    }

    repair_strategy_t *strategy = &g_strategies[matched];
    pthread_mutex_unlock(&g_lock);

    LOG_INFO_T("ActiveRepair", "Trigger", "Matched", "strategy #%d, severity=%d", matched, strategy->severity);

    /* 按优先级排序动作 */
    for (int i = 0; i < strategy->action_count - 1; i++) {
        for (int j = i + 1; j < strategy->action_count; j++) {
            if (strategy->actions[i].priority > strategy->actions[j].priority) {
                repair_action_t tmp = strategy->actions[i];
                strategy->actions[i] = strategy->actions[j];
                strategy->actions[j] = tmp;
            }
        }
    }

    int exec_success = 0;
    char action_name[64];
    char exec_msg[256];

    for (int i = 0; i < strategy->action_count; i++) {
        char msg[256];
        int ret = execute_action(&strategy->actions[i], msg, sizeof(msg));
        safe_strncpy(exec_msg, msg, sizeof(exec_msg));

        if (ret == 0) {
            exec_success = 1;
            safe_strncpy(action_name, active_repair_action_name(strategy->actions[i].type),
                         sizeof(action_name));
            LOG_INFO_T("ActiveRepair", "Trigger", "ActionOK", "%s: %s", action_name, msg);
            break;
        } else {
            LOG_WARN_T("ActiveRepair", "Trigger", "ActionFail", "%s: %s",
                       active_repair_action_name(strategy->actions[i].type), msg);
        }
    }

    if (exec_success) {
        result->success = 1;
        safe_strncpy(result->action_used, action_name, sizeof(result->action_used));
        safe_strncpy(result->error_msg, exec_msg, sizeof(result->error_msg));
        record_history(action_name, exec_msg, 1);
        audit_log("system", "active_repair", "repair", action_name, "success", 0, "high", 1);
    } else {
        result->success = 0;
        safe_strncpy(result->action_used, tr("none", "无"), sizeof(result->action_used));
        safe_strncpy(result->error_msg, tr("All actions failed", "所有操作失败"), sizeof(result->error_msg));
        record_history("all_actions", "failed", 0);
        audit_log("system", "active_repair", "repair", "all_actions", "failed", -1, "high", 0);

        if (strategy->fallback[0] != '\0') {
            LOG_WARN_T("ActiveRepair", "Trigger", "Fallback", "using fallback: %s", strategy->fallback);
            if (strcmp(strategy->fallback, "rollback") == 0) {
                char msg[256];
                repair_action_t fb_action = {.type = ACTION_ROLLBACK};
                execute_action(&fb_action, msg, sizeof(msg));
                safe_strncpy(result->error_msg, tr("Fallback rollback triggered", "降级回滚已触发"),
                             sizeof(result->error_msg));
                record_history("fallback_rollback", msg, 1);
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    result->duration_ms = (int64_t)((end.tv_sec - start.tv_sec) * 1000 +
                                    (end.tv_nsec - start.tv_nsec) / 1000000);

    LOG_INFO_T("ActiveRepair", "Trigger", "Done", "success=%d, duration=%lldms",
               result->success, (long long)result->duration_ms);
    return 0;
}

int active_repair_strategy_count(void) {
    return g_strategy_count;
}

int active_repair_get_history(char *out, size_t out_len, int limit) {
    (void)limit;
    if (!out || out_len == 0) return -1;
    safe_snprintf(out, out_len, "[]");
    return 0;
}

void active_repair_cleanup(void) {
    LOG_INFO_T("ActiveRepair", "Cleanup", "Enter", "cleaning up active repair system");

    pthread_mutex_lock(&g_lock);
    memset(g_strategies, 0, sizeof(g_strategies));
    g_strategy_count = 0;
    g_initialized = 0;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO_T("ActiveRepair", "Cleanup", "OK", "active repair system cleaned up");
}

const char* active_repair_action_name(repair_action_type_t type) {
    switch (type) {
        case ACTION_UNKNOWN:            return tr("unknown", "未知");
        case ACTION_CLEAN_CACHE:        return tr("clean_cache", "清理缓存");
        case ACTION_CLEAN_LOGS:         return tr("clean_logs", "清理日志");
        case ACTION_RESTART_AI_SERVER:  return tr("restart_ai_server", "重启AI服务器");
        case ACTION_RESTART_DAEMON:     return tr("restart_daemon", "重启守护进程");
        case ACTION_RESTART_SERVICE:    return tr("restart_service", "重启服务");
        case ACTION_NOTIFY_USER:        return tr("notify_user", "通知用户");
        case ACTION_ROLLBACK:           return tr("rollback", "回滚");
        case ACTION_REPAIR_CONFIG:      return tr("repair_config", "修复配置");
        case ACTION_REPAIR_PACK:        return tr("generate_repair_pack", "生成修复包");
        default:                        return tr("unknown", "未知");
    }
}