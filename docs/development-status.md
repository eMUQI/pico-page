# 开发状态

最后更新：2026-10-06。Read Pico 开书崩溃已修复；离线开书、翻页、排版取消/应用、刷新及切书恢复已实测通过。
最终离线应用已仅写入 `ota_0` 并通过摘要校验；重启后同页恢复已通过，修订号 24、正文 CRC32 `d1b7e837` 保持一致。
**物理目视、实体触摸/按键、睡眠及 TF 掉电仍未验收。用户已明确暂不配置 Wi-Fi，先完成离线阅读。**

修复了 miniz 与 ESP32-S3 ROM 的 ABI 冲突、内部内存分配压力和默认字体路径不一致；
已完成字体 PSRAM 分配器及全局排版反馈。当前 host 与 ASan/UBSan 各 9/9 通过，
字宽缓存改前/改后的 2,060 页边界与像素摘要一致。最新事实以本轮记录和设备验证页为准。

## 当前产物

- 桌面程序：`build/host/pico_page`。直接运行 `sh tools/run_host.sh`，使用同一原生 C++ 阅读核心。
- 最新完整构建的固件：`firmware/build/pico_page.bin`，1,956,464 字节；本地 3 MiB 应用构建配置。
  SHA-256：`95cc9b7072fe2e4b3f3179acb7505de8ff7ec67a4ea52d38797a1f536a9bb95b`。
  包含图片解码与 App 分页、资源导入、主循环门控及 410 字符内置提示字体；包含本轮崩溃、内存、字体与反馈修复。
- 演示卡：`.local/card`，原创 EPUB、公开鲁迅 EPUB、完整中文字体及许可证；实现截图：`artifacts/runtime`。
- 本地试用包：`.local/initial-build` 是此前的构建快照，不包含最新增量，也不包含设备烧录脚本。
- 所有改动仍在当前 main 工作目录，未提交、未 push、未发布。
  仅写入实测 4 MiB `ota_0` 槽的应用，偏移 `0x10000`；原 bootloader、分区表、NVS、otadata 和校准数据未改。

## 已接通的实际路径

- 原 EPUB → 有界 ZIP/XML → 当前章源位置 → Unicode 完整行分页 / PNG、JPEG 图片独立页 → 灰阶画布。
- 书库/文件夹、最近阅读、翻页、跨章、目录、内部链接及返回、工具、排版预览/应用/取消、刷新模式。
- 显示成功后保存；重开恢复；K2 立即全刷且锚点/修订号不变；显示失败恢复已确认位置。
- TOML 全局/单书层保留注释写回、外部编辑冲突保护、恢复默认、进度备份与失败重试。
- Read Pico 显示、工厂 VCOM 读取、触摸按键、TF、PMU 浅睡与独立 Wi-Fi/HTTPS 工作任务。
- 手动检查远端候选、精确映射后确认跳转、显式确认上传已保存版本；取消、旧回包、账户更换隔离。
- 缺完整字库、损坏记录、配置错误、映射不支持和网络失败均不以成功占位替代。
- 受限 USB 资源导入已实测完成：两套中文字体、许可证、《阿Ｑ正傳》和《吶喊》均读回 CRC 校验通过。

## 累计验证记录

工作目录：`/Users/wulu/workspace/pico-page`。

