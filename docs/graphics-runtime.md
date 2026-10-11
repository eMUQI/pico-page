# 共享字体与分页

`reader_graphics` 在 host 和 ESP-IDF 编译同一套 FreeType 2.14.1 源码；不依赖系统字体，不从 HTML、PNG 或浏览器截图生成正文。当前支持 TrueType 和 OpenType CFF 轮廓字体。

## 准备资源

运行 `python3 tools/setup_fonts.py`。脚本将固定源码下载到 `.local/`，将正文和 UI 字体写入 `.local/card/Fonts/`，每个下载文件都检查 SHA-256。可以用 `--card <目录>` 指定准备目录；这一步不挂载设备、不擦写 TF 卡。完整字体和源码下载不纳入 Git。

| 资源 | 固定来源 | SHA-256 |
|---|---|---|
| FreeType 2.14.1 | `freetype/freetype` commit `526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b` | tar.gz `d24f53b60034dc5c48e90d122d432a8f04a05b9e0a98c8d8e49b4a759207ce2e` |
| LXGW WenKai Regular | `lxgw/LxgwWenKai` v1.520，tag commit `b79944853c0725d9f9c8996f07f2476cff1e9463` | `8d6ba638ac9553413354cfaab97637c1cd778444e259441ea1e5f8fb2c697fba` |
| Noto Sans SC Regular | `notofonts/noto-cjk` Sans2.004，commit `523d033d6cb47f4a80c58a35753646f5c3608a78`，`Sans/SubsetOTF/SC/NotoSansSC-Regular.otf` | `faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9` |

FreeType 使用上游默认的 stdio 文件流，未改第三方源码。host 使用默认内存接口；ESP 使用 `FT_New_Library` 和静态寿命的 `FT_Memory`，将字体内部分配放入 PSRAM，释放与重分配保持 FreeType 的所有权约定。重分配失败保留旧块，零尺寸释放，负尺寸拒绝。组件只选取 TrueType、CFF、SFNT、PS 辅助模块和灰阶/单色栅格化模块；关闭压缩字体、SVG、彩色字形和外部图像解码。没有链接 HarfBuzz，复杂文字塑形、双向文字及字体间自动逐字回退暂不支持。中英文基础排版使用实际字宽；全字体中没有的字符会呈现该字体的缺字字形。工厂默认路径为 `Fonts/LXGWWenKai-Regular.ttf`，与资源准备脚本和示例配置一致。显式选择的文件缺失时仍保留原偏好并提示回退，不能用缺失字体保存排版。

每个字体对象最多缓存 8,192 个字宽结果，键同时包含字号与 Unicode 标量；重新打开字体时清空。缓存仅复用 FreeType 已成功返回的精确字宽，不估算中英文宽度、不改变保存锚点。刷新档位：`clear` 每页全刷；`balanced` 使用 16 灰阶 GL16，每 8 次正文更新后全刷；`fast` 使用八灰阶 GL16，每 5 次正文更新后全刷。全刷始终为 GC16。

文字转页是待真机验证的候选，默认关闭。设置或正文工具中的「显示与刷新 → 翻页效果」提供「关闭、文字转页 A、文字转页 B、文字转页 C、水波纹」，本次启动有效，重启恢复关闭；不写 `settings.toml`，不改变三个刷新档位及全刷间隔。诊断串口共用同一选择，见 [诊断协议](console-runtime.md)。候选只作用于 `Body` 非清屏帧；覆盖层、补图和页脚区域更新使用原档位，K2、自动全刷及基准失效仍清屏后使用 GC16。「清晰」每页全刷，因此不播放效果。

A 仅把裁剪 GL16 每条 `(to, from)` 序列左对齐；B 在 A 基础上清除对角线动作；C 在 B 基础上将 `to != 15` 的连续前导擦白动作置为保持，不将后续动作再次前移。三步在纯函数中分别可开关，A/B/C 均保持 37 相和原有相位时长元数据。源表通过初始化后的官方 `e0470_waveform_phases()` 读取，不复制波形数据。不能由相数相同推断真机耗时相同：驱动的前导保持跳过和实际内容仍影响耗时。无白场、灰阶和残影均未验收，尤其 B/C 的未驱动灰阶像素，以及 C 对 1–14 级灰阶终态的影响。

水波纹参考 kiikoread 的错相条带调度，独立于文字转页 A/B/C：使用原 GL16 的 37 相，16 条带逐拍启动，共 52 拍，固定从右向左展开。当前仅适配 Read Pico 的倒置竖屏，保留 FULL 扫描、不引入上游 FAST 扫描或软件忙等节拍，不能承诺原翻页耗时。补丁只为构建副本增加逐行相位 LUT 选择，不改 `.local/` 原件或 BSP 锁定；欠载仍沿原完整清屏重画路径恢复。

