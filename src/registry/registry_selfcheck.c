/**
 * @file    registry_selfcheck.c
 * @brief   自检回调注册与执行
 * @version LN-B-5.0.0.0
 * @changes 修复回调执行期间持有锁导致死锁的风险
 */

#include "registry.h"
#include "common/safe_string.h"
#include "lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef struct {
    char id[128];
    char module[64];
    int (*callback)(void);
    int priority;
    int enabled;
} selfcheck_entry_t;

static selfcheck_entry_t g_selfchecks[64];
static int g_selfcheck_count = 0;
static pthread_mutex_t g_sc_lock = PTHREAD_MUTEX_INITIALIZER;

int registry_register_selfcheck(const char *id, const char *module, int (*cb)(void), int priority) {
    if (!id || !module || !cb) return -1;

    pthread_mutex_lock(&g_sc_lock);

    if (g_selfcheck_count >= 64) {
        pthread_mutex_unlock(&g_sc_lock);
        LOG_ERROR_T("RegistrySelfCheck", "Register", "Overflow", "max selfchecks reached");
        return -1;
    }

    safe_strncpy(g_selfchecks[g_selfcheck_count].id, id, sizeof(g_selfchecks[0].id));
    safe_strncpy(g_selfchecks[g_selfcheck_count].module, module, sizeof(g_selfchecks[0].module));
    g_selfchecks[g_selfcheck_count].callback = cb;
    g_selfchecks[g_selfcheck_count].priority = priority;
    g_selfchecks[g_selfcheck_count].enabled = 1;
    g_selfcheck_count++;

    pthread_mutex_unlock(&g_sc_lock);
    LOG_INFO_T("RegistrySelfCheck", "Register", "OK", "registered '%s' for module '%s'", id, module);
    return 0;
}

int registry_run_all_selfchecks(void) {
    LOG_INFO_T("RegistrySelfCheck", "RunAll", "Enter", "running %d selfchecks", g_selfcheck_count);

    /* 复制当前自检列表到本地（避免回调中修改导致死锁） */
    selfcheck_entry_t local_checks[64];
    int local_count = 0;

    pthread_mutex_lock(&g_sc_lock);
    local_count = g_selfcheck_count;
    if (local_count > 64) local_count = 64;
    memcpy(local_checks, g_selfchecks, local_count * sizeof(selfcheck_entry_t));
    pthread_mutex_unlock(&g_sc_lock);

    /* 按优先级排序 */
    for (int i = 0; i < local_count - 1; i++) {
        for (int j = i + 1; j < local_count; j++) {
            if (local_checks[i].priority > local_checks[j].priority) {
                selfcheck_entry_t tmp = local_checks[i];
                local_checks[i] = local_checks[j];
                local_checks[j] = tmp;
            }
        }
    }

    int passed = 0, failed = 0;
    for (int i = 0; i < local_count; i++) {
        if (!local_checks[i].enabled) continue;
        LOG_DEBUG_T("RegistrySelfCheck", "Run", "Check", "running '%s'", local_checks[i].id);
        int ret = local_checks[i].callback();
        if (ret == 0) passed++;
        else {
            failed++;
            LOG_WARN_T("RegistrySelfCheck", "Run", "Fail", "'%s' failed with %d", local_checks[i].id, ret);
        }
    }

    LOG_INFO_T("RegistrySelfCheck", "RunAll", "Done", "passed=%d, failed=%d", passed, failed);
    return (failed == 0) ? 0 : -1;
}

/* ============================================================
 * 【2026-10-11 第 4 期】内置真实自检项（P5：registry_run_selfcheck 实装）
 * ------------------------------------------------------------
 * 每一项都是"可验证的真实检查"，不是占位：文件存在性/JSON 可解析/目录可写/
 * socket 存在/审计链校验。失败只报告事实，不伪造通过。
 * ============================================================ */
#include "cJSON.h"
#include "../common/data_path.h"
#include "../security/audit.h"

#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static int sc_config_unified_parseable(void) {
    char path[512];
    FILE *fp;
    char *buf;
    long len;
    size_t rd;
    cJSON *root;
    char root_dir[512];

    (void)safe_snprintf(root_dir, sizeof(root_dir), "%s", lingos_data_root() ? lingos_data_root() : "/LINGOS");
    safe_snprintf(path, sizeof(path), "%s/system/config/config.json", root_dir);
    fp = fopen(path, "r");
    if (!fp) return 0;   /* 未迁移设备（旧文件族）→ 不判失败 */
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > (long)(4 * 1024 * 1024)) { fclose(fp); return -1; }
    buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return -1; }
    rd = fread(buf, 1, (size_t)len, fp);
    buf[rd] = '\0';
    fclose(fp);
    root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        LOG_WARN_T("SelfCheck", "ConfigUnified", "ParseFail", "config.json exists but is not valid JSON");
        return -1;
    }
    cJSON_Delete(root);
    return 0;
}

static int sc_run_dir_writable(void) {
    char dir[512];
    char probe[512];
    FILE *fp;
    const char *root = lingos_data_root();
    if (!root) return -1;
    safe_snprintf(dir, sizeof(dir), "%s/run", root);
    mkdir(dir, 0755);
    if (access(dir, W_OK) != 0) {
        LOG_WARN_T("SelfCheck", "RunDir", "NotWritable", "%s not writable (%s)", dir, strerror(errno));
        return -1;
    }
    safe_snprintf(probe, sizeof(probe), "%s/.selfcheck_probe", dir);
    fp = fopen(probe, "w");
    if (!fp) return -1;
    fputs("ok\n", fp);
    fclose(fp);
    unlink(probe);
    return 0;
}

