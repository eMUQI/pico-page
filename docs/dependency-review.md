# 第三方依赖审查表

状态：2026-10-06 已核查并锁定实际使用的 BSP、TOML/JSON、ZIP/XML、FreeType 和字体；详见各组件记录。
项目总许可证待决定，本表不是整项目发布许可结论。

| 候选/用途 | 路径/来源 | commit | 许可证/依赖审查 | 本地修改 | 当前处置 |
|---|---|---|---|---|---|
| Read Pico 官方 BSP/PMU/驱动 | MindReset/read_pico_firmware | `28cde682a4468a581c278761922724f57d976418` | 八个 Apache-2.0 组件；epdiy 为 LGPL-3.0-or-later；详见下方审查 | 新增 checked SD release；补丁应用到构建副本 | 下载到忽略目录 `.local/`，编译引用九个组件 |
| Expat / miniz | CrossPoint 的独立第三方目录 | `50823ffa125b437b4c9fe5882f326458780198da` | 各源文件 MIT；siphash CC0；保留全部声明 | Expat 配置及 entropy 守卫移植 | 纳入 reader_document；未复制 CrossPoint 应用 |
| PapyriX cache/content | bigbag/papyrix-reader | 待查 | 查分支关系与依赖 | 无 | 按需参考 |
| EPub-InkPlate | turgu1/EPub-InkPlate | 待查 | 查字体/IDF/显示依赖 | 无 | 调度和 IDF 实现参考 |
| epdiy 与板级波形 | 官方 BSP 的 `components/epdiy` / `e0470_epaper_waveform` | 同官方 BSP commit | LGPL-3.0-or-later / Apache-2.0 分别核查 | 无 | 已编译；硬件刷新未验证 |
| tomlc17 | cktan/tomlc17 | `8a4564e0b257b010c24c973d4353e0c1ebf33f80` | MIT；C17；32 KiB 输入上限，UTF-8 校验；不支持无损写回 | Xtensa `PRIx32` 打印格式修正 | 纳入 reader_store，仅解析配置 |
| cJSON | DaveGamble/cJSON v1.7.19 | `c859b25da02955fef659d658b8f324b5cde87be3` | MIT；16 KiB 输入及 16 层嵌套上限；大修订号用字符串 | 源码无修改 | 纳入 reader_store，进度 JSON |
| FreeType | freetype/freetype | `526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b` | FTL、MIT 及 Adobe CFF 附加条款 | 模块/选项配置 | 固定下载到 .local，纳入 reader_graphics |
| 文楷 / Noto Sans SC | 官方发布与官方仓库 | 固定 SHA-256，见 graphics-runtime | OFL 1.1；随附两份许可证 | Noto UI 子集改名 Pico Page Fallback | 全字体在 .local，内置子集纳入组件 |
| CREngine 对照工具 | koreader/crengine | `f4a8be93255a439db4777d00f0bc53316acabebd` | GPL-2.0；链接工具仅用于本地验证 | 无引擎源码修改 | 不纳入固件/host 阅读程序；映射工具与来源记录单列 |
| SDL2 / OpenSSL / libcurl | 本机现有开发库 | 构建日志记录已解析版本 | zlib / Apache-2.0 / curl license，各自动态链接 | 无 | 仅 host；未复制库源码 |
| esp_tinyusb / TinyUSB | Espressif Component Registry | 2.4.0 / 0.21.0~1；component hash 见 dependencies.lock | Apache-2.0 / MIT；已读组件 LICENSE；依赖 ESP-IDF | 不修改上游文件；构建排除便利存储层，接入同步 SDMMC 回调 | 仅开启 MSC；分发时保留许可证、版权和适用 NOTICE |

USB 实现对固定 Read Pico BSP 增加了带错误返回的卸载接口：
补丁位于 `firmware/patches/read-pico-sd-release.patch`，仅应用到构建目录副本，原始检出保持不变。
该适配属于 Apache-2.0 的 Read Pico 组件，不修改 epdiy、校准或波形组件。
`esp_tinyusb 2.4.0` 的便利存储层采用延后写入，实际写失败仅记日志；因此本项目保留同步回调。官方 `auto_mount_off` 已可禁止自动回挂，自动回挂并非必须替换便利层的理由。官方示例、固定提交及逐项核对见 [USB 运行记录](usb-storage-runtime.md)。

