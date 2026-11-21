# 原生 EPUB／TXT 内容后端

`reader_document` 供 host 和 ESP-IDF 共用，直接读取原 EPUB 和 TXT，不要求专有阅读包。第三方来源与许可见 [reader_document 第三方记录](../../firmware/components/reader_document/third_party/README.md)；ZIP 与 XML 直接复用 miniz 和 Expat，没有复制 CrossPoint 或 KOReader 的代码。

## 接口与职责

`EpubDocument::open()` 计算完整 SHA-256 与 KOReader 规则的部分 MD5，读取 container、OPF、spine，以及 EPUB 3 导航或 EPUB 2 NCX；书名、作者和语言来自 OPF，缺少书名时用文件名。目录依次尝试 nav、NCX 和按 spine 生成的目录，前一来源损坏、超出预算或没有有效条目时用下一来源，不截断目录。开书不建立完整 XML 树，OPF 解析不依赖 manifest 与 spine 的先后顺序。

`load_chapter()` 只解压指定 spine 的 XHTML，失败时清空输出并返回错误，不把截断内容当作成功；未选章节的 XML 错误不影响当前章。正文是 `std::u32string`，段落、文本映射、链接和片段目标的偏移都是 Unicode 标量索引。`links` 提供本书链接的资源与片段，`fragment_offset()` 解析 `id` 的位置；外部链接只标记 `external`，不联网。脚注保留在原文顺序中，弹出与返回历史由上层实现。

## HTML 正文兼容与 TXT 编码

EPUB 包、导航和 XHTML 优先使用严格的 Expat 解析。只有以短 HTML5 doctype 声明的 `.html`／`.htm` 正文遇到 XML 语法错误时，才最多重读一次并使用有界的 HTML 适配：已成功返回的正文和锚点保持不变，只修复错误之后的部分，修复边界按 spine 记录以免顺序读取反复失败；CRC、取消、资源预算、DTD 和包元数据失败不触发放宽。适配后的正文使用本地修复锚点，并标记为不能精确换算 KOReader 位置。允许不含内部子集的 DOCTYPE，但不加载外部 DTD；内部 DTD、自定义实体、加密或混淆资源、ZIP64 和作为 manifest 项的外部资源都明确拒绝。不执行脚本，不请求外部实体。

TXT 支持 UTF-8（可带 BOM）、GB18030，以及带 BOM 的 UTF-16LE／BE；UTF-16 保持代理对和 CRLF 完整，奇数字节、无效代理对和 NUL 标量报错。开书扫描在固定目标大小附近建立原字节分段索引和章节标题，不保留整书转码结果，读取当前段时流式解码；段首若续接上一段落，沿用普通行的起始位置。`txt_offsets` 的单位是原文件字节，段内 `Anchor.offset` 是 Unicode 标量。

TXT 目录在同一次身份与编码扫描中识别独占短行的「第 N 章／回／节／卷／部／篇／集」、序章、楔子、前言、序言、引言、后记、尾声、终章、附录，以及带编号的英文 Chapter／Part／Book／Volume。目录为平铺列表，位置记录标题首字符的原字节偏移、分段和段内偏移，跳转直接加载目标分段。标题数量或内存超限时放弃这份可选目录，只保留「正文」，不截断正文。

TXT「重排段落」只在同一分段内相邻的英文散文行之间选择换行，缩进、空行、标题和短诗行保留；排版把选中的换行视为一个词间空格，原文、偏移和标题索引仍按源文件读取。

内容修订与缓存版本：UTF-8／GB18030 TXT 与 `epub-native-v1` 的进度修订保持兼容，UTF-16 使用 `txt-utf16le-v1`／`txt-utf16be-v1`；章节缓存为 `content-reading-v6`、`content-original-v5`，页索引为 `pagination-v6`（统一排版）、`original-pagination-v6`（原书排版）和 `txt-pagination-v7`。版本变化时旧派生缓存按未命中重建。

## 读取级别与分段

所有级别使用相同的分段：一段在达到约 60,000 标量后的第一个块边界结束，没有块边界时在 120,000 标量处从块内分开；文本节点或元数据过多时也提前分段。靠后的分段仍需处理前缀，因此读取更慢，但正文内存不随章节长度增长。来源锚点与分段无关，保存和恢复不受分段影响；`find_chapter_part` 按锚点或片段标识查找分段。

