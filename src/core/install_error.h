/**
 * @file    src/core/install_error.h
 * @brief   安装错误动态解析系统头文件
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：防弹/容错编程
 */

#ifndef CORE_INSTALL_ERROR_H
#define CORE_INSTALL_ERROR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 错误类型枚举
 * ============================================================ */

typedef enum {
    ERR_TYPE_UNKNOWN = 0,              /* 未知错误 */
    ERR_TYPE_PKG_NOT_FOUND,            /* E: Unable to locate package */
    ERR_TYPE_DEPENDENCY_CONFLICT,      /* Depends: xxx but yyy is to be installed */
    ERR_TYPE_SOURCE_UNREACHABLE,       /* Failed to fetch ... 404 Not Found */
    ERR_TYPE_PERMISSION_DENIED,        /* Could not open lock file */
    ERR_TYPE_PEP_668,                  /* externally-managed-environment */
    ERR_TYPE_NETWORK_TIMEOUT,          /* Connection timed out */
    ERR_TYPE_DISK_FULL,                /* No space left on device */
    ERR_TYPE_BROKEN_PACKAGE,           /* dpkg: error processing package */
    ERR_TYPE_UNMET_DEPENDENCIES,       /* unmet dependencies */
    ERR_TYPE_SIGNATURE_INVALID,        /* GPG error / Signature invalid */
    ERR_TYPE_CACHE_UPDATE_FAILED,      /* apt update failed */
    ERR_TYPE_PIP_VERSION_MISMATCH,     /* pip version conflict */
    ERR_TYPE_PIP_NO_MATCHING_DIST,     /* No matching distribution found */
    ERR_TYPE_PIP_BUILD_FAILED,         /* Failed building wheel */
    ERR_TYPE_PIP_DEPENDENCY_RESOLVE,   /* pip dependency resolver error */
    ERR_TYPE_MAX
} install_error_type_t;

/* ============================================================
 * 解决方案类型枚举
 * ============================================================ */

typedef enum {
    SOLVE_RETRY_WITH_ALT_NAME,         /* 尝试包名变体 */
    SOLVE_ADD_BREAK_FLAG,              /* 添加 --break-system-packages */
    SOLVE_FIX_BROKEN,                  /* 执行 apt install -f */
    SOLVE_TRY_MIRROR,                  /* 切换镜像源 */
    SOLVE_RETRY_WITH_BACKOFF,          /* 退避重试（1s,2s,4s,8s） */
    SOLVE_WAIT_AND_RETRY,              /* 等待后重试 */
    SOLVE_CLEAN_CACHE_AND_RETRY,       /* 清理缓存后重试 */
    SOLVE_UPDATE_KEYS,                 /* 更新 GPG 密钥 */
    SOLVE_TRY_ALT_INDEX,               /* 尝试备用 PyPI 索引 */
    SOLVE_INSTALL_BUILD_DEPS,          /* 安装编译依赖 */
    SOLVE_SKIP_PACKAGE,                /* 跳过该包 */
    SOLVE_RECORD_OUTPUT,               /* 仅记录输出，不重试 */
    SOLVE_MAX
} solution_type_t;

/* ============================================================
 * 错误解析结果结构
 * ============================================================ */

typedef struct {
    install_error_type_t error_type;   /* 识别到的错误类型 */
    solution_type_t solution_type;     /* 推荐的解决方案 */
    const char *solution_data;         /* 解决方案参数（如镜像源URL） */
    const char *matched_pattern;       /* 匹配到的模式（调试用） */
    int confidence;                    /* 匹配置信度 0-100 */
    char output_preview[256];          /* 输出预览（前256字符） */
} install_error_result_t;

/* ============================================================
 * 包名变体映射表条目
 * ============================================================ */

typedef struct {
    const char *original;              /* 原始包名 */
    const char *variants[8];           /* 可能的变体列表 */
} pkg_variant_map_t;

/* ============================================================
 * 核心 API
 * ============================================================ */

/**
 * @brief 解析安装输出，识别错误类型并推荐解决方案
 * @param output 包管理器的完整输出（stdout+stderr）
 * @param pkg_name 当前正在安装的包名
 * @param method_name 当前使用的安装方法名称（如 "apt"）
 * @param result 输出解析结果
 * @return 0 成功，-1 失败
 */
int install_error_parse(const char *output, const char *pkg_name,
                        const char *method_name, install_error_result_t *result);

/**
 * @brief 执行解决方案
 * @param result 错误解析结果
 * @param pkg_name 包名
 * @param current_method 当前方法名称
 * @param output 原始输出（用于日志记录）
 * @return 0 成功，-1 失败，-2 需要降级到下一个安装方法
 */
int install_error_execute_solution(const install_error_result_t *result,
                                   const char *pkg_name,
                                   const char *current_method,
                                   const char *output);

/**
 * @brief 尝试包名变体
 * @param pkg_name 原始包名
 * @param out_variant 输出变体名（缓冲区至少64字节）
 * @param max_variants 最大变体数量
 * @return 找到的变体数量
 */
int install_error_get_pkg_variants(const char *pkg_name,
                                   char out_variants[][64],
                                   int max_variants);

/**
 * @brief 检查是否应该重试（基于错误类型）
 * @param error_type 错误类型
 * @param attempt 当前尝试次数（从1开始）
 * @return 1 应该重试，0 不应该重试
 */
int install_error_should_retry(install_error_type_t error_type, int attempt);

/**
 * @brief 获取错误类型的友好描述
 * @param error_type 错误类型
 * @param lang 语言（"en" 或 "zh"）
 * @return 描述字符串
 */
const char* install_error_get_description(install_error_type_t error_type,
                                          const char *lang);

/**
 * @brief 获取解决方案的友好描述
 * @param solution_type 解决方案类型
 * @param lang 语言（"en" 或 "zh"）
 * @return 描述字符串
 */
const char* install_error_get_solution_desc(solution_type_t solution_type,
                                            const char *lang);

/**
 * @brief 记录完整输出到日志文件
 * @param pkg_name 包名
 * @param method_name 方法名称
 * @param output 完整输出
 */
void install_error_log_output(const char *pkg_name, const char *method_name,
                              const char *output);

/**
 * @brief 获取错误类型的优先级（用于排序）
 * @param error_type 错误类型
 * @return 优先级数值（越小越优先）
 */
int install_error_get_priority(install_error_type_t error_type);

#ifdef __cplusplus
}
#endif

#endif /* CORE_INSTALL_ERROR_H */