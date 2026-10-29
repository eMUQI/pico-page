# 开发环境

本文维护工具链定位、构建与检查方法及已知环境限制。具体批次的命令、产物和验收结果见[开发状态](../development-status.md)，设备实测见[设备检查记录](device-validation.md)。整理前的主机版本、镜像摘要、早期分区与写入记录保存在[环境历史快照](../archive/environment-20261025.md)，不作为当前机器状态或操作授权。

## 工具链与依赖定位

固件使用已有的 ESP-IDF v6.1；`firmware/dependencies.lock` 锁定 IDF 6.1.0。构建脚本优先使用已激活的 `idf.py`，标准构建在未激活 IDF 时可通过 PATH 中的 EIM 调用已有 v6.1；其他构建模式需要先激活 IDF。不安装或升级全局工具链。基础依赖及资源准备见[固件开发指南](../../firmware/README.md)与[桌面开发指南](../../host/README.md)。

构建前从 EIM 安装记录确认实际路径、版本和激活脚本，不照抄历史主机路径。在仓库根目录可使用：

```sh
eim --version
eim --log-file .local/eim.log --do-not-track true list
eim --log-file .local/eim.log --do-not-track true run "idf.py --version" v6.1
```

激活后检查 `$IDF_PATH` 对应检出的 commit、已跟踪差异和未跟踪文件，并记录实际 Python、CMake、Ninja 与交叉编译器版本。EIM 日志放到忽略目录 `.local/`。2026-10-06 的 macOS 环境存在 IDF 链接脚本修改和额外 requirements 文件，具体内容见[历史工具链记录](../archive/environment-20261025.md#本机工具链)；使用同一安装时须重新核对，不能将历史成功结果描述为干净官方工具链的本次验证。

BSP 在 `tools/setup_bsp.sh` 与固件 CMake 中按完整 commit 固定，来源、补丁与许可见 [BSP 审查](bsp-review.md)。目标芯片和默认选项由 `firmware/sdkconfig.defaults` 等配置定义；构建后核对实际生成的 sdkconfig，不能用 defaults 的改动代替生效配置验证。

## 固件构建入口与证据

产品版本唯一来源为 `firmware/version.txt`，规则见[版本管理](versioning.md)。标准入口每次先重新配置，确保提交号、标签和 dirty 状态进入应用版本；构建完成后核对镜像中的版本，并把版本、构建模式、来源和摘要写入 `provenance.json`。手动调用 IDF 时使用 `idf.py -C firmware reconfigure build`；host 重新构建前先执行下文的 CMake 配置命令。

仓库根目录执行：

```sh
python3 tools/setup_fonts.py
sh tools/build_firmware.sh
```

首次准备 BSP 需要访问 GitHub；脚本将上游检出定位到固定提交，并检查组件差异，不自动清除组件修改。构建脚本没有刷写、擦除或监视串口动作。应用、ELF 与来源记录位于 `firmware/build/`；其他构建模式使用独立目录，见[版本管理](versioning.md)。每批记录实际日志、镜像大小、摘要与应用槽余量，不在本页维护另一份「最新产物」。

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

CTest 日志位于 `build/host/Testing/Temporary/LastTest.log` 与 `build/sanitized/Testing/Temporary/LastTest.log`。核对实际注册项目和条件依赖，分别记录 macOS 本机与 Linux CI 结果。历史 Homebrew 动态库链接曾产生系统版本警告，本机运行通过不证明其他 macOS 版本兼容。历史编译警告及测试数量保存在[环境快照](../archive/environment-20261025.md#host-与交接检查)，本次结果以实际命令输出为准。

`python3 tools/check_handoff.py` 只检查交接资料，不能替代 C++ 测试、固件构建或真机验证。每批结果与未测范围记录在[开发状态](../development-status.md)。

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

两个工作流通过 `[self-hosted, linux, x64, pico-page]` 路由至本仓库专用的 `homelab` runner；当前主机为 Ubuntu 26.04 x86_64，以独立普通用户运行，不授予 sudo 或 Docker 管理权限。单个 runner 串行领取任务，离线时任务排队，不自动回退至 GitHub 托管机器。host 超时为 20 分钟，网站超时为 25 分钟（包含固件上传及完整公网读回）。工作流不执行 apt、不修改主机包源；host 检查 CMake、C++、Clang、ccache 及 pkg-config 依赖，网站检查 gh 和 curl，缺少依赖时失败。

管理员在 Ubuntu 主机上预先安装系统依赖，不能在 runner 用户中执行：

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake clang libclang-rt-dev ccache pkg-config \
  libsdl2-dev libssl-dev libcurl4-openssl-dev python3 python3-venv \
  git gh curl ca-certificates tar gzip unzip
```

host 和 COS 发布继续通过 `actions/setup-python@v5` 使用 Python 3.12；runner 用户须能写入 `RUNNER_TOOL_CACHE`，Python 发行包须匹配 Ubuntu 版本，来源见 [actions/python-versions](https://github.com/actions/python-versions/blob/main/versions-manifest.json)。不替换系统 Python。host 正文相关检查和网站构建均显式准备 Node 24，防止新机器缺少 Node 时条件测试未注册。COS 依赖安装在本任务 `RUNNER_TEMP/pico-page-cos` 虚拟环境中，不写入系统 Python 或共享工具缓存中的 Python 环境。

两个工作流均显式启用 checkout 清理，移除上次任务留下的未跟踪及忽略文件，再恢复所需缓存，避免复用旧构建目录、网站产物或下载的固件。缓存不保存构建结果；长期运行的 runner 不视为每次新建的隔离环境。runner 仍可访问主机允许的网络服务，专用用户不能代替内网访问控制。

Debug 保持原优化配置；CI sanitizer 使用 Debug 配置及 `-O1 -g`，保留断言、事件跟踪和原有 sanitizer 选项。两种构建通过 CMake compiler launcher 共用 ccache，编译器内容和编译参数参与命中判断，单份本地缓存上限为 256 MiB。缓存只影响编译，完整 CTest 每次仍执行。字体与 FreeType 仅缓存下载输入，`setup_fonts.py` 每次重新核对摘要并按需解包，不缓存测试结果或构建目录。新增缓存使用 GitHub 官方 `actions/cache@v5`，不提高仓库的缓存付费容量上限。

网站 PR 不上传 artifact；通过过期提交判断的部署运行保留网站产物 3 天。既有 artifact 的保留时间不会因工作流修改而自动缩短，清理结果单独记录；Release 固件附件和 Cloudflare 已部署网站不属于 Actions artifact。

## EPUB 分段计时构建

`sh tools/build_firmware.sh stages` 使用独立的 `build/firmware-stages/`，启用开发串口及旧式阶段日志，关闭聚合指标和事件跟踪。身份校验、EPUB 打开、章节解析、图片读取与解码、章节和图片缓存读写记录全部样本（`min_us=0`）；其他既有阶段仍使用各自阈值。日志使用单调墙钟，包含调度和日志开销，不是纯 CPU 时间。`chapter_parse` 包含章节缓存写入，`*_cache_write` 包含序列化或像素复制、校验和及 `*_cache_commit`；commit 包含文件提交协议，不等同于单次物理写卡。父子阶段不得直接相加。串口日志使用现有 `tools/summarize_reader_perf.py` 汇总，并按开书请求范围归属样本。该命令只构建，不刷写设备。
