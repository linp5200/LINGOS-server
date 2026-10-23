# LING OS 桌面自动启动（仅 tty1；安装后的系统）
# 支持内核参数：
#   lingos.desktop=0      不启动桌面，留在文本 shell（调试）
#   lingos.session=x11    强制 X11 安全图形回退（Vectras/无 KMS 环境）
#   lingos.renderer=gl|pixman  Wayland 渲染器
_lingos_cmdline="$(cat /proc/cmdline 2>/dev/null || true)"
_lingos_desktop=1
_lingos_session=""
for _lingos_a in $_lingos_cmdline; do
    case "$_lingos_a" in
        lingos.desktop=0) _lingos_desktop=0 ;;
        lingos.session=*) _lingos_session="${_lingos_a#*=}" ;;
        lingos.renderer=*) export WLR_RENDERER="${_lingos_a#*=}" ;;
    esac
done
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ] && [ "$(tty 2>/dev/null || true)" = "/dev/tty1" ]; then
    mkdir -p "$HOME/.local/share"
    if [ "$_lingos_desktop" = "0" ] || [ "$_lingos_session" = "text" ]; then
        echo "LING OS: 文本模式（lingos.desktop=0），未启动桌面"
    elif [ "$_lingos_session" = "x11" ]; then
        if command -v startx >/dev/null 2>&1; then
            exec startx >>"$HOME/.local/share/x11-boot.log" 2>&1
        fi
        echo "LING OS: 未找到 startx，回退 shell" >&2
    elif command -v labwc >/dev/null 2>&1; then
        export XDG_SESSION_TYPE=wayland
        export MOZ_ENABLE_WAYLAND=1
        export WLR_RENDERER="${WLR_RENDERER:-pixman}"
        _lingos_t0=$(date +%s)
        labwc >>"$HOME/.local/share/labwc-boot.log" 2>&1
        _lingos_rc=$?
        _lingos_dt=$(( $(date +%s) - _lingos_t0 ))
        echo "labwc exited rc=$_lingos_rc after ${_lingos_dt}s" >>"$HOME/.local/share/labwc-boot.log"
        # 无 KMS（simple-framebuffer）时 labwc 会快速失败：自动回退 X11 安全图形
        if [ "$_lingos_dt" -lt 8 ] && command -v startx >/dev/null 2>&1; then
            echo "LING OS: labwc 启动失败（${_lingos_dt}s），回退 X11 安全图形" >&2
            exec startx >>"$HOME/.local/share/x11-boot.log" 2>&1
        fi
        exec bash -l
    elif command -v startx >/dev/null 2>&1; then
        echo "LING OS: labwc 缺失，回退 X11 安全图形"
        exec startx >>"$HOME/.local/share/x11-boot.log" 2>&1
    else
        echo "LING OS: 没有可用桌面（labwc/startx 均缺失），留在 shell" >&2
        echo "提示：重启时在 GRUB 里选“X11 安全图形回退”，或安装 xserver-xorg-core/xinit/openbox" >&2
    fi
fi