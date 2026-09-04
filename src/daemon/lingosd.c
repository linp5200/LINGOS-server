/**
 * @file    src/daemon/lingosd.c
 * @brief   LING OS Daemon - Unix socket server for syscall & AI commands
 * @version LN-0.4.3
 * @changes 新增 registry_list 命令；新增 set_log_level 命令转发；
 *          修改 skill_schemas 读取注册表路径；
 *          在 forward_to_python 中增加响应有效性检查，记录无效响应。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include "nook.h"
#include "syscall_handler.h"
#include "audit.h"
#include "libling.h"
#include "log_extra.h"
#include "data_path.h"
#include "linux_io.h"
#include "linux_timer.h"
#include "tcp_client.h"
#include "permission.h"
#include "fs_layout.h"
#include "api_core.h"
#include "defense.h"
#include "ai_master.h"
#include "nook_repair.h"
#include "nook_idle.h"
#include "ai_config.h"
#include "lingos_config.h"
#include "ai_server_protocol.h"
#include "cJSON.h"
#include "safe_string.h"
#include "connection_handler.h"

#define SOCKET_PATH DAEMON_SOCKET_PATH
#define BUF_SIZE 16384
#define PID_PATH LINGOS_RUN_DIR "/lingosd.pid"
#define AI_SOCKET_PATH_FWD AI_SOCKET_PATH

static void cleanup_on_exit(int sig) {
    LOG_INFO_T("Lingosd", "Cleanup", "Enter", "signal=%d", sig);
    (void)sig;
    unlink(PID_PATH);
    unlink(SOCKET_PATH);
    /* 【新增】清理 registry.sock */
    const char *root = lingos_data_root();
    char reg_sock_path[512];
    safe_snprintf(reg_sock_path, sizeof(reg_sock_path), "%s/run/registry.sock", root);
    unlink(reg_sock_path);
    LOG_INFO_T("Lingosd", "Cleanup", "OK", "Removed PID and socket files");
    exit(0);
}

static int daemon_init(void) {
    LOG_INFO_T("Lingosd", "Init", "Enter", "Initializing lingosd");
    linux_io_init();
    linux_timer_init();
    tcp_client_init();
    log_system_init();
    permission_init();
    fs_layout_init();
    api_core_init(1);  /* R1: lingosd 强制启动 WS/HTTP */

    nook_init();
    defense_init();
    ai_master_init();
    nook_repair_init();
    nook_idle_init();

    ai_config_load();
    LOG_INFO_T("Lingosd", "Init", "OK", "lingosd initialized successfully");
    return 0;
}

static char* read_skill_index(void) {
    LOG_DEBUG_T("Lingosd", "ReadSkillIndex", "enter", "");
    const char *root = lingos_data_root();
    char index_path[512];
    safe_snprintf(index_path, sizeof(index_path), "%s/registry/skills/index.json", root);

    FILE *fp = fopen(index_path, "r");
    if (!fp) {
        LOG_WARN_T("Lingosd", "ReadSkillIndex", "NoIndex", "registry index not found, using default");
        return strdup(
            "["
            "{\"name\":\"file_write\",\"description\":\"Create or overwrite file\",\"risk\":\"medium\"},"
            "{\"name\":\"file_read\",\"description\":\"Read file content\",\"risk\":\"low\"},"
            "{\"name\":\"file_delete\",\"description\":\"Delete file\",\"risk\":\"medium\"},"
            "{\"name\":\"file_list\",\"description\":\"List directory contents\",\"risk\":\"low\"},"
            "{\"name\":\"sub_ai_dispatch\",\"description\":\"Dispatch task to sub-AI\",\"risk\":\"low\"},"
            "{\"name\":\"sub_ai_status\",\"description\":\"Query sub-AI task status\",\"risk\":\"low\"},"
            "{\"name\":\"sys_command\",\"description\":\"Execute arbitrary system command\",\"risk\":\"critical\"}"
            "]"
        );
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    if (!buf) {
        LOG_ERROR_T("Lingosd", "ReadSkillIndex", "MallocFail", "malloc(%ld) failed", len);
        fclose(fp);
        return NULL;
    }

    size_t read_len = fread(buf, 1, len, fp);
    fclose(fp);
    buf[read_len] = '\0';

    LOG_DEBUG_T("Lingosd", "ReadSkillIndex", "OK", "read %zu bytes from %s", read_len, index_path);
    return buf;
}

static void send_json(int client_fd, const char *status, const char *result_type, const char *data) {
    LOG_DEBUG_T("Lingosd", "SendJSON", "Enter", "client_fd=%d, status='%s', result_type='%s'",
                client_fd, status ? status : "(null)", result_type ? result_type : "(null)");
    if (client_fd < 0) {
        LOG_WARN_T("Lingosd", "SendJSON", "Invalid", "client_fd=%d", client_fd);
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", status);

    if (result_type && data) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "type", result_type);
        cJSON_AddStringToObject(result, "data", data);
        cJSON_AddItemToObject(root, "result", result);
    } else if (data) {
        cJSON_AddStringToObject(root, "result", data);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        LOG_ERROR_T("Lingosd", "SendJSON", "PrintFail", "cJSON_PrintUnformatted failed");
        write(client_fd, "{\"status\":\"error\"}\n", 20);
        return;
    }

    write(client_fd, json_str, strlen(json_str));
    write(client_fd, "\n", 1);
    free(json_str);

    LOG_DEBUG_T("Lingosd", "SendJSON", "OK", "response sent");
}

