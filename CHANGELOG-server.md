# LING OS 服务端更新日志（CHANGELOG）

版本线：发行版 0.0.x（独立于内部 LN-B-x.x.x.x-rc）· 包名：LINGOS-<版本>-server-<架构>.tar.gz

---

## [0.0.4] - 2026-08-23（配置优化批次——先生裁决定稿实施）

### 修复（Bug Fixes）
- **健康自检"配置完成但被误判未完成"根因修复**（先生重点指出）：
  - 根因：`configured_at` 字段仅初始化=0，**从未置位**→ 自检 `configured_at==0` 永远判定"配置不完整，需要运行配置向导"→ 用户被迫重新配置
  - 修复：`config_core_mark_configured` 写 state.json 同时**同步内存 configured_at**；`config_core_load` 从 state.json 恢复 `system_configured`→configured_at（重启后自检也通过）
- **配置向导回车逻辑**（先生裁决）：有默认值→回车=采用默认；**无默认值→回车提示"尚未选择"**，不进入下一阶段（原为"空输入→降级切换模式"）
  - wizard_engine.c SELECT/INPUT 步骤均适配；补 uart.h include

### 新增（Features）
- **日志体系重构（先生定稿——服务端/客户端拆分）**：
  - `/Debug` 目录删除 → 统一 `/log` 单文件（C 端 log_extra.c + Python ai_server.py）
  - 文件存储 JSON 四字段：`time(ISO8601毫秒带时区)/id(自增)/level/txt(原终端内容含[LEVEL][模块][函数]标识，无颜色码)`
  - 终端显示：保留原格式仅时间改 `[时间]`；颜色=低级**冷灰**/WARN**黄**/ERROR**鲜红**（原 INFO 绿/DEBUG 青）
  - 文件保存开关 `log_set_file_output`/`set_log_file_enabled`：默认开=DEBUG 全量；关=仅 WARN+
- **指令翻译层（动词子命令，先生定稿 git/docker 风格）**：
  - 形式二（动作 领域）：`list model` / `list session` / `view status` / `query balance` 等
  - 形式一变体（领域 动作）：`model switch <id>` / `session create|delete|history` / `skill enable|disable` / `memory search|write` / `ha status|states` / `voice usage` / `provider list`
  - WS 机器命令（session_list 等 JSON 字面量）**保留原样**；难适配指令（token 族/allow-high-risk/logdump 专名）**保留原样**
  - 实现：shell.c `handle_verb_command` + `send_ai_command`（daemon socket 转发，select 5s 超时防阻塞）
- **增量包 JSON manifest（先生定稿约定格式）**：
  - 新建 update_incremental_json.c/h：`{base_ver, target_ver, files:[{path,action:add/mod/del,size,hash}]}`
  - base_ver 匹配校验（不匹配拒绝 -2）+ 文件 sha256 校验 + 应用前备份（.inc_backup 可回滚）+ add/mod 先 del 后
- **技能安装（OpenClaw 式，先生定稿）**：
  - 新建 skill_install.py：技能包格式 `SKILL.md + handler.py + requirements.txt`（SKILL.md YAML front-matter：name/description/risk/handler）
  - **内置技能**（.builtin 标记）不可删除；**自定义技能**可增删
  - 命令：`skill_install`(src) / `skill_uninstall`(name) / `skill_list_custom` / 自动注册到 SKILL_REGISTRY（调用适配免手写）
- **三级记忆（先生定稿纳入本批）**：
  - L1 main（重要常驻，`[L1][重要]` 前缀自动注入）/ L2 working（`[L2]` 工作记忆）/ L3 external（默认，按需检索）
  - memory_write 支持 `level: l1/l2/l3`（importance=high 自动 L1）；memory_retrieval 支持 level 过滤 + find_working

### 已知（Known）
- 端口不可在配置中更改（先生裁决）——改端口用特定指令族（port 指令待后续批次）
- App 端日志：同格式（time/id/level/txt），默认不保存、导出落盘、显示最近 100 行——App 端实施在 App 仓库批次

---

## [0.0.3] - 2026-08-15（DeepSeek 扩展批次）
- #3 空回复/工具调用 400 根因修复（reasoning_content 回传/流式中断检测/空回复重试）
- DeepSeek 思考开关/强度（仅 DeepSeek 系）、balance_query、model_list_query
- provider 同步修复（provider_add 同步）

## [0.0.2] - 2026-08-14（0.2.2 批次）
- 同步协议（sync_full/delta/归属/冲突）、HA 部署引导、vision 六层、插件化预留

## [0.0.1] - 2026-08-14（0.2.1 批次）
- HA 集成（ha_integration.py + 5 命令 + ha_control 权限）、9.4 cmd 匹配、9.1 token 统计、自检优化、CI 双架构全捆包
