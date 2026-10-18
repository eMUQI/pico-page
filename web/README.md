# Pico Page 网站

Astro 静态站，用于介绍 Pico Page 的功能、限制和开发状态。

```sh
npm ci --prefix web --ignore-scripts
npm run build --prefix web
npm run preview --prefix web
```

构建产物在 `web/dist/`。

- `src/layouts/Site.astro`：所有页面共用的页头、导航和页脚。
- `src/styles/site.css`：共用样式，包括色板变量、字体栈、页头、页脚和按钮。各页面自己的样式写在页面文件里。
- `src/firmware.js`：构建时查找可安装的全量镜像，首页和安装页共用。
- `src/components/HomePage.astro`：首页。
- `src/pages/install.astro`：安装页（`/install/`），含安装脚本。
- `src/pages/docs.astro`：使用文档（`/docs/`），见「使用文档」。
- `src/pages/changelog.astro`：更新日志（`/changelog/`），见「更新日志」。
- `src/pages/`：路由。新增语言或文档页时在这里添加 Astro 或 Markdown 页面。
- `public/assets/screens/`：界面图，来自 host 渲染，使用合成数据；无损 WebP（`cwebp -lossless -z 9`）。锁屏作品保持 PNG 原图。
- `public/assets/fonts/`：首页字体示例用的霞鹜文楷子集，见「字体」。

网站文案面向读者。验证状态、验收标准和开发注意事项写在根目录 README 和 `docs/` 中。

## 持续集成

[网站构建工作流](../.github/workflows/web.yml) 在拉取请求更新时保留检查结果，仅对网站目录、发布说明、OTA 清单脚本及工作流依赖的变更执行构建，与必要的 host 检查并行。分支 push 不重复执行；main 的相关输入变更、发布 Release 或手动运行触发构建，其中正式部署仅限 main 或 Release。使用 Node.js 24，按 `package-lock.json` 缓存 npm 下载，执行 `npm ci --ignore-scripts` 和 `npm run build`；构建失败时不上传产物。

