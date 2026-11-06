# LING OS 0.13 rootfs（Termux）

## 构建（arm64 runner）
    sudo bash distro/rootfs/build-rootfs.sh
产物：dist/rootfs/LINGOS_rootfs_arm64_<VERSION>.tar.zst + .sha256

## 安装（Android Termux）
    pkg install -y proot zstd curl
    bash install-termux.sh LINGOS_rootfs_arm64_0.13.0.tar.zst
    lingos            # http://127.0.0.1:8080
    lingos --lan      # 局域网 + 配对码

## 说明
- 不依赖 systemd；start-web.sh 直接拉起后端与 8080。
- rootfs 内含 C/Python 服务、Web 桌面、ACL/设备 API。
- 默认仅 127.0.0.1；--lan 才绑 0.0.0.0，并在首次登录要求配对码。
- Termux 无内核级防火墙/seccomp；安全由应用层 ACL/审计承担。
