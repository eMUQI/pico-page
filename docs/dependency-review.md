# 第三方依赖审查表

状态：2026-10-06 已核查并锁定实际使用的 BSP、TOML/JSON、ZIP/XML、FreeType 和字体；详见各组件记录。项目总许可证待决定，本表不是整项目发布许可结论。

| 候选/用途 | 路径/来源 | commit | 许可证/依赖审查 | 本地修改 | 当前处置 |
|---|---|---|---|---|---|
| Read Pico 官方 BSP/PMU/驱动 | MindReset/read_pico_firmware | `5f3406deec33304de8801046db583dec7c49e55b` | 八个 Apache-2.0 组件；epdiy 为 LGPL-3.0-or-later；详见下方审查 | 新增 checked SD release；补丁应用到构建副本 | 下载到忽略目录 `.local/`，编译引用九个组件 |
| Expat / miniz | CrossPoint 的独立第三方目录 | `50823ffa125b437b4c9fe5882f326458780198da` | 各源文件 MIT；siphash CC0；保留全部声明 | Expat 配置及 entropy 守卫移植 | 纳入 reader_document；未复制 CrossPoint 应用 |
| PapyriX cache/content | bigbag/papyrix-reader | 待查 | 查分支关系与依赖 | 无 | 按需参考 |
| EPub-InkPlate | turgu1/EPub-InkPlate | 待查 | 查字体/IDF/显示依赖 | 无 | 调度和 IDF 实现参考 |
| epdiy 与板级波形 | 官方 BSP 的 `components/epdiy` / `e0470_epaper_waveform` | 同官方 BSP commit | LGPL-3.0-or-later / Apache-2.0 分别核查 | 构建副本应用 epdiy 欠载诊断及清屏检查补丁 | 已集成；部分设备证据见开发状态，完整显示验收仍需实测 |
| tomlc17 | cktan/tomlc17 | `8a4564e0b257b010c24c973d4353e0c1ebf33f80` | MIT；C17；32 KiB 输入上限，UTF-8 校验；不支持无损写回 | Xtensa `PRIx32` 打印格式修正；内存页大小使用 `offsetof`，避免空指针成员访问 | 纳入 reader_store，仅解析配置 |
| cJSON | DaveGamble/cJSON v1.7.19 | `c859b25da02955fef659d658b8f324b5cde87be3` | MIT；16 KiB 输入及 16 层嵌套上限；大修订号用字符串 | 源码无修改 | 纳入 reader_store，进度 JSON |
| FreeType | freetype/freetype | `526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b` | FTL、MIT 及 Adobe CFF 附加条款 | 模块/选项配置 | 固定下载到 .local，纳入 reader_graphics |
| JPEGDEC 渐进式缩略图 | bitbank2/JPEGDEC；CrossPoint 的三个修复补丁 | `86282979224c8a32fd51e091ed5a35b0c699a52b`；补丁源 `43694c9018d26446ab30cce2fa65ee608355ae50` | Apache-2.0；补丁保留来源 MIT 声明；不引入 Arduino 或其他阅读器应用代码 | 修复未对齐读取及未定义移位，禁用 SIMD，保持位读取器宽度 | 纳入 reader_graphics，仅渐进式 DC 预览；[来源、补丁与分发义务](../firmware/components/reader_graphics/third_party/jpegdec/README.md) |
| 文楷 / Noto Sans SC | 官方发布与官方仓库 | 固定 SHA-256，见 graphics-runtime | OFL 1.1；随附两份许可证 | Noto 子集改名 Pico Page Fallback | 全字体在 .local，内置子集纳入组件 |
| Unihan `kTGH`（《通用规范汉字表》一级字） | unicode.org Unicode 17.0.0 `Unihan.zip` | SHA-256 `f7a48b2b…4b5e`，见 graphics-runtime | Unicode License v3；只取 3,500 个字符的清单 | 按序号提取为文本 | 清单纳入 reader_graphics，决定内置字体的字符范围 |
| 内置中文子集的做法 | 0x1abin/crossmux（本地检出） | `3fb809f6ceeb753a7c40fef86dee7c9d40eabdf3` | 只参考做法，未复制代码、字表或字体 | 无 | CrossMux 内嵌 8/10/12pt 点阵，覆盖约 3,500 个高频字；本项目内嵌一份矢量子集 |
| GB18030 解码表 | `tools/generate_gb18030_table.py` 查询 CPython 标准库的 `gb18030` 编解码器 | CPython 3.14.7（Homebrew 构建，构建信息不含源码 commit）；生成结果 SHA-256 `5efc9c50…8bbb` | CPython 为 PSF-2.0；只取编解码器的输出值，未复制 CPython 源码或其表格文本；详见下方 | 双字节区压缩为 23,940 项数组，四字节区合并为 207 个连续区间 | 生成结果纳入 reader_document；发布义务待项目许可证确定时一并确认 |
| CREngine 对照工具 | koreader/crengine | `f4a8be93255a439db4777d00f0bc53316acabebd` | GPL-2.0；链接工具仅用于本地验证 | 无引擎源码修改 | 不纳入固件/host 阅读程序；映射工具与来源记录单列 |
| SDL2 / OpenSSL / libcurl | 本机现有开发库 | 构建日志记录已解析版本 | zlib / Apache-2.0 / curl license，各自动态链接 | 无 | 仅 host；未复制库源码 |
| esp_tinyusb / TinyUSB | Espressif Component Registry | 2.4.0 / 0.21.0~1；component hash 见 dependencies.lock | Apache-2.0 / MIT；已读组件 LICENSE；依赖 ESP-IDF | 不修改上游文件；构建排除便利存储层，接入同步 SDMMC 回调 | 仅开启 MSC；分发时保留许可证、版权和适用 NOTICE |