| 命令 / 操作 | 结果和证据 |
|---|---|
| `cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug`；`cmake --build build/host -j 8` | 成功；最新增量 `.local/host-current.log` |
| `ctest --test-dir build/host --output-on-failure` | 9/9 通过，含真实 app 开书、呈现失败、排版和异步同步回包回归 |
| `cmake --build build/sanitized -j 8`；`ctest --test-dir build/sanitized --output-on-failure` | ASan/UBSan 9/9 通过，无运行时错误；配置见 environment |
| `python3 tools/check_koreader_mapping.py` | 真实固定 CREngine：两个 DOM 版本 × 三档字号，每组 2,018 个精确源位置往返；`.local/koreader-reference-check.log` |
| 实际 CREngine 加载原始鲁迅 EPUB，两个 DOM 版本 × 三档字号 | 《阿Ｑ正傳》每组 45,172 个文本标量、857 个引擎书签；《吶喊》每组 98,722 个文本标量、1,875 个引擎书签；全部成功，图片元素未纳入精确同步位置范围 |
| 官方完整 KOReader 客户端经本地测试服务联测 | 两本原 EPUB × 三档字号，共六组双向接续和实际客户端重启恢复通过；详见 [同步记录](sync-runtime.md) |
| 原版鲁迅 EPUB 图片与整书分页 | 两本 PNG 封面实际解码、目检通过；host 与 ASan/UBSan 的三档字号逐页检查通过，见 [图片实现](image-runtime.md) |
| `sh tools/build_firmware.sh` | 最新完整构建成功，包含图片、App、导入与主循环门控；`.local/firmware-usb-protocol.log` |
| 仅写入 `ota_0` 应用区域 | `0x10000` 写入 1,944,560 字节，`Hash of data verified`；`.local/device/install/usb-protocol-flash.log` |
| 首版真机启动 | PMU、工厂 VCOM 读取、SD、PSRAM、书库全刷完成已确认；不等于物理目视和输入验收 |
| host `--headless --script .local/smoke.txt` | 真实 EPUB 打开/翻页/K2/工具/排版取消/目录/睡眠恢复；K2 前后保存修订号均为 5 |
| 检视 684 × 1216 实际渲染 PNG | 正文完整行；排版采用原稿结构；工具栏遮挡处移除半行残片，关闭后恢复原页 |
| SDL 窗口启动 | 沙箱内无法枚举显示设备；获准在本地 UI 环境启动后程序持续运行。CUA 无法绑定无 bundle 的命令行窗口，未声称其交互已由 CUA 验收 |
| `python3 tools/check_handoff.py` | 284 项通过；49 个原设计文件、36 张原 PNG、三份历史快照保持原样 |
| `git diff --check` | 通过 |

## 明确未完成 / 未验证

- 真机物理目视方向/灰阶/全刷效果、触摸、TF 满卡/掉电、内存峰值、睡眠/耗电：未验收。
- 历史开书崩溃已修复，设备离线阅读闭环基线已通过；最终候选的重启恢复已通过，画布与物理显示的验证边界见本轮记录。
  用户已明确取消完整备份。保留历史崩溃 ELF 和日志供追溯。
- 官方完整 KOReader 客户端与本地测试服务联测已通过；外部账户服务和 Read Pico 的 Wi-Fi/HTTPS
  双向接续仍未验收，不能用客户端本地联测代替。
- 精确同步已扩大到普通 CSS、DOCTYPE 和已对照的 HTML 文本；媒体、复杂 CSS/DOM 等仍拒绝同步定位，
  本地阅读按明确降级范围处理。PNG/JPEG 解码及普通 SVG 单图封面已实现，复杂 SVG 与其他不支持格式明确占位。
- 设备上的 Wi-Fi/账户输入、无线导入、格子书架、目录折叠、删除书籍、电量状态栏尚未实现。
- PDF、Karakeep、批注/词典、插件、统计、多主题、自定义按键与完整 OTA 仍按既定决定后置。
- 用户现有 IDF 局部修改保持原样；未在全新官方工具链复现；远端 CI 未执行；总许可证及发布义务尚未完成。

## 下一步

按 [最新交接](handoff-next-session.md) 和 [设备验证](device-validation.md) 完成剩余物理目视、触摸、睡眠及 TF 异常验收。
用户准备网络与账户配置后，再开展真实设备 Wi-Fi/HTTPS 接续验收。
扩大同步兼容性必须增加真实引擎对照，不能放宽为百分比或近似位置。

## 本轮记录

### 2026-10-06 · 字形缓存淘汰策略（偶发慢页）

