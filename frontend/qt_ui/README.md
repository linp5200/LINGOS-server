# LING OS Qt6 桌面前端（frontend/qt_ui）

先生 2026-09-04 裁决：**Qt6 Widgets** 取代 GTK3 `lingos_gui`（直接废）。
双显示形态之一（桌面原生）；网页形态见 `webui/`（浏览器 8080/ui）。

## 特性
- 灰白地形等高线背景（高斯峰 + Marching Squares——与 HTML 原型同算法）
- 统一 API 客户端（HTTP POST /api/cmd，与 Web/App 同协议）
- 侧栏导航：控制台（真数据 system_info/alert_query）/ 连接主机 / 预警 / 天气 / 视觉 / 日志 / 设置 / 关于
- 真数据原则：无 server 显示 -- 与离线态，不模拟
- 0.4.3 命名/版本：显示 LN-0.4.3

## 构建（目标机 Qt6）
```bash
cd frontend/qt_ui
mkdir build && cd build
cmake .. && make
./lingos_ui                # 桌面运行
QT_QPA_PLATFORM=offscreen ./lingos_ui   # 无显示环境
```
依赖：qt6-base-dev（Ubuntu 24.04/Debian 13 默认；老系统需自行装 Qt6）

## 连接
侧栏「◈ 连接主机」输入 `host:port`（默认 127.0.0.1:8080）→ 经 /api/cmd 拉真数据。
沙箱已实测：编译通过 + offscreen 运行正常（Alpine musl + Qt6.8）。
