# 共享字体、排版与刷新

`reader_graphics` 在 host 和 ESP-IDF 编译同一套 FreeType 2.14.1 源码，支持 TrueType 和 OpenType CFF 轮廓字体；不依赖系统字体，不从 HTML 或截图生成正文。

## 准备资源

`python3 tools/setup_fonts.py` 把固定源码下载到 `.local/`，把正文可用的完整字体写入 `.local/card/Fonts/`（可用 `--card <目录>` 指定），逐个检查 SHA-256；不挂载设备，下载内容不进 Git。

| 资源 | 固定来源 | SHA-256 |
|---|---|---|
| FreeType 2.14.1 | `freetype/freetype` commit `526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b` | tar.gz `d24f53b60034dc5c48e90d122d432a8f04a05b9e0a98c8d8e49b4a759207ce2e` |
| LXGW WenKai Regular | `lxgw/LxgwWenKai` v1.520，tag commit `b79944853c0725d9f9c8996f07f2476cff1e9463` | `8d6ba638ac9553413354cfaab97637c1cd778444e259441ea1e5f8fb2c697fba` |
| Noto Sans SC Regular | `notofonts/noto-cjk` Sans2.004，commit `523d033d6cb47f4a80c58a35753646f5c3608a78`，`Sans/SubsetOTF/SC/NotoSansSC-Regular.otf` | `faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9` |

## FreeType 配置

FreeType 源码未修改，使用上游 stdio 文件流。ESP 上用 `FT_New_Library` 和静态寿命的 `FT_Memory` 把字体内部分配放入 PSRAM，遵守 FreeType 的重分配约定（失败保留旧块、零尺寸释放、负尺寸拒绝）。只选取 TrueType、CFF、SFNT、PS 辅助和灰阶／单色栅格化模块，关闭压缩字体、SVG、彩色字形和外部图像解码。没有链接 HarfBuzz，不支持复杂文字塑形和双向文字。

