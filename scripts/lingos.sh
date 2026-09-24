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

# 【2026-09-19】全链进程清单（预检/停止/补刀共用）
_ALL_PROCS="lingos_supervisor lingos_linux lingosd lingos_alertd lingos_visiond lingos_voiced ai_server.py"
_ORPHAN_PROCS="lingos_supervisor lingosd lingos_alertd lingos_visiond lingos_voiced ai_server.py"

_wait_gone() {
    # 等待某模式进程全部退出（最多 $2 秒）；返回 0=已退净
    local pat="$1" secs="${2:-5}" i
    for i in $(seq 1 "$secs"); do
        _pids "$pat" >/dev/null || return 0
        sleep 1
    done
    return 1
}

# ---------- 子命令 ----------
case "$CMD" in
  start)
    echo "==> 启动 LING OS  (root=$ROOT)"
    mkdir -p "$ROOT/log" "$ROOT/run"
    # 【0.4.4】主程序历史上用相对路径 "./lingosd" 启动守护进程 →
    #   必须 cd 到 $ROOT 再启动（否则 cwd 不对 → lingosd 起不来 → 端口全不通）
    cd "$ROOT" || exit 1

    SKIP_START=0
    # ---------- 【2026-09-19】单实例全链预检（防双实例端口冲突——日志实证根因） ----------
    if _pids lingos_linux >/dev/null; then
        echo "  主程序已在运行（跳过启动——仅执行就绪等待）"
        SKIP_START=1
    else
        RESID=""
        for pat in $_ORPHAN_PROCS; do
            _pids "$pat" >/dev/null && RESID="$RESID $pat"
        done
        if [ -n "$RESID" ]; then
            echo "  ⚠ 检测到残留进程:$RESID"
            echo "    （上次未正常退出的孤儿——自动清理后继续）"
            for pat in $_ORPHAN_PROCS; do
                pkill -TERM -f "$pat" 2>/dev/null
            done
            sleep 2
            for pat in $_ORPHAN_PROCS; do
                p=$(pgrep -f "$pat" 2>/dev/null) && kill -9 $p 2>/dev/null
            done
        fi
    fi

    if [ "$SKIP_START" = "0" ]; then
        # 【2026-09-19】监督者优先：由其拉起主程序（崩溃自动恢复 / 心跳 / 重启限流）
        SUP_BIN=""
        if [ -x "$ROOT/bin/lingos_supervisor" ]; then
            SUP_BIN="$ROOT/bin/lingos_supervisor"
        elif [ -x "$ROOT/lingos_supervisor" ]; then
            SUP_BIN="$ROOT/lingos_supervisor"
        fi

        if [ -n "$SUP_BIN" ]; then
            if [ -d "$ROOT/lib" ] && [ -n "$(ls -A "$ROOT/lib" 2>/dev/null)" ]; then
                LD_LIBRARY_PATH="$ROOT/lib:${LD_LIBRARY_PATH:-}" \
                    nohup "$SUP_BIN" >> "$LOG/supervisor.log" 2>&1 &
            else
                nohup "$SUP_BIN" >> "$LOG/supervisor.log" 2>&1 &
            fi
            echo "  监督者已启动 (pid $!) → 由其拉起主程序（输出 → $LOG/supervisor.log）"
        else
            echo "  ⚠ 未找到 lingos_supervisor——降级为直接启动（无崩溃自动恢复）"
            if [ -x "$ROOT/start.sh" ]; then
                SRUN="$ROOT/start.sh"
            elif [ -x "$ROOT/bin/lingos_linux" ]; then
                SRUN="$ROOT/bin/lingos_linux"
            else
                echo "  ✗ 找不到主程序（$ROOT/start.sh 或 $ROOT/bin/lingos_linux）"; exit 1
            fi
            # 关键：不全局污染 LD_LIBRARY_PATH —— 仅给本次执行
            if [ -d "$ROOT/lib" ] && [ -n "$(ls -A "$ROOT/lib" 2>/dev/null)" ]; then
                LD_LIBRARY_PATH="$ROOT/lib:${LD_LIBRARY_PATH:-}" nohup "$SRUN" > "$LOG/lingos.log" 2>&1 &
            else
                nohup "$SRUN" > "$LOG/lingos.log" 2>&1 &
            fi
            echo "  主程序已启动 (pid $!)"
        fi
    fi
    # 1b) 等 lingosd 的 registry.sock 就绪（ai_server 启动时要用它加载技能表）
    echo "  等待 lingosd/registry.sock ..."
    for i in $(seq 1 20); do
        [ -S "$ROOT/run/registry.sock" ] && break
        sleep 1
    done
    [ -S "$ROOT/run/registry.sock" ] && echo "  ✓ registry.sock 就绪" \
                                     || echo "  ⚠ registry.sock 未出现（AI 将退回内置技能表）"
    sleep 2

    # 2) 兜底：若 C 端未能拉起 ai_server，则由本脚本以「干净环境 + 系统 python3」拉起
    if ! _pids ai_server.py >/dev/null; then
        PY="$(_pick_python)"
        echo "  ai_server 未运行 → 用 $PY 拉起"
        # 【0.4.4】LINGOS_NO_PARENT_MONITOR=1 —— 本脚本启动完就退出，
        #   不设此变量 ai_server 会在 5 秒后误判"父进程已死"而自杀
        #   （先生 2026-09-12 实测：Parent process terminated, exiting）
        env -u LD_LIBRARY_PATH LINGOS_NO_PARENT_MONITOR=1 \
            nohup "$PY" -u "$ROOT/bin/ai_server.py" > "$LOG/ai_server.log" 2>&1 &
        echo "  ai_server 已启动 (pid $!)"
    else
        echo "  ai_server 已由主程序拉起"
    fi
    echo ""
    echo "  等待服务就绪（最多 15s）..."
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        if (echo >/dev/tcp/127.0.0.1/8080) 2>/dev/null; then
            echo "  ✓ HTTP 8080 已就绪"; break
        fi
        sleep 1
    done
    echo ""
    bash "$0" status
    ;;

  stop)
    echo "==> 停止 LING OS（优雅优先，最多等待约 10 秒）"
    # 1) 主程序先行：其会优雅收尾（保存注册表 / 通知监督者 / 标记正常退出）
    if _pids lingos_linux >/dev/null; then
        pkill -TERM -f "lingos_linux" 2>/dev/null && echo "    ↓ 已告知主程序优雅停止"
        _wait_gone lingos_linux 8 || echo "    ⚠ 主程序未在 8s 内退出（将强杀）"
    fi
    # 2) 监督者（主程序退出时已通知其停止；此处兜底）
    if _pids lingos_supervisor >/dev/null; then
        pkill -TERM -f "lingos_supervisor" 2>/dev/null && echo "    ↓ 已停 lingos_supervisor"
        _wait_gone lingos_supervisor 3
    fi
    # 3) 其余守护与 AI
    for pat in lingosd lingos_alertd lingos_visiond lingos_voiced ai_server.py; do
        pkill -TERM -f "$pat" 2>/dev/null && echo "    ↓ 已停 $pat"
    done
    sleep 1
    # 4) 顽固进程补刀（TERM 无效者）
    for pat in $_ALL_PROCS; do
        p=$(pgrep -f "$pat" 2>/dev/null) && { kill -9 $p 2>/dev/null && echo "    ✗ 强杀 $pat"; }
    done
    echo "  已停止"
    ;;

  restart) bash "$0" stop; sleep 2; bash "$0" start ;;

  status)
    echo "==> LING OS 状态"
    s(){ printf "  %-16s %s\n" "$1" "$2"; }
    for b in lingos_supervisor lingos_linux lingosd lingos_alertd lingos_visiond lingos_voiced; do
        p=$(_pids "$b" | head -1)
        [ -n "$p" ] && s "$b" "运行中 (pid $p)" || s "$b" "未运行"
    done
    p=$(_pids ai_server.py | head -1)
    [ -n "$p" ] && s "ai_server" "运行中 (pid $p)" || s "ai_server" "未运行"
    # 【2026-09-19】就绪文件（启动就绪报告——单行 JSON）
    if [ -f "$ROOT/run/ready" ]; then
        s "ready" "$(head -c 220 "$ROOT/run/ready" 2>/dev/null)"
    else
        s "ready" "(无——尚未完成一次启动就绪)"
    fi
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
