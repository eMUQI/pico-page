# 2026-11-04 · CSS 原型调研与测量边界

候选，待维护者确认。基线为 `main 23fc5602f17ce163f3282985f2882b5dc41ad1af`，分支 `codex/css-cascade-prototype`。以下只记录独立解析模块、私有样本分析和设备读取的实际证据；本批未取得目标解析性能或阅读剩余内存数据。支持契约见 [CSS 原型](../css-style-prototype.md)。

## 实书统计

只读分析本机已有私有书库的 22 本 EPUB，未修改原文件。书籍统一以私有目录编号映射到「实书 A」至「实书 V」，公开记录不列书名、文件名、作者、样本摘要、选择器或内容。原始 CSS、匿名 JSON、数据头、Flash 读取和日志均在忽略目录 `.local/css-survey/`。

| 项目 | 本批统计 |
|---|---:|
| 外部 CSS 文件 | 39 |
| HTML／XHTML 文件 | 2,382 |
| 严格 XML 解析失败、改用 HTML 调研回退 | 19 |
| 行内 style 属性 | 465 |
| 单文档关联样式最大字节数，不含行内声明 | 140,285 字节 |
| 单文档最大规则数 | 379 |
| 单文档最大元素数 | 1,968 |
| 最大「元素数 × 规则数」 | 259,692 |
| CSS 最大嵌套／元素最大深度 | 2／8 |
| 外部 CSS 的 important 声明 | 6 |

调研解析器只用于统计关联、token 形状与规模。HTML 回退不等于固件 XML 接受这些文件；规则统计不包含 at-rule 本身。关联导入有路径校验、32 层及数量／字节上限，拒绝外部加载；它与原型整句跳过 import 的策略分别报告。CSS token 形状只输出类别，不输出字体或其他私有值。

| 常见属性 | 外部 CSS 中的声明次数 |
|---|---:|
| text-align／font-size／text-indent | 691／626／570 |
| margin-top／margin-bottom／margin-left／margin-right | 490／486／464／425 |
| font-family／line-height | 427／426 |
| margin／padding | 241／98 |
| display／font-weight／font-style | 213／131／38 |

单位为 em 3,644 次、px 334 次、百分比 297 次、pt 70 次、vh 9 次、cm 8 次、vw 6 次、mm 4 次、rem 4 次、in 2 次。选择器标记统计包含类型 2,073、类 1,167、复合 645、后代 54、分组 50、ID 42、伪类／伪元素 15、子元素 8、相邻兄弟 8、属性 2、通配符 1；它们是可重叠的标记次数，不是互斥的规则数量。at-rule 为 font-face 30、page 11、charset 4、media 2、import 0。margin／padding 观察到 1–4 项、无单位 0、长度、百分比及 auto 的组合；完整类别频次保留在私有 JSON。

## 新模块解析结果

| 输入集 | 调用数 | Complete／Limit／Cancelled／NoMemory | 拒绝选择器规则／拒绝声明／跳过 at-rule |
|---|---:|---|---|
| 外部 CSS | 39 | 39／0／0／0 | 22／1,580／47 |
| 提取的 style 元素与合成行内声明包装 | 136 | 136／0／0／0 | 0／322／9 |
| 每个文档实际关联的样式集合 | 2,382 | 2,382／0／0／0 | 2,274／159,099／4,493 |

关联集重复计算共享 CSS，三行不能相加当作不同源文件数量。最大成功保留 379 条规则、379 个选择器和 2,837 个展开声明。拒绝包括支持表之外的属性／值和非法写法；Complete 只表示完成有界解析，不表示所有声明有效或完整还原。所有超限与取消路径由原创 fixture 验证，实书中未触发上限。

## 设备操作与异常

目标通过本次 esptool 识别为 ESP32-S3 v0.2、8 MiB PSRAM、16 MiB Flash、USB Serial/JTAG。实读分区为 ota_0 `0x10000`／4 MiB、ota_1 `0x410000`／4 MiB、otadata `0x810000`／8 KiB；两个条目的 sequence 为 1、2，状态均为 VALID，CRC 均通过，选择 ota_1。只读到的应用头版本为 `0.5.1+ga2871cc`、项目 `pico_page`；这不是最终启动确认。

