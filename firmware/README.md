# 固件开发指南

Pico Page 的 ESP-IDF 工程，目标为 Read Pico（ESP32-S3）。普通安装与更新请使用[官方安装页](https://picopage.wulu.zone/install/)；本页用于从源码构建和设备开发。

## 环境与构建

使用已有的 ESP-IDF v6.1、Python 3.11+ 和 Git。在仓库根目录执行：

```sh
python3 tools/setup_fonts.py
sh tools/build_firmware.sh
```

资源准备脚本下载固定版本的完整字体与 FreeType，并核对摘要，文件保存在 `.local/`。构建脚本优先使用已激活的 `idf.py`，否则通过 EIM 调用已安装的 v6.1；不会安装或升级全局工具链。首次构建需访问 GitHub 下载固定提交的 BSP。

应用产物为 `firmware/build/pico_page.bin`，同目录包含 ELF 和 `provenance.json` 来源记录。脚本每次重新配置并核对镜像版本，只构建，不连接或烧录设备。版本来源是 [`version.txt`](version.txt)，构建模式与发布产物规则见[版本管理](../docs/versioning.md)。

Read Pico BSP 固定在 `.local/read_pico_firmware`，提交与组件许可见 [BSP 审查](../docs/bsp-review.md)。补丁应用到构建副本，不直接修改下载的上游检出。工具链定位、sdkconfig 与已知环境差异见[环境记录](../docs/environment.md)。

## 硬件与写入边界

| 项目 | 当前目标 |
|---|---|
| 设备 | Read Pico，ESP32-S3，16 MiB Flash、8 MiB PSRAM |
| 显示 | 4.7 英寸墨水屏，按 684 × 1216 竖屏使用，16 级灰阶 |
| 输入 | 触摸屏、K1/K2/K3 电容触摸键、电源键 |
| 数据 | FAT 文件系统 TF 卡；配置与进度以卡上普通文件为准 |

[`partitions.build.csv`](partitions.build.csv) 是全量安装镜像携带的分区表，包含两个 4 MiB 应用槽和 otadata。修改此表不会迁移已安装设备；现有设备需重新整片安装才会采用新布局。

- **首次安装**：按 D41 擦除内部闪存，从 `0x0` 写入包含 bootloader、分区表和应用的全量镜像。原固件、NVS 和其他内部数据会被清除，TF 卡不受影响。单独的 `pico_page.bin` 不是全量镜像。
- **仅更新应用**：只适用于已核对分区与本项目兼容的设备。写入前读回实际分区表和 otadata，确认当前启动槽与目标偏移；联网更新后设备可能从 `ota_1` 启动。不要直接执行构建日志中的默认烧录命令，也不要复用历史设备记录的偏移。
- **校准**：不得改写工厂 VCOM/PMU 校准；VCOM 读取失败时保持面板关闭。整片闪存安装不包含 PMU 中的校准数据。

安装与联网更新的验收范围见[验收清单](../docs/acceptance.md)和[开发状态](../docs/development-status.md)。[设备检查记录](../docs/device-validation.md)只描述对应设备和当次操作，不是其他设备的通用写入指南。

## 开发与验证

应用入口与桌面版共用阅读、文档解析、排版、存储和同步组件；板级实现接入显示、输入、TF 卡、PMU 和网络。职责与所有权见[架构](../docs/architecture.md)，资源和生命周期约束见[嵌入式开发规则](../docs/embedded-development.md)。

修改固件前阅读 [AGENTS.md](../AGENTS.md)，按其中的验证表执行 host 回归、必要的 sanitizer 检查和固件构建。涉及显示、输入、电源或网络的改动另需真机验收。CI 的路径选择和部署规则见[GitHub Actions](../docs/environment.md#github-actions)，CI 不构建固件，也不覆盖真机行为。

[桌面开发](../host/README.md) · [测试说明](../tests/README.md) · [平台实现](../docs/platform-runtime.md) · [文档导航](../docs/README.md)
