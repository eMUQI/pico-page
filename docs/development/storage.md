# 文件式存储

用户数据以 TF 卡上的普通文件为权威，配置可读、可改、可备份；没有数据库或隐藏的 NVS 配置。`reader_store`（`firmware/components/reader_store/`）是唯一写入口，公开接口见 `include/pico_page/store.h`：`config.cpp` 负责配置解析、校验、覆盖合成和保注释写回，`store.cpp` 负责文件提交、恢复、书库扫描、伴随记录与缓存访问。同一组件不创建线程，要求单一文件 I/O 执行者，挂载、卸载与 USB 所有权移交由调用者串行协调。测试见 `tests/store_test.cpp`、`tests/store_sync_test.cpp` 及验收的 ST／CFG 系列。

## 1. 目录

固件内部统一使用卡根相对路径，不把 host 路径编入数据。启动时补建缺少的 `Fonts/`、`LockScreens/` 和 `Dictionaries/`，不覆盖已有目录或同名文件，创建失败不阻止阅读。

```text
TF 根目录/
├── Books/                            # 默认书库根目录，可建子文件夹
│   └── 文学/
│       ├── 示例.epub                  # 原书保持原样
│       └── 示例.epub.picopage/        # 完整文件名 + 产品后缀
│           ├── progress.json         # 本地阅读位置
│           ├── bookmarks.json        # 本地书签
│           └── settings.toml         # 本书覆盖项
├── Fonts/                            # 用户字体及许可证
├── LockScreens/                      # 自定义休眠图片
├── Dictionaries/                     # StarDict 词典
└── PicoPage/
    ├── settings.toml                 # 全局配置
    ├── network.toml                  # Wi-Fi 凭据，明文
    ├── sync.toml                     # KOReader 同步账户，明文
    ├── data/                         # 最近书籍、最近列表、上次校时、轮播记录
    └── cache/                        # 可重建数据：covers/、positions/、pages/、lockscreens/、dictionaries/、update-v2.json
```

默认扫描 `Books/`，不存在时由 store 创建；自定义扫描根（`library.roots`，最多 16 项）不自动创建。单一扫描根的内容直接显示在书库首页，多个扫描根时显示各根目录入口。扫描跳过产品数据、伴随目录、临时文件与缓存；目录不可读不等于空书库。移动书籍时必须一并移动同名的 `.picopage/` 目录。

## 2. 数据等级

| 类别 | 规则 | 例子 |
|---|---|---|
| 原始内容 | 用户文件，删除须明确确认 | EPUB、TXT、字体、图片、词典 |
| 用户记录 | 不可重建，不随清缓存删除 | 设置、进度、书签、最近阅读、校时记录、轮播记录 |
| 可重建数据 | 只放在 `PicoPage/cache/`，可随时删除 | 封面、分页、章节、锁屏、词典索引、更新状态 |
| 私密数据 | 单独管理，不进 Git 或诊断导出 | `network.toml`、`sync.toml` |

缓存可以使用二进制格式，但不能保存唯一一份设置或进度。`network.toml`、`sync.toml` 由 `FileStore::save_private()` 替换，不留 `.bak`，避免旧凭据残留。

## 3. 提交与恢复

所有用户记录使用同一协议：

1. 校验记录、路径以及现有主文件与备份的身份和版本。
2. 写临时文件，`fsync` 并关闭，重新读取核对全部内容。
3. 主文件有效时，把它写成新的备份；主文件已损坏而依赖备份恢复时，保留这份备份，不覆盖唯一有效副本。
4. 删除旧主文件，把临时文件重命名为正式文件，再读回核对。
5. 所有步骤成功才返回确认；失败不宣称已保存。

ESP-IDF FatFs 的 `rename` 不覆盖已有目标，因此只能「保留另一份有效记录 → 删除目标 → 重命名」，这不是无间隙的原子替换。主文件缺失或损坏时读取有效 `.bak` 并标明来源；临时文件不参与恢复；遇到不支持的新 schema 或身份不符时返回明确错误，不采用旧备份也不重写新格式。host 对发布目录执行 `fsync`，ESP-IDF FAT VFS 没有目录描述符，只同步文件。断电只承诺恢复最后一份有效的已提交记录，不承诺保存最后一次触摸。

进度只保存已成功呈现的正文位置：页面停留一段时间且没有输入时提交一次，连续翻页只提交最后停留的页；打开工具、返回书库、换书、休眠、USB、同步上传和书籍管理前立即提交。挂载失败不自动格式化；卡移除后停止新 I/O。

## 4. 身份、移动、删除

