# 设备固件

ESP-IDF 6.1 / ESP32-S3 工程，实际接入 Read Pico 显示、输入、TF、PMU 和联网适配。应用入口与 host 共用 reader_app / core / store / document / graphics / sync。

在仓库根目录执行：

```sh
python3 tools/setup_fonts.py
sh tools/build_firmware.sh
```

官方 BSP 固定在 `.local/read_pico_firmware`，首次构建下载；现有 EIM / ESP-IDF 被复用。脚本不访问设备或刷写。`partitions.build.csv` 仅供本地构建，不能直接写入未经核验的设备。工厂 VCOM 读取失败时面板保持关闭，不写 PMU 校准。

[运行说明](../docs/runtime.md) · [平台实现](../docs/platform-runtime.md) · [实际验证状态](../docs/development-status.md)
