#!/usr/bin/env bash
# ============================================================
# LING OS 主机部署引导（0.2.2——HA + MiCam 全家 Docker 编排）
# 一键部署：bash deploy_ha.sh
# 前置：Docker（proot 环境可能不可用——见下方检测与替代）
# ============================================================
set -euo pipefail

echo "=============================================="
echo " LING OS 主机部署引导（HA + 摄像头桥）"
echo "=============================================="

# ---------- 1. Docker 检测（proot 风险项） ----------
if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    echo "✅ Docker 可用"
    DOCKER_OK=1
else
    echo "⚠️ Docker 不可用——proot 环境通常无法运行 Docker daemon"
    echo "   替代方案："
    echo "   A. 在独立设备/VM 部署 Docker（树莓派/NAS/旧电脑），LING OS 连其 IP"
    echo "   B. 仅部署 HA 到已有 Docker 主机（先生环境中 Docker 不可用时）"
    echo "   C. 跳过容器部署——HA 单独跑，摄像头桥用 Python 直跑（micam 支持非 Docker）"
    echo ""
    read -rp "选择部署方式 [1=Docker全家 2=跳过容器 3=仅HA] (默认 1): " MODE
    MODE="${MODE:-1}"
    if [ "$MODE" != "1" ]; then
        echo "==> 跳过 Docker 部署。HA/摄像头桥后续手动配置。"
        exit 0
    fi
    echo "==> Docker 不可用但选择 Docker 全家——请先安装 Docker 再运行本脚本。"
    exit 1
fi

# ---------- 2. 检查 .env（先生信息） ----------
if [ ! -f .env ]; then
    echo ""
    echo "==> 首次部署：创建 .env（先生需填写）"
    cat > .env << 'EOF'
# 小米账号密码的 MD5（Miloco 部署时设置——与 Miloco WebUI 设置的密码一致）
MILOCO_PASSWORD=
# 小方摄像头 did（米家 App 设备详情可查，或 Miloco WebUI 绑定后可见）
CAMERA_ID=
EOF
    echo "已生成 .env 模板——请填写 MILOCO_PASSWORD(密码的MD5) 和 CAMERA_ID 后重新运行"
    echo "提示：echo -n '你的密码' | md5sum"
    exit 0
fi

# ---------- 3. 部署 ----------
echo "==> 拉取镜像并启动（首次较慢）"
docker compose up -d
echo ""
echo "=============================================="
echo " ✅ 部署完成"
echo "----------------------------------------------"
echo " Home Assistant: http://localhost:8123   (首次配置向导)"
echo " Miloco WebUI:   https://localhost:8000  (自签证书——忽略警告)"
echo " Go2rtc WebUI:   http://localhost:1984   (配置/查看 RTSP 流)"
echo "----------------------------------------------"
echo " LING OS 拉流地址: rtsp://localhost:8554/小方"
echo " App → Home Assistant 面板 → 配置: host=localhost"
echo "=============================================="
