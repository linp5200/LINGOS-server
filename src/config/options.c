/**
 * @file    src/config/options.c
 * @brief   可选项开关体系实现（先生 2026-09-12 定稿）
 * @version LN-0.5.0
 *
 * 存储：/LINGOS/system/config/options.json（中敏 —— 可加密，见 sensitive_data）
 * 兼容：文件不存在 → 全部用默认值（旧版本升级无感）
 */

#include "options.h"
#include "../lib/cJSON/cJSON.h"
#include "../lib/log_extra.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * 定义表（先生定稿：7 底线 + 57 可选）
 * ============================================================ */
static const option_def_t g_options[] = {
/* ── 🔒 安全底线（不可选）──────────────────────────────── */
{ "security.token_no_log",       "Token/密钥不入日志", "Token/Key no-log",
  OPT_GROUP_SECURITY, OPT_KIND_FIXED, 0, 0,
  "敏感凭据在日志中一律脱敏（安全底线，不可关闭）", "Credentials are always redacted in logs" },
{ "security.rand_no_downgrade",  "随机源不降级", "No RNG downgrade",
  OPT_GROUP_SECURITY, OPT_KIND_FIXED, 0, 0,
  "安全随机源不可用时拒绝操作，绝不回退到弱随机", "Refuse operations when CSPRNG unavailable" },
{ "security.encrypted_honest",   "加密状态如实上报", "Honest encryption status",
  OPT_GROUP_SECURITY, OPT_KIND_FIXED, 0, 0,
  "不再声称未实际启用的加密", "Never claim encryption that isn't active" },
{ "security.skill_permission",   "技能执行前权限校验", "Skill permission check",
  OPT_GROUP_SECURITY, OPT_KIND_FIXED, 0, 0,
  "每次技能调用都经权限系统（Complete mediation）", "Every skill call mediated by permission system" },
{ "security.danger_confirm",     "危险操作强制确认", "Mandatory confirm",
  OPT_GROUP_SECURITY, OPT_KIND_FIXED, 0, 0,
  "rm -rf 等不可逆操作必须二次确认", "Irreversible operations require confirmation" },
{ "security.defense_no_disable", "防御系统不可自动禁用", "Defense cannot self-disable",
  OPT_GROUP_SECURITY, OPT_KIND_FIXED, 0, 0,
  "AI 不得自动关闭防御机制", "AI must not disable defense automatically" },
{ "security.path_normalize",     "路径规范化后校验", "Normalize paths",
  OPT_GROUP_SECURITY, OPT_KIND_FIXED, 0, 0,
  "realpath 规范化，防 ../ 穿越", "realpath before validation" },

/* ── 🟢 安全增强（默认开）────────────────────────────── */
{ "sec.encrypt_transport",   "通讯加密", "Transport encryption",
  OPT_GROUP_SECURITY, OPT_KIND_ON, 0, 0,
  "X25519+AEAD 应用层加密（对端支持时自动启用）", "App-layer encryption when peer supports" },
{ "sec.api_token_public",    "公网访问需 Token", "Token for public access",
  OPT_GROUP_SECURITY, OPT_KIND_ON, 0, 0, "公网访问 /api 需 Bearer token", "Bearer token required from public network" },
{ "sec.highrisk_confirm",    "高风险命令二次确认", "Confirm high-risk",
  OPT_GROUP_SECURITY, OPT_KIND_ON, 0, 0, "局域网内高风险命令需确认", "Confirm high-risk commands on LAN" },
{ "sec.allow_lan_highrisk",  "允许局域网运行高风险命令", "Allow high-risk on LAN",
  OPT_GROUP_SECURITY, OPT_KIND_OFF, 1, 0,
  "⚠️ 开启后局域网可免确认执行高风险命令（危险）", "⚠️ LAN may run high-risk commands without confirmation" },
{ "sec.rate_limit",          "API 限流", "API rate limit",
  OPT_GROUP_SECURITY, OPT_KIND_ON, 0, 0, "限制每 IP 请求频率（防爆破/DoS）", "Per-IP rate limiting" },
{ "sec.allow_lan_no_ratelimit", "允许局域网内连接不限流", "No rate limit on LAN",
  OPT_GROUP_SECURITY, OPT_KIND_OFF, 0, 0,
  "关闭局域网限流（内网调试用）", "Disable rate limiting for LAN" },
{ "sec.cors_strict",         "CORS 校验来源", "Strict CORS",
  OPT_GROUP_SECURITY, OPT_KIND_ON, 1, 0,
  "仅允许同源/本机来源（关闭将允许任意站点跨域，危险）", "Enforce same-origin (dangerous to disable)" },
{ "sec.ssrf_protect",        "SSRF 防护", "SSRF protection",
  OPT_GROUP_SECURITY, OPT_KIND_ON, 1, 0,
  "阻断内网/云元数据地址访问（关闭危险）", "Block internal/metadata addresses" },
{ "sec.egress_allowlist",    "出口白名单", "Egress allowlist",
  OPT_GROUP_SECURITY, OPT_KIND_ON, 0, 0, "出站请求仅允许清单内目标", "Allowlisted outbound targets only" },
{ "sec.update_signature",    "更新签名校验", "Update signature",
  OPT_GROUP_SECURITY, OPT_KIND_ON, 0, 0, "Ed25519 校验更新包来源", "Verify update packages with Ed25519" },
{ "sec.audit_log",           "审计日志", "Audit log",
  OPT_GROUP_SECURITY, OPT_KIND_ON, 0, 0, "记录命令/权限/网络行为", "Log commands/permissions/network" },
{ "sec.encrypt_sensitive",   "敏感内容加密", "Encrypt sensitive data",
  OPT_GROUP_SECURITY, OPT_KIND_ON, 0, 0, "按分级加密高敏+中敏数据", "Encrypt HIGH+MEDIUM data at rest" },
{ "sec.encrypt_backup",      "备份加密", "Encrypt backups",
  OPT_GROUP_SECURITY, OPT_KIND_ON, 0, 0, "备份包整体加密", "Encrypt backup archives" },
{ "sec.encrypt_apikey",      "API Key 加密存储", "Encrypt API keys",
  OPT_GROUP_SECURITY, OPT_KIND_ON, 0, 0, "provider.json 中的密钥加密", "Encrypt keys in provider.json" },

/* ── 🔵 隐私技术（默认关）────────────────────────────── */
{ "priv.homomorphic",   "同态加密 (HE)", "Homomorphic encryption",
  OPT_GROUP_PRIVACY, OPT_KIND_OFF, 0, 1,
  "云端可见密文不可见明文；需云端算力且较慢", "Compute on ciphertext; needs cloud, slow" },
{ "priv.diff_privacy",  "差分隐私 (DP)", "Differential privacy",
  OPT_GROUP_PRIVACY, OPT_KIND_OFF, 0, 1, "统计上报加噪，防个体反推", "Noise for statistics" },
{ "priv.federated",     "联邦学习 (FL)", "Federated learning",
  OPT_GROUP_PRIVACY, OPT_KIND_OFF, 0, 1, "数据不出本机，仅交换模型；需多设备", "Data stays local; needs multiple devices" },
{ "priv.e2ee",          "端到端加密", "End-to-end encryption",
  OPT_GROUP_PRIVACY, OPT_KIND_OFF, 0, 1, "App↔Server 全链路加密", "Full-path encryption" },
{ "priv.anonymous",     "匿名化模式", "Anonymous mode",
  OPT_GROUP_PRIVACY, OPT_KIND_OFF, 0, 0, "隐藏设备标识/主机名", "Hide device identifiers" },
{ "priv.local_infer",   "本地推理优先", "Prefer local inference",
  OPT_GROUP_PRIVACY, OPT_KIND_OFF, 0, 1, "优先用本机模型，减少外发", "Prefer on-device models" },

/* ── 🔵 功能增强 ─────────────────────────────────────── */
{ "feat.typhoon_predict",  "台风路径预测", "Typhoon prediction",
  OPT_GROUP_FEATURE, OPT_KIND_OFF, 0, 0, "AI + 历史匹配预测台风路径", "Predict typhoon tracks" },
{ "feat.weather_alert_link","天气↔预警联动", "Weather-alert link",
  OPT_GROUP_FEATURE, OPT_KIND_OFF, 0, 0, "暴雨/高温自动生成预警", "Auto-generate alerts from weather" },
{ "feat.alert_subscribe",  "预警级别订阅", "Alert subscription",
  OPT_GROUP_FEATURE, OPT_KIND_ON, 0, 0, "按级别过滤推送", "Filter alerts by level" },
{ "feat.alert_forecast",   "预警预告", "Alert forecast",
  OPT_GROUP_FEATURE, OPT_KIND_ON, 0, 0, "预测型预警（CAP urgency=Future）", "Forecast alerts (CAP Future)" },
{ "feat.monitor_standalone","监控独立进程", "Standalone monitor",
  OPT_GROUP_FEATURE, OPT_KIND_ON, 0, 0, "监控重负载独立进程运行", "Monitor runs in dedicated process" },
{ "feat.camera_backend",   "摄像头后端", "Camera backend",
  OPT_GROUP_FEATURE, OPT_KIND_OFF, 0, 1, "完整监控后端（含 ONVIF/多路）", "Full camera backend" },
{ "feat.intrusion_chain",  "防入侵联动链", "Intrusion chain",
  OPT_GROUP_FEATURE, OPT_KIND_OFF, 0, 1, "监控→识别→预警→通知 串联", "Camera→detect→alert→notify chain" },
{ "feat.ai_vision_overlay","AI 识别叠加画面", "AI vision overlay",
  OPT_GROUP_FEATURE, OPT_KIND_OFF, 0, 0, "在监控画面显示检测框/置信度", "Overlay detections on video" },
{ "feat.plugin_system",    "插件系统", "Plugin system",
  OPT_GROUP_FEATURE, OPT_KIND_ON, 0, 0, "加载/卸载 Python 插件", "Load/unload plugins" },
{ "feat.skill_market",     "技能市场", "Skill market",
  OPT_GROUP_FEATURE, OPT_KIND_ON, 0, 0, "内置技能 + 在线市场", "Builtin + online skills" },
{ "feat.sub_ai",           "子 AI 分发", "Sub-AI dispatch",
  OPT_GROUP_FEATURE, OPT_KIND_ON, 0, 0, "长任务委派给子 AI", "Delegate long tasks" },
{ "feat.ha_integration",   "Home Assistant 集成", "HA integration",
  OPT_GROUP_FEATURE, OPT_KIND_OFF, 0, 0, "HA 三通道联动（需配置地址）", "HA bridge (needs config)" },

/* ── 🔵 UI / 显示 ────────────────────────────────────── */
{ "ui.show_error_icon",  "开启错误图标显示", "Show error icon",
  OPT_GROUP_UI, OPT_KIND_OFF, 0, 0, "错误图标右侧显示错误码", "Show error codes next to icons" },
{ "ui.theme_dynamic",    "动态强调色", "Dynamic accent",
  OPT_GROUP_UI, OPT_KIND_ON, 0, 0, "强调色随状态变化", "Accent color follows state" },
{ "ui.start_screen",     "启动屏", "Boot screen",
  OPT_GROUP_UI, OPT_KIND_ON, 0, 0, "显示启动动画与自检", "Show boot animation" },
{ "ui.scale_auto",       "界面自适应缩放", "Auto scaling",
  OPT_GROUP_UI, OPT_KIND_ON, 0, 0, "网页/移动端自适应布局", "Responsive layout" },
{ "ui.msg_prefs",        "消息显示偏好", "Message prefs",
  OPT_GROUP_UI, OPT_KIND_ON, 0, 0, "思考/工具块折叠等", "Thinking/tool block preferences" },

/* ── 🔵 语音 ─────────────────────────────────────────── */
{ "voice.auto_read",     "自动朗读", "Auto read",
  OPT_GROUP_VOICE, OPT_KIND_OFF, 0, 0, "AI 回复自动语音播报", "Read replies aloud" },
{ "voice.continuous",    "连续对话", "Continuous chat",
  OPT_GROUP_VOICE, OPT_KIND_ON, 0, 0, "免唤醒连续语音交互", "Hands-free continuous dialog" },
{ "voice.tts_stream",    "TTS 流式", "TTS streaming",
  OPT_GROUP_VOICE, OPT_KIND_ON, 0, 0, "边合成边播放", "Stream TTS output" },
{ "voice.proxy",         "语音走服务端代理", "Voice via server",
  OPT_GROUP_VOICE, OPT_KIND_ON, 0, 0, "由主机提供 TTS/STT", "Server-side TTS/STT" },

/* ── 🔵 AI / 模型 ────────────────────────────────────── */
{ "ai.thinking",         "思考模式", "Thinking mode",
  OPT_GROUP_AI, OPT_KIND_ON, 0, 0, "显示模型思考链（DeepSeek 系）", "Show reasoning chain" },
{ "ai.streaming",        "流式输出", "Streaming",
  OPT_GROUP_AI, OPT_KIND_ON, 0, 0, "逐字流式显示回复", "Stream replies" },
{ "ai.auto_memory",      "自动记忆写入", "Auto memory write",
  OPT_GROUP_AI, OPT_KIND_OFF, 0, 0, "AI 自动保存重要记忆", "AI saves memories automatically" },
{ "ai.highrisk_auto",    "高风险自动授权", "High-risk auto-auth",
  OPT_GROUP_AI, OPT_KIND_OFF, 1, 0, "⚠️ 高风险技能免确认（危险）", "⚠️ Skip confirmation for high-risk" },

/* ── 🔵 数据 / 同步 ──────────────────────────────────── */
{ "data.offline_cache",  "离线缓存", "Offline cache",
  OPT_GROUP_DATA, OPT_KIND_ON, 0, 0, "本地加密缓存会话/记忆", "Encrypted local cache" },
{ "data.cross_device",   "跨设备修改会话", "Cross-device edit",
  OPT_GROUP_DATA, OPT_KIND_OFF, 0, 0, "允许其他设备修改本设备会话", "Allow other devices to edit" },
{ "data.auto_sync",      "自动同步", "Auto sync",
  OPT_GROUP_DATA, OPT_KIND_ON, 0, 0, "连接后自动同步数据", "Sync on connect" },
{ "data.log_to_file",    "日志保存到文件", "Log to file",
  OPT_GROUP_DATA, OPT_KIND_OFF, 0, 0, "日志持久化到磁盘", "Persist logs" },
{ "data.privacy_analytics","隐私分析上报", "Privacy analytics",
  OPT_GROUP_DATA, OPT_KIND_OFF, 0, 1, "匿名使用统计（默认关）", "Anonymous usage stats" },

/* ── 🔵 连接 ─────────────────────────────────────────── */
{ "conn.allow_plaintext","允许明文连接", "Allow plaintext",
  OPT_GROUP_CONN, OPT_KIND_OFF, 1, 0, "⚠️ 使用 ws:// 而非 wss://（危险）", "⚠️ Use ws:// instead of wss://" },

/* ── 🔵 开发调试（先生设定：0.x 默认开 / 正式版关——四级判定见 main.c） ── */
{ "dev.debug_log",  "开发调试日志", "Debug logging",
  OPT_GROUP_DEV, OPT_KIND_ON, 0, 0,
  "全量调试日志（含生命线）；0.x 默认开、正式版默认关；内部变量可覆盖", "Verbose debug logging (dev builds ON by default)" },
{ "dev.api_log",    "API 日志", "API logging",
  OPT_GROUP_DEV, OPT_KIND_ON, 0, 0,
  "记录 HTTP/WS/TCP/webhook/socket/UDP 请求（仅 server mode 可查看）", "Log API traffic (visible in server mode)" },
{ "dev.prevent_clear", "防清屏", "Prevent clear",
  OPT_GROUP_DEV, OPT_KIND_ON, 0, 0,
  "dev 模式下 ^L 改为保留缓冲重绘（不丢失日志）", "Keep buffer on clear (dev mode)" },

{ NULL, NULL, NULL, 0, 0, 0, 0, NULL, NULL }
};

