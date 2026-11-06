# LING OS 项目状态

> 更新时间：2026-11-06
> 当前目标版本：0.13.0（Termux rootfs + Web 桌面）
> 上一发布：v0.12.0（ISO/Cinnamon，冻结；有 P0，勿联网分发）

## 0.13 方向
- 停止 ISO 主线，转 Termux + proot Ubuntu noble arm64 rootfs。
- 桌面转 Web（8080，本地/局域网，全屏 PWA，浅/深色，DSH/终末地风格）。
- 设备接入：米家/MQTT/HA/虚拟设备，离线优先；涂鸦/易微联/Matter 后续。
- 目录 ACL 与文件管理器分离；admin/root key；四档安全 UI。
- 功能完整保留，平台抽象层区分 termux/linux/vm；未来 ISO 复用。

## 0.13.0 进行中
- [x] 0.13 路线/状态/决策文档
- [x] P0：WS 栈溢出、token 严格校验、文件写边界、8088 鉴权、默认 loopback
- [x] rootfs 构建 + Termux 安装脚本（代码完成；arm64 实际构建待 CI）
- [x] lingos-web 8080 服务（认证/设备/文件/安全/AI/命令代理/审计导出/备份，13 项单测通过）
- [x] Web 桌面骨架（DSH/终末地风格、浅/深色、PWA/全屏、应用视图）
- [x] 平台抽象/能力矩阵（platform/README.md）
- [x] 测试：tests/test_lingos_web.py + validate-syntax 接入
- [x] 设备适配层框架（虚拟/HA/MQTT/miio 实验；真实设备待 0.13.1 验证）
- [x] 目录 ACL 后端 + 管理 UI + 用户管理 + 四档安全 + root key + 独立文件管理器（files.html）
- [x] API Token / 局域网配对码 / Termux wake-lock 保活
- [x] 通知中心 + 最小化/任务栏恢复（视图级）
- [x] App WS token 同步（写入 /LINGOS/state/tokens.json）
- [ ] 多窗口拖拽、后台保活真机、其余 8 个二进制/rootfs 构建（0.13.3 收尾）

## 0.12.0 已知问题（冻结，不修）
- stage7 rsync rc24 被当致命错误；live LightDM 接管；手动/live 无 TTY。
- C/Python P0：WS 栈溢出、token 子串绕过、8088 未鉴权危机、OOBE 不重启 LightDM。
- 源码未入库/三方不一致；仅作历史版本。

## 验证约定
- 先 scripts/validate-syntax.sh；再单测；最后只触发一次 CI/发布。
- 不自动下载/构建/发布；Termux/真机验证后再发布。
