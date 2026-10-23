#!/bin/bash
# ============================================================
# LING OS live/安装器 通用日志 + 阶段进度 + 外部回传（0.9.12）
# 被 lingos-install / lingos-live-session / lingos-diag 引用。
#
# 固定标记（CI / 串口 / 共享文件夹抓取）：
#   LINGOS-BOOT-MODE=<mode>
#   LINGOS-STAGE=<n>/<total> percent=<p> eta=<s> <message>
#   LINGOS-INSTALL-DONE
#   LINGOS-INSTALL-FAIL=code=<n> <message>
#   LINGOS-DIAG-READY=<path>
#   LINGOS-LIVE-READY ip=<ip> url=<url>
#
# 输出通道（全部 best-effort，任一失败不影响安装）：
#   1) 日志文件
#   2) /dev/tty0、/dev/tty1（VNC 可见）
#   3) /dev/ttyAMA0、/dev/ttyS0（串口）
#   4) /dev/kmsg（dmesg）
#   5) 共享文件夹（QEMU fat:rw: / Vectras 共享目录）
#   6) 网络回传（QEMU user-net：guest -> 10.0.2.2:5555）
# ============================================================
if [ -n "${LINGOS_LOG_SH_LOADED:-}" ]; then
    return 0 2>/dev/null || exit 0
fi
LINGOS_LOG_SH_LOADED=1

LINGOS_MODE="${LINGOS_MODE:-unknown}"
LINGOS_LOG="${LINGOS_LOG:-/var/log/lingos-install.log}"
LINGOS_STATE_DIR="${LINGOS_STATE_DIR:-/run/lingos-install}"
LINGOS_STATE="$LINGOS_STATE_DIR/state.json"
LINGOS_SHARE_MNT="${LINGOS_SHARE_MNT:-/run/lingos-share}"
LINGOS_TOTAL_STAGES="${LINGOS_TOTAL_STAGES:-10}"
LINGOS_STAGE=0
LINGOS_START_TS=$(date +%s)
LINGOS_SHARE=""
LINGOS_REPORT_HOST="${LINGOS_REPORT_HOST:-10.0.2.2}"
LINGOS_REPORT_PORT="${LINGOS_REPORT_PORT:-5555}"
LINGOS_CONSOLE="${LINGOS_CONSOLE:-/dev/tty0 /dev/tty1 /dev/ttyAMA0 /dev/ttyS0}"
_LINGOS_NET_DONE=0
_LINGOS_LAST_NET_PCT=-1

mkdir -p "$LINGOS_STATE_DIR" 2>/dev/null || true
mkdir -p "$(dirname "$LINGOS_LOG")" 2>/dev/null || true
touch "$LINGOS_LOG" 2>/dev/null || true

_now() { date +%s; }
_stamp() { date '+%F %T'; }

# 输出到：日志文件 + 所有控制台 + 串口 + kmsg
_emit() {
    local line="$1" t
    printf '%s\n' "$line" 2>/dev/null >> "$LINGOS_LOG" || true
    printf '<6>%s\n' "$line" 2>/dev/null > /dev/kmsg || true
    # 串口直接写（CI/真机调试依赖）；tty0/tty1 用 timeout 保护，避免 getty/console buffer 阻塞整个安装
    for t in /dev/ttyAMA0 /dev/ttyS0; do
        [ -c "$t" ] && { printf '%s\n' "$line" 2>/dev/null > "$t" || true; }
    done
    for t in /dev/tty0 /dev/tty1; do
        [ -c "$t" ] || continue
        printf '%s\n' "$line" 2>/dev/null | timeout 1 tee -a "$t" >/dev/null 2>&1 || true
    done
    printf '%s\n' "$line" >&2
}

log()  { _emit "[$(_stamp)] [INFO] $*"; }
warn() { _emit "[$(_stamp)] [WARN] $*"; }
err()  { _emit "[$(_stamp)] [FAIL] $*"; }

