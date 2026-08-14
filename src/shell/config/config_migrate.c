/**
 * @file    config_migrate.c
 * @brief   配置迁移（从旧版升级到新版结构）
 * @version LN-B-5.0.0.0
 * @changes 新增 defense→security 迁移，state→registry 迁移，注册表回滚
 */

#include "config_wizard_common.h"
#include "lang.h"
#include "data_path.h"
#include "safe_string.h"
#include "version.h"
#include "log_extra.h"
#include "cJSON.h"
#include "registry.h"
#include "security_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>

#define CONFIG_VERSION_PATH "/LINGOS/system/config/config_version"
#define CONFIG_DIR "/LINGOS/system/config"
#define BACKUP_DIR "/LINGOS/backups/config_migration"

#ifndef LINGOS_VERSION
#error "LINGOS_VERSION macro is not defined"
#endif

static void extract_version_number(const char *full, char *buf, size_t size) {
    const char *p = full;
    while (*p && *p != '-') p++;
    if (*p == '-') p++;
    while (*p && *p != '-') p++;
    if (*p == '-') p++;
    strncpy(buf, p, size - 1);
    buf[size - 1] = '\0';
}

static int get_config_version(void) {
    FILE *fp = fopen(CONFIG_VERSION_PATH, "r");
    if (!fp) return 0;
    char version[32] = {0};
    if (fgets(version, sizeof(version), fp)) {
        fclose(fp);
        int major = 0;
        sscanf(version, "%d", &major);
        return major;
    }
    fclose(fp);
    return 0;
}

static int write_config_version(const char *version) {
    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/system/config/config_version", root);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%s\n", version);
    fclose(fp);
    return 0;
}

/* 备份旧配置到迁移备份目录 */
static int backup_old_configs(void) {
    const char *root = lingos_data_root();
    char backup_dir[512];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
    safe_snprintf(backup_dir, sizeof(backup_dir), "%s/backups/config_migration/%s", root, ts);
    mkdir(backup_dir, 0755);

    const char *files[] = {"/system/config/defense.conf", "/system/config/risk_policy.json",
                           "/Ensystem/state.json", NULL};
    for (int i = 0; files[i]; i++) {
        char src[512], dst[512];
        safe_snprintf(src, sizeof(src), "%s%s", root, files[i]);
        safe_snprintf(dst, sizeof(dst), "%s%s", backup_dir, files[i]);
        if (access(src, F_OK) == 0) {
            char cmd[1024];
            safe_snprintf(cmd, sizeof(cmd), "cp '%s' '%s' 2>/dev/null", src, dst);
            system(cmd);
            LOG_DEBUG_T("Migrate", "Backup", "OK", "backed up %s", files[i]);
        }
    }
    return 0;
}

