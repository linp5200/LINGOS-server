# LING OS 0.2.0 同步与编译说明（先生环境部署指南）

日期：2026-08-12 | 版本：0.2.0（App 0.2.0+11 / 服务端 LN-B-5.1.0.0-rc0.2 兼容）

---

## 一、需要同步的文件（先生环境）

### 1. Python 文件（本次 5 个有改动——复制到 /LINGOS/bin/）

| 文件 | 状态 | 说明 |
|---|---|---|
| **ai_server.py** | 🔴 改（核心） | 统一 LLM 层包装 + 上下文引擎 + 17 类错误 + 新命令 + HTTP 8088 |
| **llm_unified.py** | 🆕 新增 | 统一 LLM 调用层（必须一起复制——ai_server 启动即 import） |
| **voice_service.py** | 🆕 新增 | 语音服务（必须一起复制） |
| **skill_handlers.py** | 🟡 改 | memory_write 加 importance（[重要] 双记忆） |
| **memory_retrieval.py** | 🟡 改 | important_only 过滤 + 索引增强 |

> 其余 15 个 Python 文件（agent_orchestrator / sub_ai_scheduler / skill_loader 等）**无改动**，可不动。
> 稳妥起见也可整体复制 src/python/ 全部（先生约定：以 src/python/ 为准）。

### 2. C 二进制（本次无 C 改动——不需要重新编译）

websocket_server.c / lingos_linux / lingosd / lingos_supervisor **均未改动**——直接沿用现有二进制。

### 3. Makefile（已修改——先生环境自动装新文件）

`install_python_script` 目标已加入 `llm_unified.py voice_service.py`（原清单漏了新文件会导致复制缺失 → ai_server 启动 ModuleNotFoundError）。
三处已同步：先生环境 Makefile / 主仓 07_构建与工具/Makefile / 编译树 lingos-src/Makefile。

---

## 二、部署步骤（先生环境）

### 方式 A：cp 直接复制（推荐——最快）
```bash
cp /LINGOS_Aach64_termux_Proot_Linux/src/python/ai_server.py \
   /LINGOS_Aach64_termux_Proot_Linux/src/python/llm_unified.py \
   /LINGOS_Aach64_termux_Proot_Linux/src/python/voice_service.py \
   /LINGOS_Aach64_termux_Proot_Linux/src/python/skill_handlers.py \
   /LINGOS_Aach64_termux_Proot_Linux/src/python/memory_retrieval.py \
   /LINGOS/bin/
chmod +x /LINGOS/bin/*.py
```

### 方式 B：make 自动安装（可顺带验证 C 编译无碍）
```bash
cd <先生环境源码目录>
make install_python_script   # 只装 Python（含新文件——已改清单）
# 或完整构建：make all（C 端本次无改动，产物不变；跑一遍确认无回归）
```

### 重启 AI server
```bash
# 方式：杀旧进程 → 重启（先生环境惯例）
pkill -f ai_server.py
<启动脚本>   # 按先生环境既有方式重启
```

### 首次启动自动生成
- `/LINGOS/system/config/provider.json` 首次启动由旧 ai_config.json **自动生成**（legacy 兜底：DeepSeek + Ollama 两个 provider，deepseek 为 active）
- 兼容扁平/嵌套两种 ai_config.json 格式（先生环境若为扁平 `deepseek_api_key` 顶层——已兼容）

---

## 三、App 0.2.0（编译与安装）

### CI 构建
- 已推送 GitHub（commit 131d421 + 61328c1）——CI 自动构建 4 包（arm64-v8a / armeabi-v7a / x86_64 / all）
- 构建成功后从 **Release** 下载 `LINGOS_<架构>_android_beta_0.2.0.apk`
- ⚠️ **CI 状态需确认**（本次新增 record/audioplayers 依赖 + 5 个 Dart 文件改动——如构建失败见第四节排查）

### 本地构建（可选）
```bash
flutter pub get
flutter build apk --release
```

---

## 四、App 编译错误排查清单（本次改动涉及面）

| 可能错误 | 排查点 |
|---|---|
| record/audioplayers 版本冲突 | pubspec 已定 ^7.1.1 / ^6.8.1——若 CI 报依赖解析失败，查 pub.dev 兼容 SDK |
| providers_screen 改为 Consumer 后未用 import | 已清理（dart:async 移除）——CI analyze 严格需零 warning |
| chat_screen 新增语音方法未用变量 | 检查 `_hostFromWs` / `_toggleRecord` / `_speakAi` 引用完整 |
| const 条件表达式 | CI 旧 Flutter SDK 不支持 const 条件——本次未新增此类 |
| activeThumbColor | CI 旧 SDK 用 activeColor——本次未新增 Switch 主题 |

---

## 五、验证清单（部署后）

1. `python3 -c "import llm_unified, voice_service"` —— 无报错
2. 启动 AI server 日志出现 `provider.json` 生成 + `Voice HTTP server on :8088`
3. App 连上后：AI 配置 → LLM/MLM 提供商 → **模型切换区块**显示 deepseek/ollama
4. 复测：并行工具调用（system_info + system_memory）→ **不再 HTTP 400**
5. 语音：对话输入栏麦克风 → 录音 → STT；AI 回复点"朗读" → TTS
6. 长对话：状态行出现"📋已压缩"提示条（70% 预算触发）
