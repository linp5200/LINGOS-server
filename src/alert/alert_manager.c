/**
 * @file    alert_manager.c
 * @brief   预警核心逻辑（数据获取/合并/异常判断）
 * @version LN-B-5.0.0.0
 * @par     核心协议：容错编程 + 防弹编程
 * @changes 绝对保护豁免（level>=3 强制通知）；安全字符串替换
 */

#include "alert_manager.h"
#include "alert_sources.h"
#include "alert_notify.h"
#include "alert_history.h"
#include "alert_utils.h"
#include "plugin_loader.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ============================================================
 * 全局状态
 * ============================================================ */

static int g_has_exception = 0;
static alert_event_t g_last_events[16];
static int g_event_count = 0;

/* ============================================================
 * 多源数据合并（多数投票 + 国家分级权重）
 * ============================================================ */

static alert_event_t merge_events(alert_event_t *events, int count, const alert_config_t *config) {
    /* 【第9期】当前合并策略（国家优先/多数投票/加权平均）不依赖 config；
     *   参数保留以兼容既有调用与后续"可配置合并权重"，显式标注避免 -Wextra 噪音。 */
    (void)config;

    alert_event_t result;
    memset(&result, 0, sizeof(result));

    if (count == 0) return result;
    if (count == 1) return events[0];

    /* 按国家优先级过滤（中国最高） */
    int china_index = -1;
    for (int i = 0; i < count; i++) {
        if (alert_utils_is_china_source(events[i].source)) {
            china_index = i;
            break;
        }
    }
    if (china_index >= 0) {
        LOG_DEBUG_T("AlertManager", "Merge", "ChinaPriority", "using China source: %s", events[china_index].source);
        return events[china_index];
    }

    /* 多数投票 */
    if (count >= 3) {
        int level_counts[6] = {0};
        for (int i = 0; i < count; i++) {
            if (events[i].level >= 0 && events[i].level <= 5) {
                level_counts[events[i].level]++;
            }
        }
        int max_level = 0, max_count = 0;
        for (int i = 0; i < 6; i++) {
            if (level_counts[i] > max_count) {
                max_count = level_counts[i];
                max_level = i;
            }
        }
        if (max_count >= 2) {
            LOG_DEBUG_T("AlertManager", "Merge", "Vote", "voted level=%d", max_level);
            result.level = max_level;
            safe_strncpy(result.source, "merged(vote)", sizeof(result.source));
            return result;
        }
    }

    /* 加权平均 */
    double weighted_level = 0;
    double total_weight = 0;
    for (int i = 0; i < count; i++) {
        double weight = alert_utils_get_source_weight(events[i].source);
        weighted_level += events[i].level * weight;
        total_weight += weight;
    }
    if (total_weight > 0) {
        result.level = (int)round(weighted_level / total_weight);
        if (result.level > 5) result.level = 5;
        if (result.level < 0) result.level = 0;
        safe_strncpy(result.source, "merged(weighted)", sizeof(result.source));
        LOG_DEBUG_T("AlertManager", "Merge", "Weighted", "weighted level=%d", result.level);
    } else {
        result = events[0];
    }

    return result;
}

/* ============================================================
 * 异常判断
 * ============================================================ */

static int check_exception(const alert_event_t *event, const alert_config_t *config) {
    if (!event) return 0;

    /* 强制启用级别（橙色及以上不可禁用） */
    if (event->level >= 3) {
        LOG_DEBUG_T("AlertManager", "Exception", "ForceEnabled", "level=%d forced", event->level);
        return 1;
    }

    if (event->type == ALERT_TYPE_TYPHOON) {
        if (event->distance_km >= 0 && event->distance_km < config->typhoon_distance_threshold) {
            LOG_DEBUG_T("AlertManager", "Exception", "TyphoonDistance", "distance=%d km", event->distance_km);
            return 1;
        }
        if (event->typhoon_level >= config->typhoon_level_threshold) {
            LOG_DEBUG_T("AlertManager", "Exception", "TyphoonLevel", "level=%d", event->typhoon_level);
            return 1;
        }
    }

    if (event->type == ALERT_TYPE_EARTHQUAKE) {
        if (event->magnitude >= config->earthquake_magnitude_threshold) {
            LOG_DEBUG_T("AlertManager", "Exception", "Earthquake", "magnitude=%.1f", event->magnitude);
            return 1;
        }
        if (event->felt) {
            LOG_DEBUG_T("AlertManager", "Exception", "EarthquakeFelt", "felt detected");
            return 1;
        }
    }

    if (event->type == ALERT_TYPE_RAIN) {
        if (event->rainfall_24h >= config->rainfall_threshold) {
            LOG_DEBUG_T("AlertManager", "Exception", "Rainfall", "rain=%.1f mm", event->rainfall_24h);
            return 1;
        }
    }

    /* R7: 系统健康（level>=1 即触发——由阈值判断决定是否产生事件） */
    if (event->type == ALERT_TYPE_HEALTH && event->level >= 1) {
        return 1;
    }
    /* R7: 安全威胁 */
    if (event->type == ALERT_TYPE_SECURITY && event->level >= 2) {
        return 1;
    }

    if (config->custom_exception_check) {
        if (config->custom_exception_check((const void *)event)) {
            return 1;
        }
    }

    return 0;
}