实际版本、来源文件、传递依赖及义务分别记录在：

- [BSP 审查](bsp-review.md)：九个组件 tree SHA、SDK 许可例外、校准与 SD 副作用、发布待办。
- [解析器来源](../firmware/components/reader_store/third_party/README.md)：复用文件、随附许可证、输入限制和唯一移植补丁。
- [原生内容依赖](../firmware/components/reader_document/third_party/README.md)：复制来源、逐文件摘要、独立许可和移植范围。
- [字体与渲染](graphics-runtime.md)：固定来源、FTL 和 OFL 义务、子集生成。
- [同步实现与引擎对照](sync-runtime.md)：真实引擎工具的构建和使用边界。
- [EPUB/KOSync 探针](epub-kosync-probe.md)：CrossPoint、KOReader、base、CREngine 固定版本与使用边界。

本机全新 host 编译会产生 cJSON 上游 `sprintf` 的 macOS 弃用警告；未修改上游源码掩盖警告。
ASan/UBSan 检查无运行时错误，不能据此代替上游安全维护或最终分发审核。

每项应记录：仓库 URL、实际下载路径、完整 commit、许可证文件路径、
传递依赖、移植补丁、允许分发方式、尚未确认的问题。
不能将思想借鉴、源码复制、静态链接、配套工具调用视为完全相同的义务。
许可范围不明时先实现无关的自主核心与测试，隔离实验；不要因此伪造“已审核”，也不必停掉全部开发。
发布前完成法律/许可核对，本表不提供针对具体许可的法律结论。

## 2026-10-06 · 功能实现前的来源核对

新增能力先核对乐鑫官方示例，再检查 GitHub 已有实现及其适配边界；实际采用前记录版本、
源码、依赖和许可证。本轮没有新增第三方依赖或复制上游应用代码。