- 现象：连续翻页时约每 20 页出现一次 1.5 秒左右的页面准备。设备探针显示慢页与 PSRAM 空闲回升约 0.93 MB
  同时出现（`.local/device/install/slow-page-probe.log`），原因是字形位图缓存达到 1 MiB 后整体清空，当页全部重新栅格化。
- 修改：缓存满时只淘汰最久未绘制的一半，当前页的字形保留；字宽缓存上限由 1,024 提高到 8,192 项（满时仍整体清空）。
- 验证：host CTest 9/9、sanitized CTest 9/9；`page_compare` 的 2,060 页边界与像素摘要仍与基准相同
  （`.local/glyph-cache-check/pages-lru.log`），ASan/UBSan 版本跑《阿Ｑ正傳》三档字号无报错。
  固件构建成功；镜像 `.local/device/install/pico_page-glyph-lru.bin`，1,971,840 字节，
  SHA-256 `6af21f043ce33c8c608945f848f26a7ca20ab5f1f285c9519b958bb6ea48ab79`，仅写 Read Pico `ota_0`，摘要校验成功。
  本条取代此前的已安装镜像。
- 设备结果：《吶喊》连续 50 页，淘汰发生在第 20、33 页（PSRAM 回升约 0.47、0.52 MB），当页准备 0.56、0.29 秒；
  全程最长 1.0 秒（开书后第 3 页，缓存尚未预热），之后多为 0.15–0.6 秒；回翻 50 页回到起始画布 CRC。
  `glyph-lru-probe-nahan.log`。PSRAM 空闲最低约 2.8 MB。
- 测试副作用：《阿Ｑ正傳》的阅读位置被本轮探针翻到书末，回翻 90 页后未能对上测试前的页面，原位置未恢复；
  《吶喊》已回到测试前的页面。两书的修订号因测试增加。
- 未验证：字宽缓存达到 8,192 项时的清空耗时（两本样书未触及）、更大字号下的淘汰频率。

### 2026-10-06 · "快速"刷新档接入八灰阶波形

- 用户目视八灰阶波形后确认有少量残影、可用刷新策略控制，并选定方案：`display.refresh_preset = "fast"`
  改为八灰阶 GL16，自动全刷间隔由 20 次正文更新改为 5 次；"清晰""均衡"不变。
  刷新设置页"快速"的说明改为"翻页使用更快的灰阶更新，每 5 页执行全刷。"，所用字符均在内置提示字体内，布局未改。
- 波形由应用按档位通过 `platform::set_page_waveform()` 选择；上一条记录中的诊断命令 `wave` 和阈值黑白选项已删除。
- 实际执行与结果：host CTest 9/9、sanitized CTest 9/9；`sh tools/build_firmware.sh` 成功（`.local/firmware-fast-preset.log`）。
  镜像 `.local/device/install/pico_page-fast-preset.bin`，1,970,784 字节，
  SHA-256 `e21269ff795567b85b4485640c67bdc2435cf2d2cc7d5ab4e42dc1b0f00e5812`；仅写 Read Pico `ota_0`，摘要校验成功，
  写入前核对端口 USB 序列号与芯片 MAC（`fast-preset-flash.log`）。本条取代此前的已安装镜像。
- 设备验证（诊断输入，`fast-preset-validation.log`）：经工具栏 → 刷新 → 快速 切换后，驱动刷新由 480 降到 376 毫秒；
  连续 13 次 K3 中每 5 次灰阶更新后出现一次全刷（1,363 毫秒）；呈现阶段约 0.62 秒，页面准备多为 0.36–0.90 秒，
  其中一页为 1.47 秒，原因未查。**验证时已把《阿Ｑ正傳》的单书刷新档位设为"快速"并写入 TF 卡。**
- 未验证：5 页间隔下的残影观感（等待用户目视）、其他档位的面板实拍、自动休眠。

### 2026-10-06 · 呈现分段计时、竖屏转换与波形对照

