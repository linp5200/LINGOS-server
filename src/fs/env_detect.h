/**
 * @file    env_detect.h
 * @brief   环境检测与依赖安装接口
 * @version LN-B-4.2.0.0
 * @date    2026-07-05
 */

#ifndef FS_ENV_DETECT_H
#define FS_ENV_DETECT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Python 环境检测
 * ============================================================ */

/**
 * @brief 检查并确保 Python 环境可用（包括 flask、requests 模块）
 * @return 0 成功，-1 失败
 */
int ensure_python_environment(void);

/**
 * @brief 检查 libcurl 是否可用
 * @return 0 可用，-1 不可用
 */
int ensure_libcurl(void);

/**
 * @brief 检查 libmicrohttpd 是否可用
 * @return 0 可用，-1 不可用
 */
int ensure_microhttpd(void);

/**
 * @brief 检查 UTF-8 locale 是否设置
 * @return 0 已设置，-1 未设置
 */
int ensure_utf8_locale(void);

/* ============================================================
 * Python 模块检测
 * ============================================================ */

/**
 * @brief 检查 Python 模块是否已安装
 * @param module_name 模块名（如 "requests"）
 * @return 1 已安装，0 未安装
 */
int check_python_module_installed(const char *module_name);

/* ============================================================
 * 系统依赖检测与安装（批次21 新增）
 * ============================================================ */

/**
 * @brief 检查并安装系统依赖库（libmosquitto、libnotcurses、libsqlite3、libmicrohttpd）
 * @return 0 全部满足，-1 有缺失
 */
int ensure_system_dependencies(void);

/**
 * @brief 检查并安装 Python 依赖模块（sentence-transformers、tiktoken、requests、flask、numpy）
 * @return 0 全部满足，-1 有缺失
 */
int ensure_python_dependencies(void);

/**
 * @brief 检查并安装所有依赖（系统库 + Python 模块）
 * @return 0 全部满足，-1 有缺失
 */
int ensure_all_dependencies(void);

int check_system_library(const char *libname);
#ifdef __cplusplus
}
#endif

#endif /* FS_ENV_DETECT_H */