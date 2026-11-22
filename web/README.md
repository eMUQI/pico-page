# Pico Page 网站

Astro 静态站，部署在 https://picopage.wulu.zone（Cloudflare Pages 项目 `picopage`），介绍 Pico Page 的功能和限制，并提供网页安装、使用文档、更新日志和资源下载。网站文案面向读者；验证状态、验收标准和开发注意事项写在根目录 README 和 `docs/` 中。

```sh
npm ci --prefix web --ignore-scripts
npm test --prefix web
npm run build --prefix web
npm run preview --prefix web
```

构建产物在 `web/dist/`。

| 路径 | 内容 |
|---|---|
| `src/layouts/Site.astro`、`src/styles/site.css` | 共用页头、导航、页脚、色板和字体栈；页面样式写在各页面文件里 |
| `src/components/HomePage.astro` | 首页；锚点 `#start`、`#fonts`、`#features`、`#screens`、`#lockscreens`，旧锚点 `#install`、`#resources`、`#devices`、`#historical-seals` 保留以兼容外部链接 |
| `src/components/InstallPage.astro` | 安装页和安装脚本 |
| `src/components/DocsPage.astro` | 使用文档 |
| `src/components/ChangelogPage.astro` | 更新日志 |
| `src/firmware.js` | 构建时查找可安装的全量镜像，首页和安装页共用 |
| `src/pages/` | 静态路由；`404.astro` 让未知地址返回 404，设备据此区分「尚未发布固件」与清单读取失败 |
| `public/assets/screens/` | 界面图，由桌面程序用生产绘制路径和原创合成数据导出，无损 WebP；不是设备实拍或验收证据 |
| `public/downloads/` | 字体与锁屏下载，权利说明见下文 |

## 语言

简体中文使用现有地址，繁体中文和英文使用 `/zh-hant/`、`/en/` 前缀，涵盖首页、安装页、使用文档、更新日志和 404 页；`/downloads/`、`/assets/`、`/firmware/` 共用。访问根首页时先用保存的语言选择，首次访问按浏览器首选语言跳转（繁体及台港澳中文进繁体，其他中文进简体，其余进英文），保留查询参数和锚点；浏览器无法保存偏好时，手动切回简体会带上 `?lang=zh-Hans`。更新文案时同步核对三种语言的内容、链接和可访问名称。

官网使用文档（`DocsPage.astro`）、仓库指南（[docs/user/runtime.md](../docs/user/runtime.md)）和设备帮助（`firmware/components/reader_app/help_content.h`）分别维护，没有自动同步；只在用户操作或限制改变时核对。没有全量镜像时，首页、安装页和文档显示「网页安装尚未开放」，不写「固件尚未发布」：联网更新的固件可以先于全量镜像发布。

更新日志直接读取 `docs/releases/<版本>.md` 及 `zh-Hant/`、`en/` 下的译文，缺少译文时标注并显示简体原文，按设备支持的 Markdown 子集渲染。有 `web/public/firmware/releases.json` 时只显示已发布的版本和发布日期；本地构建没有这份列表，显示全部说明文件。

## 安装页

