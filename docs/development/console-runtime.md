# 本地 USB 诊断协议

日期：2026-10-06。组件：`firmware/components/reader_console/`。

本接口只用于受控设备验证。`CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE` 默认为 `n`，仅在 USB Serial JTAG 被选为控制台时可启用；普通固件保持关闭。启用后，连接 USB 的本地程序可以操作阅读界面，没有账户认证。组件已接入固件主循环并安装。状态、目录、资源导入/取消和完整字体及书籍导入均已在设备验证；本轮用于实际阅读操作与画布导出。

新增构建模式、metrics/trace 协议、持续采集和结果格式见 [诊断工作台](diagnostic-workbench.md)。观察命令不计入用户活动，持续采集不重置停留保存的空闲计时。

## 主循环接入

1. 在应用任务调用 `console::initialize(error)`。它安装带 8 KiB 接收、2 KiB 发送缓冲的官方 USB 驱动，将现有控制台绑定到该驱动。协议使用 `usb_serial_jtag_read_bytes(..., 0)` 非阻塞读取；回复采用独立有界缓冲，一次入队整行，避免日志 VFS 逐字节尽力发送造成回复不完整。
2. 所有显示调用完成后、`app.complete_frame()` 之前，调用 `console::frame_completed(frame, actual_result)`。结果必须来自真实平台调用。
3. 每轮主循环调用 `console::poll(app, card_root, input_sequence, actions_allowed, input_allowed)`。物理输入与诊断输入共用一个递增序号；物理释放和取消沿用对应按下的序号。
4. 有网络工作、休眠请求或物理触摸尚未结束时，传入 `actions_allowed=false`，拒绝直接开书和开始导入。仅网络工作忙且无物理接触/休眠请求时，另传 `input_allowed=true`，允许 `key/tap/hold/swipe/press/release` 走实体输入相同的应用路由，用于验证 K2 和取消；在途显示帧及资源导入仍拒绝输入。`status`、目录列表与屏幕分块仍可读取。挂载不可用时传入空 `card_root`。
5. `console::transfer_active()` 为真时，暂缓新的物理输入、网络分派与休眠。已被吞下的物理接触持续吞到释放；断卡事件仍需处理。已有导入的 data/commit/abort 不依赖 `actions_allowed`，否则事务会被自己的保护条件阻塞。

组件不调用 `complete_frame()`，不跳过 App 的呈现确认、保存或输入路由。关闭配置后，三个入口保留空实现，不读取输入、不安装 USB 驱动、不输出诊断结果。

## 命令和回复

命令使用 UTF-8，以 LF 结束，最多 4224 字节。格式为 `PP <id> <command>`，`id` 为非零 32 位无符号整数。一次只发送一个命令，收到相同 ID 的 `OK` 或 `ERROR` 后再发送下一条。接收使用原生 USB 驱动和 512 字节有界缓冲，跨块保留未解析尾部。主循环每次最多解析 4225 字节并处理一条完整命令。超长或含控制字符的行整行丢弃，同时取消活动导入，不回显输入。

