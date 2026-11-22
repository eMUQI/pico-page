# KOSync 运行时与位置兼容范围

`reader_sync` 实现有界配置读取、原生 KOSync 请求与回包编解码，以及经实际 CREngine 对照的精确位置转换。验收状态见[开发状态](../development-status.md)。

## 配置与交互边界

`PicoPage/sync.toml`：

```toml
schema_version = 1
endpoint = "https://your-sync.example"
username = "your-account"
userkey = "your-existing-koreader-userkey"
device_id = "pico-page-personal-device"
dom_version = 20260812
```

可在设备「设置 → KOReader 同步」中用屏幕键盘填写：验证成功后整份重写本文件（不保留手写注释），`device_id` 沿用已有值，没有时使用 `pico-page-<MAC>`。也可在电脑上运行 `python3 tools/setup_network.py [存储卡挂载路径]` 生成本文件和 `network.toml`：脚本只写入密码的 MD5，保留已有的 `device_id` 和 `dom_version`，默认先向服务验证（`--offline` 跳过）。

- `userkey` 是 KOReader 的账户密钥，即密码的 MD5 十六进制值；不进入请求正文、日志或诊断信息。本文件是私密文件。
- 必须使用 HTTPS。
- `device_id` 是本设备的稳定标识，不能复制其他设备的 ID。
- `dom_version` 必须与目标 KOReader 对该书采用的 DOM 版本一致，目前只验证了 20240114 和 20260812；按文件名匹配文档的模式不在范围内。文档身份使用原 EPUB 的部分 MD5。
- 配置上限 8 KiB；解析失败保留原文件，不写默认值，也不输出含密钥的错误行。

组件只生成请求和候选位置，不改变 session、正文或存储。应用把请求绑定到书籍、会话／排版代次、账户范围和发送时已保存的记录，丢弃旧结果。远端位置经用户确认后才交给正文呈现，呈现并保存成功后才成为本地记录。上传只接受已保存位置，`HttpRequest.uploaded_record` 保存发送时的精确版本。只有手动检查和明确确认的上传路径，没有休眠自动上传或后台 outbox；「留在这里」结束候选处置，之后不能静默覆盖远端。

