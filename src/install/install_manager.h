/**
 * @file    src/install/install_manager.h
 * @brief   安装管理器：统一入口、调度、汇总
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 */

#ifndef INSTALL_MANAGER_H
#define INSTALL_MANAGER_H

#include <time.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 安装结果结构
 * ============================================================ */
typedef struct install_result {
    char name[64];               /* 包/模块名称 */
    int success;                 /* 1=成功, 0=失败 */
    char error_msg[256];         /* 错误信息 */
    double elapsed_seconds;      /* 耗时（秒） */
    time_t timestamp;            /* 时间戳 */
} install_result_t;

/* ============================================================
 * 安装汇总
 * ============================================================ */
typedef struct install_summary {
    int total;                   /* 总数 */
    int success_count;           /* 成功数 */
    int failed_count;            /* 失败数 */
    double total_time;           /* 总耗时（秒） */
    install_result_t results[64];/* 详细结果 */
    int result_count;            /* 结果数量 */
    char log_file[256];          /* 日志文件路径 */
} install_summary_t;

/* ============================================================
 * API
 * ============================================================ */

/**
 * @brief 安装所有依赖（系统包 + Python 包 + 模型）
 * @param summary 输出汇总结果（可为 NULL）
 * @return 0 全部成功，-1 有失败
 */
int install_manager_run_all(install_summary_t *summary);

/**
 * @brief 仅安装系统依赖
 * @param summary 输出汇总结果（可为 NULL）
 * @return 0 成功，-1 失败
 */
int install_manager_run_system(install_summary_t *summary);

/**
 * @brief 仅安装 Python 依赖
 * @param summary 输出汇总结果（可为 NULL）
 * @return 0 成功，-1 失败
 */
int install_manager_run_python(install_summary_t *summary);

/**
 * @brief 仅下载模型
 * @param summary 输出汇总结果（可为 NULL）
 * @return 0 成功，-1 失败
 */
int install_manager_run_models(install_summary_t *summary);

/**
 * @brief 检查是否所有依赖已安装
 * @return 1 全部已安装，0 有缺失
 */
int install_manager_check_all(void);

/**
 * @brief 获取上次安装汇总
 * @return 汇总结果指针（只读）
 */
const install_summary_t* install_manager_get_last_summary(void);

/**
 * @brief 清空安装缓存（强制重新安装）
 */
void install_manager_clear_cache(void);

/**
 * @brief 设置离线模式
 * @param offline 1=离线，0=在线
 */
void install_manager_set_offline(int offline);

/**
 * @brief 获取离线模式状态
 * @return 1=离线，0=在线
 */
int install_manager_is_offline(void);

/**
 * @brief 设置镜像源（覆盖配置文件）
 * @param apt_mirror APT 镜像源 URL
 * @param pypi_mirror PyPI 镜像源 URL
 */
void install_manager_set_mirrors(const char *apt_mirror, const char *pypi_mirror);

#ifdef __cplusplus
}
#endif

#endif /* INSTALL_MANAGER_H */