# 语义动作

UI 只发送动作并渲染返回的状态。★ = 架构文档 v0.1 中没有、建议新增。

| 动作 | 入口 | 可用条件 / 处理中 | 成功 → | 失败 → |
|---|---|---|---|---|
| `OpenBook` | S01 行、继续阅读 | 书状态 ok；首屏超过阈值显示 S02-2 | S02-1 | S12-4 |
| `TurnPage` | S02 左右区、K1/K3、轻扫 | 跨章未就绪时页脚显示 S02-3，不排队 | 新页 | S02-7，留在原页 |
| ★ `OpenPanel` / `ClosePanel` | S02 中区；面板内 K2 关闭 | 始终可用；关闭不推进阅读位置 | S03 / 回到同一锚点 | — |
| `OpenToc` | S03 目录 | 需存储卡；读取超过阈值显示 S04-4 | S04-1 | S04-3（目录缺失） |
| `JumpToAnchor` | S04 行、脚注、内部链接 | 跳转前压入历史 | S04-6 或 S02-5 | S04-5 |
| ★ `ReturnToPreviousAnchor` | 返回条、目录首行 | 历史非空，最多 3 条 | 原位置 | — |
| ★ `PreviewTypography` | S05 步进器、参数格 | 不在重排中；只排当前屏，不写设置 | 预览区更新 | S05-3（字体不可用） |
| ★ `CancelTypography` | S05 收起、K2 | 有预览时 | 恢复原排版 | — |
| `ApplyTypography` | S05 应用 | 有未应用修改；处理中显示 S05-2，可取消 | 收起面板，停在原文附近 | 恢复原排版并提示 |
| ★ `SetReadingDefault` | S05 设为阅读默认 | 只更新全局默认 | 底部提示 | 保存失败提示 |
| `RequestSync` | S06 检查并同步 | 已配置；离线时尝试联网；处理中 S06-3 | S06-4 / S07 | S06-5、6、7 |
| ★ `CancelSync` | S06-3 | 同步进行中 | S06-2 | — |
| `AcceptRemotePosition` | S06 从其他设备接续、S07 | 有远端候选；approximate 时标“附近” | S07-3 → 新位置 | 退回 S07-1 附原因 |
| `KeepLocalPosition` | S07 继续此处、K2 | 始终可用，不改动远端 | 关闭冲突提示 | — |
| ★ `PushLocalReplaceRemote` | S07 上传此处、S06 将此处上传 | 已联网；必须经过 S07-4 确认 | S06-4 | S07-1 附原因 |
| `SetRefreshPreset` | S08 单选 | 切到快速前经过 S08-2 | 选中项更新 | 错误说明 |
| `ForceRefresh` | **阅读页 K2**、S03 全刷、S08 | 无刷新事务进行中（否则 S08-3 / 忽略） | 整屏刷新 | S08-4 |
| `RequestSleep` | 超时、电源键 | 无关键写入；传输中不自动休眠 | S11-0 | S11-5（低电量） |
| ★ `SetLibraryView` | S01 列表 / 卡片 | 始终可用，全局偏好 | 列表区重绘 | 保存失败提示 |
| ★ `OpenBookDetail` | S01 行右侧状态区（88px） | 始终可用 | S01-6 / S01-5c | — |
| ★ `RescanStorage` | S01-2、4、5c，S09-1 | 存储卡可读；处理中 S01-3 | S01-1 | S01-4 |
| ★ `RemoveMissingEntries` | S01-5 清理、S01-5c | 经 S01-5b 确认；执行前重新检查 | S01-1，进度记录保留 | 留在原页附原因 |
| ★ `DeleteBookLocal` | S01-6 | 经 S01-7 确认 | 书库 + 提示 | 留在原页附原因 |
| ★ `ClearLocalProgress` | S01-6 | 经 S12-5 确认 | 详情页更新 | 留在原页附原因 |
| ★ `ClearCache` | S01-6、S10-3 | 无需确认 | 提示 | 提示 |
| ★ `StartProvisioning` | S09-1 连接 Wi-Fi | 仅 2.4 GHz | S09-4 | S09-3 |
| ★ `StartTransfer` / `EndTransfer` | S09 | 结束时有写入则经 S09-6，收尾后关闭服务 | 书库 + 汇总提示 | — |
| ★ `ExportDiagnostics` | S10-3、S12-4 | 存储卡可写 | 提示路径 | 提示原因 |
