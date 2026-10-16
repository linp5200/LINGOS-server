/**
 * @file    src/api/control_url.c
 * @brief   Control URL 统一控制入口实现（禁止假成功）
 * @version LN-0.8.0
 *
 * 接线点（谁调用它）：
 *   · src/api/http_server.c:request_handler()  → GET/POST /api/control
 *   · src/api/api_core.c:api_handle_control()  → api_call("control", ...)
 *   · src/shell/shell.c:handle_builtin_command() → "control <url|动作>"
 */

#include "control_url.h"
#include "api_core.h"
#include "../common/safe_string.h"
#include "../lib/cJSON/cJSON.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CONTROL_ACTION_MAX 64
#define CONTROL_MAX_ACTIONS 32

/* 动作表：action（点分规范名 / 旧别名并存） → API 端点（api_core.c 已注册者为真实端点）
 *   旧别名（status / perm.list / fs.info / system.status ...）一律保留；
 *   点分规范名为第 4 期新增（先生决策 5）。 */
typedef struct {
    const char *action;     /* 控制面动作名 */
    const char *endpoint;   /* api_core 端点名；NULL = 无实现（诚实报错） */
    const char *note_en;
    const char *note_zh;
} control_action_t;

static const control_action_t g_control_actions[] = {
    /* —— 既有端点（旧名保留） —— */
    {"status",            "status",            "basic status",              "基础状态"},
    {"perm.list",         "perm.list",         "permission list endpoint",  "权限列表端点"},
    {"fs.info",           "fs.info",           "filesystem info",           "文件系统信息"},
    {"system.status",     "system.status",     "system status",             "系统状态"},
    {"system.metrics",    "system.metrics",    "system metrics",            "系统指标"},
    {"ai.status",         "ai.status",         "AI backend status",         "AI 后端状态"},
    /* —— 第 4 期新增点分规范名（映射到真实端点，不新增假处理器） —— */
    {"security.perm_list","perm.list",         "permission list (dotted)",  "权限列表（点分）"},
    {"system.metrics.full","system.metrics",   "system metrics (dotted)",   "系统指标（点分）"},
    {"skills.list",       "skills.list",       "skill catalog (real)",      "技能目录（真实数据）"},
    {"skill.list",        "skills.list",       "skill catalog (dotted)",    "技能目录（点分）"},
    {"skills.categories", "skills.categories", "skill categories",          "技能分类"},
    {"skill.categories",  "skills.categories", "skill categories (dotted)", "技能分类（点分）"},
    {"skills.search",     "skills.list",       "skill keyword search",      "技能关键词搜索"},
    {"skill.search",      "skills.list",       "skill keyword search (dotted)", "技能搜索（点分）"},
    /* —— 文档示例动作：服务端当前【无实现】——诚实报错，禁止假成功 —— */
    {"scene.apply",       NULL,                "declared in docs (IntentCP example); no scene subsystem in server C code yet",
                                               "文档示例动作；服务端 C 代码暂无场景子系统"},
    {NULL, NULL, NULL, NULL}
};

