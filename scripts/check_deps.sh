#!/usr/bin/env bash
# LING OS 0.4.3 依赖自检与安装（先生 2026-09-05 裁决：检测缺失→自动安装/提示）
# 覆盖：语音(espeak-ng/piper) / 监控快照与录像(ffmpeg) / python 依赖(requests)
# 用法: ./check_deps.sh [--install]   加 --install 自动 apt 安装缺失项
set -u
INSTALL=0
[ "${1:-}" = "--install" ] && INSTALL=1
HAVE_APT=0; command -v apt >/dev/null 2>&1 && HAVE_APT=1
have(){ command -v "$1" >/dev/null 2>&1; }
echo "== LING OS 依赖自检 (LN-0.4.3) =="

check_install(){
  local name="$1" pkg="$2"
  if have "$name"; then echo "  ✓ $name 已装"
  elif [ "$INSTALL" = 1 ] && [ "$HAVE_APT" = 1 ]; then
    echo "  … $name 缺失——apt 安装 $pkg …"
    apt-get install -y "$pkg" >/dev/null 2>&1 && echo "  ✓ $name 已装" || echo "  ✗ $name 安装失败(需手动: apt install $pkg)"
  else echo "  ⚠ $name 缺失——(apt install $pkg)"; fi
}

# 语音 TTS/STT（先生：装进系统——缺失自检装）
check_install espeak-ng espeak-ng
check_install ffmpeg ffmpeg
# python3 + requests（ai_server 必需）
if have python3; then
  python3 -c "import requests" >/dev/null 2>&1 && echo "  ✓ python3+requests" \
    || { echo "  ⚠ requests 缺失——(pip3 install requests 或 apt install python3-requests)"; }
else echo "  ⚠ python3 缺失——(apt install python3)"; fi

echo "== 自检完成 =="
