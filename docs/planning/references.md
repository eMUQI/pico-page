# 调研线索、原始来源与核查边界

本页转交前一轮阅读过的项目与具体文件路径，供 Codex 精读，不是新的全面审计或兼容性证明。开发分支会变化；下载时重新检查实际文件和完整 commit，不能将下面的 branch URL 当成依赖锁。初始资料整理阶段未拉取或编译这些完整项目；后续实际复用、锁定版本与构建范围见 dependency-review.md，未验证外部项目的全部能力宣称。研究不是固件验收。

## 1. 首要硬件与协议

### R1 · Read Pico 官方固件

`https://github.com/MindReset/read_pico_firmware`

优先检查 README、components/read_pico、read_pico_pmu、epdiy、e0470_epaper_waveform 和芯片驱动。上一轮 README 提供过 S3、16 MB Flash、8 MB PSRAM、1216×684 16 灰阶与 ESP-IDF v6.1 基线；实际构建仍以选定 commit 为准。不要复用演示程序所有页面，也不要覆盖 PMU 的工厂校准。实际构建锁见 `firmware/board/bsp-revision.txt`。

### R2 · KOReader 的同步端与内部位置

`https://github.com/koreader/koreader/blob/master/plugins/kosync.koplugin/main.lua` `https://github.com/koreader/koreader/blob/master/plugins/kosync.koplugin/api.json` `https://github.com/koreader/crengine`

重点核对：原书部分 MD5/文件名匹配；progress 与 percentage 的不同语义；流式文档 GotoXPointer；百分比去重；服务器时间/身份与客户端状态。API 对请求的接受不证明位置对应正文；默认不依赖服务器条件写入或多版本合并。真实往返对照必须使用实际 KOReader 输出。

### R3 · KOReader 的文件式配置思路

`https://github.com/koreader/koreader/blob/master/frontend/docsettings.lua` `https://github.com/koreader/koreader/blob/master/frontend/luasettings.lua`

借鉴伴随目录、公开文件和备份/恢复；不执行 Lua 配置，不直接写 `.sdr`，不把内部格式当公共同步协议。Pico Page 自己的路径与格式以 storage.md 为准。

## 2. 阅读与同步组件研究

### R4 · CrossPoint（首要阅读/同步参考）

`https://github.com/crosspoint-reader/crosspoint-reader` `https://github.com/crosspoint-reader/crosspoint-reader/blob/develop/docs/contributing/architecture.md` `https://github.com/crosspoint-reader/crosspoint-reader/blob/develop/lib/KOReaderSync/ProgressMapper.h` `https://github.com/crosspoint-reader/crosspoint-reader/blob/develop/lib/KOReaderSync/ChapterXPathResolver.cpp` `https://github.com/crosspoint-reader/crosspoint-reader/blob/develop/lib/KOReaderSync/CrossPointPosition.h` `https://github.com/crosspoint-reader/crosspoint-reader/blob/develop/docs/contributing/touch-and-ui.md`

前一轮代码体现了章节内可见文字偏移、页起点映射与 KOReader XPath 转换的思路，不是只用百分比。解析/渲染/Arduino Print 等耦合也要一起检查。可参考章节缓存和共享触摸/按键语义，但不整体继承应用单例、所有功能或 C3 的内存权宜策略。位置转换保留降级路径，不推断任何 EPUB 都精确兼容。

### R5 · PapyriX（状态、缓存与任务所有权）

`https://github.com/bigbag/papyrix-reader` `https://github.com/bigbag/papyrix-reader/blob/main/docs/architecture.md` `https://github.com/bigbag/papyrix-reader/blob/main/docs/rendering-pipeline.md`

重点：状态拥有数据、视图只绘制；内容后端；分批缓存；协作取消；解析器/缓存独占；主机解析工具。不要直接照搬切换 UI/reader 就重启或特定缓冲复用，不同硬件要实测。它与 CrossPoint 存在分支关系，不把重复实现视为独立验证。

### R6 · CrossMux（同硬件对照，不是本项目基础仓库）

`https://github.com/0x1abin/crossmux` `https://github.com/0x1abin/crossmux/blob/main/docs/engineering/read-pico.md` `https://github.com/0x1abin/crossmux/blob/main/docs/engineering/high-dpi-ui-profile.md`

前一轮查到 Read Pico 后端、灰阶、模拟器和部分验证记录；README 与细节文档存在过更新不同步。下载时核对实际 SDK commit、发布路径和哪些是模拟器、哪些是真机证据。借鉴失败时基准帧 unknown、成功后推进 back buffer、共享绘制/命中几何等边界。不将别人的边距、VCOM、频率或样机「看着正常」当作我们设备的验收。

