# ToolTabs

工具面板顶部：56 px 收起条（「收起」）下方是四个 171 px 标签，当前标签为 `ink` 黑底。

依据 `firmware/components/reader_app/app_impl.h` 的 `tabs()` 手写的静态示意。

## 规则

- 标签固定为「目录 · 排版 · 同步 · 刷新模式」，字号 `label`（28 px）；当前标签为黑底，文字用 `label-strong`、`paper`。标签名与打开它的工具栏入口一致。
- 整条收起条是一个关闭面板的热区；文字为 `description`、`secondary`，后接小箭头。
- 收起条顶部为 3 px 横线；标签行上下边和分隔线为 2 px。
- 同一时间只打开一层面板。K2 仍全刷当前画面，不关闭面板。
- 调用方提供顶部 y 和当前标签 ID。
