# Read Pico 板级构建

本目录定义当前唯一支持的 Read Pico 构建配置。

| 文件 | 职责 |
|---|---|
| `board.cmake` | 选择 ESP32-S3 和运行时实现、校验 BSP、准备补丁副本，并在 ESP-IDF 注册组件后配置端口消费者的链接和 TinyUSB 源文件 |
| `profile.cmake` | 固件和 host 共用的板名、更新清单地址与目标 OTA 槽大小；实际分区仍须运行时核对 |
| `bsp-revision.txt` | BSP 完整 commit；下载脚本和 CMake 共用同一版本 |
| `esp-idf-revision.txt` | 正式打包使用的 ESP-IDF 完整 commit |
| `sdkconfig.defaults` | Read Pico 固件的现有默认配置，包含硬件与产品运行参数 |
| `patches/` | BSP 补丁、辅助头及正式打包使用的 ESP-IDF 链接脚本补丁 |

从仓库根目录继续使用 `sh tools/build_firmware.sh`，其他模式使用原有参数。下载的 BSP 保存在 `.local/read_pico_firmware`，补丁副本仍在相应构建目录的 `pico_bsp/`。分区文件保留在 `firmware/partitions.build.csv`，运行时适配仍在 `firmware/components/platform_read_pico/`。

构建入口和配置优先级见[环境说明](../../docs/development/environment.md)，依赖来源及许可见 [BSP 审查](../../docs/development/bsp-review.md)。本目录不提供其他设备驱动，也不改变面板波形、工厂校准或实际设备分区。
