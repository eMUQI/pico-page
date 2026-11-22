# Read Pico BSP 来源与构建补丁

板级构建入口为 `firmware/board/board.cmake`，版本锁为 `firmware/board/bsp-revision.txt`，补丁位于 `firmware/board/patches/`。`tools/setup_bsp.sh` 把完整官方源码下载到忽略目录，下载原件不修改；构建时复制所需组件并应用补丁。本页记录来源、许可和每个补丁的作用；补丁的应用顺序以 `board.cmake` 为准。

## 来源与组件

官方仓库：[MindReset/read_pico_firmware](https://github.com/MindReset/read_pico_firmware)，锁定 commit `5f3406deec33304de8801046db583dec7c49e55b`。固件只引用下列九个组件，不纳入上游 `main/`、工厂校准、字体、图片、书库、网络传书和搜索代码。组件版本标签不代替源码锁，下表 tree SHA 都来自同一 commit。

| 组件 | 用途 | 许可 | Git tree SHA |
|---|---|---|---|
| `read_pico` | 板级装配 | Apache-2.0 | `670835c4b03a0e9d7abe4d924ce5a3fea0f9e154` |
| `read_pico_pmu` | CW32L010 PMU 协议 | Apache-2.0 | `82d8de1f7f02d3fe6a3fe8d4fcdaf2e9484cb43c` |
| `epdiy` | 显示渲染 | LGPL-3.0-or-later | `f9253919fc1d99d0307cc880c4f3710094c7f76c` |
| `e0470_epaper_waveform` | 面板波形与裁剪 | Apache-2.0 | `0bb7bc191e5b10423833b334409222366ef775d9` |
| `cst836u` | 触摸 | Apache-2.0 | `acbf8164180ee8341f03dbc0e51f7cf16f50447c` |
| `sc7a20h` | 加速度计 | Apache-2.0 | `a150458837afe3930fd4257d4ccd797316655efe` |
| `fca9555` | IO 扩展器 | Apache-2.0 | `c8648becc3983120fb6000de5a48ee00c10a81b0` |
| `sy7636a` | 显示电源 | Apache-2.0 | `b29885c27b115bfeb092d305bfcbe67e51ff05aa` |
| `pwm_audio` | 蜂鸣器后端 | Apache-2.0 | `c95f0d30b6b8c78f05a47b520709529d49d7bbc2` |

`epdiy` 是上游在 v2.0.0 基础上裁剪、修改的版本，修改记录在其 `LICENSE` 中；不能依据仓库顶层的 Apache-2.0 把它标成 Apache。`pwm_audio` 是 Espressif 组件的裁剪副本，保留原始版权声明。

ESP-IDF 锁为 v6.1，完整提交见 `firmware/board/esp-idf-revision.txt`。IDF 的 Espressif 代码主要是 Apache-2.0，以下路径另有许可：FatFs（ChaN BSD 风格）、`sdmmc_common.c`（ISC 风格）、FreeRTOS（MIT）、Xtensa 头文件（MIT）与 TLSF（BSD-3-Clause）、`esp_libc` 的 newlib／picolibc（按文件的多项宽松许可）、mbedTLS（Apache-2.0 OR GPL-2.0-or-later，本项目采用 Apache-2.0）。源码版权以文件头为准，汇总见 [IDF 版权清单](https://github.com/espressif/esp-idf/blob/v6.1/docs/en/COPYRIGHT.rst)。

## 分发义务

分发 Apache-2.0 组件时附完整许可，保留版权和适用的 NOTICE，并标明修改。分发静态链接了 epdiy 的固件时，按 LGPL-3.0 第 4 节提供 LGPL/GPL 文本、库来源与修改记录、可重新链接所需的源码或对象及必要的安装信息；只放上游链接不算完成义务。最终发布前须依据链接产物补齐随包许可与工具链运行库通知。项目整体许可见[许可说明](../LICENSING.md)，其他依赖见[依赖审查](dependency-review.md)。

## 初始化边界

`read_pico_init()` 会设置 1290 mV 默认 VCOM；官方应用另在 `main/app_main.c::resolve_vcom_at_boot()` 读取 PMU 工厂值。本项目不调用 `read_pico_init()`，直接组合公开入口并读取工厂 VCOM，不调用 `read_pico_pmu_vcom_set()` 或工厂标定入口，见[平台适配](platform-runtime.md#初始化和校准)。`read_pico_sd.c` 包含格式化入口并会建立演示目录，本项目只使用普通挂载路径。`read_pico` 的 `WHOLE_ARCHIVE` 用于保留 Flash HPM 覆盖表，不启用依赖具体 Flash 芯片的 120 MHz 配置。编译配置以上游 `sdkconfig.ci` 为构建基线（ESP32-S3、16 MB Flash、Octal PSRAM 80 MHz、USB Serial/JTAG），上游产品配置中的时序、波形和 VCOM 不作为本项目的运行策略。

锁定的 commit 已包含上游「扫描期间才打开 VCOM_EN」的修正：面板电源轨保持上电而 VCOM 长开会让画面逐渐变暗，因此 `VCOM_EN` 跟随扫描模式开关，打开后等待 2 ms 再扫描。

## 构建补丁

| 补丁 | 修改对象 | 作用 | 许可 |
|---|---|---|---|
| `read-pico-sd-release.patch` | `read_pico_sd.c` | 新增 `read_pico_sd_release()`：调用方关闭全部文件后卸载并释放卡，返回卸载错误；卸载失败时保持挂载状态 | Apache-2.0 |
| `read-pico-sd-busy.patch` | `read_pico_sd.c` | 在 `state_lock` 内暴露探卡、挂载或卸载是否进行中；浅睡在忙时拒绝交接。不代表应用文件工作已停止 | Apache-2.0 |
| `read-pico-rtc-response.patch` | `read_pico_pmu.c` | 每次 `TIME_GET` 先清除 `time_ok`，只有恰为 8 字节的成功响应才更新快照，否则返回 `ESP_ERR_INVALID_RESPONSE`，避免旧快照或长度不足的响应被当作有效时间 | Apache-2.0 |
| `read-pico-touch-reset-selftest.patch` | `read_pico_board.c` | 扩展器自检改为翻转 MODE、保持 TP_RST 为高，避免在正式复位前额外复位触摸控制器；SY_EN、VCOM_EN 保持低，正式复位时序不变 | Apache-2.0 |
| `epdiy-underrun-diagnostics.patch` | epdiy 渲染 | 欠载分支增加原子计数与行号，区分无效生产者、空队列和扫描未消费完；新增 `epd_clear_checked()` 返回清屏扫描的累计错误 | LGPL-3.0-or-later |
| `epdiy-row-phases.patch` | epdiy 渲染 | 逐行相位查找表的借用接口，用于水波纹翻页；关闭时原查找路径不变 | LGPL-3.0-or-later |
| `epdiy-phase-spans.patch` | epdiy 渲染 | 按列段为每行相位减去延迟，用于斜掠翻页；借用缓冲在绘制返回前不可修改 | LGPL-3.0-or-later |
| `epdiy-feed-start.patch` | epdiy LCD 输出 | 每个相位让两个供数任务经静态 `xEventGroupSync()` 汇合后再开扫，修复启动竞态；不增加堆、任务或队列 | LGPL-3.0-or-later |
| `epdiy-footer-hold.patch` 与 `epdiy-unchanged-pixels.h` | epdiy 渲染 | 页脚和非正文更新中相同像素的驱动位清零，复用 `epd_apply_line_mask_VE` | LGPL-3.0-or-later |
| `esp-idf-ext-ram-linker.patch` | ESP-IDF 链接脚本 | 限制外部 RAM 段位置计数器的下界，用于正式构建环境，见[发布工具链](environment.md#发布工具链) | Apache-2.0 |

这些补丁不改变波形数据、电源时序或 VCOM 电压；显示效果和耗时只能在真机上确认。

## 睡眠 IO 停放

`platform_read_pico/sleep_io.cpp` 参考上游 commit `a2a63a6a2c606540be72156a6567392d948d65fc` 的 [read_pico_board.c](https://github.com/MindReset/read_pico_firmware/blob/a2a63a6a2c606540be72156a6567392d948d65fc/components/read_pico/read_pico_board.c)：睡眠引脚、MODE／Port1 停放与关机时的 CW_INT 设置。原组件为 Apache-2.0，文件保留版权与许可标识。与上游不同，本项目通过 GPIO、FCA9555 和 CST836U 的公开接口实现，停放前读取寄存器快照，任一步写入或恢复失败都保留可重试状态，见[平台适配](platform-runtime.md#睡眠-gpio-与外设恢复)。没有引入官方应用、CPU 私有休眠接口、音频或锁屏策略。
