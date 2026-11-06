#!/bin/bash
# ============================================================
# LING OS 安装器（live ISO 内运行；0.10.3：全英文控制台 + LightDM OOBE）
# 用法：
#   lingos-install --auto        自动：选最大 >=8G 内置磁盘（排除 USB）
#   lingos-install --manual      交互：whiptail 选盘 + 确认
# 指定磁盘：内核参数 lingos.disk=/dev/sdX 或环境变量 LINGOS_DISK
# 流程：GPT -> EFI(512M)+root(ext4) -> rsync live 根 -> GRUB -> 首启配置 -> 重启
#
# 约定：图形 OOBE 之前的控制台输出一律 ASCII（Linux console 无 CJK 字体）。
# 注释可保留中文；中文只出现在图形 OOBE（0.10.3）里。
# ============================================================
set -Eeuo pipefail

# 最早期调试标记（即使后续 source/依赖失败也能在串口看到）
for _c in /dev/ttyAMA0 /dev/ttyS0; do
    [ -e "$_c" ] && printf 'LINGOS-INSTALL-SCRIPT-START\n' > "$_c" 2>/dev/null || true
done

export LINGOS_MODE="${LINGOS_MODE:-install-auto}"
export LINGOS_LOG="${LINGOS_LOG:-/var/log/lingos-install.log}"
export LINGOS_TOTAL_STAGES=10
LINGOS_VERSION=$(sed -n 's/^LINGOS_VERSION=//p' /etc/lingos-image-info 2>/dev/null | head -1)
export LINGOS_VERSION="${LINGOS_VERSION:-unknown}"
ENCRYPT=0
LUKS_PASS=""
CRYPT_NAME="lingos_crypt"
ROOT_DEV=""
LUKS_UUID=""
LINGOS_LIB=/usr/local/sbin/lingos-log.sh
if [ -f "$LINGOS_LIB" ]; then . "$LINGOS_LIB"; else echo "ERROR: missing $LINGOS_LIB" >&2; exit 1; fi
for _c in /dev/ttyAMA0 /dev/ttyS0; do [ -e "$_c" ] && printf 'LINGOS-LIB-OK\n' > "$_c" 2>/dev/null || true; done
: > "$LINGOS_LOG" 2>/dev/null || true
for _c in /dev/ttyAMA0 /dev/ttyS0; do [ -e "$_c" ] && printf 'LINGOS-LOG-TRUNCATE-OK\n' > "$_c" 2>/dev/null || true; done

# ---------- 控制台辅助（全英文/ASCII） ----------
# 时长格式化：45s / 12m / 1h05m
fmt_duration() {
    local s="${1:-0}"
    case "$s" in ''|*[!0-9]*) s=0 ;; esac
    if [ "$s" -ge 3600 ]; then
        printf '%dh%02dm' $(( s / 3600 )) $(( (s % 3600) / 60 ))
    elif [ "$s" -ge 60 ]; then
        printf '%dm' $(( s / 60 ))
    else
        printf '%ds' "$s"
    fi
}

# 字节格式化：3.1 GiB / 512 MiB / 4 KiB / 12 B（纯整数运算，不依赖 bc/awk）
human_bytes() {
    local b="${1:-0}"
    case "$b" in ''|*[!0-9]*) b=0 ;; esac
    if [ "$b" -ge 1073741824 ]; then
        printf '%d.%d GiB' $(( b / 1073741824 )) $(( (b % 1073741824) * 10 / 1073741824 ))
    elif [ "$b" -ge 1048576 ]; then
        printf '%d MiB' $(( b / 1048576 ))
    elif [ "$b" -ge 1024 ]; then
        printf '%d KiB' $(( b / 1024 ))
    else
        printf '%d B' "$b"
    fi
}

# 失败时提供调试 shell（AUTO=1 自动安装不阻塞；LINGOS_DEBUG_SHELL=0 可关闭）
debug_shell_hint() {
    local code="${1:-1}" msg="${2:-Unknown error}" ans="" wait_s="${LINGOS_DEBUG_SHELL_TIMEOUT:-60}"
    [ "${LINGOS_DEBUG_SHELL:-1}" = "1" ] || return 0
    [ "${AUTO:-0}" = "1" ] && return 0
    [ -c /dev/tty1 ] || return 0
    {
        printf '\n'
        printf 'INSTALL FAILED (code=%s): %s\n' "$code" "$msg"
        printf 'Log:   %s\n' "${LINGOS_LOG:-/var/log/lingos-install.log}"
        printf 'State: %s\n' "${LINGOS_STATE:-/run/lingos-install/state.json}"
        printf 'Type "s" + Enter within %ss for a debug shell (otherwise the installer exits).\n' "$wait_s"
    } > /dev/tty1 2>/dev/null || true
    read -r -t "$wait_s" ans < /dev/tty1 2>/dev/null || ans=""
    case "$ans" in
        s|S|shell|debug)
            printf 'Debug shell. Type "exit" to leave the installer.\n' > /dev/tty1 2>/dev/null || true
            bash -i < /dev/tty1 > /dev/tty1 2>&1 || true
            ;;
    esac
    return 0
}

