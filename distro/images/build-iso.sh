#!/usr/bin/env bash
# ============================================================
# LING OS 安装 ISO 构建（Ubuntu noble live-build，UEFI+BIOS hybrid）
# 用法：sudo bash distro/images/build-iso.sh [x86_64|aarch64] [发行版代号]
# 前置：dist/*allbin*.tar.gz 存在；root；网络（拉基础包）
# 产物：dist/images/lingos-installer-<arch>.iso
# ============================================================
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ARCHARG=$1
if [ -z "$ARCHARG" ]; then ARCHARG=$(uname -m); fi
DIST=""
if [ "$#" -ge 2 ]; then DIST=$2; fi
if [ -z "$DIST" ]; then DIST=noble; fi
BINIMG=iso
case "$ARCHARG" in
    x86_64|amd64)  ARCH=x86_64;  DEBARCH=amd64; GRUBPKG=grub-efi-amd64-bin; GRUBTARGET=x86_64-efi; MIRROR=http://archive.ubuntu.com/ubuntu;;
    aarch64|arm64) ARCH=aarch64; DEBARCH=arm64; GRUBPKG=grub-efi-arm64-bin; GRUBTARGET=arm64-efi;  MIRROR=http://ports.ubuntu.com/ubuntu-ports ;;
    *) echo "unsupported arch: $ARCHARG" >&2; exit 1 ;;
esac
OUT=$ROOT/dist/images
mkdir -p "$OUT"
BUNDLE=$(ls "$ROOT"/dist/*allbin*.tar.gz 2>/dev/null | head -1 || true)
if [ -z "$BUNDLE" ]; then
    echo "缺少 dist/*allbin*.tar.gz —— 先 make + RELEASE_VER=0.9.0 bash scripts/bundle.sh dist" >&2
    exit 1
fi
if ! command -v lb >/dev/null 2>&1; then
    echo "需要 live-build：apt-get install -y live-build debootstrap squashfs-tools xorriso mtools dosfstools rsync syslinux-utils isolinux" >&2
    exit 1
fi

WORK=$(mktemp -d /tmp/lingos-iso-XXXXXX)
echo "==> live-build 工作目录: $WORK (dist=$DIST arch=$DEBARCH)"
cd "$WORK"

lb config \
  --mode ubuntu \
  --distribution "$DIST" \
  --architecture "$DEBARCH" \
  --binary-images "$BINIMG" \
  --bootloader grub-efi \
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
  --bootappend-live "boot=live components quiet splash console=tty0 console=ttyAMA0,115200 lingos.autostart=1" \
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
grub2-common
grub-efi-amd64-bin
grub-efi-amd64-signed
grub-pc-bin
syslinux
syslinux-utils
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

# ---------- 安装器 / 服务单元 / 全捆包 ----------
mkdir -p config/includes.chroot/usr/local/sbin \
         config/includes.chroot/etc/systemd/system \
         config/includes.chroot/LINGOS/offline \
         config/hooks/live
cp "$ROOT/distro/images/live/lingos-install.sh" config/includes.chroot/usr/local/sbin/lingos-install
chmod +x config/includes.chroot/usr/local/sbin/lingos-install
cp "$ROOT/distro/images/live/lingos.service"           config/includes.chroot/etc/systemd/system/lingos.service
cp "$ROOT/distro/images/live/lingos-installer.service" config/includes.chroot/etc/systemd/system/lingos-installer.service
cp "$BUNDLE" config/includes.chroot/LINGOS/offline/lingos_bundle.tar.gz

# ---------- chroot hook ----------
cat > config/hooks/live/0100-lingos.hook.chroot <<'HOOK'
#!/bin/sh
set -e
mkdir -p /tmp/lingos_bundle
tar xzf /LINGOS/offline/lingos_bundle.tar.gz -C /tmp/lingos_bundle --strip-components=1
bash /tmp/lingos_bundle/install.sh /LINGOS
rm -rf /tmp/lingos_bundle
rm -f /LINGOS/offline/lingos_bundle.tar.gz
chmod +x /usr/local/sbin/lingos-install
mkdir -p /etc/systemd/system/multi-user.target.wants
ln -sf /etc/systemd/system/lingos-installer.service /etc/systemd/system/multi-user.target.wants/lingos-installer.service
echo "LINGOS live root prepared"
HOOK
chmod +x config/hooks/live/0100-lingos.hook.chroot

echo "==> lb build（10-30 分钟，需网络）"
lb build

ISO=$(ls -1 *.hybrid.iso 2>/dev/null | head -1 || true)
if [ -z "$ISO" ]; then ISO=$(ls -1 *.iso 2>/dev/null | head -1 || true); fi
if [ -z "$ISO" ]; then echo "构建失败：未找到 ISO 产物" >&2; exit 1; fi
cp -v "$ISO" "$OUT/lingos-installer-$ARCH.iso"
rm -rf "$WORK"
echo "OK: $OUT/lingos-installer-$ARCH.iso"