USB 实现对固定 Read Pico BSP 增加了带错误返回的卸载接口：补丁位于 `firmware/patches/read-pico-sd-release.patch`，仅应用到构建目录副本，原始检出保持不变。该适配属于 Apache-2.0 的 Read Pico 组件，不修改 epdiy、校准或波形组件。`esp_tinyusb 2.4.0` 的便利存储层采用延后写入，实际写失败仅记日志；因此本项目保留同步回调。官方 `auto_mount_off` 已可禁止自动回挂，自动回挂并非必须替换便利层的理由。官方示例、固定提交及逐项核对见 [USB 运行记录](usb-storage-runtime.md)。

GB18030 解码表的来源与复核（2026-10-07）：`firmware/components/reader_document/gb18030_data.inc` 由 `tools/generate_gb18030_table.py` 生成。脚本逐个解码全部双字节和四字节序列，记录 CPython `gb18030` 编解码器返回的码位，不读取 CPython 的源码或数据文件。用 CPython 3.14.7 重新运行，输出与仓库中的文件逐字节相同，SHA-256 为 `5efc9c505062b83e0a80f04547e80d0c400b8d2ff6397cd86ee02f3993d68bbb`。本机的 Homebrew 构建没有记录源码 commit，只能固定到发布版本号。抽查的码位显示该编解码器采用 GB 18030-2000 的映射：`A8BC` 解码为 U+E7C7，`8135F437` 解码为 U+1E3F，`FE59` 解码为 U+E81E。GB 18030-2005 交换了前两者的映射；2005 和 2022 两次修订的其余调整没有逐项核对，受影响的字符在设备上会显示为私用区码位或不同的字。许可方面：CPython 采用 PSF-2.0。本项目收录的是字符编码与 Unicode 码位的对应数值，不含 CPython 的代码；是否需要随发布附带 PSF 许可声明，与项目总许可证一并由项目所有者确认，本表不作结论。

实际版本、来源文件、传递依赖及义务分别记录在：

