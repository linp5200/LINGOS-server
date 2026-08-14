# LING OS App 0.2.1 问题清单（先生记录——2026-08-13 凌晨）

> 状态：**仅记录，未部署**（先生指示"优先记录而不是部署"）
> 全部为 App 端问题（服务端 0.2.0 已完成部署）

---

## 1. 离线模式（核心缺失）

- **现状**：App 无本地缓存——所有消息/会话/设置实时依赖 WS 连接，断连即空白
- **需求**：支持离线模式
  - 将**上一次连接的所有内容**（会话列表/消息/记忆摘要等）保存在本地
  - 未连接时**仍可显示**历史内容（只读）
  - 未连接时**禁止操作**：任何修改操作提示"当前尚未连接主机，无法修改"
- **技术方向**（2026-08-14 定稿——先生裁决）：
  - 存储：**sqflite 主存储**（sessions/messages/memory_summary 三表，messages 建 (session_id, created_at) 索引分页）+ shared_preferences 辅（连接配置/UI 偏好）
  - 策略：**全量缓存**（连接建立后拉全量快照落库，不做 N 会话限制；增量 diff 二期）
  - **加密**：sqflite 走 sqlcipher 方案（密钥 Android Keystore；CI 需带 libsqlitecipher 原生库）
  - 同步：首连全量 → 断连检测（WS onDone/超时）→ 只读渲染 + 操作拦截弹提示 → 重连拉快照恢复
  - 依赖：sqflite 引入 pubspec（连带 9.1/9.4 服务端同批改）

## 2. 模型与主机同步不完整

- **现状**：providers_screen 已有 provider_list 同步 + model_switch 切换（0.2.0 实现）
- **问题**：
  - **会话页（chat screen）无法切换模型**——切换只能去 AI 配置 → 提供商页
  - 会话中使用哪个模型无显示/无控制
- **需求**：见第 5 条（会话内模型切换）

## 3. 聊天故障：工具调用后 AI 对话中断

- **现象**：聊天 → AI 调用工具 → 工具执行完 → **与 AI 的对话直接中断**，必须用户**主动再发一次**才会继续
- **影响**：工具调用场景下 AI 无法继续回复，交互断裂
- **疑似根因**（服务端/App 待查）：
  - `_react_stream` 工具调用后返回内容为空时 done 提前发送
  - App 端 tool_call/tool_result 后未正确续接 content 流
  - 需复现：AI 并行工具调用（如 0.1.9 的 system_info+system_memory 场景）

## 4. 工具块 + 思考块：支持展开/收缩

- **现状**：工具调用为纯文本行（▸ 工具名 args）、思考为灰色文本块——不可折叠，长内容占满屏幕
- **需求**：
  - **工具块**：允许展开/收缩（默认收缩？显示工具名 + 状态，点击展开看 args/result）
  - **思考块**：允许展开/收缩（同工具块）
  - 折叠控件：ExpansionTile 或自定义展开按钮

## 5. 外观与偏好：添加"流式思考展开思考链"

- **现状**：appearance_screen 只有"思考过程显示"开关（thinking 显示/隐藏）
- **需求**：新增"流式思考展开思考链"选项——控制思考链默认展开/收缩

## 6. 会话内模型切换

- **需求**：
  - 会话正上方显示**会话名**
  - 会话名下方**允许更改模型**（使用已添加的模型列表）
  - 切换时**提示"更改模型后命中缓存将被重置"**
- **技术**：chat screen 头部 UI + model_switch 命令（服务端已有）+ 缓存重置提示

## 7. 开局显示设置

- **现状**：启动后固定进入默认页面，无选择
- **需求**：外观中添加"启动应用后首先显示"三个选项（平铺选择）：
  - 上一次的会话
  - 会话列表
  - 仪表盘

## 8. 权限授予优化（三个子问题）

- **问题 1**：部分权限仍然不正确——统一授权时没有覆盖全部（19 权限 × 5 模式列表与实际权限有出入）
- **问题 2**：部分权限无法授予——**manifest 未声明**（如部分后台/设备权限）
- **问题 3**：部分权限授予失败后**没有跳转到系统设置引导**（permission_handler 的 openAppSettings 未使用）
- **需求**：统一授权完整覆盖 + manifest 声明补全 + 失败时跳系统设置引导（引导链完整）

## 9. 会话管理增强（先生 2026-08-13 记录）

