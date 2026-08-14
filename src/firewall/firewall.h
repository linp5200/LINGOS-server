#ifndef FIREWALL_FIREWALL_H
#define FIREWALL_FIREWALL_H

/* 添加允许规则（支持 IP 或 CIDR）*/
int firewall_allow(const char *ip_or_cidr, const char *port);

/* 添加拒绝规则 */
int firewall_deny(const char *ip_or_cidr, const char *port);

/* 列出所有规则 */
void firewall_list(void);

/* 清空所有规则 */
int firewall_flush(void);

#endif