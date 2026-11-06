# LING OS 桌面自动启动（仅 tty1）
# 所有图形回退逻辑在 /usr/local/sbin/lingos-desktop-launch：
# Wayland -> X11(fbdev) -> Xvfb+VNC（局域网提示） -> 文本控制台
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ] && [ "$(tty 2>/dev/null || true)" = "/dev/tty1" ]; then
    mkdir -p "$HOME/.local/share" 2>/dev/null || true
    if [ -x /usr/local/sbin/lingos-desktop-launch ]; then
        exec /usr/local/sbin/lingos-desktop-launch
    fi
    if command -v labwc >/dev/null 2>&1; then
        export XDG_SESSION_TYPE=wayland
        export MOZ_ENABLE_WAYLAND=1
        export WLR_RENDERER="${WLR_RENDERER:-pixman}"
        exec labwc >>"$HOME/.local/share/labwc-boot.log" 2>&1
    fi
    echo "LING OS: 未找到桌面启动器，留在文本 shell（按 Ctrl+Alt+F2 可切换）"
fi