- **9.1 会话列表显示占用**：从"进入会话"改为显示**每个会话的占用**（token 占用/消息数——服务端 session_list 现只有 message_count 无 token；需服务端加 token 统计或 App 侧计算）
- **9.2 开局显示默认**：如果用户没有设置开局显示 → **默认为会话列表**（第 7 条三选项的默认值）
- **9.3 会话创建不完善**：
  - 现状：弹窗输入标题 → session_create（标题可为空 → "新会话"）
  - 问题：无默认名/无自动进入/无会话描述/创建后不刷新进入？需完整化（先生判断"不够完善不够完整"）
- **9.4 主机端模型不同步到 App**：
  - 现状：providers_screen 有 provider_list 同步，但**实际不同步**（先生实测）——模型没有从主机同步到 App
  - 疑似根因：providers_screen 的 `_loadServerModels()` 在 initState 调 `requestJson`——但 WS 命令响应是广播流，requestJson 监听 `command_response` 可能与自动同步（system_info/session_list）的响应冲突（先到先得）→ 拿到错误响应/超时 → 模型列表空白
  - 需修复：命令响应按 cmd 匹配（响应携带 cmd 标识）或独立请求 ID 配对

## 10. 其他（先生补充）

- 会话页/聊天页进入会话后的交互完善（承接 9.1——显示占用后交互）
- **语音降级链 Bug**（先生 2026-08-13 实测）：未配置 STT/TTS 提供商时**不会落到设备本地默认 TTS/STT**——服务端无提供商/无 espeak 时 App 只提示失败，未走设备本地 TTS（android TTS）/本地 STT。需求：降级链末端 = 设备本地

## 11. Home Assistant 集成（0.2.1 定稿——先生裁决全录 2026-08-13）

- 详见协议 AI-AGENT#10（连接 A+B+C / 功能全要 / 权限 A2 默认执行 / App 改名 B1 / 配置独立入口 C2 / C 端增强）
- 实施属 0.2.1 批次（先生指示"优先记录而不是部署"）

## 12. espeak-ng/piper 装进安装系统（先生决策——落点随全捆方案演进）

- 原决策：装进安装系统（C 端 install 模块，需 make 重编译）
- 全捆方案演进后：espeak-ng/piper 属系统命令依赖（二进制+数据，非 .so）——【待定】随包 bin/ 携带（与全捆逻辑一致，解压即用）vs 维持 install 模块安装

## 13. 依赖打包定稿：全捆方案 + 自检优化（2026-08-13 讨论定稿——仅记录未部署）

### 平台定位（先生裁决）
- 主机+副机自用，架构固定：aarch64 + x86_64 双包（CI 矩阵构建）
- 支持平台：**Linux 原生 / Android proot / Windows WSL2 同包**（W2——服务端不编 Windows 原生，WSL2 跑 Linux 包；速度评估通过：CPU/网络近原生，磁盘仅跨盘 /mnt/c 慢，数据放 WSL 内部 ext4 即接近原生）
- glibc 门槛：CI 构建镜像 **Ubuntu 22.04（glibc 2.35）**，覆盖过去三年主流系统；**glibc 本体永不捆绑**（业界铁律）

### 打包方案（先生裁决 A+B 升级全捆）
| 层 | 定稿 |
|---|---|
| Python 层 | **venv --copies 整体打包**（A 裁决——含解释器副本，解压即用，零安装） |
| C 层 | **全捆到 glibc 为止**（先生裁决"全部都绑"——ldd 递归收集、过滤 glibc 白名单、其余全拷入 lib/） |
| 加载机制 | rpath=$ORIGIN/../lib（主二进制）+ **LD_LIBRARY_PATH 兜底**（numpy 等 dlopen C 扩展不走 RPATH） |

### 实测关键结论
- **musl 依赖树 ≠ glibc 依赖树**：沙箱实测 Alpine 版 notcurses 带 ffmpeg 全家（libavcodec/avformat...）、libcurl 子依赖不同（libcares vs Ubuntu 的 ldap/gssapi/rtmp/ssh2）——捆绑清单**必须在 glibc 环境（CI Ubuntu 22.04）实测生成**，沙箱只能做结构参考
- 沙箱实测 7 库 NEEDED：libseccomp/libsqlite3 仅依赖 libc（干净）；libcurl 拉 libssl/libcrypto/libz 等 9 个；libmosquitto 拉 libssl/libcrypto/libcares；libmicrohttpd 拉 libgnutls；notcurses 拉 ffmpeg 六件套+core

