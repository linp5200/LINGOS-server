# LING OS 桌面自动启动（仅 tty1；适用于安装后的系统）
# 依赖：labwc + waybar + wofi + foot 已随 ISO 安装
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ "$(tty 2>/dev/null || true)" = "/dev/tty1" ]; then
    export XDG_SESSION_TYPE=wayland
    export MOZ_ENABLE_WAYLAND=1
    export WLR_RENDERER=${WLR_RENDERER:-pixman}
    exec labwc
fi
