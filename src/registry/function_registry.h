/**
 * @file    src/registry/function_registry.h
 * @brief   统一调用 API · 通用函数注册表 · rely 依赖控制
 * @version LN-0.9.0
 * @par     先生 2026-09-04 方案（camera_backend 20 章 §5 通用调用 API）：
 *           请求 {id, command, args, rely, return} → 响应 {id, code, message, data}
 *           · command = 点分路径，对应注册表中的函数
 *           · rely（默认 true）：false = 只执行当前函数，跳过其"依赖业务调用"
 *           · 批量：{"commands":[{command,args,rely}, ...]} → data 为结果数组
 * @par     依赖控制机制（§5.3）：
 *           业务函数内部调用其他业务函数的位置用 API_CALL_DEPENDENCY /
 *           API_DEPENDENCY_BEGIN..END 包装；基础函数（日志/内存/锁）不包装。
 *           rely=false 时执行期置线程局部 suppress_dependencies=1 →
 *           被包装的调用点跳过并记调试日志。
 * @note    旧 v3 命令（{"cmd":...}）**不改造**——本模块是其并肩的新体系底座。
 */

#ifndef FUNCTION_REGISTRY_H
#define FUNCTION_REGISTRY_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 错误码（先生 §5.2 错误码表：0 = 成功，非 0 = 错误）
 * ============================================================ */
#define API_ERR_OK            0
#define API_ERR_INVALID_ARG  -1001   /* 参数错误 */
#define API_ERR_NOT_FOUND    -1002   /* 命令未注册 */
#define API_ERR_EXEC_FAIL    -2001   /* 执行失败（函数内部错误） */
#define API_ERR_INTERNAL     -2002   /* 内部错误 */
#define API_ERR_DEPTH        -2003   /* 调用深度超限 */

#define API_MAX_FUNCTIONS    128
#define API_MAX_NAME         64
#define API_MAX_DESC         128
#define API_MAX_SCHEMA_STR   512
#define API_CALL_MAX_DEPTH   8

/* ============================================================
 * 注册表条目
 * ============================================================ */
typedef int (*api_function_t)(cJSON *args, cJSON **result, void *userdata);

typedef struct api_function_entry {
    char name[API_MAX_NAME];                 /* 点分命令名：camera.set_ir */
    char description[API_MAX_DESC];
    char schema[API_MAX_SCHEMA_STR];         /* 参数 schema（JSON 字符串，可空） */
    api_function_t fn;
    void *userdata;
    unsigned long call_count;
    unsigned long error_count;
} api_function_entry_t;

/* ============================================================
 * rely 依赖控制（线程局部）
 * ============================================================ */
void api_dependency_suppress(int on);            /* 1=抑制依赖业务调用，0=恢复正常 */
int  api_dependency_suppressed(void);
unsigned long api_dependency_suppressed_count(void);   /* 累计跳过次数（观测用） */
void api_dependency_note_skip(const char *file, int line, const char *what);

/* 包装"依赖业务调用点"：被抑制时不执行 call，返回 0（并记调试日志） */
#define API_CALL_DEPENDENCY(call) \
    (api_dependency_suppressed() \
        ? (api_dependency_note_skip(__FILE__, __LINE__, #call), 0) \
        : (call))

/* 指针返回值版本（调用方负责类型转换） */
#define API_CALL_DEPENDENCY_PTR(call) \
    (api_dependency_suppressed() \
        ? (api_dependency_note_skip(__FILE__, __LINE__, #call), (void *)0) \
        : (void *)(call))

/* 语句块版本（推荐）——块内为依赖业务逻辑 */
#define API_DEPENDENCY_BEGIN \
    if (!api_dependency_suppressed()) {
#define API_DEPENDENCY_END \
    } else { api_dependency_note_skip(__FILE__, __LINE__, "dependency-block"); }

/* ============================================================
 * 注册表 API
 * ============================================================ */
int  api_function_registry_init(void);
int  api_register_function(const char *name, api_function_t fn, void *userdata,
                           const char *description, const char *schema);
int  api_unregister_function(const char *name);
api_function_entry_t* api_find_function(const char *name);
int  api_function_count(void);
cJSON* api_describe_commands(const char *filter);   /* 新分配数组，调用者 cJSON_Delete */
cJSON* api_openapi_spec(void);                      /* OpenAPI 3.0 描述（新分配对象） */

/* ============================================================
 * 统一调用 API
 * ============================================================ */
int api_execute_function(const char *command, cJSON *args, int rely, cJSON **result);
int api_handle_request(const cJSON *request, cJSON **response);
const char* api_code_message(int code);

/* 注册内置真实命令（lingosd 启动时调用；全部为已存在功能，无假数据） */
void api_register_builtin_functions(void);

#ifdef __cplusplus
}
#endif

#endif /* FUNCTION_REGISTRY_H */
