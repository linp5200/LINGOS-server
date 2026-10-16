/**
 * @file    src/monitor/ir_control.c
 * @brief   红外控制实现（真实 V4L2 控制面 + 回读校验 + 诚实降级）
 * @version LN-0.9.0
 * @par     核心协议：防御性编程 + 容错编程 + 诚实红线
 *
 * 实现要点（全部为真实 syscall，无模拟）：
 *   · 枚举：VIDIOC_QUERYCTRL + V4L2_CTRL_FLAG_NEXT_CTRL 遍历**全部**控制项
 *           （含驱动私有项——IR-cut 通常以 "IR Cut Filter"/"Day/Night" 命名暴露）
 *   · 设置：VIDIOC_S_CTRL + VIDIOC_G_CTRL 回读校验（设备拒绝 → 报告失败）
 *   · 菜单项：VIDIOC_QUERYMENU 逐项按名称匹配 Day/Night/Auto
 *   · 无 IR 控制项 → 明确失败（errno/名称诊断），绝不写"已切换"假状态
 */

#include "ir_control.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#define IR_MAX_CTRLS   64
#define IR_NAME_LEN    32

/* 控制项分类 */
#define IR_KIND_OTHER  0
#define IR_KIND_CUT    1   /* IR-cut / Day-Night 开关 */
#define IR_KIND_LED    2   /* 红外补光灯 */

typedef struct {
    __u32 id;
    __u32 type;
    __s32 minimum;
    __s32 maximum;
    __s32 default_value;
    char  name[IR_NAME_LEN];
    int   kind;
} ir_ctrl_t;

static int      g_fd = -1;
static ir_ctrl_t g_ctrls[IR_MAX_CTRLS];
static int      g_ctrl_count = 0;
static int      g_cut_idx  = -1;   /* 首选：IR-cut / Day-Night */
static int      g_led_idx  = -1;   /* 次选：IR-LED */
static int      g_last_mode = -1;
static char     g_status[256] = "IR: not initialized";

/* ============================================================
 * 名称工具
 * ============================================================ */

static void lower_copy(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    size_t i = 0;
    if (src) {
        for (; src[i] && i + 1 < dst_sz; i++) {
            dst[i] = (char)tolower((unsigned char)src[i]);
        }
    }
    dst[i] = '\0';
}

static int name_has(const char *hay, const char *needle) {
    return (hay && needle && strstr(hay, needle) != NULL);
}

/**
 * 依名称把控制项归类为 IR 相关。
 * 诚实说明：V4L2 无 "IR-cut" 标准 CID，实际驱动以名称暴露
 * （如 "IR Cut Filter" / "Day/Night Switch" / "Infrared LED"），故按名称匹配；
 * 匹配不到即视为无关控制项（不猜、不乱设）。
 */
static int classify_ctrl(const char *name_lower) {
    if (name_has(name_lower, "ir cut") || name_has(name_lower, "ir-cut") ||
        name_has(name_lower, "ircut")   || name_has(name_lower, "cut filter") ||
        name_has(name_lower, "day/night") || name_has(name_lower, "daynight") ||
        name_has(name_lower, "day & night") || name_has(name_lower, "day and night") ||
        name_has(name_lower, "night mode")) {
        return IR_KIND_CUT;
    }
    if (name_has(name_lower, "ir led") || name_has(name_lower, "irled") ||
        name_has(name_lower, "infrared") || name_has(name_lower, "ir light") ||
        name_has(name_lower, "ir lamp") || name_has(name_lower, "illuminator")) {
        return IR_KIND_LED;
    }
    return IR_KIND_OTHER;
}

static const char *mode_name(int mode) {
    switch (mode) {
        case IR_MODE_KEEP: return "keep";
        case IR_MODE_OFF:  return "off(day)";
        case IR_MODE_ON:   return "on(night)";
        case IR_MODE_AUTO: return "auto";
        default:           return "unknown";
    }
}

static const char *type_name(__u32 type) {
    switch (type) {
        case V4L2_CTRL_TYPE_INTEGER:      return "int";
        case V4L2_CTRL_TYPE_BOOLEAN:      return "bool";
        case V4L2_CTRL_TYPE_MENU:         return "menu";
        case V4L2_CTRL_TYPE_BUTTON:       return "button";
        case V4L2_CTRL_TYPE_INTEGER64:    return "int64";
        case V4L2_CTRL_TYPE_CTRL_CLASS:   return "class";
        case V4L2_CTRL_TYPE_STRING:       return "string";
        case V4L2_CTRL_TYPE_BITMASK:      return "bitmask";
        case V4L2_CTRL_TYPE_INTEGER_MENU: return "intmenu";
        default:                          return "?";
    }
}

