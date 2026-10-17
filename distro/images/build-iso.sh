#!/usr/bin/env bash
# 构建 LING OS 安装 ISO（骨架：基于系统上已安装的 live-build）
# 用法：sudo bash distro/images/build-iso.sh [输出目录]
# 说明：live-build 默认面向 Debian；Ubuntu 可用 --distribution noble（需源可用）。
# 首次运行需网络下载基础包；产物可 UEFI+BIOS 启动，包含"试用/安装"入口。
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$1
if [ -z "$OUT" ]; then OUT=$ROOT/dist/images; fi
BUNDLE=$(ls "$ROOT"/dist/*allbin*.tar.gz 2>/dev/null | head -1)
if [ -z "$BUNDLE" ]; then echo "缺少 dist/*allbin*.tar.gz" >&2; exit 1; fi
if ! command -v lb >/dev/null 2>&1; then echo "需要 live-build：apt-get install live-build" >&2; exit 1; fi
mkdir -p "$OUT"
WORK=$(mktemp -d)
cd "$WORK"
lb config \
  --distribution noble \
  --architecture amd64 \
  --binary-images iso-hybrid \
  --bootloader grub-efi \
  --apt-recommends false \
  --memtest none \
  --iso-application "LING OS" \
  --iso-volume "LINGOS"
mkdir -p config/includes.chroot/LINGOS
echo "==> 提示：live-build 的 Ubuntu 源可能需要手动指定镜像；首次运行请检查 config/archives/"
# 安装包清单：内核/桌面基础/运行依赖（可按需增删）
mkdir -p config/package-lists
cat > config/package-lists/lingos.list.chroot <<'EOF'
linux-image-generic
grub-efi-amd64
systemd-sysv
network-manager
pipewire
python3
python3-requests
libcurl4
libseccomp2
libsqlite3-0
libmosquitto1
libmicrohttpd12
libssl3
espeak-ng
ffmpeg
curl
EOF
# 安装 LING OS 全捆包（chroot hook）
mkdir -p config/hooks/live
cat > config/hooks/live/0100-lingos-bundle.hook.chroot <<HOOK
#!/bin/sh
set -e
mkdir -p /tmp/lingos_bundle
tar xzf /LINGOS/offline/lingos_bundle.tar.gz -C /tmp/lingos_bundle --strip-components=1
bash /tmp/lingos_bundle/install.sh /LINGOS
rm -rf /tmp/lingos_bundle
HOOK
chmod +x config/hooks/live/0100-lingos-bundle.hook.chroot
mkdir -p config/includes.chroot/LINGOS/offline
cp "$BUNDLE" config/includes.chroot/LINGOS/offline/lingos_bundle.tar.gz
echo "==> lb build（10-20 分钟，需网络）"
lb build
cp -v live-image-*.hybrid.iso "$OUT/lingos-installer.iso" 2>/dev/null || cp -v *.iso "$OUT/lingos-installer.iso" 2>/dev/null || true
rm -rf "$WORK"
echo "OK: $OUT/lingos-installer.iso（安装器/分区方案见 docs/IMAGES.md）"