| 命令示例 | 行为 |
|---|---|
| `PP 1 status` | 视图、待呈现状态、已保存修订号、实际帧结果、画布 CRC32、堆和栈余量、电量有效性；`copy_ms`、`power_ms`、`draw_ms` 为最近一次呈现的分段耗时 |
| `PP 13 waveform default` / `ripple` | 选择本次启动的翻页效果；与屏幕入口共用状态，忙态拒绝，不保存配置；回复包含所选名称，平台准备时另记 `page_effect=` 日志 |
| `PP 2 ls 0 .` | 卡根目录，从第 0 项开始，最多返回 32 项；下页使用 `next` |
| `PP 3 ls 0 Books` | 列出安全相对目录；拒绝绝对路径、上级路径和符号链接目录 |
| `PP 4 key K2` | 经正常 App API 发送按下和释放；正文仍为立即全刷 |
| `PP 5 tap 342 600` | 发送屏幕内一次触摸；坐标范围为 x 0–683、y 0–1215 |
| `PP 5 hold 128 560` | 发送一次长按；参数与 `tap` 相同。只验证应用对长按的处理，不经过触摸采样和时长判定 |
| `PP 8 press 128 560`／`PP 9 release 128 560` | 分别发送连续接触的长按开始与松手，与触摸采样器的连续接触相同，用于选词预览后松手查词；`release` 须对应先前的 `press`，否则返回 `no_contact`。同样不经过时长判定 |
| `PP 6 open Books/example.epub` | 经正常 App API 开书；路径必须存在于卡内 |
| `PP 7 screen 12 0 256` | 导出第 12 个已确认帧从字节 0 开始的 256 字节 packed 数据 |
| `PP 8 file Fonts/reader.ttf` | 只返回白名单资源的长度与 CRC32 |
| `PP 14 file_head Books/sample.txt` | 返回白名单资源首部最多 256 字节的 `hex`，以及总长度 `bytes` 和实际读取长度 `count`；无偏移或长度参数 |
| `PP 9 begin 9 cbf43926 Books/check.epub` | 开始一个 9 字节、指定 CRC32 的新资源导入 |
| `PP 10 data 0 313233343536373839` | 写入从偏移 0 开始的十六进制数据，每块最多 2048 字节 |
| `PP 11 commit` | 校验、同步、读回并发布；无论成功或失败都结束事务 |
| `PP 12 abort` | 取消活动导入，只清理本事务创建的临时文件 |

普通回复以 `PP <id> OK ...` 或 `PP <id> ERROR <code>` 开始。协议发送缓冲为 1536 字节，每行前补 LF，避免与其他日志的半行拼接。格式化截断或入队失败会增加 `tx_failures`，不会把截断行当作完整回复。一般命令回复的驱动等待参数为 1000 ms；内部锁和队列各可等待一次。新增 metrics/trace（含 trace_start/trace_stop 返回的 metrics）整次响应共用 20 ms 传输等待预算，详见 [诊断工作台](diagnostic-workbench.md)。主动发送的 `READY` 和 `FRAME` 通知使用零等待，缓冲区满时丢弃并增加 `tx_failures`，不阻塞阅读。
目录项为 `PP <id> ENTRY kind=dir|file bytes=... name_hex=...`；名称采用 UTF-8 字节的十六进制，
避免文件名被解释为控制字符。目录每页最多 32 项，起始项不超过 4096，不递归遍历。

每次真实显示提交另发 `PP 0 FRAME number=... success=... purpose=... clean=...`。`purpose` 对应正文 0、覆盖层 1、手动刷新 2；`clean=1` 表示请求全刷路径。输入命令的 `accepted` 只表示已交给 App，不能代替后续 `FRAME success=1`。`FRAME success=1` 是驱动结果，不证明像素的物理成像或完整硬件验收。

`status` 中所有内存值和 `stack_bytes` 单位均为字节。`page`、`pages` 为全书页码与总页数，统计完成前为 0；`count` 为页数统计状态：`idle`（无书或已完成）、`counting`、`waiting`（内存不足以读取下一章）、`failed`；`largest_psram` 为 PSRAM 最大连续空闲块。`crc32` 为当前画布的标准 CRC-32，兼容 `zlib.crc32()`；`revision` 仅来自已确认保存记录。`importing` 表示当前是否有资源导入，`received` 是该事务已接收字节数。状态不输出书籍正文、源锚点、凭据或配置原文。`file_head` 单独提供受限的资源首部诊断，沿用资源白名单与 `file` 的忙碌／无卡门控，不读取其余内容；输出可能含用户正文，仅保存在私有证据目录，不提交。它与 `file` 一样不重置空闲计时。

## 屏幕导出

先读取 `status`，要求 `pending=0`、`frame_ok=1`，并记录 `frame`、`bytes` 和 `crc32`。使用该帧号逐段执行 `screen`，每段长度 1–256 字节，按 `offset` 拼接 `hex` 数据。帧号变化、有新待呈现帧或上次刷新失败时，组件拒绝导出。完成后核对长度与 CRC32；不自动翻页，不提供整书导出。

画布为 684×1216，每行 342 字节，偶数 x 在低半字节，奇数 x 在高半字节；灰度 0 黑、15 白。转换为 8 位 PGM 时，将各半字节乘以 17。导出的只是当前 App 画布，不能用于判定面板残影、方向或真实物理刷新。

## 资源导入

