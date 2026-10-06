# Read Pico 设备验证记录

日期：2026-10-06。用户已授权安装当前设备并自主测试，随后明确取消完整备份。
未提交、push 或发布；未修改 bootloader、分区表、NVS、otadata、工厂 VCOM 或 PMU 校准。

## 安装依据

设备端口 `/dev/cu.usbmodem1101`，ESP32-S3 revision v0.2，内置 8 MiB PSRAM、16 MiB Flash。
Secure Boot 和 Flash Encryption 未启用。使用 esptool 5.3.1 自动进入下载模式。
从 `0x8000` 读取分区表，并由当前 IDF 的 `gen_esp32part.py` 验证：

| 名称 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| nvs | data/nvs | `0x9000` | 24 KiB |
| phy_init | data/phy | `0xf000` | 4 KiB |
| ota_0 | app/ota_0 | `0x10000` | 4 MiB |
| ota_1 | app/ota_1 | `0x410000` | 4 MiB |
| otadata | data/ota | `0x810000` | 8 KiB |

otadata 有效条目为 sequence 1、state 2（VALID），CRC 正确，对应 OTA0。
原应用为 `mr_read_pico`，版本 `v1.1.0-5-ge10b6be`；原 bootloader 与应用均使用 ESP-IDF v6.1。
本地 `partitions.build.csv` 只用于编译，不能用其生成的完整 flash 命令覆盖实际分区。

早期完整读取因 USB 大块传输失败而中止，未得到完整备份。用户取消备份后不再重试。
后续只读取启动槽和应用兼容性所需元数据；原始元数据及私有串口日志均在忽略目录 `.local/device/`。

## 首次安装及启动

`sh tools/build_firmware.sh` 完整链接成功，包含 USB 诊断但尚未包含后续图片和导入增量。
应用 1,916,736 字节；安装副本 `.local/device/install/pico_page-first-boot.bin`，SHA-256：
`a4dba7d399cc81ae088bd7a16e2b4a36b1ec4f24d448592b3ad8591d64a2224f`。

安装命令只写已核验的 OTA0：

```sh
python -m esptool --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 115200 \
  --before usb-reset --after hard-reset write-flash \
  0x10000 .local/device/install/pico_page-first-boot.bin
```

写入成功，esptool 报告 `Hash of data verified`，随后自动重启。
启动日志确认从 `0x10000` 加载 `pico_page`，PSRAM 测试通过，PMU 协议握手成功，
工厂 VCOM 读取成功，SD 卡挂载成功。书库首帧实际驱动返回成功，`clean=1`。
启动后可用内存约 6.34 MB，其中内部可用 83,747 字节；应用任务最低剩余栈超过 12 KiB。
这是实际采样，不是全场景峰值保证。首轮日志见 `.local/device/install/first-boot.log`。

TF 原有 `trend.bin` 与 `trend.bak`；尚无 Books 或 PicoPage 字体目录。未读取或修改这两个文件。

## 诊断连接

发现将 DTR/RTS 直接设为 false 后打开串口，会在本机触发 `USB_UART_CHIP_RESET`。
客户端已采用当前官方 IDF Monitor 的无复位连接顺序：打开前两线设为 true，打开后先释放 RTS，
再释放 DTR。连续两次连接状态的 uptime 从 46,583 ms 增至 49,620 ms，确认重连未重启设备。
接口与输入/保存边界见 [USB 诊断协议](console-runtime.md)。

## 图片与导入版本

第二批完整构建已安装并通过 Flash 摘要验证，仍仅写 OTA0。
镜像 1,944,000 字节，SHA-256：
`0569056a825ffcb8abfad185c325e83f730b4d28b0adab0da9c1797a5150073f`。
日志 `.local/firmware-images-import-final.log` 与 `.local/device/install/images-import-flash.log`。
host CTest 9/9、ASan/UBSan 9/9、交接资料 280 项检查通过；这些不替代新镜像的设备运行结果。

## 配置保存与大文件导入修复

已修复 TOML 已有字符串值定位、打开面板前的外部配置变更保护、恢复默认后继续调整丢失草稿。
重新完整构建后，最新镜像为 1,944,400 字节，SHA-256：
`2cbc884c4ce71c0ee4d53f71568c91a0dbbd66f67692b5362028272b7712aa56`。
仅写 OTA0，Flash 摘要验证成功；日志 `.local/firmware-buffered-console.log`、
`.local/device/install/buffered-console-flash.log`。

早期字体导入暴露逐字节 VFS 读取的吞吐问题，其中一次无新数据超过 30 秒而自动取消。
客户端中断也收到 `aborted=1`，未将部分字体发布为正式文件。
现改为 512 字节接收缓冲，保留跨块解析和每轮一条命令限额；活动导入轮询仍让出一个 tick。
两份 OFL 许可证已成功导入并读回校验，大字体使用修复版重新导入。
完整字体和书籍验证结果仍需以下后续测试，不由已接收字节数推断成功。

