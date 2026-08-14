/**
 * @file    src/fs/self_check.h
 * @brief   启动自检头文件
 * @version LN-B-5.1.2.6-rc
 */

#ifndef FS_SELF_CHECK_H
#define FS_SELF_CHECK_H

/**
 * @brief 快速自检（同步，< 1秒）：检查目录、权限、版本等
 * @return 0 成功，-1 失败（need_configuration 为 1 时返回 -1）
 */
int self_check_and_sync(void);

/**
 * @brief 获取最后一次自检的错误原因（用于错误终端）
 * @return 错误字符串
 */
const char *get_last_selfcheck_error(void);

/**
 * @brief 后台自检（异步，包含Python环境、依赖库等耗时检查）
 * @return 0 成功，-1 失败
 */
int async_self_check(void);

/**
 * @brief 检查环境缓存是否有效（< 60分钟）
 * @return 1 有效，0 无效
 */
int is_env_cache_valid(void);

/**
 * @brief 获取缓存中的环境状态
 * @param python_ok 输出：Python环境是否正常
 * @param libcurl_ok 输出：libcurl是否可用
 * @param microhttpd_ok 输出：libmicrohttpd是否可用
 * @return 0 成功，-1 缓存不存在
 */
int get_env_cache(int *python_ok, int *libcurl_ok, int *microhttpd_ok);

/**
 * @brief 写入环境缓存
 * @param python_ok Python环境是否正常
 * @param libcurl_ok libcurl是否可用
 * @param microhttpd_ok libmicrohttpd是否可用
 * @return 0 成功，-1 失败
 */
int set_env_cache(int python_ok, int libcurl_ok, int microhttpd_ok);

/* ====== 配置完整性检查 ====== */

/**
 * @brief 检查系统配置是否完整（关键配置文件是否存在且有效）
 * @param missing_list 输出：缺失的文件列表（以逗号分隔），调用者需free，可为NULL
 * @return 0 配置完整，1 配置不完整
 */
int check_config_completeness(char **missing_list);

/**
 * @brief 全局标志：是否需要运行配置向导（由 self_check 设置）
 */
extern int need_configuration;

#endif /* FS_SELF_CHECK_H */