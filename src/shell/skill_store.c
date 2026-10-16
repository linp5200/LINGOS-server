/**
 * @file    src/shell/skill_store.c
 * @brief   本地技能商店（OpenClaw 式技能市场，完全离线可用）
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C, UD-DR#S1
 * @par     功能：skill install/list/search/enable/disable/uninstall
 *          技能市场目录 /LINGOS/skills/market/<name>/（skill.json + 实现文件）
 *          启用后注册到 /LINGOS/registry/core/registry.json（type=4 skill）
 */

#include "skill_store.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include "../drivers/uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>   /* 【第 4 期】技能关键词搜索大小写无关匹配 */

#define SKILLS_MARKET   "/skills/market"     /* 技能市场（全部可用技能包） */
#define SKILLS_ENABLED  "/skills/enabled"    /* 已启用技能（实现文件存放处） */
#define REGISTRY_FILE   "/registry/core/registry.json"  /* 主注册表 */
#define MAX_SKILL_NAME  128

/* ============================================================
 * 内部辅助：路径构造
 * ============================================================ */
static void skills_path(char *buf, size_t size, const char *sub) {
    const char *root = lingos_data_root();
    safe_snprintf(buf, size, "%s%s", root, sub);
}

/* ============================================================
 * FTF[确保目录存在]
 * ============================================================ */
static void ensure_dir(const char *path) {
    if (access(path, F_OK) != 0) {
        mkdir(path, 0755);
    }
}

/* ============================================================
 * FTF[初始化技能商店目录]
 * ============================================================ */
void skill_store_init(void) {
    char dir[512];
    skills_path(dir, sizeof(dir), SKILLS_MARKET);
    ensure_dir(dir);
    skills_path(dir, sizeof(dir), SKILLS_ENABLED);
    ensure_dir(dir);
    /* 确保 registry 目录存在 */
    const char *root = lingos_data_root();
    char reg_dir[512];
    safe_snprintf(reg_dir, sizeof(reg_dir), "%s/registry/core", root);
    ensure_dir(reg_dir);
    LOG_INFO_T("SkillStore", "Init", "OK", "skill store ready (%s, %s)", SKILLS_MARKET, SKILLS_ENABLED);
}

/* ============================================================
 * FTF[读取注册表 entries 数组]
 * ============================================================ */
