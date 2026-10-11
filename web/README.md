# Pico Page 网站

Astro 静态站，用于介绍 Pico Page 的功能、限制和开发状态。

```sh
npm ci --prefix web --ignore-scripts
npm run build --prefix web
npm run preview --prefix web
```

构建产物在 `web/dist/`。

- `src/components/HomePage.astro`：首页。
- `src/pages/`：路由。新增语言或文档页时在这里添加 Astro 或 Markdown 页面。
- `public/assets/styles.css`：样式。
- `public/assets/screens/`：界面图，来自 host 桌面版渲染，使用合成数据。

网站文案面向读者。验证状态、验收标准和开发注意事项写在根目录 README 和 `docs/` 中。

## 字体下载

安装区提供霞鹜文楷 Regular v1.520 的下载，静态文件位于 `public/downloads/`。ZIP 包含未经修改的官方 `LXGWWenKai-Regular.ttf`、该版本的 `OFL.txt` 和注明作者、来源及安装方法的 `README.txt`；网页另提供许可证文本和官方版本链接。字体按 SIL OFL 1.1 单独授权。

- 作者与项目：[LXGW / LxgwWenKai](https://github.com/lxgw/LxgwWenKai)。
- 字体来源：[v1.520 官方文件](https://github.com/lxgw/LxgwWenKai/releases/download/v1.520/LXGWWenKai-Regular.ttf)。
- 许可来源：[v1.520 OFL.txt](https://raw.githubusercontent.com/lxgw/LxgwWenKai/v1.520/OFL.txt)。
- 字体 SHA-256：`8d6ba638ac9553413354cfaab97637c1cd778444e259441ea1e5f8fb2c697fba`，与 `tools/setup_fonts.py` 锁定值一致。
- 许可证 SHA-256：`5c0f0d98a8b71a401063c9a9e814e608fef6824fefab0c2ab73fe0955a5551b1`。

更新字体时同步替换版本标识、字体、对应版本许可证和来源说明，再核对 ZIP 完整性及解压后的字体摘要。不要用主分支的新许可证替代固定版本的许可证。

## 发布前设计稿

首页按正式发布时的产品介绍组织：离线阅读、本地文件、KOReader 位置接续、界面预览、设备与安装。 KOReader 接续仅限 EPUB；TXT 的编码与本地进度限制保留在页面中。 浏览器安装按钮目前禁用，网站尚未部署。文案与设计完成不代表固件已发布或真机验收完成。

2026-10-10 的六张界面图由当前 `ReaderApp`、文档解析器、字体与画布绘制重新导出，尺寸为 684 × 1216。 示例书《从一页开始》《河岸来信》《日常的片刻》的文字与排版封面为本次原创演示内容； 未使用个人书籍或第三方出版物封面。所有页面均围绕《从一页开始》展示，书库另有两本示例书。 同步回包与电量为合成数据，导出过程不发送网络请求，也不操作设备。 截图没有修改固件绘制的文字或像素；它们不能作为设备实拍或功能验收证据。 原始帧、素材摘要与网页预览保存在本地忽略目录 `artifacts/website-20261010/`，导出程序和隔离演示卡位于 `.local/website-20261010/`。

正式发布前完成以下事项：

1. 按最终发布固件核对功能与限制，必要时重新导出界面图。
2. 接入经过验证的固件安装方式，替换当前指向 README 构建章节的说明链接；浏览器安装实际可用后再启用按钮并移除占位说明。
3. 核对文档、GitHub 与问题反馈链接对目标访问者可用，确定站点地址和部署方式。
4. 在真实手机及目标浏览器检查布局、界面切换和安装流程，再随正式版本部署。