`progress.json`：普通记录 schema 1，包含 `book_id`、`content_revision`、`writer_id`、`local_revision`（十进制字符串，避免丢失 uint64 精度）和 `anchor`（偏移为 Unicode 标量）；读取上限 16 KiB。同一写入者不能回写更低的修订，不同写入者之间不比较计数。store 不从文件名推断身份，调用者传入已核实的 `book_id`。存储格式有效不代表位置可精确同步，兼容范围见[内容后端](document-runtime.md)和[同步实现](sync-runtime.md)。

清除进度写入 schema 2 的墓碑记录（`state: "cleared"`）：先提交墓碑备份，再提交主记录；主记录损坏时，墓碑备份返回无阅读位置，不恢复旧记录。旧固件遇到 schema 2 会拒绝开书，降级前须知悉。

`bookmarks.json`：schema 1，包含 `book_id`、`content_revision`、`next_sequence` 和 `bookmarks`；每条有 `sequence`、与进度同格式的 `anchor`、`chapter`、`excerpt`，可选 `percent`，以及只在时间可信时成对出现的 `created_at`／`time_source`。序号删除后不复用。提交前比较主文件和 `.bak` 是否与读取时一致。文件损坏、schema 较新或身份不符时不使用、不修改；为当前书第一次加书签时，先把属于其他内容的旧文件改名为 `bookmarks.<旧 book_id 前 8 位>.<n>.json` 再创建新文件。清缓存、保存进度和清除进度都不修改书签。

原文件变化或同名替换时重新验证身份，不按路径和大小沿用旧进度；同内容的多个副本不静默合并记录。删除原书只删除选中的 EPUB 或 TXT，保留伴随目录，不触碰远端。书籍管理确认时捕获原书大小、CRC32 和进度文件内容，提交前再次比较，发现外部修改即拒绝；操作当前书前结束其会话，阻止旧回包恢复已清除的位置。伴随数据不使用 KOReader 的 `.sdr`，不读写 `metadata.epub.lua`。

## 5. 配置

```text
固件默认 → 全局 PicoPage/settings.toml → 本书 settings.toml → 本次面板尚未保存的预览
```

最后一层不持久化。本书文件只写与全局不同的覆盖项，只叠加出现的字段。可用字段、取值范围和默认值以 `config.cpp` 的校验为准（如 `allowed_fields`），用户参考[全局设置示例](../examples/global-settings.toml)和[单书设置示例](../examples/book-settings.toml)。`library.*`、`network.wifi_enabled`、`power.*`、`system.language`（`zh-Hans`、`zh-Hant`、`en`）、`onboarding.*` 和 `dictionary.enabled` 只允许全局设置，单书文件中出现即视为无效。全局文件上限 32 KiB；`/Books` 表示卡根相对路径 `Books`。

读取：缺失文件采用上层值；语法、UTF-8、类型、范围、未知字段或新 schema 错误时整层回退，报告路径、字段或行号，保留原文件，不触发恢复出厂。配置在启动、开书和对应设置页打开时读取，不监视文件变化。

写回：`settings_snapshot()` 捕获两层原始字节与有效值，`apply_settings()` 写入前比较原文，外部修改、创建或删除返回 `ExternalChange`。写回按解析器的源行列定位，只替换变化的单行值，保留字段顺序、空白和注释；新增字段插入对应表；行内表、多行字符串等无法确认范围的写法返回失败。变更后的全文再次校验，然后按第 3 节协议提交。普通读取不自动恢复设置备份，避免有意删除的覆盖项被旧备份重新启用。本书新值等于全局值时删除覆盖；保存全局配置不改写其他书的显式覆盖。不支持电脑与固件同时挂载写入。

新增字段会被旧固件判为未知并整层拒绝；降级前须手动移除，例如 `reader.fullscreen_reading`、`reader.original_epub`、`system.language`、`[dictionary]`。

