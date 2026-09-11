#!/usr/bin/env bash
# ============================================================
# LING OS 日常管理脚本（0.4.4 —— 启动/终止尽量简便）
# 用法：bash lingos.sh <命令>
#   start | stop | restart | status | log | ui | ai | shell | doctor
# 例：  bash lingos.sh start        # 一键启动（ai_server + 主程序）
#       bash lingos.sh stop         # 一键停止
# 说明：数据根 /LINGOS（先生架构）；ai_server 用系统 python3（避免坏 venv）
# ============================================================
set -u
ROOT="${LINGOS_ROOT:-/LINGOS}"
LOG="$ROOT/log"
CMD="${1:-status}"

# ---------- 工具函数 ----------
_pids() { pgrep -f "$1" 2>/dev/null; }
_pick_python() {
    # 优先系统 python3（SSL 正常）；跳过 proot loader / venv
    for p in /usr/bin/python3 /usr/bin/python3.13 /usr/bin/python3.12 python3; do
        if command -v "$p" >/dev/null 2>&1; then
            if env -u LD_LIBRARY_PATH "$p" -c "import ssl,requests" >/dev/null 2>&1; then
                echo "$p"; return 0
            fi
        fi
    done
    # 退而求其次：任何能跑的 python3
    command -v python3 || echo python3
}

_stop() {
    local sig="${1:-TERM}"
    for pat in lingos_linux lingosd lingos_supervisor lingos_alertd lingos_visiond lingos_voiced ai_server.py; do
        pkill "-$sig" -f "$pat" 2>/dev/null && echo "    ↓ 已停 $pat"
    done
    sleep 1
}

# ---------- 子命令 ----------
case "$CMD" in
  start)
    echo "==> 启动 LING OS  (root=$ROOT)"
    mkdir -p "$ROOT/log" "$ROOT/run"

    # 1) 先起主程序（C 端会自行拉起 ai_server —— 0.4.4 已修 LD 污染）
    if [ -x "$ROOT/start.sh" ]; then
        SRUN="$ROOT/start.sh"
    elif [ -x "$ROOT/bin/lingos_linux" ]; then
        SRUN="$ROOT/bin/lingos_linux"
    else
        echo "  ✗ 找不到主程序（$ROOT/start.sh 或 $ROOT/bin/lingos_linux）"; exit 1
    fi

    if _pids lingos_linux >/dev/null; then
        echo "  主程序已在运行"
    else
        # 关键：不全局污染 LD_LIBRARY_PATH —— 仅给本次执行
        if [ -d "$ROOT/lib" ] && [ -n "$(ls -A "$ROOT/lib" 2>/dev/null)" ]; then
            LD_LIBRARY_PATH="$ROOT/lib:${LD_LIBRARY_PATH:-}" nohup "$SRUN" > "$LOG/lingos.log" 2>&1 &
        else
            nohup "$SRUN" > "$LOG/lingos.log" 2>&1 &
        fi
        echo "  主程序已启动 (pid $!)"
    fi
    sleep 4

    # 2) 兜底：若 C 端未能拉起 ai_server，则由本脚本以「干净环境 + 系统 python3」拉起
    if ! _pids ai_server.py >/dev/null; then
        PY="$(_pick_python)"
        echo "  ai_server 未运行 → 用 $PY 拉起"
        env -u LD_LIBRARY_PATH nohup "$PY" -u "$ROOT/bin/ai_server.py" > "$LOG/ai_server.log" 2>&1 &
        echo "  ai_server 已启动 (pid $!)"
    else
        echo "  ai_server 已由主程序拉起"
    fi
    echo ""
    bash "$0" status
    ;;

  stop)
    echo "==> 停止 LING OS"
    _stop TERM
    # 顽固进程补刀
    for pat in lingos_linux ai_server.py; do
        p=$(pgrep -f "$pat" 2>/dev/null) && { kill -9 $p 2>/dev/null && echo "    ✗ 强杀 $pat"; }
    done
    echo "  已停止"
    ;;

  restart) bash "$0" stop; sleep 2; bash "$0" start ;;

  status)
    echo "==> LING OS 状态"
    s(){ printf "  %-16s %s\n" "$1" "$2"; }
    for b in lingos_linux lingosd lingos_supervisor; do
        p=$(_pids "$b" | head -1)
        [ -n "$p" ] && s "$b" "运行中 (pid $p)" || s "$b" "未运行"
    done
    p=$(_pids ai_server.py | head -1)
    [ -n "$p" ] && s "ai_server" "运行中 (pid $p)" || s "ai_server" "未运行"
    # 端口
    for pair in "2937 TCP认证" "2939 WS" "8080 HTTP/WebUI" "8088 语音REST"; do
        port=${pair%% *}; name=${pair#* }
        if (echo >/dev/tcp/127.0.0.1/$port) 2>/dev/null; then s "$name" ":$port 可达"; else s "$name" ":$port 未监听"; fi
    done
    ;;

  log)   tail -n "${2:-30}" "$LOG/${3:-ai_server}.log" 2>/dev/null || echo "无日志" ;;
  ai)    PY="$(_pick_python)"; env -u LD_LIBRARY_PATH "$PY" "$ROOT/bin/ai_server.py" ;;
  ui)    echo "Web UI: http://localhost:8080/ui  (局域网: http://<本机IP>:8080/ui)" ;;

  doctor)
    echo "==> 环境诊断"
    echo "  python3  := $(command -v python3)"
    for p in /usr/bin/python3 python3; do
        command -v "$p" >/dev/null 2>&1 || continue
        printf "  %-28s " "$p"
        env -u LD_LIBRARY_PATH "$p" -c "import ssl;print('SSL',ssl.OPENSSL_VERSION)" 2>&1 | head -1
    done
    echo "  requests : $(env -u LD_LIBRARY_PATH python3 -c 'import requests;print("OK")' 2>&1 | head -1)"
    echo -n "  LD_LIBRARY_PATH = "; echo "${LD_LIBRARY_PATH:-(未设)}"
    [ -x "$ROOT/bin/lingos_linux" ] && ldd "$ROOT/bin/lingos_linux" 2>/dev/null | grep 'not found' | sed 's/^/  缺库: /' || true
    ;;

  *)
    cat <<EOF
用法: bash lingos.sh {start|stop|restart|status|log|ui|doctor}
  start    启动（主程序 + ai_server，自动避坑）
  stop     停止全部
  restart  重启
  status   运行状态 + 端口检查
  log [n] [mod]  查看日志（默认 ai_server 30 行）
  ui       显示 Web UI 地址
  doctor   环境诊断（python/SSL/缺库）
EOF
    ;;
esac
