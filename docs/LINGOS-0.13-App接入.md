# LING OS App 接入（0.13）

Web 桌面与 App 共用同一套认证/ACL；默认只开本机，局域网由 Termux 启动终端显式开启。

## 1. 配对
服务端以 --lan 启动时，终端打印 6 位配对码。

    POST /api/pair
    {"pair":"123456","name":"android-phone"}
    -> {"ok":true,"token":"...","user":"admin","role":"admin"}

Token 只显示一次，请存 Android Keystore。

## 2. 调用
    Authorization: Bearer <token>

- GET  /api/me
- GET  /api/devices
- POST /api/devices/<id>/command
- POST /api/cmd              （代理到 C 后端；与 Web 同权）
- GET  /api/security
- GET  /api/fs/list?path=/...

## 3. WebSocket
Token 会同步写入 /LINGOS/state/tokens.json；C 服务 2939 的 auth 帧使用同一 token。
局域网模式需 LINGOS_LAN=1 启动；App 连接后发送：

    {"type":"auth","token":"<token>"}

C 侧返回 auth_ok 后即可 chat/command。HTTP 与 WS 同权；真实设备联调待 Termux 真机。

## 4. 权限
- admin：全权；高风险操作需 root key（X-Lingos-Root-Key）。
- user：按目录 ACL（设置 → 目录访问控制）；L3/L4 默认拒绝文件访问。
- 所有写操作记入审计事件。

## 5. 注意
- 不要把 18080/8088/2939 直接暴露到公网；只开 lingos-web 8080。
- Termux 无内核级防火墙；局域网仅可信网络使用，配对码勿泄露。
