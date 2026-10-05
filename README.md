# Pico Page

Read Pico 墨水屏硬件的个人 EPUB 阅读固件，框架优先 ESP-IDF。
首版目标是可靠离线阅读，并与 KOReader 接续同一处正文。

**当前仓库内容是开发交接包，不是可编译或可刷写的固件。**
已整理需求、架构、文件式存储、设计原件、开发注意事项和验收门槛。
没有创建远端仓库，没有选择项目总许可证，没有锁定依赖或进行编译/烧录。

## 现在怎么用

将本目录内容放到新的 `pico-page` 仓库根目录，确保 `AGENTS.md` 位于根目录。
已有同名文件时先对比合并，不直接覆盖。原始设计已经随包提供，不需要再次上传。
在 Codex 中打开该目录，使用 [CODEX_START.md](CODEX_START.md) 的开工任务。

项目展示名为 **Pico Page**（单数）。`Read Pico` 只表示硬件；板级模块名称不做无意义替换。
当前设计原件里的 `Pico Pages` 保持原样，以便追溯；实际产品文案使用 `Pico Page`。

## 资料入口

| 文件 | 用途 |
|---|---|
| [AGENTS.md](AGENTS.md) | 常驻工作规则，短而明确 |
| [CODEX_START.md](CODEX_START.md) | 第一次开发任务，可直接粘贴 |
| [当前决定](docs/decisions.md) | 已确认、工作默认、待验证、旧决定作废情况 |
| [程序架构](docs/architecture.md) | 模块边界、位置/帧/保存/同步契约 |
| [文件式存储](docs/storage.md) | TF 目录、伴随文件、配置写回、备份与恢复 |
| [UI 实现注意事项](docs/ui-implementation.md) | 不重画设计，开发时纠错；正文 K2 全刷 |
| [阶段任务](docs/milestones.md) | G0–G4 分阶段交付，不一口气铺完整功能 |
| [验收矩阵](docs/acceptance.md) | 场景、期望结果、证据种类 |
| [调研线索与来源](docs/references.md) | 参考项目、具体源码入口、复用注意点 |
| [设计原件入口](docs/design/README.md) | 36 张原始 PNG、5 份 HTML、文案与 tokens |
| [当前开发状态](docs/development-status.md) | 防止新会话把计划误认成实现 |
| [环境记录](docs/environment.md) | 由 Codex 填实际工具链和命令 |
| [依赖审查](docs/dependency-review.md) | 许可证与版本审查；当前均未锁定 |
| [历史材料](docs/archive/README.md) | 仅供追溯，低于当前决定 |

## 第一轮验收

G0：核查工具链与官方 BSP，建立最小 ESP-IDF 工程，真实执行可执行的构建/host 测试。
G1a：建立小型可运行 host 验证，至少证明 K2 不移动阅读位置、过期事件不被采用、文件数据不被缓存清理误删。
同时开展最小 EPUB/KOSync 位置路径验证，不把真实往返测试拖到 UI 全部完成后。
没有设备时不宣称完成硬件门槛；可继续推进独立的 host 工作。

## 本包包含和不包含什么

包括用户原始设计的 49 个文件（其中 36 PNG、5 HTML、1 JS，其余为文本规格），
三份历史设计/评审 Markdown，以及合并后的当前规格。
原设计声明 69 个页面/组件状态，但并非每个状态都有 PNG；缺图页可用已有 HTML 和文档实现，不阻塞开发。
不提供字体文件、个人书籍、凭据、第三方生产代码或假造的 KOReader 测试结果。
示例 TOML 可被解析，但固件尚未实现相应读取逻辑。

```text
pico-page/
├── AGENTS.md / CODEX_START.md
├── docs/                   当前规格、来源、开发状态
│   ├── design/original/    原始视觉与交接文件，不改动
│   └── archive/            旧方案与评审，只作历史依据
├── firmware/               待建立 ESP-IDF 工程
├── host/                   待建立共享核心的主机测试/预览
├── tests/                  待实现测试与合成样本
├── examples/               文件式存储配置示例，无字体/凭据
└── tools/check_handoff.py  可运行的交接完整性检查
```

运行 `python3 tools/check_handoff.py` 只检查包内文件、设计源摘要、TOML 与命名约定，
不意味着固件、EPUB 或 KOSync 已经通过测试。开发开始后不要用交接清单摘要替代正式 CI。
