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