- 诊断 `status` 新增最近一次呈现的 `copy_ms`、`power_ms`、`draw_ms`；新增诊断命令 `wave 0|1|2`，
  选择非全刷帧的波形（0 默认 16 灰阶 GL16、1 8 灰阶 GL16、2 阈值 DU）。该选择不保存，重启恢复 0；
  全刷始终使用 GC16。尚未接入用户设置，等待用户目视比较后决定。
- 分段计时显示竖屏转换占 347 毫秒：原实现按列跨行读取，超出 64 KiB 数据缓存。改为 64 列一组后为 75 毫秒；
  host 上用随机帧核对新旧实现输出相同（临时程序，未入库）。
- 设备实测（诊断输入，`.local/device/install/waveform-probe.log`、`banded-copy-probe.log`）：
  驱动刷新 16 灰阶 480 毫秒、8 灰阶 376 毫秒、阈值 272 毫秒；全刷 1,364 毫秒。
  默认波形下呈现阶段由约 0.99 秒降到约 0.72 秒，8 灰阶约 0.61 秒。
- 已安装镜像：`.local/device/install/pico_page-banded-copy.bin`，1,972,064 字节，
  SHA-256 `31035a80120fde767020fb58addcac7387abb5b01341cdfb7621d1f5e88bf537`；仅写 `ota_0`，摘要校验成功，
  写入前核对端口 USB 序列号与芯片 MAC（`banded-copy-flash.log`）。本条取代此前各条的已安装镜像。
- 本批只做了固件构建和设备计时；host CTest 未重跑（改动限于平台层和诊断组件，host 不编译这两部分）。
- 未验证：各波形的文字边缘、残影和灰阶观感，需用户对着面板判断；自动休眠仍未在设备上验证。

### 2026-10-06 · 字形位图缓存

- `Font::draw` 按"字号 + Unicode 标量"缓存 FreeType 栅格化结果（8 位覆盖率，与原绘制路径逐像素相同）。
  每个字体对象上限 1 MiB，超限时整体清空；重新打开字体时清空。排版、锚点和保存逻辑未改。
- 实际执行与结果：

| 命令 / 操作 | 结果 |
|---|---|
| host 构建与 `ctest --test-dir build/host` | 9/9 通过；`graphics_test` 新增缓存命中与首次栅格化像素一致的用例 |
| sanitized 构建与 CTest | 9/9 通过，无 ASan/UBSan 错误 |
| `.local/font-psram-check/page_compare.cpp` 链接当前库，两本鲁迅 EPUB × 三档字号 | 2,060 页的分页边界与像素摘要和 `.local/font-psram-check/pages-after.log` 完全相同；`.local/glyph-cache-check/pages.log` |
| `sh tools/build_firmware.sh` | 成功；1,965,408 字节，SHA-256 `48e79332c22f1790fd65004c0add89497dd733c1d33d4683ddc4a06bc8e456c6`；`.local/firmware-glyph-cache.log` |
| 仅写入 Read Pico `ota_0`（`0x10000`） | `Hash of data verified`；写入前核对端口的 USB 序列号与芯片 MAC；`.local/device/install/glyph-cache-flash.log` |
| 设备翻页计时（诊断输入，《阿Ｑ正傳》） | 见下；`.local/device/install/latency-glyph-cache.log` |

- 设备计时（页面准备 + 面板呈现）：改前每页约 1.27 + 0.99 秒；改后连续向后 8 页的准备时间
  从 1.02 秒降到 0.35–0.59 秒，回翻到已缓存的页为 0.09–0.10 秒；面板呈现仍约 0.99 秒（全刷约 1.8 秒）。
  往返各页 CRC 一致，K2 不改变修订号。这 17 页后 PSRAM 空闲从 4.33 MB 降到 3.73 MB，内部空闲约 148 KB 不变。
- 当前已安装镜像为本条的 `48e79332…`，取代此前各条记录的镜像。
- 未验证：缓存满后整体清空时的单页耗时、长时间阅读的内存峰值、面板实拍。自动休眠仍未在设备上验证。
- 下一步：面板呈现的约 1 秒是当前主要耗时；可选做空闲时预渲染下一页，或由用户对着屏幕比较更快的波形。

