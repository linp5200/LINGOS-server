/**
 * @file    dependency_check.h
 * @brief   系统依赖检查（每次启动执行）
 * @version LN-B-3.8.0.0
 */

#ifndef CORE_DEPENDENCY_CHECK_H
#define CORE_DEPENDENCY_CHECK_H

/**
 * @brief 依赖检查结果结构
 */
typedef struct {
    int python_missing;      /* 缺失 Python 模块数 */
    int cmd_missing;         /* 缺失系统命令数 */
    int lib_missing;         /* 缺失动态库数 */
    int config_missing;      /* 缺失配置文件数 */
    char details[2048];      /* 详细缺失信息 */
    char suggestions[1024];  /* 修复建议 */
} dep_check_result_t;

/**
 * @brief 检查所有必需依赖
 * @param result 输出检查结果
 * @return 0 全部满足，-1 有缺失
 */
int check_required_dependencies(dep_check_result_t *result);

/**
 * @brief 检查 Python 模块是否存在
 * @param module_name 模块名（不含 .py）
 * @return 1 存在，0 不存在
 */
int check_python_module(const char *module_name);

/**
 * @brief 检查系统命令是否可用
 * @param cmd 命令名
 * @return 1 可用，0 不可用
 */
int check_system_command(const char *cmd);

/**
 * @brief 检查动态库是否存在
 * @param lib_name 库名（如 libnotcurses.so）
 * @return 1 存在，0 不存在
 */
int check_system_library(const char *lib_name);

/**
 * @brief 检查磁盘空间是否充足
 * @param required_mb 所需 MB
 * @return 1 充足，0 不足
 */
int check_disk_space(int required_mb);

/**
 * @brief 检查 notcurses 是否可用
 * @return 1 可用，0 不可用
 */
int check_notcurses_available(void);

#endif /* CORE_DEPENDENCY_CHECK_H */