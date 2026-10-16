#ifndef SHELL_SKILL_STORE_H
#define SHELL_SKILL_STORE_H

/* 初始化技能商店（确保目录） */
void skill_store_init(void);

/* 安装技能（市场 → 启用区 + 注册 registry，含风险扫描与用户确认） */
int skill_store_install(const char *name);

/* 列出市场技能（可选关键词过滤） */
void skill_store_list(const char *filter);

/* 【第 4 期】技能两级呈现（先生：分类浏览 + 关键词搜索）
 *   一级：skill_store_browse(NULL)      —— 按分类列出计数
 *   二级：skill_store_browse("<分类>")  —— 该分类下的技能明细
 *   搜索：skill_store_search("<关键词>") —— 名称/描述/分类关键词检索
 *   数据源全部为真实文件（registry 索引 / registry.json / 技能市场 / 已启用目录），
 *   无任何示例或编造数据。 */
void skill_store_browse(const char *category);
void skill_store_search(const char *keyword);

/* 启用/禁用/卸载 */
int skill_store_enable(const char *name);
int skill_store_disable(const char *name);
int skill_store_uninstall(const char *name);

#endif /* SHELL_SKILL_STORE_H */
