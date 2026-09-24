/**
 * @file    alert_sources.c
 * @brief   默认数据源适配（USGS/EEW 四源/NMC 台风/国家预警中心）
 * @version LN-0.6.2
 * @par     核心协议：容错编程（API失败时返回空——绝不编造数据）
 * @changes 【2026-09-18 接线批次】：
 *          ① USGS 改 https（原 http 实际不可达——301）→ https_client
 *          ② 新增 EEW 秒级四源（wolfx cenc/sc/cwa/jma——生命线专项）
 *          ③ 台风重写：NMC 国家气象中心真实数据（原「编造默认事件」删除）
 *          ④ CN_WARNING 编造删除：未实现解析时诚实返回空
 */

#include "alert_sources.h"
#include "alert_utils.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include "../net/tcp_client.h"
#include "../net/https_client.h"
#include "../net/http_client.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

/* ============================================================
 * API 端点配置
 * ============================================================ */

#define USGS_API_URL "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/2.5_day.geojson"
/* EEW 秒级四源（生命线专项——已实测 https 可用；http 为 301） */
#define EEW_URL_CENC "https://api.wolfx.jp/cenc_eew.json"
#define EEW_URL_SC   "https://api.wolfx.jp/sc_eew.json"
#define EEW_URL_CWA  "https://api.wolfx.jp/cwa_eew.json"
#define EEW_URL_JMA  "https://api.wolfx.jp/jma_eew.json"
/* NMC 国家气象中心台风（真实数据源——http 可用） */
#define NMC_LIST_URL "http://typhoon.nmc.cn/weatherservice/typhoon/jsons/list_default"
#define NMC_VIEW_URL "http://typhoon.nmc.cn/weatherservice/typhoon/jsons/view_"
/* 第三方台风 API 配置（原 load_weather_api_config 空壳——2026-09-18 删除） */

/* ============================================================
 * USGS 地震 API（真实 HTTP——2026-09-18 改 https）
 * ============================================================ */

static int fetch_earthquake_usgs(alert_event_t *events, int max_count) {
    LOG_DEBUG_T("AlertSources", "USGS", "Enter", "fetching from USGS API");

    if (max_count < 1) return 0;

    /* 【2026-09-18】USGS 强制 https（http 返回 301——原实现在此静默失败）
     * 二段式：先验证证书（安全优先），失败降级不验证（生命线可用性优先）+ WARN */
    int code = 0;
    char *body_json = https_get_alloc(USGS_API_URL, 512 * 1024, 12, 1, &code);
    if (!body_json) {
        LOG_WARN_T("AlertSources", "USGS", "TLSVerify", "verified fetch failed — retry unverified (lifeline availability)");
        body_json = https_get_alloc(USGS_API_URL, 512 * 1024, 12, 0, &code);
    }
    if (!body_json || code != 200) {
        LOG_WARN_T("AlertSources", "USGS", "HTTPFail", "fetch failed (http=%d)", code);
        free(body_json);
        return 0;
    }

    cJSON *root = cJSON_Parse(body_json);
    free(body_json);
    if (!root) {
        LOG_WARN_T("AlertSources", "USGS", "JSONFail", "invalid GeoJSON");
        return 0;
    }

    cJSON *features = cJSON_GetObjectItem(root, "features");
    if (!features || !cJSON_IsArray(features)) {
        cJSON_Delete(root);
        LOG_WARN_T("AlertSources", "USGS", "NoFeatures", "no features in response");
        return 0;
    }

    int count = 0;
    int size = cJSON_GetArraySize(features);
    for (int i = 0; i < size && count < max_count; i++) {
        cJSON *feature = cJSON_GetArrayItem(features, i);
        if (!feature) continue;

        cJSON *properties = cJSON_GetObjectItem(feature, "properties");
        cJSON *geometry = cJSON_GetObjectItem(feature, "geometry");
        if (!properties || !geometry) continue;

        cJSON *mag = cJSON_GetObjectItem(properties, "mag");
        cJSON *place = cJSON_GetObjectItem(properties, "place");
        cJSON *time_prop = cJSON_GetObjectItem(properties, "time");
        cJSON *coordinates = cJSON_GetObjectItem(geometry, "coordinates");

        if (!mag || !cJSON_IsNumber(mag)) continue;
        if (!place || !cJSON_IsString(place)) continue;
        if (!coordinates || !cJSON_IsArray(coordinates)) continue;

        double magnitude = mag->valuedouble;
        if (magnitude < 2.5) continue;

        alert_event_t *ev = &events[count];
        memset(ev, 0, sizeof(alert_event_t));

        ev->type = ALERT_TYPE_EARTHQUAKE;
        ev->magnitude = magnitude;

        if (magnitude >= 6.0) ev->level = 4;
        else if (magnitude >= 5.0) ev->level = 3;
        else if (magnitude >= 4.0) ev->level = 2;
        else ev->level = 1;

        safe_strncpy(ev->source, "USGS", sizeof(ev->source));
        safe_strncpy(ev->location, place->valuestring, sizeof(ev->location));

        safe_snprintf(ev->description, sizeof(ev->description),
                      tr("Magnitude %.1f earthquake: %s", "%.1f级地震：%s"),
                      magnitude, place->valuestring);

        cJSON *lon = cJSON_GetArrayItem(coordinates, 0);
        cJSON *lat = cJSON_GetArrayItem(coordinates, 1);
        if (lon && cJSON_IsNumber(lon)) ev->longitude = lon->valuedouble;
        if (lat && cJSON_IsNumber(lat)) ev->latitude = lat->valuedouble;

        if (time_prop && cJSON_IsNumber(time_prop)) {
            ev->timestamp = (time_t)(time_prop->valuedouble / 1000.0);
        } else {
            ev->timestamp = time(NULL);
        }
        ev->expire_time = ev->timestamp + 3600;

        double user_lat, user_lon;
        alert_utils_get_user_location(&user_lat, &user_lon);
        ev->distance_km = (int)alert_utils_distance(user_lat, user_lon, ev->latitude, ev->longitude);

        count++;
    }

    cJSON_Delete(root);
    LOG_INFO_T("AlertSources", "USGS", "OK", "fetched %d earthquakes", count);
    return count;
}

