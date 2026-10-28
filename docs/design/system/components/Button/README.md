# Button

立即执行一个动作的方框按钮；黑底样式标出区域内唯一的推荐操作。

依据 `firmware/components/reader_app/app_impl.h` 中 `button()`、`disabled_button()` 和 `danger_button()` 手写的静态示意。

## 样式

| 样式 | 类名 | 绘制 |
|---|---|---|
| 次按钮 | `.pp-btn` | 2 px `ink` 外框，`label`（28 px）`ink` 文字 |
| 主按钮 | `.pp-btn--primary` | `ink` 实心底，`paper` 文字 |
| 按下 | `.pp-btn--pressed` | 6 px 外框，只保持一帧 |
| 不可用 | `.pp-btn--disabled` | 不画框，`label` 文字用 `disabled`，无热区；保留原尺寸，布局不移动 |
| 危险操作 | `.pp-btn--large` | 96 px 高、2 px `ink` 外框，不用主按钮黑底；文字写明动作（「删除原书」） |

## 规则

- 读者能点的控件都有外框或黑底；没有外框的就不可点。近黑白刷新时 `disabled` 文字显示为黑色，靠有无外框区分不可用。
- 默认高度 88 px（`button`）；底部操作和危险操作用 96 px（`button-large`）。
- 每个区域最多一个主按钮。删除类确认中推荐项是「取消」，危险动作用次按钮样式。
- 按钮立即执行动作；进入其他页面用带「›」的 `ValueRow`。
- 不可用时如有原因，用原因替换文字：「已应用」「无法应用」。到达上下限的步进键保留原符号。
- 调用方提供文字、位置尺寸（x、y、w、h）和动作 ID。
