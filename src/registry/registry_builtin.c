/**
 * @file    src/registry/registry_builtin.c
 * @brief   内置系统模块统一注册（先生裁决：一切功能进 registry 统一 API）
 * @version LN-0.4.4
 *
 * 背景（2026-09-05 先生裁决）：
 *   全系统按「类目 = 功能词」划分，命令前缀 = 系统名，所有模块须进注册表，
 *   使 App / Web / Qt / 插件 可通过统一 API 发现与调用。
 *   此前 registry 底座完整，但**全系统只有 core/state.c 注册了 1 条** → 本文件补齐。
 *
 * 注册内容 = docs/ARCHITECTURE_系统类目.md 定义的类目与系统。
 */

#include "registry.h"
#include "log_extra.h"
#include "safe_string.h"
#include "../core/version.h"
#include <string.h>

typedef struct {
    const char *id;        /* 系统 id（命令前缀同名） */
    const char *name;      /* 显示名 */
    const char *desc;      /* 职责（元数据） */
    registry_type_t type;
    const char *path;      /* 关联文件（可空） */
} builtin_module_t;

/* 类目 → 系统（与 docs/ARCHITECTURE_系统类目.md 一致） */
static const builtin_module_t BUILTIN[] = {
    /* 核心 */
    {"core",     "核心系统",   "进程/事件总线/日志/命令路由/注册表", REG_TYPE_MODULE, "bin/lingos_linux"},
    /* 通讯 */
    {"comm",     "通讯系统",   "TCP/WS/HTTP 认证/连接/路由",        REG_TYPE_MODULE, "bin/lingos_linux"},
    {"notify",   "通知系统",   "通知分发（本地/推送/铃声）",         REG_TYPE_COMPONENT, NULL},
    {"voice",    "语音系统",   "TTS/STT（人机语音通讯）",           REG_TYPE_COMPONENT, "bin/voice_service.py"},
    /* 配置 */
    {"config",   "配置系统",   "配置读写/向导/校验/热更新/路径",      REG_TYPE_MODULE, NULL},
    /* 数据 */
    {"data",     "数据系统",   "存储/记忆/会话/知识/模型数据",        REG_TYPE_MODULE, NULL},
    /* 插件 */
    {"plugin",   "插件系统",   "插件装载/热重载/进程插件",           REG_TYPE_MODULE, NULL},
    /* 安全 */
    {"security", "安全系统",   "权限/沙箱/令牌/审计",               REG_TYPE_MODULE, NULL},
    {"home",     "家居系统",   "安防设备联动/HA",                  REG_TYPE_COMPONENT, "bin/ha_integration.py"},
    {"monitor",  "监控系统",   "采集/多路/预览/录像/拍照/IR/OSD/存储（给人看）",
                                                                    REG_TYPE_MODULE, "bin/monitor_service.py"},
    {"alert",    "预警系统",   "安全预警/分级/通知",                REG_TYPE_COMPONENT, "bin/lingos_alertd"},
    /* AI */
    {"ai",           "AI 系统",     "对话/推理编排",                REG_TYPE_MODULE, "bin/ai_server.py"},
    {"skill",        "技能系统",     "技能库/安装/执行",              REG_TYPE_COMPONENT, "bin/skill_loader.py"},
    {"ai_vision",    "AI 识别引擎",  "检测/标定/追踪（给 AI 用）",     REG_TYPE_MODULE, "bin/vision_ai.py"},
    {"ocr",          "识别引擎",     "文字识别（AI 内容）",           REG_TYPE_COMPONENT, "bin/ocr_service.py"},
    /* 天气 */
    {"weather",  "天气系统",   "天气数据/源切换",                    REG_TYPE_COMPONENT, NULL},
    /* 【2026-10-11 第 4 期·registry 全量】补齐实际存在但未登记的子系统
     * ——目标：App/Web/Qt/插件经统一 API 能发现全部系统（先生"一切功能进 registry"） */
    {"vision",   "视觉系统",   "视频流/存储/NVR/重流（非 AI 部分）",  REG_TYPE_MODULE,    "src/python/nvr_ext.py"},
    {"storage",  "存储系统",   "录像/快照/磁盘/生命周期管理",         REG_TYPE_COMPONENT, "src/python/nvr_ext.py"},
    {"rules",    "规则系统",   "规则引擎/条件动作/联动",             REG_TYPE_MODULE,    "bin/lingos_linux"},
    {"mqtt",     "物联系统",   "MQTT 上报/订阅/HA 同步",            REG_TYPE_MODULE,    "bin/lingosd"},
    {"update",   "更新系统",   "升级检查/下载/签名校验/回滚",         REG_TYPE_COMPONENT, "bin/lingos-upgrade"},
    {"install",  "安装系统",   "依赖/模型/venv/引导安装",            REG_TYPE_COMPONENT, "bin/lingos_linux"},
    {"repair",   "修复系统",   "自检/修复/策略执行",                 REG_TYPE_COMPONENT, "src/python/repair_engine.py"},
    {"memory",   "记忆系统",   "三级记忆/检索/压缩",                 REG_TYPE_COMPONENT, "src/python/memory_pipeline.py"},
    {"api",      "统一调用API", "命令注册表/rely 依赖控制/OpenAPI",   REG_TYPE_COMPONENT, "bin/lingosd"},
    {"ui",       "界面系统",   "App/Web/Qt 前端与主题",             REG_TYPE_COMPONENT, "webui/index.html"},
};

#define BUILTIN_N ((int)(sizeof(BUILTIN) / sizeof(BUILTIN[0])))

int registry_register_builtin_modules(void);
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
