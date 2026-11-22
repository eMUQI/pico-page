# 桌面开发指南

桌面程序与固件共用 C++ 阅读逻辑，以 684 × 1216 灰阶窗口替代硬件显示接口，适合调试交互、解析和排版。它不模拟墨水屏波形、残影、触摸精度或耗电，不能代替真机验收。设备操作见[使用指南](../../docs/user/runtime.md)。

## 环境与启动

需要 CMake 3.20+、C/C++17 编译器、Python 3.11+、pkg-config、SDL2、OpenSSL 和 libcurl。Linux 系统包可使用 `libsdl2-dev libssl-dev libcurl4-openssl-dev pkg-config`；本机工具链记录见[开发环境](../../docs/development/environment.md)。

在仓库根目录执行：

```sh
python3 tools/setup_fonts.py
sh tools/run_host.sh
```

字体与 FreeType 按固定版本下载并核对 SHA-256，不安装或替换全局工具链。本地演示卡位于 `.local/card/`，可把测试 EPUB、TXT 放入其中的 `Books/`。演示内容为原创测试文字；不要提交个人书籍、配置凭据或私有日志。

`--card` 指定本机目录作为 TF 卡，`--book` 指定卡内 EPUB 或 TXT 的相对路径。窗口中的交互与设备逻辑共用，空格模拟休眠和唤醒。关闭窗口会重试保存未保存的进度；遇到保存错误时，先在界面重试并检查卡目录。

主机直接使用现有网络，不读取设备的 Wi-Fi 配置。KOSync 仍需配置测试账户，操作与上传确认规则见[使用指南](../../docs/user/runtime.md#与-koreader-接续位置)。

## 脚本与截图

程序支持 `--headless --script FILE`，逐步执行输入并输出真实帧：

```text
open Books/example.epub
k3
k2
tap 342 500
capture /absolute/path/tools.bmp
sleep
wake
```

`capture` 保存当前 684 × 1216 绘制图，不模拟墨水屏残影。实现截图放入被 Git 忽略的 `artifacts/`。网络请求只在明确检查或确认上传后发起，脚本模式不隐式联网。

## 测试

在仓库根目录执行：

```sh
python3 tools/setup_fonts.py
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel 4
ctest --test-dir build/host -N
ctest --test-dir build/host --output-on-failure
python3 tools/check_docs.py
```

`ctest -N` 列出实际注册的测试。缺少字体或 Node 等条件依赖时，注册数量可能减少，不能把较少的全部通过当作完整通过。测试覆盖与资源准备见[测试说明](../../tests/README.md)，sanitizer 参数见[开发环境](../../docs/development/environment.md#host-与文档检查)。

文档检查只验证文档、设计文件、配置示例和本地链接，不验证固件行为。CI 按 PR 改动路径选择检查，具体规则见[GitHub Actions](../../docs/development/environment.md#github-actions)。固件修改仍需相应的固件构建和真机验证。
