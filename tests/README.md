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

`sleep_usb_console` 编译固件实际使用的 USB 浅睡交接实现，替身按 ESP-IDF 6.1 的引脚快照／恢复顺序执行。覆盖准备取消、拒绝或失败、拒绝后成功、多个周期唤醒、成功后失败、原 PHY 关闭、原电阻与覆盖状态恢复、重复恢复，以及短睡眠补足断开间隔和长睡眠不额外等待。它验证寄存器与成功标志的边界，不证明真实 USB 电气断开、电脑枚举或串口响应。

Linux `app_test` 使用链接包装注入当前书库目录的 `readdir` 与 `lstat` 失败，覆盖首项／中途 EIO、条目 EIO／ENOENT、正常 EOF 前残留 errno、空目录、512 项上限及界面重试；不修改生产 I/O 接口或真实文件。

封面与锁屏失败回归在 `app_test`、`content_worker_test` 中验证类别与文案独立、PNG 内部分配失败、永久负缓存的 SHA-256 绑定、I/O 有界重试、取消与旧代次隔离。自定义锁屏空闲重试测试观察超过三个 5 秒间隔，再验证显式休眠仍可恢复。

测试 EPUB 均由脚本生成；不提交个人书籍。`app_test` 使用 setup_fonts 下载的完整文楷字体。`tools/check_koreader_mapping.py` 是独立的真实 CREngine 兼容性对照，不是模拟期望值。墨水屏、TF 掉电、网络真实账户与双向接续仍需按 `docs/development/acceptance.md` 验收。

## EPUB 文件探针

在仓库根目录运行。探针使用 Python 3.11+，输出指定章节的正文文本、Unicode 标量位置映射和原书摘要。以下命令使用两章合成样本。

```sh
python3 -m unittest discover -s tests -p test_epub_probe.py -v
probe_dir=$(mktemp -d)
python3 tests/make_epub_fixture.py "$probe_dir/sample.epub"
python3 tools/epub_probe.py "$probe_dir/sample.epub" --spine 0 > "$probe_dir/chapter-0.json"
python3 tools/epub_probe.py "$probe_dir/sample.epub" --spine 1 > "$probe_dir/chapter-1.json"
```

