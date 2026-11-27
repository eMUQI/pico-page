# 开发状态

本页记录主线的当前状态。各版本面向用户的变化见[更新日志](https://picopage.wulu.zone/changelog/)，后续功能见[路线图](planning/roadmap.md)，问题跟踪见 [Issues](https://github.com/eMUQI/pico-page/issues)。

## 当前版本

当前发布版本为 [0.8.0](releases/0.8.0.md)，主线可能包含尚未发布的改动。双 4 MiB 布局的 [OTA 清单](https://picopage.wulu.zone/firmware/manifest.json)固定提供 0.6.2，双 7 MiB 布局使用[独立清单](https://picopage.wulu.zone/firmware/ota-v2/manifest.json)；旧布局设备需通过[官网安装页](https://picopage.wulu.zone/install/)完整重装一次，见[分区迁移方案](planning/ota-partition-migration.md)。

主线开发版支持离线阅读统计，入口及计时规则见[使用指南](user/runtime.md#阅读统计)。

## 已知限制

- 原书排版是 EPUB 的默认排版，仍为试验功能；部分书首次打开有明显等待，全书页数需要后台统计。支持范围见[原书排版](development/original-layout.md)。
- 跨章或跨分段时仍可能等待数秒。
- KOReader 接续只在两端位置能精确对应时提供，含表格等复杂结构的章节不提供，见[同步说明](development/sync-runtime.md)。
- 繁体中文和英文界面的译文仍在校对，见[界面多语言](development/localization.md)。
- 设备不验证固件发布者的签名，只应从官网安装或更新。

## 验证

各项行为的验证要求和数值目标见[验收清单](development/acceptance.md)。host 测试、固件构建、发布和真机验证是不同的结论，PR 描述分别写明实际执行的检查和未覆盖的范围。