/* 迁移 defense.conf → security.json */
static int migrate_defense_to_security(void) {
    const char *root = lingos_data_root();
    char defense_path[512], security_path[512];
    safe_snprintf(defense_path, sizeof(defense_path), "%s/system/config/defense.conf", root);
    safe_snprintf(security_path, sizeof(security_path), "%s/system/config/security.json", root);

    if (access(defense_path, F_OK) != 0) {
        LOG_DEBUG_T("Migrate", "Defense", "NotFound", "defense.conf not found, skipping");
        return 0;
    }

    /* 读取 defense.conf */
    FILE *fp = fopen(defense_path, "r");
    if (!fp) return -1;
    char line[256];
    int shadow = 1, dark = 0, behavior = 1, threshold = 80;
    char algorithm[64] = "sliding";
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
            if (strcmp(key, "shadow_mode_default") == 0) shadow = atoi(val);
            else if (strcmp(key, "dark_mode_default") == 0) dark = atoi(val);
            else if (strcmp(key, "behavior_monitoring") == 0) behavior = atoi(val);
            else if (strcmp(key, "anomaly_threshold") == 0) threshold = atoi(val);
            else if (strcmp(key, "anomaly_algorithm") == 0) safe_strncpy(algorithm, val, sizeof(algorithm));
        }
    }
    fclose(fp);

    /* 创建 security.json */
    cJSON *root_json = cJSON_CreateObject();
    cJSON_AddStringToObject(root_json, "version", "3.0");
    cJSON_AddStringToObject(root_json, "input_mode", shadow ? "balanced" : "permissive");

    cJSON *shadow_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(shadow_obj, "enabled", shadow);
    cJSON *perms = cJSON_CreateArray();
    cJSON_AddItemToArray(perms, cJSON_CreateString("camera"));
    cJSON_AddItemToArray(perms, cJSON_CreateString("microphone"));
    cJSON_AddItemToArray(perms, cJSON_CreateString("location"));
    cJSON_AddItemToObject(shadow_obj, "permissions", perms);
    cJSON *excluded = cJSON_CreateArray();
    cJSON_AddItemToArray(excluded, cJSON_CreateString("lingos_system"));
    cJSON_AddItemToArray(excluded, cJSON_CreateString("nook_core"));
    cJSON_AddItemToObject(shadow_obj, "excluded_apps", excluded);
    cJSON_AddItemToObject(root_json, "shadow_mode", shadow_obj);

    cJSON *dark_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(dark_obj, "enabled", dark);
    cJSON_AddBoolToObject(dark_obj, "simulate_hardware_disable", 1);
    cJSON *blocked = cJSON_CreateArray();
    cJSON_AddItemToArray(blocked, cJSON_CreateString("package_install"));
    cJSON_AddItemToArray(blocked, cJSON_CreateString("package_remove"));
    cJSON_AddItemToArray(blocked, cJSON_CreateString("service_restart"));
    cJSON_AddItemToArray(blocked, cJSON_CreateString("system_reboot"));
    cJSON_AddItemToArray(blocked, cJSON_CreateString("system_update"));
    cJSON_AddItemToArray(blocked, cJSON_CreateString("process_kill"));
    cJSON_AddItemToObject(dark_obj, "blocked_features", blocked);
    cJSON_AddItemToObject(root_json, "dark_mode", dark_obj);

    cJSON *abs_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(abs_obj, "enabled", 0);
    cJSON_AddStringToObject(abs_obj, "trigger", "auto");
    cJSON_AddNumberToObject(abs_obj, "auto_close_check_interval", 30);
    cJSON_AddBoolToObject(abs_obj, "block_all_external_input", 1);
    cJSON_AddBoolToObject(abs_obj, "block_infected_internal_input", 1);
    cJSON_AddItemToObject(root_json, "absolute_protect", abs_obj);

    cJSON *beh_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(beh_obj, "enabled", behavior);
    cJSON_AddNumberToObject(beh_obj, "window_size", 100);
    cJSON_AddNumberToObject(beh_obj, "threshold", threshold);
    cJSON_AddBoolToObject(beh_obj, "auto_escalate", 1);
    cJSON_AddItemToObject(root_json, "behavior_monitoring", beh_obj);

    char *json_str = cJSON_PrintUnformatted(root_json);
    cJSON_Delete(root_json);
    if (!json_str) return -1;

    FILE *out = fopen(security_path, "w");
    if (!out) {
        free(json_str);
        return -1;
    }
    fprintf(out, "%s\n", json_str);
    fclose(out);
    free(json_str);

    LOG_INFO_T("Migrate", "Defense", "OK", "defense.conf migrated to security.json");
    return 0;
}

/* 迁移 risk_policy.json → security.json (合并) */
static int migrate_risk_to_security(void) {
    const char *root = lingos_data_root();
    char risk_path[512], security_path[512];
    safe_snprintf(risk_path, sizeof(risk_path), "%s/system/config/risk_policy.json", root);
    safe_snprintf(security_path, sizeof(security_path), "%s/system/config/security.json", root);

    if (access(risk_path, F_OK) != 0) return 0;

    /* 简单合并：读取 risk_policy.json 的 skill_defaults，写入 security.json 的 risk_mapping */
    FILE *fp = fopen(risk_path, "r");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(fp); return -1; }
    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);
    cJSON *risk_root = cJSON_Parse(buf);
    free(buf);
    if (!risk_root) return -1;

    cJSON *skill_defaults = cJSON_GetObjectItem(risk_root, "skill_defaults");
    if (skill_defaults) {
        fp = fopen(security_path, "r");
        if (!fp) { cJSON_Delete(risk_root); return -1; }
        fseek(fp, 0, SEEK_END);
        long slen = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        char *sbuf = malloc(slen + 1);
        if (!sbuf) { fclose(fp); cJSON_Delete(risk_root); return -1; }
        fread(sbuf, 1, slen, fp);
        sbuf[slen] = '\0';
        fclose(fp);
        cJSON *sec_root = cJSON_Parse(sbuf);
        free(sbuf);
        if (!sec_root) { cJSON_Delete(risk_root); return -1; }

        cJSON_AddItemToObject(sec_root, "risk_mapping", cJSON_Duplicate(skill_defaults, 1));

        char *new_json = cJSON_PrintUnformatted(sec_root);
        cJSON_Delete(sec_root);
        if (new_json) {
            fp = fopen(security_path, "w");
            if (fp) {
                fprintf(fp, "%s\n", new_json);
                fclose(fp);
            }
            free(new_json);
        }
    }
    cJSON_Delete(risk_root);
    LOG_INFO_T("Migrate", "Risk", "OK", "risk_policy.json merged into security.json");
    return 0;
}

