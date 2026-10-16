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
#include "cJSON.h"   /* 自检报告返回 cJSON*（【2026-10-11】） */

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
 * 技能注册（registry_skill.c——【0.6.0】接线：加载 + 索引导出）
 * ============================================================ */

/**
 * @brief 加载所有技能目录（builtin + custom + store）到注册表
 *        并同步导出技能索引（/LINGOS/registry/skills/index.json）
 * @return 加载的技能数（>=0）
 */
int registry_skill_load_all(void);

/**
 * @brief 导出技能索引到 /LINGOS/registry/skills/index.json
 *        合并「内存注册表 type=4」+「registry.json 磁盘条目」，供
 *        lingosd registry_list 与 Python 文件回退统一读取
 * @return 写入的技能数（>=0），-1 失败
 */
int registry_skill_write_index(void);

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

/* 【2026-10-11 第 4 期】自检实装（此前 registry_run_selfcheck 是"not yet implemented"桩，
 * registry_register_selfcheck 无任何调用者 = 典型"造好没接线"） */
int registry_register_builtin_selfchecks(void);   /* 注册内置真实自检项（幂等） */
int registry_selfcheck_run_module(const char *module);   /* 只跑某模块的自检 */
cJSON* registry_selfcheck_report(void);           /* 全量自检报告（新分配对象） */

int registry_run_all_selfchecks(void);

/* ============================================================
 * 【2026-10-16 第 4 期收尾】类目 ↔ 系统 ↔ 命令前缀
 *   单一事实源 = registry_builtin.c 的 BUILTIN 表（对齐
 *   docs/ARCHITECTURE_系统类目.md：类目名=功能词，命令前缀=系统名）。
 *   不做物理目录搬迁（先生令）——只落行为层一致性与可见性。
 * ============================================================ */

/** @brief 内置系统总数（文档 16 + 扩展） */
int registry_builtin_system_count(void);

/** @brief 取第 idx 个内置系统信息（任一输出参数可为 NULL）；0 成功，-1 越界 */
int registry_builtin_system_at(int idx, const char **id, const char **name,
                               const char **category, const char **prefix, const char **desc);

/** @brief 命令前缀是否为已登记系统（点分命令一致性校验用）；1 是，0 否 */
int registry_builtin_prefix_known(const char *prefix);

/** @brief 系统名 → 类目（未登记返回 NULL） */
const char* registry_builtin_category_of(const char *system_id);

/** @brief 真实一致性校验（前缀=系统名/唯一性/类目合法/运行期注册完整/命令面前缀归属）
 *  @return 违规项数（0 = 全部通过）；报告文本写入 out（含中英双语） */
int registry_builtin_consistency_report(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* REGISTRY_H */