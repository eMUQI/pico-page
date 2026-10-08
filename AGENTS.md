# Pico Page — repository instructions

这是 Read Pico 硬件上的个人阅读固件。项目名是 **Pico Page**，不是 Pico Pages；
仓库/目录建议 `pico-page`，构建标识 `pico_page`，板级名称仍为 `read_pico`。
项目已进入可运行固件的迭代阶段；文档、设计 HTML、mock 和路线图不是实现或验收证据。

## 必读与需求优先级

先读当前任务、`docs/decisions.md` 与 `docs/development-status.md` 的当前摘要及相关最新记录。
首次开发再读 `docs/architecture.md`、`docs/storage.md`、`docs/ui-implementation.md`、
`docs/milestones.md`。涉及固件或共享核心的修改，必须读取
[嵌入式开发规则](docs/embedded-development.md) 中对应章节及 `docs/acceptance.md` 对应测试项。
构建/依赖/板级任务另读 `docs/environment.md`；存储/USB 任务另读
`docs/store-implementation.md`、`docs/usb-storage-runtime.md`；电源/OTA/按键扩展另读 `docs/roadmap.md`。
不要每次重读全部历史，也不要把历史环境、构建通过或已烧录记录当成本次结果。
按源码中的实际组件和调用链定位；架构中的职责名称不要求逐一新增同名组件或任务。

项目需求冲突时：用户在当前任务中的明确更新 > `docs/decisions.md` >
当前 architecture/storage/ui-implementation > `docs/design/original/` > `docs/archive/`。
这是项目资料间的优先级，不覆盖工具权限和更高层规则。
历史文件保留原样供追溯，不能从中恢复已经废弃的决定。

## 不要重新讨论的产品决定

- 主要自用、计划开源，独立设计；第一版只做 Read Pico，框架优先 ESP-IDF。
- 第一版验收：EPUB 离线阅读 + KOReader 双向位置接续。HTTP 请求成功不等于接续成功。
  保留已加入的 UTF-8/GB18030 TXT 本地阅读；不能把 EPUB 的同步验收范围扩称为 TXT 已支持。
- 有效长章节用固定大小分段/流式处理，不能以整章内存超限拒绝正文。总页数允许暂时未知。
- 当前设计稿直接作为视觉基线，不重设计，不等待设计师重出稿才写代码。
- **所有可交互页面 K2 = ForceRefresh（立即全刷当前画面）**；正文中部触摸打开工具。
  这是默认映射；自定义按键（路线图 R7、D30）实现后用户可改 K1/K2/K3。默认映射下 K2 不关闭面板、不取消预览、不确认冲突、不退出 USB 传输；关闭/取消使用屏幕控件。
  刷新进行中忽略重复 K2；D14 的连续翻页保留一次后续翻页，不扩展成无限排队。
  硬件休眠的唤醒来源仍由板级能力决定。
- 自定义按键按路线图 R7：只提供上一页、下一页、全刷、回到主页，经集中的上下文动作映射执行；
  “回到主页”不得代替确认、取消冲突或退出 USB 传输。长按书库条目进入详情，长按正文图片进入图片查看；不新增其他长按快捷动作。
- 用户配置、单书设置和进度以 TF 卡普通文件为主。索引与缓存不是数据权威；NVS 不保存隐藏的权威阅读配置/进度。
- 接受电脑预处理，但不预先冻结专有阅读包；从已有原生阅读路径继续，不另起一套平行后端。
- PDF、Karakeep、批注/词典、插件、统计、多个主题和完整 OTA 产品流程后置；OTA 升级计划见路线图 R11。

## 实现边界

UI 只发动作与画状态；`session` 独占阅读位置；`display` 独占面板刷新；
`store` 统一管理文件提交；同步回包只是候选，不直接改页。
文档、排版、位置和同步策略中的纯逻辑应能在 host 测试。
异步请求带书籍/会话/排版代次；旧结果不得覆盖新会话。
只保存已成功呈现的正文位置；只同步已确认保存的版本。
按 D15 在停留后保存，离开正文、休眠、USB、同步前提交；不要恢复为每次翻页立即写卡。
当前页优先排版；不能用整书阻塞重排、超长 Label 或 CSS 裁切代替分页。
K2 手动全刷必须绕过“像素没变就不刷”的优化；不改变锚点或进度修订号。

## 嵌入式工程硬约束

适用于固件和 host 共享核心；详细检查按任务读取 `docs/embedded-development.md`。

