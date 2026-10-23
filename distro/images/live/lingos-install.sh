#!/bin/bash
# ============================================================
# LING OS 安装器（live ISO 内运行；0.9.12 带阶段日志/进度/回传）
# 用法：
#   lingos-install --auto        自动：选最大 >=8G 内置磁盘（排除 USB）
#   lingos-install --manual      交互：whiptail 选盘 + 确认
# 指定磁盘：内核参数 lingos.disk=/dev/sdX 或环境变量 LINGOS_DISK
# 流程：GPT -> EFI(512M)+root(ext4) -> rsync live 根 -> GRUB -> 首启配置 -> 重启
# ============================================================
set -Eeuo pipefail

export LINGOS_MODE="${LINGOS_MODE:-install-auto}"
export LINGOS_LOG="${LINGOS_LOG:-/var/log/lingos-install.log}"
export LINGOS_TOTAL_STAGES=10
LINGOS_LIB=/usr/local/sbin/lingos-log.sh
if [ -f "$LINGOS_LIB" ]; then . "$LINGOS_LIB"; else echo "缺少 $LINGOS_LIB" >&2; exit 1; fi
: > "$LINGOS_LOG" 2>/dev/null || true

trap 'rc=$?; set +e; fail_msg "$rc" "命令失败: $BASH_COMMAND"; exit $rc' ERR

AUTO=0
case "$LINGOS_MODE" in
    install-auto|auto) AUTO=1 ;;
    install-manual|manual|install) AUTO=0 ;;
esac
for arg in "$@"; do
    case "$arg" in
        --auto) AUTO=1 ;;
        --manual) AUTO=0 ;;
    esac
done
export TERM="${TERM:-linux}"

boot_info
stage 1 preflight "环境检查"
[ "$(id -u)" = "0" ] || { fail_msg 3 "需要 root"; exit 3; }
[ -d /run/live ] || { fail_msg 4 "未检测到 live 环境（/run/live 不存在）"; exit 4; }
log "live 环境检查通过（mode=$LINGOS_MODE auto=$AUTO）"

# ---------- 磁盘候选（排除 USB） ----------
udevadm settle 2>/dev/null || true
CANDIDATES=""
LIVE_SRC=$(findmnt -n -o SOURCE /run/live/medium 2>/dev/null || true)
while IFS= read -r row; do
    eval "$row"
    [ "${TYPE:-}" = "disk" ] || continue
    [ -b "/dev/$NAME" ] || continue
    [ "${TRAN:-}" = "usb" ] && { log "跳过 USB 磁盘 /dev/$NAME"; continue; }
    [ -n "${SIZE:-}" ] || continue
    case "$LIVE_SRC" in "/dev/$NAME"*) log "跳过 live 介质 /dev/$NAME"; continue ;; esac
    CANDIDATES="$CANDIDATES /dev/$NAME:$SIZE"
done < <(lsblk -bdnP -o NAME,TYPE,TRAN,SIZE 2>/dev/null)
[ -n "$CANDIDATES" ] || { fail_msg 5 "未发现可安装磁盘（USB 已排除）"; exit 5; }
log "可安装磁盘:$CANDIDATES"

# ---------- 选择磁盘 ----------
stage 2 select-disk "选择安装磁盘"
DISK="${LINGOS_DISK:-}"
if [ -z "$DISK" ]; then
    DISK=$(read_cmdline_param lingos.disk || true)
fi
if [ -z "$DISK" ] && [ "$AUTO" = "1" ]; then
    BEST=""; BESTSIZE=0
    for row in $CANDIDATES; do
        DEV="${row%%:*}"; SZ="${row##*:}"
        if [ "$SZ" -ge 8589934592 ] && [ "$SZ" -gt "$BESTSIZE" ]; then
            BEST="$DEV"; BESTSIZE="$SZ"
        fi
    done
    if [ -n "$BEST" ]; then
        DISK="$BEST"; log "自动模式选择磁盘 $DISK（$BESTSIZE 字节）"
    else
        fail_msg 6 "自动安装需要 >=8GB 磁盘（候选:$CANDIDATES）"; exit 6
    fi
