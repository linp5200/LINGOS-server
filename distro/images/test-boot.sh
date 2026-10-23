#!/usr/bin/env bash
# ============================================================
# LING OS ISO 启动冒烟测试（CI / 本地，UEFI + 串口日志）
# 用法：test-boot.sh <x86_64|aarch64> <iso> [超时秒=900] [boot|full]
#   boot —— 只验证引导链 + 安装器开始写盘（快，CI 默认；命中后主动结束 QEMU）
#   full —— 验证完整安装到 INSTALL-DONE（慢，真机前使用）
# 断言失败返回非 0；日志保留在 <iso目录>/boot-test-<arch>/
# ============================================================
set -u
ARCHARG="${1:-}"
ISO="${2:-}"
TIMEOUT="${3:-900}"
MODE="${4:-boot}"
case "$ARCHARG" in
    x86_64|amd64)  ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
    *) echo "用法: $0 <x86_64|aarch64> <iso> [timeout] [boot|full]" >&2; exit 2 ;;
esac
ISO=$(readlink -f "$ISO" 2>/dev/null || true)
[ -n "$ISO" ] && [ -f "$ISO" ] || { echo "ISO 不存在: ${2:-}" >&2; exit 2; }
OUT="${OUT_DIR:-$(dirname "$ISO")/boot-test-$ARCH}"
mkdir -p "$OUT"
LOG="$OUT/serial.log"
DISK="$OUT/disk.qcow2"
: > "$LOG"
qemu-img create -f qcow2 "$DISK" 12G >/dev/null || exit 2
echo "==> 启动测试 arch=$ARCH mode=$MODE timeout=${TIMEOUT}s"
echo "    ISO: $ISO"
echo "    日志: $LOG"

FAIL=0

# ---------- ISO 内容检查 ----------
echo "==> ISO 内容检查（vmlinuz/initrd/filesystem.squashfs）"
if command -v xorriso >/dev/null 2>&1; then
    xorriso -indev "$ISO" -find /live -exec lsdl 2>/dev/null | tee "$OUT/iso-live.txt" || true
    if xorriso -indev "$ISO" -find /live/filesystem.squashfs -exec lsdl 2>/dev/null | grep -q filesystem.squashfs; then
        echo "PASS: ISO 内 /live/filesystem.squashfs 存在"
    else
        echo "FAIL: ISO 内缺少 /live/filesystem.squashfs（构建问题）"; FAIL=1
    fi
else
    echo "（未安装 xorriso，跳过 ISO 内容检查）"
fi

# ---------- initrd 组件检查（live-boot 依赖的模块/工具） ----------
echo "==> initrd 组件检查"
if command -v xorriso >/dev/null 2>&1 && command -v unmkinitramfs >/dev/null 2>&1; then
    xorriso -osirrox on -indev "$ISO" -extract /live/initrd.img "$OUT/initrd.img" 2>/dev/null || true
    rm -rf "$OUT/ird" && mkdir -p "$OUT/ird"
    unmkinitramfs "$OUT/initrd.img" "$OUT/ird" >/dev/null 2>&1 || true
    {
        echo "=== live-boot 脚本 ==="
        find "$OUT/ird" -name live-boot 2>/dev/null | head -3
        echo "=== 关键工具 ==="
        for f in blkid udevadm cdrom_id; do
            echo "-- $f --"; find "$OUT/ird" -name "$f" 2>/dev/null | head -2
        done
        echo "=== 关键内核模块 ==="
        for m in squashfs sr_mod sd_mod virtio_blk usb-storage isofs overlay; do
            n=$(find "$OUT/ird" -name "*${m}.ko*" 2>/dev/null | wc -l)
            echo "$m = $n"
        done
        echo "=== 内核版本 ==="
        ls "$OUT/ird/main/lib/modules" 2>/dev/null || true
    } | tee "$OUT/initrd-check.txt"
    for m in squashfs sr_mod sd_mod; do
        if ! grep -q "^$m = [1-9]" "$OUT/initrd-check.txt"; then
            echo "WARN: initrd 缺少模块 $m（live-boot/安装器可能失败）"
        fi
    done
else
    echo "（缺少 xorriso 或 unmkinitramfs，跳过 initrd 检查）"
fi

# ---------- 启动 QEMU（后台 + 轮询，命中关键标记提前结束） ----------
echo "==> 启动 QEMU（arch=$ARCH）"
START=$(date +%s)
if [ "$ARCH" = "aarch64" ]; then
    FW=/usr/share/qemu-efi-aarch64/QEMU_EFI.fd
    [ -f "$FW" ] || { echo "缺少 $FW（apt install qemu-efi-aarch64）" >&2; exit 3; }
    timeout "$TIMEOUT" qemu-system-aarch64 -M virt,virtualization=true -cpu cortex-a76 -m 3072 -smp 2 \
        -bios "$FW" \
        -device nec-usb-xhci,id=xhci \
        -drive if=none,id=cd,format=raw,media=cdrom,file="$ISO" \
        -device usb-storage,bus=xhci.0,drive=cd \
        -drive file="$DISK",if=none,id=disk,format=qcow2 \
        -device virtio-blk-pci,drive=disk \
        -netdev user,id=n0 -device e1000,netdev=n0 \
        -device VGA -display none -monitor "unix:$OUT/mon.sock,server=on,wait=off" -no-reboot \
        -serial "file:$LOG" >"$OUT/qemu.err" 2>&1 &
    QPID=$!
