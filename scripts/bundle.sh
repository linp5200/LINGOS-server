#!/usr/bin/env bash
# ============================================================
# LING OS 打包脚本（0.4.3——先生裁决命名规范）
# 用法: ./scripts/bundle.sh [输出目录]
# 依赖: make 编译产物（lingos_linux lingosd lingos_supervisor）+ python3-venv
# 产出(新命名规范 2026-09-04 先生定)：
#   LINGOS_server_linux_v<发行版>_<arch>_allbin.tar.gz   ← 全捆（lib/venv/共享资源）
#   LINGOS_server_linux_v<发行版>_<arch>_sysbin.tar.gz   ← 仅系统二进制（依赖用户自装）
# allbin 结构:
#   ├── lingos_linux / lingosd / lingos_supervisor  (rpath=$ORIGIN/../lib)
#   ├── lib/          全捆 .so（ldd 递归——过滤 glibc 白名单）+ manifest.json
#   ├── python/       venv --copies（含解释器副本 + 全部第三方库）
#   ├── share/webui/  Web UI（0.4.3——浏览器 /ui 访问）
#   └── bin/          espeak-ng/piper 等随包命令（可选）
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/dist}"
# 【先生裁决 2026-08-14/09-04】发行版本号独立于内部版本：默认 0.4.3（CI 用 RELEASE_VER 覆盖）
VER="${RELEASE_VER:-0.4.3}"
ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64) ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
esac
# 【先生裁决 2026-09-04】新命名规范：LINGOS_<server|app>_<系统>_v<版本>_<架构>_<plugin|allbin|sysbin>
PKG="LINGOS_server_linux_v${VER}_${ARCH}_allbin"
DEST="$OUT/$PKG"

