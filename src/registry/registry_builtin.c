/**
 * @file    src/registry/registry_builtin.c
 * @brief   内置系统模块统一注册（先生裁决：一切功能进 registry 统一 API）
 * @version LN-0.9.0
 *
 * 背景（2026-09-05 先生裁决）：
 *   全系统按「类目 = 功能词」划分，**命令前缀 = 系统名**，所有模块须进注册表，
 *   使 App / Web / Qt / 插件 可通过统一 API 发现与调用。
 *
 * 【2026-10-16 第 4 期收尾·类目一致性】本文件 = 类目↔系统↔命令前缀的**单一事实源**：
 *   · 表内 category/prefix 与 docs/ARCHITECTURE_系统类目.md（唯一权威）逐条对齐；
 *   · 注册时写入 metadata{category,command_prefix,responsibility} → 落 registry.json，
 *     App/Web/Qt/插件可经统一 API 发现（不只是编译期常量）；
 *   · registry_builtin_consistency_report() 做真实一致性校验（非恒真）：
 *       ① prefix == id（类目文档：命令前缀=系统名）
 *       ② id / prefix 无重复
 *       ③ 类目属于文档 8 类目 或 明确标注的「扩展」
 *       ④ 运行期实际注册条目 vs 本表（发现静默注册失败）
 *       ⑤ function_registry 已注册点分命令的前缀必须归属已登记系统（命令面一致性）
 *   · 不做物理目录搬迁（先生令：会破坏构建）——只落行为层一致性。
 */

#include "registry.h"
#include "function_registry.h"   /* 命令面一致性校验（点分命令前缀） */
#include "log_extra.h"
#include "safe_string.h"
#include "lang.h"
#include "../core/version.h"
#include "../lib/cJSON/cJSON.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    const char *id;        /* 系统名（命令前缀同名——类目文档 §三） */
    const char *prefix;    /* 命令前缀（文档显式声明值，用于一致性校验，勿与本字段脱钩） */
    const char *name;      /* 显示名 */
    const char *category;  /* 类目（文档 §一） */
    const char *desc;      /* 职责（元数据） */
    registry_type_t type;
    const char *path;      /* 关联文件（可空） */
} builtin_module_t;

