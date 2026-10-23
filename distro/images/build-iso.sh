#!/usr/bin/env bash
# ============================================================
# LING OS 安装 ISO 构建（Ubuntu noble live-build，UEFI；x86 走 live-build，arm64 用 grub-mkrescue 重制）
# 用法：sudo bash distro/images/build-iso.sh [x86_64|aarch64] [发行版代号]
# 前置：dist/*allbin*.tar.gz 存在；root；网络
# 产物：dist/images/lingos-installer-<arch>.iso
#
# 0.9.12 起：内核/initrd 带日志；GRUB 菜单支持 自动安装/手动安装/Live/诊断；
#            安装器阶段进度写入 /run/lingos-install/state.json 并回传 tty0/串口/共享文件夹/网络。
# ============================================================
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ARCHARG=$1
if [ -z "$ARCHARG" ]; then ARCHARG=$(uname -m); fi
DIST=""
if [ "$#" -ge 2 ]; then DIST=$2; fi
if [ -z "$DIST" ]; then DIST=noble; fi
case "$ARCHARG" in
    x86_64|amd64)  ARCH=x86_64;  DEBARCH=amd64; GRUBTARGET=x86_64-efi; MIRROR=http://archive.ubuntu.com/ubuntu ;;
    aarch64|arm64) ARCH=aarch64; DEBARCH=arm64; GRUBTARGET=arm64-efi;  MIRROR=http://ports.ubuntu.com/ubuntu-ports ;;
    *) echo "unsupported arch: $ARCHARG" >&2; exit 1 ;;