#define OPT_COUNT ((int)(sizeof(g_options)/sizeof(g_options[0]) - 1))

/* ============================================================
 * 运行时状态
 * ============================================================ */
static int  g_values[128];      /* 与 g_options 同序 */
static int  g_privacy_mode = 0;
static int  g_loaded = 0;

static const char *OPTIONS_PATH = "/LINGOS/system/config/options.json";

const option_def_t *options_all(int *count) {
    if (count) *count = OPT_COUNT;
    return g_options;
}

const option_def_t *options_find(const char *key) {
    if (!key) return NULL;
    for (int i = 0; i < OPT_COUNT; i++) {
        if (strcmp(g_options[i].key, key) == 0) return &g_options[i];
    }
    return NULL;
}

static int opt_index(const char *key) {
    for (int i = 0; i < OPT_COUNT; i++)
        if (strcmp(g_options[i].key, key) == 0) return i;
    return -1;
}

static int opt_default(const option_def_t *d) {
    return (d->kind == OPT_KIND_ON) ? 1 : 0;   /* FIXED → 1（底线恒开） */
}

/* ============================================================
 * 加载 / 保存
 * ============================================================ */
int options_init(void) {
    for (int i = 0; i < OPT_COUNT; i++) g_values[i] = opt_default(&g_options[i]);

    FILE *f = fopen(OPTIONS_PATH, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 1024 * 256) {
            char *buf = malloc((size_t)sz + 1);
            if (buf) {
                size_t rd = fread(buf, 1, (size_t)sz, f);
                buf[rd] = '\0';
                cJSON *root = cJSON_Parse(buf);
                if (root) {
                    for (int i = 0; i < OPT_COUNT; i++) {
                        cJSON *v = cJSON_GetObjectItem(root, g_options[i].key);
                        if (cJSON_IsBool(v)) g_values[i] = cJSON_IsTrue(v) ? 1 : 0;
                    }
                    cJSON *pm = cJSON_GetObjectItem(root, "_privacy_mode");
                    if (cJSON_IsBool(pm)) g_privacy_mode = cJSON_IsTrue(pm) ? 1 : 0;
                    cJSON_Delete(root);
                }
                free(buf);
            }
        }
        fclose(f);
    }
    /* 底线强制为开（防被配置篡改） */
    for (int i = 0; i < OPT_COUNT; i++)
        if (g_options[i].kind == OPT_KIND_FIXED) g_values[i] = 1;

    g_loaded = 1;
    LOG_INFO_T("Options", "Init", "OK", "%d 项已加载 (privacy_mode=%d)", OPT_COUNT, g_privacy_mode);
    return 0;
}