### R7 · EPub-InkPlate（原生 ESP-IDF 调度参考）

`https://github.com/turgu1/EPub-InkPlate` `https://github.com/turgu1/EPub-InkPlate/blob/master/src/CMakeLists.txt` `https://github.com/turgu1/EPub-InkPlate/blob/master/src/models/page_locs_control.hpp`

重点看组件组织、GET_ASAP/STOP 等消息、相关 ID、前台优先与 host 对应实现。没有把它的中文/KOReader 能力验证为满足本项目，不应因此直接定为完整阅读引擎。

### R8 · 补充而非优先

`https://github.com/atomic14/diy-esp32-epub-reader`：小型内容块/Board/Renderer 组织；上一轮 README 说明 CSS 和字形范围有限，不以它作为中文最终标准。`https://github.com/CHENXiNNNX/Ebook`：路由/合成/呈现分离；上一轮 README 将 EPUB 列为规划，不将它当已经完成 EPUB 阅读的方案。

## 3. 平台与后续功能

`https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/external-ram.html` `https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/sleep_modes.html` `https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/nvs_flash.html` `https://elm-chan.org/fsw/ff/doc/appnote.html` `https://toml.io/en/v1.0.0` `https://docs.karakeep.app/api/karakeep-api/`

查与所选 IDF 版本对应的文档，不依赖 stable 将来指向不变。FAT rename/flush 不提供任意断电下绝对安全；TF 移除/写失败需故障测试。Karakeep 仅作为后续来源，当前不实现直连或爬虫。

## 4. Codex 项目指导

OpenAI 官方：`https://developers.openai.com/codex/guides/agents-md/` 本次核查时重定向至 `https://learn.chatgpt.com/docs/agent-configuration/agents-md`。官方说明 Codex 在工作前读取 AGENTS.md，并按项目目录组合指导。因此根文件只放稳定约束和必读路径；详细规格按任务读取，不把全部历史聊天塞进指导。本包不修改全局 Codex 配置，也不声称已写入用户客户端的项目设置。

## 自定义休眠图片实现参考（2026-10-11）

