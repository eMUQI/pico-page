# 许可说明

Pico Page 按 [GNU 通用公共许可证第 3 版或任何更新版本](../LICENSE)（GPL-3.0-or-later）发布，并附加下文的 ESP-IDF 二进制库链接许可。任何人都可以使用、修改和分发本项目，包括商业用途；分发固件或其修改版本时，必须按 GPL 向接收者提供完整的对应源码及安装所需信息，并保留本许可。

本说明不是法律意见。

## 适用范围

GPL-3.0-or-later 适用于本仓库中由 Pico Page 项目编写的源码、构建与诊断脚本、测试、网站代码和文档。以下内容保留各自的许可，不因本说明改为 GPL：

- 文件头带有其他 `SPDX-License-Identifier` 或版权声明的文件，以文件头为准。例如 `firmware/components/platform_read_pico/` 中改编自 Read Pico BSP 的文件为 Apache-2.0。
- 各组件 `third_party/` 目录中的代码与数据，许可证随目录保存，来源见[依赖审查](development/dependency-review.md)。
- 构建时下载的 Read Pico BSP、epdiy（LGPL-3.0-or-later）、FreeType、ESP-IDF 及其组件，见[依赖审查](development/dependency-review.md)与 [BSP 审查](development/bsp-review.md)。
- 字体使用 SIL Open Font License 1.1，许可证与字体文件放在一起。
- `web/public/downloads/` 中的锁屏图片和古印素材，权利以各目录的 `README.txt`、`RIGHTS.txt` 和 `SOURCES.txt` 为准。

## ESP-IDF 二进制库链接许可

ESP-IDF 的 Wi-Fi、PHY、蓝牙及共存等功能以闭源二进制库提供。为使 Pico Page 及其修改版本可以合法地与这些库一同分发，版权持有人依据 GPL 第 7 条授予以下附加许可：

```text
Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining it
with the binary-only libraries distributed by Espressif Systems as part of
ESP-IDF (such as the Wi-Fi, PHY, Bluetooth and coexistence libraries), or a
modified version of those libraries, the licensors of this Program grant you
additional permission to convey the resulting work. Corresponding Source for
a non-source form of such a combination shall not include the source code
for the parts of those libraries.
```

修改本项目时可以保留此附加许可，也可以按 GPL 第 7 条将其删除。

## 名称

GPL 授予的是代码的使用权，不包括「Pico Page」名称及标识的使用权（GPL 第 7 条 e 项）。

- 转发未修改的官方版本时，可以如实说明其为 Pico Page。
- 分发修改版本时，请使用其他名称，或明确标注为非官方修改版，不要让用户误以为是官方发布。
- 以「Pico Page」名义销售固件或预装设备，需事先取得项目所有者的书面许可。

「Read Pico」是其硬件厂商的名称，本项目不对其主张任何权利。

## 贡献

提交到本仓库的贡献按 GPL-3.0-or-later 及上述附加许可授权。
