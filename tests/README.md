# 验证入口

按 `docs/environment.md` 准备固定字体/依赖，构建根目录 CMake 后运行 CTest。原有 13 个固定入口：power_key、core、store、store_sync、reader_flow、epub_probe、setup_network、content_worker、graphics、document、app、provision、sync。另有 1 个入口 wifi_upload_page 依赖 Node.js：找不到 `node` 时 CMake 不注册该入口，旧基线 CTest 只列出 13 个（新增诊断测试后的总数见下段）。

新增 diagnostics_release、diagnostics_performance、diagnostics_diagnostic 三个配置入口，以及 device_workbench（18 个 Python 回归）。含 Node 时共 18 项，无 Node 时 17 项。诊断埋点的共享内容路径可用 `-DPICO_PAGE_HOST_TRACE_EVENTS=ON` 开启后运行同一套测试；三种独立记录器配置仍全部验证。用法与证据边界见 [诊断工作台](../docs/diagnostic-workbench.md)。

主要覆盖：呈现/保存事务、旧代次拒绝、持久化失败重试、配置外部编辑、真实 EPUB 源位置、分页无半行、K2 全刷不改进度、应用层缺字库/读档/显示失败、排版取消/应用、网络取消与旧回包、账户变化后禁止采用旧候选，以及协议字段与受限 XPointer 映射。`app_test` 还覆盖内置说明的离线/无卡/无字体显示、翻屏边界、K2 全刷、再次打开和阅读记录不变，并分别运行同步及固件使用的异步应用模式。阅读页脚回归覆盖左右独立循环、热区边界/交界拖动、在途与过期触摸、正文/阅读记录/进度文件不变、统计前后真实绘制、百分比两位小数与舍入、无效时间和时区跨日；两种应用模式均执行。

测试 EPUB 均由脚本生成；不提交个人书籍。`app_test` 使用 setup_fonts 下载的完整文楷字体。`tools/check_koreader_mapping.py` 是独立的真实 CREngine 兼容性对照，不是模拟期望值。墨水屏、TF 掉电、网络真实账户与双向接续仍需按 `docs/acceptance.md` 验收。

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
