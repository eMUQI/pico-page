# Read Pico BSP 审查与构建锁

核查日期：2026-10-05。范围是 G0 的 BSP 编译集成，不是设备验收或发布许可结论。

## 来源与复用范围

官方仓库：[MindReset/read_pico_firmware](https://github.com/MindReset/read_pico_firmware)。
锁定 commit：`28cde682a4468a581c278761922724f57d976418`。
`tools/setup_bsp.sh` 下载完整源码到忽略目录，固件只引用下面九个组件。
本项目没有修改这些组件；没有纳入上游 `main/`、工厂校准、字体、图片、书库、网络传书和搜索代码。

以该 commit 的各组件 `CMakeLists.txt`、`idf_component.yml`、`LICENSE` 和实现文件核对；
组件版本标签不能代替实际源码锁。下面的 tree SHA 都来自同一完整 commit。

| 组件 | 直接用途与依赖 | 组件许可 | Git tree SHA |
|---|---|---|---|
| `read_pico` | 板级装配；下列八组件、FatFs、GPIO/I2C/LEDC/RMT/SDMMC、bootloader/spi_flash/FreeRTOS | Apache-2.0 | `beb4a781efefef3907751c6e11f4dbc95d9b92bb` |
| `read_pico_pmu` | CW32L010 协议；I2C、FreeRTOS、esp_rom | Apache-2.0 | `82d8de1f7f02d3fe6a3fe8d4fcdaf2e9484cb43c` |
| `epdiy` | 显示渲染；LCD、ADC、timer、GPIO/RMT/DMA/I2C、HAL/SoC/ROM | LGPL-3.0-or-later | `f9253919fc1d99d0307cc880c4f3710094c7f76c` |
| `e0470_epaper_waveform` | 面板波形与裁剪；epdiy | Apache-2.0 | `0bb7bc191e5b10423833b334409222366ef775d9` |
| `cst836u` | 触摸；GPIO/I2C、FreeRTOS | Apache-2.0 | `acbf8164180ee8341f03dbc0e51f7cf16f50447c` |
| `sc7a20h` | 加速度计；GPIO/I2C、timer、FreeRTOS、hw_support | Apache-2.0 | `a150458837afe3930fd4257d4ccd797316655efe` |
| `fca9555` | IO 扩展；GPIO/I2C | Apache-2.0 | `c8648becc3983120fb6000de5a48ee00c10a81b0` |
| `sy7636a` | 显示电源；GPIO/I2C、timer、FreeRTOS、esp_rom | Apache-2.0 | `b29885c27b115bfeb092d305bfcbe67e51ff05aa` |
| `pwm_audio` | 蜂鸣器后端；GPIO/GPTimer/LEDC、heap | Apache-2.0 | `c95f0d30b6b8c78f05a47b520709529d49d7bbc2` |

`epdiy` 是上游在 v2.0.0 基础上裁剪、修改的版本；保留的 LCD 渲染路径、差分优化和队列修正
记录在其 `LICENSE`。不能仅依据仓库顶层 Apache-2.0 把它重新标成 Apache。
`pwm_audio` 是 Espressif 组件的裁剪副本，原始版权声明在该组件中保留。

ESP-IDF 锁为 v6.1 commit `fff9895c82d744c7237be8847347bdd1b07c6643`，
本机既有修改见环境记录。IDF 的 GPIO、I2C、LCD、SPI Flash、HAL 等 Espressif 原始代码主要是
Apache-2.0；同时核对了 `docs/en/COPYRIGHT.rst` 与以下非统一许可路径：

- `components/fatfs/src/ff.c`：FatFs R0.16，ChaN 的 BSD 风格许可，保留版权、条件和免责声明。
- `components/sdmmc/sdmmc_common.c`：OpenBSD 来源的 ISC 风格许可；SDMMC 其他 Espressif 文件另有 Apache-2.0 标头。
- `components/freertos/FreeRTOS-Kernel/LICENSE.md`：MIT。
- `components/xtensa/include/xtensa`：Tensilica MIT；heap 中 TLSF 为 BSD-3-Clause。
- `components/esp_libc/COPYING.NEWLIB`、`COPYING.picolibc`：按文件划分的多项宽松许可，不能合并标记为 Apache-2.0。
- `components/mbedtls/mbedtls/LICENSE`：Apache-2.0 OR GPL-2.0-or-later，本轮采用 Apache-2.0；由 IDF 系统组件间接引入。

源码版权以文件标头为准。[IDF 版权清单](https://github.com/espressif/esp-idf/blob/v6.1/docs/en/COPYRIGHT.rst)
提供 SDK 汇总；本轮没有复制或再分发 SDK。最终二进制发布前仍需依据链接产物补齐随包许可与工具链运行库通知。

## 初始化与数据边界

`read_pico_init()` 会初始化 EPD、启动 SD 探测、设置 18 MHz 显示时钟及 1290 mV 默认 VCOM，
随后初始化 PMU、触摸和加速度计。官方应用在 `main/app_main.c::resolve_vcom_at_boot()`
另外调用 `read_pico_pmu_vcom_get()`，再将读到的工厂值交给显示驱动；这一步不在 BSP 初始化内部。
因此当前入口不调用 `read_pico_init()`，也不复用其默认校准兜底路径。
后续适配必须先验证 PMU 读取与失败处置，不调用 `read_pico_pmu_vcom_set()` 或工厂标定入口。

`read_pico_sd.c` 的普通挂载路径不自动格式化，但它包含显式格式化入口，并会建立演示资源目录。
本轮没有调用探测、挂载或格式化函数；真实 TF 接入要明确控制这些副作用。
`read_pico` 的 `WHOLE_ARCHIVE` 用于保留 Flash HPM 覆盖表；本轮保留上游构建定义，
不启用依赖具体 Flash 芯片的 120 MHz 实验配置。

编译配置采用上游 `sdkconfig.ci` 的 ESP32-S3、16 MB Flash、Octal PSRAM 80 MHz、USB Serial/JTAG
等构建基线，不把它作为当前硬件的测量结果。上游产品配置中的 120 MHz 时序、波形和 VCOM 未移植为本项目运行策略。

## 分区与发布待办

上游 `partitions_16M.csv` 是单 factory 应用加数据区，没有 OTA 双槽。
这只说明所锁源码的分区安排，不说明用户设备当前分区。Pico Page 没有复制该 CSV；
本地单应用分区仅用于构建。现已读回设备为两个 4 MiB OTA 槽，
与上游及本地 CSV 均不同；当前启动槽和完整备份仍未核验，见 device-validation.md。

未来分发 Apache-2.0 组件时应附完整许可、保留版权和适用 NOTICE，并标明修改。
分发包含 epdiy 的静态链接固件时，应按 LGPL-3.0 第 4 节准备 LGPL/GPL 文本、库来源与修改记录、
可重新链接所需源码或对象及必要安装信息；不能只放一个上游链接就宣称完成义务。
本项目整体许可证仍待决定，本轮只在本地构建。
条款依据：[Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0)、
[LGPL-3.0-or-later 文本](https://spdx.org/licenses/LGPL-3.0-or-later.html)。
