#ifndef SHELL_SKILL_STORE_H
#define SHELL_SKILL_STORE_H

/* 初始化技能商店（确保目录） */
void skill_store_init(void);

/* 安装技能（市场 → 启用区 + 注册 registry，含风险扫描与用户确认） */
int skill_store_install(const char *name);

/* 列出市场技能（可选关键词过滤） */
void skill_store_list(const char *filter);

/* 启用/禁用/卸载 */
int skill_store_enable(const char *name);
int skill_store_disable(const char *name);
int skill_store_uninstall(const char *name);

#endif /* SHELL_SKILL_STORE_H */
