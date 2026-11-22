# 阅读设置示例

| 示例 | TF 卡上的目标路径 |
|---|---|
| [全局设置](global-settings.toml) | `PicoPage/settings.toml`，从 TF 卡根目录计算 |
| [单书设置](book-settings.toml) | `<完整文件名>.picopage/settings.toml`，伴随目录与原书同级 |

手动复制时，将示例重命名为 `settings.toml`。如果目标文件已存在，只将需要的字段合入原文件。单书设置只覆盖指定字段，其余字段沿用全局设置。示例中的字体路径需要指向 TF 卡上的字体文件。

配置格式和生效顺序见[存储约定](../development/storage.md)。Wi-Fi 和 KOSync 分别使用 `PicoPage/network.toml` 和 `PicoPage/sync.toml`，两份文件含凭据。
