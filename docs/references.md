# 调研线索、原始来源与核查边界

本页转交前一轮阅读过的项目与具体文件路径，供 Codex 精读，不是新的全面审计或兼容性证明。开发分支会变化；下载时重新检查实际文件和完整 commit，不能将下面的 branch URL 当成依赖锁。本包没有拉取/编译这些项目，也没有验证其当前全部宣称。研究不是固件验收。

## 1. 首要硬件与协议

### R1 · Read Pico 官方固件

`https://github.com/MindReset/read_pico_firmware`

优先检查 README、components/read_pico、read_pico_pmu、epdiy、e0470_epaper_waveform 和芯片驱动。上一轮 README 提供过 S3、16 MB Flash、8 MB PSRAM、1216×684 16 灰阶与 ESP-IDF v6.1 基线；实际构建仍以选定 commit 为准。不要复用演示程序所有页面，也不要覆盖 PMU 的工厂校准。证据归档：archive/architecture-v0.1.md 中 S1；它不是实际构建锁。

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
