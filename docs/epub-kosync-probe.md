# EPUB 与 KOSync 最小探针

核查日期：2026-10-05。状态：`pass_host_only`，仅身份计算、所选章节文本解析和源位置映射。没有正文分页、字体渲染、Read Pico 正文显示、原生 XPointer 转换或真实 KOSync 往返结果。

## 本轮内容路径

采用 Python 3.11+ 标准库 `zipfile` / `ElementTree`，从原 EPUB 读取 container、OPF、spine，只解析指定章节。原书以 SHA-256 标识；另输出按 KOReader 源码采样规则计算的部分 MD5。本工具不写回原书，不生成设备必需阅读包，不建立第二套正文后端。JSON 是开发诊断输出，`probe_version` 和规范化名称不是已冻结的固件存储或同步格式。

选择该入口的理由是：本轮可以独立核对真实 ZIP、章节顺序、原书身份和源文本位置，无需同时移植 CrossPoint 的 Arduino、存储、字体和排版缓存。Python 探针不是 ESP-IDF EPUB 引擎。未来设备内容通路仍须由中文分页、内存预算和实际 KOReader 对照决定。

### 可运行命令

在仓库根目录执行：

```sh
python3 -m unittest discover -s tests -p test_epub_probe.py -v
python3 tests/make_epub_fixture.py /tmp/pico-page-probe.epub
python3 tools/epub_probe.py /tmp/pico-page-probe.epub --spine 0 > /tmp/pico-page-probe-chapter0.json
python3 tools/epub_probe.py /tmp/pico-page-probe.epub --spine 1 > /tmp/pico-page-probe-chapter1.json
```

实际环境为 Python 3.14.7。四组测试通过、命令退出码均为 0。合成书包含两章原创中文、英文、内联强调、XML 空白、NBSP、`𠮷`、`😀`、列表、脚注和长段落。ZIP 时间戳和文件顺序固定；本次生成文件为 2,140 字节：

| 项目 | 实际结果 |
|---|---|
| SHA-256 | `62338330991b0244f2388036892a2891546fd9fe446fab0547c18b9707a6b118` |
| 部分 MD5 | `acc8613b9b4639011c4ff6dbdef6b6f7` |
| 第 0 章 | 75 个输出标量，18 个源映射段 |
| 第 1 章 | 1,950 个输出标量，5 个源映射段 |

测试验证同名 EPUB 内容更换后 SHA-256 不同；第二章 XML 损坏不阻止解析第一章。每个有来源的输出字符均与保存的 XML 文本节点位置核对；非 BMP 字符占一个标量。异常输入覆盖 ZIP 路径越界、OPF 相对路径越界、外部 URL、UTF-8/UTF-16 DTD、过深 XML、超限资源、缺图、畸形章节、表格和样式。拒绝输入时报错，CLI 退出码为 2。

### 源映射语义和范围

- `source_nodes` 保存 ElementTree 解码后的文本值；`#text` 是元素起始文本，`#tail` 是其结束标签后的文本。路径中的同名兄弟索引从 1 开始。它们是本探针的节点标识，不能当作 KOReader XPointer。
- 所有文本偏移都是 Unicode 标量索引，不是 UTF-8 字节或 UTF-16 单元。XML 已完成实体解码和行尾规范化；注释、处理指令和 CDATA 的词法边界不保留，因此这里不提供原始 XML 字节偏移。
- XML 空白 `SP/TAB/CR/LF` 连续出现时合并为一个空格，并去除块边界空白；NBSP 保留。合并后的空格映射到首个保留的源空白；被删除空白没有一对一逆映射。
- 块元素之间插入换行，源映射为 `null`。`source_at()` 将正文输出标量映射回节点及节点内标量；不把插入换行伪装成原书字符。
- 支持一个 package、线性 XHTML spine、UTF-8 XML，以及代码列出的基础段落、标题、容器、内联和列表元素。脚注按文档顺序输出并标 `footnotes_in_document_order`，尚无脚注跳转。图片只核对本地资源可读性、记录资源路径和 alt，并标 `images_not_rendered`；不解码、替代或显示图片。
- 外部资源、加密/混淆 EPUB、非线性 spine、CSS/内联样式、`xml:space` 覆盖和未支持元素明确拒绝。不宣称任意 EPUB 可读，不据此标记 EP01、EP02 或任何设备验收通过。

