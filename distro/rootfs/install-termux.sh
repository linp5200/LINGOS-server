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
[ -d "$ROOTFS/LINGOS" ] || { echo "rootfs 未安装：$ROOTFS"; exit 2; }
MODE="${1:-web}"
LAN="${LINGOS_LAN:-0}"
case "$MODE" in
  --lan) LAN=1; MODE=web ;;
  --proot) MODE=proot ;;
esac
[ "${2:-}" = "--lan" ] && LAN=1
if command -v termux-wake-lock >/dev/null 2>&1; then
  timeout 3 termux-wake-lock >/dev/null 2>&1 || true
fi
if command -v termux-notification >/dev/null 2>&1; then
  timeout 3 termux-notification --title "LING OS" --content "Web 桌面已启动：http://127.0.0.1:8080" >/dev/null 2>&1 || true
fi

if [ "$MODE" = "proot" ]; then
  EXTRA=""
  [ -d /sdcard ] && EXTRA="$EXTRA -b /sdcard"
  echo "LING OS (proot): http://127.0.0.1:8080  (LAN=$LAN)"
  # termux-exec 的 LD_PRELOAD 会让 proot 返回 ENOSYS；启动前清除。
  unset LD_PRELOAD
  export PROOT_NO_SECCOMP=1
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
fi

# 默认：Termux 原生 Python 直跑 Web 桌面，不依赖 proot。
# 适用于 Android 14+/16 上 proot 被 seccomp/内核限制的设备。
echo "LING OS: http://127.0.0.1:8080  (LAN=$LAN)"
PY="${LINGOS_PYTHON:-${PREFIX:-/data/data/com.termux/files/usr}/bin/python3}"
[ -x "$PY" ] || PY="$(command -v python3 || echo python3)"
DATA="${LINGOS_DATA:-$HOME/.lingos/data}"
mkdir -p "$DATA" || { echo "无法创建数据目录：$DATA"; exit 3; }
if [ "$LAN" = "1" ]; then
  exec "$PY" "$ROOTFS/LINGOS/services/lingos_web/server.py" --root "$DATA" --web "$ROOTFS/LINGOS/webdesk" --lan
fi
exec "$PY" "$ROOTFS/LINGOS/services/lingos_web/server.py" --root "$DATA" --web "$ROOTFS/LINGOS/webdesk"
EOF
chmod +x "$PREFIX/bin/lingos"
rm -rf "$ROOTFS.old" "$TMP/new"
echo "完成。启动：lingos          （仅本机 127.0.0.1:8080）"
echo "      局域网：lingos --lan（首次登录使用终端打印的配对码）"
echo "提示：Android 设置里允许 Termux 后台运行；已调用 termux-wake-lock。"
