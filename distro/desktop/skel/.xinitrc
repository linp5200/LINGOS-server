#!/bin/sh
# LING OS X11 安全图形回退会话（Vectras / 无 KMS 的 simple-framebuffer 环境）
xset -dpms 2>/dev/null || true
xset s off 2>/dev/null || true
xset s noblank 2>/dev/null || true
xrdb -merge "$HOME/.Xresources" 2>/dev/null || true
openbox-session >"$HOME/.local/share/openbox.log" 2>&1 &
sleep 1
command -v xterm >/dev/null 2>&1 && exec xterm -maximized -fa 'Noto Sans CJK SC' -fs 12
exec openbox-session
