# 在 Vectras VM（Android）里运行 LING OS aarch64 ISO

> 适用：Vectras VM 3.9.x（ARM64 手机），虚拟机架构 ARM64；ISO 版本 **v0.9.12+**。
> 0.9.12 起：GRUB 有 4 个启动选项、安装全程有进度日志、支持共享文件夹回传、失败会给出错误码。

## 一、前提：ARM64 必须启用 Vectras 的默认 UEFI

Vectras 的 ARM64 固件来自它自带的 QEMU_EFI.img + QEMU_VARS.img（源码 FirmwareManager/StartVM.java）。
设置路径：VM 设置 → **Firmware（固件）**：

- ✅ 勾选 **使用默认BIOS/UEFI**（ARM64 的 UEFI 就靠它；“使用UEFI（仅对x64生效）”对 ARM64 无效）
- 自定义参数：**留空**（0.9.12 不依赖自定义参数；串口调试可选，见第五节）
- 共享文件夹：**建议开启**，指向一个目录（如 Download）——0.9.12 会把安装进度写成
  lingos-install-progress.txt / lingos-install-state.json / lingos-install.log，
  这是黑屏时最可靠的“装到哪了”的反馈
- 内存：≥ 2048 MB（建议 3072）
- 启动自：光驱（首次安装）

启动后 Vectras 显示的 QEMU 参数里应出现：

    -drive file=.../QEMU_EFI.img,format=raw,readonly=on,if=pflash
    -drive file=.../QEMU_VARS.img,format=raw,if=pflash

## 二、硬盘

- 一块 **≥10GB** 的 qcow2 硬盘（Vectras 新建/导入）。
- 安装器自动模式会选“最大且 ≥8G 的非 USB 磁盘”；光驱（USB）与共享文件夹（小容量 vfat）会被自动排除。
- 不需要加第二块盘。

## 三、ISO（v0.9.12+）

- Release 下载：https://github.com/linp5200/LINGOS-server/releases
- aarch64 ISO 可能因 GitHub 单资产 2GiB 限制被拆成 .part0/.part1：
  按 Release 里的“合并说明.txt”执行合并：

      cat lingos-installer-aarch64.iso.part* > lingos-installer-aarch64.iso
      sha256sum lingos-installer-aarch64.iso    # 与 SHA256SUMS.txt / 合并说明核对

- **合并后先核对文件大小**：ls -l 应与 Release 说明中的“原始大小”一致；
  大小不对就是合并不完整（GRUB 能显示但内核/根文件系统加载会失败，表现为黑屏）。
- VTERM 里 qemu-img 等工具在 Vectras 私有路径下；合并/校验用系统自带命令即可。

## 四、预期启动流程（0.9.12+）

    Vectras → EDK2 UEFI（pflash）→ GNU GRUB 菜单（VNC 可见，10 秒倒计时）
      ├─ 1) LING OS 安装（自动，默认）
      ├─ 2) LING OS 安装（手动选择磁盘）
      ├─ 3) 进入 LING OS Live（不安装，可进系统）
      └─ 4) 诊断模式（收集日志 / 救援 shell）
    → 自动安装：分区 → rsync 复制（进度 %）→ 装 GRUB → 8 秒倒计时重启
    → 重启前把“启动自”改为硬盘（或移除 ISO）

### 黑屏时怎么判断“装到哪了”（0.9.12 重点）

按优先级：

1. **VNC 画面**：0.9.12 去掉了 quiet splash，内核/安装器进度会写到 tty0，正常情况下 VNC 里能看到；
2. **共享文件夹**：打开 Vectras 共享文件夹后，在文件管理器里看
   lingos-install-progress.txt（阶段/百分比/预计剩余时间）；
3. **串口**（可选，见第五节）：VTERM 里 nc -l -p 5555 可接实时日志（如果 Vectras 自定义参数生效）；
4. 最后才看 qcow2 大小：

       ls -lh /storage/emulated/0/Documents/VectrasVM/roms/<id>/Nowdark.qcow2

   只有安装器真正开始分区/复制时才会明显增长（从 196K 涨到 MB/GB 级）。
   一直 196K = 安装器没执行（引导链或选盘问题），不是“装得慢”。

## 五、可选：串口日志（Vectras 自定义参数）

Vectras 的“自定义参数”在部分版本/设置下不会生效（实测 ps -e | grep qemu 里没有 -serial）。
0.9.12 不依赖串口：先看 VNC 与共享文件夹。

若想尝试串口：

1. VM 设置 → 自定义参数，加入：

       -serial tcp:127.0.0.1:5555,server,nowait

2. 启动 VM 后，在 VTERM 里 nc 127.0.0.1 5555 查看内核/安装器日志；
3. 若 ps -e | grep qemu 看不到 -serial 参数，说明该版本没有应用自定义参数——忽略此节，用 VNC/共享文件夹即可。

## 六、安装完成后

- 重启后从硬盘启动（Vectras 里把“启动自”改为硬盘，或移除 ISO）。
- 系统内 lingos.service 会自动拉起 LINGOS（/LINGOS 数据根，WebUI 8080）。
- 安装记录：/LINGOS/system/config/install-report.json；
  安装日志：/var/log/lingos-install.log。

## 七、本地 QEMU（桌面 Linux）等价验证

    qemu-system-aarch64 -M virt,virtualization=true -cpu cortex-a76 -m 3072 -smp 2 \
      -bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
      -drive if=none,id=cd,format=raw,media=cdrom,file=lingos-installer-aarch64.iso \
      -device nec-usb-xhci,id=xhci -device usb-storage,bus=xhci.0,drive=cd \
      -drive file=lingos-vm.qcow2,if=none,id=disk,format=qcow2 \
      -device virtio-blk-pci,drive=disk \
      -device virtio-gpu-pci -display none -serial stdio

也可以直接用 distro/images/test-boot.sh（见 distro/images/README.md）。

## 八、常见现象

| 现象 | 原因 | 处理 |
|---|---|---|
| UEFI Shell Shell> | ISO 无 EFI 引导项（旧版 arm64 ISO） | 换 v0.9.12+ |
| GRUB 后黑屏且 qcow2 不变 | 内核/initrd 未加载，或合并后的 ISO 不完整 | 核对 ISO 大小/sha256；用诊断模式看串口/共享文件夹 |
| 安装进度停在某阶段 | 该阶段失败（有错误码） | 共享文件夹 lingos-install-state.json 的 message/code；诊断模式 lingos-diag |
| 提示“未发现可安装磁盘” | 没有虚拟硬盘 / 磁盘 <8G / 被识别为 USB | 加一块 ≥10G qcow2 硬盘 |
| 安装完成但重启进不了系统 | 启动顺序仍是光驱 | 把“启动自”改为硬盘或移除 ISO |
