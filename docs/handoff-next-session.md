# Pico Page 后续开发交接

> 当前增量：原稿还原与全局 K2 候选已获用户明确授权烧录，OTA0 写入与摘要校验成功。最新验收结果见开发状态。
> 最新构建、USB 已安装版本和本轮验证以 docs/development-status.md 顶部为准；下方离线镜像记录是较早批次。

最后更新：2026-10-06（Asia/Singapore）。本轮开书崩溃修复与设备离线阅读验收已完成。用户明确选择「暂不配置，先完成离线阅读」；设备 Wi-Fi/HTTPS 与 KOReader 双向接续暂缓。完整变更和验证边界见 [开发状态](development-status.md) 与 [设备检查记录](device-validation.md)。

## 目标和授权

- 第一版目标仍是 EPUB 离线阅读与 KOReader 双向位置接续；当前优先完成离线使用。
- 沿用原设计，不重设计。K2 在所有可交互页面全刷当前画面；面板关闭或取消使用屏幕控件（2026-10-06 更新）。
- 用户已授权当前设备应用烧录及自主测试，并明确取消完整 Flash 备份。
- 不提交、不推送、不发布、不创建 worktree；保留 main 中所有未提交文件。
- 不改 bootloader、分区表、NVS、otadata、OTA1 或工厂校准；不读取或推测私人凭据。
- 先读 AGENTS.md、decisions.md、development-status.md。不得把历史失败状态当作当前状态。

## 当前安装与设备状态

- 串口 `/dev/cu.usbmodem1101`；操作前重新确认，同一时刻只使用一个串口客户端。
- ESP32-S3 rev v0.2、8 MiB PSRAM、16 MiB Flash；安全启动和 Flash 加密未启用。
- 实测分区：nvs `0x9000/24K`、phy `0xf000/4K`、ota_0 `0x10000/4M`、ota_1 `0x410000/4M`、otadata `0x810000/8K`。
- 有效应用槽为 OTA0，现有 bootloader 为 IDF v6.1。仅向 `0x10000` 写入应用，禁止直接运行生成的完整 flash 命令。
- 已安装 `.local/device/install/pico_page-offline-final.bin`，1,956,464 字节。SHA-256：`95cc9b7072fe2e4b3f3179acb7505de8ff7ec67a4ea52d38797a1f536a9bb95b`。对应 ELF 同目录保存；日志 `.local/device/install/offline-final-flash.log` 确认摘要校验成功。
- 最后停在《阿Ｑ正傳》已保存正文页；修订号 24、画布 CRC32 `d1b7e837`。重启后重新开书及 K2 全刷均保持这两个值；保存状态为成功。
- 最后采样内部空闲 149,595 字节，最大内部块 102,400 字节，PSRAM 空闲 4,516,048 字节，主任务剩余栈 7,608 字节。这是该页面采样，不是所有书籍的峰值保证。
- 最终串口验收和画布导出进程均已退出。没有本轮子代理或后台设备任务。

## 已完成的修复

