# LING OS 桌面自动启动（仅 tty1；适用于安装后的系统）
# 依赖：labwc + waybar + wofi + foot 已随 ISO 安装
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ "$(tty 2>/dev/null || true)" = "/dev/tty1" ]; then
    export XDG_SESSION_TYPE=wayland
    export MOZ_ENABLE_WAYLAND=1
    export WLR_RENDERER="${WLR_RENDERER:-pixman}"
    if command -v labwc >/dev/null 2>&1; then
        mkdir -p "$HOME/.local/share"
        exec labwc >>"$HOME/.local/share/labwc-boot.log" 2>&1
    else
        echo "LING OS: 未找到 labwc，回退到 shell（检查桌面包是否安装）" >&2
    fi
fi