static cJSON* load_registry_entries(void) {
    const char *root = lingos_data_root();
    char reg_path[512];
    safe_snprintf(reg_path, sizeof(reg_path), "%s%s", root, REGISTRY_FILE);

    FILE *fp = fopen(reg_path, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rl = fread(buf, 1, (size_t)len, fp);
    buf[rl] = '\0';
    fclose(fp);

    cJSON *root_json = cJSON_Parse(buf);
    free(buf);
    if (!root_json) return NULL;
    cJSON *entries = cJSON_GetObjectItem(root_json, "entries");
    if (!cJSON_IsArray(entries)) {
        cJSON_Delete(root_json);
        return NULL;
    }
    return root_json;
}

/* ============================================================
 * FTF[保存注册表（原子写入）]
 * ============================================================ */
static int save_registry(cJSON *root_json) {
    const char *root = lingos_data_root();
    char reg_path[512];
    safe_snprintf(reg_path, sizeof(reg_path), "%s%s", root, REGISTRY_FILE);

    char *json_str = cJSON_PrintUnformatted(root_json);
    if (!json_str) return -1;
    char tmp_path[512];
    safe_snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", reg_path);
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) { free(json_str); return -1; }
    fprintf(fp, "%s\n", json_str);
    fclose(fp);
    free(json_str);
    if (rename(tmp_path, reg_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

/* ============================================================
 * FTF[查找注册表中技能条目（按 name）]
 * ============================================================ */
static cJSON* find_skill_entry(cJSON *entries, const char *name) {
    if (!entries || !name) return NULL;
    int n = cJSON_GetArraySize(entries);
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(entries, i);
        cJSON *en = cJSON_GetObjectItem(e, "name");
        if (cJSON_IsString(en) && strcmp(en->valuestring, name) == 0) {
            return e;
        }
    }
    return NULL;
}

/* ============================================================
 * FTF[风险扫描（简化：危险模式检测）]
 * ============================================================ */
static int scan_skill_risk(const char *skill_dir, char *report, size_t report_size) {
    if (report) report[0] = '\0';
    /* 扫描 skill.json 与实现文件中的危险模式 */
    const char *danger[] = {"rm -rf", "mkfs", "format(", "shutdown", "reboot",
                            "> /dev/sd", "dd if=", "chmod 777 /", "eval("};
    int danger_count = sizeof(danger) / sizeof(danger[0]);
    int found = 0;
    int severity = 0;

    DIR *d = opendir(skill_dir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[512];
        safe_snprintf(path, sizeof(path), "%s/%s", skill_dir, ent->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            for (int i = 0; i < danger_count; i++) {
                if (strstr(line, danger[i])) {
                    found = 1;
                    severity++;
                    if (report && report_size > 0) {
                        char tmp[256];
                        safe_snprintf(tmp, sizeof(tmp), "  ⚠ %s: '%s'\n", ent->d_name, danger[i]);
                        safe_strlcat(report, tmp, report_size);
                    }
                }
            }
        }
        fclose(fp);
    }
    closedir(d);
    LOG_INFO_T("SkillStore", "RiskScan", "Done", "found=%d severity=%d", found, severity);
    return severity;
}

/* ============================================================
 * 复制文件（技能实现文件 → enabled 目录）
 * ============================================================ */
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    return 0;
}

/* ============================================================
 * 注册技能到 registry（type=4 skill, status=1 active）
 * ============================================================ */
static int register_skill_entry(const char *name, const char *version,
                                const char *risk, const char *description,
                                const char *handler_path) {
    cJSON *root_json = load_registry_entries();
    cJSON *entries = NULL;
    if (!root_json) {
        root_json = cJSON_CreateObject();
        cJSON_AddStringToObject(root_json, "version", "1.0");
        cJSON_AddStringToObject(root_json, "registry_id", "lingos_registry");
        cJSON_AddNumberToObject(root_json, "updated_at", (double)time(NULL));
        entries = cJSON_CreateArray();
        cJSON_AddItemToObject(root_json, "entries", entries);
    } else {
        entries = cJSON_GetObjectItem(root_json, "entries");
    }

    /* 已存在则更新，否则新建 */
    cJSON *entry = find_skill_entry(entries, name);
    if (!entry) {
        entry = cJSON_CreateObject();
        cJSON_AddItemToArray(entries, entry);
    }

    char id[256];
    safe_snprintf(id, sizeof(id), "skill:%s", name);
    cJSON_DeleteItemFromObject(entry, "id");
    cJSON_AddStringToObject(entry, "id", id);
    cJSON_DeleteItemFromObject(entry, "type");
    cJSON_AddNumberToObject(entry, "type", 4);          /* REG_TYPE_SKILL */
    cJSON_DeleteItemFromObject(entry, "name");
    cJSON_AddStringToObject(entry, "name", name);
    cJSON_DeleteItemFromObject(entry, "version");
    cJSON_AddStringToObject(entry, "version", version ? version : "1.0.0");
    cJSON_DeleteItemFromObject(entry, "status");
    cJSON_AddNumberToObject(entry, "status", 1);        /* REG_STATUS_ACTIVE */
    cJSON_DeleteItemFromObject(entry, "created_at");
    cJSON_AddNumberToObject(entry, "created_at", (double)time(NULL));
    cJSON_DeleteItemFromObject(entry, "updated_at");
    cJSON_AddNumberToObject(entry, "updated_at", (double)time(NULL));

    /* metadata.definition */
    cJSON *metadata = cJSON_GetObjectItem(entry, "metadata");
    if (!cJSON_IsObject(metadata)) {
        cJSON_DeleteItemFromObject(entry, "metadata");
        metadata = cJSON_CreateObject();
        cJSON_AddItemToObject(entry, "metadata", metadata);
    }
    cJSON *definition = cJSON_CreateObject();
    cJSON_AddStringToObject(definition, "handler", "python");
    cJSON_AddStringToObject(definition, "handler_path", handler_path ? handler_path : name);
    cJSON_AddStringToObject(definition, "risk", risk ? risk : "low");
    cJSON_AddStringToObject(definition, "description", description ? description : "");
    cJSON_AddBoolToObject(definition, "need_confirm", 0);
    cJSON_DeleteItemFromObject(metadata, "definition");
    cJSON_AddItemToObject(metadata, "definition", definition);

    int ret = save_registry(root_json);
    cJSON_Delete(root_json);
    /* 【0.6.0】同步刷新技能索引（AI 侧即时可见——无需重启） */
    if (ret == 0) {
        extern int registry_skill_write_index(void);
        registry_skill_write_index();
    }
    return ret;
}

/* ============================================================
 * 从 registry 移除技能条目（disable/uninstall）
 * ============================================================ */
static int unregister_skill_entry(const char *name) {
    cJSON *root_json = load_registry_entries();
    if (!root_json) return -1;
    cJSON *entries = cJSON_GetObjectItem(root_json, "entries");
    if (entries) {
        int n = cJSON_GetArraySize(entries);
        for (int i = 0; i < n; i++) {
            cJSON *e = cJSON_GetArrayItem(entries, i);
            cJSON *en = cJSON_GetObjectItem(e, "name");
            if (cJSON_IsString(en) && strcmp(en->valuestring, name) == 0) {
                cJSON_DeleteItemFromArray(entries, i);
                break;
            }
        }
    }
    int ret = save_registry(root_json);
    cJSON_Delete(root_json);
    /* 【0.6.0】同步刷新技能索引 */
    if (ret == 0) {
        extern int registry_skill_write_index(void);
        registry_skill_write_index();
    }
    return ret;
}

/* ============================================================
 * 安装技能（市场 → 启用区 + 注册 registry）
 * ============================================================ */
int skill_store_install(const char *name) {
    LOG_INFO_T("SkillStore", "Install", "Enter", "name='%s'", name ? name : "(null)");
    if (!name || !*name) {
        uart_puts(tr("Usage: skill install <name>\n", "用法：skill install <名称>\n"));
        return -1;
    }

    const char *root = lingos_data_root();
    char market_dir[512], skill_json_path[512];
    safe_snprintf(market_dir, sizeof(market_dir), "%s%s/%s", root, SKILLS_MARKET, name);
    safe_snprintf(skill_json_path, sizeof(skill_json_path), "%s/skill.json", market_dir);

    if (access(market_dir, F_OK) != 0 || access(skill_json_path, F_OK) != 0) {
        uart_puts(tr("Skill not found in market: ", "技能市场中未找到："));
        uart_puts(name);
        uart_puts("\n");
        return -1;
    }

    /* 读取 skill.json */
    FILE *fp = fopen(skill_json_path, "r");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t rl = fread(buf, 1, (size_t)len, fp);
    buf[rl] = '\0';
    fclose(fp);

    cJSON *root_json = cJSON_Parse(buf);
    free(buf);
    if (!root_json) {
        uart_puts(tr("Invalid skill.json\n", "无效的 skill.json\n"));
        return -1;
    }

    cJSON *meta = cJSON_GetObjectItem(root_json, "metadata");
    cJSON *def = meta ? cJSON_GetObjectItem(meta, "definition") : NULL;
    cJSON *risk_item = cJSON_GetObjectItem(root_json, "risk");
    cJSON *ver_item = cJSON_GetObjectItem(root_json, "version");
    cJSON *handler_item = def ? cJSON_GetObjectItem(def, "handler_path") : NULL;
    cJSON *desc_item = def ? cJSON_GetObjectItem(def, "description") : NULL;

    const char *risk = risk_item && cJSON_IsString(risk_item) ? risk_item->valuestring : "low";
    const char *version = ver_item && cJSON_IsString(ver_item) ? ver_item->valuestring : "1.0.0";
    const char *handler_path = handler_item && cJSON_IsString(handler_item) ? handler_item->valuestring : name;
    const char *desc = desc_item && cJSON_IsString(desc_item) ? desc_item->valuestring : "";

    /* 风险扫描 + 用户确认 */
    char risk_report[1024];
    int severity = scan_skill_risk(market_dir, risk_report, sizeof(risk_report));
    if (severity > 0) {
        uart_puts(COLOR_YELLOW);
        uart_puts(tr("⚠ Risk scan found potential dangerous patterns:\n", "⚠ 风险扫描发现潜在危险模式：\n"));
        uart_puts(risk_report);
        uart_puts(COLOR_RESET);
    }
    uart_puts(tr("Install skill '", "安装技能 '"));
    uart_puts(name);
    uart_puts(tr("' (risk=", "'（风险="));
    uart_puts(risk);
    uart_puts(tr(")? [y/N]: ", "）？[y/N]: "));
    char c = uart_getc();
    uart_putc(c);
    uart_puts("\n");
    if (c != 'y' && c != 'Y') {
        cJSON_Delete(root_json);
        uart_puts(tr("Install cancelled.\n", "安装已取消。\n"));
        return -1;
    }

    /* 复制实现文件到 enabled 目录 */
    char enabled_dir[512];
    safe_snprintf(enabled_dir, sizeof(enabled_dir), "%s%s/%s", root, SKILLS_ENABLED, name);
    ensure_dir(enabled_dir);

    DIR *d = opendir(market_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (strcmp(ent->d_name, "skill.json") == 0) continue;
            char src[512], dst[512];
            safe_snprintf(src, sizeof(src), "%s/%s", market_dir, ent->d_name);
            safe_snprintf(dst, sizeof(dst), "%s/%s", enabled_dir, ent->d_name);
            copy_file(src, dst);
        }
        closedir(d);
    }

    /* 注册 registry */
    if (register_skill_entry(name, version, risk, desc, handler_path) != 0) {
        cJSON_Delete(root_json);
        uart_puts(tr("Failed to register skill.\n", "技能注册失败。\n"));
        return -1;
    }

    cJSON_Delete(root_json);
    uart_puts(COLOR_GREEN);
    uart_puts(tr("✅ Skill installed and enabled: ", "✅ 技能已安装并启用："));
    uart_puts(name);
    uart_puts(COLOR_RESET);
    uart_puts("\n");
    LOG_INFO_T("SkillStore", "Install", "OK", "installed %s (risk=%s)", name, risk);
    return 0;
}

