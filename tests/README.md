# 验证入口

按 `docs/environment.md` 准备固定字体/依赖，构建根目录 CMake 后运行 CTest。
当前 13 个入口：power_key、core、store、store_sync、reader_flow、epub_probe、setup_network、
graphics、document、app、provision、wifi_upload_page、sync。

主要覆盖：呈现/保存事务、旧代次拒绝、持久化失败重试、配置外部编辑、真实 EPUB 源位置、
分页无半行、K2 全刷不改进度、应用层缺字库/读档/显示失败、排版取消/应用、网络取消与旧回包、
账户变化后禁止采用旧候选，以及协议字段与受限 XPointer 映射。
`app_test` 还覆盖内置说明的离线/无卡/无字体显示、翻屏边界、K2 全刷、再次打开和阅读记录不变，
并分别运行同步及固件使用的异步应用模式。
阅读页脚回归覆盖左右独立循环、热区边界/交界拖动、在途与过期触摸、正文/阅读记录/进度文件不变、
统计前后真实绘制、百分比两位小数与舍入、无效时间和时区跨日；两种应用模式均执行。

测试 EPUB 均由脚本生成；不提交个人书籍。`app_test` 使用 setup_fonts 下载的完整文楷字体。
`tools/check_koreader_mapping.py` 是独立的真实 CREngine 兼容性对照，不是模拟期望值。
墨水屏、TF 掉电、网络真实账户与双向接续仍需按 `docs/acceptance.md` 验收。