- 崩溃根因为 ESP32-S3 ROM 的同名 `tinfl_decompress` 覆盖项目 miniz；两者解压结构体 ABI 不一致。项目在 ESP 构建中为相关 miniz 符号添加 `pico_` 前缀，并禁用不需要的压缩 API；第三方源码未修改。原始证据保留在 `open-diagnosis-raw.log` 和 `pico_page-usb-protocol.elf`，不再是当前固件故障。
- FreeType 使用 PSRAM 分配器；普通分配改为 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`。内部预留仍为 32 KiB、主任务栈仍为 32 KiB；原 TF DMA 分配失败未在修复后的离线路径复现。
- 工厂默认字体路径改为实际导入的 `fonts/LXGWWenKai-Regular.ttf`。
- 全局排版保存按有效配置反馈：单书覆盖继续生效时不重排，继承默认时应用新排版。
- 新增最多 1,024 项的字宽缓存；字体关闭时清空，字号属于缓存键。当前页实测翻页约 3.1–3.4 秒，首次开章可更慢。
- 内置提示字体更新为 410 字符；未改原设计文件或输入动作映射。

## 当前批次验证结果

| 验证 | 结果与证据 |
|---|---|
| host 构建与 CTest | 9/9；`.local/host-metrics-cache.log` |
| ASan/UBSan 构建与 CTest | 9/9；`.local/sanitized-metrics-cache.log` |
| PSRAM 字体分配器专项 | 真实字体、中文绘制、OOM、realloc 失败保留原块、零尺寸释放及字体重开；`.local/font-psram-check/sanitized.log` |
| 字宽缓存等价性 | 两本原 EPUB × 三档字号，共 2,060 页，分页边界与像素摘要逐项一致；`.local/font-psram-check/pages-before.log`、`pages-after.log` |
| ESP-IDF 完整构建 | 成功；`.local/firmware-metrics-cache.log` |
| 基础设备阅读 | 开书、连续翻页、排版取消/应用、刷新模式、切书恢复；`.local/device/install/offline-psram-smoke.log` |
| 最终固件连续设备验收 | K1/K3、K2、排版取消、全局覆盖/继承、目录返回、重启同页恢复；`.local/device/install/final-validation.log` |
| 当前设备画布 | `artifacts/runtime/read-pico-offline.pgm` 和 PNG，684 × 1216；中文正文与页脚未见裁字 |

较早脚本间的意外翻页发生时有人操作设备，用户已确认；没有据此修改输入逻辑。之后 9 个只读采样跨越 40.972 秒，帧号、修订和 CRC 均稳定；最终连续验收通过。设备动作由诊断接口注入。画布导出不能替代面板实拍、灰阶/残影、实体触摸/按键、睡眠或 TF 掉电验收。

## TF 资源已完成，不要重复导入

`.local/device/install/import-resources-native-usb.log` 已确认导入与读回 CRC：

| TF 路径 | 字节 | CRC32 |
|---|---:|---|
| PicoPage/fonts/LXGW-OFL.txt | 4448 | 977ffeed |
| PicoPage/fonts/Noto-OFL.txt | 4301 | 898314c0 |
| PicoPage/fonts/LXGWWenKai-Regular.ttf | 24744500 | e8b2c11f |
| PicoPage/fonts/NotoSansSC-Regular.otf | 8331336 | ed5f3a2a |
| Books/lu-xun-aq-25332.epub | 101741 | 9b75b424 |
| Books/lu-xun-nahan-27166.epub | 181683 | 4659dfeb |

原有 `trend.bin` / `trend.bak` 未读取或改动。公开 EPUB 来源见 [书籍记录](public-books.md)。当前全局默认字号为 40 px，《阿Ｑ正傳》的单书字号覆盖为 40 px，《吶喊》继承全局默认。用户未配置网络及服务账户，本轮未创建 network.toml/sync.toml。

## 后续边界

1. 根据用户实际使用反馈验证物理显示、实体触摸/按键及阅读体验；无人协助时不要进入可能需要物理重连的硬件睡眠。
2. 用户决定配置网络后，再测试设备 Wi-Fi/HTTPS 和双向位置接续。保留 TLS 证书验证；不能把 HTTP 成功等同于接续成功。
3. 官方完整 KOReader macOS 客户端此前通过本地服务六组双向精确位置及重启恢复检查，见 sync-runtime.md；这不代表设备或个人外部账户通过。
4. 无线导入、格子书架、目录折叠、删除、电量状态栏、持久离线 outbox 等仍有缺口，见 development-status.md。PDF、Karakeep、批注/词典、插件、统计、多主题、自定义按键与完整 OTA 仍按既定决定后置。
5. 远端 CI、全新官方工具链复现、总许可证与二进制分发义务尚未完成，不发布。

## 工具与执行入口

```sh
cmake --build build/host -j 8
ctest --test-dir build/host --output-on-failure
cmake --build build/sanitized -j 8
ctest --test-dir build/sanitized --output-on-failure
sh tools/build_firmware.sh > .local/firmware-next.log 2>&1
python3 tools/check_handoff.py
git diff --check
```

- EIM `/opt/homebrew/bin/eim`；IDF `/Users/wulu/.espressif/v6.1/esp-idf` 有用户局部修改，保持原样。
- Python/esptool `/Users/wulu/.espressif/tools/python/v6.1/venv/bin/python`。
- 应用安装命令：`python -m esptool --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 115200 --before usb-reset --after hard-reset write-flash 0x10000 <新的应用bin>`。
- `tools/device_console.py` 实现不重置打开及诊断协议。先读协议，不能随意重试可变操作。
- `.local/device/install/final_validation.py` 是本轮一次性验收脚本，会操作书籍及排版；后续运行前检查当前设备与用户操作状态。
- 原设计 49 个文件保持原样。工作目录大量新增实现尚未提交，不执行 reset/clean。