static int options_save(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    for (int i = 0; i < OPT_COUNT; i++)
        cJSON_AddBoolToObject(root, g_options[i].key, g_values[i] ? 1 : 0);
    cJSON_AddBoolToObject(root, "_privacy_mode", g_privacy_mode ? 1 : 0);

    char *js = cJSON_Print(root);
    cJSON_Delete(root);
    if (!js) return -1;

    FILE *f = fopen(OPTIONS_PATH, "wb");
    if (!f) { free(js); return -1; }
    fwrite(js, 1, strlen(js), f);
    fclose(f);
    free(js);
    return 0;
}

/* ============================================================
 * 读写
 * ============================================================ */
int options_get(const char *key) {
    int i = opt_index(key);
    if (i < 0) return -1;
    if (!g_loaded) options_init();
    return g_values[i];
}

int options_set(const char *key, int value, int force) {
    int i = opt_index(key);
    if (i < 0) return -1;
    const option_def_t *d = &g_options[i];

    /* 🔒 底线不可改 */
    if (d->kind == OPT_KIND_FIXED) {
        LOG_WARN_T("Options", "Set", "Fixed", "安全底线项不可修改: %s", key);
        return -1;
    }
    /* ⚠️ 危险开关：关闭安全项需 force（UI 侧应弹二次确认） */
    if (d->dangerous && !force && value == 0) {
        LOG_WARN_T("Options", "Set", "NeedForce", "危险开关需显式确认: %s", key);
        return -2;
    }
    if (!g_loaded) options_init();
    g_values[i] = value ? 1 : 0;
    LOG_INFO_T("Options", "Set", "OK", "%s = %d", key, g_values[i]);
    return options_save();
}

