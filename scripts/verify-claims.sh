#!/bin/bash
# LING OS 文档声明 vs 实际镜像/系统 审计（0.10）
# 用法: verify-claims.sh [ROOT]   ROOT 为 squashfs 解包根或 /（已安装系统）
ROOT="$1"
[ -n "$ROOT" ] || ROOT=/
ROOT=$(printf '%s' "$ROOT" | sed 's:/$::')
ok=0; partial=0; missing=0; mismatch=0
print_row() { printf '| %s | %s | %s |\n' "$1" "$2" "$3"; }
yes() { ok=$((ok+1)); print_row "$1" "✅ 已实现" "$2"; }
part() { partial=$((partial+1)); print_row "$1" "🟡 部分" "$2"; }
no() { missing=$((missing+1)); print_row "$1" "⬜ 未实现" "$2"; }
diff() { mismatch=$((mismatch+1)); print_row "$1" "⚠️ 文档过度声明" "$2"; }
has() { [ -e "$ROOT$1" ]; }
has_cmd() { [ -x "$ROOT$1" ]; }
grepfile() { [ -f "$ROOT$1" ] && grep -q "$2" "$ROOT$1"; }
any_bin() { for b in "$@"; do [ -x "$ROOT$b" ] && return 0; done; return 1; }
any_file() { for f in "$@"; do [ -e "$ROOT$f" ] && return 0; done; return 1; }

printf '## LING OS 0.10 声明 vs 实际审计\n\n'
printf '| 检查项 | 状态 | 说明 |\n|---|---|---|\n'

has /etc/lingos-image-info && yes "版本信息文件" "$(cat "$ROOT/etc/lingos-image-info" 2>/dev/null | tr '\n' ' ' | cut -c1-80)" || no "版本信息文件" "缺 /etc/lingos-image-info"

has_cmd /usr/local/sbin/lingos-install && yes "安装器脚本" "" || no "安装器脚本" ""
if grepfile /usr/local/sbin/lingos-install "cryptsetup luksFormat"; then yes "安装器 LUKS2 加密安装" ""; else diff "安装器 LUKS2 加密安装" "文档声明有，脚本未找到"; fi
if grepfile /usr/local/sbin/lingos-install "隐私等级"; then yes "安装器选择隐私等级" ""; else diff "安装器选择隐私等级" "文档声明有"; fi
if has_cmd /usr/local/sbin/lingos-desktop-launch; then yes "桌面启动兜底链" ""; else no "桌面启动兜底链" ""; fi
if grepfile /usr/local/sbin/lingos-desktop-launch "x11vnc"; then yes "VNC 远程兜底" ""; else no "VNC 远程兜底" ""; fi
if grepfile /usr/local/sbin/lingos-privacy-policy "dport 5900"; then yes "nftables 放行 5900" ""; else diff "nftables 放行 5900" "文档声明已修"; fi

for app in epiphany pcmanfm mousepad galculator xarchiver imv zathura mpv; do
  if any_bin "/usr/bin/$app" "/usr/local/bin/$app"; then yes "应用 $app" ""; else no "应用 $app" ""; fi
done
any_bin /usr/bin/scrot /usr/bin/grim && yes "截图工具" "" || no "截图工具" ""

