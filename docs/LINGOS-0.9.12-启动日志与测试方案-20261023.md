# LING OS 0.9.12 启动日志与测试方案

> 日期：2026-10-23　版本：0.9.12（0.9 阶段：打通“进桌面”）
> 目标：ISO 从 GRUB 到安装完成/进入系统的每一步都可观测、可测试、可回传，
> 彻底解决“黑屏时不知道装到哪了”的问题。

## 一、目标与验收

0.9 完成标志（任一架构跑通即可，最终双架构）：

1. GRUB 菜单可见、可选（自动安装/手动安装/Live/诊断）；
2. 内核 + initramfs 启动，串口与 tty0 有日志；
3. live 环境自动进入安装器（或明确失败原因）；
4. 安装过程有阶段/百分比/ETA，写盘可见；
5. 安装完成自动重启，进入系统（桌面/服务）；
6. 全链路日志可采集（本地 + 串口 + 共享文件夹/网络）。

## 二、日志分层

| 层 | 产生者 | 位置 | 内容 |
|---|---|---|---|
| L0 固件/引导 | UEFI、GRUB | VGA/串口 | 菜单选择、内核 cmdline、kernel 版本 |
| L1 initramfs | initramfs-tools + live-boot | kmsg/串口 | LINGOS-INITRAMFS-START、live 介质探测 |
| L2 安装器 | lingos-install | 屏幕/串口/文件/共享文件夹/网络 | 阶段、百分比、ETA、错误码 |
| L3 系统 | lingos.service / 守护进程 | journald / /LINGOS/log | 服务启动、WebUI、AI 后端 |
| L4 诊断 | lingos-diag | tar.gz（本地+共享文件夹） | 上述全部 + dmesg/journal/lsblk/ip 等快照 |

内核参数（0.9.12 起）：

    boot=live components debug console=ttyAMA0,115200 console=ttyS0,115200 console=tty0     loglevel=7 video=1024x768@60 lingos.mode=<mode>

- 串口 console 放最后（arm64=ttyAMA0 / x86=ttyS0）→ /dev/console 是串口，live-boot/systemd 的早期输出
  能进入串口日志（CI 可抓）；VGA 的 tty0 仍由内核 printk 与安装器显式写入，VNC 可见；
- 去掉了 quiet splash → 内核与安装器输出不再被吞；
- debug → live-boot 详细日志（0.9 调试期保留，稳定后可去掉）。

## 三、固定标记协议（CI/串口/日志都可 grep）

| 标记 | 含义 |
|---|---|
| LINGOS-BOOT-MODE=<mode> | 启动模式：install-auto / install-manual / live / diag |
| LINGOS-BOOT-INFO kernel=... arch=... cmdline=... | 引导环境 |
| LINGOS-IMAGE-INFO ... | 构建版本/架构/git/构建时间 |
| LINGOS-INITRAMFS-START kernel=... | initramfs 已执行（L0 通过） |
| LINGOS-INITRAMFS-BOTTOM | live-boot 已挂载 live 根、即将 switch_root（L0.5 通过） |
| LINGOS-STAGE=<n>/<total> percent=<p> eta=<s> <消息> | 安装阶段进度 |
| LINGOS-INSTALL-DONE | 安装完成（rsync/GRUB/配置全部成功） |
| LINGOS-INSTALL-FAIL=code=<n> <消息> | 安装失败（含错误码与原因） |
| LINGOS-INSTALL-REBOOT-IN=<n> | 自动重启倒计时 |
| LINGOS-DIAG-READY=<path> | 诊断包已生成 |
| LINGOS-LIVE-READY ip=<ip> url=<url> | Live 模式就绪 |

## 四、安装阶段与进度（10 阶段）

| 阶段 | 名称 | 默认进度 | 说明 | 失败码示例 |
|---|---|---|---|---|
| 1 | preflight | 10% | root、/run/live、磁盘枚举 | 3/4/5 |
| 2 | select-disk | 20% | 自动选最大 ≥8G 非 USB 盘 / 手动选盘 | 6/7/8/9 |
| 3 | confirm | 30% | 手动模式确认 | 10 |
| 4 | partition | 40% | GPT：EFI 512M + root ext4 | — |
| 5 | format | 50% | mkfs.vfat / mkfs.ext4 | 12 |
| 6 | mount | 60% | 挂载目标盘 | — |
| 7 | copy | 60%→85% | rsync 复制 live 根，按 rsync 百分比映射 | 11 |
| 8 | bootloader | 85% | grub-install + update-grub（arm64=arm64-efi） | — |
| 9 | configure | 95% | fstab/hostname/首启服务/安装报告 | — |
| 10 | finish | 100% | sync + 卸载 + 倒计时重启 | — |