主机工具可以执行 `upload <source> <destination>`，不会重置设备。例如：

```sh
python3 tools/device_console.py --port /dev/cu.TARGET_DEVICE upload local-book.epub Books/local-book.epub
```

端口必须替换为已确认设备。组件不查找设备、不控制主机串口；上面的命令只说明用法。主机先计算长度和 CRC32，串行发送有界数据块，再核对设备实际接收偏移。commit 成功后再次执行 file，确认正式文件摘要一致。主机请求失败时尝试 abort，设备在 30 秒没有导入活动或发现卡不可用时也终止事务。

所有写入由 `FileStore::begin_import()` / `FileImport` 管理，允许 1 字节至 64 MiB：

- `Books/` 下的 `.epub`、`.txt`，包括 `Books/Articles/` 中的文件。
- `Fonts/` 下的 `.ttf`、`.otf`、许可证 `.txt`。

目标和父目录使用既有安全路径检查，受控创建缺失父目录；拒绝绝对路径、上级路径、符号链接及非白名单类型。不会导入配置、凭据或进度，也不会覆盖已有文件。资源格式本身仍由对应阅读/字体/图片模块解析；CRC32 用于传输完整性，不能证明来源可信。

临时文件为 `<目标>.import.tmp`，使用排他创建，不采用或删除其他事务遗留的临时文件。接收完成后核对长度与 CRC32，执行 fsync，再流式读回校验，最后在同目录发布。发布前再次拒绝已有目标；host 使用 link 原子拒绝重名，ESP 使用默认拒绝重名的 FatFs rename。如果启用了 `CONFIG_FATFS_VFS_RENAME_REPLACES_DESTINATION`，设备会拒绝导入提交。发布后还会读回正式文件核对；任一步失败均不确认完成。发布后的同步失败可能留下完整正式文件，后续导入仍拒绝覆盖，可用 file 先查验。FatFs 介质掉电行为仍需真机测试，接口没有提供跨掉电的原子性保证。

不存在覆盖、任意删除、格式化、NVS、校准、重启或睡眠命令。

## 限制与依据

