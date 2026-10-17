# LING OS 系统镜像与打包（distro/images）

三种交付形态，建议按 VM → 容器 → 安装 ISO → 系统包的顺序推进：

| 形态 | 产物 | 用途 | 状态 |
|---|---|---|---|
| OCI 容器镜像 | docker/podman 镜像 | 一键试跑、CI/服务端容器化 | ✅ 脚本+CI 就绪 |
| VM 镜像 | qcow2（可选 ova/vmdk） | 演示、开发、真机前验证 | 脚本骨架（需 root+网络） |
| 安装 ISO | iso-hybrid（UEFI+BIOS） | 真机/整机部署 | live-build 骨架（需网络） |
| 系统包 | .deb / rpm / ipk | 与发行版生态集成 | 后续（见 docs/IMAGES.md） |

注意：".sio" 应为 **.iso**（ISO 9660 镜像）；"虚拟系统" 通常指 VM 镜像（qcow2/ova）。

## 快速开始（容器）

    make -j
    RELEASE_VER=0.9.0 bash scripts/bundle.sh dist
    bash distro/images/build-container.sh lingos-server:0.9.0
    docker run -d --name lingos -p 8080:8080 -p 2939:2939 -v lingos-data:/LINGOS/data lingos-server:0.9.0
    curl http://127.0.0.1:8080/system/health

CI：Actions → images → Run workflow（distro/images/Dockerfile 构建 + 容器冒烟 + artifact）。

## VM / ISO

    sudo bash distro/images/build-vm-qcow2.sh   # 产物 dist/images/lingos-vm-<arch>.qcow2
    sudo bash distro/images/build-iso.sh        # 产物 dist/images/lingos-installer.iso

两者都需要网络（debootstrap/live-build 拉基础包）与 root；本仓库 CI 默认不构建（体积/时长原因），
需要时可单独跑。分区/引导/首次配置方案见 docs/IMAGES.md。
