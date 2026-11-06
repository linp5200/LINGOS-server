#!/usr/bin/env bash
# 构建 LING OS 虚拟机镜像（qcow2）——需 root + 网络（debootstrap 取基础包）
# 用法：sudo bash distro/images/build-vm-qcow2.sh [输出目录]
# 说明：本脚本为可执行骨架；首次运行会拉取 Ubuntu base/kernel，约 10-20 分钟。
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$1
if [ -z "$OUT" ]; then OUT=$ROOT/dist/images; fi
ARCH=$(uname -m)
case "$ARCH" in
  x86_64|amd64) DEBARCH=amd64 ;;
  aarch64|arm64) DEBARCH=arm64 ;;
  *) echo "unsupported arch $ARCH" >&2; exit 1 ;;
esac
BUNDLE=$(ls "$ROOT"/dist/*allbin*.tar.gz 2>/dev/null | head -1)
if [ -z "$BUNDLE" ]; then
  echo "缺少 dist/*allbin*.tar.gz——先 make + scripts/bundle.sh dist" >&2
  exit 1
fi
WORK=$(mktemp -d)
ROOTFS=$WORK/rootfs
IMG=$OUT/lingos-vm-$DEBARCH.qcow2
mkdir -p "$OUT" "$ROOTFS"

echo "==> debootstrap（$DEBARCH / Ubuntu noble）"
debootstrap --arch="$DEBARCH" --variant=minbase noble "$ROOTFS" http://ports.ubuntu.com/ubuntu-ports 2>/dev/null || \
debootstrap --arch="$DEBARCH" --variant=minbase noble "$ROOTFS" http://archive.ubuntu.com/ubuntu

echo "==> 安装内核/引导/首次启动组件"
mount --bind /dev "$ROOTFS/dev"
mount --bind /proc "$ROOTFS/proc"
mount --bind /sys "$ROOTFS/sys"
chroot "$ROOTFS" /bin/bash -c "apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends linux-image-generic grub-efi-amd64-bin grub-pc-bin cloud-init openssh-server sudo curl && apt-get clean"
umount "$ROOTFS/sys" "$ROOTFS/proc" "$ROOTFS/dev"

echo "==> 装入 LING OS 全捆包"
mkdir -p "$ROOTFS/tmp/lingos_bundle"
tar xzf "$BUNDLE" -C "$ROOTFS/tmp/lingos_bundle" --strip-components=1
chroot "$ROOTFS" /bin/bash -c "bash /tmp/lingos_bundle/install.sh /LINGOS"

echo "==> 首次启动服务（systemd unit 模板见 distro/images/systemd/）"
cp -a "$ROOT" "$ROOTFS/tmp/lingos_repo" 2>/dev/null || true
if [ -d "$ROOTFS/tmp/lingos_repo/distro/images/systemd" ]; then
  cp -a "$ROOTFS/tmp/lingos_repo/distro/images/systemd/"*.service "$ROOTFS/etc/systemd/system/" 2>/dev/null || true
  chroot "$ROOTFS" systemctl enable lingos.service 2>/dev/null || true
fi
rm -rf "$ROOTFS/tmp/lingos_repo" "$ROOTFS/tmp/lingos_bundle"

echo "==> 生成磁盘（EFI + ext4 root）并转 qcow2"
RAW=$WORK/lingos.raw
truncate -s 8G "$RAW"
mkfs.ext4 -F "$RAW" >/dev/null
mkdir -p "$WORK/mnt"
mount -o loop "$RAW" "$WORK/mnt"
cp -a "$ROOTFS/." "$WORK/mnt/"
umount "$WORK/mnt"
qemu-img convert -f raw -O qcow2 "$RAW" "$IMG"
rm -rf "$WORK"
echo "OK: $IMG"
echo "运行：qemu-system-$ARCH -m 4096 -smp 4 -drive file=$IMG,if=virtio -netdev user,id=n0,hostfwd=tcp::18080-:8080,hostfwd=tcp::12939-:2939 -device virtio-net-pci,netdev=n0"