/* 类目 → 系统（与 docs/ARCHITECTURE_系统类目.md 一致；扩展段见文件末尾注释） */
static const builtin_module_t BUILTIN[] = {
    /* ---- 核心 ---- */
    {"core",     "core",     "核心系统",   "核心", "进程/事件总线/日志/命令路由/注册表", REG_TYPE_MODULE, "bin/lingos_linux"},
    /* ---- 通讯 ---- */
    {"comm",     "comm",     "通讯系统",   "通讯", "TCP/WS/HTTP 认证/连接/路由",        REG_TYPE_MODULE, "bin/lingos_linux"},
    {"notify",   "notify",   "通知系统",   "通讯", "通知分发（本地/推送/铃声）",         REG_TYPE_COMPONENT, NULL},
    {"voice",    "voice",    "语音系统",   "通讯", "TTS/STT（人机语音通讯）",           REG_TYPE_COMPONENT, "bin/voice_service.py"},
    /* ---- 配置 ---- */
    {"config",   "config",   "配置系统",   "配置", "配置读写/向导/校验/热更新/路径",      REG_TYPE_MODULE, NULL},
    /* ---- 数据 ---- */
    {"data",     "data",     "数据系统",   "数据", "存储/记忆/会话/知识/模型数据",        REG_TYPE_MODULE, NULL},
    /* ---- 插件 ---- */
    {"plugin",   "plugin",   "插件系统",   "插件", "插件装载/热重载/进程插件",           REG_TYPE_MODULE, NULL},
    /* ---- 安全 ---- */
    {"security", "security", "安全系统",   "安全", "权限/沙箱/令牌/审计",               REG_TYPE_MODULE, NULL},
    {"home",     "home",     "家居系统",   "安全", "安防设备联动/HA",                  REG_TYPE_COMPONENT, "bin/ha_integration.py"},
    {"monitor",  "monitor",  "监控系统",   "安全", "采集/多路/预览/录像/拍照/IR/OSD/存储（给人看）",
                                                                    REG_TYPE_MODULE, "bin/monitor_service.py"},
    {"alert",    "alert",    "预警系统",   "安全", "安全预警/分级/通知（生命线）",        REG_TYPE_COMPONENT, "bin/lingos_alertd"},
    /* ---- AI ---- */
    {"ai",           "ai",           "AI 系统",     "AI", "对话/推理编排",                REG_TYPE_MODULE, "bin/ai_server.py"},
    {"skill",        "skill",        "技能系统",     "AI", "技能库/安装/执行",              REG_TYPE_COMPONENT, "bin/skill_loader.py"},
    {"ai_vision",    "ai_vision",    "AI 识别引擎",  "AI", "检测/标定/追踪（给 AI 用）",     REG_TYPE_MODULE, "bin/vision_ai.py"},
    {"ocr",          "ocr",          "识别引擎",     "AI", "文字识别（AI 内容）",           REG_TYPE_COMPONENT, "bin/ocr_service.py"},
    /* ---- 天气 ---- */
    {"weather",  "weather",  "天气系统",   "天气", "天气数据/源切换",                    REG_TYPE_COMPONENT, NULL},
    /* ---- 扩展（类目文档 16 系统之外；2026-10-11 registry 全量新增，
     *      类目名统一标注「扩展」——不擅自归入文档 8 类目，待先生补文档后再归类） ---- */
    {"vision",   "vision",   "视觉系统",   "扩展", "视频流/存储/NVR/重流（非 AI 部分）",  REG_TYPE_MODULE,    "src/python/nvr_ext.py"},
    {"storage",  "storage",  "存储系统",   "扩展", "录像/快照/磁盘/生命周期管理",         REG_TYPE_COMPONENT, "src/python/nvr_ext.py"},
    {"rules",    "rules",    "规则系统",   "扩展", "规则引擎/条件动作/联动",             REG_TYPE_MODULE,    "bin/lingos_linux"},
    {"mqtt",     "mqtt",     "物联系统",   "扩展", "MQTT 上报/订阅/HA 同步",            REG_TYPE_MODULE,    "bin/lingosd"},
    {"update",   "update",   "更新系统",   "扩展", "升级检查/下载/签名校验/回滚",         REG_TYPE_COMPONENT, "bin/lingos-upgrade"},
    {"install",  "install",  "安装系统",   "扩展", "依赖/模型/venv/引导安装",            REG_TYPE_COMPONENT, "bin/lingos_linux"},
    {"repair",   "repair",   "修复系统",   "扩展", "自检/修复/策略执行",                 REG_TYPE_COMPONENT, "src/python/repair_engine.py"},
    {"memory",   "memory",   "记忆系统",   "扩展", "三级记忆/检索/压缩",                 REG_TYPE_COMPONENT, "src/python/memory_pipeline.py"},
    {"api",      "api",      "统一调用API", "扩展", "命令注册表/rely 依赖控制/OpenAPI",   REG_TYPE_COMPONENT, "bin/lingosd"},
    {"ui",       "ui",       "界面系统",   "扩展", "App/Web/Qt 前端与主题",             REG_TYPE_COMPONENT, "webui/index.html"},
};

#define BUILTIN_N ((int)(sizeof(BUILTIN) / sizeof(BUILTIN[0])))

/* 文档定义的 8 个类目（ARCHITECTURE_系统类目.md §一） */
static const char *DOC_CATEGORIES[] = {"核心", "通讯", "配置", "数据", "插件", "安全", "AI", "天气"};
#define DOC_CATEGORY_N ((int)(sizeof(DOC_CATEGORIES) / sizeof(DOC_CATEGORIES[0])))

/* metadata 对象：registry_register 仅浅拷贝指针 → 必须进程生命周期内常驻。
 * （与 registry_skill.c 的 metadata 约定一致；registry_save 会复制落盘。） */
static cJSON *g_builtin_meta[BUILTIN_N];