elif [ -z "$DISK" ]; then
    MENU=""
    for row in $CANDIDATES; do
        DEV="${row%%:*}"; SZ="${row##*:}"
        MENU="$MENU $DEV $(basename "$DEV") $SZ"
    done
    DISK=$(whiptail --title "LING OS 安装器" --menu "选择要安装到的磁盘（将被完全清空）" 20 70 10 $MENU 3>&1 1>&2 2>&3)         || { fail_msg 7 "用户取消磁盘选择"; exit 7; }
    log "用户选择磁盘 $DISK"
else
    log "使用指定磁盘 $DISK"
fi

[ -b "$DISK" ] || { fail_msg 8 "不是块设备: $DISK"; exit 8; }
ROOTSRC=$(findmnt -n -o SOURCE / 2>/dev/null || true)
case "$ROOTSRC" in
    "$DISK"|"$DISK"p[0-9]*|"$DISK"[0-9]*) fail_msg 9 "拒绝：安装目标包含当前 live 根（$ROOTSRC）"; exit 9 ;;
esac

# ---------- 确认 ----------
stage 3 confirm "确认安装"
if [ "$AUTO" != "1" ]; then
    whiptail --title "确认" --yesno "将清空 $DISK 并安装 LING OS，继续？" 10 60         || { fail_msg 10 "用户取消安装"; exit 10; }
fi

# ---------- 分区 ----------
stage 4 partition "分区（GPT: EFI 512M + root ext4）"
UEFI=0; [ -d /sys/firmware/efi ] && UEFI=1
case "$DISK" in
    *nvme*|*mmcblk*) P1="${DISK}p1"; P2="${DISK}p2" ;;
    *)               P1="${DISK}1";  P2="${DISK}2" ;;
esac
parted -s "$DISK" mklabel gpt
if [ "$UEFI" = "1" ]; then
    parted -s "$DISK" mkpart EFI fat32 1MiB 513MiB
    parted -s "$DISK" set 1 esp on
    parted -s "$DISK" mkpart LINGOS ext4 513MiB 100%
else
    parted -s "$DISK" mkpart BIOSBOOT 1MiB 3MiB
    parted -s "$DISK" set 1 bios_grub on
    parted -s "$DISK" mkpart LINGOS ext4 3MiB 100%
fi
partprobe "$DISK" 2>/dev/null || true
udevadm settle 2>/dev/null || true
sleep 1
log "分区完成: $P1 / $P2（UEFI=$UEFI）"

# ---------- 格式化 ----------
stage 5 format "格式化文件系统"
if [ "$UEFI" = "1" ]; then
    mkfs.vfat -F32 -n LINGOS_EFI "$P1" >/dev/null
fi
mkfs.ext4 -F -L LINGOS_ROOT "$P2" >/dev/null
log "格式化完成"

# ---------- 挂载 ----------
stage 6 mount "挂载目标磁盘"
TARGET=/target
mkdir -p "$TARGET"
mount "$P2" "$TARGET"
if [ "$UEFI" = "1" ]; then mkdir -p "$TARGET/boot/efi"; mount "$P1" "$TARGET/boot/efi"; fi
log "已挂载 $P2 -> $TARGET"

# ---------- 复制 live 根 ----------
stage 7 copy "复制系统文件"
RSYNC_START=$(_now)
set +e
rsync -aHAX --one-file-system --info=progress2 --no-inc-recursive \
    --exclude=/proc --exclude=/sys --exclude=/dev --exclude=/run --exclude=/tmp --exclude=/mnt \
    --exclude=/media --exclude=/live --exclude=/cdrom --exclude=/var/cache/apt \
    --exclude=/var/log/lingos-install.log --exclude=/run/lingos-install \
    / "$TARGET/" 2>&1 | tr '\r' '\n' | while IFS= read -r line; do
        [ -n "$line" ] || continue
        printf '%s\n' "$line" >> "$LINGOS_LOG" 2>/dev/null || true
        if [ "${line#*%}" != "$line" ]; then
            pct=$(printf '%s\n' "$line" | grep -oE '[0-9]{1,3}%' | tail -1 | tr -d '%' || true)
            if [ -n "${pct:-}" ]; then
                LINGOS_STAGE=7
                write_state "copy" "$(( 60 + pct * 25 / 100 ))" "复制系统文件 ${pct}%"
            fi
        fi
    done