/* ============================================================
 * 列出市场技能
 * ============================================================ */
void skill_store_list(const char *filter) {
    const char *root = lingos_data_root();
    char market_dir[512];
    safe_snprintf(market_dir, sizeof(market_dir), "%s%s", root, SKILLS_MARKET);
    ensure_dir(market_dir);

    DIR *d = opendir(market_dir);
    if (!d) {
        uart_puts(tr("Skill market not available.\n", "技能市场不可用。\n"));
        return;
    }

    uart_puts(COLOR_CYAN);
    uart_puts(tr("=== Local Skill Market ===\n", "=== 本地技能市场 ===\n"));
    uart_puts(COLOR_RESET);

    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (filter && !strstr(ent->d_name, filter)) continue;
        uart_puts("  • ");
        uart_puts(ent->d_name);
        /* 检查是否已启用 */
        char enabled_path[512];
        safe_snprintf(enabled_path, sizeof(enabled_path), "%s%s/%s", root, SKILLS_ENABLED, ent->d_name);
        if (access(enabled_path, F_OK) == 0) {
            uart_puts(COLOR_GREEN);
            uart_puts(tr("  [enabled]", "  [已启用]"));
            uart_puts(COLOR_RESET);
        } else {
            uart_puts(tr("  [not installed]", "  [未安装]"));
        }
        uart_puts("\n");
        count++;
    }
    closedir(d);
    uart_puts(tr("Total: ", "总计："));
    char buf[16];
    safe_snprintf(buf, sizeof(buf), "%d\n", count);
    uart_puts(buf);
}

