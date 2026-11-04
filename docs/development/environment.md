# 开发环境

本文维护工具链定位、构建与检查方法及已知环境限制。具体批次的命令、产物和验收结果见[开发状态](../development-status.md)，公开实测报告见[测试基准](testing-baselines.md)。历史设备操作与主机记录不作为当前机器状态或操作授权。

## 工具链与依赖定位

固件使用已有的 ESP-IDF v6.1；`firmware/dependencies.lock` 锁定 IDF 6.1.0。构建脚本优先使用已激活的 `idf.py`，标准构建在未激活 IDF 时可通过 PATH 中的 EIM 调用已有 v6.1；其他构建模式需要先激活 IDF。不安装或升级全局工具链。基础依赖及资源准备见[固件开发指南](../../firmware/README.md)与[桌面开发指南](../../host/README.md)。

构建前从 EIM 安装记录确认实际路径、版本和激活脚本，不照抄历史主机路径。在仓库根目录可使用：

```sh
eim --version
eim --log-file .local/eim.log --do-not-track true list
eim --log-file .local/eim.log --do-not-track true run "idf.py --version" v6.1
```

激活后检查 `$IDF_PATH` 对应检出的 commit、已跟踪差异和未跟踪文件，并记录实际 Python、CMake、Ninja 与交叉编译器版本。EIM 日志放到忽略目录 `.local/`。已有安装可能包含本地修改；构建前须核对，不能将历史成功结果描述为干净官方工具链的本次验证。

BSP 的完整 commit 由 `boards/read_pico/bsp-revision.txt` 唯一指定，`tools/setup_bsp.sh` 和板级 CMake 共用；来源、补丁与许可见 [BSP 审查](bsp-review.md)。`boards/read_pico/board.cmake` 通过 `COMPONENTS` 选择 `main` 与 `platform_read_pico`，由 ESP-IDF 展开依赖；它选择 ESP32-S3、准备 BSP 构建副本、绑定运行时端口的链接并配置 TinyUSB 源文件，默认选项位于同目录的 `sdkconfig.defaults`，补丁位于 `patches/`。`profile.cmake` 由固件和 host 入口共同选择，生成契约的 `build_profile.h`，提供更新板名、渠道与槽大小。构建后核对实际生成的 sdkconfig，不能用 defaults 的改动代替生效配置验证。

## 固件构建入口与证据

产品版本唯一来源为 `firmware/version.txt`，规则见[版本管理](versioning.md)。标准入口每次先重新配置，确保提交号、标签和 dirty 状态进入应用版本；构建完成后核对镜像中的版本，并把版本、构建模式、来源和摘要写入 `provenance.json`。手动调用 IDF 时使用 `idf.py -C firmware reconfigure build`；host 重新构建前先执行下文的 CMake 配置命令。

仓库根目录执行：

```sh
python3 tools/setup_fonts.py
sh tools/build_firmware.sh
```

首次准备 BSP 需要访问 GitHub；脚本将上游检出定位到固定提交，并检查组件差异，不自动清除组件修改。构建脚本没有刷写、擦除或监视串口动作。应用、ELF 与来源记录位于 `firmware/build/`；其他构建模式使用独立目录，见[版本管理](versioning.md)。每批记录实际日志、镜像大小、摘要与应用槽余量，不在本页维护另一份「最新产物」。

固件入口固定引用 Read Pico 板级配置，目前只有这一种板级实现。无参数构建默认读取 `boards/read_pico/sdkconfig.defaults`；其他模式在其后叠加 `firmware/configs/<mode>.defaults`。生成配置仍位于原有构建位置，分区文件仍为 `firmware/partitions.build.csv`。直接使用 IDF 时，显式的 CMake／环境变量 `SDKCONFIG_DEFAULTS` 仍可覆盖默认文件；标准构建脚本继续按项目 defaults 校验生效配置。旧脚本若显式引用 `firmware/sdkconfig.defaults`，需改用新的板级路径。

