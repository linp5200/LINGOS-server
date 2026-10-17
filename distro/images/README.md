# LING OS 系统镜像与打包（distro/images）

三种交付形态（按先生 2026-10-17 拍板：**双架构 x86_64 + aarch64、Ubuntu/Debian 基座、ISO 必做、.deb 不做**）：

| 形态 | 产物 | 用途 | 状态 |
|---|---|---|---|
| OCI 容器镜像 | docker/podman 镜像 | 一键试跑、验证、服务端容器化 | ✅ CI 验证（images workflow container） |
| VM 镜像 | qcow2 | 演示、开发、真机前验证 | ✅ 脚本（build-vm-qcow2.sh） |
| 安装 ISO | iso-hybrid（UEFI+BIOS） | 真机/整机部署 | ✅ 双架构 CI（自动安装器） |

注意：.sio 应为 .iso；"虚拟系统"指 qcow2/ova VM 镜像。

## 一、容器（最快验证）

    make && RELEASE_VER=0.9.0 bash scripts/bundle.sh dist
    bash distro/images/build-container.sh lingos-server:0.9.0
    docker run -d --name lingos -p 8080:8080 -p 2939:2939 -v lingos-data:/LINGOS/data lingos-server:0.9.0
    curl http://127.0.0.1:8080/system/health

镜像内置已配置设备形态（预置 state/config，跳过首启向导）、tini、healthcheck。

## 二、安装 ISO（真机）

    sudo bash distro/images/build-iso.sh x86_64     # 或 aarch64；默认 Ubuntu noble
    # 产物：dist/images/lingos-installer-<arch>.iso

ISO 行为：
- live 环境自动运行安装器（systemd 单元 lingos-installer.service → tty1），**默认自动安装**：
  选盘规则 = 内核参数 lingos.disk=/dev/sdX / 环境变量 LINGOS_DISK / 唯一内置磁盘；
  分区 = GPT + EFI 512M + root ext4（余量）；rsync live 根到目标盘；装 GRUB（UEFI+BIOS）。
- 手动模式：在 live 终端执行 sudo lingos-install（whiptail 选盘/确认）。
- 安装后首启由 systemd lingos.service 拉起 LINGOS（/LINGOS 数据根，WebUI 8080）。
- 桌面（labwc/waybar/wofi + WebUI kiosk）资产在 distro/desktop/，后续加入 ISO 包清单。

## 三、VM 镜像（qcow2）

    sudo bash distro/images/build-vm-qcow2.sh
    # 产物：dist/images/lingos-vm-<arch>.qcow2

## 四、CI

- workflow images：container（构建+启动+/system/health 冒烟+artifact）
  + iso-x86_64（ubuntu-latest）+ iso-arm64（ubuntu-24.04-arm）
  + release（tag v* 时把双架构 ISO 附加到 GitHub Release）。
- 手动触发：Actions → images → Run workflow。

## 五、测试/验收

    bash -n distro/images/*.sh distro/images/live/*.sh
    python3 -c "import yaml; yaml.safe_load(open('.github/workflows/images.yml'))"

真机验收：U 盘写入 ISO → 启动 → 自动安装（或手动）→ 重启 → http://<ip>:8080/ui 可访问。
