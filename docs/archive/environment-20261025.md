# 开发环境与硬件记录历史快照

本页保留 2026-10-25 整理前的 `docs/development/environment.md` 完整正文（基线 `a2aba169`），仅修正迁移后的相对链接。正文混合了 2026-10-06 起的环境、构建、写入结果及后续规则；其中「本机」「最新」「目前」和授权仅指原记录，不代表当前主机、设备或发布状态。现行操作方法见[开发环境](../development/environment.md)，实现与验收状态见[开发状态](../development-status.md)。

---

# 开发环境与硬件记录

本页工具链和设备表记录 2026-10-06 的特定 macOS 环境，路径已脱敏，不代表当前机器状态，也不是刷写授权。2026-10-10 的干净 Linux 工具链及后续构建记录见[开发状态](../development-status.md)。通用构建入口与当前分区说明见[固件开发指南](../../firmware/README.md)；操作前核对本机工具链、实际配置和目标设备。

核查日期：2026-10-06。工作目录：`~/workspace/pico-page`。现有 ESP-IDF/EIM 已复用；本轮没有安装、升级或修改全局工具链。

该次 `sh tools/build_firmware.sh` 完整构建成功，应用镜像已写入实测 `ota_0` 区域并通过摘要校验。已确认 PMU、工厂 VCOM 读取、SD、PSRAM、书库及两本 EPUB 的设备阅读路径；完整字体和书籍已导入并读回 CRC。最后的重启与画布验证见设备验证页。这不是全部功能验收；物理目视、触摸、睡眠与设备 Wi-Fi/HTTPS 同步仍待验证。用户已明确取消完整备份，目前无审批阻塞。改动未提交、推送或发布。

## 本机工具链

| 项目 | 实际值 |
|---|---|
| 主机 / shell | macOS 27.0（26A428），arm64 / zsh |
| host 编译器 | Apple clang 21.0.0，`clang-2100.1.1.101` |
| EIM | `/opt/homebrew/bin/eim`，0.19.0；已选择 v6.1 |
| ESP-IDF | `~/.espressif/v6.1/esp-idf`；`idf.py --version` 为 `ESP-IDF v6.1-dirty` |
| IDF commit | `fff9895c82d744c7237be8847347bdd1b07c6643`，tag `v6.1` |
| EIM 激活脚本 | `~/.espressif/tools/activate_idf_v6.1.sh` |
| Python | `~/.espressif/tools/python/v6.1/venv/bin/python`，3.14.7 |
| CMake / Ninja | 4.4.3 / 1.12.1 |
| 交叉编译器 | `xtensa-esp-elf-g++`，GCC 15.2.0，`esp-15.2.0_20251204` |
| 官方 BSP commit | `5f3406deec33304de8801046db583dec7c49e55b`，组件与许可见 [BSP 审查](../development/bsp-review.md) |
| 构建目标 | ESP32-S3；实测 16 MiB Flash、8 MiB PSRAM；Octal PSRAM 80 MHz 为编译配置 |

本机另有 EIM 管理的 v5.5.4、v5.5.5、v6.0.3。本轮未切换全局选择。普通 PATH 中没有 `idf.py`；通过 EIM 可以使用已安装的 v6.1。

IDF 源码原先就有一处已跟踪修改：`components/esp_system/ld/ld.ext_ram.sections` 将 `_ext_ram_on_same_bus` 分支的地址表达式包在 `MAX(ABSOLUTE(.), ...)` 中。文件 SHA-256：`2fdfc21bfb7e0d31d7c30260c6739169fe0809fb52fd9a66dcaf036f7711178f`。另有未跟踪的 `tools/requirements/requirements.merged.txt`。这些内容均保留原样；本次本机结果不能描述为「干净的官方 v6.1 环境已验证」，这是该次 macOS 构建的限制；2026-10-10 的干净 Linux 复现见开发状态。

## 已执行的环境命令

```sh
eim --version
eim --log-file .local/eim.log --do-not-track true list
eim --log-file .local/eim.log --do-not-track true run "idf.py --version" v6.1
eim --log-file .local/eim.log --do-not-track true run "python --version && cmake --version && ninja --version && xtensa-esp-elf-g++ --version" v6.1
git -C ~/.espressif/v6.1/esp-idf rev-parse HEAD
git -C ~/.espressif/v6.1/esp-idf status --short
sh -n tools/build_firmware.sh tools/setup_bsp.sh
```

以上命令退出码均为 0。没有显式 `--log-file` 的 EIM 列举命令先前因日志路径权限失败；项目脚本将 EIM 日志放到忽略目录 `.local/`。

