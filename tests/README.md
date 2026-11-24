# 验证入口

按[开发环境](../docs/development/environment.md)准备固定字体和依赖，在仓库根目录配置、构建后运行 CTest。测试名称、数量和条件注册以当前构建目录为准，不在本文维护固定总数。

```sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel 4
ctest --test-dir build/host -N
ctest --test-dir build/host --output-on-failure
```

`-N` 只列举已配置的测试，不执行测试；增删入口或改变依赖后先重新配置。需要 Node.js 的入口在未找到 Node 时可能不注册，核对列表不能以跳过代替验证。诊断埋点可用 `-DPICO_PAGE_HOST_TRACE_EVENTS=ON` 开启；三种独立记录器配置和实际测试结果按[诊断工作台](../docs/development/device-diagnostics.md)执行与记录。

测试 EPUB 均由脚本生成；不提交个人书籍。`app_test` 使用 setup_fonts 下载的完整文楷字体。`tools/check_koreader_mapping.py` 是独立的真实 CREngine 兼容性对照，不是模拟期望值。墨水屏、TF 掉电、网络真实账户与双向接续仍需按 `docs/development/acceptance.md` 验收。

## 本机阅读统计

`reading_statistics` 验证单调计时、日期拆分与队列确认，`statistics_store` 验证副本身份、清除与文件恢复，`statistics_app` 通过应用的输入、面板确认和异步内容任务验证阅读与统计页面。单独复查或导出合成画面时，在仓库根目录运行：

```sh
ctest --test-dir build/host -R '^(reading_statistics|statistics_store|statistics_app)$' --output-on-failure
mkdir -p artifacts/reading-statistics
build/host/statistics_app_test build/host/document-fixtures .local/card/Fonts/LXGWWenKai-Regular.ttf artifacts/reading-statistics
```

最后一个参数指定 PGM 输出目录。省略该参数只执行断言。PNG 画面见[阅读统计](../docs/development/ui-implementation.md#阅读统计)。

## 公开 EPUB 样本

阅读与同步对照另用两本 Project Gutenberg 的 EPUB 3，下载后放在忽略目录 `.local/public-books/`，不加入 Git。上游可能重新生成文件，结果只适用于下列摘要。书内权利声明为 `Public domain in the USA.`，使用条件见书内许可和 [Project Gutenberg 许可页](https://www.gutenberg.org/policy/license.html)。

| 作品 | 下载 | SHA-256 |
|---|---|---|
| 鲁迅《阿Ｑ正傳》 | [25332 EPUB 3](https://www.gutenberg.org/ebooks/25332.epub3.images) | `5dcf5341be4b59461f61854c023ddcc38af52655dd8e60bed99bbec241545125` |
| 鲁迅《吶喊》 | [27166 EPUB 3](https://www.gutenberg.org/ebooks/27166.epub3.images) | `373fcf0e86d48201b0ba317e15e5ae7dbdf3d33f26223c88950341395c7f2ba4` |

用生产解析器和渲染器逐页检查分页、缺字和图片绘制：

```sh
sh tools/check_public_books.sh .local/card/Fonts/LXGWWenKai-Regular.ttf \
  .local/public-books/lu-xun-aq-25332.epub .local/public-books/lu-xun-nahan-27166.epub
```

同步位置对照见[真实引擎对照](../docs/development/sync-runtime.md#真实引擎对照与复现)。

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

## BSP 波形测试

显示补丁验证使用 `cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug -DPICO_PAGE_TEST_BSP_WAVEFORM=ON`；sanitizer 配置同样开启该选项。配置会调用 `tools/setup_bsp.sh` 准备并校验固定 BSP，首次需要 GitHub 网络；准备失败直接停止，不能以跳过代替通过。CI 的 Debug 与 sanitizer 均启用。

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
