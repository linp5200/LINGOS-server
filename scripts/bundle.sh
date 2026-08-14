#!/usr/bin/env bash
# ============================================================
# LING OS 全捆打包脚本（0.2.1 定稿——先生裁决 H4：CI 出全捆包）
# 用法: ./scripts/bundle.sh [输出目录]
# 依赖: make 编译产物（lingos_linux lingosd lingos_supervisor）+ python3-venv
# 产出: LINGOS-<ver>-<arch>-linux/ 目录（解压即用）
#   ├── lingos_linux / lingosd / lingos_supervisor  (rpath=$ORIGIN/../lib)
#   ├── lib/          全捆 .so（ldd 递归——过滤 glibc 白名单）+ manifest.json
#   ├── python/       venv --copies（含解释器副本 + 全部第三方库）
#   └── bin/          espeak-ng/piper 等随包命令（可选——按需放入）
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/dist}"
VER="$(grep -oP 'VERSION = "\K[^"]+' "$ROOT/Makefile" | head -1)"
ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64) ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
esac
PKG="LINGOS-$VER-$ARCH-linux"
DEST="$OUT/$PKG"

echo "==> 打包: $PKG"
echo "==> 版本: $VER  架构: $ARCH"

rm -rf "$DEST"
mkdir -p "$DEST"/{lib,python,bin}

# ---------- 1. 复制二进制 ----------
for bin in lingos_linux lingosd lingos_supervisor; do
    if [ -x "$ROOT/$bin" ]; then
        cp -a "$ROOT/$bin" "$DEST/"
        echo "==> 复制二进制: $bin"
    else
        echo "!! 缺少 $bin（先 make）" >&2
    fi
done

# ---------- 2. 全捆动态库（ldd 递归 → 过滤 glibc 白名单） ----------
# glibc 组件永不捆绑（业界铁律——DEPENDENCIES.md 八.4）
# 含 musl 兼容（沙箱 Alpine 测试环境文件名不同）
GLIBC_SKIP="libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|ld-linux|ld-musl|libresolv\.so\.2|libnss_|librt\.so\.1|libutil\.so\.1|libgcc_s\.so\.1|libstdc\+\+\.so\.6|libc\.musl"

collect_libs() {
    local bin="$1"
    ldd "$bin" 2>/dev/null | awk '/=> \//{print $3} /^\s*\/[^ ]+\.so/{print $1}' | sort -u
}

# 已收集的库集合（去重 + 防循环依赖递归）
declare -A SEEN
LIBLIST=""

collect_recursive() {
    local bin="$1"
    for lib in $(collect_libs "$bin"); do
        local base
        base="$(basename "$lib")"
        if [[ -n "${SEEN[$base]:-}" ]]; then continue; fi
        SEEN[$base]=1
        if echo "$base" | grep -qE "$GLIBC_SKIP"; then continue; fi
        LIBLIST="$LIBLIST $lib"
        # 递归收集该库自身的依赖（库在系统目录用绝对路径；在包内用包路径）
        collect_recursive "$lib"
    done
}

for bin in lingos_linux lingosd lingos_supervisor; do
    [ -x "$DEST/$bin" ] && collect_recursive "$DEST/$bin"
done

# 复制库到 lib/（保留 SONAME 文件名）
echo "==> 捆绑动态库:"
for lib in $(echo "$LIBLIST" | tr ' ' '\n' | sort -u); do
    if [ -f "$lib" ]; then
        base="$(basename "$lib")"
        # 符号链接解析为真实文件
        real="$(readlink -f "$lib")"
        cp -a "$real" "$DEST/lib/$base" 2>/dev/null || cp -a "$lib" "$DEST/lib/$base"
        # 若为符号链接名且目标不同，同时复制目标（SONAME 链）
        if [ "$real" != "$lib" ]; then
            rbase="$(basename "$real")"
            [ -f "$DEST/lib/$rbase" ] || cp -a "$real" "$DEST/lib/$rbase"
        fi
        echo "  + $base"
    fi
done

# ---------- 3. manifest.json（自检用——check_bundled_libs 读取） ----------
python3 - "$DEST/lib" <<'PYEOF'
import json, os, sys
libdir = sys.argv[1]
libs = []
for f in sorted(os.listdir(libdir)):
    if f.endswith('.so') or '.so.' in f:
        p = os.path.join(libdir, f)
        st = os.stat(p)
        libs.append({"name": f, "size": st.st_size})
with open(os.path.join(libdir, "manifest.json"), "w") as fp:
    json.dump({"format": "lingos-bundle-v1", "libs": libs}, fp, indent=2)
print("==> manifest.json: %d 个库" % len(libs))
PYEOF

# ---------- 4. venv --copies 打包（Python 层离线） ----------
PYVER="$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
if command -v python3 >/dev/null; then
    echo "==> 构建 venv (python$PYVER --copies)"
    python3 -m venv --copies "$DEST/python"
    "$DEST/python/bin/pip" install --no-cache-dir -q \
        requests websocket-client tiktoken numpy pillow 2>/dev/null || \
    "$DEST/python/bin/pip" install --no-cache-dir -q requests websocket-client
    # 服务端脚本复制进 venv（可执行入口）
    mkdir -p "$DEST/python/server"
    cp -a "$ROOT"/src/python/*.py "$DEST/python/server/" 2>/dev/null || true
    echo "==> venv 完成（含 requests/websocket-client）"
else
    echo "!! 无 python3——跳过 venv（Python 层缺失）" >&2
fi

# ---------- 5. 环境检查脚本（降级为验证：glibc 版本 + 报缺项） ----------
cat > "$DEST/check_env.sh" <<'EOF'
#!/usr/bin/env bash
# 环境检查：全捆包仅要求宿主 glibc >= 2.35（DEPENDENCIES.md 门槛）
echo "==> LING OS 全捆包环境检查"
ldd --version 2>&1 | head -1
echo "==> 若无法运行，请确认宿主 glibc >= 2.35（Ubuntu 22.04+/Debian 12+）"
EOF
chmod +x "$DEST/check_env.sh"

# ---------- 6. 启动包装（LD_LIBRARY_PATH 兜底 dlopen 场景） ----------
cat > "$DEST/start.sh" <<'EOF'
#!/usr/bin/env bash
# 启动包装：LD_LIBRARY_PATH 兜底（numpy 等 dlopen C 扩展不走 RPATH——DEPENDENCIES.md 全捆红利）
DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR/lib:${LD_LIBRARY_PATH:-}"
export LINGOS_BUNDLED=1
exec "$DIR/lingos_linux" "$@"
EOF
chmod +x "$DEST/start.sh"

# ---------- 7. 压缩 ----------
cd "$OUT"
tar czf "$PKG.tar.gz" "$PKG"
echo "=========================================="
echo "✅ 打包完成: $OUT/$PKG.tar.gz"
echo "   体积: $(du -h "$OUT/$PKG.tar.gz" | cut -f1)"
echo "   部署: tar xzf $PKG.tar.gz && cd $PKG && ./start.sh"
echo "=========================================="
