# Pico Page

Read Pico 上的个人阅读固件，使用 ESP-IDF。当前已有可运行的桌面初版；最新设备固件已完整构建并写入，
使用同一套原生 EPUB、分页、界面、文件存储与同步逻辑。

**不是“全部功能已完成”，物理目视、触摸、睡眠和设备 Wi-Fi/HTTPS 同步仍待验收。**
首版已实际启动，确认 PMU、工厂 VCOM 读取、SD、PSRAM 和书库全刷完成；最新镜像已通过写入摘要校验。
完整字体和两本 EPUB 已导入并校验。设备开书、翻页、排版、K2 全刷、切书及重启恢复已通过诊断接口验收。
当前优先离线阅读，Wi-Fi 和 KOSync 配置暂缓；用户已取消完整备份。
操作范围和证据见 [设备检查记录](docs/device-validation.md)。

当前工作区支持文字及 PNG/JPEG 图片 EPUB 阅读，也支持 UTF-8、GB18030 TXT 离线阅读。
TXT 可带 UTF-8 BOM，使用本地进度；UTF-16 和 TXT 的 KOReader 位置接续暂不支持。无 BOM 的 TXT 优先按 UTF-8 读取，失败后尝试 GB18030；其他编码可能误判或无法打开。
本次 TXT 固件已写入 Read Pico；用户已确认 GB18030 原书正常显示中文并可翻页，重启后进度恢复仍待确认。
损坏或不支持的 EPUB 图片有明确占位提示。
KOReader 精确位置映射仅支持已验证的 XHTML 范围；不支持的内容不会用百分比猜测跳转。
官方完整 KOReader 客户端已通过本地测试服务的六组双向接续与重启恢复检查，
这不等于 Read Pico 与外部服务的实际同步验收。本次 TXT 改动尚未提交、推送或发布。

## 直接运行桌面版

本机已具备所需工具及依赖。在仓库目录运行：

```sh
sh tools/run_host.sh
```

这会准备 `.local/card` 演示卡、下载并核验固定字体、构建并启动窗口。已有书籍和配置不会被覆盖。
将自己的 EPUB、UTF-8 或 GB18030 TXT 放入 `.local/card/Books`，在书库打开；也可直接指定：

```sh
sh tools/run_host.sh --book 'Books/Pico Page 演示.epub'
```

鼠标模拟触摸。`← / 1` 为 K1，`R / 2 / Esc` 为 K2，`→ / 3` 为 K3，空格模拟休眠/唤醒。
K2 在所有可交互页面始终全刷当前画面；点击正文中部打开工具；关闭/取消使用屏幕控件。
安装条件、设备卡准备与同步配置见 [运行说明](docs/runtime.md)。

## 当前阅读流程

- 文件夹书库、最近阅读、原 EPUB 打开、完整行分页、PNG/JPEG 图片独立分页、跨章翻页、目录和内部链接返回。
- 现有视觉稿的正文、工具、目录、排版预览/应用/取消、刷新、设置、导入说明及休眠界面。
- TF 普通文件保存与恢复；设置保留注释，检测外部编辑，支持单书覆盖和阅读默认。
- 手动检查远端位置、确认跳转、明确确认后上传已保存版本；异步取消和旧结果隔离。
- Read Pico 屏幕、工厂 VCOM 读取、触摸按键、TF、浅睡、Wi-Fi 与验证证书的 HTTPS 适配。

后置项仍包括 PDF、Karakeep、批注/词典、插件、统计、多主题、自定义按键和完整 OTA。
Wi-Fi 与 KOSync 当前通过本地配置文件设置，尚无设备上的输入配置流程。

## 构建与验证

```sh
python3 tools/setup_fonts.py
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel 4
ctest --test-dir build/host --output-on-failure
sh tools/build_firmware.sh
python3 tools/check_handoff.py
```

固件脚本只构建，不刷写。`firmware/partitions.build.csv` 仅为本地 3 MiB 应用构建配置，
与实测设备的两个 4 MiB OTA 槽不同，不能直接套用生成的完整烧录命令。
本次只写入已确认的 `ota_0` 应用区域，保留原 bootloader、分区表、NVS、otadata 和校准数据。

最新完整固件构建成功；host 与 ASan/UBSan CTest 各 9/9 通过，交接检查 284 项通过。
完整镜像摘要和验证边界见 [开发状态](docs/development-status.md)。

- [开发状态与实际结果](docs/development-status.md)
- [Wi-Fi 传书](docs/wifi-transfer.md)
- [公开 EPUB 样本与验证](docs/public-books.md) · [设备诊断接口](docs/console-runtime.md)
- [环境记录](docs/environment.md)
- [产品决定](docs/decisions.md) · [架构](docs/architecture.md) · [数据存储](docs/storage.md)
- [界面实现约束](docs/ui-implementation.md) · [验收清单](docs/acceptance.md)
- [依赖审查](docs/dependency-review.md) · [同步范围及引擎验证](docs/sync-runtime.md)

原设计及历史快照保持原样。截图等实现证据保存在忽略的 `artifacts/`；个人 EPUB、凭据、设备备份不进入仓库。
