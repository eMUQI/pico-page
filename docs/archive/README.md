# 历史资料 — 不作为当前需求权威

以下三份原始设计与评审按原字节保留，用于追溯。旧决定已在 [decisions.md](../planning/decisions.md) 中逐项处理。
初次开工不需要全部阅读，当前文件已收敛有效结论。

- [architecture-v0.1.md](architecture-v0.1.md)：原始模块设计；其中 NVS 主存储、固定阅读包路线已被后续方案修正。
- [ui-brief-v0.1.md](ui-brief-v0.1.md)：设计前要求，不取代用户后来接受的设计稿。
- [ui-review-v1.md](ui-review-v1.md)：原始评审；**R06 建议改 K2 已作废**；其他条目按当前 UI 注意事项分为实现纠错与后置优化。

评审里的 PNG 和 HTML 路径对应 `../design/original/`。
没有将历史报告 HTML 及其重复渲染图全部复制进仓库，也不把旧 AI 架构图作为精确实现接口。
用户现在允许沿原稿开发，不需等待设计师完成本目录中的全部建议。

- [development-start-20261005.md](development-start-20261005.md)：初始开发任务正文快照；旧输入映射和阶段范围不再适用。

## 开发与界面历史

2026-10-24 整理将过程记录移出日常入口。以下快照保留整理前的完整正文、日期、验证结果及未完成项，仅修正迁移后的 Markdown 相对链接及一个原有失效的 COS 章节链接；「当前」「候选」和操作授权仅指原记录。现行状态查[开发状态](../development-status.md)，实现规则查[UI 说明](../development/ui-implementation.md)和[路线图](../planning/roadmap.md)。

| 归档 | 内容 |
|---|---|
| [完整开发状态](development-status-20261024.md) | 整理前的摘要和全部批次，包括命令、性能数据、失败、未测项与 Issue #53 撤回记录。 |
| [UI 说明快照](ui-implementation-20261024.md) | 原章节编号、各次设计差异、截图路径和被后续规则替代的旧行为。 |
| [路线图快照](roadmap-20261024.md) | 旧版本排期、调研出处、实现候选与验收边界。 |
| [初始开发与交接](initial-development-20261005-06.md) | 合并下列四份早期文档，旧文件入口已移除，原正文保留。 |

合并前的文件对应章节：

- `docs/implementation-g0-g1a.md` → [G0/G1a 实施](initial-development-20261005-06.md#implementation-g0-g1a)。
- `docs/runtime-plan.md` → [可用初版任务](initial-development-20261005-06.md#runtime-plan)。
- `docs/handoff-next-session.md` → [后续交接](initial-development-20261005-06.md#handoff-next-session)。
- `docs/handoff-validation.md` → [交接包检查](initial-development-20261005-06.md#handoff-validation)。

## 初期专项计划与结果

- [USB 文件传输设计](usb-file-transfer-design-20261006.md)与[实施计划](usb-file-transfer-plan-20261006.md)：从 `docs/superpowers/specs/`、`docs/superpowers/plans/` 迁入，保留历史正文并修正相对链接。当前 USB 行为查[运行说明](../development/usb-storage-runtime.md)。
- [2026-10-06 初版阶段与验收结果](initial-validation-20261006.md)：从里程碑和验收标准迁出；标准与实际结果分别维护，历史未烧录、未发布等状态不适用于当前版本。

2026-10-24 目录分类后，现行文档分别位于 `docs/user/`、`docs/development/`、`docs/planning/`。本目录快照中的 Markdown 链接随目标迁移修正；正文、命令及代码片段中的旧路径保留为历史，不能直接当作当前操作步骤。三份受保护的原始历史资料与设计原件保持原字节。

- [图片历史快照](image-runtime-20261025.md)：保留早期解码验证与透明层资源说明；当前资源交还、流式锁屏路径查平台说明，避免把旧分配方式继续当作现行规则。
