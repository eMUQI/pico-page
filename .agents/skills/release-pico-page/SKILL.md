---
name: release-pico-page
description: Prepare, package, publish, or verify a Pico Page firmware release for Read Pico. Use for release candidates, release notes, GitHub Release attachments, and checking the resulting website and OTA deployment.
---

# 发布 Pico Page

在 Pico Page 仓库根目录工作。按 `docs/development/versioning.md` 确认当前版本、四个附件和发布范围。按 `docs/development/environment.md` 准备固定的 ESP-IDF、链接脚本补丁和 SBOM 工具。

## 准备候选

1. 核对工作区、最近已发布标签和本次变更。更新 `firmware/version.txt`、`docs/releases/<版本>.md` 及 `docs/releases/zh-Hant/`、`docs/releases/en/` 下的同名译文。按 Keep a Changelog 2.0.0 选择有内容的分组，记录读者可感知的变化和必要的迁移步骤。三份说明同时供 GitHub、官网和设备更新清单使用。
2. 在提交对应的干净 `v<版本>` 标签上运行 `python3 tools/setup_fonts.py` 和 `python3 tools/package_release.py`。使用 `build/release/<版本>/` 里的四个附件。按 `docs/development/versioning.md` 的布局校验命令复核完整发布附件。核对资料包中的源码入口可公开访问，按实际分发组件复核许可与 NOTICE。
3. 用 `verify-pico-page` 选择本次变更需要的检查。分别记录构建、安装、OTA 与回滚的结果。准备发布说明或本地附件时，到候选交付为止。推送、公开发布、部署和刷写按仓库授权边界执行。

## 发布并读回

1. 确认远端标签指向已验收提交。运行 `python3 tools/release_notes.py <版本> > build/release/<版本>-notes.md` 生成三语正文。用维护者授权的 GitHub CLI 运行 `gh release create <标签> --verify-tag --draft --notes-file build/release/<版本>-notes.md build/release/<版本>/*` 创建草稿。Beta 和 RC 按版本规则标记 `--prerelease`。公开预发布同样参与新布局版本选择。首次发布双 7 MiB 固件前先部署渠道隔离。
2. 用 `gh release view <标签> --json tagName,isDraft,isPrerelease,body,assets` 核对草稿状态、正文和附件的名称及大小。用 Release Assets API 读回各附件的 `digest` 并与本地 SHA-256 比较。按授权执行 `gh release edit <标签> --draft=false`。公开 Release 会触发生产部署。
3. 跟踪该标签触发的 `Website build`。从官网读回双 4 MiB 和双 7 MiB 的 OTA 清单及安装页，核对版本、下载地址和摘要，再核对更新日志与 COS 下载内容。异常按 `docs/development/firmware-hosting.md` 处理，保持已发布标签和镜像的不可覆盖约束。