# lingos-log.sh 的 fail_msg 会打印中文“失败(...)”；这里覆盖为英文实现，
# 固定标记 / state JSON / 共享文件夹 / 网络回传的格式保持完全一致。
fail_msg() {
    local code="${1:-1}" msg="${2:-Unknown error}" json
    _emit "LINGOS-INSTALL-FAIL=code=$code $msg"
    _emit "[$(_stamp)] [FAIL] code=$code: $msg"
    json=$(printf '{"ok":false,"mode":"%s","code":%s,"message":"%s","stage":%s,"ts":"%s"}' \
        "$(json_escape "$LINGOS_MODE")" "$code" "$(json_escape "$msg")" "$LINGOS_STAGE" "$(date -u +%FT%TZ)")
    printf '%s\n' "$json" > "$LINGOS_STATE.tmp" 2>/dev/null && mv "$LINGOS_STATE.tmp" "$LINGOS_STATE" 2>/dev/null || true
    report_share "failed" "0" "$msg"
    report_net "LINGOS-INSTALL-FAIL code=$code msg=$msg"
    debug_shell_hint "$code" "$msg"
    return 0
}

trap 'rc=$?; for _c in /dev/ttyAMA0 /dev/ttyS0; do [ -e "$_c" ] && printf "LINGOS-INSTALL-ERROR line=%s rc=%s cmd=%s\n" "$LINENO" "$rc" "$BASH_COMMAND" > "$_c" 2>/dev/null || true; done; set +e; fail_msg "$rc" "Command failed: $BASH_COMMAND"; exit $rc' ERR

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
for arg in $(cat /proc/cmdline 2>/dev/null || true); do
    case "$arg" in
        lingos.encrypt=1) ENCRYPT=1 ;;
    esac
done
if [ -n "${LINGOS_LUKS_PASSPHRASE:-}" ]; then
    ENCRYPT=1
    LUKS_PASS="$LINGOS_LUKS_PASSPHRASE"
fi
export TERM="${TERM:-linux}"

# boot_info 的中文行由本脚本用等价的英文行代替（固定标记保持不变）
for _c in /dev/ttyAMA0 /dev/ttyS0; do [ -e "$_c" ] && printf 'LINGOS-BOOTINFO-BEGIN\n' > "$_c" 2>/dev/null || true; done
_emit "LINGOS-BOOT-MODE=$LINGOS_MODE"
_emit "LINGOS-BOOT-INFO kernel=$(uname -r) arch=$(uname -m) cmdline=$(cat /proc/cmdline 2>/dev/null || true)"
log "mode=$LINGOS_MODE kernel=$(uname -r) arch=$(uname -m)"
if [ -f /etc/lingos-image-info ]; then
    _emit "LINGOS-IMAGE-INFO $(cat /etc/lingos-image-info 2>/dev/null || true)"
fi
for _c in /dev/ttyAMA0 /dev/ttyS0; do [ -e "$_c" ] && printf 'LINGOS-BOOTINFO-END\n' > "$_c" 2>/dev/null || true; done
stage 1 preflight "Checking live environment"
[ "$(id -u)" = "0" ] || { fail_msg 3 "Root privileges are required"; exit 3; }
[ -d /run/live ] || { fail_msg 4 "Live environment not found (/run/live is missing)"; exit 4; }
log "Live environment check passed (mode=$LINGOS_MODE auto=$AUTO)"

# ---------- 磁盘候选（排除 USB） ----------
udevadm settle 2>/dev/null || true
CANDIDATES=""
LIVE_SRC=$(findmnt -n -o SOURCE /run/live/medium 2>/dev/null || true)
while IFS= read -r row; do
    eval "$row"
    [ "${TYPE:-}" = "disk" ] || continue
    [ -b "/dev/$NAME" ] || continue
    [ "${TRAN:-}" = "usb" ] && { log "Skipping USB disk /dev/$NAME"; continue; }
    [ -n "${SIZE:-}" ] || continue
    case "$LIVE_SRC" in "/dev/$NAME"*) log "Skipping live media /dev/$NAME"; continue ;; esac
    CANDIDATES="$CANDIDATES /dev/$NAME:$SIZE"
