# LING OS 远程访问（HTTP / WebSocket / VNC）

> 先生要求：至少支持 VNC / HTTP / WS 中的两种。现状 HTTP + WS 已具备，本文件补 VNC 并给出统一矩阵。

## 端口与用途

| 协议 | 默认端口 | 用途 | 服务端 | 认证 |
|---|---|---|---|---|
| HTTP | 8080 | WebUI、/api/cmd、文件接口、/system/health | C http_server（lingosd） | 访问令牌 / 局域网限制 |
| WebSocket | 2939 | App/桌面实时通道：chat 流、command、auth | C websocket_server（lingosd → Python） | token 认证 |
| VNC | 5900 | 远程桌面（Wayland 会话） | wayvnc（distro/desktop） | wayvnc 认证/密码或 SSH 隧道 |

## 启动

    # WebUI / HTTP / WS（随 lingosd 启动，无需单独操作）
    systemctl --user start lingos-ws   # 如拆分为独立服务时（当前随主进程）

    # VNC（Wayland 会话下）
    bash distro/desktop/install.sh      # 安装 wayvnc + 配置
    systemctl --user start wayvnc

    # 或从命令面/App：
    #   remote_status  -> 返回 HTTP/WS/VNC 端口与存活
    #   vnc_start / vnc_stop

## 配置

/LINGOS/system/config/remote.json（不存在则用默认值）：

    {"http_port": 8080, "ws_port": 2939, "vnc_port": 5900, "vnc_listen": "127.0.0.1"}

## 安全

1. 默认 VNC 只监听 127.0.0.1；对外必须配置认证（wayvnc password_file）或走 SSH 隧道：
   ssh -L 5900:127.0.0.1:5900 user@host
2. HTTP/WS 使用访问令牌；公网暴露应加 TLS 反代（nginx/caddy）。
3. 危险操作（系统控制/文件/家居）走权限闸门与审批，不使用 VNC 通道绕过。

## 命令面

| 命令 | 说明 |
|---|---|
| remote_status | HTTP/WS/VNC 端口、监听、存活、可用 URL |
| vnc_start | 启动 wayvnc（优先 systemd --user） |
| vnc_stop | 停止 wayvnc |
