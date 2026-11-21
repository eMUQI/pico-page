# 文档导航

安装和日常阅读从[使用指南](user/runtime.md)开始。固件仍处于测试阶段；使用能力以相应版本的发布说明为准，源码主线可能包含尚未发布的改动。

## 使用与反馈

| 想了解的内容 | 文档 |
|---|---|
| 安装、恢复与更新 | [官方安装页](https://picopage.wulu.zone/install/) · [固件更新](user/runtime.md#固件更新) · [更新日志](https://picopage.wulu.zone/changelog/) |
| 按键、阅读、排版、书签和休眠 | [使用指南](user/runtime.md) · [官网使用文档](https://picopage.wulu.zone/docs/) |
| 放入书籍、字体和锁屏图片 | [Wi-Fi 传书](user/wifi-transfer.md) · [USB 文件传输](development/usb-storage-runtime.md) |
| 备份、移动书籍与手改配置 | [文件与备份](user/runtime.md#文件与备份) · [存储约定](development/storage.md) · [配置示例](examples/README.md) |
| KOReader 接续与兼容范围 | [操作步骤](user/runtime.md#与-koreader-接续位置) · [同步范围与验证](development/sync-runtime.md) |
| 问题反馈和后续计划 | [Issues](https://github.com/eMUQI/pico-page/issues) · [路线图](planning/roadmap.md) |

反馈时请提供版本、复现步骤和实际结果。截图、日志与样本应先去除凭据及私人内容；不要提交个人 EPUB 或完整设备备份。

## 从源码开发

| 任务 | 入口 |
|---|---|
| 构建 Read Pico 固件 | [固件开发指南](../firmware/README.md) · [环境记录](development/environment.md) · [BSP 审查](development/bsp-review.md) |
| 在电脑上运行和测试 | [桌面开发指南](../tools/host/README.md) · [测试说明](../tests/README.md) |
| 了解结构和设计取舍 | [架构](development/architecture.md) · [产品决定](planning/decisions.md) |
| 修改固件或共享组件 | [贡献指南](../.github/CONTRIBUTING.md) · [嵌入式开发规则](development/embedded-development.md) · [验收清单](development/acceptance.md) |
| 定位具体实现 | [文档解析](development/document-runtime.md) · [字体与分页](development/graphics-runtime.md) · [图片](development/image-runtime.md) · [存储](development/storage.md) · [界面](development/ui-implementation.md) · [平台](development/platform-runtime.md) |
| EPUB 原书排版 | [支持范围](development/original-layout.md) |
| 样本与兼容验证 | [公开 EPUB](../tests/README.md#公开-epub-样本) · [真实引擎对照](development/sync-runtime.md#真实引擎对照与复现) |
| 设备诊断 | [诊断构建与工具](development/device-diagnostics.md) |
| 周期性版本测试与历史对比 | [测试基准](development/testing-baselines.md) |
| 使用 AI 编程代理 | [代理规则](../AGENTS.md) · [代理工作流](development/agent-workflow.md) |
| 版本、依赖和网站 | [版本管理](development/versioning.md) · [依赖审查](development/dependency-review.md) · [网站开发](../web/README.md) · [固件托管](development/firmware-hosting.md) |

## 状态与规划

[开发状态](development-status.md)列出当前实现与待验证项；[测试基准](development/testing-baselines.md)规定公开实测口径；[验收清单](development/acceptance.md)定义应验证的行为。测试通过、构建成功、发布和真机验收是不同状态，不能互相替代。

[路线图](planning/roadmap.md)用于后续功能规划，[设计系统](design/system/README.md)维护视觉规范。历史资料中的路径、分区和命令不能直接作为当前安装或开发步骤。现行需求冲突按 [AGENTS.md](../AGENTS.md) 中的资料优先级处理。

## 文档放在哪里

| 位置 | 内容与主要读者 | 维护边界 |
|---|---|---|
| 根目录 `README.md` | 项目介绍，面向读者与贡献者 | 保留概览和入口，不复制完整用法或验收记录 |
| `.github/CONTRIBUTING.md`、根目录 `AGENTS.md` | 贡献流程；AI 编程代理的工作规则 | 只维护规则、资料优先级和导航；技术事实与开发者共用 |
| `docs/user/` | 使用指南与 Wi-Fi 传书，面向设备用户 | 维护操作、结果与用户需要知道的限制 |
| `docs/development/` | 实现、存储、构建环境、验收、诊断和运维，供开发者与 agent 共用 | 按对应主题维护当前规则；不复制功能排期 |
| `docs/planning/` | [产品决定](planning/decisions.md)与[路线图](planning/roadmap.md) | 维护决定、范围、依赖和候选；实际完成度链接开发状态 |
| `docs/development-status.md` | 当前实现与待验证项 | 维护当前实现与验收结论；未来过程记在 PR，公开结论不依赖旧私有历史 |
| `docs/releases/` | 面向用户的三语版本说明 | 按版本维护，GitHub Release、官网和设备更新清单读取同一来源 |
| `docs/design/` | [设计系统](design/system/README.md)：颜色、字体、尺寸与组件外观，供设计、开发与 agent 查阅 | 视觉规范以设计系统为准，当前交互以开发目录的 UI 说明为准 |
| `firmware/`、`tools/host/`、`tests/`、`web/` 的 README | 对应子项目的构建、测试或部署入口 | 操作命令就近维护，跨模块规则用链接 |
| `docs/examples/`、`docs/images/`、组件 `third_party/` | 配置示例、合成画面、依赖来源与许可 | 示例随格式核对；只保留文档引用的画面；许可随依赖变更 |

官网使用说明在 `web/src/components/DocsPage.astro`，安装说明在 `web/src/components/InstallPage.astro`；设备内置帮助在 `firmware/components/reader_app/help_content.h`。它们和仓库使用指南分别维护，用户操作或限制改变时核对受影响的说明。
