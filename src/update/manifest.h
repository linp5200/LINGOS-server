#ifndef UPDATE_MANIFEST_H
#define UPDATE_MANIFEST_H

#include <stdint.h>
#include <stddef.h>   /* 新增：为 size_t 提供定义 */

/* 组件类型枚举 */
typedef enum {
    COMPONENT_BINARY,
    COMPONENT_CONFIG,
    COMPONENT_WEB,
    COMPONENT_OTHER
} component_type_t;

/* 单个变更条目 */
typedef struct {
    char *source;
    char *dest;
    int backup;
} change_entry_t;

/* 组件结构 */
typedef struct {
    component_type_t type;
    char *name;
    char *version;
    change_entry_t *changes;
    int change_count;
} manifest_component_t;

/* 修复元数据（新增） */
typedef struct {
    char reason[256];        /* 修复原因 */
    char trigger[64];        /* 触发源（watchdog_crash / skill_error / log_error / manual） */
    char fingerprint[128];   /* 错误指纹 */
    char author[64];         /* 修复作者（固定为 "Nook (AI)"） */
    double confidence;       /* 置信度（0-1） */
} repair_meta_t;

/* 整体清单 */
typedef struct {
    char *version;
    char *previous_version;
    manifest_component_t *components;
    int component_count;
    int requires_reboot;
    int requires_confirm;
    repair_meta_t repair_meta;  /* 新增：修复元数据 */
} manifest_t;

/* 解析清单文件（兼容旧版） */
int manifest_parse(const char *path, manifest_t *out);

/* 解析清单文件并提取修复元数据（新函数） */
int manifest_parse_with_repair(const char *extract_dir,
                               char *version, size_t vlen,
                               char *source_type, size_t stlen,
                               repair_meta_t *repair_meta);

/* 释放清单内存 */
void manifest_free(manifest_t *m);

#endif