/* ============================================================
 * 枚举（VIDIOC_QUERYCTRL + NEXT_CTRL，覆盖私有控制项）
 * ============================================================ */

int ir_control_init(int v4l2_fd) {
    g_fd = v4l2_fd;
    g_ctrl_count = 0;
    g_cut_idx = -1;
    g_led_idx = -1;
    g_last_mode = -1;

    if (g_fd < 0) {
        safe_snprintf(g_status, sizeof(g_status),
                      "IR: no V4L2 fd (RTSP/stream mode or camera not open) — control unavailable");
        LOG_INFO_T("IrControl", "Init", "NoFd", "%s", g_status);
        return -1;
    }

    struct v4l2_queryctrl qc;
    memset(&qc, 0, sizeof(qc));
    qc.id = V4L2_CTRL_FLAG_NEXT_CTRL;

    while (g_ctrl_count < IR_MAX_CTRLS) {
        memset(&qc.name, 0, sizeof(qc.name));
        if (ioctl(g_fd, VIDIOC_QUERYCTRL, &qc) != 0) {
            break;   /* EINVAL = 枚举结束；其它错误同样停止（诚实：不猜） */
        }
        if (!(qc.flags & V4L2_CTRL_FLAG_DISABLED)) {
            ir_ctrl_t *c = &g_ctrls[g_ctrl_count];
            memset(c, 0, sizeof(*c));
            c->id = qc.id;
            c->type = qc.type;
            c->minimum = qc.minimum;
            c->maximum = qc.maximum;
            c->default_value = qc.default_value;
            safe_strncpy(c->name, (const char *)qc.name, sizeof(c->name));

            char lower[IR_NAME_LEN];
            lower_copy(lower, sizeof(lower), c->name);
            c->kind = classify_ctrl(lower);

            if (c->kind == IR_KIND_CUT && g_cut_idx < 0) g_cut_idx = g_ctrl_count;
            if (c->kind == IR_KIND_LED && g_led_idx < 0) g_led_idx = g_ctrl_count;
            g_ctrl_count++;
        }
        qc.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }

    if (g_ctrl_count == 0) {
        safe_snprintf(g_status, sizeof(g_status),
                      "IR: VIDIOC_QUERYCTRL enumerated 0 controls (errno=%d %s) — no IR control",
                      errno, strerror(errno));
        LOG_WARN_T("IrControl", "Init", "NoControls", "%s", g_status);
        return -1;
    }

    if (g_cut_idx < 0 && g_led_idx < 0) {
        safe_snprintf(g_status, sizeof(g_status),
                      "IR: %d V4L2 controls enumerated, none IR-related "
                      "(driver exposes no IR-cut/IR-LED control) — honest: no IR switching",
                      g_ctrl_count);
        LOG_INFO_T("IrControl", "Init", "NoIrCtrl", "%s", g_status);
        return -1;
    }

    int idx = (g_cut_idx >= 0) ? g_cut_idx : g_led_idx;
    safe_snprintf(g_status, sizeof(g_status),
                  "IR: control '%s' (id=0x%08x type=%s kind=%s) available; %d controls enumerated",
                  g_ctrls[idx].name, (unsigned)g_ctrls[idx].id, type_name(g_ctrls[idx].type),
                  g_ctrls[idx].kind == IR_KIND_CUT ? "ir-cut/day-night" : "ir-led",
                  g_ctrl_count);
    LOG_INFO_T("IrControl", "Init", "OK", "%s", g_status);
    return 0;
}

int ir_control_available(void) {
    return (g_cut_idx >= 0 || g_led_idx >= 0) ? 1 : 0;
}

const char *ir_control_status(void) {
    return g_status;
}

void ir_control_cleanup(void) {
    g_fd = -1;
    g_ctrl_count = 0;
    g_cut_idx = -1;
    g_led_idx = -1;
    g_last_mode = -1;
    safe_strncpy(g_status, "IR: cleaned up", sizeof(g_status));
}

/* ============================================================
 * 菜单项名称匹配（VIDIOC_QUERYMENU）
 * ============================================================ */

static int menu_item_matches(const char *item_lower, int mode) {
    switch (mode) {
        case IR_MODE_OFF:
            return name_has(item_lower, "off") || name_has(item_lower, "day") ||
                   name_has(item_lower, "disable") || name_has(item_lower, "color") ||
                   name_has(item_lower, "colour");
        case IR_MODE_ON:
            return name_has(item_lower, "night") || name_has(item_lower, "on") ||
                   name_has(item_lower, "enable") || name_has(item_lower, "mono") ||
                   name_has(item_lower, "black");
        case IR_MODE_AUTO:
            return name_has(item_lower, "auto");
        default:
            return 0;
    }
}