int registry_register_builtin_modules(void) {
    int ok = 0;
    for (int i = 0; i < BUILTIN_N; i++) {
        registry_entry_t e;
        memset(&e, 0, sizeof(e));
        /* 命名：<类型>:<系统名>（与 state.c 的 component:<id> 风格一致） */
        const char *prefix = (BUILTIN[i].type == REG_TYPE_MODULE) ? "module:" : "component:";
        safe_snprintf(e.id, sizeof(e.id), "%s%s", prefix, BUILTIN[i].id);
        safe_strncpy(e.name, BUILTIN[i].name, sizeof(e.name));
        /* 【0.5.2 修复】版本动态化（原硬编码 LN-0.4.4——与系统版本不同步） */
        safe_strncpy(e.version, version_get(), sizeof(e.version));
        if (BUILTIN[i].path) safe_strncpy(e.path, BUILTIN[i].path, sizeof(e.path));
        e.type = BUILTIN[i].type;
        e.status = REG_STATUS_ACTIVE;

        /* 【2026-10-16】类目/命令前缀/职责 进 metadata（App/Web 可发现，非仅编译期常量） */
        if (!g_builtin_meta[i]) {
            g_builtin_meta[i] = cJSON_CreateObject();
            if (g_builtin_meta[i]) {
                cJSON_AddStringToObject(g_builtin_meta[i], "category", BUILTIN[i].category);
                cJSON_AddStringToObject(g_builtin_meta[i], "command_prefix", BUILTIN[i].prefix);
                cJSON_AddStringToObject(g_builtin_meta[i], "responsibility", BUILTIN[i].desc);
            }
        }
        e.metadata = (void*)g_builtin_meta[i];

        if (registry_register(&e) == 0) {
            ok++;
        } else {
            /* 先生 2026-09-12：日志保持逐项输出（正常诊断信息，不降噪） */
            LOG_WARN_T("Registry", "BuiltinReg", "Fail", "register %s failed", e.id);
        }
    }
    LOG_INFO_T("Registry", "BuiltinReg", "Done", "registered %d/%d builtin systems", ok, BUILTIN_N);
    return ok;
}

/* ============================================================
 * 类目/系统/命令前缀 查询 API（供 shell registry 命令与一致性校验使用）
 * ============================================================ */
int registry_builtin_system_count(void) {
    return BUILTIN_N;
}

int registry_builtin_system_at(int idx, const char **id, const char **name,
                               const char **category, const char **prefix, const char **desc) {
    if (idx < 0 || idx >= BUILTIN_N) return -1;
    if (id)       *id       = BUILTIN[idx].id;
    if (name)     *name     = BUILTIN[idx].name;
    if (category) *category = BUILTIN[idx].category;
    if (prefix)   *prefix   = BUILTIN[idx].prefix;
    if (desc)     *desc     = BUILTIN[idx].desc;
    return 0;
}

/* 命令前缀是否属于已登记系统（点分命令一致性校验用） */
int registry_builtin_prefix_known(const char *prefix) {
    if (!prefix || !*prefix) return 0;
    for (int i = 0; i < BUILTIN_N; i++) {
        if (strcmp(BUILTIN[i].prefix, prefix) == 0) return 1;
    }
    return 0;
}

const char* registry_builtin_category_of(const char *system_id) {
    if (!system_id || !*system_id) return NULL;
    for (int i = 0; i < BUILTIN_N; i++) {
        if (strcmp(BUILTIN[i].id, system_id) == 0) return BUILTIN[i].category;
    }
    return NULL;
}

static int category_is_doc(const char *category) {
    for (int i = 0; i < DOC_CATEGORY_N; i++) {
        if (strcmp(DOC_CATEGORIES[i], category) == 0) return 1;
    }
    return 0;
}

/* ============================================================
 * 一致性校验（真实检查；返回违规项数，报告写入 out）
 *   —— 不使用恒真判断：每条都为可失败的实测比对
 * ============================================================ */