| 级别 | 保留内容 | 用途 |
|---|---|---|
| `Mapping` | 全部，包括原始文本节点和样式；解析到文档末尾 | KOReader 位置换算 |
| `Reading` | 正文、块、图片、链接、片段和锚点 | 阅读 |
| `Layout` | 正文、块、图片和分段边界，不含锚点 | 统一排版的页数统计 |
| `Original` | 正文、锚点、块与行内计算样式 | [原书排版](original-layout.md#阅读行为)及其页数统计 |

从块内分开的分段带 `split_node` 标记，位置换算对其一律拒绝。解析过程中按增长后的大小检查 PSRAM 余量，不足时以 `Chapter exceeds available memory` 失败并释放，不让分配失败触发 abort；host 上可用 `set_chapter_memory_probe` 替换检查。

## 样式与降级

- 统一排版（`Reading`／`Layout`）使用统一正文样式，保留标题和列表类别；作者的粗体、斜体和隐藏样式不单独输出，CSS 不阻止阅读。
- 原书排版（`Original`）计算有界的 CSS 选择器、行内声明、块与文本样式；支持范围见[原书排版](original-layout.md)。
- 表格按行输出、单元格以制表分隔并返回 warning，不做表格布局。
- `Chapter.images` 提供图片范围、包内资源和真实媒体锚点；`read_image()` 有界读取 PNG／JPEG／GIF（只取首帧），支持 `img` 和无变换、单 `image` 的 SVG 封面包装。读取失败时保留明确占位，不清空正文。解码与缩放见[图片实现](image-runtime.md)。
- `head`、`script`、`style` 不进入正文；其他未专门排版的元素保留文本。
- TXT 没有内嵌封面，封面入口返回确定的「无封面」。

## 来源位置

每个保留的文本范围对应 `Anchor{resource,node,offset}`。`node` 是本地来源路径（如 `/html[1]/body[1]/p[1]/em[1]/text()[1]`），**不是 KOReader XPointer**；同名兄弟与文本节点编号从 1 开始。相邻的 Expat 字符回调合并为一个节点，子元素、注释与处理指令分隔文本节点，CDATA 边界本身不分隔。偏移针对实体解码后的文本。

XML 空白连续段折叠为一个空格，块首尾空白删除，NBSP 保留；保留的空格对应该段的首个源空白，被删除的空白没有反向映射。`pre` 和 `xml:space="preserve"` 保留空白。块之间插入的换行没有来源。媒体占位的锚点指向原媒体元素、偏移固定为 0，因此纯图片页也有可保存的位置。`anchor_at()` 与 `offset_for()` 不猜测近似位置。

`source_nodes` 保留当前章的原始文本节点（含首尾和纯空白），`css_sources` 有界保留内联与本地样式，供同步层判断空白和 DOM 变化。`plain_dom` 是精确映射的前置条件：受限的 `img` 允许，SVG 等其他媒体、表格、未知元素、正文中的注释／处理指令／CDATA、缺失或超限的样式会使其为 false。外部 XPointer 转换由 `reader_sync` 实现，见[同步实现](sync-runtime.md)。

## 输入边界

原书通过随机读回调访问，ZIP 不解压到 TF 卡，也不整书载入内存；miniz 只保留中央目录，XHTML 经解压回调直接送入 Expat。下列上限在代码中校验，超限明确报错：

| 对象 | 上限 |
|---|---|
| 原 EPUB | 256 MiB；拒绝多卷、ZIP64、加密、符号链接、重复或越界路径和非 STORE／DEFLATE 方法 |
| ZIP 目录 | 4,096 项，中央目录 1 MiB |
| ZIP 资源 | 单项解压 32 MiB，总计 512 MiB，大条目压缩比不超过 1,000:1 |
| XML（包、导航、章节） | 每项 16 MiB，400,000 元素，深度 64 |
| OPF 与目录 | manifest、spine 各 4,096 项，目录 8,192 项，单个标题 32 KiB，共享 1 MiB 字符串预算 |
| 每章映射 | 8,192 个文本范围与源文本节点；链接和片段各 2,048；样式 256 KiB |
| 图片 | 每章 256 个引用，每次读取 2 MiB 编码数据，CRC 失败不返回内容 |
| TXT | 64 MiB；目录最多 2,048 项、标题 80 个标量 |

这些是软件上限，不是 Read Pico 的实测峰值内存；ZIP 索引排序和单次 TF 读取是不可取消的区间。真机开书峰值与耗时须另行测量。
