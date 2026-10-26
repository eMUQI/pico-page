# 验证入口

按[开发环境](../docs/development/environment.md)准备固定字体和依赖，在仓库根目录配置、构建后运行 CTest。测试名称、数量和条件注册以当前构建目录为准，不在本文维护固定总数。

```sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel 4
ctest --test-dir build/host -N
ctest --test-dir build/host --output-on-failure
```

`-N` 只列举已配置的测试，不执行测试；增删入口或改变依赖后先重新配置。需要 Node.js 的入口在未找到 Node 时可能不注册，核对列表不能以跳过代替验证。诊断埋点可用 `-DPICO_PAGE_HOST_TRACE_EVENTS=ON` 开启；三种独立记录器配置和实际测试结果按[诊断工作台](../docs/development/diagnostic-workbench.md)执行与记录。

主要覆盖：呈现/保存事务、旧代次拒绝、持久化失败重试、配置外部编辑、真实 EPUB 源位置、分页无半行、K2 全刷不改进度、应用层缺字库/读档/显示失败、排版取消/应用、网络取消与旧回包、账户变化后禁止采用旧候选，以及协议字段与受限 XPointer 映射。`app_test` 还覆盖内置说明的离线/无卡/无字体显示、翻屏边界、K2 全刷、再次打开和阅读记录不变，并分别运行同步及固件使用的异步应用模式。阅读页脚回归覆盖左右独立循环、热区边界/交界拖动、在途与过期触摸、正文/阅读记录/进度文件不变、统计前后真实绘制、百分比两位小数与舍入、无效时间和时区跨日；两种应用模式均执行。

`sleep_io` 编译固件实际使用的 GPIO／IO 扩展器停放实现，故障替身覆盖后台探卡仍忙时在寄存器／GPIO 修改前拒绝浅睡、浅睡与关机的 CW_INT 区别、睡眠引脚范围、每一步准备／恢复失败、写入已生效但返回错误、触摸复位失败、重复调用及恢复后再次进入。它验证寄存器恢复与重试契约，不证明 PMU 实际唤醒、TF 电气行为、面板效果或功耗。

Linux `app_test` 使用链接包装注入当前书库目录的 `readdir` 与 `lstat` 失败，覆盖首项／中途 EIO、条目 EIO／ENOENT、正常 EOF 前残留 errno、空目录、512 项上限及界面重试；不修改生产 I/O 接口或真实文件。

测试 EPUB 均由脚本生成；不提交个人书籍。`app_test` 使用 setup_fonts 下载的完整文楷字体。`tools/check_koreader_mapping.py` 是独立的真实 CREngine 兼容性对照，不是模拟期望值。墨水屏、TF 掉电、网络真实账户与双向接续仍需按 `docs/development/acceptance.md` 验收。

## 整页图片缓存性能对照

`image_cache_bench` 是按需构建的 host 探针，不加入默认构建或 CTest。它运行真实异步 `ContentJob::Image`，使用已有合成 EPUB 中的 2×2 RGBA PNG，缩放到 684×1216；每组先执行两轮预热，再采集默认 40 对冷／热样本。冷路径只删除应用的图片缓存，不清空操作系统文件缓存。每次检查成功输出、冷／热像素摘要一致，以及冷路径一次解码、热路径零次解码；失败返回非零。

```sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --target image_cache_bench --parallel 4
build/host/image_cache_bench build/host/document-fixtures/image-cover.epub 40 > /tmp/image-cache.csv 2> /tmp/image-cache.log
python3 tools/summarize_reader_perf.py /tmp/image-cache.log
```

CSV 保留全部样本的耗时、整屏级分配次数、累计申请字节和像素摘要。计时从任务启动到 `wait()` 返回，包含 host 线程调度；普通 C++ `new`／`new[]` 申请至少 415,872 字节才计入分配指标，不包括 C 分配、对齐分配及小对象，不代表同时存活的堆峰值或设备内存。日志阶段带 `host_image_` 前缀且无慢样本过滤；汇总脚本中没有设备资源或溢出数据，不表示设备不存在这些问题。基线和候选使用相同配置、依赖、样本及统计方法，比较耗时时应结束其他构建任务，并交替运行两种版本。

