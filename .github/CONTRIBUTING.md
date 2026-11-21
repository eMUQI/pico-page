# 参与贡献

感谢参与 Pico Page。本文说明提交问题和改动的流程；产品规则、实现边界和验收要求以 `docs/` 中的对应文档为准。

## 反馈问题

在 [Issues](https://github.com/eMUQI/pico-page/issues) 中写明固件版本、复现步骤、预期结果和实际结果。截图、日志与样本应先去除凭据及私人内容；不要上传个人 EPUB、完整设备备份或包含 Wi-Fi 口令的配置。

## 准备环境

- 固件构建：[固件开发指南](../firmware/README.md)、[环境记录](../docs/development/environment.md)。
- 桌面运行与测试：[桌面开发指南](../tools/host/README.md)、[测试说明](../tests/README.md)。
- 结构与取舍：[架构](../docs/development/architecture.md)、[产品决定](../docs/planning/decisions.md)、[嵌入式开发规则](../docs/development/embedded-development.md)。

## 提交改动

1. 先在 Issue 中说明较大的功能或行为变化，确认与[产品决定](../docs/planning/decisions.md)和[路线图](../docs/planning/roadmap.md)一致。
2. 从 `main` 新建分支，一个 PR 只处理一个主题。改动行为时同步更新受影响的文档，见下文「改动后更新哪里」。
3. 在仓库根目录运行适用的检查：

   ```sh
   cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
   cmake --build build/host --parallel 4
   ctest --test-dir build/host --output-on-failure
   sh tools/build_firmware.sh
   python3 tools/check_handoff.py
   git diff --check
   ```

   仅改文档时运行后两项。涉及显示、电源、存储或性能的改动另按[验收清单](../docs/development/acceptance.md)做真机验证。
4. PR 描述写明改动内容、执行的检查及结果、未测范围。真机结果注明设备、构建类型和样本；性能数据按[测试基准](../docs/development/testing-baselines.md)记录。

不要提交凭据、个人 EPUB、完整 Flash 备份或私人日志。使用 AI 编程代理时，代理须遵守 [AGENTS.md](../AGENTS.md) 和[代理工作流](../docs/development/agent-workflow.md)。

## 改动后更新哪里

同一事实指定一个主要维护位置，其他文档给出必要摘要和链接，不逐份复制完整规则。更新范围按事实是否变化判断，不要求每次代码修改都更新整套文档。

| 改变的事实 | 主要维护位置 | 其他位置如何处理 |
|---|---|---|
| 产品行为、范围或取舍 | [产品决定](../docs/planning/decisions.md) | 实现说明采用现行规则；AGENTS 保留导航和关键底线 |
| 组件职责与所有权 | [架构](../docs/development/architecture.md) | 具体调用细节在对应运行文档，规划不复制 |
| 文件格式、路径与兼容性 | [存储约定](../docs/development/storage.md) | 示例同步核对；提交算法在存储实现，用户指南只说明操作和风险 |
| 颜色、字体、线框、尺寸与组件外观 | [设计系统](../docs/design/system/README.md) | UI 说明只链接视觉规则，不复制；在线设计系统页面是只读副本，改动先提交仓库再同步 |
| 页面与输入行为 | [UI 说明](../docs/development/ui-implementation.md) | 用户可见变化再核对官网、使用指南和设备帮助；不把技术细节搬进帮助 |
| 解析、排版、图片、同步、平台机制 | 对应 `docs/development/*-runtime.md` | 锁屏准备与 OTA 协议在平台文档；路线图只引用实现 |
| 构建、测试入口与依赖 | 子项目 README、[环境](../docs/development/environment.md)、[依赖审查](../docs/development/dependency-review.md) | 测试总数查 `ctest -N`，不写进文档 |
| 验收场景与数值目标 | [验收清单](../docs/development/acceptance.md) | 保留可执行的预期结果；不维护另一份当前通过清单 |
| 实现与验收状态 | [开发状态](../docs/development-status.md)，公开测量见[测试基准](../docs/development/testing-baselines.md) | 后续贡献的过程写在 PR；公开理解所需结论随文档交付，路线图引用状态 |
| 未来功能与依赖 | [路线图](../docs/planning/roadmap.md)及专题计划 | 已落地的详细规则转入实现文档 |

内部重构且行为、接口与约束不变时，不改用户指南，只更新确实失效的技术说明。性能优化记录实测条件与结果，不改写成未经验证的产品能力。已发布版本说明和带日期的测试报告不因当前实现改变而重写。迁移前的私有 PR、Issue 和 Git 历史不是公开读者的必要前提；现行规则、历史测量条件与限制须能从仓库文档独立理解。整理历史来源时保留原日期、测量值和旧源码身份，补充缺口，不冒充新的实测或完整可重现结果。

## 许可

提交到本仓库的改动按[许可说明](../docs/LICENSING.md)中的项目许可发布。复用第三方代码前核对其许可并在[依赖审查](../docs/development/dependency-review.md)中记录来源与义务。
