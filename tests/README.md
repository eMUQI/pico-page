# 验证入口

按[开发环境](../docs/development/environment.md)准备固定字体和依赖，在仓库根目录配置、构建后运行 CTest。测试名称和条件注册以当前构建目录为准。

```sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel 4
ctest --test-dir build/host -N
ctest --test-dir build/host --output-on-failure
```

`-N` 只列举已注册的测试；需要 Node.js 的入口在找不到 Node 时不注册，不能以跳过代替验证。显示补丁验证加 `-DPICO_PAGE_TEST_BSP_WAVEFORM=ON`，配置时调用 `tools/setup_bsp.sh` 准备固定 BSP，首次需要网络，CI 的 Debug 与 sanitizer 均启用。诊断埋点用 `-DPICO_PAGE_HOST_TRACE_EVENTS=ON`，见[设备诊断](../docs/development/device-diagnostics.md)。

测试 EPUB 由脚本生成，不提交个人书籍；`app_test` 使用 `setup_fonts` 下载的完整文楷字体。墨水屏、TF 掉电、真实账户与 KOReader 接续按[验收清单](../docs/development/acceptance.md)另行验证。

## 公开 EPUB 样本

阅读与同步对照使用两本 Project Gutenberg 的 EPUB 3，下载到忽略目录 `.local/public-books/`，不加入 Git；结果只适用于下列摘要。

| 作品 | 下载 | SHA-256 |
|---|---|---|
| 鲁迅《阿Ｑ正傳》 | [25332 EPUB 3](https://www.gutenberg.org/ebooks/25332.epub3.images) | `5dcf5341be4b59461f61854c023ddcc38af52655dd8e60bed99bbec241545125` |
| 鲁迅《吶喊》 | [27166 EPUB 3](https://www.gutenberg.org/ebooks/27166.epub3.images) | `373fcf0e86d48201b0ba317e15e5ae7dbdf3d33f26223c88950341395c7f2ba4` |

```sh
sh tools/check_public_books.sh .local/card/Fonts/LXGWWenKai-Regular.ttf \
  .local/public-books/lu-xun-aq-25332.epub .local/public-books/lu-xun-nahan-27166.epub
```

同步位置对照见[真实引擎对照](../docs/development/sync-runtime.md#真实引擎对照与复现)。

## 按需构建的探针

以下程序不进默认构建，用于定位性能或工作量问题；参数见各自源码或 `--help`，日志用 `tools/summarize_reader_perf.py` 汇总。host 计时包含线程调度，分配统计不等于设备堆峰值，不能代替真机测量。

| 目标 | 用途 |
|---|---|
| `tools/epub_probe.py` | 输出指定章节的正文、Unicode 标量位置映射和原书摘要 |
| `large_epub_probe` | 300／3,000 章合成 EPUB 的开书、跳转、封面和缓存回收工作量（CTest `large_epub_work` 只检查上界） |
| `image_cache_bench` | 整页图片缓存冷／热路径耗时与像素一致性 |
| `chapter_stream_probe` | 原路径与顺序续读的章节解析对照 |
| `css_probe`、`tools/survey_epub_css.py` | CSS 模块的单文件检查、模糊测试和本地书库匿名调研；调研输出放忽略目录，书籍只读 |
