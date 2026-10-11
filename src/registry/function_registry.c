/**
 * @file    src/registry/function_registry.c
 * @brief   统一调用 API + rely 依赖控制 实现（先生 2026-09-04 §5 方案）
 * @version LN-0.9.0
 * @par     设计要点：
 *           ① 注册表：点分命令名 → 函数指针（同名重复注册 = 覆盖，插件热重载友好）
 *           ② rely：rely=false → 执行期抑制"依赖业务调用"（线程局部，可嵌套）
 *           ③ 请求/响应严格按先生格式：{id,command,args,rely,return} →
 *              {id,code,message,data}；支持 "commands" 批量
 *           ④ 观测：每命令调用/错误计数、被抑制跳过次数（api.describe 可查）
 * @note    旧 {"cmd":...} 协议零改动——本模块只在出现 "command" 字段时接管。
 */

#include "function_registry.h"
#include "../lib/log_extra.h"
#include "../common/safe_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * 依赖控制状态
 * ============================================================ */
static __thread int t_suppress_dependencies = 0;   /* 线程局部（先生 §5.3） */
static __thread int t_call_depth = 0;
static unsigned long g_suppressed_skips = 0;       /* 全局观测计数 */

void api_dependency_suppress(int on) {
    t_suppress_dependencies = on ? 1 : 0;
}

int api_dependency_suppressed(void) {
    return t_suppress_dependencies;
}

unsigned long api_dependency_suppressed_count(void) {
    return g_suppressed_skips;
}

void api_dependency_note_skip(const char *file, int line, const char *what) {
    g_suppressed_skips++;
    LOG_DEBUG_T("ApiRely", "Suppress", "Skip", "%s:%d skipped '%s' (rely=false)",
                file ? file : "?", line, what ? what : "?");
}

/* ============================================================
 * 注册表
 * ============================================================ */
static api_function_entry_t g_functions[API_MAX_FUNCTIONS];
static int g_function_count = 0;
static int g_registry_ready = 0;

int api_function_registry_init(void) {
    memset(g_functions, 0, sizeof(g_functions));
    g_function_count = 0;
    g_registry_ready = 1;
    LOG_INFO_T("ApiRegistry", "Init", "OK", "unified call registry ready (max %d)", API_MAX_FUNCTIONS);
    return API_ERR_OK;
}

int api_register_function(const char *name, api_function_t fn, void *userdata,
                          const char *description, const char *schema) {
    int i;
    if (!name || !*name || !fn) return API_ERR_INVALID_ARG;
    if (!g_registry_ready) api_function_registry_init();

    for (i = 0; i < g_function_count; i++) {
        if (strcmp(g_functions[i].name, name) == 0) {
            g_functions[i].fn = fn;
            g_functions[i].userdata = userdata;
            safe_strncpy(g_functions[i].description, description ? description : "", sizeof(g_functions[i].description));
            safe_strncpy(g_functions[i].schema, schema ? schema : "", sizeof(g_functions[i].schema));
            LOG_INFO_T("ApiRegistry", "Register", "Replace", "command '%s' re-registered", name);
            return API_ERR_OK;
        }
    }
    if (g_function_count >= API_MAX_FUNCTIONS) {
        LOG_ERROR_T("ApiRegistry", "Register", "Full", "registry full (%d), '%s' rejected",
                    API_MAX_FUNCTIONS, name);
        return API_ERR_INTERNAL;
    }
    safe_strncpy(g_functions[g_function_count].name, name, sizeof(g_functions[g_function_count].name));
    safe_strncpy(g_functions[g_function_count].description, description ? description : "",
                 sizeof(g_functions[g_function_count].description));
    safe_strncpy(g_functions[g_function_count].schema, schema ? schema : "",
                 sizeof(g_functions[g_function_count].schema));
    g_functions[g_function_count].fn = fn;
    g_functions[g_function_count].userdata = userdata;
    g_functions[g_function_count].call_count = 0;
    g_functions[g_function_count].error_count = 0;
    g_function_count++;
    LOG_INFO_T("ApiRegistry", "Register", "OK", "command '%s' registered", name);
    return API_ERR_OK;
}

int api_unregister_function(const char *name) {
    int i;
    if (!name) return API_ERR_INVALID_ARG;
    for (i = 0; i < g_function_count; i++) {
        if (strcmp(g_functions[i].name, name) == 0) {
            int j;
            for (j = i; j < g_function_count - 1; j++) g_functions[j] = g_functions[j + 1];
            g_function_count--;
            memset(&g_functions[g_function_count], 0, sizeof(g_functions[0]));
            LOG_INFO_T("ApiRegistry", "Unregister", "OK", "command '%s' removed", name);
            return API_ERR_OK;
        }
    }
    return API_ERR_NOT_FOUND;
}

api_function_entry_t* api_find_function(const char *name) {
    int i;
    if (!name || !*name) return NULL;
    for (i = 0; i < g_function_count; i++) {
        if (strcmp(g_functions[i].name, name) == 0) return &g_functions[i];
    }
    return NULL;
}

