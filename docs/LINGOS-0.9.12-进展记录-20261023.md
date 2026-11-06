# LING OS 0.9.12 进展记录（2026-10-23）

> 目标：把 0.9（ISO 启动 → 安装 → 进入系统）打通，并让每一步可观测、可测试。

## 一、v0.9.11 黑屏根因（已在 CI 复现）

用 `boot-probe` workflow 下载 v0.9.11 arm64 ISO，在 QEMU UEFI + 串口下复现：

- GRUB 菜单正常显示（VNC/串口都能看到 "LING OS (aarch64)"）；
- 内核与 initrd 正常加载（串口出现 EFI stub / Booting Linux）；
- live-boot 启动后报错：

      BOOT FAILED!
      live-boot will now start a shell. The error message was:
        Unable to find a medium containing a live file system

- ISO 内已确认存在 `/live/vmlinuz`、`/live/initrd.img`、`/live/filesystem.squashfs`
  （iso-inspect workflow 检查），**不是 ISO 内容缺失/损坏**；
- 真机 Vectras 上表现为黑屏 + qcow2 不增长，与 CI 一致。

结论：问题在 live-boot 找不到/挂载不了 live 介质，而不是 ISO 烧录/合并问题。

### 1.1 根因（2026-10-23 已确认）

用 initramfs probe（init-premount 直接写串口）拿到确凿证据：

    -- filesystems --
        iso9660            # isofs 已加载
    -- devices --
    brw------- 1 11, 0 /dev/sr0
    -- blkid /dev/sr0 --
    /dev/sr0: BLOCK_SIZE="2048" LABEL="LINGOS" TYPE="iso9660" PTTYPE="PMBR"
    -- mount test --
    MOUNT-OK

而 live-boot 的 `is_supported_fs()` 逻辑是：先 `grep iso9660 /proc/filesystems`，
不满足再 `modprobe -q -b iso9660`——**但 iso9660 的模块名是 isofs，modprobe iso9660 永远失败**，
于是 live-boot 判定“不支持该文件系统”，直接跳过挂载，最终报
`Unable to find a medium containing a live file system`。

另外两个与 live-boot（Ubuntu noble 版本）相关的坑：

1. `live-media=/dev/sr0,/dev/sda` 这种**逗号列表**在 `check_dev` 里会被拆开后取最后一个
   （devname=/dev/sda，不存在）→ 强制设备失败；
2. 一旦设置了 `live-media=`，该版本 live-boot **不再扫描其他块设备**，所以设错等于直接放弃。

### 1.2 修复

- ISO 的 initramfs 增加 `/scripts/init-premount/lingos-report`：在 live-boot 之前显式
  `modprobe isofs squashfs overlay loop sr_mod sd_mod usb-storage uas virtio_blk virtio_pci`；
- 增加 initramfs hook `lingos-tools`，用 `copy_exec` 保证 `blkid`/`udevadm` 及其依赖库进入 initrd
  （live-boot 的 `get_fstype` 依赖 blkid）；
- `live-media=/dev/sr0` **必须用单一设备名**（逗号列表会被 live-boot 拆成最后一项 /dev/sda 而失败）；
  实测不设置该参数时 live-boot 的 CD 扫描分支也未能找到介质，单一 sr0 可直接命中；
- **squashfs 必须保留 /dev /proc /sys /run /tmp 目录**：live-boot 的 init-bottom 会把 initramfs 中的挂载点
  `mount --move` 到新根；若这些目录被 squashfs 排除，会报
  `mount: mounting /dev on /root/dev failed` → `can't open /root/dev/console` →
  `Kernel panic - Attempted to kill init`。已在 mksquashfs 阶段修正；
- 生产内核参数去掉 `debug`（避免 initramfs 日志重定向与额外开销），诊断模式保留 `debug`。
- **live-build 本地 chroot hook 必须命名为 `config/hooks/*.chroot`**（直接放在 config/hooks 下、
  `.chroot` 后缀）；原 `config/hooks/live/*.hook.chroot` 从未被执行，导致：
  1) `/LINGOS` 运行时（二进制/venv/WebUI）没有装进 ISO；
  2) `lingos-installer.service` 没有被 enable；
  3) 安装后的系统缺少 LINGOS 服务。
  已改为 `config/hooks/0100-lingos.chroot`，并在构建期校验 `/LINGOS/bin/lingos_linux` 存在（缺失即构建失败）。

## 二、本轮修复（0.9.12）

### 1. 启动可观测性

- 内核参数去掉 `quiet splash`，加 `loglevel=7`、`debug`；
- 串口 console 放最后（arm64=`ttyAMA0`、x86=`ttyS0`），`/dev/console` 走串口，
  live-boot/systemd 早期日志可被抓取；
- 新增 initramfs 标记：`LINGOS-INITRAMFS-START` / `LINGOS-INITRAMFS-BOTTOM`
  （确认 initramfs 执行、live-boot 挂载完成）；
