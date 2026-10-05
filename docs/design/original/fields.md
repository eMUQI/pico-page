# 状态字段

## 架构文档已有（ReadingViewState / SyncRecord）

`title`、`chapter_label`、`page_label`、`percentage_label`、`pagination_status`、`save_state`、`sync_state`、`remote_candidate`、`mapping_quality`、`refresh_preset`、`available_actions`、`transient_message`

- `chapter_label`、`percentage_label` 可为空，UI 必须处理未知。
- `pagination_status` 未完成时页脚显示“本章第 N 屏”，不显示总页数。
- `sync_state`：disabled / ready / pending / syncing / conflict / auth_error / mapping_error / network_error，分别对应 S06-1 至 S06-7 与 S07。
- `mapping_quality`：exact → S07-1；approximate → S07-2；unsupported → S06-6。

## ★ 建议新增

| 字段 | 用途 |
|---|---|
| `available_actions[].disabled_reason` | 禁用项下方显示的原因：需联网、需存储卡、屏幕正忙… |
| `sync_progress { stage, stage_count }` | S06-3 阶段显示：连接 → 读取远端 → 上传 |
| `remote_candidate { chapter_label?, percentage, received_at, read_time_reliable }` | S07 远端卡片；章名可空；“本机收到”与“阅读时间未知”分开 |
| `last_ack { at, account_scope }` | “最近一次上传确认”与当前服务/账户 |
| `jump_history[≤3] { label, anchor }` | 返回条与目录首行 |
| `typography { applied, pending, preview_status, reflow_status }` | 区分已应用 / 预览中 / 重排中 / 字体不可用 |
| `library_item.status` | ok / missing / needs_reprocess / corrupt（S01-5） |
| `library_item.cover { available, decode_ok }` | 卡片模式与休眠封面；不可用时回退文字封面 |
| `storage { state, books_bytes?, cache_bytes?, free_bytes? }` | 存储卡不可用 ≠ 空书库；读不到显示“未知”，不填 0 |
| `transfer.items[] { name, stage, progress?, error_kind }` | S09-5 逐文件三阶段与失败原因 |
| `wifi { state, ssid, band, fail_reason }` | 仅 2.4 GHz；fail_reason：密码错误 / 找不到网络（含 5 GHz）/ 超时 / 无地址 |
| `wake_sources[]` | 休眠画面唤醒文案，以板级确认为准 |
| `battery { percent, low }` | 页脚电量、S11-5 低电量 |
| `library_view` | list / card 全局偏好 |
