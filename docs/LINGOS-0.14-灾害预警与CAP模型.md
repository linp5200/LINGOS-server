# LING OS 0.14 · 灾害预警与 CAP 数据模型

> 2026-11-14 · 依据先生裁决：灾害分级按国际标准。
> 本文只定数据模型与分级通知规则，不含具体气象数据源采购。

## 一、采用的国际标准

| 标准 | 用途 | 本文用法 |
|---|---|---|
| **OASIS CAP 1.2**（Common Alerting Protocol） | 告警交换格式，国际通用 | 预警对象的字段直接照搬 |
| **EM-DAT / CRED** | 灾害类型学（自然/技术） | `category` + `type` 分类 |
| **UNDRR Sendai Framework** | 灾害风险减轻口径 | 统计与复盘口径 |
| **ISO 22320** | 事件响应管理 | 事件生命周期（新建/升级/解除） |
| **ISO 22301** | 业务连续性 | 断网/断电降级策略 |
| **FEMA IPAWS / WEA、EU-Alert** | 公众告警分级实践 | severity × urgency → 通知强度 |

## 二、CAP 字段（LING OS 内部模型）

```json
{
  "id": "lingos-alert-20261114-0001",
  "source": "sensor|weather|manual|device|network",
  "sender": "lingos-alertd",
  "sent": "2026-11-14T23:41:08+08:00",
  "status": "Actual|Test|Exercise|Draft",
  "msgType": "Alert|Update|Cancel|Ack|Error",
  "scope": "Public|Restricted|Private",
  "category": ["Met", "Geo", "Safety", "Security", "Health", "Env", "Infra", "CBRNE", "Other"],
  "event": "暴雨橙色预警 / 燃气泄漏 / 门被打开",
  "severity": "Extreme|Severe|Moderate|Minor|Unknown",
  "urgency": "Immediate|Expected|Future|Past|Unknown",
  "certainty": "Observed|Likely|Possible|Unlikely|Unknown",
  "onset": "…",
  "expires": "…",
  "areaDesc": "客厅 / 门口 / 小区",
  "geocode": [],
  "headline": "一句话说清发生了什么",
  "description": "细节，可空",
  "instruction": "要做什么",
  "contact": "本机",
  "references": [],
  "acked": false,
  "ackedBy": "",
  "ackedAt": ""
}
```

约束：
- 每条预警必须有 `severity`、`urgency`、`certainty`，缺省写 `Unknown`，不猜。
- `msgType=Cancel` 只解除同一 `id` 的前序告警，不新建。
- 同一 `id` 的 Update 覆盖显示层，历史保留（审计链）。

## 三、severity × urgency → 通知强度

| severity \ urgency | Immediate | Expected | Future |
|---|---|---|---|
| **Extreme** | 立即叫醒：声音 + 全屏 + App 推送 + 短信 | 强提醒：声音 + 推送 | 列表置顶 |
| **Severe** | 强提醒：声音 + 推送 | 推送 + 列表置顶 | 列表 |
| **Moderate** | 推送 | 列表 | 列表 |
| **Minor** | 列表 | 列表 | 静默归档 |
| **Unknown** | 按 Moderate 处理 | 按 Moderate | 静默 |

- 颜色：绿（Minor）/ 黄（Moderate）/ 橙（Severe）/ 红（Extreme），与 CAP severity 一一对应。
- 夜间（23:00–07:00）只有 **Extreme+Immediate** 允许声音叫醒；其余静音推送。
- 去重：同 `event`+`areaDesc` 在 5 分钟内合并，只更新等级。
- 升级：等级上调时重新触发通知；下调不重复打扰。
- 解除：`Cancel` 或 `expires` 到期，前端标记「已解除」。

## 四、LING OS 数据来源

| 来源 | 实现位置 | 产出 |
|---|---|---|
| 本地传感器 | `src/python/alert_store.py`、`monitor_service.py` | 烟雾/燃气/水浸/门磁/温度 |
| 设备异常 | `lingos_device` 适配器 + `ai_server` 事件 | 离线、连续失败、异常状态 |
| 气象 | `ai_server` weather 命令族（`weather_current/forecast`） | 气象预警（需外部 API key） |
| 人为/危机 | `crisis.py`（手动触发 / 语音求助） | 安全类告警 |
| 断网应急 | 0.14 Bitchat 类（离线 mesh） | 无网络时的本地广播 |

## 五、落地顺序（0.14）

1. **P0**：告警模型 + 本地接收/确认接口（`GET /api/alerts`、`POST /api/alerts/ack`），Web 首页与安全页展示。
2. **P0**：传感器 → 告警映射（烟雾/燃气/水浸/门磁），按上表分级。
3. **P1**：气象 API 接入（OpenWeather/和风/中国气象，选一个），预警按 CAP 映射。
4. **P1**：通知渠道（Webhook/邮件/短信接口），复用 0.13 已建的 `/api/notify/*` 骨架。
5. **P2**：离线 mesh（Bitchat 参考实现）+ App 端分级推送。

## 六、与现有代码的衔接

- 0.13 已有 `alert_store.py`、`alert_subscription.py`、`cmd_alert_*` 命令族；0.14 只把它们的事件结构扩展为 CAP 字段，不重写。
- Web 端 0.13 已建通知配置（`/api/notify/settings`、`/api/notify/test`，webhook 实测可达）。
- 安全等级（L0–L4）与告警联动：L3/L4 下自动关闭外发通知，只留本机声音/屏幕。