/* ============================================================
 * 启用技能（注册到 registry）
 * ============================================================ */
int skill_store_enable(const char *name) {
    if (!name || !*name) return -1;
    const char *root = lingos_data_root();
    char market_dir[512];
    safe_snprintf(market_dir, sizeof(market_dir), "%s%s/%s", root, SKILLS_MARKET, name);
    char skill_json[512];
    safe_snprintf(skill_json, sizeof(skill_json), "%s/skill.json", market_dir);
    if (access(skill_json, F_OK) != 0) {
        uart_puts(tr("Skill not found in market.\n", "技能市场中未找到该技能。\n"));
        return -1;
    }
    /* 确保实现文件存在（复制一次） */
    char enabled_dir[512];
    safe_snprintf(enabled_dir, sizeof(enabled_dir), "%s%s/%s", root, SKILLS_ENABLED, name);
    ensure_dir(enabled_dir);
    DIR *d = opendir(market_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (strcmp(ent->d_name, "skill.json") == 0) continue;
            char src[512], dst[512];
            safe_snprintf(src, sizeof(src), "%s/%s", market_dir, ent->d_name);
            safe_snprintf(dst, sizeof(dst), "%s/%s", enabled_dir, ent->d_name);
            if (access(dst, F_OK) != 0) copy_file(src, dst);
        }
        closedir(d);
    }
    int ret = register_skill_entry(name, "1.0.0", "low", "", name);
    if (ret == 0) {
        uart_puts(tr("✅ Skill enabled: ", "✅ 技能已启用："));
        uart_puts(name);
        uart_puts("\n");
    }
    return ret;
}