参考 [KOReader screensaver.lua](https://github.com/koreader/koreader/blob/master/frontend/ui/screensaver.lua) 的有界图片列表、按文件名轮播与不可用回退思路；未复制代码。本项目限定根目录 PNG/JPEG，复用现有内容 worker 和图形解码器，清醒时预转换、休眠只读缓存，不引入 KOReader 的 Lua UI 或渲染依赖。内存门槛同时检查总余量与最大连续块，依据 [ESP-IDF ESP32-S3 堆分配说明](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/system/mem_alloc.html)，沿用已安装 IDF 6.1 的能力堆接口；不新增任务、栈或 DMA 路径。

## 透明锁屏的内存与分段处理（2026-10-19）

核对 CrossPoint 官方 `develop` 的完整提交 `ad971c8ac03c9765e3835b3b54ee56f0b0332abe`：[PNG 转换器](https://github.com/crosspoint-reader/crosspoint-reader/blob/ad971c8ac03c9765e3835b3b54ee56f0b0332abe/lib/Epub/Epub/converters/PngToFramebufferConverter.cpp) 按源行分配灰度／透明度缓冲，逐行写帧缓冲，普通图片缓存使用单行 band 顺序落盘；开启透明保持时不创建该缓存。[透明休眠路径](https://github.com/crosspoint-reader/crosspoint-reader/blob/ad971c8ac03c9765e3835b3b54ee56f0b0332abe/src/activities/boot_sleep/SleepActivity.cpp) 按灰阶平面重复解码，透明度使用 4×4 有序阈值决定像素覆盖。其低峰值以重复解码和不同透明度处理换取，不能直接替代本项目的连续透明度混合、16 级输出及热缓存免解码约定。项目根许可证为 MIT；本次只研究实现思路，未复制源码、替换解码器或新增依赖。

KOReader 的 [移植说明](https://github.com/koreader/koreader/blob/master/doc/Porting.md) 建议中间格式接近输出格式；[BlitBuffer](https://github.com/koreader/koreader-base/blob/master/ffi/blitbuffer.lua) 同时支持 4 位、8 位灰度和带透明度的 `Color8A`／`TYPE_BB8A`。带透明度的中间表示有用途，但不是整屏复制和重复拼接的理由；其 Linux 平台、缓冲管理和内存预算不能直接当作 ESP32-S3 的实测依据。本次未复制 KOReader 代码或引入其 GPL／AGPL 组件，未验证其全部图片路径。

本批先借鉴「分段写入，避免整图拼接」消除已经复现的缓存提交副本，保留当前 PNG 解码、透明精度、缓存格式及取消所有权。后续本地实现已将热缓存改为 1,368 字节行缓冲：先完整校验，再从同一打开文件逐行读取并直接合成，保留连续 alpha 与现有混合公式，第二遍失败或取消丢弃独占输出。截至该批记录，冷 PNG 解码直接合成及仅存有效图层边界仍为候选；涉及缩放、Adam7 渐进覆盖、透明边缘、缓存校验与取消边界，需要另行验证，不以外部固件的代码或示例替代真机验收。

## 透明锁屏冷解码补充（2026-10-20）

本轮检查 CrossPoint `ad24aea85c1f9b1a4b2b2e0719ec0dd39020a6b4`，相关 PNG 与休眠文件同上述已核对版本一致；行缓冲值得采用，但重复解码、阈值透明度和拒绝交错 PNG 不符合本项目既有行为。KOReader 的 [screensaver](https://github.com/koreader/koreader/blob/master/frontend/ui/screensaver.lua)、[ImageWidget](https://github.com/koreader/koreader/blob/master/frontend/ui/widget/imagewidget.lua) 与 [RenderImage](https://github.com/koreader/koreader/blob/master/frontend/ui/renderimage.lua) 支持透明图片及连续 Alpha，但常规路径仍读取完整编码文件和图像缓冲，不能据此推断 ESP32-S3 上的峰值更低。

用户指定的 [read-pico-reader](https://github.com/wegooo-cell/read-pico-reader) 核对至 `2087afda444b17fb320562fc31ab9424b661e946`。[book_cover.c](https://github.com/wegooo-cell/read-pico-reader/blob/2087afda444b17fb320562fc31ab9424b661e946/main/book/book_cover.c) 使用 libpng 从文件读行、双行插值，拒绝交错 PNG；[book_lock_bookmark.c](https://github.com/wegooo-cell/read-pico-reader/blob/2087afda444b17fb320562fc31ab9424b661e946/main/book/book_lock_bookmark.c) 仍分配适配尺寸的灰度和 Alpha 整层，并直接叠加当前帧，另含自动去白及阴影。它减少输入缓冲但没有消除整层峰值；普通壁纸缓存与透明书封路径不同，不能把普通壁纸命中当作透明锁屏免解码。根许可为分文件混合规则，前者标注 Apache-2.0，后者标注 `LicenseRef-Kiikoread-Personal-Study-1.0`；本项目未复制其实现。

本次采用文件分块读取的通用思路，继续使用已有 pngle，独立实现最终采样直接合成与行缓冲临时缓存。保留现有精度、Adam7、取消恢复、画布所有权和单张缓存，不引入 libpng、新任务或外部代码。取舍是以有界 TF 随机读写承接交错行重访，须测量慢卡冷路径；内存预算不再随编码文件大小或整屏透明层增加。热缓存仍免解码，缓存失败仍能显示本次输出。来源代码观察、host 像素验证和真机耗时分别记录，不据其他固件的实现宣称其稳定性更高。

同机压力测试再次复现内容 worker 在 EPUB 解压／XML 解析期间触发 IDLE0。核对乐鑫文档 MCP 和本机 IDF v6.1 的 [Task Priorities](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/performance/speed.html#task-priorities)：持续计算任务需要阻塞让步，单纯 `taskYIELD()` 不保证低优先级空闲任务得到运行。修正现有解析块及分页页边界，不增加任务、不关闭或延长看门狗，也不主动代替空闲任务喂狗。


## TXT 目录识别（2026-10-31）

核对已有独立研究检出 KOReader CREngine `f4a8be93255a439db4777d00f0bc53316acabebd` 的 `crengine/src/lvxml.cpp`：`DetectHeadingLevelByText` 使用卷、部、章的前缀与数字模式，TXT 导入另有短行长度判定。该引擎依赖完整 DOM／文本导入，不能直接作为 ESP32-S3 原生分段阅读的目录实现；源码按 GPL 分文件许可，本批只参考模式识别思路，没有复制实现或增加依赖。Pico Page 独立实现中文与英文短标题识别，在既有编码／身份扫描中收集分段标量锚点，避免额外整书读取。调度依据乐鑫文档 MCP 的 [任务看门狗说明](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/system/wdts.html#task-watchdog-timer-twdt)及本机 IDF v6.1 FreeRTOS 接口：计算期间阻塞让出，保留 Idle 运行，不主动喂狗或改变看门狗配置。