构建前，工作流把非草稿的发布列表写到 `web/public/firmware/releases.json`（见「更新日志」），并从 GitHub Release 取回已发布的固件：在非草稿的发布中选出版本最高的 `v<版本>` 标签，把它附带的 `manifest.json`、应用镜像和全量镜像（如果有）下载到 `web/public/firmware/`，再用 `python3 tools/ota_manifest.py --check` 核对清单与应用镜像逐项一致、版本与标签相同，全量镜像中的应用与应用镜像相同，不一致时工作流失败。校验通过后，`--publish-base-url https://oc.wulu.zone/firmware/` 只将部署副本的下载地址切到腾讯云，保留原发布附件；正式部署先通过受限 SSH 账号自动上传腾讯云，再完整读回清单、应用和全量镜像，与部署副本逐字节比较；上传或校验失败时停止，不发布官网。账号与密钥边界见[腾讯云固件下载](../docs/firmware-hosting.md#自动部署)。没有这样的发布时跳过下载。发布一个 Release 也会触发本工作流。

部署前直接比较构建提交与最新 main 的网站输入：网站输入变化时让位给替代运行，仅文档或固件代码变化时继续当前部署；无法读取比较结果时停止。只有构建成功且通过该判断的正式部署运行，才保存 `web/dist/` 为 `pico-page-web` artifact，缺少产物时报错，保留 3 天。PR 检查和非 main 的手动构建不上传 artifact。下载并解压后得到静态站内容，可用于核对或手动部署；artifact 遵循仓库访问权限，不作为设备 OTA 下载地址。

## 网站统计

网站使用 Google Analytics 4，衡量 ID 为 `G-S9XSC6V1KV`。网站构建工作流通过 `PUBLIC_GA_MEASUREMENT_ID` 传入 ID；GitHub 仓库同名 Variable 可覆盖此值。该 ID 是公开标识，不是密钥。本地构建默认不启用统计；需要检查统计产物时执行 `PUBLIC_GA_MEASUREMENT_ID=G-S9XSC6V1KV npm run build --prefix web`。

统计仅在正式主机名 `picopage.wulu.zone` 下启用，打开页面后直接加载 Google tag，使用 Cookie 统计访问；不显示访客同意提示或统计设置入口，不读写同意选择。localhost、Cloudflare 默认域名和预览域名不加载 Google tag。网站不启用广告存储、广告个性化或 Google signals。

GA 数据流需开启增强型衡量，统计页面访问、滚动、外链和字体及锁屏 ZIP 下载点击。下载点击不代表下载完成。安装页另外发送四种不带自定义参数的事件：`device_connect`（点击连接设备，包括随后取消选择）、`install_start`（开始下载与安装）、`install_success`（网页写入和校验完成，仍可能需要手动重启）、`install_failure`（下载或安装失败）。这些事件不上传设备标识、书籍或串口日志，也不证明固件已经正常启动。统计失败不影响安装。

部署后需使用允许 GA 的浏览器核对正式域名自动加载 Google tag、实时报告有页面访问、安装事件正常接收，以及预览域名不加载统计。Google tag 被网络或扩展阻止时，访问和事件可能不计入；普通报表也不保证立即出现。Google Analytics 管理后台的增强型衡量及实际数据接收需要另行核对。

## 部署

网站地址为 https://picopage.wulu.zone，使用 Cloudflare Pages 的 `picopage` 项目，默认域名为 `picopage-91c.pages.dev`。Pages 项目没有连接 Git 仓库，由网站构建工作流用 wrangler 直接上传；GitHub 仓库保持私有。

工作流在以下三种情况把 `web/dist/` 部署为生产版本：`main` 的网站相关输入有变更、发布一个 Release、在 `main` 上手动运行。PR 仅在相关输入变化时构建，其他分支可手动构建，均不部署；分支 push 不触发。发布 Release 触发的运行从 `main` 构建网站。每次构建取到的固件都是该次运行取回时版本最高的发布，不一定是刚发布的那一个：补发一个较低的版本时，线上仍是最高的版本。

生产部署串行执行，GitHub 不保证按提交顺序启动运行。部署前直接比较所选源码提交与最新 `main` 的网站输入，输入变化时跳过并让位给替代运行；仅文档或固件代码变化时继续当前部署，读取比较结果失败时停止。同时等待的部署只保留后到达队列的一个，先前等待的运行会被替换；替代运行失败或被跳过时，需要重新运行对应的最新版本部署。

GitHub 对 Release 事件使用标签所在提交中的工作流文件。标签所在的提交若早于本部署流程，发布后不会自动部署，需要在 `main` 上手动运行一次工作流。

腾讯云上传使用仓库密钥 `TENCENT_DEPLOY_KEY`、`TENCENT_KNOWN_HOSTS`，仅正式部署步骤读取。Cloudflare 部署使用仓库密钥 `CLOUDFLARE_API_TOKEN`（仅有 Cloudflare Pages 编辑权限）和 `CLOUDFLARE_ACCOUNT_ID`。wrangler 固定为 4.144.0，以 `--branch=main` 上传；Pages 项目的生产分支必须是 `main`，否则只会得到预览部署。

部署前工作流在产物中写入 `deployment.txt`（实际构建的网站提交、运行编号和重试次数）。部署后核对线上内容：`/deployment.txt` 须与本次写入的一致，证明正式域名提供的就是这次部署而不是预览或旧版本；更新清单地址在已发布固件时须与本次产物中的清单一致，未发布时须返回 404。每次请求最长 8 秒，约两分钟内不符则运行失败。核对失败不会撤回已经完成的部署：此时线上可能已经是新内容，需要到 Pages 项目中查看当前生产部署，必要时在那里回滚。首页、界面图、样式、字体与锁屏下载包仍需人工核对。

需要手动部署时，下载可用的 `pico-page-web` 部署候选 artifact，重新核对其中清单、镜像与当前最高版本发布一致，再在 Pages 项目中创建生产部署，仅上传其中的内容，不要上传仓库根目录。

### GitHub Actions 不可用时

没有可用 CI artifact 时，可在本地复现同一套构建与部署。发布授权、本地检查及干净匹配标签的要求不变；创建 GitHub Release 不代表官网已经更新。操作顺序如下：

1. 完成固件发布附件的生成和核验，将应用镜像、全量镜像、清单和来源记录发布到对应的 GitHub Release；未获得发布授权时停留在本地准备阶段。
2. 备份 `web/public/firmware/`，将其替换为目标 Release 的清单和两个 BIN，避免混放不同版本的 BIN。重新从 Release 下载并逐字节比较，不直接使用旧构建目录中的附件。
3. 先按[腾讯云固件下载](../docs/firmware-hosting.md)上传并完整读回目标版本，确认应用和全量镜像与原发布附件一致。在仓库根目录执行 `gh release list --repo eMUQI/pico-page --limit 100 --exclude-drafts --json tagName,publishedAt > web/public/firmware/releases.json`，让更新日志只列出已公开的版本；然后运行 `python3 tools/ota_manifest.py --check web/public/firmware --expect-version <版本> --publish-base-url https://oc.wulu.zone/firmware/`，校验原附件并只修改官网部署副本的下载地址。后续复验该副本时增加 `--base-url https://oc.wulu.zone/firmware/`；不要改写 GitHub 原附件。
4. 执行 `npm ci --prefix web --ignore-scripts` 和 `PUBLIC_GA_MEASUREMENT_ID=G-S9XSC6V1KV npm run build --prefix web`。在 `web/dist/deployment.txt` 写入本次网站源码的完整 commit、`manual` 以及 UTC 时间，保留同内容的本地副本用于读回比较。
5. 在已登录且具有 Pages 编辑权限的本机，用 `npx --yes wrangler@4.144.0 pages deploy web/dist --project-name=picopage --branch=main` 部署；只上传 `web/dist`。凭据通过本机登录或现有环境提供，不写入命令、仓库或日志。也可在现有 Pages 项目中创建生产部署并上传同一目录。参考 [Cloudflare Direct Upload](https://developers.cloudflare.com/pages/get-started/direct-upload/)。
6. 独立读回正式域名的 `deployment.txt` 和清单，与本次本地文件逐字节比较；确认无查询参数的固定清单地址返回的 `url` 以 `https://oc.wulu.zone/firmware/` 开头。沿清单地址下载应用镜像，完整核对大小和 SHA-256；官网全量镜像仍与附件逐字节比较。用 `--base-url https://oc.wulu.zone/firmware/` 复验部署目录，核对安装页版本、更新日志、GA ID 和第三方模块能正常加载。失败时按上一节处理，不把「上传成功」记录为验收通过。

腾讯云镜像按[固件下载镜像](../docs/firmware-hosting.md)单独上传和校验。仅更新 `oc.wulu.zone` 不会改变设备固定的 `picopage.wulu.zone/firmware/manifest.json` 入口；GitHub Actions 不可用期间，不依赖 Release 事件自动完成上述步骤。

## 固件更新文件

设备从 `https://picopage.wulu.zone/firmware/manifest.json` 读取更新清单，再从 `https://oc.wulu.zone/firmware/` 下载清单指向的应用镜像；安装页另外使用官网提供的同一版本全量镜像（见「安装页」）。这些文件不放在仓库里：原文件来自 GitHub Release，部署副本只修改清单的 `url`，随 `pico-page-web` artifact 一起部署。`web/public/firmware/` 已被 Git 忽略。当前发布版本以 GitHub Release 和线上清单为准。腾讯云与官网由工作流按顺序自动部署，实际发布与核验结果见开发状态。

发布一个版本的步骤（创建 Release 需要项目所有者授权；Actions 可用时由发布事件触发部署，不可用时按上面的手动流程执行）：

1. 在带有 `v<版本>` 标签的干净检出上构建 release 配置，并完成该版本要求的设备验证。
2. 在仓库根目录生成清单和按版本命名的镜像：

   ```sh
   python3 tools/ota_manifest.py --image build/firmware-release/pico_page.bin \
     --notes <发布说明.md> --out build/release --full
   ```

   `--full` 另外生成全量镜像 `pico-page-<版本>-read_pico-release-full.bin`，每个版本都带上它（D41）。不加时只有清单和应用镜像：这样的版本可以联网更新，安装页保持「网页安装尚未开放」。

3. 用同一个标签创建 Release，附上 `build/release/` 中的全部文件；测试版和候选版标为预发布。先存为草稿核对附件，草稿不会被取用。发布只是触发部署，设备要到下一步的部署成功之后才能检查到这个版本。
4. 等待 Release 触发的网站构建工作流完成。它会取回这些文件、核对、部署，并核对线上清单与附件一致。取回、核对或构建失败时不会部署，线上保持上一次的内容；部署之后的线上核对失败时，新内容可能已经生效，按「部署」一节处理。

说明：

- 清单在第 2 步由带标签的源码生成，其中的数据格式版本取自该版本的源码；工作流核对原附件后只修改部署副本的 `url`，不重建镜像、不修改版本、摘要、数据格式或更新日志。
- 设备只有一个更新通道，取版本最高的发布，预发布也算在内。撤回一个版本时删除该 Release 或改为草稿，然后在 `main` 上手动运行工作流重新部署。
- 仅部署候选运行生成 artifact，包含该次构建取回的最高版本固件；历史产物可能包含旧版本，手动部署前须重新核对当前发布、清单和镜像。
- 发布说明不超过 8 KiB，写法见 [版本管理](../docs/versioning.md)。
- `src/pages/404.astro` 使不存在的地址返回 404。设备据此区分「还没有发布固件」和清单读取失败；删除这个页面后，Cloudflare Pages 会对未知地址返回首页和 200。
- 设备不验证发布者签名，能发布 Release、部署网站或控制该域名的人可以向设备提供任意固件，见 [功能路线图](../docs/roadmap.md) R11 的安全说明。

## 资源下载

首页的「字体」（`#fonts`）和「锁屏」（`#lockscreens`）两节提供下载，链接使用站内路径 `/downloads/...`。所有文件由静态站直接提供，不依赖私有仓库、第三方图床或运行时 API。

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

首页用霞鹜文楷显示三处示例文字（字体名、「读自己的书，接着读下去。」和缺字对比）。网站不从 jsDelivr 加载完整的 webfont，改用自托管的子集 `public/assets/fonts/PicoPageWenKaiSample.woff`（29 个字符，7,800 字节，SHA-256 `3271897152ad0e74103bf300aa15ee4365f96f23337063fc2c664de19d77eeb3`）。

- 生成：`.local/font-tools/bin/python tools/setup_web_font_sample.py`，使用 fonttools 4.60.1（环境的建立方法见 [图形运行时](../docs/graphics-runtime.md)）。脚本先核对源文件是锁定的 v1.520，再只保留脚本中 `TEXT` 列出的字符。修改首页这三处文字时，同步修改 `TEXT` 并重新生成；不在子集里的字符会落到本机楷体。
- 许可：已阅读 v1.520 的 `OFL.txt` 和字体的版权字段。上游没有声明 Reserved Font Name；子集属于修改版本，仍主动改名为 `Pico Page WenKai Sample`。字体内保留版权（LXGW、The Klee Project Authors）、OFL 1.1 许可说明和许可地址，子集继续按 OFL 1.1 分发；许可证全文在 `public/downloads/LXGWWenKai-v1.520-OFL.txt`，首页同一节有链接。
- `tools/check_handoff.py` 只允许仓库中出现固件内置子集和这个文件两个字体文件。

## 安装页

`/install/` 按设计稿《Pico Page 烧录》实现（Claude Design 项目 `9a2b4c9f-3f3d-4535-8b8e-032611291a02`，2026-10-12 读取）。页面在电脑版 Chrome 或 Edge 中通过 Web Serial 连接设备，擦除整片闪存后从 `0x0` 写入一个全量镜像，写完比对闪存内容的 MD5 并重启设备。串口协议使用 `esptool-js` 0.7.0，MD5 使用 `spark-md5` 3.0.2；两者在访问者点击「连接设备」后才加载。

构建时页面读取 `public/firmware/manifest.json` 的 `version`，并查找同目录的 `pico-page-<版本>-read_pico-release-full.bin`。两者都存在时开放安装，页面内嵌该镜像的 SHA-256；缺少任何一个时显示「网页安装尚未开放」，页面上没有可用的安装按钮，安装脚本不执行任何操作。清单无法解析或全量镜像超过 16 MiB 时构建失败：安装时先擦除后写入，放不下的镜像不能上线。

安装流程中的检查：

- 连接后核对芯片为 ESP32-S3、闪存为 16 MiB，并确认烧录 stub 已加载（整片擦除依赖它）。任一项不符就重启设备、断开，不写入。
- 芯片和容量相同的其他开发板无法与 Read Pico 区分。连接后的面板只显示核对到的芯片和容量，访问者勾选「我确认连接的是 Read Pico」后才能点击安装；每次重新连接都要重新勾选。
- 设备启用了闪存加密或安全启动时拒绝安装：本页写入的是未加密、未签名的镜像，在这样的芯片上无法启动。状态取自 ROM 在识别芯片时返回的安全信息。
- 下载的镜像与页面内嵌的 SHA-256 不一致时不擦除、不写入。
- 已连接和写入期间关闭或刷新页面会触发浏览器的离开确认。已连接的设备运行着烧录 stub，页面关闭后无法再让它复位。
- 断开、失败和完成后都让设备重启并释放串口。ESP32-S3 用 RTC 看门狗重启，寄存器写入与 esptool 的 `watchdog_reset` 相同：Read Pico 在 `esptool-js` 的 RTS 复位后会停在下载模式，屏幕没有变化，只能长按电源键强制断电（2026-10-12 用户在真机上遇到；此前用 esptool 安装时已多次记录同一现象）。`esptool-js` 0.7.0 是当前最新版本，没有看门狗复位，由本页自己写寄存器。其他芯片仍用 RTS 复位。重启后设备的 USB 串口会消失，这之后关闭串口失败不算错误。复位或释放失败时，断开后提示手动重启设备，安装完成后提示设备可能没有自动重启。

与设计稿的差异：

- 没有设计演示中的模拟进度；阶段依次为下载固件、擦除整片闪存、写入、校验，百分比来自实际写入量。擦除期间进度停在 0%，16 MiB 闪存的擦除时间未在真机上测量。
- 新增提示：连接失败、设备不符、设备启用了加密或安全启动、安装未完成（区分设备是否已被改动）、复位或释放串口失败。浏览器不支持 Web Serial 时禁用按钮并提示更换浏览器。
- 已连接面板的「设备 Read Pico」改为「芯片 ESP32-S3 · 16 MiB 闪存」，并增加确认勾选项，原因见上文的检查。
- 页脚增加「第三方许可」链接。
- 字体使用本机字体栈，不从 Google Fonts 加载 Noto Serif SC 和 IBM Plex Mono，网站仍不依赖第三方资源。
- 设计稿中的「恢复说明」链接到 Read Pico 官方文档的 [DIY Your System](https://dot.mindreset.tech/docs/read_0/firmware)，并写明目前需要用 ESP-IDF 从源码构建后烧录；官方没有发布预编译镜像，本站不提供刷回官方系统的镜像。连接后的提示相应改为指向第 1 步，不再要求先备份。页头的「文档」链接到本站的使用文档；「完整安装文档与手动安装方法」暂未放入，手动安装方法尚未确定，源码中留有 TODO；「安装完成，屏幕没变化」改为建议重新安装。
- 「尚未开放」时不显示版本号。页头、导航和页脚与首页共用，见「首页」。

发布前事项：

1. 已完成（2026-10-12）：全量镜像由 `tools/ota_manifest.py --full` 生成并核对，随 Release 发布，由网站构建工作流取回，见「固件更新文件」。`0.2.0-beta.1` 按此流程发布。
2. 已确认（2026-10-12，[决定](../docs/decisions.md) D41）：电脑安装采用整片覆盖，根目录 README、[功能路线图](../docs/roadmap.md) R11 和 [版本管理](../docs/versioning.md) 已同步。
3. 真机验证（未完成，见 [验收](../docs/acceptance.md) INST02、INST03）：在装过其他固件的设备上完成一次浏览器安装，核对启动、面板显示（官方文档写明 VCOM 保存在 PMU 中，不在闪存里）、联网更新的槽切换与回滚，以及擦除 NVS 后的 Wi-Fi 行为。目前没有保留出厂系统的设备可供验证。本页的安装流程只用模拟的串口和烧录库检查过，没有连接过设备。
4. 连接不上时的说明（2026-10-13 已写入页面）：弹窗里没有设备、安装后屏幕没有变化、复位失败时，页面提示按住电源键约 10 秒强制关机，再按住 1 秒以上开机。方法来自用户在真机上的做法；时长取自锁定 BSP `5f3406d` 的 `components/read_pico_pmu/docs/llms-full_zh_cn.md`：电源键由 PMU 处理，与运行的固件无关，开机状态按住满 10 秒强制断电，关机状态按住满 1 秒开机，这两个值是 PMU 配置的默认值。同一文档还写明开机状态下短按两次（间隔不超过 1.2 秒）再长按 3 秒以上由 PMU 让芯片进入下载模式；这个组合没有在真机上试过，没有写进页面。
5. 线上核对（2026-10-12 已做一次）：Cloudflare Rocket Loader 把安装页的模块脚本改写为由它加载。用无头 Chrome 打开线上安装页，以一个无法打开的模拟串口点击「连接设备」，页面加载了 `esptool-js` 和 `spark-md5` 并显示连接失败的提示，说明模块脚本和按需加载在改写后仍然执行。这不是真机连接。

### 浏览器依赖的许可

安装页在浏览器中加载以下 npm 包，版本由 `package-lock.json` 锁定。它们的许可证全文合并在 `public/assets/third-party-licenses.txt`，随网站发布，安装页页脚链接到该文件。

| 包 | 版本 | 许可证 | 来源 |
|---|---|---|---|
| esptool-js | 0.7.0 | Apache-2.0，包内没有 NOTICE 文件 | https://github.com/espressif/esptool-js |
| pako（esptool-js 的依赖） | 2.2.0 | MIT AND Zlib | https://github.com/nodeca/pako |
| atob-lite（esptool-js 的依赖） | 2.0.0 | MIT | https://github.com/hughsk/atob-lite |
| spark-md5 | 3.0.2 | WTFPL OR MIT | https://github.com/satazor/js-spark-md5 |

许可证文本取自 npm 包内的 `LICENSE` 或 `LICENSE.md`，未作修改。升级或增减这些包时，同步更新上表和许可证文件。源码未修改，由 Astro 打包后发布。

## 首页

首页按设计稿《Pico Page 官网 v3》实现（Claude Design 项目 `9a2b4c9f-3f3d-4535-8b8e-032611291a02`，2026-10-12 读取），与安装页、404 页共用 `src/layouts/Site.astro` 和 `src/styles/site.css`。各节的锚点是 `#start`、`#fonts`、`#features`、`#screens`、`#lockscreens`，三个页面的导航都指向这五处。

页头和页脚由 `Site.astro` 按页面决定显示哪些链接，与各自的设计稿一致：页头右侧是「文档」和「安装固件」两个按钮，当前所在的页面显示为当前页标记；页脚在首页是「文档、安装固件、QuotaFrame」，在安装页是「文档、第三方许可、返回首页」，在文档页是「安装固件、返回首页」，在 404 页是「文档、安装固件、返回首页」；除更新日志页自身外，各页页脚都有「更新日志」。第三方许可只涉及安装页加载的浏览器依赖，所以只在安装页链接。

旧首页的锚点继续有效，避免已有的外部链接落空：`#install` 落在「开始使用」，`#resources` 落在「字体」，`#devices` 落在「界面」一节末尾的规格表，`#fonts` 和 `#lockscreens` 不变。前两个是节首的空锚点，`#devices` 是规格表本身的 id；删除它们前先确认没有外部引用。

第 1 步「安装固件」随构建时是否存在全量镜像变化（判断与安装页相同，见 `src/firmware.js`）：没有时显示「网页安装尚未开放」，按钮为「查看安装步骤」；有时显示完成标志，按钮为「打开安装页」。

与设计稿的差异：

- 字体使用本机字体栈，不从 Google Fonts 加载 Noto Serif SC 和 IBM Plex Mono；霞鹜文楷示例使用自托管的子集，不从 jsDelivr 加载，见「字体」。
- 设计稿中「文档」「阅读完整文档」「传书方式详解」指向占位的 `#docs`，现在分别链接到 `/docs/`、`/docs/` 和 `/docs/#transfer`。
- 下载链接使用站内路径，界面图使用本仓库重新导出的图，不使用设计项目里的旧图。锁屏缩略图点击查看原图。
- 缺字示例：设计稿只把「䶮」画成方框。「鄱」同样不在内置的 3,500 字里（按 `firmware/components/reader_graphics/fonts/common_characters.txt` 核对），示例改为两个字都显示方框。
- 传书限制：大小上限和同名不覆盖只适用于 Wi‑Fi 上传（书籍 64 MiB、字体 32 MiB、锁屏图片 2 MiB，见 `portal_server.cpp`），文案写明「Wi‑Fi 上传」。
- 界面说明按当前固件改写：阅读工具为「目录、排版、同步、刷新模式和全刷」，排版说明加入字重。
- 第 1 步的「原厂系统或其他固件都可以直接装」补上例外：启用了闪存加密或安全启动的设备会被安装页拒绝。安装页的确认项和常见问题同步补上。
- 第 3 步写明 TF 卡插回后要重启：固件不支持热插拔。安装页第 4 步同样补上。
- 第 2 步下方的提示写明 Wi‑Fi 传书也能上传字体和锁屏图片（D42，2026-10-13 起；对应的固件发布前，线上设备仍只能传书），字体的许可证要用 USB 文件传输或读卡器。
- 锁屏使用步骤补回单张 PNG 的放法、固定或轮播，以及图片未准备好时显示简洁屏保。字体卡片里的锁屏提示改为事实陈述，不用「精选」。
- `.picopage/` 的说明写为「原书旁同名的 `.picopage/` 目录」，与设备上的使用说明一致。

`public/favicon.ico` 是项目所有者的 GitHub 头像（64×64），浏览器按默认路径 `/favicon.ico` 取用，页面不另写 `<link rel="icon">`。

## 使用文档

`/docs/` 是面向读者的使用文档，单页，各节的锚点为 `#start`、`#controls`、`#typography`、`#transfer`、`#files`、`#sync`、`#power`、`#limits`、`#faq`。没有对应的设计稿，版式沿用共用样式。仓库保持私有，文档页是读者唯一能看到的说明。

内容取自根目录 README 的「功能」「使用」、[Wi-Fi 传书](../docs/wifi-transfer.md)、[USB 文件传输](../docs/usb-storage-runtime.md)、[存储](../docs/storage.md)和设备内置说明 `firmware/components/reader_app/help_content.h`，控件名称按 `app.cpp` 核对。只写用法和限制，不写验收状态和实现细节。这些来源或界面文案变化时同步修改本页。

- 没有全量镜像时，页首显示「网页安装尚未开放」；判断与安装页相同。三个页面都不写「固件尚未发布」：联网更新的固件可以先于全量镜像发布，两者不是一回事。
- 没有「安装」一节，只链接到安装页。手动安装方法尚未确定，见「安装页」的发布前事项。
- 没有写固件更新、Wi‑Fi 连接方式等设置的细节，设备内置说明已覆盖其中一部分；发布前按最终固件补充。

## 更新日志

`/changelog/` 按版本从新到旧列出各版本的发布说明，各版本的锚点为 `#v<版本>`。没有对应的设计稿，版式沿用共用样式。安装页的版本号旁边链接到当前版本的条目。仓库保持私有，GitHub 的 Release 页面读者打不开，这一页是读者唯一能看到更新日志的地方。

- 内容直接读取 `docs/releases/<版本>.md`，与随清单发到设备上的更新日志是同一份文件，不另外维护。页面按设备支持的 Markdown 子集渲染：标题、列表、段落、`**粗体**` 和反引号，链接只保留文字。
- 网站构建工作流把非草稿的发布列表（标签和发布时间）写到 `web/public/firmware/releases.json`，随网站发布。有这份列表时，页面只显示已经发布的版本，并显示发布日期（按北京时间取日期）；说明文件先于发布合并时不会提前出现在页面上。本地构建没有这份列表，显示全部说明文件，不带日期。
- 发布一个版本不需要为这一页做额外的事：Release 触发的构建会取到新的发布列表。
- `0.2.0-beta.1` 和 `0.2.0-beta.2` 的说明在发布后删去了「已知问题」和「兼容性」两节（2026-10-13 用户确认，写法见 [版本管理](../docs/versioning.md)）。这两个版本已发布的清单不改，设备上显示的仍是发布时的原文，与本页不同。

## 发布前设计稿

首页的安装入口链接到安装页；`0.2.0-beta.1` 发布后安装页已开放，见「安装页」。KOReader 接续仅限 EPUB；TXT 的编码与本地进度限制保留在页面中。固件已公开提供下载，源码暂未公开；页面暂不提供私有仓库的文档、构建和反馈链接。固件已发布不代表真机验收完成。

六张界面图于 2026-10-12 按 main `ef3bdb5` 的 `ReaderApp`、文档解析器、字体与画布绘制重新导出，尺寸为 684 × 1216。示例书《从一页开始》《河岸来信》《日常的片刻》的文字与排版封面为原创演示内容，未使用个人书籍或第三方出版物封面。所有页面均围绕《从一页开始》展示，书库另有两本示例书。同步回包与电量为合成数据，导出过程不发送网络请求，也不操作设备。截图没有修改固件绘制的文字或像素，PGM 到 PNG 只做格式转换；它们不能作为设备实拍或功能验收证据。

- 导出程序和隔离演示卡在本地忽略目录 `.local/website-20261012/`：`prepare.py` 生成演示卡，`build.sh` 用 `build/host` 的库编译 `capture.cpp`，`topng.py` 转换格式。卡路径必须是绝对路径。
- 原始帧、摘要（`screens.json`）和网页截图在 `artifacts/website-20261012/`。2026-10-10 的导出保留在两个目录的 `website-20261010/` 下。
- 与 2026-10-10 的图逐像素比较：正文和休眠两张完全相同，文件未改；书库、阅读工具、排版、位置接续四张已替换。同步示例的远端位置由第一章 18% 改为第二章，本机与远端的差别更明显。

正式发布前完成以下事项：

1. 按最终发布固件核对功能与限制，必要时重新导出界面图。
2. 完成「安装页」一节列出的发布前事项，并提供公开可访问的安装说明。
3. 开源后再恢复 GitHub 与问题反馈链接，并核对目标访问者可用性；按最终固件核对使用文档。
4. 在真实手机及目标浏览器检查布局和安装流程。
