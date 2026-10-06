# 第三方依赖审查表

状态：2026-10-06 已核查并锁定实际使用的 BSP、TOML/JSON、ZIP/XML、FreeType 和字体；详见各组件记录。
项目总许可证待决定，本表不是整项目发布许可结论。

| 候选/用途 | 路径/来源 | commit | 许可证/依赖审查 | 本地修改 | 当前处置 |
|---|---|---|---|---|---|
| Read Pico 官方 BSP/PMU/驱动 | MindReset/read_pico_firmware | `28cde682a4468a581c278761922724f57d976418` | 八个 Apache-2.0 组件；epdiy 为 LGPL-3.0-or-later；详见下方审查 | 无 | 下载到忽略目录 `.local/`，编译引用九个组件 |
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
