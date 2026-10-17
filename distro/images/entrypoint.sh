#!/usr/bin/env bash
# LING OS 容器入口：启动系统层 + Agent 层，并前台保活。
set -u

mkdir -p /LINGOS/log /LINGOS/run /LINGOS/data /LINGOS/system/config

if [ ! -x /LINGOS/bin/lingos_linux ]; then
    echo "[entrypoint] /LINGOS/bin/lingos_linux 缺失——镜像构建异常" >&2
    exit 1
fi

cd /LINGOS || exit 1

# 启动全链（主程序会自动拉起 lingosd / 语音等；ai_server 由主程序拉起）
if [ -x /LINGOS/lingos.sh ]; then
    bash /LINGOS/lingos.sh start || echo "[entrypoint] lingos.sh start 返回非 0（继续观察日志）"
else
    /LINGOS/bin/lingos_linux --fast || echo "[entrypoint] lingos_linux 退出"
fi

touch /LINGOS/log/lingos.log

# 前台保活：跟随主日志；容器停止时一并退出
exec tail -n +1 -F /LINGOS/log/lingos.log
