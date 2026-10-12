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

## 持续集成

[网站构建工作流](../.github/workflows/web.yml) 在推送、拉取请求和手动运行时独立执行，与 host 检查并行。使用 Node.js 24，按 `package-lock.json` 缓存 npm 下载，执行 `npm ci --ignore-scripts` 和 `npm run build`；构建失败时不上传产物。

构建前，工作流从 GitHub Release 取回已发布的固件：在非草稿的发布中选出版本最高的 `v<版本>` 标签，把它附带的 `manifest.json` 和固件镜像下载到 `web/public/firmware/`，再用 `python3 tools/ota_manifest.py --check` 核对清单与镜像逐项一致、版本与标签相同，不一致时工作流失败。没有这样的发布时跳过下载。发布一个 Release 也会触发本工作流。

构建成功后保存 `web/dist/` 为 `pico-page-web` artifact，缺少产物时报错。下载并解压后得到静态站内容，可用于核对或手动部署；artifact 遵循仓库的保留期限和访问权限，不作为设备 OTA 下载地址。

## 部署

网站地址为 https://picopage.wulu.zone，使用 Cloudflare Pages 的 `picopage` 项目，默认域名为 `picopage-91c.pages.dev`。Pages 项目没有连接 Git 仓库，由网站构建工作流用 wrangler 直接上传；GitHub 仓库保持私有。

工作流在以下三种情况把 `web/dist/` 部署为生产版本：`main` 有新提交、发布一个 Release、在 `main` 上手动运行。拉取请求和其他分支只构建，不部署。发布 Release 触发的运行同样从 `main` 构建网站。每次运行取到的固件都是当时版本最高的发布，不一定是刚发布的那一个：补发一个较低的版本时，线上仍是最高的版本。部署依次进行，不并行；GitHub 不保证按提交顺序启动它们，因此运行在部署前会确认自己构建的仍是 `main` 的最新提交，不是时跳过部署，交给最新提交的那次运行。同时排队的部署只保留最新的一个，较早排队的会被取消。每次部署的内容都是当时的 `main` 加版本最高的固件，所以被取消的那次不会丢内容，但保留下来的那次失败时需要重新运行。

GitHub 对 Release 事件使用标签所在提交中的工作流文件。标签所在的提交若早于本部署流程，发布后不会自动部署，需要在 `main` 上手动运行一次工作流。

部署使用仓库密钥 `CLOUDFLARE_API_TOKEN`（仅有 Cloudflare Pages 编辑权限）和 `CLOUDFLARE_ACCOUNT_ID`。wrangler 固定为 4.144.0，以 `--branch=main` 上传；Pages 项目的生产分支必须是 `main`，否则只会得到预览部署。

部署前工作流在产物中写入 `deployment.txt`（实际构建的网站提交、运行编号和重试次数）。部署后核对线上内容：`/deployment.txt` 须与本次写入的一致，证明正式域名提供的就是这次部署而不是预览或旧版本；更新清单地址在已发布固件时须与本次产物中的清单一致，未发布时须返回 404。每次请求最长 8 秒，约两分钟内不符则运行失败。核对失败不会撤回已经完成的部署：此时线上可能已经是新内容，需要到 Pages 项目中查看当前生产部署，必要时在那里回滚。首页、界面图、样式、字体与锁屏下载包仍需人工核对。

需要手动部署时，下载 `pico-page-web` artifact，在 Pages 项目中创建生产部署，仅上传其中的内容，不要上传仓库根目录。

## 固件更新文件

设备从 `https://picopage.wulu.zone/firmware/manifest.json` 读取更新清单，并下载清单指向的固件镜像。这两个文件不放在仓库里：它们是 GitHub Release 的附件，由网站构建工作流取回，随 `pico-page-web` artifact 一起部署。`web/public/firmware/` 已被 Git 忽略。目前尚未发布任何版本。

发布一个版本的步骤（创建 Release 需要项目所有者授权；Release 一经发布，工作流就会把它部署到线上）：

1. 在带有 `v<版本>` 标签的干净检出上构建 release 配置，并完成该版本要求的设备验证。
2. 在仓库根目录生成清单和按版本命名的镜像：

   ```sh
   python3 tools/ota_manifest.py --image build/firmware-release/pico_page.bin \
     --notes <发布说明.md> --out build/release
   ```

3. 用同一个标签创建 Release，附上 `build/release/` 中的两个文件；测试版和候选版标为预发布。先存为草稿核对附件，草稿不会被取用。发布只是触发部署，设备要到下一步的部署成功之后才能检查到这个版本。
4. 等待 Release 触发的网站构建工作流完成。它会取回这两个文件、核对、部署，并核对线上清单与附件一致。取回、核对或构建失败时不会部署，线上保持上一次的内容；部署之后的线上核对失败时，新内容可能已经生效，按「部署」一节处理。

说明：

- 清单在第 2 步由带标签的源码生成，其中的数据格式版本取自该版本的源码；工作流只取回和核对，不重新生成。
- 设备只有一个更新通道，取版本最高的发布，预发布也算在内。撤回一个版本时删除该 Release 或改为草稿，然后在 `main` 上手动运行工作流重新部署。
- 任何分支构建出的 artifact 都带有当前最新发布的固件，部署任一份都不会让设备失去更新文件。
- 发布说明不超过 8 KiB，写法见 [版本管理](../docs/versioning.md)。
- `src/pages/404.astro` 使不存在的地址返回 404。设备据此区分「还没有发布固件」和清单读取失败；删除这个页面后，Cloudflare Pages 会对未知地址返回首页和 200。
- 设备不验证发布者签名，能发布 Release、部署网站或控制该域名的人可以向设备提供任意固件，见 [功能路线图](../docs/roadmap.md) R11 的安全说明。

## 资源下载

首页 `#resources` 集中展示锁屏与字体，手机导航保留「下载」入口；原有 `#fonts` 锚点继续有效。所有文件由静态站直接提供，不依赖私有仓库、第三方图床或运行时 API。

### 锁屏图片

`public/downloads/lockscreens/` 收录用户选定的五张 GPT Image 生成图：读自己的书、接着读下去、把时间留给一页书、暂时离线进入书中、下一页自有天地。来源为 Pico Page 项目制作的图集（https://picopage.wulu.zone/），本次按项目所有者要求提供下载；不标注为 CC0 或公共领域。原图与生成提示词仍保存在本地设计归档中。

- 页面预览、单张下载、ZIP 中的 PNG 字节完全一致，均为 684 × 1216、16 级灰度。第 2、3、4 张采用灰度抖动，减轻渐变断层；第 1、5 张无抖动。页面不另作视觉增强。点击缩略图在浏览器查看原图，返回即可继续选择。
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
