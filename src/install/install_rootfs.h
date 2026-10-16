/**
 * @file    src/install/install_rootfs.h
 * @brief   rootfs 真下载引擎（真实 HTTP(S) 下载 + 大小/SHA-256 校验 + 原子替换 + 失败回滚）
 * @version LN-0.9.0
 * @par     历史（docs/LINGOS-未完成功能总清单-20260919.md §1.3）：
 *            P0#10 曾"诚实标注"（不模拟进度），本模块是其**真引擎**：
 *            下载 → 校验 → 原子替换 → 解压（tar，参数化执行）→ 原子目录切换 → 登记
 * @par     诚实红线（不造假进度/不假成功）：
 *            · 校验失败/下载失败 = 明确错误码 + 现场清理（半成品删除，旧版本原样保留）
 *            · 镜像清单未提供 sha256 时**如实告警"未校验"**，绝不声称已校验
 *            · 仅内置**已实测的固定版本 + 官方 digest**（2026-10-16 取自镜像站官方文件）
 *            · 出口白名单（src/net/egress.c）未放行镜像域 → 明确报错，不静默绕过安全策略
 */

#ifndef INSTALL_ROOTFS_H
#define INSTALL_ROOTFS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 结果码（0 = 成功；负值 = 具体失败阶段） */
#define ROOTFS_OK              0
#define ROOTFS_ERR_ARG       (-1)   /* 参数非法 */
#define ROOTFS_ERR_UNKNOWN   (-2)   /* 未知发行版 / 本架构无镜像 */
#define ROOTFS_ERR_EGRESS    (-3)   /* 出口白名单未放行（安全策略，不绕过） */
#define ROOTFS_ERR_DOWNLOAD  (-4)   /* 网络/HTTP 失败 */
#define ROOTFS_ERR_SIZE      (-5)   /* 大小与清单不符 */
#define ROOTFS_ERR_SHA256    (-6)   /* SHA-256 不符 */
#define ROOTFS_ERR_ATOMIC    (-7)   /* 原子替换/回滚失败 */
#define ROOTFS_ERR_EXTRACT   (-8)   /* tar 解压失败 */
#define ROOTFS_ERR_REGISTRY  (-9)   /* 登记表读写失败 */
#define ROOTFS_ERR_TMP      (-10)   /* 临时目录/文件准备失败 */

/**
 * @brief 全链安装：下载 → 校验 → 原子替换归档 → 解压到新目录 → 原子切换 → 登记
 * @param distro 发行版名（如 "alpine" / "ubuntu"；可被 rootfs_manifest.json 扩展）
 * @return ROOTFS_OK 或负错误码（详见 install_rootfs_get_last_error）
 */
int install_rootfs_install(const char *distro);

/**
 * @brief 仅下载 + 校验 + 原子替换归档（不解压）——供离线分发/校验复用
 */
int install_rootfs_download(const char *distro);

/**
 * @brief 是否已安装（登记表有条目 **且** rootfs 目录真实存在）
 * @return 1 已安装；0 未安装
 */
int install_rootfs_is_ready(const char *distro);

/**
 * @brief 卸载：删除 rootfs 目录 + 移除登记条目（保留缓存归档，便于重装）
 */
int install_rootfs_uninstall(const char *distro);

/**
 * @brief 手工登记已部署的 rootfs（App"填写 proot 路径/rootfs 目录"路径的真实落点）
 * @return ROOTFS_OK / 负错误码（目录不存在 = ROOTFS_ERR_ARG）
 */
int install_rootfs_register_manual(const char *name, const char *rootfs_dir, const char *proot_path);

/**
 * @brief 导出已登记沙箱（JSON 数组文本）
 * @return 写入字节数；-1 缓冲不足
 */
int install_rootfs_list(char *out, size_t out_sz);

/**
 * @brief 导出本机架构可下载的发行版清单（JSON 文本）
 */
int install_rootfs_available(char *out, size_t out_sz);

/**
 * @brief 目标镜像域是否已被出口白名单放行（0 = 放行；-1 = 被拦，错误信息含放行指引）
 */
int install_rootfs_ensure_egress(const char *distro);

/**
 * @brief 用户**显式授权**后，把镜像域写入 egress_allowlist.json（真实文件读改写）
 *        默认安装流程不会自动调用——安全策略变更需用户/上层显式触发。
 * @return ROOTFS_OK / 负错误码
 */
int install_rootfs_allow_mirror(const char *distro);

/** @brief 上次错误信息（人类可读，含失败阶段） */
const char *install_rootfs_get_last_error(void);

/** @brief 上次**实测**归档 SHA-256（下载成功后填充；未下载为空串） */
const char *install_rootfs_last_sha256(void);

/** @brief 本机架构（uname -m 归一化：aarch64 / x86_64 / …） */
const char *install_rootfs_arch(void);

/** @brief 清理（仅重置模块状态；不删除任何文件） */
void install_rootfs_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* INSTALL_ROOTFS_H */
