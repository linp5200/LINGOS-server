#!/bin/sh
# LING OS VNC 启动（Wayland 会话下用 wayvnc；5900 端口，仅本机可先加 -l 127.0.0.1）
set -u
PORT="${LINGOS_VNC_PORT:-5900}"
LISTEN="${LINGOS_VNC_LISTEN:-127.0.0.1}"
exec wayvnc -l "$LISTEN" "$PORT"
