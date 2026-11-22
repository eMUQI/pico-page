# AI 编程代理工作流

使用 AI 编程代理参与开发时，按本文接入 pstack 开发流程。项目规则仍以根目录 AGENTS.md 和对应产品、实现文档为准。仓库只维护验证入口，不复制或改写上游 skill。

## 安装固定版本

使用 [cursor/plugins PR #1143](https://github.com/cursor/plugins/pull/1143) 的 Codex 生成包。该 PR 尚未合并。固定提交为 `1937543ca806041f6a9111aa97961e5bd274b7d6`，包版本为 `0.15.36`，来源目录为 `harness/pstack/codex`，许可为 MIT。保留包内的 LICENSE、代理说明和全部相对引用。

在已有 Codex CLI 的终端运行：

```sh
codex plugin marketplace add cursor/plugins --ref 1937543ca806041f6a9111aa97961e5bd274b7d6 --sparse .agents/plugins --sparse harness/pstack/codex --sparse origin-apps --json
codex plugin add pstack@cursor-plugins --json
```

如果已经配置同名 marketplace，先核对其来源和固定提交，不覆盖其他项目正在使用的配置。安装命令在 Codex 用户配置中启用 `pstack@cursor-plugins`；仓库不提供项目级 Codex 配置。

安装后新开会话；桌面客户端若未刷新插件列表，重新启动后核对。在仓库根目录运行 `codex debug prompt-input`，确认 pstack 的 `setup-pstack` 和项目的 `verify-pico-page` 出现在技能目录中。该命令可能包含本机路径和上下文，输出只放 `.local/`。上游大多数 skill 保持手动调用，因此没有出现在自动目录中不表示安装失败。

## 执行任务

从已安装 skill 的路径确定 pstack 根目录，先读 `docs/harness/codex.md`，再读 `skills/poteto-mode/SKILL.md`。可在输入中选择 `pstack:poteto-mode`，或使用 `$pstack:poteto-mode`。由主会话执行流程，子代理只承担明确分工；通过绝对路径读取同包中的其他 skill，不临时读取 GitHub main 替换固定版本。

没有模型配置表时，沿用上游 Codex 适配的 `inherit-parent` 默认值，不写入 Cursor 模型名称。同模型的独立代理不算跨模型验证。确需改变角色模型时使用 `setup-pstack`，不在 AGENTS 中重复维护模型清单。

调用 [`verify-pico-page`](../../.agents/skills/verify-pico-page/SKILL.md) 选择已有检查与实际操作入口。先复现，再修复，使用相同输入验证结果；已执行的检查和未覆盖范围分别写明。普通讨论、文案修正按原有范围处理，不为运行 pstack 扩大任务。

本项目在当前检出分支工作；没有维护者明确要求时不创建 worktree。并行代理使用互不重叠的文件范围，集成和验收由主代理负责。pstack 的自主执行说明不扩大当前任务的推送、合并、部署或设备操作授权。物理设备和串口只能由一个执行者操作。

Cursor 专用工具按包内 Codex 适配替换；缺少独立审查、UI 控制或硬件时，写明未验证项，不将跳过视为通过。压缩会话历史助手要求 Node 24.22 及更高的 24.x，或 Node 26.10 及以上；其他工作流不因此更换项目工具链。

## 升级与回退

升级前阅读原生 Codex 适配和生成器差异，选择新的固定提交，重新执行安装与发现检查，再用一个真实任务核对委派、验证和交付。更新本文的来源提交，保持安装包原样。使用 `maintain-verification-skill` 维护项目入口时，只同步已变化的命令和行为。

需要停用时，把 Codex 用户配置中 `pstack@cursor-plugins` 的 `enabled` 改为 `false`，并同步撤销 AGENTS 中的工作流入口；不删除其他项目共用的 marketplace 或插件缓存。
