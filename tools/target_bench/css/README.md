# CSS 独立测量程序

此程序只执行 CPU、能力堆、单调计时与 USB 控制台输出，不初始化屏幕、PMU、TF 卡或 Wi-Fi，不重新配置 USB，不进入睡眠。它不是产品固件，不包含阅读或保存功能。任务沿用 `content_worker` 的 32,768 字节栈与优先级 1，CPU 240 MHz、八线 80 MHz PSRAM 及分配策略与板级 defaults 一致。构建只生成产物，不授权写入。

激活已有 ESP-IDF 6.1 后，在仓库根目录执行：

```sh
idf.py -C tools/target_bench/css -B "$PWD/.local/css-survey/target-build" \
  -D "SDKCONFIG=$PWD/.local/css-survey/target-build/sdkconfig" build
```

默认输入为原创合成 CSS。实书调研脚本的 `--bench-header` 可在 `.local/` 生成匿名数据头，再用 `-D "CSS_BENCH_DATA=$PWD/.local/css-survey/bench-inputs.h"` 构建。数据头、镜像、ELF、构建目录和串口原始日志均保留在 `.local/`，不得提交。头文件中仍可能包含私有选择器、标签和 CSS；匿名输出不使输入成为公开数据。

只有取得完整有效的原应用槽备份，重新读取分区表和 otadata，并确认当前启动槽后，才能在明确授权后单独写入该应用槽。不得运行全量 `flash`，不得写分区表、bootloader、otadata 或 NVS。每次写入独立 `verify-flash`，核对前 64 KiB 与 otadata 不变，结束后恢复原应用并确认正常启动。无人值守设备发生连接失败时执行任务约定的停止流程。

程序启动后等待 3 秒供主机重新打开串口，等待时间不计入测量。每组重复 5 次，输出 `css_allocate`、`css_parse`、`css_match`、`css_cascade` 和 `css_checkpoint_max` 单调微秒耗时；阶段不相加冒充阅读耗时。匹配阶段包括行内声明的层叠选择，cascade 阶段只解析最终值。最大检查间隔只在取消回调与每轮起止时更新。它覆盖构造及阶段间隔，包含调度、采样与让出开销，记录的是本次带埋点执行的观测值。检查点每约 2 ms 阻塞让出一个 tick，不修改看门狗。

每次重复使用局部堆最低值监视，内部 RAM、PSRAM 分别输出分配前余量和最低空闲字节。能力堆最低值为各堆的最低值之和，差额不能视作同一时刻的精确峰值。最大连续块的最小值来自检查点和阶段边界采样，可能漏掉采样之间的瞬时变化。栈余量是该任务启动以来的最低值。元素及结果数组先于每轮监视分配，属于驻留内存；每个计算样式的大小与元素数另计。解析池的申请字节不含分配器开销。重复之间重新建立样式表，未做产品缓存对比。

```sh
python3 tools/summarize_reader_perf.py .local/css-survey/target.log
```

原始日志包含匿名 sample/repeat 编号。按编号分别汇总 P50、P95、最大值及各内存指标，不能混合不同输入的分布。测量方法和尚未完成的设备证据见 [CSS 候选说明](../../../docs/development/original-layout.md)。