static int forward_to_python(int client_fd, const char *json_req) {
    LOG_DEBUG_T("Lingosd", "ForwardToPython", "Enter", "client_fd=%d, json='%s'", client_fd, json_req ? json_req : "(null)");
    if (client_fd < 0 || !json_req) {
        LOG_ERROR_T("Lingosd", "ForwardToPython", "Invalid", "client_fd=%d, json_req=%p", client_fd, (void*)json_req);
        return -1;
    }

    int ai_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ai_fd < 0) {
        LOG_ERROR_T("Lingosd", "ForwardToPython", "SocketFail", "socket() error: %s (errno=%d)", strerror(errno), errno);
        send_json(client_fd, "error", NULL, "Cannot connect to AI server");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, AI_SOCKET_PATH_FWD, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    if (connect(ai_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR_T("Lingosd", "ForwardToPython", "ConnectFail", "connect to %s failed: %s (errno=%d)",
                    AI_SOCKET_PATH_FWD, strerror(errno), errno);
        close(ai_fd);
        send_json(client_fd, "error", NULL, "AI server not reachable");
        return -1;
    }

    LOG_DEBUG_T("Lingosd", "ForwardToPython", "Connected", "connected to AI server, fd=%d", ai_fd);

    if (write(ai_fd, json_req, strlen(json_req)) < 0 || write(ai_fd, "\n", 1) < 0) {
        LOG_ERROR_T("Lingosd", "ForwardToPython", "WriteFail", "write to AI server failed: %s", strerror(errno));
        close(ai_fd);
        send_json(client_fd, "error", NULL, "Write to AI server failed");
        return -1;
    }

    LOG_DEBUG_T("Lingosd", "ForwardToPython", "Sent", "request forwarded to AI server");

    char resp_buf[8192];
    int pos = 0;
    while (pos < (int)sizeof(resp_buf) - 1) {
        ssize_t n = read(ai_fd, resp_buf + pos, 1);
        if (n <= 0) {
            if (n == 0) {
                LOG_DEBUG_T("Lingosd", "ForwardToPython", "EOF", "AI server closed connection");
            } else {
                LOG_ERROR_T("Lingosd", "ForwardToPython", "ReadFail", "read error: %s (errno=%d)", strerror(errno), errno);
            }
            break;
        }
        if (resp_buf[pos] == '\n') {
            resp_buf[pos] = '\0';
            pos++;
            break;
        }
        pos++;
    }
    close(ai_fd);

    if (pos > 0) {
        if (strstr(resp_buf, "\"status\"") == NULL) {
            LOG_WARN_T("Lingosd", "ForwardToPython", "InvalidResp",
                       "response lacks 'status' field, forwarding anyway: %s", resp_buf);
        }
        write(client_fd, resp_buf, strlen(resp_buf));
        write(client_fd, "\n", 1);
        LOG_DEBUG_T("Lingosd", "ForwardToPython", "OK", "response forwarded, len=%d", pos);
        return 0;
    } else {
        LOG_WARN_T("Lingosd", "ForwardToPython", "NoResponse", "no response from AI server");
        send_json(client_fd, "error", NULL, "No response from AI server");
        return -1;
    }
}

static int read_line(int fd, char *buf, size_t buf_size) {
    LOG_DEBUG_T("Lingosd", "ReadLine", "Enter", "fd=%d, buf_size=%zu", fd, buf_size);
    if (!buf || buf_size == 0) {
        LOG_ERROR_T("Lingosd", "ReadLine", "Invalid", "buf=%p, buf_size=%zu", (void*)buf, buf_size);
        return -1;
    }

    size_t pos = 0;
    while (pos < buf_size - 1) {
        ssize_t n = read(fd, buf + pos, 1);
        if (n <= 0) {
            if (n == 0) {
                LOG_DEBUG_T("Lingosd", "ReadLine", "EOF", "connection closed, pos=%zu", pos);
            } else {
                LOG_ERROR_T("Lingosd", "ReadLine", "ReadFail", "read error: %s (errno=%d)", strerror(errno), errno);
            }
            if (pos > 0) {
                buf[pos] = '\0';
                return (int)pos;
            }
            return -1;
        }
        if (buf[pos] == '\n') {
            buf[pos] = '\0';
            LOG_DEBUG_T("Lingosd", "ReadLine", "OK", "read %zu bytes", pos);
            return (int)pos;
        }
        pos++;
    }
    buf[pos] = '\0';
    LOG_WARN_T("Lingosd", "ReadLine", "BufferFull", "buffer full, pos=%zu", pos);
    return (int)pos;
}

/* ============================================================
 * registry.sock 服务（【新增】供 Python skill_loader/registry_client 查询）
 * 协议：{"cmd":"ping|list|get|query", ...} → {"status":"ok","entries":[...]}
 * ============================================================ */

/* 类型名 ↔ registry_type_t 数字映射（registry.h: module=0...selfcheck=7） */
static int registry_type_from_string(const char *type_str) {
    if (!type_str || !*type_str) return -1;   /* -1 = 全部类型 */
    if (strcmp(type_str, "module") == 0) return 0;
    if (strcmp(type_str, "component") == 0) return 1;
    if (strcmp(type_str, "config") == 0) return 2;
    if (strcmp(type_str, "feature") == 0) return 3;
    if (strcmp(type_str, "skill") == 0) return 4;
    if (strcmp(type_str, "plugin") == 0) return 5;
    if (strcmp(type_str, "hook") == 0) return 6;
    if (strcmp(type_str, "selfcheck") == 0) return 7;
    return -1;
}

static void send_registry_response(int client_fd, const char *status, const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", status);
    if (message) cJSON_AddStringToObject(root, "message", message);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) {
        write(client_fd, s, strlen(s));
        write(client_fd, "\n", 1);
        free(s);
    }
}

