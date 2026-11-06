# LING OS 0.13 路线与实施计划

> 2026-11-06 定稿。目标版本 0.13.0 起；停止 ISO 主线，转 Termux rootfs + Web 桌面。

## 1. 定位与目标

LING OS 是面向居家安防、敏感人物安防、数据保护、智能控制的 AI 原生设备系统。
0.13 的交付目标：在 Android Termux 上安装即用——

1. 一键安装 rootfs（proot，无需 root）；
2. 浏览器打开 8080 即进入 Web 桌面（本地/局域网、全屏、浅/深色）；
3. 智能设备接入（先米家/MQTT/HA/虚拟设备）；
4. 目录访问控制（谁可访问哪个目录）、文件管理器独立应用；
5. 安全等级、审计、AI 助手可用；
6. 同一套代码可在未来 ISO/VM 上复用，不重写。

## 2. 架构

    Termux (Android)
      └─ proot-distro / PRoot
           └─ Ubuntu noble arm64 rootfs
                ├─ lingos-web :8080  (认证 / Web 桌面 / API)
                ├─ C 服务端 (lingosd/ai.sock/daemon.sock, 仅 127.0.0.1)
                ├─ Python Agent (ai_server, 仅 unix socket)
                └─ /LINGOS 数据根

- Web 桌面：独立服务，静态资源 + /api/*；后端不直接对外。
- 平台抽象：业务代码只调 platform 接口；termux / linux / vm 三实现。
- 设备：统一设备模型 + 适配器（miIO、MQTT、HA、Tuya、eWeLink、Matter 后续），离线优先。
- ACL：应用层 SQLite + 服务端强制；不依赖 POSIX ACL（proot 下不可靠）。
- 安全：Termux 无 root，无 nft/seccomp/cgroup；应用层 ACL/审计/代理；完整平台启用内核级。

## 3. 里程碑

### 0.13.0 能跑
- P0 安全修复（WS 栈溢出、token 严格校验、文件写边界、8088 鉴权、默认 loopback）。
- rootfs 构建链（arm64 tar.zst）+ Termux 安装/启动脚本。
- lingos-web 8080：首次设置、登录、会话、健康检查、API 骨架。
- Web 桌面：登录页、桌面壳、浅/深色、PWA/全屏、设备/文件/安全/设置/AI 入口。
- 平台抽象层骨架 + 能力矩阵。

### 0.13.1 能控家
- 设备抽象层、虚拟设备、MQTT、Home Assistant、米家 miIO/云适配器。
- 设备权限、离线优先、本地事件总线、设备发现。

### 0.13.2 能管权限
- 目录 ACL 服务（用户/角色/目录/只读/可写/读写/白名单/黑名单）。
- 独立文件管理器应用。
- admin/root key、四档安全 UI（关闭/常规/限制/严格 + L4 高级）、审计。

### 0.13.3 能长期用
- Web 桌面完整：窗口/任务栏/开始菜单/通知/应用/设置。
- App 同权、局域网配对码、后台保活、备份、性能、文档。
- 手稿其余功能按能力矩阵落地或保留完整平台实现。

### 0.14
应急通讯（Bitchat 类离线参考）、灾害分类/国际等级/APP 通知、涂鸦/易微联/Matter、语音/视觉。

### 0.15
ISO/VM 完整平台复用、Windows 模拟实验室插件（默认关闭）、OTA。

## 4. 目录

    distro/rootfs/           rootfs 构建 + Termux 安装脚本
    services/lingos_web/     8080 Web 桌面服务（Python 标准库）
    services/lingos_acl/     目录 ACL（0.13.2）
    services/lingos_device/  设备适配（0.13.1）
    webdesk/                 Web 桌面静态前端
    platform/                能力矩阵与平台接口说明
    src/                     C/Python 业务（复用）
    docs/                    计划/状态/决策/审查

## 5. 能力矩阵

| 能力 | Termux/proot | Linux/ISO | VM |
|---|---|---|---|
| Web 桌面 8080 | 是 | 是 | 是 |
| C/Python 服务端 | 是（无 systemd） | 是 | 是 |
| nftables/防火墙 | 否 | 是 | 是 |
| seccomp/cgroup 沙箱 | 否 | 是 | 是 |
| 挂载/内核/驱动 | 否 | 是 | 是 |
| 摄像头/麦克风/传感器 | Termux:API 桥 | 是 | 是 |
| 蓝牙/Bitchat | Android 桥/配套 App | 是 | 是 |
| 局域网设备（miIO/MQTT/HA） | 是 | 是 | 是 |
| 后台保活 | 需 wake-lock | 是 | 是 |

## 6. 安全基线

- 默认只绑 127.0.0.1；lingos start --lan 显式开放 + 配对码/token。
- 首次设置只允许本机或配对码；密码 scrypt；会话 HttpOnly/SameSite。
- 文件 API 统一 realpath + ACL + 禁止写 /LINGOS/bin|lib|python|system|etc。
- 所有写操作审计；admin/root key 二次确认。
- P0 修复前不发布、不接入不可信网络。

## 7. 验证

- 静态：validate-syntax.sh、bash -n、py_compile、JSON。
- 单测：web 服务认证/ACL/路径穿越；设备适配器 mock；C WS/token 负向用例。
- 真机：Termux 安装 → 8080 登录 → 桌面 → 虚拟设备 → 文件 ACL → 重启保活。
- 性能：Termux 启动/首屏；Vectras 仅完整验证，不承诺 30s。

## 8. 风险

- Termux 无内核级安全；蓝牙/摄像头需 Android 桥。
- Android 后台杀进程；需 wake-lock/电池白名单。
- 米家 token/云协议变动；适配器需可更新。
- Web 桌面范围大；按 0.13.x 分阶段验收。

## 9. 继承 / 创新 / 禁止

- 继承：C/Python 业务代码、L0–L4 语义、审计链、设备协议经验、发布校验习惯。
- 创新：Termux rootfs、Web 桌面、平台抽象、应用层 ACL、Web 全屏 PWA。
- 禁止：不删现有功能代码；不破坏 P0 安全边界；不自动构建/下载/发布；不擅自改用户数据。