int api_function_count(void) {
    return g_function_count;
}

cJSON* api_describe_commands(const char *filter) {
    cJSON *arr = cJSON_CreateArray();
    int i;
    if (!arr) return NULL;
    for (i = 0; i < g_function_count; i++) {
        cJSON *o;
        if (filter && *filter && !strstr(g_functions[i].name, filter)) continue;
        o = cJSON_CreateObject();
        if (!o) continue;
        cJSON_AddStringToObject(o, "command", g_functions[i].name);
        cJSON_AddStringToObject(o, "description", g_functions[i].description);
        if (g_functions[i].schema[0] != '\0') {
            cJSON *sc = cJSON_Parse(g_functions[i].schema);
            if (sc) cJSON_AddItemToObject(o, "schema", sc);
            else cJSON_AddStringToObject(o, "schema_raw", g_functions[i].schema);
        }
        cJSON_AddNumberToObject(o, "calls", (double)g_functions[i].call_count);
        cJSON_AddNumberToObject(o, "errors", (double)g_functions[i].error_count);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

/* ============================================================
 * OpenAPI 3.0 描述生成（先生 P5：OpenAPI + SDK + CLI）
 *   —— 由注册表实时生成，命令增删即反映到文档；schemas 取自各命令的 schema 字符串
 * ============================================================ */
cJSON* api_openapi_spec(void) {
    cJSON *doc = cJSON_CreateObject();
    cJSON *paths;
    cJSON *components, *schemas, *envelope;
    int i;
    if (!doc) return NULL;

    cJSON_AddStringToObject(doc, "openapi", "3.0.3");
    {
        cJSON *info = cJSON_CreateObject();
        cJSON_AddStringToObject(info, "title", "LINGOS Unified API");
        cJSON_AddStringToObject(info, "version", "1.0.0");
        cJSON_AddStringToObject(info, "description",
            "LINGOS 统一调用 API（先生 2026-09-04 方案）：POST {id,command,args,rely,return} -> {id,code,message,data}");
        cJSON_AddItemToObject(doc, "info", info);
    }
    paths = cJSON_CreateArray();
    for (i = 0; i < g_function_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON *args_schema = NULL;
        cJSON_AddStringToObject(item, "command", g_functions[i].name);
        cJSON_AddStringToObject(item, "description", g_functions[i].description);
        if (g_functions[i].schema[0] != '\0') {
            args_schema = cJSON_Parse(g_functions[i].schema);
        }
        cJSON_AddItemToObject(item, "args_schema",
                              args_schema ? args_schema : cJSON_CreateObject());
        cJSON_AddNumberToObject(item, "calls", (double)g_functions[i].call_count);
        cJSON_AddNumberToObject(item, "errors", (double)g_functions[i].error_count);
        cJSON_AddItemToArray(paths, item);
    }
    cJSON_AddItemToObject(doc, "commands", paths);

    components = cJSON_CreateObject();
    schemas = cJSON_CreateObject();
    envelope = cJSON_CreateObject();
    cJSON_AddStringToObject(envelope, "type", "object");
    {
        cJSON *props = cJSON_CreateObject();
        cJSON *idp = cJSON_CreateObject();
        cJSON_AddStringToObject(idp, "description", "客户端请求 ID（原样回显）");
        cJSON_AddItemToObject(props, "id", idp);
        {
            cJSON *codep = cJSON_CreateObject();
            cJSON_AddStringToObject(codep, "type", "integer");
            cJSON_AddStringToObject(codep, "description", "0=成功；-1001 参数；-1002 未注册；-2001 执行失败；-2002 内部；-2003 深度超限");
            cJSON_AddItemToObject(props, "code", codep);
        }
        {
            cJSON *msgp = cJSON_CreateObject();
            cJSON_AddStringToObject(msgp, "type", "string");
            cJSON_AddItemToObject(props, "message", msgp);
        }
        {
            cJSON *datap = cJSON_CreateObject();
            cJSON_AddStringToObject(datap, "description", "命令返回值（return=none 时省略）");
            cJSON_AddItemToObject(props, "data", datap);
        }
        cJSON_AddItemToObject(envelope, "properties", props);
    }
    cJSON_AddItemToObject(schemas, "Envelope", envelope);
    cJSON_AddItemToObject(components, "schemas", schemas);
    cJSON_AddItemToObject(doc, "components", components);
    return doc;
}

/* ============================================================
 * 统一调用
 * ============================================================ */
const char* api_code_message(int code) {
    switch (code) {
        case API_ERR_OK:          return "success";
        case API_ERR_INVALID_ARG: return "invalid argument";
        case API_ERR_NOT_FOUND:   return "command not registered";
        case API_ERR_EXEC_FAIL:   return "execution failed";
        case API_ERR_DEPTH:       return "call depth exceeded";
        case API_ERR_INTERNAL:
        default:                  return "internal error";
    }
}

int api_execute_function(const char *command, cJSON *args, int rely, cJSON **result) {
    api_function_entry_t *entry;
    int prev_suppress;
    int rc;

    if (result) *result = NULL;
    if (!command || !*command) return API_ERR_INVALID_ARG;
    if (t_call_depth >= API_CALL_MAX_DEPTH) {
        LOG_WARN_T("ApiRegistry", "Execute", "Depth", "'%s' rejected: depth %d >= %d",
                   command, t_call_depth, API_CALL_MAX_DEPTH);
        return API_ERR_DEPTH;
    }
    entry = api_find_function(command);
    if (!entry || !entry->fn) return API_ERR_NOT_FOUND;

    entry->call_count++;
    prev_suppress = t_suppress_dependencies;
    /* rely=false → 抑制依赖业务调用；rely=true → 保持外层状态（支持嵌套） */
    t_suppress_dependencies = rely ? prev_suppress : 1;
    t_call_depth++;
    rc = entry->fn(args, result, entry->userdata);
    t_call_depth--;
    t_suppress_dependencies = prev_suppress;
    if (rc != API_ERR_OK) entry->error_count++;
    LOG_DEBUG_T("ApiRegistry", "Execute", "Done", "'%s' rely=%d rc=%d", command, rely, rc);
    return rc;
}

/* 单条请求 → 响应对象（内部复用；batch 走同一路径） */
static int api_handle_single(const cJSON *request, cJSON **response) {
    cJSON *resp, *item;
    cJSON *data = NULL;
    const char *message = "success";
    char msgbuf[160];
    int code = API_ERR_OK;

    if (!request || !response) return API_ERR_INVALID_ARG;
    *response = NULL;
    resp = cJSON_CreateObject();
    if (!resp) return API_ERR_INTERNAL;

    item = cJSON_GetObjectItem((cJSON *)request, "id");
    if (item) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(item, 1));

    item = cJSON_GetObjectItem((cJSON *)request, "command");
    if (!cJSON_IsString(item) || item->valuestring[0] == '\0') {
        code = API_ERR_INVALID_ARG;
        message = "missing or empty 'command'";
    } else {
        const char *command = item->valuestring;
        cJSON *args = cJSON_GetObjectItem((cJSON *)request, "args");
        cJSON *rely_item = cJSON_GetObjectItem((cJSON *)request, "rely");
        cJSON *ret_item = cJSON_GetObjectItem((cJSON *)request, "return");
        int rely = cJSON_IsBool(rely_item) ? (cJSON_IsTrue(rely_item) ? 1 : 0) : 1;   /* 默认 true */
        int want_none = (cJSON_IsString(ret_item) && strcmp(ret_item->valuestring, "none") == 0);

        code = api_execute_function(command,
                                    cJSON_IsObject(args) ? args : NULL,
                                    rely, &data);
        message = api_code_message(code);
        if (code == API_ERR_NOT_FOUND) {
            safe_snprintf(msgbuf, sizeof(msgbuf), "command not registered: %s", command);
            message = msgbuf;
        }
        if (want_none && data) {
            cJSON_Delete(data);
            data = NULL;
        }
    }

    cJSON_AddNumberToObject(resp, "code", code);
    cJSON_AddStringToObject(resp, "message", message);
    if (data) cJSON_AddItemToObject(resp, "data", data);
    *response = resp;
    return code;
}

int api_handle_request(const cJSON *request, cJSON **response) {
    cJSON *batch, *resp;
    int ret;

    if (!request || !response) return API_ERR_INVALID_ARG;
    *response = NULL;

    batch = cJSON_GetObjectItem((cJSON *)request, "commands");
    if (cJSON_IsArray(batch)) {
        /* 批量：{"commands":[{command,args,rely}, ...]} → data = 结果数组 */
        cJSON *arr = cJSON_CreateArray();
        int n = cJSON_GetArraySize(batch);
        int i;
        if (!arr) return API_ERR_INTERNAL;
        for (i = 0; i < n; i++) {
            cJSON *sub_req = cJSON_GetArrayItem(batch, i);
            cJSON *sub_resp = NULL;
            if (!cJSON_IsObject(sub_req)) continue;
            (void)api_handle_single(sub_req, &sub_resp);
            if (sub_resp) cJSON_AddItemToArray(arr, sub_resp);
        }
        resp = cJSON_CreateObject();
        if (!resp) { cJSON_Delete(arr); return API_ERR_INTERNAL; }
        {
            cJSON *id_item = cJSON_GetObjectItem((cJSON *)request, "id");
            if (id_item) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id_item, 1));
        }
        cJSON_AddNumberToObject(resp, "code", API_ERR_OK);
        cJSON_AddStringToObject(resp, "message", "success");
        cJSON_AddItemToObject(resp, "data", arr);
        *response = resp;
        return API_ERR_OK;
    }

    ret = api_handle_single(request, &resp);
    *response = resp;
    return ret;
}
