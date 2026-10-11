#ifndef FIREWALL_FIREWALL_H
#define FIREWALL_FIREWALL_H

/* 【P5 接线】目标合法性校验：仅接受 IPv4/IPv6 字面量或 CIDR（含 /前缀）。
 * 规则拼接进 iptables/nft 命令串前必须先过这一关，否则主机名即命令注入。 */
int firewall_is_valid_target(const char *ip_or_cidr);

/* 添加允许规则（支持 IP 或 CIDR）*/
int firewall_allow(const char *ip_or_cidr, const char *port);

/* 添加拒绝规则 */
int firewall_deny(const char *ip_or_cidr, const char *port);

/* 列出所有规则 */
void firewall_list(void);

/* 清空所有规则 */
int firewall_flush(void);

#endif