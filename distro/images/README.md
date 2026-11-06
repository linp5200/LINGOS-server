# LING OS 系统镜像与打包（distro/images）

三种交付形态（按先生 2026-10-17 拍板：**双架构 x86_64 + aarch64、Ubuntu/Debian 基座、ISO 必做、.deb 不做**）：

| 形态 | 产物 | 用途 | 状态 |
|---|---|---|---|
| OCI 容器镜像 | docker/podman 镜像 | 一键试跑、验证、服务端容器化 | ✅ CI 验证（images workflow container） |
| VM 镜像 | qcow2 | 演示、开发、真机前验证 | ✅ 脚本（build-vm-qcow2.sh） |
| 安装 ISO | UEFI El Torito ISO | 真机/整机部署 | ✅ 双架构 CI（构建 + QEMU 启动冒烟） |

注意：.sio 应为 .iso；"虚拟系统"指 qcow2/ova VM 镜像。

## 一、容器（最快验证）

    make && RELEASE_VER=0.9.12 bash scripts/bundle.sh dist
    bash distro/images/build-container.sh lingos-server:0.9.12
    docker run -d --name lingos -p 8080:8080 -p 2939:2939 -v lingos-data:/LINGOS/data lingos-server:0.9.12
    curl http://127.0.0.1:8080/system/health

镜像内置已配置设备形态（预置 state/config，跳过首启向导）、tini、healthcheck。

## 二、安装 ISO（真机 / 虚拟机）

    sudo bash distro/images/build-iso.sh x86_64     # 或 aarch64；默认 Ubuntu noble
    # 产物：dist/images/lingos-installer-<arch>.iso

### 2.1 启动菜单（0.9.12+，arm64 手工 grub；x86 默认自动安装项）

| 菜单项 | 内核参数 | 行为 |
|---|---|---|
| LING OS 安装（自动，默认） | `lingos.mode=install-auto` | 自动选最大 ≥8G 内置盘 → 分区 → rsync → GRUB → 重启 |
| LING OS 安装（手动选择磁盘） | `lingos.mode=install-manual` | whiptail 选盘 + 确认后安装 |
| 进入 LING OS Live（不安装） | `lingos.mode=live` | 启动服务 + 操作菜单（安装/Shell/诊断/重启） |
| 诊断模式 | `lingos.mode=diag` | 收集日志 → 救援 shell；`lingos-diag` 可再次收集 |

内核参数统一为：

    boot=live components debug console=ttyAMA0,115200 console=ttyS0,115200 console=tty0 \
    loglevel=7 video=1024x768@60 lingos.mode=<mode>

> `console=tty0` 放在最后，保证 `/dev/console` 是 VGA 控制台（VNC 可见）；串口仍会收到内核日志。

### 2.2 安装日志与进度（0.9.12+）

安装器每一个阶段都会写：

- 屏幕：`/dev/tty0`（VNC 可见）与 `/dev/tty1`（服务 TTY）
- 串口：`/dev/ttyAMA0` / `/dev/ttyS0`
- dmesg：`/dev/kmsg`
- 共享文件夹（若 VM 开启）：`lingos-install-progress.txt`、`lingos-install-state.json`、`lingos-install.log`
- 网络（QEMU user-net 可用时）：guest → `10.0.2.2:5555`（宿主机可 `nc -l -p 5555` 接收）
- 本地文件：`/var/log/lingos-install.log`、`/run/lingos-install/state.json`（JSON 状态）

进度 JSON 示例：

    {"ok":true,"mode":"install-auto","stage":7,"stage_name":"copy","total":10,
     "percent":82,"message":"复制系统文件 82%","eta_seconds":95,"elapsed_seconds":420,
     "ts":"2026-10-23T12:00:00Z"}

失败时返回 `ok:false`、`code`、`message`，并输出固定标记：

    LINGOS-INSTALL-FAIL=code=<n> <message>

### 2.3 固定标记（供 CI / 串口 / 日志抓取）

    LINGOS-BOOT-MODE=<mode>
    LINGOS-STAGE=<n>/<total> percent=<p> eta=<s> <message>
    LINGOS-INSTALL-DONE
    LINGOS-INSTALL-FAIL=code=<n> <message>
    LINGOS-DIAG-READY=<path>
    LINGOS-LIVE-READY ip=<ip> url=<url>
    LINGOS-INITRAMFS-START kernel=<ver>