static void handle_registry_command(int client_fd, const char *json_req) {
    LOG_DEBUG_T("Lingosd", "RegistryCmd", "Enter", "json='%s'", json_req ? json_req : "(null)");
    if (client_fd < 0 || !json_req) return;

    cJSON *root = cJSON_Parse(json_req);
    if (!root) { send_registry_response(client_fd, "error", "invalid JSON"); return; }
    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd_item)) {
        cJSON_Delete(root);
        send_registry_response(client_fd, "error", "missing cmd");
        return;
    }
    const char *cmd = cmd_item->valuestring;

    if (strcmp(cmd, "ping") == 0) {
        send_registry_response(client_fd, "ok", NULL);
        cJSON_Delete(root);
        return;
    }

    /* 读取主注册表文件（/LINGOS/registry/core/registry.json） */
    const char *root_path = lingos_data_root();
    char reg_path[512];
    safe_snprintf(reg_path, sizeof(reg_path), "%s/registry/core/registry.json", root_path);
    FILE *fp = fopen(reg_path, "r");
    if (!fp) {
        LOG_WARN_T("Lingosd", "RegistryCmd", "NoFile", "registry.json not found");
        cJSON_Delete(root);
        send_registry_response(client_fd, "error", "registry file not found");
        return;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        cJSON_Delete(root);
        send_registry_response(client_fd, "error", "memory allocation failed");
        return;
    }
    size_t rl = fread(buf, 1, (size_t)len, fp);
    buf[rl] = '\0';
    fclose(fp);

    cJSON *reg = cJSON_Parse(buf);
    free(buf);
    if (!reg) {
        cJSON_Delete(root);
        send_registry_response(client_fd, "error", "registry parse error");
        return;
    }

    cJSON *entries = cJSON_GetObjectItem(reg, "entries");
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");

    if (strcmp(cmd, "list") == 0) {
        int want_type = -1;
        cJSON *type_item = cJSON_GetObjectItem(root, "type");
        if (cJSON_IsString(type_item)) {
            want_type = registry_type_from_string(type_item->valuestring);
        }
        cJSON *out = cJSON_CreateArray();
        if (cJSON_IsArray(entries)) {
            int n = cJSON_GetArraySize(entries);
            for (int i = 0; i < n; i++) {
                cJSON *e = cJSON_GetArrayItem(entries, i);
                if (want_type >= 0) {
                    cJSON *t = cJSON_GetObjectItem(e, "type");
                    if (!cJSON_IsNumber(t) || t->valueint != want_type) continue;
                }
                cJSON_AddItemToArray(out, cJSON_Duplicate(e, 1));
            }
        }
        cJSON_AddItemToObject(resp, "entries", out);
    } else if (strcmp(cmd, "get") == 0) {
        cJSON *id_item = cJSON_GetObjectItem(root, "id");
        const char *id = cJSON_IsString(id_item) ? id_item->valuestring : "";
        cJSON *found = NULL;
        if (cJSON_IsArray(entries)) {
            int n = cJSON_GetArraySize(entries);
            for (int i = 0; i < n; i++) {
                cJSON *e = cJSON_GetArrayItem(entries, i);
                cJSON *eid = cJSON_GetObjectItem(e, "id");
                if (cJSON_IsString(eid) && strcmp(eid->valuestring, id) == 0) {
                    found = e;
                    break;
                }
            }
        }
        if (found) {
            cJSON_AddItemToObject(resp, "entry", cJSON_Duplicate(found, 1));
        } else {
            cJSON_Delete(resp);
            resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "status", "error");
            cJSON_AddStringToObject(resp, "message", "not found");
        }
    } else if (strcmp(cmd, "query") == 0) {
        cJSON *q_item = cJSON_GetObjectItem(root, "query");
        const char *q = cJSON_IsString(q_item) ? q_item->valuestring : "";
        cJSON *out = cJSON_CreateArray();
        if (cJSON_IsArray(entries)) {
            int n = cJSON_GetArraySize(entries);
            for (int i = 0; i < n; i++) {
                cJSON *e = cJSON_GetArrayItem(entries, i);
                cJSON *eid = cJSON_GetObjectItem(e, "id");
                cJSON *ename = cJSON_GetObjectItem(e, "name");
                if ((cJSON_IsString(eid) && strstr(eid->valuestring, q)) ||
                    (cJSON_IsString(ename) && strstr(ename->valuestring, q))) {
                    cJSON_AddItemToArray(out, cJSON_Duplicate(e, 1));
                }
            }
        }
        cJSON_AddItemToObject(resp, "entries", out);
    } else {
        cJSON_Delete(resp);
        resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "message", "unsupported command");
    }

    char *out_s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (out_s) {
        write(client_fd, out_s, strlen(out_s));
        write(client_fd, "\n", 1);
        free(out_s);
    }

    LOG_DEBUG_T("Lingosd", "RegistryCmd", "OK", "cmd='%s' handled", cmd);
    cJSON_Delete(reg);
    cJSON_Delete(root);
}