### 2026-10-06 · 章节解析分配、空闲降频与自动休眠

- 章节解析不再为每个字符构造来源锚点字符串，改为每个文本节点一次；隐藏内容直接跳过。
  正文链接下划线只处理与当前页相交的链接。输出不变，见下方对照。
- 新增全局设置 `power.auto_sleep_minutes`（0–240，默认 10，0 关闭），只能在 TF 卡上编辑，
  启动或开书时读取。主循环在无物理输入、无诊断命令、无网络任务和导入时计时，到时走既有休眠路径。
  **TF 卡上现有 `settings.toml` 没有此项时按 10 分钟生效；休眠后 USB 串口会断开，需按电源键唤醒。
  无人值守的串口验证前，先把此项设为 0。**
- 启用 `CONFIG_PM_ENABLE`：活动时持有最高频率锁（240 MHz），无活动 3 秒后释放，空闲频率 80 MHz；
  `present()` 总是先恢复最高频率。未启用自动浅睡（tickless）。
- 启用 `CONFIG_FATFS_USE_FASTSEEK`，只对只读打开的文件生效；簇链表建立失败时 IDF 回退到普通 seek。
- 实际执行与结果（工作目录 `/Users/wulu/workspace/pico-page`）：

| 命令 | 结果 |
|---|---|
| `cmake --build build/host -j 8`；`ctest --test-dir build/host --output-on-failure` | 9/9 通过；`store_test` 新增自动休眠设置的默认值、0、越界和单书拒绝用例 |
| `cmake --build build/sanitized -j 8`；`ctest --test-dir build/sanitized --output-on-failure` | 9/9 通过，无 ASan/UBSan 错误 |
| `sh tools/check_public_books.sh <文楷> <两本鲁迅 EPUB>` | 各章字符数、页数、节点数、来源段数与 `.local/font-psram-check/pages-after.log` 的非像素行逐行相同；`.local/parser-alloc-check/` |
| `python3 tools/check_koreader_mapping.py` | 退出码 0，各组 2018/2018 精确源位置通过；`.local/parser-alloc-check/koreader-mapping.log` |
| `sh tools/build_firmware.sh` | 成功；`firmware/build/pico_page.bin` 1,964,144 字节，SHA-256 `8e050edba93be7d17f3242604f7d4f3a731573664fd920a3611fa953369c165d`；`.local/firmware-idle-power.log` |

- 经用户同意，本轮镜像已仅写入 Read Pico 的 `ota_0`（`0x10000`），`Hash of data verified`；
  顶部"当前产物"中的镜像大小和摘要已被本条取代。目标确认方式与设备结果见 [设备验证](device-validation.md)。
- 设备结果（诊断输入）：启动、书库全刷、重开《阿Ｑ正傳》恢复到修订 30 的原页；K3/K1 翻页每次 2.4–2.8 秒
  （含一次周期全刷的为 3.3–4.2 秒），K1 返回各页 CRC 与去程相同；空闲 5–8 秒（已释放最高频率锁）后的
  翻页和 K2 全刷均成功，K2 不改变修订号。上一轮同路径记录为 3.1–3.4 秒，未单独区分各项改动的贡献。
- 卡上的 `auto_sleep_minutes` 未能预先设为 0：诊断接口没有写配置文件的命令，本轮按默认 10 分钟运行。
- 用户决定本轮不测休眠：自动休眠的触发与唤醒均未在设备上验证，仅有 host 的设置解析测试。
- 未验证：实体触摸在 80 MHz 空闲时的响应、实际电流。这些需要人工操作或仪器。
- 未做：TF 卡章节分页缓存。字宽缓存后重排只在内存中进行，先在真机测量往回翻的耗时再决定。
- 下一步：烧录本轮镜像后，用诊断 `status` 记录翻页各阶段耗时和休眠前后电流，再决定是否精简进度保存的回读。

