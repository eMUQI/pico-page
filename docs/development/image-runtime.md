# EPUB 图片显示

`reader_document` 和 `reader_graphics` 提供共享图片读取与绘制路径。不修改原 EPUB，不生成专有阅读包，不把图片预转换到 TF 缓存。

## 接口与显示行为

- `Chapter.images` 每项包含 `begin/end` 标量范围、`resource`、`alt` 与 `source` 元素锚点。占位仍保留在文本中，供不支持或损坏图片的明确降级使用。
- `EpubDocument::read_image(resource, bytes, error)` 只读取当前 ZIP 内的规范相对路径，检查解压大小、ZIP CRC 和 PNG／JPEG／GIF 签名；失败清空输出。
- `graphics::render_image(canvas, encoded, x, y, width, height, error)` 将图片按比例缩放、居中放入给定区域。灰阶值为 0–15，透明像素与白色合成。使用最近邻取样；JPEG 在可行时先以 1/2、1/4、1/8 缩小，避免处理不需要的输出像素。原图色彩配置和 PNG gamma 暂不应用。
- 解码先写入独立的 4 位灰阶临时缓冲。所需像素解码成功后才写目标 Canvas，失败不会留下半幅图。图片成功显示后的保存、分页和失败提示由 App 层负责。
- 普通正文中的图片作为独立内容块参与本页分页。图片前后的文字可排在同一页；当前为图片区保留最多 480 px、约半页高的区域，按原图比例居中绘制，不让文字环绕图片。单图章节继续使用整个正文区域。图片放不下时移至下一页；损坏或缺失时只在对应区域显示提示，后续正文继续显示。图片的源锚点仍用于进度保存与回翻。
- 支持 PNG 灰度、RGB、索引色、alpha、1/2/4/8/16 位及 Adam7；JPEG 支持 TJpgDec 的 baseline 灰度和 YCbCr。渐进式 JPEG 使用 JPEGDEC 的 DC 系数生成宽高各 1/8 的灰阶预览，再缩放到目标区域；适用于书库封面，正文及图片查看也使用该预览，放大后不具备完整 AC 细节。此路径不验证未使用的 AC 扫描。GIF87a／GIF89a 支持首帧，包括全局／局部调色板、透明色和交错行；动画仅显示首帧，不执行后续帧合成。CMYK、WebP 及复杂 SVG 暂不支持。
- 直接 `img` 和普通 SVG 单张图片包装可解析。SVG 必须只有一张直接 `image`，从零坐标开始且尺寸匹配 viewBox；变换、裁切、滤镜、mask、opacity 或多图保留占位。Project Gutenberg 两本鲁迅原版封面的 `wrap0000.xhtml` 属于已验证包装。

## 资源边界与依赖

编码图片最多 2 MiB，原图每边最多 4096 像素、总像素最多 8 Mi。最大目标区域为 684 × 1216，临时灰阶缓冲最多 415,872 字节。PNG 使用约 44 KiB 解码结构、按宽度分配的扫描行（最坏约 32 KiB）及不超过 1 KiB 的调色板；baseline JPEG 使用固定 8 KiB 工作区；切入渐进式解码前释放它。JPEGDEC 对象在堆上分配，ESP32-S3 构建的对象大小为 17,884 字节，macOS arm64 为 17,944 字节，无库内动态分配。另有最多 16,388 字节的缩放坐标表，以及调用者持有的编码字节、目标 Canvas 和已缓存像素。不会分配原图大小的 RGB/RGBA 帧或整图系数数组。这是代码限额，不是设备总内存峰值；原章、字体、ZIP 目录和显示帧仍另占内存。ESP 路径每处理 65,536 个源像素让出一个调度 tick，避免长图持续占用 CPU。

GIF 解码器按 [GIF89a 规范](https://giflib.sourceforge.net/gifstandard/GIF89a.html)原创实现，不引入外部库或复制参考阅读器源码。LZW 字典固定 4,096 项，prefix／suffix／展开栈合计 16 KiB，分配在堆上；复用目标灰度缓冲和缩放表，不分配原图索引帧。GIF 为 CompuServe 的 Graphics Interchange Format，原规范将 GIF 与 Graphics Interchange Format 标为服务标记。正文／封面允许 GIF，不改变传书和锁屏文件入口只接收 PNG／JPEG 的规则。

PNG 使用固定 commit 的 MIT `pngle`，复用已审查的 miniz；JPEG 使用固定来源的 TJpgDec R0.03。渐进式 JPEG 另使用 Apache-2.0 JPEGDEC 与 CrossPoint 的三个修复补丁，来源 MIT 声明一并保留；本地第四个补丁修正未对齐读取、32/64 位读取宽度及未定义移位。没有引入其他阅读器的应用层。拷贝前已阅读源码与许可；精确版本、文件摘要、源代码修正及分发义务见[图片依赖记录](../../firmware/components/reader_graphics/third_party/README.md)。PNG 本地修正包括 bKGD 数组越界、窄图 Adam7 下溢、CRC 高字节移位、deflate 完整性、解压数量上限和独立解码器 RGBA 临时值；没有关闭 CRC 校验。

## 休眠图片

自定义锁屏由内容 worker 准备，与普通正文图片的读取和提交路径不同；文件、透明缓存、冷／热合成资源、取消与回退规则统一见[平台说明](platform-runtime.md#自定义锁屏)，页面和预览规则见[UI 说明](ui-implementation.md#自定义锁屏)。不把早期整层解码的分配量用于当前流式锁屏路径。

图片及锁屏的验收场景见[验收矩阵](acceptance.md)。
