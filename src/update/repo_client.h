#ifndef UPDATE_REPO_CLIENT_H
#define UPDATE_REPO_CLIENT_H

/* 仓库配置 */
#define REPO_CONFIG_PATH "/system/config/repo.conf"
#define REPO_INDEX_FILE "repo-index.json"

/* 初始化仓库客户端（读取配置）*/
int repo_client_init(void);

/* 下载并解析仓库索引 */
char *repo_download_index(void);

/* 搜索应用（返回 JSON 字符串，需 free）*/
char *repo_search_app(const char *keyword);

/* 获取应用最新版本号 */
char *repo_get_latest_version(const char *app_name);

/* 下载应用包到指定路径 */
int repo_download_app(const char *app_name, const char *target_path);

#endif