static void handle_command(int client_fd, const char *json_req) {
    LOG_DEBUG_T("Lingosd", "HandleCmd", "Enter", "client_fd=%d, json='%s'", client_fd, json_req ? json_req : "(null)");
    if (client_fd < 0 || !json_req) {
        LOG_ERROR_T("Lingosd", "HandleCmd", "Invalid", "client_fd=%d, json_req=%p", client_fd, (void*)json_req);
        return;
    }

    cJSON *json_root = cJSON_Parse(json_req);
    if (!json_root) {
        LOG_WARN_T("Lingosd", "HandleCmd", "ParseFail", "invalid JSON: %s", json_req);
        send_json(client_fd, "error", NULL, "invalid JSON");
        return;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(json_root, "cmd");
    if (!cJSON_IsString(cmd_item)) {
        LOG_WARN_T("Lingosd", "HandleCmd", "MissingCmd", "missing cmd field");
        cJSON_Delete(json_root);
        send_json(client_fd, "error", NULL, "missing cmd");
        return;
    }

    const char *cmd = cmd_item->valuestring;
    LOG_INFO_T("Lingosd", "HandleCmd", "Cmd", "processing command: %s", cmd);

    if (strcmp(cmd, "syscall") == 0) {
        cJSON *op_item = cJSON_GetObjectItem(json_root, "operation");
        cJSON *args_item = cJSON_GetObjectItem(json_root, "args");

        if (!cJSON_IsString(op_item)) {
            LOG_WARN_T("Lingosd", "HandleCmd", "Syscall", "missing operation");
            send_json(client_fd, "error", NULL, "missing operation");
            cJSON_Delete(json_root);
            return;
        }

        const char *operation = op_item->valuestring;
        const char *args_json = (cJSON_IsObject(args_item) || cJSON_IsString(args_item)) ?
                                 cJSON_PrintUnformatted(args_item) : "{}";
        char result[8192] = {0};
        int ret = handle_syscall(operation, args_json, result, sizeof(result));
        if (args_json && args_json[0] != '{' && args_json[0] != '\0') {
            free((void*)args_json);
        }
        write(client_fd, result, strlen(result));
        write(client_fd, "\n", 1);
        cJSON_Delete(json_root);
        return;
    }

    if (strcmp(cmd, "skill_schemas") == 0) {
        char *skill_json = read_skill_index();
        if (skill_json) {
            write(client_fd, "{\"status\":\"ok\",\"result\":", 31);
            write(client_fd, skill_json, strlen(skill_json));
            write(client_fd, "}\n", 2);
            free(skill_json);
        } else {
            send_json(client_fd, "error", NULL, "failed to load skills");
        }
        cJSON_Delete(json_root);
        return;
    }

    if (strcmp(cmd, "registry_list") == 0) {
        const char *data_root = lingos_data_root();
        char index_path[512];
        safe_snprintf(index_path, sizeof(index_path), "%s/registry/skills/index.json", data_root);
        FILE *fp = fopen(index_path, "r");
        if (!fp) {
            LOG_WARN_T("Lingosd", "HandleCmd", "RegistryList", "index.json not found");
            send_json(client_fd, "error", NULL, "registry index not found");
        } else {
            fseek(fp, 0, SEEK_END);
            long len = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            char *buf = malloc(len + 1);
            if (buf) {
                size_t read_len = fread(buf, 1, len, fp);
                buf[read_len] = '\0';
                fclose(fp);
                write(client_fd, "{\"status\":\"ok\",\"result\":", 31);
                write(client_fd, buf, strlen(buf));
                write(client_fd, "}\n", 2);
                free(buf);
            } else {
                LOG_ERROR_T("Lingosd", "HandleCmd", "RegistryList", "malloc failed");
                fclose(fp);
                send_json(client_fd, "error", NULL, "memory allocation failed");
            }
        }
        cJSON_Delete(json_root);
        return;
    }

    if (strcmp(cmd, "set_log_level") == 0) {
        cJSON *level_item = cJSON_GetObjectItem(json_root, "level");
        if (!cJSON_IsString(level_item)) {
            LOG_WARN_T("Lingosd", "HandleCmd", "SetLogLevel", "missing level string");
            send_json(client_fd, "error", NULL, "missing level");
            cJSON_Delete(json_root);
            return;
        }
        const char *level_str = level_item->valuestring;
        int level = log_level_from_string(level_str);
        if (level < 0) {
            LOG_WARN_T("Lingosd", "HandleCmd", "SetLogLevel", "invalid level '%s'", level_str);
            send_json(client_fd, "error", NULL, "invalid level (debug|info|warn|error)");
            cJSON_Delete(json_root);
            return;
        }
        log_set_global_level(level);
        LOG_INFO_T("Lingosd", "HandleCmd", "SetLogLevel", "C log level set to %d (%s)", level, level_str);

        cJSON *fwd_root = cJSON_CreateObject();
        cJSON_AddStringToObject(fwd_root, "cmd", "set_log_level");
        cJSON_AddStringToObject(fwd_root, "level", level_str);
        char *fwd_json = cJSON_PrintUnformatted(fwd_root);
        cJSON_Delete(fwd_root);

        if (fwd_json) {
            int ret = forward_to_python(client_fd, fwd_json);
            free(fwd_json);
            if (ret != 0) {
                LOG_ERROR_T("Lingosd", "HandleCmd", "SetLogLevel", "forward_to_python failed");
            }
        } else {
            LOG_ERROR_T("Lingosd", "HandleCmd", "SetLogLevel", "cJSON_PrintUnformatted failed");
            send_json(client_fd, "error", NULL, "Failed to construct forward request");
        }
        cJSON_Delete(json_root);
        return;
    }

    if (strcmp(cmd, "health") == 0) {
        cJSON *health = cJSON_CreateObject();
        cJSON_AddStringToObject(health, "status", "ok");
        cJSON_AddNumberToObject(health, "uptime", (double)time(NULL));
        char *health_str = cJSON_PrintUnformatted(health);
        cJSON_Delete(health);
        if (health_str) {
            write(client_fd, health_str, strlen(health_str));
            write(client_fd, "\n", 1);
            free(health_str);
        } else {
            send_json(client_fd, "ok", "text", "healthy");
        }
        cJSON_Delete(json_root);
        return;
    }

    if (strcmp(cmd, "audit_dump") == 0) {
        char buf[8192];
        audit_dump(buf, sizeof(buf));
        send_json(client_fd, "ok", "text", buf);
        cJSON_Delete(json_root);
        return;
    }

    if (strcmp(cmd, "perm_check") == 0) {
        cJSON *token_item = cJSON_GetObjectItem(json_root, "token");
        cJSON *perm_item = cJSON_GetObjectItem(json_root, "perm");
        if (!cJSON_IsString(token_item) || !cJSON_IsString(perm_item)) {
            LOG_WARN_T("Lingosd", "HandleCmd", "PermCheck", "missing token or perm");
            send_json(client_fd, "error", NULL, "missing token or perm");
            cJSON_Delete(json_root);
            return;
        }
        int allowed = permission_check_token(token_item->valuestring, perm_item->valuestring);
        send_json(client_fd, allowed ? "ok" : "denied", "text", allowed ? "allowed" : "denied");
        cJSON_Delete(json_root);
        return;
    }

    if (strcmp(cmd, "reload_config") == 0) {
        cJSON *category_item = cJSON_GetObjectItem(json_root, "category");
        const char *category = (cJSON_IsString(category_item)) ? category_item->valuestring : "all";
        cJSON *fwd_root = cJSON_CreateObject();
        cJSON_AddStringToObject(fwd_root, "cmd", "reload_config");
        cJSON_AddStringToObject(fwd_root, "category", category);
        char *fwd_json = cJSON_PrintUnformatted(fwd_root);
        cJSON_Delete(fwd_root);
        if (fwd_json) {
            int ret = forward_to_python(client_fd, fwd_json);
            free(fwd_json);
            if (ret != 0) {
                LOG_ERROR_T("Lingosd", "HandleCmd", "ReloadConfig", "forward_to_python failed");
            }
        } else {
            LOG_ERROR_T("Lingosd", "HandleCmd", "ReloadConfig", "cJSON_PrintUnformatted failed");
            send_json(client_fd, "error", NULL, "Failed to construct forward request");
        }
        cJSON_Delete(json_root);
        return;
    }

    if (strcmp(cmd, "sub_ai_status") == 0 ||
        strcmp(cmd, "sub_ai_notification") == 0 ||
        strcmp(cmd, "get_task_status") == 0) {
        int ret = forward_to_python(client_fd, json_req);
        if (ret != 0) {
            LOG_ERROR_T("Lingosd", "HandleCmd", "Forward", "forward_to_python failed");
        }
        cJSON_Delete(json_root);
        return;
    }

    if (strcmp(cmd, "shutdown") == 0) {
        LOG_WARN_T("Lingosd", "HandleCmd", "Shutdown", "shutdown command received");
        send_json(client_fd, "ok", "text", "shutting down");
        cJSON_Delete(json_root);
        close(client_fd);
        LOG_INFO_T("Lingosd", "HandleCmd", "Shutdown", "exiting");
        exit(0);
    }

    if (strcmp(cmd, "ping") == 0) {
        send_json(client_fd, "ok", "text", "pong");
        cJSON_Delete(json_root);
        return;
    }

    LOG_WARN_T("Lingosd", "HandleCmd", "Unknown", "unknown command: %s", cmd);
    send_json(client_fd, "error", NULL, "unknown command");
    cJSON_Delete(json_root);
    return;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    struct sockaddr_un addr;
    int server_fd, client_fd;
    char buf[BUF_SIZE];

    LOG_INFO_T("Lingosd", "Main", "Enter", "Starting LING OS Daemon");

    signal(SIGINT, cleanup_on_exit);
    signal(SIGTERM, cleanup_on_exit);
    signal(SIGHUP, cleanup_on_exit);

    if (daemon_init() != 0) {
        LOG_ERROR_T("Lingosd", "Main", "InitFail", "daemon_init failed");
        return 1;
    }

    unlink(SOCKET_PATH);
    unlink(PID_PATH);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_ERROR_T("Lingosd", "Main", "SocketFail", "socket() error: %s (errno=%d)", strerror(errno), errno);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR_T("Lingosd", "Main", "BindFail", "bind to %s failed: %s (errno=%d)", SOCKET_PATH, strerror(errno), errno);
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        LOG_ERROR_T("Lingosd", "Main", "ListenFail", "listen failed: %s (errno=%d)", strerror(errno), errno);
        close(server_fd);
        unlink(SOCKET_PATH);
        return 1;
    }

    FILE *fp = fopen(PID_PATH, "w");
    if (fp) {
        fprintf(fp, "%d\n", getpid());
        fclose(fp);
    } else {
        LOG_WARN_T("Lingosd", "Main", "PIDFail", "cannot write PID file %s: %s", PID_PATH, strerror(errno));
    }

    LOG_INFO_T("Lingosd", "Main", "Ready", "Daemon listening on %s (version LN-0.4.3)", SOCKET_PATH);

    /* 【新增】创建 registry.sock（供 Python skill_loader/registry_client 查询注册表） */
    int reg_fd = -1;
    const char *data_root = lingos_data_root();
    char reg_sock_path[512];
    safe_snprintf(reg_sock_path, sizeof(reg_sock_path), "%s/run/registry.sock", data_root);
    unlink(reg_sock_path);
    reg_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (reg_fd >= 0) {
        struct sockaddr_un raddr;
        memset(&raddr, 0, sizeof(raddr));
        raddr.sun_family = AF_UNIX;
        safe_strncpy(raddr.sun_path, reg_sock_path, sizeof(raddr.sun_path));
        raddr.sun_path[sizeof(raddr.sun_path)-1] = '\0';
        if (bind(reg_fd, (struct sockaddr*)&raddr, sizeof(raddr)) == 0 &&
            listen(reg_fd, 5) == 0) {
            LOG_INFO_T("Lingosd", "Main", "RegistryReady", "registry.sock listening on %s", reg_sock_path);
        } else {
            LOG_WARN_T("Lingosd", "Main", "RegistryBindFail", "registry.sock bind failed: %s", strerror(errno));
            close(reg_fd);
            reg_fd = -1;
        }
    }

    while (1) {
        struct pollfd pfds[2];
        pfds[0].fd = server_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = reg_fd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int pret = poll(pfds, 2, -1);
        if (pret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR_T("Lingosd", "Main", "PollFail", "poll error: %s (errno=%d)", strerror(errno), errno);
            break;
        }

        if (pfds[0].revents & POLLIN) {
            client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) continue;
            int ret = read_line(client_fd, buf, sizeof(buf));
            if (ret > 0) handle_command(client_fd, buf);
            close(client_fd);
        }

        if (reg_fd >= 0 && (pfds[1].revents & POLLIN)) {
            int rfd = accept(reg_fd, NULL, NULL);
            if (rfd < 0) continue;
            int ret = read_line(rfd, buf, sizeof(buf));
            if (ret > 0) handle_registry_command(rfd, buf);
            close(rfd);
        }
    }

    close(server_fd);
    if (reg_fd >= 0) close(reg_fd);
    unlink(SOCKET_PATH);
    unlink(reg_sock_path);
    unlink(PID_PATH);
    LOG_INFO_T("Lingosd", "Main", "Exit", "daemon stopped");
    return 0;
}