## 固件构建入口与证据

产品版本唯一来源为 `firmware/version.txt`，规则见[版本管理](../development/versioning.md)。标准入口每次先重新配置，确保提交号、标签和 dirty 状态进入应用版本；构建完成后核对镜像中的版本，并把版本、构建模式、来源和摘要写入 `provenance.json`。手动调用 IDF 时使用 `idf.py -C firmware reconfigure build`；host 重新构建前先执行下文的 CMake 配置命令。

仓库根目录执行：

```sh
sh tools/build_firmware.sh
```

首次下载 BSP 需要访问 GitHub。脚本与 CMake 核对 BSP 的完整 commit，发现已有不同版本或已跟踪组件修改时停止，不自动重置开发者的文件。已激活 IDF 的等价命令是 `idf.py -C firmware reconfigure build`；本机使用 `eim --log-file .local/eim.log --do-not-track true run "idf.py -C firmware reconfigure build" v6.1`。构建脚本没有刷写、擦除或监视串口动作。

初次受限运行被 macOS `psutil` 的 `sysctl()` 进程枚举权限阻止，发生在组件管理器配置阶段。随后在授权的非沙箱构建环境继续。首次完整编译发现 TOML 库把 `uint32_t` 用 `%x` 格式化，与 Xtensa 的类型定义不匹配；改为 `PRIx32` 后，完整构建退出码为 0。第三方修改见依赖记录。早期 G0 产物为 162,064 字节，SHA-256 为 `8317108ae066cad40acc87a5c259c13d5bf7cc2381efb5210670d2df1ce57cb0`；当时入口未初始化板级外设、未运行固件。该记录不代表下面的最新应用。

| 验证 | 实际结果 |
|---|---|
| BSP 源码锁与差异检查 | commit 匹配，组件无修改 |
| ESP-IDF 完整构建 | `sh tools/build_firmware.sh` 成功；包括 ROM 符号隔离、PSRAM 分配、默认字体、排版反馈、字宽缓存及 4,084 字符内置字体 |
| 应用产物 | `firmware/build/pico_page.bin`，1,956,464 字节；另有同目录 `pico_page.elf` |
| 最新完整构建日志 | `.local/firmware-metrics-cache.log`、`firmware/build/log/`；均为忽略产物 |
| 设备应用写入 | `0x10000` 的实测 4 MiB `ota_0` 槽，仅写入应用，`Hash of data verified`；`.local/device/install/offline-final-flash.log` |
| 固件运行 | 开书崩溃已修复，设备离线阅读完整流程基线通过；最终验收见 device-validation |

最新 BIN 的 SHA-256：`95cc9b7072fe2e4b3f3179acb7505de8ff7ec67a4ea52d38797a1f536a9bb95b`。构建器报告的 38% 剩余空间属于本地 3 MiB 应用构建配置，不能直接作为实测 4 MiB OTA 槽的余量。

`firmware/dependencies.lock` 记录 IDF 6.1.0。BSP 不是组件仓库浮动版本，由 `tools/setup_bsp.sh` 与固件 CMake 中的完整 commit 固定。

## 设备与分区

已通过 `/dev/cu.TARGET_DEVICE` 自动进入下载模式，读回 ESP32-S3 revision v0.2、8 MiB PSRAM、16 MiB Flash 和安全状态；Secure Boot / Flash Encryption 未启用。实际分区含 `ota_0` / `ota_1` 两个 4 MiB 槽，分区表读取及解析成功。早期完整备份首次传输失败，重试曾因自动审批服务用量限制而未执行；用户随后明确取消完整备份，目前无审批阻塞。已按实测布局仅在 `ota_0` 的 `0x10000` 写入应用，写入摘要校验通过。原 bootloader、分区表、NVS、otadata 与工厂 VCOM/PMU 校准未改，TF 未格式化。此前首版启动已确认 PMU、工厂 VCOM 读取、SD、PSRAM 与书库全刷完成。完整资源已导入并验证，离线开书、翻页、排版、保存和切书恢复已实测通过。完整证据、实际偏移及当前进展见 [设备检查记录](../development/device-validation.md)。

本地 `firmware/partitions.build.csv` 仍只用于编译，不能覆盖实际分区。物理目视显示效果、触摸/按键、TF 保存、睡眠/唤醒与功耗仍待验收。

## host 与交接检查

host 使用系统 AppleClang 和 Homebrew CMake / Python，不依赖 EIM 的交叉编译器。在仓库根目录执行：

```sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel 4
ctest --test-dir build/host --output-on-failure
```

内存及未定义行为检查使用同一批目标：