- [BSP 审查](bsp-review.md)：九个组件 tree SHA、SDK 许可例外、校准与 SD 副作用、发布待办。
- [解析器来源](../firmware/components/reader_store/third_party/README.md)：复用文件、随附许可证、输入限制和唯一移植补丁。
- [原生内容依赖](../firmware/components/reader_document/third_party/README.md)：复制来源、逐文件摘要、独立许可和移植范围。
- [字体与渲染](graphics-runtime.md)：固定来源、FTL 和 OFL 义务、子集生成。
- [同步实现与引擎对照](sync-runtime.md)：真实引擎工具的构建和使用边界。
- [EPUB/KOSync 探针](epub-kosync-probe.md)：CrossPoint、KOReader、base、CREngine 固定版本与使用边界。

本机全新 host 编译会产生 cJSON 上游 `sprintf` 的 macOS 弃用警告；未修改上游源码掩盖警告。ASan/UBSan 检查无运行时错误，不能据此代替上游安全维护或最终分发审核。

每项应记录：仓库 URL、实际下载路径、完整 commit、许可证文件路径、传递依赖、移植补丁、允许分发方式、尚未确认的问题。不能将思想借鉴、源码复制、静态链接、配套工具调用视为完全相同的义务。许可范围不明时先实现无关的自主核心与测试，隔离实验；不要因此伪造「已审核」，也不必停掉全部开发。发布前完成法律/许可核对，本表不提供针对具体许可的法律结论。

## 2026-10-06 · 功能实现前的来源核对

新增能力先核对乐鑫官方示例，再检查 GitHub 已有实现及其适配边界；实际采用前记录版本、源码、依赖和许可证。本轮没有新增第三方依赖或复制上游应用代码。

