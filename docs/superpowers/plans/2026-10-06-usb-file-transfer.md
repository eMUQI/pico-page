# USB File Transfer Implementation Plan

> 历史设计或执行计划，保留用于追溯。本文任务指令不自动授权当前操作；现行决定和实现状态以仓库根目录 AGENTS.md、docs/decisions.md 与 docs/development-status.md 为准。

> 后续决定：2026-10-06 用户将 K2 更新为所有可交互页面全刷；下文涉及 K2 取消/关闭的历史约定已被 docs/decisions.md 覆盖。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在现有设置中提供默认关闭的 TF 卡 USB 文件传输，退出后重启恢复阅读和原 USB 烧录连接。

**Architecture:** App 负责保存、关闭文件和传输界面；主循环串行协调显示、网络、导入及 USB 所有权。平台层独占 SD 块设备与 TinyUSB 生命周期，传输期间不挂载应用文件系统；退出通过重启恢复。

**Tech Stack:** C++17、ESP-IDF 6.1、官方 esp_tinyusb MSC、固定 Read Pico BSP、现有 CMake/CTest。

**Spec:** [已确认设计](../specs/2026-10-06-usb-file-transfer-design.md)

## Global Constraints

- 开关只表示当前运行状态，不写入 NVS 或 TF 卡；每次正常启动均关闭。
- 首版仅 MSC，不增加 CDC 复合设备。退出前提示电脑安全弹出；K2 取消确认。
- 正常阅读中的 K2 全刷不变；传输期间禁用阅读、同步、普通文件导入和自动休眠。
- 不格式化、不重建分区；不烧写 eFuse，不改变 bootloader、分区、NVS 或工厂校准。
- 保留原设计和用户未提交改动；当前 checkout 内实施，不创建 worktree；不提交、不推送、不发布。
- 原始私人导出、设备文件及日志不提交。新增测试复用现有测试目标，只覆盖新增风险。

## Review Focus

- 文件保存/卸载失败：不能启动 MSC 或误报成功；任务 1、2 检查。
- 字体、EPUB、网络或诊断仍持有 TF 文件：交接后不得访问；任务 2、3 检查。
- 电脑 suspend、拔线、移卡和未弹出：不自动恢复应用挂载，不误报写入完成；任务 1、4 检查。
- 控制器切换或初始化失败：提供可用错误页面及重启路径，正常重启恢复串口；任务 3、4 检查。
- 电脑移动/删除书籍或编辑配置：重启不使用旧内存覆盖外部修改；任务 4 检查。

## Task 1: 可验证的存储交接与 MSC 平台实现

**Files:** 修改 `firmware/CMakeLists.txt`、`firmware/components/platform_read_pico/CMakeLists.txt`、`firmware/components/platform_read_pico/platform.cpp`、`firmware/components/platform_read_pico/include/pico_page/platform.h`、`docs/dependency-review.md`；新增 `firmware/components/platform_read_pico/usb_storage.cpp`、该组件 `idf_component.yml`、`firmware/patches/read-pico-sd-release.patch`。

**Interfaces:** 平台层提供 `bool start_usb_storage(std::string& error)`、`bool stop_usb_storage(std::string& error)`；均由应用任务调用。start 成功后 `card_root()` 为 nullptr；stop 等待在途 I/O 完成后返回，调用方随后重启，不重新挂载。新增 BSP 窄接口 `esp_err_t read_pico_sd_release(void)` 传播卸载错误；失败后保留所有权状态，禁止另行初始化 SD。