```sh
cmake -S . -B build/sanitized -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build/sanitized --parallel 4
ctest --test-dir build/sanitized --output-on-failure
```

ASan/UBSan 通过不等于已检查内存泄漏。LLVM 官方支持 macOS 上的 LSan，但必须核对实际编译器和运行时：本机系统 AppleClang 21 的运行时在 `ASAN_OPTIONS=detect_leaks=1` 下明确报 `AddressSanitizer: detect_leaks is not supported on this platform`。这是当前工具链的限制，不能概括为 macOS 不支持。默认 macOS 命令仅记录 ASan/UBSan 结果；具备 LSan 的工具链应显式使用 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1`，不通过关闭泄漏检查冒充完整通过。现有 Linux CI 使用独立 Clang sanitizer 构建并保留上述选项，运行相同的完整 CTest。依据：[LLVM AddressSanitizer 文档](https://clang.llvm.org/docs/AddressSanitizer.html#memory-leak-detection)、[LeakSanitizer 支持平台](https://clang.llvm.org/docs/LeakSanitizer.html#supported-platforms)。

CTest 保存实际日志于 `build/host/Testing/Temporary/LastTest.log` 与 `build/sanitized/Testing/Temporary/LastTest.log`。host 针对 POSIX 文件 API；macOS 本机与 Linux CI 的结果分别记录。此前 macOS 全新 sanitizer 编译产生六处未修改 cJSON 的 `sprintf` 弃用警告，没有把这些警告描述成零警告构建。验收结果与范围由 [开发状态](../development-status.md) 记录。早期环境验证为 host CTest 与 ASan/UBSan CTest 各 9/9、交接检查 284 项；不能代替当前批次的测试数量和结果。`python3 tools/check_handoff.py` 仍只检查交接资料，不能替代 C++ 测试、固件构建或真机验证。

## GitHub Actions

个人 Pro 账户的 Actions 额度由该账户下的私有仓库共享；付费预算与付款状态由账户所有者管理，仓库工作流不修改账户计费设置。远端运行未启动时，本地验证不能记为远端 CI 通过。

`Host checks` 和 `Website build` 保留原有检查名称，PR 更新后均生成检查结果，再由 `tools/ci_changes.py` 根据完整 PR 差异选择耗时步骤。路径判断包含删除与重命名前后的路径；无法取得差异时检查失败，不静默跳过。代码与构建配置变更执行 Debug、独立 Clang ASan/UBSan/LSan 和交接检查；仅文档、实现截图或网站变更跳过 host 构建，交接检查仍执行。新出现且未分类的路径默认执行 host 检查。路径选择回归由 host 工作流单独运行，不计入 CTest 注册数量。

网站、发布说明、OTA 清单脚本及网站或共用路径脚本的改动执行 PR 网站构建与发布固件校验。分支 push 不单独触发工作流；main 仅在上述网站输入变更时构建并部署，发布 Release 和手动部署入口保留。host 合并后不自动重跑，直接推送 main 的代码需要手动补验；合并前应验证包含当前目标分支变更的结果。PR 新提交取消旧检查，正式部署继续串行并保留下载源比较及线上读回检查。COS 返回 403 时，仅允许候选清单与全部镜像均同官网现有副本逐字节一致的页面更新，并明确告警；新固件、内容变化、其他错误及认证发布失败仍阻止部署，不自动恢复桶权限。部署前通过共用路径脚本直接比较构建提交与最新 main 的网站输入：网站输入变化才让位给替代运行，只有文档或固件代码变化时仍完成当前部署；读取比较结果失败时停止部署。

两个工作流固定 `ubuntu-24.04`，host 超时为 20 分钟，网站超时为 25 分钟（包含固件上传及完整公网读回）。host 保留固定 Python 3.12，复用镜像预装的 Clang、pkg-config 和 OpenSSL 开发文件，仅通过 apt 安装 ccache、SDL2 和 curl 开发文件。镜像清单见 [Ubuntu 24.04 runner 软件](https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md)；镜像内工具仍会更新，固定系统版本不等于固定所有工具版本。

Debug 保持原优化配置；CI sanitizer 使用 Debug 配置及 `-O1 -g`，保留断言、事件跟踪和原有 sanitizer 选项。两种构建通过 CMake compiler launcher 共用 ccache，编译器内容和编译参数参与命中判断，单份本地缓存上限为 256 MiB。缓存只影响编译，完整 CTest 每次仍执行。字体与 FreeType 仅缓存下载输入，`setup_fonts.py` 每次重新核对摘要并按需解包，不缓存测试结果或构建目录。新增缓存使用 GitHub 官方 `actions/cache@v5`，不提高仓库的缓存付费容量上限。

网站 PR 不上传 artifact；通过过期提交判断的部署运行保留网站产物 3 天。既有 artifact 的保留时间不会因工作流修改而自动缩短，清理结果单独记录；Release 固件附件和 Cloudflare 已部署网站不属于 Actions artifact。

## 2026-10-06 · 可运行初版环境增量

原生 EPUB、FreeType、SDL2 窗口及 KOSync 已进入实际构建。准备和启动顺序：

```sh
python3 tools/setup_fonts.py
python3 tools/prepare_demo.py
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel 4
ctest --test-dir build/host --output-on-failure
build/host/pico_page --card .local/card --book 'Books/Pico Page 演示.epub'
```

日常可直接使用 `sh tools/run_host.sh`。本机链接 SDL2 2.32.72（sdl2-compat）、OpenSSL 3.6.5 和系统 libcurl；FreeType 源码为固定下载版本，详见 graphics-runtime。本机 Homebrew dylib 的目标系统版本高于当前编译目标，链接器有版本警告，实际 host 检查通过；这不证明构建产物可以直接复制到其他 macOS 版本运行。

沙箱内 SDL 无法枚举 macOS 显示设备（`The video driver did not add any displays`）；无窗口帧渲染/脚本/测试可正常运行。桌面窗口应从正常终端启动，或在已授权的本地 UI 环境运行。

真实 CREngine 映射复核：

```sh
python3 tools/check_koreader_mapping.py
```

此命令固定下载/构建独立的 GPL 引擎对照工具到 `.local`，不把 CREngine 链接进阅读器。详细输出在 `.local/koreader-reference-check.log`；合成 EPUB 不含个人内容。此外，官方完整 KOReader 客户端已通过本地测试服务完成六组双向接续与实际客户端重启恢复；版本、命令和逐组证据见 [同步记录](../development/sync-runtime.md)。外部账户服务及 Read Pico Wi-Fi/HTTPS 同步仍未验收。

固件加入联网后超过最初 1 MiB 诊断构建分区。现在使用本地 `firmware/partitions.build.csv` 的 4 MiB 单应用构建配置（与设备 ota_0 槽同大），完整 `idf.py build` 可通过。这只是构建配置；生成的 flash_args 不可当作已确认的安装方案。2026-10-12 起，`partitions.build.csv` 的条目改为与读回的设备布局一致（两个 4 MiB 应用槽和 otadata），并启用 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`；自 D41 起，release 构建的 bootloader、分区表和应用由 `tools/ota_manifest.py --full` 合并为全量镜像，用于整片安装；不要直接使用构建日志输出的烧录参数，开发时只写应用须先确认设备的分区表和当前启动槽。`reader_app` 按页面与职责分为多个翻译单元，私有状态与小型内联辅助函数位于 `app_impl.h`，已移除 `-mtext-section-literals`。固件构建须同时检查链接成功、应用槽余量及各对象的字面量寻址范围；拆文件不保证固件体积下降。主应用任务栈为 32 KiB，网络工作任务为 16 KiB。普通分配阈值设为 0，优先使用 PSRAM；内部预留保持 32 KiB。设备离线路径的堆、最大连续块和栈采样见 device-validation，不作为所有内容的峰值保证。


早期诊断/EPUB 增量重建曾因 IDF 依赖管理器调用 psutil 的 `sysctl` 查询被沙箱拒绝，日志 `.local/firmware-current.log`。随后已完成授权环境中的完整构建；该历史批次结果见 `.local/firmware-metrics-cache.log`；后续批次以开发状态中对应日期、commit 和镜像摘要为准。没有修改全局 IDF；构建通过不代表尚未执行的设备功能已经验收。

## EPUB 分段计时构建

`sh tools/build_firmware.sh stages` 使用独立的 `build/firmware-stages/`，启用开发串口及旧式阶段日志，关闭聚合指标和事件跟踪。身份校验、EPUB 打开、章节解析、图片读取与解码、章节和图片缓存读写记录全部样本（`min_us=0`）；其他既有阶段仍使用各自阈值。日志使用单调墙钟，包含调度和日志开销，不是纯 CPU 时间。`chapter_parse` 包含章节缓存写入，`*_cache_write` 包含序列化或像素复制、校验和及 `*_cache_commit`；commit 包含文件提交协议，不等同于单次物理写卡。父子阶段不得直接相加。串口日志使用现有 `tools/summarize_reader_perf.py` 汇总，并按开书请求范围归属样本。该命令只构建，不刷写设备。
