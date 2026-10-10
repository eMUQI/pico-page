# 公开 EPUB 阅读样本

2026-10-06 从 Project Gutenberg 官方页面取得两个未修改的 EPUB 3，保存在忽略目录 `.local/public-books`，另复制到本地演示卡 `.local/card/Books`。原 EPUB 内的来源和许可页均保留。此处记录来源声明，不将免费可下载概括为所有地区均无版权限制，也不把第三方 EPUB 加入 Git。

| 作品 | 官方条目 | 原文件大小 | SHA-256 |
|---|---|---|---|
| 鲁迅《阿Ｑ正傳》 | [25332](https://www.gutenberg.org/ebooks/25332) | 101,741 字节 | `5dcf5341be4b59461f61854c023ddcc38af52655dd8e60bed99bbec241545125` |
| 鲁迅《吶喊》 | [27166](https://www.gutenberg.org/ebooks/27166) | 181,683 字节 | `373fcf0e86d48201b0ba317e15e5ae7dbdf3d33f26223c88950341395c7f2ba4` |

下载入口分别为条目的 EPUB 3 链接：[阿Ｑ正傳 EPUB](https://www.gutenberg.org/ebooks/25332.epub3.images)、[吶喊 EPUB](https://www.gutenberg.org/ebooks/27166.epub3.images)。上游可能重新生成文件；内容身份和测试结果仅适用于上述摘要。OPF 的权利声明为 `Public domain in the USA.`，使用条件见书内许可和 [Project Gutenberg 许可页](https://www.gutenberg.org/policy/license.html)。本地 `provenance.json` 记录来源、下载时间、字节数、摘要及书内许可成员名。

## 实际阅读结果

两书通过原生 ZIP/XML 解析、当前页排版、实际灰阶绘制、来源锚点恢复以及全文连续前进检查。使用完整霞鹜文楷，无缺字。2026-10-06 首次逐页检查中，38 px 下分别为 142 页和 428 页，包含封面占位及许可页；页数随排版修正和设置变化，不作为固定进度身份。初次证据为 `.local/public-books/pagination.log`；当前代码已重跑，日志为 `.local/public-books/pagination-current.log`，三档字号均完整绘制通过，缺字数仍为 0。修正标点和重复缩进后，当前 38 px 页数分别为 140 页和 428 页。2026-10-06 图片路径接入后，两个原版 SVG 包装中的 PNG 封面均实际解码、缩放并绘制；输出和资源边界见 [图片实现](image-runtime.md)。三档正文分页和缺字结果仍通过，证据为 `.local/public-books/pagination-images.log`。重跑生产解析器和渲染器的逐页检查：

```sh
sh tools/check_public_books.sh .local/card/Fonts/LXGWWenKai-Regular.ttf \
  .local/public-books/lu-xun-aq-25332.epub .local/public-books/lu-xun-nahan-27166.epub
```

随后实际桌面应用打开《阿Ｑ正傳》，完成翻页、K2 全刷、工具面板、模拟睡眠和重开恢复。`.local/lu-xun-resume.log` 显示恢复后修订号为 6；K2 和模拟唤醒均不增加修订号。`artifacts/runtime/lu-xun-reading.png` 是真实 App 渲染输出，不能替代设备成像证据。

## 精确同步对照

对照使用原 EPUB、固定的未修改 CREngine 和两种 DOM 版本。运行：

```sh
python3 tools/check_koreader_mapping.py \
  --book .local/public-books/lu-xun-aq-25332.epub \
  --book .local/public-books/lu-xun-nahan-27166.epub
```

脚本输出逐标量正向核对、抽样反向核对、真实引擎书签恢复数量，以及明确未支持的部分。具体结果见 [同步实现记录](sync-runtime.md) 与 [开发状态](development-status.md)。引擎对照仍不等于真实 KOReader 客户端、同步账户及 Read Pico 的双向接续验收。
