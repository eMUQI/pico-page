# Read Pico BSP 审查与构建锁

当前板级构建入口为 `boards/read_pico/board.cmake`，版本锁为同目录的 `bsp-revision.txt`，补丁和辅助头位于 `boards/read_pico/patches/`。下方历史记录中的 `firmware/patches/` 为迁移前路径；补丁内容、顺序、上游提交及许可证保持原样。

核查日期：2026-10-05。范围是 G0 的 BSP 编译集成，不是设备验收或发布许可结论。

## 来源与复用范围

官方仓库：[MindReset/read_pico_firmware](https://github.com/MindReset/read_pico_firmware)。锁定 commit：`5f3406deec33304de8801046db583dec7c49e55b`（2026-10-06 由 `28cde682a4468a581c278761922724f57d976418` 升级，见下方「VCOM 门控」）。`tools/setup_bsp.sh` 下载完整源码到忽略目录，固件只引用下面九个组件。下载原件不修改；构建副本应用 checked SD release 和 epdiy 欠载诊断/清屏检查补丁，见下方修改记录；没有纳入上游 `main/`、工厂校准、字体、图片、书库、网络传书和搜索代码。

以该 commit 的各组件 `CMakeLists.txt`、`idf_component.yml`、`LICENSE` 和实现文件核对；组件版本标签不能代替实际源码锁。下面的 tree SHA 都来自同一完整 commit。

| 组件 | 直接用途与依赖 | 组件许可 | Git tree SHA |
|---|---|---|---|
| `read_pico` | 板级装配；下列八组件、FatFs、GPIO/I2C/LEDC/RMT/SDMMC、bootloader/spi_flash/FreeRTOS | Apache-2.0 | `670835c4b03a0e9d7abe4d924ce5a3fea0f9e154` |
| `read_pico_pmu` | CW32L010 协议；I2C、FreeRTOS、esp_rom | Apache-2.0 | `82d8de1f7f02d3fe6a3fe8d4fcdaf2e9484cb43c` |
| `epdiy` | 显示渲染；LCD、ADC、timer、GPIO/RMT/DMA/I2C、HAL/SoC/ROM | LGPL-3.0-or-later | `f9253919fc1d99d0307cc880c4f3710094c7f76c` |
| `e0470_epaper_waveform` | 面板波形与裁剪；epdiy | Apache-2.0 | `0bb7bc191e5b10423833b334409222366ef775d9` |
| `cst836u` | 触摸；GPIO/I2C、FreeRTOS | Apache-2.0 | `acbf8164180ee8341f03dbc0e51f7cf16f50447c` |
| `sc7a20h` | 加速度计；GPIO/I2C、timer、FreeRTOS、hw_support | Apache-2.0 | `a150458837afe3930fd4257d4ccd797316655efe` |
| `fca9555` | IO 扩展；GPIO/I2C | Apache-2.0 | `c8648becc3983120fb6000de5a48ee00c10a81b0` |
| `sy7636a` | 显示电源；GPIO/I2C、timer、FreeRTOS、esp_rom | Apache-2.0 | `b29885c27b115bfeb092d305bfcbe67e51ff05aa` |
| `pwm_audio` | 蜂鸣器后端；GPIO/GPTimer/LEDC、heap | Apache-2.0 | `c95f0d30b6b8c78f05a47b520709529d49d7bbc2` |

`epdiy` 是上游在 v2.0.0 基础上裁剪、修改的版本；保留的 LCD 渲染路径、差分优化和队列修正记录在其 `LICENSE`。不能仅依据仓库顶层 Apache-2.0 把它重新标成 Apache。`pwm_audio` 是 Espressif 组件的裁剪副本，原始版权声明在该组件中保留。

ESP-IDF 锁为 v6.1 commit `fff9895c82d744c7237be8847347bdd1b07c6643`，本机既有修改见环境记录。IDF 的 GPIO、I2C、LCD、SPI Flash、HAL 等 Espressif 原始代码主要是 Apache-2.0；同时核对了 `docs/en/COPYRIGHT.rst` 与以下非统一许可路径：

- `components/fatfs/src/ff.c`：FatFs R0.16，ChaN 的 BSD 风格许可，保留版权、条件和免责声明。
- `components/sdmmc/sdmmc_common.c`：OpenBSD 来源的 ISC 风格许可；SDMMC 其他 Espressif 文件另有 Apache-2.0 标头。
- `components/freertos/FreeRTOS-Kernel/LICENSE.md`：MIT。
- `components/xtensa/include/xtensa`：Tensilica MIT；heap 中 TLSF 为 BSD-3-Clause。
- `components/esp_libc/COPYING.NEWLIB`、`COPYING.picolibc`：按文件划分的多项宽松许可，不能合并标记为 Apache-2.0。
- `components/mbedtls/mbedtls/LICENSE`：Apache-2.0 OR GPL-2.0-or-later，本轮采用 Apache-2.0；由 IDF 系统组件间接引入。

源码版权以文件标头为准。[IDF 版权清单](https://github.com/espressif/esp-idf/blob/v6.1/docs/en/COPYRIGHT.rst)提供 SDK 汇总；本轮没有复制或再分发 SDK。最终二进制发布前仍需依据链接产物补齐随包许可与工具链运行库通知。

## 初始化与数据边界

`read_pico_init()` 会初始化 EPD、启动 SD 探测、设置 18 MHz 显示时钟及 1290 mV 默认 VCOM，随后初始化 PMU、触摸和加速度计。官方应用在 `main/app_main.c::resolve_vcom_at_boot()` 另外调用 `read_pico_pmu_vcom_get()`，再将读到的工厂值交给显示驱动；这一步不在 BSP 初始化内部。因此当前入口不调用 `read_pico_init()`，也不复用其默认校准回退路径。后续适配必须先验证 PMU 读取与失败处置，不调用 `read_pico_pmu_vcom_set()` 或工厂标定入口。

`read_pico_sd.c` 的普通挂载路径不自动格式化，但它包含显式格式化入口，并会建立演示资源目录。本轮没有调用探测、挂载或格式化函数；真实 TF 接入要明确控制这些副作用。`read_pico` 的 `WHOLE_ARCHIVE` 用于保留 Flash HPM 覆盖表；本轮保留上游构建定义，不启用依赖具体 Flash 芯片的 120 MHz 实验配置。

编译配置采用上游 `sdkconfig.ci` 的 ESP32-S3、16 MB Flash、Octal PSRAM 80 MHz、USB Serial/JTAG 等构建基线，不把它作为当前硬件的测量结果。上游产品配置中的 120 MHz 时序、波形和 VCOM 未移植为本项目运行策略。

## VCOM 门控（上游 `5f3406d`，2026-10-06）

上游提交「feat: enhance VCOM control during scanning」只修改 `components/read_pico/read_pico_board.c`（18 行），其余八个组件的 tree SHA 不变。内容：

- 面板电源轨上电后，IO 扩展器上的 `VCOM_EN` 跟随扫描模式位 `ep_mode`：开始扫描时拉高，扫描结束后拉低；断电前恢复原来的控制方式。上游说明：电源轨保持上电而 VCOM 长开，会把画面慢慢压黑，在阳光下表现为发灰。
- `VCOM_EN` 由低变高后等待 2 毫秒再开始扫描。

对本项目的影响：

- 本项目每次刷新后保持电源轨 8 秒（`platform.cpp` 的 `rails_idle_us`），此前这 8 秒内 VCOM 一直开启，正是上游修正的情况。
- epdiy 的 `lcd_do_update()` 每次刷新只进入一次扫描模式，全刷时每个清屏脉冲再各进入一次，因此每次刷新只多等几个 2 毫秒。
- 不涉及 VCOM 电压值：本项目仍从 PMU 读取工厂值交给显示驱动，不调用 `read_pico_pmu_vcom_set()`。
- `firmware/patches/read-pico-sd-release.patch` 修改的是 `read_pico_sd.c`，与本次改动无关，仍可直接应用。

显示效果和刷新耗时的变化只能在真机上确认。

## 分区与发布待办

上游 `partitions_16M.csv` 是单 factory 应用加数据区，没有 OTA 双槽。这只说明所锁源码的分区安排，不说明用户设备当前分区。Pico Page 没有复制该 CSV；本地单应用分区仅用于构建。现已读回设备为两个 4 MiB OTA 槽，与上游及本地 CSV 均不同；当前启动槽和完整备份仍未核验，见 device-validation.md。

未来分发 Apache-2.0 组件时应附完整许可、保留版权和适用 NOTICE，并标明修改。分发包含 epdiy 的静态链接固件时，应按 LGPL-3.0 第 4 节准备 LGPL/GPL 文本、库来源与修改记录、可重新链接所需源码或对象及必要安装信息；不能只放一个上游链接就宣称完成义务。本项目整体许可证仍待决定，本轮只在本地构建。条款依据：[Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0)、[LGPL-3.0-or-later 文本](https://spdx.org/licenses/LGPL-3.0-or-later.html)。

## EPD 欠载诊断适配（2026-10-09）

仍锁定 `5f3406deec33304de8801046db583dec7c49e55b`，不修改下载目录。构建将 `epdiy` 复制至 `build/pico_bsp/epdiy`，应用 `firmware/patches/epdiy-underrun-diagnostics.patch`：在已有欠载分支增加 4 个原子计数/行号字段，区分无效生产者、空队列和扫描未消费完；ISR 只记录数值，汇合生产者后由调用任务输出日志。新增同步 `epd_clear_checked()` 返回清屏扫描的累计错误，避免后续绘制重置错误而误认清屏成功。诊断与检查接口不改变波形、预填、时钟或调度。原 LGPL-3.0-or-later 许可和分发义务保留，源码补丁与锁定来源一并保留；组件版本未变，依赖锁只更新构建路径变化对应的 manifest hash。


## 2026-10-11 · 实验水波纹逐行相位补丁

用户明确授权在构建副本加入 `firmware/patches/epdiy-row-phases.patch`，顺序在原欠载补丁之后；官方锁定和下载原件不变。新增单相差分扫描的逐行 LUT 借用接口，只用于 Read Pico 倒置竖屏的 16 条带水波纹；关闭时原查找路径不变。生产者汇合后清除上下文，调用方每拍返回后清除全局借用，失败不提交 back buffer，仍由平台原清屏恢复处理。来源、许可与裁剪范围见 [依赖记录](dependency-review.md#2026-10-11--文字转页与水波纹候选)。该候选的真机欠载、灰阶、残影与时延待验收，不把编译或故障替身通过视为面板验收。

## RTC 响应校验与接入（2026-10-11）

实现前核对锁定官方源码的 `main/apps/app_pmu.c`、`main/factory/pmu_selftest.c`、`components/read_pico_pmu/read_pico_pmu.c` 与协议文档。官方演示通过 `TIME_SYNC` 写 UTC 秒并用 `TIME_GET` 读回；自检检查 `time_ok`、`time_synced` 和秒差。Pico Page 使用相同公开协议，时钟来源采用真实 SNTP 回包，不移植工厂自检的时间来源。官方文档的 PMU 复位失效与 LSI 漂移限制保留。

`firmware/patches/read-pico-rtc-response.patch` 仅修改构建副本的 `read_pico_pmu.c`：每次 `TIME_GET` 开始清除 `time_ok`；成功响应必须恰为协议规定的 8 字节才更新快照，否则返回 `ESP_ERR_INVALID_RESPONSE`。防止旧快照或 CRC 正确但长度不足的响应被当作本次有效时间。官方 commit、下载原件和组件 tree SHA 不变。组件仍为 Apache-2.0，保留源文件版权与许可证；新增适配由项目实现，不引入新依赖或复制工厂校准代码。


## 2026-10-26 · 睡眠 IO 停放移植

保留 BSP 锁定 `5f3406deec33304de8801046db583dec7c49e55b` 及原有五个构建补丁。`platform_read_pico/sleep_io.cpp` 参考上游 [read_pico_board.c](https://github.com/MindReset/read_pico_firmware/blob/a2a63a6a2c606540be72156a6567392d948d65fc/components/read_pico/read_pico_board.c) 的睡眠引脚、MODE／Port1 停放与关机 CW_INT 设置；来源 commit 为 `a2a63a6a2c606540be72156a6567392d948d65fc`，原组件 Apache-2.0，保留版权与许可标识，沿用平台目录完整 LICENSE。另加 `read-pico-sd-busy.patch`，在 `state_lock` 内读取 BSP 的 `probe_state`，仅暴露探卡／挂载／卸载是否进行中；不代表应用文件工作已停止，也不预约卡所有权。浅睡在此状态为忙时拒绝交接，拔卡不能把忙状态掩盖。该补丁不改变原有五个补丁或原件；同为 Apache-2.0。没有引入官方 main 应用、CPU 私有休眠接口、音频或默认锁屏策略。

本项目通过原有 GPIO、FCA9555 及 CST836U 公开接口实现：停放前读取运行寄存器快照，恢复时先释放 CW_INT，再恢复输出；全部扩展器恢复完成后才复位触摸。与上游不同，任一步写入或恢复失败都保留可重试状态，既有采样锁排除 BSP 输出缓存与直接寄存器写入交错。未修改 VCOM、波形、校准或 PMU 协议。适配和资源边界见 [运行时说明](platform-runtime.md#睡眠-gpio-与外设恢复)，实测状态见开发记录。