### 2.4 安装后

- 首启由 systemd `lingos.service` 拉起 LINGOS（/LINGOS 数据根，WebUI 8080）。
- `/LINGOS/system/config/install-report.json` 记录安装模式/磁盘/时间；`install-state.json` 记录最终进度。
- 桌面（0.9.12+）：ISO 已内置 labwc/waybar/wofi/foot 与中文 Noto 字体；安装器创建 `lingos` 用户并配置
  tty1 自动登录，`~/.bash_profile` 自动执行 labwc —— 安装后重启即进入 Wayland 桌面（VNC/QEMU 画面可见）。
  WebUI kiosk（chromium）在 0.10 接入，当前可用 `foot` 终端 + WebUI 浏览器手动访问。

### 2.5 写盘方式

ISO 为 UEFI El Torito 格式——用 Rufus「ISO 模式」、光学介质或虚拟机挂载；dd 直写 U 盘的 hybrid MBR 后续版本再补。

## 三、VM 镜像（qcow2）

    sudo bash distro/images/build-vm-qcow2.sh
    # 产物：dist/images/lingos-vm-<arch>.qcow2

Vectras VM（Android ARM64）的完整配置见 [vm/VECTRAS-VM.md](vm/VECTRAS-VM.md)。

## 四、CI

- `images` workflow（手动 dispatch 或 tag v*）：
  - container：构建 + 启动 + `/system/health` 冒烟 + artifact
  - iso-x86_64（ubuntu-latest）/ iso-arm64（ubuntu-24.04-arm）：构建 ISO 后执行
    `distro/images/test-boot.sh`（QEMU UEFI + 串口）做**启动冒烟**
  - release：tag v* 时拆分 >1.9GiB 资产、生成 `SHA256SUMS.txt` 与合并说明，附加到 GitHub Release
- 启动冒烟日志作为 artifact 上传（`boot-log-x86_64` / `boot-log-arm64`）。

## 五、测试 / 验收

    # 语法检查
    bash -n distro/images/*.sh distro/images/live/*.sh
    python3 -c "import yaml; yaml.safe_load(open('.github/workflows/images.yml'))"

    # 启动冒烟（需 qemu-system-<arch> + UEFI 固件；boot 模式=只验证引导+开始写盘）
    sudo bash distro/images/test-boot.sh aarch64 dist/images/lingos-installer-aarch64.iso 900 boot
    sudo bash distro/images/test-boot.sh x86_64  dist/images/lingos-installer-x86_64.iso  600 boot

    # 完整安装（慢，需要更长时间；真机前跑）
    sudo bash distro/images/test-boot.sh aarch64 dist/images/lingos-installer-aarch64.iso 2400 full

真机验收：U 盘/VM 挂载 ISO → 启动 → 自动安装（或手动）→ 重启 → 进入系统 / http://<ip>:8080/ui 可访问。

## 六、常见故障

| 现象 | 排查 |
|---|---|
| GRUB 后黑屏 | 0.9.12+ 默认无 `quiet`，tty0 会输出；若无输出看串口/共享文件夹进度文件 |
| 一直停在安装器某阶段 | 看 `/run/lingos-install/state.json` 与 `/var/log/lingos-install.log`；诊断模式执行 `lingos-diag` |
| 未发现可安装磁盘 | 需要 ≥8G 非 USB 磁盘；共享文件夹/光驱会被自动排除 |
| Release 资产是 .part0/.part1 | GitHub 单资产 2GiB 限制；按 `.合并说明.txt` 执行 `cat part* > iso`，并用 `SHA256SUMS.txt` 校验 |
## v0.9.12 发布（2026-10-23）

- Release: https://github.com/linp5200/LINGOS-server/releases/tag/v0.9.12
- arm64（Vectras/QEMU UEFI）：下载 `lingos-installer-aarch64.iso.part0/part1`，合并：
  `cat lingos-installer-aarch64.iso.part* > lingos-installer-aarch64.iso`
  合并后 sha256 见 `lingos-installer-aarch64.iso.txt`。
- x86_64：`lingos-installer-x86_64.iso`（UEFI 启动问题仍待修，见进展记录）。
- CI 启动冒烟已通过：run 40021339089（`qemu_result=installer-writing`，安装器完成分区/格式化）。