- 文件操作：核对本机 ESP-IDF `fff9895c82d744c7237be8847347bdd1b07c6643`（v6.1）的
  [SDMMC 示例](https://github.com/espressif/esp-idf/blob/v6.1/examples/storage/sd_card/sdmmc/main/sd_card_example_main.c)
  和 `components/fatfs/vfs/vfs_fat.c`。示例文件头为 Public Domain / CC0；示例通过
  `stat`、`unlink`、`rename` 替换文件，VFS 的 `fsync` 调用 `f_sync`。
  它不提供本项目的进度主记录/备份恢复协议，因此沿用 FileStore 的比较、同步和回读提交机制。
- 同步：重新查看 [KOReader KOSync 插件](https://github.com/koreader/koreader/blob/master/plugins/kosync.koplugin/main.lua)
  和 [官方同步服务器](https://github.com/koreader/koreader-sync-server/blob/master/README.md)。
  本机已有 KOReader 客户端源码，检查了位置接续、设备字段和时间戳判断路径。
  本轮只核对协议与行为，没有复制或链接其 AGPL 应用代码；此前固定提交与许可证见
  [EPUB/KOSync 探针](epub-kosync-probe.md)。本项目仍要求明确选择远端位置或确认上传；
  API 未提供的时间和设备在线状态不作推断。
- CrossPoint 的固定源码和耦合审查已见同一探针记录。本轮沿用现有精确 XPointer 映射，
  不因上游具备类似功能就引入整套 Arduino 阅读器或用百分比代替源位置。

## 2026-10-06 · 交互耗时修复前的来源核对

针对 [开发状态](development-status.md) 中“交互耗时排查”列出的问题，先查官方资料与已有实现。
本轮只读源码和文档，没有复制代码、新增依赖或修改固件。

| 来源 | 版本 | 许可证 | 查看范围 |
|---|---|---|---|
| [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) | `7394e40fa97f61007f61458ed8d94e0dd378c323` | MIT（顶层） | 与 CrossMux 同结构，本轮以 CrossMux 为准 |
| [0x1abin/crossmux](https://github.com/0x1abin/crossmux) | `1512e8e049f2b1a4efc7c004a5b1a93cbc5e3f9f` | MIT（顶层） | 封面缓存、章节分页缓存、加载提示、缓存目录、Read Pico 记录 |
| [0x1abin/freeink-sdk](https://github.com/0x1abin/freeink-sdk) | `96de1be6ce08eb732909e6e8149af8f892b9a2c5` | MIT（顶层） | FreeType 字体后端的取字宽与 hinting 选项 |
| [KOReader coverbrowser](https://github.com/koreader/koreader/blob/master/plugins/coverbrowser.koplugin/bookinfomanager.lua) | master，2026-10-06 查看 | AGPL-3.0，仅参考行为 | 封面缓存与失效判断 |
| [ESP-IDF 速度优化指南](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/performance/speed.html)、[FatFs 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/fatfs.html) | stable，2026-10-06 查看 | 文档 | stdio 缓冲、fast seek、读写粒度 |
| [espressif/esp_jpeg](https://components.espressif.com/components/espressif/esp_jpeg) | 1.3.1（未引入） | 未核对，采用前再读 | JPEG 解码缩放能力 |

可采用的做法：

- 封面：CrossMux 把缩略图写成每本书缓存目录下的文件，只生成一次；空文件表示“没有可用封面”，不再重试。
  KOReader 把缩放后的缩略图存入缓存库，用文件大小和修改时间判断失效，并记录失败次数。
  两者都只缓存缩放后的结果。本项目的 `PicoPage/cache` 已由“清理全部缓存”覆盖，可放同类文件；
  失效判断沿用现有内存缓存的文件大小和修改时间。
- 加载提示：CrossMux 只在预计耗时较长时才画提示（章节原文超过 96 KiB 且需解压，或目标页超过 20 页），
  提示画在当前画面上并立即刷新，避免短操作多闪一次。封面批量生成时先显示提示，完成后只做一次最终刷新。
- 分页：CrossMux 按章节把分页结果写到 TF 卡，文件头含字体、行距、视口等排版参数；支持只排到目标页的部分缓存，之后再续排。
  本项目暂不引入磁盘分页缓存；先采用“只排到需要的位置并记住途中页起点”。
- 字宽：freeink-sdk 的说明指出 `FT_LOAD_NO_HINTING` 会把字宽由取整变为截断，从而改变分页，不能用它换速度。
  它为 TF 卡字体保留持久的字宽表，并按页预热字形。本项目的 FreeType 未编入自动 hinter，因此不存在自动 hinting 开销。
- TF 读取：官方指南说明 stdio 默认缓冲为 128 字节，建议用 `setvbuf` 或 `CONFIG_FATFS_VFS_FSTAT_BLKSIZE`
  提高到 4096 字节以上，或直接使用 `read`。本项目的 FreeType 默认流使用 `fseek`/`fread`，属于受影响路径；需真机计时确认收益。
- TF 总线：CrossMux 记录 Read Pico 为 1-bit SDMMC、40 MHz。本项目实测顺序读约 3.0 MiB/s，属于该接法的正常范围，不作为优化对象。
- JPEG：`esp_jpeg` 与本项目已有的 tjpgd 同源，同样只支持 1/2、1/4、1/8 缩放和基线 JPEG；更换组件不能解决 PNG 或渐进式 JPEG 的耗时。

不采用：CrossMux 用路径哈希作为缓存键、把非活动 OTA 分区用作字体缓存、预处理字体格式。
前者与本项目以内容摘要识别书籍的进度规则不同；后两者涉及分区和新文件格式，超出本轮范围。

实现结果：封面缓存、状态行和页起点记录均为本项目自行实现，没有复制上述项目的代码。许可证义务没有变化。

- `CONFIG_FATFS_VFS_FSTAT_BLKSIZE=4096` 经真机对照后未采用：顺序读没有可见收益，
  字形的随机小块读取变慢，新页准备时间增加约六成。官方建议针对顺序吞吐，不适用于本项目的字体访问方式。
- 字宽改用 FreeType 自带的 `FT_Get_Advance`（[接口说明](https://freetype.org/freetype2/docs/reference/ft2-quick_advance.html)），
  属于已固定版本 FreeType 的公开接口，没有新增依赖。与 freeink-sdk 的结论一致：不使用 `FT_LOAD_NO_HINTING`，
  而是复现带 hinting 加载时的取整结果，并用全字符比对确认一致。
