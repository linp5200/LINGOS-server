#ifndef COMMON_DATA_PATH_H
#define COMMON_DATA_PATH_H

#include <stddef.h>

/* LING OS 路径集中管理（先生 2026-09-05 裁决：路径以函数集中，不逐文件硬编码）
 * 根 = /LINGOS（可被 LINGOS_ROOT env 覆盖——全捆 install.sh 迁移后默认 /LINGOS）
 * 用法：lingos_path(P_CFG) 等返回根下标准路径（内部 static 缓冲，勿长期持有跨调用） */

/* 路径标识（先生 FHS 布局：bin/etc/run/var{lib,log}/share/...） */
typedef enum {
    P_ROOT = 0,        /* /LINGOS */
    P_BIN,             /* /LINGOS/bin 程序与工具 */
    P_ETC,             /* /LINGOS/system/config 配置（原 system/config） */
    P_RUN,             /* /LINGOS/run 运行期 sock/pid */
    P_LOG,             /* /LINGOS/log 日志 */
    P_DATA,            /* /LINGOS/data 数据 */
    P_STATE,           /* /LINGOS/state 状态持久 */
    P_MODELS,          /* /LINGOS/models */
    P_SKILLS,          /* /LINGOS/skills */
    P_SHARE,           /* /LINGOS/share 只读资源 */
    P_WEBUI,           /* /LINGOS/share/webui Web UI */
    P_REGISTRY,        /* /LINGOS/registry 注册中心 */
    P_PLUGINS,         /* /LINGOS/plugins 系统插件 */
    P_SNAPSHOTS,       /* /LINGOS/snapshots */
    P_EN,              /* /LINGOS/Ensystem 环境引导缓存 */
    P_COUNT
} path_id_t;

/* 数据根（默认 /LINGOS，LINGOS_ROOT env 覆盖） */
const char *lingos_data_root(void);

/* 按 id 取标准路径（内部 static 缓冲——立即使用或 strdup） */
const char *lingos_path(path_id_t id);

/* 便捷：拼接根 + 相对子路径到用户缓冲 */
void lingos_path_join(const char *rel, char *out, size_t out_sz);

#endif /* COMMON_DATA_PATH_H */