说明：
- ETA = 已用时间 × (100-当前百分比)/当前百分比，仅供参考；
- copy 阶段用 rsync --info=progress2 的百分比实时映射到 60–85%；
- 每个阶段写一次 state.json，并回传控制台/共享文件夹（网络每 10% 一次，避免刷屏）。

## 五、多通道回传（全部 best-effort，任一失败不影响安装）

| 通道 | 实现 | 适用场景 |
|---|---|---|
| tty0/tty1 | 直接写 /dev/tty0、/dev/tty1 | VNC/Vectras 可见 |
| 串口 | /dev/ttyAMA0、/dev/ttyS0 | CI、加 -serial 的 QEMU、真机串口 |
| kmsg | /dev/kmsg | dmesg、内核日志 |
| 本地文件 | /var/log/lingos-install.log、/run/lingos-install/state.json | 系统内排查 |
| 共享文件夹 | 自动探测可写 vfat 分区并挂载；写 progress.txt / state.json / install.log | Vectras 黑屏时最可靠 |
| 网络 | QEMU user-net：guest → 10.0.2.2:5555（dhclient + nc） | 宿主机 nc -l -p 5555 接日志 |

共享文件夹探测逻辑：lsblk 找到 vfat/exfat 分区 → 排除 live 介质 → 尝试 rw 挂载并写测试文件；
成功后固定挂载到 /run/lingos-share，后续每个阶段更新文件。

网络回传逻辑：首次尝试对 eth0/ens3 执行 dhclient，然后 nc -w 2 10.0.2.2 5555，
失败静默忽略（不阻塞安装）。

## 六、state.json 结构

    {"ok":true,"mode":"install-auto","stage":7,"stage_name":"copy","total":10,
     "percent":82,"message":"复制系统文件 82%","eta_seconds":95,"elapsed_seconds":420,
     "ts":"2026-10-23T12:00:00Z"}

失败时：

    {"ok":false,"mode":"install-auto","code":11,"message":"rsync 复制失败（rc=23）",
     "stage":7,"ts":"..."}

安装成功后写入目标盘：
- /LINGOS/system/config/install-report.json（模式/磁盘/UEFI/UUID/时间）
- /LINGOS/system/config/install-state.json（最终 state）
- /var/log/lingos-install.log（完整日志）

## 七、测试矩阵

| 编号 | 测试 | 方法 | 通过标准 |
|---|---|---|---|
| T1 | 引导冒烟（arm64） | CI/本地：distro/images/test-boot.sh aarch64 ISO 900 boot | LINGOS-BOOT-MODE、STAGE=1/10、STAGE=2/10、磁盘增长 |
| T2 | 引导冒烟（x86_64） | CI/本地：test-boot.sh x86_64 ISO 600 boot | 同上 |
| T3 | 完整安装（arm64） | test-boot.sh aarch64 ISO 2400 full | LINGOS-INSTALL-DONE + 重启 |
| T4 | 真机 Vectras | 手机：GRUB → 自动安装 → 重启进系统 | VNC/共享文件夹可见进度；qcow2 增长；进系统 |
| T5 | 诊断模式 | GRUB 选“诊断模式” → lingos-diag | 生成 tar.gz；共享文件夹/串口可见 DIAG-READY |
| T6 | 失败路径 | 不给磁盘 / 磁盘 <8G / 破坏 ISO | 明确失败码 + 提示，不静默 |

CI 集成：images workflow 的 iso-x86_64 / iso-arm64 在构建后自动执行 T1/T2，
启动日志 + 定时 VGA 截屏（monitor screendump）上传为 artifact（boot-log-x86_64 / boot-log-arm64）。
测试脚本命中 LINGOS-STAGE=2/10 或 BOOT FAILED 会提前结束 QEMU，避免空等超时。

## 八、0.9 验收清单

- [ ] arm64 ISO：GRUB 菜单 4 项可见
- [ ] 内核/initramfs 启动（串口出现 LINGOS-INITRAMFS-START / BOOT-MODE）
- [ ] 安装器自动选中磁盘并分区（state.json stage≥4）
- [ ] rsync 复制百分比推进，ETA 合理
- [ ] GRUB 安装成功，重启进入系统
- [ ] 安装后 /LINGOS/system/config/install-report.json 存在
- [ ] 真机 Vectras 黑屏时可通过共享文件夹/串口看到进度
- [ ] x86_64 ISO 同样通过 T2

## 九、0.10+ 演进

- 安装器图形化（进度条/品牌界面），阶段进度通过 API 暴露给 WebUI/App；
- 结构化日志统一（JSON lines + trace_id + 日志脱敏/轮转）；
- 首启配置向导（图形化首次启动 → 注册用户 → 桌面）；
- Live 模式桌面化（labwc/waybar/wofi + kiosk WebUI）。