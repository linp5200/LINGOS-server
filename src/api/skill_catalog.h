/**
 * @file    src/api/skill_catalog.h
 * @brief   API 侧真实技能目录（registry 索引 + registry.json + 技能市场 + 已启用目录）
 * @version LN-0.8.0
 *
 * 【第 4 期】清零 API 假数据：
 *   此前 src/api/api_core.c:api_handle_skills_list() 与
 *   src/api/api_routes.c:api_route_skills("list") 都返回**写死的 15 条技能名**
 *   （文件写入/系统信息等示例），与真实注册表无关——先生铁律的"假数据"。
 *   现统一走本模块：只读真实文件，读不到就返回空数组（诚实空态）。
 *
 * 接线点（谁调用它）：
 *   · src/api/api_core.c:api_handle_skills_list()
 *   · src/api/api_core.c:api_handle_skills_categories()
 *   · src/api/api_routes.c:api_route_skills() 的 list 分支
 *   · src/api/control_url.c 的 skills.list / skills.categories 动作
 */
#ifndef API_SKILL_CATALOG_H
#define API_SKILL_CATALOG_H

#include <stddef.h>

/* 输出真实技能目录 JSON 数组：
 *   [{"name","description","risk","category","source"}, ...]
 * category / keyword 可为 NULL（不过滤）。keyword 匹配名称/描述/分类（ASCII 大小写无关）。
 * 返回条目数；缓冲区不足返回 -1（不输出截断的 JSON）；无数据返回 0 并写 "[]"。 */
int skill_catalog_json(char *out, size_t out_size, const char *category, const char *keyword);

/* 输出分类计数 JSON 数组：[{"category":"...","count":N}, ...]（一级浏览数据源） */
int skill_catalog_categories_json(char *out, size_t out_size);

/* 真实技能总数（合并去重后） */
int skill_catalog_total(void);

#endif /* API_SKILL_CATALOG_H */