/* ============================================================
 * 主检查函数
 * ============================================================ */

void alert_manager_check_all(const alert_config_t *config) {
    LOG_DEBUG_T("AlertManager", "CheckAll", "Enter", "starting check cycle");

    alert_event_t events[32];
    int event_count = 0;

    alert_event_t builtin_events[16];
    int builtin_count = alert_sources_fetch_all(builtin_events, 16);
    for (int i = 0; i < builtin_count && event_count < 32; i++) {
        events[event_count++] = builtin_events[i];
    }

    alert_event_t plugin_events[16];
    int plugin_count = plugin_loader_fetch_all(plugin_events, 16);
    for (int i = 0; i < plugin_count && event_count < 32; i++) {
        events[event_count++] = plugin_events[i];
    }

    /* ============================================================
     * 【第9期·生命线守护】"数据源全挂"独立上报通道
     *   原行为：event_count==0 → 只打 DEBUG 后 return，用户侧完全静默
     *           （把"预警能力失效"误读成"一切正常"——最危险的漏报形态）
     *   现行为：全源不可达 → 合成 HEALTH level=3 事件，**不参与 merge/多数投票**
     *           （避免被合并降级），直接走绝对保护豁免强制通知 → App/Qt/Web 可见；
     *           首次立即报，持续期间每 30 分钟提醒一次，恢复自动复位。
     * ============================================================ */
    alert_event_t degraded;
    if (alert_sources_degraded_event(&degraded)) {
        LOG_ERROR_T("AlertManager", "CheckAll", "SourcesDegraded",
                    "all alert sources unreachable — forcing degraded-coverage alert (source=%s)",
                    degraded.source);
        if (alert_history_save(&degraded) != 0) {
            LOG_ERROR_T("AlertManager", "CheckAll", "HistoryWriteFail",
                        "degraded-coverage alert could not be persisted (disk full / read-only?) — notifying anyway");
        }
        alert_notify_force_send(&degraded);
        g_has_exception = 1;   /* 保持高频轮询——尽快发现恢复 */
    }

    if (event_count == 0) {
        LOG_DEBUG_T("AlertManager", "CheckAll", "NoEvents", "no events from any source");
        return;
    }

    alert_event_t merged = merge_events(events, event_count, config);

    int is_exception = check_exception(&merged, config);
    g_has_exception = is_exception;

    if (is_exception) {
        LOG_INFO_T("AlertManager", "CheckAll", "Exception", "alert triggered: type=%d, level=%d",
                   merged.type, merged.level);

        /* 【第9期·不静默】历史写入失败必须可见（磁盘满/只读时预警无痕迹，排障会失去线索） */
        if (alert_history_save(&merged) != 0) {
            LOG_ERROR_T("AlertManager", "CheckAll", "HistoryWriteFail",
                        "alert level=%d triggered but history write FAILED (disk full / read-only?) — notifying anyway",
                        merged.level);
        }

        /* 【修改】绝对保护豁免：level>=3 时强制通知 */
        if (merged.level >= 3) {
            LOG_INFO_T("AlertManager", "CheckAll", "AbsoluteProtect", "level>=3, force sending notification");
            alert_notify_force_send(&merged);
        } else {
            alert_notify_send(&merged, config);
        }
    } else {
        LOG_DEBUG_T("AlertManager", "CheckAll", "NoException", "no exception detected");
    }

    if (event_count > 0) {
        g_event_count = event_count < 16 ? event_count : 16;
        for (int i = 0; i < g_event_count; i++) {
            g_last_events[i] = events[i];
        }
    }
}

/* ============================================================
 * 查询接口（供 AI 技能调用）
 * ============================================================ */

int alert_manager_query(const char *location, const char *type_str, int time_range_hours,
                        alert_event_t *out, int max_count) {
    LOG_DEBUG_T("AlertManager", "Query", "Enter", "location=%s, type=%s, range=%d",
                location ? location : "(null)", type_str ? type_str : "(null)", time_range_hours);

    if (!out || max_count <= 0) return 0;

    int count = alert_history_query(location, type_str, time_range_hours, out, max_count);

    if (count == 0 && g_event_count > 0) {
        for (int i = 0; i < g_event_count && count < max_count; i++) {
            out[count++] = g_last_events[i];
        }
    }

    LOG_DEBUG_T("AlertManager", "Query", "Result", "returned %d events", count);
    return count;
}

int alert_manager_has_exception(void) {
    return g_has_exception;
}

int alert_manager_get_latest(alert_event_t *out, int max_count) {
    if (!out || max_count <= 0) return 0;
    int count = g_event_count < max_count ? g_event_count : max_count;
    for (int i = 0; i < count; i++) {
        out[i] = g_last_events[i];
    }
    return count;
}