## 原生 USB 协议收发

512 字节 VFS 读取提高了吞吐，但一次传输到约 14 MiB 后仍超时并安全取消。
该证据不能独自区分 RX 丢失与 TX 回复丢失。核对本地官方 IDF 后确认日志 VFS 按字节发送，
发送失败时可继续返回原始长度，因此协议改用原生 USB 驱动一次入队整行，并记录入队失败数。
超时记录新增已接收字节数和不完整命令长度，便于区分故障方向；未关闭 CRC 或超时保护。

完整构建和 OTA0 摘要校验通过。最新镜像 1,944,560 字节，SHA-256：
`252040c83737bd5b4a920014496f9a5e82d8ee91cfa1a70722dc3b3db228b29d`。
证据 `.local/firmware-usb-protocol.log`、`.local/device/install/usb-protocol-flash.log`。
原生协议完整资源导入正在验证，结果以正式文件读回摘要为准。

## 尚待验证

画布导出只能证明应用绘图和驱动确认，不能替代屏幕实拍、灰阶/残影/方向、实体触摸和
电源键的人工验收。当前离线验收结果见本页后续记录。

USB Serial/JTAG 的手动浅睡可能需要物理拔插后重新连接，因此无人值守测试不主动进入硬件睡眠。
TF 满卡/掉电、长期耗电、实际 Wi-Fi/HTTPS 服务同步仍未验收。
## 2026-10-06 08:34 交接时状态

完整字体、许可证、《阿Ｑ正傳》和《吶喊》已导入 TF 并读回 CRC 校验通过，日志为
`.local/device/install/import-resources-native-usb.log`。随后重启加载资源，书库显示事务成功。

实际执行 `open Books/lu-xun-aq-25332.epub` 时触发 `InstrFetchProhibited`（PC 0）并自动重启。
原始日志 `.local/device/install/open-diagnosis-raw.log`；崩溃对应 ELF 已保留为
`.local/device/install/pico_page-usb-protocol.elf`。该交接时开书崩溃尚未修复；后续结果如下。

## 开书崩溃根因与内存修复

反汇编确认原 ZIP 解压调用跳到 ROM `tinfl_decompress`（`0x40000828`），而传入的是
项目 miniz 3.1.2 的结构体。Xtensa 编译测得项目结构体为 8,364 字节，ROM 头文件对应
10,992 字节，布局也不同；原回调指针位于被越界覆盖的栈区域。IDF 的 ROM 链接脚本
使用绝对符号赋值，覆盖了项目同名函数。修复在组件构建配置中隔离 inflate/checksum
符号，并将同一配置传给 PNG 调用方；miniz 原件未改，主栈仍为 32 KiB。

