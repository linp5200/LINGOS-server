#!/usr/bin/env bash
# LING OS 容器入口：以"已配置的设备"形态无头启动系统层 + Agent 层。
set -u

export LINGOS_BUNDLED=1
export LINGOS_ROOT=/LINGOS

mkdir -p /LINGOS/log /LINGOS/run /LINGOS/data /LINGOS/Debug/log /LINGOS/system/config

if [ ! -x /LINGOS/bin/lingos_linux ]; then
    echo "[entrypoint] /LINGOS/bin/lingos_linux 缺失——镜像构建异常" >&2
    exit 1
fi

# 容器镜像 = 已配置设备：预置最小配置，跳过首启向导（无 TTY 下向导读 EOF，端口不会启动）
if [ ! -s /LINGOS/system/config/state.json ]; then
    printf '%s\n' '{"system_configured": true, "last_config_time": "1970-01-01T00:00:00Z"}' > /LINGOS/system/config/state.json
    echo "[entrypoint] seeded state.json (headless configured)"
fi
if [ ! -s /LINGOS/system/config/config.json ]; then
    printf '%s\n' '{"schema_version": 1, "ai": {"ai_config": {"backend": "deepseek", "language": "zh", "stream_enabled": true, "thinking_enabled": false}, "provider": {"active": "deepseek", "deepseek": {"api_key": "", "model": "deepseek-v4-pro", "base_url": "https://api.deepseek.com"}, "ollama": {"url": "http://127.0.0.1:11434", "model": "qwen3"}}}}' > /LINGOS/system/config/config.json
    echo "[entrypoint] seeded config.json (headless default)"
fi

cd /LINGOS || exit 1

if [ -x /LINGOS/lingos.sh ]; then
    bash /LINGOS/lingos.sh start || echo "[entrypoint] lingos.sh start 返回非 0（继续观察日志）"
else
    /LINGOS/bin/lingos_linux --fast || echo "[entrypoint] lingos_linux 退出"
fi

touch /LINGOS/log/lingos.log
exec tail -n +1 -F /LINGOS/log/lingos.log
