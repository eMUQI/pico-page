# 第三方依赖

项目自有代码按 GPL-3.0-or-later 及 ESP-IDF 二进制库链接许可发布，见[许可说明](../../LICENSING.md)。本文只记录进入固件、桌面程序、网站或部署产物的第三方内容，以及改编自外部来源的代码：来源、固定版本、许可、本地修改和分发义务。只参考思路、未复制代码或数据时不在此记录；对设计有影响的项目见[调研参考](../planning/references.md)。本文不提供法律结论。

新增依赖时写清仓库 URL、完整 commit 或发行版本、许可证文件、传递依赖、本地补丁和分发方式。许可范围不明时先隔离实验，不写成已审核。

## 固件与桌面程序

| 依赖 | 来源与固定版本 | 许可 | 本地修改 | 使用方式 |
|---|---|---|---|---|
| Read Pico BSP、PMU 与驱动 | [MindReset/read_pico_firmware](https://github.com/MindReset/read_pico_firmware) `5f3406deec33304de8801046db583dec7c49e55b` | 八个组件 Apache-2.0；epdiy LGPL-3.0-or-later | 补丁只应用到构建副本，见 [BSP 审查](bsp-review.md) | 下载到忽略目录 `.local/`，编译引用九个组件 |
| epdiy 与 E0470 波形 | 同 BSP commit | LGPL-3.0-or-later／Apache-2.0 | 欠载诊断、清屏检查、逐行相位和页脚保持补丁，见下文「改编代码」 | 显示驱动 |
| Expat、miniz | CrossPoint 第三方目录 `50823ffa125b437b4c9fe5882f326458780198da` | MIT；siphash CC0 | Expat 配置与 entropy 守卫移植；miniz 使用 `pico_tinfl_decompress` 名称，避免调用布局不同的 ROM 版本 | `reader_document`、`reader_dictionary`；[来源与逐文件摘要](../../firmware/components/reader_document/third_party/README.md) |
| tomlc17 | [cktan/tomlc17](https://github.com/cktan/tomlc17) `8a4564e0b257b010c24c973d4353e0c1ebf33f80` | MIT | Xtensa `PRIx32` 格式修正；内存页大小改用 `offsetof` | `reader_store` 配置解析；[来源](../../firmware/components/reader_store/third_party/README.md) |
| cJSON | [DaveGamble/cJSON](https://github.com/DaveGamble/cJSON) v1.7.19 `c859b25da02955fef659d658b8f324b5cde87be3` | MIT | 无 | `reader_store` 进度 JSON |
| FreeType | [FreeType](https://freetype.org) `526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b` | FTL、MIT 及 Adobe CFF 附加条款 | 模块与选项配置 | 固定下载，`reader_graphics`；见[字体与渲染](graphics-runtime.md) |
| JPEGDEC | [bitbank2/JPEGDEC](https://github.com/bitbank2/JPEGDEC) `86282979224c8a32fd51e091ed5a35b0c699a52b`，补丁源 CrossPoint `43694c9018d26446ab30cce2fa65ee608355ae50` | Apache-2.0；补丁保留 MIT 声明 | 修复未对齐读取与未定义移位，禁用 SIMD | 渐进式 JPEG 的 DC 预览；[来源与补丁](../../firmware/components/reader_graphics/third_party/jpegdec/README.md) |
| 霞鹜文楷、Noto Sans SC | 官方发布，固定 SHA-256 见[字体与渲染](graphics-runtime.md) | OFL 1.1 | Noto 子集改名 Pico Page Fallback；文楷子集改名 Pico Page Welcome | 全字体在 `.local/`，内置子集随组件分发，附许可证 |
| Unihan `kTGH` | Unicode 17.0.0 `Unihan.zip`，SHA-256 见[字体与渲染](graphics-runtime.md) | Unicode License v3 | 只提取 3,500 字清单 | 决定内置字体的字符范围 |
| GB18030 解码表 | 由 `tools/generate_gb18030_table.py` 查询 CPython 3.14.7 `gb18030` 编解码器生成，结果 SHA-256 `5efc9c505062b83e0a80f04547e80d0c400b8d2ff6397cd86ee02f3993d68bbb` | CPython 为 PSF-2.0；只取编码与码位的对应数值，不含 CPython 代码 | 双字节区 23,940 项数组，四字节区 207 个区间 | `reader_document` |
| Phosphor 图标 | [phosphor-icons/core](https://github.com/phosphor-icons/core) `2b75f3ad12b420c9504ef05df8d2564a28f8500e` | MIT | 七个 SVG 预栅格为 40 × 40、4 位透明度掩码 | `reader_app`；[来源与生成](../../firmware/components/reader_app/third_party/phosphor/README.md) |
| esp_tinyusb、TinyUSB | Espressif Component Registry 2.4.0／0.21.0~1，hash 见 `dependencies.lock` | Apache-2.0／MIT | 不改上游；排除便利存储层，接入同步 SDMMC 回调 | 仅 MSC；见 [USB 运行记录](usb-storage-runtime.md) |
| SDL2、OpenSSL、libcurl | 本机开发库，版本见构建日志 | zlib／Apache-2.0／curl | 无 | 仅桌面程序，动态链接 |
| CREngine | [koreader/crengine](https://github.com/koreader/crengine) `f4a8be93255a439db4777d00f0bc53316acabebd` | GPL-2.0 | 无 | 仅本地同步映射对照工具，不进入固件或阅读程序；见[同步实现](sync-runtime.md) |

GB18030 解码表用 CPython 3.14.7 重新生成后与仓库文件逐字节相同。该编解码器采用 GB 18030-2000 映射：`A8BC` 解码为 U+E7C7，`8135F437` 解码为 U+1E3F，GB 18030-2005 交换了这两项；2005 与 2022 修订的其余调整未逐项核对，受影响的字符会显示为私用区码位或不同的字。

## 改编代码

| 代码 | 来源 | 许可 | 范围 |
|---|---|---|---|
| `platform_read_pico/changed_pixels_waveform.cpp` | [FreeInk SDK](https://github.com/0x1abin/freeink-sdk/tree/96de1be6ce08eb732909e6e8149af8f892b9a2c5) `libs/display/EpdiyLcd/src/e0470/e0470_epaper_waveform.c` | Apache-2.0（文件头与同目录许可，非仓库根 MIT） | 只保留对角线保持及边界检查，改为纯函数；不含 FreeInk 波形数据 |
| `platform_read_pico/ripple.cpp` | Read Pico 厂商的 `e0470_page_turn`，公开副本见 [kiikoread](https://github.com/wegooo-cell/read-pico-reader/tree/8d6f1a6f2f5cbee5bba08df43ebb5001d2584d83) | Apache-2.0；官方确认允许使用并要求原样保留 | 只保留倒置竖屏全屏从右向左的 16 条带路径，PSRAM 表 37 KiB |
| `firmware/board/patches/epdiy-row-phases.patch` | 同上 commit 的 epdiy `render.c`、`render_context.h`、`render_lcd.c` | LGPL-3.0-or-later | 逐行相位 LUT 借用接口、相位范围与单相差分检查 |
| `firmware/board/patches/epdiy-footer-hold.patch` 与 `epdiy-unchanged-pixels.h` | 本项目实现，修改 BSP 的 epdiy | LGPL-3.0-or-later | 页脚相同像素的驱动位清零，复用 `epd_apply_line_mask_VE` |
| `epdiy-underrun-diagnostics.patch`、`epdiy-feed-start.patch` | 本项目实现，修改 BSP 的 epdiy | LGPL-3.0-or-later | 欠载诊断与清屏检查；LCD 双供数任务的启动屏障 |
| BSP SD 卸载、SD 忙状态与 PMU RTC 校验补丁 | Read Pico BSP | Apache-2.0 | 见 [BSP 审查](bsp-review.md) |

补丁只应用到构建目录副本，原始 BSP 检出不变。分发固件时须提供修改后 epdiy 的对应源码，并保留原文件署名与许可证。

## 网站

安装页在浏览器中运行以下 npm 包，源码未修改，由 Astro 打包；不进入固件和桌面程序。许可证全文合并在 `web/public/assets/third-party-licenses.txt`，安装页页脚提供链接。升级时同步更新该文件和本表。

| 包 | 版本 | 许可 |
|---|---|---|
| [esptool-js](https://github.com/espressif/esptool-js) | 0.7.0 | Apache-2.0（包内无 NOTICE） |
| [pako](https://github.com/nodeca/pako) | 2.2.0 | MIT AND Zlib |
| [atob-lite](https://github.com/hughsk/atob-lite) | 2.0.0 | MIT |
| [spark-md5](https://github.com/satazor/js-spark-md5) | 3.0.2 | WTFPL OR MIT |

## 部署工具

COS 上传与监控使用 `tools/cos-requirements.txt` 固定的 PyPI 发行包，只在 GitHub Actions 和云函数包中运行，不进入固件、桌面程序或网站。依赖不复制进仓库；函数包保留各依赖的许可与元数据。

| 包 | 版本 | 许可 |
|---|---|---|
| cos-python-sdk-v5 | 1.9.44 | MIT |
| tencentcloud-sdk-python-common／monitor | 3.1.188 | Apache-2.0 |
| certifi | 2026.7.22 | MPL-2.0 |
| charset-normalizer／crcmod／six／urllib3／xmltodict | 3.5.2／1.7／1.17.0／2.8.0／1.0.4 | MIT |
| idna | 3.20 | BSD-3-Clause |
| pycryptodome | 4.0.1 | BSD-2-Clause AND Unlicense |
| requests | 2.34.2 | Apache-2.0 |

## 固件 SBOM 组件清单（2026-11-06）

用 ESP-IDF 6.1 环境中的 `esp-idf-sbom` 1.3.1，对标准构建的 `firmware/build/project_description.json` 执行 `python -m esp_idf_sbom create --rem-unused --file-tags`，得到 120 个 SPDX 包，覆盖实际链接进应用的项目组件、ESP-IDF 组件、托管组件和工具链运行库。每次发布应按发布构建重新生成。

许可以 Apache-2.0 为主，另有 MIT、BSD-2-Clause、BSD-3-Clause、ISC、CC0-1.0／Unlicense 等宽松许可；`epdiy` 为 LGPL-3.0-or-later，mbedTLS 为 Apache-2.0 OR GPL-2.0-or-later（本项目采用 Apache-2.0）。工具未给出结论的包已按许可文件核对：`esp_wifi`、`esp_phy`、`esp_coex` 的二进制库为 Apache-2.0；lwIP 为 BSD-3-Clause；http_parser 为沿用 NGINX 条款的 MIT 式许可；`esp_libc` 的 picolibc／newlib 部分以 `COPYING.picolibc` 和 `COPYING.NEWLIB` 为准；micro-ecc 为 BSD-2-Clause，只进入 bootloader，因此只随全量镜像分发。项目组件按[许可说明](../../LICENSING.md)为 GPL-3.0-or-later。

二进制发布需要随附宽松许可要求保留的版权与许可文本、Apache-2.0 组件的许可及 NOTICE（如有）、FreeType 的 FTL 致谢和字体 OFL；epdiy 的 LGPL 义务由公开的对应源码与构建步骤满足。随固件发布的许可汇编尚未生成，NOTICE 文件也未逐包确认；发布前按[公开发布检查](publication.md)处理。
