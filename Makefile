# LING OS Linux Build System (LN-B-5.1.2.6-rc)
# 开发版：确保 lingos_linux lingosd lingos_supervisor 三核心编译
# 包含 test-render 目标用于渲染测试
# 其他目标保留，用户可手动编译
# 默认目标包含 install_python_script

# ================================================================
# 默认目标（三核心 + 安装 Python 脚本）
# ================================================================
TARGETS = lingos_linux lingosd lingos_supervisor

all: $(TARGETS) install_python_script

# ================================================================
# 编译器与标志
# ================================================================
CC = gcc
CFLAGS = -Wall -Wextra -O1 -g -std=gnu11 -D_GNU_SOURCE -D_LARGEFILE64_SOURCE
CFLAGS += -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
CFLAGS += -Wno-format-truncation -Wno-sign-compare

# 【0.2.1 全捆】rpath=$ORIGIN/../lib——主二进制从包内 lib/ 找动态库（便携解压即用）
LDFLAGS += -Wl,-rpath,'$$ORIGIN/../lib'

NOTCURSES_CFLAGS := $(shell pkg-config --cflags notcurses 2>/dev/null)
NOTCURSES_LIBS   := $(shell pkg-config --libs notcurses 2>/dev/null)
ifeq ($(NOTCURSES_CFLAGS),)
    $(warning "notcurses not found, falling back to CLI mode")
    NOTCURSES_CFLAGS =
    NOTCURSES_LIBS = -lnotcurses -lnotcurses-core
endif

CFLAGS += $(NOTCURSES_CFLAGS)

# ================================================================
# 链接库
# ================================================================
BASE_LDFLAGS = $(LDFLAGS) -lpthread -lm -lcurl -lseccomp -lsqlite3 -lmosquitto

# 主程序（含 TUI）
TUI_LDFLAGS = $(BASE_LDFLAGS) $(NOTCURSES_LIBS) -lmicrohttpd

# 守护进程/监督者（不含 TUI）
MINIMAL_LDFLAGS = $(BASE_LDFLAGS) -lmicrohttpd

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

VERSION = "LN-B-5.1.2.6-rc"
CFLAGS += -DLINGOS_VERSION="\"$(VERSION)\""

SRC_DIR = src
TEST_DIR = tests

INCLUDES = -I$(SRC_DIR) -I$(SRC_DIR)/common -I$(SRC_DIR)/drivers -I$(SRC_DIR)/net \
           -I$(SRC_DIR)/net/mqtt -I$(SRC_DIR)/ipc \
           -I$(SRC_DIR)/shell -I$(SRC_DIR)/shell/config \
           -I$(SRC_DIR)/security -I$(SRC_DIR)/security/crypto -I$(SRC_DIR)/security/verify \
           -I$(SRC_DIR)/security/sandbox \
           -I$(SRC_DIR)/fs -I$(SRC_DIR)/api -I$(SRC_DIR)/ai \
           -I$(SRC_DIR)/ai/memory -I$(SRC_DIR)/ai/routing -I$(SRC_DIR)/ai/reminder \
           -I$(SRC_DIR)/lib -I$(SRC_DIR)/lib/cJSON -I$(SRC_DIR)/lib/crypto \
           -I$(SRC_DIR)/update -I$(SRC_DIR)/test -I$(SRC_DIR)/debug \
           -I$(SRC_DIR)/scan -I$(SRC_DIR)/health -I$(SRC_DIR)/health/repair \
           -I$(SRC_DIR)/core -I$(SRC_DIR)/core/plugin -I$(SRC_DIR)/core/snapshot \
           -I$(SRC_DIR)/alert -I$(SRC_DIR)/firewall -I$(SRC_DIR)/daemon \
           -I$(SRC_DIR)/include -I$(SRC_DIR)/tui \
           -I$(SRC_DIR)/tui/desktop -I$(SRC_DIR)/tui/widgets \
           -I$(SRC_DIR)/vision -I$(SRC_DIR)/voice -I$(SRC_DIR)/rules \
           -I$(SRC_DIR)/registry -I$(SRC_DIR)/supervisor -I$(SRC_DIR)/gui \
           -I$(SRC_DIR)/ui \
           -I$(SRC_DIR)/config \
           -I$(SRC_DIR)/install \
           -I$(SRC_DIR)/wizard