/* ============================================================
 * 【2026-09-18 新增】EEW 秒级四源（wolfx——中国地震预警网/四川/台湾/日本）
 *   生命线专项：地震秒级预警（先生定位核心）
 *   字段：HypoCenter(Latitude/Longitude/Magnitude|Magunitude)；jma 用 Hypocenter
 *   isCancel/isTraining/isAssumption 跳过
 *   注：ReportNum 递增（同事件多次报告）——去重/更新由 alert 历史层处理
 * ============================================================ */

typedef struct {
    const char *url;
    const char *source;
} eew_source_t;

static int fetch_eew_one(const eew_source_t *src, alert_event_t *events, int max_count) {
    if (max_count < 1) return 0;

    int code = 0;
    char *body = https_get_alloc(src->url, 64 * 1024, 8, 1, &code);
    if (!body) {
        body = https_get_alloc(src->url, 64 * 1024, 8, 0, &code);
    }
    if (!body || code != 200) {
        LOG_WARN_T("AlertSources", "EEW", src->source, "fetch failed (http=%d)", code);
        free(body);
        return 0;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return 0;

    /* 取消报 / 训练报 / 模拟报 → 跳过 */
    cJSON *jc = cJSON_GetObjectItem(root, "isCancel");
    if (jc && cJSON_IsTrue(jc)) { cJSON_Delete(root); return 0; }
    cJSON *jt = cJSON_GetObjectItem(root, "isTraining");
    if (jt && cJSON_IsTrue(jt)) { cJSON_Delete(root); return 0; }
    cJSON *ja = cJSON_GetObjectItem(root, "isAssumption");
    if (ja && cJSON_IsTrue(ja)) { cJSON_Delete(root); return 0; }

    cJSON *jmag = cJSON_GetObjectItem(root, "Magnitude");
    if (!jmag) jmag = cJSON_GetObjectItem(root, "Magunitude");
    if (!jmag || !cJSON_IsNumber(jmag)) { cJSON_Delete(root); return 0; }
    double mag = jmag->valuedouble;

    alert_event_t *ev = &events[0];
    memset(ev, 0, sizeof(*ev));
    ev->type = ALERT_TYPE_EARTHQUAKE;
    ev->magnitude = mag;
    ev->level = mag >= 6.0 ? 4 : (mag >= 5.0 ? 3 : (mag >= 4.0 ? 2 : 1));
    safe_strncpy(ev->source, src->source, sizeof(ev->source));

    cJSON *loc = cJSON_GetObjectItem(root, "HypoCenter");
    if (!loc) loc = cJSON_GetObjectItem(root, "Hypocenter");
    if (loc && cJSON_IsString(loc)) safe_strncpy(ev->location, loc->valuestring, sizeof(ev->location));

    cJSON *lat = cJSON_GetObjectItem(root, "Latitude");
    cJSON *lon = cJSON_GetObjectItem(root, "Longitude");
    if (lat && cJSON_IsNumber(lat)) ev->latitude = lat->valuedouble;
    if (lon && cJSON_IsNumber(lon)) ev->longitude = lon->valuedouble;

    cJSON *otime = cJSON_GetObjectItem(root, "OriginTime");
    if (otime && cJSON_IsString(otime)) {
        safe_snprintf(ev->description, sizeof(ev->description),
                      tr("EEW M%.1f earthquake: %s (origin %s)", "EEW %.1f级地震：%s（发震 %s）"),
                      mag, ev->location, otime->valuestring);
    } else {
        safe_snprintf(ev->description, sizeof(ev->description),
                      tr("EEW M%.1f earthquake: %s", "EEW %.1f级地震：%s"), mag, ev->location);
    }
    ev->timestamp = time(NULL);
    ev->expire_time = ev->timestamp + 3600;

    double user_lat, user_lon;
    alert_utils_get_user_location(&user_lat, &user_lon);
    ev->distance_km = (int)alert_utils_distance(user_lat, user_lon, ev->latitude, ev->longitude);

    cJSON_Delete(root);
    return 1;
}

static int fetch_eew_wolfx(alert_event_t *events, int max_count) {
    static const eew_source_t srcs[] = {
        {EEW_URL_CENC, "CENC-EEW"},
        {EEW_URL_SC,   "SC-EEW"},
        {EEW_URL_CWA,  "CWA-EEW"},
        {EEW_URL_JMA,  "JMA-EEW"},
    };
    int count = 0;
    for (size_t i = 0; i < sizeof(srcs) / sizeof(srcs[0]) && count < max_count; i++) {
        int n = fetch_eew_one(&srcs[i], events + count, max_count - count);
        if (n > 0) {
            LOG_INFO_T("AlertSources", "EEW", srcs[i].source, "M%.1f %s",
                       events[count].magnitude, events[count].location);
            count += n;
        }
    }
    return count;
}

/* ============================================================
 * 【2026-09-18 重写】台风源：NMC 国家气象中心真实数据
 *   原实现为「编造默认事件」（API 未实现 + HTML 未实现 → 直接捏造南海台风）
 *   —— 本次接线：接 typhoon.nmc.cn JSONP 真实数据；无台风/失败 = 诚实返回 0
 *   数据路径：list_default（活动列表）→ view_<id>（路径点：最新观测）
 * ============================================================ */

/* JSONP 剥壳：typhoon_jsons_xxx(({...})) → {...}
 * 【2026-09-18 修复】NMC 实际响应为**双层括号** `name(({...}))`——
 * 原实现只剥一层 → 解析必失败（冒烟实测 ListParseFail）。现跳过全部前导 '('。 */
static char *strip_jsonp(char *s) {
    if (!s) return NULL;
    char *p1 = strchr(s, '(');
    if (!p1) return s;
    while (*p1 == '(') p1++;          /* 跳过 (( 双层包裹 */
    char *p2 = strrchr(s, ')');
    if (p2 > p1) *p2 = '\0';
    return p1;
}

static int fetch_typhoon_cma(alert_event_t *events, int max_count) {
    LOG_DEBUG_T("AlertSources", "NMC", "Enter", "fetching typhoon data (NMC real source)");

    if (max_count < 1) return 0;

    /* ---- 1) 活动台风列表 ---- */
    int code = 0;
    char *list_raw = https_get_alloc(NMC_LIST_URL, 256 * 1024, 10, 0, &code);
    if (!list_raw || code != 200) {
        LOG_WARN_T("AlertSources", "NMC", "ListFail", "fetch failed (http=%d)", code);
        free(list_raw);
        return 0;
    }
    cJSON *root = cJSON_Parse(strip_jsonp(list_raw));
    free(list_raw);
    if (!root) {
        LOG_WARN_T("AlertSources", "NMC", "ListParseFail", "invalid JSONP payload");
        return 0;
    }

    char active_id[32] = {0};
    char name_cn[64] = {0};
    char name_en[64] = {0};
    {
        cJSON *tl = cJSON_GetObjectItem(root, "typhoonList");
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, tl) {
            cJSON *st = cJSON_GetArrayItem(item, 7);
            if (st && cJSON_IsString(st) && strcmp(st->valuestring, "start") == 0) {
                cJSON *jid = cJSON_GetArrayItem(item, 0);
                cJSON *jen = cJSON_GetArrayItem(item, 1);
                cJSON *jcn = cJSON_GetArrayItem(item, 2);
                if (jid && cJSON_IsNumber(jid)) {
                    safe_snprintf(active_id, sizeof(active_id), "%.0f", jid->valuedouble);
                } else if (jid && cJSON_IsString(jid)) {
                    safe_strncpy(active_id, jid->valuestring, sizeof(active_id));
                }
                if (jen && cJSON_IsString(jen)) safe_strncpy(name_en, jen->valuestring, sizeof(name_en));
                if (jcn && cJSON_IsString(jcn)) safe_strncpy(name_cn, jcn->valuestring, sizeof(name_cn));
                break;
            }
        }
    }
    cJSON_Delete(root);

    if (!active_id[0]) {
        LOG_INFO_T("AlertSources", "NMC", "NoActive", "no active typhoon — honest empty (no fabrication)");
        return 0;
    }

    /* ---- 2) 详情（最新路径点） ---- */
    char url[256];
    safe_snprintf(url, sizeof(url), "%s%s", NMC_VIEW_URL, active_id);
    char *view_raw = https_get_alloc(url, 512 * 1024, 10, 0, &code);
    if (!view_raw || code != 200) {
        LOG_WARN_T("AlertSources", "NMC", "ViewFail", "fetch view_%s failed (http=%d)", active_id, code);
        free(view_raw);
        return 0;
    }
    cJSON *vroot = cJSON_Parse(strip_jsonp(view_raw));
    free(view_raw);
    if (!vroot) return 0;

    double lat = 0, lon = 0, pressure = 0, wind = 0;
    char grade[64] = {0}, obs_time[64] = {0};
    {
        cJSON *tv = cJSON_GetObjectItem(vroot, "typhoon");
        if (tv && cJSON_IsArray(tv)) {
            cJSON *points = cJSON_GetArrayItem(tv, 8);
            int np = (points && cJSON_IsArray(points)) ? cJSON_GetArraySize(points) : 0;
            if (np > 0) {
                cJSON *last = cJSON_GetArrayItem(points, np - 1);
                if (last && cJSON_IsArray(last)) {
                    cJSON *jt = cJSON_GetArrayItem(last, 1);
                    cJSON *jg = cJSON_GetArrayItem(last, 3);
                    cJSON *jlo = cJSON_GetArrayItem(last, 4);
                    cJSON *jla = cJSON_GetArrayItem(last, 5);
                    cJSON *jpr = cJSON_GetArrayItem(last, 6);
                    cJSON *jwi = cJSON_GetArrayItem(last, 7);
                    if (jt && cJSON_IsString(jt)) safe_strncpy(obs_time, jt->valuestring, sizeof(obs_time));
                    if (jg && cJSON_IsString(jg)) safe_strncpy(grade, jg->valuestring, sizeof(grade));
                    if (jlo && cJSON_IsNumber(jlo)) lon = jlo->valuedouble;
                    if (jla && cJSON_IsNumber(jla)) lat = jla->valuedouble;
                    if (jpr && cJSON_IsNumber(jpr)) pressure = jpr->valuedouble;
                    if (jwi && cJSON_IsNumber(jwi)) wind = jwi->valuedouble;
                }
            }
        }
    }
    cJSON_Delete(vroot);

    /* ---- 3) 生成事件（真实数据——无信息则返回 0，绝不编造） ---- */
    if (lat == 0 && lon == 0) {
        LOG_WARN_T("AlertSources", "NMC", "NoPoint", "typhoon %s has no path point yet", active_id);
        return 0;
    }

    alert_event_t *ev = &events[0];
    memset(ev, 0, sizeof(*ev));
    ev->type = ALERT_TYPE_TYPHOON;
    /* 强度关键字 → 等级（顺序敏感：先长后短） */
    int tlvl = 1;
    if (strstr(grade, "超强台风")) tlvl = 5;
    else if (strstr(grade, "强台风")) tlvl = 4;
    else if (strstr(grade, "台风")) tlvl = 3;
    else if (strstr(grade, "强热带风暴")) tlvl = 2;
    else if (strstr(grade, "热带风暴")) tlvl = 2;
    else if (strstr(grade, "热带低压")) tlvl = 1;
    ev->typhoon_level = tlvl;
    ev->level = tlvl >= 4 ? 4 : (tlvl >= 2 ? 2 : 1);
    safe_strncpy(ev->source, "NMC", sizeof(ev->source));
    if (name_cn[0]) {
        safe_strncpy(ev->location, name_cn, sizeof(ev->location));
    } else if (name_en[0]) {
        safe_strncpy(ev->location, name_en, sizeof(ev->location));
    } else {
        safe_strncpy(ev->location, tr("Western Pacific", "西太平洋"), sizeof(ev->location));
    }
    ev->latitude = lat;
    ev->longitude = lon;
    ev->pressure = (int)pressure;
    ev->wind_speed = (int)wind;
    safe_snprintf(ev->description, sizeof(ev->description),
                  tr("Typhoon %s (%s, NMC): %.1fN %.1fE, %d hPa%s%s",
                     "台风 %s（%s·NMC）：北纬%.1f 东经%.1f，%d hPa%s%s"),
                  name_cn[0] ? name_cn : active_id, grade[0] ? grade : "—",
                  lat, lon, (int)pressure,
                  obs_time[0] ? tr(", observed ", "，观测时间 ") : "",
                  obs_time[0] ? obs_time : "");
    ev->timestamp = time(NULL);
    ev->expire_time = ev->timestamp + 86400;

    double user_lat, user_lon;
    alert_utils_get_user_location(&user_lat, &user_lon);
    ev->distance_km = (int)alert_utils_distance(user_lat, user_lon, ev->latitude, ev->longitude);

    LOG_INFO_T("AlertSources", "NMC", "OK", "typhoon %s (%s) at %.1f,%.1f — real data",
               name_cn[0] ? name_cn : active_id, grade, lat, lon);
    return 1;
}

