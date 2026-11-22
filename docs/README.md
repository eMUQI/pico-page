# 文档导航

安装和日常阅读从[使用指南](user/runtime.md)开始。各版本的功能以对应的发布说明为准，源码主线可能包含尚未发布的改动。

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
| 使用 AI 编程代理 | [代理规则](../AGENTS.md) · [代理工作流](development/agent-workflow.md) |
| 版本、依赖和网站 | [版本管理](development/versioning.md) · [依赖审查](development/dependency-review.md) · [网站开发](../web/README.md) · [固件托管](development/firmware-hosting.md) |

## 状态与规划

[开发状态](development-status.md)列出当前版本和已知限制；[验收清单](development/acceptance.md)定义应验证的行为。

[路线图](planning/roadmap.md)用于后续功能规划，[设计系统](design/system/README.md)维护视觉规范。现行需求冲突按 [AGENTS.md](../AGENTS.md) 中的资料优先级处理。

## 文档放在哪里

各类事实的主要维护位置见[贡献指南](../.github/CONTRIBUTING.md#改动后更新哪里)。官网使用说明在 `web/src/components/DocsPage.astro`，安装说明在 `web/src/components/InstallPage.astro`；设备内置帮助在 `firmware/components/reader_app/help_content.h`。它们和仓库使用指南分别维护，用户操作或限制改变时核对受影响的说明。