### 自检系统优化逻辑（先生指示——全捆后自检不该再"装依赖"）
- 现状：check_item_dependencies 只查 command -v python3；check_python 还在查 flask（过时）；7 项检查 fix_func 全 NULL（只报不修）；env_bootstrap copy_python_scripts 面向宿主
- 优化：判定**捆绑形态**（包内 lib/+python/ 存在）→ 捆绑查 lib/manifest.json 齐全性 + dlopen 试加载 + venv import 验证 → **缺失才触发 fix**（不无条件安装）
- **修复链修正（2026-08-13 先生"所以"确认）**：fix_func 全代码库无调用方（仅 check_manager.h:49 定义）——**不新建 fix_func 链**，检测失败改调 `active_repair_trigger()` 走现有 socket→Python repair engine 诊断链
- check_python 去掉 flask；copy_python_scripts 捆绑形态跳过

### 部署前争议点裁决（2026-08-13 先生逐条）
| 争议点 | 裁决 |
|---|---|
| A1 服务端 CI | **建**：服务端 git 化上 GitHub → workflows 矩阵（x86_64 原生 + arm64 经 docker/setup-qemu-action 容器编译）→ 各出 tar.gz Release asset |
| A2 fix_func | 不新建链，修复走 active_repair（见上） |
| B1 HA 三通道归属 | **B REST 控制 + C WS 实时事件订阅 = Python 端实现**（ai_server.py）；C 端只留 A MQTT 上报（mqtt_ha.c 已有）——C 端改动最小化 |
| B2 espeak/piper 落点 | **随包 bin/ 携带**（全捆逻辑一致，解压即用；piper 模型 ~60MB 计入包体） |
| C1 #3 聊天故障 | **确认为 0.2.1 新问题**（非 0.1.9 400 旧病复发——0.2.0 已修复）——部署前需复现定位 |
| C2 0.2.0 部署 | ✅ 先生环境已完成（5 文件复制+重启+App 0.2.0） |
| C3 服务端版本控制 | git 化是 CI 前提——【待定】并入 LINGOS-APP 子目录 vs 新开 LINGOS-server 仓库 |

### 部署前争议点二轮裁决（2026-08-14 先生逐条——记录不部署）
| 争议点 | 裁决 |
|---|---|
| Q1 服务端 git 化 | **新开独立仓库，定名 `LINGOS-server`**（2026-08-14 先生定名）——CI workflows 建于此仓库 |
| Q2 离线缓存清理策略 | **全量无限缓存**（不做最近 N 会话限制） |
| Q3 离线缓存隐私 | **加密**（sqflite 加密需 sqlcipher 方案；连带成本：CI 需带 libsqlitecipher 原生库、密钥管理走 Android Keystore——实施时评估） |
| Q4 sqflite 引入 | **确认引入**（连带 9.1 session_list 加 token、9.4 响应按 cmd 匹配——同批改服务端，一次到位） |
| Q5 0.2.1 批次范围 | **一批次全做**（13 项 + HA 集成 + 依赖全捆打包 + 版本号 5.1.2.6-rc 已改源码） |
| H4 部署形态 | **CI 出全捆包**——先生环境为纯部署方（装 CI 产物解压即用，本地不再 make 编译）；版本号生效走 CI 包，先生环境现有二进制待 0.2.1 包替换 |

### 包体结构（定稿）
```
LINGOS-<ver>-<arch>-linux/
├── lingos_linux          # rpath=$ORIGIN/../lib
├── lib/                  # 全捆 .so（到 glibc 为止）+ manifest.json（CI 生成，自检用）
├── python/               # venv --copies
├── install_deps.sh       # 降级为环境检查：glibc 版本 + 报缺项
└── DEPENDENCIES.md
```

### 风险记录
- 捆绑 OpenSSL 不随系统打补丁（自用可接受）
- 包体膨胀：x86_64 libcurl 全家桶 + notcurses/ffmpeg 估算 100~200MB（出包后实测记录）
- 产出：DEPENDENCIES.md 初稿（编译期/运行期/可选三层）已随本次记录生成

---

## 记录状态

| 项 | 状态 |
|---|---|
| 服务端 0.2.0 | ✅ 已实施部署（llm_unified/voice_service/上下文引擎） |
| App 0.2.0 | ✅ CI 成功，Release v0.2.0 已发布（4 包） |
| 本批 App 问题 | 📝 **仅记录**（待先生指示后再实施） |
| 依赖打包（#12/#13） | 📝 **定稿仅记录**（全捆方案+自检优化——2026-08-13） |
| 目标版本 | 0.2.1（下批实施后定） |
