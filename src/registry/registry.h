/**
 * @file    registry.h
 * @brief   注册表核心数据结构和 API 声明
 * @version LN-B-5.0.0.0
 */

#ifndef REGISTRY_H
#define REGISTRY_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 注册类型
 * ============================================================ */

typedef enum {
    REG_TYPE_MODULE = 0,
    REG_TYPE_COMPONENT,
    REG_TYPE_CONFIG,
    REG_TYPE_FEATURE,
    REG_TYPE_SKILL,
    REG_TYPE_PLUGIN,
    REG_TYPE_HOOK,
    REG_TYPE_SELFCHECK
} registry_type_t;

/* ============================================================
 * 注册状态
 * ============================================================ */

typedef enum {
    REG_STATUS_INACTIVE = 0,
    REG_STATUS_ACTIVE,
    REG_STATUS_DEPRECATED,
    REG_STATUS_ERROR
} registry_status_t;

/* ============================================================
 * 注册项结构
 * ============================================================ */

typedef struct registry_entry {
    char id[128];               /* 唯一标识，如 "module:vision" */
    registry_type_t type;
    char name[64];
    char version[32];
    registry_status_t status;
    char path[512];             /* 关联文件路径（可选） */
    void *metadata;             /* cJSON 指针，存储额外元数据 */
    time_t created_at;
    time_t updated_at;
} registry_entry_t;

/* ============================================================
 * 核心 API
 * ============================================================ */

/**
 * @brief 初始化注册表系统（创建目录，加载主索引）
 * @return 0 成功，-1 失败
 */
int registry_init(void);

/**
 * @brief 注册一个新条目
 * @param entry 条目结构（必须包含 id 和 type）
 * @return 0 成功，-1 失败
 */
int registry_register(const registry_entry_t *entry);

/**
 * @brief 注销条目（按 id）
 * @param id 条目唯一标识
 * @return 0 成功，-1 失败
 */
int registry_unregister(const char *id);

/**
 * @brief 更新条目
 * @param id 条目唯一标识
 * @param entry 新条目内容
 * @return 0 成功，-1 失败
 */
int registry_update(const char *id, const registry_entry_t *entry);

/**
 * @brief 根据 id 获取条目
 * @param id 条目唯一标识
 * @return 条目指针（静态分配，调用者不应释放），未找到返回 NULL
 */
const registry_entry_t* registry_get(const char *id);

/**
 * @brief 按类型列出所有条目
 * @param type 注册类型，若为 -1 则列出所有
 * @param out 输出数组（调用者分配）
 * @param max_count 最大数量
 * @return 实际数量
 */
int registry_list(int type, registry_entry_t **out, int max_count);

/**
 * @brief 查询条目（按名称或 id 模糊匹配）
 * @param query 查询字符串
 * @param out 输出数组
 * @param max_count 最大数量
 * @return 实际数量
 */
int registry_query(const char *query, registry_entry_t **out, int max_count);

/**
 * @brief 持久化保存注册表
 * @return 0 成功，-1 失败
 */
int registry_save(void);

/**
 * @brief 从持久化存储加载注册表
 * @return 0 成功，-1 失败
 */
int registry_load(void);

/**
 * @brief 热重载注册表
 * @return 0 成功，-1 失败
 */
int registry_reload(void);

/**
 * @brief 注册变更回调
 * @param cb 回调函数，参数 (id, action, user_data)
 * @param user_data 用户数据
 * @return 0 成功，-1 失败
 */
typedef void (*registry_change_cb)(const char *id, int action, void *user_data);
int registry_on_change(registry_change_cb cb, void *user_data);

/* ============================================================
 * 自检集成 API
 * ============================================================ */

/**
 * @brief 获取所有自检回调列表
 * @param out 输出数组
 * @param max_count 最大数量
 * @return 实际数量
 */
int registry_get_selfcheck_list(registry_entry_t **out, int max_count);

/**
 * @brief 运行指定模块的自检回调
 * @param module_name 模块名称（如 "vision"）
 * @return 0 成功，-1 失败
 */
int registry_run_selfcheck(const char *module_name);

int registry_run_all_selfchecks(void);
#ifdef __cplusplus
}
#endif

#endif /* REGISTRY_H */