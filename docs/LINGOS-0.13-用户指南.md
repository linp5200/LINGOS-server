# LING OS 0.13 用户指南（Termux + Web 桌面）

## 1. 安装
1. Android 安装 Termux（F-Droid 版）。
2. 下载 CI rootfs artifact：Actions → rootfs → 最新运行 → lingos-rootfs-arm64。
3. 解压得到 LINGOS_rootfs_arm64_0.13.0.tar.zst 与 .sha256。
4. 在 Termux：

    pkg install -y proot zstd curl
    bash install-termux.sh LINGOS_rootfs_arm64_0.13.0.tar.zst

## 2. 启动
    lingos          # 仅本机 http://127.0.0.1:8080
    lingos --lan    # 局域网开放；终端打印 6 位配对码

Android 浏览器打开地址，可“添加到主屏幕”获得全屏 PWA 体验。
当前 Android 16 上默认使用 Termux 原生 Python 跑 Web（不依赖 proot）；完整 proot 模式可用 `lingos --proot`，但本机 proot 受内核限制可能不可用。

后台保活：Termux 已调用 termux-wake-lock；Android 设置中允许 Termux 后台运行/不优化电池。

### 调试控制台
桌面 → 设置 → 关于 → 调试控制台，或直接打开：

    http://127.0.0.1:8080/debug.html

只读诊断：health、会话、设备、最近事件、安全状态，以及 `/api/` 开头的只读 GET。不会执行 shell，不接受写操作。

## 3. 首次设置
- 设置管理员用户名/密码（至少 8 位）。
- 保存弹出的一次性 root key；高风险操作（ACL、删除）需要它。
- 局域网首次登录需配对码。

## 4. 设备
- 虚拟设备：设备页 → 添加虚拟设备（灯/锁/传感器/开关）。
- Home Assistant：设置 → 配置 HA（URL + Long-Lived Token）。
- MQTT：设置 → 配置 MQTT（host/port；需 python3-paho-mqtt）。
- 米家：设置 → 配置米家（实验，需 python-miio 与设备 token）。
- 断网优先使用局域网协议；云端仅远程。

## 5. 文件与 ACL
- 桌面“文件”或独立打开 /files.html。
- 设置 → 目录访问控制：subject（用户名/角色）+ 路径 + read/write/rw。
- admin 全权；普通用户按 ACL；L3/L4 非管理员默认拒绝文件访问。
- L3/L4 仅管理员可绕过。

## 6. App
- 设置 → 生成 App Token（只显示一次）。
- App 用 Authorization: Bearer；WS 2939 发 auth 帧。
- 详见 LINGOS-0.13-App接入.md。

## 7. 备份/审计
- 设置 → 备份：下载 tar.gz（数据库/文件/安全策略）。
- 设置 → 导出审计：JSON 事件。
- 设置 → 恢复备份：选择 lingos-backup.tar.gz；会覆盖数据库、文件和策略，需要 root key。
  服务端只接受 webdesk.db、files/、security-policy.json，校验 SQLite 完整性；失败不会写入。

## 8. 安全
- 默认只绑 127.0.0.1；不要直接暴露 18080/8088/2939。
- 局域网仅可信网络；配对码勿泄露。
- Termux 无内核级防火墙/seccomp；安全由应用层 ACL/审计承担。
- 完整平台（ISO/VM）将启用内核级能力。

## 9. 故障
- 8080 打不开：检查 lingos 进程、端口、Android 浏览器本机访问。
- 设备不可用：检查适配器配置/局域网/Token。
- 页面空白：清浏览器缓存或重装 PWA。
- 日志：/LINGOS/log/lingos-start.log；Web 服务 stdout。