# ================================================================
# 模块分组
# ================================================================

PLATFORM_SRCS = $(SRC_DIR)/drivers/linux_io.c \
                $(SRC_DIR)/drivers/linux_timer.c

NET_SRCS = $(SRC_DIR)/net/tcp_client.c \
           $(SRC_DIR)/net/mqtt/mqtt_client.c \
           $(SRC_DIR)/net/mqtt/mqtt_ha.c \
           $(SRC_DIR)/net/mqtt/mqtt_sync.c \
           $(SRC_DIR)/net/mqtt/mqtt_sync_data.c \
           $(SRC_DIR)/net/mqtt/mqtt_sync_enhanced.c

IPC_SRCS = $(SRC_DIR)/ipc/ipc_core.c

COMMON_SRCS = $(SRC_DIR)/common/data_path.c \
              $(SRC_DIR)/common/distro_detect.c \
              $(SRC_DIR)/common/error_codes.c \
              $(SRC_DIR)/common/error_report.c \
              $(SRC_DIR)/common/init_cache.c \
              $(SRC_DIR)/common/interactive.c \
              $(SRC_DIR)/common/lang.c \
              $(SRC_DIR)/common/mode.c \
              $(SRC_DIR)/common/safe_string.c \
              $(SRC_DIR)/common/string_no_sys.c \
              $(SRC_DIR)/common/network.c \
              $(SRC_DIR)/common/markdown_renderer.c \
              $(SRC_DIR)/ai/ai_config.c

LIB_SRCS = $(SRC_DIR)/lib/deb_parser.c \
           $(SRC_DIR)/lib/lapt_parser.c \
           $(SRC_DIR)/lib/libling.c \
           $(SRC_DIR)/lib/log_extra.c \
           $(SRC_DIR)/lib/path_utils.c \
           $(SRC_DIR)/lib/pkg_deps.c \
           $(SRC_DIR)/lib/cJSON/cJSON.c \
           $(SRC_DIR)/lib/crypto/monocypher.c

CRYPTO_SRCS = $(SRC_DIR)/security/crypto/crypto_core.c \
              $(SRC_DIR)/security/crypto/envelope.c

UI_SRCS = $(SRC_DIR)/ui/startup_ui.c \
          $(SRC_DIR)/ui/progress_bar.c

CORE_SRCS = $(SRC_DIR)/core/app_runner.c \
            $(SRC_DIR)/core/app_sandbox.c \
            $(SRC_DIR)/core/backup.c \
            $(SRC_DIR)/core/background_init.c \
            $(SRC_DIR)/core/component_version.c \
            $(SRC_DIR)/core/config_loader.c \
            $(SRC_DIR)/core/dependency_check.c \
            $(SRC_DIR)/core/env_bootstrap.c \
            $(SRC_DIR)/core/install.c \
            $(SRC_DIR)/core/startup_mode.c \
            $(SRC_DIR)/core/state.c \
            $(SRC_DIR)/core/version.c \
            $(SRC_DIR)/core/plugin/plugin.c \
            $(SRC_DIR)/core/plugin/plugin_loader.c \
            $(SRC_DIR)/core/snapshot/snapshot.c \
            $(SRC_DIR)/core/snapshot/snapshot_diff.c \
            $(SRC_DIR)/core/install_error.c \
            $(SRC_DIR)/core/exit_status.c \
            $(SRC_DIR)/core/repair_mode.c

REGISTRY_SRCS = $(SRC_DIR)/registry/registry.c \
                $(SRC_DIR)/registry/registry_feature.c \
                $(SRC_DIR)/registry/registry_plugin.c \
                $(SRC_DIR)/registry/registry_selfcheck.c \
                $(SRC_DIR)/registry/registry_skill.c

