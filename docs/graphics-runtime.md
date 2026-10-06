# 共享字体与分页

`reader_graphics` 在 host 和 ESP-IDF 编译同一套 FreeType 2.14.1 源码；不依赖系统字体，
不从 HTML、PNG 或浏览器截图生成正文。当前支持 TrueType 和 OpenType CFF 轮廓字体。

## 准备资源

运行 `python3 tools/setup_fonts.py`。脚本将固定源码下载到 `.local/`，将正文和 UI 字体写入
`.local/card/PicoPage/fonts/`，每个下载文件都检查 SHA-256。可以用 `--card <目录>`
指定准备目录；这一步不挂载设备、不擦写 TF 卡。完整字体和源码下载不纳入 Git。

| 资源 | 固定来源 | SHA-256 |
|---|---|---|
| FreeType 2.14.1 | `freetype/freetype` commit `526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b` | tar.gz `d24f53b60034dc5c48e90d122d432a8f04a05b9e0a98c8d8e49b4a759207ce2e` |
| LXGW WenKai Regular | `lxgw/LxgwWenKai` v1.520，tag commit `b79944853c0725d9f9c8996f07f2476cff1e9463` | `8d6ba638ac9553413354cfaab97637c1cd778444e259441ea1e5f8fb2c697fba` |
| Noto Sans SC Regular | `notofonts/noto-cjk` Sans2.004，commit `523d033d6cb47f4a80c58a35753646f5c3608a78`，`Sans/SubsetOTF/SC/NotoSansSC-Regular.otf` | `faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9` |

FreeType 使用上游默认的 stdio 文件流，未改第三方源码。host 使用默认内存接口；ESP 使用
`FT_New_Library` 和静态寿命的 `FT_Memory`，将字体内部分配放入 PSRAM，释放与重分配保持
FreeType 的所有权约定。重分配失败保留旧块，零尺寸释放，负尺寸拒绝。组件只选取 TrueType、
CFF、SFNT、PS 辅助模块和灰阶/单色栅格化模块；关闭压缩字体、SVG、彩色字形和外部图像解码。
没有链接 HarfBuzz，复杂文字塑形、双向文字及字体间自动逐字回退暂不支持。
中英文基础排版使用实际字宽；全字体中没有的字符会呈现该字体的缺字字形。
工厂默认路径为 `fonts/LXGWWenKai-Regular.ttf`，与资源准备脚本和示例配置一致。
显式选择的文件缺失时仍保留原偏好并提示回退，不能用缺失字体保存排版。

每个字体对象最多缓存 8,192 个字宽结果，键同时包含字号与 Unicode 标量；重新打开字体时清空。
缓存仅复用 FreeType 已成功返回的精确字宽，不估算中英文宽度、不改变保存锚点。
刷新档位：`clear` 每页全刷；`balanced` 使用 16 灰阶 GL16，每 8 次正文更新后全刷；
`fast` 使用八灰阶 GL16，每 5 次正文更新后全刷。全刷始终为 GC16。

每个字体对象另缓存已栅格化的字形位图，键相同，上限 1 MiB，超限时淘汰最久未绘制的一半，重新打开字体时清空；
缓存命中与首次栅格化的像素相同。
2026-10-06 的改前/改后探针对两本鲁迅 EPUB、28/38/56 px 的 2,060 页逐页比较边界和像素摘要，
输出完全相同；证据 `.local/font-psram-check/pages-before.log` 与 `pages-after.log`。

## 像素与排版契约

- 684×1216 竖屏，4 位灰阶，0 为黑色、15 为白色；偶数 x 位于字节低半部，奇数 x 位于高半部。
  一帧为 415872 字节。方向转换由板级显示适配负责，与官方 EPDiy 像素顺序一致。
- `Font` 单一所有者，设置字号、测量和绘制不能并发。`open_memory` 的字节由调用者保持有效。
- `layout_page` 只布局从指定 Unicode 标量偏移开始的一页，`[start,end)` 包含消耗的换段符。
  返回错误时不推进 end；不根据估计页码改写位置。
- 依据字号、字形 advance、行高、边距、首行缩进和段间距计算完整行。行高不足以容纳
  字体 ascender 和 descender 时明确失败。英文优先在空格/连字符处折行，长词允许分行。
- 本页最多消耗一个视口的完整行；不整书预排。每行超过 65536 标量的异常输入会被拒绝。
  基础中日韩禁则优先避免常见闭合标点置于行首；不宣称完整实现 Unicode 换行算法。
- 改字号后可将同一源标量作为新页起点；源偏移到书籍锚点的映射由文档与 session 管理。

设计默认正文 38 px、行高 57 px、左右边距 36 px。实际 WenKai 度量为 ascender 36、
descender 10 px；Noto UI 28 px 的对应值是 33 和 9 px。绘制调用使用 baseline，不能将 top
直接当 baseline，否则中文上部会被裁切。

## 内置提示字体与许可

`fonts/PicoPageFallback.otf` 从上述 Noto Sans SC 按本轮 UI 字符生成，保留 ASCII 和中文错误提示。
完整字形清单在 `fonts/fallback_characters.txt`。字体未作为完整中文正文替代方案。
生成器遇到源字体缺失的请求字符会失败，不把缺字框当作有效提示字形。
新增 UI 文案后运行：

```sh
python3 -m venv .local/font-tools
.local/font-tools/bin/pip install fonttools==4.60.1
.local/font-tools/bin/python tools/setup_fonts_subset.py --strings firmware/components/reader_app/app.cpp
```

fonttools 只用于维护生成步骤，普通构建不需要它。字体子集修改了字形范围和字体名称，
名称表及 CFF 名称已改为 `Pico Page Fallback`，保持 OFL 1.1。已阅读上游 OFL 及字体版权字段；
随字体分发的版权是 © 2014–2021 Adobe。上游 LICENSE 未另列 Reserved Font Name；派生字体仍主动改名。
`Noto-OFL.txt` 和 `LXGW-OFL.txt` 随准备目录字体一起复制；内置子集保留版权与许可元数据。

本软件部分基于 FreeType Team 的工作。Portions of this software are copyright © 2025
The FreeType Project (https://freetype.org). All rights reserved.
FreeType 采用 FTL，见组件内 `FreeType-FTL.txt`；`fthash` 的 MIT 风格许可和 Adobe CFF
解释器的附加专利条款见 `FreeType-extra-notices.txt`。二进制分发时必须保留这些说明和字体 OFL。
这只说明本组件义务，不替代 BSP 或整个项目的发布许可核对。

## 已执行验证与边界

2026-10-05，AppleClang 21：共享库与 `tests/graphics_test.cpp` 实际编译通过。
同一检查分别使用内置字体、WenKai、Noto，均通过像素半字节顺序、中文提示覆盖、
28/38/56 px 连续分页、完整行边界、非 BMP 标量、同偏移重排和无效行高检查。
这验证 host 排版与字形代码，不验证 TF 卡吞吐、设备内存峰值、面板灰阶、残影或阅读舒适度。


2026-10-06 真实鲁迅 EPUB 检视修正：小标点 `﹐﹒﹖﹗`、常见引号和括号加入行首/行尾禁则；
源段首已经包含全角空格或制表缩进时不再叠加配置缩进。源标量和保存锚点保持不变。
三档字号的连续分页和边界检查通过，实际截图在 `artifacts/runtime/lu-xun-reading.png`。

2026-10-06 增加共享 PNG/JPEG 图片解码与灰阶缩放；接口、内存限额、许可和实际验证见
[图片实现](image-runtime.md)。正文仍由 FreeType 排版，图片路径独立处理。