/* ============================================================
 * 隐私保护模式
 * ============================================================ */
int options_apply_privacy_mode(void) {
    if (!g_loaded) options_init();

    static const char *kOn[] = {
        "sec.encrypt_transport", "sec.api_token_public", "sec.highrisk_confirm",
        "sec.rate_limit", "sec.cors_strict", "sec.ssrf_protect", "sec.egress_allowlist",
        "sec.update_signature", "sec.audit_log", "sec.encrypt_sensitive",
        "sec.encrypt_backup", "sec.encrypt_apikey",
        "priv.homomorphic", "priv.diff_privacy", "priv.federated",
        "priv.e2ee", "priv.anonymous", "priv.local_infer",
        NULL
    };
    static const char *kOff[] = {
        "sec.allow_lan_highrisk", "sec.allow_lan_no_ratelimit",
        "conn.allow_plaintext", "ai.highrisk_auto", "data.privacy_analytics",
        NULL
    };
    for (int i = 0; kOn[i]; i++) { int x = opt_index(kOn[i]); if (x >= 0) g_values[x] = 1; }
    for (int i = 0; kOff[i]; i++) { int x = opt_index(kOff[i]); if (x >= 0) g_values[x] = 0; }

    g_privacy_mode = 1;
    options_save();
    LOG_INFO_T("Options", "PrivacyMode", "ON", "隐私保护模式已启用（全部安全项最严）");
    return 0;
}

