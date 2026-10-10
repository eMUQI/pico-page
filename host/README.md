# 桌面运行接口

`sh tools/run_host.sh` 构建并启动实际 C++ 阅读程序；依赖见 [运行说明](../docs/runtime.md)。`--card` 指定本机目录作为 TF，`--book` 指定卡内 EPUB 相对路径。

程序还支持 `--headless --script FILE`，逐步执行输入并输出真实帧：

```text
open Books/example.epub
k3
k2
tap 342 500
capture /absolute/path/tools.bmp
sleep
wake
```

`capture` 保存当前 684 × 1216 原始绘制图，不模拟墨水屏残影。网络请求只在交互界面明确检查/确认上传后发起；脚本模式不隐式联网。
