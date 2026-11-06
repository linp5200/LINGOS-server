#!/usr/bin/env bash
# LING OS 0.13 rootfs 构建（Termux / proot 用，arm64）
# 用法：sudo bash distro/rootfs/build-rootfs.sh [输出目录]
# 依赖：arm64 Linux（CI ubuntu-24.04-arm）、mmdebstrap 或 debootstrap、make、zstd
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/dist/rootfs}"
VER="$(cat "$ROOT/VERSION" 2>/dev/null | head -1 | tr -d '[:space:]')"
[ -n "$VER" ] || VER=0.13.0
ARCH="$(uname -m)"
case "$ARCH" in
  aarch64|arm64) DEBARCH=arm64 ;;
  *) echo "错误：rootfs 构建需 arm64 主机（当前 $ARCH）。请在 CI ubuntu-24.04-arm 上构建。" >&2; exit 1 ;;
esac
command -v mmdebstrap >/dev/null 2>&1 || command -v debootstrap >/dev/null 2>&1 || {
  echo "需要 mmdebstrap 或 debootstrap：apt-get install -y mmdebstrap debootstrap" >&2; exit 1; }
command -v zstd >/dev/null 2>&1 || { echo "需要 zstd" >&2; exit 1; }
mkdir -p "$OUT"
WORK="$(mktemp -d /tmp/lingos-rootfs-XXXXXX)"
R="$WORK/rootfs"
trap 'rm -rf "$WORK"' EXIT

echo "==> 构建 Ubuntu noble $DEBARCH minbase rootfs"
PKGS="python3,python3-requests,ca-certificates,curl,rsync,libseccomp2,libsqlite3-0,libmosquitto1,libssl3,libcurl4,procps,iproute2,tzdata,locales,tar,zstd,xz-utils,util-linux,kmod"
if command -v mmdebstrap >/dev/null 2>&1; then
  MODE=unshare
  [ "$(id -u)" = "0" ] && MODE=root
  mmdebstrap --mode="$MODE" --arch="$DEBARCH" --variant=minbase \
    --include="$PKGS" noble "$R" http://ports.ubuntu.com/ubuntu-ports
else
  debootstrap --arch="$DEBARCH" --variant=minbase --include="${PKGS//,/ }" noble "$R" http://ports.ubuntu.com/ubuntu-ports
fi

echo "==> 编译 LING OS C 组件（若尚未构建）"
cd "$ROOT"
if [ ! -x lingos_linux ] || [ ! -x lingosd ]; then
  make -j"$(nproc)"
fi
for b in lingos_linux lingosd lingos_supervisor lingos_alertd lingos_visiond lingos_voiced lingos_monitor lingos_ai_vision lingosctl; do
  [ -x "$ROOT/$b" ] || { echo "缺构建产物：$b（先 make）" >&2; exit 1; }
done

echo "==> 装配 /LINGOS"
mkdir -p "$R/LINGOS"/{bin,lib,run,state,data,log,system/config,share/webui,registry,plugins,skills,models,Ensystem,snapshots,repairs,Dump,backups,cache,AH,webdesk,services/lingos_web,services/lingos_device}
cp -a "$ROOT"/lingos_linux "$ROOT"/lingosd "$ROOT"/lingos_supervisor \
      "$ROOT"/lingos_alertd "$ROOT"/lingos_visiond "$ROOT"/lingos_voiced \
      "$ROOT"/lingos_monitor "$ROOT"/lingos_ai_vision "$ROOT"/lingosctl "$R/LINGOS/bin/"
cp -a "$ROOT"/src/python/*.py "$R/LINGOS/bin/" 2>/dev/null || true
mkdir -p "$R/LINGOS/bin/plugin"
cp -a "$ROOT"/src/python/plugin/*.py "$R/LINGOS/bin/plugin/" 2>/dev/null || true
mkdir -p "$R/LINGOS/python/server/plugin"
cp -a "$ROOT"/src/python/*.py "$R/LINGOS/python/server/" 2>/dev/null || true
cp -a "$ROOT"/src/python/plugin/*.py "$R/LINGOS/python/server/plugin/" 2>/dev/null || true
cp -a "$ROOT"/webdesk/. "$R/LINGOS/webdesk/"
cp -a "$ROOT"/services/. "$R/LINGOS/services/"
rm -rf "$R/LINGOS/services/__pycache__" "$R/LINGOS/services/lingos_web/__pycache__" "$R/LINGOS/services/lingos_device/__pycache__" 2>/dev/null || true
cp -a "$ROOT"/scripts/lingos.sh "$R/LINGOS/lingos.sh"
cp -a "$ROOT"/webui/index.html "$R/LINGOS/share/webui/index.html" 2>/dev/null || true
# workspace files are 0600; make shipped assets readable.
find "$R/LINGOS/webdesk" -type f -exec chmod 644 {} + 2>/dev/null || true
find "$R/LINGOS/services" -type f -exec chmod 644 {} + 2>/dev/null || true
chmod 755 "$R/LINGOS/lingos.sh" "$R/LINGOS/start-web.sh" 2>/dev/null || true
chmod 755 "$R/LINGOS/bin/"* 2>/dev/null || true
printf '%s\n' '{"system_configured": true, "last_config_time": "1970-01-01T00:00:00Z"}' > "$R/LINGOS/system/config/state.json"
printf '%s\n' '{"schema_version":1,"privacy_level":"L1","local_only":true,"flags":{}}' > "$R/LINGOS/system/config/security-policy.json"
chmod 644 "$R/LINGOS/system/config/security-policy.json"

cat > "$R/LINGOS/start-web.sh" <<'EOF'
#!/bin/sh
# Termux/proot 启动：先起后端，再前台跑 Web 桌面（8080）。
set -u
cd /LINGOS || exit 1
mkdir -p log run
[ -x ./lingos.sh ] && ./lingos.sh start >>log/lingos-start.log 2>&1 || true
if [ "${LINGOS_LAN:-0}" = "1" ]; then
  exec python3 /LINGOS/services/lingos_web/server.py --root /LINGOS --web /LINGOS/webdesk --lan
else
  exec python3 /LINGOS/services/lingos_web/server.py --root /LINGOS --web /LINGOS/webdesk
fi
EOF
chmod +x "$R/LINGOS/start-web.sh"

# 禁用 rootfs 内的 systemd 服务启动（proot 无 systemd）
rm -f "$R/etc/systemd/system/multi-user.target.wants/"* 2>/dev/null || true

echo "==> 打包"
NAME="LINGOS_rootfs_arm64_${VER}.tar.zst"
tar -C "$R" --numeric-owner -cf - . | zstd -T0 -19 -o "$OUT/$NAME"
( cd "$OUT" && sha256sum "$NAME" > "$NAME.sha256" )
ls -lh "$OUT/$NAME" "$OUT/$NAME.sha256"
echo "OK: $OUT/$NAME"