json_escape() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }

human_eta() {
    local s="${1:-0}"
    [ "$s" -le 0 ] 2>/dev/null && { echo "即将完成"; return; }
    if [ "$s" -lt 60 ] 2>/dev/null; then echo "${s} 秒"; else echo "$((s / 60)) 分 $((s % 60)) 秒"; fi
}

# ---------- 共享文件夹（Vectras / QEMU fat:rw:） ----------
find_share() {
    [ -n "$LINGOS_SHARE" ] && return 0
    mkdir -p "$LINGOS_SHARE_MNT" 2>/dev/null || return 1
    local live_src part fstype dev
    live_src=$(findmnt -n -o SOURCE /run/live/medium 2>/dev/null || true)
    while read -r part fstype; do
        case "$fstype" in vfat|exfat|msdos) ;; *) continue ;; esac
        dev="/dev/$part"
        [ "$dev" = "$live_src" ] && continue
        mount -t vfat -o rw,umask=0000 "$dev" "$LINGOS_SHARE_MNT" 2>/dev/null || continue
        if touch "$LINGOS_SHARE_MNT/.lingos-write-test" 2>/dev/null; then
            rm -f "$LINGOS_SHARE_MNT/.lingos-write-test" 2>/dev/null || true
            LINGOS_SHARE="$LINGOS_SHARE_MNT"
            log "共享文件夹已启用: $dev -> $LINGOS_SHARE"
            return 0
        fi
        umount "$LINGOS_SHARE_MNT" 2>/dev/null || true
    done < <(lsblk -ln -o NAME,FSTYPE 2>/dev/null)
    return 1
}

report_share() {
    find_share || return 0
    {
        printf 'LING OS 安装进度\n'
        printf '模式: %s\n' "$LINGOS_MODE"
        printf '状态: %s\n' "$1"
        printf '阶段: %s/%s\n' "$LINGOS_STAGE" "$LINGOS_TOTAL_STAGES"
        printf '进度: %s%%\n' "$2"
        printf '消息: %s\n' "$3"
        printf '更新时间: %s\n' "$(_stamp)"
    } 2>/dev/null > "$LINGOS_SHARE/lingos-install-progress.txt" || true
    cp -f "$LINGOS_STATE" "$LINGOS_SHARE/lingos-install-state.json" 2>/dev/null || true
    cp -f "$LINGOS_LOG"   "$LINGOS_SHARE/lingos-install.log" 2>/dev/null || true
}

# ---------- 网络回传（QEMU user-net：guest -> 宿主 10.0.2.2） ----------
_net_once() {
    [ "$_LINGOS_NET_DONE" = "1" ] && return 0
    _LINGOS_NET_DONE=1
    local i
    for i in eth0 ens3 enp0s3 enp0s1; do
        ip link show "$i" >/dev/null 2>&1 || continue
        ip link set "$i" up 2>/dev/null || true
        if command -v dhclient >/dev/null 2>&1; then
            timeout 10 dhclient -1 "$i" >/dev/null 2>&1 || true
        fi
    done
}

report_net() {
    command -v nc >/dev/null 2>&1 || return 0
    _net_once
    printf '%s\n' "$1" | timeout 3 nc -w 2 "$LINGOS_REPORT_HOST" "$LINGOS_REPORT_PORT" >/dev/null 2>&1 || true
}

