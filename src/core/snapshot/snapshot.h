/**
 * @file    snapshot.h
 * @brief   系统快照接口
 * @version LN-B-4.2.0.0
 */

#ifndef CORE_SNAPSHOT_SNAPSHOT_H
#define CORE_SNAPSHOT_SNAPSHOT_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * 快照信息结构
 * ============================================================ */

typedef struct {
    char id[64];                /* 快照ID (snapshot_YYYYMMDD_HHMMSS) */
    char name[128];             /* 用户自定义名称 */
    char description[256];      /* 描述 */
    int64_t created_at;         /* 创建时间戳 */
    int64_t size_bytes;         /* 大小（字节） */
    char version[32];           /* 系统版本 */
} snapshot_info_t;

/* ============================================================
 * 公共 API
 * ============================================================ */

/**
 * @brief 初始化快照系统
 * @return 0 成功，-1 失败
 */
int snapshot_init(void);

/**
 * @brief 创建快照
 * @param name 快照名称（可选，NULL 使用自动生成）
 * @param description 描述（可选）
 * @param out_id 输出快照ID
 * @param out_id_len 输出缓冲区大小
 * @return 0 成功，-1 失败
 */
int snapshot_create(const char *name, const char *description, char *out_id, size_t out_id_len);

/**
 * @brief 恢复快照
 * @param id 快照ID
 * @return 0 成功，-1 失败
 */
int snapshot_restore(const char *id);

/**
 * @brief 删除快照
 * @param id 快照ID
 * @return 0 成功，-1 失败
 */
int snapshot_delete(const char *id);

/**
 * @brief 列出所有快照
 * @param out 输出数组
 * @param max_count 最大数量
 * @return 实际数量
 */
int snapshot_list(snapshot_info_t *out, int max_count);

/**
 * @brief 比较快照与当前系统差异
 * @param id 快照ID
 * @param out 输出缓冲区
 * @param out_len 缓冲区大小
 * @return 0 成功，-1 失败
 */
int snapshot_diff(const char *id, char *out, size_t out_len);

/**
 * @brief 获取快照存储路径
 * @return 路径字符串
 */
const char* snapshot_get_dir(void);

/**
 * @brief 清理快照系统
 */
void snapshot_cleanup(void);

#endif /* CORE_SNAPSHOT_SNAPSHOT_H */