文字转页仅有一份内部 RAM LUT，2,368 字节，切换 A/B/C 时重建。水波纹按需分配 37,888 字节、16 字节对齐的 PSRAM 相位查找表，另有内部 RAM 的 684 字节逐行相位索引与 37 个指针；关闭或切换到文字转页时释放 PSRAM。二者均由应用任务在同步刷新之间准备；EPD 生产者任务只读，底层返回前已汇合，不在刷新中重建或释放。无新增任务或队列。准备失败记录日志、取消实验选择并回退当前刷新档位；不改变阅读锚点或保存语义。文字转换为有界 256 条序列扫描，水波纹重建为 37×256 项；实际准备耗时、任务余栈及资源峰值仍待真机采样。刷新期间不取消，最长不可取消区间分别为一次原 37 相事务或完整 52 拍事务及既有恢复，尚无实测毫秒上界。


每个字体对象另缓存已栅格化的字形位图，键相同，上限 1 MiB，超限时淘汰最久未绘制的一半，重新打开字体时清空；缓存命中与首次栅格化的像素相同。UI 强调字重使用同一固定 FreeType 源码的 `base/ftsynth.c` 与 `FT_GlyphSlot_Embolden`，缓存键区分常规/加粗；正文标题也使用加粗字重。未修改第三方源码，也未新增字体下载；FreeType 原许可与署名义务继续适用。2026-10-06 的改前/改后探针对两本鲁迅 EPUB、28/38/56 px 的 2,060 页逐页比较边界和像素摘要，输出完全相同；证据 `.local/font-psram-check/pages-before.log` 与 `pages-after.log`。

### 正文字重（2026-10-11）

排版提供常规、中粗、粗；同族有 `-Light.ttf` 或 `-Light.otf` 且字号至少 34 px 时另提供细档。固件只按已枚举文件中的明确 `-Regular`、`-Light`、`-Medium`、`-Bold` 后缀选择同前缀、同扩展名的兄弟文件，不猜测任意文件名的字体家族，也不解析可变字体轴。未提供对应文件的中粗、粗通过 FreeType 2.14.1 的 `FT_Outline_Embolden` 调整轮廓；强度分别为字号的 1/64、2/64 像素，不改变字宽。正文标题的既有强调仍叠加。细档不做合成缩细；不可用的配置显示替代效果并禁止从面板保存该排版。

合成字重加入字形缓存键，使用原有 1 MiB 上限，不新增另一份字形缓存。切换合成字重清除正文和预取位图，保留同一字体 face 与度量缓存；选择实际兄弟文件时重开字体，分页缓存绑定该实际文件及内容摘要。预览仍只画当前页，取消恢复应用值；不改阅读锚点和进度修订。合成字重的冷热像素一致性在 host 验证，实体小字号清晰度、残影与资源峰值仍待真机。

## 像素与排版契约

- 684×1216 竖屏，4 位灰阶，0 为黑色、15 为白色；偶数 x 位于字节低半部，奇数 x 位于高半部。一帧为 415872 字节。方向转换由板级显示适配负责，与官方 EPDiy 像素顺序一致。
- `Font` 单一所有者，设置字号、测量和绘制不能并发。`open_memory` 的字节由调用者保持有效。
- `layout_page` 只布局从指定 Unicode 标量偏移开始的一页，`[start,end)` 包含消耗的换段符。返回错误时不推进 end；不根据估计页码改写位置。
- 依据字号、字形 advance、行高、边距、首行缩进和段间距计算完整行。行高不足以容纳字体 ascender 和 descender 时明确失败。英文优先在空格/连字符处折行，长词允许分行。
- 本页最多消耗一个视口的完整行；不整书预排。每行超过 65536 标量的异常输入会被拒绝。基础中日韩禁则优先避免常见闭合标点置于行首；不宣称完整实现 Unicode 换行算法。
- 改字号后可将同一源标量作为新页起点；源偏移到书籍锚点的映射由文档与 session 管理。
- EPUB `h1`–`h6` 目前统一按标题处理：无首行缩进，字号约为正文的 1.15 倍并加粗；标题上下留白，页末尽量与下一行正文同页。原 EPUB 的不同标题级别尚未分别设定字号。正文仍使用原有字号、行高、两字缩进和零段间距。

设计默认正文 38 px、行高 57 px、左右边距 36 px。实际 WenKai 度量为 ascender 36、descender 10 px；Noto UI 28 px 的对应值是 33 和 9 px。绘制调用使用 baseline，不能将 top 直接当 baseline，否则中文上部会被裁切。

## 内置字体与许可

`fonts/PicoPageFallback.otf` 从上述 Noto Sans SC 裁剪生成，共 4,085 个字符、916,468 字节，SHA-256 `717338dd42f6fd4d608980ef2e9161141b43069cf27a376498c7149eca754e92`。它有两个用途：TF 卡上的界面字体打不开时显示界面和错误提示；TF 卡上没有可用正文字体时显示正文。