`-fstack-usage` 实测 open 栈帧 9,328 字节，parse 1,216 字节，callback 解压 9,664 字节。
这些是静态单帧值，不是调用链峰值。修复版设备最低剩余栈另按真实状态采样。
乐鑫文档 MCP 本轮返回每小时限流；使用本机 IDF 6.1 源码、对应 ELF 及官方
[异常说明](https://docs.espressif.com/projects/esp-idf/en/v5.3.5/esp32s3/api-guides/fatal-errors.html)
核对，未把文档中的一般诊断当作本设备根因证据。

字体对象使用 PSRAM 分配器后，书库内部空闲内存由约 25 KB 增至 133 KB；但章节小对象
仍消耗内部堆。仅把普通分配阈值从 16 KiB 降至 1 KiB，阅读时最大内部块仍只有 992 字节。
最终使用 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`，普通分配优先 PSRAM，保留原 32 KiB
内部预留及显式 DMA/任务分配。未启用 Wi-Fi PSRAM 选项，未修改 TLS 验证或内存策略。

阈值为 0 的设备完整离线脚本已通过开书、K2、连续翻页、排版取消/应用、刷新模式和切书恢复。
两书采样的内部空闲内存最低 149,419 字节，最大块 102,400 字节，最低剩余栈 7,528 字节；
未再出现 TF DMA 分配错误。证据 `.local/device/install/offline-psram-smoke.log` 和
`offline-psram-serial.log`。这些是本次路径的采样，不是所有 EPUB 的峰值承诺。

## 空闲降频与自动休眠镜像

应用 `.local/device/install/pico_page-idle-power.bin`，1,964,144 字节，SHA-256：
`8e050edba93be7d17f3242604f7d4f3a731573664fd920a3611fa953369c165d`，ELF 同目录保存。
同时连接着另一台 ESP32-S3 设备（`/dev/cu.usbmodem101`，USB 序列号 `…:56:C8`），未对其执行任何操作。
写入前用三项独立证据确认目标为 `/dev/cu.usbmodem1101`：ioreg 中该端口的 USB 序列号为
`…:18:B8`；该端口应答 Pico Page 诊断 `status`；esptool `read-mac` 返回 `…:18:B8`，
与此前各次烧录日志一致。写入命令与此前相同，仅 `0x10000`；日志 `idle-power-premac.log`、`idle-power-flash.log`。

诊断输入验证记录在 `.local/device/install/idle-power-validation.log`：开书恢复修订 30 原页；
K3×4、K1×4 往返，各页画布 CRC 往返一致；空闲 5–8 秒后的翻页与 K2 全刷 `frame_ok=1`；
内部空闲约 148 KB、最大内部块 98,304 字节、主任务最低剩余栈 7,536 字节。
这些是诊断注入和画布摘要，不是面板实拍、实体输入或电流测量。

## 字形位图缓存镜像

应用 `.local/device/install/pico_page-glyph-cache.bin`，1,965,408 字节，SHA-256：
`48e79332c22f1790fd65004c0add89497dd733c1d33d4683ddc4a06bc8e456c6`，ELF 同目录保存。
写入前由脚本核对 `/dev/cu.usbmodem1101` 的 USB 序列号和 esptool `read-mac`，均为 `…:18:B8`，
任一不符即中止；另一台设备（`/dev/cu.usbmodem101`）未被操作。仅写 `0x10000`，摘要校验成功，
日志 `glyph-cache-flash.log`。翻页计时 `latency-glyph-cache.log`：页面准备 0.35–1.02 秒（缓存预热中）、
已缓存页 0.09–0.10 秒，面板呈现约 0.99 秒；改前同脚本为 1.27 + 0.99 秒（`latency-idle-power.log`）。
以上为诊断注入和画布摘要，不是面板实拍或实体输入验收。

## 最终离线候选与重启验收

最终应用为 `.local/device/install/pico_page-offline-final.bin`，1,956,464 字节，SHA-256：
`95cc9b7072fe2e4b3f3179acb7505de8ff7ec67a4ea52d38797a1f536a9bb95b`。
对应 ELF 同目录保存；完整构建 `.local/firmware-metrics-cache.log`，应用槽写入与摘要验证
`.local/device/install/offline-final-flash.log`。仍只写 `0x10000`，未执行生成的完整 flash 命令。

`.local/device/install/final_validation.py` 连续验证通过：

- 当前保存页重新打开；K3→K1 回到同一画布；正文 K2 全刷不改变画布或进度修订。
- 排版预览取消不改原页；有单书覆盖时保存全局默认返回工具并保持本书排版。
- 无单书覆盖的《吶喊》继承全局默认，应用后改变排版但不改变当前来源位置的保存版本。
- 两书切换恢复；目录打开与 K2 返回；USB 硬复位后从书库重新打开恢复。
- 重启前后《阿Ｑ正傳》的保存修订号均为 24，正文 CRC32 均为 `d1b7e837`；
  重启后的 K2 仍保持相同画布与修订号，驱动确认 `clean=1`、`purpose=2`。

完整结果 `.local/device/install/final-validation.log`、原始串口 `final-validation-serial.log`。
较早脚本曾使用不同分页历史作比较，另一次脚本间翻页发生时有人操作设备，用户已确认；这些中断不计为通过。
随后只读空闲检查的 9 个采样跨越 40.972 秒，帧号、修订和 CRC 均未变化。
最终连续验收在设备静置时完成，没有为排除人工输入修改触摸逻辑。

字宽缓存使用最多 1,024 个条目。本轮当前会话 K3 为正文准备 2.430 秒、呈现等待 0.988 秒；
K1 为 2.064 + 0.989 秒。首次开章仍可能更慢，不能承诺所有页面都达到相同速度。
缓存改前/改后的完整 host 内容对照见 graphics-runtime；不把不同翻页历史下的上一页
当作同一页做性能或像素比较。

最终画布已导出至 `artifacts/runtime/read-pico-offline.pgm` 并转换为同名 PNG，尺寸 684 × 1216。
逐图检查中文正文和页脚未见裁字；该图来自设备应用画布，不是面板光学照片。
导出完成后串口进程退出，设备停留在已保存的《阿Ｑ正傳》正文页。

以上使用设备真实应用、TF 文件和显示驱动，经诊断输入执行；不替代物理目视、实体输入、
灰阶/残影、睡眠及掉电验收。用户选择暂不配置网络，Wi-Fi/HTTPS 与设备双向接续留待后续。
