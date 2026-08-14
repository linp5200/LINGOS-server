#ifndef UPDATE_WEB_UPDATE_H
#define UPDATE_WEB_UPDATE_H

/**
 * @brief 从更新包解压目录安装 Web UI（复制 web/ 到 /LINGOS/web）
 * @param extract_dir 更新包解压目录（如 /tmp/update_xxx）
 * @return 0 成功，-1 失败
 */
int install_web_component(const char *extract_dir);

#endif