首次 4 MiB 应用备份在读取到 2,879,488 字节附近中断，原始错误为 `A fatal error occurred: No more data to read from the serial port. This can have many causes, for troubleshooting steps visit: https://docs.espressif.com/projects/esptool/en/latest/troubleshooting.html`。完整输出文件没有产生。第 1 次受限只读 chip-id 检查成功，仍为同一设备、stub 已运行，无复位。

之后以 256 KiB 分块读取，前 10 块成功；第 11 块在 258,048／262,144 字节处同样出现上述错误。第 2 次、也是最后一次受限只读 chip-id 检查成功。单独读取剩余尾部在 249,856／1,310,720 字节处同样失败。缩小本次 esptool 实例的读帧至 1 KiB 后，仍报 `esptool.util.FatalError: Serial data stream stopped: Possible serial noise or corruption.`。没有把成功连接当作备份成功，没有继续进行固件写入。

两种减小传输规模的方法均未通过完整备份检查，其共同前提是 stub 串口传输能够完成可靠读取。当前证据只能确认主机与设备仍能握手，不能确定丢包发生在主机、USB 链路或 stub。停止继续调整传输或复位试验，保留全部原始异常。

收尾时 `run` 配合 `no-reset` 仍留在 stub；随后执行一次明确的 RTS 正常复位，并在同一串口采集 25 秒，日志为空。因而只确认复位命令已执行，未确认最终启动、TF、VCOM、固件 SHA-256 或休眠状态。没有完整恢复镜像和独立 verify-flash 结果；设备没有被写入新固件，不能声称「恢复镜像已校验」或「设备验收通过」。

本批未写 Flash、分区表、otadata、NVS、bootloader 或校准，未打开任何实际书籍，未修改 TF 文件、位置、排版设置、账户或 Wi-Fi，未进入 MSC，也未发送睡眠或电源菜单动作。Release 不提供诊断 K2 保活，本批设备异常后停止操作，无法确认后续是否因原有空闲设置自动休眠。没有要恢复的测试书籍位置；原应用后续行为及最终设备状态需要设备旁核对。

## 目标测量缺口

合成输入和实书私有数据的独立测量程序均只构建，不刷写；Diagnostic 产品候选只构建，不安装。目标解析／匹配／层叠耗时 P50、P95、最大值，内部 RAM／PSRAM 峰值及最大连续块、最长不可取消区间、任务栈余量、阅读时实际剩余内存全部未测。没有用 host 计时或构建结果填充这些字段，也没有将候选上限写入 acceptance。

后续应先由设备旁确认原固件启动和串口状态，再在授权范围内取得有效完整备份，按[独立测量程序](../../../tools/target_bench/css/README.md)每组运行至少 5 次，并测量阅读时内存对照。不得用当前不完整的分块数据刷写或擦除设备。

## host 与构建验证

本批按根 AGENTS 执行 Debug、ASan／UBSan、标准固件、Diagnostic 和独立程序构建，最终结果记入[开发状态](../../development-status.md#2026-11-04--css-解析与层叠有界原型候选)。首次行为测试在空实现的字号断言失败；CRLF 转义及 HTML 关联回退均先加入失败用例再修复。原创 fixture 覆盖选择器回溯、层叠、important、行内优先级、继承、单位／百分比、简写拒绝、错误恢复、at-rule 隔离、各容量、32 位长度边界、分配失败、取消、Unicode、空输入和截断。

首轮 30 秒 ASan／UBSan 变异输入运行 207,958 轮，无错误；最终源码再次运行 30 秒、220,381 轮，无错误。独立的三项定向变异分别移除 important 优先级、破坏 pt 换算、把 margin 百分比基准改为字号，三项均被行为断言检出。crapper、dryer、mutator 未提供可用工具或命令，未安装替代依赖，不声称获得这些工具的分数或完整变异覆盖。macOS LSan 支持与 Linux CI 泄漏检查分别记录。
