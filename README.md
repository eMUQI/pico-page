# Pico Page

项目仍处于开发阶段。本页提供构建入口，不是经过完整验证的通用安装指南；浏览器安装入口已开放，只有用户的一次使用反馈，没有按验收表逐项验证；项目总许可证尚待确定。历史设备记录中的写入偏移和授权不可直接用于其他设备。

Pico Page 是运行在 Read Pico 墨水屏阅读器上的个人阅读固件，基于 ESP-IDF 开发。固件直接读取 TF 卡上的 EPUB 和 TXT，并可与 KOReader 互相接续 EPUB 的阅读位置。

项目主要供作者自用，按开源项目的方式维护。第一版只适配 Read Pico。

> **项目状态**：开发中。测试版 `0.2.0-beta.5` 已于 2026-10-16 发布，第一版（`1.0.0`）尚未发布。固件已能在 Read Pico 上运行，但第一版验收（EPUB 离线阅读与 KOReader 双向位置接续）尚未完成。host 测试通过或固件构建成功不代表真机行为已验收。各项功能的实际验证结果见[开发状态](docs/development-status.md)。

当前测试版本为 `0.2.0-beta.5`（2026-10-16 发布，预发布）。`0.2.0-beta.1` 无法验证更新服务器的证书，不能联网更新，需要在安装页重新安装。版本来源与发版规则见[版本管理](docs/versioning.md)。

## 功能

### 阅读

- 直接打开 TF 卡上的 EPUB，不需要在电脑上预先转换。
- 支持 UTF-8（可带 BOM）和 GB18030 编码的 TXT。
- 按完整行分页，支持跨章翻页、目录、内部链接跳转与返回。
- 显示 EPUB 中的 PNG、JPEG 图片。图片与前后的文字可以排在同一页，文字不环绕图片；只含一张图片的章节使用整个正文区域。长按图片进入图片查看。
- 可调整字体、字号、行距等排版参数。调整时先预览，应用后才保存。单本书的设置可以覆盖阅读默认值。
- 提供清晰、均衡、快速三档刷新模式，自动全刷间隔可调。

### 书库与文件

- 书库首页直接显示 `Books/` 中的书和文件夹，支持列表和封面卡片两种视图。
- 最近阅读的书从已保存的位置继续。
- 阅读进度、设置和单书记录都是 TF 卡上的普通文件，可以在电脑上查看、备份和手动修改。
- 传书有三种方式：USB 文件传输、经设备热点的 Wi-Fi 传书、经已保存 Wi-Fi 的传书。

### KOReader 位置接续

- 使用 KOSync 协议，与 KOReader 互相接续同一本 EPUB 的阅读位置。
- 同步由用户手动发起。设备不会自动跳转，也不会自动上传。
- 设备只在精确映射成功时提供跳转，不按百分比推测位置。
- 设备只上传已保存到 TF 卡的位置，上传前需要在屏幕上再次确认。

### 设备

- 短按电源键休眠，休眠画面可选书籍封面、保留正文或简洁屏保。
- 长按电源键打开关机、重启菜单。
- 「设置 → 使用说明」提供离线的内置说明，没有 TF 卡时也能查看。

### 当前限制

- TXT 只使用本地进度，不支持 KOReader 位置接续，也不支持 UTF-16。无 BOM 的 TXT 先按 UTF-8 读取，失败后按 GB18030 读取；其他编码可能误判或无法打开。
- KOReader 位置映射只覆盖已验证的 XHTML 范围。超出范围的内容会被明确拒绝。
- EPUB 使用统一的正文排版。表格、复杂 SVG 和复杂 CSS 会降级显示或被拒绝。
- 不支持复杂文字塑形和双向文字。
- 不支持 PDF、批注、高亮、词典、插件、阅读统计和多主题。

## 验证状态

下表概括各功能在真机上的验证程度。日期、命令、日志位置和未测项以[开发状态](docs/development-status.md)和[设备检查记录](docs/device-validation.md)为准。

