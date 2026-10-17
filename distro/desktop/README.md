# LING OS 桌面（Wayland：labwc + waybar + wofi + WebUI kiosk）

> 目标：与常规 Linux 桌面同构，LINGOS 只提供 AI 层、首屏与系统设置，不再自研窗口管理器。
> TUI 桌面（src/tui/）将退役，仅保留无显示环境兜底。

## 组成

| 组件 | 作用 |
|---|---|
| labwc | 轻量 Wayland 合成器（窗口管理/快捷键） |
| waybar | 顶栏：时间/网络/音量/AI 状态 |
| wofi | 应用启动器（Super 键） |
| WebUI kiosk | Chromium/WebKit 全屏运行 WebUI，作为默认"桌面壳" |
| wayvnc | VNC 远程桌面（可选，需 compositor 支持） |

## 快速开始（Debian/Ubuntu 24.04+）

    sudo bash distro/desktop/install.sh

脚本会：安装 labwc/waybar/wofi/wayvnc/chromium（已装则跳过）→ 复制配置到 ~/.config →
安装 systemd 用户/系统服务（kiosk + wayvnc）→ 提示重启进入图形会话。

## 目录

    labwc/rc.xml           窗口规则与快捷键
    waybar/config.jsonc    顶栏模块
    waybar/style.css       顶栏样式
    wofi/style.css         启动器样式
    kiosk-start.sh         WebUI kiosk 启动（chromium/firefox 自动选择）
    wayvnc-start.sh        VNC 启动
    wayvnc/config          VNC 配置（5900 端口，可改）
    systemd/               两个 service 模板

## 远程访问矩阵

| 协议 | 端口 | 用途 | 状态 |
|---|---|---|---|
| HTTP | 8080 | WebUI、/api/cmd、文件、健康检查 | 已支持 |
| WS | 2939 | App/桌面实时通道（chat/command） | 已支持 |
| VNC | 5900 | 远程桌面（wayvnc，Wayland 会话） | 本目录接入 |

安全建议：默认仅监听 127.0.0.1；对外暴露请走 SSH 隧道或配置 TLS/认证（VNC 密码由 wayvnc 配置文件管理）。

## 退出策略

- kiosk 失败 → 自动回退 WebUI 浏览器窗口（不黑屏）；
- 无 Wayland → 回退 TUI 单页兜底；
- Qt6 桌面前端冻结（frontend/qt_ui），不再作为默认。