SECURITY_SRCS = $(SRC_DIR)/security/absolute_protect.c \
                $(SRC_DIR)/security/audit.c \
                $(SRC_DIR)/security/dark_mode.c \
                $(SRC_DIR)/security/defense.c \
                $(SRC_DIR)/security/defense_mode.c \
                $(SRC_DIR)/security/execution_gate.c \
                $(SRC_DIR)/security/input_filter.c \
                $(SRC_DIR)/security/output_filter.c \
                $(SRC_DIR)/security/perm_debug.c \
                $(SRC_DIR)/security/permission.c \
                $(SRC_DIR)/security/permission_check.c \
                $(SRC_DIR)/security/permission_whitelist.c \
                $(SRC_DIR)/security/privilege_manager.c \
                $(SRC_DIR)/security/secure_memory.c \
                $(SRC_DIR)/security/security_config.c \
                $(SRC_DIR)/security/shadow_mode.c \
                $(SRC_DIR)/security/token_verify.c \
                $(SRC_DIR)/security/virus_scanner.c \
                $(SRC_DIR)/security/sandbox/app_sandbox_cgroup.c \
                $(SRC_DIR)/security/sandbox/app_sandbox_seccomp.c \
                $(SRC_DIR)/security/verify/second_verify.c

FS_SRCS = $(SRC_DIR)/fs/env_detect.c \
          $(SRC_DIR)/fs/file_integrity.c \
          $(SRC_DIR)/fs/fs_layout.c \
          $(SRC_DIR)/fs/self_check.c \
          $(SRC_DIR)/fs/sync_template.c

API_SRCS = $(SRC_DIR)/api/api_core.c \
           $(SRC_DIR)/api/api_routes.c \
           $(SRC_DIR)/api/http_server.c \
           $(SRC_DIR)/api/websocket_server.c

UPDATE_SRCS = $(SRC_DIR)/update/apply_changes.c \
              $(SRC_DIR)/update/backup_restore.c \
              $(SRC_DIR)/update/manifest.c \
              $(SRC_DIR)/update/repo_client.c \
              $(SRC_DIR)/update/system_update.c \
              $(SRC_DIR)/update/update_auto_check.c \
              $(SRC_DIR)/update/update_dev_mode.c \
              $(SRC_DIR)/update/update_incremental.c \
              $(SRC_DIR)/update/update_rollback.c \
              $(SRC_DIR)/update/web_update.c

AI_SRCS = $(SRC_DIR)/ai/ai_master.c \
          $(SRC_DIR)/ai/nook.c \
          $(SRC_DIR)/ai/nook_idle.c \
          $(SRC_DIR)/ai/nook_personality.c \
          $(SRC_DIR)/ai/nook_repair.c \
          $(SRC_DIR)/ai/memory/memory_vector.c \
          $(SRC_DIR)/ai/reminder/ai_reminder.c \
          $(SRC_DIR)/ai/reminder/ai_reminder_scheduler.c \
          $(SRC_DIR)/ai/reminder/ai_reminder_store.c \
          $(SRC_DIR)/ai/routing/model_router.c \
          $(SRC_DIR)/ai/ai_privilege.c

HEALTH_SRCS = $(SRC_DIR)/health/behavior_monitor.c \
              $(SRC_DIR)/health/health_trend.c \
              $(SRC_DIR)/health/health_watchdog.c \
              $(SRC_DIR)/health/system_health.c \
              $(SRC_DIR)/health/repair/active_repair.c \
              $(SRC_DIR)/health/check_manager.c \
              $(SRC_DIR)/health/check_items.c \
              $(SRC_DIR)/health/check_cache.c

TEST_SRCS = $(SRC_DIR)/test/test_cases.c \
            $(SRC_DIR)/test/test_framework.c \
            $(SRC_DIR)/test/test_update.c

DEBUG_SRCS = $(SRC_DIR)/debug/crash_dump.c \
             $(SRC_DIR)/debug/crash_handler.c \
             $(SRC_DIR)/debug/error_logger.c \
             $(SRC_DIR)/debug/proot_detect.c

