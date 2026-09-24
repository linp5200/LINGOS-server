/**
 * @file    src/net/egress.h
 * @brief   出口白名单（先生裁决 S2 / OWASP LLM06 §3——“出站请求仅允许清单内”）
 * @version LN-0.7.0
 *
 * 设计：
 *   · 放行规则
 *       本机（localhost、127.* 、::1）           → 放行
 *       局域网 / 私网（10./172.16-31./192.168./fe80:: 等）→ 放行（HA、内网上报）
 *       链路本地 169.254.*（云元数据）           → 拒绝（SSRF 头号目标）
 *       公网域名                                 → 仅白名单命中放行
 *   · 白名单 = 内置默认清单 + 用户文件（合并）
 *       用户文件：/LINGOS/system/config/egress_allowlist.json
 *         {"enabled": true, "domains": ["example.com", "*.example.org"]}
 *   · 开关：选项 sec.egress_allowlist（默认开）；关闭时全放行
 *   · 匹配：host 等于条目，或以 “.条目” 结尾（子域自动命中）
 *
 * 意图：被 AI 提示注入诱导的外连（把数据发到攻击者服务器）被清单拦截；
 *       生命线数据源/常用 LLM/更新源已全部内置（详见 egress.c）。
 */

#ifndef NET_EGRESS_H
#define NET_EGRESS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 出口检查
 * @param host 目标主机（域名或 IP，不含端口）
 * @return 0 = 放行；-1 = 拒绝（已打 WARN 日志）
 */
int egress_check(const char *host);

/**
 * @brief 从 URL 提取 host 后检查（http/https）
 * @return 0 = 放行；-1 = 拒绝 / URL 非法
 */
int egress_check_url(const char *url);

/**
 * @brief 清缓存（清单文件被修改后调用——下次检查重新加载）
 */
void egress_reload(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_EGRESS_H */