int registry_builtin_consistency_report(char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    int violations = 0;
    size_t used = 0;
    out[0] = '\0';

#define REP_APPEND(...) do {         int _w = safe_snprintf(out + used, out_size - used, __VA_ARGS__);         if (_w < 0) { return violations; }         if ((size_t)_w >= out_size - used) { used = out_size - 1; } else { used += (size_t)_w; }     } while (0)

    REP_APPEND("%s\n", tr("=== Category/System/Command-prefix consistency ===",
                           "=== 类目/系统/命令前缀 一致性校验 ==="));
    REP_APPEND("%s: %d (%s %d + %s %d)\n",
               tr("Registered systems in table", "表内系统总数"), BUILTIN_N,
               tr("doc categories", "文档 16 系统"), 16,
               tr("extensions", "扩展"), BUILTIN_N - 16);

    /* ①② prefix==id、id/prefix 重复 */
    int mismatch = 0, dup = 0;
    for (int i = 0; i < BUILTIN_N; i++) {
        if (strcmp(BUILTIN[i].id, BUILTIN[i].prefix) != 0) {
            mismatch++;
            REP_APPEND("  [%s] %s: id='%s' prefix='%s'\n",
                       tr("FAIL", "失败"), tr("prefix != system name", "命令前缀≠系统名"),
                       BUILTIN[i].id, BUILTIN[i].prefix);
        }
        for (int j = i + 1; j < BUILTIN_N; j++) {
            if (strcmp(BUILTIN[i].id, BUILTIN[j].id) == 0 ||
                strcmp(BUILTIN[i].prefix, BUILTIN[j].prefix) == 0) {
                dup++;
                REP_APPEND("  [%s] %s: '%s' <-> '%s'\n",
                           tr("FAIL", "失败"), tr("duplicate id/prefix", "重复 id/前缀"),
                           BUILTIN[i].id, BUILTIN[j].id);
                break;
            }
        }
    }
    violations += mismatch + dup;
    REP_APPEND("%s prefix==id : %s (%d)\n", tr("Check", "检查"),
               mismatch ? tr("FAIL", "失败") : tr("PASS", "通过"), mismatch);
    REP_APPEND("%s unique     : %s (%d)\n", tr("Check", "检查"),
               dup ? tr("FAIL", "失败") : tr("PASS", "通过"), dup);

    /* ③ 类目合法性（文档 8 类目 或 明确标注「扩展」） */
    int badcat = 0;
    for (int i = 0; i < BUILTIN_N; i++) {
        if (!category_is_doc(BUILTIN[i].category) && strcmp(BUILTIN[i].category, "扩展") != 0) {
            badcat++;
            REP_APPEND("  [%s] %s: %s -> '%s'\n", tr("FAIL", "失败"),
                       tr("unknown category", "未知类目"), BUILTIN[i].id, BUILTIN[i].category);
        }
    }
    violations += badcat;
    REP_APPEND("%s category   : %s (%d)\n", tr("Check", "检查"),
               badcat ? tr("FAIL", "失败") : tr("PASS", "通过"), badcat);

    /* ④ 运行期注册完整性（检测静默注册失败） */
    int missing = 0;
    for (int i = 0; i < BUILTIN_N; i++) {
        char full[160];
        const char *p = (BUILTIN[i].type == REG_TYPE_MODULE) ? "module:" : "component:";
        safe_snprintf(full, sizeof(full), "%s%s", p, BUILTIN[i].id);
        if (!registry_get(full)) {
            missing++;
            REP_APPEND("  [%s] %s: %s\n", tr("FAIL", "失败"),
                       tr("not registered at runtime", "运行期未注册"), full);
        }
    }
    violations += missing;
    REP_APPEND("%s runtime reg: %s (%d %s)\n", tr("Check", "检查"),
               missing ? tr("FAIL", "失败") : tr("PASS", "通过"),
               missing, tr("missing", "项缺失"));

    /* ⑤ 命令面前缀归属：function_registry 已注册点分命令 */
    cJSON *cmds = api_describe_commands(NULL);
    int cmd_total = 0, cmd_bad = 0;
    if (cmds) {
        int n = cJSON_GetArraySize(cmds);
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(cmds, i);
            const cJSON *name = cJSON_GetObjectItem(it, "command");
            if (!cJSON_IsString(name)) continue;
            cmd_total++;
            const char *dot = strchr(name->valuestring, '.');
            char pfx[64];
            if (!dot) {
                cmd_bad++;
                REP_APPEND("  [%s] %s: '%s'\n", tr("FAIL", "失败"),
                           tr("command without <system>.<action> prefix", "命令缺少 系统.动作 前缀"),
                           name->valuestring);
                continue;
            }
            size_t plen = (size_t)(dot - name->valuestring);
            if (plen == 0 || plen >= sizeof(pfx)) { cmd_bad++; continue; }
            memcpy(pfx, name->valuestring, plen);
            pfx[plen] = '\0';
            if (!registry_builtin_prefix_known(pfx)) {
                cmd_bad++;
                REP_APPEND("  [%s] %s: '%s' (%s '%s')\n", tr("FAIL", "失败"),
                           tr("command prefix not a registered system", "命令前缀不属于已登记系统"),
                           name->valuestring, tr("prefix", "前缀"), pfx);
            }
        }
        cJSON_Delete(cmds);
    }
    violations += cmd_bad;
    REP_APPEND("%s cmd prefix : %s (%d/%d %s)\n", tr("Check", "检查"),
               cmd_bad ? tr("FAIL", "失败") : tr("PASS", "通过"),
               cmd_bad, cmd_total, tr("violations", "条违规"));

    REP_APPEND("%s: %d\n", tr("Violations total", "违规项合计"), violations);
#undef REP_APPEND
    return violations;
}