探针的源节点标识仅用于检查 XML 文本映射。核对 KOReader XPointer 时使用[真实引擎对照](../docs/development/sync-runtime.md#真实引擎对照与复现)。

## 大目录 EPUB 工作量对照

Linux `large_epub_work` CTest 运行 `large_epub_probe`，复用构建时生成的原创 300／3,000 章 EPUB（303／3,030 项 NCX），另有带同一 2×2 PNG 的对应封面样本。覆盖直接开书、已开书末章跳转、真实异步 `ContentJob::Open` 恢复 75% 处的已保存锚点、读取／缺失封面、四本书的冷封面任务，以及空闲／受压的实际 `reclaim_caches`。受压场景只在 host 替代内存压力判定，页索引仍小于既有 1 MiB 上限；它不模拟 ESP-IDF 堆扫描耗时。

```sh
cmake --build build/host --target large_epub_probe --parallel 4
build/host/large_epub_probe build/host/document-fixtures --measure 3 > /tmp/large-epub.csv 2> /tmp/large-epub.log
python3 tools/summarize_reader_perf.py /tmp/large-epub.log
ctest --test-dir build/host -R '^large_epub_work$' --output-on-failure
```

CSV 保留每个样本及 ZIP 初始化／建索引、OPF、spine、目录阶段的计数和单调微秒耗时。阶段耗时包含嵌套阶段，不应相加；阶段工作量归属最内层命名阶段。计数包括项目 ZIP 文件名 API 调用（含长度查询）、项目 ZIP 索引比较、spine 定位比较、解压回调提供的字节、提供给 XML 的字节、文档 ZIP 读取字节、页索引统计访问和回收访问。miniz 内部排序比较、完整身份摘要的直接文件读取不计入对应计数；其复杂度须结合源码判断。

探针通过 Linux 链接包装和 C++ 分配替换统计普通／对齐 `new`、项目及静态依赖的 malloc/calloc/realloc/posix_memalign；固定指针表不递归分配。峰值为本次捕获中新申请且同时存活的请求字节，不含捕获前驻留对象、分配器开销及动态链接库内部的 C 分配，不能作为设备堆峰值。异步路径从启动计时到 join，包含 host 调度；OS 文件缓存未清，程序层封面缓存按轮清理。测量前后不要并行构建，原始全部样本使用 `min_us=0` 汇总。

不带 `--measure` 时检查语义、取消、ZIP 工作量增长和受压回收访问上界；`--measure` 只关闭复杂度上界，允许在未修复基线上采样，其他断言保留。断言不依赖墙钟时间。当前正文、源锚点和保存记录必须保持，回收保留当前章页索引。host 埋点默认不捕获，在 ESP_PLATFORM 编译中完全移除；不改变固件任务、队列或堆查询。

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

## 平台边界

`architecture` 检查应用组件不导入 Read Pico 实现，契约不导入 SDK/BSP 头。host 构建逐个编译 `platform_contract` 公共头，避免依赖头文件顺序；`display_network_gate` 编译实际端口实现，验证网络接收持有、显示延期、显示优先、撤回及完成释放。`core` 和 `app` 直接使用共享 `reader_input` 验证手势和休眠准备策略。`display_compatibility` 显式定义 `NDEBUG`，使用实际应用画布和 Read Pico 尺寸，验证正常尺寸、宽高颠倒但字节数相同、单轴不匹配及无效尺寸；校验失败不会被 Release 的断言裁剪掉。`architecture` 另检查 provider 不依赖应用输入策略，并通过 CMake 读取 profile，核对两个 OTA 槽大小。实际 ESP-IDF 应用循环仍须固件构建，设备侧刷新、输入和电源交接另做真机验收。

## 首次引导

`onboarding_test` 覆盖新卡三屏、两处跳过、K2／左右键和轻扫、排队及重复尾部、同屏休眠恢复、重看不写标记及错误返回、无卡与提交失败重启、旧配置／历史／书旁进度与备份、扫描上限和符号链接保守回退。可传输出目录导出四张 PGM（欢迎、练习、工具、说明首页）供视觉检查。既有 app 场景的 Driver 在全新夹具首次呈现后显式跳过引导，继续原场景；字体取消仍比较进入编辑前的完整配置字节，不再假定配置文件必须不存在。

## 页脚白底维护驱动

显示补丁验证使用 `cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug -DPICO_PAGE_TEST_BSP_WAVEFORM=ON`；sanitizer 配置同样开启该选项。配置会调用 `tools/setup_bsp.sh` 准备并校验固定 BSP，首次需要 GitHub 网络；准备失败直接停止，不能以跳过代替通过。CI 的 Debug 与 sanitizer 均启用。

`footer_waveform` 编译实际 BSP 初始化及裁剪代码，遍历全部 256 种旧/新灰阶组合，核对默认 GL16 的第 33 相、完整 GL16 的第 45 相白→白维护驱动（从零计）、Gray8 无新增脉冲、变化像素序列及 Overlay 的非白对角保持与白底维护。`changed_pixels_waveform` 穷举掩码位序、边界、非法几何及禁用状态；`core` 验证 FullClean、ManualRefresh、失效基准及 Overlay 的实际帧路由。分配失败仍由既有驱动退回未掩码波形，分配与恢复流程沿用原实现；真机低内存、闪烁、灰底和恢复效果按 DP08／DP10 验收。

## CSS 独立原型

`css` CTest 直接调用独立模块，验证作者层叠、选择器祖先回溯、单位基准、错误恢复、拒绝不支持的简写／at-rule、容量、取消及分配失败，不经过正文排版。`css_survey` 以原创 ZIP fixture 检查只读关联、导入循环、HTML 调研回退与匿名输出。不会读取用户私有样本，Linux 和 macOS 均注册。

```sh
cmake --build build/host --target css_probe --parallel 4
build/host/css_probe /path/to/local.css
build/sanitized/css_probe --fuzz-seconds 30
python3 tools/survey_epub_css.py /path/to/private-library .local/css-survey-new \
  --probe build/host/css_probe --bench-header .local/css-survey-new-inputs.h
```

调研输出目录必须尚不存在，脚本不覆盖旧证据；书籍只读，CSS、数据头和 JSON 必须留在忽略目录。`--probe` 既检查所有 CSS 和提取的行内声明，也检查每个 HTML/XHTML 的关联样式集合。原始统计不提交，公开结果只给匿名汇总。目标程序及构建方法见[测量程序](../tools/target_bench/css/README.md)，支持和证据边界见[候选说明](../docs/development/original-layout.md)。
