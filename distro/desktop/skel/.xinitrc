#!/bin/sh
# LING OS X11 安全图形会话（Vectras / 无 KMS 的 simple-framebuffer）
xset -dpms 2>/dev/null || true
xset s off 2>/dev/null || true
xset s noblank 2>/dev/null || true
xsetroot -cursor_name left_ptr 2>/dev/null || true
export XCURSOR_THEME=Adwaita
export XCURSOR_SIZE=24
export XDG_CURRENT_DESKTOP=LINGOS
xrdb -merge "$HOME/.Xresources" 2>/dev/null || true
exec openbox-session
