# 设计基线与原件

用户已决定按这套设计开始开发。视觉稿不重新生成，行为修正按 [UI 实现注意事项](../development/ui-implementation.md)。首先服从 [当前决定](../planning/decisions.md)，不要将原始 README 的旧约定当作更高优先级。

原始包：`Pico阅读固件界面设计.zip`。其中 49 个文件均已逐字节保留于 `original/`。只有目录位置改变，PNG、HTML、JavaScript、Markdown 与 tokens 的内容没有修改。本次未重新渲染图像、未补画缺失页面、未打包字体。

| 资料 | 数量/入口 |
|---|---|
| PNG | 36 张，均为 684×1216，位于 original/png |
| HTML 参考 | 5 份，位于 original/reference |
| 文案、字段、动作、输入、tokens | [原交接入口](original/README.md) |
| 文档列出的页面/组件状态 | 69 个 |
| 尚无单独 PNG 的状态 | 33 个，可按已有 HTML/文字实现 |

[完整资源清单](inventory.json) 记录每张 PNG 的状态 ID、尺寸及未导出状态。原件可以证明视觉交付内容，不证明动作、协议、硬件和存储已经工作。

## 实现时怎么查

先查当前 ui-implementation，再按页面 ID 看 original/screens.md、对应 PNG 或 HTML。默认 38 px 正文等数值来自原件 tokens；真机测试确实需要调整时留差异记录，不先大改视觉。原件名称中的 Pico Pages 仅是设计历史；新产品文字为 Pico Page。旧稿中的全书阻塞重排、严格一次一帧、状态推断和固定目录示例已被当前规格修正。**原稿的正文 K2 全刷得到保留**，不遵循旧评审 R06 的改键建议。

original/reference/support.js 与 HTML 只作来源资料，不将其代码直接用作固件业务层。开发者需要运行参考 HTML 时先检查脚本与远程资源；本交接流程没有执行它们。
