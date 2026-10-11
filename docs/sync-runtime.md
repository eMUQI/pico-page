# KOSync 运行时与位置兼容范围

更新日期：2026-10-10。`reader_sync` 已实现有界配置读取、原生 KOSync 请求/回包编解码，以及经过实际 CREngine 对照的精确位置转换。官方 macOS KOReader 完整客户端已使用本地测试服务完成双向位置与重启接续联测。2026-10-09 的开发记录包含 Read Pico 上传后 Kindle 接续，以及采用远端位置后呈现、保存和重开的设备证据；这仍不等于完整异常场景和重复稳定性验收通过。下文带日期的 host 联测记录保留当时范围，不能解释为目前从未联测真机。

## 配置与交互边界

在 TF 卡 `PicoPage/sync.toml` 设置：

```toml
schema_version = 1
endpoint = "https://your-sync.example"
username = "your-account"
userkey = "your-existing-koreader-userkey"
device_id = "pico-page-personal-device"
dom_version = 20260812
```

在设备上，可在「设置 › KOReader 同步」中用屏幕键盘填写：设备验证账户后写入本文件，`device_id` 沿用已有值，没有时使用 `pico-page-<MAC>`；整个文件被重写，手写的注释不保留。

也可以在电脑上运行 `python3 tools/setup_network.py [存储卡挂载路径]` 生成本文件和 `network.toml`：脚本询问账户密码并只写入其 MD5，保留已有的 `device_id` 和 `dom_version`，默认先向同步服务验证账户（`--offline` 跳过）。输入的 Wi-Fi 加入已保存的网络（最多 5 个），同名网络只替换密码；现有 `network.toml` 无法读取时不写入任何文件。手工编辑仍然有效。

`userkey` 是 KOReader 使用的账户密钥，即账户密码的 MD5 十六进制值；它不是显示用标识。本项目不记录密钥、不将其放入请求正文或诊断信息。配置属于私密文件，不应加入仓库或公开截图。必须使用 HTTPS。`device_id` 是此设备的稳定标识，不能复制其他设备的 ID。`dom_version` 必须与目标 KOReader 对该书采用的 DOM 版本一致，目前只验证了 20240114 和 20260812。更早版本、未列出的版本、按文件名匹配文档的模式均不在当前范围；文档身份使用原 EPUB 的部分 MD5。

Wi-Fi 由板级网络适配读取 `PicoPage/network.toml`，字段见板级运行说明。同步配置上限 8 KiB；解析失败保留原文件，不写入默认值，也不输出含密钥的 TOML 错误行。

组件只生成请求和候选位置，不改变 session、正文或存储。应用必须将请求绑定到书籍、会话/排版代次、账户范围及发送时已保存的记录，丢弃旧结果。远端位置需要用户确认后才交给正文呈现；呈现和保存成功后才成为新的本地记录。上传只接受已保存位置，`HttpRequest.uploaded_record` 保存发送时的精确版本；后续翻页不改变它。当前采用手动检查和明确确认的上传路径，不提供休眠自动上传或后台 outbox。「保留本地」结束候选处置，不能随后无声覆盖远端。

## 协议

