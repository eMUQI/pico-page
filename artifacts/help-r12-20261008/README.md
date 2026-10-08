# R12 内置使用说明

9 张 PNG 来自真实 `ReaderApp` host 画布：设置入口和 8 屏说明。
捕获时没有 TF 卡目录、用户字体或网络连接，使用固件内置字体。
截图不是设备屏幕或新用户操作验收。

沿用现有顶栏、列表行、分段文字和翻屏页脚；原稿没有帮助页面，新增入口位于设置首页的空余区域。
K1/K3 和触摸页脚翻屏，K2 全刷，左上角返回设置。首尾不可继续翻屏，再次打开从第 1 屏开始。

在仓库根目录复现：

```sh
source /workspace/cloud-tools/activate-host.sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel 4
python3 artifacts/help-r12-20261008/render.py
```

`render.py` 使用已有 host 构建的编译/链接参数；在其他机器上按 `docs/environment.md` 配置工具链即可。
临时捕获工具与原始 PGM 保存在临时目录，结束后清理，不向书库写入说明或阅读记录。