else
    OVMF_CODE=$(ls /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd 2>/dev/null | head -1)
    OVMF_VARS=$(ls /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd 2>/dev/null | head -1)
    if [ -z "$OVMF_CODE" ] || [ -z "$OVMF_VARS" ]; then
        echo "缺少 OVMF（apt install ovmf）" >&2; exit 3
    fi
    cp -f "$OVMF_VARS" "$OUT/vars.fd"
    ACCEL=""
    if [ -w /dev/kvm ]; then ACCEL="-accel kvm"; fi
    timeout "$TIMEOUT" qemu-system-x86_64 -machine q35 -m 2048 -smp 2 $ACCEL \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file="$OUT/vars.fd" \
        -cdrom "$ISO" \
        -drive file="$DISK",if=virtio,format=qcow2 \
        -netdev user,id=n0 -device e1000,netdev=n0 \
        -display none -monitor "unix:$OUT/mon.sock,server=on,wait=off" -no-reboot \
        -serial "file:$LOG" >"$OUT/qemu.err" 2>&1 &
    QPID=$!
fi

# 后台定时截屏（VNC/VGA 画面；用于确认 tty0 上到底显示了什么）
capture_screens() {
    local n=0
    while [ "$n" -lt 24 ]; do
        sleep 15
        n=$((n + 1))
        python3 - "$OUT/mon.sock" "$OUT/screen-$n.ppm" <<'PYEOF' >/dev/null 2>&1 || true
import socket, sys, time
try:
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(5)
    s.connect(sys.argv[1])
    try: s.recv(4096)
    except Exception: pass
    s.sendall(("screendump " + sys.argv[2] + "\n").encode())
    time.sleep(0.5)
    try: s.recv(4096)
    except Exception: pass
    s.close()
except Exception:
    pass
PYEOF
        if command -v pnmtopng >/dev/null 2>&1 && [ -f "$OUT/screen-$n.ppm" ]; then
            pnmtopng "$OUT/screen-$n.ppm" > "$OUT/screen-$n.png" 2>/dev/null || true
            rm -f "$OUT/screen-$n.ppm"
        fi
    done
}
capture_screens &
CAPPID=$!

RESULT="timeout"
i=0
while [ "$i" -lt "$TIMEOUT" ]; do
    i=$((i + 1))
    if ! kill -0 "$QPID" 2>/dev/null; then RESULT="exited"; break; fi
    if [ "$MODE" = "full" ]; then
        if grep -aq "LINGOS-INSTALL-DONE" "$LOG"; then RESULT="install-done"; break; fi
    else
        if grep -aq "LINGOS-STAGE=2/10" "$LOG"; then RESULT="installer-started"; sleep 3; break; fi
    fi
    if grep -aq "Unable to find a medium\|BOOT FAILED\|Kernel panic\|not syncing" "$LOG"; then
        RESULT="boot-failed"; break
    fi
    sleep 1
done
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
kill "$CAPPID" 2>/dev/null || true
ELAPSED=$(( $(date +%s) - START ))
echo "qemu_result=$RESULT elapsed=${ELAPSED}s"
echo "=== serial.log 末尾 ==="
tail -80 "$LOG" 2>/dev/null || true

# ---------- 断言 ----------
need() {
    if grep -aq "$1" "$LOG" 2>/dev/null; then echo "PASS: $2"; else echo "FAIL: $2"; FAIL=1; fi
}
need "LINGOS-BOOT-MODE=" "内核启动 + lingos-boot-mode 执行"
need "LINGOS-STAGE=1/10" "安装器进入 preflight"
if [ "$MODE" = "full" ]; then
    need "LINGOS-INSTALL-DONE" "安装完成（INSTALL-DONE）"
else
    need "LINGOS-STAGE=2/10" "进入磁盘选择"
fi
DISK_SZ=$(stat -c%s "$DISK" 2>/dev/null || echo 0)
echo "disk.qcow2=$DISK_SZ bytes"
if [ "$DISK_SZ" -gt 1048576 ]; then
    echo "PASS: 磁盘已写入（安装器确实执行）"
else
    echo "FAIL: 磁盘未写入（安装器未执行或未分区）"
    FAIL=1
fi
echo "=== 关键日志行 ==="
grep -aE "LINGOS-|BOOT FAILED|Unable to find|Kernel panic|not syncing|EFI stub|Booting Linux" "$LOG" 2>/dev/null | head -100 || true
cp -f "$LOG" "$OUT/final.log" 2>/dev/null || true
if [ "$FAIL" != "0" ]; then
    echo "==> 启动冒烟测试失败"
    exit 1
fi
echo "==> 启动冒烟测试通过"
exit 0