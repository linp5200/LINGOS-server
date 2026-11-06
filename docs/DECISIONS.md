# LING OS 关键决策

1. 桌面：0.13 起以 Web 桌面为默认；Cinnamon/XFCE 随 ISO 冻结，仅作完整平台/救援保留。
2. 运行时：Termux + proot Ubuntu noble arm64 rootfs；无 systemd；服务用脚本启动；默认无 root。
3. Web 桌面：8080 同时承载桌面与 /api/*；后端仅 unix socket/127.0.0.1；默认 loopback，lingos start --lan 显式开放 + 配对码。
4. 平台抽象：业务只调 platform 接口；termux（应用层降级）/linux（nft/seccomp/systemd）/vm（完整）三实现；不删功能代码。
5. 安全：Termux 无内核级能力，采用应用层 ACL/审计/加密/代理；完整平台再启用内核级；P0 先修。
6. 设备：统一设备模型 + 适配器；先米家 miIO/云、MQTT、HA、虚拟设备；离线优先；涂鸦/易微联/Matter 后续。
7. 目录 ACL：应用层 SQLite + 服务端强制；回答谁可访问哪个目录；文件管理器作为独立 Web 应用。
8. 安全等级：内核保留 L0–L4；UI 四档（关闭/常规/限制/严格），L4 作高级 MAX；admin 密码 + root key 文件。
9. 版本：0.13.0 能跑 → 0.13.1 设备 → 0.13.2 ACL → 0.13.3 完整；0.14 应急/灾害；0.15 ISO/VM/Windows 实验室。
10. 发布：arm64 优先；Termux rootfs tar.zst + SHA256；不构建 ISO；P0 修复前不发布。
