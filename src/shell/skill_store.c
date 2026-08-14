/**
 * @file    src/shell/skill_store.c
 * @brief   本地技能商店（OpenClaw 式技能市场，完全离线可用）
 * @version LN-B-5.1.2.6-rc
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