SCAN_SRCS = $(SRC_DIR)/scan/scan_analyzer.c \
            $(SRC_DIR)/scan/scan_config.c \
            $(SRC_DIR)/scan/scan_daemon.c

ALERT_CORE_SRCS = $(SRC_DIR)/alert/alert_config.c \
                  $(SRC_DIR)/alert/alert_history.c \
                  $(SRC_DIR)/alert/alert_manager.c \
                  $(SRC_DIR)/alert/alert_notify.c \
                  $(SRC_DIR)/alert/alert_sources.c \
                  $(SRC_DIR)/alert/alert_utils.c \
                  $(SRC_DIR)/alert/plugin_loader.c \
                  $(SRC_DIR)/alert/weather.c

ALERT_MAIN_SRCS = $(SRC_DIR)/alert/alertd.c

VISION_CORE_SRCS = $(SRC_DIR)/vision/camera_input.c \
                   $(SRC_DIR)/vision/detection_engine.c \
                   $(SRC_DIR)/vision/spatial_mapper.c \
                   $(SRC_DIR)/vision/tracker.c \
                   $(SRC_DIR)/vision/vision_memory.c \
                   $(SRC_DIR)/vision/vision_train.c \
                   $(SRC_DIR)/vision/vision_config.c

VISION_MAIN_SRCS = $(SRC_DIR)/vision/visiond.c

VOICE_CORE_SRCS = $(SRC_DIR)/voice/audio_input.c \
                  $(SRC_DIR)/voice/voice_cmds.c \
                  $(SRC_DIR)/voice/wakeword_engine.c \
                  $(SRC_DIR)/voice/voice_config.c

VOICE_MAIN_SRCS = $(SRC_DIR)/voice/voiced.c

DAEMON_CORE_SRCS = $(SRC_DIR)/daemon/app_daemon.c \
                   $(SRC_DIR)/daemon/connection_handler.c \
                   $(SRC_DIR)/daemon/discovery_server.c \
                   $(SRC_DIR)/daemon/syscall_handler.c

DAEMON_MAIN_SRCS = $(SRC_DIR)/daemon/lingosd.c

INSTALL_HELPERS_SRCS = $(SRC_DIR)/shell/install_helpers.c

# ================================================================
# Shell 源文件
# ================================================================
SHELL_SRCS = $(SRC_DIR)/shell/ai_config_cmd.c \
             $(SRC_DIR)/shell/skill_store.c \
             $(SRC_DIR)/shell/alert_cmds.c \
             $(SRC_DIR)/shell/alias.c \
             $(SRC_DIR)/shell/app_cmds.c \
             $(SRC_DIR)/shell/basic_cmds.c \
             $(SRC_DIR)/shell/behavior_cmd.c \
             $(SRC_DIR)/shell/chat_terminal.c \
             $(SRC_DIR)/shell/cmd_plugin.c \
             $(SRC_DIR)/shell/cmd_reminder.c \
             $(SRC_DIR)/shell/cmd_snapshot.c \
             $(SRC_DIR)/shell/commands.c \
             $(SRC_DIR)/shell/completion.c \
             $(SRC_DIR)/shell/debug_cmd.c \
             $(SRC_DIR)/shell/defense_cmd.c \
             $(SRC_DIR)/shell/error_shell.c \
             $(SRC_DIR)/shell/history.c \
             $(SRC_DIR)/shell/host_cmd.c \
             $(SRC_DIR)/shell/privilege_cmd.c \
             $(SRC_DIR)/shell/registry_cmd.c \
             $(SRC_DIR)/shell/repo_cmds.c \
             $(SRC_DIR)/shell/rollback_cmd.c \
             $(SRC_DIR)/shell/rules_cmds.c \
             $(SRC_DIR)/shell/security_cmd.c \
             $(SRC_DIR)/shell/shell.c \
             $(SRC_DIR)/shell/system_config.c \
             $(SRC_DIR)/shell/system_debug.c \
             $(SRC_DIR)/shell/system_info.c \
             $(SRC_DIR)/shell/syswatch.c \
             $(SRC_DIR)/shell/config/config_backup.c \
             $(SRC_DIR)/shell/config/config_cmd.c \
             $(SRC_DIR)/shell/config/config_migrate.c