- [x] 核对官方 esp_tinyusb 的 ESP-IDF 6.1 支持、MSC 回调、退出等待接口及许可证，固定精确版本并生成依赖锁；核对板级 USB/VBUS 检测依据，记录在依赖审查中，不猜 GPIO。
- [x] 添加受控 BSP 构建副本补丁流程：验证固定上游 commit 和原件无修改，构建目录生成副本、检查并应用补丁，只改变 SD release 接口和卸载错误传播。不得改 `.local` 原件或跳过既有完整性检查。
- [x] 实现 checked release：拒绝探测任务忙碌，关闭失败不得清空 card 指针或返回 ESP_OK；导出接口供平台使用。
- [x] 实现 start：卸载成功后按已核实的板级 SD 配置初始化原始块设备，接入官方 MSC；失败返回具体错误，保持独占状态；已取得的资源统一在退出时释放，不挂载 FAT，不格式化。
- [x] 实现有界 MSC 读写：检查扇区范围、偏移、长度和溢出；使用适合 DMA 的缓冲；部分扇区请求按所选官方 API 契约处理。移卡后本次 USB 会话永久报告介质不可用，不接纳重新插入的卡。
- [x] 实现 stop：停止接受新请求，等待在途操作结束，再断开 USB 并释放资源；等待失败不自动重启。USB suspend 不能触发 stop 或本地重挂载。
- [x] 对实际引入的范围检查纯逻辑在既有 host 目标加入边界断言；设备故障行为在任务 4 验证，不以 mock 声称硬件正确。
- [x] 运行 `sh tools/build_firmware.sh`，要求退出码 0；核对固件大小、依赖锁和补丁差异，保存日志。此阶段不安装。

## Task 2: 设置开关、保存与文件释放

**Files:** 修改 `firmware/components/reader_app/include/pico_page/app.h`、`firmware/components/reader_app/app.cpp`、`tests/app_test.cpp`、`tools/setup_fonts_subset.py` 及既有内置字体生成产物。

**Interfaces:** 新增 `enum class UsbTransferAction { None, Start, Exit }`；`ReaderApp` 提供 `UsbTransferAction take_usb_transfer_action()`、`bool prepare_usb_transfer()`、`void show_usb_transfer(bool active, const std::string& error)`。prepare 只在正常且无显示/网络事务时保存并释放卡文件；失败保持安全状态。show 只使用内置字体和内存数据，不访问 TF。UI 返回动作，由主循环决定是否可执行。

- [x] 在 `app_test` 写失败用例：保存失败使 prepare 返回 false、保存修订不虚增；显示未完成/网络活动时拒绝；正常准备后可将 host 测试卡目录移走而传输页和退出确认仍能呈现。
- [x] 运行 `cmake --build build/host -j 8 && ctest --test-dir build/host -R app --output-on-failure`，确认新增断言首先失败。
- [x] 沿用设置中的文件导入入口展示「USB 文件传输」开关及说明；启动前不显示开启状态。退出确认默认取消，K2 只取消确认；确认后产生 Exit 动作。
- [x] 实现 prepare：保存最后呈现位置，取消同步候选，关闭 EPUB 和 UI/正文字体句柄，清理可导致卡访问的缓存/回调。传输状态阻止 open_book、网络完成、睡眠和普通导航进入读卡路径。
- [x] 添加断言：K2 不产生 Exit、重复动作不重复准备、Start 失败显示关闭及错误、传输状态 open_book 被拒绝、旧网络完成不改变状态或写卡。
- [x] 更新提示字库覆盖新增中文，运行相同 host 检查直至通过；检查设置入口截图；传输页的脱卡渲染由 App 测试验证，物理目视仍待确认，保留原稿不变。

## Task 3: 主循环交接、串口停止及退出重启

**Files:** 修改 `firmware/main/app_main.cpp`、`firmware/components/reader_console/console.cpp`、`firmware/components/reader_console/include/pico_page/console.h`、`docs/console-runtime.md`。

**Interfaces:** 消费任务 1、2 的接口；新增 `bool pico_page::console::shutdown(std::string& error)`，拒绝活动导入并停止原 USB Serial/JTAG 驱动。关闭失败时不得启动 TinyUSB。禁用诊断编译配置下接口安全返回。