echo "==> 打包: $PKG  (内部版本: $(grep -oP 'VERSION = "\K[^"]+' "$ROOT/Makefile" | head -1))"
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
        requests websocket-client tiktoken numpy pillow pytesseract 2>/dev/null || \
    "$DEST/python/bin/pip" install --no-cache-dir -q requests websocket-client tiktoken pillow pytesseract 2>/dev/null || \
    "$DEST/python/bin/pip" install --no-cache-dir -q requests websocket-client
    # 【0.2.2 vision】OCR/视觉可选依赖（体积不敏感裁决——失败不阻塞主包）
    "$DEST/python/bin/pip" install --no-cache-dir -q paddleocr paddlepaddle 2>/dev/null || true
    "$DEST/python/bin/pip" install --no-cache-dir -q opencv-python-headless 2>/dev/null || true
    # 服务端脚本复制进 venv（可执行入口）
    mkdir -p "$DEST/python/server"
    cp -a "$ROOT"/src/python/*.py "$DEST/python/server/" 2>/dev/null || true
    echo "==> venv 完成（含 requests/websocket-client/OCR/视觉依赖）"
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

# ---------- 6. 启动包装（LD_LIBRARY_PATH 兜底 dlopen 场景 + 包根导出） ----------
cat > "$DEST/start.sh" <<'EOF'
#!/usr/bin/env bash
# 启动包装（0.4.3：数据根统一 /LINGOS——先生架构；包内仅 bin/lib/venv）
# 用法：本文件应在 /LINGOS/（install.sh 已迁移）或全捆目录内执行
DIR="$(cd "$(dirname "$0")" && pwd)"
# 【0.4.3 修复】二进制位置自适应：install.sh 迁到 /LINGOS 后放 bin/；包根直跑时在 DIR/
if [ -x "$DIR/bin/lingos_linux" ]; then
    BIN="$DIR/bin/lingos_linux"
    LIB="$DIR/lib"
elif [ -x "$DIR/lingos_linux" ]; then
    BIN="$DIR/lingos_linux"
    LIB="$DIR/lib"
else
    echo "lingos_linux not found (期望 $DIR/bin/ 或 $DIR/)" >&2
    exit 1
fi
export LD_LIBRARY_PATH="$LIB:${LD_LIBRARY_PATH:-}"
export LINGOS_BUNDLED=1
# 【0.4.3 先生裁决】数据根 = /LINGOS（config/state/data/models 全在这——沿用老配置）
# 不设 LINGOS_ROOT（代码默认 /LINGOS）；venv 位置显式给 active_repair 用
export LINGOS_VENV="$DIR/python"
exec "$BIN" "$@"
EOF
chmod +x "$DEST/start.sh"

# ---------- 6c. 安装脚本（0.4.3——先生 B 方案：解压→迁移到 /LINGOS 全家桶） ----------
cat > "$DEST/install.sh" <<'EOF'
#!/usr/bin/env bash
# LING OS 0.4.3 安装脚本——将全捆包内容迁移到 /LINGOS 统一根（先生架构）
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET="${1:-/LINGOS}"
echo "==> LING OS 0.4.3 安装到 $TARGET"
[ "$(id -u)" = 0 ] || echo "!! 建议 root 运行（proot 内一般已是）"
# 1) 创建目录骨架
mkdir -p "$TARGET"/{bin,lib,run,state,data,log,system/config,share/webui,registry,plugins,models,skills,Ensystem,snapshots,repairs,Dump,backups,cache,AH}
echo "==> 目录骨架已建: $TARGET"
# 2) 复制二进制/库/venv/webui（保留可执行位）
cp -a "$DIR"/lingos_linux "$DIR"/lingosd "$DIR"/lingos_supervisor "$TARGET/bin/" 2>/dev/null || true
cp -a "$DIR"/lib/* "$TARGET/lib/" 2>/dev/null || true
[ -d "$DIR/python" ] && cp -a "$DIR/python" "$TARGET/python" 2>/dev/null || true
[ -d "$DIR/share/webui" ] && cp -a "$DIR"/share/webui/* "$TARGET/share/webui/" 2>/dev/null || true
# 3) 放启动脚本
cp -a "$DIR/start.sh" "$TARGET/start.sh" 2>/dev/null || true
chmod +x "$TARGET/bin/"* "$TARGET/start.sh" 2>/dev/null || true
# 4) 若目标已是 /LINGOS 且存在老 config/state——保留（沿用）
if [ -f "$TARGET/system/config/state.json" ]; then
  echo "==> 检测到已有配置 ($TARGET/system/config)——沿用，不覆盖"
fi
echo "=========================================="
echo "✅ 安装完成: $TARGET"
echo "   启动: cd '$TARGET' && ./start.sh"
echo "   Web UI: http://<host>:8080/ui"
echo "=========================================="
EOF
chmod +x "$DEST/install.sh"

# ---------- 6b. Web UI（0.4.3——网页访问 http://host:8080/ui） ----------
if [ -d "$ROOT/webui" ]; then
    mkdir -p "$DEST/share/webui"
    cp -a "$ROOT"/webui/* "$DEST/share/webui/" 2>/dev/null || true
    echo "==> Web UI 已装入 share/webui/（浏览器访问 /ui）"
fi

# ---------- 7b. sysbin 包（0.4.3——仅系统二进制，依赖用户自装；先生裁决三包型） ----------
SYS_PKG="LINGOS_server_linux_v${VER}_${ARCH}_sysbin"
SYS_DEST="$OUT/$SYS_PKG"
mkdir -p "$SYS_DEST" "$SYS_DEST/share/webui"
for bin in lingos_linux lingosd lingos_supervisor; do
    [ -f "$DEST/$bin" ] && cp -a "$DEST/$bin" "$SYS_DEST/"
done
cp -a "$ROOT"/src/python/*.py "$SYS_DEST/" 2>/dev/null || true
# 【0.4.3】Web UI 随 sysbin（网页访问 http://host:8080/ui——先生重点要求）
cp -a "$ROOT"/webui/* "$SYS_DEST/share/webui/" 2>/dev/null || true
cat > "$SYS_DEST/README.txt" <<'EOF'
LING OS sysbin 包（仅系统二进制 + Python 脚本 + Web UI）
依赖（用户自行安装）：libcurl libseccomp libsqlite3 libmosquitto libmicrohttpd libnotcurses(可选)
python3 + requests/websocket-client；glibc >= 2.35
部署：解压到目标目录，参考 DEPENDENCIES.md 安装依赖后 ./lingos_linux
Web UI：浏览器访问 http://<host>:8080/ui（本包已含 share/webui）
EOF
( cd "$OUT" && tar czf "$SYS_PKG.tar.gz" "$SYS_PKG" && rm -rf "$SYS_PKG" )
echo "✅ sysbin 包: $OUT/$SYS_PKG.tar.gz"

# ---------- 7d. plugin 包（0.4.3——先生三包型裁决：非基础功能插件集，可增删） ----------
PLG_PKG="LINGOS_server_linux_v${VER}_${ARCH}_plugin"
PLG_DEST="$OUT/$PLG_PKG"
mkdir -p "$PLG_DEST/plugins/python" "$PLG_DEST/share/webui"
# 扩展服务插件（非基础功能——核心不含时按需装入 /LINGOS 对应目录启用）
cp -a "$ROOT"/src/python/rtsp_streamer.py "$ROOT"/src/python/ocr_service.py \
      "$ROOT"/src/python/calibration_service.py "$ROOT"/src/python/yolo_service.py \
      "$ROOT"/src/python/vision_ai.py "$ROOT"/src/python/vision_train.py \
      "$ROOT"/src/python/ha_integration.py "$ROOT"/src/python/voice_service.py \
      "$ROOT"/src/python/plugin/*.py "$PLG_DEST/plugins/python/" 2>/dev/null || true
cp -a "$ROOT"/webui/* "$PLG_DEST/share/webui/" 2>/dev/null || true
cat > "$PLG_DEST/README.txt" <<'EOF'
LING OS plugin 包（系统插件集——先生 2026-09-04 三包型裁决）
含扩展服务插件（python）：视觉(rtsp/ocr/标定/yolo/vision_ai/vision_train)、HA 桥、语音、Web UI
安装：放入宿主 /LINGOS/plugins/（或 registry 注册）后由系统插件管理器加载/热重载
适用范围：LINGOS_server_linux 0.4.3（插件支持的系统版本范围内可插入）
非基础功能——可按需删减/扩展
EOF
( cd "$OUT" && tar czf "$PLG_PKG.tar.gz" "$PLG_PKG" && rm -rf "$PLG_PKG" )
echo "✅ plugin 包: $OUT/$PLG_PKG.tar.gz"

# ---------- 7c. 压缩（allbin） ----------
cd "$OUT"
tar czf "$PKG.tar.gz" "$PKG"
echo "=========================================="
echo "✅ 打包完成: $OUT/$PKG.tar.gz"
echo "   体积: $(du -h "$PKG.tar.gz" | cut -f1)"
echo "   部署: tar xzf $PKG.tar.gz && cd $PKG && ./start.sh"
echo "=========================================="