### 2026-10-06 · 开书修复与设备离线阅读闭环

- 原开书崩溃由 ESP32-S3 ROM miniz 替换项目同名函数造成，解压结构体 ABI 不兼容。
  使用组件符号前缀修复 EPUB 和 PNG 调用，保留 vendor 原件，未增大主栈。
- 字体分配器经过正常打开、中文绘制、分配失败、重分配失败/零尺寸清理验证；普通分配改为
  PSRAM 优先，设备阅读内部空闲约 149 KB、最大连续块 100 KiB，不再出现原 TF DMA 分配失败。
- 默认文楷路径与准备脚本一致；补齐全局排版保存的覆盖/继承反馈，保持 TF 配置权威。
  有界字宽缓存不改变已验证的 2,060 页边界与像素摘要；提示子集为 410 字符。
- 实际执行：`cmake --build build/host -j 8`、host CTest 9/9；相同 sanitized 构建和 CTest 9/9。
  日志 `.local/host-metrics-cache.log`、`.local/sanitized-metrics-cache.log`。
- `sh tools/build_firmware.sh` 完整构建成功；仅向实测 OTA0 写入最终应用，Flash 摘要验证成功。
  构建与烧录证据见 [设备验证](device-validation.md)。没有提交、推送或发布。
- 真机经诊断输入完成两书开读、前后翻页、K2、排版取消/应用、默认继承/覆盖、目录返回、
  切书恢复与硬复位后同页恢复。最后保存修订 24 与正文 CRC `d1b7e837` 在重启前后相同。
  `.local/device/install/final-validation.log`；最终画布 `artifacts/runtime/read-pico-offline.png`
  已检查，中文正文与页脚未见裁字。画布导出不等于物理成像验收。
- 用户明确先完成离线阅读；网络/服务配置、设备 Wi-Fi/HTTPS 双向接续暂缓。
- 收尾 `python3 tools/check_handoff.py` 通过 288 项，原设计 49 文件及 3 个历史快照未变；
  `git diff --check` 通过，安装镜像与当前构建 SHA-256 一致，串口客户端已退出。
  物理目视、触摸、睡眠、TF 满卡/掉电与长期功耗仍未验收；不将这些事项标为通过。
- 下一步：按真实硬件使用反馈修正问题；具备网络配置后再做设备与 KOReader 双向联测。


### 2026-10-06 · 图片、导入与设备应用安装

- 原生 EPUB 接入有界 PNG/JPEG 解码、普通 SVG 单图封面与独立图片分页；保留元素来源锚点，
  损坏或不支持的媒体显示明确占位。内置提示字体更新为 406 字符，覆盖当前 App 文案。
- 完整 `sh tools/build_firmware.sh` 成功，日志 `.local/firmware-usb-protocol.log`；
  镜像大小与摘要见顶部。host 与 ASan/UBSan CTest 各 9/9 通过，交接检查 284 项通过。
- 官方完整 KOReader 客户端六组双向接续和重启恢复通过；仍只证明本地测试服务联测范围。
- 用户取消完整备份后，按已确认布局仅写入 `ota_0` 应用；写入摘要校验通过，未改 bootloader、
  分区表、NVS、otadata 或工厂校准。此前首版实际启动已确认 PMU、VCOM 读取、SD、PSRAM 与书库全刷。
- 新镜像诊断与资源导入正在验证；没有将后续操作写成通过。物理目视、触摸、睡眠和实际设备同步仍待验收。
- 当前 main 工作目录保留全部改动，未提交、推送或发布；原设计与历史快照未修改。

### 2026-10-06 · 真实 EPUB、设备预检与诊断接入

- 取得未修改的《阿Ｑ正傳》和《吶喊》，来源、摘要与许可保存在 [公开样本记录](public-books.md)。
  两书实际逐页绘制通过。App 使用《阿Ｑ正傳》验证保存后重开、K2、工具和模拟睡眠恢复，
  修订号保持 6；全刷前后与恢复后的 BMP 摘要相同，证据 `.local/lu-xun-resume.log`。