`Font` 由单一所有者使用，设置字号、测量和绘制不能并发；`open_memory` 的字节由调用者保持有效。正文与界面字体各自缓存精确字宽和栅格化位图，按最久未用淘汰，重新打开字体时清空；缓存命中与首次栅格化的像素相同，单个字形超过缓存上限时绘制失败。字体失败保留阶段、原始 FreeType 错误码和 Unicode 码位，只有 `FT_ERROR_BASE(error) == FT_Err_Out_Of_Memory` 时才标记内存不足；应用映射到[错误编号](ui-implementation.md#错误返回与恢复)。

## 字体选择与缺字

工厂默认正文字体为 `Fonts/LXGWWenKai-Regular.ttf`。显式选择的文件缺失时保留原偏好并提示回退，不能用缺失字体保存排版。统一排版不在多套字体间逐字回退；原书排版按 CSS 字体族逐字尝试包内字体，再用用户正文字体，见[原书排版](original-layout.md#阅读行为)。

书库内容使用全局阅读默认字体：与当前正文 face 相同时复用，否则持有一个独立 face 和较小的缓存预算；打开失败时整段使用界面字体。书内目录、章节名等标签复用正文 face。这些字体只由应用任务测量和绘制，不交给内容 worker；USB、移卡和关机时释放，低内存时优先裁减书库字形缓存。字体打开和首次字形读取在应用任务同步执行，慢 TF 的阻塞须按 CFG10 测量。

任何字体缺少的字符都不使用该字体的 `.notdef`，统一画空心方框：被 `cjk()` 判为 CJK 的字符（汉字、假名、CJK 标点和扩展区）占一个字宽，其余占半个字宽，谚文、全角形式和绘文字也占半个字宽。只有 Unicode 规定无可见形态的码位（Default_Ignorable_Code_Point 与 C0 控制符）缺少时不画、不占宽度。正文使用内置字体且当前页有缺字时，页脚提示可在排版中更换字体。

## 字重

统一排版提供常规、中粗、粗，同族有 `-Light` 文件且字号足够大时另提供细档。固件只按已枚举文件中明确的 `-Regular`、`-Light`、`-Medium`、`-Bold` 后缀选择同前缀、同扩展名的兄弟文件，不猜测字体家族，不解析可变字体轴。

- 没有对应文件的中粗、粗以 Regular 同族文件经 `FT_Outline_Embolden` 合成，不改变字宽；细档不做合成缩细。
- 只有已加权的字体文件时，请求的字重标记为不可用，不在其上再次加粗，提示「所选字重不可用，保留字体原有字重」。
- `regular` 保留所选文件原有的字重，旧配置显式指定的 `-Bold` 等文件不会被自动换成 Regular。
- 原书排版的「稍细」「稍粗」微调字体轮廓，保留作者粗体标记和字形步进。
- 合成字重进入字形缓存键，不另建缓存；切换合成字重清除正文与预取位图，选择实际兄弟文件时重开字体。不可用的配置显示替代效果，并禁止从面板保存。

UI 强调使用 FreeType `FT_GlyphSlot_Embolden`。

## 像素与排版契约

- 画布为 684 × 1216 竖屏 4 位灰阶，0 为黑、15 为白，偶数 x 在字节低半部；方向转换由板级适配负责。
- `layout_page` 只布局从指定 Unicode 标量偏移开始的一页，`[start,end)` 包含消耗的换段符；出错时不推进 end，不按估计页码改写位置。不整书预排。
- 依据字号、advance、行高、边距、首行缩进和段间距计算完整行；行高放不下字体的 ascender 与 descender 时明确失败。绘制使用 baseline，不能把行顶当 baseline，否则中文上部会被裁切。
- 统一排版中 EPUB 的 `h1`–`h6` 统一按标题处理（约 1.15 倍字号、加粗、无首行缩进，页末尽量与下一行同页）；原书排版的标题样式由默认样式和作者 CSS 决定。
- 行首行尾禁则覆盖常见闭合标点、小标点、引号和括号；不宣称完整实现 Unicode 换行算法。统一排版保留段首原有的全角空格或制表缩进，此时不再加用户设置的首行缩进。
- 统一排版默认左对齐，可选两端对齐；含中日韩字符的两端对齐行把剩余宽度均分到可断字间，开括号之后与闭标点之前不加宽。行尾空格和段首缩进不参与分配。
- 英文断行识别软连字符、零宽空格、不换行空格、词连接符和常见连接号；行末连字符计入宽度并单独标记为绘制字形，源索引、命中和选区仍以原文为准。超宽整词优先用合法断点，否则紧急断行。参考 [UAX #14](https://www.unicode.org/reports/tr14/) 与 [CSS Text Hyphenation](https://www.w3.org/TR/css-text-3/#hyphenation)。
- 原书排版按行内计算样式度量字宽和字号，行框取整行最大的上伸、下伸与样式行高；正文、命中区域和页数统计共用分页边界。图片与文字共同分页，见[图片实现](image-runtime.md)。

## 刷新档位与翻页效果

`clear` 每页全刷；`balanced` 使用 16 灰阶 GL16；`fast` 使用八灰阶 GL16；全刷始终为 GC16。自动全刷间隔见[验收 DP01](acceptance.md#显示电源和设计)。非正文更新由 `changed_pixels_waveform` 把未变化灰阶的动作置为保持，其余相位不变。

翻页效果只作用于 `Body` 的非清屏帧，覆盖层、补图和页脚更新使用原档位。效果共用 BSP 的八灰阶 GL16 波形（30 相），适用于倒置竖屏的全屏扫描：

- 水波纹按 16 个行带错开起始时间，共 45 拍，相位表放在 PSRAM。
- 斜掠把 5 个行带与 9 个列段的延迟相加，共 42 拍；列段端点按 16 像素对齐，相位表在内部 RAM 中按拍滚动生成，避免供数任务从 PSRAM 查表。
- 「随点按位置」复用同一块内部 RAM，按起点选择平扫或上、下角斜掠；上下角沿纵向镜像，回翻沿横向镜像。

应用任务独占调度，供数任务只读，扫描返回后才能改写或释放表；切换效果时短暂持有新旧两块表，关闭时释放。准备失败时关闭效果并回退当前档位。整次效果扫描和欠载恢复期间不可取消。

## 内置字体与许可

`fonts/PicoPageFallback.otf` 由上述 Noto Sans SC 裁剪生成，编译进固件只读数据段并经 Flash 映射读取，用于界面文字；没有可用正文字体时正文也回退到它。字符清单在 `fonts/fallback_characters.txt`，由三部分组成：

- ASCII、界面文案与三语语言表的全部字符，以及 `--strings` 指定源文件中的字符；源字体缺少任何一个时生成失败。
- 《通用规范汉字表》一级字 3,500 个（`fonts/common_characters.txt`，取自 Unicode 17.0.0 Unihan 的 `kTGH` 字段 `2013:1`–`2013:3500`，Unicode License v3，仓库只含字符清单）；缺少时生成失败。
- 正文常见符号区段（U+00A0–017F、U+01CD–01DC、U+2000–206F、U+2460–2473、U+3000–303F、U+FF00–FFEF），源字体没有的码位跳过。

内置字体不保证一级字以外的简体、繁体和生僻字，繁体正文建议使用完整字体。首次引导的欢迎副标题使用 `fonts/PicoPageWelcome.ttf`，是只含该句简繁字形的文楷子集，只在绘制欢迎页时临时打开。新增界面文案后运行：

```sh
python3 -m venv .local/font-tools
.local/font-tools/bin/pip install fonttools==4.60.1
python3 tools/generate_translations.py
.local/font-tools/bin/python tools/setup_fonts_subset.py
.local/font-tools/bin/python tools/setup_onboarding_font.py
```

fonttools 只用于维护生成，普通构建不需要。两个子集都是修改版本，已改名为 `Pico Page Fallback` 和 `Pico Page Welcome`，保留原版权（Noto 为 © 2014–2021 Adobe）与 OFL 1.1 元数据，分发时随附 `Noto-OFL.txt`、`LXGW-OFL.txt`。

本软件部分基于 FreeType Team 的工作。Portions of this software are copyright © 2025 The FreeType Project (https://freetype.org). All rights reserved. FreeType 采用 FTL，见组件内 `FreeType-FTL.txt`；`fthash` 的 MIT 风格许可和 Adobe CFF 解释器的附加专利条款见 `FreeType-extra-notices.txt`。二进制分发时必须保留这些说明和字体 OFL。