异常回归复用 `app_test` 与 `content_worker_test`：前者覆盖旧格式、像素、摘要损坏、真实文件截断、块间取消、`RLIMIT_FSIZE` 短写及不可写缓存；后者编译 ESP_PLATFORM 内容路径，用能力堆桩验证热缓存门槛和冷路径拒绝。桩验证不替代设备上的实际分配或碎片验收。

## 顺序分段解析对照

`chapter_stream_probe` 对比原有 `load_chapter` 和 `read_next_chapter_part`；后者保留 ZIP 解压及 XML 状态，异步 `ReaderApp` 通过受控章节会话使用续读接口。探针按需构建，不新增 CTest 项。两条路径都复用已打开的文档，不读写正文缓存；计时包含解析与必要的容量收尾，不含开书、摘要生成、排版、绘制或面板。每次运行使用新进程和新文档，操作系统文件缓存未清空。

```sh
cmake --build build/host --target document_test chapter_stream_probe --parallel 4
build/host/chapter_stream_probe load build/host/document-fixtures/stream-events.epub 0 > /tmp/chapter-load.csv
build/host/chapter_stream_probe stream build/host/document-fixtures/stream-events.epub 0 > /tmp/chapter-stream.csv
build/host/chapter_stream_probe stream build/host/document-fixtures/stream-events.epub 0 --memory > /tmp/chapter-memory.csv
```

章节参数为从零开始的 spine 序号，`all` 逐项读取整本书。CSV 逐段记录耗时、原书回调返回字节、实际送入 XML 的字节、保留的 miniz 分配、取消检查间隔及正文／全部 Reading 字段的 SHA-256；摘要只用于同一 host 的语义对比，不写出书籍正文。标准错误输出进程峰值 RSS；失败返回非零。测量时结束构建与其他测试，交替运行两种模式并保留全部样本。可将各样本的 `elapsed_us` 转为 `reader_perf: stage=host_chapter us=<值> min_us=0`，使用 `tools/summarize_reader_perf.py` 汇总分位数。

`--memory` 只支持普通 macOS host 构建，通过所有 malloc zone 统计采样；不要将其耗时混入常规计时。`heap_retained_bytes` 为释放输出 Chapter 后相对于开书完成时的堆增量，`heap_sampled_peak_bytes` 为检查点与返回时观察到的最大增量，可能漏掉瞬时分配峰值；两者都包含进程内其他分配，不能当作设备 PSRAM 峰值。无采样时这些字段为零，表示未测。`zip_retained_bytes` 仅计 miniz 的仍存活载荷，不包含 XML、容器、分配器开销及文档原有 ZIP 索引。

现有 `document` 回归逐段核对文本、块、源锚点、链接、图片、分段标志和末段判定，覆盖 stored／deflate、UTF-16、实体、跨字符回调及最后一次 XML 输入中的暂停。取消、预算拒绝、随机访问、移动所有权、关闭、截断、坏 XML 和 CRC 失败必须清空输出并释放游标。超过 64 KiB 仍无完整 XML 事件的输入停止续读，验证同一有效正文仍可由原路径读完；这限制未处理输入，不是 XML 或总堆的完整上限。`app` 回归覆盖开书向章节任务移交游标、不重复开包、排队取消、换书和休眠／USB 后会话释放。设备内存、慢 TF、实体输入、睡眠／USB 交接及输入到正文呈现时间另按设备专项验证。

## 首次引导

`onboarding_test` 覆盖新卡三屏、两处跳过、K2／左右键和轻扫、排队及重复尾部、同屏休眠恢复、重看不写标记及错误返回、无卡与提交失败重启、旧配置／历史／书旁进度与备份、扫描上限和符号链接保守回退。可传输出目录导出四张 PGM（欢迎、练习、工具、说明首页）供视觉检查。既有 app 场景的 Driver 在全新夹具首次呈现后显式跳过引导，继续原场景；字体取消仍比较进入编辑前的完整配置字节，不再假定配置文件必须不存在。