- [x] 主循环处理 Start 前检查 network_busy、pending_frame、transfer_active、physical_source 和 sleep_requested，忙碌时提示重试并消费本次请求，不在后台自动进入。
- [x] 保存/释放前先确认过渡画面成功显示；prepare 成功后停串口并调用 start。成功后显示「USB 文件传输中」；失败显示错误和重启入口，不重新进入可能已失效的 ReaderApp 会话。
- [x] 传输状态走独立主循环分支：继续现有触摸/按键序号与显示确认，仅允许传输页操作；禁止网络派发、诊断轮询、自动休眠和应用卡状态扫描。电源键不触发休眠。
- [x] 用户确认 Exit 后调用 stop，成功才 `esp_restart()`；失败留在错误页，不声称退出成功。保留硬件 BOOT/复位恢复路径。
- [x] 运行 host 与 sanitized 构建/CTest，以及完整固件构建；均要求退出码 0。检查正常模式 K2/保存/同步既有回归结果。
- [x] 审查所有绕过主循环的 TF 调用，确认无并发读写；记录未能在 host 覆盖的硬件项，交付候选镜像及摘要。

## Task 4: 真机验收与已授权目录整理

**Files:** 更新 `docs/development-status.md`、`docs/device-validation.md`、`docs/console-runtime.md`、新增 `docs/usb-storage-runtime.md`；私人日志和摘要放 `.local/usb-storage-check/`。

- [x] 重新确认端口、设备身份和安装授权，核对 OTA0 槽；在允许安装的范围内只写应用，不使用整片或分区烧录命令。
- [ ] 默认模式检查正常阅读、串口诊断、TF 配置及 K2 全刷。进入 USB 后确认电脑枚举出的确为目标 TF 盘，记录卷身份与容量。
- [ ] 仅在专用测试目录新增小文件、读回摘要、重命名并再次校验；安全弹出后设备退出重启，检查串口恢复、TF 挂载和文件变化可见。
- [ ] 抽测保存失败拒绝进入、忙碌拒绝、退出确认取消、USB suspend 不回挂、初始化失败恢复；移卡/异常拔线只在无写入时检查，不对私人 TF 主动做破坏性掉电测试。
- [ ] 单书保存后进入 USB，电脑修改测试书配置，再退出重启确认新配置生效且进度未丢；移动/删除专用测试书验证安全返回书库。验证默认 USB 烧录恢复，不能用仅出现串口代替完整烧录验证。
- [ ] 基础磁盘读写和退出流程通过后，使用已有私有 manifest 将正式书籍从 `Books/Kindle/` 移到 `Books/`，网页收藏和 news 移到 `Articles/`；对应 `.picopage/` 随书移动。同名目标存在时先核实，不覆盖。字体及其他资源不动。
- [ ] 核对全部移动文件摘要、源/目标清单及抽测已有阅读进度；安全弹出后退出重启。记录实际完成和未验证项。
- [x] 执行 `python3 tools/check_handoff.py`、`git diff --check`，检查个人数据未进入 tracked diff。更新开发状态和设计实现状态，不提交或推送。

## Self-review

已覆盖默认关闭、退出重启、TF 独占、失败阻断、字体文件释放、串口停止、输入隔离、原 K2 行为及既有目录整理。平台特有的 USB/SD 故障与物理操作留在真机验收表，不用额外通用状态框架或模拟成功替代。

## 实际执行偏差与待验收

- 未使用子代理，按用户要求在当前工作目录执行。
- 初始化失败后保留独占错误页，资源由退出流程统一清理；不在失败分支尝试恢复原阅读会话。
- 目录整理已完成并安全弹出：19 本书、52 篇网页收藏、7 个单书数据目录；21 个字体不变。70 个文件移动，1 个 macOS 无法按名称打开的韩文组合字符文件从原始导出复制为短名，旧条目保留。
- 1 MiB 文件写入、fsync、重命名及摘要读回通过；首次访问等待系统可移动宗卷授权，不能作为性能数据。
- 物理退出重启、重启后读回和正常烧录恢复仍待设备操作。故障注入、移卡和异常拔线未对私人 TF 卡执行。
