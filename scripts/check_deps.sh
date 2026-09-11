#!/usr/bin/env bash
# LING OS 依赖自检与安装（0.4.4）
# 先生裁决：检测缺失 → 自动安装/提示
# 覆盖：语音(espeak-ng/piper) / 监控快照与录像(ffmpeg) / python(requests/websocket-client)
#       / 运行期共享库(libldap liblber libav* libunistring 等)
# 用法: ./check_deps.sh [--install]    加 --install 自动 apt 安装缺失项
set -u
INSTALL=0
[ "${1:-}" = "--install" ] && INSTALL=1
HAVE_APT=0; command -v apt >/dev/null 2>&1 && HAVE_APT=1
HAVE_APK=0; command -v apk >/dev/null 2>&1 && HAVE_APK=1
have(){ command -v "$1" >/dev/null 2>&1; }
MISS=0
echo "== LING OS 依赖自检 (LN-0.4.4) =="

check_install(){
  local name="$1" apt_pkg="$2" apk_pkg="${3:-}"
  if have "$name"; then echo "  ✓ $name 已装"; return 0; fi
  MISS=$((MISS+1))
  if [ "$INSTALL" = 1 ] && [ "$HAVE_APT" = 1 ]; then
    echo "  … $name 缺失——apt 安装 $apt_pkg …"
    apt-get install -y "$apt_pkg" >/dev/null 2>&1 && { echo "  ✓ $name 已装"; return 0; }
  elif [ "$INSTALL" = 1 ] && [ "$HAVE_APK" = 1 ] && [ -n "$apk_pkg" ]; then
    echo "  … $name 缺失——apk 安装 $apk_pkg …"
    apk add --no-cache "$apk_pkg" >/dev/null 2>&1 && { echo "  ✓ $name 已装"; return 0; }
  fi
  echo "  ✗ $name 缺失——请装：apt install $apt_pkg"
  return 1
}

echo "-- 语音（TTS/STT）--"
check_install espeak-ng espeak-ng espeak-ng          # 【先生裁决】语音含 piper，此处补检
check_install piper     piper-tts   || true          # piper 常无 apt 包 → 见下方 pip 提示
if ! have piper; then
  echo "      ↳ piper 可 pip 安装：pip3 install --break-system-packages piper-tts"
fi

echo "-- 监控（快照/录像）--"
check_install ffmpeg ffmpeg ffmpeg

echo "-- Python 运行时 --"
if have python3; then
  echo "  ✓ python3: $(command -v python3)"
  for m in requests websocket; do
    mod="$m"; [ "$m" = "websocket" ] && mod="websocket"
    if env -u LD_LIBRARY_PATH python3 -c "import $mod" >/dev/null 2>&1; then
      echo "  ✓ python3 - $mod"
    else
      MISS=$((MISS+1))
      echo "  ✗ python3 - $mod 缺失——pip3 install --break-system-packages $([ "$m" = websocket ] && echo websocket-client || echo requests)"
    fi
  done
  # SSL 自检（0.4.4——LD 污染曾致 SSL 失效）
  if env -u LD_LIBRARY_PATH python3 -c "import ssl" >/dev/null 2>&1; then
    echo "  ✓ python3 - ssl（干净环境）"
  else
    echo "  ✗ python3 - ssl 不可用！检查 LD_LIBRARY_PATH 是否被设为包内 lib/"
    echo "      LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-(未设)}"
  fi
else
  MISS=$((MISS+1))
  echo "  ✗ python3 缺失——apt install python3 python3-requests"
fi

echo "-- 运行期共享库（本体 ldd）--"
BIN=""
for c in /LINGOS/bin/lingos_linux ./lingos_linux ../bin/lingos_linux; do
  [ -x "$c" ] && { BIN="$c"; break; }
done
if [ -n "$BIN" ]; then
  echo "  检查 $BIN"
  if command -v ldd >/dev/null 2>&1; then
    NF=$(ldd "$BIN" 2>/dev/null | awk '/not found/{print $1}')
    if [ -n "$NF" ]; then
      MISS=$((MISS+1))
      echo "  ✗ 缺库："
      echo "$NF" | sed 's/^/      /'
      echo "      ↳ Debian/Ubuntu 25.10: apt install libldap2 liblber2 libavcodec* libavformat* libswscale* libavutil* libunistring5"
      echo "      ↳ 或直接使用 allbin 包（自带全部依赖）"
    else
      echo "  ✓ 动态库齐全"
    fi
  fi
else
  echo "  （未找到 lingos_linux，跳过）"
fi

echo "== 自检完成 =="
if [ "$MISS" -gt 0 ]; then
  echo "!! 共 $MISS 项缺失/异常（详见上）"
  exit 1
fi
echo "全部就绪 ✅"
