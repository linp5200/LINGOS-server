#!/usr/bin/env bash
# ============================================================
# LING OS 安装器（在 live ISO 内运行）
# 用法：
#   lingos-install            交互（whiptail 选盘 + 确认）
#   lingos-install --auto     自动：LINGOS_DISK / cmdline lingos.disk=/dev/sdX 优先；
#                             否则自动选「最大且 >=8G 的内置磁盘」（忽略 USB）
# 流程：GPT → EFI(512M)+root(ext4,余量) → rsync live 根 → GRUB → 首启配置 → 重启
# ============================================================
set -euo pipefail

LOG=/var/log/lingos-install.log
mkdir -p /var/log
exec > >(tee -a "$LOG") 2>&1

AUTO=0
if [ "$1" = "--auto" ] || [ "$1" = "auto" ]; then AUTO=1; fi
echo "==> LING OS 安装器  auto=$AUTO  时间=$(date)"
if [ "$(id -u)" != "0" ]; then echo "需要 root" >&2; exit 1; fi

# ---------- 选择磁盘 ----------
DISK=$(printenv LINGOS_DISK || true)
if [ -z "$DISK" ]; then
    for a in $(cat /proc/cmdline); do
        case "$a" in
            lingos.disk=*) DISK=$(echo "$a" | cut -d= -f2-) ;;
        esac
    done
fi

# 候选：内置磁盘（排除 USB），格式 "设备:字节数"
CANDIDATES=""
for row in $(lsblk -bdn -o NAME,TYPE,TRAN,SIZE 2>/dev/null | awk '$2=="disk" && $3!="usb" {print $1":"$4}'); do
    CANDIDATES="$CANDIDATES $row"
done
if [ -z "$CANDIDATES" ]; then echo "未发现可安装磁盘（USB 已排除）" >&2; exit 1; fi

pick_largest() {
    BEST=""; BESTSIZE=0
    for row in $CANDIDATES; do
        DEV=$(echo "$row" | cut -d: -f1)
        SZ=$(echo "$row" | cut -d: -f2)
        if [ "$SZ" -ge 8589934592 ] && [ "$SZ" -gt "$BESTSIZE" ]; then
            BEST="/dev/$DEV"; BESTSIZE=$SZ
        fi
    done
    echo "$BEST"
}

if [ -z "$DISK" ]; then
    if [ "$AUTO" = "1" ]; then
        DISK=$(pick_largest)
        if [ -z "$DISK" ]; then
            echo "自动安装需要指定磁盘：lingos.disk=/dev/xxx（候选：$CANDIDATES）" >&2
            exit 1
        fi
        echo "自动模式：选择最大可安装磁盘 $DISK"
    else
        MENU=""
        for row in $CANDIDATES; do
            DEV=$(echo "$row" | cut -d: -f1)
            SZ=$(echo "$row" | cut -d: -f2)
            MENU="$MENU /dev/$DEV $(basename "/dev/$DEV") $SZ"
        done
        DISK=$(whiptail --title "LING OS 安装器" --menu "选择要安装到的磁盘（将被完全清空）" 20 70 10 $MENU 3>&1 1>&2 2>&3) || exit 1
    fi
fi
echo "目标磁盘：$DISK"

if [ "$AUTO" != "1" ]; then
    whiptail --title "确认" --yesno "将清空 $DISK 并安装 LING OS，继续？" 10 60 || exit 1
fi
if [ ! -b "$DISK" ]; then echo "不是块设备：$DISK" >&2; exit 1; fi

ROOTSRC=$(findmnt -n -o SOURCE / 2>/dev/null || true)
case "$ROOTSRC" in
    "$DISK"|"$DISK"p[0-9]*|"$DISK"[0-9]*)
        echo "拒绝：安装目标包含当前运行根文件系统（$DISK）" >&2; exit 1 ;;
esac

# ---------- 分区 ----------
UEFI=0
[ -d /sys/firmware/efi ] && UEFI=1
echo "UEFI=$UEFI"
case "$DISK" in
    *nvme*|*mmcblk*) P1="$DISK"p1; P2="$DISK"p2 ;;
    *) P1="$DISK"1; P2="$DISK"2 ;;
