# Pico Page — repository instructions

这是 Read Pico 硬件上的个人阅读固件。项目名是 **Pico Page**，不是 Pico Pages；
仓库/目录建议 `pico-page`，构建标识 `pico_page`，板级名称仍为 `read_pico`。
这是开工交接仓库；不能把文档、设计 HTML 或 mock 当成已经实现的固件。

## 必读与需求优先级

先读 `docs/decisions.md`、`docs/development-status.md` 和当前任务。
首次开发再读 `docs/architecture.md`、`docs/storage.md`、
`docs/ui-implementation.md`、`docs/milestones.md`；按当前工作读取 `docs/acceptance.md`。

项目需求冲突时：用户在当前任务中的明确更新 > `docs/decisions.md` >
当前 architecture/storage/ui-implementation > `docs/design/original/` > `docs/archive/`。
这是项目资料间的优先级，不覆盖工具权限和更高层规则。
历史文件保留原样供追溯，不能从中恢复已经废弃的决定。

## 不要重新讨论的产品决定

- 主要自用、计划开源，独立设计；第一版只做 Read Pico，框架优先 ESP-IDF。
- 第一版验收：EPUB 离线阅读 + KOReader 双向位置接续。HTTP 请求成功不等于接续成功。
- 当前设计稿直接作为视觉基线，不重设计，不等待设计师重出稿才写代码。
- **所有可交互页面 K2 = ForceRefresh（立即全刷当前画面）**；正文中部触摸打开工具。
  K2 不关闭面板、不取消预览、不确认冲突、不退出 USB 传输；关闭/取消使用屏幕控件。
  刷新进行中忽略重复 K2；硬件休眠的唤醒来源仍由板级能力决定。
- 后续可能自定义按键：目前只做集中的上下文动作映射，不新增自定义页面。长按书库条目进入详情，长按正文图片进入图片查看；不新增其他长按快捷动作。
- 用户配置、单书设置和进度以 TF 卡普通文件为主。索引与缓存不是数据权威；NVS 不保存隐藏的权威阅读配置/进度。
- 接受电脑预处理，但不预先冻结专有阅读包；先验证一个最小可用 EPUB 路径，不同时实现两套后端。
- PDF、Karakeep、批注/词典、插件、统计、多个主题和完整 OTA 产品流程后置。

## 实现边界

UI 只发动作与画状态；`session` 独占阅读位置；`display` 独占面板刷新；
`store` 统一管理文件提交；同步回包只是候选，不直接改页。
文档、排版、位置和同步策略中的纯逻辑应能在 host 测试。
异步请求带书籍/会话/排版代次；旧结果不得覆盖新会话。
只保存已成功呈现的正文位置；只同步已确认保存的版本。
当前页优先排版；不能用整书阻塞重排、超长 Label 或 CSS 裁切代替分页。
K2 手动全刷必须绕过“像素没变就不刷”的优化；不改变锚点或进度修订号。

## 现有设计的处理

按 `docs/ui-implementation.md` 修正裁字、误导状态、输入穿透和数据丢失风险。
允许沿用现有组件做最小文案/状态/热区修正；不要借评审重做布局、品牌或导航。
原始 PNG/HTML 不修改；新增实现截图放 `artifacts/`，记录差异与原因。
设计图里的书名、时间、百分比、服务地址不是运行时真值。

## 工作方式与安全边界

先检查已有文件与 Git 差异，保护用户未提交的改动。先查现有 ESP-IDF/EIM 工具链，
不要因 PATH 中没有 `idf.py` 就断言未安装；不要自行替换全局工具链。
复用前读第三方源码/许可证，记录 commit、依赖、修改与义务；顶层许可证不代表子组件。
未经用户明确授权，不发布仓库、不推送远端、不刷写设备、不改分区、不擦除 Flash/TF。
不得改写工厂 VCOM/PMU 校准，也不能直接复制其他固件的烧录偏移。
凭据、个人 EPUB、完整 Flash 备份和私有日志不提交；不默认关闭 TLS 验证。
不能编译/没连设备时继续完成可运行的 host 工作，并明确未验证项。

## 验证与交接

交接检查：`python3 tools/check_handoff.py`（Python 3.11+；仅检查交接包）。
host：根目录 CMake 构建后运行 CTest；固件：`sh tools/build_firmware.sh`。
具体环境、命令和实际结果见 `docs/environment.md`；编译通过不等于硬件测试通过。
每批完成后更新 `docs/development-status.md`：变更、实际命令与结果、未测项、下一步。
当前已进入可运行初版集成，继续按实际开发状态推进；不要退回只产出计划。
遇到硬件/网络阻塞不得用成功占位顶替；桌面运行与引擎对照不等于真机双向接续。

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

重点阻止：K2 被改成导航、取消或确认、私自重设计、NVS 覆盖手改配置、删缓存丢进度、
上传旧版本却标新版本已上传、未显示页记已读、旧异步回包污染、失败或取消后静默覆盖远端、
保存失败仍宣称已保存、把编译/模拟器通过写成真机通过。
