/**
 * @file    src/shell/install_helpers.h
 * @brief   多发行版安装辅助函数（安全冗余）
 * @version LN-B-5.1.2.6-rc
 * @changes 增加预检查函数声明和包管理器检测声明
 */

#ifndef SHELL_INSTALL_HELPERS_H
#define SHELL_INSTALL_HELPERS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 检测当前系统使用的包管理器
 * @return 包管理器类型枚举值（见内部实现）
 */
int detect_package_manager(void);

/**
 * @brief 安装指定的软件包（自动适配发行版，含重试机制）
 * @param tool_name 工具名称 (如 "systemd", "nmap", "arp-scan")
 * @param max_retries 最大重试次数（推荐 2）
 * @return 0 成功，-1 失败
 */
int install_package(const char *tool_name, int max_retries);

/**
 * @brief 检查 systemd 是否可用
 * @return 1 可用，0 不可用
 */
int check_systemd_available(void);

/**
 * @brief 安装 Python 模块（使用 pip3）
 * @param module_name 模块名称
 * @return 0 成功，-1 失败
 */
int install_python_module(const char *module_name);

/* ============================================================
 * 预检查和验证函数（供 env_bootstrap.c 调用）
 * ============================================================ */

/**
 * @brief 检查系统包是否已安装（多发行版支持）
 * @param pkg_name 包名
 * @return 1 已安装，0 未安装或检测失败
 */
int is_system_package_installed(const char *pkg_name);

/**
 * @brief 检查 Python 模块是否可导入
 * @param module_name 模块名
 * @return 1 可导入，0 不可导入
 */
int is_python_module_installed(const char *module_name);

/**
 * @brief 获取发行版对应的包名
 * @param logical_name 逻辑包名（如 "mosquitto-dev"）
 * @return 实际包名，若不存在则返回 NULL
 */
const char* get_system_package_name(const char *logical_name);

/* ============================================================
 * 包管理器可用性检测（供 env_bootstrap.c 的多方法安装使用）
 * ============================================================ */
int check_apt_available(void);
int check_dnf_available(void);
int check_yum_available(void);
int check_pacman_available(void);
int check_zypper_available(void);
int check_apk_available(void);
int check_pip_available(void);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_INSTALL_HELPERS_H */