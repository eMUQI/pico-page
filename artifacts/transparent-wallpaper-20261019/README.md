# 透明 PNG 休眠图片本地核对

2026-10-19，基于 `aa0b05a7aac745c879cfa339532e898a41ad63df` 的未提交实现。画布为真实 host 渲染结果，使用 `tests/image_fixtures.h` 中原创 2 × 2 RGBA PNG，黑、白、灰及透明象限仅用于核对合成，不是推荐壁纸。原设计文件未修改。

`power-off.png`、`power-on.png` 覆盖草稿开关与布局；`preview-on.png` 覆盖以进入预览前设置页为底；`sleep-off.png`、`sleep-on.png` 与对应 `reading-before-*.png` 覆盖白底和透明保留正文；`reading-after-on.png` 覆盖唤醒恢复。截图不证明实体面板灰阶或输入。

`capture.txt` 可交给 `build/host/pico_page --card .local/wallpaper-20261019/card --book Books/Demo.txt --headless --script artifacts/transparent-wallpaper-20261019/capture.txt`。该本地卡目录使用合成文本、上述 PNG 和已有测试字体，不提交字体与卡数据；需按脚本中的文件名准备输入。capture 输出 BMP，PNG 由系统 sips 转换。

## host 冷热路径对照

macOS AppleClang Debug、现有 APFS 目录，单独运行临时探针 `host-perf-probe.cpp`，同一 PNG、同一 684 × 1216 输出，5 轮串行，每轮先清理本探针卡的派生缓存，再按冷白底、热白底、热叠加顺序执行；热叠加使用灰度 7 底图。测量包含 worker 启动、文件校验、缓存读写及合成，不包含面板、网络或实际 TF 卡；2 × 2 源图不能代表大 PNG 解码成本。

| 路径 | 样本 | P50 ms | P95 / 最大 ms | 每轮解码次数 |
| --- | ---: | ---: | ---: | ---: |
| 冷缓存，白底 | 5 | 10.957 | 16.391 | 1 |
| 热缓存，白底 | 5 | 7.347 | 7.810 | 0 |
| 热缓存，叠加 | 5 | 6.336 | 6.479 | 0 |

原始输出为 `host-perf-raw.log`。`host-perf-stages.log` 仅将同一探针样本改写为现有汇总器可读取的格式，明确不是串口日志；`python3 tools/summarize_reader_perf.py artifacts/transparent-wallpaper-20261019/host-perf-stages.log --json` 产生 `host-perf-summary.json`，阈值为零，包含全部 15 个样本。5 个样本的高分位数仅供排查。没有设备堆、栈或总峰值测量。

该透明缓存文件为 1,663,628 字节，其中图层载荷 1,663,488 字节；4 位输出画布 415,872 字节。冷缓存序列化可能同时持有两份图层，载荷约 3.18 MiB；编码输入会先释放，其他主画布、字体、显示缓冲及系统分配另计。空闲预转换完成后不保留图层；不透明图片保持 4 位缓存。设备内存、冷/热耗时、取消延迟、慢卡与实际面板观感仍按 PW07 验收。
