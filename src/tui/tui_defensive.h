/**
 * @file    tui_defensive.h
 * @brief   TUI 防御宏定义
 * @version LN-B-4.3.0.0
 * @par     核心协议：防御性编程
 */

#ifndef TUI_DEFENSIVE_H
#define TUI_DEFENSIVE_H

#include <notcurses/notcurses.h>
#include "../common/error_report.h"
#include "../lib/log_extra.h"

/* ============================================================
 * 错误码
 * ============================================================ */

#define TUI_OK          0
#define TUI_ERR_NULL    -1
#define TUI_ERR_MEM     -2
#define TUI_ERR_PLANE   -3
#define TUI_ERR_STATE   -4
#define TUI_ERR_TIMEOUT -5

/* ============================================================
 * 检查宏
 * ============================================================ */

#define TUI_CHECK(expr, ret, msg) \
    do { if (!(expr)) { LOG_WARN_T("TUI", "Check", "Fail", msg); return ret; } } while(0)

#define TUI_CHECK_PLANE(p, ret) \
    TUI_CHECK(p, ret, "plane is NULL")

#define TUI_CHECK_RENDERER(r) \
    TUI_CHECK(r && r->nc && r->stdplane, TUI_ERR_STATE, "renderer invalid")

#define TUI_CHECK_PTR(p, ret) \
    do { if (!(p)) { LOG_ERROR_T("TUI", "CheckPtr", "NULL", #p " is NULL"); return ret; } } while(0)

/* ============================================================
 * 资源宏
 * ============================================================ */

#define TUI_SAFE_ALLOC(size, fallback) \
    ({ void *p = calloc(1, size); if (!p) { LOG_ERROR_T("TUI", "Alloc", "Fail", "malloc failed"); p = fallback; } p; })

#define TUI_SAFE_PLANE_CREATE(parent, opts, fallback) \
    ({ struct ncplane *p = ncplane_create(parent, opts); \
       if (!p) { LOG_ERROR_T("TUI", "Plane", "CreateFail", "ncplane_create failed"); p = fallback; } \
       if (p) tui_resource_register(p); p; })

/* ============================================================
 * 状态检查
 * ============================================================ */

#define TUI_IS_INITIALIZED() (g_tui_state.initialized && g_tui_state.nc)
#define TUI_IS_READY() (TUI_IS_INITIALIZED() && g_tui_state.stdplane)

#endif /* TUI_DEFENSIVE_H */