/* ============================================================
 * 禁用技能（从 registry 移除）
 * ============================================================ */
int skill_store_disable(const char *name) {
    if (!name || !*name) return -1;
    int ret = unregister_skill_entry(name);
    if (ret == 0) {
        uart_puts(tr("✅ Skill disabled: ", "✅ 技能已禁用："));
        uart_puts(name);
        uart_puts("\n");
    } else {
        uart_puts(tr("Failed to disable skill.\n", "禁用技能失败。\n"));
    }
    return ret;
}

/* ============================================================
 * 卸载技能（registry 移除 + 删除 enabled 目录）
 * ============================================================ */
int skill_store_uninstall(const char *name) {
    if (!name || !*name) return -1;
    unregister_skill_entry(name);
    const char *root = lingos_data_root();
    char enabled_dir[512];
    safe_snprintf(enabled_dir, sizeof(enabled_dir), "%s%s/%s", root, SKILLS_ENABLED, name);
    if (access(enabled_dir, F_OK) == 0) {
        char cmd[640];
        safe_snprintf(cmd, sizeof(cmd), "rm -rf '%s'", enabled_dir);
        system(cmd);
    }
    uart_puts(tr("✅ Skill uninstalled: ", "✅ 技能已卸载："));
    uart_puts(name);
    uart_puts("\n");
    return 0;
}

/* ============================================================
 * 【第 4 期】技能两级呈现：分类浏览（一级）+ 关键词搜索（二级）
 * ------------------------------------------------------------
 * 数据源（全部真实文件，禁止假数据/示例数据）：
 *   ① $ROOT/registry/skills/index.json       —— registry 导出的技能索引
 *   ② $ROOT/registry/core/registry.json      —— type=4 技能条目（含 metadata.definition）
 *   ③ $ROOT/skills/market/<name>/skill.json  —— 本地技能市场
 *   ④ $ROOT/skills/enabled/<name>/           —— 已启用技能目录
 * 分类口径：
 *   · skill.json 顶层 category 或 metadata.definition.category（技能自己声明的分类）
 *   · 未声明 → 退回真实来源标签（index/registry/market/enabled），不编造分类
 *   同名技能按来源优先级去重（index > registry > market > enabled）。
 * ============================================================ */

#define SKILLS_INDEX_FILE "/registry/skills/index.json"

