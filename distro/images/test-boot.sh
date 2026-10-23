#!/usr/bin/env bash
# ============================================================
# LING OS ISO 启动冒烟测试（CI / 本地，UEFI + 串口日志）
# 用法：test-boot.sh <x86_64|aarch64> <iso> [超时秒=900] [boot|full]
#   boot —— 只验证引导链 + 安装器开始写盘（快，CI 默认）
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
echo "==> ISO 内容检查（vmlinuz/initrd/filesystem.squashfs）"
if command -v xorriso >/dev/null 2>&1; then
    xorriso -indev "$ISO" -find /live -exec lsdl 2>/dev/null | tee "$OUT/iso-live.txt" || true
    if xorriso -indev "$ISO" -find /live/filesystem.squashfs -exec lsdl 2>/dev/null | grep -q filesystem.squashfs; then
        echo "PASS: ISO 内 /live/filesystem.squashfs 存在"
    else
        echo "FAIL: ISO 内缺少 /live/filesystem.squashfs（构建问题）"
    fi
else
    echo "（未安装 xorriso，跳过 ISO 内容检查）"
fi
echo "==> 启动测试 arch=$ARCH mode=$MODE timeout=${TIMEOUT}s"
echo "    ISO: $ISO"
echo "    日志: $LOG"
START=$(date +%s)
set +e
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
        -device virtio-gpu-pci -display none -monitor none -no-reboot \
        -serial "file:$LOG" >"$OUT/qemu.err" 2>&1
    RC=$?
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
        -display none -monitor none -no-reboot \
        -serial "file:$LOG" >"$OUT/qemu.err" 2>&1
    RC=$?
fi
set -e
ELAPSED=$(( $(date +%s) - START ))
echo "qemu_rc=$RC elapsed=${ELAPSED}s"
echo "=== serial.log 末尾 ==="
tail -60 "$LOG" 2>/dev/null || true

FAIL=0
need() {
    if grep -aq "$1" "$LOG" 2>/dev/null; then echo "PASS: $2"; else echo "FAIL: $2"; FAIL=1; fi
}
need "LINGOS-BOOT-MODE=" "内核启动 + lingos-boot-mode 执行"
need "LINGOS-STAGE=1/10" "安装器进入 preflight"
need "LINGOS-STAGE=2/10" "进入磁盘选择"
if [ "$MODE" = "full" ]; then
    need "LINGOS-INSTALL-DONE" "安装完成（INSTALL-DONE）"
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
grep -aE "LINGOS-|live-boot|live-config|Kernel panic|Unable to find|not syncing|EFI stub|Booting Linux" "$LOG" 2>/dev/null | head -80 || true
cp -f "$LOG" "$OUT/final.log" 2>/dev/null || true
if [ "$FAIL" != "0" ]; then
    echo "==> 启动冒烟测试失败"
    exit 1
fi
echo "==> 启动冒烟测试通过"
exit 0