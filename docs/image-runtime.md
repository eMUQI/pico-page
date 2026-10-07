# EPUB 图片显示

2026-10-06，`reader_document` 和 `reader_graphics` 已提供共享图片读取与绘制路径。
不修改原 EPUB，不生成专有阅读包，不把图片预转换到 TF 缓存。

## 接口与显示行为

- `Chapter.images` 每项包含 `begin/end` 标量范围、`resource`、`alt` 与 `source` 元素锚点。
  占位仍保留在文本中，供不支持或损坏图片的明确降级使用。
- `EpubDocument::read_image(resource, bytes, error)` 只读取当前 ZIP 内的规范相对路径，
  检查解压大小、ZIP CRC 和 PNG/JPEG 签名；失败清空输出。
- `graphics::render_image(canvas, encoded, x, y, width, height, error)` 将图片按比例缩放、居中放入
  给定区域。灰阶值为 0–15，透明像素与白色合成。使用最近邻取样；JPEG 在可行时先以
  1/2、1/4、1/8 缩小，避免处理不需要的输出像素。原图色彩配置和 PNG gamma 暂不应用。
- 解码先写入独立的 4 位灰阶临时缓冲。完整校验成功后才写目标 Canvas，失败不会留下半幅图。
  图片成功显示后的保存、分页和失败提示由 App 层负责。
- 普通正文中的图片作为独立内容块参与本页分页。图片前后的文字可排在同一页；
  当前为图片区保留最多 480 px、约半页高的区域，按原图比例居中绘制，不让文字环绕图片。
  单图章节继续使用整个正文区域。图片放不下时移至下一页；损坏或缺失时只在对应区域显示提示，
  后续正文继续显示。图片的源锚点仍用于进度保存与回翻。
- 支持 PNG 灰度、RGB、索引色、alpha、1/2/4/8/16 位及 Adam7；JPEG 支持 TJpgDec 的 baseline
  灰度和 YCbCr。渐进 JPEG、CMYK、WebP、GIF、动画及复杂 SVG 暂不支持。
- 直接 `img` 和普通 SVG 单张图片包装可解析。SVG 必须只有一张直接 `image`，从零坐标开始且
  尺寸匹配 viewBox；变换、裁切、滤镜、mask、opacity 或多图保留占位。
  Project Gutenberg 两本鲁迅原版封面的 `wrap0000.xhtml` 属于已验证包装。

## 资源边界与依赖

编码图片最多 2 MiB，原图每边最多 4096 像素、总像素最多 8 Mi。
最大目标区域为 684 × 1216，临时灰阶缓冲最多 415,872 字节。
PNG 使用约 44 KiB 解码结构、按宽度分配的扫描行（最坏约 32 KiB）及不超过 1 KiB 的调色板；
JPEG 使用固定 8 KiB 工作区。不会分配原图大小的 RGB/RGBA 帧。
这是代码限额，不是设备总内存峰值；原章、字体、ZIP 目录和显示帧仍另占内存。
ESP 路径每处理 65,536 个源像素让出一个调度 tick，避免长图持续占用 CPU。

PNG 使用固定 commit 的 MIT `pngle`，复用已审查的 miniz；JPEG 使用固定来源的 TJpgDec R0.03。
拷贝前已阅读源码与许可；精确版本、文件摘要、源代码修正及分发义务见
[图片依赖记录](../firmware/components/reader_graphics/third_party/README.md)。
PNG 本地修正包括 bKGD 数组越界、窄图 Adam7 下溢、CRC 高字节移位、deflate 完整性、
解压数量上限和独立解码器 RGBA 临时值；没有关闭 CRC 校验。

## 实际验证

- host `document` 和 `graphics` CTest：2/2 通过。
- ASan/UBSan 同两项：2/2 通过，没有运行时错误。
- 原创 RGBA PNG 验证黑/白/红灰阶、透明白底、绘制边界；Adam7 窄图、16 位灰度与 baseline
  JPEG 验证真实解码和缩放。截断、CRC 错误、过大尺寸均失败且保持原 Canvas。
- 文档测试验证单图 SVG 来源锚点、内部图片读取、路径逃逸/远程路径/过大资源拒绝，
  复杂 SVG 保留占位。
- 原样《阿Ｑ正傳》《吶喊》PNG 封面均解码成功，输出为
  `.local/public-books/aq-cover.png`、`.local/public-books/nahan-cover.png`；已目检两者的书名、作者和图案。
  两书三档字号逐页检查仍通过；证据 `.local/public-books/pagination-images.log`。
  同一整书检查在 ASan/UBSan 下通过；证据 `.local/public-books/pagination-images-sanitized.log`。
- 内置提示字体重新按当前 `reader_app/app.cpp` 生成，包含 406 个字符。
  读取输出字体 cmap 确认全部 344 个非空白源字符均有字形，覆盖图片缺失和格式不支持提示。

上述为 2026-10-06 的 host 证据。图文同页的验证记录见 [开发状态](development-status.md)。
设备显示、TF 吞吐、解码时长和总内存峰值由真机验证单独记录。