/* 读取文本文件（调用者 free） */
static char* read_text_file(const char *path, long *out_len) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long len = ftell(fp);
    if (len < 0 || len > (16 * 1024 * 1024)) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rl = fread(buf, 1, (size_t)len, fp);
    buf[rl] = '\0';
    fclose(fp);
    if (out_len) *out_len = (long)rl;
    return buf;
}

/* JSON 字符串字段取值（不存在/非字符串 → NULL） */
static const char* json_field(const cJSON *obj, const char *key) {
    if (!obj || !key) return NULL;
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

/* 追加技能条目（按 name 去重——先到优先） */
static void catalog_add(cJSON *arr, const char *name, const char *desc,
                        const char *risk, const char *category, const char *source) {
    if (!arr || !name || !*name) return;
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        const char *nm = json_field(it, "name");
        if (nm && strcmp(nm, name) == 0) return;
    }
    cJSON *item = cJSON_CreateObject();
    if (!item) return;
    cJSON_AddStringToObject(item, "name", name);
    cJSON_AddStringToObject(item, "description", desc ? desc : "");
    cJSON_AddStringToObject(item, "risk", (risk && *risk) ? risk : "unknown");
    /* 分类：技能自声明优先；否则用真实来源标签（不编造） */
    cJSON_AddStringToObject(item, "category",
                            (category && *category) ? category : (source ? source : "unknown"));
    cJSON_AddStringToObject(item, "source", source ? source : "unknown");
    cJSON_AddItemToArray(arr, item);
}

/* ① registry 导出的技能索引 */
static void catalog_load_index(cJSON *arr) {
    char path[512];
    skills_path(path, sizeof(path), SKILLS_INDEX_FILE);
    char *txt = read_text_file(path, NULL);
    if (!txt) return;
    cJSON *j = cJSON_Parse(txt);
    free(txt);
    if (!j) return;
    if (cJSON_IsArray(j)) {
        int n = cJSON_GetArraySize(j);
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(j, i);
            catalog_add(arr, json_field(it, "name"), json_field(it, "description"),
                        json_field(it, "risk"), json_field(it, "category"), "index");
        }
    }
    cJSON_Delete(j);
}

/* ② registry.json 中 type=4 的技能条目 */
static void catalog_load_registry(cJSON *arr) {
    cJSON *root_json = load_registry_entries();
    if (!root_json) return;
    cJSON *entries = cJSON_GetObjectItem(root_json, "entries");
    int n = cJSON_GetArraySize(entries);
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(entries, i);
        const cJSON *t = cJSON_GetObjectItem(e, "type");
        if (!cJSON_IsNumber(t) || t->valueint != 4) continue;   /* REG_TYPE_SKILL */
        cJSON *meta = cJSON_GetObjectItem(e, "metadata");
        cJSON *def = meta ? cJSON_GetObjectItem(meta, "definition") : NULL;
        const char *cat = json_field(def, "category");
        if (!cat) cat = json_field(meta, "category");
        catalog_add(arr, json_field(e, "name"),
                    json_field(def, "description"), json_field(def, "risk"),
                    cat, "registry");
    }
    cJSON_Delete(root_json);
}

/* ③ 本地技能市场：$ROOT/skills/market/<name>/skill.json */
static void catalog_load_market(cJSON *arr) {
    char market_dir[512];
    skills_path(market_dir, sizeof(market_dir), SKILLS_MARKET);
    DIR *d = opendir(market_dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char sj[640];
        safe_snprintf(sj, sizeof(sj), "%s/%s/skill.json", market_dir, ent->d_name);
        char *txt = read_text_file(sj, NULL);
        if (!txt) {
            /* 目录存在但无 skill.json —— 如实登记（来源=market，描述留空） */
            catalog_add(arr, ent->d_name, NULL, NULL, NULL, "market");
            continue;
        }
        cJSON *j = cJSON_Parse(txt);
        free(txt);
        if (!j) {
            catalog_add(arr, ent->d_name, NULL, NULL, NULL, "market");
            continue;
        }
        const char *name = json_field(j, "name");
        if (!name || !*name) name = ent->d_name;
        cJSON *meta = cJSON_GetObjectItem(j, "metadata");
        cJSON *def = meta ? cJSON_GetObjectItem(meta, "definition") : NULL;
        const char *desc = json_field(def, "description");
        if (!desc) desc = json_field(j, "description");
        const char *risk = json_field(j, "risk");
        if (!risk) risk = json_field(def, "risk");
        const char *cat = json_field(j, "category");
        if (!cat) cat = json_field(def, "category");
        catalog_add(arr, name, desc, risk, cat, "market");
        cJSON_Delete(j);
    }
    closedir(d);
}