# ================================================================
# 配置模块：核心 + 渲染器 + 调试
# ================================================================
CONFIG_CORE_SRCS = $(SRC_DIR)/config/config_core.c \
                   $(SRC_DIR)/config/wizard_engine.c \
                   $(SRC_DIR)/config/wizard_step_defs.c \
                   $(SRC_DIR)/config/config_validator.c \
                   $(SRC_DIR)/config/config_saver.c

CONFIG_RENDER_SRCS = $(SRC_DIR)/config/config_renderer.c \
                     $(SRC_DIR)/config/config_renderer_tui.c \
                     $(SRC_DIR)/config/config_renderer_cli.c \
                     $(SRC_DIR)/config/config_renderer_raw.c \
                     $(SRC_DIR)/config/config_debug.c

# ================================================================
# 安装模块
# ================================================================
INSTALL_SRCS = $(SRC_DIR)/install/install_manager.c \
               $(SRC_DIR)/install/install_system.c \
               $(SRC_DIR)/install/install_python.c \
               $(SRC_DIR)/install/install_model.c \
               $(SRC_DIR)/install/install_progress.c \
               $(SRC_DIR)/install/install_speed.c \
               $(SRC_DIR)/install/install_cache.c \
               $(SRC_DIR)/install/install_config.c

# ================================================================
# TUI 桌面
# ================================================================
TUI_SRCS = $(SRC_DIR)/tui/tui_controls.c \
           $(SRC_DIR)/tui/tui_logctl.c \
           $(SRC_DIR)/tui/tui_renderer.c \
           $(SRC_DIR)/tui/tui_resource.c \
           $(SRC_DIR)/tui/tui_signal.c \
           $(SRC_DIR)/tui/desktop/tui_app_launcher.c \
           $(SRC_DIR)/tui/desktop/tui_desktop.c \
           $(SRC_DIR)/tui/desktop/tui_desktop_events.c \
           $(SRC_DIR)/tui/desktop/tui_desktop_icons.c \
           $(SRC_DIR)/tui/desktop/tui_desktop_menu.c \
           $(SRC_DIR)/tui/desktop/tui_desktop_render.c \
           $(SRC_DIR)/tui/desktop/tui_desktop_taskbar.c \
           $(SRC_DIR)/tui/desktop/tui_desktop_window.c \
           $(SRC_DIR)/tui/widgets/widget_chat.c \
           $(SRC_DIR)/tui/widgets/widget_files.c \
           $(SRC_DIR)/tui/widgets/widget_monitor.c \
           $(SRC_DIR)/tui/widgets/widget_terminal.c

RULES_SRCS = $(SRC_DIR)/rules/rules_ai_guard.c \
             $(SRC_DIR)/rules/rules_engine.c \
             $(SRC_DIR)/rules/rules_executor.c \
             $(SRC_DIR)/rules/rules_parser.c \
             $(SRC_DIR)/rules/rules_storage.c

FIREWALL_SRCS = $(SRC_DIR)/firewall/firewall.c

SUPERVISOR_SRCS = $(SRC_DIR)/supervisor/supervisor.c

GUI_SRCS = $(SRC_DIR)/gui/lingos_gui.c

HTTP_SRCS = $(SRC_DIR)/api/http_server.c

MAIN_SRCS = $(SRC_DIR)/core/main.c

# ================================================================
# 组合依赖
# ================================================================

# 基础层（供所有二进制使用）
CORE_BASE = $(PLATFORM_SRCS) \
            $(IPC_SRCS) \
            $(COMMON_SRCS) \
            $(LIB_SRCS) \
            $(UI_SRCS) \
            $(SECURITY_SRCS) \
            $(CRYPTO_SRCS) \
            $(FS_SRCS) \
            $(CORE_SRCS) \
            $(REGISTRY_SRCS) \
            $(FIREWALL_SRCS) \
            $(INSTALL_HELPERS_SRCS) \
            $(CONFIG_CORE_SRCS) \
            $(INSTALL_SRCS) \
            $(HEALTH_SRCS) \
            $(SRC_DIR)/shell/error_shell.c