rc=${PIPESTATUS[0]:-$?}
set -e
if [ "$rc" != "0" ]; then fail_msg 11 "rsync 复制失败（rc=$rc）"; exit 11; fi
log "系统文件复制完成（$(( $(_now) - RSYNC_START )) 秒）"

# ---------- GRUB ----------
stage 8 bootloader "安装 GRUB 引导"
mkdir -p "$TARGET/proc" "$TARGET/sys" "$TARGET/dev"
mount --bind /proc "$TARGET/proc" 2>/dev/null || true
mount --bind /sys  "$TARGET/sys"  2>/dev/null || true
mount --bind /dev  "$TARGET/dev"  2>/dev/null || true
GRUBTARGET=""
case "$(uname -m)" in
    x86_64|amd64)  GRUBTARGET=x86_64-efi ;;
    aarch64|arm64) GRUBTARGET=arm64-efi ;;
esac
if [ "$UEFI" = "1" ]; then
    chroot "$TARGET" grub-install --target="$GRUBTARGET" --efi-directory=/boot/efi \
        --bootloader-id=LINGOS --removable --no-nvram
else
    chroot "$TARGET" grub-install --target=i386-pc "$DISK"
fi
chroot "$TARGET" update-grub
umount "$TARGET/dev" "$TARGET/sys" "$TARGET/proc" 2>/dev/null || true
log "GRUB 安装完成（target=$GRUBTARGET uefi=$UEFI）"

# ---------- 配置 ----------
stage 9 configure "写入配置与首启服务"
ROOT_UUID=$(blkid -s UUID -o value "$P2")
{
    echo "UUID=$ROOT_UUID  /  ext4  errors=remount-ro  0  1"
    if [ "$UEFI" = "1" ]; then
        EFI_UUID=$(blkid -s UUID -o value "$P1")
        echo "UUID=$EFI_UUID  /boot/efi  vfat  umask=0077  0  1"
    fi
} > "$TARGET/etc/fstab"
echo lingos > "$TARGET/etc/hostname"
mkdir -p "$TARGET/LINGOS/system/config" "$TARGET/LINGOS/data" "$TARGET/LINGOS/log" \
         "$TARGET/LINGOS/run" "$TARGET/LINGOS/Debug/log"
printf '%s\n' '{"system_configured": true, "last_config_time": "1970-01-01T00:00:00Z"}' \
    > "$TARGET/LINGOS/system/config/state.json"
printf '%s\n' '{"schema_version": 1, "ai": {"ai_config": {"backend": "deepseek", "language": "zh"}, "provider": {"active": "deepseek", "deepseek": {"api_key": "", "model": "deepseek-v4-pro", "base_url": "https://api.deepseek.com"}}}}' \
    > "$TARGET/LINGOS/system/config/config.json"
cat > "$TARGET/LINGOS/system/config/install-report.json" <<REPORT
{"version":"0.9.12","mode":"$LINGOS_MODE","disk":"$DISK","uefi":$UEFI,"root_uuid":"$ROOT_UUID","installed_at":"$(date -u +%FT%TZ)"}
REPORT
mkdir -p "$TARGET/var/log"
cp -f "$LINGOS_LOG" "$TARGET/var/log/lingos-install.log" 2>/dev/null || true
cp -f "$LINGOS_STATE" "$TARGET/LINGOS/system/config/install-state.json" 2>/dev/null || true
mkdir -p "$TARGET/etc/systemd/system/multi-user.target.wants"
ln -sf /etc/systemd/system/lingos.service "$TARGET/etc/systemd/system/multi-user.target.wants/lingos.service"
rm -f "$TARGET/etc/systemd/system/multi-user.target.wants/lingos-installer.service"
chroot "$TARGET" passwd -d root 2>/dev/null || true
log "配置写入完成"

# ---------- 完成 ----------
stage 10 finish "完成，准备重启"
sync
umount "$TARGET/boot/efi" 2>/dev/null || true
umount "$TARGET" 2>/dev/null || true
mark_done
if [ "$AUTO" = "1" ]; then
    log "自动安装完成，8 秒后重启"
    for i in 8 7 6 5 4 3 2 1; do
        _emit "LINGOS-INSTALL-REBOOT-IN=$i"
        sleep 1
    done
    reboot -f
else
    whiptail --title "安装完成" --msgbox "LING OS 已安装到 $DISK，重启后生效。" 10 60 || true
fi
