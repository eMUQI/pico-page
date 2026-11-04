# Pico Page — repository instructions

**Pico Page** 是 Read Pico 硬件上的个人阅读固件；仓库目录为 `pico-page`，构建标识为 `pico_page`，板级名称为 `read_pico`。

## 核心开发原则

以目标设备上的性能、响应速度和阅读体验为首要优化目标，以数据正确、可靠保存和硬件安全为前提。从实际阅读需求和设备约束出发，优先采用能完整解决问题的简单方案；当前阅读、输入反馈和必要保存优先于可选后台工作。性能取舍依据完整用户路径的实测结果，不能仅凭代码量、分配次数或平均耗时判断。权威说明见 [固件设计原则](docs/planning/decisions.md#固件设计原则)（D44）。

## 按任务读取与资料优先级

按任务读取下表中的相关章节；错字、链接或格式修正只核对直接相关内容。

| 任务涉及 | 读取资料 |
|---|---|
| 产品行为、范围或需求冲突 | `docs/planning/decisions.md` 对应决定 |
| 当前实现、验收状态或继续已有工作 | `docs/development-status.md` 的「当前摘要」；仅为追溯具体问题读取相关历史记录 |
| 组件职责、调用与所有权边界 | `docs/development/architecture.md` |
| 固件或共享核心修改 | `docs/development/embedded-development.md` 对应章节及 `docs/development/acceptance.md` 对应测试项 |
| 存储、USB | `docs/development/storage.md`、`docs/development/store-implementation.md`、`docs/development/usb-storage-runtime.md` 对应部分 |
| UI、文案或交互 | `docs/development/ui-implementation.md` 对应部分；视觉规范查 `docs/design/system/` |
| 构建、依赖或板级 | `docs/development/environment.md` |
| 电源与 OTA 实现 | `docs/development/platform-runtime.md` 对应章节；页面行为查 UI 说明 |
| 按键扩展或阶段规划 | `docs/planning/roadmap.md` 对应功能；阶段规划另查 `docs/planning/milestones.md` |

项目需求冲突时：维护者在当前任务中的明确更新 > `docs/planning/decisions.md` > 当前 architecture/storage/ui-implementation/设计系统 > `docs/design/original/`。这是项目资料间的优先级，不覆盖工具权限和更高层规则。历史资料仅用于追溯，已废弃的决定不作为现行要求。以源码实际组件和调用链定位实现；架构中的职责名称不要求对应独立组件或任务。

## 技术资料与候选参考

- 涉及 ESP-IDF、芯片、驱动、内存、调度或电源行为的设计与排障，主动优先使用乐鑫文档 MCP（`search_espressif_sources`）查证，核对目标芯片和实际 IDF 版本；MCP 不可用或资料不足时，查对应版本的官方文档及本地 SDK/BSP 源码。
- 设计或调整阅读、分页、缓存、存储、同步及任务机制时，主动考察 KOReader、CrossPoint 等成熟生态的相关设计作为候选；已有线索见 [调研参考](docs/planning/references.md)。按本次问题选择相关部分，比较资源预算、平台耦合、复杂度和许可后决定是否采用；保留本项目已确认的产品与视觉边界。局部修正已有方案时，无需重复全面调研。

## 产品规则的使用

功能范围和后续候选以[产品决定](docs/planning/decisions.md)及[路线图](docs/planning/roadmap.md)为准，实际实现、发布和验收状态查开发状态。本文件不维护完整功能列表或各功能的详细交互；按任务表读取对应资料。

- 默认映射下，**所有可交互页面 K2 = ForceRefresh（立即全刷当前画面）**；不代替关闭、确认、取消或退出 USB。休眠准备按 D43 忽略触摸和 K1/K2/K3，仅短按电源键取消，长按仍请求电源菜单；不重放忽略的输入。完整输入规则见 UI 说明。
- 用户配置、单书设置和进度以 TF 卡普通文件为主；索引与缓存不是数据权威。NVS 不保存隐藏的权威阅读配置/进度。
- 不以总页数未知或章节过长拒绝有效正文；沿用原生阅读路径，固定大小分段或流式处理，当前位置优先。首版 EPUB 阅读与实际 KOReader 双向位置接续的验收边界不因扩展功能改变。

## 实现边界

- UI 只发动作与画状态；`session` 独占阅读位置；`display` 独占面板刷新；`store` 统一管理文件提交。文档、排版、位置和同步策略中的纯逻辑应能在 host 测试。
- 异步请求带书籍/会话/排版代次，必要时校验挂载代次和账户作用域；旧结果不得覆盖新会话。同步回包只是候选，不直接改页；失败或取消后不得静默覆盖远端。
- 只保存已成功呈现的正文位置，只上传已确认保存的版本，上传确认只对应实际发送的快照；保存失败不得宣称已保存。按 D15 在页面停留后保存，离开正文、休眠、USB、同步前提交。清缓存不得丢失配置或进度，持久位置使用源锚点。
- 当前页优先排版，其余按需或后台计算；正文必须真实分页，不等待整书重排。

## 嵌入式工程硬约束

固件和 host 共享核心遵守以下底线；详细检查见[嵌入式开发规则](docs/development/embedded-development.md) 对应章节。

- **资源与响应**：涉及任务、缓存、解析、图片、网络或显示，说明执行上下文、峰值内存、最长不可取消区间和失败回退；小改动写在交接中即可。当前正文与必要保存优先；输入分发和绘制回调不新增扫描、解压、解码或等待后台退出的长阻塞。资源增长必须有界，资源不足时降低或停用可选工作，不破坏正文、保存和错误入口。
- **所有权与取消**：非线程安全对象由单一执行者访问。取消请求不等于完成；确认停止访问借用资源后，才能移交或释放资源，不强删持锁/文件的任务。取消检查间隔不能冒充阻塞调用或整条路径的时限保证。
- **显示与输入**：排版完成、像素准备、面板完成和进度保存是不同状态。在途画布不可改写/释放；刷新失败后使基准失效并恢复。保留完整输入序列，避免输入穿透或队列溢出后按下状态永久残留。先测量，后改波形、时序、频率与供电窗口。
- **存储与电源**：USB 与本机文件系统不得同时拥有 TF 卡。接管、卸载/换卡、休眠前停止相关生产者，等待资源交还并处理保存结果；保留 tmp/备份/恢复协议。电源事件必须核对中断确认、待处理事件和唤醒原因，不能把周期事件当作用户唤醒。
- **边界与证据**：外部长度、尺寸先校验再算术/分配，host 64 位结果不替代目标 32 位边界验证。耗时用单调时钟，明确时间、字节和 Unicode 偏移单位。不得靠扩大资源或关闭/无条件喂看门狗掩盖未定位的阻塞；涉及缓存时验证冷/热路径及资源不足降级。

硬件常量放源码/配置，数值目标放 `docs/development/acceptance.md`，实际测量放开发记录。

## 现有设计的处理

沿用现有设计稿与 `docs/development/ui-implementation.md` 的视觉和交互基线；修正裁字、误导状态、输入穿透和数据丢失风险时，允许最小文案、状态及热区调整，不重做布局、品牌或导航。原始 PNG/HTML 保持不变；临时截图放被忽略的 `artifacts/`，文档引用的合成画面放 `docs/images/`，记录差异与原因。书名、时间、百分比和服务地址使用运行时数据。

## 工作方式与安全边界

Codex 非平凡开发任务使用已安装的 `pstack:poteto-mode`，验证使用 `verify-pico-page`。按[接入说明](docs/development/agent-workflow.md)读取安装包内的 Codex 适配，保留本仓库的产品、验证与授权边界。

先检查已有文件与 Git 差异，保护未提交的改动。实现或修复限于约定范围，包含配套文档、本地构建、测试及本次改动引入的失败修正。审计或讨论请求只交付结论；要求先审阅或暂不提交时，遵守该边界。

构建前通过 ESP-IDF/EIM 安装记录及 `docs/development/environment.md` 定位已有工具链，不自行替换全局工具链。核对实际 IDF/BSP、依赖锁、补丁和生效 sdkconfig；改 defaults 后验证实际采用值。复用第三方代码前逐组件核对源码与许可证，记录 commit、依赖、修改与义务。

远端发布、推送、设备刷写、改分区及擦除 Flash/TF 需要维护者明确授权；不从历史开发记录推定新的设备操作授权。不得改写工厂 VCOM/PMU 校准；烧录偏移以目标设备实测分区为准。凭据、个人 EPUB、完整 Flash 备份和私有日志不提交；不默认关闭 TLS 验证。

## 验证与交接

按下表选择检查，在仓库根目录使用已有入口；缺少测试字体等资源时按 `docs/development/environment.md` 准备，不以跳过代替验证。

| 改动 | 最低验证范围 |
|---|---|
| 仅文档/注释 | 相关事实、链接与规则一致性；交接检查、diff 检查 |
| 共享 C/C++、解析、排版、存储、会话 | 回归测试、host Debug/CTest；涉及内存/生命周期用 ASan/UBSan；固件构建 |
| ESP_PLATFORM 分支、main、板级、sdkconfig、依赖/构建 | 固件构建必须覆盖改动路径；验证可隔离逻辑；列出对应真机验证 |
| 响应性、缓存、并发、显示、电源 | 上述适用项，加同条件性能/资源对比及 `docs/development/acceptance.md` 对应专项 |

```sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel 4
ctest --test-dir build/host -N
ctest --test-dir build/host --output-on-failure
sh tools/build_firmware.sh
python3 tools/check_handoff.py
git diff --check
```

优先复用现有测试，仅在覆盖不足时补充必要回归；纯文案或等价整理不新增测试。修改失败或取消处理必须覆盖对应异常路径。检查 CTest 实际注册项目及资源/Node 等条件依赖；异步路径使用固件对应模式。不得降低断言、删除失败测试、禁用组件或替换依赖来冒充标准验证通过。适用检查通过且没有新改动或未解决疑点时，结束验证。

性能结论使用 `tools/summarize_reader_perf.py` 及原始日志，记录 commit、样本、冷/热缓存、配置、样本数、分位数/最大值和内存；阈值过滤日志不是完整分布。交接检查仅检查资料；host、sanitizer、固件构建、烧录摘要、真机行为和实际 KOReader 往返分别报告。host 耗时和文件操作不证明真机性能或 TF 掉电可靠，mock 和历史结果不替代本次验收。

实现任务的完成标准：约定改动及配套资料齐全、适用检查通过、本次改动引入的问题已修正。受环境或设备限制时，继续完成其余可验证工作，交付待验证候选并列明阻塞和未测项。

实际修改代码、配置或项目文档的批次在 `docs/development-status.md` 追加变更、命令/环境、结果、未测项和下一步；仅在当前实现或验收状态变化时更新「当前摘要」。纯讨论和只读审计不强制产生仓库改动。稳定规则更新其权威文档，历史记录保留原样，不把过程日志复制到 AGENTS。文档位置与维护职责见[文档导航](docs/README.md#改动后更新哪里)：只更新受影响事实，不重复抄写状态、测试总数或完整功能列表；官网、仓库指南和设备帮助仍分别维护，仅在用户操作或限制变化时核对。根文件保持小于 16,000 UTF-8 字节。

## Naming Policy

- Names must express their domain meaning within the enclosing namespace or type. Functions name an action and its object; variables name the value or state they hold. Boolean names describe a predicate or state. Avoid opaque abbreviations and generic names when the context does not identify their role; conventional local indices, coordinates and established technical abbreviations are acceptable.
- Distinguish request construction, scheduling and completion in names. Include units or representation where ambiguity matters, such as `_us`, `_bytes`, Unicode scalar offsets, and a fraction in `[0, 1]` versus a percentage in `[0, 100]`.
- Follow the surrounding C/C++ conventions: `snake_case` for functions and variables, `CamelCase` for types. Apply the [ESP-IDF style guide](https://docs.espressif.com/projects/esp-idf/zh_CN/stable/esp32/contribute/style-guide.html) where compatible. Preserve external protocol names and third-party interfaces; rename unclear project-owned symbols with all callers, without unrelated style churn.

## Code Comment Policy

Default to no comments, following [pstack comment-sicko](https://github.com/cursor/plugins/blob/main/pstack/agents/comment-sicko.md). Delete narration, section banners, implementation history, commented-out code and explanations compensating for unclear project-owned code. Express intent through names, types, constants and control flow; fix the underlying naming or structure with its callers instead of polishing the prose.

Only these exceptions may remain:

1. Legal and license headers.
2. Non-obvious behavior forced by an external dependency, platform, vendor or protocol that this project cannot reshape.
3. Formatter directives and narrowly scoped lint suppressions for faulty or style-only rules. Never suppress correctness or safety checks to silence a failure.
4. Documentation defining a public API contract that callers need, including cross-component interfaces. A private helper or implementation detail does not become a public contract by adding documentation syntax.
5. Issue or RFC links explaining a constraint that code cannot express.

Verify each retained or added comment against the current implementation, callers and, for external constraints, the relevant source. Keep it concise, objective and limited to the proven constraint. If an exception is uncertain, delete the comment. Apply the same rules to docstrings and comments emitted by project-owned generators; preserve upstream license notices and generated or vendored source provenance.

Comments are evidence to recheck, not authority that freezes a design. Warnings such as `IMPORTANT` or `do not remove`, a prior decision, and an agent's confidence do not establish an exception. Put project decisions in their authoritative documents and history in Git or issues; enforce requirements in code or existing checks where practical. Review nearby comments whenever changing code.

## 面向用户的说明

面向用户的中文说明、操作文档和界面文案参考 [Tech-Doc-Style-Chinese](https://github.com/Fenng/Tech-Doc-Style-Chinese)。其中借鉴 ASD-STE100 的方法按中文语境采用，不直接套用英文词典、语法或句长规则，也不据此宣称符合该标准。

- 表达准确、简洁、克制；保持术语一致，避免空泛宣传、行业黑话和机械直译。
- 操作说明写清执行者、对象及预期结果，区分用户操作与系统行为；按执行顺序组织步骤，每步围绕一个主要动作，必要条件与风险放在对应动作之前。
- 改写必须保留事实、限制、例外和确定程度，不补造能力、数值或结果，不为缩短句子破坏条件与因果关系。
- 中文与英文、数字混排时保持适当留白；代码、URL、API 路径及其他机器可读标识符保持原样。
- 现行文档的中文正文使用直角引号「」（嵌套用『』），段落在源文件中一段一行；`docs/design/original/` 和开发状态的历史记录保持原样。
- 根据内容选择规则；简短按钮、导航标签和概念说明不机械套用操作步骤格式。项目既有术语和明确约定优先。