done < <(lsblk -bdnP -o NAME,TYPE,TRAN,SIZE 2>/dev/null)
[ -n "$CANDIDATES" ] || { fail_msg 5 "No installable disk found (USB disks are excluded)"; exit 5; }
log "Installable disks:$CANDIDATES"

# ---------- 选择磁盘 ----------
stage 2 select-disk "Selecting install disk"
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
        DISK="$BEST"; log "Auto mode selected disk $DISK ($BESTSIZE bytes)"
    else
        fail_msg 6 "Auto install needs a disk of at least 8GB (candidates:$CANDIDATES)"; exit 6
    fi
elif [ -z "$DISK" ]; then
    MENU=""
    for row in $CANDIDATES; do
        DEV="${row%%:*}"; SZ="${row##*:}"
        MENU="$MENU $DEV $(basename "$DEV") $SZ"
    done
    DISK=$(whiptail --title "LING OS $LINGOS_VERSION Installer" --menu "Select the disk to install LING OS on.\n\nALL DATA ON THE SELECTED DISK WILL BE ERASED." 20 70 10 $MENU 3>&1 1>&2 2>&3)         || { fail_msg 7 "Disk selection cancelled by user"; exit 7; }
    log "User selected disk $DISK"
else
    log "Using the specified disk $DISK"
fi

[ -b "$DISK" ] || { fail_msg 8 "Not a block device: $DISK"; exit 8; }
ROOTSRC=$(findmnt -n -o SOURCE / 2>/dev/null || true)
case "$ROOTSRC" in
    "$DISK"|"$DISK"p[0-9]*|"$DISK"[0-9]*) fail_msg 9 "Refusing to install: target contains the running live root ($ROOTSRC)"; exit 9 ;;
esac

# ---------- 确认 ----------
stage 3 confirm "Confirming installation"
if [ "$AUTO" != "1" ]; then
    whiptail --title "LING OS $LINGOS_VERSION Confirm" --yesno "This will erase $DISK and install LING OS.\n\nContinue?" 10 60         || { fail_msg 10 "Installation cancelled by user"; exit 10; }
fi

# ---------- 全盘加密选项（LUKS2；默认关闭，避免 TCG 设备过慢） ----------
if [ "$AUTO" != "1" ] && [ "$ENCRYPT" = "0" ]; then
    if whiptail --title "LING OS $LINGOS_VERSION Full Disk Encryption" --yesno "Enable LUKS2 full disk encryption?\n\nRecommended: protects your data if the device is lost.\nSoftware-emulated machines (e.g. Vectras) will be noticeably slower." 12 64; then
        ENCRYPT=1
    fi
fi
if [ "$ENCRYPT" = "1" ] && [ -z "$LUKS_PASS" ]; then
    if [ "$AUTO" = "1" ]; then
        log "Auto mode: no LINGOS_LUKS_PASSPHRASE given, falling back to unencrypted"
        ENCRYPT=0
    else
        LUKS_PASS=$(whiptail --title "LING OS $LINGOS_VERSION Encryption Passphrase" --passwordbox "Set the LUKS2 unlock passphrase (at least 8 characters):" 10 64 3>&1 1>&2 2>&3) || { fail_msg 12 "Passphrase entry cancelled by user"; exit 12; }
        LUKS_PASS2=$(whiptail --title "LING OS $LINGOS_VERSION Encryption Passphrase" --passwordbox "Re-enter the passphrase:" 10 64 3>&1 1>&2 2>&3) || { fail_msg 12 "Passphrase entry cancelled by user"; exit 12; }
        [ "$LUKS_PASS" = "$LUKS_PASS2" ] || { fail_msg 13 "Passphrases do not match"; exit 13; }
        [ ${#LUKS_PASS} -ge 8 ] || { fail_msg 14 "Passphrase must be at least 8 characters"; exit 14; }
    fi
fi
log "Full disk encryption: $ENCRYPT"

# ---------- 隐私等级（L0-L4，默认 L1） ----------
PRIVACY_LEVEL="${LINGOS_PRIVACY_LEVEL:-}"
for arg in $(cat /proc/cmdline 2>/dev/null || true); do
    case "$arg" in
        lingos.privacy=L[0-4]) PRIVACY_LEVEL="${arg#lingos.privacy=}" ;;
    esac
done
if [ -z "$PRIVACY_LEVEL" ] && [ "$AUTO" != "1" ]; then
    PRIVACY_LEVEL=$(whiptail --title "LING OS $LINGOS_VERSION Privacy Level" --menu "Select the default privacy level (it can be changed later in Security Settings):" 16 70 5 \
        L0 "Compatible (features first)" L1 "Enhanced (recommended)" L2 "Strict" L3 "Extreme" L4 "MAX (deny everything)" 3>&1 1>&2 2>&3) || PRIVACY_LEVEL=L1
fi
case "$PRIVACY_LEVEL" in L0|L1|L2|L3|L4) ;; *) PRIVACY_LEVEL=L1 ;; esac
log "Privacy level: $PRIVACY_LEVEL"