if any_bin /usr/sbin/lightdm /usr/bin/lightdm /usr/sbin/gdm3 /usr/bin/gdm3 /usr/sbin/sddm /usr/bin/sddm /usr/bin/greetd; then yes "登录管理器（登录界面）" ""; else no "登录管理器（登录界面）" "用户反馈无登录界面"; fi
if any_file /usr/share/lingos/wallpapers /usr/share/backgrounds; then yes "壁纸资源" ""; else no "壁纸资源" "用户反馈无壁纸"; fi
if grepfile /etc/skel/.config/waybar/config.jsonc "wlr/taskbar"; then yes "任务栏窗口列表" ""; else no "任务栏窗口列表" "用户反馈最小化无法恢复"; fi
if grepfile /etc/skel/.config/waybar/config.jsonc '"position": "bottom"'; then yes "底部任务栏布局" ""; else no "底部任务栏布局" "waybar 目前为顶部"; fi
any_bin /usr/bin/wofi /usr/bin/fuzzel && yes "开始菜单启动器" "" || no "开始菜单启动器" ""
any_bin /usr/bin/swaybg /usr/bin/wbg && yes "Wayland 壁纸守护" "" || no "Wayland 壁纸守护" ""
any_bin /usr/bin/mako /usr/bin/dunst && yes "通知守护" "" || no "通知守护" ""
any_file /usr/share/icons/Adwaita/cursors /usr/share/icons/default/index.theme && yes "光标主题" "" || no "光标主题" "用户反馈无鼠标光标"
any_bin /usr/bin/swaylock /usr/bin/i3lock /usr/bin/light-locker && yes "锁屏工具" "" || no "锁屏工具" ""
if ls "$ROOT/etc/skel/Desktop/"*.desktop >/dev/null 2>&1; then yes "桌面图标" "$(ls "$ROOT/etc/skel/Desktop/"*.desktop 2>/dev/null | wc -l) 个"; else no "桌面图标" ""; fi
if [ -f "$ROOT/etc/skel/.config/waybar/config.jsonc" ]; then yes "waybar 面板" ""; else no "waybar 面板" ""; fi

for f in lingos-security-settings lingos-privacy-policy lingos-sandbox-run; do
  has_cmd "/usr/local/sbin/$f" && yes "安全组件 $f" "" || no "安全组件 $f" ""
done
for f in lingos-audit lingos-geoip lingos-ai-tool-gate lingos-plugin-gate lingos-version; do
  has_cmd "/usr/local/bin/$f" && yes "工具 $f" "" || no "工具 $f" ""
done
for u in lingos-privacy-apply.service lingos-audit-boot.service lingos-desktop-report.service; do
  has "/etc/systemd/system/$u" && yes "systemd $u" "" || no "systemd $u" ""
done
[ -L "$ROOT/etc/systemd/system/multi-user.target.wants/lingos-privacy-apply.service" ] && yes "隐私策略开机自启" "" || no "隐私策略开机自启" ""
for f in /etc/lingos/privacy-levels.json /etc/lingos/security-policy.json /etc/lingos/trust-policy.json /usr/share/lingos/geoip/city.csv /etc/firefox/policies/policies.json; do
  has "$f" && yes "配置 $f" "" || no "配置 $f" ""
done
any_bin /usr/bin/nft /usr/sbin/nft && yes "nftables" "" || no "nftables" ""
any_bin /usr/bin/bwrap && yes "bubblewrap" "" || no "bubblewrap" ""
any_bin /usr/sbin/cryptsetup /usr/bin/cryptsetup && yes "cryptsetup" "" || no "cryptsetup" ""
any_bin /usr/bin/firefox /usr/bin/epiphany && yes "浏览器" "" || no "浏览器" ""

diff "多 edition 构建（server/desktop/system/bin）" "版本规范声明，0.10 构建未产出"
diff "Setup 网络安装器" "文档声明，未实现"
diff "x86_64 ISO" "构建链损坏，发布被过滤（include_x86 开关）"
diff "A/B 更新 / dm-verity / 可验证启动" "0.11 计划，文档未标清"
diff "安全中心 UI / 隐私仪表盘" "仅有 CLI，无 UI"
diff "AI Agent Runtime 已打包" "agent_runtime.py 仅在仓库，未安装进 ISO"
diff "用户登录（图形登录界面）" "仅 tty1 自动登录，无登录器"
diff "桌面壁纸/任务栏/开始菜单/光标/通知" "文档未标注未实现"

printf '\n**统计：✅ %d / 🟡 %d / ⬜ %d / ⚠️ %d**\n' "$ok" "$partial" "$missing" "$mismatch"
