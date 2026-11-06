#!/data/data/com.termux/files/usr/bin/bash
# LING OS 0.13 Termux 安装器（无需 root）
# 用法：bash install-termux.sh <LINGOS_rootfs_arm64_0.13.0.tar.zst | URL>
set -euo pipefail
SRC="${1:-}"
[ -n "$SRC" ] || { echo "用法：bash install-termux.sh <rootfs.tar.zst|URL>"; exit 2; }
command -v proot >/dev/null 2>&1 || { echo "安装 proot..."; pkg install -y proot; }
command -v zstd >/dev/null 2>&1 || { echo "安装 zstd..."; pkg install -y zstd; }
command -v curl >/dev/null 2>&1 || pkg install -y curl
BASE="${PREFIX:-$HOME/../usr}/var/lib/lingos"
ROOTFS="$BASE/rootfs"
mkdir -p "$BASE"
TMP="$(mktemp -d "$BASE/tmp.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
if echo "$SRC" | grep -qE '^https?://'; then
  echo "下载 rootfs..."
  curl -fL --retry 2 -o "$TMP/rootfs.tar.zst" "$SRC"
else
  cp -f "$SRC" "$TMP/rootfs.tar.zst"
fi
if [ -f "$SRC.sha256" ] || [ -f "$TMP/rootfs.tar.zst.sha256" ]; then
  SUMFILE="$SRC.sha256"; [ -f "$SUMFILE" ] || SUMFILE="$TMP/rootfs.tar.zst.sha256"
  EXPECTED="$(awk '{print $1}' "$SUMFILE" | head -1)"
  ACTUAL="$(sha256sum "$TMP/rootfs.tar.zst" | awk '{print $1}')"
  [ -n "$EXPECTED" ] && [ "$EXPECTED" = "$ACTUAL" ] || { echo "sha256 校验失败"; exit 3; }
fi
mkdir -p "$TMP/new"
echo "解压 rootfs..."
zstd -d -c "$TMP/rootfs.tar.zst" | tar -C "$TMP/new" -xf -
[ -x "$TMP/new/LINGOS/start-web.sh" ] || { echo "rootfs 内容不完整：缺 /LINGOS/start-web.sh"; exit 4; }
rm -rf "$ROOTFS.old"
[ -d "$ROOTFS" ] && mv "$ROOTFS" "$ROOTFS.old"
mv "$TMP/new" "$ROOTFS"

mkdir -p "$PREFIX/bin"
cat > "$PREFIX/bin/lingos" <<'EOF'
#!/data/data/com.termux/files/usr/bin/sh
set -u
BASE="${PREFIX:-$HOME/../usr}/var/lib/lingos"
ROOTFS="${LINGOS_ROOTFS:-$BASE/rootfs}"
LAN="${LINGOS_LAN:-0}"
[ "${1:-}" = "--lan" ] && LAN=1
[ -d "$ROOTFS/LINGOS" ] || { echo "rootfs 未安装：$ROOTFS"; exit 2; }
if command -v termux-wake-lock >/dev/null 2>&1; then
  timeout 3 termux-wake-lock >/dev/null 2>&1 || true
fi
EXTRA=""
[ -d /sdcard ] && EXTRA="$EXTRA -b /sdcard"
echo "LING OS: http://127.0.0.1:8080  (LAN=$LAN)"
if command -v termux-notification >/dev/null 2>&1; then
  timeout 3 termux-notification --title "LING OS" --content "Web 桌面已启动：http://127.0.0.1:8080" >/dev/null 2>&1 || true
fi
proot --link2symlink -0 -r "$ROOTFS" \
  -b /dev -b /proc -b /sys $EXTRA \
  -w /LINGOS \
  /usr/bin/env -i HOME=/root PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  TERM=xterm-256color LANG=C.UTF-8 LINGOS_LAN="$LAN" \
  /bin/bash -lc '/LINGOS/start-web.sh'
rc=$?
if command -v termux-wake-unlock >/dev/null 2>&1; then
  timeout 3 termux-wake-unlock >/dev/null 2>&1 || true
fi
exit $rc
EOF
chmod +x "$PREFIX/bin/lingos"
rm -rf "$ROOTFS.old" "$TMP/new"
echo "完成。启动：lingos          （仅本机 127.0.0.1:8080）"
echo "      局域网：lingos --lan（首次登录使用终端打印的配对码）"
echo "提示：Android 设置里允许 Termux 后台运行；已调用 termux-wake-lock。"
