# 调研参考

下列项目影响过 Pico Page 的设计取舍。这里只记录借鉴了什么思路，不表示复制了代码；实际进入产物或改编的第三方内容见[第三方依赖](../development/dependency-review.md)。开发分支会变化，引用具体实现时重新核对 commit 和许可。

## 硬件与协议

- [Read Pico 官方固件](https://github.com/MindReset/read_pico_firmware)：板级、PMU、epdiy 与波形组件的来源，构建以 [BSP 审查](../development/bsp-review.md)锁定的 commit 为准。
- [KOReader](https://github.com/koreader/koreader)：KOSync 协议与 XPointer 位置（`plugins/kosync.koplugin`、[CREngine](https://github.com/koreader/crengine)）、伴随目录式的文件配置（`docsettings.lua`）、休眠图片轮播与回退（`screensaver.lua`）、轻扫方向判定、TXT 章节标题识别（CREngine `lvxml.cpp`）。KOReader 为 AGPL-3.0，CREngine 为 GPL，只参考行为。
- [ESP-IDF 文档](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/)：外部 RAM、睡眠模式、任务优先级与看门狗。按实际使用的 IDF 版本查对应文档。

## 阅读器实现

- [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader)：章节内可见文字偏移与 KOReader XPath 转换、章节缓存、CSS 解析的固定池与部分结果状态、PNG 行缓冲写入。
- [PapyriX](https://github.com/bigbag/papyrix-reader)：状态拥有数据、视图只绘制；分批缓存与协作取消。
- [CrossMux](https://github.com/0x1abin/crossmux)：同硬件对照；轻扫不激活下方条目、内置常用汉字子集的做法。
- [FreeInk SDK](https://github.com/0x1abin/freeink-sdk)：触摸轻扫阈值（60 px、700 ms、点按容差 28 px）。
- [EPub-InkPlate](https://github.com/turgu1/EPub-InkPlate)：原生 ESP-IDF 下的前台优先与消息式调度。

## 规范

- [W3C 中文排版需求](https://www.w3.org/TR/clreq/)、[CSS Writing Modes](https://www.w3.org/TR/css-writing-modes-3/)、[EPUB 3.3](https://www.w3.org/TR/epub-33/)：竖排与原书排版的规则来源。
- [CSS Syntax Level 3](https://www.w3.org/TR/css-syntax-3/)、[CSS Cascade Level 4](https://www.w3.org/TR/css-cascade-4/)：原书排版的 CSS 子集。
