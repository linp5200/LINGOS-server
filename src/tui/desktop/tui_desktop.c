/**
 * @file    src/tui/desktop/tui_desktop.c
 * @brief   TUI 桌面主循环实现
 * @version LN-B-5.0.0.0
 * @changes 添加 NCKEY_MOUSE 条件定义；包含所有必需头文件
 */

#include "tui_desktop.h"
#include "tui_desktop_render.h"
#include "tui_desktop_window.h"
#include "tui_desktop_icons.h"
#include "tui_desktop_menu.h"
#include "tui_desktop_taskbar.h"
#include "tui_desktop_events.h"
#include "log_extra.h"
#include "safe_string.h"
#include "lang.h"
#include "uart.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#ifndef NCKEY_MOUSE
#define NCKEY_MOUSE 0x7000  /* 兼容旧版 notcurses */
#endif

static tui_desktop_state_t g_state;
static int g_initialized = 0;

static const tui_desktop_config_t default_config = {
    .enable_mouse = 1,
    .show_taskbar = 1,
    .show_statusbar = 1,
    .icon_size = 8,
    .icon_spacing = 2,
    .window_animation = 0,
    .background = "dark"
};

static void init_config(tui_desktop_config_t *config) {
    if (!config) return;
    memcpy(config, &default_config, sizeof(tui_desktop_config_t));
    const char *env = getenv("LINGOS_DESKTOP_CONFIG");
    if (env && strstr(env, "demo")) {
        config->window_animation = 1;
    }
}

static void desktop_signal_handler(int sig) {
    LOG_WARN_T("TUIDesktop", "Signal", "Received", "signal=%d", sig);
    if (g_initialized) {
        g_state.running = 0;
        g_state.exit_code = 128 + sig;
    }
}

int tui_desktop_run(void) {
    LOG_INFO_T("TUIDesktop", "Run", "Enter", "starting TUI desktop");
    if (g_initialized) {
        LOG_WARN_T("TUIDesktop", "Run", "Already", "desktop already running");
        return 0;
    }

    signal(SIGINT, desktop_signal_handler);
    signal(SIGTERM, desktop_signal_handler);
    signal(SIGWINCH, SIG_IGN);

    if (tui_desktop_init(&g_state) != 0) {
        LOG_ERROR_T("TUIDesktop", "Run", "InitFail", "desktop initialization failed");
        return -1;
    }

    g_initialized = 1;
    int ret = tui_desktop_loop(&g_state);
    tui_desktop_destroy(&g_state);
    g_initialized = 0;
    LOG_INFO_T("TUIDesktop", "Run", "Exit", "desktop exited with code %d", ret);
    return ret;
}

int tui_desktop_init(tui_desktop_state_t *state) {
    LOG_INFO_T("TUIDesktop", "Init", "Enter", "initializing desktop state");
    if (!state) return -1;

    memset(state, 0, sizeof(tui_desktop_state_t));
    init_config(&state->config);

    struct notcurses_options opts = {
        .flags = NCOPTION_NO_ALTERNATE_SCREEN,
        .loglevel = NCLOGLEVEL_FATAL,
    };

    state->nc = notcurses_init(&opts, NULL);
    if (!state->nc) {
        LOG_ERROR_T("TUIDesktop", "Init", "NotcursesFail", "notcurses_init failed");
        return -1;
    }

    state->stdplane = notcurses_stdplane(state->nc);
    if (!state->stdplane) {
        notcurses_stop(state->nc);
        return -1;
    }

    unsigned int height_u, width_u;
    ncplane_dim_yx(state->stdplane, &height_u, &width_u);
    state->height = (int)height_u;
    state->width = (int)width_u;

    if (state->height < 10 || state->width < 30) {
        LOG_WARN_T("TUIDesktop", "Init", "SmallTerm", "terminal too small (%dx%d)", state->width, state->height);
    }

    int status_height = state->config.show_statusbar ? 1 : 0;
    int taskbar_height = state->config.show_taskbar ? 1 : 0;
    int desktop_height = state->height - status_height - taskbar_height;

    struct ncplane_options nopts = {
        .y = 0,
        .x = 0,
        .rows = desktop_height,
        .cols = state->width,
        .userptr = NULL,
        .name = NULL,
        .resizecb = NULL,
        .flags = 0,
    };
    state->desktop_plane = ncplane_create(state->stdplane, &nopts);
    if (!state->desktop_plane) {
        LOG_ERROR_T("TUIDesktop", "Init", "DesktopPlaneFail", "ncplane_create failed");
        tui_desktop_destroy(state);
        return -1;
    }

    if (state->config.show_statusbar) {
        struct ncplane_options snopts = {
            .y = state->height - taskbar_height - 1,
            .x = 0,
            .rows = 1,
            .cols = state->width,
            .userptr = NULL,
            .name = NULL,
            .resizecb = NULL,
            .flags = 0,
        };
        state->status_plane = ncplane_create(state->stdplane, &snopts);
        if (!state->status_plane) {
            LOG_WARN_T("TUIDesktop", "Init", "StatusPlaneFail", "status plane creation failed");
        }
    }

    if (state->config.show_taskbar) {
        struct ncplane_options tnopts = {
            .y = state->height - 1,
            .x = 0,
            .rows = 1,
            .cols = state->width,
            .userptr = NULL,
            .name = NULL,
            .resizecb = NULL,
            .flags = 0,
        };
        state->taskbar_plane = ncplane_create(state->stdplane, &tnopts);
        if (!state->taskbar_plane) {
            LOG_WARN_T("TUIDesktop", "Init", "TaskbarPlaneFail", "taskbar plane creation failed");
        }
    }

    if (state->config.enable_mouse) {
        if (notcurses_mice_enable(state->nc, 0) == 0) {
            LOG_DEBUG_T("TUIDesktop", "Init", "Mouse", "mouse support enabled");
        } else {
            LOG_WARN_T("TUIDesktop", "Init", "MouseFail", "mouse support not available");
            state->config.enable_mouse = 0;
        }
    }

    state->running = 1;
    state->exit_code = 0;

    /* 初始化桌面组件 */
    tui_desktop_icons_init();
    tui_desktop_menu_init();
    tui_desktop_taskbar_init();
    tui_desktop_events_init();

    LOG_INFO_T("TUIDesktop", "Init", "OK", "desktop initialized (%dx%d)", state->width, state->height);
    return 0;
}