`[onboarding]` 的 `done`、`in_progress` 由引导流程按上述协议写入，提交失败不在内存中宣称完成；判定规则见[首次开机引导](ui-implementation.md#首次开机引导)。

## 6. 数据文件

- `PicoPage/data/last-book.txt`：上次打开的书的安全卡根相对路径，正文成功显示后写入；读取拒绝路径穿越、控制字符、无效 UTF-8、符号链接和不存在的书籍。
- `PicoPage/data/recent-books.txt`：`PPRECENT1` 版本行之后，每行为 Unix 秒、制表符和路径；最多 128 本，新打开的书移到首位，未校时记作 0。与 `last-book.txt` 各自提交，不组成跨文件事务。只用于书库排序和日期，不是位置权威，也不参与同步。
- `PicoPage/data/last-clock-sync.txt`：单行 UTC Unix 秒，有效范围 2024–2099 年，只用于显示上次校准时间。
- `PicoPage/data/last-lock-screen.txt`：轮播记录，见[自定义锁屏](platform-runtime.md#自定义锁屏)。

## 7. 缓存

所有缓存位于 `PicoPage/cache/`，只由「清理全部缓存」整体删除。`clear_cache()` 只删除该目录的子项，不接触原书、伴随记录、`PicoPage/data` 和配置；调用前必须停止使用缓存的任务，取消可能留下部分已删除的缓存。缓存写入沿用临时文件、同步和重命名，失败不发布部分内容，也不影响本次已准备的结果。读取时文件头、长度或摘要不符即按未命中处理并重建。

- `covers/`：书库缩略图、详情和休眠封面，各尺寸一个文件，文件名为书籍相对路径的 FNV-1a 64 位摘要加尺寸，文件头记录路径、原书大小和修改时间（`PPCOVER4`）。负记录（`NEG2`）区分无可用封面、资源缺失和格式错误，并带原书 SHA-256，读取时重新核对摘要。只有 `NoCover`、`MissingResource`、`InvalidFormat` 可写负记录；I/O、内存、源文件变化、取消和超时只能重试。正缓存无法发现大小和修改时间都不变的原书替换，清缓存后恢复。
- `positions/`：书库显示的章节名和全书百分比（`PPPOS3`），只在位置成功显示并确认保存后写入；不参与恢复阅读位置。
- `pages/`：全书分页（`PPPAGES3`），文件头记录内容版本、原书与字体身份、版心与排版参数；统计完成或离开书籍时写入已完成章节，超过上限不写。只用于页码显示。
- 章节内容缓存与图片缓存：格式见[内容后端](document-runtime.md#html-正文兼容与-txt-编码)和[架构](architecture.md#正文整页图片缓存)。
- `lockscreens/current`：见[自定义锁屏](platform-runtime.md#自定义锁屏)。
- `update-v2.json`：固件更新的最近清单、检查时间、安装记录和跳过的版本；缓存清单只用于显示，下载前须向服务器复核。

### 阅读统计

全局配置的 `[statistics]` 节保存采集开关、最短停留时间和单次上限。书籍副本的身份保存在原书旁的 `.picopage/statistics-id.{0,1}`，汇总保存在 `PicoPage/data/statistics/`。复制一份仍留在原处的书及其伴随目录后，新副本使用独立身份；连同伴随目录移动书籍可保留绑定。统计汇总与正式阅读进度分别保存，删除进度或清理缓存会保留阅读历史。

统计使用位置签名验证单书百分比，签名包含书籍和已保存的进度信息。书库可继续显示语言匹配的旧版位置缓存。新缓存写入当前界面语言及进度签名。

## 8. 多端访问与备份

USB MSC 与固件不能同时挂载同一可写 FAT 卷：先提交并关闭文件、移交所有权，电脑安全弹出后在设备上退出并重启，再挂载和扫描。Wi-Fi 传书只新增文件到 `Books/`、`Fonts/`、`LockScreens/`，不覆盖、不删除；未完成的文件不进入书库。

备份单书：原书加伴随目录，必要时带上所用字体。备份阅读环境：所有扫描目录加 `PicoPage/`，可排除 `cache/`；凭据单独处理。

## 词典配置与可重建索引

全局 `[dictionary].enabled` 是字符串数组，按查词优先级保存最多 32 个不同标识；标识是 `Dictionaries/` 下 `.ifo` 或 `.ifo.gz` 的相对路径，拒绝绝对路径、隐藏目录、路径穿越、控制字符和不支持的扩展名。空数组表示全部停用；引用暂时缺失的词典时保留标识，不自动换成同名词典。数组编辑保留数组内与行尾注释，无法安全编辑的写法返回错误。

索引缓存位于 `PicoPage/cache/dictionaries/<路径摘要>/`：`index.bin`（`PPDIX01`）保存源文件组合 SHA-256、稀疏偏移和载荷摘要；`identity.bin`（`PPDID01`）保存组合摘要及各源文件大小与修改时间，只在完整摘要后写入；`.idx.gz` 流式展开为 `index.raw`，完整 CRC、长度和摘要校验通过后才发布，原始 `.idx` 不复制，`.dict.dz` 按块读取。

每个挂载作用域首次使用词典时，若源文件大小与修改时间与 `identity.bin` 一致则沿用其摘要（与 sdcv 判断偏移缓存的做法相同，不是内容校验），否则重新计算完整摘要；明确重试会对已记录失败的来源重新完整摘要。USB 交还、换卡、重新扫描或外部替换时清除 `dictionary::Verification`，它不持有句柄，不是跨挂载的权威记录。`FileStore::dictionary_files` 提供有界目录发现，达到上限时保留部分结果并标记未扫描完整，取消或 I/O 失败时清空结果；`dictionary_path` 校验配套文件路径并拒绝符号链接。