/* ============================================================
 * 【新增】国家预警中心（用户配置 + IP 省级过滤）
 * ============================================================ */

static int fetch_warning_cn(alert_event_t *events, int max_count) {
    LOG_DEBUG_T("AlertSources", "CN_Warning", "Enter", "fetching warning data");

    if (max_count < 1) return 0;

    const char *root = lingos_data_root();
    char config_path[512];
    safe_snprintf(config_path, sizeof(config_path), "%s/system/config/warning_cn.conf", root);

    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        LOG_DEBUG_T("AlertSources", "CN_Warning", "NoConfig", "warning_cn.conf not found, skipping");
        return 0;
    }

    char url[512] = {0};
    char province[64] = {0};
    char line[256];

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64], val[256];
        if (sscanf(line, "%63[^=]=%255s", key, val) == 2) {
            if (strcmp(key, "url") == 0) safe_strncpy(url, val, sizeof(url));
            else if (strcmp(key, "province") == 0) safe_strncpy(province, val, sizeof(province));
        }
    }
    fclose(fp);

    if (url[0] == '\0') {
        LOG_DEBUG_T("AlertSources", "CN_Warning", "NoURL", "no URL configured, skipping");
        return 0;
    }

    /* 如果用户未指定省份，标注未指定（不再假装 GeoIP 检测） */
    if (province[0] == '\0') {
        safe_strncpy(province, "-", sizeof(province));
    }

    LOG_DEBUG_T("AlertSources", "CN_Warning", "Config", "url=%s, province=%s", url, province);

    /* 【2026-09-18 改造】原「模拟数据（降级）」编造删除。
     * 现实现：拉取用户配置 URL → 解析标准 JSON 数组格式（下方），解析失败/格式不符 → 诚实返回空。
     * 标准格式：[{"title":"...","level":1-4,"location":"...","description":"...","lat":x,"lon":y}] */
    int code = 0;
    char *raw = https_get_alloc(url, 256 * 1024, 10, 0, &code);
    if (!raw || (code != 200 && code != 0)) {
        LOG_WARN_T("AlertSources", "CN_Warning", "FetchFail", "url fetch failed (http=%d) — honest empty", code);
        free(raw);
        return 0;
    }

    cJSON *jroot = cJSON_Parse(raw);
    free(raw);
    if (!jroot) {
        LOG_WARN_T("AlertSources", "CN_Warning", "ParseFail",
                   "response not valid JSON (expected array of {title,level,location,description}) — honest empty");
        return 0;
    }

    cJSON *arr = jroot;
    if (cJSON_IsObject(jroot)) {
        cJSON *w = cJSON_GetObjectItem(jroot, "warnings");
        if (w) arr = w;
    }

    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (count >= max_count) break;
        if (!cJSON_IsObject(item)) continue;

        alert_event_t *ev = &events[count];
        memset(ev, 0, sizeof(*ev));
        ev->type = ALERT_TYPE_RAIN;
        ev->level = 2;
        cJSON *jl = cJSON_GetObjectItem(item, "level");
        if (jl && cJSON_IsNumber(jl)) {
            int lv = (int)jl->valuedouble;
            ev->level = (lv >= 0 && lv <= 5) ? lv : 2;
        }
        safe_strncpy(ev->source, "CN_WARNING", sizeof(ev->source));
        cJSON *jt = cJSON_GetObjectItem(item, "title");
        cJSON *jl2 = cJSON_GetObjectItem(item, "location");
        cJSON *jd = cJSON_GetObjectItem(item, "description");
        safe_strncpy(ev->location, (jl2 && cJSON_IsString(jl2)) ? jl2->valuestring
                                                            : province, sizeof(ev->location));
        safe_snprintf(ev->description, sizeof(ev->description), "%s%s%s",
                      (jt && cJSON_IsString(jt)) ? jt->valuestring : tr("Warning", "预警"),
                      (jd && cJSON_IsString(jd)) ? "： " : "",
                      (jd && cJSON_IsString(jd)) ? jd->valuestring : "");
        cJSON *jlat = cJSON_GetObjectItem(item, "lat");
        cJSON *jlon = cJSON_GetObjectItem(item, "lon");
        if (jlat && cJSON_IsNumber(jlat)) ev->latitude = jlat->valuedouble;
        if (jlon && cJSON_IsNumber(jlon)) ev->longitude = jlon->valuedouble;
        cJSON *jrain = cJSON_GetObjectItem(item, "rainfall_24h");
        if (jrain && cJSON_IsNumber(jrain)) ev->rainfall_24h = jrain->valuedouble;
        ev->timestamp = time(NULL);
        ev->expire_time = ev->timestamp + 86400;
        count++;
    }
    cJSON_Delete(jroot);

    if (count == 0) {
        LOG_INFO_T("AlertSources", "CN_Warning", "Empty", "no warnings in response — honest empty");
    } else {
        LOG_INFO_T("AlertSources", "CN_Warning", "OK", "parsed %d warnings (user source)", count);
    }
    return count;
}

/* ============================================================
 * 主获取函数
 * ============================================================ */

int alert_sources_fetch_all(alert_event_t *events, int max_count) {
    if (!events || max_count <= 0) {
        LOG_DEBUG_T("AlertSources", "FetchAll", "Invalid", "events=%p, max_count=%d", (void*)events, max_count);
        return 0;
    }

    int count = 0;

    /* 【2026-09-18】EEW 秒级四源（生命线专项——地震秒级） */
    int n = fetch_eew_wolfx(events + count, max_count - count);
    count += n;

    /* 中国天气网台风（含第三方 API） */
    n = fetch_typhoon_cma(events + count, max_count - count);
    count += n;

    /* USGS 地震 */
    n = fetch_earthquake_usgs(events + count, max_count - count);
    count += n;

    /* 国家预警中心（用户配置） */
    n = fetch_warning_cn(events + count, max_count - count);
    count += n;

    LOG_INFO_T("AlertSources", "FetchAll", "OK", "fetched %d events total", count);
    return count;
}