已查询乐鑫文档 MCP，并核对本机 ESP-IDF v6.1 的 `usb_serial_jtag_vfs.c`、`driver/usb_serial_jtag.h` 和 FreeRTOS `task.h`。官方资料：[控制台非阻塞 I/O](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/stdio.html#blocking-and-non-blocking-i-o)、[USB Serial JTAG 睡眠限制](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/usb-serial-jtag-console.html#sleep-mode-considerations)。手动浅睡期间 USB 不响应。平台在实际唤醒后短暂撤销 USB 上拉，恢复原配置以请求主机重新枚举；此恢复候选尚未真机验证，必要时仍需物理拔插。已有串口句柄需重新打开，不能通过这个连接承诺无人值守的睡眠/唤醒验证。取消入睡不会主动断开 USB。

本轮将组件的启用与关闭分支分别用当前 Xtensa 工具链和固件配置编译，均通过 `-Werror`。这只是目标文件检查；完整链接和实际设备命令结果由主任务另行记录。导入新增的 host 检查复用 `store_test`、`store_sync_test`，覆盖 CRC、读回损坏、外部同名、残留临时文件、路径越界及同步失败；没有由这些检查推断 TF 掉电安全。

## 主机客户端

USB 文件传输开启前会停止本诊断连接。MSC 与 USB Serial/JTAG 分时使用同一内部 PHY；电脑安全弹出后，需在设备上确认退出重启，才能恢复诊断与正常烧录。详见 [USB 文件传输](usb-storage-runtime.md)。

`tools/device_console.py` 使用 ESP-IDF Python 环境中已有的 pyserial，不安装驱动，采用官方 IDF Monitor 的无复位顺序：打开前 DTR/RTS 设为 true，打开后先将 RTS、再将 DTR 设为 false。本机已用连续两次 uptime 采样验证重连不重启。USB 枚举可能早于应用输入驱动安装，因此建立连接后先在总超时内重试只读 status，收到回复后才允许调用方发起动作。仅在诊断固件已运行后使用：

```sh
python3 tools/device_console.py --port /dev/cu.TARGET_DEVICE status
python3 tools/device_console.py --port /dev/cu.TARGET_DEVICE ls Books
python3 tools/device_console.py --port /dev/cu.TARGET_DEVICE open Books/lu-xun-aq-25332.epub
python3 tools/device_console.py --port /dev/cu.TARGET_DEVICE key K2
python3 tools/device_console.py --port /dev/cu.TARGET_DEVICE capture .local/device/frame.pgm
```

所选 Python 必须已有 pyserial。端口必须依据当前枚举结果选定；示例不代表固件已安装。客户端逐块核对帧号、偏移、长度和整帧 CRC，再写入新 PGM；已有输出文件不会被覆盖。控制字符、超长回复和导出期间变帧会失败，不能输出伪装为完整帧的文件。模拟传输已检查协议分帧、像素半字节顺序、变帧拒绝及已有文件保护；真实设备已验证状态、目录查询、无复位重连、两份许可证导入及未完成大文件取消；完整字体/EPUB 导入与画布导出另行验证。

## 实体输入与耗时诊断

实体触摸和 K1/K2/K3 在释放后派发一次动作，保留按下时的目标；取消不派发。按住期间若提交过其他画面，释放时取消手势，避免命中新页面控件。完整手势结束后才进入同步排版、刷新与保存，避免刷屏期间漏读释放导致下一次按下被当成持续按住。刷新仍为同步事务。独立的采样任务在刷新期间继续记录触摸；其中只有紧接着翻页的一次翻页点击会在刷新后补执行，其余在刷新期间开始的输入被丢弃。串口 `key` / `tap` 是应用层注入，不能证明触摸控制器的实体检测正常。

`Input` 日志记录实体序列、类型、阶段和坐标；`Input dispatch` 记录是否为刷新期间的输入（`queued`）、从抬起到派发的等待时间（`waited`）和排版/动作准备耗时；`Input superseded` 表示同一次刷新期间被更晚点击取代的点击；`Frame complete` 分别记录画布转换、面板供电、波形及呈现后保存耗时，单位毫秒。这些日志不输出正文、锚点或凭据。USB 文件传输期间日志仍按原规则停止。


## 翻页效果候选对比

默认关闭。设备已安装本批诊断固件后，使用 `python3 tools/device_console.py --port <已确认端口> waveform ripple`，关闭时使用 `default`；也可从「设置 → 阅读 → 翻页效果」选择。命令不立即翻页或全刷；平台在下一次呈现前准备候选，准备失败记录日志并回退原刷新档位。非正文及全刷继续原路径。重启恢复关闭。当前候选的屏幕效果、残影、灰度、耗时和实体操作均未验收。

每组使用同一本书、同一源位置、同一字号和「均衡」档位；切换后先按 K2 建立同样的清屏起点，再按相同顺序翻页至下一次自动全刷，分别记录默认 GL16 和水波纹，并在水波纹下分别核对上一页与下一页。观察中间白场、连续残影、抗锯齿边缘灰度和带图页；水波纹额外观察条带边界、方向和中断后恢复。保留照片或录像与原始串口日志，不以画布导出代替物理成像。

面板耗时对比需使用相同的旧计时诊断配置：启用 `CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE=y`、`CONFIG_PICO_PAGE_LEGACY_PERF_LOG=y`，关闭 `CONFIG_PICO_PAGE_PERF_METRICS` 与 `CONFIG_PICO_PAGE_TRACE_EVENTS`，核对生成的 sdkconfig。将每组非清屏翻页的 `Frame complete` 原始行分组保存，再用 `python3 tools/summarize_reader_perf.py <该组日志> --json` 汇总 `panel_draw`；全刷、失败和恢复另列。保留完整原始日志及分组规则，记录 commit、固件摘要、缓存冷/热、样本数、P50/P95/P99/最大值和资源数据。默认 Diagnostic 模式关闭旧计时，不能拿无样本汇总声称耗时已验证。

## 2026-10-11 · 轻扫动作注入

`python3 tools/device_console.py --port <串口> swipe left` 注入下一页，`swipe right` 注入上一页；设备协议为 `swipe left|right`。命令直接发送非触摸的 `Input::Next/Previous`，沿用应用上下文与忙碌限制，不模拟采样、坐标、时长或物理轻扫识别，不作为阈值或真机手感的验收证据。
