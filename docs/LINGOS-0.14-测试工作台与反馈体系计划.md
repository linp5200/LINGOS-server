# LING OS 0.14 · 测试工作台与反馈体系计划

> 2026-11-14 · 先生反馈：成熟度不足、部分功能未并入、动画缺失、日志风格不统一（错误红底，其他级别也应底色）、反馈太弱（弹窗/文字提醒/清提示/警告/模态/气泡/确认框）、测试版需要 GUI 与终端两份测试工作台、首配缺少可选项且要支持插件导入、每次 AI/UI 验证前应先用 URL 技能检索参考。
>
> **状态：计划待批准，未部署；验收门槛：review-code 与 review-ui 均 ≥ 9.0 分**。本机已有原型改动（server 日志底色、首配可选项、插件 API、Web 端测试工作台/终端骨架），批准后再补 CSS/联调/测试/版本/CI。

## 零、调研依据（2026-11-14 实际检索，完整清单见 [UI 参考与规范](LINGOS-0.14-UI参考与规范.md)）

| 来源 | 关键结论 | 落到本项目的规则 |
|---|---|---|
| [Motion & Micro-Interactions Reference](https://raw.githubusercontent.com/curiositech/windags-skills/main/skills/beautiful-gui-design/references/04-motion-and-microinteractions.md) | 动效时长基线 100–300ms：微反馈 100ms、局部 150ms、页面切换 200ms、全屏 300ms；进入用 ease-out、退出用 ease-in；只动 `transform/opacity`；同时动效 ≤3 个；列表 stagger 每项 ≤50ms、总 ≤500ms；不确定等待用 spinner，禁止假进度 | 全站动效 token 化（见 3.2）；模态/终端进入 180–200ms；列表 stagger 仅首次、最多前 5 项 |
| [Accessible Modal Dialogs（TestParty）](https://testparty.ai/blog/modal-dialog-accessibility) | 模态必须：打开时移焦、焦点陷阱、Esc 关闭、关闭后焦点归还触发元素、`role=dialog aria-modal aria-labelledby`、背景 inert/aria-hidden | `modalBox` 重写为焦点陷阱 + 背景 `inert` + 焦点归还；确认条保留 |
| [Material 3 · Easing & Duration](https://m3.material.io/styles/motion/easing-and-duration/tokens-specs) | 官方提供了标准/强调/减速/加速曲线与时长 token，强调“有无必要”而非装饰 | 采用 `--ease-standard: cubic-bezier(.2,0,.8,1)`、`--ease-decelerate: cubic-bezier(0,0,.2,1)`；禁止 linear（除 spinner 旋转） |
| [B站：UI高级感来源于设计对精度的控制](https://www.bilibili.com/video/BV1SFWr6oEeC)（1:47，字幕已读） | 高级感来自三件事：从容感（元素聚焦、信息不拥挤）、秩序感（层级有主次）、克制感（描边/细节“精确到刚刚好”） | 每屏只允许 1 个主视觉焦点；动效只服务“方向/反馈/主次”；边框与阴影保持 1px 级克制，不靠加粗堆细节 |

> 完整检索记录、36 条社区规则映射、空态五要素、无障碍硬指标与“未验证”清单，见 [LINGOS-0.14-UI参考与规范.md](LINGOS-0.14-UI参考与规范.md)。
> 说明：以上为公开资料结论，非本项目既有规范；本轮把它们固化成 LING OS 动效/反馈 token，并在实现后用 review-code / review-ui 复核。

## 一、现状与差距

| 项 | 现状 | 差距 |
|---|---|---|
| 反馈 | 只有底部 toast（单一样式）+ 底部确认条 | 无分级 toast、无居中模态、无横幅/清提示/警告样式、无 loading 态 |
| 动画 | 视图/卡片入场、骨架、toast；尊重 reduced-motion | 模态/终端/开关/列表交互缺动效；缺少“反馈出现”的节奏 |
| 日志 | 时间 · 级别 · IP · 方法 · 路径 · 状态 · 耗时；ERROR 红底、WARN 黄字、INFO 绿字、DEBUG 灰字 | 各级别未统一底色；路径控制符已清洗 |
| 测试工作台 | 无 | 需 GUI（关于连点版本 3 次）+ 终端（Ctrl+A 进 / Ctrl+Q 退），终端打开时隐藏日志区，导航/设置仍可用 |
| 首配 | 只有用户名/密码 | 缺设备名、语言、隐私等级、插件导入；全部应可跳过 |
| 插件 | 无 | 需清单导入（JSON）、列表、启停展示；服务端只存元数据，不执行代码 |
| 功能并入 | 设置子页/内核 61 项/AI 三协议/摄像头/通知/文件/系统命令已并入 | 通知渠道实测、摄像头预览、文件预览/上传、PWA 离线仍待补 |
| 版本 | 0.13.3 已部署，0.13.4 待发 | 每次推送 bump，包名带 +short-sha |
| 调研 | 未做 | 每次 AI/UI 改造前先 web_search / B站取参考并记录结论 |

## 二、目标（本轮可验收）

1. **反馈体系**：四类 toast（成功/提示/警告/错误）、居中模态（提示/详情/一次性凭据）、底部确认条（危险操作）、内联横幅（页面级错误/提示）、加载态（按钮 busy、骨架、进度）。
2. **动画**：模态缩放淡入、toast 滑入、列表 stagger、开关滑动、终端展开/收起、页面切换；`prefers-reduced-motion` 与“界面动效”偏好一律降级。
3. **日志统一**：ERROR 红底白字、WARN 黄底黑字、INFO 绿底黑字、DEBUG 灰底白字；非 TTY 自动去色。
4. **测试工作台**：
   - GUI：设置 → 关于 → 版本号连点 3 次进入；可跑健康/设备/内核/插件/AI 后端真实请求，输出 JSON；可打开终端。
   - 终端：`Ctrl+A` 进入、`Ctrl+Q` 返回；命令 `help / health / devices / options / plugins / cmd <name> / clear / quit`；打开时隐藏日志区（事件/命令输出），导航与设置仍可用。
5. **首配**：高级区可选（设备名/语言/隐私等级/插件 JSON），全部可跳过；完成后用模态展示一次性 root key（当前实现会立刻隐藏，用户看不到）。
6. **插件**：设置 → 插件页列出已装清单；导入 JSON 校验 id/entry；导入成功 toast + 列表刷新；不执行插件代码（安全边界）。
7. **功能补充（并入清单第一批）**：通知配置实测入口、摄像头预览开关、文件上传/预览、AI provider 测试按钮、PWA 离线清单。
8. **交付**：版本 0.13.4（或后续递增）；CI 三套全绿；rootfs 包名带 `+<sha>`；子代理 **review-code 与 review-ui 均 ≥ 9.0** 且 P0/P1=0，未达 9.0 不部署、继续修。

## 三、交互规范

### 3.1 反馈组件

| 组件 | 用途 | 规格 |
|---|---|---|
| `toast(kind)` | 轻提示：保存成功、复制、开关生效 | 底部居中；成功绿边、提示墨边、警告黄边、错误红边；成功 2.6s、错误 4.2s；滑入 180ms |
| `modalBox` | 需要读的内容：root key、详情、确认说明 | 居中卡片 + 遮罩；`role=dialog aria-modal`；Esc/遮罩关闭；焦点陷阱与归还；缩放淡入 180ms |
| `confirmBox` | 破坏性操作：删除设备/用户、开锁、停用安全项 | 底部确认条；默认焦点在“算了”；Enter 不触发危险动作 |
| `banner` | 页面级：后台不可达、命令服务未启动、AI 未配置 | 顶部内联，带重试按钮；不自动消失 |
| `busy` | 按钮执行中 | 按钮禁用 + spinner + 文案“执行中…”；失败恢复并 toast(err) |
| `skeleton` | 首屏读取 | 现有骨架扩展至设置/文件/安全 |

### 3.2 动效 token 与“动效强度”偏好

| token | 值 | 用途 |
|---|---|---|
| `--dur-micro` | 100ms | 按钮按压、开关、勾选 |
| `--dur-local` | 160ms | toast、tooltip、banner、输入聚焦 |
| `--dur-panel` | 200ms | 模态、终端、设置子页 |
| `--dur-page` | 260ms | 视图切换、全屏面板 |
| `--ease-standard` | `cubic-bezier(.2,0,.8,1)` | 通用 |
| `--ease-decelerate` | `cubic-bezier(0,0,.2,1)` | 进入 |
| `--ease-accelerate` | `cubic-bezier(.3,0,1,1)` | 退出 |

偏好里新增 **动效强度** 四档（存 `lo-prefs.motionLevel`，默认“标准”）：

| 档位 | 行为 |
|---|---|
| 关闭 | 全部时长 0.01ms（保持可用性，不删除反馈） |
| 精简 | 只保留 100ms 级微反馈；模态/视图直接出现 |
| 标准 | 按上表 token |
| 丰富 | 在标准基础上加入 stagger、模态缩放、终端展开、按钮涟漪；仍禁止 parallax/scroll-jack |

约束：同时进行的主动画 ≤3 个；列表 stagger 仅首次进入、每项 ≤50ms；`prefers-reduced-motion` 强制等同“关闭”；不在 hover 上做持续动画（触屏 `@media (hover:none)` 关闭）。

### 3.3 日志（终端）

```
12:04:59  INFO  127.0.0.1 "GET /api/health HTTP/1.1" 200 - 14.8ms
```
- ERROR 红底白字、WARN 黄底黑字、INFO 绿底黑字、DEBUG 灰底白字；`NO_COLOR` 或非 TTY 去色。
- 路径/UA 中控制符替换为 `?`，长度截断 400。

### 3.4 测试工作台

- **GUI**：关于页版本行改为可点击；1.5s 内连点 3 次切换；面板含 6 个真实接口按钮 + 输出 `<pre>` + “打开终端”。
- **终端**：底部覆盖层（约 45vh），输入框 + 输出区；`Ctrl+A` 打开（输入框/文本域聚焦时不拦截），`Ctrl+Q` 关闭；打开时 body 加 `term-open` 隐藏 `.timeline`/`.cmd-out` 日志，导航与设置保持可用；错误命令给出用法。
- **边界**：非 admin 也能打开终端，但 `/api/cmd` 会 403，终端如实显示。

### 3.5 首配（OOBE）

- 步骤 1 账号密码（必填，≥8 位）。
- 步骤 2 高级（可跳过）：设备名、语言、隐私等级 L1–L4、插件 JSON 导入。
- 完成后模态展示 root key（一次性）+ “进入桌面”。

## 四、实施步骤

### P0（本轮，约 1 个会话）
1. 服务端：日志底色（已原型）、setup 可选字段（已原型）、`GET/POST /api/plugins`、`/api/plugins/import`（已原型）。
2. Web：toast 分级、modalBox、banner、busy；关于页版本连点 + GUI 面板；终端覆盖层；插件页；首配高级区 + root key 模态。
3. CSS：模态/终端/动画/底色/触控尺寸。
4. 测试：
   - 单测：setup 可选字段落库、插件导入校验（非法 id/entry）、插件列表、非 admin 403；
   - Playwright：390×667/844、1440×900 下设置子页、连点进入工作台、Ctrl+A/Q、首配跳过、插件导入；
   - 日志：启动后截取 INFO/WARN/ERROR 三色样例。
5. 版本 bump → 0.13.4；CI（ci/build/rootfs）；子代理 review-code / review-ui 打分，**均 ≥9.0 才算完成**；`docs` 记录。
6. 手机安装由先生启动；安装后我只看 `/api/health.version` 与日志，不代跑。

### P1（下一批）
- 通知渠道实测（webhook 本地接收、SMTP 可选）、摄像头预览实图、文件预览/上传（二进制安全）、PWA 离线。
- 动画细化：页面切换方向、终端高度动画、列表 stagger 限制首次。
- 键盘无障碍：Tab 顺序、aria-current、确认框焦点归还复核。

### P2（可选）
- B站/URL 参考专项：UI 反馈与动效参考整理进 docs；按参考微调节奏与配色。
- 事件聚合/中文化、系统页收纳进高级、版本信息单一来源。

## 五、验收矩阵

| 项 | 验收 |
|---|---|
| 反馈 | 保存/失败/警告/复制各出现对应 toast；删除弹确认；root key 只出现一次于模态 |
| 动画 | 关闭“界面动效”后无动画；reduced-motion 生效；模态/终端有入场 |
| 日志 | 启动日志三级别底色正确；非 TTY 无 ANSI |
| 工作台 | 关于连点 3 次出现；6 个按钮真实返回；终端 Ctrl+A/Q 正常；终端打开时日志区隐藏、设置可用 |
| 首配 | 不填高级项可完成；填隐私等级后 `/api/security` 反映；插件 JSON 导入后列表出现 |
| 数据 | 全站无模拟数据；失败显示错误而非空态 |
| 响应式 | 320/390/1440 无横向溢出；底部 Tab 不遮输入 |
| 版本 | VERSION=0.13.4；包名 `...+<sha>`；CI 三绿；rootfs 可装 |
| 评分 | review-code ≥ 9.0 且 review-ui ≥ 9.0；未达则继续迭代，不部署 |

## 六、风险与回滚

- 插件导入只存元数据、不执行代码，避免 RCE；entry 仅允许 `[A-Za-z0-9_./-]`。
- 终端命令走 `/api/cmd`，非 admin 403；不新增 shell 执行接口。
- 首配 root key 模态只出现一次；localStorage 清理由登出统一处理。
- 回滚：`git revert <提交>` 后重跑 CI；手机保留 `rootfs.legacy-*` 目录可回退。

## 七、评分委托规则（2026-11-14 先生纠正）

- **不得向子代理透露目标分数/门槛**（如“目标 ≥9.0”“部署前必须通过”），否则会诱导其压线迎合。
- 评分 prompt 只写：独立审查、只读、按 P0/P1/P2 给证据、给 0-10 分；不写验收线、不写“最后一步”。
- 是否达标由我在收到报告后判断；未达标就修完再请同一会话复审，复审仍不透露门槛。
- 评分委托也要避免“必须达到 X 分”这类压力措辞，保持评分独立。

## 八、发布流程（每次推送）

1. `scripts/bump-version.sh patch`。
2. 本地静态校验 + 单测 + Playwright。
3. 推送 → CI 三套（ci / build / rootfs）。
4. 子代理 review-code / review-ui 打分：**均 ≥9.0** 且 P0/P1 清零，才可部署。
5. 先生决定是否安装到手机（由先生自己运行 `lingos` 看日志）。
