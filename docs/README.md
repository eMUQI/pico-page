# 文档导航

安装和日常阅读从[使用指南](user/runtime.md)开始。固件仍处于测试阶段；使用能力以相应版本的发布说明为准，源码主线可能包含尚未发布的改动。

## 文档放在哪里

| 位置 | 内容与主要读者 | 维护边界 |
|---|---|---|
| 根目录 `README.md` | 项目介绍，面向读者与贡献者 | 保留概览和入口，不复制完整用法或验收记录 |
| 根目录 `AGENTS.md` | agent 工作规则与开工入口 | 只维护规则、资料优先级和导航；技术事实与开发者共用 |
| `docs/user/` | 使用指南与 Wi-Fi 传书，面向设备用户 | 维护操作、结果与用户需要知道的限制 |
| `docs/development/` | 实现、存储、构建环境、验收、诊断和运维，供开发者与 agent 共用 | 按对应主题维护当前规则；不复制功能排期 |
| `docs/planning/` | 产品决定、路线图、里程碑、词典方案与调研 | 维护决定、范围、依赖和候选；实际完成度链接开发状态 |
| `docs/development-status.md` | 当前实现、未验收项和批次记录，供维护者与 agent 共用 | 当前摘要只在状态变化时更新；每批记录对应命令、结果和未测项 |
| `docs/releases/` | 面向用户的版本说明 | 按版本维护，官网和设备更新清单读取同一来源 |
| `docs/design/` | [设计系统](design/system/README.md)、当前设计说明及原始 PNG／HTML，供设计、开发与 agent 查阅 | 原件不修改；视觉规范以设计系统为准，当前交互以开发目录的 UI 说明为准 |
| `docs/archive/` | 旧设计、计划和验证记录 | 只供追溯，不跟随代码更新；迁移时只修正必要链接并注明来源 |
| `firmware/`、`host/`、`tests/`、`web/` 的 README | 对应子项目的构建、测试或部署入口 | 操作命令就近维护，跨模块规则用链接 |
| `examples/`、`artifacts/`、组件 `third_party/` | 配置示例、截图证据、依赖来源与许可 | 示例随格式核对；证据按批次保留；许可随依赖变更 |

官网使用说明仍在 `web/src/pages/docs.astro`，安装说明在 `web/src/pages/install.astro`；设备内置帮助仍在 `firmware/components/reader_app/help_content.h`。它们和仓库使用指南分别维护，本次目录整理没有合并内容来源；用户操作或限制改变时核对受影响的说明。

## 改动后更新哪里

同一事实指定一个主要维护位置，其他文档给出必要摘要和链接，不逐份复制完整规则。更新范围按事实是否变化判断，不要求每次代码修改都更新整套文档。

| 改变的事实 | 主要维护位置 | 其他位置如何处理 |
|---|---|---|
| 产品行为、范围或取舍 | [产品决定](planning/decisions.md) | 实现说明采用现行规则；AGENTS 保留导航和关键底线 |
| 组件职责与所有权 | [架构](development/architecture.md) | 具体调用细节在对应运行文档，规划不复制 |
| 文件格式、路径与兼容性 | [存储约定](development/storage.md) | 示例同步核对；提交算法在存储实现，用户指南只说明操作和风险 |
| 颜色、字体、线框、尺寸与组件外观 | [设计系统](design/system/README.md) | UI 说明只链接视觉规则，不复制；在线设计系统页面是只读副本，改动先提交仓库再同步 |
| 页面与输入行为 | [UI 说明](development/ui-implementation.md) | 用户可见变化再核对三处用户说明；不把技术细节搬进帮助 |
| 解析、排版、图片、同步、平台机制 | 对应 `docs/development/*-runtime.md` | 锁屏准备与 OTA 协议在平台文档；路线图只引用实现 |
| 构建、测试入口与依赖 | 子项目 README、[环境](development/environment.md)、[依赖审查](development/dependency-review.md) | 测试总数查 `ctest -N`；实际命令输出只记当次结果 |
| 验收场景与数值目标 | [验收矩阵](development/acceptance.md) | 保留可执行的预期结果；不维护另一份当前通过清单 |
| 合并、发布、测试及性能实测结果 | [开发状态](development-status.md)，设备明细在[设备记录](development/device-validation.md) | 路线图、计划和入口引用状态，不重复维护提交号与通过数量 |
| 未来功能与依赖 | [路线图](planning/roadmap.md)及专题计划 | 已落地的详细规则转入实现文档；旧方案留归档 |

