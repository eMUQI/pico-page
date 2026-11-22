# 版本管理

产品版本的唯一来源为 `firmware/version.txt`。各版本内容见 [`docs/releases/`](../releases/) 和 GitHub Release；当前版本见[开发状态](../development-status.md)。

## 编号与兼容性

下表中的 `0.2.0` 等版本仅为编号示例，不表示当前版本或下一次发布计划。

采用 [SemVer 2.0.0](https://semver.org/lang/zh-CN/) 的三段编号和预发布排序，项目接受 `X.Y.Z`、`X.Y.Z-beta.N` 和 `X.Y.Z-rc.N`。三段数字不补零，阶段序号从 1 开始。

| 版本 | 用途 |
|---|---|
| `0.2.0-beta.N` | 正式发布前的连续测试构建，允许调整功能 |
| `0.2.0-rc.1` | 功能冻结后的候选版，只修复发布阻塞问题 |
| `0.2.0` | 正式发布版，项目整体仍处于 0.x 阶段 |
| `0.2.1` | 兼容的问题修复 |
| `0.3.0-beta.1` | 下一轮功能测试 |
| `1.0.0` | 核心功能范围与兼容性承诺稳定 |

0.x 阶段，兼容修复增加修订号，新增功能或不兼容调整增加次版本号。1.0 以后，兼容修复增加修订号，兼容功能增加次版本号，不兼容修改增加主版本号；高位增加时低位归零。兼容性重点包括 TF 卡配置、阅读进度和同步约定，内部重构不自动构成大版本。数据 schema 独立维护，不能用产品版本代替文件格式检查。

同一目标版本按 `beta.1 < beta.2 < rc.1 < 无后缀版本` 排序。每次发给测试者的新固件都增加版本号，已发布的标签与固件不可覆盖。

1.0 的门槛是核心功能范围与相关验收完成，包括 EPUB 离线阅读、KOReader 双向位置接续和关键保存路径；不要求完成全部路线图。版本号本身不是验收证据。

## 构建与显示

固件和 host 共用 `tools/project_version.py` 解析版本；固件把结果写入 ESP-IDF 的 `PROJECT_VER`，设置页和使用说明通过应用描述读取同一版本。host 预览也显示该版本。

干净检出且 HEAD 带有匹配的 `v<版本>` 标签时，版本为文件中的原值，例如 `0.2.0-beta.1`。其余构建附加 `+g<七位提交号>`，存在已跟踪或未忽略的未跟踪改动时再附加 `.dirty`，例如 `0.2.0-beta.1+g1234567.dirty`。完整提交号和源内容摘要保存在构建来源记录中。源码必须来自 Git 检出，不从缺失的 Git 信息推断发布身份。

ESP-IDF 应用描述的版本字段为 32 字节，其中一字节用于终止符；脚本拒绝超过 31 字节的结果，不允许静默截断。不得通过 `PROJECT_VER` 缓存变量、Kconfig 版本覆盖或排除版本字段来绕过版本文件。

标准固件入口 `sh tools/build_firmware.sh [release|performance|diagnostic]` 每次重新配置以刷新 Git 状态，并在完成时检查镜像内版本。直接使用 IDF 时执行 `idf.py -C firmware reconfigure build`；host 执行 `cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug` 后再构建。仅执行底层增量构建命令可能保留上一次配置时的 Git 标识。

`beta` / `rc` 表示发布阶段，`release` / `performance` / `diagnostic` 表示编译配置。普通测试者使用 `release` 配置，需要排障时才提供标明配置的其他产物。`provenance.json` 保留版本、构建模式、完整 commit、dirty 状态、来源摘要和产物 SHA-256；`default` 表示无参数构建入口，不能冒充 release 配置。

## 发布说明

每个版本在 `docs/releases/<版本>.md` 维护简体中文，并在同目录的 `zh-Hant/` 和 `en/` 下维护同名译文。三份说明记录相同的变更、适用条件和迁移步骤。网站按页面语言选择译文，GitHub Release 正文由三份说明生成，OTA 清单携带三种正文。版本号和日期由各入口显示。

按 [Keep a Changelog 2.0.0](https://keepachangelog.com/en/2.0.0/) 记录读者可感知的变化。按需使用「新增」「变更」「弃用」「移除」「修复」「安全」二级标题，只保留有内容的分组。每条写清受影响的操作和实际变化。需要重新安装或迁移时，写明原因及操作。测试记录、内部格式版本和已知问题留在发布 PR、Issue 或[开发状态](../development-status.md)。

## 构建与打包

1. 更新版本文件和发布说明，完成适用检查并提交。使用干净的 `v<版本>` 标签检出。包内的项目源码入口指向公开标签，发布时须能匿名访问对应提交。
2. 按[发布工具链](environment.md#发布工具链)准备 ESP-IDF 与 SBOM 工具，并执行 `python3 tools/setup_fonts.py`。在仓库根目录执行 `python3 tools/package_release.py`。
3. 使用 `build/release/<版本>/` 中的镜像完成该版本要求的设备验证。留存验证范围与结果，状态变化时更新开发状态。

打包命令校验标签和 SDK，调用标准 `release` 构建，核对应用版本与构建来源，并生成下列四个文件。

| 附件 | 用途 |
|---|---|
| `pico-page-<版本>-read_pico-release-full.bin` | 首次安装或恢复，从 `0x0` 写入。包含 bootloader、分区表和同一次构建的应用（D41）。 |
| `pico-page-<版本>-read_pico-release.bin` | 设备联网更新，写入兼容布局的备用应用槽。 |
| `manifest.json` | 设备使用的 OTA 元数据，由网站部署读取。 |
| `pico-page-<版本>-source-materials.tar.gz` | 对应源码入口、构建说明、许可与 NOTICE、应用和 bootloader 的 SBOM、构建来源及前三个附件的 SHA-256。 |

双 7 MiB 发布附件可通过 `python3 tools/ota_manifest.py --check build/release/<版本> --expect-layout ota7 --require-full --expect-version <版本>` 复核。核对旧布局附件时使用 `--expect-layout legacy`。

资料包通过固定提交与校验值引用上游源码。发布前核对这些来源可公开下载，确认实际分发组件所需的声明齐全。上游来源不可用时，提供该版本的公开源码副本并更新指引。组件许可依据见[依赖审查](dependency-review.md#固件-sbom-组件清单2026-11-06)。

## 发布到 GitHub

确认远端标签、候选构建与本地附件一致。运行 `python3 tools/release_notes.py <版本> > build/release/<版本>-notes.md` 生成 Release 正文：简体中文直接显示，繁体中文和英文放在以语言名为标题的折叠区。GitHub Release 只有一份正文，不提供语言切换。用该标签创建 GitHub Release 草稿，将生成文件传给 `--notes-file`，附上打包目录的四个文件。核对草稿的正文、两个折叠区、附件名称与 SHA-256 后再公开。Beta 和 RC 默认标为预发布，已公开的预发布也参与新布局版本选择。首次发布双 7 MiB 版本前，须先部署渠道隔离并验收[分区迁移方案](../planning/ota-partition-migration.md#验收顺序)。发布说明应引导旧布局用户直接通过电脑完整重装。

官网更新日志列出 `docs/releases/` 中已发布的版本。日期优先取 [`docs/releases/release-dates.json`](../releases/release-dates.json) 中固定的原始发布日期，其余版本取 GitHub Release 的发布时间（北京时间）；GitHub 上删除旧 Release 不影响已固定的版本。

等待 Release 触发的 `Website build` 完成。核对线上清单、安装页版本与更新日志，并确认 COS 下载的镜像与发布附件一致。部署与恢复步骤见[固件托管](firmware-hosting.md#自动部署)。发布后的下载验证与真机安装、升级和回滚分别记录。

## 联网更新使用的版本信息

设备端的版本比较在 `firmware/components/reader_sync/update.cpp`，与 `tools/project_version.py` 接受同一组版本形式：`+` 后的构建元数据不参与排序；带构建元数据的版本视为开发构建，不能据此判断新旧。清单中的版本必须是不带构建元数据的发布版本，并与镜像应用描述中的版本一致，否则设备拒绝写入。

打包入口使用 `tools/ota_manifest.py` 的镜像校验和清单生成逻辑。新布局的应用镜像上限为 7 MiB，旧布局为 4 MiB。每种语言的发布说明上限为 8 KiB。三语清单保留简体 `notes`，通过 `notes_i18n` 写入 `zh-Hant` 和 `en`。生成后的 JSON 上限为 23 KiB，为设备的 24 KiB 状态缓存留出元数据空间。校验器也接受已有的单语清单。全量镜像中的应用必须与 OTA 镜像逐字节一致。网站工作流分别验证旧布局固定的 0.6.2 和新布局的最高公开版本。清单的 `data_schema` 取自设备源码中的 `progress_schema` 和 `config_schema`，修改进度或配置文件格式时同步更新相应常量。

网站部署以 COS 为固件下载源。工作流使用 `--check --publish-base-url` 指向[固件托管](firmware-hosting.md#下载源与职责)中的正式地址，先校验原清单和镜像，再原子替换部署副本清单的 `url`；其他字段与 GitHub Release 原附件不变。认证上传、完整读回、匿名核验及网站发布的顺序和失败条件见[自动部署](firmware-hosting.md#自动部署)。旧服务器仅供人工恢复，不用作新发布命令的目标。