# 守护进程核心（添加网络、守护进程基础）
CORE_DAEMON = $(CORE_BASE) \
              $(NET_SRCS) \
              $(DAEMON_CORE_SRCS)

# 完整守护进程（添加 AI、API、更新、规则、预警、视觉、语音）
CORE_DAEMON_FULL = $(CORE_DAEMON) \
                   $(AI_SRCS) \
                   $(API_SRCS) \
                   $(UPDATE_SRCS) \
                   $(RULES_SRCS) \
                   $(ALERT_CORE_SRCS) \
                   $(VISION_CORE_SRCS) \
                   $(VOICE_CORE_SRCS)

# 完整核心（添加测试、调试、扫描）
CORE_FULL = $(CORE_DAEMON_FULL) \
            $(TEST_SRCS) \
            $(DEBUG_SRCS) \
            $(SCAN_SRCS)

# 界面层（主程序独有：Shell + TUI + 渲染器）
INTERFACE = $(SHELL_SRCS) $(TUI_SRCS) $(CONFIG_RENDER_SRCS)

# ================================================================
# 编译目标
# ================================================================

# 主程序：包含完整核心 + 界面
lingos_linux: $(CORE_FULL) $(INTERFACE) $(MAIN_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(TUI_LDFLAGS)

# 守护进程：不包含界面和渲染器
lingosd: $(CORE_DAEMON_FULL) $(DAEMON_MAIN_SRCS) $(HTTP_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(MINIMAL_LDFLAGS)

# 监督者：仅基础核心
lingos_supervisor: $(SUPERVISOR_SRCS) $(CORE_BASE)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(MINIMAL_LDFLAGS) -lpthread

# ================================================================
# 可选目标（用户手动编译）
# ================================================================
lingos_alertd: $(ALERT_MAIN_SRCS) $(ALERT_CORE_SRCS) $(CORE_DAEMON)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(MINIMAL_LDFLAGS) -lpthread -ldl -lm

lingos_visiond: $(VISION_MAIN_SRCS) $(VISION_CORE_SRCS) $(CORE_DAEMON)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(MINIMAL_LDFLAGS) -lpthread -lsqlite3 -lm

lingos_voiced: $(VOICE_MAIN_SRCS) $(VOICE_CORE_SRCS) $(CORE_DAEMON)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(MINIMAL_LDFLAGS) -lpthread -lm

lingos_gui: $(GUI_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $(GTK_CFLAGS) $^ -o $@ $(GTK_LIBS)

# ================================================================
# 测试目标
# ================================================================
.PHONY: test-render

test-render:
	@echo "Running renderer tests..."
	@if [ -f $(TEST_DIR)/test_tui_render.py ]; then \
	    python3 $(TEST_DIR)/test_tui_render.py; \
	else \
	    echo "⚠ Test script not found: $(TEST_DIR)/test_tui_render.py"; \
	    exit 1; \
	fi

# ================================================================
# 辅助目标
# ================================================================

install_python_script:
	@mkdir -p /LINGOS/bin
	@for f in ai_server.py sub_ai_scheduler.py repair_engine.py authorization_service.py skill_handlers.py syscall_client.py config_helpers.py embed_service.py registry_client.py skill_loader.py yolo_service.py diagnosis_engine.py memory_retrieval.py ha_archive.py agent_orchestrator.py web_search.py git_skills.py llm_unified.py voice_service.py; do \
	    if [ -f $(SRC_DIR)/python/$$f ]; then \
	        cp $(SRC_DIR)/python/$$f /LINGOS/bin/; \
	        chmod +x /LINGOS/bin/$$f; \
	        echo "Installed $$f"; \
	    else \
	        echo "Warning: $$f not found, skipping"; \
	    fi; \
	done

clean:
	rm -f $(TARGETS) lingos_gui lingos_alertd lingos_visiond lingos_voiced

run: lingos_linux
	./lingos_linux

.PHONY: all clean run install_python_script test-render