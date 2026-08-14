# LING OS 依赖清单（DEPENDENCIES.md）

版本：初稿（2026-08-13 讨论定稿——**仅记录，未部署**）
状态：全捆打包方案（先生裁决）+ 自检系统优化逻辑

---

## 一、平台与架构定位

| 项 | 定稿 |
|---|---|
| 部署形态 | 主机+副机自用（架构固定，不追求通用发行物） |
| 支持平台 | **Linux 原生 / Android proot（Termux）/ Windows WSL2** 同包 |
| Windows 服务端 | WSL2 跑 Linux 包（不编 Windows 原生——seccomp/POSIX 全栈无法原生，W2 裁决） |
| 架构 | aarch64 + x86_64 双包（CI 矩阵构建） |
| glibc 门槛 | ≥ 2.35（CI 构建镜像 Ubuntu 22.04，覆盖过去三年主流系统） |

## 二、打包方案（全捆定稿）

| 层 | 方案 |
|---|---|
| Python 层 | **venv --copies 整体打包**（含解释器副本，解压即用，零安装） |
| C 库层 | **全捆到 glibc 为止**（ldd 递归收集 → 过滤 glibc 白名单 → 其余全拷入 lib/） |
| glibc 本体 | **永不捆绑**（业界铁律——与内核交互极深，NixOS 都放弃） |
| 加载机制 | rpath=$ORIGIN/../lib（主二进制）+ LD_LIBRARY_PATH 兜底（dlopen C 扩展） |

### 包体结构
```
LINGOS-<ver>-<arch>-linux/
├── lingos_linux          # 编译时 -Wl,-rpath,'$ORIGIN/../lib'
├── lib/                  # 全捆 .so（到 glibc 为止）
│   └── manifest.json     # CI 生成：文件名+版本+校验和（自检用）
├── python/               # venv --copies（解释器 + site-packages）
├── install_deps.sh       # 降级为环境检查：glibc 版本验证 + 报缺项
└── DEPENDENCIES.md
```

## 三、编译期依赖（构建机——CI Ubuntu 22.04 镜像）

- C 端：libcurl-dev / libseccomp-dev / libsqlite3-dev / libmosquitto-dev / libmicrohttpd-dev / notcurses-dev + gcc / make / pkg-config
- Python：python3 + venv（构建时打包用）
- 架构：x86_64 原生编；aarch64 交叉编译链或 qemu-user

## 四、运行期依赖（全捆后几乎为零）

- **宿主唯一硬性要求：glibc ≥ 2.35**（Ubuntu 22.04 / Debian 12 / proot ubuntu / WSL2 全覆盖）
- 内核：Linux（seccomp 沙箱需要）
- 其余 .so 全部包内 lib/ 自带；Python 全部包内 venv 自带

## 五、可选依赖（未捆绑/按需）

| 依赖 | 用途 | 落点 |
|---|---|---|
| espeak-ng / espeak / piper | 本地 TTS 降级链 | 【待定】随包 bin/ 携带 vs install 模块安装（先生决策原为装进安装系统） |
| git / ping / cron / pip3 / apt-get | 技能运行时调用 | 宿主可选（技能用，缺则对应技能降级） |

## 六、自检系统优化逻辑（定稿——全捆后自检不再"装依赖"）

```
check_item_dependencies 重写：
  1. 判定形态：包内存在 lib/+python/ → 捆绑形态（自包含）；否则 → 传统形态
  2. 捆绑形态：
     a. 对照 lib/manifest.json 检查 .so 齐全性（文件存在 + dlopen 试加载）
     b. python/ venv 验证（解释器可执行 + import requests/tiktoken）
     c. 全过 → PASS；缺失 → 触发 fix（不无条件安装）
  3. 传统形态兜底：python3/关键库存在？缺失 → fix
  fix（挂 fix_func，现全 NULL）：
     - 捆绑：包内备份恢复 → 失败提示"包体损坏，重新解压"
     - 传统：install_deps.sh（ldconfig 查 .so + 按需 apt/pip）
清理：
  - check_python 去掉过时 flask 检查 → 验证包内 venv + requests
  - env_bootstrap copy_python_scripts 捆绑形态下跳过
```

## 七、实测记录（沙箱 Alpine/musl——仅结构参考，非捆绑清单）

7 库 NEEDED 实测：
- libseccomp.so.2 / libsqlite3.so.0：仅依赖 libc（干净，跨发行版稳定）
- libcurl.so.4：libcares / libnghttp2 / libidn2 / libpsl / libssl / libcrypto / libzstd / libbrotlidec / libz
- libmosquitto.so.1：libssl / libcrypto / libcares
- libmicrohttpd.so.12：libgnutls
- libnotcurses.so.3：libnotcurses-core + **ffmpeg 六件套**（libavcodec/avdevice/avformat/swscale/avutil——Alpine 版带媒体支持）
- libnotcurses-core.so.3：libdeflate / libncursesw / libunistring

**⚠️ 结论：musl 树 ≠ glibc 树**（Ubuntu libcurl 无 libcares、有 ldap/gssapi/rtmp/ssh2；Ubuntu notcurses 无 ffmpeg）——**捆绑清单必须在 glibc 环境（CI Ubuntu 22.04）实测生成**。沙箱数据仅用于结构理解。

## 八、风险记录

1. 捆绑 OpenSSL/libssl 不随系统补丁更新——自用环境可接受，如对外发行需定期重建
2. 包体膨胀：x86_64 libcurl 协议全家桶 + notcurses/ffmpeg 估算 100~200MB（出包后实测记录）
3. LD_LIBRARY_PATH 全局生效需启动脚本包装（避免影响系统其他程序）
4. 宿主 glibc 低于 2.35 的环境不可用（约束明确写入部署说明）

## 九、CI 打包流程（0.2.1 实施时落地）

```
matrix: [aarch64, x86_64]
1. make 编译（rpath=$ORIGIN/../lib）
2. ldd lingos_linux 递归收集 → 过滤 glibc 白名单（libc/libm/ld-linux/libpthread/libdl/libresolv/libnss_*）
3. 拷贝 .so → lib/ + 生成 manifest.json（名+版本+校验）
4. venv --copies 打包 → python/
5. 出 LINGOS-<ver>-<arch>-linux.tar.gz（双架构产物）
```
