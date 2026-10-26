# 诊断构建与回归工作台

这是 Issue #37 的分阶段实现。PR #38 已交付编译隔离、操作计数、有界事件、持续串口采集、人工标记和 K2 全刷状态回归。阶段 C 第一批补充正文图片的解码、画布采用、驱动提交关联，以及人工操作时的网络交叠检查。完整场景编排、同步位置保护断言、连续改字号与重开恢复仍未完成；不能用当前报告宣称这些场景或物理屏幕已验收。

## 构建

先按 [环境说明](environment.md) 激活已有 ESP-IDF 6.1，然后在仓库根目录运行：

```sh
sh tools/build_firmware.sh release
sh tools/build_firmware.sh performance
sh tools/build_firmware.sh diagnostic
```

每种模式使用独立的 `build/firmware-<mode>/` 和其中的 `sdkconfig`，由通用 defaults 加 `firmware/configs/<mode>.defaults` 生成。没有参数的原构建入口不变，仍使用本地 `firmware/sdkconfig`。模式目录首次配置后也会保留生成的 sdkconfig，已有的 sdkconfig 优先于 defaults。构建入口在构建前后核对 `firmware/sdkconfig.defaults` 与模式 defaults 中的每一项是否在生效 sdkconfig 中取同样的值，不一致时报错并列出选项；此时移走该 sdkconfig 重新生成，或显式更新它。无参数入口的本地 `firmware/sdkconfig` 同样受此核对，本地只能改动 defaults 未指定的选项。核对 `build/firmware-<mode>/config/sdkconfig.h`，不能只凭文件夹名称判断模式。

| 模式 | 控制台 | PERF_METRICS | TRACE_EVENTS | LEGACY_PERF_LOG |
|---|---|---|---|---|
| Release | 关 | 关 | 关 | 关 |
| Performance | 关 | 开 | 关 | 关 |
| Diagnostic | 开 | 开 | 开 | 关 |

新指标调用使用 `PP_DIAGNOSTIC`，关闭后连参数求值、时钟读取和上下文构造一起删除；实现文件为空，不建立记录器、事件缓冲、轮询或任务。Performance 只有固定大小聚合记录器；Diagnostic 另有 64 个事件。正式构建没有新增 USB 命令。性能构建可通过调试器查看 `pico_page::diagnostics::recorder`；如需串口读聚合指标，可在隔离配置中额外启用诊断控制台，但必须将此配置差异记入结果，不能当作纯 Performance 基线。

已审计并编译保护主循环的输入、分发、帧完成、页数统计、Wi-Fi 阶段和网络任务资源明细，以及同步映射、HTTP 结果明细。内容 worker 的每次完成资源输出已移除，创建失败的资源证据保留为 WARN。`StageTimer` 和 ZIP 读/解析/checkpoint 的旧计时只在 `CONFIG_PICO_PAGE_LEGACY_PERF_LOG` 下执行。该兼容开关默认关闭，依赖诊断控制台且不能与 PERF_METRICS 同开；保留 `tools/summarize_reader_perf.py` 对旧日志的使用方式。没有关闭全局 ESP-IDF 日志，启动信息和 WARN/ERROR 仍保留。本次没有证据证明旧日志导致了卡顿。

## OTA 分阶段计时

启用 `CONFIG_PICO_PAGE_PERF_METRICS` 时，下载任务输出 `pico_ota_perf` 的 `start`、每 5% 的 `progress` 和终态 `end`，显示所有者另外输出逐帧 `frame`。计时使用 `esp_timer_get_time()`，字段以 `_us` 结尾的值均为单调微秒；这些串口输出位于聚合记录器的临界区之外。关闭开关时不读取时钟或输出日志，不改变正式发布配置。

`end` 的 `read_us` 是全部读取步骤的墙钟总和；当前每步可在 `http_step()` 内多次轮询 `esp_http_client_read()`，计时包含 TCP 等待、TLS 解密、重试间隔、显示门控及任务调度，不等同于单纯的公网传输时间；`write_us` 是全部 `esp_ota_write()` 的总和；旧顺序擦写实现包含逐扇区擦除，当前预擦除实现的擦除另计入 `begin_us`。`wait_us` 计下载任务等待 Flash/显示锁及输入事务锁的时间，`begin_us`、`digest_us`、`verify_us`、`connect_us` 和 `headers_us` 分别记录 OTA 初始化及预擦除、流式摘要、最终镜像校验、HTTPS 建连和响应头。`total_us` 从下载函数内准备 HTTP 连接到下载及校验返回，不包含之前的 Wi-Fi 连接、校时和清单检查，也不包含后续 HTTP 清理与页面确认。`read_calls` 是读取步骤数，一步可包含多次 SDK 轮询；`write_calls` 是实际写入调用数；两项 `max_us` 分别是最长读取步骤与最长写入调用的墙钟耗时；失败时同样输出已完成字节及对应阶段数据，不能将失败样本当作完整下载。