# ---------- 分区 ----------
stage 4 partition "Partitioning the disk (GPT: 512M EFI + ext4 root)"
UEFI=0; [ -d /sys/firmware/efi ] && UEFI=1
# 清理目标盘上的挂载/签名（Vectras 等可能把预格式化 vfat 分区挂成共享文件夹；
# 设备 busy 会让 parted mklabel 失败）
for _p in $(lsblk -ln -o NAME,TYPE "$DISK" 2>/dev/null | awk '$2=="part"{print $1}'); do
    umount -f "/dev/$_p" 2>/dev/null || true
done
if [ -n "${LINGOS_SHARE:-}" ]; then
    _src=$(findmnt -n -o SOURCE "$LINGOS_SHARE" 2>/dev/null || true)
    if [ -n "$_src" ]; then
        _pk=$(lsblk -no PKNAME "$_src" 2>/dev/null | head -1)
        if [ "$_src" = "$DISK" ] || { [ -n "$_pk" ] && [ "/dev/$_pk" = "$DISK" ]; }; then
            umount -f "$LINGOS_SHARE" 2>/dev/null || true
            LINGOS_SHARE=""
            _emit "LINGOS-SHARE-RELEASED=$DISK"
        fi
    fi
fi
swapoff -a 2>/dev/null || true
wipefs -a "$DISK" >/dev/null 2>&1 || true
partprobe "$DISK" 2>/dev/null || true
_emit "LINGOS-DISK-PREPARE=$DISK"
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
log "Partitioning completed: $P1 / $P2 (UEFI=$UEFI)"

# ---------- 格式化 ----------
stage 5 format "Creating filesystems"
if [ "$UEFI" = "1" ]; then
    mkfs.vfat -F32 -n LINGOS_EFI "$P1" >/dev/null
fi
if [ "$ENCRYPT" = "1" ]; then
    command -v cryptsetup >/dev/null 2>&1 || { fail_msg 15 "cryptsetup is missing, encrypted install is not possible"; exit 15; }
    printf '%s\n' "$LUKS_PASS" | cryptsetup luksFormat --type luks2 --pbkdf argon2id --iter-time 1000 --batch-mode "$P2" - || { fail_msg 16 "luksFormat failed"; exit 16; }
    printf '%s\n' "$LUKS_PASS" | cryptsetup luksOpen "$P2" "$CRYPT_NAME" - || { fail_msg 17 "luksOpen failed"; exit 17; }
    ROOT_DEV="/dev/mapper/$CRYPT_NAME"
    LUKS_UUID=$(blkid -s UUID -o value "$P2")
    log "LUKS2 created: $P2 ($LUKS_UUID) -> $CRYPT_NAME"
else
    ROOT_DEV="$P2"
fi
mkfs.ext4 -F -L LINGOS_ROOT "$ROOT_DEV" >/dev/null
log "Filesystems created"

# ---------- 挂载 ----------
stage 6 mount "Mounting target filesystems"
TARGET=/target
mkdir -p "$TARGET"
mount "$ROOT_DEV" "$TARGET"
if [ "$UEFI" = "1" ]; then mkdir -p "$TARGET/boot/efi"; mount "$P1" "$TARGET/boot/efi"; fi
log "Mounted $P2 -> $TARGET"

# ---------- 复制 live 根 ----------
stage 7 copy "Copying system files"
RSYNC_START=$(_now)
COPY_START_SECONDS=$SECONDS
log "Copying the live system to $TARGET (rsync; excludes doc/man/info, caches and logs)"
_emit "STAGE 7/10 COPY 0% (elapsed 0s, eta unknown)"

# rsync --info=progress2 每次刷新都带百分比；控制台只在百分比前进 >=5 或者
# 距上次输出 >=10 秒时打印一行，避免 3GB/TCG 场景刷屏（Vectras 约 30 分钟）。
copy_progress_line() {   # <percent> <elapsed-seconds>
    local pct="$1" elapsed="$2" eta_txt
    if [ "$pct" -ge 100 ]; then
        eta_txt="0s"
    elif [ "$pct" -gt 0 ]; then
        eta_txt=$(fmt_duration $(( elapsed * (100 - pct) / pct )))
    else
        eta_txt="unknown"
    fi
    _emit "STAGE 7/10 COPY ${pct}% (elapsed $(fmt_duration "$elapsed"), eta ${eta_txt})"
    # 状态机/CI 标记每 5% 才写一次，避免和上面的人类可读行重复刷屏
    if [ "$pct" -eq 100 ] || [ $(( pct - last_state_pct )) -ge 5 ]; then
        last_state_pct="$pct"
        write_state "copy" "$(( 60 + pct * 25 / 100 ))" "Copying system files ${pct}%"
    fi
}

