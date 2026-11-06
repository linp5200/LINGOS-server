# LING OS 0.10.1 体验与启动整改计划

> 2026-10-31
> 目标：Windows 式桌面基本盘 + TCG 到桌面 ≤30 秒 + 文档声明回归真实。

## 1. 问题来源

0.10 正式版体验反馈：
- 没有登录界面（tty1 自动登录）
- 没有壁纸、没有任务栏/开始菜单，最小化窗口无法恢复
- 没有鼠标光标提示
- 桌面整体不像 Windows，用户体验不足
- 启动速度已明显提升，但还需要量化与继续压榨

## 2. 文档声明 vs 实际（审计摘要）

| 类别 | 状态 | 说明 |
|---|---|---|
| 版本号全链路 | ✅ | GRUB/安装器/waybar/image-info/lingos-version |
| 安装器 LUKS2/隐私等级/分区修复 | ✅ | 0.10 已交付 |
| 桌面应用包 | ✅ | 浏览器/文件管理器/编辑器/计算器/归档/图片/PDF/播放器/截图 |
| 兜底链 Wayland→X11→VNC→文本 | 🟡 | 能进桌面；缺少光标/壁纸/任务栏细节 |
| 隐私脚本/审计/GeoIP/AI 闸门/沙箱 | 🟡 | 脚本与配置在 ISO；无 UI，AI 未接实际调用链 |
| 登录界面/用户登录 | ⬜ | 0.10 无；0.10.1 补 LightDM |
| 壁纸/任务栏/开始菜单/光标/通知/锁屏 | ⬜ | 0.10 无；0.10.1 补 |
| Agent Runtime 打包 | 🟡 | 代码在仓库，未安装进 ISO |
| 多 edition / Setup 网络安装器 | ⬜ | 文档声明，尚未实现 |
| 双架构发布 | ⚠️ | x86 构建链损坏，发布仅 arm64 |
| 安全中心 UI / 隐私仪表盘 | ⬜ | 仅 CLI |
| A/B 更新 / dm-verity / 可验证启动 | ⬜ | 0.11 计划 |
| CI 冒烟门禁 | ⚠️ | 曾出现假绿；arm64 必须红，x86 单独标记 |

处理原则：文档增加状态标记（✅ 已交付 / 🟡 部分 / ⬜ 计划），CI 增加 verify-claims 审计。

## 3. 0.10.1 已落地清单

### 桌面（Windows 式基本盘）
- LightDM + lightdm-gtk-greeter：安装后图形登录（用户列表/密码/会话选择）
- 安装器不再自动登录 tty1；安装时设置 lingos 用户密码（手动询问，自动安装随机生成并记录）
- 壁纸：内置 1920×1080 LING OS 默认壁纸，swaybg（Wayland）/feh（X11）
- Waybar 底部任务栏：开始菜单（wofi）、wlr/taskbar 任务按钮、托盘、网络/音量/电池/隐私等级/时钟/电源
- X11 回退：openbox + tint2 底部任务栏（任务按钮/启动器/托盘/时钟）+ feh 壁纸 + dunst 通知
- 窗口：Super+M 最小化、Alt+Tab 切换、Super+Q 关闭、Super+F 全屏、点击任务栏恢复
- 开始菜单：wofi drun；X11 另可用 rofi
- 通知：mako（Wayland）/dunst（X11）
- 锁屏/电源菜单：swaylock/i3lock + lingos-power-menu（锁屏/注销/重启/关机）
- 鼠标光标：Adwaita 光标主题 + WLR_NO_HARDWARE_CURSORS=1 + XCURSOR_THEME/XCURSOR_SIZE，X11/VNC 下可见
- 登录会话文件：/usr/share/wayland-sessions/lingos-wayland.desktop、/usr/share/xsessions/lingos-x11.desktop

### 启动速度（TCG 目标 ≤30s）
- 新增 lingos-boot-report：开机记录 systemd-analyze、blame top20、critical-chain、失败单元，写 /var/log/lingos/boot-report.txt 与共享目录
- 安装后优化：initramfs MODULES=dep + zstd、GRUB_TIMEOUT=1、quiet loglevel=3
- 安装后 mask：avahi/cups/cups-browsed/whoopsie/apport/kerneloops/ModemManager/bluetooth/apt-daily/man-db/e2scrub/unattended-upgrades
- 登录器/面板/壁纸均选轻量组件；桌面自启只保留必要项

### 文档与验证
- 新增 scripts/verify-claims.sh：声明 vs 实际审计（可对已安装系统或解包 rootfs 运行）
- 本文件作为 0.10.1 整改说明；后续文档加状态标记

## 4. 验证方法

- 已安装系统：sudo bash scripts/verify-claims.sh /
- 或对 ISO 解包 rootfs：sudo bash scripts/verify-claims.sh /path/to/rootfs
- 启动耗时：cat /var/log/lingos/boot-report.txt

判定：
- TCG 上电→登录 ≤30s（若未达标，用 boot-report 定位 top 单元，继续优化）
- 登录→桌面可用 ≤5s
- 桌面可见：壁纸、底部任务栏、开始按钮、时钟、任务按钮；最小化后点击任务栏可恢复
- 光标可见（X11/Wayland/VNC）
- 注销回登录界面；锁屏/关机可用

## 5. 遗留（0.10.x / 0.11）

- x86_64 构建链修复；修复后 include_x86=true 恢复双架构发布
- 安全中心 UI / 隐私仪表盘、只读 API
- Agent Runtime 打包进 ISO 并接入 AI 调用链
- 多 edition 构建、Setup 网络安装器
- A/B 更新、dm-verity、可验证启动（0.11）
- CI 冒烟真门禁 + verify-claims 自动审计