static int sc_daemon_socket_present(void) {
    char path[512];
    struct stat st;
    const char *root = lingos_data_root();
    if (!root) return -1;
    safe_snprintf(path, sizeof(path), "%s/run/daemon.sock", root);
    if (stat(path, &st) != 0) {
        LOG_WARN_T("SelfCheck", "DaemonSock", "Missing", "%s missing", path);
        return -1;
    }
    if (!S_ISSOCK(st.st_mode)) {
        LOG_WARN_T("SelfCheck", "DaemonSock", "NotSocket", "%s is not a socket", path);
        return -1;
    }
    return 0;
}

static int sc_registry_index_parseable(void) {
    char path[512];
    const char *root = lingos_data_root();
    FILE *fp;
    char *buf;
    long len;
    size_t rd;
    cJSON *j;
    if (!root) return -1;
    safe_snprintf(path, sizeof(path), "%s/registry/core/registry.json", root);
    fp = fopen(path, "r");
    if (!fp) return 0;   /* 尚无注册表文件：非失败 */
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0) { fclose(fp); return 0; }
    buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return -1; }
    rd = fread(buf, 1, (size_t)len, fp);
    buf[rd] = '\0';
    fclose(fp);
    j = cJSON_Parse(buf);
    free(buf);
    if (!j) {
        LOG_WARN_T("SelfCheck", "RegistryIndex", "ParseFail", "registry.json invalid");
        return -1;
    }
    cJSON_Delete(j);
    return 0;
}

static int sc_audit_chain(void) {
    char err[256];
    err[0] = '\0';
    if (audit_verify(err, sizeof(err)) != 0) {
        LOG_WARN_T("SelfCheck", "AuditChain", "VerifyFail", "%s", err[0] ? err : "audit verify failed");
        return -1;
    }
    return 0;
}

int registry_register_builtin_selfchecks(void) {
    /* 幂等：先查是否已注册（id 唯一） */
    pthread_mutex_lock(&g_sc_lock);
    for (int i = 0; i < g_selfcheck_count; i++) {
        if (strcmp(g_selfchecks[i].id, "config.unified_parse") == 0) {
            pthread_mutex_unlock(&g_sc_lock);
            return 0;   /* 已注册 */
        }
    }
    pthread_mutex_unlock(&g_sc_lock);

    (void)registry_register_selfcheck("config.unified_parse", "config", sc_config_unified_parseable, 10);
    (void)registry_register_selfcheck("fs.run_dir_writable", "fs", sc_run_dir_writable, 20);
    (void)registry_register_selfcheck("daemon.socket_present", "daemon", sc_daemon_socket_present, 30);
    (void)registry_register_selfcheck("registry.index_parse", "registry", sc_registry_index_parseable, 40);
    (void)registry_register_selfcheck("security.audit_chain", "security", sc_audit_chain, 50);
    LOG_INFO_T("RegistrySelfCheck", "Builtin", "OK", "registered %d builtin selfchecks", g_selfcheck_count);
    return 0;
}

int registry_selfcheck_run_module(const char *module) {
    selfcheck_entry_t local_checks[64];
    int local_count = 0;
    int ran = 0, failed = 0;
    if (!module) return -1;

    pthread_mutex_lock(&g_sc_lock);
    local_count = g_selfcheck_count;
    if (local_count > 64) local_count = 64;
    memcpy(local_checks, g_selfchecks, (size_t)local_count * sizeof(selfcheck_entry_t));
    pthread_mutex_unlock(&g_sc_lock);

    for (int i = 0; i < local_count; i++) {
        if (!local_checks[i].enabled) continue;
        if (strcmp(local_checks[i].module, module) != 0) continue;
        ran++;
        if (local_checks[i].callback() != 0) failed++;
    }
    LOG_INFO_T("RegistrySelfCheck", "RunModule", "Done", "module='%s' ran=%d failed=%d", module, ran, failed);
    if (ran == 0) return -1;      /* 没有该模块的自检项 = 未注册 */
    return (failed == 0) ? 0 : -1;
}

cJSON* registry_selfcheck_report(void) {
    selfcheck_entry_t local_checks[64];
    int local_count = 0;
    int passed = 0, failed = 0;
    cJSON *report = cJSON_CreateObject();
    cJSON *arr;
    if (!report) return NULL;

    pthread_mutex_lock(&g_sc_lock);
    local_count = g_selfcheck_count;
    if (local_count > 64) local_count = 64;
    memcpy(local_checks, g_selfchecks, (size_t)local_count * sizeof(selfcheck_entry_t));
    pthread_mutex_unlock(&g_sc_lock);

    arr = cJSON_CreateArray();
    if (!arr) { cJSON_Delete(report); return NULL; }
    for (int i = 0; i < local_count; i++) {
        cJSON *item = cJSON_CreateObject();
        int rc = local_checks[i].enabled ? local_checks[i].callback() : -2;
        if (!item) continue;
        cJSON_AddStringToObject(item, "id", local_checks[i].id);
        cJSON_AddStringToObject(item, "module", local_checks[i].module);
        cJSON_AddStringToObject(item, "status", rc == 0 ? "pass" : (rc == -2 ? "disabled" : "fail"));
        cJSON_AddItemToArray(arr, item);
        if (rc == 0) passed++;
        else if (rc != -2) failed++;
    }
    cJSON_AddNumberToObject(report, "total", local_count);
    cJSON_AddNumberToObject(report, "passed", passed);
    cJSON_AddNumberToObject(report, "failed", failed);
    cJSON_AddItemToObject(report, "checks", arr);
    return report;
}
