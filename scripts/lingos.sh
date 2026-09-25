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

# 【0.7.0 S0-1 修复】同步 python/server/*.py → bin/
#   背景：install.sh 历史版本从不把脚本部署到 bin/ → 跑老版 ai_server →
#   App 命令大面积 "Unknown command"（先生真机 2026-09-24 取证）。
#   本函数在每次 start 时对比主文件，源更新则全量同步（幂等，开销 ~毫秒）。
_sync_python_scripts() {
    local src="$ROOT/python/server" dst="$ROOT/bin"
    [ -d "$src" ] || return 0
    [ -f "$src/ai_server.py" ] || return 0
    mkdir -p "$dst"
    if [ ! -f "$dst/ai_server.py" ] || ! cmp -s "$src/ai_server.py" "$dst/ai_server.py" 2>/dev/null; then
        cp -a "$src"/*.py "$dst/" 2>/dev/null || true
        if [ -d "$src/plugin" ]; then
            mkdir -p "$dst/plugin"
            cp -a "$src/plugin"/*.py "$dst/plugin/" 2>/dev/null || true
        fi
        rm -rf "$dst/__pycache__"
        chmod +x "$dst"/*.py 2>/dev/null || true
        echo "  ✓ Python 脚本已同步到 bin/（防老版 ai_server）"
    fi
    # 【S2-3】registry/skills 子目录预建（缺目录 → OpenFail 警告）
    mkdir -p "$ROOT/registry/builtin" "$ROOT/registry/custom" "$ROOT/registry/store" \
             "$ROOT/skills/builtin" "$ROOT/skills/custom" 2>/dev/null || true
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
        # 【0.7.0 S0-1】启动前同步 Python 脚本（防跑老版 ai_server——App 命令 Unknown 根因）
        _sync_python_scripts

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
    # 【0.7.0 S1-4 修复】不只等"文件存在"——真实 connect 测试到通为止
    #   （旧行为：文件已在但监听未就绪 → ai_server 连接拒绝 → 技能表退回内置）
    echo "  等待 lingosd/registry.sock ..."
    _REG_PY="$(_pick_python)"
    for i in $(seq 1 20); do
        if [ -S "$ROOT/run/registry.sock" ]; then
            if env -u LD_LIBRARY_PATH "$_REG_PY" -c \
                "import socket,sys; s=socket.socket(socket.AF_UNIX); s.settimeout(1); s.connect(sys.argv[1]); s.close()" \
                "$ROOT/run/registry.sock" 2>/dev/null; then
                break
            fi
        fi
        sleep 1
    done
    [ -S "$ROOT/run/registry.sock" ] && echo "  ✓ registry.sock 就绪（可连接）" \
                                     || echo "  ⚠ registry.sock 未出现（AI 将退回内置技能表）"
    sleep 1

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
    # 5) 【0.7.0 S3-1 修复】清理就绪文件（防"停止后 status 仍显示服务=1"过期显示）
    rm -f "$ROOT/run/ready"
    # 【0.7.0 S3-2】清理过期历史日志（>30 天的 lingos_YYYYMMDD_son*.log 旧部署残留）
    find "$ROOT/log" -maxdepth 1 -name 'lingos_20*.log' -mtime +30 -delete 2>/dev/null || true
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

  log)   # 【0.7.0-hf】默认看主日志 lingos.log（原默认 ai_server.log——先生反馈"看不到日志"）
         M="${3:-lingos}"; n="${2:-50}"
         if [ "$M" = "list" ]; then
             ls -la "$LOG"/ 2>/dev/null
         else
             tail -n "$n" "$LOG/$M.log" 2>/dev/null || echo "无日志（用法: bash lingos.sh log [行数] [lingos|ai_server|supervisor|api]）"
         fi ;;
  ai)    PY="$(_pick_python)"; env -u LD_LIBRARY_PATH "$PY" "$ROOT/bin/ai_server.py" ;;
  ui)    echo "Web UI: http://localhost:8080/ui  (局域网: http://<本机IP>:8080/ui)" ;;

  fg|foreground)
    # 【0.7.0-hf】前台运行（先生工作流：实时日志 + shell 交互；Ctrl-C 优雅退出）
    #   与 start 的区别：start 走后台+监督者（崩溃自动恢复但看不到日志）；
    #   fg 直接前台跑主程序（日志全显示）——退出时 v0.7.0 会自动收尾全部子进程。
    if _pids lingos_linux >/dev/null || _pids lingos_supervisor >/dev/null; then
        echo "  检测到后台模式运行中——先停止（避免双实例冲突）..."
        bash "$0" stop
        sleep 2
    fi
    cd "$ROOT" || exit 1
    _sync_python_scripts
    echo "==> 前台运行 LING OS（实时日志 + 交互界面）"
    echo "    Ctrl-C / Ctrl-Q 退出（子进程自动收尾）"
    echo ""
    bash "$ROOT/start.sh"
    _rc=$?
    # 退出后恢复终端（server mode / TUI 可能留下 raw 模式——无回显时靠这行救回）
    if [ -t 0 ]; then stty sane 2>/dev/null || true; fi
    echo "==> 已退出（终端已恢复）"
    exit $_rc
    ;;

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
用法: bash lingos.sh {start|stop|restart|fg|status|log|ui|doctor}
  start    启动（后台模式：监督者+崩溃自动恢复；日志进文件）
  fg       前台运行（实时日志 + shell 交互；Ctrl-C 退出）
  stop     停止全部
  restart  重启
  status   运行状态 + 端口检查
  log [n] [mod]  查看日志（默认 lingos 50 行；mod=lingos|ai_server|supervisor|api|list）
  ui       显示 Web UI 地址
  doctor   环境诊断（python/SSL/缺库）
EOF
    ;;
esac