字符范围由三部分组成，完整清单在 `fonts/fallback_characters.txt`：

- ASCII、脚本内的界面文案，以及 `--strings` 指定源文件中的全部字符。源字体缺少其中任何一个时生成失败。
- 《通用规范汉字表》一级字 3,500 个，清单在 `fonts/common_characters.txt`。源字体缺少其中任何一个时生成失败。
- 正文常见符号：U+00A0–017F、U+01CD–01DC、U+2000–206F、U+2460–2473、U+3000–303F、U+FF00–FFEF。源字体没有的码位直接跳过。

内置字体不能替代完整中文字体：一级字以外的简体字、繁体字和生僻字不在其中。子集保留了 `.notdef` 轮廓，缺少的字符显示为方框。正文使用内置字体且当前页有缺字时，页面照常显示，状态栏提示缺字时可在排版中更换字体；没有缺字的页面显示页码。实测两本繁体公版书（《呐喊》《阿Q正传》）约 19% 的字符缺字，简体演示书缺字数为 0。

`common_characters.txt` 取自 Unicode 17.0.0 的 Unihan 数据库：`https://www.unicode.org/Public/17.0.0/ucd/Unihan.zip`，SHA-256 `f7a48b2b545acfaa77b2d607ae28747404ce02baefee16396c5d2d7a8ef34b5e`。提取 `Unihan_OtherMappings.txt` 中 `kTGH` 字段值为 `2013:1` 至 `2013:3500` 的字符，按序号排列，每行 50 个。Unihan 数据适用 Unicode License v3；仓库只包含这 3,500 个字符的清单，不包含 Unihan 文件。

新增界面文案后运行：

```sh
python3 -m venv .local/font-tools
.local/font-tools/bin/pip install fonttools==4.60.1
.local/font-tools/bin/python tools/setup_fonts_subset.py \
  --strings firmware/components/reader_app/app.cpp \
  --strings firmware/main/app_main.cpp \
  --strings firmware/components/reader_sync/provision.cpp
```

字体字节编译进固件只读数据段，由 Flash 映射读取，不复制到 RAM。真机用内置字体阅读时，内部空闲内存与使用卡上字体时相同（约 137 KB）；字体打开耗时没有单独测量。

fonttools 只用于维护生成步骤，普通构建不需要它。字体子集修改了字形范围和字体名称，名称表及 CFF 名称已改为 `Pico Page Fallback`，保持 OFL 1.1。已阅读上游 OFL 及字体版权字段；随字体分发的版权是 © 2014–2021 Adobe。上游 LICENSE 未另列 Reserved Font Name；派生字体仍主动改名。`Noto-OFL.txt` 和 `LXGW-OFL.txt` 随准备目录字体一起复制；内置子集保留版权与许可元数据。

本软件部分基于 FreeType Team 的工作。Portions of this software are copyright © 2025 The FreeType Project (https://freetype.org). All rights reserved. FreeType 采用 FTL，见组件内 `FreeType-FTL.txt`；`fthash` 的 MIT 风格许可和 Adobe CFF 解释器的附加专利条款见 `FreeType-extra-notices.txt`。二进制分发时必须保留这些说明和字体 OFL。这只说明本组件义务，不替代 BSP 或整个项目的发布许可核对。

## 已执行验证与边界

2026-10-05，AppleClang 21：共享库与 `tests/graphics_test.cpp` 实际编译通过。同一检查分别使用内置字体、WenKai、Noto，均通过像素半字节顺序、中文提示覆盖、28/38/56 px 连续分页、完整行边界、非 BMP 标量、同偏移重排和无效行高检查。这验证 host 排版与字形代码，不验证 TF 卡吞吐、设备内存峰值、面板灰阶、残影或阅读舒适度。


2026-10-06 真实鲁迅 EPUB 检视修正：小标点 `﹐﹒﹖﹗`、常见引号和括号加入行首/行尾禁则；源段首已经包含全角空格或制表缩进时不再叠加配置缩进。源标量和保存锚点保持不变。三档字号的连续分页和边界检查通过，实际截图在 `artifacts/runtime/lu-xun-reading.png`。

2026-10-06 增加共享 PNG/JPEG 图片解码与灰阶缩放；接口、内存限额、许可和实际验证见[图片实现](image-runtime.md)。正文仍由 FreeType 排版，图片按内容块与文字共同分页。

常规字重（`regular`）保留所选字体文件的原有字重，旧配置显式指定 `-Bold`、`-Medium` 或 `-Light` 文件时不会自动替换成 Regular 文件；重新选择字体也恢复到该文件的原有字重。其他字重选择仍优先使用已安装的同族文件。