/* ④ 已启用技能目录：$ROOT/skills/enabled/<name>/ */
static void catalog_load_enabled(cJSON *arr) {
    char enabled_dir[512];
    skills_path(enabled_dir, sizeof(enabled_dir), SKILLS_ENABLED);
    DIR *d = opendir(enabled_dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        catalog_add(arr, ent->d_name, NULL, NULL, NULL, "enabled");
    }
    closedir(d);
}

/* 合并四个真实来源 */
static cJSON* load_skill_catalog(void) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    catalog_load_index(arr);
    catalog_load_registry(arr);
    catalog_load_market(arr);
    catalog_load_enabled(arr);
    return arr;
}

/* 来源标签本地化 */
static const char* catalog_source_label(const char *source) {
    if (!source) return tr("unknown", "未知");
    if (strcmp(source, "index") == 0)    return tr("registry index", "注册表索引");
    if (strcmp(source, "registry") == 0) return tr("registry entry", "注册表条目");
    if (strcmp(source, "market") == 0)   return tr("skill market", "技能市场");
    if (strcmp(source, "enabled") == 0)  return tr("enabled dir", "已启用目录");
    return source;
}

/* ASCII 大小写无关子串匹配（关键词搜索用） */
static int contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* 打印单条技能（真实字段，缺描述如实标注） */
static void print_skill_item(const cJSON *it) {
    char buf[512];
    const char *name = json_field(it, "name");
    const char *desc = json_field(it, "description");
    const char *risk = json_field(it, "risk");
    const char *source = json_field(it, "source");
    safe_snprintf(buf, sizeof(buf), "  • %s  [%s]  %s\n",
                  name ? name : "?", risk ? risk : "unknown",
                  (desc && *desc) ? desc : tr("(no description)", "（无描述）"));
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), "      %s: %s\n",
                  tr("source", "来源"), catalog_source_label(source));
    uart_puts(buf);
}

/* ============================================================
 * 一级：分类浏览（category 为空）；二级：分类内明细（category 给定）
 * ============================================================ */
