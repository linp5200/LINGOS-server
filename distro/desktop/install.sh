#!/bin/sh
# LING OS 桌面安装（Debian/Ubuntu 24.04+；其他发行版按注释调整包名）
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
echo "==> 安装依赖（已装则跳过）"
PKGS="labwc waybar wofi wayvnc chromium fonts-noto-cjk"
if command -v apt-get >/dev/null 2>&1; then
  sudo apt-get update -qq || true
  sudo apt-get install -y $PKGS || echo "部分包安装失败，请手动检查：$PKGS"
else
  echo "非 apt 系统：请手动安装：$PKGS" >&2
fi
echo "==> 安装脚本与配置"
sudo install -Dm755 "$HERE/kiosk-start.sh" /usr/local/bin/lingos-kiosk
sudo install -Dm755 "$HERE/wayvnc-start.sh" /usr/local/bin/lingos-wayvnc
CONF="${XDG_CONFIG_HOME:-$HOME/.config}"
mkdir -p "$CONF/labwc" "$CONF/waybar" "$CONF/wofi" "$CONF/wayvnc"
cp "$HERE/labwc/rc.xml" "$CONF/labwc/rc.xml"
cp "$HERE/waybar/config.jsonc" "$CONF/waybar/config.jsonc"
cp "$HERE/waybar/style.css" "$CONF/waybar/style.css"
cp "$HERE/wofi/style.css" "$CONF/wofi/style.css"
cp "$HERE/wayvnc/config" "$CONF/wayvnc/config"
echo "==> 安装 systemd 服务（用户级）"
mkdir -p "$CONF/systemd/user"
cp "$HERE/systemd/lingos-kiosk.service" "$CONF/systemd/user/"
cp "$HERE/systemd/wayvnc.service" "$CONF/systemd/user/"
systemctl --user daemon-reload 2>/dev/null || true
systemctl --user enable --now lingos-kiosk.service 2>/dev/null || echo "请登录 Wayland 会话后执行：systemctl --user enable --now lingos-kiosk"
echo "==> 完成。重新登录 Wayland 会话（labwc）即可；VNC: systemctl --user enable --now wayvnc"
