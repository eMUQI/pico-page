# CheckCell

三个 196 × 176 的检测格之一（Wi-Fi、互联网、更新服务器），显示联网检查进行到哪一步。

依据 `firmware/components/reader_app/app_impl.h` 的 `check_cell()` 和 `check_cells()` 手写的静态示意。

## 状态

| 状态 | 外框 | 标记与文字 |
|---|---|---|
| 完成 | 2 px `ink` | 值如「已连接」「正常」 |
| 失败 | 3 px `ink` | 右上角 30 × 30 `ink` 方块内为 `paper`「!」，值写明失败内容 |
| 未进行 | 2 px `disabled` 实线 | 「未进行」，`disabled` |

## 规则

- 失败项之前的步骤为完成，之后的步骤为未进行。
- 标签 `description`，值 `label-strong`，备注 `description`，内边距 20 px。
- 失败同时有加粗外框和「!」两个标记，不只靠 1 px 线宽差。
- 不用虚线（D39）。
