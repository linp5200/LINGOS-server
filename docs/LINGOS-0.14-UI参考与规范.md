# LING OS 0.14 · UI 参考与规范（检索版）

> 2026-11-14 先生要求：每次做 AI/UI 前后必须真的上网检索，不许自己凭空造规范。
> 本文是实际抓取公开资料后的结论汇总，作为 [测试工作台与反馈体系计划](LINGOS-0.14-测试工作台与反馈体系计划.md) 的依据。
> 未读到的内容明确标“未验证”，不冒充规范。

## 一、检索清单（实际打开过的）

| # | 来源 | 类型 | 关键结论 |
|---|---|---|---|
| 1 | [Motion & Micro-Interactions Reference](https://raw.githubusercontent.com/curiositech/windags-skills/main/skills/beautiful-gui-design/references/04-motion-and-microinteractions.md) | 社区规范（Markdown 原文） | 时长 100–300ms；进入 ease-out、退出 ease-in；只动 transform/opacity；同时动效 ≤3；stagger ≤50ms/项、总 ≤500ms；未知等待用 spinner；禁止假进度 |
| 2 | [SSW: toast notifications](https://raw.githubusercontent.com/SSWConsulting/SSW.Rules.Content/main/public/uploads/rules/toast-notifications/rule.mdx) | 社区规范（原文） | toast 只用于非阻断反馈；表单错误用行内、页面错误用 banner、破坏性用 dialog；底部居中；成功/提示 `role=status`、错误 `role=alert`；自动消失 4–6s；错误 toast 不自动消失 |
| 3 | [Accessible Modal Dialogs（TestParty）](https://testparty.ai/blog/modal-dialog-accessibility) | 社区实践文 | 打开移焦、焦点陷阱、Esc 关闭、关闭归还焦点、`role=dialog aria-modal aria-labelledby`、背景 inert |
| 4 | [WCAG 2.2 SC 2.4.13 Focus Appearance（香港数字政策办公室）](https://www.digitalpolicy.gov.hk/en/our_work/digital_infrastructure/digital_inclusion/accessibility/promulgating_resources/handbook/appendix_a/a21_2413_focus_appearance.html) | 官方释义 | 焦点指示面积 ≈ 2 CSS px 外框，与未聚焦状态对比 ≥3:1 |
| 5 | [Nomi UI 基本规则（36 条，草案）](https://raw.githubusercontent.com/aqm857886159/Nomi/main/docs/research/2026-10-07-ui-basic-rules.md) | 社区规范（中文、含官方引用） | 决定栏：取消在左、主动作最右；破坏性动作固定最右/最左并隔开；✓ 只表状态；按钮写具体动词；禁用要有原因；点击目标 ≥24（AA）；焦点可见；纯图标按钮要有名字；对话框要有名字；同类控件同组件 |
| 6 | [App Empty States: Best Practices](https://www.appypie.com/blog/app-empty-states) | 行业文章 | 空态三类（首次/无结果/失败）；五要素（插图或图标、邀请式标题、价值副标题、唯一主 CTA、可选次路径）；不要把空态写成错误 |
| 7 | [B站《UI高级感来源于设计对精度的控制》](https://www.bilibili.com/video/BV1SFWr6oEeC)（1:47，字幕已读） | 视频 | 高级感=从容感（元素聚焦、信息不挤）+秩序感（层级主次）+克制感（描边/细节精确到刚好） |
| 8 | [Material 3 Easing & Duration](https://m3.material.io/styles/motion/easing-and-duration/tokens-specs) | 官方（页面正文脚本渲染，只读到标题） | 采用官方提供的标准/减速/加速曲线概念；**具体 token 数值未从该页读到，引用时不得声称来自 M3 原文** |

B 站还检索到（未逐条观看，仅作素材登记）：
- BV1jZ4y1a7kW「全局美化toast弹窗」、BV15m4y1k7h9「前端全组件系列-对话框弹窗组件」、BV1jReH6yE7k「OriginOS 7 流畅动效解析」、BV1mP4y1T7zm「Apple Watch UI 动效解析」、BV1FZoEYKELJ「一分钟解决N卡弹窗通知」。

## 二、反馈模式选择矩阵（照 SSW 表 + 本项目）

| 场景 | 用什么 | 不用什么 | LING OS 例子 |
|---|---|---|---|
| 轻量成功（复制、保存、开关生效） | toast（成功） | 大模态 | 保存提供商、复制 root key、开关偏好 |
| 需要撤销 | toast + 撤销按钮，停留 ≥6s | 确认框 | 删除单条事件（后续做） |
| 表单字段错误 | 字段旁行内错误 + 首次无效字段聚焦 | toast | 添加用户、改密码 |
| 页面级错误（后台不可达、服务未启动） | 顶部 banner + 重试 | toast | `/api/health` 失败、命令服务未启动 |
| 破坏性/阻塞（开锁、删用户、停安全项） | 底部确认条或模态 | toast | 开锁、删设备、关闭危险内核项 |
| 需要阅读的内容（root key、详情） | 模态 | toast | 首配 root key、备份恢复说明 |
| 未知等待 | 按钮 busy / 骨架 / spinner | 假进度条 | 保存、导入插件、刷新 |

## 三、动效规范（token 化）

| token | 值 | 用途 |
|---|---|---|
| `--dur-micro` | 100ms | 按压、开关、勾选 |
| `--dur-local` | 160ms | toast、banner、tooltip、输入聚焦 |
| `--dur-panel` | 200ms | 模态、终端、设置子页 |
| `--dur-page` | 260ms | 视图切换 |
| `--ease-standard` | `cubic-bezier(.2,0,.8,1)` | 通用 |
| `--ease-decelerate` | `cubic-bezier(0,0,.2,1)` | 进入 |
| `--ease-accelerate` | `cubic-bezier(.3,0,1,1)` | 退出 |

规则：
1. 只动 `transform` / `opacity`；不动 width/height/top/left。
2. 进入用 ease-out/decelerate，退出用 ease-in/accelerate；spinner 之外禁止 linear。
3. 同时主动画 ≤3 个；列表 stagger 每项 ≤50ms、总 ≤500ms，只首次进入；10 项以上只前 5 项 stagger。
4. 未知等待用 spinner 或骨架；有真实分母才用进度条。
5. `prefers-reduced-motion` 与“动效强度=关闭”一律压到 0.01ms（不是删除反馈）。
6. 克制：边框 1px、阴影只一档；一个屏幕一个主视觉焦点（B站“三感”）。

### 动效强度偏好（用户可调）

| 档位 | 行为 |
|---|---|
| 关闭 | 时长 0.01ms，保留状态变化 |
| 精简 | 只保留 100ms 微反馈；面板/页面直接出现 |
| 标准（默认） | 按上表 token |
| 丰富 | 加 stagger、模态缩放、终端展开、按钮涟漪；仍禁 parallax |

存储：`lo-prefs.motionLevel = off|lite|std|rich`；设置 → 外观与偏好。

## 四、无障碍硬指标

- 文字对比 ≥4.5:1（大字 3:1）；控件边界/图标 ≥3:1。
- 焦点环：≥2 CSS px 视觉厚度、与未聚焦态 ≥3:1；不摘 `:focus-visible`。
- 点击目标：WCAG AA ≥24×24；LING OS 触屏目标 ≥44×44（移动端）。
- toast：成功/提示 `role=status`，错误 `role=alert`；错误不自动消失；可手动关闭。
- 模态：`role=dialog aria-modal=true aria-labelledby`；焦点陷阱；Esc；关闭归还焦点；背景 inert。
- 图标按钮必须有 `aria-label`；对话框必须有名字。
- 文案：按钮写具体动词（保存/删除这张），不写“确定/OK”；取消固定写“取消”。

## 五、空态五要素（照 Appy Pie）

1. 图标或插图（品牌感，不用“红叉文件夹”）。
2. 邀请式标题（“还没有设备”，不是“无数据”）。
3. 一句价值副标题（“添加后就能在这里看到状态”）。
4. 一个主 CTA（“加一台”/“去发现”）。
5. 可选次路径（“或查看帮助”）。

三类空态分别处理：首次使用（引导）、无结果（给替代/清筛选项）、失败（说明原因 + 重试）。

## 六、LING OS 本轮落地映射

| 规范 | 落地 |
|---|---|
| toast 四类 + role | `toast(msg, kind)` → `role=status/alert`、错误不自动消失、可点关闭 |
| 模态无障碍 | `modalBox`：焦点陷阱、Esc、inert 背景、归还焦点 |
| 决定栏顺序 | `confirmBox`：取消在左、主动作在右；危险动作非默认焦点 |
| 图标按钮 | 全站补 `aria-label`（纯图标按钮） |
| 点击目标 | 移动端 44px；内核行整行 label；chip ≥36/44 |
| 空态 | 设备/文件/插件/事件四处按五要素重写 |
| 动效 token + 强度偏好 | `app.css` 变量 + `body.motion-*`；设置四档 |
| 日志底色 | ERROR 红底白字、WARN 黄底黑字、INFO 绿底黑字、DEBUG 灰底白字 |
| 测试工作台 | GUI 连点版本 3 次；终端 Ctrl+A/Q；终端打开隐藏日志区 |

## 七、未验证 / 限制

- Material 3 正文未读到，只用了“标准/减速/加速曲线”概念；具体数值来自引用它的社区规范，不标注为 M3 原文。
- B 站除 BV1SFWr6oEeC（字幕已读）外，其余仅检索到标题，未逐条观看。
- Nomi 的 36 条规则是社区草案，其中部分条文自述“非官方”；本文只取与 LING OS 相关且多来源互证的条目。
- 空态/暗色仅读了行业文章与部分来源；暗色 token 细节未从官方原文读到，落地时用项目现有 token 验证对比度。
- 未做真机 VoiceOver/TalkBack、iOS 安全区、软键盘实测。

## 八、验收（与计划一致）

1. `review-ui ≥ 9.0` 且 `review-code ≥ 9.0`，P0/P1=0。
2. 390×667/844、1440×900 无溢出/遮挡；终端与模态键盘闭环。
3. 日志三级底色实拍；toast 四类实拍；模态焦点归还实测。
4. 空态四例、首配跳过、插件导入实测。
5. 对照本文每一条“落地映射”逐项打勾，未做写“未验证”。