- **预算与交互**：涉及任务、缓存、解析、图片、网络或显示，说明执行上下文、峰值内存、最长不可取消区间和失败回退；小改动写在交接中即可。
  当前正文与必要保存优先，可选预取/统计使用剩余资源。输入分发和绘制回调不新增整书扫描、解压、图片解码或等待后台退出的长阻塞。
  分批计算保留可安全复用的解析器、字体及游标，不靠反复重建实现让出。
- **所有权与并发**：新任务说明优先级、亲和性、栈字节数、队列容量、溢出策略和退出协议。
  非线程安全对象只交给一个执行者；`shared_ptr`、`volatile` 不代表线程安全。ISR 仅做必要采样/通知，使用适用的 FromISR API；临界区不做 I/O、分配或等待。
- **取消与完成**：取消请求不等于停止访问资源。完成后才移交结果、销毁借用对象；校验书籍/会话/排版代次，必要时加挂载代次/账户作用域。
  不强删持锁/文件的任务；取消检查目标不能冒充阻塞驱动调用或整条链路的时限保证。
- **内存与降级**：分别核算内部 RAM、PSRAM 和 DMA/IRAM 要求，检查连续块及最低剩余栈。
  峰值包含旧/新缓冲、容器扩容、解码、TLS、任务栈及库内分配；检查实际分配结果。可选缓存不足只降低命中率，不破坏正文、保存和错误入口。
  不一概禁止 STL/堆分配，也不靠加栈/扩大缓存掩盖根因。
- **调度与看门狗**：`taskYIELD()` 不保证低优先级或 Idle 任务运行。不要用关闭/无条件喂看门狗、提高优先级或换核心掩盖未定位的阻塞。
  合法长操作确需调整预算时，提供依据和回归证据。
- **显示与输入**：排版完成、像素准备、面板完成、进度保存是不同状态。在途画布不可改写/释放；刷新失败后使基准失效并走恢复路径。
  保留 D14 的一次后续翻页、K2 忙时忽略及完整输入序列；队列溢出不能使按下状态永久残留。先测量，后改波形、时序、频率与供电窗口。
- **缓存与位置**：区分内容、排版、位图缓存键；身份、算法、字体、几何变化正确失效。更改归一化/分页算法检查持久缓存和源锚点兼容性。
  验证冷/热命中及内存不足降级；不能仅凭写缓存代码存在就声称优化有效。页码不是永久位置。
- **存储与电源**：USB 接管、卸载/换卡、休眠前停止相关生产者，等待资源交还并处理保存结果。
  保留 tmp/备份/恢复协议；host 文件操作成功不证明 TF 掉电可靠。PMU/扩展器需一起核对中断确认顺序、电平、待处理队列及唤醒原因。
- **单位与边界**：耗时用单调时钟；明确 ms、µs、tick、字节和 Unicode 偏移单位。外部长度、图像尺寸先校验再算术/分配；host 64 位结果不替代目标 32 位边界验证。

硬件常量放源码/配置，数值目标放 `docs/acceptance.md`，实际测量放开发记录，不在多份 AGENTS 中复制可变数值。

## 现有设计的处理

按 `docs/ui-implementation.md` 修正裁字、误导状态、输入穿透和数据丢失风险。
允许沿用现有组件做最小文案/状态/热区修正；不要借评审重做布局、品牌或导航。
原始 PNG/HTML 不修改；新增实现截图放 `artifacts/`，记录差异与原因。
设计图里的书名、时间、百分比、服务地址不是运行时真值。

## 工作方式与安全边界

先检查已有文件与 Git 差异，保护用户未提交的改动。先查现有 ESP-IDF/EIM 工具链，
不要因 PATH 中没有 `idf.py` 就断言未安装；不要自行替换全局工具链。
核对实际 IDF 版本、BSP/依赖锁、已有补丁和最终生效的 sdkconfig；历史机器路径不是当前机器事实。
修改 sdkconfig.defaults 后验证构建实际采用了该值；不能靠关闭组件/测试或替换依赖来冒充标准构建通过。
复用前读第三方源码/许可证，记录 commit、依赖、修改与义务；顶层许可证不代表子组件。
未经用户明确授权，不发布仓库、不推送远端、不刷写设备、不改分区、不擦除 Flash/TF。
不得改写工厂 VCOM/PMU 校准，也不能直接复制其他固件的烧录偏移。
凭据、个人 EPUB、完整 Flash 备份和私有日志不提交；不默认关闭 TLS 验证。
不能编译/没连设备时继续完成可运行的 host 工作，并明确未验证项。

## 验证与交接

在仓库根目录使用已有入口；首次 host 构建缺少测试字体时按 `docs/environment.md` 准备，
不要用缺资源导致的测试跳过替代验收。

```sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel 4
ctest --test-dir build/host -N
ctest --test-dir build/host --output-on-failure
sh tools/build_firmware.sh
python3 tools/check_handoff.py
git diff --check
```

按改动风险选择检查，不机械地为每次文案修改烧录设备：

| 改动 | 最低验证范围 |
|---|---|
| 仅文档/注释 | 相关事实、链接与规则一致性；交接检查、diff 检查 |
| 共享 C/C++、解析、排版、存储、会话 | 回归测试、host Debug/CTest；涉及内存/生命周期用 ASan/UBSan；固件构建 |
| ESP_PLATFORM 分支、main、板级、sdkconfig、依赖/构建 | 固件构建必须覆盖改动路径；加可隔离逻辑测试；列出对应真机验证 |
| 响应性、缓存、并发、显示、电源 | 上述适用项，加同条件性能/资源对比及 `docs/acceptance.md` 对应专项 |

检查 CTest 实际注册的项目及资源/Node 等可选依赖；“测试数变少但全绿”不能直接视为完整通过。
异步路径必须用固件对应模式测试，不能只跑默认同步模式。修改失败处理时添加失败/取消回归，
不要只测正常路径，不降低断言或删测试来凑通过。
性能结论用 `tools/summarize_reader_perf.py` 及原始日志支持；记录 commit、样本、缓存冷/热、
配置、样本数、分位数/最大值和内存。阈值过滤日志不代表完整分布，host 耗时不冒充真机。

交接检查仅检查资料；host、sanitizer、固件构建、烧录摘要、真机行为和实际 KOReader 往返
必须分开报告。不能编译/没连设备时继续完成可验证工作，将产物标为待验证候选，写明缺口；
不得用 mock 成功或旧日志顶替本次结果，也不据此声称功能已验收。
每批在 `docs/development-status.md` 更新当前摘要并追加：变更、命令/环境、结果、未测项、下一步。
新的稳定规则改其权威文档；历史记录留存，不把过程日志复制到 AGENTS。保持根文件小于 16,000 UTF-8 字节。

## Code Comment Policy

Comments explain the current implementation to maintainers without conversation context. Prefer clear names, types, constants and control flow; retain useful comments without unnecessary refactoring.

- Explain non-obvious intent, invariants, hardware or third-party constraints, timing, units, side effects, ownership, lifetime and concurrency requirements. Do not restate the code.
- Use concise, objective technical language and established terminology. State the underlying constraint rather than a conversational warning: `// The controller requires at least 120 µs after reset before the first SPI transaction.`
- Keep implementation history, rejected approaches, user instructions and discussion in Git, issues or ADRs. Comments are not changelogs or scratchpads.
- Delete obsolete code, including commented-out code. A temporarily disabled operation requires a TODO naming the concrete blocking condition or remaining action.
- Document public API behavior, parameters, return values, errors, valid ranges, ownership and usage requirements where callers need them. Use Doxygen tags for C/C++ APIs when appropriate.
- When changing code, review nearby comments and remove or update obsolete, redundant, conversational or historical material. Every added or modified comment must be accurate, necessary and understandable independently of the task.

## 面向用户的说明

面向用户的中文说明、操作文档和界面文案参考 [Tech-Doc-Style-Chinese](https://github.com/Fenng/Tech-Doc-Style-Chinese)。其中借鉴 ASD-STE100 的方法按中文语境采用，不直接套用英文词典、语法或句长规则，也不据此宣称符合该标准。

- 表达准确、简洁、克制；保持术语一致，避免空泛宣传、行业黑话和机械直译。
- 操作说明写清执行者、对象及预期结果，区分用户操作与系统行为；按执行顺序组织步骤，每步围绕一个主要动作，必要条件与风险放在对应动作之前。
- 改写必须保留事实、限制、例外和确定程度，不补造能力、数值或结果，不为缩短句子破坏条件与因果关系。
- 中文与英文、数字混排时保持适当留白；代码、URL、API 路径及其他机器可读标识符保持原样。
- 根据内容选择规则；简短按钮、导航标签和概念说明不机械套用操作步骤格式。项目既有术语和明确约定优先。

## Code Review Rules

重点阻止：默认映射被改动、按键动作绕过确认或 USB 传输退出、私自重设计、NVS 覆盖手改配置、删缓存丢进度、
上传旧版本却标新版本已上传、未显示页记已读、旧异步回包污染、失败或取消后静默覆盖远端、
保存失败仍宣称已保存、把编译/模拟器通过写成真机通过；
另重点检查主线程长阻塞、无界资源增长、取消后仍访问已释放对象、刷新缓冲竞态、
TF 与 USB 双重所有权、休眠事件误判，以及只有平均耗时/host 结果的“性能优化”。