/* 动作名合法性：[A-Za-z0-9._-]{1,CONTROL_ACTION_MAX-1} */
static int control_action_valid(const char *action) {
    if (!action || !*action) return 0;
    size_t n = strlen(action);
    if (n >= CONTROL_ACTION_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)action[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static const control_action_t* control_find(const char *action) {
    if (!action) return NULL;
    for (int i = 0; g_control_actions[i].action; i++) {
        if (strcmp(g_control_actions[i].action, action) == 0) return &g_control_actions[i];
    }
    return NULL;
}

int control_url_parse(const char *url, char *action, size_t action_size,
                      char *query, size_t query_size) {
    if (!url || !action || action_size == 0) return -1;
    action[0] = '\0';
    if (query && query_size) query[0] = '\0';

    const char *p = url;
    while (*p == ' ' || *p == '\t') p++;
    /* 可选 scheme：lingos:// */
    if (strncmp(p, "lingos://", 9) == 0) {
        p += 9;
        /* 可选 host "control/" —— 规范形式；也接受 lingos://<action> 简写 */
        if (strncmp(p, "control/", 8) == 0) {
            p += 8;
        } else if (strcmp(p, "control") == 0) {
            return -1;   /* 只有 host 没有动作 */
        }
    } else if (strncmp(p, "control://", 10) == 0) {
        p += 10;
    }

    /* 动作段（到 '?' 或结尾） */
    size_t alen = 0;
    while (p[alen] && p[alen] != '?' && alen < action_size - 1) {
        action[alen] = p[alen];
        alen++;
    }
    action[alen] = '\0';
    if (!control_action_valid(action)) {
        LOG_WARN_T("ControlURL", "Parse", "BadAction", "url='%s'", url);
        return -1;
    }

    if (p[alen] == '?') {
        const char *q = p + alen + 1;
        if (query && query_size) {
            size_t qlen = strlen(q);
            if (qlen >= query_size) return -1;   /* 查询串过长 → 拒绝，不截断 */
            memcpy(query, q, qlen + 1);
        } else if (*q) {
            return -1;   /* 调用者不接收查询串却给了参数 */
        }
    }
    return 0;
}

/* URL 解码（%XX 与 '+'）；失败返回 -1 */
static int control_url_decode(const char *in, char *out, size_t out_size) {
    size_t oi = 0;
    for (size_t i = 0; in[i]; i++) {
        char c = in[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (!isxdigit((unsigned char)in[i + 1]) || !isxdigit((unsigned char)in[i + 2])) return -1;
            char hex[3] = { in[i + 1], in[i + 2], '\0' };
            c = (char)strtol(hex, NULL, 16);
            i += 2;
        }
        if (oi + 1 >= out_size) return -1;
        out[oi++] = c;
    }
    out[oi] = '\0';
    return 0;
}

/* 查询串 → args JSON 对象（cJSON 负责转义）；无参数时返回 "{}" */
static int control_query_to_json(const char *query, char *out, size_t out_size) {
    cJSON *args = cJSON_CreateObject();
    if (!args) return -1;

    if (query && *query) {
        char buf[1024];
        if (strlen(query) >= sizeof(buf)) { cJSON_Delete(args); return -1; }
        safe_strncpy(buf, query, sizeof(buf));
        char *save = NULL;
        for (char *tok = strtok_r(buf, "&", &save); tok; tok = strtok_r(NULL, "&", &save)) {
            char *eq = strchr(tok, '=');
            char kbuf[128], vbuf[512];
            if (eq) {
                *eq = '\0';
                if (control_url_decode(tok, kbuf, sizeof(kbuf)) != 0) { cJSON_Delete(args); return -1; }
                if (control_url_decode(eq + 1, vbuf, sizeof(vbuf)) != 0) { cJSON_Delete(args); return -1; }
                /* 类型推断：true/false → bool；整数 → number；其余 → string */
                if (strcmp(vbuf, "true") == 0) cJSON_AddBoolToObject(args, kbuf, 1);
                else if (strcmp(vbuf, "false") == 0) cJSON_AddBoolToObject(args, kbuf, 0);
                else {
                    char *endp = NULL;
                    long num = strtol(vbuf, &endp, 10);
                    if (endp && *endp == '\0' && endp != vbuf) cJSON_AddNumberToObject(args, kbuf, (double)num);
                    else cJSON_AddStringToObject(args, kbuf, vbuf);
                }
            } else {
                if (control_url_decode(tok, kbuf, sizeof(kbuf)) != 0) { cJSON_Delete(args); return -1; }
                cJSON_AddBoolToObject(args, kbuf, 1);   /* 无 '=' → 开关参数 */
            }
        }
    }

    char *s = cJSON_PrintUnformatted(args);
    cJSON_Delete(args);
    if (!s) return -1;
    size_t len = strlen(s);
    if (len + 1 > out_size) { free(s); return -1; }
    memcpy(out, s, len + 1);
    free(s);
    return 0;
}

int control_url_list_actions(char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return -1;
    int n = 0;
    for (int i = 0; g_control_actions[i].action; i++) {
        cJSON *it = cJSON_CreateObject();
        if (!it) continue;
        cJSON_AddStringToObject(it, "action", g_control_actions[i].action);
        if (g_control_actions[i].endpoint) {
            cJSON_AddStringToObject(it, "endpoint", g_control_actions[i].endpoint);
            cJSON_AddBoolToObject(it, "implemented", 1);
        } else {
            cJSON_AddBoolToObject(it, "implemented", 0);
            cJSON_AddStringToObject(it, "note", g_control_actions[i].note_en);
        }
        cJSON_AddItemToArray(arr, it);
        n++;
    }
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!s) return -1;
    size_t len = strlen(s);
    if (len + 1 > out_size) { free(s); return -1; }
    memcpy(out, s, len + 1);
    free(s);
    return n;
}

/* 输出诚实错误信封（code 404/501 等） */
static int control_error(char *resp, size_t resp_size, const char *id,
                         int code, const char *message, const char *action) {
    cJSON *env = cJSON_CreateObject();
    if (!env) return -1;
    if (id && *id) cJSON_AddStringToObject(env, "id", id);
    cJSON_AddNumberToObject(env, "code", code);
    cJSON_AddStringToObject(env, "message", message);
    if (action && *action) cJSON_AddStringToObject(env, "action", action);
    char actions[4096];
    if (control_url_list_actions(actions, sizeof(actions)) > 0) {
        cJSON *a = cJSON_Parse(actions);
        if (a) cJSON_AddItemToObject(env, "available_actions", a);
    }
    char *s = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
    if (!s) return -1;
    size_t len = strlen(s);
    if (len + 1 > resp_size) { free(s); return -1; }
    memcpy(resp, s, len + 1);
    free(s);
    return 0;
}

int control_url_execute(const char *url, const char *id, char *resp, size_t resp_size) {
    if (!resp || resp_size == 0) return -1;
    resp[0] = '\0';
    if (!url || !*url) return control_error(resp, resp_size, id, 400, "missing control url", NULL);

    char action[CONTROL_ACTION_MAX];
    char query[1024];
    if (control_url_parse(url, action, sizeof(action), query, sizeof(query)) != 0) {
        return control_error(resp, resp_size, id, 400,
                             "invalid control url (expect lingos://control/<system.action>?k=v)", NULL);
    }

    char args[1024];
    if (control_query_to_json(query, args, sizeof(args)) != 0) {
        return control_error(resp, resp_size, id, 400, "invalid control query string", action);
    }
    return control_url_execute_action(action, args, id, resp, resp_size);
}

int control_url_execute_action(const char *action, const char *args_json,
                               const char *id, char *resp, size_t resp_size) {
    if (!resp || resp_size == 0) return -1;
    resp[0] = '\0';
    if (!control_action_valid(action)) {
        return control_error(resp, resp_size, id, 400, "invalid control action name", NULL);
    }

    const control_action_t *ca = control_find(action);
    if (!ca) {
        char msg[256];
        safe_snprintf(msg, sizeof(msg), "unknown control action: %s", action);
        LOG_WARN_T("ControlURL", "Execute", "UnknownAction", "action=%s", action);
        return control_error(resp, resp_size, id, 404, msg, action);
    }
    if (!ca->endpoint) {
        /* 文档已声明但服务端无实现 → 诚实 501，禁止假成功 */
        char msg[384];
        safe_snprintf(msg, sizeof(msg),
                      "control action '%s' is declared but NOT implemented in server C code (no fabricated success)",
                      action);
        LOG_WARN_T("ControlURL", "Execute", "NotImplemented", "action=%s", action);
        return control_error(resp, resp_size, id, 501, msg, action);
    }

    /* —— 真实调用：统一端点注册表（api_core.c） —— */
    char data[4096];
    int rc = api_call(ca->endpoint, (args_json && *args_json) ? args_json : "{}", data, sizeof(data));
    if (rc != 0) {
        char msg[256];
        safe_snprintf(msg, sizeof(msg), "endpoint '%s' not available (api_call failed)", ca->endpoint);
        LOG_WARN_T("ControlURL", "Execute", "CallFail", "action=%s endpoint=%s", action, ca->endpoint);
        return control_error(resp, resp_size, id, 503, msg, action);
    }

    cJSON *env = cJSON_CreateObject();
    if (!env) return -1;
    if (id && *id) cJSON_AddStringToObject(env, "id", id);
    cJSON_AddNumberToObject(env, "code", 0);
    cJSON_AddStringToObject(env, "message", "success");
    cJSON_AddStringToObject(env, "action", action);
    cJSON_AddStringToObject(env, "endpoint", ca->endpoint);
    cJSON *data_json = cJSON_Parse(data);
    if (data_json) cJSON_AddItemToObject(env, "data", data_json);
    else cJSON_AddStringToObject(env, "data", data);   /* 非 JSON 输出 → 原样透传（不伪造） */

    char *s = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
    if (!s) return -1;
    size_t len = strlen(s);
    if (len + 1 > resp_size) {
        LOG_WARN_T("ControlURL", "Execute", "RespTooSmall", "need=%zu have=%zu", len + 1, resp_size);
        free(s);
        return -1;
    }
    memcpy(resp, s, len + 1);
    free(s);
    LOG_INFO_T("ControlURL", "Execute", "OK", "action=%s endpoint=%s", action, ca->endpoint);
    return 0;
}