- 文件操作：核对本机 ESP-IDF `fff9895c82d744c7237be8847347bdd1b07c6643`（v6.1）的 [SDMMC 示例](https://github.com/espressif/esp-idf/blob/v6.1/examples/storage/sd_card/sdmmc/main/sd_card_example_main.c)和 `components/fatfs/vfs/vfs_fat.c`。示例文件头为 Public Domain / CC0；示例通过 `stat`、`unlink`、`rename` 替换文件，VFS 的 `fsync` 调用 `f_sync`。它不提供本项目的进度主记录/备份恢复协议，因此沿用 FileStore 的比较、同步和回读提交机制。
- 同步：重新查看 [KOReader KOSync 插件](https://github.com/koreader/koreader/blob/master/plugins/kosync.koplugin/main.lua)和 [官方同步服务器](https://github.com/koreader/koreader-sync-server/blob/master/README.md)。本机已有 KOReader 客户端源码，检查了位置接续、设备字段和时间戳判断路径。本轮只核对协议与行为，没有复制或链接其 AGPL 应用代码；此前固定提交与许可证见 [EPUB/KOSync 探针](epub-kosync-probe.md)。本项目仍要求明确选择远端位置或确认上传；API 未提供的时间和设备在线状态不作推断。
- CrossPoint 的固定源码和耦合审查已见同一探针记录。本轮沿用现有精确 XPointer 映射，不因上游具备类似功能就引入整套 Arduino 阅读器或用百分比代替源位置。

## 2026-10-06 · 交互耗时修复前的来源核对

针对 [开发状态](development-status.md) 中「交互耗时排查」列出的问题，先查官方资料与已有实现。本轮只读源码和文档，没有复制代码、新增依赖或修改固件。

| 来源 | 版本 | 许可证 | 查看范围 |
|---|---|---|---|
| [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) | `7394e40fa97f61007f61458ed8d94e0dd378c323` | MIT（顶层） | 与 CrossMux 同结构，本轮以 CrossMux 为准 |
| [0x1abin/crossmux](https://github.com/0x1abin/crossmux) | `1512e8e049f2b1a4efc7c004a5b1a93cbc5e3f9f` | MIT（顶层） | 封面缓存、章节分页缓存、加载提示、缓存目录、Read Pico 记录 |
| [0x1abin/freeink-sdk](https://github.com/0x1abin/freeink-sdk) | `96de1be6ce08eb732909e6e8149af8f892b9a2c5` | MIT（顶层） | FreeType 字体后端的取字宽与 hinting 选项 |
| [KOReader coverbrowser](https://github.com/koreader/koreader/blob/master/plugins/coverbrowser.koplugin/bookinfomanager.lua) | master，2026-10-06 查看 | AGPL-3.0，仅参考行为 | 封面缓存与失效判断 |
| [ESP-IDF 速度优化指南](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/performance/speed.html)、[FatFs 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/fatfs.html) | stable，2026-10-06 查看 | 文档 | stdio 缓冲、fast seek、读写粒度 |
| [espressif/esp_jpeg](https://components.espressif.com/components/espressif/esp_jpeg) | 1.3.1（未引入） | 未核对，采用前再读 | JPEG 解码缩放能力 |

可采用的做法：

- 封面：CrossMux 把缩略图写成每本书缓存目录下的文件，只生成一次；空文件表示「没有可用封面」，不再重试。KOReader 把缩放后的缩略图存入缓存库，用文件大小和修改时间判断失效，并记录失败次数。两者都只缓存缩放后的结果。本项目的 `PicoPage/cache` 已由「清理全部缓存」覆盖，可放同类文件；失效判断沿用现有内存缓存的文件大小和修改时间。
- 加载提示：CrossMux 只在预计耗时较长时才画提示（章节原文超过 96 KiB 且需解压，或目标页超过 20 页），提示画在当前画面上并立即刷新，避免短操作多闪一次。封面批量生成时先显示提示，完成后只做一次最终刷新。
- 分页：CrossMux 按章节把分页结果写到 TF 卡，文件头含字体、行距、视口等排版参数；支持只排到目标页的部分缓存，之后再续排。本项目暂不引入磁盘分页缓存；先采用「只排到需要的位置并记住途中页起点」。
- 字宽：freeink-sdk 的说明指出 `FT_LOAD_NO_HINTING` 会把字宽由取整变为截断，从而改变分页，不能用它换速度。它为 TF 卡字体保留持久的字宽表，并按页预热字形。本项目的 FreeType 未编入自动 hinter，因此不存在自动 hinting 开销。
- TF 读取：官方指南说明 stdio 默认缓冲为 128 字节，建议用 `setvbuf` 或 `CONFIG_FATFS_VFS_FSTAT_BLKSIZE` 提高到 4096 字节以上，或直接使用 `read`。本项目的 FreeType 默认流使用 `fseek`/`fread`，属于受影响路径；需真机计时确认收益。
- TF 总线：CrossMux 记录 Read Pico 为 1-bit SDMMC、40 MHz。本项目实测顺序读约 3.0 MiB/s，属于该接法的正常范围，不作为优化对象。
- JPEG：`esp_jpeg` 与本项目已有的 tjpgd 同源，同样只支持 1/2、1/4、1/8 缩放和基线 JPEG；更换组件不能解决 PNG 或渐进式 JPEG 的耗时。

不采用：CrossMux 用路径哈希作为缓存键、把非活动 OTA 分区用作字体缓存、预处理字体格式。前者与本项目以内容摘要识别书籍的进度规则不同；后两者涉及分区和新文件格式，超出本轮范围。

实现结果：封面缓存、状态行和页起点记录均为本项目自行实现，没有复制上述项目的代码。许可证义务没有变化。

## 2026-10-06 · 配网与同步配置的来源核对

| 来源 | 版本 | 许可证 | 查看范围与取舍 |
|---|---|---|---|
| ESP-IDF 示例 `examples/protocols/http_server/captive_portal` | `fff9895c82d744c7237be8847347bdd1b07c6643`（v6.1） | Unlicense OR CC0-1.0 | 采用其做法：DNS 把所有主机名指向热点地址，未知路径 303 跳转并带正文（iOS 需要正文）。没有复制 `dns_server.c`：它的停止路径直接删除任务，不关闭套接字，每次会话泄漏一个；本项目每次配置都要启停，因此按同一协议另写了约 40 行的应答任务。DHCP 114 选项未采用 |
| ESP-IDF `esp_http_server`、`esp_wifi`（APSTA、扫描）、`esp_netif` | 同上 | Apache-2.0 | 作为 SDK 组件使用 |
| `espressif/idf-extra-components` `qrcode/qrcodegen.c`、`qrcodegen.h` | `3117b1e6806738c0271bbd18df17f6d74ec66452`（组件 `espressif/qrcode` 0.2.0） | MIT（Project Nayuki，文件头） | 复制到 `reader_graphics/third_party/qrcodegen/`，未修改，host 与固件共用；组件的 Apache-2.0 封装文件未复制。哈希见该目录的 `manifest.json` |
| CrossMux（本机检出） `src/activities/network/WifiSelectionActivity.cpp`、`CrossPointWebServerActivity.cpp`、`src/activities/settings/KOReaderSettingsActivity.cpp`、`lib/KOReaderSync/KOReaderCredentialStore.cpp` | `3fb809f6ceeb753a7c40fef86dee7c9d40eabdf3` | MIT（顶层） | 只读。它用屏幕键盘输入 Wi-Fi 密码和 KOReader 账户，热点模式下同样运行 DNS 重定向；凭据存 TF 卡。未复制代码；屏幕键盘留作以后的后备 |
| KOReader `plugins/kosync.koplugin/api.json`、`KOSyncClient.lua` | `2ce49e115fc1bcc47b473078c549e59b3922105b` | AGPL-3.0 | 只读协议：`GET /users/auth` 返回 200 表示账户有效、401 表示拒绝；userkey 为密码的 MD5。未复制代码 |

- SmartConfig（ESPTouch/AirKiss）未采用：只能传 SSID 和密码，需要专用 App 或微信，且要求手机连在 2.4 GHz 上。
- 乐鑫统一配网（BLE 或 SoftAP 加专用 App）未采用：同样需要专用 App，BLE 还要加入蓝牙协议栈。

## 2026-10-06 · 全书页数与设置页的来源核对

本轮只读源码和文档，没有复制代码或新增第三方依赖。

| 来源 | 版本 | 许可证 | 查看范围 |
|---|---|---|---|
| [turgu1/EPub-InkPlate](https://github.com/turgu1/EPub-InkPlate) `src/models/page_locs.hpp`、`page_locs.cpp` | master，2026-10-06 查看 | MIT（源文件头） | 全书页位置的后台计算、缓存文件和完成前的显示 |
| CrossMux（本机检出，远端 `eMUQI/crossmux`） | `3fb809f6ceeb753a7c40fef86dee7c9d40eabdf3` | MIT（顶层） | 状态栏的章内页数与全书百分比、设置页取存储容量 |
| ESP-IDF | `fff9895c82d744c7237be8847347bdd1b07c6643`（v6.1） | Apache-2.0 | `esp_vfs_fat_info`、`esp_app_get_description`；官方示例 `examples/storage/fatfs/ext_flash` |

- EPub-InkPlate 用独立任务排完整本书，页位置连同字号、字体、方向等参数存为原书旁的 `.locs` 文件，参数不符时重算；计算完成前界面显示已完成的章节百分比，完成后才显示页码和总页数。前台需要某一章时发消息让后台优先处理。
- CrossMux 与 CrossPoint 只统计章内页数，未排完的章节按已消耗字节外推；全书百分比按各章节在 EPUB 中的累计大小计算，没有全书页数。
- KOReader 的 crengine 在开书时排完整本书，页码始终精确；本项目的「当前页优先」规则不允许开书时整书排版。
- 采用：空闲时统计全书分页、结果按排版参数缓存、完成前显示估算百分比（按章节大小加权）。
- 不采用：独立后台任务。本项目的文档、字体和会话由应用任务独占，改为在应用任务的空闲时间分片执行；也不把缓存放在原书旁，沿用 `PicoPage/cache`。
- 资源审计时补看 CrossMux 检出中沿用 CrossPoint 的工程规则（`docs/engineering/memory-and-allocation.md`、`hardware-constraints.md`，MIT）和章节解析器接口 `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`：关闭异常时裸 `new` 失败即 abort，大分配须先检查可用总量和最大连续块并保留余量（`memory::hasAllocationHeadroom`）；追加循环前先 `reserve`；章节以可续接的步骤流式解析，页面生成后立即写入 TF 卡，内存中不保留整章。本项目采用其中的余量检查和预留做法；统计仍在内存中解析整章，但改为有上界的精简解析。只读规则与接口，未复制代码。技术参考以 CrossPoint 和 KOReader 为主，采用前仍需核对具体源码、约束和许可。
- 容量与版本使用乐鑫官方接口：`esp_vfs_fat_info` 读取 FAT 卷的总容量和可用空间，`esp_app_get_description` 读取固件版本。ESP-IDF 的 FatFs 配置 `FF_FS_NOFSINFO` 为 0，FAT32 的可用簇数优先取自 FSInfo 扇区。host 使用 `statvfs`。`reader_store` 新增对 `fatfs` 组件的依赖，`reader_app` 新增对 `esp_app_format` 的依赖，均为 ESP-IDF 自带组件。

- `CONFIG_FATFS_VFS_FSTAT_BLKSIZE=4096` 经真机对照后未采用：顺序读没有可见收益，字形的随机小块读取变慢，新页准备时间增加约六成。官方建议针对顺序吞吐，不适用于本项目的字体访问方式。
- 字宽改用 FreeType 自带的 `FT_Get_Advance`（[接口说明](https://freetype.org/freetype2/docs/reference/ft2-quick_advance.html)），属于已固定版本 FreeType 的公开接口，没有新增依赖。与 freeink-sdk 的结论一致：不使用 `FT_LOAD_NO_HINTING`，而是复现带 hinting 加载时的取整结果，并用全字符比对确认一致。


## 2026-10-11 · 文字转页与水波纹候选

文字转页来源：[FreeInk SDK](https://github.com/0x1abin/freeink-sdk/tree/96de1be6ce08eb732909e6e8149af8f892b9a2c5)，完整 commit `96de1be6ce08eb732909e6e8149af8f892b9a2c5`，文件 `libs/display/EpdiyLcd/src/e0470/e0470_epaper_waveform.c`。移植前读取根 `LICENSE`（MIT、2026 FreeInk）、同目录 `LICENSE` 与源码头（2026 mindreset、Apache-2.0）；适用的波形文件许可为 Apache-2.0，不能用根 MIT 替代。仅移植左对齐、对角线保持、深色目标前导擦白置零三步至 `platform_read_pico/changed_pixels_waveform.cpp`。修改为不依赖 ESP-IDF 的纯函数、独立布尔选项、容量与重叠检查，删除临时序列数组；beta.6 移除文字转页后仅保留对角线保持及边界检查，删除左对齐、前导擦白处理和布尔选项。不引入 FreeInk 波形数据或其他代码。保留版权/SPDX 与修改声明，组件原 `LICENSE` 署名保留，增加完整 `Apache-2.0.txt`。

参考中文注释反对清对角线，但代码实际清除，对应本项目 B/C；中文注释中的抗锯齿残影风险仍未解决。C 将前导动作 2 置为 0，不再次压缩或左移后续推黑，也不把动作 3 改成推黑；`to != 15` 涵盖 1–14 级灰色，并非只有纯黑，最终亮度必须真机对比。A 提供保守对照，三步也分别受合成 LUT 测试覆盖。

水波纹来源：[wegooo-cell/read-pico-reader（kiikoread）](https://github.com/wegooo-cell/read-pico-reader/tree/8d6f1a6f2f5cbee5bba08df43ebb5001d2584d83)，完整 commit `8d6f1a6f2f5cbee5bba08df43ebb5001d2584d83`，`components/e0470_page_turn/e0470_page_turn.c` 和公开头文件；根 `LICENSE` 为 Apache-2.0，组件文件署名 2026 mindreset、Apache-2.0。读取 `components/epdiy/LICENSE`：该子组件仍为 LGPL-3.0-or-later。移植条带逐拍调度至 `platform_read_pico/ripple.cpp`，仅保留倒置竖屏全屏从右向左的 16 条带路径，PSRAM 表精确为 37 KiB；移除多方向、区域裁剪、40 相预留、FAST 扫描和软件忙等，不复制其应用界面、配置或波形表。

驱动参考范围为该 commit 的 `components/epdiy/src/render.c`、`output_common/render_context.h`、`output_lcd/render_lcd.c`：仅适配逐行相位 LUT 借用接口，增加相位范围与单相差分模式检查、同步返回后的指针清理。补丁 `firmware/patches/epdiy-row-phases.patch` 应用到构建目录的 epdiy 副本，沿用 LGPL-3.0-or-later 义务与原文件署名，不将其标成 Apache。原欠载汇合补丁在前，BSP 锁定 `5f3406deec33304de8801046db583dec7c49e55b` 不变，`.local/read_pico_firmware` 无源码修改。依赖仍为现有 epdiy、E0470 波形组件、ESP-IDF 能力堆和应用显示所有者，没有新增 SDK。源码及补丁的公开分发应保留相应许可、署名和可对应的修改源码；项目总许可证未因此改变。

## 2026-10-10 · 左右轻扫规则参考（2026-10-11 完成复核）

本批只借鉴规则，分类及派发代码独立实现，未复制第三方代码、未引入依赖。屏幕布局、默认按键与确认语义沿用 Pico Page。

| 来源 | 版本与许可 | 参考范围与取舍 |
|---|---|---|
| [CrossMux](https://github.com/0x1abin/crossmux/tree/3fb809f6ceeb753a7c40fef86dee7c9d40eabdf3) | `3fb809f`，MIT；本地源码与 LICENSE 已读 | `src/MappedInputManager.cpp`、`docs/contributing/touch-and-ui.md`：轻扫不激活下方条目，方向经集中入口派发；不采用其边缘返回/主页手势 |
| [FreeInk SDK InputManager](https://github.com/0x1abin/freeink-sdk/tree/e8bfe8c/libs/hardware/InputManager) | `e8bfe8c`，MIT；源码、头文件及 LICENSE 已读 | `TOUCH_SWIPE_MIN_PX=60`、`TOUCH_SWIPE_MAX_MS=700`、`TOUCH_TAP_SLOP_PX=28`；按下点作为点按目标。采用规则，不移植采样器或 SDK |
| [KOReader 手势识别](https://github.com/koreader/koreader/blob/master/frontend/device/gesturedetector.lua)、[菜单方向](https://github.com/koreader/koreader/blob/master/frontend/ui/widget/menu.lua) | master，2026-10-10 在线读取，AGPL-3.0；只参考行为 | `gesturedetector.lua` 的约 30°–60° 对角带和超时不认定轻扫；`menu.lua` 左滑下一页。Pico Page 的慢拖直接忽略，不采用 KOReader 的对角全刷、循环或纵向关闭 |
| Espressif [esp_lcd_touch](https://components.espressif.com/components/espressif/esp_lcd_touch)、[esp_lvgl_port](https://components.espressif.com/components/espressif/esp_lvgl_port)、[touch_slider_sensor](https://docs.espressif.com/projects/esp-iot-solution/en/latest/touch/touch_slider_sensor.html) | 官方组件/文档，2026-10-11 复核 | 前者提供坐标读取；LVGL port 的输入依赖 LVGL；slider 面向电容触摸通道。在这三项中没有适合当前非 LVGL 坐标事件路径直接复用的轻扫实现，因此不增加依赖 |

## 2026-10-12 · 网站安装页的浏览器依赖

网站安装页（`web/src/pages/install.astro`）在访问者的浏览器中运行两个直接依赖及其传递依赖，均为 npm 发布的原包，源码未修改，由 Astro 打包进网站产物。它们不进入固件和 host 程序。

| 包 | 版本（`web/package-lock.json`） | 许可证 | 用途 |
|---|---|---|---|
| [esptool-js](https://github.com/espressif/esptool-js) | 0.7.0 | Apache-2.0；npm 包内只有 `LICENSE`，没有 NOTICE 文件 | 通过 Web Serial 执行 ESP 串口烧录协议，包内带各芯片的烧录 stub |
| [pako](https://github.com/nodeca/pako) | 2.2.0 | MIT AND Zlib | esptool-js 的依赖，压缩写入的数据 |
| [atob-lite](https://github.com/hughsk/atob-lite) | 2.0.0 | MIT | esptool-js 的依赖，解码 stub |
| [spark-md5](https://github.com/satazor/js-spark-md5) | 3.0.2 | WTFPL OR MIT | 计算镜像的 MD5，与闪存内容比对 |

分发方式：四份许可证全文合并为 `web/public/assets/third-party-licenses.txt`，随网站发布，安装页页脚提供链接；清单同时记录在[网站说明](../web/README.md)的「浏览器依赖的许可」。升级这些包时同步更新许可证文件和两处清单。尚未确认：打包产物中是否保留了各源文件的版权注释；esptool-js 内置 stub 的上游来源和许可只按该包的 `LICENSE` 处理，没有单独追溯。

## 2026-10-11 · PMU RTC 接入

RTC 沿用已锁定的 MindReset BSP `5f3406deec33304de8801046db583dec7c49e55b`，参考该提交的 PMU 演示、自检、驱动及协议文档；不新增第三方依赖。`read_pico_pmu`（Apache-2.0）构建副本新增 TIME_GET 响应长度与快照有效性校验，原件不变，补丁随源码保留；源码版权与许可证继续保留。实现和修改范围见 [BSP 审查](bsp-review.md#rtc-响应校验与接入2026-10-11)。未复制 PMU 工厂标定、自检时间来源或额外驱动。


## 2026-10-19 · COS 部署与封禁候选的云端依赖

仅用于云函数候选包及 GitHub Actions 上传步骤，不进入固件、host 或浏览器。版本固定于 `tools/cos-requirements.txt`，来自 PyPI 官方发行包；已读取 SDK 的请求、凭据、上传头映射和本次接口实现及发行包许可元数据。发行包没有提供可直接确认的上游源码 commit，不将 master 当作版本证据；当前记录以发行版本及本地完整函数包 SHA-256 为准，尚未云端部署。

| 包 | 版本 | 许可 | 处理 |
|---|---|---|---|
| 腾讯云 COS Python SDK | 1.9.44 | MIT | 上游代码不改；HTTPS 验证开启；调用方增加禁止覆盖上传头 |
| tencentcloud-sdk-python-common / monitor | 各 3.1.188 | Apache-2.0 | 上游代码不改；只包含本次 SDK 公共部分和监控模块 |
| certifi | 2026.7.22 | MPL-2.0 | 保留证书数据、源码及许可，包内源码未修改 |
| charset-normalizer / crcmod / six / urllib3 / xmltodict | 3.5.2 / 1.7 / 1.17.0 / 2.8.0 / 1.0.4 | MIT | 保留声明；crcmod 仅选用上游支持的纯 Python 实现及源包元数据 |
| idna | 3.20 | BSD-3-Clause | 保留版权、条件和免责声明 |
| pycryptodome | 4.0.1 | BSD-2-Clause AND Unlicense | 保留各组成部分声明，使用官方 manylinux x86-64 扩展 |
| requests | 2.34.2 | Apache-2.0 | 上游代码不改，保留许可和元数据 |

函数包保留各依赖的 LICENSE/声明及元数据。crcmod 的纯 Python 代码来自同版本上游发行包，保留其 LICENSE 和 PKG-INFO，不附带本机编译的 macOS 扩展或错误的平台轮子元数据；其上游 12 项测试已在此纯 Python 包上通过。依赖没有复制进仓库，完整 ZIP 及来源证据位于忽略目录 `.local/cos-protection-20261018/`；云端导入、原生库兼容及真实接口权限仍待验证。本记录不替代整项目许可审查。