/**
 * 菜单型控制项取值：按菜单项名称匹配；匹配不到时**仅当恰好 2 项**才按位置
 * 兜底（min=OFF / max=ON）并打 WARN —— 位置兜底是启发式，会如实记录日志。
 * @return 0 找到取值；-1 无法确定（诚实失败）
 */
static int pick_menu_value(const ir_ctrl_t *c, int mode, __s32 *out_val) {
    if (!c || !out_val) return -1;
    if (c->maximum < c->minimum) return -1;

    for (int i = (int)c->minimum; i <= (int)c->maximum; i++) {
        struct v4l2_querymenu qm;
        memset(&qm, 0, sizeof(qm));
        qm.id = c->id;
        qm.index = (__u32)i;
        if (ioctl(g_fd, VIDIOC_QUERYMENU, &qm) != 0) continue;

        char lower[IR_NAME_LEN];
        lower_copy(lower, sizeof(lower), (const char *)qm.name);
        if (lower[0] && menu_item_matches(lower, mode)) {
            *out_val = (__s32)i;
            return 0;
        }
    }

    if (mode != IR_MODE_AUTO && (int)(c->maximum - c->minimum) == 1) {
        *out_val = (mode == IR_MODE_ON) ? c->maximum : c->minimum;
        LOG_WARN_T("IrControl", "Menu", "PositionalFallback",
                   "control '%s': no day/night keyword in menu names — falling back to position "
                   "(index %d for %s); driver names may be non-standard",
                   c->name, (int)*out_val, mode_name(mode));
        return 0;
    }
    return -1;
}

/* 菜单当前值 → 模式（回读反查） */
static int menu_value_to_mode(const ir_ctrl_t *c, __s32 val) {
    if (!c) return -1;
    struct v4l2_querymenu qm;
    memset(&qm, 0, sizeof(qm));
    qm.id = c->id;
    qm.index = (__u32)val;
    if (ioctl(g_fd, VIDIOC_QUERYMENU, &qm) == 0) {
        char lower[IR_NAME_LEN];
        lower_copy(lower, sizeof(lower), (const char *)qm.name);
        if (menu_item_matches(lower, IR_MODE_ON)) return IR_MODE_ON;
        if (menu_item_matches(lower, IR_MODE_OFF)) return IR_MODE_OFF;
        if (menu_item_matches(lower, IR_MODE_AUTO)) return IR_MODE_AUTO;
    }
    if ((int)(c->maximum - c->minimum) == 1) {
        return (val == c->minimum) ? IR_MODE_OFF : IR_MODE_ON;
    }
    return -1;
}

/* ============================================================
 * 设置模式
 * ============================================================ */

static int apply_ctrl_value(const ir_ctrl_t *c, int mode, __s32 *out_val) {
    if (!c || !out_val) return -1;

    switch (c->type) {
        case V4L2_CTRL_TYPE_BOOLEAN:
            if (mode == IR_MODE_AUTO) return -1;   /* 布尔项无自动档（诚实） */
            *out_val = (mode == IR_MODE_ON) ? 1 : 0;
            return 0;

        case V4L2_CTRL_TYPE_MENU:
            return pick_menu_value(c, mode, out_val);

        case V4L2_CTRL_TYPE_INTEGER:
        case V4L2_CTRL_TYPE_INTEGER_MENU: {
            if (mode == IR_MODE_AUTO) return -1;   /* 整数项无自动档（诚实） */
            *out_val = (mode == IR_MODE_ON) ? c->maximum : c->minimum;
            return 0;
        }

        default:
            return -1;   /* button/bitmask/string… 不是开关语义，不乱设 */
    }
}

static int read_back_mode(const ir_ctrl_t *c) {
    if (!c) return -1;
    struct v4l2_control ctl;
    memset(&ctl, 0, sizeof(ctl));
    ctl.id = c->id;
    if (ioctl(g_fd, VIDIOC_G_CTRL, &ctl) != 0) return -1;

    switch (c->type) {
        case V4L2_CTRL_TYPE_BOOLEAN:
            return ctl.value ? IR_MODE_ON : IR_MODE_OFF;
        case V4L2_CTRL_TYPE_MENU:
            return menu_value_to_mode(c, ctl.value);
        case V4L2_CTRL_TYPE_INTEGER:
        case V4L2_CTRL_TYPE_INTEGER_MENU:
            if (ctl.value == c->minimum) return IR_MODE_OFF;
            if (ctl.value == c->maximum) return IR_MODE_ON;
            return -1;
        default:
            return -1;
    }
}