内部重构且行为、接口与约束不变时，不改用户指南；只更新确实失效的技术说明和当次开发记录。性能优化记录实测条件与结果，不改写成未经验证的产品能力。历史证据和已发布版本说明不因当前实现改变而重写。

## 使用与反馈

| 想了解的内容 | 文档 |
|---|---|
| 安装、恢复与更新 | [官方安装页](https://picopage.wulu.zone/install/) · [固件更新](user/runtime.md#固件更新) · [更新日志](https://picopage.wulu.zone/changelog/) |
| 按键、阅读、排版、书签和休眠 | [使用指南](user/runtime.md) · [官网使用文档](https://picopage.wulu.zone/docs/) |
| 放入书籍、字体和锁屏图片 | [Wi-Fi 传书](user/wifi-transfer.md) · [USB 文件传输](development/usb-storage-runtime.md) |
| 备份、移动书籍与手改配置 | [文件与备份](user/runtime.md#文件与备份) · [存储约定](development/storage.md) · [配置示例](../examples/README.md) |
| KOReader 接续与兼容范围 | [操作步骤](user/runtime.md#与-koreader-接续位置) · [同步范围与验证](development/sync-runtime.md) |
| 问题反馈和后续计划 | [Issues](https://github.com/eMUQI/pico-page/issues) · [路线图](planning/roadmap.md) |

反馈时请提供版本、复现步骤和实际结果。截图、日志与样本应先去除凭据及私人内容；不要提交个人 EPUB 或完整设备备份。

## 从源码开发

| 任务 | 入口 |
|---|---|
| 构建 Read Pico 固件 | [固件开发指南](../firmware/README.md) · [环境记录](development/environment.md) · [BSP 审查](development/bsp-review.md) |
| 在电脑上运行和测试 | [桌面开发指南](../host/README.md) · [测试说明](../tests/README.md) |
| 了解结构和设计取舍 | [架构](development/architecture.md) · [产品决定](planning/decisions.md) · [调研参考](planning/references.md) |
| 修改固件或共享组件 | [仓库规则](../AGENTS.md) · [嵌入式开发规则](development/embedded-development.md) · [验收清单](development/acceptance.md) |
| 定位具体实现 | [文档解析](development/document-runtime.md) · [字体与分页](development/graphics-runtime.md) · [图片](development/image-runtime.md) · [存储](development/store-implementation.md) · [界面](development/ui-implementation.md) · [平台](development/platform-runtime.md) |
| EPUB 原书样式原型 | [支持契约与接入建议](development/css-style-prototype.md) · [调研与测量边界](development/baselines/2026-11-04-css-prototype.md) |
| 样本与兼容探针 | [公开 EPUB](development/public-books.md) · [EPUB/KOSync 探针](development/epub-kosync-probe.md) |
| 设备诊断 | [诊断接口](development/console-runtime.md) · [诊断工作台](development/diagnostic-workbench.md) |
| 周期性版本测试与历史对比 | [测试基准](development/testing-baselines.md) |
| 版本、依赖和网站 | [版本管理](development/versioning.md) · [依赖审查](development/dependency-review.md) · [网站开发](../web/README.md) · [固件托管](development/firmware-hosting.md) |

## 状态与历史资料

[开发状态](development-status.md)集中列出当前实现、待验收项与新批次；详细历史通过[归档导航](archive/README.md)按原日期追溯；[设备检查记录](development/device-validation.md)记录特定设备的操作；[验收清单](development/acceptance.md)定义应验证的行为。测试通过、构建成功、发布和真机验收是不同状态，不能互相替代。

[里程碑](planning/milestones.md)与[词典计划](planning/dictionary-plan.md)用于阶段规划，[原始设计](design/README.md)和[归档](archive/README.md)用于追溯。历史中的路径、分区、命令和授权仅适用于当次记录，不能直接作为当前安装或开发步骤。现行需求冲突按 [AGENTS.md](../AGENTS.md) 中的资料优先级处理。