- 全部日志同时写 tty0/tty1/串口/kmsg/共享文件夹/网络（见日志方案文档）。

### 2. 安装器

- 10 阶段进度、百分比、ETA，写 `/run/lingos-install/state.json`；
- 修复：`GRUBTARGET` 未定义（会导致 GRUB 安装失败）、`lsblk` TRAN 空串列导致磁盘识别失败；
- 共享文件夹自动探测（Vectras 黑屏时最可靠反馈），网络回传（guest → 10.0.2.2:5555）；
- 安装后创建 `lingos` 用户 + tty1 自动登录 + labwc 桌面配置。

### 3. GRUB 菜单（arm64）

4 个选项：自动安装（默认）/ 手动安装 / 进入 Live（不安装）/ 诊断模式。

### 4. 桌面（0.9 目标“进桌面”）

- ISO 包清单加入 labwc / waybar / wofi / foot / fonts-noto-cjk；
- 安装器创建用户并配置自动登录，`~/.bash_profile` 自动 exec labwc；
- 安装后重启应进入 Wayland 桌面（VNC/QEMU 画面）。

### 4.5 安装器启动后停滞（systemd tty 绑定导致 SIGTTOU 挂起）

- **最终根因（交互式串口调试确认）**：安装器脚本本身完全正常——手动在 live 串口 shell 里执行
  `bash -x /usr/local/sbin/lingos-install --auto` 可正常走到分区/rsync（5MB/s）。
  但由 systemd 服务启动时，unit 里 `StandardInput=tty`/`TTYPath=/dev/tty1` 会让进程控制终端绑到 tty1，
  而 live-config 已在 tty1 自动登录 user；服务进程写 tty1 时触发 **SIGTTOU**，进程被挂起（stopped），
  于是出现“service Started 后静默卡死、qcow2 不增长”。
- **修复**：unit 改为 `StandardInput=null`、`StandardOutput=journal`、`StandardError=journal`，
  不再绑定 tty1；安装器通过 `/dev/tty0`、串口显式输出进度（写入均带 timeout 保护）。
- 旧记录（tty 写入阻塞）保留如下：

### 4.5 旧分析：安装器启动后停滞（tty 写入阻塞）

- 现象：systemd 显示 `Started lingos-installer.service`，串口出现
  `LINGOS-BOOT-MODE=install-auto` 后**再无任何进度**，qcow2 不增长。
- 原因：安装器 `_emit()` 会同时写 tty0/tty1；live-config 在 tty1 自动登录了 `user`
  （或 getty 持有 tty1），安装器向 tty1 写日志时**阻塞**，后续 stage 全部卡住。
- 修复：`_emit()` 改为**串口优先**，tty0/tty1 写入用 `timeout 1 tee` 保护；
  另加 service `ExecStartPre`、boot-mode/install 脚本最早期串口标记与 ERR trap，任何失败都能在串口看到。

### 5. 测试

- `distro/images/test-boot.sh`：ISO 内容检查 + initrd 组件检查 + QEMU UEFI 启动 +
  串口断言 + 关键标记提前结束 + 定时 VGA 截屏；
- images workflow 构建后自动跑启动冒烟，日志/截图上传 artifact；
- 新增 `boot-probe` / `iso-inspect` 诊断 workflow（跨版本复现/解包检查）。

## 三、当前状态

- [x] v0.9.11 故障在 CI 复现并定位到 live-boot
- [x] 启动日志/进度/菜单/诊断/桌面集成代码完成
- [x] 容器 CI 通过
- [x] **0.9.12 arm64 启动冒烟通过（CI）**：run 40021339089，`qemu_result=installer-writing`，
      PASS：boot-mode / preflight / 选盘，磁盘已写入（qcow2 7.6MB，分区+格式化阶段）
- [x] **v0.9.12 Release 已发布**：https://github.com/linp5200/LINGOS-server/releases/tag/v0.9.12
      - arm64：`lingos-installer-aarch64.iso.part0`（1,992,294,400）+ `part1`（1,069,608,960），
        合并后 3,061,903,360 字节，sha256 `72b1e456f20eed8d180187aca8aab0e4bc026e7d741e8eef78a56f0870ab68d2`
      - x86_64：`lingos-installer-x86_64.iso`（1,632,577,536）
      - `SHA256SUMS.txt`、`lingos-installer-aarch64.iso.txt`（合并说明）
      - 发布流水线：publish（40023687345）与 publish-fast（40026295197，aria2 并行下载）均成功
- [ ] 真机 Vectras 验证安装 + 桌面

## 四、后续

1. 启动冒烟通过后发布 v0.9.12（含 SHA256SUMS、分片合并说明）；
2. 真机 Vectras 按《VECTRAS-VM.md》验证：共享文件夹看进度 → 安装 → 重启进桌面；
3. 若仍失败：诊断模式 + 共享文件夹/串口日志定位；优先修 live-boot 介质识别；
4. 0.10 再补：Live 模式桌面、WebUI kiosk、首次启动向导、增量更新等。