int options_clear_privacy_mode(void) {
    if (!g_loaded) options_init();
    for (int i = 0; i < OPT_COUNT; i++) g_values[i] = opt_default(&g_options[i]);
    for (int i = 0; i < OPT_COUNT; i++)
        if (g_options[i].kind == OPT_KIND_FIXED) g_values[i] = 1;
    g_privacy_mode = 0;
    options_save();
    LOG_INFO_T("Options", "PrivacyMode", "OFF", "已恢复默认");
    return 0;
}

int options_in_privacy_mode(void) {
    if (!g_loaded) options_init();
    return g_privacy_mode;
}

/* ============================================================
 * 导出 JSON（供 App/Web 渲染）
 * ============================================================ */
char *options_to_json(void) {
    if (!g_loaded) options_init();
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddBoolToObject(root, "privacy_mode", g_privacy_mode ? 1 : 0);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "options", arr);
    for (int i = 0; i < OPT_COUNT; i++) {
        const option_def_t *d = &g_options[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "key", d->key);
        cJSON_AddStringToObject(o, "name_zh", d->name_zh ? d->name_zh : "");
        cJSON_AddStringToObject(o, "name_en", d->name_en ? d->name_en : "");
        cJSON_AddNumberToObject(o, "group", (double)d->group);
        cJSON_AddNumberToObject(o, "kind", (double)d->kind);
        cJSON_AddBoolToObject(o, "dangerous", d->dangerous ? 1 : 0);
        cJSON_AddBoolToObject(o, "needs_condition", d->needs_condition ? 1 : 0);
        cJSON_AddBoolToObject(o, "value", g_values[i] ? 1 : 0);
        cJSON_AddStringToObject(o, "desc_zh", d->desc_zh ? d->desc_zh : "");
        cJSON_AddStringToObject(o, "desc_en", d->desc_en ? d->desc_en : "");
        cJSON_AddItemToArray(arr, o);
    }
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return js;
}