历史 macOS 构建曾在组件管理器配置阶段因 `psutil` 的 `sysctl()` 查询受沙箱限制而失败。遇到相同错误时先确认失败阶段及执行权限，不以更换依赖或关闭检查规避；该历史故障不表示当前构建必然失败。

## 设备与分区

`firmware/partitions.build.csv` 是全量安装镜像携带的分区表，当前包含两个 4 MiB 应用槽与 otadata；修改构建配置不会迁移已安装设备。首次安装、仅更新应用与校准保护的操作边界统一见[固件开发指南](../../firmware/README.md#硬件与写入边界)。设备身份、分区表、otadata 和写入偏移必须在对应操作中核对，不复用历史设备端口、偏移或授权。

构建、写入摘要校验与真机验收分别记录；面板、输入、保存、睡眠、功耗及网络的待测项以[开发状态](../development-status.md)和[验收清单](acceptance.md)为准。

## host 与交接检查

host 使用本机 C/C++ 编译器与 CMake，不依赖 EIM 的交叉编译器；依赖和测试字体准备见[桌面开发指南](../../host/README.md)与[测试说明](../../tests/README.md)。在仓库根目录执行：

```sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel 4
ctest --test-dir build/host -N
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

ASan/UBSan 通过不等于已检查内存泄漏。须核对实际编译器与运行时的 LSan 支持；既有 macOS AppleClang 21 记录报过 `AddressSanitizer: detect_leaks is not supported on this platform`，不能据此概括所有 macOS 工具链。上述 macOS 命令仅记录 ASan/UBSan 结果；具备 LSan 的工具链应显式使用 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1`。现有 Linux CI 使用独立 Clang sanitizer 构建并保留这些选项，不通过关闭泄漏检查冒充完整通过。

CTest 日志位于 `build/host/Testing/Temporary/LastTest.log` 与 `build/sanitized/Testing/Temporary/LastTest.log`。核对实际注册项目和条件依赖，分别记录 macOS 本机与 Linux CI 结果。历史 Homebrew 动态库链接曾产生系统版本警告，本机运行通过不证明其他 macOS 版本兼容。本次编译警告、测试数量及结果以实际命令输出为准。

`python3 tools/check_handoff.py` 只检查公开文档与示例，不能替代 C++ 测试、固件构建或真机验证。每批结果与未测范围记录在[开发状态](../development-status.md)。

## 桌面预览与位置映射复核

`sh tools/run_host.sh` 准备字体与演示卡、构建并启动桌面阅读器；参数与无窗口模式见[桌面开发指南](../../host/README.md)。沙箱内 SDL 若报 `The video driver did not add any displays`，可使用无窗口模式验证像素或逻辑，桌面窗口需在能够访问显示设备的环境中验证；两种结果分别记录。

真实 CREngine 映射复核入口：

```sh
python3 tools/check_koreader_mapping.py
```

此命令固定下载并构建独立的 GPL 引擎对照工具到 `.local/`，不把 CREngine 链接进阅读器。引擎对照、完整 KOReader 客户端双向接续和 Read Pico Wi-Fi/HTTPS 同步是不同验证范围；方法见[同步说明](sync-runtime.md)，已有结果及未测项查开发状态，不从引擎对照通过推定实际客户端或设备验收通过。

## GitHub Actions

个人 Pro 账户的 Actions 额度由该账户下的私有仓库共享；付费预算与付款状态由账户所有者管理，仓库工作流不修改账户计费设置。远端运行未启动时，本地验证不能记为远端 CI 通过。

`Host checks` 和 `Website build` 保留原有检查名称，PR 更新后均生成检查结果，再由 `tools/ci_changes.py` 根据完整 PR 差异选择耗时步骤。路径判断包含删除与重命名前后的路径；无法取得差异时检查失败，不静默跳过。代码与构建配置变更执行 Debug、独立 Clang ASan/UBSan/LSan 和交接检查；仅文档、实现截图或网站变更跳过 host 构建，交接检查仍执行。新出现且未分类的路径默认执行 host 检查。路径选择回归由 host 工作流单独运行，不计入 CTest 注册数量。

网站、发布说明、OTA 清单脚本及网站或共用路径脚本的改动执行 PR 网站构建与发布固件校验。分支 push 不单独触发工作流；main 仅在上述网站输入变更时构建并部署，发布 Release 和手动部署入口保留。host 合并后不自动重跑，直接推送 main 的代码需要手动补验；合并前应验证包含当前目标分支变更的结果。PR 新提交取消旧检查，正式部署继续串行并保留下载源比较及线上读回检查。COS 返回 403 时，仅允许候选清单与全部镜像均同官网现有副本逐字节一致的页面更新，并明确告警；新固件、内容变化、其他错误及认证发布失败仍阻止部署，不自动恢复桶权限。部署前通过共用路径脚本直接比较构建提交与最新 main 的网站输入：网站输入变化才让位给替代运行，只有文档或固件代码变化时仍完成当前部署；读取比较结果失败时停止部署。

两个工作流继续通过 `[self-hosted, linux, x64, pico-page]` 使用现有私有 runner；是否切换到 GitHub 托管环境及切换时机另行决定，不作为本次整理或发布的前置条件。runner 串行领取任务，离线时排队，不自动回退。host 超时为 20 分钟，网站超时为 25 分钟（包含固件上传及完整公网读回）。系统依赖由管理员预先准备，工作流不执行 apt 或 sudo；host 检查 CMake、C++、Clang、ccache 及 pkg-config 依赖，网站检查 gh 和 curl，缺少依赖时失败。

host 和 COS 发布继续通过 `actions/setup-python@v5` 使用 Python 3.12，不替换系统 Python。host 正文相关检查和网站构建均显式准备 Node 24，防止缺少 Node 时条件测试未注册。COS 依赖安装在本任务 `RUNNER_TEMP/pico-page-cos` 虚拟环境中，不写入系统 Python。

两个工作流均显式启用 checkout 清理，再恢复所需缓存；缓存不保存构建结果、测试结果、网站产物或下载的发布固件。持久运行的私有 runner 不等同于每次新建的隔离环境。

Debug 保持原优化配置；CI sanitizer 使用 Debug 配置及 `-O1 -g`，保留断言、事件跟踪和原有 sanitizer 选项。两种构建通过 CMake compiler launcher 共用 ccache，编译器内容和编译参数参与命中判断，单份本地缓存上限为 256 MiB。缓存只影响编译，完整 CTest 每次仍执行。字体与 FreeType 仅缓存下载输入，`setup_fonts.py` 每次重新核对摘要并按需解包，不缓存测试结果或构建目录。新增缓存使用 GitHub 官方 `actions/cache@v5`，不提高仓库的缓存付费容量上限。

网站 PR 不上传 artifact；通过过期提交判断的部署运行保留网站产物 3 天。既有 artifact 的保留时间不会因工作流修改而自动缩短，清理结果单独记录；Release 固件附件和 Cloudflare 已部署网站不属于 Actions artifact。

## EPUB 分段计时构建

`sh tools/build_firmware.sh stages` 使用独立的 `build/firmware-stages/`，启用开发串口及旧式阶段日志，关闭聚合指标和事件跟踪。身份校验、EPUB 打开、章节解析、图片读取与解码、章节和图片缓存读写记录全部样本（`min_us=0`）；其他既有阶段仍使用各自阈值。日志使用单调墙钟，包含调度和日志开销，不是纯 CPU 时间。`chapter_parse` 包含章节缓存写入，`*_cache_write` 包含序列化或像素复制、校验和及 `*_cache_commit`；commit 包含文件提交协议，不等同于单次物理写卡。父子阶段不得直接相加。串口日志使用现有 `tools/summarize_reader_perf.py` 汇总，并按开书请求范围归属样本。该命令只构建，不刷写设备。