| 功能 | 真机验证程度 |
|---|---|
| EPUB、TXT 离线阅读 | 已在设备上使用，专项验收未完成 |
| KOReader 位置接续 | 已在设备上完成过上传和采用远端位置，重复同步的稳定性未验收 |
| USB 文件传输 | 已验证挂载、写入读回和退出重启，唤醒后重新枚举未验证 |
| Wi-Fi 传书 | 经设备热点已完成过上传，经已保存 Wi-Fi 的传书未验证 |
| 在设备上输入 Wi-Fi 密码和同步账户 | 未验证 |
| 关机与重启菜单、书签、内置使用说明 | 未验证 |

资源峰值、掉电恢复、休眠耗电和长期稳定性均未完成测量。

## 硬件

| 项目 | 说明 |
|---|---|
| 设备 | Read Pico |
| 主控 | ESP32-S3，16 MiB Flash，8 MiB PSRAM |
| 屏幕 | 4.7 英寸单色墨水屏，面板分辨率 1216 × 684，固件按 684 × 1216 竖屏使用 |
| 像素密度 | 约 297 PPI。这是按标称尺寸和分辨率计算的值，不是官方标称值 |
| 灰阶 | 16 级。固件画布为 4 位灰阶，0 为黑色，15 为白色 |
| 输入 | 触摸屏，K1、K2、K3 三个电容触摸键，电源键 |
| 存储 | TF 卡，FAT 文件系统 |
| 板级支持 | [MindReset/read_pico_firmware](https://github.com/MindReset/read_pico_firmware) 中的 BSP 组件，固定到指定 commit，见 [BSP 审查](docs/bsp-review.md) |

## 使用

### 按键与触摸

以下是默认映射。

| 操作 | 结果 |
|---|---|
| K1，或点击正文左侧 | 上一页 |
| K3，或点击正文右侧 | 下一页 |
| K2 | 立即全刷当前画面 |
| 点击正文中部 | 打开阅读工具 |
| 长按书库中的一本书 | 进入书籍详情 |
| 长按正文中的图片 | 进入图片查看 |
| 短按电源键 | 休眠 |
| 长按电源键 | 打开关机、重启菜单 |

K2 在所有可交互页面都只全刷当前画面。请求休眠后，触摸和 K1/K2/K3 暂不响应，K2 不全刷；再短按电源键取消，长按电源键仍打开电源菜单。K2 不关闭面板，不取消预览，不确认操作，也不退出 USB 文件传输。关闭、取消和确认使用屏幕上的控件。

### 准备 TF 卡

TF 卡使用 FAT 文件系统，目录结构如下：

```text
Books/                     EPUB、TXT；网页文章可放入 Books/Articles/
Fonts/                     用户字体及其许可证
PicoPage/settings.toml     阅读配置
PicoPage/network.toml      Wi-Fi 配置，含凭据
PicoPage/sync.toml         KOSync 账户配置，含凭据
```

每本书的进度和单书设置保存在原书旁边的 `<完整文件名>.picopage/` 目录中。移动书籍时，把这个目录和原书一起移动，否则这本书的进度和设置不会跟随。

TF 卡上没有可用的正文字体时，正文使用内置字体。内置字体只包含 3,500 个常用汉字，缺少的字符显示为方框。`python3 tools/setup_fonts.py` 下载的完整字体位于 `.local/card/Fonts/`，可以连同许可证一起复制到 TF 卡的 `Fonts/`。

`network.toml` 和 `sync.toml` 含有凭据，已被 Git 忽略。不要把这两个文件放进配置导出或公开日志。

### 传书

- **USB 文件传输**：在设备上开启后，电脑将 TF 卡识别为可移动磁盘。退出前必须先在电脑上安全弹出。步骤见 [USB 文件传输](docs/usb-storage-runtime.md)。
- **Wi-Fi 传书**：用手机或电脑的浏览器上传书籍（EPUB、TXT，最大 64 MiB）、字体（TTF、OTF，最大 32 MiB）和锁屏图片（PNG、JPEG，最大 2 MiB），同名文件不覆盖。步骤和失败处理见 [Wi-Fi 传书](docs/wifi-transfer.md)。

### 与 KOReader 接续位置

前置条件：设备和 KOReader 使用内容完全相同的原始 EPUB，并且已经配置同一个 KOSync 账户。

1. 在设备上打开这本书，点击正文中部，打开阅读工具中的同步。
2. 点击「检查远端位置」。设备只查询，不跳转，也不上传。
3. 精确映射成功后，可以选择跳转到远端位置。目标正文显示成功后，设备才保存新位置。
4. 要用本机位置更新远端，点击上传并再次确认。

取消只停止尚未发送的请求。已经发出的上传仍可能在服务端生效。结果不确定时，先重新检查远端位置，不要直接重试上传。

配置字段和映射范围见[同步范围及引擎验证](docs/sync-runtime.md)。完整的操作说明见[运行说明](docs/runtime.md)。

## 构建固件

**依赖**：ESP-IDF v6.1。`idf.py` 已在 `PATH` 中时，构建脚本直接使用；否则脚本通过 EIM 调用已安装的 v6.1。脚本不安装、不升级工具链。

在仓库根目录运行：

```sh
python3 tools/setup_fonts.py
sh tools/build_firmware.sh
```

首次构建会从 GitHub 下载固定 commit 的 Read Pico BSP，保存到被 Git 忽略的 `.local/`。构建产物是 `firmware/build/pico_page.bin`。

构建脚本只构建，不访问设备，不烧录。

> **烧录前必读**
>
> - 首次安装使用全量镜像：擦除整片闪存，再从 `0x0` 写入 bootloader、分区表和应用（D41）。设备上原有的固件、分区表、NVS 和其他内部数据都会被清除，TF 卡不受影响。[网站安装页](https://picopage.wulu.zone/install/)按这种方式安装，整片安装还没有在真机上验证。
> - `firmware/partitions.build.csv` 是全量镜像使用的分区表，含两个 4 MiB 应用槽和 otadata。修改它会改变之后安装的所有设备的布局，已安装的设备需要重新整片安装。
> - 只写应用（例如开发时只更新一个应用槽）仅适用于分区表与 `partitions.build.csv` 一致的设备。写入前读取设备的分区表和 otadata，确认偏移和当前启动槽：设备经联网更新后可能运行在 `ota_1`。不要直接执行构建日志中输出的烧录命令。
> - 不要改写工厂校准数据。面板 VCOM 保存在 PMU 中，不在闪存里，整片安装不涉及它。
> - 作者设备出厂时的分区和此前只写应用的过程记录在[设备检查记录](docs/device-validation.md)中。记录中的偏移量来自该设备的实测结果，不能直接用于其他设备。

环境版本、BSP 锁定方式和已知的本机差异见[环境记录](docs/environment.md)。

## 开发与测试

```sh
python3 tools/setup_fonts.py
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel 4
ctest --test-dir build/host -N
ctest --test-dir build/host --output-on-failure
python3 tools/check_handoff.py
```

`ctest -N` 列出实际注册的测试。缺少字体资源或 Node 等可选依赖时，注册的测试会减少，这种情况下的全部通过不等于完整通过。

CI 在每次推送和拉取请求时运行 host 测试、ASan/UBSan 测试和交接检查，配置见 [`.github/workflows/host.yml`](.github/workflows/host.yml)。网站由独立的 [`.github/workflows/web.yml`](.github/workflows/web.yml) 构建并保存静态产物，两项工作流均支持手动运行。CI 不构建固件，也不覆盖真机行为。

开发时可以用桌面版在电脑上运行同一套阅读逻辑：`sh tools/run_host.sh`。桌面版不模拟墨水屏的波形、残影、触摸精度和耗电，不能代替真机验证，用法见[桌面运行接口](host/README.md)。

各测试的覆盖范围见[测试说明](tests/README.md)，验收项见[验收清单](docs/acceptance.md)。

## 仓库结构

| 路径 | 内容 |
|---|---|
| `firmware/` | ESP-IDF 工程。`components/` 中的阅读逻辑由固件和桌面版共用 |
| `host/` | 桌面版入口 |
| `tests/` | host 测试和测试素材生成脚本 |
| `tools/` | 构建、资源准备、设备诊断和性能汇总脚本 |
| `examples/` | TF 卡配置和单书设置的示例 |
| `docs/` | 决定、架构、实现记录和验证记录 |
| `docs/design/original/` | 原始设计稿，保持原样，不修改 |

## 路线图

### 第一版

第一版以下面两项通过真机验收为完成标准，目前都在进行中。

- [ ] EPUB 离线阅读
- [ ] KOReader 双向位置接续

### 第一版之后

实施顺序尚未排定。未实现的功能不会在界面上显示入口。

| 功能 | 状态 |
|---|---|
| 关机与重启：长按电源键打开菜单 | 已实现，待真机验证 |
| Wi-Fi 开关 | 已实现，待真机验证 |
| 书签：只保存在本机，不随 KOReader 同步 | 已实现，待真机验证 |
| 内置使用说明 | 已实现，待真机验证 |
| 自定义按键：K1、K2、K3 可指定上一页、下一页、全刷或回到主页 | 未开始 |
| 自定义锁屏：轮播或指定图片 | 已实现，待真机验收 |
| 锁屏密码 | 未开始 |
| 蓝牙翻页与蓝牙开关：仅支持 BLE 遥控器 | 未开始 |
| 低电量保护 | 未开始 |
| 可选翻页效果 | 未开始 |
| OTA 升级：联网更新 | 第一批已实现，真机待验证 |
| 主页左右滑动翻页 | 未开始 |
| 两台设备之间近场传输书籍、字体和图片 | 想法，待探索 |

### 后置

PDF、Karakeep、批注、高亮、词典、插件、阅读统计和多主题目前没有实施安排。

各项的设计、前提和依赖关系见[功能路线图](docs/roadmap.md)，阶段划分见[开发里程碑](docs/milestones.md)。

## 文档

- **使用**：[运行说明](docs/runtime.md) · [Wi-Fi 传书](docs/wifi-transfer.md) · [USB 文件传输](docs/usb-storage-runtime.md)
- **状态与验证**：[开发状态](docs/development-status.md) · [设备检查记录](docs/device-validation.md) · [验收清单](docs/acceptance.md) · [设备诊断接口](docs/console-runtime.md)
- **设计与决定**：[产品决定](docs/decisions.md) · [功能路线图](docs/roadmap.md) · [开发里程碑](docs/milestones.md) · [架构](docs/architecture.md) · [数据存储](docs/storage.md) · [界面实现约束](docs/ui-implementation.md)
- **实现记录**：[平台运行](docs/platform-runtime.md) · [字体与分页](docs/graphics-runtime.md) · [图片实现](docs/image-runtime.md) · [同步范围及引擎验证](docs/sync-runtime.md) · [存储实现](docs/store-implementation.md)
- **环境与依赖**：[环境记录](docs/environment.md) · [BSP 审查](docs/bsp-review.md) · [依赖审查](docs/dependency-review.md) · [公开 EPUB 样本](docs/public-books.md)
- **开发规则**：[仓库开发规则](AGENTS.md) · [嵌入式开发规则](docs/embedded-development.md)

## 参与

项目主要供作者自用，功能取舍以[产品决定](docs/decisions.md)为准。欢迎通过 [Issues](https://github.com/eMUQI/pico-page/issues) 报告问题。

提交代码前请阅读[仓库开发规则](AGENTS.md)，并按其中的验证表运行对应检查。以下内容不要提交到仓库：凭据、个人 EPUB、完整 Flash 备份和私有日志。

## 许可证

项目的总许可证尚未选定。

第三方组件保留各自的许可证，例如 BSP 中的 `epdiy` 使用 LGPL-3.0-or-later，内置字体使用 OFL 1.1。来源、版本和义务见[依赖审查](docs/dependency-review.md)和 [BSP 审查](docs/bsp-review.md)。