# ---------- 状态 / 进度 ----------
write_state() {
    local name="$1" percent="$2" msg="$3" extra="${4:-}" now elapsed eta_json json
    now=$(_now); elapsed=$(( now - LINGOS_START_TS ))
    if [ "$percent" -ge 100 ] 2>/dev/null; then
        eta_json=0
    elif [ "$percent" -gt 0 ] 2>/dev/null; then
        eta_json=$(( elapsed * (100 - percent) / percent ))
    else
        eta_json=null
    fi
    json=$(printf '{"ok":true,"mode":"%s","stage":%s,"stage_name":"%s","total":%s,"percent":%s,"message":"%s","eta_seconds":%s,"elapsed_seconds":%s,"ts":"%s"%s}' \
        "$(json_escape "$LINGOS_MODE")" "$LINGOS_STAGE" "$(json_escape "$name")" "$LINGOS_TOTAL_STAGES" "$percent" \
        "$(json_escape "$msg")" "$eta_json" "$elapsed" "$(date -u +%FT%TZ)" "$extra")
    printf '%s\n' "$json" 2>/dev/null > "$LINGOS_STATE.tmp" && mv "$LINGOS_STATE.tmp" "$LINGOS_STATE" 2>/dev/null || true
    _emit "LINGOS-STAGE=$LINGOS_STAGE/$LINGOS_TOTAL_STAGES percent=$percent eta=${eta_json}s $msg"
    report_share "进行中" "$percent" "$msg"
    if [ "$percent" != "$_LINGOS_LAST_NET_PCT" ] && [ $(( percent % 10 )) -eq 0 ]; then
        _LINGOS_LAST_NET_PCT="$percent"
        report_net "LINGOS progress mode=$LINGOS_MODE stage=$LINGOS_STAGE/$LINGOS_TOTAL_STAGES percent=$percent msg=$msg"
    fi
}

stage() {              # stage <n> <name> <message>
    LINGOS_STAGE="$1"
    local pct=$(( LINGOS_STAGE * 100 / LINGOS_TOTAL_STAGES ))
    [ "$pct" -gt 98 ] && pct=98
    write_state "$2" "$pct" "${3:-$2}"
}

stage_progress() {     # stage_progress <percent> <name> <message>
    local pct="$1"
    [ "$pct" -lt 0 ] && pct=0
    [ "$pct" -gt 100 ] && pct=100
    write_state "$2" "$pct" "$3"
}

mark_done() {
    write_state "done" 100 "安装完成"
    _emit "LINGOS-INSTALL-DONE mode=$LINGOS_MODE ts=$(_stamp)"
    report_net "LINGOS-INSTALL-DONE mode=$LINGOS_MODE"
}

fail_msg() {
    local code="${1:-1}" msg="${2:-未知错误}" json
    _emit "LINGOS-INSTALL-FAIL=code=$code $msg"
    err "失败(code=$code): $msg"
    json=$(printf '{"ok":false,"mode":"%s","code":%s,"message":"%s","stage":%s,"ts":"%s"}' \
        "$(json_escape "$LINGOS_MODE")" "$code" "$(json_escape "$msg")" "$LINGOS_STAGE" "$(date -u +%FT%TZ)")
    printf '%s\n' "$json" > "$LINGOS_STATE.tmp" 2>/dev/null && mv "$LINGOS_STATE.tmp" "$LINGOS_STATE" 2>/dev/null || true
    report_share "失败" "0" "$msg"
    report_net "LINGOS-INSTALL-FAIL code=$code msg=$msg"
}

read_cmdline_param() {  # read_cmdline_param <key>
    local key="$1" a
    for a in $(cat /proc/cmdline 2>/dev/null); do
        case "$a" in "$key="*) printf '%s\n' "${a#*=}"; return 0 ;; esac
    done
    return 1
}

boot_info() {
    _emit "LINGOS-BOOT-MODE=$LINGOS_MODE"
    _emit "LINGOS-BOOT-INFO kernel=$(uname -r) arch=$(uname -m) cmdline=$(cat /proc/cmdline 2>/dev/null || true)"
    log "模式=$LINGOS_MODE 内核=$(uname -r) 架构=$(uname -m)"
    [ -f /etc/lingos-image-info ] && _emit "LINGOS-IMAGE-INFO $(cat /etc/lingos-image-info 2>/dev/null || true)"
    return 0
}