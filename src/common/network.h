/**
 * @file    src/common/network.h
 * @brief   网络检测功能头文件
 * @version LN-B-5.1.2.6-rc
 */

#ifndef COMMON_NETWORK_H
#define COMMON_NETWORK_H

#include <stdint.h>

/**
 * @brief 检测 DNS 解析是否正常
 * @param host     要解析的主机名（如 "archive.ubuntu.com"）
 * @param timeout  超时秒数（建议 3 秒）
 * @return 0 成功，-1 失败（超时或解析错误）
 */
int network_dns_resolve(const char *host, int timeout);

/**
 * @brief 检测 apt 源是否可达（默认 archive.ubuntu.com）
 * @param timeout  超时秒数
 * @return 0 可达，-1 不可达
 */
int network_check_apt_source(int timeout);

/**
 * @brief 检测 PyPI 镜像是否可达（默认 pypi.tuna.tsinghua.edu.cn）
 * @param timeout  超时秒数
 * @return 0 可达，-1 不可达
 */
int network_check_pypi_mirror(int timeout);

/**
 * @brief 快速网络预检（检测 DNS、apt 源、PyPI 源），全部成功才返回 0
 * @param timeout  超时秒数
 * @return 0 网络可用，-1 网络不可用或部分失败
 */
int network_check_online(int timeout);

#endif /* COMMON_NETWORK_H */