`frame` 的 `present_us` 覆盖完整平台刷新调用，另列等待 Flash 锁及已有 copy/power/draw 分项。HTTPS 与屏幕现由门控串行执行，等待显示的时间计入相应网络阶段墙钟，刷屏总耗时不能与下载阶段直接相加；下载任务的 `wait_us` 仅说明直接等待显示/Flash 锁及输入事务锁的时间，不包含网络门控等待。`begin_us` 包括初始块及后续预擦除，`erase_max_us` 为最长单块擦除；`write_us` 不再包含顺序扇区擦除。按 `start`/`end` 时间选取相交帧，完整帧时长与窗口内相交时长分开报告，避免把最后一次刷新在下载结束之后的部分算入下载总时长。`connect_us` 和 `headers_us` 累计探测及擦除后下载两次连接。原始字节、构建来源、生效配置、完整镜像大小及摘要、网络条件、样本数和未测项须随结果保留；5% 区间分布不是单次 4 KiB 读取或擦写的完整分布。

本项目使用的 ESP-IDF 为 v6.1；参考同版本的 [native_ota_example](https://github.com/espressif/esp-idf/blob/v6.1/examples/system/ota/native_ota_example/main/native_ota_example.c)、[advanced_https_ota](https://github.com/espressif/esp-idf/blob/v6.1/examples/system/ota/advanced_https_ota/main/advanced_https_ota_example.c#L400-L413)、[OTA 性能说明](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/system/ota.html#tuning-ota-performance)及 [Wi-Fi 性能与省电说明](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html)。官方示例用于对照机制，不替代本板的取消、显示、电源和内存验收。

## 指标与事件语义

记录器仅供任务上下文调用，禁止 ISR 使用。应用任务、网络 worker 和内容 worker 的写入由 ESP-IDF `portMUX` 跨核临界区串行化；临界区只读单调时钟并更新固定数据，不做 I/O、分配、格式化或等待队列。host 使用互斥锁测试同一记录器逻辑。没有新增任务、栈、优先级、亲和性或取消协议，原有任务仍独占业务对象。缓冲满时丢弃新事件并增加 `dropped`，聚合计数继续更新。最长新增临界区是一份固定事件或快照的更新/复制；实际微秒开销待真机测量。

| kind | 覆盖边界 | success 的含义 |
|---|---|---|
| 0 Frame | `platform::present` 开始至返回，所有成功和失败帧 | 驱动返回成功，不包括随后 App 回调，不证明物理屏幕 |
| 1 Input | 实体输入经 App 的 Begin/End 分发 | 路由函数返回，不保证该输入被业务接受；不包括触摸采样等待 |
| 2 Network | 同步请求（detail=0）或更新任务（detail=1）的联网／校时／传输包装函数 | 同步为无错误且 HTTP 2xx，清单检查另要求 HTTP 200，下载要求镜像验证成功；不代表位置映射、保存、安装或双向接续成功，不包括 Wi-Fi 管理和传书服务 |
| 3 Content | 首次 start 尝试至拒绝或 worker 返回，包含排队 | 无已报告错误/失败图片；保存任务另检查提交状态，不代表图片已用于画布 |
| 4 ImagePatch | 正文渐进图片通过当前页身份检查后，复制像素至画布并请求局部刷新 | 所有图片均已采用且刷新请求被接收；必须再关联 Frame 成功，才确认驱动提交 |
| 5 NetworkStage | 同步／更新任务的一个阶段：detail=1 联网，2 校时，3 HTTPS 请求，4 服务器解析，5 固件下载；request 与所属 Network 相同 | 该阶段的包装函数返回成功；HTTP 阶段成功只表示传输完成，状态码仍由 Network 判断 |

内容任务拒绝创建、资源不足、失败、取消均纳入计数。重复调用已开始任务的 start 不产生第二次操作。取消以现有取消标志判断；对于不可取消的保存，这表示完成时观察到了取消请求，仍需检查实际保存状态。result 编码为 success=0、failure=1、cancelled=2、timeout=3。记录器支持显式超时，当前固件包装层没有独立的超时原因类型，网络超时计入 failure；主机命令超时独立统计。不能从 timeout=0 推断网络从未超时。

每项聚合保存 started、active、四类终态数、所有终态的 total_us/max_us。未完成操作保留 active。时间单位是设备单调微秒，父子操作的墙钟不能相加。资源低水位在显式 metrics 查询时读取 ESP-IDF 已有的内部 RAM/PSRAM 历史最低余量。internal_min 是各内部堆（含 `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 储备池）各自历史最低值之和，各堆的最低值出现在不同时刻，因此它不是某一时刻的总余量，也不能与 `internal` 剩余量直接相减来估算瞬时占用；需要瞬时值时按堆分别读取；status 仍可查询连续块和应用/输入任务余栈。不在每次操作结束时扫描任务栈；内容和网络任务最低余栈仍需调试器或旧诊断配置专项采样。

事件含 seq、time_us、op、duration_us、task、kind、begin、result、session、layout、frame、request、detail。op 和 seq 是本次启动单调递增 ID；任务 ID 是固件任务句柄的整数表示。Frame 携带原始会话/排版代次、实际呈现序号和 frame token request，detail 为 FramePurpose。Content 的 request 是内容 generation，detail 为 ContentJob::Kind，可选的预取任务再加 256；Network 的 request 是网络操作 ID，Input 是实体输入序号。没有来源的上下文填 0，不伪造书籍/排版身份，不输出书名、路径或正文。

渐进图片 Content（detail=2）另携带来源正文的 session/layout。ImagePatch 沿用这些代次和内容 generation，frame 是局部刷新使用的 UI token request，可与后续 Frame 的 request 关联；不是物理呈现序号。detail 是本次任务成功解码的图片数；灰阶缓存命中为 0，不能据此声称本轮执行了解码。失败图片的提示画布仍会提交，但 ImagePatch 为 failure。旧结果在现有身份检查处被拒绝，不产生 ImagePatch。开启配置在现有请求上增加固定标量，不新增任务、队列、I/O 或等待；关闭配置删除这些字段和记录调用。

## 协议扩展

仅启用对应开关时提供：

- `PP <id> metrics`：每个 kind 一行 ENTRY 聚合计数（当前 6 行）和一行 OK，后者含 schema=1、queued、dropped、sequence、tx_failures、internal_min、psram_min。字段结构保持不变，先后新增 kind=4 和 kind=5；旧主机工具不识别这些事件，需使用同一源码配套的工具。新工具可以读取旧固件，但不能为缺少 ImagePatch 的记录确认图片链路，也不能把缺少 NetworkStage 的已取消请求算作网络交叠。
- `PP <id> trace`：取出最多 8 个事件作为 ENTRY，OK 返回上述队列/丢失状态。耗时与输出发生在显式命令处理时，不在事件写入路径中。
- `PP <id> trace_start`：在记录器临界区中开始新窗口，清空旧计数和事件；此前的任务 token 不计入新窗口。`trace_stop` 在同一临界区冻结事件与计数并返回含 `stopped=1` 的 metrics。仍在执行的业务任务正常继续，但在截止点计为 unfinished；后续操作不进入该窗口。主机收到截止序号后排空到该序号，才结束采集。
- metrics 与 trace 的整次响应共用 20 ms 传输等待预算；计入 IDF 驱动对 TX 互斥锁和环形缓冲的两次等待。超出后非阻塞发送，失败计入 tx_failures；该预算不包含任务被抢占和格式化的墙钟时间。
- status 新增 content_busy、save_busy、save_due、已保存位置的身份 anchor（资源、节点与偏移的 CRC32，不输出路径或正文）、生效正文字号 font_px，以及同步面板最近一次结果 sync（unchecked、no-record、same、different、unmapped、uploaded、uncertain、unauthorized、failed、link-failed、config-changed；开始检查时回到 unchecked）。sync 为 different 时，sync_order 为 1 表示远端位置在当前页之后，-1 表示在当前页之前，0 表示在当前页内；其他结果下为 0。pending=0 只说明无待呈现帧。
- `PP <id> file_head <卡内相对路径>`：读取资源首部最多 256 字节，返回 `bytes=<文件总长度> count=<本次字节数> hex=<十六进制原始字节>`，短文件及空文件按实际长度返回；不接受偏移或自定读取长度，不扫描或解码其余内容。仅允许既有 `file`／导入规则中的 Books、Fonts 和 LockScreens 资源，拒绝目录穿越、符号链接、非普通文件、超过 64 MiB 的资源以及配置、凭据、缓存和进度路径。与 `file` 共用无卡、正在导入及卡访问忙碌门控；调用前由主循环确保卡与文件所有权安全。可用 `python3 tools/device_console.py --port <串口> file_head 'Books/sample.txt'` 定位 BOM 或编码；输出可能包含用户正文，仅保存在私有证据目录，不提交。

`status/metrics/trace/trace_start/trace_stop/screen/ls/file/file_head` 是观察命令，不重置主循环空闲计时；否则连续采集会阻止延迟保存、分页和休眠。它们仍可能占用串口和 CRC 计算时间，Diagnostic 性能不等同于 Performance。保留原 `PP 0 FRAME` 主动通知和所有已有导入安全限制。trace 导出失败仍通过 tx_failures 和序号缺口暴露，不能悄悄丢失后显示通过。

翻页候选通过现有 `device_console.py waveform default|ripple` 或设备「翻页效果」页选择。它是改变选择的命令，不属于观察命令；忙态拒绝。单串口所有权不变，切换与工作台采集分开进行，不同时打开两个串口客户端。详见 [候选对比步骤](console-runtime.md#翻页效果候选对比)。

## 持续采集与人工标记

先准备独立的测试卡目录，素材优先使用 host 构建生成的 `build/host/document-fixtures/`；不要覆盖用户书籍或配置。安装候选固件需单独核对真实设备与分区，本工具不刷机。关闭其他串口程序，再运行（需 pyserial；当前 CLI 面向 Linux/macOS）：

```sh
python3 tools/device_workbench.py --port /dev/ttyACM0 \
  --firmware build/firmware-diagnostic --metadata .local/test-conditions.json \
  --output .local/runs/manual-001 --mode manual
```

metadata 示例中的值必须由操作者填写，不要填入账号或密码：

```json
{
  "hardware": "Read Pico，板卡标识与供电方式",
  "tf": "测试卡型号、文件系统及测试目录版本",
  "books_sha256": ["实际测试 EPUB 的 SHA-256"],
  "cache": "cold 或 warm，并说明缓存准备方式",
  "network": "离线，或网络条件与受控同步服务标识",
  "device_build_verified": true
}
```

`device_build_verified` 只能在确认设备实际安装了所记录镜像后填写 true；本工具核对构建入口生成的 `provenance.json`，其中包含构建前后的源码内容摘要、commit、dirty 状态及 ELF/bin/sdkconfig 摘要；来源缺失、产物被替换或当前源码不匹配时降为 inconclusive。单凭本地摘要不能证明设备安装状态。run.json 同时保留构建来源、当前 checkout、生效配置全文和模式。缺少素材/硬件/网络条件或固件摘要时，自动断言成功也会降为 inconclusive。

开始后单一读取线程持续保留原始字节。命令写入串行化，回复按 ID 分发，主动 FRAME 和普通日志都留存。沿用 `device_console.py` 的 RTS/DTR 无复位顺序，并使用串口独占锁；命令超时不关闭重开串口。输入 `mark 简短说明` 标记问题，`end` 结束；输出包含每个标记前后 10 秒的事件窗口。trace_start 回复中的 dropped/tx_failures 作为基线，之前的原始字节仍保留；测量窗口内新增丢失或计数回退均是缺口。窗口开始前已经启动的操作不进入本窗口；若收到缺少 begin 的终态，标记不可判定。结束时必须收到设备截止回复并排空尾部，否则不可判定。

断连或读取错误会唤醒等待命令并以 infra_error 结束；不自动重连或复位。重新连接后需启动一个新结果目录，并在 metadata 中写 previous_run 关联前一轮；两轮之间是证据缺口，不能拼成连续通过。超长行和尾部半行也会记录 gap，原始字节不丢弃。

## 最小自动回归

在设备上打开一份合成测试 EPUB，保持正文页，运行：

```sh
python3 tools/device_workbench.py --port /dev/ttyACM0 \
  --firmware build/firmware-diagnostic --metadata .local/test-conditions.json \
  --output .local/runs/k2-001 --mode refresh --timeout 15
```

状态机有明确期限：等待正文且显示、内容、保存和待保存状态均空闲；通过已有 `key K2` 正常路由注入；等待更新的帧号和已完成状态；独立断言驱动成功、ManualRefresh、FullClean、视图/页码/保存修订/画布 CRC 不变，再按原分块协议导出并校验画布。accepted、pending=0 或 FRAME success=1 都不能单独通过。非正文起点标为 skip，忙态不会被绕过。它验证 K2 不污染位置，不是图片或同步完成检查；实体触摸/按键时序另测。

每轮生成 run.json、serial.bin、chunks.jsonl、events.jsonl、marks.jsonl、mark-windows.json、report.html；自动检查成功时另存 canvas.pgm。chunks.jsonl 的 offset/size 精确指向原始字节区间，并保存主机单调时间和 UTC 纳秒时间。报告可离线打开。输出路径必须不存在，工具不会覆盖已有证据。

状态分 pass、fail、skip、inconclusive、infra_error。聚合分别列成功、失败、取消、超时和未完成；命令失败/超时与操作终态分开。设备在有待呈现帧时拒绝注入输入并回复 `ERROR busy`，这是忙态保护，单独计入 busy，不算命令失败。发现漏事件、溢出、未完成操作、设备重启或缺元数据，不显示全绿。完整事件样本才给 p50/p95/p99；有缺口时分位数为 null。当前分布按 kind 汇总所有终态（包含失败和取消），应同时查看样本数、max_us 和失败率，不能当成单一图片类型的成功耗时。旧的阈值阶段日志仅原样留存，不纳入新分布。

```sh
python3 tools/device_workbench.py --compare .local/runs/base/run.json .local/runs/new/run.json
```

比较要求两轮结果均为 pass、已核验的构建来源、固定脚本身份（`refresh-v1:K2x1`，自动场景另含书籍路径、起始页、起始位置的 anchor 和起始字号，例如 `reading-flow-v1:rounds=3,turns=4:book=…:page=7/12:anchor=8dccb332:font_px=46`）以及相同 kind/detail/终态样本数量；自动场景的 workload 必须包含有效的正整数 `font_px` 后缀；两份旧报告即使标识相同，缺少字号时也不能标为可比。自由人工操作没有固定工作负载，不能标为可比。另检查测试条件、dirty 状态和除四个诊断开关外的 sdkconfig；不一致、缺字段、无样本或有证据缺口会提示不可比。保留两边源码 commit 和原始统计，并用 same_source 区分同源码配置 A/B 与跨版本比较；不会把跨版本差异直接归因于诊断开关。

原始日志可能含路径、服务地址等隐私。结果默认放在已忽略的 `.local/` 下；不要将真实日志、书籍内容、账号或完整 sdkconfig 未经检查提交仓库。

## 自动场景

两个入口都通过现有诊断口的 `open`、`tap`、`key` 走正常应用路由，按状态和事件等待，不使用固定 sleep，也不绕过忙态保护。每次点击后核对结果视图；点击位置取自当前源码中 684×1216 布局的控件中心，布局变更后需同步更新 `tools/device_workbench.py` 顶部的常量，否则运行会因视图不符而超时结束。`--book` 是测试卡上合成测试 EPUB 的相对路径。只在合成测试书上运行：场景会翻页、保存进度并改动该书的单书排版设置。场景中途失败或超时时不做清理，设备可能停在同步页或保留加大一档的字号，下一轮开始前需人工确认状态。

### 场景 1：图片加载与同步交叠（`--mode image-sync`）

前提：测试书的已保存位置在一个每次都需要解码的图文页（例如 `image-flow.epub` 的衍生本）；Wi-Fi 和同步账户已配置，同步服务可达。只发起检查和取消，不上传，不采用远端位置。

```sh
python3 tools/device_workbench.py --port /dev/ttyACM0 \
  --firmware build/firmware-diagnostic --metadata .local/test-conditions.json \
  --output .local/runs/image-sync-001 --mode image-sync \
  --book Books/image-flow-test.epub --timeout 20
```

1. 打开测试书，立即进入工具→同步并检查远端位置，等待请求结束、位置映射完成且 status 的 sync 不再是 unchecked。结果为 different 时点「留在这里」，否则从页眉返回正文。只有 no-record、same、different、unmapped 算有效结果，其他结果（例如 HTTP 成功但应用判为 failed）为 inconclusive。断言保存修订不变。
2. 打开测试书，等到图片解码的 Content 开始事件出现后再打开一次；随后确认那次解码以取消结束。解码在第二次打开前已经完成时为 inconclusive，因为没有真正放弃一个进行中的图片任务。
3. 再次进入同步并检查，等到 HTTPS 阶段事件（NetworkStage detail=3）开始后点「取消同步」，在「正在结束同步」页点「继续离线阅读」。正文恢复后图片重新解码，此时被取消的请求尚未退出。等该请求结束并要求其终态为 cancelled；以其他终态结束说明取消没有赶在请求结束之前生效，结果为 inconclusive。断言保存修订和 anchor 不变。
4. 导出并校验画布，冻结记录器。结果须满足人工入口的同一条图片证据链（见下节），并且 `analysis.stale_image_patches` 为空：没有任何图片在更新会话或排版的帧已呈现之后被采用。该项不为空时，证据和运行条件完整才判为 fail，否则为 inconclusive。

请求在 HTTPS 阶段之前就结束、检查未成功或没有形成交叠时，结果为 inconclusive，不是 fail。等待期间设备进入错误页（例如帧失败后的「屏幕更新失败」）时，场景立即以 fail 结束并照常冻结记录器，使已记录的失败按完整证据判定。第 3 步有意让正文重画落在 TLS 握手期间，这是已知会偶发 EPD 行队列欠载的时序；出现时结果为 fail 并保留驱动日志，这是场景的检出，不是工具故障。页码在计数完成前未知，因此位置保护用保存修订和 anchor 判断。检查因网络、账户或服务不可用而失败时，这是前提未满足：场景把该请求及其阶段的失败标为 excused，结果保持 inconclusive；同一轮中的其他失败仍按规则判定。本入口不验证同步结果内容、上传、采用远端位置或 KOReader 往返。

### 场景 2：翻页、改字号与重开（`--mode reading-flow`）

前提：测试书的已保存位置距书末至少 `--turns` 页，并且是通过翻页到达的页面。场景开始时自行打开 `--book`，使各轮和 workload 标识对应同一本书。

```sh
python3 tools/device_workbench.py --port /dev/ttyACM0 \
  --firmware build/firmware-diagnostic --metadata .local/test-conditions.json \
  --output .local/runs/reading-flow-001 --mode reading-flow \
  --book Books/image-flow-test.epub --rounds 3 --turns 5 --timeout 20
```

每一轮：

1. 向后翻 `--turns` 页，每页等待帧完成并断言页码恰好加一、驱动成功。从恢复或跳转得到的位置出发时，一页可能跨两个已统计页，页码会重复或跳过一次；出现这种情况时结果为 inconclusive，其他页码变化为 fail；停留后等待保存，断言保存修订增加且 anchor 已不同于起始位置。再向前翻回，断言再次保存且 anchor 回到起始位置。
2. 进入排版，字号加一档并应用，等待重新分页并断言 font_px 确实变化；再减一档并应用。字号已是最大档时加一档不产生变化，场景改为先减后加；两个方向都不产生变化时判为 fail。断言 font_px、anchor、页码和总页数回到起始值。
3. 返回书库并重新打开测试书，断言 anchor、保存修订、font_px 和页码与返回书库前相同。页码不能唯一确定位置，以 anchor 为准。
4. 记录稳定态的内部 RAM、PSRAM 及其最大连续块。

所有轮次结束后，后续轮次的内部 RAM 稳定值比首轮低超过 4,096 B，或其最大连续块比首轮小超过 8,192 B 时判为 fail。PSRAM 中的缓存会正常增长，只记录不断言。

诊断口在有待呈现帧时拒绝输入，所以本入口的「连续翻页」是逐页等待后注入，覆盖显示完成、保存修订和重开位置；实体连续按键时保留一次后续翻页（D14）的行为无法由注入输入验证，仍需人工模式和实体操作另测。

## 图片与网络交叠：人工操作、自动检查证据

此入口只验证阶段 C 场景 1 的图片证据链，不验证远端位置采用、双向接续、所有取消路径或长期稳定性。先将构建生成的 `image-flow.epub` 放入隔离测试卡目录；首页含有效的灰阶图片，后续页含故意缺失的图片，正常成功采集应停在首页。首次采集前在 metadata 记录实际素材摘要和缓存条件，使用受控测试同步账户，不修改生产账户或用户书籍。工具不上传素材、不删除缓存、不自动输入凭据。

```sh
python3 tools/device_workbench.py --port /dev/ttyACM0 \
  --firmware build/firmware-diagnostic --metadata .local/test-conditions.json \
  --output .local/runs/image-network-001 --mode image-network
```

1. 开始采集后，在设备上打开测试图片页，按正常界面操作发起同步、取消或返回正文。忙态限制保持不变。
2. 可以输入 `mark 简短说明` 标记观察到的问题。结束前回到目标图文页，等待图片补刷和进度保存完成，再输入 `end`。
3. 工具检查正文、保存和任务状态，导出并校验当前画布，再冻结、排空记录器。画布导出前后的帧号、CRC、修订号或视图变化时，结果为 inconclusive；导出过程中设备或工具因帧变化拒绝继续时同样为 inconclusive，其他导出错误仍是 infra_error。
4. 报告的 `analysis.image_chains` 列出内容、图片采用、驱动帧及交叠网络操作的 ID。只有成功 Content、同一 session/layout/generation 的成功 ImagePatch、对应成功 Frame 和本轮截图帧全部相符，且该内容任务区间与网络任务区间严格相交，才具备通过依据。网络任务须为成功，或有同一 request 的 NetworkStage 落在该内容任务区间内：在入口处即被取消的请求也会留下一个 Network 区间，但没有执行联网、校时或 HTTP，不能当作交叠。`network_stages` 列出这些阶段操作的 ID。detail=0 的缓存命中不满足本入口的解码要求。

没有实际交叠、旧固件没有 ImagePatch、帧不匹配、漏事件或未完成任务均不可判定。证据和运行条件都完整时，已记录的失败、超时或驱动帧失败报告 fail，即使结束时的状态观察本身不可判定；证据有缺口、没有事件或缺少必需的条件与来源记录时仍为 inconclusive，与通过路径使用同一完整性判断。人工操作不使用固定 sleep 假设交叠，也不作为固定性能工作负载参与版本比较。真实交叠可能难以触发，需要重复独立采集；未触发不能伪造通过，也不能据此断言业务存在故障。网络操作仍仅证明 HTTP 包装函数运行，不证明同步正确。画布与驱动结果不证明物理屏幕。

## 可执行验证与真机待测

```sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel 4
ctest --test-dir build/host -N
ctest --test-dir build/host --output-on-failure
```

在原 14 项（含 Node 页面测试）基础上新增 3 项编译配置测试和 1 项 Python 工作台入口，共 18 项；无 Node 时仍少 1 项，应明确记录。记录器测试覆盖参数编译消除、四类终态、未完成操作、环形缓冲溢出及并发写入。Python 使用可控串口替身覆盖交错字节/主动事件/回复、过期回复、忙态错误、超时、断连、溢出、缺 begin、上下文变化、未完成、标记窗口、HTML 转义和 K2 独立断言。替身不是硬件验证。

共享内容路径的开启配置可用 `-DPICO_PAGE_HOST_TRACE_EVENTS=ON`；它同时启用 metrics。ASan/UBSan/LSan 应覆盖此配置和现有异步应用/内容 worker 回归。正式固件关闭验证应同时检查 sdkconfig.h、ELF/map 和 reader_core archive：Release 无 diagnostics 记录器/调用符号；Performance 无 64 事件数组；Diagnostic 有固定容量实例。

真机 A/B 使用同 commit、同设备、同 TF/书籍摘要、同冷/热缓存和同电源/无线条件。分别构建 Release/Performance，单独记录翻页、图片和同步的外部观测时延、内部/PSRAM 最低余量、连续块、各任务余栈；不要用 Diagnostic 串口查询期间的耗时冒充纯 Performance。Release 无内部追踪，需相同外部计时或调试器方法。保留原始样本、数量、p50/p95/p99/max，重复多轮。首轮同条件数据见 `docs/development-status.md` 的 2026-10-10「PR #39：Release/Performance 真机 A/B」记录：两模式中位数差异与单样本散布同量级，未得出可分辨的百分比开销。还需验证观察命令不阻止停留保存、休眠/USB 断连能报告缺口，以及 K2 全刷的实体屏幕效果。