void skill_store_browse(const char *category) {
    cJSON *catalog = load_skill_catalog();
    if (!catalog) {
        uart_puts(tr("Skill catalog unavailable.\n", "技能目录不可用。\n"));
        return;
    }
    int n = cJSON_GetArraySize(catalog);
    if (n == 0) {
        uart_puts(tr("Skill catalog is empty (no local skills registered).\n",
                     "技能目录为空（本地无已注册技能）。\n"));
        uart_puts(tr("Checked: registry/skills/index.json, registry/core/registry.json, skills/market, skills/enabled\n",
                     "已检查：registry/skills/index.json、registry/core/registry.json、skills/market、skills/enabled\n"));
        cJSON_Delete(catalog);
        return;
    }

    if (!category || !*category) {
        /* ---- 一级：分类浏览 ---- */
        uart_puts(COLOR_CYAN);
        uart_puts(tr("=== Skill Categories (level 1) ===\n", "=== 技能分类浏览（一级）===\n"));
        uart_puts(COLOR_RESET);
        int printed = 0;
        for (int i = 0; i < n; i++) {
            const char *cat = json_field(cJSON_GetArrayItem(catalog, i), "category");
            if (!cat) continue;
            int dup = 0;
            for (int k = 0; k < i; k++) {
                const char *prev = json_field(cJSON_GetArrayItem(catalog, k), "category");
                if (prev && strcmp(prev, cat) == 0) { dup = 1; break; }
            }
            if (dup) continue;
            int cnt = 0;
            for (int j = 0; j < n; j++) {
                const char *c2 = json_field(cJSON_GetArrayItem(catalog, j), "category");
                if (c2 && strcmp(c2, cat) == 0) cnt++;
            }
            char buf[256];
            safe_snprintf(buf, sizeof(buf), "  %-24s %d\n", cat, cnt);
            uart_puts(buf);
            printed++;
        }
        if (printed == 0) {
            uart_puts(tr("  (no categorized skills)\n", "  （无分类技能）\n"));
        }
        char total[128];
        safe_snprintf(total, sizeof(total), "%s: %d\n", tr("Total skills", "技能总数"), n);
        uart_puts(total);
        uart_puts(tr("Next: skill browse <category>  |  skill search <keyword>\n",
                     "下一步：skill browse <分类>  |  skill search <关键词>\n"));
    } else {
        /* ---- 二级：分类内明细 ---- */
        char head[256];
        safe_snprintf(head, sizeof(head), "%s: %s\n", tr("Category", "分类"), category);
        uart_puts(COLOR_CYAN);
        uart_puts(tr("=== Skills in Category (level 2) ===\n", "=== 分类内技能（二级）===\n"));
        uart_puts(COLOR_RESET);
        uart_puts(head);
        int shown = 0;
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(catalog, i);
            const char *cat = json_field(it, "category");
            const char *src = json_field(it, "source");
            if (!((cat && strcmp(cat, category) == 0) || (src && strcmp(src, category) == 0))) continue;
            print_skill_item(it);
            shown++;
        }
        if (shown == 0) {
            /* 诚实空态：该分类确实没有技能（不编造） */
            uart_puts(tr("No skill in this category. Run 'skill browse' to list real categories.\n",
                         "该分类下没有技能。运行 'skill browse' 查看真实分类。\n"));
        } else {
            char buf[64];
            safe_snprintf(buf, sizeof(buf), "%s: %d\n", tr("Matched", "匹配"), shown);
            uart_puts(buf);
        }
    }
    cJSON_Delete(catalog);
}

/* ============================================================
 * 二级：关键词搜索（名称 / 描述 / 分类；ASCII 大小写无关）
 * ============================================================ */
void skill_store_search(const char *keyword) {
    if (!keyword || !*keyword) {
        uart_puts(tr("Usage: skill search <keyword>\n", "用法：skill search <关键词>\n"));
        uart_puts(tr("Tip: 'skill browse' lists categories first.\n",
                     "提示：先 'skill browse' 查看分类。\n"));
        return;
    }
    cJSON *catalog = load_skill_catalog();
    if (!catalog) {
        uart_puts(tr("Skill catalog unavailable.\n", "技能目录不可用。\n"));
        return;
    }
    char head[256];
    safe_snprintf(head, sizeof(head), "%s: %s\n", tr("Skill search", "技能搜索"), keyword);
    uart_puts(COLOR_CYAN);
    uart_puts(head);
    uart_puts(COLOR_RESET);

    int n = cJSON_GetArraySize(catalog);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(catalog, i);
        const char *name = json_field(it, "name");
        const char *desc = json_field(it, "description");
        const char *cat  = json_field(it, "category");
        if (!contains_ci(name, keyword) && !contains_ci(desc, keyword) && !contains_ci(cat, keyword)) continue;
        print_skill_item(it);
        shown++;
    }
    if (shown == 0) {
        /* 诚实空态：无匹配就是无匹配（不返回编造结果） */
        char buf[256];
        safe_snprintf(buf, sizeof(buf),
                      tr("No skill matches '%s' (no fabricated results).\n",
                         "没有匹配 '%s' 的技能（不返回编造结果）。\n"), keyword);
        uart_puts(buf);
    } else {
        char buf[64];
        safe_snprintf(buf, sizeof(buf), "%s: %d\n", tr("Matched", "匹配"), shown);
        uart_puts(buf);
    }
    cJSON_Delete(catalog);
}