/* 迁移 state.json → registry */
static int migrate_state_to_registry(void) {
    const char *root = lingos_data_root();
    char state_path[512];
    safe_snprintf(state_path, sizeof(state_path), "%s/Ensystem/state.json", root);

    if (access(state_path, F_OK) != 0) return 0;

    FILE *fp = fopen(state_path, "r");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(fp); return -1; }
    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);
    cJSON *state_root = cJSON_Parse(buf);
    free(buf);
    if (!state_root) return -1;

    int configured = 0;
    char mode[16] = "app";
    cJSON *cfg = cJSON_GetObjectItem(state_root, "system_configured");
    if (cfg) configured = cJSON_IsTrue(cfg) ? 1 : 0;
    cJSON *mode_item = cJSON_GetObjectItem(state_root, "mode");
    if (mode_item && mode_item->valuestring) safe_strncpy(mode, mode_item->valuestring, sizeof(mode));

    cJSON_Delete(state_root);

    /* 写入 registry/core/registry.json 的元数据 */
    char registry_path[512];
    safe_snprintf(registry_path, sizeof(registry_path), "%s/registry/core/registry.json", root);
    fp = fopen(registry_path, "r");
    cJSON *reg_root = NULL;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long rlen = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        char *rbuf = malloc(rlen + 1);
        if (rbuf) {
            fread(rbuf, 1, rlen, fp);
            rbuf[rlen] = '\0';
            reg_root = cJSON_Parse(rbuf);
            free(rbuf);
        }
        fclose(fp);
    }
    if (!reg_root) reg_root = cJSON_CreateObject();

    cJSON_AddStringToObject(reg_root, "version", "1.0");
    cJSON_AddStringToObject(reg_root, "system_configured", configured ? "true" : "false");
    cJSON_AddStringToObject(reg_root, "mode", mode);
    char time_str[64];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", tm);
    cJSON_AddStringToObject(reg_root, "last_config_time", time_str);

    char *new_json = cJSON_PrintUnformatted(reg_root);
    cJSON_Delete(reg_root);
    if (!new_json) return -1;

    fp = fopen(registry_path, "w");
    if (!fp) { free(new_json); return -1; }
    fprintf(fp, "%s\n", new_json);
    fclose(fp);
    free(new_json);

    LOG_INFO_T("Migrate", "State", "OK", "state.json migrated to registry");
    return 0;
}

/* 注册表回滚功能 */
int migrate_rollback_registry(void) {
    const char *root = lingos_data_root();
    char backup_dir[512];
    safe_snprintf(backup_dir, sizeof(backup_dir), "%s/backups/config_migration", root);
    DIR *d = opendir(backup_dir);
    if (!d) {
        LOG_WARN_T("Migrate", "Rollback", "NoBackup", "no backup found");
        return -1;
    }
    /* 找最新的备份目录 */
    char latest[512] = {0};
    time_t latest_ts = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char full[512];
        safe_snprintf(full, sizeof(full), "%s/%s", backup_dir, entry->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (st.st_mtime > latest_ts) {
                latest_ts = st.st_mtime;
                safe_strncpy(latest, full, sizeof(latest));
            }
        }
    }
    closedir(d);
    if (latest[0] == '\0') return -1;

    /* 恢复 security.json, privilege.json, registry 等 */
    const char *files[] = {"/system/config/security.json", "/system/config/privilege.json",
                           "/registry/core/registry.json", NULL};
    for (int i = 0; files[i]; i++) {
        char src[512], dst[512];
        safe_snprintf(src, sizeof(src), "%s%s", latest, files[i]);
        safe_snprintf(dst, sizeof(dst), "%s%s", root, files[i]);
        if (access(src, F_OK) == 0) {
            char cmd[1024];
            safe_snprintf(cmd, sizeof(cmd), "cp '%s' '%s' 2>/dev/null", src, dst);
            system(cmd);
            LOG_INFO_T("Migrate", "Rollback", "OK", "restored %s", files[i]);
        }
    }
    return 0;
}

/* 主迁移入口 */
int migrate_config(void) {
    int current_version = get_config_version();
    char current_version_str[32] = {0};
    extract_version_number(LINGOS_VERSION, current_version_str, sizeof(current_version_str));
    int new_major = 0;
    sscanf(current_version_str, "%d", &new_major);

    if (current_version >= new_major) {
        LOG_DEBUG_T("Migrate", "Check", "OK", "Config version %d, no migration needed", current_version);
        return 0;
    }

    LOG_INFO_T("Migrate", "Start", "Begin", "Starting config migration from version %d to %s", current_version, current_version_str);

    /* 备份旧配置 */
    backup_old_configs();

    int ret = 0;
    ret |= migrate_defense_to_security();
    ret |= migrate_risk_to_security();
    ret |= migrate_state_to_registry();

    if (ret == 0) {
        write_config_version(current_version_str);
        LOG_INFO_T("Migrate", "Complete", "OK", "Config migration completed successfully to version %s", current_version_str);
    } else {
        LOG_ERROR_T("Migrate", "Complete", "Fail", "Config migration failed, attempting rollback");
        migrate_rollback_registry();
        ret = -1;
    }

    return ret;
}