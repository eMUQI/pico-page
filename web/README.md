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

## 部署

网站地址为 https://picopage.wulu.zone，使用 Cloudflare Pages 的 `picopage` 项目，默认域名为 `picopage-91c.pages.dev`。当前采用手动直接上传，未连接 Git 仓库或设置自动部署；GitHub 仓库保持私有。

更新时执行上述构建命令，在 Pages 项目中创建生产部署，仅上传 `web/dist/` 的内容。不要上传仓库根目录。部署后核对自定义域的 HTTPS 首页、界面图、样式、字体与锁屏下载包与本地构建产物一致。

## 资源下载

首页 `#resources` 集中展示锁屏与字体，手机导航保留「下载」入口；原有 `#fonts` 锚点继续有效。所有文件由静态站直接提供，不依赖私有仓库、第三方图床或运行时 API。

### 锁屏图片

`public/downloads/lockscreens/` 收录用户选定的五张 GPT Image 生成图：读自己的书、接着读下去、把时间留给一页书、暂时离线进入书中、下一页自有天地。来源为 Pico Page 项目制作的图集（https://picopage.wulu.zone/），本次按项目所有者要求提供下载；不标注为 CC0 或公共领域。原图与生成提示词仍保存在本地设计归档中。

- 页面预览、单张下载、ZIP 中的 PNG 字节完全一致，均为 684 × 1216、16 级灰度、无抖动；不另作视觉增强。点击缩略图在浏览器查看原图，返回即可继续选择。
- `PicoPage-LockScreens-01.zip` 包含 `LockScreens/` 下的五张 PNG，以及顶层 `README.txt` 和 `manifest.json`；清单文件名相对于 ZIP 的 `LockScreens/` 目录，记录来源、规格与 SHA-256。
- 使用说明要求支持自定义图片的固件，并说明目录、固定图片或轮播、保存设置及准备未完成时的回退。素材未嵌入固件。
- 更新图集时同步更新 PNG、清单摘要、ZIP 与页面。ZIP 仅打包上述七个文件，禁止带入本机路径、原始会话或其他未选素材；逐张核对尺寸、灰阶、摘要与包内文件。
- 图片来自已选定的设计素材；浏览器预览与下载验证不等于真机显示验收。

### 字体

资源区提供霞鹜文楷 Regular v1.520 的下载，静态文件位于 `public/downloads/`。ZIP 包含未经修改的官方 `LXGWWenKai-Regular.ttf`、该版本的 `OFL.txt` 和注明作者、来源及安装方法的 `README.txt`；网页另提供许可证文本和官方版本链接。字体按 SIL OFL 1.1 单独授权。

- 作者与项目：[LXGW / LxgwWenKai](https://github.com/lxgw/LxgwWenKai)。
- 字体来源：[v1.520 官方文件](https://github.com/lxgw/LxgwWenKai/releases/download/v1.520/LXGWWenKai-Regular.ttf)。
- 许可来源：[v1.520 OFL.txt](https://raw.githubusercontent.com/lxgw/LxgwWenKai/v1.520/OFL.txt)。
- 字体 SHA-256：`8d6ba638ac9553413354cfaab97637c1cd778444e259441ea1e5f8fb2c697fba`，与 `tools/setup_fonts.py` 锁定值一致。
- 许可证 SHA-256：`5c0f0d98a8b71a401063c9a9e814e608fef6824fefab0c2ab73fe0955a5551b1`。

更新字体时同步替换版本标识、字体、对应版本许可证和来源说明，再核对 ZIP 完整性及解压后的字体摘要。不要用主分支的新许可证替代固定版本的许可证。

## 发布前设计稿

首页按正式发布时的产品介绍组织：离线阅读、本地文件、KOReader 位置接续、界面预览、设备与安装。 KOReader 接续仅限 EPUB；TXT 的编码与本地进度限制保留在页面中。 浏览器安装按钮目前禁用，源码与固件暂未公开；页面暂不提供私有仓库的文档、构建和反馈链接。文案与设计完成不代表固件已发布或真机验收完成。

2026-10-10 的六张界面图由当前 `ReaderApp`、文档解析器、字体与画布绘制重新导出，尺寸为 684 × 1216。 示例书《从一页开始》《河岸来信》《日常的片刻》的文字与排版封面为本次原创演示内容； 未使用个人书籍或第三方出版物封面。所有页面均围绕《从一页开始》展示，书库另有两本示例书。 同步回包与电量为合成数据，导出过程不发送网络请求，也不操作设备。 截图没有修改固件绘制的文字或像素；它们不能作为设备实拍或功能验收证据。 原始帧、素材摘要与网页预览保存在本地忽略目录 `artifacts/website-20261010/`，导出程序和隔离演示卡位于 `.local/website-20261010/`。

正式发布前完成以下事项：

1. 按最终发布固件核对功能与限制，必要时重新导出界面图。
2. 接入经过验证的固件安装方式并提供公开可访问的安装说明；浏览器安装实际可用后再启用按钮并移除占位说明。
3. 开源后再恢复文档、GitHub 与问题反馈链接，并核对目标访问者可用性。
4. 在真实手机及目标浏览器检查布局、界面切换和安装流程，再更新网站的安装入口。