上限是 host 探针的防护预算：原文件 16 MiB、ZIP 128 项、单项解压 1 MiB、声明解压总量 8 MiB、压缩比 200:1、XML 深度 64/节点 10,000、选定章源文本和输出各 200,000 标量。ZIP 不解压到文件系统；在读取前拒绝重复路径、符号链接、加密和非 STORE/DEFLATE 压缩。读取时再次限制大小，XML 建树时限制深度和节点数，并在 DTD 回调中拒绝 DTD。不读取未选章节正文；原书快照和 ZIP 目录仍在 host 内存中，尚未验证设备 RAM 或长书吞吐。

## 固定源码与依赖审查

本轮通过 GitHub commit/tree API 与固定 commit 的 raw 文件核对以下内容。上游研究文件保存在 `/tmp/pico-page-epub-research/`，不作为项目编译依赖或随仓库分发。

| 来源 | 固定版本 | 本轮用途与许可证据 |
|---|---|---|
| CrossPoint | `50823ffa125b437b4c9fe5882f326458780198da` | 阅读位置映射、章节缓存、EPUB 解析边界；根 [LICENSE](https://github.com/crosspoint-reader/crosspoint-reader/blob/50823ffa125b437b4c9fe5882f326458780198da/LICENSE) 为 MIT |
| CrossPoint vendored Expat | 随上述 commit；文件头标注 `2.7.3+` | [xmlparse.c 文件头](https://github.com/crosspoint-reader/crosspoint-reader/blob/50823ffa125b437b4c9fe5882f326458780198da/lib/expat/xmlparse.c) 含独立 MIT 授权和版权名单 |
| CrossPoint vendored miniz | 随上述 commit | [miniz.c 文件头](https://github.com/crosspoint-reader/crosspoint-reader/blob/50823ffa125b437b4c9fe5882f326458780198da/lib/miniz/third_party/miniz.c) 含独立 MIT 授权及 RAD/Valve/Rich Geldreich 等版权信息 |
| KOReader | `2ce49e115fc1bcc47b473078c549e59b3922105b` | 检查 KOSync/部分 MD5；根 [COPYING](https://github.com/koreader/koreader/blob/2ce49e115fc1bcc47b473078c549e59b3922105b/COPYING) 为 AGPLv3 |
| KOReader base | `3fea60920966f6ecf990ea0d93d7867cde938670` | 由上述 KOReader gitlink 固定；检查 CREngine Lua 桥接；[ffi/sha2.lua](https://github.com/koreader/koreader-base/blob/3fea60920966f6ecf990ea0d93d7867cde938670/ffi/sha2.lua) 自带 MIT 许可声明 |
| KOReader CREngine | `f4a8be93255a439db4777d00f0bc53316acabebd` | 由上述 base 的 `thirdparty/kpvcrlib/crengine` gitlink 固定；[README.TXT](https://github.com/koreader/crengine/blob/f4a8be93255a439db4777d00f0bc53316acabebd/README.TXT) 声明第三方目录外源码为 GPLv2，第三方目录需单独审查 |

本轮未复制或修改上述实现进入工程。探针为独立标准库代码；部分 MD5 依据公开采样规则实现。不能因 CrossPoint 根目录为 MIT 就把整个应用依赖集合视为 MIT。若以后复制 MIT 实现，需保留对应版权和授权文本；若采用 KOReader/CREngine 实现，需按实际组合与分发方式审核对应 copyleft 和源码提供义务，不能只贴顶层 MIT 文本。本记录不决定 Pico Page 总许可证，也不等于完整依赖审计。

### CrossPoint 的实际耦合

[CrossPointPosition.h](https://github.com/crosspoint-reader/crosspoint-reader/blob/50823ffa125b437b4c9fe5882f326458780198da/lib/KOReaderSync/CrossPointPosition.h) 以 spine、可见文本 codepoint 偏移和「是否已解析/已映射」标志表达内容与页映射状态。[ProgressMapper.cpp](https://github.com/crosspoint-reader/crosspoint-reader/blob/50823ffa125b437b4c9fe5882f326458780198da/lib/KOReaderSync/ProgressMapper.cpp) 结合真实 XPath、章节缓存页起点及百分比降级，不是独立于正文引擎的通用位置转换器。依赖 `Epub`、`Section`、`GfxRenderer`、HTML 实体、UTF-8 处理和日志，页面结果取决于当前排版缓存。

[ChapterXPathResolver.cpp](https://github.com/crosspoint-reader/crosspoint-reader/blob/50823ffa125b437b4c9fe5882f326458780198da/lib/KOReaderSync/ChapterXPathResolver.cpp) 通过 Arduino `Print` 接口流式读取章节，使用 Expat 回调维护祖先/兄弟和文本节点序号，计数段落及列表内容，并处理注释、PI、CDATA 边界；其正文可见性规则还依赖 [VisibleTextUtils.h](https://github.com/crosspoint-reader/crosspoint-reader/blob/50823ffa125b437b4c9fe5882f326458780198da/lib/Epub/Epub/VisibleTextUtils.h)。源码中的「KOReader-compatible」注释不是本项目的兼容性证据。

[Section.cpp](https://github.com/crosspoint-reader/crosspoint-reader/blob/50823ffa125b437b4c9fe5882f326458780198da/lib/Epub/Epub/Section.cpp) 在分页时记录可见偏移，读取页起点表恢复位置，同时依赖字体缓存、`HalStorage`、序列化、CSS、断词及 `ChapterHtmlSlimParser`。`Epub` 又依赖 ZIP、图片转换、缓存与存储。[platformio.ini](https://github.com/crosspoint-reader/crosspoint-reader/blob/50823ffa125b437b4c9fe5882f326458780198da/platformio.ini) 还引用 FreeInk SDK（gitlink `aef1a6c89e36f331b2e1aacbbf7ce0debdeb732a`）、ArduinoJson、SdFat 和图片等依赖。本轮未集成这些依赖，也未核完 FreeInk/字体/图片库的完整许可证闭包。

### KOReader 的身份和位置不是同一问题

[util.partialMD5](https://github.com/koreader/koreader/blob/2ce49e115fc1bcc47b473078c549e59b3922105b/frontend/util.lua#L1094) 在文件偏移 `0, 1024, 4096, 16384, …, 1073741824` 各取最多 1,024 字节，遇到 EOF 停止，将样本串联计算 MD5。首项来自 LuaJIT 的 32 位 `lshift(1024, -2)`，不是 256。采样不覆盖全部文件，不能替代项目强内容身份。

本轮从固定 `util.lua` 提取原始函数，使用固定 base 的 `ffi/sha2.lua` 在已安装的 `/opt/homebrew/bin/luajit`（LuaJIT 2.1.1787165859）执行：

```sh
/opt/homebrew/bin/luajit /tmp/pico-page-epub-research/upstream-partial-md5.lua /tmp/pico-page-probe.epub
```

退出码 0，结果为 `acc8613b9b4639011c4ff6dbdef6b6f7`，与 Python 输出一致。包装文件仅定义 `util`、绑定 `bit.lshift` 和 `sha2.md5`，插入未修改的 `util.partialMD5` 函数后输出结果。这是一项上游函数对照，**不是实际 KOReader 客户端导出的阅读记录**；JSON 因此标 `identity_status=source_algorithm_only`、`position_status=not_verified`，不输出猜测的 XPointer。

[KOSync 插件](https://github.com/koreader/koreader/blob/2ce49e115fc1bcc47b473078c549e59b3922105b/plugins/kosync.koplugin/main.lua)可选文件名 MD5 或内容部分 MD5；重排文档以 `GotoXPointer` 恢复。插件还会在舍入百分比相同或 progress 字符串相同时跳过更新；因此只比较百分比会漏掉内容偏移问题。

[base/cre.cpp](https://github.com/koreader/koreader-base/blob/3fea60920966f6ecf990ea0d93d7867cde938670/cre.cpp) 的 `getXPointer()` 导出引擎 bookmark，`gotoXPointer()` 在引擎 DOM 中解析并跳转。[CREngine lvtinydom.cpp](https://github.com/koreader/crengine/blob/f4a8be93255a439db4777d00f0bc53316acabebd/crengine/src/lvtinydom.cpp) 有 DOM 版本和规范化路径规则；当前代码记录 20240114 的 DocFragment 生成变化，以及 20260812 的首兄弟显式 `[1]` 规则。因此原始 XHTML 路径、文本单位、引擎 DOM 版本和配置必须一起核对。

## 尚缺的实际证据

只读检查在 PATH、`/Applications`、`~/Applications` 未找到 KOReader 启动程序，`/Users/wulu/workspace/koreader` 是空目录。本机有 LuaJIT 和 SDL2，但这不等于存在可运行的 KOReader。进程查询受当前执行环境限制，未把查询失败当作「没有任何 KOReader 进程」的证据。本轮未安装客户端、未操作个人 EPUB 或 `.sdr`、未联系 KOSync 服务，也没有账户凭据。

下一步最小验证：在实际 KOReader 打开上述未修改合成 EPUB，记录客户端版本、DOM 版本、原书摘要、正文片段和真实 progress；至少取第一章的非 BMP/内联文本点、第二章跨章点及一次回读。先逐项核对源节点/文本偏移，再实现受限 XPointer 转换，并用不同字号做双向对照。没有这些输出前，SYNC01–SYNC03 的原生位置兼容门槛保持 `not_run`；不能用本探针自造路径作唯一预期值。