- 原文小标点及引号纳入换行禁则，原文已有全角缩进时不再叠加默认缩进；没有更改视觉布局。
- EPUB 有界保留样式和首文本子节点元数据；同步支持普通 CSS/DOCTYPE 与块首空白索引变化。
  扩大的真实引擎检查同时核对逐字符值和实际 `getBookmark()` 反向位置，详见同步记录。
- 接入默认关闭的 USB 诊断组件。主循环统一物理/诊断输入序号，只有真实面板完成后记录帧结果。
  主机客户端支持安全命令和带帧号/CRC 的当前画布导出；无 Flash、NVS、校准或任意文件写入口。
- 最新 host 与 ASan/UBSan 各 9/9 通过。变更 C++ 已做 Xtensa 目标文件编译；这不等于完整链接。
  完整 IDF 重建因沙箱禁止依赖管理器 `sysctl` 查询而停止；当前 bin 仍为前次完整构建。
- 硬件只读确认 ESP32-S3、8 MiB PSRAM、16 MiB Flash 和双应用槽；未改任何 Flash 字节。
  全片读取首次传输失败，低速分区读取成功；完整备份重试因自动审批服务用量已达上限而未执行。
  审批失败不是安全性拒绝；未绕过审批，未重启退出下载模式，未宣称设备正在运行 Pico Page。
- 工作继续保留在当前 main，未提交、未 push、未发布。完整可用固件目标仍未达成。

### 2026-10-05 · 代码注释规范
- 改动：在 `AGENTS.md` 中加入完整的 Code Comment Policy，明确注释内容、措辞、API 文档及修改时的清理要求；面向用户的说明参考 Tech-Doc-Style-Chinese，补充中文表达、操作步骤、事实保真和排版原则，并明确 ASD-STE100 方法的中文适用边界。
- 实际执行命令与工作目录：在 `/Users/wulu/workspace/pico-page` 执行 `python3 tools/check_handoff.py`，248 项交接包检查通过。
- 验证范围：仅文档与交接包检查；未新增测试，未执行固件构建、host 或真机验证。
- 当轮限制：修改规范时目录尚未初始化 Git，无法检查 Git 差异；现已完成初始化，见下方记录。
- 新的兼容性、迁移或许可证风险：无，仅更新开发规范。
- 下一项最小工作：继续按 `CODEX_START.md` 推进 G0 + G1a。

### 2026-10-05 · 初始化本地仓库
- 改动：执行 `git init -b main`，建立本地 Git 仓库；按现有 `.gitignore` 纳入当前交接资料、设计原件、示例与检查工具。
- 实际执行命令与工作目录：在 `/Users/wulu/workspace/pico-page` 执行 `python3 tools/check_handoff.py`，248 项交接包检查通过；常见凭据模式与个人 EPUB、二进制备份、密钥、日志文件名检查未发现匹配项。
- 验证范围：仅资料完整性与提交内容检查；固件构建、host 和真机验证仍未执行。
- 远端状态：未配置远端，未推送。
- 下一项最小工作：继续按 `CODEX_START.md` 推进 G0 + G1a。

### 2026-10-05 · G0 / G1a 实现与 G1b 探针

- 改动：完成上述 core/store 和诊断工程，新增构建脚本、host CI、合成 EPUB 探针与逐项依赖审查。
  未修改 49 个原设计文件或三份历史快照；没有新增占位 UI、自定义按键、网络成功状态或专有阅读包。
- 工作目录：`/Users/wulu/workspace/pico-page`。所有改动保留本地，未提交、未发布、未 push、未刷写。
- 实际命令和结果：

