# 简化后的电源菜单

截图来自实际 `ReaderApp` C++ 画布，尺寸 684 × 1216，使用内置回退字体。
合成 EPUB 和临时 TF 卡目录用于演示；未调用网络或硬件断电接口。

- `menu.png`：选择后直接执行，无正常二次确认。
- `save-failed.png`：模拟最近阅读记录无法写入 TF 卡，提供返回和明确继续。
- `shutdown.png`：本地保存成功后的收尾画面。
- `manual-off.png`：关闭文件后以回退字体展示手动关机提示。

根目录 host 构建后执行 `python3 artifacts/power-menu-simple-20261008/render.py`。
脚本复用 `build` 中的 CMake include/link 参数编译 `render.cpp`，仅用 Python 标准库
将原始 8 位灰阶画布无缩放编码为 PNG。不能作为墨水屏光学效果、PMU 断电或重启的真机证据。
旧目录 `artifacts/power-menu-20261008/` 中的二次确认页保留为历史记录。
