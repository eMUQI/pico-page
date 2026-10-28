# ValueRow

列表行：标题、可选说明和右对齐的值，下方以 2 px `rule` 线与下一行分隔。

依据 `firmware/components/reader_app/app_impl.h` 的 `value_row()` 手写的静态示意。

## 结构

- 文字从 x 36 到 648（`space-36`）；热区为 x 24–660。
- 标题 `label`（28 px）；说明在标题下方，`description`（24 px）、`secondary`。
- 值为 `description`，最宽 400 px；「开」「关」等开关状态加粗。
- 默认高度 104 px（`list-row`）。

## 规则

- 只有进入其他页面的行在值后加「 ›」；原地切换的行只显示值。
- `rule` 分隔线仅作装饰，近黑白刷新时会消失，不得表示状态。
- 调用方提供标题、说明、值、y 和动作 ID（只读行为 0）。