依据固定 KOReader commit `2ce49e115fc1bcc47b473078c549e59b3922105b` 的 [`KOSyncClient.lua`](https://github.com/koreader/koreader/blob/2ce49e115fc1bcc47b473078c549e59b3922105b/plugins/kosync.koplugin/KOSyncClient.lua) 和 [`api.json`](https://github.com/koreader/koreader/blob/2ce49e115fc1bcc47b473078c549e59b3922105b/plugins/kosync.koplugin/api.json)，独立实现 GET `/syncs/progress/<partial_md5>` 与 PUT `/syncs/progress`。请求头使用 `accept: application/vnd.koreader.v1+json`、`x-auth-user` 和 `x-auth-key`。PUT 正文包含 document、progress、percentage、device、device_id。

percentage 只作为服务展示数据，不参与本项目的位置查找、冲突判定和前后关系判断；各客户端的计算方法不同，不能与本机百分比比较。远端位置映射成功后，面板显示按本机方法重新计算的百分比；只有无法映射时才显示服务器记录的数值。响应上限 32 KiB，拒绝重复字段、无效 JSON、NUL、无效位置和范围外百分比。200 空对象表示尚无记录；401/403 表示鉴权失败；其他错误保留本地记录。上传只把 200 作为发送版本的服务确认。202、取消已发请求、连接中断等情况不能保证远端未变化，再次上传前应重新读取核对。服务端没有条件写入保障，确认到实际写入之间仍存在竞争窗口。

## 精确映射与明确拒绝

本机 `Anchor` 为原 XHTML 文本节点路径与 Unicode 标量偏移。CREngine 使用合并的文档 DOM；例如原始 `/html[1]/body[1]/p[1]/em[1]/text()[1]` 在第一章对应 `/body[1]/DocFragment[1]/body[1]/p[1]/em[1]/text()[1]`。转换必须同时核对 spine 顺序、节点路径和逐节点 XML 空白折叠，不能只替换路径前缀。原文 `  中文 ` 的「中」在原节点偏移 2，在 CREngine 节点偏移 1。`𠮷` 和 `😀` 各占一个标量，不按 UTF-16 的两个单元计数。

20240114 的引擎输出会省略部分唯一节点的 `[1]`；20260812 显式输出首索引。入站允许这两种等价写法，出站使用显式索引。实际引擎已验证两种版本均可解析。

当前精确范围是 `Chapter.plain_dom` 为真的基础 XHTML，且目标路径满足已核对的 HTML 嵌套规则。普通 CSS、DOCTYPE、元数据、内联样式、基础 HTML5 块与行内元素、`<img>` 和 `<hr>` 已经纳入对照。图片位置映射为元素指针：本机图片锚点 `/html[1]/body[1]/p[3]/img[1]` 对应 `/body[1]/DocFragment[n]/body[1]/p[3]/img[1].0`，偏移只能是 0；CREngine 对以图片开头的页给出的书签就是这种形式，它在图片外自动加入的容器节点不出现在这两个 DOM 版本的指针里。正文层的图片（`/body[1]/img[k]`）可以映射，直接位于正文层的文字仍不在范围内。SVG、`<image>`、仅由正文背景图构成的页、其他媒体、表格、ruby、pre、xml:space、正文注释/PI/CDATA、外部命名空间、未知元素等仍使映射失败。CSS 预格式空白、生成内容、转义标识符和未展开的 `@import` 需要进一步解析，当前明确拒绝。块首纯空白被 CREngine 删除时调整文本兄弟索引；存在影响该判断的 CSS display 声明时拒绝猜测。阅读后端仍可明确降级阅读其中一部分内容，不能由「可读」推断「可精确同步」。拒绝以章节分段为单位：分段里出现一个范围外的元素（例如 `<table>`），该分段内的全部位置（包括普通段落里的文字）在上传和下载两个方向都返回 `sync::kUnsupportedStructure`。`<img>` 曾属于这种元素，带插图的书因此大部分章节无法同步（Issue #40），现已纳入范围；界面对这一原因单独提示，固件以 WARN 记录映射失败的原因文本，其中不含正文、位置和账户。远端偏移落入已删除空白、元素级位置、非文本位置、无效章索引或不支持 DOM 时均返回错误，不跳到第一页，不按百分比寻找附近位置，不把候选写成已接续。

## 真实引擎对照与复现

`tools/check_koreader_mapping.py` 下载并校验固定 CREngine 源码与 xxHash，全部构建在 `.local/`。需要本机已有 CMake、C/C++17、pkg-config、FreeType、HarfBuzz、zlib、libpng、libjpeg、OpenSSL。脚本不安装全局依赖。先完成 `python3 tools/setup_fonts.py`，再执行：

```sh
python3 tools/check_koreader_mapping.py
```

脚本同时生成并核对 `tests/make_document_fixtures.py` 的 `image-mapping.epub`（图片位于段落内、独占段落、块容器内、正文层、链接和列表项内、连续多张，以及 `<hr>`）。该独立 host 程序链接未修改的实际 CREngine，加载同一个原创合成 EPUB，并真正执行 Render、createXPointer、引擎 XPointer 序列化及选定位置的 goToBookmark/SetPos。每个可映射标量都会核对引擎节点内的实际字符；每 53 个正文标量和章末位置抽查反解，并实际恢复引擎 `getBookmark()` 生成的书签。引擎分页可能从上一章开始，验证按书签真实资源核对。

| 对照参数 | 实际结果 |
|---|---|
| CREngine commit | `f4a8be93255a439db4777d00f0bc53316acabebd` |
| 源码 tar.gz SHA-256 | `0551a3daef8d08203f35aefaf00af8227aa1925effaef6d74839b8bb77012583` |
| 合成书 SHA-256 | `62338330991b0244f2388036892a2891546fd9fe446fab0547c18b9707a6b118` |
| DOM 20240114，28/38/56 px | 每组 2018 个精确源位置往返通过 |
| DOM 20260812，28/38/56 px | 每组 2018 个精确源位置往返通过 |
| 图片位置合成书 `image-mapping.epub`（2026-10-11），两种 DOM 版本，28/38/56 px | 每组 3935/3935 个源位置，其中 13 张图片逐一核对为同一 `img` 元素和同一 `src`；193 个位置反解，193 个引擎书签通过 |
| 证据日志 | `.local/koreader-reference-check.log` |

上述 2,018 个全量往返结果是早期合成书检查。当前扩大验证还包含带 CSS/DOCTYPE 的样本与[两个公开鲁迅 EPUB](public-books.md)，并分别报告正向标量数、抽查反向数和真实引擎书签数。参考程序使用 Release 构建；库源码未修改。反向定位只检查相同父路径的文本兄弟，避免遍历每个候选时再次扫描整章形成二次方开销。

最新原始 EPUB 结果（两个 DOM 版本 × 28/38/56 px，每组均相同）：

| 文件 | 正向字符 | 抽样反向位置 | 实际引擎书签 | 未支持 |
|---|---:|---:|---:|---|
| 原创合成书 | 2,018 / 2,018 | 41 | 41 | 无 |
| CSS/DOCTYPE 样本 | 1,960 / 1,960 | 40 | 40 | 无 |
| 《阿Ｑ正傳》 | 45,172 / 45,184 | 857 | 857 | 12 个图片封面占位标量 |
| 《吶喊》 | 98,722 / 98,734 | 1,875 | 1,875 | 12 个图片封面占位标量 |

各组运行退出码均为 0。日志为 `.local/koreader-synthetic-current.log`、`.local/koreader-styled-check.log`、`.local/koreader-aq-check.log` 和 `.local/koreader-nahan-check.log`。正向分母只包含拥有来源的标量，排版插入换行不在其中。图片占位未伪装成原书文字，正文和许可页的可映射字符全部对照成功；这不扩大为所有 EPUB 或真实客户端的验收结论。

此外，`tests/sync_test.cpp` 检查来自上述引擎的已知中文/非 BMP/跨章位置、unsupported 分支、旧上传版本快照、鉴权/无记录/无效响应及配置错误不泄露密钥。没有向外部同步服务发送书籍身份或进度，没有 Read Pico 真机接续结果。真实客户端的补充证据与边界见下一节；它不替代 SYNC01/SYNC02 的设备验证。

## 官方客户端的本地双向联测

下载并核对了官方 [GitHub Actions 构建产物](https://github.com/koreader/koreader/actions/runs/37257705797)，不是重新实现的 KOReader 客户端：

| 项目 | 固定值 |
|---|---|
| KOReader commit | `83f443ce4b3dfd2e3366b97bf3e3333dfc46718e` |
| 官方版本 | `v2026.07.2-220-g83f443c_2026-10-04_macos` |
| ARM64 artifact ID | `11323735856` |
| 下载文件 SHA-256 | `ec5342a53f85131cb7a903cd6ed550619e06b62899b95ba25cca3e13ff3fb258` |
| 客户端 DOM | `20260812` |
| 显示运行方式 | 官方 `reader.lua`、完整 ReaderUI、SDL dummy 驱动、684 × 1216 framebuffer |

解包与运行数据均在 `.local/`，没有安装系统应用或修改全局工具链。通过官方 user-patch 入口驱动现有动作，没有替换阅读器、文档引擎、KOSync 插件、Spore HTTP 客户端或跳转处理器。源 EPUB 以内容不变的隔离副本打开，避免 KOReader 的 `.sdr` 文件污染原书目录。每轮使用独立 `KO_HOME` 和一次性随机账户；账户只存在于本地服务内存，临时配置在结束时删除。

实际流程如下：

1. 官方客户端注册、正确鉴权、错误鉴权，并读取尚无进度的原书。
2. 官方 ReaderUI 打开正文页，原 KOSync 插件 `updateProgress` 发送真实 HTTP PUT。
3. host 桥接程序调用生产 `reader_sync::parse_remote/from_xpointer`，反解为原 EPUB 的 source anchor；再核对官方引擎从该 XPointer 取出的原文。上传书签必须属于实际显示页。
4. 生产 `reader_sync::to_xpointer/put_request` 生成另一处原文位置和上传请求，本地服务保存该请求。
5. 官方 KOSync 插件 `getProgress` 获取该记录，经原 `GotoXPointer` 处理器显示目标页；指针、当前页及连续 24 个原文标量必须一致。
6. 调用官方正常关闭路径，重启完整客户端，从其实际 sidecar 恢复同一指针和原文。

字号通过官方 `copt_font_size` 配置在打开文档前设置。早期在封面修改字号后立即取书签的试跑没有提供可靠的已显示页证据，因此不计入最终验证；最终脚本明确检查上传指针的页面归属。

最终六组均通过双向原文核对与实际客户端重启恢复，进程退出码均为 0：

| 原始 EPUB | 字号 | 官方客户端上传页 | 接收原文位置后所在页 | 重启恢复 |
|---|---:|---:|---:|---|
| 《阿Ｑ正傳》 | 28 px | 37 | 60 | 同一 XPointer 与原文 |
| 《阿Ｑ正傳》 | 38 px | 65 | 103 | 同一 XPointer 与原文 |
| 《阿Ｑ正傳》 | 56 px | 138 | 210 | 同一 XPointer 与原文 |
| 《吶喊》 | 28 px | 113 | 81 | 同一 XPointer 与原文 |
| 《吶喊》 | 38 px | 186 | 130 | 同一 XPointer 与原文 |
| 《吶喊》 | 56 px | 374 | 251 | 同一 XPointer 与原文 |

例如《阿Ｑ正傳》的接收位置在三种字号下始终为 `/body[1]/DocFragment[2]/body[1]/h5[128]/text()[1].51`，对应「小Ｄ是什麼東西呢﹖……」；实际页码随排版变化，位置没有由百分比反推。汇总证据另存 `.local/koreader-client-final-results.json`。

复现命令：

```sh
python3 tools/check_koreader_client.py \
  --book .local/public-books/lu-xun-aq-25332.epub \
  --book .local/public-books/lu-xun-nahan-27166.epub
```

脚本需要 macOS ARM64、现有 host CMake/C++ 依赖和 `gh`。首次下载官方 Actions 产物需要现有 GitHub CLI 登录，产物保留期受 GitHub 控制；后续运行校验本地缓存摘要。默认依次验证 28、38、56 px，也可用 `--font-size` 指定单组。实际日志为 `.local/koreader-client-final.log`；每轮目录 `.local/koreader-client-runs/<书名>-<字号>-<随机标识>/` 保留去除账户信息的 `result.json`、客户端及重启日志、实际 framebuffer `client-page.png`。

测试服务是只监听 `127.0.0.1` 的可控协议 fixture，传输使用 HTTP。桥接程序仅在测试运输层将生产组件生成的 HTTPS 请求送入 loopback；固件与产品配置的 HTTPS 限制未变。这验证了完整官方客户端、真实协议请求和本项目位置转换的互通，不验证公共服务、设备 Wi-Fi、固件 TLS、网络中断期间的远端状态，也不代表使用者在真机上完成了手动接续操作。

## 外部代码边界与许可

固件的协议和映射为独立实现，没有复制 KOReader/CREngine 源码或将其链接进固件。KOReader 根许可为 AGPLv3；研究参考的 CREngine 根声明为 GPLv2，具体子目录另有许可。本轮只将其构建为本地校验程序，不随产品发布；若分发该校验二进制，应先完成其 GPL 与外部链接依赖的分发义务核对，不能套用 Pico Page 尚未决定的总许可证。本地参考依赖 xxHash 0.8.3 为 BSD-2-Clause，源文件保留上游版权与许可。