esac
BOOTOPT=""
if [ "$DEBARCH" = "amd64" ]; then BOOTOPT="--bootloader grub-efi"; fi
OUT=$ROOT/dist/images
mkdir -p "$OUT"
BUNDLE=$(ls "$ROOT"/dist/*allbin*.tar.gz 2>/dev/null | head -1 || true)
if [ -z "$BUNDLE" ]; then
    echo "缺少 dist/*allbin*.tar.gz —— 先 make + RELEASE_VER=0.9.0 bash scripts/bundle.sh dist" >&2
    exit 1
fi
if ! command -v lb >/dev/null 2>&1; then
    echo "需要 live-build：apt-get install -y live-build debootstrap squashfs-tools xorriso mtools dosfstools rsync" >&2
    exit 1
fi
GITREV=$(cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null || echo unknown)
BUILD_TS=$(date -u +%FT%TZ)
# 0.9.12 统一内核参数：不静默、开日志、串口放最后（/dev/console -> 串口，CI/真机可抓 live-boot 日志）
if [ "$DEBARCH" = "amd64" ]; then
    KCONSOLE="console=tty0 console=ttyS0,115200"
else
    KCONSOLE="console=tty0 console=ttyAMA0,115200"
fi
KCMDLINE="boot=live components live-media=/dev/sr0 $KCONSOLE loglevel=5 video=1024x768@60 lingos.mode=install-auto"

WORK=$(mktemp -d /tmp/lingos-iso-XXXXXX)
echo "==> live-build 工作目录: $WORK (dist=$DIST arch=$DEBARCH)"
cd "$WORK"

lb config \
  --mode ubuntu \
  --distribution "$DIST" \
  --architecture "$DEBARCH" \
  --binary-images iso \
  $BOOTOPT \
  --apt-recommends false \
  --memtest none \
  --iso-application "LING OS" \
  --iso-volume "LINGOS" \
  --archive-areas "main restricted universe multiverse" \
  --parent-distribution "$DIST" \
  --parent-archive-areas "main restricted universe multiverse" \
  --mirror-bootstrap "$MIRROR" \
  --mirror-chroot "$MIRROR" \
  --mirror-binary "$MIRROR" \
  --parent-mirror-bootstrap "$MIRROR" \
  --parent-mirror-chroot "$MIRROR" \
  --parent-mirror-binary "$MIRROR" \
  --bootappend-live "$KCMDLINE" \
  --linux-flavours generic

# ---------- 包清单 ----------
mkdir -p config/package-lists
cat > config/package-lists/lingos.list.chroot <<'EOF'
live-boot
live-config
linux-image-generic
systemd-sysv
network-manager
ca-certificates
curl
python3
python3-requests
rsync
e2fsprogs
dosfstools
parted
gdisk
whiptail
util-linux
kmod
netcat-openbsd
isc-dhcp-client
psmisc
grub2-common
grub-efi-amd64-bin
grub-efi-amd64-signed
grub-pc-bin
syslinux
syslinux-utils
labwc
waybar
wofi
foot
fonts-noto-cjk
espeak-ng
ffmpeg
libcurl4
libseccomp2
libsqlite3-0
libmosquitto1
libmicrohttpd12
libssl3
EOF
if [ "$DEBARCH" = "arm64" ]; then
    sed -i '/grub-efi-amd64/d; /grub-pc-bin/d; /^syslinux$/d; /^syslinux-utils$/d' config/package-lists/lingos.list.chroot
    echo 'grub-efi-arm64-bin' >> config/package-lists/lingos.list.chroot
    echo 'grub-efi-arm64-signed' >> config/package-lists/lingos.list.chroot
fi

# ---------- 安装器 / 服务单元 / 日志库 / 全捆包 ----------
mkdir -p config/includes.chroot/usr/local/sbin \
         config/includes.chroot/etc/systemd/system \
         config/includes.chroot/etc/initramfs-tools/scripts/init-premount \
         config/includes.chroot/etc/initramfs-tools/scripts/init-bottom \
         config/includes.chroot/LINGOS/offline \
         config/hooks/live
cp "$ROOT/distro/images/live/lingos-install.sh" "config/includes.chroot/usr/local/sbin/lingos-install"
chmod +x "config/includes.chroot/usr/local/sbin/lingos-install"
for f in lingos-boot-mode lingos-live-session lingos-diag lingos-diag-session; do
    cp "$ROOT/distro/images/live/$f" "config/includes.chroot/usr/local/sbin/$f"
    chmod +x "config/includes.chroot/usr/local/sbin/$f"
done
cp "$ROOT/distro/images/live/lingos-log.sh" "config/includes.chroot/usr/local/sbin/lingos-log.sh"
chmod 644 "config/includes.chroot/usr/local/sbin/lingos-log.sh"
cp "$ROOT/distro/images/live/lingos-initramfs-report" \
   "config/includes.chroot/etc/initramfs-tools/scripts/init-premount/lingos-report"
chmod +x "config/includes.chroot/etc/initramfs-tools/scripts/init-premount/lingos-report"
cp "$ROOT/distro/images/live/lingos-initramfs-bottom" \
   "config/includes.chroot/etc/initramfs-tools/scripts/init-bottom/lingos-bottom"
chmod +x "config/includes.chroot/etc/initramfs-tools/scripts/init-bottom/lingos-bottom"
# 桌面配置（安装后由 installer 创建 lingos 用户，useradd -m 会复制 /etc/skel）
mkdir -p config/includes.chroot/etc/skel/.config/labwc \
         config/includes.chroot/etc/skel/.config/waybar \
         config/includes.chroot/etc/skel/.config/wofi
cp "$ROOT/distro/desktop/skel/.bash_profile"        config/includes.chroot/etc/skel/.bash_profile
cp "$ROOT/distro/desktop/skel/.config/labwc/autostart" config/includes.chroot/etc/skel/.config/labwc/autostart
cp "$ROOT/distro/desktop/labwc/rc.xml"              config/includes.chroot/etc/skel/.config/labwc/rc.xml
cp "$ROOT/distro/desktop/waybar/config.jsonc"       config/includes.chroot/etc/skel/.config/waybar/config.jsonc
cp "$ROOT/distro/desktop/waybar/style.css"          config/includes.chroot/etc/skel/.config/waybar/style.css
cp "$ROOT/distro/desktop/wofi/style.css"            config/includes.chroot/etc/skel/.config/wofi/style.css
cp "$ROOT/distro/images/live/lingos.service"           config/includes.chroot/etc/systemd/system/lingos.service
cp "$ROOT/distro/images/live/lingos-installer.service" config/includes.chroot/etc/systemd/system/lingos-installer.service
cp "$BUNDLE" config/includes.chroot/LINGOS/offline/lingos_bundle.tar.gz
cat > config/includes.chroot/etc/lingos-image-info <<INFO
LINGOS_VERSION=0.9.12
LINGOS_ARCH=$ARCH
LINGOS_GIT=$GITREV
LINGOS_BUILD_TS=$BUILD_TS
LINGOS_KCMDLINE=$KCMDLINE
INFO

# ---------- chroot hook：安装全捆 + 启用自动安装器 + initramfs 日志 ----------
cat > config/hooks/live/0100-lingos.hook.chroot <<'HOOK'
#!/bin/sh
set -e
mkdir -p /tmp/lingos_bundle
tar xzf /LINGOS/offline/lingos_bundle.tar.gz -C /tmp/lingos_bundle --strip-components=1
bash /tmp/lingos_bundle/install.sh /LINGOS
rm -rf /tmp/lingos_bundle
rm -f /LINGOS/offline/lingos_bundle.tar.gz
chmod +x /usr/local/sbin/lingos-* 2>/dev/null || true
chmod 644 /usr/local/sbin/lingos-log.sh 2>/dev/null || true
mkdir -p /etc/systemd/system/multi-user.target.wants
ln -sf /etc/systemd/system/lingos-installer.service /etc/systemd/system/multi-user.target.wants/lingos-installer.service

# initramfs：加入 LING OS 早期启动标记；MODULES=most 保证 live-boot 能识别 USB/光驱/squashfs
mkdir -p /etc/initramfs-tools/scripts/init-premount
if [ ! -f /etc/initramfs-tools/scripts/init-premount/lingos-report ]; then
cat > /etc/initramfs-tools/scripts/init-premount/lingos-report <<'IREPORT'
#!/bin/sh
PREREQ=""
prereqs() { echo "$PREREQ"; }
case "$1" in prereqs) prereqs; exit 0 ;; esac
for m in isofs squashfs overlay loop sr_mod sd_mod usb-storage uas virtio_blk virtio_pci; do
    modprobe -q "$m" 2>/dev/null || true
done
printf '<6>LINGOS-INITRAMFS-START kernel=%s\n' "$(uname -r)" > /dev/kmsg 2>/dev/null || true
for c in /dev/ttyAMA0 /dev/ttyS0; do
    [ -e "$c" ] && printf 'LINGOS-INITRAMFS-START kernel=%s\n' "$(uname -r)" > "$c" 2>/dev/null || true
done
exit 0
IREPORT

# 确保 blkid / udevadm 及其依赖库进入 initramfs（live-boot 的 get_fstype 依赖 blkid）
mkdir -p /etc/initramfs-tools/hooks
cat > /etc/initramfs-tools/hooks/lingos-tools <<'ITOOLS'
#!/bin/sh
PREREQ=""
prereqs() { echo "$PREREQ"; }
case "$1" in prereqs) prereqs; exit 0 ;; esac
. /usr/share/initramfs-tools/hook-functions
copy_exec /usr/sbin/blkid /usr/sbin
copy_exec /usr/bin/udevadm /usr/bin
copy_exec /bin/mount /bin
exit 0
ITOOLS
chmod +x /etc/initramfs-tools/hooks/lingos-tools
fi
chmod +x /etc/initramfs-tools/scripts/init-premount/lingos-report
mkdir -p /etc/initramfs-tools/scripts/init-bottom
if [ ! -f /etc/initramfs-tools/scripts/init-bottom/lingos-bottom ]; then
cat > /etc/initramfs-tools/scripts/init-bottom/lingos-bottom <<'IBOTTOM'
#!/bin/sh
PREREQ=""
prereqs() { echo "$PREREQ"; }
case "$1" in prereqs) prereqs; exit 0 ;; esac
printf '<6>LINGOS-INITRAMFS-BOTTOM\n' > /dev/kmsg 2>/dev/null || true
for c in /dev/ttyAMA0 /dev/ttyS0; do
    [ -e "$c" ] && printf 'LINGOS-INITRAMFS-BOTTOM\n' > "$c" 2>/dev/null || true
done
exit 0
IBOTTOM
fi
chmod +x /etc/initramfs-tools/scripts/init-bottom/lingos-bottom
# 显式保证 live-boot 所需模块进入 initrd（MODULES=most 之外再兜底）
for m in squashfs loop overlay sr_mod sd_mod usb-storage uas isofs virtio_blk virtio_pci; do
    grep -qx "$m" /etc/initramfs-tools/modules 2>/dev/null || echo "$m" >> /etc/initramfs-tools/modules
done
if grep -q '^MODULES=' /etc/initramfs-tools/initramfs.conf 2>/dev/null; then
    sed -i 's/^MODULES=.*/MODULES=most/' /etc/initramfs-tools/initramfs.conf
else
    echo 'MODULES=most' >> /etc/initramfs-tools/initramfs.conf
fi
update-initramfs -u -k all >/dev/null 2>&1 || echo "!! update-initramfs 失败（继续构建）"
echo "LINGOS live root prepared"
HOOK
chmod +x config/hooks/live/0100-lingos.hook.chroot

echo "==> lb build（10-30 分钟，需网络）"
lb build

# ============================================================
# arm64：live-build 无 bootloader 支持 → 用 chroot 生成 squashfs +
# grub-mkrescue 重建带 UEFI El Torito 的可启动 ISO
# ============================================================
if [ "$DEBARCH" = "arm64" ]; then
    echo "==> arm64: live-build 无 bootloader；从 chroot 生成 squashfs + grub-mkrescue 重制 UEFI ISO"
    BIN_DIR="$WORK/binary"
    CHROOT_DIR="$WORK/chroot"
    rm -f "$CHROOT_DIR/filesystem.squashfs"
    mkdir -p "$BIN_DIR/live"
    KERNEL=$(ls "$CHROOT_DIR"/boot/vmlinuz-* 2>/dev/null | head -1 || true)
    INITRD=$(ls "$CHROOT_DIR"/boot/initrd.img-* 2>/dev/null | head -1 || true)
    if [ -z "$KERNEL" ] || [ -z "$INITRD" ]; then
        echo "错误：chroot/boot 缺少 vmlinuz/initrd" >&2
        exit 1
    fi
    # Ubuntu arm64 的 /boot/vmlinuz-* 是 gzip 压缩；GRUB arm64 linux 需要 PE/EFI-stub 内核
    if file -L "$KERNEL" | grep -qi gzip; then
        gunzip -c "$KERNEL" > "$BIN_DIR/live/vmlinuz"
        echo "==> kernel 已解压为 EFI-stub Image: $(file -L "$BIN_DIR/live/vmlinuz" | cut -c1-80)"
    else
        cp -f "$KERNEL" "$BIN_DIR/live/vmlinuz"
    fi
    cp -f "$INITRD" "$BIN_DIR/live/initrd.img"
    echo "==> mksquashfs chroot -> binary/live/filesystem.squashfs（xz，排除缓存/文档；保留 /boot）"
    mksquashfs "$CHROOT_DIR" "$BIN_DIR/live/filesystem.squashfs" -comp xz -noappend \
        -e proc sys dev run tmp var/cache/apt var/lib/apt/lists var/log usr/share/doc usr/share/man filesystem.squashfs >/dev/null
    mkdir -p "$BIN_DIR/boot/grub"
    cat > "$BIN_DIR/boot/grub/grub.cfg" <<GRUBCFG
set default=0
set timeout=10
menuentry "LING OS 安装（自动，默认）" {
    linux /live/vmlinuz boot=live components live-media=/dev/sr0 $KCONSOLE loglevel=5 video=1024x768@60 lingos.mode=install-auto
    initrd /live/initrd.img
}
menuentry "LING OS 安装（手动选择磁盘）" {
    linux /live/vmlinuz boot=live components live-media=/dev/sr0 $KCONSOLE loglevel=5 video=1024x768@60 lingos.mode=install-manual
    initrd /live/initrd.img
}
menuentry "进入 LING OS Live（不安装，可进系统）" {
    linux /live/vmlinuz boot=live components live-media=/dev/sr0 $KCONSOLE loglevel=5 video=1024x768@60 lingos.mode=live
    initrd /live/initrd.img
}
menuentry "诊断模式（收集日志 / 救援 shell）" {
    linux /live/vmlinuz boot=live components live-media=/dev/sr0 $KCONSOLE loglevel=5 video=1024x768@60 lingos.mode=install-auto-usb
}
menuentry "诊断模式（收集日志 / 救援 shell，自动扫描介质）" {
    linux /live/vmlinuz boot=live components $KCONSOLE loglevel=7 debug lingos.mode=diag
    initrd /live/initrd.img
}
GRUBCFG
    grub-mkrescue --modules="part_gpt part_msdos fat iso9660 normal linux search search_fs_file search_fs_uuid search_label configfile echo serial terminal all_video" \
        -o "$OUT/lingos-installer-$ARCH.iso" "$BIN_DIR" -- -volid LINGOS
    if ! xorriso -indev "$OUT/lingos-installer-$ARCH.iso" -report_el_torito plain 2>/dev/null | grep -q "UEFI"; then
        echo "错误：grub-mkrescue 产物没有 UEFI El Torito 引导项" >&2
        exit 1
    fi
    ls -lh "$OUT/lingos-installer-$ARCH.iso"
    rm -rf "$WORK"
    echo "OK: $OUT/lingos-installer-$ARCH.iso (grub-mkrescue)"
    exit 0
fi

# x86_64：live-build 自带 bootloader
ISO=$(ls -1 *.hybrid.iso 2>/dev/null | head -1 || true)
if [ -z "$ISO" ]; then ISO=$(ls -1 *.iso 2>/dev/null | head -1 || true); fi
if [ -z "$ISO" ]; then echo "构建失败：未找到 ISO 产物" >&2; exit 1; fi
cp -v "$ISO" "$OUT/lingos-installer-$ARCH.iso"
rm -rf "$WORK"
echo "OK: $OUT/lingos-installer-$ARCH.iso"