CrossPoint 托管服务收到低于现有百分比的上传时，会返回 200 并保留较高的位置（见其[服务端回归用例](https://github.com/crosspoint-reader/crosspoint-sync/blob/452322dd19bd8b9609a807748676a8198d383b6d/test/kosync.compat.test.ts#L181-L211)）；确认回退后的位置是否生效须重新检查远端。

## 协议

依据固定 KOReader commit `2ce49e115fc1bcc47b473078c549e59b3922105b` 的 [`KOSyncClient.lua`](https://github.com/koreader/koreader/blob/2ce49e115fc1bcc47b473078c549e59b3922105b/plugins/kosync.koplugin/KOSyncClient.lua) 和 [`api.json`](https://github.com/koreader/koreader/blob/2ce49e115fc1bcc47b473078c549e59b3922105b/plugins/kosync.koplugin/api.json) 独立实现 GET `/syncs/progress/<partial_md5>` 与 PUT `/syncs/progress`；请求头为 `accept: application/vnd.koreader.v1+json`、`x-auth-user`、`x-auth-key`；PUT 正文包含 document、progress、percentage、device、device_id。

percentage 只作为展示数据，不参与位置查找、冲突判定或前后比较，各客户端的算法不同。映射成功后面板显示按本机方法重算的百分比，只有无法映射时才显示服务器记录的值。响应上限 32 KiB，拒绝重复字段、无效 JSON、NUL、无效位置和范围外百分比。200 空对象表示尚无记录；401／403 表示鉴权失败；其他错误保留本地记录。上传只把 200 当作发送版本的确认；202、取消已发请求和连接中断都不能保证远端未变化，再次上传前须重新读取。服务端没有条件写入，确认与实际写入之间存在竞争窗口。

## 精确映射与明确拒绝

同步入口与异步 `ContentJob::MapUpload` 共用 `sync::map_upload_position()`，统一分段查找、XPointer 生成、上传百分比和映射错误。函数读取调用者独占的原书，不读写章节缓存，不修改进度；结果仍须通过任务代次、账户和已保存快照校验才能生成上传请求。

本机 `Anchor` 是原 XHTML 文本节点路径加 Unicode 标量偏移；CREngine 使用合并的文档 DOM，例如原始 `/html[1]/body[1]/p[1]/em[1]/text()[1]` 在第一章对应 `/body[1]/DocFragment[1]/body[1]/p[1]/em[1]/text()[1]`。转换必须同时核对 spine 顺序、节点路径和逐节点的 XML 空白折叠，不能只替换路径前缀：原文 `  中文 ` 的「中」在原节点偏移 2，在 CREngine 节点偏移 1；`𠮷`、`😀` 各占一个标量。20240114 会省略部分唯一节点的 `[1]`，20260812 显式输出；入站接受两种写法，出站使用显式索引。

精确范围是 `Chapter.plain_dom` 为真的基础 XHTML，且目标路径满足已核对的 HTML 嵌套规则：普通 CSS、DOCTYPE、元数据、内联样式、基础 HTML5 块与行内元素、`<img>` 和 `<hr>`。图片映射为元素指针，如 `/html[1]/body[1]/p[3]/img[1]` 对应 `/body[1]/DocFragment[n]/body[1]/p[3]/img[1].0`，偏移只能为 0；正文层的图片可以映射，直接位于正文层的文字不行。

以下情况使映射失败：SVG、`<image>`、仅由背景图构成的页、其他媒体、表格、ruby、pre、`xml:space`、正文中的注释／PI／CDATA、外部命名空间、未知元素，以及需要进一步解析的 CSS 预格式空白、生成内容、转义标识符和未展开的 `@import`。块首纯空白被 CREngine 删除时调整文本兄弟索引；存在影响该判断的 CSS display 声明时拒绝猜测。

拒绝以章节分段为单位：分段中出现一个范围外元素（例如 `<table>`），该分段内的全部位置在上传和下载两个方向都返回 `sync::kUnsupportedStructure`，界面单独提示原因，固件以 WARN 记录原因文本（不含正文、位置和账户）。远端偏移落入已删除空白、元素级位置、非文本位置、无效章索引或不支持的 DOM 时返回错误，不跳到第一页，不按百分比找附近位置。可读不等于可精确同步。

超长文本节点：固定版本的 CREngine 在实体解码和空白处理前按最多 8,192 个源字符分块，分块后的文本兄弟编号可能与原 XHTML 不同。映射加载时累计原始事件长度（UTF-8 按源标量，其他编码用字节数上界），任一正文节点达到 8,192 时，该 XHTML 的全部分段双向拒绝精确映射。该标记只用于映射，不改变正文、锚点、分页或配置。

## 真实引擎对照与复现

`tools/check_koreader_mapping.py` 下载并校验固定 CREngine（commit `f4a8be93255a439db4777d00f0bc53316acabebd`）与 xxHash，构建在 `.local/`，需要本机已有 CMake、C/C++17、pkg-config、FreeType、HarfBuzz、zlib、libpng、libjpeg、OpenSSL，不安装全局依赖。先运行 `python3 tools/setup_fonts.py`：

```sh
python3 tools/check_koreader_mapping.py
```

该程序链接未修改的 CREngine，加载同一份原创合成 EPUB（含 `tests/make_document_fixtures.py` 生成的 `image-mapping.epub`），实际执行 Render、createXPointer、XPointer 序列化与 goToBookmark／SetPos：逐个核对每个可映射标量在引擎节点中的实际字符，抽查反解，并恢复引擎自己的 `getBookmark()` 书签。两个 DOM 版本在 28／38／56 px 下，合成书、CSS 样本和[两个公开鲁迅 EPUB](../../tests/README.md#公开-epub-样本)的可映射字符全部通过；只有图片封面占位的标量不在范围内。这不扩大为所有 EPUB 的结论。

`tests/sync_test.cpp` 用来自该引擎的中文、非 BMP 与跨章位置，覆盖不支持分支、旧上传版本快照、鉴权、无记录、无效响应，以及配置错误不泄露密钥。

`tools/check_koreader_client.py` 用官方 KOReader macOS ARM64 构建（Actions 产物，首次下载需要 `gh` 登录）做本地双向联测：官方客户端经原 KOSync 插件上传，生产 `reader_sync` 反解并生成另一处位置，官方客户端取回后经原 `GotoXPointer` 跳转，核对指针、当前页和连续原文，再重启客户端核对恢复。

```sh
python3 tools/check_koreader_client.py \
  --book .local/public-books/lu-xun-aq-25332.epub \
  --book .local/public-books/lu-xun-nahan-27166.epub
```

默认验证 28、38、56 px，可用 `--font-size` 指定单组；每轮使用独立 `KO_HOME` 和一次性账户，结果保存在 `.local/koreader-client-runs/`。测试服务只监听 `127.0.0.1` 并使用 HTTP，只验证客户端、协议与位置转换的互通，不验证公共服务、设备 Wi-Fi、固件 TLS 或网络中断时的远端状态。

## 外部代码边界与许可

固件的协议和映射为独立实现，没有复制 KOReader／CREngine 源码，也不链接进固件。KOReader 根许可为 AGPLv3，CREngine 根声明为 GPLv2（子目录另有许可）；它们只构建为本地校验程序，不随产品发布。若分发该校验程序，须另行完成 GPL 与外部依赖的分发义务。本地参考依赖 xxHash 0.8.3 为 BSD-2-Clause。