`/install/` 在电脑版 Chrome 或 Edge 中通过 Web Serial 连接设备，擦除整片闪存后从 `0x0` 写入全量镜像，比对 MD5 并重启。`esptool-js` 0.7.0 与 `spark-md5` 3.0.2 在点击「连接设备」后才加载，许可证全文在 `public/assets/third-party-licenses.txt`，清单见[第三方依赖](../docs/development/dependency-review.md#网站)；升级这些包时同步更新两处。

构建优先选择 `public/firmware/ota-v2/` 的新布局附件，用 `tools/ota_manifest.py` 核验清单、应用和全量镜像，任一缺失或无效时构建失败；新布局未发布时使用旧渠道的全量镜像。页面内嵌所选镜像的 SHA-256。

安装流程中的检查：

- 芯片须为 ESP32-S3、闪存 16 MiB、烧录 stub 已加载；不符时重启设备并断开，不写入。芯片和容量相同的开发板无法区分，访问者须勾选「我确认连接的是 Read Pico」，每次重新连接都要重新勾选。
- 启用闪存加密或安全启动时拒绝安装，因为本页写入的是未加密、未签名的镜像。
- 下载内容与内嵌 SHA-256 一致才擦写；整片擦除用 `eraseFlash()` 单独等待，写入时 `eraseAll: false`。
- 已连接和写入期间离开页面会触发确认。
- 结束时让设备重启并释放串口。Read Pico 在 `esptool-js` 的 RTS 复位后会停在下载模式，因此 ESP32-S3 用与 esptool `watchdog_reset` 相同的 RTC 看门狗寄存器写入重启（`esptool-js` 0.7.0 没有这一功能）；重启后串口消失，之后的关闭失败不算错误。
- 进度只在写入期间按实际传输回调显示百分比，其他阶段使用循环动画；`writeFlash()` 成功返回后才进入重启阶段。成功面板只确认「固件已写入并通过校验」，网页无法确认设备已正常启动。

失败时显示独立错误面板：失败原因、是否已进入擦写、可截图的错误码，主操作「重新连接并重试」（仍须重新勾选确认），以及「复制错误信息」和折叠的详情。复制内容包含错误码、阶段、UTC 时间、固件版本、异常类型、浏览器信息，以及可取得的 HTTP 状态、请求 ID（`x-cos-request-id` 等，需服务端暴露响应头）、耗时和接收字节数；不包含下载地址和原始异常消息，不自动上传。`npm test` 用模拟的下载响应和擦写接口覆盖这些流程，不连接真实设备。

| 错误码 | 排查入口 |
|---|---|
| `CONNECT_FAILED` | 依赖加载、串口连接或设备检查 |
| `DOWNLOAD_HTTP_<状态码>` | 按实际 HTTP 状态检查下载服务；401／403 结合请求 ID 检查权限规则，429 检查限流，404 检查文件是否存在 |
| `DOWNLOAD_UNREACHABLE` | 未取得可读响应；检查网络、TLS 和 CORS |
| `DOWNLOAD_INTERRUPTED` | 已取得响应，但未读完整个文件 |
| `VERIFY_FAILED` / `VERIFY_MISMATCH` | 摘要计算失败，或下载内容与 SHA-256 不符 |
| `FLASH_FAILED` | 已进入擦除、写入或校验，须按恢复说明重新连接；不能宣称设备尚未擦除 |

## 网站统计

网站使用 Google Analytics 4（衡量 ID `G-S9XSC6V1KV`，公开标识，不是密钥），由工作流通过 `PUBLIC_GA_MEASUREMENT_ID` 传入，本地构建默认不启用。统计只在正式主机名下加载，使用 Cookie，不显示同意提示；不启用广告存储、广告个性化或 Google signals。安装页另发送 `device_connect`、`install_start`、`install_success`、`install_failure` 四个不带参数的事件，不上传设备标识、书籍或串口日志；统计失败不影响安装。

## 部署与固件文件

[网站构建工作流](../.github/workflows/web.yml)在 PR 上只构建和测试；main 上的网站输入变化、发布 Release 或在 main 手动运行时部署。部署流程：

1. 把非草稿发布列表写到 `web/public/firmware/releases.json`，把固定的 0.6.2 附件放入 `web/public/firmware/`，把高于 0.6.2 的最高版本放入 `ota-v2/`，用 `tools/ota_manifest.py --check` 核验，并用 `--publish-base-url` 把部署副本的 `url` 指向腾讯云 COS。`web/public/firmware/` 被 Git 忽略。
2. 通过 GitHub OIDC 取得临时身份，把版本 BIN 上传到 COS（禁止覆盖，逐字节读回）；权限边界见[固件托管](../docs/development/firmware-hosting.md)。
3. 构建前若最新 main 的网站输入已变化，让位给替代运行。
4. 用 wrangler 以 `--branch=main` 上传到 Pages（Pages 项目未连接 Git，生产分支必须是 `main`），使用仓库密钥 `CLOUDFLARE_API_TOKEN`、`CLOUDFLARE_ACCOUNT_ID`。
5. 读回正式域名的 `deployment.txt`、两个渠道的清单，以及三种语言安装页的镜像地址、版本和 SHA-256；新渠道未发布时必须返回 404。

GitHub 对 Release 事件使用标签所在提交的工作流文件；标签早于当前流程时，需要在 main 上手动运行一次。部署运行会保存 `pico-page-web` artifact 3 天，可用于核对或手动部署；它不是设备的下载地址。

Actions 不可用时可在本地复现：按[版本管理](../docs/development/versioning.md#构建与打包)生成附件并发布到 Release；把两个渠道的附件放入 `web/public/firmware/`，用工作流中的同一组 `ota_manifest.py` 命令校验并改写下载地址；由管理员把 COS 上缺少的版本 BIN 上传到桶中（不覆盖同名对象，保持桶 ACL 继承），并逐字节核对已有对象与 Release 附件一致，见[固件托管](../docs/development/firmware-hosting.md#首次上传链路过慢时的恢复)——改写后的清单和安装页都指向 COS，Pages 部署不会上传这些 BIN；以 `PUBLIC_GA_MEASUREMENT_ID=G-S9XSC6V1KV npm run build --prefix web` 构建，在 `web/dist/deployment.txt` 写入 commit、`manual` 和 UTC 时间；用 `npx --yes wrangler@4.144.0 pages deploy web/dist --project-name=picopage --branch=main` 上传，凭据只来自本机登录；最后按第 5 步读回核对。

设备不验证发布者签名，能发布 Release、部署网站或控制域名的人可以向设备提供任意固件，见[设备更新安全说明](../docs/development/platform-runtime.md#安全说明)。

## 资源下载

首页的「字体」和「锁屏」两节提供 `/downloads/...` 下的静态文件，不依赖第三方图床或运行时 API。浏览器预览与下载验证不等于真机显示验收。项目代码的 GPL 不扩展到这些独立下载素材。

### 锁屏图片

`public/downloads/lockscreens/` 收录项目制作的五张 GPT Image 生成图，684 × 1216、16 级灰度，页面预览、单张下载和 ZIP 中的 PNG 字节完全一致。`PicoPage-LockScreens-01.zip` 包含 `LockScreens/` 下五张 PNG 以及 `README.txt`、`manifest.json`（来源、规格与 SHA-256）。由项目所有者提供下载，不标注为 CC0 或公共领域。更新图集时同步更新 PNG、清单摘要、ZIP 和页面，ZIP 不得带入其他文件。

### 透明锁屏

`#transparent-lockscreens` 提供八张透明 PNG：三方古印（「常欠讀書債」徐三庚 1867、「願讀人間未見書」陳鍊 1760、「桃華書屋」徐三庚 1870）、两方现代印和三种植物。文件在 `public/downloads/transparent-lockscreens/`，页面从同目录 `manifest.json` 读取署名、来源、规格和摘要；下载 PNG 与真机确认的版本逐字节一致，页面预览只裁切主体并加白底。需要 0.4.0 及以上固件的「叠加当前画面」。

`PicoPage-Transparent-LockScreens.zip` 包含八张 PNG 以及清单、使用说明、作者与来源、权利说明和现代印字体的 OFL 文本。古印的财产权期限与平面忠实复制的判断，和现代印、生成植物的权利状态分开说明；古印图源不等于开放许可，字体 OFL 也不作为整套图集的许可。现代印、植物和生成锁屏的第三方复用许可由维护者决定，未决定前保留原权利说明。历史古印目录和旧 ZIP 保留以兼容已有下载地址。

### 字体

资源区提供霞鹜文楷 Regular v1.520：ZIP 包含未修改的官方 `LXGWWenKai-Regular.ttf`、该版本的 `OFL.txt` 和 `README.txt`，按 SIL OFL 1.1 单独授权。

- 来源：[LXGW / LxgwWenKai](https://github.com/lxgw/LxgwWenKai) 的 [v1.520 字体](https://github.com/lxgw/LxgwWenKai/releases/download/v1.520/LXGWWenKai-Regular.ttf)与 [OFL.txt](https://raw.githubusercontent.com/lxgw/LxgwWenKai/v1.520/OFL.txt)。
- 字体 SHA-256 `8d6ba638ac9553413354cfaab97637c1cd778444e259441ea1e5f8fb2c697fba`，与 `tools/setup_fonts.py` 锁定值一致；许可证 SHA-256 `5c0f0d98a8b71a401063c9a9e814e608fef6824fefab0c2ab73fe0955a5551b1`。更新时使用对应版本的许可证，不用主分支的新许可证替代。

首页三处示例文字使用自托管子集 `public/assets/fonts/PicoPageWenKaiSample.woff`，由 `.local/font-tools/bin/python tools/setup_web_font_sample.py` 生成（只保留脚本 `TEXT` 中的字符；环境见[图形运行时](../docs/development/graphics-runtime.md)）。修改这三处文字时同步修改 `TEXT` 并重新生成。子集按 OFL 改名为 `Pico Page WenKai Sample`，保留版权和许可说明，许可证全文在 `public/downloads/LXGWWenKai-v1.520-OFL.txt`。`tools/check_docs.py` 只允许仓库中出现固件内置子集和这个文件两个字体文件。

`public/favicon.ico` 是项目所有者的 GitHub 头像。