int tui_desktop_loop(tui_desktop_state_t *state) {
    LOG_INFO_T("TUIDesktop", "Loop", "Enter", "starting desktop main loop");
    if (!state || !state->running) return -1;

    int frame_count = 0;

    while (state->running) {
        int key = notcurses_get(state->nc, NULL, NULL);

        if (key == -1) {
            usleep(100000);
            continue;
        }

        if (key == 0) {
            frame_count++;
            if (frame_count % 10 == 0) {
                tui_desktop_refresh(state);
            }
            usleep(100000);
            continue;
        }

        LOG_DEBUG_T("TUIDesktop", "Loop", "Key", "key=0x%x (%d)", key, key);

        /* 处理鼠标事件（如果启用） */
        if (state->config.enable_mouse && key == NCKEY_MOUSE) {
            ncinput input;
            notcurses_get(state->nc, NULL, &input);
            tui_desktop_events_handle_mouse(state, key, &input);
            continue;
        }

        /* 处理键盘事件 */
        tui_desktop_events_handle_key(state, key);

        tui_desktop_refresh(state);
        usleep(50000);
    }

    LOG_INFO_T("TUIDesktop", "Loop", "Exit", "main loop exited");
    return state->exit_code;
}

void tui_desktop_destroy(tui_desktop_state_t *state) {
    LOG_INFO_T("TUIDesktop", "Destroy", "Enter", "destroying desktop");
    if (!state) return;

    tui_desktop_icons_cleanup();
    tui_desktop_menu_cleanup();
    tui_desktop_taskbar_cleanup();
    tui_desktop_events_cleanup();

    if (state->desktop_plane) { ncplane_destroy(state->desktop_plane); state->desktop_plane = NULL; }
    if (state->status_plane) { ncplane_destroy(state->status_plane); state->status_plane = NULL; }
    if (state->taskbar_plane) { ncplane_destroy(state->taskbar_plane); state->taskbar_plane = NULL; }
    if (state->menu_plane) { ncplane_destroy(state->menu_plane); state->menu_plane = NULL; }
    if (state->nc) { notcurses_stop(state->nc); state->nc = NULL; }

    state->running = 0;
    state->stdplane = NULL;
    LOG_INFO_T("TUIDesktop", "Destroy", "OK", "desktop destroyed");
}

void tui_desktop_refresh(tui_desktop_state_t *state) {
    if (!state || !state->nc) return;

    if (state->desktop_plane) {
        tui_desktop_render_background(state);
        tui_desktop_icons_render(state);
        tui_desktop_render_windows(state);
    }
    if (state->status_plane) {
        tui_desktop_render_statusbar(state);
    }
    if (state->taskbar_plane) {
        tui_desktop_render_taskbar(state);
    }
    notcurses_render(state->nc);
}

int tui_desktop_should_exit(tui_desktop_state_t *state) {
    return (state && !state->running) ? 1 : 0;
}

void tui_desktop_set_exit(tui_desktop_state_t *state, int exit_code) {
    if (!state) return;
    state->running = 0;
    state->exit_code = exit_code;
    LOG_INFO_T("TUIDesktop", "SetExit", "OK", "exit requested with code %d", exit_code);
}

tui_desktop_state_t* tui_desktop_get_state(void) {
    return &g_state;
}