# tr 默认按 4KB 块缓冲，进度行会成批到达；有 stdbuf 时改用行缓冲，保证 10 秒节流真实生效
if command -v stdbuf >/dev/null 2>&1; then
    TR_PROGRESS=(stdbuf -oL tr '\r' '\n')
else
    TR_PROGRESS=(tr '\r' '\n')
fi

set +e
rsync -aHAX --one-file-system --info=progress2 --no-inc-recursive --stats \
    --exclude=/proc --exclude=/sys --exclude=/dev --exclude=/run --exclude=/tmp --exclude=/mnt \
    --exclude=/media --exclude=/live --exclude=/cdrom --exclude=/var/cache/apt \
    --exclude=/usr/share/doc --exclude=/usr/share/man --exclude=/usr/share/info \
    --exclude=/var/log/lingos-install.log --exclude=/run/lingos-install \
    / "$TARGET/" 2>&1 | "${TR_PROGRESS[@]}" | {
        last_pct=-1
        last_state_pct=-1
        last_ts=$SECONDS
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            if [[ "$line" =~ ([0-9]{1,3})% ]]; then
                pct=$(( 10#${BASH_REMATCH[1]} ))
                [ "$pct" -le 100 ] 2>/dev/null || continue
                [ "$pct" -gt "$last_pct" ] 2>/dev/null || continue
                now=$SECONDS
                if [ "$pct" -eq 100 ] || [ $(( pct - last_pct )) -ge 5 ] || [ $(( now - last_ts )) -ge 10 ]; then
                    last_pct="$pct"
                    last_ts="$now"
                    copy_progress_line "$pct" "$(( now - COPY_START_SECONDS ))"
                fi
            else
                # rsync --stats / 错误行：只进日志，不刷控制台
                printf '%s\n' "$line" >> "$LINGOS_LOG" 2>/dev/null || true
            fi
        done
    }
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" != "0" ]; then fail_msg 11 "rsync copy failed (rc=$rc)"; exit 11; fi
COPY_ELAPSED=$(( $(_now) - RSYNC_START ))
COPY_BYTES=$(du -sbx "$TARGET" 2>/dev/null | awk '{print $1}' || true)
[ -n "${COPY_BYTES:-}" ] || COPY_BYTES=0
log "Copy completed in $(fmt_duration "$COPY_ELAPSED") (${COPY_ELAPSED}s): $(human_bytes "$COPY_BYTES") copied (${COPY_BYTES} bytes, root filesystem)"

# ---------- GRUB ----------
stage 8 bootloader "Installing the GRUB bootloader"
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
# ---------- 安装后安全启动项（文本 / X11 回退 / 详细日志） ----------
ROOT_UUID=$(blkid -s UUID -o value "$ROOT_DEV" 2>/dev/null || true)
CRYPT_KARGS=""
if [ "$ENCRYPT" = "1" ] && [ -n "$LUKS_UUID" ]; then
    CRYPT_KARGS="cryptdevice=UUID=$LUKS_UUID:$CRYPT_NAME"
fi
KVER=$(basename "$(ls "$TARGET"/boot/vmlinuz-* 2>/dev/null | head -1)" 2>/dev/null | sed 's/^vmlinuz-//')
if [ -n "$ROOT_UUID" ] && [ -n "$KVER" ]; then
    mkdir -p "$TARGET/etc/grub.d" "$TARGET/etc/default"
    {
        echo '#!/bin/sh'
        echo 'exec tail -n +3 $0'
        echo "menuentry 'LING OS (text debug console, no desktop)' --class ubuntu --class gnu-linux --class os {"
        echo "    linux /boot/vmlinuz-$KVER root=UUID=$ROOT_UUID ro loglevel=7 $CRYPT_KARGS systemd.unit=multi-user.target lingos.desktop=0"
        echo "    initrd /boot/initrd.img-$KVER"
        echo '}'
        echo "menuentry 'LING OS (X11 safe graphics fallback)' --class ubuntu --class gnu-linux --class os {"
        echo "    linux /boot/vmlinuz-$KVER root=UUID=$ROOT_UUID ro loglevel=5 $CRYPT_KARGS lingos.session=x11"
        echo "    initrd /boot/initrd.img-$KVER"
        echo '}'
        echo "menuentry 'LING OS (verbose boot log)' --class ubuntu --class gnu-linux --class os {"
        echo "    linux /boot/vmlinuz-$KVER root=UUID=$ROOT_UUID ro loglevel=7 $CRYPT_KARGS systemd.show_status=1"
        echo "    initrd /boot/initrd.img-$KVER"
        echo '}'
        echo "menuentry 'LING OS (recovery mode)' --class ubuntu --class gnu-linux --class os {"
        echo "    linux /boot/vmlinuz-$KVER root=UUID=$ROOT_UUID ro loglevel=5 $CRYPT_KARGS systemd.unit=rescue.target"
        echo "    initrd /boot/initrd.img-$KVER"
        echo '}'
    } > "$TARGET/etc/grub.d/40_custom"
    chmod +x "$TARGET/etc/grub.d/40_custom"
    touch "$TARGET/etc/default/grub"
    if grep -q '^GRUB_TIMEOUT=' "$TARGET/etc/default/grub"; then
        sed -i 's/^GRUB_TIMEOUT=.*/GRUB_TIMEOUT=8/' "$TARGET/etc/default/grub"
    else
        echo 'GRUB_TIMEOUT=8' >> "$TARGET/etc/default/grub"
    fi
    if grep -q '^GRUB_TIMEOUT_STYLE=' "$TARGET/etc/default/grub"; then
        sed -i 's/^GRUB_TIMEOUT_STYLE=.*/GRUB_TIMEOUT_STYLE=menu/' "$TARGET/etc/default/grub"
    else
        echo 'GRUB_TIMEOUT_STYLE=menu' >> "$TARGET/etc/default/grub"
    fi
    sed -i 's/ video=[^ "]*//g' "$TARGET/etc/default/grub" 2>/dev/null || true
fi
chroot "$TARGET" update-grub
umount "$TARGET/dev" "$TARGET/sys" "$TARGET/proc" 2>/dev/null || true
log "GRUB installed (target=$GRUBTARGET uefi=$UEFI)"

# ---------- 配置 ----------
stage 9 configure "Writing configuration and first-boot setup"
ROOT_UUID=$(blkid -s UUID -o value "$ROOT_DEV")
{
    echo "UUID=$ROOT_UUID  /  ext4  errors=remount-ro  0  1"
    if [ "$UEFI" = "1" ]; then
        EFI_UUID=$(blkid -s UUID -o value "$P1")
        echo "UUID=$EFI_UUID  /boot/efi  vfat  umask=0077  0  1"
    fi
} > "$TARGET/etc/fstab"
if [ "$ENCRYPT" = "1" ] && [ -n "$LUKS_UUID" ]; then
    printf '%s UUID=%s none luks,discard\n' "$CRYPT_NAME" "$LUKS_UUID" > "$TARGET/etc/crypttab"
    printf 'GRUB_CMDLINE_LINUX="${GRUB_CMDLINE_LINUX:-} cryptdevice=UUID=%s:%s"\n' "$LUKS_UUID" "$CRYPT_NAME" >> "$TARGET/etc/default/grub"
    mkdir -p "$TARGET/proc" "$TARGET/sys" "$TARGET/dev"
    mount --bind /proc "$TARGET/proc" 2>/dev/null || true
    mount --bind /sys "$TARGET/sys" 2>/dev/null || true
    mount --bind /dev "$TARGET/dev" 2>/dev/null || true
    chroot "$TARGET" update-initramfs -u -k all >/dev/null 2>&1 || true
    chroot "$TARGET" update-grub >/dev/null 2>&1 || true
    umount "$TARGET/dev" "$TARGET/sys" "$TARGET/proc" 2>/dev/null || true
    log "Encryption config written (crypttab/GRUB/initramfs)"
fi
echo lingos > "$TARGET/etc/hostname"
mkdir -p "$TARGET/LINGOS/system/config" "$TARGET/LINGOS/data" "$TARGET/LINGOS/log" \
         "$TARGET/LINGOS/run" "$TARGET/LINGOS/Debug/log"
printf '%s\n' '{"system_configured": true, "last_config_time": "1970-01-01T00:00:00Z"}' \
    > "$TARGET/LINGOS/system/config/state.json"
mkdir -p "$TARGET/etc/lingos"
printf '{"schema_version":1,"privacy_level":"%s","local_only":true,"flags":{}}\n' "$PRIVACY_LEVEL" \
    > "$TARGET/etc/lingos/security-policy.json"
chmod 600 "$TARGET/etc/lingos/security-policy.json" 2>/dev/null || true
printf '%s\n' '{"schema_version": 1, "ai": {"ai_config": {"backend": "deepseek", "language": "zh"}, "provider": {"active": "deepseek", "deepseek": {"api_key": "", "model": "deepseek-v4-pro", "base_url": "https://api.deepseek.com"}}}}' \
    > "$TARGET/LINGOS/system/config/config.json"
cat > "$TARGET/LINGOS/system/config/install-report.json" <<REPORT
{"version":"$LINGOS_VERSION","mode":"$LINGOS_MODE","disk":"$DISK","uefi":$UEFI,"root_uuid":"$ROOT_UUID","installed_at":"$(date -u +%FT%TZ)"}
REPORT
mkdir -p "$TARGET/var/log"
cp -f "$LINGOS_LOG" "$TARGET/var/log/lingos-install.log" 2>/dev/null || true
cp -f "$LINGOS_STATE" "$TARGET/LINGOS/system/config/install-state.json" 2>/dev/null || true
mkdir -p "$TARGET/etc/systemd/system/multi-user.target.wants"
ln -sf /etc/systemd/system/lingos.service "$TARGET/etc/systemd/system/multi-user.target.wants/lingos.service"
rm -f "$TARGET/etc/systemd/system/multi-user.target.wants/lingos-installer.service"
rm -f "$TARGET/etc/systemd/system/lingos-installer.service" "$TARGET/etc/lingos-live"
chroot "$TARGET" passwd -d root 2>/dev/null || true

# 桌面用户（useradd -m 会复制 /etc/skel 中的桌面配置）
chroot "$TARGET" useradd -m -s /bin/bash -G video,render,input,audio lingos 2>/dev/null \
    || chroot "$TARGET" useradd -m -s /bin/bash lingos 2>/dev/null || true

# 只设临时密码：最终凭据由图形 OOBE 设置。随机值同时作为 OOBE 无法启动时的
# 兜底提示，写入 root-only 的 /root/lingos-oobe-password.txt，且不打印到控制台。
if [ -n "${LINGOS_PASSWORD:-}" ]; then
    USER_PASS="$LINGOS_PASSWORD"
    log "Using LINGOS_PASSWORD as the temporary password for user lingos"
else
    USER_PASS=$(od -An -N8 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n' || true)
    log "Generated a random temporary password for user lingos"
fi
[ -n "${USER_PASS:-}" ] || USER_PASS=$(od -An -N8 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n' || true)
if [ -z "${USER_PASS:-}" ]; then
    fail_msg 18 "Could not generate a temporary password for user lingos"
    exit 18
fi
printf 'lingos:%s\n' "$USER_PASS" | chroot "$TARGET" chpasswd 2>/dev/null || true
mkdir -p "$TARGET/root"
install -m 0600 /dev/null "$TARGET/root/lingos-oobe-password.txt"
printf '%s\n' "$USER_PASS" > "$TARGET/root/lingos-oobe-password.txt"
chmod 0600 "$TARGET/root/lingos-oobe-password.txt" 2>/dev/null || true
chown 0:0 "$TARGET/root/lingos-oobe-password.txt" 2>/dev/null || true
log "Temporary password stored in /root/lingos-oobe-password.txt (mode 0600, root only)"

# 首启 OOBE 提权：只允许 lingos 免密执行 lingos-oobe-apply（OOBE 完成后由它自行删除该文件）
mkdir -p "$TARGET/etc/sudoers.d"
cat > "$TARGET/etc/sudoers.d/90-lingos-oobe" <<'SUDOERS'
lingos ALL=(root) NOPASSWD: /usr/local/sbin/lingos-oobe-apply
SUDOERS
chmod 0440 "$TARGET/etc/sudoers.d/90-lingos-oobe"
chown 0:0 "$TARGET/etc/sudoers.d/90-lingos-oobe" 2>/dev/null || true
if [ -f "$TARGET/etc/sudoers" ] && ! grep -qE '^[#@]includedir[[:space:]]+/etc/sudoers.d' "$TARGET/etc/sudoers"; then
    log "WARNING: /etc/sudoers does not include /etc/sudoers.d, the OOBE sudo rule may be inactive"
fi

# 图形登录：LightDM 自动登录 lingos 进入一次性 OOBE 会话。
# 不再创建 tty1 getty 自动登录，也不通过 systemd 启用 lingos-oobe.service。
LIGHTDM_D="$TARGET/etc/lightdm/lightdm.conf.d"
mkdir -p "$LIGHTDM_D"
cat > "$LIGHTDM_D/40-lingos-oobe.conf" <<'LIGHTDM_OOBE'
[Seat:*]
autologin-user=lingos
autologin-user-timeout=0
user-session=lingos-oobe
LIGHTDM_OOBE
chmod 0644 "$LIGHTDM_D/40-lingos-oobe.conf"

# lightdm.conf.d 按文件名顺序合并、后者覆盖前者：镜像自带的 50-lingos.conf
# 设了 autologin-user=（空）和 user-session=lingos-cinnamon，会盖掉 40- 文件。
# 因此再放一份 60- 覆盖；lingos-oobe-apply 首启完成后会同时删除 40-/60-，
# 日常登录自然回到 50-lingos.conf 的桌面会话。
cat > "$LIGHTDM_D/60-lingos-oobe.conf" <<'LIGHTDM_OOBE'
[Seat:*]
autologin-user=lingos
autologin-user-timeout=0
user-session=lingos-oobe
LIGHTDM_OOBE
chmod 0644 "$LIGHTDM_D/60-lingos-oobe.conf"

# 清理旧版 tty1 自动登录 drop-in 与旧 OOBE systemd 单元残留
rm -rf "$TARGET/etc/systemd/system/getty@tty1.service.d"
rm -f "$TARGET/etc/systemd/system/multi-user.target.wants/lingos-oobe.service"

chroot "$TARGET" systemctl enable lightdm >/dev/null 2>&1 \
    || ln -sf /lib/systemd/system/lightdm.service "$TARGET/etc/systemd/system/display-manager.service"
# 安装后启动优化：initramfs 精简 + GRUB 快速启动 + mask 慢服务
mkdir -p "$TARGET/etc/initramfs-tools/conf.d" "$TARGET/etc/default"
printf 'MODULES=dep\nCOMPRESS=zstd\n' > "$TARGET/etc/initramfs-tools/conf.d/lingos-boot.conf"
if [ -f "$TARGET/etc/default/grub" ]; then
    sed -i 's/^GRUB_TIMEOUT=.*/GRUB_TIMEOUT=1/' "$TARGET/etc/default/grub"
    grep -q '^GRUB_TIMEOUT_STYLE=' "$TARGET/etc/default/grub" || echo 'GRUB_TIMEOUT_STYLE=hidden' >> "$TARGET/etc/default/grub"
    grep -q 'quiet' "$TARGET/etc/default/grub" || sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT=.*/GRUB_CMDLINE_LINUX_DEFAULT="quiet splash loglevel=3"/' "$TARGET/etc/default/grub"
grep -q 'splash' "$TARGET/etc/default/grub" || sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT=.*/GRUB_CMDLINE_LINUX_DEFAULT="quiet splash loglevel=3"/' "$TARGET/etc/default/grub"
fi
for unit in avahi-daemon.service cups.service cups-browsed.service whoopsie.service apport.service kerneloops.service ModemManager.service bluetooth.service apt-daily.service apt-daily-upgrade.service man-db.service e2scrub_all.service unattended-upgrades.service; do
    chroot "$TARGET" systemctl mask "$unit" >/dev/null 2>&1 || true
done
chroot "$TARGET" update-initramfs -u -k all >/dev/null 2>&1 || true
chroot "$TARGET" update-grub >/dev/null 2>&1 || true
chmod +x "$TARGET/usr/local/sbin/lingos-desktop-report" 2>/dev/null || true
ln -sf /etc/systemd/system/lingos-desktop-report.service \
    "$TARGET/etc/systemd/system/multi-user.target.wants/lingos-desktop-report.service" 2>/dev/null || true
ln -sf /etc/systemd/system/lingos-boot-report.service \
    "$TARGET/etc/systemd/system/multi-user.target.wants/lingos-boot-report.service" 2>/dev/null || true
log "Desktop user lingos, LightDM OOBE autologin and boot tuning configured"
log "Configuration written"

# ---------- 完成 ----------
stage 10 finish "Complete, ready to reboot"
sync
umount "$TARGET/boot/efi" 2>/dev/null || true
umount "$TARGET" 2>/dev/null || true
if [ "$ENCRYPT" = "1" ]; then cryptsetup close "$CRYPT_NAME" 2>/dev/null || true; fi
# mark_done 的中文提示由等价的英文实现替代（固定标记与回传保持一致）
write_state "done" 100 "Installation complete"
_emit "LINGOS-INSTALL-DONE mode=$LINGOS_MODE ts=$(_stamp)"
report_net "LINGOS-INSTALL-DONE mode=$LINGOS_MODE"
if [ "$AUTO" = "1" ]; then
    log "Auto install complete, rebooting in 8 seconds"
    for i in 8 7 6 5 4 3 2 1; do
        _emit "LINGOS-INSTALL-REBOOT-IN=$i"
        sleep 1
    done
    reboot -f
else
    whiptail --title "LING OS $LINGOS_VERSION Installation Complete" --msgbox "LING OS has been installed to $DISK.\n\nRemove the installation media and reboot to start the first-boot setup (OOBE)." 12 64 || true
fi
