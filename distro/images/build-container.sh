#!/usr/bin/env bash
# 构建 LING OS 服务端容器镜像（本地/CI 通用）
# 前置：make（或已有 dist/ 全捆包）+ docker 可用
# 用法：bash distro/images/build-container.sh [镜像tag]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TAG=$1
if [ -z "$TAG" ]; then TAG=lingos-server:dev; fi

BUNDLE=$(ls "$ROOT"/dist/*allbin*.tar.gz 2>/dev/null | head -1)
if [ -z "$BUNDLE" ]; then
    echo "缺少 dist/*allbin*.tar.gz——先执行：RELEASE_VER=0.9.0 bash scripts/bundle.sh dist" >&2
    exit 1
fi
cp -f "$BUNDLE" "$ROOT/dist/lingos-container-bundle.tar.gz"

cd "$ROOT"
docker build -f distro/images/Dockerfile -t "$TAG" .
echo "OK: $TAG（docker run -p 8080:8080 -p 2939:2939 -v lingos-data:/LINGOS/data $TAG）"