int ir_control_set_mode(int mode) {
    if (mode == IR_MODE_KEEP) {
        safe_snprintf(g_status, sizeof(g_status),
                      "IR: mode=keep — hardware untouched (honest no-op)");
        LOG_INFO_T("IrControl", "SetMode", "Keep", "%s", g_status);
        return 0;
    }

    if (g_fd < 0) {
        safe_snprintf(g_status, sizeof(g_status),
                      "IR: cannot set %s — no V4L2 fd (device not open / stream mode)", mode_name(mode));
        LOG_WARN_T("IrControl", "SetMode", "NoFd", "%s", g_status);
        return -1;
    }

    if (mode != IR_MODE_OFF && mode != IR_MODE_ON && mode != IR_MODE_AUTO) {
        safe_snprintf(g_status, sizeof(g_status), "IR: invalid mode %d", mode);
        LOG_WARN_T("IrControl", "SetMode", "BadMode", "%s", g_status);
        return -1;
    }

    int idx = (g_cut_idx >= 0) ? g_cut_idx : g_led_idx;
    if (idx < 0) {
        safe_snprintf(g_status, sizeof(g_status),
                      "IR: cannot set %s — driver exposed no IR control "
                      "(see ir_control_list_controls; honest failure, nothing changed)",
                      mode_name(mode));
        LOG_WARN_T("IrControl", "SetMode", "NoIrCtrl", "%s", g_status);
        return -1;
    }

    const ir_ctrl_t *c = &g_ctrls[idx];
    __s32 val = 0;
    if (apply_ctrl_value(c, mode, &val) != 0) {
        safe_snprintf(g_status, sizeof(g_status),
                      "IR: control '%s' (type=%s) cannot express mode %s (no such menu档) — unchanged",
                      c->name, type_name(c->type), mode_name(mode));
        LOG_WARN_T("IrControl", "SetMode", "Unsupported", "%s", g_status);
        return -1;
    }

    struct v4l2_control ctl;
    memset(&ctl, 0, sizeof(ctl));
    ctl.id = c->id;
    ctl.value = val;
    if (ioctl(g_fd, VIDIOC_S_CTRL, &ctl) != 0) {
        safe_snprintf(g_status, sizeof(g_status),
                      "IR: VIDIOC_S_CTRL '%s'=%d failed: errno=%d %s",
                      c->name, (int)val, errno, strerror(errno));
        LOG_WARN_T("IrControl", "SetMode", "IoctlFail", "%s", g_status);
        return -1;
    }

    int rb = read_back_mode(c);
    if (rb < 0 || rb != mode) {
        safe_snprintf(g_status, sizeof(g_status),
                      "IR: set '%s'=%d but read-back=%s (driver kept different value) — reported as failure",
                      c->name, (int)val, rb < 0 ? "unknown" : mode_name(rb));
        LOG_WARN_T("IrControl", "SetMode", "ReadBackMismatch", "%s", g_status);
        g_last_mode = rb;
        return -1;
    }

    g_last_mode = rb;
    safe_snprintf(g_status, sizeof(g_status),
                  "IR: %s applied via control '%s' (value=%d, read-back verified)",
                  mode_name(mode), c->name, (int)val);
    LOG_INFO_T("IrControl", "SetMode", "OK", "%s", g_status);
    return 0;
}

int ir_control_get_mode(void) {
    int idx = (g_cut_idx >= 0) ? g_cut_idx : g_led_idx;
    if (g_fd >= 0 && idx >= 0) {
        int m = read_back_mode(&g_ctrls[idx]);
        if (m >= 0) {
            g_last_mode = m;
            return m;
        }
    }
    return g_last_mode;
}

int ir_control_list_controls(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return -1;
    out[0] = '\0';
    int n = 0;
    for (int i = 0; i < g_ctrl_count; i++) {
        char line[160];
        const char *kind = g_ctrls[i].kind == IR_KIND_CUT ? "ir-cut/day-night"
                         : (g_ctrls[i].kind == IR_KIND_LED ? "ir-led" : "other");
        int w = safe_snprintf(line, sizeof(line), "0x%08x type=%-8s min=%d max=%d kind=%s name=%s\n",
                              (unsigned)g_ctrls[i].id, type_name(g_ctrls[i].type),
                              (int)g_ctrls[i].minimum, (int)g_ctrls[i].maximum, kind,
                              g_ctrls[i].name);
        if (w <= 0 || (size_t)n + (size_t)w + 1 > out_sz) return -1;
        memcpy(out + n, line, (size_t)w);
        n += w;
        out[n] = '\0';
    }
    return n;
}