| 命令 | 实际结果与证据 |
|---|---|
| `cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug` | 配置通过；AppleClang 21 / C++17 / C17 / Python 3.14.7 |
| `cmake --build build/host -j 4` | 退出码 0 |
| `ctest --test-dir build/host --output-on-failure` | 5/5 通过；`build/host/Testing/Temporary/LastTest.log` |
| sanitizer 配置命令见 `docs/environment.md`，随后构建并运行 CTest | 5/5 通过；`build/sanitized/Testing/Temporary/LastTest.log`；无 ASan/UBSan 运行时错误 |
| `sh tools/build_firmware.sh` | 退出码 0；`firmware/build/pico_page.bin`，162064 字节；`.local/firmware-build-final.log` |
| `python3 -m unittest discover -s tests -p test_epub_probe.py -v` | 四组 Python 检查通过，亦已纳入 CTest |
| `python3 tests/make_epub_fixture.py /tmp/pico-page-probe.epub`；探针 `--spine 0` / `--spine 1` | 原创两章 EPUB 解析成功；摘要和标量证据见 `docs/epub-kosync-probe.md` |
| `python3 tools/check_handoff.py` | 原设计摘要、示例及本地文档链接检查通过，仅交接范围 |
| `git diff --check`；`sh -n tools/build_firmware.sh tools/setup_bsp.sh` | 均退出码 0 |

独立代码审查找到并修复了目录同步失败后重试错误确认保存的问题。
`store_sync_test` 先复现失败，再验证持续失败不返回确认、故障解除后才成功；复审确认原问题已解决。
同时修正 FatFs 不支持覆盖重命名的发布协议、无效 UTF-8 读取，以及 tomlc17 的 Xtensa 打印格式。
这些都是实际失败或源代码约束；没有用成功占位掩盖问题。

| 验收项 | 本轮覆盖与限制 |
|---|---|
| IN01–IN03 | `pass_host_only`：集中映射和全刷意图、忙碌去重、面板安全动作；物理全刷及 UI 仍待 D/V |
| IN04 | `pass_host_only`：序列尾部与获准唤醒输入不穿透；实际唤醒来源待 D |
| EV01–EV02 | `pass_host_only`：已实现的布局/呈现/保存代次，失败/旧请求不推进进度；网络回包尚无实现 |
| ST01 | `pass_host_only`：缓存清理保留原书、记录、配置、封面与 prepared；真实离线阅读待内容/设备接入 |
| ST02 / ST07 | 部分 host 覆盖：临时文件不恢复、坏/缺主文件恢复、写入受阻/短写/同步失败；未穷举 syscall，未做 TF 掉电/满卡 |
| ST03 | 部分 host 覆盖：已核实内容身份不符时拒绝复用；原书摘要核实仍由调用方负责 |
| CFG01 | `pass_host_only`：语法、UTF-8、字段、版本错误有诊断且保留原文 |
| CFG03 | 部分 host 覆盖：本书覆盖与全局继承；删除覆盖/界面应用未实现 |
| EP03–EP05 | 部分 host 探针：源标量、章节隔离、资源限额和显式拒绝；没有分页、图像或设备内容后端 |
| SYNC01–SYNC03 | `not_run`：真实客户端位置往返未执行；上游 MD5 函数一致不算同步完成 |
| DP/PW/其他 D/K/V | 未执行，不由编译或 recording display 代替 |

兼容性和许可：BSP/SDK 存在多种组件许可，最终二进制分发义务未完成；项目总许可证待选。
G0 的硬件门槛、G1b 原生定位、G2 阅读界面、G3 同步和 G4 发布均未完成。
下一步按本页顶部顺序推进，不重新讨论正文 K2 和既有设计。

## 每轮追加记录模板

### <日期> · <任务/Git commit（若有）>
- 改动：
- 实际执行命令与工作目录：
- 测试结果与证据路径：
- 仅编译通过 / host 验证 / 真机验证分别是什么：
- 未验证/阻塞（不得写成通过）：
- 新的兼容性、迁移或许可证风险：
- 下一项最小工作：

真实结果应来自当前运行，不能沿用交接包检查的“通过”填充固件完成率。
