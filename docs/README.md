# 文档导航

安装和日常阅读从[使用指南](runtime.md)开始。固件仍处于测试阶段；使用能力以相应版本的发布说明为准，源码主线可能包含尚未发布的改动。

## 使用与反馈

| 想了解的内容 | 文档 |
|---|---|
| 安装、恢复与更新 | [官方安装页](https://picopage.wulu.zone/install/) · [固件更新](runtime.md#固件更新) · [更新日志](https://picopage.wulu.zone/changelog/) |
| 按键、阅读、排版、书签和休眠 | [使用指南](runtime.md) · [官网使用文档](https://picopage.wulu.zone/docs/) |
| 放入书籍、字体和锁屏图片 | [Wi-Fi 传书](wifi-transfer.md) · [USB 文件传输](usb-storage-runtime.md) |
| 备份、移动书籍与手改配置 | [文件与备份](runtime.md#文件与备份) · [存储约定](storage.md) · [配置示例](../examples/README.md) |
| KOReader 接续与兼容范围 | [操作步骤](runtime.md#与-koreader-接续位置) · [同步范围与验证](sync-runtime.md) |
| 问题反馈和后续计划 | [Issues](https://github.com/eMUQI/pico-page/issues) · [路线图](roadmap.md) |

反馈时请提供版本、复现步骤和实际结果。截图、日志与样本应先去除凭据及私人内容；不要提交个人 EPUB 或完整设备备份。

## 从源码开发

| 任务 | 入口 |
|---|---|
| 构建 Read Pico 固件 | [固件开发指南](../firmware/README.md) · [环境记录](environment.md) · [BSP 审查](bsp-review.md) |
| 在电脑上运行和测试 | [桌面开发指南](../host/README.md) · [测试说明](../tests/README.md) |
| 了解结构和设计取舍 | [架构](architecture.md) · [产品决定](decisions.md) · [调研参考](references.md) |
| 修改固件或共享组件 | [仓库规则](../AGENTS.md) · [嵌入式开发规则](embedded-development.md) · [验收清单](acceptance.md) |
| 定位具体实现 | [文档解析](document-runtime.md) · [字体与分页](graphics-runtime.md) · [图片](image-runtime.md) · [存储](store-implementation.md) · [界面](ui-implementation.md) · [平台](platform-runtime.md) |
| 设备诊断 | [诊断接口](console-runtime.md) · [诊断工作台](diagnostic-workbench.md) |
| 版本、依赖和网站 | [版本管理](versioning.md) · [依赖审查](dependency-review.md) · [网站开发](../web/README.md) |

## 状态与历史资料

[开发状态](development-status.md)记录各批次的改动、环境、结果和未测项；[设备检查记录](device-validation.md)记录特定设备的操作；[验收清单](acceptance.md)定义应验证的行为。测试通过、构建成功、发布和真机验收是不同状态，不能互相替代。

[里程碑](milestones.md)用于阶段规划，[原始设计](design/README.md)和[归档](archive/README.md)用于追溯。历史中的路径、分区、命令和授权仅适用于当次记录，不能直接作为当前安装或开发步骤。现行需求冲突按 [AGENTS.md](../AGENTS.md) 中的资料优先级处理。