esac
parted -s "$DISK" mklabel gpt
if [ "$UEFI" = "1" ]; then
    parted -s "$DISK" mkpart EFI fat32 1MiB 513MiB
    parted -s "$DISK" set 1 esp on
    parted -s "$DISK" mkpart LINGOS ext4 513MiB 100%
    mkfs.vfat -F32 -n LINGOS_EFI "$P1"
else
    parted -s "$DISK" mkpart BIOSBOOT 1MiB 3MiB
    parted -s "$DISK" set 1 bios_grub on
    parted -s "$DISK" mkpart LINGOS ext4 3MiB 100%
fi
mkfs.ext4 -F -L LINGOS_ROOT "$P2"

# ---------- 挂载并复制 live 根 ----------
TARGET=/target
mkdir -p "$TARGET"
mount "$P2" "$TARGET"
mkdir -p "$TARGET/boot/efi"
if [ "$UEFI" = "1" ]; then mount "$P1" "$TARGET/boot/efi"; fi
echo "==> 复制系统文件（rsync）"
rsync -aHAX --one-file-system \
  --exclude=/proc --exclude=/sys --exclude=/dev --exclude=/run \
  --exclude=/tmp --exclude=/mnt --exclude=/media --exclude=/live \
  --exclude=/cdrom --exclude=/var/cache/apt --exclude=/var/log/lingos-install.log \
  / "$TARGET/"

# ---------- fstab / 主机名 / 首启配置 ----------
ROOT_UUID=$(blkid -s UUID -o value "$P2")
echo "UUID=$ROOT_UUID  /  ext4  errors=remount-ro  0  1" > "$TARGET/etc/fstab"
if [ "$UEFI" = "1" ]; then
    EFI_UUID=$(blkid -s UUID -o value "$P1")
    echo "UUID=$EFI_UUID  /boot/efi  vfat  umask=0077  0  1" >> "$TARGET/etc/fstab"
fi
echo lingos > "$TARGET/etc/hostname"
mkdir -p "$TARGET/LINGOS/system/config" "$TARGET/LINGOS/data" "$TARGET/LINGOS/log" "$TARGET/LINGOS/run" "$TARGET/LINGOS/Debug/log"
printf '%s\n' '{"system_configured": true, "last_config_time": "1970-01-01T00:00:00Z"}' > "$TARGET/LINGOS/system/config/state.json"
printf '%s\n' '{"schema_version": 1, "ai": {"ai_config": {"backend": "deepseek", "language": "zh"}, "provider": {"active": "deepseek", "deepseek": {"api_key": "", "model": "deepseek-v4-pro", "base_url": "https://api.deepseek.com"}}}}' > "$TARGET/LINGOS/system/config/config.json"

# ---------- GRUB ----------
mkdir -p "$TARGET/proc" "$TARGET/sys" "$TARGET/dev"
mount --bind /proc "$TARGET/proc"
mount --bind /sys  "$TARGET/sys"
mount --bind /dev  "$TARGET/dev"
if [ "$UEFI" = "1" ]; then
    chroot "$TARGET" grub-install --target="$GRUBTARGET" --efi-directory=/boot/efi --bootloader-id=LINGOS --removable --no-nvram
else
    chroot "$TARGET" grub-install --target=i386-pc "$DISK"
fi
chroot "$TARGET" update-grub
umount "$TARGET/dev" "$TARGET/sys" "$TARGET/proc" 2>/dev/null || true

# ---------- 首启服务 ----------
mkdir -p "$TARGET/etc/systemd/system/multi-user.target.wants"
ln -sf /etc/systemd/system/lingos.service "$TARGET/etc/systemd/system/multi-user.target.wants/lingos.service"
rm -f "$TARGET/etc/systemd/system/multi-user.target.wants/lingos-installer.service"
chroot "$TARGET" passwd -d root 2>/dev/null || true

sync
echo "==> 安装完成，卸载并重启"
umount "$TARGET/boot/efi" 2>/dev/null || true
umount "$TARGET" 2>/dev/null || true
if [ "$AUTO" = "1" ]; then
    echo "自动安装完成，5 秒后重启"
    sleep 5
    reboot -f
else
    whiptail --title "安装完成" --msgbox "LING OS 已安装到 $DISK，重启后生效。" 10 60 || true
fi
